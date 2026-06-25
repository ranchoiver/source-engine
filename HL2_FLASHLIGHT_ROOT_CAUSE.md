# HL2 Flashlight Shadow Artifacts

## Symptom

On the native macOS/OpenGL path described by the AppleGamingWiki Half-Life 2 guide, the HL2 flashlight can draw black, screen-cell-like blocks when the projected flashlight intersects shadowed geometry. The guide works around this with `r_newflashlight 0`, which disables the projected HL2 flashlight and falls back to the older HL1-style flashlight.

The screenshots on the Desktop (`Screenshot_15.jpg`, `Screenshot_16.jpg`, and `Screenshot_17.jpg`) show black rectangular/checker artifacts inside the flashlight cone around world geometry. The pattern is not a plausible shadow edge; it looks like failed shadow-map filtering.

## Root Cause

The broken path is the `WorldTwoTextureBlend` flashlight shadow shader contract.

`materialsystem/stdshaders/worldtwotextureblend.cpp` binds:

- sampler 2: flashlight cookie texture
- sampler 6: auxiliary texture
- sampler 7: flashlight depth texture

But `materialsystem/stdshaders/worldtwotextureblend_ps2x.fxc` passed `FlashlightSampler` as both the cookie sampler and the depth sampler:

```hlsl
DoFlashlight( ..., FlashlightSampler, FlashlightSampler, NormalizeSampler, ... )
```

That means the shadow code sampled the cookie texture where it expected the shadow depth map. On the OpenGL/ToGL backend this was compounded by `ShadowDepthSamplerMaskFromName()` marking sampler 2 as the shadow-depth compare sampler for `worldtwotextureblend_ps`, even though the C++ render state puts the actual depth texture on sampler 7. The random-rotation sampler was also fed from the normalization-cubemap slot instead of the 2D shadow-noise texture used by the other flashlight shaders.

The result is binary/undefined shadow comparisons driven by the flashlight cookie and wrong noise input. On GL hardware PCF this shows up as blocky black artifacts inside the flashlight projection.

## Fix

The shader now declares and uses the same samplers that the C++ render path binds:

- `FlashlightSampler` remains on sampler 2.
- `RandomRotationSampler` uses sampler 6.
- `FlashlightDepthSampler` uses sampler 7.

`worldtwotextureblend.cpp` now binds `TEXTURE_SHADOW_NOISE_2D` to sampler 6 when flashlight shadows are active, and the ToGL shader-name sampler mask now treats sampler 7 as the depth-compare sampler for `worldtwotextureblend_ps`.

## Build Notes

The waf build compiles the engine and `stdshader_dx9` C++ DLL, but shader bytecode normally comes from `.vcs` files loaded at runtime. Because this fix changes `worldtwotextureblend_ps2x.fxc`, a packaged game build also needs regenerated `worldtwotextureblend_ps20b.vcs` and related shader outputs from `materialsystem/stdshaders/buildshaders.bat stdshader_dx9_20b` using the repo's shader compile toolchain.

Without refreshed `.vcs` shader blobs, the C++/ToGL fix is present but the old shader bytecode can still contain the wrong sampler usage.

## Isolated Validation

The repo includes a repeatable Windows-side verifier:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\verify-hl2-flashlight-sampler.ps1
```

That script uses the repo's `dx9sdk/utilities/fxc.exe` and a tiny local D3DX disassembler helper built under `.deps/hl2_flashlight_verify`. It compiles and disassembles all 384 valid shadowed `WorldTwoTextureBlend` `ps_2_b` flashlight combos (`FLASHLIGHT=1`, `FLASHLIGHTSHADOWS=1`, and the static skip rules that apply when flashlight rendering is enabled).

The verifier asserts the compiled bytecode register table and texture instructions:

- `RandomRotationSampler` is `s6`.
- `FlashlightDepthSampler` is `s7`.
- the rotation/noise texture read comes from `s6`.
- the flashlight shadow-depth reads come from `s7`.

The command passed for all 384 combos in this Windows checkout.

The official `buildshaders.bat stdshader_dx9_20b` path can generate the makefile/worklist after a local Perl `String::CRC32` shim and Visual Studio environment are supplied. Full `.vcs` regeneration remains a separate packaging step because this checkout does not ship a ready-to-use `game/bin/shadercompile.exe` layout, and locally built shadercompile binaries still hit legacy utility issues before emitting `.vcs` output.
