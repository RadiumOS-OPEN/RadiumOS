/* rjs_canvas.c - native half of the <canvas> 2D context (see canvas.h for the
 * rasterizer it drives, dom_prelude.js for the JS half).
 *
 * Split of work. The prelude's CanvasRenderingContext2D keeps the drawing
 * state (transform, styles, line settings, the save/restore stack, the clip
 * rectangle) and records paths as flat Float64Array command lists. Arcs and
 * ellipses are already cubic Beziers there. The natives here flatten those
 * lists (through an affine matrix, so Path2D objects in user space and the
 * context's device-space current path share one code path), stroke them
 * into polygons, and hand edges to canvas.c.
 *
 * Path commands: MOVE x y | LINE x y | QUAD cx cy x y | CUBIC ax ay bx by x y
 * | CLOSE. After CLOSE the current point is the subpath's start; a following
 * LINE starts a new subpath there (as the spec's closePath does).
 *
 * Simplifications, all documented in the prelude: the clip region is a
 * device rectangle (the bounding box of the clip path), strokes use the
 * transform's average scale (no elliptical pens under non-uniform scale),
 * text is the 8x8 system font scaled to the font size, patterns and shadows
 * do not paint, and only source-over, copy, destination-out and lighter
 * composite (others draw as source-over).
 */
#include "quickjs.h"
#include "rjs_internal.h"
#include "../rbrowse/engine.h"
#include "../rbrowse/canvas.h"
#include "../rbrowse/image.h"
#include <math.h>
#include <stdint.h>

extern uint8_t vga_font_8x8[256][8];

enum { P_MOVE = 0, P_LINE, P_QUAD, P_CUBIC, P_CLOSE };
enum { CAP_BUTT = 0, CAP_ROUND, CAP_SQUARE };
enum { JOIN_MITER = 0, JOIN_ROUND, JOIN_BEVEL };

/* ---- flattened paths: subpaths of device points ---- */

#define PT_MAX 65536
#define SUB_MAX 16384
static double g_x[PT_MAX], g_y[PT_MAX];
static int g_npt;
typedef struct { int first, n, closed; } Sub;
static Sub g_sub[SUB_MAX];
static int g_nsub;

/* the current state the next fill/stroke/text/image uses */
static RbCvPaint g_paint;
static uint32_t g_lut[256];
static int g_paint_ok = 1;          /* 0: this paint draws nothing (degenerate gradient, pattern) */
static int g_alpha = 255, g_op = RB_CV_OVER, g_smooth = 1;
static RbCvClip g_clip;
static int g_has_clip;

static void sub_start(void) {
    if (g_nsub >= SUB_MAX) return;
    g_sub[g_nsub].first = g_npt;
    g_sub[g_nsub].n = 0;
    g_sub[g_nsub].closed = 0;
    g_nsub++;
}

static void pt_add(double x, double y) {
    if (g_nsub == 0 || g_npt >= PT_MAX) return;
    Sub *s = &g_sub[g_nsub - 1];
    if (s->first + s->n != g_npt) return;   /* a subpath that was cut off by SUB_MAX */
    if (s->n > 0 && g_x[g_npt - 1] == x && g_y[g_npt - 1] == y) {
        /* repeated point: keep it only as a marker of a zero-length segment */
        if (s->n > 1) return;
    }
    g_x[g_npt] = x;
    g_y[g_npt] = y;
    g_npt++;
    s->n++;
}

static int seg_count(double d) {
    /* d: flatness measure (control polygon deviation) in device px */
    if (!(d > 0)) return 1;
    double n = sqrt(d * 3.0);   /* tolerance about 1/4 px */
    if (n < 1) return 1;
    if (n > 128) return 128;
    return (int)n + 1;
}

static double absd(double v) { return v < 0 ? -v : v; }

/* Flatten n doubles of commands through m (a b c d e f) into g_sub/g_x/g_y. */
static void flatten(const double *d, int n, const double m[6]) {
    g_npt = 0;
    g_nsub = 0;
    double cx = 0, cy = 0, sx = 0, sy = 0;   /* current point, subpath start (device) */
    int have = 0, closed = 0;
#define TX(x, y) (m[0] * (x) + m[2] * (y) + m[4])
#define TY(x, y) (m[1] * (x) + m[3] * (y) + m[5])
    int i = 0;
    while (i < n) {
        int op = (int)d[i];
        if (op == P_CLOSE) {
            i++;
            if (have && g_nsub) {
                g_sub[g_nsub - 1].closed = 1;
                cx = sx;
                cy = sy;
                closed = 1;
            }
            continue;
        }
        int need = op == P_MOVE || op == P_LINE ? 2 : op == P_QUAD ? 4 : op == P_CUBIC ? 6 : -1;
        if (need < 0 || i + 1 + need > n) break;
        const double *a = d + i + 1;
        i += 1 + need;
        double ex = TX(a[need - 2], a[need - 1]), ey = TY(a[need - 2], a[need - 1]);
        if (op == P_MOVE) {
            sub_start();
            pt_add(ex, ey);
            cx = sx = ex;
            cy = sy = ey;
            have = 1;
            closed = 0;
            continue;
        }
        if (!have || closed) {
            /* no current point: the first point starts a subpath (spec "ensure
               there is a subpath"); after a close, restart at its start */
            sub_start();
            if (!have) { cx = sx = TX(a[0], a[1]); cy = sy = TY(a[0], a[1]); }
            else { sx = cx; sy = cy; }
            pt_add(cx, cy);
            have = 1;
            closed = 0;
        }
        if (op == P_LINE) {
            pt_add(ex, ey);
        } else if (op == P_QUAD) {
            double qx = TX(a[0], a[1]), qy = TY(a[0], a[1]);
            double dd = absd(cx - 2 * qx + ex) + absd(cy - 2 * qy + ey);
            int k = seg_count(dd);
            for (int j = 1; j <= k; j++) {
                double t = (double)j / k, u = 1 - t;
                pt_add(u * u * cx + 2 * u * t * qx + t * t * ex, u * u * cy + 2 * u * t * qy + t * t * ey);
            }
        } else {
            double ax = TX(a[0], a[1]), ay = TY(a[0], a[1]);
            double bx = TX(a[2], a[3]), by = TY(a[2], a[3]);
            double d1 = absd(cx - 2 * ax + bx) + absd(cy - 2 * ay + by);
            double d2 = absd(ax - 2 * bx + ex) + absd(ay - 2 * by + ey);
            int k = seg_count(d1 > d2 ? d1 : d2);
            for (int j = 1; j <= k; j++) {
                double t = (double)j / k, u = 1 - t;
                double w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
                pt_add(w0 * cx + w1 * ax + w2 * bx + w3 * ex, w0 * cy + w1 * ay + w2 * by + w3 * ey);
            }
        }
        cx = ex;
        cy = ey;
    }
#undef TX
#undef TY
}

/* ---- edge output with automatic flushing ---- */

static int g_target = -1;       /* slot to flush into; -1: collecting for a hit test */
static int g_evenodd;

static void flush_fill(void) {
    if (g_target >= 0 && g_paint_ok) rb_cv_fill(g_target, g_evenodd, &g_paint, g_alpha, g_op, g_has_clip ? &g_clip : 0);
    rb_cv_begin();
}

static void line(double x0, double y0, double x1, double y1) {
    if (rb_cv_line(x0, y0, x1, y1)) return;
    if (g_target < 0) return;    /* hit test: the table is full, ignore the rest */
    flush_fill();
    rb_cv_line(x0, y0, x1, y1);
}

/* A closed polygon, oriented so all stroke pieces wind the same way (their
   nonzero union is then the stroke). */
static void poly(const double *x, const double *y, int n) {
    double area = 0;
    for (int k = 0; k < n; k++) {
        int j = (k + 1) % n;
        area += x[k] * y[j] - x[j] * y[k];
    }
    if (area < 0) {
        for (int k = n - 1; k >= 0; k--) {
            int j = k == 0 ? n - 1 : k - 1;
            line(x[k], y[k], x[j], y[j]);
        }
    } else {
        for (int k = 0; k < n; k++) {
            int j = (k + 1) % n;
            line(x[k], y[k], x[j], y[j]);
        }
    }
}

static void circle(double cx, double cy, double r) {
    if (!(r > 0)) return;
    int n = r >= 40 ? 64 : (int)(r * 1.5) + 8;
    if (n > 64) n = 64;
    double x[64], y[64];
    for (int k = 0; k < n; k++) {
        double a = 6.283185307179586 * k / n;
        x[k] = cx + r * cos(a);
        y[k] = cy + r * sin(a);
    }
    poly(x, y, n);
}

/* ---- fill ---- */

static void fill_subpaths(void) {
    for (int s = 0; s < g_nsub; s++) {
        const Sub *sp = &g_sub[s];
        if (sp->n < 2) continue;
        int f = sp->first;
        for (int k = 0; k + 1 < sp->n; k++) line(g_x[f + k], g_y[f + k], g_x[f + k + 1], g_y[f + k + 1]);
        line(g_x[f + sp->n - 1], g_y[f + sp->n - 1], g_x[f], g_y[f]);   /* fills close implicitly */
    }
}

/* ---- stroke ---- */

static double g_hw;             /* half the line width, device px */
static int g_cap, g_join;
static double g_miter;

static void seg_quad(double x0, double y0, double x1, double y1) {
    double dx = x1 - x0, dy = y1 - y0, len = sqrt(dx * dx + dy * dy);
    if (!(len > 0)) return;
    double nx = -dy / len * g_hw, ny = dx / len * g_hw;
    double px[4] = {x0 + nx, x1 + nx, x1 - nx, x0 - nx};
    double py[4] = {y0 + ny, y1 + ny, y1 - ny, y0 - ny};
    poly(px, py, 4);
}

static void cap_at(double x, double y, double dx, double dy) {
    /* (dx, dy): unit direction pointing out of the line at this end */
    if (g_cap == CAP_ROUND) {
        circle(x, y, g_hw);
    } else if (g_cap == CAP_SQUARE) {
        double nx = -dy * g_hw, ny = dx * g_hw, ox = dx * g_hw, oy = dy * g_hw;
        double px[4] = {x + nx, x + nx + ox, x - nx + ox, x - nx};
        double py[4] = {y + ny, y + ny + oy, y - ny + oy, y - ny};
        poly(px, py, 4);
    }
}

static void join_at(double x, double y, double d0x, double d0y, double d1x, double d1y) {
    double cross = d0x * d1y - d0y * d1x;
    double dot = d0x * d1x + d0y * d1y;
    if (cross > -1e-9 && cross < 1e-9 && dot > 0) return;   /* straight on */
    if (g_join == JOIN_ROUND) {
        circle(x, y, g_hw);
        return;
    }
    double s = cross > 0 ? -1 : 1;    /* the outer side */
    double n0x = -d0y * s, n0y = d0x * s, n1x = -d1y * s, n1y = d1x * s;
    double ax = x + n0x * g_hw, ay = y + n0y * g_hw, bx = x + n1x * g_hw, by = y + n1y * g_hw;
    if (g_join == JOIN_MITER) {
        double mx = n0x + n1x, my = n0y + n1y, ml = sqrt(mx * mx + my * my);
        if (ml > 1e-9) {
            mx /= ml;
            my /= ml;
            double c = mx * n0x + my * n0y;      /* cos of half the turn */
            if (c > 1e-9 && 1.0 / c <= g_miter) {
                double tx = x + mx * g_hw / c, ty = y + my * g_hw / c;
                double px[4] = {x, ax, tx, bx}, py[4] = {y, ay, ty, by};
                poly(px, py, 4);
                return;
            }
        }
    }
    double px[3] = {x, ax, bx}, py[3] = {y, ay, by};
    poly(px, py, 3);
}

/* Stroke one polyline (device points), open or closed. */
static void stroke_poly(const double *x, const double *y, int n, int closed) {
    /* drop zero-length segments */
    int m = 0;
    static double sx[PT_MAX], sy[PT_MAX];
    for (int k = 0; k < n; k++) {
        if (m > 0 && x[k] == sx[m - 1] && y[k] == sy[m - 1]) continue;
        sx[m] = x[k];
        sy[m] = y[k];
        m++;
    }
    if (closed && m > 2 && sx[m - 1] == sx[0] && sy[m - 1] == sy[0]) m--;
    if (m == 1) {
        /* a zero-length subpath: round and square caps still draw a dot */
        if (n >= 2 && !closed) {
            if (g_cap == CAP_ROUND) circle(sx[0], sy[0], g_hw);
            else if (g_cap == CAP_SQUARE) {
                double px[4] = {sx[0] - g_hw, sx[0] + g_hw, sx[0] + g_hw, sx[0] - g_hw};
                double py[4] = {sy[0] - g_hw, sy[0] - g_hw, sy[0] + g_hw, sy[0] + g_hw};
                poly(px, py, 4);
            }
        }
        return;
    }
    if (m < 2) return;
    int segs = closed ? m : m - 1;
    for (int k = 0; k < segs; k++) {
        int j = (k + 1) % m;
        seg_quad(sx[k], sy[k], sx[j], sy[j]);
    }
    /* joins at interior vertices (and all of them when closed) */
    for (int k = closed ? 0 : 1; k < (closed ? m : m - 1); k++) {
        int p = (k - 1 + m) % m, q = (k + 1) % m;
        double ax = sx[k] - sx[p], ay = sy[k] - sy[p], bx = sx[q] - sx[k], by = sy[q] - sy[k];
        double la = sqrt(ax * ax + ay * ay), lb = sqrt(bx * bx + by * by);
        if (!(la > 0) || !(lb > 0)) continue;
        join_at(sx[k], sy[k], ax / la, ay / la, bx / lb, by / lb);
    }
    if (!closed && g_cap != CAP_BUTT) {
        double dx = sx[0] - sx[1], dy = sy[0] - sy[1], l = sqrt(dx * dx + dy * dy);
        cap_at(sx[0], sy[0], dx / l, dy / l);
        dx = sx[m - 1] - sx[m - 2];
        dy = sy[m - 1] - sy[m - 2];
        l = sqrt(dx * dx + dy * dy);
        cap_at(sx[m - 1], sy[m - 1], dx / l, dy / l);
    }
}

#define DASH_MAX 32
static double g_dash[DASH_MAX];
static int g_ndash;
static double g_dash_off;

/* Dash one subpath into open pieces and stroke each. */
static void stroke_dashed(int first, int n, int closed) {
    static double px[PT_MAX], py[PT_MAX];
    double total = 0;
    for (int k = 0; k < g_ndash; k++) total += g_dash[k];
    if (!(total > 0)) {
        stroke_poly(g_x + first, g_y + first, n, closed);
        return;
    }
    /* phase at the start */
    double off = fmod(g_dash_off, total);
    if (off < 0) off += total;
    int di = 0;
    while (off >= g_dash[di]) {
        off -= g_dash[di];
        di = (di + 1) % g_ndash;
    }
    double left = g_dash[di] - off;     /* length left in the current dash entry */
    int on = (di % 2) == 0;
    int np = 0;
    int segs = closed ? n : n - 1;
    if (on) { px[0] = g_x[first]; py[0] = g_y[first]; np = 1; }
    int budget = 200000;                /* dash pieces per subpath */
    for (int k = 0; k < segs && budget > 0; k++) {
        double x0 = g_x[first + k], y0 = g_y[first + k];
        int j = first + (k + 1) % n;
        double x1 = g_x[j], y1 = g_y[j];
        double dx = x1 - x0, dy = y1 - y0, len = sqrt(dx * dx + dy * dy), pos = 0;
        while (len - pos > left && budget-- > 0) {
            pos += left;
            double t = pos / len, qx = x0 + dx * t, qy = y0 + dy * t;
            if (on) {
                if (np < PT_MAX) { px[np] = qx; py[np] = qy; np++; }
                stroke_poly(px, py, np, 0);
                np = 0;
            } else {
                px[0] = qx;
                py[0] = qy;
                np = 1;
            }
            on = !on;
            di = (di + 1) % g_ndash;
            left = g_dash[di];
        }
        left -= len - pos;
        if (on && np < PT_MAX) {
            px[np] = x1;
            py[np] = y1;
            np++;
        }
    }
    if (on && np >= 2) stroke_poly(px, py, np, 0);
}

static void stroke_subpaths(void) {
    for (int s = 0; s < g_nsub; s++) {
        const Sub *sp = &g_sub[s];
        if (sp->n < 1) continue;
        if (g_ndash) stroke_dashed(sp->first, sp->n, sp->closed);
        else stroke_poly(g_x + sp->first, g_y + sp->first, sp->n, sp->closed);
    }
}

/* ---- argument helpers ---- */

static double num(JSContext *ctx, JSValueConst v) {
    double d = 0;
    if (JS_ToFloat64(ctx, &d, v)) return 0;
    return d;
}

static int inum(JSContext *ctx, JSValueConst v) {
    int32_t i = 0;
    JS_ToInt32(ctx, &i, v);
    return i;
}

/* A Float64Array's data. 0 when it is not one. */
static const double *f64(JSContext *ctx, JSValueConst v, int *count) {
    size_t off, len, per;
    *count = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &per);
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    size_t size;
    uint8_t *p = JS_GetArrayBuffer(ctx, &size, buf);
    JS_FreeValue(ctx, buf);
    if (!p || per != 8 || off + len > size) return 0;
    *count = (int)(len / 8);
    return (const double *)(p + off);
}

static uint8_t *u8(JSContext *ctx, JSValueConst v, size_t *count) {
    size_t off, len, per;
    *count = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &per);
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    size_t size;
    uint8_t *p = JS_GetArrayBuffer(ctx, &size, buf);
    JS_FreeValue(ctx, buf);
    if (!p || per != 1 || off + len > size) return 0;
    *count = len;
    return p + off;
}

static void mat(JSContext *ctx, JSValueConst *argv, double m[6]) {
    for (int k = 0; k < 6; k++) m[k] = num(ctx, argv[k]);
}

static int finite_mat(const double m[6]) {
    for (int k = 0; k < 6; k++)
        if (!(m[k] == m[k]) || m[k] > 1e12 || m[k] < -1e12) return 0;
    return 1;
}

static int invert(const double m[6], double o[6]) {
    double det = m[0] * m[3] - m[1] * m[2];
    if (!(det > 1e-12 || det < -1e-12)) return 0;
    double id = 1.0 / det;
    o[0] = m[3] * id;
    o[1] = -m[1] * id;
    o[2] = -m[2] * id;
    o[3] = m[0] * id;
    o[4] = (m[2] * m[5] - m[3] * m[4]) * id;
    o[5] = (m[1] * m[4] - m[0] * m[5]) * id;
    return 1;
}

static double mat_scale(const double m[6]) {
    double det = m[0] * m[3] - m[1] * m[2];
    return sqrt(det < 0 ? -det : det);
}

#define FN(name) static JSValue name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
#define UNUSED (void)this_val; (void)argc

/* ---- canvas element ---- */

/* open(node) -> slot or -1 */
FN(c_open) { UNUSED; return JS_NewInt32(ctx, rb_canvas_open(inum(ctx, argv[0]))); }
/* resize(slot, w, h) -> 1 when it has pixels */
FN(c_resize) { UNUSED; return JS_NewInt32(ctx, rb_canvas_resize(inum(ctx, argv[0]), inum(ctx, argv[1]), inum(ctx, argv[2]))); }
/* attrSize(node) -> [w, h] from the width/height attributes */
FN(c_attr_size) {
    UNUSED;
    int w, h;
    rb_canvas_attr_size(inum(ctx, argv[0]), &w, &h);
    JSValue a = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, a, 0, JS_NewInt32(ctx, w));
    JS_SetPropertyUint32(ctx, a, 1, JS_NewInt32(ctx, h));
    return a;
}

/* color(str) -> 0xAARRGGBB as a number, or null when it does not parse */
FN(c_color) {
    UNUSED;
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s) return JS_EXCEPTION;
    uint32_t argb;
    int ok = rb_parse_color(s, (int)n, &argb);
    JS_FreeCString(ctx, s);
    return ok ? JS_NewFloat64(ctx, (double)argb) : JS_NULL;
}

/* ---- paint and compositing state ---- */

/* paint(argb): a solid colour */
FN(c_paint) {
    UNUSED;
    double v = num(ctx, argv[0]);
    g_paint.kind = RB_CV_SOLID;
    g_paint.argb = v >= 0 && v <= 4294967295.0 ? (uint32_t)v : 0;
    g_paint_ok = 1;
    return JS_UNDEFINED;
}

/* paintNone(): the style paints nothing (pattern, unsupported gradient) */
FN(c_paint_none) { UNUSED; (void)ctx; (void)argv; g_paint_ok = 0; return JS_UNDEFINED; }

static uint32_t lerp_premul(uint32_t a, uint32_t b, double t) {
    double aa = (a >> 24) / 255.0, ba = (b >> 24) / 255.0;
    double oa = aa + (ba - aa) * t;
    if (oa <= 0) return 0;
    uint32_t out = (uint32_t)(oa * 255 + 0.5) << 24;
    for (int sh = 16; sh >= 0; sh -= 8) {
        double ac = ((a >> sh) & 255) * aa, bc = ((b >> sh) & 255) * ba;
        double c = (ac + (bc - ac) * t) / oa;
        if (c < 0) c = 0;
        if (c > 255) c = 255;
        out |= (uint32_t)(c + 0.5) << sh;
    }
    return out;
}

/* gradient(kind 1 linear | 2 radial, x0, y0, r0, x1, y1, r1, stops Float64Array
   [offset, argb, ...] sorted by offset, a b c d e f = the transform it is
   painted under) */
FN(c_gradient) {
    UNUSED;
    int kind = inum(ctx, argv[0]);
    double x0 = num(ctx, argv[1]), y0 = num(ctx, argv[2]), r0 = num(ctx, argv[3]);
    double x1 = num(ctx, argv[4]), y1 = num(ctx, argv[5]), r1 = num(ctx, argv[6]);
    int ns;
    const double *st = f64(ctx, argv[7], &ns);
    double m[6], inv[6];
    mat(ctx, argv + 8, m);
    g_paint_ok = 0;
    if (!st || ns < 2 || !invert(m, inv)) return JS_UNDEFINED;
    ns /= 2;
    /* the lookup table: stops interpolated in premultiplied space */
    for (int k = 0; k < 256; k++) {
        double t = k / 255.0;
        int j = 0;
        while (j < ns && st[2 * j] <= t) j++;
        uint32_t c;
        if (j == 0) c = (uint32_t)st[1];
        else if (j == ns) c = (uint32_t)st[2 * (ns - 1) + 1];
        else {
            double o0 = st[2 * (j - 1)], o1 = st[2 * j];
            double f = o1 > o0 ? (t - o0) / (o1 - o0) : 1;
            c = lerp_premul((uint32_t)st[2 * (j - 1) + 1], (uint32_t)st[2 * j + 1], f);
        }
        g_lut[k] = c;
    }
    g_paint.lut = g_lut;
    if (kind == 1) {
        double dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
        if (!(l2 > 0)) return JS_UNDEFINED;
        g_paint.kind = RB_CV_LINEAR;
        g_paint.gx = (dx * inv[0] + dy * inv[1]) / l2;
        g_paint.gy = (dx * inv[2] + dy * inv[3]) / l2;
        g_paint.g0 = (dx * (inv[4] - x0) + dy * (inv[5] - y0)) / l2;
    } else {
        if (x0 == x1 && y0 == y1 && r0 == r1) return JS_UNDEFINED;
        g_paint.kind = RB_CV_RADIAL;
        for (int k = 0; k < 6; k++) g_paint.inv[k] = inv[k];
        g_paint.x0 = x0; g_paint.y0 = y0; g_paint.r0 = r0;
        g_paint.x1 = x1; g_paint.y1 = y1; g_paint.r1 = r1;
    }
    g_paint_ok = 1;
    return JS_UNDEFINED;
}

/* comp(alpha 0..255, op, smooth, x0, y0, x1, y1): x0 < 0 means no clip */
FN(c_comp) {
    UNUSED;
    g_alpha = inum(ctx, argv[0]);
    if (g_alpha < 0) g_alpha = 0;
    if (g_alpha > 255) g_alpha = 255;
    g_op = inum(ctx, argv[1]);
    if (g_op < RB_CV_OVER || g_op > RB_CV_CLEAR) g_op = RB_CV_OVER;
    g_smooth = inum(ctx, argv[2]) != 0;
    g_clip.x0 = inum(ctx, argv[3]);
    g_clip.y0 = inum(ctx, argv[4]);
    g_clip.x1 = inum(ctx, argv[5]);
    g_clip.y1 = inum(ctx, argv[6]);
    g_has_clip = g_clip.x0 >= 0;
    return JS_UNDEFINED;
}

/* ---- path operations ---- */

/* fill(slot, path, n, a..f, evenodd) */
FN(c_fill) {
    UNUSED;
    int slot = inum(ctx, argv[0]), cnt;
    const double *d = f64(ctx, argv[1], &cnt);
    int n = inum(ctx, argv[2]);
    double m[6];
    mat(ctx, argv + 3, m);
    if (!d || n > cnt || !finite_mat(m) || !rb_canvas_pixels(slot)) return JS_UNDEFINED;
    flatten(d, n, m);
    g_target = slot;
    g_evenodd = inum(ctx, argv[9]) != 0;
    rb_cv_begin();
    fill_subpaths();
    flush_fill();
    return JS_UNDEFINED;
}

static void line_style(JSContext *ctx, JSValueConst *argv, const double m[6]) {
    double sc = mat_scale(m);
    double w = num(ctx, argv[0]);
    g_hw = w * sc * 0.5;
    g_cap = inum(ctx, argv[1]);
    g_join = inum(ctx, argv[2]);
    g_miter = num(ctx, argv[3]);
    g_ndash = 0;
    int nd;
    const double *dash = f64(ctx, argv[4], &nd);
    if (dash) {
        double total = 0;
        for (int k = 0; k < nd && g_ndash < DASH_MAX; k++) {
            g_dash[g_ndash++] = dash[k] * sc;
            total += dash[k];
        }
        if (!(total > 0)) g_ndash = 0;
        /* pieces shorter than 1/16 px would only make a huge number of edges */
        if (total * sc < 0.0625 * g_ndash) g_ndash = 0;
    }
    g_dash_off = num(ctx, argv[5]) * sc;
}

/* stroke(slot, path, n, a..f, width, cap, join, miter, dash Float64Array|null, dashOffset) */
FN(c_stroke) {
    UNUSED;
    int slot = inum(ctx, argv[0]), cnt;
    const double *d = f64(ctx, argv[1], &cnt);
    int n = inum(ctx, argv[2]);
    double m[6];
    mat(ctx, argv + 3, m);
    if (!d || n > cnt || !finite_mat(m) || !rb_canvas_pixels(slot)) return JS_UNDEFINED;
    line_style(ctx, argv + 9, m);
    if (!(g_hw > 0) || g_hw > 1e5) return JS_UNDEFINED;
    flatten(d, n, m);
    g_target = slot;
    g_evenodd = 0;
    rb_cv_begin();
    stroke_subpaths();
    flush_fill();
    return JS_UNDEFINED;
}

/* inside(path, n, a..f, x, y, evenodd) -> isPointInPath */
FN(c_inside) {
    UNUSED;
    int cnt;
    const double *d = f64(ctx, argv[0], &cnt);
    int n = inum(ctx, argv[1]);
    double m[6];
    mat(ctx, argv + 2, m);
    if (!d || n > cnt || !finite_mat(m)) return JS_FALSE;
    flatten(d, n, m);
    g_target = -1;
    rb_cv_begin();
    fill_subpaths();
    int r = rb_cv_inside(num(ctx, argv[8]), num(ctx, argv[9]), inum(ctx, argv[10]) != 0);
    rb_cv_begin();
    return JS_NewBool(ctx, r);
}

/* inStroke(path, n, a..f, width, cap, join, miter, dash, dashOffset, x, y) */
FN(c_in_stroke) {
    UNUSED;
    int cnt;
    const double *d = f64(ctx, argv[0], &cnt);
    int n = inum(ctx, argv[1]);
    double m[6];
    mat(ctx, argv + 2, m);
    if (!d || n > cnt || !finite_mat(m)) return JS_FALSE;
    line_style(ctx, argv + 8, m);
    if (!(g_hw > 0) || g_hw > 1e5) return JS_FALSE;
    flatten(d, n, m);
    g_target = -1;
    rb_cv_begin();
    stroke_subpaths();
    int r = rb_cv_inside(num(ctx, argv[14]), num(ctx, argv[15]), 0);
    rb_cv_begin();
    return JS_NewBool(ctx, r);
}

/* bounds(path, n, a..f) -> [x0, y0, x1, y1] in device px, or null if empty */
FN(c_bounds) {
    UNUSED;
    int cnt;
    const double *d = f64(ctx, argv[0], &cnt);
    int n = inum(ctx, argv[1]);
    double m[6];
    mat(ctx, argv + 2, m);
    if (!d || n > cnt || !finite_mat(m)) return JS_NULL;
    flatten(d, n, m);
    if (g_npt == 0) return JS_NULL;
    double x0 = g_x[0], y0 = g_y[0], x1 = x0, y1 = y0;
    for (int k = 1; k < g_npt; k++) {
        if (g_x[k] < x0) x0 = g_x[k];
        if (g_x[k] > x1) x1 = g_x[k];
        if (g_y[k] < y0) y0 = g_y[k];
        if (g_y[k] > y1) y1 = g_y[k];
    }
    JSValue a = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, a, 0, JS_NewFloat64(ctx, x0));
    JS_SetPropertyUint32(ctx, a, 1, JS_NewFloat64(ctx, y0));
    JS_SetPropertyUint32(ctx, a, 2, JS_NewFloat64(ctx, x1));
    JS_SetPropertyUint32(ctx, a, 3, JS_NewFloat64(ctx, y1));
    return a;
}

/* ---- text: the 8x8 font, one cell = size/2 wide and size tall ---- */

/* text(slot, str, x, y, size, a..f): glyph cells start at user (x, y), the
   top-left of the first cell; the prelude did alignment and baseline. */
FN(c_text) {
    UNUSED;
    int slot = inum(ctx, argv[0]);
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!s) return JS_EXCEPTION;
    double x = num(ctx, argv[2]), y = num(ctx, argv[3]), size = num(ctx, argv[4]);
    double m[6];
    mat(ctx, argv + 5, m);
    if (!rb_canvas_pixels(slot) || !finite_mat(m) || !(size > 0) || size > 10000) {
        JS_FreeCString(ctx, s);
        return JS_UNDEFINED;
    }
    double pw = size / 16.0, ph = size / 8.0;   /* one font pixel */
    g_target = slot;
    g_evenodd = 0;
    rb_cv_begin();
    double cx = x;
    for (size_t i = 0; i < len;) {
        unsigned c = (uint8_t)s[i++];
        if (c >= 0x80) {
            /* one glyph per code point: skip continuation bytes */
            while (i < len && ((uint8_t)s[i] & 0xC0) == 0x80) i++;
            c = '?';
        }
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        const uint8_t *g = vga_font_8x8[c];
        for (int row = 0; row < 8; row++) {
            uint8_t bits = g[row];
            int col = 0;
            while (col < 8) {
                if (!(bits & (1u << col))) { col++; continue; }
                int start = col;
                while (col < 8 && (bits & (1u << col))) col++;
                /* run of set pixels [start, col) on this row: one rectangle */
                double ux0 = cx + start * pw, ux1 = cx + col * pw, uy0 = y + row * ph, uy1 = uy0 + ph;
                double qx[4], qy[4];
                double ux[4] = {ux0, ux1, ux1, ux0}, uy[4] = {uy0, uy0, uy1, uy1};
                for (int k = 0; k < 4; k++) {
                    qx[k] = m[0] * ux[k] + m[2] * uy[k] + m[4];
                    qy[k] = m[1] * ux[k] + m[3] * uy[k] + m[5];
                }
                poly(qx, qy, 4);
            }
        }
        cx += size / 2.0;
    }
    JS_FreeCString(ctx, s);
    flush_fill();
    return JS_UNDEFINED;
}

/* ---- images ---- */

/* image(slot, kind 0 = <img> node | 1 = canvas slot, src, sx, sy, sw, sh,
   dx, dy, dw, dh, a..f) -> 1 drawn (or nothing to draw), 0 source not usable */
FN(c_image) {
    UNUSED;
    int slot = inum(ctx, argv[0]), kind = inum(ctx, argv[1]), src = inum(ctx, argv[2]);
    double sx = num(ctx, argv[3]), sy = num(ctx, argv[4]), sw = num(ctx, argv[5]), sh = num(ctx, argv[6]);
    double dx = num(ctx, argv[7]), dy = num(ctx, argv[8]), dw = num(ctx, argv[9]), dh = num(ctx, argv[10]);
    double m[6];
    mat(ctx, argv + 11, m);
    const uint32_t *px = 0;
    int w = 0, h = 0, nw = 0, nh = 0;
    if (kind == 0) {
        if (!rb_image_node_pixels(src, &px, &w, &h, &nw, &nh)) return JS_NewInt32(ctx, 0);
    } else {
        px = rb_canvas_pixels(src);
        rb_canvas_size(src, &w, &h);
        nw = w;
        nh = h;
        if (!px) return JS_NewInt32(ctx, 1);   /* an empty canvas draws nothing */
    }
    if (!rb_canvas_pixels(slot) || !finite_mat(m) || nw <= 0 || nh <= 0) return JS_NewInt32(ctx, 1);
    /* normalise negative sizes (the spec flips them into place) */
    if (sw < 0) { sx += sw; sw = -sw; }
    if (sh < 0) { sy += sh; sh = -sh; }
    if (dw < 0) { dx += dw; dw = -dw; }
    if (dh < 0) { dy += dh; dh = -dh; }
    if (!(sw > 0) || !(sh > 0) || !(dw > 0) || !(dh > 0)) return JS_NewInt32(ctx, 1);
    double inv[6];
    if (!invert(m, inv)) return JS_NewInt32(ctx, 1);
    double fx = (double)w / nw, fy = (double)h / nh, kx = sw / dw, ky = sh / dh;
    double t[6];
    t[0] = fx * kx * inv[0];
    t[2] = fx * kx * inv[2];
    t[4] = fx * (sx + (inv[4] - dx) * kx);
    t[1] = fy * ky * inv[1];
    t[3] = fy * ky * inv[3];
    t[5] = fy * (sy + (inv[5] - dy) * ky);
    /* device bounding box of the destination rectangle */
    double cxs[4] = {dx, dx + dw, dx + dw, dx}, cys[4] = {dy, dy, dy + dh, dy + dh};
    double bx0 = 1e9, by0 = 1e9, bx1 = -1e9, by1 = -1e9;
    for (int k = 0; k < 4; k++) {
        double X = m[0] * cxs[k] + m[2] * cys[k] + m[4], Y = m[1] * cxs[k] + m[3] * cys[k] + m[5];
        if (X < bx0) bx0 = X;
        if (X > bx1) bx1 = X;
        if (Y < by0) by0 = Y;
        if (Y > by1) by1 = Y;
    }
    if (bx0 < -1e6) bx0 = -1e6;
    if (by0 < -1e6) by0 = -1e6;
    if (bx1 > 1e6) bx1 = 1e6;
    if (by1 > 1e6) by1 = 1e6;
    int ix0 = (int)floor(bx0), iy0 = (int)floor(by0), ix1 = (int)ceil(bx1), iy1 = (int)ceil(by1);
    rb_cv_image(slot, px, w, h, w, fx * sx, fy * sy, fx * (sx + sw), fy * (sy + sh), t,
                ix0, iy0, ix1, iy1, g_alpha, g_op, g_smooth, g_has_clip ? &g_clip : 0);
    return JS_NewInt32(ctx, 1);
}

/* imageSize(kind, src) -> [w, h] (natural size) or null when not usable */
FN(c_image_size) {
    UNUSED;
    int kind = inum(ctx, argv[0]), src = inum(ctx, argv[1]);
    const uint32_t *px;
    int w = 0, h = 0, nw = 0, nh = 0;
    if (kind == 0) {
        if (!rb_image_node_pixels(src, &px, &w, &h, &nw, &nh)) return JS_NULL;
    } else {
        rb_canvas_size(src, &nw, &nh);
    }
    JSValue a = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, a, 0, JS_NewInt32(ctx, nw));
    JS_SetPropertyUint32(ctx, a, 1, JS_NewInt32(ctx, nh));
    return a;
}

/* ---- pixels ---- */

/* get(slot, x, y, w, h, Uint8ClampedArray) */
FN(c_get) {
    UNUSED;
    size_t n;
    uint8_t *p = u8(ctx, argv[5], &n);
    int w = inum(ctx, argv[3]), h = inum(ctx, argv[4]);
    if (!p || w <= 0 || h <= 0 || (size_t)w * (size_t)h * 4 > n) return JS_UNDEFINED;
    rb_cv_get(inum(ctx, argv[0]), inum(ctx, argv[1]), inum(ctx, argv[2]), w, h, p);
    return JS_UNDEFINED;
}

/* put(slot, Uint8ClampedArray, iw, ih, dx, dy, x0, y0, x1, y1) */
FN(c_put) {
    UNUSED;
    size_t n;
    uint8_t *p = u8(ctx, argv[1], &n);
    int iw = inum(ctx, argv[2]), ih = inum(ctx, argv[3]);
    if (!p || iw <= 0 || ih <= 0 || (size_t)iw * (size_t)ih * 4 > n) return JS_UNDEFINED;
    rb_cv_put(inum(ctx, argv[0]), p, iw, ih, inum(ctx, argv[4]), inum(ctx, argv[5]),
              inum(ctx, argv[6]), inum(ctx, argv[7]), inum(ctx, argv[8]), inum(ctx, argv[9]));
    return JS_UNDEFINED;
}

/* png(slot) -> "data:image/png;base64,..." or null. The PNG is built on the
   JS heap: stored deflate blocks make it a little larger than the pixels. */
FN(c_png) {
    UNUSED;
    int slot = inum(ctx, argv[0]), cw, ch;
    if (!rb_canvas_size(slot, &cw, &ch)) return JS_NULL;
    size_t raw = (size_t)ch * (1 + 4 * (size_t)cw);
    size_t cap = raw + raw / 65535 * 5 + 256;
    if (cap > 64u * 1024 * 1024) return JS_NULL;
    uint8_t *png = js_malloc(ctx, cap);
    if (!png) return JS_EXCEPTION;
    int n = rb_cv_png(slot, png, (int)cap);
    if (n < 0) {
        js_free(ctx, png);
        return JS_NULL;
    }
    static const char pre[] = "data:image/png;base64,";
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out = sizeof(pre) - 1 + (size_t)(n + 2) / 3 * 4;
    char *s = js_malloc(ctx, out + 1);
    if (!s) {
        js_free(ctx, png);
        return JS_EXCEPTION;
    }
    size_t w = 0;
    for (size_t k = 0; k < sizeof(pre) - 1; k++) s[w++] = pre[k];
    for (int i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)png[i] << 16;
        if (i + 1 < n) v |= (uint32_t)png[i + 1] << 8;
        if (i + 2 < n) v |= png[i + 2];
        s[w++] = b64[(v >> 18) & 63];
        s[w++] = b64[(v >> 12) & 63];
        s[w++] = i + 1 < n ? b64[(v >> 6) & 63] : '=';
        s[w++] = i + 2 < n ? b64[v & 63] : '=';
    }
    js_free(ctx, png);
    JSValue r = JS_NewStringLen(ctx, s, w);
    js_free(ctx, s);
    return r;
}

static const JSCFunctionListEntry g_canvas_natives[] = {
    JS_CFUNC_DEF("cvOpen", 1, c_open),
    JS_CFUNC_DEF("cvResize", 3, c_resize),
    JS_CFUNC_DEF("cvAttrSize", 1, c_attr_size),
    JS_CFUNC_DEF("cvColor", 1, c_color),
    JS_CFUNC_DEF("cvPaint", 1, c_paint),
    JS_CFUNC_DEF("cvPaintNone", 0, c_paint_none),
    JS_CFUNC_DEF("cvGradient", 14, c_gradient),
    JS_CFUNC_DEF("cvComp", 7, c_comp),
    JS_CFUNC_DEF("cvFill", 10, c_fill),
    JS_CFUNC_DEF("cvStroke", 15, c_stroke),
    JS_CFUNC_DEF("cvInside", 11, c_inside),
    JS_CFUNC_DEF("cvInStroke", 16, c_in_stroke),
    JS_CFUNC_DEF("cvBounds", 8, c_bounds),
    JS_CFUNC_DEF("cvText", 11, c_text),
    JS_CFUNC_DEF("cvImage", 17, c_image),
    JS_CFUNC_DEF("cvImageSize", 2, c_image_size),
    JS_CFUNC_DEF("cvGet", 6, c_get),
    JS_CFUNC_DEF("cvPut", 10, c_put),
    JS_CFUNC_DEF("cvPng", 1, c_png),
};

void rjs_canvas_install(JSContext *ctx, JSValueConst rb) {
    g_paint_ok = 1;
    g_paint.kind = RB_CV_SOLID;
    g_paint.argb = 0xFF000000u;
    g_alpha = 255;
    g_op = RB_CV_OVER;
    g_has_clip = 0;
    JS_SetPropertyFunctionList(ctx, rb, g_canvas_natives, (int)(sizeof(g_canvas_natives) / sizeof(g_canvas_natives[0])));
}
