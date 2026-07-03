param(
	[string]$OutputPath = '',
	[switch]$KeepWork
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$shaderDir = Join-Path $repoRoot 'materialsystem\stdshaders'
$fxc = Join-Path $repoRoot 'dx9sdk\utilities\fxc.exe'
$scratch = Join-Path $repoRoot '.deps\hl2_flashlight_vcs'

if ( !$OutputPath )
{
	# Default to the repo's shipped-shader location so the fixed bytecode is
	# a deliverable, not a build byproduct.
	$OutputPath = Join-Path $repoRoot 'materialsystem\stdshaders\shaders\fxc\worldtwotextureblend_ps20b.vcs'
}
if ( !( [System.IO.Path]::IsPathRooted( $OutputPath ) ) )
{
	$OutputPath = Join-Path $repoRoot $OutputPath
}

if ( !( Test-Path -LiteralPath $fxc ) )
{
	throw "Missing DX9 shader compiler: $fxc"
}

New-Item -ItemType Directory -Force -Path $scratch | Out-Null
New-Item -ItemType Directory -Force -Path ( Split-Path -Parent $OutputPath ) | Out-Null

$workBin = Join-Path $scratch 'worldtwotextureblend_ps20b_combo.bin'
$dynamicCombos = 16
$totalCombos = 24576
$staticCombos = 1536
# worldtwotextureblend_ps2x.fxc declares CENTROID on TEXCOORD2/TEXCOORD3, so
# the official pipeline stamps (1<<2)|(1<<3) into the header; ToGL derives the
# same mask from the shader name, and D3D9 uses the header for the ATI MSAA
# centroid patch.
$centroidMask = 12
# The engine unpacks each uncompressed block into a fixed 128 KB buffer
# (MAX_SHADER_UNPACKED_BLOCK_SIZE); a larger block would corrupt the heap at
# load time.
$maxUnpackedBlockSize = 131072

function Get-StaticDefines( [int]$staticId )
{
	return @{
		CONVERT_TO_SRGB = ( $staticId -shr 0 ) -band 1
		DETAILTEXTURE = ( $staticId -shr 1 ) -band 1
		BUMPMAP = ( $staticId -shr 2 ) -band 1
		VERTEXCOLOR = ( $staticId -shr 3 ) -band 1
		SELFILLUM = ( $staticId -shr 4 ) -band 1
		DIFFUSEBUMPMAP = ( $staticId -shr 5 ) -band 1
		DETAIL_ALPHA_MASK_BASE_TEXTURE = ( $staticId -shr 6 ) -band 1
		FLASHLIGHT = ( $staticId -shr 7 ) -band 1
		SEAMLESS = ( $staticId -shr 8 ) -band 1
		FLASHLIGHTDEPTHFILTERMODE = ( $staticId -shr 9 )
	}
}

function Get-DynamicDefines( [int]$dynamicId )
{
	return @{
		WRITEWATERFOGTODESTALPHA = ( $dynamicId -shr 0 ) -band 1
		PIXELFOGTYPE = ( $dynamicId -shr 1 ) -band 1
		WRITE_DEPTH_TO_DESTALPHA = ( $dynamicId -shr 2 ) -band 1
		FLASHLIGHTSHADOWS = ( $dynamicId -shr 3 ) -band 1
	}
}

function Test-StaticComboSkipped( $s )
{
	return `
		( $s.DETAILTEXTURE -and $s.BUMPMAP -and !$s.DETAIL_ALPHA_MASK_BASE_TEXTURE ) -or `
		( !$s.BUMPMAP -and $s.DIFFUSEBUMPMAP ) -or `
		( $s.VERTEXCOLOR -and $s.BUMPMAP ) -or `
		( $s.FLASHLIGHT -and $s.SELFILLUM ) -or `
		( $s.FLASHLIGHT -and $s.DETAIL_ALPHA_MASK_BASE_TEXTURE ) -or `
		( $s.FLASHLIGHT -and ( $s.BUMPMAP -or $s.DIFFUSEBUMPMAP ) )
}

function Test-DynamicComboSkipped( $s, $d )
{
	return ( $s.FLASHLIGHT -eq 0 ) -and ( $d.FLASHLIGHTSHADOWS -eq 1 )
}

function Write-U32( [System.IO.BinaryWriter]$writer, [uint64]$value )
{
	$writer.Write( [uint32]$value )
}

function Compile-Combo( [int]$staticId, [int]$dynamicId, $s, $d )
{
	$shaderCombo = ( $staticId * $script:dynamicCombos ) + $dynamicId
	Remove-Item -LiteralPath $script:workBin -Force -ErrorAction SilentlyContinue

	$fxcArgs = @(
		'/nologo',
		'/Tps_2_b',
		'/Emain',
		'/DSHADER_MODEL_PS_2_B=1',
		'/Dmain=main',
		"/DTOTALSHADERCOMBOS=$script:totalCombos",
		"/DCENTROIDMASK=$script:centroidMask",
		"/DNUMDYNAMICCOMBOS=$script:dynamicCombos",
		'/DFLAGS=0x0',
		"/DSHADERCOMBO=$shaderCombo",
		"/DWRITEWATERFOGTODESTALPHA=$($d.WRITEWATERFOGTODESTALPHA)",
		"/DPIXELFOGTYPE=$($d.PIXELFOGTYPE)",
		"/DWRITE_DEPTH_TO_DESTALPHA=$($d.WRITE_DEPTH_TO_DESTALPHA)",
		"/DFLASHLIGHTSHADOWS=$($d.FLASHLIGHTSHADOWS)",
		"/DCONVERT_TO_SRGB=$($s.CONVERT_TO_SRGB)",
		"/DDETAILTEXTURE=$($s.DETAILTEXTURE)",
		"/DBUMPMAP=$($s.BUMPMAP)",
		"/DVERTEXCOLOR=$($s.VERTEXCOLOR)",
		"/DSELFILLUM=$($s.SELFILLUM)",
		"/DDIFFUSEBUMPMAP=$($s.DIFFUSEBUMPMAP)",
		"/DDETAIL_ALPHA_MASK_BASE_TEXTURE=$($s.DETAIL_ALPHA_MASK_BASE_TEXTURE)",
		"/DFLASHLIGHT=$($s.FLASHLIGHT)",
		"/DSEAMLESS=$($s.SEAMLESS)",
		"/DFLASHLIGHTDEPTHFILTERMODE=$($s.FLASHLIGHTDEPTHFILTERMODE)",
		"/Fo$script:workBin",
		'worldtwotextureblend_ps2x.fxc'
	)

	# Under $ErrorActionPreference = 'Stop', redirected native stderr lines
	# become terminating NativeCommandError records even when fxc exits 0
	# (e.g. a warning); relax it around the invocation and rely on the exit
	# code check below.
	$prevEap = $ErrorActionPreference
	$ErrorActionPreference = 'Continue'
	$output = & $script:fxc @fxcArgs 2>&1
	$ErrorActionPreference = $prevEap
	if ( $LASTEXITCODE -ne 0 -or !( Test-Path -LiteralPath $script:workBin ) )
	{
		throw "fxc failed for static=$staticId dynamic=$dynamicId shaderCombo=$shaderCombo`n$output"
	}

	return [System.IO.File]::ReadAllBytes( $script:workBin )
}

function New-StaticComboPayload( [int]$staticId, [byte[][]]$bytecodesByDynamicId )
{
	$payloadStream = New-Object System.IO.MemoryStream
	$payloadWriter = New-Object System.IO.BinaryWriter( $payloadStream )
	try
	{
		for ( $dynamicId = 0; $dynamicId -lt $script:dynamicCombos; ++$dynamicId )
		{
			$bytecode = $bytecodesByDynamicId[$dynamicId]
			if ( $null -eq $bytecode )
			{
				continue
			}
			Write-U32 $payloadWriter $dynamicId
			Write-U32 $payloadWriter $bytecode.Length
			$payloadWriter.Write( $bytecode )
		}
		$payloadWriter.Flush()
		$payload = $payloadStream.ToArray()
	}
	finally
	{
		$payloadWriter.Dispose()
		$payloadStream.Dispose()
	}

	if ( $payload.Length -gt $script:maxUnpackedBlockSize )
	{
		throw "Static combo $staticId payload is $($payload.Length) bytes; the engine's unpack buffer is $script:maxUnpackedBlockSize"
	}

	$chunkStream = New-Object System.IO.MemoryStream
	$chunkWriter = New-Object System.IO.BinaryWriter( $chunkStream )
	try
	{
		Write-U32 $chunkWriter ( [uint64]2147483648 -bor [uint64]$payload.Length )
		$chunkWriter.Write( $payload )
		Write-U32 $chunkWriter 4294967295
		$chunkWriter.Flush()
		return $chunkStream.ToArray()
	}
	finally
	{
		$chunkWriter.Dispose()
		$chunkStream.Dispose()
	}
}

$staticPayloads = @{}
$expectedCombos = New-Object 'System.Collections.Generic.HashSet[string]'
$compiledCombos = 0

Push-Location $shaderDir
try
{
	for ( $staticId = 0; $staticId -lt $staticCombos; ++$staticId )
	{
		$s = Get-StaticDefines $staticId
		if ( Test-StaticComboSkipped $s )
		{
			continue
		}

		$bytecodesByDynamicId = New-Object 'byte[][]' $dynamicCombos
		for ( $dynamicId = 0; $dynamicId -lt $dynamicCombos; ++$dynamicId )
		{
			$d = Get-DynamicDefines $dynamicId
			if ( Test-DynamicComboSkipped $s $d )
			{
				continue
			}

			$bytecodesByDynamicId[$dynamicId] = Compile-Combo $staticId $dynamicId $s $d
			[void]$expectedCombos.Add( "$staticId/$dynamicId" )
			++$compiledCombos
			if ( ( $compiledCombos % 256 ) -eq 0 )
			{
				Write-Host "Compiled $compiledCombos shader combos..."
			}
		}

		$staticPayloads[$staticId] = New-StaticComboPayload $staticId $bytecodesByDynamicId
	}
}
finally
{
	Pop-Location
}

$staticIds = @( $staticPayloads.Keys | Sort-Object { [int]$_ } )
$recordCount = $staticIds.Count + 1
$records = New-Object System.Collections.Generic.List[object]

$stream = New-Object System.IO.FileStream( $OutputPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None )
$writer = New-Object System.IO.BinaryWriter( $stream )
try
{
	Write-U32 $writer 6
	Write-U32 $writer $totalCombos
	Write-U32 $writer $dynamicCombos
	Write-U32 $writer 0
	Write-U32 $writer $centroidMask
	Write-U32 $writer $recordCount
	Write-U32 $writer 0

	$recordTableOffset = $stream.Position
	for ( $i = 0; $i -lt $recordCount; ++$i )
	{
		Write-U32 $writer 0
		Write-U32 $writer 0
	}

	Write-U32 $writer 0

	foreach ( $staticId in $staticIds )
	{
		$offset = [uint32]$stream.Position
		$records.Add( [pscustomobject]@{ StaticId = [uint32]$staticId; Offset = $offset } )
		$writer.Write( [byte[]]$staticPayloads[$staticId] )
	}

	$records.Add( [pscustomobject]@{ StaticId = [uint32]4294967295; Offset = [uint32]$stream.Position } )

	$stream.Position = $recordTableOffset
	foreach ( $record in $records )
	{
		Write-U32 $writer $record.StaticId
		Write-U32 $writer $record.Offset
	}
}
finally
{
	$writer.Dispose()
	$stream.Dispose()
}

function Read-U32( [System.IO.BinaryReader]$reader )
{
	return $reader.ReadUInt32()
}

$seenCombos = New-Object 'System.Collections.Generic.HashSet[string]'
$stream = New-Object System.IO.FileStream( $OutputPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read )
$reader = New-Object System.IO.BinaryReader( $stream )
try
{
	$version = Read-U32 $reader
	$total = Read-U32 $reader
	$dynamic = Read-U32 $reader
	$flags = Read-U32 $reader
	$centroid = Read-U32 $reader
	$numStaticRecords = Read-U32 $reader
	$sourceCrc = Read-U32 $reader

	if ( $version -ne 6 -or $total -ne $totalCombos -or $dynamic -ne $dynamicCombos -or $flags -ne 0 -or $centroid -ne $centroidMask -or $sourceCrc -ne 0 )
	{
		throw "Unexpected VCS header in $OutputPath"
	}
	if ( $numStaticRecords -ne $recordCount )
	{
		throw "Unexpected VCS static record count $numStaticRecords; expected $recordCount"
	}

	$readRecords = @()
	for ( $i = 0; $i -lt $numStaticRecords; ++$i )
	{
		$readRecords += [pscustomobject]@{ StaticId = Read-U32 $reader; Offset = Read-U32 $reader }
	}

	$dupCount = Read-U32 $reader
	if ( $dupCount -ne 0 )
	{
		throw "Unexpected duplicate combo record count $dupCount"
	}

	for ( $i = 0; $i -lt $readRecords.Count - 1; ++$i )
	{
		$record = $readRecords[$i]
		$nextRecord = $readRecords[$i + 1]
		if ( $record.StaticId -ge $nextRecord.StaticId -and $nextRecord.StaticId -ne 4294967295 )
		{
			throw "Static combo records are not sorted at index $i"
		}

		$stream.Position = $record.Offset
		while ( $true )
		{
			$blockSize = Read-U32 $reader
			if ( $blockSize -eq 4294967295 )
			{
				break
			}
			if ( ( $blockSize -band 3221225472 ) -ne 2147483648 )
			{
				throw "Unexpected compressed block in generated VCS for static combo $($record.StaticId)"
			}

			$payloadSize = [int]( $blockSize -band 1073741823 )
			$payload = $reader.ReadBytes( $payloadSize )
			$payloadStream = New-Object System.IO.MemoryStream( ,$payload )
			$payloadReader = New-Object System.IO.BinaryReader( $payloadStream )
			try
			{
				while ( $payloadStream.Position -lt $payloadStream.Length )
				{
					$dynamicId = Read-U32 $payloadReader
					$shaderSize = Read-U32 $payloadReader
					if ( $dynamicId -ge $dynamicCombos )
					{
						throw "Invalid dynamic combo id $dynamicId in static combo $($record.StaticId)"
					}
					if ( $shaderSize -eq 0 )
					{
						throw "Empty shader bytecode for static combo $($record.StaticId), dynamic combo $dynamicId"
					}
					[void]$payloadReader.ReadBytes( [int]$shaderSize )
					[void]$seenCombos.Add( "$($record.StaticId)/$dynamicId" )
				}
			}
			finally
			{
				$payloadReader.Dispose()
				$payloadStream.Dispose()
			}
		}
		if ( $stream.Position -ne $nextRecord.Offset )
		{
			throw "Static combo $($record.StaticId) ended at $($stream.Position), expected next offset $($nextRecord.Offset)"
		}
	}

	$sentinel = $readRecords[$readRecords.Count - 1]
	if ( $sentinel.StaticId -ne 4294967295 -or $sentinel.Offset -ne $stream.Length )
	{
		throw "Invalid VCS sentinel record"
	}
}
finally
{
	$reader.Dispose()
	$stream.Dispose()
}

foreach ( $expected in $expectedCombos )
{
	if ( !$seenCombos.Contains( $expected ) )
	{
		throw "Generated VCS is missing combo $expected"
	}
}
if ( $seenCombos.Count -ne $expectedCombos.Count )
{
	throw "Generated VCS has $($seenCombos.Count) combos, expected $($expectedCombos.Count)"
}

if ( !$KeepWork )
{
	Remove-Item -LiteralPath $workBin -Force -ErrorAction SilentlyContinue
}

Write-Host "Wrote $OutputPath"
Write-Host "Static records: $($staticIds.Count) plus sentinel"
Write-Host "Compiled dynamic combos: $compiledCombos"
Write-Host "Validated generated VCS structure and combo coverage."
