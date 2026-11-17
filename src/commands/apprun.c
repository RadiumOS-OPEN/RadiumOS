// apprun.c - runs a static ELF "app" (port/Makefile: C++ on musl + libc++) inside the kernel
//
//   app FILE [args...]     FILE is read from /disk (FAT32 first, then the fetch store)
//
// The app is linked at a fixed address (0x10000000). It is loaded there, given a private arena of RAM from the
// physical memory manager, and called as radium_app_main(api, argc, argv); everything it needs from the system
// goes through the table in port/rt/radium_api.h. It runs in ring 0 on the kernel stack with interrupts as the
// shell left them, so it uses the TSC for time and polls the keyboard controller for keys.

#include <stdint.h>
#include <stdbool.h>
#include "apprun.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../vga/fbcon.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../io/io.h"
#include "../memory/pmm.h"
#include "../scheduler/task.h"
#include "../timers/tsc.h"
#include "../timers/date.h"
#include "../keyboard/keyboard.h"
#include "../../port/rt/radium_api.h"
#include "acmods.h"
#include "../network/udpsock.h"
#include "../network/dns.h"
#include "../network/netstack.h"
#include "../user/users.h"
#include "../user/sudo.h"
#include "mv.h"
#include "../kernel/ctrlc.h"

extern int cat_resolve_path(const char *arg, char *out, int out_sz);

extern GDTPointer gdt_pointer;
extern int rust_prp_random(uint8_t *output, uint32_t len);
extern int rust_disk_store_size(const uint8_t *path);
extern int rust_disk_store_read(const uint8_t *path, uint32_t offset, uint8_t *buf, uint32_t len);

#define APP_BASE        0x10000000u
#define APP_IMAGE_MAX   0x0C000000u          // 192 MB for the ELF image
#define ARENA_MAX_PAGES (1536u * 256u)       // 1.5 GB of RAM at most

#define APP_ERR(...) do { terminal_setcolor(VGA_COLOR_LIGHT_RED); printr(__VA_ARGS__); terminal_setcolor(VGA_COLOR_WHITE); } while (0)

// ---- capabilities --------------------------------------------------------------------------------------------------
// The app runs in ring 0, so this limits what a well-behaved app can ask the kernel for through radium_api_t; it is not a
// memory boundary against hostile native code. Each api call below checks g_caps (and the protected-file list unless the
// user is root). Default: root and the single-user mode get everything, accounts get everything but the network.
// /disk/etc/apps ("NAME=cap,cap" per line) sets an app's ceiling, `app --sandbox` and `app --caps=LIST` can only lower it.

#define CAP_FS_READ  1u
#define CAP_FS_WRITE 2u
#define CAP_NET      4u
#define CAP_GFX      8u
#define CAP_MODS     16u
#define CAP_ALL      31u

static uint32_t g_caps = CAP_ALL;
static bool     g_trusted = true;     // may touch protected files
static uint32_t g_denied;
static int      g_gfx_used;
static int      g_gfx_on;       // the display is currently handed to the app

static const struct { const char *name; uint32_t bit; } CAP_NAMES[] = {
    { "fs.read", CAP_FS_READ }, { "fs.write", CAP_FS_WRITE }, { "net", CAP_NET }, { "gfx", CAP_GFX }, { "mods", CAP_MODS },
};

// "fs.read,net" -> mask; "all" and "none" work too. False for an unknown name.
static bool parse_caps(const char *list, uint32_t *mask)
{
    uint32_t m = 0;
    while (*list) {
        char word[16];
        size_t n = 0;
        while (*list && *list != ',' && n < sizeof(word) - 1) word[n++] = *list++;
        word[n] = '\0';
        if (*list == ',') list++;
        else if (*list) return false;
        if (strcmp(word, "all") == 0) { m |= CAP_ALL; continue; }
        if (strcmp(word, "none") == 0) continue;
        bool known = false;
        for (size_t i = 0; i < sizeof(CAP_NAMES) / sizeof(CAP_NAMES[0]); i++)
            if (strcmp(word, CAP_NAMES[i].name) == 0) { m |= CAP_NAMES[i].bit; known = true; }
        if (!known) return false;
    }
    *mask = m;
    return true;
}

static void print_caps(uint32_t m)
{
    bool any = false;
    for (size_t i = 0; i < sizeof(CAP_NAMES) / sizeof(CAP_NAMES[0]); i++)
        if (m & CAP_NAMES[i].bit) { printr("%s%s", any ? "," : "", CAP_NAMES[i].name); any = true; }
    if (!any) print("none");
}

// The ceiling set for `stem` in /disk/etc/apps, if there is a line for it.
static bool policy_caps(const char *stem, uint32_t *mask)
{
    static char buf[2048];
    int size = avfs_file_exists("/disk/etc/apps") ? avfs_get_filesize("/disk/etc/apps") : -1;
    if (size <= 0 || size >= (int)sizeof(buf) || avfs_read_file("/disk/etc/apps", buf, (uint32_t)size, 0) != 0) return false;
    buf[size] = '\0';
    size_t sl = strlen(stem);
    for (char *line = buf; *line; ) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = '\0';
        if (line[0] != '#' && strncmp(line, stem, sl) == 0 && line[sl] == '=' && parse_caps(line + sl + 1, mask)) return true;
        if (!next) break;
        line = next;
    }
    return false;
}

static void cap_denied(const char *what)
{
    if (g_denied++ < 8) { char l[64]; int n = snprintf(l, sizeof l, "app: denied %s\n", what); for (int i = 0; i < n; i++) outb(0xE9, (uint8_t)l[i]); }
}

static bool fs_ok(const char *path, uint32_t cap)
{
    if (!(g_caps & cap)) { cap_denied(cap == CAP_FS_WRITE ? "file write" : "file read"); return false; }
    if (!g_trusted) {
        char r[512];
        mv_resolve(path, r);
        if (sudo_path_protected(r, cap == CAP_FS_WRITE)) { cap_denied("a protected file"); return false; }
    }
    return true;
}

typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff, flags;
                 uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } elf_header_t;
typedef struct { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; } elf_phdr_t;

// ---- file access: FAT32 first, then the raw fetch store ------------------------------------------------------------

static int file_size_of(const char *path)
{
    if (avfs_file_exists(path) && !avfs_is_directory(path)) return avfs_get_filesize(path);
    return rust_disk_store_size((const uint8_t *)path);
}

bool app_file_exists(const char *path) { return file_size_of(path) >= 0; }

static int read_at(const char *path, uint32_t off, void *buf, uint32_t n)
{
    if (avfs_file_exists(path) && !avfs_is_directory(path)) {
        int size = avfs_get_filesize(path);
        if (size < 0 || off >= (uint32_t)size) return 0;
        if (n > (uint32_t)size - off) n = (uint32_t)size - off;
        for (uint32_t done = 0; done < n; ) {
            uint32_t chunk = n - done > 65536 ? 65536 : n - done;
            if (avfs_read_file(path, (uint8_t *)buf + done, chunk, off + done) != 0) return -1;
            done += chunk;
        }
        return (int)n;
    }
    int size = rust_disk_store_size((const uint8_t *)path);
    if (size < 0) return -1;
    if (off >= (uint32_t)size) return 0;
    if (n > (uint32_t)size - off) n = (uint32_t)size - off;
    for (uint32_t done = 0; done < n; ) {
        uint32_t chunk = n - done > 65536 ? 65536 : n - done;
        if (rust_disk_store_read((const uint8_t *)path, off + done, (uint8_t *)buf + done, chunk) != (int)chunk) return -1;
        done += chunk;
    }
    return (int)n;
}

// ---- the radium_api_t table ---------------------------------------------------------------------------------------

static uint64_t g_start_tsc;

static uint64_t rdtsc64(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void api_console_write(const char *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) terminal_putchar(s[i]);
}

static void api_debug_write(const char *s, uint32_t n) { for (uint32_t i = 0; i < n; i++) outb(0xE9, (uint8_t)s[i]); }

// Scancodes and mouse bytes share the PS/2 output buffer: both are read here so neither blocks the other.
static int g_keyq[64], g_keyq_h, g_keyq_t;
static int g_mouse_dx, g_mouse_dy, g_mouse_btn, g_mouse_last_btn, g_mouse_idx;
static uint8_t g_mouse_pkt[3];

static void ps2_pump(void)
{
    for (int guard = 0; guard < 64; guard++) {
        uint8_t st = inb(0x64);
        if (!(st & 1)) return;
        uint8_t d = inb(0x60);
        if (st & 0x20) {                                   // mouse byte
            if (g_mouse_idx == 0 && !(d & 8)) continue;    // out of sync
            g_mouse_pkt[g_mouse_idx++] = d;
            if (g_mouse_idx == 3) {
                g_mouse_idx = 0;
                int dx = g_mouse_pkt[1] - ((g_mouse_pkt[0] << 4) & 0x100), dy = g_mouse_pkt[2] - ((g_mouse_pkt[0] << 3) & 0x100);
                if (!(g_mouse_pkt[0] & 0xC0)) { g_mouse_dx += dx; g_mouse_dy -= dy; }
                g_mouse_btn = g_mouse_pkt[0] & 7;
            }
        } else if (((g_keyq_h + 1) & 63) != g_keyq_t) {
            g_keyq[g_keyq_h] = d;
            g_keyq_h = (g_keyq_h + 1) & 63;
        }
    }
}

static int api_key_poll(void)
{
    fbcon_poll();
    ps2_pump();
    if (g_keyq_t == g_keyq_h) return -1;
    int d = g_keyq[g_keyq_t];
    g_keyq_t = (g_keyq_t + 1) & 63;
    return d;
}

static void kbc_wait_write(void) { for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) { } }

static void mouse_cmd(uint8_t c)
{
    kbc_wait_write(); outb(0x64, 0xD4);
    kbc_wait_write(); outb(0x60, c);
    for (int i = 0; i < 100000; i++) if (inb(0x64) & 1) { if (inb(0x60) == 0xFA) return; }
}

static int api_mouse_poll(int *dx, int *dy, int *buttons)
{
    if (!g_gfx_used) { *dx = *dy = *buttons = 0; return 0; }
    ps2_pump();
    int changed = g_mouse_dx || g_mouse_dy || g_mouse_btn != g_mouse_last_btn;
    *dx = g_mouse_dx; *dy = g_mouse_dy; *buttons = g_mouse_btn;
    g_mouse_dx = g_mouse_dy = 0;
    g_mouse_last_btn = g_mouse_btn;
    return changed;
}

// ---- network ----------------------------------------------------------------------------------------------------

static int api_net_udp_open(void)
{
    if (!(g_caps & CAP_NET)) { cap_denied("network"); return -1; }
    g_net_quiet = 1;
    return udpsock_open();
}
static int api_net_resolve(const char *host, uint32_t *ip)
{
    if (!(g_caps & CAP_NET)) { cap_denied("name lookup"); return -1; }
    udpsock_ensure_config();
    int ok = dns_resolve(host, ip);
    char line[96];
    int n = snprintf(line, sizeof line, "net: resolve %s -> %s\n", host, ok ? "ok" : "failed");
    api_debug_write(line, (uint32_t)n);
    return ok ? 0 : -1;
}
static uint32_t api_net_local_ip(void) { return (g_caps & CAP_NET) ? net_config.ip_address : 0; }

static int api_gfx_open(uint32_t w, uint32_t h)
{
    (void)w; (void)h;
    if (!(g_caps & CAP_GFX)) { cap_denied("graphics"); return -1; }
    ctrlc_disarm();                                        // a program that takes the screen reads its own keys
    g_gfx_used = 1;
    g_gfx_on = 1;
    fbcon_suspend();
    static uint32_t black[FBCON_WIDTH];
    for (int y = 0; y < FBCON_HEIGHT; y++) fbcon_blit(black, FBCON_WIDTH, 0, y, FBCON_WIDTH, 1);
    kbc_wait_write(); outb(0x64, 0xA8);                    // enable the auxiliary (mouse) port
    mouse_cmd(0xF6);
    mouse_cmd(0xF4);                                       // start streaming movement packets
    g_mouse_idx = g_mouse_dx = g_mouse_dy = g_mouse_btn = g_mouse_last_btn = 0;
    return 0;
}

static void api_gfx_present(const uint32_t *px, uint32_t w, uint32_t h)
{
    static uint32_t row[FBCON_WIDTH];
    if (!g_gfx_used) return;
    uint32_t k = FBCON_WIDTH / w < FBCON_HEIGHT / h ? FBCON_WIDTH / w : FBCON_HEIGHT / h;
    if (k == 0) k = 1;
    int ox = (int)(FBCON_WIDTH - w * k) / 2, oy = (int)(FBCON_HEIGHT - h * k) / 2;
    uint32_t cw = w * k > FBCON_WIDTH ? FBCON_WIDTH : w * k;
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t *src = px + (size_t)y * w;
        if (k == 1) { fbcon_blit(src, (int)w, ox, oy + (int)y, (int)cw, 1); continue; }
        for (uint32_t x = 0; x < w && x * k < cw; x++) for (uint32_t j = 0; j < k; j++) row[x * k + j] = src[x];
        for (uint32_t j = 0; j < k; j++) fbcon_blit(row, (int)cw, ox, oy + (int)(y * k + j), (int)cw, 1);
    }
    ps2_pump();
}

static void api_gfx_close(void)
{
    if (!g_gfx_on) return;
    g_gfx_on = 0;
    mouse_cmd(0xF5);
    g_keyq_h = g_keyq_t = 0;
    fbcon_resume();
}

static uint32_t api_millis(void)
{
    uint64_t delta = rdtsc64() - g_start_tsc;
    uint32_t per_ms = tsc_cycles_per_ms();
    uint32_t hi = (uint32_t)(delta >> 32), lo = (uint32_t)delta, ms = 0;
    // delta / per_ms with 32-bit steps: (hi:lo) / per_ms by long division (no 64-bit divide in the kernel)
    uint32_t rem = 0;
    for (int bit = 63; bit >= 0; bit--) {
        uint32_t in = bit >= 32 ? (hi >> (bit - 32)) & 1 : (lo >> bit) & 1;
        rem = (rem << 1) | in;
        if (rem >= per_ms) { rem -= per_ms; if (bit < 32) ms |= 1u << bit; }
    }
    return ms;
}

static void api_screen_blit(const uint16_t *cells, uint32_t cols, uint32_t rows)
{
    extern uint16_t *terminal_buffer;
    extern size_t terminal_width, terminal_height;
    for (uint32_t y = 0; y < rows && y < terminal_height; y++)
        memcpy(terminal_buffer + y * terminal_width, cells + (size_t)y * cols, (cols < terminal_width ? cols : terminal_width) * 2);
    fbcon_poll();
}

static uint32_t api_unix_time(void) { return get_unix_timestamp(); }
static uint32_t api_random32(void)  { return ((uint32_t)returnRandomInteger() << 16) ^ (uint32_t)returnRandomInteger() ^ (uint32_t)rdtsc64(); }

static void api_sleep_ms(uint32_t ms)
{
    uint64_t deadline = tsc_deadline_ms(ms);
    while (!tsc_reached(deadline)) { fbcon_poll(); __asm__ __volatile__("pause"); }
}

static uint32_t api_tls_selector(uint32_t base)
{
    set_gdt_entry(6, base, 0xFFFFFFFF, 0xF2, 0xC0);       // flat data segment starting at `base`, DPL 3
    load_gdt((uint32_t)&gdt_pointer);
    return (6u << 3) | 3u;
}

static uint32_t api_fs_selector(uint32_t base)
{
    set_gdt_entry(7, base, 0xFFFFFFFF, 0xF2, 0xC0);
    load_gdt((uint32_t)&gdt_pointer);
    return (7u << 3) | 3u;
}

static int api_file_stat(const char *path, uint32_t *size, int *is_dir)
{
    if (!fs_ok(path, CAP_FS_READ)) return -1;
    if (avfs_file_exists(path)) {
        *is_dir = avfs_is_directory(path) ? 1 : 0;
        int s = *is_dir ? 0 : avfs_get_filesize(path);
        *size = s < 0 ? 0 : (uint32_t)s;
        return 0;
    }
    int s = rust_disk_store_size((const uint8_t *)path);
    if (s < 0) return -1;
    *size = (uint32_t)s;
    *is_dir = 0;
    return 0;
}

static int api_file_read(const char *path, uint32_t off, void *buf, uint32_t n)
{
    return fs_ok(path, CAP_FS_READ) ? read_at(path, off, buf, n) : -1;
}

static int api_file_write(const char *path, const void *buf, uint32_t n, int append)
{
    if (!fs_ok(path, CAP_FS_WRITE)) return -1;
    if (append && avfs_file_exists(path)) return n == 0 ? 0 : avfs_append_file(path, buf, n);
    return avfs_save_file(path, buf, n, true);
}

static int api_file_remove(const char *path) { return fs_ok(path, CAP_FS_WRITE) ? avfs_remove_file(path) : -1; }
static int api_make_dir(const char *path)    { return fs_ok(path, CAP_FS_WRITE) ? avfs_create_dir(path) : -1; }

#define DIR_CACHE_MAX 512
static DirectoryEntry g_dir_cache[DIR_CACHE_MAX];
static char g_dir_cache_path[160];
static int  g_dir_cache_n = -1;

static int api_dir_entry(const char *dir, int index, char name[64], uint32_t *size, int *is_dir)
{
    if (!fs_ok(dir, CAP_FS_READ)) return 0;
    if (index == 0 || g_dir_cache_n < 0 || strcmp(dir, g_dir_cache_path) != 0) {
        int n = avfs_list_directory(dir, g_dir_cache, DIR_CACHE_MAX);
        if (n < 0) { g_dir_cache_n = -1; return 0; }
        int kept = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(g_dir_cache[i].name, ".") == 0 || strcmp(g_dir_cache[i].name, "..") == 0) continue;
            if (kept != i) g_dir_cache[kept] = g_dir_cache[i];
            kept++;
        }
        g_dir_cache_n = kept;
        strncpy(g_dir_cache_path, dir, sizeof(g_dir_cache_path) - 1);
        g_dir_cache_path[sizeof(g_dir_cache_path) - 1] = '\0';
    }
    if (index >= g_dir_cache_n) return 0;
    strncpy(name, g_dir_cache[index].name, 63);
    name[63] = '\0';
    *is_dir = g_dir_cache[index].is_directory ? 1 : 0;
    *size = 0;
    return 1;
}

static int  api_mod_scan(void)                                  { return (g_caps & CAP_MODS) ? acmods_scan() : 0; }
static int  api_mod_info(int i, char n[32], char d[48], int *e) { return (g_caps & CAP_MODS) ? acmods_info(i, n, d, e) : 0; }
static int  api_mod_toggle(int i)                               { return (g_caps & CAP_MODS) ? acmods_toggle(i) : -1; }
static void api_mod_run(uint32_t *screen, uint32_t *a)          { if (g_caps & CAP_MODS) acmods_run(screen, a); }

// ---- loading ------------------------------------------------------------------------------------------------------

static int reserve_pages(uint32_t lo, uint32_t hi)
{
    for (uint32_t p = lo / PAGE_SIZE; p < hi / PAGE_SIZE; p++)
        if (!pmm_is_page_free(p)) return -1;
    for (uint32_t p = lo / PAGE_SIZE; p < hi / PAGE_SIZE; p++) pmm_mark_page_used(p);
    return 0;
}

// What Ctrl+C has to give back if it ends the app without letting it return (see app_abort_cleanup).
static struct { bool live; uint32_t lo, hi, pages; void *arena; } g_live;

static void release_pages(uint32_t lo, uint32_t hi)
{
    g_live.live = false;
    for (uint32_t p = lo / PAGE_SIZE; p < hi / PAGE_SIZE; p++) pmm_mark_page_free(p);
}

// Windows programs (port/win) are built for CPUs with SSE; the kernel itself never uses it, so turning it on for apps is safe.
static void enable_sse(void)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    if (!(edx & (1u << 25)) || !(edx & (1u << 24))) return;      // no SSE or no FXSAVE
    uint32_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 & ~(1u << 2)) | (1u << 1);                         // EM off, MP on
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1u << 9) | (1u << 10);                                // OSFXSR, OSXMMEXCPT
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

void app_command(int argc, char *argv[])
{
    uint32_t narrow = CAP_ALL;
    int fi = 1;
    for (; fi < argc && strncmp(argv[fi], "--", 2) == 0; fi++) {
        if (strcmp(argv[fi], "--sandbox") == 0) narrow &= CAP_FS_READ | CAP_GFX;
        else if (strncmp(argv[fi], "--caps=", 7) == 0) {
            uint32_t m;
            if (!parse_caps(argv[fi] + 7, &m)) { APP_ERR("app: unknown capability in %s (fs.read, fs.write, net, gfx, mods, all, none)\n", argv[fi]); shell_set_status(1); return; }
            narrow &= m;
        } else { APP_ERR("app: unknown option %s\n", argv[fi]); shell_set_status(1); return; }
    }
    if (fi >= argc) {
        APP_ERR("Usage: app [--sandbox] [--caps=fs.read,fs.write,net,gfx,mods] FILE [args...]   (a static ELF built with port/Makefile)\n");
        return;
    }
    char resolved[256];
    const char *path = cat_resolve_path(argv[fi], resolved, sizeof resolved) ? resolved : argv[fi];
    int fsize = file_size_of(path);
    if (fsize < 0) { APP_ERR("app: %s: no such file\n", path); return; }

    // capabilities: the default for the user, replaced by the policy line for this app, then narrowed by the options
    g_trusted = users_is_root();
    uint32_t caps = g_trusted ? CAP_ALL : (CAP_ALL & ~CAP_NET);
    char stem[24];
    {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        size_t n = 0;
        while (base[n] && base[n] != '.' && n < sizeof(stem) - 1) { stem[n] = (base[n] >= 'A' && base[n] <= 'Z') ? (char)(base[n] + 32) : base[n]; n++; }
        stem[n] = '\0';
        uint32_t pm;
        if (policy_caps(stem, &pm)) caps = pm;
    }
    g_caps = caps & narrow;
    g_denied = 0;
    if (g_caps != CAP_ALL) { print("app: capabilities: "); print_caps(g_caps); print("\n"); }

    elf_header_t eh;
    elf_phdr_t ph[16];
    if (read_at(path, 0, &eh, sizeof(eh)) != (int)sizeof(eh) || eh.ident[0] != 0x7F || eh.ident[1] != 'E' ||
        eh.ident[2] != 'L' || eh.ident[3] != 'F' || eh.ident[4] != 1 || eh.machine != 3 || eh.phnum == 0 || eh.phnum > 16 ||
        eh.phentsize != sizeof(elf_phdr_t)) {
        APP_ERR("app: %s is not a 32-bit x86 ELF\n", path);
        return;
    }
    if (read_at(path, eh.phoff, ph, (uint32_t)eh.phnum * sizeof(elf_phdr_t)) != (int)(eh.phnum * sizeof(elf_phdr_t))) {
        APP_ERR("app: cannot read the program headers\n");
        return;
    }

    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (int i = 0; i < eh.phnum; i++) {
        if (ph[i].type != 1 || ph[i].memsz == 0) continue;
        if (ph[i].vaddr < lo) lo = ph[i].vaddr;
        if (ph[i].vaddr + ph[i].memsz > hi) hi = ph[i].vaddr + ph[i].memsz;
    }
    lo &= ~(PAGE_SIZE - 1);
    hi = (hi + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (lo < APP_BASE || hi > APP_BASE + APP_IMAGE_MAX || hi <= lo) {
        APP_ERR("app: the image must be linked inside 0x%x-0x%x (see port/Makefile BASE)\n", APP_BASE, APP_BASE + APP_IMAGE_MAX);
        return;
    }
    if (reserve_pages(lo, hi) != 0) { APP_ERR("app: the memory at 0x%x is in use\n", lo); return; }
    g_live.live = true; g_live.lo = lo; g_live.hi = hi; g_live.arena = NULL; g_live.pages = 0;

    for (int i = 0; i < eh.phnum; i++) {
        if (ph[i].type != 1 || ph[i].memsz == 0) continue;
        uint8_t *dst = (uint8_t *)ph[i].vaddr;
        if (ph[i].filesz && read_at(path, ph[i].offset, dst, ph[i].filesz) != (int)ph[i].filesz) {
            APP_ERR("app: read error while loading %s\n", path);
            release_pages(lo, hi);
            return;
        }
        memset(dst + ph[i].filesz, 0, ph[i].memsz - ph[i].filesz);
    }

    // The arena: as much RAM as is free (up to 1.5 GB), minus some for the kernel.
    uint32_t pages = pmm_get_free_pages();
    pages = pages > 16384u ? (pages - 16384u) : 0;
    if (pages > ARENA_MAX_PAGES) pages = ARENA_MAX_PAGES;
    void *arena = NULL;
    while (pages >= 4096u && !(arena = pmm_alloc_pages(pages))) pages -= pages / 8;
    if (!arena) { APP_ERR("app: not enough free RAM (needs 16 MB)\n"); release_pages(lo, hi); return; }

    g_live.arena = arena; g_live.pages = pages;

    // Heap/stack base randomisation: the app's arena starts a random number of pages (up to 16 MB) into the block.
    uint32_t slide = 0, slide_max = pages / 16 > 4096u ? 4096u : pages / 16;
    if (slide_max) { uint32_t r = 0; rust_prp_random((uint8_t *)&r, sizeof r); slide = r % slide_max; }

    static radium_api_t api;
    api.version = RADIUM_API_VERSION;
    api.arena_base = (uint32_t)arena + slide * PAGE_SIZE;
    api.arena_size = (pages - slide) * PAGE_SIZE;
    extern size_t terminal_width, terminal_height;
    api.cols = (uint32_t)terminal_width;
    api.rows = (uint32_t)terminal_height;
    api.screen_blit = api_screen_blit;
    api.console_write = api_console_write;
    api.debug_write = api_debug_write;
    api.key_poll = api_key_poll;
    api.millis = api_millis;
    api.unix_time = api_unix_time;
    api.random32 = api_random32;
    api.sleep_ms = api_sleep_ms;
    api.tls_selector = api_tls_selector;
    api.file_stat = api_file_stat;
    api.file_read = api_file_read;
    api.file_write = api_file_write;
    api.file_remove = api_file_remove;
    api.dir_entry = api_dir_entry;
    api.make_dir = api_make_dir;
    api.gfx_open = api_gfx_open;
    api.gfx_present = api_gfx_present;
    api.gfx_close = api_gfx_close;
    api.mouse_poll = api_mouse_poll;
    api.net_udp_open = api_net_udp_open;
    api.net_udp_bind = udpsock_bind;
    api.net_udp_sendto = udpsock_sendto;
    api.net_udp_recvfrom = udpsock_recvfrom;
    api.net_udp_pending = udpsock_pending;
    api.net_udp_close = udpsock_close;
    api.net_resolve = api_net_resolve;
    api.net_local_ip = api_net_local_ip;
    api.mod_scan = api_mod_scan;
    api.mod_info = api_mod_info;
    api.mod_toggle = api_mod_toggle;
    api.mod_run = api_mod_run;
    api.fs_selector = api_fs_selector;
    g_dir_cache_n = -1;
    g_start_tsc = rdtsc64();

    execute_command_no_history("anim stop all");             // the overlays would cover the app's top row and corner
    g_gfx_used = 0;
    enable_sse();
    radium_app_main_fn entry = (radium_app_main_fn)eh.entry;
    if (strcmp(stem, "cdda") == 0 || strcmp(stem, "ac") == 0) ctrlc_disarm();   // games keep Ctrl for themselves
    int rc = entry(&api, argc - fi, argv + fi);
    udpsock_close_all();
    g_net_quiet = 0;

    __asm__ __volatile__("movw $0, %%ax\n\tmovw %%ax, %%gs\n\tmovw %%ax, %%fs" : : : "ax");
    g_live.arena = NULL;
    for (uint32_t i = 0; i < pages; i++) pmm_free_page((uint8_t *)arena + (size_t)i * PAGE_SIZE);
    release_pages(lo, hi);
    if (rc == 0 && g_gfx_used) terminal_clear();   // a graphics app leaves its last frame behind; text output and a crashed app's last screen stay readable
    execute_command_no_history("anim demo");
    printr("\napp: exited with status %d\n", rc);
    if (g_denied) printr("app: %u operation(s) were denied by the capability policy\n", g_denied);
    g_caps = CAP_ALL;
    g_trusted = true;
}

// Ctrl+C ended the running app without letting it return: give back what app_command would have.
void app_abort_cleanup(void)
{
    if (!g_live.live) return;
    udpsock_close_all();
    g_net_quiet = 0;
    __asm__ __volatile__("movw $0, %%ax\n\tmovw %%ax, %%gs\n\tmovw %%ax, %%fs" : : : "ax");
    api_gfx_close();
    if (g_live.arena)
        for (uint32_t i = 0; i < g_live.pages; i++) pmm_free_page((uint8_t *)g_live.arena + (size_t)i * PAGE_SIZE);
    g_live.arena = NULL;
    release_pages(g_live.lo, g_live.hi);
    execute_command_no_history("anim demo");
    g_caps = CAP_ALL;
    g_trusted = true;
    print("\napp: stopped\n");
}

// Capability parsing and denial (see selftest.c). 0 = ok, else the number of the failed check.
int app_selftest(void)
{
    uint32_t m = 0, keep_caps = g_caps;
    bool keep_trusted = g_trusted;
    int fail = 0;
    if (!parse_caps("fs.read,net", &m) || m != (CAP_FS_READ | CAP_NET)) fail = 1;
    else if (!parse_caps("all", &m) || m != CAP_ALL || !parse_caps("none", &m) || m != 0) fail = 2;
    else if (parse_caps("fs.read,warp", &m) || parse_caps("fs.read,", &m) == false) fail = 3;
    if (!fail) {
        char buf[8];
        uint32_t size; int dir;
        char name[64];
        g_trusted = true;
        g_caps = CAP_GFX;                                                   // no files, no network
        uint32_t keep_denied = g_denied;
        if (api_file_read("/disk/x", 0, buf, 1) != -1 || api_file_write("/disk/x", buf, 1, 0) != -1) fail = 4;
        else if (api_file_stat("/disk", &size, &dir) != -1 || api_dir_entry("/disk", 0, name, &size, &dir) != 0) fail = 5;
        else if (api_net_udp_open() != -1 || api_net_resolve("localhost", &size) != -1 || api_net_local_ip() != 0) fail = 6;
        else if (api_mod_scan() != 0 || api_mod_toggle(0) != -1) fail = 7;
        g_caps = CAP_FS_READ;
        if (!fail && (api_gfx_open(320, 200) != -1)) fail = 8;
        g_denied = keep_denied;
    }
    g_caps = keep_caps;
    g_trusted = keep_trusted;
    return fail;
}
