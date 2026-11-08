//! Guard tasks: background checks that keep the kernel alive and notice tampering.
//!
//! Like every task here they run inside the timer tick, one task per tick, so they only read memory and registers: no AVFS,
//! no VGA, no allocation. What they find goes into a ring of events (see `rust_guard_event`) that the `guard` command and the
//! desktop read from normal context.
//!
//!   guard-text   hashes the kernel code and read-only data block by block and reports a block that changes
//!   guard-cpu    CR0/CR4 protection bits, I/O privilege, the stack canary and the interrupt table (damaged gates are restored)
//!   guard-tasks  supervisor: reaps zombies, kills tasks that run code outside the kernel, trims task floods, respawns the
//!                essential tasks (including the other guards) and frees ones stuck asleep or blocked
//!   guard-mem    notices when physical memory runs low

use super::*;
use core::ptr::{addr_of, read_volatile};

extern "C" {
    static _text_start: u8;
    static _text_end: u8;
    static _rodata_end: u8;
    static __stack_chk_guard: u32;
    fn pmm_get_free_memory() -> u64;
    fn pmm_get_total_memory() -> u64;
}

// ---- events and statistics -----------------------------------------------------------------------------------------------------

pub const K_START: u32 = 15;
pub const K_TEXT: u32 = 1;
pub const K_CANARY: u32 = 2;
pub const K_IDT: u32 = 3;
pub const K_CR0: u32 = 4;
pub const K_CR4: u32 = 5;
pub const K_IOPL: u32 = 6;
pub const K_ROGUE: u32 = 7;
pub const K_RESPAWN: u32 = 8;
pub const K_COUNT: u32 = 9;
pub const K_LOWMEM: u32 = 10;
pub const K_REFUSED: u32 = 11;
pub const K_STUCK: u32 = 12;
pub const K_FLOOD: u32 = 13;
pub const K_SPAWN_FAIL: u32 = 14;

const EV_MAX: usize = 64;

#[derive(Copy, Clone)]
struct Event { tick: u32, kind: u32, a: u32, b: u32 }

static mut EVENTS: [Event; EV_MAX] = [Event { tick: 0, kind: 0, a: 0, b: 0 }; EV_MAX];
static mut EV_TOTAL: u32 = 0;
static mut LAST_OF_KIND: [u32; 16] = [0; 16];

// Statistics, as handed out by rust_guard_stats (the order is part of the interface with src/commands/guard.c).
const S_EVENTS: usize = 0;
const S_ALERTS: usize = 1;
const S_TEXT_CHECKED: usize = 2;
const S_TEXT_PASSES: usize = 3;
const S_TEXT_TOTAL: usize = 4;
const S_CPU_CHECKS: usize = 5;
const S_REPAIRS: usize = 6;
const S_RESPAWNS: usize = 7;
const S_REAPED: usize = 8;
const S_ROGUE: usize = 9;
const S_REFUSED: usize = 10;
const S_LOWMEM: usize = 11;
const S_GUARDS: usize = 12;
const S_FLOOD: usize = 13;
const S_TICKS: usize = 14;
const S_COUNT: usize = 16;
static mut STAT: [u32; S_COUNT] = [0; S_COUNT];

fn now() -> u32 { SYSTEM_TICKS.load(Ordering::Relaxed) }

/// Notes something; an alert is a possible attack or corruption, the rest is information. At most one event per kind and
/// second is kept in the ring (the counters still count every one).
unsafe fn record(kind: u32, a: u32, b: u32, alert: bool) {
    let t = now();
    if alert { STAT[S_ALERTS] = STAT[S_ALERTS].wrapping_add(1); }
    let k = (kind as usize) & 15;
    if LAST_OF_KIND[k] != 0 && t.wrapping_sub(LAST_OF_KIND[k]) < 1000 { return; }
    LAST_OF_KIND[k] = t | 1;
    EVENTS[(EV_TOTAL as usize) % EV_MAX] = Event { tick: t, kind, a, b };
    EV_TOTAL = EV_TOTAL.wrapping_add(1);
    STAT[S_EVENTS] = EV_TOTAL;
    // The debug port is plain I/O and safe from the tick.
    for &c in b"guard: " { outb(0xE9, c); }
    let mut v = kind;
    let mut digits = [0u8; 10];
    let mut n = 0;
    loop { digits[n] = b'0' + (v % 10) as u8; v /= 10; n += 1; if v == 0 { break; } }
    while n > 0 { n -= 1; outb(0xE9, digits[n]); }
    outb(0xE9, b'\n');
}

#[no_mangle]
pub extern "C" fn rust_guard_stats(out: *mut u32, n: u32) -> u32 {
    if out.is_null() { return 0; }
    let m = core::cmp::min(n as usize, S_COUNT);
    unsafe {
        STAT[S_TICKS] = now();
        for i in 0..m { *out.add(i) = STAT[i]; }
    }
    m as u32
}

/// Event number `seq` (counted from the first one ever recorded) as tick, kind, a, b. 0 when it is gone or not there yet.
#[no_mangle]
pub extern "C" fn rust_guard_event(seq: u32, out: *mut u32) -> u32 {
    if out.is_null() { return 0; }
    unsafe {
        let total = EV_TOTAL;
        if seq >= total || total - seq > EV_MAX as u32 { return 0; }
        let e = EVENTS[(seq as usize) % EV_MAX];
        *out = e.tick; *out.add(1) = e.kind; *out.add(2) = e.a; *out.add(3) = e.b;
    }
    1
}

// ---- which tasks are essential -------------------------------------------------------------------------------------------------

const MAX_ESS: usize = 8;
static mut ESS: [(u32, u8); MAX_ESS] = [(0, 0); MAX_ESS];
static mut N_ESS: usize = 0;
static mut RESPAWN_LAST: [u32; MAX_ESS] = [0; MAX_ESS];

/// Marks a task entry as essential: it cannot be killed and is started again if it ever goes missing.
pub unsafe fn protect(entry: u32, priority: u8) {
    if N_ESS < MAX_ESS { ESS[N_ESS] = (entry, priority); N_ESS += 1; }
}

pub fn is_essential(entry: u32) -> bool {
    unsafe { for k in 0..N_ESS { if ESS[k].0 == entry { return true; } } }
    false
}

pub fn kill_refused(pid: u32) {
    unsafe { STAT[S_REFUSED] = STAT[S_REFUSED].wrapping_add(1); record(K_REFUSED, pid, 0, false); }
}

pub fn name(entry: u32) -> *const u8 {
    if entry == text_guard_task as usize as u32 { return b"guard-text\0".as_ptr(); }
    if entry == cpu_guard_task as usize as u32 { return b"guard-cpu\0".as_ptr(); }
    if entry == supervisor_task as usize as u32 { return b"guard-tasks\0".as_ptr(); }
    if entry == mem_guard_task as usize as u32 { return b"guard-mem\0".as_ptr(); }
    core::ptr::null()
}

unsafe fn is_alive(entry: u32) -> bool {
    for i in 0..MAX_TASKS {
        if TASKS[i].is_active && TASKS[i].entry_point == entry && TASKS[i].state != TaskState::Zombie { return true; }
    }
    false
}

unsafe fn respawn(k: usize) {
    let t = now();
    if t.wrapping_sub(RESPAWN_LAST[k]) < 500 { return; }
    RESPAWN_LAST[k] = t;
    let (entry, pri) = ESS[k];
    let pid = rust_create_task_ex(entry, true, pri, 0, 0);
    if pid < 0 { record(K_SPAWN_FAIL, entry, 0, true); }
    else { STAT[S_RESPAWNS] = STAT[S_RESPAWNS].wrapping_add(1); record(K_RESPAWN, entry, pid as u32, false); }
}

// ---- guard-text ----------------------------------------------------------------------------------------------------------------

const CHUNK: usize = 4096;
const MAX_CHUNKS: usize = 4096;                      // up to 16 MB of code and read-only data
static mut HASH: [u32; MAX_CHUNKS] = [0; MAX_CHUNKS];
static mut SEEN: [bool; MAX_CHUNKS] = [false; MAX_CHUNKS];
static mut TEXT_POS: usize = 0;
static mut TEXT_LAST: u32 = 0;

unsafe fn hash_chunk(addr: usize, len: usize) -> u32 {
    let mut h: u32 = 0x811C_9DC5 ^ len as u32;
    let p = addr as *const u32;
    for i in 0..len / 4 { h = (h ^ read_volatile(p.add(i))).rotate_left(5).wrapping_mul(0x9E37_79B1); }
    let mut a = addr + (len / 4) * 4;
    while a < addr + len { h = (h ^ read_volatile(a as *const u8) as u32).wrapping_mul(0x0100_0193); a += 1; }
    h
}

extern "C" fn text_guard_task(_pid: u32) {
    unsafe {
        let t = now();
        if t.wrapping_sub(TEXT_LAST) < 100 { return; }
        TEXT_LAST = t;
        let start = addr_of!(_text_start) as usize;
        let end = addr_of!(_rodata_end) as usize;
        if end <= start { return; }
        let total = core::cmp::min((end - start + CHUNK - 1) / CHUNK, MAX_CHUNKS);
        STAT[S_TEXT_TOTAL] = total as u32;
        for _ in 0..16 {
            let c = TEXT_POS;
            let addr = start + c * CHUNK;
            let h = hash_chunk(addr, core::cmp::min(CHUNK, end - addr));
            if SEEN[c] {
                if HASH[c] != h { record(K_TEXT, c as u32, addr as u32, true); HASH[c] = h; }
            } else {
                SEEN[c] = true;
                HASH[c] = h;
            }
            STAT[S_TEXT_CHECKED] = STAT[S_TEXT_CHECKED].wrapping_add(1);
            TEXT_POS += 1;
            if TEXT_POS >= total { TEXT_POS = 0; STAT[S_TEXT_PASSES] = STAT[S_TEXT_PASSES].wrapping_add(1); }
        }
    }
}

// ---- guard-cpu -----------------------------------------------------------------------------------------------------------------

const CR0_BITS: u32 = 0x8001_0001;                   // PG, WP, PE
const CR4_BITS: u32 = 0x0030_0020;                   // SMEP, SMAP, PAE

static mut CPU_READY: bool = false;
static mut CPU_LAST: u32 = 0;
static mut CR0_BASE: u32 = 0;
static mut CR4_BASE: u32 = 0;
static mut CANARY_BASE: u32 = 0;
static mut IDT_COPY: [u8; 2048] = [0; 2048];
static mut IDT_BASE: u32 = 0;
static mut IDT_LIMIT: u16 = 0;
static mut IDT_IN_TEXT: bool = false;
static mut DPL3: [bool; 256] = [false; 256];

unsafe fn read_cr0() -> u32 { let v: u32; core::arch::asm!("mov {}, cr0", out(reg) v, options(nomem, nostack, preserves_flags)); v }
unsafe fn read_cr4() -> u32 { let v: u32; core::arch::asm!("mov {}, cr4", out(reg) v, options(nomem, nostack, preserves_flags)); v }
unsafe fn write_cr0(v: u32) { core::arch::asm!("mov cr0, {}", in(reg) v, options(nostack)); }
unsafe fn read_eflags() -> u32 { let v: u32; core::arch::asm!("pushfd", "pop {}", out(reg) v); v }

unsafe fn idt_location() -> (u32, u16) {
    let mut d = [0u8; 6];
    core::arch::asm!("sidt [{}]", in(reg) d.as_mut_ptr(), options(nostack));
    (u32::from_le_bytes([d[2], d[3], d[4], d[5]]), u16::from_le_bytes([d[0], d[1]]))
}

unsafe fn gate_handler(g: *const u8) -> u32 {
    (read_volatile(g as *const u16) as u32) | ((read_volatile(g.add(6) as *const u16) as u32) << 16)
}

unsafe fn cpu_learn() {
    CR0_BASE = read_cr0() & CR0_BITS;
    CR4_BASE = read_cr4() & CR4_BITS;
    CANARY_BASE = read_volatile(addr_of!(__stack_chk_guard));
    let (base, limit) = idt_location();
    IDT_BASE = base;
    IDT_LIMIT = limit;
    let n = core::cmp::min(limit as usize + 1, 2048);
    let lo = addr_of!(_text_start) as u32;
    let hi = addr_of!(_text_end) as u32;
    IDT_IN_TEXT = true;
    for i in 0..n { IDT_COPY[i] = read_volatile((base as usize + i) as *const u8); }
    for v in 0..n / 8 {
        let g = (base as usize + v * 8) as *const u8;
        let attr = read_volatile(g.add(5));
        if attr & 0x80 == 0 { continue; }
        let h = gate_handler(g);
        if h < lo || h >= hi { IDT_IN_TEXT = false; }
        DPL3[v] = (attr >> 5) & 3 == 3;
    }
    CPU_READY = true;
}

extern "C" fn cpu_guard_task(_pid: u32) {
    unsafe {
        let t = now();
        if t.wrapping_sub(CPU_LAST) < 100 { return; }
        CPU_LAST = t;
        if !CPU_READY { cpu_learn(); return; }
        STAT[S_CPU_CHECKS] = STAT[S_CPU_CHECKS].wrapping_add(1);

        // Control registers: a lost write-protect or paging bit is put back, anything else is reported.
        let cr0 = read_cr0() & CR0_BITS;
        if cr0 != CR0_BASE {
            record(K_CR0, CR0_BASE, cr0, true);
            if CR0_BASE & 0x1_0000 != 0 && cr0 & 0x1_0000 == 0 {
                write_cr0(read_cr0() | 0x1_0000);
                STAT[S_REPAIRS] = STAT[S_REPAIRS].wrapping_add(1);
            }
        }
        let cr4 = read_cr4() & CR4_BITS;
        if cr4 != CR4_BASE { record(K_CR4, CR4_BASE, cr4, true); CR4_BASE = cr4; }
        if read_eflags() & 0x3000 != 0 { record(K_IOPL, read_eflags(), 0, true); }

        // The stack canary is only ever written at boot.
        let canary = read_volatile(addr_of!(__stack_chk_guard));
        if canary != CANARY_BASE {
            record(K_CANARY, CANARY_BASE, canary, true);
            core::ptr::write_volatile(addr_of!(__stack_chk_guard) as *mut u32, CANARY_BASE);
            STAT[S_REPAIRS] = STAT[S_REPAIRS].wrapping_add(1);
        }

        // Interrupt table: same place, same gates; a changed gate is reported and restored from the copy taken at start-up.
        let (base, limit) = idt_location();
        if base != IDT_BASE || limit != IDT_LIMIT { record(K_IDT, 0xFFFF_FFFF, base, true); return; }
        let n = core::cmp::min(limit as usize + 1, 2048);
        let lo = addr_of!(_text_start) as u32;
        let hi = addr_of!(_text_end) as u32;
        for v in 0..n / 8 {
            let g = (base as usize + v * 8) as *mut u8;
            let mut changed = false;
            for i in 0..8 { if read_volatile(g.add(i)) != IDT_COPY[v * 8 + i] { changed = true; } }
            let attr = read_volatile(g.add(5));
            let h = gate_handler(g);
            let hooked = attr & 0x80 != 0 && IDT_IN_TEXT && (h < lo || h >= hi);
            let escalated = attr & 0x80 != 0 && (attr >> 5) & 3 == 3 && !DPL3[v];
            if changed || hooked || escalated {
                record(K_IDT, v as u32, h, true);
                for i in 0..8 { core::ptr::write_volatile(g.add(i), IDT_COPY[v * 8 + i]); }
                STAT[S_REPAIRS] = STAT[S_REPAIRS].wrapping_add(1);
            }
        }
        // The supervisor is what restarts the other guards; this one makes sure the supervisor itself stays alive.
        for k in 0..N_ESS { if ESS[k].0 == supervisor_task as usize as u32 && !is_alive(ESS[k].0) { respawn(k); } }
    }
}

// ---- guard-tasks (supervisor) -----------------------------------------------------------------------------------------------

static mut SUP_LAST: u32 = 0;
const FLOOD_LIMIT: usize = 48;

extern "C" fn supervisor_task(_pid: u32) {
    unsafe {
        let t = now();
        if t.wrapping_sub(SUP_LAST) < 100 { return; }
        SUP_LAST = t;
        let cur = CURRENT_TASK_ID;
        let lo = addr_of!(_text_start) as usize;
        let hi = addr_of!(_text_end) as usize;

        // A task whose code is outside the kernel image would run with kernel rights: never let it dispatch again.
        for i in 1..MAX_TASKS {
            if !TASKS[i].is_active || i == cur || TASKS[i].state == TaskState::Zombie { continue; }
            let e = TASKS[i].entry_point as usize;
            if e != 0 && (e < lo || e >= hi) {
                TASKS[i].state = TaskState::Zombie;
                STAT[S_ROGUE] = STAT[S_ROGUE].wrapping_add(1);
                record(K_ROGUE, TASKS[i].pid, e as u32, true);
            }
        }

        // Zombies (from kill, the sections above, or a bug) give their slots back.
        for i in 1..MAX_TASKS {
            if TASKS[i].is_active && TASKS[i].state == TaskState::Zombie && i != cur {
                TASKS[i] = Task::new();
                STAT[S_REAPED] = STAT[S_REAPED].wrapping_add(1);
            }
        }

        // The task counter must match the table.
        let mut active = 0usize;
        for i in 0..MAX_TASKS { if TASKS[i].is_active { active += 1; } }
        if active != NUM_TASKS {
            record(K_COUNT, NUM_TASKS as u32, active as u32, false);
            NUM_TASKS = active;
            STAT[S_REPAIRS] = STAT[S_REPAIRS].wrapping_add(1);
        }

        // Too many tasks at once is a runaway: the newest ones that are not essential are stopped.
        let mut trimmed = 0;
        while active > FLOOD_LIMIT && trimmed < 8 {
            let mut victim = usize::MAX;
            let mut newest = 0u32;
            for i in 1..MAX_TASKS {
                if !TASKS[i].is_active || i == cur || TASKS[i].state == TaskState::Zombie || is_essential(TASKS[i].entry_point) { continue; }
                if victim == usize::MAX || TASKS[i].created_tick >= newest { victim = i; newest = TASKS[i].created_tick; }
            }
            if victim == usize::MAX { break; }
            TASKS[victim].state = TaskState::Zombie;
            active -= 1;
            trimmed += 1;
            STAT[S_FLOOD] = STAT[S_FLOOD].wrapping_add(1);
            record(K_FLOOD, TASKS[victim].pid, active as u32, true);
        }

        // Essential tasks: start the missing ones again, wake the ones that cannot wake themselves.
        let mut guards = 0u32;
        for k in 0..N_ESS {
            let (entry, _) = ESS[k];
            let mut alive = false;
            for i in 0..MAX_TASKS {
                if !TASKS[i].is_active || TASKS[i].entry_point != entry || TASKS[i].state == TaskState::Zombie { continue; }
                alive = true;
                let stuck_asleep = TASKS[i].state == TaskState::Sleeping && (TASKS[i].wake_at.wrapping_sub(t) as i32) > 10_000;
                if TASKS[i].state == TaskState::Blocked || stuck_asleep {
                    TASKS[i].state = TaskState::Ready;
                    TASKS[i].block_reason = BlockReason::None;
                    record(K_STUCK, TASKS[i].pid, entry, false);
                    STAT[S_REPAIRS] = STAT[S_REPAIRS].wrapping_add(1);
                }
                break;
            }
            if alive { if name(entry) != core::ptr::null() { guards += 1; } } else { respawn(k); }
        }
        STAT[S_GUARDS] = guards;
    }
}

// ---- guard-mem ---------------------------------------------------------------------------------------------------------------

static mut MEM_LAST: u32 = 0;
static mut LOWMEM_ACTIVE: bool = false;

extern "C" fn mem_guard_task(_pid: u32) {
    unsafe {
        let t = now();
        if t.wrapping_sub(MEM_LAST) < 1000 { return; }
        MEM_LAST = t;
        let free_mb = (pmm_get_free_memory() >> 20) as u32;
        let total_mb = (pmm_get_total_memory() >> 20) as u32;
        if total_mb == 0 { return; }
        let pct = free_mb * 100 / total_mb;
        if free_mb < 4 || pct < 3 {
            if !LOWMEM_ACTIVE {
                LOWMEM_ACTIVE = true;
                STAT[S_LOWMEM] = STAT[S_LOWMEM].wrapping_add(1);
                record(K_LOWMEM, free_mb, total_mb, false);
            }
        } else if pct > 6 {
            LOWMEM_ACTIVE = false;
        }
    }
}

/// While a command runs (or a download waits) the scheduler does not dispatch tasks; the timer calls this instead so the guards
/// keep watching. Each task gates itself by time, so most calls return at once.
#[no_mangle]
pub extern "C" fn rust_guard_tick() {
    unsafe { supervisor_task(0); cpu_guard_task(0); mem_guard_task(0); text_guard_task(0); }
}

// ---- start-up -----------------------------------------------------------------------------------------------------------------

/// Starts the guard tasks and registers them (and the tasks that were started before) as essential.
pub unsafe fn spawn_all() {
    let list: [(u32, u8); 4] = [
        (supervisor_task as usize as u32, 9),
        (cpu_guard_task as usize as u32, 7),
        (text_guard_task as usize as u32, 5),
        (mem_guard_task as usize as u32, 3),
    ];
    for (entry, pri) in list.iter() {
        protect(*entry, *pri);
        if rust_create_task_ex(*entry, true, *pri, 0, 0) < 0 { record(K_SPAWN_FAIL, *entry, 0, true); }
    }
    record(K_START, N_ESS as u32, 0, false);
}

// ---- self-test (used by `guard selftest`) ---------------------------------------------------------------------------------------

static mut ROGUE_CODE: [u8; 16] = [0xC3; 16];        // a bare `ret`, living in data
static mut ST_ROGUE_PID: i32 = -1;
static mut ST_ESSENTIAL_PID: u32 = 0;
static mut ST_ESSENTIAL_SLOT: usize = 0;
static mut ST_BEFORE: [u32; S_COUNT] = [0; S_COUNT];
static mut ST_MEM_PID: u32 = 0;

/// Sets up three faults and one forbidden kill; `rust_guard_selftest_result` tells a few seconds later what the guards made of them.
#[no_mangle]
pub extern "C" fn rust_guard_selftest_begin() -> u32 {
    unsafe {
        ST_BEFORE = STAT;
        // 1. a task whose code is not in the kernel image
        ST_ROGUE_PID = rust_create_task_ex(addr_of!(ROGUE_CODE) as usize as u32, true, 2, 0, 0);
        // 2. an essential task that "crashed"
        ST_MEM_PID = 0;
        for i in 1..MAX_TASKS {
            if TASKS[i].is_active && TASKS[i].entry_point == mem_guard_task as usize as u32 {
                ST_MEM_PID = TASKS[i].pid;
                TASKS[i].state = TaskState::Zombie;
                break;
            }
        }
        // 3. a gate in the interrupt table that jumps outside the kernel (vector 100 is not used)
        let (base, _) = idt_location();
        let g = (base as usize + 100 * 8) as *mut u8;
        let h = addr_of!(ROGUE_CODE) as usize as u32;
        let bytes = [h as u8, (h >> 8) as u8, 0x08, 0x00, 0x00, 0x8E, (h >> 16) as u8, (h >> 24) as u8];
        for i in 0..8 { core::ptr::write_volatile(g.add(i), bytes[i]); }
        // 4. somebody tries to kill an essential task
        ST_ESSENTIAL_PID = 0;
        for i in 1..MAX_TASKS {
            if TASKS[i].is_active && is_essential(TASKS[i].entry_point) && TASKS[i].entry_point != mem_guard_task as usize as u32 {
                ST_ESSENTIAL_PID = TASKS[i].pid;
                ST_ESSENTIAL_SLOT = i;
                break;
            }
        }
        if ST_ESSENTIAL_PID != 0 { rust_kill_task(ST_ESSENTIAL_PID); }
        1
    }
}

/// Bit 0: the rogue task is gone. 1: the crashed guard runs again under a new pid. 2: the interrupt gate is back to normal.
/// 3: the essential task survived the kill. 4: the supervisor counted the rogue task.
#[no_mangle]
pub extern "C" fn rust_guard_selftest_result() -> u32 {
    unsafe {
        let mut r = 0u32;
        let mut rogue_present = false;
        let mut mem_new = false;
        for i in 0..MAX_TASKS {
            if !TASKS[i].is_active { continue; }
            if ST_ROGUE_PID >= 0 && TASKS[i].pid == ST_ROGUE_PID as u32 && TASKS[i].state != TaskState::Zombie { rogue_present = true; }
            if TASKS[i].entry_point == mem_guard_task as usize as u32 && TASKS[i].state != TaskState::Zombie && TASKS[i].pid != ST_MEM_PID { mem_new = true; }
        }
        if !rogue_present { r |= 1; }
        if mem_new { r |= 2; }
        let (base, _) = idt_location();
        let g = (base as usize + 100 * 8) as *const u8;
        let mut zero = true;
        for i in 0..8 { if read_volatile(g.add(i)) != 0 { zero = false; } }
        if zero { r |= 4; }
        let t = &TASKS[ST_ESSENTIAL_SLOT];
        if ST_ESSENTIAL_PID != 0 && t.is_active && t.pid == ST_ESSENTIAL_PID && t.state != TaskState::Zombie { r |= 8; }
        if STAT[S_ROGUE] > ST_BEFORE[S_ROGUE] { r |= 16; }
        r
    }
}
