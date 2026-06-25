$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$audioUnitPath = Join-Path $repoRoot 'engine/audio/snd_dev_mac_audiounit.cpp'
$audioUnitHeaderPath = Join-Path $repoRoot 'engine/audio/snd_dev_mac_audiounit.h'
$sndWinPath = Join-Path $repoRoot 'engine/audio/snd_win.cpp'
$engineWscriptPath = Join-Path $repoRoot 'engine/wscript'
$rootWscriptPath = Join-Path $repoRoot 'wscript'
$engineVpcPath = Join-Path $repoRoot 'engine/engine.vpc'

$audioUnit = Get-Content -Raw -LiteralPath $audioUnitPath
$audioUnitHeader = Get-Content -Raw -LiteralPath $audioUnitHeaderPath
$sndWin = Get-Content -Raw -LiteralPath $sndWinPath
$engineWscript = Get-Content -Raw -LiteralPath $engineWscriptPath
$rootWscript = Get-Content -Raw -LiteralPath $rootWscriptPath
$engineVpc = Get-Content -Raw -LiteralPath $engineVpcPath

function Assert-TextMatch {
    param(
        [string]$Text,
        [string]$Pattern,
        [string]$Message
    )

    if ($Text -notmatch $Pattern) {
        throw $Message
    }
}

Assert-TextMatch $audioUnitHeader 'Audio_CreateMacAudioUnitDevice' 'AudioUnit backend must expose a factory.'
Assert-TextMatch $audioUnit '#include\s+<AudioUnit/AudioUnit\.h>' 'AudioUnit backend must use AudioUnit APIs.'
Assert-TextMatch $audioUnit '#include\s+<CoreAudio/CoreAudio\.h>' 'AudioUnit backend must use CoreAudio device APIs.'
Assert-TextMatch $audioUnit 'kAudioUnitSubType_HALOutput' 'Modern macOS output must use the HAL output AudioUnit.'
Assert-TextMatch $audioUnit 'AudioComponentFindNext' 'Backend should use AudioComponent APIs, not old Component APIs.'
Assert-TextMatch $audioUnit 'AudioComponentInstanceNew' 'Backend should instantiate an AudioUnit component.'
Assert-TextMatch $audioUnit 'kAudioUnitProperty_SetRenderCallback' 'AudioUnit output must be callback-driven.'
Assert-TextMatch $audioUnit 'AudioOutputUnitStart' 'AudioUnit backend must start output through AudioOutputUnitStart.'
Assert-TextMatch $audioUnit 'kAudioHardwarePropertyDefaultOutputDevice' 'Backend must track the default output device.'
Assert-TextMatch $audioUnit 'kAudioDevicePropertyNominalSampleRate' 'Backend must watch device sample-rate changes.'
Assert-TextMatch $audioUnit 'kAudioDevicePropertyBufferFrameSize' 'Backend must watch hardware buffer-size changes.'
Assert-TextMatch $audioUnit 'kAudioDevicePropertyDeviceIsAlive' 'Backend must watch device liveness changes.'
Assert-TextMatch $audioUnit 'AudioObjectAddPropertyListener' 'Backend must install CoreAudio property listeners.'
Assert-TextMatch $audioUnit 'RecoverAudioUnit\(\s*"CoreAudio device change"' 'Device changes must recover on the game thread.'
Assert-TextMatch $audioUnit 'm_underrunCount\+\+' 'Underruns must be counted.'
Assert-TextMatch $audioUnit 'SilenceOutput' 'Underruns and unsupported callback layouts must emit silence.'
Assert-TextMatch $audioUnit 'S_TransferStereo16\(\s*m_sndBuffers' 'Backend must preserve Source mixer ring semantics.'
Assert-TextMatch $audioUnit 'GetOutputPosition[\s\S]*m_renderedFrames' 'Playback clock must use frames rendered by the AudioUnit callback.'
Assert-TextMatch $audioUnit 'FramesAvailableForHardware\(\)\s*>=\s*m_startThresholdFrames' 'AudioUnit should wait for prefilled audio before starting.'

$renderMatch = [regex]::Match(
    $audioUnit,
    'OSStatus\s+CAudioDeviceMacAudioUnit::RenderAudio\s*\([^\)]*\)\s*\{(?<body>.*?)^\}',
    [System.Text.RegularExpressions.RegexOptions]::Singleline -bor [System.Text.RegularExpressions.RegexOptions]::Multiline
)

if (!$renderMatch.Success) {
    throw 'Could not find CAudioDeviceMacAudioUnit::RenderAudio.'
}

$renderBody = $renderMatch.Groups['body'].Value
if ($renderBody -match 'Audio(OutputUnit(Start|Stop)|Unit(Uninitialize|Initialize|SetProperty)|ComponentInstance(New|Dispose)|Object(Add|Remove)PropertyListener)') {
    throw 'AudioUnit render callback must not perform device lifecycle, property, or listener work.'
}

if ($renderBody -notmatch 'm_renderedFrames\s*\+=\s*requestedFrames') {
    throw 'Render callback must advance rendered frame clock by hardware-requested frames.'
}

if ($renderBody -notmatch 'CopyFromMixRing') {
    throw 'Render callback must copy from the pre-mixed Source ring.'
}

$audioUnitPos = $sndWin.IndexOf('Audio_CreateMacAudioUnitDevice')
$audioQueuePos = $sndWin.IndexOf('Audio_CreateMacAudioQueueDevice')
if ($audioUnitPos -lt 0 -or $audioQueuePos -lt 0 -or $audioUnitPos -gt $audioQueuePos) {
    throw 'macOS runtime selection must try AudioUnit before AudioQueue fallback.'
}

Assert-TextMatch $sndWin 'snd_macaudiounit' 'macOS AudioUnit default must be controllable by cvar.'
Assert-TextMatch $sndWin 'CheckParm\(\s*"-snd_audioqueue"\s*\)' 'Users must be able to force AudioQueue fallback.'
Assert-TextMatch $engineWscript 'audio/snd_dev_mac_audiounit\.cpp' 'Waf must compile the AudioUnit backend on Darwin.'
Assert-TextMatch $engineWscript "'AUDIOUNIT'" 'Engine Waf target must link the AudioUnit framework.'
Assert-TextMatch $rootWscript 'FRAMEWORK_AUDIOUNIT\s*=\s*"AudioUnit"' 'Root Waf configure must define the AudioUnit framework.'
Assert-TextMatch $engineVpc 'snd_dev_mac_audiounit\.cpp' 'VPC must include the AudioUnit backend source on macOS.'
Assert-TextMatch $engineVpc 'AudioUnit' 'VPC must link the AudioUnit framework.'

Write-Host 'Verified macOS AudioUnit modernization invariants.'
Write-Host 'AudioUnit is default before AudioQueue fallback; render callback is pull-based, route-aware, and real-time safe.'
