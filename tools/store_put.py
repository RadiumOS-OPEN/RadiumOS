#!/usr/bin/env python3
"""Copy a file into the fetch store of a RadiumOS disk image (what `fetch -o /disk/NAME` writes in the guest).

  store_put.py fetchdisk.img /disk/doom1.wad ~/doom1.wad

The store lives at sector 65536 on, inside the volume's hidden STORE.BIN (224 MiB): sector 0 is "RDFETCH1" + the next free sector, sectors 1-9 hold
32 directory entries (128-byte name, start sector, size), file data follows from sector 16. In RadiumOS, programs that
read /disk/NAME (doom, mp4) find the file there when it is not on the FAT volume. Close QEMU first.
"""
import os, struct, sys

BASE = 65536 * 512
SECTOR = 512
ENTRIES, ENTRY_SIZE, DIR_SECTORS, DATA_START = 32, 136, 9, 16
STORE_SECTORS = 458752          # keep equal to STORE_SECTORS in src/Avfs/Avfs.c
MAGIC = b"RDFETCH1"


def main():
    if len(sys.argv) != 4 or not sys.argv[2].startswith("/disk/"):
        sys.exit("usage: store_put.py IMAGE /disk/NAME FILE")
    image, name, path = sys.argv[1], sys.argv[2][6:].encode(), sys.argv[3]
    if not name or len(name) >= 128:
        sys.exit("bad name")
    data = open(path, "rb").read()
    with open(image, "r+b") as f:
        f.seek(BASE)
        head = f.read(SECTOR)
        if head[:8] != MAGIC:
            head = MAGIC + struct.pack("<I", DATA_START) + bytes(SECTOR - 12)
            f.seek(BASE)
            f.write(head)
            f.write(bytes(SECTOR * DIR_SECTORS))
        directory = bytearray(f.read(0) or b"")
        f.seek(BASE + SECTOR)
        directory = bytearray(f.read(SECTOR * DIR_SECTORS))
        free = slot = None
        high = DATA_START
        for i in range(ENTRIES):
            e = directory[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE]
            if e[0] == 0:
                free = i if free is None else free
                continue
            start, size = struct.unpack("<II", e[128:136])
            high = max(high, start + (size + SECTOR - 1) // SECTOR)
            if e[:128].split(b"\0")[0] == name:
                slot = i
        slot = slot if slot is not None else free
        if slot is None:
            sys.exit("the store directory is full")
        sectors = (len(data) + SECTOR - 1) // SECTOR
        if (high + sectors) > STORE_SECTORS or BASE + (high + sectors) * SECTOR > os.path.getsize(image):
            sys.exit("the store (224 MiB, the hidden STORE.BIN of the volume) is too small for this file")
        f.seek(BASE + high * SECTOR)
        f.write(data + bytes(sectors * SECTOR - len(data)))
        directory[slot * ENTRY_SIZE:(slot + 1) * ENTRY_SIZE] = name.ljust(128, b"\0") + struct.pack("<II", high, len(data))
        f.seek(BASE)
        f.write(MAGIC + struct.pack("<I", high + sectors))
        f.seek(BASE + SECTOR)
        f.write(directory)
    print("stored %s (%d bytes) in %s: /disk/%s" % (path, len(data), image, name.decode()))


if __name__ == "__main__":
    main()
