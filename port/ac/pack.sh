#!/bin/bash
# Builds ac.app and the data pack for RadiumOS (see port/ac/Makefile):  port/ac/pack.sh [AC_DIR]
#   then copy out/ac.app and out/ac.pak to /disk (the FAT volume only allows 8.3 names) and run:  app /disk/ac.app
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
AC_DIR=${1:-$HERE/assaultcube}
make -C "$HERE" AC_DIR="$AC_DIR" -j"$(nproc)"
python3 "$HERE/../tools/pack_dir.py" "$HERE/out/ac.pak" "$AC_DIR" config packages bot --skip packages/audio
ls -la "$HERE/out/ac.app" "$HERE/out/ac.pak"
