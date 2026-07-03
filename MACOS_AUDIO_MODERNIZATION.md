# macOS Audio Modernization

## Summary

The modern macOS output path now uses an Audio Unit HAL output backend by default, with the existing AudioQueue recovery backend retained as fallback and `-snd_openal` still available.

The new backend is designed around the macOS/Core Audio model instead of the old push-buffer AudioQueue loop:

- the hardware render callback pulls already-mixed Source PCM from the engine ring buffer;
- the callback never starts, stops, rebuilds, allocates, or calls engine mixing;
- underruns emit silence and increment a counter rather than reusing stale audio;
- default-output, sample-rate, buffer-size, and device-alive property listeners mark route changes;
- when supported by the SDK/runtime, CoreAudio is told not to favor power-saving audio behavior that can inflate I/O buffer size;
- the game thread performs device recovery and restart work outside the real-time callback, rate-limited to once per second;
- the mixer publishes its ring write cursor with a full memory barrier and the render callback reads it the same way, so the callback can never observe the cursor ahead of the ring bytes it covers (required on weakly-ordered Apple Silicon);
- `GetOutputPosition()` is clocked by frames rendered by the Audio Unit callback, advancing monotonically so `GetSoundTime()` never sees a spurious ring wrap.

## Runtime Selection

On macOS, `IAudioDevice::AutoDetectInit()` now tries:

1. Audio Unit HAL output, unless `-snd_audioqueue` is passed or `snd_macaudiounit 0` is set.
2. AudioQueue, if Audio Unit creation fails and `snd_audioqueue 1` is still enabled.
3. OpenAL, including the existing `-snd_openal` explicit path.

This keeps the new path default while preserving a working rollback path for older machines or SDK/runtime differences.

## Why AUHAL

Apple's Core Audio overview describes AUHAL as the output unit for dedicated I/O to a hardware device. That maps better to a game engine than queuing many fixed buffers into AudioQueue: the system asks for exactly the frames it needs, and the backend can keep its real-time work to a bounded copy from a pre-mixed ring.

The older AudioQueue path remains useful as a fallback and already gained route/stall recovery in the focused Bluetooth-stutter PR. The modernization PR moves the default toward Audio Units without throwing away that safety net.

## Latency And Stutter Policy

The backend keeps Source's existing `snd_mixahead` policy, but removes the extra AudioQueue submission depth from the default path. Startup waits until at least the current hardware buffer depth, clamped to a practical range, is already mixed before starting the Audio Unit. During playback, the Audio Unit callback advances the hardware clock by rendered frames and fills missing frames with silence if the engine falls behind.

It also sets `kAudioHardwarePropertyPowerHint` to `kAudioHardwarePowerHintNone` (gated on the macOS 10.9 SDK by version macro - the property ID is an enum constant, so `#if defined()` on it is always false and would silently compile the request out). Apple TN2321 documents that favoring power saving can increase the default I/O buffer size from 512 to 4096 frames (about 81 ms at 44.1 kHz); note that the power-saving policy is normally opt-in via an app's `AudioHardwarePowerHint` Info.plist key, so this request is defensive rather than a measured latency win, and it is nonfatal if it fails. Independent of the hint, the unit's `MaximumFramesPerSlice` is sized to cover the device's actual I/O buffer, because a render slice larger than that limit fails without reaching the callback at all.

The start threshold is additionally clamped to the `snd_mixahead` prefill budget: the mixer can never mix further ahead than that, so demanding more prefill than the budget (large power-save device buffers, or a user-lowered `snd_mixahead`) would leave the unit waiting forever with no error.

That gives a cleaner failure mode: a counted underrun instead of stale buffer replay, unbounded queue drift, or audio-thread device recovery.

## Validation

- `scripts/verify-macos-audiounit-modernization.ps1`
  - verifies Audio Unit is tried before AudioQueue on macOS;
  - verifies Audio Unit framework linkage in Waf and VPC;
  - verifies the render callback pulls from the Source mix ring, emits silence on underrun, and does not perform device lifecycle work;
  - verifies the low-latency CoreAudio power policy is requested when supported;
  - verifies route/device property listeners drive game-thread recovery.
- `scripts/verify-audioqueue-recovery.ps1`
- `git diff --check`
- `py -3 waf configure -T release --build-games=hl2`
- `py -3 waf build --targets=soundemittersystem,vaudio_minimp3`
- `py -3 waf build --targets=engine -j1`

This Windows host cannot compile macOS-only Audio Unit code, so the dedicated verifier covers the macOS backend invariants and the local Waf build catches shared engine/build regressions.

## References

- Apple Core Audio overview: https://developer.apple.com/library/archive/documentation/MusicAudio/Conceptual/CoreAudioOverview/CoreAudioEssentials/CoreAudioEssentials.html
- Apple Common Tasks in OS X, AUHAL current device: https://developer.apple.com/library/archive/documentation/MusicAudio/Conceptual/CoreAudioOverview/ARoadmaptoCommonTasks/ARoadmaptoCommonTasks.html
- Apple TN2091 AUHAL: https://developer.apple.com/library/archive/technotes/tn2091/_index.html
- Apple `AURenderCallback`: https://developer.apple.com/documentation/audiotoolbox/aurendercallback
- Apple TN2321 low-latency audio: https://developer.apple.com/library/archive/technotes/tn2321/_index.html
