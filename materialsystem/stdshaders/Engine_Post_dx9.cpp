//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================

#include "BaseVSShader.h"

#include "screenspaceeffect_vs20.inc"
#include "engine_post_ps20.inc"
#include "engine_post_ps20b.inc"

#include "../materialsystem_global.h"


DEFINE_FALLBACK_SHADER( Engine_Post, Engine_Post_dx9 )
BEGIN_VS_SHADER_FLAGS( Engine_Post_dx9, "Engine post-processing effects (software anti-aliasing, bloom, color-correction", SHADER_NOT_EDITABLE )
	BEGIN_SHADER_PARAMS
		SHADER_PARAM( FBTEXTURE,				SHADER_PARAM_TYPE_TEXTURE,	"_rt_FullFrameFB",	"Full framebuffer texture" )
		SHADER_PARAM( AAENABLE,					SHADER_PARAM_TYPE_BOOL,		"0",				"Enable software anti-aliasing" )
		SHADER_PARAM( AAINTERNAL1,				SHADER_PARAM_TYPE_VEC4,		"[0 0 0 0]",		"Internal anti-aliasing values set via material proxy" )
		SHADER_PARAM( AAINTERNAL2,				SHADER_PARAM_TYPE_VEC4,		"[0 0 0 0]",		"Internal anti-aliasing values set via material proxy" )
		SHADER_PARAM( AAINTERNAL3,				SHADER_PARAM_TYPE_VEC4,		"[0 0 0 0]",		"Internal anti-aliasing values set via material proxy" )
		SHADER_PARAM( BLOOMENABLE,				SHADER_PARAM_TYPE_BOOL,		"1",				"Enable bloom" )
		SHADER_PARAM( CRYOSTASISENABLE,			SHADER_PARAM_TYPE_BOOL,		"0",				"Enable HL2 Cryostasis ReShade post-processing" )
		SHADER_PARAM( CRYOSTASISINTERNAL1,		SHADER_PARAM_TYPE_VEC4,		"[0 0 0 0]",		"Internal Cryostasis values set via material proxy" )
		SHADER_PARAM( CRYOSTASISINTERNAL2,		SHADER_PARAM_TYPE_VEC4,		"[0 0 0 0]",		"Internal Cryostasis values set via material proxy" )
		SHADER_PARAM( CRYOSTASISINTERNAL3,		SHADER_PARAM_TYPE_VEC4,		"[0 0 0 0]",		"Internal Cryostasis values set via material proxy" )
		SHADER_PARAM( CRYOSTASISINTERNAL4,		SHADER_PARAM_TYPE_VEC4,		"[0 0 0 0]",		"Internal Cryostasis values set via material proxy" )
		SHADER_PARAM( CRYOSTASISINTERNAL5,		SHADER_PARAM_TYPE_VEC4,		"[0 0 0 0]",		"Internal Cryostasis values set via material proxy" )
		SHADER_PARAM( CRYOSTASISBLOOM0,			SHADER_PARAM_TYPE_TEXTURE,	"_rt_CryostasisBloom0",	"HL2 Cryostasis MagicHDR bloom LOD 0" )
		SHADER_PARAM( CRYOSTASISBLOOM1,			SHADER_PARAM_TYPE_TEXTURE,	"_rt_CryostasisBloom1",	"HL2 Cryostasis MagicHDR bloom LOD 1" )
		SHADER_PARAM( CRYOSTASISBLOOM2,			SHADER_PARAM_TYPE_TEXTURE,	"_rt_CryostasisBloom2",	"HL2 Cryostasis MagicHDR bloom LOD 2" )
		SHADER_PARAM( CRYOSTASISBLOOM3,			SHADER_PARAM_TYPE_TEXTURE,	"_rt_CryostasisBloom3",	"HL2 Cryostasis MagicHDR bloom LOD 3" )
		SHADER_PARAM( CRYOSTASISBLOOM4,			SHADER_PARAM_TYPE_TEXTURE,	"_rt_CryostasisBloom4",	"HL2 Cryostasis MagicHDR bloom LOD 4" )
		SHADER_PARAM( CRYOSTASISBLOOM5,			SHADER_PARAM_TYPE_TEXTURE,	"_rt_CryostasisBloom5",	"HL2 Cryostasis MagicHDR bloom LOD 5" )
		SHADER_PARAM( CRYOSTASISBLOOM6,			SHADER_PARAM_TYPE_TEXTURE,	"_rt_CryostasisBloom6",	"HL2 Cryostasis MagicHDR bloom LOD 6" )
		SHADER_PARAM( CRYOSTASISDIRT,			SHADER_PARAM_TYPE_TEXTURE,	"reshade/cryostasis/Dirt",		"HL2 Cryostasis AmbientLight Dirt.png" )
		SHADER_PARAM( CRYOSTASISDIRTOVR,		SHADER_PARAM_TYPE_TEXTURE,	"reshade/cryostasis/DirtOVR",	"HL2 Cryostasis AmbientLight DirtOVR.png" )
		SHADER_PARAM( CRYOSTASISDIRTOVB,		SHADER_PARAM_TYPE_TEXTURE,	"reshade/cryostasis/DirtOVB",	"HL2 Cryostasis AmbientLight DirtOVB.png" )
	END_SHADER_PARAMS

	SHADER_INIT_PARAMS()
	{
		if( !params[ AAENABLE ]->IsDefined() )
		{
			params[ AAENABLE ]->SetIntValue( 0 );
		}
		if( !params[ AAINTERNAL1 ]->IsDefined() )
		{
			params[ AAINTERNAL1 ]->SetVecValue( 0, 0, 0, 0 );
		}
		if( !params[ AAINTERNAL2 ]->IsDefined() )
		{
			params[ AAINTERNAL2 ]->SetVecValue( 0, 0, 0, 0 );
		}
		if( !params[ AAINTERNAL3 ]->IsDefined() )
		{
			params[ AAINTERNAL3 ]->SetVecValue( 0, 0, 0, 0 );
		}
		if( !params[ BLOOMENABLE ]->IsDefined() )
		{
			params[ BLOOMENABLE ]->SetIntValue( 1 );
		}
		if( !params[ CRYOSTASISENABLE ]->IsDefined() )
		{
			params[ CRYOSTASISENABLE ]->SetIntValue( 0 );
		}
		if( !params[ CRYOSTASISINTERNAL1 ]->IsDefined() )
		{
			params[ CRYOSTASISINTERNAL1 ]->SetVecValue( 0, 0, 0, 0 );
		}
		if( !params[ CRYOSTASISINTERNAL2 ]->IsDefined() )
		{
			params[ CRYOSTASISINTERNAL2 ]->SetVecValue( 0, 0, 0, 0 );
		}
		if( !params[ CRYOSTASISINTERNAL3 ]->IsDefined() )
		{
			params[ CRYOSTASISINTERNAL3 ]->SetVecValue( 0, 0, 0, 0 );
		}
		if( !params[ CRYOSTASISINTERNAL4 ]->IsDefined() )
		{
			params[ CRYOSTASISINTERNAL4 ]->SetVecValue( 0, 0, 0, 0 );
		}
		if( !params[ CRYOSTASISINTERNAL5 ]->IsDefined() )
		{
			params[ CRYOSTASISINTERNAL5 ]->SetVecValue( 0, 0, 0, 0 );
		}
		SET_FLAGS2( MATERIAL_VAR2_NEEDS_FULL_FRAME_BUFFER_TEXTURE );
	}

	SHADER_FALLBACK
	{
		// This shader should not be *used* unless we're >= DX9  (bloomadd.vmt/screenspace_general_dx8 should be used for DX8)
		return 0;
	}

	SHADER_INIT
	{
		if ( params[BASETEXTURE]->IsDefined() )
		{
			LoadTexture( BASETEXTURE );
		}
		if ( params[FBTEXTURE]->IsDefined() )
		{
			LoadTexture( FBTEXTURE );
		}
		if ( params[CRYOSTASISBLOOM0]->IsDefined() )
		{
			LoadTexture( CRYOSTASISBLOOM0 );
		}
		if ( params[CRYOSTASISBLOOM1]->IsDefined() )
		{
			LoadTexture( CRYOSTASISBLOOM1 );
		}
		if ( params[CRYOSTASISBLOOM2]->IsDefined() )
		{
			LoadTexture( CRYOSTASISBLOOM2 );
		}
		if ( params[CRYOSTASISBLOOM3]->IsDefined() )
		{
			LoadTexture( CRYOSTASISBLOOM3 );
		}
		if ( params[CRYOSTASISBLOOM4]->IsDefined() )
		{
			LoadTexture( CRYOSTASISBLOOM4 );
		}
		if ( params[CRYOSTASISBLOOM5]->IsDefined() )
		{
			LoadTexture( CRYOSTASISBLOOM5 );
		}
		if ( params[CRYOSTASISBLOOM6]->IsDefined() )
		{
			LoadTexture( CRYOSTASISBLOOM6 );
		}
		if ( params[CRYOSTASISDIRT]->IsDefined() )
		{
			LoadTexture( CRYOSTASISDIRT );
		}
		if ( params[CRYOSTASISDIRTOVR]->IsDefined() )
		{
			LoadTexture( CRYOSTASISDIRTOVR );
		}
		if ( params[CRYOSTASISDIRTOVB]->IsDefined() )
		{
			LoadTexture( CRYOSTASISDIRTOVB );
		}
	}

	SHADER_DRAW
	{
		SHADOW_STATE
		{
			// This shader uses opaque blending, but needs to match the behaviour of bloom_add/screen_spacegeneral,
			// which uses additive blending (and is used when bloom is enabled but col-correction and AA are not).
			//   BUT!
			// Hardware sRGB blending is incorrect (on pre-DX10 cards, sRGB values are added directly).
			//   SO...
			// When doing the bloom addition in the pixel shader, we need to emulate that incorrect
			// behaviour - by turning sRGB read OFF for the FB texture and by turning sRGB write OFF
			// (which is fine, since the AA process works better on an sRGB framebuffer than a linear
			// one; gamma colours more closely match luminance perception. The color-correction process
			// has always taken gamma-space values as input anyway).

			// On OpenGL OSX, we MUST do sRGB reads from the bloom and full framebuffer textures AND sRGB
			// writes on the way out to the framebuffer.  Hence, our colors are linear in the shader.
			// Given this, we use the LINEAR_INPUTS combo to convert to sRGB for the purposes of color
			// correction, since that is how the color correction textures are authored.
			bool bLinearInput = IsOSX() && g_pHardwareConfig->CanDoSRGBReadFromRTs();
			bool bLinearOutput = IsOSX() && !g_pHardwareConfig->FakeSRGBWrite() && g_pHardwareConfig->CanDoSRGBReadFromRTs();

			bool bForceSRGBReadsAndWrites = IsOSX() && g_pHardwareConfig->CanDoSRGBReadFromRTs();

			pShaderShadow->EnableBlending( false );

			// The (sRGB) bloom texture is bound to sampler 0
			pShaderShadow->EnableTexture(  SHADER_SAMPLER0, true  );
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER0, bForceSRGBReadsAndWrites );
			pShaderShadow->EnableSRGBWrite( bForceSRGBReadsAndWrites );

			// The (sRGB) full framebuffer texture is bound to sampler 1:
			pShaderShadow->EnableTexture(  SHADER_SAMPLER1, true  );
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER1, bForceSRGBReadsAndWrites );

			// Up to 4 (sRGB) color-correction lookup textures are bound to samplers 2-5:
			pShaderShadow->EnableTexture(  SHADER_SAMPLER2, true );
			pShaderShadow->EnableTexture(  SHADER_SAMPLER3, true );
			pShaderShadow->EnableTexture(  SHADER_SAMPLER4, true );
			pShaderShadow->EnableTexture(  SHADER_SAMPLER5, true );
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER2, false );
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER3, false );
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER4, false );
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER5, false );

			// Full-frame scene depth for the Cryostasis Emphasize pass.
			pShaderShadow->EnableTexture(  SHADER_SAMPLER6, true );
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER6, false );

			// MagicHDR's remaining bloom LODs are bound to samplers 7-12.
			pShaderShadow->EnableTexture(  SHADER_SAMPLER7, true );
			pShaderShadow->EnableTexture(  SHADER_SAMPLER8, true );
			pShaderShadow->EnableTexture(  SHADER_SAMPLER9, true );
			pShaderShadow->EnableTexture(  SHADER_SAMPLER10, true );
			pShaderShadow->EnableTexture(  SHADER_SAMPLER11, true );
			pShaderShadow->EnableTexture(  SHADER_SAMPLER12, true );

			// AmbientLight dirt textures use the last three ps_2_b samplers.
			pShaderShadow->EnableTexture(  SHADER_SAMPLER13, true );
			pShaderShadow->EnableTexture(  SHADER_SAMPLER14, true );
			pShaderShadow->EnableTexture(  SHADER_SAMPLER15, true );
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER7, false );
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER8, false );
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER9, false );
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER10, false );
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER11, false );
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER12, false );
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER13, false );
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER14, false );
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER15, false );

			int		format				= VERTEX_POSITION;
			int		numTexCoords		= 1;
			int *	pTexCoordDimensions	= NULL;
			int		userDataSize		= 0;
			pShaderShadow->VertexShaderVertexFormat( format, numTexCoords, pTexCoordDimensions, userDataSize );

			DECLARE_STATIC_VERTEX_SHADER( screenspaceeffect_vs20 );
			SET_STATIC_VERTEX_SHADER( screenspaceeffect_vs20 );
			
			if( g_pHardwareConfig->SupportsPixelShaders_2_b() || g_pHardwareConfig->ShouldAlwaysUseShaderModel2bShaders() ) // GL always goes the ps2b way for this shader, even on "ps20" parts
			{
				DECLARE_STATIC_PIXEL_SHADER( engine_post_ps20b );
				SET_STATIC_PIXEL_SHADER_COMBO( LINEAR_INPUT,  bLinearInput );
				SET_STATIC_PIXEL_SHADER_COMBO( LINEAR_OUTPUT, bLinearOutput );
				SET_STATIC_PIXEL_SHADER( engine_post_ps20b );
			}
			else
			{
				DECLARE_STATIC_PIXEL_SHADER( engine_post_ps20 );
				SET_STATIC_PIXEL_SHADER( engine_post_ps20 );
			}
		}
		DYNAMIC_STATE
		{
			// FIXME: need to set FBTEXTURE to be point-sampled (will speed up this shader significantly on 360)
			//        and assert that it's set to SHADER_TEXWRAPMODE_CLAMP (since the shader will sample offscreen)
			BindTexture( SHADER_SAMPLER1, FBTEXTURE,   -1 );

			ShaderColorCorrectionInfo_t ccInfo;
			pShaderAPI->GetCurrentColorCorrection( &ccInfo );
			int colCorrectNumLookups = ccInfo.m_nLookupCount;
			for( int i = 0; i < colCorrectNumLookups; i++ )
			{
				pShaderAPI->BindStandardTexture( (Sampler_t)(SHADER_SAMPLER2 + i), (StandardTextureId_t)(TEXTURE_COLOR_CORRECTION_VOLUME_0 + i) );
			}

			// Upload 1-pixel X&Y offsets [ (+dX,0,+dY,-dX) is chosen to work with the allowed ps20 swizzles ]
			// The shader will sample in a cross (up/down/left/right from the current sample), for 5-tap
			// (quality 0) mode and add another 4 samples in a diagonal cross, for 9-tap (quality 1) mode
			ITexture * pTarget	= params[FBTEXTURE]->GetTextureValue();
			int		width		= pTarget->GetActualWidth();
			int		height		= pTarget->GetActualHeight();
			float	dX			= 1.0f / width;
			float	dY			= 1.0f / height;
			float	offsets[4]	= { +dX, 0, +dY, -dX };
			pShaderAPI->SetPixelShaderConstant( 0, &offsets[0], 1 );
			// Upload AA tweakables:
			//   x - strength (this can be used to toggle the AA off, or to weaken it where pathological cases are showing)
			//   y - reduction of 1-pixel-line blurring (blurring of 1-pixel lines causes issues, so it's tunable)
			//   z - edge threshold multiplier (default 1.0, < 1.0 => more edges softened, > 1.0 => fewer edges softened)
			//   w - tap offset multiplier (default 1.0, < 1.0 => sharper image, > 1.0 => blurrier image)
			float	tweakables[4] = {	params[ AAINTERNAL1 ]->GetVecValue()[0],
										params[ AAINTERNAL1 ]->GetVecValue()[1],
										params[ AAINTERNAL3 ]->GetVecValue()[0],
										params[ AAINTERNAL3 ]->GetVecValue()[1] };
			pShaderAPI->SetPixelShaderConstant( 1, &tweakables[0], 1 );
			// Upload AA UV transform (converts bloom texture UVs to framebuffer texture UVs)
			// NOTE: we swap the order of the z and w components since 'wz' is an allowed ps20 swizzle, but 'zw' is not:
			float	uvTrans[4] = {	params[ AAINTERNAL2 ]->GetVecValue()[0],
									params[ AAINTERNAL2 ]->GetVecValue()[1],
									params[ AAINTERNAL2 ]->GetVecValue()[3],
									params[ AAINTERNAL2 ]->GetVecValue()[2] };
			pShaderAPI->SetPixelShaderConstant( 2, &uvTrans[0], 1 );

			// Upload color-correction weights:
			pShaderAPI->SetPixelShaderConstant( 3, &ccInfo.m_flDefaultWeight );
			pShaderAPI->SetPixelShaderConstant( 4, ccInfo.m_pLookupWeights );

			int aaEnabled					=    ( params[ AAINTERNAL1      ]->GetVecValue()[0] == 0.0f ) ? 0 : 1;
			int aaReduceOnePixelLineBlur	=    ( params[ AAINTERNAL1      ]->GetVecValue()[1] == 0.0f ) ? 0 : 1;
			int aaQualityMode				= (int)params[ AAINTERNAL1      ]->GetVecValue()[2];
//			int aaDebugMode					= (int)params[ AAINTERNAL1      ]->GetVecValue()[3];
			int bloomEnabled				=    ( params[ BLOOMENABLE      ]->GetIntValue()    == 0    ) ? 0 : 1;
			int colCorrectEnabled			=    ccInfo.m_bIsEnabled;
			int cryostasisEnabled			=    ( params[ CRYOSTASISENABLE ]->GetIntValue()    == 0    ) ? 0 : 1;
			cryostasisEnabled				= cryostasisEnabled && ( g_pHardwareConfig->SupportsPixelShaders_2_b() || g_pHardwareConfig->ShouldAlwaysUseShaderModel2bShaders() );

			if ( cryostasisEnabled )
				BindTexture( SHADER_SAMPLER0, CRYOSTASISBLOOM0, -1 );
			else
				BindTexture( SHADER_SAMPLER0, BASETEXTURE, -1 );

			float flBloomFactor = bloomEnabled ? 1.0f : 0.0f;
			float bloomConstant[4] = { flBloomFactor, flBloomFactor, flBloomFactor, flBloomFactor };
			pShaderAPI->SetPixelShaderConstant( 5, bloomConstant );

			pShaderAPI->SetPixelShaderConstant( 6, params[ CRYOSTASISINTERNAL1 ]->GetVecValue(), 1 );
			pShaderAPI->SetPixelShaderConstant( 7, params[ CRYOSTASISINTERNAL2 ]->GetVecValue(), 1 );
			pShaderAPI->SetPixelShaderConstant( 8, params[ CRYOSTASISINTERNAL3 ]->GetVecValue(), 1 );
			pShaderAPI->SetPixelShaderConstant( 9, params[ CRYOSTASISINTERNAL4 ]->GetVecValue(), 1 );
			pShaderAPI->SetPixelShaderConstant( 10, params[ CRYOSTASISINTERNAL5 ]->GetVecValue(), 1 );
			if ( cryostasisEnabled )
			{
				pShaderAPI->BindStandardTexture( SHADER_SAMPLER6, TEXTURE_FRAME_BUFFER_FULL_DEPTH );
				BindTexture( SHADER_SAMPLER7, CRYOSTASISBLOOM1, -1 );
				BindTexture( SHADER_SAMPLER8, CRYOSTASISBLOOM2, -1 );
				BindTexture( SHADER_SAMPLER9, CRYOSTASISBLOOM3, -1 );
				BindTexture( SHADER_SAMPLER10, CRYOSTASISBLOOM4, -1 );
				BindTexture( SHADER_SAMPLER11, CRYOSTASISBLOOM5, -1 );
				BindTexture( SHADER_SAMPLER12, CRYOSTASISBLOOM6, -1 );
				BindTexture( SHADER_SAMPLER13, CRYOSTASISDIRT, -1 );
				BindTexture( SHADER_SAMPLER14, CRYOSTASISDIRTOVR, -1 );
				BindTexture( SHADER_SAMPLER15, CRYOSTASISDIRTOVB, -1 );
			}

			if ( !colCorrectEnabled )
			{
				colCorrectNumLookups = 0;
			}
			
			if( g_pHardwareConfig->SupportsPixelShaders_2_b() || g_pHardwareConfig->ShouldAlwaysUseShaderModel2bShaders() ) // GL always goes the ps2b way for this shader, even on "ps20" parts
			{
				DECLARE_DYNAMIC_PIXEL_SHADER( engine_post_ps20b );
				SET_DYNAMIC_PIXEL_SHADER_COMBO( AA_ENABLE,						aaEnabled );
				SET_DYNAMIC_PIXEL_SHADER_COMBO( AA_QUALITY_MODE,				aaQualityMode );
				SET_DYNAMIC_PIXEL_SHADER_COMBO( AA_REDUCE_ONE_PIXEL_LINE_BLUR,	aaReduceOnePixelLineBlur );
//				SET_DYNAMIC_PIXEL_SHADER_COMBO( AA_DEBUG_MODE,					aaDebugMode );
				SET_DYNAMIC_PIXEL_SHADER_COMBO( COL_CORRECT_NUM_LOOKUPS,		colCorrectNumLookups );
				SET_DYNAMIC_PIXEL_SHADER_COMBO( CRYOSTASIS_ENABLE,				cryostasisEnabled );
				SET_DYNAMIC_PIXEL_SHADER( engine_post_ps20b );
			}
			else
			{
				DECLARE_DYNAMIC_PIXEL_SHADER( engine_post_ps20 );
				SET_DYNAMIC_PIXEL_SHADER_COMBO( AA_ENABLE,						aaEnabled );
				SET_DYNAMIC_PIXEL_SHADER_COMBO( AA_QUALITY_MODE,				0 ); // Only enough instruction slots in ps2b
				SET_DYNAMIC_PIXEL_SHADER_COMBO( AA_REDUCE_ONE_PIXEL_LINE_BLUR,	0 );
//				SET_DYNAMIC_PIXEL_SHADER_COMBO( AA_DEBUG_MODE,					aaDebugMode );
				SET_DYNAMIC_PIXEL_SHADER_COMBO( COL_CORRECT_NUM_LOOKUPS,		colCorrectNumLookups );
				SET_DYNAMIC_PIXEL_SHADER_COMBO( CRYOSTASIS_ENABLE,				0 );
				SET_DYNAMIC_PIXEL_SHADER( engine_post_ps20 );
			}

			DECLARE_DYNAMIC_VERTEX_SHADER( screenspaceeffect_vs20 );
			SET_DYNAMIC_VERTEX_SHADER( screenspaceeffect_vs20 );
		}
		Draw();
	}
END_SHADER
