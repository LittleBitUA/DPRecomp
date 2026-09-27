## Deadly Premonition Recompilation 2.0.9

Smoother 60 FPS.

Install from the launcher's Update button, or unzip over any earlier version. Saves and settings are kept.

### What changed
- **Even frame pacing** ([#36](https://github.com/LittleBitUA/DPRecomp/issues/36)). The game's frame limiter ran very slightly slower than 60 FPS, and every few seconds the game caught up with a double step: a small jerk while York or the camera moves. Frames now follow exact 1/60 s deadlines, so the game advances one step per frame. With vsync on, the same drift also made a frame miss the monitor's refresh now and then; that should be gone too.

### Known issues
- A crash in the thread that streams the town in is still open. A save from shortly before it would help ([#39](https://github.com/LittleBitUA/DPRecomp/issues/39)).
- Very long sessions with the native renderer (about 80 minutes or more) can run out of texture slots: buildings turn black and the map breaks ([#37](https://github.com/LittleBitUA/DPRecomp/issues/37)). Restarting the game avoids it until the fix lands.
- AMD Radeon with the native renderer: if the picture looks purple, update your AMD driver first; for one player that fixed it ([#37](https://github.com/LittleBitUA/DPRecomp/issues/37)).
- The lens flare around the sun twinkles. That is the game's own effect, just twice as fast at 60 FPS.

### Thank you
Thanks to valeriyjurievich-ctrl and valeraudovenko32-oss for the logs, saves and video that tracked this down. [2.0.8 notes](RELEASE-NOTES-2.0.8.md) · [2.0 notes](RELEASE-NOTES-2.0.md).
