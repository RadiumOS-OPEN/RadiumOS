// guard.c - the `guard` command: what the kernel guard tasks (rust_lib/src/guard.rs) have found and done

#include <stdint.h>
#include <stdbool.h>
#include "guard.h"
#include "../terminal/terminal.h"
#include "../utility/utility.h"
#include "../keyboard/keyboard.h"
#include "../timers/timer.h"
#include "../Avfs/Avfs.h"

extern uint32_t rust_guard_stats(uint32_t *out, uint32_t n);
extern uint32_t rust_guard_event(uint32_t seq, uint32_t *out);
extern uint32_t rust_guard_selftest_begin(void);
extern uint32_t rust_guard_selftest_result(void);

enum { S_EVENTS, S_ALERTS, S_TEXT_CHECKED, S_TEXT_PASSES, S_TEXT_TOTAL, S_CPU_CHECKS, S_REPAIRS, S_RESPAWNS, S_REAPED, S_ROGUE,
       S_REFUSED, S_LOWMEM, S_GUARDS, S_FLOOD, S_TICKS, S_COUNT };

static const char *const KIND[16] = {
    "", "kernel code or read-only data changed", "stack canary changed (restored)", "interrupt gate changed (restored)",
    "CR0 protection bits changed", "CR4 protection bits changed", "I/O privilege level raised", "task with code outside the kernel (stopped)",
    "essential task started again", "task counter repaired", "physical memory is low", "kill of an essential task refused",
    "stuck task woken", "task flood trimmed", "could not start a task", "guard tasks started",
};

static bool is_alert(uint32_t k) { return (k >= 1 && k <= 7) || k == 13 || k == 14; }

static void describe(uint32_t kind, uint32_t a, uint32_t b, char *out, int n)
{
    const char *t = kind < 16 ? KIND[kind] : "unknown";
    switch (kind) {
    case 1: snprintf(out, (size_t)n, "%s: block %u at 0x%x", t, a, b); break;
    case 3: snprintf(out, (size_t)n, "%s: vector %u -> 0x%x", t, a, b); break;
    case 4: case 5: snprintf(out, (size_t)n, "%s: 0x%x -> 0x%x", t, a, b); break;
    case 7: snprintf(out, (size_t)n, "%s: pid %u entry 0x%x", t, a, b); break;
    case 8: snprintf(out, (size_t)n, "%s: entry 0x%x, pid %u", t, a, b); break;
    case 9: snprintf(out, (size_t)n, "%s: %u -> %u", t, a, b); break;
    case 10: snprintf(out, (size_t)n, "%s: %u of %u MB free", t, a, b); break;
    case 11: case 12: case 13: snprintf(out, (size_t)n, "%s: pid %u", t, a); break;
    default: snprintf(out, (size_t)n, "%s", t); break;
    }
}

unsigned guard_alert_count(void)
{
    uint32_t s[S_COUNT];
    rust_guard_stats(s, S_COUNT);
    return s[S_ALERTS];
}

void guard_flush_log(void)
{
    static uint32_t flushed;
    uint32_t s[S_COUNT];
    rust_guard_stats(s, S_COUNT);
    uint32_t total = s[S_EVENTS];
    if (total <= flushed || !avfs_is_directory("/disk")) return;
    if (total - flushed > 64) flushed = total - 64;
    for (; flushed < total; flushed++) {
        uint32_t e[4];
        if (!rust_guard_event(flushed, e)) continue;
        char text[120], line[160];
        describe(e[1], e[2], e[3], text, sizeof(text));
        int len = snprintf(line, sizeof(line), "[tick %u] %s%s\n", e[0], is_alert(e[1]) ? "ALERT " : "", text);
        if (!avfs_file_exists("/disk/guard.log")) avfs_save_file("/disk/guard.log", line, (uint32_t)len, true);
        else avfs_append_file("/disk/guard.log", line, (uint32_t)len);
    }
}

static int selftest(void)
{
    static const char *const WHAT[5] = { "a task running code outside the kernel is stopped", "a crashed guard task is started again",
                                         "a hooked interrupt gate is restored", "an essential task cannot be killed",
                                         "the supervisor counted the rogue task" };
    static const uint32_t BIT[5] = { 1, 2, 4, 8, 16 };
    printr("guard selftest: injecting a rogue task, a crashed guard, a hooked interrupt gate and a kill of an essential task\n");
    rust_guard_selftest_begin();
    uint32_t got = 0;
    for (int i = 0; i < 40 && (got & 31) != 31; i++) { sleep_ms(250); got = rust_guard_selftest_result(); }
    int bad = 0;
    for (int i = 0; i < 5; i++) {
        bool ok = (got & BIT[i]) != 0;
        if (!ok) bad++;
        terminal_setcolor(ok ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_RED);
        printr("  %s  %s\n", ok ? "ok  " : "FAIL", WHAT[i]);
        terminal_setcolor(VGA_COLOR_WHITE);
    }
    printr("guard selftest: %d of 5 checks passed\n", 5 - bad);
    return bad;
}

void guard_command(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) { if (selftest()) shell_set_status(1); return; }
    if (argc >= 2 && strcmp(argv[1], "log") == 0) {
        guard_flush_log();
        if (avfs_file_exists("/disk/guard.log")) execute_command_no_history("cat /disk/guard.log");
        else print("guard: nothing logged yet\n");
        return;
    }
    if (argc >= 2) { print("Usage: guard [selftest | log]\n"); shell_set_status(1); return; }

    uint32_t s[S_COUNT];
    rust_guard_stats(s, S_COUNT);
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("=== Kernel guard ===\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    printr("guard tasks running   %u of 4 (text, cpu, tasks, mem)\n", s[S_GUARDS]);
    printr("kernel code + rodata  %u blocks of 4 KB, %u checked, %u full passes\n", s[S_TEXT_TOTAL], s[S_TEXT_CHECKED], s[S_TEXT_PASSES]);
    printr("cpu state checks      %u (CR0/CR4, I/O privilege, stack canary, interrupt table)\n", s[S_CPU_CHECKS]);
    printr("repairs               %u\n", s[S_REPAIRS]);
    printr("tasks                 %u essential tasks restarted, %u zombies reaped, %u rogue stopped, %u kills refused, %u floods trimmed\n",
           s[S_RESPAWNS], s[S_REAPED], s[S_ROGUE], s[S_REFUSED], s[S_FLOOD]);
    printr("low-memory episodes   %u\n", s[S_LOWMEM]);
    terminal_setcolor(s[S_ALERTS] ? VGA_COLOR_LIGHT_RED : VGA_COLOR_LIGHT_GREEN);
    printr("alerts                %u%s\n", s[S_ALERTS], s[S_ALERTS] ? "  (see the events below and 'guard log')" : "  (clean)");
    terminal_setcolor(VGA_COLOR_WHITE);

    uint32_t total = s[S_EVENTS], first = total > 12 ? total - 12 : 0;
    if (total == 0) return;
    print("\nrecent events\n");
    for (uint32_t i = first; i < total; i++) {
        uint32_t e[4];
        if (!rust_guard_event(i, e)) continue;
        char text[120];
        describe(e[1], e[2], e[3], text, sizeof(text));
        terminal_setcolor(is_alert(e[1]) ? VGA_COLOR_LIGHT_RED : VGA_COLOR_LIGHT_GREY);
        printr("  %us  %s\n", e[0] / 1000, text);
    }
    terminal_setcolor(VGA_COLOR_WHITE);
}
