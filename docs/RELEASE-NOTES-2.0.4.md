## Deadly Premonition Recompilation 2.0.4

Fixes holes and spikes on some objects with the native renderer: the pillows in York's room, York's tie showing through his back, door handles and trees pulled across the screen.

Install from the launcher's Update button, or unzip over any earlier version. Saves and settings are kept.

### What changed
- **Holes and spikes on some objects with the native renderer: fixed** ([#35](https://github.com/LittleBitUA/DPRecomp/issues/35)). The renderer misread the size of the game's lists of triangle corners: a list with an odd number of entries lost its last one, and the last triangle of that model then pointed at the model's first corner. That drew a long spike across the object, or a triangle facing away that the card skipped, leaving a hole. Thanks to kite1234567 and Crowley9 for the screenshots, logs and saves, and to our own tester for the RenderDoc capture that showed it.

![York's room with the native renderer in 2.0.4: the pillows are whole again](https://raw.githubusercontent.com/LittleBitUA/DPRecomp/master/docs/screenshots/2.0.4_native_pillows_fixed.jpg)

### Thank you
Thank you all for your activity and for supporting the project. Every issue, log, save and screenshot you send is read, and the last three updates were built straight from them. Everyone who has helped on GitHub is now listed in the [README](https://github.com/LittleBitUA/DPRecomp#everyone-who-helped-on-github).

### Still asking for your reports
Please keep telling us how the native renderer runs for you: impressions, FPS (1x and 2x) and your hardware, in the [issues](https://github.com/LittleBitUA/DPRecomp/issues/new). A save and the log from the `logs` folder make a report ten times faster to fix. [2.0.3 notes](RELEASE-NOTES-2.0.3.md) · [2.0 notes](RELEASE-NOTES-2.0.md).
