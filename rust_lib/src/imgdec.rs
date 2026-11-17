//! A small PNG decoder for the Discord client: PNG bytes in, ARGB pixels out, shrunk to fit a box.
//! Handles every colour type and bit depth of non-interlaced PNGs (what Discord's image proxy produces).

use alloc::vec;
use alloc::vec::Vec;

// ---- inflate ---------------------------------------------------------------------------------------------------

struct Bits<'a> {
    data: &'a [u8],
    pos: usize,
    hold: u32,
    count: u32,
}

impl<'a> Bits<'a> {
    fn bit(&mut self) -> Result<u32, ()> {
        if self.count == 0 {
            if self.pos >= self.data.len() {
                return Err(());
            }
            self.hold = self.data[self.pos] as u32;
            self.pos += 1;
            self.count = 8;
        }
        let b = self.hold & 1;
        self.hold >>= 1;
        self.count -= 1;
        Ok(b)
    }

    fn bits(&mut self, n: u32) -> Result<u32, ()> {
        let mut v = 0;
        for i in 0..n {
            v |= self.bit()? << i;
        }
        Ok(v)
    }

    fn align(&mut self) {
        self.count = 0;
    }
}

struct Tree {
    counts: [u16; 16],
    symbols: [u16; 288],
}

impl Tree {
    fn new(lengths: &[u8]) -> Tree {
        let mut t = Tree { counts: [0; 16], symbols: [0; 288] };
        for &l in lengths {
            t.counts[l as usize] += 1;
        }
        t.counts[0] = 0;
        let mut offs = [0u16; 16];
        for i in 1..16 {
            offs[i] = offs[i - 1] + t.counts[i - 1];
        }
        for (sym, &l) in lengths.iter().enumerate() {
            if l != 0 {
                t.symbols[offs[l as usize] as usize] = sym as u16;
                offs[l as usize] += 1;
            }
        }
        t
    }

    fn decode(&self, br: &mut Bits) -> Result<u16, ()> {
        let (mut sum, mut cur, mut len) = (0i32, 0i32, 0usize);
        loop {
            cur = 2 * cur + br.bit()? as i32;
            len += 1;
            if len > 15 {
                return Err(());
            }
            sum += self.counts[len] as i32;
            cur -= self.counts[len] as i32;
            if cur < 0 {
                break;
            }
        }
        let at = (sum + cur) as usize;
        if at >= 288 {
            return Err(());
        }
        Ok(self.symbols[at])
    }
}

const LEN_BASE: [u16; 29] = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
const LEN_EXTRA: [u8; 29] = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
const DIST_BASE: [u16; 30] = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577];
const DIST_EXTRA: [u8; 30] = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];
const CL_ORDER: [usize; 19] = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];

fn inflate_block(br: &mut Bits, lit: &Tree, dist: &Tree, out: &mut Vec<u8>, limit: usize) -> Result<(), ()> {
    loop {
        let sym = lit.decode(br)? as usize;
        if sym < 256 {
            if out.len() >= limit {
                return Err(());
            }
            out.push(sym as u8);
        } else if sym == 256 {
            return Ok(());
        } else {
            let s = sym - 257;
            if s >= 29 {
                return Err(());
            }
            let len = LEN_BASE[s] as usize + br.bits(LEN_EXTRA[s] as u32)? as usize;
            let d = dist.decode(br)? as usize;
            if d >= 30 {
                return Err(());
            }
            let distance = DIST_BASE[d] as usize + br.bits(DIST_EXTRA[d] as u32)? as usize;
            if distance > out.len() || out.len() + len > limit {
                return Err(());
            }
            for _ in 0..len {
                let b = out[out.len() - distance];
                out.push(b);
            }
        }
    }
}

fn inflate(data: &[u8], limit: usize) -> Result<Vec<u8>, ()> {
    let mut br = Bits { data, pos: 0, hold: 0, count: 0 };
    let mut out = Vec::with_capacity(limit);
    loop {
        let last = br.bit()?;
        match br.bits(2)? {
            0 => {
                br.align();
                if br.pos + 4 > data.len() {
                    return Err(());
                }
                let len = data[br.pos] as usize | (data[br.pos + 1] as usize) << 8;
                br.pos += 4;
                if br.pos + len > data.len() || out.len() + len > limit {
                    return Err(());
                }
                out.extend_from_slice(&data[br.pos..br.pos + len]);
                br.pos += len;
            }
            1 => {
                let mut l = [0u8; 288];
                for i in 0..144 { l[i] = 8; }
                for i in 144..256 { l[i] = 9; }
                for i in 256..280 { l[i] = 7; }
                for i in 280..288 { l[i] = 8; }
                let lit = Tree::new(&l);
                let dist = Tree::new(&[5u8; 30]);
                inflate_block(&mut br, &lit, &dist, &mut out, limit)?;
            }
            2 => {
                let hlit = br.bits(5)? as usize + 257;
                let hdist = br.bits(5)? as usize + 1;
                let hclen = br.bits(4)? as usize + 4;
                let mut cl = [0u8; 19];
                for i in 0..hclen {
                    cl[CL_ORDER[i]] = br.bits(3)? as u8;
                }
                let cl_tree = Tree::new(&cl);
                let mut lengths = [0u8; 320];
                let mut n = 0;
                while n < hlit + hdist {
                    let sym = cl_tree.decode(&mut br)?;
                    let (value, repeat) = match sym {
                        0..=15 => (sym as u8, 1),
                        16 => {
                            if n == 0 { return Err(()); }
                            (lengths[n - 1], 3 + br.bits(2)? as usize)
                        }
                        17 => (0, 3 + br.bits(3)? as usize),
                        _ => (0, 11 + br.bits(7)? as usize),
                    };
                    if n + repeat > hlit + hdist {
                        return Err(());
                    }
                    for _ in 0..repeat {
                        lengths[n] = value;
                        n += 1;
                    }
                }
                let lit = Tree::new(&lengths[..hlit]);
                let dist = Tree::new(&lengths[hlit..hlit + hdist]);
                inflate_block(&mut br, &lit, &dist, &mut out, limit)?;
            }
            _ => return Err(()),
        }
        if last == 1 {
            return Ok(out);
        }
    }
}

// ---- PNG --------------------------------------------------------------------------------------------------------

fn be32(p: &[u8]) -> u32 {
    (p[0] as u32) << 24 | (p[1] as u32) << 16 | (p[2] as u32) << 8 | p[3] as u32
}

fn paeth(a: u8, b: u8, c: u8) -> u8 {
    let (ia, ib, ic) = (a as i32, b as i32, c as i32);
    let p = ia + ib - ic;
    let (pa, pb, pc) = ((p - ia).abs(), (p - ib).abs(), (p - ic).abs());
    if pa <= pb && pa <= pc { a } else if pb <= pc { b } else { c }
}

const MAX_SOURCE_PIXELS: u64 = 8_000_000;

fn decode_png(data: &[u8]) -> Result<(usize, usize, Vec<u32>), &'static str> {
    if data.len() < 33 || data[..8] != [0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A] {
        return Err("not a PNG");
    }
    let (mut w, mut h, mut depth, mut ctype, mut interlace) = (0usize, 0usize, 0u8, 0u8, 0u8);
    let mut palette: Vec<u8> = Vec::new();
    let mut trns: Vec<u8> = Vec::new();
    let mut zdata: Vec<u8> = Vec::new();
    let mut got_header = false;

    let mut pos = 8;
    while pos + 12 <= data.len() {
        let len = be32(&data[pos..]) as usize;
        let kind = &data[pos + 4..pos + 8];
        if pos + 12 + len > data.len() {
            return Err("truncated PNG");
        }
        let body = &data[pos + 8..pos + 8 + len];
        match kind {
            b"IHDR" if len >= 13 => {
                w = be32(body) as usize;
                h = be32(&body[4..]) as usize;
                depth = body[8];
                ctype = body[9];
                interlace = body[12];
                got_header = true;
            }
            b"PLTE" => palette = body.to_vec(),
            b"tRNS" => trns = body.to_vec(),
            b"IDAT" => zdata.extend_from_slice(body),
            b"IEND" => break,
            _ => {}
        }
        pos += 12 + len;
    }
    if !got_header || w == 0 || h == 0 || (w as u64) * (h as u64) > MAX_SOURCE_PIXELS {
        return Err("unsupported PNG size");
    }
    if interlace != 0 {
        return Err("interlaced PNG");
    }
    let channels = match ctype { 0 => 1, 2 => 3, 3 => 1, 4 => 2, 6 => 4, _ => return Err("bad colour type") };
    if !matches!(depth, 1 | 2 | 4 | 8 | 16) || (ctype != 0 && ctype != 3 && depth < 8) || (ctype == 3 && depth == 16) {
        return Err("bad bit depth");
    }
    let bits = channels * depth as usize;
    let bpp = core::cmp::max(1, bits / 8);
    let stride = (w * bits + 7) / 8;
    if zdata.len() < 3 || zdata[0] & 0x0F != 8 {
        return Err("bad zlib stream");
    }
    let mut raw = inflate(&zdata[2..], h * (stride + 1)).map_err(|_| "corrupt PNG data")?;
    drop(zdata);
    if raw.len() < h * (stride + 1) {
        return Err("short PNG data");
    }

    // Undo the row filters in place.
    for y in 0..h {
        let row = y * (stride + 1);
        let filter = raw[row];
        for x in 0..stride {
            let i = row + 1 + x;
            let a = if x >= bpp { raw[i - bpp] } else { 0 };
            let b = if y > 0 { raw[i - (stride + 1)] } else { 0 };
            let c = if x >= bpp && y > 0 { raw[i - (stride + 1) - bpp] } else { 0 };
            let add = match filter {
                0 => 0,
                1 => a,
                2 => b,
                3 => ((a as u16 + b as u16) / 2) as u8,
                4 => paeth(a, b, c),
                _ => return Err("bad PNG filter"),
            };
            raw[i] = raw[i].wrapping_add(add);
        }
    }

    let mut px = vec![0u32; w * h];
    let max = (1u32 << depth.min(8)) - 1;
    for y in 0..h {
        let row = &raw[y * (stride + 1) + 1..(y + 1) * (stride + 1)];
        for x in 0..w {
            let sample = |i: usize| -> u32 {
                match depth {
                    8 => row[x * channels + i] as u32,
                    16 => row[(x * channels + i) * 2] as u32,
                    _ => {
                        let per = 8 / depth as usize;
                        let byte = row[x / per];
                        let shift = 8 - depth as usize * (x % per + 1);
                        ((byte >> shift) as u32) & max
                    }
                }
            };
            let scale = |v: u32| -> u32 { if depth < 8 { v * 255 / max } else { v } };
            let argb = match ctype {
                0 => { let g = scale(sample(0)); 0xFF00_0000 | g << 16 | g << 8 | g }
                2 => 0xFF00_0000 | sample(0) << 16 | sample(1) << 8 | sample(2),
                3 => {
                    let i = sample(0) as usize;
                    if i * 3 + 2 >= palette.len() { 0xFF00_0000 } else {
                        let a = trns.get(i).copied().unwrap_or(255) as u32;
                        a << 24 | (palette[i * 3] as u32) << 16 | (palette[i * 3 + 1] as u32) << 8 | palette[i * 3 + 2] as u32
                    }
                }
                4 => { let g = sample(0); sample(1) << 24 | g << 16 | g << 8 | g }
                _ => sample(3) << 24 | sample(0) << 16 | sample(1) << 8 | sample(2),
            };
            px[y * w + x] = argb;
        }
    }
    Ok((w, h, px))
}

/// Box-filter shrink to nw x nh, weighting colour by alpha.
fn shrink(src: &[u32], w: usize, h: usize, nw: usize, nh: usize) -> Vec<u32> {
    let mut out = vec![0u32; nw * nh];
    for dy in 0..nh {
        let y0 = dy * h / nh;
        let y1 = core::cmp::max(y0 + 1, (dy + 1) * h / nh);
        for dx in 0..nw {
            let x0 = dx * w / nw;
            let x1 = core::cmp::max(x0 + 1, (dx + 1) * w / nw);
            let (mut a, mut r, mut g, mut b, mut n) = (0u32, 0u32, 0u32, 0u32, 0u32);
            for sy in y0..y1 {
                for sx in x0..x1 {
                    let p = src[sy * w + sx];
                    let pa = p >> 24;
                    a += pa;
                    r += ((p >> 16) & 255) * pa;
                    g += ((p >> 8) & 255) * pa;
                    b += (p & 255) * pa;
                    n += 1;
                }
            }
            out[dy * nw + dx] = if a == 0 {
                0
            } else {
                (a / n) << 24 | (r / a) << 16 | (g / a) << 8 | (b / a)
            };
        }
    }
    out
}

/// Decodes a PNG into `out` as 0xAARRGGBB, shrunk (never enlarged) to fit max_w x max_h. Returns 0 and the size in
/// *w_out / *h_out; -1 if the data is not a PNG this decoder handles, -2 if the result does not fit `out_cap` pixels.
#[no_mangle]
pub unsafe extern "C" fn rust_png_decode_fit(
    data: *const u8,
    len: u32,
    max_w: u32,
    max_h: u32,
    out: *mut u32,
    out_cap: u32,
    w_out: *mut u32,
    h_out: *mut u32,
) -> i32 {
    if data.is_null() || out.is_null() || w_out.is_null() || h_out.is_null() || max_w == 0 || max_h == 0 {
        return -1;
    }
    let bytes = core::slice::from_raw_parts(data, len as usize);
    let Ok((w, h, px)) = decode_png(bytes) else { return -1 };

    let (mw, mh) = (max_w as usize, max_h as usize);
    let (nw, nh) = if w <= mw && h <= mh {
        (w, h)
    } else if w * mh > h * mw {
        (mw, core::cmp::max(1, h * mw / w))
    } else {
        (core::cmp::max(1, w * mh / h), mh)
    };
    if nw * nh > out_cap as usize {
        return -2;
    }
    let result = if (nw, nh) == (w, h) { px } else { shrink(&px, w, h, nw, nh) };
    core::ptr::copy_nonoverlapping(result.as_ptr(), out, nw * nh);
    *w_out = nw as u32;
    *h_out = nh as u32;
    0
}
