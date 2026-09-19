## Deadly Premonition Recompilation 1.4.2

Two fixes from the tracker and a preview. Installs in place from the launcher (Update button) or unzip over any earlier version; saves and settings are kept.

### 💾 The endless "Saving" screen (#21)
Rarely, saving would sit on "Saving" forever, the old save gone and no new one written. The log from the report named the step: the runtime removes the old save folder before writing the new container, and that removal failed while another program (real-time antivirus, an indexer) held a file inside it for a moment. The game's own save routine then waits forever, because on a console that call cannot fail that way. The runtime now retries the removal for a few seconds, then overwrites the folder in place, and the log says which path and which Windows error got in the way. If it still has to give up, the game is told "storage device not connected", which it knows how to show, so the save screen ends and you can try again.

### 🎯 Diagonal mouse aim (#30)
Aiming diagonally with the mouse pulled toward horizontal or vertical in a staircase. The aim routine reads the stick per axis; the mouse mapping now clears both deadzones at once while aiming (launcher → Mouse → "Aim: per-axis mouse floor", on by default; turn it off to get the previous behaviour).

### 🧪 Native renderer preview (Experimental tab)
The launcher has a new Experimental tab, and the first thing in it is the native D3D12 renderer that is being built to replace the GPU emulation. It is off by default and stays off unless you switch it on; the stock path is byte for byte what 1.4.1 ran. Switched on, it draws the world, York, the menus and the movies, but it is not finished: the floor comes out white, walls are too bright, sun shadows are partial, it is PAL-only and slower than the emulator right now. Have a look if you are curious, switch it off to play.

Everything from 1.4.1 is included.
