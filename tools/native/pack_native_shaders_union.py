# -*- coding: utf-8 -*-
# Same cache format as pack_native_shaders.py, but unions several XenosRecomp
# runs (the fork's DXC step fails a few random shaders per run with
# "bad allocation"): runs are shader_stage/out/run_<n>/dp_shader_cache.cpp(.bin).
# Usage: python pack_native_shaders_union.py <run_dir> [<run_dir> ...]
import re, struct, sys, os
STAGE = r"E:\XboxDP\DPProject\docs\native_render_deep_2026-09-18\shader_stage\out"
DEST = [os.path.join(STAGE, "dp_native_shaders.bin"), r"E:\XboxDP\dist\deadlyprem-nightly\native\dp_native_shaders.bin"]
best = {}  # hash -> (is_pixel, dxil bytes)
for run in sys.argv[1:]:
    cpp = open(os.path.join(run, "dp_shader_cache.cpp"), encoding="utf-8").read()
    dxil = open(os.path.join(run, "dp_shader_cache.cpp.bin"), "rb").read()
    rows = re.findall(r"\{\s*0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)(?:,\s*(\d+))?\s*\}", cpp)
    added = 0
    for h, uh, off, size, so, ss, spec, ispix, veo, vec, hostflags in rows:
        size = int(size); off = int(off)
        if size == 0:
            continue
        hv = int(h, 16)
        if hv in best:
            continue
        assert dxil[off:off + 4] == b"DXBC", (run, h)
        best[hv] = (int(ispix), dxil[off:off + size], int(hostflags or 0))
        added += 1
    print(run, "rows", len(rows), "added", added)
entries = sorted(best.items())
blob_dxil = bytearray()
table = []
for hv, (ispix, data, hostflags) in entries:
    off = len(blob_dxil)
    blob_dxil += data
    while len(blob_dxil) % 16:
        blob_dxil += b"\0"
    table.append((hv, off, len(data), ispix, hostflags))
blob = bytearray(b"DPNS0001" + struct.pack("<II", len(table), len(blob_dxil)))
for hv, off, size, ispix, hostflags in table:
    blob += struct.pack("<QIIII", hv, off, size, ispix, hostflags)
blob += blob_dxil
# [NEW FABLE VERSION] 2026-09-23: never write into dist while the game runs
# (rule: dist is untouched until the player closes the game; 23.09 23:5x this
# script overwrote the live cache). The stage copy is always written.
import subprocess
running = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout.lower()
game_running = "deadlyprem.exe" in running or "deadlyprem_usa.exe" in running
for dest in DEST:
    if game_running and "dist" in dest.lower():
        print("SKIPPED (game running, deploy after it exits):", dest)
        continue
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    open(dest, "wb").write(blob)
    print(dest, len(blob))
print("entries", len(table), "vs", sum(1 for e in table if e[3] == 0), "ps", sum(1 for e in table if e[3] == 1))
