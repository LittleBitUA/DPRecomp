## Deadly Premonition Recompilation 1.3.1

Hotfix for the audio that died in the middle of cutscenes (#16). Installs in place from the launcher (Update button) or unzip over any earlier version; saves and settings are kept. Steam Deck: the in-launcher updater still does not apply under Proton (it relies on PowerShell), so unzip the release over your folder by hand for now.

### 🔊 Sound that stopped a few seconds into a cutscene (#16)
- Symptom: in the morgue cutscene after examining Anna, in the tree-profiling scene and in the cutscene after the lumbermill nightmare, every sound (voices, music, effects) stopped a few seconds in and never came back while the game kept running. Reported on 0.1, 1.0, 1.1 and 1.3, both regions.
- Cause: the runtime's XMA decoder was a snapshot of Xenia-canary's from before May 2026. Since then canary fixed five things in the handshake between the decoder and the game's audio engine: the output buffer was flagged as finished too early; the last partial frame of a stream was never delivered once the decoder went idle, and the game's audio thread waits for that frame forever, so everything downstream starves; the stall detector some titles poll never fired; and the walk to the next packet could step into a different sub-stream inside an interleaved buffer. The two logs SilentHeII attached pointed straight at it: after the drop the audio device kept receiving frames from the game's mixer, so the mixer was alive and its inputs were gone.
- Fix: all five upstream changes are ported (`xma_context.cpp`, the diff is in `docs/sdk-patches`). Verified on the reporter's own save: the morgue scene now plays through with sound, and the decoder error that used to mark the moment of the drop is gone from the log.
- Thanks to SilentHeII and michaelbub for the saves and the logs. If any scene still loses sound, attach `logs\deadlyprem_XXX.log` from that session to #16.

### 🧩 Under the hood
- Only the runtime DLL changed (`rexruntimerd.dll`). The launcher and both game executables carry the 1.3.1 version string; nothing else moved since 1.3.

### Known issues
- Steam Deck / Proton: the launcher's Update button downloads but does not apply (PowerShell). Unzip manually; a PowerShell-free updater is next.
- 60 FPS: some animation-keyed sounds play twice and a few prop attach events misfire (#17, #18); the experimental `dp_anim_event_dedupe` switch is still off by default.
- Terrain layer blending shows hard triangle edges while walking and blends correctly when standing still (long-standing, under investigation).
- Some dialogue scenes show light-blue polygonal patches on characters with the ROV path (the scene's fog colour bleeding through the composite mask). Being investigated with RenderDoc.
- 2x internal resolution: post-process passes are sized for 1x (fine grid on bright edges, softer blur). Internal Resolution 1x does not have it.
- Clouds with hard edges (#10).
- Linux is not supported yet.
