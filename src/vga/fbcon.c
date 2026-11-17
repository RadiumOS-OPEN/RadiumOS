// fbcon.c - 1280x720x32 framebuffer console (see fbcon.h)

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "fbcon.h"
#include "scaler.h"
#include "../io/io.h"
#include "../timers/tsc.h"

// Terminal view: 8x8 cells, 1:1 pixels.
#define CELL     8
// Legacy view: the 0xB8000 window (80x50 cells at most). Its cell size and position come from the scaler,
// so it fills whatever screen the terminal reports.
#define L_COLS   80
#define L_ROWS   50
#define L_CELL_MAX 64
// Legacy cells that background tasks repaint (the watchdog panel) do not count as a program painting.
#define L_HUD_COL 46
#define L_HUD_ROW 25

#define DISPI_INDEX_PORT 0x01CE
#define DISPI_DATA_PORT  0x01CF
#define DISPI_ID         0
#define DISPI_XRES       1
#define DISPI_YRES       2
#define DISPI_BPP        3
#define DISPI_ENABLE     4
#define DISPI_VIRT_W     6
#define DISPI_XOFF       8
#define DISPI_YOFF       9

#define FB_DEFAULT_BASE  0xFD000000u
#define FB_MAP_END       0xFE000000u   // boot.asm maps 0xFD000000 + 16 MiB

extern uint8_t  vga_font_8x8[256][8];   // bit 0 is the left pixel
extern uint32_t bptl[];                 // page directory for the first GiB, 2-MiB entries (boot.asm)

typedef enum { VIEW_TERMINAL, VIEW_LEGACY } view_t;

static volatile bool g_enabled = false;
static volatile bool g_active  = false;
static volatile bool g_busy    = false;
static view_t g_view = VIEW_TERMINAL;
static bool   g_force = true;

static uint32_t *g_fb = (uint32_t *)FB_DEFAULT_BASE;

// The terminal as pixels, without overlays; overlays are composed onto a copy of it.
static uint32_t g_back[FBCON_WIDTH * FBCON_HEIGHT];

typedef struct {
    bool used;
    char name[16];
    int x, y, w, h;
    fbcon_overlay_fn fn;
    void *arg;
    uint32_t last_ms;
    uint32_t buf[FBCON_OVERLAY_MAX_PIXELS];
} overlay_t;

#define OVERLAY_PERIOD_MS 33
static overlay_t g_overlays[FBCON_OVERLAY_MAX];
static uint64_t g_tsc0 = 0;
static uint32_t g_ms = 0;
// Bounding box of terminal pixels redrawn during the current flush.
static int g_dx0, g_dy0, g_dx1, g_dy1;

static uint16_t g_term[FBCON_COLS * FBCON_ROWS];        // terminal cells
static uint16_t g_term_drawn[FBCON_COLS * FBCON_ROWS];  // what the terminal view last showed

// Backs the VGA text window 0xB8000-0xBFFFF once remapped.
static uint16_t g_shadow[0x8000 / 2] __attribute__((aligned(4096)));
static uint16_t g_legacy_seen[L_COLS * L_ROWS];   // last shadow contents looked at
static uint16_t g_legacy_drawn[L_COLS * L_ROWS];  // what the legacy view last showed
static uint32_t g_pt[1024] __attribute__((aligned(4096)));   // 512 PAE entries (lo, hi)

static uint32_t g_cursor = 0xFFFFFFFFu;
static uint32_t g_cursor_drawn = 0xFFFFFFFFu;
static bool     g_cursor_on = true;
static bool     g_cursor_on_drawn = true;
static uint64_t g_blink_at = 0;
static uint64_t g_last_poll = 0;
static volatile uint32_t g_tick_count = 0;

static const uint32_t PALETTE[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static inline uint64_t rdtsc64(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void dispi_write(uint16_t index, uint16_t value)
{
    outw(DISPI_INDEX_PORT, index);
    outw(DISPI_DATA_PORT, value);
}

static uint16_t dispi_read(uint16_t index)
{
    outw(DISPI_INDEX_PORT, index);
    return inw(DISPI_DATA_PORT);
}

static void set_video_mode(void)
{
    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, FBCON_WIDTH);
    dispi_write(DISPI_YRES, FBCON_HEIGHT);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_VIRT_W, FBCON_WIDTH);
    dispi_write(DISPI_XOFF, 0);
    dispi_write(DISPI_YOFF, 0);
    dispi_write(DISPI_ENABLE, 0x01 | 0x40);   // enabled + linear framebuffer
}

static void clear_screen(void)
{
    for (uint32_t i = 0; i < (uint32_t)FBCON_WIDTH * FBCON_HEIGHT; i++) g_fb[i] = g_back[i] = 0;
}

// PCI BAR 0 of the Bochs/QEMU VGA device (1234:1111), or the usual QEMU address if it cannot be found.
static uint32_t find_framebuffer(void)
{
    for (uint8_t dev = 0; dev < 32; dev++) {
        if (pci_read_config_dword(0, dev, 0, 0) == 0x11111234u) {
            uint32_t bar = pci_read_config_dword(0, dev, 0, 0x10);
            if (bar != 0 && bar != 0xFFFFFFFFu && (bar & 1) == 0) return bar & 0xFFFFFFF0u;
        }
    }
    return FB_DEFAULT_BASE;
}

// Points 0xB8000-0xBFFFF at g_shadow by splitting the first 2-MiB page into 4-KiB pages.
static void remap_text_window(void)
{
    for (uint32_t i = 0; i < 512; i++) {
        uint32_t phys = i * 4096u;
        if (i >= 0xB8 && i <= 0xBF) phys = (uint32_t)(uintptr_t)g_shadow + (i - 0xB8) * 4096u;
        g_pt[i * 2]     = phys | 0x3;
        g_pt[i * 2 + 1] = 0;
    }
    bptl[0] = (uint32_t)(uintptr_t)g_pt | 0x3;
    bptl[1] = 0;
    __asm__ volatile("mov %%cr3, %%eax\n\tmov %%eax, %%cr3" ::: "eax", "memory");
}

bool fbcon_init(void)
{
    uint16_t id = dispi_read(DISPI_ID);
    if (id < 0xB0C0 || id > 0xB0CF) return false;

    uint32_t base = find_framebuffer();
    uint32_t bytes = (uint32_t)FBCON_WIDTH * FBCON_HEIGHT * 4;
    if (base < FB_DEFAULT_BASE || base + bytes > FB_MAP_END) return false;
    g_fb = (uint32_t *)(uintptr_t)base;

    remap_text_window();
    for (uint32_t i = 0; i < 0x8000 / 2; i++) g_shadow[i] = 0x0720;
    for (uint32_t i = 0; i < L_COLS * L_ROWS; i++) g_legacy_seen[i] = g_legacy_drawn[i] = 0x0720;
    for (uint32_t i = 0; i < FBCON_COLS * FBCON_ROWS; i++) g_term[i] = g_term_drawn[i] = 0x0720;

    set_video_mode();
    clear_screen();

    g_tsc0 = rdtsc64();
    g_view = VIEW_TERMINAL;
    g_enabled = true;
    g_force = true;
    g_active = true;
    fbcon_flush();
    return true;
}

bool fbcon_enabled(void) { return g_enabled; }
bool fbcon_active(void)  { return g_active; }
uint16_t *fbcon_text_buffer(void) { return g_term; }

static void draw_term_cell(uint32_t index)
{
    uint16_t cell = g_term[index];
    uint8_t attr = (uint8_t)(cell >> 8);
    uint32_t fg = PALETTE[attr & 0x0F];
    uint32_t bg = PALETTE[attr >> 4];
    const uint8_t *glyph = vga_font_8x8[cell & 0xFF];
    bool cursor_here = g_cursor_on && index == g_cursor;

    uint32_t col = index % FBCON_COLS, row = index / FBCON_COLS;
    uint32_t offset = (uint32_t)(row * CELL) * FBCON_WIDTH + col * CELL;
    uint32_t *dst = g_back + offset;
    uint32_t *out = g_fb + offset;

    for (uint32_t dy = 0; dy < CELL; dy++, dst += FBCON_WIDTH, out += FBCON_WIDTH) {
        if (cursor_here && dy >= CELL - 2) {
            for (uint32_t x = 0; x < CELL; x++) dst[x] = fg;
        } else {
            uint8_t bits = glyph[dy];
            for (uint32_t dx = 0; dx < CELL; dx++) dst[dx] = ((bits >> dx) & 1) ? fg : bg;
        }
        for (uint32_t x = 0; x < CELL; x++) out[x] = dst[x];
    }

    int px = (int)(col * CELL), py = (int)(row * CELL);
    if (px < g_dx0) g_dx0 = px;
    if (py < g_dy0) g_dy0 = py;
    if (px + CELL > g_dx1) g_dx1 = px + CELL;
    if (py + CELL > g_dy1) g_dy1 = py + CELL;
}

// Source glyph column for each pixel column of a legacy cell, rebuilt when the scaler's geometry changes.
static uint8_t g_xmap[L_CELL_MAX];
static uint32_t g_scaler_gen = 0;

static bool sync_scaler(void)
{
    if (g_scaler_gen == scaler_generation()) return false;
    g_scaler_gen = scaler_generation();

    int cw = scaler_info()->cell_w;
    if (cw > L_CELL_MAX) cw = L_CELL_MAX;
    for (int dx = 0; dx < L_CELL_MAX; dx++) g_xmap[dx] = (uint8_t)(dx < cw ? dx * 8 / cw : 7);
    return true;
}

static void draw_legacy_cell(uint32_t index)
{
    const scaler_info_t *s = scaler_info();
    uint32_t col = index % L_COLS, row = index / L_COLS;
    if ((int)col >= s->win_cols || (int)row >= s->win_rows) return;

    uint16_t cell = g_shadow[index];
    uint8_t attr = (uint8_t)(cell >> 8);
    uint32_t fg = PALETTE[attr & 0x0F];
    uint32_t bg = PALETTE[attr >> 4];
    const uint8_t *glyph = vga_font_8x8[cell & 0xFF];

    uint32_t cw = (uint32_t)(s->cell_w > L_CELL_MAX ? L_CELL_MAX : s->cell_w), ch = (uint32_t)s->cell_h;
    uint32_t *dst = g_fb + (uint32_t)(s->top + (int)(row * ch)) * FBCON_WIDTH + (uint32_t)s->left + col * cw;

    for (uint32_t dy = 0; dy < ch; dy++, dst += FBCON_WIDTH) {
        uint8_t bits = glyph[(dy * 8) / ch];
        for (uint32_t dx = 0; dx < cw; dx++) dst[dx] = ((bits >> g_xmap[dx]) & 1) ? fg : bg;
    }
}

// True if a program painted the legacy window since the last look (the watchdog panel does not count).
static bool legacy_painted(void)
{
    bool painted = false;
    for (uint32_t i = 0; i < L_COLS * L_ROWS; i++) {
        uint16_t v = g_shadow[i];
        if (v == g_legacy_seen[i]) continue;
        g_legacy_seen[i] = v;
        if (!((i % L_COLS) >= L_HUD_COL && (i / L_COLS) < L_HUD_ROW)) painted = true;
    }
    return painted;
}

static bool terminal_changed(void)
{
    for (uint32_t i = 0; i < FBCON_COLS * FBCON_ROWS; i++)
        if (g_term[i] != g_term_drawn[i]) return true;
    return false;
}

// ---- overlays ---------------------------------------------------------------------------------

static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t a)
{
    uint32_t inv = 255 - a;
    uint32_t r = (((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * inv + 127) / 255;
    uint32_t g = (((src >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * inv + 127) / 255;
    uint32_t b = ((src & 0xFF) * a + (dst & 0xFF) * inv + 127) / 255;
    return (r << 16) | (g << 8) | b;
}

void fbc_pixel(fbcon_canvas_t *c, int x, int y, uint32_t color, uint8_t alpha)
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h || alpha == 0) return;
    uint32_t *p = &c->px[y * c->w + x];
    *p = alpha == 255 ? (color & 0xFFFFFF) : blend(*p, color, alpha);
}

void fbc_rect(fbcon_canvas_t *c, int x, int y, int w, int h, uint32_t color, uint8_t alpha)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) fbc_pixel(c, xx, yy, color, alpha);
}

void fbc_glyph(fbcon_canvas_t *c, int x, int y, uint8_t ch, uint32_t color, int scale, uint8_t alpha)
{
    const uint8_t *glyph = vga_font_8x8[ch];
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if ((glyph[row] >> col) & 1) fbc_rect(c, x + col * scale, y + row * scale, scale, scale, color, alpha);
}

int fbc_text(fbcon_canvas_t *c, int x, int y, const char *text, uint32_t color, int scale, uint8_t alpha)
{
    int start = x;
    for (; *text; text++, x += 8 * scale) fbc_glyph(c, x, y, (uint8_t)*text, color, scale, alpha);
    return x - start;
}

static bool name_equals(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static overlay_t *find_overlay(const char *name)
{
    for (int i = 0; i < FBCON_OVERLAY_MAX; i++)
        if (g_overlays[i].used && name_equals(g_overlays[i].name, name)) return &g_overlays[i];
    return NULL;
}

static void restore_rect(int x, int y, int w, int h)
{
    for (int row = y; row < y + h; row++) {
        if (row < 0 || row >= FBCON_HEIGHT) continue;
        for (int col = x; col < x + w; col++)
            if (col >= 0 && col < FBCON_WIDTH) g_fb[row * FBCON_WIDTH + col] = g_back[row * FBCON_WIDTH + col];
    }
}

uint32_t fbcon_time_ms(void) { return g_ms; }

int fbcon_overlay_add(const char *name, int x, int y, int w, int h, fbcon_overlay_fn fn, void *arg)
{
    if (!g_enabled || !fn || w <= 0 || h <= 0 || w * h > FBCON_OVERLAY_MAX_PIXELS) return -1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > FBCON_WIDTH || y + h > FBCON_HEIGHT) return -1;

    fbcon_overlay_remove(name);
    overlay_t *slot = NULL;
    for (int i = 0; i < FBCON_OVERLAY_MAX && !slot; i++) if (!g_overlays[i].used) slot = &g_overlays[i];
    if (!slot) return -1;

    int n = 0;
    while (name[n] && n < 15) { slot->name[n] = name[n]; n++; }
    slot->name[n] = '\0';
    slot->x = x; slot->y = y; slot->w = w; slot->h = h;
    slot->fn = fn; slot->arg = arg;
    slot->last_ms = g_ms - OVERLAY_PERIOD_MS;
    slot->used = true;
    return 0;
}

bool fbcon_overlay_remove(const char *name)
{
    overlay_t *ov = find_overlay(name);
    if (!ov) return false;
    ov->used = false;
    if (g_active && g_view == VIEW_TERMINAL) restore_rect(ov->x, ov->y, ov->w, ov->h);
    return true;
}

void fbcon_overlay_clear(void)
{
    for (int i = 0; i < FBCON_OVERLAY_MAX; i++) {
        if (!g_overlays[i].used || g_overlays[i].name[0] == '!') continue;   // "!" overlays are pinned (system markers)
        g_overlays[i].used = false;
        if (g_active && g_view == VIEW_TERMINAL) restore_rect(g_overlays[i].x, g_overlays[i].y, g_overlays[i].w, g_overlays[i].h);
    }
}

const char *fbcon_overlay_at(int n, int *x, int *y, int *w, int *h)
{
    for (int i = 0; i < FBCON_OVERLAY_MAX; i++) {
        if (!g_overlays[i].used) continue;
        if (n-- > 0) continue;
        if (x) *x = g_overlays[i].x;
        if (y) *y = g_overlays[i].y;
        if (w) *w = g_overlays[i].w;
        if (h) *h = g_overlays[i].h;
        return g_overlays[i].name;
    }
    return NULL;
}

static bool g_ov_paused = false;

void fbcon_overlays_pause(bool paused)
{
    if (paused == g_ov_paused) return;
    g_ov_paused = paused;
    if (!paused || !g_active || g_view != VIEW_TERMINAL) return;
    for (int i = 0; i < FBCON_OVERLAY_MAX; i++)
        if (g_overlays[i].used) restore_rect(g_overlays[i].x, g_overlays[i].y, g_overlays[i].w, g_overlays[i].h);
}

// Redraws every overlay whose frame is due, or that sits where terminal text was just redrawn.
static void present_overlays(bool text_redrawn)
{
    if (g_ov_paused) return;
    for (int i = 0; i < FBCON_OVERLAY_MAX; i++) {
        overlay_t *ov = &g_overlays[i];
        if (!ov->used) continue;

        bool due = g_ms - ov->last_ms >= OVERLAY_PERIOD_MS;
        bool covered = text_redrawn && g_dx0 < ov->x + ov->w && g_dx1 > ov->x && g_dy0 < ov->y + ov->h && g_dy1 > ov->y;
        if (!due && !covered) continue;
        if (due) ov->last_ms = g_ms;

        for (int row = 0; row < ov->h; row++)
            for (int col = 0; col < ov->w; col++)
                ov->buf[row * ov->w + col] = g_back[(ov->y + row) * FBCON_WIDTH + ov->x + col];

        fbcon_canvas_t canvas = { ov->buf, ov->w, ov->h };
        ov->fn(&canvas, g_ms, ov->arg);

        for (int row = 0; row < ov->h; row++)
            for (int col = 0; col < ov->w; col++)
                g_fb[(ov->y + row) * FBCON_WIDTH + ov->x + col] = ov->buf[row * ov->w + col];
    }
}

static void switch_view(view_t view)
{
    g_view = view;
    clear_screen();
    g_force = true;
    g_cursor_drawn = 0xFFFFFFFFu;
}

void fbcon_flush(void)
{
    if (!g_active || g_busy) return;
    g_busy = true;

    uint32_t per_ms = tsc_cycles_per_ms();
    if (per_ms) {
        uint64_t now = rdtsc64();
        if (now >= g_blink_at) {
            g_cursor_on = !g_cursor_on;
            g_blink_at = now + (uint64_t)per_ms * 530;
        }
    }

    if (per_ms) g_ms = (uint32_t)((rdtsc64() - g_tsc0) / per_ms);

    if (sync_scaler() && g_view == VIEW_LEGACY) switch_view(VIEW_LEGACY);

    bool painted = legacy_painted();
    bool typed = terminal_changed();
    if (g_view == VIEW_TERMINAL && painted && !typed) switch_view(VIEW_LEGACY);
    else if (g_view == VIEW_LEGACY && typed && !painted) switch_view(VIEW_TERMINAL);

    bool force = g_force;
    g_force = false;

    if (g_view == VIEW_TERMINAL) {
        g_dx0 = g_dy0 = 0x7FFFFFFF;
        g_dx1 = g_dy1 = -1;
        for (uint32_t i = 0; i < FBCON_COLS * FBCON_ROWS; i++) {
            uint16_t cell = g_term[i];
            if (!force && cell == g_term_drawn[i]) continue;
            g_term_drawn[i] = cell;
            draw_term_cell(i);
        }
        if (g_cursor != g_cursor_drawn || g_cursor_on != g_cursor_on_drawn) {
            if (g_cursor_drawn < FBCON_COLS * FBCON_ROWS) draw_term_cell(g_cursor_drawn);
            if (g_cursor < FBCON_COLS * FBCON_ROWS) draw_term_cell(g_cursor);
            g_cursor_drawn = g_cursor;
            g_cursor_on_drawn = g_cursor_on;
        }
        present_overlays(g_dx1 >= 0);
    } else {
        for (uint32_t i = 0; i < L_COLS * L_ROWS; i++) {
            uint16_t cell = g_shadow[i];
            if (!force && cell == g_legacy_drawn[i]) continue;
            g_legacy_drawn[i] = cell;
            draw_legacy_cell(i);
        }
    }

    g_busy = false;
}

static void (*g_poll_hook)(void);
static bool g_in_poll_hook;

void fbcon_set_poll_hook(void (*fn)(void)) { g_poll_hook = fn; }

void fbcon_poll(void)
{
    if (g_poll_hook && !g_in_poll_hook) {
        g_in_poll_hook = true;
        g_poll_hook();
        g_in_poll_hook = false;
    }
    if (!g_active) return;
    uint32_t per_ms = tsc_cycles_per_ms();
    if (per_ms) {
        uint64_t now = rdtsc64();
        if (now - g_last_poll < (uint64_t)per_ms * 12) return;
        g_last_poll = now;
    }
    fbcon_flush();
}

void fbcon_tick(void)
{
    if (!g_active) return;
    if ((++g_tick_count & 15) == 0) fbcon_flush();
}

void fbcon_set_cursor(uint32_t cell) { g_cursor = cell; }

static fbcon_redirect_fn g_redirect;
void fbcon_set_redirect(fbcon_redirect_fn fn) { g_redirect = fn; }
fbcon_redirect_fn fbcon_get_redirect(void) { return g_redirect; }

void fbcon_suspend(void) { if (g_redirect) return; g_active = false; }

void fbcon_suspend_hw(void)
{
    if (!g_enabled || g_redirect) return;
    g_active = false;
    dispi_write(DISPI_ENABLE, 0);
}

void fbcon_resume(void)
{
    if (!g_enabled || g_redirect) return;
    g_active = false;
    set_video_mode();
    clear_screen();
    g_force = true;
    g_cursor_drawn = 0xFFFFFFFFu;
    g_active = true;
    fbcon_flush();
}

void fbcon_blit(const uint32_t *src, int stride, int x, int y, int w, int h)
{
    if (g_redirect) { g_redirect(src, stride, x, y, w, h); return; }
    if (!g_enabled || g_active) return;
    if (x < 0) { src -= x; w += x; x = 0; }
    if (y < 0) { src -= (long)y * stride; h += y; y = 0; }
    if (x + w > FBCON_WIDTH) w = FBCON_WIDTH - x;
    if (y + h > FBCON_HEIGHT) h = FBCON_HEIGHT - y;
    for (int row = 0; row < h; row++) {
        uint32_t *dst = g_fb + (uint32_t)(y + row) * FBCON_WIDTH + (uint32_t)x;
        const uint32_t *s = src + (long)row * stride;
        for (int col = 0; col < w; col++) dst[col] = s[col];
    }
}
