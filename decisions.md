# HL2 Bluetooth Audio Investigation Log

## 2026-06-26

- Created separate branch `codex/fix-hl2-bluetooth-audio-stutter` from `origin/master` so this PR stays independent from the flashlight work.
- AppleGamingWiki notes two audio clues for the native macOS build:
  - async-audio workaround: `snd_async_fullyasync 1; snd_async_minsize 0; snd_noextraupdate 1`
  - AirPods bug: audio starts stuttering randomly; muting/unmuting the Mac fixes it temporarily.
- The async-audio workaround reduces blocking sound-file/stream loads and disables extra main-thread mixer updates; it does not address the output device clock or Bluetooth route changes directly.
- The native macOS path in `engine/audio/snd_win.cpp` uses `AudioQueue` by default; OpenAL is selected only by the `-snd_openal` launch option or if AudioQueue creation fails. (A `snd_audioqueue` convar exists but is never consulted by the selection logic.)
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
  - and on a running queue that has queued buffers but no completion progress for longer than the stall tolerance.
- Registered the current-device listener unconditionally. `kAudioQueueProperty_CurrentDevice` is an enum constant, not a macro, so it cannot be feature-tested with `#ifdef` (an earlier revision did exactly that, which always evaluates false and silently compiled the listener out); the property exists in every SDK this engine builds against, and registration failure is nonfatal.
- Recovery preserves Source's mixed ring buffer and re-syncs the submit cursor to the completed playback point. Because `AudioQueueStop(..., true)` flushes unplayed buffers by firing their completion callbacks, up to the in-flight ~93 ms is skipped rather than replayed; the important property is that the completed-buffer playback clock stays monotonic and the rebuilt queue resumes from valid painted ring data.
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

## 2026-07-02 review fixes

A full review of the backend produced these hardening changes:

- Removed the `#ifdef kAudioQueueProperty_CurrentDevice` guards: the property ID is an enum constant, so the guard was always false and the route-change listener (the headline recovery trigger) never compiled in. The listener is now registered unconditionally.
- Clamped buffer submission to the mixer's painted frontier (`PaintedAheadFrames()`). With the playback clock now derived from completed buffers, the old fixed 16-buffer refill left only ~7 ms between the enqueue frontier and the painted frontier; a slow mix pass or a lowered `snd_mixahead` would enqueue ring regions still holding the previous lap's audio (~0.74 s old). The clamp is computed from pure deltas (`g_paintedtime - g_soundtime`, re-based onto the live completion count via the snapshot taken in `GetOutputPosition`), so it survives the engine's `g_paintedtime` rebases.
- Rate-limited recovery with a 1 s cooldown shared by all trigger sites, and limited enqueue-failure recovery to one rebuild per `PaintEnd`; the previous code could loop teardown/rebuild of a 128-buffer queue on the main thread indefinitely.
- Raised the stall tolerance to 2.5 s with exponential backoff to 20 s while recoveries stay unproductive (reset on real progress). AirPods route establishment can take 1-2 s during which a started queue legitimately completes nothing; a 1 s hair-trigger tears the route down mid-setup and can thrash.
- A declined device-change recovery (paused or on cooldown) now re-arms the change flag instead of dropping it.
- Fixed uninitialized `ioDataSize` passed to `AudioQueueGetProperty` in the IsRunning listener (must be `sizeof(running)` on input); with stack garbage the call could fail nondeterministically and leave `m_bRunning` stale.
- Added a third submit-cursor re-sync after the queue rebuild in `RecoverWaveOut`, closing the window where a late flush callback could leave `sent < completed`.
- Removed the dead prime/start block in `UnPause` (`Pause` re-syncs the cursors, so the queued count is always zero there; `PaintEnd` performs the actual restart).

## 2026-07-03 AudioUnit review fixes

A full review of the AudioUnit backend produced these fixes:

- Fixed `kAudioDevicePropertyScopeGlobal` (does not exist in any Apple SDK) to `kAudioObjectPropertyScopeGlobal` in the buffer-size listener address. This was a hard compile error that would have broken the entire macOS engine build, fallback paths included.
- Replaced `#if defined( kAudioHardwarePropertyPowerHint )` (always false - enum constants are invisible to the preprocessor, so the whole power-hint feature was compiled out) with an SDK version gate, and did the same for the `kAudioObjectPropertyElementMain` compatibility define. Documented that the TN2321 power-saving policy is normally opt-in via Info.plist, so the request is defensive.
- Added a barrier-published ring write cursor (`m_writtenFrames`): `TransferSamples` publishes it with `ThreadInterlockedExchangeAdd` after `S_TransferStereo16`, and the render callback reads it the same way instead of touching `g_paintedtime` directly. Plain int reads of a global the mixer writes concurrently were a data race, and on weakly-ordered Apple Silicon the callback could observe the new cursor before the ring bytes and copy a stale lap.
- Made the render clock update a single monotonic store (`m_renderedFrames = startFrame + requestedFrames`). The old resync path stored a backward value first and re-advanced it, and `GetSoundTime()` interprets any decrease as a full ring wrap - a spurious +0.74 s jump of `g_soundtime`. Transient stalls now emit silence while moving forward; only a beyond-one-lap divergence (engine timeline rebase) snaps the clock. The same forward-only rule now guards `StopAllSounds`.
- Clamped the start threshold to the `snd_mixahead` prefill budget. The mixer can never mix more than `snd_mixahead * 44100` frames ahead, so a threshold above that (4096-frame power-save device buffers, or a lowered `snd_mixahead`) meant the unit never started: permanent silence with no failure flag.
- Sized `MaximumFramesPerSlice` to cover the device's actual I/O buffer instead of capping at 4096; an oversized render slice fails with `kAudioUnitErr_TooManyFramesToProcess` and the callback never runs.
- Rate-limited `RecoverAudioUnit` to once per second and made `ServiceDeviceChanges` acknowledge only the device-change generation observed before the rebuild, so a change landing mid-recovery still triggers another pass.
- Extended the verifier accordingly, including rejecting `#ifdef` probes of CoreAudio enum constants and requiring the barrier-published cursor.
