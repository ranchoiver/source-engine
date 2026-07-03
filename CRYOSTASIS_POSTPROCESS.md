# HL2 Cryostasis Postprocess

Recreates the "Cryostasis" ReShade preset (AmbientLight -> Curves -> PandaFX ->
MagicHDR -> Emphasize) inside HL2's native `dev/engine_post` pipeline, so it
works where ReShade does not exist - primarily the macOS/OpenGL build.

## Usage

```
hl2_cryostasis <0..2|on|off|toggle|reset|status>
```

Requires ps_2_b-capable hardware (all OpenGL/macOS paths qualify: GL always
uses the ps2b shader model for engine_post).

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
reducing the whole stage to a white-clip mask. Float storage also removes
every sRGB encode/decode concern - the chain samples and writes raw values on
both D3D9 and macOS GL, and the `APPROX_SRGB_ADAPTER` shader combos are
compiled but permanently off.

Sampler map in the Cryostasis combine (engine_post): s0 native thresholded
bloom (vanilla binding, AmbientLight's bright-pass source), s1 framebuffer,
s2-s4 AmbientLight dirt (native color correction is disabled while active),
s6 full-frame depth, s7-s12 bloom LODs 1-6, s13 bloom LOD 0, s15 gamma table
(untouched).

## Depth for Emphasize

On PC the "full frame depth" texture is a framebuffer copy whose destination
alpha holds linear view depth divided by `DestAlphaDepthRange` (192 world
units; 8192 under float HDR). The shader rescales that onto ReShade's
depth-buffer convention (fraction of the default 1000-unit far plane) that the
preset's focus thresholds were tuned against; the client passes
`DestAlphaDepthRange / 1000` per HDR mode. Depth saturates at 192 units, but
the preset's focus band ends far inside that range, so the clamp is not
visible. The depth snapshot is taken by the engine right after opaques render
(the only point destination alpha is coherent) - the post stage deliberately
does not re-copy it.

## Known deviations from the ReShade preset

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
- Emphasize depth comes from destination alpha, which saturates at 192 world
  units - far outside the preset's focus band, so not visible in practice.
  Depth/dirt lookups assume the viewport matches the framebuffer texture size
  (true in normal fullscreen play; `mat_viewportscale` shifts them).
- The dirt textures (~24 MB RGBA) and the eight float16 quarter-res RTs
  (~8 MB at 1080p) are resident whenever the engine_post material loads,
  Cryostasis active or not. Known cost, accepted for simplicity.
- `hl2_cryostasis` adjusts archived tonemap/bloom convars while active. Run
  `hl2_cryostasis off` before quitting to restore them - the restore snapshot
  does not survive a game restart.
