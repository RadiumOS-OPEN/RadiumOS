// sysutils.c - uptime, free, df, uname, sleep, true, false, poweroff
//
//   uptime            time of day and how long the system has been running
//   free              physical RAM and RAM filesystem usage, in KB
//   df                RAM filesystem and /disk usage, in KB
//   uname             system name and architecture
//   sleep <seconds>   pause
//   true | false      do nothing and succeed / fail (for && and ||)
//   poweroff          shut the machine down (also: shutdown)

#include <stdint.h>
#include <stdbool.h>
#include "sysutils.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../keyboard/keyboard.h"
#include "../timers/timer.h"
#include "../timers/date.h"
#include "../memory/pmm.h"
#include "../io/io.h"
#include "rsd.h"

void uptime_command(int argc, char *argv[])
{
    (void)argc; (void)argv;

    uint32_t days, hours, mins, secs, ms;
    get_uptime_precise(&days, &hours, &mins, &secs, &ms);

    datetime_t now;
    char clock[32];
    rtc_get_time(&now);
    format_time_24h(&now, clock);

    printr(" %s  up ", clock);
    if (days > 0) printr("%u day%s, ", days, days == 1 ? "" : "s");
    printr("%u:%02u:%02u\n", hours, mins, secs);
}

static void kb_row(const char *label, uint32_t total, uint32_t used, uint32_t avail)
{
    print(label);
    for (size_t n = strlen(label); n < 9; n++) print(" ");
    printr("%10u %10u %10u\n", total, used, avail);
}

void free_command(int argc, char *argv[])
{
    (void)argc; (void)argv;

    print("             total       used      avail   (KB)\n");

    uint32_t total = (uint32_t)(pmm_get_total_memory() >> 10);
    uint32_t avail = (uint32_t)(pmm_get_free_memory()  >> 10);
    uint32_t used  = avail >= total ? 0 : total - avail;
    kb_row("Mem:", total, used, avail);

    avfs_ramfs_info_t ram;
    if (avfs_ramfs_query(&ram) == 0 && ram.ready)
        kb_row("RamFS:", ram.total_bytes >> 10, ram.used_bytes >> 10, ram.free_bytes >> 10);
}

static void df_row(const char *name, uint32_t total_kb, uint32_t used_kb, uint32_t avail_kb, const char *mount)
{
    print(name);
    for (size_t n = strlen(name); n < 9; n++) print(" ");
    uint32_t pct = total_kb ? used_kb / ((total_kb + 99) / 100) : 0;
    printr("%10u %10u %10u %3u%%  %s\n", total_kb, used_kb, avail_kb, pct > 100 ? 100 : pct, mount);
}

void df_command(int argc, char *argv[])
{
    (void)argc; (void)argv;

    print("Filesystem      Size       Used      Avail Use%  Mounted on (KB)\n");

    avfs_ramfs_info_t ram;
    if (avfs_ramfs_query(&ram) == 0 && ram.ready)
        df_row("ramfs", ram.total_bytes >> 10, ram.used_bytes >> 10, ram.free_bytes >> 10, "/");

    avfs_disk_info_t disk;
    if (avfs_disk_query(&disk) == 0 && disk.mounted)
        df_row("disk", disk.total_kb, disk.used_kb, disk.free_kb, "/disk");
}

void uname_command(int argc, char *argv[])
{
    (void)argc; (void)argv;
    print("RadiumOS i686\n");
}

void sleep_command(int argc, char *argv[])
{
    if (argc != 2) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: sleep <seconds>\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }

    uint32_t seconds = 0;
    for (const char *p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '9' || seconds > 3600) {
            terminal_setcolor(VGA_COLOR_LIGHT_RED);
            print("sleep: give a whole number of seconds (max 3600)\n");
            terminal_setcolor(VGA_COLOR_WHITE);
            return;
        }
        seconds = seconds * 10 + (uint32_t)(*p - '0');
    }
    if (seconds > 3600) seconds = 3600;

    sleep_seconds(seconds);
}

void true_command(int argc, char *argv[])
{
    (void)argc; (void)argv;
    shell_set_status(0);
}

void false_command(int argc, char *argv[])
{
    (void)argc; (void)argv;
    shell_set_status(1);
}

void poweroff_command(int argc, char *argv[])
{
    // poweroff -f skips the orderly service shutdown
    if (!(argc > 1 && strcmp(argv[1], "-f") == 0)) rsd_shutdown("Power Off");
    avfs_mark_clean();
    print("Powering off...\n");

    outw(0x604, 0x2000);    // QEMU (newer machine types)
    outw(0xB004, 0x2000);   // QEMU (older) and Bochs
    outw(0x4004, 0x3400);   // VirtualBox
    for (const char *s = "Shutdown"; *s; s++) outb(0x8900, (uint8_t)*s);   // Bochs

    print("\nIt is now safe to turn off the computer.\n");
    while (1) asm volatile ("cli; hlt");
}
