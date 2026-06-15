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

Known visual artifacts on alpha-blended mip-mapped textures (hair, foliage) are
inherited from the upstream Xenia GPU backend that ReXGlue currently uses. CPU-side
behaviour (dialogue advance, script timing, threading) runs natively — *not* via
Xenia's JIT — so logic bugs that affect DP in Xenia (chapter-1 hardlocks, broken
dialogue advance reported by the community) do not apply here.

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
