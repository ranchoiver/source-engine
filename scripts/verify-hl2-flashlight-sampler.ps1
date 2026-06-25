param(
	[switch]$KeepArtifacts
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$shaderDir = Join-Path $repoRoot 'materialsystem\stdshaders'
$fxc = Join-Path $repoRoot 'dx9sdk\utilities\fxc.exe'
$dxInclude = Join-Path $repoRoot 'dx9sdk\include'
$dxLib = Join-Path $repoRoot 'dx9sdk\lib\x86'
$scratch = Join-Path $repoRoot '.deps\hl2_flashlight_verify'
$helperCpp = Join-Path $scratch 'disasm_shader.cpp'
$helperExe = Join-Path $scratch 'disasm_shader.exe'

if ( !( Test-Path -LiteralPath $fxc ) )
{
	throw "Missing DX9 shader compiler: $fxc"
}

New-Item -ItemType Directory -Force -Path $scratch | Out-Null

$helperSource = @'
#include <windows.h>
#include <d3dx9shader.h>

#include <stdio.h>
#include <vector>

int main( int argc, char **argv )
{
	if ( argc != 2 )
	{
		fprintf( stderr, "usage: disasm_shader <shader.bin>\n" );
		return 2;
	}

	FILE *fp = fopen( argv[1], "rb" );
	if ( !fp )
	{
		fprintf( stderr, "failed to open %s\n", argv[1] );
		return 2;
	}

	fseek( fp, 0, SEEK_END );
	long len = ftell( fp );
	fseek( fp, 0, SEEK_SET );

	std::vector< unsigned char > data( len );
	if ( len > 0 )
	{
		fread( data.data(), 1, len, fp );
	}
	fclose( fp );

	LPD3DXBUFFER listing = NULL;
	HRESULT hr = D3DXDisassembleShader( reinterpret_cast< const DWORD * >( data.data() ), FALSE, NULL, &listing );
	if ( FAILED( hr ) || !listing )
	{
		fprintf( stderr, "D3DXDisassembleShader failed: 0x%08lx\n", hr );
		return 1;
	}

	fwrite( listing->GetBufferPointer(), 1, listing->GetBufferSize(), stdout );
	listing->Release();
	return 0;
}
'@

$existingHelperSource = if ( Test-Path -LiteralPath $helperCpp ) { Get-Content -Raw -LiteralPath $helperCpp } else { '' }
if ( $existingHelperSource -ne $helperSource )
{
	Set-Content -LiteralPath $helperCpp -Value $helperSource -Encoding ASCII
}

$vsDevCmdCandidates = @(
	'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat',
	'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat',
	'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat',
	'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat',
	'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat'
)
$vsDevCmd = $vsDevCmdCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ( !$vsDevCmd )
{
	throw 'Could not find Visual Studio 2022 VsDevCmd.bat to build the D3DX disassembler helper.'
}

Push-Location $scratch
try
{
	$buildCmd = 'call "{0}" -arch=x86 >nul && cl /nologo /EHsc /I "{1}" "{2}" /Fe:"{3}" /link /LIBPATH:"{4}" d3dx9.lib' -f $vsDevCmd, $dxInclude, $helperCpp, $helperExe, $dxLib
	$buildOutput = & cmd.exe /d /c $buildCmd 2>&1
	if ( $LASTEXITCODE -ne 0 )
	{
		throw "Failed to build $helperExe`n$buildOutput"
	}
}
finally
{
	Pop-Location
}

$bools = @( 0, 1 )
$depthFilterModes = @( 0, 1, 2 )
$failures = New-Object System.Collections.Generic.List[string]
$comboCount = 0
$bin = Join-Path $scratch 'worldtwotextureblend_ps20b_verify.bin'

Push-Location $shaderDir
try
{
	foreach ( $convertToSrgb in $bools )
	{
		foreach ( $detailTexture in $bools )
		{
			foreach ( $vertexColor in $bools )
			{
				foreach ( $seamless in $bools )
				{
					foreach ( $depthFilterMode in $depthFilterModes )
					{
						foreach ( $writeWaterFogToDestAlpha in $bools )
						{
							foreach ( $pixelFogType in $bools )
							{
								foreach ( $writeDepthToDestAlpha in $bools )
								{
									$comboLabel = "srgb=$convertToSrgb detail=$detailTexture vertexcolor=$vertexColor seamless=$seamless filter=$depthFilterMode waterfog=$writeWaterFogToDestAlpha pixelfog=$pixelFogType writedepth=$writeDepthToDestAlpha"
									Remove-Item -LiteralPath $bin -Force -ErrorAction SilentlyContinue

									$fxcArgs = @(
										'/nologo',
										'/Tps_2_b',
										'/Emain',
										'/DSHADER_MODEL_PS_2_B=1',
										'/Dmain=main',
										'/DTOTALSHADERCOMBOS=24576',
										'/DCENTROIDMASK=0',
										'/DNUMDYNAMICCOMBOS=16',
										'/DFLAGS=0x0',
										"/DSHADERCOMBO=$comboCount",
										"/DWRITEWATERFOGTODESTALPHA=$writeWaterFogToDestAlpha",
										"/DPIXELFOGTYPE=$pixelFogType",
										"/DWRITE_DEPTH_TO_DESTALPHA=$writeDepthToDestAlpha",
										'/DFLASHLIGHTSHADOWS=1',
										"/DCONVERT_TO_SRGB=$convertToSrgb",
										"/DDETAILTEXTURE=$detailTexture",
										'/DBUMPMAP=0',
										"/DVERTEXCOLOR=$vertexColor",
										'/DSELFILLUM=0',
										'/DDIFFUSEBUMPMAP=0',
										'/DDETAIL_ALPHA_MASK_BASE_TEXTURE=0',
										'/DFLASHLIGHT=1',
										"/DSEAMLESS=$seamless",
										"/DFLASHLIGHTDEPTHFILTERMODE=$depthFilterMode",
										"/Fo$bin",
										'worldtwotextureblend_ps2x.fxc'
									)

									$compileOutput = & $fxc @fxcArgs 2>&1
									if ( $LASTEXITCODE -ne 0 -or !( Test-Path -LiteralPath $bin ) )
									{
										$failures.Add( "fxc failed for $comboLabel`n$compileOutput" )
										continue
									}

									$asmOutput = & $helperExe $bin 2>&1
									if ( $LASTEXITCODE -ne 0 )
									{
										$failures.Add( "disassembly failed for $comboLabel`n$asmOutput" )
										continue
									}

									$asm = $asmOutput -join "`n"
									$checks = @(
										@{ Name = 'RandomRotationSampler register table entry'; Pattern = '(?m)^\s*(//\s*)?RandomRotationSampler\s+s6\s+1\b' },
										@{ Name = 'FlashlightDepthSampler register table entry'; Pattern = '(?m)^\s*(//\s*)?FlashlightDepthSampler\s+s7\s+1\b' },
										@{ Name = 'sampler 6 declaration'; Pattern = '(?m)^\s*dcl_2d\s+s6\b' },
										@{ Name = 'sampler 7 declaration'; Pattern = '(?m)^\s*dcl_2d\s+s7\b' },
										@{ Name = 'random rotation/noise texture read from s6'; Pattern = '(?m)^\s*texldp?\s+[^\r\n]*,\s*s6\b' },
										@{ Name = 'flashlight depth texture read from s7'; Pattern = '(?m)^\s*texldp?\s+[^\r\n]*,\s*s7\b' }
									)

									foreach ( $check in $checks )
									{
										if ( $asm -notmatch $check.Pattern )
										{
											$failures.Add( "$($check.Name) missing for $comboLabel" )
										}
									}

									$comboCount++
									if ( ( $comboCount % 64 ) -eq 0 )
									{
										Write-Host "Verified $comboCount combos..."
									}
								}
							}
						}
					}
				}
			}
		}
	}
}
finally
{
	Pop-Location
}

if ( $failures.Count -gt 0 )
{
	$sampleFailures = $failures | Select-Object -First 20
	throw "HL2 flashlight sampler verification failed with $($failures.Count) issue(s):`n$($sampleFailures -join "`n`n")"
}

if ( !$KeepArtifacts )
{
	Remove-Item -LiteralPath $bin -Force -ErrorAction SilentlyContinue
}

Write-Host "Verified $comboCount valid shadowed WorldTwoTextureBlend ps_2_b flashlight combos."
Write-Host 'RandomRotationSampler uses s6 and FlashlightDepthSampler uses s7 in compiled bytecode.'
