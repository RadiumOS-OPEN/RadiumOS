/* rbrowse web layout helpers: intrinsic sizes, inline-block and replaced
   elements, list markers. Included by engine.c before layout_inline_el, so
   everything here only needs layout_block's prototype. */

#define MC_AVAIL 3000

static int is_block_level(int d) {
    return d == DISP_BLOCK || d == DISP_FLEX || d == DISP_GRID || d == DISP_LIST_ITEM ||
           d == DISP_TABLE || d == DISP_TABLE_ROW || d == DISP_TABLE_ROWGROUP ||
           d == DISP_TABLE_CAPTION || d == DISP_TABLE_CELL;
}

/* ---- floats ------------------------------------------------------------------
   One flat list of active floats. A block formatting context root (overflow
   other than visible, inline-block, table cell, a float, an absolute box)
   hides the outer floats from its content (g_fl_base) and contains its own. */
#define MAX_FL 96
typedef struct { int x0, x1, y0, y1; uint8_t side; } FloatRec;
static FloatRec g_fl[MAX_FL];
static int g_fl_n, g_fl_base;

/* Narrow [*le, *re] by the floats overlapping the line band [y, y + h). */
static int fl_extent(int y, int h, int *le, int *re) {
    int any = 0;
    for (int i = g_fl_base; i < g_fl_n; i++) {
        const FloatRec *f = &g_fl[i];
        if (f->y1 <= y || f->y0 >= y + h) continue;
        any = 1;
        if (f->side == 1) { if (f->x1 > *le) *le = f->x1; }
        else if (f->x0 < *re) *re = f->x0;
    }
    return any;
}

/* The lowest bottom edge among floats overlapping y (to step past them). */
static int fl_next_y(int y) {
    int best = 0;
    for (int i = g_fl_base; i < g_fl_n; i++) {
        const FloatRec *f = &g_fl[i];
        if (f->y1 <= y || f->y0 > y) continue;
        if (!best || f->y1 < best) best = f->y1;
    }
    return best;
}

/* y below every float a `clear` value names (bit0 left, bit1 right). */
static int fl_clear_y(int y, int clr) {
    for (int i = g_fl_base; i < g_fl_n; i++) {
        const FloatRec *f = &g_fl[i];
        if (!((f->side == 1 && (clr & 1)) || (f->side == 2 && (clr & 2)))) continue;
        if (f->y1 > y) y = f->y1;
    }
    return y;
}

/* Re-derive the current (empty) line's extent from the active floats. */
static void fl_adjust(Lay *L) {
    if (!L->bright || g_fl_n <= g_fl_base) return;
    for (int k = 0; k < 24; k++) {
        int le = L->bleft, re = L->bright;
        int any = fl_extent(L->y, 8, &le, &re);
        if (!any || re - le >= 16) { L->left = le; L->right = re; break; }
        int ny = fl_next_y(L->y);
        if (!ny || ny <= L->y) { L->left = le; L->right = re; break; }
        L->y = ny;
    }
    if (L->right < L->left + 8) L->right = L->left + 8;
    if (!L->used) L->x = L->left;
}

/* ---- intrinsic (max-content) width ------------------------------------------
   Cached per node for one layout pass. The element is laid out at a very wide
   available width with its boxes discarded; the widest emitted box (ignoring
   the element's own background/border rectangles, which span the available
   width) is its max-content width. */
typedef struct { uint16_t node; int16_t w; } McEnt;
#define MC_N 2048
static McEnt g_mc[MC_N];
static int g_probe_depth;
static int g_work;                 /* layout_block calls this pass (work budget) */
#define WORK_BUDGET 90000
#define SOFT_BUDGET 40000          /* past this, measuring stops laying out and estimates */

/* Right edge of a box for intrinsic-size purposes: what an overflow:hidden ancestor cuts off
   (an sr-only label, a text-overflow ellipsis cell) does not widen its container. */
static int box_right_clipped(const RbBox *bx) {
    int r = bx->x + bx->w;
    if (bx->clip && bx->clip <= g_clip_n) {
        const ClipRect *c = &g_clip_tab[bx->clip - 1];
        if (r > c->x + c->w) r = c->x + c->w;
    }
    return r;
}

static int block_max_content(int node, RbColor ic, uint8_t isc, uint8_t ia) {
    McEnt *e = &g_mc[node & (MC_N - 1)];
    if (e->node == node) return e->w;
    Style st = compute_style(node, ic, isc, ia);
    if (g_probe_depth >= 5 || g_work > WORK_BUDGET) return RB_VIEW_W;
    int bn = g_box_n, pm = g_pool_n, cn = g_clip_n, on = g_oof_n;
    int fn = g_fl_n, fb = g_fl_base;
    g_fl_base = g_fl_n;
    g_force_w = g_force_h = -1;
    int sx = g_ext_right, ss = g_ext_skip;
    g_ext_right = -32768; g_ext_skip = node;
    g_probe_depth++;
    g_intr++;
    layout_block(node, 0, 0, MC_AVAIL, ic, isc, ia);
    g_intr--;
    g_probe_depth--;
    g_fl_n = fn;
    g_fl_base = fb;
    int ml = st.x.ml_auto ? 0 : m_px(st.ml, MC_AVAIL);
    int right = ml;      /* an empty block still starts at its left margin: its width is then 0, not -ml */
    for (int b = bn; b < g_box_n; b++) {
        const RbBox *bx = &g_box[b];
        if (bx->node == node && (bx->kind == RB_BOX_BG || bx->kind == RB_BOX_BORDER || bx->kind == RB_BOX_RULE)) continue;
        if (bx->flags & RB_BOX_OOF) continue;
        int r = box_right_clipped(bx);
        if (r > right) right = r;
    }
    if (g_ext_right > right) right = g_ext_right;
    g_ext_right = sx; g_ext_skip = ss;
    g_box_n = bn;
    g_pool_n = pm;
    g_clip_n = cn;
    g_oof_n = on;
    int w = right - ml + st.pr;
    if (st.width > 0) w = st.width;
    if (st.min_w > 0 && w < st.min_w) w = st.min_w;
    if (st.max_w > 0 && w > st.max_w) w = st.max_w;
    if (w < st.pl + st.pr) w = st.pl + st.pr;
    if (w > 30000) w = 30000;
    e->node = (uint16_t)node;
    e->w = (int16_t)w;
    return w;
}

/* ---- cached measure-only layout ------------------------------------------------------
   Flex, grid and shrink-to-fit lay an item out just to learn its height and
   natural width, then throw the boxes away. Done naively that is a full layout
   of the subtree per pass, per nesting level: exponential. The answer depends
   only on the node, the width and the forced sizes, so it is cached for the
   pass; the discarded layout also gives back everything it allocated. */
typedef struct {
    uint16_t node;
    int16_t w, fw, fh, fxw, fnw, fxh, fnh;
    uint8_t fit;
    int h, right;
} MeasEnt;
#define MEAS_N 16384
static MeasEnt g_meas[MEAS_N];

/* Lay `node` out at (x, y, w) with boxes discarded. Returns layout_block's
   height; *right gets the widest non-background box's right edge relative to x
   (-1 when nothing was emitted). */
static int measure_block(int node, int x, int y, int w, RbColor ic, uint8_t isc, uint8_t ia,
                         int fw, int fh, int *right) {
    int fxw = g_force_max_w, fnw = g_force_min_w, fxh = g_force_max_h, fnh = g_force_min_h;
    unsigned hsh = ((unsigned)node * 2654435761u) ^ ((unsigned)(w + 7) * 40503u) ^ ((unsigned)(fw + 3) * 9973u) ^
                   ((unsigned)(fh + 5) * 7919u) ^ ((unsigned)(fxw + 1) * 31u) ^ ((unsigned)(fnw + 1) * 17u) ^
                   ((unsigned)(fxh + 1) * 13u) ^ ((unsigned)(fnh + 1) * 11u) ^ ((unsigned)g_fit * 7777u);
    MeasEnt *e = &g_meas[(hsh >> 8) & (MEAS_N - 1)];
    if (g_work > SOFT_BUDGET && !(e->node == node && e->w == w && e->fw == fw && e->fh == fh)) {
        /* A deeply nested page has used its measuring allowance: stop paying a
           full layout per measure and guess (one line, as wide as the slot),
           so the real pass still finishes and shows the content. */
        if (right) *right = w;
        return 9;
    }
    if (e->node == node && e->w == w && e->fw == fw && e->fh == fh && e->fxw == fxw && e->fnw == fnw &&
        e->fxh == fxh && e->fnh == fnh && e->fit == (uint8_t)g_fit) {
        if (right) *right = e->right;
        return e->h;
    }
    int bn = g_box_n, pm = g_pool_n, cn = g_clip_n, on = g_oof_n, fn = g_fl_n, fb = g_fl_base;
    g_fl_base = g_fl_n;
    g_force_w = fw;
    g_force_h = fh;
    int sx = g_ext_right, ss = g_ext_skip;
    g_ext_right = -32768; g_ext_skip = node;
    g_intr++;
    int h = layout_block(node, x, y, w, ic, isc, ia);
    g_intr--;
    g_force_w = g_force_h = -1;
    int r = -1;
    if (g_ext_right > -32768 && g_ext_right - x > r) r = g_ext_right - x;
    g_ext_right = sx; g_ext_skip = ss;
    for (int b = bn; b < g_box_n; b++) {
        const RbBox *bx = &g_box[b];
        if ((bx->kind == RB_BOX_BG || bx->kind == RB_BOX_BORDER) && !(bx->flags & RB_BOX_REPL)) continue;
        if (bx->flags & RB_BOX_OOF) continue;
        int e2 = box_right_clipped(bx) - x;
        if (e2 > r) r = e2;
    }
    g_box_n = bn; g_pool_n = pm; g_clip_n = cn; g_oof_n = on; g_fl_n = fn; g_fl_base = fb;
    e->node = (uint16_t)node;
    e->w = (int16_t)w; e->fw = (int16_t)fw; e->fh = (int16_t)fh;
    e->fit = (uint8_t)g_fit;
    e->fxw = (int16_t)fxw; e->fnw = (int16_t)fnw; e->fxh = (int16_t)fxh; e->fnh = (int16_t)fnh;
    e->h = h;
    e->right = r;
    if (right) *right = r;
    return h;
}

/* Shortest the content of `node` (laid out under inherited ic/isc/ia) can be:
   its longest word, widened by the margins/padding/borders of nested blocks
   and by any fixed-width descendant. One cheap DOM walk, styles memoised. */
/* A nowrap flex row or a table row puts its children side by side, so their
   minimum widths add up instead of taking the widest. */
static int row_sums(const Style *st) {
    if (st->display == DISP_TABLE_ROW) return 1;
    return (st->display == DISP_FLEX || st->display == DISP_INLINE_FLEX) && (st->flex_dir == FLEX_ROW || st->flex_dir == FLEX_ROW_REV) &&
           st->flex_wrap == FLEX_NOWRAP;
}

static int replaced_size(int node, const Style *st, int avail, int *rw, int *rh);

static int min_content_words(int node, RbColor ic, uint8_t isc, uint8_t ia, int sum, int depth) {
    int best = 0, total = 0;
    int scale = isc ? isc : 1;
    for (int c = g_nodes[node].first; c; c = g_nodes[c].next) {
        if (g_nodes[c].dead || g_nodes[c].hidden) continue;
        if (g_nodes[c].kind == 1) {
            const char *s = g_pool + g_nodes[c].text_off;
            int run = 0, n = (int)g_nodes[c].text_len;
            for (int i = 0; i <= n; i++) {
                if (i == n || s[i] == ' ' || s[i] == '\n' || s[i] == '\t') {
                    if (run * 8 * scale > best) best = run * 8 * scale;
                    run = 0;
                } else {
                    run++;
                    if (brk_after(s, n, i)) { if (run * 8 * scale > best) best = run * 8 * scale; run = 0; }
                }
            }
            continue;
        }
        Style cs = compute_style(c, ic, isc, ia);
        if (cs.display == DISP_NONE || is_oof(&cs)) continue;     /* out of flow: no say in the width */
        int t = g_nodes[c].tag;
        int m;
        if (t == RB_TAG_INPUT || t == RB_TAG_BUTTON || t == RB_TAG_SELECT) m = 24;
        else if ((t == RB_TAG_IMG && g_images) || (t == RB_TAG_CANVAS && g_canvas)) {
            /* an image cannot be narrower than its size, unless a percentage
               width or max-width lets it shrink (CSS: compressible) */
            int rw = 0, rh = 0;
            m = (cs.width < 0 && cs.width != -32768) || cs.max_w < 0 ? 0 :
                replaced_size(c, &cs, MC_AVAIL, &rw, &rh) ? rw + cs.pl + cs.pr : 0;
        }
        else if (cs.width > 0) m = cs.width;
        else if (depth < 14) m = min_content_words(c, cs.color, cs.scale, cs.align, row_sums(&cs), depth + 1) + cs.pl + cs.pr;
        else m = 0;
        if (cs.max_w > 0 && m > cs.max_w) m = cs.max_w;      /* max-width caps what the box asks for */
        if (is_block_level(cs.display) || cs.display == DISP_INLINE_BLOCK || cs.display == DISP_INLINE_FLEX) {
            int ml = cs.x.ml_auto ? 0 : m_px(cs.ml, RB_VIEW_W), mr = cs.x.mr_auto ? 0 : m_px(cs.mr, RB_VIEW_W);
            if (ml > 0) m += ml;
            if (mr > 0) m += mr;
        }
        if (m > best) best = m;
        total += m;
    }
    return sum ? total : best;
}

/* ---- replaced elements ---------------------------------------------------------- */

static int is_replaced_tag(int tag) {
    return tag == RB_TAG_IMG || tag == RB_TAG_SVG || tag == RB_TAG_CANVAS || tag == RB_TAG_VIDEO ||
           tag == RB_TAG_IFRAME || tag == RB_TAG_OBJECT || tag == RB_TAG_EMBED || tag == RB_TAG_PROGRESS ||
           tag == RB_TAG_METER;
}

/* An <img> laid out as an RB_BOX_IMAGE box (image mode): loaded, or (web
   profile) still loading. Broken or src-less images keep the placeholder. */
static int img_as_box(int node) {
    if (!g_images || g_nodes[node].tag != RB_TAG_IMG) return 0;
    int s = g_nodes[node].img_state;
    return s == RB_IMG_OK || (g_web && s == RB_IMG_NONE);
}

/* Content size of a replaced element (screen px). Returns 0 when it has no
   box at all (an empty <img> with no size: its alt text flows inline).
   In image mode an <img> takes the other dimension, or both, from its
   natural size, and is not halved vertically in the web profile (engine.h,
   "images"): a CSS/attribute height arrives halved and is doubled back.
   A canvas in canvas mode does the same with its width/height attributes
   as the natural size. */
static int replaced_size(int node, const Style *st, int avail, int *rw, int *rh) {
    int tag = g_nodes[node].tag;
    int cvs = tag == RB_TAG_CANVAS && g_canvas;
    int img = tag == RB_TAG_IMG || cvs;          /* sized like an image */
    int unhalve = img && g_web && (g_images || cvs);
    int w = 0, h = 0;
    int aw = st->width, ah = st->height;
    if (aw > 0) w = aw - st->pl - st->pr;
    else if (aw < 0 && aw != -32768) w = resolve_pct(aw, avail, 1) + st->wadj - st->pl - st->pr;
    if (ah > 0) h = unhalve ? ah * 2 : ah;
    else if (ah < 0 && ah != -32768) h = 0;
    if (w < 0) w = 0;
    if (w > 30000) w = 30000;              /* keeps the products below in int */
    if (h > 30000) h = 30000;
    int fixed = w && h;
    int dw = 0, dh = 0;
    switch (tag) {
    case RB_TAG_IMG:
        if (g_images && g_nodes[node].img_state == RB_IMG_OK) { dw = g_nodes[node].img_w; dh = g_nodes[node].img_h; }
        break;
    case RB_TAG_SVG: dw = 20; dh = zy(20); break;
    case RB_TAG_CANVAS:
        if (cvs) { rb_canvas_attr_size(node, &dw, &dh); break; }
        dw = 300; dh = zy(150); break;
    case RB_TAG_IFRAME: case RB_TAG_VIDEO: case RB_TAG_OBJECT: case RB_TAG_EMBED:
        dw = 300; dh = zy(150); break;
    case RB_TAG_PROGRESS: case RB_TAG_METER: dw = 160; dh = zy(16); break;
    default: break;
    }
    if (!dw || !dh) dw = dh = 0;
    if (!w && !h) { w = dw; h = dh; }
    else if (w && !h) h = dw ? w * dh / dw : (img ? w * 3 / 4 : w);
    else if (!w && h) w = dh ? h * dw / dh : (img ? h * 4 / 3 : h);
    if (w > 30000) w = 30000;
    if (h > 30000) h = 30000;
    if (tag == RB_TAG_IMG) g_nodes[node].img_fixed = (uint8_t)(fixed && g_images);
    if (!w || !h) { *rw = *rh = 0; return 0; }
    /* max-width / max-height clamp, keeping the aspect ratio */
    int mx = st->max_w != 0 ? st_len_px(st->max_w, avail) + (st->max_w < 0 ? st->maxw_adj : 0) : 0;
    if (mx > 0) mx -= st->pl + st->pr;
    int cap = avail - st->pl - st->pr;
    if (cap < 1) cap = 1;
    if (mx > 0 && mx < cap) cap = mx;
    if (w > cap) { h = h * cap / w; w = cap; }
    int mxh = st->max_h > 0 ? (unhalve ? st->max_h * 2 : st->max_h) : 0;
    if (mxh > 0 && h > mxh) { w = w * mxh / h; h = mxh; }
    if (img && h > 8000) { w = w * 8000 / h; h = 8000; }
    if (st->min_w > 0 && w < st->min_w - st->pl - st->pr) w = st->min_w - st->pl - st->pr;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    *rw = w;
    *rh = h;
    return 1;
}

/* Draw the stand-in for a replaced element: a pale box for sizeable images,
   with the alt text (or a tag label) inside; nothing for small icons. */
static void emit_replaced(int node, int hot, const Style *st, int x, int y, int w, int h) {
    int tag = g_nodes[node].tag;
    int owner = hot ? hot : node;
    if (tag == RB_TAG_PROGRESS || tag == RB_TAG_METER) {
        int fill = rb_map_rgb(200, 200, 200);
        emit_box(x, y, w, h, owner, st->color, fill, 1, RB_BOX_BG);
        return;
    }
    if (tag == RB_TAG_CANVAS && g_canvas) {
        /* the painter draws the pixels over the element's background */
        int b = emit_box(x, y, w, h, owner, st->color, st->has_bg ? st->bg : 0, 1, RB_BOX_CANVAS);
        if (b >= 0) {
            g_box[b].flags |= RB_BOX_REPL | (st->has_bg ? RB_BOX_IMGBG : 0);
            g_box[b].toff = (uint32_t)node;
        }
        return;
    }
    if (img_as_box(node)) {
        /* the painter draws the pixels once decoded, the fill until then */
        int b = emit_box(x, y, w, h, owner, st->color, st->has_bg ? st->bg : rb_map_rgb(232, 232, 232), 1, RB_BOX_IMAGE);
        if (b >= 0) {
            g_box[b].flags |= RB_BOX_REPL | (st->has_bg ? RB_BOX_IMGBG : 0);
            g_box[b].toff = (uint32_t)node;
        }
        return;
    }
    int big = w >= 12 && h >= 12;
    if (big) {
        int fill = tag == RB_TAG_IMG ? rb_map_rgb(232, 232, 232) : rb_map_rgb(240, 240, 240);
        if (st->has_bg) fill = st->bg;
        int b = emit_box(x, y, w, h, owner, st->color, fill, 1, RB_BOX_BG);
        if (b >= 0) g_box[b].flags |= RB_BOX_REPL;
        if (b >= 0 && st->x.radius) g_box[b].radius = st->x.radius == RADIUS_FULL ? (uint8_t)(w < h ? w / 2 : h / 2) : st->x.radius;
    } else if (st->has_bg) {
        emit_box(x, y, w, h, owner, st->color, st->bg, 1, RB_BOX_BG);
    }
    char label[64];
    int n = 0;
    if (tag == RB_TAG_IMG && g_nodes[node].text_len) {
        n = (int)g_nodes[node].text_len;
        if (n > 60) n = 60;
        memcpy(label, g_pool + g_nodes[node].text_off, (size_t)n);
    } else if (big && tag == RB_TAG_IFRAME && frame_src(node, g_frame_buf, (int)sizeof(g_frame_buf)) > 0) {
        /* "[open frame: example.com/path]": click or Enter opens it as the page */
        const char *src = g_frame_buf;
        const char *p = src;
        for (const char *q = src; *q; q++)
            if (q[0] == '/' && q[1] == '/') { p = q + 2; break; }
        static const char pre[] = "[open frame: ";
        n = 0;
        for (int k = 0; pre[k] && n < 60; k++) label[n++] = pre[k];
        for (; *p && n < 61; p++) label[n++] = *p;
        if (n > 61) n = 61;
        label[n++] = ']';
    } else if (big && tag != RB_TAG_IMG) {
        const char *nm = tag == RB_TAG_IFRAME ? "[iframe]" : tag == RB_TAG_VIDEO ? "[video]" :
                         tag == RB_TAG_CANVAS ? "[canvas]" : tag == RB_TAG_SVG ? "" : "[embed]";
        n = (int)strlen(nm);
        memcpy(label, nm, (size_t)n);
    }
    if (n > 0 && big) {
        int maxc = (w - 4) / 8;
        if (maxc < 1) return;
        if (n > maxc) n = maxc;
        int ty = y + 2;
        if (ty + 8 > y + h) ty = y + (h - 8) / 2;
        bind_text(emit_box(x + 2, ty, n * 8, 8, owner, rb_map_rgb(110, 110, 110), 0, 1, RB_BOX_TEXT), label, n);
    }
}

/* ---- inline-level placement ----------------------------------------------------------- */

static void place_replaced_inline(int node, const Style *st, Lay *L, int hot) {
    int avail = L->right - L->left;
    int rw, rh;
    int ml = st->x.ml_auto ? 0 : m_px(st->ml, avail), mr = st->x.mr_auto ? 0 : m_px(st->mr, avail);
    if (!replaced_size(node, st, avail - ml - mr, &rw, &rh)) {
        /* unsized image: its alt text flows like a link label */
        if (g_nodes[node].tag == RB_TAG_IMG && g_nodes[node].text_len) {
            place_text(L, g_pool + g_nodes[node].text_off, (int)g_nodes[node].text_len,
                       hot ? hot : node, st->color, st->scale, 0);
        }
        return;
    }
    int ow = rw + st->pl + st->pr;
    if (L->x > L->left && L->x + ow + ml + mr > L->right) newline(L);
    int x = L->x + ml;
    int y = L->y + st->mt;
    int oh = rh + st->pt + st->pb;
    /* border of the element itself */
    for (int k = 0; k < 4; k++) {
        if (!st->x.bw[k]) continue;
        int bc = st->x.bcol[k] == BC_CUR ? st->color : st->x.bcol[k];
        int bw = st->x.bw[k];
        if (k == 0) emit_box(x, y, ow, bw, hot ? hot : node, st->color, bc, 1, RB_BOX_BG);
        else if (k == 1) emit_box(x + ow - bw, y, bw, oh, hot ? hot : node, st->color, bc, 1, RB_BOX_BG);
        else if (k == 2) emit_box(x, y + oh - bw, ow, bw, hot ? hot : node, st->color, bc, 1, RB_BOX_BG);
        else emit_box(x, y, bw, oh, hot ? hot : node, st->color, bc, 1, RB_BOX_BG);
    }
    emit_replaced(node, hot, st, x + st->pl, y + st->pt, rw, rh);
    /* an image inside a link paints under the link's node: record its own
       border box so rb_dom_rect (img.width, getBoundingClientRect) finds it */
    if (g_nodes[node].tag == RB_TAG_IMG || g_nodes[node].tag == RB_TAG_CANVAS) NBOX(node, x, y, ow, oh);
    L->x += ml + ow + mr + (st->scale > 1 ? 0 : 0);
    int lh = oh + st->mt + st->mb;
    if (st->valign == 4 || st->valign == 5) lh = oh;      /* sub / super: ignore */
    if (lh > L->line_h) L->line_h = lh;
    L->used = 1;
}

static void place_inline_block(int node, const Style *st, Lay *L, RbColor ic, uint8_t isc, uint8_t ia) {
    int avail = L->right - L->left;
    int ml = st->x.ml_auto ? 0 : m_px(st->ml, avail), mr = st->x.mr_auto ? 0 : m_px(st->mr, avail);
    int w = block_max_content(node, ic, isc, ia);
    if (st->width < 0 && st->width != -32768) w = resolve_pct(st->width, avail, 1) + st->wadj;
    if (w > avail - ml - mr) w = avail - ml - mr;
    if (w < st->pl + st->pr + 1) w = st->pl + st->pr + 1;
    int need = w + ml + mr;
    if (L->x > L->left && L->x + need > L->right) newline(L);
    g_force_w = w;
    int h = layout_block(node, L->x, L->y, need, ic, isc, ia);
    L->x += need;
    if (g_intr && L->x > g_ext_right) g_ext_right = L->x;     /* an empty inline-block still takes room */
    if (h > L->line_h) L->line_h = h;
    L->used = 1;
}

/* ---- list markers ----------------------------------------------------------------------- */

static int g_ord_node, g_ord_val;      /* last item numbered: its successor is one more */

static int li_ordinal(int node) {
    int n = 1;
    int pl = 0;
    for (int p = g_nodes[node].prev; p; p = g_nodes[p].prev)
        if (g_nodes[p].kind == 0 && !g_nodes[p].dead && g_nodes[p].tag == RB_TAG_LI) { pl = p; break; }
    if (pl && pl == g_ord_node && g_nodes[pl].gen && g_ord_val > 0) {
        n = g_ord_val + 1;
        g_ord_node = node; g_ord_val = n;
        const char *v0; int vn0;
        (void)v0; (void)vn0;
        return n;
    }
    for (int p = g_nodes[node].prev; p; p = g_nodes[p].prev)
        if (g_nodes[p].kind == 0 && !g_nodes[p].dead && g_nodes[p].tag == RB_TAG_LI) n++;
    int par = g_nodes[node].parent;
    const char *v; int vn;
    if (par && attr_get(par, "start", 5, &v, &vn)) {
        int s = 0;
        if (parse_int_tok(v, vn, &s)) n += s - 1;
    }
    g_ord_node = node; g_ord_val = n;
    return n;
}

static int fmt_counter(int n, int type, char *out) {
    int w = 0;
    if (type == LST_LALPHA || type == LST_UALPHA) {
        char tmp[8];
        int k = 0;
        if (n < 1) n = 1;
        while (n > 0 && k < 7) { n--; tmp[k++] = (char)((type == LST_LALPHA ? 'a' : 'A') + n % 26); n /= 26; }
        while (k > 0) out[w++] = tmp[--k];
    } else if (type == LST_LROMAN || type == LST_UROMAN) {
        static const int vals[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
        static const char *const syms[] = { "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i" };
        if (n < 1 || n > 3999) n = n < 1 ? 1 : 3999;
        for (int k = 0; k < 13; k++)
            while (n >= vals[k]) {
                for (const char *s = syms[k]; *s; s++) out[w++] = type == LST_UROMAN ? (char)(*s - 32) : *s;
                n -= vals[k];
            }
    } else {
        char tmp[12];
        int k = 0;
        if (n < 0) { out[w++] = '-'; n = -n; }
        if (n == 0) tmp[k++] = '0';
        while (n > 0 && k < 11) { tmp[k++] = (char)('0' + n % 10); n /= 10; }
        while (k > 0) out[w++] = tmp[--k];
    }
    out[w] = 0;
    return w;
}

/* Marker for a list item, drawn in the gutter just left of its border box. */
static void emit_list_marker(int node, const Style *st, int ix, int y) {
    int type = st->lstype ? st->lstype : LST_DISC;
    if (type == LST_NONE) return;
    int step = 8 * st->scale;
    if (type == LST_DISC || type == LST_CIRCLE || type == LST_SQUARE) {
        int sz = st->scale > 1 ? 7 : 4;
        int mx = ix - sz - 4;
        int my = y + (step - sz) / 2 + 1;
        if (type == LST_SQUARE) { emit_box(mx, my, sz - 1, sz - 1, node, st->color, st->color, 1, RB_BOX_BG); return; }
        if (type == LST_DISC) {
            int b = emit_box(mx, my, sz, sz, node, st->color, st->color, 1, RB_BOX_BG);
            if (b >= 0) g_box[b].radius = (uint8_t)(sz / 2);
        } else {
            int b = emit_box(mx, my, sz, sz, node, st->color, st->color, 1, RB_BOX_BORDER);
            if (b >= 0) { g_box[b].scale = 1; g_box[b].radius = (uint8_t)(sz / 2); }
        }
        return;
    }
    char buf[24];
    int n = fmt_counter(li_ordinal(node), type, buf);
    buf[n++] = '.';
    buf[n] = 0;
    int mx = ix - n * step - step / 2;
    bind_text(emit_box(mx, y, n * step, step, node, st->color, 0, st->scale, RB_BOX_TEXT), buf, n);
}

/* ---- placing a float ---------------------------------------------------------------- */

static void place_float(int node, const Style *st, Lay *L, RbColor ic, uint8_t isc, uint8_t ia) {
    int avail = L->bright ? L->bright - L->bleft : L->right - L->left;
    int ml = st->x.ml_auto ? 0 : m_px(st->ml, avail), mr = st->x.mr_auto ? 0 : m_px(st->mr, avail);
    int w;
    if (st->width > 0) w = st->width;
    else if (st->width < 0 && st->width != -32768) w = resolve_pct(st->width, avail, 1) + st->wadj;
    else w = block_max_content(node, ic, isc, ia);
    if (g_web && is_replaced_tag(g_nodes[node].tag) && st->width == -32768) {
        int rw, rh;
        if (replaced_size(node, st, avail - ml - mr, &rw, &rh)) w = rw + st->pl + st->pr;
    }
    if (w > avail - ml - mr) w = avail - ml - mr;
    if (w < st->pl + st->pr + 1) w = st->pl + st->pr + 1;
    int need = w + ml + mr;
    int left_side = st->flt == 1;
    int y = L->y;
    if (st->clr) y = fl_clear_y(y, st->clr);
    int x = 0;
    for (int k = 0; k < 32; k++) {
        int le = L->bright ? L->bleft : L->left, re = L->bright ? L->bright : L->right;
        int any = fl_extent(y, 1, &le, &re);
        if (re - le >= need || !any) { x = left_side ? le : re - need; break; }
        int ny = fl_next_y(y);
        if (!ny || ny <= y) { x = left_side ? le : re - need; break; }
        y = ny;
    }
    int save_hot = g_hot_in;
    int fb = g_fl_base;
    g_fl_base = g_fl_n;          /* the float's content is its own formatting context */
    int fn = g_fl_n;
    g_force_w = w;
    int h = layout_block(node, x, y, need, ic, isc, ia);
    g_fl_n = fn;                 /* what it floated inside is contained */
    g_fl_base = fb;
    g_hot_in = save_hot;
    if (g_fl_n >= MAX_FL) {      /* drop the oldest rather than the newest */
        for (int i = 1; i < g_fl_n; i++) g_fl[i - 1] = g_fl[i];
        g_fl_n--;
        if (g_fl_base > 0) g_fl_base--;
        SET_OOM();
    }
    FloatRec *f = &g_fl[g_fl_n++];
#ifdef RBROWSE_HOST
    if (getenv("RB_FLDBG")) fprintf(stderr, "float node %d x %d y %d need %d h %d side %d\n", node, x, y, need, h, left_side);
#endif
    f->x0 = x; f->x1 = x + need; f->y0 = y; f->y1 = y + h; f->side = (uint8_t)(left_side ? 1 : 2);
    if (!L->used) fl_adjust(L);
}

/* ---- generated content (::before / ::after) --------------------------------------------
   Materialised as real child elements (web profile only): the owner's pseudo
   rules style them, selectors skip them, and layout needs no special case. */

static void pseudo_strip(void) {
    for (int n = 2; n < g_node_n; n++) {
        if (g_nodes[n].dead || !g_nodes[n].pseudo) continue;
        rb_dom_remove_child(n);
    }
}

static int pseudo_has_rules(void) {
    for (int r = 0; r < g_crule_n; r++) if (g_crule[r].pseudo) return 1;
    return 0;
}

static void pseudo_make(int owner, int which, int content_id) {
    int pn = new_node();
    if (!pn) { g_stop = 1; return; }
    g_nodes[pn].tag = RB_TAG_OTHER;
    g_nodes[pn].pseudo = (uint8_t)which;
    int len;
    const char *txt = intern_str(content_id - 1, &len);
    if (len > 0) {
        int off = pool_add(txt, len);
        int t = off >= 0 ? new_node() : 0;
        if (t) {
            g_nodes[t].kind = 1;
            g_nodes[t].text_off = (uint32_t)off;
            g_nodes[t].text_len = (uint32_t)len;
            add_child(pn, t);
        }
    }
    if (which == 1 && g_nodes[owner].first) {
        int f = g_nodes[owner].first;
        g_nodes[pn].parent = (uint16_t)owner;
        g_nodes[pn].next = (uint16_t)f;
        g_nodes[pn].prev = 0;
        g_nodes[f].prev = (uint16_t)pn;
        g_nodes[owner].first = (uint16_t)pn;
    } else add_child(owner, pn);
}

static void pseudo_materialize(void) {
    pseudo_strip();
    if (!pseudo_has_rules()) return;
    int top = g_node_n;
    for (int n = 2; n < top; n++) {
        Node *nd = &g_nodes[n];
        if (nd->dead || nd->kind != 0 || nd->pseudo) continue;
        int tag = nd->tag;
        if (tag_is_void(tag) || raw_kind(tag) || tag == RB_TAG_TEXTAREA || tag == RB_TAG_SELECT ||
            is_replaced_tag(tag)) continue;
        for (int which = 1; which <= 2; which++) {
            Style st;
            int content = compute_pseudo_style(n, which, &st, g_nodes[n].color, 1, 0);
            if (content > 0 && st.display != DISP_NONE) pseudo_make(n, which, content);
        }
    }
}
