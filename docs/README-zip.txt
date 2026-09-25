Deadly Premonition Recompilation 2.0.3  -  by the <<Little Bit>> team  -  MADE IN UKRAINE
https://github.com/LittleBitUA/DPRecomp

WHAT IS THIS
  A native Windows port of Deadly Premonition (Xbox 360, 2010) made by static
  recompilation on the ReXGlue SDK. No emulator. You provide your own game.

QUICK START
  1. Unzip anywhere (not inside Program Files).
  2. Either copy your extracted disc files into  assets\  (default.xex must be
     at assets\default.xex), or just press PLAY and pick your .iso - the
     built-in installer extracts it (a few minutes).
  3. Start PlayDeadlyPremonition.exe and press PLAY.

REGIONS
  European (PAL) and USA discs are both supported. The launcher looks at
  assets\default.xex and starts deadlyprem.exe (PAL) or deadlyprem_usa.exe
  (USA) - nothing to configure.

SETTINGS
  PlayDeadlyPremonition.exe -> Settings: graphics (internal resolution, FSR 3,
  FXAA, 60 FPS), mouse (direct camera control, sensitivity), controls (every
  key, DualSense adaptive triggers), advanced, debug, experimental. Inside the
  game: F4 = live settings overlay, F3 = performance overlay, F7 = achievements.
  NATIVE RENDERER (new in 2.0, Settings -> Native): the game drawn directly
  with DirectX 12 instead of an emulated Xbox 360 graphics chip - faster, and
  up to 4x internal resolution. This is the first test version brought to a
  proper state. The launcher offers it once; off = the emulated path exactly
  as before. Please tell us how it runs (GPU, CPU, FPS, anything that looks
  wrong): https://github.com/LittleBitUA/DPRecomp/issues

DEFAULT CONTROLS (full table and rebinding: launcher -> Settings -> Controls)
  WASD move, mouse look, wheel = weapon, E interact, R cancel/reload,
  F flashlight, C observe, Space (hold) aim, Space+LMB fire, Ctrl lock-on,
  Z/X strafe, Enter pause, M map. A controller works at the same time.

BUTTON PROMPTS / TEXTURE MODS
  Launcher -> Settings -> Controls -> Button Prompts: Keyboard (your keys),
  Xbox (original) or PlayStation (icons by Zacksly, CC BY 3.0 - prompts\).
  textures\<hash>.png replaces a game texture; Advanced -> Texture Dump writes
  every loaded texture to textures\dump\ so you can find the one to edit.

SAVES
  userdata\  next to the game. Back up that folder to keep your progress.

UPDATES
  The launcher checks GitHub on start and installs new releases in place,
  keeping your config and saves.

TROUBLESHOOTING
  - Black window / crash at start: try Settings -> Graphics -> Internal
    Resolution Scale = 1x and Render Target Path = RTV, then report with the
    newest file from logs\ at the GitHub issues page.
  - Keep video mode at 1280x720 (the game's native mode); change the WINDOW
    size instead. Internal resolution is a separate setting (2x default).
