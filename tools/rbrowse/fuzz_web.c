/*
 * Host-side fuzzer for the rbrowse WEB profile (rb_open_web): real HTML and CSS
 * pulled apart and glued back together, under ASan + UBSan.
 *
 *   clang -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
 *       -DRBROWSE_HOST -I src/rbrowse tools/rbrowse/fuzz_web.c src/rbrowse/engine.c \
 *       -o /tmp/rbfuzz_web
 *   /tmp/rbfuzz_web [iterations] [seed] [seed.html ...]
 *
 * With -DFUZZ_PAINT (add src/rbrowse/paint.c and the font, built as in
 * test_paint.c) every page is also painted at a few scroll offsets, at
 * 640x360 or 320x200, so ASan watches the painter's clipping too; image
 * boxes get stand-in pixel views of the box size or of another size.
 *
 * Most iterations run in image mode: every <img> gets a random outcome
 * (decoded with any natural size up to 65535, broken, no source, pending),
 * as rbrowse.c's loader would store it, followed by a relayout.
 *
 * Without seed files it builds pages from the token tables below. With seed
 * files it mutates them (byte flips, chunk copies, token splices). Every
 * iteration also feeds a stylesheet through rb_css_provide when the page names
 * one, relayouts, and checks the engine's invariants: box geometry stays inside
 * int16 range, the tables never overrun, and the layout work stays bounded.
 */
#include "engine.h"
#ifdef FUZZ_PAINT
#include "paint.h"
static uint32_t g_fb[RB_FB_MAX_W * RB_FB_MAX_H];
static uint8_t g_fb8[RB_FB_MAX_W * RB_FB_MAX_H];
/* A stand-in for image.c: views of a few sizes (exact, smaller, larger than
   the box) so both the 1:1 blit and the nearest-neighbour path run. */
static uint32_t g_imgpx[64 * 64];
static int fake_images(int node, int w, int h, RbImageView *out) {
    int k = (node + w + h) % 4;
    if (k == 0) return 0;
    out->px = g_imgpx;
    out->alpha = k == 1;
    if (k == 2 && w <= 64 && h <= 64) { out->w = w; out->h = h; out->stride = w; }
    else { out->w = 1 + (node % 64); out->h = 1 + (w % 64); out->stride = out->w; }
    return 1;
}
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng = 0x2545F4914F6CDD1DULL;

static unsigned rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (unsigned)(rng >> 16);
}

static const char *html_toks[] = {
    "<div>", "</div>", "<span>", "</span>", "<p>", "</p>", "<a href=\"/x\">", "</a>", "<ul>", "<li>",
    "</li>", "</ul>", "<ol start=3>", "</ol>", "<table>", "</table>", "<tr>", "</tr>", "<td>", "</td>",
    "<th colspan=2>", "<td rowspan=3>", "<td colspan=99>", "<thead>", "<tbody>", "<tfoot>", "<caption>",
    "<table border=1 cellpadding=3 cellspacing=2 width=80%>", "<td width=50%>", "<td valign=bottom>",
    "<img src=x width=40 height=30 alt=pic>", "<img width=100000 height=100000>", "<svg width=10>", "</svg>",
    "<img src=a.png>", "<img src=b.jpg style=\"max-width:100%;height:auto\">", "<img src=c.gif width=50%>",
    "<img src=d.png style=\"height:3em;min-width:400px\">", "<img srcset=\"e.png 2x\" alt=\"long alternative text here\">",
    "<picture><source srcset=f.webp type=image/webp><img src=f.jpg></picture>", "<img src=g.png style=\"display:block;margin:auto\">",
    "<img src=h.png style=\"float:left;max-height:20px\">", "<a href=/i><img src=i.png width=0 height=9999></a>",
    "<iframe src=x>", "</iframe>", "<select><option>one<option selected>two</select>", "<input>",
    "<input type=checkbox checked>", "<button>go</button>", "<textarea>t</textarea>", "<form>", "</form>",
    "<h1>", "</h1>", "<h2>", "<h3>", "<b>", "</b>", "<i>", "<u>", "<code>", "<pre>", "</pre>", "<br>", "<hr>",
    "<center>", "</center>", "<font color=red size=+2>", "</font>", "<details>", "<summary>", "</details>",
    "<blockquote>", "<dl><dt>x<dd>y</dl>", "<nav>", "<header>", "<footer>", "<section>", "<article>",
    "<span style=\"float:left;width:50px\">", "<div style=\"float:right\">", "<div style=\"clear:both\">",
    "<div style=\"display:inline-block;width:70px\">", "<div style=\"display:flex\">", "<div style=\"display:grid;grid-template-columns:1fr 2fr\">",
    "<div style=\"display:table\">", "<div style=\"display:table-cell\">", "<div style=\"position:absolute;top:5px;left:5px\">",
    "<div style=\"position:fixed;bottom:0\">", "<div style=\"overflow:hidden;height:20px\">",
    "<div style=\"margin:-50px 0 0 -50px\">", "<div style=\"padding:1000px\">", "<div style=\"width:calc(100% - 30px)\">",
    "<div style=\"width:99999px\">", "<div style=\"height:99999px\">", "<div style=\"margin:auto;width:50%\">",
    "<div style=\"line-height:3;text-indent:20px;text-align:justify\">", "<div style=\"white-space:pre\">",
    "<div class=a>", "<div class=\"a b c\">", "<div id=x>", "<p class=hot>", "<span class=a>", "<li class=a>",
    "<style>", "</style>", "<script>", "</script>", "<noscript>", "</noscript>", "<template>", "</template>",
    "<link rel=stylesheet href=a.css>", "<link rel=stylesheet href=b.css media=print>", "<meta charset=utf-8>",
    "<!doctype html>", "<!-- c -->", "<!--", "-->", "<![CDATA[", "]]>", "<?xml ?>", "<", ">", "&", "&amp;", "&lt;",
    "&#65;", "&#x1F600;", "&nbsp;", "&hellip;", "&notanentity;", "\xc3\xa9", "\xe2\x80\x94", "\xf0\x9f\x98\x80",
    "\xff\xfe", "text ", "word ", "averyveryveryverylongwordthatwillnotwrapatallbecauseitisjustonewordohno ", "\n",
};

static const char *css_toks[] = {
    "div", "p", "a", ".a", ".b", "#x", "ul li", "div > p", "h1 + p", "a ~ span", "li:first-child", "li:last-child",
    "li:nth-child(2n+1)", "p:not(.a)", "a[href]", "input[type=text]", "[class*=\"a\"]", "a:hover", "p::first-line",
    "p::before", "p::after", ":root", "html", "body", "*", "td", "th", "table", "tr:nth-of-type(odd)",
    "div:is(.a,.b)", ":where(p)", "a:link", "@media (max-width: 600px) {", "@media print {", "@media (min-width:1px) and (max-width:9999px){",
    "@supports (display:grid) {", "@import url(c.css);", "@font-face {", "@keyframes x {", "@layer base {", "}", "{", ";", ":",
    "color:#f00;", "color:rgb(1,2,3);", "color:hsl(120,50%,50%);", "color:var(--c);", "--c:#00f;", "--w:calc(1px + 2%);",
    "background:#eee;", "background:url(x) no-repeat #fff;", "background:linear-gradient(red,blue);",
    "width:calc(100% - 20px);", "width:var(--w);", "width:50%;", "width:10em;", "width:99999px;", "min-width:200px;",
    "max-width:100%;", "height:50px;", "margin:0 auto;", "margin:-10px;", "margin:1em 2em 3em 4em;", "padding:5px 10px;",
    "padding:99999px;", "border:1px solid #000;", "border-bottom:2px dashed red;", "border-radius:50%;",
    "display:none;", "display:block;", "display:inline-block;", "display:flex;", "display:grid;", "display:table;",
    "display:table-cell;", "display:table-row;", "display:list-item;", "display:contents;", "float:left;", "float:right;",
    "clear:both;", "position:absolute;", "position:relative;", "position:fixed;", "position:sticky;", "top:0;", "left:10px;",
    "right:-5px;", "bottom:50%;", "z-index:5;", "overflow:hidden;", "overflow:auto;", "opacity:.5;", "visibility:hidden;",
    "font-size:2em;", "font-size:12px;", "font:bold 20px/1.5 serif;", "font-weight:700;", "line-height:2;", "line-height:30px;",
    "text-align:center;", "text-align:right;", "text-indent:30px;", "text-transform:uppercase;", "text-decoration:underline;",
    "letter-spacing:3px;", "white-space:nowrap;", "vertical-align:middle;", "list-style:none;", "list-style-type:upper-roman;",
    "content:\"x\";", "content:\"\";", "content:'\\2014';", "content:counter(c);", "flex:1;", "flex-direction:column;",
    "flex-wrap:wrap;", "justify-content:space-between;", "align-items:center;", "gap:10px 20px;", "order:2;",
    "grid-template-columns:repeat(3,1fr);", "grid-column:1 / span 2;", "box-sizing:border-box;", "border-collapse:collapse;",
    "border-spacing:5px;", "!important;", " !important", "/*", "*/", "\n", " ", "\"", "'", "(", ")", "var(", "calc(", "url(",
};

static char g_page[1 << 18];
static char g_css[1 << 16];

static int gen_tokens(char *out, int cap, const char *const *toks, int ntok, int count) {
    int n = 0;
    for (int i = 0; i < count; i++) {
        const char *t = toks[rnd() % (unsigned)ntok];
        int l = (int)strlen(t);
        if (n + l + 1 >= cap) break;
        memcpy(out + n, t, (size_t)l);
        n += l;
    }
    out[n] = 0;
    return n;
}

static void mutate(char *buf, int *n, int cap) {
    int ops = 1 + (int)(rnd() % 8);
    for (int k = 0; k < ops && *n > 4; k++) {
        int a = (int)(rnd() % (unsigned)*n);
        switch (rnd() % 6) {
        case 0: buf[a] = (char)rnd(); break;                                   /* flip */
        case 1: {                                                              /* delete a run */
            int len = 1 + (int)(rnd() % 200);
            if (a + len > *n) len = *n - a;
            memmove(buf + a, buf + a + len, (size_t)(*n - a - len));
            *n -= len;
            break;
        }
        case 2: {                                                              /* duplicate a run */
            int len = 1 + (int)(rnd() % 400);
            if (a + len > *n) len = *n - a;
            if (*n + len >= cap) break;
            memmove(buf + a + len, buf + a, (size_t)(*n - a));
            memcpy(buf + a, buf + a + len, (size_t)len);
            *n += len;
            break;
        }
        case 3: {                                                              /* splice an HTML token */
            const char *t = html_toks[rnd() % (sizeof(html_toks) / sizeof(html_toks[0]))];
            int l = (int)strlen(t);
            if (*n + l >= cap) break;
            memmove(buf + a + l, buf + a, (size_t)(*n - a));
            memcpy(buf + a, t, (size_t)l);
            *n += l;
            break;
        }
        case 4: {                                                              /* splice a CSS token */
            const char *t = css_toks[rnd() % (sizeof(css_toks) / sizeof(css_toks[0]))];
            int l = (int)strlen(t);
            if (*n + l >= cap) break;
            memmove(buf + a + l, buf + a, (size_t)(*n - a));
            memcpy(buf + a, t, (size_t)l);
            *n += l;
            break;
        }
        default: {                                                             /* truncate */
            if (rnd() % 8 == 0) *n = a;
            break;
        }
        }
    }
    buf[*n] = 0;
}

static int g_fails;

static void check_invariants(const char *what) {
    int n = rb_box_count();
    if (n < 0 || n > 16384) { printf("fuzz: %s: box count %d\n", what, n); g_fails++; return; }
    for (int i = 0; i < n; i++) {
        const RbBox *b = rb_box(i);
        if (!b) { printf("fuzz: %s: null box %d\n", what, i); g_fails++; return; }
        if (b->w < 0 || b->h < 0) { printf("fuzz: %s: negative size %d\n", what, i); g_fails++; return; }
        if (b->kind == RB_BOX_TEXT) {
            int len = 0;
            const char *t = rb_box_text(i, &len);
            if (len < 0 || (len > 0 && !t)) { printf("fuzz: %s: bad text box %d\n", what, i); g_fails++; return; }
        }
    }
    int h = rb_page_height();
    if (h < 0 || h > 4000000) { printf("fuzz: %s: page height %d\n", what, h); g_fails++; }
    if (rb_debug_stat(8) > 200000) { printf("fuzz: %s: layout work %d\n", what, rb_debug_stat(8)); g_fails++; }
}

static void drive(const char *html, int n) {
    /* the viewport is a runtime setting: mostly the two real screens, now and
       then anything rb_set_viewport accepts */
    unsigned v = rnd() % 10;
    if (v < 5) rb_set_viewport(RB_VIEW_W_DEFAULT, RB_VIEW_H_DEFAULT);
    else if (v < 8) rb_set_viewport(622, 332);
    else rb_set_viewport((int)(rnd() % 1100), (int)(rnd() % 5000));
    rb_set_images(rnd() % 4 != 0);
    rb_open_web(html, n);
    check_invariants("open");
    /* feed every stylesheet the page names with generated CSS */
    for (int i = 0; i < rb_css_pending() && i < 8; i++) {
        int cn = gen_tokens(g_css, (int)sizeof(g_css), css_toks, (int)(sizeof(css_toks) / sizeof(css_toks[0])),
                            20 + (int)(rnd() % 300));
        rb_css_provide(i, g_css, cn);
    }
    rb_relayout();
    check_invariants("css");
    /* image outcomes as a loader would store them, any natural size */
    for (int node = rb_dom_img_next(0); node; node = rb_dom_img_next(node)) {
        static const int sizes[] = { 0, 1, 2, 7, 16, 100, 640, 1023, 4096, 65535 };
        int st = (int)(rnd() % 5);
        rb_dom_img_set(node, st > 3 ? RB_IMG_NONE : st, (int)(rnd() % 64), sizes[rnd() % 10], sizes[rnd() % 10]);
        if (rnd() % 8 == 0) rb_dom_set_attr(node, "src", 3, "z.png", 5);
    }
    rb_relayout();
    check_invariants("images");
    /* hit tests, focus walk and a keystroke must stay in bounds */
    for (int k = 0; k < 8; k++) {
        int x = (int)(rnd() % (unsigned)(RB_VIEW_W + 16)), y = (int)(rnd() % 2000);
        int nd = rb_hit_at(x, y, (int)(rnd() % 500));
        (void)nd;
    }
    int fc = rb_focus_count();
    if (fc > 0) {
        int f = rb_focus_node(0);
        if (rb_is_field(f)) rb_type_char(f, 'x');
    }
    rb_relayout();
    check_invariants("relayout");
#ifdef FUZZ_PAINT
    if (rnd() % 2) rb_ui_layout(RB_FB_MAX_W, RB_FB_MAX_H);
    else rb_ui_layout(320, 200);
    rb_paint_begin(g_fb, rb_ui.w, rb_ui.h);
    rb_paint_set_images(rnd() % 3 ? fake_images : 0);
    for (int k = 0; k < 3; k++) {
        int scroll = k == 0 ? 0 : (int)(rnd() % (unsigned)(rb_page_height() + 400)) - 200;
        rb_paint_page(scroll, rb_focus_count() > 0 ? rb_focus_node(0) : 0);
    }
    if (rnd() % 4 == 0) rb_quantize_8bpp(g_fb, g_fb8, rb_ui.w, rb_ui.h);
#endif
}

static int read_file(const char *path, char *out, int cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int n = (int)fread(out, 1, (size_t)cap - 1, f);
    fclose(f);
    out[n] = 0;
    return n;
}

int main(int argc, char **argv) {
    int iters = argc > 1 ? atoi(argv[1]) : 500;
    if (argc > 2) rng = (unsigned long long)atoll(argv[2]) * 0x9E3779B97F4A7C15ULL + 1;
    static char seed_buf[8][1 << 17];
    int seed_len[8], nseeds = 0;
    for (int a = 3; a < argc && nseeds < 8; a++) {
        seed_len[nseeds] = read_file(argv[a], seed_buf[nseeds], (int)sizeof(seed_buf[0]));
        if (seed_len[nseeds] > 0) nseeds++;
    }
    for (int it = 0; it < iters; it++) {
        int n;
        if (nseeds && rnd() % 4) {
            int s = (int)(rnd() % (unsigned)nseeds);
            n = seed_len[s] < (int)sizeof(g_page) - 1 ? seed_len[s] : (int)sizeof(g_page) - 1;
            memcpy(g_page, seed_buf[s], (size_t)n);
            g_page[n] = 0;
            mutate(g_page, &n, (int)sizeof(g_page) - 2);
        } else {
            n = gen_tokens(g_page, (int)sizeof(g_page), html_toks, (int)(sizeof(html_toks) / sizeof(html_toks[0])),
                           10 + (int)(rnd() % 600));
            /* every so often: very deep nesting */
            if (rnd() % 16 == 0) {
                int depth = 20 + (int)(rnd() % 600);
                int w = 0;
                for (int d = 0; d < depth && w + 40 < (int)sizeof(g_page); d++) {
                    const char *t = rnd() % 3 ? "<div>" : (rnd() % 2 ? "<span>" : "<table><tr><td>");
                    int l = (int)strlen(t);
                    memcpy(g_page + w, t, (size_t)l);
                    w += l;
                }
                memcpy(g_page + w, "deep", 4);
                w += 4;
                g_page[w] = 0;
                n = w;
            }
        }
        drive(g_page, n);
        if (g_fails) {
            printf("fuzz: failing input (iteration %d, %d bytes) written to /tmp/rbfuzz_web_fail.html\n", it, n);
            FILE *f = fopen("/tmp/rbfuzz_web_fail.html", "wb");
            if (f) { fwrite(g_page, 1, (size_t)n, f); fclose(f); }
            return 1;
        }
    }
    printf("fuzz_web done\n");
    return 0;
}
