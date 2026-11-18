/* rbrowse HTML front end: character folding, entities, attributes and the
   tree builder. Included by engine.c. The 8x8 bitmap font only has ASCII, so
   every decoded code point is folded to a short ASCII string. */

/* ---- Unicode -> ASCII ---------------------------------------------------- */

static const char *const LATIN1_HI[64] = {
    "A","A","A","A","A","A","AE","C","E","E","E","E","I","I","I","I",
    "D","N","O","O","O","O","O","x","O","U","U","U","U","Y","Th","ss",
    "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
    "d","n","o","o","o","o","o","/","o","u","u","u","u","y","th","y"
};

static const char *const LATIN1_LO[32] = {   /* 160..191 */
    " ","!","c","L","o","Y","|","S","\"","(c)","a","<<","!","","(R)","-",
    "o","+-","2","3","'","u","P",".",",","1","o",">>","1/4","1/2","3/4","?"
};

/* Latin Extended-A, 256..383: one base letter each (Ĳ/ĳ/Œ/œ handled apart). */
static const char LATIN_EXT_A[] =
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

static const char *const CYR_UP[32] = {
    "A","B","V","G","D","E","Zh","Z","I","J","K","L","M","N","O","P","R","S","T","U","F","Kh","Ts","Ch","Sh","Shch","\"","Y","'","E","Yu","Ya"
};
static const char *const GRK_UP[25] = {     /* 0x391..0x3A9, Alpha..Omega (0x3A2 unused) */
    "A","B","G","D","E","Z","E","Th","I","K","L","M","N","X","O","P","R","","S","T","Y","Ph","Ch","Ps","O"
};

static int cat_str(char *out, int w, int cap, const char *s) {
    while (*s && w < cap) out[w++] = *s++;
    return w;
}

/* Fold one code point to ASCII, appending to out (cap bytes). Returns the new
   length. */
static int fold_cp(int cp, char *out, int w, int cap) {
    if (w >= cap) return w;
    if (cp < 0) return w;
    if (cp < 0x80) {
        if (cp == '\r' || cp == 0) return w;
        if (cp < 32 && cp != '\n' && cp != '\t') return w;
        out[w++] = (char)cp;
        return w;
    }
    if (cp < 0xA0) return w;                          /* C1 controls */
    if (cp < 0xC0) return cat_str(out, w, cap, LATIN1_LO[cp - 160]);
    if (cp < 0x100) return cat_str(out, w, cap, LATIN1_HI[cp - 192]);
    if (cp < 0x180) {
        if (cp == 306) return cat_str(out, w, cap, "IJ");
        if (cp == 307) return cat_str(out, w, cap, "ij");
        if (cp == 338) return cat_str(out, w, cap, "OE");
        if (cp == 339) return cat_str(out, w, cap, "oe");
        out[w++] = LATIN_EXT_A[cp - 256];
        return w;
    }
    if (cp >= 0x300 && cp <= 0x36F) return w;          /* combining marks */
    if (cp >= 0x391 && cp <= 0x3A9) return cat_str(out, w, cap, GRK_UP[cp - 0x391]);
    if (cp >= 0x3B1 && cp <= 0x3C9) {
        if (cp == 0x3C2) return cat_str(out, w, cap, "s");
        const char *u = GRK_UP[cp - 0x3B1];
        char c = u[0] >= 'A' && u[0] <= 'Z' ? (char)(u[0] + 32) : u[0];
        if (!c) return w;
        out[w++] = c;
        if (u[1] && w < cap) out[w++] = u[1];
        return w;
    }
    if (cp == 0x401) return cat_str(out, w, cap, "Yo");
    if (cp == 0x451) return cat_str(out, w, cap, "yo");
    if (cp >= 0x410 && cp <= 0x42F) return cat_str(out, w, cap, CYR_UP[cp - 0x410]);
    if (cp >= 0x430 && cp <= 0x44F) {
        const char *u = CYR_UP[cp - 0x430];
        out[w++] = u[0] >= 'A' && u[0] <= 'Z' ? (char)(u[0] + 32) : u[0];
        for (int k = 1; u[k] && w < cap; k++) out[w++] = u[k];
        return w;
    }
    if (cp >= 0x2000 && cp <= 0x200A) { out[w++] = ' '; return w; }
    switch (cp) {
    case 0x200B: case 0x200C: case 0x200D: case 0x200E: case 0x200F: case 0x2060: case 0xFEFF:
    case 0x202A: case 0x202B: case 0x202C: case 0x202D: case 0x202E: case 0x2066: case 0x2067:
    case 0x2068: case 0x2069: case 0xAD:
        return w;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
        out[w++] = '-'; return w;
    case 0x2018: case 0x2019: case 0x201B: case 0x2032: case 0x2035:
        out[w++] = '\''; return w;
    case 0x201A: out[w++] = ','; return w;
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033:
        out[w++] = '"'; return w;
    case 0x2020: case 0x2021: out[w++] = '+'; return w;
    case 0x2022: case 0x25CF: case 0x25CB: case 0x2023: case 0x2043: case 0x2219: case 0x22C5: case 0xB7:
        out[w++] = '*'; return w;
    case 0x2026: return cat_str(out, w, cap, "...");
    case 0x202F: case 0x205F: case 0x3000: out[w++] = ' '; return w;
    case 0x2030: return cat_str(out, w, cap, "%o");
    case 0x2039: out[w++] = '<'; return w;
    case 0x203A: out[w++] = '>'; return w;
    case 0x2044: out[w++] = '/'; return w;
    case 0x20AC: out[w++] = 'E'; return w;
    case 0x20A4: out[w++] = 'L'; return w;
    case 0x2122: return cat_str(out, w, cap, "TM");
    case 0x2190: return cat_str(out, w, cap, "<-");
    case 0x2191: out[w++] = '^'; return w;
    case 0x2192: return cat_str(out, w, cap, "->");
    case 0x2193: out[w++] = 'v'; return w;
    case 0x2194: return cat_str(out, w, cap, "<->");
    case 0x21D2: return cat_str(out, w, cap, "=>");
    case 0x21D4: return cat_str(out, w, cap, "<=>");
    case 0x221E: return cat_str(out, w, cap, "inf");
    case 0x2248: out[w++] = '~'; return w;
    case 0x2260: return cat_str(out, w, cap, "!=");
    case 0x2264: return cat_str(out, w, cap, "<=");
    case 0x2265: return cat_str(out, w, cap, ">=");
    case 0x221A: return cat_str(out, w, cap, "sqrt");
    case 0x2211: out[w++] = 'E'; return w;
    case 0x25A0: case 0x25AA: case 0x25A1: out[w++] = '#'; return w;
    case 0x25B2: case 0x25B4: out[w++] = '^'; return w;
    case 0x25BC: case 0x25BE: out[w++] = 'v'; return w;
    case 0x25B6: case 0x25BA: case 0x25B8: out[w++] = '>'; return w;
    case 0x25C0: case 0x25C4: case 0x25C2: out[w++] = '<'; return w;
    case 0x25CA: return cat_str(out, w, cap, "<>");
    case 0x2605: case 0x2606: out[w++] = '*'; return w;
    case 0x2713: case 0x2714: out[w++] = 'v'; return w;
    case 0x2715: case 0x2716: case 0x2717: case 0x2718: out[w++] = 'x'; return w;
    case 0x2500: case 0x2501: case 0x2504: case 0x2505: out[w++] = '-'; return w;
    case 0x2502: case 0x2503: case 0x2506: case 0x2507: out[w++] = '|'; return w;
    default: break;
    }
    if (cp >= 0x2500 && cp <= 0x257F) { out[w++] = '+'; return w; }       /* box drawing */
    if (cp >= 0xFE00 && cp <= 0xFE0F) return w;                          /* variation selectors */
    if (cp >= 0x2190 && cp <= 0x21FF) { out[w++] = '>'; return w; }
    if ((cp >= 0x2600 && cp <= 0x27BF) || cp >= 0x1F000 || (cp >= 0x2B00 && cp <= 0x2BFF) ||
        (cp >= 0xE000 && cp <= 0xF8FF)) return w;                         /* symbols, emoji, private use */
    out[w++] = '?';
    return w;
}

static int unicode_to_glyph(int cp) {
    char b[8];
    int n = fold_cp(cp, b, 0, 7);
    return n > 0 ? (unsigned char)b[0] : '?';
}

typedef struct { const char *n; uint16_t cp; } EntDef;
static const EntDef g_ent[] = {
    {"AElig",198},{"Aacute",193},{"Acirc",194},{"Agrave",192},{"Alpha",913},{"Aring",197},{"Atilde",195},
    {"Auml",196},{"Beta",914},{"Ccedil",199},{"Chi",935},{"Dagger",8225},{"Delta",916},{"ETH",208},
    {"Eacute",201},{"Ecirc",202},{"Egrave",200},{"Epsilon",917},{"Eta",919},{"Euml",203},{"Gamma",915},
    {"Iacute",205},{"Icirc",206},{"Igrave",204},{"Iota",921},{"Iuml",207},{"Kappa",922},{"Lambda",923},
    {"Mu",924},{"Ntilde",209},{"Nu",925},{"OElig",338},{"Oacute",211},{"Ocirc",212},{"Ograve",210},
    {"Omega",937},{"Omicron",927},{"Oslash",216},{"Otilde",213},{"Ouml",214},{"Phi",934},{"Pi",928},
    {"Prime",8243},{"Psi",936},{"Rho",929},{"Scaron",352},{"Sigma",931},{"THORN",222},{"Tau",932},
    {"Theta",920},{"Uacute",218},{"Ucirc",219},{"Ugrave",217},{"Upsilon",933},{"Uuml",220},{"Xi",926},
    {"Yacute",221},{"Yuml",376},{"Zeta",918},{"aacute",225},{"acirc",226},{"acute",180},{"aelig",230},
    {"agrave",224},{"alpha",945},{"amp",38},{"and",8743},{"apos",39},{"aring",229},{"asymp",8776},
    {"atilde",227},{"auml",228},{"bdquo",8222},{"beta",946},{"brvbar",166},{"bsol",92},{"bull",8226},
    {"cap",8745},{"ccedil",231},{"cedil",184},{"cent",162},{"check",10003},{"chi",967},{"circ",710},
    {"clubs",9827},{"colon",58},{"comma",44},{"commat",64},{"cong",8773},{"copy",169},{"cross",10007},
    {"cup",8746},{"curren",164},{"dagger",8224},{"darr",8595},{"deg",176},{"delta",948},{"diams",9830},
    {"divide",247},{"dollar",36},{"eacute",233},{"ecirc",234},{"egrave",232},{"empty",8709},{"emsp",8195},
    {"ensp",8194},{"epsilon",949},{"equals",61},{"equiv",8801},{"eta",951},{"eth",240},{"euml",235},
    {"euro",8364},{"excl",33},{"exist",8707},{"forall",8704},{"frac12",189},{"frac14",188},{"frac34",190},
    {"frasl",8260},{"gamma",947},{"ge",8805},{"grave",96},{"gt",62},{"hArr",8660},{"hairsp",8202},
    {"harr",8596},{"hearts",9829},{"hellip",8230},{"iacute",237},{"icirc",238},{"iexcl",161},{"igrave",236},
    {"infin",8734},{"int",8747},{"iota",953},{"iquest",191},{"isin",8712},{"iuml",239},{"kappa",954},
    {"lArr",8656},{"lambda",955},{"laquo",171},{"larr",8592},{"lbrace",123},{"lbrack",91},{"lcub",123},
    {"ldquo",8220},{"le",8804},{"lowast",8727},{"lowbar",95},{"loz",9674},{"lpar",40},{"lrm",8206},
    {"lsaquo",8249},{"lsqb",91},{"lsquo",8216},{"lt",60},{"macr",175},{"mdash",8212},{"micro",181},
    {"middot",183},{"minus",8722},{"mu",956},{"nabla",8711},{"nbsp",160},{"ndash",8211},{"ne",8800},
    {"ni",8715},{"not",172},{"notin",8713},{"nsub",8836},{"ntilde",241},{"nu",957},{"num",35},
    {"oacute",243},{"ocirc",244},{"oelig",339},{"ograve",242},{"oline",8254},{"omega",969},{"omicron",959},
    {"or",8744},{"ordf",170},{"ordm",186},{"oslash",248},{"otilde",245},{"ouml",246},{"para",182},
    {"part",8706},{"percnt",37},{"period",46},{"permil",8240},{"perp",8869},{"phi",966},{"pi",960},
    {"plus",43},{"plusmn",177},{"pound",163},{"prime",8242},{"prod",8719},{"psi",968},{"quest",63},
    {"quot",34},{"rArr",8658},{"radic",8730},{"raquo",187},{"rarr",8594},{"rbrace",125},{"rbrack",93},
    {"rcub",125},{"rdquo",8221},{"reg",174},{"rho",961},{"rlm",8207},{"rpar",41},{"rsaquo",8250},
    {"rsqb",93},{"rsquo",8217},{"sbquo",8218},{"scaron",353},{"sdot",8901},{"sect",167},{"semi",59},
    {"shy",173},{"sigma",963},{"sigmaf",962},{"sim",8764},{"sol",47},{"spades",9824},{"star",9734},
    {"starf",9733},{"sub",8834},{"sube",8838},{"sum",8721},{"sup",8835},{"sup1",185},{"sup2",178},
    {"sup3",179},{"supe",8839},{"szlig",223},{"tau",964},{"there4",8756},{"theta",952},{"thinsp",8201},
    {"thorn",254},{"tilde",732},{"times",215},{"trade",8482},{"uacute",250},{"uarr",8593},{"ucirc",251},
    {"ugrave",249},{"uml",168},{"upsilon",965},{"uuml",252},{"verbar",124},{"vert",124},{"xi",958},
    {"yacute",253},{"yen",165},{"yuml",255},{"zeta",950},{"zwj",8205},{"zwnj",8204},
};

/* Entity at s[0] == '&'. Returns the code point (>= 0) and sets *used, or -1
   when it is not an entity. */
static int entity_cp(const char *s, int n, int *used, int in_attr) {
    if (n < 2) return -1;
    if (s[1] == '#') {
        int i = 2, hex = 0, v = 0, any = 0;
        if (i < n && (s[i] == 'x' || s[i] == 'X')) { hex = 1; i++; }
        while (i < n) {
            int d = hex ? hex_val(s[i]) : (s[i] >= '0' && s[i] <= '9' ? s[i] - '0' : -1);
            if (d < 0) break;
            if (v < 0x110000) v = hex ? v * 16 + d : v * 10 + d;
            any = 1;
            i++;
        }
        if (!any) return -1;
        if (i < n && s[i] == ';') i++;
        *used = i;
        if (v == 0 || v > 0x10FFFF) return 0xFFFD;
        if (v >= 0x80 && v < 0xA0) {         /* windows-1252 range */
            static const uint16_t w1252[32] = { 0x20AC,0x81,0x201A,0x192,0x201E,0x2026,0x2020,0x2021,0x2C6,0x2030,0x160,0x2039,0x152,0x8D,0x17D,0x8F,
                0x90,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x2DC,0x2122,0x161,0x203A,0x153,0x9D,0x17E,0x178 };
            v = w1252[v - 0x80];
        }
        return v;
    }
    int e = 1;
    while (e < n && e < 12 && ((s[e] >= 'a' && s[e] <= 'z') || (s[e] >= 'A' && s[e] <= 'Z') || (s[e] >= '0' && s[e] <= '9'))) e++;
    int nl = e - 1;
    if (nl <= 0) return -1;
    int semi = e < n && s[e] == ';';
    int lo = 0, hi = (int)(sizeof(g_ent) / sizeof(g_ent[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *nm = g_ent[mid].n;
        int c = 0;
        int i = 0;
        for (; i < nl; i++) {
            if (!nm[i]) { c = 1; break; }
            if (s[1 + i] != nm[i]) { c = (unsigned char)s[1 + i] < (unsigned char)nm[i] ? -1 : 1; break; }
        }
        if (c == 0 && nm[i]) c = -1;
        if (c == 0) {
            /* without the semicolon only the classic ones are honoured, and
               only when not followed by more name characters */
            if (!semi && !(g_ent[mid].cp < 256 || g_ent[mid].cp == 8212 || g_ent[mid].cp == 8211)) return -1;
            /* in an attribute a semicolon-less entity followed by '=' or a
               letter/digit is literal text (href="?a=1&para=2") */
            if (!semi && in_attr && e < n && (s[e] == '=' || is_alpha_c(s[e]) || is_digit_c(s[e]))) return -1;
            *used = e + (semi ? 1 : 0);
            return g_ent[mid].cp;
        }
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

/* A no-break space in page text. Folding it to ' ' would let it collapse with
   neighbouring whitespace and let lines break at it; the glyph of 0x1F is blank
   and 8px wide like a space, and only ' ' collapses or breaks. */
#define NBSP_CH '\x1f'
static int g_keep_nbsp;

/* Decode UTF-8 (and entities when asked) from s into folded ASCII. Invalid
   UTF-8 bytes are taken as Latin-1. Returns the output length (< cap). */
static int text_fold(const char *s, int n, char *out, int cap, int entities) {
    int w = 0, i = 0;
    cap -= 1;
    while (i < n && w < cap) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            if (c == '&' && entities) {
                int used = 1;
                int cp = entity_cp(s + i, n - i, &used, entities == 2);
                if (cp >= 0) {
                    if (cp == 160 && g_keep_nbsp) out[w++] = NBSP_CH; else w = fold_cp(cp, out, w, cap);
                    i += used;
                    continue;
                }
            }
            w = fold_cp(c, out, w, cap);
            i++;
            continue;
        }
        int cp = c, len = 1;
        if ((c & 0xE0) == 0xC0 && i + 1 < n && (s[i + 1] & 0xC0) == 0x80) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); len = 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < n && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80) {
            cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); len = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < n && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80 &&
                   (s[i + 3] & 0xC0) == 0x80) {
            cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); len = 4;
        }
        if (cp == 160 && g_keep_nbsp) out[w++] = NBSP_CH; else w = fold_cp(cp, out, w, cap);
        i += len;
    }
    out[w] = 0;
    return w;
}

/* ---- text nodes ------------------------------------------------------------ */

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
        /* a leading space separates this text from the inline before it */
        if (sp && (w > 0 || g_web) && w < cap) out[w++] = ' ';
        sp = 0;
        if (w < cap) out[w++] = c;
    }
    if (w == 0 && n > 0 && cap > 0) out[w++] = ' ';
    else if (sp && w > 0 && w < cap) out[w++] = ' ';
    return w;
}

static int is_pre_tag(int tag) {
    if (g_web) return tag == RB_TAG_PRE || tag == RB_TAG_XMP || tag == RB_TAG_LISTING ||
                      tag == RB_TAG_PLAINTEXT || tag == RB_TAG_TEXTAREA;
    return tag == RB_TAG_PRE || tag == RB_TAG_CODE;
}

static int ancestor_pre(int node) {
    while (node) {
        if (is_pre_tag(g_nodes[node].tag)) return 1;
        node = g_nodes[node].parent;
    }
    return 0;
}

/* Parents whose whitespace-only text is dropped (it would only add nodes). */
static int no_ws_parent(int tag) {
    switch (tag) {
    case RB_TAG_DOC: case RB_TAG_HTML: case RB_TAG_HEAD: case RB_TAG_TABLE: case RB_TAG_TBODY:
    case RB_TAG_THEAD: case RB_TAG_TFOOT: case RB_TAG_TR: case RB_TAG_UL: case RB_TAG_OL:
    case RB_TAG_DL: case RB_TAG_SELECT: case RB_TAG_COLGROUP: case RB_TAG_OPTGROUP:
    case RB_TAG_DATALIST: case RB_TAG_MENU: case RB_TAG_DIR: case RB_TAG_FRAMESET:
        return 1;
    default:
        return 0;
    }
}

static void add_text_node(int parent, const char *s, int n, int preserve) {
    if (!parent || n <= 0) return;
    int cn = collapse_text(s, n, g_fold, RAW_MAX, preserve);
    if (cn <= 0) return;
    if (!preserve && cn == 1 && g_fold[0] == ' ' && no_ws_parent(g_nodes[parent].tag)) return;
    int off = pool_add(g_fold, cn);
    if (off < 0) return;
    int t = new_node();
    if (!t) { g_stop = 1; return; }
    g_nodes[t].kind = 1;
    g_nodes[t].preserve = (uint8_t)(preserve ? 1 : 0);
    g_nodes[t].text_off = (uint32_t)off;
    g_nodes[t].text_len = (uint32_t)cn;
    add_child(parent, t);
}

/* ---- attributes -------------------------------------------------------------- */

static void store_attr(uint32_t *off, uint32_t *len, const char *s, int n) {
    int o = pool_add(s, n);
    if (o < 0) return;
    *off = (uint32_t)o;
    *len = (uint32_t)n;
}

static int decode_attr(const char *s, int n, char *out, int cap) {
    if (!s || n < 0) n = 0;
    if (!out || cap < 1) return 0;
    return text_fold(s, n, out, cap, 2);
}

static int takes_type_attr(int tag);
static void apply_type(Node *n, const char *val, int vn);

/* Attributes kept in the node's blob for selectors and presentation hints. */
static int attr_keep(const char *name, int nn) {
    if (g_scripting) return nn > 0;     /* a script can read any attribute back */
    static const char *const keep[] = {
        "type", "role", "rel", "target", "src", "for", "width", "height", "align", "valign",
        "bgcolor", "text", "color", "face", "size", "border", "cellpadding", "cellspacing",
        "colspan", "rowspan", "nowrap", "placeholder", "required", "readonly", "selected",
        "multiple", "rows", "cols", "maxlength", "min", "max", "media", "content", "open",
        "start", "label", "span", "scope", "hspace", "vspace", "marginwidth", "marginheight",
        "leftmargin", "topmargin", "title", "lang", "charset", "http-equiv", "as", "value",
        "background", "link", "vlink", "alink", "srcset", "aria-hidden", "aria-expanded", "aria-label", "tabindex", "dir", "inert", 0 };
    for (int i = 0; keep[i]; i++) if (eq_lit(name, nn, keep[i])) return 1;
    /* data-* hooks are what scripts and stylesheets key on; short ones only */
    return nn > 5 && eq_lit(name, 5, "data-");
}

static char g_ablob[8192];
static int g_ablob_n;

static void blob_add(const char *name, int nn, const char *val, int vn) {
    int vmax = g_scripting ? 4000 : (nn > 5 && eq_lit(name, 5, "data-") ? 48 : 300);
    if (nn <= 0 || nn > (g_scripting ? 64 : 32) || vn > vmax) return;
    if (g_ablob_n + nn + vn + 2 >= (int)sizeof(g_ablob)) return;
    for (int i = 0; i < nn; i++) g_ablob[g_ablob_n++] = (char)lower_ch((unsigned char)name[i]);
    g_ablob[g_ablob_n++] = 0;
    memcpy(g_ablob + g_ablob_n, val, (size_t)vn);
    g_ablob_n += vn;
    g_ablob[g_ablob_n++] = 0;
}

static void apply_attr(int node, const char *name, int nn, const char *val, int vn) {
    /* Attribute values are entity-decoded in HTML, not just href. */
    int decoded = decode_attr(val, vn, g_fold, RAW_MAX);
    val = g_fold;
    vn = decoded;
    if (eq_lit(name, nn, "id")) store_attr(&g_nodes[node].id_off, &g_nodes[node].id_len, val, vn);
    else if (eq_lit(name, nn, "class")) store_attr(&g_nodes[node].class_off, &g_nodes[node].class_len, val, vn);
    else if (eq_lit(name, nn, "href")) store_attr(&g_nodes[node].href_off, &g_nodes[node].href_len, val, vn);
    else if (eq_lit(name, nn, "style")) store_attr(&g_nodes[node].style_off, &g_nodes[node].style_len, val, vn);
    else if (eq_lit(name, nn, "onclick")) store_attr(&g_nodes[node].click_off, &g_nodes[node].click_len, val, vn);
    else if (eq_lit(name, nn, "name")) store_attr(&g_nodes[node].name_off, &g_nodes[node].name_len, val, vn);
    else if (eq_lit(name, nn, "action")) store_attr(&g_nodes[node].action_off, &g_nodes[node].action_len, val, vn);
    else if (eq_lit(name, nn, "method")) {
        /* Only GET is supported. Flag POST (case-insensitive) so activation
           refuses the form instead of silently converting it to GET. */
        g_nodes[node].method_post = (uint8_t)(vn == 4 &&
            (val[0] | 32) == 'p' && (val[1] | 32) == 'o' &&
            (val[2] | 32) == 's' && (val[3] | 32) == 't');
        blob_add(name, nn, val, vn);
    }
    else if (eq_lit(name, nn, "value") || eq_lit(name, nn, "alt"))
        store_attr(&g_nodes[node].text_off, &g_nodes[node].text_len, val, vn);
    else if (eq_lit(name, nn, "hidden")) g_nodes[node].hidden = 1;
    else if (eq_lit(name, nn, "disabled")) g_nodes[node].disabled = 1;
    else if (eq_lit(name, nn, "checked")) g_nodes[node].checked = 1;
    else if (eq_lit(name, nn, "type")) {
        if (takes_type_attr(g_nodes[node].tag)) apply_type(&g_nodes[node], val, vn);
        blob_add(name, nn, val, vn);
    }
    else if (attr_keep(name, nn)) blob_add(name, nn, val, vn);
}

static void parse_attrs(int node, const char *tag, int n) {
    int i = 0;
    g_ablob_n = 0;
    while (i < n && tag[i] != ' ' && tag[i] != '\t' && tag[i] != '\n' && tag[i] != '\r' && tag[i] != '/' && tag[i] != '>') i++;
    while (i < n) {
        while (i < n && (tag[i] == ' ' || tag[i] == '\t' || tag[i] == '\n' || tag[i] == '\r' || tag[i] == '/')) i++;
        if (i >= n || tag[i] == '>') break;
        int ns = i;
        while (i < n && tag[i] != '=' && tag[i] != ' ' && tag[i] != '\t' && tag[i] != '\n' && tag[i] != '\r' &&
               tag[i] != '/' && tag[i] != '>') i++;
        int nn = i - ns;
        while (i < n && (tag[i] == ' ' || tag[i] == '\t' || tag[i] == '\n' || tag[i] == '\r')) i++;
        if (i < n && tag[i] == '=') {
            i++;
            while (i < n && (tag[i] == ' ' || tag[i] == '\t' || tag[i] == '\n' || tag[i] == '\r')) i++;
            if (i < n && (tag[i] == '"' || tag[i] == '\'')) {
                char q = tag[i++];
                int vs = i;
                while (i < n && tag[i] != q) i++;
                apply_attr(node, tag + ns, nn, tag + vs, i - vs);
                if (i < n) i++;
            } else {
                /* Unquoted value: HTML keeps '/' in the value (href=/x). */
                int vs = i;
                while (i < n && tag[i] != ' ' && tag[i] != '\t' && tag[i] != '\n' && tag[i] != '\r' && tag[i] != '>') {
                    if (tag[i] == '/' && (i + 1 >= n || tag[i + 1] == '>')) break;
                    i++;
                }
                apply_attr(node, tag + ns, nn, tag + vs, i - vs);
            }
        } else if (nn > 0) {
            apply_attr(node, tag + ns, nn, "", 0);
        }
    }
    if (g_ablob_n > 0) {
        int off = pool_add(g_ablob, g_ablob_n);
        if (off >= 0) { g_nodes[node].attr_off = (uint32_t)off; g_nodes[node].attr_len = (uint32_t)g_ablob_n; }
    }
}

/* ---- tree builder ------------------------------------------------------------- */

#define STK_MAX 480
static int g_stk[STK_MAX];
static int g_sp;

static int tag_is_void(int tag) {
    switch (tag) {
    case RB_TAG_BR: case RB_TAG_HR: case RB_TAG_IMG: case RB_TAG_INPUT: case RB_TAG_AREA:
    case RB_TAG_BASE: case RB_TAG_COL: case RB_TAG_EMBED: case RB_TAG_LINK: case RB_TAG_META:
    case RB_TAG_PARAM: case RB_TAG_SOURCE: case RB_TAG_TRACK: case RB_TAG_WBR: case RB_TAG_SPACER:
    case RB_TAG_FRAME:
        return 1;
    default:
        return 0;
    }
}

/* Raw-text handling: 0 = ordinary element; 1 = text kept (title, textarea);
   2 = content skipped (noscript, svg, ...); 3 = script; 4 = style. */
static int raw_kind(int tag) {
    switch (tag) {
    case RB_TAG_TITLE: case RB_TAG_TEXTAREA: return 1;
    case RB_TAG_SCRIPT: return 3;
    case RB_TAG_STYLE: return 4;
    case RB_TAG_NOSCRIPT: case RB_TAG_SVG: case RB_TAG_MATH: case RB_TAG_CANVAS: case RB_TAG_VIDEO:
    case RB_TAG_AUDIO: case RB_TAG_TEMPLATE: case RB_TAG_IFRAME: case RB_TAG_NOFRAMES:
        return 2;
    default: return 0;
    }
}

static int is_special_tag(int tag) {
    switch (tag) {
    case RB_TAG_ADDRESS: case RB_TAG_APPLET: case RB_TAG_AREA: case RB_TAG_ARTICLE: case RB_TAG_ASIDE:
    case RB_TAG_BASE: case RB_TAG_BODY: case RB_TAG_BLOCKQUOTE: case RB_TAG_BR: case RB_TAG_BUTTON:
    case RB_TAG_CAPTION: case RB_TAG_CENTER: case RB_TAG_COL: case RB_TAG_COLGROUP: case RB_TAG_DD:
    case RB_TAG_DETAILS: case RB_TAG_DIR: case RB_TAG_DL: case RB_TAG_DT: case RB_TAG_EMBED:
    case RB_TAG_FIELDSET: case RB_TAG_FIGCAPTION: case RB_TAG_FIGURE: case RB_TAG_FOOTER:
    case RB_TAG_FORM: case RB_TAG_FRAME: case RB_TAG_FRAMESET: case RB_TAG_H1: case RB_TAG_H2:
    case RB_TAG_H3: case RB_TAG_H4: case RB_TAG_H5: case RB_TAG_H6: case RB_TAG_HEAD:
    case RB_TAG_HEADER: case RB_TAG_HGROUP: case RB_TAG_HR: case RB_TAG_HTML: case RB_TAG_IFRAME:
    case RB_TAG_IMG: case RB_TAG_INPUT: case RB_TAG_LI: case RB_TAG_LINK: case RB_TAG_LISTING:
    case RB_TAG_MAIN: case RB_TAG_MARQUEE: case RB_TAG_MENU: case RB_TAG_META: case RB_TAG_NAV:
    case RB_TAG_NOFRAMES: case RB_TAG_NOSCRIPT: case RB_TAG_OBJECT: case RB_TAG_OL: case RB_TAG_P:
    case RB_TAG_PARAM: case RB_TAG_PLAINTEXT: case RB_TAG_PRE: case RB_TAG_SCRIPT: case RB_TAG_SEARCH:
    case RB_TAG_SECTION: case RB_TAG_SELECT: case RB_TAG_SOURCE: case RB_TAG_STYLE: case RB_TAG_SUMMARY:
    case RB_TAG_TABLE: case RB_TAG_TBODY: case RB_TAG_TD: case RB_TAG_TEMPLATE: case RB_TAG_TEXTAREA:
    case RB_TAG_TFOOT: case RB_TAG_TH: case RB_TAG_THEAD: case RB_TAG_TITLE: case RB_TAG_TR:
    case RB_TAG_TRACK: case RB_TAG_UL: case RB_TAG_WBR: case RB_TAG_XMP:
        return 1;
    default: return 0;
    }
}

static int scope_barrier(int tag) {
    switch (tag) {
    case RB_TAG_TABLE: case RB_TAG_TD: case RB_TAG_TH: case RB_TAG_CAPTION: case RB_TAG_HTML:
    case RB_TAG_OBJECT: case RB_TAG_APPLET: case RB_TAG_MARQUEE: case RB_TAG_TEMPLATE: case RB_TAG_DOC:
        return 1;
    default: return 0;
    }
}


static int stk_matches(int idx, int tag, uint16_t hash) {
    int nd = g_stk[idx];
    if (g_nodes[nd].tag != tag) return 0;
    return tag != RB_TAG_OTHER || g_nodes[nd].name_hash == hash;
}

/* Index of the nearest open element `tag` before a scope barrier, or -1.
   `stop_button` also treats <button> as a barrier (the <p> rule). */
static int stk_find_scoped(int tag, uint16_t hash, int stop_button) {
    for (int i = g_sp; i > 0; i--) {
        if (stk_matches(i, tag, hash)) return i;
        int t = g_nodes[g_stk[i]].tag;
        if (scope_barrier(t) || (stop_button && t == RB_TAG_BUTTON)) return -1;
    }
    return -1;
}

static void stk_pop_to(int idx) {
    if (idx < 1) idx = 1;
    if (idx <= g_sp) g_sp = idx - 1;
}

static void close_p(void) {
    int i = stk_find_scoped(RB_TAG_P, 0, 1);
    if (i > 0) stk_pop_to(i);
}

static int closes_p(int tag) {
    switch (tag) {
    case RB_TAG_ADDRESS: case RB_TAG_ARTICLE: case RB_TAG_ASIDE: case RB_TAG_BLOCKQUOTE:
    case RB_TAG_CENTER: case RB_TAG_DETAILS: case RB_TAG_DIALOG: case RB_TAG_DIR: case RB_TAG_DIV:
    case RB_TAG_DL: case RB_TAG_FIELDSET: case RB_TAG_FIGCAPTION: case RB_TAG_FIGURE:
    case RB_TAG_FOOTER: case RB_TAG_FORM: case RB_TAG_H1: case RB_TAG_H2: case RB_TAG_H3:
    case RB_TAG_H4: case RB_TAG_H5: case RB_TAG_H6: case RB_TAG_HEADER: case RB_TAG_HGROUP:
    case RB_TAG_HR: case RB_TAG_MAIN: case RB_TAG_MENU: case RB_TAG_NAV: case RB_TAG_OL:
    case RB_TAG_P: case RB_TAG_PRE: case RB_TAG_SEARCH: case RB_TAG_SECTION: case RB_TAG_SUMMARY:
    case RB_TAG_TABLE: case RB_TAG_UL: case RB_TAG_LISTING: case RB_TAG_XMP: case RB_TAG_LI:
    case RB_TAG_DD: case RB_TAG_DT: case RB_TAG_PLAINTEXT:
        return 1;
    default: return 0;
    }
}

static int head_allowed(int tag) {
    return tag == RB_TAG_TITLE || tag == RB_TAG_STYLE || tag == RB_TAG_SCRIPT || tag == RB_TAG_META ||
           tag == RB_TAG_LINK || tag == RB_TAG_BASE || tag == RB_TAG_NOSCRIPT || tag == RB_TAG_TEMPLATE ||
           tag == RB_TAG_HEAD || tag == RB_TAG_HTML;
}

/* Stylesheets named by <link rel=stylesheet>, resolved by the host. */
#define MAX_PENDING_CSS 32
typedef struct { uint32_t href_off; uint16_t href_len; uint16_t ord; uint32_t next_idx; uint8_t done; uint8_t interned; int8_t parent; } PendCss;
static PendCss g_pcss[MAX_PENDING_CSS];
static int g_pcss_n;

/* Move the @imports found since `mark` into the pending list. They sit just
   before their importer in the cascade (importer ordinal - 8 + k), so the
   importing sheet's own rules still win ties, and each resolves against its
   importer's URL (parent index; -1 is the page). */
static void queue_imports(int mark, int parent, int importer_ord) {
    int k = 0;
    for (int m = mark; m < g_import_n && g_pcss_n < MAX_PENDING_CSS && k < 8; m++, k++) {
        PendCss *np = &g_pcss[g_pcss_n++];
        np->href_off = g_imports[m].off;
        np->href_len = g_imports[m].len;
        np->interned = 1;
        np->ord = (uint16_t)(importer_ord - 8 + k);
        np->next_idx = 0;
        np->done = 0;
        np->parent = (int8_t)parent;
    }
    g_import_n = mark;
}

static void note_stylesheet_link(int node) {
    const char *v; int vn;
    if (!attr_get(node, "rel", 3, &v, &vn)) return;
    if (!token_in_list(v, vn, "stylesheet", 10, 1)) return;
    if (token_in_list(v, vn, "alternate", 9, 1)) return;
    if (attr_get(node, "media", 5, &v, &vn) && !media_matches(v, vn)) return;
    if (!g_nodes[node].href_len || g_pcss_n >= MAX_PENDING_CSS) return;
    g_pcss[g_pcss_n].href_off = g_nodes[node].href_off;
    g_pcss[g_pcss_n].href_len = (uint16_t)g_nodes[node].href_len;
    g_pcss[g_pcss_n].parent = -1;
    g_pcss[g_pcss_n].ord = (uint16_t)g_sheet_ord;
    g_sheet_ord += SHEET_STEP;
    g_pcss[g_pcss_n].next_idx = 0;
    g_pcss[g_pcss_n].interned = 0;
    g_pcss[g_pcss_n].done = 0;
    g_pcss_n++;
}

static int g_el_pushed;      /* start_element: the new element is on the open-element stack */

static void start_element(int tag, uint16_t hash, const char *html, int ts, int te, int self) {
    int cur;
    g_el_pushed = 0;
    g_last_el = 0;
    /* a fragment never re-opens the document's own html/head/body */
    if (g_frag && (tag == RB_TAG_HTML || tag == RB_TAG_HEAD || tag == RB_TAG_BODY)) return;
    if (tag == RB_TAG_HTML || tag == RB_TAG_BODY) {
        for (int i = 1; i <= g_sp; i++) if (g_nodes[g_stk[i]].tag == tag) return;
    }
    /* anything but head content ends an open <head> */
    while (g_sp > 0 && g_nodes[g_stk[g_sp]].tag == RB_TAG_HEAD && !head_allowed(tag)) g_sp--;
    if (closes_p(tag) && tag != RB_TAG_LI && tag != RB_TAG_DD && tag != RB_TAG_DT) close_p();
    switch (tag) {
    case RB_TAG_H1: case RB_TAG_H2: case RB_TAG_H3: case RB_TAG_H4: case RB_TAG_H5: case RB_TAG_H6: {
        int t = g_nodes[g_stk[g_sp]].tag;
        if (t >= RB_TAG_H1 && t <= RB_TAG_H6) g_sp--;
        break;
    }
    case RB_TAG_LI: case RB_TAG_DD: case RB_TAG_DT: {
        for (int i = g_sp; i > 0; i--) {
            int t = g_nodes[g_stk[i]].tag;
            int same = tag == RB_TAG_LI ? t == RB_TAG_LI : (t == RB_TAG_DD || t == RB_TAG_DT);
            if (same) { stk_pop_to(i); break; }
            if (is_special_tag(t) && t != RB_TAG_ADDRESS && t != RB_TAG_DIV && t != RB_TAG_P) break;
        }
        close_p();
        break;
    }
    case RB_TAG_TR: {
        for (int i = g_sp; i > 0; i--) {
            int t = g_nodes[g_stk[i]].tag;
            if (t == RB_TAG_TR) { stk_pop_to(i); break; }
            if (t == RB_TAG_TABLE || t == RB_TAG_TBODY || t == RB_TAG_THEAD || t == RB_TAG_TFOOT) break;
        }
        break;
    }
    case RB_TAG_TD: case RB_TAG_TH: {
        for (int i = g_sp; i > 0; i--) {
            int t = g_nodes[g_stk[i]].tag;
            if (t == RB_TAG_TD || t == RB_TAG_TH) { stk_pop_to(i); break; }
            if (t == RB_TAG_TR || t == RB_TAG_TABLE) break;
        }
        break;
    }
    case RB_TAG_THEAD: case RB_TAG_TBODY: case RB_TAG_TFOOT: {
        for (int i = g_sp; i > 0; i--) {
            int t = g_nodes[g_stk[i]].tag;
            if (t == RB_TAG_THEAD || t == RB_TAG_TBODY || t == RB_TAG_TFOOT) { stk_pop_to(i); break; }
            if (t == RB_TAG_TABLE) break;
        }
        break;
    }
    case RB_TAG_OPTION: case RB_TAG_OPTGROUP: {
        int t = g_nodes[g_stk[g_sp]].tag;
        if (t == RB_TAG_OPTION) g_sp--;
        if (tag == RB_TAG_OPTGROUP && g_nodes[g_stk[g_sp]].tag == RB_TAG_OPTGROUP) g_sp--;
        break;
    }
    case RB_TAG_A: {
        int i = stk_find_scoped(RB_TAG_A, 0, 0);
        if (i > 0) stk_pop_to(i);
        break;
    }
    case RB_TAG_BUTTON: {
        int i = stk_find_scoped(RB_TAG_BUTTON, 0, 0);
        if (i > 0) stk_pop_to(i);
        break;
    }
    default: break;
    }
    cur = g_stk[g_sp];
    int node = new_node();
    if (!node) { g_stop = 1; return; }
    g_nodes[node].tag = (uint8_t)tag;
    g_nodes[node].name_hash = hash;
    if (tag == RB_TAG_OTHER) g_nodes[node].tname = (uint8_t)tname_intern(html + ts, g_tag_nn);
    parse_attrs(node, html + ts, te - ts);
    add_child(cur, node);
    g_last_el = node;
    if (tag == RB_TAG_LINK && !g_frag) note_stylesheet_link(node);
    if (!self && !tag_is_void(tag) && g_sp + 1 < STK_MAX) { g_stk[++g_sp] = node; g_el_pushed = 1; }
    else if (self && !tag_is_void(tag) && tag != RB_TAG_OTHER) {
        /* <div/> is an open tag in HTML, but <br/>-style slashes on non-void
           elements are common XHTML leftovers: treat as empty. */
    }
}

static void end_element(int tag, uint16_t hash) {
    if (tag == RB_TAG_BR) { return; }
    if (g_frag && (tag == RB_TAG_HTML || tag == RB_TAG_HEAD || tag == RB_TAG_BODY)) return;
    if (tag == RB_TAG_P) {
        int i = stk_find_scoped(RB_TAG_P, 0, 1);
        if (i > 0) stk_pop_to(i);
        return;
    }
    int idx = -1;
    int barrier = 0;
    int is_block_end = (tag == RB_TAG_TD || tag == RB_TAG_TH || tag == RB_TAG_TR || tag == RB_TAG_TABLE ||
                        tag == RB_TAG_TBODY || tag == RB_TAG_THEAD || tag == RB_TAG_TFOOT || tag == RB_TAG_CAPTION);
    for (int i = g_sp; i > 0; i--) {
        if (stk_matches(i, tag, hash)) { idx = i; break; }
        int t = g_nodes[g_stk[i]].tag;
        if (!is_block_end && scope_barrier(t)) { barrier = 1; break; }
        if (is_block_end && t == RB_TAG_TABLE && tag != RB_TAG_TABLE) { barrier = 1; break; }
    }
    if (idx < 0 || barrier) return;
    stk_pop_to(idx);
}

/* A raw-text element's content runs to its matching close tag. */
static int find_close(const char *html, int len, int i, const char *name, int nn) {
    while (i < len) {
        if (html[i] == '<' && i + 1 < len && html[i + 1] == '/') {
            int j = i + 2, k = 0;
            while (k < nn && j < len && lower_ch((unsigned char)html[j]) == name[k]) { j++; k++; }
            if (k == nn && (j >= len || !(is_alpha_c(html[j]) || is_digit_c(html[j]) || html[j] == '-'))) return i;
        }
        i++;
    }
    return len;
}

static const char *tag_name_of(int tag, uint16_t hash) {
    (void)hash;
    for (int i = 0; i < (int)(sizeof(g_tagnames) / sizeof(g_tagnames[0])); i++)
        if (g_tagnames[i].tag == tag) return g_tagnames[i].name;
    return "";
}

static void flush_text(const char *html, int a, int b) {
    int cur = g_stk[g_sp];
    int pre = ancestor_pre(cur);
    while (a < b) {
        int chunk = b - a;
        if (chunk > RAW_MAX / 2) chunk = RAW_MAX / 2;
        /* do not split inside an entity or a UTF-8 sequence */
        if (a + chunk < b) {
            int back = 0;      /* a few bytes at most: never walk a long run of '&' */
            while (chunk > 1 && back < 8 && (html[a + chunk - 1] == '&' || ((unsigned char)html[a + chunk] & 0xC0) == 0x80)) { chunk--; back++; }
        }
        if (g_stop) return;
        g_keep_nbsp = g_web;
        int rn = text_fold(html + a, chunk, g_raw, RAW_MAX, 1);
        g_keep_nbsp = 0;
        add_text_node(cur, g_raw, rn, pre);
        a += chunk;
    }
}

static void parse_loop(const char *html, int len) {
    int i = 0;
    while (i < len && !g_stop) {
        if (html[i] != '<') {
            int a = i;
            while (i < len && html[i] != '<') i++;
            /* text outside <head> content ends an open head */
            if (g_sp > 0 && g_nodes[g_stk[g_sp]].tag == RB_TAG_HEAD) {
                int all_ws = 1;
                for (int k = a; k < i; k++) if (html[k] != ' ' && html[k] != '\t' && html[k] != '\n' && html[k] != '\r') { all_ws = 0; break; }
                if (all_ws) continue;
                g_sp--;
            }
            flush_text(html, a, i);
            continue;
        }
        /* '<' not followed by a tag start is text */
        if (i + 1 >= len) { flush_text(html, i, len); break; }
        char c1 = html[i + 1];
        if (c1 == '!') {
            if (i + 3 < len && html[i + 2] == '-' && html[i + 3] == '-') {
                i += 4;
                while (i + 2 < len && !(html[i] == '-' && html[i + 1] == '-' && html[i + 2] == '>')) i++;
                i = i + 2 < len ? i + 3 : len;
                continue;
            }
            if (i + 8 < len && memcmp(html + i, "<![CDATA[", 9) == 0) {
                i += 9;
                int a = i;
                while (i + 2 < len && !(html[i] == ']' && html[i + 1] == ']' && html[i + 2] == '>')) i++;
                flush_text(html, a, i);
                i = i + 2 < len ? i + 3 : len;
                continue;
            }
            if (i + 9 < len && starts_lit(html + i + 2, len - i - 2, "doctype") && !g_frag) g_quirks = 0;
            while (i < len && html[i] != '>') i++;
            if (i < len) i++;
            continue;
        }
        if (c1 == '?') {
            while (i < len && html[i] != '>') i++;
            if (i < len) i++;
            continue;
        }
        if (c1 == '/') {
            int j = i + 2;
            while (j < len && (html[j] == ' ' || html[j] == '\t' || html[j] == '\n')) j++;
            int ns = j;
            while (j < len && html[j] != '>' && html[j] != ' ' && html[j] != '\t' && html[j] != '\n' && html[j] != '/') j++;
            if (j == ns || !is_alpha_c(html[ns])) {      /* "</ >" and friends: skip */
                while (i < len && html[i] != '>') i++;
                if (i < len) i++;
                continue;
            }
            int tag = tag_of(html + ns, j - ns);
            uint16_t hash = tag == RB_TAG_OTHER ? name_hash(html + ns, j - ns) : 0;
            end_element(tag, hash);
            while (i < len && html[i] != '>') i++;
            if (i < len) i++;
            continue;
        }
        if (!is_alpha_c(c1)) {                           /* a bare '<' in text */
            int a = i;
            i++;
            while (i < len && html[i] != '<') i++;
            flush_text(html, a, i);
            continue;
        }
        int ts = i + 1;
        int te = ts;
        int inq = 0;
        char q = 0;
        while (te < len) {
            char c = html[te];
            if (inq) { if (c == q) inq = 0; te++; continue; }
            if (c == '"' || c == '\'') {
                /* a quote only opens a value after '=' */
                int b = te - 1;
                while (b > ts && (html[b] == ' ' || html[b] == '\t' || html[b] == '\n')) b--;
                if (html[b] == '=') { inq = 1; q = c; }
                te++;
                continue;
            }
            if (c == '>') break;
            te++;
        }
        int name_n = 0;
        while (ts + name_n < te && html[ts + name_n] != ' ' && html[ts + name_n] != '\t' &&
               html[ts + name_n] != '\n' && html[ts + name_n] != '\r' && html[ts + name_n] != '/' && html[ts + name_n] != '>')
            name_n++;
        int self = 0;
        int slash = te - 1;
        while (slash > ts && (html[slash] == ' ' || html[slash] == '\t')) slash--;
        if (slash > ts && html[slash] == '/') self = 1;
        int tag = tag_of(html + ts, name_n);
        uint16_t hash = tag == RB_TAG_OTHER ? name_hash(html + ts, name_n) : 0;
        g_tag_nn = name_n;
        i = te < len ? te + 1 : len;
        int rk = raw_kind(tag);
        if (rk && !self) {
            /* element with raw content */
            const char *nm = tag_name_of(tag, hash);
            int nn = (int)strlen(nm);
            int close = find_close(html, len, i, nm, nn);
            start_element(tag, hash, html, ts, te, 0);
            if (rk == 3 && g_scripting && !g_frag) script_note(g_last_el, html + i, close - i);
            if (g_el_pushed) {
                int node = g_stk[g_sp];
                int cn = close - i;
                if (rk == 1) {
                    int rn = text_fold(html + i, cn > RAW_MAX - 1 ? RAW_MAX - 1 : cn, g_raw, RAW_MAX, tag == RB_TAG_TEXTAREA || tag == RB_TAG_TITLE);
                    if (tag == RB_TAG_TEXTAREA) {
                        /* a leading newline right after <textarea> is dropped */
                        int sk = rn > 0 && g_raw[0] == '\n' ? 1 : 0;
                        int off = pool_add(g_raw + sk, rn - sk);
                        if (off >= 0) { g_nodes[node].text_off = (uint32_t)off; g_nodes[node].text_len = (uint32_t)(rn - sk); }
                    } else if (!g_frag) {
                        int tn = collapse_text(g_raw, rn, g_title, (int)sizeof(g_title) - 1, 0);
                        while (tn > 0 && g_title[tn - 1] == ' ') tn--;
                        int st = 0;
                        while (st < tn && g_title[st] == ' ') st++;
                        if (st) memmove(g_title, g_title + st, (size_t)(tn - st));
                        g_title[tn - st] = 0;
                    }
                } else if (rk == 3) {
                    if (!g_scripting && !g_frag && cn > 0 && cn <= SCRIPT_KEEP_MAX) {
                        int rn = text_fold(html + i, cn, g_raw, RAW_MAX, 0);
                        run_script(g_raw, rn);
                    }
                } else if (rk == 4 && !g_frag) {
                    const char *mv; int mvn;
                    int screen = !(attr_get(node, "media", 5, &mv, &mvn) && !media_matches(mv, mvn));
                    int rn = cn < RAW_MAX - 1 ? cn : RAW_MAX - 1;
                    if (!screen) rn = 0;
                    /* style text is ASCII-folded without entity decoding */
                    int fn = text_fold(html + i, rn, g_raw, RAW_MAX, 0);
                    if (rn > 0) css_add_sheet(g_raw, fn);
                }
                g_sp--;
            }
            i = close;
            while (i < len && html[i] != '>') i++;
            if (i < len) i++;
            continue;
        }
        start_element(tag, hash, html, ts, te, self);
        if (tag == RB_TAG_SCRIPT && g_scripting && !g_frag) script_note(g_last_el, html + i, 0);
    }
}

static void parse_html(const char *html, int len) {
    g_sp = 0;
    g_stk[0] = 1;
    g_pcss_n = 0;
    g_quirks = 1;             /* until a <!doctype> says otherwise */
    g_frag = 0;
    g_pool_busy++;
    parse_loop(html, len);
    g_pool_busy--;
    g_sp = 0;
}

/* Parse markup as the children of `root` (a detached container): the tree
   builder is the document's, minus everything that is document-wide. */
static void parse_fragment(const char *html, int len, int root) {
    g_sp = 0;
    g_stk[0] = root;
    g_frag = 1;
    g_pool_busy++;
    parse_loop(html, len);
    g_pool_busy--;
    g_frag = 0;
    g_sp = 0;
}
