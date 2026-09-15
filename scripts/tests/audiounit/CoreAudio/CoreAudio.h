#pragma once
#include <AudioToolbox/AudioToolbox.h>
#define MAC_OS_VERSION_12_0 120000
#define MAC_OS_X_VERSION_MAX_ALLOWED 120000
#define MAC_OS_X_VERSION_10_9 1090
typedef UInt32 AudioDeviceID;
typedef UInt32 AudioObjectID;
typedef UInt32 AudioObjectPropertySelector;
typedef UInt32 AudioObjectPropertyScope;
struct AudioObjectPropertyAddress { UInt32 mSelector,mScope,mElement; };
typedef OSStatus (*AudioObjectPropertyListenerProc)(AudioObjectID,UInt32,const AudioObjectPropertyAddress[],void*);
enum { kAudioObjectPropertyElementMain=0, kAudioObjectPropertyElementMaster=0,
       kAudioObjectPropertyScopeGlobal=30, kAudioObjectUnknown=0,
       kAudioObjectSystemObject=1, kAudioHardwarePropertyDefaultOutputDevice=31,
       kAudioHardwarePropertyPowerHint=32, kAudioHardwarePowerHintNone=0,
       kAudioDevicePropertyNominalSampleRate=33, kAudioDevicePropertyBufferFrameSize=34,
       kAudioDevicePropertyDeviceIsAlive=35 };
OSStatus AudioObjectGetPropertyData(AudioObjectID,const AudioObjectPropertyAddress*,UInt32,const void*,UInt32*,void*);
OSStatus AudioObjectSetPropertyData(AudioObjectID,const AudioObjectPropertyAddress*,UInt32,const void*,UInt32,const void*);
OSStatus AudioObjectAddPropertyListener(AudioObjectID,const AudioObjectPropertyAddress*,AudioObjectPropertyListenerProc,void*);
OSStatus AudioObjectRemovePropertyListener(AudioObjectID,const AudioObjectPropertyAddress*,AudioObjectPropertyListenerProc,void*);
