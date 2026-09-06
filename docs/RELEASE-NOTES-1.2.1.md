## Deadly Premonition Recompilation 1.2.1

Hotfix for the black screen with FSR 2 / FSR 3 when Steam's Game Recording is hooked into the game (issue #13). Installs in place from the launcher (Update button) or unzip over 1.0 / 1.1 / 1.2; saves and settings are kept.

### 🖥️ Black screen with FSR 2 / FSR 3 (#13)
- The presenter's FSR 2 / FSR 3 pass left FidelityFX's descriptor heaps bound and then ran the sharpening pass with descriptor tables from its own heap - Direct3D 12 `EXECUTION ERROR #708 SET_DESCRIPTOR_TABLE_INVALID` on every frame. GPUs normally tolerate it, which is why it went unnoticed; with Steam's background **Game Recording** capturing the swap chain it turned into a black screen (the game ran, audio played, nothing was shown). Reported by SilentHeII with a DebugView log that pointed straight at it.
- Fixed: the presenter re-binds its heap after every FidelityFX dispatch. Verified under the D3D12 debug layer: 8728 errors in 40 s before, zero after.
- If the FidelityFX dispatch ever fails, the log now says so (`FidelityFX temporal upscaler dispatch failed`) instead of silently falling back to spatial FSR.

### Misc
- Launcher, both game builds and the zip README report 1.2.1.
- `docs/sdk-patches` regenerated with the presenter change.

### Known issues (unchanged from 1.2)
- Stair-stepped sun halo and soft bloom / DoF at 2× internal resolution (post-process passes sized for 1×); a "resolution scale threshold" is planned for 1.3. Internal Resolution 1× does not have it.
- Clouds with hard edges (#10).
