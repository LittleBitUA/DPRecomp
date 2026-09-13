## Deadly Premonition Recompilation 1.3.4

A diagnostics release, no gameplay changes. Its whole point is the three open reports we cannot reproduce here: the hang while saving (#21), the GPU device removal while tailing Nick (#22) and the green / rainbow panels on the ROV path (#23). If you hit any of them on 1.3.4, the log now carries what we need. Installs in place from the launcher (Update button, Steam Deck too) or unzip over any earlier version; saves and settings are kept.

### 📊 A `[stats]` line in the log every minute
- Process working set and private bytes, handle count, free system memory, VRAM budget and usage, every guest heap, kernel object and thread counts, GPU texture / pipeline cache sizes, shared-memory use, XMA contexts, audio clients. `log_stats_interval` in `deadlyprem.toml` sets the period (default 60 s, 0 = off).
- It is written from its own thread, so it keeps coming while the game thread is stuck. **#21 (save hang):** play until it hangs, wait a minute, then send the log: the last `[stats]` lines show whether memory or handles kept climbing, and `objects busy` / `threads busy` there means a kernel lock is held by the stuck thread.
- **#22 (device removed):** the last `[stats]` line before the crash shows how much VRAM the process and the adapter had at that moment.

### 🟩 Green / rainbow panels on the ROV path (#23)
- The ROV path keeps the HDR scene and the 8-bit UI in the same EDRAM tiles and converts between the two formats in place. A resolve that reads an 8-bit render target from tiles still holding HDR bits is now caught and logged as `ROV resolve: N of M source tiles ... are tagged 7e3`, with the frame number. That is the green frame. We already see one such frame on the title -> loading transition; **if you see the panels during play, note the time and send the log.**
- Two switches to try in `deadlyprem.toml`, one at a time, and tell us which changes what: `rov_7e3_convert_on_resolve = true` (convert those tiles before the resolve) and `rov_8888_full_extent = true` (convert the whole render target for 8-bit draws, not only the estimated draw area).
- Stair-stepped sky or fog edges in some scenes (the lake at dusk, for one): `rov_shadow_debug_tint = true` paints every pixel that lost its smooth alpha magenta. A screenshot of that plus the log with `rov_bind_log = true` shows us which pass breaks it.

### 🧾 The game's own errors in the log
- PhysX 2.6 error stream (`dp_physx_log`, on by default): invalid parameters, skipped calls, out-of-memory, asserts, with file and line, the first 5 of each text then every 100th. Long sessions with a few of these before a crash are exactly what we want to see.
- The game's STL / CRT asserts and conditional traps now log their guest address and return address instead of a bare "trap hit".
- Sound-effect cues (`dp_audio_cue_log`, off): every cue the game fires, with the frame number.

### 🧹 Removed
- The experimental `dp_anim_event_dedupe` / `dp_anim_event_log` switches from 1.3.0. The doubled sounds of #17 / #18 went away with whole ticks in 1.3, and player logs showed the "triple" picks were three different objects. A leftover key in `deadlyprem.toml` only logs one "unknown cvar" warning.

### How to report
Attach the newest `logs\deadlyprem_XXX.log` (and the `.dmp` next to it after a crash), your GPU and driver, ROV or RTV, internal resolution, and a screenshot when it is visual. Every new line described above is `grep`-able: `[stats]`, `ROV resolve`, `PhysX`, `trap`.
