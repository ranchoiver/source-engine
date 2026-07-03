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
the engine binaries.** It is committed at:

```
game/hl2/shaders/fxc/engine_post_ps20b.vcs
```

Install by copying `game/hl2/*` over the Half-Life 2 game directory (or into
`hl2/custom/<name>/` which is mounted automatically). To regenerate it on
Windows (`dx9sdk/utilities/fxc.exe` is required, so macOS users need the
checked-in artifact):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-cryostasis-vcs.ps1
```

The script compiles all 8 static x 80 dynamic combos, splits payloads under
the engine's 128 KB unpack limit, and validates the written file by reading it
back. `engine_post_ps20.vcs` (ps20) is unaffected: the Cryostasis combo is
pinned to 0 there and the index math is unchanged.

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

- AmbientLight's dirt textures are not vendored in this PR; the dirt weighting
  is preserved against Source's bloom with white dirt. (The follow-up parity
  PR adds the real textures.)
- AmbientLight's time-based vibrance pulse, dither, and dark-pixel guard are
  dropped, and its threshold applies to Source's pre-blurred bloom rather than
  upstream's center blur tap.
- MagicHDR's seven-level bloom pyramid is approximated with Source's
  quarter-res bloom (the follow-up parity PR builds the real chain).
- Native color correction runs after the Cryostasis stack; real ReShade ran
  after it.
- `hl2_cryostasis` adjusts archived tonemap/bloom convars while active. Run
  `hl2_cryostasis off` before quitting to restore them - the restore snapshot
  does not survive a game restart.
