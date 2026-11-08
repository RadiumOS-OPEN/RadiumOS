// stackguard.c - the stack canary behind -fstack-protector-strong (makefile CFLAGS)
//
// The compiler stores __stack_chk_guard below the return address of every function with an array or an address-taken
// local and compares it before returning; a buffer overflow that reaches the return address changes it and ends in
// __stack_chk_fail instead of a hijacked return. The value starts as a terminator canary (NUL, CR, LF, 0xFF stop
// string-based overflows) and is replaced by a random one at boot.

#include <stdint.h>
#include "stackguard.h"
#include "../io/io.h"
#include "../cpu/cpu.h"
#include "../terminal/terminal.h"

uint32_t __stack_chk_guard = 0x000A0DFFu;

static uint32_t g_boot_guard;

__attribute__((no_stack_protector)) void stackguard_init(void)
{
    uint32_t r = 0, lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    r = lo ^ (hi << 7) ^ 0xA5C3E1B7u;
    uint32_t hw = 0;
    if (cpu_rdrand32(&hw)) r ^= hw;
    r ^= r << 13; r ^= r >> 17; r ^= r << 5;
    r &= 0xFFFFFF00u;                      // keep a NUL byte: string functions cannot copy it past the canary
    if (r == 0) r = 0x5CA1AB00u;
    g_boot_guard = r;
    __stack_chk_guard = r;
}

uint32_t stackguard_value(void) { return __stack_chk_guard; }

__attribute__((noreturn, no_stack_protector)) void __stack_chk_fail(void)
{
    static const char msg[] = "\n*** stack smashing detected - kernel halted ***\n";
    for (const char *p = msg; *p; p++) { outb(0xE9, (uint8_t)*p); outb(0x3F8, (uint8_t)*p); }
    terminal_setcolor(VGA_COLOR_LIGHT_RED);
    print(msg);
    for (;;) __asm__ volatile("cli; hlt");
}
