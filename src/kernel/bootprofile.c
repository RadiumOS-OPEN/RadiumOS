// bootprofile.c - boot milestones on the QEMU debug console (port 0xE9)
//
// boot_mark("label") writes "[boot] label @<ms>" to the isa-debugcon device, which the Makefile
// sends to debug.log. Real hardware ignores writes to that port. Use it to see where boot time goes:
//   grep '\[boot\]' debug.log

#include <stdint.h>
#include "bootprofile.h"
#include "../io/io.h"
#include "../timers/timer.h"
#include "../vga/fbcon.h"

static void debugcon_puts(const char *s)
{
    while (*s) outb(0xE9, (uint8_t)*s++);
}

static void debugcon_uint(uint32_t v)
{
    char digits[11];
    int n = 0;
    do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n > 0) outb(0xE9, (uint8_t)digits[--n]);
}

void boot_mark(const char *label)
{
    fbcon_poll();
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    uint32_t mcycles = (hi << 12) | (lo >> 20);   // TSC / 2^20, available before the timer runs

    debugcon_puts("[boot] ");
    debugcon_puts(label);
    debugcon_puts(" ticks=");
    debugcon_uint(get_time_ms());
    debugcon_puts(" mcycles=");
    debugcon_uint(mcycles);
    debugcon_puts("\n");
}
