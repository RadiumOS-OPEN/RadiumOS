# Writes the small test images in tools/rbrowse/img (PNG RGBA, JPEG via cjpeg,
# GIF, BMP): python3 tools/rbrowse/mkimg.py tools/rbrowse/img (needs cjpeg).
# The kernel test page (src/kernel/createFiles.c) embeds the same bytes.
import sys, zlib, struct, math, subprocess, os
out = sys.argv[1]

def png(path, w, h, px, alpha=True):
    raw = b''
    for y in range(h):
        raw += b'\0' + b''.join(bytes(px(x, y)[:4 if alpha else 3]) for x in range(w))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6 if alpha else 2, 0, 0, 0))
    data += chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(path, 'wb').write(data)

# PNG: a sun with soft alpha edge over a transparent background, a bar of hues.
def sun(x, y):
    w, h = 96, 64
    d = math.hypot(x - 48, y - 30)
    if y >= 54:
        hue = x / w * 6
        i = int(hue) % 6; f = hue - int(hue)
        r, g, b = [(1,f,0),(1-f,1,0),(0,1,f),(0,1-f,1),(f,0,1),(1,0,1-f)][i]
        return (int(r*255), int(g*255), int(b*255), 255)
    a = max(0, min(255, int((26 - d) * 40)))
    return (255, 180 + int(60 * (1 - min(d, 26) / 26)), 0, a)
png(f'{out}/sun.png', 96, 64, sun)

# tiny data: URL image: 16x16 checker, opaque
png(f'{out}/check.png', 16, 16, lambda x, y: (200, 0, 0, 255) if (x // 4 + y // 4) % 2 else (255, 255, 255, 255), alpha=False)

# JPEG: a "landscape": sky gradient, hills, a lake; cjpeg from a PPM.
w, h = 160, 100
ppm = bytearray(b'P6 %d %d 255\n' % (w, h))
for y in range(h):
    for x in range(w):
        hill = 55 + 12 * math.sin(x / 17.0) + 6 * math.sin(x / 5.3)
        if y < hill:
            t = y / hill
            c = (int(70 + 120 * t), int(130 + 90 * t), 255)
            if math.hypot(x - 125, y - 22) < 11: c = (255, 240, 170)
        elif y < 78:
            c = (int(40 + 30 * math.sin(x / 9.0)), int(120 + (y - hill) * 2) % 256, 40)
        else:
            c = (30, int(80 + 20 * math.sin((x + y) / 4.0)), int(150 + (y - 78) * 3))
        ppm += bytes(c)
open(f'{out}/lake.ppm', 'wb').write(ppm)
subprocess.check_call(['cjpeg', '-quality', '80', '-outfile', f'{out}/lake.jpg', f'{out}/lake.ppm'])
subprocess.check_call(['cjpeg', '-quality', '80', '-progressive', '-outfile', f'{out}/lake_prog.jpg', f'{out}/lake.ppm'])

# GIF: 40x24, 4-colour stripes + diagonal, LZW with a clear code before the
# code size would grow (8-bit min code size, 9-bit codes).
gw, gh = 40, 24
pal = [(0, 0, 0), (255, 200, 0), (0, 120, 255), (255, 255, 255)] + [(0, 0, 0)] * 252
pix = [(3 if x == y + 8 else (1 if (y // 6) % 2 else 2)) for y in range(gh) for x in range(gw)]
codes = []
clear, eoi = 256, 257
for i, p in enumerate(pix):
    if i % 250 == 0: codes.append(clear)
    codes.append(p)
codes.append(eoi)
bits = 0; nb = 0; data = bytearray()
for c in codes:
    bits |= c << nb; nb += 9
    while nb >= 8: data.append(bits & 255); bits >>= 8; nb -= 8
if nb: data.append(bits & 255)
gif = b'GIF89a' + struct.pack('<HHBBB', gw, gh, 0xF7, 0, 0) + b''.join(bytes(c) for c in pal)
gif += b',' + struct.pack('<HHHHB', 0, 0, gw, gh, 0) + bytes([8])
for i in range(0, len(data), 255):
    blk = data[i:i + 255]; gif += bytes([len(blk)]) + blk
gif += b'\0;'
open(f'{out}/stripes.gif', 'wb').write(gif)

# BMP: 24-bit 30x20 bottom-up, a blue-to-green ramp
bw, bh = 30, 20
row = (bw * 3 + 3) & ~3
body = bytearray()
for y in range(bh - 1, -1, -1):
    r = bytearray()
    for x in range(bw): r += bytes((255 - x * 8, x * 8, 40))   # B, G, R
    body += r + b'\0' * (row - bw * 3)
hdr = b'BM' + struct.pack('<IHHI', 54 + len(body), 0, 0, 54) + struct.pack('<IiiHHIIiiII', 40, bw, bh, 1, 24, 0, len(body), 2835, 2835, 0, 0)
open(f'{out}/ramp.bmp', 'wb').write(hdr + body)
os.remove(f'{out}/lake.ppm')
for f in sorted(os.listdir(out)): print(f, os.path.getsize(f'{out}/{f}'))
