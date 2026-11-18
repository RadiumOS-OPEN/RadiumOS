/* rbrowse CSS values: colours, lengths with units, calc(), var().
   Included by engine.c (one translation unit, freestanding: integer math only,
   no libc beyond memcpy/memcmp/memset/strlen). */

/* ---- profile ------------------------------------------------------------
   The native profile is the engine's original look (CSS px == screen px,
   amber-on-brown). The web profile is for real pages: white paper, CSS px are
   halved (the 8x8 glyph stands for a 16px font) and the viewport is
   RB_VIEW_W CSS px wide and 2 * RB_VIEW_H tall. */
static int g_web;                 /* 1: web profile */
static int g_fs_thresh = 20;      /* CSS px at which text goes to scale 2 */

/* css px -> screen px. The 8x8 font advances 8px per character, which is what
   a 16px proportional font averages, so horizontal lengths map 1:1; the glyph
   is half as tall as that font's line, so vertical lengths are halved. g_axis
   says which one the property being parsed is (0 horizontal, 1 vertical). */
static int g_axis;
static int zy(int v) {
    if (!g_web) return v;
    return v >= 0 ? (v + 1) / 2 : -((-v + 1) / 2);
}
static int px_zoom(int v) { return g_axis ? zy(v) : v; }

/* Hash of a tag name (case-insensitive): tells apart the elements the engine
   has no tag id for, so custom-element selectors can match them. */
static uint16_t name_hash(const char *s, int n) {
    uint32_t h = 5381;
    for (int i = 0; i < n; i++) h = h * 33 + (unsigned)lower_ch((unsigned char)s[i]);
    return (uint16_t)(h ^ (h >> 16));
}

/* ---- colours ------------------------------------------------------------ */

static int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

typedef struct { const char *name; uint32_t rgb; } NamedColor;
static const NamedColor g_named[] = {
    {"aliceblue",0xF0F8FF},{"antiquewhite",0xFAEBD7},{"aqua",0x00FFFF},{"aquamarine",0x7FFFD4},
    {"azure",0xF0FFFF},{"beige",0xF5F5DC},{"bisque",0xFFE4C4},{"black",0x000000},
    {"blanchedalmond",0xFFEBCD},{"blue",0x0000FF},{"blueviolet",0x8A2BE2},{"brown",0xA52A2A},
    {"burlywood",0xDEB887},{"cadetblue",0x5F9EA0},{"chartreuse",0x7FFF00},{"chocolate",0xD2691E},
    {"coral",0xFF7F50},{"cornflowerblue",0x6495ED},{"cornsilk",0xFFF8DC},{"crimson",0xDC143C},
    {"cyan",0x00FFFF},{"darkblue",0x00008B},{"darkcyan",0x008B8B},{"darkgoldenrod",0xB8860B},
    {"darkgray",0xA9A9A9},{"darkgreen",0x006400},{"darkgrey",0xA9A9A9},{"darkkhaki",0xBDB76B},
    {"darkmagenta",0x8B008B},{"darkolivegreen",0x556B2F},{"darkorange",0xFF8C00},{"darkorchid",0x9932CC},
    {"darkred",0x8B0000},{"darksalmon",0xE9967A},{"darkseagreen",0x8FBC8F},{"darkslateblue",0x483D8B},
    {"darkslategray",0x2F4F4F},{"darkslategrey",0x2F4F4F},{"darkturquoise",0x00CED1},{"darkviolet",0x9400D3},
    {"deeppink",0xFF1493},{"deepskyblue",0x00BFFF},{"dimgray",0x696969},{"dimgrey",0x696969},
    {"dodgerblue",0x1E90FF},{"firebrick",0xB22222},{"floralwhite",0xFFFAF0},{"forestgreen",0x228B22},
    {"fuchsia",0xFF00FF},{"gainsboro",0xDCDCDC},{"ghostwhite",0xF8F8FF},{"gold",0xFFD700},
    {"goldenrod",0xDAA520},{"gray",0x808080},{"green",0x008000},{"greenyellow",0xADFF2F},
    {"grey",0x808080},{"honeydew",0xF0FFF0},{"hotpink",0xFF69B4},{"indianred",0xCD5C5C},
    {"indigo",0x4B0082},{"ivory",0xFFFFF0},{"khaki",0xF0E68C},{"lavender",0xE6E6FA},
    {"lavenderblush",0xFFF0F5},{"lawngreen",0x7CFC00},{"lemonchiffon",0xFFFACD},{"lightblue",0xADD8E6},
    {"lightcoral",0xF08080},{"lightcyan",0xE0FFFF},{"lightgoldenrodyellow",0xFAFAD2},{"lightgray",0xD3D3D3},
    {"lightgreen",0x90EE90},{"lightgrey",0xD3D3D3},{"lightpink",0xFFB6C1},{"lightsalmon",0xFFA07A},
    {"lightseagreen",0x20B2AA},{"lightskyblue",0x87CEFA},{"lightslategray",0x778899},{"lightslategrey",0x778899},
    {"lightsteelblue",0xB0C4DE},{"lightyellow",0xFFFFE0},{"lime",0x00FF00},{"limegreen",0x32CD32},
    {"linen",0xFAF0E6},{"magenta",0xFF00FF},{"maroon",0x800000},{"mediumaquamarine",0x66CDAA},
    {"mediumblue",0x0000CD},{"mediumorchid",0xBA55D3},{"mediumpurple",0x9370DB},{"mediumseagreen",0x3CB371},
    {"mediumslateblue",0x7B68EE},{"mediumspringgreen",0x00FA9A},{"mediumturquoise",0x48D1CC},
    {"mediumvioletred",0xC71585},{"midnightblue",0x191970},{"mintcream",0xF5FFFA},{"mistyrose",0xFFE4E1},
    {"moccasin",0xFFE4B5},{"navajowhite",0xFFDEAD},{"navy",0x000080},{"oldlace",0xFDF5E6},
    {"olive",0x808000},{"olivedrab",0x6B8E23},{"orange",0xFFA500},{"orangered",0xFF4500},
    {"orchid",0xDA70D6},{"palegoldenrod",0xEEE8AA},{"palegreen",0x98FB98},{"paleturquoise",0xAFEEEE},
    {"palevioletred",0xDB7093},{"papayawhip",0xFFEFD5},{"peachpuff",0xFFDAB9},{"peru",0xCD853F},
    {"pink",0xFFC0CB},{"plum",0xDDA0DD},{"powderblue",0xB0E0E6},{"purple",0x800080},
    {"rebeccapurple",0x663399},{"red",0xFF0000},{"rosybrown",0xBC8F8F},{"royalblue",0x4169E1},
    {"saddlebrown",0x8B4513},{"salmon",0xFA8072},{"sandybrown",0xF4A460},{"seagreen",0x2E8B57},
    {"seashell",0xFFF5EE},{"sienna",0xA0522D},{"silver",0xC0C0C0},{"skyblue",0x87CEEB},
    {"slateblue",0x6A5ACD},{"slategray",0x708090},{"slategrey",0x708090},{"snow",0xFFFAFA},
    {"springgreen",0x00FF7F},{"steelblue",0x4682B4},{"tan",0xD2B48C},{"teal",0x008080},
    {"thistle",0xD8BFD8},{"tomato",0xFF6347},{"turquoise",0x40E0D0},{"violet",0xEE82EE},
    {"wheat",0xF5DEB3},{"white",0xFFFFFF},{"whitesmoke",0xF5F5F5},{"yellow",0xFFFF00},
    {"yellowgreen",0x9ACD32},
};

static int named_color(const char *s, int n, int *r, int *g, int *b) {
    int lo = 0, hi = (int)(sizeof(g_named) / sizeof(g_named[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *nm = g_named[mid].name;
        int c = 0;
        for (int i = 0;; i++) {
            int a = i < n ? lower_ch((unsigned char)s[i]) : 0;
            int bch = (unsigned char)nm[i];
            if (a != bch) { c = a < bch ? -1 : 1; break; }
            if (!bch) break;
        }
        if (c == 0) {
            uint32_t v = g_named[mid].rgb;
            *r = (int)((v >> 16) & 255); *g = (int)((v >> 8) & 255); *b = (int)(v & 255);
            return 1;
        }
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return 0;
}

/* Colours are exact 24-bit RGB (0xRRGGBB) in both profiles. */
int rb_map_rgb(int r, int g, int b) {
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (int)RB_RGB(r, g, b);
}

/* Nearest palette entry for an 8-bit display: the 6x6x6 cube (16..231) or
   the gray ramp (232..255, 0..230 in steps of 10). Near-grays pick whichever
   is closer, so pale backgrounds like #f6f6f6 stay distinct from white. */
int rb_rgb_index(RbColor c) {
    int r = RB_R(c), g = RB_G(c), b = RB_B(c);
    int ri = (r * 5 + 127) / 255;
    int gi = (g * 5 + 127) / 255;
    int bi = (b * 5 + 127) / 255;
    int best = 16 + ri * 36 + gi * 6 + bi;
    int cr = ri * 51 - r, cg = gi * 51 - g, cb = bi * 51 - b;
    int cd = cr * cr + cg * cg + cb * cb;
    int avg = (r + g + b) / 3;
    int gi2 = (avg + 5) / 10;
    if (gi2 > 23) gi2 = 23;
    int gv = gi2 * 10;
    int gd = (gv - r) * (gv - r) + (gv - g) * (gv - g) + (gv - b) * (gv - b);
    if (gd < cd) best = 232 + gi2;
    return best;
}

void rb_cube_rgb(int index, int *r, int *g, int *b) {
    if (index >= 232 && index <= 255) {
        int v = (index - 232) * 10;
        *r = *g = *b = v;
        return;
    }
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

/* ---- numbers and lengths ------------------------------------------------ */

typedef struct {
    int mil;          /* value * 1000, saturated */
    uint8_t unit;     /* U_* */
    uint8_t ok;
} Dim;

enum { U_NONE = 0, U_PX, U_EM, U_REM, U_PCT, U_PT, U_PC, U_CM, U_MM, U_IN, U_VW, U_VH,
       U_VMIN, U_VMAX, U_EX, U_CH, U_FR, U_DEG, U_S, U_MS, U_Q };

static int is_alpha_c(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_digit_c(int c) { return c >= '0' && c <= '9'; }

/* Parse "-1.5em" style tokens. Returns chars consumed (0 when not a number). */
static int parse_dim(const char *s, int n, Dim *d) {
    int i = 0, neg = 0;
    d->mil = 0; d->unit = U_NONE; d->ok = 0;
    if (i < n && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; i++; }
    int any = 0;
    int v = 0;
    while (i < n && is_digit_c(s[i])) {
        if (v < 2000) v = v * 10 + (s[i] - '0');      /* saturates near 20000 */
        i++; any = 1;
    }
    v *= 1000;
    if (i < n && s[i] == '.') {
        i++;
        int scale = 100;
        while (i < n && is_digit_c(s[i])) {
            if (scale > 0) { v += (s[i] - '0') * scale; scale /= 10; }
            i++; any = 1;
        }
    }
    if (!any) return 0;
    if (i < n && (s[i] == 'e' || s[i] == 'E') && i + 1 < n && (is_digit_c(s[i + 1]) || s[i + 1] == '-')) {
        /* exponent form: not worth supporting, treat the number as 0 */
        i++;
        if (i < n && s[i] == '-') i++;
        while (i < n && is_digit_c(s[i])) i++;
        v = 0;
    }
    if (v > 20000000L) v = 20000000L;
    d->mil = (int)(neg ? -v : v);
    int us = i;
    while (i < n && (is_alpha_c(s[i]) || s[i] == '%')) i++;
    int un = i - us;
    d->unit = U_NONE;
    if (un == 1 && s[us] == '%') d->unit = U_PCT;
    else if (un == 2 && eq_lit(s + us, 2, "px")) d->unit = U_PX;
    else if (un == 2 && eq_lit(s + us, 2, "em")) d->unit = U_EM;
    else if (un == 3 && eq_lit(s + us, 3, "rem")) d->unit = U_REM;
    else if (un == 2 && eq_lit(s + us, 2, "pt")) d->unit = U_PT;
    else if (un == 2 && eq_lit(s + us, 2, "pc")) d->unit = U_PC;
    else if (un == 2 && eq_lit(s + us, 2, "cm")) d->unit = U_CM;
    else if (un == 2 && eq_lit(s + us, 2, "mm")) d->unit = U_MM;
    else if (un == 2 && eq_lit(s + us, 2, "in")) d->unit = U_IN;
    else if (un == 2 && eq_lit(s + us, 2, "vw")) d->unit = U_VW;
    else if (un == 2 && eq_lit(s + us, 2, "vh")) d->unit = U_VH;
    else if (un == 4 && eq_lit(s + us, 4, "vmin")) d->unit = U_VMIN;
    else if (un == 4 && eq_lit(s + us, 4, "vmax")) d->unit = U_VMAX;
    else if (un == 2 && eq_lit(s + us, 2, "ex")) d->unit = U_EX;
    else if (un == 2 && eq_lit(s + us, 2, "ch")) d->unit = U_CH;
    else if (un == 2 && eq_lit(s + us, 2, "fr")) d->unit = U_FR;
    else if (un == 3 && eq_lit(s + us, 3, "deg")) d->unit = U_DEG;
    else if (un == 1 && (s[us] == 's' || s[us] == 'S')) d->unit = U_S;
    else if (un == 2 && eq_lit(s + us, 2, "ms")) d->unit = U_MS;
    else if (un == 1 && (s[us] == 'q' || s[us] == 'Q')) d->unit = U_Q;
    else if (un != 0) return 0;      /* unknown unit */
    d->ok = 1;
    return i;
}

/* A dimension in CSS px, thousandths. em/rem resolve against 16px. */
static int dim_css_mil(const Dim *d) {
    int v = d->mil;          /* |v| <= 20,000,000: every product below fits an int */
    switch (d->unit) {
    case U_NONE: case U_PX: return v;
    case U_EM: case U_REM: return v * 16;
    case U_EX: case U_CH: return v * 8;
    case U_PT: return v * 4 / 3;
    case U_PC: return v * 16;
    case U_CM: return v / 10 * 378;
    case U_MM: return v / 100 * 378;
    case U_Q: return v / 100 * 378 / 4;
    case U_IN: return v * 96;
    case U_VW: return v / 100 * RB_VIEW_W;
    case U_VH: return v / 100 * (RB_VIEW_H * 2);        /* the viewport is 2 * RB_VIEW_H css px tall */
    case U_VMIN: return v / 100 * RB_VIEW_W;
    case U_VMAX: return v / 100 * (RB_VIEW_H * 2);
    default: return v;
    }
}

/* Screen px of a (non-percentage) dimension. Viewport units already are in the
   web profile's CSS space (RB_VIEW_W wide) and zoom with everything else. */
static int dim_px(const Dim *d) {
    int css = dim_css_mil(d) / 1000;
    if (d->unit == U_PX || d->unit == U_NONE) {
        /* a sub-pixel positive length (0.5px borders) must not vanish */
        int m = dim_css_mil(d);
        if (m > 0 && css == 0) css = 1;
    }
    if (!g_web) {
        if (d->unit == U_VW) return d->mil / 100 * RB_VIEW_W / 1000;
        if (d->unit == U_VH) return d->mil / 100 * RB_VIEW_H / 1000;
        if (d->unit == U_VMIN) return d->mil / 100 * RB_VIEW_H / 1000;
        if (d->unit == U_VMAX) return d->mil / 100 * RB_VIEW_W / 1000;
        if (d->unit == U_EM || d->unit == U_REM) return d->mil * 8 / 1000;
    }
    return px_zoom(css);
}

/* ---- calc() / min() / max() / clamp() ------------------------------------
   Everything reduces to  pct% + px  (thousandths, screen px). */
typedef struct { int pct; int px; int ok; } Lin;

/* a * b / c in 32-bit arithmetic only (long is 32 bits on the target): the
   product is checked against the int range first, and an oversized one falls
   back to dividing first, saturating at +-1e8. */
#define CALC_MAX 100000000
static int sat(int v) { return v > CALC_MAX ? CALC_MAX : (v < -CALC_MAX ? -CALC_MAX : v); }
static int sat_add(int a, int b) { return sat(sat(a) + sat(b)); }   /* both clamped: the sum fits */

static int mul_div(int a, int b, int c) {
    if (c == 0 || a == 0 || b == 0) return 0;
    int neg = 0;
    if (c < 0) { c = -c; neg ^= 1; }
    if (a < 0) { a = -a; neg ^= 1; }
    if (b < 0) { b = -b; neg ^= 1; }
    unsigned ua = (unsigned)a, ub = (unsigned)b, uc = (unsigned)c, r;
    if (ua <= 2000000000u / ub) r = ua * ub / uc;
    else {
        unsigned q = ua / uc;
        r = q > (unsigned)CALC_MAX / ub ? (unsigned)CALC_MAX : q * ub;
    }
    if (r > (unsigned)CALC_MAX) r = (unsigned)CALC_MAX;
    return neg ? -(int)r : (int)r;
}

static int calc_expr(const char *s, int n, int *pos, Lin *out);

static int calc_atom(const char *s, int n, int *pos, Lin *out) {
    int i = *pos;
    while (i < n && s[i] == ' ') i++;
    Lin a = { 0, 0, 1 };
    if (i < n && s[i] == '(') {
        i++;
        if (!calc_expr(s, n, &i, &a)) return 0;
        while (i < n && s[i] == ' ') i++;
        if (i < n && s[i] == ')') i++;
        *out = a; *pos = i;
        return 1;
    }
    if (i + 4 <= n && (starts_lit(s + i, n - i, "calc(") || starts_lit(s + i, n - i, "min(") ||
                       starts_lit(s + i, n - i, "max(") || starts_lit(s + i, n - i, "clamp("))) {
        int kind = starts_lit(s + i, n - i, "min(") ? 1 : starts_lit(s + i, n - i, "max(") ? 2 :
                   starts_lit(s + i, n - i, "clamp(") ? 3 : 0;
        while (i < n && s[i] != '(') i++;
        i++;
        Lin args[3];
        int na = 0;
        for (;;) {
            Lin v;
            if (!calc_expr(s, n, &i, &v)) return 0;
            if (na < 3) args[na++] = v;
            while (i < n && s[i] == ' ') i++;
            if (i < n && s[i] == ',') { i++; continue; }
            break;
        }
        if (i < n && s[i] == ')') i++;
        if (kind == 0) a = args[0];
        else {
            /* Percentages inside min/max/clamp resolve against the viewport
               width here (no layout context at parse time). */
            int v[3];
            for (int k = 0; k < na; k++) v[k] = sat_add(args[k].px, sat(args[k].pct / 100) * RB_VIEW_W);
            if (kind == 1) { int m = v[0]; for (int k = 1; k < na; k++) if (v[k] < m) m = v[k]; a.px = m; }
            else if (kind == 2) { int m = v[0]; for (int k = 1; k < na; k++) if (v[k] > m) m = v[k]; a.px = m; }
            else if (na == 3) { int m = v[1]; if (m < v[0]) m = v[0]; if (m > v[2]) m = v[2]; a.px = m; }
            else a.px = v[0];
            a.pct = 0;
        }
        *out = a; *pos = i;
        return 1;
    }
    Dim d;
    int c = parse_dim(s + i, n - i, &d);
    if (!c) return 0;
    i += c;
    if (d.unit == U_PCT) { a.pct = d.mil; a.px = 0; }
    else if (d.unit == U_NONE) { a.px = d.mil; a.pct = 0; a.ok = 2; }   /* bare number */
    else a.px = dim_px(&d) * 1000;
    *out = a; *pos = i;
    return 1;
}

static int calc_term(const char *s, int n, int *pos, Lin *out) {
    Lin a;
    if (!calc_atom(s, n, pos, &a)) return 0;
    for (;;) {
        int i = *pos;
        while (i < n && s[i] == ' ') i++;
        if (i < n && (s[i] == '*' || s[i] == '/')) {
            char op = s[i];
            i++;
            Lin b;
            *pos = i;
            if (!calc_atom(s, n, pos, &b)) return 0;
            if (op == '*') {
                if (b.ok == 2) { a.px = mul_div(a.px, b.px, 1000); a.pct = mul_div(a.pct, b.px, 1000); }
                else if (a.ok == 2) { int k = a.px; a = b; a.px = mul_div(a.px, k, 1000); a.pct = mul_div(a.pct, k, 1000); }
                else return 0;
            } else {
                if (b.ok != 2 || b.px == 0) return 0;
                a.px = mul_div(a.px, 1000, b.px);
                a.pct = mul_div(a.pct, 1000, b.px);
            }
            continue;
        }
        break;
    }
    *out = a;
    return 1;
}

static int calc_expr(const char *s, int n, int *pos, Lin *out) {
    Lin a;
    if (!calc_term(s, n, pos, &a)) return 0;
    for (;;) {
        int i = *pos;
        while (i < n && s[i] == ' ') i++;
        if (i < n && (s[i] == '+' || s[i] == '-') && i + 1 < n && s[i + 1] == ' ') {
            char op = s[i];
            *pos = i + 1;
            Lin b;
            if (!calc_term(s, n, pos, &b)) return 0;
            if (op == '+') { a.px = sat_add(a.px, b.px); a.pct = sat_add(a.pct, b.pct); }
            else { a.px = sat_add(a.px, -sat(b.px)); a.pct = sat_add(a.pct, -sat(b.pct)); }
            a.ok = 1;
            continue;
        }
        *pos = i;
        break;
    }
    *out = a;
    return 1;
}

/* Parse a whole length token (dimension, bare 0, or a math function) into
   screen px and an optional percentage. Returns 0 when it is neither. */
/* em/rem component (thousandths of an em / rem) of the last simple length parsed by
   parse_length: the px it returns assume a 16px font, so the cascade re-resolves
   it against the element's computed font-size (em) or the root's (rem), see
   em_fix() in css_cascade.inc. Zero for px, %, calc() and the like. */
static int g_len_em, g_len_rem;

static int parse_length(const char *s, int n, int *px, int *pct, int *has_pct) {
    g_len_em = g_len_rem = 0;
    while (n > 0 && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    *px = 0; *pct = 0; *has_pct = 0;
    if (n <= 0) return 0;
    if (starts_lit(s, n, "calc(") || starts_lit(s, n, "min(") || starts_lit(s, n, "max(") ||
        starts_lit(s, n, "clamp(")) {
        int pos = 0;
        Lin v;
        if (!calc_expr(s, n, &pos, &v)) return 0;
        *px = v.px >= 0 ? (v.px + 500) / 1000 : -((-v.px + 500) / 1000);
        if (v.pct) { *has_pct = 1; *pct = (int)(v.pct / 1000); }
        return 1;
    }
    Dim d;
    int c = parse_dim(s, n, &d);
    if (!c || c != n) return 0;
    if (d.unit == U_PCT) { *has_pct = 1; *pct = d.mil / 1000; return 1; }
    if (d.unit == U_NONE && d.mil != 0) return 0;     /* unitless non-zero is not a length */
    if (d.unit == U_FR || d.unit == U_DEG || d.unit == U_S || d.unit == U_MS) return 0;
    *px = dim_px(&d);
    if (g_web && d.mil >= -32000 && d.mil <= 32000) {
        if (d.unit == U_EM) g_len_em = d.mil;
        else if (d.unit == U_REM) g_len_rem = d.mil;
    }
    return 1;
}

/* ---- colour values ------------------------------------------------------ */

#define COL_NONE (-2)     /* transparent / none */
#define COL_INHERIT (-3)
#define COL_CURRENT (-4)

static int parse_func_args(const char *s, int n, int *v, int *isp, int max) {
    /* "12, 34%, 5" or "12 34% 5 / .5" -> numbers (thousandths for alpha). */
    int cnt = 0, i = 0;
    while (i < n && cnt < max) {
        while (i < n && (s[i] == ' ' || s[i] == ',' || s[i] == '/')) i++;
        if (i >= n) break;
        Dim d;
        int c = parse_dim(s + i, n - i, &d);
        if (!c) {
            if (starts_lit(s + i, n - i, "none")) { v[cnt] = 0; isp[cnt++] = 0; i += 4; continue; }
            return -1;
        }
        i += c;
        v[cnt] = d.mil > 10000000 ? 10000000 : (d.mil < -10000000 ? -10000000 : d.mil);
        isp[cnt] = d.unit == U_PCT ? 1 : (d.unit == U_DEG ? 2 : 0);
        cnt++;
    }
    return cnt;
}

static int hsl_chan(int p, int q, int t) {      /* 0..255; t is the hue in millidegrees */
    if (t < 0) t += 360000;
    if (t >= 360000) t -= 360000;
    if (t < 60000) return p + (q - p) * (t / 10) / 6000;
    if (t < 180000) return q;
    if (t < 240000) return p + (q - p) * ((240000 - t) / 10) / 6000;
    return p;
}

/* Returns 1 and sets *out to an 0xRRGGBB colour and *alpha to its alpha
   (0..255), or *out to COL_NONE (transparent, alpha 0), COL_CURRENT or
   COL_INHERIT (alpha 255). The colour is not blended with anything. */
static int parse_color_alpha(const char *s, int n, int *out, int *alpha) {
    *alpha = 255;
    while (n > 0 && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == ';' || s[n - 1] == '\t')) n--;
    if (n <= 0) return 0;
    if (s[0] == '#') {
        int h[8], k = 0;
        for (int i = 1; i < n && k < 8; i++) { h[k] = hex_val(s[i]); if (h[k] < 0) return 0; k++; }
        if (k != 3 && k != 4 && k != 6 && k != 8) return 0;
        int r, g, b, a = 255;
        if (k <= 4) { r = h[0] * 17; g = h[1] * 17; b = h[2] * 17; if (k == 4) a = h[3] * 17; }
        else { r = h[0] * 16 + h[1]; g = h[2] * 16 + h[3]; b = h[4] * 16 + h[5]; if (k == 8) a = h[6] * 16 + h[7]; }
        *out = rb_map_rgb(r, g, b);
        *alpha = a;
        return 1;
    }
    if (starts_lit(s, n, "light-dark(")) {
        /* the engine is always in the light scheme: take the first argument */
        int d = 0, e = 11;
        while (e < n) {
            if (s[e] == '(') d++;
            else if (s[e] == ')') { if (d == 0) break; d--; }
            else if (s[e] == ',' && d == 0) break;
            e++;
        }
        return parse_color_alpha(s + 11, e - 11, out, alpha);
    }
    if (eq_lit(s, n, "transparent")) { *out = COL_NONE; *alpha = 0; return 1; }
    if (eq_lit(s, n, "currentcolor")) { *out = COL_CURRENT; return 1; }
    if (eq_lit(s, n, "inherit")) { *out = COL_INHERIT; return 1; }
    int r, g, b;
    if (named_color(s, n, &r, &g, &b)) { *out = rb_map_rgb(r, g, b); return 1; }
    int hsl = starts_lit(s, n, "hsl(") || starts_lit(s, n, "hsla(");
    int rgb = starts_lit(s, n, "rgb(") || starts_lit(s, n, "rgba(");
    if (hsl || rgb) {
        int op = 0;
        while (op < n && s[op] != '(') op++;
        int close = n;
        if (s[n - 1] == ')') close = n - 1;
        int v[5], isp[5];
        int c = parse_func_args(s + op + 1, close - op - 1, v, isp, 5);
        if (c < 3) return 0;
        int a = 255;
        if (c >= 4) {
            a = isp[3] ? (v[3] > 100000 ? 255 : (v[3] < 0 ? 0 : (v[3] * 255 + 50000) / 100000)) : (v[3] > 1000 ? 255 : (v[3] < 0 ? 0 : (v[3] * 255 + 500) / 1000));
            if (a > 255) a = 255;
            if (a < 0) a = 0;
        }
        if (rgb) {
            int ch[3];
            for (int k = 0; k < 3; k++) {
                ch[k] = isp[k] ? (v[k] > 100000 ? 255 : (v[k] < 0 ? 0 : v[k] * 255 / 100000)) : v[k] / 1000;
                if (ch[k] < 0) ch[k] = 0;
                if (ch[k] > 255) ch[k] = 255;
            }
            r = ch[0]; g = ch[1]; b = ch[2];
        } else {
            int hue = v[0] % 360000;      /* millidegrees */
            if (hue < 0) hue += 360000;
            int sat = v[1] / 1000, lig = v[2] / 1000;
            if (sat > 100) sat = 100;
            if (sat < 0) sat = 0;
            if (lig > 100) lig = 100;
            if (lig < 0) lig = 0;
            int q = lig < 50 ? lig * (100 + sat) / 100 : lig + sat - lig * sat / 100;
            int p = 2 * lig - q;
            int q255 = q * 255 / 100, p255 = p * 255 / 100;
            r = hsl_chan(p255, q255, hue + 120000);
            g = hsl_chan(p255, q255, hue);
            b = hsl_chan(p255, q255, hue - 120000);
        }
        *out = rb_map_rgb(r, g, b);
        *alpha = a;
        return 1;
    }
    return 0;
}

/* Returns 1 and sets *out to an 0xRRGGBB colour, COL_NONE (transparent),
   COL_CURRENT or COL_INHERIT. A page has no alpha: a colour under half
   opacity is transparent, any other translucent one is blended with white. */
static int parse_color(const char *s, int n, int *out) {
    int a;
    if (!parse_color_alpha(s, n, out, &a)) return 0;
    if (*out < 0) return 1;
    if (a < 128) { *out = COL_NONE; return 1; }
    if (a < 255) {
        int r = RB_R(*out), g = RB_G(*out), b = RB_B(*out);
        r = (r * a + 255 * (255 - a)) / 255; g = (g * a + 255 * (255 - a)) / 255; b = (b * a + 255 * (255 - a)) / 255;
        *out = rb_map_rgb(r, g, b);
    }
    return 1;
}

int rb_parse_color(const char *s, int len, uint32_t *argb) {
    int c, a;
    if (!s || len < 0 || len > 256 || !parse_color_alpha(s, len, &c, &a)) return 0;
    if (c == COL_NONE) { *argb = 0; return 1; }
    if (c < 0) return 0;
    *argb = ((uint32_t)a << 24) | ((uint32_t)c & 0xFFFFFFu);
    return 1;
}

/* Next whitespace-separated token; parentheses and quotes protect inner
   spaces. */
static void trim_tok(const char **s, int *n) {
    while (*n > 0 && ((*s)[0] == ' ' || (*s)[0] == '\t' || (*s)[0] == '\n' || (*s)[0] == '\r')) { (*s)++; (*n)--; }
    while (*n > 0 && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t' || (*s)[*n - 1] == '\n' || (*s)[*n - 1] == '\r')) (*n)--;
}

static int next_tok(const char *s, int n, int *i, const char **t, int *tn) {
    while (*i < n && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\n' || s[*i] == '\r')) (*i)++;
    if (*i >= n) return 0;
    int st = *i, depth = 0;
    char q = 0;
    while (*i < n) {
        char c = s[*i];
        if (q) { if (c == q) q = 0; (*i)++; continue; }
        if (c == '"' || c == '\'') q = c;
        else if (c == '(') depth++;
        else if (c == ')' && depth > 0) depth--;
        else if ((c == ' ' || c == '\t' || c == '\n' || c == '\r') && depth == 0) break;
        (*i)++;
    }
    *t = s + st;
    *tn = *i - st;
    return 1;
}

static int ends_lit(const char *s, int n, const char *lit) {
    int ln = (int)strlen(lit);
    return n >= ln && memcmp(s + n - ln, lit, (size_t)ln) == 0;
}

/* Bare number, hundredths. Returns 1 only for a whole-token number. */
static int parse_number(const char *s, int n, int *out) {
    trim_tok(&s, &n);
    Dim d;
    int c = parse_dim(s, n, &d);
    if (!c || c != n || d.unit != U_NONE) return 0;
    int v = d.mil / 10;
    if (v < 0) v = 0;
    if (v > FLEX_NUM_MAX * FLEX_SCALE) v = FLEX_NUM_MAX * FLEX_SCALE;
    *out = (int)v;
    return 1;
}

/* Signed integer; returns 1 only when the whole token is one. */
static int parse_int_tok(const char *s, int n, int *out) {
    trim_tok(&s, &n);
    int neg = 0, i = 0, v = 0;
    if (n > 0 && (s[0] == '-' || s[0] == '+')) { neg = s[0] == '-'; i = 1; }
    int any = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); if (v > 100000) v = 100000; i++; any = 1; }
    if (!any || i != n) return 0;
    *out = neg ? -v : v;
    return 1;
}
