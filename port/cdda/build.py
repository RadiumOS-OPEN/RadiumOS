#!/usr/bin/env python3
"""Generates build.ninja for Cataclysm: DDA on RadiumOS and runs ninja.
   usage: port/cdda/build.py [CDDA_DIR]   (default /tmp/cxx/cdda); output: port/out/cdda.app"""
import os, subprocess, sys, glob

WORK = os.environ.get('WORK', '/tmp/cxx')
CDDA = sys.argv[1] if len(sys.argv) > 1 else WORK + '/cdda'
HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
SYSROOT = WORK + '/sysroot'
OUT = WORK + '/cdda-build'
TARGET = '--target=i686-linux-musl --sysroot=' + SYSROOT
ARCH = '-march=i686 -mno-sse -mno-mmx -fno-pic -fno-pie -fno-stack-protector -funwind-tables'
DEFS = ('-D_DEFAULT_SOURCE -D_XOPEN_SOURCE=600 -DNO_STALE_DATA_WARN -DRELEASE -DZSTD_STATIC_LINKING_ONLY '
        '-DZSTD_DISABLE_ASM -DFMT_USE_LOCALE=0 -DIMTUI -DGIT_VERSION')
INC = f'-I{CDDA}/src -isystem {CDDA}/src/third-party -isystem {PORT}/curses'
CXXFLAGS = f'{TARGET} {ARCH} -O2 -std=c++17 -fsigned-char -w -nostdinc++ -isystem {SYSROOT}/usr/include/c++/v1 {DEFS} {INC}'
CFLAGS = f'{TARGET} {ARCH} -O2 -std=c17 -w {DEFS} {INC}'

def rel(p): return os.path.relpath(p, CDDA)

cxx = sorted(glob.glob(CDDA + '/src/*.cpp'))
cxx += sorted(glob.glob(CDDA + '/src/third-party/flatbuffers/*.cpp')) + sorted(glob.glob(CDDA + '/src/third-party/fmt/*.cc'))
imgui = ['imgui', 'imgui_demo', 'imgui_draw', 'imgui_stdlib', 'imgui_tables', 'imgui_widgets']
cxx += [f'{CDDA}/src/third-party/imgui/{n}.cpp' for n in imgui]
cxx += [f'{CDDA}/src/third-party/imtui/imtui-impl-ncurses.cpp', f'{CDDA}/src/third-party/imtui/imtui-impl-text.cpp']
cxx = [p for p in cxx if not p.endswith(('sdltiles.cpp', 'sdlsound.cpp', 'sdl_wrappers.cpp', 'sdl_font.cpp', 'sdl_geometry.cpp',
                                        'sdl_quad_batch.cpp', 'sdl_utils.cpp', 'sdl_gamepad.cpp', 'sound_backend_sdl3.cpp',
                                        'cursesport.cpp'))]
c = sorted(glob.glob(CDDA + '/src/third-party/zstd/*/*.c')) + [CDDA + '/src/cata_allocator_c.c']
extra_cxx = [PORT + '/curses/radium_curses.cpp', PORT + '/cdda/radium_platform.cpp']

os.makedirs(OUT, exist_ok=True)
lines = [f'cxxflags = {CXXFLAGS}', f'cflags = {CFLAGS}', '',
         'rule cxx\n  command = clang++ $cxxflags -MMD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\n  description = CXX $in',
         'rule cc\n  command = clang $cflags -MMD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\n  description = CC $in',
         'rule link\n  command = clang++ ' + TARGET + ' -static -nostartfiles -nostdlib -fuse-ld=lld -Wl,--image-base=0x10000000 '
         '-Wl,-e,radium_app_main -Wl,--gc-sections -Wl,--build-id=none -Wl,-z,norelro -Wl,--start-group $in '
         f'-L{SYSROOT}/usr/lib -lz -lc++ -lc++abi -lunwind -lc {WORK}/lib/libbuiltins.a -Wl,--end-group -o $out\n  description = LINK $out',
         '']
objs = []
for src in cxx + extra_cxx:
    o = os.path.join(OUT, os.path.relpath(src, '/')).replace('/', '_') + '.o'
    lines.append(f'build {o}: cxx {src}')
    objs.append(o)
for src in c:
    o = os.path.join(OUT, os.path.relpath(src, '/')).replace('/', '_') + '.o'
    lines.append(f'build {o}: cc {src}')
    objs.append(o)
glue = OUT + '/glue.o'
lines.append(f'build {glue}: cc {PORT}/rt/glue.c')
objs.append(glue)
vfs = OUT + '/vfs.o'
lines.append(f'build {vfs}: cc {PORT}/rt/vfs.c')
objs.append(vfs)
os.makedirs(PORT + '/out', exist_ok=True)
lines.append('build ' + PORT + '/out/cdda.app: link ' + ' '.join(objs))
open(OUT + '/build.ninja', 'w').write('\n'.join(lines) + '\n')
sys.exit(subprocess.call(['ninja', '-C', OUT, '-k', '0'] + sys.argv[2:]))
