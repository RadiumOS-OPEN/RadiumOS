// fault_guard.h - recover from CPU faults and hangs inside one guarded region
//
// A program (the browser) arms a guard with a resume point made by
// fg_setjmp. While armed, a CPU exception raised by that program, or the
// program running fg_limit_ms without calling fg_pet, does not hang the
// machine: the interrupt handler rewrites the return frame so the iret lands
// in a trampoline that restores the resume point, and fg_setjmp returns a
// second time with a nonzero value. Faults raised from inside an interrupt
// handler (scheduler tasks, drivers) are never redirected: their stacks and
// locks do not belong to the guarded program.
//
// Only state the guarded program owns can be trusted afterwards; it must
// reset its own globals (the browser re-opens its engine from scratch).
// Kernel state is not the program's: while it calls into the kernel (AVFS,
// the FAT32/ATA driver, the network stack) it brackets the call with
// fg_crit_enter/fg_crit_exit, so a redirect never abandons a kernel routine
// half way with its drive command, sector cache or connection table torn.
// All 4 GiB of virtual space is mapped (boot.asm, NULL included), so stray
// pointer writes do not fault: the guard catches #DE/#UD/#GP/#SS and
// friends, and hangs via the timer, not wild stores.

#ifndef FAULT_GUARD_H
#define FAULT_GUARD_H

#include <stdint.h>

typedef struct {
    uint32_t ebx, esi, edi, ebp, esp, eip;
} FgJmp;

#define FG_KIND_FAULT 1
#define FG_KIND_HANG  2

typedef struct {
    int kind;           // FG_KIND_*
    uint32_t vector;    // CPU exception vector (FG_KIND_FAULT)
    uint32_t error;     // error code pushed by the CPU, 0 if none
    uint32_t eip;       // where it happened
    uint32_t ms;        // FG_KIND_HANG: how long the program had been stuck
} FgReport;

// Returns 0 when called, nonzero when resumed after a fault or hang.
__attribute__((returns_twice)) int fg_setjmp(FgJmp *jb);

// Arm the guard. limit_ms == 0 disables the hang watchdog.
void fg_arm(FgJmp *jb, uint32_t limit_ms);
void fg_disarm(void);
// "Still making progress": restart the hang watchdog.
void fg_pet(void);
// Pause or resume the hang watchdog for long waits the program expects
// (network transfers). Calls nest.
void fg_watchdog_pause(void);
void fg_watchdog_resume(void);
// Critical section around a kernel call made with the guard armed. Calls
// nest. Inside one:
//  - the watchdog never redirects, and the time spent inside does not count
//    against the limit (a slow disk read is not the program hanging), so a
//    program that loops on its own code, kernel calls or not, is still
//    caught once it has used up the limit outside them; a kernel call that
//    never returns is not recoverable;
//  - a CPU exception is not redirected either: it was raised by kernel code
//    whose locks and state the program does not own, so it takes the
//    kernel's normal exception path, as any other kernel fault does.
// Spinlocks (src/network/spinlock.c) need nothing: they run with interrupts
// off, so the timer never sees one held.
void fg_crit_enter(void);
void fg_crit_exit(void);

// Details of the last recovery.
const FgReport *fg_last_report(void);
// Human-readable one-liner for the last recovery.
void fg_describe(char *out, int cap);

// Interrupt-path hooks, called from handle_interrupt.
void fg_irq_enter(void);
void fg_irq_exit(void);
// Exception vectors 0-31: returns 1 if the frame was redirected to the
// resume point (the handler must then return straight to the stub).
int fg_on_exception(void *trap_frame);
// Timer tick, before scheduling: returns 1 if the frame was redirected.
int fg_on_timer(void *trap_frame);

#endif
