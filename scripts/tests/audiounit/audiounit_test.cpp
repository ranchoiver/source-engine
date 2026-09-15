#include "audio_pch.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static thread_local bool inCallback=false;
static thread_local unsigned callbackCopies=0;
static void require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
static void nonRealtime() { require(!inCallback,"operation on real-time thread"); }
static int allocationCount=0,failAllocation=0;
static void* checkedMalloc(size_t n) { nonRealtime(); if (++allocationCount==failAllocation) return nullptr; return std::malloc(n); }
static void checkedFree(void* p) { nonRealtime(); std::free(p); }
static void* checkedCopy(void* dst,const void* src,size_t n) {
    if (inCallback) ++callbackCopies;
    return std::memcpy(dst,src,n);
}
static void checkedLog(const char*,...) { nonRealtime(); }
#undef Q_memcpy
#define Q_memcpy checkedCopy
#define malloc checkedMalloc
#define free checkedFree
#define DevMsg checkedLog
#define private public
#include "snd_dev_mac_audiounit.cpp"
#undef private
#undef malloc
#undef free
#undef DevMsg

bool snd_firsttime=false;
int g_paintedtime=0;
double testTime=0;
portable_samplepair_t testPaint[4096];
TestConVar snd_surround={},snd_legacy_surround={};
ConVar snd_mixahead={0.1f};
bool recording=false;
TestMovieInfo cl_movieinfo;
paintbuffer_t* MIX_GetCurrentPaintbufferPtr() { static paintbuffer_t p={}; return &p; }
bool MIX_ScaleChannelVolume(paintbuffer_t*,channel_t*,int*,int) { return false; }
void S_SpatializeChannel(int*,int,const Vector*,float,float) {}
void OnSndSurroundCvarChanged2(IConVar*,const char*,float) {}
void OnSndSurroundLegacyChanged2(IConVar*,const char*,float) {}
void S_TransferStereo16(void* out,const portable_samplepair_t* input,int begin,int end) {
    nonRealtime();
    if (recording) return; // Real transfer records but leaves device PCM untouched.
    // Samples come from the paintbuffer, independently of their destination
    // ring index. This models the real transfer's input/output contract.
    for (int n=0;n<end-begin;++n) {
        uint32 pcm=uint16_t(input[n].left) | (uint32(uint16_t(input[n].right))<<16);
        static_cast<uint32*>(out)[(begin+n) & 32767]=pcm;
    }
}

struct FakeUnit { bool running; AURenderCallbackStruct callback; UInt32 maxFrames; };
struct Listener { AudioObjectID object; AudioObjectPropertyAddress address; AudioObjectPropertyListenerProc proc; void* context; };
static std::vector<Listener> listeners;
static std::vector<Listener> pendingListeners;
static FakeUnit* current=nullptr;
static int creates=0,starts=0,stops=0,disposes=0;
static bool failStart=false,failStop=false,failCreate=false,failMaxSlice=false,changeDuringInit=false,scaleSlice=false;
static UInt32 hardwareFrames=512;
static void notifyDevice() {
    auto snapshot=listeners;
    for (const auto& l:snapshot) l.proc(l.object,1,&l.address,l.context);
}
AudioComponent AudioComponentFindNext(AudioComponent,const AudioComponentDescription*) { nonRealtime(); return (void*)1; }
OSStatus AudioComponentInstanceNew(AudioComponent,AudioUnit* out) {
    nonRealtime(); ++creates;
    if (failCreate) { *out=nullptr; return -1; }
    *out=current=new FakeUnit{false,{},4096}; return noErr;
}
OSStatus AudioComponentInstanceDispose(AudioUnit unit) {
    nonRealtime(); ++disposes; require(unit==current,"wrong unit disposed");
    delete unit; current=nullptr; return noErr;
}
OSStatus AudioUnitSetProperty(AudioUnit unit,UInt32 property,UInt32,UInt32,const void* data,UInt32) {
    nonRealtime();
    if (property==kAudioUnitProperty_MaximumFramesPerSlice) {
        if (failMaxSlice) return -1;
        unit->maxFrames=*static_cast<const UInt32*>(data);
    }
    if (property==kAudioUnitProperty_SetRenderCallback) unit->callback=*static_cast<const AURenderCallbackStruct*>(data);
    return noErr;
}
OSStatus AudioUnitGetProperty(AudioUnit unit,UInt32 property,UInt32,UInt32,void* out,UInt32*) {
    nonRealtime(); require(property==kAudioUnitProperty_MaximumFramesPerSlice,"unexpected unit query");
    *static_cast<UInt32*>(out)=unit->maxFrames; return noErr;
}
OSStatus AudioUnitInitialize(AudioUnit unit) {
    if (scaleSlice) unit->maxFrames*=2;
    nonRealtime(); if (changeDuringInit) { changeDuringInit=false; for (const auto& l:pendingListeners) l.proc(l.object,1,&l.address,l.context); } return noErr;
}
OSStatus AudioUnitUninitialize(AudioUnit) { nonRealtime(); return noErr; }
OSStatus AudioOutputUnitStart(AudioUnit unit) {
    nonRealtime(); ++starts;
    if (failStart) return -1;
    unit->running=true; return noErr;
}
OSStatus AudioOutputUnitStop(AudioUnit unit) {
    nonRealtime(); ++stops;
    if (failStop) return -1;
    unit->running=false; return noErr;
}
OSStatus AudioObjectGetPropertyData(AudioObjectID,const AudioObjectPropertyAddress* addr,UInt32,const void*,UInt32*,void* out) {
    nonRealtime();
    if (addr->mSelector==kAudioDevicePropertyNominalSampleRate) *static_cast<Float64*>(out)=48000;
    else *static_cast<UInt32*>(out)=addr->mSelector==kAudioDevicePropertyBufferFrameSize?hardwareFrames:42;
    return noErr;
}
OSStatus AudioObjectSetPropertyData(AudioObjectID,const AudioObjectPropertyAddress*,UInt32,const void*,UInt32,const void*) { nonRealtime(); return noErr; }
OSStatus AudioObjectAddPropertyListener(AudioObjectID obj,const AudioObjectPropertyAddress* addr,AudioObjectPropertyListenerProc proc,void* ctx) {
    nonRealtime(); listeners.push_back({obj,*addr,proc,ctx}); return noErr;
}
OSStatus AudioObjectRemovePropertyListener(AudioObjectID obj,const AudioObjectPropertyAddress* addr,AudioObjectPropertyListenerProc proc,void* ctx) {
    nonRealtime();
    for (auto i=listeners.begin();i!=listeners.end();++i) {
        if (i->object==obj && i->address.mSelector==addr->mSelector && i->proc==proc && i->context==ctx) {
            listeners.erase(i); return noErr;
        }
    }
    return -1;
}

struct Device : CAudioDeviceMacAudioUnit {
    Device() { require(Init(),"Init failed"); }
    ~Device() { Shutdown(); }
};
static void paint(Device& d,int count) {
    while (count>0) {
        int chunk=std::min(count,4096);
        for (int n=0;n<chunk;++n) {
            uint32 pcm=uint32(g_paintedtime)+n+1;
            testPaint[n].left=pcm & 65535; testPaint[n].right=pcm>>16;
        }
        d.TransferSamples(g_paintedtime+chunk); g_paintedtime+=chunk; count-=chunk;
    }
}
static OSStatus render(Device& d,AudioBufferList& buffer,UInt32 frames,AudioUnitRenderActionFlags& flags) {
    inCallback=true; callbackCopies=0;
    OSStatus status=d.RenderAudio(&flags,frames,&buffer);
    inCallback=false;
    require(callbackCopies<=2,"render exceeded two PCM copies");
    return status;
}
static std::vector<uint32> pull(Device& d,unsigned frames) {
    std::vector<uint32> pcm(frames,0xdeadbeef);
    AudioBufferList buffer={1,{{2,frames*4,pcm.data()}}};
    AudioUnitRenderActionFlags flags=0;
    require(render(d,buffer,frames,flags)==noErr,"render failed");
    return pcm;
}
static void tagged(const std::vector<uint32>& pcm,unsigned begin,unsigned count) {
    for (unsigned n=0;n<count;++n) require(pcm[n]==begin+n+1,"stale, missing or reordered PCM");
    for (unsigned n=count;n<pcm.size();++n) require(pcm[n]==0,"underrun was not silence");
}
static void start(Device& d) { paint(d,2048); d.PaintEnd(); require(current && current->running,"output not started"); }
static void stress(Device& d) {
    const unsigned total=4000000;
    std::atomic<bool> done(false);
    std::string consumerError;
    std::thread consumer([&] {
        try {
            unsigned expected=0;
            uint32 pcm[509];
            while (!done.load() || expected<total) {
                AudioBufferList b={1,{{2,sizeof(pcm),pcm}}}; AudioUnitRenderActionFlags flags=0;
                require(render(d,b,509,flags)==noErr,"concurrent render failed");
                bool silent=false;
                for (uint32 value:pcm) {
                    if (!value) silent=true;
                    else { require(!silent && value==++expected,"concurrent PCM corruption"); }
                }
                if (!expected) std::this_thread::yield();
            }
            require(expected==total,"concurrent frame count mismatch");
        } catch (const std::exception& e) { consumerError=e.what(); done.store(true); }
    });
    unsigned written=0;
    while (written<total && !done.load()) {
        if (d.FramesAvailableForHardware()>32768-251) { std::this_thread::yield(); continue; }
        unsigned count=std::min(251u,total-written); paint(d,count); written+=count;
    }
    done.store(true); consumer.join();
    require(consumerError.empty(),consumerError.c_str());
    require(written==total,"producer stopped early");
}
static void run(const std::string& name) {
    if (name=="late-listener") {
        Device* d=new Device;
        auto pending=listeners; delete d;
        for (const auto& l:pending) l.proc(l.object,1,&l.address,l.context);
        return;
    }
    if (name=="allocation-failure") {
        { Device probe; }
        int allocationsPerInit=allocationCount;
        for (int n=1;n<=allocationsPerInit;++n) {
            allocationCount=0; failAllocation=n;
            CAudioDeviceMacAudioUnit d;
            require(!d.Init(),"allocation failure accepted");
            require(!current,"allocation failure leaked an audio unit");
        }
        return;
    }
    if (name=="max-slice-failure") {
        failMaxSlice=true; CAudioDeviceMacAudioUnit d;
        bool ok=d.Init(); if (ok) d.Shutdown();
        require(!ok,"unsafe max-slice configuration accepted"); return;
    }
    if (name=="large-slice") hardwareFrames=16384;
    if (name=="scaled-null-storage") scaleSlice=true;
    Device d;
    if (name=="pcm-order") { paint(d,1024); tagged(pull(d,512),0,512); tagged(pull(d,512),512,512); }
    else if (name=="wrap-copy") {
        paint(d,32760); pull(d,32760); paint(d,1000); tagged(pull(d,1000),32760,1000);
    } else if (name=="partial-underrun") {
        paint(d,300); tagged(pull(d,512),0,300);
        paint(d,512); tagged(pull(d,512),300,512);
    } else if (name=="long-underrun") {
        paint(d,512); pull(d,512);
        for (int n=0;n<200;++n) tagged(pull(d,512),0,0);
        require(d.GetOutputPosition()==512,"empty callbacks moved the PCM clock");
        paint(d,1024); tagged(pull(d,1024),512,1024);
    } else if (name=="tiny-mixahead" || name=="zero-mixahead") {
        snd_mixahead.value=name=="tiny-mixahead"?0.001f:0;
        int end=d.PaintBegin(snd_mixahead.value,0,0); paint(d,end); d.PaintEnd();
        require(current->running,"small mix budget deadlocked startup");
    } else if (name=="mix-budget-bounds") {
        for (float value : {-1.0f,0.0f,0.001f,1.0f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
            int end=d.PaintBegin(value,0,0);
            require(end>=128 && end<32768 && end%4==0,"unsafe or full-lap mix budget");
        }
    } else if (name=="silence-flag") {
        uint32 pcm[512]; AudioBufferList b={1,{{2,sizeof(pcm),pcm}}}; AudioUnitRenderActionFlags flags=0;
        render(d,b,512,flags); require(flags & kAudioUnitRenderAction_OutputIsSilence,"missing silence flag");
        paint(d,512); render(d,b,512,flags); require(!(flags & kAudioUnitRenderAction_OutputIsSilence),"PCM marked as silence");
    } else if (name=="engine-rebase") {
        paint(d,2048); pull(d,1024); d.StopAllSounds();
        g_paintedtime=32768;
        paint(d,1024); tagged(pull(d,1024),32768,1024);
    } else if (name=="engine-clock-rebase") {
        const unsigned origin=0x6fffff00u;
        d.m_renderedFrames=origin; d.m_writtenFrames=origin;
        g_paintedtime=origin; paint(d,4408); pull(d,512);
        int sampled=d.GetOutputPosition(); require(sampled==256,"test did not cross rebase boundary");
        // Exact GetSoundTime rollover order: sample, reset paintedtime,
        // ClearBuffer/StopAllSounds, then assign soundtime from that sample.
        g_paintedtime=32768; d.ClearBuffer(); d.StopAllSounds();
        int soundtime=32768+sampled;
        int end=d.PaintBegin(0.1f,soundtime,g_paintedtime);
        paint(d,end-g_paintedtime);
        require(d.GetOutputPosition()==sampled,"rebase changed the sampled modulo clock");
        require(d.FramesAvailableForHardware()==end-soundtime,"rebase inflated queued latency");
        tagged(pull(d,512),soundtime,512);
    } else if (name=="movie-transition") {
        start(d); pull(d,512); recording=true; g_paintedtime=0;
        int end=d.PaintBegin(0.1f,735,0); paint(d,end);
        require(d.FramesAvailableForHardware()==0,"offline recording queued stale device audio");
        g_paintedtime=1000000; paint(d,512);
        recording=false; end=d.PaintBegin(0.1f,80000,g_paintedtime);
        require(g_paintedtime==80000,"movie exit retained offline timeline");
        paint(d,end-g_paintedtime); d.PaintEnd();
        require(current->running,"movie exit never restarted audio");
        tagged(pull(d,512),80000,512);
    } else if (name=="signed-arithmetic") {
        paint(d,32768);
        d.m_renderedFrames=0x7fffff80u; d.m_writtenFrames=0x80000080u;
        auto pcm=pull(d,256);
        for (unsigned n=0;n<256;++n) require(pcm[n]==((32640+n)&32767)+1,"signed boundary PCM mismatch");
        require(d.GetOutputPosition()==128,"signed boundary clock mismatch");
    } else if (name=="counter-wrap") {
        for (unsigned origin : {0x7fffff80u,0xffffff80u}) {
            d.m_renderedFrames=origin; d.m_writtenFrames=origin;
            g_paintedtime=origin & 32767;
            paint(d,512); tagged(pull(d,512),origin & 32767,512);
            require(d.GetOutputPosition()==int((origin+512)&32767),"clock rollover failed");
        }
    } else if (name=="short-buffer" || name=="zero-capacity") {
        uint32 pcm[4]={1,2,3,4}; AudioBufferList b={1,{{2,name=="zero-capacity"?0u:4u,pcm}}};
        AudioUnitRenderActionFlags flags=0;
        require(render(d,b,512,flags)!=noErr,"short buffer accepted");
        require(pcm[1]==2 && pcm[3]==4,"wrote beyond advertised capacity");
    } else if (name=="null-storage") {
        paint(d,512); AudioBufferList b={1,{{2,0,nullptr}}}; AudioUnitRenderActionFlags flags=0;
        require(render(d,b,512,flags)==noErr && b.mBuffers[0].mData,"did not supply preallocated PCM");
        require(static_cast<uint32*>(b.mBuffers[0].mData)[0]==1,"null-storage PCM missing");
    } else if (name=="scaled-null-storage") {
        paint(d,8192); AudioBufferList b={1,{{2,0,nullptr}}}; AudioUnitRenderActionFlags flags=0;
        require(render(d,b,8192,flags)==noErr && b.mBuffers[0].mData,"scaled maximum not allocated");
        require(static_cast<uint32*>(b.mBuffers[0].mData)[8191]==8192,"scaled output truncated");
    } else if (name=="bad-layout") {
        uint32 pcm[512]; AudioBufferList b={1,{{1,sizeof(pcm),pcm}}}; AudioUnitRenderActionFlags flags=0;
        require(render(d,b,512,flags)!=noErr,"unsupported layout silently accepted");
        int before=creates; d.PaintEnd(); require(creates==before+1,"render error did not recover");
    } else if (name=="clear-buffer") {
        start(d); d.ClearBuffer(); require(!current->running,"clear did not quiesce callback");
        paint(d,512); tagged(pull(d,512),2048,512);
    } else if (name=="stop-failure") {
        start(d); failStop=true; d.StopAllSounds();
        require(!current && disposes==1,"failed stop retained an active callback");
    } else if (name=="nested-pause") {
        start(d); d.Pause(); d.Pause(); d.PaintEnd(); d.UnPause();
        require(!current->running,"nested pause restarted unit");
        d.UnPause(); require(current->running,"unpause failed");
    } else if (name=="start-failure") {
        failStart=true; paint(d,2048); d.PaintEnd();
        int before=starts; d.PaintEnd(); d.PaintEnd(); d.UnPause();
        require(starts<=before+1,"failed start retried within cooldown");
        failStart=false; testTime=1.1; d.PaintEnd(); require(current->running,"failed start did not recover");
    } else if (name=="device-change" || name=="change-during-recovery") {
        start(d); notifyDevice();
        // A notification already queued before listener removal can arrive
        // while a replacement is being initialized.
        pendingListeners=listeners;
        changeDuringInit=name=="change-during-recovery";
        d.PaintEnd(); require(creates==2,"route not rebuilt");
        if (name=="change-during-recovery") {
            testTime=1.1; d.PaintEnd(); require(creates==3,"new route event lost");
        } else { d.PaintEnd(); require(creates==2,"route rebuilt twice"); }
    } else if (name=="create-failure") {
        start(d); failCreate=true; notifyDevice(); d.PaintEnd();
        int before=creates; d.PaintEnd(); require(creates==before,"create not rate limited");
        failCreate=false; testTime=1.1; d.PaintEnd(); require(current->running,"route failed to return");
    } else if (name=="failed-pause-recovery") {
        start(d); failStop=true; d.Pause(); require(!current,"failed pause retained unit");
        failStop=false; d.PaintEnd(); require(current && !current->running,"paused recovery started playback");
        d.UnPause(); require(current->running,"pause recovery never resumed");
    } else if (name=="restart-grace") {
        start(d); d.Pause(); testTime=100; d.UnPause(); d.PaintEnd();
        require(creates==1,"long pause caused spurious stall recovery");
    } else if (name=="no-callback-recovery") {
        start(d); testTime=3; d.PaintEnd(); require(creates==2,"silent driver never recovered");
    } else if (name=="underrun-not-stall") {
        start(d); pull(d,2048);
        for (int n=1;n<10;++n) { pull(d,512); testTime=n; d.PaintEnd(); }
        require(creates==1,"normal underrun churned devices");
    } else if (name=="movie-silence") {
        paint(d,32768); pull(d,32768); recording=true; paint(d,512); tagged(pull(d,512),0,0);
    } else if (name=="fifo-full") {
        paint(d,32768); paint(d,512); tagged(pull(d,32768),0,32768);
        require(d.FramesAvailableForHardware()==0,"FIFO overrun");
    } else if (name=="large-slice") {
        paint(d,32768); tagged(pull(d,16384),0,16384);
        require(current->maxFrames>=hardwareFrames,"max slice below hardware size");
    } else if (name=="spsc-stress") stress(d);
    else if (name=="benchmark-concurrent") {
        auto begin=std::chrono::steady_clock::now();
        stress(d);
        double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        std::cout<<"{\"million_frames_per_second\":"<<4.0/seconds<<"}\n";
    }
    else if (name=="benchmark") {
        for (unsigned frames : {32u,128u,512u,4096u}) {
            std::vector<uint32> pcm(frames);
            std::vector<double> times;
            const int iterations=30000;
            times.reserve(iterations);
            uint32 checksum=0;
            auto begin=std::chrono::steady_clock::now();
            for (int n=0;n<iterations+1000;++n) {
                paint(d,frames);
                AudioBufferList b={1,{{2,frames*4,pcm.data()}}}; AudioUnitRenderActionFlags flags=0;
                auto before=std::chrono::steady_clock::now();
                render(d,b,frames,flags);
                auto after=std::chrono::steady_clock::now();
                if (n>=1000) times.push_back(std::chrono::duration<double,std::nano>(after-before).count());
                else if (n==999) begin=after;
                checksum^=pcm.back();
            }
            double total=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-begin).count()/iterations;
            std::sort(times.begin(),times.end());
            std::cout<<"{\"frames\":"<<frames<<",\"callback_median_ns\":"<<times[iterations/2]
                     <<",\"callback_p99_ns\":"<<times[iterations*99/100]
                     <<",\"pair_mean_ns\":"<<total<<",\"checksum\":"<<checksum<<"}\n";
        }
    } else throw std::runtime_error("unknown test");
}
int main(int argc,char** argv) {
    if (argc!=2) return 2;
    try { run(argv[1]); std::cout<<"PASS "<<argv[1]<<'\n'; return 0; }
    catch (const std::exception& e) { std::cerr<<"FAIL "<<argv[1]<<": "<<e.what()<<'\n'; return 1; }
}
