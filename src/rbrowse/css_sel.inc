/* rbrowse CSS selectors and stylesheets: interning, selector compiler and
   matcher, rule table with a bucket index, @media / @supports evaluation and
   the stylesheet parser. Included by engine.c after css_decl.inc. */

/* ---- string interning ---------------------------------------------------- */

#define ISTR_MAX LIM(160 * 1024, 8 * 1024 * 1024)
#define MAX_INTERN LIM(8192, 60000)
#define IHASH LIM(16384, 131072)
static char g_istr[ISTR_MAX];
static int g_istr_n;
typedef struct { uint32_t off; uint16_t len; } IEnt;
static IEnt g_ient[MAX_INTERN];
static int g_ient_n;
static uint16_t g_ih[IHASH];
static int g_ient_ua, g_istr_ua;

static uint32_t ihash(const char *s, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

/* Returns the entry id, or -1 when add is 0 and the string is unknown (or the
   table is full). */
static int intern(const char *s, int n, int add) {
    if (n < 0) n = 0;
    uint32_t h = ihash(s, n) & (IHASH - 1);
    for (int probe = 0; probe < IHASH; probe++) {
        int e = g_ih[h];
        if (!e) break;
        e--;
        if (g_ient[e].len == n && memcmp(g_istr + g_ient[e].off, s, (size_t)n) == 0) return e;
        h = (h + 1) & (IHASH - 1);
    }
    if (!add) return -1;
    if (g_ient_n >= MAX_INTERN || g_istr_n + n + 1 >= ISTR_MAX || n > 65000) { SET_OOM(); return -1; }
    int id = g_ient_n++;
    g_ient[id].off = (uint32_t)g_istr_n;
    g_ient[id].len = (uint16_t)n;
    memcpy(g_istr + g_istr_n, s, (size_t)n);
    g_istr[g_istr_n + n] = 0;
    g_istr_n += n + 1;
    g_ih[h] = (uint16_t)(id + 1);
    return id;
}

static const char *intern_str(int id, int *len) {
    if (id < 0 || id >= g_ient_n) { *len = 0; return ""; }
    *len = g_ient[id].len;
    return g_istr + g_ient[id].off;
}

static void intern_rebuild_hash(void) {
    memset(g_ih, 0, sizeof(g_ih));
    for (int e = 0; e < g_ient_n; e++) {
        uint32_t h = ihash(g_istr + g_ient[e].off, g_ient[e].len) & (IHASH - 1);
        while (g_ih[h]) h = (h + 1) & (IHASH - 1);
        g_ih[h] = (uint16_t)(e + 1);
    }
}

/* ---- atoms ---------------------------------------------------------------- */

enum { AT_END = 1, AT_TAG, AT_ID, AT_CLASS, AT_ATTR, AT_PSEUDO, AT_NTH, AT_NOT, AT_IS, AT_STOP,
       AT_NEVER, AT_UNIV };

/* pseudo-class kinds */
enum { PS_ROOT = 1, PS_EMPTY, PS_LINK, PS_CHECKED, PS_DISABLED, PS_ENABLED, PS_REQUIRED, PS_OPTIONAL,
       PS_BTN, PS_FIRSTC, PS_LASTC, PS_ONLYC, PS_FIRSTT, PS_LASTT, PS_ONLYT, PS_TARGET_NEVER };

/* attribute ops */
enum { AO_EXISTS = 0, AO_EQ, AO_WORD, AO_DASH, AO_PREFIX, AO_SUFFIX, AO_CONTAINS, AO_ICASE = 0x80 };

typedef struct {
    uint8_t type;
    uint8_t op;
    int16_t b;
    int32_t a;
} Atom;

#define MAX_ATOMS LIM(48000, 1000000)
static Atom g_atoms[MAX_ATOMS];
static int g_atom_n;

/* ---- rules ----------------------------------------------------------------- */

typedef struct {
    uint32_t order;      /* (sheet ordinal << 20) | index: cascade tie-break */
    uint32_t spec;
    uint32_t sel;        /* first atom of the rightmost compound */
    uint32_t decl;
    uint16_t ndecl;
    uint16_t next;       /* bucket chain */
    uint8_t pseudo;      /* 0 element, 1 ::before, 2 ::after */
    uint8_t origin;      /* 0 user agent, 1 author */
} CRule;

#define MAX_CRULES LIM(16000, 60000)
static CRule g_crule[MAX_CRULES];
static int g_crule_n;
static int g_crule_ua;

/* Rule index: heads of per-key chains (rule index + 1; 0 = none). */
static uint16_t g_rh_tag[RB_TAG_OTHER + 1];
static uint16_t g_rh_id[MAX_INTERN];
static uint16_t g_rh_cls[MAX_INTERN];
static uint16_t g_rh_any;

/* ---- node helpers for matching ----------------------------------------------- */

static int el_parent(int node) {
    int p = g_nodes[node].parent;
    if (!p || g_nodes[p].kind != 0 || g_nodes[p].tag == RB_TAG_DOC) return 0;
    return p;
}

static int el_prev(int node) {
    for (int p = g_nodes[node].prev; p; p = g_nodes[p].prev)
        if (g_nodes[p].kind == 0 && !g_nodes[p].dead && !g_nodes[p].pseudo) return p;
    return 0;
}

static int el_next(int node) {
    for (int p = g_nodes[node].next; p; p = g_nodes[p].next)
        if (g_nodes[p].kind == 0 && !g_nodes[p].dead && !g_nodes[p].pseudo) return p;
    return 0;
}

/* Look an attribute up: the node's dedicated fields first, then its extra
   attribute blob ("name\0value\0" pairs). Returns 1 and the value span. */
static int attr_get(int node, const char *name, int nn, const char **v, int *vn) {
    Node *nd = &g_nodes[node];
    if (eq_lit(name, nn, "id")) { if (!nd->id_len) return 0; *v = g_pool + nd->id_off; *vn = (int)nd->id_len; return 1; }
    if (eq_lit(name, nn, "class")) { if (!nd->class_len) return 0; *v = g_pool + nd->class_off; *vn = (int)nd->class_len; return 1; }
    if (eq_lit(name, nn, "href")) { if (!nd->href_len) return 0; *v = g_pool + nd->href_off; *vn = (int)nd->href_len; return 1; }
    if (eq_lit(name, nn, "style")) { if (!nd->style_len) return 0; *v = g_pool + nd->style_off; *vn = (int)nd->style_len; return 1; }
    if (eq_lit(name, nn, "name")) { if (!nd->name_len) return 0; *v = g_pool + nd->name_off; *vn = (int)nd->name_len; return 1; }
    if (eq_lit(name, nn, "action")) { if (!nd->action_len) return 0; *v = g_pool + nd->action_off; *vn = (int)nd->action_len; return 1; }
    if (eq_lit(name, nn, "onclick")) { if (!nd->click_len) return 0; *v = g_pool + nd->click_off; *vn = (int)nd->click_len; return 1; }
    /* text_off holds alt on an image and value on anything else that has one (a textarea's is its content) */
    if ((eq_lit(name, nn, "alt") && (nd->tag == RB_TAG_IMG || nd->tag == RB_TAG_AREA)) ||
        (eq_lit(name, nn, "value") && nd->tag != RB_TAG_IMG && nd->tag != RB_TAG_AREA && nd->tag != RB_TAG_TEXTAREA)) {
        if (!nd->text_len) return 0;
        *v = g_pool + nd->text_off; *vn = (int)nd->text_len; return 1;
    }
    if (eq_lit(name, nn, "hidden")) { *v = ""; *vn = 0; return nd->hidden; }
    if (eq_lit(name, nn, "disabled")) { *v = ""; *vn = 0; return nd->disabled; }
    if (eq_lit(name, nn, "checked")) { *v = ""; *vn = 0; return nd->checked; }
    if (nd->attr_len) {
        const char *p = g_pool + nd->attr_off;
        const char *e = p + nd->attr_len;
        while (p < e) {
            int ln = (int)strlen(p);
            const char *val = p + ln + 1;
            int lv = (int)strlen(val);
            if (ln == nn && eq_lit(p, ln, name) ) { *v = val; *vn = lv; return 1; }
            /* name stored lowercase; eq_lit lowercases only its first arg */
            if (ln == nn) {
                int same = 1;
                for (int i = 0; i < ln; i++) if (p[i] != lower_ch((unsigned char)name[i])) { same = 0; break; }
                if (same) { *v = val; *vn = lv; return 1; }
            }
            p = val + lv + 1;
        }
    }
    return 0;
}

static int token_in_list(const char *list, int ln, const char *tok, int tn, int icase) {
    int i = 0;
    while (i < ln) {
        while (i < ln && (list[i] == ' ' || list[i] == '\t' || list[i] == '\n')) i++;
        int st = i;
        while (i < ln && list[i] != ' ' && list[i] != '\t' && list[i] != '\n') i++;
        if (i - st == tn) {
            int same = 1;
            for (int k = 0; k < tn; k++) {
                int a = (unsigned char)list[st + k], b = (unsigned char)tok[k];
                if (icase) { a = lower_ch(a); b = lower_ch(b); }
                if (a != b) { same = 0; break; }
            }
            if (same) return 1;
        }
    }
    return 0;
}

static int streq_ic(const char *a, int an, const char *b, int bn, int icase) {
    if (an != bn) return 0;
    for (int i = 0; i < an; i++) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (icase) { x = lower_ch(x); y = lower_ch(y); }
        if (x != y) return 0;
    }
    return 1;
}

static int match_compound(int node, int ai);

/* Match nested compound lists (:not / :is): [ai, STOP) holds compounds each
   ending in AT_END. Returns 1 when any compound matches. */
static int match_list_any(int node, int ai) {
    while (g_atoms[ai].type != AT_STOP) {
        if (match_compound(node, ai)) return 1;
        while (g_atoms[ai].type != AT_END) ai++;
        ai++;
    }
    return 0;
}

static int nth_index(int node, int of_type, int from_end) {
    int idx = 1;
    int p = from_end ? el_next(node) : el_prev(node);
    while (p) {
        if (!of_type || g_nodes[p].tag == g_nodes[node].tag) idx++;
        p = from_end ? el_next(p) : el_prev(p);
    }
    return idx;
}

static int nth_match(int a, int b, int idx) {
    /* idx = a*n + b for some n >= 0 */
    if (a == 0) return idx == b;
    int d = idx - b;
    if (d % a != 0) return 0;
    return d / a >= 0;
}

static int match_compound(int node, int ai) {
    Node *nd = &g_nodes[node];
    for (;; ai++) {
        const Atom *a = &g_atoms[ai];
        switch (a->type) {
        case AT_END: case AT_STOP: return 1;
        case AT_NEVER: return 0;
        case AT_UNIV: break;
        case AT_TAG:
            if (nd->tag != a->a) return 0;
            if (a->a == RB_TAG_OTHER && (int16_t)nd->name_hash != a->b) return 0;
            break;
        case AT_ID: {
            if (!nd->id_len) return 0;
            int l; const char *s = intern_str(a->a, &l);
            if ((int)nd->id_len != l || memcmp(g_pool + nd->id_off, s, (size_t)l) != 0) return 0;
            break;
        }
        case AT_CLASS: {
            if (!nd->class_len) return 0;
            int l; const char *s = intern_str(a->a, &l);
            if (!token_in_list(g_pool + nd->class_off, (int)nd->class_len, s, l, 0)) return 0;
            break;
        }
        case AT_ATTR: {
            int nl, vl = 0;
            const char *nm = intern_str(a->a, &nl);
            const char *v;
            int vn;
            if (!attr_get(node, nm, nl, &v, &vn)) return 0;
            int op = a->op & 0x7F, ic = (a->op & AO_ICASE) != 0;
            if (op == AO_EXISTS) break;
            const char *want = intern_str(a->b, &vl);
            if (op == AO_EQ) { if (!streq_ic(v, vn, want, vl, ic)) return 0; }
            else if (op == AO_WORD) { if (!token_in_list(v, vn, want, vl, ic)) return 0; }
            else if (op == AO_DASH) {
                if (!(streq_ic(v, vn, want, vl, ic) ||
                      (vn > vl && streq_ic(v, vl, want, vl, ic) && v[vl] == '-'))) return 0;
            } else if (op == AO_PREFIX) { if (vl == 0 || vn < vl || !streq_ic(v, vl, want, vl, ic)) return 0; }
            else if (op == AO_SUFFIX) { if (vl == 0 || vn < vl || !streq_ic(v + vn - vl, vl, want, vl, ic)) return 0; }
            else if (op == AO_CONTAINS) {
                if (vl == 0) return 0;
                int found = 0;
                for (int i = 0; i + vl <= vn; i++) if (streq_ic(v + i, vl, want, vl, ic)) { found = 1; break; }
                if (!found) return 0;
            }
            break;
        }
        case AT_PSEUDO:
            switch (a->b) {
            case PS_ROOT: if (nd->tag != RB_TAG_HTML) return 0; break;
            case PS_EMPTY: {
                for (int c = nd->first; c; c = g_nodes[c].next)
                    if (!g_nodes[c].dead && !g_nodes[c].pseudo && (g_nodes[c].kind == 0 || g_nodes[c].text_len)) return 0;
                break;
            }
            case PS_LINK: if (!((nd->tag == RB_TAG_A) && nd->href_len)) return 0; break;
            case PS_CHECKED: if (!nd->checked) return 0; break;
            case PS_DISABLED: if (!nd->disabled) return 0; break;
            case PS_ENABLED:
                if (nd->disabled || !(nd->tag == RB_TAG_INPUT || nd->tag == RB_TAG_BUTTON ||
                                      nd->tag == RB_TAG_TEXTAREA || nd->tag == RB_TAG_SELECT)) return 0;
                break;
            case PS_REQUIRED: { const char *v; int vn; if (!attr_get(node, "required", 8, &v, &vn)) return 0; break; }
            case PS_OPTIONAL: { const char *v; int vn; if (attr_get(node, "required", 8, &v, &vn)) return 0; break; }
            case PS_BTN: if (!(nd->tag == RB_TAG_BUTTON || (nd->tag == RB_TAG_INPUT && nd->input_btn))) return 0; break;
            case PS_FIRSTC: if (el_prev(node)) return 0; break;
            case PS_LASTC: if (el_next(node)) return 0; break;
            case PS_ONLYC: if (el_prev(node) || el_next(node)) return 0; break;
            case PS_FIRSTT: if (nth_index(node, 1, 0) != 1) return 0; break;
            case PS_LASTT: if (nth_index(node, 1, 1) != 1) return 0; break;
            case PS_ONLYT: if (nth_index(node, 1, 0) != 1 || nth_index(node, 1, 1) != 1) return 0; break;
            default: return 0;
            }
            break;
        case AT_NTH: {
            int of_type = a->op & 2, from_end = a->op & 1;
            int aa = a->a, bb = a->b;
            if (!nth_match(aa, bb, nth_index(node, of_type != 0, from_end != 0))) return 0;
            break;
        }
        case AT_NOT:
            if (match_list_any(node, a->a)) return 0;
            break;
        case AT_IS:
            if (!match_list_any(node, a->a)) return 0;
            break;
        default:
            return 0;
        }
    }
}

static int compound_end(int ai) {
    while (g_atoms[ai].type != AT_END) ai++;
    return ai;
}

static int g_match_budget;

static int match_complex(int node, int ai) {
    if (--g_match_budget < 0) return 0;
    if (g_nodes[node].kind != 0 || g_nodes[node].tag == RB_TAG_DOC) return 0;
    if (!match_compound(node, ai)) return 0;
    int e = compound_end(ai);
    int comb = g_atoms[e].a;
    if (!comb) return 1;
    int next = e + 1;
    int p;
    switch (comb) {
    case '>':
        p = el_parent(node);
        return p && match_complex(p, next);
    case ' ':
        for (p = el_parent(node); p; p = el_parent(p))
            if (match_complex(p, next)) return 1;
        return 0;
    case '+':
        p = el_prev(node);
        return p && match_complex(p, next);
    case '~':
        for (p = el_prev(node); p; p = el_prev(p))
            if (match_complex(p, next)) return 1;
        return 0;
    }
    return 0;
}

/* ---- selector compiler -------------------------------------------------------- */

static int is_ident_c(int c) {
    return is_alpha_c(c) || is_digit_c(c) || c == '-' || c == '_' || c >= 0x80 || c == '\\';
}

/* Parse "an+b" (also odd/even/N). Returns 1 on success. */
static int parse_nth(const char *s, int n, int *A, int *B) {
    trim_tok(&s, &n);
    if (eq_lit(s, n, "odd")) { *A = 2; *B = 1; return 1; }
    if (eq_lit(s, n, "even")) { *A = 2; *B = 0; return 1; }
    int np = -1;
    for (int i = 0; i < n; i++) if (s[i] == 'n' || s[i] == 'N') { np = i; break; }
    if (np < 0) { *A = 0; return parse_int_tok(s, n, B); }
    const char *as = s;
    int an = np;
    trim_tok(&as, &an);
    if (an == 0 || (an == 1 && as[0] == '+')) *A = 1;
    else if (an == 1 && as[0] == '-') *A = -1;
    else if (!parse_int_tok(as, an, A)) return 0;
    const char *bs = s + np + 1;
    int bn = n - np - 1;
    trim_tok(&bs, &bn);
    if (bn == 0) { *B = 0; return 1; }
    char buf[16];
    int w = 0;
    for (int i = 0; i < bn && w < 15; i++) if (bs[i] != ' ') buf[w++] = bs[i];
    return parse_int_tok(buf, w, B);
}

static int compile_compound(const char *s, int n, int *pos, Atom *out, int *no, int cap, int *spec, int *pseudo_el, int depth);

/* ---- document vocabulary ---------------------------------------------------------
   When a stylesheet arrives after the page is parsed, a selector naming a class,
   id or tag the document does not contain can never match; it is dropped
   before it costs atoms, strings or rules (a framework's CSS is mostly such
   rules). Bitmaps keyed by a hash, so a false positive only keeps a rule. */
static uint32_t g_voc_cls[2048], g_voc_id[2048], g_voc_tag[8];
static int g_prune;

static void voc_set(uint32_t *map, const char *s, int n) {
    uint32_t h = ihash(s, n) & 0xFFFF;
    map[h >> 5] |= 1u << (h & 31);
}
static int voc_has(const uint32_t *map, const char *s, int n) {
    uint32_t h = ihash(s, n) & 0xFFFF;
    return (map[h >> 5] >> (h & 31)) & 1u;
}

static void vocab_build(void) {
    memset(g_voc_cls, 0, sizeof(g_voc_cls));
    memset(g_voc_id, 0, sizeof(g_voc_id));
    memset(g_voc_tag, 0, sizeof(g_voc_tag));
    for (int n = 2; n < g_node_n; n++) {
        Node *nd = &g_nodes[n];
        if (nd->dead || nd->kind != 0) continue;
        g_voc_tag[nd->tag >> 5] |= 1u << (nd->tag & 31);
        if (nd->id_len) voc_set(g_voc_id, g_pool + nd->id_off, (int)nd->id_len);
        if (nd->class_len) {
            const char *cl = g_pool + nd->class_off;
            int ln = (int)nd->class_len, i = 0;
            while (i < ln) {
                while (i < ln && (cl[i] == ' ' || cl[i] == '\t' || cl[i] == '\n')) i++;
                int st = i;
                while (i < ln && cl[i] != ' ' && cl[i] != '\t' && cl[i] != '\n') i++;
                if (i > st) voc_set(g_voc_cls, cl + st, i - st);
            }
        }
    }
}

/* Nested selector list for :not()/:is(): compounds only. Returns the first
   atom index, or -1. */
#define NEST_ALTS 8

/* Nested selector list for :not()/:is(): compounds only. Alternatives that can
   never match (a :hover inside) are dropped. Returns the first atom index of
   the list, -2 when every alternative was dropped, -1 on a syntax error. The
   compounds are compiled first (a nested :is() inside one writes its own list
   to g_atoms as it goes) and only then laid out contiguously. */
static int compile_nested_list(const char *s, int n, int *spec, int depth) {
    if (depth > 2) return -1;
    static const int ATOMS_PER = 22;
    Atom bufs[NEST_ALTS][24];
    int lens[NEST_ALTS];
    int na = 0, dropped = 0, i = 0, max_spec = 0;
    int mark = g_atom_n;
    while (i <= n) {
        int e = i, d = 0;
        while (e < n && !(s[e] == ',' && d == 0)) { if (s[e] == '(' || s[e] == '[') d++; else if (s[e] == ')' || s[e] == ']') d--; e++; }
        const char *cs = s + i;
        int cn = e - i;
        trim_tok(&cs, &cn);
        int tn = 0, pos = 0, sp = 0, pe = 0;
        if (na >= NEST_ALTS) { g_atom_n = mark; return -1; }
        if (cn <= 0 || !compile_compound(cs, cn, &pos, bufs[na], &tn, ATOMS_PER, &sp, &pe, depth + 1) || pos != cn || pe) {
            g_atom_n = mark;
            return -1;
        }
        int never = 0;
        for (int k = 0; k < tn; k++) if (bufs[na][k].type == AT_NEVER) { never = 1; break; }
        if (never) dropped++;
        else { lens[na] = tn; na++; if (sp > max_spec) max_spec = sp; }
        i = e + 1;
    }
    if (na == 0) { (void)dropped; return -2; }
    int total = 0;
    for (int k = 0; k < na; k++) total += lens[k] + 1;
    if (g_atom_n + total + 2 >= MAX_ATOMS) { g_atom_n = mark; SET_OOM(); return -1; }
    int start = g_atom_n;
    for (int k = 0; k < na; k++) {
        for (int q = 0; q < lens[k]; q++) g_atoms[g_atom_n++] = bufs[k][q];
        g_atoms[g_atom_n].type = AT_END; g_atoms[g_atom_n].op = 0; g_atoms[g_atom_n].b = 0; g_atoms[g_atom_n].a = 0;
        g_atom_n++;
    }
    g_atoms[g_atom_n].type = AT_STOP; g_atoms[g_atom_n].op = 0; g_atoms[g_atom_n].b = 0; g_atoms[g_atom_n].a = 0;
    g_atom_n++;
    *spec += max_spec;
    return start;
}

/* Decode CSS escapes in an identifier (".md\\:flex", ".w-1\\/2", "\\31 0") */
static int css_unescape(const char *s, int n, char *out, int cap) {
    int w = 0, i = 0;
    while (i < n && w < cap - 1) {
        if (s[i] == '\\' && i + 1 < n) {
            i++;
            int cp = 0, digits = 0;
            while (i < n && digits < 6 && hex_val(s[i]) >= 0) { cp = cp * 16 + hex_val(s[i]); i++; digits++; }
            if (digits) {
                if (i < n && s[i] == ' ') i++;
                out[w++] = (char)(cp > 0 && cp < 128 ? cp : '?');
            } else out[w++] = s[i++];
        } else out[w++] = s[i++];
    }
    out[w] = 0;
    return w;
}

/* One compound selector starting at *pos. Appends atoms to out[] (no END
   marker). *pseudo_el: 1/2 for ::before/::after, 3 for unsupported pseudo
   elements (drop). Returns 1 on success. */
static int compile_compound(const char *s, int n, int *pos, Atom *out, int *no, int cap, int *spec, int *pseudo_el, int depth) {
    int i = *pos;
    int any = 0;
    int w = *no;
#define PUSH(t, op_, a_, b_) do { if (w >= cap) return 0; out[w].type = (uint8_t)(t); out[w].op = (uint8_t)(op_); out[w].a = (a_); out[w].b = (int16_t)(b_); w++; } while (0)
    /* type selector or * */
    if (i < n && s[i] == '*') { i++; any = 1; PUSH(AT_UNIV, 0, 0, 0); }
    else if (i < n && (is_ident_c(s[i]) && s[i] != '\\')) {
        int st = i;
        while (i < n && is_ident_c(s[i])) i++;
        int tag = tag_of(s + st, i - st);
        any = 1;
        if (tag == RB_TAG_OTHER) PUSH(AT_TAG, 0, RB_TAG_OTHER, (int16_t)name_hash(s + st, i - st));
        else if (g_prune && depth == 0 && tag != RB_TAG_HTML && tag != RB_TAG_BODY && tag != RB_TAG_HEAD &&
                 !((g_voc_tag[tag >> 5] >> (tag & 31)) & 1u)) PUSH(AT_NEVER, 0, 0, 0);
        else PUSH(AT_TAG, 0, tag, 0);
        *spec += 1;
    }
    while (i < n) {
        char c = s[i];
        if (c == '.' || c == '#') {
            int st = ++i;
            while (i < n && is_ident_c(s[i])) {
                if (s[i] == '\\' && i + 1 < n) {     /* escape: skip the escaped char / hex run */
                    i++;
                    if (hex_val(s[i]) >= 0) { int dg = 0; while (i < n && dg < 6 && hex_val(s[i]) >= 0) { i++; dg++; } if (i < n && s[i] == ' ') i++; }
                    else i++;
                } else i++;
            }
            if (i == st) return 0;
            char nm[64];
            int nlen = css_unescape(s + st, i - st, nm, (int)sizeof(nm));
            const char *nms = nm;
            /* a CSS escape (\: in tailwind-style names) is kept literally */
            if (g_prune && depth == 0 && !voc_has(c == '.' ? g_voc_cls : g_voc_id, nms, nlen)) {
                PUSH(AT_NEVER, 0, 0, 0);
                *spec += c == '.' ? 100 : 10000;
                any = 1;
                continue;
            }
            int id = intern(nms, nlen, 1);
            if (id < 0) return 0;
            PUSH(c == '.' ? AT_CLASS : AT_ID, 0, id, 0);
            *spec += c == '.' ? 100 : 10000;
            any = 1;
        } else if (c == '[') {
            int e = i + 1, d = 1;
            char q = 0;
            while (e < n && d > 0) {
                if (q) { if (s[e] == q) q = 0; }
                else if (s[e] == '"' || s[e] == '\'') q = s[e];
                else if (s[e] == '[') d++;
                else if (s[e] == ']') d--;
                e++;
            }
            if (d != 0) return 0;
            const char *as = s + i + 1;
            int an = e - i - 2;
            trim_tok(&as, &an);
            int k = 0;
            while (k < an && is_ident_c(as[k])) k++;
            if (k == 0) return 0;
            char lname[48];
            int ln = k < 47 ? k : 47;
            for (int j = 0; j < ln; j++) lname[j] = (char)lower_ch((unsigned char)as[j]);
            int nid = intern(lname, ln, 1);
            if (nid < 0) return 0;
            int op = AO_EXISTS, vid = -1;
            int j = k;
            while (j < an && as[j] == ' ') j++;
            if (j < an) {
                if (as[j] == '=') { op = AO_EQ; j++; }
                else if (j + 1 < an && as[j + 1] == '=') {
                    char o = as[j];
                    op = o == '~' ? AO_WORD : o == '|' ? AO_DASH : o == '^' ? AO_PREFIX : o == '$' ? AO_SUFFIX : o == '*' ? AO_CONTAINS : -1;
                    if (op < 0) return 0;
                    j += 2;
                } else return 0;
                while (j < an && as[j] == ' ') j++;
                const char *vs = as + j;
                int vn = an - j;
                int ic = 0;
                if (vn > 0 && (vs[vn - 1] == 'i' || vs[vn - 1] == 'I') && vn >= 3 && vs[vn - 2] == ' ') { ic = 1; vn -= 2; }
                while (vn > 0 && vs[vn - 1] == ' ') vn--;
                if (vn >= 2 && (vs[0] == '"' || vs[0] == '\'') && vs[vn - 1] == vs[0]) { vs++; vn -= 2; }
                vid = intern(vs, vn, 1);
                if (vid < 0) return 0;
                if (ic) op |= AO_ICASE;
            }
            PUSH(AT_ATTR, op, nid, vid);
            *spec += 100;
            i = e;
            any = 1;
        } else if (c == ':') {
            int dbl = i + 1 < n && s[i + 1] == ':';
            int st = i + (dbl ? 2 : 1);
            int e = st;
            while (e < n && is_ident_c(s[e])) e++;
            int nl = e - st;
            const char *nm = s + st;
            const char *arg = 0;
            int argn = 0;
            if (e < n && s[e] == '(') {
                int d = 1, k = e + 1;
                while (k < n && d > 0) { if (s[k] == '(') d++; else if (s[k] == ')') d--; k++; }
                if (d != 0) return 0;
                arg = s + e + 1;
                argn = k - e - 2;
                e = k;
            }
            i = e;
            any = 1;
            if (dbl || eq_lit(nm, nl, "before") || eq_lit(nm, nl, "after")) {
                if (eq_lit(nm, nl, "before")) *pseudo_el = 1;
                else if (eq_lit(nm, nl, "after")) *pseudo_el = 2;
                else *pseudo_el = 3;
                continue;
            }
            *spec += 100;
            if (eq_lit(nm, nl, "root")) PUSH(AT_PSEUDO, 0, 0, PS_ROOT);
            else if (eq_lit(nm, nl, "empty")) PUSH(AT_PSEUDO, 0, 0, PS_EMPTY);
            else if (eq_lit(nm, nl, "link") || eq_lit(nm, nl, "any-link")) PUSH(AT_PSEUDO, 0, 0, PS_LINK);
            else if (eq_lit(nm, nl, "checked")) PUSH(AT_PSEUDO, 0, 0, PS_CHECKED);
            else if (eq_lit(nm, nl, "disabled")) PUSH(AT_PSEUDO, 0, 0, PS_DISABLED);
            else if (eq_lit(nm, nl, "enabled")) PUSH(AT_PSEUDO, 0, 0, PS_ENABLED);
            else if (eq_lit(nm, nl, "required")) PUSH(AT_PSEUDO, 0, 0, PS_REQUIRED);
            else if (eq_lit(nm, nl, "optional")) PUSH(AT_PSEUDO, 0, 0, PS_OPTIONAL);
            else if (eq_lit(nm, nl, "-rb-btn")) PUSH(AT_PSEUDO, 0, 0, PS_BTN);
            else if (eq_lit(nm, nl, "first-child")) PUSH(AT_PSEUDO, 0, 0, PS_FIRSTC);
            else if (eq_lit(nm, nl, "last-child")) PUSH(AT_PSEUDO, 0, 0, PS_LASTC);
            else if (eq_lit(nm, nl, "only-child")) PUSH(AT_PSEUDO, 0, 0, PS_ONLYC);
            else if (eq_lit(nm, nl, "first-of-type")) PUSH(AT_PSEUDO, 0, 0, PS_FIRSTT);
            else if (eq_lit(nm, nl, "last-of-type")) PUSH(AT_PSEUDO, 0, 0, PS_LASTT);
            else if (eq_lit(nm, nl, "only-of-type")) PUSH(AT_PSEUDO, 0, 0, PS_ONLYT);
            else if (eq_lit(nm, nl, "nth-child") || eq_lit(nm, nl, "nth-last-child") ||
                     eq_lit(nm, nl, "nth-of-type") || eq_lit(nm, nl, "nth-last-of-type")) {
                int A, B;
                if (!arg) return 0;
                /* "2n+1 of S" is not supported: drop the "of" part */
                int an2 = argn;
                for (int k = 0; k + 3 < argn; k++) if (arg[k] == ' ' && arg[k + 1] == 'o' && arg[k + 2] == 'f' && arg[k + 3] == ' ') { an2 = k; break; }
                if (!parse_nth(arg, an2, &A, &B)) return 0;
                int op = (nl >= 8 && eq_lit(nm, 8, "nth-last") ? 1 : 0) | (ends_lit(nm, nl, "of-type") ? 2 : 0);
                PUSH(AT_NTH, op, A, B);
            } else if (eq_lit(nm, nl, "not") || eq_lit(nm, nl, "is") || eq_lit(nm, nl, "where") ||
                       eq_lit(nm, nl, "matches") || eq_lit(nm, nl, "-webkit-any") || eq_lit(nm, nl, "-moz-any")) {
                if (!arg) return 0;
                int sp = 0;
                int start = compile_nested_list(arg, argn, &sp, depth);
                if (start == -1) return 0;
                if (start == -2) {
                    /* every alternative can never match: :not() of nothing is
                       a no-op, :is() of nothing matches nothing */
                    *spec += -100;
                    if (!eq_lit(nm, nl, "not")) PUSH(AT_NEVER, 0, 0, 0);
                    continue;
                }
                if (eq_lit(nm, nl, "where")) sp = 0;
                *spec += sp - 100;       /* the pseudo itself adds nothing */
                PUSH(eq_lit(nm, nl, "not") ? AT_NOT : AT_IS, 0, start, 0);
            } else {
                /* :hover :focus :active :visited :target ... never match */
                PUSH(AT_NEVER, 0, 0, 0);
            }
        } else break;
    }
#undef PUSH
    *no = w;
    *pos = i;
    return any;
}

/* Compile one complex selector into g_atoms (rightmost compound first).
   Returns the first atom index of the rightmost compound, or -1. */
static int g_sel_never;     /* set when compile_selector refused a well-formed selector that can never match */
static int compile_selector(const char *s, int n, int *spec_out, int *pseudo_out) {
    Atom tmp[96];
    g_sel_never = 0;
    int tn = 0;
    int cstart[8], ccomb[8];       /* compound start offsets in tmp; combinator AFTER it */
    int cn = 0;
    int i = 0;
    int spec = 0, pseudo = 0;
    trim_tok(&s, &n);
    if (n <= 0) return -1;
    for (;;) {
        if (cn >= 8) return -1;
        cstart[cn] = tn;
        int pe = 0;
        if (!compile_compound(s, n, &i, tmp, &tn, 90, &spec, &pe, 0)) return -1;
        if (pe) pseudo = pe;
        ccomb[cn] = 0;
        cn++;
        /* combinator */
        int j = i;
        int sp = 0;
        while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) { j++; sp = 1; }
        if (j >= n) { i = j; break; }
        int comb = 0;
        if (s[j] == '>' || s[j] == '+' || s[j] == '~') {
            comb = s[j++];
            while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) j++;
        } else if (sp) comb = ' ';
        else return -1;
        if (j >= n) return -1;
        ccomb[cn - 1] = comb;
        i = j;
    }
    if (i < n) return -1;
    if (pseudo == 3) { g_sel_never = 1; return -1; }
    for (int k = 0; k < tn; k++) if (tmp[k].type == AT_NEVER) { g_sel_never = 1; return -1; }      /* can never match */
    /* copy reversed */
    int need = tn + cn + 1;
    if (g_atom_n + need >= MAX_ATOMS) { SET_OOM(); return -1; }
    int first = g_atom_n;
    for (int c = cn - 1; c >= 0; c--) {
        int a0 = cstart[c];
        int a1 = c + 1 < cn ? cstart[c + 1] : tn;
        for (int k = a0; k < a1; k++) g_atoms[g_atom_n++] = tmp[k];
        g_atoms[g_atom_n].type = AT_END;
        g_atoms[g_atom_n].op = 0;
        g_atoms[g_atom_n].b = 0;
        g_atoms[g_atom_n].a = c > 0 ? ccomb[c - 1] : 0;
        g_atom_n++;
    }
    *spec_out = spec;
    *pseudo_out = pseudo;
    return first;
}

/* ---- media queries --------------------------------------------------------------- */

/* The media viewport: the screen width (content + the 16px scrollbar) and the
   content height in the profile's CSS px (the web profile halves vertical
   lengths, so a screen row is two CSS px). Set by rb_set_viewport. */
static int g_media_w = RB_VIEW_W_DEFAULT + 16;
static int g_media_h = RB_VIEW_H_DEFAULT * 2;
#define MEDIA_W g_media_w
#define MEDIA_H g_media_h

/* A media-feature length in px: a plain dimension or a calc()/min()/max()/clamp(). */
static int media_len(const char *v, int vl, int *px) {
    Dim d;
    int c = parse_dim(v, vl, &d);
    if (c && c == vl) { *px = dim_css_mil(&d) / 1000; return 1; }
    int pct, hp;
    if (parse_length(v, vl, px, &pct, &hp) && !hp) return 1;
    return 0;
}

static int media_feature(const char *f, int fn) {
    const char *colon = 0;
    for (int i = 0; i < fn; i++) if (f[i] == ':') { colon = f + i; break; }
    const char *name = f;
    int nl = colon ? (int)(colon - f) : fn;
    const char *val = colon ? colon + 1 : "";
    int vl = colon ? fn - nl - 1 : 0;
    trim_tok(&name, &nl);
    trim_tok(&val, &vl);
    if (!colon) {
        return eq_lit(name, nl, "color") || eq_lit(name, nl, "hover") || eq_lit(name, nl, "any-hover") ||
               eq_lit(name, nl, "pointer") || eq_lit(name, nl, "any-pointer");
    }
    int is_min = starts_lit(name, nl, "min-"), is_max = starts_lit(name, nl, "max-");
    const char *base = name + ((is_min || is_max) ? 4 : 0);
    int bl = nl - ((is_min || is_max) ? 4 : 0);
    if (eq_lit(base, bl, "width") || eq_lit(base, bl, "height") || eq_lit(base, bl, "device-width") ||
        eq_lit(base, bl, "device-height")) {
        int is_w = eq_lit(base, bl, "width") || eq_lit(base, bl, "device-width");
        int px;
        if (!media_len(val, vl, &px)) return 0;
        int cur = is_w ? MEDIA_W : MEDIA_H;
        if (is_min) return cur >= px;
        if (is_max) return cur <= px;
        return cur == px;
    }
    if (eq_lit(base, bl, "orientation")) return eq_lit(val, vl, "landscape") ? MEDIA_W >= MEDIA_H : MEDIA_W < MEDIA_H;
    if (eq_lit(base, bl, "prefers-color-scheme")) return eq_lit(val, vl, "light");
    if (eq_lit(base, bl, "prefers-reduced-motion")) return eq_lit(val, vl, "reduce");
    if (eq_lit(base, bl, "prefers-contrast")) return eq_lit(val, vl, "no-preference");
    if (eq_lit(base, bl, "hover") || eq_lit(base, bl, "any-hover")) return eq_lit(val, vl, "hover");
    if (eq_lit(base, bl, "pointer") || eq_lit(base, bl, "any-pointer")) return eq_lit(val, vl, "fine");
    if (eq_lit(base, bl, "color")) return 1;
    if (eq_lit(base, bl, "resolution") || eq_lit(base, bl, "device-pixel-ratio") ||
        eq_lit(base, bl, "-webkit-device-pixel-ratio")) {
        Dim d;
        int c = parse_dim(val, vl, &d);
        if (!c) return 0;
        return is_min ? d.mil <= 1000 : (is_max ? d.mil >= 1000 : d.mil == 1000);
    }
    if (eq_lit(base, bl, "display-mode")) return eq_lit(val, vl, "browser");
    if (eq_lit(base, bl, "scripting")) return eq_lit(val, vl, "none");
    return 0;
}

/* One query (no top-level commas): [not|only] type [and (f)]... */
static int media_one(const char *q, int n) {
    trim_tok(&q, &n);
    if (n <= 0) return 1;
    int neg = 0, i = 0, tn;
    const char *t;
    int result = 1;
    int first = 1;
    while (next_tok(q, n, &i, &t, &tn)) {
        if (eq_lit(t, tn, "and")) continue;
        if (first && eq_lit(t, tn, "not")) { neg = 1; continue; }
        if (first && eq_lit(t, tn, "only")) continue;
        first = 0;
        if (t[0] == '(') {
            const char *f = t + 1;
            int fn = tn - 2;
            if (fn < 0) { result = 0; continue; }
            /* (a) and (b) inside parens, or a range like (width >= 400px) */
            int ge = -1;
            for (int k = 0; k + 1 < fn; k++) if ((f[k] == '>' || f[k] == '<')) { ge = k; break; }
            if (ge >= 0) {
                /* (width >= 600px): support  <name> <op> <len>  and <len> <op> <name> loosely */
                int ok = 0;
                const char *nm = f; int nl = ge;
                trim_tok(&nm, &nl);
                int eqs = ge + 1 < fn && f[ge + 1] == '=';
                const char *vv = f + ge + 1 + eqs; int vl = fn - ge - 1 - eqs;
                trim_tok(&vv, &vl);
                int px;
                if (media_len(vv, vl, &px) && (eq_lit(nm, nl, "width") || eq_lit(nm, nl, "height"))) {
                    int cur = eq_lit(nm, nl, "width") ? MEDIA_W : MEDIA_H;
                    if (f[ge] == '>') ok = eqs ? cur >= px : cur > px;
                    else ok = eqs ? cur <= px : cur < px;
                }
                if (!ok) result = 0;
            } else if (!media_feature(f, fn)) result = 0;
        } else if (eq_lit(t, tn, "screen") || eq_lit(t, tn, "all")) {
        } else {
            result = 0;      /* print, speech, tty, ... */
        }
    }
    return neg ? !result : result;
}

static int media_matches(const char *q, int n) {
    int i = 0, any = 0;
    trim_tok(&q, &n);
    if (n <= 0) return 1;
    while (i <= n) {
        int e = i, d = 0;
        while (e < n && !(q[e] == ',' && d == 0)) { if (q[e] == '(') d++; else if (q[e] == ')') d--; e++; }
        if (media_one(q + i, e - i)) any = 1;
        i = e + 1;
    }
    return any;
}

/* ---- stylesheet parser ------------------------------------------------------------ */

#define SHEET_STEP 16        /* ordinal spacing: room for a sheet's @imports before it */
#define MAX_IMPORTS 24
typedef struct { uint32_t off; uint16_t len; uint8_t sheet; } ImportEnt;
static ImportEnt g_imports[MAX_IMPORTS];     /* @import urls, as intern ids in off */
static int g_import_n;

static int g_sheet_ord;       /* ordinal of the sheet being parsed */
static uint32_t g_sheet_idx;  /* rule index within the sheet */
static int g_cur_origin;      /* 0 UA, 1 author */
static int g_in_layer;        /* inside @layer: loses to unlayered author rules */

static int skip_block(const char *s, int n, int i) {
    /* s[i] is just after '{'; returns the index after the matching '}' */
    int d = 1;
    char q = 0;
    while (i < n && d > 0) {
        char c = s[i];
        if (q) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == q) q = 0; }
        else if (c == '"' || c == '\'') q = c;
        else if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i++;
        } else if (c == '{') d++;
        else if (c == '}') d--;
        i++;
    }
    return i > n ? n : i;
}

/* Custom properties are kept globally (there is no per-element variable
   map), so only a rule whose selector matches the root or body element may
   define them. */
static int find_root_el(int tag) {
    for (int c = g_nodes[1].first; c; c = g_nodes[c].next) {
        if (g_nodes[c].kind != 0 || g_nodes[c].dead) continue;
        if (g_nodes[c].tag == RB_TAG_HTML) {
            if (tag == RB_TAG_HTML) return c;
            for (int d = g_nodes[c].first; d; d = g_nodes[d].next)
                if (g_nodes[d].kind == 0 && !g_nodes[d].dead && g_nodes[d].tag == tag) return d;
            return 0;
        }
        if (g_nodes[c].tag == tag) return c;
    }
    return 0;
}

#define MAX_ALTS 200
typedef struct { int first, spec, pseudo; } AltSel;

static void add_rule_set(const char *sel, int sn, const char *body, int bn) {
    if (g_crule_n >= MAX_CRULES) { SET_OOM(); return; }
    AltSel alts[MAX_ALTS];
    int na = 0;
    int atom_mark0 = g_atom_n;
    int i = 0;
    while (i <= sn && na < MAX_ALTS) {
        int e = i, d = 0;
        char q = 0;
        while (e < sn && !(sel[e] == ',' && d == 0 && !q)) {
            char c = sel[e];
            if (q) { if (c == q) q = 0; }
            else if (c == '"' || c == '\'') q = c;
            else if (c == '(' || c == '[') d++;
            else if (c == ')' || c == ']') d--;
            e++;
        }
        int spec = 0, pseudo = 0;
        int atom_mark = g_atom_n;
        int first = compile_selector(sel + i, e - i, &spec, &pseudo);
        if (first >= 0) { alts[na].first = first; alts[na].spec = spec; alts[na].pseudo = pseudo; na++; }
        else g_atom_n = atom_mark;
        i = e + 1;
    }
    if (!na) { g_atom_n = atom_mark0; return; }
    /* does any alternative select the root or body element? then its --vars count */
    int capture = 0;
    if (g_node_n > 1) {
        capture = 0;
        int html_el = find_root_el(RB_TAG_HTML), body_el = find_root_el(RB_TAG_BODY);
        for (int k = 0; k < na && !capture; k++) {
            if (alts[k].pseudo) continue;
            g_match_budget = 400;
            if (html_el && match_complex(html_el, alts[k].first)) capture = 1;
            else if (body_el) { g_match_budget = 400; if (match_complex(body_el, alts[k].first)) capture = 1; }
        }
        /* a fragment with no <html> or <body>: there is no root to ask, so go by
           what the selector says */
        if (!html_el && !body_el) {
            for (int k = 0, i2 = 0; i2 <= sn && !capture && k < MAX_ALTS; k++) {
                int e2 = i2, d2 = 0;
                while (e2 < sn && !(sel[e2] == ',' && d2 == 0)) { if (sel[e2] == '(' || sel[e2] == '[') d2++; else if (sel[e2] == ')' || sel[e2] == ']') d2--; e2++; }
                const char *rs = sel + i2; int rn = e2 - i2;
                trim_tok(&rs, &rn);
                if (eq_lit(rs, rn, ":root") || eq_lit(rs, rn, "html") || eq_lit(rs, rn, "body") || eq_lit(rs, rn, "*")) capture = 1;
                i2 = e2 + 1;
            }
        }
    } else capture = 1;
    /* A --var set by any other rule is scoped to the elements it matches. Real
       scoping needs per-element custom properties; as an approximation the
       first such definition of a name is kept page-wide (<html>/<body> rules
       always win), which beats dropping every var() that depends on it. */
    if (!capture) for (int k = 0; k < na; k++) if (!alts[k].pseudo) { capture = 2; break; }
    int dstart = g_decl_n;
    g_var_capture = capture;
    g_dout = g_decl; g_dn = &g_decl_n; g_dcap = MAX_DECL;
    parse_decl_list(body, bn);
    g_var_capture = 0;
    int dn = g_decl_n - dstart;
    if (dn <= 0) { g_atom_n = atom_mark0; g_decl_n = dstart; return; }
    for (int k = 0; k < na && g_crule_n < MAX_CRULES; k++) {
        CRule *r = &g_crule[g_crule_n++];
        r->order = ((uint32_t)(g_sheet_ord > 4000 ? 4000 : g_sheet_ord) << 20) | (g_sheet_idx++ & 0xFFFFF);
        r->spec = (uint32_t)alts[k].spec;
        r->sel = (uint32_t)alts[k].first;
        r->decl = (uint32_t)dstart;
        r->ndecl = (uint16_t)(dn > 65535 ? 65535 : dn);
        r->next = 0;
        r->pseudo = (uint8_t)alts[k].pseudo;
        r->origin = (uint8_t)(g_cur_origin == 1 && g_in_layer ? 2 : g_cur_origin);
    }
}

static int atrule_name(const char *s, int n, int i, int *nl) {
    int st = i + 1;
    int e = st;
    while (e < n && (is_alpha_c(s[e]) || s[e] == '-')) e++;
    *nl = e - st;
    return st;
}

static void parse_sheet(const char *s, int n, int depth) {
    int i = 0;
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
        if (i >= n) break;
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i = i + 2 <= n ? i + 2 : n;
            continue;
        }
        if (i + 3 < n && s[i] == '<' && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') { i += 4; continue; }
        if (i + 2 < n && s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>') { i += 3; continue; }
        if (s[i] == '}') { i++; continue; }
        if (s[i] == '@') {
            int nl;
            int ns = atrule_name(s, n, i, &nl);
            const char *nm = s + ns;
            int p = ns + nl;
            /* prelude up to '{' or ';' */
            int ps = p;
            int d = 0;
            char q = 0;
            while (p < n) {
                char c = s[p];
                if (q) { if (c == '\\' && p + 1 < n) { p += 2; continue; } if (c == q) q = 0; }
                else if (c == '"' || c == '\'') q = c;
                else if (c == '(') d++;
                else if (c == ')') d--;
                else if ((c == '{' || c == ';') && d <= 0) break;
                p++;
            }
            const char *pre = s + ps;
            int pn = p - ps;
            if (p >= n) { i = n; break; }
            if (s[p] == ';') {
                if (eq_lit(nm, nl, "import") && depth < 3 && g_import_n < MAX_IMPORTS) {
                    /* @import url("x") screen; | @import "x"; */
                    const char *u = pre;
                    int un = pn;
                    trim_tok(&u, &un);
                    int mediaq_at = -1;
                    const char *ub = u;
                    int ul = un;
                    if (starts_lit(u, un, "url(")) {
                        int e = 4;
                        while (e < un && u[e] != ')') e++;
                        ub = u + 4; ul = e - 4;
                        mediaq_at = e + 1;
                    } else if (un > 0 && (u[0] == '"' || u[0] == '\'')) {
                        int e = 1;
                        while (e < un && u[e] != u[0]) e++;
                        ub = u + 1; ul = e - 1;
                        mediaq_at = e + 1;
                    } else ul = 0;
                    trim_tok(&ub, &ul);
                    if (ul >= 2 && (ub[0] == '"' || ub[0] == '\'') && ub[ul - 1] == ub[0]) { ub++; ul -= 2; }
                    int okm = 1;
                    if (mediaq_at >= 0 && mediaq_at < un) okm = media_matches(u + mediaq_at, un - mediaq_at);
                    if (ul > 0 && okm) {
                        int id = intern(ub, ul, 1);
                        if (id >= 0) {
                            g_imports[g_import_n].off = (uint32_t)id;
                            g_imports[g_import_n].len = (uint16_t)ul;
                            g_imports[g_import_n].sheet = (uint8_t)g_sheet_ord;
                            g_import_n++;
                        }
                    }
                }
                i = p + 1;
                continue;
            }
            int bs = p + 1;
            int be = skip_block(s, n, bs);
            int inner_end = be > bs && s[be - 1] == '}' ? be - 1 : be;
            if (eq_lit(nm, nl, "media")) {
                if (depth < 4 && media_matches(pre, pn)) parse_sheet(s + bs, inner_end - bs, depth + 1);
            } else if (eq_lit(nm, nl, "supports")) {
                int neg = 0;
                for (int k = 0; k + 3 < pn; k++) if (starts_lit(pre + k, pn - k, "not ") && (k == 0 || pre[k - 1] == ' ' || pre[k - 1] == '(')) { neg = 1; break; }
                if (!neg && depth < 4) parse_sheet(s + bs, inner_end - bs, depth + 1);
            } else if (eq_lit(nm, nl, "layer") || eq_lit(nm, nl, "scope") || eq_lit(nm, nl, "document") ||
                       eq_lit(nm, nl, "-moz-document")) {
                if (depth < 4) {
                    int save_layer = g_in_layer;
                    if (eq_lit(nm, nl, "layer")) g_in_layer = 1;
                    parse_sheet(s + bs, inner_end - bs, depth + 1);
                    g_in_layer = save_layer;
                }
            }
            /* @font-face, @keyframes, @page, @container, ... are skipped */
            i = be;
            continue;
        }
        /* qualified rule */
        int ss = i;
        int d = 0;
        char q = 0;
        while (i < n) {
            char c = s[i];
            if (q) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == q) q = 0; }
            else if (c == '"' || c == '\'') q = c;
            else if (c == '(' || c == '[') d++;
            else if (c == ')' || c == ']') d--;
            else if (c == '{' && d <= 0) break;
            else if (c == ';' && d <= 0) break;
            i++;
        }
        if (i >= n) break;
        if (s[i] == ';') { i++; continue; }
        int se = i;
        int bs = i + 1;
        int be = skip_block(s, n, bs);
        int inner_end = be > bs && s[be - 1] == '}' ? be - 1 : be;
        add_rule_set(s + ss, se - ss, s + bs, inner_end - bs);
        i = be;
    }
}

/* ---- rule index -------------------------------------------------------------------- */

static void rules_index(void) {
    memset(g_rh_tag, 0, sizeof(g_rh_tag));
    memset(g_rh_id, 0, sizeof(g_rh_id));
    memset(g_rh_cls, 0, sizeof(g_rh_cls));
    g_rh_any = 0;
    for (int r = g_crule_n - 1; r >= 0; r--) {
        CRule *cr = &g_crule[r];
        int ai = (int)cr->sel;
        int tag = -1, id = -1, cls = -1;
        for (;; ai++) {
            const Atom *a = &g_atoms[ai];
            if (a->type == AT_END) break;
            if (a->type == AT_ID) id = a->a;
            else if (a->type == AT_CLASS && cls < 0) cls = a->a;
            else if (a->type == AT_TAG) tag = a->a;
        }
        uint16_t *head;
        if (id >= 0) head = &g_rh_id[id];
        else if (cls >= 0) head = &g_rh_cls[cls];
        else if (tag >= 0) head = &g_rh_tag[tag];
        else head = &g_rh_any;
        cr->next = *head;
        *head = (uint16_t)(r + 1);
    }
}
