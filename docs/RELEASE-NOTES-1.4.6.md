## Deadly Premonition Recompilation 1.4.6

Everything in this release is about the **native renderer preview**, which is still off by default. The emulated path you get without touching anything is byte for byte the 1.4.5 one.

Install from the launcher's Update button, or unzip over any earlier version; saves and settings are kept.

### 🖥 Internal resolution up to 4x
The native renderer now renders the whole frame - the scene, the reflections, the shadow maps and the final image - at a multiple of the console's resolution and scales it down to your window. Jagged edges and crawling foliage go away, and the picture stays the one the game intended: every shader addresses its targets through normalized coordinates, so nothing about the image changes except how finely it is sampled.

Launcher → Settings → **Native** → **Internal Resolution**: 1x (1280x720, as the console), 2x (2560x1440), 3x, 4x. Or `dp_native_scale = 2` in `deadlyprem.toml`.

Measured on an RTX 5070 in the hospital corridor: 1x costs 0.5 ms of GPU time per frame, 2x costs 1.1 ms, and both hold the game's 60 fps cap with the CPU side at about 2 ms per frame. If a target does not fit in video memory the renderer falls back to 1x for it and says so in the log.

### 🔍 Textures got their mipmaps back
Until now the native renderer uploaded only the base level of every texture, so anything at a distance shimmered and crawled, and the picture looked sharper than the console's in a way that was wrong rather than better. The full mip chain is uploaded now, and the sampler follows the game's LOD bias and mip range.

### 🌑 Shadow maps in the console's depth format
The shadow maps were kept as 32-bit float depth while the polygon offset that separates a caster from the surface it stands on was computed for the console's 24-bit unorm depth. The two disagree by more the further away you are, which is where the acne came from. They now use the same 24-bit format the game asked for.

### 🎨 Colour clamped where the console clamped it
The scene render targets hold 7e3 floats - three exponent bits, seven mantissa bits, 31.875 at most. Only the resolve applied that range, so a bright additive pass (street lamps, the flamethrower) could accumulate far past what the hardware could store before anything clamped it. Every draw into such a target clamps now.

### 🧩 Recompiler: conditional blocks in 297 shaders
`cond_exec` in the Xenos microcode runs its block only when a bool constant matches. The recompiler ignored the condition and always ran the block, which in 297 of 1131 shaders turned the XDK compiler's bool-to-float pattern into a constant "true" - the sun-lit branch of walls and floors, among others. The shader cache shipped with this build is rebuilt with the fix.

### 📊 Numbers in the log
A native run now prints, every 600 frames: draws, resolves, clears, uploaded bytes, live pipelines, and the frame time, the renderer's CPU time and the GPU time as p50/p90/p99. That line is what we need from testers more than anything else.

### Still not fixed in the preview
The white mirror floor in the sheriff's station, shadows that slide with the character, and a few triangle-fan draws that are skipped. They are the next items on the list.

Everything from 1.4.5 is included: the settings-label fix (#32), the Audio Output option, the working Share Shader Cache, the save-hang fix (#21) and diagonal mouse aim (#30).
