//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//===========================================================================//

#include "audio_pch.h"
#include <AudioToolbox/AudioQueue.h>
#include <AudioToolbox/AudioFile.h>
#include <AudioToolbox/AudioFormat.h>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

extern bool snd_firsttime;
extern int g_soundtime;
extern bool MIX_ScaleChannelVolume( paintbuffer_t *ppaint, channel_t *pChannel, int volume[CCHANVOLUMES], int mixchans );
extern void S_SpatializeChannel( int volume[6], int master_vol, const Vector *psourceDir, float gain, float mono );

#define NUM_BUFFERS_SOURCES		128
#define	BUFF_MASK				(NUM_BUFFERS_SOURCES - 1 )
#define	BUFFER_SIZE			0x0400

// Recovery tears down and rebuilds the entire queue, so it must never run
// more often than the output route can plausibly come back.
#define AQ_RECOVER_COOLDOWN_SEC			1.0
// Bluetooth route establishment (AirPods profile switches) can take a couple
// of seconds during which a started queue completes nothing; start tolerant
// and back off further while recoveries stay unproductive so we don't tear
// the route down while it is still coming up.
#define AQ_STALL_TOLERANCE_BASE_SEC		2.5
#define AQ_STALL_TOLERANCE_MAX_SEC		20.0


//-----------------------------------------------------------------------------
//
// NOTE: This only allows 16-bit, stereo wave out
//
//-----------------------------------------------------------------------------
class CAudioDeviceAudioQueue : public CAudioDeviceBase
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
	void		SpatializeChannel( int volume[CCHANVOLUMES/2], int master_vol, const Vector& sourceDir, float gain, float mono);
	void		ApplyDSPEffects( int idsp, portable_samplepair_t *pbuffront, portable_samplepair_t *pbufrear, portable_samplepair_t *pbufcenter, int samplecount );

	const char *DeviceName( void )			{ return "AudioQueue"; }
	int			DeviceChannels( void )		{ return 2; }
	int			DeviceSampleBits( void )	{ return 16; }
	int			DeviceSampleBytes( void )	{ return 2; }
	int			DeviceDmaSpeed( void )		{ return SOUND_DMA_SPEED; }
	int			DeviceSampleCount( void )	{ return m_deviceSampleCount; }

	void BufferCompleted() { m_buffersCompleted++; }
	void SetRunning( bool bState ) { m_bRunning = bState ? 1 : 0; }
	void MarkQueueDeviceChanged() { m_bQueueDeviceChanged = 1; }
	
private:
	void	OpenWaveOut( void );
	void	CloseWaveOut( bool bFreeMixBuffer = true );
	bool	RecoverWaveOut( const char *pReason, OSStatus nError = noErr );
	bool	ValidWaveOut( void ) const;
	bool	BIsPlaying();
	bool	IsQueueRunning( void ) const;
	int		QueuedBufferCount( void ) const;
	int		PaintedAheadFrames( void );
	void	ResetQueuedBufferStateToPlayback( void );

	AudioStreamBasicDescription m_DataFormat;
	AudioQueueRef               m_Queue;
	AudioQueueBufferRef         m_Buffers[NUM_BUFFERS_SOURCES];
	
	int		m_SndBufSize;
	
	void *m_sndBuffers;
	
	CInterlockedInt	m_deviceSampleCount;

	int			m_buffersSent;
	CInterlockedInt	m_buffersCompleted;
	int			m_pauseCount;
	bool		m_bSoundsShutdown;
	
	bool m_bFailed;
	CInterlockedInt m_bRunning;
	CInterlockedInt m_bQueueDeviceChanged;
	int m_lastObservedCompleted;
	int m_completedAtLastClockQuery;
	double m_lastProgressTime;
	double m_flNextRecoverTime;
	double m_flStallToleranceSec;
	
	
};

CAudioDeviceAudioQueue *wave = NULL;


static void AudioCallback(void *pContext, AudioQueueRef pQueue, AudioQueueBufferRef pBuffer)
{
	CAudioDeviceAudioQueue *pAudioQueue = (CAudioDeviceAudioQueue *)pContext;
	if ( pAudioQueue )
		pAudioQueue->BufferCompleted();
}


IAudioDevice *Audio_CreateMacAudioQueueDevice( void )
{
	wave = new CAudioDeviceAudioQueue;
	if ( wave->Init() )
		return wave;
	
	delete wave;
	wave = NULL;
	
	return NULL;
}


void OnSndSurroundCvarChanged2( IConVar *pVar, const char *pOldString, float flOldValue );
void OnSndSurroundLegacyChanged2( IConVar *pVar, const char *pOldString, float flOldValue );

//-----------------------------------------------------------------------------
// Init, shutdown
//-----------------------------------------------------------------------------
bool CAudioDeviceAudioQueue::Init( void )
{
	m_SndBufSize = 0;
	m_sndBuffers = NULL;
	m_pauseCount = 0;

	m_bSurround = false;
	m_bSurroundCenter = false;
	m_bHeadphone = false;
	m_buffersSent = 0;
	m_buffersCompleted = 0;
	m_lastObservedCompleted = 0;
	m_completedAtLastClockQuery = 0;
	m_lastProgressTime = Plat_FloatTime();
	m_flNextRecoverTime = 0.0;
	m_flStallToleranceSec = AQ_STALL_TOLERANCE_BASE_SEC;
	m_pauseCount = 0;
	m_bSoundsShutdown = false;
	m_bFailed = false;
	m_bRunning = 0;
	m_bQueueDeviceChanged = 0;
	
	m_Queue = NULL;
	Q_memset( m_Buffers, 0, sizeof( m_Buffers ) );
	
	static bool first = true;
	if ( first )
	{
		snd_surround.SetValue( 2 );
		snd_surround.InstallChangeCallback( &OnSndSurroundCvarChanged2 );
		snd_legacy_surround.InstallChangeCallback( &OnSndSurroundLegacyChanged2 );
		first = false;
	}
	
	OpenWaveOut();

	if ( snd_firsttime )
	{
		DevMsg( "Wave sound initialized\n" );
	}
	return ValidWaveOut() && !m_bFailed;
}

void CAudioDeviceAudioQueue::Shutdown( void )
{
	CloseWaveOut();
}


//-----------------------------------------------------------------------------
// WAV out device
//-----------------------------------------------------------------------------
inline bool CAudioDeviceAudioQueue::ValidWaveOut( void ) const 
{ 
	return m_sndBuffers != 0 && m_Queue; 
}

inline bool CAudioDeviceAudioQueue::IsQueueRunning( void ) const
{
	return ( m_bRunning != 0 );
}

int CAudioDeviceAudioQueue::QueuedBufferCount( void ) const
{
	int cQueued = m_buffersSent - (int)m_buffersCompleted;
	return ( cQueued > 0 ) ? cQueued : 0;
}

void CAudioDeviceAudioQueue::ResetQueuedBufferStateToPlayback( void )
{
	// Re-sync the submit cursor with the completion cursor. Note that
	// AudioQueueStop( ..., true ) flushes unplayed buffers by firing their
	// completion callbacks, so up to the in-flight ~93ms is skipped rather
	// than replayed; what matters is that the completed-buffer clock stays
	// monotonic, which is what GetSoundTime() requires.
	m_buffersSent = (int)m_buffersCompleted;
}

int CAudioDeviceAudioQueue::PaintedAheadFrames( void )
{
	// Frames of valid mixed audio ahead of the live hardware clock.
	// g_soundtime was derived from m_completedAtLastClockQuery (see
	// GetOutputPosition), so subtracting what the hardware consumed since
	// that clock sample re-bases (g_paintedtime - g_soundtime) onto
	// m_buffersCompleted. Pure deltas only: this survives the engine's
	// g_paintedtime rebases (32-bit chop, movie recording).
	const int nFramesPerBuffer = BUFFER_SIZE / ( DeviceSampleBytes() * DeviceChannels() );
	return ( g_paintedtime - g_soundtime ) -
		( (int)m_buffersCompleted - m_completedAtLastClockQuery ) * nFramesPerBuffer;
}


//-----------------------------------------------------------------------------
// called by the mac audioqueue code when we run out of playback buffers
//-----------------------------------------------------------------------------
void AudioQueueIsRunningCallback( void* inClientData, AudioQueueRef inAQ, AudioQueuePropertyID inID)
{
    CAudioDeviceAudioQueue* audioqueue = (CAudioDeviceAudioQueue*)inClientData;
	if ( !audioqueue )
		return;
	
	UInt32 running = 0;
	UInt32 size = sizeof( running );
	OSStatus err = AudioQueueGetProperty(inAQ, kAudioQueueProperty_IsRunning, &running, &size);
	if ( err == noErr )
	{
		audioqueue->SetRunning( running != 0 );
	}
	//DevWarning( "AudioQueueStart %d\n", running );
}

void AudioQueueCurrentDeviceChangedCallback( void* inClientData, AudioQueueRef inAQ, AudioQueuePropertyID inID)
{
	CAudioDeviceAudioQueue* audioqueue = (CAudioDeviceAudioQueue*)inClientData;
	if ( audioqueue )
	{
		audioqueue->MarkQueueDeviceChanged();
	}
}




//-----------------------------------------------------------------------------
// Opens the windows wave out device
//-----------------------------------------------------------------------------
void CAudioDeviceAudioQueue::OpenWaveOut( void )
{
	if ( m_Queue ) 
		return;
		
	m_bFailed = false;
	m_bRunning = 0;
	Q_memset( m_Buffers, 0, sizeof( m_Buffers ) );
		
    m_DataFormat.mSampleRate       = 44100;
    m_DataFormat.mFormatID         = kAudioFormatLinearPCM;
    m_DataFormat.mFormatFlags      = kAudioFormatFlagIsSignedInteger|kAudioFormatFlagIsPacked;
    m_DataFormat.mBytesPerPacket   = 4; // 16-bit samples * 2 channels
    m_DataFormat.mFramesPerPacket  = 1;
    m_DataFormat.mBytesPerFrame    = 4; // 16-bit samples * 2 channels
    m_DataFormat.mChannelsPerFrame = 2;
    m_DataFormat.mBitsPerChannel   = 16;
    m_DataFormat.mReserved         = 0;
	
    // Create the audio queue that will be used to manage the array of audio
    // buffers used to queue samples.
    OSStatus err = AudioQueueNewOutput(&m_DataFormat, AudioCallback, this, NULL, NULL, 0, &m_Queue);	
	if ( err != noErr) 
	{
		DevMsg( "Failed to create AudioQueue output %d\n", (int)err );
		m_bFailed = true;
		return;
	}
		
    for ( int i = 0; i < NUM_BUFFERS_SOURCES; ++i) 
	{
        err = AudioQueueAllocateBuffer( m_Queue, BUFFER_SIZE,&(m_Buffers[i]));
		if ( err != noErr) 
		{
			DevMsg( "Failed to AudioQueueAllocateBuffer output %d (%i)\n",(int)err,i );
			m_bFailed = true;
			CloseWaveOut( false );
			return;
		}
		
        m_Buffers[i]->mAudioDataByteSize = BUFFER_SIZE;        
        Q_memset( m_Buffers[i]->mAudioData, 0, BUFFER_SIZE );
    }
	
    err = AudioQueuePrime( m_Queue, 0, NULL);
	if ( err != noErr) 
	{
		DevMsg( "Failed to create AudioQueue output %d\n", (int)err );
		m_bFailed = true;
		CloseWaveOut( false );
		return;
	}
	
	AudioQueueSetParameter( m_Queue, kAudioQueueParam_Volume, 1.0);
	
	err = AudioQueueAddPropertyListener( m_Queue, kAudioQueueProperty_IsRunning, AudioQueueIsRunningCallback, this );
	if ( err != noErr) 
	{
		DevMsg( "Failed to create AudioQueue output %d\n", (int)err );
		m_bFailed = true;
		CloseWaveOut( false );
		return;
	}

	// NOTE: kAudioQueueProperty_CurrentDevice is an enum constant, not a macro,
	// so it must not be probed with #ifdef (that always evaluates false and
	// silently compiles the listener out). It exists in every macOS SDK this
	// engine can build against; registration failure is nonfatal.
	err = AudioQueueAddPropertyListener( m_Queue, kAudioQueueProperty_CurrentDevice, AudioQueueCurrentDeviceChangedCallback, this );
	if ( err != noErr )
	{
		DevMsg( "Failed to listen for AudioQueue device changes %d\n", (int)err );
	}
	
	m_SndBufSize = NUM_BUFFERS_SOURCES*BUFFER_SIZE;
	m_deviceSampleCount = m_SndBufSize / DeviceSampleBytes();
	
	if ( !m_sndBuffers )
	{
		m_sndBuffers = malloc( m_SndBufSize );
		memset( m_sndBuffers, 0x0, m_SndBufSize );
	}
}


//-----------------------------------------------------------------------------
// Closes the windows wave out device
//-----------------------------------------------------------------------------
void CAudioDeviceAudioQueue::CloseWaveOut( bool bFreeMixBuffer )
{ 
	if ( m_Queue )
	{
		AudioQueueStop(m_Queue, true);
		m_bRunning = 0;
		
		AudioQueueRemovePropertyListener( m_Queue, kAudioQueueProperty_IsRunning, AudioQueueIsRunningCallback, this );
		AudioQueueRemovePropertyListener( m_Queue, kAudioQueueProperty_CurrentDevice, AudioQueueCurrentDeviceChangedCallback, this );
		
		for ( int i = 0; i < NUM_BUFFERS_SOURCES; i++ )
		{
			if ( m_Buffers[i] )
			{
				AudioQueueFreeBuffer( m_Queue, m_Buffers[i]);
				m_Buffers[i] = NULL;
			}
		}

		AudioQueueDispose( m_Queue, true);
		
		m_Queue = NULL;
	}
	
	if ( bFreeMixBuffer && m_sndBuffers )
	{
		free( m_sndBuffers );
		m_sndBuffers = NULL;
	}
}

bool CAudioDeviceAudioQueue::RecoverWaveOut( const char *pReason, OSStatus nError )
{
	if ( m_pauseCount > 0 )
		return false;

	// Rate-limit recovery. Every caller retries on later frames, so declining
	// here only delays the rebuild; recovering at frame rate would hitch the
	// main thread and can keep knocking a Bluetooth route back down while it
	// is trying to come up.
	double flNow = Plat_FloatTime();
	if ( flNow < m_flNextRecoverTime )
		return false;
	m_flNextRecoverTime = flNow + AQ_RECOVER_COOLDOWN_SEC;

	if ( nError == noErr )
	{
		DevMsg( "Recovering AudioQueue output after %s\n", pReason ? pReason : "unknown error" );
	}
	else
	{
		DevMsg( "Recovering AudioQueue output after %s %d\n", pReason ? pReason : "unknown error", (int)nError );
	}

	ResetQueuedBufferStateToPlayback();
	CloseWaveOut( false );
	ResetQueuedBufferStateToPlayback();
	OpenWaveOut();
	// AudioQueueStop( ..., true ) in CloseWaveOut should deliver flush
	// callbacks synchronously, but that isn't strictly guaranteed; sync once
	// more after the rebuild so a late callback can't leave sent < completed.
	ResetQueuedBufferStateToPlayback();
	m_lastObservedCompleted = (int)m_buffersCompleted;
	m_lastProgressTime = Plat_FloatTime();

	return ValidWaveOut() && !m_bFailed;
}



//-----------------------------------------------------------------------------
// Mixing setup
//-----------------------------------------------------------------------------
int CAudioDeviceAudioQueue::PaintBegin( float mixAheadTime, int soundtime, int paintedtime )
{
	//  soundtime - total samples that have been played out to hardware at dmaspeed
	//  paintedtime - total samples that have been mixed at speed
	//  endtime - target for samples in mixahead buffer at speed

	unsigned int endtime = soundtime + mixAheadTime * DeviceDmaSpeed();
	
	int samps = DeviceSampleCount() >> (DeviceChannels()-1);

	if ((int)(endtime - soundtime) > samps)
		endtime = soundtime + samps;

	if ((endtime - paintedtime) & 0x3)
	{
		// The difference between endtime and painted time should align on 
		// boundaries of 4 samples.  This is important when upsampling from 11khz -> 44khz.
		endtime -= (endtime - paintedtime) & 0x3;
	}

	return endtime;
}


//-----------------------------------------------------------------------------
// Actually performs the mixing
//-----------------------------------------------------------------------------
void CAudioDeviceAudioQueue::PaintEnd( void )
{
	if ( !ValidWaveOut() || m_bFailed )
	{
		RecoverWaveOut( "invalid queue" );
		if ( !ValidWaveOut() || m_bFailed )
			return;
	}

	if ( m_bQueueDeviceChanged.InterlockedExchange( 0 ) != 0 )
	{
		if ( !RecoverWaveOut( "AudioQueue device change" ) )
		{
			// Recovery declined (paused or on cooldown) - keep the request
			// pending so the route change isn't dropped.
			m_bQueueDeviceChanged = 1;
			return;
		}
	}

	int completed = (int)m_buffersCompleted;
	if ( completed != m_lastObservedCompleted )
	{
		m_lastObservedCompleted = completed;
		m_lastProgressTime = Plat_FloatTime();
		m_flStallToleranceSec = AQ_STALL_TOLERANCE_BASE_SEC;
	}
	else if ( IsQueueRunning() && QueuedBufferCount() > 0 && ( Plat_FloatTime() - m_lastProgressTime ) > m_flStallToleranceSec )
	{
		// Back off while recoveries stay unproductive; real progress resets
		// the tolerance above.
		if ( m_flStallToleranceSec * 2.0 <= AQ_STALL_TOLERANCE_MAX_SEC )
			m_flStallToleranceSec *= 2.0;
		if ( !RecoverWaveOut( "playback stall" ) )
			return;
	}

	if ( IsQueueRunning() && QueuedBufferCount() == 0 )
	{
		// We are running the audio queue but have become starved of buffers.
		// Stop the audio queue so we force a restart of it.
		OSStatus err = AudioQueueStop( m_Queue, true );
		m_bRunning = 0;
		if ( err != noErr )
		{
			if ( !RecoverWaveOut( "AudioQueueStop", err ) )
				return;
		}
	}

	//
	// submit a few new sound blocks, but never past the mixer's painted
	// frontier: ring regions beyond it still hold the previous lap's audio
	// (~0.74s old), which is what a frame hitch or a lowered snd_mixahead
	// would otherwise make audible.
	//
	// 44K sound support
	const int cTargetQueuedBuffers = 16;
	const int nFramesPerBuffer = BUFFER_SIZE / ( DeviceSampleBytes() * DeviceChannels() );
	bool bRecoveredThisPaint = false;
	while ( QueuedBufferCount() < cTargetQueuedBuffers &&
		( QueuedBufferCount() + 1 ) * nFramesPerBuffer <= PaintedAheadFrames() )
	{
		int iBuf = m_buffersSent&BUFF_MASK;

		m_Buffers[iBuf]->mAudioDataByteSize = BUFFER_SIZE;
		Q_memcpy( m_Buffers[iBuf]->mAudioData, (char *)m_sndBuffers + iBuf*BUFFER_SIZE, BUFFER_SIZE);

		// Queue the buffer for playback.
		OSStatus err = AudioQueueEnqueueBuffer( m_Queue, m_Buffers[iBuf], 0, NULL);
		if ( err != noErr)
		{
			DevMsg( "Failed to AudioQueueEnqueueBuffer output %d\n", (int)err );
			// At most one rebuild per paint: if the fresh queue also refuses
			// buffers, looping recover-and-retry here would hang the main
			// thread rebuilding 128-buffer queues back to back.
			if ( !bRecoveredThisPaint && RecoverWaveOut( "AudioQueueEnqueueBuffer", err ) )
			{
				bRecoveredThisPaint = true;
				continue;
			}
			break;
		}

		m_buffersSent++;
	}

	
	if ( !IsQueueRunning() && QueuedBufferCount() > 0 )
	{
		DevMsg( "Restarting sound playback\n" );
		OSStatus err = AudioQueuePrime( m_Queue, 0, NULL );
		if ( err != noErr )
		{
			DevMsg( "Failed to AudioQueuePrime output %d\n", (int)err );
			RecoverWaveOut( "AudioQueuePrime", err );
			return;
		}

		err = AudioQueueStart( m_Queue, NULL);
		if ( err == noErr )
		{
			m_bRunning = 1;
		}
		else
		{
			DevMsg( "Failed to AudioQueueStart output %d\n", (int)err );
			RecoverWaveOut( "AudioQueueStart", err );
		}
	}

}

int CAudioDeviceAudioQueue::GetOutputPosition( void )
{
	int completed = (int)m_buffersCompleted;

	// GetSoundTime() derives g_soundtime from this position; remember the
	// completion count backing the engine's most recent clock sample so
	// PaintedAheadFrames() can re-base (g_paintedtime - g_soundtime) onto the
	// live completion count.
	m_completedAtLastClockQuery = completed;

	int s = completed * BUFFER_SIZE;

	s >>= SAMPLE_16BIT_SHIFT;

	s &= (DeviceSampleCount()-1);

	return s / DeviceChannels();
}


//-----------------------------------------------------------------------------
// Pausing
//-----------------------------------------------------------------------------
void CAudioDeviceAudioQueue::Pause( void )
{
	m_pauseCount++;
	if (m_pauseCount == 1)
	{
		m_bRunning = 0;
		if ( m_Queue )
		{
			AudioQueueStop(m_Queue, true);
			ResetQueuedBufferStateToPlayback();
			m_lastObservedCompleted = (int)m_buffersCompleted;
			m_lastProgressTime = Plat_FloatTime();
		}
	}
}


void CAudioDeviceAudioQueue::UnPause( void )
{
	if ( m_pauseCount > 0 )
	{
		m_pauseCount--;
	}
	
	// Pause() re-synced the submit cursor to the completion cursor, so the
	// queue has nothing in flight here; the next PaintEnd() enqueues fresh
	// buffers and restarts playback.
}

bool CAudioDeviceAudioQueue::IsActive( void )
{
	return ( m_pauseCount == 0 );
}

float CAudioDeviceAudioQueue::MixDryVolume( void )
{
	return 0;
}


bool CAudioDeviceAudioQueue::Should3DMix( void )
{
	return false;
}


void CAudioDeviceAudioQueue::ClearBuffer( void )
{
	if ( !m_sndBuffers )
		return;

	Q_memset( m_sndBuffers, 0x0, DeviceSampleCount() * DeviceSampleBytes() );
}

void CAudioDeviceAudioQueue::UpdateListener( const Vector& position, const Vector& forward, const Vector& right, const Vector& up )
{
}


bool CAudioDeviceAudioQueue::BIsPlaying()
{
	if ( !m_Queue )
		return false;

	UInt32 isRunning;  
	UInt32 propSize = sizeof(isRunning);  
  
    OSStatus result = AudioQueueGetProperty( m_Queue, kAudioQueueProperty_IsRunning, &isRunning, &propSize);  
	return result == noErr && isRunning != 0;
}


void CAudioDeviceAudioQueue::MixBegin( int sampleCount )
{
	MIX_ClearAllPaintBuffers( sampleCount, false );
}


void CAudioDeviceAudioQueue::MixUpsample( int sampleCount, int filtertype )
{
	paintbuffer_t *ppaint = MIX_GetCurrentPaintbufferPtr();
	int ifilter = ppaint->ifilter;
	
	Assert (ifilter < CPAINTFILTERS);

	S_MixBufferUpsample2x( sampleCount, ppaint->pbuf, &(ppaint->fltmem[ifilter][0]), CPAINTFILTERMEM, filtertype );

	ppaint->ifilter++;
}

void CAudioDeviceAudioQueue::Mix8Mono( channel_t *pChannel, char *pData, int outputOffset, int inputOffset, fixedint rateScaleFix, int outCount, int timecompress )
{
	int volume[CCHANVOLUMES];
	paintbuffer_t *ppaint = MIX_GetCurrentPaintbufferPtr();

	if (!MIX_ScaleChannelVolume( ppaint, pChannel, volume, 1))
		return;

	Mix8MonoWavtype( pChannel, ppaint->pbuf + outputOffset, volume, (byte *)pData, inputOffset, rateScaleFix, outCount );
}


void CAudioDeviceAudioQueue::Mix8Stereo( channel_t *pChannel, char *pData, int outputOffset, int inputOffset, fixedint rateScaleFix, int outCount, int timecompress )
{
	int volume[CCHANVOLUMES];
	paintbuffer_t *ppaint = MIX_GetCurrentPaintbufferPtr();

	if (!MIX_ScaleChannelVolume( ppaint, pChannel, volume, 2 ))
		return;

	Mix8StereoWavtype( pChannel, ppaint->pbuf + outputOffset, volume, (byte *)pData, inputOffset, rateScaleFix, outCount );
}


void CAudioDeviceAudioQueue::Mix16Mono( channel_t *pChannel, short *pData, int outputOffset, int inputOffset, fixedint rateScaleFix, int outCount, int timecompress )
{
	int volume[CCHANVOLUMES];
	paintbuffer_t *ppaint = MIX_GetCurrentPaintbufferPtr();

	if (!MIX_ScaleChannelVolume( ppaint, pChannel, volume, 1 ))
		return;

	Mix16MonoWavtype( pChannel, ppaint->pbuf + outputOffset, volume, pData, inputOffset, rateScaleFix, outCount );
}


void CAudioDeviceAudioQueue::Mix16Stereo( channel_t *pChannel, short *pData, int outputOffset, int inputOffset, fixedint rateScaleFix, int outCount, int timecompress )
{
	int volume[CCHANVOLUMES];
	paintbuffer_t *ppaint = MIX_GetCurrentPaintbufferPtr();

	if (!MIX_ScaleChannelVolume( ppaint, pChannel, volume, 2 ))
		return;

	Mix16StereoWavtype( pChannel, ppaint->pbuf + outputOffset, volume, pData, inputOffset, rateScaleFix, outCount );
}


void CAudioDeviceAudioQueue::ChannelReset( int entnum, int channelIndex, float distanceMod )
{
}


void CAudioDeviceAudioQueue::TransferSamples( int end )
{
	int		lpaintedtime = g_paintedtime;
	int		endtime = end;
	
	// resumes playback...

	if ( m_sndBuffers )
	{
		S_TransferStereo16( m_sndBuffers, PAINTBUFFER, lpaintedtime, endtime );
	}
}

void CAudioDeviceAudioQueue::SpatializeChannel( int volume[CCHANVOLUMES/2], int master_vol, const Vector& sourceDir, float gain, float mono )
{
	VPROF("CAudioDeviceAudioQueue::SpatializeChannel");
	S_SpatializeChannel( volume, master_vol, &sourceDir, gain, mono );
}

void CAudioDeviceAudioQueue::StopAllSounds( void )
{
	m_bSoundsShutdown = true;
	m_bRunning = 0;
	if ( m_Queue )
	{
		AudioQueueStop(m_Queue, true);
		ResetQueuedBufferStateToPlayback();
		m_lastObservedCompleted = (int)m_buffersCompleted;
		m_lastProgressTime = Plat_FloatTime();
	}
}



void CAudioDeviceAudioQueue::ApplyDSPEffects( int idsp, portable_samplepair_t *pbuffront, portable_samplepair_t *pbufrear, portable_samplepair_t *pbufcenter, int samplecount )
{
	//SX_RoomFX( endtime, filter, timefx );
	DSP_Process( idsp, pbuffront, pbufrear, pbufcenter, samplecount );
}


static uint32 GetOSXSpeakerConfig()
{
	return 2;
}

static uint32 GetSpeakerConfigForSurroundMode( int surroundMode, const char **pConfigDesc )
{
	uint32 newSpeakerConfig = 2;
	*pConfigDesc = "stereo speaker";
	return newSpeakerConfig;
}



void OnSndSurroundCvarChanged2( IConVar *pVar, const char *pOldString, float flOldValue )
{
	// if the old value is -1, we're setting this from the detect routine for the first time
	// no need to reset the device
	if ( flOldValue == -1 )
		return;
	
	// get the user's previous speaker config
	uint32 speaker_config = GetOSXSpeakerConfig();
	
	// get the new config
	uint32 newSpeakerConfig = 0;
	const char *speakerConfigDesc = "";
	
	ConVarRef var( pVar );
	newSpeakerConfig = GetSpeakerConfigForSurroundMode( var.GetInt(), &speakerConfigDesc );
	// make sure the config has changed
	if (newSpeakerConfig == speaker_config)
		return;
	
	// set new configuration
	//SetWindowsSpeakerConfig(newSpeakerConfig);
	
	Msg("Speaker configuration has been changed to %s.\n", speakerConfigDesc);
	
	// restart sound system so it takes effect
	//g_pSoundServices->RestartSoundSystem();
}

void OnSndSurroundLegacyChanged2( IConVar *pVar, const char *pOldString, float flOldValue )
{
}


