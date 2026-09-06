<div align="center">

# Deadly Premonition Recompilation

### Play *Deadly Premonition* natively on Windows: 60 FPS, real mouse look, a launcher, DualSense triggers, FSR 3 — no emulator required.

[![Latest release](https://img.shields.io/github/v/release/LittleBitUA/DPRecomp?style=for-the-badge&label=Download&color=blue)](https://github.com/LittleBitUA/DPRecomp/releases/latest)
[![Total downloads](https://img.shields.io/github/downloads/LittleBitUA/DPRecomp/latest/total?style=for-the-badge&color=brightgreen)](https://github.com/LittleBitUA/DPRecomp/releases/latest)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-0078D6?style=for-the-badge&logo=windows)](https://github.com/LittleBitUA/DPRecomp/releases/latest)
[![Stars](https://img.shields.io/github/stars/LittleBitUA/DPRecomp?style=for-the-badge&color=yellow)](https://github.com/LittleBitUA/DPRecomp/stargazers)

![York in the rain — Deadly Premonition Recompilation 1.0](docs/screenshots/york.jpg)

## [⬇  Download 1.0 for Windows](https://github.com/LittleBitUA/DPRecomp/releases/latest)

**by «Little Bit»**

</div>

---

> [!IMPORTANT]
> **1.0 is a full restart of the project.** The old v0.1.1 preview was built on an outdated SDK and is superseded in every respect: new runtime, new GPU plugin, new launcher, new input. If you still have v0.1.1, delete it and start fresh — the launcher, the config file and the save location have all changed.
>
> **Region:** the **European (PAL) Xbox 360 disc** is supported right now. **A USA (NTSC) build is being prepared** — see [FAQ](#frequently-asked-questions).

---

## Table of contents

- [What is this?](#what-is-this)
- [What's new in 1.0](#whats-new-in-10)
- [Screenshots](#screenshots)
- [What you need before playing](#what-you-need-before-playing)
- [How to install and play](#how-to-install-and-play)
- [The launcher](#the-launcher)
- [Default controls](#default-controls)
- [Frequently asked questions](#frequently-asked-questions)
- [Known issues](#known-issues)
- [Building from source](#building-from-source)
- [Credits](#credits)
- [Legal](#legal)

---

## What is this?

**Deadly Premonition Recompilation is a native Windows port of *Deadly Premonition* (Access Games / Rising Star Games, Xbox 360, 2010), directed by Hidetaka "Swery" Suehiro.** The Xbox 360 executable is converted into a regular Windows program once, at build time, by *static recompilation*. What you run is a real x86-64 `.exe` — there is no emulator, no JIT and no per-instruction interpretation.

It is the same technique as [N64: Recompiled](https://github.com/Mr-Wiseguy/N64Recomp), [Skate 3 Recomp](https://github.com/Sergeanur/Skate3Recomp) and the author's own [Silent Hill: Downpour port](https://github.com/LittleBitUA/DownpourRecomp). The project is built on the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (a Xenia-derived Xbox 360 host runtime), plus a set of game-specific fixes and hooks that live in this repository.

> [!NOTE]
> **You provide your own legally-owned copy of the game.** The release zip is the host shell only. It does not contain `default.xex`, game data, music or textures. See [Legal](#legal).

---

## What's new in 1.0

Everything below is new compared with v0.1.1.

### Playability
- **60 FPS.** The game logic is unlocked from the 30 FPS vblank gate (a port of [ehw's Xenia patch](https://github.com/ehw/game-patches), issue #3), with the in-game timer kept at real time. Can be switched off in the launcher (Graphics → *60 FPS (ehw patch)*) or live in the in-game settings overlay.
- **Real mouse look.** The mouse no longer emulates an analog stick. Mouse motion is fed straight into the game's camera code (walking, aiming, driving and cutscene cameras), so there is no dead zone, no acceleration curve and no "stick" feel. Sensitivity and Y-inversion are in the launcher (Mouse tab) and in the overlay.
- **Keyboard layout of the Director's Cut PC version**, including combos (`Space + LMB` to fire while aiming) and mouse-wheel weapon switching. Every key is rebindable.
- **DualSense adaptive triggers** (PS5 controller): weapon click on the right trigger, resistance on the left, all configurable. Xbox, DualShock 4 and any XInput / SDL pad work out of the box.
- **Saves and the shader cache live next to the game** (`userdata\`), not in Documents. That also removes the OneDrive-related "save sequence never ends" soft-lock reported against v0.1.1 (issue #6).

### Graphics
- **2× supersampling (2560×1440 internal) on the fast ROV render path** — and the rainbow-noise fix for hair, foliage and glow now works on ROV as well (v0.1.1 only fixed the slow RTV path).
- **AMD FSR 3 with Native AA + FXAA** as the default presenter, sharpness adjustable, or plain bilinear / CAS / FSR 1 if you prefer.
- **No pop-in while shaders compile:** draws wait for their pipeline instead of being dropped (`block` policy), and shader/pipeline storage is pre-warmed at start.
- Anisotropic filtering, window size and monitor selection, VRR/tearing switch.

### Convenience
- **A launcher** (`PlayDeadlyPremonition.exe`): every setting in a UI (English / Ukrainian), config self-healing, **automatic updates from GitHub releases**.
- **First-run installer:** if the game data is missing, the game asks for your `.iso` and extracts it itself (Redump / XGD3 / bare partition images). Contributed to the Downpour project by [Alexbeav](https://github.com/Alexbeav) and ported here.
- Portable layout: unzip anywhere, nothing is written to your user profile.
- Runs on older CPUs: the binaries are built for the SSE4.1 baseline (no AVX/AVX2 requirement).

### Under the hood (for the curious)
- ReXGlue **0.10 nightly** with a set of local SDK changes: the 7e3 → 8888 EDRAM ownership fix for ROV (in-place compute conversion), the PSO wait policy, once-per-constant logging for invalid texture fetch constants, the mouse camera API, the overlay build stamp.
- Game hooks are done as *mid-asm hooks* generated by the recompiler (see `deadlyprem_config.toml`), so they survive regeneration.

---

## Screenshots

All captured from the 1.0 build at 2× internal resolution.

| | |
|---|---|
| ![Intro](docs/screenshots/intro.jpg) | ![Main menu](docs/screenshots/main_menu.jpg) |
| ![Load game](docs/screenshots/load_game.jpg) | ![Menu](docs/screenshots/menu.jpg) |
| ![Achievement](docs/screenshots/achievement.jpg) | ![York](docs/screenshots/york.jpg) |

---

## What you need before playing

1. **A legally-owned copy of *Deadly Premonition* for Xbox 360 — the European (PAL) release** — as a disc image (`.iso`) or as already extracted files (`default.xex`, `nxeart`, `updata`, …). The USA disc does not work yet (different executable, see FAQ).
2. **Windows 10 or 11**, 64-bit.
3. **A GPU with Direct3D 12 and rasterizer-ordered views** (NVIDIA GTX 900+ / RTX, AMD RX 400+, Intel Arc). Integrated graphics from the last few years also work, at 1× internal resolution.
4. ~6 GB of free disk space for the extracted game data.

---

## How to install and play

1. Download the latest release zip and **unzip it anywhere** (not inside *Program Files*).
2. Put your game data into the `assets` folder next to `deadlyprem.exe`:
   - **either** drop your `.iso` anywhere and start the game — the built-in installer asks for the image and extracts it into `assets` (takes a few minutes, resumable);
   - **or** copy the already-extracted disc contents (`default.xex` must end up at `assets\default.xex`).
3. Start **`PlayDeadlyPremonition.exe`** and press **PLAY**. The first launch compiles a few shaders; later launches are instant.

Saves are stored in `userdata\` next to the game. To move or back up your progress, copy that folder.

---

## The launcher

`PlayDeadlyPremonition.exe` is the recommended way to start the game. It:

- shows every setting in five tabs — **Graphics** (render path, internal resolution, upscaler, sharpness, FXAA, 60 FPS, window / monitor), **Advanced** (game language, GPU adapter, texture cache, audio), **Mouse** (direct camera control, sensitivity, invert), **Controls** (DualSense triggers, every key binding), **Debug** (log level, PSO policy);
- writes `deadlyprem.toml` next to the game and repairs keys that the in-game overlay may drop;
- checks GitHub for a newer release on start and installs it in place with one click, keeping your config and saves.

Inside the game, **F4** opens the runtime settings overlay (live changes, `Save to config` writes them back), **F3** shows the performance overlay, **F7** shows your achievements.

---

## Default controls

Mirrors the Director's Cut PC keymap. A controller works at the same time.

| Action | Keyboard / mouse | Pad |
|---|---|---|
| Move | `W` `A` `S` `D` | Left stick |
| Look | Mouse | Right stick |
| Change weapon | Mouse wheel | D-pad up / down |
| Interact / accept | `E` | A |
| Cancel / reload | `R` | B |
| Observe | `C` | Left stick press |
| Flashlight | `F` | Y |
| Cutscene actions | `Shift` | X |
| Draw weapon / aim | hold `Space` | Right trigger |
| Fire | `Space` + `LMB` | A while aiming |
| Hold breath / lock-on | `Control` | Left trigger |
| Strafe left / right | `Z` / `X` | LB / RB |
| Pause menu | `Enter` | Start |
| Map | `M` | Back |

Rebind anything in the launcher (Controls tab) or in `deadlyprem.toml`. Combo syntax: `,` separates alternatives, `+` means "held together" (`keybind_a = "E,Space+LMB"`).

---

## Frequently asked questions

<details>
<summary><b>Is this an emulator?</b></summary>

No. An emulator runs Xbox 360 instructions on a virtual CPU at runtime. Here the instructions were converted to native code once, at build time. The GPU is still *translated* (Xenos → Direct3D 12) by the ReXGlue runtime, which is why the settings overlay looks familiar to Xenia users.
</details>

<details>
<summary><b>Does the USA (NTSC) version work?</b></summary>

Not yet. The USA disc ships a different executable (`XThread::Execute - No function registered at 824E9558` is the symptom), so it needs its own recompilation. **A USA build is in progress** and will be released as a separate download.
</details>

<details>
<summary><b>Is 60 FPS safe?</b></summary>

It changes the game's time base rather than just doubling the frame rate, so animation, physics and the clock run at normal speed. It has been played through large parts of the game without problems, but if you ever see something time-related misbehave, switch it off (Graphics → *60 FPS (ehw patch)*) and please open an issue with the location.
</details>

<details>
<summary><b>The mouse feels different from a PC shooter.</b></summary>

Mouse motion drives the game's own camera, which is a spring-damped orbit camera by design — it always eases towards where you point. Tune *Camera Hook Sensitivity* in the launcher (or `dp_mouse_camera_sensitivity` in F4 → DP1, live). Turning *Mouse Controls Camera Directly* off falls back to stick emulation.
</details>

<details>
<summary><b>Where are my saves?</b></summary>

`userdata\` next to `deadlyprem.exe`. v0.1.1 kept them in `Documents\deadlyprem`; they are not migrated automatically (the old preview was not save-compatible in practice), start fresh.
</details>

<details>
<summary><b>Does it run on my old CPU?</b></summary>

The build targets the x86-64 baseline plus SSSE3/SSE4.1 — any Intel Core from 2008 or AMD from Bulldozer (2011) onwards. No AVX or AVX2 is required.
</details>

<details>
<summary><b>Something crashed / looks wrong. What do I do?</b></summary>

Open an issue with: your GPU and driver, whether the render path is ROV or RTV, the internal resolution, a screenshot, and the newest file from `logs\`. Please try `Internal Resolution Scale = 1x` and `Render Target Path = RTV` first and say whether that changed anything.
</details>

---

## Known issues

- At 2× internal resolution some low-resolution post-process effects (glow, depth of field) can show a fine grid on bright edges; 1× does not have it. Being investigated.
- Clouds in some outdoor scenes render as flat blotches (issue #10).
- Achievements are tracked locally (list and unlock toasts on **F7**); there is no Xbox Live.
- Linux is not supported yet.

---

## Building from source

<details>
<summary><b>Click to expand</b></summary>

1. Install Visual Studio 2022 Build Tools, LLVM/Clang 18+, CMake 3.25+ and Ninja.
2. Build and install the bundled ReXGlue SDK (0.10 nightly, `development` branch with the local patches from `docs/sdk-patches`): `cmake --preset win-amd64 -DREXGLUE_ENABLE_FIDELITYFX=ON`, then build and install the `Release` and `RelWithDebInfo` configurations.
3. Put your PAL `default.xex` into `assets\` and run the recompiler: `rexglue codegen deadlyprem_manifest.toml` (about 10 seconds; the function list and the mid-asm hooks live in `deadlyprem_config.toml`).
4. Configure with `cmake --preset dp-relwithdebinfo` (uses `CMakeUserPresets.json` to point at your SDK install) and build. The default build is the compatibility (SSE4.1) build; `-DDP_ENABLE_X86_64_V3=ON` enables AVX2.
5. The launcher is a separate CMake project in `launcher\` (Visual Studio generator).
</details>

---

## Credits

- **«Little Bit»** — the port, the launcher, the SDK fixes.
- **[ehw](https://github.com/ehw/game-patches)** — the 60 FPS patch (originally for Xenia), ported to the PAL executable.
- **[Alexbeav](https://github.com/Alexbeav)** — the first-run disc image installer (contributed to DownpourRecomp, ported here).
- **[ReXGlue](https://github.com/rexglue/rexglue-sdk)** and **[Xenia](https://xenia.jp/)** — the runtime this port stands on.
- **[DPfix](https://github.com/PeterTh/dpfix)** by Peter Thoman — invaluable notes on how the game renders.
- Everyone who tested v0.1.1 and filed issues — the USA-region reports, the save soft-lock, the 60 FPS request all shaped 1.0.

---

## Legal

This repository contains no game code, assets or data. You must own *Deadly Premonition* for Xbox 360 and provide your own disc image or extracted files. *Deadly Premonition* is © Access Games / Rising Star Games / Marvelous. This project is not affiliated with or endorsed by them, Microsoft, or the ReXGlue authors.
