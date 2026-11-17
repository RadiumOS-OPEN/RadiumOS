// SPIR-V reader and interpreter: runs one shader invocation (a vertex or a fragment) at a time.
//
// Supported: 32-bit scalars, vectors, matrices, arrays and structs; Input / Output / Private / Function memory, uniform
// and storage buffers and push constants (std140/std430 layouts from the Offset, ArrayStride and MatrixStride
// decorations); structured control flow (branches, loops, switch, phi, function calls); the GLSL.std.450 functions
// that shaders commonly use. Not supported: 64-bit and 16-bit types, images and samplers, atomics, derivatives.

use alloc::{vec, vec::Vec};

pub const STAGE_VERTEX: u32 = 0;
pub const STAGE_FRAGMENT: u32 = 4;

// pointer encoding: class in bits 28..31, binding slot in bits 24..27, offset in bits 0..23
// (words for Input/Output/Private/Function memory, bytes for buffers and push constants)
const P_IN: u32 = 1;
const P_OUT: u32 = 2;
const P_PRIV: u32 = 3;
const P_FN: u32 = 4;
const P_EXT: u32 = 5;
const P_PUSH: u32 = 6;
const NO_REG: u32 = u32::MAX;
const INSTRUCTION_LIMIT: u32 = 4_000_000;

const BUILTIN_POSITION: u32 = 0;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Kind { Void, Bool, Int, Float, Vector, Matrix, Array, Struct, Pointer, Other }

#[derive(Clone)]
struct Ty {
    kind: Kind,
    a: u32,           // element / column / pointee type id
    n: u32,           // component, column or element count; storage class for pointers
    members: Vec<u32>,
    size: u32,        // words when packed
}

#[derive(Clone, Copy, Default)]
struct Deco {
    location: Option<u32>,
    builtin: Option<u32>,
    binding: u32,
    set: u32,
    flat: bool,
    array_stride: u32,
}

#[derive(Clone, Copy)]
struct Inst { op: u16, n: u16, i: u32 }

/// One shader input or output: a location (or builtin) and where its words live in the Input/Output memory.
#[derive(Clone, Copy)]
pub struct Slot {
    pub location: u32,
    pub builtin: u32,   // u32::MAX when it is a location
    pub offset: u32,    // words into the Input or Output memory
    pub words: u32,
    pub flat: bool,
}

struct Var { class: u32, base: u32, init: u32 }

pub struct BindingUse { pub set: u32, pub binding: u32, pub slot: u32 }

pub struct Module {
    w: Vec<u32>,
    insts: Vec<Inst>,
    ty: Vec<Option<Ty>>,
    id_ty: Vec<u32>,
    reg: Vec<u32>,
    nregs: usize,
    consts: Vec<(u32, Vec<u32>)>,
    const_words: Vec<Option<Vec<u32>>>,
    deco: Vec<Deco>,
    member_deco: Vec<(u32, u32, u32, u32)>,   // struct id, member, decoration, value
    labels: Vec<u32>,
    func_entry: Vec<u32>,
    func_params: Vec<Vec<u32>>,
    entries: Vec<(u32, u32, Vec<u8>)>,        // execution model, function id, name
    vars: Vec<Var>,
    glsl: u32,
    mem_words: [u32; 5],
    pub in_slots: Vec<Slot>,
    pub out_slots: Vec<Slot>,
    pub bindings: Vec<BindingUse>,
    pub uses_push: bool,
    entry_func: u32,
    pub stage: u32,
    /// No data-dependent control flow: all invocations can run in lock step.
    pub straight: bool,
    /// Pointer ids whose value is the same in every invocation (variables and constant-index access chains of them).
    ptr_uniform: Vec<bool>,
}

fn lit_string(words: &[u32]) -> Vec<u8> {
    let mut out = Vec::new();
    'outer: for w in words {
        for b in w.to_le_bytes() {
            if b == 0 { break 'outer; }
            out.push(b);
        }
    }
    out
}

fn has_type_and_result(op: u16) -> bool {
    matches!(op, 12 | 41..=46 | 48..=52 | 55 | 57 | 59 | 61 | 65 | 66 | 79..=84 | 109..=200 | 207..=209 | 245)
}

impl Module {
    fn size_of(&self, ty: u32) -> u32 {
        self.ty.get(ty as usize).and_then(|t| t.as_ref()).map_or(0, |t| t.size)
    }

    fn t(&self, ty: u32) -> &Ty {
        self.ty[ty as usize].as_ref().unwrap()
    }

    fn member_off_words(&self, st: u32, member: u32) -> u32 {
        let t = self.t(st);
        t.members[..member as usize].iter().map(|m| self.size_of(*m)).sum()
    }

    fn member_value(&self, st: u32, member: u32, what: u32) -> Option<u32> {
        self.member_deco.iter().find(|d| d.0 == st && d.1 == member && d.2 == what).map(|d| d.3)
    }

    pub fn parse(w: &[u32]) -> Result<Module, &'static str> {
        if w.len() < 6 || w[0] != 0x0723_0203 { return Err("not SPIR-V"); }
        let bound = w[3] as usize;
        if bound == 0 || bound > 1 << 20 { return Err("bad id bound"); }
        let mut m = Module {
            w: w.to_vec(), insts: Vec::new(), ty: vec![None; bound], id_ty: vec![0; bound], reg: vec![NO_REG; bound], nregs: 0,
            consts: Vec::new(), const_words: vec![None; bound], deco: vec![Deco::default(); bound], member_deco: Vec::new(),
            labels: vec![0; bound], func_entry: vec![0; bound], func_params: vec![Vec::new(); bound], entries: Vec::new(),
            vars: Vec::new(), glsl: 0, mem_words: [0; 5], in_slots: Vec::new(), out_slots: Vec::new(), bindings: Vec::new(),
            uses_push: false, entry_func: 0, stage: 0, straight: true, ptr_uniform: vec![false; bound],
        };

        let mut i = 5usize;
        while i < w.len() {
            let n = (w[i] >> 16) as usize;
            if n == 0 || i + n > w.len() { return Err("truncated instruction"); }
            m.insts.push(Inst { op: (w[i] & 0xFFFF) as u16, n: n as u16, i: i as u32 });
            i += n;
        }

        m.straight = !m.insts.iter().any(|x| matches!(x.op, 245 | 246 | 250 | 251 | 252));
        let mut cur_func = 0u32;
        let mut in_function = false;
        let mut want_entry = false;
        for idx in 0..m.insts.len() {
            let Inst { op, n, i } = m.insts[idx];
            let (i, n) = (i as usize, n as usize);
            let ops = &w[i + 1..i + n];
            let id_ok = |id: u32| (id as usize) < bound;
            match op {
                11 => { if id_ok(ops[0]) && lit_string(&ops[1..]) == b"GLSL.std.450" { m.glsl = ops[0]; } }
                15 => { m.entries.push((ops[0], ops[1], lit_string(&ops[2..]))); }
                71 if n >= 3 && id_ok(ops[0]) => {
                    let d = &mut m.deco[ops[0] as usize];
                    let v = ops.get(2).copied().unwrap_or(0);
                    match ops[1] {
                        6 => d.array_stride = v,
                        11 => d.builtin = Some(v),
                        14 => d.flat = true,
                        30 => d.location = Some(v),
                        33 => d.binding = v,
                        34 => d.set = v,
                        _ => {}
                    }
                }
                72 if n >= 4 => { m.member_deco.push((ops[0], ops[1], ops[2], ops.get(3).copied().unwrap_or(0))); }
                19 | 20 | 21 | 22 | 25 | 26 | 27 | 23 | 24 | 28 | 29 | 30 | 32 | 33 => {
                    let id = ops[0] as usize;
                    if id >= bound { return Err("bad type id"); }
                    let t = match op {
                        19 => Ty { kind: Kind::Void, a: 0, n: 0, members: Vec::new(), size: 0 },
                        20 => Ty { kind: Kind::Bool, a: 0, n: 0, members: Vec::new(), size: 1 },
                        21 => { if ops[1] != 32 { return Err("only 32-bit integers are supported"); }
                                Ty { kind: Kind::Int, a: 0, n: ops[2], members: Vec::new(), size: 1 } }
                        22 => { if ops[1] != 32 { return Err("only 32-bit floats are supported"); }
                                Ty { kind: Kind::Float, a: 0, n: 0, members: Vec::new(), size: 1 } }
                        23 => Ty { kind: Kind::Vector, a: ops[1], n: ops[2], members: Vec::new(), size: ops[2] * m.size_of(ops[1]) },
                        24 => Ty { kind: Kind::Matrix, a: ops[1], n: ops[2], members: Vec::new(), size: ops[2] * m.size_of(ops[1]) },
                        28 => {
                            let len = m.const_words.get(ops[2] as usize).and_then(|c| c.as_ref()).and_then(|c| c.first()).copied().unwrap_or(0);
                            Ty { kind: Kind::Array, a: ops[1], n: len, members: Vec::new(), size: len * m.size_of(ops[1]) }
                        }
                        29 => Ty { kind: Kind::Array, a: ops[1], n: 0, members: Vec::new(), size: 0 },
                        30 => {
                            let members: Vec<u32> = ops[1..].to_vec();
                            let size = members.iter().map(|x| m.size_of(*x)).sum();
                            Ty { kind: Kind::Struct, a: 0, n: members.len() as u32, members, size }
                        }
                        32 => Ty { kind: Kind::Pointer, a: ops[2], n: ops[1], members: Vec::new(), size: 1 },
                        _ => Ty { kind: Kind::Other, a: 0, n: 0, members: Vec::new(), size: 1 },
                    };
                    m.ty[id] = Some(t);
                }
                41 | 42 | 43 | 44 | 46 | 48 | 49 | 50 | 51 => {
                    let (tid, id) = (ops[0], ops[1] as usize);
                    if id >= bound || m.ty.get(tid as usize).map_or(true, |t| t.is_none()) { return Err("bad constant"); }
                    let size = m.size_of(tid) as usize;
                    let mut vals = vec![0u32; size];
                    match op {
                        41 | 48 => vals[0] = 1,
                        43 | 50 => { if ops.len() > 3 { return Err("64-bit constants are not supported"); } vals[0] = ops[2]; }
                        44 | 51 => {
                            let mut at = 0usize;
                            for c in &ops[2..] {
                                let src = m.const_words.get(*c as usize).and_then(|x| x.as_ref()).ok_or("constituent is not constant")?;
                                vals[at..at + src.len()].copy_from_slice(src);
                                at += src.len();
                            }
                        }
                        _ => {}
                    }
                    m.id_ty[id] = tid;
                    m.reg[id] = m.nregs as u32;
                    m.nregs += size;
                    m.consts.push((m.reg[id], vals.clone()));
                    m.const_words[id] = Some(vals);
                }
                54 => { cur_func = ops[1]; in_function = true; want_entry = true; m.id_ty[ops[1] as usize] = ops[0]; }
                55 => {
                    let (tid, id) = (ops[0], ops[1] as usize);
                    m.id_ty[id] = tid;
                    m.reg[id] = m.nregs as u32;
                    m.nregs += m.size_of(tid) as usize;
                    m.func_params[cur_func as usize].push(id as u32);
                }
                56 => { in_function = false; }
                248 => {
                    m.labels[ops[0] as usize] = idx as u32;
                    if in_function && want_entry { m.func_entry[cur_func as usize] = idx as u32; want_entry = false; }
                }
                59 => {
                    let (tid, id, class) = (ops[0], ops[1] as usize, ops[2]);
                    m.id_ty[id] = tid;
                    m.reg[id] = m.nregs as u32;
                    m.nregs += 1;
                    let pointee = m.t(tid).a;
                    let words = m.size_of(pointee);
                    let init = ops.get(3).copied().unwrap_or(0);
                    let enc;
                    match class {
                        1 | 3 | 6 | 7 => {
                            let (code, slot) = match class { 1 => (P_IN, 0), 3 => (P_OUT, 1), 6 => (P_PRIV, 2), _ => (P_FN, 3) };
                            let base = m.mem_words[slot];
                            m.mem_words[slot] += words;
                            enc = (code << 28) | base;
                            m.vars.push(Var { class: code, base, init });
                            if class == 1 || class == 3 { m.collect_slots(id as u32, pointee, class == 1, base)?; }
                        }
                        2 | 12 => {
                            let d = m.deco[id];
                            let slot = m.bindings.len() as u32;
                            if slot > 15 { return Err("too many buffer bindings"); }
                            m.bindings.push(BindingUse { set: d.set, binding: d.binding, slot });
                            enc = (P_EXT << 28) | (slot << 24);
                        }
                        9 => { m.uses_push = true; enc = P_PUSH << 28; }
                        _ => { enc = 0; }
                    }
                    m.consts.push((m.reg[id], vec![enc]));
                    m.ptr_uniform[id] = true;
                }
                _ => {
                    if (op == 65 || op == 66) && n >= 5 && (ops[1] as usize) < bound && (ops[2] as usize) < bound {
                        m.ptr_uniform[ops[1] as usize] = m.ptr_uniform[ops[2] as usize]
                            && ops[3..].iter().all(|ix| (*ix as usize) < bound && m.const_words[*ix as usize].is_some());
                    }
                    if has_type_and_result(op) && n >= 3 {
                        let (tid, id) = (ops[0], ops[1] as usize);
                        if id >= bound { return Err("bad result id"); }
                        if m.ty.get(tid as usize).map_or(true, |t| t.is_none()) { return Err("bad result type"); }
                        m.id_ty[id] = tid;
                        m.reg[id] = m.nregs as u32;
                        m.nregs += m.size_of(tid).max(1) as usize;
                    }
                }
            }
        }
        // the pending OpFunction ids' types were noted above; calls need the return type from the function type
        Ok(m)
    }

    fn collect_slots(&mut self, var: u32, pointee: u32, input: bool, base: u32) -> Result<(), &'static str> {
        let d = self.deco[var as usize];
        let mut found: Vec<Slot> = Vec::new();
        if let Some(b) = d.builtin {
            found.push(Slot { location: 0, builtin: b, offset: base, words: self.size_of(pointee), flat: d.flat });
        } else if let Some(l) = d.location {
            found.push(Slot { location: l, builtin: u32::MAX, offset: base, words: self.size_of(pointee), flat: d.flat });
        } else if self.ty[pointee as usize].as_ref().map_or(false, |t| t.kind == Kind::Struct) {
            let count = self.t(pointee).n;
            for k in 0..count {
                let off = base + self.member_off_words(pointee, k);
                let words = self.size_of(self.t(pointee).members[k as usize]);
                if let Some(b) = self.member_value(pointee, k, 11) {
                    found.push(Slot { location: 0, builtin: b, offset: off, words, flat: false });
                } else if let Some(l) = self.member_value(pointee, k, 30) {
                    found.push(Slot { location: l, builtin: u32::MAX, offset: off, words, flat: self.member_value(pointee, k, 14).is_some() });
                }
            }
        }
        for s in found {
            if s.builtin == u32::MAX && (s.location as usize * 4 + s.words as usize) > super::MAX_VARYING_WORDS { return Err("shader interface is too large"); }
            if input { self.in_slots.push(s); } else { self.out_slots.push(s); }
        }
        Ok(())
    }

    /// Picks the entry point `name` and fixes the pipeline stage it belongs to.
    pub fn select_entry(&mut self, name: &[u8], model: u32) -> bool {
        for e in &self.entries {
            if e.2 == name && e.0 == model {
                self.entry_func = e.1;
                self.stage = model;
                return true;
            }
        }
        false
    }

    pub fn output_builtin(&self, builtin: u32) -> Option<Slot> {
        self.out_slots.iter().find(|s| s.builtin == builtin).copied()
    }

    pub fn has_position(&self) -> bool { self.output_builtin(BUILTIN_POSITION).is_some() }
}

#[derive(Debug, PartialEq, Eq)]
pub enum Fault { Killed, Limit, Bad }

/// Executes a shader for up to `lanes` invocations at once: every register word is stored as `lanes` consecutive values
/// (one per invocation). Shaders without data-dependent control flow (`Module::straight`) run all lanes together, which
/// amortizes the instruction dispatch; any other shader runs one invocation at a time (lane 0).
pub struct Machine {
    regs: Vec<u32>,
    mem: [Vec<u32>; 5],      // index 1 Input, 2 Output, 3 Private, 4 Function (0 unused); same lane layout as regs
    l: usize,
    pub ext: Vec<(*const u8, usize)>,
    pub push: (*const u8, usize),
    ptr_stride: Vec<u32>,
    stack: Vec<(u32, u32)>,   // return instruction, result id
}

#[inline(always)]
fn f(x: u32) -> f32 { f32::from_bits(x) }
#[inline(always)]
fn b(x: f32) -> u32 { x.to_bits() }

// The lane loops use raw pointers: register numbers and lane counts come from a module that parse() has validated.
macro_rules! ew1 {
    ($s:ident, $d:expr, $a:expr, $sz:expr, $n:expr, $fun:expr) => {{
        let l = $s.l;
        assert!(($d + $sz) * l <= $s.regs.len() && ($a + $sz) * l <= $s.regs.len() && $n <= l);
        let rp = $s.regs.as_mut_ptr();
        for k in 0..$sz {
            let (di, ai) = (($d + k) * l, ($a + k) * l);
            for lane in 0..$n { unsafe { let x = *rp.add(ai + lane); *rp.add(di + lane) = $fun(x); } }
        }
    }};
}

macro_rules! ew2 {
    ($s:ident, $d:expr, $a:expr, $bb:expr, $sz:expr, $n:expr, $fun:expr) => {{
        let l = $s.l;
        assert!(($d + $sz) * l <= $s.regs.len() && ($a + $sz) * l <= $s.regs.len() && ($bb + $sz) * l <= $s.regs.len() && $n <= l);
        let rp = $s.regs.as_mut_ptr();
        for k in 0..$sz {
            let (di, ai, bi) = (($d + k) * l, ($a + k) * l, ($bb + k) * l);
            for lane in 0..$n { unsafe { let x = *rp.add(ai + lane); let y = *rp.add(bi + lane); *rp.add(di + lane) = $fun(x, y); } }
        }
    }};
}

macro_rules! ew3 {
    ($s:ident, $d:expr, $a:expr, $bb:expr, $c:expr, $sz:expr, $n:expr, $fun:expr) => {{
        let l = $s.l;
        for k in 0..$sz {
            let (di, ai, bi, ci) = (($d + k) * l, ($a + k) * l, ($bb + k) * l, ($c + k) * l);
            for lane in 0..$n {
                let x = $s.regs[ai + lane]; let y = $s.regs[bi + lane]; let z = $s.regs[ci + lane];
                $s.regs[di + lane] = $fun(x, y, z);
            }
        }
    }};
}

impl Machine {
    pub fn new(m: &Module, lanes: usize) -> Machine {
        let l = lanes.max(1);
        let mut regs = vec![0u32; (m.nregs + 4) * l];
        for (r, vals) in &m.consts {
            for (k, v) in vals.iter().enumerate() {
                for lane in 0..l { regs[(*r as usize + k) * l + lane] = *v; }
            }
        }
        Machine {
            regs,
            mem: [Vec::new(), vec![0; m.mem_words[0] as usize * l], vec![0; m.mem_words[1] as usize * l], vec![0; m.mem_words[2] as usize * l], vec![0; m.mem_words[3] as usize * l]],
            l,
            ext: vec![(core::ptr::null(), 0); m.bindings.len()],
            push: (core::ptr::null(), 0),
            ptr_stride: vec![16; m.reg.len()],
            stack: Vec::new(),
        }
    }

    pub fn lanes(&self) -> usize { self.l }

    #[inline]
    pub fn set_input(&mut self, slot: &Slot, lane: usize, vals: &[u32]) {
        let n = (slot.words as usize).min(vals.len());
        for k in 0..n { self.mem[1][(slot.offset as usize + k) * self.l + lane] = vals[k]; }
    }

    #[inline]
    pub fn output(&self, slot: &Slot, lane: usize, out: &mut [u32]) {
        let n = (slot.words as usize).min(out.len());
        for k in 0..n { out[k] = self.mem[2][(slot.offset as usize + k) * self.l + lane]; }
    }

    #[inline]
    fn r(&self, m: &Module, id: u32) -> usize { m.reg[id as usize] as usize }

    fn read_block(mem: (*const u8, usize), off: usize) -> u32 {
        if off + 4 > mem.1 || mem.0.is_null() { return 0; }
        unsafe { core::ptr::read_unaligned(mem.0.add(off) as *const u32) }
    }

    fn load_block(&self, m: &Module, mem: (*const u8, usize), ty: u32, off: usize, mstride: u32, out: &mut [u32]) {
        let t = m.t(ty);
        match t.kind {
            Kind::Struct => {
                let mut at = 0usize;
                for k in 0..t.n {
                    let member = t.members[k as usize];
                    let size = m.size_of(member) as usize;
                    let moff = m.member_value(ty, k, 35).unwrap_or(0) as usize;
                    let ms = m.member_value(ty, k, 7).unwrap_or(mstride);
                    self.load_block(m, mem, member, off + moff, ms, &mut out[at..at + size]);
                    at += size;
                }
            }
            Kind::Array => {
                let stride = m.deco[ty as usize].array_stride as usize;
                let size = m.size_of(t.a) as usize;
                for e in 0..t.n as usize {
                    self.load_block(m, mem, t.a, off + e * stride, mstride, &mut out[e * size..(e + 1) * size]);
                }
            }
            Kind::Matrix => {
                let size = m.size_of(t.a) as usize;
                for c in 0..t.n as usize {
                    self.load_block(m, mem, t.a, off + c * mstride as usize, mstride, &mut out[c * size..(c + 1) * size]);
                }
            }
            Kind::Vector => {
                for c in 0..t.n as usize { out[c] = Self::read_block(mem, off + c * 4); }
            }
            _ => { if !out.is_empty() { out[0] = Self::read_block(mem, off); } }
        }
    }

    fn block_mem(&self, p: u32) -> (*const u8, usize) {
        if p >> 28 == P_PUSH { self.push } else { self.ext.get(((p >> 24) & 0xF) as usize).copied().unwrap_or((core::ptr::null(), 0)) }
    }

    /// Runs the entry point for the first `n` lanes (n must be 1 unless the module is straight-line).
    pub fn run(&mut self, m: &Module, n: usize) -> Result<(), Fault> {
        let l = self.l;
        if n == 0 || n > l || (n > 1 && !m.straight) { return Err(Fault::Bad); }
        self.mem[2].iter_mut().for_each(|x| *x = 0);
        self.mem[3].iter_mut().for_each(|x| *x = 0);
        self.mem[4].iter_mut().for_each(|x| *x = 0);
        for v in &m.vars {
            if v.init != 0 && v.class != P_IN && v.class != P_OUT {
                if let Some(c) = m.const_words[v.init as usize].as_ref() {
                    let cls = v.class as usize;
                    for (k, val) in c.iter().enumerate() { for lane in 0..l { self.mem[cls][(v.base as usize + k) * l + lane] = *val; } }
                }
            }
        }
        self.stack.clear();

        let w = &m.w;
        let mut pc = m.func_entry[m.entry_func as usize] as usize;
        let mut cur_label = 0u32;
        let mut prev_label = 0u32;
        let mut budget = INSTRUCTION_LIMIT;

        loop {
            if budget == 0 { return Err(Fault::Limit); }
            budget -= 1;
            let inst = m.insts[pc];
            let i = inst.i as usize;
            let cnt = inst.n as usize;
            match inst.op {
                248 => { cur_label = w[i + 1]; pc += 1; }
                249 => { prev_label = cur_label; pc = m.labels[w[i + 1] as usize] as usize; }
                250 => {
                    let c = self.regs[self.r(m, w[i + 1]) * l];
                    prev_label = cur_label;
                    pc = m.labels[(if c != 0 { w[i + 2] } else { w[i + 3] }) as usize] as usize;
                }
                251 => {
                    let sel = self.regs[self.r(m, w[i + 1]) * l];
                    let mut target = w[i + 2];
                    let mut k = 3;
                    while k + 1 < cnt {
                        if w[i + k] == sel { target = w[i + k + 1]; break; }
                        k += 2;
                    }
                    prev_label = cur_label;
                    pc = m.labels[target as usize] as usize;
                }
                252 => return Err(Fault::Killed),
                253 | 254 => {
                    let Some((ret, res)) = self.stack.pop() else { return Ok(()); };
                    if inst.op == 254 && res != 0 {
                        let src = self.r(m, w[i + 1]);
                        let dst = self.r(m, res);
                        let size = m.size_of(m.id_ty[res as usize]) as usize;
                        for k in 0..size { for lane in 0..n { self.regs[(dst + k) * l + lane] = self.regs[(src + k) * l + lane]; } }
                    }
                    pc = ret as usize;
                }
                255 => return Err(Fault::Bad),
                245 => {
                    // all phis of the block at once: they read values from before the block (single-invocation mode)
                    let mut end = pc;
                    let mut staged: Vec<(usize, [u32; 16], usize)> = Vec::new();
                    while m.insts[end].op == 245 {
                        let pi = m.insts[end].i as usize;
                        let pn = m.insts[end].n as usize;
                        let res = w[pi + 2];
                        let size = m.size_of(w[pi + 1]) as usize;
                        if size > 16 { return Err(Fault::Bad); }
                        let mut val = [0u32; 16];
                        let mut k = 3;
                        while k + 1 < pn {
                            if w[pi + k + 1] == prev_label {
                                let src = self.r(m, w[pi + k]);
                                for e in 0..size { val[e] = self.regs[(src + e) * l]; }
                                break;
                            }
                            k += 2;
                        }
                        staged.push((self.r(m, res), val, size));
                        end += 1;
                    }
                    for (dst, val, size) in staged { for e in 0..size { self.regs[(dst + e) * l] = val[e]; } }
                    pc = end;
                }
                57 => {
                    let res = w[i + 2];
                    let callee = w[i + 3];
                    let params = &m.func_params[callee as usize];
                    for (k, p) in params.iter().enumerate() {
                        let src = self.r(m, w[i + 4 + k]);
                        let dst = self.r(m, *p);
                        let size = m.size_of(m.id_ty[*p as usize]) as usize;
                        for e in 0..size { for lane in 0..n { self.regs[(dst + e) * l + lane] = self.regs[(src + e) * l + lane]; } }
                    }
                    if self.stack.len() > 64 { return Err(Fault::Bad); }
                    self.stack.push(((pc + 1) as u32, if m.size_of(w[i + 1]) > 0 { res } else { 0 }));
                    pc = m.func_entry[callee as usize] as usize;
                }
                _ => { self.exec_simple(m, inst, n)?; pc += 1; }
            }
        }
    }

    fn exec_simple(&mut self, m: &Module, inst: Inst, n: usize) -> Result<(), Fault> {
        let w = &m.w;
        let i = inst.i as usize;
        let cnt = inst.n as usize;
        let op = inst.op;
        let l = self.l;
        let res_ty = w[i + 1];
        let res = if cnt > 2 { w[i + 2] } else { 0 };
        macro_rules! dst { () => { self.r(m, res) }; }
        macro_rules! src { ($k:expr) => { self.r(m, w[i + $k]) }; }
        let size = if m.ty.get(res_ty as usize).map_or(false, |t| t.is_some()) { m.size_of(res_ty) as usize } else { 0 };

        match op {
            0 | 246 | 247 | 56 | 54 | 55 | 59 => {}
            12 => return self.ext_inst(m, inst, n),
            83 => { let (d, s) = (dst!(), src!(3)); ew1!(self, d, s, size, n, |x: u32| x); }
            61 => {
                let pr = src!(3);
                let d = dst!();
                let ms = self.ptr_stride[w[i + 3] as usize];
                let mut tmp = [0u32; 64];
                if size > 64 { return Err(Fault::Bad); }
                let p0 = self.regs[pr * l];
                let uniform = n > 1 && m.ptr_uniform[w[i + 3] as usize];
                if uniform && !(p0 >> 28 == P_EXT || p0 >> 28 == P_PUSH) {
                    // the same variable in every lane: its lanes are consecutive in memory
                    let class = (p0 >> 28) as usize;
                    let off = (p0 & 0xFF_FFFF) as usize;
                    if class == 0 || class > 4 || (off + size) * l > self.mem[class].len() { return Err(Fault::Bad); }
                    for k in 0..size {
                        let (from, to) = ((off + k) * l, (d + k) * l);
                        self.regs[to..to + n].copy_from_slice(&self.mem[class][from..from + n]);
                    }
                } else if uniform {
                    // the same buffer address in every lane (push constants, uniform buffers): read it once
                    let mem = self.block_mem(p0);
                    self.load_block(m, mem, res_ty, (p0 & 0xFF_FFFF) as usize, ms, &mut tmp[..size]);
                    for k in 0..size { for lane in 0..n { self.regs[(d + k) * l + lane] = tmp[k]; } }
                } else {
                for lane in 0..n {
                    let p = self.regs[pr * l + lane];
                    let (class, off) = (p >> 28, (p & 0xFF_FFFF) as usize);
                    match class {
                        P_IN | P_OUT | P_PRIV | P_FN => {
                            let mem = &self.mem[class as usize];
                            if (off + size) * l > mem.len() { return Err(Fault::Bad); }
                            for k in 0..size { self.regs[(d + k) * l + lane] = mem[(off + k) * l + lane]; }
                        }
                        P_EXT | P_PUSH => {
                            let mem = self.block_mem(p);
                            self.load_block(m, mem, res_ty, off, ms, &mut tmp[..size]);
                            for k in 0..size { self.regs[(d + k) * l + lane] = tmp[k]; }
                        }
                        _ => return Err(Fault::Bad),
                    }
                }
                }
            }
            62 => {
                let pr = src!(1);
                let s = src!(2);
                let size = m.size_of(m.id_ty[w[i + 2] as usize]) as usize;
                let p0 = self.regs[pr * l];
                if n > 1 && m.ptr_uniform[w[i + 1] as usize] && matches!(p0 >> 28, P_OUT | P_PRIV | P_FN | P_IN) {
                    let class = (p0 >> 28) as usize;
                    let off = (p0 & 0xFF_FFFF) as usize;
                    if (off + size) * l > self.mem[class].len() { return Err(Fault::Bad); }
                    for k in 0..size {
                        let (from, to) = ((s + k) * l, (off + k) * l);
                        self.mem[class][to..to + n].copy_from_slice(&self.regs[from..from + n]);
                    }
                    return Ok(());
                }
                for lane in 0..n {
                    let p = self.regs[pr * l + lane];
                    let (class, off) = (p >> 28, (p & 0xFF_FFFF) as usize);
                    if matches!(class, P_OUT | P_PRIV | P_FN | P_IN) {
                        let mem = &mut self.mem[class as usize];
                        if (off + size) * l > mem.len() { return Err(Fault::Bad); }
                        for k in 0..size { mem[(off + k) * l + lane] = self.regs[(s + k) * l + lane]; }
                    }
                }
            }
            65 | 66 => {
                let base = w[i + 3];
                let pr = self.r(m, base);
                let d = dst!();
                let ty0 = m.t(m.id_ty[base as usize]).a;
                let mstride0 = self.ptr_stride[base as usize];
                let mut last_mstride = mstride0;
                let uniform = n > 1 && m.ptr_uniform[res as usize];
                let walk_lanes = if uniform { 1 } else { n };
                for lane in 0..walk_lanes {
                    let p = self.regs[pr * l + lane];
                    let class = p >> 28;
                    let block = class == P_EXT || class == P_PUSH;
                    let mut off = (p & 0xFF_FFFF) as usize;
                    let mut ty = ty0;
                    let mut mstride = mstride0;
                    for k in 4..cnt {
                        let idx = self.regs[self.r(m, w[i + k]) * l + lane] as usize;
                        let t = m.t(ty);
                        match t.kind {
                            Kind::Struct => {
                                if idx >= t.members.len() { return Err(Fault::Bad); }
                                if block {
                                    off += m.member_value(ty, idx as u32, 35).unwrap_or(0) as usize;
                                    mstride = m.member_value(ty, idx as u32, 7).unwrap_or(mstride);
                                } else {
                                    off += m.member_off_words(ty, idx as u32) as usize;
                                }
                                ty = t.members[idx];
                            }
                            Kind::Array => {
                                let stride = if block { m.deco[ty as usize].array_stride as usize } else { m.size_of(t.a) as usize };
                                off += idx * stride;
                                ty = t.a;
                            }
                            Kind::Vector => { off += idx * if block { 4 } else { 1 }; ty = t.a; }
                            Kind::Matrix => { off += idx * if block { mstride as usize } else { m.size_of(t.a) as usize }; ty = t.a; }
                            _ => return Err(Fault::Bad),
                        }
                    }
                    if off > 0xFF_FFFF { return Err(Fault::Bad); }
                    self.regs[d * l + lane] = (p & 0xFF00_0000) | off as u32;
                    last_mstride = mstride;
                }
                if uniform { let v = self.regs[d * l]; for lane in 1..n { self.regs[d * l + lane] = v; } }
                self.ptr_stride[res as usize] = last_mstride;
            }
            79 => {
                let (d, a, bb) = (dst!(), src!(3), src!(4));
                let na = m.size_of(m.id_ty[w[i + 3] as usize]) as usize;
                for k in 0..size {
                    let c = w[i + 5 + k];
                    for lane in 0..n {
                        self.regs[(d + k) * l + lane] = if c == 0xFFFF_FFFF { 0 } else if (c as usize) < na { self.regs[(a + c as usize) * l + lane] } else { self.regs[(bb + c as usize - na) * l + lane] };
                    }
                }
            }
            80 => {
                let d = dst!();
                let mut at = 0;
                for k in 3..cnt {
                    let s = src!(k);
                    let cn = m.size_of(m.id_ty[w[i + k] as usize]) as usize;
                    for e in 0..cn { for lane in 0..n { self.regs[(d + at + e) * l + lane] = self.regs[(s + e) * l + lane]; } }
                    at += cn;
                }
            }
            81 | 82 => {
                let (comp_id, first_idx) = if op == 81 { (w[i + 3], 4) } else { (w[i + 4], 5) };
                let mut ty = m.id_ty[comp_id as usize];
                let mut off = 0usize;
                for k in first_idx..cnt {
                    let idx = w[i + k] as usize;
                    let t = m.t(ty);
                    match t.kind {
                        Kind::Struct => { off += m.member_off_words(ty, idx as u32) as usize; ty = t.members[idx]; }
                        Kind::Array | Kind::Vector | Kind::Matrix => { off += idx * m.size_of(t.a) as usize; ty = t.a; }
                        _ => return Err(Fault::Bad),
                    }
                }
                let d = dst!();
                let c = self.r(m, comp_id);
                if op == 81 {
                    let len = m.size_of(ty) as usize;
                    for k in 0..len { for lane in 0..n { self.regs[(d + k) * l + lane] = self.regs[(c + off + k) * l + lane]; } }
                } else {
                    let obj = src!(3);
                    let olen = m.size_of(m.id_ty[w[i + 3] as usize]) as usize;
                    for k in 0..size { for lane in 0..n { self.regs[(d + k) * l + lane] = self.regs[(c + k) * l + lane]; } }
                    for k in 0..olen { for lane in 0..n { self.regs[(d + off + k) * l + lane] = self.regs[(obj + k) * l + lane]; } }
                }
            }
            84 => {
                let (d, s) = (dst!(), src!(3));
                let t = m.t(res_ty);
                let (rows_out, cols_out) = (m.size_of(t.a) as usize, t.n as usize);
                for c in 0..cols_out { for r in 0..rows_out { for lane in 0..n { self.regs[(d + c * rows_out + r) * l + lane] = self.regs[(s + r * cols_out + c) * l + lane]; } } }
            }
            109 => { let (d, s) = (dst!(), src!(3)); ew1!(self, d, s, size, n, |x: u32| { let v = f(x); if v <= 0.0 || v != v { 0 } else { v as u32 } }); }
            110 => { let (d, s) = (dst!(), src!(3)); ew1!(self, d, s, size, n, |x: u32| (f(x) as i32) as u32); }
            111 => { let (d, s) = (dst!(), src!(3)); ew1!(self, d, s, size, n, |x: u32| b((x as i32) as f32)); }
            112 => { let (d, s) = (dst!(), src!(3)); ew1!(self, d, s, size, n, |x: u32| b(x as f32)); }
            124 => { let (d, s) = (dst!(), src!(3)); ew1!(self, d, s, size, n, |x: u32| x); }
            126 => { let (d, s) = (dst!(), src!(3)); ew1!(self, d, s, size, n, |x: u32| (x as i32).wrapping_neg() as u32); }
            127 => { let (d, s) = (dst!(), src!(3)); ew1!(self, d, s, size, n, |x: u32| b(-f(x))); }
            200 => { let (d, s) = (dst!(), src!(3)); ew1!(self, d, s, size, n, |x: u32| !x); }
            168 => { let (d, s) = (dst!(), src!(3)); ew1!(self, d, s, size, n, |x: u32| (x == 0) as u32); }
            156 => { let (d, s) = (dst!(), src!(3)); ew1!(self, d, s, size, n, |x: u32| { let v = f(x); (v != v) as u32 }); }
            157 => { let (d, s) = (dst!(), src!(3)); ew1!(self, d, s, size, n, |x: u32| f(x).is_infinite() as u32); }
            128 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| x.wrapping_add(y)); }
            129 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| b(f(x) + f(y))); }
            130 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| x.wrapping_sub(y)); }
            131 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| b(f(x) - f(y))); }
            132 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| x.wrapping_mul(y)); }
            133 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| b(f(x) * f(y))); }
            134 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| if y == 0 { 0 } else { x / y }); }
            135 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| if y == 0 { 0 } else { (x as i32).wrapping_div(y as i32) as u32 }); }
            136 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| b(f(x) / f(y))); }
            137 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| if y == 0 { 0 } else { x % y }); }
            138 | 139 => {
                let (d, a, bb) = (dst!(), src!(3), src!(4));
                ew2!(self, d, a, bb, size, n, |x: u32, y: u32| -> u32 {
                    let (sx, sy) = (x as i32, y as i32);
                    if sy == 0 { 0 } else {
                        let r = sx.wrapping_rem(sy);
                        (if op == 139 && r != 0 && ((r < 0) != (sy < 0)) { r + sy } else { r }) as u32
                    }
                });
            }
            140 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| b(libm::fmodf(f(x), f(y)))); }
            141 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| b(f(x) - f(y) * libm::floorf(f(x) / f(y)))); }
            164 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| ((x != 0) == (y != 0)) as u32); }
            165 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| ((x != 0) != (y != 0)) as u32); }
            166 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| ((x != 0) || (y != 0)) as u32); }
            167 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| ((x != 0) && (y != 0)) as u32); }
            170 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| (x == y) as u32); }
            171 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| (x != y) as u32); }
            172 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| (x > y) as u32); }
            173 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| ((x as i32) > (y as i32)) as u32); }
            174 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| (x >= y) as u32); }
            175 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| ((x as i32) >= (y as i32)) as u32); }
            176 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| (x < y) as u32); }
            177 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| ((x as i32) < (y as i32)) as u32); }
            178 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| (x <= y) as u32); }
            179 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| ((x as i32) <= (y as i32)) as u32); }
            180 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| (f(x) == f(y)) as u32); }
            181 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| { let (p, q) = (f(x), f(y)); (p == q || p != p || q != q) as u32 }); }
            182 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| { let (p, q) = (f(x), f(y)); (p != q && p == p && q == q) as u32 }); }
            183 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| (f(x) != f(y)) as u32); }
            184 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| (f(x) < f(y)) as u32); }
            185 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| { let (p, q) = (f(x), f(y)); (p < q || p != p || q != q) as u32 }); }
            186 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| (f(x) > f(y)) as u32); }
            187 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| { let (p, q) = (f(x), f(y)); (p > q || p != p || q != q) as u32 }); }
            188 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| (f(x) <= f(y)) as u32); }
            189 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| { let (p, q) = (f(x), f(y)); (p <= q || p != p || q != q) as u32 }); }
            190 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| (f(x) >= f(y)) as u32); }
            191 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| { let (p, q) = (f(x), f(y)); (p >= q || p != p || q != q) as u32 }); }
            194 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| x.checked_shr(y).unwrap_or(0)); }
            195 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| (x as i32).checked_shr(y).unwrap_or(if (x as i32) < 0 { -1 } else { 0 }) as u32); }
            196 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| x.checked_shl(y).unwrap_or(0)); }
            197 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| x | y); }
            198 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| x ^ y); }
            199 => { let (d, a, bb) = (dst!(), src!(3), src!(4)); ew2!(self, d, a, bb, size, n, |x: u32, y: u32| x & y); }
            169 => {
                let (d, cond, x, y) = (dst!(), src!(3), src!(4), src!(5));
                let scalar_cond = m.size_of(m.id_ty[w[i + 3] as usize]) == 1;
                for k in 0..size {
                    let ci = (cond + if scalar_cond { 0 } else { k }) * l;
                    for lane in 0..n {
                        self.regs[(d + k) * l + lane] = if self.regs[ci + lane] != 0 { self.regs[(x + k) * l + lane] } else { self.regs[(y + k) * l + lane] };
                    }
                }
            }
            142 | 143 => {
                let (d, a, s) = (dst!(), src!(3), src!(4));
                for k in 0..size { for lane in 0..n { self.regs[(d + k) * l + lane] = b(f(self.regs[(a + k) * l + lane]) * f(self.regs[s * l + lane])); } }
            }
            148 => {
                let (d, a, bb) = (dst!(), src!(3), src!(4));
                let len = m.size_of(m.id_ty[w[i + 3] as usize]) as usize;
                for lane in 0..n {
                    let mut sum = 0.0f32;
                    for k in 0..len { sum += f(self.regs[(a + k) * l + lane]) * f(self.regs[(bb + k) * l + lane]); }
                    self.regs[d * l + lane] = b(sum);
                }
            }
            154 | 155 => {
                let (d, a) = (dst!(), src!(3));
                let len = m.size_of(m.id_ty[w[i + 3] as usize]) as usize;
                for lane in 0..n {
                    let (mut any, mut all) = (false, true);
                    for k in 0..len { if self.regs[(a + k) * l + lane] != 0 { any = true } else { all = false } }
                    self.regs[d * l + lane] = if op == 154 { any as u32 } else { all as u32 };
                }
            }
            144 => {
                let (d, a, bb) = (dst!(), src!(3), src!(4));
                let tb = m.t(m.id_ty[w[i + 4] as usize]);
                let (cols, rows) = (tb.n as usize, m.size_of(tb.a) as usize);
                for lane in 0..n {
                    for c in 0..cols {
                        let mut s = 0.0f32;
                        for r in 0..rows { s += f(self.regs[(a + r) * l + lane]) * f(self.regs[(bb + c * rows + r) * l + lane]); }
                        self.regs[(d + c) * l + lane] = b(s);
                    }
                }
            }
            145 => {
                let (d, a, bb) = (dst!(), src!(3), src!(4));
                let t = m.t(m.id_ty[w[i + 3] as usize]);
                let (cols, rows) = (t.n as usize, m.size_of(t.a) as usize);
                for lane in 0..n {
                    for r in 0..rows {
                        let mut s = 0.0f32;
                        for c in 0..cols { s += f(self.regs[(a + c * rows + r) * l + lane]) * f(self.regs[(bb + c) * l + lane]); }
                        self.regs[(d + r) * l + lane] = b(s);
                    }
                }
            }
            146 => {
                let (d, a, bb) = (dst!(), src!(3), src!(4));
                let ta_t = m.t(m.id_ty[w[i + 3] as usize]);
                let (a_cols, rows) = (ta_t.n as usize, m.size_of(ta_t.a) as usize);
                let b_cols = m.t(m.id_ty[w[i + 4] as usize]).n as usize;
                for lane in 0..n {
                    for j in 0..b_cols {
                        for r in 0..rows {
                            let mut s = 0.0f32;
                            for k in 0..a_cols { s += f(self.regs[(a + k * rows + r) * l + lane]) * f(self.regs[(bb + j * a_cols + k) * l + lane]); }
                            self.regs[(d + j * rows + r) * l + lane] = b(s);
                        }
                    }
                }
            }
            207..=209 => { let d = dst!(); for k in 0..size { for lane in 0..n { self.regs[(d + k) * l + lane] = 0; } } }
            _ => return Err(Fault::Bad),
        }
        Ok(())
    }

    fn ext_inst(&mut self, m: &Module, inst: Inst, n: usize) -> Result<(), Fault> {
        let w = &m.w;
        let i = inst.i as usize;
        let cnt = inst.n as usize;
        let l = self.l;
        if w[i + 3] != m.glsl { return Err(Fault::Bad); }
        let code = w[i + 4];
        let d = self.r(m, w[i + 2]);
        let size = m.size_of(w[i + 1]) as usize;
        let nargs = cnt - 5;
        let a = if nargs > 0 { self.r(m, w[i + 5]) } else { 0 };
        let bb = if nargs > 1 { self.r(m, w[i + 6]) } else { 0 };
        let c = if nargs > 2 { self.r(m, w[i + 7]) } else { 0 };
        match code {
            66 | 67 => {
                let len = m.size_of(m.id_ty[w[i + 5] as usize]) as usize;
                for lane in 0..n {
                    let mut s = 0.0f32;
                    for k in 0..len {
                        let v = if code == 67 { f(self.regs[(a + k) * l + lane]) - f(self.regs[(bb + k) * l + lane]) } else { f(self.regs[(a + k) * l + lane]) };
                        s += v * v;
                    }
                    self.regs[d * l + lane] = b(libm::sqrtf(s));
                }
            }
            68 => {
                for lane in 0..n {
                    let g = |r: usize, k: usize| f(self.regs[(r + k) * l + lane]);
                    let (x, y) = ([g(a, 0), g(a, 1), g(a, 2)], [g(bb, 0), g(bb, 1), g(bb, 2)]);
                    self.regs[d * l + lane] = b(x[1] * y[2] - x[2] * y[1]);
                    self.regs[(d + 1) * l + lane] = b(x[2] * y[0] - x[0] * y[2]);
                    self.regs[(d + 2) * l + lane] = b(x[0] * y[1] - x[1] * y[0]);
                }
            }
            69 => {
                for lane in 0..n {
                    let mut s = 0.0f32;
                    for k in 0..size { let v = f(self.regs[(a + k) * l + lane]); s += v * v; }
                    let inv = if s > 0.0 { 1.0 / libm::sqrtf(s) } else { 0.0 };
                    for k in 0..size { self.regs[(d + k) * l + lane] = b(f(self.regs[(a + k) * l + lane]) * inv); }
                }
            }
            71 => {
                for lane in 0..n {
                    let mut dot = 0.0f32;
                    for k in 0..size { dot += f(self.regs[(a + k) * l + lane]) * f(self.regs[(bb + k) * l + lane]); }
                    for k in 0..size { self.regs[(d + k) * l + lane] = b(f(self.regs[(a + k) * l + lane]) - 2.0 * dot * f(self.regs[(bb + k) * l + lane])); }
                }
            }
            _ => {
                // functions that take a scalar operand together with vectors (Step, Mix, ...) broadcast it
                let la = if nargs > 0 && m.size_of(m.id_ty[w[i + 5] as usize]) > 1 { l } else { 0 };
                let lb = if nargs > 1 && m.size_of(m.id_ty[w[i + 6] as usize]) > 1 { l } else { 0 };
                let lc = if nargs > 2 && m.size_of(m.id_ty[w[i + 7] as usize]) > 1 { l } else { 0 };
                for k in 0..size {
                    for lane in 0..n {
                        let x = self.regs[a * l + k * la + lane];
                        let y = if nargs > 1 { self.regs[bb * l + k * lb + lane] } else { 0 };
                        let z = if nargs > 2 { self.regs[c * l + k * lc + lane] } else { 0 };
                        let (fx, fy, fz) = (f(x), f(y), f(z));
                        let out: u32 = match code {
                            1 => b(libm::roundf(fx)),
                            2 => b(libm::rintf(fx)),
                            3 => b(libm::truncf(fx)),
                            4 => b(libm::fabsf(fx)),
                            5 => (x as i32).wrapping_abs() as u32,
                            6 => b(if fx > 0.0 { 1.0 } else if fx < 0.0 { -1.0 } else { 0.0 }),
                            7 => (x as i32).signum() as u32,
                            8 => b(libm::floorf(fx)),
                            9 => b(libm::ceilf(fx)),
                            10 => b(fx - libm::floorf(fx)),
                            11 => b(fx * 0.017453292),
                            12 => b(fx * 57.29578),
                            13 => b(libm::sinf(fx)),
                            14 => b(libm::cosf(fx)),
                            15 => b(libm::tanf(fx)),
                            16 => b(libm::asinf(fx)),
                            17 => b(libm::acosf(fx)),
                            18 => b(libm::atanf(fx)),
                            19 => b(libm::sinhf(fx)),
                            20 => b(libm::coshf(fx)),
                            21 => b(libm::tanhf(fx)),
                            25 => b(libm::atan2f(fx, fy)),
                            26 => b(libm::powf(fx, fy)),
                            27 => b(libm::expf(fx)),
                            28 => b(libm::logf(fx)),
                            29 => b(libm::exp2f(fx)),
                            30 => b(libm::log2f(fx)),
                            31 => b(libm::sqrtf(fx)),
                            32 => b(1.0 / libm::sqrtf(fx)),
                            37 => b(if fy < fx { fy } else { fx }),
                            38 => x.min(y),
                            39 => (x as i32).min(y as i32) as u32,
                            40 => b(if fy > fx { fy } else { fx }),
                            41 => x.max(y),
                            42 => (x as i32).max(y as i32) as u32,
                            43 => b(fx.max(fy).min(fz)),
                            44 => x.max(y).min(z),
                            45 => (x as i32).max(y as i32).min(z as i32) as u32,
                            46 => b(fx * (1.0 - fz) + fy * fz),
                            48 => b(if fy < fx { 0.0 } else { 1.0 }),
                            49 => { let t = ((fz - fx) / (fy - fx)).max(0.0).min(1.0); b(t * t * (3.0 - 2.0 * t)) }
                            50 => b(fx * fy + fz),
                            _ => return Err(Fault::Bad),
                        };
                        self.regs[(d + k) * l + lane] = out;
                    }
                }
            }
        }
        Ok(())
    }
}
