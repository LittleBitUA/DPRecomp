## Deadly Premonition Recompilation 2.0

**The native renderer is ready for testing.** Deadly Premonition can now be drawn directly with DirectX 12 instead of an emulated Xbox 360 graphics chip. This is the first version of that renderer we consider in a proper state: colours, light, the sky, depth of field and the mirror floors look the way they should.

It is still a test version, so it stays optional: the launcher asks you once whether to turn it on, and you can switch it on or off at any time in **Settings → Native**. With it off you get the emulated path, exactly as in 1.4.7.

Install from the launcher's Update button, or unzip over any earlier version. Saves and settings are kept.

| The sheriff's station | The hallway |
|---|---|
| ![Native renderer at 2x: the sheriff's station with the dark mirror floor reflecting the room](https://raw.githubusercontent.com/LittleBitUA/DPRecomp/master/docs/screenshots/native_20_station.jpg) | ![Native renderer at 2x: York and two deputies in the station's hallway](https://raw.githubusercontent.com/LittleBitUA/DPRecomp/master/docs/screenshots/native_20_hallway.jpg) |

### 🧪 Please play it and tell us
Turn it on (the launcher asks, or **Settings → Native → Native Renderer**), try **Internal Resolution** 2x, play for a while, and open an [issue](https://github.com/LittleBitUA/DPRecomp/issues/new) with:
- your impressions: what looks right, what looks wrong,
- your FPS with the native renderer off and on, at 1x and 2x,
- your hardware: graphics card, processor and driver,
- a screenshot of anything that looks wrong, with the place in the game,
- the newest `logs\deadlyprem_XXX.log` (the lines starting `Native renderer: frame #` carry the numbers we need).

"It runs fine on my machine" is a useful report too.

### ☀️ The main light is back
The world looked flat and a bit purple because it was lit by the cool ambient light alone. A bug in our shader recompiler skipped the block that switches on each scene's main light (the sun outdoors: its light and the highlights it puts on surfaces), in 297 of the game's 462 pixel shaders. The block is conditional on a flag the game sets, and the recompiler read that condition as a different kind of test that was always false at that point.

### 🌫 No more milky veil
The glow is supposed to pick out only the brightest pixels. Its threshold test was translated wrong and let every pixel through, so the final pass added a blurred copy of the whole frame on top of it at 60–70% strength: a haze over everything, washed-out contrast, a soapy look.

### ☁️ The sky, trails behind moving things, doubled signs
On the Xbox 360 the final colour pass draws over the HDR scene in the same video memory, and where the scene is see-through (the sky, cloud edges, glass) the scene itself shows through. The native renderer kept those as two separate images, so the sky showed black or the previous frame instead: a brown sky with a hard edge over the mountains, trails behind enemies, doubled text on signs, flickering light. It now carries that memory over the way the console does.

### 🪞 The mirror floor in the sheriff's station
It came out white. The floor's shader computes two things in one instruction, and the recompiler let the second read the first's new result instead of the old value, so the floor's transparency came from its brightness and the dark parts were thrown away. Env-map reflections on shiny objects also sampled the wrong side of their cube map.

### 🔥 And more
- Fire and smoke in the forest look like fire again (a constant-addressing bug stretched the particle textures into stripes).
- Keyboard prompts use the key textures with the native renderer too, and the launcher draws them in the PC Director's Cut style: dark keys with white letters.
- The post-processing buffers are 16-bit float, as on the console, instead of 8-bit.
- A bright hard-edged shape around lamps in some cutscenes is gone.

Measured on an RTX 5070 at 2x in the sheriff's station: 60 fps held, the native renderer spends 5.1–6.1 ms of CPU and 1.4–1.6 ms of GPU time per frame.

### Still open in the native renderer
A mirror can show a doubled image, and in one cutscene a lamp's light switches on and off as the camera turns. Shadows sliding with the character and thin dark contours on tree crowns were reported before 2.0 and have not been re-checked since; tell us if you still see them. Most of the testing was on the PAL version; the USA build runs the same renderer with fewer hours on it.

Everything from 1.4.7 is included.
