// anim.c - animated overlays for the 1280x720 console (see fbcon.h)
//
//   anim                         list the animations and which are running
//   anim start NAME [x y]        start one (ticker takes text instead: anim start ticker some words)
//   anim stop NAME | all         stop one or all of them
//   anim demo                    start the standard set
//
// Each animation draws into a small pinned rectangle about 30 times a second, so it keeps moving
// while the terminal scrolls or clears. Put `anim start ...` lines in .radiumrc to have them at boot.

#include <stdint.h>
#include <stdbool.h>
#include "anim.h"
#include "bootpage.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../vga/fbcon.h"
#include "../vga/scaler.h"
#include "../utility/utility.h"
#include "../timers/date.h"

#define OUT_ERR(...) do { terminal_setcolor(VGA_COLOR_LIGHT_RED); printr(__VA_ARGS__); terminal_setcolor(VGA_COLOR_WHITE); } while (0)

static const uint32_t RAINBOW[6] = { 0xFF5555, 0xFFFF55, 0x55FF55, 0x55FFFF, 0x5588FF, 0xFF55FF };

static const int16_t SIN12[12] = { 0, 128, 222, 256, 222, 128, 0, -128, -222, -256, -222, -128 };
static const int16_t COS12[12] = { 256, 222, 128, 0, -128, -222, -256, -222, -128, 0, 128, 222 };

static int tri(int x)   // 0..255 triangle wave with period 512
{
    int m = x & 511;
    return m < 256 ? m : 511 - m;
}

static void backdrop(fbcon_canvas_t *c, uint32_t t, uint8_t alpha)
{
    fbc_rect(c, 0, 0, c->w, c->h, 0x000010, alpha);
    uint32_t border = RAINBOW[(t / 400) % 6];
    fbc_rect(c, 0, 0, c->w, 1, border, 200);
    fbc_rect(c, 0, c->h - 1, c->w, 1, border, 200);
    fbc_rect(c, 0, 0, 1, c->h, border, 200);
    fbc_rect(c, c->w - 1, 0, 1, c->h, border, 200);
}

static void two_digits(char *out, int v)
{
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
}

// ---- spinner: twelve dots chasing each other ------------------------------------------------

static void draw_spinner(fbcon_canvas_t *c, uint32_t t, void *arg)
{
    (void)arg;
    int cx = c->w / 2, cy = c->h / 2, r = (c->w < c->h ? c->w : c->h) / 2 - 6;
    int head = (int)((t / 70) % 12);

    for (int i = 0; i < 12; i++) {
        int age = (head - i + 12) % 12;
        uint8_t alpha = (uint8_t)(255 - age * 19);
        int x = cx + SIN12[i] * r / 256, y = cy - COS12[i] * r / 256;
        int size = age < 3 ? 6 : 4;
        fbc_rect(c, x - size / 2, y - size / 2, size, size, RAINBOW[(i + (int)(t / 300)) % 6], alpha);
    }
}

// ---- clock: wall time with a sweeping seconds bar ---------------------------------------------

static void draw_clock(fbcon_canvas_t *c, uint32_t t, void *arg)
{
    (void)arg;
    static uint32_t last_second = 0xFFFFFFFFu;
    static datetime_t now;
    if (t / 1000 != last_second) { last_second = t / 1000; rtc_get_time(&now); }

    backdrop(c, t, 150);
    char text[9];
    two_digits(text, now.hour);     text[2] = (t / 500) % 2 ? ':' : ' ';
    two_digits(text + 3, now.minute); text[5] = (t / 500) % 2 ? ':' : ' ';
    two_digits(text + 6, now.second); text[8] = '\0';
    fbc_text(c, (c->w - 8 * 16) / 2, 5, text, 0xFFFFFF, 2, 255);

    int fill = (now.second * 1000 + (int)(t % 1000)) * (c->w - 8) / 60000;
    fbc_rect(c, 4, c->h - 8, c->w - 8, 3, 0x303050, 255);
    for (int x = 0; x < fill; x++) fbc_rect(c, 4 + x, c->h - 8, 1, 3, RAINBOW[(x / 16 + (int)(t / 200)) % 6], 255);
}

// ---- wave: bouncing equalizer bars ------------------------------------------------------------

static void draw_wave(fbcon_canvas_t *c, uint32_t t, void *arg)
{
    (void)arg;
    backdrop(c, t, 130);
    int bars = 20, bw = (c->w - 8) / bars, top = 4, floor = c->h - 4;
    for (int i = 0; i < bars; i++) {
        int level = (tri((int)(t / 3) + i * 37) + tri((int)(t / 5) + i * 71)) / 2;   // 0..255
        int h = 2 + level * (floor - top - 2) / 255;
        uint32_t color = level < 100 ? 0x55FF55 : (level < 180 ? 0xFFFF55 : 0xFF5555);
        fbc_rect(c, 4 + i * bw, floor - h, bw - 2, h, color, 230);
        fbc_rect(c, 4 + i * bw, floor - h - 2, bw - 2, 2, 0xFFFFFF, 200);
    }
}

// ---- stats: uptime and memory, refreshed each second -------------------------------------------

static void copy_var(char *out, size_t size, const char *name)
{
    const char *v = bootpage_var(name);
    strncpy(out, v ? v : "?", size - 1);
    out[size - 1] = '\0';
}

static void draw_stats(fbcon_canvas_t *c, uint32_t t, void *arg)
{
    (void)arg;
    static uint32_t last_second = 0xFFFFFFFFu;
    static char up[16], mem[8], disk[8];
    if (t / 1000 != last_second) {
        last_second = t / 1000;
        copy_var(up, sizeof(up), "UPTIME");
        copy_var(mem, sizeof(mem), "MEM_PCT");
        copy_var(disk, sizeof(disk), "DISK_PCT");
    }

    backdrop(c, t, 150);
    char line[40];
    strcpy(line, "UP ");
    strcat(line, up);
    fbc_text(c, 6, 4, line, 0x55FFFF, 1, 255);

    strcpy(line, "MEM ");  strcat(line, mem);  strcat(line, "%  DSK ");  strcat(line, disk);  strcat(line, "%");
    fbc_text(c, 6, 15, line, 0xFFFF55, 1, 255);
}

// ---- ticker: rainbow text scrolling across the top ----------------------------------------------

static char g_ticker_text[160] =
    "RadiumOS 1280x720  *  systemctl list-units  *  journalctl  *  anim list  *  help  *  update  *  ";

static void draw_ticker(fbcon_canvas_t *c, uint32_t t, void *arg)
{
    (void)arg;
    fbc_rect(c, 0, 0, c->w, c->h, 0x000018, 170);
    int len = (int)strlen(g_ticker_text);
    int span = len * 8;
    if (span == 0) return;

    int shift = (int)((t / 16) % (uint32_t)span);   // 62 pixels per second
    for (int x = -shift; x < c->w; x += span)
        for (int i = 0; i < len; i++) {
            int px = x + i * 8;
            if (px < -8 || px >= c->w) continue;
            fbc_glyph(c, px, 1, (uint8_t)g_ticker_text[i], RAINBOW[(i + (int)(t / 150)) % 6], 1, 255);
        }
}

// ---- rain: falling glyph columns ----------------------------------------------------------------

static uint32_t hash3(uint32_t a, uint32_t b, uint32_t d)
{
    uint32_t h = a * 73856093u ^ b * 19349663u ^ d * 83492791u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return h;
}

static void draw_rain(fbcon_canvas_t *c, uint32_t t, void *arg)
{
    (void)arg;
    int cols = c->w / 8, rows = c->h / 8, trail = 9;
    for (int col = 0; col < cols; col++) {
        int speed = 30 + (col * 13) % 50;                       // pixels per second
        int period = (rows + trail) * 8;
        int head = (int)((t * (uint32_t)speed / 1000 + (uint32_t)col * 53) % (uint32_t)period) / 8;
        for (int k = 0; k < trail; k++) {
            int row = head - k;
            if (row < 0 || row >= rows) continue;
            uint8_t ch = (uint8_t)(33 + hash3((uint32_t)col, (uint32_t)row, t / 140) % 90);
            uint32_t color = k == 0 ? 0xDDFFDD : 0x33FF55;
            fbc_glyph(c, col * 8, row * 8, ch, color, 1, (uint8_t)(k == 0 ? 255 : 220 - k * 22));
        }
    }
}

// ---- registry ------------------------------------------------------------------------------------

typedef struct {
    const char *name;
    const char *desc;
    int x, y, w, h;          // designed for a 1280-pixel-wide screen
    fbcon_overlay_fn fn;
    bool full_width;         // spans the whole screen width
    bool right;              // keeps its distance from the right edge
} anim_def_t;

static const anim_def_t ANIMS[] = {
    { "ticker",  "scrolling rainbow text across the top",   0,    0,   1280, 10,  draw_ticker,  true,  false },
    { "clock",   "live wall clock with a seconds sweep",    1104, 14,  168,  36,  draw_clock,   false, true  },
    { "stats",   "uptime, memory and disk use",              1104, 54,  168,  28,  draw_stats,   false, true  },
    { "wave",    "bouncing equalizer bars",                  1104, 86,  168,  44,  draw_wave,    false, true  },
    { "spinner", "twelve dots chasing around a circle",      1056, 14,  44,   44,  draw_spinner, false, true  },
    { "rain",    "falling glyph columns",                    0,    16,  200,  112, draw_rain,    false, false },
};
#define ANIM_COUNT ((int)(sizeof(ANIMS) / sizeof(ANIMS[0])))

static const anim_def_t *find_anim(const char *name)
{
    for (int i = 0; i < ANIM_COUNT; i++) if (strcmp(ANIMS[i].name, name) == 0) return &ANIMS[i];
    return NULL;
}

// Default position and size on the current screen.
static void default_geometry(const anim_def_t *a, int *x, int *y, int *w)
{
    const scaler_info_t *s = scaler_info();
    *w = a->full_width ? s->px_w : a->w;
    *x = a->right ? s->px_w - (SCALER_REF_W - a->x) : scaler_x(a->x);
    *y = scaler_y(a->y);
}

static bool start_at(const anim_def_t *a, int x, int y, int w)
{
    return fbcon_overlay_add(a->name, x, y, w, a->h, a->fn, NULL) == 0;
}

bool anim_start(const char *name)
{
    const anim_def_t *a = find_anim(name);
    if (!a) return false;
    int x, y, w;
    default_geometry(a, &x, &y, &w);
    return start_at(a, x, y, w);
}

bool anim_stop(const char *name)
{
    return fbcon_overlay_remove(name);
}

static bool is_number(const char *s)
{
    if (!*s) return false;
    for (; *s; s++) if (*s < '0' || *s > '9') return false;
    return true;
}

static int to_int(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

static void list_animations(void)
{
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Animations (pinned overlays that keep moving while the screen scrolls)\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    for (int i = 0; i < ANIM_COUNT; i++) {
        bool on = false;
        for (int n = 0;; n++) {
            const char *name = fbcon_overlay_at(n, NULL, NULL, NULL, NULL);
            if (!name) break;
            if (strcmp(name, ANIMS[i].name) == 0) on = true;
        }
        terminal_setcolor(on ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_GREY);
        print("  ");
        print(ANIMS[i].name);
        for (size_t n = strlen(ANIMS[i].name); n < 9; n++) print(" ");
        print(on ? "running  " : "stopped  ");
        terminal_setcolor(VGA_COLOR_WHITE);
        print(ANIMS[i].desc);
        print("\n");
    }
    terminal_setcolor(VGA_COLOR_LIGHT_GREY);
    print("  anim start NAME [x y]   anim start ticker TEXT...   anim stop NAME|all   anim demo\n");
    terminal_setcolor(VGA_COLOR_WHITE);
}

void anim_command(int argc, char *argv[])
{
    if (argc < 2 || strcmp(argv[1], "list") == 0) { list_animations(); return; }

    if (!fbcon_enabled()) { OUT_ERR("anim: needs the 1280x720 console (this display is in plain text mode)\n"); return; }

    if (strcmp(argv[1], "stop") == 0) {
        if (argc != 3) { OUT_ERR("Usage: anim stop NAME | all\n"); return; }
        if (strcmp(argv[2], "all") == 0) { fbcon_overlay_clear(); return; }
        if (!anim_stop(argv[2])) OUT_ERR("anim: %s is not running\n", argv[2]);
        return;
    }

    if (strcmp(argv[1], "demo") == 0) {
        const char *set[] = { "ticker", "clock", "stats", "wave", "spinner" };
        for (int i = 0; i < 5; i++) anim_start(set[i]);
        return;
    }

    if (strcmp(argv[1], "start") == 0) {
        if (argc < 3) { OUT_ERR("Usage: anim start NAME [x y]\n"); return; }
        const anim_def_t *a = find_anim(argv[2]);
        if (!a) { OUT_ERR("anim: unknown animation '%s' (anim list shows them)\n", argv[2]); return; }

        int x, y, w;
        default_geometry(a, &x, &y, &w);
        if (argc >= 5 && is_number(argv[3]) && is_number(argv[4])) {
            x = to_int(argv[3]);
            y = to_int(argv[4]);
        } else if (strcmp(a->name, "ticker") == 0 && argc > 3) {
            size_t o;
            o = 0;
            for (int i = 3; i < argc; i++) {
                for (const char *p = argv[i]; *p && o + 4 < sizeof(g_ticker_text); p++) g_ticker_text[o++] = *p;
                if (i + 1 < argc && o + 4 < sizeof(g_ticker_text)) g_ticker_text[o++] = ' ';
            }
            g_ticker_text[o++] = ' '; g_ticker_text[o++] = '*'; g_ticker_text[o++] = ' ';
            g_ticker_text[o] = '\0';
        }
        if (!start_at(a, x, y, w)) OUT_ERR("anim: cannot place %s at %d,%d (off screen, or no free overlay slot)\n", a->name, x, y);
        return;
    }

    OUT_ERR("Usage: anim [list] | start NAME [x y] | stop NAME|all | demo\n");
}
