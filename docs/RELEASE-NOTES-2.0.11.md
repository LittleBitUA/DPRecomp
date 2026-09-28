## Deadly Premonition Recompilation 2.0.11

Housekeeping from the latest ReXGlue updates.

Install from the launcher's Update button, or unzip over any earlier version. Saves and settings are kept.

### What changed
- **The `logs` folder stays small.** Old logs are removed when the folder grows past 256 MB, and only the 5 newest crash dumps are kept. Before this, nothing was ever removed.
- **A controller plugged in or woken up mid-game is noticed** by the game.
- **Closing a system message no longer also presses a button in the game.** While a message or the storage device selector is on screen, the game ignores the controller and keyboard.

### Known issues
- A crash in the thread that streams the town in is still open. A save from shortly before it would help ([#39](https://github.com/LittleBitUA/DPRecomp/issues/39)).
- Very long sessions with the native renderer (about 80 minutes or more) can run out of texture slots: buildings turn black and the map breaks ([#37](https://github.com/LittleBitUA/DPRecomp/issues/37)). Restarting the game avoids it until the fix lands.
- AMD Radeon with the native renderer: if the picture looks purple, update your AMD driver first; for one player that fixed it ([#37](https://github.com/LittleBitUA/DPRecomp/issues/37)).
- With the native renderer, keep Vertical sync off; in native mode it doesn't sync to the monitor.

### Thank you
Thanks to the ReXGlue developers for the upstream changes. [2.0.10 notes](RELEASE-NOTES-2.0.10.md) · [2.0 notes](RELEASE-NOTES-2.0.md).
