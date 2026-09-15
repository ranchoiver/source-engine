// Shared CPU layout for the Cryostasis passes and their shader constants.
#ifndef CRYOSTASIS_HELPERS_H
#define CRYOSTASIS_HELPERS_H

namespace Cryostasis
{
enum { BloomLevels = 7 };

inline int LevelSize( int size, int level )
{
	const int reduced = size / ( 1 << level );
	return reduced > 0 ? reduced : 1;
}

// xy maps normalized image UVs into the active rectangle; zw is half a
// physical texel. Clamp each lookup to [zw, xy-zw], including blur taps.
// Integer division must agree with the viewport even at odd resolutions.
inline void BloomRegion( int width, int height, int level, float *region )
{
	region[0] = (float)LevelSize( width, level ) / width;
	region[1] = (float)LevelSize( height, level ) / height;
	region[2] = 0.5f / width;
	region[3] = 0.5f / height;
}

// DrawScreenSpaceRectangle accepts the source texels at the first and last
// destination pixel CENTERS. Ordinary 0..sourceSize-1 endpoints stretch the
// filter on downsampling and put a one-pixel destination at the wrong edge.
inline float FirstSourceTexel( int sourceSize, int destSize )
{
	return 0.5f * (float)sourceSize / destSize - 0.5f;
}

inline float LastSourceTexel( int sourceSize, int destSize )
{
	return (float)sourceSize - 1.0f - FirstSourceTexel( sourceSize, destSize );
}
}

#endif
