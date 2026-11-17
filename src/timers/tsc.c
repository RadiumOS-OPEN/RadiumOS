// tsc.c - time source that works before the PIT timer interrupt is running
//
// The TSC is calibrated once against PIT channel 2 (a 10 ms one-shot), so boot-time waits are accurate
// on any machine instead of depending on how fast a busy loop happens to spin.

#include <stdint.h>
#include <stdbool.h>
#include "tsc.h"
#include "../vga/fbcon.h"
#include "../io/io.h"

static uint32_t g_cycles_per_ms = 0;

static inline uint64_t rdtsc64(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void tsc_calibrate(void)
{
    // Gate channel 2 on with the speaker off, then count 5966 PIT ticks (1193182 Hz) = 5 ms, several times.
    outb(0x61, (uint8_t)((inb(0x61) & 0xFC) | 0x01));

    // A host stall only lengthens a trial, so the shortest one is the accurate one.
    uint32_t cycles_5ms = 0xFFFFFFFFu;
    for (int trial = 0; trial < 7; trial++) {
        outb(0x43, 0xB0);
        outb(0x42, (uint8_t)(5966 & 0xFF));
        outb(0x42, (uint8_t)(5966 >> 8));

        uint64_t start = rdtsc64();
        uint32_t guard = 0;
        while (!(inb(0x61) & 0x20) && ++guard < 50000000u) { }
        uint64_t elapsed = rdtsc64() - start;

        uint32_t c = elapsed > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)elapsed;   // 32-bit math: no 64-bit divide in the kernel
        if (c >= 200000u && c < cycles_5ms) cycles_5ms = c;   // anything shorter is a missed edge, not 5 ms
    }
    g_cycles_per_ms = cycles_5ms / 5u;
    if (cycles_5ms == 0xFFFFFFFFu || g_cycles_per_ms < 1000) g_cycles_per_ms = 1000000;   // calibration failed: assume 1 GHz
}

uint32_t tsc_cycles_per_ms(void)
{
    if (g_cycles_per_ms == 0) tsc_calibrate();
    return g_cycles_per_ms;
}

uint64_t tsc_deadline_ms(uint32_t ms)
{
    return rdtsc64() + (uint64_t)ms * tsc_cycles_per_ms();
}

bool tsc_reached(uint64_t deadline)
{
    return rdtsc64() >= deadline;
}

bool tsc_wait_key_ms(uint32_t ms)
{
    uint64_t deadline = tsc_deadline_ms(ms);
    while (!tsc_reached(deadline)) {
        fbcon_poll();
        if (inb(0x64) & 0x01) return true;
    }
    return false;
}
