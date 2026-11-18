/* rbrowse CSS cascade: user-agent stylesheets, presentational hints, the
   per-node cascade and Style computation. Included by engine.c after
   css_sel.inc. */

/* ---- user-agent stylesheets ------------------------------------------------ */

static const char UA_WEB[] =
    "html,body,address,article,aside,blockquote,center,dd,details,dialog,dir,div,dl,dt,"
    "fieldset,figcaption,figure,footer,form,h1,h2,h3,h4,h5,h6,header,hgroup,hr,legend,"
    "main,menu,nav,ol,p,pre,search,section,summary,ul,xmp,listing,plaintext,textarea{display:block}"
    "li{display:list-item}"
    "table{display:table;border-spacing:2px;box-sizing:border-box}"
    "caption{display:table-caption;text-align:center}"
    "colgroup,col{display:none}"
    "thead,tbody,tfoot{display:table-row-group}"
    "tr{display:table-row}"
    "td,th{display:table-cell;padding:1px}"
    "head,link,meta,script,style,title,base,template,noscript,area,datalist,param,source,track,rp,"
    "frameset,frame,noframes,applet,map{display:none}"
    "dialog:not([open]){display:none}"
    "[hidden]{display:none}"
    "body{margin:8px}"
    "p,dl,ul,ol,pre,figure{margin:1em 0}"
    "blockquote{margin:1em 40px}"
    "figure{margin:1em 40px}"
    "h1{font-size:2em;margin:.67em 0}"
    "h2{font-size:1.5em;margin:.83em 0}"
    "h3{font-size:1.17em;margin:1em 0}"
    "h4{margin:1.33em 0}"
    "h5{font-size:.83em;margin:1.67em 0}"
    "h6{font-size:.67em;margin:2.33em 0}"
    "ul,ol,dir,menu{padding-left:40px}"
    "ul ul,ul ol,ol ul,ol ol,ul dl,ol dl,dl ul,dl ol,dl dl{margin-top:0;margin-bottom:0}"
    "ul{list-style-type:disc}"
    "ul ul{list-style-type:circle}"
    "ul ul ul{list-style-type:square}"
    "ol{list-style-type:decimal}"
    "dd{margin-left:40px}"
    "pre,xmp,listing,plaintext{white-space:pre}"
    "hr{margin:.5em auto;border:1px solid #888;height:0}"
    "b,strong,th,h1,h2,h3,h4,h5,h6,dt{font-weight:bold}"
    "i,em,cite,var,dfn,address{font-style:italic}"
    "u,ins{text-decoration:underline}"
    "s,strike,del{text-decoration:line-through}"
    "small{font-size:.83em}"
    "big{font-size:1.17em}"
    "sub{font-size:.83em;vertical-align:sub}"
    "sup{font-size:.83em;vertical-align:super}"
    "mark{background:#ff0;color:#000}"
    "center{text-align:-webkit-center}"
    "th{text-align:center}"
    "a:link{color:#00e;text-decoration:underline}"
    "q:before{content:'\"'}q:after{content:'\"'}"
    "nobr{white-space:nowrap}"
    "fieldset{margin:0 2px;padding:.35em .75em .6em;border:1px solid #888}"
    "legend{padding:0 2px}"
    "details>summary{display:block}"
    "details:not([open])>:not(summary){display:none !important}"
    "summary:before{content:'+ '}"
    "details[open]>summary:before{content:'- '}"
    "summary{font-weight:bold}"
    "input,textarea,select,button{box-sizing:border-box}"
    "input,textarea,select{background:#fff;color:#000}"
    "button,input:-rb-btn,select{background:#ddd;color:#000;padding:1px 6px}"
    "img,svg,video,canvas,iframe,object,embed,audio{display:inline-block}"
    "audio,source,track{display:none}";

static const char UA_NATIVE[] =
    "html,head,body,title,style,script,div,p,h1,h2,h3,h4,h5,h6,ul,ol,li,hr,pre,form,nav,header,"
    "footer,main,section,article,table,tr,td,th,center,textarea{display:block}"
    "head,style,script,title{display:none}"
    "h1{font-size:20px;margin-bottom:4px}"
    "h2,h3{margin-bottom:3px}"
    "p{margin-bottom:4px}"
    "li{margin-left:8px}"
    "a{color:#ff7722}"
    "input,textarea{background:#f0e8dc;color:#14100c;padding:1px}"
    "button,input:-rb-btn{background:#5c2200;color:#ffccaa;padding:2px}"
    "[hidden]{display:none}"
    "body{background:#663c22;color:#ffe6c8}"
    "a,u{text-decoration:underline}"
    "b,strong,th,h1,h2,h3,h4,h5,h6{font-weight:bold}"
    "center{text-align:center}"
    "b,strong{color:#ffdcbe}";

static int g_ua_profile = -1;
static int g_decl_ua, g_atom_ua, g_aux_ua;

/* (Re)build every CSS table from scratch with the UA sheet of a profile. */
static void css_ua_init(int web) {
    g_web = web;
    g_fs_thresh = web ? 26 : 20;
    g_istr_n = 0;
    g_ient_n = 0;
    memset(g_ih, 0, sizeof(g_ih));
    g_atom_n = 0;
    g_crule_n = 0;
    g_decl_n = 0;
    g_aux_n = 0;
    g_var_n = 0;
    g_import_n = 0;
    g_sheet_ord = 0;
    g_sheet_idx = 0;
    g_cur_origin = 0;
    int save_oom = g_oom;
    parse_sheet(web ? UA_WEB : UA_NATIVE, (int)strlen(web ? UA_WEB : UA_NATIVE), 0);
    g_oom = save_oom;
    g_crule_ua = g_crule_n;
    g_atom_ua = g_atom_n;
    g_decl_ua = g_decl_n;
    g_aux_ua = g_aux_n;
    g_ient_ua = g_ient_n;
    g_istr_ua = g_istr_n;
    g_var_ua_n = g_var_n;
    g_ua_profile = web;
    g_cur_origin = 1;
    g_sheet_ord = SHEET_STEP;
    rules_index();
}

/* Drop author rules, strings and variables; keep the UA sheet. */
static void css_doc_reset(void) {
    g_crule_n = g_crule_ua;
    g_atom_n = g_atom_ua;
    g_decl_n = g_decl_ua;
    g_aux_n = g_aux_ua;
    g_var_n = g_var_ua_n;
    g_import_n = 0;
    g_ient_n = g_ient_ua;
    g_istr_n = g_istr_ua;
    intern_rebuild_hash();
    g_sheet_ord = SHEET_STEP;
    g_sheet_idx = 0;
    g_cur_origin = 1;
}

/* ---- per-node computed bookkeeping ----------------------------------------------- */

static uint16_t g_fpx[MAX_NODES];     /* computed font-size in CSS px (0 = not yet computed) */
static uint8_t g_lstype[MAX_NODES];   /* computed list-style-type: inherited, so children read it from here */

/* ---- presentational hints --------------------------------------------------------- */

static int attr_px_dim(const char *v, int vn, int32_t *out, int vertical) {
    /* "50", "50px", "50%" -> size encoding */
    int pct = 0, i = 0, num = 0, any = 0;
    while (i < vn && v[i] == ' ') i++;
    while (i < vn && is_digit_c(v[i])) { num = num * 10 + (v[i] - '0'); if (num > 100000) num = 100000; i++; any = 1; }
    if (!any) return 0;
    if (i < vn && v[i] == '.') { i++; while (i < vn && is_digit_c(v[i])) i++; }
    if (i < vn && v[i] == '%') pct = 1;
    else if (i + 1 < vn && v[i] == 'e' && v[i + 1] == 'm') num *= 16;       /* width="7em" (svg): 1em = 16px */
    if (pct) { *out = -(num > 100 ? 100 : num); if (num == 0) *out = 0; }
    else { int px = num > 30000 ? 30000 : num; *out = vertical ? zy(px) : px; }
    return 1;
}

static int font_size_attr(const char *v, int vn) {
    static const int px[8] = { 10, 10, 13, 16, 18, 24, 32, 48 };
    int i = 0, sign = 0;
    if (vn > 0 && (v[0] == '+' || v[0] == '-')) { sign = v[0] == '-' ? -1 : 1; i = 1; }
    int num = 0, any = 0;
    while (i < vn && is_digit_c(v[i])) { num = num * 10 + (v[i] - '0'); i++; any = 1; }
    if (!any) return 0;
    int lvl = sign ? 3 + sign * num : num;
    if (lvl < 1) lvl = 1;
    if (lvl > 7) lvl = 7;
    return px[lvl];
}

static int table_ancestor(int node) {
    for (int p = g_nodes[node].parent; p; p = g_nodes[p].parent)
        if (g_nodes[p].tag == RB_TAG_TABLE) return p;
    return 0;
}

#define MAX_HINTS 24

/* Decls implied by legacy presentation attributes. Emitted with the
   author-origin / zero-specificity key, i.e. below every author rule. */
static int hint_decls(int node, Decl *buf) {
    Decl *save_out = g_dout; int *save_n = g_dn; int save_cap = g_dcap;
    int cnt = 0;
    g_dout = buf; g_dn = &cnt; g_dcap = MAX_HINTS;
    int tag = g_nodes[node].tag;
    const char *v; int vn;
    int c;
    if (attr_get(node, "bgcolor", 7, &v, &vn) && parse_color(v, vn, &c) && c >= 0) emit_decl(P_BG, c, 0);
    if (tag == RB_TAG_BODY) {
        if (attr_get(node, "text", 4, &v, &vn) && parse_color(v, vn, &c) && c >= 0) emit_decl(P_COLOR, c, 0);
        /* leftmargin/topmargin/marginwidth: legacy body insets */
        if (attr_get(node, "marginwidth", 11, &v, &vn) || attr_get(node, "leftmargin", 10, &v, &vn)) {
            int32_t d; if (attr_px_dim(v, vn, &d, 0) && d >= 0) { emit_decl(P_ML, d, 0); emit_decl(P_MR, d, 0); }
        }
        if (attr_get(node, "marginheight", 12, &v, &vn) || attr_get(node, "topmargin", 9, &v, &vn)) {
            int32_t d; if (attr_px_dim(v, vn, &d, 1) && d >= 0) { emit_decl(P_MT, d, 0); emit_decl(P_MB, d, 0); }
        }
    }
    if (tag == RB_TAG_FONT) {
        if (attr_get(node, "color", 5, &v, &vn) && parse_color(v, vn, &c) && c >= 0) emit_decl(P_COLOR, c, 0);
        if (attr_get(node, "size", 4, &v, &vn)) { int px = font_size_attr(v, vn); if (px) emit_decl(P_FSIZE, px, 0); }
    }
    if (attr_get(node, "align", 5, &v, &vn)) {
        int k = -1;
        if (eq_lit(v, vn, "center") || eq_lit(v, vn, "middle")) k = 1;
        else if (eq_lit(v, vn, "right")) k = 2;
        else if (eq_lit(v, vn, "left")) k = 0;
        if (k >= 0) {
            if (tag == RB_TAG_TABLE || tag == RB_TAG_IMG) {
                if (k == 1 && tag == RB_TAG_TABLE) { emit_decl(P_ML, OFF_AUTO, 0); emit_decl(P_MR, OFF_AUTO, 0); }
                else if (k == 2) emit_decl(P_FLOAT, 2, 0);
                else if (k == 0 && tag == RB_TAG_IMG) emit_decl(P_FLOAT, 1, 0);
            } else emit_decl(P_TEXTALIGN, k, 0);
        }
    }
    if (tag == RB_TAG_TD || tag == RB_TAG_TH || tag == RB_TAG_TR || tag == RB_TAG_TBODY ||
        tag == RB_TAG_THEAD || tag == RB_TAG_TFOOT) {
        if (attr_get(node, "valign", 6, &v, &vn)) {
            if (eq_lit(v, vn, "top")) emit_decl(P_VALIGN, 1, 0);
            else if (eq_lit(v, vn, "middle")) emit_decl(P_VALIGN, 2, 0);
            else if (eq_lit(v, vn, "bottom")) emit_decl(P_VALIGN, 3, 0);
        }
    }
    if (tag == RB_TAG_TD || tag == RB_TAG_TH) {
        if (attr_get(node, "nowrap", 6, &v, &vn)) emit_decl(P_WS, WS_NOWRAP, 0);
        int t = table_ancestor(node);
        if (t && attr_get(t, "cellpadding", 11, &v, &vn)) {
            int32_t dx, dy;
            if (attr_px_dim(v, vn, &dx, 0) && dx >= 0 && attr_px_dim(v, vn, &dy, 1))
                for (int k = 0; k < 4; k++) emit_decl(P_PT + k, (k & 1) ? dx : dy, 0);
        }
        if (t && attr_get(t, "border", 6, &v, &vn)) {
            int num = 0; int i2 = 0;
            while (i2 < vn && is_digit_c(v[i2])) { num = num * 10 + (v[i2] - '0'); i2++; }
            if (i2 == 0 && vn == 0) num = 1;
            if (num > 0) for (int k = 0; k < 4; k++) { emit_decl(P_BWT + k, 1, 0); emit_decl(P_BST + k, 1, 0); emit_decl(P_BCT + k, rb_map_rgb(128, 128, 128), 0); }
        }
    }
    if (tag == RB_TAG_TABLE) {
        if (attr_get(node, "cellspacing", 11, &v, &vn)) {
            int32_t d;
            if (attr_px_dim(v, vn, &d, 0) && d >= 0) emit_decl(P_BSPACE, d > 32 ? 32 : d, 0);
        }
        if (attr_get(node, "border", 6, &v, &vn)) {
            int num = 0; int i2 = 0;
            while (i2 < vn && is_digit_c(v[i2])) { num = num * 10 + (v[i2] - '0'); i2++; }
            if (i2 == 0 && vn == 0) num = 1;
            if (num > 0) {
                int wx = num > 8 ? 8 : num, wy = zy(num); if (wx < 1) wx = 1; if (wy < 1) wy = 1; if (wy > 8) wy = 8;
                for (int k = 0; k < 4; k++) { emit_decl(P_BWT + k, (k & 1) ? wx : wy, 0); emit_decl(P_BST + k, 1, 0); emit_decl(P_BCT + k, rb_map_rgb(128, 128, 128), 0); }
            }
        }
    }
    if (tag == RB_TAG_TABLE || tag == RB_TAG_TD || tag == RB_TAG_TH || tag == RB_TAG_IMG ||
        tag == RB_TAG_HR || tag == RB_TAG_COL || tag == RB_TAG_IFRAME || tag == RB_TAG_VIDEO ||
        (tag == RB_TAG_CANVAS && !g_canvas) || tag == RB_TAG_OBJECT || tag == RB_TAG_EMBED || tag == RB_TAG_SVG) {
        /* (in canvas mode a canvas's width/height are its intrinsic size) */
        int32_t d;
        if (attr_get(node, "width", 5, &v, &vn) && attr_px_dim(v, vn, &d, 0) && d != 0) emit_decl(P_WIDTH, d, 0);
        if (attr_get(node, "height", 6, &v, &vn) && attr_px_dim(v, vn, &d, 1) && d != 0) emit_decl(P_HEIGHT, d, 0);
    }
    if (tag == RB_TAG_UL || tag == RB_TAG_OL || tag == RB_TAG_LI) {
        if (attr_get(node, "type", 4, &v, &vn)) {
            int t = -1;
            if (vn == 1) t = v[0] == '1' ? LST_DECIMAL : v[0] == 'a' ? LST_LALPHA : v[0] == 'A' ? LST_UALPHA :
                             v[0] == 'i' ? LST_LROMAN : v[0] == 'I' ? LST_UROMAN : -1;
            else if (eq_lit(v, vn, "disc")) t = LST_DISC;
            else if (eq_lit(v, vn, "circle")) t = LST_CIRCLE;
            else if (eq_lit(v, vn, "square")) t = LST_SQUARE;
            if (t >= 0) emit_decl(P_LSTYPE, t, 0);
        }
    }
    g_dout = save_out; g_dn = save_n; g_dcap = save_cap;
    return cnt;
}

/* ---- applying declarations ----------------------------------------------------------- */

typedef struct {
    int fs_has;
    int32_t fs_val;
    int fs_rem;                 /* font-size written in rem: thousandths of the root font-size, else 0 */
    int16_t em[P_COUNT];        /* per property: thousandths of em (or rem when rem[] is set) of the winning length */
    uint8_t rem[P_COUNT];
    int bw[4];
    int bs[4];
    int bc_cur[4];
    int bg_cur;
    int content;
    int has_content;
    int disp_author;
} CascTmp;

static int m_px(int v, int basis) {
    if (v >= OFF_PCT - 200 && v <= OFF_PCT + 200) return basis * (v - OFF_PCT) / 100;
    return v;
}

static void apply_decl(Style *st, CascTmp *t, const Decl *d, int ic, int author) {
    int32_t v = d->val;
    t->em[d->prop] = d->kind ? 0 : d->em;
    t->rem[d->prop] = d->rem;
    if (d->kind == 1) {      /* inherit */
        if (d->prop == P_COLOR) st->color = ic;
        return;
    }
    switch (d->prop) {
    case P_COLOR: st->color = (RbColor)v; break;
    case P_BG:
        if (v == COL_NONE) st->has_bg = 0;
        else if (v == COL_CURRENT) { t->bg_cur = 1; st->has_bg = 1; }
        else { st->bg = (RbColor)v; st->has_bg = 1; t->bg_cur = 0; }
        break;
    case P_FSIZE: t->fs_has = 1; t->fs_val = v; t->fs_rem = d->rem ? d->em : 0; break;
    case P_DISPLAY: st->display = (uint8_t)v; if (author) t->disp_author = 1; break;
    case P_TEXTALIGN: st->align = (uint8_t)v; break;
    case P_MT: st->mt = (int16_t)(v == OFF_AUTO ? 0 : v); break;
    case P_MB: st->mb = (int16_t)(v == OFF_AUTO ? 0 : v); break;
    case P_ML: st->ml = (int16_t)(v == OFF_AUTO ? 0 : v); st->x.ml_auto = v == OFF_AUTO; break;
    case P_MR: st->mr = (int16_t)(v == OFF_AUTO ? 0 : v); st->x.mr_auto = v == OFF_AUTO; break;
    case P_PT: st->pt = (int16_t)v; break;
    case P_PR: st->pr = (int16_t)v; break;
    case P_PB: st->pb = (int16_t)v; break;
    case P_PL: st->pl = (int16_t)v; break;
    case P_WIDTH: case P_MINW: case P_MAXW: {
        int base = v, adj = 0;
        if (d->prop == P_MINW) st->x.minw0 = (v == MINW_ZERO);
        if (v == MINW_ZERO) base = 0;
        else if (v >= 0 && (v & (1 << 30))) { base = -(v & 0xFF); adj = ((v >> 8) & 0x3FFF) - 8192; }
        else if (v > 32767) base = 32767;
        /* width:0 must not read as "unset" (0 is falsy all over layout): use the thinnest real width */
        if (d->prop == P_WIDTH) { st->width = (int16_t)(base == 0 && adj == 0 ? 1 : base); st->wadj = (int16_t)adj; }
        else if (d->prop == P_MINW) { st->min_w = (int16_t)base; st->minw_adj = (int16_t)adj; }
        else { st->max_w = (int16_t)base; st->maxw_adj = (int16_t)adj; }
        break;
    }
    case P_HEIGHT: st->height = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); break;
    case P_MINH: st->min_h = (int16_t)(v > 32767 ? 32767 : v); break;
    case P_MAXH: st->max_h = (int16_t)(v > 32767 ? 32767 : v); break;
    case P_FLEXDIR: st->flex_dir = (uint8_t)v; break;
    case P_JUSTIFY: st->justify = (uint8_t)v; break;
    case P_ALIGNITEMS: st->align_items = (uint8_t)v; break;
    case P_ALIGNSELF: st->align_self = (uint8_t)v; break;
    case P_FLEXWRAP: st->flex_wrap = (uint8_t)v; break;
    case P_ALIGNCONTENT: st->align_content = (uint8_t)v; break;
    case P_ROWGAP: st->row_gap = (uint8_t)(v > 255 ? 255 : v); break;
    case P_COLGAP: st->column_gap = (uint8_t)(v > 255 ? 255 : v); break;
    case P_GROW: st->grow = (uint16_t)v; break;
    case P_SHRINK: st->shrink = (uint16_t)v; break;
    case P_BASIS: st->basis = (int16_t)v; break;
    case P_ORDER: st->order = (int16_t)v; break;
    case P_POS: st->pos = (uint8_t)v; break;
    case P_TOP: st->top = (int16_t)v; break;
    case P_RIGHT: st->right = (int16_t)v; break;
    case P_BOTTOM: st->bottom = (int16_t)v; break;
    case P_LEFT: st->left = (int16_t)v; break;
    case P_ZINDEX:
        if (v == 0x7FFFFFFF) { st->z_auto = 1; st->z_index = 0; }
        else { st->z_auto = 0; st->z_index = (int16_t)(v > 32000 ? 32000 : (v < -32000 ? -32000 : v)); }
        break;
    case P_OVERFLOW: st->overflow = (uint8_t)v; break;
    case P_VISIBILITY: st->visibility = (uint8_t)v; break;
    case P_OPACITY: st->opacity = (uint8_t)v; break;
    case P_CURSOR: st->cursor = (uint8_t)v; break;
    case P_BOXSZ: st->x.boxsz = (uint8_t)v; break;
    case P_BWT: case P_BWR: case P_BWB: case P_BWL: t->bw[d->prop - P_BWT] = v; break;
    case P_BST: case P_BSR: case P_BSB: case P_BSL: t->bs[d->prop - P_BST] = v; break;
    case P_BCT: case P_BCR: case P_BCB: case P_BCL: st->x.bcol[d->prop - P_BCT] = (RbColor)v; break;
    case P_RADIUS: st->x.radius = (uint8_t)v; break;
    case P_WEIGHT: st->x.weight = (uint8_t)v; break;
    case P_DECOR: st->x.decor = (uint8_t)v; break;
    case P_TRANSFORM: st->x.transform = (uint8_t)v; break;
    case P_WS: st->x.ws = (uint8_t)v; break;
    case P_LINEH: st->x.lineh = (int16_t)v; break;
    case P_LETSP: st->x.letsp = (uint8_t)v; break;
    case P_GCOLS: {
        const int16_t *a = &g_aux[v];
        st->x.gn = (uint8_t)a[0];
        st->x.gautokind = (uint8_t)a[1];
        st->x.gautomin = (uint16_t)a[2];
        for (int k = 0; k < GRID_MAX_TRACKS; k++) st->x.gtrack[k] = k < a[0] ? a[3 + k] : 0;
        break;
    }
    case P_GAREAS: st->x.gareas = (int16_t)v; break;
    case P_GAREA: st->x.garea = (uint16_t)v; break;
    case P_JITEMS: st->x.jitems = (uint8_t)v; break;
    case P_JSELF: st->x.jself = (uint8_t)v; break;
    case P_GCOL: st->x.gcs = (int16_t)(v & 0xFFFF); st->x.gce = (int16_t)((uint32_t)v >> 16); break;
    case P_GCOLSPAN: st->x.gcspan = (uint8_t)v; break;
    case P_GROWL: st->x.grs = (int16_t)(v & 0xFFFF); st->x.gre = (int16_t)((uint32_t)v >> 16); break;
    case P_GROWSPAN: st->x.grspan = (uint8_t)v; break;
    case P_FLOAT: st->flt = (uint8_t)v; break;
    case P_CLEAR: st->clr = (uint8_t)v; break;
    case P_VALIGN: st->valign = (uint8_t)v; break;
    case P_INDENT: st->indent = (int16_t)v; break;
    case P_LSTYPE: st->lstype = (uint8_t)v; break;
    case P_COLLAPSE: st->collapse = (uint8_t)v; break;
    case P_BSPACE: st->bspace = (uint8_t)v; break;
    case P_CONTENT: t->content = v; t->has_content = 1; break;
    case P_ITALIC: st->italic = (uint8_t)v; break;
    case P_TX: st->tx = v; break;
    case P_TY: st->ty = v; break;
    default: break;
    }
}

/* ---- the cascade ----------------------------------------------------------------------- */

#define MAX_MATCH 640
static uint16_t g_match[MAX_MATCH];
static int g_match_n;

static void match_chain(int node, int head, int pseudo) {
    for (int r = head; r; r = g_crule[r - 1].next) {
        CRule *cr = &g_crule[r - 1];
        if (cr->pseudo != pseudo) continue;
        g_match_budget = 400;
        if (!match_complex(node, (int)cr->sel)) continue;
        if (g_match_n < MAX_MATCH) g_match[g_match_n++] = (uint16_t)(r - 1);
    }
}

/* Collect rules matching node, sorted by (order). */
static void collect_matches(int node, int pseudo) {
    g_match_n = 0;
    Node *nd = &g_nodes[node];
    match_chain(node, g_rh_any, pseudo);
    match_chain(node, g_rh_tag[nd->tag], pseudo);
    if (nd->id_len) {
        int id = intern(g_pool + nd->id_off, (int)nd->id_len, 0);
        if (id >= 0) match_chain(node, g_rh_id[id], pseudo);
    }
    if (nd->class_len) {
        const char *cl = g_pool + nd->class_off;
        int ln = (int)nd->class_len, i = 0;
        while (i < ln) {
            while (i < ln && (cl[i] == ' ' || cl[i] == '\t' || cl[i] == '\n')) i++;
            int st = i;
            while (i < ln && cl[i] != ' ' && cl[i] != '\t' && cl[i] != '\n') i++;
            if (i > st) {
                int id = intern(cl + st, i - st, 0);
                if (id >= 0) match_chain(node, g_rh_cls[id], pseudo);
            }
        }
    }
    /* insertion sort by order; a rule can appear twice only if it hashed under
       two keys, which the index never does */
    for (int i = 1; i < g_match_n; i++) {
        uint16_t x = g_match[i];
        uint32_t ox = g_crule[x].order;
        int j = i - 1;
        while (j >= 0 && g_crule[g_match[j]].order > ox) { g_match[j + 1] = g_match[j]; j--; }
        g_match[j + 1] = x;
    }
}

static void style_defaults(Style *st, int ic, int ia) {
    memset(st, 0, sizeof(*st));
    st->color = ic;
    st->align = (uint8_t)ia;
    st->display = DISP_INLINE;
    st->width = -32768;
    st->height = -32768;
    st->flex_dir = FLEX_ROW;
    st->justify = JUSTIFY_START;
    st->align_items = ALIGN_STRETCH;
    st->align_self = ALIGN_AUTO;
    st->flex_wrap = FLEX_NOWRAP;
    st->align_content = ACNT_STRETCH;
    st->shrink = FLEX_SCALE;
    st->basis = -32768;
    st->pos = POS_STATIC;
    st->overflow = OVF_VISIBLE;
    st->visibility = VIS_VISIBLE;
    st->cursor = CUR_AUTO;
    st->opacity = 255;
    st->z_auto = 1;
    st->top = st->right = st->bottom = st->left = OFF_AUTO;
    st->x.lineh = LH_UNSET;
    st->x.letsp = LS_UNSET;
    st->x.jitems = JI_STRETCH;
    for (int k = 0; k < 4; k++) st->x.bcol[k] = BC_CUR;
    st->x.boxsz = g_web ? BS_CONTENT : BS_LEGACY;
    st->lstype = 0;
    st->bspace = 0;
}

/* Cascade for one element. `pseudo` selects ::before/::after rules (0 for
   the element itself). */
static void cascade_into(int node, int pseudo, Style *st, CascTmp *t, int ic) {
    uint32_t pk[P_COUNT];
    memset(pk, 0, sizeof(pk));
    collect_matches(node, pseudo);
    int i = 0;
    /* UA rules first */
    for (; i < g_match_n && g_crule[g_match[i]].origin == 0; i++) {
        CRule *cr = &g_crule[g_match[i]];
        for (int k = 0; k < cr->ndecl; k++) {
            const Decl *d = &g_decl[cr->decl + k];
            uint32_t key = (d->imp ? (1u << 30) : 0) | cr->spec;
            if (key >= pk[d->prop]) { apply_decl(st, t, d, ic, 0); pk[d->prop] = key; }
        }
    }
    if (!pseudo) {
        Decl hint[MAX_HINTS];
        int hn = hint_decls(node, hint);
        for (int k = 0; k < hn; k++) {
            uint32_t key = (1u << 29);
            if (key >= pk[hint[k].prop]) { apply_decl(st, t, &hint[k], ic, 1); pk[hint[k].prop] = key; }
        }
    }
    for (; i < g_match_n; i++) {
        CRule *cr = &g_crule[g_match[i]];
        for (int k = 0; k < cr->ndecl; k++) {
            const Decl *d = &g_decl[cr->decl + k];
            uint32_t key = (d->imp ? (1u << 30) : 0) | (cr->origin == 2 ? (1u << 28) : (1u << 29)) | cr->spec;
            if (key >= pk[d->prop]) { apply_decl(st, t, d, ic, 1); pk[d->prop] = key; }
        }
    }
    /* quirks mode: a table starts a fresh text alignment */
    if (g_web && g_quirks && g_nodes[node].tag == RB_TAG_TABLE && !pseudo && pk[P_TEXTALIGN] == 0) st->align = 0;
    /* a table (hence its cells) never inherits the -webkit-center of a <center> */
    if (g_web && g_nodes[node].tag == RB_TAG_TABLE && !pseudo && st->align == 3) st->align = 0;
    if (!pseudo && g_nodes[node].style_len) {
        Decl inl[160];
        int cnt = 0;
        int aux_mark = g_aux_n;                 /* grid tracks parsed here are scratch */
        Decl *so = g_dout; int *sn = g_dn; int sc = g_dcap;
        g_dout = inl; g_dn = &cnt; g_dcap = 160;
        int sv = g_var_capture;
        g_var_capture = 0;
        parse_decl_list(g_pool + g_nodes[node].style_off, (int)g_nodes[node].style_len);
        g_var_capture = sv;
        g_dout = so; g_dn = sn; g_dcap = sc;
        for (int k = 0; k < cnt; k++) {
            uint32_t key = (inl[k].imp ? (1u << 30) : 0) | (1u << 29) | (1u << 22);
            if (key >= pk[inl[k].prop]) { apply_decl(st, t, &inl[k], ic, 1); pk[inl[k].prop] = key; }
        }
        g_aux_n = aux_mark;
    }
}

/* Computed font-size of the root element (what rem means); 16 for the root itself. */
static int root_fpx(int node) {
    int n = node, p;
    while ((p = g_nodes[n].parent) != 0 && g_nodes[p].tag != RB_TAG_DOC) n = p;
    if (n == node || !g_fpx[n]) return 16;
    return g_fpx[n];
}

static int em_round(int mil, int fpx) {      /* mil/1000 * fpx, rounded to nearest */
    int p = mil * fpx;                       /* |mil| <= 32767 and fpx <= 400: fits */
    return p >= 0 ? (p + 500) / 1000 : -((-p + 500) / 1000);
}

static int sat_i(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Lengths written in em/rem were parsed against a 16px font; redo them against the
   element's computed font-size (em) or the root's (rem). Vertical ones are halved
   like every vertical length (zy). */
static void em_fix(int node, Style *st, const CascTmp *t, int fpx) {
    static const uint8_t props[] = { P_MT, P_MR, P_MB, P_ML, P_PT, P_PR, P_PB, P_PL,
        P_WIDTH, P_MINW, P_MAXW, P_HEIGHT, P_MINH, P_MAXH, P_TOP, P_RIGHT, P_BOTTOM, P_LEFT,
        P_INDENT, P_ROWGAP, P_COLGAP };
    int rootpx = -1;
    for (unsigned k = 0; k < sizeof(props); k++) {
        int pr = props[k], em = t->em[pr];
        if (!em) continue;
        int base = fpx;
        if (t->rem[pr]) { if (rootpx < 0) rootpx = root_fpx(node); base = rootpx; }
        int px = em_round(em, base);
        switch (pr) {
        case P_PT: case P_PB: case P_MT: case P_MB: case P_TOP: case P_BOTTOM:
        case P_HEIGHT: case P_MINH: case P_MAXH: case P_ROWGAP:
            px = zy(px); break;
        default: break;
        }
        switch (pr) {
        case P_MT: st->mt = (int16_t)sat_i(px, -8000, 8000); break;
        case P_MR: st->mr = (int16_t)sat_i(px, -8000, 8000); break;
        case P_MB: st->mb = (int16_t)sat_i(px, -8000, 8000); break;
        case P_ML: st->ml = (int16_t)sat_i(px, -8000, 8000); break;
        case P_PT: st->pt = (int16_t)sat_i(px, 0, 8000); break;
        case P_PR: st->pr = (int16_t)sat_i(px, 0, 8000); break;
        case P_PB: st->pb = (int16_t)sat_i(px, 0, 8000); break;
        case P_PL: st->pl = (int16_t)sat_i(px, 0, 8000); break;
        case P_WIDTH: st->width = (int16_t)sat_i(px, 1, 30000); break;      /* 0 would read as unset */
        case P_MINW: st->min_w = (int16_t)sat_i(px, 0, 30000); break;
        case P_MAXW: st->max_w = (int16_t)sat_i(px, 0, 30000); break;
        case P_HEIGHT: st->height = (int16_t)sat_i(px, 0, 30000); break;
        case P_MINH: st->min_h = (int16_t)sat_i(px, 0, 30000); break;
        case P_MAXH: st->max_h = (int16_t)sat_i(px, 0, 30000); break;
        case P_TOP: st->top = (int16_t)sat_i(px, -8000, 8000); break;
        case P_RIGHT: st->right = (int16_t)sat_i(px, -8000, 8000); break;
        case P_BOTTOM: st->bottom = (int16_t)sat_i(px, -8000, 8000); break;
        case P_LEFT: st->left = (int16_t)sat_i(px, -8000, 8000); break;
        case P_INDENT: st->indent = (int16_t)sat_i(px, -400, 400); break;
        case P_ROWGAP: st->row_gap = (uint8_t)sat_i(px, 0, 255); break;
        case P_COLGAP: st->column_gap = (uint8_t)sat_i(px, 0, 255); break;
        default: break;
        }
    }
}

static int parent_fpx(int node, int isc) {
    int p = g_nodes[node].parent;
    if (p && g_fpx[p]) return g_fpx[p];
    return isc >= 2 ? 32 : 16;
}

/* Fold the cascade temp state into the Style the layout code reads. */
static void style_finish(int node, Style *st, CascTmp *t, int isc) {
    /* font size -> scale */
    int pf = parent_fpx(node, isc);
    int fpx = pf;
    if (t->fs_has) {
        if (t->fs_val >= 0) fpx = t->fs_rem ? em_round(t->fs_rem, root_fpx(node)) : t->fs_val;
        else fpx = (pf * (-t->fs_val) + 500) / 1000;    /* tenths of a percent of the parent's */
        if (fpx < 1) fpx = 1;
        if (fpx > 400) fpx = 400;
    }
    if (g_web) em_fix(node, st, t, fpx);
    st->fpx = (uint16_t)fpx;
    g_fpx[node] = (uint16_t)fpx;
    {
        int pp = g_nodes[node].parent;
        if (!st->lstype && pp) st->lstype = g_lstype[pp];
        g_lstype[node] = st->lstype;
    }
    st->scale = (uint8_t)(fpx >= g_fs_thresh ? 2 : 1);
    if (t->bg_cur) st->bg = st->color;
    /* borders: a side has width only with a visible style */
    for (int k = 0; k < 4; k++) {
        int w = t->bs[k] ? t->bw[k] : 0;
        if (w > 8) w = 8;
        st->x.bw[k] = (uint8_t)w;
    }
    /* percentages that need a containing block resolve against the viewport */
    int pad[4] = { st->pt, st->pr, st->pb, st->pl };
    for (int k = 0; k < 4; k++) {
        pad[k] = m_px(pad[k], RB_VIEW_W);
        if (pad[k] < 0) pad[k] = 0;
        if (pad[k] > 2000) pad[k] = 2000;
    }
    st->mt = (int16_t)m_px(st->mt, RB_VIEW_W);
    st->mb = (int16_t)m_px(st->mb, RB_VIEW_W);
    /* the content inset: padding plus border, per side */
    st->pt = (int16_t)(pad[0] + st->x.bw[0]);
    st->pr = (int16_t)(pad[1] + st->x.bw[1]);
    st->pb = (int16_t)(pad[2] + st->x.bw[2]);
    st->pl = (int16_t)(pad[3] + st->x.bw[3]);
    /* floats and absolutely positioned boxes are block-level */
    int disp = st->display;
    if ((st->flt || st->pos == POS_ABSOLUTE || st->pos == POS_FIXED) &&
        (disp == DISP_INLINE || disp == DISP_INLINE_BLOCK)) disp = DISP_BLOCK;
    if (disp == DISP_INLINE_FLEX && (!g_web || st->flt || st->pos == POS_ABSOLUTE || st->pos == POS_FIXED)) disp = DISP_FLEX;
    if (!g_web) {
        /* the native profile never had these display types */
        if (disp == DISP_INLINE_BLOCK || disp == DISP_CONTENTS) disp = DISP_INLINE;
        else if (disp == DISP_LIST_ITEM || (disp >= DISP_TABLE && disp <= DISP_TABLE_CAPTION)) disp = DISP_BLOCK;
    }
    /* <table style="display:block"> (responsive-table CSS) still holds an anonymous
       table around its rows; keep laying the rows out as a table, not stacked blocks */
    if (g_web && disp == DISP_BLOCK && g_nodes[node].tag == RB_TAG_TABLE && t->disp_author) disp = DISP_TABLE;
    st->display = (uint8_t)disp;
    int hs = st->pl + st->pr, vs = st->pt + st->pb;
    if (st->x.boxsz == BS_CONTENT) {
        if (st->width > 0) { int w = st->width + hs; st->width = (int16_t)(w > 30000 ? 30000 : w); }
        if (st->min_w > 0) st->min_w = (int16_t)(st->min_w + hs);
        if (st->max_w > 0) st->max_w = (int16_t)(st->max_w + hs);
    } else if (st->x.boxsz == BS_BORDER) {
        if (st->height > 0) { int h = st->height - vs; st->height = (int16_t)(h < 1 ? 1 : h); }
        if (st->min_h > 0) { int h = st->min_h - vs; st->min_h = (int16_t)(h < 1 ? 1 : h); }
        if (st->max_h > 0) { int h = st->max_h - vs; st->max_h = (int16_t)(h < 1 ? 1 : h); }
    }
    if (t->disp_author && st->display == DISP_BLOCK) st->forced_block = 1;
    if (g_nodes[node].hidden) st->display = DISP_NONE;
}

static Style compute_style(int node, RbColor inherit_color, uint8_t inherit_scale, uint8_t inherit_align) {
    /* Memo per node inside one layout pass. Every layout/flow/focus caller
       passes the same inherited triple for a node, so a hit equals the
       recompute; the g_nodes[node].color side effect is kept on both paths. */
    StyleCache *c = &g_style_cache[node];
    if (c->valid && c->ic == inherit_color && c->isc == inherit_scale && c->ia == inherit_align) {
        g_style_hits++;
        g_nodes[node].color = c->st.color;
        g_fpx[node] = c->st.fpx;
        g_lstype[node] = c->st.lstype;
        return c->st;
    }
    g_style_misses++;
    Style st;
    CascTmp t;
    memset(&t, 0, sizeof(t));
    for (int k = 0; k < 4; k++) { t.bw[k] = (k & 1) ? 3 : zy(3); t.bs[k] = 0; }
    style_defaults(&st, inherit_color, inherit_align);
    if (g_nodes[node].pseudo) {
        /* a generated ::before/::after: styled by its owner's pseudo rules */
        cascade_into(g_nodes[node].parent, g_nodes[node].pseudo, &st, &t, inherit_color);
        t.has_content = 0;
    } else cascade_into(node, 0, &st, &t, inherit_color);
    /* script-set style (el.style.color = ...) wins over everything */
    if (g_nodes[node].js_mask) {
        uint32_t m = g_nodes[node].js_mask;
        if (m & JS_COLOR) st.color = g_nodes[node].js_color;
        if (m & JS_BG) { st.bg = g_nodes[node].js_bg; st.has_bg = 1; t.bg_cur = 0; }
        if (m & JS_DISP) { st.display = g_nodes[node].js_display; t.disp_author = 1; }
    }
    style_finish(node, &st, &t, inherit_scale);
    g_nodes[node].color = st.color;
    /* Publish the C1 probes (also serves rb_style_* after a relayout). */
    g_nodepos[node].pos = st.pos;
    g_nodepos[node].overflow = st.overflow;
    g_nodepos[node].visibility = st.visibility;
    g_nodepos[node].cursor = st.cursor;
    g_nodepos[node].opacity = st.opacity;
    g_nodepos[node].z_auto = st.z_auto;
    g_nodepos[node].z_index = st.z_index;
    c->st = st;
    c->ic = inherit_color;
    c->isc = inherit_scale;
    c->ia = inherit_align;
    c->valid = 1;
    return st;
}

/* Style of a ::before / ::after pseudo-element of `node`, or 0 when it has no
   generated content. Returns the intern id + 1 of the content string. */
static int compute_pseudo_style(int node, int which, Style *st, int ic, int isc, int ia) {
    CascTmp t;
    memset(&t, 0, sizeof(t));
    for (int k = 0; k < 4; k++) { t.bw[k] = (k & 1) ? 3 : zy(3); t.bs[k] = 0; }
    style_defaults(st, ic, ia);
    cascade_into(node, which, st, &t, ic);
    if (!t.has_content || t.content <= 0) return 0;
    uint16_t save = g_fpx[node];
    uint8_t save_ls = g_lstype[node];
    style_finish(node, st, &t, isc);
    g_fpx[node] = save;
    g_lstype[node] = save_ls;
    return t.content;
}
