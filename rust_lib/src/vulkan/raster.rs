// Software rasterizer behind vkCmdDraw: runs the vertex shader, assembles and clips primitives, rasterizes triangles
// with perspective-correct interpolation and runs the fragment shader for every covered pixel.

use super::spirv::{Fault, Machine, Module, Slot};
use super::MAX_VARYING_WORDS;
use alloc::{vec, vec::Vec};

pub const MAX_VERTEX_BINDINGS: usize = 8;

#[derive(Clone, Copy, Default)]
pub struct Attribute { pub location: u32, pub binding: u32, pub format: u32, pub offset: u32 }

#[derive(Clone, Copy, Default)]
pub struct VertexBinding { pub stride: u32, pub per_instance: bool }

#[derive(Clone, Copy)]
pub struct Blend {
    pub enable: bool,
    pub src_color: u32, pub dst_color: u32, pub color_op: u32,
    pub src_alpha: u32, pub dst_alpha: u32, pub alpha_op: u32,
    pub write_mask: u32,
}

pub struct Pipe {
    pub vs: Module,
    pub fs: Module,
    pub vsm: Machine,
    pub fsm: Machine,
    pub attrs: Vec<Attribute>,
    pub bindings: [VertexBinding; MAX_VERTEX_BINDINGS],
    pub topology: u32,
    pub cull: u32,            // bit 0 front, bit 1 back
    pub front_ccw: bool,
    pub depth_test: bool,
    pub depth_write: bool,
    pub depth_op: u32,
    pub blend: Blend,
    pub viewport: Option<[f32; 6]>,
    pub scissor: Option<[i32; 4]>,   // x, y, w, h
    pub dyn_viewport: bool,
    pub dyn_scissor: bool,
}

pub struct Target {
    pub color: *mut u32,
    pub depth: *mut f32,
    pub width: i32,
    pub height: i32,
    pub swap_rb: bool,      // memory order R,G,B,A instead of B,G,R,A
    pub depth_w: i32,
}

#[derive(Clone, Copy)]
pub struct VBuf { pub ptr: *const u8, pub len: usize }

pub struct Draw<'a> {
    pub first_vertex: u32,
    pub vertex_count: u32,
    pub first_instance: u32,
    pub instance_count: u32,
    pub index: Option<(VBuf, bool, u32, i32)>,   // buffer (from its binding offset), 32-bit?, first index, vertex offset
    pub vbufs: &'a [(VBuf, u32); MAX_VERTEX_BINDINGS],   // base pointer already offset; stride from pipeline
    pub viewport: [f32; 6],
    pub scissor: [i32; 4],
}

#[derive(Clone, Copy)]
struct Vtx { pos: [f32; 4], var: [u32; MAX_VARYING_WORDS] }

#[derive(Clone, Copy)]
struct SVtx { x: f32, y: f32, z: f32, iw: f32, var: [f32; MAX_VARYING_WORDS], raw: [u32; MAX_VARYING_WORDS] }

fn read_f32(buf: VBuf, off: usize) -> u32 {
    if off + 4 > buf.len || buf.ptr.is_null() { return 0; }
    unsafe { core::ptr::read_unaligned(buf.ptr.add(off) as *const u32) }
}

fn read_u8(buf: VBuf, off: usize) -> u32 {
    if off >= buf.len || buf.ptr.is_null() { return 0; }
    unsafe { *buf.ptr.add(off) as u32 }
}

// (component count, kind) for a vertex attribute format; kind 0 float, 1 uint, 2 sint, 3 unorm8, 4 unorm8 bgra
fn format_info(format: u32) -> Option<(usize, u32)> {
    Some(match format {
        100 => (1, 0), 103 => (2, 0), 106 => (3, 0), 109 => (4, 0),
        98 => (1, 1), 101 => (2, 1), 104 => (3, 1), 107 => (4, 1),
        99 => (1, 2), 102 => (2, 2), 105 => (3, 2), 108 => (4, 2),
        37 => (4, 3), 44 => (4, 4),
        _ => return None,
    })
}

fn fetch_attribute(buf: VBuf, off: usize, format: u32) -> [u32; 4] {
    let Some((n, kind)) = format_info(format) else { return [0, 0, 0, 0x3F80_0000] };
    let one = if kind == 0 || kind >= 3 { 0x3F80_0000 } else { 1 };
    let mut out = [0, 0, 0, one];
    match kind {
        3 | 4 => {
            let px = [read_u8(buf, off), read_u8(buf, off + 1), read_u8(buf, off + 2), read_u8(buf, off + 3)];
            let c = if kind == 3 { px } else { [px[2], px[1], px[0], px[3]] };
            for k in 0..4 { out[k] = ((c[k] as f32) / 255.0).to_bits(); }
        }
        _ => { for k in 0..n { out[k] = read_f32(buf, off + k * 4); } }
    }
    out
}

fn compare(op: u32, a: f32, b: f32) -> bool {
    match op { 0 => false, 1 => a < b, 2 => a == b, 3 => a <= b, 4 => a > b, 5 => a != b, 6 => a >= b, _ => true }
}

fn blend_factor(f: u32, src: [f32; 4], dst: [f32; 4], comp: usize) -> f32 {
    match f {
        0 => 0.0,
        1 => 1.0,
        2 => src[comp],
        3 => 1.0 - src[comp],
        4 => dst[comp],
        5 => 1.0 - dst[comp],
        6 => src[3],
        7 => 1.0 - src[3],
        8 => dst[3],
        9 => 1.0 - dst[3],
        _ => 1.0,
    }
}

fn blend_op(op: u32, s: f32, d: f32) -> f32 {
    match op { 1 => s - d, 2 => d - s, 3 => s.min(d), 4 => s.max(d), _ => s + d }
}

fn unpack(px: u32, swap_rb: bool) -> [f32; 4] {
    let (b0, b1, b2, a) = (px & 0xFF, (px >> 8) & 0xFF, (px >> 16) & 0xFF, px >> 24);
    let (r, g, b) = if swap_rb { (b0, b1, b2) } else { (b2, b1, b0) };
    [r as f32 / 255.0, g as f32 / 255.0, b as f32 / 255.0, a as f32 / 255.0]
}

fn pack(c: [f32; 4], swap_rb: bool) -> u32 {
    let q = |v: f32| -> u32 { (v.max(0.0).min(1.0) * 255.0 + 0.5) as u32 };
    let (r, g, b, a) = (q(c[0]), q(c[1]), q(c[2]), q(c[3]));
    if swap_rb { (a << 24) | (b << 16) | (g << 8) | r } else { (a << 24) | (r << 16) | (g << 8) | b }
}

pub fn clear_color(t: &Target, rect: [i32; 4], color: [f32; 4]) {
    let px = pack(color, t.swap_rb);
    let (x0, y0) = (rect[0].max(0), rect[1].max(0));
    let (x1, y1) = ((rect[0] + rect[2]).min(t.width), (rect[1] + rect[3]).min(t.height));
    for y in y0..y1 {
        for x in x0..x1 { unsafe { *t.color.add((y * t.width + x) as usize) = px; } }
    }
}

pub fn clear_depth(t: &Target, rect: [i32; 4], depth: f32) {
    if t.depth.is_null() { return; }
    let (x0, y0) = (rect[0].max(0), rect[1].max(0));
    let (x1, y1) = ((rect[0] + rect[2]).min(t.depth_w), (rect[1] + rect[3]).min(t.height));
    for y in y0..y1 {
        for x in x0..x1 { unsafe { *t.depth.add((y * t.depth_w + x) as usize) = depth; } }
    }
}

fn run_vertex(p: &mut Pipe, d: &Draw, vertex_index: u32, instance: u32, vertex_id: i32) -> Option<Vtx> {
    let vs = &p.vs;
    for slot in vs.in_slots.iter() {
        if slot.builtin != u32::MAX {
            let v = match slot.builtin { 42 | 5 => (vertex_id as u32), 43 | 6 => instance, _ => 0 };
            p.vsm.set_input(slot, 0, &[v]);
            continue;
        }
        let mut val = [0, 0, 0, 0x3F80_0000];
        if let Some(a) = p.attrs.iter().find(|a| a.location == slot.location) {
            let bi = (a.binding as usize).min(MAX_VERTEX_BINDINGS - 1);
            let binding = p.bindings[bi];
            let index = if binding.per_instance { instance } else { vertex_index };
            let off = index as usize * binding.stride as usize + a.offset as usize;
            val = fetch_attribute(d.vbufs[bi].0, off, a.format);
        }
        p.vsm.set_input(slot, 0, &val);
    }
    match p.vsm.run(&p.vs, 1) {
        Ok(()) => {}
        Err(Fault::Killed) => return None,
        Err(_e) => { #[cfg(test)] std::println!("vertex fault {:?}", _e); return None }
    }
    let mut out = Vtx { pos: [0.0, 0.0, 0.0, 1.0], var: [0; MAX_VARYING_WORDS] };
    for slot in p.vs.out_slots.iter() {
        let mut words = [0u32; 16];
        p.vsm.output(slot, 0, &mut words);
        let n = (slot.words as usize).min(16);
        if slot.builtin == 0 {
            for k in 0..4.min(n) { out.pos[k] = f32::from_bits(words[k]); }
        } else if slot.builtin == u32::MAX {
            let at = slot.location as usize * 4;
            out.var[at..at + n].copy_from_slice(&words[..n]);
        }
    }
    Some(out)
}

fn lerp_vtx(a: &Vtx, b: &Vtx, t: f32, flat: &[Slot]) -> Vtx {
    let mut o = *a;
    for k in 0..4 { o.pos[k] = a.pos[k] + (b.pos[k] - a.pos[k]) * t; }
    for k in 0..MAX_VARYING_WORDS {
        o.var[k] = (f32::from_bits(a.var[k]) + (f32::from_bits(b.var[k]) - f32::from_bits(a.var[k])) * t).to_bits();
    }
    for s in flat {
        let at = s.location as usize * 4;
        for k in 0..s.words as usize { o.var[at + k] = a.var[at + k]; }
    }
    o
}

// Clips against z >= 0, z <= w and w >= epsilon; returns the polygon (at most 3 + 3 vertices).
fn clip_polygon(tri: [Vtx; 3], flat: &[Slot]) -> Vec<Vtx> {
    let mut poly: Vec<Vtx> = tri.to_vec();
    for plane in 0..3 {
        if poly.is_empty() { break; }
        let dist = |v: &Vtx| -> f32 {
            match plane { 0 => v.pos[2], 1 => v.pos[3] - v.pos[2], _ => v.pos[3] - 1e-5 }
        };
        let mut out: Vec<Vtx> = Vec::with_capacity(poly.len() + 1);
        for k in 0..poly.len() {
            let (a, b) = (&poly[k], &poly[(k + 1) % poly.len()]);
            let (da, db) = (dist(a), dist(b));
            if da >= 0.0 { out.push(*a); }
            if (da >= 0.0) != (db >= 0.0) {
                let t = da / (da - db);
                out.push(lerp_vtx(a, b, t, flat));
            }
        }
        poly = out;
    }
    poly
}

pub fn draw(p: &mut Pipe, t: &Target, d: &Draw) {
    let topo = p.topology;
    if !matches!(topo, 3 | 4 | 5) { return; }
    let flat: Vec<Slot> = p.fs.in_slots.iter().filter(|s| s.flat && s.builtin == u32::MAX).copied().collect();

    // vertex indices for the whole draw
    let mut ids: Vec<(u32, i32)> = Vec::with_capacity(d.vertex_count as usize);
    for k in 0..d.vertex_count {
        match d.index {
            Some((buf, wide, first, voff)) => {
                let at = (first + k) as usize;
                let raw = if wide { read_f32(buf, at * 4) } else { read_u8(buf, at * 2) | (read_u8(buf, at * 2 + 1) << 8) };
                ids.push(((raw as i32 + voff) as u32, raw as i32 + voff));
            }
            None => ids.push((d.first_vertex + k, (d.first_vertex + k) as i32)),
        }
    }

    for inst in 0..d.instance_count {
        let instance = d.first_instance + inst;
        let mut cache: Vec<Option<Vtx>> = vec![None; ids.len()];
        let mut shaded = |p: &mut Pipe, k: usize| -> Option<Vtx> {
            if cache[k].is_none() { cache[k] = run_vertex(p, d, ids[k].0, instance, ids[k].1); }
            cache[k]
        };
        let count = ids.len();
        let tris = match topo { 3 => count / 3, _ => count.saturating_sub(2) };
        for tri in 0..tris {
            let (i0, i1, i2) = match topo {
                3 => (tri * 3, tri * 3 + 1, tri * 3 + 2),
                4 => if tri % 2 == 0 { (tri, tri + 1, tri + 2) } else { (tri + 1, tri, tri + 2) },
                _ => (tri + 1, tri + 2, 0),
            };
            let (Some(a), Some(b), Some(c)) = (shaded(p, i0), shaded(p, i1), shaded(p, i2)) else { continue };
            let poly = clip_polygon([a, b, c], &flat);
            if poly.len() < 3 { continue; }
            for k in 1..poly.len() - 1 {
                raster_triangle(p, t, d, &poly[0], &poly[k], &poly[k + 1]);
            }
        }
    }
}

fn to_screen(v: &Vtx, vp: &[f32; 6]) -> SVtx {
    let iw = 1.0 / v.pos[3];
    let (nx, ny, nz) = (v.pos[0] * iw, v.pos[1] * iw, v.pos[2] * iw);
    let mut s = SVtx {
        x: vp[0] + (nx * 0.5 + 0.5) * vp[2],
        y: vp[1] + (ny * 0.5 + 0.5) * vp[3],
        z: vp[4] + nz * (vp[5] - vp[4]),
        iw,
        var: [0.0; MAX_VARYING_WORDS],
        raw: v.var,
    };
    for k in 0..MAX_VARYING_WORDS { s.var[k] = f32::from_bits(v.var[k]) * iw; }
    s
}

#[derive(Clone, Copy)]
struct Frag { x: i32, y: i32, z: f32, l: [f32; 3] }

const NO_FRAG: Frag = Frag { x: 0, y: 0, z: 0.0, l: [0.0; 3] };

fn shade_batch(p: &mut Pipe, t: &Target, s: &[&SVtx; 3], front: bool, fs_in: &[Slot], out_slot: Option<Slot>, frags: &[Frag]) {
    let w = (s[0].iw, s[1].iw, s[2].iw);
    for (lane, fr) in frags.iter().enumerate() {
        let l = fr.l;
        let iw = l[0] * w.0 + l[1] * w.1 + l[2] * w.2;
        let inv = 1.0 / iw;
        for slot in fs_in.iter() {
            if slot.builtin == 15 {
                p.fsm.set_input(slot, lane, &[(fr.x as f32 + 0.5).to_bits(), (fr.y as f32 + 0.5).to_bits(), fr.z.to_bits(), iw.to_bits()]);
            } else if slot.builtin == 17 {
                p.fsm.set_input(slot, lane, &[front as u32]);
            } else if slot.builtin == u32::MAX {
                let at = slot.location as usize * 4;
                let mut vals = [0u32; 16];
                let n = (slot.words as usize).min(16);
                for k in 0..n {
                    vals[k] = if slot.flat { s[0].raw[at + k] } else {
                        ((l[0] * s[0].var[at + k] + l[1] * s[1].var[at + k] + l[2] * s[2].var[at + k]) * inv).to_bits()
                    };
                }
                p.fsm.set_input(slot, lane, &vals[..n]);
            }
        }
    }
    if p.fsm.run(&p.fs, frags.len()).is_err() { return; }

    for (lane, fr) in frags.iter().enumerate() {
        let mut color = [0.0f32, 0.0, 0.0, 1.0];
        if let Some(sl) = out_slot {
            let mut o = [0u32; 4];
            p.fsm.output(&sl, lane, &mut o);
            for k in 0..4 { color[k] = f32::from_bits(o[k]); }
        }
        if p.depth_write && !t.depth.is_null() { unsafe { *t.depth.add((fr.y * t.depth_w + fr.x) as usize) = fr.z; } }

        let at = (fr.y * t.width + fr.x) as usize;
        let bl = &p.blend;
        let dst_px = unsafe { *t.color.add(at) };
        let mut out_c = color;
        if bl.enable {
            let dst = unpack(dst_px, t.swap_rb);
            for k in 0..3 {
                out_c[k] = blend_op(bl.color_op, color[k] * blend_factor(bl.src_color, color, dst, k), dst[k] * blend_factor(bl.dst_color, color, dst, k));
            }
            out_c[3] = blend_op(bl.alpha_op, color[3] * blend_factor(bl.src_alpha, color, dst, 3), dst[3] * blend_factor(bl.dst_alpha, color, dst, 3));
        }
        let mut packed = pack(out_c, t.swap_rb);
        if bl.write_mask != 0xF {
            let lanes: [(u32, u32); 4] = if t.swap_rb { [(0, 0), (8, 1), (16, 2), (24, 3)] } else { [(16, 0), (8, 1), (0, 2), (24, 3)] };
            for (shift, comp) in lanes {
                if bl.write_mask & (1 << comp) == 0 { packed = (packed & !(0xFF << shift)) | (dst_px & (0xFF << shift)); }
            }
        }
        unsafe { *t.color.add(at) = packed; }
    }
}

fn raster_triangle(p: &mut Pipe, t: &Target, d: &Draw, a: &Vtx, b: &Vtx, c: &Vtx) {
    let vp = &d.viewport;
    let (s0, mut s1, mut s2) = (to_screen(a, vp), to_screen(b, vp), to_screen(c, vp));
    let area = (s1.x - s0.x) * (s2.y - s0.y) - (s2.x - s0.x) * (s1.y - s0.y);
    if area == 0.0 || area != area { return; }
    // Vulkan: counter-clockwise (y down) means a negative value of this expression
    let front = if p.front_ccw { area < 0.0 } else { area > 0.0 };
    if (front && p.cull & 1 != 0) || (!front && p.cull & 2 != 0) { return; }
    if area < 0.0 { core::mem::swap(&mut s1, &mut s2); }
    let area = area.abs();

    let min_x = libm::floorf(s0.x.min(s1.x).min(s2.x)).max(d.scissor[0] as f32).max(0.0) as i32;
    let min_y = libm::floorf(s0.y.min(s1.y).min(s2.y)).max(d.scissor[1] as f32).max(0.0) as i32;
    let max_x = (libm::ceilf(s0.x.max(s1.x).max(s2.x)) as i32).min(d.scissor[0] + d.scissor[2]).min(t.width);
    let max_y = (libm::ceilf(s0.y.max(s1.y).max(s2.y)) as i32).min(d.scissor[1] + d.scissor[3]).min(t.height);
    if min_x >= max_x || min_y >= max_y { return; }

    // edge i is opposite vertex i; inclusion with the top-left rule
    let top_left = |a: &SVtx, b: &SVtx| -> bool {
        let (dx, dy) = (b.x - a.x, b.y - a.y);
        (dy == 0.0 && dx > 0.0) || dy < 0.0
    };
    let tl = [top_left(&s1, &s2), top_left(&s2, &s0), top_left(&s0, &s1)];
    let ep = [(s1.x, s1.y, s2.x, s2.y), (s2.x, s2.y, s0.x, s0.y), (s0.x, s0.y, s1.x, s1.y)];
    let inv_area = 1.0 / area;
    let (z0, z1, z2) = (s0.z, s1.z, s2.z);

    let fs_in: Vec<Slot> = p.fs.in_slots.clone();
    let out_slot = p.fs.out_slots.iter().find(|s| s.builtin == u32::MAX && s.location == 0).copied();
    let cap = p.fsm.lanes().min(64);
    let mut frags = [NO_FRAG; 64];
    let mut count = 0usize;
    let verts = [&s0, &s1, &s2];

    for y in min_y..max_y {
        let py = y as f32 + 0.5;
        for x in min_x..max_x {
            let px = x as f32 + 0.5;
            let mut e = [0.0f32; 3];
            let mut inside = true;
            for k in 0..3 {
                let (ax, ay, bx, by) = ep[k];
                e[k] = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
                if e[k] < 0.0 || (e[k] == 0.0 && !tl[k]) { inside = false; break; }
            }
            if !inside { continue; }
            let l = [e[0] * inv_area, e[1] * inv_area, e[2] * inv_area];
            let z = l[0] * z0 + l[1] * z1 + l[2] * z2;
            if z < 0.0 || z > 1.0 { continue; }
            if p.depth_test && !t.depth.is_null() {
                let old = unsafe { *t.depth.add((y * t.depth_w + x) as usize) };
                if !compare(p.depth_op, z, old) { continue; }
            }
            frags[count] = Frag { x, y, z, l };
            count += 1;
            if count == cap {
                shade_batch(p, t, &verts, front, &fs_in, out_slot, &frags[..count]);
                count = 0;
            }
        }
    }
    if count > 0 { shade_batch(p, t, &verts, front, &fs_in, out_slot, &frags[..count]); }
}
