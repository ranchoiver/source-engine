#include "shaderlib/cshader.h"
class cryostasis_magichdr_blur_ps20b_Static_Index
{
private:
	int m_nAPPROX_SRGB_ADAPTER;
#ifdef _DEBUG
	bool m_bAPPROX_SRGB_ADAPTER;
#endif
public:
	void SetAPPROX_SRGB_ADAPTER( int i )
	{
		Assert( i >= 0 && i <= 1 );
		m_nAPPROX_SRGB_ADAPTER = i;
#ifdef _DEBUG
		m_bAPPROX_SRGB_ADAPTER = true;
#endif
	}
	void SetAPPROX_SRGB_ADAPTER( bool i )
	{
		m_nAPPROX_SRGB_ADAPTER = i ? 1 : 0;
#ifdef _DEBUG
		m_bAPPROX_SRGB_ADAPTER = true;
#endif
	}
public:
	cryostasis_magichdr_blur_ps20b_Static_Index()
	{
#ifdef _DEBUG
		m_bAPPROX_SRGB_ADAPTER = false;
#endif
		m_nAPPROX_SRGB_ADAPTER = 0;
	}
	int GetIndex()
	{
#ifdef _DEBUG
		bool bAllStaticVarsDefined = m_bAPPROX_SRGB_ADAPTER;
		Assert( bAllStaticVarsDefined );
#endif
		return ( 1 * m_nAPPROX_SRGB_ADAPTER ) + 0;
	}
};
#define shaderStaticTest_cryostasis_magichdr_blur_ps20b psh_forgot_to_set_static_APPROX_SRGB_ADAPTER + 0
class cryostasis_magichdr_blur_ps20b_Dynamic_Index
{
public:
	cryostasis_magichdr_blur_ps20b_Dynamic_Index()
	{
	}
	int GetIndex()
	{
		return 0;
	}
};
#define shaderDynamicTest_cryostasis_magichdr_blur_ps20b 0
