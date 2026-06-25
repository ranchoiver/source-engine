# HL2 Bluetooth Audio Investigation Log

## 2026-06-26

- Created separate branch `codex/fix-hl2-bluetooth-audio-stutter` from `origin/master` so this PR stays independent from the flashlight work.
- AppleGamingWiki notes two audio clues for the native macOS build:
  - async-audio workaround: `snd_async_fullyasync 1; snd_async_minsize 0; snd_noextraupdate 1`
  - AirPods bug: audio starts stuttering randomly; muting/unmuting the Mac fixes it temporarily.
- The async-audio workaround reduces blocking sound-file/stream loads and disables extra main-thread mixer updates; it does not address the output device clock or Bluetooth route changes directly.
- The native macOS path in `engine/audio/snd_win.cpp` uses `AudioQueue` by default (`snd_audioqueue 1`), falling back to OpenAL only with `-snd_openal` or if AudioQueue creation fails.
- The macOS `AudioQueue` backend uses a push queue of 1024-byte buffers and tries to keep about 16 buffers queued. That is roughly 93 ms at 44.1 kHz stereo 16-bit.
- Potential root cause direction:
  - Bluetooth/CoreAudio devices can pause, restart, and alter callback cadence during route changes.
  - The current `AudioQueue` backend only tracks completed buffers through callbacks and restarts when it sees the queue stopped/starved during `PaintEnd()`.
  - It does not listen for AudioQueue/CoreAudio device-state changes, does not recover failed enqueue/start calls by recreating the queue, and its playback clock is derived from submitted buffer count rather than actual device time.
- Implementation direction:
  - Keep the fix scoped to the macOS `AudioQueue` output backend first, because that is the native default path described by the page.
  - Add robust queue recovery for stopped/starved/failed AudioQueue states.
  - Prefer actual AudioQueue timeline/progress when available so Source's mixer tracks playback instead of submission depth.

### Patch decisions

- Changed the macOS `AudioQueue` playback cursor to advance from `m_buffersCompleted` instead of `m_buffersSent`.
  - `m_buffersCompleted` is incremented by the AudioQueue output callback after CoreAudio has consumed a buffer.
  - `m_buffersSent` only means Source handed a buffer to AudioQueue, which can be ahead of playback and can be wrong after route-change failures.
- Made `m_buffersCompleted` and `m_bRunning` interlocked because the AudioQueue callback/property listener can update them off the game thread.
- Stopped counting failed `AudioQueueEnqueueBuffer` calls as submitted audio.
- Added AudioQueue recovery:
  - on invalid/failed queue state,
  - on AudioQueue current-device changes,
  - on enqueue/start/stop errors,
  - and on a running queue that has queued buffers but no completion progress for more than one second.
- Guarded the current-device listener with `#ifdef kAudioQueueProperty_CurrentDevice` so newer SDKs get route-change recovery without breaking older SDK builds that do not expose that property.
- Recovery preserves Source's mixed ring buffer but resets submitted queue state back to the completed playback point. That lets the rebuilt AudioQueue resubmit already-mixed samples instead of skipping ahead or playing from an uninitialized buffer.
- Recovery resets submitted state again after the old queue is closed, because immediate stop/dispose can still race with completion callbacks.
- Pause now also resets submitted queue state after `AudioQueueStop(..., true)`, because immediate stop discards pending CoreAudio buffers.
- Restart paths now call `AudioQueuePrime` after buffers are enqueued and before `AudioQueueStart`, so route recovery explicitly re-primes CoreAudio instead of relying on stale queue state.
- Kept `snd_mixahead`, `snd_async_fullyasync`, `snd_async_minsize`, and `snd_noextraupdate` defaults unchanged. Those are workarounds for different symptoms and should not be the permanent fix for Bluetooth output recovery.
- Broader modernization should be macOS-only in a follow-up PR, likely evaluating AUHAL/Audio Units or AVAudioEngine plus a callback-fed ring buffer, route-property tracking, underrun counters, and drift policy.

## 2026-06-26 macOS modernization PR

- Rebased `codex/modernize-macos-audio-stack` on top of PR #2 (`codex/fix-hl2-bluetooth-audio-stutter`) after the user asked for the modernization PR to build on the focused Bluetooth fix.
- Chose Audio Unit HAL output (`kAudioUnitSubType_HALOutput`) as the default modern macOS backend because it matches Apple's hardware I/O model and lets CoreAudio pull exactly the frames it needs.
- Kept the existing AudioQueue backend as a fallback instead of deleting it. PR #2 already hardens that path, and a fallback is useful for older systems or unexpected AUHAL setup failures.
- Added `snd_macaudiounit` as a default-on macOS cvar and `-snd_audioqueue` as a command-line escape hatch for the legacy AudioQueue path.
- The Audio Unit render callback only copies from the pre-mixed Source ring, emits silence for underruns, increments counters, and advances the rendered-frame clock. It does not allocate, call mixer code, restart devices, or mutate CoreAudio properties.
- CoreAudio default-output, sample-rate, buffer-size, and device-alive listeners only mark a route/device generation. The game thread performs recovery from `PaintBegin()`/`PaintEnd()`.
- Added AudioUnit framework linkage to Waf; VPC already linked the framework, and now also includes the new backend source/header.
- Added a guarded `kAudioHardwarePropertyPowerHint` / `kAudioHardwarePowerHintNone` request for the AudioUnit backend. This is nonfatal and SDK-compatible, but prevents macOS from selecting the power-saving audio policy that Apple documents can expand the default I/O buffer from 512 to 4096 frames.
