# -*- coding: utf-8 -*-
# Pack the XenosRecomp (dpxr-fork) output into the DP1 native renderer's shader
# cache: dp_native_shaders.bin = "DPNS0001" | u32 count | u32 dxil_size |
# entries[count] { u64 container_hash; u32 dxil_offset; u32 dxil_size; u32 is_pixel; u32 reserved }
# | dxil bytes.  Only successfully compiled shaders are kept.
import re, struct, sys, os
OUT_DIR = r"E:\XboxDP\DPProject\docs\native_render_deep_2026-09-18\shader_stage\out"
cpp = open(os.path.join(OUT_DIR, "dp_shader_cache.cpp"), encoding='utf-8').read()
dxil = open(os.path.join(OUT_DIR, "dp_shader_cache.cpp.bin"), 'rb').read()
rows = re.findall(r"\{\s*0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)(?:,\s*(\d+))?\s*\}", cpp)
entries = []
seen = set()
for h, uh, off, size, so, ss, spec, ispix, veo, vec, hostflags in rows:
    size = int(size); off = int(off)
    if size == 0:
        continue
    hv = int(h, 16)
    assert hv not in seen
    seen.add(hv)
    assert off + size <= len(dxil), (off, size, len(dxil))
    assert dxil[off:off+4] == b'DXBC', (h, dxil[off:off+4])
    entries.append((hv, off, size, int(ispix), int(hostflags or 0)))
entries.sort()
blob = bytearray()
blob += b"DPNS0001" + struct.pack("<II", len(entries), len(dxil))
for hv, off, size, ispix, hostflags in entries:
    blob += struct.pack("<QIIII", hv, off, size, ispix, hostflags)
blob += dxil
for dest in [os.path.join(OUT_DIR, "dp_native_shaders.bin"), r"E:\XboxDP\dist\deadlyprem-nightly\native\dp_native_shaders.bin"]:
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    open(dest, 'wb').write(blob)
    print(dest, len(blob))
print("entries", len(entries), "vs", sum(1 for e in entries if e[3] == 0), "ps", sum(1 for e in entries if e[3] == 1))
