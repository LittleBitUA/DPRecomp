# GPU artifact investigation log — rainbow noise on hair/foliage (RESOLVED)

The rainbow-noise artifact on alpha-tested geometry has been **fixed** in the
ReXGlue SDK: it was an EDRAM ownership-transfer behaviour in
`src/graphics/d3d12/render_target_cache.cpp` that preserved 7e3 bit patterns
when reinterpreting an EDRAM tile from HDR to UNORM8 format. Bit preservation
is Xenos-faithful, but on the host side it surfaces as colourful garbage at
any pixel that subsequent draws don't fully overwrite — exactly what happens
to alpha-test discarded fragments and SrcAlpha-blended pixels with `src.a == 0`.
Saturating the source HDR floats to [0, 1] and writing them directly as UNORM8
leaves clipped-LDR-looking residue at those pixels instead of rainbow noise,
which is visually indistinguishable from the surrounding tonemap output.

The full investigation is kept here as a reference for similar bugs in other
ports — the discarded hypotheses are useful as "do not re-bisect" notes.

![Red Room — Prologue, after the GPU artifact fix](screenshots/red_room.png)

## What we know

- DP renders the main scene to an EDRAM render target at tile 720 in
  **`k_2_10_10_10_FLOAT` (Xenos 7e3 HDR)** format. RenderDoc's `RT @ 720t`
  view interpreted as `k_2_10_10_10_FLOAT` shows a clean HDR scene; interpreted
  as `k_8_8_8_8` shows the same rainbow noise visible in the final frame. So
  the bytes really are 7e3 — the noise is a *misinterpretation* artifact.
- The game renders at **1xMSAA** (verified — see disproved hypothesis below).
- The corruption is already present in EDRAM before any resolve runs; texture
  cache and gamma passes faithfully propagate it.
- Noise is bounded to **alpha-tested/blended geometry**: hair strands, leaves,
  outlines of alpha-cut billboards. Solid skin, clothing, walls and floors
  are clean. This makes a generic "wrong format conversion" diagnosis suspect
  on its own — see below.

## Disproved hypotheses

1. **Fast vs full resolve copy path.** Patched `IsColorResolveFormatBitwiseEquivalent`
   in `rexglue-sdk/include/rex/graphics/xenos.h` to always return false, forcing
   every 32bpp resolve through `resolve_full_32bpp_cs` instead of
   `resolve_fast_32bpp_1x2xmsaa_cs`. No visual change. Reason: the FULL shader
   also reads `format` from push constants and skips conversion when source and
   destination formats match (both `k_8_8_8_8` in our case).
2. **MSAA misidentification.** Forced `color_edram_info.msaa_samples =
   MsaaSamples::k4X` in `rexglue-sdk/src/graphics/util/draw.cpp`. Result: full-
   screen cyan/green/red garbage. Confirms the game really is 1xMSAA — forcing
   4xMSAA causes the resolve to read from non-existent samples.
3. **Plain 7e3→8888 conversion in resolve.** Forced `color_edram_info.format`
   to `k_2_10_10_10_FLOAT` for all non-64bpp color resolves. Result: scene
   becomes all white (with only UI text faintly visible). The decoded 7e3
   values cover 0..32, so without scaling they clip against the 8888 0..1
   destination.
4. **7e3 source + exp_bias = -5 (1/32 scale).** Combined hack: force
   `color_edram_info.format = k_2_10_10_10_FLOAT` *and* hard-code `exp_bias = -5`
   for any 32bpp colour resolve. Result is the most informative test of the
   series: the game scene stays recognisable but heavily darkened, the
   *disclaimer* and *title* screens come out green-tinted, and crucially —
   **the rainbow-noise pattern on hair/foliage edges is still present**,
   visible as bright red speckles on the now-dark scene. This is a definitive
   ruling: if the noise survived a `×1/32` scale unchanged, it isn't a
   resolve-side format/exp_bias issue. It's intrinsic to the EDRAM bytes that
   the pixel shader produced. (The green-tinted UI screens additionally prove
   that not every 32bpp resolve is 7e3 — UI and disclaimer screens are
   genuinely `k_8_8_8_8`, so blanket format remapping at the resolve is the
   wrong shape of fix even if it had worked.)
5. **Alpha-to-coverage emulation disabled.** Dumped every translated pixel
   shader via `--dump_shaders shader_dump`, found that the translator expands
   Xenos alpha-to-coverage into a screen-space dither pattern + `oMask`
   coverage at the end of each fragment shader. The 1xMSAA fallback in that
   block is a hard checkerboard that *should* average out at 4xMSAA — exactly
   the shape of the rainbow noise we see. Patched `command_processor.cpp` to
   force `system_constants_.alpha_to_mask = 0`, killing that whole codepath.
   No visible change in-game: DP never sets `RB_COLORCONTROL.alpha_to_mask_enable`,
   so the value was already 0 in practice and the patch is a no-op.
6. **Resolve format-tracking override for HDR-host destinations.** Added a
   per-resolve diagnostic log at `draw.cpp:1057` capturing
   `(base_tiles, pitch_tiles, msaa, src_fmt, dest_fmt, exp_bias, dest_base)`
   for every 32bpp color resolve. The log identifies exactly five unique tuples
   across an entire run (disclaimer → title → in-game scene). For the in-game
   tile (`base=720 pitch=13 msaa=1x`), the HDR scene resolve correctly tracks
   `src_fmt=12` (`k_2_10_10_10_FLOAT_AS_16_16_16_16` — DP's HDR mode) into
   `dest_fmt=32` (`k_16_16_16_16_FLOAT` host texture). One rare frame showed a
   stray `src_fmt=0 dest_fmt=32` for the SAME `dest_base=0x1BE4E000` — an HDR
   host texture momentarily mis-tracked as 8888 source. Patched: when
   `src=k_8_8_8_8 && dest=k_16_16_16_16_FLOAT`, override source to
   `k_2_10_10_10_FLOAT_AS_16_16_16_16`. The rare mistrack went away in the next
   log capture, but no visible change in-game — that resolve must not actually
   feed any pixels the player sees.
7. **State-based "in-game HDR mode" override.** Stronger hypothesis: once the
   game has done an HDR `format=12` resolve at the scene tile, EDRAM at that
   tile holds 7e3 bits, so every subsequent `src=0 dest=6` resolve at the same
   tile is reading 7e3 bits and mis-interpreting them as 8888 → rainbow noise.
   Patched: a static `dp1_in_game_mode` flag latches once we observe
   `src=12, base=720, pitch=13, msaa=1x`; from then on, every
   `src=0, base=720, pitch=13, msaa=1x` resolve gets re-tagged as `src=12`
   with `exp_bias=-5`. Result: full-screen wavy blue/purple garbage with UI
   text crisp on top. This **disproves** the hypothesis — most
   `pitch=13 src=0 dest=6` resolves are genuinely 8888 (tonemap output, UI,
   intermediate post-process), and forcing 7e3 decode on them mangles
   everything. The fact that pre-hack scene rendered *correctly* on solid
   surfaces and only had rainbow on alpha-tested edges is the giveaway:
   resolve format-tracking is fine.

## Confirmed-fine pieces (don't rebisect)

- `texture_load_32bpb_cs` is innocent. Verified byte-for-byte that the shader
  reads `Buffer 317` (guest memory) at the correct tiled addresses, applies the
  expected 8-in-32 endian swap, and writes `Buffer 4403` (host staging) exactly
  as the source dictates. The noise is already in `Buffer 317` because the
  resolve upstream of it put noise there. Original "Pipeline State 388 / hash
  `247b7771-…`" lead was correct identification of the shader but wrong
  identification of the buggy stage.
- The GPU CVars `use_fuzzy_alpha_epsilon`, `native_2x_msaa=false`,
  `gpu_allow_invalid_fetch_constants`, `depth_float24_round`,
  `gamma_render_target_as_unorm16=false`, `readback_resolve=full`,
  `d3d12_readback_resolve=true`, `d3d12_readback_memexport=true` were all
  tested empirically (singly and in combinations). None affect this bug.

## Where the bug actually lived (resolved)

Pinned to the EDRAM **ownership-transfer** pass between two views of the same
tile. DP's frame structure for the Red Room scene is:

1. Render HDR scene → EDRAM tile 720 in `k_2_10_10_10_FLOAT` (7e3) — clean.
2. Switch `RB_COLOR_INFO` to `k_8_8_8_8`. ReXGlue inserts an
   `xe_transfer_color` pixel-shader pass (EID 8378 in the reference capture)
   that re-encodes float values from the 7e3 live representation back into
   7e3 bits and unpacks the same bits as UNORM8 — *bit-preserving* ownership
   transfer. EDRAM tile 720 now contains the original 7e3 byte pattern
   reinterpreted as 8888 garbage everywhere.
3. Game's tonemap pixel shader (EID 8432, hash `EDFBF3009908B2BA`) blends
   HDR + bloom + LUT and writes `oC0`. `oC0.w` is taken from the previous
   G-buffer's alpha (Tex 860, read via `tf0`). At alpha-test hair, foliage,
   statue billboards, and door-carving edges, that alpha is 0.
4. The OM blend state is standard SrcAlpha/InvSrcAlpha — at `src.a == 0`,
   `dest` is unchanged. So the tonemap pass overwrites *solid* pixels with
   the proper LDR scene, but every alpha-test edge keeps the
   ownership-transfer garbage from step 2.
5. EID 8639's resolve compute shader copies EDRAM tile 720 to `Tex 865`,
   which becomes the on-screen frame. Rainbow noise lands exactly on every
   alpha-tested silhouette.

Verified end-to-end against the reference frame:
- EDRAM `RT @ 720t, <13t>, 1xMSAA, k_2_10_10_10_FLOAT` at end-of-frame: clean
  HDR scene.
- Same bytes viewed as `k_8_8_8_8`: the rainbow noise pattern seen in-game.
- EDRAM `k_8_8_8_8` view stepped through EIDs 8378 → 8432 → 8448 → 8631:
  fully noisy after 8378, mostly clean after 8432 with noise only on
  alpha-test geometry, unchanged through 8448/8631.
- EID 8432 OM alpha channel: 0 exactly at the noisy regions, 1 everywhere
  else.
- EID 8432's pixel shader contains no explicit `discard` — the "don't write"
  behaviour is purely the blend math with `src.a == 0`.

**Fix:** patch `src/graphics/d3d12/render_target_cache.cpp` so that the
`xe_transfer_color` pass, when source is `k_2_10_10_10_FLOAT(_AS_16_16_16_16)`
and destination is `k_8_8_8_8` (or `k_8_8_8_8_GAMMA`), saturates the source
floats to `[0, 1]` and writes them directly as UNORM8 instead of
bit-preserving. Other dest formats (16_16, 16_16_16_16, 32_FLOAT, etc.)
keep the original Xenos-faithful bit-preserving path. At "don't write"
pixels the residue is now a clipped-LDR version of the HDR scene rather
than rainbow noise — visually indistinguishable from the tonemap output
sitting beside it.

### What `texture_load_32bpb_cs` looked like

That lead came from a bisection that correctly identified the shader
hash `247b7771-…` reading and writing tiled bytes, but the noise was
already in `Buffer 317` upstream of that copy. The shader is innocent;
it just propagates whatever the resolve put there.
