/* rbrowse table layout (display: table / table-row / table-cell ...).
   Included by engine.c after layout_web.inc.

   Auto table layout, simplified: every cell reports a min-content and a
   max-content width; columns take the larger of their cells' figures; the
   table then hands out its width between min and max (percent and fixed
   widths first). Cells are laid out once, in place, as blocks of the column
   (span) width; a row's height is the tallest cell, and shorter cells are
   stretched afterwards by patching their own background/border boxes and
   shifting their content for vertical-align. */

#define MAX_TC 3072
#define MAX_TR 1536
#define TBL_COLS 32

typedef struct {
    uint16_t node;
    RbColor ic; uint8_t isc, ia;
    int16_t row, col, cs, rs;
    int minw, maxw;          /* border-box widths */
    int pct;                 /* percentage width, 0 = none */
    int fixw;                /* explicit px width, 0 = none */
    int h, y0, w;            /* placed height, top, width */
    int box_a, box_b;        /* the cell's boxes [a, b) */
    int own;                 /* leading boxes that are the cell's own backgrounds/borders */
    uint8_t valign;
} TCell;

typedef struct {
    uint16_t node;
    RbColor ic; uint8_t isc, ia;
    int y, h;
    int bg_box;              /* row background box, -1 none */
    int grp;                 /* row-group index it belongs to, -1 none */
    uint32_t occ;            /* occupied columns (rowspans from above) */
} TRow;

typedef struct {
    uint16_t node;
    int first_row, last_row;
    int bg_box;
} TGrp;

#define MAX_TG 256
static TCell g_tc[MAX_TC];
static TRow g_tr[MAX_TR];
static TGrp g_tg[MAX_TG];
static int g_tc_top, g_tr_top, g_tg_top;

/* flags consumed by layout_block on entry */
static int g_cell_mode;          /* 1 cell, 2 no left border, 4 no top border */

typedef struct {
    int tc0, tr0, tg0;           /* slice bases */
    int nc, nr, ng;
    int ncols;
    int caption;                 /* caption node or 0 */
    RbColor cic; uint8_t cisc, cia;
    int colmin[TBL_COLS], colmax[TBL_COLS], colpct[TBL_COLS], colw[TBL_COLS], colx[TBL_COLS];
    int colfix[TBL_COLS];
} Tbl;

static int tbl_attr_span(int node, const char *name, int nn, int max) {
    const char *v; int vn, n = 1;
    if (attr_get(node, name, nn, &v, &vn) && parse_int_tok(v, vn, &n)) {
        if (n < 1) n = 1;
        if (n > max) n = max;
    }
    return n;
}

static void tbl_gather_rows(Tbl *t, int parent, RbColor ic, uint8_t isc, uint8_t ia, int grp, int depth);

/* Append the cell `c` (style cs) to row `rowi` of the table being gathered. */
static int tbl_add_cell(Tbl *t, int c, const Style *cs, RbColor ic, uint8_t isc, uint8_t ia, int rowi) {
    if (g_tc_top >= MAX_TC) { SET_OOM(); return 0; }
    TCell *cell = &g_tc[g_tc_top++];
    memset(cell, 0, sizeof(*cell));
    cell->node = (uint16_t)c;
    cell->ic = ic; cell->isc = isc; cell->ia = ia;
    cell->row = (int16_t)rowi;
    cell->cs = (int16_t)tbl_attr_span(c, "colspan", 7, TBL_COLS);
    cell->rs = (int16_t)tbl_attr_span(c, "rowspan", 7, 64);
    cell->valign = cs->valign;
    t->nc++;
    return 1;
}

static void tbl_add_row(Tbl *t, int rnode, RbColor ic, uint8_t isc, uint8_t ia, int grp) {
    if (g_tr_top >= MAX_TR) { SET_OOM(); return; }
    Style rs = compute_style(rnode, ic, isc, ia);
    int ri = g_tr_top++;
    TRow *r = &g_tr[ri];
    r->node = (uint16_t)rnode;
    r->ic = rs.color; r->isc = rs.scale; r->ia = rs.align;
    r->y = r->h = 0;
    r->bg_box = -1;
    r->grp = grp;
    r->occ = 0;
    t->nr++;
    int rowi = ri - t->tr0;
    for (int c = g_nodes[rnode].first; c; c = g_nodes[c].next) {
        if (g_nodes[c].dead || g_nodes[c].hidden || g_nodes[c].kind != 0) continue;
        Style cs = compute_style(c, rs.color, rs.scale, rs.align);
        if (cs.display == DISP_NONE) continue;
        if (is_oof(&cs)) { oof_defer(c, &cs, rs.color, rs.scale, rs.align, 0, 0); continue; }
        if (!tbl_add_cell(t, c, &cs, rs.color, rs.scale, rs.align, rowi)) return;
    }
}

static void tbl_gather_rows(Tbl *t, int parent, RbColor ic, uint8_t isc, uint8_t ia, int grp, int depth) {
    int anon_row = -1;      /* open anonymous row collecting consecutive display:table-cell children */
    for (int c = g_nodes[parent].first; c; c = g_nodes[c].next) {
        if (g_nodes[c].dead || g_nodes[c].hidden || g_nodes[c].kind != 0) continue;
        Style cs = compute_style(c, ic, isc, ia);
        if (cs.display == DISP_NONE) continue;
        if (is_oof(&cs)) { oof_defer(c, &cs, ic, isc, ia, 0, 0); continue; }
        if (cs.display == DISP_TABLE_CELL) {
            /* cells with no row around them (<li style="display:table"><div cell><a cell>) share an
               anonymous row instead of each becoming a one-cell row of its own */
            if (anon_row < 0) {
                if (g_tr_top >= MAX_TR) { SET_OOM(); return; }
                int ri = g_tr_top++;
                TRow *r = &g_tr[ri];
                r->node = 0;
                r->ic = ic; r->isc = isc; r->ia = ia;
                r->y = r->h = 0; r->bg_box = -1; r->grp = grp; r->occ = 0;
                t->nr++;
                anon_row = ri - t->tr0;
            }
            if (!tbl_add_cell(t, c, &cs, ic, isc, ia, anon_row)) return;
            continue;
        }
        anon_row = -1;
        if (cs.display == DISP_TABLE_CAPTION) { if (!t->caption) t->caption = c; continue; }
        if (cs.display == DISP_TABLE_ROW) { tbl_add_row(t, c, cs.color, cs.scale, cs.align, grp); continue; }
        if (cs.display == DISP_TABLE_ROWGROUP && depth < 2) {
            int gi = -1;
            if (g_tg_top < MAX_TG && cs.has_bg) {
                gi = g_tg_top++;
                g_tg[gi].node = (uint16_t)c;
                g_tg[gi].first_row = g_tr_top;
                g_tg[gi].last_row = g_tr_top - 1;
                g_tg[gi].bg_box = -1;
                t->ng++;
            }
            tbl_gather_rows(t, c, cs.color, cs.scale, cs.align, gi >= 0 ? gi : grp, depth + 1);
            if (gi >= 0) g_tg[gi].last_row = g_tr_top - 1;
            continue;
        }
        /* a stray block inside a table: a one-cell row of its own */
        if (g_tr_top < MAX_TR && g_tc_top < MAX_TC) {
            int ri = g_tr_top++;
            TRow *r = &g_tr[ri];
            r->node = 0;
            r->ic = ic; r->isc = isc; r->ia = ia;
            r->y = r->h = 0; r->bg_box = -1; r->grp = grp; r->occ = 0;
            t->nr++;
            TCell *cell = &g_tc[g_tc_top++];
            memset(cell, 0, sizeof(*cell));
            cell->node = (uint16_t)c;
            cell->ic = ic; cell->isc = isc; cell->ia = ia;
            cell->row = (int16_t)(ri - t->tr0);
            cell->cs = TBL_COLS;      /* spans every column: fixed up after the grid is known */
            cell->rs = 1;
            t->nc++;
        } else SET_OOM();
    }
}

static int tbl_mincontent(int node, const Style *st) {
    int m = min_content_words(node, st->color, st->scale, st->align, row_sums(st), 0);
    if (st->x.ws == WS_NOWRAP || st->x.ws == WS_PRE) return 0x7FFF;
    return m + st->pl + st->pr;
}

/* Collect cells, build the grid, measure every cell and size the columns.
   Returns the table's preferred width (sum of column max widths + spacing).
   `avail` is the inner width to distribute; pass 0 to only measure. */
static int tbl_setup(Tbl *t, int node, const Style *tst, int avail, RbColor ic, uint8_t isc, uint8_t ia) {
    memset(t, 0, sizeof(*t));
    t->tc0 = g_tc_top; t->tr0 = g_tr_top; t->tg0 = g_tg_top;
    t->cic = ic; t->cisc = isc; t->cia = ia;
    RbColor pic = tst->color; uint8_t pisc = tst->scale, pia = tst->align;
    (void)ic; (void)isc; (void)ia;
    tbl_gather_rows(t, node, pic, pisc, pia, -1, 0);
    /* place cells on the grid */
    int ncols = 0;
    int sp_max_row = 0;
    for (int ci = 0; ci < t->nc; ci++) {
        TCell *c = &g_tc[t->tc0 + ci];
        TRow *r = &g_tr[t->tr0 + c->row];
        int col = 0;
        while (col < TBL_COLS && (r->occ >> col) & 1u) col++;
        if (c->cs == TBL_COLS) { /* stray block: spans what the grid ends up with */ c->col = 0; continue; }
        if (col >= TBL_COLS) { c->cs = 0; continue; }
        if (col + c->cs > TBL_COLS) c->cs = (int16_t)(TBL_COLS - col);
        c->col = (int16_t)col;
        uint32_t mask = c->cs >= 32 ? 0xFFFFFFFFu : (((1u << c->cs) - 1u) << col);
        for (int k = 0; k < c->rs && c->row + k < t->nr; k++) g_tr[t->tr0 + c->row + k].occ |= mask;
        if (col + c->cs > ncols) ncols = col + c->cs;
        if (c->row + c->rs > sp_max_row) sp_max_row = c->row + c->rs;
    }
    if (ncols < 1) ncols = 1;
    t->ncols = ncols;
    for (int ci = 0; ci < t->nc; ci++) {
        TCell *c = &g_tc[t->tc0 + ci];
        if (c->cs == TBL_COLS) c->cs = (int16_t)ncols;
    }
    int sp = tst->bspace;
    if (tst->collapse) sp = 0;
    int spy = g_web ? (sp + 1) / 2 : sp;
    (void)spy;
    /* measure */
    for (int ci = 0; ci < t->nc; ci++) {
        TCell *c = &g_tc[t->tc0 + ci];
        if (c->cs <= 0) continue;
        Style cs = compute_style(c->node, c->ic, c->isc, c->ia);
        int content_mx = 0;
        if (cs.width <= 0 || cs.width == -32768 || cs.width < 0) {
            g_cell_mode = 1;
            content_mx = block_max_content(c->node, c->ic, c->isc, c->ia);
            g_cell_mode = 0;
        }
        int mx = cs.width > 0 ? cs.width : content_mx;
        int mn = tbl_mincontent(c->node, &cs);
        /* nowrap / pre cell: its words cannot break, so min-content is the max-content width */
        if (mn == 0x7FFF) mn = cs.width > 0 ? cs.width : content_mx;
        if (cs.min_w > 0 && mn < 0x7FFF && mn < cs.min_w) mn = cs.min_w;     /* min-width is a floor for the column */
        /* min-content can exceed the measured extent (trailing margins of nested
           blocks are not boxes), so lift max rather than clip min */
        if (mn > mx) mx = mn;
        if (mn > mx) mn = mx;
        c->minw = mn;
        c->maxw = mx;
        c->fixw = cs.width > 0 ? cs.width : 0;
        c->pct = (cs.width < 0 && cs.width != -32768) ? -cs.width : 0;
    }
    /* single-column cells first, then spanning cells share their excess */
    for (int ci = 0; ci < t->nc; ci++) {
        TCell *c = &g_tc[t->tc0 + ci];
        if (c->cs != 1) continue;
        int k = c->col;
        if (c->minw > t->colmin[k]) t->colmin[k] = c->minw;
        if (c->maxw > t->colmax[k]) t->colmax[k] = c->maxw;
        if (c->pct > t->colpct[k]) t->colpct[k] = c->pct;
        if (c->fixw > t->colfix[k]) t->colfix[k] = c->fixw;
    }
    for (int ci = 0; ci < t->nc; ci++) {
        TCell *c = &g_tc[t->tc0 + ci];
        if (c->cs <= 1) continue;
        int smin = 0, smax = 0;
        for (int k = c->col; k < c->col + c->cs; k++) { smin += t->colmin[k]; smax += t->colmax[k]; }
        int ex = c->minw - smin - sp * (c->cs - 1);
        if (ex > 0) for (int k = c->col; k < c->col + c->cs; k++) t->colmin[k] += ex / c->cs + (k == c->col ? ex % c->cs : 0);
        ex = c->maxw - smax - sp * (c->cs - 1);
        if (ex > 0) for (int k = c->col; k < c->col + c->cs; k++) t->colmax[k] += ex / c->cs + (k == c->col ? ex % c->cs : 0);
        if (c->pct) { int p = c->pct / c->cs; for (int k = c->col; k < c->col + c->cs; k++) if (p > t->colpct[k]) t->colpct[k] = p; }
    }
    for (int k = 0; k < ncols; k++) if (t->colmax[k] < t->colmin[k]) t->colmax[k] = t->colmin[k];
    int pref = sp * (ncols + 1);
    for (int k = 0; k < ncols; k++) pref += t->colmax[k];
    if (avail <= 0) return pref;

    /* hand out the width */
    int room = avail - sp * (ncols + 1);
    if (room < ncols * 4) room = ncols * 4;
    int pct_sum = 0;
    for (int k = 0; k < ncols; k++) pct_sum += t->colpct[k];
    {
        /* A table that cannot even fit its columns' min-content widths gets no percentage widths: every
           column is squeezed in proportion to its min-content instead (percentages would starve the others). */
        int tot_min = 0;
        for (int k = 0; k < ncols; k++) tot_min += t->colmin[k];
        if (tot_min >= room) pct_sum = 0;
    }
    int fixed = 0;
    for (int k = 0; k < ncols; k++) t->colw[k] = 0;
    if (pct_sum > 0) {
        for (int k = 0; k < ncols; k++) {
            if (!t->colpct[k]) continue;
            int p = t->colpct[k];
            int w = mul_div(room, p, pct_sum > 100 ? pct_sum : 100);
            if (w < t->colmin[k]) w = t->colmin[k];
            t->colw[k] = w;
            fixed += w;
        }
        /* the other columns keep at least their min-content: percent columns give up the rest
           (width:100% next to a min-width cell must not push the cell out of the table) */
        int nonpct_min = 0, pct_min = 0;
        for (int k = 0; k < ncols; k++) { if (t->colpct[k]) pct_min += t->colmin[k]; else nonpct_min += t->colmin[k]; }
        int budget = room - nonpct_min;
        if (fixed > budget && budget > pct_min) {
            int over = fixed - budget, flex = fixed - pct_min;
            for (int k = 0; k < ncols; k++) {
                if (!t->colpct[k]) continue;
                int cut = flex > 0 ? mul_div(over, t->colw[k] - t->colmin[k], flex) : 0;
                t->colw[k] -= cut;
            }
            fixed = 0;
            for (int k = 0; k < ncols; k++) if (t->colpct[k]) fixed += t->colw[k];
        }
    }
    int rem = room - fixed;
    int smin = 0, smax = 0;
    for (int k = 0; k < ncols; k++) if (!t->colpct[k] || t->colw[k] == 0) { smin += t->colmin[k]; smax += t->colmax[k]; }
    int free_cols = 0;
    for (int k = 0; k < ncols; k++) if (!t->colw[k]) free_cols++;
    if (free_cols) {
        if (rem >= smax) {
            int extra = rem - smax;
            for (int k = 0; k < ncols; k++) {
                if (t->colw[k]) continue;
                int w = t->colmax[k];
                if (extra > 0 && smax > 0) w += mul_div(extra, t->colmax[k], smax);
                else if (extra > 0) w += extra / free_cols;
                t->colw[k] = w;
            }
        } else if (rem <= smin) {
            for (int k = 0; k < ncols; k++) {
                if (t->colw[k]) continue;
                int w = smin > 0 ? mul_div(rem, t->colmin[k], smin) : rem / free_cols;
                if (w < 4) w = 4;
                t->colw[k] = w;
            }
        } else {
            int span = smax - smin;
            for (int k = 0; k < ncols; k++) {
                if (t->colw[k]) continue;
                int w = t->colmin[k] + (span > 0 ? mul_div(t->colmax[k] - t->colmin[k], rem - smin, span) : 0);
                t->colw[k] = w;
            }
        }
    } else if (rem > 0) {
        for (int k = 0; k < ncols; k++) t->colw[k] += mul_div(rem, t->colw[k], fixed > 0 ? fixed : 1);
    }
    /* integer slack goes to the widest column so the table fills `avail` */
    int total = sp * (ncols + 1);
    for (int k = 0; k < ncols; k++) total += t->colw[k];
    int widest = 0;
    for (int k = 1; k < ncols; k++) if (t->colw[k] > t->colw[widest]) widest = k;
    if (total < avail && pref >= avail) t->colw[widest] += avail - total;
    else if (total > avail && t->colw[widest] > avail - total + 4) t->colw[widest] += avail - total;
    return pref;
}

static void shift_range_to(int a, int b, int dx, int dy) {
    if (!dx && !dy) return;
    for (int i = a; i < b && i < g_box_n; i++) {
        g_box[i].x = (int16_t)clamp16(g_box[i].x + dx);
        g_box[i].y = clampy(g_box[i].y + dy);
        if (g_box[i].clip) {
            ClipRect c = g_clip_tab[g_box[i].clip - 1];
            g_box[i].clip = (uint8_t)clip_intern(clamp16(c.x + dx), clampy(c.y + dy), c.w, c.h);
        }
    }
}

/* Stretch a placed cell to `new_h`: its own background/border boxes grow and
   its content moves for vertical-align. */
static void tbl_finish_cell(TCell *c, int new_h) {
    int old_h = c->h;
    if (new_h <= old_h) return;
    int own_end = c->box_a + c->own;
    for (int b = c->box_a; b < own_end && b < g_box_n; b++) {
        RbBox *bx = &g_box[b];
        if (bx->y == c->y0 && bx->h == old_h) bx->h = new_h;
        else if (bx->y + bx->h == c->y0 + old_h && bx->h < old_h) bx->y = bx->y + new_h - old_h;
    }
    int dy = 0;
    if (c->valign == 2) dy = (new_h - old_h) / 2;
    else if (c->valign == 3) dy = new_h - old_h;
    if (dy) shift_range_to(own_end, c->box_b, 0, dy);
    c->h = new_h;
}

/* Lay the table out; returns its content height. (cx, cy, cw): the content box. */
static int layout_table(int node, const Style *tst, int cx, int cy, int cw, RbColor ic, uint8_t isc, uint8_t ia) {
    Tbl t;
    int sp = tst->collapse ? 0 : tst->bspace;
    int spy = g_web ? (sp + 1) / 2 : sp;      /* spacing between rows is vertical */
    int base_tc = g_tc_top, base_tr = g_tr_top, base_tg = g_tg_top;
    tbl_setup(&t, node, tst, cw, ic, isc, ia);
    g_tc_top = t.tc0 + t.nc;
    g_tr_top = t.tr0 + t.nr;
    g_tg_top = t.tg0 + t.ng;
    int y = cy;
    if (t.caption) {
        Style cps = compute_style(t.caption, tst->color, tst->scale, tst->align);
        int h = layout_block(t.caption, cx, y, cw, tst->color, tst->scale, tst->align);
        (void)cps;
        y += h;
    }
    y += spy;
    t.colx[0] = cx + sp;
    for (int k = 1; k < t.ncols; k++) t.colx[k] = t.colx[k - 1] + t.colw[k - 1] + sp;
    int tw = sp;
    for (int k = 0; k < t.ncols; k++) tw += t.colw[k] + sp;
    (void)tw;
#ifdef RBROWSE_HOST
    if (getenv("RB_TBLDBG")) {
        fprintf(stderr, "table node %d cw %d sp %d ncols %d:", node, cw, sp, t.ncols);
        for (int k = 0; k < t.ncols; k++) fprintf(stderr, " [%d min %d max %d w %d]", k, t.colmin[k], t.colmax[k], t.colw[k]);
        fprintf(stderr, "\n");
    }
#endif
    int cell_i = 0;
    for (int ri = 0; ri < t.nr; ri++) {
        TRow *r = &g_tr[t.tr0 + ri];
        r->y = y;
        /* group background starts with the group's first row */
        for (int gi = t.tg0; gi < t.tg0 + t.ng; gi++) {
            TGrp *g = &g_tg[gi];
            if (g->first_row == t.tr0 + ri && g->bg_box < 0) {
                Style gs = compute_style(g->node, tst->color, tst->scale, tst->align);
                if (gs.has_bg) g->bg_box = emit_box(cx, y - spy, cw, 0, g->node, gs.color, gs.bg, 1, RB_BOX_BG);
            }
        }
        if (r->node) {
            Style rs = compute_style(r->node, tst->color, tst->scale, tst->align);
            if (rs.has_bg) r->bg_box = emit_box(cx + sp, y, cw - 2 * sp, 0, r->node, rs.color, rs.bg, 1, RB_BOX_BG);
        }
        int row_h = 0;
        int first_cell = cell_i;
        while (cell_i < t.nc && g_tc[t.tc0 + cell_i].row == ri) {
            TCell *c = &g_tc[t.tc0 + cell_i++];
            if (c->cs <= 0) continue;
            int x = t.colx[c->col];
            int w = 0;
            for (int k = c->col; k < c->col + c->cs && k < t.ncols; k++) w += t.colw[k] + (k > c->col ? sp : 0);
            c->y0 = y;
            c->w = w;
            c->box_a = g_box_n;
            int mode = 1;
            if (tst->collapse) { if (c->col > 0) mode |= 2; if (ri > 0) mode |= 4; }
            g_cell_mode = mode;
            g_force_w = w;
            int h = layout_block(c->node, x, y, w, c->ic, c->isc, c->ia);
            g_cell_mode = 0;
            c->h = h;
            c->box_b = g_box_n;
            /* leading boxes of the cell that are its own background/border */
            int own = 0;
            while (c->box_a + own < c->box_b && g_box[c->box_a + own].node == c->node &&
                   (g_box[c->box_a + own].kind == RB_BOX_BG || g_box[c->box_a + own].kind == RB_BOX_BORDER)) own++;
            c->own = own;
            if (c->rs == 1 && h > row_h) row_h = h;
        }
        /* rowspan cells that end in this row must fit the rows they span */
        for (int ci = 0; ci < t.nc; ci++) {
            TCell *c = &g_tc[t.tc0 + ci];
            if (c->cs <= 0 || c->rs <= 1 || c->row + c->rs - 1 != ri) continue;
            int above = 0;
            for (int k = c->row; k < ri; k++) above += g_tr[t.tr0 + k].h + spy;
            int need = c->h - above;
            if (need > row_h) row_h = need;
        }
        r->h = row_h;
        for (int ci = first_cell; ci < cell_i; ci++) {
            TCell *c = &g_tc[t.tc0 + ci];
            if (c->cs > 0 && c->rs == 1) tbl_finish_cell(c, row_h);
        }
        for (int ci = 0; ci < t.nc; ci++) {
            TCell *c = &g_tc[t.tc0 + ci];
            if (c->cs <= 0 || c->rs <= 1 || c->row + c->rs - 1 != ri) continue;
            int span_h = 0;
            for (int k = c->row; k <= ri; k++) span_h += g_tr[t.tr0 + k].h + (k > c->row ? spy : 0);
            tbl_finish_cell(c, span_h);
        }
        if (r->bg_box >= 0) g_box[r->bg_box].h = row_h;
        y += row_h + spy;
        for (int gi = t.tg0; gi < t.tg0 + t.ng; gi++) {
            TGrp *g = &g_tg[gi];
            if (g->last_row == t.tr0 + ri && g->bg_box >= 0) g_box[g->bg_box].h = y - g_box[g->bg_box].y;
        }
    }
    g_tc_top = base_tc; g_tr_top = base_tr; g_tg_top = base_tg;
    return y - cy;
}

/* Preferred (max-content) inner width of a table, for shrink-to-fit. */
static int table_pref_width(int node, const Style *tst, RbColor ic, uint8_t isc, uint8_t ia) {
    Tbl t;
    int base_tc = g_tc_top, base_tr = g_tr_top, base_tg = g_tg_top;
    int pref = tbl_setup(&t, node, tst, 0, ic, isc, ia);
    g_tc_top = base_tc; g_tr_top = base_tr; g_tg_top = base_tg;
    return pref;
}
