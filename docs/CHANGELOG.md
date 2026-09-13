# Changelog

Release notes for each version: [1.3.5](RELEASE-NOTES-1.3.5.md) · [1.3.4](RELEASE-NOTES-1.3.4.md) · [1.3.3](RELEASE-NOTES-1.3.3.md) · [1.3.2](RELEASE-NOTES-1.3.2.md) · [1.3.1](RELEASE-NOTES-1.3.1.md) · [1.3.0](RELEASE-NOTES-1.3.0.md) · [1.2.1](RELEASE-NOTES-1.2.1.md) · [1.2.0](RELEASE-NOTES-1.2.0.md) · [1.1.0](RELEASE-NOTES-1.1.0.md) · [1.0.0](RELEASE-NOTES-1.0.0.md)

## 1.3.5 (September 2026)

Hotfix for the GPU device removal while tailing Nick to the art gallery (#22). The 1.3.3 debug-layer drain named it: a `CopyTextureRegion` in the texture loader wrote a 64×64 mip into a 32×32 level of an R8G8B8A8 texture, the command list failed to close and Direct3D 12 removed the device (`INVALID_CALL`). Both texture copy paths now check the source extent against the destination level, clamp the copy and log the texture (size, format, mips, packed level, box) once, with a `gpu/texture_cache/copies_clamped` gauge in `[stats]`. The frame renders with a partial mip instead of killing the device; the logged texture is the input for the real layout fix. GPU plugin DLL only.

## 1.3.4 (September 2026)

- **Periodic `[stats]` line in the log** (`log_stats_interval`, default 60 s, 0 = off): guest heap usage, kernel object and thread counts, GPU texture / pipeline cache sizes, shared-memory and scaled-resolve buffer use, XMA contexts, audio clients, VRAM budget and usage, process working set, private bytes, handle count and free system memory. Written from its own thread, so it keeps coming while the game is hung (#21) and the last line before a GPU device removal shows the VRAM picture (#22).
- **The game's own error channels reach the log.** PhysX 2.6 error stream (`dp_physx_log`, on): every invalid parameter, skipped call, out-of-memory and assert the physics SDK reports, with file and line, the first 5 of each text then every 100th with a count, and a `game/physx_reports` gauge in `[stats]`. Sound-effect cues (`dp_audio_cue_log`, off): cue id and arguments per trigger, with the frame number.
- **Green / rainbow panels on the ROV path (#23) are now caught in the log.** The ROV path tags every EDRAM tile that holds HDR (7e3) color and converts it in place before 8-bit draws; a resolve that copies an 8-bit render target from tiles still tagged HDR now logs a warning with the frame, EDRAM base and size (that is the green frame). `rov_7e3_track_log` logs every conversion, `rov_7e3_convert_on_resolve` and `rov_8888_full_extent` are the two experiments; `gpu/rov/*` gauges in `[stats]`.
- **Trap instructions carry their address.** A `twi 31,r0,22` (the game's STL / CRT assert, 33 sites) or a conditional trap now logs `at 0x8XXXXXXX, lr 0x...` instead of a bare "trap hit", so a crash that follows one can be tied to a function.
- The experimental `dp_anim_event_dedupe` / `dp_anim_event_log` switches from 1.3.0 are removed: the doubled sounds of #17 / #18 went away with whole ticks in 1.3, and the player logs showed the "triple picks" were different controllers, not a duplicate. A leftover key in `deadlyprem.toml` only logs one "unknown cvar" warning.

## 1.3.3 (September 2026)

- **Steam Deck (#20):** the launcher applies updates itself (a helper copy of the launcher with a built-in zip reader, miniz) instead of a PowerShell script, so the Update button works under Proton.
- **Mouse aiming in the clock-tower boss fight (#24):** holding the aim key hands the mouse to the aim whatever camera runs; the fight's locked camera was not one of the hooked gameplay cameras.
- **GPU device removal (#22):** the Direct3D 12 and DXGI debug queues are drained at the moment of the removal, so the fatal validation message reaches the log.

## 1.3.2 (September 2026)

Diagnostics release, no gameplay changes. The runtime gained a crash handler (exception, module, host registers, the game's thread, PowerPC registers and a guest call stack in the log, plus a minidump in `logs\`), thread names and frame numbers on every log line (the game's own thread names via a hook on its thread wrapper), DRED breadcrumbs on GPU device removal by default (#22), debug-layer messages written into the log when `d3d12_debug` is on, save-container steps logged (#21), `vfs_log_opens`, memory-failure and audio-registration lines, KeBugCheck / RtlRaiseException as errors, honoured "invalid" fetch constants reported once, a build stamp as the first line and a 5-second flush.

## 1.3.1 (September 2026)

Hotfix: all sound stopped a few seconds into some cutscenes (the morgue after examining Anna, the tree profiling, the lumbermill nightmare, #16) while the game kept running. The XMA decoder was a pre-May-2026 snapshot of Xenia-canary's; five upstream fixes to the decoder / audio-engine handshake are ported (early output-buffer invalidation, the last partial frame never being delivered so the game's audio thread waited forever, the stall detector, the packet walk crossing into another sub-stream). Verified on the reporter's save. Runtime DLL only.

## 1.3 (September 2026)

- **The sky, tree crowns and glass edges** on the ROV path: the game's HDR scene format stores 2 bits of alpha and the game draws its sky / fog masks into them, so the console-faithful path showed a stair-stepped sky and hard contours. 1.3 keeps a tag-validated 16-bit shadow alpha next to the EDRAM buffer; blending, resolves and the HDR to 8-bit conversion read it. ROV is the recommended path now.
- **FSR 3 flicker** between consecutive frames and brightness flashes on scene cuts: the upscaler was created in HDR mode with auto exposure and reset every frame. Off now.
- **60 FPS on whole ticks (#12)**: integer ticks with an error accumulator, like the console's vblank count and like the PC port's cutscene timing. Game time tracks real time to four decimals and scripted scenes no longer see fractional ticks (which made them run fast). `dp_60fps_integer_tick`, `dp_60fps_stats`.
- **Doubled sounds / prop swaps at 60 FPS (#17, #18)**: the animation event dispatcher is mapped; an experimental `dp_anim_event_dedupe` switch and a `dp_anim_event_log` are in, off by default.
- Pipeline cache entries are versioned (first launch after the update recompiles once), audio underruns are logged, SDK patches regenerated with the XeSL shader sources.
- Steam artwork pack (vertical capsule, hero, logo) as a second release asset and in `docs/steam`.

## 1.2.1 (September 2026)

The FSR 2 / FSR 3 presenter pass left the wrong descriptor heap bound for the sharpening pass (D3D12 error #708 every frame). GPUs let it slide, Steam's background Game Recording did not: black screen with sound (#13). Fixed and verified under the D3D12 debug layer.

## 1.2 (September 2026)

- **Button prompts** in three flavours — Keyboard (your bindings, in the game's own font), Xbox (original), PlayStation (three icon looks by [Zacksly](https://zacksly.itch.io), CC BY 3.0). Launcher → Controls → *Button Prompts*.
- **Texture replacement and dumping** for modders: `textures\<hash>.png` / `.overlay.png`, launcher → Advanced → *Texture Dump*. The title logo now says *Recompilation* through it.
- **Stick-shake QTEs on a keyboard** really work: rapid A > D > A > D taps drive a full-amplitude shake, and taps shorter than a poll are no longer lost.
- **Map zoom with the mouse wheel**; the mouse becomes the right stick again in menus and the map.
- **Mouse sensitivity while running** matches walking (the direct camera write covers every gameplay camera routine now).
- **60 FPS (#12 follow-up)**: the tick can go below 1.0 (`dp_60fps_tick_min`, default 0.5), so the game no longer runs ahead of the audio when the limiter releases frames early. (Superseded in 1.3 by whole ticks.)
- **Alt+Tab** back into the fullscreen game keeps it above the taskbar; **Monitor** is a real list of displays; complete Ukrainian launcher translation.
- The "invalid" fetch constants the game uses for the sky panorama, the frame copy and the bloom buffers are honoured now.

## 1.1 (September 2026)

- **Mouse aiming.** The aim camera (Space held) is a separate routine driven by the *left* stick with its own acceleration, which is why 1.0 aimed with WASD only. 1.1 detects that camera and feeds the mouse into the left stick while it runs.
- **Mouse look is position control now.** 1.0 nudged the camera target once per frame and the game's camera spring forgot most of it; 1.1 keeps the un-reached angle pending and also turns the yaw state directly, so a mouse move is a fixed angle. Sensitivity is in radians per pixel (default 0.003); auto-centering resumes after the mouse has been idle for `dp_mouse_camera_hold_ms`.
- **Keyboard fix:** holding Shift froze York and Control (LT) did nothing, because the key driver silenced every plain key while a modifier was held. Run = hold **Shift** (the X button); the 1.0 controls sheet wrongly listed E as run.
- **60 FPS:** the logic tick is derived from the measured frame time instead of being pinned to 1/60 s, so long cutscenes no longer drift out of sync with the voices, and the runtime sleeps with a high-resolution timer so the limiter actually holds 16.7 ms.
- **Saves and shader cache next to the game — for real.** In 1.0 the runtime kept using `Documents\deadlyprem`. 1.1 uses `userdata\` and copies your existing saves and shader storage from Documents on first launch.
- **Shader / PSO cache:** the storage never grew after its first session (fixed); driver-compiled pipelines are cached on disk (`userdata\cache\shaders\local`), and a "Preparing shaders" toast at launch plus a corner badge during play show when pipelines are being built.
- **Steam:** launcher → Advanced → *Steam Overlay* `off` for the black-screen / tinted-quarter-frame problem when starting through Steam.
- **Opt-in shader cache sharing:** the launcher asks once whether it may send your shader cache (anonymous, shader microcode and pipeline descriptions only) to the project; merged caches ship with the next release. `tools/merge_shader_storage.py` merges the collected files.
- **Steam Deck preset:** the launcher recognises a Deck (Steam sets `SteamDeck=1`; the APU reports as AMD Custom GPU 0405/0932) and applies the community-tested settings once: RTV, 1×, 2× MSAA, 16× AF, FXAA + CAS, 30 FPS, VSync, fullscreen 1280×800. Everything stays editable.
- **Keyboard stick with travel time** (60 ms to full deflection, like the PC port), so stick-shake QTEs ("Get it off!") count every A > D tap.
- Launcher, both game builds and the F3 watermark report 1.1; the launcher updates in place from GitHub.

## 1.0 (September 2026)

A full restart of the project. The old v0.1.1 preview was built on an outdated SDK and is superseded in every respect: new runtime, new GPU plugin, new launcher, new input. If you still have v0.1.1, delete it and start fresh — the launcher, the config file and the save location have all changed.

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

### Under the hood
- ReXGlue **0.10 nightly** with a set of local SDK changes: the 7e3 → 8888 EDRAM ownership fix for ROV (in-place compute conversion), the PSO wait policy, once-per-constant logging for invalid texture fetch constants, the mouse camera API, the overlay build stamp.
- Game hooks are done as *mid-asm hooks* generated by the recompiler (see `deadlyprem_config.toml`), so they survive regeneration.
