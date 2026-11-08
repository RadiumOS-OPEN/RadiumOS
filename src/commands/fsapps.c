// fsapps.c - full-screen programs built on fsapp.h. They size themselves from the terminal grid, so they fill
// the screen at any resolution.
//
//   sysmon     live system monitor: system, memory history, disk, services (systemctl) and the journal
//   life       Conway's Game of Life on the whole screen (Space pause, R reseed, +/- speed)
//   matrix     falling-glyph rain
//   resinfo    what the scaler currently derived from the terminal size
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "fsapp.h"
#include "rsd.h"
#include "bootpage.h"
#include "../terminal/terminal.h"
#include "../vga/fbcon.h"
#include "../vga/scaler.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../kernel/cmdline.h"
#include "../kernel/createFiles.h"

extern const char *kernel_boot_source(void);

#define KEY_SPACE 0x39
#define KEY_P     0x19
#define KEY_R     0x13
#define KEY_PLUS  0x0D   // = / +
#define KEY_MINUS 0x0C

enum { BLACK, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, GREY, DGREY, LBLUE, LGREEN, LCYAN, LRED, LMAGENTA, YELLOW, WHITE };

static uint32_t g_rng = 2463534242u;

static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static void seed_rng(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    g_rng = (lo ^ (hi * 2654435761u)) | 1u;
}

static void var(char *out, size_t size, const char *name)
{
    const char *v = bootpage_var(name);
    strncpy(out, v ? v : "?", size - 1);
    out[size - 1] = '\0';
}

// Text cut off at max_w columns.
static void text_clip(int x, int y, const char *s, int max_w, uint8_t attr)
{
    for (int i = 0; s[i] && i < max_w; i++) fs_put(x + i, y, s[i], attr);
}

static int to_pct(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v > 100 ? 100 : v;
}

static uint32_t to_uint(const char *s)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}

// ---- sysmon -----------------------------------------------------------------------------------------

#define BODY    fs_attr(GREY, BLACK)
#define BORDER  fs_attr(LCYAN, BLACK)
#define LABEL   fs_attr(CYAN, BLACK)
#define TEXT    fs_attr(WHITE, BLACK)

static void panel(int x, int y, int w, int h, const char *title)
{
    fs_fill(x, y, w, h, ' ', BODY);
    fs_box(x, y, w, h, BORDER, title);
}

static void kv(int x, int y, int w, const char *key, const char *value)
{
    text_clip(x, y, key, 11, LABEL);
    text_clip(x + 12, y, value, w - 12, TEXT);
}

static uint8_t pct_color(int pct)
{
    return pct < 60 ? fs_attr(LGREEN, BLACK) : (pct < 85 ? fs_attr(YELLOW, BLACK) : fs_attr(LRED, BLACK));
}

static void draw_bar_row(int x, int y, int w, const char *label, int pct, const char *detail)
{
    char num[8];
    snprintf(num, sizeof(num), "%d%%", pct);
    text_clip(x, y, label, 5, LABEL);
    int bar_w = w - 5 - 6;
    if (bar_w < 4) return;
    fs_bar(x + 5, y, bar_w, pct, pct_color(pct), fs_attr(DGREY, BLACK));
    text_clip(x + 5 + bar_w + 1, y, num, 5, TEXT);
    if (detail && detail[0]) text_clip(x + 5, y + 1, detail, w - 5, LABEL);
}

static void draw_history(int x, int y, int w, int h, const uint32_t *data, int count, uint32_t scale)
{
    for (int col = 0; col < w; col++) {
        int idx = count - w + col;
        int pct = idx >= 0 ? (int)(data[idx] * 100u / scale) : -1;
        int filled = pct < 0 ? 0 : (pct * h + 99) / 100;
        for (int row = 0; row < h; row++) {
            int from_bottom = h - 1 - row;
            if (from_bottom < filled) fs_put(x + col, y + row, (char)0xDB, pct_color(pct));
            else fs_put(x + col, y + row, (from_bottom == 0 ? (char)0xC4 : ' '), LABEL);
        }
    }
}

static void draw_services(int x, int y, int w, int h)
{
    panel(x, y, w, h, "Services");
    int rows = h - 3;
    text_clip(x + 2, y + 1, "UNIT", 12, LABEL);
    text_clip(x + 15, y + 1, "STATE", 9, LABEL);
    text_clip(x + 25, y + 1, "TARGET", 11, LABEL);
    text_clip(x + 37, y + 1, "RUNS", 5, LABEL);
    text_clip(x + 43, y + 1, "DESCRIPTION", w - 45, LABEL);

    rsd_unit_view_t u;
    int shown = 0;
    for (int i = 0; shown < rows && rsd_unit_view(i, &u); i++, shown++) {
        uint8_t state = u.failed ? fs_attr(LRED, BLACK) : (u.running ? fs_attr(LGREEN, BLACK) : fs_attr(DGREY, BLACK));
        char runs[8];
        snprintf(runs, sizeof(runs), "%u", u.runs);
        fs_put(x + 2, y + 2 + shown, u.failed ? 'x' : (u.running ? '*' : 'o'), state);
        text_clip(x + 4, y + 2 + shown, u.name, 10, TEXT);
        text_clip(x + 15, y + 2 + shown, u.state, 9, state);
        text_clip(x + 25, y + 2 + shown, u.target, 11, BODY);
        text_clip(x + 37, y + 2 + shown, runs, 5, BODY);
        text_clip(x + 43, y + 2 + shown, u.desc, w - 45, BODY);
    }
}

static void draw_journal(int x, int y, int w, int h)
{
    panel(x, y, w, h, "Journal");
    int rows = h - 2;
    uint32_t ms;
    char unit[16], text[112], line[160];
    int level;

    // oldest line shown first, from the top; once the panel is full the newest line is at the bottom
    int back = rows - 1;
    while (back >= 0 && !rsd_journal_view(back, &ms, unit, sizeof(unit), text, sizeof(text), &level)) back--;
    for (int row = 0; back >= 0 && row < rows; row++, back--) {
        if (!rsd_journal_view(back, &ms, unit, sizeof(unit), text, sizeof(text), &level)) break;
        snprintf(line, sizeof(line), "[%5u.%03u] %s: %s", ms / 1000, ms % 1000, unit, text);
        text_clip(x + 2, y + 1 + row, line, w - 4, level ? fs_attr(LRED, BLACK) : BODY);
    }
}

void sysmon_command(int argc, char *argv[])
{
    (void)argc; (void)argv;
    if (!fs_begin()) return;

    static uint32_t mem_hist[FBCON_COLS * 2];   // KB in use, one sample per refresh
    int hist_n = 0;
    bool paused = false;

    for (;;) {
        int C = fs_cols(), R = fs_rows();
        char up[24], mem_pct[8], mem_kb[16], mem_info[40], disk_pct[8], disk_info[96], cpu[64], host[24], user[32], net[40];
        var(up, sizeof(up), "UPTIME");
        var(mem_pct, sizeof(mem_pct), "MEM_PCT");
        var(mem_kb, sizeof(mem_kb), "MEM_USED_KB");
        var(mem_info, sizeof(mem_info), "MEM_INFO");
        var(disk_pct, sizeof(disk_pct), "DISK_PCT");
        var(disk_info, sizeof(disk_info), "DISK_INFO");
        var(cpu, sizeof(cpu), "CPU");
        var(host, sizeof(host), "HOSTNAME");
        var(user, sizeof(user), "USER");
        var(net, sizeof(net), "NET");

        if (!paused) {
            if (hist_n == (int)(sizeof(mem_hist) / sizeof(mem_hist[0]))) {
                for (int i = 1; i < hist_n; i++) mem_hist[i - 1] = mem_hist[i];
                hist_n--;
            }
            mem_hist[hist_n++] = (uint32_t)to_uint(mem_kb);
        }

        fs_clear(BODY);

        // header and footer bars
        fs_fill(0, 0, C, 1, ' ', fs_attr(WHITE, BLUE));
        text_clip(1, 0, " RadiumOS System Monitor ", C - 2, fs_attr(YELLOW, BLUE));
        char head[64];
        snprintf(head, sizeof(head), "%s@%s  up %s%s ", user, host, up, paused ? "  [paused]" : "");
        fs_text_right(C - 2, 0, head, fs_attr(WHITE, BLUE));
        fs_fill(0, R - 1, C, 1, ' ', fs_attr(BLACK, GREY));
        text_clip(1, R - 1, "Q/Esc quit   P pause   (units: systemctl, log: journalctl)", C - 2, fs_attr(BLACK, GREY));

        int left_w = C / 2 < 44 ? C : C / 2;
        bool two_cols = left_w < C;
        int right_x = two_cols ? left_w : 0, right_w = two_cols ? C - left_w : C;

        // system
        int sys_h = 11;
        panel(0, 1, left_w, sys_h, "System");
        kv(2, 2, left_w - 4, "OS", "RadiumOS");
        kv(2, 3, left_w - 4, "Build", build_stamp());
        kv(2, 4, left_w - 4, "Kernel", strcmp(kernel_boot_source(), "disk") == 0 ? "updated (disk)" :
                                      (strcmp(kernel_boot_source(), "old") == 0 ? "previous (disk)" : "built-in (ISO)"));
        kv(2, 5, left_w - 4, "Target", rsd_current_target());
        kv(2, 6, left_w - 4, "CPU", cpu);
        kv(2, 7, left_w - 4, "Network", net);
        char scr[48];
        snprintf(scr, sizeof(scr), "%dx%d cells, %dx%d px", C, R, scaler_info()->px_w, scaler_info()->px_h);
        kv(2, 8, left_w - 4, "Screen", scr);
        kv(2, 9, left_w - 4, "Cmdline", cmdline_get()[0] ? cmdline_get() : "(none)");

        // memory and disk
        panel(0, 1 + sys_h, left_w, 7, "Memory and disk");
        draw_bar_row(2, 2 + sys_h, left_w - 4, "RAM", to_pct(mem_pct), mem_info);
        draw_bar_row(2, 5 + sys_h, left_w - 4, "DISK", to_pct(disk_pct), disk_info);

        // memory history fills the rest of the left column
        int hist_y = 1 + sys_h + 7;
        int hist_h = R - 1 - hist_y;
        if (hist_h >= 4) {
            uint32_t scale = 256;   // KB; grows with the largest sample so small changes stay visible
            for (int i = 0; i < hist_n; i++) if (mem_hist[i] > scale) scale = mem_hist[i];
            char title[48];
            snprintf(title, sizeof(title), "Memory in use (KB), top of graph = %u", scale);
            panel(0, hist_y, left_w, hist_h, title);
            draw_history(2, hist_y + 1, left_w - 4, hist_h - 2, mem_hist, hist_n, scale);
        }

        // services and journal share the right column (or stack under the left one on a narrow screen)
        int top = two_cols ? 1 : hist_y + (hist_h >= 4 ? hist_h : 0);
        int avail = R - 1 - top;
        if (avail >= 8) {
            int units = rsd_unit_count();
            int svc_h = units + 3;
            if (svc_h > avail * 55 / 100) svc_h = avail * 55 / 100;
            if (svc_h < 5) svc_h = 5;
            draw_services(right_x, top, right_w, svc_h);
            if (avail - svc_h >= 4) draw_journal(right_x, top + svc_h, right_w, avail - svc_h);
        }

        fs_present();
        uint8_t key = fs_wait(500);
        if (key == FS_KEY_ESC || key == FS_KEY_Q) break;
        if (key == KEY_P) paused = !paused;
    }
    fs_end();
}

// ---- life -------------------------------------------------------------------------------------------

static uint8_t g_life[2][FBCON_ROWS][FBCON_COLS];   // 0 dead, otherwise the cell's age (1..255)

static void life_seed(int cur, int cols, int rows)
{
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) g_life[cur][y][x] = (rnd() % 4 == 0) ? 1 : 0;
}

void life_command(int argc, char *argv[])
{
    (void)argc; (void)argv;
    if (!fs_begin()) return;
    seed_rng();

    int cols = fs_cols(), rows = fs_rows(), cur = 0;
    uint32_t generation = 0, delay = 80;
    bool paused = false;
    life_seed(cur, cols, rows);

    static const uint8_t AGE_COLOR[4] = { LGREEN, LCYAN, YELLOW, WHITE };

    for (;;) {
        int pop = 0;
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < cols; x++) {
                uint8_t age = g_life[cur][y][x];
                if (age) pop++;
                uint8_t color = AGE_COLOR[age >= 24 ? 3 : (age >= 8 ? 2 : (age >= 3 ? 1 : 0))];
                if (age) fs_put(x, y, (char)0xDB, fs_attr(color, BLACK)); else fs_put(x, y, ' ', BODY);
            }

        char status[80];
        snprintf(status, sizeof(status), " LIFE  gen %u  alive %d  %ums/step%s   Space pause  R reseed  +/- speed  Q quit ",
                 generation, pop, delay, paused ? "  [paused]" : "");
        fs_fill(0, 0, cols, 1, ' ', fs_attr(BLACK, GREY));
        text_clip(0, 0, status, cols, fs_attr(BLACK, GREY));
        fs_present();

        uint8_t key = fs_wait(delay);
        if (key == FS_KEY_ESC || key == FS_KEY_Q) break;
        if (key == KEY_SPACE || key == KEY_P) paused = !paused;
        if (key == KEY_R) { life_seed(cur, cols, rows); generation = 0; continue; }
        if (key == KEY_PLUS && delay > 10) delay -= 10;
        if (key == KEY_MINUS && delay < 500) delay += 10;
        if (paused) continue;

        int nxt = cur ^ 1;
        for (int y = 0; y < rows; y++) {
            int ym = (y + rows - 1) % rows, yp = (y + 1) % rows;
            for (int x = 0; x < cols; x++) {
                int xm = (x + cols - 1) % cols, xp = (x + 1) % cols;
                int n = (g_life[cur][ym][xm] != 0) + (g_life[cur][ym][x] != 0) + (g_life[cur][ym][xp] != 0) +
                        (g_life[cur][y][xm] != 0) + (g_life[cur][y][xp] != 0) +
                        (g_life[cur][yp][xm] != 0) + (g_life[cur][yp][x] != 0) + (g_life[cur][yp][xp] != 0);
                uint8_t age = g_life[cur][y][x];
                if (age) g_life[nxt][y][x] = (n == 2 || n == 3) ? (age < 255 ? (uint8_t)(age + 1) : age) : 0;
                else     g_life[nxt][y][x] = (n == 3) ? 1 : 0;
            }
        }
        cur = nxt;
        generation++;
        if (pop == 0) { life_seed(cur, cols, rows); generation = 0; }
    }
    fs_end();
}

// ---- matrix -----------------------------------------------------------------------------------------

static uint8_t g_glow[FBCON_ROWS][FBCON_COLS];
static char    g_glyph[FBCON_ROWS][FBCON_COLS];

void matrix_command(int argc, char *argv[])
{
    (void)argc; (void)argv;
    if (!fs_begin()) return;
    seed_rng();

    int cols = fs_cols(), rows = fs_rows();
    static int head[FBCON_COLS], period[FBCON_COLS], wait[FBCON_COLS];
    for (int x = 0; x < cols; x++) {
        head[x] = -(int)(rnd() % (uint32_t)rows);
        period[x] = 1 + (int)(rnd() % 3);
        wait[x] = 0;
        for (int y = 0; y < rows; y++) { g_glow[y][x] = 0; g_glyph[y][x] = (char)(33 + rnd() % 90); }
    }
    fs_clear(fs_attr(GREEN, BLACK));

    for (;;) {
        for (int x = 0; x < cols; x++) {
            if (++wait[x] >= period[x]) {
                wait[x] = 0;
                head[x]++;
                if (head[x] >= 0 && head[x] < rows) { g_glow[head[x]][x] = 255; g_glyph[head[x]][x] = (char)(33 + rnd() % 90); }
                if (head[x] - 6 > rows + (int)(rnd() % 12)) {
                    head[x] = -(int)(rnd() % 20);
                    period[x] = 1 + (int)(rnd() % 3);
                }
            }
        }
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < cols; x++) {
                uint8_t g = g_glow[y][x];
                if (g == 0) { fs_put(x, y, ' ', BODY); continue; }
                if (g > 6 && rnd() % 24 == 0) g_glyph[y][x] = (char)(33 + rnd() % 90);
                uint8_t color = g > 235 ? WHITE : (g > 150 ? LGREEN : GREEN);
                fs_put(x, y, g_glyph[y][x], fs_attr(color, BLACK));
                g_glow[y][x] = g > 235 ? (uint8_t)(g - 10) : (uint8_t)(g > 12 ? g - 12 : 0);
            }
        fs_present();
        uint8_t key = fs_wait(45);
        if (key == FS_KEY_ESC || key == FS_KEY_Q) break;
    }
    fs_end();
}

// ---- resinfo ----------------------------------------------------------------------------------------

void screeninfo_command(int argc, char *argv[])
{
    (void)argc; (void)argv;
    const scaler_info_t *s = scaler_info();
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Display scaler (derived from the terminal size)\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    printr("  Terminal grid   %d x %d cells (%d x %d pixels)\n", s->term_cols, s->term_rows, s->px_w, s->px_h);
    printr("  Window grid     %d x %d cells, each %d x %d pixels, offset %d,%d\n", s->win_cols, s->win_rows, s->cell_w, s->cell_h, s->left, s->top);
    printr("  Console         %s\n", fbcon_enabled() ? "framebuffer (1280x720x32)" : "plain text mode");
    printr("  Full-screen     sysmon  life  matrix        (Q or Esc leaves)\n");
    printr("  Scaled apps     snake (pong), files (rawr), anim overlays, every 0xB8000 window\n");
}
