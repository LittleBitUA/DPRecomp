## Deadly Premonition Recompilation 2.0.14

Mouse look fixes.

Install from the launcher's Update button, or unzip over any earlier version. Saves and settings are kept.

### What changed
- **Looking up with the mouse no longer shakes the camera against the ground** ([#45](https://github.com/LittleBitUA/DPRecomp/issues/45)). The mouse could tilt the camera further than the game allows with the stick, right into the ground. It now stops where the stick stops.
- **In the car, the camera stays where you turn it** ([#45](https://github.com/LittleBitUA/DPRecomp/issues/45)). Turning the camera with the mouse while driving used to snap it straight back behind the car. It now holds while you move the mouse and for a second after, then glides back behind the car like it does when you let go of the stick.

### Still open from #45
- Peeking through windows lets the mouse look past the window, shaving turns York instead of the camera, and walking backwards right after a mouse turn snaps York to his old direction. These are next.

### Please tell us
- If the car camera returns too early or too late for your taste, the delay is `dp_mouse_camera_car_hold_ms` in `deadlyprem.toml` (milliseconds, 1000 by default).

### Known issues
- AMD Radeon with the native renderer: if the picture looks purple, update your AMD driver first ([#37](https://github.com/LittleBitUA/DPRecomp/issues/37)).
- With the native renderer, keep Vertical sync off; in native mode it doesn't sync to the monitor.

### Thank you
Thanks to Crowley9 for the detailed mouse look report. [2.0.13 notes](RELEASE-NOTES-2.0.13.md) · [2.0 notes](RELEASE-NOTES-2.0.md).
