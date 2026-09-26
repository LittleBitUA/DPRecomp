## Deadly Premonition Recompilation 2.0.6

A fix for the native renderer at 3x and 4x, and a log that explains graphics problems.

Install from the launcher's Update button, or unzip over any earlier version. Saves and settings are kept.

### What changed
- **Sun shadows at 3x and 4x** ([#38](https://github.com/LittleBitUA/DPRecomp/issues/38)). At 3x and above the sun shadow map was larger than Direct3D 12 allows for that kind of texture, so it was never created and kept being rebuilt, which drew a hard line of brighter light in the distance that moved with York. It now stays at the largest size the GPU accepts, while the rest of the picture keeps your resolution. Thanks to SilentHeII for the report, logs and save.
- **Clearer logs for graphics problems.** The log now starts with your GPU and driver version, and when the native renderer cannot create something it says once what and why (too large for the GPU, out of video memory and so on) instead of repeating an error every frame. If you report a graphics issue, the newest file from the `logs` folder now tells us much more.

### Known issues
- Shadows with the native renderer are still not right in many places; a larger shadow pass is next.
- AMD Radeon with the native renderer: characters and the picture look purple ([#37](https://github.com/LittleBitUA/DPRecomp/issues/37)). Until it is fixed, use the default renderer on Radeon cards.

### Thank you
Thank you all for the reports, logs and saves. [2.0.5 notes](RELEASE-NOTES-2.0.5.md) · [2.0 notes](RELEASE-NOTES-2.0.md).
