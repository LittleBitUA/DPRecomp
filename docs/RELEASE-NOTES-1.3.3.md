## Deadly Premonition Recompilation 1.3.3

Three fixes from this week's reports. Installs in place from the launcher (Update button) or unzip over any earlier version; saves and settings are kept.

### 🎮 Steam Deck: the launcher's Update button works under Proton now (#20)
- The update used to be applied by a PowerShell script, and Proton has no PowerShell: the download finished, the launcher closed, nothing was installed. The apply step is now done by the launcher itself. It copies itself to the temp folder, that copy waits for the launcher to exit, unpacks the release with a built-in zip reader, backs up `userdata` (without the shader cache) and the settings files to `%TEMP%\dp1_update_backup`, copies the new files over, and restarts the launcher. No shell, no scripts.
- This release still has to be installed by hand once on the Deck (unzip over your folder). Every release after it installs from the button.

### 🔫 Mouse aiming in the clock tower fight (#24)
- The tower boss uses its own locked camera, and the mouse was only handed to the game's aim while one of the regular gameplay cameras was running. Holding the aim key (Space, the right trigger) now hands the mouse to the aim whatever camera is active.

### 🧾 GPU device removal: the debug layer's last message makes it into the log (#22)
- The debug layer's messages are copied into the log once per frame, and a device removal is detected on the very frame that produced the fatal message, so that message was lost when the game exited. Both the Direct3D 12 and the DXGI queues are now drained at the moment of the removal. With `d3d12_debug = true` in `deadlyprem.toml` the log ends with the actual validation error.

### 🧩 Under the hood
- Launcher: built-in zip extraction ([miniz](https://github.com/richgel999/miniz), MIT) and a `--apply-update` helper mode; the `%TEMP%\dp1_update.log` convention is unchanged, so a failed update still surfaces its log on the next start.
- Runtime and GPU plugin changed (queue drains, DXGI info queue, aim mapping); both game executables carry the version.

### Known issues
- Keyboard driving: accelerating needs the aim key (Space) held, because it is mapped to the right trigger. A driving-aware mapping (W / S for gas and brake behind the wheel) is in progress.
- ROV render path: green / rainbow-dithered UI panels in some sessions (#23). RTV is the workaround.
- Terrain layer blending shows hard triangle edges while walking (long-standing).
- Some dialogue scenes show light-blue polygonal patches on characters with the ROV path.
- 2x internal resolution: post-process passes are sized for 1x.
- Clouds with hard edges (#10).
- Linux is not supported yet.
