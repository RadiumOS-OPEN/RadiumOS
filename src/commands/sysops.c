// sysops.c - system monitoring and maintenance tasks
//
//   cpuinfo               processor details (vendor, model, caches, features)
//   meminfo               physical memory, RAM filesystem and /disk use
//   lspci                 PCI devices with their class and, for common ones, a name
//   watch [-n S] CMD...   run CMD again every S seconds (default 2) until a key is pressed
//   sysstat [N]           history of memory use, disk use and task count as graphs (sampled by the `sample` unit)
//   sysstat sample|reset  take a sample now / forget the history
//   syshealth [-q]        health checks (memory, /disk, clock, network, services, trust); -q prints only problems
//
// The stock units `sample` (every 10 s) and `health` (every 2 min) in rsd.c run sysstat sample and syshealth -q, so the
// journal (journalctl -u health) shows problems as they appear.

#include <stdint.h>
#include <stdbool.h>
#include "sysops.h"
#include "rsd.h"
#include "update.h"
#include "bootpage.h"
#include "fsapp.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../keyboard/keyboard.h"
#include "../timers/timer.h"
#include "../timers/date.h"
#include "../memory/pmm.h"
#include "../cpu/cpu.h"
#include "../user/sudo.h"

extern uint32_t pci_config_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
extern uint32_t rust_get_task_count(void);
extern int rust_rtl8139_get_mac(uint8_t *mac_out);

#define COL(c) terminal_setcolor(c)
#define WHITE  VGA_COLOR_WHITE

static uint32_t percent(uint32_t part, uint32_t whole)
{
    return whole ? (uint32_t)(((uint64_t)part * 100) / whole) : 0;
}

static uint32_t mem_total_kb(void) { return (uint32_t)(pmm_get_total_memory() >> 10); }
static uint32_t mem_free_kb(void)  { return (uint32_t)(pmm_get_free_memory() >> 10); }

static void pad(const char *s, size_t width)
{
    print(s);
    for (size_t n = strlen(s); n < width; n++) print(" ");
}

static void bar(uint32_t pct, int width)
{
    if (pct > 100) pct = 100;
    int fill = (int)(pct * (uint32_t)width / 100);
    COL(pct >= 90 ? VGA_COLOR_LIGHT_RED : (pct >= 70 ? VGA_COLOR_LIGHT_BROWN : VGA_COLOR_LIGHT_GREEN));
    print("[");
    for (int i = 0; i < width; i++) print(i < fill ? "#" : ".");
    print("]");
    COL(WHITE);
}

// ---- cpuinfo --------------------------------------------------------------------------------------

void cpuinfo_command(int argc, char *argv[])
{
    (void)argc; (void)argv;
    print_detailed_cpu_info();
}

// ---- meminfo --------------------------------------------------------------------------------------

void meminfo_command(int argc, char *argv[])
{
    (void)argc; (void)argv;
    uint32_t total = mem_total_kb(), freek = mem_free_kb();
    uint32_t used = freek >= total ? 0 : total - freek;

    COL(VGA_COLOR_LIGHT_CYAN);
    print("Physical memory\n");
    COL(WHITE);
    printr("  Total   %8u KB  (%u MB)\n", total, total >> 10);
    printr("  Used    %8u KB  ", used);  bar(percent(used, total), 40);  printr(" %u%%\n", percent(used, total));
    printr("  Free    %8u KB\n", freek);
    printr("  Pages   %u of 4 KB (%u used, %u free)\n", pmm_get_total_pages(), pmm_get_used_pages(), pmm_get_free_pages());

    avfs_ramfs_info_t ram;
    if (avfs_ramfs_query(&ram) == 0 && ram.ready) {
        COL(VGA_COLOR_LIGHT_CYAN);
        print("RAM filesystem (/)\n");
        COL(WHITE);
        printr("  Used    %8u KB of %u KB  ", ram.used_bytes >> 10, ram.total_bytes >> 10);
        bar(percent(ram.used_bytes >> 10, ram.total_bytes >> 10), 40);
        printr("\n  Files   %u, directories %u\n", ram.file_count, ram.dir_count);
    }

    avfs_disk_info_t disk;
    if (avfs_disk_query(&disk) == 0 && disk.mounted) {
        COL(VGA_COLOR_LIGHT_CYAN);
        print("Disk (/disk)\n");
        COL(WHITE);
        printr("  Used    %8u KB of %u KB  ", disk.used_kb, disk.total_kb);
        bar(percent(disk.used_kb, disk.total_kb), 40);
        printr("\n  Files   %u, directories %u\n", disk.file_count, disk.dir_count);
    }
}

// ---- lspci ----------------------------------------------------------------------------------------

static const struct { uint16_t vendor, device; const char *name; } PCI_KNOWN[] = {
    { 0x8086, 0x1237, "Intel 440FX host bridge" },       { 0x8086, 0x7000, "Intel PIIX3 ISA bridge" },
    { 0x8086, 0x7010, "Intel PIIX3 IDE controller" },    { 0x8086, 0x7113, "Intel PIIX4 power management" },
    { 0x8086, 0x2415, "Intel 82801AA AC'97 audio" },     { 0x8086, 0x100E, "Intel 82540EM Gigabit Ethernet" },
    { 0x8086, 0x29C0, "Intel Q35 host bridge" },         { 0x1234, 0x1111, "QEMU/Bochs VGA display" },
    { 0x10EC, 0x8139, "Realtek RTL8139 Ethernet" },      { 0x1AF4, 0x1000, "virtio network" },
    { 0x1AF4, 0x1001, "virtio block" },                  { 0x15AD, 0x0405, "VMware SVGA II" },
};

static const char *pci_vendor(uint16_t v)
{
    switch (v) {
    case 0x8086: return "Intel";    case 0x10EC: return "Realtek";   case 0x1234: return "QEMU";
    case 0x1AF4: return "Red Hat";  case 0x15AD: return "VMware";    case 0x80EE: return "VirtualBox";
    case 0x10DE: return "NVIDIA";   case 0x1002: return "AMD/ATI";   case 0x1022: return "AMD";
    default: return NULL;
    }
}

static const char *pci_class(uint8_t cls, uint8_t sub)
{
    switch (cls) {
    case 0x01: return sub == 0x01 ? "IDE controller" : (sub == 0x06 ? "SATA controller" : "Storage controller");
    case 0x02: return sub == 0x00 ? "Ethernet controller" : "Network controller";
    case 0x03: return sub == 0x00 ? "VGA controller" : "Display controller";
    case 0x04: return sub == 0x01 ? "Audio device" : (sub == 0x03 ? "HD audio" : "Multimedia device");
    case 0x06: return sub == 0x00 ? "Host bridge" : (sub == 0x01 ? "ISA bridge" : (sub == 0x04 ? "PCI bridge" : "Bridge"));
    case 0x0C: return sub == 0x03 ? "USB controller" : "Serial bus controller";
    default:   return "Device";
    }
}

void lspci_command(int argc, char *argv[])
{
    (void)argc; (void)argv;
    int found = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t id0 = pci_config_read_dword((uint8_t)bus, (uint8_t)slot, 0, 0);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            bool multi = (pci_config_read_dword((uint8_t)bus, (uint8_t)slot, 0, 0x0C) >> 16) & 0x80;
            for (int fn = 0; fn < (multi ? 8 : 1); fn++) {
                uint32_t id = pci_config_read_dword((uint8_t)bus, (uint8_t)slot, (uint8_t)fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                uint16_t vendor = (uint16_t)(id & 0xFFFF), device = (uint16_t)(id >> 16);
                uint32_t cc = pci_config_read_dword((uint8_t)bus, (uint8_t)slot, (uint8_t)fn, 0x08);
                const char *name = NULL;
                for (size_t i = 0; i < sizeof(PCI_KNOWN) / sizeof(PCI_KNOWN[0]); i++)
                    if (PCI_KNOWN[i].vendor == vendor && PCI_KNOWN[i].device == device) name = PCI_KNOWN[i].name;
                const char *vname = pci_vendor(vendor);

                COL(VGA_COLOR_LIGHT_CYAN);
                printr("%02x:%02x.%u ", bus, slot, fn);
                COL(WHITE);
                printr("%04x:%04x  ", vendor, device);
                COL(VGA_COLOR_LIGHT_GREEN);
                print(pci_class((uint8_t)(cc >> 24), (uint8_t)(cc >> 16)));
                COL(WHITE);
                if (name) printr(": %s", name);
                else if (vname) printr(": %s device", vname);
                print("\n");
                found++;
            }
        }
    }
    printr("%d PCI device%s\n", found, found == 1 ? "" : "s");
}

// ---- watch ----------------------------------------------------------------------------------------

void watch_command(int argc, char *argv[])
{
    uint32_t secs = 2;
    int first = 1;
    if (argc > 2 && strcmp(argv[1], "-n") == 0) {
        secs = (uint32_t)atoi(argv[2]);
        if (secs < 1 || secs > 3600) { COL(VGA_COLOR_LIGHT_RED); print("watch: -n takes 1 to 3600 seconds\n"); COL(WHITE); return; }
        first = 3;
    }
    if (first >= argc) { COL(VGA_COLOR_LIGHT_RED); print("Usage: watch [-n SECONDS] COMMAND...\n"); COL(WHITE); return; }
    if (strcmp(argv[first], "watch") == 0) { COL(VGA_COLOR_LIGHT_RED); print("watch: cannot watch itself\n"); COL(WHITE); return; }

    static char cmd[256];
    cmd[0] = '\0';
    for (int i = first; i < argc; i++) {
        if (strlen(cmd) + strlen(argv[i]) + 2 >= sizeof(cmd)) break;
        if (i > first) strcat(cmd, " ");
        strcat(cmd, argv[i]);
    }

    for (;;) {
        terminal_clear();
        datetime_t now;
        char clock[32];
        rtc_get_time(&now);
        format_time_24h(&now, clock);
        COL(VGA_COLOR_LIGHT_CYAN);
        printr("Every %us: %s    %s    (any key quits)\n\n", secs, cmd, clock);
        COL(WHITE);
        execute_command_no_history(cmd);
        if (fs_wait(secs * 1000)) break;
    }
    print("\n");
}

// ---- sysstat --------------------------------------------------------------------------------------

#define ST_MAX 180

typedef struct { uint32_t ms; uint8_t mem_pct, disk_pct; uint16_t tasks; uint32_t free_kb; } sample_t;

static sample_t g_hist[ST_MAX];
static int      g_hist_n = 0;

static void take_sample(void)
{
    if (g_hist_n == ST_MAX) { memmove(&g_hist[0], &g_hist[1], sizeof(sample_t) * (ST_MAX - 1)); g_hist_n--; }
    sample_t *s = &g_hist[g_hist_n++];
    uint32_t total = mem_total_kb(), freek = mem_free_kb();
    avfs_disk_info_t disk;
    bool mounted = avfs_disk_query(&disk) == 0 && disk.mounted;
    s->ms = get_time_ms();
    s->mem_pct = (uint8_t)percent(freek >= total ? 0 : total - freek, total);
    s->disk_pct = mounted ? (uint8_t)percent(disk.used_kb, disk.total_kb) : 0;
    s->tasks = (uint16_t)rust_get_task_count();
    s->free_kb = freek;
}

// A one-line graph of the last `cols` samples scaled between lo and hi.
static void sparkline(const uint32_t *v, int n, int cols, uint32_t lo, uint32_t hi)
{
    static const char ramp[] = " .:-=+*#%@";
    int start = n > cols ? n - cols : 0;
    for (int i = start; i < n; i++) {
        uint32_t level = hi > lo ? (v[i] - lo) * 9 / (hi - lo) : 0;
        char one[2] = { ramp[level > 9 ? 9 : level], '\0' };
        print(one);
    }
}

static void graph_row(const char *label, const uint32_t *v, int n, int cols, uint32_t lo, uint32_t hi, const char *unit)
{
    uint32_t mn = v[0], mx = v[0], sum = 0;
    for (int i = 0; i < n; i++) { if (v[i] < mn) mn = v[i]; if (v[i] > mx) mx = v[i]; sum += v[i]; }
    COL(VGA_COLOR_LIGHT_CYAN);
    pad(label, 7);
    COL(VGA_COLOR_LIGHT_GREEN);
    print("|");
    sparkline(v, n, cols, lo, hi);
    for (int i = n < cols ? n : cols; i < cols; i++) print(" ");
    print("|");
    COL(WHITE);
    printr(" now %u%s  min %u  avg %u  max %u\n", v[n - 1], unit, mn, sum / (uint32_t)n, mx);
}

void sysstat_command(int argc, char *argv[])
{
    if (argc > 1 && strcmp(argv[1], "sample") == 0) { take_sample(); return; }
    if (argc > 1 && strcmp(argv[1], "reset") == 0) { g_hist_n = 0; print("sysstat: history cleared\n"); return; }

    int cols = 60;
    if (argc > 1) cols = atoi(argv[1]);
    if (cols < 10 || cols > 120) { COL(VGA_COLOR_LIGHT_RED); print("Usage: sysstat [10-120] | sample | reset\n"); COL(WHITE); return; }

    take_sample();
    if (g_hist_n < 2) take_sample();
    static uint32_t mem[ST_MAX], disk[ST_MAX], tasks[ST_MAX], freek[ST_MAX];
    for (int i = 0; i < g_hist_n; i++) {
        mem[i] = g_hist[i].mem_pct; disk[i] = g_hist[i].disk_pct; tasks[i] = g_hist[i].tasks; freek[i] = g_hist[i].free_kb >> 10;
    }
    uint32_t span = (g_hist[g_hist_n - 1].ms - g_hist[0].ms) / 1000;
    COL(VGA_COLOR_LIGHT_CYAN);
    printr("System history: %d sample%s over %u s\n", g_hist_n, g_hist_n == 1 ? "" : "s", span);
    COL(WHITE);
    graph_row("mem %", mem, g_hist_n, cols, 0, 100, "%");
    graph_row("disk %", disk, g_hist_n, cols, 0, 100, "%");
    graph_row("tasks", tasks, g_hist_n, cols, 0, 16, "");
    graph_row("free", freek, g_hist_n, cols, 0, mem_total_kb() >> 10, " MB");
    COL(VGA_COLOR_LIGHT_GREY);
    print("  (the `sample` unit adds a point every 10 seconds; `sysstat sample|reset`)\n");
    COL(WHITE);
}

// ---- syshealth ------------------------------------------------------------------------------------

typedef struct { int ok, warn, fail; bool quiet; } health_t;

static void check(health_t *h, int level, const char *name, const char *detail)
{
    if (level == 0) h->ok++; else if (level == 1) h->warn++; else h->fail++;
    if (h->quiet && level == 0) return;
    COL(WHITE); print("[");
    COL(level == 0 ? VGA_COLOR_LIGHT_GREEN : (level == 1 ? VGA_COLOR_LIGHT_BROWN : VGA_COLOR_LIGHT_RED));
    print(level == 0 ? "  OK  " : (level == 1 ? " WARN " : " FAIL "));
    COL(WHITE);
    print("] ");
    pad(name, 15);
    print(detail);
    print("\n");
}

void syshealth_command(int argc, char *argv[])
{
    health_t h = { 0, 0, 0, argc > 1 && strcmp(argv[1], "-q") == 0 };
    char text[96];

    uint32_t total = mem_total_kb(), freek = mem_free_kb();
    uint32_t free_pct = percent(freek, total);
    snprintf(text, sizeof(text), "%u MB free of %u MB (%u%%)", freek >> 10, total >> 10, free_pct);
    check(&h, free_pct < 3 ? 2 : (free_pct < 10 ? 1 : 0), "memory", text);

    avfs_disk_info_t disk;
    if (avfs_disk_query(&disk) == 0 && disk.mounted) {
        uint32_t dfree = percent(disk.free_kb, disk.total_kb);
        snprintf(text, sizeof(text), "%u KB free of %u KB (%u%%)", disk.free_kb, disk.total_kb, dfree);
        check(&h, dfree < 3 ? 2 : (dfree < 10 ? 1 : 0), "/disk", text);
    } else {
        check(&h, 2, "/disk", "not mounted - files live in RAM and are lost on reboot");
    }

    datetime_t now;
    rtc_get_time(&now);
    snprintf(text, sizeof(text), "%u-%02u-%02u %02u:%02u:%02u", now.year, now.month, now.day, now.hour, now.minute, now.second);
    check(&h, now.year >= 2024 && now.year < 2100 ? 0 : 1, "clock", now.year >= 2024 && now.year < 2100 ? text : "the real-time clock looks wrong");

    uint8_t mac[6];
    bool nic = rust_rtl8139_get_mac(mac) == 0;
    check(&h, nic ? 0 : 1, "network", nic ? "RTL8139 present" : "no network card found");

    uint32_t tasks = rust_get_task_count();
    snprintf(text, sizeof(text), "%u running", tasks);
    check(&h, tasks > 64 ? 1 : 0, "tasks", text);

    int failed = 0, units = rsd_unit_count();
    char first[16] = "";
    for (int i = 0; i < units; i++) {
        rsd_unit_view_t v;
        if (rsd_unit_view(i, &v) && v.failed) { if (!failed) strncpy(first, v.name, sizeof(first) - 1); failed++; }
    }
    if (failed) snprintf(text, sizeof(text), "%d failed (%s...) - systemctl list-units", failed, first);
    else snprintf(text, sizeof(text), "%d units, none failed", units);
    check(&h, failed ? 1 : 0, "services", text);

    check(&h, update_is_unverified() ? 1 : 0, "update trust",
          update_is_unverified() ? "(unverified): an unsigned update is installed" : "no unsigned update installed");
    check(&h, sudo_is_configured() ? 0 : 1, "sudo", sudo_is_configured() ? "password set" : "no sudo password - run cpw");

    if (!h.quiet) printr("\n%d ok, %d warning%s, %d failure%s\n", h.ok, h.warn, h.warn == 1 ? "" : "s", h.fail, h.fail == 1 ? "" : "s");
    shell_set_status(h.fail ? 2 : (h.warn && !h.quiet ? 1 : 0));   // -q is for the timer unit: only failures mark it failed
}
