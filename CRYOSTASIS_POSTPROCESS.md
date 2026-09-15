# HL2 Cryostasis Postprocess

Ports parts of the "Cryostasis" ReShade preset (AmbientLight -> Curves -> PandaFX ->
MagicHDR -> Emphasize) inside HL2's native `dev/engine_post` pipeline, so it
works where ReShade does not exist - primarily the macOS/OpenGL build.

## Usage

```
hl2_cryostasis <0..2|on|off|toggle|reset|status>
```

Requires a PC build, ps_2_b shaders, and filterable float16 render targets
(`SupportsHDRMode(HDR_TYPE_FLOAT)`). The current rendering mode must be LDR or
integer HDR: Source's separate float-HDR path never calls this engine_post
combine. The command rejects unsupported configurations before changing convars.

## Shader shipping - REQUIRED reading

The engine loads precompiled bytecode from `shaders/fxc/*.vcs` at runtime; the
waf build does NOT compile `.fxc` sources. This PR adds a dynamic combo
(`CRYOSTASIS_ENABLE`) to engine_post, which doubles the dynamic combo count
(40 -> 80) and changes the `.inc` index tables. Against Valve's retail
`engine_post_ps20b.vcs` the new tables index the WRONG combos - including for
the vanilla `LINEAR_INPUT`/`LINEAR_OUTPUT` statics that every macOS frame uses.

**The regenerated `engine_post_ps20b.vcs` must therefore ship together with
the engine binaries**, and the parity layer's two new shader families have no
retail bytecode at all. All three are committed at:

```
game/hl2/shaders/fxc/engine_post_ps20b.vcs
game/hl2/shaders/fxc/cryostasis_magichdr_inverse_ps20b.vcs
game/hl2/shaders/fxc/cryostasis_magichdr_blur_ps20b.vcs
```

Install by copying `game/hl2/*` over the Half-Life 2 game directory (or into
`hl2/custom/<name>/` which is mounted automatically). To regenerate it on
Windows (`dx9sdk/utilities/fxc.exe` is required, so macOS users need the
checked-in artifact):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-cryostasis-vcs.ps1
```

The script compiles all 8 static x 80 dynamic engine_post combos plus both
MagicHDR shader families, splits payloads under the engine's 128 KB unpack
limit, and validates each written file by reading it back.
`engine_post_ps20.vcs` (ps20) is unaffected: the Cryostasis combo is pinned to
0 there and the index math is unchanged.

## MagicHDR bloom chain

The parity layer builds MagicHDR's seven-level bloom pyramid: an inverse
tonemap pass (Reinhard inverse x exp(2.055) brightness) into
`_rt_CryostasisBloom0`, then per level a horizontal+vertical separable blur
into progressively half-sized active regions of `_rt_CryostasisBloom1..6`.
The final engine_post combine weights the seven LODs with MagicHDR's
`NormalDistribution(i, 5.6, 3.5) / 7` distribution.

All chain RTs are **float16** (`IMAGE_FORMAT_RGBA16161616F`): the inverse
pass emits HDR values up to ~78, which an 8-bit RT would clip to 1.0,
reducing the whole stage to a white-clip mask. The intermediate chain samples and writes raw float values. The framebuffer
INPUT to the inverse pass still needs special handling on macOS: when Source
forces an sRGB read from that renderable texture, adapter combo 1 applies the
same `LinearToGamma` conversion as engine_post. Its float16 output is never
squared or gamma-encoded. Blur always uses adapter combo 0.

Sampler map in the Cryostasis combine (engine_post): s0 native thresholded
bloom (vanilla binding, AmbientLight's bright-pass source), s1 framebuffer,
s2-s4 AmbientLight dirt (native color correction is disabled while active),
s6 private opaque depth, s7-s12 bloom LODs 1-6, s13 bloom LOD 0, s15 gamma table
(untouched).

## Depth for Emphasize

The shared `_rt_FullFrameDepth` name aliases `_rt_PowerOfTwoFB` on PC.
Refraction can overwrite it after opaques. Cryostasis now copies main-view
opaque destination alpha to its own `_rt_CryostasisDepth`, before translucency.
`RenderView` invalidates that snapshot; a frame without a fresh capture uses
the native postprocess. Secondary views do not overwrite the main snapshot.
The copy stretches the current viewport to the whole texture, matching the
framebuffer UV mapping even with viewport scaling or offsets. No extra copy
runs while Cryostasis is inactive.

Destination alpha stores projected depth divided by `DestAlphaDepthRange`
(192 in the supported modes). The inherited conversion divides by a presumed
1000-unit ReShade far plane. This is an approximation: matching the original
ReShade linearization also depends on its exact preprocessor definitions and
the game's projection. The original preset archive is not in this repository;
its depth appearance has **not** been established by an in-game comparison.

## Review fixes and regression coverage

- The seven bloom viewports and shader sampling regions use the same integer
  dimensions. This matters at 1080p (270 quarter-resolution rows) and other
  sizes not divisible by 64. Downsampling maps pixel centers correctly,
  including one-pixel levels.
- Each blur/final lookup clamps to written texel centers. No speculative
  border draw or stale inactive texture region is needed. Final bloom UVs
  follow the framebuffer rather than Source's differently padded native bloom.
- The private depth snapshot prevents refraction from corrupting Emphasize.
- The inverse shader handles forced macOS sRGB input without transforming its
  raw HDR output. This follows Source's existing engine_post color convention.
- Invalid materials, wrong-size targets, and non-float targets fail before
  pushing render state. Command and runtime capability checks agree.

Run `python3 scripts/test-cryostasis.py --sanitize` on Linux/macOS. It compiles
production pass orchestration, depth capture, and blur FXC math against a CPU
rendering boundary. Eighteen constant, gradient, and patterned image cases
exercise 126 bloom levels, including 1366x768/1920x1080 quarter-res dimensions,
plus small/one-pixel targets. Results are compared with an independent
reference using tightly sized images and generated Gaussian weights. It also
checks depth view/viewport lifetime, capability failures, and invalid resources.
The inverse shader and hardware sampling are not emulated by that test.

The `Cryostasis regression` workflow compiles all 644 shader variants on
Windows, verifies VCS structure, requires regenerated bytes to match the
committed files, and builds engine, client, and stdshader_dx9. These checks
cannot establish real macOS/D3D9 visual output or GPU performance.

Reference for the bloom pass topology:
[luluco250/FXShaders MagicHDR.fx, commit 76365e3](https://github.com/luluco250/FXShaders/blob/76365e35c48e30170985ca371e67d8daf8eb9a98/Shaders/MagicHDR.fx).
This independently retrieved source is not proof of the shader version in the
original Cryostasis archive.

## Known deviations from the ReShade preset

- This is not exact preset parity. The MagicHDR bloom input is currently the
  ungraded framebuffer; upstream runs after AmbientLight, Curves, and PandaFX.
  Their grading and native bloom therefore do not feed the inverse bloom pass.
  The original archive is needed to verify input/output sRGB definitions too.
- The blur passes use a 13-tap Gaussian (MagicHDR's default) even though
  Cryostasis.ini pins `MAGIC_HDR_BLUR_SAMPLES=1`, which upstream degenerates
  to a single tap (pure downsampling). The 13-tap chain is slightly softer
  and less aliased than the literal preset.
- AmbientLight's bright-pass source is Source's thresholded quarter-res bloom
  instead of upstream's own thresholded blur pyramid; its time-based vibrance
  pulse, dither, and dark-pixel guard are dropped, and its threshold applies
  to that pre-blurred bloom rather than upstream's center blur tap.
- Native color correction is disabled while Cryostasis is active (ReShade ran
  after it); the samplers it used carry the dirt textures instead.
- Emphasize uses destination alpha rather than the original depth buffer; see
  the projection/linearization qualification above.
- The dirt textures (~24 MB RGBA) and the eight float16 quarter-res RTs
  (~8 MB at 1080p), plus a full-resolution depth copy (~8 MB), remain allocated
  on supported hardware,
  Cryostasis active or not. Known cost, accepted for simplicity.
- `hl2_cryostasis` adjusts archived tonemap/bloom convars while active. Run
  `hl2_cryostasis off` before quitting to restore them - the restore snapshot
  does not survive a game restart.
