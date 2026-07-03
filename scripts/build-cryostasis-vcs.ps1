param(
	[string]$OutputDir = '',
	[switch]$KeepWork
)

# Regenerates the .vcs shader bytecode affected by the Cryostasis postprocess
# changes. The engine loads precompiled shaders/fxc/*.vcs at runtime, and this
# PR changes engine_post's dynamic-combo layout (40 -> 80 combos), so the
# retail engine_post_ps20b.vcs would be indexed WRONG by the new .inc tables -
# including for vanilla LINEAR_INPUT/LINEAR_OUTPUT (macOS) combos. The
# regenerated file must ship together with the code change.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$shaderDir = Join-Path $repoRoot 'materialsystem\stdshaders'
$fxc = Join-Path $repoRoot 'dx9sdk\utilities\fxc.exe'
$scratch = Join-Path $repoRoot '.deps\cryostasis_vcs'

if ( !$OutputDir )
{
	# Ship in the game content tree so "copy game/hl2 over the HL2 dir"
	# installs it (same convention as the Cryostasis materials).
	$OutputDir = Join-Path $repoRoot 'game\hl2\shaders\fxc'
}
if ( !( [System.IO.Path]::IsPathRooted( $OutputDir ) ) )
{
	$OutputDir = Join-Path $repoRoot $OutputDir
}

if ( !( Test-Path -LiteralPath $fxc ) )
{
	throw "Missing DX9 shader compiler: $fxc"
}

New-Item -ItemType Directory -Force -Path $scratch | Out-Null
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$workBin = Join-Path $scratch 'combo.bin'

# The engine unpacks each uncompressed block into a fixed 128 KB buffer
# (MAX_SHADER_UNPACKED_BLOCK_SIZE); split payloads into multiple blocks
# below that limit, as shadercompile does.
$maxUnpackedBlockSize = 131072

function Write-U32( [System.IO.BinaryWriter]$writer, [uint64]$value )
{
	$writer.Write( [uint32]$value )
}

function Compile-Combo( [string]$sourceFile, [hashtable]$defines, [string]$label )
{
	Remove-Item -LiteralPath $script:workBin -Force -ErrorAction SilentlyContinue

	$fxcArgs = @( '/nologo', '/Tps_2_b', '/Emain', '/DSHADER_MODEL_PS_2_B=1' )
	foreach ( $name in ( $defines.Keys | Sort-Object ) )
	{
		$fxcArgs += "/D$name=$($defines[$name])"
	}
	$fxcArgs += "/Fo$script:workBin"
	$fxcArgs += $sourceFile

	# Under EAP=Stop, redirected native stderr lines become terminating
	# NativeCommandError records even when fxc exits 0; relax around the call.
	$prevEap = $ErrorActionPreference
	$ErrorActionPreference = 'Continue'
	$output = & $script:fxc @fxcArgs 2>&1
	$ErrorActionPreference = $prevEap
	if ( $LASTEXITCODE -ne 0 -or !( Test-Path -LiteralPath $script:workBin ) )
	{
		throw "fxc failed for $label`n$output"
	}

	return [System.IO.File]::ReadAllBytes( $script:workBin )
}

function New-StaticComboChunks( [byte[][]]$bytecodesByDynamicId )
{
	# Emit one or more uncompressed blocks, each holding whole
	# (dynamicId, size, bytecode) entries and staying under the unpack limit,
	# terminated by the 0xFFFFFFFF sentinel.
	$chunkStream = New-Object System.IO.MemoryStream
	$chunkWriter = New-Object System.IO.BinaryWriter( $chunkStream )
	try
	{
		$pending = New-Object System.IO.MemoryStream
		$pendingWriter = New-Object System.IO.BinaryWriter( $pending )

		function Flush-Pending
		{
			if ( $pending.Length -gt 0 )
			{
				$pendingWriter.Flush()
				$payload = $pending.ToArray()
				Write-U32 $chunkWriter ( [uint64]2147483648 -bor [uint64]$payload.Length )
				$chunkWriter.Write( $payload )
				$pending.SetLength( 0 )
			}
		}

		for ( $dynamicId = 0; $dynamicId -lt $bytecodesByDynamicId.Length; ++$dynamicId )
		{
			$bytecode = $bytecodesByDynamicId[$dynamicId]
			if ( $null -eq $bytecode )
			{
				continue
			}

			$entrySize = 8 + $bytecode.Length
			if ( $entrySize -gt $script:maxUnpackedBlockSize )
			{
				throw "Single combo $dynamicId bytecode exceeds the engine unpack limit"
			}
			if ( ( $pending.Length + $entrySize ) -gt $script:maxUnpackedBlockSize )
			{
				Flush-Pending
			}

			Write-U32 $pendingWriter $dynamicId
			Write-U32 $pendingWriter $bytecode.Length
			$pendingWriter.Write( $bytecode )
		}

		Flush-Pending
		$pendingWriter.Dispose()
		$pending.Dispose()

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

function Write-Vcs( [string]$outputPath, [uint32]$totalCombos, [uint32]$dynamicCombos, [uint32]$centroidMask, [hashtable]$staticPayloads )
{
	$staticIds = @( $staticPayloads.Keys | Sort-Object { [int]$_ } )
	$recordCount = $staticIds.Count + 1
	$records = New-Object System.Collections.Generic.List[object]

	$stream = New-Object System.IO.FileStream( $outputPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None )
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
			$records.Add( [pscustomobject]@{ StaticId = [uint32]$staticId; Offset = [uint32]$stream.Position } )
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
}

function Test-Vcs( [string]$outputPath, [uint32]$totalCombos, [uint32]$dynamicCombos, [uint32]$centroidMask, $expectedCombos )
{
	$seenCombos = New-Object 'System.Collections.Generic.HashSet[string]'
	$stream = New-Object System.IO.FileStream( $outputPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read )
	$reader = New-Object System.IO.BinaryReader( $stream )
	try
	{
		$version = $reader.ReadUInt32()
		$total = $reader.ReadUInt32()
		$dynamic = $reader.ReadUInt32()
		$flags = $reader.ReadUInt32()
		$centroid = $reader.ReadUInt32()
		$numStaticRecords = $reader.ReadUInt32()
		$sourceCrc = $reader.ReadUInt32()

		if ( $version -ne 6 -or $total -ne $totalCombos -or $dynamic -ne $dynamicCombos -or $flags -ne 0 -or $centroid -ne $centroidMask -or $sourceCrc -ne 0 )
		{
			throw "Unexpected VCS header in $outputPath"
		}

		$readRecords = @()
		for ( $i = 0; $i -lt $numStaticRecords; ++$i )
		{
			$readRecords += [pscustomobject]@{ StaticId = $reader.ReadUInt32(); Offset = $reader.ReadUInt32() }
		}

		$dupCount = $reader.ReadUInt32()
		if ( $dupCount -ne 0 )
		{
			throw "Unexpected duplicate combo record count $dupCount"
		}

		for ( $i = 0; $i -lt $readRecords.Count - 1; ++$i )
		{
			$record = $readRecords[$i]
			$nextRecord = $readRecords[$i + 1]
			$stream.Position = $record.Offset
			while ( $true )
			{
				$blockSize = $reader.ReadUInt32()
				if ( $blockSize -eq 4294967295 )
				{
					break
				}
				if ( ( $blockSize -band 3221225472 ) -ne 2147483648 )
				{
					throw "Unexpected compressed block for static combo $($record.StaticId)"
				}

				$payloadSize = [int]( $blockSize -band 1073741823 )
				if ( $payloadSize -gt $script:maxUnpackedBlockSize )
				{
					throw "Block exceeds engine unpack limit for static combo $($record.StaticId)"
				}
				$payload = $reader.ReadBytes( $payloadSize )
				$payloadStream = New-Object System.IO.MemoryStream( ,$payload )
				$payloadReader = New-Object System.IO.BinaryReader( $payloadStream )
				try
				{
					while ( $payloadStream.Position -lt $payloadStream.Length )
					{
						$dynamicId = $payloadReader.ReadUInt32()
						$shaderSize = $payloadReader.ReadUInt32()
						if ( $dynamicId -ge $dynamicCombos -or $shaderSize -eq 0 )
						{
							throw "Invalid combo entry in static combo $($record.StaticId)"
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
				throw "Static combo $($record.StaticId) ended at $($stream.Position), expected $($nextRecord.Offset)"
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
}

# ---------------------------------------------------------------------------
# engine_post_ps20b: 8 static combos (CONVERT_TO_SRGB, LINEAR_INPUT,
# LINEAR_OUTPUT) x 80 dynamic combos (AA_ENABLE, AA_QUALITY_MODE,
# AA_REDUCE_ONE_PIXEL_LINE_BLUR, COL_CORRECT_NUM_LOOKUPS 0..4,
# CRYOSTASIS_ENABLE), mirroring fxctmp9/engine_post_ps20b.inc index math.
# No skip rules; no CENTROID declarations.
# ---------------------------------------------------------------------------

$dynamicCombos = 80
$staticCombos = 8
$totalCombos = [uint32]( $staticCombos * $dynamicCombos )

$staticPayloads = @{}
$expectedCombos = New-Object 'System.Collections.Generic.HashSet[string]'
$compiledCombos = 0

Push-Location $shaderDir
try
{
	for ( $staticId = 0; $staticId -lt $staticCombos; ++$staticId )
	{
		$srgb = $staticId -band 1
		$linearInput = ( $staticId -shr 1 ) -band 1
		$linearOutput = ( $staticId -shr 2 ) -band 1

		$bytecodesByDynamicId = New-Object 'byte[][]' $dynamicCombos
		for ( $dynamicId = 0; $dynamicId -lt $dynamicCombos; ++$dynamicId )
		{
			# NOTE: [int] rounds in PowerShell; [Math]::Floor is required for
			# truncating division.
			$cryostasis = [int][Math]::Floor( $dynamicId / 40 )
			$rem = $dynamicId % 40
			$colCorrect = [int][Math]::Floor( $rem / 8 )
			$rem = $rem % 8
			$aaBlur = [int][Math]::Floor( $rem / 4 )
			$aaQuality = [int][Math]::Floor( ( $rem % 4 ) / 2 )
			$aa = $rem % 2

			$defines = @{
				CONVERT_TO_SRGB = $srgb
				LINEAR_INPUT = $linearInput
				LINEAR_OUTPUT = $linearOutput
				AA_ENABLE = $aa
				AA_QUALITY_MODE = $aaQuality
				AA_REDUCE_ONE_PIXEL_LINE_BLUR = $aaBlur
				COL_CORRECT_NUM_LOOKUPS = $colCorrect
				CRYOSTASIS_ENABLE = $cryostasis
			}

			$bytecodesByDynamicId[$dynamicId] = Compile-Combo 'Engine_Post_ps2x.fxc' $defines "engine_post static=$staticId dynamic=$dynamicId"
			[void]$expectedCombos.Add( "$staticId/$dynamicId" )
			++$compiledCombos
			if ( ( $compiledCombos % 64 ) -eq 0 )
			{
				Write-Host "Compiled $compiledCombos engine_post combos..."
			}
		}

		$staticPayloads[$staticId] = New-StaticComboChunks $bytecodesByDynamicId
	}
}
finally
{
	Pop-Location
}

$outputPath = Join-Path $OutputDir 'engine_post_ps20b.vcs'
Write-Vcs $outputPath $totalCombos $dynamicCombos 0 $staticPayloads
Test-Vcs $outputPath $totalCombos $dynamicCombos 0 $expectedCombos

if ( !$KeepWork )
{
	Remove-Item -LiteralPath $workBin -Force -ErrorAction SilentlyContinue
}

Write-Host "Wrote $outputPath"
Write-Host "Compiled dynamic combos: $compiledCombos across $staticCombos static combos"
Write-Host "Validated generated VCS structure and combo coverage."
