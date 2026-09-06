## Deadly Premonition Recompilation 1.2

Keyboard and PlayStation button prompts, texture modding, and the fixes reported after 1.1. Installs in place from the launcher (Update button) or unzip over 1.0 / 1.1; saves and settings are kept.

### 🎨 Button prompts that match what you play on
- **Keyboard prompts.** The game draws its button hints from one small atlas (A / B / X / Y, sticks, LB / RB / LT / RT, L3 / R3). With Mouse & Keyboard Mode on, the launcher paints key caps with *your* bindings over that atlas — **E** Save, **R** Observe, **SHIFT** run, **SPACE** aim, **CTRL** for LT — in the game's own font (Cinema Calligraphy). Change a binding, the pictures follow. Stick-shake prompts show **A D**; mouse prompts show a mouse with the wheel highlighted.
- **PlayStation prompts** for DualShock / DualSense players: ✕ ○ □ △, L1 / R1 / L2 / R2, L3 / R3, stick glyphs — three looks (solid, solid with ring, outline). Icons: *PS5 Button Icons and Controls* by **Zacksly** (CC BY 3.0, https://zacksly.itch.io).
- Launcher → Controls → **Button Prompts**: Keyboard / Xbox (original) / PlayStation ×3. The icon sets are plain PNGs in `prompts\`; drop another folder with the same file names to add your own.

### 🧩 Texture replacement and dumping (modding)
- Every texture the game loads is identified by a hash of its pixels. `textures\<hash>.png` replaces it (any size, mipmaps generated at load); `textures\<hash>.overlay.png` is composited over the original where the PNG is opaque, so you only ship the pixels you changed.
- Launcher → Advanced → **Texture Dump** writes every loaded texture to `textures\dump\<hash>_<w>x<h>_<format>.png` while you play (off by default; DXT1/3/5 and RGBA8 formats).
- The title logo now reads *Deadly Premonition Recompilation* — done with exactly this mechanism (`textures\6E42BC7CF738BCF0.png`).

### 🖱️ Mouse and keyboard
- **Stick-shake QTEs ("Get it off!")**: tapping A > D > A > D rapidly now drives a full-amplitude shake for 300 ms after each tap (`mnk_tap_shake_window_ms` / `mnk_tap_shake_hold_ms`), and taps shorter than one poll are no longer lost. 1.1's ramp alone left short taps far from the stick extremes.
- **Map zoom with the mouse wheel**: outside gameplay cameras (menus, the map) each wheel notch also holds the right stick up / down for 120 ms (`mnk_wheel_rstick_ms`). Moving the mouse forward / back zooms too.
- **Mouse sensitivity while running**: the direct camera write now works in every gameplay camera routine (walk, run and the other absolute-camera modes), not only while walking.
- **Menus and the map**: the mouse falls back to the right stick whenever no gameplay camera is running, instead of banking a camera snap for later.

### ⏱️ 60 FPS (#12, follow-up)
- 1.1 never let the logic tick go below 1.0, so frames released at 16.0 ms by the game's millisecond-granularity limiter ran the game about 4 % faster than real time — the animation got ahead of the voices. `dp_60fps_tick_min` (default 0.5) lets the tick follow real time in both directions; 1.0 restores the 1.1 behaviour.

### 🪟 Window, launcher, misc
- **Alt+Tab** back into the fullscreen game no longer leaves the taskbar on top: the window is held above the taskbar while it has focus and released when focus leaves.
- **Monitor** is a drop-down of your actual displays instead of a 0–16 slider.
- Full Ukrainian translation of every 1.1 setting, shorter labels that fit, clearer Debug-tab wording.
- `gpu_allow_invalid_fetch_constants` is on: the game binds several real textures (the 2048×1024 sky panorama, the 1024×576 frame copy, bloom buffers) with an "invalid" fetch type that was previously skipped.
- Both game builds and the launcher report 1.2.

### Known issues
- The stair-stepped edge in the sky around the sun (the sun glow is rendered in a low-resolution buffer) and the soft bloom / DoF look at 2× internal resolution are the same class of problem: post-process passes sized for 1× being scaled. A "resolution scale threshold" (small render targets stay unscaled) is the planned fix for 1.3. Set Internal Resolution to 1× if it bothers you.
- Clouds still have hard edges (#10).
- Steam black screen (#13) is still being diagnosed with the reporter.
