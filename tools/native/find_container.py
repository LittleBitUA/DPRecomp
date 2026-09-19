# -*- coding: utf-8 -*-
# Find compiled.pkg containers by XXH3 container hash, extract them and emit HLSL via XenosRecomp.
import struct, sys, os, subprocess, xxhash
PKG = r"E:\XboxDP\dist\deadlyprem-portable\assets\updata\pack\compiled.pkg"
OUT = r"E:\XboxDP\DPProject\docs\native_render_deep_2026-09-18\shader_stage\hlsl"
TOOL = r"E:\XboxDP\XenosRecomp\build\XenosRecomp\XenosRecomp.exe"
COMMON = r"E:\XboxDP\XenosRecomp\XenosRecomp\shader_common.h"
os.makedirs(OUT, exist_ok=True)
data = open(PKG, 'rb').read()
wanted = {int(h, 16) for h in sys.argv[1:]}
i = 0
found = 0
while i < len(data) - 36:
    magic = struct.unpack('>I', data[i:i+4])[0]
    if magic in (0x102A1100, 0x102A1101):
        vs, ps = struct.unpack('>II', data[i+4:i+12])
        size = vs + ps
        if 0 < size < 0x100000 and i + size <= len(data):
            h = xxhash.xxh3_64_intdigest(data[i:i+size])
            if h in wanted:
                name = f"{h:016X}_{'vs' if magic & 1 else 'ps'}"
                bin_path = os.path.join(OUT, name + ".bin")
                open(bin_path, 'wb').write(data[i:i+size])
                hlsl = os.path.join(OUT, name + ".hlsl")
                r = subprocess.run([TOOL, bin_path, hlsl, COMMON], capture_output=True, text=True)
                print(name, "pkg offset", hex(i), "size", size, "->", hlsl, r.returncode)
                found += 1
            i += 4
            continue
    i += 4
print("found", found, "of", len(wanted))
