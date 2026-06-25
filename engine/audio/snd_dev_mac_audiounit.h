//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: macOS Audio Unit output device
//
//===========================================================================//

#ifndef SND_DEV_MAC_AUDIOUNIT_H
#define SND_DEV_MAC_AUDIOUNIT_H
#pragma once

class IAudioDevice;
IAudioDevice *Audio_CreateMacAudioUnitDevice( void );

#endif // SND_DEV_MAC_AUDIOUNIT_H
