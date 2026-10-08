# Helios v6.0 Language Reference & Specification

**Target Environment:** RadiumOS i686 (Ring 0 / Ring 3)  
**Binary Format:** RXE (RadiumOS Executable) v6  
**Author:** `scp_2801`  

---

## Version History

| Version | Summary |
| :--- | :--- |
| **v3.0** | Initial language spec: stack VM, structs, unsafe blocks, PRP imports |
| **v4.0** | `let mut`, void inference, named `break`, stepped `for`, nested field chains, SetGs fix, arena allocators, 64 K const pool, new diagnostics (E003/E004/E008/E009/E011), new shell commands (`disasm`, `info`, `bench`, `fmt`), string ops (`StrCat`, `StrSub`, `StrFind`, `IntToStr`) |
| **v5.0** | `if let`, top-level `const`, `assert!()`, `\uXXXX` escapes, char literals, `StrUpper`/`StrLower`/`PrintStr`/`StrByte` opcodes, `VM_UNSAFE_STACK_RESERVE`, heap tracker, step counter / E_TIMEOUT, new shell commands (`version`, `symbols`, `strip`, `const`), `helios run -stack <KB>`, `-vvv` trace, new diagnostics (E012/E013/W603/W604) |
| **v6.0** | `include` directive, multi-file `build`, `print_hex` as statement, expanded inline asm (hlt / pushfd / popfd / lidt / lgdt / wbinvd / invlpg / get_cr2 / clts / hw_map_mem), new shell commands (`cat`, `deps`, `build`, `demo`), RXE version bumped to `0x06` |

---

## 1. Architecture & Execution Model

Helios is a systems-level compiled language that targets the RXE v6 binary format. Programs run directly inside RadiumOS at Ring 0 or Ring 3, with full access to hardware when `unsafe` is used.

**VM limits:**

| Resource | Limit |
| :--- | :--- |
| Value stack | 4 MB heap-allocated (configurable with `-stack <KB>`) |
| Call stack depth | 256 frames |
| Named registers | `r0`–`r15` (16 × 32-bit) |
| Const pool | 64 K entries |
| Execution timeout | 50 million steps (E_TIMEOUT) |
| Unsafe reserve | 16 KB pre-faulted scratch page freed on first `unsafe` entry |

**RXE v6 header** (32 bytes):

| Offset | Field | Value |
| :--- | :--- | :--- |
| 0–7 | magic | `RADIUM_X` |
| 8 | version | `0x06` |
| 9 | arch | `0x86` (i686) |
| 10–13 | entry_point | LE u32 |
| 14–17 | code_size | LE u32 |
| 18–21 | data_size | LE u32 |
| 22–23 | flags | LE u16 |
| 24–31 | author_tag | 8 bytes (`scp_2801`) |

RXE sections follow the header: `0x01` CODE, `0x02` DATA, `0x03` SYM (optional debug symbol table), `0xFF` END.

---

## 2. Core Syntax & Data Structures

Helios uses a deterministic Rust-like syntax. Every program requires a `main` entry point.

### Variables

```rust
// Immutable binding (type annotation optional)
let base_addr: int = 0xB8000;

// let mut is accepted; mutability is advisory (memory model is already mutable)
let mut counter: int = 0;

// Shorthand: type can be omitted
let x = 42;
```

### Numeric Literals

```rust
let dec  = 255;
let hex  = 0xFF;
let bin  = 0b11111111;
let ch   = 'A';        // char literal → integer 65
```

Type suffixes (`u8`, `i32`, etc.) are parsed and discarded — all integers are 32-bit internally.

### Top-Level Constants (v5)

```rust
const MAX_COLS: int = 80;
const VGA_BASE: int = 0xB8000;

fn main() {
    let offset = MAX_COLS * 2;
}
```

Constants are hoisted and substituted at compile time. Redefining a constant name raises E013.

### Arrays

```rust
// Explicit array literal
let buffer: [int; 8] = [0; 8];

// Shorthand allocation
let fast_buf[16] = 0;

// Array literal with values
let primes = [2, 3, 5, 7, 11];

// Array index assignment
buffer[0] = 0xFF;
```

Arrays > 256 elements emit W601 (heap pressure warning).

### Structs

```rust
struct VgaChar {
    ascii: int,
    color: int
}

// Instantiate with `new`
let cell = new VgaChar { ascii: 65, color: 0x0F };

// Field access (single or chained)
let a = cell.ascii;
let deep = device.header.flags;   // nested chain resolved at compile time
```

Fields are stored linearly in declaration order. Organize largest → smallest types for hardware register structs to maintain predictable alignment.

### Functions

```rust
// Return type is required for non-void functions
fn calculate_offset(x: int, y: int) -> int {
    return (y * 80) + x;
}

// `-> void` is optional on void functions
fn setup_network() {
    // ...
}
```

Functions with > 16 parameters emit W602. Duplicate function names raise E003.

---

## 3. Control Flow

### if / else

```rust
if condition {
    // then
} else if other {
    // ...
} else {
    // fallback
}

// Parentheses around the condition are optional
if (x > 0) { ... }
```

Constant conditions (literals) emit W201.

### if let (v5)

```rust
// Pattern-match an integer expression
if let IntLit(n) = some_expr {
    print(n);
}
```

Currently supports integer patterns only.

### while

```rust
while x < 100 {
    x += 1;
}
```

### for with range and optional step (v4)

```rust
// Exclusive range
for i in 0..80 { }

// Inclusive range
for i in 0..=10 { }

// Stepped range (v4)
for i in 0..100 step 2 { }

// Negative step
for i in 100..0 step -1 { }
```

### loop / break

```rust
// Infinite loop — must have a reachable break or W202 is emitted
loop {
    if check_interrupt() == 1 {
        break;
    }
}

// Named break with value (v4) — the break expression becomes the result
let result = loop {
    if done() { break 42; }
};
```

### match

Supports integer literals, string literals, and a wildcard arm. String matching compiles to a `StrCmp` chain.

```rust
match status {
    0 => { print(0); },
    1 => { setup_network(); },
    "ok" => { print(1); },
    _ => { print(2); }   // catch-all; strongly recommended
}
```

### Compound Assignment Operators

`+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`

### continue

```rust
for i in 0..10 {
    if i == 5 { continue; }
    print(i);
}
```

---

## 4. Types & Casting

Helios has four primitive types: `int`, `str`, `bool`, `ptr`. The `void` return type is used for functions with no return value.

```rust
// Cast between int and bool
let flag = (port_val as bool);
let raw  = (true as int);
```

Basic type mismatch (int used where str is expected) raises E011.

---

## 5. Memory & Hardware Interfaces

All direct hardware access and raw memory operations must be wrapped in an `unsafe` block. An `unsafe` block with no actual unsafe operations emits W603. Using `asm!()` outside `unsafe` emits W604.

### Pointers

```rust
let addr: int = &my_var;     // address-of
let val: int  = *ptr;        // dereference
let casted    = ptr as int;  // cast to integer
```

### peek / poke — Physical Memory Access

```rust
unsafe {
    let val8  = peek(addr, 8);    // read  8-bit
    let val16 = peek(addr, 16);   // read 16-bit
    let val32 = peek(addr, 32);   // read 32-bit
    poke(addr, value, 8);         // write  8-bit
    poke(addr, value, 16);        // write 16-bit
    poke(addr, value, 32);        // write 32-bit
}
```

### malloc / free

```rust
let ptr = malloc(256);    // allocate 256 bytes; returns ptr
// ... use ptr ...
free(ptr);
```

### memcpy / memset

```rust
memcpy(dest, src, len);
memset(dest, value, len);
```

### Inline Assembly

All `asm!()` calls must be inside an `unsafe` block.

```rust
unsafe {
    asm!("cli");              // disable interrupts
    asm!("sti");              // enable interrupts
    asm!("nop");              // no-op
    asm!("int 0x80");         // software interrupt (vector as literal)
    asm!("hlt");              // halt CPU (v6)
    asm!("pushfd");           // push EFLAGS → VM stack (v6)
    asm!("popfd");            // pop VM stack → EFLAGS (v6)
    asm!("lidt");             // load IDT from ptr on VM stack (v6)
    asm!("lgdt");             // load GDT from ptr on VM stack (v6)
    asm!("wbinvd");           // write-back + invalidate caches (v6)
    asm!("invlpg");           // invalidate TLB for addr on VM stack (v6)
    asm!("mov_cr2_eax");      // read CR2 (page-fault address) → VM stack (v6)
    asm!("clts");             // clear CR0.TS task-switched flag (v6)
    asm!("xchg_eax_ebx");     // atomic EAX↔EBX exchange via lock prefix (v6)
}
```

### Hardware I/O Ports

```rust
unsafe {
    let v8  = in8(port);          // read  8-bit from I/O port
    out8(port, value);             // write  8-bit
    let v16 = in16(port);          // read 16-bit
    out16(port, value);            // write 16-bit
    let v32 = in32(port);          // read 32-bit
    out32(port, value);            // write 32-bit

    // Bulk port I/O
    port_burst_read(port, buf, count);    // rep insb
    port_burst_write(port, buf, count);   // rep outsb
}
```

### Segment Registers

```rust
unsafe {
    let cs = get_cs();
    let ds = get_ds();
    let es = get_es();
    let fs = get_fs();
    let gs = get_gs();
    let ss = get_ss();
    set_fs(value);
    set_gs(value);
}
```

### Control Registers

```rust
unsafe {
    let cr0 = get_cr0();
    set_cr0(value);
    let cr2 = get_cr2();      // page-fault address (v6 via asm!("mov_cr2_eax"))
    let cr3 = get_cr3();
    set_cr3(value);
}
```

### System Descriptors

```rust
unsafe {
    let gdt_base = sgdt();   // store GDT → returns base address
    let idt_base = sidt();   // store IDT
    // load via asm!("lgdt") / asm!("lidt") with ptr on stack
}
```

### MSR Access

```rust
unsafe {
    let lo  = rdmsr(msr_id);          // read MSR low 32 bits
    wrmsr(msr_id, value);              // write MSR low 32 bits
    let val64 = rdmsr64(msr_id);       // read full 64-bit MSR (v4)
    wrmsr64(msr_id, val64);            // write full 64-bit MSR (v4)
    let tscp  = rdtscp();              // serialized TSC (v4)
    iopl3();                           // elevate to IOPL 3 (v4)
}
```

### FPU / SSE State

```rust
unsafe {
    fxsave(buf_ptr);     // save FPU/SSE state to 512-byte aligned buffer
    fxrstor(buf_ptr);    // restore FPU/SSE state
}
```

### PIC / PIT / Timer

```rust
unsafe {
    pic_eoi(irq);           // send end-of-interrupt to PIC
    pic_mask(irq);          // mask IRQ line
    pic_unmask(irq);        // unmask IRQ line
    pit_set_hz(frequency);  // reprogram PIT channel 0

    let ticks = get_ticks();  // system tick counter (kernel uptime)
    sleep_ms(100);            // sleep for N milliseconds
}
```

### CPUID / TSC

```rust
let eax_leaf0 = cpuid(0);    // query CPUID leaf; returns EAX
let tsc = rdtsc();           // read timestamp counter
```

### Hardware Memory Mapping (v6)

```rust
unsafe {
    let ok = hw_map_mem(phys_base, length);  // identity-map physical range; 0 = ok, -1 = error
}
```

### Memory Query

```rust
let total = mem_query();   // query total available memory from kernel
```

### Page Allocation

```rust
unsafe {
    let page = alloc_page();   // allocate one 4 KB page; returns address
    free_page(page);
}
```

### Debug Break

```rust
unsafe { debug_break(); }   // emit int3 breakpoint
```

### Example: VGA Text Buffer Write

```rust
fn draw_vga_char(x: int, y: int, char: int, color: int) {
    unsafe {
        let vga_base = 0xB8000;
        let offset   = ((y * 80) + x) * 2;
        let payload  = (color << 8) | char;
        poke(vga_base + offset, payload, 16);
    }
}
```

---

## 6. String Operations

These are built-in functions available without any import.

```rust
let n   = strlen(s);           // byte length of string
let eq  = strcmp(a, b);        // 0 if equal, non-zero otherwise
let cat = strcat(a, b);        // concatenate two strings → new heap string
let sub = strsub(s, start, len); // substring copy
let idx = strfind(s, needle);  // index of first occurrence, -1 if not found
let i   = strtoint(s);         // parse string as integer
let s2  = inttostr(n);         // format integer as string (kernel scratch buffer)
let up  = strupper(s);         // in-place uppercase copy (v5)
let lo  = strlower(s);         // in-place lowercase copy (v5)
let b   = strbyte(s);          // first byte of string as integer (v5)
print_str(ptr);                 // print heap-pointer string without tag-bit check (v5)
```

---

## 7. Assertions (v5)

```rust
assert!(condition);
assert!(condition, "custom failure message");
```

Pops the condition from the stack. If it is zero, execution halts with E012 and the optional message is printed. Compiles to opcode `0xD0`.

---

## 8. PRP Imports

Import the complete PRP surface with `using::prp::*;` or name individual functions. Calling a PRP function without an import raises E014.

```rust
using::prp::keygen;
using::prp::sha256_file;
// or
using::prp::*;
```

Available PRP functions:

| Function | Description |
| :--- | :--- |
| `keygen(path)` | Generate a keypair; writes files at `path` |
| `fingerprint(keyfile, out_buf)` | Compute key fingerprint into `out_buf` |
| `sha256_file(filename, out_buf)` | SHA-256 of a file into 32-byte `out_buf` |
| `seal_file(input, output, key_hex)` | Symmetric encryption (shared key) |
| `open_file(input, output, key_hex)` | Symmetric decryption |
| `seal_text(text, len, key_hex, output)` | Encrypt a text buffer |
| `seal_file_pub(input, output, recipient_pub)` | Encrypt file to recipient public key |
| `open_file_prv(input, output, private_key)` | Decrypt file with private key |
| `seal_text_pub(text, len, recipient_pub, output)` | Encrypt text buffer to public key |
| `sign(file, private_key)` | Sign a file |
| `verify(file, expected_pub)` | Verify a file's signature against public key |
| `selftest()` | Run PRP internal self-test; returns 0 on pass |
| `random_bytes(output, len)` | Fill buffer with cryptographic random bytes |

```rust
using::prp::keygen;
using::prp::sha256_file;

fn main() {
    let result = keygen("/tmp/demo-key");
    print(result);
    let digest = malloc(32);
    let hash_status = sha256_file("/README.txt", digest);
    print(hash_status);
    free(digest);
}
```

---

## 9. Fetch / HTTPS Imports

```rust
using::fetch::get;
using::fetch::active;
// or
using::fetch::*;
```

Available fetch functions:

| Function | Description |
| :--- | :--- |
| `https_get(url)` | Perform an HTTPS GET request via PSCA proxy; returns status |
| `fetch_active()` | Returns 1 if a fetch is currently in progress |
| `test_https()` | Run HTTPS connectivity self-test; returns 0 on success |

---

## 10. Keyboard / Input

```rust
let line = input();           // read a line from keyboard; returns str
print_str(line);              // display the returned string

// During Helios keyboard capture, the shell poller yields port 0x60 to the VM
// and resumes after execution ends.

let sc = poll_scancode();     // returns next scancode or -1 if none available
```

---

## 11. File System

```rust
let exists = file_exists("/path/to/file");  // 1 if exists, 0 if not
let size   = file_size("/path/to/file");    // file size in bytes (0 if missing or empty)
let n      = file_read("/path", buf, len, offset);  // read bytes into buffer
let n      = file_write("/path", buf, len, offset); // write bytes from buffer
```

---

## 12. File Inclusion (v6)

```rust
include "utils.hls"
include "drivers/rtl8139.hls"
```

`include` is processed at compile time. All `struct`, `const`, and `fn` definitions from the included file are merged before compilation begins. Duplicate definitions across included files are deduplicated via include-guard logic. Use `helios deps <file.hls>` to list all `include` dependencies of a source file.

---

## 13. Recommended Practices

1. **Isolate `unsafe` blocks.** Keep `unsafe` scope as small as possible. Wrap raw `peek`/`poke` calls and `asm!` instructions inside dedicated safe helper functions, such as port or register drivers.

2. **Always break out of `loop`.** A bare `loop {}` without a reachable `break` will completely halt the executing thread on bare metal. The compiler emits W202 to catch this, but relying on the warning is not a substitute for correct logic.

3. **Handle discarded return values.** If a function returns a value but you only need its side effects, assign the result to a throwaway variable or cast it. Unreceived return values accumulate on the 4 MB value stack and will eventually exhaust it.

4. **Always include a catch-all `match` arm.** Hardware signals, IRQ status, and register flags can yield unexpected integer values. An exhaustive `_ =>` arm prevents unhandled branching exceptions.

5. **Order struct fields largest → smallest.** When defining structs that map to hardware registers or network packet headers, declaring wider fields first keeps memory offsets predictable and avoids implicit padding.

6. **Use `const` for hardware addresses.** Replace magic numbers like `0xB8000` with top-level `const` definitions so addresses are named and changeable in one place.

7. **Group parameters into structs.** Functions with more than 16 parameters emit W602. Pass related data as a struct instead.

8. **Prefer `for` over manual `while` counters.** The `for i in a..b step N` form is clearer, and the compiler can validate range bounds more easily.

---

## 14. Diagnostics & Error Code Reference

Errors (`E`) halt compilation. Warnings (`W`) flag structural issues. The diagnostic engine renders VGA-coloured multi-span output with source snippets, column carets, help lines, and notes. Duplicate-definition errors include a secondary span pointing to the original definition site.

### Errors

| Code | Description | Remediation |
| :--- | :--- | :--- |
| **E001** | Missing `fn` or `struct` definition | Ensure all top-level code is inside a function or struct |
| **E002** | Missing `main` entry point | Define `fn main() -> void {}` |
| **E003** | Duplicate function name *(v4)* | Rename one of the conflicting functions |
| **E004** | Duplicate struct name *(v4)* | Rename one of the conflicting structs |
| **E005** | Unexpected token | Check for a missing delimiter or semicolon |
| **E006** | Missing semicolon `;` | Terminate the flagged statement with `;` |
| **E007** | Unexpected token in expression | Valid starts: literal, identifier, `(`, `[`, `*`, `&`, `!`, `~` |
| **E008** | Undefined variable *(v4)* | Declare the variable with `let` before use |
| **E009** | Undefined function call *(v4)* | Check the function name or add its definition |
| **E010** | Expected identifier | Supply a valid identifier in the flagged position |
| **E011** | Mismatched types *(v4)* | Use `int` where `int` is expected; `str` where `str` is expected |
| **E012** | `assert!()` failure *(v5, runtime)* | The asserted condition evaluated to zero at runtime |
| **E013** | `const` redefinition *(v5)* | Each constant name may only be defined once |
| **E014** | Unknown or missing PRP / fetch import | Import the function with `using::prp::<name>` or `using::fetch::<name>` before calling it |
| **E050** | `break` outside a loop | Move `break` inside a `for`, `while`, or `loop` block |
| **E051** | `continue` outside a loop | Move `continue` inside a loop block |

### Warnings

| Code | Description | Remediation |
| :--- | :--- | :--- |
| **W101** | Discarded expression result | Assign to a variable or remove the expression |
| **W201** | Constant condition in `if` | Use a dynamic variable instead of a literal |
| **W202** | Infinite `loop` with no reachable `break` | Add conditional `break` logic, or use `while` |
| **W301** | Block nesting depth ≥ 5 | Extract inner logic into helper functions |
| **W302** | Unreachable code after `return` | Remove dead code or move it before the `return` |
| **W303** | Empty block `{}` | Add code or a comment: `// intentionally empty` |
| **W401** | Missing `return` on some code path *(improved in v4)* | Ensure every path returns the declared type |
| **W601** | Array literal with > 256 elements *(v4)* | Allocate in smaller chunks to reduce heap pressure |
| **W602** | Function with > 16 parameters *(v4)* | Group related parameters into a struct |
| **W603** | `unsafe` block with no unsafe operations *(v5)* | Remove the `unsafe` wrapper or add hardware/asm ops |
| **W604** | `asm!()` used outside `unsafe` block *(v5)* | Wrap the `asm!()` call in `unsafe { }` |

---

## 15. Shell Commands

All commands are invoked as `helios <command> [args]`.

### Compile & Run

| Command | Description |
| :--- | :--- |
| `helios run <file.hls>` | Compile and run a source file |
| `helios run -stack <KB> <file.hls>` | Run with a custom VM stack size (v5) |
| `helios -v run <file.hls>` | Verbose: show step count |
| `helios -vv run <file.hls>` | Extra-verbose: show heap bytes allocated / freed (v4) |
| `helios -vvv run <file.hls>` | Ultra-trace: every push/pop with value; asm op count at exit (v5/v6) |

### Multi-File Build (v6)

```
helios build file1.hls file2.hls ... -o out.rxe
```

Reads all source files, concatenates them with include-guard deduplication, adds synthetic comment boundaries for error-span accuracy, then compiles to a single RXE binary. If `-o` is omitted, the output name is derived from the first source file.

### Inspect & Debug

| Command | Description |
| :--- | :--- |
| `helios tokens <file.hls>` | Dump the token stream from the lexer |
| `helios disasm <file.rxe>` | Pretty-print RXE bytecode (v4) |
| `helios info   <file.rxe>` | Print RXE header fields (v4) |
| `helios symbols <file.rxe>` | Dump the symbol table if present; compile with `-g` to include one (v5) |
| `helios cat    <file.hls>` | Dump source with line numbers (v6) |
| `helios deps   <file.hls>` | List all `include` dependencies (v6) |

### Optimize & Clean

| Command | Description |
| :--- | :--- |
| `helios strip <file.rxe>` | Strip the debug symbol section; writes `<name>_stripped.rxe` (v5) |
| `helios fmt   <file.hls>` | Pretty-print AST as reformatted source (v4) |

### Benchmark & Test

| Command | Description |
| :--- | :--- |
| `helios bench <file.hls>` | Compile and run 5×, report mean step count (v4) |
| `helios test all` | Run the full Helios test suite |

### One-Shot Constants & Info

| Command | Description |
| :--- | :--- |
| `helios const <name> <val>` | Define a one-shot compile constant (v5) |
| `helios version` | Print detailed build info: RXE version, stack size, opcode range, features (v5) |

### Built-in Demos (v6)

```
helios demo <name>
```

| Demo | Description |
| :--- | :--- |
| `hw_port` | Blink keyboard LED via ports `0x60` / `0x64` |
| `pit_tick` | Reprogram PIT channel 0 and count ticks |
| `mem_scan` | Walk physical memory via `peek32` |
| `asm_chain` | Showcase every new v6 inline asm instruction |
| `cpuid_info` | Print CPU vendor string and feature flags via `cpuid` |

---

## 16. Opcode Reference

Below is the full opcode table for the RXE v6 bytecode format, grouped by category.

### Stack

| Opcode | Byte | Description |
| :--- | :--- | :--- |
| push | 0x01 | Push a 32-bit immediate integer |
| push_str | 0x02 | Push a const-pool string reference |
| pop | 0x03 | Discard top of stack |
| dup | 0x04 | Duplicate top of stack |
| swap | 0x05 | Swap top two values |
| over | 0x06 | Copy second value to top |

### Arithmetic

| Opcode | Byte |
| :--- | :--- |
| add | 0x10 |
| sub | 0x11 |
| mul | 0x12 |
| div | 0x13 |
| mod | 0x14 |
| neg | 0x15 |

### Bitwise

| Opcode | Byte |
| :--- | :--- |
| band | 0x16 |
| bor | 0x17 |
| bxor | 0x18 |
| bnot | 0x19 |
| shl | 0x1A |
| shr | 0x1B |

### Comparison & Logical

| Opcode | Byte |
| :--- | :--- |
| cmpeq | 0x20 |
| cmpne | 0x21 |
| cmplt | 0x22 |
| cmple | 0x23 |
| cmpgt | 0x24 |
| cmpge | 0x25 |
| and | 0x28 |
| or | 0x29 |
| not | 0x2A |

### Control Flow

| Opcode | Byte | Notes |
| :--- | :--- | :--- |
| jmp | 0x30 | Unconditional jump; 4-byte target |
| jz | 0x31 | Jump if top of stack is zero |
| jnz | 0x32 | Jump if top of stack is non-zero |
| call | 0x33 | Call function at address; 4-byte target |
| ret | 0x34 | Return from function |
| halt | 0x35 | Halt VM |

### Locals & Registers

| Opcode | Byte |
| :--- | :--- |
| load | 0x40 |
| store | 0x41 |
| lreg | 0x42 |
| sreg | 0x43 |

### Heap & Arrays

| Opcode | Byte |
| :--- | :--- |
| halloc | 0x44 |
| hfree | 0x45 |
| aload | 0x46 |
| astore | 0x47 |

### I/O & File System

| Opcode | Byte |
| :--- | :--- |
| print | 0x50 |
| printnum | 0x51 |
| printc | 0x52 |
| input | 0x53 |
| printhex | 0x54 |
| fexist | 0x62 |
| fread | 0x63 |
| fwrite | 0x64 |
| fsize | 0x65 |

### Hardware — I/O Ports

| Opcode | Byte |
| :--- | :--- |
| in8 | 0x70 |
| out8 | 0x71 |
| in16 | 0x72 |
| out16 | 0x73 |
| in32 | 0x74 |
| out32 | 0x75 |
| cli | 0x76 |
| sti | 0x77 |
| hlt | 0x78 |
| rdtsc | 0x79 |
| memcpy | 0x7A |
| memset | 0x7B |
| palloc | 0x7C |
| pfree | 0x7D |

### Hardware — Control Registers & Physical Memory

| Opcode | Byte |
| :--- | :--- |
| gcr0 | 0x7E |
| scr0 | 0x7F |
| gcr3 | 0x80 |
| scr3 | 0x81 |
| int3 | 0x82 |
| peek8 | 0x83 |
| poke8 | 0x84 |
| peek16 | 0x85 |
| poke16 | 0x86 |
| peek32 | 0x87 |
| poke32 | 0x88 |

### Hardware — Segment Registers & Extended Syscalls

| Opcode | Byte |
| :--- | :--- |
| gcs | 0x89 |
| gds | 0x8A |
| ges | 0x8B |
| gfs | 0x8C |
| ggs | 0x8D |
| gss | 0x8E |
| sfs | 0x8F |
| sgs | 0x90 |
| pic_eoi | 0x91 |
| pic_mask | 0x92 |
| pic_unmask | 0x93 |
| pit_hz | 0x94 |
| ticks | 0x95 |
| sleep | 0x96 |
| cpuid | 0x97 |
| rdmsr | 0x98 |
| wrmsr | 0x99 |
| insb | 0x9A |
| outsb | 0x9B |
| memqry | 0x9C |

### Extended Hardware (v4)

| Opcode | Byte |
| :--- | :--- |
| rdmsr64 | 0xA0 |
| wrmsr64 | 0xA1 |
| sgdt | 0xA2 |
| sidt | 0xA3 |
| rdtscp | 0xA4 |
| iopl3 | 0xA5 |
| anop | 0xA6 |
| acli | 0xA7 |
| asti | 0xA8 |
| aint | 0xA9 |
| fxsave | 0xAA |
| fxrstor | 0xAB |

### String Operations (v4 / v5)

| Opcode | Byte | Notes |
| :--- | :--- | :--- |
| strlen | 0xB0 | |
| strcmp | 0xB1 | |
| strcat | 0xB2 | heap scratch buffer |
| strsub | 0xB3 | |
| strfind | 0xB4 | |
| itos | 0xB5 | int → string (kernel scratch) |
| stoi | 0xB6 | string → int |
| strupr | 0xB7 | in-place uppercase copy (v5) |
| strlwr | 0xB8 | in-place lowercase copy (v5) |
| printstr | 0xB9 | print heap-ptr string without tag-bit check (v5) |
| strbyte | 0xBA | first byte of string as integer (v5) |

### Casting

| Opcode | Byte |
| :--- | :--- |
| cint | 0xC0 |
| cbool | 0xC1 |

### Assert & PRP (v5)

| Opcode | Byte | Notes |
| :--- | :--- | :--- |
| assert | 0xD0 | pop cond; halt with E012 if 0 |
| prp_selftest | 0xD1 | |
| prp_random | 0xD2 | |
| prp_sha256_file | 0xD3 | |
| prp_seal_file | 0xD4 | |
| prp_open_file | 0xD5 | |
| prp_seal_text | 0xD6 | |
| prp_fingerprint | 0xD7 | |
| prp_keygen | 0xD8 | |
| prp_sign | 0xD9 | |
| prp_verify | 0xDA | |
| prp_seal_file_pub | 0xDB | |
| prp_open_file_prv | 0xDC | |
| prp_seal_text_pub | 0xDD | |

### Expanded Inline ASM (v6)

| Opcode | Byte | Description |
| :--- | :--- | :--- |
| asm_hlt | 0xE0 | emit `hlt` |
| asm_pushfd | 0xE1 | push EFLAGS → VM stack |
| asm_popfd | 0xE2 | pop VM stack → EFLAGS |
| asm_lidt | 0xE3 | `lidt` from ptr on VM stack |
| asm_lgdt | 0xE4 | `lgdt` from ptr on VM stack |
| asm_wbinvd | 0xE5 | write-back + invalidate caches |
| asm_invlpg | 0xE6 | invalidate TLB for address on VM stack |
| get_cr2 | 0xE7 | read CR2 → VM stack |
| asm_clts | 0xE8 | clear CR0.TS |
| hw_map_mem | 0xE9 | identity-map physical range (base, len) → 0 ok / -1 err |
| poll_scancode | 0xEA | return next scancode or -1 |

### Fetch / HTTPS

| Opcode | Byte |
| :--- | :--- |
| fetch_get | 0xED |
| fetch_active | 0xEE |
| fetch_test | 0xEF |

### Misc

| Opcode | Byte |
| :--- | :--- |
| nop | 0xFF |