// Shared minimal engine declarations; the real backend and real atomics run.
#pragma once
#include "../audioqueue/audio_pch.h"
#include <limits>
typedef int32_t int32;
template<class T> T Min(T a,T b) { return std::min(a,b); }
template<class T> T Max(T a,T b) { return std::max(a,b); }
struct ConVar { float value; float GetFloat() const { return value; } };
// Compatibility for reproducing the old source, which used engine atomics.
inline int operator--(CInterlockedInt& v,int) { return v.AtomicAdd(-1); }
inline int operator--(CInterlockedInt& v) { return v.AtomicAdd(-1)-1; }
inline int operator+=(CInterlockedInt& v, int n) { return v.AtomicAdd(n)+n; }
inline int32 ThreadInterlockedExchangeAdd(volatile int32* p,int32 n) { return __atomic_fetch_add(p,n,__ATOMIC_SEQ_CST); }

extern bool recording;
struct TestMovieInfo { bool IsRecording() const { return recording; } };
extern TestMovieInfo cl_movieinfo;
inline bool IsReplayRendering() { return false; }
