## Deadly Premonition Recompilation 2.0.1

A small safety update for the native renderer, right after 2.0. Nothing you saw in 2.0 looks different; if 2.0 runs well for you, 2.0.1 runs the same.

Install from the launcher's Update button, or unzip over any earlier version. Saves and settings are kept.

### What changed
- **An experimental texture-sharing path is off by default.** It was added in 2.0 to let two views of the same memory share one image, the way the emulator does. A review after the release found a case where, after a level change, it could keep showing an old image instead of the real texture. It never actually triggered in our testing, and nothing depends on it, so it is off now.
- **A possible crash is closed off.** If the game released a render target on another thread at the wrong moment, the renderer could still copy from it. We have not seen it happen, but it could.
- **Less CPU time wasted:** the renderer no longer re-checks every render target on every draw when nothing has changed.

### Still asking for your reports
Everything from the 2.0 notes stands: please play with the native renderer and tell us how it goes, your FPS (off and on, 1x and 2x) and your hardware, in the [issues](https://github.com/LittleBitUA/DPRecomp/issues/new). [2.0 notes](RELEASE-NOTES-2.0.md).
