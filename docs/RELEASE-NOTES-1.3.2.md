## Deadly Premonition Recompilation 1.3.2

A diagnostics release: no gameplay changes, but the log now explains crashes, hangs and GPU device losses on its own, so the next bug reports can be solved from a single file. Please update; a report from 1.3.2 is worth much more than one from 1.3.1. Installs in place from the launcher (Update button) or unzip over any earlier version; saves and settings are kept. Steam Deck: still unzip by hand (the in-launcher updater relies on PowerShell).

### 🧾 What the log contains now
- **Crash report.** When the game dies, `logs\deadlyprem_XXX.log` ends with the exception code and address, the module and offset, the host registers, the guest thread that crashed, the PowerPC registers and a guest call stack (`sub_XXXXXXXX+offset`), and a minidump `logs\deadlyprem_<date>_<pid>.dmp` is written next to it. Attach both with a crash report.
- **Every line carries the game's thread name and the frame number**, for example `[GameThread (F8000330)] [f2052]`. The runtime reads the names the game gives its threads: GameThread, LoadThread, PhysicsThread, RenderThread, RouteThread, MapThread, InitThread, the pack loaders, the update workers. Thread start and exit are logged.
- **GPU device removal (#22).** DRED (Device Removed Extended Data) is on by default, so a `D3D12 device removed` line is followed by the GPU's own breadcrumb list of the last operations. With `d3d12_debug = true` in `deadlyprem.toml` the debug layer's messages are written into the log too; DebugView is not needed any more.
- **Saves (#21).** Every save-container create, open, close and delete is logged with its result, so a hang on the "Saving" screen names the step that did not complete.
- **Files.** `vfs_log_opens = true` logs every successful file open with its handle (for missing-asset and streaming reports). Failed opens were already logged.
- **Memory, audio, fatal paths.** Guest memory allocation failures are logged with their size, audio driver registration is logged, and the game's own fatal stops (KeBugCheck, unhandled RtlRaiseException) are logged as errors before the runtime stops. Texture fetch constants that are honoured despite their "invalid" type are reported once each.
- The build stamp is the first application line of every log, and the log is flushed every 5 seconds so a hard kill loses at most a few seconds.

### 🧪 For the open reports
- **#22 (crash tailing Nick to the gallery):** run that section once on 1.3.2 with your usual settings; if it crashes, the log now carries the DRED breadcrumbs. Attach the whole log.
- **#21 (hang on Saving):** keep playing on 1.3.2; when it hangs, attach the log from that session. The save steps are in it.
- **Any crash:** the log plus the `.dmp` from `logs\`.

### 🧩 Under the hood
- Silent exits are covered too: `std::terminate` and `abort()` paths write a report and a minidump before the process ends, and the filter is re-asserted every frame in case a driver or overlay DLL replaced it.
- Runtime DLL, GPU plugin and both game executables changed (a mid-asm hook on the game's thread wrapper supplies the thread names). Launcher carries the version. `docs/sdk-patches` regenerated.
- New settings: `d3d12_dred` (default on), `vfs_log_opens` (default off), `crash_handler_test` (default off; raises a deliberate crash at start to verify the report path).

### Known issues
- Steam Deck / Proton: the launcher's Update button downloads but does not apply (PowerShell). Unzip manually; a PowerShell-free updater is next.
- Keyboard driving: accelerating needs the aim key (Space) held, because it is mapped to the right trigger. A driving-aware mapping (W / S for gas and brake behind the wheel) is in progress.
- ROV render path: green / rainbow-dithered UI panels in some sessions (#23). RTV is the workaround.
- Terrain layer blending shows hard triangle edges while walking (long-standing).
- Some dialogue scenes show light-blue polygonal patches on characters with the ROV path.
- 2x internal resolution: post-process passes are sized for 1x.
- Clouds with hard edges (#10).
- Linux is not supported yet.
