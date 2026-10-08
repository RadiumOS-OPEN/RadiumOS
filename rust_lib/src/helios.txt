// ============================================================================
//  HELIOS - The RadiumOS Native Compiler  v7.0
//  author: scp_2801
//  file:   rust_lib/src/helios.rs
//
//  v4.0 over v3.0:
//    ┌─ FIXES ────────────────────────────────────────────────────────────────┐
//    │ • from_u8 opcode table: SetGs(0x90) was missing, shifting every        │
//    │   extended syscall (PicEoi..MemQuery) one slot off — corrected          │
//    │ • SetGs VM arm was missing entirely — added                             │
//    │ • Expr::Field always emitted index 0 — now resolves field name→index    │
//    │   via the struct table passed into Emitter                              │
//    │ • OOM on complex code: Lexer/Parser/Emitter now use arena scratch       │
//    │   buffers and the value stack is allocated via kernel malloc() instead  │
//    │   of Box::new([0; 1M]), which blew the Rust stack for deep programs     │
//    │ • Const pool cap increased from 32 KB (u16-indexed) to 64 K entries;   │
//    │   large string programs no longer OOM the pool                          │
//    │ • Vec over-allocation: all Vecs now constructed with with_capacity()    │
//    │   where the size is known, halving peak heap usage during compilation   │
//    │ • avfs_get_filesize() could return 0 for a file that exists but has     │
//    │   never been written; read_src() now treats 0 as valid                  │
//    └────────────────────────────────────────────────────────────────────────┘
//    ┌─ LANGUAGE ─────────────────────────────────────────────────────────────┐
//    │ • `fn` return type inference: `-> void` may be omitted on void fns     │
//    │ • `let mut` accepted (mut is advisory, memory model is already mutable) │
//    │ • Named `break value`: `break expr;` in `loop` returns expr             │
//    │ • Multi-arm `match` on str literals (StrCmp chain)                      │
//    │ • Char literal  'A'  -> IntLit(65)                                      │
//    │ • Range step    for i in 0..100 step 2 { }                              │
//    │ • Nested struct field chains resolved: foo.bar.baz                      │
//    └────────────────────────────────────────────────────────────────────────┘
//    ┌─ DIAGNOSTICS ──────────────────────────────────────────────────────────┐
//    │ • E003 duplicate function name                                          │
//    │ • E004 duplicate struct name                                            │
//    │ • E008 undefined variable                                               │
//    │ • E009 undefined function call                                          │
//    │ • E011 mismatched types (basic: int used where str expected)            │
//    │ • W401 improved: reports the exact missing return path                  │
//    │ • W601 large array literal (>256 elements) - heap pressure warning      │
//    │ • W602 function with >16 parameters                                     │
//    │ • `helios test all`  runs every new test case                           │
//    └────────────────────────────────────────────────────────────────────────┘
//    ┌─ VM ───────────────────────────────────────────────────────────────────┐
//    │ • Value stack: heap-allocated via malloc(), not Rust stack Box          │
//    │   Size configurable at runtime; default 4 MB (1 M i32 slots)          │
//    │ • String concat (StrCat) fully implemented with heap scratch buffer     │
//    │ • StrSub / StrFind implemented                                          │
//    │ • IntToStr implemented using kernel scratch buffer                      │
//    │ • Heap tracker: VM reports bytes_allocd / bytes_freed on -vv           │
//    │ • Execution step counter; halts with E_TIMEOUT after 50 M steps        │
//    └────────────────────────────────────────────────────────────────────────┘
//    ┌─ SHELL ────────────────────────────────────────────────────────────────┐
//    │ • `helios disasm <file.rxe>`  pretty-prints RXE bytecode               │
//    │ • `helios info   <file.rxe>`  prints header fields                     │
//    │ • `helios bench  <file.hls>`  compiles + runs 5x, reports mean steps   │
//    │ • `helios fmt    <file.hls>`  pretty-prints AST as re-formatted source  │
//    └────────────────────────────────────────────────────────────────────────┘
//
//  v5.0 over v4.0:
//    ┌─ FIXES ────────────────────────────────────────────────────────────────┐
//    │ • OOM in unsafe / inline-asm blocks: unsafe { } now lowers stack        │
//    │   guard to 0 and pre-faults the scratch page before emitting asm ops;  │
//    │   VM_UNSAFE_STACK_RESERVE (16 KB) is malloc'd at VM init and freed     │
//    │   on first unsafe op — eliminates OOM trap on first `unsafe` entry     │
//    │ • inline asm OOM: AsmInt/AsmNop/AsmCli/AsmSti no longer push a dummy  │
//    │   Push(0) for the expression result; the emitter now emits the asm op  │
//    │   only for stmt context and a load-bearing Push(0) only in expr context │
//    │   via a new emit_expr_asm() path — halves bytecode for asm-heavy files  │
//    │ • Input OOM: keyboard_input panic on null replaced with graceful OOM    │
//    │   handler that pushes empty-string const and prints a warning           │
//    │ • PortBurstR/W: rep insb/outsb used edi/esi directly; now uses the     │
//    │   correct i686 constraint so the Rust register allocator doesn't alias  │
//    └────────────────────────────────────────────────────────────────────────┘
//    ┌─ LANGUAGE ─────────────────────────────────────────────────────────────┐
//    │ • `if let` pattern: if let IntLit(n) = expr { } (int only for now)    │
//    │ • `const` top-level integer constant: const FOO: int = 42;             │
//    │ • `assert!(cond)` built-in — halts with E099 + message on failure      │
//    │ • String escape \uXXXX unicode codepoint in str/char literals          │
//    │ • Compound range: for i in a..b, c..d — reserved syntax detected       │
//    └────────────────────────────────────────────────────────────────────────┘
//    ┌─ DIAGNOSTICS ──────────────────────────────────────────────────────────┐
//    │ • E012 assert failure (runtime halt code)                               │
//    │ • E013 const redefinition                                               │
//    │ • W603 unsafe block with no unsafe ops (useless unsafe)                 │
//    │ • W604 asm!() used outside unsafe block                                 │
//    │ • Diag span: secondary spans added (for duplicate-def errors, shows     │
//    │   original definition site alongside re-definition site)                │
//    └────────────────────────────────────────────────────────────────────────┘
//    ┌─ VM ───────────────────────────────────────────────────────────────────┐
//    │ • Assert opcode (0xD0): pops cond; if 0, prints E099 and halts         │
//    │ • VM_UNSAFE_STACK_RESERVE: 16 KB reserve freed on first unsafe entry   │
//    │ • StrUpper / StrLower (0xB7 / 0xB8): in-place case conversion          │
//    │ • PrintStr opcode (0xB9): prints heap-ptr string without tag-bit check  │
//    │ • VM step display on -v now also shows unsafe-op count                  │
//    └────────────────────────────────────────────────────────────────────────┘
//    ┌─ SHELL ────────────────────────────────────────────────────────────────┐
//    │ • `helios version`  prints detailed build info                          │
//    │ • `helios symbols <file.rxe>`  dumps symbol table if present           │
//    │ • `helios strip   <file.rxe>`  strips debug section, writes <out.rxe>  │
//    │ • `helios const   <name> <val>` — define a one-shot compile constant   │
//    │ • `helios run` now accepts `-stack <KB>` to override VM stack size      │
//    │ • `-vvv` flag: ultra-trace (every push/pop with value)                  │
//    └────────────────────────────────────────────────────────────────────────┘
//
//  v6.0 over v5.0:
//    ┌─ LANGUAGE ─────────────────────────────────────────────────────────────┐
//    │ • `include "file.hls"` — compile-time file inclusion; merges structs,  │
//    │   consts, and functions from another .hls file before compiling         │
//    │ • `print_hex(expr)` as a statement-form keyword (was expr-only)         │
//    └────────────────────────────────────────────────────────────────────────┘
//    ┌─ INLINE ASM (expanded) ─────────────────────────────────────────────────┐
//    │ • asm!("hlt")        — halt the CPU                                     │
//    │ • asm!("pushfd")     — push EFLAGS, result on VM stack                  │
//    │ • asm!("popfd")      — pop EFLAGS from VM stack                         │
//    │ • asm!("lidt")       — load IDT from ptr on VM stack (unsafe)           │
//    │ • asm!("lgdt")       — load GDT from ptr on VM stack (unsafe)           │
//    │ • asm!("wbinvd")     — write-back + invalidate caches                   │
//    │ • asm!("invlpg")     — invalidate TLB for address on VM stack           │
//    │ • asm!("mov_cr2_eax")— read CR2 (page-fault address) → VM stack        │
//    │ • asm!("clts")       — clear CR0.TS (task-switched) flag                │
//    │ • asm!("xchg_eax_ebx")— atomically exchange EAX/EBX via lock prefix    │
//    └────────────────────────────────────────────────────────────────────────┘
//    ┌─ HARDWARE OPCODES (new) ─────────────────────────────────────────────────┐
//    │ • AsmHlt    (0xE0) — emit hlt instruction                               │
//    │ • AsmPushfd (0xE1) — push EFLAGS onto VM stack via pushfd               │
//    │ • AsmPopfd  (0xE2) — pop EFLAGS from VM stack via popfd                 │
//    │ • AsmLidt   (0xE3) — lgdt/lidt from VM stack ptr (kernel use)           │
//    │ • AsmLgdt   (0xE4) — lgdt from VM stack ptr                             │
//    │ • AsmWbinvd (0xE5) — wbinvd (cache flush)                               │
//    │ • AsmInvlpg (0xE6) — invlpg for addr on VM stack                        │
//    │ • GetCr2    (0xE7) — read CR2 (page-fault addr) → VM stack             │
//    │ • AsmClts   (0xE8) — clear CR0.TS                                       │
//    │ • HwMapMem  (0xE9) — identity-map a physical range (peek/poke helper)   │
//    │ • PortRd16  (0xEA) — in16 shorthand with auto push                      │
//    │ • PortWr16  (0xEB) — out16 shorthand                                    │
//    └────────────────────────────────────────────────────────────────────────┘
//    ┌─ SHELL (new commands) ──────────────────────────────────────────────────┐
//    │ • `helios cat   <file.hls>`  — dump source with line numbers             │
//    │ • `helios deps  <file.hls>`  — list `include` dependencies               │
//    │ • `helios build <file.hls> [file2.hls ...] -o out.rxe`                  │
//    │     multi-file compilation: merges all sources, writes one RXE          │
//    │ • `helios demo  <name>`      — run a built-in demo program               │
//    │     demos: hw_port   — blink keyboard LED via port 0x60/0x64            │
//    │            pit_tick  — reprogram PIT and count ticks                     │
//    │            mem_scan  — walk physical memory via peek32                   │
//    │            asm_chain — showcase every new inline asm instruction         │
//    │            cpuid_info— print CPU vendor string via cpuid                 │
//    │ • `helios -vvv` now counts and dumps all asm ops at exit                 │
//    └────────────────────────────────────────────────────────────────────────┘
//    ┌─ MULTI-FILE COMPILATION ────────────────────────────────────────────────┐
//    │ • `helios build a.hls b.hls c.hls -o out.rxe` reads all sources,       │
//    │   concatenates them with include-guard deduplication, then compiles     │
//    │   a single unified program; each file is separated by a synthetic       │
//    │   comment boundary for error-span accuracy                              │
//    └────────────────────────────────────────────────────────────────────────┘
//
//  v7.0 over v6.0:
//    ┌─ INLINE EVAL ──────────────────────────────────────────────────────────┐
//    │ • `helios eval <code...>`                                               │
//    │     Joins all remaining argv tokens with spaces, wraps bare snippets    │
//    │     in `fn main() { ... }` if no `fn main` / `fn ` / `struct ` is      │
//    │     detected, then compiles and runs in-process — no file required.     │
//    │   Examples:                                                             │
//    │     helios eval print("hello");                                         │
//    │     helios eval let x = cpuid(0); print_hex(x);                        │
//    │     helios eval fn main() { unsafe { asm!("cli"); } }                  │
//    │     helios -v eval unsafe { let f = get_cr0(); print_hex(f); }         │
//    │                                                                         │
//    │ • `helios run <code...>` — extended to auto-detect file vs source       │
//    │     If the first token ends with .hls/.rxe or names an existing AVFS   │
//    │     file, behaves exactly as before.  Otherwise joins remaining tokens  │
//    │     and compiles them as inline source (same wrap logic as eval).       │
//    │                                                                         │
//    │ • helios_maybe_wrap()  — heuristic auto-wrapper (Section 15b)          │
//    │ • helios_collect_inline_src() — joins argv[start..argc] with spaces    │
//    │ • -v flag shows the (possibly wrapped) source before compiling          │
//    └────────────────────────────────────────────────────────────────────────┘
// ============================================================================

#![allow(dead_code)]
#![allow(unused_variables)]
#![allow(unused_mut)]
#![allow(unused_assignments)]

extern crate alloc;
use alloc::vec::Vec;
use alloc::string::String;
use alloc::format;
use alloc::boxed::Box;
use core::str;
use alloc::vec;

// ─── FFI - RadiumOS kernel ────────────────────────────────────────────────────
extern "C" {
    fn terminal_putchar(c: u8);
    fn terminal_setcolor(color: u8);
    fn print(s: *const u8);
    fn malloc(size: u32) -> *mut u8;
    fn free(ptr: *mut u8);
    fn avfs_read_file(name: *const u8, buffer: *mut u8, size: u32, offset: u32) -> i32;
    fn avfs_write_file(name: *const u8, buffer: *const u8, size: u32, offset: u32) -> i32;
    fn avfs_create_file(name: *const u8, size: u32) -> i32;
    fn avfs_file_exists(name: *const u8) -> bool;
    fn avfs_is_directory(name: *const u8) -> bool;
    fn avfs_get_filesize(name: *const u8) -> i32;
    fn keyboard_input(buf: *mut u8) -> i32;
    fn keyboard_poll_scancode() -> i32;
    fn keyboard_scancode_capture_begin();
    fn keyboard_scancode_capture_end();
    fn get_ticks() -> u32;
    fn sleep_ms(ms: u32);
    fn register_command(
        name:        *const u8,
        description: *const u8,
        execute:     extern "C" fn(i32, *mut *mut u8),
    ) -> i32;
    fn rust_prp_selftest() -> i32;
    fn rust_prp_random(output: *mut u8, len: u32) -> i32;
    fn rust_prp_sha256_file(filename: *const u8, output: *mut u8) -> i32;
    fn rust_prp_seal_file(input: *const u8, output: *const u8, key_hex: *const u8) -> i32;
    fn rust_prp_open_file(input: *const u8, output: *const u8, key_hex: *const u8) -> i32;
    fn rust_prp_seal_text(text: *const u8, len: u32, key_hex: *const u8, output: *const u8) -> i32;
    fn rust_prp_fingerprint(keyfile: *const u8, output: *mut u8) -> i32;
    fn rust_prp_keygen(name: *const u8) -> i32;
    fn rust_prp_sign(file: *const u8, private_key: *const u8) -> i32;
    fn rust_prp_verify(file: *const u8, expected_pub: *const u8) -> i32;
    fn rust_prp_seal_file_pub(input: *const u8, output: *const u8, recipient_pub: *const u8) -> i32;
    fn rust_prp_open_file_prv(input: *const u8, output: *const u8, private_key: *const u8) -> i32;
    fn rust_prp_seal_text_pub(text: *const u8, len: u32, recipient_pub: *const u8, output: *const u8) -> i32;
    fn rust_https_get(url: *const u8) -> i32;
    fn rust_fetch_active() -> bool;
    fn rust_test_https() -> i32;
}

// ─── VGA colour constants ─────────────────────────────────────────────────────
const COL_WHITE:   u8 = 15;
const COL_RED:     u8 = 12;   // errors
const COL_YELLOW:  u8 = 14;   // warnings
const COL_BLUE:    u8 = 9;    // notes / line numbers
const COL_CYAN:    u8 = 11;   // hints / verbose / underline
const COL_GREEN:   u8 = 10;   // success
const COL_GREY:    u8 = 7;    // trace / gutter
const COL_MAGENTA: u8 = 13;   // struct / type names

// ─── Terminal helpers ─────────────────────────────────────────────────────────
#[inline] unsafe fn kputc(c: u8) { terminal_putchar(c); }
#[inline] unsafe fn kprint(s: &[u8]) { for &c in s { kputc(c); } }
#[inline] unsafe fn kprintln(s: &[u8]) { kprint(s); kputc(b'\n'); }
#[inline] unsafe fn kprint_str(s: &str) { kprint(s.as_bytes()); }
#[inline] unsafe fn kprint_col(col: u8, s: &[u8]) {
    terminal_setcolor(col); kprint(s); terminal_setcolor(COL_WHITE);
}
#[inline] unsafe fn kprintln_col(col: u8, s: &[u8]) {
    kprint_col(col, s); kputc(b'\n');
}

unsafe fn kprint_padded(s: &[u8], width: usize) {
    for _ in s.len()..width { kputc(b' '); }
    kprint(s);
}

fn i32_to_str(mut n: i32, buf: &mut [u8; 16]) -> &[u8] {
    if n == 0 { buf[0] = b'0'; return &buf[..1]; }
    let neg = n < 0;
    if neg { n = if n == i32::MIN { 2147483648u32 as i32 } else { -n }; }
    let mut i = 15usize;
    let mut pos = i;
    loop {
        buf[i] = b'0' + (n % 10) as u8;
        n /= 10; pos = i;
        if n == 0 { break; }
        i -= 1;
    }
    if neg { pos -= 1; buf[pos] = b'-'; }
    &buf[pos..16]
}
unsafe fn kprint_i32(n: i32) {
    let mut b = [0u8; 16]; let s = i32_to_str(n, &mut b); kprint(s);
}
unsafe fn kprint_u32_hex(n: u32) {
    kprint(b"0x");
    for shift in [28u32, 24, 20, 16, 12, 8, 4, 0] {
        let nib = (n >> shift) & 0xF;
        kputc(if nib < 10 { b'0' + nib as u8 } else { b'a' + nib as u8 - 10 });
    }
}
// Helper: write decimal u32 into caller-supplied buffer; returns written slice
fn u32_to_str(mut n: u32, buf: &mut [u8; 12]) -> &[u8] {
    if n == 0 { buf[0] = b'0'; return &buf[..1]; }
    let mut i = 11usize; let mut pos = i;
    loop {
        buf[i] = b'0' + (n % 10) as u8;
        n /= 10; pos = i;
        if n == 0 { break; }
        i -= 1;
    }
    &buf[pos..12]
}

fn hex_byte(c: u8) -> u8 {
    if c >= b'a' { c - b'a' + 10 }
    else if c >= b'A' { c - b'A' + 10 }
    else { c - b'0' }
}

// ============================================================================
//  SECTION 1 - RXE BINARY FORMAT
// ============================================================================

const RXE_MAGIC:        [u8; 8] = [0x52,0x41,0x44,0x49,0x55,0x4D,0x5F,0x58]; // "RADIUM_X"
const RXE_VERSION:      u8      = 0x06;   // bumped for v6 (expanded asm, multi-file, include)
const RXE_ARCH_I686:    u8      = 0x86;
const RXE_SECTION_CODE: u8      = 0x01;
const RXE_SECTION_DATA: u8      = 0x02;
const RXE_SECTION_SYM:  u8      = 0x03;   // NEW: optional debug symbol table
const RXE_SECTION_END:  u8      = 0xFF;

struct RxeHeader {
    magic:       [u8; 8],
    version:     u8,
    arch:        u8,
    entry_point: u32,
    code_size:   u32,
    data_size:   u32,
    flags:       u16,
    author_tag:  [u8; 8],
}
impl RxeHeader {
    fn new(ep: u32, cs: u32, ds: u32) -> Self {
        RxeHeader {
            magic: RXE_MAGIC, version: RXE_VERSION, arch: RXE_ARCH_I686,
            entry_point: ep, code_size: cs, data_size: ds, flags: 0,
            author_tag: *b"scp_2801",
        }
    }
    fn to_bytes(&self) -> [u8; 32] {
        let mut b = [0u8; 32];
        b[0..8].copy_from_slice(&self.magic);
        b[8]  = self.version;
        b[9]  = self.arch;
        b[10..14].copy_from_slice(&self.entry_point.to_le_bytes());
        b[14..18].copy_from_slice(&self.code_size.to_le_bytes());
        b[18..22].copy_from_slice(&self.data_size.to_le_bytes());
        b[22..24].copy_from_slice(&self.flags.to_le_bytes());
        b[24..32].copy_from_slice(&self.author_tag);
        b
    }
}

// ============================================================================
//  SECTION 2 - OPCODE TABLE  (v4: SetGs restored; from_u8 corrected)
// ============================================================================
#[derive(Clone, Debug, PartialEq)]
#[repr(u8)]
enum Opcode {
    // Stack
    Push=0x01, PushStr=0x02, Pop=0x03, Dup=0x04, Swap=0x05, Over=0x06,
    // Arithmetic
    Add=0x10, Sub=0x11, Mul=0x12, Div=0x13, Mod=0x14, Neg=0x15,
    // Bitwise
    BAnd=0x16, BOr=0x17, BXor=0x18, BNot=0x19, Shl=0x1A, Shr=0x1B,
    // Comparison
    CmpEq=0x20, CmpNe=0x21, CmpLt=0x22, CmpLe=0x23, CmpGt=0x24, CmpGe=0x25,
    // Logical
    And=0x28, Or=0x29, Not=0x2A,
    // Control
    Jmp=0x30, Jz=0x31, Jnz=0x32, Call=0x33, Ret=0x34, Halt=0x35,
    // Locals / Registers (reg file r0..r15)
    LoadLocal=0x40, StoreLocal=0x41,
    LoadReg=0x42,   StoreReg=0x43,
    // Heap / arrays
    HeapAlloc=0x44, HeapFree=0x45,
    ArrLoad=0x46,   ArrStore=0x47,
    // I/O
    Print=0x50, PrintNum=0x51, PrintChar=0x52, Input=0x53, PrintHex=0x54,
    // FS
    FileExist=0x62, FileRead=0x63, FileWrite=0x64, FileSize=0x65,
    // Hardware - I/O ports
    In8=0x70,  Out8=0x71,  In16=0x72, Out16=0x73, In32=0x74, Out32=0x75,
    // Hardware - control
    Cli=0x76, Sti=0x77, Hlt=0x78, Rdtsc=0x79,
    // Hardware - memory ops
    MemCpy=0x7A, MemSet=0x7B, AllocPage=0x7C, FreePage=0x7D,
    // Hardware - CR registers
    GetCr0=0x7E, SetCr0=0x7F, GetCr3=0x80, SetCr3=0x81,
    DebugBreak=0x82,
    // Hardware - physical r/w
    Peek8=0x83, Poke8=0x84,
    Peek16=0x85, Poke16=0x86,
    Peek32=0x87, Poke32=0x88,
    // Hardware - segment registers
    GetCs=0x89, GetDs=0x8A, GetEs=0x8B,
    GetFs=0x8C, GetGs=0x8D, GetSs=0x8E,
    SetFs=0x8F,
    SetGs=0x90,  // FIX: was missing from from_u8; all syscalls below shifted by 1 before
    // Extended syscalls (v4: discriminants corrected to match from_u8)
    PicEoi=0x91,   PicMask=0x92,    PicUnmask=0x93, PitSetHz=0x94,
    GetTicks=0x95, SleepMs=0x96,
    Cpuid=0x97,
    RdMsr=0x98,    WrMsr=0x99,
    PortBurstR=0x9A, PortBurstW=0x9B, MemQuery=0x9C,
    // Extended hardware
    RdMsr64=0xA0, WrMsr64=0xA1,
    Sgdt=0xA2, Sidt=0xA3,
    Rdtscp=0xA4,
    Iopl3=0xA5,
    AsmNop=0xA6, AsmCli=0xA7, AsmSti=0xA8, AsmInt=0xA9,
    FxSave=0xAA, FxRstor=0xAB,
    // String ops
    StrLen=0xB0, StrCmp=0xB1, StrCat=0xB2, StrSub=0xB3, StrFind=0xB4,
    IntToStr=0xB5, StrToInt=0xB6,
    StrUpper=0xB7, StrLower=0xB8, // v5: in-place case conversion (heap copy)
    PrintStr=0xB9,                 // v5: print heap-ptr string (no tag-bit)
    StrByte=0xBA,                  // first byte of a string as an integer
    // Cast
    CastInt=0xC0, CastBool=0xC1,
    // Misc / v5
    Assert=0xD0,   // v5: pop cond; halt with E012 if 0
    PrpSelfTest=0xD1, PrpRandom=0xD2, PrpSha256File=0xD3,
    PrpSealFile=0xD4, PrpOpenFile=0xD5, PrpSealText=0xD6,
    PrpFingerprint=0xD7, PrpKeygen=0xD8, PrpSign=0xD9,
    PrpVerify=0xDA, PrpSealFilePub=0xDB, PrpOpenFilePrv=0xDC,
    PrpSealTextPub=0xDD,
    FetchGet=0xED, FetchActive=0xEE, FetchTest=0xEF,
    // v6: expanded inline asm opcodes
    AsmHlt=0xE0,    // emit hlt
    AsmPushfd=0xE1, // push EFLAGS -> VM stack
    AsmPopfd=0xE2,  // pop VM stack -> EFLAGS (UNSAFE)
    AsmLidt=0xE3,   // lidt from ptr on VM stack (UNSAFE)
    AsmLgdt=0xE4,   // lgdt from ptr on VM stack (UNSAFE)
    AsmWbinvd=0xE5, // wbinvd - write-back invalidate caches
    AsmInvlpg=0xE6, // invlpg for address on VM stack
    GetCr2=0xE7,    // read CR2 (page-fault address) -> VM stack
    AsmClts=0xE8,   // clts - clear CR0.TS
    HwMapMem=0xE9,  // identity-map physical range (base, len) -> 0 ok / -1 err
    PollScancode=0xEA,
    Nop=0xFF,
}

impl Opcode {
    fn from_u8(b: u8) -> Option<Opcode> {
        match b {
            0x01=>Some(Opcode::Push),      0x02=>Some(Opcode::PushStr),   0x03=>Some(Opcode::Pop),
            0x04=>Some(Opcode::Dup),       0x05=>Some(Opcode::Swap),      0x06=>Some(Opcode::Over),
            0x10=>Some(Opcode::Add),       0x11=>Some(Opcode::Sub),       0x12=>Some(Opcode::Mul),
            0x13=>Some(Opcode::Div),       0x14=>Some(Opcode::Mod),       0x15=>Some(Opcode::Neg),
            0x16=>Some(Opcode::BAnd),      0x17=>Some(Opcode::BOr),       0x18=>Some(Opcode::BXor),
            0x19=>Some(Opcode::BNot),      0x1A=>Some(Opcode::Shl),       0x1B=>Some(Opcode::Shr),
            0x20=>Some(Opcode::CmpEq),     0x21=>Some(Opcode::CmpNe),     0x22=>Some(Opcode::CmpLt),
            0x23=>Some(Opcode::CmpLe),     0x24=>Some(Opcode::CmpGt),     0x25=>Some(Opcode::CmpGe),
            0x28=>Some(Opcode::And),       0x29=>Some(Opcode::Or),        0x2A=>Some(Opcode::Not),
            0x30=>Some(Opcode::Jmp),       0x31=>Some(Opcode::Jz),        0x32=>Some(Opcode::Jnz),
            0x33=>Some(Opcode::Call),      0x34=>Some(Opcode::Ret),       0x35=>Some(Opcode::Halt),
            0x40=>Some(Opcode::LoadLocal), 0x41=>Some(Opcode::StoreLocal),
            0x42=>Some(Opcode::LoadReg),   0x43=>Some(Opcode::StoreReg),
            0x44=>Some(Opcode::HeapAlloc), 0x45=>Some(Opcode::HeapFree),
            0x46=>Some(Opcode::ArrLoad),   0x47=>Some(Opcode::ArrStore),
            0x50=>Some(Opcode::Print),     0x51=>Some(Opcode::PrintNum),
            0x52=>Some(Opcode::PrintChar), 0x53=>Some(Opcode::Input),     0x54=>Some(Opcode::PrintHex),
            0x62=>Some(Opcode::FileExist), 0x63=>Some(Opcode::FileRead),
            0x64=>Some(Opcode::FileWrite), 0x65=>Some(Opcode::FileSize),
            0x70=>Some(Opcode::In8),       0x71=>Some(Opcode::Out8),
            0x72=>Some(Opcode::In16),      0x73=>Some(Opcode::Out16),
            0x74=>Some(Opcode::In32),      0x75=>Some(Opcode::Out32),
            0x76=>Some(Opcode::Cli),       0x77=>Some(Opcode::Sti),
            0x78=>Some(Opcode::Hlt),       0x79=>Some(Opcode::Rdtsc),
            0x7A=>Some(Opcode::MemCpy),    0x7B=>Some(Opcode::MemSet),
            0x7C=>Some(Opcode::AllocPage), 0x7D=>Some(Opcode::FreePage),
            0x7E=>Some(Opcode::GetCr0),    0x7F=>Some(Opcode::SetCr0),
            0x80=>Some(Opcode::GetCr3),    0x81=>Some(Opcode::SetCr3),
            0x82=>Some(Opcode::DebugBreak),
            0x83=>Some(Opcode::Peek8),     0x84=>Some(Opcode::Poke8),
            0x85=>Some(Opcode::Peek16),    0x86=>Some(Opcode::Poke16),
            0x87=>Some(Opcode::Peek32),    0x88=>Some(Opcode::Poke32),
            0x89=>Some(Opcode::GetCs),     0x8A=>Some(Opcode::GetDs),
            0x8B=>Some(Opcode::GetEs),     0x8C=>Some(Opcode::GetFs),
            0x8D=>Some(Opcode::GetGs),     0x8E=>Some(Opcode::GetSs),
            0x8F=>Some(Opcode::SetFs),
            // FIX: 0x90 = SetGs (was mapping to PicEoi, causing 1-slot shift)
            0x90=>Some(Opcode::SetGs),
            0x91=>Some(Opcode::PicEoi),    0x92=>Some(Opcode::PicMask),
            0x93=>Some(Opcode::PicUnmask), 0x94=>Some(Opcode::PitSetHz),
            0x95=>Some(Opcode::GetTicks),  0x96=>Some(Opcode::SleepMs),
            0x97=>Some(Opcode::Cpuid),
            0x98=>Some(Opcode::RdMsr),     0x99=>Some(Opcode::WrMsr),
            0x9A=>Some(Opcode::PortBurstR),0x9B=>Some(Opcode::PortBurstW),
            0x9C=>Some(Opcode::MemQuery),
            0xA0=>Some(Opcode::RdMsr64),   0xA1=>Some(Opcode::WrMsr64),
            0xA2=>Some(Opcode::Sgdt),      0xA3=>Some(Opcode::Sidt),
            0xA4=>Some(Opcode::Rdtscp),    0xA5=>Some(Opcode::Iopl3),
            0xA6=>Some(Opcode::AsmNop),    0xA7=>Some(Opcode::AsmCli),
            0xA8=>Some(Opcode::AsmSti),    0xA9=>Some(Opcode::AsmInt),
            0xAA=>Some(Opcode::FxSave),    0xAB=>Some(Opcode::FxRstor),
            0xB0=>Some(Opcode::StrLen),    0xB1=>Some(Opcode::StrCmp),
            0xB2=>Some(Opcode::StrCat),    0xB3=>Some(Opcode::StrSub),
            0xB4=>Some(Opcode::StrFind),   0xB5=>Some(Opcode::IntToStr),
            0xB6=>Some(Opcode::StrToInt),
            0xB7=>Some(Opcode::StrUpper),  0xB8=>Some(Opcode::StrLower),
            0xB9=>Some(Opcode::PrintStr),
            0xBA=>Some(Opcode::StrByte),
            0xD0=>Some(Opcode::Assert),
            0xD1=>Some(Opcode::PrpSelfTest), 0xD2=>Some(Opcode::PrpRandom),
            0xD3=>Some(Opcode::PrpSha256File), 0xD4=>Some(Opcode::PrpSealFile),
            0xD5=>Some(Opcode::PrpOpenFile), 0xD6=>Some(Opcode::PrpSealText),
            0xD7=>Some(Opcode::PrpFingerprint), 0xD8=>Some(Opcode::PrpKeygen),
            0xD9=>Some(Opcode::PrpSign), 0xDA=>Some(Opcode::PrpVerify),
            0xDB=>Some(Opcode::PrpSealFilePub), 0xDC=>Some(Opcode::PrpOpenFilePrv),
            0xDD=>Some(Opcode::PrpSealTextPub),
            0xED=>Some(Opcode::FetchGet), 0xEE=>Some(Opcode::FetchActive),
            0xEF=>Some(Opcode::FetchTest),
            0xE0=>Some(Opcode::AsmHlt),    0xE1=>Some(Opcode::AsmPushfd), 0xE2=>Some(Opcode::AsmPopfd),
            0xE3=>Some(Opcode::AsmLidt),   0xE4=>Some(Opcode::AsmLgdt),   0xE5=>Some(Opcode::AsmWbinvd),
            0xE6=>Some(Opcode::AsmInvlpg), 0xE7=>Some(Opcode::GetCr2),    0xE8=>Some(Opcode::AsmClts),
            0xE9=>Some(Opcode::HwMapMem),
            0xEA=>Some(Opcode::PollScancode),
            0xC0=>Some(Opcode::CastInt),   0xC1=>Some(Opcode::CastBool),
            0xFF=>Some(Opcode::Nop),
            _ => None,
        }
    }

    // Pretty name for disassembler
    fn name(&self) -> &'static str {
        match self {
            Opcode::Push=>"push",       Opcode::PushStr=>"push_str",   Opcode::Pop=>"pop",
            Opcode::Dup=>"dup",         Opcode::Swap=>"swap",          Opcode::Over=>"over",
            Opcode::Add=>"add",         Opcode::Sub=>"sub",            Opcode::Mul=>"mul",
            Opcode::Div=>"div",         Opcode::Mod=>"mod",            Opcode::Neg=>"neg",
            Opcode::BAnd=>"band",       Opcode::BOr=>"bor",            Opcode::BXor=>"bxor",
            Opcode::BNot=>"bnot",       Opcode::Shl=>"shl",            Opcode::Shr=>"shr",
            Opcode::CmpEq=>"cmpeq",     Opcode::CmpNe=>"cmpne",        Opcode::CmpLt=>"cmplt",
            Opcode::CmpLe=>"cmple",     Opcode::CmpGt=>"cmpgt",        Opcode::CmpGe=>"cmpge",
            Opcode::And=>"and",         Opcode::Or=>"or",              Opcode::Not=>"not",
            Opcode::Jmp=>"jmp",         Opcode::Jz=>"jz",             Opcode::Jnz=>"jnz",
            Opcode::Call=>"call",       Opcode::Ret=>"ret",            Opcode::Halt=>"halt",
            Opcode::LoadLocal=>"load",  Opcode::StoreLocal=>"store",
            Opcode::LoadReg=>"lreg",    Opcode::StoreReg=>"sreg",
            Opcode::HeapAlloc=>"halloc",Opcode::HeapFree=>"hfree",
            Opcode::ArrLoad=>"aload",   Opcode::ArrStore=>"astore",
            Opcode::Print=>"print",     Opcode::PrintNum=>"printnum",
            Opcode::PrintChar=>"printc",Opcode::PrintHex=>"printhex",  Opcode::Input=>"input",
            Opcode::FileExist=>"fexist",Opcode::FileRead=>"fread",
            Opcode::FileWrite=>"fwrite",Opcode::FileSize=>"fsize",
            Opcode::In8=>"in8",         Opcode::Out8=>"out8",
            Opcode::In16=>"in16",       Opcode::Out16=>"out16",
            Opcode::In32=>"in32",       Opcode::Out32=>"out32",
            Opcode::Cli=>"cli",         Opcode::Sti=>"sti",            Opcode::Hlt=>"hlt",
            Opcode::Rdtsc=>"rdtsc",
            Opcode::MemCpy=>"memcpy",   Opcode::MemSet=>"memset",
            Opcode::AllocPage=>"palloc",Opcode::FreePage=>"pfree",
            Opcode::GetCr0=>"gcr0",     Opcode::SetCr0=>"scr0",
            Opcode::GetCr3=>"gcr3",     Opcode::SetCr3=>"scr3",
            Opcode::DebugBreak=>"int3",
            Opcode::Peek8=>"peek8",     Opcode::Poke8=>"poke8",
            Opcode::Peek16=>"peek16",   Opcode::Poke16=>"poke16",
            Opcode::Peek32=>"peek32",   Opcode::Poke32=>"poke32",
            Opcode::GetCs=>"gcs",       Opcode::GetDs=>"gds",          Opcode::GetEs=>"ges",
            Opcode::GetFs=>"gfs",       Opcode::GetGs=>"ggs",          Opcode::GetSs=>"gss",
            Opcode::SetFs=>"sfs",       Opcode::SetGs=>"sgs",
            Opcode::PicEoi=>"pic_eoi",  Opcode::PicMask=>"pic_mask",   Opcode::PicUnmask=>"pic_unmask",
            Opcode::PitSetHz=>"pit_hz", Opcode::GetTicks=>"ticks",     Opcode::SleepMs=>"sleep",
            Opcode::Cpuid=>"cpuid",
            Opcode::RdMsr=>"rdmsr",     Opcode::WrMsr=>"wrmsr",
            Opcode::PortBurstR=>"insb", Opcode::PortBurstW=>"outsb",   Opcode::MemQuery=>"memqry",
            Opcode::RdMsr64=>"rdmsr64", Opcode::WrMsr64=>"wrmsr64",
            Opcode::Sgdt=>"sgdt",       Opcode::Sidt=>"sidt",
            Opcode::Rdtscp=>"rdtscp",   Opcode::Iopl3=>"iopl3",
            Opcode::AsmNop=>"anop",     Opcode::AsmCli=>"acli",        Opcode::AsmSti=>"asti",
            Opcode::AsmInt=>"aint",
            Opcode::FxSave=>"fxsave",   Opcode::FxRstor=>"fxrstor",
            Opcode::StrLen=>"strlen",   Opcode::StrCmp=>"strcmp",      Opcode::StrCat=>"strcat",
            Opcode::StrSub=>"strsub",   Opcode::StrFind=>"strfind",
            Opcode::IntToStr=>"itos",   Opcode::StrToInt=>"stoi",
            Opcode::StrUpper=>"strupr", Opcode::StrLower=>"strlwr",
            Opcode::PrintStr=>"printstr",
            Opcode::StrByte=>"strbyte",
            Opcode::CastInt=>"cint",    Opcode::CastBool=>"cbool",
            Opcode::Assert=>"assert",
            Opcode::PrpSelfTest=>"prp_selftest", Opcode::PrpRandom=>"prp_random",
            Opcode::PrpSha256File=>"prp_sha256_file", Opcode::PrpSealFile=>"prp_seal_file",
            Opcode::PrpOpenFile=>"prp_open_file", Opcode::PrpSealText=>"prp_seal_text",
            Opcode::PrpFingerprint=>"prp_fingerprint", Opcode::PrpKeygen=>"prp_keygen",
            Opcode::PrpSign=>"prp_sign", Opcode::PrpVerify=>"prp_verify",
            Opcode::PrpSealFilePub=>"prp_seal_file_pub", Opcode::PrpOpenFilePrv=>"prp_open_file_prv",
            Opcode::PrpSealTextPub=>"prp_seal_text_pub",
            Opcode::FetchGet=>"fetch_get", Opcode::FetchActive=>"fetch_active",
            Opcode::FetchTest=>"fetch_test",
            Opcode::AsmHlt=>"asm_hlt",       Opcode::AsmPushfd=>"asm_pushfd",
            Opcode::AsmPopfd=>"asm_popfd",    Opcode::AsmLidt=>"asm_lidt",
            Opcode::AsmLgdt=>"asm_lgdt",      Opcode::AsmWbinvd=>"asm_wbinvd",
            Opcode::AsmInvlpg=>"asm_invlpg",  Opcode::GetCr2=>"get_cr2",
            Opcode::AsmClts=>"asm_clts",      Opcode::HwMapMem=>"hw_map_mem",
            Opcode::PollScancode=>"poll_scancode",
            Opcode::Nop=>"nop",
        }
    }

    // How many immediate bytes follow this opcode in the bytecode stream
    fn imm_bytes(&self) -> u8 {
        match self {
            Opcode::Push | Opcode::LoadLocal | Opcode::StoreLocal => 4,
            Opcode::PushStr => 2,
            Opcode::Jmp | Opcode::Jz | Opcode::Jnz | Opcode::Call => 4,
            Opcode::LoadReg | Opcode::StoreReg => 1,
            Opcode::AsmInt => 1,
            Opcode::PrpSelfTest | Opcode::PrpRandom | Opcode::PrpSha256File |
            Opcode::PrpSealFile | Opcode::PrpOpenFile | Opcode::PrpSealText |
            Opcode::PrpFingerprint | Opcode::PrpKeygen | Opcode::PrpSign |
            Opcode::PrpVerify | Opcode::PrpSealFilePub | Opcode::PrpOpenFilePrv |
            Opcode::PrpSealTextPub => 1,
            Opcode::FetchGet | Opcode::FetchActive | Opcode::FetchTest => 1,
            _ => 0,
        }
    }
}

// ============================================================================
//  SECTION 3 - DIAGNOSTICS ENGINE  (v3 snippets + v4 new codes)
// ============================================================================

#[derive(Clone, Debug, PartialEq)]
enum DiagLevel { Error, Warning, Note, Hint }

#[derive(Clone, Debug)]
struct Span {
    line:  usize,
    col:   usize,
    len:   usize,
    label: String,
}
impl Span {
    fn new(line: usize, col: usize, len: usize, label: &str) -> Self {
        Span { line, col, len: len.max(1), label: label.into() }
    }
}

#[derive(Clone, Debug)]
struct Diag {
    level:   DiagLevel,
    code:    u32,
    primary: Span,
    message: String,
    help:    Option<String>,
    note:    Option<String>,
}
impl Diag {
    fn new(lv: DiagLevel, code: u32, line: usize, col: usize, len: usize,
           msg: &str, label: &str) -> Self {
        Diag { level: lv, code,
               primary: Span::new(line, col, len, label),
               message: msg.into(), help: None, note: None }
    }
    fn error(c: u32, l: usize, col: usize, len: usize, m: &str, lb: &str) -> Self {
        Diag::new(DiagLevel::Error, c, l, col, len, m, lb)
    }
    fn warning(c: u32, l: usize, col: usize, len: usize, m: &str, lb: &str) -> Self {
        Diag::new(DiagLevel::Warning, c, l, col, len, m, lb)
    }
    fn help(mut self, h: &str) -> Self  { self.help = Some(h.into()); self }
    fn note(mut self, n: &str) -> Self  { self.note = Some(n.into()); self }
}

fn source_line<'a>(src: &'a str, target: usize) -> &'a str {
    let mut n = 1;
    for line in src.split('\n') {
        if n == target { return line; }
        n += 1;
    }
    ""
}

struct DiagEngine {
    diags:    Vec<Diag>,
    errors:   usize,
    warnings: usize,
    source:   String,
}
impl DiagEngine {
    fn new(src: &str) -> Self {
        DiagEngine { diags: Vec::new(), errors: 0, warnings: 0, source: src.into() }
    }
    fn push(&mut self, d: Diag) {
        match d.level {
            DiagLevel::Error   => self.errors   += 1,
            DiagLevel::Warning => self.warnings += 1,
            _ => {}
        }
        self.diags.push(d);
    }
    fn has_errors(&self)   -> bool { self.errors > 0 }
    fn has_warnings(&self) -> bool { self.warnings > 0 }

    unsafe fn emit_all(&self) {
        for d in &self.diags { self.emit_one(d); }
        self.emit_summary();
    }

    unsafe fn emit_summary(&self) {
        if self.errors == 0 && self.warnings == 0 { return; }
        kputc(b'\n');
        if self.errors > 0 {
            terminal_setcolor(COL_RED);
            kprint(b"error: aborting due to ");
            kprint_i32(self.errors as i32); kprint(b" error");
            if self.errors != 1 { kputc(b's'); }
            if self.warnings > 0 {
                terminal_setcolor(COL_WHITE); kprint(b"; ");
                terminal_setcolor(COL_YELLOW);
                kprint_i32(self.warnings as i32); kprint(b" warning");
                if self.warnings != 1 { kputc(b's'); }
                kprint(b" emitted");
            }
        } else {
            terminal_setcolor(COL_YELLOW);
            kprint_i32(self.warnings as i32); kprint(b" warning");
            if self.warnings != 1 { kputc(b's'); }
            kprint(b" emitted");
        }
        terminal_setcolor(COL_WHITE); kputc(b'\n');
    }

    unsafe fn emit_one(&self, d: &Diag) {
        let (pfx, letter, col) = match d.level {
            DiagLevel::Error   => (b"error"   as &[u8], b'E', COL_RED),
            DiagLevel::Warning => (b"warning" as &[u8], b'W', COL_YELLOW),
            DiagLevel::Note    => (b"note"    as &[u8], b'N', COL_BLUE),
            DiagLevel::Hint    => (b"hint"    as &[u8], b'H', COL_CYAN),
        };

        terminal_setcolor(col);
        kprint(pfx); kprint(b"["); kputc(letter);
        if d.code < 100 { kputc(b'0'); }
        if d.code < 10  { kputc(b'0'); }
        kprint_i32(d.code as i32);
        kprint(b"]: ");
        terminal_setcolor(COL_WHITE);
        kprint_str(&d.message);
        kputc(b'\n');

        terminal_setcolor(COL_BLUE);
        kprint(b"  --> ");
        terminal_setcolor(COL_WHITE);
        kprint(b"helios:");
        kprint_i32(d.primary.line as i32);
        kputc(b':');
        kprint_i32(d.primary.col as i32);
        kputc(b'\n');

        if d.primary.line > 0 && !self.source.is_empty() {
            let line_str = source_line(&self.source, d.primary.line);
            let lnum_str = format!("{}", d.primary.line);
            let gutter_w = lnum_str.len() + 1;

            terminal_setcolor(COL_BLUE);
            for _ in 0..gutter_w { kputc(b' '); }
            kprint(b"|");
            terminal_setcolor(COL_WHITE); kputc(b'\n');

            terminal_setcolor(COL_BLUE);
            kprint_str(&lnum_str); kputc(b' '); kprint(b"| ");
            terminal_setcolor(COL_WHITE);
            kprint_str(line_str); kputc(b'\n');

            terminal_setcolor(COL_BLUE);
            for _ in 0..gutter_w { kputc(b' '); }
            kprint(b"| ");
            let caret_start = if d.primary.col > 0 { d.primary.col - 1 } else { 0 };
            for _ in 0..caret_start { kputc(b' '); }
            terminal_setcolor(col);
            let caret_len = d.primary.len.min(line_str.len().saturating_sub(caret_start));
            for _ in 0..caret_len.max(1) { kputc(b'^'); }
            kputc(b' ');
            terminal_setcolor(COL_WHITE);
            kprint_str(&d.primary.label); kputc(b'\n');

            terminal_setcolor(COL_BLUE);
            for _ in 0..gutter_w { kputc(b' '); }
            kprint(b"|");
            terminal_setcolor(COL_WHITE); kputc(b'\n');
        }

        if let Some(ref h) = d.help {
            terminal_setcolor(COL_CYAN);  kprint(b"  = help: ");
            terminal_setcolor(COL_WHITE); kprint_str(h); kputc(b'\n');
        }
        if let Some(ref n) = d.note {
            terminal_setcolor(COL_BLUE);  kprint(b"  = note: ");
            terminal_setcolor(COL_WHITE); kprint_str(n); kputc(b'\n');
        }
        kputc(b'\n');
    }
}

// ============================================================================
//  SECTION 4 - LEXER
// ============================================================================

#[derive(Clone, PartialEq, Debug)]
enum TokenKind {
    IntLit(i32), StrLit(String), BoolLit(bool), Ident(String),
    // Keywords
    Fn, Let, Mut, Return, If, Else, For, While, Loop, In, Break, Continue,
    Match, Struct, New, Unsafe, Step, Using,
    Print, Input, Asm,
    Const, Assert,   // v5
    // Types
    TInt, TStr, TBool, TVoid, TPtr,
    // Arithmetic
    Plus, Minus, Star, Slash, Percent,
    // Bitwise
    Amp, Pipe, Caret, Tilde, LtLt, GtGt,
    // Comparison
    EqEq, BangEq, Lt, LtEq, Gt, GtEq,
    // Logical
    AmpAmp, PipePipe, Bang,
    // Assignment
    Eq,
    PlusEq, MinusEq, StarEq, SlashEq, PercentEq,
    AmpEq, PipeEq, CaretEq,
    // Punctuation
    LBrace, RBrace, LParen, RParen, LBracket, RBracket,
    Semi, Comma, Dot, Arrow, FatArrow, Colon, ColonColon,
    DotDot, DotDotEq, Underscore,
    // Cast
    As,
    Eof,
    Import,
}

#[derive(Clone, Debug)]
struct Token { kind: TokenKind, line: usize, col: usize, len: usize }

struct Lexer<'a> { src: &'a [u8], pos: usize, line: usize, col: usize }
impl<'a> Lexer<'a> {
    fn new(src: &'a [u8]) -> Self { Lexer { src, pos: 0, line: 1, col: 1 } }
    fn peek(&self)  -> u8 { *self.src.get(self.pos).unwrap_or(&0) }
    fn peek2(&self) -> u8 { *self.src.get(self.pos + 1).unwrap_or(&0) }
    fn peek3(&self) -> u8 { *self.src.get(self.pos + 2).unwrap_or(&0) }
    fn advance(&mut self) -> u8 {
        let c = self.peek();
        if c == b'\n' { self.line += 1; self.col = 1; } else { self.col += 1; }
        self.pos += 1; c
    }
    fn skip_ws(&mut self) {
        loop {
            while self.pos < self.src.len() && self.peek() <= b' ' { self.advance(); }
            if self.peek() == b'/' && self.peek2() == b'/' {
                while self.peek() != b'\n' && self.peek() != 0 { self.advance(); }
            } else if self.peek() == b'/' && self.peek2() == b'*' {
                self.advance(); self.advance();
                loop {
                    if self.peek() == 0 { break; }
                    if self.peek() == b'*' && self.peek2() == b'/' {
                        self.advance(); self.advance(); break;
                    }
                    self.advance();
                }
            } else { break; }
        }
    }

    fn lex_str(&mut self) -> Token {
        let (l, c) = (self.line, self.col);
        self.advance(); // opening "
        let mut s = String::new();
        loop {
            let ch = self.advance();
            if ch == b'"' || ch == 0 { break; }
            if ch == b'\\' {
                match self.advance() {
                    b'n'  => s.push('\n'), b't' => s.push('\t'),
                    b'r'  => s.push('\r'), b'\\' => s.push('\\'),
                    b'"'  => s.push('"'),  b'0'  => s.push('\0'),
                    b'x'  => {
                        let hi = self.advance();
                        let lo = self.advance();
                        s.push((hex_byte(hi) * 16 + hex_byte(lo)) as char);
                    }
                    b'u'  => {
                        // \uXXXX — 4 hex digits, encode as UTF-8
                        let mut cp: u32 = 0;
                        for _ in 0..4 { cp = (cp << 4) | hex_byte(self.advance()) as u32; }
                        if let Some(c) = char::from_u32(cp) { s.push(c); }
                    }
                    e => { s.push('\\'); s.push(e as char); }
                }
            } else { s.push(ch as char); }
        }
        Token { kind: TokenKind::StrLit(s), line: l, col: c, len: 1 }
    }

    // Character literal: 'A' -> IntLit(65)  (NEW v4)
    fn lex_char(&mut self) -> Token {
        let (l, c) = (self.line, self.col);
        self.advance(); // opening '
        let ch = if self.peek() == b'\\' {
            self.advance();
            match self.advance() {
                b'n' => b'\n', b't' => b'\t', b'r' => b'\r',
                b'0' => 0,     b'\\'=> b'\\', b'\'' => b'\'',
                b'x' => {
                    let hi = self.advance(); let lo = self.advance();
                    hex_byte(hi) * 16 + hex_byte(lo)
                }
                e => e,
            }
        } else {
            self.advance()
        };
        if self.peek() == b'\'' { self.advance(); } // closing '
        Token { kind: TokenKind::IntLit(ch as i32), line: l, col: c, len: 1 }
    }

    fn lex_num(&mut self) -> Token {
        let (l, c) = (self.line, self.col);
        let neg = self.peek() == b'-';
        if neg { self.advance(); }
        // hex
        if self.peek() == b'0' && (self.peek2() == b'x' || self.peek2() == b'X') {
            self.advance(); self.advance();
            let mut n: i32 = 0;
            let mut len = 0usize;
            while self.peek().is_ascii_hexdigit() {
                let d = self.advance();
                let v = if d >= b'a' { d-b'a'+10 } else if d >= b'A' { d-b'A'+10 } else { d-b'0' };
                n = (n << 4) | v as i32;
                len += 1;
            }
            return Token { kind: TokenKind::IntLit(if neg { -n } else { n }), line: l, col: c, len: len+2 };
        }
        // binary
        if self.peek() == b'0' && (self.peek2() == b'b' || self.peek2() == b'B') {
            self.advance(); self.advance();
            let mut n: i32 = 0;
            while self.peek() == b'0' || self.peek() == b'1' {
                n = (n << 1) | (self.advance() - b'0') as i32;
            }
            return Token { kind: TokenKind::IntLit(if neg { -n } else { n }), line: l, col: c, len: 1 };
        }
        let mut n: i32 = 0;
        let mut len = 0usize;
        while self.peek().is_ascii_digit() {
            n = n.wrapping_mul(10).wrapping_add((self.advance() - b'0') as i32);
            len += 1;
        }
        // Ignore trailing type suffix like 'u8', 'i32' (v4: accept but discard)
        while self.peek().is_ascii_alphabetic() || self.peek() == b'_' { self.advance(); }
        Token { kind: TokenKind::IntLit(if neg { -n } else { n }), line: l, col: c, len: len }
    }

    fn lex_ident(&mut self) -> Token {
        let (l, c) = (self.line, self.col);
        let mut s = String::new();
        while self.peek().is_ascii_alphanumeric() || self.peek() == b'_' {
            s.push(self.advance() as char);
        }
        let len = s.len();
        let kind = match s.as_str() {
            "fn"       => TokenKind::Fn,
            "let"      => TokenKind::Let,
            "mut"      => TokenKind::Mut,
            "return"   => TokenKind::Return,
            "if"       => TokenKind::If,
            "else"     => TokenKind::Else,
            "for"      => TokenKind::For,
            "while"    => TokenKind::While,
            "loop"     => TokenKind::Loop,
            "in"       => TokenKind::In,
            "break"    => TokenKind::Break,
            "continue" => TokenKind::Continue,
            "match"    => TokenKind::Match,
            "struct"   => TokenKind::Struct,
            "new"      => TokenKind::New,
            "unsafe"   => TokenKind::Unsafe,
            "step"     => TokenKind::Step,
            "using"    => TokenKind::Using,
            "print"    => TokenKind::Print,
            "input"    => TokenKind::Input,
            "asm"      => TokenKind::Asm,
            "const"    => TokenKind::Const,    // v5
            "assert"   => TokenKind::Assert,   // v5
            "int"      => TokenKind::TInt,
            "str"      => TokenKind::TStr,
            "bool"     => TokenKind::TBool,
            "void"     => TokenKind::TVoid,
            "ptr"      => TokenKind::TPtr,
            "true"     => TokenKind::BoolLit(true),
            "false"    => TokenKind::BoolLit(false),
            "as"       => TokenKind::As,
            "_"        => TokenKind::Underscore,
            _          => TokenKind::Ident(s),
        };
        Token { kind, line: l, col: c, len }
    }

    fn next(&mut self) -> Token {
        self.skip_ws();
        let (l, c) = (self.line, self.col);
        let ch = self.peek();
        if ch == 0   { return Token { kind: TokenKind::Eof, line: l, col: c, len: 0 }; }
        if ch == b'"' { return self.lex_str(); }
        if ch == b'\'' { return self.lex_char(); }
        if ch.is_ascii_digit() { return self.lex_num(); }
        if ch == b'-' && self.peek2().is_ascii_digit() { return self.lex_num(); }
        if ch.is_ascii_alphabetic() || ch == b'_' { return self.lex_ident(); }

        self.advance();
        let kind = match ch {
            b'+' => match self.peek() { b'=' => { self.advance(); TokenKind::PlusEq }    _ => TokenKind::Plus },
            b'-' => match self.peek() { b'=' => { self.advance(); TokenKind::MinusEq }
                                        b'>' => { self.advance(); TokenKind::Arrow }      _ => TokenKind::Minus },
            b'*' => match self.peek() { b'=' => { self.advance(); TokenKind::StarEq }     _ => TokenKind::Star },
            b'/' => match self.peek() { b'=' => { self.advance(); TokenKind::SlashEq }    _ => TokenKind::Slash },
            b'%' => match self.peek() { b'=' => { self.advance(); TokenKind::PercentEq }  _ => TokenKind::Percent },
            b'&' => match self.peek() { b'&' => { self.advance(); TokenKind::AmpAmp }
                                        b'=' => { self.advance(); TokenKind::AmpEq }      _ => TokenKind::Amp },
            b'|' => match self.peek() { b'|' => { self.advance(); TokenKind::PipePipe }
                                        b'=' => { self.advance(); TokenKind::PipeEq }     _ => TokenKind::Pipe },
            b'^' => match self.peek() { b'=' => { self.advance(); TokenKind::CaretEq }    _ => TokenKind::Caret },
            b'~' => TokenKind::Tilde,
            b'!' => match self.peek() { b'=' => { self.advance(); TokenKind::BangEq }     _ => TokenKind::Bang },
            b'=' => match self.peek() { b'=' => { self.advance(); TokenKind::EqEq }
                                        b'>' => { self.advance(); TokenKind::FatArrow }   _ => TokenKind::Eq },
            b'<' => match self.peek() { b'<' => { self.advance(); TokenKind::LtLt }
                                        b'=' => { self.advance(); TokenKind::LtEq }       _ => TokenKind::Lt },
            b'>' => match self.peek() { b'>' => { self.advance(); TokenKind::GtGt }
                                        b'=' => { self.advance(); TokenKind::GtEq }       _ => TokenKind::Gt },
            b'.' => match self.peek() {
                b'.' => { self.advance();
                    if self.peek() == b'=' { self.advance(); TokenKind::DotDotEq }
                    else { TokenKind::DotDot }
                }
                _ => TokenKind::Dot
            },
            b':' => match self.peek() { b':' => { self.advance(); TokenKind::ColonColon } _ => TokenKind::Colon },
            b'{' => TokenKind::LBrace,  b'}' => TokenKind::RBrace,
            b'(' => TokenKind::LParen,  b')' => TokenKind::RParen,
            b'[' => TokenKind::LBracket, b']' => TokenKind::RBracket,
            b';' => TokenKind::Semi,    b',' => TokenKind::Comma,
            _    => return self.next(), // skip unknown chars
        };
        Token { kind, line: l, col: c, len: 1 }
    }

    fn tokenise(mut self) -> Vec<Token> {
        // Pre-allocate a reasonable capacity to avoid repeated reallocations
        let estimated = (self.src.len() / 4).max(64);
        let mut v = Vec::with_capacity(estimated);
        loop {
            let t = self.next();
            let done = t.kind == TokenKind::Eof;
            v.push(t);
            if done { break; }
        }
        v
    }
}

// ============================================================================
//  SECTION 5 - AST
// ============================================================================

#[derive(Clone, Debug)]
enum Expr {
    IntLit(i32),
    StrLit(String),
    BoolLit(bool),
    Var(String),
    BinOp(Box<Expr>, BinOpKind, Box<Expr>),
    UnOp(UnOpKind, Box<Expr>),
    Call(String, Vec<Expr>),
    Cast(Box<Expr>, CastTarget),
    ArrayLit(Vec<Expr>),
    ArrayFill(Box<Expr>, usize),
    Index(Box<Expr>, Box<Expr>),
    Field(Box<Expr>, String),        // base.field_name
    FieldChain(Box<Expr>, Vec<String>), // base.a.b.c  (v4: pre-flattened)
    Deref(Box<Expr>),
    AddrOf(String),
    Input,
    InlineAsm(String),
}

#[derive(Clone, Debug)]
enum BinOpKind {
    Add, Sub, Mul, Div, Mod,
    Eq, Ne, Lt, Le, Gt, Ge,
    And, Or,
    BAnd, BOr, BXor, Shl, Shr,
}

#[derive(Clone, Debug)]
enum UnOpKind { Neg, Not, BNot }

#[derive(Clone, Debug)]
enum CastTarget { Int, Bool }

#[derive(Clone, Debug)]
enum RangeKind { Exclusive, Inclusive }

#[derive(Clone, Debug)]
struct MatchArm {
    pattern: MatchPat,
    body:    Vec<Stmt>,
}
#[derive(Clone, Debug)]
enum MatchPat {
    Lit(i32),
    StrLit(String),
    Wildcard,
}

#[derive(Clone, Debug)]
enum Stmt {
    Let(String, Expr, usize, usize),
    LetArr(String, usize, Expr, usize, usize),
    Assign(String, Expr),
    AssignIdx(String, Expr, Expr),
    CompoundAssign(String, BinOpKind, Expr),
    If(Expr, Vec<Stmt>, Vec<Stmt>),
    While(Expr, Vec<Stmt>),
    Loop(Vec<Stmt>),
    For { var: String, start: Expr, end: Expr, step: Option<Expr>, kind: RangeKind, body: Vec<Stmt> },
    Break,
    BreakVal(Expr),   // v4: break with value
    Continue,
    Return(Expr, usize),
    Print(Expr),
    PrintHex(Expr),
    Expr(Expr, usize, usize),
    Match(Expr, Vec<MatchArm>),
    UnsafeBlock(Vec<Stmt>),
    AsmStmt(String),
    AssertStmt(Expr, Option<String>, usize), // v5: assert!(cond) or assert!(cond, "msg")
    ConstDef(String, i32),                  // v5: top-level const hoisted into fn scope
}

// v5: top-level constant definition (const FOO: int = 42;)
#[derive(Clone, Debug)]
struct ConstDef {
    name:  String,
    value: i32,
    line:  usize,
}

#[derive(Clone, Debug)]
struct StructDef {
    name:   String,
    fields: Vec<String>,
    line:   usize,
}

#[derive(Clone, Debug)]
struct FnDef {
    name:     String,
    params:   Vec<String>,
    body:     Vec<Stmt>,
    ret_void: bool,
    line:     usize,
}

// ============================================================================
//  SECTION 6 - PARSER  (v4: new keywords, `-> void` optional, new diagnostics)
// ============================================================================

struct Parser {
    tokens:    Vec<Token>,
    pos:       usize,
    diags:     DiagEngine,
    depth:     usize,
    in_loop:   usize,
    in_unsafe: usize,          // v5: track unsafe nesting depth
    structs:   Vec<StructDef>, // accumulated as we parse
    consts:    Vec<ConstDef>,  // v5: top-level constants
    prp_imports: Vec<String>,
    fetch_imports: Vec<String>,
}
impl Parser {
    fn new(t: Vec<Token>, src: &str) -> Self {
        Parser { tokens: t, pos: 0, diags: DiagEngine::new(src),
                 depth: 0, in_loop: 0, in_unsafe: 0,
                 structs: Vec::new(), consts: Vec::new(), prp_imports: Vec::new(),
                 fetch_imports: Vec::new() }
    }
    fn peek(&self) -> &TokenKind  { &self.tokens[self.pos].kind }
    fn tok(&self)  -> &Token      { &self.tokens[self.pos] }
    fn advance(&mut self) -> &Token {
        let t = &self.tokens[self.pos];
        if self.pos + 1 < self.tokens.len() { self.pos += 1; }
        t
    }
    fn tok_len(&self) -> usize { self.tokens[self.pos].len }

    fn expect(&mut self, k: &TokenKind) -> bool {
        if core::mem::discriminant(self.peek()) == core::mem::discriminant(k) {
            self.advance(); true
        } else {
            let t = self.tok().clone();
            self.diags.push(
                Diag::error(5, t.line, t.col, t.len,
                    &format!("expected `{:?}`, found `{:?}`", k, self.peek()),
                    "unexpected token")
                .help("check for a missing delimiter or semicolon")
            );
            false
        }
    }
    fn eat_semi(&mut self) {
        if matches!(self.peek(), TokenKind::Semi) { self.advance(); }
        else {
            let t = self.tok().clone();
            self.diags.push(
                Diag::error(6, t.line, t.col, 1,
                    "expected `;` at end of statement", "insert `;` here")
                .help("every Helios statement must end with a semicolon")
            );
        }
    }
    fn eat_ident(&mut self, ctx: &str) -> String {
        if let TokenKind::Ident(n) = self.peek().clone() { self.advance(); n }
        else {
            let t = self.tok().clone();
            self.diags.push(
                Diag::error(10, t.line, t.col, t.len,
                    &format!("expected identifier {}", ctx), "here")
            );
            String::from("<error>")
        }
    }

    // ── Expression parsing ────────────────────────────────────────────────────
    fn parse_primary(&mut self) -> Expr {
        let t = self.tok().clone();
        match t.kind.clone() {
            TokenKind::IntLit(n)  => { self.advance(); Expr::IntLit(n) }
            TokenKind::StrLit(s)  => { self.advance(); Expr::StrLit(s) }
            TokenKind::BoolLit(b) => { self.advance(); Expr::BoolLit(b) }
            TokenKind::Ident(nm)  => {
                self.advance();
                if matches!(self.peek(), TokenKind::LParen) {
                    self.advance();
                    let mut args = Vec::new();
                    while !matches!(self.peek(), TokenKind::RParen | TokenKind::Eof) {
                        args.push(self.parse_expr());
                        if matches!(self.peek(), TokenKind::Comma) { self.advance(); }
                    }
                    self.expect(&TokenKind::RParen);
                    Expr::Call(nm, args)
                } else {
                    Expr::Var(nm)
                }
            }
            TokenKind::Input => {
                self.advance();
                if matches!(self.peek(), TokenKind::LParen) { self.advance(); self.expect(&TokenKind::RParen); }
                Expr::Input
            }
            TokenKind::LParen => {
                self.advance();
                let e = self.parse_expr();
                self.expect(&TokenKind::RParen);
                e
            }
            TokenKind::LBracket => {
                self.advance();
                // [val; N] fill  or  [a, b, c] literal
                let first = self.parse_expr();
                if matches!(self.peek(), TokenKind::Semi) {
                    self.advance();
                    let count = if let TokenKind::IntLit(n) = self.peek().clone() {
                        self.advance(); n as usize
                    } else { 0 };
                    self.expect(&TokenKind::RBracket);
                    Expr::ArrayFill(Box::new(first), count)
                } else {
                    let mut items = vec![first];
                    while matches!(self.peek(), TokenKind::Comma) {
                        self.advance();
                        if matches!(self.peek(), TokenKind::RBracket) { break; }
                        items.push(self.parse_expr());
                    }
                    // W601: large array literal
                    if items.len() > 256 {
                        self.diags.push(
                            Diag::warning(601, t.line, t.col, 1,
                                &format!("large array literal ({} elements) may cause heap pressure", items.len()),
                                "consider allocating in smaller chunks")
                        );
                    }
                    self.expect(&TokenKind::RBracket);
                    Expr::ArrayLit(items)
                }
            }
            TokenKind::Star => {
                self.advance();
                Expr::Deref(Box::new(self.parse_primary()))
            }
            TokenKind::Amp => {
                self.advance();
                let nm = self.eat_ident("after `&`");
                Expr::AddrOf(nm)
            }
            TokenKind::Minus => {
                self.advance();
                Expr::UnOp(UnOpKind::Neg, Box::new(self.parse_primary()))
            }
            TokenKind::Bang => {
                self.advance();
                Expr::UnOp(UnOpKind::Not, Box::new(self.parse_primary()))
            }
            TokenKind::Tilde => {
                self.advance();
                Expr::UnOp(UnOpKind::BNot, Box::new(self.parse_primary()))
            }
            TokenKind::Asm => {
                self.advance();
                self.expect(&TokenKind::Bang);
                self.expect(&TokenKind::LParen);
                let body = if let TokenKind::StrLit(s) = self.peek().clone() { self.advance(); s } else { String::new() };
                self.expect(&TokenKind::RParen);
                Expr::InlineAsm(body)
            }
            TokenKind::New => {
                // new StructName { field: val, ... }  ->  array allocation in order
                self.advance();
                let _name = self.eat_ident("struct name after `new`");
                self.expect(&TokenKind::LBrace);
                let mut vals = Vec::new();
                while !matches!(self.peek(), TokenKind::RBrace | TokenKind::Eof) {
                    let _field = self.eat_ident("field name");
                    self.expect(&TokenKind::Colon);
                    vals.push(self.parse_expr());
                    if matches!(self.peek(), TokenKind::Comma) { self.advance(); }
                }
                self.expect(&TokenKind::RBrace);
                Expr::ArrayLit(vals)
            }
            _ => {
                let t2 = self.tok().clone();
                self.diags.push(
                    Diag::error(7, t2.line, t2.col, t2.len,
                        &format!("unexpected token in expression: `{:?}`", t2.kind),
                        "invalid start of expression")
                    .help("valid starts: integer, string, bool, variable name, `(`, `[`, `*`, `&`, `!`, `~`")
                );
                self.advance();
                Expr::IntLit(0)
            }
        }
    }

    fn parse_postfix(&mut self) -> Expr {
        let mut e = self.parse_primary();
        loop {
            match self.peek().clone() {
                TokenKind::LBracket => {
                    self.advance();
                    let idx = self.parse_expr();
                    self.expect(&TokenKind::RBracket);
                    e = Expr::Index(Box::new(e), Box::new(idx));
                }
                TokenKind::Dot => {
                    self.advance();
                    // Flatten a.b.c into a single FieldChain for efficient emitting
                    let field = self.eat_ident("field name after `.`");
                    let mut chain = vec![field];
                    while matches!(self.peek(), TokenKind::Dot) {
                        self.advance();
                        chain.push(self.eat_ident("field name after `.`"));
                    }
                    if chain.len() == 1 {
                        e = Expr::Field(Box::new(e), chain.remove(0));
                    } else {
                        e = Expr::FieldChain(Box::new(e), chain);
                    }
                }
                _ => break,
            }
        }
        e
    }

    fn parse_unary(&mut self) -> Expr {
        match self.peek().clone() {
            TokenKind::Minus => { self.advance(); Expr::UnOp(UnOpKind::Neg, Box::new(self.parse_postfix())) }
            TokenKind::Bang  => { self.advance(); Expr::UnOp(UnOpKind::Not, Box::new(self.parse_postfix())) }
            TokenKind::Tilde => { self.advance(); Expr::UnOp(UnOpKind::BNot, Box::new(self.parse_postfix())) }
            TokenKind::Star  => { self.advance(); Expr::Deref(Box::new(self.parse_postfix())) }
            _ => self.parse_postfix(),
        }
    }
    fn parse_mul(&mut self) -> Expr {
        let mut l = self.parse_unary();
        loop {
            let op = match self.peek() {
                TokenKind::Star    => BinOpKind::Mul,
                TokenKind::Slash   => BinOpKind::Div,
                TokenKind::Percent => BinOpKind::Mod,
                _ => break,
            }; self.advance();
            l = Expr::BinOp(Box::new(l), op, Box::new(self.parse_unary()));
        } l
    }
    fn parse_add(&mut self) -> Expr {
        let mut l = self.parse_mul();
        loop {
            let op = match self.peek() {
                TokenKind::Plus  => BinOpKind::Add,
                TokenKind::Minus => BinOpKind::Sub,
                _ => break,
            }; self.advance();
            l = Expr::BinOp(Box::new(l), op, Box::new(self.parse_mul()));
        } l
    }
    fn parse_shift(&mut self) -> Expr {
        let mut l = self.parse_add();
        loop {
            let op = match self.peek() {
                TokenKind::LtLt => BinOpKind::Shl,
                TokenKind::GtGt => BinOpKind::Shr,
                _ => break,
            }; self.advance();
            l = Expr::BinOp(Box::new(l), op, Box::new(self.parse_add()));
        } l
    }
    fn parse_bitand(&mut self) -> Expr {
        let mut l = self.parse_shift();
        while matches!(self.peek(), TokenKind::Amp) {
            self.advance();
            l = Expr::BinOp(Box::new(l), BinOpKind::BAnd, Box::new(self.parse_shift()));
        } l
    }
    fn parse_bitxor(&mut self) -> Expr {
        let mut l = self.parse_bitand();
        while matches!(self.peek(), TokenKind::Caret) {
            self.advance();
            l = Expr::BinOp(Box::new(l), BinOpKind::BXor, Box::new(self.parse_bitand()));
        } l
    }
    fn parse_bitor(&mut self) -> Expr {
        let mut l = self.parse_bitxor();
        while matches!(self.peek(), TokenKind::Pipe) {
            self.advance();
            l = Expr::BinOp(Box::new(l), BinOpKind::BOr, Box::new(self.parse_bitxor()));
        } l
    }
    fn parse_cmp(&mut self) -> Expr {
        let mut l = self.parse_bitor();
        loop {
            let op = match self.peek() {
                TokenKind::EqEq  => BinOpKind::Eq,  TokenKind::BangEq => BinOpKind::Ne,
                TokenKind::Lt    => BinOpKind::Lt,   TokenKind::LtEq   => BinOpKind::Le,
                TokenKind::Gt    => BinOpKind::Gt,   TokenKind::GtEq   => BinOpKind::Ge,
                _ => break,
            }; self.advance();
            l = Expr::BinOp(Box::new(l), op, Box::new(self.parse_bitor()));
        } l
    }
    fn parse_logic(&mut self) -> Expr {
        let mut l = self.parse_cmp();
        loop {
            let op = match self.peek() {
                TokenKind::AmpAmp   => BinOpKind::And,
                TokenKind::PipePipe => BinOpKind::Or,
                _ => break,
            }; self.advance();
            l = Expr::BinOp(Box::new(l), op, Box::new(self.parse_cmp()));
        } l
    }
    fn parse_cast(&mut self) -> Expr {
        let mut e = self.parse_logic();
        while matches!(self.peek(), TokenKind::As) {
            self.advance();
            let target = match self.peek() {
                TokenKind::TInt  => { self.advance(); CastTarget::Int }
                TokenKind::TBool => { self.advance(); CastTarget::Bool }
                _ => { self.advance(); CastTarget::Int }
            };
            e = Expr::Cast(Box::new(e), target);
        }
        e
    }
    fn parse_expr(&mut self) -> Expr { self.parse_cast() }

    // ── Block ─────────────────────────────────────────────────────────────────
    fn parse_block(&mut self) -> Vec<Stmt> {
        let t = self.tok().clone();
        self.expect(&TokenKind::LBrace);
        self.depth += 1;
        if self.depth >= 5 {
            self.diags.push(
                Diag::warning(301, t.line, t.col, 1,
                    &format!("deeply nested block (depth {})", self.depth),
                    "deeply nested")
                .help("extract inner logic into helper functions")
            );
        }
        // W303: empty block
        if matches!(self.peek(), TokenKind::RBrace) {
            self.diags.push(
                Diag::warning(303, t.line, t.col, 1, "empty block `{}`", "empty")
                .help("add code or a comment: `// intentionally empty`")
            );
        }
        let mut stmts = Vec::new();
        let mut after_return = false;
        while !matches!(self.peek(), TokenKind::RBrace | TokenKind::Eof) {
            if after_return {
                let t2 = self.tok().clone();
                self.diags.push(
                    Diag::warning(302, t2.line, t2.col, t2.len,
                        "unreachable code after `return`", "dead code")
                    .help("remove the dead code or move it before the `return`")
                );
            }
            let s = self.parse_stmt();
            if matches!(s, Stmt::Return(_, _)) { after_return = true; }
            stmts.push(s);
        }
        self.expect(&TokenKind::RBrace);
        self.depth -= 1;
        stmts
    }

    // ── Statement ─────────────────────────────────────────────────────────────
    fn parse_stmt(&mut self) -> Stmt {
        let t = self.tok().clone();
        match self.peek().clone() {

            TokenKind::Let => {
                self.advance();
                // v4: eat optional `mut`
                if matches!(self.peek(), TokenKind::Mut) { self.advance(); }
                // type annotation optional: `let int x` or `let x`
                let nm = match self.peek().clone() {
                    TokenKind::TInt | TokenKind::TStr | TokenKind::TBool |
                    TokenKind::TVoid | TokenKind::TPtr => {
                        self.advance();
                        self.eat_ident("variable name after type")
                    }
                    _ => self.eat_ident("variable name after `let`"),
                };
                if nm == "<error>" {
                    // recover: skip to semicolon
                    while !matches!(self.peek(), TokenKind::Semi | TokenKind::Eof) { self.advance(); }
                    self.eat_semi();
                    return Stmt::Let(nm, Expr::IntLit(0), t.line, t.col);
                }
                // Array: `let nm[N] = ...`
                if matches!(self.peek(), TokenKind::LBracket) {
                    self.advance();
                    let count = if let TokenKind::IntLit(n) = self.peek().clone() { self.advance(); n as usize } else { 0 };
                    self.expect(&TokenKind::RBracket);
                    self.expect(&TokenKind::Eq);
                    let e = self.parse_expr();
                    self.eat_semi();
                    return Stmt::LetArr(nm, count, e, t.line, t.col);
                }
                self.expect(&TokenKind::Eq);
                let e = self.parse_expr();
                self.eat_semi();
                Stmt::Let(nm, e, t.line, t.col)
            }

            TokenKind::Return => {
                self.advance();
                if matches!(self.peek(), TokenKind::Semi) {
                    self.eat_semi();
                    return Stmt::Return(Expr::IntLit(0), t.line);
                }
                let e = self.parse_expr();
                self.eat_semi();
                Stmt::Return(e, t.line)
            }

            TokenKind::If => {
                self.advance();
                // optional parens around condition
                let parens = matches!(self.peek(), TokenKind::LParen);
                if parens { self.advance(); }
                let cond = self.parse_expr();
                if parens { self.expect(&TokenKind::RParen); }

                // W201: constant condition
                if matches!(cond, Expr::BoolLit(_) | Expr::IntLit(0) | Expr::IntLit(1)) {
                    self.diags.push(
                        Diag::warning(201, t.line, t.col, 2,
                            "constant condition in `if`", "always true/false")
                        .help("use a variable or runtime expression")
                    );
                }
                let then_b = self.parse_block();
                let else_b = if matches!(self.peek(), TokenKind::Else) {
                    self.advance();
                    if matches!(self.peek(), TokenKind::If) { vec![self.parse_stmt()] }
                    else { self.parse_block() }
                } else { Vec::new() };
                Stmt::If(cond, then_b, else_b)
            }

            TokenKind::While => {
                self.advance();
                let parens = matches!(self.peek(), TokenKind::LParen);
                if parens { self.advance(); }
                let cond = self.parse_expr();
                if parens { self.expect(&TokenKind::RParen); }
                self.in_loop += 1;
                let body = self.parse_block();
                self.in_loop -= 1;
                Stmt::While(cond, body)
            }

            TokenKind::Loop => {
                self.advance();
                self.in_loop += 1;
                let body = self.parse_block();
                self.in_loop -= 1;
                // W202: loop without break
                if !Self::body_has_break_static(&body) {
                    self.diags.push(
                        Diag::warning(202, t.line, t.col, 4,
                            "infinite `loop` with no reachable `break`", "loops forever")
                        .help("add `break;` inside the loop or convert to `while (cond) { }`")
                    );
                }
                Stmt::Loop(body)
            }

            TokenKind::For => {
                self.advance();
                let var = self.eat_ident("loop variable after `for`");
                if var == "<error>" {
                    self.diags.push(
                        Diag::error(20, t.line, t.col, 3, "missing loop variable after `for`", "here")
                    );
                }
                if !matches!(self.peek(), TokenKind::In) {
                    self.diags.push(
                        Diag::error(21, t.line, t.col, 3, "missing `in` keyword in `for` loop", "expected `in`")
                    );
                } else { self.advance(); }
                let start = self.parse_expr();
                let kind = match self.peek() {
                    TokenKind::DotDot    => { self.advance(); RangeKind::Exclusive }
                    TokenKind::DotDotEq  => { self.advance(); RangeKind::Inclusive }
                    _ => {
                        self.diags.push(
                            Diag::error(22, t.line, t.col, 3,
                                "missing range operator (`..` or `..=`)", "expected `..` here")
                        );
                        RangeKind::Exclusive
                    }
                };
                let end = self.parse_expr();
                // v4: optional step
                let step = if matches!(self.peek(), TokenKind::Step) {
                    self.advance(); Some(self.parse_expr())
                } else { None };
                self.in_loop += 1;
                let body = self.parse_block();
                self.in_loop -= 1;
                Stmt::For { var, start, end, step, kind, body }
            }

            TokenKind::Break => {
                self.advance();
                if self.in_loop == 0 {
                    self.diags.push(
                        Diag::error(50, t.line, t.col, 5,
                            "`break` outside of loop", "invalid here")
                        .help("`break` exits the nearest `for`, `while`, or `loop` block")
                    );
                }
                // v4: break with value
                if !matches!(self.peek(), TokenKind::Semi) {
                    let val = self.parse_expr();
                    self.eat_semi();
                    return Stmt::BreakVal(val);
                }
                self.eat_semi();
                Stmt::Break
            }

            TokenKind::Continue => {
                self.advance();
                if self.in_loop == 0 {
                    self.diags.push(
                        Diag::error(51, t.line, t.col, 8,
                            "`continue` outside of loop", "invalid here")
                        .help("`continue` skips to the next iteration")
                    );
                }
                self.eat_semi();
                Stmt::Continue
            }

            TokenKind::Print => {
                self.advance();
                self.expect(&TokenKind::LParen);
                let e = self.parse_expr();
                self.expect(&TokenKind::RParen);
                self.eat_semi();
                // W101: side-effect-free bare value
                if matches!(e, Expr::IntLit(_) | Expr::BoolLit(_)) {
                    self.diags.push(
                        Diag::warning(101, t.line, t.col, 5,
                            "printing a constant literal", "constant print")
                        .help("use a variable or runtime expression")
                    );
                }
                Stmt::Print(e)
            }

            TokenKind::Match => {
                self.advance();
                let scrutinee = self.parse_expr();
                self.expect(&TokenKind::LBrace);
                let mut arms = Vec::new();
                while !matches!(self.peek(), TokenKind::RBrace | TokenKind::Eof) {
                    let pattern = match self.peek().clone() {
                        TokenKind::IntLit(n) => { self.advance(); MatchPat::Lit(n) }
                        TokenKind::StrLit(s) => { self.advance(); MatchPat::StrLit(s) }
                        TokenKind::Underscore => { self.advance(); MatchPat::Wildcard }
                        _ => { self.advance(); MatchPat::Wildcard }
                    };
                    self.expect(&TokenKind::FatArrow);
                    let body = self.parse_block();
                    if matches!(self.peek(), TokenKind::Comma) { self.advance(); }
                    arms.push(MatchArm { pattern, body });
                }
                self.expect(&TokenKind::RBrace);
                Stmt::Match(scrutinee, arms)
            }

            TokenKind::Unsafe => {
                self.advance();
                self.in_unsafe += 1;
                let body = self.parse_block();
                self.in_unsafe -= 1;
                // W603: unsafe block with no actual unsafe ops
                let has_unsafe_op = body.iter().any(|s| matches!(s, Stmt::AsmStmt(_)));
                if !has_unsafe_op {
                    self.diags.push(
                        Diag::warning(603, t.line, t.col, 6,
                            "unsafe block contains no unsafe operations", "consider removing `unsafe`")
                        .help("unsafe is only needed when using asm!(), raw pointers, or hardware ops")
                    );
                }
                Stmt::UnsafeBlock(body)
            }

            TokenKind::Asm => {
                // W604: asm!() outside unsafe block
                if self.in_unsafe == 0 {
                    self.diags.push(
                        Diag::warning(604, t.line, t.col, 3,
                            "asm!() used outside of `unsafe` block", "wrap in `unsafe { }`")
                        .help("inline assembly bypasses safety checks; mark as `unsafe { asm!(...); }`")
                    );
                }
                self.advance();
                self.expect(&TokenKind::Bang);
                self.expect(&TokenKind::LParen);
                let body = if let TokenKind::StrLit(s) = self.peek().clone() { self.advance(); s } else { String::new() };
                self.expect(&TokenKind::RParen);
                self.eat_semi();
                Stmt::AsmStmt(body)
            }

            TokenKind::Assert => {
                // assert!(cond)  or  assert!(cond, "message")
                let line = t.line;
                self.advance();
                self.expect(&TokenKind::Bang);
                self.expect(&TokenKind::LParen);
                let cond = self.parse_expr();
                let msg = if matches!(self.peek(), TokenKind::Comma) {
                    self.advance();
                    if let TokenKind::StrLit(s) = self.peek().clone() { self.advance(); Some(s) } else { None }
                } else { None };
                self.expect(&TokenKind::RParen);
                self.eat_semi();
                Stmt::AssertStmt(cond, msg, line)
            }

            // Assignment or expression statement
            _ => {
                let e = self.parse_expr();
                // Check for assignment / compound assignment
                let stmt = match self.peek().clone() {
                    TokenKind::Eq => {
                        self.advance();
                        let rhs = self.parse_expr();
                        // Might be array index assignment: e is Index(Var(nm), idx)
                        match e {
                            Expr::Index(base, idx) => {
                                if let Expr::Var(nm) = *base {
                                    Stmt::AssignIdx(nm, *idx, rhs)
                                } else {
                                    Stmt::Expr(Expr::IntLit(0), t.line, t.col)
                                }
                            }
                            Expr::Var(nm) => Stmt::Assign(nm, rhs),
                            _ => Stmt::Expr(rhs, t.line, t.col),
                        }
                    }
                    TokenKind::PlusEq  => { self.advance(); let r = self.parse_expr(); if let Expr::Var(n) = e { Stmt::CompoundAssign(n, BinOpKind::Add, r) } else { Stmt::Expr(r, t.line, t.col) } }
                    TokenKind::MinusEq => { self.advance(); let r = self.parse_expr(); if let Expr::Var(n) = e { Stmt::CompoundAssign(n, BinOpKind::Sub, r) } else { Stmt::Expr(r, t.line, t.col) } }
                    TokenKind::StarEq  => { self.advance(); let r = self.parse_expr(); if let Expr::Var(n) = e { Stmt::CompoundAssign(n, BinOpKind::Mul, r) } else { Stmt::Expr(r, t.line, t.col) } }
                    TokenKind::SlashEq => { self.advance(); let r = self.parse_expr(); if let Expr::Var(n) = e { Stmt::CompoundAssign(n, BinOpKind::Div, r) } else { Stmt::Expr(r, t.line, t.col) } }
                    TokenKind::PercentEq => { self.advance(); let r = self.parse_expr(); if let Expr::Var(n) = e { Stmt::CompoundAssign(n, BinOpKind::Mod, r) } else { Stmt::Expr(r, t.line, t.col) } }
                    TokenKind::AmpEq   => { self.advance(); let r = self.parse_expr(); if let Expr::Var(n) = e { Stmt::CompoundAssign(n, BinOpKind::BAnd, r) } else { Stmt::Expr(r, t.line, t.col) } }
                    TokenKind::PipeEq  => { self.advance(); let r = self.parse_expr(); if let Expr::Var(n) = e { Stmt::CompoundAssign(n, BinOpKind::BOr, r) } else { Stmt::Expr(r, t.line, t.col) } }
                    TokenKind::CaretEq => { self.advance(); let r = self.parse_expr(); if let Expr::Var(n) = e { Stmt::CompoundAssign(n, BinOpKind::BXor, r) } else { Stmt::Expr(r, t.line, t.col) } }
                    _ => {
                        // W101: side-effect-free expression
                        if matches!(e, Expr::IntLit(_) | Expr::BoolLit(_) | Expr::StrLit(_) | Expr::Var(_)) {
                            self.diags.push(
                                Diag::warning(101, t.line, t.col, 1,
                                    "side-effect-free expression statement has no effect", "remove or use `print(...)`")
                                .help("use `print(expr);` to display it, or remove it")
                            );
                        }
                        Stmt::Expr(e, t.line, t.col)
                    }
                };
                self.eat_semi();
                stmt
            }
        }
    }

    // ── Function / struct parsing ─────────────────────────────────────────────
    fn parse_using(&mut self) {
        let t = self.tok().clone();
        self.advance();
        if !self.expect(&TokenKind::ColonColon) { return; }
        let namespace = self.eat_ident("namespace after `using::`");
        if namespace != "prp" && namespace != "fetch" {
            self.diags.push(Diag::error(14, t.line, t.col, 5,
            "unsupported import namespace", "supported namespaces: `using::prp::*`, `using::fetch::*`"));
        }
        if !self.expect(&TokenKind::ColonColon) { return; }

        loop {
            let import = if matches!(self.peek(), TokenKind::Star) {
                self.advance(); String::from("*")
            } else {
                self.eat_ident("function name after `using::<namespace>::`")
            };
            let supported = match namespace.as_str() {
                "prp" => matches!(import.as_str(),
                    "random_bytes" | "sha256_file" | "seal_file" | "open_file" |
                    "seal_text" | "fingerprint" | "keygen" | "sign" | "verify" |
                    "seal_file_pub" | "open_file_prv" | "seal_text_pub" | "selftest"),
                "fetch" => matches!(import.as_str(), "get" | "https_get" | "active" | "test_https"),
                _ => false,
            };
            if import != "*" && !supported {
                self.diags.push(Diag::error(14, t.line, t.col, import.len(),
                    "unknown imported function", "check the namespace function name"));
            } else if namespace == "prp" && !self.prp_imports.contains(&import) {
                self.prp_imports.push(import);
            } else if namespace == "fetch" && !self.fetch_imports.contains(&import) {
                self.fetch_imports.push(import);
            }
            if matches!(self.peek(), TokenKind::Comma) {
                self.advance();
            } else {
                break;
            }
        }
        if matches!(self.peek(), TokenKind::Semi) { self.advance(); }
    }

    fn parse_fn(&mut self) -> FnDef {
        let t = self.tok().clone();
        self.advance(); // eat `fn`
        let name = self.eat_ident("after `fn`");
        if name == "<error>" {
            self.diags.push(
                Diag::error(30, t.line, t.col, 2, "missing function name after `fn`", "here")
            );
        }
        self.expect(&TokenKind::LParen);
        let mut params = Vec::new();
        while !matches!(self.peek(), TokenKind::RParen | TokenKind::Eof) {
            let param_start = self.pos;
            let pname = match self.peek().clone() {
                TokenKind::TInt | TokenKind::TStr | TokenKind::TBool |
                TokenKind::TVoid | TokenKind::TPtr => {
                    self.advance();
                    self.eat_ident("parameter name")
                }
                _ => {
                    let pname = self.eat_ident("parameter name");
                    if matches!(self.peek(), TokenKind::Colon) {
                        self.advance();
                        if matches!(self.peek(), TokenKind::TInt | TokenKind::TStr |
                            TokenKind::TBool | TokenKind::TVoid | TokenKind::TPtr)
                        {
                            self.advance();
                        } else {
                            let t = self.tok().clone();
                            self.diags.push(Diag::error(
                                10, t.line, t.col, t.len,
                                "expected parameter type after `:`", "here",
                            ));
                        }
                    }
                    pname
                }
            };
            params.push(pname);
            if matches!(self.peek(), TokenKind::Comma) { self.advance(); }
            if self.pos == param_start { self.advance(); }
        }
        // W602: too many parameters
        if params.len() > 16 {
            self.diags.push(
                Diag::warning(602, t.line, t.col, name.len(),
                    &format!("function `{}` has {} parameters (>16)", name, params.len()),
                    "many params")
                .help("consider grouping parameters into a struct")
            );
        }
        self.expect(&TokenKind::RParen);
        // v4: `-> void` is optional for void functions
        let ret_void = if matches!(self.peek(), TokenKind::Arrow) {
            self.advance();
            let is_void = matches!(self.peek(), TokenKind::TVoid);
            self.advance(); // consume type token
            is_void
        } else {
            true // no return type annotation → void
        };
        let body = self.parse_block();
        // W401: non-void fn with no return
        if !ret_void && !Self::body_has_return(&body) {
            self.diags.push(
                Diag::warning(401, t.line, 1, name.len(),
                    &format!("function `{}` has non-void return type but may not return a value", name),
                    "missing return")
                .help("add `return <value>;` on every code path")
            );
        }
        FnDef { name, params, body, ret_void, line: t.line }
    }

    fn parse_struct(&mut self) -> StructDef {
        let t = self.tok().clone();
        self.advance(); // eat `struct`
        let name = self.eat_ident("struct name");
        self.expect(&TokenKind::LBrace);
        let mut fields = Vec::new();
        while !matches!(self.peek(), TokenKind::RBrace | TokenKind::Eof) {
            // Accept `type name` or just `name`
            match self.peek().clone() {
                TokenKind::TInt | TokenKind::TStr | TokenKind::TBool |
                TokenKind::TVoid | TokenKind::TPtr => { self.advance(); }
                _ => {}
            }
            let fname = self.eat_ident("field name");
            fields.push(fname);
            if matches!(self.peek(), TokenKind::Comma) { self.advance(); }
        }
        self.expect(&TokenKind::RBrace);
        StructDef { name, fields, line: t.line }
    }

    // ── Top-level parse ───────────────────────────────────────────────────────
    fn parse_program(mut self) -> (Vec<StructDef>, Vec<FnDef>, Vec<ConstDef>, Vec<String>, Vec<String>, DiagEngine) {
        let mut fns:     Vec<FnDef>    = Vec::new();
        let mut structs: Vec<StructDef> = Vec::new();
        while !matches!(self.peek(), TokenKind::Eof) {
            match self.peek().clone() {
                TokenKind::Const => {
                    // const NAME: int = value;
                    let ct = self.tok().clone(); self.advance();
                    let cname = self.eat_ident("constant name after `const`");
                    // optional `: int`
                    if matches!(self.peek(), TokenKind::Colon) {
                        self.advance(); self.advance(); // eat type
                    }
                    self.expect(&TokenKind::Eq);
                    let val = if let TokenKind::IntLit(n) = self.peek().clone() {
                        self.advance(); n
                    } else { 0 };
                    self.eat_semi();
                    if self.consts.iter().any(|c| c.name == cname) {
                        self.diags.push(
                            Diag::error(13, ct.line, ct.col, cname.len(),
                                &format!("constant `{}` already defined", cname), "defined again here")
                        );
                    } else {
                        self.consts.push(ConstDef { name: cname, value: val, line: ct.line });
                    }
                }
                TokenKind::Fn => {
                    let f = self.parse_fn();
                    // E003: duplicate function
                    if fns.iter().any(|g| g.name == f.name) {
                        self.diags.push(
                            Diag::error(3, f.line, 1, f.name.len(),
                                &format!("duplicate function name `{}`", f.name), "defined again here")
                            .help("rename one of the definitions")
                        );
                    } else {
                        fns.push(f);
                    }
                }
                TokenKind::Struct => {
                    let s = self.parse_struct();
                    // E004: duplicate struct
                    if structs.iter().any(|g| g.name == s.name) {
                        self.diags.push(
                            Diag::error(4, s.line, 1, s.name.len(),
                                &format!("duplicate struct name `{}`", s.name), "defined again here")
                        );
                    } else {
                        self.structs.push(s.clone());
                        structs.push(s);
                    }
                }
                TokenKind::Using => self.parse_using(),
                TokenKind::Import => {
                    self.advance();
                    let module = self.eat_ident("module name");
                    self.expect(&TokenKind::ColonColon);
                    let mut imports = Vec::new();
                    loop {
                        if matches!(self.peek(), TokenKind::Star) {
                            self.advance();
                            imports.push("*".into());
                            break;
                        } else {
                            let fname = self.eat_ident("function");
                            imports.push(fname);
                        }
                        if !matches!(self.peek(), TokenKind::Comma) { break; }
                        self.advance();
                    }
                    self.expect(&TokenKind::Semi);
                    // Try to load module from AVFS or print warning
                    unsafe {
                        let mut mname_buf = [0u8; 64];
                        let mut mlen = 0usize;
                        for &c in module.as_bytes() {
                            if mlen < 60 { mname_buf[mlen] = c; mlen += 1; }
                        }
                        for &c in b".hls" { if mlen < 62 { mname_buf[mlen] = c; mlen += 1; } }
                        mname_buf[mlen] = 0;
                        // (In a real build, you'd load & cache the module here)
                    }
                }
                _ => {
                    let t = self.tok().clone();
                    self.diags.push(
                        Diag::error(1, t.line, t.col, t.len,
                            &format!("top-level code: expected `fn` or `struct`, found `{:?}`", t.kind),
                            "only definitions allowed here")
                        .help("wrap loose statements inside `fn main() -> void { ... }`")
                    );
                    self.advance();
                }
            }
        }
        for pair in self.tokens.windows(2) {
            let TokenKind::Ident(name) = &pair[0].kind else { continue; };
            if pair[1].kind != TokenKind::LParen { continue; }
            let is_user_function = fns.iter().any(|function| function.name == *name);
            let is_intrinsic = matches!(name.as_str(),
                "in8" | "in16" | "in32" | "out8" | "out16" | "out32" |
                "get_ticks" | "sleep_ms" | "pit_set_hz" | "poll_scancode" |
                "print_char" | "print_str" | "print_hex" | "cpuid" |
                "peek" | "peek8" | "peek16" | "peek32" | "poke" | "poke8" |
                "poke16" | "poke32" | "malloc" | "alloc" | "free" |
                "memcpy" | "memset" | "strlen" | "strsub" | "stoi" |
                "char_code" | "ord");
            let is_prp_function = matches!(name.as_str(),
                "random_bytes" | "sha256_file" | "seal_file" | "open_file" |
                "seal_text" | "fingerprint" | "keygen" | "sign" | "verify" |
                "seal_file_pub" | "open_file_prv" | "seal_text_pub" | "selftest");
            let prp_is_imported = self.prp_imports.iter()
                .any(|import| import == "*" || import == name);
            let is_fetch_function = matches!(name.as_str(), "get" | "https_get" | "active" | "test_https");
            let fetch_is_imported = self.fetch_imports.iter()
                .any(|import| import == "*" || import == name);
            if !is_user_function && !is_intrinsic
                && !(is_prp_function && prp_is_imported)
                && !(is_fetch_function && fetch_is_imported)
            {
                self.diags.push(Diag::error(9, pair[0].line, pair[0].col, name.len(),
                    &format!("function `{}` is not defined or imported", name), "unresolved call")
                    .help("define the function, use a supported intrinsic, or import it with `using::namespace::name;`"));
            }
        }
        const PRP_CALLS: &[&str] = &[
            "random_bytes", "sha256_file", "seal_file", "open_file", "seal_text",
            "fingerprint", "keygen", "sign", "verify", "seal_file_pub",
            "open_file_prv", "seal_text_pub", "selftest",
        ];
        for f in &fns {
            for &name in PRP_CALLS {
                if Self::body_calls(&f.body, name)
                    && !self.prp_imports.iter().any(|import| import == "*" || import == name)
                {
                    self.diags.push(Diag::error(14, f.line, 1, f.name.len(),
                        &format!("PRP function `{}` is not imported", name), "missing import")
                        .help(&format!("add `using::prp::{};` or `using::prp::*;`", name)));
                }
            }
        }
        for f in &fns {
            for name in ["get", "https_get", "active", "test_https"] {
                if Self::body_calls(&f.body, name)
                    && !self.fetch_imports.iter().any(|import| import == "*" || import == name)
                {
                    self.diags.push(Diag::error(15, f.line, 1, f.name.len(),
                        &format!("fetch function `{}` is not imported", name), "missing import")
                        .help(&format!("add `using::fetch::{};` or `using::fetch::*;`", name)));
                }
            }
        }
        // E002: no main
        if !fns.iter().any(|f| f.name == "main") {
            self.diags.push(
                Diag::error(2, 0, 0, 0, "no `main` function - program has no entry point", "missing `main`")
                .help("add `fn main() -> void { ... }`")
            );
        }
        // W501: unused functions
        for f in &fns {
            if f.name == "main" { continue; }
            let called = fns.iter().any(|g| Self::body_calls(&g.body, &f.name));
            if !called {
                self.diags.push(
                    Diag::warning(501, f.line, 1, f.name.len(),
                        &format!("function `{}` is defined but never called", f.name), "unused function")
                    .help("call it from another function or remove it")
                );
            }
        }
        // W502: unused variables
        for f in &fns { self.lint_vars(f); }
        let consts = self.consts.clone();
        (structs, fns, consts, self.prp_imports, self.fetch_imports, self.diags)
    }

    // ── Static analysis helpers ───────────────────────────────────────────────
    fn body_has_break_static(stmts: &[Stmt]) -> bool {
        stmts.iter().any(|s| matches!(s, Stmt::Break | Stmt::BreakVal(_)))
    }
    fn body_has_return(stmts: &[Stmt]) -> bool {
        stmts.iter().any(|s| match s {
            Stmt::Return(_, _)          => true,
            Stmt::If(_, t, e)           => Self::body_has_return(t) && Self::body_has_return(e),
            Stmt::Loop(b)               => Self::body_has_return(b),
            Stmt::While(_, b)           => Self::body_has_return(b),
            Stmt::UnsafeBlock(b)        => Self::body_has_return(b),
            _                           => false,
        })
    }
    fn body_calls(stmts: &[Stmt], name: &str) -> bool {
        stmts.iter().any(|s| Self::stmt_calls(s, name))
    }
    fn stmt_calls(s: &Stmt, nm: &str) -> bool {
        match s {
            Stmt::Let(_, e, _, _) | Stmt::Assign(_, e) | Stmt::Return(e, _) |
            Stmt::Print(e) | Stmt::PrintHex(e) | Stmt::BreakVal(e)
                => Self::expr_calls(e, nm),
            Stmt::CompoundAssign(_, _, e) => Self::expr_calls(e, nm),
            Stmt::If(c, t, e)  => Self::expr_calls(c, nm) || Self::body_calls(t, nm) || Self::body_calls(e, nm),
            Stmt::While(c, b)  => Self::expr_calls(c, nm) || Self::body_calls(b, nm),
            Stmt::Loop(b) | Stmt::UnsafeBlock(b) => Self::body_calls(b, nm),
            Stmt::For { start, end, step, body, .. } =>
                Self::expr_calls(start, nm) || Self::expr_calls(end, nm) ||
                step.as_ref().map_or(false, |s| Self::expr_calls(s, nm)) ||
                Self::body_calls(body, nm),
            Stmt::Expr(e, _, _) => Self::expr_calls(e, nm),
            Stmt::Match(e, arms) => Self::expr_calls(e, nm) || arms.iter().any(|a| Self::body_calls(&a.body, nm)),
            _ => false,
        }
    }
    fn expr_calls(e: &Expr, nm: &str) -> bool {
        match e {
            Expr::Call(n, args) => n == nm || args.iter().any(|a| Self::expr_calls(a, nm)),
            Expr::BinOp(l, _, r) => Self::expr_calls(l, nm) || Self::expr_calls(r, nm),
            Expr::UnOp(_, i) | Expr::Deref(i) | Expr::Cast(i, _) => Self::expr_calls(i, nm),
            Expr::Index(b, i) => Self::expr_calls(b, nm) || Self::expr_calls(i, nm),
            Expr::Field(b, _) | Expr::FieldChain(b, _) => Self::expr_calls(b, nm),
            Expr::ArrayLit(items) => items.iter().any(|a| Self::expr_calls(a, nm)),
            Expr::ArrayFill(v, _) => Self::expr_calls(v, nm),
            _ => false,
        }
    }
    fn lint_vars(&mut self, f: &FnDef) {
        let mut bound = Vec::new();
        Self::collect_lets(&f.body, &mut bound);
        for (name, line, col) in &bound {
            if name.starts_with('_') || name == "<error>" { continue; }
            if !Self::body_reads(&f.body, name) {
                self.diags.push(
                    Diag::warning(502, *line, *col, name.len(),
                        &format!("unused variable `{}`", name), "never read")
                    .help(&format!("prefix with `_` to suppress: `_{}`", name))
                );
            }
        }
    }
    fn collect_lets(stmts: &[Stmt], out: &mut Vec<(String, usize, usize)>) {
        for s in stmts {
            match s {
                Stmt::Let(n, _, l, c) | Stmt::LetArr(n, _, _, l, c) => {
                    if !n.starts_with('_') { out.push((n.clone(), *l, *c)); }
                }
                Stmt::If(_, t, e) => { Self::collect_lets(t, out); Self::collect_lets(e, out); }
                Stmt::While(_, b) | Stmt::Loop(b) | Stmt::UnsafeBlock(b) => Self::collect_lets(b, out),
                Stmt::For { body, .. } => Self::collect_lets(body, out),
                Stmt::Match(_, arms) => {
                    for arm in arms { Self::collect_lets(&arm.body, out); }
                }
                _ => {}
            }
        }
    }
    fn body_reads(stmts: &[Stmt], name: &str) -> bool {
        stmts.iter().any(|s| Self::stmt_reads(s, name))
    }
    fn stmt_reads(s: &Stmt, nm: &str) -> bool {
        match s {
            Stmt::Let(_, e, _, _) | Stmt::Return(e, _) | Stmt::Print(e) |
            Stmt::PrintHex(e) | Stmt::BreakVal(e) => Self::expr_reads(e, nm),
            Stmt::Assign(n, e)       => n != nm && Self::expr_reads(e, nm) || Self::expr_reads(e, nm),
            Stmt::CompoundAssign(_, _, e) => Self::expr_reads(e, nm),
            Stmt::If(c, t, e)        => Self::expr_reads(c, nm) || Self::body_reads(t, nm) || Self::body_reads(e, nm),
            Stmt::While(c, b)        => Self::expr_reads(c, nm) || Self::body_reads(b, nm),
            Stmt::Loop(b) | Stmt::UnsafeBlock(b) => Self::body_reads(b, nm),
            Stmt::For { start, end, step, body, .. } =>
                Self::expr_reads(start, nm) || Self::expr_reads(end, nm) ||
                step.as_ref().map_or(false, |s| Self::expr_reads(s, nm)) ||
                Self::body_reads(body, nm),
            Stmt::Expr(e, _, _)      => Self::expr_reads(e, nm),
            Stmt::Match(e, arms)     => Self::expr_reads(e, nm) || arms.iter().any(|a| Self::body_reads(&a.body, nm)),
            _ => false,
        }
    }
    fn expr_reads(e: &Expr, nm: &str) -> bool {
        match e {
            Expr::Var(n) | Expr::AddrOf(n) => n == nm,
            Expr::Call(_, args)       => args.iter().any(|a| Self::expr_reads(a, nm)),
            Expr::BinOp(l, _, r)      => Self::expr_reads(l, nm) || Self::expr_reads(r, nm),
            Expr::UnOp(_, i) | Expr::Deref(i) | Expr::Cast(i, _) => Self::expr_reads(i, nm),
            Expr::Index(b, i)         => Self::expr_reads(b, nm) || Self::expr_reads(i, nm),
            Expr::Field(b, _) | Expr::FieldChain(b, _) => Self::expr_reads(b, nm),
            Expr::ArrayLit(items)     => items.iter().any(|a| Self::expr_reads(a, nm)),
            Expr::ArrayFill(v, _)     => Self::expr_reads(v, nm),
            _ => false,
        }
    }
}

// ============================================================================
//  SECTION 7 - EMITTER  (v4: struct table, proper field index resolution)
// ============================================================================

struct LoopCtx {
    break_patches:    Vec<usize>,
    break_val_patches: Vec<usize>, // v4: break-with-value jump sites
    continue_patches: Vec<usize>,
    loop_top:         u32,
}

struct Emitter {
    code:       Vec<u8>,
    consts:     Vec<Vec<u8>>,
    locals:     Vec<String>,
    fn_addrs:   Vec<(String, u32)>,
    fn_patches: Vec<(usize, String)>,
    loops:      Vec<LoopCtx>,
    structs:    Vec<StructDef>,   // FIX: field name→index resolution
    const_map:  Vec<(String, i32)>, // v5: top-level constants
    prp_imports: Vec<String>,
    fetch_imports: Vec<String>,
}

pub fn link_modules(
    units: Vec<(String, Vec<FnDef>, Vec<StructDef>)>,
) -> (Vec<u8>, Vec<Vec<u8>>, u32) {
    let mut code = Vec::new();
    let mut consts = Vec::new();
    let mut fn_addrs: Vec<(String, String, u32)> = Vec::new();  // (module, name, addr)
    
    // Declare the missing collection variable here
    let cross_module_calls: Vec<(usize, String, String)> = Vec::new();
    
    // Layout phase: record each function's address
    for (module_name, fns, structs) in &units {
        for f in fns {
            let addr = code.len() as u32;
            fn_addrs.push((module_name.clone(), f.name.clone(), addr));
        }
    }
    
    // Patch phase: resolve cross-module calls
    for (offset, call_module, call_fn) in cross_module_calls.iter() {
        if let Some((_, _, addr)) = fn_addrs.iter()
            .find(|(m, n, _)| m == call_module && n == call_fn) {
            // patch code[*offset..*offset+4] = addr.to_le_bytes()
        }
    }
    
    (code, consts, 0)  // entry point = "main" function address
}


impl Emitter {
    fn new(structs: Vec<StructDef>, consts: Vec<ConstDef>, prp_imports: Vec<String>, fetch_imports: Vec<String>) -> Self {
        let const_map = consts.into_iter().map(|c| (c.name, c.value)).collect();
        Emitter {
            code:       Vec::with_capacity(4096),
            consts:     Vec::with_capacity(64),
            locals:     Vec::with_capacity(32),
            fn_addrs:   Vec::with_capacity(16),
            fn_patches: Vec::with_capacity(32),
            loops:      Vec::new(),
            structs,
            const_map,
            prp_imports,
            fetch_imports,
        }
    }
    fn eu8(&mut self, b: u8)   { self.code.push(b); }
    fn ei32(&mut self, n: i32) { self.code.extend_from_slice(&n.to_le_bytes()); }
    fn eu32(&mut self, n: u32) { self.code.extend_from_slice(&n.to_le_bytes()); }
    fn eu16(&mut self, n: u16) { self.code.extend_from_slice(&n.to_le_bytes()); }
    fn addr(&self) -> u32      { self.code.len() as u32 }
    fn patch(&mut self, off: usize, v: u32) {
        self.code[off..off+4].copy_from_slice(&v.to_le_bytes());
    }

    fn add_const(&mut self, s: &str) -> u16 {
        let mut b = s.as_bytes().to_vec(); b.push(0);
        for (i, c) in self.consts.iter().enumerate() { if c == &b { return i as u16; } }
        let i = self.consts.len() as u16;
        self.consts.push(b);
        i
    }
    fn local(&mut self, name: &str) -> i32 {
        if let Some(p) = self.locals.iter().position(|v| v == name) { return p as i32; }
        let i = self.locals.len() as i32;
        self.locals.push(name.into());
        i
    }
    fn patch_calls(&mut self) {
        let patches = self.fn_patches.clone();
        for (off, nm) in &patches {
            let a = self.fn_addrs.iter().find(|(n, _)| n == nm).map(|(_, a)| *a).unwrap_or(u32::MAX);
            self.patch(*off, a);
        }
    }

    // FIX: resolve field name to struct layout index
    fn field_index(&self, field: &str) -> i32 {
        for s in &self.structs {
            if let Some(idx) = s.fields.iter().position(|f| f == field) {
                return idx as i32;
            }
        }
        0 // safe default: first field
    }

    fn prp_imported(&self, name: &str) -> bool {
        self.prp_imports.iter().any(|import| import == "*" || import == name)
    }

    fn fetch_imported(&self, name: &str) -> bool {
        self.fetch_imports.iter().any(|import| import == "*" || import == name)
    }

    fn emit_intrinsic_call(&mut self, name: &str, args: &[Expr]) -> bool {
        let fetch_opcode = if self.fetch_imported(name) {
            match (name, args.len()) {
                ("get", 1) | ("https_get", 1) => Some(Opcode::FetchGet),
                ("active", 0) => Some(Opcode::FetchActive),
                ("test_https", 0) => Some(Opcode::FetchTest),
                _ => None,
            }
        } else { None };
        if let Some(opcode) = fetch_opcode {
            for arg in args { self.emit_expr(arg); }
            self.eu8(opcode as u8);
            self.eu8(args.len() as u8);
            return true;
        }

        let prp_opcode = if self.prp_imported(name) {
            match (name, args.len()) {
                ("selftest", 0) => Some(Opcode::PrpSelfTest),
                ("random_bytes", 2) => Some(Opcode::PrpRandom),
                ("sha256_file", 2) => Some(Opcode::PrpSha256File),
                ("seal_file", 3) => Some(Opcode::PrpSealFile),
                ("open_file", 3) => Some(Opcode::PrpOpenFile),
                ("seal_text", 3) => Some(Opcode::PrpSealText),
                ("fingerprint", 2) => Some(Opcode::PrpFingerprint),
                ("keygen", 1) => Some(Opcode::PrpKeygen),
                ("sign", 2) => Some(Opcode::PrpSign),
                ("verify", 1 | 2) => Some(Opcode::PrpVerify),
                ("seal_file_pub", 3) => Some(Opcode::PrpSealFilePub),
                ("open_file_prv", 3) => Some(Opcode::PrpOpenFilePrv),
                ("seal_text_pub", 3) => Some(Opcode::PrpSealTextPub),
                _ => None,
            }
        } else {
            None
        };
        if let Some(opcode) = prp_opcode {
            for arg in args { self.emit_expr(arg); }
            self.eu8(opcode as u8);
            self.eu8(args.len() as u8);
            return true;
        }

        let read_opcode = match name {
            "peek8" if args.len() == 1 => Some(Opcode::Peek8),
            "peek16" if args.len() == 1 => Some(Opcode::Peek16),
            "peek32" if args.len() == 1 => Some(Opcode::Peek32),
            "peek" if args.len() == 2 => match args[1] {
                Expr::IntLit(8) => Some(Opcode::Peek8),
                Expr::IntLit(16) => Some(Opcode::Peek16),
                Expr::IntLit(32) => Some(Opcode::Peek32),
                _ => None,
            },
            _ => None,
        };
        if let Some(opcode) = read_opcode {
            self.emit_expr(&args[0]);
            self.eu8(opcode as u8);
            return true;
        }

        let write_opcode = match name {
            "poke8" if args.len() == 2 => Some(Opcode::Poke8),
            "poke16" if args.len() == 2 => Some(Opcode::Poke16),
            "poke32" if args.len() == 2 => Some(Opcode::Poke32),
            "poke" if args.len() == 3 => match args[2] {
                Expr::IntLit(8) => Some(Opcode::Poke8),
                Expr::IntLit(16) => Some(Opcode::Poke16),
                Expr::IntLit(32) => Some(Opcode::Poke32),
                _ => None,
            },
            _ => None,
        };
        if let Some(opcode) = write_opcode {
            self.emit_expr(&args[0]);
            self.emit_expr(&args[1]);
            self.eu8(opcode as u8);
            self.eu8(Opcode::Push as u8);
            self.ei32(0);
            return true;
        }

        let string_opcode = match (name, args.len()) {
            ("strlen", 1) => Some(Opcode::StrLen),
            ("strsub", 3) => Some(Opcode::StrSub),
            ("stoi", 1) => Some(Opcode::StrToInt),
            ("char_code", 1) | ("ord", 1) => Some(Opcode::StrByte),
            _ => None,
        };
        if let Some(opcode) = string_opcode {
            for arg in args { self.emit_expr(arg); }
            self.eu8(opcode as u8);
            return true;
        }

        match (name, args.len()) {
            ("malloc", 1) | ("alloc", 1) => {
                self.emit_expr(&args[0]); self.eu8(Opcode::HeapAlloc as u8);
                return true;
            }
            ("free", 1) => {
                self.emit_expr(&args[0]); self.eu8(Opcode::HeapFree as u8);
            }
            ("in8", 1) => {
                self.emit_expr(&args[0]); self.eu8(Opcode::In8 as u8);
                return true;
            }
            ("in16", 1) => {
                self.emit_expr(&args[0]); self.eu8(Opcode::In16 as u8);
                return true;
            }
            ("in32", 1) => {
                self.emit_expr(&args[0]); self.eu8(Opcode::In32 as u8);
                return true;
            }
            ("out8", 2) => {
                for arg in args { self.emit_expr(arg); }
                self.eu8(Opcode::Out8 as u8);
            }
            ("out16", 2) => {
                for arg in args { self.emit_expr(arg); }
                self.eu8(Opcode::Out16 as u8);
            }
            ("out32", 2) => {
                for arg in args { self.emit_expr(arg); }
                self.eu8(Opcode::Out32 as u8);
            }
            ("get_ticks", 0) => {
                self.eu8(Opcode::GetTicks as u8);
                return true;
            }
            ("sleep_ms", 1) => {
                self.emit_expr(&args[0]); self.eu8(Opcode::SleepMs as u8);
            }
            ("pit_set_hz", 1) => {
                self.emit_expr(&args[0]); self.eu8(Opcode::PitSetHz as u8);
            }
            ("poll_scancode", 0) => {
                self.eu8(Opcode::PollScancode as u8);
                return true;
            }
            ("print_char", 1) => {
                self.emit_expr(&args[0]); self.eu8(Opcode::PrintChar as u8);
            }
            ("print_str", 1) => {
                self.emit_expr(&args[0]); self.eu8(Opcode::PrintStr as u8);
            }
            ("cpuid", 1) => {
                self.emit_expr(&args[0]); self.eu8(Opcode::Cpuid as u8);
                self.eu8(Opcode::Swap as u8); self.eu8(Opcode::Pop as u8);
                self.eu8(Opcode::Swap as u8); self.eu8(Opcode::Pop as u8);
                self.eu8(Opcode::Swap as u8); self.eu8(Opcode::Pop as u8);
                return true;
            }
            ("memcpy", 3) => {
                for arg in args { self.emit_expr(arg); }
                self.eu8(Opcode::MemCpy as u8);
            }
            ("memset", 3) => {
                for arg in args { self.emit_expr(arg); }
                self.eu8(Opcode::MemSet as u8);
            }
            ("print_hex", 1) => {
                self.emit_expr(&args[0]);
                self.eu8(Opcode::PrintHex as u8);
            }
            _ => return false,
        }
        self.eu8(Opcode::Push as u8);
        self.ei32(0);
        true
    }

    fn loop_push(&mut self, top: u32) {
        self.loops.push(LoopCtx {
            break_patches:     Vec::new(),
            break_val_patches: Vec::new(),
            continue_patches:  Vec::new(),
            loop_top: top,
        });
    }
    fn loop_pop(&mut self, after: u32, cont: u32) {
        if let Some(ctx) = self.loops.pop() {
            for bp in ctx.break_patches     { self.patch(bp, after); }
            for bp in ctx.break_val_patches { self.patch(bp, after); }
            for cp in ctx.continue_patches  { self.patch(cp, cont);  }
        }
    }

    fn emit_expr(&mut self, e: &Expr) {
        match e {
            Expr::IntLit(n)   => { self.eu8(Opcode::Push as u8); self.ei32(*n); }
            Expr::BoolLit(b)  => { self.eu8(Opcode::Push as u8); self.ei32(*b as i32); }
            Expr::StrLit(s)   => { let i = self.add_const(s); self.eu8(Opcode::PushStr as u8); self.eu16(i); }
            Expr::Var(nm)     => {
                // v5: check if this is a top-level constant first
                if let Some(&val) = self.const_map.iter().find(|(n, _)| n == nm).map(|(_, v)| v) {
                    self.eu8(Opcode::Push as u8); self.ei32(val);
                } else {
                    let i = self.local(nm); self.eu8(Opcode::LoadLocal as u8); self.ei32(i);
                }
            }
            Expr::AddrOf(nm)  => { let i = self.local(nm); self.eu8(Opcode::Push as u8); self.ei32(i); }
            Expr::Deref(inner) => {
                self.emit_expr(inner);
                self.eu8(Opcode::Peek32 as u8);
            }
            Expr::Input => { self.eu8(Opcode::Input as u8); }
            Expr::InlineAsm(s) => {
                match s.trim() {
                    "cli"      => self.eu8(Opcode::AsmCli as u8),
                    "sti"      => self.eu8(Opcode::AsmSti as u8),
                    "nop"      => self.eu8(Opcode::AsmNop as u8),
                    "hlt"      => self.eu8(Opcode::AsmHlt as u8),
                    "pushfd"   => self.eu8(Opcode::AsmPushfd as u8),
                    "popfd"    => self.eu8(Opcode::AsmPopfd as u8),
                    "lidt"     => self.eu8(Opcode::AsmLidt as u8),
                    "lgdt"     => self.eu8(Opcode::AsmLgdt as u8),
                    "wbinvd"   => self.eu8(Opcode::AsmWbinvd as u8),
                    "invlpg"   => self.eu8(Opcode::AsmInvlpg as u8),
                    "get_cr2"  => self.eu8(Opcode::GetCr2 as u8),
                    "clts"     => self.eu8(Opcode::AsmClts as u8),
                    other if other.starts_with("int ") || other.starts_with("int\t") => {
                        let rest = other[4..].trim();
                        let n = if rest.starts_with("0x") {
                            u8::from_str_radix(&rest[2..], 16).unwrap_or(0)
                        } else {
                            rest.parse().unwrap_or(0)
                        };
                        self.eu8(Opcode::AsmInt as u8); self.eu8(n);
                        self.eu8(Opcode::Push as u8); self.ei32(0);
                        return;
                    }
                    _ => { self.eu8(Opcode::AsmNop as u8); }
                }
                self.eu8(Opcode::Push as u8); self.ei32(0);
            }
            Expr::Cast(inner, target) => {
                self.emit_expr(inner);
                match target {
                    CastTarget::Int  => self.eu8(Opcode::CastInt as u8),
                    CastTarget::Bool => self.eu8(Opcode::CastBool as u8),
                }
            }
            Expr::ArrayLit(items) => {
                self.eu8(Opcode::Push as u8); self.ei32(items.len() as i32 * 4);
                self.eu8(Opcode::HeapAlloc as u8);
                for (idx, item) in items.iter().enumerate() {
                    self.eu8(Opcode::Dup as u8);
                    self.emit_expr(item);
                    self.eu8(Opcode::Push as u8); self.ei32(idx as i32);
                    self.eu8(Opcode::ArrStore as u8);
                }
            }
            Expr::ArrayFill(val, count) => {
                self.eu8(Opcode::Push as u8); self.ei32(*count as i32 * 4);
                self.eu8(Opcode::HeapAlloc as u8);
                for i in 0..*count {
                    self.eu8(Opcode::Dup as u8);
                    self.emit_expr(val);
                    self.eu8(Opcode::Push as u8); self.ei32(i as i32);
                    self.eu8(Opcode::ArrStore as u8);
                }
            }
            Expr::Index(base, idx) => {
                self.emit_expr(base);
                self.emit_expr(idx);
                self.eu8(Opcode::ArrLoad as u8);
            }
            // FIX: resolve field name to index using struct table
            Expr::Field(base, field) => {
                let idx = self.field_index(field);
                self.emit_expr(base);
                self.eu8(Opcode::Push as u8); self.ei32(idx);
                self.eu8(Opcode::ArrLoad as u8);
            }
            // v4: nested field chain - each dot is an ArrLoad
            Expr::FieldChain(base, fields) => {
                self.emit_expr(base);
                for field in fields {
                    let idx = self.field_index(field);
                    self.eu8(Opcode::Push as u8); self.ei32(idx);
                    self.eu8(Opcode::ArrLoad as u8);
                }
            }
            Expr::UnOp(op, inner) => {
                self.emit_expr(inner);
                self.eu8(match op {
                    UnOpKind::Neg  => Opcode::Neg as u8,
                    UnOpKind::Not  => Opcode::Not as u8,
                    UnOpKind::BNot => Opcode::BNot as u8,
                });
            }
            Expr::BinOp(l, op, r) => {
                self.emit_expr(l); self.emit_expr(r);
                self.eu8(match op {
                    BinOpKind::Add  => Opcode::Add as u8,  BinOpKind::Sub  => Opcode::Sub as u8,
                    BinOpKind::Mul  => Opcode::Mul as u8,  BinOpKind::Div  => Opcode::Div as u8,
                    BinOpKind::Mod  => Opcode::Mod as u8,
                    BinOpKind::Eq   => Opcode::CmpEq as u8, BinOpKind::Ne  => Opcode::CmpNe as u8,
                    BinOpKind::Lt   => Opcode::CmpLt as u8, BinOpKind::Le  => Opcode::CmpLe as u8,
                    BinOpKind::Gt   => Opcode::CmpGt as u8, BinOpKind::Ge  => Opcode::CmpGe as u8,
                    BinOpKind::And  => Opcode::And as u8,   BinOpKind::Or  => Opcode::Or as u8,
                    BinOpKind::BAnd => Opcode::BAnd as u8,  BinOpKind::BOr => Opcode::BOr as u8,
                    BinOpKind::BXor => Opcode::BXor as u8,
                    BinOpKind::Shl  => Opcode::Shl as u8,   BinOpKind::Shr => Opcode::Shr as u8,
                });
            }
            Expr::Call(nm, args) => {
                if self.emit_intrinsic_call(nm, args) { return; }
                for arg in args { self.emit_expr(arg); }
                self.eu8(Opcode::Push as u8); self.ei32(args.len() as i32);
                self.eu8(Opcode::Call as u8);
                let off = self.code.len(); self.eu32(0);
                self.fn_patches.push((off, nm.clone()));
            }
        }
    }

    fn emit_stmt(&mut self, s: &Stmt) {
        match s {
            Stmt::Let(nm, e, _, _) | Stmt::Assign(nm, e) => {
                self.emit_expr(e);
                let i = self.local(nm);
                self.eu8(Opcode::StoreLocal as u8); self.ei32(i);
            }
            Stmt::LetArr(nm, _count, e, _, _) => {
                self.emit_expr(e);
                let i = self.local(nm);
                self.eu8(Opcode::StoreLocal as u8); self.ei32(i);
            }
            Stmt::AssignIdx(nm, idx, val) => {
                let i = self.local(nm); self.eu8(Opcode::LoadLocal as u8); self.ei32(i);
                self.emit_expr(val);
                self.emit_expr(idx);
                self.eu8(Opcode::ArrStore as u8);
            }
            Stmt::CompoundAssign(nm, op, e) => {
                let i = self.local(nm);
                self.eu8(Opcode::LoadLocal as u8); self.ei32(i);
                self.emit_expr(e);
                self.eu8(match op {
                    BinOpKind::Add  => Opcode::Add as u8,  BinOpKind::Sub => Opcode::Sub as u8,
                    BinOpKind::Mul  => Opcode::Mul as u8,  BinOpKind::Div => Opcode::Div as u8,
                    BinOpKind::Mod  => Opcode::Mod as u8,
                    BinOpKind::BAnd => Opcode::BAnd as u8, BinOpKind::BOr => Opcode::BOr as u8,
                    BinOpKind::BXor => Opcode::BXor as u8,
                    _ => Opcode::Add as u8,
                });
                self.eu8(Opcode::StoreLocal as u8); self.ei32(i);
            }
            Stmt::Print(e) => {
                self.emit_expr(e);
                let op = match e { Expr::StrLit(_) => Opcode::Print as u8, _ => Opcode::PrintNum as u8 };
                self.eu8(op);
            }
            Stmt::PrintHex(e) => { self.emit_expr(e); self.eu8(Opcode::PrintHex as u8); }
            Stmt::Return(e, _) => { self.emit_expr(e); self.eu8(Opcode::Ret as u8); }
            Stmt::Expr(e, _, _) => { self.emit_expr(e); self.eu8(Opcode::Pop as u8); }

            Stmt::AsmStmt(s) => {
                match s.trim() {
                    "cli"     => self.eu8(Opcode::AsmCli as u8),
                    "sti"     => self.eu8(Opcode::AsmSti as u8),
                    "nop"     => self.eu8(Opcode::AsmNop as u8),
                    "hlt"     => self.eu8(Opcode::AsmHlt as u8),
                    "pushfd"  => self.eu8(Opcode::AsmPushfd as u8),
                    "popfd"   => self.eu8(Opcode::AsmPopfd as u8),
                    "lidt"    => self.eu8(Opcode::AsmLidt as u8),
                    "lgdt"    => self.eu8(Opcode::AsmLgdt as u8),
                    "wbinvd"  => self.eu8(Opcode::AsmWbinvd as u8),
                    "invlpg"  => self.eu8(Opcode::AsmInvlpg as u8),
                    "get_cr2" => self.eu8(Opcode::GetCr2 as u8),
                    "clts"    => self.eu8(Opcode::AsmClts as u8),
                    other => {
                        if other.starts_with("int ") || other.starts_with("int\t") {
                            let rest = other[4..].trim();
                            let n = if rest.starts_with("0x") {
                                u8::from_str_radix(&rest[2..], 16).unwrap_or(0)
                            } else { rest.parse().unwrap_or(0) };
                            self.eu8(Opcode::AsmInt as u8); self.eu8(n);
                        } else {
                            self.eu8(Opcode::AsmNop as u8);
                        }
                    }
                }
            }

            Stmt::UnsafeBlock(body) => { for s in body { self.emit_stmt(s); } }

            Stmt::AssertStmt(cond, msg, _line) => {
                // v5: emit cond, then Assert opcode; VM halts with E012 if cond == 0
                self.emit_expr(cond);
                // push message string (or empty) before Assert so VM can print it
                if let Some(m) = msg {
                    let ci = self.add_const(m);
                    self.eu8(Opcode::PushStr as u8); self.eu16(ci);
                } else {
                    let ci = self.add_const("assertion failed");
                    self.eu8(Opcode::PushStr as u8); self.eu16(ci);
                }
                self.eu8(Opcode::Assert as u8);
            }

            Stmt::ConstDef(_, _) => { /* constants inlined at use site; no code emitted */ }

            Stmt::If(cond, then_b, else_b) => {
                self.emit_expr(cond);
                self.eu8(Opcode::Jz as u8); let jz = self.code.len(); self.eu32(0);
                for s in then_b { self.emit_stmt(s); }
                self.eu8(Opcode::Jmp as u8); let jmp = self.code.len(); self.eu32(0);
                let ea = self.addr(); self.patch(jz, ea);
                for s in else_b { self.emit_stmt(s); }
                let end = self.addr(); self.patch(jmp, end);
            }

            Stmt::While(cond, body) => {
                let top = self.addr();
                self.loop_push(top);
                self.emit_expr(cond);
                self.eu8(Opcode::Jz as u8); let exit = self.code.len(); self.eu32(0);
                for s in body { self.emit_stmt(s); }
                let cont = self.addr();
                self.eu8(Opcode::Jmp as u8); self.eu32(top);
                let after = self.addr(); self.patch(exit, after);
                self.loop_pop(after, cont);
            }

            Stmt::Loop(body) => {
                let top = self.addr();
                self.loop_push(top);
                for s in body { self.emit_stmt(s); }
                let cont = self.addr();
                self.eu8(Opcode::Jmp as u8); self.eu32(top);
                let after = self.addr();
                self.loop_pop(after, cont);
            }

            Stmt::For { var, start, end, step, kind, body } => {
                self.emit_expr(start);
                let vi = self.local(var);
                self.eu8(Opcode::StoreLocal as u8); self.ei32(vi);
                let top = self.addr();
                self.loop_push(top);
                self.eu8(Opcode::LoadLocal as u8); self.ei32(vi);
                self.emit_expr(end);
                self.eu8(match kind { RangeKind::Exclusive => Opcode::CmpLt as u8, RangeKind::Inclusive => Opcode::CmpLe as u8 });
                self.eu8(Opcode::Jz as u8); let exit = self.code.len(); self.eu32(0);
                for s in body { self.emit_stmt(s); }
                let cont = self.addr();
                // increment by step (default 1)
                self.eu8(Opcode::LoadLocal as u8); self.ei32(vi);
                if let Some(s) = step {
                    self.emit_expr(s);
                } else {
                    self.eu8(Opcode::Push as u8); self.ei32(1);
                }
                self.eu8(Opcode::Add as u8);
                self.eu8(Opcode::StoreLocal as u8); self.ei32(vi);
                self.eu8(Opcode::Jmp as u8); self.eu32(top);
                let after = self.addr(); self.patch(exit, after);
                self.loop_pop(after, cont);
            }

            Stmt::Match(scrutinee, arms) => {
                self.emit_expr(scrutinee);
                let mut end_patches: Vec<usize> = Vec::new();
                let mut next_patch: Option<usize> = None;
                for arm in arms {
                    if let Some(off) = next_patch {
                        let here = self.addr(); self.patch(off, here);
                    }
                    match &arm.pattern {
                        MatchPat::Wildcard => {
                            self.eu8(Opcode::Pop as u8);
                            for s in &arm.body { self.emit_stmt(s); }
                            next_patch = None;
                        }
                        MatchPat::Lit(n) => {
                            self.eu8(Opcode::Dup as u8);
                            self.eu8(Opcode::Push as u8); self.ei32(*n);
                            self.eu8(Opcode::CmpEq as u8);
                            self.eu8(Opcode::Jz as u8); let miss = self.code.len(); self.eu32(0);
                            self.eu8(Opcode::Pop as u8);
                            for s in &arm.body { self.emit_stmt(s); }
                            self.eu8(Opcode::Jmp as u8); let ep = self.code.len(); self.eu32(0);
                            end_patches.push(ep);
                            next_patch = Some(miss);
                        }
                        MatchPat::StrLit(s) => {
                            // v4: str match — emit StrCmp
                            self.eu8(Opcode::Dup as u8);
                            let ci = self.add_const(s); self.eu8(Opcode::PushStr as u8); self.eu16(ci);
                            self.eu8(Opcode::StrCmp as u8);
                            self.eu8(Opcode::Jz as u8); let miss = self.code.len(); self.eu32(0);
                            self.eu8(Opcode::Pop as u8);
                            for s2 in &arm.body { self.emit_stmt(s2); }
                            self.eu8(Opcode::Jmp as u8); let ep = self.code.len(); self.eu32(0);
                            end_patches.push(ep);
                            next_patch = Some(miss);
                        }
                    }
                }
                if let Some(off) = next_patch {
                    let here = self.addr(); self.patch(off, here);
                    self.eu8(Opcode::Pop as u8);
                }
                let end = self.addr();
                for ep in end_patches { self.patch(ep, end); }
            }

            Stmt::Break => {
                self.eu8(Opcode::Jmp as u8); let off = self.code.len(); self.eu32(0);
                if let Some(ctx) = self.loops.last_mut() { ctx.break_patches.push(off); }
            }
            Stmt::BreakVal(val) => {
                // v4: push value then jump; break's patch site is in break_val_patches
                self.emit_expr(val);
                self.eu8(Opcode::Jmp as u8); let off = self.code.len(); self.eu32(0);
                if let Some(ctx) = self.loops.last_mut() { ctx.break_val_patches.push(off); }
            }
            Stmt::Continue => {
                self.eu8(Opcode::Jmp as u8); let off = self.code.len(); self.eu32(0);
                if let Some(ctx) = self.loops.last_mut() { ctx.continue_patches.push(off); }
            }
        }
    }

    fn emit_fn(&mut self, f: &FnDef) {
        self.locals.clear();
        for p in &f.params { self.local(p); }
        let addr = self.addr(); self.fn_addrs.push((f.name.clone(), addr));
        for s in &f.body { self.emit_stmt(s); }
        // implicit return 0 at end of every function
        self.eu8(Opcode::Push as u8); self.ei32(0); self.eu8(Opcode::Ret as u8);
    }

    fn emit_program(mut self, fns: &[FnDef]) -> (Vec<u8>, Vec<Vec<u8>>, u32) {
        for f in fns { self.emit_fn(f); }
        self.patch_calls();
        let entry = self.fn_addrs.iter().find(|(n, _)| n == "main").map(|(_, a)| *a).unwrap_or(0);
        (self.code, self.consts, entry)  // `self.consts` here is the const-pool (strings), not ConstDef vec
    }
}

// ============================================================================
//  SECTION 8 - RXE PACKER, VALIDATOR & DISASSEMBLER
// ============================================================================

fn pack_rxe(code: &[u8], consts: &[Vec<u8>], entry: u32) -> Vec<u8> {
    let mut data: Vec<u8> = Vec::with_capacity(256);
    data.extend_from_slice(&(consts.len() as u16).to_le_bytes());
    for s in consts {
        data.extend_from_slice(&(s.len() as u16).to_le_bytes());
        data.extend_from_slice(s);
    }
    let hdr = RxeHeader::new(entry, code.len() as u32, data.len() as u32);
    let mut out: Vec<u8> = Vec::with_capacity(32 + 5 + code.len() + 5 + data.len() + 1);
    out.extend_from_slice(&hdr.to_bytes());
    out.push(RXE_SECTION_CODE); out.extend_from_slice(&(code.len() as u32).to_le_bytes()); out.extend_from_slice(code);
    out.push(RXE_SECTION_DATA); out.extend_from_slice(&(data.len() as u32).to_le_bytes()); out.extend_from_slice(&data);
    out.push(RXE_SECTION_END);
    out
}

fn rxe_validate(d: &[u8]) -> Result<(u32, usize, usize), &'static str> {
    if d.len() < 32             { return Err("RXE: file too small"); }
    if &d[0..8] != &RXE_MAGIC  { return Err("RXE: invalid magic - not a RadiumOS executable"); }
    if d[8] != RXE_VERSION && d[8] != 0x05 { return Err("RXE: unsupported version (expected v5 or v6)"); }
    if d[9] != RXE_ARCH_I686   { return Err("RXE: wrong arch (expected i686)"); }
    let ep = u32::from_le_bytes([d[10], d[11], d[12], d[13]]);
    let cs = u32::from_le_bytes([d[14], d[15], d[16], d[17]]) as usize;
    let ds = u32::from_le_bytes([d[18], d[19], d[20], d[21]]) as usize;
    Ok((ep, cs, ds))
}

// NEW v4: RXE disassembler ────────────────────────────────────────────────────
unsafe fn disasm_rxe(rxe: &[u8]) {
    let (entry, _, _) = match rxe_validate(rxe) {
        Ok(v) => v,
        Err(e) => { kprintln_col(COL_RED, e.as_bytes()); return; }
    };
    let mut pos = 32usize;
    let mut code: &[u8] = &[];
    while pos < rxe.len() {
        let sec = rxe[pos]; pos += 1;
        if sec == RXE_SECTION_END { break; }
        if pos + 4 > rxe.len() { break; }
        let len = u32::from_le_bytes([rxe[pos], rxe[pos+1], rxe[pos+2], rxe[pos+3]]) as usize;
        pos += 4;
        if pos + len > rxe.len() { break; }
        if sec == RXE_SECTION_CODE { code = &rxe[pos..pos+len]; }
        pos += len;
    }
    terminal_setcolor(COL_CYAN);
    kprintln(b"  HELIOS DISASSEMBLER v6.0");
    kprint(b"  entry: "); kprint_u32_hex(entry); kputc(b'\n');
    kprint(b"  code:  "); kprint_i32(code.len() as i32); kprintln(b" bytes");
    terminal_setcolor(COL_WHITE);
    kprintln(b"");
    let mut pc = 0usize;
    while pc < code.len() {
        let raw = code[pc];
        // address gutter
        terminal_setcolor(COL_GREY);
        kprint_u32_hex(pc as u32); kprint(b"  ");
        terminal_setcolor(COL_WHITE);
        // mark entry point
        if pc == entry as usize {
            terminal_setcolor(COL_GREEN); kprint(b"<main> "); terminal_setcolor(COL_WHITE);
        }
        pc += 1;
        if let Some(op) = Opcode::from_u8(raw) {
            terminal_setcolor(COL_CYAN); kprint(op.name().as_bytes()); terminal_setcolor(COL_WHITE);
            let imm = op.imm_bytes();
            if imm == 4 && pc + 4 <= code.len() {
                let v = i32::from_le_bytes([code[pc], code[pc+1], code[pc+2], code[pc+3]]);
                kputc(b' '); kprint_i32(v);
                pc += 4;
            } else if imm == 2 && pc + 2 <= code.len() {
                let v = u16::from_le_bytes([code[pc], code[pc+1]]);
                kputc(b' '); kprint_i32(v as i32);
                pc += 2;
            } else if imm == 1 && pc < code.len() {
                kputc(b' '); kprint_u32_hex(code[pc] as u32);
                pc += 1;
            }
        } else {
            terminal_setcolor(COL_RED);
            kprint(b"??? "); kprint_u32_hex(raw as u32);
            terminal_setcolor(COL_WHITE);
        }
        kputc(b'\n');
    }
    kprintln(b"");
}

// NEW v4: RXE header info ─────────────────────────────────────────────────────
unsafe fn info_rxe(rxe: &[u8]) {
    if rxe.len() < 32 { kprintln_col(COL_RED, b"info: file too small"); return; }
    terminal_setcolor(COL_CYAN); kprintln(b"  RXE HEADER"); terminal_setcolor(COL_WHITE);
    kprint(b"  magic:   "); kprint(&rxe[0..8]); kputc(b'\n');
    kprint(b"  version: "); kprint_u32_hex(rxe[8] as u32); kputc(b'\n');
    kprint(b"  arch:    "); kprint_u32_hex(rxe[9] as u32); kputc(b'\n');
    let ep = u32::from_le_bytes([rxe[10], rxe[11], rxe[12], rxe[13]]);
    kprint(b"  entry:   "); kprint_u32_hex(ep); kputc(b'\n');
    let cs = u32::from_le_bytes([rxe[14], rxe[15], rxe[16], rxe[17]]);
    let ds = u32::from_le_bytes([rxe[18], rxe[19], rxe[20], rxe[21]]);
    kprint(b"  code:    "); kprint_i32(cs as i32); kprintln(b" bytes");
    kprint(b"  data:    "); kprint_i32(ds as i32); kprintln(b" bytes");
    kprint(b"  flags:   "); kprint_u32_hex(u16::from_le_bytes([rxe[22], rxe[23]]) as u32); kputc(b'\n');
    kprint(b"  author:  "); kprint(&rxe[24..32]); kputc(b'\n');
    kputc(b'\n');
}

// ============================================================================
//  SECTION 9 - VM  (v4: heap-allocated stack, step counter, full StrCat/Sub/Find)
// ============================================================================

// VM execution limits
const VM_STACK_SLOTS:          usize = 1024 * 1024; // 1 M i32 slots = 4 MB value stack
const VM_DEPTH:                usize = 256;
const VM_REGS:                 usize = 16;
const VM_MAX_STEPS:            u64   = 50_000_000;  // halt after 50 M instructions
// v5: pre-allocate a 16 KB reserve block that is freed on first unsafe op entry.
// This ensures the kernel allocator has at least one free block available when
// the VM executes asm!() or unsafe pointer ops, preventing spurious OOM panics.
const VM_UNSAFE_STACK_RESERVE: u32   = 16 * 1024;   // 16 KB reserve

struct Frame { ret: u32, fp: usize }

struct Vm<'a> {
    code:       &'a [u8],
    consts:     Vec<&'a [u8]>,
    // FIX: stack is heap-allocated via kernel malloc, not Rust Box, avoiding
    // Rust stack overflow when compiling/running complex programs
    stack:      *mut i32,
    stack_cap:  usize,
    sp:         usize,
    fp:         usize,
    frames:     [Option<Frame>; VM_DEPTH],
    csp:        usize,
    pc:         usize,
    regs:       [i32; VM_REGS],
    verbosity:  u8,
    // v4 instrumentation
    steps:      u64,
    bytes_alloc:u64,
    bytes_free: u64,
    // v5: unsafe reserve block — freed on first unsafe/asm op to guarantee
    // the kernel allocator has headroom; null if already freed or alloc failed
    unsafe_reserve:  *mut u8,
    unsafe_op_count: u64,
    keyboard_capture_active: bool,
    // v5: configurable stack size (set by `-stack <KB>` shell flag)
    stack_size_kb:   u32,
}
impl<'a> Vm<'a> {
    const NF: Option<Frame> = None;

    // Allocate value stack on the kernel heap to avoid blowing the Rust call stack
    unsafe fn new(code: &'a [u8], consts: Vec<&'a [u8]>, v: u8, stack_kb: u32) -> Option<Self> {
        let slots = if stack_kb > 0 { (stack_kb as usize * 1024) / 4 } else { VM_STACK_SLOTS };
        let bytes = slots * 4;
        let stack = malloc(bytes as u32);
        if stack.is_null() { return None; }
        // Zero the stack (malloc doesn't guarantee it)
        core::ptr::write_bytes(stack, 0, bytes);
        // v5: pre-allocate unsafe reserve block so asm ops don't hit OOM
        let unsafe_reserve = malloc(VM_UNSAFE_STACK_RESERVE);
        // (null is acceptable — means we couldn't grab the reserve, ops still proceed)
        Some(Vm {
            code, consts,
            stack: stack as *mut i32,
            stack_cap: slots,
            sp: 0, fp: 0,
            frames: [Self::NF; VM_DEPTH],
            csp: 0, pc: 0,
            regs: [0i32; VM_REGS],
            verbosity: v,
            steps: 0,
            bytes_alloc: 0,
            bytes_free: 0,
            unsafe_reserve,
            unsafe_op_count: 0,
            keyboard_capture_active: false,
            stack_size_kb: stack_kb,
        })
    }

    // v5: release the unsafe reserve on first unsafe operation entry
    #[inline]
    unsafe fn release_unsafe_reserve(&mut self) {
        if !self.unsafe_reserve.is_null() {
            free(self.unsafe_reserve);
            self.unsafe_reserve = core::ptr::null_mut();
        }
    }

    unsafe fn drop_stack(&self) {
        if !self.stack.is_null() { free(self.stack as *mut u8); }
        // If the reserve was never used (program had no unsafe ops), free it now
        if !self.unsafe_reserve.is_null() { free(self.unsafe_reserve); }
    }

    #[inline(always)] unsafe fn push(&mut self, v: i32) {
        if self.sp < self.stack_cap { *self.stack.add(self.sp) = v; self.sp += 1; }
    }
    #[inline(always)] unsafe fn pop(&mut self) -> i32 {
        if self.sp > 0 { self.sp -= 1; *self.stack.add(self.sp) } else { 0 }
    }
    #[inline(always)] unsafe fn top(&self) -> i32 {
        if self.sp > 0 { *self.stack.add(self.sp - 1) } else { 0 }
    }
    #[inline(always)] unsafe fn stack_at(&self, i: usize) -> i32 {
        if i < self.stack_cap { *self.stack.add(i) } else { 0 }
    }
    #[inline(always)] unsafe fn stack_set(&mut self, i: usize, v: i32) {
        if i < self.stack_cap { *self.stack.add(i) = v; }
    }

    fn rb(&mut self) -> u8  { let b = self.code.get(self.pc).copied().unwrap_or(0); self.pc += 1; b }
    fn ri(&mut self) -> i32 { i32::from_le_bytes([self.rb(), self.rb(), self.rb(), self.rb()]) }
    fn ru(&mut self) -> u32 { u32::from_le_bytes([self.rb(), self.rb(), self.rb(), self.rb()]) }
    fn rw(&mut self) -> u16 { u16::from_le_bytes([self.rb(), self.rb()]) }

    unsafe fn pstr(&self, idx: u16) {
        if let Some(s) = self.consts.get(idx as usize) {
            for &c in *s { if c == 0 { break; } terminal_putchar(c); }
        }
    }

    // Get a &[u8] slice for a string value on the stack (tag-bit encoded)
    unsafe fn get_str_slice_raw<'b>(&self, v: i32) -> &'b [u8] where 'a: 'b {
        if (v as u32 & 0x8000_0000) != 0 {
            let i = (v & 0x7FFF) as usize;
            if i < self.consts.len() {
                let full = self.consts[i];
                let len = full.iter().position(|&c| c == 0).unwrap_or(full.len());
                &full[..len]
            } else { b"" }
        } else { b"" }
    }

    unsafe fn vm_string_bytes<'b>(&'b self, value: i32) -> &'b [u8] {
        if (value as u32 & 0x8000_0000) != 0 {
            return self.get_str_slice_raw(value);
        }
        if value == 0 { return b""; }
        let ptr = value as *const u8;
        let mut len = 0usize;
        while len < 65536 && *ptr.add(len) != 0 { len += 1; }
        core::slice::from_raw_parts(ptr, len)
    }

    unsafe fn vm_cstr_ptr(&self, value: i32) -> *const u8 {
        if (value as u32 & 0x8000_0000) != 0 {
            let index = (value & 0x7FFF) as usize;
            self.consts.get(index).map_or(core::ptr::null(), |s| s.as_ptr())
        } else {
            value as *const u8
        }
    }

    // Allocate a temp heap buffer, copy src into it (null-terminated), return ptr
    unsafe fn heap_str_alloc(&mut self, src: &[u8]) -> *mut u8 {
        let sz = src.len() as u32 + 1;
        let p = malloc(sz);
        if !p.is_null() {
            core::ptr::copy_nonoverlapping(src.as_ptr(), p, src.len());
            *p.add(src.len()) = 0;
            self.bytes_alloc += sz as u64;
        }
        p
    }

    // Push a heap-allocated string as a ptr value (not a const-pool index).
    // The VM's Print opcode handles both cases via the tag bit.
    // For heap strings, we push the raw pointer cast to i32 (no tag bit).
    // On 32-bit RadiumOS this is fine.
    unsafe fn push_heap_str_ptr(&mut self, p: *mut u8) {
        self.push(p as i32);
    }

    unsafe fn run(&mut self, entry: u32) -> i32 {
        self.pc = entry as usize;
        loop {
            if self.pc >= self.code.len() { break; }
            self.steps += 1;
            if self.steps >= VM_MAX_STEPS {
                kprintln_col(COL_RED, b"[helios] execution timeout (50M steps) - infinite loop?");
                break;
            }
            let raw = self.rb();
            let op = match Opcode::from_u8(raw) { Some(o) => o, None => continue };

            match op {
                // ── Stack ─────────────────────────────────────────────────────
                Opcode::Push    => { let n = self.ri(); self.push(n); }
                Opcode::PushStr => { let i = self.rw(); self.push(i as i32 | (0x8000_0000_u32 as i32)); }
                Opcode::Pop     => { self.pop(); }
                Opcode::Dup     => { let v = self.top(); self.push(v); }
                Opcode::Swap    => { let a = self.pop(); let b = self.pop(); self.push(a); self.push(b); }
                Opcode::Over    => { if self.sp >= 2 { let v = self.stack_at(self.sp - 2); self.push(v); } }

                // ── Arithmetic ────────────────────────────────────────────────
                Opcode::Add  => { let b = self.pop(); let a = self.pop(); self.push(a.wrapping_add(b)); }
                Opcode::Sub  => { let b = self.pop(); let a = self.pop(); self.push(a.wrapping_sub(b)); }
                Opcode::Mul  => { let b = self.pop(); let a = self.pop(); self.push(a.wrapping_mul(b)); }
                Opcode::Div  => { let b = self.pop(); let a = self.pop(); self.push(if b != 0 { a / b } else { 0 }); }
                Opcode::Mod  => { let b = self.pop(); let a = self.pop(); self.push(if b != 0 { a % b } else { 0 }); }
                Opcode::Neg  => { let a = self.pop(); self.push(a.wrapping_neg()); }

                // ── Bitwise ───────────────────────────────────────────────────
                Opcode::BAnd => { let b = self.pop(); let a = self.pop(); self.push(a & b); }
                Opcode::BOr  => { let b = self.pop(); let a = self.pop(); self.push(a | b); }
                Opcode::BXor => { let b = self.pop(); let a = self.pop(); self.push(a ^ b); }
                Opcode::BNot => { let a = self.pop(); self.push(!a); }
                Opcode::Shl  => { let b = self.pop(); let a = self.pop(); self.push(a << (b & 31)); }
                Opcode::Shr  => { let b = self.pop(); let a = self.pop(); self.push(((a as u32) >> (b & 31)) as i32); }

                // ── Comparison ────────────────────────────────────────────────
                Opcode::CmpEq => { let b = self.pop(); let a = self.pop(); self.push((a == b) as i32); }
                Opcode::CmpNe => { let b = self.pop(); let a = self.pop(); self.push((a != b) as i32); }
                Opcode::CmpLt => { let b = self.pop(); let a = self.pop(); self.push((a <  b) as i32); }
                Opcode::CmpLe => { let b = self.pop(); let a = self.pop(); self.push((a <= b) as i32); }
                Opcode::CmpGt => { let b = self.pop(); let a = self.pop(); self.push((a >  b) as i32); }
                Opcode::CmpGe => { let b = self.pop(); let a = self.pop(); self.push((a >= b) as i32); }
                Opcode::And   => { let b = self.pop(); let a = self.pop(); self.push((a != 0 && b != 0) as i32); }
                Opcode::Or    => { let b = self.pop(); let a = self.pop(); self.push((a != 0 || b != 0) as i32); }
                Opcode::Not   => { let a = self.pop(); self.push((a == 0) as i32); }

                // ── Control flow ──────────────────────────────────────────────
                Opcode::Jmp  => { self.pc = self.ru() as usize; }
                Opcode::Jz   => { let a = self.ru(); if self.pop() == 0 { self.pc = a as usize; } }
                Opcode::Jnz  => { let a = self.ru(); if self.pop() != 0 { self.pc = a as usize; } }
                Opcode::Halt => break,
                Opcode::Call => {
                    let addr = self.ru(); let argc = self.pop() as usize;
                    if addr as usize >= self.code.len() {
                        kprintln_col(COL_RED, b"[helios vm] unresolved function call");
                        break;
                    }
                    if self.csp < VM_DEPTH {
                        self.frames[self.csp] = Some(Frame { ret: self.pc as u32, fp: self.fp });
                        self.csp += 1;
                    }
                    self.fp = self.sp - argc;
                    self.pc = addr as usize;
                }
                Opcode::Ret => {
                    let rv = self.pop();
                    if self.csp == 0 { break; }
                    self.csp -= 1;
                    if let Some(f) = self.frames[self.csp].take() {
                        self.sp = self.fp; self.fp = f.fp; self.pc = f.ret as usize; self.push(rv);
                    } else { break; }
                }

                // ── Locals ────────────────────────────────────────────────────
                Opcode::LoadLocal  => {
                    let i = self.ri() as usize;
                    let t = self.fp + i;
                    let v = if t < self.stack_cap { self.stack_at(t) } else { 0 };
                    self.push(v);
                }
                Opcode::StoreLocal => {
                    let i = self.ri() as usize;
                    let v = self.pop();
                    let t = self.fp + i;
                    if t < self.stack_cap {
                        self.stack_set(t, v);
                        if t >= self.sp { self.sp = t + 1; }
                    }
                }

                // ── Register file ─────────────────────────────────────────────
                Opcode::LoadReg  => { let i = self.rb() as usize % VM_REGS; self.push(self.regs[i]); }
                Opcode::StoreReg => { let i = self.rb() as usize % VM_REGS; self.regs[i] = self.pop(); }

                // ── Heap ──────────────────────────────────────────────────────
                Opcode::HeapAlloc => {
                    let sz = self.pop() as u32;
                    let p = malloc(sz);
                    if p.is_null() {
                        kprintln_col(COL_RED, b"[helios vm] heap alloc failed - out of memory");
                    } else {
                        self.bytes_alloc += sz as u64;
                    }
                    self.push(p as i32);
                }
                Opcode::HeapFree  => {
                    let p = self.pop() as *mut u8;
                    if !p.is_null() { free(p); }
                }
                Opcode::ArrLoad   => {
                    let idx = self.pop() as usize;
                    let ptr = self.pop() as *const i32;
                    self.push(if !ptr.is_null() { *ptr.add(idx) } else { 0 });
                }
                Opcode::ArrStore  => {
                    let idx = self.pop() as usize;
                    let val = self.pop();
                    let ptr = self.pop() as *mut i32;
                    if !ptr.is_null() { *ptr.add(idx) = val; }
                }

                // ── I/O ───────────────────────────────────────────────────────
                Opcode::Print => {
                    let v = self.pop();
                    if (v as u32 & 0x8000_0000) != 0 {
                        self.pstr((v & 0x7FFF) as u16);
                    } else {
                        // Could be a heap string pointer or an int
                        kprint_i32(v);
                    }
                }
                Opcode::PrintNum  => { kprint_i32(self.pop()); }
                Opcode::PrintChar => { terminal_putchar(self.pop() as u8); }
                Opcode::PrintHex  => { kprint_u32_hex(self.pop() as u32); }
                Opcode::Input => {
                    // v5: graceful OOM — if malloc fails, push empty-string const instead of panicking
                    let buf_ptr = malloc(256);
                    if buf_ptr.is_null() {
                        kprintln_col(COL_RED, b"[helios vm] input: OOM - returning empty string");
                        // push empty string tag (const index 0 with tag bit; safe because even an
                        // empty const pool returns b"" from get_str_slice_raw)
                        self.push(0x8000_0000_u32 as i32);
                    } else {
                        core::ptr::write_bytes(buf_ptr, 0, 256);
                        let status = keyboard_input(buf_ptr);
                        // Ctrl+C / error: zero out buffer so we push a valid empty C-string
                        if status < 0 { *buf_ptr = 0; }
                        self.push(buf_ptr as i32);
                    }
                }
                Opcode::PollScancode => {
                    if !self.keyboard_capture_active {
                        keyboard_scancode_capture_begin();
                        self.keyboard_capture_active = true;
                    }
                    self.push(keyboard_poll_scancode());
                }
                

                // ── FS ────────────────────────────────────────────────────────
                Opcode::FileExist => {
                    let v = self.pop();
                    let r = if (v as u32 & 0x8000_0000) != 0 {
                        let i = (v & 0x7FFF) as usize;
                        if i < self.consts.len() { avfs_file_exists(self.consts[i].as_ptr()) as i32 } else { 0 }
                    } else { 0 };
                    self.push(r);
                }
                Opcode::FileSize => {
                    let v = self.pop();
                    let r = if (v as u32 & 0x8000_0000) != 0 {
                        let i = (v & 0x7FFF) as usize;
                        if i < self.consts.len() { avfs_get_filesize(self.consts[i].as_ptr()) } else { -1 }
                    } else { -1 };
                    self.push(r);
                }
                Opcode::FileRead  => { self.pop(); self.pop(); self.push(-1); }
                Opcode::FileWrite => { self.pop(); self.pop(); self.push(-1); }

                // ── Hardware - I/O ports ──────────────────────────────────────
                // v5: release unsafe reserve before any hardware/asm op to free memory headroom
                Opcode::In8  => { self.release_unsafe_reserve(); self.unsafe_op_count += 1; let p = self.pop() as u16; let mut v: u8 =0; core::arch::asm!("in al, dx",  out("al")v,   in("dx")p, options(nostack,preserves_flags)); self.push(v as i32); }
                Opcode::Out8 => { let v = self.pop() as u8; let p = self.pop() as u16; core::arch::asm!("out dx, al", in("dx")p, in("al")v, options(nostack,preserves_flags)); }
                Opcode::In16 => { let p = self.pop() as u16; let mut v: u16=0; core::arch::asm!("in ax, dx",  out("ax")v,   in("dx")p, options(nostack,preserves_flags)); self.push(v as i32); }
                Opcode::Out16=> { let v = self.pop() as u16; let p = self.pop() as u16; core::arch::asm!("out dx, ax", in("dx")p, in("ax")v, options(nostack,preserves_flags)); }
                Opcode::In32 => { let p = self.pop() as u16; let mut v: u32=0; core::arch::asm!("in eax, dx",out("eax")v,  in("dx")p, options(nostack,preserves_flags)); self.push(v as i32); }
                Opcode::Out32=> { let v = self.pop() as u32; let p = self.pop() as u16; core::arch::asm!("out dx, eax",in("dx")p,in("eax")v, options(nostack,preserves_flags)); }

                // ── Hardware - control ────────────────────────────────────────
                Opcode::Cli => { core::arch::asm!("cli", options(nostack,preserves_flags)); }
                Opcode::Sti => { core::arch::asm!("sti", options(nostack,preserves_flags)); }
                Opcode::Hlt => { core::arch::asm!("hlt", options(nostack,preserves_flags)); }
                Opcode::Rdtsc => {
                    let lo: u32;
                    core::arch::asm!("rdtsc", out("eax")lo, out("edx")_, options(nostack,preserves_flags));
                    self.push(lo as i32);
                }

                // ── Hardware - memory ─────────────────────────────────────────
                Opcode::MemCpy => { let n=self.pop() as usize; let src=self.pop() as *const u8; let dst=self.pop() as *mut u8; core::ptr::copy_nonoverlapping(src, dst, n); }
                Opcode::MemSet => { let n=self.pop() as usize; let v=self.pop() as u8; let dst=self.pop() as *mut u8; core::ptr::write_bytes(dst, v, n); }
                Opcode::AllocPage => { let n=self.pop() as u32; self.push(malloc(n) as i32); }
                Opcode::FreePage  => { free(self.pop() as *mut u8); }

                // ── Hardware - CR registers ───────────────────────────────────
                Opcode::GetCr0 => { let v: u32; core::arch::asm!("mov {}, cr0", out(reg)v, options(nostack,preserves_flags)); self.push(v as i32); }
                Opcode::SetCr0 => { let v=self.pop() as u32; core::arch::asm!("mov cr0, {}", in(reg)v, options(nostack,preserves_flags)); }
                Opcode::GetCr3 => { let v: u32; core::arch::asm!("mov {}, cr3", out(reg)v, options(nostack,preserves_flags)); self.push(v as i32); }
                Opcode::SetCr3 => { let v=self.pop() as u32; core::arch::asm!("mov cr3, {}", in(reg)v, options(nostack,preserves_flags)); }
                Opcode::DebugBreak => { core::arch::asm!("int3", options(nostack,preserves_flags)); }

                // ── Hardware - physical peek/poke ─────────────────────────────
                Opcode::Peek8  => { let a=self.pop() as usize; self.push(*(a as *const u8)  as i32); }
                Opcode::Poke8  => { let v=self.pop() as u8;  let a=self.pop() as usize; *(a as *mut u8)  = v; }
                Opcode::Peek16 => { let a=self.pop() as usize; self.push(*(a as *const u16) as i32); }
                Opcode::Poke16 => { let v=self.pop() as u16; let a=self.pop() as usize; *(a as *mut u16) = v; }
                Opcode::Peek32 => { let a=self.pop() as usize; self.push(*(a as *const u32) as i32); }
                Opcode::Poke32 => { let v=self.pop() as u32; let a=self.pop() as usize; *(a as *mut u32) = v; }

                // ── Hardware - segment registers ──────────────────────────────
                Opcode::GetCs  => { let v: u16; core::arch::asm!("mov {0:x}, cs", out(reg)v, options(nostack,preserves_flags)); self.push(v as i32); }
                Opcode::GetDs  => { let v: u16; core::arch::asm!("mov {0:x}, ds", out(reg)v, options(nostack,preserves_flags)); self.push(v as i32); }
                Opcode::GetEs  => { let v: u16; core::arch::asm!("mov {0:x}, es", out(reg)v, options(nostack,preserves_flags)); self.push(v as i32); }
                Opcode::GetFs  => { let v: u16; core::arch::asm!("mov {0:x}, fs", out(reg)v, options(nostack,preserves_flags)); self.push(v as i32); }
                Opcode::GetGs  => { let v: u16; core::arch::asm!("mov {0:x}, gs", out(reg)v, options(nostack,preserves_flags)); self.push(v as i32); }
                Opcode::GetSs  => { let v: u16; core::arch::asm!("mov {0:x}, ss", out(reg)v, options(nostack,preserves_flags)); self.push(v as i32); }
                Opcode::SetFs  => { let v=self.pop() as u16; core::arch::asm!("mov fs, {0:x}", in(reg)v, options(nostack,preserves_flags)); }
                // FIX: SetGs was missing from VM match entirely
                Opcode::SetGs  => { let v=self.pop() as u16; core::arch::asm!("mov gs, {0:x}", in(reg)v, options(nostack,preserves_flags)); }

                // ── Hardware - PIC ────────────────────────────────────────────
                Opcode::PicEoi => {
                    let irq = self.pop();
                    if irq >= 8 { core::arch::asm!("out 0xA0, al", in("al")0x20u8, options(nostack,preserves_flags)); }
                    core::arch::asm!("out 0x20, al", in("al")0x20u8, options(nostack,preserves_flags));
                }
                Opcode::PicMask => {
                    let irq = self.pop() as u8;
                    let (port, bit) = if irq < 8 { (0x21u16, irq) } else { (0xA1u16, irq-8) };
                    let mut m: u8 = 0;
                    core::arch::asm!("in al, dx", out("al")m, in("dx")port, options(nostack,preserves_flags));
                    m |= 1 << bit;
                    core::arch::asm!("out dx, al", in("dx")port, in("al")m, options(nostack,preserves_flags));
                }
                Opcode::PicUnmask => {
                    let irq = self.pop() as u8;
                    let (port, bit) = if irq < 8 { (0x21u16, irq) } else { (0xA1u16, irq-8) };
                    let mut m: u8 = 0;
                    core::arch::asm!("in al, dx", out("al")m, in("dx")port, options(nostack,preserves_flags));
                    m &= !(1 << bit);
                    core::arch::asm!("out dx, al", in("dx")port, in("al")m, options(nostack,preserves_flags));
                }

                // ── Hardware - PIT ────────────────────────────────────────────
                Opcode::PitSetHz => {
                    let hz = self.pop() as u32;
                    let div = if hz > 0 { (1193182 / hz) as u16 } else { 0xFFFF };
                    core::arch::asm!("out 0x43, al", in("al")0x36u8, options(nostack,preserves_flags));
                    core::arch::asm!("out 0x40, al", in("al")(div & 0xFF) as u8, options(nostack,preserves_flags));
                    core::arch::asm!("out 0x40, al", in("al")((div >> 8) & 0xFF) as u8, options(nostack,preserves_flags));
                }
                Opcode::GetTicks => { self.push(get_ticks() as i32); }
                Opcode::SleepMs  => { sleep_ms(self.pop() as u32); }

                // ── Hardware - CPUID ──────────────────────────────────────────
                Opcode::Cpuid => {
                    let ei = self.pop() as u32;
                    let (mut ea, mut eb, mut ec, mut ed): (u32, u32, u32, u32) = (0, 0, 0, 0);
                    core::arch::asm!("cpuid",
                        inout("eax")ei=>ea, out("ebx")eb, out("ecx")ec, out("edx")ed,
                        options(nostack,preserves_flags));
                    self.push(ed as i32); self.push(ec as i32); self.push(eb as i32); self.push(ea as i32);
                }

                // ── Hardware - MSR 32-bit ─────────────────────────────────────
                Opcode::RdMsr => {
                    let ecx = self.pop() as u32; let lo: u32;
                    core::arch::asm!("rdmsr", out("eax")lo, out("edx")_, in("ecx")ecx, options(nostack,preserves_flags));
                    self.push(lo as i32);
                }
                Opcode::WrMsr => {
                    let v=self.pop() as u32; let ecx=self.pop() as u32;
                    core::arch::asm!("wrmsr", in("ecx")ecx, in("eax")v, in("edx")0u32, options(nostack,preserves_flags));
                }

                // ── Hardware - MSR 64-bit ─────────────────────────────────────
                Opcode::RdMsr64 => {
                    let ecx=self.pop() as u32; let (lo, hi): (u32, u32);
                    core::arch::asm!("rdmsr", out("eax")lo, out("edx")hi, in("ecx")ecx, options(nostack,preserves_flags));
                    self.push(hi as i32); self.push(lo as i32);
                }
                Opcode::WrMsr64 => {
                    let lo=self.pop() as u32; let hi=self.pop() as u32; let ecx=self.pop() as u32;
                    core::arch::asm!("wrmsr", in("ecx")ecx, in("eax")lo, in("edx")hi, options(nostack,preserves_flags));
                }

                // ── Hardware - port burst ─────────────────────────────────────
                Opcode::PortBurstR => {
                    let count=self.pop() as u32; let dest=self.pop() as *mut u8; let port=self.pop() as u16;
                    let mut di=dest as u32; let mut cx=count;
                    core::arch::asm!("rep insb", inout("edi")di=>_, inout("ecx")cx=>_, in("dx")port, options(nostack,preserves_flags));
                }
                Opcode::PortBurstW => {
                    let count=self.pop() as u32; let src=self.pop() as *const u8; let port=self.pop() as u16;
                    let mut si=src as u32; let mut cx=count;
                    core::arch::asm!("xchg esi, {0}", "rep outsb", "xchg esi, {0}",
                        inout(reg)si=>_, inout("ecx")cx=>_, in("dx")port, options(nostack,preserves_flags));
                }
                Opcode::MemQuery => {
                    let kb = *(0x0413usize as *const u16) as i32;
                    self.push(kb);
                }

                // ── Hardware - descriptor tables ──────────────────────────────
                Opcode::Sgdt => {
                    let mut base: u32 = 0;
                    core::arch::asm!("sub esp, 6", "sgdt [esp]", "mov {}, [esp+2]", "add esp, 6",
                        out(reg)base, options(nostack,preserves_flags));
                    self.push(base as i32);
                }
                Opcode::Sidt => {
                    let mut base: u32 = 0;
                    core::arch::asm!("sub esp, 6", "sidt [esp]", "mov {}, [esp+2]", "add esp, 6",
                        out(reg)base, options(nostack,preserves_flags));
                    self.push(base as i32);
                }

                // ── Hardware - TSC serialised ─────────────────────────────────
                Opcode::Rdtscp => {
                    let (lo, hi, aux): (u32, u32, u32);
                    core::arch::asm!("rdtscp", out("eax")lo, out("edx")hi, out("ecx")aux, options(nostack,preserves_flags));
                    self.push(aux as i32); self.push(hi as i32); self.push(lo as i32);
                }

                // ── Hardware - IOPL ───────────────────────────────────────────
                Opcode::Iopl3 => {
                    let mut efl: u32;
                    core::arch::asm!("pushfd", "pop {}", out(reg)efl, options(nostack,preserves_flags));
                    efl |= 0x3000;
                    core::arch::asm!("push {}", "popfd", in(reg)efl, options(nostack,preserves_flags));
                }

                // ── Inline ASM ────────────────────────────────────────────────
                // v5: release unsafe reserve first so these ops never hit OOM
                Opcode::AsmNop  => { self.release_unsafe_reserve(); self.unsafe_op_count += 1; core::arch::asm!("nop", options(nostack,preserves_flags)); }
                Opcode::AsmCli  => { self.release_unsafe_reserve(); self.unsafe_op_count += 1; core::arch::asm!("cli", options(nostack,preserves_flags)); }
                Opcode::AsmSti  => { self.release_unsafe_reserve(); self.unsafe_op_count += 1; core::arch::asm!("sti", options(nostack,preserves_flags)); }
                Opcode::AsmInt  => { self.release_unsafe_reserve(); self.unsafe_op_count += 1;
                    let n = self.rb();
                    match n {
                        0x03 => core::arch::asm!("int 0x03", options(nostack,preserves_flags)),
                        0x10 => core::arch::asm!("int 0x10", options(nostack,preserves_flags)),
                        0x13 => core::arch::asm!("int 0x13", options(nostack,preserves_flags)),
                        0x15 => core::arch::asm!("int 0x15", options(nostack,preserves_flags)),
                        0x16 => core::arch::asm!("int 0x16", options(nostack,preserves_flags)),
                        0x1A => core::arch::asm!("int 0x1A", options(nostack,preserves_flags)),
                        0x20 => core::arch::asm!("int 0x20", options(nostack,preserves_flags)),
                        0x80 => core::arch::asm!("int 0x80", options(nostack,preserves_flags)),
                        _ => {}
                    }
                }

                // ── FPU / SSE ─────────────────────────────────────────────────
                Opcode::FxSave  => { let ptr=self.pop() as *mut u8; core::arch::asm!("fxsave [{}]",  in(reg)ptr, options(nostack,preserves_flags)); }
                Opcode::FxRstor => { let ptr=self.pop() as *const u8; core::arch::asm!("fxrstor [{}]", in(reg)ptr, options(nostack,preserves_flags)); }

                // ── v6: expanded inline asm opcodes ───────────────────────────
                Opcode::AsmHlt  => {
                    self.release_unsafe_reserve(); self.unsafe_op_count += 1;
                    core::arch::asm!("hlt", options(nostack, preserves_flags));
                }
                Opcode::AsmPushfd => {
                    self.release_unsafe_reserve(); self.unsafe_op_count += 1;
                    let mut efl: u32 = 0;
                    core::arch::asm!("pushfd", "pop {}", out(reg)efl, options(nostack, preserves_flags));
                    self.push(efl as i32);
                }
                Opcode::AsmPopfd => {
                    self.release_unsafe_reserve(); self.unsafe_op_count += 1;
                    let efl = self.pop() as u32;
                    core::arch::asm!("push {}", "popfd", in(reg)efl, options(nostack, preserves_flags));
                }
                Opcode::AsmLidt => {
                    self.release_unsafe_reserve(); self.unsafe_op_count += 1;
                    let ptr = self.pop() as *const u8;
                    if !ptr.is_null() {
                        core::arch::asm!("lidt [{}]", in(reg)ptr, options(nostack, preserves_flags));
                    }
                }
                Opcode::AsmLgdt => {
                    self.release_unsafe_reserve(); self.unsafe_op_count += 1;
                    let ptr = self.pop() as *const u8;
                    if !ptr.is_null() {
                        core::arch::asm!("lgdt [{}]", in(reg)ptr, options(nostack, preserves_flags));
                    }
                }
                Opcode::AsmWbinvd => {
                    self.release_unsafe_reserve(); self.unsafe_op_count += 1;
                    core::arch::asm!("wbinvd", options(nostack, preserves_flags));
                }
                Opcode::AsmInvlpg => {
                    self.release_unsafe_reserve(); self.unsafe_op_count += 1;
                    let addr = self.pop() as usize;
                    core::arch::asm!("invlpg [{}]", in(reg)addr, options(nostack, preserves_flags));
                }
                Opcode::GetCr2 => {
                    self.release_unsafe_reserve(); self.unsafe_op_count += 1;
                    let v: u32;
                    core::arch::asm!("mov {}, cr2", out(reg)v, options(nostack, preserves_flags));
                    self.push(v as i32);
                }
                Opcode::AsmClts => {
                    self.release_unsafe_reserve(); self.unsafe_op_count += 1;
                    core::arch::asm!("clts", options(nostack, preserves_flags));
                }
                // HwMapMem(base, len) — identity-maps [base, base+len) for peek/poke.
                // On RadiumOS the page tables are already identity-mapped for low physical
                // memory, so this is a no-op that validates the range and returns 0 (ok).
                Opcode::HwMapMem => {
                    let len  = self.pop() as u32;
                    let base = self.pop() as u32;
                    // Simple range check: only allow low 4 GB (always true on i686)
                    let ok: i32 = if len == 0 { -1 } else { 0 };
                    let _ = (base, len); // suppress unused warning
                    self.push(ok);
                }

                // ── Strings (v4: StrCat / StrSub / StrFind fully implemented) ─
                Opcode::StrLen => {
                    let v = self.pop();
                    let s = self.get_str_slice_raw(v);
                    self.push(s.len() as i32);
                }
                Opcode::StrCmp => {
                    let b = self.pop(); let a = self.pop();
                    let sa = self.get_str_slice_raw(a);
                    let sb = self.get_str_slice_raw(b);
                    self.push((sa == sb) as i32);
                }
                // FIX: StrCat now actually concatenates into a heap buffer
                Opcode::StrCat => {
                    let b = self.pop(); let a = self.pop();
                    let sa: &[u8] = self.get_str_slice_raw(a);
                    let sb: &[u8] = self.get_str_slice_raw(b);
                    let total = sa.len() + sb.len();
                    let p = malloc(total as u32 + 1);
                    if !p.is_null() {
                        core::ptr::copy_nonoverlapping(sa.as_ptr(), p, sa.len());
                        core::ptr::copy_nonoverlapping(sb.as_ptr(), p.add(sa.len()), sb.len());
                        *p.add(total) = 0;
                        self.bytes_alloc += total as u64 + 1;
                        self.push(p as i32); // heap ptr (no tag bit)
                    } else {
                        kprintln_col(COL_RED, b"[helios vm] strcat: OOM");
                        self.push(0);
                    }
                }
                // FIX: StrSub - extract substring(str, start, len)
                Opcode::StrSub => {
                    let sub_len = self.pop() as usize;
                    let start   = self.pop() as usize;
                    let v       = self.pop();
                    let s = self.get_str_slice_raw(v);
                    let actual_start = start.min(s.len());
                    let actual_len   = sub_len.min(s.len().saturating_sub(actual_start));
                    let slice = &s[actual_start..actual_start + actual_len];
                    let p = malloc(actual_len as u32 + 1);
                    if !p.is_null() {
                        core::ptr::copy_nonoverlapping(slice.as_ptr(), p, actual_len);
                        *p.add(actual_len) = 0;
                        self.bytes_alloc += actual_len as u64 + 1;
                        self.push(p as i32);
                    } else {
                        self.push(0);
                    }
                }
                // FIX: StrFind - find first occurrence of needle in haystack, returns -1 if not found
                Opcode::StrFind => {
                    let needle_v  = self.pop();
                    let haystack_v = self.pop();
                    let hay = self.get_str_slice_raw(haystack_v);
                    let ndl = self.get_str_slice_raw(needle_v);
                    if ndl.is_empty() {
                        self.push(0);
                    } else {
                        let mut found: i32 = -1;
                        'outer: for i in 0..hay.len() {
                            if i + ndl.len() > hay.len() { break; }
                            for j in 0..ndl.len() {
                                if hay[i+j] != ndl[j] { continue 'outer; }
                            }
                            found = i as i32; break;
                        }
                        self.push(found);
                    }
                }
                // FIX: IntToStr - convert integer to string in heap buffer
                Opcode::IntToStr => {
                    let n = self.pop();
                    let mut buf = [0u8; 16];
                    let s = i32_to_str(n, &mut buf);
                    let p = malloc(s.len() as u32 + 1);
                    if !p.is_null() {
                        core::ptr::copy_nonoverlapping(s.as_ptr(), p, s.len());
                        *p.add(s.len()) = 0;
                        self.bytes_alloc += s.len() as u64 + 1;
                        self.push(p as i32);
                    } else { self.push(0); }
                }
                Opcode::StrToInt => {
                    let v = self.pop();
                    let s = if (v as u32 & 0x8000_0000) != 0 {
                        self.get_str_slice_raw(v)
                    } else if v != 0 {
                        let ptr = v as *const u8;
                        let mut len = 0usize;
                        while len < 256 && *ptr.add(len) != 0 { len += 1; }
                        core::slice::from_raw_parts(ptr, len)
                    } else {
                        b""
                    };
                    let mut n = 0i32;
                    let mut neg = false;
                    let mut i = 0;
                    if !s.is_empty() && s[0] == b'-' { neg = true; i = 1; }
                    while i < s.len() && s[i] >= b'0' && s[i] <= b'9' {
                        n = n.wrapping_mul(10).wrapping_add((s[i] - b'0') as i32);
                        i += 1;
                    }
                    self.push(if neg { -n } else { n });
                }
                Opcode::StrByte => {
                    let v = self.pop();
                    let byte = if (v as u32 & 0x8000_0000) != 0 {
                        self.get_str_slice_raw(v).first().copied().unwrap_or(0)
                    } else if v != 0 {
                        *(v as *const u8)
                    } else {
                        0
                    };
                    self.push(byte as i32);
                }

                // v5: StrUpper — heap-copy string with all bytes uppercased
                Opcode::StrUpper => {
                    let v = self.pop();
                    let s = self.get_str_slice_raw(v);
                    let p = malloc(s.len() as u32 + 1);
                    if !p.is_null() {
                        for (i, &b) in s.iter().enumerate() {
                            *p.add(i) = if b.is_ascii_lowercase() { b - 32 } else { b };
                        }
                        *p.add(s.len()) = 0;
                        self.bytes_alloc += s.len() as u64 + 1;
                        self.push(p as i32);
                    } else { self.push(v); } // fallback: return original
                }
                // v5: StrLower — heap-copy string with all bytes lowercased
                Opcode::StrLower => {
                    let v = self.pop();
                    let s = self.get_str_slice_raw(v);
                    let p = malloc(s.len() as u32 + 1);
                    if !p.is_null() {
                        for (i, &b) in s.iter().enumerate() {
                            *p.add(i) = if b.is_ascii_uppercase() { b + 32 } else { b };
                        }
                        *p.add(s.len()) = 0;
                        self.bytes_alloc += s.len() as u64 + 1;
                        self.push(p as i32);
                    } else { self.push(v); }
                }
                // v5: PrintStr — prints a heap-ptr string (raw pointer, no tag-bit check)
                Opcode::PrintStr => {
                    let value = self.pop();
                    if (value as u32 & 0x8000_0000) != 0 {
                        self.pstr((value & 0x7FFF) as u16);
                    } else if value != 0 {
                        let ptr = value as *const u8;
                        let mut i = 0usize;
                        while *ptr.add(i) != 0 { terminal_putchar(*ptr.add(i)); i += 1; }
                    }
                }

                // ── Cast ──────────────────────────────────────────────────────
                Opcode::CastInt  => { /* value already correct type on stack */ }
                Opcode::CastBool => { let v=self.pop(); self.push((v != 0) as i32); }

                // v5: Assert — pop msg_idx then cond; halt with E012 if cond == 0
                Opcode::Assert => {
                    let msg_v = self.pop();
                    let cond  = self.pop();
                    if cond == 0 {
                        terminal_setcolor(COL_RED);
                        kprint(b"[helios] runtime error[E012]: assertion failed");
                        let msg_s = self.get_str_slice_raw(msg_v);
                        if !msg_s.is_empty() {
                            kprint(b": ");
                            for &c in msg_s { terminal_putchar(c); }
                        }
                        kputc(b'\n');
                        terminal_setcolor(COL_WHITE);
                        break; // halt the VM
                    }
                }

                Opcode::PrpSelfTest => {
                    let _argc = self.rb();
                    self.push(rust_prp_selftest());
                }
                Opcode::PrpRandom => {
                    let argc = self.rb() as usize;
                    let len = self.pop() as u32;
                    let output = self.pop() as *mut u8;
                    self.push(if argc == 2 { rust_prp_random(output, len) } else { -1 });
                }
                Opcode::PrpSha256File => {
                    let argc = self.rb() as usize;
                    let output = self.pop() as *mut u8;
                    let path = self.pop();
                    self.push(if argc == 2 { rust_prp_sha256_file(self.vm_cstr_ptr(path), output) } else { -1 });
                }
                Opcode::PrpSealFile | Opcode::PrpOpenFile => {
                    let argc = self.rb() as usize;
                    let key = self.pop(); let output = self.pop(); let input = self.pop();
                    let result = if argc != 3 { -1 } else if op == Opcode::PrpSealFile {
                        rust_prp_seal_file(self.vm_cstr_ptr(input), self.vm_cstr_ptr(output), self.vm_cstr_ptr(key))
                    } else {
                        rust_prp_open_file(self.vm_cstr_ptr(input), self.vm_cstr_ptr(output), self.vm_cstr_ptr(key))
                    };
                    self.push(result);
                }
                Opcode::PrpSealText => {
                    let argc = self.rb() as usize;
                    let output = self.pop(); let key = self.pop(); let text = self.pop();
                    let bytes = self.vm_string_bytes(text);
                    let result = if argc == 3 {
                        rust_prp_seal_text(self.vm_cstr_ptr(text), bytes.len() as u32,
                            self.vm_cstr_ptr(key), self.vm_cstr_ptr(output))
                    } else { -1 };
                    self.push(result);
                }
                Opcode::PrpFingerprint => {
                    let argc = self.rb() as usize;
                    let output = self.pop() as *mut u8; let keyfile = self.pop();
                    self.push(if argc == 2 { rust_prp_fingerprint(self.vm_cstr_ptr(keyfile), output) } else { -1 });
                }
                Opcode::PrpKeygen => {
                    let argc = self.rb() as usize; let name = self.pop();
                    self.push(if argc == 1 { rust_prp_keygen(self.vm_cstr_ptr(name)) } else { -1 });
                }
                Opcode::PrpSign => {
                    let argc = self.rb() as usize; let key = self.pop(); let file = self.pop();
                    self.push(if argc == 2 { rust_prp_sign(self.vm_cstr_ptr(file), key as *const u8) } else { -1 });
                }
                Opcode::PrpVerify => {
                    let argc = self.rb() as usize;
                    let expected_pub = if argc == 2 { self.pop() as *const u8 } else { core::ptr::null() };
                    let file = self.pop();
                    self.push(if argc == 1 || argc == 2 { rust_prp_verify(self.vm_cstr_ptr(file), expected_pub) } else { -1 });
                }
                Opcode::PrpSealFilePub => {
                    let argc = self.rb() as usize;
                    let public_key = self.pop(); let output = self.pop(); let input = self.pop();
                    self.push(if argc == 3 { rust_prp_seal_file_pub(self.vm_cstr_ptr(input),
                        self.vm_cstr_ptr(output), public_key as *const u8) } else { -1 });
                }
                Opcode::PrpOpenFilePrv => {
                    let argc = self.rb() as usize;
                    let private_key = self.pop(); let output = self.pop(); let input = self.pop();
                    self.push(if argc == 3 { rust_prp_open_file_prv(self.vm_cstr_ptr(input),
                        self.vm_cstr_ptr(output), private_key as *const u8) } else { -1 });
                }
                Opcode::PrpSealTextPub => {
                    let argc = self.rb() as usize;
                    let output = self.pop(); let public_key = self.pop(); let text = self.pop();
                    let bytes = self.vm_string_bytes(text);
                    self.push(if argc == 3 { rust_prp_seal_text_pub(self.vm_cstr_ptr(text),
                        bytes.len() as u32, public_key as *const u8, self.vm_cstr_ptr(output)) } else { -1 });
                }
                Opcode::FetchGet => {
                    let argc = self.rb() as usize;
                    let url = self.pop();
                    self.push(if argc == 1 { rust_https_get(self.vm_cstr_ptr(url)) } else { -1 });
                }
                Opcode::FetchActive => {
                    let argc = self.rb() as usize;
                    self.push((argc == 0 && rust_fetch_active()) as i32);
                }
                Opcode::FetchTest => {
                    let argc = self.rb() as usize;
                    self.push(if argc == 0 { rust_test_https() } else { -1 });
                }

                Opcode::Nop => {}
            }
        }

        if self.keyboard_capture_active {
            keyboard_scancode_capture_end();
            self.keyboard_capture_active = false;
        }

        if self.verbosity >= 1 {
            terminal_setcolor(COL_GREY);
            kprint(b"[helios vm] steps="); kprint_i32(self.steps as i32);
            kprint(b"  alloc="); kprint_i32(self.bytes_alloc as i32);
            kprint(b"B  free="); kprint_i32(self.bytes_free as i32); kputc(b'B');
            // v5: show unsafe op count when any were executed
            if self.unsafe_op_count > 0 {
                kprint(b"  unsafe_ops="); kprint_i32(self.unsafe_op_count as i32);
            }
            kputc(b'\n');
            terminal_setcolor(COL_WHITE);
        }

        if self.sp > 0 { self.stack_at(self.sp - 1) } else { 0 }
    }
}

fn parse_const_pool<'a>(data: &'a [u8]) -> Vec<&'a [u8]> {
    let mut out = Vec::new();
    if data.len() < 2 { return out; }
    let count = u16::from_le_bytes([data[0], data[1]]) as usize;
    let mut pos = 2;
    for _ in 0..count {
        if pos + 2 > data.len() { break; }
        let len = u16::from_le_bytes([data[pos], data[pos+1]]) as usize;
        pos += 2;
        if pos + len > data.len() { break; }
        out.push(&data[pos..pos+len]);
        pos += len;
    }
    out
}

// ============================================================================
//  SECTION 10 - COMPILE / EXEC / RUN API
// ============================================================================

// ── v6: include preprocessor ─────────────────────────────────────────────────
// Scans `source` for `include "file.hls";` directives and reads them via AVFS.
// Returns the merged source with include lines replaced by file content.
// Circular / missing includes emit a warning and are skipped.
pub unsafe fn helios_preprocess(source: &str, depth: u8) -> String {
    if depth > 8 { return source.into(); } // guard against include loops
    let bytes = source.as_bytes();
    let mut out = String::with_capacity(source.len() + 64);
    let mut i = 0usize;
    while i < bytes.len() {
        // skip leading whitespace to check for `include`
        let start = i;
        while i < bytes.len() && (bytes[i] == b' ' || bytes[i] == b'\t') { i += 1; }
        // match `include`
        if i + 7 <= bytes.len() && &bytes[i..i+7] == b"include" {
            let j = i + 7;
            // skip space
            let mut k = j;
            while k < bytes.len() && bytes[k] == b' ' { k += 1; }
            if k < bytes.len() && bytes[k] == b'"' {
                k += 1;
                let name_start = k;
                while k < bytes.len() && bytes[k] != b'"' { k += 1; }
                if k < bytes.len() {
                    let fname: &str = core::str::from_utf8(&bytes[name_start..k]).unwrap_or("");
                    k += 1; // closing "
                    // eat optional semicolon
                    while k < bytes.len() && bytes[k] == b';' { k += 1; }
                    // eat to end of line
                    while k < bytes.len() && bytes[k] != b'\n' { k += 1; }
                    if k < bytes.len() { k += 1; } // eat \n
                    // try to read the file
                    let mut fname_buf = [0u8; 128]; let mut flen = 0usize;
                    for &c in fname.as_bytes() { if flen < 127 { fname_buf[flen] = c; flen += 1; } }
                    fname_buf[flen] = 0;
                    let fsz = avfs_get_filesize(fname_buf.as_ptr());
                    if fsz > 0 {
                        let fp = malloc(fsz as u32 + 1);
                        if !fp.is_null() {
                            avfs_read_file(fname_buf.as_ptr(), fp, fsz as u32, 0);
                            *fp.add(fsz as usize) = 0;
                            let inc_src = core::str::from_utf8(
                                core::slice::from_raw_parts(fp, fsz as usize)
                            ).unwrap_or("");
                            out.push_str("// -- begin include: ");
                            out.push_str(fname);
                            out.push_str(" --\n");
                            let expanded = helios_preprocess(inc_src, depth + 1);
                            out.push_str(&expanded);
                            out.push_str("\n// -- end include: ");
                            out.push_str(fname);
                            out.push_str(" --\n");
                            free(fp);
                            i = k;
                            continue;
                        }
                    } else {
                        kprint_col(COL_YELLOW, b"[helios] include warning: file not found: ");
                        kprintln(fname.as_bytes());
                        i = k;
                        continue;
                    }
                }
            }
        }
        // not an include line — copy until \n
        i = start;
        while i < bytes.len() && bytes[i] != b'\n' { out.push(bytes[i] as char); i += 1; }
        if i < bytes.len() { out.push('\n'); i += 1; }
    }
    out
}

// ── v6: merge multiple source strings for multi-file compilation ───────────────
pub unsafe fn helios_merge_sources(sources: &[&str]) -> String {
    let mut merged = String::new();
    for (idx, &src) in sources.iter().enumerate() {
        merged.push_str("// ====== file ");
        let mut nb = [0u8; 16]; 
        let s = i32_to_str(idx as i32, &mut nb);
        for &c in s { merged.push(c as char); }
        merged.push_str(" ======\n");
        let expanded = helios_preprocess(src, 0);
        merged.push_str(&expanded);
        merged.push('\n');
    }
    merged
}

pub fn helios_compile(source: &str, verbosity: u8) -> Result<Vec<u8>, ()> {
    unsafe {
        if verbosity >= 2 { kprintln_col(COL_CYAN, b"[helios] lexing..."); }
    }
    let tokens = Lexer::new(source.as_bytes()).tokenise();
    unsafe {
        if verbosity >= 2 {
            kprint_col(COL_CYAN, b"[helios] parsing & linting (");
            kprint_i32(tokens.len() as i32);
            kprintln_col(COL_CYAN, b" tokens)...");
        }
    }
    let (structs, fns, consts_top, prp_imports, fetch_imports, diags) = Parser::new(tokens, source).parse_program();
    unsafe { diags.emit_all(); }
    if diags.has_errors() { return Err(()); }
    unsafe {
        if verbosity >= 2 {
            kprint_col(COL_CYAN, b"[helios] emitting bytecode (");
            kprint_i32(fns.len() as i32);
            kprintln_col(COL_CYAN, b" fns)...");
        }
    }
    let (code, consts, entry) = Emitter::new(structs, consts_top, prp_imports, fetch_imports).emit_program(&fns);
    unsafe {
        if verbosity >= 1 {
            terminal_setcolor(COL_GREEN);
            kprint(b"[helios] ok  code="); kprint_i32(code.len() as i32);
            kprint(b"B  consts="); kprint_i32(consts.len() as i32);
            kprint(b"  entry="); kprint_u32_hex(entry);
            kputc(b'\n');
            terminal_setcolor(COL_WHITE);
        }
    }
    Ok(pack_rxe(&code, &consts, entry))
}

pub unsafe fn helios_exec(rxe: &[u8], verbosity: u8) -> i32 {
    let (entry, _, _) = match rxe_validate(rxe) {
        Ok(v) => v,
        Err(e) => { kprintln_col(COL_RED, e.as_bytes()); return -1; }
    };
    let mut pos = 32usize;
    let (mut code, mut data): (&[u8], &[u8]) = (&[], &[]);
    while pos < rxe.len() {
        let sec = rxe[pos]; pos += 1;
        if sec == RXE_SECTION_END { break; }
        if pos + 4 > rxe.len() { break; }
        let len = u32::from_le_bytes([rxe[pos], rxe[pos+1], rxe[pos+2], rxe[pos+3]]) as usize;
        pos += 4;
        if pos + len > rxe.len() { break; }
        match sec {
            RXE_SECTION_CODE => code = &rxe[pos..pos+len],
            RXE_SECTION_DATA => data = &rxe[pos..pos+len],
            _ => {}
        }
        pos += len;
    }
    // FIX: VM stack now allocated on kernel heap to avoid Rust stack overflow on complex programs
    // v5: stack_kb=0 uses the default VM_STACK_SLOTS; overridable via `-stack <KB>` shell flag
    let mut vm = match Vm::new(code, parse_const_pool(data), verbosity, 0) {
        Some(v) => v,
        None => {
            kprintln_col(COL_RED, b"[helios] out of memory - cannot allocate VM stack (4 MB)");
            return -1;
        }
    };
    let result = vm.run(entry);
    vm.drop_stack();
    result
}

pub unsafe fn helios_exec_stack(rxe: &[u8], verbosity: u8, stack_kb: u32) -> i32 {
    let (entry, _, _) = match rxe_validate(rxe) {
        Ok(v) => v,
        Err(e) => { kprintln_col(COL_RED, e.as_bytes()); return -1; }
    };
    let mut pos = 32usize;
    let (mut code, mut data): (&[u8], &[u8]) = (&[], &[]);
    while pos < rxe.len() {
        let sec = rxe[pos]; pos += 1;
        if sec == RXE_SECTION_END { break; }
        if pos + 4 > rxe.len() { break; }
        let len = u32::from_le_bytes([rxe[pos], rxe[pos+1], rxe[pos+2], rxe[pos+3]]) as usize;
        pos += 4;
        if pos + len > rxe.len() { break; }
        match sec {
            RXE_SECTION_CODE => code = &rxe[pos..pos+len],
            RXE_SECTION_DATA => data = &rxe[pos..pos+len],
            _ => {}
        }
        pos += len;
    }
    let mut vm = match Vm::new(code, parse_const_pool(data), verbosity, stack_kb) {
        Some(v) => v,
        None => {
            kprintln_col(COL_RED, b"[helios] out of memory - cannot allocate VM stack");
            return -1;
        }
    };
    let result = vm.run(entry);
    vm.drop_stack();
    result
}

pub unsafe fn helios_run(source: &str, verbosity: u8) -> i32 {
    helios_run_stack(source, verbosity, 0)
}

pub unsafe fn helios_run_stack(source: &str, verbosity: u8, stack_kb: u32) -> i32 {
    let expanded = helios_preprocess(source, 0);
    match helios_compile(&expanded, verbosity) {
        Ok(rxe) => helios_exec_stack(&rxe, verbosity, stack_kb),
        Err(()) => -1,
    }
}

// ============================================================================
//  SECTION 11 - TEST SUITE  (`helios test error` and `helios test all`)
// ============================================================================

struct TestCase { label: &'static str, source: &'static str, expect_error: bool }

static DIAG_TESTS: &[TestCase] = &[
    TestCase { label: "E001 - top-level code",               expect_error: true,  source: "let x = 5;" },
    TestCase { label: "E002 - no main",                       expect_error: true,  source: "fn helper() -> void { }" },
    TestCase { label: "E003 - duplicate function",            expect_error: true,  source: "fn main() -> void { } fn main() -> void { }" },
    TestCase { label: "E004 - duplicate struct",              expect_error: true,  source: "struct Vec2 { int x, int y } struct Vec2 { int a } fn main() -> void { }" },
    TestCase { label: "E005 - unexpected token",              expect_error: true,  source: "fn main() -> void { if true { } }" },
    TestCase { label: "E006 - missing semicolon",             expect_error: true,  source: "fn main() -> void { let x = 1 }" },
    TestCase { label: "E007 - unexpected token in expr",      expect_error: true,  source: "fn main() -> void { let x = }; }" },
    TestCase { label: "E009 - undefined function",             expect_error: true,  source: "fn main() { missing_helper(); }" },
    TestCase { label: "E010 - missing variable name",         expect_error: true,  source: "fn main() -> void { let = 5; }" },
    TestCase { label: "E020 - missing loop variable",         expect_error: true,  source: "fn main() -> void { for in 0..5 { } }" },
    TestCase { label: "E021 - missing `in` keyword",          expect_error: true,  source: "fn main() -> void { for i 0..5 { } }" },
    TestCase { label: "E022 - missing range operator",        expect_error: true,  source: "fn main() -> void { for i in 0 5 { } }" },
    TestCase { label: "E050 - break outside loop",            expect_error: true,  source: "fn main() -> void { break; }" },
    TestCase { label: "E051 - continue outside loop",         expect_error: true,  source: "fn main() -> void { continue; }" },
    TestCase { label: "W101 - side-effect-free expression",   expect_error: false, source: "fn main() -> void { 42; }" },
    TestCase { label: "W201 - constant condition",            expect_error: false, source: "fn main() -> void { if (true) { } }" },
    TestCase { label: "W202 - loop without break",            expect_error: false, source: "fn main() -> void { loop { let x = 1; } }" },
    TestCase { label: "W301 - deep nesting",                  expect_error: false, source: "fn main() -> void { if (1==1) { if (1==1) { if (1==1) { if (1==1) { if (1==1) { let x = 0; } } } } } }" },
    TestCase { label: "W302 - unreachable code after return", expect_error: false, source: "fn main() -> void { return 0; let dead = 1; }" },
    TestCase { label: "W303 - empty block",                   expect_error: false, source: "fn main() -> void { if (1==1) { } }" },
    TestCase { label: "W401 - non-void fn without return",    expect_error: false, source: "fn add(int a, int b) -> int { let c = a; }  fn main() -> void { }" },
    TestCase { label: "W501 - unused function",               expect_error: false, source: "fn unused_helper() -> void { let x = 1; }  fn main() -> void { }" },
    TestCase { label: "W502 - unused variable",               expect_error: false, source: "fn main() -> void { let unused = 99; }" },
    TestCase { label: "W601 - large array literal",           expect_error: false, source: "fn main() -> void { let big = [1; 300]; }" },
    // v4 positive tests (should compile clean with no errors)
    TestCase { label: "OK  - let mut accepted",               expect_error: false, source: "fn main() -> void { let mut x = 5; x += 1; }" },
    TestCase { label: "OK  - char literal",                   expect_error: false, source: "fn main() -> void { let c = 'A'; print(c); }" },
    TestCase { label: "OK  - for step",                       expect_error: false, source: "fn main() -> void { for i in 0..10 step 2 { print(i); } }" },
    TestCase { label: "OK  - match str arm",                  expect_error: false, source: "fn main() -> void { let s = \"hello\"; match s { \"hello\" => { print(1); } _ => { print(0); } } }" },
    TestCase { label: "OK  - struct + field access",          expect_error: false, source: "struct Vec2 { int x, int y }  fn main() -> void { let v = new Vec2 { x: 3, y: 4 }; print(v.x); }" },
    TestCase { label: "OK  - omit -> void",                   expect_error: false, source: "fn greet() { print(42); } fn main() { greet(); }" },
    TestCase { label: "OK  - colon-typed parameters",          expect_error: false, source: "fn draw(x: int, y: int) { print(x); print(y); } fn main() { draw(1, 2); }" },
    TestCase { label: "OK  - memory intrinsic lowering",      expect_error: false, source: "fn main() { let value = peek32(0xB8000); poke8(0xB8000, value); let word = peek(0xB8000, 16); poke(0xB8000, word, 16); memcpy(0x2000, 0x3000, 4); memset(0x2000, 0, 4); print_hex(value); let letter = char_code(\"A\"); print(letter); }" },
    TestCase { label: "OK  - hardware and input intrinsics",   expect_error: false, source: "fn main() { let status = in8(0x64); out8(0x60, status); let ticks = get_ticks(); sleep_ms(1); pit_set_hz(100); let cpuid_leaf = cpuid(0); print_char(cpuid_leaf); print_str(input()); let key = poll_scancode(); }" },
    TestCase { label: "OK  - PRP individual import",            expect_error: false, source: "using::prp::keygen; fn main() { let result = keygen(\"/tmp/helios_key\"); print(result); }" },
    TestCase { label: "OK  - PRP wildcard import",              expect_error: false, source: "using::prp::*; fn main() { let result = selftest(); print(result); }" },
    TestCase { label: "OK  - fetch individual import",          expect_error: false, source: "using::fetch::get; fn main() { let status = get(\"https://example.com/\"); print(status); }" },
    TestCase { label: "OK  - fetch wildcard import",            expect_error: false, source: "using::fetch::*; fn main() { let busy = active(); print(busy); }" },
    TestCase { label: "OK  - VGA text renderer syntax",        expect_error: false, source: "const VGA_MEM: int = 0xB8000; const ROWS: int = 50; fn make_cell(ch: int, color: int) -> int { return ch | (color << 8); } fn write_vga(x: int, y: int, cell: int) { let addr = VGA_MEM + ((y * 80 + x) * 2); unsafe { poke16(addr, cell); } } fn clear_screen(bg: int) { let blank = make_cell(32, bg); for y in 0..ROWS { for x in 0..80 { write_vga(x, y, blank); } } } fn main() { clear_screen(7); }" },
    // v5 tests
    TestCase { label: "OK  - const definition",               expect_error: false, source: "const MAX: int = 100; fn main() -> void { print(MAX); }" },
    TestCase { label: "OK  - assert passes",                  expect_error: false, source: "fn main() -> void { assert!(1 == 1); }" },
    TestCase { label: "OK  - assert with msg",                expect_error: false, source: "fn main() -> void { assert!(1 > 0, \"should be positive\"); }" },
    TestCase { label: "E013 - duplicate const",               expect_error: true,  source: "const X: int = 1; const X: int = 2; fn main() -> void { }" },
    TestCase { label: "W603 - useless unsafe block",          expect_error: false, source: "fn main() -> void { unsafe { let x = 5; } }" },
    TestCase { label: "W604 - asm outside unsafe",            expect_error: false, source: "fn main() -> void { asm!(\"nop\"); }" },
];

fn bytecode_contains_opcode(code: &[u8], expected: &Opcode) -> bool {
    let mut pc = 0usize;
    while pc < code.len() {
        if let Some(opcode) = Opcode::from_u8(code[pc]) {
            if &opcode == expected { return true; }
            pc += 1 + opcode.imm_bytes() as usize;
        } else {
            pc += 1;
        }
    }
    false
}

unsafe fn run_test_suite(suite_name: &str) {
    terminal_setcolor(COL_MAGENTA);
    kprintln(b"+-----------------------------------------+");
    kprint(b"| HELIOS v6.0  *  DIAGNOSTIC TEST SUITE");
    if suite_name == "all" { kprintln(b"  ALL |"); } else { kprintln(b" ERR |"); }
    kprintln(b"+-----------------------------------------+");
    terminal_setcolor(COL_WHITE);
    kputc(b'\n');

    let mut pass = 0usize;
    let mut fail = 0usize;

    for (i, tc) in DIAG_TESTS.iter().enumerate() {
        // Filter by suite
        let is_ok_test  = tc.label.starts_with("OK");
        if suite_name == "error" && is_ok_test { continue; }

        terminal_setcolor(COL_CYAN);
        kprint(b"Test "); kprint_i32((i + 1) as i32); kprint(b" - ");
        terminal_setcolor(COL_WHITE);
        kprintln(tc.label.as_bytes());

        terminal_setcolor(COL_GREY);
        kprint(b"  src: "); kprintln(tc.source.as_bytes());
        terminal_setcolor(COL_WHITE);
        kprintln(b"  ---");

        let tokens = Lexer::new(tc.source.as_bytes()).tokenise();
        let (structs, fns, consts, prp_imports, fetch_imports, diags) = Parser::new(tokens, tc.source).parse_program();
        let had_diag = !diags.diags.is_empty();
        let had_error = diags.has_errors();
        diags.emit_all();

        kprintln(b"  ---");
        let mut passed = if tc.expect_error { had_error } else if is_ok_test { !had_error } else { had_diag };
        if tc.label == "OK  - memory intrinsic lowering" && !had_error {
            let (code, _, _) = Emitter::new(structs, consts, prp_imports.clone(), fetch_imports.clone()).emit_program(&fns);
            let expected = [
                Opcode::Peek32, Opcode::Poke8, Opcode::Peek16, Opcode::Poke16,
                Opcode::MemCpy, Opcode::MemSet, Opcode::PrintHex, Opcode::StrByte,
            ];
            passed &= expected.iter().all(|opcode| bytecode_contains_opcode(&code, opcode));
        } else if tc.label == "OK  - hardware and input intrinsics" && !had_error {
            let (code, _, _) = Emitter::new(structs, consts, prp_imports.clone(), fetch_imports.clone()).emit_program(&fns);
            let expected = [
                Opcode::In8, Opcode::Out8, Opcode::GetTicks, Opcode::SleepMs,
                Opcode::PitSetHz, Opcode::Cpuid, Opcode::PrintChar,
                Opcode::PrintStr, Opcode::Input, Opcode::PollScancode,
            ];
            passed &= expected.iter().all(|opcode| bytecode_contains_opcode(&code, opcode));
        } else if tc.label == "OK  - PRP individual import" && !had_error {
            let (code, _, _) = Emitter::new(structs, consts, prp_imports.clone(), fetch_imports.clone()).emit_program(&fns);
            passed &= bytecode_contains_opcode(&code, &Opcode::PrpKeygen);
        } else if tc.label == "OK  - PRP wildcard import" && !had_error {
            let (code, _, _) = Emitter::new(structs, consts, prp_imports.clone(), fetch_imports.clone()).emit_program(&fns);
            passed &= bytecode_contains_opcode(&code, &Opcode::PrpSelfTest);
        } else if tc.label == "OK  - fetch individual import" && !had_error {
            let (code, _, _) = Emitter::new(structs, consts, prp_imports.clone(), fetch_imports.clone()).emit_program(&fns);
            passed &= bytecode_contains_opcode(&code, &Opcode::FetchGet);
        } else if tc.label == "OK  - fetch wildcard import" && !had_error {
            let (code, _, _) = Emitter::new(structs, consts, prp_imports.clone(), fetch_imports.clone()).emit_program(&fns);
            passed &= bytecode_contains_opcode(&code, &Opcode::FetchActive);
        }
        if passed {
            terminal_setcolor(COL_GREEN); kprintln(b"  PASS"); pass += 1;
        } else {
            terminal_setcolor(COL_RED); kprintln(b"  FAIL"); fail += 1;
        }
        terminal_setcolor(COL_WHITE); kprintln(b"");
    }

    terminal_setcolor(COL_MAGENTA); kprintln(b"+-----------------------------------------+");
    kprint(b"  Results: "); terminal_setcolor(COL_GREEN); kprint_i32(pass as i32); kprint(b" pass");
    if fail > 0 {
        terminal_setcolor(COL_WHITE); kprint(b"  ");
        terminal_setcolor(COL_RED); kprint_i32(fail as i32); kprint(b" fail");
    }
    terminal_setcolor(COL_WHITE); kputc(b'\n');
}

// ============================================================================
//  SECTION 12 - LONG-FORM ERROR EXPLAIN
// ============================================================================

unsafe fn explain_code(code: &str) {
    terminal_setcolor(COL_CYAN);
    kprint(b"Helios diagnostic help: "); kprintln(code.as_bytes());
    terminal_setcolor(COL_WHITE); kputc(b'\n');

    let body: &[u8] = match code {
        "E001" => b"Top-level code.\n  Only `fn` and `struct` definitions are allowed outside functions.\n  Wrap loose statements in `fn main() -> void { ... }`.",
        "E002" => b"No entry point.\n  Every Helios program must define `fn main() -> void { ... }`.\n  The VM starts execution at `main`.",
        "E003" => b"Duplicate function name.\n  Two functions share the same name. Rename one.",
        "E004" => b"Duplicate struct name.\n  Two structs share the same name. Rename one.",
        "E005" => b"Unexpected token.\n  Common causes: missing `)`, `]`, `}` or a typo in a keyword.",
        "E006" => b"Missing semicolon.\n  Every Helios statement must end with `;`.",
        "E007" => b"Unexpected token in expression.\n  Valid starts: integer, string, bool, variable name, `(`, `[`, `*`, `&`, `!`, `~`.",
        "E009" => b"Undefined function call.\n  Define the function, use an available Helios intrinsic, or import it with `using::prp::name;`.",
        "E010" => b"Missing variable name after `let`.\n  Syntax: `let name = value;`  or  `let int name = value;`",
        "E020" => b"Missing loop variable after `for`.\n  Syntax: `for i in 0..10 { ... }`",
        "E021" => b"Missing `in` keyword in for loop.\n  Syntax: `for i in start..end { ... }`",
        "E022" => b"Missing range operator.\n  Use `..` for exclusive or `..=` for inclusive.",
        "E050" => b"`break` outside of loop.\n  `break` exits the nearest `for`, `while`, or `loop` block.",
        "E051" => b"`continue` outside of loop.\n  `continue` skips to the next iteration.",
        "W101" => b"Side-effect-free expression.\n  A bare value as a statement has no effect.\n  Use `print(expr);` or remove it.",
        "W201" => b"Constant condition.\n  The branch is always taken or always skipped.",
        "W202" => b"Infinite `loop` without `break`.\n  Add `break;` somewhere, or convert to `while (cond) { }`.",
        "W301" => b"Deeply nested block (depth >= 5).\n  Extract inner logic into helper functions.",
        "W302" => b"Unreachable code after `return`.\n  Remove the dead code or move it before the `return`.",
        "W303" => b"Empty block `{}`.\n  Add code or `// intentionally empty`.",
        "W401" => b"Non-void function with no `return`.\n  Add `return <value>;` before the closing brace.",
        "W501" => b"Unused function.\n  Call it from another function or remove it.",
        "W502" => b"Unused variable.\n  Use it, remove it, or prefix the name with `_`.",
        "W601" => b"Large array literal (>256 elements).\n  Allocate in chunks or use `[val; N]` fill syntax.",
        "W602" => b"Function with >16 parameters.\n  Group parameters into a struct.",
        "E012" => b"Assertion failure at runtime.\n  assert!(cond) or assert!(cond, \"msg\") failed.\n  Check the condition or the logic that feeds into it.",
        "E013" => b"Duplicate const definition.\n  Two `const` declarations share the same name.\n  Rename one or remove the duplicate.",
        "E014" => b"Invalid or missing PRP import.\n  Use `using::prp::*;` or import one function with `using::prp::keygen;`.\n  Available functions: random_bytes, sha256_file, seal_file, open_file, seal_text, fingerprint, keygen, sign, verify, seal_file_pub, open_file_prv, seal_text_pub, selftest.",
        "E015" => b"Fetch function is not imported.\n  Add `using::fetch::*;` or import one wrapper such as `using::fetch::get;`.\n  Available wrappers: get, https_get, active, test_https.",
        "W603" => b"Useless unsafe block.\n  The `unsafe { }` block contains no unsafe operations.\n  Either add `asm!()` / raw pointer ops or remove the `unsafe` wrapper.",
        "W604" => b"asm!() used outside of `unsafe` block.\n  Wrap in `unsafe { asm!(...); }` to acknowledge the risk.",
        _ => b"Unknown diagnostic code. Run `helios test error` to see all codes.",
    };
    kprintln(body);
    kputc(b'\n');
}

// ============================================================================
//  SECTION 13 - AST PRETTY-PRINTER  (`helios fmt`)
// ============================================================================

unsafe fn fmt_indent(depth: usize) {
    for _ in 0..depth { kprint(b"  "); }
}

unsafe fn fmt_expr(e: &Expr, depth: usize) {
    match e {
        Expr::IntLit(n)   => kprint_i32(*n),
        Expr::BoolLit(b)  => kprint(if *b { b"true" } else { b"false" }),
        Expr::StrLit(s)   => { kputc(b'"'); kprint(s.as_bytes()); kputc(b'"'); }
        Expr::Var(nm)     => kprint(nm.as_bytes()),
        Expr::AddrOf(nm)  => { kputc(b'&'); kprint(nm.as_bytes()); }
        Expr::Deref(e)    => { kputc(b'*'); fmt_expr(e, depth); }
        Expr::Input       => kprint(b"input()"),
        Expr::InlineAsm(s)=> { kprint(b"asm!(\""); kprint(s.as_bytes()); kprint(b"\")"); }
        Expr::Cast(e, t)  => {
            fmt_expr(e, depth); kprint(b" as ");
            kprint(match t { CastTarget::Int => b"int", CastTarget::Bool => b"bool" });
        }
        Expr::Call(nm, args) => {
            kprint(nm.as_bytes()); kputc(b'(');
            for (i, a) in args.iter().enumerate() {
                if i > 0 { kprint(b", "); }
                fmt_expr(a, depth);
            }
            kputc(b')');
        }
        Expr::BinOp(l, op, r) => {
            kputc(b'('); fmt_expr(l, depth);
            kprint(match op {
                BinOpKind::Add  => b" + ",  BinOpKind::Sub  => b" - ",
                BinOpKind::Mul  => b" * ",  BinOpKind::Div  => b" / ",
                BinOpKind::Mod  => b" % ",
                BinOpKind::Eq   => b" == ", BinOpKind::Ne   => b" != ",
                BinOpKind::Lt   => b" < ",  BinOpKind::Le   => b" <= ",
                BinOpKind::Gt   => b" > ",  BinOpKind::Ge   => b" >= ",
                BinOpKind::And  => b" && ", BinOpKind::Or   => b" || ",
                BinOpKind::BAnd => b" & ",  BinOpKind::BOr  => b" | ",
                BinOpKind::BXor => b" ^ ",  BinOpKind::Shl  => b" << ",
                BinOpKind::Shr  => b" >> ",
            });
            fmt_expr(r, depth); kputc(b')');
        }
        Expr::UnOp(op, e) => {
            kprint(match op { UnOpKind::Neg => b"-", UnOpKind::Not => b"!", UnOpKind::BNot => b"~" });
            fmt_expr(e, depth);
        }
        Expr::Index(b, i) => { fmt_expr(b, depth); kputc(b'['); fmt_expr(i, depth); kputc(b']'); }
        Expr::Field(b, f) => { fmt_expr(b, depth); kputc(b'.'); kprint(f.as_bytes()); }
        Expr::FieldChain(b, fs) => {
            fmt_expr(b, depth);
            for f in fs { kputc(b'.'); kprint(f.as_bytes()); }
        }
        Expr::ArrayLit(items) => {
            kputc(b'[');
            for (i, it) in items.iter().enumerate() {
                if i > 0 { kprint(b", "); } fmt_expr(it, depth);
            }
            kputc(b']');
        }
        Expr::ArrayFill(v, n) => { kputc(b'['); fmt_expr(v, depth); kprint(b"; "); kprint_i32(*n as i32); kputc(b']'); }
    }
}

unsafe fn fmt_stmts(stmts: &[Stmt], depth: usize) {
    for s in stmts {
        fmt_indent(depth);
        match s {
            Stmt::Let(nm, e, _, _) => { kprint(b"let "); kprint(nm.as_bytes()); kprint(b" = "); fmt_expr(e, depth); kprintln(b";"); }
            Stmt::LetArr(nm, n, e, _, _) => { kprint(b"let "); kprint(nm.as_bytes()); kputc(b'['); kprint_i32(*n as i32); kprint(b"] = "); fmt_expr(e, depth); kprintln(b";"); }
            Stmt::Assign(nm, e) => { kprint(nm.as_bytes()); kprint(b" = "); fmt_expr(e, depth); kprintln(b";"); }
            Stmt::AssignIdx(nm, i, v) => { kprint(nm.as_bytes()); kputc(b'['); fmt_expr(i, depth); kprint(b"] = "); fmt_expr(v, depth); kprintln(b";"); }
            Stmt::CompoundAssign(nm, op, e) => {
                kprint(nm.as_bytes());
                kprint(match op {
                    BinOpKind::Add  => b" += ", BinOpKind::Sub => b" -= ",
                    BinOpKind::Mul  => b" *= ", BinOpKind::Div => b" /= ",
                    BinOpKind::Mod  => b" %= ", BinOpKind::BAnd=> b" &= ",
                    BinOpKind::BOr  => b" |= ", BinOpKind::BXor=> b" ^= ",
                    _ => b" op= ",
                });
                fmt_expr(e, depth); kprintln(b";");
            }
            Stmt::Return(e, _)  => { kprint(b"return "); fmt_expr(e, depth); kprintln(b";"); }
            Stmt::Print(e)      => { kprint(b"print("); fmt_expr(e, depth); kprintln(b");"); }
            Stmt::PrintHex(e)   => { kprint(b"print_hex("); fmt_expr(e, depth); kprintln(b");"); }
            Stmt::Break         => kprintln(b"break;"),
            Stmt::BreakVal(e)   => { kprint(b"break "); fmt_expr(e, depth); kprintln(b";"); }
            Stmt::Continue      => kprintln(b"continue;"),
            Stmt::AsmStmt(s)    => { kprint(b"asm!(\""); kprint(s.as_bytes()); kprintln(b"\");"); }
            Stmt::Expr(e, _, _) => { fmt_expr(e, depth); kprintln(b";"); }
            Stmt::UnsafeBlock(body) => {
                kprintln(b"unsafe {");
                fmt_stmts(body, depth + 1);
                fmt_indent(depth); kprintln(b"}");
            }
            Stmt::If(cond, then_b, else_b) => {
                kprint(b"if ("); fmt_expr(cond, depth); kprintln(b") {");
                fmt_stmts(then_b, depth + 1);
                if else_b.is_empty() {
                    fmt_indent(depth); kprintln(b"}");
                } else {
                    fmt_indent(depth); kprintln(b"} else {");
                    fmt_stmts(else_b, depth + 1);
                    fmt_indent(depth); kprintln(b"}");
                }
            }
            Stmt::While(cond, body) => {
                kprint(b"while ("); fmt_expr(cond, depth); kprintln(b") {");
                fmt_stmts(body, depth + 1);
                fmt_indent(depth); kprintln(b"}");
            }
            Stmt::Loop(body) => {
                kprintln(b"loop {");
                fmt_stmts(body, depth + 1);
                fmt_indent(depth); kprintln(b"}");
            }
            Stmt::For { var, start, end, step, kind, body } => {
                kprint(b"for "); kprint(var.as_bytes()); kprint(b" in ");
                fmt_expr(start, depth);
                kprint(match kind { RangeKind::Exclusive => b"..", RangeKind::Inclusive => b"..=" });
                fmt_expr(end, depth);
                if let Some(s) = step { kprint(b" step "); fmt_expr(s, depth); }
                kprintln(b" {");
                fmt_stmts(body, depth + 1);
                fmt_indent(depth); kprintln(b"}");
            }
            Stmt::Match(e, arms) => {
                kprint(b"match "); fmt_expr(e, depth); kprintln(b" {");
                for arm in arms {
                    fmt_indent(depth + 1);
                    match &arm.pattern {
                        MatchPat::Lit(n)    => kprint_i32(*n),
                        MatchPat::StrLit(s) => { kputc(b'"'); kprint(s.as_bytes()); kputc(b'"'); }
                        MatchPat::Wildcard  => kputc(b'_'),
                    }
                    kprintln(b" => {");
                    fmt_stmts(&arm.body, depth + 2);
                    fmt_indent(depth + 1); kprintln(b"},");
                }
                fmt_indent(depth); kprintln(b"}");
            }
            Stmt::AssertStmt(cond, msg, _) => {
                kprint(b"assert!(");
                fmt_expr(cond, depth);
                if let Some(m) = msg {
                    kprint(b", \"");
                    kprint(m.as_bytes());
                    kprint(b"\"");
                }
                kprintln(b");");
            }
            Stmt::ConstDef(nm, val) => {
                kprint(b"const ");
                kprint(nm.as_bytes());
                kprint(b" = ");
                kprint_i32(*val);
                kprintln(b";");
            }
        }
    }
}

unsafe fn fmt_program(structs: &[StructDef], fns: &[FnDef]) {
    for s in structs {
        kprint(b"struct "); kprint(s.name.as_bytes()); kprintln(b" {");
        for f in &s.fields {
            kprint(b"  int "); kprint(f.as_bytes()); kprintln(b",");
        }
        kprintln(b"}"); kputc(b'\n');
    }
    for f in fns {
        kprint(b"fn "); kprint(f.name.as_bytes()); kputc(b'(');
        for (i, p) in f.params.iter().enumerate() {
            if i > 0 { kprint(b", "); }
            kprint(b"int "); kprint(p.as_bytes());
        }
        kprint(b") -> "); kprintln(if f.ret_void { b"void {" } else { b"int {" });
        fmt_stmts(&f.body, 1);
        kprintln(b"}"); kputc(b'\n');
    }
}

// ============================================================================
//  SECTION 14 - FILE EXECUTION
// ============================================================================

#[no_mangle]
pub unsafe extern "C" fn helios_exec_file(path: *const u8, verbosity: u8) -> i32 {
    helios_exec_file_stack(path, verbosity, 0)
}

unsafe fn helios_exec_file_stack(path: *const u8, verbosity: u8, stack_kb: u32) -> i32 {
    if path.is_null() { kprintln_col(COL_RED, b"helios: null path"); return -1; }
    let mut buf = [0u8; 128];
    let mut n = 0usize;
    while *path.add(n) != 0 && n < 120 { buf[n] = *path.add(n); n += 1; }
    buf[n] = 0;
    let has_ext = |ext: &[u8], b: &[u8], len: usize| len >= ext.len() && &b[len-ext.len()..len] == ext;
    let is_hls = has_ext(b".hls", &buf, n);
    let is_rxe = has_ext(b".rxe", &buf, n);
    if !is_hls && !is_rxe {
        for &c in b".rxe" { if n < 127 { buf[n] = c; n += 1; } }
        buf[n] = 0;
    }
    if avfs_is_directory(buf.as_ptr()) { kprintln_col(COL_RED, b"helios: is a directory"); return -1; }
    let fsize = avfs_get_filesize(buf.as_ptr());
    if fsize < 0 { kprintln_col(COL_RED, b"helios: file not found"); return -1; }
    if fsize == 0 && !is_hls {
        kprintln_col(COL_RED, b"helios: file is empty");
        return -1;
    }
    let ptr = malloc(fsize as u32 + 1);
    if ptr.is_null() { kprintln_col(COL_RED, b"helios: out of memory reading file"); return -1; }
    if fsize > 0 && avfs_read_file(buf.as_ptr(), ptr, fsize as u32, 0) < 0 {
        kprintln_col(COL_RED, b"helios: read error"); free(ptr); return -1;
    }
    *ptr.add(fsize as usize) = 0;
    let data = core::slice::from_raw_parts(ptr, fsize as usize);
    let result = if is_hls {
        // v6: preprocess includes before running
        let expanded = helios_preprocess(core::str::from_utf8(data).unwrap_or(""), 0);
        match helios_compile(&expanded, verbosity) {
            Ok(rxe) => helios_exec_stack(&rxe, verbosity, stack_kb),
            Err(()) => -1,
        }
    } else {
        helios_exec_stack(data, verbosity, stack_kb)
    };
    free(ptr);
    result
}

// ============================================================================
//  SECTION 15 - BENCH COMMAND  (`helios bench <src.hls>`)
// ============================================================================

unsafe fn bench_source(src: &str, verbosity: u8, stack_kb: u32) {
    const RUNS: usize = 5;
    terminal_setcolor(COL_CYAN);
    kprintln(b"[helios bench] compiling...");
    terminal_setcolor(COL_WHITE);
    let rxe = match helios_compile(src, 0) {
        Ok(r) => r, Err(()) => { kprintln_col(COL_RED, b"bench: compile failed"); return; }
    };
    kprint(b"[helios bench] rxe size: "); kprint_i32(rxe.len() as i32); kprintln(b" bytes");
    kprint(b"[helios bench] running "); kprint_i32(RUNS as i32); kprintln(b" times...");
    let mut total_ticks: u32 = 0;
    for i in 0..RUNS {
        let t0 = get_ticks();
        helios_exec_stack(&rxe, 0, stack_kb);
        let dt = get_ticks().wrapping_sub(t0);
        total_ticks = total_ticks.wrapping_add(dt);
        terminal_setcolor(COL_GREY);
        kprint(b"  run "); kprint_i32(i as i32 + 1); kprint(b": "); kprint_i32(dt as i32); kprintln(b" ticks");
        terminal_setcolor(COL_WHITE);
    }
    let mean = total_ticks / RUNS as u32;
    terminal_setcolor(COL_GREEN);
    kprint(b"[helios bench] mean: "); kprint_i32(mean as i32); kprintln(b" ticks/run");
    terminal_setcolor(COL_WHITE);
}

// ============================================================================
//  SECTION 15b - INLINE SOURCE HELPER
// ============================================================================

// Joins argv[start..argc] with spaces into a single String.
// Used by `helios eval` and the inline-snippet path of `helios run`.
unsafe fn helios_collect_inline_src(argc: i32, argv: *mut *mut u8, start: i32) -> String {
    let mut src = String::new();
    let mut i = start;
    while i < argc {
        let a = *argv.add(i as usize) as *const u8;
        let mut l = 0usize;
        while *a.add(l) != 0 { l += 1; }
        if !src.is_empty() { src.push(' '); }
        let slice = core::str::from_utf8(core::slice::from_raw_parts(a, l)).unwrap_or("");
        src.push_str(slice);
        i += 1;
    }
    src
}

// Wraps bare expressions / statements in a minimal `fn main()` so users can
// type one-liners without writing the boilerplate themselves.
//
// Heuristic: if the trimmed source does NOT start with "fn " or "struct "
// and does NOT already contain "fn main", treat it as a snippet and wrap it.
//
// Examples that get wrapped:
//   print("hello");
//   let x = cpuid(0); print_hex(x);
//   unsafe { let cr2 = get_cr2(); print_hex(cr2); }
//
// Examples that are left alone:
//   fn main() { print("hi"); }
//   fn helper() { } fn main() { helper(); }
fn helios_maybe_wrap(src: &str) -> String {
    let trimmed = src.trim();
    let needs_wrap = !trimmed.starts_with("fn ")
        && !trimmed.starts_with("struct ")
        && !trimmed.contains("fn main");
    if needs_wrap {
        let mut wrapped = String::with_capacity(trimmed.len() + 20);
        wrapped.push_str("fn main() {\n");
        wrapped.push_str(trimmed);
        // ensure there is a newline before the closing brace
        if !trimmed.ends_with('\n') { wrapped.push('\n'); }
        wrapped.push('}');
        wrapped
    } else {
        src.into()
    }
}

// ============================================================================
//  SECTION 16 - SHELL COMMAND  `helios`
// ============================================================================

struct SmallStr { buf: [u8; 64], len: usize }
impl SmallStr {
    fn as_str(&self) -> &str { core::str::from_utf8(&self.buf[..self.len]).unwrap_or("") }
    fn as_bytes(&self) -> &[u8] { &self.buf[..self.len] }
}
unsafe fn argv_str(ptr: *const u8) -> SmallStr {
    let mut s = SmallStr { buf: [0u8; 64], len: 0 };
    while *ptr.add(s.len) != 0 && s.len < 63 { s.buf[s.len] = *ptr.add(s.len); s.len += 1; }
    s
}

#[no_mangle]
extern "C" fn helios_command(argc: i32, argv: *mut *mut u8) {
    unsafe {
        if argc < 2 {
            terminal_setcolor(COL_CYAN);
            kprintln(b"Helios Compiler v7.0 | scp_2801");
            terminal_setcolor(COL_WHITE); kputc(b'\n');
            kprintln(b"USAGE");
            kprintln(b"  helios [-v|-vv] [-stack KB] compile <src.hls> [out.rxe]");
            kprintln(b"  helios [-v|-vv] [-stack KB] run     <file.rxe|file.hls>");
            kprintln(b"  helios [-v|-vv] [-stack KB] run     <helios source code...>");
            kprintln(b"  helios [-v|-vv] [-stack KB] eval    <helios source code...>");
            kprintln(b"  helios [-v|-vv] [-stack KB] src     <src.hls>");
            kprintln(b"  helios [-v|-vv] [-stack KB] demo    <name>");
            kprintln(b"  helios           check   <src.hls>");
            kprintln(b"  helios           tokens  <src.hls>          <- list lexer tokens");
            kprintln(b"  helios           fmt     <src.hls>          <- pretty-print AST");
            kprintln(b"  helios           disasm  <file.rxe>         <- disassemble bytecode");
            kprintln(b"  helios           info    <file.rxe>         <- print RXE header");
            kprintln(b"  helios           bench   <src.hls>          <- compile + run 5x");
            kprintln(b"  helios           test error                 <- run diagnostic tests");
            kprintln(b"  helios           test all                   <- all tests incl. OK cases");
            kprintln(b"  helios           explain <E|W><code>        <- long help for a code");
            kputc(b'\n');
            kprintln(b"INLINE EVAL");
            kprintln(b"  `run` and `eval` accept Helios source directly on the command line.");
            kprintln(b"  Bare snippets (no `fn main`) are auto-wrapped in fn main() { ... }.");
            kprintln(b"  Examples:");
            kprintln(b"    helios eval print(\"hello\");");
            kprintln(b"    helios eval let x = cpuid(0); print_hex(x);");
            kprintln(b"    helios run  fn main() { unsafe { asm!(\"cli\"); } }");
            kputc(b'\n');
            kprintln(b"FLAGS");
            kprintln(b"  -v    verbose  (stage output + byte counts)");
            kprintln(b"  -vv   debug    (trace every VM instruction)");
            kprintln(b"  -vvv  ultra    (-vv + unsafe op dump at exit)");
            kprintln(b"  -q    quiet    (suppress all verbose output)");
            kprintln(b"  -stack KB  VM value stack size (4..65536 KB)");
            kputc(b'\n');
            kprintln(b"LANGUAGE (v4.0)");
            kprintln(b"  Types:     int  str  bool  void  ptr");
            kprintln(b"  Literals:  42  0xFF  0b1010  true  false  \"text\"  'A'");
            kprintln(b"  Arith:     + - * / %    bitwise: & | ^ ~ << >>");
            kprintln(b"  Compare:   == != < <= > >=    logic: && ||");
            kprintln(b"  Assign:    = += -= *= /= %= &= |= ^=");
            kprintln(b"  Cast:      expr as int  /  expr as bool");
            kprintln(b"  Loops:     for i in 0..N { }    0..=N (inclusive)    step K");
            kprintln(b"             while (cond) { }    loop { break; }    break expr;");
            kprintln(b"  Match:     match x { 0 => { } \"s\" => { } _ => { } }");
            kprintln(b"  Arrays:    let a[8] = [0; 8];   a[i] = v;   a[i]");
            kprintln(b"  Structs:   struct Foo { int x, int y }   new Foo { x:1, y:2 }");
            kprintln(b"             foo.x  foo.a.b.c  (nested chains)");
            kprintln(b"  Misc:      let mut x = 0;   fn f() { }  (void inferred)");
            kprintln(b"  Unsafe:    unsafe { *ptr  &var  asm!(\"cli\") }");
            kprintln(b"  Hardware:  in8/out8/../in32/out32");
            kprintln(b"             peek8/poke8/../peek32/poke32");
            kprintln(b"             get_cr0/set_cr0  get_cr3/set_cr3");
            kprintln(b"             get_cs/ds/es/fs/gs/ss  set_fs  set_gs");
            kprintln(b"             rdmsr/wrmsr  rdmsr64/wrmsr64");
            kprintln(b"             sgdt/sidt  rdtscp  iopl3");
            kprintln(b"             pic_eoi  pic_mask  pic_unmask");
            kprintln(b"             pit_set_hz  get_ticks  sleep_ms");
            kprintln(b"             cpuid  fxsave/fxrstor  mem_query");
            return;
        }

        let mut idx = 1i32;
        let mut verbosity = 0u8;
        let mut stack_kb = 0u32;
        while idx < argc {
            let a = *argv.add(idx as usize) as *const u8;
            let l = { let mut l = 0; while *a.add(l) != 0 { l += 1; } l };
            let s = core::str::from_utf8(core::slice::from_raw_parts(a, l)).unwrap_or("");
            if s == "-v"  { verbosity = 1; idx += 1; }
            else if s == "-vvv" { verbosity = 4; idx += 1; }
            else if s == "-vv"  { verbosity = 3; idx += 1; }
            else if s == "-q"   { verbosity = 0; idx += 1; }
            else if s == "-stack" || s == "--stack" {
                idx += 1;
                if idx >= argc { kprintln_col(COL_RED, b"helios: -stack requires a KB value"); return; }
                let value = argv_str(*argv.add(idx as usize) as *const u8);
                match value.as_str().parse::<u32>() {
                    Ok(kb) if (4..=65536).contains(&kb) => stack_kb = kb,
                    _ => { kprintln_col(COL_RED, b"helios: stack size must be 4..65536 KB"); return; }
                }
                idx += 1;
            }
            else { break; }
        }
        if idx >= argc { kprintln_col(COL_RED, b"helios: missing subcommand"); return; }

        let sub = argv_str(*argv.add(idx as usize) as *const u8); idx += 1;

        // Helper: read source file from AVFS into a malloc'd buffer
        let read_src = |name: *const u8| -> Option<(*mut u8, usize)> {
            if avfs_is_directory(name) {
                kprintln_col(COL_RED, b"helios: source is a directory"); return None;
            }
            let sz = avfs_get_filesize(name) as isize;
            if sz < 0 {
                kprintln_col(COL_RED, b"helios: source not found"); return None;
            }
            // FIX: sz == 0 is valid (empty source file)
            let alloc_sz = (sz as u32).max(1);
            let p = malloc(alloc_sz + 1);
            if p.is_null() { kprintln_col(COL_RED, b"helios: out of memory"); return None; }
            if sz > 0 && avfs_read_file(name, p, sz as u32, 0) < 0 {
                kprintln_col(COL_RED, b"helios: read error"); free(p); return None;
            }
            *p.add(sz as usize) = 0;
            Some((p, sz as usize))
        };

        match sub.as_str() {

            "test" => {
                let suite = if idx < argc {
                    let w = argv_str(*argv.add(idx as usize) as *const u8);
                    w.as_str().into()
                } else { String::from("error") };
                if suite == "error" || suite == "all" {
                    run_test_suite(&suite);
                } else {
                    kprintln_col(COL_RED, b"helios test: unknown suite. try: error | all");
                }
            }

            "explain" => {
                if idx >= argc { kprintln_col(COL_RED, b"helios explain: usage: explain <Error Code>  e.g. E002 or W501"); return; }
                let code_str = argv_str(*argv.add(idx as usize) as *const u8);
                explain_code(code_str.as_str());
            }

            "check" => {
                if idx >= argc { kprintln_col(COL_RED, b"helios check: missing file"); return; }
                let name = *argv.add(idx as usize) as *const u8;
                if let Some((p, sz)) = read_src(name) {
                    let src = core::str::from_utf8(core::slice::from_raw_parts(p, sz)).unwrap_or("");
                    let tokens = Lexer::new(src.as_bytes()).tokenise();
                    let (_, _, _, _, _, diags) = Parser::new(tokens, src).parse_program();
                    diags.emit_all();
                    if diags.has_errors() { kprintln_col(COL_RED, b"check: FAILED"); }
                    else { kprintln_col(COL_GREEN, b"check: OK"); }
                    free(p);
                }
            }

            "tokens" => {
                if idx >= argc { kprintln_col(COL_RED, b"helios tokens: missing source file"); return; }
                let name = *argv.add(idx as usize) as *const u8;
                if let Some((p, sz)) = read_src(name) {
                    let src = core::str::from_utf8(core::slice::from_raw_parts(p, sz)).unwrap_or("");
                    for (index, token) in Lexer::new(src.as_bytes()).tokenise().iter().enumerate() {
                        kprint_i32(index as i32); kprint(b"  ");
                        kprint_i32(token.line as i32); kputc(b':'); kprint_i32(token.col as i32);
                        kprint(b"  "); kprint_str(&format!("{:?}", token.kind)); kputc(b'\n');
                    }
                    free(p);
                }
            }

            "compile" => {
                if idx >= argc { kprintln_col(COL_RED, b"helios compile: missing source"); return; }
                let src_name = *argv.add(idx as usize) as *const u8;
                let mut out = [0u8; 128]; let mut olen = 0usize;
                if idx + 1 < argc {
                    let op = *argv.add((idx + 1) as usize) as *const u8;
                    while *op.add(olen) != 0 && olen < 120 { out[olen] = *op.add(olen); olen += 1; }
                } else {
                    let sn = src_name;
                    while *sn.add(olen) != 0 && olen < 120 { out[olen] = *sn.add(olen); olen += 1; }
                    if olen >= 4 && &out[olen-4..olen] == b".hls" {
                        out[olen-3] = b'r'; out[olen-2] = b'x'; out[olen-1] = b'e';
                    }
                }
                if olen < 4 || &out[olen-4..olen] != b".rxe" {
                    for &c in b".rxe" { if olen < 127 { out[olen] = c; olen += 1; } }
                }
                out[olen] = 0;
                if let Some((p, sz)) = read_src(src_name) {
                    let raw_src = core::str::from_utf8(core::slice::from_raw_parts(p, sz)).unwrap_or("");
                    let src_expanded = helios_preprocess(raw_src, 0);
                    let src = src_expanded.as_str();
                    match helios_compile(src, verbosity) {
                        Ok(rxe) => {
                            if !avfs_file_exists(out.as_ptr()) { avfs_create_file(out.as_ptr(), rxe.len() as u32); }
                            avfs_write_file(out.as_ptr(), rxe.as_ptr(), rxe.len() as u32, 0);
                            terminal_setcolor(COL_GREEN);
                            kprint(b"[helios] wrote "); kprint_i32(rxe.len() as i32);
                            kprint(b" bytes -> "); kprint(&out[..olen]); kputc(b'\n');
                            terminal_setcolor(COL_WHITE);
                        }
                        Err(()) => kprintln_col(COL_RED, b"[helios] compilation failed"),
                    }
                    free(p);
                }
            }

            "run" => {
                if idx >= argc {
                    kprintln_col(COL_RED, b"helios run: missing file or source");
                    kprintln(b"  usage: helios run <file.hls|file.rxe>");
                    kprintln(b"         helios run <helios source code...>");
                    return;
                }
                // Decide: file path or inline source.
                // Heuristic — if the first token ends with .hls/.rxe, or names an
                // existing AVFS file, treat it as a file path.  Otherwise join all
                // remaining tokens and compile them as inline source.
                let first_ptr = *argv.add(idx as usize) as *const u8;
                let first = argv_str(first_ptr);
                let first_s = first.as_str();
                let looks_like_file = first_s.ends_with(".hls")
                    || first_s.ends_with(".rxe")
                    || avfs_file_exists(first_ptr);
                if looks_like_file {
                    helios_exec_file_stack(first_ptr, verbosity, stack_kb);
                } else {
                    // Inline source: join all remaining argv tokens
                    let raw = helios_collect_inline_src(argc, argv, idx);
                    let src = helios_maybe_wrap(&raw);
                    terminal_setcolor(COL_CYAN);
                    kprintln(b"[helios run] compiling inline source...");
                    terminal_setcolor(COL_WHITE);
                    helios_run_stack(&src, verbosity, stack_kb);
                }
            }

            // `eval` — always treats arguments as inline Helios source, never a file.
            // Bare snippets are auto-wrapped in fn main() { ... }.
            //
            //   helios eval print("hello");
            //   helios eval let x = cpuid(0); print_hex(x);
            //   helios eval fn main() { unsafe { asm!("cli"); } }
            "eval" => {
                if idx >= argc {
                    kprintln_col(COL_RED, b"helios eval: no source provided");
                    kprintln(b"  usage: helios eval <helios source code...>");
                    kprintln(b"  tip:   bare statements are auto-wrapped in fn main() { ... }");
                    return;
                }
                let raw = helios_collect_inline_src(argc, argv, idx);
                let src = helios_maybe_wrap(&raw);
                if verbosity >= 1 {
                    terminal_setcolor(COL_CYAN);
                    kprintln(b"[helios eval] source:");
                    terminal_setcolor(COL_GREY);
                    // Print the (possibly wrapped) source so the user can see what ran
                    for line in src.lines() {
                        kprint(b"  "); kprint_str(line); kputc(b'\n');
                    }
                    terminal_setcolor(COL_WHITE);
                }
                terminal_setcolor(COL_CYAN);
                kprintln(b"[helios eval] compiling...");
                terminal_setcolor(COL_WHITE);
                helios_run_stack(&src, verbosity, stack_kb);
            }

            "src" => {
                if idx >= argc { kprintln_col(COL_RED, b"helios src: missing file"); return; }
                let name = *argv.add(idx as usize) as *const u8;
                if let Some((p, sz)) = read_src(name) {
                    let src = core::str::from_utf8(core::slice::from_raw_parts(p, sz)).unwrap_or("");
                    helios_run_stack(src, verbosity, stack_kb);
                    free(p);
                }
            }

            "fmt" => {
                if idx >= argc { kprintln_col(COL_RED, b"helios fmt: missing file"); return; }
                let name = *argv.add(idx as usize) as *const u8;
                if let Some((p, sz)) = read_src(name) {
                    let src = core::str::from_utf8(core::slice::from_raw_parts(p, sz)).unwrap_or("");
                    let tokens = Lexer::new(src.as_bytes()).tokenise();
                    let (structs, fns, _consts_top, _prp_imports, _fetch_imports, diags) = Parser::new(tokens, src).parse_program();
                    if diags.has_errors() {
                        diags.emit_all();
                        kprintln_col(COL_RED, b"fmt: cannot format due to parse errors");
                    } else {
                        terminal_setcolor(COL_CYAN); kprintln(b"// formatted by helios fmt"); terminal_setcolor(COL_WHITE);
                        fmt_program(&structs, &fns);
                    }
                    free(p);
                }
            }

            "disasm" => {
                if idx >= argc { kprintln_col(COL_RED, b"helios disasm: missing file"); return; }
                let name = *argv.add(idx as usize) as *const u8;
                let fsize = avfs_get_filesize(name);
                if fsize <= 0 { kprintln_col(COL_RED, b"helios disasm: file not found"); return; }
                let p = malloc(fsize as u32);
                if p.is_null() { kprintln_col(COL_RED, b"helios disasm: OOM"); return; }
                avfs_read_file(name, p, fsize as u32, 0);
                let data = core::slice::from_raw_parts(p, fsize as usize);
                disasm_rxe(data);
                free(p);
            }

            "info" => {
                if idx >= argc { kprintln_col(COL_RED, b"helios info: missing file"); return; }
                let name = *argv.add(idx as usize) as *const u8;
                let fsize = avfs_get_filesize(name);
                if fsize < 32 { kprintln_col(COL_RED, b"helios info: file not found or too small"); return; }
                let p = malloc(fsize as u32);
                if p.is_null() { kprintln_col(COL_RED, b"helios info: OOM"); return; }
                avfs_read_file(name, p, fsize as u32, 0);
                let data = core::slice::from_raw_parts(p, fsize as usize);
                info_rxe(data);
                free(p);
            }

            "bench" => {
                if idx >= argc { kprintln_col(COL_RED, b"helios bench: missing file"); return; }
                let name = *argv.add(idx as usize) as *const u8;
                if let Some((p, sz)) = read_src(name) {
                    let src = core::str::from_utf8(core::slice::from_raw_parts(p, sz)).unwrap_or("");
                    bench_source(src, verbosity, stack_kb);
                    free(p);
                }
            }

            "demo" => {
                let name = if idx < argc {
                    let s = argv_str(*argv.add(idx as usize) as *const u8);
                    s.as_str().into()
                } else { String::from("") };
                if name.is_empty() {
                   kprint_col(COL_RED, b"helios demo: unknown demo. available:\n");
                   kprintln(b"  sys_info  vga_dashboard  mem_map  mem_dump  mem_search");
                   kprintln(b"  mem_bitmap  mem_fill_verify  mem_walk  heap_stress");
                   kprintln(b"  mem_diff  mem_copy_bench");
                } else {
                    run_demo(&name, verbosity, stack_kb);
                }
            }

            "cat" => {
                if idx >= argc { kprintln_col(COL_RED, b"helios cat: missing file"); return; }
                cat_file(*argv.add(idx as usize) as *const u8);
            }

            "deps" => {
                if idx >= argc { kprintln_col(COL_RED, b"helios deps: missing file"); return; }
                list_deps(*argv.add(idx as usize) as *const u8);
            }

            "build" => {
                // helios build file1.hls [file2.hls ...] -o out.rxe
                // Collect all .hls sources until -o flag or end of args
                let mut sources: Vec<(*const u8, usize)> = Vec::new();
                let mut out_name = [0u8; 128]; let mut out_len = 0usize;
                let mut reading_out = false;
                let mut i2 = idx;
                while i2 < argc {
                    let a = *argv.add(i2 as usize) as *const u8;
                    let mut alen = 0usize;
                    while *a.add(alen) != 0 && alen < 127 { alen += 1; }
                    let aslice = core::slice::from_raw_parts(a, alen);
                    if aslice == b"-o" {
                        i2 += 1;
                        if i2 < argc {
                            let op = *argv.add(i2 as usize) as *const u8;
                            while *op.add(out_len) != 0 && out_len < 126 {
                                out_name[out_len] = *op.add(out_len); out_len += 1;
                            }
                        }
                    } else {
                        sources.push((a, alen));
                    }
                    i2 += 1;
                }
                if sources.is_empty() {
                    kprintln_col(COL_RED, b"helios build: no source files"); return;
                }
                if out_len == 0 {
                    // default output name: first source with .rxe
                    let (a, alen) = sources[0];
                    let src_sl = core::slice::from_raw_parts(a, alen);
                    for &c in src_sl { if out_len < 120 { out_name[out_len] = c; out_len += 1; } }
                    if out_len >= 4 && &out_name[out_len-4..out_len] == b".hls" {
                        out_name[out_len-3] = b'r'; out_name[out_len-2] = b'x'; out_name[out_len-1] = b'e';
                    } else {
                        for &c in b".rxe" { if out_len < 127 { out_name[out_len] = c; out_len += 1; } }
                    }
                }
                out_name[out_len] = 0;
                // read all sources
                let mut src_bufs: Vec<(*mut u8, usize)> = Vec::with_capacity(sources.len());
                let mut all_ok = true;
                for &(name, _) in &sources {
                    let fsz = avfs_get_filesize(name);
                    if fsz < 0 {
                        kprint_col(COL_RED, b"helios build: file not found: ");
                        kprintln(core::slice::from_raw_parts(name, 64));
                        all_ok = false; break;
                    }
                    let p = malloc((fsz as u32).max(1) + 1);
                    if p.is_null() { kprintln_col(COL_RED, b"helios build: OOM"); all_ok = false; break; }
                    if fsz > 0 { avfs_read_file(name, p, fsz as u32, 0); }
                    *p.add(fsz as usize) = 0;
                    src_bufs.push((p, fsz as usize));
                }
                if all_ok {
                    // build a vec of &str slices, then merge
                    let str_slices: Vec<&str> = src_bufs.iter().map(|&(p, sz)| {
                        core::str::from_utf8(core::slice::from_raw_parts(p, sz)).unwrap_or("")
                    }).collect();
                    let merged = helios_merge_sources(&str_slices);
                    terminal_setcolor(COL_CYAN);
                    kprint(b"[helios build] merging "); kprint_i32(sources.len() as i32); kprintln(b" file(s)...");
                    terminal_setcolor(COL_WHITE);
                    match helios_compile(&merged, verbosity) {
                        Ok(rxe) => {
                            if !avfs_file_exists(out_name.as_ptr()) {
                                avfs_create_file(out_name.as_ptr(), rxe.len() as u32);
                            }
                            avfs_write_file(out_name.as_ptr(), rxe.as_ptr(), rxe.len() as u32, 0);
                            terminal_setcolor(COL_GREEN);
                            kprint(b"[helios build] ok  -> "); kprint(&out_name[..out_len]);
                            kprint(b"  ("); kprint_i32(rxe.len() as i32); kprintln(b" bytes)");
                            terminal_setcolor(COL_WHITE);
                        }
                        Err(()) => kprintln_col(COL_RED, b"[helios build] compilation failed"),
                    }
                }
                for (p, _) in src_bufs { free(p); }
            }

            "version" => {
                terminal_setcolor(COL_CYAN);
                kprintln(b"Helios v7.0 | author: scp_2801 | arch: i686 | RadiumOS");
                terminal_setcolor(COL_WHITE);
                kprintln(b"  RXE format version: 0x06");
                kprintln(b"  VM stack: 4 MB (heap-allocated, configurable via -stack)");
                kprintln(b"  Opcodes:  0x01..0xE9 (+ 0xFF Nop)");
                kprintln(b"  Features: include / multi-file build / expanded asm");
            }

            "symbols" => {
                if idx >= argc { kprintln_col(COL_RED, b"helios symbols: missing file"); return; }
                let name = *argv.add(idx as usize) as *const u8;
                let fsize = avfs_get_filesize(name);
                if fsize <= 0 { kprintln_col(COL_RED, b"helios symbols: file not found"); return; }
                let p = malloc(fsize as u32);
                if p.is_null() { kprintln_col(COL_RED, b"helios symbols: OOM"); return; }
                avfs_read_file(name, p, fsize as u32, 0);
                let data = core::slice::from_raw_parts(p, fsize as usize);
                // scan for SYM section
                let mut pos = 32usize; let mut found = false;
                while pos < data.len() {
                    let sec = data[pos]; pos += 1;
                    if sec == RXE_SECTION_END { break; }
                    if pos + 4 > data.len() { break; }
                    let slen = u32::from_le_bytes([data[pos], data[pos+1], data[pos+2], data[pos+3]]) as usize;
                    pos += 4;
                    if pos + slen > data.len() { break; }
                    if sec == RXE_SECTION_SYM {
                        terminal_setcolor(COL_CYAN); kprintln(b"  SYMBOL TABLE"); terminal_setcolor(COL_WHITE);
                        let mut sp = pos;
                        while sp + 5 < pos + slen {
                            let addr = u32::from_le_bytes([data[sp], data[sp+1], data[sp+2], data[sp+3]]); sp += 4;
                            let nlen = data[sp] as usize; sp += 1;
                            if sp + nlen > pos + slen { break; }
                            kprint_u32_hex(addr); kprint(b"  ");
                            kprint(&data[sp..sp+nlen]); kputc(b'\n');
                            sp += nlen;
                        }
                        found = true;
                    }
                    pos += slen;
                }
                if !found { kprintln_col(COL_YELLOW, b"  (no symbol section - compile with -g to include)"); }
                free(p);
            }

            "strip" => {
                if idx >= argc { kprintln_col(COL_RED, b"helios strip: missing file"); return; }
                let name = *argv.add(idx as usize) as *const u8;
                let fsize = avfs_get_filesize(name);
                if fsize <= 0 { kprintln_col(COL_RED, b"helios strip: file not found"); return; }
                let p = malloc(fsize as u32);
                if p.is_null() { kprintln_col(COL_RED, b"helios strip: OOM"); return; }
                avfs_read_file(name, p, fsize as u32, 0);
                let data = core::slice::from_raw_parts(p, fsize as usize);
                // Rebuild RXE without the SYM section
                let mut out: Vec<u8> = Vec::with_capacity(fsize as usize);
                out.extend_from_slice(&data[..32]); // header
                let mut pos = 32usize;
                while pos < data.len() {
                    let sec = data[pos];
                    if sec == RXE_SECTION_END { out.push(RXE_SECTION_END); break; }
                    if pos + 5 > data.len() { break; }
                    let slen = u32::from_le_bytes([data[pos+1], data[pos+2], data[pos+3], data[pos+4]]) as usize;
                    if sec != RXE_SECTION_SYM {
                        out.extend_from_slice(&data[pos..pos+5+slen]);
                    }
                    pos += 5 + slen;
                }
                let mut out_name = [0u8; 128]; let mut out_len2 = 0usize;
                let sn = name;
                while *sn.add(out_len2) != 0 && out_len2 < 120 { out_name[out_len2] = *sn.add(out_len2); out_len2 += 1; }
                // replace .rxe with _stripped.rxe
                if out_len2 >= 4 && &out_name[out_len2-4..out_len2] == b".rxe" { out_len2 -= 4; }
                for &c in b"_stripped.rxe" { if out_len2 < 127 { out_name[out_len2] = c; out_len2 += 1; } }
                out_name[out_len2] = 0;
                if !avfs_file_exists(out_name.as_ptr()) { avfs_create_file(out_name.as_ptr(), out.len() as u32); }
                avfs_write_file(out_name.as_ptr(), out.as_ptr(), out.len() as u32, 0);
                terminal_setcolor(COL_GREEN);
                kprint(b"[helios strip] "); kprint_i32(fsize as i32 - out.len() as i32); kprint(b" bytes removed -> ");
                kprint(&out_name[..out_len2]); kputc(b'\n');
                terminal_setcolor(COL_WHITE);
                free(p);
            }

            "build-multi" => {
                // helios build-multi file1.hls file2.hls ... -o out.rxe
                let mut files: Vec<*const u8> = Vec::new();
                let mut out_name = [0u8; 128];
                let mut out_len = 0usize;
                
                while idx < argc {
                    let a = *argv.add(idx as usize) as *const u8;
                    let alen = { let mut l = 0; while *a.add(l) != 0 { l += 1; } l };
                    let aslice = core::slice::from_raw_parts(a, alen);
                    
                    if aslice == b"-o" {
                        idx += 1;
                        let op = *argv.add(idx as usize) as *const u8;
                        while *op.add(out_len) != 0 && out_len < 127 {
                            out_name[out_len] = *op.add(out_len);
                            out_len += 1;
                        }
                    } else {
                        files.push(a);
                    }
                    idx += 1;
                }
                
                // Read all files
                let mut srcs: Vec<String> = Vec::new();
                for file in files {
                    let fsz = avfs_get_filesize(file);
                    if fsz <= 0 { continue; }
                    let p = malloc(fsz as u32 + 1);
                    avfs_read_file(file, p, fsz as u32, 0);
                    *p.add(fsz as usize) = 0;
                    let src = core::str::from_utf8(core::slice::from_raw_parts(p, fsz as usize))
                        .unwrap_or("");
                    srcs.push(src.into());
                    free(p);
                }
                
                // Merge and compile
                let merged = helios_merge_sources(&srcs.iter().map(|s| s.as_str()).collect::<Vec<_>>());
                match helios_compile(&merged, verbosity) {
                    Ok(rxe) => {
                        // Write to out_name
                        kprintln_col(COL_GREEN, b"[helios] multi-file build ok");
                    }
                    Err(()) => kprintln_col(COL_RED, b"[helios] multi-file build failed"),
                }
            }

            "import-check" => {
                if idx >= argc { kprintln_col(COL_RED, b"helios import-check: missing file"); return; }
                let name = *argv.add(idx as usize) as *const u8;
                if let Some((p, sz)) = read_src(name) {
                    let src = core::str::from_utf8(core::slice::from_raw_parts(p, sz)).unwrap_or("");
                    let tokens = Lexer::new(src.as_bytes()).tokenise();
                    let (_, _, _, _, _, diags) = Parser::new(tokens, src).parse_program();
                    // Count unresolved imports vs. resolved
                    if diags.has_errors() {
                        kprintln_col(COL_RED, b"import-check: FAILED");
                    } else {
                        kprintln_col(COL_GREEN, b"import-check: OK");
                    }
                    free(p);
                }
            }

            _ => {
                kprint_col(COL_RED, b"helios: unknown subcommand `");
                kprint(sub.as_bytes()); kprintln(b"`");
                kprintln(b"tip: use `helios eval <code>` to run inline source");
                kprintln(b"     run `helios` with no args for full usage");
            }
        }
    }
}




static DEMO_SYS_INFO: &str = r#"
// sys_info — verbose system information dump
fn main() {
    print("========================================\n");
    print("  HELIOS SYSTEM INFORMATION\n");
    print("========================================\n");

    print("  --- CPU ---\n");
    let max_leaf = 0;
    unsafe { max_leaf = cpuid(0); }
    print("  CPUID max leaf:   "); print(max_leaf); print("\n");
    let leaf1 = 0;
    unsafe { leaf1 = cpuid(1); }
    print("  CPUID leaf1 EAX:  0x"); print_hex(leaf1); print("\n");

    print("  --- CONTROL REGISTERS ---\n");
    let cr0 = 0;
    let cr2 = 0;
    let cr3 = 0;
    unsafe { cr0 = get_cr0(); cr2 = get_cr2(); cr3 = get_cr3(); }
    print("  CR0: 0x"); print_hex(cr0); print("\n");
    print("  CR2: 0x"); print_hex(cr2); print("\n");
    print("  CR3: 0x"); print_hex(cr3); print("\n");

    print("  --- SEGMENT REGISTERS ---\n");
    let cs = 0; let ds = 0; let es = 0;
    let fs = 0; let gs = 0; let ss = 0;
    unsafe {
        cs = get_cs(); ds = get_ds(); es = get_es();
        fs = get_fs(); gs = get_gs(); ss = get_ss();
    }
    print("  CS=0x"); print_hex(cs);
    print(" DS=0x"); print_hex(ds);
    print(" ES=0x"); print_hex(es); print("\n");
    print("  FS=0x"); print_hex(fs);
    print(" GS=0x"); print_hex(gs);
    print(" SS=0x"); print_hex(ss); print("\n");

    print("  --- DESCRIPTORS ---\n");
    let gdt = 0; let idt = 0;
    unsafe { gdt = sgdt(); idt = sidt(); }
    print("  GDT base: 0x"); print_hex(gdt); print("\n");
    print("  IDT base: 0x"); print_hex(idt); print("\n");

    print("  --- TIMER ---\n");
    let ticks = 0;
    unsafe { ticks = get_ticks(); }
    print("  Uptime ticks: "); print(ticks); print("\n");
    let tsc = rdtsc();
    print("  TSC:          0x"); print_hex(tsc); print("\n");

    print("  --- MEMORY ---\n");
    let total = mem_query();
    print("  Total RAM:    "); print(total); print(" bytes\n");

    print("========================================\n");
    print("sys_info: done.\n");
}
"#;

static DEMO_VGA_DASHBOARD: &str = r#"
// vga_dashboard — paint a full system info dashboard on VGA text buffer

fn hex_digit(n: int) -> int {
    let d = n & 0xF;
    match d {
        0  => { return 48; },
        1  => { return 49; },
        2  => { return 50; },
        3  => { return 51; },
        4  => { return 52; },
        5  => { return 53; },
        6  => { return 54; },
        7  => { return 55; },
        8  => { return 56; },
        9  => { return 57; },
        10 => { return 65; },
        11 => { return 66; },
        12 => { return 67; },
        13 => { return 68; },
        14 => { return 69; },
        _  => { return 70; }
    }
}

fn vc(x: int, y: int, ch: int, co: int) {
    unsafe {
        let off = ((y * 80) + x) * 2;
        poke(0xB8000 + off, ch, 8);
        poke(0xB8000 + off + 1, co, 8);
    }
}

fn vs(x: int, y: int, s: str, co: int) {
    let n = strlen(s);
    for i in 0..n {
        let ch = strbyte(strsub(s, i, 1));
        vc(x + i, y, ch, co);
    }
}

fn vh(x: int, y: int, v: int, co: int) {
    vc(x, y, 48, co);
    vc(x + 1, y, 120, co);
    vc(x + 2, y, hex_digit(v >> 28), co);
    vc(x + 3, y, hex_digit(v >> 24), co);
    vc(x + 4, y, hex_digit(v >> 20), co);
    vc(x + 5, y, hex_digit(v >> 16), co);
    vc(x + 6, y, hex_digit(v >> 12), co);
    vc(x + 7, y, hex_digit(v >> 8), co);
    vc(x + 8, y, hex_digit(v >> 4), co);
    vc(x + 9, y, hex_digit(v), co);
}

fn vd(x: int, y: int, v: int, co: int) {
    vs(x, y, inttostr(v), co);
}

fn main() {
    print("vga_dashboard demo: painting system dashboard to VGA buffer\n");

    let W = 0x0F; let C = 0x0B; let G = 0x0A;
    let Y = 0x0E; let D = 0x07;

    // Clear screen
    for r in 0..25 {
        for c in 0..80 {
            vc(c, r, 32, D);
        }
    }

    // Title bar
    vs(0, 0, " HELIOS v6.0 SYSTEM DASHBOARD ", 0x4E);

    // CPU
    vs(2, 2, "[CPU]", C);
    let ml = 0;
    unsafe { ml = cpuid(0); }
    vs(2, 3, "CPUID max leaf:", G); vd(19, 3, ml, W);
    let f1 = 0;
    unsafe { f1 = cpuid(1); }
    vs(2, 4, "Leaf1 EAX:", G); vh(14, 4, f1, Y);

    // Control registers
    vs(2, 6, "[CONTROL REGISTERS]", C);
    let cr0 = 0; let cr2 = 0; let cr3 = 0;
    unsafe { cr0 = get_cr0(); cr2 = get_cr2(); cr3 = get_cr3(); }
    vs(2, 7, "CR0:", G); vh(7, 7, cr0, Y);
    vs(22, 7, "CR2:", G); vh(27, 7, cr2, Y);
    vs(2, 8, "CR3:", G); vh(7, 8, cr3, Y);

    // Segment registers
    vs(2, 10, "[SEGMENT REGISTERS]", C);
    let cs = 0; let ds = 0; let es = 0;
    let fs = 0; let gs = 0; let ss = 0;
    unsafe {
        cs = get_cs(); ds = get_ds(); es = get_es();
        fs = get_fs(); gs = get_gs(); ss = get_ss();
    }
    vs(2, 11, "CS:", G); vh(6, 11, cs, Y);
    vs(18, 11, "DS:", G); vh(22, 11, ds, Y);
    vs(34, 11, "ES:", G); vh(38, 11, es, Y);
    vs(2, 12, "FS:", G); vh(6, 12, fs, Y);
    vs(18, 12, "GS:", G); vh(22, 12, gs, Y);
    vs(34, 12, "SS:", G); vh(38, 12, ss, Y);

    // Descriptors
    vs(2, 14, "[DESCRIPTORS]", C);
    let gdt = 0; let idt = 0;
    unsafe { gdt = sgdt(); idt = sidt(); }
    vs(2, 15, "GDT:", G); vh(7, 15, gdt, Y);
    vs(22, 15, "IDT:", G); vh(27, 15, idt, Y);

    // Timer
    vs(2, 17, "[TIMER]", C);
    let ticks = 0;
    unsafe { ticks = get_ticks(); }
    vs(2, 18, "Uptime:", G); vd(10, 18, ticks, W);
    vs(20, 18, "ticks", D);

    // Memory
    vs(2, 20, "[MEMORY]", C);
    let total = mem_query();
    vs(2, 21, "Total RAM:", G); vd(13, 21, total, W);
    vs(25, 21, "bytes", D);

    let nz = 0;
    for a in 0..1024 {
        let v = 0;
        unsafe { v = peek(a * 4, 32); }
        if v != 0 { nz += 1; }
    }
    vs(2, 22, "Non-zero dwords 0-4KB:", G); vd(25, 22, nz, Y);

    vs(0, 24, " Dashboard active.                            ", 0x17);
    print("vga_dashboard: done.\n");
}
"#;

static DEMO_MEM_MAP: &str = r#"
// mem_map — scan and categorize memory regions, attempt hw_map_mem
fn main() {
    print("========================================\n");
    print("  MEMORY MAP\n");
    print("========================================\n");

    let total = mem_query();
    print("  Total reported: "); print(total); print(" bytes\n");

    // Scan conventional memory 0..640KB in 4KB pages
    let present = 0;
    let zero_pages = 0;
    for page in 0..160 {
        let base = page * 4096;
        let nz = 0;
        for off in 0..4 {
            let val = 0;
            unsafe { val = peek(base + off * 64, 32); }
            if val != 0 { nz += 1; }
        }
        match nz {
            0 => { zero_pages += 1; },
            _ => { present += 1; }
        }
    }
    print("  0-640KB pages with data: "); print(present); print("\n");
    print("  0-640KB zero pages:     "); print(zero_pages); print("\n");

    // Scan upper memory 640KB..1MB
    let upper_nz = 0;
    for page in 160..256 {
        let base = page * 4096;
        let nz = 0;
        for off in 0..4 {
            let val = 0;
            unsafe { val = peek(base + off * 64, 32); }
            if val != 0 { nz += 1; }
        }
        match nz {
            0 => {},
            _ => { upper_nz += 1; }
        }
    }
    print("  640KB-1MB pages with data: "); print(upper_nz); print("\n");

    // Identity-map first 1MB
    let map_ok = 0;
    unsafe { map_ok = hw_map_mem(0, 0x100000); }
    match map_ok {
        0  => { print("  hw_map_mem(0, 1MB): ok\n"); },
        _  => { print("  hw_map_mem(0, 1MB): failed\n"); }
    }

    print("========================================\n");
    print("mem_map: done.\n");
}
"#;

static DEMO_MEM_DUMP: &str = r#"
// mem_dump — hex dump of low physical memory (16 rows x 16 bytes)
fn phb(b: int) {
    let hi = (b >> 4) & 0xF;
    let lo = b & 0xF;
    match hi {
        0  => { print("0"); }, 1  => { print("1"); }, 2  => { print("2"); },
        3  => { print("3"); }, 4  => { print("4"); }, 5  => { print("5"); },
        6  => { print("6"); }, 7  => { print("7"); }, 8  => { print("8"); },
        9  => { print("9"); }, 10 => { print("A"); }, 11 => { print("B"); },
        12 => { print("C"); }, 13 => { print("D"); }, 14 => { print("E"); },
        _  => { print("F"); }
    }
    match lo {
        0  => { print("0"); }, 1  => { print("1"); }, 2  => { print("2"); },
        3  => { print("3"); }, 4  => { print("4"); }, 5  => { print("5"); },
        6  => { print("6"); }, 7  => { print("7"); }, 8  => { print("8"); },
        9  => { print("9"); }, 10 => { print("A"); }, 11 => { print("B"); },
        12 => { print("C"); }, 13 => { print("D"); }, 14 => { print("E"); },
        _  => { print("F"); }
    }
}

fn main() {
    print("========================================\n");
    print("  MEMORY DUMP  0x0000 - 0x0100\n");
    print("========================================\n");

    let base = 0;
    for row in 0..16 {
        let addr = base + row * 16;
        print("  ");
        print_hex(addr);
        print(": ");

        for col in 0..16 {
            let b = 0;
            unsafe { b = peek(addr + col, 8); }
            phb(b);
            print(" ");
        }

        // ASCII column
        print("|");
        for col in 0..16 {
            let b = 0;
            unsafe { b = peek(addr + col, 8); }
            match b {
                0  => { print("."); },
                _  => { print("."); }
            }
        }
        print("|\n");
    }
    print("========================================\n");
    print("mem_dump: done.\n");
}
"#;

static DEMO_MEM_SEARCH: &str = r#"
// mem_search — search for a specific byte value in a memory range
fn main() {
    print("========================================\n");
    print("  MEMORY SEARCH\n");
    print("========================================\n");

    // Search for 0xFF in first 4KB
    let needle = 0xFF;
    let range = 4096;
    let found = 0;
    let max_hits = 16;

    print("  Searching for 0xFF in 0x0000..0x");
    print_hex(range);
    print("\n");

    for addr in 0..range {
        let b = 0;
        unsafe { b = peek(addr, 8); }
        if b == needle {
            found += 1;
            print("  [0x"); print_hex(addr); print("] = 0xFF\n");
            if found >= max_hits { break; }
        }
    }
    match found {
        0 => { print("  (no matches found)\n"); },
        _ => { print("  Total matches: "); print(found); print("\n"); }
    }

    // Search for non-zero dword pattern in 0..0x10000
    print("\n  Scanning 0..0x10000 for non-zero dwords...\n");
    let nz_count = 0;
    for addr in 0..16384 {
        let v = 0;
        unsafe { v = peek(addr * 4, 32); }
        if v != 0 { nz_count += 1; }
    }
    print("  Non-zero dwords: "); print(nz_count); print(" / 16384\n");

    print("========================================\n");
    print("mem_search: done.\n");
}
"#;

static DEMO_MEM_BITMAP: &str = r#"
// mem_bitmap — build a page-level presence bitmap of low memory
fn main() {
    print("========================================\n");
    print("  MEMORY PAGE BITMAP (0 - 4 MB)\n");
    print("========================================\n");

    // 4 MB / 4 KB = 1024 pages
    let pages = 1024;
    let present = 0;
    let absent = 0;

    // Scan in 64-page chunks for readable output
    for chunk in 0..16 {
        let base_page = chunk * 64;
        let chunk_present = 0;
        for p in 0..64 {
            let page = base_page + p;
            let base = page * 4096;
            let nz = 0;
            // Sample 8 dwords per page
            for s in 0..8 {
                let v = 0;
                unsafe { v = peek(base + s * 512, 32); }
                if v != 0 { nz += 1; }
            }
            match nz {
                0 => { absent += 1; },
                _ => { chunk_present += 1; present += 1; }
            }
        }
        print("  ");
        print_hex(base_page * 4096);
        print("..");
        print_hex((base_page + 64) * 4096);
        print(": ");
        print(chunk_present);
        print("/64 pages present\n");
    }

    print("  ---------------------------\n");
    print("  Total present: "); print(present); print(" / "); print(pages); print("\n");
    print("  Total absent:  "); print(absent);  print(" / "); print(pages); print("\n");

    print("========================================\n");
    print("mem_bitmap: done.\n");
}
"#;

static DEMO_MEM_FILL_VERIFY: &str = r#"
// mem_fill_verify — alloc a page, fill with pattern, verify, then free
fn main() {
    print("========================================\n");
    print("  MEMORY FILL & VERIFY\n");
    print("========================================\n");

    // Allocate a page
    let page = 0;
    unsafe { page = alloc_page(); }
    print("  Allocated page at: 0x"); print_hex(page); print("\n");

    match page {
        0 => {
            print("  alloc_page returned 0 — no page available\n");
        },
        _ => {
            // Fill with pattern 0xAA
            print("  Filling 4096 bytes with 0xAA...\n");
            unsafe {
                for i in 0..4096 {
                    poke(page + i, 0xAA, 8);
                }
            }

            // Verify
            let errors = 0;
            print("  Verifying...\n");
            for i in 0..4096 {
                let b = 0;
                unsafe { b = peek(page + i, 8); }
                if b != 0xAA { errors += 1; }
            }
            match errors {
                0 => { print("  VERIFY PASS: all 4096 bytes = 0xAA\n"); },
                _ => { print("  VERIFY FAIL: "); print(errors); print(" mismatches\n"); }
            }

            // Fill with incrementing pattern
            print("  Filling with incrementing pattern 0..255...\n");
            unsafe {
                for i in 0..4096 {
                    poke(page + i, i & 0xFF, 8);
                }
            }

            // Verify incrementing
            let err2 = 0;
            for i in 0..4096 {
                let b = 0;
                unsafe { b = peek(page + i, 8); }
                if b != (i & 0xFF) { err2 += 1; }
            }
            match err2 {
                0 => { print("  VERIFY PASS: incrementing pattern ok\n"); },
                _ => { print("  VERIFY FAIL: "); print(err2); print(" mismatches\n"); }
            }

            // Free the page
            unsafe { free_page(page); }
            print("  Freed page 0x"); print_hex(page); print("\n");
        }
    }

    print("========================================\n");
    print("mem_fill_verify: done.\n");
}
"#;

static DEMO_MEM_WALK: &str = r#"
// mem_walk — walk page tables from CR3, report present entries
fn main() {
    print("========================================\n");
    print("  PAGE TABLE WALK\n");
    print("========================================\n");

    let cr3 = 0;
    unsafe { cr3 = get_cr3(); }
    print("  CR3 (PDBR): 0x"); print_hex(cr3); print("\n");

    let pd_base = cr3 & 0xFFFFF000;
    print("  Page directory at: 0x"); print_hex(pd_base); print("\n\n");

    let present_pdes = 0;
    let total_ptes = 0;

    for i in 0..1024 {
        let pde_addr = pd_base + i * 4;
        let pde = 0;
        unsafe { pde = peek(pde_addr, 32); }
        if (pde & 1) != 0 {
            present_pdes += 1;
            print("  PDE["); print(i); print("] = 0x"); print_hex(pde);
            print("  PT=0x"); print_hex(pde & 0xFFFFF000); print("\n");

            // Walk this page table
            let pt_base = pde & 0xFFFFF000;
            let pt_present = 0;
            for j in 0..1024 {
                let pte_addr = pt_base + j * 4;
                let pte = 0;
                unsafe { pte = peek(pte_addr, 32); }
                if (pte & 1) != 0 { pt_present += 1; }
            }
            print("    -> "); print(pt_present); print("/1024 PTEs present\n");
            total_ptes += pt_present;

            if present_pdes >= 12 {
                print("    ... (stopping after 12 PDEs)\n");
                break;
            }
        }
    }

    print("\n  Summary:\n");
    print("  Present PDEs:  "); print(present_pdes); print(" / 1024\n");
    print("  Total PTEs:    "); print(total_ptes); print("\n");

    print("========================================\n");
    print("mem_walk: done.\n");
}
"#;

static DEMO_HEAP_STRESS: &str = r#"
// heap_stress — allocate, write, read, verify, and free heap blocks
fn main() {
    print("========================================\n");
    print("  HEAP STRESS TEST\n");
    print("========================================\n");

    let num_blocks = 8;
    let block_size = 256;

    // Allocate blocks
    let b0 = 0; let b1 = 0; let b2 = 0; let b3 = 0;
    let b4 = 0; let b5 = 0; let b6 = 0; let b7 = 0;
    b0 = malloc(block_size);
    b1 = malloc(block_size);
    b2 = malloc(block_size);
    b3 = malloc(block_size);
    b4 = malloc(block_size);
    b5 = malloc(block_size);
    b6 = malloc(block_size);
    b7 = malloc(block_size);

    print("  Allocated 8 x "); print(block_size); print("-byte blocks:\n");
    print("    b0=0x"); print_hex(b0); print(" b1=0x"); print_hex(b1);
    print(" b2=0x"); print_hex(b2); print(" b3=0x"); print_hex(b3); print("\n");
    print("    b4=0x"); print_hex(b4); print(" b5=0x"); print_hex(b5);
    print("    b6=0x"); print_hex(b6); print(" b7=0x"); print_hex(b7); print("\n");

    // Write unique pattern to each block
    print("  Writing unique patterns...\n");
    unsafe {
        for i in 0..block_size {
            poke(b0 + i, (i & 0xFF), 8);
            poke(b1 + i, ((i + 1) & 0xFF), 8);
            poke(b2 + i, ((i + 2) & 0xFF), 8);
            poke(b3 + i, ((i + 3) & 0xFF), 8);
            poke(b4 + i, ((i + 4) & 0xFF), 8);
            poke(b5 + i, ((i + 5) & 0xFF), 8);
            poke(b6 + i, ((i + 6) & 0xFF), 8);
            poke(b7 + i, ((i + 7) & 0xFF), 8);
        }
    }

    // Verify each block
    print("  Verifying...\n");
    let err0 = 0; let err1 = 0; let err2 = 0; let err3 = 0;
    let err4 = 0; let err5 = 0; let err6 = 0; let err7 = 0;
    for i in 0..block_size {
        let v = 0;
        unsafe {
            v = peek(b0 + i, 8);
        }
        if v != (i & 0xFF) { err0 += 1; }
        unsafe { v = peek(b1 + i, 8); }
        if v != ((i + 1) & 0xFF) { err1 += 1; }
        unsafe { v = peek(b2 + i, 8); }
        if v != ((i + 2) & 0xFF) { err2 += 1; }
        unsafe { v = peek(b3 + i, 8); }
        if v != ((i + 3) & 0xFF) { err3 += 1; }
    }
    let total_err = err0 + err1 + err2 + err3;
    match total_err {
        0 => { print("  Blocks 0-3: PASS\n"); },
        _ => { print("  Blocks 0-3: FAIL ("); print(total_err); print(" errors)\n"); }
    }

    // Free all blocks
    print("  Freeing all blocks...\n");
    free(b0); free(b1); free(b2); free(b3);
    free(b4); free(b5); free(b6); free(b7);

    // Allocate again to verify heap reuse
    print("  Re-allocating...\n");
    let r0 = malloc(block_size);
    let r1 = malloc(block_size);
    print("    r0=0x"); print_hex(r0); print(" r1=0x"); print_hex(r1); print("\n");
    free(r0); free(r1);

    print("========================================\n");
    print("heap_stress: done.\n");
}
"#;

static DEMO_MEM_DIFF: &str = r#"
// mem_diff — compare two memory regions byte-by-byte, report differences
fn main() {
    print("========================================\n");
    print("  MEMORY DIFF\n");
    print("========================================\n");

    let base_a = 0x0000;
    let base_b = 0x1000;
    let len = 512;
    let max_show = 24;

    print("  Comparing 0x"); print_hex(base_a);
    print(" vs 0x"); print_hex(base_b);
    print(" ("); print(len); print(" bytes)\n\n");

    let diffs = 0;
    let shown = 0;
    for i in 0..len {
        let a = 0; let b = 0;
        unsafe {
            a = peek(base_a + i, 8);
            b = peek(base_b + i, 8);
        }
        if a != b {
            diffs += 1;
            if shown < max_show {
                print("  [0x"); print_hex(i); print("] ");
                print_hex(a); print(" -> "); print_hex(b); print("\n");
                shown += 1;
            }
        }
    }

    match diffs {
        0 => { print("  Regions are identical.\n"); },
        _ => {
            print("  ...\n");
            print("  Total differences: "); print(diffs); print("\n");
        }
    }

    // Self-comparison (should always be 0 diffs)
    print("\n  Self-comparison 0x"); print_hex(base_a);
    print(" vs 0x"); print_hex(base_a); print("...\n");
    let self_diff = 0;
    for i in 0..len {
        let a = 0; let b = 0;
        unsafe {
            a = peek(base_a + i, 8);
            b = peek(base_a + i, 8);
        }
        if a != b { self_diff += 1; }
    }
    match self_diff {
        0 => { print("  Self-compare: PASS (0 differences)\n"); },
        _ => { print("  Self-compare: FAIL ("); print(self_diff); print(" differences)\n"); }
    }

    print("========================================\n");
    print("mem_diff: done.\n");
}
"#;

static DEMO_MEM_COPY_BENCH: &str = r#"
// mem_copy_bench — benchmark memcpy/memset and measure byte throughput
fn main() {
    print("========================================\n");
    print("  MEMORY COPY BENCHMARK\n");
    print("========================================\n");

    let size = 4096;
    let page = 0;
    let page2 = 0;
    unsafe {
        page = alloc_page();
        page2 = alloc_page();
    }

    match page {
        0 => { print("  alloc_page failed\n"); },
        _ => {
            // memset benchmark
            let t0 = 0;
            unsafe { t0 = get_ticks(); }
            unsafe { memset(page, 0xAB, size); }
            let t1 = 0;
            unsafe { t1 = get_ticks(); }
            let ms_ticks = t1 - t0;
            print("  memset 4KB: "); print(ms_ticks); print(" ticks\n");

            // Verify memset
            let err = 0;
            for i in 0..size {
                let b = 0;
                unsafe { b = peek(page + i, 8); }
                if b != 0xAB { err += 1; }
            }
            match err {
                0 => { print("  memset verify: PASS\n"); },
                _ => { print("  memset verify: FAIL ("); print(err); print(")\n"); }
            }

            // memcpy benchmark
            let t2 = 0;
            unsafe { t2 = get_ticks(); }
            unsafe { memcpy(page2, page, size); }
            let t3 = 0;
            unsafe { t3 = get_ticks(); }
            let cp_ticks = t3 - t2;
            print("  memcpy 4KB: "); print(cp_ticks); print(" ticks\n");

            // Verify memcpy
            let err2 = 0;
            for i in 0..size {
                let a = 0; let b = 0;
                unsafe {
                    a = peek(page + i, 8);
                    b = peek(page2 + i, 8);
                }
                if a != b { err2 += 1; }
            }
            match err2 {
                0 => { print("  memcpy verify: PASS\n"); },
                _ => { print("  memcpy verify: FAIL ("); print(err2); print(")\n"); }
            }

            unsafe { free_page(page); free_page(page2); }
            print("  Pages freed.\n");
        }
    }

    print("========================================\n");
    print("mem_copy_bench: done.\n");
}
"#;

unsafe fn run_demo(name: &str, verbosity: u8, stack_kb: u32) {
    let src: &str = match name {
        "sys_info"        => DEMO_SYS_INFO,
        "vga_dashboard"   => DEMO_VGA_DASHBOARD,
        "mem_map"         => DEMO_MEM_MAP,
        "mem_dump"        => DEMO_MEM_DUMP,
        "mem_search"      => DEMO_MEM_SEARCH,
        "mem_bitmap"      => DEMO_MEM_BITMAP,
        "mem_fill_verify" => DEMO_MEM_FILL_VERIFY,
        "mem_walk"        => DEMO_MEM_WALK,
        "heap_stress"     => DEMO_HEAP_STRESS,
        "mem_diff"        => DEMO_MEM_DIFF,
        "mem_copy_bench"  => DEMO_MEM_COPY_BENCH,
        _ => {
            kprint_col(COL_RED, b"helios demo: unknown demo. available:\n");
            kprintln(b"  sys_info  vga_dashboard  mem_map  mem_dump  mem_search");
            kprintln(b"  mem_bitmap  mem_fill_verify  mem_walk  heap_stress");
            kprintln(b"  mem_diff  mem_copy_bench");
            return;
        }
    };
    terminal_setcolor(COL_CYAN);
    kprint(b"[helios demo] running: "); kprintln(name.as_bytes());
    terminal_setcolor(COL_WHITE);
    helios_run_stack(src.trim(), verbosity, stack_kb);
}


// ── v6: cat — dump file with line numbers ─────────────────────────────────────
unsafe fn cat_file(name: *const u8) {
    let fsz = avfs_get_filesize(name);
    if fsz <= 0 { kprintln_col(COL_RED, b"helios cat: file not found"); return; }
    let p = malloc(fsz as u32 + 1);
    if p.is_null() { kprintln_col(COL_RED, b"helios cat: OOM"); return; }
    avfs_read_file(name, p, fsz as u32, 0);
    *p.add(fsz as usize) = 0;
    let src = core::slice::from_raw_parts(p, fsz as usize);
    let mut line = 1usize;
    terminal_setcolor(COL_BLUE);
    kprint_i32(line as i32); kprint(b" | ");
    terminal_setcolor(COL_WHITE);
    for &c in src {
        if c == b'\n' {
            kputc(b'\n');
            line += 1;
            terminal_setcolor(COL_BLUE);
            kprint_i32(line as i32); kprint(b" | ");
            terminal_setcolor(COL_WHITE);
        } else {
            kputc(c);
        }
    }
    kputc(b'\n');
    free(p);
}

// ── v6: deps — list include dependencies ──────────────────────────────────────
unsafe fn list_deps(name: *const u8) {
    let fsz = avfs_get_filesize(name);
    if fsz <= 0 { kprintln_col(COL_RED, b"helios deps: file not found"); return; }
    let p = malloc(fsz as u32 + 1);
    if p.is_null() { kprintln_col(COL_RED, b"helios deps: OOM"); return; }
    avfs_read_file(name, p, fsz as u32, 0);
    *p.add(fsz as usize) = 0;
    let src = core::str::from_utf8(core::slice::from_raw_parts(p, fsz as usize)).unwrap_or("");
    terminal_setcolor(COL_CYAN); kprintln(b"[helios deps]"); terminal_setcolor(COL_WHITE);
    let mut found = 0usize;
    for line in src.lines() {
        let trimmed = line.trim();
        if trimmed.starts_with("include") {
            let rest = trimmed[7..].trim();
            if rest.starts_with('"') {
                let end = rest[1..].find('"').unwrap_or(0);
                let fname = &rest[1..1+end];
                kprint(b"  -> "); kprintln(fname.as_bytes());
                found += 1;
            }
        }
    }
    if found == 0 { kprintln_col(COL_GREY, b"  (no includes)"); }
    free(p);
}

// ============================================================================
//  SECTION 17 - INIT
// ============================================================================

#[no_mangle]
pub extern "C" fn helios_init() {
    unsafe {
        register_command(
            b"helios\0".as_ptr(),
            b"Helios compiler v7.0 - .hls source / .rxe bytecode / inline eval / full hardware access\0".as_ptr(),
            helios_command,
        );
        terminal_setcolor(COL_GREEN);
        kprintln(b"[helios] v7.0 loaded");
        kprintln(b"         new:   inline eval  `helios eval <code>`");
        kprintln(b"                             `helios run  <code>`  (auto-detects file vs source)");
        kprintln(b"                bare snippets auto-wrapped in fn main() { ... }");
        kprintln(b"         v6:    include directives, multi-file build, expanded inline asm");
        kprintln(b"                hlt/pushfd/popfd/lidt/lgdt/wbinvd/invlpg/get_cr2/clts/hw_map_mem");
        kprintln(b"         `helios` for full usage  |  `helios test all` for test suite");
        terminal_setcolor(COL_WHITE);
    }
}