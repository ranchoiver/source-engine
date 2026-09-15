// Minimal engine boundary for compiling the production AudioQueue backend.
// Mixer and CoreAudio behavior are supplied by audioqueue_test.cpp.
#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>

typedef uint32_t uint32;
typedef unsigned char byte;
typedef int fixedint;
template<class T> class TestInterlocked {
    std::atomic<T> value;
public:
    TestInterlocked() : value(0) {}
    operator T() const { return value.load(); }
    T operator=(T n) { value.store(n); return n; }
    T operator++() { return value.fetch_add(1) + 1; }
    T operator++(int) { return value.fetch_add(1); }
    T AtomicAdd(T n) { return value.fetch_add(n); }
    T InterlockedExchange(T n) { return value.exchange(n); }
};
typedef TestInterlocked<int> CInterlockedInt;
typedef TestInterlocked<unsigned> CInterlockedUInt;
struct Vector {};
struct channel_t {};
struct portable_samplepair_t { int left, right; };
struct paintbuffer_t {
    int ifilter;
    portable_samplepair_t* pbuf;
    int fltmem[4][4];
};
struct IConVar {};
struct TestConVar {
    int value;
    void SetValue(int n) { value=n; }
    void InstallChangeCallback(void (*)(IConVar*, const char*, float)) {}
};
struct ConVarRef {
    explicit ConVarRef(IConVar*) {}
    int GetInt() const { return 2; }
};
struct IAudioDevice { virtual ~IAudioDevice() {} };
struct CAudioDeviceBase : IAudioDevice {
    bool m_bSurround, m_bSurroundCenter, m_bHeadphone;
};
extern TestConVar snd_surround, snd_legacy_surround;
extern int g_paintedtime;
extern portable_samplepair_t testPaint[4096];
extern double testTime;
inline double Plat_FloatTime() { return testTime; }
#define SOUND_DMA_SPEED 44100
#define CCHANVOLUMES 12
#define CPAINTFILTERS 4
#define CPAINTFILTERMEM 4
#define SAMPLE_16BIT_SHIFT 1
#define PAINTBUFFER testPaint
#define Q_memset std::memset
#define Q_memcpy std::memcpy
#define Assert assert
#define VPROF(x) ((void)0)
inline void DevMsg(const char*, ...) {}
inline void Msg(const char*, ...) {}
inline void MIX_ClearAllPaintBuffers(int, bool) {}
paintbuffer_t* MIX_GetCurrentPaintbufferPtr();
inline void S_MixBufferUpsample2x(int, portable_samplepair_t*, int*, int, int) {}
inline void Mix8MonoWavtype(channel_t*, portable_samplepair_t*, int*, byte*, int, fixedint, int) {}
inline void Mix8StereoWavtype(channel_t*, portable_samplepair_t*, int*, byte*, int, fixedint, int) {}
inline void Mix16MonoWavtype(channel_t*, portable_samplepair_t*, int*, short*, int, fixedint, int) {}
inline void Mix16StereoWavtype(channel_t*, portable_samplepair_t*, int*, short*, int, fixedint, int) {}
inline void DSP_Process(int, portable_samplepair_t*, portable_samplepair_t*, portable_samplepair_t*, int) {}
void S_TransferStereo16(void*, const portable_samplepair_t*, int, int);
