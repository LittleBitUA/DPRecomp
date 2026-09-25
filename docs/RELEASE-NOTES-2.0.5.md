## Deadly Premonition Recompilation 2.0.5

Driving with the keyboard, a much faster start with Skip Intro, and the Director's Cut controller layout fixed after driving.

Install from the launcher's Update button, or unzip over any earlier version. Saves and settings are kept.

### What changed
- **Drive with W and S.** In a car, `W` now accelerates and `S` brakes and reverses, so you no longer hold `Space` to drive and the mouse stays on the camera instead of steering. `A` and `D` steer as before. `Space` and `Ctrl` still work as the accelerator and brake; while `Space` is held the mouse steers, as it always did. To turn it off, set `dp_keyboard_driving = false` in `deadlyprem.toml`.
- **Getting out of the car.** The game keeps updating York's car after he steps out, and the port took that as "still driving". Now driving ends the moment York leaves the driver's seat. This also fixes the Director's Cut controller layout staying paused on foot after a drive, with aim and fire back on the original triggers, which is likely what [#19](https://github.com/LittleBitUA/DPRecomp/issues/19) ran into.
- **Skip Intro skips the opening movie too.** With Skip Intro on, the game still played its almost 6-minute opening movie before the title screen, which looked like the game hanging at start. Now both Skip Intro settings skip it: the main menu appears in about 7 seconds (USA) or 11 seconds (PAL).
- **Stutter reports.** The native renderer's statistics line in the log now also shows how long each frame takes to reach the screen, to track down stutter like [#36](https://github.com/LittleBitUA/DPRecomp/issues/36).

### Known issue
- **AMD Radeon with the native renderer: characters look dark or purple** ([#37](https://github.com/LittleBitUA/DPRecomp/issues/37)). We are working on it. Until then, use the default renderer on Radeon cards (launcher, Native page, Native Renderer off).

### Thank you
Thanks to Crowley9 for the save we tested the car with, to kite1234567 for the log that showed the long start, to valeraudovenko32-oss and valeriyjurievich-ctrl for the detailed performance logs in #36, to EliParker28 for sticking with #19, and to Dumpster73 for the Radeon video in #37. Thank you all for your activity and for supporting the project. Everyone who has helped on GitHub is listed in the [README](https://github.com/LittleBitUA/DPRecomp#everyone-who-helped-on-github).

### Still asking for your reports
Please keep telling us how the native renderer runs for you: impressions, FPS (1x and 2x) and your hardware, in the [issues](https://github.com/LittleBitUA/DPRecomp/issues/new). A save and the log from the `logs` folder make a report ten times faster to fix. [2.0.4 notes](RELEASE-NOTES-2.0.4.md) · [2.0 notes](RELEASE-NOTES-2.0.md).
