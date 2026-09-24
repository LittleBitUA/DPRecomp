## Deadly Premonition Recompilation 2.0.2

Fixes for the native renderer: no more black screen after updating with the launcher, and no more frozen window when the graphics card stops responding.

Install from the launcher's Update button, or unzip over any earlier version. Saves and settings are kept.

### What changed
- **Black screen with the native renderer after a launcher update: fixed** ([#33](https://github.com/LittleBitUA/DPRecomp/issues/33)). The launcher's Update button never copied the native renderer's shader file, so after an update the renderer either had nothing to draw with (a black screen from the start) or kept the file from an older version. The game now also looks for it in a folder every launcher does update, so updating to 2.0.2 with the Update button is enough. If the file is missing completely, the game starts with the emulated renderer instead of a black screen. The launcher copies the file too from now on.
- **If the graphics card stops responding (GPU lost), the game tells you and closes** instead of freezing with a window that cannot be closed. Before closing it writes into the log what the graphics card was doing. If this happens to you, please [open an issue](https://github.com/LittleBitUA/DPRecomp/issues/new) and attach the log file the message names: we have seen it a few times in our own testing and these logs are how we find the cause.

### For testers
- `dp_native_gpu_markers = true` in `deadlyprem.toml` makes that log name the exact draw the graphics card was busy with (costs a little CPU).
- `dp_native_gamma_ramp = true` applies the Xbox 360's display gamma to the native renderer's picture: slightly darker shadows, the same as the emulated renderer. It is off by default for now.

### Still asking for your reports
Please keep telling us how the native renderer runs for you: impressions, FPS (1x and 2x) and your hardware, in the [issues](https://github.com/LittleBitUA/DPRecomp/issues/new). [2.0 notes](RELEASE-NOTES-2.0.md) · [2.0.1 notes](RELEASE-NOTES-2.0.1.md).
