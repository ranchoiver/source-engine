//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: macOS Audio Unit output device
//
//===========================================================================//

#include "audio_pch.h"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <atomic>

// A render callback must never fall back to a library lock.
static_assert( ATOMIC_INT_LOCK_FREE == 2, "AudioUnit requires lock-free 32-bit atomics" );

// kAudioObjectPropertyElementMain replaced ...ElementMaster in the macOS 12
// SDK. Both are enum constants, so they cannot be probed with #ifndef; gate
// on the SDK version instead.
#if !defined( MAC_OS_VERSION_12_0 ) || MAC_OS_X_VERSION_MAX_ALLOWED < 120000
#define kAudioObjectPropertyElementMain kAudioObjectPropertyElementMaster
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

extern bool snd_firsttime;
extern int g_paintedtime;
extern bool MIX_ScaleChannelVolume( paintbuffer_t *ppaint, channel_t *pChannel, int volume[CCHANVOLUMES], int mixchans );
extern void S_SpatializeChannel( int volume[6], int master_vol, const Vector *psourceDir, float gain, float mono );

#define AUDIOUNIT_RING_FRAMES 32768
#define AUDIOUNIT_RING_BYTES ( AUDIOUNIT_RING_FRAMES * 2 * sizeof( short ) )

class CAudioDeviceMacAudioUnit : public CAudioDeviceBase
{
public:
	bool		IsActive( void );
	bool		Init( void );
	void		Shutdown( void );
	void		PaintEnd( void );
	int			GetOutputPosition( void );
	void		ChannelReset( int entnum, int channelIndex, float distanceMod );
	void		Pause( void );
	void		UnPause( void );
	float		MixDryVolume( void );
	bool		Should3DMix( void );
	void		StopAllSounds( void );

	int			PaintBegin( float mixAheadTime, int soundtime, int paintedtime );
	void		ClearBuffer( void );
	void		UpdateListener( const Vector& position, const Vector& forward, const Vector& right, const Vector& up );
	void		MixBegin( int sampleCount );
	void		MixUpsample( int sampleCount, int filtertype );
	void		Mix8Mono( channel_t *pChannel, char *pData, int outputOffset, int inputOffset, fixedint rateScaleFix, int outCount, int timecompress );
	void		Mix8Stereo( channel_t *pChannel, char *pData, int outputOffset, int inputOffset, fixedint rateScaleFix, int outCount, int timecompress );
	void		Mix16Mono( channel_t *pChannel, short *pData, int outputOffset, int inputOffset, fixedint rateScaleFix, int outCount, int timecompress );
	void		Mix16Stereo( channel_t *pChannel, short *pData, int outputOffset, int inputOffset, fixedint rateScaleFix, int outCount, int timecompress );

	void		TransferSamples( int end );
	void		SpatializeChannel( int volume[CCHANVOLUMES/2], int master_vol, const Vector& sourceDir, float gain, float mono );
	void		ApplyDSPEffects( int idsp, portable_samplepair_t *pbuffront, portable_samplepair_t *pbufrear, portable_samplepair_t *pbufcenter, int samplecount );

	const char *DeviceName( void )			{ return "AudioUnit"; }
	int			DeviceChannels( void )		{ return 2; }
	int			DeviceSampleBits( void )	{ return 16; }
	int			DeviceSampleBytes( void )	{ return 2; }
	int			DeviceDmaSpeed( void )		{ return SOUND_DMA_SPEED; }
	int			DeviceSampleCount( void )	{ return m_deviceSampleCount; }

	OSStatus	RenderAudio( AudioUnitRenderActionFlags *ioActionFlags, UInt32 inNumberFrames, AudioBufferList *ioData );

private:
	bool		OpenAudioUnit( void );
	void		CloseAudioUnit( bool bFreeMixBuffer = true );
	bool		RecoverAudioUnit( const char *pReason, OSStatus nError = noErr );
	bool		ValidAudioUnit( void ) const;
	bool		StartAudioUnit( void );
	bool		StopAudioUnit( void );
	int			StartThresholdFrames( void );
	void		ServiceDeviceChanges( void );
	void		FillStreamFormat( AudioStreamBasicDescription *pFormat );
	bool		GetDefaultOutputDevice( AudioDeviceID *pDeviceID ) const;
	void		InstallDeviceListeners( AudioDeviceID deviceID );
	void		RemoveDeviceListeners( void );
	void		RefreshDeviceTiming( AudioDeviceID deviceID );
	int			FramesAvailableForHardware( void );
	void		CopyFromMixRing( short *pOutput, int startFrame, int frameCount );
	void		DiscardQueuedFrames( void );
	void		SilenceOutput( AudioBufferList *ioData, UInt32 inNumberFrames ) const;

	AudioUnit						m_AudioUnit;
	AudioDeviceID					m_OutputDeviceID;
	AudioStreamBasicDescription		m_SourceFormat;

	int				m_SndBufSize;
	void			*m_sndBuffers;
	int				m_deviceSampleCount;

	// The mixer owns m_sndBuffers. The callback reads only this SPSC FIFO.
	// Cursors count PCM frames, independently of Source's rebased timeline.
	void			*m_outputBuffers;
	std::atomic<unsigned> m_writtenFrames;
	// Separate producer/consumer writes even when the engine's C++11
	// allocator does not support over-aligned new. Padding rather than an
	// over-aligned class preserves its ordinary allocation contract.
	char m_producerPadding[128];
	std::atomic<unsigned> m_renderedFrames;
	std::atomic<unsigned> m_underrunCount;
	std::atomic<unsigned> m_callbackCount;
	std::atomic<int> m_renderError;
	char m_consumerPadding[128];
	unsigned m_lastCallbackCount;
	double m_lastCallbackTime;
	void *m_callbackBuffer;
	UInt32 m_maxCallbackFrames;
	bool			m_bRunning;
	bool			m_bFailed;
	int				m_pauseCount;

	unsigned			m_lastDeviceChangeGeneration;
	unsigned			m_lastReportedUnderruns;
	int				m_startThresholdFrames;
	UInt32			m_deviceBufferFrames;
	int				m_mixBudgetFrames;
	int				m_lastMixerFrameEnd;
	int				m_skipUntilTime;
	bool			m_bRecording;
	double			m_lastUnderrunReportTime;
	double			m_flNextRecoverTime;
	bool			m_bDefaultDeviceListenerInstalled;
	bool			m_bDeviceListenersInstalled;

};

CAudioDeviceMacAudioUnit *g_pMacAudioUnit = NULL;

// Listener callbacks may already be queued when a device is removed. Give
// them process-lifetime storage, never a pointer to a destructible backend.
static std::atomic<unsigned> s_deviceChangeGeneration( 0 );

static AudioObjectPropertyAddress MakeAudioObjectAddress( AudioObjectPropertySelector selector, AudioObjectPropertyScope scope )
{
	AudioObjectPropertyAddress address;
	address.mSelector = selector;
	address.mScope = scope;
	address.mElement = kAudioObjectPropertyElementMain;
	return address;
}

static void PreferLowLatencyCoreAudioPowerPolicy()
{
	// kAudioHardwarePropertyPowerHint / kAudioHardwarePowerHintNone are enum
	// constants (macOS 10.9+), so they cannot be feature-tested with the
	// preprocessor - #if defined() on them is always false and would compile
	// this function to a no-op. Gate on the SDK version instead. Per TN2321
	// the power-saving policy can inflate the device I/O buffer from 512 to
	// 4096 frames; the request is defensive and nonfatal.
#if defined( MAC_OS_X_VERSION_10_9 ) && MAC_OS_X_VERSION_MAX_ALLOWED >= 1090
	AudioObjectPropertyAddress address = MakeAudioObjectAddress( kAudioHardwarePropertyPowerHint, kAudioObjectPropertyScopeGlobal );
	UInt32 powerHint = kAudioHardwarePowerHintNone;
	OSStatus status = AudioObjectSetPropertyData( kAudioObjectSystemObject, &address, 0, NULL, sizeof( powerHint ), &powerHint );
	if ( status != noErr )
	{
		DevMsg( "Failed to set macOS low-latency audio power policy %d\n", (int)status );
	}
#endif
}

static OSStatus MacAudioUnitRenderCallback( void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp,
	UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData )
{
	CAudioDeviceMacAudioUnit *pDevice = (CAudioDeviceMacAudioUnit *)inRefCon;
	if ( !pDevice )
		return noErr;

	return pDevice->RenderAudio( ioActionFlags, inNumberFrames, ioData );
}

static OSStatus MacAudioUnitDeviceChangedCallback( AudioObjectID inObjectID, UInt32 inNumberAddresses,
	const AudioObjectPropertyAddress inAddresses[], void *inClientData )
{
	s_deviceChangeGeneration.fetch_add( 1, std::memory_order_relaxed );

	return noErr;
}

IAudioDevice *Audio_CreateMacAudioUnitDevice( void )
{
	g_pMacAudioUnit = new CAudioDeviceMacAudioUnit;
	if ( g_pMacAudioUnit->Init() )
		return g_pMacAudioUnit;

	delete g_pMacAudioUnit;
	g_pMacAudioUnit = NULL;

	return NULL;
}

void OnSndSurroundCvarChanged2( IConVar *pVar, const char *pOldString, float flOldValue );
void OnSndSurroundLegacyChanged2( IConVar *pVar, const char *pOldString, float flOldValue );

bool CAudioDeviceMacAudioUnit::Init( void )
{
	m_AudioUnit = NULL;
	m_OutputDeviceID = kAudioObjectUnknown;
	Q_memset( &m_SourceFormat, 0, sizeof( m_SourceFormat ) );

	m_SndBufSize = AUDIOUNIT_RING_BYTES;
	m_sndBuffers = NULL;
	m_deviceSampleCount = m_SndBufSize / DeviceSampleBytes();
	m_renderedFrames = 0;
	m_writtenFrames = 0;
	m_bRunning = 0;
	m_bFailed = 0;
	m_outputBuffers = NULL;
	m_underrunCount = 0;
	m_callbackCount = 0;
	m_renderError = noErr;
	m_lastCallbackCount = 0;
	m_lastCallbackTime = 0;
	m_callbackBuffer = NULL;
	m_maxCallbackFrames = 0;
	m_pauseCount = 0;
	m_lastDeviceChangeGeneration = s_deviceChangeGeneration.load( std::memory_order_relaxed );
	m_lastReportedUnderruns = 0;
	m_startThresholdFrames = 512;
	m_deviceBufferFrames = 512;
	m_mixBudgetFrames = 128;
	m_lastMixerFrameEnd = 0;
	m_skipUntilTime = 0;
	m_bRecording = false;
	m_lastUnderrunReportTime = 0.0;
	m_flNextRecoverTime = 0.0;
	m_bDefaultDeviceListenerInstalled = false;
	m_bDeviceListenersInstalled = false;

	m_bSurround = false;
	m_bSurroundCenter = false;
	m_bHeadphone = false;

	static bool first = true;
	if ( first )
	{
		snd_surround.SetValue( 2 );
		snd_surround.InstallChangeCallback( &OnSndSurroundCvarChanged2 );
		snd_legacy_surround.InstallChangeCallback( &OnSndSurroundLegacyChanged2 );
		first = false;
	}

	m_sndBuffers = malloc( m_SndBufSize );
	m_outputBuffers = malloc( m_SndBufSize );
	if ( !m_sndBuffers || !m_outputBuffers )
	{
		CloseAudioUnit();
		return false;
	}

	Q_memset( m_sndBuffers, 0, m_SndBufSize );

	if ( !OpenAudioUnit() )
	{
		CloseAudioUnit();
		return false;
	}

	if ( snd_firsttime )
	{
		DevMsg( "Mac AudioUnit sound initialized\n" );
	}

	return ValidAudioUnit() && !m_bFailed;
}

void CAudioDeviceMacAudioUnit::Shutdown( void )
{
	CloseAudioUnit();
}

inline bool CAudioDeviceMacAudioUnit::ValidAudioUnit( void ) const
{
	return m_sndBuffers != NULL && m_outputBuffers != NULL && m_AudioUnit != NULL;
}

void CAudioDeviceMacAudioUnit::FillStreamFormat( AudioStreamBasicDescription *pFormat )
{
	Q_memset( pFormat, 0, sizeof( *pFormat ) );
	pFormat->mSampleRate = SOUND_DMA_SPEED;
	pFormat->mFormatID = kAudioFormatLinearPCM;
	pFormat->mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
	pFormat->mBytesPerPacket = 4;
	pFormat->mFramesPerPacket = 1;
	pFormat->mBytesPerFrame = 4;
	pFormat->mChannelsPerFrame = 2;
	pFormat->mBitsPerChannel = 16;
}

bool CAudioDeviceMacAudioUnit::GetDefaultOutputDevice( AudioDeviceID *pDeviceID ) const
{
	if ( !pDeviceID )
		return false;

	AudioObjectPropertyAddress address = MakeAudioObjectAddress( kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal );
	UInt32 size = sizeof( *pDeviceID );
	AudioDeviceID deviceID = kAudioObjectUnknown;
	OSStatus status = AudioObjectGetPropertyData( kAudioObjectSystemObject, &address, 0, NULL, &size, &deviceID );
	if ( status != noErr || deviceID == kAudioObjectUnknown )
	{
		DevMsg( "Failed to query default output device %d\n", (int)status );
		return false;
	}

	*pDeviceID = deviceID;
	return true;
}

bool CAudioDeviceMacAudioUnit::OpenAudioUnit( void )
{
	if ( m_AudioUnit )
		return true;

	m_bFailed = 0;
	m_bRunning = 0;
	m_renderError.store( noErr, std::memory_order_relaxed );
	m_deviceBufferFrames = 512;
	m_startThresholdFrames = 512;

	PreferLowLatencyCoreAudioPowerPolicy();

	if ( !GetDefaultOutputDevice( &m_OutputDeviceID ) )
	{
		m_bFailed = 1;
		return false;
	}

	AudioComponentDescription desc;
	desc.componentType = kAudioUnitType_Output;
	desc.componentSubType = kAudioUnitSubType_HALOutput;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;
	desc.componentFlags = 0;
	desc.componentFlagsMask = 0;

	AudioComponent component = AudioComponentFindNext( NULL, &desc );
	if ( !component )
	{
		DevMsg( "Failed to find macOS HAL output AudioUnit\n" );
		m_bFailed = 1;
		return false;
	}

	OSStatus status = AudioComponentInstanceNew( component, &m_AudioUnit );
	if ( status != noErr )
	{
		DevMsg( "Failed to create macOS HAL output AudioUnit %d\n", (int)status );
		m_AudioUnit = NULL;
		m_bFailed = 1;
		return false;
	}

	UInt32 one = 1;
	status = AudioUnitSetProperty( m_AudioUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &one, sizeof( one ) );
	if ( status != noErr )
	{
		DevMsg( "Failed to enable macOS AudioUnit output %d\n", (int)status );
		m_bFailed = 1;
		CloseAudioUnit( false );
		return false;
	}

	UInt32 zero = 0;
	AudioUnitSetProperty( m_AudioUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &zero, sizeof( zero ) );

	status = AudioUnitSetProperty( m_AudioUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
		&m_OutputDeviceID, sizeof( m_OutputDeviceID ) );
	if ( status != noErr )
	{
		DevMsg( "Failed to bind macOS AudioUnit to default output device %d\n", (int)status );
		m_bFailed = 1;
		CloseAudioUnit( false );
		return false;
	}

	FillStreamFormat( &m_SourceFormat );
	status = AudioUnitSetProperty( m_AudioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
		&m_SourceFormat, sizeof( m_SourceFormat ) );
	if ( status != noErr )
	{
		DevMsg( "Failed to set macOS AudioUnit stream format %d\n", (int)status );
		m_bFailed = 1;
		CloseAudioUnit( false );
		return false;
	}

	RefreshDeviceTiming( m_OutputDeviceID );

	// Must cover the device's actual I/O buffer: a render slice larger than
	// this fails with kAudioUnitErr_TooManyFramesToProcess and the callback
	// simply never fires - silence with no error path.
	UInt32 maxFramesPerSlice = Max( 4096U, m_deviceBufferFrames );
	status = AudioUnitSetProperty( m_AudioUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
		&maxFramesPerSlice, sizeof( maxFramesPerSlice ) );
	if ( status != noErr )
	{
		DevMsg( "Failed to set macOS AudioUnit max frames per slice %d\n", (int)status );
		m_bFailed = true;
		CloseAudioUnit( false );
		return false;
	}

	AURenderCallbackStruct callbackStruct;
	callbackStruct.inputProc = MacAudioUnitRenderCallback;
	callbackStruct.inputProcRefCon = this;
	status = AudioUnitSetProperty( m_AudioUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
		&callbackStruct, sizeof( callbackStruct ) );
	if ( status != noErr )
	{
		DevMsg( "Failed to set macOS AudioUnit render callback %d\n", (int)status );
		m_bFailed = 1;
		CloseAudioUnit( false );
		return false;
	}

	status = AudioUnitInitialize( m_AudioUnit );
	if ( status != noErr )
	{
		DevMsg( "Failed to initialize macOS AudioUnit %d\n", (int)status );
		m_bFailed = 1;
		CloseAudioUnit( false );
		return false;
	}

	// Query after initialization: the unit can enlarge this for conversion.
	UInt32 size = sizeof( m_maxCallbackFrames );
	status = AudioUnitGetProperty( m_AudioUnit, kAudioUnitProperty_MaximumFramesPerSlice,
		kAudioUnitScope_Global, 0, &m_maxCallbackFrames, &size );
	if ( status != noErr || !m_maxCallbackFrames || m_maxCallbackFrames > (UInt32)( ~0U / 4 ) )
	{
		DevMsg( "Invalid macOS AudioUnit maximum callback size (%d, %u)\n", (int)status, (unsigned)m_maxCallbackFrames );
		m_bFailed = true;
		CloseAudioUnit( false );
		return false;
	}
	m_callbackBuffer = malloc( (size_t)m_maxCallbackFrames * 4 );
	if ( !m_callbackBuffer )
	{
		m_bFailed = true;
		CloseAudioUnit( false );
		return false;
	}

	InstallDeviceListeners( m_OutputDeviceID );
	// Cover a default-device change between the first query and listener
	// installation. Later changes are delivered to the listener.
	AudioDeviceID currentDevice;
	if ( !GetDefaultOutputDevice( &currentDevice ) || currentDevice != m_OutputDeviceID )
		s_deviceChangeGeneration.fetch_add( 1, std::memory_order_relaxed );
	DevMsg( "Using macOS AudioUnit output device %u at %.0f Hz, start threshold %d frames\n",
		(unsigned)m_OutputDeviceID, m_SourceFormat.mSampleRate, m_startThresholdFrames );

	return true;
}

void CAudioDeviceMacAudioUnit::CloseAudioUnit( bool bFreeMixBuffer )
{
	StopAudioUnit();
	RemoveDeviceListeners();

	if ( m_AudioUnit )
	{
		AudioUnitUninitialize( m_AudioUnit );
		AudioComponentInstanceDispose( m_AudioUnit );
		m_AudioUnit = NULL;
	}

	free( m_callbackBuffer );
	m_callbackBuffer = NULL;
	m_maxCallbackFrames = 0;
	m_OutputDeviceID = kAudioObjectUnknown;
	m_bRunning = 0;

	if ( bFreeMixBuffer && m_outputBuffers )
	{
		free( m_outputBuffers );
		m_outputBuffers = NULL;
	}

	if ( bFreeMixBuffer && m_sndBuffers )
	{
		free( m_sndBuffers );
		m_sndBuffers = NULL;
	}
}

bool CAudioDeviceMacAudioUnit::RecoverAudioUnit( const char *pReason, OSStatus nError )
{
	// Rate-limit: recovery re-instantiates the whole AudioUnit and churns
	// property listeners. Every caller retries from a later frame, so
	// declining here only delays the rebuild instead of running it (up to
	// twice per frame) against a persistently failing device.
	double flNow = Plat_FloatTime();
	if ( flNow < m_flNextRecoverTime )
		return false;
	m_flNextRecoverTime = flNow + 1.0;

	if ( nError == noErr )
	{
		DevMsg( "Recovering macOS AudioUnit output after %s\n", pReason ? pReason : "unknown change" );
	}
	else
	{
		DevMsg( "Recovering macOS AudioUnit output after %s %d\n", pReason ? pReason : "unknown error", (int)nError );
	}

	CloseAudioUnit( false );
	bool bRecovered = OpenAudioUnit();

	return bRecovered && ValidAudioUnit() && !m_bFailed;
}

bool CAudioDeviceMacAudioUnit::StartAudioUnit( void )
{
	if ( !ValidAudioUnit() || m_bFailed || m_pauseCount > 0 )
		return false;

	if ( m_bRunning )
		return true;

	OSStatus status = AudioOutputUnitStart( m_AudioUnit );
	if ( status == noErr )
	{
		m_bRunning = 1;
		m_lastCallbackCount = m_callbackCount.load( std::memory_order_relaxed );
		m_lastCallbackTime = Plat_FloatTime();
		return true;
	}

	DevMsg( "Failed to start macOS AudioUnit output %d\n", (int)status );
	m_bFailed = 1;
	RecoverAudioUnit( "AudioOutputUnitStart", status );
	return false;
}

bool CAudioDeviceMacAudioUnit::StopAudioUnit( void )
{
	// Stop even after a failed start: a failed operation may be partial.
	OSStatus status = m_AudioUnit ? AudioOutputUnitStop( m_AudioUnit ) : noErr;
	m_bRunning = false;
	if ( status != noErr )
	{
		DevMsg( "Failed to stop macOS AudioUnit output %d\n", (int)status );
		m_bFailed = true;
		return false;
	}
	return true;
}

void CAudioDeviceMacAudioUnit::InstallDeviceListeners( AudioDeviceID deviceID )
{
	AudioObjectPropertyAddress defaultDeviceAddress = MakeAudioObjectAddress( kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal );
	if ( AudioObjectAddPropertyListener( kAudioObjectSystemObject, &defaultDeviceAddress, MacAudioUnitDeviceChangedCallback, NULL ) == noErr )
	{
		m_bDefaultDeviceListenerInstalled = true;
	}
	else
	{
		DevMsg( "Failed to listen for macOS default output changes\n" );
	}

	AudioObjectPropertyAddress sampleRateAddress = MakeAudioObjectAddress( kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal );
	AudioObjectPropertyAddress bufferSizeAddress = MakeAudioObjectAddress( kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal );
	AudioObjectPropertyAddress aliveAddress = MakeAudioObjectAddress( kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal );

	bool bSampleRate = AudioObjectAddPropertyListener( deviceID, &sampleRateAddress, MacAudioUnitDeviceChangedCallback, NULL ) == noErr;
	bool bBufferSize = AudioObjectAddPropertyListener( deviceID, &bufferSizeAddress, MacAudioUnitDeviceChangedCallback, NULL ) == noErr;
	bool bAlive = AudioObjectAddPropertyListener( deviceID, &aliveAddress, MacAudioUnitDeviceChangedCallback, NULL ) == noErr;
	m_bDeviceListenersInstalled = bSampleRate || bBufferSize || bAlive;
	if ( !m_bDeviceListenersInstalled )
	{
		DevMsg( "Failed to listen for macOS output device property changes\n" );
	}
}

void CAudioDeviceMacAudioUnit::RemoveDeviceListeners( void )
{
	if ( m_bDefaultDeviceListenerInstalled )
	{
		AudioObjectPropertyAddress defaultDeviceAddress = MakeAudioObjectAddress( kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal );
		AudioObjectRemovePropertyListener( kAudioObjectSystemObject, &defaultDeviceAddress, MacAudioUnitDeviceChangedCallback, NULL );
		m_bDefaultDeviceListenerInstalled = false;
	}

	if ( m_bDeviceListenersInstalled && m_OutputDeviceID != kAudioObjectUnknown )
	{
		AudioObjectPropertyAddress sampleRateAddress = MakeAudioObjectAddress( kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal );
		AudioObjectPropertyAddress bufferSizeAddress = MakeAudioObjectAddress( kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal );
		AudioObjectPropertyAddress aliveAddress = MakeAudioObjectAddress( kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal );

		AudioObjectRemovePropertyListener( m_OutputDeviceID, &sampleRateAddress, MacAudioUnitDeviceChangedCallback, NULL );
		AudioObjectRemovePropertyListener( m_OutputDeviceID, &bufferSizeAddress, MacAudioUnitDeviceChangedCallback, NULL );
		AudioObjectRemovePropertyListener( m_OutputDeviceID, &aliveAddress, MacAudioUnitDeviceChangedCallback, NULL );
		m_bDeviceListenersInstalled = false;
	}
}

void CAudioDeviceMacAudioUnit::RefreshDeviceTiming( AudioDeviceID deviceID )
{
	UInt32 bufferFrameSize = 0;
	UInt32 size = sizeof( bufferFrameSize );
	AudioObjectPropertyAddress bufferSizeAddress = MakeAudioObjectAddress( kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal );
	if ( AudioObjectGetPropertyData( deviceID, &bufferSizeAddress, 0, NULL, &size, &bufferFrameSize ) == noErr && bufferFrameSize > 0 )
	{
		m_deviceBufferFrames = bufferFrameSize;
		m_startThresholdFrames = Max( 512U, Min( 2048U, bufferFrameSize ) * 2 );
	}

	Float64 nominalSampleRate = 0.0;
	size = sizeof( nominalSampleRate );
	AudioObjectPropertyAddress sampleRateAddress = MakeAudioObjectAddress( kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal );
	if ( AudioObjectGetPropertyData( deviceID, &sampleRateAddress, 0, NULL, &size, &nominalSampleRate ) == noErr && nominalSampleRate > 0.0 )
	{
		DevMsg( "macOS AudioUnit output hardware nominal rate %.0f Hz, Source mix rate %d Hz\n",
			(double)nominalSampleRate, SOUND_DMA_SPEED );
	}
}

void CAudioDeviceMacAudioUnit::ServiceDeviceChanges( void )
{
	unsigned generation = s_deviceChangeGeneration.load( std::memory_order_relaxed );
	if ( generation != m_lastDeviceChangeGeneration )
	{
		if ( RecoverAudioUnit( "CoreAudio device change" ) )
		{
			// Acknowledge only the generation observed before the rebuild:
			// a change that lands mid-recovery must trigger another pass.
			m_lastDeviceChangeGeneration = generation;
		}
		return;
	}

	OSStatus renderError = m_renderError.load( std::memory_order_relaxed );
	if ( m_bFailed || renderError != noErr )
	{
		RecoverAudioUnit( "failed AudioUnit state", renderError );
		return;
	}
	if ( m_bRunning )
	{
		unsigned callbacks = m_callbackCount.load( std::memory_order_relaxed );
		double now = Plat_FloatTime();
		if ( callbacks != m_lastCallbackCount )
		{
			m_lastCallbackCount = callbacks;
			m_lastCallbackTime = now;
		}
		else if ( now - m_lastCallbackTime > 2.5 )
		{
			RecoverAudioUnit( "stalled render callback" );
		}
	}
}

int CAudioDeviceMacAudioUnit::FramesAvailableForHardware( void )
{
	unsigned written = m_writtenFrames.load( std::memory_order_relaxed );
	unsigned read = m_renderedFrames.load( std::memory_order_acquire );
	return (int)( written - read );
}

int CAudioDeviceMacAudioUnit::StartThresholdFrames( void )
{
	// Use the actual last PaintBegin budget, including extra mixer updates.
	return Min( m_startThresholdFrames, Max( 128, ( m_mixBudgetFrames / 4 ) * 3 ) );
}

void CAudioDeviceMacAudioUnit::CopyFromMixRing( short *pOutput, int startFrame, int frameCount )
{
	if ( !pOutput || !m_outputBuffers || frameCount <= 0 )
		return;

	const int ringFrames = DeviceSampleCount() / DeviceChannels();
	const int frameMask = ringFrames - 1;
	const int frameBytes = DeviceChannels() * DeviceSampleBytes();
	int sourceFrame = startFrame & frameMask;
	int framesRemaining = frameCount;
	char *pRing = (char *)m_outputBuffers;
	char *pDest = (char *)pOutput;

	while ( framesRemaining > 0 )
	{
		int framesThisCopy = Min( framesRemaining, ringFrames - sourceFrame );
		int bytesThisCopy = framesThisCopy * frameBytes;
		Q_memcpy( pDest, pRing + sourceFrame * frameBytes, bytesThisCopy );
		pDest += bytesThisCopy;
		framesRemaining -= framesThisCopy;
		sourceFrame = 0;
	}
}

void CAudioDeviceMacAudioUnit::SilenceOutput( AudioBufferList *ioData, UInt32 inNumberFrames ) const
{
	if ( !ioData )
		return;

	for ( UInt32 i = 0; i < ioData->mNumberBuffers; i++ )
	{
		if ( ioData->mBuffers[i].mData )
		{
			UInt32 bytesToClear = ioData->mBuffers[i].mDataByteSize;

			Q_memset( ioData->mBuffers[i].mData, 0, bytesToClear );
		}
	}
}

OSStatus CAudioDeviceMacAudioUnit::RenderAudio( AudioUnitRenderActionFlags *ioActionFlags, UInt32 inNumberFrames, AudioBufferList *ioData )
{
	if ( inNumberFrames == 0 )
		return noErr;
	// Only this callback writes these counters while the unit is running.
	// Plain atomic load/store avoids an unnecessary read-modify-write (and
	// LL/SC retry loop on CPUs without a native atomic-add instruction).
	m_callbackCount.store( m_callbackCount.load( std::memory_order_relaxed ) + 1, std::memory_order_relaxed );

	// A null data pointer asks the input callback to supply its own storage.
	// This buffer is allocated once, before starting the unit.
	if ( ioData && ioData->mNumberBuffers == 1 && !ioData->mBuffers[0].mData &&
		inNumberFrames <= m_maxCallbackFrames )
	{
		ioData->mBuffers[0].mData = m_callbackBuffer;
		ioData->mBuffers[0].mDataByteSize = inNumberFrames * 4;
	}

	// We negotiated one interleaved stereo buffer. Never increase a caller's
	// capacity before checking it, or invent capacity for a zero-size buffer.
	if ( !ioData || ioData->mNumberBuffers != 1 ||
		ioData->mBuffers[0].mNumberChannels != 2 || !ioData->mBuffers[0].mData ||
		inNumberFrames > ioData->mBuffers[0].mDataByteSize / 4 )
	{
		SilenceOutput( ioData, inNumberFrames );
		m_renderError.store( kAudio_ParamError, std::memory_order_relaxed );
		return kAudio_ParamError;
	}

	AudioBuffer *pBuffer = &ioData->mBuffers[0];
	unsigned read = m_renderedFrames.load( std::memory_order_relaxed );
	unsigned written = m_writtenFrames.load( std::memory_order_acquire );
	unsigned available = written - read;
	unsigned copied = Min( inNumberFrames, available );
	short *pOutput = (short *)pBuffer->mData;
	if ( copied )
		CopyFromMixRing( pOutput, read & ( AUDIOUNIT_RING_FRAMES - 1 ), copied );
	if ( copied < inNumberFrames )
	{
		Q_memset( pOutput + copied * 2, 0, ( inNumberFrames - copied ) * 4 );
		m_underrunCount.store( m_underrunCount.load( std::memory_order_relaxed ) + 1, std::memory_order_relaxed );
	}
	pBuffer->mDataByteSize = inNumberFrames * 4;
	if ( ioActionFlags )
	{
		if ( !copied )
			*ioActionFlags |= kAudioUnitRenderAction_OutputIsSilence;
		else
			*ioActionFlags &= ~kAudioUnitRenderAction_OutputIsSilence;
	}

	// Publish only after the copy, so the mixer cannot reuse unread slots.
	// Silence does not consume PCM or move Source's clock. In particular, a
	// long mixer stall cannot lap the ring or require a backward clock reset.
	if ( copied )
		m_renderedFrames.store( read + copied, std::memory_order_release );
	return noErr;
}

int CAudioDeviceMacAudioUnit::PaintBegin( float mixAheadTime, int soundtime, int paintedtime )
{
	ServiceDeviceChanges();

	bool recording = cl_movieinfo.IsRecording() || IsReplayRendering();
	bool wasRecording = m_bRecording;
	m_bRecording = recording;
	if ( wasRecording && !recording )
	{
		// Offline movie time is unrelated to the hardware clock. Resume live
		// mixing at the clock sampled by GetSoundTime, not the movie endpoint.
		g_paintedtime = paintedtime = soundtime;
	}
	if ( paintedtime != m_lastMixerFrameEnd || recording != wasRecording )
	{
		if ( !StopAudioUnit() )
			CloseAudioUnit( false );
		// GetSoundTime already sampled this position. Re-anchor the empty
		// FIFO there without exposing a new modulo position to that caller.
		m_renderedFrames.store( (unsigned)soundtime, std::memory_order_relaxed );
		m_writtenFrames.store( (unsigned)soundtime, std::memory_order_relaxed );
		m_lastMixerFrameEnd = paintedtime;
		m_skipUntilTime = soundtime;
	}

	// Clamp before float-to-int conversion (also handles NaN and infinity).
	if ( !( mixAheadTime > 0.0f ) )
		m_mixBudgetFrames = 128;
	else if ( mixAheadTime >= (float)AUDIOUNIT_RING_FRAMES / DeviceDmaSpeed() )
		m_mixBudgetFrames = AUDIOUNIT_RING_FRAMES - 4;
	else
		m_mixBudgetFrames = Min( AUDIOUNIT_RING_FRAMES - 4, Max( 128, (int)( mixAheadTime * DeviceDmaSpeed() ) ) );
	// Leave a four-frame gap: an entire lap drained between GetSoundTime
	// calls is indistinguishable from no progress to Source's modulo clock.
	unsigned int endtime = (unsigned)soundtime + m_mixBudgetFrames;

	if ( ( endtime - paintedtime ) & 0x3 )
	{
		endtime -= ( endtime - paintedtime ) & 0x3;
	}

	return endtime;
}

void CAudioDeviceMacAudioUnit::PaintEnd( void )
{
	ServiceDeviceChanges();

	unsigned underruns = m_underrunCount.load( std::memory_order_relaxed );
	double now = Plat_FloatTime();
	if ( underruns != m_lastReportedUnderruns && now - m_lastUnderrunReportTime > 1.0 )
	{
		DevMsg( "macOS AudioUnit underruns: %u\n", underruns );
		m_lastReportedUnderruns = underruns;
		m_lastUnderrunReportTime = now;
	}

	if ( IsActive() && !m_bRunning && FramesAvailableForHardware() >= StartThresholdFrames() )
	{
		StartAudioUnit();
	}
}

int CAudioDeviceMacAudioUnit::GetOutputPosition( void )
{
	int samplePairCount = DeviceSampleCount() / DeviceChannels();
	return m_renderedFrames.load( std::memory_order_acquire ) & ( samplePairCount - 1 );
}

void CAudioDeviceMacAudioUnit::Pause( void )
{
	m_pauseCount++;
	if ( m_pauseCount == 1 )
	{
		if ( !StopAudioUnit() )
			CloseAudioUnit( false );
	}
}

void CAudioDeviceMacAudioUnit::UnPause( void )
{
	if ( m_pauseCount > 0 )
	{
		m_pauseCount--;
	}

	if ( m_pauseCount == 0 && FramesAvailableForHardware() >= StartThresholdFrames() )
	{
		StartAudioUnit();
	}
}

bool CAudioDeviceMacAudioUnit::IsActive( void )
{
	return ( m_pauseCount == 0 );
}

float CAudioDeviceMacAudioUnit::MixDryVolume( void )
{
	return 0;
}

bool CAudioDeviceMacAudioUnit::Should3DMix( void )
{
	return false;
}

void CAudioDeviceMacAudioUnit::DiscardQueuedFrames( void )
{
	// Caller stopped/disposed the unit; it is now the only reader/writer.
	if ( g_paintedtime == m_lastMixerFrameEnd )
		m_renderedFrames.store( m_writtenFrames.load( std::memory_order_relaxed ), std::memory_order_release );
	else
		// Source rebased its timeline; PaintBegin will re-anchor to the
		// sampled soundtime. Discard without adding the old prefill to it.
		m_writtenFrames.store( m_renderedFrames.load( std::memory_order_relaxed ), std::memory_order_release );
}

void CAudioDeviceMacAudioUnit::ClearBuffer( void )
{
	if ( !StopAudioUnit() )
		CloseAudioUnit( false );
	DiscardQueuedFrames();
	if ( m_sndBuffers )
		Q_memset( m_sndBuffers, 0, m_SndBufSize );
}

void CAudioDeviceMacAudioUnit::UpdateListener( const Vector& position, const Vector& forward, const Vector& right, const Vector& up )
{
}

void CAudioDeviceMacAudioUnit::MixBegin( int sampleCount )
{
	MIX_ClearAllPaintBuffers( sampleCount, false );
}

void CAudioDeviceMacAudioUnit::MixUpsample( int sampleCount, int filtertype )
{
	paintbuffer_t *ppaint = MIX_GetCurrentPaintbufferPtr();
	int ifilter = ppaint->ifilter;

	Assert( ifilter < CPAINTFILTERS );

	S_MixBufferUpsample2x( sampleCount, ppaint->pbuf, &(ppaint->fltmem[ifilter][0]), CPAINTFILTERMEM, filtertype );

	ppaint->ifilter++;
}

void CAudioDeviceMacAudioUnit::Mix8Mono( channel_t *pChannel, char *pData, int outputOffset, int inputOffset, fixedint rateScaleFix, int outCount, int timecompress )
{
	int volume[CCHANVOLUMES];
	paintbuffer_t *ppaint = MIX_GetCurrentPaintbufferPtr();

	if ( !MIX_ScaleChannelVolume( ppaint, pChannel, volume, 1 ) )
		return;

	Mix8MonoWavtype( pChannel, ppaint->pbuf + outputOffset, volume, (byte *)pData, inputOffset, rateScaleFix, outCount );
}

void CAudioDeviceMacAudioUnit::Mix8Stereo( channel_t *pChannel, char *pData, int outputOffset, int inputOffset, fixedint rateScaleFix, int outCount, int timecompress )
{
	int volume[CCHANVOLUMES];
	paintbuffer_t *ppaint = MIX_GetCurrentPaintbufferPtr();

	if ( !MIX_ScaleChannelVolume( ppaint, pChannel, volume, 2 ) )
		return;

	Mix8StereoWavtype( pChannel, ppaint->pbuf + outputOffset, volume, (byte *)pData, inputOffset, rateScaleFix, outCount );
}

void CAudioDeviceMacAudioUnit::Mix16Mono( channel_t *pChannel, short *pData, int outputOffset, int inputOffset, fixedint rateScaleFix, int outCount, int timecompress )
{
	int volume[CCHANVOLUMES];
	paintbuffer_t *ppaint = MIX_GetCurrentPaintbufferPtr();

	if ( !MIX_ScaleChannelVolume( ppaint, pChannel, volume, 1 ) )
		return;

	Mix16MonoWavtype( pChannel, ppaint->pbuf + outputOffset, volume, pData, inputOffset, rateScaleFix, outCount );
}

void CAudioDeviceMacAudioUnit::Mix16Stereo( channel_t *pChannel, short *pData, int outputOffset, int inputOffset, fixedint rateScaleFix, int outCount, int timecompress )
{
	int volume[CCHANVOLUMES];
	paintbuffer_t *ppaint = MIX_GetCurrentPaintbufferPtr();

	if ( !MIX_ScaleChannelVolume( ppaint, pChannel, volume, 2 ) )
		return;

	Mix16StereoWavtype( pChannel, ppaint->pbuf + outputOffset, volume, pData, inputOffset, rateScaleFix, outCount );
}

void CAudioDeviceMacAudioUnit::ChannelReset( int entnum, int channelIndex, float distanceMod )
{
}

void CAudioDeviceMacAudioUnit::TransferSamples( int end )
{
	if ( !m_sndBuffers || !m_outputBuffers || end <= g_paintedtime )
		return;

	// Keep the original transfer (including movie recording) on the mixer
	// thread. This scratch ring is never accessed by the render callback.
	S_TransferStereo16( m_sndBuffers, PAINTBUFFER, g_paintedtime, end );
	m_lastMixerFrameEnd = end;
	// Offline recording has its own clock and intentionally leaves device
	// PCM untouched. Do not enqueue old scratch bytes or block recording on
	// the real-time consumer. PaintBegin re-anchors when recording ends.
	if ( cl_movieinfo.IsRecording() || IsReplayRendering() )
		return;
	unsigned skip = g_paintedtime < m_skipUntilTime ? Min( end, m_skipUntilTime ) - g_paintedtime : 0;

	unsigned written = m_writtenFrames.load( std::memory_order_relaxed );
	unsigned read = m_renderedFrames.load( std::memory_order_acquire );
	unsigned freeFrames = AUDIOUNIT_RING_FRAMES - ( written - read );
	unsigned remaining = Min( (unsigned)( end - g_paintedtime ) - skip, freeFrames );
	unsigned source = (unsigned)g_paintedtime + skip;
	while ( remaining )
	{
		unsigned src = source & ( AUDIOUNIT_RING_FRAMES - 1 );
		unsigned dst = written & ( AUDIOUNIT_RING_FRAMES - 1 );
		unsigned count = Min( remaining, Min( AUDIOUNIT_RING_FRAMES - src, AUDIOUNIT_RING_FRAMES - dst ) );
		Q_memcpy( (short *)m_outputBuffers + dst * 2, (short *)m_sndBuffers + src * 2, count * 4 );
		source += count;
		written += count;
		remaining -= count;
	}
	m_writtenFrames.store( written, std::memory_order_release );
}

void CAudioDeviceMacAudioUnit::SpatializeChannel( int volume[CCHANVOLUMES/2], int master_vol, const Vector& sourceDir, float gain, float mono )
{
	VPROF( "CAudioDeviceMacAudioUnit::SpatializeChannel" );
	S_SpatializeChannel( volume, master_vol, &sourceDir, gain, mono );
}

void CAudioDeviceMacAudioUnit::StopAllSounds( void )
{
	if ( !StopAudioUnit() )
		CloseAudioUnit( false );
	DiscardQueuedFrames();
}

void CAudioDeviceMacAudioUnit::ApplyDSPEffects( int idsp, portable_samplepair_t *pbuffront, portable_samplepair_t *pbufrear, portable_samplepair_t *pbufcenter, int samplecount )
{
	DSP_Process( idsp, pbuffront, pbufrear, pbufcenter, samplecount );
}
