#!/usr/bin/env python3
"""Packs a directory tree into one file for the app VFS (port/rt/vfs.c, mounted at /cdda).
   usage: pack_dir.py OUT.pack ROOT SUBDIR... [--skip PREFIX]...
   Stored paths are relative to ROOT (e.g. data/json/items/tool.json). Layout:
   "RPACK001", u32 file count, u32 index offset, file data..., index (sorted by path):
   u16 path length, path bytes, u32 data offset, u32 size."""
import os, struct, sys

args = sys.argv[1:]
skips = []
while '--skip' in args:
    i = args.index('--skip')
    skips.append(args[i + 1])
    del args[i:i + 2]
out, root, subdirs = args[0], args[1], args[2:]

entries = []
for sub in subdirs:
    top = os.path.join(root, sub)
    if os.path.isfile(top):
        entries.append(sub.replace(os.sep, '/'))
        continue
    for dirpath, dirnames, filenames in os.walk(top):
        for name in filenames:
            rel = os.path.relpath(os.path.join(dirpath, name), root).replace(os.sep, '/')
            if any(rel.startswith(s) for s in skips):
                continue
            entries.append(rel)
entries.sort()

with open(out, 'wb') as f:
    f.write(b'RPACK001' + struct.pack('<II', 0, 0))
    index = []
    for rel in entries:
        data = open(os.path.join(root, rel), 'rb').read()
        index.append((rel, f.tell(), len(data)))
        f.write(data)
    ioff = f.tell()
    for rel, off, size in index:
        b = rel.encode()
        f.write(struct.pack('<H', len(b)) + b + struct.pack('<II', off, size))
    f.seek(8)
    f.write(struct.pack('<II', len(index), ioff))
print(f'{len(index)} files, {os.path.getsize(out) / 1e6:.1f} MB -> {out}')
