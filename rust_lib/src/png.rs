//! PNG decoder + Mode 13h blit. Follows the ARIMG FFI shape in lib.rs.
//!
//! The C malloc pool is 1 KB, so this path never heap-allocates. Inflate
//! streams into a 32 KB window and nearest-neighbor downscales each
//! scanline into the 320x200 SCREEN as it arrives.

use crate::{
    avfs_create_file, avfs_get_filesize, avfs_read_file, avfs_remove_file, avfs_write_file,
    keyboard_wait_for_key, rust_print,
};

extern "C" {
    fn vga_set_mode13h();
    fn vga_dac_set_palette(rgb: *const u8, count: i32);
    fn vga_restore_text_80x50();
}

const MODE13_W: usize = 320;
const MODE13_H: usize = 200;
const MODE13_PIXELS: usize = MODE13_W * MODE13_H;
const PNG_SIG: [u8; 8] = [137, 80, 78, 71, 13, 10, 26, 10];
const PNG_MAX_FILE: u32 = 768 * 1024;
const PNG_MAX_DIM: u32 = 4096;
const MAX_STRIDE: usize = 1 + 4096 * 4;
const WIN_SIZE: usize = 32768;

static mut SCREEN: [u8; MODE13_PIXELS] = [0; MODE13_PIXELS];
static mut PALETTE: [u8; 256 * 3] = [0; 256 * 3];
static mut TEST_PNG_BUFFER: [u8; 16384] = [0; 16384];
static mut FILE_BUF: [u8; PNG_MAX_FILE as usize] = [0; PNG_MAX_FILE as usize];
static mut WIN: [u8; WIN_SIZE] = [0; WIN_SIZE];
static mut SCAN: [u8; MAX_STRIDE] = [0; MAX_STRIDE];
static mut PREV: [u8; MAX_STRIDE] = [0; MAX_STRIDE];

const LEN_BASE: [u16; 29] = [
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131,
    163, 195, 227, 258,
];
const LEN_EXTRA: [u8; 29] = [
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
];
const DIST_BASE: [u16; 30] = [
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049,
    3073, 4097, 6145, 8193, 12289, 16385, 24577,
];
const DIST_EXTRA: [u8; 30] = [
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
];
const CLEN_ORDER: [u8; 19] = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];

struct BitReader<'a> {
    png: &'a [u8],
    spans: [(usize, usize); 32],
    span_n: usize,
    idat_len: usize,
    pos: usize,
    bitbuf: u32,
    bitcnt: u32,
}

impl<'a> BitReader<'a> {
    fn next_byte(&mut self) -> Result<u8, ()> {
        if self.pos >= self.idat_len {
            return Err(());
        }
        let mut off = self.pos;
        self.pos += 1;
        for i in 0..self.span_n {
            let (start, n) = self.spans[i];
            if off < n {
                return Ok(self.png[start + off]);
            }
            off -= n;
        }
        Err(())
    }

    fn get_bits(&mut self, n: u32) -> Result<u32, ()> {
        if n == 0 {
            return Ok(0);
        }
        if n > 16 {
            return Err(());
        }
        while self.bitcnt < n {
            let b = self.next_byte()? as u32;
            self.bitbuf |= b << self.bitcnt;
            self.bitcnt += 8;
        }
        let val = self.bitbuf & ((1u32 << n) - 1);
        self.bitbuf >>= n;
        self.bitcnt -= n;
        Ok(val)
    }

    fn align_byte(&mut self) {
        self.bitbuf = 0;
        self.bitcnt = 0;
    }

    fn read_u16_le(&mut self) -> Result<u16, ()> {
        self.align_byte();
        let lo = self.next_byte()? as u16;
        let hi = self.next_byte()? as u16;
        Ok(lo | (hi << 8))
    }
}

struct Huffman {
    count: [u16; 16],
    symbol: [u16; 288],
}

impl Huffman {
    fn empty() -> Self {
        Self {
            count: [0; 16],
            symbol: [0; 288],
        }
    }
}

fn construct(h: &mut Huffman, lengths: &[u8]) -> Result<(), ()> {
    h.count = [0; 16];
    for &l in lengths {
        if l > 15 {
            return Err(());
        }
        h.count[l as usize] += 1;
    }
    h.count[0] = 0;

    let mut left = 1u32;
    for len in 1..=15 {
        left <<= 1;
        let c = h.count[len] as u32;
        if left < c {
            return Err(());
        }
        left -= c;
    }

    let mut offs = [0u16; 16];
    for len in 1..15 {
        offs[len + 1] = offs[len].wrapping_add(h.count[len]);
    }
    for (sym, &l) in lengths.iter().enumerate() {
        if l != 0 {
            let i = offs[l as usize] as usize;
            if i >= h.symbol.len() {
                return Err(());
            }
            h.symbol[i] = sym as u16;
            offs[l as usize] += 1;
        }
    }
    Ok(())
}

fn huffman_decode(br: &mut BitReader, h: &Huffman) -> Result<u16, ()> {
    let mut code: i32 = 0;
    let mut first: i32 = 0;
    let mut index: i32 = 0;
    for len in 1..=15 {
        code |= br.get_bits(1)? as i32;
        let count = h.count[len] as i32;
        if code - first < count {
            let si = (index + (code - first)) as usize;
            if si >= h.symbol.len() {
                return Err(());
            }
            return Ok(h.symbol[si]);
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    Err(())
}

struct PngSink {
    width: usize,
    height: usize,
    bpp: usize,
    color_type: u8,
    stride: usize,
    ox: usize,
    oy: usize,
    dw: usize,
    dh: usize,
    row: usize,
    col: usize,
    emitted: usize,
    drawn: i32,
}

impl PngSink {
    fn emit(&mut self, b: u8) -> Result<(), ()> {
        unsafe {
            WIN[self.emitted % WIN_SIZE] = b;
        }
        self.emitted += 1;
        if self.row >= self.height {
            return Ok(());
        }
        if self.col >= self.stride {
            return Err(());
        }
        unsafe {
            SCAN[self.col] = b;
        }
        self.col += 1;
        if self.col == self.stride {
            self.finish_row()?;
            self.col = 0;
            self.row += 1;
        }
        Ok(())
    }

    fn copy_byte(&self, dist: usize) -> Result<u8, ()> {
        if dist == 0 || dist > self.emitted {
            return Err(());
        }
        unsafe { Ok(WIN[(self.emitted - dist) % WIN_SIZE]) }
    }

    fn finish_row(&mut self) -> Result<(), ()> {
        let first = self.row == 0;
        unfilter_row(self.stride, self.bpp, first)?;
        let y = self.row;
        for dy in 0..self.dh {
            if dy * self.height / self.dh != y {
                continue;
            }
            let py = self.oy + dy;
            if py >= MODE13_H {
                continue;
            }
            for dx in 0..self.dw {
                let sx = dx * self.width / self.dw;
                let px = self.ox + dx;
                if px >= MODE13_W {
                    continue;
                }
                let idx = sample_scan(self.color_type, self.bpp, sx);
                unsafe {
                    SCREEN[py * MODE13_W + px] = idx;
                }
                self.drawn += 1;
            }
        }
        unsafe {
            PREV[..self.stride].copy_from_slice(&SCAN[..self.stride]);
        }
        Ok(())
    }
}

fn unfilter_row(stride: usize, bpp: usize, first: bool) -> Result<(), ()> {
    let row_bytes = stride - 1;
    let filter = unsafe { SCAN[0] };
    for i in 0..row_bytes {
        let x = unsafe { SCAN[1 + i] };
        let left = if i >= bpp {
            unsafe { SCAN[1 + i - bpp] }
        } else {
            0
        };
        let up = if first {
            0
        } else {
            unsafe { PREV[1 + i] }
        };
        let up_left = if first || i < bpp {
            0
        } else {
            unsafe { PREV[1 + i - bpp] }
        };
        let recon = match filter {
            0 => x,
            1 => x.wrapping_add(left),
            2 => x.wrapping_add(up),
            3 => x.wrapping_add(((left as u16 + up as u16) / 2) as u8),
            4 => x.wrapping_add(paeth(left, up, up_left)),
            _ => return Err(()),
        };
        unsafe {
            SCAN[1 + i] = recon;
        }
    }
    Ok(())
}

fn sample_scan(color_type: u8, bpp: usize, sx: usize) -> u8 {
    let p = 1 + sx * bpp;
    match color_type {
        0 => unsafe { rgb_to_index(SCAN[p], SCAN[p], SCAN[p]) },
        3 => unsafe { SCAN[p] },
        2 => unsafe { rgb_to_index(SCAN[p], SCAN[p + 1], SCAN[p + 2]) },
        4 => unsafe {
            let a = SCAN[p + 1] as u16;
            let g = (SCAN[p] as u16 * a / 255) as u8;
            rgb_to_index(g, g, g)
        },
        6 => unsafe {
            let a = SCAN[p + 3] as u16;
            let r = (SCAN[p] as u16 * a / 255) as u8;
            let g = (SCAN[p + 1] as u16 * a / 255) as u8;
            let b = (SCAN[p + 2] as u16 * a / 255) as u8;
            rgb_to_index(r, g, b)
        },
        _ => 0,
    }
}

fn find_png(data: &[u8]) -> Option<&[u8]> {
    if data.len() < 8 {
        return None;
    }
    let last = data.len() - 8;
    let mut i = 0usize;
    while i <= last {
        if data[i..i + 8] == PNG_SIG {
            return Some(&data[i..]);
        }
        i += 1;
    }
    None
}

fn print_hex_byte(b: u8) {
    const HEX: &[u8; 16] = b"0123456789ABCDEF";
    rust_print(&[HEX[(b >> 4) as usize], HEX[(b & 0x0F) as usize], b' ']);
}

fn explain_not_png(data: &[u8]) {
    rust_print(b"ERROR: not a PNG\n");
    if data.len() >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF {
        rust_print(b"(file is JPEG)\n");
        return;
    }
    if data.starts_with(b"GIF87a") || data.starts_with(b"GIF89a") {
        rust_print(b"(file is GIF)\n");
        return;
    }
    if data.len() >= 2 && data[0] == b'B' && data[1] == b'M' {
        rust_print(b"(file is BMP)\n");
        return;
    }
    if data.len() >= 4 && data.starts_with(b"RIFF") {
        rust_print(b"(file is RIFF/WebP/WAV)\n");
        return;
    }
    if data.first() == Some(&b'<') || data.starts_with(b"HTTP/") || data.starts_with(b"<!DOC") {
        rust_print(b"(file is HTML/HTTP, not image bytes)\n");
        return;
    }
    rust_print(b"first bytes: ");
    let n = data.len().min(16);
    for &b in &data[..n] {
        print_hex_byte(b);
    }
    rust_print(b"\n");
}

fn inflate_block_stored(br: &mut BitReader, sink: &mut PngSink) -> Result<(), ()> {
    let len = br.read_u16_le()? as usize;
    let nlen = br.read_u16_le()?;
    if (len as u16) ^ 0xFFFF != nlen {
        return Err(());
    }
    for _ in 0..len {
        let b = br.next_byte()?;
        sink.emit(b)?;
    }
    Ok(())
}

fn inflate_block_huffman(
    br: &mut BitReader,
    sink: &mut PngSink,
    lit: &Huffman,
    dist: &Huffman,
) -> Result<(), ()> {
    loop {
        let sym = huffman_decode(br, lit)?;
        if sym < 256 {
            sink.emit(sym as u8)?;
        } else if sym == 256 {
            return Ok(());
        } else if sym <= 285 {
            let li = (sym - 257) as usize;
            if li >= LEN_BASE.len() {
                return Err(());
            }
            let length = LEN_BASE[li] as usize + br.get_bits(LEN_EXTRA[li] as u32)? as usize;
            let ds = huffman_decode(br, dist)?;
            if ds as usize >= DIST_BASE.len() {
                return Err(());
            }
            let distance = DIST_BASE[ds as usize] as usize
                + br.get_bits(DIST_EXTRA[ds as usize] as u32)? as usize;
            for _ in 0..length {
                let b = sink.copy_byte(distance)?;
                sink.emit(b)?;
            }
        } else {
            return Err(());
        }
    }
}

fn build_fixed_trees(lit: &mut Huffman, dist: &mut Huffman) -> Result<(), ()> {
    let mut lit_len = [0u8; 288];
    for i in 0..144 {
        lit_len[i] = 8;
    }
    for i in 144..256 {
        lit_len[i] = 9;
    }
    for i in 256..280 {
        lit_len[i] = 7;
    }
    for i in 280..288 {
        lit_len[i] = 8;
    }
    construct(lit, &lit_len)?;
    let dist_len = [5u8; 32];
    construct(dist, &dist_len)
}

fn build_dynamic_trees(br: &mut BitReader, lit: &mut Huffman, dist: &mut Huffman) -> Result<(), ()> {
    let hlit = br.get_bits(5)? as usize + 257;
    let hdist = br.get_bits(5)? as usize + 1;
    let hclen = br.get_bits(4)? as usize + 4;
    if hlit > 286 || hdist > 32 || hclen > 19 {
        return Err(());
    }

    let mut clen = [0u8; 19];
    for i in 0..hclen {
        clen[CLEN_ORDER[i] as usize] = br.get_bits(3)? as u8;
    }
    let mut clen_tree = Huffman::empty();
    construct(&mut clen_tree, &clen)?;

    let total = hlit + hdist;
    let mut lengths = [0u8; 318];
    let mut n = 0usize;
    while n < total {
        let sym = huffman_decode(br, &clen_tree)?;
        if sym < 16 {
            lengths[n] = sym as u8;
            n += 1;
        } else {
            let (val, reps) = if sym == 16 {
                if n == 0 {
                    return Err(());
                }
                (lengths[n - 1], br.get_bits(2)? as usize + 3)
            } else if sym == 17 {
                (0u8, br.get_bits(3)? as usize + 3)
            } else if sym == 18 {
                (0u8, br.get_bits(7)? as usize + 11)
            } else {
                return Err(());
            };
            if n + reps > total {
                return Err(());
            }
            for _ in 0..reps {
                lengths[n] = val;
                n += 1;
            }
        }
    }
    construct(lit, &lengths[..hlit])?;
    construct(dist, &lengths[hlit..hlit + hdist])
}

fn inflate_deflate(br: &mut BitReader, sink: &mut PngSink) -> Result<(), ()> {
    loop {
        let bfinal = br.get_bits(1)?;
        let btype = br.get_bits(2)?;
        match btype {
            0 => inflate_block_stored(br, sink)?,
            1 => {
                let mut lit = Huffman::empty();
                let mut dist = Huffman::empty();
                build_fixed_trees(&mut lit, &mut dist)?;
                inflate_block_huffman(br, sink, &lit, &dist)?;
            }
            2 => {
                let mut lit = Huffman::empty();
                let mut dist = Huffman::empty();
                build_dynamic_trees(br, &mut lit, &mut dist)?;
                inflate_block_huffman(br, sink, &lit, &dist)?;
            }
            _ => return Err(()),
        }
        if bfinal == 1 {
            break;
        }
    }
    Ok(())
}

fn be32(p: &[u8]) -> u32 {
    ((p[0] as u32) << 24) | ((p[1] as u32) << 16) | ((p[2] as u32) << 8) | (p[3] as u32)
}

fn paeth(a: u8, b: u8, c: u8) -> u8 {
    let aa = a as i16;
    let bb = b as i16;
    let cc = c as i16;
    let p = aa + bb - cc;
    let pa = (p - aa).abs();
    let pb = (p - bb).abs();
    let pc = (p - cc).abs();
    if pa <= pb && pa <= pc {
        a
    } else if pb <= pc {
        b
    } else {
        c
    }
}

fn rgb_to_index(r: u8, g: u8, b: u8) -> u8 {
    if r == g && g == b {
        return 216 + ((r as u16 * 39) / 255) as u8;
    }
    let r6 = (r as u16 * 5 / 255) as u8;
    let g6 = (g as u16 * 5 / 255) as u8;
    let b6 = (b as u16 * 5 / 255) as u8;
    r6 * 36 + g6 * 6 + b6
}

fn fill_cube_palette(pal: &mut [u8]) {
    let mut i = 0usize;
    for r in 0..6u8 {
        for g in 0..6u8 {
            for b in 0..6u8 {
                pal[i] = r * 51;
                pal[i + 1] = g * 51;
                pal[i + 2] = b * 51;
                i += 3;
            }
        }
    }
    for g in 0..40u8 {
        let v = (g as u16 * 255 / 39) as u8;
        pal[i] = v;
        pal[i + 1] = v;
        pal[i + 2] = v;
        i += 3;
    }
}

fn dest_size(src_w: usize, src_h: usize, ox: usize, oy: usize) -> (usize, usize) {
    let max_w = MODE13_W.saturating_sub(ox);
    let max_h = MODE13_H.saturating_sub(oy);
    if src_w <= max_w && src_h <= max_h {
        (src_w, src_h)
    } else if src_w * max_h > src_h * max_w {
        (max_w, (src_h * max_w / src_w).max(1))
    } else {
        ((src_w * max_h / src_h).max(1), max_h)
    }
}

fn decode_to_screen(data: &[u8], x: i32, y: i32) -> Result<(i32, i32), ()> {
    let png = match find_png(data) {
        Some(p) => p,
        None => {
            explain_not_png(data);
            return Err(());
        }
    };

    let mut pos = 8usize;
    let mut width = 0u32;
    let mut height = 0u32;
    let mut bit_depth = 0u8;
    let mut color_type = 0u8;
    let mut interlace = 0u8;
    let mut got_ihdr = false;
    let mut pal_count = 0i32;
    let mut idat_spans: [(usize, usize); 32] = [(0, 0); 32];
    let mut idat_n = 0usize;
    let mut idat_total = 0usize;

    while pos + 12 <= png.len() {
        let len = be32(&png[pos..]) as usize;
        let typ = &png[pos + 4..pos + 8];
        let data_off = pos + 8;
        if data_off + len + 4 > png.len() {
            rust_print(b"ERROR: truncated chunk\n");
            return Err(());
        }
        let data = &png[data_off..data_off + len];

        if typ == b"IHDR" {
            if len != 13 {
                rust_print(b"ERROR: bad IHDR\n");
                return Err(());
            }
            width = be32(&data[0..]);
            height = be32(&data[4..]);
            bit_depth = data[8];
            color_type = data[9];
            if data[10] != 0 || data[11] != 0 {
                rust_print(b"ERROR: unsupported compression/filter\n");
                return Err(());
            }
            interlace = data[12];
            got_ihdr = true;
        } else if typ == b"PLTE" {
            if len == 0 || len % 3 != 0 || len > 768 {
                rust_print(b"ERROR: bad PLTE\n");
                return Err(());
            }
            pal_count = (len / 3) as i32;
            unsafe {
                PALETTE[..len].copy_from_slice(data);
            }
        } else if typ == b"IDAT" {
            if idat_n >= idat_spans.len() {
                rust_print(b"ERROR: too many IDAT chunks\n");
                return Err(());
            }
            idat_spans[idat_n] = (data_off, len);
            idat_n += 1;
            idat_total += len;
        } else if typ == b"IEND" {
            break;
        }

        pos = data_off + len + 4;
    }

    if !got_ihdr {
        rust_print(b"ERROR: missing IHDR\n");
        return Err(());
    }
    if interlace != 0 {
        rust_print(b"ERROR: interlaced PNG not supported\n");
        return Err(());
    }
    if bit_depth != 8 {
        rust_print(b"ERROR: only 8-bit PNG supported\n");
        return Err(());
    }
    if width == 0 || height == 0 || width > PNG_MAX_DIM || height > PNG_MAX_DIM {
        rust_print(b"ERROR: image dimensions not supported\n");
        return Err(());
    }
    let bpp = match color_type {
        0 => 1usize,
        2 => 3usize,
        3 => {
            if pal_count == 0 {
                rust_print(b"ERROR: indexed PNG missing PLTE\n");
                return Err(());
            }
            1
        }
        4 => 2usize,
        6 => 4usize,
        _ => {
            rust_print(b"ERROR: unsupported color type\n");
            return Err(());
        }
    };
    let stride = (width as usize)
        .checked_mul(bpp)
        .and_then(|v| v.checked_add(1))
        .ok_or(())?;
    if stride > MAX_STRIDE {
        rust_print(b"ERROR: scanline too wide\n");
        return Err(());
    }
    if idat_n == 0 || idat_total < 2 {
        rust_print(b"ERROR: missing IDAT\n");
        return Err(());
    }

    let ox = if x < 0 { 0usize } else { x as usize };
    let oy = if y < 0 { 0usize } else { y as usize };
    if ox >= MODE13_W || oy >= MODE13_H {
        return Ok((0, if color_type == 3 { pal_count } else { 256 }));
    }
    let (dw, dh) = dest_size(width as usize, height as usize, ox, oy);

    unsafe {
        SCREEN.fill(0);
        WIN.fill(0);
        SCAN.fill(0);
        PREV.fill(0);
        if color_type != 3 {
            fill_cube_palette(&mut PALETTE);
        }
    }

    let mut br = BitReader {
        png,
        spans: idat_spans,
        span_n: idat_n,
        idat_len: idat_total,
        pos: 0,
        bitbuf: 0,
        bitcnt: 0,
    };
    let cmf = br.next_byte()?;
    let flg = br.next_byte()?;
    if cmf & 0x0F != 8 {
        rust_print(b"ERROR: inflate failed\n");
        return Err(());
    }
    if (((cmf as u16) << 8) | flg as u16) % 31 != 0 || flg & 0x20 != 0 {
        rust_print(b"ERROR: inflate failed\n");
        return Err(());
    }

    let mut sink = PngSink {
        width: width as usize,
        height: height as usize,
        bpp,
        color_type,
        stride,
        ox,
        oy,
        dw,
        dh,
        row: 0,
        col: 0,
        emitted: 0,
        drawn: 0,
    };
    if inflate_deflate(&mut br, &mut sink).is_err() {
        rust_print(b"ERROR: inflate failed\n");
        return Err(());
    }
    if sink.row != sink.height {
        rust_print(b"ERROR: inflated size mismatch\n");
        return Err(());
    }

    let pal_count_out = if color_type == 3 { pal_count } else { 256 };
    Ok((sink.drawn, pal_count_out))
}

unsafe fn present(drawn: i32, pal_count: i32) {
    vga_set_mode13h();
    vga_dac_set_palette(PALETTE.as_ptr(), pal_count);
    let vram = 0xA0000 as *mut u8;
    for i in 0..MODE13_PIXELS {
        core::ptr::write_volatile(vram.add(i), SCREEN[i]);
    }
    keyboard_wait_for_key(0);
    vga_restore_text_80x50();
    rust_print(b"PNG displayed (");
    print_i32(drawn);
    rust_print(b" pixels). Key pressed.\n");
}

fn print_i32(mut num: i32) {
    if num == 0 {
        rust_print(b"0");
        return;
    }
    if num < 0 {
        rust_print(b"-");
        num = -num;
    }
    let mut buf = [0u8; 16];
    let mut i = 0usize;
    while num > 0 {
        buf[i] = b'0' + (num % 10) as u8;
        num /= 10;
        i += 1;
    }
    while i > 0 {
        i -= 1;
        rust_print(&buf[i..i + 1]);
    }
}

fn crc32(data: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            crc = if crc & 1 != 0 {
                (crc >> 1) ^ 0xEDB8_8320
            } else {
                crc >> 1
            };
        }
    }
    !crc
}

fn adler32(data: &[u8]) -> u32 {
    let mut s1 = 1u32;
    let mut s2 = 0u32;
    for &b in data {
        s1 = (s1 + b as u32) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    (s2 << 16) | s1
}

fn put_be32(buf: &mut [u8], off: usize, v: u32) {
    buf[off] = (v >> 24) as u8;
    buf[off + 1] = (v >> 16) as u8;
    buf[off + 2] = (v >> 8) as u8;
    buf[off + 3] = v as u8;
}

fn write_chunk(buf: &mut [u8], idx: &mut usize, typ: &[u8; 4], data: &[u8]) -> Result<(), ()> {
    let need = 12 + data.len();
    if *idx + need > buf.len() {
        return Err(());
    }
    put_be32(buf, *idx, data.len() as u32);
    buf[*idx + 4..*idx + 8].copy_from_slice(typ);
    buf[*idx + 8..*idx + 8 + data.len()].copy_from_slice(data);
    let mut crc_in = [0u8; 4];
    crc_in.copy_from_slice(typ);
    let crc = if data.is_empty() {
        crc32(&crc_in)
    } else {
        let mut c = 0xFFFF_FFFFu32;
        for &b in typ.iter().chain(data.iter()) {
            c ^= b as u32;
            for _ in 0..8 {
                c = if c & 1 != 0 {
                    (c >> 1) ^ 0xEDB8_8320
                } else {
                    c >> 1
                };
            }
        }
        !c
    };
    put_be32(buf, *idx + 8 + data.len(), crc);
    *idx += need;
    Ok(())
}

fn build_test_png(buf: &mut [u8]) -> Result<usize, ()> {
    const W: usize = 80;
    const H: usize = 50;
    const BPP: usize = 3;
    let row = 1 + W * BPP;
    let raw_len = row * H;
    let mut raw = [0u8; (1 + 80 * 3) * 50];
    if raw_len > raw.len() {
        return Err(());
    }
    for y in 0..H {
        let off = y * row;
        raw[off] = 0;
        for x in 0..W {
            let p = off + 1 + x * 3;
            raw[p] = (x * 255 / (W - 1)) as u8;
            raw[p + 1] = (y * 255 / (H - 1)) as u8;
            raw[p + 2] = if x > 20 && x < 60 && y > 15 && y < 35 {
                220
            } else {
                40
            };
        }
    }

    let mut zlib = [0u8; 16 + (1 + 80 * 3) * 50];
    let zlen = 2 + 5 + raw_len + 4;
    if zlen > zlib.len() {
        return Err(());
    }
    zlib[0] = 0x78;
    zlib[1] = 0x01;
    zlib[2] = 0x01;
    zlib[3] = (raw_len & 0xFF) as u8;
    zlib[4] = ((raw_len >> 8) & 0xFF) as u8;
    let nlen = (raw_len as u16) ^ 0xFFFF;
    zlib[5] = (nlen & 0xFF) as u8;
    zlib[6] = ((nlen >> 8) & 0xFF) as u8;
    zlib[7..7 + raw_len].copy_from_slice(&raw[..raw_len]);
    put_be32(&mut zlib, 7 + raw_len, adler32(&raw[..raw_len]));

    let sig: [u8; 8] = [137, 80, 78, 71, 13, 10, 26, 10];
    if buf.len() < 8 {
        return Err(());
    }
    buf[..8].copy_from_slice(&sig);
    let mut idx = 8usize;

    let mut ihdr = [0u8; 13];
    put_be32(&mut ihdr, 0, W as u32);
    put_be32(&mut ihdr, 4, H as u32);
    ihdr[8] = 8;
    ihdr[9] = 2;
    write_chunk(buf, &mut idx, b"IHDR", &ihdr)?;
    write_chunk(buf, &mut idx, b"IDAT", &zlib[..zlen])?;
    write_chunk(buf, &mut idx, b"IEND", &[])?;
    Ok(idx)
}

fn avfs_write_all(name: *const u8, data: *const u8, size: u32) -> bool {
    unsafe {
        let mut written = 0u32;
        let chunk = 256u32;
        while written < size {
            let remaining = size - written;
            let to_write = if remaining < chunk { remaining } else { chunk };
            let res = avfs_write_file(name, data.add(written as usize), to_write, written);
            if res < 0 {
                return false;
            }
            written += to_write;
        }
        true
    }
}

#[no_mangle]
pub extern "C" fn rust_draw_png_from_memory(png_data: *const u8, data_len: u32, x: i32, y: i32) -> i32 {
    if png_data.is_null() || data_len == 0 {
        rust_print(b"ERROR: Null PNG pointer\n");
        return -1;
    }
    if data_len > PNG_MAX_FILE {
        rust_print(b"ERROR: PNG too large (max 768KB)\n");
        return -1;
    }

    unsafe {
        let png = core::slice::from_raw_parts(png_data, data_len as usize);
        match decode_to_screen(png, x, y) {
            Ok((drawn, pal_count)) => {
                rust_print(b"[PNG] decode ok (downscaled to Mode 13h)\n");
                present(drawn, pal_count);
                drawn
            }
            Err(()) => -1,
        }
    }
}

#[no_mangle]
pub extern "C" fn rust_load_and_render_png(filename: *const u8, x: i32, y: i32) -> i32 {
    if filename.is_null() {
        return -1;
    }
    unsafe {
        let filesize = avfs_get_filesize(filename);
        if filesize <= 0 {
            rust_print(b"ERROR: File not found\n");
            return -1;
        }
        if filesize as u32 > PNG_MAX_FILE {
            rust_print(b"ERROR: PNG too large (max 768KB)\n");
            return -1;
        }
        // avfs_read_file returns 0 on success, not a byte count.
        if avfs_read_file(filename, FILE_BUF.as_mut_ptr(), filesize as u32, 0) != 0 {
            rust_print(b"ERROR: Could not read full file\n");
            return -1;
        }
        rust_draw_png_from_memory(FILE_BUF.as_ptr(), filesize as u32, x, y)
    }
}

#[no_mangle]
pub extern "C" fn rust_render_test_png() -> i32 {
    unsafe {
        rust_print(b"=== GENERATING TEST PNG ===\n");
        let idx = match build_test_png(&mut TEST_PNG_BUFFER) {
            Ok(n) => n,
            Err(()) => {
                rust_print(b"ERROR: test PNG encode failed\n");
                return -1;
            }
        };
        let required_size = idx as u32;
        let filename = b"test.png\0";
        let current_size = avfs_get_filesize(filename.as_ptr());
        let needs_creation = current_size == -1 || current_size as u32 != required_size;
        if needs_creation {
            if current_size != -1 {
                avfs_remove_file(filename.as_ptr());
            }
            rust_print(b"CREATING FILE...\n");
            let create_res = avfs_create_file(filename.as_ptr(), required_size);
            if create_res == -2 {
                rust_print(b"ERROR: Disk Full (OOM)\n");
                return -1;
            }
        }
        rust_print(b"WRITING TO DISK...\n");
        let _ = avfs_write_all(filename.as_ptr(), TEST_PNG_BUFFER.as_ptr(), required_size);
        rust_draw_png_from_memory(TEST_PNG_BUFFER.as_ptr(), required_size, 40, 20)
    }
}
