## Deadly Premonition Recompilation 1.4.1

Hotfix for the 1.4.0 controller layout and two fixes from the tracker. Installs in place from the launcher (Update button) or unzip over any earlier version; saves and settings are kept.

### 🚗 Director's Cut layout stands down in the car (#28)
1.4.0 decided "York is driving" from a flag on the camera object, and on a pad that flag read "on foot" while driving, so the layout remapped the car: gas on LT, brake on RT, and A while accelerating became the breath hold, so Zach could not be talked to. The signal now comes from the game itself: the routine that reads the triggers as throttle and brake runs only while York is at the wheel, and the layout pauses while it runs (plus 150 ms). In the car everything is the original Xbox 360 mapping again; on foot the Director's Cut triggers stay.

### 🌍 Game Language works on the European disc (#27)
Picking German, French, Spanish or Italian in the launcher did nothing. The game reads the language correctly, but then only honours it when the console reports a retail region; the runtime reported a development-kit region, so the European build always used its English set. It now reports the region of the disc's own executable (Europe for the PAL disc, North America for the USA disc). Verified by which text tables the game opens. The USA disc is English-only, so this changes nothing there.

### 🎮 Prompt icons follow the Director's Cut layout (#19)
With that layout on, the PlayStation prompt sets show L2 where the game means "aim" and R2 where it means "hold breath". The original Xbox icons cannot be swapped, and the "A = fire" prompt stays A because the same icon means "interact" everywhere else.

Everything from 1.4.0 is included.
