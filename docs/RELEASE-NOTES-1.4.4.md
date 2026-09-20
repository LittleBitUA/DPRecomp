## Deadly Premonition Recompilation 1.4.4

Small update on top of 1.4.3. Installs in place from the launcher (Update button) or unzip over any earlier version; saves and settings are kept.

### 🔊 Audio Output: Auto / Stereo / Surround
The game mixes 5.1 internally, exactly as it did on the Xbox 360, and the port hands those six channels to whatever your default Windows output device is. That is right when the device really is a 5.1 or 7.1 setup, and wrong when Windows *says* 5.1 but stereo speakers or headphones are plugged in: the dialogue lives in the center channel, and there is no speaker for it, so voices go quiet while music and effects do not (#19, via a Reddit report).

Launcher → Advanced → **Audio Output**:
- **Auto** (default): follow the Windows device, as before.
- **Stereo**: always fold 5.1 down to 2.0 inside the port (center and surrounds at -3 dB, LFE left out, the standard ITU/Dolby fold). Pick this if dialogue is quiet or missing.
- **Surround**: always send the six channels, whatever Windows reports.

The choice is `audio_channels` in `deadlyprem.toml`; the game log shows the result on the `audio endpoint` line.

Everything from 1.4.3 and 1.4.2 is included: the working Share Shader Cache, the save-hang fix (#21), diagonal mouse aim (#30) and the native renderer preview under the Experimental tab (off by default).
