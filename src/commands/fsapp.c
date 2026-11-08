// fsapp.c - toolkit for programs that own the whole terminal grid (see fsapp.h)
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "fsapp.h"
#include "../terminal/terminal.h"
#include "../vga/fbcon.h"
#include "../vga/scaler.h"
#include "../io/io.h"
#include "../timers/tsc.h"

extern size_t terminal_width, terminal_height, terminal_row, terminal_column;
extern uint16_t *terminal_buffer;

static uint16_t g_saved[FBCON_COLS * FBCON_ROWS];
static size_t g_saved_w, g_saved_h, g_saved_row, g_saved_col;
static bool g_active = false;

bool fs_begin(void)
{
    if (g_active) return false;
    g_saved_w = terminal_width;
    g_saved_h = terminal_height;
    g_saved_row = terminal_row;
    g_saved_col = terminal_column;
    if (g_saved_w * g_saved_h > FBCON_COLS * FBCON_ROWS) return false;
    for (size_t i = 0; i < g_saved_w * g_saved_h; i++) g_saved[i] = terminal_buffer[i];

    g_active = true;
    fbcon_overlays_pause(true);
    fbcon_set_cursor(0xFFFFFFFFu);
    fs_clear(fs_attr(7, 0));
    return true;
}

void fs_end(void)
{
    if (!g_active) return;
    for (size_t i = 0; i < g_saved_w * g_saved_h; i++) terminal_buffer[i] = g_saved[i];
    terminal_row = g_saved_row;
    terminal_column = g_saved_col;
    terminal_update_cursor();
    g_active = false;
    fbcon_overlays_pause(false);
    fs_present();
}

int fs_cols(void) { return scaler_term_cols(); }
int fs_rows(void) { return scaler_term_rows(); }

void fs_put(int x, int y, char c, uint8_t attr)
{
    if (x < 0 || y < 0 || x >= fs_cols() || y >= fs_rows()) return;
    terminal_buffer[y * (int)terminal_width + x] = (uint16_t)((uint8_t)c | ((uint16_t)attr << 8));
}

void fs_clear(uint8_t attr) { fs_fill(0, 0, fs_cols(), fs_rows(), ' ', attr); }

void fs_fill(int x, int y, int w, int h, char c, uint8_t attr)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) fs_put(xx, yy, c, attr);
}

int fs_text(int x, int y, const char *s, uint8_t attr)
{
    for (; *s; s++, x++) fs_put(x, y, *s, attr);
    return x;
}

int fs_text_right(int right_x, int y, const char *s, uint8_t attr)
{
    int n = 0;
    while (s[n]) n++;
    return fs_text(right_x - n + 1, y, s, attr);
}

void fs_box(int x, int y, int w, int h, uint8_t attr, const char *title)
{
    if (w < 2 || h < 2) return;
    for (int i = 1; i < w - 1; i++) { fs_put(x + i, y, (char)0xCD, attr); fs_put(x + i, y + h - 1, (char)0xCD, attr); }
    for (int j = 1; j < h - 1; j++) { fs_put(x, y + j, (char)0xBA, attr); fs_put(x + w - 1, y + j, (char)0xBA, attr); }
    fs_put(x, y, (char)0xC9, attr);
    fs_put(x + w - 1, y, (char)0xBB, attr);
    fs_put(x, y + h - 1, (char)0xC8, attr);
    fs_put(x + w - 1, y + h - 1, (char)0xBC, attr);
    if (title && title[0]) {
        fs_put(x + 2, y, ' ', attr);
        int end = fs_text(x + 3, y, title, attr);
        fs_put(end, y, ' ', attr);
    }
}

void fs_bar(int x, int y, int w, int percent, uint8_t fill_attr, uint8_t empty_attr)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int filled = w * percent / 100;
    for (int i = 0; i < w; i++) {
        if (i < filled) fs_put(x + i, y, (char)0xDB, fill_attr);
        else            fs_put(x + i, y, (char)0xB0, empty_attr);
    }
}

void fs_present(void) { fbcon_poll(); }

uint8_t fs_poll_key(void)
{
    fbcon_poll();
    if (!(inb(0x64) & 0x01)) return 0;
    uint8_t sc = inb(0x60);
    if (sc == 0xE0 || sc == 0xE1 || (sc & 0x80)) return 0;
    return sc;
}

uint8_t fs_wait(uint32_t ms)
{
    uint64_t deadline = tsc_deadline_ms(ms);
    while (!tsc_reached(deadline)) {
        uint8_t key = fs_poll_key();
        if (key) return key;
        __asm__ volatile("pause");
    }
    return 0;
}

extern const char keyboard_map[128];
extern const char shifted_keyboard_map[128];

bool fs_get_event(fs_event_t *ev)
{
    static bool shift = false, ctrl = false, alt = false, ext = false;

    fbcon_poll();
    if (!(inb(0x64) & 0x01)) return false;
    uint8_t sc = inb(0x60);
    if (sc == 0xE0 || sc == 0xE1) { ext = true; return false; }

    bool released = (sc & 0x80) != 0;
    uint8_t key = sc & 0x7F;
    bool was_ext = ext;
    ext = false;

    if (key == 0x2A || key == 0x36) { shift = !released; return false; }
    if (key == 0x1D)                { ctrl = !released;  return false; }
    if (key == 0x38)                { alt = !released;   return false; }
    if (released) return false;

    ev->sc = key;
    ev->ext = was_ext;
    ev->shift = shift;
    ev->ctrl = ctrl;
    ev->alt = alt;
    ev->ch = 0;
    if (!was_ext && !ctrl && !alt) ev->ch = shift ? shifted_keyboard_map[key] : keyboard_map[key];
    return true;
}
