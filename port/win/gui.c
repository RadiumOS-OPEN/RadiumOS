/* gui.c - user32, gdi32 and the bits of winmm that Windows programs use to put a window on the screen.
 *
 * There is no window manager: the program's top-level window is a 32-bit pixel buffer (its client area, no frame)
 * and is shown through the kernel's frame buffer call, enlarged by a whole factor when it is small. Child windows draw
 * into their top-level window. Keyboard and mouse come from the raw PS/2 stream and are turned into WM_ messages here.
 * Not supported: real controls (BUTTON, EDIT, ...), dialogs, menus, regions, OpenGL/Direct3D, more than one visible window. */
#include "winapi.h"
#include "font8x8.h"
#include "../rt/radium_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <strings.h>

extern const radium_api_t *radium_api_ptr;
#define API (radium_api_ptr)

typedef void *HWND, *HDC, *HGDIOBJ;
typedef uint32_t WPARAM, COLORREF;
typedef int32_t  LPARAM, LRESULT;
typedef LRESULT (WINAPI *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef void (WINAPI *TIMERPROC)(HWND, UINT, UINT, DWORD);
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
typedef struct { LONG cx, cy; } SIZE;
typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG;
typedef struct { UINT style; WNDPROC proc; int cbCls, cbWnd; void *inst, *icon, *cursor, *bg; const char *menu, *name; } WNDCLASSA;
typedef struct { UINT size; UINT style; WNDPROC proc; int cbCls, cbWnd; void *inst, *icon, *cursor, *bg; const char *menu, *name; void *iconSm; } WNDCLASSEXA;
typedef struct { void *param, *inst, *menu; HWND parent; LONG cy, cx, y, x, style; const char *name, *cls; DWORD ex; } CREATESTRUCTA;
typedef struct { HDC hdc; BOOL erase; RECT rc; BOOL restore, incUpdate; BYTE reserved[32]; } PAINTSTRUCT;
typedef struct { DWORD size; LONG w, h; WORD planes, bpp; DWORD comp, sizeImage; LONG xppm, yppm; DWORD clrUsed, clrImportant; } BITMAPINFOHEADER;
typedef struct { LONG type, w, h, wbytes; WORD planes, bpp; void *bits; } BITMAP;

#define WM_CREATE 0x01
#define WM_DESTROY 0x02
#define WM_MOVE 0x03
#define WM_SIZE 0x05
#define WM_ACTIVATE 0x06
#define WM_SETFOCUS 0x07
#define WM_KILLFOCUS 0x08
#define WM_PAINT 0x0F
#define WM_CLOSE 0x10
#define WM_QUIT 0x12
#define WM_ERASEBKGND 0x14
#define WM_SHOWWINDOW 0x18
#define WM_SETTEXT 0x0C
#define WM_GETTEXT 0x0D
#define WM_GETTEXTLENGTH 0x0E
#define WM_NCCREATE 0x81
#define WM_NCDESTROY 0x82
#define WM_KEYDOWN 0x100
#define WM_KEYUP 0x101
#define WM_CHAR 0x102
#define WM_SYSKEYDOWN 0x104
#define WM_SYSKEYUP 0x105
#define WM_SYSCOMMAND 0x112
#define WM_TIMER 0x113
#define WM_MOUSEMOVE 0x200
#define WM_LBUTTONDOWN 0x201
#define WM_LBUTTONUP 0x202
#define WM_RBUTTONDOWN 0x204
#define WM_RBUTTONUP 0x205
#define WM_MBUTTONDOWN 0x207
#define WM_MBUTTONUP 0x208
#define SC_CLOSE 0xF060

#define SCREEN_W 1280
#define SCREEN_H 720

enum { O_BMP = 0xB1, O_PEN, O_BRUSH, O_FONT, O_DC, O_WND = 0xC1 };

typedef struct bmp { int type, stock, w, h, bpp, stride, bottomup, owns; uint8_t *bits; uint32_t pal[256]; } bmp_t;
typedef struct { int type, stock, style, width; COLORREF color; } pen_t;
typedef struct { int type, stock, style; COLORREF color; } brush_t;
typedef struct { int type, stock, cw, ch; } font_t;
typedef struct dc { int type; struct wnd *wnd; bmp_t *bm, *own_bm; pen_t *pen; brush_t *brush; font_t *font; COLORREF text, bk; int bkmode, talign, cx, cy, ox, oy; } dc_t;
typedef struct wclass { char name[64]; WNDPROC proc; UINT style; void *bg, *cursor, *icon; int cbWnd; struct wclass *next; } wclass_t;
typedef struct wnd {
    int type; wclass_t *cls; WNDPROC proc; DWORD style, ex; int x, y, w, h; char title[128];
    struct wnd *parent, *next; bmp_t *surf; int visible, invalid, id; LONG user; LONG extra[16]; void *inst;
} wnd_t;

/* ---- state ---------------------------------------------------------------------------------------------------- */

static wclass_t *g_classes;
static wnd_t *g_wnds, *g_front, *g_focus, *g_capture;
static MSG g_q[512];
static int g_qh, g_qt;
static int g_quit, g_quit_code;
static uint8_t g_vk[256];
static int g_gfx_open, g_dirty, g_cursor_vis = 1, g_mouse_used;
static uint32_t g_last_present;
static int g_mx, g_my, g_mfx, g_mfy;                      /* cursor in client pixels; fractions in 1/256 */
static int g_mbtn;
static int g_ext;                                         /* 0xE0 prefix seen */
static uint8_t g_toggle[256];                             /* caps/num/scroll lock state */
static struct { wnd_t *w; UINT id, ms; TIMERPROC proc; uint32_t due; int used; } g_timers[32];
static int g_next_timer = 0x1000;

static bmp_t st_bmp;
static pen_t st_pen[3] = { { O_PEN, 1, 0, 1, 0xFFFFFF }, { O_PEN, 1, 0, 1, 0x000000 }, { O_PEN, 1, 5, 0, 0 } };
static brush_t st_brush[6] = { { O_BRUSH, 1, 0, 0xFFFFFF }, { O_BRUSH, 1, 0, 0xC0C0C0 }, { O_BRUSH, 1, 0, 0x808080 },
                               { O_BRUSH, 1, 0, 0x404040 }, { O_BRUSH, 1, 0, 0x000000 }, { O_BRUSH, 1, 1, 0 } };
static brush_t st_dcbrush = { O_BRUSH, 1, 0, 0xFFFFFF };
static pen_t st_dcpen = { O_PEN, 1, 0, 1, 0 };
static font_t st_font = { O_FONT, 1, 8, 16 };
static int g_dummy_handle[8];

static uint32_t now_ms(void) { return API->millis(); }
static uint32_t rgb(COLORREF c) { return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }

/* ---- pixel buffers ----------------------------------------------------------------------------------------------- */

static int bmp_stride(int w, int bpp) { return ((w * bpp + 31) / 32) * 4; }

static bmp_t *bmp_new(int w, int h, int bpp)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if ((int64_t)w * h > 4096 * 4096) return NULL;
    bmp_t *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->type = O_BMP; b->w = w; b->h = h; b->bpp = bpp; b->stride = bmp_stride(w, bpp); b->owns = 1;
    b->bits = calloc(1, (size_t)b->stride * (size_t)h);
    if (!b->bits) { free(b); return NULL; }
    return b;
}

static uint8_t *rowp(const bmp_t *b, int y) { return b->bits + (size_t)(b->bottomup ? b->h - 1 - y : y) * (size_t)b->stride; }

static uint32_t getpx(const bmp_t *b, int x, int y)
{
    if (x < 0 || y < 0 || x >= b->w || y >= b->h) return 0;
    const uint8_t *r = rowp(b, y);
    switch (b->bpp) {
    case 32: return ((const uint32_t *)r)[x] & 0xFFFFFF;
    case 24: return r[x * 3] | (r[x * 3 + 1] << 8) | (r[x * 3 + 2] << 16);
    case 16: { uint32_t v = ((const uint16_t *)r)[x]; return (((v >> 10) & 31) * 255 / 31) << 16 | (((v >> 5) & 31) * 255 / 31) << 8 | ((v & 31) * 255 / 31); }
    case 8:  return b->pal[r[x]];
    default: return 0;
    }
}

static void putpx(bmp_t *b, int x, int y, uint32_t px)
{
    if (x < 0 || y < 0 || x >= b->w || y >= b->h) return;
    uint8_t *r = rowp(b, y);
    switch (b->bpp) {
    case 32: ((uint32_t *)r)[x] = px; break;
    case 24: r[x * 3] = (uint8_t)px; r[x * 3 + 1] = (uint8_t)(px >> 8); r[x * 3 + 2] = (uint8_t)(px >> 16); break;
    case 16: ((uint16_t *)r)[x] = (uint16_t)(((px >> 19) & 31) << 10 | ((px >> 11) & 31) << 5 | ((px >> 3) & 31)); break;
    case 8: { int best = 0; uint32_t bd = 0xFFFFFFFFu; for (int i = 0; i < 256; i++) { int dr = (int)((px >> 16) & 255) - (int)((b->pal[i] >> 16) & 255), dg = (int)((px >> 8) & 255) - (int)((b->pal[i] >> 8) & 255), db = (int)(px & 255) - (int)(b->pal[i] & 255); uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db); if (d < bd) { bd = d; best = i; } } r[x] = (uint8_t)best; break; }
    }
}

static void fill_rect(bmp_t *b, int x0, int y0, int x1, int y1, uint32_t px)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > b->w) x1 = b->w;
    if (y1 > b->h) y1 = b->h;
    if (x0 >= x1 || y0 >= y1) return;
    for (int y = y0; y < y1; y++) {
        if (b->bpp == 32) { uint32_t *r = (uint32_t *)rowp(b, y); for (int x = x0; x < x1; x++) r[x] = px; }
        else for (int x = x0; x < x1; x++) putpx(b, x, y, px);
    }
}

/* ---- window and DC lookup -------------------------------------------------------------------------------------- */

static wnd_t *W(HWND h) { wnd_t *w = h; return (w && w->type == O_WND) ? w : NULL; }
static wnd_t *root_of(wnd_t *w) { while (w && w->parent) w = w->parent; return w; }

static void mark_dirty(dc_t *d) { if (d && d->bm && d->wnd) g_dirty = 1; }

static dc_t *dc_for(wnd_t *w)
{
    dc_t *d = calloc(1, sizeof *d);
    d->type = O_DC; d->wnd = w;
    wnd_t *r = root_of(w);
    if (r && r->surf) d->bm = r->surf; else { if (!st_bmp.bits) { st_bmp.type = O_BMP; st_bmp.stock = 1; st_bmp.w = st_bmp.h = 1; st_bmp.bpp = 32; st_bmp.stride = 4; st_bmp.bits = calloc(1, 4); } d->bm = &st_bmp; }
    for (wnd_t *c = w; c && c->parent; c = c->parent) { d->ox += c->x; d->oy += c->y; }
    d->pen = &st_pen[1]; d->brush = &st_brush[0]; d->font = &st_font;
    d->text = 0; d->bk = 0xFFFFFF; d->bkmode = 2;
    return d;
}

static dc_t *D(HDC h) { dc_t *d = h; return (d && d->type == O_DC) ? d : NULL; }

/* ---- drawing primitives ------------------------------------------------------------------------------------------ */

static void draw_line(dc_t *d, int x0, int y0, int x1, int y1)
{
    if (d->pen->style == 5) return;
    uint32_t c = rgb(d->pen == &st_dcpen ? st_dcpen.color : d->pen->color);
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy, n = d->pen->width > 1 ? d->pen->width : 1;
    for (;;) {
        if (n == 1) putpx(d->bm, x0, y0, c); else fill_rect(d->bm, x0 - n / 2, y0 - n / 2, x0 - n / 2 + n, y0 - n / 2 + n, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static int brush_visible(const brush_t *b) { return b->style != 1; }

static COLORREF sys_color(int i)
{
    switch (i) { case 5: case 14: return 0xFFFFFF; case 8: case 18: return 0; case 4: case 15: return 0xF0F0F0; case 13: return 0xD77800; case 1: return 0x6A240A; default: return 0xC0C0C0; }
}

static void fill_with(dc_t *d, int x0, int y0, int x1, int y1, void *hbrush)
{
    uintptr_t v = (uintptr_t)hbrush;
    COLORREF c;
    if (v < 64) c = sys_color((int)v - 1);
    else { brush_t *b = hbrush; if (b->type != O_BRUSH || !brush_visible(b)) return; c = b == &st_dcbrush ? st_dcbrush.color : b->color; }
    fill_rect(d->bm, x0 + d->ox, y0 + d->oy, x1 + d->ox, y1 + d->oy, rgb(c));
    mark_dirty(d);
}

static void ellipse_path(dc_t *d, int l, int t, int r, int b, int fill)
{
    double a = (r - l) / 2.0, bb = (b - t) / 2.0, cx = l + a, cy = t + bb;
    uint32_t pc = rgb(d->pen->color), fc = rgb(d->brush->color);
    int prev_l = 0, prev_r = 0, have = 0;
    for (int y = t; y < b; y++) {
        double dy = (y + 0.5 - cy) / bb;
        if (dy * dy > 1.0) continue;
        double w = a * sqrt(1.0 - dy * dy);
        int xl = (int)(cx - w + 0.5), xr = (int)(cx + w + 0.5);
        if (fill && brush_visible(d->brush)) fill_rect(d->bm, xl, y, xr, y + 1, fc);
        if (d->pen->style != 5) {
            if (!have || y == t || y == b - 1) fill_rect(d->bm, xl, y, xr, y + 1, pc);
            else {
                fill_rect(d->bm, xl < prev_l ? xl : prev_l, y, (xl > prev_l ? xl : prev_l) + 1, y + 1, pc);
                fill_rect(d->bm, (xr < prev_r ? xr : prev_r) - 1, y, xr > prev_r ? xr : prev_r, y + 1, pc);
            }
        }
        prev_l = xl; prev_r = xr; have = 1;
    }
}

static void poly_fill(dc_t *d, const POINT *p, int n)
{
    if (!brush_visible(d->brush) || n < 3) return;
    int ymin = p[0].y, ymax = p[0].y;
    for (int i = 1; i < n; i++) { if (p[i].y < ymin) ymin = p[i].y; if (p[i].y > ymax) ymax = p[i].y; }
    if (ymin < 0) ymin = 0;
    if (ymax >= d->bm->h) ymax = d->bm->h - 1;
    int *xs = malloc(sizeof(int) * (size_t)n);
    uint32_t c = rgb(d->brush->color);
    for (int y = ymin; y <= ymax; y++) {
        int k = 0;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            if ((p[i].y <= y && p[j].y > y) || (p[j].y <= y && p[i].y > y))
                xs[k++] = p[i].x + (int)((int64_t)(y - p[i].y) * (p[j].x - p[i].x) / (p[j].y - p[i].y));
        }
        for (int a = 1; a < k; a++) { int v = xs[a], b = a - 1; while (b >= 0 && xs[b] > v) { xs[b + 1] = xs[b]; b--; } xs[b + 1] = v; }
        for (int a = 0; a + 1 < k; a += 2) fill_rect(d->bm, xs[a], y, xs[a + 1], y + 1, c);
    }
    free(xs);
}

/* ---- text ------------------------------------------------------------------------------------------------------- */

static void draw_glyph(dc_t *d, int x, int y, unsigned char ch, uint32_t fg, uint32_t bg, int opaque)
{
    int cw = d->font->cw, chh = d->font->ch;
    const unsigned char *g = gdi_font8x8[ch];
    for (int yy = 0; yy < chh; yy++) {
        unsigned bits = g[yy * 8 / chh];
        for (int xx = 0; xx < cw; xx++) {
            if (bits & (1u << (xx * 8 / cw))) putpx(d->bm, x + xx, y + yy, fg);
            else if (opaque) putpx(d->bm, x + xx, y + yy, bg);
        }
    }
}

static void text_run(dc_t *d, int x, int y, const char *s, int n)
{
    int cw = d->font->cw, chh = d->font->ch, wpx = n * cw;
    if ((d->talign & 6) == 6) x -= wpx / 2; else if (d->talign & 2) x -= wpx;
    if ((d->talign & 24) == 24) y -= chh * 3 / 4; else if (d->talign & 8) y -= chh;
    uint32_t fg = rgb(d->text), bg = rgb(d->bk);
    for (int i = 0; i < n; i++) draw_glyph(d, x + d->ox + i * cw, y + d->oy, (unsigned char)s[i], fg, bg, d->bkmode == 2);
    mark_dirty(d);
}

/* ---- blits ------------------------------------------------------------------------------------------------------- */

static uint32_t apply_rop(uint32_t rop, uint32_t s, uint32_t dst, uint32_t pat)
{
    switch (rop) {
    case 0x00CC0020: return s;
    case 0x00EE0086: return s | dst;
    case 0x008800C6: return s & dst;
    case 0x00660046: return s ^ dst;
    case 0x00330008: return ~s & 0xFFFFFF;
    case 0x00440328: return s & ~dst & 0xFFFFFF;
    case 0x00BB0226: return (~s | dst) & 0xFFFFFF;
    case 0x00000042: return 0;
    case 0x00FF0062: return 0xFFFFFF;
    case 0x00F00021: return pat;
    case 0x005A0049: return pat ^ dst;
    case 0x000F0001: return ~pat & 0xFFFFFF;
    case 0x00550009: return ~dst & 0xFFFFFF;
    default: return s;
    }
}

static void stretch(dc_t *d, int dx, int dy, int dw, int dh, const bmp_t *src, int sx, int sy, int sw, int sh, uint32_t rop)
{
    if (!dw || !dh || !sw || !sh) return;
    int flipx = (dw < 0) != (sw < 0), flipy = (dh < 0) != (sh < 0);
    if (dw < 0) { dx += dw; dw = -dw; }
    if (dh < 0) { dy += dh; dh = -dh; }
    if (sw < 0) { sx += sw; sw = -sw; }
    if (sh < 0) { sy += sh; sh = -sh; }
    dx += d->ox; dy += d->oy;
    uint32_t pat = rgb(d->brush->color);
    int x0 = dx < 0 ? -dx : 0, y0 = dy < 0 ? -dy : 0, x1 = dw, y1 = dh;
    if (dx + x1 > d->bm->w) x1 = d->bm->w - dx;
    if (dy + y1 > d->bm->h) y1 = d->bm->h - dy;
    int same = (sw == dw && sh == dh && !flipx && !flipy);
    for (int y = y0; y < y1; y++) {
        int syy = sy + (flipy ? (dh - 1 - y) : y) * sh / dh;
        if (same && src->bpp == 32 && d->bm->bpp == 32 && rop == 0x00CC0020 && sx >= 0 && sx + sw <= src->w && syy >= 0 && syy < src->h) {
            int xs = x0, xe = x1;
            memmove((uint32_t *)rowp(d->bm, dy + y) + dx + xs, (const uint32_t *)rowp(src, syy) + sx + xs, (size_t)(xe - xs) * 4);
            continue;
        }
        for (int x = x0; x < x1; x++) {
            int sxx = sx + (flipx ? (dw - 1 - x) : x) * sw / dw;
            uint32_t s = src ? getpx(src, sxx, syy) : 0;
            if (rop != 0x00CC0020) s = apply_rop(rop, s, getpx(d->bm, dx + x, dy + y), pat);
            putpx(d->bm, dx + x, dy + y, s);
        }
    }
    mark_dirty(d);
}

static bmp_t *dib_view(const void *bmi, const void *bits, bmp_t *v)
{
    const BITMAPINFOHEADER *h = bmi;
    memset(v, 0, sizeof *v);
    v->type = O_BMP; v->w = h->w; v->h = h->h < 0 ? -h->h : h->h; v->bpp = h->bpp; v->bottomup = h->h > 0;
    v->stride = bmp_stride(v->w, v->bpp); v->bits = (uint8_t *)bits;
    if (v->bpp <= 8) {
        const uint8_t *pal = (const uint8_t *)bmi + h->size;
        int n = h->clrUsed ? (int)h->clrUsed : 1 << v->bpp;
        for (int i = 0; i < n && i < 256; i++) v->pal[i] = pal[i * 4] | (pal[i * 4 + 1] << 8) | (pal[i * 4 + 2] << 16);
        if (v->bpp != 8) return NULL;                    /* 1 and 4 bit pictures are not supported */
    }
    if (v->bpp != 8 && v->bpp != 16 && v->bpp != 24 && v->bpp != 32) return NULL;
    return v;
}

/* ---- input ------------------------------------------------------------------------------------------------------ */

static const uint8_t k_vk_main[0x59] = {
    0, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0xBD, 0xBB, 0x08, 0x09,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0xDB, 0xDD, 0x0D, 0xA2, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xBA, 0xDE, 0xC0, 0xA0, 0xDC, 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', 0xBC, 0xBE, 0xBF, 0xA1, 0x6A, 0xA4, 0x20, 0x14, 0x70, 0x71, 0x72, 0x73, 0x74,
    0x75, 0x76, 0x77, 0x78, 0x79, 0x90, 0x91, 0x67, 0x68, 0x69, 0x6D, 0x64, 0x65, 0x66, 0x6B, 0x61,
    0x62, 0x63, 0x60, 0x6E, 0, 0, 0, 0x7A, 0x7B,
};

static int vk_of_scan(int make, int ext)
{
    if (ext) {
        switch (make) {
        case 0x1C: return 0x0D; case 0x1D: return 0xA3; case 0x35: return 0x6F; case 0x38: return 0xA5;
        case 0x47: return 0x24; case 0x48: return 0x26; case 0x49: return 0x21; case 0x4B: return 0x25; case 0x4D: return 0x27;
        case 0x4F: return 0x23; case 0x50: return 0x28; case 0x51: return 0x22; case 0x52: return 0x2D; case 0x53: return 0x2E;
        case 0x5B: return 0x5B; case 0x5C: return 0x5C; default: return 0;
        }
    }
    return make < 0x59 ? k_vk_main[make] : 0;
}

static void recompute_generic(void)
{
    g_vk[0x10] = g_vk[0xA0] | g_vk[0xA1];
    g_vk[0x11] = g_vk[0xA2] | g_vk[0xA3];
    g_vk[0x12] = g_vk[0xA4] | g_vk[0xA5];
}

static void post(HWND h, UINT m, WPARAM w, LPARAM l)
{
    int n = (g_qh + 1) & 511;
    if (n == g_qt) return;                                   /* queue full: drop */
    MSG *q = &g_q[g_qh];
    q->hwnd = h; q->message = m; q->wParam = w; q->lParam = l; q->time = now_ms(); q->pt.x = g_mx; q->pt.y = g_my;
    g_qh = n;
}

static wnd_t *target_window(void)
{
    if (g_focus && g_focus->type == O_WND) return g_focus;
    return g_front;
}

static int present_geometry(int *ow, int *oh)
{
    bmp_t *s = g_front ? g_front->surf : NULL;
    if (!s) { *ow = SCREEN_W; *oh = SCREEN_H; return 1; }
    if (s->w > SCREEN_W || s->h > SCREEN_H) {
        if ((int64_t)s->w * SCREEN_H > (int64_t)s->h * SCREEN_W) { *ow = SCREEN_W; *oh = (int)((int64_t)s->h * SCREEN_W / s->w); }
        else { *oh = SCREEN_H; *ow = (int)((int64_t)s->w * SCREEN_H / s->h); }
        return 1;
    }
    int k = SCREEN_W / s->w < SCREEN_H / s->h ? SCREEN_W / s->w : SCREEN_H / s->h;
    if (k < 1) k = 1;
    *ow = s->w * k; *oh = s->h * k;
    return k;
}

static void key_event(int make, int ext, int down)
{
    int vk = vk_of_scan(make, ext);
    if (!vk) return;
    int was = g_vk[vk];
    g_vk[vk] = (uint8_t)down;
    recompute_generic();
    if (down && !was && (vk == 0x14 || vk == 0x90 || vk == 0x91)) g_toggle[vk] ^= 1;
    if (down && g_vk[0x11] && g_vk[0x12] && vk == 'Q') { fprintf(stderr, "winrun: Ctrl+Alt+Q\n"); exit(130); }
    int generic = vk == 0xA0 || vk == 0xA1 ? 0x10 : (vk == 0xA2 || vk == 0xA3 ? 0x11 : (vk == 0xA4 || vk == 0xA5 ? 0x12 : vk));
    int sys = g_vk[0x12] || vk == 0x12 || generic == 0x12;
    LPARAM l = 1 | (make << 16) | (ext << 24) | (was ? 1 << 30 : 0) | (down ? 0 : (int)0xC0000000 /* previous + transition */);
    if (sys) l |= 1 << 29;
    post(target_window(), down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP), (WPARAM)generic, l);
}

static void mouse_event_pump(void)
{
    int dx, dy, btn;
    if (!API->mouse_poll(&dx, &dy, &btn)) return;
    g_mouse_used = 1;
    bmp_t *s = g_front ? g_front->surf : NULL;
    int ow, oh; present_geometry(&ow, &oh);
    int pw = s ? s->w : SCREEN_W, ph = s ? s->h : SCREEN_H;
    if (dx || dy) {
        g_mfx += dx * 256 * pw / ow; g_mfy += dy * 256 * ph / oh;
        g_mx += g_mfx >> 8; g_my += g_mfy >> 8; g_mfx &= 255; g_mfy &= 255;
        if (g_mx < 0) g_mx = 0;
        if (g_mx >= pw) g_mx = pw - 1;
        if (g_my < 0) g_my = 0;
        if (g_my >= ph) g_my = ph - 1;
    }
    WPARAM mk = (btn & 1 ? 1 : 0) | (btn & 2 ? 2 : 0) | (btn & 4 ? 0x10 : 0) | (g_vk[0x10] ? 4 : 0) | (g_vk[0x11] ? 8 : 0);
    wnd_t *t = g_capture ? g_capture : g_front;
    LPARAM pos = (g_mx & 0xFFFF) | (g_my << 16);
    if (dx || dy) post(t, WM_MOUSEMOVE, mk, pos);
    static const UINT downm[3] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN }, upm[3] = { WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP };
    static const int vks[3] = { 1, 2, 4 };
    for (int b = 0; b < 3; b++) {
        int was = g_mbtn & (1 << b), is = btn & (1 << b);
        if (was == is) continue;
        g_vk[vks[b]] = is != 0;
        post(t, is ? downm[b] : upm[b], mk, pos);
    }
    g_mbtn = btn;
}

static void pump(void)
{
    int c;
    while ((c = API->key_poll()) >= 0) {
        if (c == 0xE0) { g_ext = 1; continue; }
        if (c == 0xE1) continue;
        key_event(c & 0x7F, g_ext, !(c & 0x80));
        g_ext = 0;
    }
    mouse_event_pump();
    uint32_t t = now_ms();
    for (int i = 0; i < 32; i++) {
        if (!g_timers[i].used || (int32_t)(t - g_timers[i].due) < 0) continue;
        g_timers[i].due = t + (g_timers[i].ms ? g_timers[i].ms : 1);
        post(g_timers[i].w, WM_TIMER, g_timers[i].id, (LPARAM)(uintptr_t)g_timers[i].proc);
    }
}

/* ---- presenting ---------------------------------------------------------------------------------------------------- */

static const char *const k_cursor[16] = {
    "X          ", "XX         ", "X.X        ", "X..X       ", "X...X      ", "X....X     ", "X.....X    ", "X......X   ",
    "X.......X  ", "X........X ", "X.....XXXXX", "X..X..X    ", "X.X X..X   ", "XX  X..X   ", "X    X..X  ", "     XXXX  " };

static void shutdown_gfx(void)
{
    if (g_gfx_open) { g_gfx_open = 0; API->gfx_close(); }
}

void win_gui_shutdown(void) { shutdown_gfx(); }

static void present_now(void)
{
    wnd_t *w = g_front;
    if (!w || !w->surf) return;
    if (!g_gfx_open) { API->gfx_open((uint32_t)w->surf->w, (uint32_t)w->surf->h); g_gfx_open = 1; atexit(shutdown_gfx); }
    bmp_t *s = w->surf;
    int ow, oh; present_geometry(&ow, &oh);
    static uint32_t *tmp; static size_t tmp_n;
    const uint32_t *px = (const uint32_t *)s->bits;
    uint32_t pw = (uint32_t)s->w, ph = (uint32_t)s->h;
    int need_copy = (g_cursor_vis && g_mouse_used) || s->w > SCREEN_W || s->h > SCREEN_H;
    if (need_copy) {
        uint32_t cw = ow > s->w ? (uint32_t)s->w : (uint32_t)ow, chh = oh > s->h ? (uint32_t)s->h : (uint32_t)oh;
        size_t n = (size_t)cw * chh;
        if (tmp_n < n) { free(tmp); tmp = malloc(n * 4); tmp_n = tmp ? n : 0; }
        if (tmp) {
            if (cw == (uint32_t)s->w && chh == (uint32_t)s->h) memcpy(tmp, s->bits, n * 4);
            else for (uint32_t y = 0; y < chh; y++) for (uint32_t x = 0; x < cw; x++) tmp[y * cw + x] = ((const uint32_t *)s->bits)[(size_t)(y * (uint32_t)s->h / chh) * (size_t)s->w + x * (uint32_t)s->w / cw];
            if (g_cursor_vis && g_mouse_used) {
                int cx = g_mx * (int)cw / s->w, cy = g_my * (int)chh / s->h;
                for (int yy = 0; yy < 16; yy++) for (int xx = 0; xx < 11; xx++) {
                    char ch = k_cursor[yy][xx];
                    int px_x = cx + xx, px_y = cy + yy;
                    if (ch == ' ' || px_x >= (int)cw || px_y >= (int)chh) continue;
                    tmp[(size_t)px_y * cw + px_x] = ch == 'X' ? 0x000000 : 0xFFFFFF;
                }
            }
            px = tmp; pw = cw; ph = chh;
        }
    }
    API->gfx_present(px, pw, ph);
    g_dirty = 0;
    g_last_present = now_ms();
}

static void flush(void)
{
    if ((g_dirty || (g_cursor_vis && g_mouse_used)) && g_front && (now_ms() - g_last_present) >= 8) present_now();
}

void win_gui_tick(void) { pump(); flush(); }

/* ---- classes and windows -------------------------------------------------------------------------------------- */

static LRESULT WINAPI DefWindowProcA(HWND, UINT, WPARAM, LPARAM);
static LRESULT WINAPI SendMessageA(HWND, UINT, WPARAM, LPARAM);

static wclass_t *find_class(const char *name)
{
    if ((uintptr_t)name < 0x10000) return NULL;
    for (wclass_t *c = g_classes; c; c = c->next) if (!strcasecmp(c->name, name)) return c;
    return NULL;
}

static WORD WINAPI RegisterClassA(const WNDCLASSA *wc)
{
    if ((uintptr_t)wc->name < 0x10000) return 0;
    wclass_t *c = find_class(wc->name);
    if (!c) { c = calloc(1, sizeof *c); strncpy(c->name, wc->name, sizeof c->name - 1); c->next = g_classes; g_classes = c; }
    c->proc = wc->proc; c->style = wc->style; c->bg = wc->bg; c->cursor = wc->cursor; c->icon = wc->icon; c->cbWnd = wc->cbWnd;
    return (WORD)(0xC001 + (uintptr_t)c % 0x3000);
}
static WORD WINAPI RegisterClassExA(const WNDCLASSEXA *wc)
{
    WNDCLASSA a = { wc->style, wc->proc, wc->cbCls, wc->cbWnd, wc->inst, wc->icon, wc->cursor, wc->bg, wc->menu, wc->name };
    return RegisterClassA(&a);
}
static BOOL WINAPI UnregisterClassA(const char *n, void *inst) { (void)inst; (void)n; return TRUE; }

static void narrow(const WCHAR *w, char *out, size_t cap)
{
    size_t i = 0;
    if ((uintptr_t)w < 0x10000) { out[0] = 0; return; }
    for (; w[i] && i < cap - 1; i++) out[i] = w[i] < 128 ? (char)w[i] : '?';
    out[i] = 0;
}
static WORD WINAPI RegisterClassW(const void *wc)
{
    const WNDCLASSA *a = wc; char name[64]; narrow((const WCHAR *)a->name, name, sizeof name);
    WNDCLASSA c = *a; c.name = name; return RegisterClassA(&c);
}
static WORD WINAPI RegisterClassExW(const void *wc)
{
    const WNDCLASSEXA *a = wc; char name[64]; narrow((const WCHAR *)a->name, name, sizeof name);
    WNDCLASSA c = { a->style, a->proc, a->cbCls, a->cbWnd, a->inst, a->icon, a->cursor, a->bg, a->menu, name }; return RegisterClassA(&c);
}

static HWND WINAPI CreateWindowExA(DWORD ex, const char *cls, const char *title, DWORD style, int x, int y, int w, int h, HWND parent, void *menu, void *inst, void *param)
{
    wclass_t *c = find_class(cls);
    static wclass_t builtin;
    if (!c) { c = &builtin; if (!builtin.proc) { builtin.proc = (WNDPROC)DefWindowProcA; strcpy(builtin.name, "builtin"); } }
    wnd_t *wn = calloc(1, sizeof *wn);
    wn->type = O_WND; wn->cls = c; wn->proc = c->proc; wn->style = style; wn->ex = ex; wn->parent = W(parent); wn->inst = inst;
    if (title) strncpy(wn->title, title, sizeof wn->title - 1);
    int child = (style & 0x40000000) != 0;
    if (!child && (style & 0x80000000) == 0 && (style & 0x00C00000) == 0x00C00000) { /* overlapped with caption: sizes are outer sizes, we show only the client area */ }
    wn->x = x == (int)0x80000000 ? 0 : x; wn->y = y == (int)0x80000000 ? 0 : y;
    wn->w = w == (int)0x80000000 ? 640 : w; wn->h = h == (int)0x80000000 ? 480 : h;
    if (wn->w < 1) wn->w = 1;
    if (wn->h < 1) wn->h = 1;
    if (!wn->parent) { wn->parent = NULL; wn->x = wn->y = 0; wn->surf = bmp_new(wn->w, wn->h, 32); if (!wn->surf) { free(wn); return NULL; } }
    wn->id = (int)(intptr_t)menu;
    wn->next = g_wnds; g_wnds = wn;
    CREATESTRUCTA cs = { param, inst, menu, parent, h, w, y, x, (LONG)style, title, cls, ex };
    SendMessageA(wn, WM_NCCREATE, 0, (LPARAM)(uintptr_t)&cs);
    if ((int)SendMessageA(wn, WM_CREATE, 0, (LPARAM)(uintptr_t)&cs) == -1) {
        g_wnds = wn->next; if (wn->surf) { free(wn->surf->bits); free(wn->surf); } free(wn); return NULL;
    }
    SendMessageA(wn, WM_SIZE, 0, (LPARAM)((wn->w & 0xFFFF) | (wn->h << 16)));
    SendMessageA(wn, WM_MOVE, 0, 0);
    if (style & 0x10000000) {                                /* WS_VISIBLE */
        wn->visible = 1; wn->invalid = 1;
        if (!wn->parent) { g_front = wn; if (!g_focus) g_focus = wn; }
    }
    return wn;
}
static HWND WINAPI CreateWindowExW(DWORD ex, const WCHAR *cls, const WCHAR *title, DWORD style, int x, int y, int w, int h, HWND parent, void *menu, void *inst, void *param)
{
    char c[64], t[128]; narrow(cls, c, sizeof c); narrow(title, t, sizeof t);
    if ((uintptr_t)cls < 0x10000) return NULL;
    return CreateWindowExA(ex, c, t, style, x, y, w, h, parent, menu, inst, param);
}

static BOOL WINAPI ShowWindow(HWND h, int cmd)
{
    wnd_t *w = W(h);
    if (!w) return FALSE;
    int was = w->visible;
    w->visible = cmd != 0;
    if (w->visible) {
        w->invalid = 1;
        if (!w->parent) {
            g_front = w; g_focus = w;
            SendMessageA(w, WM_SHOWWINDOW, 1, 0);
            SendMessageA(w, WM_ACTIVATE, 1, 0);
            SendMessageA(w, WM_SETFOCUS, 0, 0);
            g_dirty = 1;
        }
    }
    return was;
}

static BOOL WINAPI DestroyWindow(HWND h)
{
    wnd_t *w = W(h);
    if (!w) return FALSE;
    for (wnd_t *c = g_wnds; c; c = c->next) if (c->parent == w && c->type == O_WND) DestroyWindow(c);
    SendMessageA(w, WM_DESTROY, 0, 0);
    SendMessageA(w, WM_NCDESTROY, 0, 0);
    for (wnd_t **p = &g_wnds; *p; p = &(*p)->next) if (*p == w) { *p = w->next; break; }
    if (g_front == w) { g_front = NULL; for (wnd_t *c = g_wnds; c; c = c->next) if (!c->parent && c->visible) { g_front = c; break; } }
    if (g_focus == w) g_focus = g_front;
    if (g_capture == w) g_capture = NULL;
    for (int i = 0; i < 32; i++) if (g_timers[i].w == w) g_timers[i].used = 0;
    w->type = 0;                                             /* stale handles are caught by W() */
    if (w->surf) { free(w->surf->bits); free(w->surf); w->surf = NULL; }
    return TRUE;
}

static void paint_background(dc_t *d, wnd_t *w)
{
    void *bg = w->cls ? w->cls->bg : NULL;
    if (bg) fill_with(d, 0, 0, w->w, w->h, bg);
}

static LRESULT WINAPI DefWindowProcA(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    wnd_t *w = W(h);
    if (!w) return 0;
    switch (m) {
    case WM_NCCREATE: return 1;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_SYSKEYDOWN: if (wp == 0x73 /* F4 */) { SendMessageA(h, WM_SYSCOMMAND, SC_CLOSE, 0); } return 0;
    case WM_SYSCOMMAND: if ((wp & 0xFFF0) == SC_CLOSE) SendMessageA(h, WM_CLOSE, 0, 0); return 0;
    case WM_ERASEBKGND: { dc_t *d = D((HDC)(uintptr_t)wp); if (d) paint_background(d, w); return 1; }
    case WM_PAINT: { w->invalid = 0; return 0; }
    case WM_SETTEXT: strncpy(w->title, (const char *)(uintptr_t)lp, sizeof w->title - 1); return 1;
    case WM_GETTEXT: { size_t n = strlen(w->title); if ((size_t)wp <= n) n = wp ? wp - 1 : 0; if (wp) { memcpy((char *)(uintptr_t)lp, w->title, n); ((char *)(uintptr_t)lp)[n] = 0; } return (LRESULT)n; }
    case WM_GETTEXTLENGTH: return (LRESULT)strlen(w->title);
    default: return 0;
    }
}
static LRESULT WINAPI DefWindowProcW(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_SETTEXT) { char t[128]; narrow((const WCHAR *)(uintptr_t)lp, t, sizeof t); return DefWindowProcA(h, m, wp, (LPARAM)(uintptr_t)t); }
    if (m == WM_GETTEXT || m == WM_GETTEXTLENGTH) return 0;
    return DefWindowProcA(h, m, wp, lp);
}

static LRESULT WINAPI SendMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    wnd_t *w = W(h);
    if (!w) return 0;
    return w->proc ? w->proc(h, m, wp, lp) : DefWindowProcA(h, m, wp, lp);
}
static LRESULT WINAPI SendMessageW(HWND h, UINT m, WPARAM wp, LPARAM lp) { return SendMessageA(h, m, wp, lp); }
static BOOL WINAPI PostMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp) { if (!h) return FALSE; post(h, m, wp, lp); return TRUE; }
static BOOL WINAPI PostThreadMessageA(DWORD t, UINT m, WPARAM wp, LPARAM lp) { (void)t; post(NULL, m, wp, lp); return TRUE; }
static LRESULT WINAPI CallWindowProcA(WNDPROC p, HWND h, UINT m, WPARAM wp, LPARAM lp) { return p(h, m, wp, lp); }
static void WINAPI PostQuitMessage(int code) { g_quit = 1; g_quit_code = code; }

static int next_paint(MSG *out, int remove)
{
    for (wnd_t *w = g_wnds; w; w = w->next) {
        if (w->type == O_WND && w->visible && w->invalid) {
            memset(out, 0, sizeof *out); out->hwnd = w; out->message = WM_PAINT; out->time = now_ms();
            (void)remove;
            return 1;
        }
    }
    return 0;
}

static int get_msg(MSG *msg, HWND filter, UINT lo, UINT hi, int remove, int block)
{
    (void)filter;
    for (;;) {
        pump();
        for (int i = g_qt; i != g_qh; i = (i + 1) & 511) {
            MSG *q = &g_q[i];
            if (lo || hi) { if (q->message < lo || q->message > hi) continue; }
            *msg = *q;
            if (remove) {
                for (int j = i; j != g_qt; j = (j - 1) & 511) g_q[j] = g_q[(j - 1) & 511];
                g_qt = (g_qt + 1) & 511;
            }
            return 1;
        }
        if (g_quit && !lo && !hi) { memset(msg, 0, sizeof *msg); msg->message = WM_QUIT; msg->wParam = (WPARAM)g_quit_code; if (remove) g_quit = 0; return 2; }
        if (!(lo || hi) || (lo <= WM_PAINT && hi >= WM_PAINT)) if (next_paint(msg, remove)) return 1;
        flush();
        if (!block) return 0;
        API->sleep_ms(1);
    }
}

static BOOL WINAPI GetMessageA(MSG *msg, HWND h, UINT lo, UINT hi)
{
    int r = get_msg(msg, h, lo, hi, 1, 1);
    return r == 2 ? 0 : 1;
}
static BOOL WINAPI PeekMessageA(MSG *msg, HWND h, UINT lo, UINT hi, UINT flags)
{
    int r = get_msg(msg, h, lo, hi, (flags & 1) != 0, 0);
    if (r == 2) return TRUE;
    return r != 0;
}

static int char_of(int vk, int shift, int caps, int ctrl)
{
    if (vk >= 'A' && vk <= 'Z') { if (ctrl) return vk - 'A' + 1; return ((shift != 0) ^ (caps != 0)) ? vk : vk + 32; }
    if (vk >= '0' && vk <= '9') return shift ? ")!@#$%^&*("[vk - '0'] : vk;
    if (vk >= 0x60 && vk <= 0x69) return '0' + (vk - 0x60);
    switch (vk) {
    case 0x20: return ' '; case 0x0D: return '\r'; case 0x09: return '\t'; case 0x08: return 8; case 0x1B: return 27;
    case 0x6A: return '*'; case 0x6B: return '+'; case 0x6D: return '-'; case 0x6E: return '.'; case 0x6F: return '/';
    case 0xBA: return shift ? ':' : ';'; case 0xBB: return shift ? '+' : '='; case 0xBC: return shift ? '<' : ',';
    case 0xBD: return shift ? '_' : '-'; case 0xBE: return shift ? '>' : '.'; case 0xBF: return shift ? '?' : '/';
    case 0xC0: return shift ? '~' : '`'; case 0xDB: return shift ? '{' : '['; case 0xDC: return shift ? '|' : '\\';
    case 0xDD: return shift ? '}' : ']'; case 0xDE: return shift ? '"' : '\'';
    default: return 0;
    }
}

static BOOL WINAPI TranslateMessage(const MSG *m)
{
    if (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN) return FALSE;
    if (g_vk[0x12] && !g_vk[0x11]) return FALSE;
    int c = char_of((int)m->wParam, g_vk[0x10], g_toggle[0x14], g_vk[0x11]);
    if (c) post(m->hwnd, WM_CHAR, (WPARAM)c, m->lParam & 0x00FFFFFF);
    return TRUE;
}

static LRESULT WINAPI DispatchMessageA(const MSG *m)
{
    wnd_t *w = W(m->hwnd);
    if (!w) return 0;
    if (m->message == WM_TIMER && m->lParam) { ((TIMERPROC)(uintptr_t)m->lParam)(m->hwnd, WM_TIMER, m->wParam, now_ms()); return 0; }
    LRESULT r = w->proc ? w->proc(m->hwnd, m->message, m->wParam, m->lParam) : DefWindowProcA(m->hwnd, m->message, m->wParam, m->lParam);
    if (m->message == WM_PAINT && W(m->hwnd)) w->invalid = 0;                /* a handler that forgot BeginPaint must not be sent WM_PAINT forever */
    return r;
}

static UINT WINAPI SetTimer(HWND h, UINT id, UINT ms, TIMERPROC proc)
{
    int slot = -1;
    for (int i = 0; i < 32; i++) if (g_timers[i].used && g_timers[i].w == W(h) && h && g_timers[i].id == id) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < 32; i++) if (!g_timers[i].used) { slot = i; break; }
    if (slot < 0) return 0;
    if (!h) id = (UINT)g_next_timer++;
    g_timers[slot].used = 1; g_timers[slot].w = W(h); g_timers[slot].id = id; g_timers[slot].ms = ms; g_timers[slot].proc = proc; g_timers[slot].due = now_ms() + (ms ? ms : 1);
    return id;
}
static BOOL WINAPI KillTimer(HWND h, UINT id)
{
    for (int i = 0; i < 32; i++) if (g_timers[i].used && g_timers[i].id == id && g_timers[i].w == W(h)) { g_timers[i].used = 0; return TRUE; }
    return FALSE;
}

/* ---- window properties ---------------------------------------------------------------------------------------------- */

static BOOL WINAPI GetClientRect(HWND h, RECT *r) { wnd_t *w = W(h); if (!w) return FALSE; r->left = r->top = 0; r->right = w->w; r->bottom = w->h; return TRUE; }
static BOOL WINAPI GetWindowRect(HWND h, RECT *r) { wnd_t *w = W(h); if (!w) return FALSE; r->left = w->x; r->top = w->y; r->right = w->x + w->w; r->bottom = w->y + w->h; return TRUE; }
static BOOL WINAPI AdjustWindowRect(RECT *r, DWORD style, BOOL menu) { (void)r; (void)style; (void)menu; return TRUE; }
static BOOL WINAPI AdjustWindowRectEx(RECT *r, DWORD style, BOOL menu, DWORD ex) { (void)ex; return AdjustWindowRect(r, style, menu); }

static void resize_window(wnd_t *w, int nw, int nh)
{
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw == w->w && nh == w->h) return;
    if (!w->parent) {
        bmp_t *n = bmp_new(nw, nh, 32);
        if (!n) return;
        if (w->surf) { free(w->surf->bits); free(w->surf); }
        w->surf = n;
        if (w->visible) g_dirty = 1;
    }
    w->w = nw; w->h = nh; w->invalid = 1;
    SendMessageA(w, WM_SIZE, 0, (LPARAM)((nw & 0xFFFF) | (nh << 16)));
}
static BOOL WINAPI MoveWindow(HWND h, int x, int y, int cw, int ch, BOOL repaint)
{
    wnd_t *w = W(h); (void)repaint;
    if (!w) return FALSE;
    if (w->parent) { w->x = x; w->y = y; }
    resize_window(w, cw, ch);
    return TRUE;
}
static BOOL WINAPI SetWindowPos(HWND h, HWND after, int x, int y, int cw, int ch, UINT flags)
{
    wnd_t *w = W(h); (void)after;
    if (!w) return FALSE;
    if (!(flags & 2) && w->parent) { w->x = x; w->y = y; }
    if (!(flags & 1)) resize_window(w, cw, ch);
    if (flags & 0x40) ShowWindow(h, 1);
    if (flags & 0x80) ShowWindow(h, 0);
    return TRUE;
}

static BOOL WINAPI IsWindow(HWND h) { return W(h) != NULL; }
static BOOL WINAPI IsWindowVisible(HWND h) { wnd_t *w = W(h); return w && w->visible; }
static BOOL WINAPI IsIconic(HWND h) { (void)h; return FALSE; }
static BOOL WINAPI IsZoomed(HWND h) { (void)h; return FALSE; }
static HWND WINAPI GetParent(HWND h) { wnd_t *w = W(h); return w ? w->parent : NULL; }
static HWND WINAPI GetDesktopWindow(void) { return NULL; }
static HWND WINAPI GetForegroundWindow(void) { return g_front; }
static HWND WINAPI GetActiveWindow(void) { return g_front; }
static HWND WINAPI GetFocus(void) { return g_focus; }
static HWND WINAPI SetFocus(HWND h) { HWND old = g_focus; if (W(h)) g_focus = W(h); return old; }
static HWND WINAPI SetActiveWindow(HWND h) { HWND old = g_front; if (W(h) && !W(h)->parent) g_front = W(h); return old; }
static BOOL WINAPI SetForegroundWindow(HWND h) { SetActiveWindow(h); return TRUE; }
static BOOL WINAPI BringWindowToTop(HWND h) { SetActiveWindow(h); return TRUE; }
static HWND WINAPI SetCapture(HWND h) { HWND old = g_capture; g_capture = W(h); return old; }
static BOOL WINAPI ReleaseCapture(void) { g_capture = NULL; return TRUE; }
static HWND WINAPI GetCapture(void) { return g_capture; }
static BOOL WINAPI EnableWindow(HWND h, BOOL e) { (void)h; (void)e; return FALSE; }
static HWND WINAPI FindWindowA(const char *c, const char *t) { (void)c; (void)t; return NULL; }
static BOOL WINAPI UpdateWindow(HWND h) { wnd_t *w = W(h); if (w && w->invalid && w->visible) SendMessageA(w, WM_PAINT, 0, 0); return TRUE; }
static BOOL WINAPI InvalidateRect(HWND h, const RECT *r, BOOL erase) { (void)r; (void)erase; if (!h) { for (wnd_t *w = g_wnds; w; w = w->next) w->invalid = 1; return TRUE; } wnd_t *w = W(h); if (w) w->invalid = 1; return TRUE; }
static BOOL WINAPI ValidateRect(HWND h, const RECT *r) { (void)r; wnd_t *w = W(h); if (w) w->invalid = 0; return TRUE; }
static BOOL WINAPI GetUpdateRect(HWND h, RECT *r, BOOL erase) { (void)erase; wnd_t *w = W(h); if (!w || !w->invalid) return FALSE; if (r) { r->left = r->top = 0; r->right = w->w; r->bottom = w->h; } return TRUE; }
static BOOL WINAPI RedrawWindow(HWND h, const RECT *r, void *rgn, UINT flags) { (void)rgn; InvalidateRect(h, r, TRUE); if (flags & 0x100) UpdateWindow(h); return TRUE; }
static BOOL WINAPI SetWindowTextA(HWND h, const char *t) { wnd_t *w = W(h); if (!w) return FALSE; strncpy(w->title, t ? t : "", sizeof w->title - 1); return TRUE; }
static BOOL WINAPI SetWindowTextW(HWND h, const WCHAR *t) { char b[128]; narrow(t, b, sizeof b); return SetWindowTextA(h, b); }
static int WINAPI GetWindowTextA(HWND h, char *b, int n) { wnd_t *w = W(h); if (!w || n <= 0) return 0; strncpy(b, w->title, (size_t)n - 1); b[n - 1] = 0; return (int)strlen(b); }
static int WINAPI GetWindowTextLengthA(HWND h) { wnd_t *w = W(h); return w ? (int)strlen(w->title) : 0; }
static int WINAPI GetClassNameA(HWND h, char *b, int n) { wnd_t *w = W(h); if (!w || n <= 0) return 0; strncpy(b, w->cls->name, (size_t)n - 1); b[n - 1] = 0; return (int)strlen(b); }

static LONG WINAPI GetWindowLongA(HWND h, int i)
{
    wnd_t *w = W(h);
    if (!w) return 0;
    switch (i) {
    case -4: return (LONG)(uintptr_t)w->proc; case -16: return (LONG)w->style; case -20: return (LONG)w->ex; case -21: return w->user;
    case -12: return w->id; case -6: return (LONG)(uintptr_t)w->inst; case -8: return (LONG)(uintptr_t)w->parent;
    default: return (i >= 0 && i < 64 && i % 4 == 0) ? w->extra[i / 4] : 0;
    }
}
static LONG WINAPI SetWindowLongA(HWND h, int i, LONG v)
{
    wnd_t *w = W(h);
    if (!w) return 0;
    LONG old = GetWindowLongA(h, i);
    switch (i) {
    case -4: w->proc = (WNDPROC)(uintptr_t)v; break; case -16: w->style = (DWORD)v; break; case -20: w->ex = (DWORD)v; break;
    case -21: w->user = v; break; case -12: w->id = v; break;
    default: if (i >= 0 && i < 64 && i % 4 == 0) w->extra[i / 4] = v;
    }
    return old;
}
static DWORD WINAPI GetClassLongA(HWND h, int i) { wnd_t *w = W(h); if (!w) return 0; if (i == -10) return (DWORD)(uintptr_t)w->cls->bg; if (i == -12) return (DWORD)(uintptr_t)w->cls->cursor; return 0; }

/* ---- screen, cursor, keyboard state ----------------------------------------------------------------------------- */

static int WINAPI GetSystemMetrics(int i)
{
    switch (i) {
    case 0: case 16: case 61: case 78: return SCREEN_W;
    case 1: case 17: case 62: case 79: return SCREEN_H;
    case 2: case 3: return 17; case 4: return 23; case 5: case 6: return 1; case 7: case 8: return 3; case 15: return 20;
    case 32: case 33: return 4; case 80: return 1; case 43: return 3; case 75: return 1;
    default: return 0;
    }
}
static BOOL WINAPI SystemParametersInfoA(UINT a, UINT b, void *c, UINT d) { (void)a; (void)b; (void)c; (void)d; return TRUE; }
static BOOL WINAPI SetProcessDPIAware(void) { return TRUE; }
static BOOL WINAPI GetCursorPos(POINT *p) { p->x = g_mx; p->y = g_my; return TRUE; }
static BOOL WINAPI SetCursorPos(int x, int y)
{
    bmp_t *s = g_front ? g_front->surf : NULL;
    g_mx = x; g_my = y; g_mfx = g_mfy = 0;
    if (s) { if (g_mx < 0) g_mx = 0; if (g_mx >= s->w) g_mx = s->w - 1; if (g_my < 0) g_my = 0; if (g_my >= s->h) g_my = s->h - 1; }
    return TRUE;
}
static BOOL WINAPI ScreenToClient(HWND h, POINT *p) { (void)h; (void)p; return TRUE; }
static BOOL WINAPI ClientToScreen(HWND h, POINT *p) { (void)h; (void)p; return TRUE; }
static int g_show_count, g_cursor_null;
static void update_cursor_vis(void) { g_cursor_vis = g_show_count >= 0 && !g_cursor_null; g_dirty = 1; }
static int WINAPI ShowCursor(BOOL show) { g_show_count += show ? 1 : -1; update_cursor_vis(); return g_show_count; }
static void *WINAPI SetCursor(void *c) { static void *cur; void *old = cur; cur = c; g_cursor_null = c == NULL; update_cursor_vis(); return old; }
static void *WINAPI LoadCursorA(void *inst, const char *n) { (void)inst; (void)n; return &g_dummy_handle[0]; }
static void *WINAPI LoadIconA(void *inst, const char *n) { (void)inst; (void)n; return &g_dummy_handle[1]; }
static void *WINAPI LoadImageA(void *inst, const char *n, UINT t, int w, int h, UINT f) { (void)inst; (void)n; (void)t; (void)w; (void)h; (void)f; return &g_dummy_handle[2]; }
static BOOL WINAPI ClipCursor(const RECT *r) { (void)r; return TRUE; }
static BOOL WINAPI DestroyIcon(void *h) { (void)h; return TRUE; }
static int WINAPI GetDoubleClickTime(void) { return 500; }

static void sync_input(void) { pump(); }
static short WINAPI GetAsyncKeyState(int vk) { sync_input(); return (vk >= 0 && vk < 256 && g_vk[vk]) ? (short)0x8000 : 0; }
static short WINAPI GetKeyState(int vk)
{
    sync_input();
    if (vk < 0 || vk > 255) return 0;
    return (short)((g_vk[vk] ? 0x8000 : 0) | (g_toggle[vk] ? 1 : 0));
}
static BOOL WINAPI GetKeyboardState(uint8_t *k) { sync_input(); for (int i = 0; i < 256; i++) k[i] = (uint8_t)((g_vk[i] ? 0x80 : 0) | (g_toggle[i] ? 1 : 0)); return TRUE; }
static UINT WINAPI MapVirtualKeyA(UINT code, UINT type)
{
    if (type == 0) { for (int i = 1; i < 0x59; i++) if (k_vk_main[i] == code) return (UINT)i; return 0; }
    if (type == 1) return code < 0x59 ? k_vk_main[code] : 0;
    return (UINT)char_of((int)code, 0, 0, 0);
}
static void *WINAPI GetKeyboardLayout(DWORD t) { (void)t; return (void *)0x04090409; }

static UINT WINAPI RegisterWindowMessageA(const char *s) { (void)s; static UINT n = 0xC000; return n++; }
static BOOL WINAPI IsDialogMessageA(HWND h, MSG *m) { (void)h; (void)m; return FALSE; }
static int WINAPI TranslateAcceleratorA(HWND h, void *a, MSG *m) { (void)h; (void)a; (void)m; return 0; }
static void *WINAPI LoadAcceleratorsA(void *i, const char *n) { (void)i; (void)n; return &g_dummy_handle[3]; }
static void *WINAPI CreateMenu(void) { return &g_dummy_handle[4]; }
static BOOL WINAPI AppendMenuA(void *m, UINT f, UINT id, const char *s) { (void)m; (void)f; (void)id; (void)s; return TRUE; }
static BOOL WINAPI SetMenu(HWND h, void *m) { (void)h; (void)m; return TRUE; }
static BOOL WINAPI DrawMenuBar(HWND h) { (void)h; return TRUE; }
static BOOL WINAPI DestroyMenu(void *m) { (void)m; return TRUE; }
static BOOL WINAPI EnumDisplaySettingsA(const char *d, DWORD m, void *dm) { (void)d; (void)m; (void)dm; return FALSE; }
static LONG WINAPI ChangeDisplaySettingsA(void *dm, DWORD f) { (void)dm; (void)f; return 0; }

static int wsprintfA(char *buf, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = vsprintf(buf, fmt, ap); va_end(ap); return n; }
static int WINAPI wvsprintfA(char *buf, const char *fmt, va_list ap) { return vsprintf(buf, fmt, ap); }
static char *WINAPI CharUpperA(char *s) { if ((uintptr_t)s < 0x10000) { int c = (int)(uintptr_t)s; return (char *)(uintptr_t)((c >= 'a' && c <= 'z') ? c - 32 : c); } for (char *p = s; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32; return s; }
static char *WINAPI CharLowerA(char *s) { if ((uintptr_t)s < 0x10000) { int c = (int)(uintptr_t)s; return (char *)(uintptr_t)((c >= 'A' && c <= 'Z') ? c + 32 : c); } for (char *p = s; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32; return s; }

/* rectangles */
static BOOL WINAPI SetRect(RECT *r, int l, int t, int rr, int b) { r->left = l; r->top = t; r->right = rr; r->bottom = b; return TRUE; }
static BOOL WINAPI SetRectEmpty(RECT *r) { memset(r, 0, sizeof *r); return TRUE; }
static BOOL WINAPI CopyRect(RECT *d, const RECT *s) { *d = *s; return TRUE; }
static BOOL WINAPI OffsetRect(RECT *r, int x, int y) { r->left += x; r->right += x; r->top += y; r->bottom += y; return TRUE; }
static BOOL WINAPI InflateRect(RECT *r, int x, int y) { r->left -= x; r->right += x; r->top -= y; r->bottom += y; return TRUE; }
static BOOL WINAPI PtInRect(const RECT *r, POINT p) { return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }
static BOOL WINAPI IsRectEmpty(const RECT *r) { return r->right <= r->left || r->bottom <= r->top; }
static BOOL WINAPI EqualRect(const RECT *a, const RECT *b) { return !memcmp(a, b, sizeof *a); }
static BOOL WINAPI IntersectRect(RECT *d, const RECT *a, const RECT *b)
{
    d->left = a->left > b->left ? a->left : b->left; d->top = a->top > b->top ? a->top : b->top;
    d->right = a->right < b->right ? a->right : b->right; d->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    if (IsRectEmpty(d)) { memset(d, 0, sizeof *d); return FALSE; }
    return TRUE;
}
static BOOL WINAPI UnionRect(RECT *d, const RECT *a, const RECT *b)
{
    if (IsRectEmpty(a)) { *d = *b; return !IsRectEmpty(b); }
    if (IsRectEmpty(b)) { *d = *a; return TRUE; }
    d->left = a->left < b->left ? a->left : b->left; d->top = a->top < b->top ? a->top : b->top;
    d->right = a->right > b->right ? a->right : b->right; d->bottom = a->bottom > b->bottom ? a->bottom : b->bottom;
    return TRUE;
}

/* ---- DCs and GDI objects ------------------------------------------------------------------------------------------- */

static HDC WINAPI GetDC(HWND h) { return dc_for(W(h)); }
static HDC WINAPI GetWindowDC(HWND h) { return dc_for(W(h)); }
static int WINAPI ReleaseDC(HWND h, HDC d) { (void)h; free(d); return 1; }
static HDC WINAPI CreateCompatibleDC(HDC src)
{
    (void)src;
    dc_t *d = dc_for(NULL);
    d->wnd = NULL;
    d->own_bm = bmp_new(1, 1, 32);
    d->bm = d->own_bm;
    return d;
}
static BOOL WINAPI DeleteDC(HDC h) { dc_t *d = D(h); if (!d) return FALSE; if (d->own_bm) { free(d->own_bm->bits); free(d->own_bm); } free(d); return TRUE; }

static HDC WINAPI BeginPaint(HWND h, PAINTSTRUCT *ps)
{
    wnd_t *w = W(h);
    dc_t *d = dc_for(w);
    memset(ps, 0, sizeof *ps);
    ps->hdc = d;
    if (w) {
        ps->rc.right = w->w; ps->rc.bottom = w->h; ps->erase = 1;
        w->invalid = 0;
        if (SendMessageA(h, WM_ERASEBKGND, (WPARAM)(uintptr_t)d, 0)) ps->erase = 0;
    }
    return d;
}
static BOOL WINAPI EndPaint(HWND h, const PAINTSTRUCT *ps) { (void)h; g_dirty = 1; free(ps->hdc); return TRUE; }

static HGDIOBJ WINAPI GetStockObject(int i)
{
    switch (i) {
    case 0: case 1: case 2: case 3: case 4: case 5: return &st_brush[i];
    case 6: return &st_pen[0]; case 7: return &st_pen[1]; case 8: return &st_pen[2];
    case 18: return &st_dcbrush; case 19: return &st_dcpen;
    default: return &st_font;
    }
}
static HGDIOBJ WINAPI CreateSolidBrush(COLORREF c) { brush_t *b = calloc(1, sizeof *b); b->type = O_BRUSH; b->color = c; return b; }
static HGDIOBJ WINAPI CreateHatchBrush(int s, COLORREF c) { (void)s; return CreateSolidBrush(c); }
static HGDIOBJ WINAPI CreatePen(int style, int width, COLORREF c) { pen_t *p = calloc(1, sizeof *p); p->type = O_PEN; p->style = style; p->width = width; p->color = c; return p; }
static HGDIOBJ WINAPI CreateFontA(int h, int w, int e, int o, int wt, DWORD i, DWORD u, DWORD s, DWORD cs, DWORD op, DWORD cp, DWORD q, DWORD pf, const char *face)
{
    (void)w; (void)e; (void)o; (void)wt; (void)i; (void)u; (void)s; (void)cs; (void)op; (void)cp; (void)q; (void)pf; (void)face;
    font_t *f = calloc(1, sizeof *f); f->type = O_FONT;
    int a = h < 0 ? -h : h;
    f->ch = a ? (a < 8 ? 8 : a > 64 ? 64 : a) : 16; f->cw = (f->ch + 1) / 2; if (f->cw < 4) f->cw = 4;
    return f;
}
static HGDIOBJ WINAPI CreateFontIndirectA(const LONG *lf) { return CreateFontA(lf[0], 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL); }
static HGDIOBJ WINAPI CreateCompatibleBitmap(HDC d, int w, int h) { (void)d; return bmp_new(w, h, 32); }
static HGDIOBJ WINAPI CreateBitmap(int w, int h, UINT planes, UINT bpp, const void *bits) { (void)planes; (void)bpp; (void)bits; return bmp_new(w, h, 32); }
static HGDIOBJ WINAPI CreateDIBSection(HDC dc, const void *bmi, UINT usage, void **ppv, HANDLE sec, DWORD off)
{
    (void)dc; (void)usage; (void)sec; (void)off;
    const BITMAPINFOHEADER *hd = bmi;
    int bpp = hd->bpp, w = hd->w, h = hd->h < 0 ? -hd->h : hd->h;
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return NULL;
    bmp_t *b = bmp_new(w, h, bpp);
    if (!b) return NULL;
    b->bottomup = hd->h > 0;
    if (bpp == 8) {
        const uint8_t *pal = (const uint8_t *)bmi + hd->size;
        int n = hd->clrUsed ? (int)hd->clrUsed : 256;
        for (int i = 0; i < n && i < 256; i++) b->pal[i] = pal[i * 4] | (pal[i * 4 + 1] << 8) | (pal[i * 4 + 2] << 16);
    }
    if (ppv) *ppv = b->bits;
    return b;
}
static HGDIOBJ WINAPI SelectObject(HDC h, HGDIOBJ o)
{
    dc_t *d = D(h);
    if (!d || !o) return NULL;
    switch (*(int *)o) {
    case O_PEN: { void *old = d->pen; d->pen = o; return old; }
    case O_BRUSH: { void *old = d->brush; d->brush = o; return old; }
    case O_FONT: { void *old = d->font; d->font = o; return old; }
    case O_BMP: { void *old = d->bm; if (d->wnd) return NULL; d->bm = o; return old; }
    }
    return NULL;
}
static BOOL WINAPI DeleteObject(HGDIOBJ o)
{
    if (!o) return FALSE;
    int type = *(int *)o;
    if (type == O_PEN || type == O_BRUSH || type == O_FONT) { if (((pen_t *)o)->stock) return TRUE; free(o); return TRUE; }
    if (type == O_BMP) { bmp_t *b = o; if (b->stock) return TRUE; if (b->owns) free(b->bits); free(b); return TRUE; }
    return FALSE;
}
static int WINAPI GetObjectA(HGDIOBJ o, int n, void *out)
{
    if (!o || *(int *)o != O_BMP || n < (int)sizeof(BITMAP)) return 0;
    bmp_t *b = o; BITMAP *bm = out;
    memset(bm, 0, sizeof *bm); bm->w = b->w; bm->h = b->h; bm->wbytes = b->stride; bm->planes = 1; bm->bpp = (WORD)b->bpp; bm->bits = b->bits;
    return (int)sizeof(BITMAP);
}
static DWORD WINAPI GetObjectType(HGDIOBJ o) { if (!o) return 0; switch (*(int *)o) { case O_PEN: return 1; case O_BRUSH: return 2; case O_DC: return 3; case O_BMP: return 7; case O_FONT: return 6; } return 0; }
static int WINAPI GetDeviceCaps(HDC h, int i)
{
    (void)h;
    switch (i) { case 8: return SCREEN_W; case 10: return SCREEN_H; case 12: return 32; case 14: return 1; case 88: case 90: return 96; case 24: return -1; case 104: return 0; case 4: return 340; case 6: return 190; default: return 0; }
}
static COLORREF WINAPI SetBkColor(HDC h, COLORREF c) { dc_t *d = D(h); if (!d) return 0; COLORREF o = d->bk; d->bk = c; return o; }
static COLORREF WINAPI SetTextColor(HDC h, COLORREF c) { dc_t *d = D(h); if (!d) return 0; COLORREF o = d->text; d->text = c; return o; }
static int WINAPI SetBkMode(HDC h, int m) { dc_t *d = D(h); if (!d) return 0; int o = d->bkmode; d->bkmode = m; return o; }
static UINT WINAPI SetTextAlign(HDC h, UINT a) { dc_t *d = D(h); if (!d) return 0; UINT o = (UINT)d->talign; d->talign = (int)a; return o; }
static COLORREF WINAPI SetDCBrushColor(HDC h, COLORREF c) { (void)h; COLORREF o = st_dcbrush.color; st_dcbrush.color = c; return o; }
static COLORREF WINAPI SetDCPenColor(HDC h, COLORREF c) { (void)h; COLORREF o = st_dcpen.color; st_dcpen.color = c; return o; }
static int WINAPI SetStretchBltMode(HDC h, int m) { (void)h; (void)m; return 1; }
static int WINAPI SetROP2(HDC h, int m) { (void)h; (void)m; return 13; }
static int WINAPI SetMapMode(HDC h, int m) { (void)h; (void)m; return 1; }
static BOOL WINAPI GdiFlush(void) { flush(); return TRUE; }
static int WINAPI SaveDC(HDC h) { (void)h; return 1; }
static BOOL WINAPI RestoreDC(HDC h, int n) { (void)h; (void)n; return TRUE; }
static COLORREF WINAPI GetSysColor(int i) { return sys_color(i); }
static HGDIOBJ WINAPI GetSysColorBrush(int i) { return CreateSolidBrush(sys_color(i)); }

static COLORREF WINAPI SetPixel(HDC h, int x, int y, COLORREF c) { dc_t *d = D(h); if (!d) return (COLORREF)-1; putpx(d->bm, x + d->ox, y + d->oy, rgb(c)); mark_dirty(d); return c; }
static COLORREF WINAPI GetPixel(HDC h, int x, int y) { dc_t *d = D(h); if (!d) return (COLORREF)-1; return rgb(getpx(d->bm, x + d->ox, y + d->oy)); }
static BOOL WINAPI MoveToEx(HDC h, int x, int y, POINT *old) { dc_t *d = D(h); if (!d) return FALSE; if (old) { old->x = d->cx; old->y = d->cy; } d->cx = x; d->cy = y; return TRUE; }
static BOOL WINAPI LineTo(HDC h, int x, int y) { dc_t *d = D(h); if (!d) return FALSE; draw_line(d, d->cx + d->ox, d->cy + d->oy, x + d->ox, y + d->oy); d->cx = x; d->cy = y; mark_dirty(d); return TRUE; }
static BOOL WINAPI Polyline(HDC h, const POINT *p, int n) { dc_t *d = D(h); if (!d) return FALSE; for (int i = 0; i + 1 < n; i++) draw_line(d, p[i].x + d->ox, p[i].y + d->oy, p[i + 1].x + d->ox, p[i + 1].y + d->oy); mark_dirty(d); return TRUE; }
static BOOL WINAPI Polygon(HDC h, const POINT *p, int n)
{
    dc_t *d = D(h);
    if (!d || n < 2) return FALSE;
    POINT *q = malloc(sizeof(POINT) * (size_t)n);
    for (int i = 0; i < n; i++) { q[i].x = p[i].x + d->ox; q[i].y = p[i].y + d->oy; }
    poly_fill(d, q, n);
    for (int i = 0; i < n; i++) draw_line(d, q[i].x, q[i].y, q[(i + 1) % n].x, q[(i + 1) % n].y);
    free(q); mark_dirty(d); return TRUE;
}
static BOOL WINAPI Rectangle(HDC h, int l, int t, int r, int b)
{
    dc_t *d = D(h);
    if (!d) return FALSE;
    l += d->ox; r += d->ox; t += d->oy; b += d->oy;
    if (brush_visible(d->brush)) fill_rect(d->bm, l, t, r, b, rgb(d->brush == &st_dcbrush ? st_dcbrush.color : d->brush->color));
    if (d->pen->style != 5) { draw_line(d, l, t, r - 1, t); draw_line(d, r - 1, t, r - 1, b - 1); draw_line(d, r - 1, b - 1, l, b - 1); draw_line(d, l, b - 1, l, t); }
    mark_dirty(d); return TRUE;
}
static BOOL WINAPI Ellipse(HDC h, int l, int t, int r, int b) { dc_t *d = D(h); if (!d) return FALSE; ellipse_path(d, l + d->ox, t + d->oy, r + d->ox, b + d->oy, 1); mark_dirty(d); return TRUE; }
static int WINAPI FillRect(HDC h, const RECT *r, HGDIOBJ br) { dc_t *d = D(h); if (!d) return 0; fill_with(d, r->left, r->top, r->right, r->bottom, br); return 1; }
static int WINAPI FrameRect(HDC h, const RECT *r, HGDIOBJ br)
{
    dc_t *d = D(h);
    if (!d) return 0;
    fill_with(d, r->left, r->top, r->right, r->top + 1, br); fill_with(d, r->left, r->bottom - 1, r->right, r->bottom, br);
    fill_with(d, r->left, r->top, r->left + 1, r->bottom, br); fill_with(d, r->right - 1, r->top, r->right, r->bottom, br);
    return 1;
}
static BOOL WINAPI PatBlt(HDC h, int x, int y, int w, int hh, DWORD rop)
{
    dc_t *d = D(h);
    if (!d) return FALSE;
    uint32_t pat = rgb(d->brush->color);
    for (int yy = 0; yy < hh; yy++) for (int xx = 0; xx < w; xx++) putpx(d->bm, x + d->ox + xx, y + d->oy + yy, apply_rop(rop, 0, getpx(d->bm, x + d->ox + xx, y + d->oy + yy), pat));
    mark_dirty(d); return TRUE;
}
static BOOL WINAPI BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
    dc_t *d = D(dst), *s = D(src);
    if (!d) return FALSE;
    if (!s) { if (rop == 0x00000042 || rop == 0x00FF0062 || rop == 0x00F00021) return PatBlt(dst, x, y, w, h, rop); return FALSE; }
    stretch(d, x, y, w, h, s->bm, sx + s->ox, sy + s->oy, w, h, rop);
    return TRUE;
}
static BOOL WINAPI StretchBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, DWORD rop)
{
    dc_t *d = D(dst), *s = D(src);
    if (!d || !s) return FALSE;
    stretch(d, x, y, w, h, s->bm, sx + s->ox, sy + s->oy, sw, sh, rop);
    return TRUE;
}
static int WINAPI StretchDIBits(HDC dst, int xd, int yd, int wd, int hd, int xs, int ys, int ws, int hs, const void *bits, const void *bmi, UINT usage, DWORD rop)
{
    (void)usage;
    dc_t *d = D(dst);
    bmp_t v;
    if (!d || !bits || !bmi || !dib_view(bmi, bits, &v)) return 0;
    int ah = hs < 0 ? -hs : hs;
    int ytop = v.bottomup ? v.h - ys - ah : ys;            /* the source rectangle is given in bottom-up rows for a bottom-up DIB */
    stretch(d, xd, yd, wd, hd, &v, xs, ytop, ws, ah, rop);
    return hd < 0 ? -hd : hd;
}
static int WINAPI SetDIBitsToDevice(HDC dst, int xd, int yd, DWORD w, DWORD h, int xs, int ys, UINT start, UINT lines, const void *bits, const void *bmi, UINT usage)
{
    (void)start; (void)lines; (void)usage;
    dc_t *d = D(dst);
    bmp_t v;
    if (!d || !bits || !bmi || !dib_view(bmi, bits, &v)) return 0;
    int ytop = v.bottomup ? v.h - ys - (int)h : ys;
    stretch(d, xd, yd, (int)w, (int)h, &v, xs, ytop, (int)w, (int)h, 0x00CC0020);
    return (int)h;
}

static BOOL WINAPI TextOutA(HDC h, int x, int y, const char *s, int n) { dc_t *d = D(h); if (!d) return FALSE; text_run(d, x, y, s, n); return TRUE; }
static BOOL WINAPI ExtTextOutA(HDC h, int x, int y, UINT o, const RECT *r, const char *s, UINT n, const int *dx) { (void)o; (void)r; (void)dx; return TextOutA(h, x, y, s, (int)n); }
static BOOL WINAPI GetTextExtentPoint32A(HDC h, const char *s, int n, SIZE *sz) { (void)s; dc_t *d = D(h); if (!d) return FALSE; sz->cx = n * d->font->cw; sz->cy = d->font->ch; return TRUE; }
static BOOL WINAPI GetTextMetricsA(HDC h, LONG *tm)
{
    dc_t *d = D(h);
    if (!d) return FALSE;
    memset(tm, 0, 56);
    tm[0] = d->font->ch; tm[1] = d->font->ch * 3 / 4; tm[2] = d->font->ch - tm[1]; tm[5] = d->font->cw; tm[6] = d->font->cw; tm[7] = 400;
    ((uint8_t *)tm)[44] = 32; ((uint8_t *)tm)[45] = 255; ((uint8_t *)tm)[46] = '?'; ((uint8_t *)tm)[47] = ' ';
    return TRUE;
}
static int WINAPI DrawTextA(HDC h, const char *s, int n, RECT *rc, UINT fl)
{
    dc_t *d = D(h);
    if (!d) return 0;
    if (n < 0) n = (int)strlen(s);
    int cw = d->font->cw, chh = d->font->ch, width = rc->right - rc->left;
    int maxc = ((fl & 0x20) || width <= 0) ? 0x7FFFFFFF : width / cw;
    if (maxc < 1) maxc = 1;
    int *st = malloc(sizeof(int) * (size_t)(n + 2)), *ln = malloc(sizeof(int) * (size_t)(n + 2)), lines = 0, maxw = 0;
    for (int i = 0;;) {
        int j = i;
        while (j < n && s[j] != '\n' && j - i < maxc) j++;
        st[lines] = i; ln[lines] = j - i; if (ln[lines] * cw > maxw) maxw = ln[lines] * cw; lines++;
        if (j >= n) break;
        i = s[j] == '\n' ? j + 1 : j;
        if (i >= n) break;
    }
    int total = lines * chh, y = rc->top;
    if (fl & 0x400) {                                        /* DT_CALCRECT */
        rc->bottom = rc->top + total; rc->right = rc->left + (width > 0 && maxw > width ? width : maxw);
        free(st); free(ln); return total;
    }
    if (fl & 4) y = rc->top + (rc->bottom - rc->top - total) / 2; else if (fl & 8) y = rc->bottom - total;
    for (int i = 0; i < lines; i++) {
        int lw = ln[i] * cw, x = rc->left;
        if (fl & 1) x = rc->left + (width - lw) / 2; else if (fl & 2) x = rc->right - lw;
        int save = d->talign; d->talign = 0;
        text_run(d, x, y, s + st[i], ln[i]);
        d->talign = save;
        y += chh;
    }
    free(st); free(ln);
    return total;
}

/* ---- message boxes ------------------------------------------------------------------------------------------------ */

static int WINAPI MessageBoxA(HWND owner, const char *text, const char *caption, UINT type)
{
    (void)owner;
    int kind = (int)(type & 0xF);
    fprintf(stderr, "[MessageBox] %s: %s\n", caption ? caption : "", text ? text : "");
    int ok = 1, yes = 6, no = 7, cancel = 2;
    if (!g_front || !g_front->surf) return kind == 4 || kind == 3 ? yes : ok;
    bmp_t *s = g_front->surf;
    size_t bytes = (size_t)s->stride * (size_t)s->h;
    uint8_t *save = malloc(bytes);
    if (!save) return ok;
    memcpy(save, s->bits, bytes);
    dc_t *d = dc_for(g_front);
    d->font = &st_font;
    int lines = 1, maxc = 0, cur = 0;
    for (const char *p = text ? text : ""; *p; p++) { if (*p == '\n') { lines++; if (cur > maxc) maxc = cur; cur = 0; } else cur++; }
    if (cur > maxc) maxc = cur;
    int cap = caption ? (int)strlen(caption) : 0;
    if (cap > maxc) maxc = cap;
    int bw = maxc * 8 + 32, bh = lines * 16 + 16 * 4 + 8;
    if (bw > s->w - 4) bw = s->w - 4;
    int bx = (s->w - bw) / 2, by = (s->h - bh) / 2;
    const char *hint = kind == 4 ? "[Y] Yes   [N] No" : (kind == 1 ? "[Enter] OK   [Esc] Cancel" : "[Enter] OK");
    fill_rect(s, bx, by, bx + bw, by + bh, 0xF0F0F0);
    fill_rect(s, bx, by, bx + bw, by + 20, 0x2060C0);
    fill_rect(s, bx, by, bx + bw, by + 1, 0x000000); fill_rect(s, bx, by + bh - 1, bx + bw, by + bh, 0x000000);
    fill_rect(s, bx, by, bx + 1, by + bh, 0x000000); fill_rect(s, bx + bw - 1, by, bx + bw, by + bh, 0x000000);
    d->bkmode = 1; d->text = 0xFFFFFF; text_run(d, bx + 8, by + 2, caption ? caption : "", cap);
    d->text = 0x000000;
    { int ly = by + 28; const char *p = text ? text : ""; while (*p) { const char *e = strchr(p, '\n'); int n = e ? (int)(e - p) : (int)strlen(p); text_run(d, bx + 16, ly, p, n); ly += 16; if (!e) break; p = e + 1; } }
    text_run(d, bx + (bw - (int)strlen(hint) * 8) / 2, by + bh - 28, hint, (int)strlen(hint));
    free(d);
    int result = 0;
    present_now();
    while (!result) {
        int c;
        while ((c = API->key_poll()) >= 0) {
            if (c == 0xE0) { g_ext = 1; continue; }
            int make = c & 0x7F, down = !(c & 0x80), vk = vk_of_scan(make, g_ext);
            g_ext = 0;
            if (!down) continue;
            if (kind == 4) { if (vk == 'Y' || vk == 0x0D) result = yes; else if (vk == 'N' || vk == 0x1B) result = no; }
            else if (kind == 1 || kind == 3) { if (vk == 0x0D) result = ok; else if (vk == 0x1B) result = cancel; }
            else if (vk == 0x0D || vk == 0x1B || vk == 0x20) result = ok;
        }
        int dx, dy, b; static int lastb; if (API->mouse_poll(&dx, &dy, &b)) { if ((b & 1) && !(lastb & 1)) result = ok; lastb = b; }
        API->sleep_ms(5);
    }
    memcpy(s->bits, save, bytes);
    free(save);
    present_now();
    return result;
}
static int WINAPI MessageBoxW(HWND h, const WCHAR *t, const WCHAR *c, UINT ty) { char a[256], b[128]; narrow(t, a, sizeof a); narrow(c, b, sizeof b); return MessageBoxA(h, a, b, ty); }

/* ---- winmm -------------------------------------------------------------------------------------------------------- */

static DWORD WINAPI timeGetTime(void) { return now_ms(); }
static UINT WINAPI timeBeginPeriod(UINT p) { (void)p; return 0; }
static UINT WINAPI timeEndPeriod(UINT p) { (void)p; return 0; }
static BOOL WINAPI PlaySoundA(const char *s, void *m, DWORD f) { (void)s; (void)m; (void)f; return TRUE; }

/* ---- export table ------------------------------------------------------------------------------------------------- */

#define U(n) { "user32", #n, (void *)n }
#define G(n) { "gdi32", #n, (void *)n }
#define M(n) { "winmm", #n, (void *)n }
#define UA(n) { "user32", #n "A", (void *)n##A }

const win_export_t win_gui_exports[] = {
    UA(RegisterClass), UA(RegisterClassEx), UA(UnregisterClass), UA(CreateWindowEx), U(RegisterClassW), U(RegisterClassExW), U(CreateWindowExW),
    U(ShowWindow), U(DestroyWindow), UA(DefWindowProc), U(DefWindowProcW), UA(SendMessage), U(SendMessageW), UA(PostMessage), UA(PostThreadMessage),
    UA(CallWindowProc), U(PostQuitMessage), UA(GetMessage), UA(PeekMessage), U(TranslateMessage), UA(DispatchMessage),
    { "user32", "GetMessageW", (void *)GetMessageA }, { "user32", "PeekMessageW", (void *)PeekMessageA }, { "user32", "DispatchMessageW", (void *)DispatchMessageA },
    { "user32", "PostMessageW", (void *)PostMessageA }, { "user32", "CallWindowProcW", (void *)CallWindowProcA },
    U(SetTimer), U(KillTimer), U(GetClientRect), U(GetWindowRect), U(AdjustWindowRect), U(AdjustWindowRectEx), U(MoveWindow), U(SetWindowPos),
    U(IsWindow), U(IsWindowVisible), U(IsIconic), U(IsZoomed), U(GetParent), U(GetDesktopWindow), U(GetForegroundWindow), U(GetActiveWindow),
    U(GetFocus), U(SetFocus), U(SetActiveWindow), U(SetForegroundWindow), U(BringWindowToTop), U(SetCapture), U(ReleaseCapture), U(GetCapture),
    U(EnableWindow), UA(FindWindow), U(UpdateWindow), U(InvalidateRect), U(ValidateRect), U(GetUpdateRect), U(RedrawWindow),
    UA(SetWindowText), U(SetWindowTextW), UA(GetWindowText), UA(GetWindowTextLength), UA(GetClassName), UA(GetWindowLong), UA(SetWindowLong), UA(GetClassLong),
    { "user32", "GetWindowLongW", (void *)GetWindowLongA }, { "user32", "SetWindowLongW", (void *)SetWindowLongA },
    U(GetSystemMetrics), UA(SystemParametersInfo), U(SetProcessDPIAware), U(GetCursorPos), U(SetCursorPos), U(ScreenToClient), U(ClientToScreen),
    U(ShowCursor), U(SetCursor), UA(LoadCursor), UA(LoadIcon), UA(LoadImage), U(ClipCursor), U(DestroyIcon), U(GetDoubleClickTime),
    { "user32", "LoadCursorW", (void *)LoadCursorA }, { "user32", "LoadIconW", (void *)LoadIconA }, { "user32", "LoadImageW", (void *)LoadImageA },
    U(GetAsyncKeyState), U(GetKeyState), U(GetKeyboardState), UA(MapVirtualKey), U(GetKeyboardLayout), UA(RegisterWindowMessage),
    UA(IsDialogMessage), UA(TranslateAccelerator), UA(LoadAccelerators), U(CreateMenu), UA(AppendMenu), U(SetMenu), U(DrawMenuBar), U(DestroyMenu),
    UA(EnumDisplaySettings), UA(ChangeDisplaySettings), U(wsprintfA), U(wvsprintfA), UA(CharUpper), UA(CharLower),
    U(SetRect), U(SetRectEmpty), U(CopyRect), U(OffsetRect), U(InflateRect), U(PtInRect), U(IsRectEmpty), U(EqualRect), U(IntersectRect), U(UnionRect),
    U(GetDC), U(GetWindowDC), U(ReleaseDC), U(BeginPaint), U(EndPaint), U(FillRect), U(FrameRect), UA(MessageBox), U(MessageBoxW), U(GetSysColor), U(GetSysColorBrush),
    UA(DrawText),
    G(CreateCompatibleDC), G(DeleteDC), G(GetStockObject), G(CreateSolidBrush), G(CreateHatchBrush), G(CreatePen), UA(CreateFont), G(CreateFontIndirectA),
    G(CreateCompatibleBitmap), G(CreateBitmap), G(CreateDIBSection), G(SelectObject), G(DeleteObject), UA(GetObject), G(GetObjectType), G(GetDeviceCaps),
    G(SetBkColor), G(SetTextColor), G(SetBkMode), G(SetTextAlign), G(SetDCBrushColor), G(SetDCPenColor), G(SetStretchBltMode), G(SetROP2), G(SetMapMode),
    G(GdiFlush), G(SaveDC), G(RestoreDC), G(SetPixel), G(GetPixel), G(MoveToEx), G(LineTo), G(Polyline), G(Polygon), G(Rectangle), G(Ellipse), G(PatBlt),
    G(BitBlt), G(StretchBlt), G(StretchDIBits), G(SetDIBitsToDevice), G(TextOutA), G(ExtTextOutA), G(GetTextExtentPoint32A), G(GetTextMetricsA),
    M(timeGetTime), M(timeBeginPeriod), M(timeEndPeriod), M(PlaySoundA),
    { NULL, NULL, NULL }
};
