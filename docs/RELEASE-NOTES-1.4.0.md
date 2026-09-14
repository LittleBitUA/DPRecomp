## Deadly Premonition Recompilation 1.4.0

Two requests from the wishlist thread (#19), both switched off by default. Installs in place from the launcher (Update button) or unzip over any earlier version; saves and settings are kept.

### ⏭️ Skip Intro (launcher → Advanced → Skip Intro)
- **Skip the logos:** the game boots straight to the title screen instead of the four publisher logos.
- **Skip the logos and press Start:** it also presses Start for you and lands in the main menu. The press goes through the game's own input path, so the game still picks whichever controller you use.
- How it works: the title screen is a state machine inside the game; the port moves it from the first logo to the title fade-in, exactly what the last logo does when its timer runs out. Nothing is patched in the game files.
- One thing to know: the logos used to hide the game's start-up loading. With the skip on, the title scene loads while the packs are still streaming, so on a cold start you get a few seconds of black (up to about 20 s on a slow disk) instead of the Dolby screen. Never longer than the logos took; warm starts are near-instant.

### 🎮 Controller Layout (launcher → Controls → Controller Layout)
- **Original:** the Xbox 360 game as shipped (right trigger aims, A fires, left trigger holds breath).
- **Director's Cut:** left trigger aims, right trigger fires, A while aiming holds breath. Outside aiming the right trigger does what the left one did (breath, lock-on, trigger prompts), so nothing becomes unreachable. Menus and interaction keep A. A press keeps its meaning until you release it, so lowering the weapon never fires a stray action.
- Driving is never remapped (the car stays on RT/LT), and keyboard bindings are not touched: this is for pads only.
- Tell us in #19 if anything in that layout feels off; the port's team plays on keyboard and mouse most of the time.

### 🔧 Under the hood
- Runtime: a per-device pad state filter and a synthetic button press API (`rex/input/state_filter.h`), unit tested.
- Game: the intro skip state machine (`src/deadlyprem_title_skip.h`) and the pad remap (`src/deadlyprem_pad_layout.h`) are pure code with their own test executables. `dp_title_state_log` logs every title state transition.
- Tracy profiler client and a per-frame counter CSV can be switched on for performance work (`tracy_enable`, `perf_log_csv`); the GPU plugin logs each distinct EDRAM resolve and HDR-relevant texture fetch once (`gpu_hdr_log`).

Everything from 1.3.7 is included.
