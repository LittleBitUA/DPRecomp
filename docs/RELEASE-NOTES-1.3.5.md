## Deadly Premonition Recompilation 1.3.5

Hotfix for #22, the GPU device removal while tailing Nick to the art gallery. Installs in place from the launcher (Update button) or unzip over any earlier version; saves and settings are kept.

### 🧾 What it was
The debug-layer drain added in 1.3.3 caught it on the reporter's machine: a `CopyTextureRegion` in the texture loader copied a 64×64 mip into a 32×32 level of an R8G8B8A8 texture. Direct3D 12 refuses to close a command list after that, the next `ExecuteCommandLists` is an illegal call, and the device is removed with `DXGI_ERROR_INVALID_CALL`. Not VRAM, not the driver.

### 🩹 What changed
- Both texture copy paths (guest texture loads and PNG replacements) check the source extent against the destination level before recording the copy. If it does not fit, the copy is clamped to the level and the texture is logged once: `Texture load: copy of 64x64x1 into level N of a WxHxD (mips, format) texture ... clamped to the level (DPRecomp #22)`, plus the guest size, format, mip count, packed level and the source box.
- The frame renders with a partial mip on that one texture instead of the game dying. A `gpu/texture_cache/copies_clamped` gauge shows up in the `[stats]` line when it happened.
- This is a guard, not the final fix: the logged texture tells us which layout case the loader gets wrong. If you hit the section, please attach the log so we can finish it.

GPU plugin DLL only; everything else is 1.3.4.
