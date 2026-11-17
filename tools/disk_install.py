#!/usr/bin/env python3
"""Install an update package (dist/update) straight into a mounted /disk volume.

usage: disk_install.py MOUNTPOINT PACKAGE_DIR

Same result as running `update apply` inside RadiumOS, minus the signature check
(the package was just built locally). The kernel goes to radium/kernel.bin and
the one it replaces is kept as kernel.old.
"""
import os
import shutil
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    mnt, pkg = sys.argv[1], sys.argv[2]
    version, entries = None, []
    for line in open(os.path.join(pkg, "manifest.txt"), encoding="latin-1"):
        p = line.split()
        if len(p) == 2 and p[0] == "version":
            version = p[1]
        elif len(p) == 5 and p[0] == "file":
            entries.append((p[3], p[4]))

    n = 0
    for target, stage in entries:
        if not target.startswith("/disk/"):
            continue
        dst = os.path.join(mnt, target[len("/disk/"):])
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if target == "/disk/radium/kernel.bin" and os.path.exists(dst):
            shutil.copyfile(dst, os.path.join(os.path.dirname(dst), "kernel.old"))
        shutil.copyfile(os.path.join(pkg, stage), dst)
        n += 1
    if version:
        os.makedirs(os.path.join(mnt, "radium"), exist_ok=True)
        with open(os.path.join(mnt, "radium", "version.txt"), "w") as f:
            f.write(version)
    print("installed %d file(s), version %s" % (n, version))


if __name__ == "__main__":
    main()
