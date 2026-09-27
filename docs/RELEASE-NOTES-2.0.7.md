## Deadly Premonition Recompilation 2.0.7

Native renderer shadows closer to the console, and a clean sun.

Install from the launcher's Update button, or unzip over any earlier version. Saves and settings are kept.

### What changed
- **Shadows.** We compared the native renderer with the emulated one frame by frame. The shadow maps themselves match; the differences were around them. At 2x and above surfaces no longer shadow themselves in dark stair-stepped bands, shadow edges at 3x and 4x are sharp again, and the fog veil in floor reflections is back.
- **The sun** no longer has a dark jagged ring around it.
- Texture filtering uses the anisotropy levels the game asks for.

### Known issues
- The lens flare around the sun twinkles. That is the game's own effect (the PC version does it too), just twice as fast at 60 FPS; we're looking at matching the console's pace.
- Very long sessions with the native renderer (about 80 minutes or more) can run out of texture slots: buildings turn black and the map breaks ([#37](https://github.com/LittleBitUA/DPRecomp/issues/37)). Restarting the game avoids it until the fix lands.
- AMD Radeon with the native renderer: the picture looks purple ([#37](https://github.com/LittleBitUA/DPRecomp/issues/37)). Use the default renderer on Radeon for now.

### Thank you
Thanks to SilentHeII for confirming the 2.0.6 fix and for the new report and log, and to everyone sending saves and logs. [2.0.6 notes](RELEASE-NOTES-2.0.6.md) · [2.0 notes](RELEASE-NOTES-2.0.md).
