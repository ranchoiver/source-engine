// Deterministic fake platform declarations. Native syntax tests remove these.
#pragma once
#include <cstdint>
typedef int32_t OSStatus;
typedef uint32_t UInt32;
typedef double Float64;
typedef UInt32 AudioUnitRenderActionFlags;
struct AudioTimeStamp {};
struct AudioStreamBasicDescription {
    double mSampleRate;
    UInt32 mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket,
           mBytesPerFrame, mChannelsPerFrame, mBitsPerChannel, mReserved;
};
struct AudioBuffer { UInt32 mNumberChannels, mDataByteSize; void* mData; };
struct AudioBufferList { UInt32 mNumberBuffers; AudioBuffer mBuffers[1]; };
struct AudioComponentDescription {
    UInt32 componentType, componentSubType, componentManufacturer, componentFlags, componentFlagsMask;
};
struct FakeUnit;
typedef FakeUnit* AudioUnit;
typedef void* AudioComponent;
typedef OSStatus (*AURenderCallback)(void*,AudioUnitRenderActionFlags*,const AudioTimeStamp*,UInt32,UInt32,AudioBufferList*);
struct AURenderCallbackStruct { AURenderCallback inputProc; void* inputProcRefCon; };
enum { noErr=0, kAudio_ParamError=-50,
       kAudioFormatLinearPCM=1, kAudioFormatFlagIsSignedInteger=2,
       kAudioFormatFlagIsPacked=4, kAudioFormatFlagsNativeEndian=0,
       kAudioUnitType_Output=10, kAudioUnitSubType_HALOutput=11,
       kAudioUnitManufacturer_Apple=12, kAudioOutputUnitProperty_EnableIO=13,
       kAudioOutputUnitProperty_CurrentDevice=14, kAudioUnitScope_Global=15,
       kAudioUnitScope_Input=16, kAudioUnitScope_Output=17,
       kAudioUnitProperty_StreamFormat=18, kAudioUnitProperty_MaximumFramesPerSlice=19,
       kAudioUnitProperty_SetRenderCallback=20, kAudioUnitRenderAction_OutputIsSilence=16 };
AudioComponent AudioComponentFindNext(AudioComponent,const AudioComponentDescription*);
OSStatus AudioComponentInstanceNew(AudioComponent,AudioUnit*);
OSStatus AudioComponentInstanceDispose(AudioUnit);
OSStatus AudioUnitSetProperty(AudioUnit,UInt32,UInt32,UInt32,const void*,UInt32);
OSStatus AudioUnitGetProperty(AudioUnit,UInt32,UInt32,UInt32,void*,UInt32*);
OSStatus AudioUnitInitialize(AudioUnit);
OSStatus AudioUnitUninitialize(AudioUnit);
OSStatus AudioOutputUnitStart(AudioUnit);
OSStatus AudioOutputUnitStop(AudioUnit);
