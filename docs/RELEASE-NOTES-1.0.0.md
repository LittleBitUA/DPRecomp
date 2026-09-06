## 🎉 Deadly Premonition Recompilation 1.0

A complete restart of the project on the current ReXGlue 0.10 nightly. Everything from v0.1.1 was rebuilt: runtime, GPU plugin, input, launcher. If you have v0.1.1, delete it and unzip 1.0 fresh.

**Both the European (PAL) and the USA disc are supported.** The launcher looks at your `default.xex` and starts the matching build automatically.

### ⚡ Playability
- **60 FPS** with correct game speed (port of ehw's patch, #3). Off switch in the launcher and in the F4 overlay.
- **Real mouse look** — mouse motion drives the game's camera code directly (walking, aiming, driving, cutscenes). No stick emulation, no dead zone. Sensitivity and invert-Y in the launcher.
- **Director's Cut keyboard layout**, combos (`Space + LMB` = fire while aiming), mouse-wheel weapon switch, everything rebindable.
- **DualSense adaptive triggers**; Xbox / DualShock / any SDL pad work alongside the keyboard.
- **Saves and shader cache next to the game** (`userdata\`), fixing the "save sequence never ends after a reboot" soft-lock of v0.1.1 (#6).

### 🖼️ Graphics
- 2× supersampling (2560×1440 internal) on the fast **ROV** render path, with the rainbow-noise fix now working on ROV too.
- **AMD FSR 3** (Native AA) + FXAA by default, sharpness adjustable; bilinear / CAS / FSR 1 selectable.
- No geometry pop-in while shaders compile; shader/pipeline storage pre-warmed at start (shipped in the zip).

### 🧰 Convenience
- **Launcher** with every setting, English / Ukrainian UI, config self-healing and **auto-update from GitHub**.
- **Built-in installer**: press PLAY with no game data and pick your `.iso` — it extracts itself and starts the right regional build (installer by Alexbeav, from the Downpour project).
- Portable: unzip anywhere, nothing written to your profile.
- Runs on older CPUs (SSE4.1 baseline, no AVX2 requirement).

### 🚫 What this does NOT include
- Achievements on Xbox Live (tracked locally by the overlay only).
- Linux.
- A fix for the flat clouds in some outdoor scenes (#10) — still open.
- At 2× internal resolution some low-res post effects (glow, depth of field) can show a fine grid on bright edges; 1× is clean. Being investigated.

### 🔄 Auto-update
Future releases install in place from the launcher. This 1.0 zip must be installed by hand (v0.1.1 had no compatible updater).

### 📦 Files
`DPRecomp-1.0.0-win64.zip` — launcher, both game builds, runtime, empty `assets\` for your game files, default config, controls reference, pre-warmed shader storage.

### 🙏 Credits
[ehw](https://github.com/ehw/game-patches) (60 FPS patch), [Alexbeav](https://github.com/Alexbeav) (disc installer), [ReXGlue](https://github.com/rexglue/rexglue-sdk) / [Xenia](https://xenia.jp/), [DPfix](https://github.com/PeterTh/dpfix), and everyone who reported issues against v0.1.1.

— «Little Bit»
