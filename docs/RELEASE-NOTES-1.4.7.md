## Deadly Premonition Recompilation 1.4.7

This release fixes the worst bug in the **native renderer preview**: the whole picture was blurred. The preview is still off by default, and the emulated path you get without touching anything is the same as in 1.4.6.

Install from the launcher's Update button, or unzip over any earlier version. Saves and settings are kept.

### 🔍 Depth of field works: the picture is sharp again
With the native renderer every frame came out evenly blurred, York included, as if the depth of field put everything out of focus. The renderer cleared a render target on every resolve that carried the flag `0x10`, because we had read that flag as "clear". It is `D3DRESOLVE_FRAGMENT0`, the MSAA sample select, and the XDK even fills it in by itself. Deadly Premonition never asks a resolve to clear, and its light pass depends on that: it resolves a buffer, draws into one colour channel, and resolves again, and the depth kept in another channel of that buffer is what the final pass uses to decide how blurred each pixel is. The clear wiped that depth, so every pixel got the same answer. Now the background is soft and York and everything near him are sharp, as on the console.

![Native renderer at 2x in the Red Room: York and the twins sharp, the background out of focus](https://raw.githubusercontent.com/LittleBitUA/DPRecomp/master/docs/screenshots/native_147_redroom.jpg)

![Native renderer at 2x: the map table with the figurines in focus](https://raw.githubusercontent.com/LittleBitUA/DPRecomp/master/docs/screenshots/native_147_map.jpg)

*Native renderer, 2x internal resolution, captured by us in 1.4.7.*

### 🖥 2x and higher no longer wrecks the image
Six pixel shaders (depth of field, edge detection, the post chain and the shadow mask of local lights) read the pixel position and turn it into texture coordinates with constants made for the console's resolution. At 2x they got the position in the larger internal resolution, so the effects sampled the wrong places. The position is scaled back to console pixels now.

### 🧩 Missing geometry
Triangle fans were skipped, so a few small pieces of geometry were missing. They are drawn now.

### ⚡ Less CPU per frame
Shader constants are uploaded only when the game changes them, and the root signature is bound once per frame. Measured on an RTX 5070 at 2x in the Red Room: 60 fps held, the native renderer spends 3.1–3.3 ms of CPU and 1.2–1.3 ms of GPU time per frame.

### 🧪 Please play it and tell us
This is the build where the native renderer starts looking like the game. Turn it on in the launcher (**Native** page → Native Renderer, and try **Internal Resolution** 2x), play for a while, and open an [issue](https://github.com/LittleBitUA/DPRecomp/issues/new) with:
- your GPU, CPU and driver,
- the frame rate with the native renderer off and on, at 1x and 2x,
- a screenshot of anything that looks wrong, with the place in the game,
- the newest `logs\deadlyprem_XXX.log` (the lines starting `Native renderer: frame #` carry the numbers we need).

"It works on my machine" is a useful report too.

### Still not fixed in the preview
The white mirror floor in the sheriff's station and the milky veil outdoors, thin dark contour lines on tree crowns and bushes, and shadows that slide with the character (not re-checked since this fix; tell us if you still see it).

For controller testers of #19: `dp_pad_layout_log = true` in `deadlyprem.toml` logs what your pad sends and what the game receives under the Director's Cut layout.

Everything from 1.4.6 is included.
