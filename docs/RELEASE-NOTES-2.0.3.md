## Deadly Premonition Recompilation 2.0.3

Fixes the native renderer crashing with "GPU lost" after some minutes of play, while exploring or after closing the pause menu.

Install from the launcher's Update button, or unzip over any earlier version. Saves and settings are kept.

### What changed
- **"GPU lost" crashes with the native renderer: fixed** ([#33](https://github.com/LittleBitUA/DPRecomp/issues/33), [#34](https://github.com/LittleBitUA/DPRecomp/issues/34)). When the game let go of a model's data between two frames (it does that when it loads the next part of the map, on the roads, in the hotel, entering buildings, and when the pause menu closes), the native renderer could throw away its copy on the graphics card while the card was still drawing the previous frame with it. The card then read memory that no longer existed and Windows reset it. The renderer now keeps everything it stops using until the graphics card has finished every frame that used it. Thanks to PsychotropicPineapples and kite1234567 for the logs that pinned it down.
- More hardening in the same area: objects the game releases on its loading threads are only taken apart between frames, and long sessions no longer risk running out of the renderer's render-target slots.

### If it still happens
The "GPU lost" message now names exactly what the graphics card was reading. If you see it on 2.0.3, please [open an issue](https://github.com/LittleBitUA/DPRecomp/issues/new) and attach the log file the message names.

### Still asking for your reports
Please keep telling us how the native renderer runs for you: impressions, FPS (1x and 2x) and your hardware, in the [issues](https://github.com/LittleBitUA/DPRecomp/issues/new). [2.0.2 notes](RELEASE-NOTES-2.0.2.md) · [2.0 notes](RELEASE-NOTES-2.0.md).
