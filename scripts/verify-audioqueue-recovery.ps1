$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$sourcePath = Join-Path $repoRoot 'engine/audio/snd_dev_mac_audioqueue.cpp'
$source = Get-Content -Raw -LiteralPath $sourcePath

function Assert-Match {
    param(
        [string]$Pattern,
        [string]$Message
    )

    if ($source -notmatch $Pattern) {
        throw $Message
    }
}

Assert-Match 'CInterlockedInt\s+m_buffersCompleted\s*;' 'm_buffersCompleted must be interlocked because AudioQueue callbacks update it.'
Assert-Match 'CInterlockedInt\s+m_bRunning\s*;' 'm_bRunning must be interlocked because the AudioQueue property listener updates it.'
Assert-Match 'CInterlockedInt\s+m_bQueueDeviceChanged\s*;' 'AudioQueue current-device changes must be communicated safely to the game thread.'
Assert-Match 'void\s+CAudioDeviceAudioQueue::ResetQueuedBufferStateToPlayback' 'AudioQueue recovery must reset submitted queue state to the playback point.'
Assert-Match 'CloseWaveOut\(\s*false\s*\)' 'AudioQueue recovery must preserve Source''s mixed ring buffer.'
Assert-Match 'kAudioQueueProperty_CurrentDevice' 'AudioQueue backend must listen for output device changes.'
Assert-Match 'RecoverWaveOut\(\s*"AudioQueue device change"' 'AudioQueue recovery must handle route/device changes.'

# CoreAudio property IDs are enum constants, not macros: #ifdef/#if defined()
# on them is always false and silently compiles the guarded code out.
if ($source -match '#\s*if(def\s+|\s+defined\s*\(\s*)kAudio') {
    throw 'CoreAudio enum constants must not be probed with #ifdef/#if defined() - that always evaluates false and dead-codes the feature.'
}
Assert-Match 'RecoverWaveOut\(\s*"playback stall"' 'AudioQueue recovery must handle a running queue that stops completing buffers.'
Assert-Match 'RecoverWaveOut\(\s*"AudioQueueEnqueueBuffer"' 'AudioQueue recovery must handle enqueue failures.'
Assert-Match 'RecoverWaveOut\(\s*"AudioQueueStart"' 'AudioQueue recovery must handle start failures.'
Assert-Match 'RecoverWaveOut\(\s*"AudioQueuePrime"' 'AudioQueue recovery must handle prime failures.'
Assert-Match 'AudioQueuePrime\(\s*m_Queue,\s*0,\s*NULL\s*\)' 'AudioQueue restart paths must explicitly prime before starting playback.'
Assert-Match 'while\s*\(\s*QueuedBufferCount\(\)\s*<\s*cTargetQueuedBuffers\s*&&' 'Queue refill should be based on actual queued-buffer count.'
Assert-Match 'PaintedAheadFrames\(\)' 'Queue refill must be clamped to the mixer''s painted frontier so stale ring laps are never enqueued.'
Assert-Match 'm_flNextRecoverTime' 'Recovery must be rate-limited so failure loops cannot rebuild the queue at frame rate.'
Assert-Match 'm_flStallToleranceSec' 'The stall detector must tolerate slow Bluetooth route establishment and back off when recoveries stay unproductive.'
Assert-Match 'CAudioDeviceAudioQueue\s+\*pAudioQueue\s*=\s*\(CAudioDeviceAudioQueue\s+\*\)pContext\s*;' 'AudioQueue callback should use its context pointer, not the global device pointer.'
Assert-Match 'AudioQueueStop\(m_Queue,\s*true\);\s*ResetQueuedBufferStateToPlayback\(\);' 'Immediate AudioQueue stops must reset submitted-buffer state.'

$getOutputPosition = [regex]::Match(
    $source,
    'int\s+CAudioDeviceAudioQueue::GetOutputPosition\s*\(\s*void\s*\)\s*\{(?<body>.*?)^\}',
    [System.Text.RegularExpressions.RegexOptions]::Singleline -bor [System.Text.RegularExpressions.RegexOptions]::Multiline
)

if (!$getOutputPosition.Success) {
    throw 'Could not find CAudioDeviceAudioQueue::GetOutputPosition.'
}

$getOutputPositionBody = $getOutputPosition.Groups['body'].Value
if ($getOutputPositionBody -notmatch 'm_buffersCompleted') {
    throw 'GetOutputPosition must clock playback from completed AudioQueue buffers.'
}

if ($getOutputPositionBody -match 'm_buffersSent') {
    throw 'GetOutputPosition must not clock playback from submitted AudioQueue buffers.'
}

$enqueuePos = $source.IndexOf('AudioQueueEnqueueBuffer')
$recoverPos = $source.IndexOf('RecoverWaveOut( "AudioQueueEnqueueBuffer"', $enqueuePos)
$incrementPos = $source.IndexOf('m_buffersSent++;', $enqueuePos)

if ($enqueuePos -lt 0 -or $recoverPos -lt 0 -or $incrementPos -lt 0) {
    throw 'Could not find AudioQueue enqueue, recovery, and submitted-buffer increment sites.'
}

if ($incrementPos -lt $recoverPos) {
    throw 'Failed AudioQueueEnqueueBuffer calls must recover before m_buffersSent can advance.'
}

Write-Host 'Verified macOS AudioQueue recovery invariants.'
Write-Host 'Playback clock uses completed buffers; failed enqueue/start/stall paths recover; mixed ring buffer is preserved.'
