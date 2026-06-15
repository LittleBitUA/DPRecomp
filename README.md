# DPRecomp

Static recompilation of **Deadly Premonition** (Xbox 360, 2010) for native Windows,
built on the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

This project converts the Xbox 360 PowerPC `default.xex` into native x86_64 code
at build time, then wraps it with a small host runtime (FP exception guard,
hooks, future renderer patches) so the game runs natively as a real Windows
executable — no emulator required.

> **You must own the game.** This project does **not** ship any Deadly Premonition
> code, data, or assets. You provide your own legally dumped disc.

## Status

In-game playable through the Prologue cutscene with dialogue. Main menu navigates,
audio plays, controllers (DualSense tested) work, English subtitles render.

Known visual artifacts on hair, foliage, and other alpha-tested geometry are
inherited from the upstream Xenia GPU backend that ReXGlue currently uses. CPU-side
behaviour (dialogue advance, script timing, threading) runs natively — *not* via
Xenia's JIT — so logic bugs that affect DP in Xenia (chapter-1 hardlocks, broken
dialogue advance reported by the community) do not apply here.

## GPU artifact investigation log (rainbow noise on hair/foliage)

This section records what's been ruled out for the foliage/hair rainbow-noise
artifact, so future investigators don't repeat the same dead ends.

### What we know

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

### Disproved hypotheses

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

### Confirmed-fine pieces (don't rebisect)

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

### Where the bug actually lives

After ruling out the four resolve-side options above, the noise can only be
coming from earlier in the pipeline — the bytes are *already* corrupt by the
time the resolve runs. The remaining viable hypotheses all live upstream of
resolve:

- **Pixel-shader translation bug.** DP's foliage/hair pixel shader is
  presumably a Xenos shader that uses some instruction (alpha-to-coverage on
  1xMSAA, dithered alpha output, a `KILL_*` variant, or per-pixel exp_bias
  output) that `rexglue-sdk/src/graphics/shaders/dxbc_translator_*.cpp`
  lowers incorrectly. The translated DXBC then writes garbage on the
  alpha-edge fragments that should produce smooth coverage.
- **Per-pixel exp_bias output bug.** Xenos pixel shaders can write an
  `oColor.a` that the EDRAM ROP interprets as a per-pixel exp_bias for 7e3.
  If the translator isn't routing that channel correctly, alpha-edge pixels
  end up with arbitrary exponent bits.
- **A `discard`/`KILL_*` lowering that leaves the destination undefined**
  rather than writing the alpha-blended sample value. Edge pixels of
  alpha-tested geometry that *should* be opaque end up untouched and
  showing whatever junk is in EDRAM.

The shape of the bug — only at alpha-tested geometry edges, only on this
specific render target, surviving every conceivable resolve-side
transformation — points squarely at one of the above. Diagnosing further
requires extracting the actual Xenos pixel-shader bytecode DP uses for hair
and foliage (via the `dump_shaders` CVar or a RenderDoc capture of the
pre-resolve draw calls), comparing it against the translated DXBC, and
looking for the instruction or output channel that goes wrong.

## Building

### Prerequisites

- Windows 11
- Visual Studio 2022 Build Tools with the C++ workload (or full IDE)
- LLVM/Clang 20 or newer
- CMake 3.25 or newer
- Ninja 1.11 or newer
- A built and installed ReXGlue SDK (https://github.com/rexglue/rexglue-sdk),
  registered in CMake's user package registry. The SDK must be built in the same
  configuration the consumer project uses (Debug or RelWithDebInfo).

### Provide the game

Drop the contents of your Xbox 360 disc dump into `assets/`. The build expects
`assets/default.xex` to exist before codegen runs:

```
assets/
  default.xex
  nxeart
  updata/
    readfile.dir
    readfile.tbl
    readfile_en.dir
    ...
```

### Build

```powershell
# Source the MSVC environment once per shell so clang can find the headers
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'

cmake --preset win-amd64-relwithdebinfo
cmake --build --preset win-amd64-relwithdebinfo --target deadlyprem_codegen
cmake --build --preset win-amd64-relwithdebinfo --parallel 6
```

Codegen produces ~4.45M lines of recompiled C++ in `generated/default/`. First
build takes 20-30 minutes; incremental builds after config tweaks are much
faster.

### Run

```powershell
cd out\build\win-amd64-relwithdebinfo
.\deadlyprem.exe --game_data_root assets
```

If `assets/` is not next to the executable, create a junction:

```powershell
New-Item -ItemType Junction -Path out\build\win-amd64-relwithdebinfo\assets `
  -Target (Resolve-Path .\assets)
```

## Project structure

- `deadlyprem_manifest.toml` — top-level ReXGlue manifest; points at the XEX
  and pulls in `deadlyprem_config.toml`.
- `deadlyprem_config.toml` — codegen hints: manually-registered functions that
  ReXGlue's auto-discovery missed (vtable forwarders, tail-branch targets), and
  templates for switch tables and midasm hooks.
- `src/deadlyprem_app.h` — `rex::ReXApp` subclass; installs the FP exception
  guard at start, removes it at shutdown.
- `src/deadlyprem_fp_guard.h` — VEH (Windows) / SIGFPE (POSIX) handler that
  masks SSE FP exceptions raised by the recompiled guest code.
- `src/deadlyprem_hooks.cpp` — bodies for any named functions and midasm hooks
  declared in the TOML.
- `src/main.cpp` — `REX_DEFINE_APP` entry point.
- `scripts/build.py` — wraps codegen + configure + build into one command.

## Discovering missing functions

When the runtime fatals with `Call to invalid or unregistered function at guest
address 0xADDR`, add an entry under `[functions]` in `deadlyprem_config.toml`:

```toml
"0xADDR" = { name = "sub_ADDR" }
```

For batch discovery, use [`sp00nznet/360tools`](https://github.com/sp00nznet/360tools):

```powershell
python tools/extract_pe.py assets/default.xex generated/dp_pe.bin
python tools/find_missing_vtable_funcs.py generated/dp_pe.bin generated/default/deadlyprem_init.cpp
```

The scanner output is paste-compatible with `[functions]` after a trivial case
fix (`0X` → `0x`).

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) — the recompilation
  toolkit.
- [EternalSonataReprise](https://github.com/birabittoh/EternalSonataReprise) —
  the host-glue template that this project's `src/` follows, including the FP
  exception guard pattern.
- [`sp00nznet/360tools`](https://github.com/sp00nznet/360tools) — Python scanners
  for batch vtable/switch-table/import discovery.
- Xenia project — the upstream GPU stack that ReXGlue's `src/graphics/` ports in.

## Legal

The host-side source under `src/`, build scripts, CMake files, TOML configs,
and documentation are released under the MIT License (see `LICENSE`).

The recompiled game code produced at build time contains symbols and logic
from Deadly Premonition and is **not** redistributable. Do not share
`assets/default.xex`, the `generated/default/` directory, or any built binary
that links against them.
