#include "engine.h"

#ifdef RBROWSE_HOST
#include <string.h>
#else
#include "../utility/utility.h"
#endif

#define MAX_NODES 512
#define MAX_BOX 768
#define MAX_RULES 96
#define MAX_FOCUS 64
#define POOL_MAX (40 * 1024)
#define RAW_MAX 12288

#define SET_COLOR 1u
#define SET_BG 2u
#define SET_FONT 4u
#define SET_DISP 8u
#define SET_ML 16u
#define SET_MT 32u
#define SET_MR 64u
#define SET_MB 128u
#define SET_PAD 256u
#define SET_W 512u
#define SET_H 1024u
#define SET_ALIGN 2048u

#define DISP_INLINE 0
#define DISP_BLOCK 1
#define DISP_NONE 2

typedef struct {
    uint8_t kind;
    uint8_t tag;
    uint8_t dead;
    uint8_t preserve;
    uint8_t input_btn;
    uint8_t hidden;
    uint8_t color;
    uint8_t js_color, js_bg, js_display, js_scale;
    uint16_t js_mask;
    uint16_t parent, first, next;
    uint16_t text_off, text_len;
    uint16_t id_off, id_len;
    uint16_t class_off, class_len;
    uint16_t href_off, href_len;
    uint16_t click_off, click_len;
    uint16_t style_off, style_len;
} Node;

typedef struct {
    uint8_t sel;
    uint8_t tag;
    uint16_t name_off, name_len;
    uint16_t spec;
    uint16_t mask;
    uint8_t color, bg, scale, display, align, pad;
    uint8_t ml, mt, mr, mb;
    int16_t width, height;
} Rule;

typedef struct {
    uint8_t color, bg, scale, display, align, pad;
    uint8_t ml, mt, mr, mb;
    uint8_t has_bg;
    uint8_t forced_block;
    int16_t width, height;
} Style;

typedef struct {
    uint16_t color, bg, font, disp, ml, mt, mr, mb, pad, w, h, align;
} Specs;

typedef struct {
    int left, right, x, y, line_h, line_start, align, used;
} Lay;

static Node g_nodes[MAX_NODES];
static int g_node_n;
static RbBox g_box[MAX_BOX];
static int g_box_n;
static Rule g_rules[MAX_RULES];
static int g_rule_n;
static int g_focus[MAX_FOCUS];
static int g_focus_n;
static char g_pool[POOL_MAX];
static int g_pool_n;
static char g_raw[RAW_MAX];
static char g_fold[RAW_MAX];
static char g_title[80];
static char g_nav[480];
static int g_nav_len;
static int g_page_h;
static int g_page_bg;
static int g_stop;

int rb_map_rgb(int r, int g, int b) {
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    int ri = (r * 5 + 127) / 255;
    int gi = (g * 5 + 127) / 255;
    int bi = (b * 5 + 127) / 255;
    return 16 + ri * 36 + gi * 6 + bi;
}

void rb_cube_rgb(int index, int *r, int *g, int *b) {
    if (index < 16 || index > 231) {
        *r = *g = *b = 0;
        return;
    }
    int i = index - 16;
    int ri = i / 36;
    int gi = (i / 6) % 6;
    int bi = i % 6;
    *r = ri * 51;
    *g = gi * 51;
    *b = bi * 51;
}

static int pool_add(const char *s, int n) {
    if (n < 0) n = 0;
    if (g_pool_n + n + 1 >= POOL_MAX) return -1;
    int off = g_pool_n;
    if (n > 0 && s) memcpy(g_pool + off, s, (size_t)n);
    g_pool[off + n] = 0;
    g_pool_n += n + 1;
    return off;
}

static int new_node(void) {
    if (g_node_n >= MAX_NODES) return 0;
    int i = g_node_n++;
    memset(&g_nodes[i], 0, sizeof(g_nodes[i]));
    return i;
}

static void add_child(int parent, int child) {
    if (!parent || !child) return;
    g_nodes[child].parent = (uint16_t)parent;
    g_nodes[child].next = 0;
    if (!g_nodes[parent].first) {
        g_nodes[parent].first = (uint16_t)child;
        return;
    }
    int c = g_nodes[parent].first;
    while (g_nodes[c].next) c = g_nodes[c].next;
    g_nodes[c].next = (uint16_t)child;
}

static int lower_ch(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

static int eq_lit(const char *s, int n, const char *lit) {
    int i = 0;
    while (lit[i]) {
        if (i >= n || lower_ch((unsigned char)s[i]) != lit[i]) return 0;
        i++;
    }
    return i == n;
}

static int starts_lit(const char *s, int n, const char *lit) {
    int i = 0;
    while (lit[i]) {
        if (i >= n || lower_ch((unsigned char)s[i]) != (unsigned char)lit[i]) return 0;
        i++;
    }
    return 1;
}

static int tag_of(const char *s, int n) {
    if (eq_lit(s, n, "html")) return RB_TAG_HTML;
    if (eq_lit(s, n, "head")) return RB_TAG_HEAD;
    if (eq_lit(s, n, "body")) return RB_TAG_BODY;
    if (eq_lit(s, n, "title")) return RB_TAG_TITLE;
    if (eq_lit(s, n, "style")) return RB_TAG_STYLE;
    if (eq_lit(s, n, "script")) return RB_TAG_SCRIPT;
    if (eq_lit(s, n, "div")) return RB_TAG_DIV;
    if (eq_lit(s, n, "p")) return RB_TAG_P;
    if (eq_lit(s, n, "span")) return RB_TAG_SPAN;
    if (eq_lit(s, n, "a")) return RB_TAG_A;
    if (eq_lit(s, n, "h1")) return RB_TAG_H1;
    if (eq_lit(s, n, "h2")) return RB_TAG_H2;
    if (eq_lit(s, n, "h3")) return RB_TAG_H3;
    if (eq_lit(s, n, "h4")) return RB_TAG_H4;
    if (eq_lit(s, n, "h5")) return RB_TAG_H5;
    if (eq_lit(s, n, "h6")) return RB_TAG_H6;
    if (eq_lit(s, n, "ul")) return RB_TAG_UL;
    if (eq_lit(s, n, "ol")) return RB_TAG_OL;
    if (eq_lit(s, n, "li")) return RB_TAG_LI;
    if (eq_lit(s, n, "br")) return RB_TAG_BR;
    if (eq_lit(s, n, "hr")) return RB_TAG_HR;
    if (eq_lit(s, n, "img")) return RB_TAG_IMG;
    if (eq_lit(s, n, "button")) return RB_TAG_BUTTON;
    if (eq_lit(s, n, "input")) return RB_TAG_INPUT;
    if (eq_lit(s, n, "textarea")) return RB_TAG_TEXTAREA;
    if (eq_lit(s, n, "form")) return RB_TAG_FORM;
    if (eq_lit(s, n, "b")) return RB_TAG_B;
    if (eq_lit(s, n, "strong")) return RB_TAG_STRONG;
    if (eq_lit(s, n, "i")) return RB_TAG_I;
    if (eq_lit(s, n, "em")) return RB_TAG_EM;
    if (eq_lit(s, n, "u")) return RB_TAG_U;
    if (eq_lit(s, n, "pre")) return RB_TAG_PRE;
    if (eq_lit(s, n, "code")) return RB_TAG_CODE;
    if (eq_lit(s, n, "nav")) return RB_TAG_NAV;
    if (eq_lit(s, n, "header")) return RB_TAG_HEADER;
    if (eq_lit(s, n, "footer")) return RB_TAG_FOOTER;
    if (eq_lit(s, n, "main")) return RB_TAG_MAIN;
    if (eq_lit(s, n, "section")) return RB_TAG_SECTION;
    if (eq_lit(s, n, "article")) return RB_TAG_ARTICLE;
    if (eq_lit(s, n, "table")) return RB_TAG_TABLE;
    if (eq_lit(s, n, "tr")) return RB_TAG_TR;
    if (eq_lit(s, n, "td")) return RB_TAG_TD;
    if (eq_lit(s, n, "th")) return RB_TAG_TH;
    if (eq_lit(s, n, "center")) return RB_TAG_CENTER;
    return RB_TAG_OTHER;
}

static int is_void_tag(int tag) {
    return tag == RB_TAG_BR || tag == RB_TAG_HR || tag == RB_TAG_IMG || tag == RB_TAG_INPUT;
}

static int is_raw_tag(int tag) {
    return tag == RB_TAG_SCRIPT || tag == RB_TAG_STYLE || tag == RB_TAG_TEXTAREA || tag == RB_TAG_TITLE;
}

static int default_block(int tag) {
    switch (tag) {
    case RB_TAG_HTML: case RB_TAG_HEAD: case RB_TAG_BODY: case RB_TAG_TITLE:
    case RB_TAG_STYLE: case RB_TAG_SCRIPT:
    case RB_TAG_DIV: case RB_TAG_P:
    case RB_TAG_H1: case RB_TAG_H2: case RB_TAG_H3:
    case RB_TAG_H4: case RB_TAG_H5: case RB_TAG_H6:
    case RB_TAG_UL: case RB_TAG_OL: case RB_TAG_LI:
    case RB_TAG_HR: case RB_TAG_PRE: case RB_TAG_FORM:
    case RB_TAG_NAV: case RB_TAG_HEADER: case RB_TAG_FOOTER: case RB_TAG_MAIN:
    case RB_TAG_SECTION: case RB_TAG_ARTICLE:
    case RB_TAG_TABLE: case RB_TAG_TR: case RB_TAG_TD: case RB_TAG_TH:
    case RB_TAG_CENTER: case RB_TAG_TEXTAREA: case RB_TAG_DOC:
        return 1;
    default:
        return 0;
    }
}

static int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int named_color(const char *s, int n, int *r, int *g, int *b) {
    struct { const char *name; int rv, gv, bv; } table[] = {
        {"black", 0, 0, 0}, {"white", 255, 255, 255}, {"red", 255, 0, 0},
        {"green", 0, 128, 0}, {"lime", 0, 255, 0}, {"blue", 0, 0, 255},
        {"yellow", 255, 255, 0}, {"orange", 255, 165, 0}, {"gray", 128, 128, 128},
        {"grey", 128, 128, 128}, {"silver", 192, 192, 192}, {"cyan", 0, 255, 255},
        {"aqua", 0, 255, 255}, {"magenta", 255, 0, 255}, {"fuchsia", 255, 0, 255},
        {"purple", 128, 0, 128}, {"maroon", 128, 0, 0}, {"navy", 0, 0, 128},
        {"teal", 0, 128, 128}, {"olive", 128, 128, 0}, {"brown", 165, 42, 42},
        {"pink", 255, 192, 203},
    };
    int count = (int)(sizeof(table) / sizeof(table[0]));
    for (int i = 0; i < count; i++) {
        if (eq_lit(s, n, table[i].name)) {
            *r = table[i].rv;
            *g = table[i].gv;
            *b = table[i].bv;
            return 1;
        }
    }
    return 0;
}

static int parse_color(const char *s, int n, int *out) {
    while (n > 0 && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == ';')) n--;
    if (n >= 4 && s[0] == '#' ) {
        int r, g, b;
        if (n >= 7 && hex_val(s[1]) >= 0) {
            r = (hex_val(s[1]) << 4) | hex_val(s[2]);
            g = (hex_val(s[3]) << 4) | hex_val(s[4]);
            b = (hex_val(s[5]) << 4) | hex_val(s[6]);
            if (r >= 0 && g >= 0 && b >= 0) { *out = rb_map_rgb(r, g, b); return 1; }
        } else if (n >= 4) {
            int hr = hex_val(s[1]), hg = hex_val(s[2]), hb = hex_val(s[3]);
            if (hr >= 0 && hg >= 0 && hb >= 0) {
                *out = rb_map_rgb(hr * 17, hg * 17, hb * 17);
                return 1;
            }
        }
    }
    int r, g, b;
    if (named_color(s, n, &r, &g, &b)) { *out = rb_map_rgb(r, g, b); return 1; }
    return 0;
}

static int parse_len(const char *s, int n, int *pct) {
    int i = 0;
    *pct = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    int v = 0, any = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        if (v > 4000) v = 4000;
        i++;
        any = 1;
    }
    if (i < n && s[i] == '%') *pct = 1;
    return any ? v : -1;
}

static void apply_prop(Rule *rule, const char *prop, int pn, const char *val, int vn) {
    while (pn > 0 && (prop[pn - 1] == ' ' || prop[pn - 1] == '\t')) pn--;
    while (vn > 0 && (val[vn - 1] == ' ' || val[vn - 1] == '\t')) vn--;
    int tmp = 0;
    if (eq_lit(prop, pn, "color")) {
        if (parse_color(val, vn, &tmp)) { rule->color = (uint8_t)tmp; rule->mask |= SET_COLOR; }
    } else if (eq_lit(prop, pn, "background-color") || eq_lit(prop, pn, "background")) {
        if (parse_color(val, vn, &tmp)) { rule->bg = (uint8_t)tmp; rule->mask |= SET_BG; }
    } else if (eq_lit(prop, pn, "font-size")) {
        int pct = 0, v = parse_len(val, vn, &pct);
        if (v > 0) {
            rule->scale = (uint8_t)(v >= 20 ? 2 : 1);
            rule->mask |= SET_FONT;
        }
    } else if (eq_lit(prop, pn, "display")) {
        if (starts_lit(val, vn, "none")) { rule->display = DISP_NONE; rule->mask |= SET_DISP; }
        else if (eq_lit(val, vn, "block")) { rule->display = DISP_BLOCK; rule->mask |= SET_DISP; }
        else if (eq_lit(val, vn, "inline") || eq_lit(val, vn, "inline-block")) {
            rule->display = DISP_INLINE; rule->mask |= SET_DISP;
        }
    } else if (eq_lit(prop, pn, "visibility")) {
        if (starts_lit(val, vn, "hidden") || starts_lit(val, vn, "collapse")) {
            rule->display = DISP_NONE;
            rule->mask |= SET_DISP;
        }
    } else if (eq_lit(prop, pn, "text-align")) {
        if (eq_lit(val, vn, "center")) { rule->align = 1; rule->mask |= SET_ALIGN; }
        else if (eq_lit(val, vn, "right")) { rule->align = 2; rule->mask |= SET_ALIGN; }
        else if (eq_lit(val, vn, "left")) { rule->align = 0; rule->mask |= SET_ALIGN; }
    } else if (eq_lit(prop, pn, "padding") || eq_lit(prop, pn, "padding-left") ||
               eq_lit(prop, pn, "padding-top") || eq_lit(prop, pn, "padding-right") ||
               eq_lit(prop, pn, "padding-bottom")) {
        int pct = 0, v = parse_len(val, vn, &pct);
        if (v >= 0) {
            if (v > 40) v = 40;
            rule->pad = (uint8_t)v;
            rule->mask |= SET_PAD;
        }
    } else if (eq_lit(prop, pn, "margin")) {
        int vals[4];
        int nv = 0;
        int i = 0;
        while (i < vn && nv < 4) {
            while (i < vn && (val[i] == ' ' || val[i] == '\t')) i++;
            if (i >= vn) break;
            int st = i;
            while (i < vn && val[i] != ' ' && val[i] != '\t') i++;
            int pct = 0, v = parse_len(val + st, i - st, &pct);
            if (v < 0) break;
            if (v > 80) v = 80;
            vals[nv++] = v;
        }
        if (nv == 1) vals[1] = vals[2] = vals[3] = vals[0];
        else if (nv == 2) { vals[2] = vals[0]; vals[3] = vals[1]; }
        else if (nv == 3) vals[3] = vals[1];
        if (nv >= 1) {
            rule->mt = (uint8_t)vals[0];
            rule->mr = (uint8_t)vals[1];
            rule->mb = (uint8_t)vals[2];
            rule->ml = (uint8_t)vals[3];
            rule->mask |= SET_MT | SET_MR | SET_MB | SET_ML;
        }
    } else if (eq_lit(prop, pn, "margin-left") || eq_lit(prop, pn, "margin-right") ||
               eq_lit(prop, pn, "margin-top") || eq_lit(prop, pn, "margin-bottom")) {
        int pct = 0, v = parse_len(val, vn, &pct);
        if (v >= 0) {
            if (v > 80) v = 80;
            if (eq_lit(prop, pn, "margin-left")) { rule->ml = (uint8_t)v; rule->mask |= SET_ML; }
            else if (eq_lit(prop, pn, "margin-right")) { rule->mr = (uint8_t)v; rule->mask |= SET_MR; }
            else if (eq_lit(prop, pn, "margin-top")) { rule->mt = (uint8_t)v; rule->mask |= SET_MT; }
            else { rule->mb = (uint8_t)v; rule->mask |= SET_MB; }
        }
    } else if (eq_lit(prop, pn, "width") || eq_lit(prop, pn, "height")) {
        int pct = 0, v = parse_len(val, vn, &pct);
        if (v >= 0) {
            int16_t stored = pct ? (int16_t)(-v) : (int16_t)v;
            if (eq_lit(prop, pn, "width")) { rule->width = stored; rule->mask |= SET_W; }
            else { rule->height = stored; rule->mask |= SET_H; }
        }
    }
}

static void parse_decl_list(const char *s, int n, Rule *rule) {
    int i = 0;
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == ';')) i++;
        if (i >= n) break;
        int ps = i;
        while (i < n && s[i] != ':' && s[i] != ';' && s[i] != '}') i++;
        if (i >= n || s[i] != ':') break;
        int pn = i - ps;
        i++;
        int vs = i;
        while (i < n && s[i] != ';' && s[i] != '}') i++;
        apply_prop(rule, s + ps, pn, s + vs, i - vs);
        if (i < n && s[i] == ';') i++;
    }
}

static int selector_ok(const char *s, int n) {
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '>' || c == '+' || c == '~' || c == ':' || c == '[') return 0;
    }
    return n > 0;
}

static void add_selector(const char *sel, int n, const Rule *proto) {
    while (n > 0 && (sel[0] == ' ' || sel[0] == '\t' || sel[0] == '\n')) { sel++; n--; }
    while (n > 0 && (sel[n - 1] == ' ' || sel[n - 1] == '\t' || sel[n - 1] == '\n')) n--;
    if (!selector_ok(sel, n) || g_rule_n >= MAX_RULES) return;
    Rule r = *proto;
    if (sel[0] == '.') {
        int off = pool_add(sel + 1, n - 1);
        if (off < 0) return;
        r.sel = 2;
        r.name_off = (uint16_t)off;
        r.name_len = (uint16_t)(n - 1);
        r.spec = 10;
    } else if (sel[0] == '#') {
        int off = pool_add(sel + 1, n - 1);
        if (off < 0) return;
        r.sel = 3;
        r.name_off = (uint16_t)off;
        r.name_len = (uint16_t)(n - 1);
        r.spec = 100;
    } else {
        r.sel = 1;
        r.tag = (uint8_t)tag_of(sel, n);
        r.spec = 1;
    }
    g_rules[g_rule_n++] = r;
}

static void parse_css(const char *s, int n) {
    int i = 0;
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            if (i + 1 < n) i += 2;
            continue;
        }
        if (i >= n) break;
        if (s[i] == '@') {
            while (i < n && s[i] != '{') i++;
            int depth = 0;
            while (i < n) {
                if (s[i] == '{') depth++;
                else if (s[i] == '}') {
                    depth--;
                    i++;
                    if (depth <= 0) break;
                    continue;
                }
                i++;
            }
            continue;
        }
        int ss = i;
        while (i < n && s[i] != '{') i++;
        if (i >= n) break;
        int se = i;
        i++;
        int bs = i;
        while (i < n && s[i] != '}') i++;
        int be = i;
        if (i < n) i++;
        Rule proto;
        memset(&proto, 0, sizeof(proto));
        proto.scale = 1;
        parse_decl_list(s + bs, be - bs, &proto);
        int k = ss;
        while (k < se) {
            int a = k;
            while (k < se && s[k] != ',') k++;
            add_selector(s + a, k - a, &proto);
            if (k < se && s[k] == ',') k++;
        }
    }
}

static void inline_rule(int node, Rule *out) {
    memset(out, 0, sizeof(*out));
    out->scale = 1;
    out->spec = 1000;
    if (g_nodes[node].style_len == 0) return;
    parse_decl_list(g_pool + g_nodes[node].style_off, g_nodes[node].style_len, out);
}

static int class_has(int node, const char *name, int nlen) {
    if (g_nodes[node].class_len == 0 || nlen <= 0) return 0;
    const char *s = g_pool + g_nodes[node].class_off;
    int len = g_nodes[node].class_len;
    int i = 0;
    while (i < len) {
        while (i < len && s[i] == ' ') i++;
        int st = i;
        while (i < len && s[i] != ' ') i++;
        if (i - st == nlen && memcmp(s + st, name, (size_t)nlen) == 0) return 1;
    }
    return 0;
}

static int rule_matches(const Rule *r, int node) {
    if (g_nodes[node].kind != 0) return 0;
    if (r->sel == 1) return g_nodes[node].tag == r->tag;
    if (r->sel == 2) return class_has(node, g_pool + r->name_off, r->name_len);
    if (r->sel == 3) {
        return g_nodes[node].id_len == r->name_len &&
               memcmp(g_pool + g_nodes[node].id_off, g_pool + r->name_off, r->name_len) == 0;
    }
    return 0;
}

static void consider(Style *st, Specs *sp, uint16_t spec, uint16_t mask, const Rule *r) {
    if ((mask & SET_COLOR) && spec >= sp->color) { st->color = r->color; sp->color = spec; }
    if ((mask & SET_BG) && spec >= sp->bg) { st->bg = r->bg; st->has_bg = 1; sp->bg = spec; }
    if ((mask & SET_FONT) && spec >= sp->font) { st->scale = r->scale ? r->scale : 1; sp->font = spec; }
    if ((mask & SET_DISP) && spec >= sp->disp) { st->display = r->display; sp->disp = spec; }
    if ((mask & SET_ML) && spec >= sp->ml) { st->ml = r->ml; sp->ml = spec; }
    if ((mask & SET_MT) && spec >= sp->mt) { st->mt = r->mt; sp->mt = spec; }
    if ((mask & SET_MR) && spec >= sp->mr) { st->mr = r->mr; sp->mr = spec; }
    if ((mask & SET_MB) && spec >= sp->mb) { st->mb = r->mb; sp->mb = spec; }
    if ((mask & SET_PAD) && spec >= sp->pad) { st->pad = r->pad; sp->pad = spec; }
    if ((mask & SET_W) && spec >= sp->w) { st->width = r->width; sp->w = spec; }
    if ((mask & SET_H) && spec >= sp->h) { st->height = r->height; sp->h = spec; }
    if ((mask & SET_ALIGN) && spec >= sp->align) { st->align = r->align; sp->align = spec; }
}

static void ua_rule(int node, Rule *r) {
    memset(r, 0, sizeof(*r));
    r->scale = 1;
    int tag = g_nodes[node].tag;
    if (default_block(tag)) { r->display = DISP_BLOCK; r->mask |= SET_DISP; }
    if (tag == RB_TAG_HEAD || tag == RB_TAG_STYLE || tag == RB_TAG_SCRIPT || tag == RB_TAG_TITLE) {
        r->display = DISP_NONE;
        r->mask |= SET_DISP;
    }
    if (tag == RB_TAG_H1) { r->scale = 2; r->mask |= SET_FONT; r->mb = 4; r->mask |= SET_MB; }
    if (tag == RB_TAG_H2 || tag == RB_TAG_H3) { r->mb = 3; r->mask |= SET_MB; }
    if (tag == RB_TAG_P) { r->mb = 4; r->mask |= SET_MB; }
    if (tag == RB_TAG_LI) { r->ml = 8; r->mask |= SET_ML; }
    if (tag == RB_TAG_A) {
        r->color = (uint8_t)rb_map_rgb(255, 119, 34);
        r->mask |= SET_COLOR;
    }
    if (tag == RB_TAG_BUTTON || (tag == RB_TAG_INPUT && g_nodes[node].input_btn)) {
        r->bg = (uint8_t)rb_map_rgb(92, 34, 0);
        r->color = (uint8_t)rb_map_rgb(255, 204, 170);
        r->pad = 2;
        r->mask |= SET_BG | SET_COLOR | SET_PAD;
    } else if (tag == RB_TAG_INPUT || tag == RB_TAG_TEXTAREA) {
        r->bg = (uint8_t)rb_map_rgb(240, 232, 220);
        r->color = (uint8_t)rb_map_rgb(20, 16, 12);
        r->pad = 1;
        r->mask |= SET_BG | SET_COLOR | SET_PAD;
    }
    if (g_nodes[node].hidden) {
        r->display = DISP_NONE;
        r->mask |= SET_DISP;
    }
    if (tag == RB_TAG_BODY) {
        /* 0x12,0x0C,0x06 quantizes to cube index 16, which is pure black.
           Keep a dark brown the 6x6x6 palette can actually show, with light text. */
        r->bg = (uint8_t)rb_map_rgb(0x66, 0x3C, 0x22);
        r->color = (uint8_t)rb_map_rgb(0xFF, 0xE6, 0xC8);
        r->mask |= SET_BG | SET_COLOR;
    }
    if (tag == RB_TAG_CENTER) { r->align = 1; r->mask |= SET_ALIGN; }
    if (tag == RB_TAG_B || tag == RB_TAG_STRONG) { r->color = (uint8_t)rb_map_rgb(255, 220, 190); r->mask |= SET_COLOR; }
}

static Style compute_style(int node, uint8_t inherit_color, uint8_t inherit_scale, uint8_t inherit_align) {
    Style st;
    memset(&st, 0, sizeof(st));
    st.color = inherit_color;
    st.scale = inherit_scale ? inherit_scale : 1;
    st.align = inherit_align;
    st.display = DISP_INLINE;
    st.width = -32768;
    st.height = -32768;
    Specs sp;
    memset(&sp, 0, sizeof(sp));
    Rule ua;
    ua_rule(node, &ua);
    consider(&st, &sp, 0, ua.mask, &ua);
    for (int i = 0; i < g_rule_n; i++) {
        if (rule_matches(&g_rules[i], node))
            consider(&st, &sp, g_rules[i].spec, g_rules[i].mask, &g_rules[i]);
    }
    Rule inl;
    inline_rule(node, &inl);
    if (g_nodes[node].style_len) consider(&st, &sp, 1000, inl.mask, &inl);
    if (g_nodes[node].js_mask) {
        Rule js;
        memset(&js, 0, sizeof(js));
        js.color = g_nodes[node].js_color;
        js.bg = g_nodes[node].js_bg;
        js.display = g_nodes[node].js_display;
        js.scale = g_nodes[node].js_scale ? g_nodes[node].js_scale : st.scale;
        consider(&st, &sp, 2000, g_nodes[node].js_mask, &js);
    }
    if (st.scale == 0) st.scale = 1;
    if (sp.disp > 0 && st.display == DISP_BLOCK) st.forced_block = 1;
    if (g_nodes[node].hidden) st.display = DISP_NONE;
    g_nodes[node].color = st.color;
    return st;
}

static int emit_box(int x, int y, int w, int h, int node, int color, int bg, int scale, int kind) {
    if (g_box_n >= MAX_BOX) return -1;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    RbBox *b = &g_box[g_box_n++];
    b->x = (int16_t)x;
    b->y = (int16_t)y;
    b->w = (int16_t)w;
    b->h = (int16_t)h;
    b->node = (uint16_t)node;
    b->color = (uint8_t)color;
    b->bg = (uint8_t)bg;
    b->scale = (uint8_t)(scale ? scale : 1);
    b->kind = (uint8_t)kind;
    b->toff = 0;
    b->tlen = 0;
    return g_box_n - 1;
}

static void bind_text(int id, const char *s, int n) {
    if (id < 0 || n <= 0) return;
    int off = pool_add(s, n);
    if (off < 0) return;
    g_box[id].toff = (uint16_t)off;
    g_box[id].tlen = (uint16_t)n;
}

static void newline(Lay *L) {
    int lh = L->line_h > 0 ? L->line_h : (L->used ? 8 : 0);
    if (!L->used && L->line_h == 0) lh = 0;
    int line_w = L->x - L->left;
    int extra = (L->right - L->left) - line_w;
    if (extra < 0) extra = 0;
    int shift = 0;
    if (L->align == 1) shift = extra / 2;
    else if (L->align == 2) shift = extra;
    if (shift) {
        for (int i = L->line_start; i < g_box_n; i++) g_box[i].x = (int16_t)(g_box[i].x + shift);
    }
    /* One extra pixel so an 8px glyph and the focus ring do not sit on the next line. */
    if (lh > 0) L->y += lh + 1;
    L->x = L->left;
    L->line_h = 0;
    L->used = 0;
    L->line_start = g_box_n;
}

static void place_text(Lay *L, const char *s, int n, int node, int color, int scale, int preserve) {
    int step = 8 * (scale ? scale : 1);
    int i = 0;
    while (i < n) {
        if (s[i] == '\n') {
            newline(L);
            i++;
            continue;
        }
        if (!preserve && s[i] == ' ') {
            if (L->x > L->left) {
                L->x += step;
                if (L->x >= L->right) newline(L);
                if (step > L->line_h) L->line_h = step;
                L->used = 1;
            }
            i++;
            continue;
        }
        int start = i;
        int limit = L->right - L->x;
        if (limit < step && L->x > L->left) {
            newline(L);
            limit = L->right - L->left;
        }
        int maxc = limit / step;
        if (maxc < 1) maxc = 1;
        int count = 0;
        if (preserve) {
            while (i < n && s[i] != '\n' && count < maxc) { i++; count++; }
        } else {
            while (i < n && s[i] != ' ' && s[i] != '\n') { i++; count++; }
            if (count > maxc) {
                if (L->x > L->left) {
                    newline(L);
                    i = start;
                    continue;
                }
                i = start + maxc;
                count = maxc;
            }
        }
        if (count <= 0) { i++; continue; }
        int ww = count * step;
        bind_text(emit_box(L->x, L->y, ww, step, node, color, 0, scale, RB_BOX_TEXT), s + start, count);
        L->x += ww;
        if (step > L->line_h) L->line_h = step;
        L->used = 1;
    }
}

static int collect_rec(int node, char *out, int cap, int pos, int skip_raw);

static int text_of(int node, char *out, int cap) {
    if (cap <= 0) return 0;
    int n = collect_rec(node, out, cap, 0, 0);
    if (n < 0) n = 0;
    if (n >= cap) n = cap - 1;
    out[n] = 0;
    return n;
}

static int resolve_pct(int stored, int avail, int fill_if_tiny) {
    int pct = -stored;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (avail < 8 || pct < 1) return avail > 8 ? avail : 8;
    int w = avail * pct / 100;
    if (w < 8) return fill_if_tiny ? avail : 8;
    return w;
}

/* one_line: text field or image. Height stays one glyph row; CSS height and
   horizontal padding must not inflate it into a page-sized rectangle. */
static void place_control(Lay *L, int node, const Style *st, const char *label, int min_w, int min_h, int one_line) {
    int scale = st->scale ? st->scale : 1;
    int step = 8 * scale;
    int avail = L->right - L->left;
    if (avail < step) avail = step;
    if (avail > RB_VIEW_W) avail = RB_VIEW_W;
    int vpad = st->pad;
    if (vpad > 2) vpad = 2;
    int lab_n = label ? (int)strlen(label) : 0;
    int bw = min_w;
    if (st->width < 0 && st->width != -32768) {
        bw = resolve_pct(st->width, avail, 1);
    } else if (st->width > 0) {
        bw = st->width;
        if (bw > RB_VIEW_W) bw = RB_VIEW_W;
    } else if (one_line && lab_n == 0) {
        bw = avail > 200 ? 200 : avail;
    } else {
        int tw = lab_n * step + vpad * 2;
        if (tw > bw) bw = tw;
    }
    if (bw < min_w) bw = min_w;
    if (bw > avail) bw = avail;
    if (bw < step) bw = step;

    int bh = step + vpad * 2;
    if (one_line) {
        bh = step + 4;
        if (bh < 12) bh = 12;
    } else {
        if (bh < min_h) bh = min_h;
        if (st->height > 0) {
            int h = st->height;
            if (h > 48) h = 48;
            if (h > bh) bh = h;
        }
    }
    if (L->x > L->left && L->x + bw > L->right) newline(L);
    int bg = st->has_bg ? st->bg : (uint8_t)rb_map_rgb(240, 232, 220);
    int box = emit_box(L->x, L->y, bw, bh, node, st->color, bg, scale, RB_BOX_CONTROL);
    if (lab_n > 0) {
        int inner = bw - vpad * 2;
        if (inner < step) inner = step;
        int maxc = inner / step;
        if (maxc < 1) maxc = 1;
        if (one_line) {
            int n = lab_n > maxc ? maxc : lab_n;
            int ty = L->y + (bh - step) / 2;
            bind_text(emit_box(L->x + vpad, ty, n * step, step, node, st->color, 0, scale, RB_BOX_TEXT),
                      label, n);
        } else {
            Lay t;
            memset(&t, 0, sizeof(t));
            t.left = t.x = L->x + vpad;
            t.right = t.left + inner;
            t.y = L->y + vpad;
            t.line_start = g_box_n;
            place_text(&t, label, lab_n, node, st->color, scale, 0);
            if (t.used) newline(&t);
            int need = (t.y - L->y) + vpad;
            if (need < step + vpad * 2) need = step + vpad * 2;
            if (need > 200) need = 200;
            if (need > bh && box >= 0) {
                bh = need;
                g_box[box].h = (int16_t)bh;
            }
        }
    }
    L->x += bw + step / 2;
    if (bh > L->line_h) L->line_h = bh;
    L->used = 1;
}

static int layout_block(int node, int x, int y, int w, uint8_t ic, uint8_t isc, uint8_t ia);

/* A div with no box of its own and no direct text is a wrapper. Google's
   header is one link per div; flowing those children keeps the links on a row. */
static int flow_wrapper(int node, const Style *st) {
    if (g_nodes[node].tag != RB_TAG_DIV) return 0;
    if (st->forced_block || st->has_bg || st->pad) return 0;
    if (st->ml || st->mr || st->mt || st->mb) return 0;
    if (st->width != -32768 || st->height != -32768) return 0;
    if (st->display == DISP_NONE) return 0;
    for (int c = g_nodes[node].first; c; c = g_nodes[c].next) {
        if (g_nodes[c].dead) continue;
        if (g_nodes[c].kind == 1) {
            const char *s = g_pool + g_nodes[c].text_off;
            for (int i = 0; i < g_nodes[c].text_len; i++) {
                char ch = s[i];
                if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') return 0;
            }
            continue;
        }
        if (g_nodes[c].hidden) continue;
        Style cs = compute_style(c, st->color, st->scale, st->align);
        if (cs.display == DISP_NONE) continue;
        if (cs.display == DISP_BLOCK && !flow_wrapper(c, &cs)) return 0;
    }
    return 1;
}

static void inline_gap(Lay *L) {
    int step = 8;
    if (!L->used || L->x <= L->left) return;
    if (L->x + step > L->right) return;
    L->x += step;
    if (step > L->line_h) L->line_h = step;
}

static void layout_inline(int node, Lay *L, uint8_t ic, uint8_t isc, uint8_t ia, int hot) {
    if (!node || g_nodes[node].dead || g_nodes[node].hidden) return;
    if (g_nodes[node].kind == 1) {
        int box_node = hot ? hot : node;
        place_text(L, g_pool + g_nodes[node].text_off, g_nodes[node].text_len,
                   box_node, ic, isc, g_nodes[node].preserve);
        return;
    }
    Style st = compute_style(node, ic, isc, ia);
    if (st.display == DISP_NONE) return;
    int tag = g_nodes[node].tag;
    if (tag == RB_TAG_BR) { newline(L); return; }
    if (tag == RB_TAG_BUTTON || (tag == RB_TAG_INPUT && g_nodes[node].input_btn)) {
        char label[80];
        int n = text_of(node, label, (int)sizeof(label));
        if (n <= 0 && g_nodes[node].text_len) {
            n = g_nodes[node].text_len;
            if (n > 78) n = 78;
            memcpy(label, g_pool + g_nodes[node].text_off, (size_t)n);
        }
        label[n > 0 ? n : 0] = 0;
        if (n <= 0) {
            memcpy(label, tag == RB_TAG_BUTTON ? "button" : "ok", tag == RB_TAG_BUTTON ? 7 : 3);
        }
        if (L->used) newline(L);
        place_control(L, node, &st, label, 24, 12, 0);
        newline(L);
        return;
    }
    if (tag == RB_TAG_INPUT || tag == RB_TAG_TEXTAREA) {
        char label[80];
        int n = g_nodes[node].text_len;
        if (n > 78) n = 78;
        if (n > 0) memcpy(label, g_pool + g_nodes[node].text_off, (size_t)n);
        label[n] = 0;
        int area = tag == RB_TAG_TEXTAREA;
        inline_gap(L);
        place_control(L, node, &st, label, area ? 140 : 72, area ? 24 : 12, area ? 0 : 1);
        return;
    }
    if (tag == RB_TAG_IMG) {
        char alt[48];
        int n = g_nodes[node].text_len;
        if (n > 46) n = 46;
        if (n > 0) memcpy(alt, g_pool + g_nodes[node].text_off, (size_t)n);
        alt[n] = 0;
        if (n <= 0) memcpy(alt, "[img]", 6);
        inline_gap(L);
        place_control(L, node, &st, alt, 24, 12, 1);
        return;
    }
    if (st.display == DISP_BLOCK) {
        if (flow_wrapper(node, &st)) {
            int nhot = hot;
            if (tag == RB_TAG_A && g_nodes[node].href_len) nhot = node;
            for (int c = g_nodes[node].first; c; c = g_nodes[c].next)
                layout_inline(c, L, st.color, st.scale, st.align, nhot);
            return;
        }
        if (L->used) newline(L);
        int h = layout_block(node, L->left, L->y, L->right - L->left, ic, isc, ia);
        L->y += h;
        L->x = L->left;
        L->line_h = 0;
        L->used = 0;
        L->line_start = g_box_n;
        return;
    }
    int nhot = hot;
    if (tag == RB_TAG_A && g_nodes[node].href_len) nhot = node;
    inline_gap(L);
    for (int c = g_nodes[node].first; c; c = g_nodes[c].next)
        layout_inline(c, L, st.color, st.scale, st.align, nhot);
}

static int layout_block(int node, int x, int y, int w, uint8_t ic, uint8_t isc, uint8_t ia) {
    if (!node || g_nodes[node].dead || g_nodes[node].hidden) return 0;
    if (g_nodes[node].kind == 1) {
        Lay L;
        memset(&L, 0, sizeof(L));
        L.left = L.x = x;
        L.right = x + w;
        L.y = y;
        L.line_start = g_box_n;
        place_text(&L, g_pool + g_nodes[node].text_off, g_nodes[node].text_len, node, ic, isc, g_nodes[node].preserve);
        if (L.used) newline(&L);
        return L.y - y;
    }
    Style st = compute_style(node, ic, isc, ia);
    if (st.display == DISP_NONE) return 0;
    if (g_nodes[node].tag == RB_TAG_BODY && st.has_bg) g_page_bg = st.bg;
    int ml = st.ml, mr = st.mr, mt = st.mt, mb = st.mb, pad = st.pad;
    int iw = w - ml - mr;
    if (iw < 8) iw = 8;
    if (st.width > 0) {
        int vw = st.width;
        if (vw > RB_VIEW_W) vw = RB_VIEW_W;
        if (vw > iw) vw = iw;
        if (vw < iw) iw = vw;
    } else if (st.width < 0 && st.width != -32768) {
        iw = resolve_pct(st.width, w > iw ? w : iw, 1);
        if (iw > w - ml - mr && w - ml - mr >= 8) iw = w - ml - mr;
    }
    int ix = x + ml;
    int iy = y + mt;
    int bg_i = -1;
    if (st.has_bg) bg_i = emit_box(ix, iy, iw, 0, node, st.color, st.bg, 1, RB_BOX_BG);
    if (g_nodes[node].tag == RB_TAG_HR) {
        emit_box(ix, iy + 2, iw, 1, node, st.color, 0, 1, RB_BOX_RULE);
        if (bg_i >= 0) g_box[bg_i].h = 6;
        return mt + 6 + mb;
    }
    Lay L;
    memset(&L, 0, sizeof(L));
    L.left = L.x = ix + pad;
    L.right = ix + iw - pad;
    if (L.right < L.left + 8) L.right = L.left + 8;
    L.y = iy + pad;
    L.align = st.align;
    L.line_start = g_box_n;
    if (g_nodes[node].tag == RB_TAG_LI) {
        place_text(&L, "- ", 2, node, st.color, st.scale, 0);
    }
    if (g_nodes[node].tag == RB_TAG_INPUT || g_nodes[node].tag == RB_TAG_TEXTAREA ||
        g_nodes[node].tag == RB_TAG_BUTTON) {
        char label[80];
        int n = 0;
        if (g_nodes[node].tag == RB_TAG_BUTTON) n = text_of(node, label, (int)sizeof(label));
        else {
            n = g_nodes[node].text_len;
            if (n > 78) n = 78;
            if (n > 0) memcpy(label, g_pool + g_nodes[node].text_off, (size_t)n);
            label[n] = 0;
        }
        if (n == 0 && g_nodes[node].tag == RB_TAG_BUTTON) {
            memcpy(label, "button", 7);
        }
        int area = g_nodes[node].tag == RB_TAG_TEXTAREA;
        int btn = g_nodes[node].tag == RB_TAG_BUTTON || g_nodes[node].input_btn;
        int min_w = area ? 140 : (btn ? 24 : 72);
        int min_h = area ? 24 : 12;
        place_control(&L, node, &st, label, min_w, min_h, (area || btn) ? 0 : 1);
    } else {
        int hot = 0;
        if (g_nodes[node].tag == RB_TAG_A && g_nodes[node].href_len) hot = node;
        for (int c = g_nodes[node].first; c; c = g_nodes[c].next)
            layout_inline(c, &L, st.color, st.scale, st.align, hot);
    }
    if (L.used) newline(&L);
    int content_h = L.y - (iy + pad);
    if (content_h < 0) content_h = 0;
    int inner_h = content_h;
    if (st.height > 0) {
        int h = st.height;
        if (h > 200) h = 200;
        if (h > inner_h) inner_h = h;
    }
    int total = inner_h + pad * 2;
    if (bg_i >= 0) g_box[bg_i].h = (int16_t)total;
    return mt + total + mb;
}

static int is_focusable(int node) {
    if (g_nodes[node].hidden) return 0;
    int tag = g_nodes[node].tag;
    if (tag == RB_TAG_A) return g_nodes[node].href_len > 0;
    if (tag == RB_TAG_BUTTON) return 1;
    if (tag == RB_TAG_TEXTAREA) return 1;
    if (tag == RB_TAG_INPUT) return 1;
    return 0;
}

static void focus_walk(int node, uint8_t ic, uint8_t isc, uint8_t ia) {
    if (!node || g_nodes[node].dead || g_nodes[node].kind == 1 || g_nodes[node].hidden) return;
    Style st = compute_style(node, ic, isc, ia);
    if (st.display == DISP_NONE) return;
    if (is_focusable(node) && g_focus_n < MAX_FOCUS) g_focus[g_focus_n++] = node;
    for (int c = g_nodes[node].first; c; c = g_nodes[c].next)
        focus_walk(c, st.color, st.scale, st.align);
}

static void do_layout(void) {
    g_box_n = 0;
    g_focus_n = 0;
    g_page_h = 0;
    g_page_bg = rb_map_rgb(0x66, 0x3C, 0x22);
    if (g_node_n <= 1) return;
    uint8_t ic = (uint8_t)rb_map_rgb(0xFF, 0xE6, 0xC8);
    g_page_h = layout_block(1, 0, 0, RB_VIEW_W, ic, 1, 0);
    if (g_page_h < 0) g_page_h = 0;
    focus_walk(1, ic, 1, 0);
}

static int ascii_fold(int v) {
    if (v == 0xA0) return ' ';
    if (v >= 0xC0 && v <= 0xC5) return 'A';
    if (v >= 0xE0 && v <= 0xE5) return 'a';
    if (v == 0xC7) return 'C';
    if (v == 0xE7) return 'c';
    if (v >= 0xC8 && v <= 0xCB) return 'E';
    if (v >= 0xE8 && v <= 0xEB) return 'e';
    if (v >= 0xCC && v <= 0xCF) return 'I';
    if (v >= 0xEC && v <= 0xEF) return 'i';
    if (v == 0xD1) return 'N';
    if (v == 0xF1) return 'n';
    if ((v >= 0xD2 && v <= 0xD6) || v == 0xD8) return 'O';
    if ((v >= 0xF2 && v <= 0xF6) || v == 0xF8) return 'o';
    if (v >= 0xD9 && v <= 0xDC) return 'U';
    if (v >= 0xF9 && v <= 0xFC) return 'u';
    if (v == 0xDD) return 'Y';
    if (v == 0xFD || v == 0xFF) return 'y';
    if (v == 260 || v == 261) return v == 260 ? 'A' : 'a';
    if (v == 262 || v == 263) return v == 262 ? 'C' : 'c';
    if (v == 280 || v == 281) return v == 280 ? 'E' : 'e';
    if (v == 321 || v == 322) return v == 321 ? 'L' : 'l';
    if (v == 323 || v == 324) return v == 323 ? 'N' : 'n';
    if (v == 346 || v == 347) return v == 346 ? 'S' : 's';
    if (v == 377 || v == 378 || v == 379 || v == 380) return (v == 377 || v == 379) ? 'Z' : 'z';
    return 0;
}

static int decode_entity(const char *s, int n, int *used) {
    if (n >= 5 && eq_lit(s, 5, "&amp;")) { *used = 5; return '&'; }
    if (n >= 4 && eq_lit(s, 4, "&lt;")) { *used = 4; return '<'; }
    if (n >= 4 && eq_lit(s, 4, "&gt;")) { *used = 4; return '>'; }
    if (n >= 6 && eq_lit(s, 6, "&nbsp;")) { *used = 6; return ' '; }
    if (n >= 6 && eq_lit(s, 6, "&quot;")) { *used = 6; return '"'; }
    if (n >= 6 && eq_lit(s, 6, "&apos;")) { *used = 6; return '\''; }
    if (n >= 3 && s[1] == '#') {
        int i = 2;
        int hex = 0;
        int v = 0;
        if (s[i] == 'x' || s[i] == 'X') { hex = 1; i++; }
        int any = 0;
        while (i < n && s[i] != ';') {
            int d = hex ? hex_val(s[i]) : (s[i] >= '0' && s[i] <= '9' ? s[i] - '0' : -1);
            if (d < 0) break;
            v = hex ? (v * 16 + d) : (v * 10 + d);
            any = 1;
            i++;
        }
        if (any && i < n && s[i] == ';') {
            *used = i + 1;
            if (v == 160) return ' ';
            if (v >= 32 && v <= 126) return v;
            int fold = ascii_fold(v);
            if (fold) return fold;
            return '?';
        }
    }
    *used = 1;
    return '&';
}

static int collapse_text(const char *s, int n, char *out, int cap, int preserve) {
    if (preserve) {
        int c = n < cap ? n : cap;
        if (c > 0) memcpy(out, s, (size_t)c);
        return c;
    }
    int w = 0;
    int sp = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            sp = 1;
            continue;
        }
        if (sp && w > 0 && w < cap) out[w++] = ' ';
        sp = 0;
        if (w < cap) out[w++] = c;
    }
    if (w == 0 && n > 0 && cap > 0) out[w++] = ' ';
    else if (sp && w > 0 && w < cap) out[w++] = ' ';
    return w;
}

static void add_text_node(int parent, const char *s, int n, int preserve) {
    if (!parent || n <= 0) return;
    /* The shell task stack is 16KB. A 12KB frame here overflows it and
       page-faults while mode 13h is up. */
    int cn = collapse_text(s, n, g_fold, RAW_MAX, preserve);
    if (cn <= 0) return;
    if (!preserve && cn == 1 && g_fold[0] == ' ') {
        /* keep a single spacer so adjacent inlines do not glue together */
    }
    int off = pool_add(g_fold, cn);
    if (off < 0) return;
    int t = new_node();
    if (!t) { g_stop = 1; return; }
    g_nodes[t].kind = 1;
    g_nodes[t].preserve = (uint8_t)(preserve ? 1 : 0);
    g_nodes[t].text_off = (uint16_t)off;
    g_nodes[t].text_len = (uint16_t)cn;
    add_child(parent, t);
}

static void store_attr(uint16_t *off, uint16_t *len, const char *s, int n) {
    int o = pool_add(s, n);
    if (o < 0) return;
    *off = (uint16_t)o;
    *len = (uint16_t)n;
}

static int decode_attr(const char *s, int n, char *out, int cap) {
    int w = 0;
    int i = 0;
    if (!s || n < 0) n = 0;
    if (!out || cap < 1) return 0;
    while (i < n && w + 1 < cap) {
        int used = 1;
        int ch = (unsigned char)s[i];
        if (s[i] == '&') ch = decode_entity(s + i, n - i, &used);
        if (used < 1) used = 1;
        out[w++] = (char)ch;
        i += used;
    }
    out[w] = 0;
    return w;
}

static void apply_attr(int node, const char *name, int nn, const char *val, int vn) {
    int decoded = 0;
    if (eq_lit(name, nn, "href") || eq_lit(name, nn, "value") || eq_lit(name, nn, "alt")) {
        decoded = decode_attr(val, vn, g_fold, RAW_MAX);
        val = g_fold;
        vn = decoded;
    }
    if (eq_lit(name, nn, "id")) store_attr(&g_nodes[node].id_off, &g_nodes[node].id_len, val, vn);
    else if (eq_lit(name, nn, "class")) store_attr(&g_nodes[node].class_off, &g_nodes[node].class_len, val, vn);
    else if (eq_lit(name, nn, "href")) store_attr(&g_nodes[node].href_off, &g_nodes[node].href_len, val, vn);
    else if (eq_lit(name, nn, "style")) store_attr(&g_nodes[node].style_off, &g_nodes[node].style_len, val, vn);
    else if (eq_lit(name, nn, "onclick")) store_attr(&g_nodes[node].click_off, &g_nodes[node].click_len, val, vn);
    else if (eq_lit(name, nn, "value") || eq_lit(name, nn, "alt"))
        store_attr(&g_nodes[node].text_off, &g_nodes[node].text_len, val, vn);
    else if (eq_lit(name, nn, "hidden")) g_nodes[node].hidden = 1;
    else if (eq_lit(name, nn, "type")) {
        if (eq_lit(val, vn, "hidden")) g_nodes[node].hidden = 1;
        else if (eq_lit(val, vn, "button") || eq_lit(val, vn, "submit") || eq_lit(val, vn, "reset"))
            g_nodes[node].input_btn = 1;
    }
}

static void parse_attrs(int node, const char *tag, int n) {
    int i = 0;
    while (i < n && tag[i] != ' ' && tag[i] != '\t' && tag[i] != '\n' && tag[i] != '/' && tag[i] != '>') i++;
    while (i < n) {
        while (i < n && (tag[i] == ' ' || tag[i] == '\t' || tag[i] == '\n' || tag[i] == '\r')) i++;
        if (i >= n || tag[i] == '/' || tag[i] == '>') break;
        int ns = i;
        while (i < n && tag[i] != '=' && tag[i] != ' ' && tag[i] != '\t' && tag[i] != '/' && tag[i] != '>') i++;
        int nn = i - ns;
        while (i < n && (tag[i] == ' ' || tag[i] == '\t')) i++;
        if (i < n && tag[i] == '=') {
            i++;
            while (i < n && (tag[i] == ' ' || tag[i] == '\t')) i++;
            if (i < n && (tag[i] == '"' || tag[i] == '\'')) {
                char q = tag[i++];
                int vs = i;
                while (i < n && tag[i] != q) i++;
                apply_attr(node, tag + ns, nn, tag + vs, i - vs);
                if (i < n) i++;
            } else {
                int vs = i;
                while (i < n && tag[i] != ' ' && tag[i] != '\t' && tag[i] != '/' && tag[i] != '>') i++;
                apply_attr(node, tag + ns, nn, tag + vs, i - vs);
            }
        } else {
            apply_attr(node, tag + ns, nn, "", 0);
        }
    }
}

static int ancestor_pre(int node) {
    while (node) {
        if (g_nodes[node].tag == RB_TAG_PRE || g_nodes[node].tag == RB_TAG_CODE) return 1;
        node = g_nodes[node].parent;
    }
    return 0;
}

static const char *skip_ws(const char *s, const char *e) {
    while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
    return s;
}

static int ident_is(const char *s, const char *e, const char *lit, const char **adv) {
    int n = (int)strlen(lit);
    if (s + n > e || memcmp(s, lit, (size_t)n) != 0) return 0;
    if (s + n < e) {
        char c = s[n];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9')) return 0;
    }
    *adv = s + n;
    return 1;
}

static const char *parse_js_string(const char *s, const char *e, char *out, int cap, int *olen) {
    s = skip_ws(s, e);
    *olen = 0;
    if (s >= e) return s;
    if (*s != '"' && *s != '\'') return s;
    char q = *s++;
    int n = 0;
    while (s < e && *s != q) {
        char c = *s++;
        if (c == '\\' && s < e) {
            char e2 = *s++;
            if (e2 == 'n') c = '\n';
            else if (e2 == 't') c = ' ';
            else c = e2;
        }
        if (n + 1 < cap) out[n++] = c;
    }
    if (s < e && *s == q) s++;
    out[n] = 0;
    *olen = n;
    return s;
}

static void kill_tree(int node) {
    if (!node) return;
    int c = g_nodes[node].first;
    g_nodes[node].first = 0;
    g_nodes[node].dead = 1;
    while (c) {
        int n = g_nodes[c].next;
        kill_tree(c);
        c = n;
    }
}

static int set_elem_text(int node, const char *s, int n) {
    if (!node || g_nodes[node].dead) return 0;
    if (g_nodes[node].tag == RB_TAG_INPUT || g_nodes[node].tag == RB_TAG_TEXTAREA) {
        int off = pool_add(s, n);
        if (off < 0) return 0;
        g_nodes[node].text_off = (uint16_t)off;
        g_nodes[node].text_len = (uint16_t)n;
        return 1;
    }
    int c = g_nodes[node].first;
    g_nodes[node].first = 0;
    while (c) {
        int nx = g_nodes[c].next;
        kill_tree(c);
        c = nx;
    }
    int off = pool_add(s, n);
    if (off < 0) return 0;
    int t = new_node();
    if (!t) return 0;
    g_nodes[t].kind = 1;
    g_nodes[t].text_off = (uint16_t)off;
    g_nodes[t].text_len = (uint16_t)n;
    add_child(node, t);
    return 1;
}

static void set_nav(const char *s, int n) {
    if (n >= (int)sizeof(g_nav)) n = (int)sizeof(g_nav) - 1;
    if (n < 0) n = 0;
    if (n > 0) memcpy(g_nav, s, (size_t)n);
    g_nav[n] = 0;
    g_nav_len = n;
}

static int run_one(const char *start, const char *e) {
    const char *s = skip_ws(start, e);
    const char *adv = s;
    if (ident_is(s, e, "document", &adv)) {
        s = skip_ws(adv, e);
        if (s >= e || *s != '.') return 0;
        s = skip_ws(s + 1, e);
        if (!ident_is(s, e, "getElementById", &adv)) return 0;
        s = skip_ws(adv, e);
        if (s >= e || *s != '(') return 0;
        s++;
        char id[64];
        int idn = 0;
        s = parse_js_string(s, e, id, (int)sizeof(id), &idn);
        s = skip_ws(s, e);
        if (s >= e || *s != ')') return 0;
        s = skip_ws(s + 1, e);
        if (s >= e || *s != '.') return 0;
        s = skip_ws(s + 1, e);
        int node = 0;
        for (int i = 1; i < g_node_n; i++) {
            if (g_nodes[i].dead || g_nodes[i].id_len != idn) continue;
            if (memcmp(g_pool + g_nodes[i].id_off, id, (size_t)idn) == 0) { node = i; break; }
        }
        if (!node) return 0;
        if (ident_is(s, e, "textContent", &adv) || ident_is(s, e, "innerText", &adv) ||
            ident_is(s, e, "innerHTML", &adv)) {
            s = skip_ws(adv, e);
            if (s >= e || *s != '=') return 0;
            char val[240];
            int vn = 0;
            parse_js_string(s + 1, e, val, (int)sizeof(val), &vn);
            return set_elem_text(node, val, vn);
        }
        if (ident_is(s, e, "style", &adv)) {
            s = skip_ws(adv, e);
            if (s >= e || *s != '.') return 0;
            s = skip_ws(s + 1, e);
            int kind = 0;
            if (ident_is(s, e, "color", &adv)) kind = SET_COLOR;
            else if (ident_is(s, e, "backgroundColor", &adv) || ident_is(s, e, "background", &adv)) kind = SET_BG;
            else if (ident_is(s, e, "display", &adv)) kind = SET_DISP;
            else return 0;
            s = skip_ws(adv, e);
            if (s >= e || *s != '=') return 0;
            char val[64];
            int vn = 0;
            parse_js_string(s + 1, e, val, (int)sizeof(val), &vn);
            if (kind == SET_DISP) {
                if (eq_lit(val, vn, "none")) g_nodes[node].js_display = DISP_NONE;
                else if (eq_lit(val, vn, "block")) g_nodes[node].js_display = DISP_BLOCK;
                else g_nodes[node].js_display = DISP_INLINE;
                g_nodes[node].js_mask |= SET_DISP;
                return 1;
            }
            int col = 0;
            if (!parse_color(val, vn, &col)) return 0;
            if (kind == SET_COLOR) g_nodes[node].js_color = (uint8_t)col;
            else g_nodes[node].js_bg = (uint8_t)col;
            g_nodes[node].js_mask |= (uint16_t)kind;
            return 1;
        }
        return 0;
    }
    s = skip_ws(start, e);
    if (ident_is(s, e, "window", &adv)) {
        s = skip_ws(adv, e);
        if (s < e && *s == '.') s = skip_ws(s + 1, e);
    }
    if (!ident_is(s, e, "location", &adv)) return 0;
    s = skip_ws(adv, e);
    if (s < e && *s == '.') {
        s = skip_ws(s + 1, e);
        if (!ident_is(s, e, "href", &adv)) return 0;
        s = skip_ws(adv, e);
    }
    if (s >= e || *s != '=') return 0;
    char url[400];
    int un = 0;
    parse_js_string(s + 1, e, url, (int)sizeof(url), &un);
    if (un <= 0) return 0;
    set_nav(url, un);
    return 2;
}

static const char *stmt_end(const char *s, const char *e) {
    int depth = 0;
    char q = 0;
    while (s < e) {
        char c = *s;
        if (q) {
            if (c == '\\' && s + 1 < e) { s += 2; continue; }
            if (c == q) q = 0;
            s++;
            continue;
        }
        if (c == '"' || c == '\'') { q = c; s++; continue; }
        if (c == '{') depth++;
        else if (c == '}') { if (depth) depth--; }
        else if (c == ';' && depth == 0) return s;
        s++;
    }
    return e;
}

static int run_script(const char *s, int n) {
    const char *p = s;
    const char *e = s + n;
    int result = 0;
    while (p < e) {
        p = skip_ws(p, e);
        if (p >= e) break;
        if (p + 1 < e && p[0] == '/' && p[1] == '/') {
            while (p < e && *p != '\n') p++;
            continue;
        }
        if (p + 1 < e && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < e && !(p[0] == '*' && p[1] == '/')) p++;
            if (p + 1 < e) p += 2;
            continue;
        }
        const char *adv = p;
        if (ident_is(p, e, "function", &adv)) {
            p = adv;
            int depth = 0;
            int seen = 0;
            char q = 0;
            while (p < e) {
                char c = *p++;
                if (q) { if (c == q) q = 0; continue; }
                if (c == '"' || c == '\'') { q = c; continue; }
                if (c == '{') { depth++; seen = 1; }
                else if (c == '}') {
                    if (depth) depth--;
                    if (seen && depth == 0) break;
                } else if (c == ';' && depth == 0 && !seen) break;
            }
            continue;
        }
        const char *end = stmt_end(p, e);
        int r = run_one(p, end);
        if (r == 2) result = 2;
        else if (r == 1 && result == 0) result = 1;
        p = end < e ? end + 1 : e;
    }
    return result;
}

static int close_matches(const char *html, int i, int len, int tag) {
    if (i + 2 >= len || html[i] != '<' || html[i + 1] != '/') return 0;
    int j = i + 2;
    while (j < len && (html[j] == ' ' || html[j] == '\t')) j++;
    int ns = j;
    while (j < len && html[j] != '>' && html[j] != ' ' && html[j] != '\t') j++;
    return tag_of(html + ns, j - ns) == tag;
}

static void raw_child(int node, const char **s, int *n) {
    *s = "";
    *n = 0;
    int c = g_nodes[node].first;
    if (!c || g_nodes[c].kind != 1) return;
    *s = g_pool + g_nodes[c].text_off;
    *n = g_nodes[c].text_len;
}

static void on_close(int node) {
    if (!node) return;
    int tag = g_nodes[node].tag;
    if (tag == RB_TAG_STYLE || tag == RB_TAG_SCRIPT) {
        const char *s;
        int n = 0;
        raw_child(node, &s, &n);
        if (tag == RB_TAG_STYLE) parse_css(s, n);
        else run_script(s, n);
    } else if (tag == RB_TAG_TITLE) {
        text_of(node, g_title, (int)sizeof(g_title));
    }
}

static void parse_html(const char *html, int len) {
    int stack[48];
    int sp = 0;
    stack[0] = 1;
    int i = 0;
    while (i < len && !g_stop) {
        int cur = stack[sp];
        int raw = is_raw_tag(g_nodes[cur].tag) && g_nodes[cur].tag != RB_TAG_DOC;
        if (raw) {
            if (close_matches(html, i, len, g_nodes[cur].tag)) {
                on_close(cur);
                if (sp > 0) sp--;
                while (i < len && html[i] != '>') i++;
                if (i < len) i++;
                continue;
            }
            int rn = 0;
            while (i < len && !close_matches(html, i, len, g_nodes[cur].tag)) {
                char ch = html[i];
                int used = 1;
                if (ch == '&') ch = (char)decode_entity(html + i, len - i, &used);
                if (rn < RAW_MAX) g_raw[rn++] = ch;
                i += used;
            }
            if (rn > 0) {
                int preserve = g_nodes[cur].tag == RB_TAG_TEXTAREA || g_nodes[cur].tag == RB_TAG_PRE;
                if (g_nodes[cur].tag == RB_TAG_TEXTAREA || g_nodes[cur].tag == RB_TAG_TITLE) {
                    int off = pool_add(g_raw, rn);
                    if (off >= 0) {
                        if (g_nodes[cur].tag == RB_TAG_TEXTAREA) {
                            g_nodes[cur].text_off = (uint16_t)off;
                            g_nodes[cur].text_len = (uint16_t)rn;
                        } else {
                            int t = new_node();
                            if (t) {
                                g_nodes[t].kind = 1;
                                g_nodes[t].text_off = (uint16_t)off;
                                g_nodes[t].text_len = (uint16_t)rn;
                                g_nodes[t].preserve = (uint8_t)preserve;
                                add_child(cur, t);
                            }
                        }
                    }
                } else {
                    int off = pool_add(g_raw, rn);
                    if (off >= 0) {
                        int t = new_node();
                        if (t) {
                            g_nodes[t].kind = 1;
                            g_nodes[t].text_off = (uint16_t)off;
                            g_nodes[t].text_len = (uint16_t)rn;
                            add_child(cur, t);
                        }
                    }
                }
            }
            continue;
        }
        if (html[i] != '<') {
            int rn = 0;
            while (i < len && html[i] != '<') {
                char ch = html[i];
                int used = 1;
                if (ch == '&') ch = (char)decode_entity(html + i, len - i, &used);
                if (rn < RAW_MAX) g_raw[rn++] = ch;
                i += used > 0 ? used : 1;
            }
            add_text_node(cur, g_raw, rn, ancestor_pre(cur));
            continue;
        }
        if (i + 3 < len && html[i + 1] == '!' && html[i + 2] == '-' && html[i + 3] == '-') {
            i += 4;
            while (i + 2 < len && !(html[i] == '-' && html[i + 1] == '-' && html[i + 2] == '>')) i++;
            if (i + 2 < len) i += 3;
            else i = len;
            continue;
        }
        if (i + 1 < len && html[i + 1] == '!') {
            while (i < len && html[i] != '>') i++;
            if (i < len) i++;
            continue;
        }
        if (i + 1 < len && html[i + 1] == '/') {
            int j = i + 2;
            while (j < len && (html[j] == ' ' || html[j] == '\t')) j++;
            int ns = j;
            while (j < len && html[j] != '>' && html[j] != ' ' && html[j] != '\t') j++;
            int tag = tag_of(html + ns, j - ns);
            while (sp > 0 && g_nodes[stack[sp]].tag != tag) {
                on_close(stack[sp]);
                sp--;
            }
            if (sp > 0 && g_nodes[stack[sp]].tag == tag) {
                on_close(stack[sp]);
                sp--;
            }
            while (i < len && html[i] != '>') i++;
            if (i < len) i++;
            continue;
        }
        int ts = i + 1;
        int te = ts;
        int inq = 0;
        char q = 0;
        while (te < len) {
            char c = html[te];
            if (inq) { if (c == q) inq = 0; te++; continue; }
            if (c == '"' || c == '\'') { inq = 1; q = c; te++; continue; }
            if (c == '>') break;
            te++;
        }
        int name_n = 0;
        while (ts + name_n < te && html[ts + name_n] != ' ' && html[ts + name_n] != '\t' &&
               html[ts + name_n] != '\n' && html[ts + name_n] != '/' && html[ts + name_n] != '>')
            name_n++;
        int self = 0;
        int slash = te - 1;
        while (slash > ts && (html[slash] == ' ' || html[slash] == '\t')) slash--;
        if (slash > ts && html[slash] == '/') self = 1;
        int tag = tag_of(html + ts, name_n);
        /* <body> (and any other visible tag) ends an unclosed <head>, otherwise
           the whole page stays display:none and paints as an empty frame. */
        if (tag != RB_TAG_TITLE && tag != RB_TAG_STYLE && tag != RB_TAG_SCRIPT && tag != RB_TAG_OTHER) {
            while (sp > 0 && (g_nodes[stack[sp]].tag == RB_TAG_HEAD ||
                              g_nodes[stack[sp]].tag == RB_TAG_TITLE ||
                              g_nodes[stack[sp]].tag == RB_TAG_STYLE ||
                              g_nodes[stack[sp]].tag == RB_TAG_SCRIPT)) {
                on_close(stack[sp]);
                sp--;
            }
        }
        int node = new_node();
        if (!node) { g_stop = 1; break; }
        g_nodes[node].tag = (uint8_t)tag;
        parse_attrs(node, html + ts, te - ts);
        add_child(cur, node);
        i = te < len ? te + 1 : len;
        if (!self && !is_void_tag(tag)) {
            if (sp + 1 < 48) stack[++sp] = node;
        } else if (tag == RB_TAG_IMG || tag == RB_TAG_INPUT) {
            /* attrs already stored value/alt on the node */
        }
    }
    while (sp > 0) {
        on_close(stack[sp]);
        sp--;
    }
}

static void reset_doc(void) {
    memset(g_nodes, 0, sizeof(g_nodes));
    g_node_n = 1;
    g_box_n = 0;
    g_rule_n = 0;
    g_focus_n = 0;
    g_pool_n = 0;
    g_nav_len = 0;
    g_nav[0] = 0;
    g_title[0] = 0;
    g_page_h = 0;
    g_stop = 0;
    int root = new_node();
    g_nodes[root].tag = RB_TAG_DOC;
}

static int tag_at(const char *s, int n, int i, const char *lit) {
    if (i >= n || s[i] != '<') return 0;
    int j = i + 1;
    int k = 0;
    while (lit[k]) {
        if (j >= n || lower_ch((unsigned char)s[j]) != (unsigned char)lit[k]) return 0;
        j++;
        k++;
    }
    if (j < n) {
        int c = (unsigned char)s[j];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':')
            return 0;
    }
    return 1;
}

static int skip_gt(const char *s, int n, int i) {
    int q = 0;
    while (i < n) {
        char c = s[i++];
        if (q) {
            if (c == q) q = 0;
            continue;
        }
        if (c == '"' || c == '\'') { q = c; continue; }
        if (c == '>') break;
    }
    return i;
}

static int skip_element(const char *s, int n, int i, const char *lit) {
    int llen = 0;
    while (lit[llen]) llen++;
    i = skip_gt(s, n, i);
    while (i + llen + 2 < n) {
        if (s[i] == '<' && i + 1 < n && s[i + 1] == '/') {
            int j = i + 2;
            while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) j++;
            int k = 0;
            int ok = 1;
            while (lit[k]) {
                if (j >= n || lower_ch((unsigned char)s[j]) != (unsigned char)lit[k]) { ok = 0; break; }
                j++;
                k++;
            }
            if (ok && (j >= n || !((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z') ||
                                   (s[j] >= '0' && s[j] <= '9') || s[j] == '-' || s[j] == '_' || s[j] == ':')))
                return skip_gt(s, n, j);
        }
        i++;
    }
    return n;
}

int rb_compact_markup(const char *src, int n, char *dst, int cap) {
    if (!dst || cap <= 0) return 0;
    if (!src || n < 0) n = 0;
    int w = 0;
    int i = 0;
    while (i < n && w + 1 < cap) {
        const char *drop = 0;
        if (tag_at(src, n, i, "script")) drop = "script";
        else if (tag_at(src, n, i, "style")) drop = "style";
        if (drop) {
            i = skip_element(src, n, i, drop);
            continue;
        }
        dst[w++] = src[i++];
    }
    dst[w] = 0;
    return w;
}

int rb_open(const char *html, int len) {
    reset_doc();
    if (!html) html = "";
    if (len < 0) len = (int)strlen(html);
    parse_html(html, len);
    do_layout();
    return 0;
}

void rb_relayout(void) { do_layout(); }
int rb_page_height(void) { return g_page_h; }
int rb_page_bg(void) { return g_page_bg; }

int rb_title(char *out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = (int)strlen(g_title);
    if (n >= cap) n = cap - 1;
    memcpy(out, g_title, (size_t)n);
    out[n] = 0;
    return n;
}

int rb_find_id(const char *id) {
    if (!id) return 0;
    int n = (int)strlen(id);
    for (int i = 1; i < g_node_n; i++) {
        if (g_nodes[i].dead || g_nodes[i].id_len != n) continue;
        if (memcmp(g_pool + g_nodes[i].id_off, id, (size_t)n) == 0) return i;
    }
    return 0;
}

int rb_node_tag(int node) {
    if (node <= 0 || node >= g_node_n) return RB_TAG_OTHER;
    return g_nodes[node].tag;
}

int rb_node_color(int node) {
    if (node <= 0 || node >= g_node_n) return 0;
    return g_nodes[node].color;
}

int rb_node_href(int node, char *out, int cap) {
    if (!out || cap <= 0 || node <= 0 || node >= g_node_n) return 0;
    if (g_nodes[node].tag != RB_TAG_A || g_nodes[node].href_len == 0) return 0;
    int n = g_nodes[node].href_len;
    if (n >= cap) n = cap - 1;
    memcpy(out, g_pool + g_nodes[node].href_off, (size_t)n);
    out[n] = 0;
    return 1;
}

static int collect_rec(int node, char *out, int cap, int pos, int skip_raw) {
    if (!node || g_nodes[node].dead || pos >= cap) return pos;
    if (g_nodes[node].kind == 1) {
        int n = g_nodes[node].text_len;
        if (pos + n > cap) n = cap - pos;
        if (n > 0) memcpy(out + pos, g_pool + g_nodes[node].text_off, (size_t)n);
        return pos + n;
    }
    if ((g_nodes[node].tag == RB_TAG_INPUT || g_nodes[node].tag == RB_TAG_TEXTAREA) &&
        g_nodes[node].text_len && g_nodes[node].first == 0) {
        int n = g_nodes[node].text_len;
        if (pos + n > cap) n = cap - pos;
        if (n > 0) memcpy(out + pos, g_pool + g_nodes[node].text_off, (size_t)n);
        return pos + n;
    }
    if (skip_raw && (g_nodes[node].tag == RB_TAG_SCRIPT || g_nodes[node].tag == RB_TAG_STYLE)) return pos;
    for (int c = g_nodes[node].first; c; c = g_nodes[c].next)
        pos = collect_rec(c, out, cap, pos, 1);
    return pos;
}

int rb_collect_text(int node, char *out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = collect_rec(node, out, cap - 1, 0, 0);
    if (n < 0) n = 0;
    out[n] = 0;
    return n;
}

int rb_box_count(void) { return g_box_n; }
const RbBox *rb_box(int i) {
    if (i < 0 || i >= g_box_n) return 0;
    return &g_box[i];
}

const char *rb_box_text(int i, int *len) {
    if (len) *len = 0;
    if (i < 0 || i >= g_box_n || g_box[i].tlen == 0) return "";
    if (len) *len = g_box[i].tlen;
    return g_pool + g_box[i].toff;
}

int rb_first_box(int node, int *x, int *y, int *w, int *h) {
    for (int i = 0; i < g_box_n; i++) {
        if (g_box[i].node == node && g_box[i].kind != RB_BOX_BG && g_box[i].w > 0 && g_box[i].h > 0) {
            if (x) *x = g_box[i].x;
            if (y) *y = g_box[i].y;
            if (w) *w = g_box[i].w;
            if (h) *h = g_box[i].h;
            return 1;
        }
    }
    return 0;
}

static int clickable(int node) {
    if (node <= 0 || node >= g_node_n) return 0;
    int tag = g_nodes[node].tag;
    if (tag == RB_TAG_A && g_nodes[node].href_len) return 1;
    if (tag == RB_TAG_BUTTON || tag == RB_TAG_INPUT || tag == RB_TAG_TEXTAREA) return 1;
    return 0;
}

int rb_hit(int x, int y) {
    for (int i = g_box_n - 1; i >= 0; i--) {
        RbBox *b = &g_box[i];
        if (b->kind == RB_BOX_BG && !clickable(b->node)) continue;
        if (b->w <= 0 || b->h <= 0) continue;
        if (x >= b->x && y >= b->y && x < b->x + b->w && y < b->y + b->h) return b->node;
    }
    return -1;
}

int rb_focus_count(void) { return g_focus_n; }
int rb_focus_node(int index) {
    if (index < 0 || index >= g_focus_n) return 0;
    return g_focus[index];
}

int rb_focus_index(int node) {
    for (int i = 0; i < g_focus_n; i++) if (g_focus[i] == node) return i;
    return -1;
}

int rb_tab(int index, int backward) {
    if (g_focus_n <= 0) return -1;
    if (index < 0 || index >= g_focus_n) return backward ? g_focus_n - 1 : 0;
    if (backward) return index == 0 ? g_focus_n - 1 : index - 1;
    return index + 1 >= g_focus_n ? 0 : index + 1;
}

int rb_activate(int node) {
    if (node <= 0 || node >= g_node_n || g_nodes[node].dead) return 0;
    int changed = 0;
    if (g_nodes[node].click_len) {
        int r = run_script(g_pool + g_nodes[node].click_off, g_nodes[node].click_len);
        if (r == 2) return 2;
        if (r == 1) changed = 1;
    }
    if (g_nodes[node].tag == RB_TAG_A && g_nodes[node].href_len) {
        set_nav(g_pool + g_nodes[node].href_off, g_nodes[node].href_len);
        return 2;
    }
    return changed;
}

int rb_click(int x, int y) {
    int n = rb_hit(x, y);
    if (n < 0) return -1;
    rb_activate(n);
    return n;
}

int rb_take_nav(char *out, int cap) {
    if (g_nav_len <= 0 || !out || cap <= 0) return 0;
    int n = g_nav_len;
    if (n >= cap) n = cap - 1;
    memcpy(out, g_nav, (size_t)n);
    out[n] = 0;
    g_nav_len = 0;
    g_nav[0] = 0;
    return 1;
}

int rb_is_field(int node) {
    if (node <= 0 || node >= g_node_n || g_nodes[node].hidden) return 0;
    if (g_nodes[node].tag == RB_TAG_TEXTAREA) return 1;
    if (g_nodes[node].tag == RB_TAG_INPUT && !g_nodes[node].input_btn) return 1;
    return 0;
}

static int field_set(int node, const char *s, int n) {
    if (n > 60) n = 60;
    int off = pool_add(s, n);
    if (off < 0) return 0;
    g_nodes[node].text_off = (uint16_t)off;
    g_nodes[node].text_len = (uint16_t)n;
    return 1;
}

int rb_type_char(int node, char ch) {
    if (!rb_is_field(node) || ch < 32 || ch > 126) return 0;
    char buf[64];
    int n = g_nodes[node].text_len;
    if (n > 59) return 0;
    if (n > 0) memcpy(buf, g_pool + g_nodes[node].text_off, (size_t)n);
    buf[n++] = ch;
    buf[n] = 0;
    return field_set(node, buf, n);
}

int rb_backspace(int node) {
    if (!rb_is_field(node)) return 0;
    int n = g_nodes[node].text_len;
    if (n <= 0) return 0;
    char buf[64];
    if (n > 60) n = 60;
    memcpy(buf, g_pool + g_nodes[node].text_off, (size_t)(n - 1));
    return field_set(node, buf, n - 1);
}

static int begins(const char *s, const char *lit) {
    int i = 0;
    while (lit[i]) {
        if (s[i] != lit[i]) return 0;
        i++;
    }
    return 1;
}

static void append_n(char *out, int cap, int *w, const char *s, int n) {
    for (int i = 0; i < n && *w + 1 < cap; i++) out[(*w)++] = s[i];
    if (*w < cap) out[*w] = 0;
}

static void collapse_path(char *path) {
    char tmp[480];
    int n = 0;
    while (path[n] && n + 1 < (int)sizeof(tmp)) { tmp[n] = path[n]; n++; }
    tmp[n] = 0;
    char parts[40][64];
    int np = 0;
    int i = 0;
    if (tmp[0] == '/') i++;
    while (tmp[i] && np < 40) {
        int st = i;
        while (tmp[i] && tmp[i] != '/') i++;
        int len = i - st;
        if (len == 1 && tmp[st] == '.') {
        } else if (len == 2 && tmp[st] == '.' && tmp[st + 1] == '.') {
            if (np) np--;
        } else if (len > 0 && len < 64) {
            memcpy(parts[np], tmp + st, (size_t)len);
            parts[np][len] = 0;
            np++;
        }
        if (tmp[i] == '/') i++;
    }
    int w = 0;
    path[w++] = '/';
    for (int p = 0; p < np; p++) {
        int L = (int)strlen(parts[p]);
        for (int k = 0; k < L && w + 1 < 480; k++) path[w++] = parts[p][k];
        if (p + 1 < np && w + 1 < 480) path[w++] = '/';
    }
    path[w] = 0;
}

static int file_url_to_path(const char *href, char *out, int cap) {
    const char *p = href + 7;
    if (p[0] == '-' && p[1] == '/') p++;
    if (begins(p, "localhost/")) p += 9;
    if (p[0] != '/') {
        int w = 0;
        append_n(out, cap, &w, "/", 1);
        append_n(out, cap, &w, p, (int)strlen(p));
    } else {
        int w = 0;
        append_n(out, cap, &w, p, (int)strlen(p));
    }
    char *hash = out;
    while (*hash && *hash != '#') hash++;
    *hash = 0;
    char *q = out;
    while (*q && *q != '?') q++;
    *q = 0;
    collapse_path(out);
    return 1;
}

int rb_resolve(const char *base, const char *href, char *out, int cap) {
    if (!out || cap < 2) return 0;
    out[0] = 0;
    if (!href || !href[0] || href[0] == '#') return 0;
    while (*href == ' ' || *href == '\t') href++;
    if (begins(href, "file://")) return file_url_to_path(href, out, cap);
    if (begins(href, "http://") || begins(href, "https://")) {
        int w = 0;
        append_n(out, cap, &w, href, (int)strlen(href));
        char *hash = out;
        while (*hash && *hash != '#') hash++;
        *hash = 0;
        return out[0] ? 1 : 0;
    }
    if (!base) base = "/";
    char query[160];
    query[0] = 0;
    const char *q = href;
    while (*q && *q != '?' && *q != '#') q++;
    if (*q == '?') {
        int i = 0;
        while (q[i] && q[i] != '#' && i + 1 < (int)sizeof(query)) { query[i] = q[i]; i++; }
        query[i] = 0;
    }
    int hlen = (int)(q - href);
    int http = begins(base, "http://") || begins(base, "https://");
    if (http) {
        const char *p = base + (begins(base, "https://") ? 8 : 7);
        const char *slash = p;
        while (*slash && *slash != '/' && *slash != '?' && *slash != '#') slash++;
        int origin = (int)(slash - base);
        if (hlen > 0 && href[0] == '/') {
            int w = 0;
            append_n(out, cap, &w, base, origin);
            append_n(out, cap, &w, href, hlen);
        } else {
            const char *path = (*slash == '/') ? slash : "/";
            int path_len = 0;
            if (*slash == '/') {
                while (path[path_len] && path[path_len] != '?' && path[path_len] != '#') path_len++;
            }
            int dir = path_len;
            while (dir > 0 && path[dir - 1] != '/') dir--;
            if (dir == 0) dir = path_len;
            int w = 0;
            append_n(out, cap, &w, base, origin);
            if (*slash == '/') append_n(out, cap, &w, path, dir);
            else append_n(out, cap, &w, "/", 1);
            append_n(out, cap, &w, href, hlen);
        }
        char *path = out;
        if (begins(out, "https://")) path = out + 8;
        else if (begins(out, "http://")) path = out + 7;
        while (*path && *path != '/') path++;
        if (*path == '/') collapse_path(path);
    } else {
        char pathbase[480];
        int w = 0;
        if (begins(base, "file://")) file_url_to_path(base, pathbase, (int)sizeof(pathbase));
        else {
            append_n(pathbase, (int)sizeof(pathbase), &w, base, (int)strlen(base));
        }
        if (hlen > 0 && href[0] == '/') {
            w = 0;
            append_n(out, cap, &w, href, hlen);
        } else {
            int n = (int)strlen(pathbase);
            int dir = n;
            while (dir > 0 && pathbase[dir - 1] != '/') dir--;
            w = 0;
            if (dir == 0) append_n(out, cap, &w, "/", 1);
            else append_n(out, cap, &w, pathbase, dir);
            append_n(out, cap, &w, href, hlen);
        }
        collapse_path(out);
    }
    if (query[0]) {
        int w = (int)strlen(out);
        append_n(out, cap, &w, query, (int)strlen(query));
    }
    return out[0] ? 1 : 0;
}
