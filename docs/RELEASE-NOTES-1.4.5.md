## Deadly Premonition Recompilation 1.4.5

Launcher fix on top of 1.4.4. Installs in place from the launcher (Update button) or unzip over any earlier version; saves and settings are kept.

### 🧹 Settings labels no longer get cut off (#32)
Two settings labels were longer than their column, wrapped to a second line, and the row clipped it (thanks Crowley9 for the screenshot). Labels are single-line now, the two long ones read "Aim: per-axis mouse floor" and "Native Renderer (preview)", and hovering a label shows the full explanation as a tooltip. Anything that still doesn't fit ends in an ellipsis instead of disappearing under the next row.

Also: the game window title said "1.3" since, well, 1.3; it now shows the real version.

### 🧪 Native renderer preview: please try it and tell us how it runs
Since 1.4.2 the build ships a work-in-progress native Direct3D 12 renderer next to the stock Xbox 360 GPU emulation. It is off by default because the picture is not finished: floors in the sheriff's station come out white, some surfaces are too bright, shadows are partial. What we don't know yet is how it performs on hardware other than ours, and that is where you can help.

To switch it on: launcher → Settings → **Experimental** tab → tick **Native Renderer (preview)** → Save & Close, then Play. (Or set `dp_native_render = true` in `deadlyprem.toml`.) Both the European and the USA disc builds support it. To go back, untick the box; nothing else changes, and saves are untouched either way.

What would help most: a note here or in a new issue with your GPU and CPU, the frame rate you get in the same spot with the option off and on (the same scene, ideally standing still; the launcher's FPS counter or any overlay is fine), and whether anything besides the known picture problems is broken (missing objects, crashes, black screen). A `logs\deadlyprem_XXX.log` from the native run tells us the rest. If the game doesn't start at all with it on, that log is the one we need.

Everything from 1.4.4 is included: the Audio Output option (Auto / Stereo / Surround), the working Share Shader Cache, the save-hang fix (#21), diagonal mouse aim (#30) and the native renderer preview under the Experimental tab (off by default).
