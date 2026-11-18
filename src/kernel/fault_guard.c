// fault_guard.c - see fault_guard.h

#include "fault_guard.h"
#include "../scheduler/task.h"

extern volatile uint32_t ticks;   // 1 kHz PIT counter (timer.c)

static FgJmp *volatile g_jb;      // armed resume point, 0 when disarmed
static volatile uint32_t g_limit_ms;
static volatile uint32_t g_last_pet;
static volatile int g_paused;
// fg_crit_enter depth: the program is inside a kernel call (since g_crit_t0).
static volatile int g_crit;
static uint32_t g_crit_t0;
// IRQ nesting depth. The guarded program runs at depth 0; anything at a
// deeper level is an interrupt handler or a scheduler task running on top of
// it, and its faults are not ours to unwind.
static volatile int g_irq_depth;
static volatile int g_armed_depth;
static FgReport g_report;

// fg_setjmp: save the callee-saved registers, the caller's stack pointer and
// the return address. Resuming reloads them and "returns" 1.
__attribute__((naked, returns_twice)) int fg_setjmp(FgJmp *jb) {
    __asm__ volatile(
        "movl 4(%esp), %edx\n\t"
        "movl %ebx, 0(%edx)\n\t"
        "movl %esi, 4(%edx)\n\t"
        "movl %edi, 8(%edx)\n\t"
        "movl %ebp, 12(%edx)\n\t"
        "leal 4(%esp), %ecx\n\t"
        "movl %ecx, 16(%edx)\n\t"
        "movl (%esp), %ecx\n\t"
        "movl %ecx, 20(%edx)\n\t"
        "xorl %eax, %eax\n\t"
        "ret\n\t");
}

// Where the rewritten iret lands, with EAX = the FgJmp. The faulting stack
// may be in any state, so nothing is pushed before ESP is replaced. The
// x87 unit is reset (and set back to double precision) because the fault
// may have left its register stack half-used. Interrupts come back on just before the jump (the frame we
// iret'd from could have had them off).
__attribute__((naked)) static void fg_resume(void) {
    __asm__ volatile(
        "cld\n\t"
        "fninit\n\t"
        "movl %eax, %edx\n\t"
        "movl 0(%edx), %ebx\n\t"
        "movl 4(%edx), %esi\n\t"
        "movl 8(%edx), %edi\n\t"
        "movl 12(%edx), %ebp\n\t"
        "movl 16(%edx), %esp\n\t"
        "pushl $0x027F\n\t"          /* double precision, as set at boot */
        "fldcw (%esp)\n\t"
        "addl $4, %esp\n\t"
        "movl $1, %eax\n\t"
        "sti\n\t"
        "jmp *20(%edx)\n\t");
}

void fg_arm(FgJmp *jb, uint32_t limit_ms) {
    g_limit_ms = limit_ms;
    g_last_pet = ticks;
    g_paused = 0;
    g_crit = 0;
    g_armed_depth = g_irq_depth;
    g_jb = jb;
}

void fg_disarm(void) {
    g_jb = 0;
}

void fg_pet(void) {
    g_last_pet = ticks;
}

void fg_watchdog_pause(void) {
    g_paused++;
}

void fg_watchdog_resume(void) {
    if (g_paused > 0) g_paused--;
    g_last_pet = ticks;
}

void fg_crit_enter(void) {
    if (g_crit++ == 0) g_crit_t0 = ticks;
}

// The outermost exit gives the watchdog back the time spent inside: the
// last pet moves forward by it, never past now. With interrupts off, since
// a paused watchdog's tick rewrites g_last_pet too.
void fg_crit_exit(void) {
    if (g_crit <= 0) return;
    if (--g_crit > 0) return;
    uint32_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    uint32_t now = ticks;
    uint32_t inside = now - g_crit_t0;
    if (inside >= now - g_last_pet) g_last_pet = now;
    else g_last_pet += inside;
    if (flags & 0x200) __asm__ volatile("sti" : : : "memory");
}

const FgReport *fg_last_report(void) {
    return &g_report;
}

void fg_irq_enter(void) {
    g_irq_depth++;
}

void fg_irq_exit(void) {
    if (g_irq_depth > 0) g_irq_depth--;
}

// Redirect the interrupted context to fg_resume. The guard disarms itself so
// a second fault before the program re-arms takes the normal path.
static int redirect(TrapFrame *regs) {
    FgJmp *jb = g_jb;
    if (!jb) return 0;
    g_jb = 0;
    g_paused = 0;
    regs->eax = (uint32_t)jb;
    regs->eip = (uint32_t)&fg_resume;
    regs->eflags &= ~(1u << 10);   // DF
    return 1;
}

int fg_on_exception(void *trap_frame) {
    TrapFrame *regs = (TrapFrame *)trap_frame;
    // Only faults the guarded program itself raised (not from a handler or
    // task running on top of it), and only kernel-mode code.
    if (!g_jb || g_irq_depth != g_armed_depth || (regs->cs & 3) != 0) return 0;
    // NMI and debug/breakpoint traps are not the program's errors.
    if (regs->interrupt == 1 || regs->interrupt == 2 || regs->interrupt == 3) return 0;
    // Raised inside a kernel call (fg_crit_enter): kernel code, kernel state.
    if (g_crit) return 0;
    g_report.kind = FG_KIND_FAULT;
    g_report.vector = regs->interrupt;
    g_report.error = regs->error;
    g_report.eip = regs->eip;
    g_report.ms = 0;
    return redirect(regs);
}

int fg_on_timer(void *trap_frame) {
    TrapFrame *regs = (TrapFrame *)trap_frame;
    // The timer must have interrupted the guarded program directly; a tick
    // that lands inside another handler or task leaves the program alone.
    if (!g_jb || !g_limit_ms || (regs->cs & 3) != 0) return 0;
    if (g_irq_depth != g_armed_depth + 1) return 0;
    uint32_t now = ticks;
    if (g_paused) {
        g_last_pet = now;
        return 0;
    }
    uint32_t stuck = now - g_last_pet;
    if (stuck < g_limit_ms) return 0;
    // Inside a kernel call: let it finish (fg_crit_exit discounts its time).
    if (g_crit) return 0;
    g_report.kind = FG_KIND_HANG;
    g_report.vector = 0;
    g_report.error = 0;
    g_report.eip = regs->eip;
    g_report.ms = stuck;
    return redirect(regs);
}

static const char *const g_vec_names[32] = {
    "divide error", "debug", "NMI", "breakpoint", "overflow", "bound range exceeded",
    "invalid opcode", "device not available", "double fault", "coprocessor overrun",
    "invalid TSS", "segment not present", "stack fault", "general protection fault",
    "page fault", "reserved", "x87 floating-point error", "alignment check",
    "machine check", "SIMD floating-point error", "virtualization", "control protection",
    "reserved", "reserved", "reserved", "reserved", "reserved", "reserved",
    "reserved", "reserved", "security", "reserved",
};

static void put_str(char *out, int cap, int *n, const char *s) {
    while (*s && *n + 1 < cap) out[(*n)++] = *s++;
    out[*n] = 0;
}

static void put_hex(char *out, int cap, int *n, uint32_t v) {
    static const char hex[] = "0123456789ABCDEF";
    put_str(out, cap, n, "0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        if (*n + 1 >= cap) break;
        out[(*n)++] = hex[(v >> shift) & 15];
    }
    out[*n] = 0;
}

static void put_dec(char *out, int cap, int *n, uint32_t v) {
    char tmp[12];
    int k = 0;
    do {
        tmp[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && k < 11);
    while (k > 0 && *n + 1 < cap) out[(*n)++] = tmp[--k];
    out[*n] = 0;
}

void fg_describe(char *out, int cap) {
    int n = 0;
    if (cap <= 0) return;
    out[0] = 0;
    if (g_report.kind == FG_KIND_HANG) {
        put_str(out, cap, &n, "Watchdog: stuck for ");
        put_dec(out, cap, &n, g_report.ms / 1000);
        put_str(out, cap, &n, " s at ");
        put_hex(out, cap, &n, g_report.eip);
        put_str(out, cap, &n, ".");
        return;
    }
    put_str(out, cap, &n, "CPU exception ");
    put_dec(out, cap, &n, g_report.vector);
    put_str(out, cap, &n, " (");
    put_str(out, cap, &n, g_report.vector < 32 ? g_vec_names[g_report.vector] : "unknown");
    put_str(out, cap, &n, ") at ");
    put_hex(out, cap, &n, g_report.eip);
    if (g_report.error) {
        put_str(out, cap, &n, ", error ");
        put_hex(out, cap, &n, g_report.error);
    }
    put_str(out, cap, &n, ".");
}
