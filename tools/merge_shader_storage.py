#!/usr/bin/env python3
"""Merge ReXGlue shader storage files collected from players.

Usage:
  merge_shader_storage.py OUT_DIR IN_1 [IN_2 ...]

Inputs are .xsh (shader microcode) and .xpso (pipeline descriptions) files or
.zip archives containing them (what the launcher's "Share Shader Cache"
option uploads). Files are grouped by name (e.g. 4D5607D1.xsh,
4D5607D1.rov.d3d12.xpso) and merged per name into OUT_DIR, deduplicating
entries by hash. Headers must match (same storage format version); files with
a different header are skipped with a warning.

Formats (rexglue-sdk src/graphics/d3d12/pipeline_cache.cpp):
  .xsh  : u32 magic 'XESH', u32 version(be); then entries
          { u64 ucode_data_hash; u32 dword_count:31|type:1; (padding to 16) }
          followed by dword_count * 4 bytes of ucode (guest endian).
  .xpso : u32 magic 'XEPS', u32 magic_api, u32 version(be); then fixed
          records { u64 description_hash; PipelineDescription (64 bytes) }.
The entry header size of .xsh is auto-detected (16 or 12 bytes) by walking
the file; a file that does not land exactly on EOF is truncated at the last
valid entry, like the runtime does.
"""
import io
import os
import struct
import sys
import zipfile
from collections import OrderedDict

XPSO_HEADER = 12
XPSO_RECORD = 72
XSH_HEADER = 8


def walk_xsh(data, entry_header):
    """Yield (hash, entry_bytes) for each entry; stop at the first bad entry."""
    pos = XSH_HEADER
    n = len(data)
    while pos + entry_header <= n:
        ucode_hash, packed = struct.unpack_from('<QI', data, pos)
        dword_count = packed & 0x7FFFFFFF
        size = entry_header + dword_count * 4
        if dword_count == 0 or pos + size > n:
            return
        yield ucode_hash, data[pos:pos + size]
        pos += size
    return


def parse_xsh(data):
    if len(data) < XSH_HEADER:
        return None, []
    header = data[:XSH_HEADER]
    best = None
    for eh in (16, 12):
        entries = list(walk_xsh(data, eh))
        consumed = XSH_HEADER + sum(len(e[1]) for e in entries)
        if entries and consumed == len(data):
            return header, entries
        if best is None or len(entries) > len(best[1]):
            best = (header, entries)
    return best


def parse_xpso(data):
    if len(data) < XPSO_HEADER:
        return None, []
    header = data[:XPSO_HEADER]
    entries = []
    pos = XPSO_HEADER
    while pos + XPSO_RECORD <= len(data):
        rec = data[pos:pos + XPSO_RECORD]
        entries.append((struct.unpack_from('<Q', rec)[0], rec))
        pos += XPSO_RECORD
    return header, entries


def collect(paths):
    """Return {filename: [(source, bytes), ...]}."""
    groups = OrderedDict()
    for p in paths:
        if p.lower().endswith('.zip'):
            with zipfile.ZipFile(p) as z:
                for name in z.namelist():
                    base = os.path.basename(name)
                    if base.endswith(('.xsh', '.xpso')):
                        groups.setdefault(base, []).append((f'{p}:{name}', z.read(name)))
        elif p.lower().endswith(('.xsh', '.xpso')):
            with open(p, 'rb') as f:
                groups.setdefault(os.path.basename(p), []).append((p, f.read()))
        elif os.path.isdir(p):
            for root, _, files in os.walk(p):
                for fn in files:
                    if fn.endswith(('.xsh', '.xpso', '.zip')):
                        for k, v in collect([os.path.join(root, fn)]).items():
                            groups.setdefault(k, []).extend(v)
    return groups


def merge_group(name, sources):
    parse = parse_xsh if name.endswith('.xsh') else parse_xpso
    header = None
    merged = OrderedDict()
    for src, data in sources:
        h, entries = parse(data)
        if h is None:
            print(f'  skip {src}: too short')
            continue
        if header is None:
            header = h
        elif h != header:
            print(f'  skip {src}: header mismatch (different storage version)')
            continue
        new = 0
        for key, blob in entries:
            if key not in merged:
                merged[key] = blob
                new += 1
        print(f'  {src}: {len(entries)} entries, {new} new')
    if header is None:
        return None
    out = io.BytesIO()
    out.write(header)
    for blob in merged.values():
        out.write(blob)
    return out.getvalue(), len(merged)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    out_dir = argv[1]
    os.makedirs(out_dir, exist_ok=True)
    groups = collect(argv[2:])
    if not groups:
        print('no .xsh/.xpso inputs found')
        return 1
    for name, sources in groups.items():
        print(f'{name}:')
        result = merge_group(name, sources)
        if result is None:
            continue
        data, count = result
        out_path = os.path.join(out_dir, name)
        with open(out_path, 'wb') as f:
            f.write(data)
        print(f'  -> {out_path}: {count} entries, {len(data)} bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
