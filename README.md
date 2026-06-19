<div align="center">

# Deadly Premonition — PC Port (DPRecomp)

### Play *Deadly Premonition* natively on Windows, with keyboard + mouse, more stable than the official PC port — no emulator required.

[![Latest release](https://img.shields.io/github/v/release/LittleBitUA/DPRecomp?style=for-the-badge&label=Download&color=blue)](https://github.com/LittleBitUA/DPRecomp/releases/latest)
[![Total downloads](https://img.shields.io/github/downloads/LittleBitUA/DPRecomp/latest/total?style=for-the-badge&color=brightgreen)](https://github.com/LittleBitUA/DPRecomp/releases)
[![License](https://img.shields.io/github/license/LittleBitUA/DPRecomp?style=for-the-badge&color=lightgrey)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-0078D6?style=for-the-badge&logo=windows)](https://github.com/LittleBitUA/DPRecomp/releases/latest)
[![Stars](https://img.shields.io/github/stars/LittleBitUA/DPRecomp?style=for-the-badge&color=yellow)](https://github.com/LittleBitUA/DPRecomp/stargazers)

![Red Room — Prologue, after the GPU artifact fix](docs/screenshots/red_room.png)

## [⬇  Download v0.1.1 for Windows](https://github.com/LittleBitUA/DPRecomp/releases/latest)

</div>

---

## Table of contents

- [What is this?](#what-is-this)
- [Why does this exist?](#why-does-this-exist)
- [Comparison: official PC port vs Xenia vs DPRecomp](#comparison-official-pc-port-vs-xenia-vs-dprecomp)
- [Screenshots](#screenshots)
- [What you need before playing](#what-you-need-before-playing)
- [How to install and play](#how-to-install-and-play)
- [Default controls (PC Director's Cut style)](#default-controls-pc-directors-cut-style)
- [Frequently asked questions](#frequently-asked-questions)
- [Building from source](#building-from-source)
- [Technical deep-dives](#technical-deep-dives)
- [Credits](#credits)
- [Legal](#legal)

---

## What is this?

**DPRecomp is a native Windows port of *Deadly Premonition* (Access Games / Rising Star Games, Xbox 360, 2010), directed by Hidetaka "Swery" Suehiro.** The original Xbox 360 game is converted into a regular Windows program — once, at build time — so it runs on your PC the same way as any other Windows app.

If you've used Xenia or RPCS3 before, this is **not** that. There is no emulator, no JIT translator running on every CPU instruction, no per-frame interpretation overhead. The PowerPC code in the original Xbox 360 binary is translated into native x86-64 C++ code ahead of time, and then linked against a small host runtime that handles the Xbox-specific parts (input, kernel calls, GPU command processor, EDRAM). The result is a real `deadlyprem.exe` that boots like any other game.

This technique is called **static recompilation**. It's the same idea behind:
- [N64: Recompiled](https://github.com/Mr-Wiseguy/N64Recomp) (the project that started the trend),
- [Sonic Mania: Recompiled](https://github.com/SonicMania-Recompiled/Sonic-Mania-Recompiled),
- [Skate 3 Recomp](https://github.com/Sergeanur/Skate3Recomp),
- [DownpourRecomp](https://github.com/LittleBitUA/DownpourRecomp) (the same author's *Silent Hill: Downpour* port).

DPRecomp uses the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) — a Xenia-derived Xbox 360 host runtime — as the foundation, and adds the *Deadly Premonition*-specific glue plus the SDK-level fix for the rainbow-noise GPU artifact that used to appear on hair, foliage, and alpha-tested edges.

> [!NOTE]
> **You provide your own legally-owned copy of the game.** The release zip is the host shell only (~16 MiB). It does not contain `default.xex`, game data, music, or any Access Games / Rising Star assets. See [What you need before playing](#what-you-need-before-playing).

---

## Why does this exist?

*Deadly Premonition* is one of those rare cult-classic games — a 2010 Xbox 360 open-world horror / detective oddity that nothing else plays quite like. Twin Peaks vibes, a sharp script, and absurdly broken on PC.

The official PC release, *Deadly Premonition: The Director's Cut*, is one of the most notoriously broken AAA-budget PC ports ever shipped:

- **720p resolution cap**, no fix in settings.
- **Hard 30 FPS lock** that the engine can't even hit half the time.
- **Broken mouse-look** that requires third-party fixers to be playable.
- **Save/load corruption** in specific chapters.
- **A long tail of crashes** that the publisher never patched.

The Xbox 360 release, by contrast, was *the* stable version of the game. Most "best way to play DP" guides for years recommended that path.

**DPRecomp takes that stable Xbox 360 binary and runs it natively on Windows.** External testers have completed full end-to-end playthroughs (driving sequences, all chapters, every cutscene) with **no crashes** — the recompiled game **does not crash and runs more stably than the official PC port**. Because the game logic runs natively, the bugs that plague *Deadly Premonition* under Xenia (chapter-1 hardlocks, broken dialogue advance) also don't apply here.

GPU output uses the upstream Xenia D3D12 stack ported into ReXGlue, so visuals match xenia-canary — with the rainbow-noise artifact on hair/foliage **fixed at the SDK level** ([investigation log](docs/gpu-rainbow-noise.md)).

---

## Comparison: official PC port vs Xenia vs DPRecomp

| | Official PC (Director's Cut) | Xenia emulator | **DPRecomp** |
| --- | --- | --- | --- |
| **Resolution** | 720p cap | up to 4K (DSR) | up to 4K, **native 1080p default** |
| **Frame rate** | 30 FPS lock, often misses | 30 FPS (engine cap) | 30 FPS (engine cap) |
| **Stability** | Frequent crashes | Chapter-1 hardlocks, dialogue advance bugs | **No reported crashes in full playthroughs** |
| **Mouse-look** | Broken — third-party patcher needed | Not supported (controller only) | **Working natively** — Director's Cut bindings preconfigured |
| **Keyboard bindings** | Limited, broken on remap | n/a | Full PC-style Director's Cut bindings, remappable |
| **Save/load** | Corruption in some chapters | Works under Xenia | **Working** |
| **GPU rendering** | Custom (broken) | Xenia D3D12 / Vulkan | Xenia D3D12 backend (ported) |
| **Rainbow noise on hair/foliage** | n/a | known artifact | **fixed** ([log](docs/gpu-rainbow-noise.md)) |
| **CPU execution** | Native (broken port) | Dynamic recompiler (JIT) | **Statically recompiled to native x86-64** |
| **Install size** | ~6 GB + game | ~80 MiB + your dump | ~50 MiB + your dump |

---

## Screenshots

<div align="center">

![Red Room — Prologue, after the GPU artifact fix](docs/screenshots/red_room.png)

</div>

---

## What you need before playing

The download is **the application only**. You bring the game. To play, you need:

1. **A legally-owned copy of *Deadly Premonition* for Xbox 360** — disc, digital download, or backup of either.
2. **Your own dumped `default.xex`** extracted from that copy.
3. **The full game data tree** — `nxeart`, `updata/`, and the rest of the disc's files. The game streams content from disk at runtime, so the XEX alone is not enough.
4. A modern Windows PC: Windows 10 or 11 (x86-64), a D3D12-capable GPU.

> [!IMPORTANT]
> Do not ask in the issue tracker or anywhere else where to get the XEX. Bring your own legally-acquired copy.

---

## How to install and play

1. **Download** the latest release zip: [DPRecomp v0.1.1 →](https://github.com/LittleBitUA/DPRecomp/releases/latest)
2. **Extract** the zip somewhere with read/write access (e.g. `C:\Games\DPRecomp\`).
3. **Put your game files** into an `assets/` folder next to `deadlyprem.exe`. The expected layout:

   ```
   C:\Games\DPRecomp\
     deadlyprem.exe
     rexruntimerd.dll
     TracyClientrd.dll
     deadlyprem.toml       ← rename from deadlyprem.toml.sample
     start.bat
     CONTROLS_EN.txt
     assets\
       default.xex         ← your XEX, from your dumped copy
       nxeart
       updata\
       ...                 ← full disc data tree
   ```

4. **Rename** `deadlyprem.toml.sample` → `deadlyprem.toml`. It already enables mouse mode and ships PC-style Director's Cut bindings (no further config needed).
5. **Double-click `start.bat`**, or open a terminal and run:

   ```powershell
   deadlyprem.exe --game_data_root assets
   ```

6. The game launches in fullscreen. Press **F4** in-game for the **settings overlay** (cvars, keybinds, mouse sensitivity, render scale). Press **`** (backtick) for the **console**.

That's it — you're playing *Deadly Premonition* natively on PC.

---

## Default controls (PC Director's Cut style)

| Action | Key |
| --- | --- |
| Move | `W` `A` `S` `D` |
| Camera | Mouse |
| Interact / fire | `E` / `LMB` |
| Run | `Shift` |
| Light on/off | `F` |
| Strafe left / right | `Z` / `X` |
| Hold breath / lock on | `Ctrl` |
| Look / draw weapon | `Space` |
| Reload / cancel | `R` |
| Map | `M` |
| Pause | `Enter` |
| Switch weapon | Mouse wheel |
| Settings overlay | `F4` |
| Console | `` ` `` |

See [`CONTROLS_EN.txt`](https://github.com/LittleBitUA/DPRecomp/releases/latest) inside the release zip for the full mapping. All bindings are remappable live via F4 → Input → Keybinds.

---

## Frequently asked questions

<details>
<summary><b>Is this an emulator?</b></summary>

No. An emulator runs the original Xbox 360 instructions on a virtual CPU at runtime. DPRecomp converts the Xbox 360 instructions into native x86-64 code at build time, so what you run on your PC is a real Windows executable.

</details>

<details>
<summary><b>Why isn't the game executable included?</b></summary>

Including the game's binary or any of its data files would be copyright infringement. You need to obtain the XEX and game data from your own legally-owned copy of the Xbox 360 release. We will not tell you where to download a copy, and asking will get your issue closed.

</details>

<details>
<summary><b>Is this better than the Steam Director's Cut?</b></summary>

In every practical way relevant to playing the game, yes:

- **Stability**: full external playthroughs with no crashes. The Steam release crashes frequently and was never properly patched.
- **Resolution**: any resolution your GPU can handle. The Steam release is capped at 720p.
- **Input**: native mouse and keyboard with smoothing, fully remappable. The Steam release has broken mouse-look.
- **Saves**: working. Some chapters corrupt saves in the Steam release.

The Steam release does have one thing this doesn't: official online distribution. If you don't already own a Xbox 360 copy, you can buy *Director's Cut* on Steam and use **that** XEX is not possible — they're different binaries — but Steam is at least an easy legal way to own *some* copy of the game while you find the Xbox 360 version.

</details>

<details>
<summary><b>Does the controller work?</b></summary>

Yes — Xbox 360, Xbox One/Series, DualShock 4, DualSense, and most XInput-compatible controllers work out of the box. DualSense was tested by the maintainer.

</details>

<details>
<summary><b>Can I unlock the frame rate above 30 FPS?</b></summary>

Not in v0.1.1. The 30 FPS cap is inside the game's UE3-ish tick loop in the recompiled code. Lifting it would require patching the recompiled game itself to decouple logic tick rate from vblank — raising the host present rate alone makes gameplay run at 2x real speed (animations, physics, scripts all fast-forward), not 60 FPS.

</details>

<details>
<summary><b>Do achievements unlock?</b></summary>

No. There is no Xbox Live backend in this port, so any code path that submits an achievement is stubbed. Saves work; achievements don't.

</details>

<details>
<summary><b>I'm getting a crash / artefact / weird behaviour. What do I do?</b></summary>

Open an issue on [GitHub Issues](https://github.com/LittleBitUA/DPRecomp/issues) with:

- The exact symptom and the scene where it happens.
- A screenshot or short video if visual.
- The `logs/` folder next to `deadlyprem.exe`.
- Your `deadlyprem.toml`.
- GPU model and driver version.

Do **not** attach any game files or binaries that link against game data.

</details>

---

## Building from source

<details>
<summary><b>Click to expand — full build instructions</b></summary>

### Prerequisites

- Windows 11
- Visual Studio 2022 Build Tools with the C++ workload (or full IDE)
- LLVM/Clang 20 or newer
- CMake 3.25 or newer
- Ninja 1.11 or newer
- A built and installed [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), registered in CMake's user package registry. The SDK must be built in the same configuration the consumer project uses (Debug or RelWithDebInfo).

### Provide the game

Drop the contents of your Xbox 360 disc dump into `assets/`:

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

Codegen produces ~4.45M lines of recompiled C++ in `generated/default/`. First build takes 20-30 minutes; incremental builds after config tweaks are much faster.

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

### Discovering missing functions

When the runtime fatals with `Call to invalid or unregistered function at guest address 0xADDR`, add an entry under `[functions]` in `deadlyprem_config.toml`:

```toml
"0xADDR" = { name = "sub_ADDR" }
```

For batch discovery, use [`sp00nznet/360tools`](https://github.com/sp00nznet/360tools):

```powershell
python tools/extract_pe.py assets/default.xex generated/dp_pe.bin
python tools/find_missing_vtable_funcs.py generated/dp_pe.bin generated/default/deadlyprem_init.cpp
```

The scanner output is paste-compatible with `[functions]` after a trivial case fix (`0X` → `0x`).

</details>

---

## Technical deep-dives

- 📜 [**GPU rainbow-noise investigation log**](docs/gpu-rainbow-noise.md) — the full forensic trail of how the EDRAM ownership-transfer artifact on hair / foliage / alpha-tested edges was found and fixed, including 7 disproved hypotheses kept as "do not re-bisect" notes.

---

## Project structure

- `deadlyprem_manifest.toml` — top-level ReXGlue manifest; points at the XEX and pulls in `deadlyprem_config.toml`.
- `deadlyprem_config.toml` — codegen hints: manually-registered functions, templates for switch tables and midasm hooks.
- `src/deadlyprem_app.h` — `rex::ReXApp` subclass; installs the FP exception guard at start, removes it at shutdown.
- `src/deadlyprem_fp_guard.h` — VEH (Windows) / SIGFPE (POSIX) handler that masks SSE FP exceptions raised by the recompiled guest code.
- `src/deadlyprem_hooks.cpp` — bodies for any named functions and midasm hooks declared in the TOML.
- `src/main.cpp` — `REX_DEFINE_APP` entry point.

---

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) — the recompilation toolkit.
- [EternalSonataReprise](https://github.com/birabittoh/EternalSonataReprise) — the host-glue template that this project's `src/` follows, including the FP exception guard pattern.
- [`sp00nznet/360tools`](https://github.com/sp00nznet/360tools) — Python scanners for batch vtable / switch-table / import discovery.
- [Xenia project](https://github.com/xenia-canary/xenia-canary) — the upstream GPU stack that ReXGlue's `src/graphics/` ports in.
- [Weighted Coils](https://www.youtube.com/@WeightedCoils) — testing and end-to-end playthrough validation.

---

## Legal

The host-side source under `src/`, build scripts, CMake files, TOML configs, and documentation are released under the **MIT License** — see [LICENSE](LICENSE).

The recompiled game code produced at build time contains symbols and logic from *Deadly Premonition* and is **not** redistributable. Do not share `assets/default.xex`, the `generated/default/` directory, or any built binary that links against them.

---

<div align="center">

**Related projects by the same author:**
[DownpourRecomp — Silent Hill: Downpour (PC port)](https://github.com/LittleBitUA/DownpourRecomp)

</div>
