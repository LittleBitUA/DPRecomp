## Deadly Premonition Recompilation 2.0.8

Fixes a crash later in the story.

Install from the launcher's Update button, or unzip over any earlier version. Saves and settings are kept.

### What changed
- **Crash in the game's collision code fixed** ([#39](https://github.com/LittleBitUA/DPRecomp/issues/39)). Some collision checks needed more stack than the game gives its main thread, and the game closed. The game's threads now get more room. This happened with both renderers.

### Known issues
- Another crash from the same report, in the thread that streams the town in, is still open. A save from shortly before it would help ([#39](https://github.com/LittleBitUA/DPRecomp/issues/39)).
- Very long sessions with the native renderer (about 80 minutes or more) can run out of texture slots: buildings turn black and the map breaks ([#37](https://github.com/LittleBitUA/DPRecomp/issues/37)). Restarting the game avoids it until the fix lands.
- AMD Radeon with the native renderer: if the picture looks purple, update your AMD driver first; for one player that fixed it ([#37](https://github.com/LittleBitUA/DPRecomp/issues/37)).
- The lens flare around the sun twinkles. That is the game's own effect, just twice as fast at 60 FPS.

### Thank you
Thanks to michaelbub for the crash logs and dumps, and to Dumpster73 for testing a new driver and sending a save. [2.0.7 notes](RELEASE-NOTES-2.0.7.md) · [2.0 notes](RELEASE-NOTES-2.0.md).
