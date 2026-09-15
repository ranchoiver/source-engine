# macOS AudioUnit output

PR #3 is stacked on PR #2 (`codex/fix-hl2-bluetooth-audio-stutter`). The review
integrates PR #2's `7a0926a` fixes into the modernization branch; neither PR
is merged into `master` by this update.

## Output selection

macOS tries AUHAL first, then AudioQueue if creation fails and `snd_audioqueue`
is enabled, then OpenAL. `-snd_audioqueue` bypasses AUHAL; `-snd_openal`
bypasses both native backends. `snd_macaudiounit 0` also disables AUHAL.
These are initialization fallbacks; a later AUHAL device failure is handled by
rebuilding AUHAL, not by replacing the active backend with AudioQueue.

## PCM ownership and clock

Source mixes stereo 16-bit PCM at 44.1 kHz on the game/mixer thread. The original
`S_TransferStereo16` still handles conversion, master volume, and recording.
Its scratch ring belongs exclusively to that thread. Completed PCM is copied
into a separate 32,768-frame single-producer/single-consumer FIFO.

The producer publishes its unsigned write cursor with a release store. The
callback acquires it before reading PCM, then releases consumed slots after
copying. The producer acquires the read cursor before reusing slots. These
C++11 atomics must be lock-free at compile time. The callback performs at most
two PCM copies, optional silence fill, and fixed-size atomic operations. It
never mixes, allocates, logs, waits, or performs device operations. The extra
FIFO costs 128 KiB and one producer-side copy (176,400 bytes/second at the
Source mix rate); it adds no second queue of scheduled audio.

`GetOutputPosition` counts PCM actually consumed from this FIFO, modulo its
size. Silence on underrun does not advance Source's mixer clock. This keeps
long mixer stalls from lapping or resetting the clock and lets playback resume
with the next queued samples. It is a PCM-consumption clock, **not** a measured
speaker/DAC/Bluetooth presentation timestamp. CoreAudio conversion and the
output device can add latency after consumption.

The mixer budget remains controlled by `snd_mixahead`, with a 128-frame minimum
(about 2.9 ms) to guarantee startup and four frames reserved below a full ring
to keep progress observable through Source's modulo clock. Startup prefill
fits the actual last `PaintBegin` budget. Pathological low settings can still
underrun; this is not a promise of 2.9 ms audible latency.

Engine timeline resets discard the old FIFO while output is stopped and
re-anchor it to the position already sampled by `GetSoundTime`. Catch-up PCM
before that point is mixed but not played. Offline movie/replay recording
retains the original recording transfer, queues no stale scratch PCM, and
resumes live mixing at the hardware-derived engine timeline on exit.

## Callback buffers and recovery

The callback validates interleaving, channel count, pointer, and advertised
capacity before writing. A null output-data pointer is served from storage
allocated before startup. Allocation uses the unit's maximum slice queried
**after** initialization, including enlargement for conversion. Invalid layouts
or capacities return an error and request recovery outside the callback.

Pause and clearing stop the unit before discarding slots. Failed stops force
teardown before buffer reuse. Start failures stay latched through recovery
cooldown; nested pauses are honored. Callback progress is monitored separately
from PCM consumption: 2.5 seconds without callbacks triggers recovery, while
an active callback producing silence does not churn devices. Successful starts
receive a fresh stall grace period.

CoreAudio default-output, nominal-rate, buffer-size, and alive listeners use a
process-lifetime generation counter rather than a pointer to a destructible
backend. Recovery acknowledges only the generation observed before rebuilding;
a later event remains pending. A second default-device query covers the gap
between choosing a device and registering listeners. Rebuilds are limited to
once per second.

The guarded `kAudioHardwarePowerHintNone` request is nonfatal. It opts out of
CoreAudio's documented power-saving policy; it is not a measured latency gain.
Hardware buffer size, sample conversion, and Bluetooth transport still matter.

## Validation

Run from the repository root with Python 3.8+ and GCC/Clang:

```sh
python3 scripts/test-audiounit.py --sanitize
python3 scripts/test-audiounit.py --thread-sanitize --test spsc-stress
python3 scripts/test-audioqueue-recovery.py --sanitize
# macOS: real Apple SDK declarations, both arm64 and x86_64
python3 scripts/test-audiounit.py --native-syntax
# Informational local producer + callback timing, not device latency
python3 scripts/test-audiounit.py --test pcm-order --benchmark
```

The harness compiles the **actual production backend** with a minimal engine
boundary and deterministic fake CoreAudio. It checks PCM order, concurrent
ownership, both cursor boundaries, short/null/large buffers, underruns, startup
budgets, the engine's exact rebase order, recording transitions, failed API
operations/allocations, late listeners, and route recovery. The stress test
checks four million ordered stereo frames and enforces at most two callback
copies. Runtime guards reject allocations, logging, mixing, and platform
operations from the callback. The targeted workflow runs both backends on
Linux/macOS and adds ThreadSanitizer and real Apple SDK checks.

Local review validation: 36 AudioUnit tests and all 18 AudioQueue tests pass
with AddressSanitizer/UBSan; the concurrent transport passes ThreadSanitizer.
Local LeakSanitizer is disabled because this host blocks its process scan.
The supplemental PowerShell script checks source/build patterns only; it is
not a behavioral or real-time correctness proof.

These tests and SDK checks do not establish a successful full engine build,
audible quality, or actual CoreAudio scheduling and driver behavior. Before
merging for release, build/run the native game and listen through built-in
output and Bluetooth. Exercise 44.1/48 kHz devices, route changes, disconnects,
sleep/wake, pause/resume, recording exit, and ordinary gameplay stalls.
Compare `-snd_audioqueue` when diagnosing output differences.

## Transport optimization review

The data structure remains a fixed-capacity SPSC ring with bulk release/acquire
publication. More general concurrent queues would add machinery unnecessary
for a single producer and consumer. The callback's metadata work is O(1);
copying/filling N requested frames is O(N).

The follow-up optimization separates producer-written state, callback-written
state, and game-thread bookkeeping with 128-byte padding gaps. This prevents
cross-owner false sharing for cache lines up to that size without requiring
an over-aligned allocation from the engine's C++11 `new` implementation. The
object grows by 256 bytes; audio capacity and prefill are unchanged.

Only the render callback writes its progress/underrun counters while running,
so those counters now use atomic load/store rather than fetch-add. This removes
unnecessary read-modify-write instructions and potential exclusive-store retry
loops. The multi-writer device-notification counter still uses fetch-add.
Empty callbacks no longer store an unchanged consumption cursor.

A direct-conversion prototype reserved FIFO slots and wrote converted PCM
there without the producer copy. It passed the ownership tests, but host
benchmarks gave mixed results and worse 4096-frame callback timings. It was
not retained. Remote-cursor caching and double-mapped virtual-memory rings
are also deferred: batching already amortizes synchronization, and the current
ring needs at most two copies at wrap. Avoiding those rare splits does not
justify new reset/lifetime/VM complexity without target-device measurements.

Run the reproducible comparison (the baseline commit must exist locally):

```sh
python3 scripts/benchmark-audiounit.py --repetitions 7
```

The benchmark compiles both real backend revisions with the same fake platform
and mixer boundary. Each sample has a warmup and 30,000 measured callbacks;
seven paired runs alternate baseline/candidate ordering. The committed raw
sample is `scripts/tests/audiounit/benchmark-linux-x86_64.json`.

| Frames/callback | Baseline median / p99 | Optimized median / p99 |
|---|---:|---:|
| 32 | 37 / 43 ns | 34 / 37 ns |
| 128 | 39 / 48 ns | 37 / 45 ns |
| 512 | 62 / 79 ns | 52 / 67 ns |
| 4096 | 335 / 369 ns | 326 / 356 ns |

These are medians of seven runs' callback medians/p99s on a Linux x86-64 host
with GCC 13.3, including timer and harness overhead, without CPU affinity or
real audio cadence. The concurrent throughput stress also improved in this
sample but is particularly sensitive to scheduler placement and busy polling.
These measurements are **not** game FPS, audible latency, full mixer cost,
or a macOS performance guarantee. No timing threshold is used as a CI gate.

The next meaningful latency work requires a native game trace: measure mixer
scheduling jitter, callback deadlines and underruns before considering an
optional bounded adaptive prefill policy. Device/Bluetooth presentation delay
cannot be inferred from this FIFO's consumption clock.

This applies established specialized-queue techniques, as discussed in
[Rigtorp's SPSC implementation notes](https://github.com/rigtorp/SPSCQueue#implementation);
it does not add that queue as a dependency.

## Apple references

- [AUHAL I/O](https://developer.apple.com/library/archive/technotes/tn2091/_index.html)
- [Render callback](https://developer.apple.com/documentation/audiotoolbox/aurendercallback)
- [Maximum slice sizes and conversion](https://developer.apple.com/library/archive/qa/qa1533/_index.html)
- [Low-latency audio and power policy](https://developer.apple.com/library/archive/technotes/tn2321/_index.html)
