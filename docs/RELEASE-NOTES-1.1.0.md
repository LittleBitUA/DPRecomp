## Deadly Premonition Recompilation 1.1

Fixes for the three reports that came in right after 1.0. Installs in place from the launcher (Update button) or unzip over 1.0; saves and settings are kept.

### 🎮 Mouse (#11)
- **Real position control.** In 1.0 the mouse only "nudged" the camera target once per frame; the game's camera spring consumed a fraction of it and forgot the rest, so the camera felt slow and frame-rate dependent. 1.1 keeps the angle the camera has not reached yet pending until it gets there, and also turns the camera yaw state directly, so a mouse move is a fixed angle and lands immediately.
- The game's own auto-centering behind York resumes only after the mouse has been idle (`dp_mouse_camera_hold_ms`, 120 ms by default). Sensitivity is now in radians per pixel; the default changed from 0.015 to 0.003 (about one full turn per 2100 px). Both, plus the direct / catch-up switches, are in the launcher under Mouse and live in the F4 overlay.
- **Aiming with the mouse.** The aim camera (Space held) is a different routine that reads the *left* stick with its own acceleration, which is why 1.0 aimed with WASD only. 1.1 detects that camera and feeds the mouse into the left stick while it runs, so you aim with the mouse; WASD still works there too.
- **Stick-shake QTEs ("Get it off!") on a keyboard**: the keyboard stick now travels to full deflection in 60 ms and back like the PC port (`mnk_key_stick_ramp_ms`), so tapping A > D > A > D sweeps through the centre and every tap counts; 1.0 jumped between the extremes and missed half of them. Optional auto-shake while holding A + D (off by default).
- **Running**: hold **Shift** (X button). The 1.0 controls sheet wrongly described E as run; E is the A button (interact / confirm). CONTROLS files corrected.

- **Keyboard: holding Shift froze York, LT (Control) did nothing.** A rule in the key driver silenced every plain key while any modifier was held, and a bare modifier bind never fired. Fixed: Shift + WASD runs, Control = LT; the D-pad on Shift + arrows still works.

### 🧩 Shader / PSO cache
- **The shader storage never grew after its first session** (the stream stayed in read mode after loading; every later write was silently dropped). Fixed, so the cache in `userdata\cache\shaders\shareable` now accumulates as you play and the zip ships a fuller one.
- **Pipeline library**: driver-compiled PSO blobs are kept in `userdata\cache\shaders\local\*.pso_lib`, so the second launch skips the driver compiles entirely (Downpour port).
- **Visible progress**: a "Preparing shaders" toast at launch while the cache is replayed, and a small corner badge ("Compiling shaders…" / "Shader stall N ms") whenever new pipelines are built in a new scene. Both in the launcher under Graphics.

### ⏱️ 60 FPS: cutscene audio drift (#12)
- The 60 FPS patch pinned the game's logic tick to exactly 1/60 s regardless of the real frame time, so every frame longer than 16.7 ms (Windows sleep granularity, heavy cutscenes) slowed game time while the streamed audio kept real time. The animations fell behind the voices, most visibly in long scenes.
- 1.1 derives the tick from the measured frame time (a 50 FPS frame advances 1.2 ticks, a 60 FPS frame 1.0), and the runtime now sleeps with a high-resolution timer so the game's own limiter holds 16.7 ms instead of 17–30 ms.

### 🎮 Steam Deck
- The launcher recognises a Steam Deck and applies the community-tested preset once (RTV, 1×, 2× MSAA, 16× AF, FXAA + CAS, 30 FPS, VSync, fullscreen 1280×800); change anything afterwards in Settings.

### 🟦 Steam (#13)
- New launcher setting **Advanced → Steam Overlay** (`off` disables overlay drawing for the game process). Use it if the game shows a black screen or a tinted quarter-size frame when started through Steam as a non-Steam game.

### 💾 Saves and cache really next to the game now
- 1.0 claimed a portable `userdata\` layout, but the runtime kept using `Documents\deadlyprem` for saves and the shader cache (the folder next to the exe was never read, and the pre-warmed cache in the zip was ignored). 1.1 uses `userdata\` next to the exe for real and **copies your existing saves and shader storage from Documents on first launch** (nothing is deleted there).

### 🤝 Share your shader cache (opt-in)
- The launcher asks once on first start whether it may send your shader cache to the project (anonymous: only shader microcode and pipeline descriptions, never saves or personal data). Merged caches ship with the next release so everyone gets fewer first-time stutters. Switch it at any time under Advanced → *Share Shader Cache*.

### 🔢 Versions
- Game builds (PAL and USA), launcher and F3 watermark all report 1.1. The launcher's auto-updater now targets releases ≥ v1.1.0.

— «Little Bit» (Dmytro Bidlov) · 🇺🇦 MADE IN UKRAINE
