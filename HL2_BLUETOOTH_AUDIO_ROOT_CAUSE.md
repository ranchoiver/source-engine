# HL2 macOS AudioQueue recovery

PR #2 hardens Source's macOS AudioQueue backend for Bluetooth/AirPods route
changes and output stalls. This is the native path unless `-snd_openal` is
selected. Other audio backends and global mixer defaults are unchanged.

## What the callback actually means

Apple calls `AudioQueueOutputCallback` when it has **acquired the buffer's
contents**, making that buffer reusable. The PCM may still be buffered
internally and may not have reached the speaker. Priming can return buffers
before playback starts. A return caused by an immediate reset can instead
represent discarded audio.

The backend therefore uses a **buffer-acquisition clock**, not an exact
speaker/DAC clock. This is more useful than counting submitted buffers, but
does not measure Bluetooth latency or prove audible playback progress.

The most serious remaining error in the earlier PR was treating zero pending
buffers as proof of audible starvation: it called `AudioQueueStop(..., true)`
and discarded PCM that CoreAudio could still be waiting to play. The reviewed
backend refills a running queue without stopping it just because its buffers
are reusable. A stopped queue is primed and started after data is enqueued.

## Reviewed behavior

- Completion and submission counters use unsigned arithmetic, including across
  wrap. Callback counters are read through interlocked operations.
- `GetOutputPosition()` masks to one ring before converting buffers to frames.
  The previous signed `completed * 1024` overflowed after 2,097,152 buffers,
  approximately 3.38 hours at 44.1 kHz with 256 stereo frames per buffer.
- Refill never submits a complete buffer beyond the mixer's painted frontier.
  `PaintBegin()` budgets at least one 256-frame buffer (about 5.8 ms), so
  lowering `snd_mixahead` below that duration cannot deadlock startup.
- Queue creation does not prime an empty queue. Playback follows Apple's
  documented enqueue -> prime -> start order.
- Enqueue, prime and start errors latch failure even when recovery is on
  cooldown. A broken queue is not retried every frame or partially restarted.
- Device changes, unavailable queues and stalled acquisition trigger recovery
  outside the CoreAudio callback. Rebuilds are rate-limited to once per second.
- A pending device-change request survives cooldown or creation failure. The
  next successful invalid-queue rebuild consumes the existing request rather
  than unnecessarily tearing down the newly restored device again. Changes
  arriving during a rebuild remain pending.
- Stall grace starts anew when playback starts, including after a long pause.
  Repeated stalls back off from 2.5 to 5, 10 and 20 seconds; subsequent buffer
  acquisition restores the initial tolerance.
- Pause and StopAllSounds share checked immediate-stop handling. If stop fails,
  the queue is synchronously disposed before any buffer can be reused.
- Disposal owns freeing the AudioQueue buffers. The backend no longer calls
  `AudioQueueFreeBuffer` while a failed-stop queue may still own a buffer.
- Successful synchronous stop/dispose accounts for discarded submitted PCM,
  preserving the forward clock and retaining the engine's allocated mix ring.
  No old-queue callback can run after synchronous disposal returns.
- `PaintEnd()` respects nested pauses. Allocation failure is handled before
  clearing the mix ring.

The engine ring contains 128 x 1,024 bytes, or 32,768 stereo frames. The refill
target is 16 outstanding buffers (about 93 ms); this does **not** include
additional PCM already acquired and buffered by CoreAudio or the device.

## Behavioral regression tests

```
python3 scripts/test-audioqueue-recovery.py --sanitize
```

Requires Python 3.8+ and GCC or Clang (`CXX` can select the compiler). The runner
compiles the **actual production backend** with a minimal engine boundary and
a deterministic fake AudioQueue. The fake independently models reusable
buffers and PCM still awaiting playback; it can inject creation, enqueue,
prime, start and stop failures. It does not duplicate the backend's recovery
state machine.

The 18 scenarios cover early buffer returns, tiny mixahead, long sessions,
unsigned and signed counter boundaries, stop/disposal ownership, nested pause,
restart grace, failure cooldowns, route recovery, stall backoff and the painted
frontier. `--source /path/to/old.cpp` runs the same cases against an earlier
revision. Several fail against `c59ec07`, including signed overflow detected by
UndefinedBehaviorSanitizer.

On macOS, also run:

```
python3 scripts/test-audioqueue-recovery.py --native-syntax
```

This additionally compiles the backend against the real Apple AudioToolbox SDK
declarations, retaining the minimal engine boundary. It is not a full engine
build. The dedicated GitHub workflow runs behavioral checks on Linux/macOS and
this SDK check on macOS when Actions are enabled for the fork.

`scripts/verify-audioqueue-recovery.ps1` remains a supplemental source-pattern
check. A Windows engine build does not compile this macOS-only implementation.

## Runtime verification still needed

The deterministic tests verify backend control flow, buffer ownership and clock
arithmetic. They cannot establish the real AirPods/Bluetooth listening result.
On a native macOS build, check speaker -> Bluetooth -> speaker transitions,
disconnect/reconnect, long and nested pauses, loading hitches, and small
`snd_mixahead` values. Confirm there is no repeated clipping/restarting and
that audio resumes after the output route returns.

## API references

- [AudioQueueOutputCallback](https://developer.apple.com/documentation/audiotoolbox/audioqueueoutputcallback)
- [AudioQueuePrime](https://developer.apple.com/documentation/audiotoolbox/audioqueueprime(_:_:_:))
- [AudioQueueStop](https://developer.apple.com/documentation/audiotoolbox/audioqueuestop(_:_:))
- [AudioQueueReset](https://developer.apple.com/documentation/audiotoolbox/audioqueuereset(_:))
- [AudioQueueDispose](https://developer.apple.com/documentation/audiotoolbox/audioqueuedispose(_:_:))
