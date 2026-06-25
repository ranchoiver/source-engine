//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: macOS Audio Unit output device
//
//===========================================================================//

#include "audio_pch.h"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>

#ifndef kAudioObjectPropertyElementMain
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
	void		MarkDeviceChanged( void ) { m_deviceChangeGeneration++; }

private:
	bool		OpenAudioUnit( void );
	void		CloseAudioUnit( bool bFreeMixBuffer = true );
	bool		RecoverAudioUnit( const char *pReason, OSStatus nError = noErr );
	bool		ValidAudioUnit( void ) const;
	bool		StartAudioUnit( void );
	void		StopAudioUnit( void );
	void		ServiceDeviceChanges( void );
	void		FillStreamFormat( AudioStreamBasicDescription *pFormat );
	bool		GetDefaultOutputDevice( AudioDeviceID *pDeviceID ) const;
	void		InstallDeviceListeners( AudioDeviceID deviceID );
	void		RemoveDeviceListeners( void );
	void		RefreshDeviceTiming( AudioDeviceID deviceID );
	int			FramesAvailableForHardware( void );
	void		CopyFromMixRing( short *pOutput, int startFrame, int frameCount );
	void		SilenceOutput( AudioBufferList *ioData, UInt32 inNumberFrames ) const;

	AudioUnit						m_AudioUnit;
	AudioDeviceID					m_OutputDeviceID;
	AudioStreamBasicDescription		m_SourceFormat;

	int				m_SndBufSize;
	void			*m_sndBuffers;
	CInterlockedInt	m_deviceSampleCount;

	CInterlockedInt	m_renderedFrames;
	CInterlockedInt	m_bRunning;
	CInterlockedInt	m_bFailed;
	CInterlockedInt	m_deviceChangeGeneration;
	CInterlockedInt	m_underrunCount;
	CInterlockedInt	m_pauseCount;

	int				m_lastDeviceChangeGeneration;
	int				m_lastReportedUnderruns;
	int				m_startThresholdFrames;
	double			m_lastUnderrunReportTime;
	bool			m_bDefaultDeviceListenerInstalled;
	bool			m_bDeviceListenersInstalled;
	bool			m_bSoundsShutdown;
};

CAudioDeviceMacAudioUnit *g_pMacAudioUnit = NULL;

static AudioObjectPropertyAddress MakeAudioObjectAddress( AudioObjectPropertySelector selector, AudioObjectPropertyScope scope )
{
	AudioObjectPropertyAddress address;
	address.mSelector = selector;
	address.mScope = scope;
	address.mElement = kAudioObjectPropertyElementMain;
	return address;
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
	CAudioDeviceMacAudioUnit *pDevice = (CAudioDeviceMacAudioUnit *)inClientData;
	if ( pDevice )
		pDevice->MarkDeviceChanged();

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
	m_bRunning = 0;
	m_bFailed = 0;
	m_deviceChangeGeneration = 0;
	m_underrunCount = 0;
	m_pauseCount = 0;
	m_lastDeviceChangeGeneration = 0;
	m_lastReportedUnderruns = 0;
	m_startThresholdFrames = 512;
	m_lastUnderrunReportTime = 0.0;
	m_bDefaultDeviceListenerInstalled = false;
	m_bDeviceListenersInstalled = false;
	m_bSoundsShutdown = false;

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
	if ( !m_sndBuffers )
		return false;

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
	return m_sndBuffers != NULL && m_AudioUnit != NULL;
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

	UInt32 maxFramesPerSlice = Max( 512, m_startThresholdFrames );
	status = AudioUnitSetProperty( m_AudioUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
		&maxFramesPerSlice, sizeof( maxFramesPerSlice ) );
	if ( status != noErr )
	{
		DevMsg( "Failed to set macOS AudioUnit max frames per slice %d\n", (int)status );
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

	InstallDeviceListeners( m_OutputDeviceID );
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

	m_OutputDeviceID = kAudioObjectUnknown;
	m_bRunning = 0;

	if ( bFreeMixBuffer && m_sndBuffers )
	{
		free( m_sndBuffers );
		m_sndBuffers = NULL;
	}
}

bool CAudioDeviceMacAudioUnit::RecoverAudioUnit( const char *pReason, OSStatus nError )
{
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
	m_lastDeviceChangeGeneration = m_deviceChangeGeneration;

	return bRecovered && ValidAudioUnit() && !m_bFailed;
}

bool CAudioDeviceMacAudioUnit::StartAudioUnit( void )
{
	if ( !ValidAudioUnit() )
		return false;

	if ( m_bRunning )
		return true;

	OSStatus status = AudioOutputUnitStart( m_AudioUnit );
	if ( status == noErr )
	{
		m_bRunning = 1;
		return true;
	}

	DevMsg( "Failed to start macOS AudioUnit output %d\n", (int)status );
	m_bFailed = 1;
	RecoverAudioUnit( "AudioOutputUnitStart", status );
	return false;
}

void CAudioDeviceMacAudioUnit::StopAudioUnit( void )
{
	if ( m_AudioUnit && m_bRunning )
	{
		AudioOutputUnitStop( m_AudioUnit );
	}
	m_bRunning = 0;
}

void CAudioDeviceMacAudioUnit::InstallDeviceListeners( AudioDeviceID deviceID )
{
	AudioObjectPropertyAddress defaultDeviceAddress = MakeAudioObjectAddress( kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal );
	if ( AudioObjectAddPropertyListener( kAudioObjectSystemObject, &defaultDeviceAddress, MacAudioUnitDeviceChangedCallback, this ) == noErr )
	{
		m_bDefaultDeviceListenerInstalled = true;
	}
	else
	{
		DevMsg( "Failed to listen for macOS default output changes\n" );
	}

	AudioObjectPropertyAddress sampleRateAddress = MakeAudioObjectAddress( kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal );
	AudioObjectPropertyAddress bufferSizeAddress = MakeAudioObjectAddress( kAudioDevicePropertyBufferFrameSize, kAudioDevicePropertyScopeGlobal );
	AudioObjectPropertyAddress aliveAddress = MakeAudioObjectAddress( kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal );

	bool bSampleRate = AudioObjectAddPropertyListener( deviceID, &sampleRateAddress, MacAudioUnitDeviceChangedCallback, this ) == noErr;
	bool bBufferSize = AudioObjectAddPropertyListener( deviceID, &bufferSizeAddress, MacAudioUnitDeviceChangedCallback, this ) == noErr;
	bool bAlive = AudioObjectAddPropertyListener( deviceID, &aliveAddress, MacAudioUnitDeviceChangedCallback, this ) == noErr;
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
		AudioObjectRemovePropertyListener( kAudioObjectSystemObject, &defaultDeviceAddress, MacAudioUnitDeviceChangedCallback, this );
		m_bDefaultDeviceListenerInstalled = false;
	}

	if ( m_bDeviceListenersInstalled && m_OutputDeviceID != kAudioObjectUnknown )
	{
		AudioObjectPropertyAddress sampleRateAddress = MakeAudioObjectAddress( kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal );
		AudioObjectPropertyAddress bufferSizeAddress = MakeAudioObjectAddress( kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal );
		AudioObjectPropertyAddress aliveAddress = MakeAudioObjectAddress( kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal );

		AudioObjectRemovePropertyListener( m_OutputDeviceID, &sampleRateAddress, MacAudioUnitDeviceChangedCallback, this );
		AudioObjectRemovePropertyListener( m_OutputDeviceID, &bufferSizeAddress, MacAudioUnitDeviceChangedCallback, this );
		AudioObjectRemovePropertyListener( m_OutputDeviceID, &aliveAddress, MacAudioUnitDeviceChangedCallback, this );
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
		m_startThresholdFrames = Max( 512, Min( 4096, (int)bufferFrameSize * 2 ) );
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
	int generation = m_deviceChangeGeneration;
	if ( generation != m_lastDeviceChangeGeneration )
	{
		RecoverAudioUnit( "CoreAudio device change" );
		return;
	}

	if ( m_bFailed )
	{
		RecoverAudioUnit( "failed AudioUnit state" );
	}
}

int CAudioDeviceMacAudioUnit::FramesAvailableForHardware( void )
{
	int available = g_paintedtime - (int)m_renderedFrames;
	if ( available < 0 )
		return 0;

	int ringFrames = DeviceSampleCount() / DeviceChannels();
	return Min( available, ringFrames );
}

void CAudioDeviceMacAudioUnit::CopyFromMixRing( short *pOutput, int startFrame, int frameCount )
{
	if ( !pOutput || !m_sndBuffers || frameCount <= 0 )
		return;

	const int ringFrames = DeviceSampleCount() / DeviceChannels();
	const int frameMask = ringFrames - 1;
	const int frameBytes = DeviceChannels() * DeviceSampleBytes();
	int sourceFrame = startFrame & frameMask;
	int framesRemaining = frameCount;
	char *pRing = (char *)m_sndBuffers;
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
			if ( bytesToClear == 0 )
			{
				bytesToClear = inNumberFrames * ioData->mBuffers[i].mNumberChannels * sizeof( short );
				ioData->mBuffers[i].mDataByteSize = bytesToClear;
			}
			Q_memset( ioData->mBuffers[i].mData, 0, bytesToClear );
		}
	}
}

OSStatus CAudioDeviceMacAudioUnit::RenderAudio( AudioUnitRenderActionFlags *ioActionFlags, UInt32 inNumberFrames, AudioBufferList *ioData )
{
	if ( !ioData || !m_sndBuffers || m_pauseCount > 0 || inNumberFrames == 0 )
	{
		SilenceOutput( ioData, inNumberFrames );
		return noErr;
	}

	if ( ioData->mNumberBuffers != 1 || ioData->mBuffers[0].mNumberChannels != 2 )
	{
		SilenceOutput( ioData, inNumberFrames );
		m_underrunCount++;
		m_renderedFrames += inNumberFrames;
		return noErr;
	}

	AudioBuffer *pBuffer = &ioData->mBuffers[0];
	const int requestedFrames = (int)inNumberFrames;
	const int frameBytes = DeviceChannels() * DeviceSampleBytes();
	pBuffer->mDataByteSize = requestedFrames * frameBytes;
	if ( !pBuffer->mData )
	{
		m_underrunCount++;
		m_renderedFrames += requestedFrames;
		return noErr;
	}

	int startFrame = (int)m_renderedFrames;
	int availableFrames = g_paintedtime - startFrame;
	if ( availableFrames < 0 || availableFrames > ( DeviceSampleCount() / DeviceChannels() ) )
	{
		startFrame = g_paintedtime;
		availableFrames = 0;
		m_renderedFrames = startFrame;
	}

	int framesToCopy = Min( requestedFrames, availableFrames );
	short *pOutput = (short *)pBuffer->mData;
	if ( framesToCopy > 0 )
	{
		CopyFromMixRing( pOutput, startFrame, framesToCopy );
	}

	if ( framesToCopy < requestedFrames )
	{
		Q_memset( pOutput + framesToCopy * DeviceChannels(), 0, ( requestedFrames - framesToCopy ) * frameBytes );
		m_underrunCount++;
	}

	m_renderedFrames += requestedFrames;
	return noErr;
}

int CAudioDeviceMacAudioUnit::PaintBegin( float mixAheadTime, int soundtime, int paintedtime )
{
	ServiceDeviceChanges();

	unsigned int endtime = soundtime + mixAheadTime * DeviceDmaSpeed();
	int samps = DeviceSampleCount() >> (DeviceChannels()-1);

	if ( (int)( endtime - soundtime ) > samps )
		endtime = soundtime + samps;

	if ( ( endtime - paintedtime ) & 0x3 )
	{
		endtime -= ( endtime - paintedtime ) & 0x3;
	}

	return endtime;
}

void CAudioDeviceMacAudioUnit::PaintEnd( void )
{
	ServiceDeviceChanges();

	int underruns = m_underrunCount;
	double now = Plat_FloatTime();
	if ( underruns != m_lastReportedUnderruns && now - m_lastUnderrunReportTime > 1.0 )
	{
		DevMsg( "macOS AudioUnit underruns: %d\n", underruns );
		m_lastReportedUnderruns = underruns;
		m_lastUnderrunReportTime = now;
	}

	if ( IsActive() && !m_bRunning && FramesAvailableForHardware() >= m_startThresholdFrames )
	{
		StartAudioUnit();
	}
}

int CAudioDeviceMacAudioUnit::GetOutputPosition( void )
{
	int samplePairCount = DeviceSampleCount() / DeviceChannels();
	return (int)m_renderedFrames & ( samplePairCount - 1 );
}

void CAudioDeviceMacAudioUnit::Pause( void )
{
	m_pauseCount++;
	if ( m_pauseCount == 1 )
	{
		StopAudioUnit();
	}
}

void CAudioDeviceMacAudioUnit::UnPause( void )
{
	if ( m_pauseCount > 0 )
	{
		m_pauseCount--;
	}

	if ( m_pauseCount == 0 && FramesAvailableForHardware() >= m_startThresholdFrames )
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

void CAudioDeviceMacAudioUnit::ClearBuffer( void )
{
	if ( !m_sndBuffers )
		return;

	Q_memset( m_sndBuffers, 0, DeviceSampleCount() * DeviceSampleBytes() );
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
	int lpaintedtime = g_paintedtime;

	if ( m_sndBuffers )
	{
		S_TransferStereo16( m_sndBuffers, PAINTBUFFER, lpaintedtime, end );
	}
}

void CAudioDeviceMacAudioUnit::SpatializeChannel( int volume[CCHANVOLUMES/2], int master_vol, const Vector& sourceDir, float gain, float mono )
{
	VPROF( "CAudioDeviceMacAudioUnit::SpatializeChannel" );
	S_SpatializeChannel( volume, master_vol, &sourceDir, gain, mono );
}

void CAudioDeviceMacAudioUnit::StopAllSounds( void )
{
	m_bSoundsShutdown = true;
	StopAudioUnit();
	m_renderedFrames = g_paintedtime;
}

void CAudioDeviceMacAudioUnit::ApplyDSPEffects( int idsp, portable_samplepair_t *pbuffront, portable_samplepair_t *pbufrear, portable_samplepair_t *pbufcenter, int samplecount )
{
	DSP_Process( idsp, pbuffront, pbufrear, pbufcenter, samplecount );
}
