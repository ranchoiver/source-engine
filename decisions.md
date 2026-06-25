# HL2 Flashlight Debug Decisions

## Current Objective

Fix the HL2 projected flashlight artifacts seen on the native macOS/OpenGL Source port path, explain the root cause, and verify/build as far as this Windows environment allows.

## Evidence Read

- AppleGamingWiki Half-Life 2 page recommends `r_newflashlight 0` as a workaround because the HL2 flashlight artifacts when interacting with shadows.
- Desktop screenshots inspected:
  - `C:\Users\kate\Desktop\Screenshot_15.jpg`
  - `C:\Users\kate\Desktop\Screenshot_16.jpg`
  - `C:\Users\kate\Desktop\Screenshot_17.jpg`
- The screenshots show black rectangular/checker blocks inside the projected flashlight cone, around world geometry shadows. This looks like bad shadow-map filtering/sampler state rather than HUD, battery, or gameplay flashlight logic.

## Main Finding

The broken path is `WorldTwoTextureBlend` flashlight shadow rendering.

`materialsystem/stdshaders/worldtwotextureblend.cpp` already binds the flashlight cookie to sampler 2 and the flashlight depth texture to sampler 7. But `materialsystem/stdshaders/worldtwotextureblend_ps2x.fxc` was passing `FlashlightSampler` as both the flashlight cookie and the shadow-depth sampler, and it was passing a normalization-cubemap slot as the random-rotation sampler.

On ToGL/OpenGL this was made worse by `togl/linuxwin/dxabstract.cpp` marking sampler 2 as the `worldtwotextureblend_ps` shadow-depth compare sampler. That made the GL translator/sampler state treat the cookie sampler as a depth-compare sampler while the real depth texture on sampler 7 was not the compare sampler for that shader.

## Code Changes Made

- `materialsystem/stdshaders/worldtwotextureblend_ps2x.fxc`
  - Added `RandomRotationSampler : register( s6 )`.
  - Added `FlashlightDepthSampler : register( s7 )`.
  - Changed `DoFlashlight(...)` to pass `FlashlightDepthSampler` and `RandomRotationSampler` instead of reusing `FlashlightSampler` and `NormalizeSampler`.
- `materialsystem/stdshaders/worldtwotextureblend.cpp`
  - Enables sampler 6 for flashlight passes.
  - Treats flashlight shadows as active only when the depth texture exists, shadows are enabled, and `g_pConfig->ShadowDepthTexture()` is true.
  - Binds `TEXTURE_SHADOW_NOISE_2D` to sampler 6 when flashlight shadows are active.
- `togl/linuxwin/dxabstract.cpp`
  - Changed `ShadowDepthSamplerMaskFromName("worldtwotextureblend_ps")` from sampler 2 to sampler 7.
- `HL2_FLASHLIGHT_ROOT_CAUSE.md`
  - Added root-cause and build notes.

## Verification So Far

- `git diff --check` passed with no whitespace errors.
- Focused search found no stale `FlashlightSampler, FlashlightSampler` usage in the fixed shader.
- MSVC syntax check passed for `materialsystem/stdshaders/worldtwotextureblend.cpp` using manual project defines and Visual Studio environment:
  - `cl /Zs ... materialsystem\stdshaders\worldtwotextureblend.cpp`
- Initial full waf configure failed because this Windows checkout lacked initialized submodules and legacy-named dependency libraries.
- After initializing submodules and adding an ignored local dependency alias layer, `py -3 waf configure -T release --build-games=hl2` passed.
- Focused Waf build passed for:
  - `stdshader_dx9`
  - `shaderapidx9`
- `--use-sdl=1 --use-togl=1` configure also passed, which is the closest Windows-side configuration to the native macOS/OpenGL path.
- Full `togl` target build under Windows still fails in unrelated ToGL translation units:
  - without SDL, `ILauncherMgr` is not declared because `USE_SDL` is off.
  - with SDL, `dx9asmtogl2.cpp` fails in the Windows SDK `GL/gl.h` include path before a complete ToGL link.
- Isolated syntax-only compile for `togl/linuxwin/dxabstract.cpp` passed using the Waf-generated SDL+ToGL compile arguments.
- `buildshaders.bat stdshader_dx9_20b` was rerun with:
  - Visual Studio environment for `nmake`.
  - `PERL5LIB=/c/source-engine/.deps/perl5:/c/source-engine/devtools/bin`.
  - a local ignored `.deps/perl5/String/CRC32.pm` compatibility shim.
- With that setup, the shader scripts generated `makefile.stdshader_dx9_20b` and worklist data, but the full batch still cannot produce `.vcs` files because `game/bin/shadercompile.exe` is absent.
- `fxc_prep.pl -novcs` succeeded for:
  - `worldtwotextureblend_ps20`
  - `worldtwotextureblend_ps20b`
- The generated `worldtwotextureblend_ps20.inc` matches the checked-in include exactly.
- The generated `worldtwotextureblend_ps20b.inc` differs from the checked-in include only by a blank line, so this fix does not need a committed combo-index include update.
- The repo's `dx9sdk/utilities/fxc.exe` successfully compiled representative shadowed-flashlight `ps_2_b` combos from the modified shader for:
  - `FLASHLIGHTDEPTHFILTERMODE=0`
  - `FLASHLIGHTDEPTHFILTERMODE=1`
  - `FLASHLIGHTDEPTHFILTERMODE=2`
- The emitted assembly declares `RandomRotationSampler` on `s6` and `FlashlightDepthSampler` on `s7`, with rotation/noise sampled from `s6` and all depth taps sampled from `s7`.
- `shadercompile.exe` was not found in this checkout, so shader `.vcs` regeneration has not been done yet.
- VPC can generate `shadercompile_dll` and `shadercompile_launcher` projects, but MSBuild of `shadercompile_dll` is blocked on old internal/public library layout assumptions:
  - it expects `lib/public/ftol3.obj`.
  - it expects `lib/public/tier0.lib`, `tier1.lib`, `tier2.lib`, `vmpi.lib`, and `vstdlib.lib`.
  - it expects `lib/common/win32/2015/release/lzma.lib`.

## Build/Dependency State

- Visual Studio Build Tools are installed and usable:
  - `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat`
  - MSVC 14.44 detected by waf.
- The Visual Studio-bundled vcpkg exists but refused classic install mode without a manifest, so a normal vcpkg checkout was cloned and bootstrapped under `.deps/vcpkg`.
- Local dependency scratch paths are now ignored in `.gitignore`:
  - `.deps/`
  - `.vcpkg_installed/`
  - `.vcpkg_downloads/`
- Local vcpkg install completed successfully for:
  - `zlib`
  - `sdl2`
  - `freetype`
  - `libpng`
  - `libjpeg-turbo`
  - `curl`
- Installed vcpkg libraries use modern names:
  - `z.lib`
  - `SDL2.lib`
  - `freetype.lib`
  - `jpeg.lib`
  - `libpng16.lib`
  - `libcurl.lib`
- Waf/source legacy checks may still need local alias library filenames:
  - `libz.lib`
  - `freetype2.lib`
  - `libjpeg.lib`
  - `libpng.lib`
  - `curl.lib`
- The `.gitmodules` submodules were not initialized at first. `git submodule update --init --recursive` populated:
  - `ivp`
  - `lib`
  - `thirdparty`
- After submodule initialization, the repo-provided `lib/win32/amd64` also contains Source-style libraries such as `libz.lib`, `SDL2.lib`, `libjpeg.lib`, and `libpng.lib`.
- Still missing/unresolved for VCS regeneration:
  - `shadercompile.exe` / `shadercompile_dll.dll`
- No `shadercompile.exe` exists under `C:\source-engine`.
- `SDL_opengl.h` is available after initializing `thirdparty`.
- Local-only shader-script helpers/artifacts used during validation are intentionally ignored under `.deps/` or cleaned after use.

## Important Build Note

The waf build compiles the C++ engine/stdshader DLLs, but runtime shader bytecode normally comes from `.vcs` files. Because the core shader fix changes `worldtwotextureblend_ps2x.fxc`, a playable/package test needs regenerated `worldtwotextureblend_ps20b.vcs` and related shader outputs. Without refreshed VCS blobs, old bytecode may still contain the bad sampler usage.

## Next Decisions

- Keep the source/C++ fix in this PR rather than trying to commit generated `.vcs` blobs from an incomplete local shadercompile setup.
- Call out in the PR that shader bytecode regeneration is still needed for a packaged/runtime verification pass.
- Open the PR as a draft unless the user explicitly wants ready-for-review.

## Outstanding

- Full normal `waf configure -T release --build-games=hl2` is no longer blocked.
- Focused build for `stdshader_dx9` and `shaderapidx9` passed.
- Full `togl` link has not passed on Windows, but isolated `dxabstract.cpp` syntax check passed with Waf's SDL+ToGL arguments.
- `worldtwotextureblend_ps20b.vcs` has not been regenerated.
- `shadercompile.exe` / `shadercompile_dll.dll` still need either a matching Source SDK/Valve tool layout or a separate working build recipe.
- No live HL2 runtime render test has been performed in this Windows environment.
