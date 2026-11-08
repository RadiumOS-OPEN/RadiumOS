#!/usr/bin/env python3
"""Build a RadiumOS update package (manifest.txt + files) that `update apply` installs.

  mkupdate.py --out dist/update [--kernel os.bin] [--files update/files] [--version TEXT]

--kernel  the kernel image; installed as /disk/radium/kernel.bin, which GRUB prefers over the ISO's kernel
--files   a folder mirroring /disk: update/files/apps/notes.hls is installed as /disk/apps/notes.hls
--sign-key  signs manifest.txt with a PRP (PRPSIG1) trailer; the manifest holds every file's SHA-256, so this signs the
          whole package including the kernel. Online updates are refused unless the signature checks out.
Packaged files get plain 8.3 names (F0001.BIN, ...) because the on-disk filesystem has no long names.
"""
import argparse, hashlib, os, re, shutil, subprocess, sys, time

# Files the user edits: installed only when they do not exist yet.
KEEP_IF_EXISTS = {".radiumrc"}
KERNEL_TARGET = "/disk/radium/kernel.bin"
MAX_FILE = 10 * 1024 * 1024   # update.c reads one file at a time into a 10 MB buffer, and the FAT volume is ~15 MB


def read_version():
    """The release version from the VERSION file at the repo root (e.g. 5.2)."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "VERSION")
    with open(path) as f:
        text = f.read().strip().lstrip("vV")
    if not re.fullmatch(r"\d+(\.\d+){0,3}", text):
        sys.exit("VERSION must look like 5.2 or 5.2.1, got %r" % text)
    return text


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--kernel")
    ap.add_argument("--files", default="update/files")
    ap.add_argument("--version", default=read_version())
    ap.add_argument("--sign-key")
    args = ap.parse_args()

    entries = []  # (source path, target, keep)
    if os.path.isdir(args.files):
        for root, _, names in sorted(os.walk(args.files)):
            for name in sorted(names):
                if name.startswith("."  ) and name not in KEEP_IF_EXISTS and name != ".gitkeep":
                    pass
                if name == ".gitkeep" or name == "README.txt" and root == args.files:
                    continue
                src = os.path.join(root, name)
                if os.path.getsize(src) > MAX_FILE:
                    print("  skipping %s: %d MB is more than an update package carries (%d MB); big files go in with `make doom-wad WAD=...` or `fetch -o`" % (src, os.path.getsize(src) >> 20, MAX_FILE >> 20))
                    continue
                rel = os.path.relpath(src, args.files).replace(os.sep, "/")
                bad = [p for p in rel.split("/") if not p.startswith(".") and not re.fullmatch(r"[^./]{1,8}(\.[^./]{1,3})?", p)]
                if bad:
                    print("  skipping %s: %r is not an 8.3 name (the /disk filesystem has no long names)" % (src, bad[0]))
                    continue
                entries.append((src, "/disk/" + rel, name in KEEP_IF_EXISTS))
    if args.kernel:
        entries.append((args.kernel, KERNEL_TARGET, False))  # last: apps are verified before the kernel

    if os.path.isdir(args.out):
        shutil.rmtree(args.out)
    os.makedirs(args.out)

    lines = ["radiumos-update 1", "version " + args.version]
    for i, (src, target, keep) in enumerate(entries, 1):
        packaged = "F%04d.BIN" % i
        shutil.copyfile(src, os.path.join(args.out, packaged))
        size = os.path.getsize(src)
        lines.append("file %s %d %s %s%s" % (sha256(src), size, target, packaged, " keep" if keep else ""))
        print("  %-44s %9d bytes  ->  %s" % (target, size, packaged))

    manifest = os.path.join(args.out, "manifest.txt")
    with open(manifest, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    signed = "UNSIGNED"
    if args.sign_key:
        subprocess.check_call([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "prp_sign.py"),
                               "sign", manifest, "--key", args.sign_key])
        signed = "signed"
    print("wrote %s (%d files, version %s, %s)" % (manifest, len(entries), args.version, signed))


if __name__ == "__main__":
    sys.exit(main())
