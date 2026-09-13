## Deadly Premonition Recompilation 1.3.6

Hotfix for 1.3.5, which dies about a minute into any session. Installs in place from the launcher (Update button) or unzip over any earlier version; saves and settings are kept. If you are on 1.3.5, update now.

### 🧾 What it was
The copy guard added in 1.3.5 for #22 checked every texture upload against the raw texel size of the destination mip level. DXT textures are addressed in 4×4 blocks: the last mips are 2×2 and 1×1 texels but still one whole block, and copying a 4×4 block into them is legal. The guard clamped those copies to 2×2 and 1×1, which is not block-aligned, and Direct3D 12 removed the device with `INVALID_CALL` - the exact thing the guard was meant to prevent. It fires on the title screen when the attract demo loads (reproduced locally at 60 seconds), and in the gameplay load reported in #22.

### 🩹 What changed
- The guard works in whole blocks of the resource format (BC1-7 4×4, packed 4:2:2 2×1, everything else 1×1). A copy that fits is never touched; a copy that does not is clamped to a block-aligned extent and logged with the block size and the clamped size.
- The math is a standalone header with unit tests covering the two copies from the #22 log 4, the original 64×64-into-32×32 case, and alignment of the clamped box.
- 1.3.5 remains the last hotfix for the original #22 device removal (a 64×64 R8G8B8A8 copy into a 32×32 level): that guard still holds, and the log still names the texture when it happens. If you reach the art gallery on 1.3.6, please attach the log.

GPU plugin DLL only; everything else is 1.3.4.
