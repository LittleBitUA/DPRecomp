## Deadly Premonition Recompilation 1.4.3

Launcher hotfix on top of 1.4.2. Installs in place from the launcher (Update button) or unzip over any earlier version; saves and settings are kept.

### 📤 "Share Shader Cache" now actually shares it
Since 1.1 the launcher has offered to send the shader cache the game builds while you play to the project, so merged caches can ship with the next release. It turns out not a single upload ever arrived: the helper passed the upload's JSON on a command line, Windows PowerShell quietly stripped the quotes out of it, Discord rejected the request, and the helper swallowed the error. The JSON now travels through a file, the launcher only marks a cache as "shared" after the upload succeeded (so a failed send is retried next time), and if you had sharing switched on, your current cache goes out once on the next launcher start. Nothing else changes; it is still opt-in (Settings → Advanced → Share Shader Cache) and still only shader microcode and pipeline descriptions, no saves and no personal data.

Everything from 1.4.2 is included: the save-hang fix (#21), diagonal mouse aim (#30) and the native renderer preview under the Experimental tab (off by default).
