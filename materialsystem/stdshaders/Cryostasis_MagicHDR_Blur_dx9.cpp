//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: HL2 Cryostasis MagicHDR gaussian bloom blur pass.
//
//===========================================================================//

#include "BaseVSShader.h"

#include "screenspaceeffect_vs20.inc"
#include "cryostasis_magichdr_blur_ps20b.inc"

DEFINE_FALLBACK_SHADER( Cryostasis_MagicHDR_Blur, Cryostasis_MagicHDR_Blur_dx9 )
BEGIN_VS_SHADER_FLAGS( Cryostasis_MagicHDR_Blur_dx9, "HL2 Cryostasis MagicHDR gaussian bloom blur", SHADER_NOT_EDITABLE )
	BEGIN_SHADER_PARAMS
		SHADER_PARAM( CRYOSTASISBLURPARAMS, SHADER_PARAM_TYPE_VEC4, "[0 0 1 1]", "xy blur texel step, zw source active-region scale" )
	END_SHADER_PARAMS

	SHADER_INIT_PARAMS()
	{
		if ( !params[CRYOSTASISBLURPARAMS]->IsDefined() )
		{
			params[CRYOSTASISBLURPARAMS]->SetVecValue( 0.0f, 0.0f, 1.0f, 1.0f );
		}
	}

	SHADER_FALLBACK
	{
		if ( !g_pHardwareConfig->SupportsPixelShaders_2_b() && !g_pHardwareConfig->ShouldAlwaysUseShaderModel2bShaders() )
		{
			return "Wireframe";
		}
		return 0;
	}

	SHADER_INIT
	{
		if ( params[BASETEXTURE]->IsDefined() )
		{
			LoadTexture( BASETEXTURE );
		}
	}

	SHADER_DRAW
	{
		SHADOW_STATE
		{
			pShaderShadow->EnableDepthWrites( false );
			pShaderShadow->EnableAlphaWrites( true );
			pShaderShadow->EnableTexture( SHADER_SAMPLER0, true );
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER0, false );
			pShaderShadow->EnableSRGBWrite( false );
			pShaderShadow->VertexShaderVertexFormat( VERTEX_POSITION, 1, NULL, 0 );

			DECLARE_STATIC_VERTEX_SHADER( screenspaceeffect_vs20 );
			SET_STATIC_VERTEX_SHADER( screenspaceeffect_vs20 );

			DECLARE_STATIC_PIXEL_SHADER( cryostasis_magichdr_blur_ps20b );
			SET_STATIC_PIXEL_SHADER( cryostasis_magichdr_blur_ps20b );
		}

		DYNAMIC_STATE
		{
			BindTexture( SHADER_SAMPLER0, BASETEXTURE, -1 );
			pShaderAPI->SetPixelShaderConstant( 0, params[CRYOSTASISBLURPARAMS]->GetVecValue(), 1 );

			DECLARE_DYNAMIC_VERTEX_SHADER( screenspaceeffect_vs20 );
			SET_DYNAMIC_VERTEX_SHADER( screenspaceeffect_vs20 );

			DECLARE_DYNAMIC_PIXEL_SHADER( cryostasis_magichdr_blur_ps20b );
			SET_DYNAMIC_PIXEL_SHADER( cryostasis_magichdr_blur_ps20b );
		}
		Draw();
	}
END_SHADER
