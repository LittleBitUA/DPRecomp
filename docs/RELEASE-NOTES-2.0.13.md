## Deadly Premonition Recompilation 2.0.13

Reflections in the native renderer.

Install from the launcher's Update button, or unzip over any earlier version. Saves and settings are kept.

### What changed
- **Reflections in the native renderer show the right thing.** The mirror in Emily's house showed a blown-up piece of the wall instead of York ([#46](https://github.com/LittleBitUA/DPRecomp/issues/46)), and the lake by the hospital reflected a shifted strip of the forest, with the fish icon floating above the water ([#41](https://github.com/LittleBitUA/DPRecomp/issues/41)). The native renderer cut every reflection with the wrong plane, so things behind the mirror or under the water got into the picture. It now uses the plane the game sets, like the default renderer does. We checked Emily's mirror and the station's mirror floor in game; the lake goes through the same code, so it should match the default renderer now too.

### Please tell us
- If a reflection still looks different from the default renderer (especially the lake), please open an issue with a screenshot of both.
- **Does the car feel weaker at 60 FPS than at 30 FPS?** If accelerating or the top speed feel clearly slower at 60 FPS (you can switch 60 FPS off in the launcher to compare), please tell us in an issue.

### Known issues
- AMD Radeon with the native renderer: if the picture looks purple, update your AMD driver first ([#37](https://github.com/LittleBitUA/DPRecomp/issues/37)).
- With the native renderer, keep Vertical sync off; in native mode it doesn't sync to the monitor.

### Thank you
Thanks to Crowley9 for the save next to Emily's mirror and to Dominus41 for the lake screenshots, and to everyone testing. [2.0.12 notes](RELEASE-NOTES-2.0.12.md) · [2.0 notes](RELEASE-NOTES-2.0.md).
