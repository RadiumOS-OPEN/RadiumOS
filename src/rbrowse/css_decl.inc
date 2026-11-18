/* rbrowse CSS declarations: property ids, the declaration store and the
   property/shorthand parsers. Included by engine.c after css_value.inc. */

enum {
    P_COLOR, P_BG, P_FSIZE, P_DISPLAY, P_TEXTALIGN,
    P_MT, P_MR, P_MB, P_ML, P_PT, P_PR, P_PB, P_PL,
    P_WIDTH, P_HEIGHT, P_MINW, P_MAXW, P_MINH, P_MAXH,
    P_FLEXDIR, P_JUSTIFY, P_ALIGNITEMS, P_ALIGNSELF, P_FLEXWRAP, P_ALIGNCONTENT,
    P_ROWGAP, P_COLGAP, P_GROW, P_SHRINK, P_BASIS, P_ORDER,
    P_POS, P_TOP, P_RIGHT, P_BOTTOM, P_LEFT, P_ZINDEX, P_OVERFLOW, P_VISIBILITY,
    P_OPACITY, P_CURSOR, P_BOXSZ,
    P_BWT, P_BWR, P_BWB, P_BWL, P_BCT, P_BCR, P_BCB, P_BCL, P_BST, P_BSR, P_BSB, P_BSL,
    P_RADIUS, P_WEIGHT, P_DECOR, P_TRANSFORM, P_WS, P_LINEH, P_LETSP,
    P_GCOLS, P_JITEMS, P_GCOL, P_GCOLSPAN, P_GROWL, P_GROWSPAN,
    P_FLOAT, P_CLEAR, P_VALIGN, P_INDENT, P_LSTYPE, P_COLLAPSE, P_BSPACE, P_CONTENT,
    P_ITALIC, P_TX, P_TY, P_JSELF, P_GAREAS, P_GAREA, P_COUNT
};

/* One declaration. val's meaning is per property (see apply_decl). */
typedef struct {
    uint8_t prop;
    uint8_t imp : 1;
    uint8_t kind : 1;  /* 0 normal, 1 inherit */
    uint8_t rem : 1;   /* em is in rem (root font-size) instead of em (own font-size) */
    int16_t em;        /* thousandths of an em/rem the length is made of, 0 = plain px (val already holds it at 16px) */
    int32_t val;
} Decl;

#define MAX_DECL LIM(40000, 1000000)
static Decl g_decl[MAX_DECL];
static int g_decl_n;

/* Output sink for the declaration parser: the global store for stylesheets, a
   small stack buffer for style="" attributes. */
static Decl *g_dout = g_decl;
static int *g_dn = &g_decl_n;
static int g_dcap = MAX_DECL;

/* A length that was written in em or rem also records that, so the cascade can
   resolve it against the right font-size (val is the 16px-font value). */
static void emit_decl_e(int prop, int32_t val, int imp, int em, int rem) {
    if (*g_dn >= g_dcap) { if (g_dout == g_decl) SET_OOM(); return; }
    Decl *d = &g_dout[(*g_dn)++];
    d->prop = (uint8_t)prop;
    d->imp = imp != 0;
    d->kind = 0;
    d->rem = rem != 0;
    d->em = (int16_t)(rem ? rem : em);
    d->val = val;
}

static void emit_decl(int prop, int32_t val, int imp) { emit_decl_e(prop, val, imp, 0, 0); }

static void emit_inherit(int prop, int imp) {
    if (*g_dn >= g_dcap) return;
    Decl *d = &g_dout[(*g_dn)++];
    d->prop = (uint8_t)prop;
    d->imp = imp != 0;
    d->kind = 1;
    d->rem = 0;
    d->em = 0;
    d->val = 0;
}

/* Side storage for values that do not fit an int32 (grid tracks, strings). */
#define MAX_AUX 8192
static int16_t g_aux[MAX_AUX];
static int g_aux_n;

/* ---- keyword tables ------------------------------------------------------ */

static int kw_find(const char *v, int n, const char *const *tab) {
    for (int i = 0; tab[i]; i++) if (eq_lit(v, n, tab[i])) return i;
    return -1;
}

static const char *const KW_DISPLAY[] = { "inline", "block", "none", "flex", "grid", "inline-block",
    "table", "table-row", "table-cell", "list-item", "table-row-group", "table-caption",
    "contents", "inline-flex", "inline-grid", "inline-table", "table-header-group",
    "table-footer-group", "table-column", "table-column-group", "flow-root", 0 };
static const uint8_t KW_DISPLAY_V[] = { DISP_INLINE, DISP_BLOCK, DISP_NONE, DISP_FLEX, DISP_GRID,
    DISP_INLINE_BLOCK, DISP_TABLE, DISP_TABLE_ROW, DISP_TABLE_CELL, DISP_LIST_ITEM,
    DISP_TABLE_ROWGROUP, DISP_TABLE_CAPTION, DISP_CONTENTS, DISP_INLINE_FLEX, DISP_GRID, DISP_TABLE,
    DISP_TABLE_ROWGROUP, DISP_TABLE_ROWGROUP, DISP_NONE, DISP_NONE, DISP_BLOCK };

static const char *const KW_POS[] = { "static", "relative", "absolute", "fixed", "sticky", 0 };
static const char *const KW_OVF[] = { "visible", "hidden", "clip", "auto", "scroll", 0 };
static const char *const KW_CURSOR[] = { "auto", "default", "pointer", "text", "move", "wait", "help",
    "crosshair", "not-allowed", "grab", 0 };
static const char *const KW_FLEXDIR[] = { "row", "column", "row-reverse", "column-reverse", 0 };
static const char *const KW_FLEXWRAP[] = { "nowrap", "wrap", "wrap-reverse", 0 };
static const char *const KW_JUSTIFY[] = { "flex-start", "flex-end", "center", "space-between",
    "space-around", "space-evenly", "start", "end", "left", "right", "normal", "stretch", 0 };
static const uint8_t KW_JUSTIFY_V[] = { JUSTIFY_START, JUSTIFY_END, JUSTIFY_CENTER, JUSTIFY_BETWEEN,
    JUSTIFY_AROUND, JUSTIFY_EVENLY, JUSTIFY_START, JUSTIFY_END, JUSTIFY_START, JUSTIFY_END,
    JUSTIFY_START, JUSTIFY_START };
static const char *const KW_ALIGN[] = { "flex-start", "flex-end", "center", "stretch", "start", "end",
    "baseline", "normal", "self-start", "self-end", "first baseline", "last baseline", 0 };
static const uint8_t KW_ALIGN_V[] = { ALIGN_START, ALIGN_END, ALIGN_CENTER, ALIGN_STRETCH, ALIGN_START,
    ALIGN_END, ALIGN_START, ALIGN_STRETCH, ALIGN_START, ALIGN_END, ALIGN_START, ALIGN_START };
static const char *const KW_ACNT[] = { "flex-start", "flex-end", "center", "stretch", "space-between",
    "space-around", "space-evenly", "start", "end", "normal", 0 };
static const uint8_t KW_ACNT_V[] = { ACNT_START, ACNT_END, ACNT_CENTER, ACNT_STRETCH, ACNT_BETWEEN,
    ACNT_AROUND, ACNT_EVENLY, ACNT_START, ACNT_END, ACNT_STRETCH };
static const char *const KW_TALIGN[] = { "left", "center", "right", "justify", "start", "end", "-webkit-center",
    "-moz-center", 0 };
/* 3 = -webkit-center: centres inline content AND shrink-wrapped block children, but a table
   does not inherit it (see compute_style) */
static const uint8_t KW_TALIGN_V[] = { 0, 1, 2, 0, 0, 2, 3, 1 };
static const char *const KW_FLOAT[] = { "none", "left", "right", "inline-start", "inline-end", 0 };
static const uint8_t KW_FLOAT_V[] = { 0, 1, 2, 1, 2 };
static const char *const KW_CLEAR[] = { "none", "left", "right", "both", "inline-start", "inline-end", 0 };
static const uint8_t KW_CLEAR_V[] = { 0, 1, 2, 3, 1, 2 };
static const char *const KW_VALIGN[] = { "baseline", "top", "middle", "bottom", "sub", "super",
    "text-top", "text-bottom", 0 };
static const char *const KW_LSTYPE[] = { "disc", "circle", "square", "decimal", "none", "lower-alpha",
    "upper-alpha", "lower-roman", "upper-roman", "decimal-leading-zero", "lower-latin", "upper-latin",
    "disclosure-open", "disclosure-closed", 0 };
static const uint8_t KW_LSTYPE_V[] = { LST_DISC, LST_CIRCLE, LST_SQUARE, LST_DECIMAL, LST_NONE,
    LST_LALPHA, LST_UALPHA, LST_LROMAN, LST_UROMAN, LST_DECIMAL, LST_LALPHA, LST_UALPHA, LST_DISC, LST_DISC };
static const char *const KW_BSTYLE[] = { "none", "hidden", "solid", "dashed", "dotted", "double",
    "groove", "ridge", "inset", "outset", 0 };

/* ---- generic value helpers ------------------------------------------------ */

/* Resolve one length token to the engine's signed encoding: px as-is, a
   percentage as OFF_PCT + p, OFF_AUTO for the keyword. Returns 0 when it is
   none of those. allow_auto: accept "auto". */
static int g_tok_em, g_tok_rem;      /* em/rem part of the token parse_off_tok/parse_size_tok just read */

static int parse_off_tok(const char *s, int n, int allow_auto, int32_t *out) {
    g_tok_em = g_tok_rem = 0;
    trim_tok(&s, &n);
    if (allow_auto && eq_lit(s, n, "auto")) { *out = OFF_AUTO; return 1; }
    int px, pct, hp;
    if (!parse_length(s, n, &px, &pct, &hp)) return 0;
    if (hp) {
        if (pct > 200) pct = 200;
        if (pct < -200) pct = -200;
        /* a mixed calc(p% + x px) keeps only the percentage here */
        *out = OFF_PCT + pct;
        return 1;
    }
    if (px > 8000) px = 8000;
    else if (px < -8000) px = -8000;
    else { g_tok_em = g_len_em; g_tok_rem = g_len_rem; }
    *out = px;
    return 1;
}

/* width/height/min/max: px >= 0, percentage < 0, auto = -32768. A
   calc(p% + x px) packs the px adjustment in the high 16 bits. */
static int parse_size_tok(const char *s, int n, int is_minmax, int32_t *out) {
    g_tok_em = g_tok_rem = 0;
    trim_tok(&s, &n);
    if (eq_lit(s, n, "auto") || eq_lit(s, n, "none") || eq_lit(s, n, "fit-content") ||
        eq_lit(s, n, "max-content") || eq_lit(s, n, "min-content") || eq_lit(s, n, "stretch") ||
        eq_lit(s, n, "-webkit-fill-available") || eq_lit(s, n, "-moz-available") ||
        starts_lit(s, n, "fit-content(")) {
        *out = is_minmax ? 0 : -32768;
        return 1;
    }
    int px, pct, hp;
    if (!parse_length(s, n, &px, &pct, &hp)) return 0;
    if (hp) {
        if (pct > 100) pct = 100;
        if (pct < 0) pct = 0;
        if (px == 0) { *out = pct ? -pct : 0; return 1; }
        if (px > 8000) px = 8000;
        if (px < -8000) px = -8000;
        /* calc(p% + x px): flag | adj+8192 << 8 | pct */
        *out = (int32_t)((1u << 30) | ((uint32_t)(px + 8192) << 8) | (uint32_t)pct);
        return 1;
    }
    if (px >= 0 && px <= 30000) { g_tok_em = g_len_em; g_tok_rem = g_len_rem; }
    if (px < 0) px = 0;
    if (px > 30000) px = 30000;
    *out = px;
    return 1;
}

/* translate() operand: like parse_off_tok, but a mixed calc(p% + x px) keeps both parts, packed as
   TR_MIX | (x + 8192) << 9 | (p + 256). Decoded by tr_px() in engine.c. */
static int parse_tr_tok(const char *s, int n, int32_t *out) {
    trim_tok(&s, &n);
    int px, pct, hp;
    if (!parse_length(s, n, &px, &pct, &hp)) return 0;
    if (hp && px) {
        if (pct > 200) pct = 200;
        if (pct < -200) pct = -200;
        if (px > 8000) px = 8000;
        if (px < -8000) px = -8000;
        *out = (int32_t)(TR_MIX | ((uint32_t)(px + 8192) << 9) | (uint32_t)(pct + 256));
        return 1;
    }
    return parse_off_tok(s, n, 0, out);
}

static void emit_kw(int prop, const char *v, int vn, const char *const *tab, const uint8_t *map, int imp) {
    int k = kw_find(v, vn, tab);
    if (k < 0) return;
    emit_decl(prop, map ? map[k] : k, imp);
}

static int is_inherit_kw(const char *v, int vn) { return eq_lit(v, vn, "inherit"); }

/* ---- border ---------------------------------------------------------------- */

#define BC_NONE_SET 0x1000001   /* border colour decl: not yet resolved (above any 0xRRGGBB) */

static int border_width_tok_ax(const char *t, int tn, int *w, int axis) {
    int save = g_axis;
    g_axis = axis;
    int ok = 0;
    if (eq_lit(t, tn, "thin")) { *w = 1; ok = 1; }
    else if (eq_lit(t, tn, "medium")) { *w = px_zoom(3); ok = 1; }
    else if (eq_lit(t, tn, "thick")) { *w = px_zoom(5); ok = 1; }
    else {
        int px, pct, hp;
        if (tn > 0 && (is_digit_c(t[0]) || t[0] == '.') && parse_length(t, tn, &px, &pct, &hp) && !hp) {
            *w = px > 8 ? 8 : px;
            ok = 1;
        }
    }
    g_axis = save;
    return ok;
}

/* "<width> <style> <colour>" in any order; emits for the sides in `sides`
   (bit0 top, 1 right, 2 bottom, 3 left). */
static void emit_border(const char *val, int vn, int sides, int imp) {
    int i = 0, tn;
    const char *t;
    int wx = 3, wy = zy(3), style = 2, color = BC_CUR;     /* initial: medium, (set) solid, currentColor */
    while (next_tok(val, vn, &i, &t, &tn)) {
        int w, k, c;
        if ((k = kw_find(t, tn, KW_BSTYLE)) >= 0) { style = k <= 1 ? 0 : 1; }
        else if (border_width_tok_ax(t, tn, &w, 0)) { wx = w; border_width_tok_ax(t, tn, &wy, 1); }
        else if (parse_color(t, tn, &c)) color = c == COL_NONE ? BC_NONE_SET : (c == COL_CURRENT || c == COL_INHERIT ? BC_CUR : c);
        else if (eq_lit(t, tn, "0")) wx = wy = 0;
    }
    for (int k = 0; k < 4; k++) {
        if (!(sides & (1 << k))) continue;
        emit_decl(P_BWT + k, (k & 1) ? wx : wy, imp);        /* top/bottom: vertical, right/left: horizontal */
        emit_decl(P_BST + k, style, imp);
        emit_decl(P_BCT + k, color == BC_NONE_SET ? BC_CUR : color, imp);
    }
}

/* Up to four values -> top/right/bottom/left. Returns the count (0 on error). */
static int split4(const char *val, int vn, const char **t, int *tl) {
    int n = 0, i = 0, tn;
    const char *tk;
    while (n < 4 && next_tok(val, vn, &i, &tk, &tn)) { t[n] = tk; tl[n] = tn; n++; }
    if (n == 0) return 0;
    if (n == 1) { for (int k = 1; k < 4; k++) { t[k] = t[0]; tl[k] = tl[0]; } }
    else if (n == 2) { t[2] = t[0]; tl[2] = tl[0]; t[3] = t[1]; tl[3] = tl[1]; }
    else if (n == 3) { t[3] = t[1]; tl[3] = tl[1]; }
    return n;
}

/* ---- grid value parsers (bounded recursion) -------------------------------- */

/* A single grid track: 120px, 1fr, 25%, auto, minmax(a, b) (uses b; an fr b with
   a length a gets the zero-minimum flag GT_FR0), fit-content(x) (GT_FIT | px).
   Returns 0 for anything else. */
static int parse_grid_track(const char *s, int n, int16_t *out, int depth) {
    trim_tok(&s, &n);
    if (n <= 0 || depth > 3) return 0;
    if (eq_lit(s, n, "auto") || eq_lit(s, n, "min-content") || eq_lit(s, n, "max-content")) {
        *out = GT_AUTO; return 1;
    }
    if (starts_lit(s, n, "fit-content(")) {
        int px, pct, hp, end = n;
        if (s[end - 1] == ')') end--;
        if (parse_length(s + 12, end - 12, &px, &pct, &hp) && !hp) {
            if (px < 0) px = 0;
            *out = (int16_t)(GT_FIT | (px > 4000 ? 4000 : px));
        } else *out = GT_AUTO;
        return 1;
    }
    if (starts_lit(s, n, "minmax(")) {
        int open = 7, comma = -1, d = 0;
        for (int i = open; i < n; i++) {
            if (s[i] == '(') d++;
            else if (s[i] == ')') d--;
            else if (s[i] == ',' && d == 0) { comma = i; break; }
        }
        if (comma < 0) return 0;
        int end = n;
        if (s[end - 1] == ')') end--;
        if (!parse_grid_track(s + comma + 1, end - comma - 1, out, depth + 1)) return 0;
        if ((*out & GT_PCT) == GT_FR) {
            /* minmax(<length>, <fr>): the track may shrink below its content's min-content */
            const char *a = s + open;
            int an = comma - open;
            trim_tok(&a, &an);
            if (!eq_lit(a, an, "auto") && !eq_lit(a, an, "min-content") && !eq_lit(a, an, "max-content"))
                *out |= GT_FR0;
        }
        return 1;
    }
    if (n > 2 && ends_lit(s, n, "fr")) {
        int num = 0;
        if (!parse_number(s, n - 2, &num)) return 0;
        if (num < 1) num = 1;
        if (num > 4095) num = 4095;
        *out = (int16_t)(GT_FR | num);
        return 1;
    }
    int px, pct, hp;
    if (!parse_length(s, n, &px, &pct, &hp)) return 0;
    if (hp) { *out = (int16_t)(GT_PCT | (pct > 100 ? 100 : (pct < 0 ? 0 : pct))); return 1; }
    if (px < 0) px = 0;
    *out = (int16_t)(px > 4000 ? 4000 : px);
    return 1;
}

/* grid-template-columns: [n][autokind][automin][tracks...] stored in g_aux.
   Returns the aux index or -1. repeat(N, ...) expands up to GRID_MAX_TRACKS;
   repeat(auto-fill|auto-fit, minmax(Npx, 1fr) | Npx | 1fr) is an auto repeat
   resolved against the container width at layout time. */
static int parse_grid_template(const char *val, int vn) {
    int i = 0, tn, n = 0;
    const char *t;
    int16_t tr[GRID_MAX_TRACKS];
    int autokind = 0, automin = 0;
    while (next_tok(val, vn, &i, &t, &tn)) {
        if (starts_lit(t, tn, "repeat(")) {
            int comma = -1;
            for (int k = 7; k < tn; k++) if (t[k] == ',') { comma = k; break; }
            if (comma < 0) return -1;
            const char *cnt = t + 7;
            int cn = comma - 7;
            trim_tok(&cnt, &cn);
            const char *body = t + comma + 1;
            int bn = tn - comma - 1;
            if (bn > 0 && body[bn - 1] == ')') bn--;
            if (eq_lit(cnt, cn, "auto-fill") || eq_lit(cnt, cn, "auto-fit")) {
                int16_t one;
                const char *b2 = body;
                int b2n = bn;
                trim_tok(&b2, &b2n);
                int min = 0;
                if (starts_lit(b2, b2n, "minmax(")) {
                    int px, pct, hp;
                    int e = 7;
                    while (e < b2n && b2[e] != ',') e++;
                    if (parse_length(b2 + 7, e - 7, &px, &pct, &hp) && !hp) min = px;
                } else {
                    int px, pct, hp;
                    if (parse_length(b2, b2n, &px, &pct, &hp) && !hp && !ends_lit(b2, b2n, "fr")) min = px;
                }
                if (!parse_grid_track(b2, b2n, &one, 0)) return -1;
                autokind = eq_lit(cnt, cn, "auto-fill") ? 1 : 2;
                automin = min > 0 ? min : 1;
                n = 0;
                tr[n++] = one;
                break;
            }
            int count = 0;
            if (!parse_int_tok(cnt, cn, &count) || count < 1) return -1;
            int16_t one[GRID_MAX_TRACKS];
            int on = 0, j = 0, un;
            const char *ut;
            while (next_tok(body, bn, &j, &ut, &un) && on < GRID_MAX_TRACKS)
                if (parse_grid_track(ut, un, &one[on], 0)) on++;
            if (on == 0) return -1;
            for (int r = 0; r < count && n < GRID_MAX_TRACKS; r++)
                for (int k = 0; k < on && n < GRID_MAX_TRACKS; k++) tr[n++] = one[k];
            continue;
        }
        if (n >= GRID_MAX_TRACKS) break;
        if (!parse_grid_track(t, tn, &tr[n], 0)) return -1;
        n++;
    }
    if (n == 0) return -1;
    if (g_aux_n + 3 + n > MAX_AUX) return -1;
    int at = g_aux_n;
    g_aux[at] = (int16_t)n;
    g_aux[at + 1] = (int16_t)autokind;
    g_aux[at + 2] = (int16_t)automin;
    for (int k = 0; k < n; k++) g_aux[at + 3 + k] = tr[k];
    g_aux_n += 3 + n;
    return at;
}

/* One grid-column / grid-row line: "span N" or a signed line number. */
static void parse_grid_line(const char *s, int n, int *line, int *span) {
    trim_tok(&s, &n);
    *line = 0;
    *span = 0;
    if (starts_lit(s, n, "span")) {
        int v = 0;
        if (parse_int_tok(s + 4, n - 4, &v) && v >= 1) *span = v > 12 ? 12 : v;
        return;
    }
    int v = 0;
    if (parse_int_tok(s, n, &v)) *line = v;
}

/* "A / B", "A", "span N", "A / span N". */
static void parse_grid_area(const char *val, int vn, int16_t *start, int16_t *end, uint8_t *span) {
    int slash = -1;
    for (int i = 0; i < vn; i++) if (val[i] == '/') { slash = i; break; }
    int l1, s1, l2 = 0, s2 = 0;
    parse_grid_line(val, slash < 0 ? vn : slash, &l1, &s1);
    if (slash >= 0) parse_grid_line(val + slash + 1, vn - slash - 1, &l2, &s2);
    *start = (int16_t)l1;
    *end = 0;
    *span = 0;
    if (s1) *span = (uint8_t)s1;
    if (s2) *span = (uint8_t)s2;
    if (l2) *end = (int16_t)l2;
}

/* Area names are compared by a 16-bit hash (0 = no name). */
static int grid_name_hash(const char *s, int n) {
    unsigned h = 5381;
    for (int i = 0; i < n; i++) h = h * 33u + (unsigned char)s[i];
    h &= 0xFFFF;
    return h ? (int)h : 1;
}

/* grid-template-areas: a list of quoted rows of names ("." = empty cell).
   Stored in g_aux as [ncols][nrows][nareas] then per area
   [hash][col][row][cspan][rspan] (rectangles: min/max extent of the name).
   Returns the aux index or -1. Only call it with strings in v. */
#define GRID_MAX_AREAS 16
static int parse_grid_areas(const char *v, int vn) {
    int16_t ah[GRID_MAX_AREAS], ac0[GRID_MAX_AREAS], ar0[GRID_MAX_AREAS], ac1[GRID_MAX_AREAS], ar1[GRID_MAX_AREAS];
    int na = 0, row = 0, ncols = 0, i = 0, tn;
    const char *t;
    while (next_tok(v, vn, &i, &t, &tn)) {
        if (tn < 2 || (t[0] != '"' && t[0] != '\'')) continue;
        const char *r = t + 1;
        int rn = tn - 1;
        if (rn > 0 && (r[rn - 1] == '"' || r[rn - 1] == '\'')) rn--;
        int j = 0, col = 0, cn;
        const char *c;
        while (next_tok(r, rn, &j, &c, &cn)) {
            if (!(cn == 1 && c[0] == '.')) {
                int h = grid_name_hash(c, cn), k;
                for (k = 0; k < na && (uint16_t)ah[k] != (unsigned)h; k++) {}
                if (k == na && na < GRID_MAX_AREAS) {
                    ah[na] = (int16_t)h; ac0[na] = (int16_t)col; ar0[na] = (int16_t)row;
                    ac1[na] = (int16_t)col; ar1[na] = (int16_t)row; na++;
                } else if (k < na) {
                    if (col > ac1[k]) ac1[k] = (int16_t)col;
                    if (row > ar1[k]) ar1[k] = (int16_t)row;
                }
            }
            col++;
        }
        if (col > ncols) ncols = col;
        row++;
    }
    if (row == 0 || ncols == 0) return -1;
    if (g_aux_n + 3 + na * 5 > MAX_AUX) return -1;
    int at = g_aux_n;
    g_aux[at] = (int16_t)(ncols > GRID_MAX_TRACKS ? GRID_MAX_TRACKS : ncols);
    g_aux[at + 1] = (int16_t)row;
    g_aux[at + 2] = (int16_t)na;
    for (int k = 0; k < na; k++) {
        int16_t *o = &g_aux[at + 3 + k * 5];
        o[0] = ah[k]; o[1] = ac0[k]; o[2] = ar0[k];
        o[3] = (int16_t)(ac1[k] - ac0[k] + 1); o[4] = (int16_t)(ar1[k] - ar0[k] + 1);
    }
    g_aux_n += 3 + na * 5;
    return at;
}

/* ---- flex shorthand -------------------------------------------------------- */

static int parse_basis_tok(const char *s, int n, int16_t *out) {
    trim_tok(&s, &n);
    int px, pct, hp;
    if (!parse_length(s, n, &px, &pct, &hp)) return 0;
    if (hp) { *out = (int16_t)(-(pct < 0 ? 0 : pct > 100 ? 100 : pct)); return 1; }
    if (px < 0) px = 0;
    *out = (int16_t)(px > 30000 ? 30000 : px);
    return 1;
}

static void emit_flex(const char *val, int vn, int imp) {
    const char *t[3];
    int tl[3];
    int n = 0, i = 0, tn;
    const char *tk;
    while (n < 3 && next_tok(val, vn, &i, &tk, &tn)) { t[n] = tk; tl[n] = tn; n++; }
    if (n == 0) return;
    if (n == 1) {
        if (eq_lit(t[0], tl[0], "none")) { emit_decl(P_GROW, 0, imp); emit_decl(P_SHRINK, 0, imp); emit_decl(P_BASIS, -32768, imp); return; }
        if (eq_lit(t[0], tl[0], "auto")) { emit_decl(P_GROW, FLEX_SCALE, imp); emit_decl(P_SHRINK, FLEX_SCALE, imp); emit_decl(P_BASIS, -32768, imp); return; }
        if (eq_lit(t[0], tl[0], "initial")) { emit_decl(P_GROW, 0, imp); emit_decl(P_SHRINK, FLEX_SCALE, imp); emit_decl(P_BASIS, -32768, imp); return; }
        int num = 0;
        if (parse_number(t[0], tl[0], &num)) {
            emit_decl(P_GROW, num, imp); emit_decl(P_SHRINK, FLEX_SCALE, imp); emit_decl(P_BASIS, 0, imp);
            return;
        }
        int16_t b;
        if (parse_basis_tok(t[0], tl[0], &b)) {
            emit_decl(P_GROW, FLEX_SCALE, imp); emit_decl(P_SHRINK, FLEX_SCALE, imp); emit_decl(P_BASIS, b, imp);
        }
        return;
    }
    int num = 0;
    if (!parse_number(t[0], tl[0], &num)) return;
    int shrink = FLEX_SCALE, basis = 0;
    int idx = 1;
    int sn = 0;
    if (parse_number(t[idx], tl[idx], &sn)) { shrink = sn; idx++; }
    if (idx < n) {
        if (eq_lit(t[idx], tl[idx], "auto")) basis = -32768;
        else { int16_t b; if (parse_basis_tok(t[idx], tl[idx], &b)) basis = b; }
    }
    emit_decl(P_GROW, num, imp);
    emit_decl(P_SHRINK, shrink, imp);
    emit_decl(P_BASIS, basis, imp);
}

/* ---- font ------------------------------------------------------------------ */

/* A negative font-size value is a multiple of the parent's font-size in tenths of
   a percent (em, %, smaller/larger); g_fs_rem is the rem part (thousandths) of a
   length written in rem, whose px value then assumes a 16px root. */
static int g_fs_rem;

static int font_size_val(const char *v, int vn, int32_t *out) {
    static const char *const kws[] = { "xx-small", "x-small", "small", "medium", "large", "x-large",
        "xx-large", "xxx-large", "smaller", "larger", 0 };
    static const int16_t vals[] = { 9, 10, 13, 16, 18, 24, 32, 48, -833, -1200 };
    g_fs_rem = 0;
    trim_tok(&v, &vn);
    int k = kw_find(v, vn, kws);
    if (k >= 0) { *out = vals[k]; return 1; }
    Dim d;
    int c = parse_dim(v, vn, &d);
    if (!c || c != vn) {
        if (starts_lit(v, vn, "calc(") || starts_lit(v, vn, "min(") || starts_lit(v, vn, "max(") ||
            starts_lit(v, vn, "clamp(")) {
            int px, pct, hp;
            if (parse_length(v, vn, &px, &pct, &hp)) {
                if (hp) { *out = -(pct < 1 ? 1 : (pct > 10000 ? 10000 : pct)) * 10; return 1; }
                *out = px;
                if (*out < 1) *out = 1;
                return 1;
            }
        }
        return 0;
    }
    if (d.unit == U_PCT) { int p = d.mil / 100; *out = -(p < 1 ? 1 : (p > 100000 ? 100000 : p)); return 1; }
    if (d.unit == U_EM) { int p = d.mil; *out = -(p < 1 ? 1 : (p > 100000 ? 100000 : p)); return 1; }
    if (d.unit == U_REM && d.mil >= 1 && d.mil <= 32000) g_fs_rem = d.mil;
    if (d.unit == U_NONE && d.mil != 0) return 0;
    int css = dim_css_mil(&d) / 1000;
    if (d.unit == U_VW || d.unit == U_VH || d.unit == U_VMIN || d.unit == U_VMAX)
        css = g_web ? dim_css_mil(&d) / 1000 : dim_px(&d);
    if (css < 1) css = 1;
    if (css > 400) css = 400;
    *out = css;
    return 1;
}

static int lineh_val(const char *t, int tn, int32_t *out) {
    trim_tok(&t, &tn);
    if (eq_lit(t, tn, "normal")) { *out = 0; return 1; }
    Dim d;
    int c = parse_dim(t, tn, &d);
    if (c && c == tn) {
        if (d.unit == U_NONE) { int p = d.mil / 10; if (p < 1) p = 1; if (p > 800) p = 800; *out = -p; return 1; }
        if (d.unit == U_PCT) { int p = d.mil / 1000; if (p < 1) p = 1; if (p > 800) p = 800; *out = -p; return 1; }
        if (d.unit == U_EM || d.unit == U_REM) { int p = d.mil / 10; if (p < 1) p = 1; if (p > 800) p = 800; *out = -p; return 1; }
    }
    int px, pct, hp;
    g_axis = 1;
    int ok = parse_length(t, tn, &px, &pct, &hp) && !hp;
    g_axis = 0;
    if (ok) { if (px < 1) px = 1; if (px > 200) px = 200; *out = px; return 1; }
    return 0;
}

static int weight_val(const char *v, int vn) {
    int num = 0;
    if (eq_lit(v, vn, "bold") || eq_lit(v, vn, "bolder")) return TW_BOLD;
    if (eq_lit(v, vn, "normal") || eq_lit(v, vn, "lighter")) return TW_NORMAL;
    if (parse_int_tok(v, vn, &num)) return num >= 600 ? TW_BOLD : TW_NORMAL;
    return 0;
}

static void emit_font(const char *val, int vn, int imp) {
    int i = 0, tn;
    const char *t;
    int got_size = 0;
    while (next_tok(val, vn, &i, &t, &tn)) {
        int32_t sv;
        int w;
        if (eq_lit(t, tn, "normal") || eq_lit(t, tn, "italic") || eq_lit(t, tn, "oblique") ||
            eq_lit(t, tn, "small-caps")) continue;
        if ((w = weight_val(t, tn)) && !got_size) { emit_decl(P_WEIGHT, w, imp); continue; }
        /* size[/line-height] */
        int slash = -1;
        for (int k = 0; k < tn; k++) if (t[k] == '/') { slash = k; break; }
        int sn = slash >= 0 ? slash : tn;
        if (!got_size && font_size_val(t, sn, &sv)) {
            emit_decl_e(P_FSIZE, sv, imp, 0, g_fs_rem);
            got_size = 1;
            if (slash >= 0) {
                int32_t lv;
                if (lineh_val(t + slash + 1, tn - slash - 1, &lv)) emit_decl(P_LINEH, lv, imp);
            } else {
                /* "12px / 1.5": the slash can be its own token */
                int j2 = i, tn2;
                const char *t2;
                if (next_tok(val, vn, &j2, &t2, &tn2) && tn2 == 1 && t2[0] == '/') {
                    int j3 = j2, tn3;
                    const char *t3;
                    if (next_tok(val, vn, &j3, &t3, &tn3)) {
                        int32_t lv;
                        if (lineh_val(t3, tn3, &lv)) emit_decl(P_LINEH, lv, imp);
                        i = j3;
                    }
                }
            }
            break;     /* the family list follows */
        }
    }
}

/* ---- content (for ::before / ::after) -------------------------------------- */

static int intern(const char *s, int n, int add);

static int unicode_to_glyph(int cp);

/* content: "text" -> interned string id + 1; none/normal -> 0; anything else
   (counters, attr(), url()) -> -1 (no content). */
static int parse_content(const char *v, int vn) {
    trim_tok(&v, &vn);
    if (eq_lit(v, vn, "none") || eq_lit(v, vn, "normal")) return 0;
    if (vn >= 2 && (v[0] == '"' || v[0] == '\'')) {
        char q = v[0];
        char buf[64];
        int w = 0, i = 1;
        while (i < vn && v[i] != q && w < (int)sizeof(buf) - 1) {
            if (v[i] == '\\' && i + 1 < vn) {
                i++;
                int cp = 0, digits = 0;
                while (i < vn && digits < 6 && hex_val(v[i]) >= 0) { cp = cp * 16 + hex_val(v[i]); i++; digits++; }
                if (digits) {
                    if (i < vn && v[i] == ' ') i++;
                    buf[w++] = (char)unicode_to_glyph(cp);
                    continue;
                }
                buf[w++] = v[i++];
                continue;
            }
            unsigned char c = (unsigned char)v[i];
            if (c >= 0x80) {     /* UTF-8 literal */
                int cp = 0, len = 1;
                if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
                else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
                else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
                for (int k = 1; k < len && i + k < vn; k++) cp = (cp << 6) | (v[i + k] & 0x3F);
                i += len;
                buf[w++] = (char)unicode_to_glyph(cp);
                continue;
            }
            buf[w++] = v[i++];
        }
        int id = intern(buf, w, 1);
        return id < 0 ? -1 : id + 1;
    }
    return -1;
}

/* ---- the property table ----------------------------------------------------- */

enum { F_COLOR = 1, F_OFF, F_OFFA, F_SIZE, F_ENUM, F_NUM, F_SPECIAL, F_IGNORE };

typedef struct { const char *name; uint8_t form; uint8_t prop; } PropDef;

enum {
    S_MARGIN = 1, S_PADDING, S_BORDER, S_BORDER_T, S_BORDER_R, S_BORDER_B, S_BORDER_L, S_BORDER_W,
    S_BORDER_C, S_BORDER_S, S_BACKGROUND, S_FONT, S_FLEX, S_FLEXFLOW, S_GAP, S_INSET, S_OVERFLOW,
    S_LISTSTYLE, S_DECOR, S_PLACEITEMS, S_RADIUS, S_FSIZE, S_LINEH, S_WEIGHT, S_LETSP, S_GCOLS, S_GAREAS, S_GAREA, S_GTEMPLATE,
    S_GCOL, S_GROW, S_ZINDEX, S_OPACITY, S_FLOATV, S_CONTENT, S_BORDERSPACING, S_BORDERCOLLAPSE,
    S_BORDERW_T, S_BORDERW_R, S_BORDERW_B, S_BORDERW_L, S_BORDERC_T, S_BORDERC_R, S_BORDERC_B,
    S_BORDERC_L, S_BORDERS_T, S_BORDERS_R, S_BORDERS_B, S_BORDERS_L, S_INDENT, S_TEXTALIGN,
    S_JITEMS, S_JSELF, S_PLACECONTENT, S_BOXSZ, S_ORDER, S_GROWNUM, S_SHRINKNUM, S_BASIS, S_FLEXDIR,
    S_FLEXWRAP, S_ROWGAP, S_COLGAP, S_TRANSFORM, S_WS, S_BGCOLOR, S_ITALIC, S_VALIGN, S_LSTYPE,
    S_CLEAR, S_CURSOR, S_VISIBILITY, S_POSITION, S_DISPLAY, S_TRANSFORMFN,
    S_MARGIN_IN, S_MARGIN_BL, S_PADDING_IN, S_PADDING_BL, S_INSET_IN, S_INSET_BL,
    /* logical border shorthands that expand to a pair of physical sides (horizontal-tb, ltr only) */
    S_L_BORDER_I, S_L_BORDER_B, S_L_BORDERW_I, S_L_BORDERW_B, S_L_BORDERC_I, S_L_BORDERC_B,
    S_L_BORDERS_I, S_L_BORDERS_B
};

static const PropDef g_props[] = {
    {"color", F_COLOR, P_COLOR},
    {"background-color", F_SPECIAL, S_BGCOLOR},
    {"background", F_SPECIAL, S_BACKGROUND},
    {"font-size", F_SPECIAL, S_FSIZE},
    {"font", F_SPECIAL, S_FONT},
    {"font-weight", F_SPECIAL, S_WEIGHT},
    {"font-style", F_SPECIAL, S_ITALIC},
    {"display", F_SPECIAL, S_DISPLAY},
    {"text-align", F_SPECIAL, S_TEXTALIGN},
    {"margin", F_SPECIAL, S_MARGIN},
    {"margin-top", F_OFFA, P_MT}, {"margin-right", F_OFFA, P_MR},
    {"margin-bottom", F_OFFA, P_MB}, {"margin-left", F_OFFA, P_ML},
    /* logical properties, horizontal-tb ltr: the same cascade slot as the physical side */
    {"margin-inline-start", F_OFFA, P_ML}, {"margin-inline-end", F_OFFA, P_MR},
    {"margin-block-start", F_OFFA, P_MT}, {"margin-block-end", F_OFFA, P_MB},
    {"margin-inline", F_SPECIAL, S_MARGIN_IN}, {"margin-block", F_SPECIAL, S_MARGIN_BL},
    {"padding-inline-start", F_OFF, P_PL}, {"padding-inline-end", F_OFF, P_PR},
    {"padding-block-start", F_OFF, P_PT}, {"padding-block-end", F_OFF, P_PB},
    {"padding-inline", F_SPECIAL, S_PADDING_IN}, {"padding-block", F_SPECIAL, S_PADDING_BL},
    {"inset-inline-start", F_OFFA, P_LEFT}, {"inset-inline-end", F_OFFA, P_RIGHT},
    {"inset-block-start", F_OFFA, P_TOP}, {"inset-block-end", F_OFFA, P_BOTTOM},
    {"inset-inline", F_SPECIAL, S_INSET_IN}, {"inset-block", F_SPECIAL, S_INSET_BL},
    {"inline-size", F_SIZE, P_WIDTH}, {"block-size", F_SIZE, P_HEIGHT},
    {"min-inline-size", F_SIZE, P_MINW}, {"max-inline-size", F_SIZE, P_MAXW},
    {"min-block-size", F_SIZE, P_MINH}, {"max-block-size", F_SIZE, P_MAXH},
    {"padding", F_SPECIAL, S_PADDING},
    {"padding-top", F_OFF, P_PT}, {"padding-right", F_OFF, P_PR},
    {"padding-bottom", F_OFF, P_PB}, {"padding-left", F_OFF, P_PL},
    {"width", F_SIZE, P_WIDTH}, {"height", F_SIZE, P_HEIGHT},
    {"min-width", F_SIZE, P_MINW}, {"max-width", F_SIZE, P_MAXW},
    {"min-height", F_SIZE, P_MINH}, {"max-height", F_SIZE, P_MAXH},
    {"flex-direction", F_SPECIAL, S_FLEXDIR}, {"flex-wrap", F_SPECIAL, S_FLEXWRAP},
    {"flex-flow", F_SPECIAL, S_FLEXFLOW},
    {"justify-content", F_ENUM, P_JUSTIFY}, {"align-items", F_ENUM, P_ALIGNITEMS},
    {"align-self", F_ENUM, P_ALIGNSELF}, {"align-content", F_ENUM, P_ALIGNCONTENT},
    {"gap", F_SPECIAL, S_GAP}, {"grid-gap", F_SPECIAL, S_GAP},
    {"row-gap", F_SPECIAL, S_ROWGAP}, {"column-gap", F_SPECIAL, S_COLGAP},
    {"grid-row-gap", F_SPECIAL, S_ROWGAP}, {"grid-column-gap", F_SPECIAL, S_COLGAP},
    {"flex-grow", F_SPECIAL, S_GROWNUM}, {"flex-shrink", F_SPECIAL, S_SHRINKNUM},
    {"flex-basis", F_SPECIAL, S_BASIS}, {"flex", F_SPECIAL, S_FLEX},
    {"order", F_SPECIAL, S_ORDER},
    {"position", F_SPECIAL, S_POSITION},
    {"top", F_OFFA, P_TOP}, {"right", F_OFFA, P_RIGHT},
    {"bottom", F_OFFA, P_BOTTOM}, {"left", F_OFFA, P_LEFT},
    {"inset", F_SPECIAL, S_INSET},
    {"z-index", F_SPECIAL, S_ZINDEX},
    {"overflow", F_SPECIAL, S_OVERFLOW}, {"overflow-x", F_SPECIAL, S_OVERFLOW}, {"overflow-y", F_SPECIAL, S_OVERFLOW},
    {"visibility", F_SPECIAL, S_VISIBILITY},
    {"opacity", F_SPECIAL, S_OPACITY},
    {"cursor", F_SPECIAL, S_CURSOR},
    {"box-sizing", F_SPECIAL, S_BOXSZ},
    {"border", F_SPECIAL, S_BORDER},
    {"border-top", F_SPECIAL, S_BORDER_T}, {"border-right", F_SPECIAL, S_BORDER_R},
    {"border-bottom", F_SPECIAL, S_BORDER_B}, {"border-left", F_SPECIAL, S_BORDER_L},
    {"border-width", F_SPECIAL, S_BORDER_W}, {"border-color", F_SPECIAL, S_BORDER_C},
    {"border-style", F_SPECIAL, S_BORDER_S},
    {"border-top-width", F_SPECIAL, S_BORDERW_T}, {"border-right-width", F_SPECIAL, S_BORDERW_R},
    {"border-bottom-width", F_SPECIAL, S_BORDERW_B}, {"border-left-width", F_SPECIAL, S_BORDERW_L},
    {"border-top-color", F_SPECIAL, S_BORDERC_T}, {"border-right-color", F_SPECIAL, S_BORDERC_R},
    {"border-bottom-color", F_SPECIAL, S_BORDERC_B}, {"border-left-color", F_SPECIAL, S_BORDERC_L},
    {"border-top-style", F_SPECIAL, S_BORDERS_T}, {"border-right-style", F_SPECIAL, S_BORDERS_R},
    {"border-bottom-style", F_SPECIAL, S_BORDERS_B}, {"border-left-style", F_SPECIAL, S_BORDERS_L},
    {"border-radius", F_SPECIAL, S_RADIUS},
    {"border-spacing", F_SPECIAL, S_BORDERSPACING}, {"border-collapse", F_SPECIAL, S_BORDERCOLLAPSE},
    {"text-decoration", F_SPECIAL, S_DECOR}, {"text-decoration-line", F_SPECIAL, S_DECOR},
    {"text-transform", F_SPECIAL, S_TRANSFORM},
    {"transform", F_SPECIAL, S_TRANSFORMFN},
    {"translate", F_SPECIAL, S_TRANSFORMFN},
    {"white-space", F_SPECIAL, S_WS},
    {"line-height", F_SPECIAL, S_LINEH},
    {"letter-spacing", F_SPECIAL, S_LETSP},
    {"text-indent", F_SPECIAL, S_INDENT},
    {"vertical-align", F_SPECIAL, S_VALIGN},
    {"float", F_SPECIAL, S_FLOATV}, {"clear", F_SPECIAL, S_CLEAR},
    {"list-style", F_SPECIAL, S_LISTSTYLE}, {"list-style-type", F_SPECIAL, S_LSTYPE},
    {"content", F_SPECIAL, S_CONTENT},
    {"grid-template-columns", F_SPECIAL, S_GCOLS},
    {"grid-template-areas", F_SPECIAL, S_GAREAS}, {"grid-area", F_SPECIAL, S_GAREA},
    {"grid-template", F_SPECIAL, S_GTEMPLATE},
    {"grid-column", F_SPECIAL, S_GCOL}, {"grid-row", F_SPECIAL, S_GROW},
    /* logical borders (margin/padding/inset/size ones are above) */
    {"border-inline-start", F_SPECIAL, S_BORDER_L}, {"border-inline-end", F_SPECIAL, S_BORDER_R},
    {"border-block-start", F_SPECIAL, S_BORDER_T}, {"border-block-end", F_SPECIAL, S_BORDER_B},
    {"border-inline", F_SPECIAL, S_L_BORDER_I}, {"border-block", F_SPECIAL, S_L_BORDER_B},
    {"border-inline-start-width", F_SPECIAL, S_BORDERW_L}, {"border-inline-end-width", F_SPECIAL, S_BORDERW_R},
    {"border-block-start-width", F_SPECIAL, S_BORDERW_T}, {"border-block-end-width", F_SPECIAL, S_BORDERW_B},
    {"border-inline-start-color", F_SPECIAL, S_BORDERC_L}, {"border-inline-end-color", F_SPECIAL, S_BORDERC_R},
    {"border-block-start-color", F_SPECIAL, S_BORDERC_T}, {"border-block-end-color", F_SPECIAL, S_BORDERC_B},
    {"border-inline-start-style", F_SPECIAL, S_BORDERS_L}, {"border-inline-end-style", F_SPECIAL, S_BORDERS_R},
    {"border-block-start-style", F_SPECIAL, S_BORDERS_T}, {"border-block-end-style", F_SPECIAL, S_BORDERS_B},
    {"border-inline-width", F_SPECIAL, S_L_BORDERW_I}, {"border-block-width", F_SPECIAL, S_L_BORDERW_B},
    {"border-inline-color", F_SPECIAL, S_L_BORDERC_I}, {"border-block-color", F_SPECIAL, S_L_BORDERC_B},
    {"border-inline-style", F_SPECIAL, S_L_BORDERS_I}, {"border-block-style", F_SPECIAL, S_L_BORDERS_B},
    {"justify-self", F_SPECIAL, S_JSELF}, {"justify-items", F_SPECIAL, S_JITEMS}, {"place-items", F_SPECIAL, S_PLACEITEMS},
    {"place-content", F_SPECIAL, S_PLACECONTENT},
    {0, 0, 0}
};

static int g_var_capture;       /* a :root, html, body or star rule is being parsed: --vars are kept */

/* Custom properties. Names and values live in the intern buffer. */
#define MAX_VARS LIM(1536, 20000)
typedef struct { int name; uint32_t voff; uint16_t vlen; } VarEnt;
static VarEnt g_vars[MAX_VARS];
static int g_var_n;
static int g_var_ua_n;

/* --x: var(--x, fb) is a cycle: it computes to the guaranteed-invalid value, so a var(--x, ...) that reads
   it gets its own fallback. Rewrite direct self references to their fallback text; -1 = nothing left (invalid). */
static int var_self_strip(const char *name, int nn, const char *val, int vn, char *out, int cap) {
    int w = 0, i = 0;
    while (i < vn) {
        if (i + 4 < vn && val[i] == 'v' && val[i + 1] == 'a' && val[i + 2] == 'r' && val[i + 3] == '(') {
            int j = i + 4, d = 1, comma = -1;
            while (j < vn && d > 0) {
                if (val[j] == '(') d++;
                else if (val[j] == ')') { d--; if (d == 0) break; }
                else if (val[j] == ',' && d == 1 && comma < 0) comma = j;
                j++;
            }
            const char *nm = val + i + 4;
            int nl = (comma >= 0 ? comma : j) - (i + 4);
            trim_tok(&nm, &nl);
            if (nl == nn && memcmp(nm, name, (size_t)nn) == 0) {
                if (comma < 0) return -1;
                const char *fb = val + comma + 1;
                int fl = j - comma - 1;
                trim_tok(&fb, &fl);
                for (int k = 0; k < fl && w < cap - 1; k++) out[w++] = fb[k];
                i = j < vn ? j + 1 : vn;
                continue;
            }
        }
        if (w < cap - 1) out[w++] = val[i];
        i++;
    }
    out[w] = 0;
    return w;
}

static void var_set(const char *name, int nn, const char *val, int vn, int scoped) {
    char selfbuf[512];
    if (vn >= 5 && vn < (int)sizeof(selfbuf)) {
        int has = 0;
        for (int i = 0; i + 4 < vn && !has; i++)
            if (val[i] == 'v' && val[i + 1] == 'a' && val[i + 2] == 'r' && val[i + 3] == '(') {
                const char *nm = val + i + 4;
                int rest = vn - (i + 4);
                int k = 0;
                while (k < rest && nm[k] == ' ') k++;
                if (rest - k >= nn && memcmp(nm + k, name, (size_t)nn) == 0) has = 1;
            }
        if (has) {
            int m = var_self_strip(name, nn, val, vn, selfbuf, (int)sizeof(selfbuf));
            if (m <= 0) return;
            val = selfbuf; vn = m;
        }
    }
    int id = intern(name, nn, 1);
    if (id < 0) return;
    int vid = intern(val, vn, 1);
    if (vid < 0) return;
    for (int i = 0; i < g_var_n; i++) {
        if (g_vars[i].name == id) {
            if (!scoped) { g_vars[i].voff = (uint32_t)vid; g_vars[i].vlen = (uint16_t)vn; }
            return;
        }
    }
    if (g_var_n >= MAX_VARS) { SET_OOM(); return; }      /* was silent: later var() references just failed */
    g_vars[g_var_n].name = id;
    g_vars[g_var_n].voff = (uint32_t)vid;      /* intern id of the value text */
    g_vars[g_var_n].vlen = (uint16_t)vn;
    g_var_n++;
}

static const char *intern_str(int id, int *len);

static int var_get(const char *name, int nn, const char **v, int *vn) {
    int id = intern(name, nn, 0);
    if (id < 0) return 0;
    for (int i = 0; i < g_var_n; i++) {
        if (g_vars[i].name == id) { *v = intern_str((int)g_vars[i].voff, vn); return 1; }
    }
    return 0;
}

/* Replace var(--x[, fallback]) in val with the stored text. Returns the new
   length, written to out (cap bytes), or -1 if a var is unresolved with no
   fallback. */
static int g_var_steps;       /* var() expansions left for the declaration being parsed */

static int var_subst(const char *val, int vn, char *out, int cap, int depth) {
    int w = 0, i = 0;
    while (i < vn) {
        if (i + 4 < vn && val[i] == 'v' && val[i + 1] == 'a' && val[i + 2] == 'r' && val[i + 3] == '(') {
            int j = i + 4, d = 1, comma = -1;
            while (j < vn && d > 0) {
                if (val[j] == '(') d++;
                else if (val[j] == ')') { d--; if (d == 0) break; }
                else if (val[j] == ',' && d == 1 && comma < 0) comma = j;
                j++;
            }
            int ne = comma >= 0 ? comma : j;
            const char *nm = val + i + 4;
            int nl = ne - (i + 4);
            trim_tok(&nm, &nl);
            const char *rv;
            int rl = 0;
            int ok = 0;
            char tmp[256];
            if (--g_var_steps < 0) return -1;          /* a var() bomb: give the declaration up */
            if (var_get(nm, nl, &rv, &rl)) {
                if (depth < 6 && rl < (int)sizeof(tmp)) {
                    int m = var_subst(rv, rl, tmp, (int)sizeof(tmp), depth + 1);
                    if (m >= 0) { rv = tmp; rl = m; ok = 1; }
                } else ok = 1;
            }
            if (!ok && comma >= 0) {
                const char *fb = val + comma + 1;
                int fl = j - comma - 1;
                trim_tok(&fb, &fl);
                int m = depth < 6 ? var_subst(fb, fl, tmp, (int)sizeof(tmp), depth + 1) : -1;
                if (m >= 0) { rv = tmp; rl = m; ok = 1; }
            }
            if (!ok) return -1;
            for (int k = 0; k < rl && w < cap - 1; k++) out[w++] = rv[k];
            i = j < vn ? j + 1 : vn;
            continue;
        }
        if (w < cap - 1) out[w++] = val[i];
        i++;
    }
    out[w] = 0;
    return w;
}

/* ---- apply one declaration ---------------------------------------------------- */

static const PropDef *prop_by_name(const char *name) {
    for (const PropDef *pd = g_props; pd->name; pd++) {
        int i = 0;
        while (name[i] && name[i] == pd->name[i]) i++;
        if (!name[i] && !pd->name[i]) return pd;
    }
    return 0;
}

static void decl_prop(const PropDef *pd, const char *val, int vn, int imp);

/* border-inline-width: <start> [<end>] and friends: one or two values, applied to two physical sides.
   whole != 0 (border-inline: <width> <style> <color>): the full value goes to both sides. */
static void decl_pair(const char *na, const char *nb, const char *v, int n, int imp, int whole) {
    const PropDef *a = prop_by_name(na), *b = prop_by_name(nb);
    if (!a || !b) return;
    if (whole) { decl_prop(a, v, n, imp); decl_prop(b, v, n, imp); return; }
    int i = 0, tn, cnt = 0;
    const char *t, *t0 = v, *t1 = v;
    int l0 = n, l1 = n;
    while (next_tok(v, n, &i, &t, &tn)) {
        if (cnt == 0) { t0 = t; l0 = tn; t1 = t; l1 = tn; }
        else if (cnt == 1) { t1 = t; l1 = tn; }
        else return;                        /* three or more values: invalid */
        cnt++;
    }
    if (!cnt) return;
    decl_prop(a, t0, l0, imp);
    decl_prop(b, t1, l1, imp);
}

static void decl_prop(const PropDef *pd, const char *val, int vn, int imp) {
    const char *v = val;
    int n = vn;
    trim_tok(&v, &n);
    if (n <= 0) return;
    int32_t x;
    int k, c;
    /* CSS-wide keywords */
    if (eq_lit(v, n, "inherit")) {
        if (pd->form == F_COLOR || (pd->form == F_SPECIAL && (pd->prop == S_FSIZE || pd->prop == S_BGCOLOR)))
            ;                       /* handled below as a colour/size keyword */
        else if (pd->form != F_SPECIAL) { emit_inherit(pd->prop, imp); return; }
    }
    if (eq_lit(v, n, "initial") || eq_lit(v, n, "unset") || eq_lit(v, n, "revert") ||
        eq_lit(v, n, "revert-layer")) {
        /* initial values: a handful matter in practice */
        if (pd->form == F_SPECIAL) {
            switch (pd->prop) {
            case S_DISPLAY: emit_decl(P_DISPLAY, DISP_INLINE, imp); break;
            case S_BGCOLOR: case S_BACKGROUND: emit_decl(P_BG, COL_NONE, imp); break;
            case S_FLOATV: emit_decl(P_FLOAT, 0, imp); break;
            case S_POSITION: emit_decl(P_POS, POS_STATIC, imp); break;
            case S_OVERFLOW: emit_decl(P_OVERFLOW, OVF_VISIBLE, imp); break;
            case S_MARGIN: for (k = 0; k < 4; k++) emit_decl(P_MT + k, 0, imp); break;
            case S_PADDING: for (k = 0; k < 4; k++) emit_decl(P_PT + k, 0, imp); break;
            default: break;
            }
        } else if (pd->form == F_SIZE) {
            emit_decl(pd->prop, (pd->prop == P_WIDTH || pd->prop == P_HEIGHT) ? -32768 : 0, imp);
        } else if (pd->form == F_OFFA) {
            emit_decl(pd->prop, (pd->prop >= P_TOP && pd->prop <= P_LEFT) ? OFF_AUTO : 0, imp);
        } else if (pd->form == F_OFF) {
            emit_decl(pd->prop, 0, imp);
        }
        return;
    }
    g_axis = 0;
    if (pd->form == F_OFF || pd->form == F_OFFA || pd->form == F_SIZE) {
        switch (pd->prop) {
        case P_MT: case P_MB: case P_PT: case P_PB: case P_TOP: case P_BOTTOM:
        case P_HEIGHT: case P_MINH: case P_MAXH:
            g_axis = 1;
            break;
        default: break;
        }
    }
    switch (pd->form) {
    case F_COLOR:
        if (parse_color(v, n, &c)) {
            if (c == COL_INHERIT) emit_inherit(P_COLOR, imp);
            else if (c >= 0) emit_decl(P_COLOR, c, imp);
        }
        return;
    case F_OFF:
        if (parse_off_tok(v, n, 0, &x) && x >= 0) emit_decl_e(pd->prop, x, imp, g_tok_em, g_tok_rem);
        return;
    case F_OFFA:
        if (parse_off_tok(v, n, 1, &x)) emit_decl_e(pd->prop, x, imp, g_tok_em, g_tok_rem);
        return;
    case F_SIZE:
        if (parse_size_tok(v, n, pd->prop != P_WIDTH && pd->prop != P_HEIGHT, &x)) {
            /* min-width: 0 is not min-width: auto (a flex item may shrink below its content);
               both store 0, so an explicit length zero is passed as MINW_ZERO */
            if (pd->prop == P_MINW && x == 0 && ((v[0] >= '0' && v[0] <= '9') || v[0] == '.' || v[0] == '+' || v[0] == '-'))
                x = MINW_ZERO;
            emit_decl_e(pd->prop, x, imp, g_tok_em, g_tok_rem);
        }
        return;
    case F_ENUM:
        if (pd->prop == P_JUSTIFY) emit_kw(P_JUSTIFY, v, n, KW_JUSTIFY, KW_JUSTIFY_V, imp);
        else if (pd->prop == P_ALIGNITEMS) emit_kw(P_ALIGNITEMS, v, n, KW_ALIGN, KW_ALIGN_V, imp);
        else if (pd->prop == P_ALIGNSELF) {
            if (eq_lit(v, n, "auto")) emit_decl(P_ALIGNSELF, ALIGN_AUTO, imp);
            else emit_kw(P_ALIGNSELF, v, n, KW_ALIGN, KW_ALIGN_V, imp);
        }
        else if (pd->prop == P_ALIGNCONTENT) emit_kw(P_ALIGNCONTENT, v, n, KW_ACNT, KW_ACNT_V, imp);
        return;
    default:
        break;
    }
    /* shorthands and the rest */
    switch (pd->prop) {
    case S_L_BORDER_I: decl_pair("border-left", "border-right", v, n, imp, 1); return;
    case S_L_BORDER_B: decl_pair("border-top", "border-bottom", v, n, imp, 1); return;
    case S_L_BORDERW_I: decl_pair("border-left-width", "border-right-width", v, n, imp, 0); return;
    case S_L_BORDERW_B: decl_pair("border-top-width", "border-bottom-width", v, n, imp, 0); return;
    case S_L_BORDERC_I: decl_pair("border-left-color", "border-right-color", v, n, imp, 0); return;
    case S_L_BORDERC_B: decl_pair("border-top-color", "border-bottom-color", v, n, imp, 0); return;
    case S_L_BORDERS_I: decl_pair("border-left-style", "border-right-style", v, n, imp, 0); return;
    case S_L_BORDERS_B: decl_pair("border-top-style", "border-bottom-style", v, n, imp, 0); return;
    case S_DISPLAY: {
        /* two-value syntax ("block flow", "inline flex"): take the last */
        int i = 0, tn;
        const char *t, *last = v;
        int lastn = n;
        while (next_tok(v, n, &i, &t, &tn)) { last = t; lastn = tn; }
        int found = -1;
        if (eq_lit(v, n, "inline flow-root")) found = DISP_INLINE_BLOCK;
        else if ((k = kw_find(last, lastn, KW_DISPLAY)) >= 0) found = KW_DISPLAY_V[k];
        if (eq_lit(v, n, "inline flex")) found = DISP_INLINE_FLEX;
        if (eq_lit(v, n, "block flex")) found = DISP_FLEX;
        if (found >= 0) emit_decl(P_DISPLAY, found, imp);
        return;
    }
    case S_TEXTALIGN:
        if ((k = kw_find(v, n, KW_TALIGN)) >= 0) emit_decl(P_TEXTALIGN, KW_TALIGN_V[k], imp);
        return;
    case S_MARGIN: {
        const char *t[4]; int tl[4]; int32_t m[4]; int me[4], mr[4];
        if (!split4(v, n, t, tl)) return;
        for (k = 0; k < 4; k++) {
            g_axis = (k & 1) ? 0 : 1;
            if (!parse_off_tok(t[k], tl[k], 1, &m[k])) { g_axis = 0; return; }
            me[k] = g_tok_em; mr[k] = g_tok_rem;
        }
        g_axis = 0;
        for (k = 0; k < 4; k++) emit_decl_e(P_MT + k, m[k], imp, me[k], mr[k]);
        return;
    }
    case S_PADDING: {
        const char *t[4]; int tl[4]; int32_t m[4]; int me[4], mr[4];
        if (!split4(v, n, t, tl)) return;
        for (k = 0; k < 4; k++) {
            g_axis = (k & 1) ? 0 : 1;
            if (!parse_off_tok(t[k], tl[k], 0, &m[k]) || m[k] < 0) { g_axis = 0; return; }
            me[k] = g_tok_em; mr[k] = g_tok_rem;
        }
        g_axis = 0;
        for (k = 0; k < 4; k++) emit_decl_e(P_PT + k, m[k], imp, me[k], mr[k]);
        return;
    }
    case S_MARGIN_IN: case S_MARGIN_BL: case S_PADDING_IN: case S_PADDING_BL: case S_INSET_IN: case S_INSET_BL: {
        /* one or two values: start [end] */
        int block = pd->prop == S_MARGIN_BL || pd->prop == S_PADDING_BL || pd->prop == S_INSET_BL;
        int pad = pd->prop == S_PADDING_IN || pd->prop == S_PADDING_BL;
        int ins = pd->prop == S_INSET_IN || pd->prop == S_INSET_BL;
        int p0 = pad ? (block ? P_PT : P_PL) : ins ? (block ? P_TOP : P_LEFT) : (block ? P_MT : P_ML);
        int p1 = pad ? (block ? P_PB : P_PR) : ins ? (block ? P_BOTTOM : P_RIGHT) : (block ? P_MB : P_MR);
        const char *t[2]; int tl[2]; int32_t m[2]; int me[2], mr[2];
        int i = 0, tn, cnt = 0;
        const char *tk;
        while (cnt < 2 && next_tok(v, n, &i, &tk, &tn)) { t[cnt] = tk; tl[cnt] = tn; cnt++; }
        if (!cnt) return;
        if (cnt == 1) { t[1] = t[0]; tl[1] = tl[0]; }
        g_axis = block ? 1 : 0;
        for (k = 0; k < 2; k++) {
            if (!parse_off_tok(t[k], tl[k], !pad, &m[k]) || (pad && m[k] < 0)) { g_axis = 0; return; }
            me[k] = g_tok_em; mr[k] = g_tok_rem;
        }
        g_axis = 0;
        emit_decl_e(p0, m[0], imp, me[0], mr[0]);
        emit_decl_e(p1, m[1], imp, me[1], mr[1]);
        return;
    }
    case S_INSET: {
        const char *t[4]; int tl[4]; int32_t m[4]; int me[4], mr[4];
        if (!split4(v, n, t, tl)) return;
        for (k = 0; k < 4; k++) {
            g_axis = (k & 1) ? 0 : 1;
            if (!parse_off_tok(t[k], tl[k], 1, &m[k])) { g_axis = 0; return; }
            me[k] = g_tok_em; mr[k] = g_tok_rem;
        }
        g_axis = 0;
        emit_decl_e(P_TOP, m[0], imp, me[0], mr[0]); emit_decl_e(P_RIGHT, m[1], imp, me[1], mr[1]);
        emit_decl_e(P_BOTTOM, m[2], imp, me[2], mr[2]); emit_decl_e(P_LEFT, m[3], imp, me[3], mr[3]);
        return;
    }
    case S_BGCOLOR:
        if (parse_color(v, n, &c)) {
            if (c == COL_INHERIT) emit_inherit(P_BG, imp);
            else if (c == COL_CURRENT) emit_decl(P_BG, COL_CURRENT, imp);
            else emit_decl(P_BG, c, imp);
        }
        return;
    case S_BACKGROUND: {
        int i = 0, tn;
        const char *t;
        int col = COL_NONE;
        while (next_tok(v, n, &i, &t, &tn)) {
            if (starts_lit(t, tn, "url(") || starts_lit(t, tn, "linear-gradient(") ||
                starts_lit(t, tn, "radial-gradient(") || starts_lit(t, tn, "repeating-")) continue;
            if (parse_color(t, tn, &c)) { col = c; }
        }
        if (col == COL_INHERIT) emit_inherit(P_BG, imp);
        else emit_decl(P_BG, col, imp);
        return;
    }
    case S_FSIZE:
        if (is_inherit_kw(v, n)) { emit_inherit(P_FSIZE, imp); return; }
        if (font_size_val(v, n, &x)) emit_decl_e(P_FSIZE, x, imp, 0, g_fs_rem);
        return;
    case S_FONT:
        if (is_inherit_kw(v, n)) { emit_inherit(P_FSIZE, imp); return; }
        emit_font(v, n, imp);
        return;
    case S_WEIGHT:
        if ((c = weight_val(v, n))) emit_decl(P_WEIGHT, c, imp);
        return;
    case S_ITALIC:
        emit_decl(P_ITALIC, eq_lit(v, n, "italic") || eq_lit(v, n, "oblique"), imp);
        return;
    case S_FLEXDIR:
        emit_kw(P_FLEXDIR, v, n, KW_FLEXDIR, 0, imp);
        return;
    case S_FLEXWRAP:
        emit_kw(P_FLEXWRAP, v, n, KW_FLEXWRAP, 0, imp);
        return;
    case S_FLEXFLOW: {
        int i = 0, tn;
        const char *t;
        while (next_tok(v, n, &i, &t, &tn)) {
            if ((k = kw_find(t, tn, KW_FLEXDIR)) >= 0) emit_decl(P_FLEXDIR, k, imp);
            else if ((k = kw_find(t, tn, KW_FLEXWRAP)) >= 0) emit_decl(P_FLEXWRAP, k, imp);
        }
        return;
    }
    case S_GAP: case S_ROWGAP: case S_COLGAP: {
        int i = 0, tn, px, pct, hp, cnt = 0;
        const char *t;
        int vals[2] = { 0, 0 }, ve[2] = { 0, 0 }, vr[2] = { 0, 0 };
        const char *t0 = 0; int tn0 = 0;
        while (cnt < 2 && next_tok(v, n, &i, &t, &tn)) {
            if (cnt == 0) { t0 = t; tn0 = tn; }
            if (eq_lit(t, tn, "normal")) { vals[cnt++] = 0; continue; }
            /* row-gap is vertical, column-gap horizontal; "gap: row column" */
            g_axis = pd->prop == S_COLGAP ? 0 : (pd->prop == S_ROWGAP ? 1 : (cnt == 0 ? 1 : 0));
            if (!parse_length(t, tn, &px, &pct, &hp) || hp) { g_axis = 0; return; }
            if (px >= 0 && px <= 255) { ve[cnt] = g_len_em; vr[cnt] = g_len_rem; }
            if (px < 0) px = 0;
            vals[cnt++] = px > 255 ? 255 : px;
        }
        g_axis = 0;
        if (!cnt) return;
        /* "gap: 10px" is one value for both axes, but the row gap is vertical
           (halved on this screen) and the column gap is not: reparse it. */
        if (pd->prop == S_GAP && cnt == 1 && t0 && !eq_lit(t0, tn0, "normal")) {
            int px2, pct2, hp2;
            if (parse_length(t0, tn0, &px2, &pct2, &hp2) && !hp2) {
                if (px2 < 0) px2 = 0;
                vals[1] = px2 > 255 ? 255 : px2;
                if (px2 <= 255) { ve[1] = g_len_em; vr[1] = g_len_rem; }
                cnt = 2;
            }
        }
        if (pd->prop == S_GAP) {
            int c1 = cnt > 1;
            int cv = vals[1];
            if (!c1) {
                /* one value is both gaps; the row gap was measured on the (halved) vertical
                   axis, the column gap must be measured on the horizontal one */
                cv = 0;
                if (!eq_lit(t0, tn0, "normal") && parse_length(t0, tn0, &px, &pct, &hp) && !hp)
                    cv = px < 0 ? 0 : (px > 255 ? 255 : px);
            }
            emit_decl_e(P_ROWGAP, vals[0], imp, ve[0], vr[0]);
            emit_decl_e(P_COLGAP, cv, imp, c1 ? ve[1] : ve[0], c1 ? vr[1] : vr[0]);
        } else if (pd->prop == S_ROWGAP) emit_decl_e(P_ROWGAP, vals[0], imp, ve[0], vr[0]);
        else emit_decl_e(P_COLGAP, vals[0], imp, ve[0], vr[0]);
        return;
    }
    case S_GROWNUM:
        if (parse_number(v, n, &c)) emit_decl(P_GROW, c, imp);
        return;
    case S_SHRINKNUM:
        if (parse_number(v, n, &c)) emit_decl(P_SHRINK, c, imp);
        return;
    case S_BASIS: {
        int16_t b;
        if (eq_lit(v, n, "auto") || eq_lit(v, n, "content")) emit_decl(P_BASIS, -32768, imp);
        else if (parse_basis_tok(v, n, &b)) emit_decl(P_BASIS, b, imp);
        return;
    }
    case S_FLEX:
        emit_flex(v, n, imp);
        return;
    case S_ORDER: {
        int num;
        if (parse_int_tok(v, n, &num)) {
            if (num > 320) num = 320;
            if (num < -320) num = -320;
            emit_decl(P_ORDER, num * 100, imp);
        }
        return;
    }
    case S_POSITION:
        if ((k = kw_find(v, n, KW_POS)) >= 0) emit_decl(P_POS, k, imp);
        else if (eq_lit(v, n, "-webkit-sticky")) emit_decl(P_POS, POS_STICKY, imp);
        return;
    case S_ZINDEX:
        if (eq_lit(v, n, "auto")) emit_decl(P_ZINDEX, 0x7FFFFFFF, imp);
        else if (parse_int_tok(v, n, &c)) emit_decl(P_ZINDEX, c, imp);
        return;
    case S_OVERFLOW: {
        int i = 0, tn;
        const char *t;
        int cnt = 0;
        int best = OVF_VISIBLE;
        while (next_tok(v, n, &i, &t, &tn) && cnt < 2) {
            k = kw_find(t, tn, KW_OVF);
            if (k < 0) return;
            /* x and y are not independent here: hidden/clip wins; auto/scroll
               only counts when nothing clips. */
            if (k == OVF_HIDDEN || k == OVF_CLIP) best = k;
            else if (best == OVF_VISIBLE) best = k;
            cnt++;
        }
        if (cnt) emit_decl(P_OVERFLOW, best, imp);
        return;
    }
    case S_VISIBILITY:
        if (eq_lit(v, n, "visible")) emit_decl(P_VISIBILITY, VIS_SHOWN, imp);
        else if (eq_lit(v, n, "hidden") || eq_lit(v, n, "collapse")) emit_decl(P_VISIBILITY, VIS_HIDDEN, imp);
        return;
    case S_OPACITY: {
        Dim d;
        int cc = parse_dim(v, n, &d);
        if (!cc || cc != n) return;
        int op;
        int om = d.mil > 1000000 ? 1000000 : (d.mil < 0 ? 0 : d.mil);
        if (d.unit == U_PCT) op = om * 255 / 100000;
        else if (d.unit == U_NONE) op = om * 255 / 1000;
        else return;
        if (op < 0) op = 0;
        if (op > 255) op = 255;
        emit_decl(P_OPACITY, op, imp);
        return;
    }
    case S_CURSOR: {
        /* "url(x), pointer": the last token is the keyword */
        int i = 0, tn;
        const char *t, *last = v;
        int lastn = n;
        while (next_tok(v, n, &i, &t, &tn)) { last = t; lastn = tn; }
        if (lastn > 0 && last[lastn - 1] == ',') lastn--;
        if ((k = kw_find(last, lastn, KW_CURSOR)) >= 0) emit_decl(P_CURSOR, k, imp);
        return;
    }
    case S_BOXSZ:
        if (eq_lit(v, n, "border-box")) emit_decl(P_BOXSZ, BS_BORDER, imp);
        else if (eq_lit(v, n, "content-box")) emit_decl(P_BOXSZ, BS_CONTENT, imp);
        return;
    case S_BORDER: emit_border(v, n, 15, imp); return;
    case S_BORDER_T: emit_border(v, n, 1, imp); return;
    case S_BORDER_R: emit_border(v, n, 2, imp); return;
    case S_BORDER_B: emit_border(v, n, 4, imp); return;
    case S_BORDER_L: emit_border(v, n, 8, imp); return;
    case S_BORDER_W: {
        const char *t[4]; int tl[4]; int w[4];
        if (!split4(v, n, t, tl)) return;
        for (k = 0; k < 4; k++) if (!border_width_tok_ax(t[k], tl[k], &w[k], (k & 1) ? 0 : 1)) return;
        for (k = 0; k < 4; k++) emit_decl(P_BWT + k, w[k], imp);
        return;
    }
    case S_BORDER_C: {
        const char *t[4]; int tl[4]; int cc[4];
        if (!split4(v, n, t, tl)) return;
        for (k = 0; k < 4; k++) {
            if (!parse_color(t[k], tl[k], &cc[k])) return;
            if (cc[k] < 0) cc[k] = BC_CUR;
        }
        for (k = 0; k < 4; k++) emit_decl(P_BCT + k, cc[k], imp);
        return;
    }
    case S_BORDER_S: {
        const char *t[4]; int tl[4]; int st[4];
        if (!split4(v, n, t, tl)) return;
        for (k = 0; k < 4; k++) {
            int q = kw_find(t[k], tl[k], KW_BSTYLE);
            if (q < 0) return;
            st[k] = q <= 1 ? 0 : 1;
        }
        for (k = 0; k < 4; k++) emit_decl(P_BST + k, st[k], imp);
        return;
    }
    case S_BORDERW_T: case S_BORDERW_R: case S_BORDERW_B: case S_BORDERW_L: {
        int w, side = pd->prop - S_BORDERW_T;
        if (border_width_tok_ax(v, n, &w, (side & 1) ? 0 : 1)) emit_decl(P_BWT + side, w, imp);
        return;
    }
    case S_BORDERC_T: case S_BORDERC_R: case S_BORDERC_B: case S_BORDERC_L:
        if (parse_color(v, n, &c)) emit_decl(P_BCT + (pd->prop - S_BORDERC_T), c < 0 ? BC_CUR : c, imp);
        return;
    case S_BORDERS_T: case S_BORDERS_R: case S_BORDERS_B: case S_BORDERS_L: {
        int q = kw_find(v, n, KW_BSTYLE);
        if (q >= 0) emit_decl(P_BST + (pd->prop - S_BORDERS_T), q <= 1 ? 0 : 1, imp);
        return;
    }
    case S_RADIUS: {
        /* first value only; "a b c d" and elliptical forms collapse to it */
        int sp = 0;
        while (sp < n && v[sp] != ' ' && v[sp] != '\t' && v[sp] != '/') sp++;
        int px, pct, hp;
        if (!parse_length(v, sp, &px, &pct, &hp)) return;
        int r;
        if (hp) r = pct >= 50 ? RADIUS_FULL : pct * 2;
        else r = px > 254 ? 254 : px;
        if (!hp && px >= 100) r = RADIUS_FULL;
        if (r < 0) r = 0;
        emit_decl(P_RADIUS, r, imp);
        return;
    }
    case S_BORDERSPACING: {
        int px, pct, hp;
        int sp = 0;
        while (sp < n && v[sp] != ' ') sp++;
        if (parse_length(v, sp, &px, &pct, &hp) && !hp) emit_decl(P_BSPACE, px < 0 ? 0 : (px > 32 ? 32 : px), imp);
        return;
    }
    case S_BORDERCOLLAPSE:
        emit_decl(P_COLLAPSE, eq_lit(v, n, "collapse"), imp);
        return;
    case S_DECOR: {
        int i = 0, tn, d = 0;
        const char *t;
        while (next_tok(v, n, &i, &t, &tn)) {
            if (eq_lit(t, tn, "none")) d = TD_NONE;
            else if (eq_lit(t, tn, "underline")) d = TD_UNDERLINE;
            else if (eq_lit(t, tn, "line-through")) d = TD_LINETHROUGH;
        }
        if (d) emit_decl(P_DECOR, d, imp);
        return;
    }
    case S_TRANSFORMFN: {
        /* translate()/translateX()/translateY()/translate3d() only; anything
           else (rotate, scale, matrix) is ignored */
        int32_t tx = 0, ty = 0;
        int i = 0, tn, any = 0;
        const char *t;
        while (next_tok(v, n, &i, &t, &tn)) {
            int op = 0;
            while (op < tn && t[op] != '(') op++;
            if (op >= tn) continue;
            int close = tn;
            if (t[tn - 1] == ')') close--;
            int axis_x = 0, axis_y = 0, both = 0;
            if (op == 10 && eq_lit(t, 10, "translatex")) axis_x = 1;
            else if (op == 10 && eq_lit(t, 10, "translatey")) axis_y = 1;
            else if (op == 9 && eq_lit(t, 9, "translate")) both = 1;
            else if (op == 11 && eq_lit(t, 11, "translate3d")) both = 1;
            else continue;
            const char *a = t + op + 1;
            int an = close - op - 1;
            int comma = -1, d2 = 0;
            for (int q = 0; q < an; q++) {
                if (a[q] == '(') d2++;
                else if (a[q] == ')') d2--;
                else if (a[q] == ',' && d2 == 0) { comma = q; break; }
            }
            int32_t first, second = 0;
            g_axis = 0;
            if (!parse_tr_tok(a, comma < 0 ? an : comma, &first)) { g_axis = 0; continue; }
            if (axis_x || both) { tx = first; any = 1; }
            if (axis_y) {
                g_axis = 1;
                if (parse_tr_tok(a, an, &second)) { ty = second; any = 1; }
                g_axis = 0;
            }
            if (both && comma >= 0) {
                int rest = an - comma - 1, e2 = comma + 1, d3 = 0;
                for (int q = e2; q < an; q++) {
                    if (a[q] == '(') d3++;
                    else if (a[q] == ')') d3--;
                    else if (a[q] == ',' && d3 == 0) { rest = q - e2; break; }
                }
                g_axis = 1;
                if (parse_tr_tok(a + e2, rest, &second)) ty = second;
                g_axis = 0;
            }
        }
        if (any || eq_lit(v, n, "none")) { emit_decl(P_TX, tx, imp); emit_decl(P_TY, ty, imp); }
        return;
    }
    case S_TRANSFORM:
        if (eq_lit(v, n, "uppercase")) emit_decl(P_TRANSFORM, TT_UPPER, imp);
        else if (eq_lit(v, n, "lowercase")) emit_decl(P_TRANSFORM, TT_LOWER, imp);
        else if (eq_lit(v, n, "capitalize")) emit_decl(P_TRANSFORM, TT_CAPITALIZE, imp);
        else if (eq_lit(v, n, "none")) emit_decl(P_TRANSFORM, TT_NONE, imp);
        return;
    case S_WS:
        if (eq_lit(v, n, "nowrap")) emit_decl(P_WS, WS_NOWRAP, imp);
        else if (eq_lit(v, n, "pre-wrap") || eq_lit(v, n, "break-spaces")) emit_decl(P_WS, WS_PREWRAP, imp);
        else if (eq_lit(v, n, "pre-line")) emit_decl(P_WS, WS_PRELINE, imp);
        else if (eq_lit(v, n, "pre")) emit_decl(P_WS, WS_PRE, imp);
        else if (eq_lit(v, n, "normal")) emit_decl(P_WS, WS_NORMAL, imp);
        return;
    case S_LINEH:
        if (lineh_val(v, n, &x)) emit_decl(P_LINEH, x, imp);
        return;
    case S_LETSP: {
        int px, pct, hp;
        if (eq_lit(v, n, "normal")) emit_decl(P_LETSP, 0, imp);
        else if (parse_length(v, n, &px, &pct, &hp) && !hp) emit_decl(P_LETSP, px < 0 ? 0 : (px > 8 ? 8 : px), imp);
        return;
    }
    case S_INDENT: {
        int px, pct, hp;
        if (parse_length(v, n, &px, &pct, &hp)) {
            int em = g_len_em, rem = g_len_rem;
            if (hp) px = RB_VIEW_W * pct / 100;
            if (px > 400 || px < -400) em = rem = 0;
            if (px > 400) px = 400;
            if (px < -400) px = -400;
            emit_decl_e(P_INDENT, px, imp, em, rem);
        }
        return;
    }
    case S_VALIGN:
        if ((k = kw_find(v, n, KW_VALIGN)) >= 0) emit_decl(P_VALIGN, k, imp);
        return;
    case S_FLOATV:
        if ((k = kw_find(v, n, KW_FLOAT)) >= 0) emit_decl(P_FLOAT, KW_FLOAT_V[k], imp);
        return;
    case S_CLEAR:
        if ((k = kw_find(v, n, KW_CLEAR)) >= 0) emit_decl(P_CLEAR, KW_CLEAR_V[k], imp);
        return;
    case S_LISTSTYLE: {
        int i = 0, tn;
        const char *t;
        while (next_tok(v, n, &i, &t, &tn)) {
            if ((k = kw_find(t, tn, KW_LSTYPE)) >= 0) emit_decl(P_LSTYPE, KW_LSTYPE_V[k], imp);
        }
        return;
    }
    case S_LSTYPE:
        if ((k = kw_find(v, n, KW_LSTYPE)) >= 0) emit_decl(P_LSTYPE, KW_LSTYPE_V[k], imp);
        return;
    case S_CONTENT: {
        int id = parse_content(v, n);
        emit_decl(P_CONTENT, id, imp);
        return;
    }
    case S_GAREAS: {
        if (g_dout != g_decl) return;           /* inline style: aux is scratch */
        int at = parse_grid_areas(v, n);
        if (at >= 0) emit_decl(P_GAREAS, at + 1, imp);
        return;
    }
    case S_GTEMPLATE: {
        /* [rows and "area" strings] / [columns]; rows are ignored (auto) */
        int slash = -1, q = 0;
        for (int k = 0; k < n; k++) {
            if (v[k] == '"' || v[k] == '\'') q = !q;
            else if (v[k] == '/' && !q) { slash = k; break; }
        }
        int rn = slash < 0 ? n : slash;
        if (g_dout == g_decl) {
            int at = parse_grid_areas(v, rn);
            if (at >= 0) emit_decl(P_GAREAS, at + 1, imp);
        }
        if (slash >= 0) {
            int at = parse_grid_template(v + slash + 1, n - slash - 1);
            if (at >= 0) emit_decl(P_GCOLS, at, imp);
        }
        return;
    }
    case S_GAREA: {
        int i = 0, tn, cnt = 0;
        const char *t;
        const char *seg[4];
        int segn[4];
        int slash = 0;
        for (int k = 0; k < n; k++) if (v[k] == '/') slash++;
        if (slash == 0 && next_tok(v, n, &i, &t, &tn) && !next_tok(v, n, &i, &t, &tn)) {
            const char *t1; int t1n, tmp;
            i = 0; next_tok(v, n, &i, &t1, &t1n);
            if (!parse_int_tok(t1, t1n, &tmp) && !starts_lit(t1, t1n, "span") && !eq_lit(t1, t1n, "auto")) {
                emit_decl(P_GAREA, grid_name_hash(t1, t1n), imp);
                return;
            }
        }
        /* row-start / column-start / row-end / column-end */
        int s = 0;
        for (int k = 0; k <= n && cnt < 4; k++) {
            if (k == n || v[k] == '/') { seg[cnt] = v + s; segn[cnt] = k - s; cnt++; s = k + 1; }
        }
        int16_t rs = 0, re = 0, cs = 0, ce = 0; int rsp = 0, csp = 0;
        for (int k = 0; k < cnt; k++) {
            int l, spn;
            parse_grid_line(seg[k], segn[k], &l, &spn);
            if (k == 0) { rs = (int16_t)l; if (spn) rsp = spn; }
            else if (k == 1) { cs = (int16_t)l; if (spn) csp = spn; }
            else if (k == 2) { re = (int16_t)l; if (spn) rsp = spn; }
            else { ce = (int16_t)l; if (spn) csp = spn; }
        }
        emit_decl(P_GROWL, (int32_t)((uint32_t)(uint16_t)rs | ((uint32_t)(uint16_t)re << 16)), imp);
        emit_decl(P_GROWSPAN, rsp, imp);
        emit_decl(P_GCOL, (int32_t)((uint32_t)(uint16_t)cs | ((uint32_t)(uint16_t)ce << 16)), imp);
        emit_decl(P_GCOLSPAN, csp, imp);
        return;
    }
    case S_GCOLS: {
        int at = parse_grid_template(v, n);
        if (at >= 0) emit_decl(P_GCOLS, at, imp);
        return;
    }
    case S_GCOL: case S_GROW: {
        int16_t s0, e0;
        uint8_t sp;
        parse_grid_area(v, n, &s0, &e0, &sp);
        int base = pd->prop == S_GCOL ? P_GCOL : P_GROWL;
        emit_decl(base, (int32_t)((uint32_t)(uint16_t)s0 | ((uint32_t)(uint16_t)e0 << 16)), imp);
        emit_decl(base + 1, sp, imp);
        return;
    }
    case S_JITEMS: case S_PLACEITEMS: {
        int i = 0, tn;
        const char *t;
        if (!next_tok(v, n, &i, &t, &tn)) return;
        int j = -1;
        if (eq_lit(t, tn, "center")) j = JI_CENTER;
        else if (eq_lit(t, tn, "stretch") || eq_lit(t, tn, "normal")) j = JI_STRETCH;
        else if (eq_lit(t, tn, "start") || eq_lit(t, tn, "flex-start") || eq_lit(t, tn, "left")) j = JI_START;
        else if (eq_lit(t, tn, "end") || eq_lit(t, tn, "flex-end") || eq_lit(t, tn, "right")) j = JI_END;
        if (j >= 0) {
            emit_decl(P_JITEMS, j, imp);
            if (pd->prop == S_PLACEITEMS) emit_decl(P_ALIGNITEMS, j, imp);
        }
        return;
    }
    case S_JSELF: {
        /* only honoured on block-level boxes in block flow: 0 auto, 1 start, 2 end, 3 centre */
        int i = 0, tn;
        const char *t;
        if (!next_tok(v, n, &i, &t, &tn)) return;
        int j = 0;
        if (eq_lit(t, tn, "center")) j = 3;
        else if (eq_lit(t, tn, "start") || eq_lit(t, tn, "flex-start") || eq_lit(t, tn, "left") ||
                 eq_lit(t, tn, "self-start")) j = 1;
        else if (eq_lit(t, tn, "end") || eq_lit(t, tn, "flex-end") || eq_lit(t, tn, "right") ||
                 eq_lit(t, tn, "self-end")) j = 2;
        emit_decl(P_JSELF, j, imp);
        return;
    }
    case S_PLACECONTENT: {
        int i = 0, tn;
        const char *t;
        if (!next_tok(v, n, &i, &t, &tn)) return;
        if ((k = kw_find(t, tn, KW_ACNT)) >= 0) emit_decl(P_ALIGNCONTENT, KW_ACNT_V[k], imp);
        const char *t2;
        int tn2;
        const char *src = t;
        int srcn = tn;
        if (next_tok(v, n, &i, &t2, &tn2)) { src = t2; srcn = tn2; }
        if ((k = kw_find(src, srcn, KW_JUSTIFY)) >= 0) emit_decl(P_JUSTIFY, KW_JUSTIFY_V[k], imp);
        return;
    }
    default:
        return;
    }
}

/* Parse "prop: value; ..." into the current sink. */
static void parse_decl_list(const char *s, int n) {
    int i = 0;
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == ';')) i++;
        if (i >= n) break;
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i = i + 2 < n ? i + 2 : n;
            continue;
        }
        int ps = i;
        while (i < n && s[i] != ':' && s[i] != ';' && s[i] != '}') i++;
        if (i >= n || s[i] != ':') {
            while (i < n && s[i] != ';') i++;
            continue;
        }
        int pn = i - ps;
        i++;
        int vs = i;
        int depth = 0;
        char q = 0;
        while (i < n) {
            char c = s[i];
            if (q) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == q) q = 0; i++; continue; }
            if (c == '"' || c == '\'') q = c;
            else if (c == '(') depth++;
            else if (c == ')' && depth > 0) depth--;
            else if (c == ';' && depth == 0) break;
            else if (c == '}' && depth == 0) break;
            i++;
        }
        int ve = i;
        while (pn > 0 && (s[ps + pn - 1] == ' ' || s[ps + pn - 1] == '\t' || s[ps + pn - 1] == '\n')) pn--;
        const char *val = s + vs;
        int vn = ve - vs;
        trim_tok(&val, &vn);
        int imp = 0;
        if (vn >= 10) {
            /* "... !important" */
            int e = vn;
            while (e > 0 && val[e - 1] == ' ') e--;
            if (e >= 10 && eq_lit(val + e - 9, 9, "important")) {
                int b = e - 9;
                while (b > 0 && val[b - 1] == ' ') b--;
                if (b > 0 && val[b - 1] == '!') { imp = 1; vn = b - 1; while (vn > 0 && val[vn - 1] == ' ') vn--; }
            }
        }
        if (pn > 2 && s[ps] == '-' && s[ps + 1] == '-') {
            if (g_var_capture) var_set(s + ps, pn, val, vn, g_var_capture == 2);
            continue;
        }
        char sub[512];
        if (vn > 4) {
            int has = 0;
            for (int k = 0; k + 3 < vn; k++) if (val[k] == 'v' && val[k + 1] == 'a' && val[k + 2] == 'r' && val[k + 3] == '(') { has = 1; break; }
            if (has) {
                g_var_steps = 48;
                int m = var_subst(val, vn, sub, (int)sizeof(sub), 0);
                if (m < 0) continue;
                val = sub;
                vn = m;
            }
        }
        for (const PropDef *pd = g_props; pd->name; pd++) {
            if (eq_lit(s + ps, pn, pd->name)) { decl_prop(pd, val, vn, imp); break; }
        }
        g_axis = 0;
    }
}
