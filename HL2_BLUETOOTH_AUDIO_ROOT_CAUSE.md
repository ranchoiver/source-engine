# HL2 Bluetooth Audio Stutter Root Cause

## Summary

The AirPods/Bluetooth stutter reported for the native macOS Half-Life 2 build was rooted in the macOS `AudioQueue` output backend, not in AirPods-specific game logic.

The engine's default macOS output path uses `AudioQueue` (`engine/audio/snd_win.cpp`) unless `-snd_openal` is passed. That backend pushed fixed-size PCM buffers into CoreAudio, but it clocked Source's mixer from the number of buffers submitted to `AudioQueue`, not the number of buffers CoreAudio had actually consumed. It also treated `AudioQueueEnqueueBuffer` and `AudioQueueStart` failures as console messages instead of device-loss or route-change recovery events.

Bluetooth devices make that fragile behavior visible because macOS can pause, restart, or renegotiate the output path when the route changes, when AirPods switch profiles, or when the system audio server hiccups. The old Source backend could then keep advancing its internal clock ahead of real playback, skip recovery after failed queue operations, and get stuck around stale queued-buffer state. Muting and unmuting the Mac helped because it forced the system output path to recover underneath the game.

## Why The Wiki Workarounds Helped

AppleGamingWiki lists two relevant workarounds:

- async audio: `snd_async_fullyasync 1; snd_async_minsize 0; snd_noextraupdate 1`
- AirPods stutter: mute and unmute the Mac

The async audio settings can hide stalls from file and streaming sound loads, and disabling extra updates reduces some main-thread mixer timing churn. They do not repair the output-device clock or recover a broken `AudioQueue`.

The mute/unmute workaround fits the actual output-backend failure mode better: it nudges macOS/CoreAudio to rebuild or restart the selected output route, which can get playback moving again even though Source itself did not repair its queue state.

## Broken Behavior

The old `AudioQueue` backend had three important problems:

1. `GetOutputPosition()` used `m_buffersSent`.
   - `m_buffersSent` means Source handed a buffer to `AudioQueue`.
   - It does not prove that the buffer was heard, consumed, or even survived a route change.
   - Source's mixer uses `GetOutputPosition()` as the hardware playback clock, so this made the engine chase a submitted-buffer clock instead of a playback-progress clock.

2. Failed queue operations did not recover.
   - `AudioQueueEnqueueBuffer` errors were logged but still followed by normal loop flow.
   - `AudioQueueStart` errors were ignored.
   - AudioQueue current-device changes were not treated as route changes that should rebuild output state.
   - A queue that stopped completing buffers could remain half-alive.

3. Immediate stops flushed CoreAudio queue state without re-syncing Source's submit cursor.
   - `AudioQueueStop(..., true)` returns unplayed buffers through their completion callbacks, so the in-flight counts self-corrected, but the submitted-buffer playback clock then included up to ~93 ms of audio that was never heard, and nothing re-aligned the submit cursor with the ring before new buffers went out.

## Fix

`engine/audio/snd_dev_mac_audioqueue.cpp` now:

- clocks playback from `m_buffersCompleted`, which is advanced by the `AudioQueue` output callback after CoreAudio has taken a buffer;
- makes `m_buffersCompleted` and `m_bRunning` interlocked because they are updated from AudioQueue callbacks/property listeners;
- does not advance submitted-buffer state after a failed enqueue;
- watches AudioQueue current-device changes (registered unconditionally: the property ID is an enum constant, so it cannot be feature-tested with the preprocessor);
- recovers the `AudioQueue` on invalid queue state, device changes, enqueue/prime/start/stop errors, and a running queue that stops completing buffers;
- rate-limits recovery to once per second across all trigger sites, allows at most one rebuild per mix pass from the enqueue path, and uses an escalating stall tolerance (2.5 s doubling to 20 s, reset on progress) so slow Bluetooth route establishment is not torn down mid-setup;
- preserves Source's mixed ring buffer during recovery, then re-syncs the submit cursor to the completed playback point (the up-to-~93 ms CoreAudio flushed is skipped rather than replayed, keeping the playback clock monotonic);
- clamps buffer submission to the mixer's painted frontier so ring regions still holding the previous lap's audio are never enqueued, protecting slow mix passes and lowered `snd_mixahead` values;
- explicitly primes the queue after buffers are enqueued and before playback starts;
- resets submitted queue state after pause uses `AudioQueueStop(..., true)`.

## Scope

This PR intentionally fixes the native macOS/AirPods stutter path without changing global mixer defaults or rewriting every backend.

A deeper macOS modernization should be a separate follow-up PR. That work should evaluate an Audio Unit/AUHAL or AVAudioEngine-style output path, device sample-rate/buffer-size tracking, and a callback-fed ring buffer with explicit underrun and drift handling. That is a larger backend design change than the focused `AudioQueue` route-recovery fix here.

## Validation

- `scripts/verify-audioqueue-recovery.ps1`
  - verifies that the macOS playback clock uses completed AudioQueue buffers rather than submitted buffers;
  - verifies enqueue/start/stall recovery paths, recovery rate limiting, and the painted-frontier submission clamp;
  - rejects `#ifdef`/`#if defined()` probes of CoreAudio enum constants (always false, silently dead-codes the feature);
  - verifies recovery preserves the mixed ring buffer.
  - Note: this is a source-shape check, not a compile or runtime test; it exists because this file only compiles on macOS.
- `py -3 waf configure -T release --build-games=hl2`
- `py -3 waf build --targets=soundemittersystem,vaudio_minimp3`
- `py -3 waf build --targets=engine -j1`

The macOS `AudioQueue` file cannot be compiled on this Windows host, so the targeted verifier covers the macOS-only invariants and the local host build catches shared engine/audio regressions.

## References

- AppleGamingWiki Half-Life 2 page: https://www.applegamingwiki.com/wiki/Half-Life_2
- Apple `AudioQueueOutputCallback`: https://developer.apple.com/documentation/audiotoolbox/audioqueueoutputcallback
- Apple `kAudioQueueProperty_IsRunning`: https://developer.apple.com/documentation/audiotoolbox/kaudioqueueproperty_isrunning
- Apple Audio Queue property IDs: https://developer.apple.com/documentation/audiotoolbox/audio-queue-property-ids
- Apple `AudioQueuePrime`: https://developer.apple.com/documentation/audiotoolbox/audioqueueprime%28_%3A_%3A_%3A%29
- Apple Core Audio overview: https://developer.apple.com/library/archive/documentation/MusicAudio/Conceptual/CoreAudioOverview/CoreAudioEssentials/CoreAudioEssentials.html
- Apple TN2321 low-latency audio: https://developer.apple.com/library/archive/technotes/tn2321/_index.html
