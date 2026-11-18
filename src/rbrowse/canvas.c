// rbrowse <canvas>: the canvas table, its static pixel arena and the 2D
// rasterizer (see canvas.h). The kernel builds this file at -O2 (makefile):
// fills and image draws touch every pixel they cover. Only 32-bit integer
// and double arithmetic: no 64-bit division (the kernel's __divdi3 is a
// slow bit loop) and no libm.

#include "canvas.h"
#include "engine.h"

#include <stddef.h>
#ifdef RBROWSE_HOST
#include <string.h>
#else
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *ptr, int value, size_t num);
#endif

/* ---- the table and the arena ---------------------------------------------- */

typedef struct {
    int node, gen;          /* the <canvas> element (gen guards against a recycled slot) */
    int w, h;               /* size as set; pixels exist only when `has` */
    uint32_t off, cap;      /* block in the arena, in pixels */
    uint8_t live, has;
    uint8_t dirty;          /* drawn on since rb_canvas_present */
    uint8_t shown_dirty;    /* the shown copy changed since rb_canvas_clean */
} Slot;

#define ARENA_PX (RB_CANVAS_ARENA / 4u)

static Slot g_slot[RB_CANVAS_MAX];
static uint32_t g_arena[ARENA_PX];
/* What the screen shows of each canvas: a copy made by rb_canvas_present
   when a frame's drawing is complete, at the same offset as the pixels, so
   a frame that is still being rasterized is never seen half done. */
static uint32_t g_shown[ARENA_PX];
static uint32_t g_top;      /* first free pixel; everything above is free */
static int g_dirty;

void rb_canvas_reset(void) {
    memset(g_slot, 0, sizeof(g_slot));
    g_top = 0;
    g_dirty = 1;
}

static Slot *slot_at(int i) {
    if (i < 0 || i >= RB_CANVAS_MAX || !g_slot[i].live) return 0;
    return &g_slot[i];
}

int rb_canvas_find(int node) {
    int gen = rb_dom_generation(node);
    if (!gen) return -1;
    for (int i = 0; i < RB_CANVAS_MAX; i++)
        if (g_slot[i].live && g_slot[i].node == node && g_slot[i].gen == gen) return i;
    return -1;
}

int rb_canvas_open(int node) {
    int found = rb_canvas_find(node);
    if (found >= 0) return found;
    int gen = rb_dom_generation(node);
    if (!gen) return -1;
    int pick = -1;
    for (int i = 0; i < RB_CANVAS_MAX && pick < 0; i++)
        if (!g_slot[i].live) pick = i;
    /* a slot whose element died: its block becomes a hole the next
       compaction closes */
    for (int i = 0; i < RB_CANVAS_MAX && pick < 0; i++)
        if (rb_dom_generation(g_slot[i].node) != g_slot[i].gen) pick = i;
    if (pick < 0) return -1;
    Slot *s = &g_slot[pick];
    memset(s, 0, sizeof(*s));
    s->live = 1;
    s->node = node;
    s->gen = gen;
    return pick;
}

/* Close the holes: move every block down, in arena order. Moving down is a
   forward copy, safe for overlapping ranges. */
static void compact(void) {
    uint32_t pos = 0;
    for (;;) {
        int best = -1;
        for (int i = 0; i < RB_CANVAS_MAX; i++) {
            Slot *s = &g_slot[i];
            if (!s->live || !s->cap || s->off < pos) continue;
            if (best < 0 || s->off < g_slot[best].off) best = i;
        }
        if (best < 0) break;
        Slot *s = &g_slot[best];
        uint32_t n = s->has ? (uint32_t)s->w * (uint32_t)s->h : 0;
        if (s->off != pos) {
            uint32_t *d = g_arena + pos;
            const uint32_t *src = g_arena + s->off;
            for (uint32_t k = 0; k < n; k++) d[k] = src[k];
            d = g_shown + pos;
            src = g_shown + s->off;
            for (uint32_t k = 0; k < n; k++) d[k] = src[k];
            s->off = pos;
        }
        s->cap = n;
        if (!n) { s->off = 0; continue; }
        pos += n;
    }
    /* blocks freed by dead slots were skipped above; empty slots keep off 0 */
    g_top = pos;
}

int rb_canvas_resize(int i, int w, int h) {
    Slot *s = slot_at(i);
    if (!s) return 0;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (w > 65535) w = 65535;
    if (h > 65535) h = 65535;
    s->w = w;
    s->h = h;
    s->has = 0;
    s->dirty = 0;
    s->shown_dirty = 1;     /* blank at the new size right away */
    g_dirty = 1;
    if (!w || !h || w > RB_CANVAS_DIM_MAX || h > RB_CANVAS_DIM_MAX) {
        s->cap = 0;
        return 0;
    }
    uint32_t need = (uint32_t)w * (uint32_t)h;
    if (need > s->cap) {
        s->cap = 0;
        if (g_top + need > ARENA_PX) compact();
        if (g_top + need > ARENA_PX) return 0;
        s->off = g_top;
        s->cap = need;
        g_top += need;
    }
    s->has = 1;
    memset(g_arena + s->off, 0, (size_t)need * 4);
    memset(g_shown + s->off, 0, (size_t)need * 4);
    return 1;
}

int rb_canvas_size(int i, int *w, int *h) {
    Slot *s = slot_at(i);
    if (w) *w = s ? s->w : 0;
    if (h) *h = s ? s->h : 0;
    return s && s->has;
}

uint32_t *rb_canvas_pixels(int i) {
    Slot *s = slot_at(i);
    return s && s->has ? g_arena + s->off : 0;
}

unsigned rb_canvas_used(void) { return g_top * 4u; }

/* n pixels: the kernel's memcpy goes a byte at a time, and a frame of a
   big canvas is megabytes */
static void copy_px(uint32_t *d, const uint32_t *s, size_t n) {
#if defined(__i386__) && !defined(RBROWSE_HOST)
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
#else
    memcpy(d, s, n * 4);
#endif
}

int rb_canvas_present(void) {
    int n = 0;
    for (int i = 0; i < RB_CANVAS_MAX; i++) {
        Slot *s = &g_slot[i];
        if (!s->live || !s->dirty) continue;
        s->dirty = 0;
        if (s->has) copy_px(g_shown + s->off, g_arena + s->off, (size_t)s->w * (size_t)s->h);
        s->shown_dirty = 1;
        n++;
    }
    return n;
}

int rb_canvas_node_dirty(int node) {
    int i = rb_canvas_find(node);
    return i >= 0 && g_slot[i].shown_dirty;
}

void rb_canvas_clean(void) {
    for (int i = 0; i < RB_CANVAS_MAX; i++) g_slot[i].shown_dirty = 0;
}

int rb_canvas_take_dirty(void) {
    int d = g_dirty;
    g_dirty = 0;
    return d;
}

int rb_canvas_view_node(int node, int w, int h, RbImageView *out) {
    (void)w;
    (void)h;
    int i = rb_canvas_find(node);
    if (!rb_canvas_pixels(i)) return 0;
    out->px = g_shown + g_slot[i].off;
    out->w = g_slot[i].w;
    out->h = g_slot[i].h;
    out->stride = g_slot[i].w;
    out->alpha = 1;
    return 1;
}

/* ---- compositing -------------------------------------------------------------
   d: the canvas pixel; rgb + pa: the paint (straight alpha, globalAlpha
   included); cv: coverage 0..256. */

static inline uint32_t comp(uint32_t d, uint32_t rgb, int pa, int cv, int op) {
    int sa = (pa * cv + 128) >> 8;
    int da = (int)(d >> 24);
    if (op == RB_CV_CLEAR || op == RB_CV_DEST_OUT) {
        if (op == RB_CV_CLEAR) sa = cv >= 256 ? 255 : (cv * 255) >> 8;
        int na = da * (255 - sa) / 255;
        return na ? ((uint32_t)na << 24) | (d & 0xFFFFFFu) : 0;
    }
    if (op == RB_CV_COPY && cv >= 256) return pa ? ((uint32_t)pa << 24) | rgb : 0;
    if (sa <= 0) return d;
    int sr = (int)(rgb >> 16) & 255, sg = (int)(rgb >> 8) & 255, sb = (int)rgb & 255;
    int dr = (int)(d >> 16) & 255, dg = (int)(d >> 8) & 255, db = (int)d & 255;
    if (op == RB_CV_LIGHTER) {
        int oa = sa + da;
        if (oa > 255) oa = 255;
        int r = (sr * sa + dr * da) / oa, g = (sg * sa + dg * da) / oa, b = (sb * sa + db * da) / oa;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        return ((uint32_t)oa << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    /* source-over */
    if (sa >= 255 || da == 0) return ((uint32_t)sa << 24) | rgb;
    if (da == 255) {
        int ia = 255 - sa;
        int r = (sr * sa + dr * ia + 127) / 255, g = (sg * sa + dg * ia + 127) / 255, b = (sb * sa + db * ia + 127) / 255;
        return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    int dw = da * (255 - sa) / 255;
    int oa = sa + dw;
    int r = (sr * sa + dr * dw) / oa, g = (sg * sa + dg * dw) / oa, b = (sb * sa + db * dw) / oa;
    return ((uint32_t)oa << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void clip_bounds(const Slot *s, const RbCvClip *clip, int *x0, int *y0, int *x1, int *y1) {
    *x0 = 0; *y0 = 0; *x1 = s->w; *y1 = s->h;
    if (clip) {
        if (clip->x0 > *x0) *x0 = clip->x0;
        if (clip->y0 > *y0) *y0 = clip->y0;
        if (clip->x1 < *x1) *x1 = clip->x1;
        if (clip->y1 < *y1) *y1 = clip->y1;
    }
}

/* ---- edges and the scanline fill ------------------------------------------------ */

typedef struct {
    double y0, y1;          /* y0 < y1 */
    double x0, dxdy;        /* x at y0, slope */
    int dir;                /* +1 downwards as given, -1 upwards */
} Edge;

static Edge g_edge[RB_CANVAS_EDGES_MAX];
static int g_nedge;
static int g_order[RB_CANVAS_EDGES_MAX];   /* edge indices by y0 */
static int g_tmp_i[RB_CANVAS_EDGES_MAX];
static int g_active[RB_CANVAS_EDGES_MAX];
/* crossings of one sub-scanline: x in 1/256 px and direction */
static int g_cx[RB_CANVAS_EDGES_MAX], g_cd[RB_CANVAS_EDGES_MAX];
static int g_tcx[RB_CANVAS_EDGES_MAX], g_tcd[RB_CANVAS_EDGES_MAX];
/* coverage of one row: partial pixels, and a difference array for runs of
   fully covered ones */
static int g_cov[RB_CANVAS_DIM_MAX + 2], g_dif[RB_CANVAS_DIM_MAX + 2];

static int (*g_abort)(void);
void rb_cv_set_abort(int (*fn)(void)) { g_abort = fn; }
/* every 8 rows: a fill of 65536 edges over 2048 rows is seconds of work */
#define ABORT_ROW(y) ((((y) & 7) == 0) && g_abort && g_abort())

void rb_cv_begin(void) { g_nedge = 0; }
int rb_cv_edges(void) { return g_nedge; }

int rb_cv_line(double x0, double y0, double x1, double y1) {
    /* NaN and huge values never reach the scanline loop: coordinates are
       kept within +-1e6 px (far outside any canvas) */
    if (!(x0 == x0) || !(y0 == y0) || !(x1 == x1) || !(y1 == y1)) return 1;
    if (x0 < -1e6) x0 = -1e6; else if (x0 > 1e6) x0 = 1e6;
    if (x1 < -1e6) x1 = -1e6; else if (x1 > 1e6) x1 = 1e6;
    if (y0 < -1e6) y0 = -1e6; else if (y0 > 1e6) y0 = 1e6;
    if (y1 < -1e6) y1 = -1e6; else if (y1 > 1e6) y1 = 1e6;
    if (y0 == y1) return 1;                  /* horizontal: never crosses a scanline */
    if (g_nedge >= RB_CANVAS_EDGES_MAX) return 0;
    Edge *e = &g_edge[g_nedge++];
    if (y0 < y1) {
        e->y0 = y0; e->y1 = y1; e->x0 = x0; e->dir = 1;
    } else {
        e->y0 = y1; e->y1 = y0; e->x0 = x1; e->dir = -1;
    }
    e->dxdy = (x1 - x0) / (y1 - y0);
    return 1;
}

/* g_order = edge indices sorted by y0 (stable merge sort). */
static void sort_order(int n) {
    for (int i = 0; i < n; i++) g_order[i] = i;
    for (int width = 1; width < n; width *= 2) {
        for (int lo = 0; lo < n; lo += 2 * width) {
            int mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
            int a = lo, b = mid, k = lo;
            while (a < mid && b < hi) g_tmp_i[k++] = g_edge[g_order[b]].y0 < g_edge[g_order[a]].y0 ? g_order[b++] : g_order[a++];
            while (a < mid) g_tmp_i[k++] = g_order[a++];
            while (b < hi) g_tmp_i[k++] = g_order[b++];
        }
        for (int i = 0; i < n; i++) g_order[i] = g_tmp_i[i];
    }
}

static void sort_cross(int n) {
    if (n <= 24) {
        for (int i = 1; i < n; i++) {
            int x = g_cx[i], d = g_cd[i], j = i - 1;
            while (j >= 0 && g_cx[j] > x) { g_cx[j + 1] = g_cx[j]; g_cd[j + 1] = g_cd[j]; j--; }
            g_cx[j + 1] = x;
            g_cd[j + 1] = d;
        }
        return;
    }
    for (int width = 1; width < n; width *= 2) {
        for (int lo = 0; lo < n; lo += 2 * width) {
            int mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
            int a = lo, b = mid, k = lo;
            while (a < mid && b < hi) {
                if (g_cx[b] < g_cx[a]) { g_tcx[k] = g_cx[b]; g_tcd[k++] = g_cd[b++]; }
                else { g_tcx[k] = g_cx[a]; g_tcd[k++] = g_cd[a++]; }
            }
            while (a < mid) { g_tcx[k] = g_cx[a]; g_tcd[k++] = g_cd[a++]; }
            while (b < hi) { g_tcx[k] = g_cx[b]; g_tcd[k++] = g_cd[b++]; }
        }
        for (int i = 0; i < n; i++) { g_cx[i] = g_tcx[i]; g_cd[i] = g_tcd[i]; }
    }
}

static inline uint32_t lut_at(const uint32_t *lut, double t) {
    if (!(t > 0)) return lut[0];         /* also NaN */
    if (t >= 1) return lut[255];
    return lut[(int)(t * 255.0 + 0.5)];
}

/* Radial gradient parameter at user point (px, py); 0 when no circle of
   the family passes through it (r(t) < 0 for every root). */
static int radial_t(const RbCvPaint *p, double px, double py, double *t) {
    double cdx = p->x1 - p->x0, cdy = p->y1 - p->y0, dr = p->r1 - p->r0;
    double pdx = px - p->x0, pdy = py - p->y0;
    double a = cdx * cdx + cdy * cdy - dr * dr;
    double b = pdx * cdx + pdy * cdy + p->r0 * dr;
    double c = pdx * pdx + pdy * pdy - p->r0 * p->r0;
    if (a > -1e-9 && a < 1e-9) {
        if (b > -1e-12 && b < 1e-12) return 0;
        double s = c / (2 * b);
        if (p->r0 + s * dr < 0) return 0;
        *t = s;
        return 1;
    }
    double disc = b * b - a * c;
    if (disc < 0) return 0;
    double sq;
#ifdef RBROWSE_HOST
    sq = __builtin_sqrt(disc);
#else
    __asm__("fsqrt" : "=t"(sq) : "0"(disc));
#endif
    double s1 = (b + sq) / a, s2 = (b - sq) / a;
    double hi = s1 > s2 ? s1 : s2, lo = s1 > s2 ? s2 : s1;
    if (p->r0 + hi * dr >= 0) { *t = hi; return 1; }
    if (p->r0 + lo * dr >= 0) { *t = lo; return 1; }
    return 0;
}

/* Composite one row's accumulated coverage, [xa, xb]. */
static void flush_row(uint32_t *row, int y, int xa, int xb, int cx0, int cx1, const RbCvPaint *p, int alpha, int op) {
    int run = 0;
    uint32_t rgb = p->argb & 0xFFFFFFu;
    int pa = (int)(p->argb >> 24) * alpha / 255;
    double cy = y + 0.5;
    double t = 0, dt = 0;
    if (p->kind == RB_CV_LINEAR) {
        t = p->gx * (xa + 0.5) + p->gy * cy + p->g0;
        dt = p->gx;
    }
    double ux = 0, uy = 0;
    if (p->kind == RB_CV_RADIAL) {
        ux = p->inv[0] * (xa + 0.5) + p->inv[2] * cy + p->inv[4];
        uy = p->inv[1] * (xa + 0.5) + p->inv[3] * cy + p->inv[5];
    }
    for (int x = xa; x <= xb; x++) {
        run += g_dif[x];
        int c = run + g_cov[x];
        g_dif[x] = 0;
        g_cov[x] = 0;
        if (c > 0 && x >= cx0 && x < cx1) {
            int cv = c >= 1024 ? 256 : c >> 2;
            if (p->kind == RB_CV_LINEAR) {
                uint32_t col = lut_at(p->lut, t);
                rgb = col & 0xFFFFFFu;
                pa = (int)(col >> 24) * alpha / 255;
            } else if (p->kind == RB_CV_RADIAL) {
                double tt;
                if (radial_t(p, ux, uy, &tt)) {
                    uint32_t col = lut_at(p->lut, tt);
                    rgb = col & 0xFFFFFFu;
                    pa = (int)(col >> 24) * alpha / 255;
                } else {
                    pa = 0;
                }
            }
            if (cv > 0) row[x] = comp(row[x], rgb, pa, cv, op);
        }
        t += dt;
        ux += p->inv[0];
        uy += p->inv[1];
    }
}

void rb_cv_fill(int i, int evenodd, const RbCvPaint *p, int alpha, int op, const RbCvClip *clip) {
    Slot *s = slot_at(i);
    if (!s || !s->has || g_nedge == 0 || !p) return;
    if ((p->kind == RB_CV_LINEAR || p->kind == RB_CV_RADIAL) && !p->lut) return;
    if (alpha <= 0 && op != RB_CV_CLEAR) return;
    if (alpha > 255) alpha = 255;
    int cx0, cy0, cx1, cy1;
    clip_bounds(s, clip, &cx0, &cy0, &cx1, &cy1);
    if (cx0 >= cx1 || cy0 >= cy1) return;
    /* vertical extent of the edges */
    double ymin = g_edge[0].y0, ymax = g_edge[0].y1;
    for (int k = 1; k < g_nedge; k++) {
        if (g_edge[k].y0 < ymin) ymin = g_edge[k].y0;
        if (g_edge[k].y1 > ymax) ymax = g_edge[k].y1;
    }
    int ry0 = ymin < cy0 ? cy0 : (int)ymin;
    int ry1 = ymax > cy1 ? cy1 : (int)ymax + 1;
    if (ry1 > cy1) ry1 = cy1;
    if (ry0 >= ry1) return;
    sort_order(g_nedge);
    g_dirty = 1;
    s->dirty = 1;
    uint32_t *px = g_arena + s->off;
    int next = 0, nact = 0;
    int lx = cx0 * 256, rx = cx1 * 256;
    for (int y = ry0; y < ry1; y++) {
        if (ABORT_ROW(y - ry0)) break;    /* each row's coverage was flushed: scratch is clean */
        /* edges starting above the row's bottom join; ended ones leave */
        while (next < g_nedge && g_edge[g_order[next]].y0 < y + 1) g_active[nact++] = g_order[next++];
        int w = 0;
        for (int k = 0; k < nact; k++)
            if (g_edge[g_active[k]].y1 > y) g_active[w++] = g_active[k];
        nact = w;
        if (!nact) {
            if (next >= g_nedge) break;
            continue;
        }
        int xa = 1 << 30, xb = -1;
        for (int sub = 0; sub < 4; sub++) {
            double sy = y + (sub * 2 + 1) * 0.125;
            int n = 0;
            for (int k = 0; k < nact; k++) {
                const Edge *e = &g_edge[g_active[k]];
                if (sy < e->y0 || sy >= e->y1) continue;
                double x = e->x0 + (sy - e->y0) * e->dxdy;
                int xi;
                if (x <= cx0) xi = lx;
                else if (x >= cx1) xi = rx;
                else xi = (int)(x * 256.0);
                g_cx[n] = xi;
                g_cd[n++] = e->dir;
            }
            if (n < 2) continue;
            sort_cross(n);
            int wind = 0;
            for (int k = 0; k + 1 < n; k++) {
                wind += g_cd[k];
                int in = evenodd ? (wind & 1) : wind != 0;
                if (!in) continue;
                int a = g_cx[k], b = g_cx[k + 1];
                if (b <= a) continue;
                int ia = a >> 8, ib = b >> 8;
                if (ia == ib) {
                    g_cov[ia] += b - a;
                } else {
                    g_cov[ia] += 256 - (a & 255);
                    g_dif[ia + 1] += 256;
                    g_dif[ib] -= 256;
                    g_cov[ib] += b & 255;
                }
                if (ia < xa) xa = ia;
                if (ib > xb) xb = ib;
            }
        }
        if (xb >= xa) flush_row(px + (size_t)y * (size_t)s->w, y, xa, xb, cx0, cx1, p, alpha, op);
    }
}

int rb_cv_inside(double x, double y, int evenodd) {
    int wind = 0;
    for (int k = 0; k < g_nedge; k++) {
        const Edge *e = &g_edge[k];
        if (y < e->y0 || y >= e->y1) continue;
        double ex = e->x0 + (y - e->y0) * e->dxdy;
        if (ex > x) wind += e->dir;
    }
    return evenodd ? (wind & 1) : wind != 0;
}

/* ---- drawImage --------------------------------------------------------------- */

#define SELF_COPY_MAX (1024 * 1024)
static uint32_t g_self[SELF_COPY_MAX];

/* Premultiplied bilinear sample at source coordinate (u, v), the four
   neighbours clamped to [iu0,iu1) x [iv0,iv1). */
static uint32_t bilinear(const uint32_t *src, int stride, double u, double v, int iu0, int iv0, int iu1, int iv1) {
    u -= 0.5;
    v -= 0.5;
    int x0 = u < 0 ? -1 : (int)u, y0 = v < 0 ? -1 : (int)v;
    int fx = (int)((u - x0) * 256.0), fy = (int)((v - y0) * 256.0);
    if (fx < 0) fx = 0;
    if (fx > 256) fx = 256;
    if (fy < 0) fy = 0;
    if (fy > 256) fy = 256;
    int xs[2] = {x0, x0 + 1}, ys[2] = {y0, y0 + 1};
    for (int k = 0; k < 2; k++) {
        if (xs[k] < iu0) xs[k] = iu0;
        if (xs[k] >= iu1) xs[k] = iu1 - 1;
        if (ys[k] < iv0) ys[k] = iv0;
        if (ys[k] >= iv1) ys[k] = iv1 - 1;
    }
    int wts[4] = {(256 - fx) * (256 - fy), fx * (256 - fy), (256 - fx) * fy, fx * fy};
    uint32_t acc_a = 0, acc_r = 0, acc_g = 0, acc_b = 0;
    for (int k = 0; k < 4; k++) {
        uint32_t p = src[ys[k >> 1] * stride + xs[k & 1]];
        uint32_t a = p >> 24, w = (uint32_t)wts[k] >> 8;     /* 0..256 */
        uint32_t aw = a * w;                                   /* <= 65280 */
        acc_a += aw;
        acc_r += ((p >> 16) & 255) * aw >> 8;
        acc_g += ((p >> 8) & 255) * aw >> 8;
        acc_b += (p & 255) * aw >> 8;
    }
    if (acc_a < 256) return 0;
    uint32_t a = acc_a >> 8;
    if (a > 255) a = 255;
    uint32_t r = acc_r * 256 / acc_a, g = acc_g * 256 / acc_a, b = acc_b * 256 / acc_a;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

void rb_cv_image(int i, const uint32_t *src, int sw, int sh, int stride,
                 double u0, double v0, double u1, double v1, const double inv[6],
                 int bx0, int by0, int bx1, int by1, int alpha, int op, int smooth, const RbCvClip *clip) {
    Slot *s = slot_at(i);
    if (!s || !s->has || !src || sw <= 0 || sh <= 0 || alpha <= 0) return;
    if (alpha > 255) alpha = 255;
    if (u0 < 0) u0 = 0;
    if (v0 < 0) v0 = 0;
    if (u1 > sw) u1 = sw;
    if (v1 > sh) v1 = sh;
    if (!(u0 < u1) || !(v0 < v1)) return;
    int iu0 = (int)u0, iv0 = (int)v0, iu1 = (int)u1, iv1 = (int)v1;
    if (iu1 < u1) iu1++;
    if (iv1 < v1) iv1++;
    if (iu1 > sw) iu1 = sw;
    if (iv1 > sh) iv1 = sh;
    if (iu0 >= iu1 || iv0 >= iv1) return;
    uint32_t *px = g_arena + s->off;
    double m[6];
    for (int k = 0; k < 6; k++) m[k] = inv[k];
    if (src == px) {
        /* drawing a canvas onto itself: read from a copy of the source
           rect, with source coordinates shifted to the copy's origin */
        int cw = iu1 - iu0, ch = iv1 - iv0;
        if ((uint32_t)cw * (uint32_t)ch > SELF_COPY_MAX) return;
        for (int y = 0; y < ch; y++)
            memcpy(g_self + y * cw, src + (iv0 + y) * stride + iu0, (size_t)cw * 4);
        src = g_self;
        stride = cw;
        m[4] -= iu0;
        m[5] -= iv0;
        u0 -= iu0; u1 -= iu0; v0 -= iv0; v1 -= iv0;
        iu1 -= iu0; iv1 -= iv0; iu0 = 0; iv0 = 0;
    }
    int cx0, cy0, cx1, cy1;
    clip_bounds(s, clip, &cx0, &cy0, &cx1, &cy1);
    if (bx0 > cx0) cx0 = bx0;
    if (by0 > cy0) cy0 = by0;
    if (bx1 < cx1) cx1 = bx1;
    if (by1 < cy1) cy1 = by1;
    if (cx0 >= cx1 || cy0 >= cy1) return;
    g_dirty = 1;
    s->dirty = 1;
    for (int y = cy0; y < cy1; y++) {
        if (ABORT_ROW(y - cy0)) break;
        double cyc = y + 0.5;
        double u = m[0] * (cx0 + 0.5) + m[2] * cyc + m[4];
        double v = m[1] * (cx0 + 0.5) + m[3] * cyc + m[5];
        uint32_t *row = px + (size_t)y * (size_t)s->w;
        for (int x = cx0; x < cx1; x++, u += m[0], v += m[1]) {
            if (!(u >= u0 && u < u1 && v >= v0 && v < v1)) continue;
            uint32_t p;
            if (smooth) {
                p = bilinear(src, stride, u, v, iu0, iv0, iu1, iv1);
            } else {
                int su = (int)u, sv = (int)v;
                if (su >= iu1) su = iu1 - 1;
                if (sv >= iv1) sv = iv1 - 1;
                p = src[sv * stride + su];
            }
            int pa = (int)(p >> 24) * alpha / 255;
            row[x] = comp(row[x], p & 0xFFFFFFu, pa, 256, op);
        }
    }
}

/* ---- pixel access ----------------------------------------------------------------- */

void rb_cv_get(int i, int x, int y, int w, int h, uint8_t *rgba) {
    if (w <= 0 || h <= 0 || !rgba) return;
    Slot *s = slot_at(i);
    const uint32_t *px = s && s->has ? g_arena + s->off : 0;
    for (int r = 0; r < h; r++) {
        uint8_t *o = rgba + (size_t)r * (size_t)w * 4;
        int yy = y + r;
        for (int c = 0; c < w; c++, o += 4) {
            int xx = x + c;
            if (!px || yy < 0 || yy >= s->h || xx < 0 || xx >= s->w) {
                o[0] = o[1] = o[2] = o[3] = 0;
                continue;
            }
            uint32_t p = px[yy * s->w + xx];
            o[0] = (uint8_t)(p >> 16);
            o[1] = (uint8_t)(p >> 8);
            o[2] = (uint8_t)p;
            o[3] = (uint8_t)(p >> 24);
        }
    }
}

void rb_cv_put(int i, const uint8_t *rgba, int iw, int ih, int dx, int dy, int x0, int y0, int x1, int y1) {
    Slot *s = slot_at(i);
    if (!s || !s->has || !rgba || iw <= 0 || ih <= 0) return;
    /* An image placed entirely off the canvas draws nothing. Past these two
       tests dx, dy and the rectangle are all small (|dx| < max(iw, w),
       0 <= x0 < x1 <= iw), so the sums below cannot wrap: with wrapping,
       putImageData(img, 2147483647, 0) or a huge dirty-rect origin got past
       the clamps and read and wrote far outside the buffers. */
    if (dx <= -iw || dx >= s->w || dy <= -ih || dy >= s->h) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > iw) x1 = iw;
    if (y1 > ih) y1 = ih;
    if (x0 >= x1 || y0 >= y1) return;
    /* and inside the canvas */
    if (dx + x0 < 0) x0 = -dx;
    if (dy + y0 < 0) y0 = -dy;
    if (dx + x1 > s->w) x1 = s->w - dx;
    if (dy + y1 > s->h) y1 = s->h - dy;
    if (x0 >= x1 || y0 >= y1) return;
    g_dirty = 1;
    s->dirty = 1;
    uint32_t *px = g_arena + s->off;
    for (int y = y0; y < y1; y++) {
        const uint8_t *in = rgba + ((size_t)y * (size_t)iw + (size_t)x0) * 4;
        uint32_t *o = px + (size_t)(dy + y) * (size_t)s->w + (dx + x0);
        for (int x = x0; x < x1; x++, in += 4) {
            uint32_t a = in[3];
            *o++ = a ? (a << 24) | ((uint32_t)in[0] << 16) | ((uint32_t)in[1] << 8) | in[2] : 0;
        }
    }
}

/* ---- PNG ------------------------------------------------------------------------------- */

static uint32_t g_crc_table[256];

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, int n) {
    if (!g_crc_table[1]) {
        for (uint32_t k = 0; k < 256; k++) {
            uint32_t c = k;
            for (int j = 0; j < 8; j++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            g_crc_table[k] = c;
        }
    }
    for (int k = 0; k < n; k++) crc = g_crc_table[(crc ^ p[k]) & 255] ^ (crc >> 8);
    return crc;
}

static void put32(uint8_t *o, uint32_t v) {
    o[0] = (uint8_t)(v >> 24);
    o[1] = (uint8_t)(v >> 16);
    o[2] = (uint8_t)(v >> 8);
    o[3] = (uint8_t)v;
}

/* Write a chunk's length, type and CRC around data already at o + 8. */
static int chunk(uint8_t *o, const char *type, int len) {
    put32(o, (uint32_t)len);
    for (int k = 0; k < 4; k++) o[4 + k] = (uint8_t)type[k];
    put32(o + 8 + len, crc32_update(0xFFFFFFFFu, o + 4, len + 4) ^ 0xFFFFFFFFu);
    return len + 12;
}

int rb_cv_png(int i, uint8_t *out, int cap) {
    Slot *s = slot_at(i);
    if (!s || !s->has || !out) return -1;
    uint32_t raw = (uint32_t)s->h * (1u + 4u * (uint32_t)s->w);
    uint32_t blocks = (raw + 65534u) / 65535u;
    uint32_t idat = 2 + raw + 5 * blocks + 4;
    uint32_t total = 8 + 25 + 12 + idat + 12;
    if (total > (uint32_t)cap || total > 0x7FFFFFFFu) return -1;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    memcpy(out, sig, 8);
    int w = 8;
    uint8_t *ih = out + w + 8;
    put32(ih, (uint32_t)s->w);
    put32(ih + 4, (uint32_t)s->h);
    ih[8] = 8; ih[9] = 6; ih[10] = 0; ih[11] = 0; ih[12] = 0;   /* 8-bit RGBA */
    w += chunk(out + w, "IHDR", 13);
    uint8_t *d = out + w + 8;
    int k = 0;
    d[k++] = 0x78;
    d[k++] = 0x01;
    const uint32_t *px = g_arena + s->off;
    uint32_t left = raw, in_block = 0, a1 = 1, a2 = 0;
    for (int y = 0; y < s->h; y++) {
        for (int x = -1; x < s->w; x++) {
            uint8_t b[4];
            int nb;
            if (x < 0) { b[0] = 0; nb = 1; }                  /* filter: none */
            else {
                uint32_t p = px[y * s->w + x];
                b[0] = (uint8_t)(p >> 16); b[1] = (uint8_t)(p >> 8); b[2] = (uint8_t)p; b[3] = (uint8_t)(p >> 24);
                nb = 4;
            }
            for (int j = 0; j < nb; j++) {
                if (!in_block) {
                    uint32_t len = left > 65535u ? 65535u : left;
                    d[k++] = left == len ? 1 : 0;              /* BFINAL on the last block, BTYPE stored */
                    d[k++] = (uint8_t)len;
                    d[k++] = (uint8_t)(len >> 8);
                    d[k++] = (uint8_t)~len;
                    d[k++] = (uint8_t)(~len >> 8);
                    in_block = len;
                }
                d[k++] = b[j];
                in_block--;
                left--;
                a1 += b[j];
                if (a1 >= 65521u) a1 -= 65521u;
                a2 += a1;
                if (a2 >= 65521u) a2 -= 65521u;
            }
        }
    }
    put32(d + k, (a2 << 16) | a1);
    k += 4;
    w += chunk(out + w, "IDAT", k);
    w += chunk(out + w, "IEND", 0);
    return w;
}
