## Deadly Premonition Recompilation 2.0.12

Long sessions with the native renderer, and effects at 60 FPS.

Install from the launcher's Update button, or unzip over any earlier version. Saves and settings are kept.

### What changed
- **The native renderer no longer stops drawing objects after about 75 minutes** ([#37](https://github.com/LittleBitUA/DPRecomp/issues/37), [#43](https://github.com/LittleBitUA/DPRecomp/issues/43)). It kept every texture it ever loaded, until there was no room left: buildings, the map and York himself stopped being drawn. Textures that have not been on screen for a minute are now released and loaded again when they come back, so video memory also stays in check.
- **Muzzle flashes, splashes and hits play at the console's speed at 60 FPS.** The game advances these effects by a fixed step every frame, which at 60 FPS made them twice as fast as on the console.

### Please tell us
- **Does the car feel weaker at 60 FPS than at 30 FPS?** Parts of the car's physics are tied to the frame rate on the console too. If accelerating or the top speed feel clearly slower at 60 FPS (you can switch 60 FPS off in the launcher to compare), please tell us in an issue.

### Known issues
- Reflections in the native renderer can be wrong (the lake by the hospital, the mirror in Emily's house): [#41](https://github.com/LittleBitUA/DPRecomp/issues/41), [#46](https://github.com/LittleBitUA/DPRecomp/issues/46). The default renderer shows them correctly.
- AMD Radeon with the native renderer: if the picture looks purple, update your AMD driver first ([#37](https://github.com/LittleBitUA/DPRecomp/issues/37)).
- With the native renderer, keep Vertical sync off; in native mode it doesn't sync to the monitor.

### Thank you
Thanks to Crowley9 and SilentHeII for the logs that pinned down the long-session bug, and to everyone testing. [2.0.11 notes](RELEASE-NOTES-2.0.11.md) · [2.0 notes](RELEASE-NOTES-2.0.md).
