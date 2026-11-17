// gui.c - see gui.h
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "gui.h"
#include "fbcon.h"

extern uint8_t vga_font_8x8[256][8];   // bit 0 is the left pixel

static uint32_t g_canvas[GUI_W * GUI_H];
static int g_cx0, g_cy0, g_cx1 = GUI_W, g_cy1 = GUI_H;   // clip rectangle [x0, x1) x [y0, y1)

bool gui_begin(void)
{
    if (!fbcon_enabled()) return false;
    fbcon_suspend();
    gui_unclip();
    for (int i = 0; i < GUI_W * GUI_H; i++) g_canvas[i] = 0;
    return true;
}

void gui_end(void) { fbcon_resume(); }

void gui_present(void) { fbcon_blit(g_canvas, GUI_W, 0, 0, GUI_W, GUI_H); }

void gui_clip(int x, int y, int w, int h)
{
    g_cx0 = x < 0 ? 0 : x;
    g_cy0 = y < 0 ? 0 : y;
    g_cx1 = x + w > GUI_W ? GUI_W : x + w;
    g_cy1 = y + h > GUI_H ? GUI_H : y + h;
}

void gui_unclip(void) { g_cx0 = 0; g_cy0 = 0; g_cx1 = GUI_W; g_cy1 = GUI_H; }

static inline void blend_px(int x, int y, uint32_t rgb, uint32_t alpha)
{
    if (x < g_cx0 || y < g_cy0 || x >= g_cx1 || y >= g_cy1 || alpha == 0) return;
    uint32_t *p = &g_canvas[y * GUI_W + x];
    if (alpha >= 255) { *p = rgb; return; }
    uint32_t d = *p, inv = 255 - alpha;
    uint32_t r = (((rgb >> 16) & 255) * alpha + ((d >> 16) & 255) * inv + 127) / 255;
    uint32_t g = (((rgb >> 8) & 255) * alpha + ((d >> 8) & 255) * inv + 127) / 255;
    uint32_t b = ((rgb & 255) * alpha + (d & 255) * inv + 127) / 255;
    *p = (r << 16) | (g << 8) | b;
}

void gui_fill(int x, int y, int w, int h, uint32_t rgb)
{
    int x0 = x < g_cx0 ? g_cx0 : x, y0 = y < g_cy0 ? g_cy0 : y;
    int x1 = x + w > g_cx1 ? g_cx1 : x + w, y1 = y + h > g_cy1 ? g_cy1 : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = &g_canvas[yy * GUI_W];
        for (int xx = x0; xx < x1; xx++) row[xx] = rgb;
    }
}

// Coverage 0..255 of a pixel at (dx, dy) from the centre of a corner arc of radius r (dx, dy >= 1 inside the corner zone).
static uint32_t corner_alpha(int dx, int dy, int r)
{
    int d2 = dx * dx + dy * dy, outer = r * r, inner = (r - 1) * (r - 1);
    if (d2 >= outer) return 0;
    if (d2 <= inner) return 255;
    return (uint32_t)(255 * (outer - d2) / (outer - inner));
}

// Mask for pixel (px, py) of a w x h box with corner radius r.
static uint32_t mask_alpha(int px, int py, int w, int h, int r)
{
    if (r <= 0) return 255;
    int dx = 0, dy = 0;
    if (px < r) dx = r - px; else if (px >= w - r) dx = px - (w - r) + 1;
    if (py < r) dy = r - py; else if (py >= h - r) dy = py - (h - r) + 1;
    if (dx <= 0 || dy <= 0) return 255;
    return corner_alpha(dx, dy, r);
}

void gui_round(int x, int y, int w, int h, int r, uint32_t rgb)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) blend_px(x + xx, y + yy, rgb, mask_alpha(xx, yy, w, h, r));
}

void gui_circle(int cx, int cy, int radius, uint32_t rgb)
{
    gui_round(cx - radius, cy - radius, radius * 2, radius * 2, radius, rgb);
}

int gui_text_width(const char *s, int size)
{
    int n = 0;
    while (s[n]) n++;
    return n * size;
}

// Source pixel of an 8x8 glyph (0 outside it).
static inline int gpx(const uint8_t *g, int x, int y)
{
    return (x < 0 || y < 0 || x > 7 || y > 7) ? 0 : (g[y] >> x) & 1;
}

// Size 16 doubles each pixel with Scale2x/EPX smoothing, which rounds the staircase corners of the 8x8 font.
static void glyph16(int x, int y, const uint8_t *g, uint32_t rgb)
{
    for (int sy = 0; sy < 8; sy++) {
        for (int sx = 0; sx < 8; sx++) {
            int p = gpx(g, sx, sy), a = gpx(g, sx, sy - 1), b = gpx(g, sx + 1, sy);
            int c = gpx(g, sx - 1, sy), d = gpx(g, sx, sy + 1);
            int o0 = p, o1 = p, o2 = p, o3 = p;
            if (c == a && c != d && a != b) o0 = a;
            if (a == b && a != c && b != d) o1 = b;
            if (d == c && d != b && c != a) o2 = c;
            if (b == d && b != a && d != c) o3 = d;
            if (o0) blend_px(x + sx * 2, y + sy * 2, rgb, 255);
            if (o1) blend_px(x + sx * 2 + 1, y + sy * 2, rgb, 255);
            if (o2) blend_px(x + sx * 2, y + sy * 2 + 1, rgb, 255);
            if (o3) blend_px(x + sx * 2 + 1, y + sy * 2 + 1, rgb, 255);
        }
    }
}

// Draws one glyph into a size x size box (nearest-neighbour scaling, except 16 which is smoothed).
static void glyph(int x, int y, uint8_t ch, uint32_t rgb, int size)
{
    const uint8_t *g = vga_font_8x8[ch];
    if (size == 16) { glyph16(x, y, g, rgb); return; }
    for (int oy = 0; oy < size; oy++) {
        uint8_t bits = g[oy * 8 / size];
        if (!bits) continue;
        for (int ox = 0; ox < size; ox++)
            if ((bits >> (ox * 8 / size)) & 1) blend_px(x + ox, y + oy, rgb, 255);
    }
}

int gui_text(int x, int y, const char *s, uint32_t rgb, int size, bool bold)
{
    for (; *s; s++, x += size) {
        if ((unsigned char)*s < 32) continue;
        glyph(x, y, (uint8_t)*s, rgb, size);
        if (bold) glyph(x + 1, y, (uint8_t)*s, rgb, size);
    }
    return x;
}

void gui_image(const gui_image_t *img, int x, int y, int w, int h, int radius)
{
    if (!img || !img->px || w <= 0 || h <= 0) return;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    for (int dy = 0; dy < h; dy++) {
        int sy = dy * img->h / h;
        for (int dx = 0; dx < w; dx++) {
            int sx = dx * img->w / w;
            uint32_t p = img->px[sy * img->w + sx];
            uint32_t a = (p >> 24) * mask_alpha(dx, dy, w, h, radius) / 255;
            blend_px(x + dx, y + dy, p & 0xFFFFFF, a);
        }
    }
}
