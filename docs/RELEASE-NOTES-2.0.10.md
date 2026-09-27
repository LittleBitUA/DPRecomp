## Deadly Premonition Recompilation 2.0.10

One frame shown per game frame.

Install from the launcher's Update button, or unzip over any earlier version. Saves and settings are kept.

### What changed
- **No more extra presents** ([#36](https://github.com/LittleBitUA/DPRecomp/issues/36)). The small corner indicators (shader compiling, achievements) kept the interface repainting on every monitor refresh even while hidden, so on top of each game frame the screen was presented again: an FPS overlay showed 120 on a 60 Hz monitor. Now the game shows exactly one image per frame, 60 per second, with both renderers.
- With the native renderer, keep Vertical sync off: in native mode it doesn't sync to the monitor, it only changes the game's internal timing.

### Known issues
- A crash in the thread that streams the town in is still open. A save from shortly before it would help ([#39](https://github.com/LittleBitUA/DPRecomp/issues/39)).
- Very long sessions with the native renderer (about 80 minutes or more) can run out of texture slots: buildings turn black and the map breaks ([#37](https://github.com/LittleBitUA/DPRecomp/issues/37)). Restarting the game avoids it until the fix lands.
- AMD Radeon with the native renderer: if the picture looks purple, update your AMD driver first; for one player that fixed it ([#37](https://github.com/LittleBitUA/DPRecomp/issues/37)).
- The lens flare around the sun twinkles. That is the game's own effect, just twice as fast at 60 FPS.

### Thank you
Thanks to valeriyjurievich-ctrl for the overlay screenshot that showed the extra presents. [2.0.9 notes](RELEASE-NOTES-2.0.9.md) · [2.0 notes](RELEASE-NOTES-2.0.md).
