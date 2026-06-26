//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: HL2 Cryostasis MagicHDR inverse-tonemap bloom prepass.
//
//===========================================================================//

#include "BaseVSShader.h"

#include "screenspaceeffect_vs20.inc"
#include "cryostasis_magichdr_inverse_ps20b.inc"

DEFINE_FALLBACK_SHADER( Cryostasis_MagicHDR_Inverse, Cryostasis_MagicHDR_Inverse_dx9 )
BEGIN_VS_SHADER_FLAGS( Cryostasis_MagicHDR_Inverse_dx9, "HL2 Cryostasis MagicHDR inverse-tonemap bloom prepass", SHADER_NOT_EDITABLE )
	BEGIN_SHADER_PARAMS
		SHADER_PARAM( CRYOSTASISMAGICHDRPARAMS, SHADER_PARAM_TYPE_VEC4, "[7.806838 0 0 0]", "x bloom brightness multiplier, y bloom saturation" )
	END_SHADER_PARAMS

	SHADER_INIT_PARAMS()
	{
		if ( !params[CRYOSTASISMAGICHDRPARAMS]->IsDefined() )
		{
			params[CRYOSTASISMAGICHDRPARAMS]->SetVecValue( 7.806838f, 0.0f, 0.0f, 0.0f );
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

			DECLARE_STATIC_PIXEL_SHADER( cryostasis_magichdr_inverse_ps20b );
			SET_STATIC_PIXEL_SHADER( cryostasis_magichdr_inverse_ps20b );
		}

		DYNAMIC_STATE
		{
			BindTexture( SHADER_SAMPLER0, BASETEXTURE, -1 );
			pShaderAPI->SetPixelShaderConstant( 0, params[CRYOSTASISMAGICHDRPARAMS]->GetVecValue(), 1 );

			DECLARE_DYNAMIC_VERTEX_SHADER( screenspaceeffect_vs20 );
			SET_DYNAMIC_VERTEX_SHADER( screenspaceeffect_vs20 );

			DECLARE_DYNAMIC_PIXEL_SHADER( cryostasis_magichdr_inverse_ps20b );
			SET_DYNAMIC_PIXEL_SHADER( cryostasis_magichdr_inverse_ps20b );
		}
		Draw();
	}
END_SHADER
