//! Picture decoding for the desktop's web browser: PNG (through imgdec), JPEG (baseline and progressive Huffman),
//! GIF (first frame) and BMP. Pixels come out as 0xAARRGGBB, shrunk (never enlarged) to fit a box.
//!
//! Integer arithmetic only: the kernel does not save FPU state across task switches.

use alloc::vec::Vec;

pub const KIND_PNG: i32 = 1;
pub const KIND_JPEG: i32 = 2;
pub const KIND_GIF: i32 = 3;
pub const KIND_BMP: i32 = 4;

/// Largest picture decoded (width x height). JPEG keeps every coefficient while it decodes, so this bounds the
/// heap a single picture can take to roughly 60 MB.
const MAX_PIXELS: usize = 6_000_000;
const MAX_SIDE: usize = 16384;

type Picture = (usize, usize, Vec<u32>);
type Result<T> = core::result::Result<T, &'static str>;

/// Which kind of picture the bytes hold, by their signature: KIND_* or 0.
pub fn kind(d: &[u8]) -> i32 {
    if d.starts_with(b"\x89PNG\r\n\x1a\n") {
        KIND_PNG
    } else if d.len() >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF {
        KIND_JPEG
    } else if d.starts_with(b"GIF87a") || d.starts_with(b"GIF89a") {
        KIND_GIF
    } else if d.len() >= 26 && d[0] == b'B' && d[1] == b'M' {
        KIND_BMP
    } else {
        0
    }
}

/// Decodes any supported picture at its full size.
pub fn decode(d: &[u8]) -> Result<(i32, Picture)> {
    let k = kind(d);
    let pic = match k {
        KIND_PNG => crate::imgdec::decode_png(d)?,
        KIND_JPEG => jpeg::decode(d)?,
        KIND_GIF => gif::decode(d)?,
        KIND_BMP => bmp::decode(d)?,
        _ => return Err("not a PNG, JPEG, GIF or BMP picture"),
    };
    Ok((k, pic))
}

fn check_size(w: usize, h: usize) -> Result<()> {
    if w == 0 || h == 0 {
        return Err("picture has no pixels");
    }
    if w > MAX_SIDE || h > MAX_SIDE || w * h > MAX_PIXELS {
        return Err("picture is too large");
    }
    Ok(())
}

/// The size `w` x `h` shrinks to so that it fits `max_w` x `max_h`, keeping its shape.
pub fn fit(w: usize, h: usize, max_w: usize, max_h: usize) -> (usize, usize) {
    if w <= max_w && h <= max_h {
        (w, h)
    } else if w * max_h > h * max_w {
        (max_w, core::cmp::max(1, h * max_w / w))
    } else {
        (core::cmp::max(1, w * max_h / h), max_h)
    }
}

fn u16le(d: &[u8], at: usize) -> Result<usize> {
    d.get(at..at + 2).map(|b| u16::from_le_bytes([b[0], b[1]]) as usize).ok_or("truncated picture")
}

fn u32le(d: &[u8], at: usize) -> Result<u32> {
    d.get(at..at + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]])).ok_or("truncated picture")
}

/// What kind of picture `data` holds: 1 PNG, 2 JPEG, 3 GIF, 4 BMP, 0 none of them.
#[no_mangle]
pub unsafe extern "C" fn rust_image_kind(data: *const u8, len: u32) -> i32 {
    if data.is_null() {
        return 0;
    }
    kind(core::slice::from_raw_parts(data, len as usize))
}

/// Decodes a PNG, JPEG, GIF (first frame) or BMP into `out` as 0xAARRGGBB, shrunk (never enlarged) to fit
/// max_w x max_h. Returns the picture's kind (as rust_image_kind) and its size in *w_out / *h_out; -1 if the data
/// is not a picture this decoder handles, -2 if the result does not fit `out_cap` pixels.
#[no_mangle]
pub unsafe extern "C" fn rust_image_decode_fit(
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
    let Ok((k, (w, h, px))) = decode(bytes) else { return -1 };
    let (nw, nh) = fit(w, h, max_w as usize, max_h as usize);
    if nw * nh > out_cap as usize {
        return -2;
    }
    let result = if (nw, nh) == (w, h) { px } else { crate::imgdec::shrink(&px, w, h, nw, nh) };
    core::ptr::copy_nonoverlapping(result.as_ptr(), out, nw * nh);
    *w_out = nw as u32;
    *h_out = nh as u32;
    k
}

// ---- BMP ---------------------------------------------------------------------------------------------------------

mod bmp {
    use super::{check_size, u16le, u32le, Picture, Result};
    use alloc::vec;

    /// Uncompressed BMPs: 1/4/8-bit palettes, 16/24/32-bit, bottom-up or top-down, BI_BITFIELDS masks.
    pub fn decode(d: &[u8]) -> Result<Picture> {
        let offset = u32le(d, 10)? as usize;
        let dib = u32le(d, 14)? as usize;
        let (w, h_raw, bpp, compression, colors_used, top_down);
        if dib == 12 {
            w = u16le(d, 18)?;
            h_raw = u16le(d, 20)?;
            bpp = u16le(d, 24)?;
            compression = 0;
            colors_used = 0;
            top_down = false;
        } else if dib >= 40 {
            w = u32le(d, 18)? as i32 as isize as usize;
            let hh = u32le(d, 22)? as i32;
            top_down = hh < 0;
            h_raw = hh.unsigned_abs() as usize;
            bpp = u16le(d, 28)?;
            compression = u32le(d, 30)?;
            colors_used = u32le(d, 46)? as usize;
        } else {
            return Err("unsupported BMP header");
        }
        let h = h_raw;
        check_size(w, h)?;
        if !matches!(bpp, 1 | 2 | 4 | 8 | 16 | 24 | 32) {
            return Err("unsupported BMP bit depth");
        }

        // Channel masks for 16/32-bit pixels.
        let (mut masks, mut has_alpha) = match bpp {
            16 => ([0x7C00u32, 0x03E0, 0x001F, 0], false),
            _ => ([0x00FF_0000u32, 0x0000_FF00, 0x0000_00FF, 0], false),
        };
        match compression {
            0 => {}
            3 | 6 if bpp == 16 || bpp == 32 => {
                masks = [u32le(d, 54)?, u32le(d, 58)?, u32le(d, 62)?, 0];
                if compression == 6 || dib >= 56 {
                    masks[3] = u32le(d, 66).unwrap_or(0);
                    has_alpha = masks[3] != 0;
                }
            }
            _ => return Err("compressed BMPs are not supported"),
        }

        let mut palette = [0xFF00_0000u32; 256];
        if bpp <= 8 {
            let entry = if dib == 12 { 3 } else { 4 };
            let count = if colors_used == 0 || colors_used > 256 { 1 << bpp } else { colors_used };
            let base = 14 + dib;
            for (i, p) in palette.iter_mut().enumerate().take(count) {
                let at = base + i * entry;
                let Some(c) = d.get(at..at + 3) else { break };
                *p = 0xFF00_0000 | (c[2] as u32) << 16 | (c[1] as u32) << 8 | c[0] as u32;
            }
        }

        let stride = (w * bpp + 31) / 32 * 4;
        if d.len() < offset + stride * (h - 1) + (w * bpp + 7) / 8 {
            return Err("truncated BMP");
        }
        let channel = |px: u32, mask: u32| -> u32 {
            if mask == 0 {
                return 0;
            }
            let v = ((px & mask) >> mask.trailing_zeros()) as u64;
            let max = (mask >> mask.trailing_zeros()) as u64;
            (if max == 255 { v } else { (v * 255 + max / 2) / max }) as u32
        };
        let mut out = vec![0u32; w * h];
        for y in 0..h {
            let row = &d[offset + stride * y..];
            let oy = if top_down { y } else { h - 1 - y };
            let dst = &mut out[oy * w..oy * w + w];
            for (x, o) in dst.iter_mut().enumerate() {
                *o = match bpp {
                    1 | 4 | 8 => {
                        let bit = x * bpp;
                        let byte = row[bit / 8];
                        let idx = (byte >> (8 - bpp - bit % 8)) & ((1u16 << bpp) - 1) as u8;
                        palette[idx as usize]
                    }
                    24 => {
                        let p = &row[x * 3..x * 3 + 3];
                        0xFF00_0000 | (p[2] as u32) << 16 | (p[1] as u32) << 8 | p[0] as u32
                    }
                    _ => {
                        let px = if bpp == 16 {
                            u16::from_le_bytes([row[x * 2], row[x * 2 + 1]]) as u32
                        } else {
                            u32::from_le_bytes([row[x * 4], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3]])
                        };
                        let a = if has_alpha { channel(px, masks[3]) } else { 255 };
                        a << 24 | channel(px, masks[0]) << 16 | channel(px, masks[1]) << 8 | channel(px, masks[2])
                    }
                };
            }
        }
        Ok((w, h, out))
    }
}

// ---- GIF ---------------------------------------------------------------------------------------------------------

mod gif {
    use super::{check_size, u16le, Picture, Result};
    use alloc::vec;
    use alloc::vec::Vec;

    fn read_palette(d: &[u8], at: usize, count: usize) -> Result<[u32; 256]> {
        let src = d.get(at..at + count * 3).ok_or("truncated GIF palette")?;
        let mut pal = [0xFF00_0000u32; 256];
        for i in 0..count {
            pal[i] = 0xFF00_0000 | (src[i * 3] as u32) << 16 | (src[i * 3 + 1] as u32) << 8 | src[i * 3 + 2] as u32;
        }
        Ok(pal)
    }

    /// Joins the data sub-blocks starting at `pos`; returns them and the position after the terminator.
    fn sub_blocks(d: &[u8], mut pos: usize) -> Result<(Vec<u8>, usize)> {
        let mut data = Vec::new();
        loop {
            let n = *d.get(pos).ok_or("truncated GIF")? as usize;
            pos += 1;
            if n == 0 {
                return Ok((data, pos));
            }
            // A file cut short still shows what arrived.
            let end = core::cmp::min(pos + n, d.len());
            data.extend_from_slice(&d[pos..end]);
            pos += n;
            if pos >= d.len() {
                return Ok((data, d.len()));
            }
        }
    }

    fn skip_sub_blocks(d: &[u8], mut pos: usize) -> Result<usize> {
        loop {
            let n = *d.get(pos).ok_or("truncated GIF")? as usize;
            pos += 1 + n;
            if n == 0 {
                return Ok(pos);
            }
        }
    }

    /// LZW-decodes into `out` (palette indices), stopping when it is full or the data ends.
    fn lzw(data: &[u8], min_size: u32, out: &mut [u8]) -> Result<()> {
        if !(1..=11).contains(&min_size) {
            return Err("bad GIF code size");
        }
        let clear = 1u32 << min_size;
        let end = clear + 1;
        let mut prefix = [0u16; 4096];
        let mut suffix = [0u8; 4096];
        let mut first = [0u8; 4096];
        for i in 0..clear as usize {
            suffix[i] = i as u8;
            first[i] = i as u8;
        }
        let mut stack = [0u8; 4097];
        let mut size = min_size + 1;
        let mut next = end + 1;
        let mut prev: Option<u32> = None;
        let (mut acc, mut bits, mut pos, mut o) = (0u32, 0u32, 0usize, 0usize);
        while o < out.len() {
            while bits < size {
                let Some(&b) = data.get(pos) else { return Ok(()) };
                acc |= (b as u32) << bits;
                bits += 8;
                pos += 1;
            }
            let code = acc & ((1 << size) - 1);
            acc >>= size;
            bits -= size;
            if code == clear {
                size = min_size + 1;
                next = end + 1;
                prev = None;
                continue;
            }
            if code == end {
                return Ok(());
            }
            let Some(p) = prev else {
                if code >= clear {
                    return Err("bad GIF data");
                }
                out[o] = code as u8;
                o += 1;
                prev = Some(code);
                continue;
            };
            // The string for `code`; a code not yet in the table is prev's string plus its own first byte.
            let (mut c, mut sp) = (code, 0usize);
            if code >= next {
                if code > next {
                    return Err("bad GIF data");
                }
                stack[sp] = first[p as usize];
                sp += 1;
                c = p;
            }
            while c >= clear {
                stack[sp] = suffix[c as usize];
                sp += 1;
                c = prefix[c as usize] as u32;
            }
            stack[sp] = c as u8;
            sp += 1;
            let lead = c as u8;
            while sp > 0 && o < out.len() {
                sp -= 1;
                out[o] = stack[sp];
                o += 1;
            }
            if next < 4096 {
                prefix[next as usize] = p as u16;
                suffix[next as usize] = lead;
                first[next as usize] = first[p as usize];
                next += 1;
                if next == 1 << size && size < 12 {
                    size += 1;
                }
            }
            prev = Some(code);
        }
        Ok(())
    }

    /// The first frame, drawn on a transparent canvas the size of the logical screen.
    pub fn decode(d: &[u8]) -> Result<Picture> {
        let sw = u16le(d, 6)?;
        let sh = u16le(d, 8)?;
        check_size(sw, sh)?;
        let flags = *d.get(10).ok_or("truncated GIF")?;
        let mut pos = 13;
        let mut global = [0xFF00_0000u32; 256];
        if flags & 0x80 != 0 {
            let n = 2usize << (flags & 7);
            global = read_palette(d, pos, n)?;
            pos += n * 3;
        }
        let mut transparent: Option<u8> = None;
        loop {
            match *d.get(pos).ok_or("GIF has no picture")? {
                0x21 => {
                    let label = *d.get(pos + 1).ok_or("truncated GIF")?;
                    if label == 0xF9 && d.get(pos + 2) == Some(&4) {
                        let packed = *d.get(pos + 3).ok_or("truncated GIF")?;
                        transparent = if packed & 1 != 0 { Some(*d.get(pos + 6).ok_or("truncated GIF")?) } else { None };
                    }
                    pos = skip_sub_blocks(d, pos + 2)?;
                }
                0x2C => {
                    let left = u16le(d, pos + 1)?;
                    let top = u16le(d, pos + 3)?;
                    let fw = u16le(d, pos + 5)?;
                    let fh = u16le(d, pos + 7)?;
                    let packed = *d.get(pos + 9).ok_or("truncated GIF")?;
                    pos += 10;
                    let palette = if packed & 0x80 != 0 {
                        let n = 2usize << (packed & 7);
                        let p = read_palette(d, pos, n)?;
                        pos += n * 3;
                        p
                    } else {
                        global
                    };
                    check_size(fw, fh)?;
                    let min_size = *d.get(pos).ok_or("truncated GIF")? as u32;
                    let (data, _) = sub_blocks(d, pos + 1)?;
                    let mut idx = vec![0u8; fw * fh];
                    if let Some(t) = transparent {
                        idx.fill(t); // pixels the data never reaches stay see-through
                    }
                    lzw(&data, min_size, &mut idx)?;

                    // Interlaced frames store rows 0,8,16.. then 4,12.. then 2,6.. then 1,3..
                    let mut rows: Vec<usize> = (0..fh).collect();
                    if packed & 0x40 != 0 {
                        rows.clear();
                        for (start, step) in [(0, 8), (4, 8), (2, 4), (1, 2)] {
                            rows.extend((start..fh).step_by(step));
                        }
                    }
                    let mut out = vec![0u32; sw * sh];
                    for (src_row, &y) in rows.iter().enumerate() {
                        let cy = top + y;
                        if cy >= sh {
                            continue;
                        }
                        for x in 0..fw {
                            let cx = left + x;
                            if cx >= sw {
                                break;
                            }
                            let i = idx[src_row * fw + x];
                            if Some(i) != transparent {
                                out[cy * sw + cx] = palette[i as usize];
                            }
                        }
                    }
                    return Ok((sw, sh, out));
                }
                _ => return Err("GIF has no picture"),
            }
        }
    }
}

// ---- JPEG --------------------------------------------------------------------------------------------------------

mod jpeg {
    use super::{check_size, Picture, Result};
    use alloc::vec;
    use alloc::vec::Vec;

    /// Zig-zag position -> natural (row-major) position, padded so a corrupt run can't index past it.
    const ZZ: [usize; 80] = [
        0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
        28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
        54, 47, 55, 62, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,
    ];

    struct Huff {
        fast: [u16; 512], // 9-bit prefix -> (length << 8 | value), 0 if the code is longer
        maxcode: [i32; 18],
        valptr: [i32; 17],
        mincode: [i32; 17],
        vals: [u8; 256],
    }

    impl Huff {
        fn new(counts: &[u8], vals: &[u8]) -> Result<Huff> {
            let mut h = Huff { fast: [0; 512], maxcode: [-1; 18], valptr: [0; 17], mincode: [0; 17], vals: [0; 256] };
            if vals.len() > 256 {
                return Err("bad JPEG Huffman table");
            }
            h.vals[..vals.len()].copy_from_slice(vals);
            let (mut code, mut k) = (0i32, 0usize);
            for len in 1..=16usize {
                h.valptr[len] = k as i32;
                h.mincode[len] = code;
                for _ in 0..counts[len - 1] {
                    if k >= vals.len() || code >= 1 << len {
                        return Err("bad JPEG Huffman table");
                    }
                    if len <= 9 {
                        let base = (code as usize) << (9 - len);
                        for e in &mut h.fast[base..base + (1 << (9 - len))] {
                            *e = (len as u16) << 8 | vals[k] as u16;
                        }
                    }
                    code += 1;
                    k += 1;
                }
                h.maxcode[len] = if counts[len - 1] > 0 { code - 1 } else { -1 };
                code <<= 1;
            }
            h.maxcode[17] = i32::MAX;
            Ok(h)
        }
    }

    /// Entropy-coded data: byte stuffing removed, zeros fed once a marker is reached.
    struct Bits<'a> {
        d: &'a [u8],
        pos: usize,
        acc: u32,
        n: u32,
        marker: bool,
    }

    impl<'a> Bits<'a> {
        fn fill(&mut self) {
            while self.n <= 24 {
                let mut b = 0u32;
                while !self.marker && self.pos < self.d.len() {
                    let v = self.d[self.pos];
                    if v != 0xFF {
                        self.pos += 1;
                        b = v as u32;
                        break;
                    }
                    match self.d.get(self.pos + 1) {
                        Some(0) => {
                            self.pos += 2;
                            b = 0xFF;
                            break;
                        }
                        Some(0xFF) => self.pos += 1, // fill byte before a marker
                        _ => self.marker = true,
                    }
                }
                self.acc |= b << (24 - self.n);
                self.n += 8;
            }
        }

        fn bits(&mut self, k: u32) -> u32 {
            if k == 0 {
                return 0;
            }
            self.fill();
            let v = self.acc >> (32 - k);
            self.acc <<= k;
            self.n -= k;
            v
        }

        fn bit(&mut self) -> bool {
            self.bits(1) != 0
        }

        /// A `s`-bit magnitude category value with its sign (JPEG's EXTEND).
        fn extend(&mut self, s: u32) -> i32 {
            if s == 0 {
                return 0;
            }
            if s > 16 {
                return 0;
            }
            let v = self.bits(s) as i32;
            if v < 1 << (s - 1) { v - (1 << s) + 1 } else { v }
        }

        fn decode(&mut self, h: &Huff) -> Result<u8> {
            self.fill();
            let e = h.fast[(self.acc >> 23) as usize];
            if e != 0 {
                let len = (e >> 8) as u32;
                self.acc <<= len;
                self.n -= len;
                return Ok(e as u8);
            }
            let mut code = 0i32;
            for len in 1..=16usize {
                code = code << 1 | self.bits(1) as i32;
                if code <= h.maxcode[len] {
                    return Ok(h.vals[(h.valptr[len] + code - h.mincode[len]) as usize & 255]);
                }
            }
            Err("bad JPEG Huffman code")
        }

        /// After a restart interval: drop the leftover bits and step over the RSTn marker.
        fn restart(&mut self) {
            self.acc = 0;
            self.n = 0;
            self.marker = false;
            while self.pos + 1 < self.d.len() {
                if self.d[self.pos] == 0xFF && (0xD0..=0xD7).contains(&self.d[self.pos + 1]) {
                    self.pos += 2;
                    return;
                }
                if self.d[self.pos] == 0xFF && self.d[self.pos + 1] != 0 && self.d[self.pos + 1] != 0xFF {
                    return; // some other marker: leave it for the segment parser
                }
                self.pos += 1;
            }
        }
    }

    struct Comp {
        id: u8,
        h: usize,
        v: usize,
        tq: usize,
        td: usize,
        ta: usize,
        bw: usize, // blocks per row in the padded plane
        bh: usize,
        coef: Vec<i16>,
        pred: i32,
    }

    struct Jpeg {
        w: usize,
        h: usize,
        progressive: bool,
        comps: Vec<Comp>,
        hmax: usize,
        vmax: usize,
        mcux: usize,
        mcuy: usize,
        qt: [[u16; 64]; 4],
        dc: [Option<Huff>; 4],
        ac: [Option<Huff>; 4],
        restart: usize,
        eobrun: u32,
        adobe: Option<u8>,
        frame: bool,
    }

    fn be16(d: &[u8], at: usize) -> Result<usize> {
        d.get(at..at + 2).map(|b| (b[0] as usize) << 8 | b[1] as usize).ok_or("truncated JPEG")
    }

    pub fn decode(d: &[u8]) -> Result<Picture> {
        let mut j = Jpeg {
            w: 0,
            h: 0,
            progressive: false,
            comps: Vec::new(),
            hmax: 1,
            vmax: 1,
            mcux: 0,
            mcuy: 0,
            qt: [[1; 64]; 4],
            dc: [None, None, None, None],
            ac: [None, None, None, None],
            restart: 0,
            eobrun: 0,
            adobe: None,
            frame: false,
        };
        let mut pos = 2;
        let mut scans = 0;
        loop {
            // Next marker (a cut-short file ends here with what was decoded).
            while pos < d.len() && d[pos] != 0xFF {
                pos += 1;
            }
            while pos < d.len() && d[pos] == 0xFF {
                pos += 1;
            }
            let Some(&m) = d.get(pos) else { break };
            pos += 1;
            match m {
                0xD9 => break,
                0xD0..=0xD7 | 0x01 => continue,
                _ => {}
            }
            let len = be16(d, pos)?;
            if len < 2 {
                return Err("bad JPEG segment");
            }
            let seg = d.get(pos + 2..pos + len).ok_or("truncated JPEG")?;
            match m {
                0xC0 | 0xC1 | 0xC2 => {
                    j.progressive = m == 0xC2;
                    frame(&mut j, seg)?;
                }
                0xC3 | 0xC5..=0xC7 | 0xC9..=0xCB | 0xCD..=0xCF => return Err("unsupported JPEG coding (lossless, hierarchical or arithmetic)"),
                0xC4 => huffman(&mut j, seg)?,
                0xDB => quant(&mut j, seg)?,
                0xDD => j.restart = be16(seg, 0)?,
                0xEE if seg.len() >= 12 && seg.starts_with(b"Adobe") => j.adobe = Some(seg[11]),
                0xDA => {
                    if !j.frame {
                        return Err("JPEG scan before its frame header");
                    }
                    let mut bits = Bits { d, pos: pos + len, acc: 0, n: 0, marker: false };
                    scan(&mut j, seg, &mut bits)?;
                    scans += 1;
                    pos = bits.pos;
                    continue;
                }
                _ => {}
            }
            pos += len;
        }
        if scans == 0 {
            return Err("JPEG has no picture data");
        }
        Ok((j.w, j.h, output(&mut j)?))
    }

    fn frame(j: &mut Jpeg, s: &[u8]) -> Result<()> {
        if j.frame {
            return Err("JPEG has two frames");
        }
        if s.len() < 6 || s[0] != 8 {
            return Err("only 8-bit JPEGs are supported");
        }
        j.h = be16(s, 1)?;
        j.w = be16(s, 3)?;
        check_size(j.w, j.h)?;
        let n = s[5] as usize;
        if !(n == 1 || n == 3 || n == 4) || s.len() < 6 + n * 3 {
            return Err("unsupported JPEG component count");
        }
        for i in 0..n {
            let c = &s[6 + i * 3..9 + i * 3];
            let (h, v) = ((c[1] >> 4) as usize, (c[1] & 15) as usize);
            if !(1..=4).contains(&h) || !(1..=4).contains(&v) || c[2] > 3 {
                return Err("bad JPEG component");
            }
            j.comps.push(Comp { id: c[0], h, v, tq: c[2] as usize, td: 0, ta: 0, bw: 0, bh: 0, coef: Vec::new(), pred: 0 });
        }
        j.hmax = j.comps.iter().map(|c| c.h).max().unwrap_or(1);
        j.vmax = j.comps.iter().map(|c| c.v).max().unwrap_or(1);
        j.mcux = (j.w + 8 * j.hmax - 1) / (8 * j.hmax);
        j.mcuy = (j.h + 8 * j.vmax - 1) / (8 * j.vmax);
        for c in &mut j.comps {
            c.bw = j.mcux * c.h;
            c.bh = j.mcuy * c.v;
            c.coef = vec![0i16; c.bw * c.bh * 64];
        }
        j.frame = true;
        Ok(())
    }

    fn huffman(j: &mut Jpeg, s: &[u8]) -> Result<()> {
        let mut p = 0;
        while p < s.len() {
            let tc = s[p] >> 4;
            let th = (s[p] & 15) as usize;
            let counts = s.get(p + 1..p + 17).ok_or("truncated JPEG Huffman table")?;
            let total: usize = counts.iter().map(|&c| c as usize).sum();
            let vals = s.get(p + 17..p + 17 + total).ok_or("truncated JPEG Huffman table")?;
            if th > 3 || tc > 1 {
                return Err("bad JPEG Huffman table");
            }
            let t = Huff::new(counts, vals)?;
            if tc == 0 { j.dc[th] = Some(t) } else { j.ac[th] = Some(t) }
            p += 17 + total;
        }
        Ok(())
    }

    fn quant(j: &mut Jpeg, s: &[u8]) -> Result<()> {
        let mut p = 0;
        while p < s.len() {
            let pq = s[p] >> 4;
            let tq = (s[p] & 15) as usize;
            if tq > 3 || pq > 1 {
                return Err("bad JPEG quantisation table");
            }
            let size = if pq == 0 { 64 } else { 128 };
            let t = s.get(p + 1..p + 1 + size).ok_or("truncated JPEG quantisation table")?;
            for k in 0..64 {
                j.qt[tq][ZZ[k]] = if pq == 0 { t[k] as u16 } else { (t[2 * k] as u16) << 8 | t[2 * k + 1] as u16 };
            }
            p += 1 + size;
        }
        Ok(())
    }

    fn scan(j: &mut Jpeg, s: &[u8], bits: &mut Bits) -> Result<()> {
        let n = *s.first().ok_or("bad JPEG scan")? as usize;
        if n == 0 || n > 4 || s.len() < 1 + n * 2 + 3 {
            return Err("bad JPEG scan");
        }
        let mut members = Vec::new();
        for i in 0..n {
            let id = s[1 + i * 2];
            let t = s[2 + i * 2];
            let ci = j.comps.iter().position(|c| c.id == id).ok_or("JPEG scan names an unknown component")?;
            j.comps[ci].td = (t >> 4) as usize & 3;
            j.comps[ci].ta = (t & 15) as usize & 3;
            members.push(ci);
        }
        let ss = s[1 + n * 2] as usize;
        let se = s[2 + n * 2] as usize;
        let ah = (s[3 + n * 2] >> 4) as u32;
        let al = (s[3 + n * 2] & 15) as u32;
        // Some encoders write odd Ss/Se values in baseline scans; they always mean the whole block.
        let (ss, se) = if j.progressive { (ss, se) } else { (0, 63) };
        if se > 63 || ss > se || (j.progressive && ss > 0 && n != 1) {
            return Err("bad JPEG progressive scan");
        }
        for c in &mut j.comps {
            c.pred = 0;
        }
        j.eobrun = 0;

        // One block of the scan: (component, block column, block row).
        let mut units: usize = 0;
        let each = |j: &mut Jpeg, ci: usize, bx: usize, by: usize, bits: &mut Bits| -> Result<()> {
            let c = &j.comps[ci];
            let at = (by * c.bw + bx) * 64;
            if !j.progressive {
                block_baseline(j, ci, at, bits)
            } else if ss == 0 {
                block_dc(j, ci, at, ah, al, bits)
            } else if ah == 0 {
                block_ac_first(j, ci, at, ss, se, al, bits)
            } else {
                block_ac_refine(j, ci, at, ss, se, al, bits)
            }
        };
        let restart_check = |j: &mut Jpeg, bits: &mut Bits, units: &mut usize| {
            if j.restart > 0 {
                if *units > 0 && *units % j.restart == 0 {
                    bits.restart();
                    for c in &mut j.comps {
                        c.pred = 0;
                    }
                    j.eobrun = 0;
                }
                *units += 1;
            }
        };

        if n == 1 {
            // Non-interleaved: the component's own blocks, without MCU padding.
            let ci = members[0];
            let (ch, cv) = (j.comps[ci].h, j.comps[ci].v);
            let cw = (j.w * ch + j.hmax - 1) / j.hmax;
            let chh = (j.h * cv + j.vmax - 1) / j.vmax;
            let (bx_n, by_n) = ((cw + 7) / 8, (chh + 7) / 8);
            for by in 0..by_n {
                for bx in 0..bx_n {
                    restart_check(j, bits, &mut units);
                    each(j, ci, bx, by, bits)?;
                }
            }
        } else {
            for my in 0..j.mcuy {
                for mx in 0..j.mcux {
                    restart_check(j, bits, &mut units);
                    for &ci in &members {
                        let (ch, cv) = (j.comps[ci].h, j.comps[ci].v);
                        for v in 0..cv {
                            for h in 0..ch {
                                each(j, ci, mx * ch + h, my * cv + v, bits)?;
                            }
                        }
                    }
                }
            }
        }
        // Leave pos at the marker that ended the scan.
        if !bits.marker {
            while bits.pos + 1 < bits.d.len() && !(bits.d[bits.pos] == 0xFF && bits.d[bits.pos + 1] != 0) {
                bits.pos += 1;
            }
        }
        Ok(())
    }

    fn table<'a>(t: &'a Option<Huff>) -> Result<&'a Huff> {
        t.as_ref().ok_or("JPEG scan uses a missing Huffman table")
    }

    fn block_baseline(j: &mut Jpeg, ci: usize, at: usize, bits: &mut Bits) -> Result<()> {
        let (td, ta) = (j.comps[ci].td, j.comps[ci].ta);
        let dc = table(&j.dc[td])?;
        let t = bits.decode(dc)? as u32;
        let diff = bits.extend(t);
        let ac = table(&j.ac[ta])?;
        let c = &mut j.comps[ci];
        c.pred = c.pred.wrapping_add(diff);
        let blk = &mut c.coef[at..at + 64];
        blk[0] = c.pred as i16;
        let mut k = 1;
        while k < 64 {
            let rs = bits.decode(ac)?;
            let (r, s) = ((rs >> 4) as usize, (rs & 15) as u32);
            if s == 0 {
                if r == 15 {
                    k += 16;
                    continue;
                }
                break;
            }
            k += r;
            if k > 63 {
                break;
            }
            blk[ZZ[k]] = bits.extend(s) as i16;
            k += 1;
        }
        Ok(())
    }

    fn block_dc(j: &mut Jpeg, ci: usize, at: usize, ah: u32, al: u32, bits: &mut Bits) -> Result<()> {
        if ah == 0 {
            let dc = table(&j.dc[j.comps[ci].td])?;
            let t = bits.decode(dc)? as u32;
            let diff = bits.extend(t);
            let c = &mut j.comps[ci];
            c.pred = c.pred.wrapping_add(diff);
            c.coef[at] = c.pred.wrapping_shl(al) as i16;
        } else if bits.bit() {
            j.comps[ci].coef[at] |= 1 << al;
        }
        Ok(())
    }

    fn block_ac_first(j: &mut Jpeg, ci: usize, at: usize, ss: usize, se: usize, al: u32, bits: &mut Bits) -> Result<()> {
        if j.eobrun > 0 {
            j.eobrun -= 1;
            return Ok(());
        }
        let ac = table(&j.ac[j.comps[ci].ta])?;
        let blk = &mut j.comps[ci].coef[at..at + 64];
        let mut k = ss;
        while k <= se {
            let rs = bits.decode(ac)?;
            let (r, s) = ((rs >> 4) as u32, (rs & 15) as u32);
            if s != 0 {
                k += r as usize;
                if k > 63 {
                    break;
                }
                blk[ZZ[k]] = bits.extend(s).wrapping_shl(al) as i16;
            } else if r == 15 {
                k += 15;
            } else {
                let mut run = 1u32 << r;
                if r > 0 {
                    run += bits.bits(r);
                }
                j.eobrun = run - 1;
                break;
            }
            k += 1;
        }
        Ok(())
    }

    fn block_ac_refine(j: &mut Jpeg, ci: usize, at: usize, ss: usize, se: usize, al: u32, bits: &mut Bits) -> Result<()> {
        let p1: i16 = 1 << al;
        let m1: i16 = -1 << al;
        let ac = table(&j.ac[j.comps[ci].ta])?;
        let blk = &mut j.comps[ci].coef[at..at + 64];
        let refine = |c: &mut i16, bits: &mut Bits| {
            if bits.bit() && *c & p1 == 0 {
                *c = c.wrapping_add(if *c >= 0 { p1 } else { m1 });
            }
        };
        let mut k = ss;
        if j.eobrun == 0 {
            while k <= se {
                let rs = bits.decode(ac)?;
                let mut r = (rs >> 4) as i32;
                let s = rs & 15;
                let mut val = 0i16;
                if s != 0 {
                    val = if bits.bit() { p1 } else { m1 };
                } else if r != 15 {
                    j.eobrun = 1 << r;
                    if r > 0 {
                        j.eobrun += bits.bits(r as u32);
                    }
                    break;
                }
                while k <= se {
                    let z = ZZ[k];
                    if blk[z] != 0 {
                        refine(&mut blk[z], bits);
                    } else {
                        r -= 1;
                        if r < 0 {
                            break;
                        }
                    }
                    k += 1;
                }
                if val != 0 && k <= 63 {
                    blk[ZZ[k]] = val;
                }
                k += 1;
            }
        }
        if j.eobrun > 0 {
            while k <= se {
                let z = ZZ[k];
                if blk[z] != 0 {
                    refine(&mut blk[z], bits);
                }
                k += 1;
            }
            j.eobrun -= 1;
        }
        Ok(())
    }

    // Integer inverse DCT (the "islow" algorithm of the IJG libjpeg, jidctint.c).
    const CONST_BITS: i32 = 13;
    const PASS1_BITS: i32 = 2;
    const F0_298: i32 = 2446;
    const F0_390: i32 = 3196;
    const F0_541: i32 = 4433;
    const F0_765: i32 = 6270;
    const F0_899: i32 = 7373;
    const F1_175: i32 = 9633;
    const F1_501: i32 = 12299;
    const F1_847: i32 = 15137;
    const F1_961: i32 = 16069;
    const F2_053: i32 = 16819;
    const F2_562: i32 = 20995;
    const F3_072: i32 = 25172;

    fn descale(x: i32, n: i32) -> i32 {
        x.wrapping_add(1 << (n - 1)) >> n
    }

    /// The 1-D 8-point IDCT both passes share: returns (out0..out7) before descaling. Wrapping arithmetic: valid
    /// data never comes near i32's range, and corrupt data must give garbage pixels rather than a panic.
    fn idct8(i: [i32; 8]) -> [i32; 8] {
        use core::num::Wrapping as W;
        let c = |k: i32| W(k);
        let v: [W<i32>; 8] = i.map(W);
        let z1 = (v[2] + v[6]) * c(F0_541);
        let tmp2 = z1 - v[6] * c(F1_847);
        let tmp3 = z1 + v[2] * c(F0_765);
        let tmp0 = W((v[0] + v[4]).0.wrapping_shl(CONST_BITS as u32));
        let tmp1 = W((v[0] - v[4]).0.wrapping_shl(CONST_BITS as u32));
        let (t10, t13, t11, t12) = (tmp0 + tmp3, tmp0 - tmp3, tmp1 + tmp2, tmp1 - tmp2);

        let (mut o0, mut o1, mut o2, mut o3) = (v[7], v[5], v[3], v[1]);
        let z1 = o0 + o3;
        let z2 = o1 + o2;
        let z3 = o0 + o2;
        let z4 = o1 + o3;
        let z5 = (z3 + z4) * c(F1_175);
        o0 *= c(F0_298);
        o1 *= c(F2_053);
        o2 *= c(F3_072);
        o3 *= c(F1_501);
        let z1 = -z1 * c(F0_899);
        let z2 = -z2 * c(F2_562);
        let z3 = -z3 * c(F1_961) + z5;
        let z4 = -z4 * c(F0_390) + z5;
        o0 += z1 + z3;
        o1 += z2 + z4;
        o2 += z2 + z3;
        o3 += z1 + z4;
        [t10 + o3, t11 + o2, t12 + o1, t13 + o0, t13 - o0, t12 - o1, t11 - o2, t10 - o3].map(|x| x.0)
    }

    fn idct_block(coef: &[i16], q: &[u16; 64], out: &mut [u8], stride: usize) {
        let mut ws = [0i32; 64];
        for col in 0..8 {
            let mut v = [0i32; 8];
            for r in 0..8 {
                // Valid data stays far inside this range; clamping keeps corrupt data from overflowing below.
                v[r] = (coef[r * 8 + col] as i32 * q[r * 8 + col] as i32).clamp(-4096, 4095);
            }
            if v[1..].iter().all(|&x| x == 0) {
                for r in 0..8 {
                    ws[r * 8 + col] = v[0] * (1 << PASS1_BITS);
                }
                continue;
            }
            let o = idct8(v);
            for r in 0..8 {
                ws[r * 8 + col] = descale(o[r], CONST_BITS - PASS1_BITS);
            }
        }
        for row in 0..8 {
            let mut v = [0i32; 8];
            v.copy_from_slice(&ws[row * 8..row * 8 + 8]);
            let o = idct8(v);
            for x in 0..8 {
                let p = descale(o[x], CONST_BITS + PASS1_BITS + 3) + 128;
                out[row * stride + x] = p.clamp(0, 255) as u8;
            }
        }
    }

    fn clamp8(x: i32) -> u32 {
        x.clamp(0, 255) as u32
    }

    fn ycc(y: i32, cb: i32, cr: i32) -> (u32, u32, u32) {
        let (cb, cr) = (cb - 128, cr - 128);
        (
            clamp8(y + ((91881 * cr + 32768) >> 16)),
            clamp8(y - ((22554 * cb + 46802 * cr + 32768) >> 16)),
            clamp8(y + ((116130 * cb + 32768) >> 16)),
        )
    }

    fn blinn(x: u32, y: u32) -> u32 {
        let t = x * y + 128;
        (t + (t >> 8)) >> 8
    }

    fn output(j: &mut Jpeg) -> Result<Vec<u32>> {
        // Each component's samples, full plane (padded to whole blocks).
        let mut planes: Vec<(Vec<u8>, usize)> = Vec::new();
        for c in &mut j.comps {
            let stride = c.bw * 8;
            let mut plane = vec![0u8; stride * c.bh * 8];
            let q = j.qt[c.tq];
            for by in 0..c.bh {
                for bx in 0..c.bw {
                    let at = (by * c.bw + bx) * 64;
                    idct_block(&c.coef[at..at + 64], &q, &mut plane[by * 8 * stride + bx * 8..], stride);
                }
            }
            c.coef = Vec::new();
            planes.push((plane, stride));
        }
        let (w, h) = (j.w, j.h);
        let mut out = vec![0u32; w * h];
        let rgb_ids = j.comps.len() == 3 && j.comps[0].id == b'R' && j.comps[1].id == b'G' && j.comps[2].id == b'B';
        let n = j.comps.len();
        let mut s = [0i32; 4];
        for y in 0..h {
            for x in 0..w {
                for (ci, c) in j.comps.iter().enumerate() {
                    // Nearest sample (box upsampling) for subsampled components.
                    let sx = x * c.h / j.hmax;
                    let sy = y * c.v / j.vmax;
                    s[ci] = planes[ci].0[sy * planes[ci].1 + sx] as i32;
                }
                let (r, g, b) = match n {
                    1 => (s[0] as u32, s[0] as u32, s[0] as u32),
                    3 if rgb_ids || j.adobe == Some(0) => (s[0] as u32, s[1] as u32, s[2] as u32),
                    3 => ycc(s[0], s[1], s[2]),
                    _ => {
                        let k = s[3] as u32;
                        if j.adobe == Some(2) {
                            let (r, g, b) = ycc(s[0], s[1], s[2]);
                            (blinn(255 - r, k), blinn(255 - g, k), blinn(255 - b, k))
                        } else {
                            (blinn(s[0] as u32, k), blinn(s[1] as u32, k), blinn(s[2] as u32, k))
                        }
                    }
                };
                out[y * w + x] = 0xFF00_0000 | r << 16 | g << 8 | b;
            }
        }
        Ok(out)
    }
}
