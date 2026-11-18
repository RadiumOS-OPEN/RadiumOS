// rbrowse painter: boxes -> 32-bit back buffer (0x00RRGGBB). Split out of
// rbrowse.c so the host tools (tools/rbrowse/render.c) paint with the very
// same code the kernel runs. The kernel builds this file at -O2 (makefile):
// it touches every pixel of up to 640x360 on each repaint.

#include "engine.h"
#include "paint.h"

#include <stddef.h>

extern uint8_t vga_font_8x8[256][8];

RbUi rb_ui;

/* Palette entries 0..15 of the mode-13h fallback (see paint.h). */
const uint32_t rb_ui_palette[16] = {
    UI_BLACK, 0x120C06u, UI_CHROME, UI_LINE, UI_URL, UI_URL_TEXT, UI_BTN, UI_BTN_TEXT,
    UI_STATUS, UI_STATUS_TEXT, UI_FOCUS, UI_SCROLL, UI_THUMB, 0x505050u, UI_WHITE, 0x000000u
};

void rb_ui_layout(int w, int h) {
    if (w > RB_FB_MAX_W) w = RB_FB_MAX_W;
    if (h > RB_FB_MAX_H) h = RB_FB_MAX_H;
    int wide = w >= 600;
    RbUi *u = &rb_ui;
    u->w = w;
    u->h = h;
    u->glyph = wide ? 2 : 1;
    /* 320x200 keeps the original mode-13h chrome pixel for pixel; 640 wide
       gets a slightly taller bar and status line around the same 8x8 text. */
    u->chrome_h = wide ? 18 : 16;
    u->btn_y = 2;
    u->btn_h = wide ? 14 : 12;
    u->text_y = u->btn_y + (u->btn_h - 8) / 2;
    u->status_h = wide ? 10 : 8;
    u->status_y = h - u->status_h;
    u->content_y = u->chrome_h;
    u->content_h = u->status_y - u->content_y;
    u->scroll_w = wide ? 10 : 8;
    u->scroll_x = w - u->scroll_w;
    u->view_x = 4;
    u->view_w = u->scroll_x - 2 * u->view_x;
    u->back_x = 2;
    u->back_w = wide ? 16 : 14;
    u->url_x = u->back_x + u->back_w + 2;
    u->go_w = wide ? 40 : 38;
    u->go_x = w - u->go_w - 10;
    u->url_w = u->go_x - 4 - u->url_x;
}

static uint32_t *g_fb;
static int g_w, g_h;
/* Clip rectangle every primitive also respects (rb_paint_clip): the whole
   framebuffer unless a partial repaint narrowed it. */
static int g_kx0, g_ky0, g_kx1, g_ky1;

void rb_paint_begin(uint32_t *fb, int w, int h) {
    g_fb = fb;
    g_w = w;
    g_h = h;
    g_kx0 = 0; g_ky0 = 0; g_kx1 = w; g_ky1 = h;
}

void rb_paint_clip(int x0, int y0, int x1, int y1) {
    g_kx0 = x0 > 0 ? x0 : 0;
    g_ky0 = y0 > 0 ? y0 : 0;
    g_kx1 = x1 < g_w ? x1 : g_w;
    g_ky1 = y1 < g_h ? y1 : g_h;
}

/* Opacity of the box being painted: 255 overwrites, anything less blends
   the colour over what is already there. */
static int g_alpha = 255;

static inline uint32_t blend(uint32_t dst, uint32_t src, int a) {
    uint32_t rb = ((src & 0xFF00FFu) * (uint32_t)a + (dst & 0xFF00FFu) * (uint32_t)(255 - a)) >> 8;
    uint32_t g = ((src & 0x00FF00u) * (uint32_t)a + (dst & 0x00FF00u) * (uint32_t)(255 - a)) >> 8;
    return (rb & 0xFF00FFu) | (g & 0x00FF00u);
}

static inline void put(int x, int y, uint32_t c) {
    uint32_t *p = &g_fb[y * g_w + x];
    *p = g_alpha >= 255 ? c : blend(*p, c, g_alpha);
}

void rb_px(int x, int y, uint32_t c) {
    if (x < g_kx0 || x >= g_kx1 || y < g_ky0 || y >= g_ky1) return;
    put(x, y, c);
}

void rb_rect(int x, int y, int w, int h, uint32_t c, int x0, int y0, int x1, int y1) {
    /* intersect first: a box can be tens of thousands of rows tall */
    int ya = y > y0 ? y : y0, yb = y + h < y1 ? y + h : y1;
    int xa = x > x0 ? x : x0, xb = x + w < x1 ? x + w : x1;
    if (ya < g_ky0) ya = g_ky0;
    if (yb > g_ky1) yb = g_ky1;
    if (xa < g_kx0) xa = g_kx0;
    if (xb > g_kx1) xb = g_kx1;
    if (xa >= xb) return;
    for (int yy = ya; yy < yb; yy++) {
        uint32_t *row = &g_fb[yy * g_w];
        if (g_alpha >= 255) {
            for (int xx = xa; xx < xb; xx++) row[xx] = c;
        } else {
            for (int xx = xa; xx < xb; xx++) row[xx] = blend(row[xx], c, g_alpha);
        }
    }
}

void rb_blit_rgb(int x, int y, const uint32_t *src, int sw, int sh, int src_stride,
                 int has_alpha, int x0, int y0, int x1, int y1) {
    if (!src || sw <= 0 || sh <= 0) return;
    int ya = y > y0 ? y : y0, yb = y + sh < y1 ? y + sh : y1;
    int xa = x > x0 ? x : x0, xb = x + sw < x1 ? x + sw : x1;
    if (ya < g_ky0) ya = g_ky0;
    if (yb > g_ky1) yb = g_ky1;
    if (xa < g_kx0) xa = g_kx0;
    if (xb > g_kx1) xb = g_kx1;
    for (int yy = ya; yy < yb; yy++) {
        const uint32_t *s = src + (yy - y) * src_stride + (xa - x);
        uint32_t *d = &g_fb[yy * g_w];
        for (int xx = xa; xx < xb; xx++, s++) {
            uint32_t p = *s;
            int a = has_alpha ? (int)(p >> 24) : 255;
            if (g_alpha < 255) a = a * g_alpha / 255;
            if (a >= 255) d[xx] = p & 0xFFFFFFu;
            else if (a > 0) d[xx] = blend(d[xx], p & 0xFFFFFFu, a);
        }
    }
}

/* Nearest-neighbour blit of a view into a w x h box (a box too big for the
   scaled-image cache). Clipped like rb_blit_rgb. */
static void blit_nearest(int x, int y, int w, int h, const RbImageView *v, int x0, int y0, int x1, int y1) {
    if (w <= 0 || h <= 0 || !v->px || v->w <= 0 || v->h <= 0) return;
    int ya = y > y0 ? y : y0, yb = y + h < y1 ? y + h : y1;
    int xa = x > x0 ? x : x0, xb = x + w < x1 ? x + w : x1;
    if (ya < g_ky0) ya = g_ky0;
    if (yb > g_ky1) yb = g_ky1;
    if (xa < g_kx0) xa = g_kx0;
    if (xb > g_kx1) xb = g_kx1;
    for (int yy = ya; yy < yb; yy++) {
        /* 32-bit products: box sides are below 16384 and views below 4096 */
        const uint32_t *s = v->px + (size_t)((yy - y) * v->h / h) * (size_t)v->stride;
        uint32_t *d = &g_fb[yy * g_w];
        for (int xx = xa; xx < xb; xx++) {
            uint32_t p = s[(xx - x) * v->w / w];
            int a = v->alpha ? (int)(p >> 24) : 255;
            if (g_alpha < 255) a = a * g_alpha / 255;
            if (a >= 255) d[xx] = p & 0xFFFFFFu;
            else if (a > 0) d[xx] = blend(d[xx], p & 0xFFFFFFu, a);
        }
    }
}

static RbImageHook g_image_hook;
static RbImageHook g_canvas_hook;

void rb_paint_set_images(RbImageHook fn) { g_image_hook = fn; }
void rb_paint_set_canvas(RbImageHook fn) { g_canvas_hook = fn; }

/* Rounded rectangles (border-radius). A corner pixel is painted when its
   centre is inside the quarter circle of radius r; everything is integer. */
static int corner_inset(int r, int ry) {
    int dy = 2 * r - 2 * ry - 1;
    for (int px = 0; px < r; px++) {
        int dx = 2 * r - 2 * px - 1;
        if (dx * dx + dy * dy <= 4 * r * r) return px;
    }
    return r;
}

static int row_inset(int h, int r, int row) {
    if (r <= 0) return 0;
    if (row < r) return corner_inset(r, row);
    if (row >= h - r) return corner_inset(r, h - 1 - row);
    return 0;
}

static void rrect(int x, int y, int w, int h, int r, uint32_t c, int x0, int y0, int x1, int y1) {
    if (r <= 0) { rb_rect(x, y, w, h, c, x0, y0, x1, y1); return; }
    int r0 = y0 - y > 0 ? y0 - y : 0, r1 = y1 - y < h ? y1 - y : h;
    for (int row = r0; row < r1; row++) {
        int o = row_inset(h, r, row);
        rb_rect(x + o, y + row, w - 2 * o, 1, c, x0, y0, x1, y1);
    }
}

/* A border ring of width bw inside a rounded rectangle. */
static void rring(int x, int y, int w, int h, int r, int bw, uint32_t c, int x0, int y0, int x1, int y1) {
    int ir = r > bw ? r - bw : 0;
    int ih = h - 2 * bw;
    int r0 = y0 - y > 0 ? y0 - y : 0, r1 = y1 - y < h ? y1 - y : h;
    for (int row = r0; row < r1; row++) {
        int o = row_inset(h, r, row);
        if (row < bw || row >= h - bw || ih <= 0) {
            rb_rect(x + o, y + row, w - 2 * o, 1, c, x0, y0, x1, y1);
            continue;
        }
        int i = bw + row_inset(ih, ir, row - bw);
        if (i < o) i = o;
        rb_rect(x + o, y + row, i - o, 1, c, x0, y0, x1, y1);
        rb_rect(x + w - i, y + row, i - o, 1, c, x0, y0, x1, y1);
    }
}

void rb_frame_rect(int x, int y, int w, int h, uint32_t c) {
    for (int i = 0; i < w; i++) {
        rb_px(x + i, y, c);
        rb_px(x + i, y + h - 1, c);
    }
    for (int i = 0; i < h; i++) {
        rb_px(x, y + i, c);
        rb_px(x + w - 1, y + i, c);
    }
}

static void glyph(int x, int y, int ch, uint32_t color, int scale, int x0, int y0, int x1, int y1) {
    if (ch < 0 || ch > 255) ch = '?';
    if (scale < 1) scale = 1;
    if (x0 < g_kx0) x0 = g_kx0;
    if (y0 < g_ky0) y0 = g_ky0;
    if (x1 > g_kx1) x1 = g_kx1;
    if (y1 > g_ky1) y1 = g_ky1;
    /* the whole cell is outside the clip: nothing to do (most of a long page) */
    if (x >= x1 || y >= y1 || x + 8 * scale <= x0 || y + 8 * scale <= y0) return;
    const uint8_t *g = vga_font_8x8[ch];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        if (!bits) continue;
        /* Font bytes are stored LSB = leftmost pixel. Text mode bit-reverses
           them into the VGA plane (which shifts MSB out first). */
        for (int col = 0; col < 8; col++) {
            if ((bits & (1u << col)) == 0) continue;
            for (int sy = 0; sy < scale; sy++) {
                int py = y + row * scale + sy;
                if (py < y0 || py >= y1) continue;
                for (int sx = 0; sx < scale; sx++) {
                    int px_ = x + col * scale + sx;
                    if (px_ < x0 || px_ >= x1) continue;
                    put(px_, py, color);
                }
            }
        }
    }
}

void rb_text_at(int x, int y, const char *s, uint32_t color, int x0, int y0, int x1, int y1) {
    int cx = x;
    while (s && *s) {
        glyph(cx, y, (unsigned char)*s, color, 1, x0, y0, x1, y1);
        cx += 8;
        s++;
    }
}

void rb_text_scaled(int x, int y, const char *s, uint32_t color, int scale) {
    int cx = x;
    while (s && *s) {
        glyph(cx, y, (unsigned char)*s, color, scale, 0, 0, g_w, g_h);
        cx += 8 * scale;
        s++;
    }
}

int rb_box_paint_rect(int i, int scroll, int *x, int *y, int *x0, int *y0, int *x1, int *y1) {
    const RbBox *b = rb_box(i);
    if (!b) return 0;
    const RbUi *u = &rb_ui;
    /* A fixed box lives in viewport space: no scroll offset. */
    int scr = (b->flags & RB_BOX_FIXED) ? 0 : scroll;
    *x = u->view_x + b->x;
    *y = u->content_y + b->y - scr;
    /* Paint bounds: the content area, narrowed by the box's clip rect. */
    int bx0 = 0, by0 = u->content_y, bx1 = u->scroll_x, by1 = u->status_y;
    int cx, cy, cw, ch;
    if (rb_box_clip(i, &cx, &cy, &cw, &ch)) {
        int kx0 = u->view_x + cx, ky0 = u->content_y + cy - scr;
        if (kx0 > bx0) bx0 = kx0;
        if (ky0 > by0) by0 = ky0;
        if (kx0 + cw < bx1) bx1 = kx0 + cw;
        if (ky0 + ch < by1) by1 = ky0 + ch;
    }
    *x0 = bx0; *y0 = by0; *x1 = bx1; *y1 = by1;
    return 1;
}

void rb_paint_page(int scroll, int focus_node_idx) {
    const RbUi *u = &rb_ui;
    g_alpha = 255;
    rb_rect(0, u->content_y, u->scroll_x, u->content_h, (uint32_t)rb_page_bg(), 0, 0, g_w, g_h);
    int boxes = rb_box_count();
    for (int i = 0; i < boxes; i++) {
        const RbBox *b = rb_box(i);
        if (!b) continue;
        int x, y, bx0, by0, bx1, by1;
        rb_box_paint_rect(i, scroll, &x, &y, &bx0, &by0, &bx1, &by1);
        if (bx0 >= bx1 || by0 >= by1) continue;
        /* off screen vertically: skip without walking the box (a glyph is
           at most 16 rows, whatever the box height says) */
        int bh = b->h > 16 ? b->h : 16;
        if (y >= by1 || y + bh <= by0) continue;
        g_alpha = b->opacity;
        if (g_alpha <= 0) continue;
        if (b->kind == RB_BOX_BG) {
            rrect(x, y, b->w, b->h, b->radius, b->bg, bx0, by0, bx1, by1);
        } else if (b->kind == RB_BOX_BORDER) {
            rring(x, y, b->w, b->h, b->radius, b->scale, b->bg, bx0, by0, bx1, by1);
        } else if (b->kind == RB_BOX_IMAGE) {
            RbImageView v;
            int have = g_image_hook && g_image_hook((int)b->toff, b->w, b->h, &v);
            if ((b->flags & RB_BOX_IMGBG) || (!have && b->w >= 12 && b->h >= 12))
                rb_rect(x, y, b->w, b->h, b->bg, bx0, by0, bx1, by1);
            if (have && v.w == b->w && v.h == b->h)
                rb_blit_rgb(x, y, v.px, v.w, v.h, v.stride, v.alpha, bx0, by0, bx1, by1);
            else if (have)
                blit_nearest(x, y, b->w, b->h, &v, bx0, by0, bx1, by1);
        } else if (b->kind == RB_BOX_CANVAS) {
            /* the element's background, then the pixels (transparent where
               nothing was drawn); a canvas without pixels shows nothing */
            RbImageView v;
            if (b->flags & RB_BOX_IMGBG) rb_rect(x, y, b->w, b->h, b->bg, bx0, by0, bx1, by1);
            if (g_canvas_hook && g_canvas_hook((int)b->toff, b->w, b->h, &v)) {
                if (v.w == b->w && v.h == b->h)
                    rb_blit_rgb(x, y, v.px, v.w, v.h, v.stride, v.alpha, bx0, by0, bx1, by1);
                else
                    blit_nearest(x, y, b->w, b->h, &v, bx0, by0, bx1, by1);
            }
        } else if (b->kind == RB_BOX_RULE) {
            rb_rect(x, y, b->w, b->h > 0 ? b->h : 1, b->color, bx0, by0, bx1, by1);
        } else if (b->kind == RB_BOX_CONTROL) {
            rrect(x, y, b->w, b->h, b->radius, b->bg, bx0, by0, bx1, by1);
            g_alpha = 255;
            rb_rect(x, y, b->w, 1, UI_LINE, bx0, by0, bx1, by1);
            rb_rect(x, y + b->h - 1, b->w, 1, UI_LINE, bx0, by0, bx1, by1);
            rb_rect(x, y, 1, b->h, UI_LINE, bx0, by0, bx1, by1);
            rb_rect(x + b->w - 1, y, 1, b->h, UI_LINE, bx0, by0, bx1, by1);
        } else if (b->kind == RB_BOX_TEXT) {
            int len = 0;
            const char *t = rb_box_text(i, &len);
            int scale = b->scale ? b->scale : 1;
            int pitch = 8 * scale + b->ls;       /* letter-spacing */
            for (int k = 0; k < len; k++) {
                int gx = x + k * pitch;
                if (gx >= bx1) break;
                glyph(gx, y, (unsigned char)t[k], b->color, scale, bx0, by0, bx1, by1);
                /* font-weight: bold is the glyph smeared one pixel right. */
                if (b->flags & RB_BOX_BOLD)
                    glyph(gx + 1, y, (unsigned char)t[k], b->color, scale, bx0, by0, bx1, by1);
            }
            if (b->flags & RB_BOX_UNDER)
                rb_rect(x, y + 8 * scale - 1, len * pitch - b->ls, 1, b->color, bx0, by0, bx1, by1);
            if (b->flags & RB_BOX_STRIKE)
                rb_rect(x, y + 4 * scale, len * pitch - b->ls, 1, b->color, bx0, by0, bx1, by1);
        }
        g_alpha = 255;
    }

    int fnode = focus_node_idx;
    if (fnode > 0) {
        int node = fnode;
        int minx = 100000, miny = 100000, maxx = -100000, maxy = -100000;
        for (int i = 0; i < boxes; i++) {
            const RbBox *b = rb_box(i);
            if (!b || b->node != node || b->kind == RB_BOX_BG || b->kind == RB_BOX_BORDER) continue;
            int x = u->view_x + b->x;
            int y = u->content_y + b->y - ((b->flags & RB_BOX_FIXED) ? 0 : scroll);
            if (x < minx) minx = x;
            if (y < miny) miny = y;
            if (x + b->w > maxx) maxx = x + b->w;
            if (y + b->h > maxy) maxy = y + b->h;
        }
        if (maxx > minx && maxy > miny && maxy > u->content_y && miny < u->status_y) {
            if (miny < u->content_y) miny = u->content_y;
            if (maxy > u->status_y) maxy = u->status_y;
            rb_frame_rect(minx - 1, miny - 1, (maxx - minx) + 2, (maxy - miny) + 2, UI_FOCUS);
        }
    }
}

/* ---- mode 13h fallback ------------------------------------------------------ */

static const uint8_t g_bayer4[16] = {
    0, 8, 2, 10,
    12, 4, 14, 6,
    3, 11, 1, 9,
    15, 7, 13, 5
};

/* Nearest cube level (0..5) of v, nudged by the ordered-dither threshold d
   (-24..+23, about half a cube step either way). */
static inline int cube_level(int v, int d) {
    v += d;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (v * 5 + 127) / 255;
}

void rb_quantize_8bpp(const uint32_t *src, uint8_t *dst, int w, int h) {
    uint32_t last = 0xFFFFFFFFu;
    int last_fixed = -1;          /* index for `last` when it needs no dither, else -1 */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t c = src[y * w + x] & 0xFFFFFFu;
            if (c != last) {
                last = c;
                last_fixed = -1;
                for (int i = 0; i < 15; i++) {
                    if (rb_ui_palette[i] == c) { last_fixed = i; break; }
                }
                if (last_fixed < 0) {
                    int r = RB_R(c), g = RB_G(c), b = RB_B(c);
                    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
                    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
                    /* grays and pale tints take the gray ramp: a dither
                       of near-white cube levels speckles text backgrounds */
                    if (mx - mn <= 24) last_fixed = rb_rgb_index(c);
                }
            }
            if (last_fixed >= 0) {
                dst[y * w + x] = (uint8_t)last_fixed;
                continue;
            }
            int d = g_bayer4[(y & 3) * 4 + (x & 3)] * 3 - 24;
            int ri = cube_level(RB_R(c), d), gi = cube_level(RB_G(c), d), bi = cube_level(RB_B(c), d);
            dst[y * w + x] = (uint8_t)(16 + ri * 36 + gi * 6 + bi);
        }
    }
}
