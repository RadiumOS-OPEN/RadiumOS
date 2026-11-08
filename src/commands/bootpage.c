// bootpage.c - the colourful boot page, and the end-of-boot sequence (startup script, prompt)
//
// boot_finish() runs at the end of every boot: it runs the startup script (the same thing `rrc` does),
// draws the boot page with the result, and shows the prompt. `bootpage` shows the page again.

#include <stdint.h>
#include <stdbool.h>
#include "bootpage.h"
#include "shellvars.h"
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
#include "../user/users.h"
#include "../kernel/cmdline.h"
#include "../io/io.h"
#include "../kernel/bootprofile.h"
#include "fsapp.h"
#include "update.h"
#include "rsd.h"

extern size_t terminal_column;
extern size_t terminal_width;
extern int rust_rtl8139_get_mac(uint8_t *mac_out);

// The panel is centred: 100 columns on a wide terminal, 78 on an 80-column one.
static size_t panel_w(void)     { return terminal_width >= 104 ? 100 : 78; }
static size_t panel_left(void)  { return (terminal_width - panel_w()) / 2; }
static size_t panel_right(void) { return panel_left() + panel_w() - 1; }   // column of the right-hand border
static void pad_to(size_t col)  { while (terminal_column < col) terminal_putchar(' '); }

#define COL_BORDER VGA_COLOR_LIGHT_CYAN
#define COL_LABEL  VGA_COLOR_LIGHT_MAGENTA
#define COL_TEXT   VGA_COLOR_WHITE
#define COL_DIM    VGA_COLOR_LIGHT_GREY
#define COL_GOOD   VGA_COLOR_LIGHT_GREEN
#define COL_WARN   VGA_COLOR_LIGHT_BROWN
#define COL_BAD    VGA_COLOR_LIGHT_RED

// Result of the startup script run by boot_finish(), shown on the page.
static int  g_rc_ran = -2;          // -2 not run yet, -1 no script, otherwise the number of commands
static bool g_rc_errors = false;
static char g_rc_path[64] = "";

// pmm_get_total_memory() is the usable RAM; the pages the allocator still has free are subtracted for "used".
static uint32_t mem_total_kb(void) { return (uint32_t)(pmm_get_total_memory() >> 10); }
static uint32_t mem_used_kb(void)
{
    uint32_t total = mem_total_kb(), free_kb = (uint32_t)(pmm_get_free_memory() >> 10);
    return free_kb >= total ? 0 : total - free_kb;
}

static void fg(uint8_t color) { terminal_setcolor(vga_entry_color(color, VGA_COLOR_BLACK)); }

// ---- colours --------------------------------------------------------------------

static const struct { const char *name; uint8_t color; } COLOR_NAMES[] = {
    { "black", VGA_COLOR_BLACK },           { "blue", VGA_COLOR_BLUE },
    { "green", VGA_COLOR_GREEN },           { "cyan", VGA_COLOR_CYAN },
    { "red", VGA_COLOR_RED },               { "magenta", VGA_COLOR_MAGENTA },
    { "brown", VGA_COLOR_BROWN },           { "grey", VGA_COLOR_LIGHT_GREY },
    { "gray", VGA_COLOR_LIGHT_GREY },       { "darkgrey", VGA_COLOR_DARK_GREY },
    { "darkgray", VGA_COLOR_DARK_GREY },    { "lightblue", VGA_COLOR_LIGHT_BLUE },
    { "lightgreen", VGA_COLOR_LIGHT_GREEN },{ "lightcyan", VGA_COLOR_LIGHT_CYAN },
    { "lightred", VGA_COLOR_LIGHT_RED },    { "lightmagenta", VGA_COLOR_LIGHT_MAGENTA },
    { "yellow", VGA_COLOR_LIGHT_BROWN },    { "white", VGA_COLOR_WHITE },
    { "good", COL_GOOD }, { "warn", COL_WARN }, { "bad", COL_BAD }, { "dim", COL_DIM },
    { "label", COL_LABEL }, { "border", COL_BORDER }, { "text", COL_TEXT },
};

static bool color_by_name(const char *name, uint8_t *out)
{
    for (size_t i = 0; i < sizeof(COLOR_NAMES) / sizeof(COLOR_NAMES[0]); i++) {
        if (strcmp(name, COLOR_NAMES[i].name) == 0) { *out = COLOR_NAMES[i].color; return true; }
    }
    return false;
}

// Set once any screen command (banner, center, box) has drawn something, so boot_finish() knows the
// startup script provides its own boot screen and skips the built-in one.
static bool g_screen_drawn = false;

#define SCREEN_ERR(...) do { fg(COL_BAD); printr(__VA_ARGS__); fg(COL_TEXT); } while (0)

// ---- banner (5x5 block letters) ---------------------------------------------------

#define GLYPH_ROWS  5
#define GLYPH_WIDTH 5

static const char FONT_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.!:";

static const char *const FONT[][GLYPH_ROWS] = {
    { ".###.", "#...#", "#####", "#...#", "#...#" },   // A
    { "####.", "#...#", "####.", "#...#", "####." },   // B
    { ".####", "#....", "#....", "#....", ".####" },   // C
    { "####.", "#...#", "#...#", "#...#", "####." },   // D
    { "#####", "#....", "###..", "#....", "#####" },   // E
    { "#####", "#....", "###..", "#....", "#...." },   // F
    { ".####", "#....", "#..##", "#...#", ".###." },   // G
    { "#...#", "#...#", "#####", "#...#", "#...#" },   // H
    { "#####", "..#..", "..#..", "..#..", "#####" },   // I
    { "..###", "...#.", "...#.", "#..#.", ".##.." },   // J
    { "#...#", "#..#.", "###..", "#..#.", "#...#" },   // K
    { "#....", "#....", "#....", "#....", "#####" },   // L
    { "#...#", "##.##", "#.#.#", "#...#", "#...#" },   // M
    { "#...#", "##..#", "#.#.#", "#..##", "#...#" },   // N
    { ".###.", "#...#", "#...#", "#...#", ".###." },   // O
    { "####.", "#...#", "####.", "#....", "#...." },   // P
    { ".###.", "#...#", "#.#.#", "#..#.", ".##.#" },   // Q
    { "####.", "#...#", "####.", "#..#.", "#...#" },   // R
    { ".####", "#....", ".###.", "....#", "####." },   // S
    { "#####", "..#..", "..#..", "..#..", "..#.." },   // T
    { "#...#", "#...#", "#...#", "#...#", ".###." },   // U
    { "#...#", "#...#", "#...#", ".#.#.", "..#.." },   // V
    { "#...#", "#...#", "#.#.#", "##.##", "#...#" },   // W
    { "#...#", ".#.#.", "..#..", ".#.#.", "#...#" },   // X
    { "#...#", ".#.#.", "..#..", "..#..", "..#.." },   // Y
    { "#####", "...#.", "..#..", ".#...", "#####" },   // Z
    { ".###.", "#..##", "#.#.#", "##..#", ".###." },   // 0
    { "..#..", ".##..", "..#..", "..#..", ".###." },   // 1
    { ".###.", "#...#", "..##.", ".#...", "#####" },   // 2
    { "####.", "....#", ".###.", "....#", "####." },   // 3
    { "#..#.", "#..#.", "#####", "...#.", "...#." },   // 4
    { "#####", "#....", "####.", "....#", "####." },   // 5
    { ".###.", "#....", "####.", "#...#", ".###." },   // 6
    { "#####", "....#", "...#.", "..#..", "..#.." },   // 7
    { ".###.", "#...#", ".###.", "#...#", ".###." },   // 8
    { ".###.", "#...#", ".####", "....#", ".###." },   // 9
    { ".....", ".....", "#####", ".....", "....." },   // -
    { ".....", ".....", ".....", ".....", "..#.." },   // .
    { "..#..", "..#..", "..#..", ".....", "..#.." },   // !
    { ".....", "..#..", ".....", "..#..", "....." },   // :
};

static const uint8_t RAINBOW[] = {
    VGA_COLOR_LIGHT_RED, VGA_COLOR_LIGHT_BROWN, VGA_COLOR_LIGHT_GREEN, VGA_COLOR_LIGHT_CYAN,
    VGA_COLOR_LIGHT_BLUE, VGA_COLOR_LIGHT_MAGENTA,
};
#define RAINBOW_COUNT ((int)(sizeof(RAINBOW) / sizeof(RAINBOW[0])))

static int glyph_index(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (int i = 0; FONT_CHARS[i]; i++) if (FONT_CHARS[i] == c) return i;
    return -1;   // space and anything unknown
}

// Draws `text` in block letters, centred. color < 0 means rainbow.
static void render_banner(const char *text, int color)
{
    size_t len = strlen(text);
    size_t width = 0;
    size_t max_chars = len;

    for (size_t i = 0; i < len; i++) {
        size_t w = (glyph_index(text[i]) < 0) ? 3 : GLYPH_WIDTH;
        if (width + w + (width ? 1 : 0) > terminal_width) { max_chars = i; break; }
        width += w + (width ? 1 : 0);
    }
    size_t pad = (terminal_width - width) / 2;

    for (int row = 0; row < GLYPH_ROWS; row++) {
        for (size_t i = 0; i < pad; i++) terminal_putchar(' ');
        int letter = 0;
        for (size_t i = 0; i < max_chars; i++) {
            int g = glyph_index(text[i]);
            if (i > 0) terminal_putchar(' ');
            if (g < 0) { for (int k = 0; k < 3; k++) terminal_putchar(' '); continue; }
            fg(color >= 0 ? (uint8_t)color : RAINBOW[letter % RAINBOW_COUNT]);
            letter++;
            for (int col = 0; col < GLYPH_WIDTH; col++)
                terminal_putchar(FONT[g][row][col] == '#' ? '\xDB' : ' ');
        }
        terminal_putchar('\n');
    }
    g_screen_drawn = true;
}

static void print_centered_text(const char *text, uint8_t color)
{
    size_t len = strlen(text);
    for (size_t i = 0; i < (len < terminal_width ? (terminal_width - len) / 2 : 0); i++) terminal_putchar(' ');
    fg(color);
    print(text);
    terminal_putchar('\n');
}

// ---- panel ----------------------------------------------------------------------

static void panel_top(const char *title)
{
    pad_to(panel_left());
    fg(COL_BORDER);
    terminal_putchar('\xC9');
    terminal_putchar('\xCD');
    terminal_putchar('[');
    fg(COL_TEXT);
    print(" ");
    print(title);
    print(" ");
    fg(COL_BORDER);
    terminal_putchar(']');
    while (terminal_column < panel_right()) terminal_putchar('\xCD');
    terminal_putchar('\xBB');
    terminal_putchar('\n');
}

static void panel_bottom(void)
{
    pad_to(panel_left());
    fg(COL_BORDER);
    terminal_putchar('\xC8');
    for (size_t i = 1; i < panel_w() - 1; i++) terminal_putchar('\xCD');
    terminal_putchar('\xBC');
    terminal_putchar('\n');
}

static void row_begin(const char *label)
{
    pad_to(panel_left());
    fg(COL_BORDER);
    terminal_putchar('\xBA');
    print(" ");
    fg(COL_LABEL);
    print(label);
    for (size_t n = strlen(label); n < 10; n++) print(" ");
    fg(COL_TEXT);
}

static void row_end(void)
{
    while (terminal_column < panel_right()) terminal_putchar(' ');
    fg(COL_BORDER);
    terminal_putchar('\xBA');
    terminal_putchar('\n');
}

static void print_bar_pct(uint32_t pct)
{
    const uint32_t cells = 20;
    if (pct > 100) pct = 100;
    uint32_t filled = pct * cells / 100;
    uint8_t color = pct < 60 ? COL_GOOD : (pct < 85 ? COL_WARN : COL_BAD);

    fg(COL_DIM);
    print("[");
    fg(color);
    for (uint32_t i = 0; i < cells; i++) terminal_putchar(i < filled ? '\xDB' : '\xB0');
    fg(COL_DIM);
    print("] ");
    fg(color);
    printr("%u%%", pct);
    fg(COL_TEXT);
}

static uint32_t percent_of(uint32_t used, uint32_t total)
{
    if (!total) return 0;
    if (used >= total) return 100;
    return used > 40000000u ? used / (total / 100 + 1) : used * 100u / total;
}

static void print_bar(uint32_t used, uint32_t total)
{
    print_bar_pct(percent_of(used, total));
}

static void current_user(char *name, size_t size)
{
    strncpy(name, users_current_name(), size - 1);
    name[size - 1] = '\0';
}

// ---- the page -------------------------------------------------------------------

void show_boot_page(void)
{
    terminal_clear();
    print("\n");
    render_banner("RADIUMOS", -1);
    print("\n");
    print_centered_text("A small operating system you can actually use", COL_DIM);
    print("\n");

    panel_top("System");

    uint32_t days, hours, mins, secs, ms;
    get_uptime_precise(&days, &hours, &mins, &secs, &ms);
    row_begin("System");
    print("RadiumOS  i686, 32-bit protected mode");
    fg(COL_DIM);
    printr("   up %u:%02u:%02u", days * 24 + hours, mins, secs);
    row_end();

    row_begin("CPU");
    const char *brand = get_cpu_brand_string();
    char brand_text[64];
    strncpy(brand_text, (brand && brand[0]) ? brand : "unknown processor", sizeof(brand_text) - 1);
    brand_text[sizeof(brand_text) - 1] = '\0';
    print(brand_text);
    row_end();

    row_begin("Memory");
    uint32_t total_kb = mem_total_kb();
    uint32_t used_kb  = mem_used_kb();
    printr("%u of %u MB   ", used_kb / 1024, total_kb / 1024);
    print_bar(used_kb, total_kb);
    row_end();

    row_begin("Disk");
    avfs_disk_info_t disk;
    if (avfs_disk_query(&disk) == 0 && disk.mounted) {
        uint32_t d_total = disk.total_kb, d_used = disk.used_kb;
        printr("/disk  %u of %u MB   ", d_used / 1024, d_total / 1024);
        print_bar(d_used, d_total);
    } else {
        fg(COL_WARN);
        print("/disk is not mounted - files live in RAM and are lost on reboot");
    }
    row_end();

    row_begin("Network");
    uint8_t mac[6];
    if (rust_rtl8139_get_mac(mac) == 0) {
        fg(COL_GOOD);
        print("RTL8139 ready");
        fg(COL_DIM);
        printr("   MAC %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        fg(COL_WARN);
        print("no network card found");
    }
    row_end();

    row_begin("User");
    char user[48];
    current_user(user, sizeof(user));
    datetime_t now;
    rtc_get_time(&now);
    printr("%s@radiumos", user);
    fg(COL_DIM);
    printr("   %04u-%02u-%02u %02u:%02u", now.year, now.month, now.day, now.hour, now.minute);
    row_end();

    row_begin("Startup");
    if (g_rc_ran >= 0) {
        fg(g_rc_errors ? COL_WARN : COL_GOOD);
        printr("%s ran %d command%s", g_rc_path, g_rc_ran, g_rc_ran == 1 ? "" : "s");
        if (g_rc_errors) print(" (with errors)");
    } else if (g_rc_ran == -1) {
        fg(COL_WARN);
        print("no .radiumrc found (create /disk/.radiumrc, then run rrc)");
    } else {
        fg(COL_DIM);
        print("not run yet - use rrc");
    }
    row_end();

    row_begin("Security");
    if (!sudo_is_configured()) {
        fg(COL_WARN);
        print("sudo is not set up - run cpw to create a sudo password");
    } else if (sudo_is_unlocked()) {
        fg(COL_WARN);
        print("sudo is unlocked");
    } else {
        fg(COL_GOOD);
        print("sudo is locked");
        fg(COL_DIM);
        print("   password sealed with PRP in /disk/.sudoInfo");
    }
    row_end();

    panel_bottom();

    print("\n");
    fg(COL_LABEL);
    print(" Try: ");
    fg(COL_TEXT);
    print("help");
    fg(COL_DIM); print("  commands   ");
    fg(COL_TEXT); print("search");
    fg(COL_DIM); print("  web search   ");
    fg(COL_TEXT); print("notes");
    fg(COL_DIM); print("  notes   ");
    fg(COL_TEXT); print("rrc");
    fg(COL_DIM); print("  reload startup script\n");
    fg(COL_DIM);
    print(" Tab completes commands and files. sudo <command> runs privileged commands.\n\n");
    fg(VGA_COLOR_LIGHT_GREY);
}

// ---- variables for startup scripts -----------------------------------------------
//
// Expanded by the shell like any $NAME (a variable you export yourself takes precedence).

typedef struct { char *p; size_t cap; size_t len; } vb_t;

static void vb_str(vb_t *b, const char *s)
{
    while (*s && b->len + 1 < b->cap) b->p[b->len++] = *s++;
    b->p[b->len] = '\0';
}

static void vb_uint(vb_t *b, uint32_t v, int min_digits)
{
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n < min_digits && n < 11) tmp[n++] = '0';
    while (n > 0 && b->len + 1 < b->cap) b->p[b->len++] = tmp[--n];
    b->p[b->len] = '\0';
}

static char g_vbuf[6][112];
static int  g_vnext = 0;

const char *bootpage_var(const char *name)
{
    char *slot = g_vbuf[g_vnext];
    vb_t b = { slot, sizeof(g_vbuf[0]), 0 };
    slot[0] = '\0';

    if (strcmp(name, "OS") == 0) vb_str(&b, "RadiumOS");
    else if (strcmp(name, "HOSTNAME") == 0) vb_str(&b, "radiumos");
    else if (strcmp(name, "USER") == 0) {
        char user[48];
        current_user(user, sizeof(user));
        vb_str(&b, user);
    }
    else if (strcmp(name, "CPU") == 0) {
        const char *brand = get_cpu_brand_string();
        while (brand && *brand == ' ') brand++;
        vb_str(&b, (brand && *brand) ? brand : "unknown processor");
    }
    else if (strcmp(name, "MEM_USED") == 0)  vb_uint(&b, (mem_used_kb() >> 10), 0);
    else if (strcmp(name, "MEM_USED_KB") == 0) vb_uint(&b, mem_used_kb(), 0);
    else if (strcmp(name, "MEM_TOTAL") == 0) vb_uint(&b, (mem_total_kb() >> 10), 0);
    else if (strcmp(name, "MEM_PCT") == 0)
        vb_uint(&b, percent_of(mem_used_kb(), mem_total_kb()), 0);
    else if (strcmp(name, "MEM_INFO") == 0) {
        vb_uint(&b, (mem_used_kb() >> 10), 0);
        vb_str(&b, " of ");
        vb_uint(&b, (mem_total_kb() >> 10), 0);
        vb_str(&b, " MB");
    }
    else if (strncmp(name, "DISK", 4) == 0) {
        avfs_disk_info_t disk;
        bool mounted = (avfs_disk_query(&disk) == 0 && disk.mounted);
        uint32_t used = mounted ? disk.used_kb : 0, total = mounted ? disk.total_kb : 0;
        if (strcmp(name, "DISK_USED") == 0)       vb_uint(&b, used >> 10, 0);
        else if (strcmp(name, "DISK_TOTAL") == 0) vb_uint(&b, total >> 10, 0);
        else if (strcmp(name, "DISK_PCT") == 0)   vb_uint(&b, percent_of(used, total), 0);
        else if (strcmp(name, "DISK") == 0)       vb_str(&b, mounted ? "mounted" : "not mounted");
        else if (strcmp(name, "DISK_COLOR") == 0) vb_str(&b, mounted ? "text" : "warn");
        else if (strcmp(name, "DISK_INFO") == 0) {
            if (mounted) {
                vb_str(&b, "/disk  ");
                vb_uint(&b, used >> 10, 0);
                vb_str(&b, " of ");
                vb_uint(&b, total >> 10, 0);
                vb_str(&b, " MB");
            } else {
                vb_str(&b, "/disk is not mounted - files live in RAM and are lost on reboot");
            }
        }
        else return NULL;
    }
    else if (strcmp(name, "NET") == 0 || strcmp(name, "NET_COLOR") == 0 || strcmp(name, "MAC") == 0) {
        uint8_t mac[6];
        bool up = (rust_rtl8139_get_mac(mac) == 0);
        if (strcmp(name, "NET") == 0) vb_str(&b, up ? "RTL8139 ready" : "no network card found");
        else if (strcmp(name, "NET_COLOR") == 0) vb_str(&b, up ? "good" : "warn");
        else if (up) {
            static const char hex[] = "0123456789abcdef";
            for (int i = 0; i < 6; i++) {
                char two[4] = { hex[mac[i] >> 4], hex[mac[i] & 15], i < 5 ? ':' : '\0', '\0' };
                vb_str(&b, two);
            }
        } else vb_str(&b, "none");
    }
    else if (strcmp(name, "UPTIME") == 0) {
        uint32_t days, hours, mins, secs, ms;
        get_uptime_precise(&days, &hours, &mins, &secs, &ms);
        vb_uint(&b, days * 24 + hours, 0); vb_str(&b, ":");
        vb_uint(&b, mins, 2);              vb_str(&b, ":");
        vb_uint(&b, secs, 2);
    }
    else if (strcmp(name, "DATE") == 0 || strcmp(name, "TIME") == 0 || strcmp(name, "NOW") == 0) {
        datetime_t now;
        rtc_get_time(&now);
        if (name[0] != 'T') {
            vb_uint(&b, now.year, 4);  vb_str(&b, "-");
            vb_uint(&b, now.month, 2); vb_str(&b, "-");
            vb_uint(&b, now.day, 2);
        }
        if (name[0] == 'N') vb_str(&b, " ");
        if (name[0] != 'D') {
            vb_uint(&b, now.hour, 2); vb_str(&b, ":");
            vb_uint(&b, now.minute, 2);
        }
    }
    else if (strcmp(name, "SUDO") == 0 || strcmp(name, "SUDO_COLOR") == 0) {
        bool want_color = (name[4] == '_');
        if (!sudo_is_configured())      vb_str(&b, want_color ? "warn" : "not set up - run cpw to create a sudo password");
        else if (sudo_is_unlocked())    vb_str(&b, want_color ? "warn" : "unlocked");
        else                            vb_str(&b, want_color ? "good" : "locked, password sealed with PRP");
    }
    else if (strcmp(name, "RC_PATH") == 0) vb_str(&b, g_rc_path[0] ? g_rc_path : "(none)");
    else return NULL;

    g_vnext = (g_vnext + 1) % 6;
    return slot;
}

// ---- screen commands for startup scripts ------------------------------------------
//
//   color NAME                              text colour for what follows (grey resets)
//   banner [-c COLOR] TEXT...               big block letters, rainbow unless -c is given
//   center [-c COLOR] TEXT...               one centred line
//   box top TITLE                           top border with a title
//   box row LABEL TEXT... [--color NAME] [--bar PERCENT]
//   box bottom                              closing border

static void screen_usage_banner(void) { SCREEN_ERR("Usage: banner [-c COLOR] TEXT...\n"); }

static int parse_color_opt(int argc, char *argv[], int *first_text, int *color_out)
{
    *first_text = 1;
    *color_out = -1;
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        uint8_t c;
        if (strcmp(argv[2], "rainbow") != 0) {
            if (!color_by_name(argv[2], &c)) { SCREEN_ERR("unknown colour: %s\n", argv[2]); return -1; }
            *color_out = c;
        }
        *first_text = 3;
    }
    return 0;
}

static void join_text(int argc, char *argv[], int start, char *out, size_t size)
{
    size_t o = 0;
    for (int i = start; i < argc; i++) {
        if (i > start && o + 1 < size) out[o++] = ' ';
        for (const char *p = argv[i]; *p && o + 1 < size; p++) out[o++] = *p;
    }
    out[o] = '\0';
    // Quotes are optional here: "two words" and two words print the same.
    if (o >= 2 && (out[0] == '"' || out[0] == '\'') && out[o - 1] == out[0]) {
        memmove(out, out + 1, o - 2);
        out[o - 2] = '\0';
    }
}

void banner_command(int argc, char *argv[])
{
    int first, color;
    char text[96];
    if (parse_color_opt(argc, argv, &first, &color) != 0) return;
    if (first >= argc) { screen_usage_banner(); return; }
    join_text(argc, argv, first, text, sizeof(text));
    render_banner(text, color);
    fg(COL_TEXT);
}

void center_command(int argc, char *argv[])
{
    int first, color;
    char text[96];
    if (parse_color_opt(argc, argv, &first, &color) != 0) return;
    if (first > argc) return;
    join_text(argc, argv, first, text, sizeof(text));
    print_centered_text(text, color >= 0 ? (uint8_t)color : COL_TEXT);
    g_screen_drawn = true;
    fg(COL_TEXT);
}

void color_command(int argc, char *argv[])
{
    uint8_t c;
    if (argc != 2 || !color_by_name(argv[1], &c)) {
        SCREEN_ERR("Usage: color NAME   (black blue green cyan red magenta brown grey darkgrey lightblue\n"
                   "                      lightgreen lightcyan lightred lightmagenta yellow white)\n");
        return;
    }
    fg(c);
}

static void print_clipped(const char *text)
{
    for (; *text && terminal_column < panel_right() - 1; text++) terminal_putchar(*text);
}

void box_command(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "top") == 0) {
        char title[64];
        join_text(argc, argv, 2, title, sizeof(title));
        panel_top(title);
        g_screen_drawn = true;
        fg(COL_TEXT);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "bottom") == 0) {
        panel_bottom();
        g_screen_drawn = true;
        fg(COL_TEXT);
        return;
    }
    if (argc >= 3 && strcmp(argv[1], "row") == 0) {
        const char *label = argv[2];
        char text[112];
        int text_end = 3;
        uint8_t color = COL_TEXT;
        int bar = -1;

        while (text_end < argc && strncmp(argv[text_end], "--", 2) != 0) text_end++;
        join_text(text_end, argv, 3, text, sizeof(text));

        for (int i = text_end; i < argc; i++) {
            if (strcmp(argv[i], "--color") == 0 && i + 1 < argc) {
                if (!color_by_name(argv[++i], &color)) { SCREEN_ERR("box: unknown colour: %s\n", argv[i]); return; }
            } else if (strcmp(argv[i], "--bar") == 0 && i + 1 < argc) {
                bar = 0;
                for (const char *c = argv[++i]; *c >= '0' && *c <= '9'; c++) bar = bar * 10 + (*c - '0');
            } else {
                SCREEN_ERR("box: unexpected %s\n", argv[i]);
                return;
            }
        }

        row_begin(label);
        fg(color);
        print_clipped(text);
        if (bar >= 0) {
            print_clipped("   ");
            print_bar_pct((uint32_t)bar);
        }
        row_end();
        g_screen_drawn = true;
        fg(COL_TEXT);
        return;
    }
    SCREEN_ERR("Usage: box top TITLE | box row LABEL TEXT... [--color NAME] [--bar PERCENT] | box bottom\n");
}

// ---- boot --------------------------------------------------------------------------

// Clears the screen and runs the startup script, which normally draws the boot screen itself.
// If it draws nothing (no script, or an older script without screen commands) the built-in page is shown.
static void run_startup_screen(void)
{
    const char *path = (cmdline_has("safe") || cmdline_has("norc")) ? NULL : shell_rc_path();   // built-in page only

    g_rc_ran = -1;
    g_rc_errors = false;
    g_rc_path[0] = '\0';
    g_screen_drawn = false;

    terminal_clear();
    if (path) {
        strncpy(g_rc_path, path, sizeof(g_rc_path) - 1);
        g_rc_path[sizeof(g_rc_path) - 1] = '\0';
        terminal_error_flag_take();
        g_rc_ran = shell_run_script(path);
        g_rc_errors = terminal_error_flag_take() != 0;
    }

    if (!g_screen_drawn) show_boot_page();
    fg(VGA_COLOR_LIGHT_GREY);
}

// Repair actions requested on the kernel command line (GRUB: Repair options).
static void run_boot_actions(void)
{
    if (cmdline_has("selftest")) {            // headless regression run: tools/test/run_tests.py
        execute_command_no_history("selftest");
        avfs_mark_clean();
        outw(0x604, 0x2000);
        for (;;) __asm__ volatile("hlt");
    }
    if (cmdline_has("autorollback")) execute_command_no_history("update rollback");
    if (cmdline_has("autoupdate"))
        execute_command_no_history(avfs_file_exists("/disk/update/manifest.txt") ? "update install" : "update install online");
}

#define UPDATE_ASK_MS 20000

// Asks the update server (one small request) whether a newer version than the current one is out.
static void offer_update(void)
{
    uint8_t mac[6];
    char version[48];
    if (cmdline_has("autoupdate") || cmdline_has("fastboot") || cmdline_has("safe") || cmdline_has("nonet")) return;
    if (!avfs_is_directory("/disk") || rust_rtl8139_get_mac(mac) != 0) return;

    fg(VGA_COLOR_DARK_GREY);
    print("  Checking for updates...\n");
    if (!update_online_available(version, sizeof(version))) return;

    print("\n");
    fg(VGA_COLOR_LIGHT_BROWN);
    print("  >>> HEY! A FRESH UPDATE IS OUT <<<\n");
    fg(VGA_COLOR_WHITE);
    printr("  v%s is out (you're on v%s). ", version, update_current_version());
    print("Wanna grab it now, or nah?\n\n");
    fg(VGA_COLOR_LIGHT_GREEN);
    print("  [N] Update now   ");
    fg(VGA_COLOR_LIGHT_CYAN);
    print("[L] Later");
    fg(VGA_COLOR_LIGHT_GREY);
    print("   (no answer = later; we'll ask again within the hour)\n");

    for (;;) {
        uint8_t key = fs_wait(UPDATE_ASK_MS);
        if (key == 0x31 || key == 0x1C) {          // N or Enter
            print("\n  Alright, updating!\n");
            execute_command_no_history("update install online");
            return;
        }
        if (key == 0 || key == 0x26 || key == FS_KEY_ESC) break;   // timeout, L or Esc
    }
    print("\n  Cool, later it is. Run `update install online` whenever you're ready.\n");
}

void boot_finish(void)
{
    update_refresh_marker();
    run_startup_screen();
    boot_mark("startup-script");
    rsd_boot();
    boot_mark("services");
    offer_update();
    boot_mark("update-check");
    run_boot_actions();
    shell_show_prompt();
}

// bootpage [-b]: run the startup script's boot screen again; -b shows the built-in page instead
void bootpage_command(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "-b") == 0) {
        show_boot_page();
        return;
    }
    if (argc > 1) {
        SCREEN_ERR("Usage: bootpage [-b]\n");
        return;
    }
    run_startup_screen();
}
