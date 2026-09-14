// Fake platform boundary, not an implementation of the production state machine.
#pragma once
#include <cstdint>
typedef int32_t OSStatus;
typedef uint32_t UInt32;
typedef UInt32 AudioQueuePropertyID;
typedef UInt32 AudioQueueParameterID;
struct AudioTimeStamp;
struct AudioStreamPacketDescription;
struct FakeQueue;
typedef FakeQueue* AudioQueueRef;
struct AudioQueueBuffer { UInt32 mAudioDataByteSize; void* mAudioData; };
typedef AudioQueueBuffer* AudioQueueBufferRef;
typedef void (*AudioQueueOutputCallback)(void*, AudioQueueRef, AudioQueueBufferRef);
typedef void (*AudioQueuePropertyListenerProc)(void*, AudioQueueRef, AudioQueuePropertyID);
struct AudioStreamBasicDescription {
    double mSampleRate;
    UInt32 mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket,
           mBytesPerFrame, mChannelsPerFrame, mBitsPerChannel, mReserved;
};
enum { noErr=0, kAudioFormatLinearPCM=1, kAudioFormatFlagIsSignedInteger=2,
       kAudioFormatFlagIsPacked=4, kAudioQueueParam_Volume=5,
       kAudioQueueProperty_IsRunning=6, kAudioQueueProperty_CurrentDevice=7 };
OSStatus AudioQueueNewOutput(const AudioStreamBasicDescription*, AudioQueueOutputCallback,
                            void*, const void*, const void*, UInt32, AudioQueueRef*);
OSStatus AudioQueueAllocateBuffer(AudioQueueRef, UInt32, AudioQueueBufferRef*);
OSStatus AudioQueueFreeBuffer(AudioQueueRef, AudioQueueBufferRef);
OSStatus AudioQueuePrime(AudioQueueRef, UInt32, UInt32*);
OSStatus AudioQueueSetParameter(AudioQueueRef, AudioQueueParameterID, float);
OSStatus AudioQueueAddPropertyListener(AudioQueueRef, AudioQueuePropertyID, AudioQueuePropertyListenerProc, void*);
OSStatus AudioQueueRemovePropertyListener(AudioQueueRef, AudioQueuePropertyID, AudioQueuePropertyListenerProc, void*);
OSStatus AudioQueueGetProperty(AudioQueueRef, AudioQueuePropertyID, void*, UInt32*);
OSStatus AudioQueueStop(AudioQueueRef, bool);
OSStatus AudioQueueDispose(AudioQueueRef, bool);
OSStatus AudioQueueEnqueueBuffer(AudioQueueRef, AudioQueueBufferRef, UInt32, const AudioStreamPacketDescription*);
OSStatus AudioQueueStart(AudioQueueRef, const AudioTimeStamp*);
