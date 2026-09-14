// Include the actual backend, copied beside the lightweight engine header by
// the runner. No queue-control or mixer-clock code is duplicated in the test.
#include "audio_pch.h"
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#define private public
#include "snd_dev_mac_audioqueue.cpp"
#undef private

bool snd_firsttime=false;
int g_soundtime=0, g_paintedtime=0;
double testTime=0;
portable_samplepair_t testPaint[4096];
TestConVar snd_surround={}, snd_legacy_surround={};
paintbuffer_t* MIX_GetCurrentPaintbufferPtr() { static paintbuffer_t p={}; return &p; }
bool MIX_ScaleChannelVolume(paintbuffer_t*, channel_t*, int*, int) { return false; }
void S_SpatializeChannel(int*, int, const Vector*, float, float) {}
void S_TransferStereo16(void* output, const portable_samplepair_t*, int begin, int end) {
    // Tag every ring frame with its mixer position; tests can detect old audio.
    for (int i=begin; i<end; ++i) static_cast<uint32*>(output)[i & 32767]=uint32(i)+1;
}

struct FakeQueue {
    AudioQueueOutputCallback callback;
    void* context;
    bool running;
    std::vector<AudioQueueBufferRef> buffers;
    std::deque<AudioQueueBufferRef> pending;
    // CoreAudio may acquire buffers while their PCM is still waiting to play.
    std::vector<uint32> hardwarePCM;
};
static FakeQueue* current=nullptr;
static int creates=0, stops=0, starts=0, primes=0, emptyPrimes=0, enqueues=0, freesWhileQueued=0;
static bool returnOnPrime=false, failStop=false, failEnqueue=false, failPrime=false,
            failStart=false, failCreate=false;
static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void acquire(FakeQueue* q) {
    while (!q->pending.empty()) {
        AudioQueueBufferRef b=q->pending.front(); q->pending.pop_front();
        const uint32* pcm=static_cast<uint32*>(b->mAudioData);
        q->hardwarePCM.insert(q->hardwarePCM.end(), pcm, pcm+b->mAudioDataByteSize/4);
        q->callback(q->context, q, b);
    }
}
static void flush(FakeQueue* q) {
    while (!q->pending.empty()) {
        AudioQueueBufferRef b=q->pending.front(); q->pending.pop_front();
        q->callback(q->context,q,b);
    }
    q->hardwarePCM.clear(); q->running=false;
}
OSStatus AudioQueueNewOutput(const AudioStreamBasicDescription*, AudioQueueOutputCallback cb,
                            void* ctx, const void*, const void*, UInt32, AudioQueueRef* out) {
    ++creates;
    if (failCreate) { *out=nullptr; return -1; }
    *out=new FakeQueue{cb,ctx,false,{},{},{}}; current=*out; return noErr;
}
OSStatus AudioQueueAllocateBuffer(AudioQueueRef q, UInt32 n, AudioQueueBufferRef* out) {
    *out=new AudioQueueBuffer{0,std::malloc(n)}; q->buffers.push_back(*out); return noErr;
}
OSStatus AudioQueueFreeBuffer(AudioQueueRef q, AudioQueueBufferRef b) {
    if (std::find(q->pending.begin(),q->pending.end(),b)!=q->pending.end()) {
        ++freesWhileQueued; return -1;
    }
    q->buffers.erase(std::remove(q->buffers.begin(),q->buffers.end(),b),q->buffers.end());
    std::free(b->mAudioData); delete b; return noErr;
}
OSStatus AudioQueuePrime(AudioQueueRef q, UInt32, UInt32*) {
    ++primes;
    if (q->pending.empty()) ++emptyPrimes;
    if (failPrime) return -1;
    if (returnOnPrime) acquire(q);
    return noErr;
}
OSStatus AudioQueueSetParameter(AudioQueueRef, AudioQueueParameterID, float) { return noErr; }
OSStatus AudioQueueAddPropertyListener(AudioQueueRef, AudioQueuePropertyID, AudioQueuePropertyListenerProc, void*) { return noErr; }
OSStatus AudioQueueRemovePropertyListener(AudioQueueRef, AudioQueuePropertyID, AudioQueuePropertyListenerProc, void*) { return noErr; }
OSStatus AudioQueueGetProperty(AudioQueueRef q, AudioQueuePropertyID, void* out, UInt32* size) {
    require(*size==sizeof(UInt32), "property size uninitialized");
    *static_cast<UInt32*>(out)=q->running?1:0; return noErr;
}
OSStatus AudioQueueStop(AudioQueueRef q, bool) {
    ++stops; if (failStop) return -1; flush(q); return noErr;
}
OSStatus AudioQueueDispose(AudioQueueRef q, bool) {
    flush(q);
    while (!q->buffers.empty()) AudioQueueFreeBuffer(q,q->buffers.back());
    delete q; if (current==q) current=nullptr; return noErr;
}
OSStatus AudioQueueEnqueueBuffer(AudioQueueRef q, AudioQueueBufferRef b, UInt32, const AudioStreamPacketDescription*) {
    ++enqueues;
    if (failEnqueue) return -1;
    require(std::find(q->pending.begin(),q->pending.end(),b)==q->pending.end(), "re-enqueued an owned buffer");
    q->pending.push_back(b); return noErr;
}
OSStatus AudioQueueStart(AudioQueueRef q, const AudioTimeStamp*) {
    ++starts; if (failStart) return -1; q->running=true; return noErr;
}

struct Device {
    CAudioDeviceAudioQueue device;
    Device() { require(device.Init(), "init failed"); }
    ~Device() { device.Shutdown(); }
    void mix(float ahead=0.1f) {
        const int position=device.GetOutputPosition();
        // GetSoundTime's ring unwrap; no independent backend implementation.
        if (position < (g_soundtime & 32767)) g_soundtime += 32768;
        g_soundtime=(g_soundtime & ~32767)+position;
        int end=device.PaintBegin(ahead,g_soundtime,g_paintedtime);
        if (end > g_paintedtime) { device.TransferSamples(end); g_paintedtime=end; }
        device.PaintEnd();
    }
};
static void run(const std::string& name) {
    Device d;
    if (name=="prime-order") {
        require(emptyPrimes==0, "OpenWaveOut primed before enqueueing any data");
        d.mix(); require(starts==1, "startup failed");
    } else if (name=="early-buffer-return") {
        returnOnPrime=true; d.mix();
        require(!current->hardwarePCM.empty(), "no primed audio");
        const auto original=current->hardwarePCM;
        const int stopped=stops;
        testTime+=0.02; d.mix();
        require(stops==stopped, "reusable buffers mistaken for starvation; stopped pending audio");
        require(current->hardwarePCM.size()>=original.size() &&
                std::equal(original.begin(),original.end(),current->hardwarePCM.begin()), "pending PCM lost");
    } else if (name=="tiny-mixahead") {
        d.mix(0.001f);
        require(starts==1 && enqueues>=1, "sub-buffer mixahead cannot start its own playback clock");
    } else if (name=="long-session") {
        d.device.m_buffersCompleted=2097152u;
        require(d.device.GetOutputPosition()==0, "bad position after signed-byte-count boundary");
    } else if (name=="counter-wrap" || name=="signed-counter-boundary") {
        uint32 initial=name=="counter-wrap" ? 0xfffffff0u : 0x7ffffff0u;
        d.device.m_buffersSent=initial;
        d.device.m_buffersCompleted=initial;
        g_soundtime=32512-15*256; g_paintedtime=g_soundtime;
        d.mix(); acquire(current); d.mix();
        require(enqueues>=32, "refill failed across unsigned counter wrap");
    } else if (name=="stop-failure") {
        d.mix(); failStop=true; d.device.Pause(); failStop=false;
        d.device.UnPause(); testTime+=2; d.mix();
        require(freesWhileQueued==0, "freed buffers still owned by failed-stop queue");
        require(current && current->running, "did not recover failed pause");
    } else if (name=="dispose-after-stop-failure") {
        d.mix(); failStop=true; d.device.RecoverWaveOut("test"); failStop=false;
        require(freesWhileQueued==0, "freed buffers before synchronous disposal after stop failure");
    } else if (name=="pause-stays-paused") {
        d.mix(); d.device.Pause(); int started=starts;
        testTime+=10; d.device.PaintEnd();
        require(starts==started, "PaintEnd restarted a paused device");
    } else if (name=="restart-grace") {
        d.mix(); d.device.Pause(); testTime=100; d.device.UnPause(); d.mix();
        int created=creates; testTime+=0.02; d.mix();
        require(creates==created, "restart inherited stale stall deadline");
    } else if (name=="enqueue-failure") {
        failEnqueue=true; d.mix(); testTime+=0.01; d.mix();
        int attempted=enqueues, created=creates; testTime+=0.01; d.mix();
        require(creates==created, "recovery cooldown bypassed");
        require(enqueues==attempted, "failed queue retried during recovery cooldown");
        failEnqueue=false; testTime+=2; d.mix(); d.mix();
        require(current && current->running, "enqueue recovery never resumed");
    } else if (name=="prime-failure" || name=="start-failure") {
        failPrime=name=="prime-failure"; failStart=!failPrime;
        d.mix(); testTime+=0.01; d.mix();
        int attempted=primes+starts, created=creates; testTime+=0.01; d.mix();
        require(creates==created && primes+starts==attempted, "failed start/prime retried during cooldown");
        failPrime=failStart=false; testTime+=2; d.mix(); d.mix();
        require(current && current->running, "start/prime recovery never resumed");
    } else if (name=="device-change-cooldown") {
        d.mix();
        require(d.device.RecoverWaveOut("test"), "initial recovery failed");
        int created=creates;
        d.device.MarkQueueDeviceChanged(); testTime+=0.1; d.mix();
        require(creates==created, "device change bypassed cooldown");
        testTime+=1; d.mix();
        require(creates==created+1, "pending device change was lost");
    } else if (name=="create-failure") {
        d.mix(); failCreate=true; d.device.MarkQueueDeviceChanged(); d.mix();
        require(!current, "failed queue creation left an output queue");
        int created=creates; testTime+=0.01; d.mix();
        require(creates==created, "failed creation retried during cooldown");
        failCreate=false; testTime+=2; d.mix(); d.mix();
        require(current && current->running, "device did not recover after route returned");
    } else if (name=="stall-backoff") {
        d.mix(); int created=creates;
        testTime=2.4; d.mix(); require(creates==created, "initial stall grace too short");
        testTime=2.6; d.mix(); require(creates==++created, "initial stall not recovered");
        testTime=7.5; d.mix(); require(creates==created, "stall tolerance did not back off");
        testTime=7.7; d.mix(); require(creates==++created, "second stall not recovered");
        acquire(current); testTime=7.8; d.mix();
        testTime=10.4; d.mix(); require(creates==++created, "real acquisition did not reset stall tolerance");
    } else if (name=="nested-pause") {
        d.mix(); d.device.Pause(); d.device.Pause(); int started=starts;
        d.device.UnPause(); d.mix(); require(starts==started, "nested pause resumed early");
        d.device.UnPause(); d.mix(); require(starts==started+1, "nested pause did not resume");
    } else if (name=="paint-frontier") {
        d.mix(0.01f);
        for (auto b:current->pending) {
            const uint32* frames=static_cast<uint32*>(b->mAudioData);
            for (unsigned i=0;i<b->mAudioDataByteSize/4;++i)
                require(frames[i]>0 && frames[i]<=uint32(g_paintedtime), "submitted stale ring audio");
        }
    } else throw std::runtime_error("unknown test");
}
int main(int argc, char** argv) {
    try { require(argc==2,"test name required"); run(argv[1]); std::cout<<"PASS "<<argv[1]<<'\n'; }
    catch (const std::exception& e) { std::cerr<<"FAIL "<<argv[1]<<": "<<e.what()<<'\n'; return 1; }
}
