## Deadly Premonition Recompilation 1.3.7

Root-cause fix for #22, the GPU device removal while tailing Nick to the art gallery. Installs in place from the launcher (Update button) or unzip over any earlier version; saves and settings are kept.

### 🧾 What it was
The guard log from 1.3.6 named the texture: a 32×32 RGBA8 texture loaded from the scaled resolve buffer (render resolution scale 2×), so its contents are uploaded as 64×64. The resource it was uploaded into was 32×32: it is the 2D wrapper the renderer creates to sample problematic 3D textures, and that wrapper was created at the guest size, not the scaled size. Direct3D 12 refuses a 64×64 copy into a 32×32 level, the command list cannot close, and the device is removed. The regular texture path already scaled its resources; only the wrapper did not.

### 🩹 What changed
- Both resource creation paths (regular textures and the 3D-as-2D wrapper) compute the host size through one function that applies the render resolution scale for scaled-resolve textures. Unit tested.
- That wrapper is also refreshed when the game rewrites the texture (it used to keep its first contents forever), so a scaled-resolve texture sampled this way now shows the current frame.
- The copy guard from 1.3.5/1.3.6 stays as the safety net: if any other size mismatch exists, the game survives it and the log names the texture.
- If you play at 100% render resolution (scale 1×) this never affected you; the crash needed 2× or 3×.

GPU plugin DLL only; everything else is 1.3.4.
