/* Host tests for the rbrowse web profile (rb_open_web): the CSS cascade and
   selectors, units, the HTML tree builder, and web layout (lists, tables,
   floats, inline-block, replaced elements, generated content).

     clang -g -O1 -fsanitize=address,undefined -DRBROWSE_HOST -I src/rbrowse \
         tools/rbrowse/test_web.c src/rbrowse/engine.c -o /tmp/rbtest_web && /tmp/rbtest_web
*/
#include "engine.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("fail %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        fails++; \
    } \
} while (0)

static void web(const char *html) { rb_open_web(html, (int)strlen(html)); }

/* Like web(), with every element given a background so rb_dom_rect can measure it. */
static void webg(const char *html) {
    static char buf[8192];
    snprintf(buf, sizeof(buf), "<style>*{background:#ccc}</style>%s", html);
    web(buf);
}

static int rect(const char *id, int *x, int *y, int *w, int *h) {
    int n = rb_find_id(id);
    if (!n) return 0;
    return rb_dom_rect(n, x, y, w, h);
}

/* x of the first text box whose text starts with `word`, or -1 */
static int text_x(const char *word) {
    int wl = (int)strlen(word);
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        int tl = 0;
        const char *t = rb_box_text(i, &tl);
        if (b->kind == RB_BOX_TEXT && tl >= wl && memcmp(t, word, (size_t)wl) == 0) return b->x;
    }
    return -1;
}

static int text_y(const char *word) {
    int wl = (int)strlen(word);
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        int tl = 0;
        const char *t = rb_box_text(i, &tl);
        if (b->kind == RB_BOX_TEXT && tl >= wl && memcmp(t, word, (size_t)wl) == 0) return b->y;
    }
    return -1;
}

static int text_color(const char *word) {
    int wl = (int)strlen(word);
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        int tl = 0;
        const char *t = rb_box_text(i, &tl);
        if (b->kind == RB_BOX_TEXT && tl >= wl && memcmp(t, word, (size_t)wl) == 0) return b->color;
    }
    return -1;
}

static int has_text(const char *word) { return text_x(word) >= 0; }

static int node_color(const char *id) { return rb_node_color(rb_find_id(id)); }

/* ---- selectors and the cascade ---------------------------------------------- */

static void test_selectors(void) {
    web("<style>"
        "p{color:#ff0000} .a{color:#00ff00} #i{color:#0000ff}"
        "div p{color:#ffff00} div>p.b{color:#00ffff} h1+p{color:#ff00ff}"
        "a[href]{color:#808080} a[href^=\"http\"]{color:#404040}"
        "li:first-child{color:#800000} li:last-child{color:#008000} li:nth-child(2){color:#000080}"
        "p:not(.a){text-decoration:underline}"
        "</style>"
        "<p id=p1>x</p><p id=p2 class=a>x</p><p id=i>x</p>"
        "<div><p id=d1>x</p><div><p id=d2 class=b>x</p></div></div>"
        "<h1>t</h1><p id=adj>x</p>"
        "<a id=l1 href=\"/rel\">x</a><a id=l2 href=\"http://x\">x</a><a id=l3>x</a>"
        "<ul><li id=f>1<li id=m>2<li id=e>3</ul>");
    CHECK(node_color("p1") == rb_map_rgb(255, 0, 0));
    CHECK(node_color("p2") == rb_map_rgb(0, 255, 0));          /* .a beats p */
    CHECK(node_color("i") == rb_map_rgb(0, 0, 255));           /* #id beats everything */
    CHECK(node_color("d1") == rb_map_rgb(255, 255, 0));        /* descendant */
    CHECK(node_color("d2") == rb_map_rgb(0, 255, 255));        /* child + class beats descendant */
    CHECK(node_color("adj") == rb_map_rgb(255, 0, 255));       /* adjacent sibling */
    CHECK(node_color("l1") == rb_map_rgb(128, 128, 128));      /* [href] */
    CHECK(node_color("l2") == rb_map_rgb(64, 64, 64));         /* [href^=] later and equally specific */
    CHECK(node_color("f") == rb_map_rgb(128, 0, 0));
    CHECK(node_color("e") == rb_map_rgb(0, 128, 0));
    CHECK(node_color("m") == rb_map_rgb(0, 0, 128));
    /* a rule with an unknown pseudo-class or a pseudo-element on the link never matches */
    web("<style>a:hover{color:#ff0000} p::first-line{color:#ff0000} a{color:#0000ff}</style><a id=l href=/x>x</a><p id=p>x</p>");
    CHECK(node_color("l") == rb_map_rgb(0, 0, 255));
    CHECK(node_color("p") != rb_map_rgb(255, 0, 0));
}

static void test_important_and_order(void) {
    web("<style>p{color:#ff0000 !important} #a{color:#00ff00} p{color:#0000ff}</style>"
        "<p id=a style=\"color:#00ffff\">x</p><p id=b>x</p>");
    CHECK(node_color("a") == rb_map_rgb(255, 0, 0));           /* !important beats inline and id */
    CHECK(node_color("b") == rb_map_rgb(255, 0, 0));
    /* later equal-specificity rule wins; inline beats rules */
    web("<style>p{color:#ff0000} p{color:#00ff00}</style><p id=a>x</p><p id=b style=\"color:#0000ff\">x</p>");
    CHECK(node_color("a") == rb_map_rgb(0, 255, 0));
    CHECK(node_color("b") == rb_map_rgb(0, 0, 255));
}

static void test_media_and_supports(void) {
    web("<style>"
        "@media (max-width: 400px){ #a{color:#ff0000} }"
        "@media (min-width: 900px){ #b{color:#ff0000} }"
        "@media print{ #c{color:#ff0000} }"
        "@media screen and (max-width:700px){ #d{color:#ff0000} }"
        "@media (prefers-color-scheme: dark){ #e{color:#ff0000} }"
        "@supports (display:grid){ #f{color:#ff0000} }"
        "</style><p id=a>x</p><p id=b>x</p><p id=c>x</p><p id=d>x</p><p id=e>x</p><p id=f>x</p>");
    int red = rb_map_rgb(255, 0, 0);
    CHECK(node_color("a") == red);
    CHECK(node_color("b") != red);
    CHECK(node_color("c") != red);
    CHECK(node_color("d") == red);
    CHECK(node_color("e") != red);
    CHECK(node_color("f") == red);
}

static void test_custom_properties(void) {
    webg("<style>:root{--c:#ff0000;--w:40px} #a{color:var(--c);width:var(--w)} #b{color:var(--nope,#00ff00)}"
        "#c{color:var(--nope)}</style><div id=a>x</div><div id=b>x</div><div id=c>x</div>");
    CHECK(node_color("a") == rb_map_rgb(255, 0, 0));
    int x, y, w, h;
    CHECK(rect("a", &x, &y, &w, &h) && w == 40);
    CHECK(node_color("b") == rb_map_rgb(0, 255, 0));            /* fallback */
    CHECK(node_color("c") == rb_map_rgb(0, 0, 0));              /* unresolved: declaration dropped */
}

/* Phase 4a: colours are exact 0xRRGGBB (no 6x6x6 cube) and the viewport is a
   runtime setting. */
static void test_truecolor(void) {
    web("<style>#a{color:#123456} #b{color:rgb(1,2,3)} #c{background:#f6f6ef} #d{color:hsl(210,50%,40%)}"
        "#e{border:2px solid #fedcba}</style>"
        "<p id=a>a</p><p id=b>b</p><div id=c>c</div><p id=d>d</p><div id=e>e</div>"
        "<table><tr><td id=t bgcolor=\"#0a0b0c\">t</td></tr></table>");
    CHECK(node_color("a") == 0x123456);
    CHECK(node_color("b") == 0x010203);
    CHECK(node_color("d") == 0x336699);            /* hsl(210,50%,40%) */
    int c = rb_find_id("c"), e = rb_find_id("e"), t = rb_find_id("t");
    int bg_c = 0, ring_e = 0, bg_t = 0;
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        if (b->node == c && b->kind == RB_BOX_BG && b->bg == 0xf6f6ef) bg_c = 1;
        if (b->node == e && b->kind == RB_BOX_BG && b->bg == 0xfedcba) ring_e = 1;
        if (b->node == t && b->kind == RB_BOX_BG && b->bg == 0x0a0b0c) bg_t = 1;
    }
    CHECK(bg_c && ring_e && bg_t);
    web("<body style=\"background:#f6f6ef\">x</body>");
    CHECK(rb_page_bg() == 0xf6f6ef);
    /* the 8-bit fallback's quantizer */
    CHECK(rb_rgb_index(RB_RGB(255, 0, 0)) == 16 + 5 * 36);
    CHECK(rb_rgb_index(RB_RGB(255, 255, 255)) == 231);
    CHECK(rb_rgb_index(RB_RGB(100, 100, 100)) == 232 + 10);
    int r, g, b;
    rb_cube_rgb(rb_rgb_index(0xff6600), &r, &g, &b);
    CHECK(r == 255 && g == 102 && b == 0);
}

static void test_viewport(void) {
    const char *page = "<style>@media (min-width:600px){#w{color:#ff0000}} #h{width:50vw}</style>"
                       "<div id=w>wide</div><div id=h>half</div><div id=f>fill</div>";
    int x, y, w, h;
    CHECK(rb_view_width() == RB_VIEW_W_DEFAULT && rb_view_height() == RB_VIEW_H_DEFAULT);
    webg(page);
    CHECK(node_color("w") != rb_map_rgb(255, 0, 0));
    CHECK(rb_dom_rect(rb_find_id("f"), &x, &y, &w, &h) && w == 304);
    CHECK(rb_dom_rect(rb_find_id("h"), &x, &y, &w, &h) && w == 152);
    /* the 640x360 screen: 622 px of page, 332 rows */
    rb_set_viewport(622, 332);
    CHECK(RB_VIEW_W == 622 && RB_VIEW_H == 332);
    webg(page);
    CHECK(node_color("w") == rb_map_rgb(255, 0, 0));
    CHECK(rb_dom_rect(rb_find_id("f"), &x, &y, &w, &h) && w == 622);
    CHECK(rb_dom_rect(rb_find_id("h"), &x, &y, &w, &h) && w == 311);
    /* a relayout alone picks up a new width (media rules wait for rb_open) */
    rb_set_viewport(500, 332);
    rb_relayout();
    CHECK(rb_dom_rect(rb_find_id("f"), &x, &y, &w, &h) && w == 500);
    for (int i = 0; i < rb_box_count(); i++) CHECK(rb_box(i)->x + rb_box(i)->w <= 500);
    /* clamped */
    rb_set_viewport(5, 100000);
    CHECK(RB_VIEW_W == 64 && RB_VIEW_H == 4096);
    rb_set_viewport(RB_VIEW_W_DEFAULT, RB_VIEW_H_DEFAULT);
    webg(page);
    CHECK(node_color("w") != rb_map_rgb(255, 0, 0));
    CHECK(rb_dom_rect(rb_find_id("f"), &x, &y, &w, &h) && w == 304);
}

static void test_colors(void) {
    web("<style>#a{color:rgb(255,0,0)} #b{color:hsl(120,100%,50%)} #c{color:#0000ffff} #d{color:rgba(0,0,0,.2)}"
        "#e{color:rebeccapurple} #f{color:light-dark(#f00,#00f)} #g{background:transparent}</style>"
        "<p id=a>a</p><p id=b>b</p><p id=c>c</p><p id=d style=\"background:rgba(0,0,0,.05)\">d</p><p id=e>e</p><p id=f>f</p><p id=g>g</p>");
    CHECK(node_color("a") == rb_map_rgb(255, 0, 0));
    CHECK(node_color("b") == rb_map_rgb(0, 255, 0));
    CHECK(node_color("c") == rb_map_rgb(0, 0, 255));
    CHECK(node_color("e") == rb_map_rgb(0x66, 0x33, 0x99));
    CHECK(node_color("f") == rb_map_rgb(255, 0, 0));            /* light scheme */
}

/* ---- units and the zoom ------------------------------------------------------- */

static void test_units(void) {
    int x, y, w, h;
    /* horizontal lengths map 1:1, vertical lengths are halved */
    webg("<body style=\"margin:0\"><div id=a style=\"width:100px;height:40px\"></div>");
    CHECK(rect("a", &x, &y, &w, &h) && w == 100 && h == 20);
    webg("<body style=\"margin:0\"><div id=a style=\"width:5em;height:4em\"></div>");   /* 1em = 16 css px */
    CHECK(rect("a", &x, &y, &w, &h) && w == 80 && h == 32);
    webg("<body style=\"margin:0\"><div id=a style=\"width:50%;height:4px\"></div>");
    CHECK(rect("a", &x, &y, &w, &h) && w == 152);
    webg("<body style=\"margin:0\"><div id=a style=\"width:calc(100% - 40px);height:4px\"></div>");
    CHECK(rect("a", &x, &y, &w, &h) && w == 264);
    webg("<body style=\"margin:0\"><div id=a style=\"width:50vw;height:4px\"></div>");
    CHECK(rect("a", &x, &y, &w, &h) && w == 152);
    webg("<body style=\"margin:0\"><div id=a style=\"margin-left:10px;width:30px;height:4px\"></div>");
    CHECK(rect("a", &x, &y, &w, &h) && x == 10);
    webg("<body style=\"margin:0\"><div id=a style=\"padding:8px 20px\"><div id=s style=\"height:4px\"></div></div>");
    CHECK(rect("s", &x, &y, &w, &h) && x == 20 && y == 4 && w == 264);   /* 1:1 across, 8px css = 4px down */
    webg("<body><div id=a style=\"height:4px\"></div>");
    CHECK(rect("a", &x, &y, &w, &h) && x == 8);                           /* the UA's 8px body margin */
}

static void test_transform(void) {
    int x, y, w, h;
    webg("<body style=\"margin:0\"><div id=a style=\"width:100px;height:8px;transform:translateX(-100%)\"></div>");
    CHECK(rect("a", &x, &y, &w, &h) && x == -100 && w == 100);       /* percent of the box itself */
    webg("<body style=\"margin:0\"><div id=a style=\"width:100px;height:8px;transform:translate(30px,10px)\"></div>");
    CHECK(rect("a", &x, &y, &w, &h) && x == 30 && y == 5);           /* vertical halved */
    webg("<body style=\"margin:0\"><div id=a style=\"width:100px;height:8px;transform:rotate(45deg)\"></div>");
    CHECK(rect("a", &x, &y, &w, &h) && x == 0 && y == 0);            /* unsupported functions are ignored */
}

static void test_font_scale(void) {
    web("<h1 id=h>big</h1><p id=p>small</p>");
    int h1 = -1, p = -1;
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        if (b->kind != RB_BOX_TEXT) continue;
        int l;
        const char *t = rb_box_text(i, &l);
        if (l >= 3 && memcmp(t, "big", 3) == 0) h1 = b->scale;
        if (l >= 5 && memcmp(t, "small", 5) == 0) p = b->scale;
    }
    CHECK(h1 == 2);          /* 2em of 16px */
    CHECK(p == 1);
    web("<div style=\"font-size:32px\"><span id=a>big</span></div><div style=\"font-size:12px\">sm</div>");
    int found2 = 0;
    for (int i = 0; i < rb_box_count(); i++) if (rb_box(i)->kind == RB_BOX_TEXT && rb_box(i)->scale == 2) found2++;
    CHECK(found2 >= 1);
}

/* ---- HTML tree builder --------------------------------------------------------- */

static void test_parser(void) {
    /* <p> closes before a block, <li> before <li>, td/tr implied end tags */
    web("<p id=a>one<div id=b>two</div><p id=c>three");
    int pa = rb_find_id("a"), pb = rb_find_id("b");
    CHECK(rb_dom_parent(pa) == rb_dom_parent(pb));                /* siblings, not nested */
    web("<ul><li id=a>x<li id=b>y<li id=c>z</ul>");
    CHECK(rb_dom_parent(rb_find_id("a")) == rb_dom_parent(rb_find_id("b")));
    web("<table><tr id=r1><td id=c1>a<td id=c2>b<tr id=r2><td id=c3>c</table>");
    CHECK(rb_dom_parent(rb_find_id("c1")) == rb_find_id("r1"));
    CHECK(rb_dom_parent(rb_find_id("c2")) == rb_find_id("r1"));
    CHECK(rb_dom_parent(rb_find_id("c3")) == rb_find_id("r2"));
    /* a stray end tag does not close the document */
    web("<div id=a>one</span></p></div></b><div id=b>two</div>");
    CHECK(has_text("one") && has_text("two"));
    CHECK(rb_dom_parent(rb_find_id("a")) == rb_dom_parent(rb_find_id("b")));
    /* script and style bodies never become text; comments vanish */
    web("<p>x<script>var a = '<p>nope</p>';</script><style>p{color:red}</style><!-- hidden -->y</p>");
    CHECK(!has_text("nope") && !has_text("hidden") && !has_text("var"));
    /* entities and UTF-8 fold to ASCII for the 8x8 font */
    web("<p id=e>caf&eacute; &copy; 2026 &mdash; &#8220;q&#8221; &hellip; \xc3\xa9\xe2\x80\x94</p>");
    char t[128];
    int n = rb_collect_text(rb_find_id("e"), t, (int)sizeof(t));
    CHECK(n > 0 && strstr(t, "cafe (c) 2026 - \"q\" ... e-") != 0);
    /* a leading space between inline runs survives */
    web("<p><b>a</b> b</p>");
    CHECK(text_x("b") > text_x("a") + 8);
    /* custom elements match type selectors by name */
    web("<style>my-tag{color:#ff0000} other-tag{color:#00ff00}</style><my-tag id=a>x</my-tag><other-tag id=b>y</other-tag>");
    CHECK(node_color("a") == rb_map_rgb(255, 0, 0));
    CHECK(node_color("b") == rb_map_rgb(0, 255, 0));
}

static void test_deep_nesting(void) {
    static char big[1 << 16];
    int n = 0;
    for (int i = 0; i < 400; i++) n += (int)strlen(strcpy(big + n, "<div>"));
    n += (int)strlen(strcpy(big + n, "deepest"));
    rb_open_web(big, n);
    /* must neither crash nor loop; very deep content may be cut off */
    CHECK(rb_page_height() >= 0);
    n = 0;
    for (int i = 0; i < 60; i++) n += (int)strlen(strcpy(big + n, "<div><span>"));
    n += (int)strlen(strcpy(big + n, "ok"));
    rb_open_web(big, n);
    CHECK(has_text("ok"));
}

/* ---- layout ------------------------------------------------------------------- */

static void test_lists(void) {
    webg("<ul><li id=a>one</li><li id=b>two</li></ul><ol><li id=c>x</li><li id=d>y</li></ol>");
    int discs = 0, nums = 0;
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        if (b->kind == RB_BOX_BG && b->radius > 0 && b->w <= 8) discs++;
        int l;
        const char *t = rb_box_text(i, &l);
        if (b->kind == RB_BOX_TEXT && l == 2 && t[1] == '.' && (t[0] == '1' || t[0] == '2')) nums++;
    }
    CHECK(discs == 2);
    CHECK(nums == 2);
    int ax, ay, aw, ah, bx, by, bw, bh;
    CHECK(rect("a", &ax, &ay, &aw, &ah) && rect("b", &bx, &by, &bw, &bh));
    CHECK(by > ay);
    /* the marker sits in the gutter left of the item */
    webg("<ul><li id=a>one</li></ul>");
    CHECK(text_x("one") == 40);          /* the UA's 40px list gutter; the marker is left of it */
    webg("<ul style=\"list-style:none;padding:0\"><li id=a>one</li></ul>");
    int none_markers = 0;
    for (int i = 0; i < rb_box_count(); i++) if (rb_box(i)->kind == RB_BOX_BG && rb_box(i)->radius > 0) none_markers++;
    CHECK(none_markers == 0);
}

static void test_margin_collapse(void) {
    int x, y1, y2, w, h1, h2;
    webg("<body style=\"margin:0\"><div id=a style=\"height:20px;margin-bottom:30px\"></div>"
        "<div id=b style=\"height:20px;margin-top:10px\"></div>");
    CHECK(rect("a", &x, &y1, &w, &h1) && rect("b", &x, &y2, &w, &h2));
    /* 20px css height is 10px; adjoining margins 15 and 5 collapse to 15 */
    CHECK(y2 - (y1 + h1) == 15);
}

static void test_inline_block_and_replaced(void) {
    int x, y, w, h;
    webg("<body style=\"margin:0\"><span id=a style=\"display:inline-block;width:60px;height:20px\">aa</span>"
        "<span id=b style=\"display:inline-block;width:60px;height:20px\">bb</span>");
    int ax, bx;
    CHECK(rect("a", &ax, &y, &w, &h) && w == 60);
    CHECK(rect("b", &bx, &y, &w, &h) && bx >= ax + 60);
    webg("<img id=i src=x.png width=100 height=40 alt=pic>");
    CHECK(rect("i", &x, &y, &w, &h) && w == 100 && h == 20);
    webg("<p><img id=i src=x.png alt=\"just words\"></p>");
    CHECK(has_text("just"));                                         /* unsized: alt flows as text */
    webg("<style>img{max-width:100%}</style><img id=i src=x width=900 height=450>");
    CHECK(rect("i", &x, &y, &w, &h) && w <= 304 && h * 2 <= w + 2);  /* aspect kept: 2:1 css => 4:1 here */
    webg("<select><option>alpha<option selected>beta</select>");
    CHECK(has_text("beta") && !has_text("alpha"));
}

static void test_tables(void) {
    int ax, ay, aw, ah, bx, by, bw, bh, cx, cy, cw, ch;
    webg("<table style=\"border-spacing:0\"><tr><td id=a>one</td><td id=b>twotwotwo</td></tr>"
        "<tr><td id=c>x</td><td>y</td></tr></table>");
    CHECK(rect("a", &ax, &ay, &aw, &ah) && rect("b", &bx, &by, &bw, &bh) && rect("c", &cx, &cy, &cw, &ch));
    CHECK(ay == by);                       /* cells share a row */
    CHECK(bx >= ax + aw);
    CHECK(cx == ax);                       /* columns line up */
    CHECK(cy > ay);
    CHECK(aw >= 24 && bw >= 72);           /* each column at least its text */
    /* colspan */
    webg("<table style=\"border-spacing:0\"><tr><td id=a colspan=2>wide cell here</td></tr>"
        "<tr><td id=b>1</td><td id=c>2</td></tr></table>");
    CHECK(rect("a", &ax, &ay, &aw, &ah) && rect("b", &bx, &by, &bw, &bh) && rect("c", &cx, &cy, &cw, &ch));
    CHECK(aw >= bw + cw - 1);
    /* a shorter cell is stretched to the row (its background reaches the row's bottom) */
    webg("<table style=\"border-spacing:0\"><tr><td id=a style=\"background:#ff0000\">x</td>"
        "<td id=b>one<br>two<br>three</td></tr></table>");
    CHECK(rect("a", &ax, &ay, &aw, &ah) && rect("b", &bx, &by, &bw, &bh));
    CHECK(ah == bh);
    /* percentage widths */
    webg("<table width=100%><tr><td id=a width=25%>a</td><td id=b>b</td></tr></table>");
    CHECK(rect("a", &ax, &ay, &aw, &ah) && rect("b", &bx, &by, &bw, &bh));
    CHECK(aw < bw);
    /* a table shrinks to its content unless told otherwise */
    webg("<table style=\"border-spacing:0\"><tr><td id=a>hi</td></tr></table>");
    CHECK(rect("a", &ax, &ay, &aw, &ah) && aw < 100);
}

static void test_floats(void) {
    int x, y, w, h;
    webg("<body style=\"margin:0\"><div id=f style=\"float:left;width:100px;height:40px\"></div>"
        "<p id=p>text wraps beside the float</p>");
    CHECK(rect("f", &x, &y, &w, &h) && x == 0 && w == 100);
    CHECK(text_x("text") >= 100);                  /* the line starts after the float */
    webg("<body style=\"margin:0\"><div id=f style=\"float:right;width:100px;height:20px\"></div>"
        "<p id=p>aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa</p>");
    CHECK(rect("f", &x, &y, &w, &h) && x + w == 304);
    /* below the float the text uses the full width again */
    webg("<body style=\"margin:0\"><div style=\"float:left;width:100px;height:8px\"></div>"
        "<p>one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen</p>");
    int xs[8], n = 0;
    for (int i = 0; i < rb_box_count() && n < 8; i++) {
        const RbBox *b = rb_box(i);
        if (b->kind == RB_BOX_TEXT) xs[n++] = b->x;
    }
    CHECK(n >= 5);
    int min_x = 1000, saw_full = 0;
    for (int i = 0; i < n; i++) if (xs[i] < min_x) min_x = xs[i];
    saw_full = min_x == 0;
    CHECK(saw_full);
    /* clear:both pushes a block below the float */
    webg("<body style=\"margin:0\"><div id=f style=\"float:left;width:50px;height:60px\"></div>"
        "<div id=c style=\"clear:both;height:10px\"></div>");
    int fy, fh, cy, ch;
    CHECK(rect("f", &x, &fy, &w, &fh) && rect("c", &x, &cy, &w, &ch));
    CHECK(cy >= fy + fh);
    /* an overflow:hidden parent contains its floats */
    webg("<body style=\"margin:0\"><div id=p style=\"overflow:hidden\"><div style=\"float:left;width:50px;height:60px\"></div></div>"
        "<div id=n style=\"height:5px\"></div>");
    int py, ph, ny;
    CHECK(rect("p", &x, &py, &w, &ph));
    CHECK(rect("n", &x, &ny, &w, &h));
    CHECK(ny >= py + 30);
}

static void test_text_indent(void) {
    web("<body style=\"margin:0\"><p style=\"margin:0;text-indent:30px\">first line wraps around here to a second line of text</p>");
    CHECK(text_x("first") == 30);
    int second = -1;
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        if (b->kind == RB_BOX_TEXT && b->y > 0 && second < 0) second = b->x;
    }
    CHECK(second == 0);                                  /* only the first line is indented */
}

static void test_generated_content(void) {
    web("<style>p:before{content:\"[\"} p:after{content:\"]\"} .e:after{content:\"\"}</style><p id=p>mid</p>");
    CHECK(has_text("["));
    CHECK(has_text("]"));
    CHECK(text_x("[") < text_x("mid"));
    CHECK(text_x("mid") < text_x("]"));
    /* they do not count as children for structural selectors */
    web("<style>li:first-child{color:#ff0000} li:before{content:\"* \"}</style><ul><li id=a>x</li><li id=b>y</li></ul>");
    CHECK(node_color("a") == rb_map_rgb(255, 0, 0));
    CHECK(node_color("b") != rb_map_rgb(255, 0, 0));
}

static void test_details_and_hidden(void) {
    web("<details><summary>head</summary><p>body text</p></details>");
    CHECK(has_text("head") && !has_text("body"));
    web("<details open><summary>head</summary><p>body text</p></details>");
    CHECK(has_text("body"));
    web("<p hidden>secret</p><p>shown</p><div style=\"display:none\">gone</div>");
    CHECK(!has_text("secret") && !has_text("gone") && has_text("shown"));
}

static void test_flex_min_content(void) {
    /* a flex item never shrinks below its longest word */
    int x, y, w, h;
    webg("<body style=\"margin:0\"><div style=\"display:flex;width:60px\"><div id=a>abcdefghij</div><div id=b>zz</div></div>");
    CHECK(rect("a", &x, &y, &w, &h));
    CHECK(h <= 12);                         /* one line, not ten */
}

/* ---- external stylesheets ------------------------------------------------------- */

static void test_stylesheet_loading(void) {
    web("<link rel=stylesheet href=\"a.css\"><link rel=stylesheet href=print.css media=print>"
        "<link rel=icon href=i.png><p id=p>x</p><link rel=stylesheet href=\"/b.css\">");
    CHECK(rb_css_pending() == 2);                       /* print and icon are not stylesheets */
    char href[64];
    CHECK(rb_css_href(0, href, (int)sizeof(href)) > 0 && strcmp(href, "a.css") == 0);
    CHECK(rb_css_href(1, href, (int)sizeof(href)) > 0 && strcmp(href, "/b.css") == 0);
    int before = node_color("p");
    const char *css = "p{color:#ff0000} @import url(\"c.css\");";
    CHECK(rb_css_provide(0, css, (int)strlen(css)));
    rb_relayout();
    CHECK(node_color("p") == rb_map_rgb(255, 0, 0) && before != node_color("p"));
    CHECK(rb_css_pending() == 3);                       /* the @import was queued */
    CHECK(rb_css_href(2, href, (int)sizeof(href)) > 0 && strcmp(href, "c.css") == 0);
    /* document order beats arrival order: b.css comes after a.css */
    const char *b = "p{color:#0000ff}";
    CHECK(rb_css_provide(1, b, (int)strlen(b)));
    rb_relayout();
    CHECK(node_color("p") == rb_map_rgb(0, 0, 255));
    const char *early = "p{color:#00ff00}";
    CHECK(rb_css_provide(0, early, (int)strlen(early)));    /* a second chunk of the first sheet */
    rb_relayout();
    CHECK(node_color("p") == rb_map_rgb(0, 0, 255));        /* still the later sheet's rule */
}


/* ---- regressions found in review ------------------------------------------------ */

static void test_review_fixes(void) {
    int red = rb_map_rgb(255, 0, 0);
    /* nested selector lists keep their own atoms */
    web("<style>p:is(.a:not(.b)){color:#ff0000}</style><p id=x class=a>x</p><p id=y class=\"a b\">y</p><p id=z class=b>z</p>");
    CHECK(node_color("x") == red);
    CHECK(node_color("y") != red);
    CHECK(node_color("z") != red);
    /* a never-matching pseudo-class inside :not() / :is() is not fatal */
    web("<style>p:not(:hover){color:#ff0000} .q:is(:hover,.k){background:#00ff00}</style><p id=p>x</p><div id=d class=\"q k\">y</div>");
    CHECK(node_color("p") == red);
    CHECK(rb_dom_class_contains(rb_find_id("d"), "k", 1));
    /* escaped class names */
    web("<style>.md\\:flex{color:#ff0000} .w-1\\/2{color:#0000ff}</style><p id=a class=\"md:flex\">a</p><p id=b class=\"w-1/2\">b</p>");
    CHECK(node_color("a") == red);
    CHECK(node_color("b") == rb_map_rgb(0, 0, 255));
    /* a raw-text element that closes <head> does not swallow the page */
    web("<head><title>T</title><svg><circle/></svg><p>Hello world</p>");
    CHECK(has_text("Hello"));
    web("<html><head><textarea>x</textarea><p>after textarea</p>");
    CHECK(has_text("after"));
    /* legacy entities stay literal in an attribute before '=' or a letter */
    web("<a id=l href=\"/s?q=1&para=2&times=3\">x</a>");
    char href[64];
    CHECK(rb_node_href(rb_find_id("l"), href, (int)sizeof(href)) > 0 && strcmp(href, "/s?q=1&para=2&times=3") == 0);
    web("<p id=e>&excl;&exist;</p>");
    char tx[16];
    CHECK(rb_collect_text(rb_find_id("e"), tx, (int)sizeof(tx)) >= 1 && tx[0] == '!');
    /* <style media=print> does not apply on screen */
    web("<style media=print>p{color:#ff0000}</style><p id=p>x</p>");
    CHECK(node_color("p") != red);
    /* unlayered rules beat @layer rules whatever their order */
    web("<style>@layer base{p{color:#ff0000}} p{color:#0000ff}</style><p id=p>x</p>");
    CHECK(node_color("p") == rb_map_rgb(0, 0, 255));
    /* a var() bomb is dropped quickly instead of expanding exponentially */
    static char bomb[4096];
    int n = snprintf(bomb, sizeof(bomb), "<style>:root{--a:1px;--b:var(--a)var(--a)var(--a)var(--a);--c:var(--b)var(--b)var(--b)var(--b);"
                     "--d:var(--c)var(--c)var(--c)var(--c);--e:var(--d)var(--d)var(--d)var(--d);--f:var(--e)var(--e)var(--e)var(--e)}"
                     "p{margin-left:var(--f)}</style><p id=p>x</p>");
    (void)n;
    web(bomb);
    CHECK(has_text("x"));
    /* calc() arithmetic stays inside 32 bits */
    webg("<body style=\"margin:0\"><div id=a style=\"width:calc(400px*46);height:4px\"></div>");
    int x, y, w, h;
    CHECK(rect("a", &x, &y, &w, &h) && w >= 0 && w <= 304);
    webg("<body style=\"margin:0\"><div id=a style=\"width:calc(1000px / -2);height:4px\"></div>");
    CHECK(rect("a", &x, &y, &w, &h));
    /* a very wide replaced element inside a bordered flex item keeps its strips sane */
    web("<div style=\"display:flex\"><div id=b style=\"border:1px solid\"><img width=100000 height=10></div></div>");
    for (int i = 0; i < rb_box_count(); i++) CHECK(rb_box(i)->w >= 0 && rb_box(i)->x >= -16000);
    /* inline @import in <style> is queued; imports come before their importer */
    web("<style>@import 'base.css'; .btn{color:#ff0000}</style><p id=p class=btn>x</p>");
    CHECK(rb_css_pending() == 1 && rb_css_parent(0) == -1);
    const char *base = ".btn{color:#0000ff}";
    CHECK(rb_css_provide(0, base, (int)strlen(base)));
    rb_relayout();
    CHECK(node_color("p") == red);                    /* the importer's own rule wins the tie */
    /* a later list item continues the numbering without rescanning */
    web("<ol><li>a</li><li>b</li><li>c</li><li>d</li></ol>");
    int nums = 0;
    for (int i = 0; i < rb_box_count(); i++) {
        int l; const char *tt = rb_box_text(i, &l);
        if (rb_box(i)->kind == RB_BOX_TEXT && l == 2 && tt[1] == '.' && tt[0] >= '1' && tt[0] <= '4') nums++;
    }
    CHECK(nums == 4);
}

/* Ten nested flex levels must not blank the page. */
static void test_nested_flex_budget(void) {
    static char big[1 << 16];
    int n = 0;
    for (int d = 0; d < 10; d++) n += snprintf(big + n, sizeof(big) - (size_t)n, "<div style=\"display:flex;%s\">",
                                                d & 1 ? "flex-direction:column" : "");
    for (int i = 0; i < 60; i++) n += snprintf(big + n, sizeof(big) - (size_t)n, "<div><p>card %d</p><span>text</span></div>", i);
    for (int d = 0; d < 10; d++) n += snprintf(big + n, sizeof(big) - (size_t)n, "</div>");
    rb_open_web(big, n);
    CHECK(has_text("card"));
    CHECK(rb_page_height() > 0);
    rb_open(big, n);
    CHECK(has_text("card"));
}

/* ---- the native profile keeps its look -------------------------------------------- */

static void test_profiles(void) {
    rb_open("<p id=p>x</p>", 13);
    CHECK(rb_page_bg() == rb_map_rgb(0x66, 0x3C, 0x22));
    web("<p id=p>x</p>");
    CHECK(rb_page_bg() == rb_map_rgb(255, 255, 255));
    rb_open("<p id=p>x</p>", 13);
    CHECK(rb_page_bg() == rb_map_rgb(0x66, 0x3C, 0x22));
}

/* Sizing and centring cases cross-checked against Chromium (tools/rbrowse/refdiff, 304px viewport). */
static void test_sizing_fixes(void) {
    int x, y, w, h;
    /* inline-flex shrink-wraps instead of filling the line */
    webg("<div><label id=l style=\"display:inline-flex;padding:0 11px\"><span>Main menu</span></label></div>");
    CHECK(rect("l", &x, &y, &w, &h) && x == 0 && w == 94);
    /* a div that only holds inline content still breaks the line around it */
    web("<div><span>aaa</span></div><div><span>bbb</span></div>");
    CHECK(text_y("bbb") > text_y("aaa") && text_x("bbb") == 0);
    /* a flex item forced to its width keeps it when its margins are negative */
    webg("<div style=\"display:flex\"><div id=n style=\"margin:0 -12px;width:44px\">b</div></div>");
    CHECK(rect("n", &x, &y, &w, &h) && x == -12 && w == 44);
    /* rem follows the root font size, em the element's */
    web("<html style=\"font-size:62.5%\"><body style=\"margin:0\"><div style=\"padding-left:.5rem\">rem</div>");
    CHECK(text_x("rem") == 5);
    web("<div style=\"padding-left:2em;font-size:.75em\">em</div>");
    CHECK(text_x("em") == 24);
    /* a custom property that refers to itself falls back to the var() fallback */
    webg("<style>body{--m:var(--m,1rem)}</style><span id=s style=\"display:inline-block;height:5px;width:calc(var(--m,1rem) + 4px)\"></span>");
    CHECK(rect("s", &x, &y, &w, &h) && w == 20);
}


/* ===== scripting API ===== */

static int has_str(const char *a, const char *b) { return strstr(a, b) != 0; }

static void test_script_table(void) {
    static char page[16384];
    int node, len;
    const char *text;
    const char *html = "<html><head><script>var a = 1 < 2 && b;</script>"
        "<script src=\"x.js\"></script></head><body id=b>"
        "<div id=d onclick=\"document.getElementById('d').textContent='stub'\">t</div>"
        "<script>document.write('&amp; <b>')</script></body></html>";

    /* off (the default): nothing recorded, the stub still runs onclick */
    rb_set_scripting(0);
    web(html);
    CHECK(rb_script_count() == 0);
    rb_dom_dispatch_check:;
    CHECK(rb_dispatch(rb_find_id("d"), RB_EV_CLICK) == 1);
    {
        char t[16];
        rb_dom_get_text(rb_find_id("d"), t, sizeof(t));
        CHECK(strcmp(t, "stub") == 0);
    }

    /* on: bodies recorded exactly, in source order, pointing into the buffer */
    rb_set_scripting(1);
    web(html);
    CHECK(rb_script_count() == 3);
    CHECK(rb_script_get(0, &node, &text, &len) == 1);
    CHECK(len == (int)strlen("var a = 1 < 2 && b;") && memcmp(text, "var a = 1 < 2 && b;", (size_t)len) == 0);
    CHECK(text >= html && text < html + strlen(html));
    CHECK(rb_dom_tag_name(node, 0, 0) == 6);
    CHECK(rb_script_get(1, &node, &text, &len) == 1 && len == 0);
    {
        char v[16];
        CHECK(rb_dom_get_attr(node, "src", 3, v, sizeof(v)) == 1 && strcmp(v, "x.js") == 0);
    }
    CHECK(rb_script_get(2, &node, &text, &len) == 1);
    CHECK(len == (int)strlen("document.write('&amp; <b>')") && memcmp(text, "document.write('&amp; <b>')", (size_t)len) == 0);
    CHECK(rb_script_get(3, &node, &text, &len) == 0);
    CHECK(rb_script_get(-1, &node, &text, &len) == 0);

    /* onclick: the stub no longer runs, the attribute is still readable */
    {
        int d = rb_find_id("d");
        char t[16], v[128];
        CHECK(rb_dispatch(d, RB_EV_CLICK) == 0);
        rb_dom_get_text(d, t, sizeof(t));
        CHECK(strcmp(t, "t") == 0);
        CHECK(rb_dom_get_attr(d, "onclick", 7, v, sizeof(v)) == 1 && has_str(v, "textContent"));
    }

    /* a body well past the stub's 4096-byte cap, with < and & in it */
    {
        int n = 0, i;
        n += snprintf(page + n, sizeof(page) - (size_t)n, "<script>");
        for (i = 0; i < 700; i++) n += snprintf(page + n, sizeof(page) - (size_t)n, "if (a<b&&c) x%03d();\n", i % 1000);
        int body_len = n - 8;
        n += snprintf(page + n, sizeof(page) - (size_t)n, "</script><p id=after>ok</p>");
        CHECK(body_len > 4096);
        web(page);
        CHECK(rb_script_count() == 1);
        CHECK(rb_script_get(0, &node, &text, &len) == 1);
        CHECK(len == body_len && memcmp(text, page + 8, (size_t)len) == 0);
        CHECK(rb_find_id("after") != 0);
    }

    /* a script that has a src and a body, and scripts are recorded in rb_open too */
    {
        const char *h2 = "<script src=a.js>inline</script>";
        rb_open(h2, (int)strlen(h2));
        CHECK(rb_script_count() == 1);
        CHECK(rb_script_get(0, &node, &text, &len) == 1 && len == 6 && memcmp(text, "inline", 6) == 0);
    }

    /* sticky across rb_open, then off again: unchanged behaviour */
    web("<p id=p>x</p>");
    CHECK(rb_script_count() == 0);
    rb_set_scripting(0);
    web(html);
    CHECK(rb_script_count() == 0);
    CHECK(rb_dispatch(rb_find_id("d"), RB_EV_CLICK) == 1);
    /* with scripting off only the whitelisted attributes are kept; on keeps all */
    web("<div id=a onfoo=bar data-long=\"0123456789012345678901234567890123456789012345678901234567890123456789\">");
    CHECK(!rb_dom_has_attr(rb_find_id("a"), "onfoo", 5));
    rb_set_scripting(1);
    web("<div id=a onfoo=bar data-long=\"0123456789012345678901234567890123456789012345678901234567890123456789\">");
    CHECK(rb_dom_has_attr(rb_find_id("a"), "onfoo", 5));
    CHECK(rb_dom_attr_len(rb_find_id("a"), "data-long", 9) == 70);
    rb_set_scripting(0);
}

static void test_tag_names(void) {
    char nm[64];
    web("<div id=d><my-widget id=w class=c>x</my-widget><X-Y id=y></X-Y><section id=s></section></div>");
    CHECK(rb_tag_from_name("div", 3) == RB_TAG_DIV);
    CHECK(rb_tag_from_name("DIV", -1) == RB_TAG_DIV);
    CHECK(rb_tag_from_name("Section", 7) == RB_TAG_SECTION);
    CHECK(rb_tag_from_name("my-widget", 9) == RB_TAG_OTHER);
    CHECK(rb_tag_from_name("", 0) == RB_TAG_OTHER);
    CHECK(rb_dom_tag_name(rb_find_id("d"), nm, sizeof(nm)) == 3 && strcmp(nm, "div") == 0);
    CHECK(rb_dom_tag_name(rb_find_id("w"), nm, sizeof(nm)) == 9 && strcmp(nm, "my-widget") == 0);
    CHECK(rb_dom_tag_name(rb_find_id("y"), nm, sizeof(nm)) == 3 && strcmp(nm, "x-y") == 0);
    CHECK(rb_dom_tag_name(rb_find_id("s"), nm, sizeof(nm)) == 7 && strcmp(nm, "section") == 0);
    CHECK(rb_dom_tag_name(1, nm, sizeof(nm)) == 9 && strcmp(nm, "#document") == 0);
    CHECK(rb_dom_tag_name(rb_dom_first(rb_find_id("w")), nm, sizeof(nm)) == 5 && strcmp(nm, "#text") == 0);
    CHECK(rb_dom_tag_name(0, nm, sizeof(nm)) == 0 && nm[0] == 0);
    CHECK(rb_dom_tag_name(99999, nm, sizeof(nm)) == 0);
    CHECK(rb_dom_tag_name(rb_find_id("w"), nm, 4) == 9 && strcmp(nm, "my-") == 0);   /* full length, truncated copy */
    /* every known tag has a name that maps back to it */
    for (int t = 1; t < RB_TAG_OTHER; t++) {
        int e = rb_dom_create_element(t);
        CHECK(e != 0 && rb_dom_tag_name(e, nm, sizeof(nm)) > 0);
        if (e) CHECK(rb_tag_from_name(nm, -1) == t);
    }
    /* created by name */
    int a = rb_dom_create_element_named("Custom-Thing", -1);
    CHECK(a != 0 && rb_node_tag(a) == RB_TAG_OTHER);
    CHECK(rb_dom_tag_name(a, nm, sizeof(nm)) == 12 && strcmp(nm, "custom-thing") == 0);
    int b = rb_dom_create_element_named("SPAN", 4);
    CHECK(b != 0 && rb_node_tag(b) == RB_TAG_SPAN);
    CHECK(rb_dom_create_element_named("", 0) == 0);
    CHECK(rb_dom_create_element_named("a b", 3) == 0);
    CHECK(rb_dom_create_element_named("1a", 2) == 0);
    CHECK(rb_dom_create_element_named(0, 3) == 0);
    /* a created custom element matches the type selector like a parsed one */
    CHECK(rb_dom_append_child(rb_find_id("d"), a));
    CHECK(rb_dom_matches(a, "custom-thing", -1) == 1);
    CHECK(rb_dom_matches(rb_find_id("w"), "custom-thing", -1) == 0);
    CHECK(rb_dom_matches(rb_find_id("w"), "MY-WIDGET", -1) == 1);
    /* serialization keeps the name */
    char h[256];
    rb_dom_get_html(rb_find_id("d"), 0, h, sizeof(h));
    CHECK(has_str(h, "<my-widget id=\"w\" class=\"c\">x</my-widget>"));
    CHECK(has_str(h, "<x-y id=\"y\"></x-y>"));
    CHECK(has_str(h, "<custom-thing></custom-thing>"));
    /* many distinct names do not break anything */
    for (int i = 0; i < 300; i++) {
        char n2[24];
        snprintf(n2, sizeof(n2), "el-%d", i);
        int e = rb_dom_create_element_named(n2, -1);
        CHECK(e != 0);
        if (e && i < 200) CHECK(rb_dom_tag_name(e, nm, sizeof(nm)) == (int)strlen(n2));
        if (e) rb_dom_remove_child(e);
    }
    /* siblings */
    web("<ul id=u><li id=a>1</li><li id=b>2</li><li id=c>3</li></ul>");
    int u = rb_find_id("u");
    CHECK(rb_dom_last(u) == rb_find_id("c"));
    CHECK(rb_dom_prev(rb_find_id("c")) == rb_find_id("b"));
    CHECK(rb_dom_prev(rb_find_id("a")) == 0);
    CHECK(rb_dom_last(rb_find_id("a")) == rb_dom_first(rb_find_id("a")));   /* its text node */
    CHECK(rb_dom_last(rb_dom_first(rb_find_id("a"))) == 0);
    CHECK(rb_dom_last(0) == 0 && rb_dom_prev(0) == 0);
}

static int attr_find(int node, const char *name, char *val, int vcap) {
    char nm[64];
    for (int i = 0; i < rb_dom_attr_count(node); i++) {
        if (rb_dom_attr_at(node, i, nm, sizeof(nm), val, vcap) && strcmp(nm, name) == 0) return i;
    }
    return -1;
}

static void test_generic_attrs(void) {
    char v[300], nm[64];
    web("<style>[data-x=on]{color:#ff0000} [aria-expanded=true]{color:#00ff00}</style>"
        "<p id=p data-x=off title=\"a b\">hello</p><input id=i type=text value=v1 disabled><custom-el id=c></custom-el>");
    int p = rb_find_id("p");
    int base = text_color("hello");
    /* parsed */
    CHECK(rb_dom_get_attr(p, "data-x", 6, v, sizeof(v)) == 1 && strcmp(v, "off") == 0);
    CHECK(rb_dom_get_attr(p, "DATA-X", 6, v, sizeof(v)) == 1 && strcmp(v, "off") == 0);
    CHECK(rb_dom_has_attr(p, "title", 5));
    CHECK(!rb_dom_has_attr(p, "data-y", 6));
    CHECK(rb_dom_get_attr(p, "data-y", 6, v, sizeof(v)) == 0 && v[0] == 0);
    /* set: new, replace, empty value, uppercase name */
    CHECK(rb_dom_set_attr(p, "data-x", 6, "on", 2) == 1);
    CHECK(rb_dom_get_attr(p, "data-x", 6, v, sizeof(v)) == 1 && strcmp(v, "on") == 0);
    rb_relayout();
    CHECK(text_color("hello") == rb_map_rgb(255, 0, 0));
    CHECK(rb_dom_set_attr(p, "Aria-Expanded", -1, "true", -1) == 1);
    CHECK(rb_dom_get_attr(p, "aria-expanded", 13, v, sizeof(v)) == 1 && strcmp(v, "true") == 0);
    rb_relayout();
    CHECK(text_color("hello") == rb_map_rgb(0, 255, 0));    /* same specificity, later rule wins */
    CHECK(rb_dom_set_attr(p, "data-empty", 10, "", 0) == 1);
    CHECK(rb_dom_has_attr(p, "data-empty", 10));
    CHECK(rb_dom_get_attr(p, "data-empty", 10, v, sizeof(v)) == 1 && v[0] == 0);
    CHECK(rb_dom_attr_len(p, "data-empty", 10) == 0);
    CHECK(rb_dom_attr_len(p, "data-nope", 9) == -1);
    CHECK(rb_dom_attr_len(p, "title", 5) == 3);
    /* grow and shrink the same attribute many times */
    for (int i = 0; i < 50; i++) {
        char big[200];
        int bl = 1 + (i * 37) % 150;
        memset(big, 'a' + i % 26, (size_t)bl);
        CHECK(rb_dom_set_attr(p, "data-x", 6, big, bl) == 1);
        CHECK(rb_dom_attr_len(p, "data-x", 6) == bl);
        CHECK(rb_dom_get_attr(p, "data-x", 6, v, sizeof(v)) == 1 && (int)strlen(v) == bl && v[0] == 'a' + i % 26);
        CHECK(rb_dom_has_attr(p, "title", 5) && rb_dom_has_attr(p, "aria-expanded", 13));   /* neighbours survive */
    }
    /* enumerate: dedicated fields first, then the rest; stable */
    rb_dom_set_attr(p, "data-x", 6, "on", 2);
    int n0 = rb_dom_attr_count(p);
    CHECK(n0 == 5);     /* id, data-x, aria-expanded, data-empty, title */
    CHECK(attr_find(p, "id", v, sizeof(v)) == 0 && strcmp(v, "p") == 0);
    CHECK(attr_find(p, "data-x", v, sizeof(v)) >= 0 && strcmp(v, "on") == 0);
    CHECK(attr_find(p, "aria-expanded", v, sizeof(v)) >= 0 && strcmp(v, "true") == 0);
    CHECK(attr_find(p, "title", v, sizeof(v)) >= 0 && strcmp(v, "a b") == 0);
    CHECK(rb_dom_attr_at(p, n0, nm, sizeof(nm), v, sizeof(v)) == 0);
    CHECK(rb_dom_attr_at(p, -1, nm, sizeof(nm), v, sizeof(v)) == 0);
    CHECK(rb_dom_attr_count(0) == 0);
    CHECK(rb_dom_attr_count(rb_dom_first(p)) == 0);     /* a text node */
    CHECK(rb_dom_attr_at(p, 0, nm, 3, v, 1) == 1 && strcmp(nm, "id") == 0 && v[0] == 0);   /* truncation */
    rb_dom_attr_at(p, 0, nm, 2, 0, 0);
    CHECK(strcmp(nm, "i") == 0);
    /* dedicated attributes show up too */
    rb_dom_set_attr(p, "class", 5, "k l", 3);
    rb_dom_set_attr(p, "style", 5, "margin:0", 8);
    rb_dom_set_attr(p, "href", 4, "/h", 2);
    rb_dom_set_attr(p, "hidden", 6, "", 0);
    CHECK(attr_find(p, "class", v, sizeof(v)) >= 0 && strcmp(v, "k l") == 0);
    CHECK(attr_find(p, "style", v, sizeof(v)) >= 0 && strcmp(v, "margin:0") == 0);
    CHECK(attr_find(p, "href", v, sizeof(v)) >= 0 && strcmp(v, "/h") == 0);
    CHECK(attr_find(p, "hidden", v, sizeof(v)) >= 0 && v[0] == 0);
    CHECK(rb_dom_get_attr(p, "hidden", 6, v, sizeof(v)) == 1 && v[0] == 0);
    int before = rb_dom_attr_count(p);
    CHECK(rb_dom_remove_attr(p, "hidden", 6) == 1);
    CHECK(!rb_dom_has_attr(p, "hidden", 6));
    CHECK(rb_dom_attr_count(p) == before - 1);
    /* remove generic */
    CHECK(rb_dom_remove_attr(p, "data-x", 6) == 1);
    CHECK(!rb_dom_has_attr(p, "data-x", 6));
    CHECK(rb_dom_get_attr(p, "data-x", 6, v, sizeof(v)) == 0);
    CHECK(rb_dom_remove_attr(p, "data-x", 6) == 1);       /* absent: still fine */
    CHECK(rb_dom_has_attr(p, "title", 5) && rb_dom_has_attr(p, "aria-expanded", 13));
    rb_dom_remove_attr(p, "aria-expanded", 13);
    rb_relayout();
    CHECK(text_color("hello") == base);                    /* neither rule matches any more */
    /* the input: effective type, disabled, value */
    int in = rb_find_id("i");
    CHECK(attr_find(in, "type", v, sizeof(v)) >= 0 && strcmp(v, "text") == 0);
    CHECK(attr_find(in, "value", v, sizeof(v)) >= 0 && strcmp(v, "v1") == 0);
    CHECK(attr_find(in, "disabled", v, sizeof(v)) >= 0 && v[0] == 0);
    CHECK(rb_dom_remove_attr(in, "disabled", 8) == 1 && !rb_dom_has_attr(in, "disabled", 8));
    CHECK(rb_dom_set_attr(in, "disabled", 8, "", 0) == 1 && rb_dom_has_attr(in, "disabled", 8));
    CHECK(rb_dom_set_attr(in, "type", 4, "checkbox", 8) == 1);
    CHECK(rb_dom_remove_attr(in, "type", 4) == 1 && !rb_dom_has_attr(in, "type", 4));
    CHECK(rb_dom_get_attr(in, "type", 4, v, sizeof(v)) == 1 && strcmp(v, "text") == 0);   /* effective type */
    /* custom element + invalid names */
    int c = rb_find_id("c");
    CHECK(rb_dom_set_attr(c, "x-y", 3, "1", 1) == 1 && rb_dom_has_attr(c, "x-y", 3));
    CHECK(rb_dom_set_attr(c, "bad name", 8, "1", 1) == 0);
    CHECK(rb_dom_set_attr(c, "", 0, "1", 1) == 0);
    CHECK(rb_dom_set_attr(c, "a=b", 3, "1", 1) == 0);
    CHECK(rb_dom_set_attr(0, "a", 1, "1", 1) == 0);
    CHECK(rb_dom_set_attr(c, "a", 1, 0, 3) == 0);
    CHECK(rb_dom_set_attr(c, "a", 1, 0, 0) == 1);
    /* a very large value does not fit the attribute list and is refused, leaving the rest alone */
    {
        static char huge[20000];
        memset(huge, 'z', sizeof(huge));
        CHECK(rb_dom_set_attr(c, "data-huge", 9, huge, sizeof(huge)) == 0);
        CHECK(rb_dom_has_attr(c, "x-y", 3));
    }
    /* onclick and method are real attributes now */
    CHECK(rb_dom_set_attr(c, "onclick", 7, "go()", 4) == 1);
    CHECK(rb_dom_get_attr(c, "onclick", 7, v, sizeof(v)) == 1 && strcmp(v, "go()") == 0);
    CHECK(attr_find(c, "onclick", v, sizeof(v)) >= 0);
    CHECK(rb_dom_remove_attr(c, "onclick", 7) == 1 && !rb_dom_has_attr(c, "onclick", 7));
    web("<form id=f method=get action=/s><input id=fi></form>");
    int f = rb_find_id("f"), fi = rb_find_id("fi");
    CHECK(rb_dom_get_attr(f, "method", 6, v, sizeof(v)) == 1 && strcmp(v, "get") == 0);
    CHECK(!rb_form_post(fi));
    CHECK(rb_dom_set_attr(f, "method", 6, "POST", 4) == 1 && rb_form_post(fi));
    CHECK(rb_dom_get_attr(f, "method", 6, v, sizeof(v)) == 1 && strcmp(v, "POST") == 0);
    CHECK(rb_dom_remove_attr(f, "method", 6) == 1 && !rb_form_post(fi) && !rb_dom_has_attr(f, "method", 6));
    /* a hidden input stays hidden through attribute edits */
    web("<form><input id=h type=hidden name=k value=1></form>");
    int h = rb_find_id("h");
    CHECK(!rb_dom_has_attr(h, "hidden", 6));
    CHECK(attr_find(h, "hidden", v, sizeof(v)) < 0);
    CHECK(rb_dom_set_attr(h, "type", 4, "text", 4) == 1 && rb_is_field(h));
}

static int ids_of(const int *h, int n, char *out, int cap) {
    int w = 0;
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        char id[32];
        rb_dom_get_attr(h[i], "id", 2, id, sizeof(id));
        w += snprintf(out + w, (size_t)(cap - w), "%s%s", i ? "," : "", id);
    }
    return w;
}

static void test_selector_api(void) {
    int h[64];
    char s[256];
    web("<div id=root class=wrap><ul id=list><li id=a class=item>A</li><li id=b class=item selected>B</li>"
        "<li id=c class=\"item last\" data-n=\"7\">C</li></ul><p id=p1>one</p><p id=p2 class=note>two <b id=bold>x</b></p>"
        "<form id=f><input id=i1 type=checkbox checked><input id=i2 disabled><input id=i3 type=text required></form>"
        "<a id=lnk href=/x>l</a><my-el id=me></my-el></div>");
    int root = rb_find_id("root");
    int n;
    n = rb_dom_query(1, "li", -1, h, 64);
    CHECK(n == 3); ids_of(h, n, s, sizeof(s)); CHECK(strcmp(s, "a,b,c") == 0);
    n = rb_dom_query(root, ".item", -1, h, 64);
    CHECK(n == 3);
    n = rb_dom_query(root, "li.item.last", -1, h, 64);
    CHECK(n == 1 && h[0] == rb_find_id("c"));
    /* lists, document order */
    n = rb_dom_query(1, "p.note, li#a, #bold , my-el", -1, h, 64);
    CHECK(n == 4); ids_of(h, n, s, sizeof(s)); CHECK(strcmp(s, "a,p2,bold,me") == 0);
    n = rb_dom_query(1, "#c,#c,li", -1, h, 64);
    CHECK(n == 3);                                     /* no duplicates */
    /* the root itself is excluded, descendants only */
    n = rb_dom_query(root, "div", -1, h, 64);
    CHECK(n == 0);
    n = rb_dom_query(rb_find_id("list"), "li, ul", -1, h, 64);
    CHECK(n == 3);
    /* combinators */
    n = rb_dom_query(1, "ul > li", -1, h, 64); CHECK(n == 3);
    n = rb_dom_query(1, "div li", -1, h, 64); CHECK(n == 3);
    n = rb_dom_query(1, "div > li", -1, h, 64); CHECK(n == 0);
    n = rb_dom_query(1, "li + li", -1, h, 64); CHECK(n == 2);
    n = rb_dom_query(1, "#a ~ li", -1, h, 64); CHECK(n == 2);
    n = rb_dom_query(1, "p > b", -1, h, 64); CHECK(n == 1 && h[0] == rb_find_id("bold"));
    n = rb_dom_query(1, "ul li:not(.last)", -1, h, 64); CHECK(n == 2);
    /* attribute selectors */
    n = rb_dom_query(1, "[data-n]", -1, h, 64); CHECK(n == 1 && h[0] == rb_find_id("c"));
    n = rb_dom_query(1, "[data-n=\"7\"]", -1, h, 64); CHECK(n == 1);
    n = rb_dom_query(1, "[data-n='8']", -1, h, 64); CHECK(n == 0);
    n = rb_dom_query(1, "a[href^=\"/\"]", -1, h, 64); CHECK(n == 1);
    n = rb_dom_query(1, "a[href$=x], a[href*=\",\"]", -1, h, 64); CHECK(n == 1);     /* a comma inside quotes is not a separator */
    n = rb_dom_query(1, "li[class~=last]", -1, h, 64); CHECK(n == 1);
    n = rb_dom_query(1, "[selected]", -1, h, 64); CHECK(n == 1 && h[0] == rb_find_id("b"));
    n = rb_dom_query(1, "input[type=checkbox]", -1, h, 64); CHECK(n == 1);
    /* pseudo-classes */
    n = rb_dom_query(1, "li:first-child", -1, h, 64); CHECK(n == 1 && h[0] == rb_find_id("a"));
    n = rb_dom_query(1, "li:last-child", -1, h, 64); CHECK(n == 1 && h[0] == rb_find_id("c"));
    n = rb_dom_query(1, "li:nth-child(2n+1)", -1, h, 64); CHECK(n == 2);
    n = rb_dom_query(1, "li:nth-child(2)", -1, h, 64); CHECK(n == 1 && h[0] == rb_find_id("b"));
    n = rb_dom_query(1, "input:checked", -1, h, 64); CHECK(n == 1 && h[0] == rb_find_id("i1"));
    n = rb_dom_query(1, "input:disabled", -1, h, 64); CHECK(n == 1 && h[0] == rb_find_id("i2"));
    n = rb_dom_query(1, "input:enabled", -1, h, 64); CHECK(n == 2);
    n = rb_dom_query(1, "input:required", -1, h, 64); CHECK(n == 1 && h[0] == rb_find_id("i3"));
    n = rb_dom_query(1, ":is(p, li):not(.item)", -1, h, 64); CHECK(n == 2);
    n = rb_dom_query(1, "a:link", -1, h, 64); CHECK(n == 1);
    n = rb_dom_query(1, "my-el:empty", -1, h, 64); CHECK(n == 1);
    n = rb_dom_query(1, "*", -1, h, 64); CHECK(n > 10);
    /* total beyond max, and counting only */
    n = rb_dom_query(1, "li", -1, h, 2); CHECK(n == 3 && h[0] == rb_find_id("a") && h[1] == rb_find_id("b"));
    n = rb_dom_query(1, "li", -1, 0, 0); CHECK(n == 3);
    n = rb_dom_query(1, "li", 2, h, 64); CHECK(n == 3);        /* explicit length */
    n = rb_dom_query(1, "lixxx", 2, h, 64); CHECK(n == 3);
    /* valid but never matching */
    n = rb_dom_query(1, "a:hover", -1, h, 64); CHECK(n == 0);
    n = rb_dom_query(1, "input:focus", -1, h, 64); CHECK(n == 0);
    n = rb_dom_query(1, "p::before", -1, h, 64); CHECK(n == 0);
    n = rb_dom_query(1, "a:hover, li", -1, h, 64); CHECK(n == 3);
    /* invalid */
    CHECK(rb_dom_query(1, "", -1, h, 64) == -1);
    CHECK(rb_dom_query(1, "   ", -1, h, 64) == -1);
    CHECK(rb_dom_query(1, "li,", -1, h, 64) == -1);
    CHECK(rb_dom_query(1, ",li", -1, h, 64) == -1);
    CHECK(rb_dom_query(1, "li >", -1, h, 64) == -1);
    CHECK(rb_dom_query(1, "[", -1, h, 64) == -1);
    CHECK(rb_dom_query(1, "[a=", -1, h, 64) == -1);
    CHECK(rb_dom_query(1, "li:nth-child(", -1, h, 64) == -1);
    CHECK(rb_dom_query(1, "$$", -1, h, 64) == -1);
    CHECK(rb_dom_query(1, "..a", -1, h, 64) == -1);
    CHECK(rb_dom_query(0, "li", -1, h, 64) == -1);
    CHECK(rb_dom_query(99999, "li", -1, h, 64) == -1);
    CHECK(rb_dom_query(1, 0, 0, h, 64) == -1);
    /* matches */
    CHECK(rb_dom_matches(rb_find_id("c"), "li.item", -1) == 1);
    CHECK(rb_dom_matches(rb_find_id("c"), "ul > li.last, p", -1) == 1);
    CHECK(rb_dom_matches(rb_find_id("c"), "p", -1) == 0);
    CHECK(rb_dom_matches(rb_find_id("c"), "div .item", -1) == 1);
    CHECK(rb_dom_matches(rb_find_id("c"), "a:hover", -1) == 0);
    CHECK(rb_dom_matches(rb_find_id("c"), "li[", -1) == -1);
    CHECK(rb_dom_matches(rb_dom_first(rb_find_id("c")), "li", -1) == 0);    /* a text node */
    CHECK(rb_dom_matches(1, "*", -1) == 0);                                /* the document */
    CHECK(rb_dom_matches(0, "li", -1) == -1);
    /* closest: inclusive, nearest first */
    CHECK(rb_dom_closest(rb_find_id("bold"), "p", -1) == rb_find_id("p2"));
    CHECK(rb_dom_closest(rb_find_id("bold"), "b, p", -1) == rb_find_id("bold"));
    CHECK(rb_dom_closest(rb_find_id("bold"), "div.wrap", -1) == root);
    CHECK(rb_dom_closest(rb_find_id("bold"), "ul", -1) == 0);
    CHECK(rb_dom_closest(rb_find_id("bold"), "!!", -1) == -1);
    CHECK(rb_dom_closest(rb_dom_first(rb_find_id("bold")), "p", -1) == rb_find_id("p2"));   /* from a text node */
    CHECK(rb_dom_closest(rb_find_id("a"), "li, ul", -1) == rb_find_id("a"));

    /* the cascade's tables are left alone: many distinct selectors, then style still works */
    int atoms0 = rb_debug_stat(3), interned0 = rb_debug_stat(6), str0 = rb_debug_stat(7);
    for (int i = 0; i < 20000; i++) {
        char sel[64];
        int len = snprintf(sel, sizeof(sel), "div.cls%d [data-k%d=\"v%d\"] li:not(.n%d)", i, i, i, i);
        CHECK(rb_dom_query(1, sel, len, h, 8) == 0);
        if (i % 1000 == 0) CHECK(rb_dom_matches(rb_find_id("a"), sel, len) == 0);
    }
    CHECK(rb_debug_stat(3) == atoms0);
    CHECK(rb_debug_stat(6) == interned0 && rb_debug_stat(7) == str0);
    CHECK(!rb_oom());
    CHECK(rb_dom_query(1, "li", -1, h, 64) == 3);
    /* a failing selector rolls back too */
    for (int i = 0; i < 3000; i++) {
        char sel[64];
        int len = snprintf(sel, sizeof(sel), ".z%d [q%d=", i, i);
        CHECK(rb_dom_query(1, sel, len, h, 8) == -1);
    }
    CHECK(rb_debug_stat(6) == interned0 && rb_debug_stat(3) == atoms0 && !rb_oom());
    /* and the page still lays out and styles */
    web("<style>.x{color:#ff0000}</style><p class=x id=q>zz</p>");
    rb_dom_query(1, ".x, .y [data-a=b]", -1, h, 8);
    rb_relayout();
    CHECK(text_color("zz") == rb_map_rgb(255, 0, 0));
    /* a detached subtree is not in the document: not found from the document, found from its own root */
    web("<div id=r><span id=s class=k>t</span></div>");
    int r = rb_find_id("r"), sp = rb_find_id("s");
    CHECK(rb_dom_query(1, ".k", -1, h, 8) == 1);
    CHECK(rb_dom_detach(r) == 1);
    CHECK(rb_dom_query(1, ".k", -1, h, 8) == 0);
    CHECK(rb_dom_query(r, ".k", -1, h, 8) == 1 && h[0] == sp);
    CHECK(rb_dom_matches(sp, "div > .k", -1) == 1);
    CHECK(rb_find_id("s") == 0 && rb_find_id("") == 0);
}

static void test_markup_api(void) {
    char out[2048], v[64];
    rb_set_scripting(1);       /* keep every attribute, as the host does for a script-driven page */
    /* round trip */
    web("<div id=d>old<i>x</i></div><p id=after>z</p>");
    int d = rb_find_id("d");
    const char *frag = "<ul class=\"a b\"><li>one &amp; two</li><li data-k=\"q&quot;r\">x &lt; y &gt; z</li></ul><br><img src=a.png alt=\"A &amp; B\"><hr>"
        "<table id=t1><tr><td>c1<table><tr><td>inner</td></tr></table></td><td>c2</td></tr><tr><td colspan=2>wide</td></tr></table>"
        "<my-thing foo=bar>custom</my-thing><input type=checkbox checked name=n value=\"1\"><span>t</span>";
    CHECK(rb_dom_set_inner_html(d, frag, -1) == 1);
    int n1 = rb_dom_get_html(d, 0, out, sizeof(out));
    CHECK(n1 == (int)strlen(out));
    CHECK(has_str(out, "<ul class=\"a b\"><li>one &amp; two</li><li data-k=\"q&quot;r\">x &lt; y &gt; z</li></ul>"));
    CHECK(has_str(out, "<br><img alt=\"A &amp; B\" src=\"a.png\"><hr>"));
    CHECK(!has_str(out, "</br>") && !has_str(out, "</img>") && !has_str(out, "</hr>") && !has_str(out, "</input>"));
    CHECK(has_str(out, "<table id=\"t1\"><tr><td>c1<table><tr><td>inner</td></tr></table></td><td>c2</td></tr><tr><td colspan=\"2\">wide</td></tr></table>"));
    CHECK(has_str(out, "<my-thing foo=\"bar\">custom</my-thing>"));
    CHECK(has_str(out, "<input name=\"n\" value=\"1\" checked=\"\" type=\"checkbox\">"));
    CHECK(!has_str(out, "old"));
    /* serialize -> parse -> serialize is a fixed point */
    {
        static char out2[2048];
        CHECK(rb_dom_set_inner_html(d, out, n1) == 1);
        int n2 = rb_dom_get_html(d, 0, out2, sizeof(out2));
        CHECK(n2 == n1 && strcmp(out, out2) == 0);
    }
    /* the tree is real: query, attributes, text */
    CHECK(rb_dom_query(d, "td", -1, 0, 0) == 4);
    CHECK(rb_dom_query(d, "table table td", -1, 0, 0) == 1);
    CHECK(rb_dom_children_count(rb_dom_first(d)) == 2);
    {
        int li[4];
        CHECK(rb_dom_query(d, "li", -1, li, 4) == 2);
        rb_dom_text_content(li[1], v, sizeof(v));
        CHECK(strcmp(v, "x < y > z") == 0);
        CHECK(rb_dom_get_attr(li[1], "data-k", 6, v, sizeof(v)) == 1 && strcmp(v, "q\"r") == 0);
    }
    rb_relayout();
    CHECK(text_x("inne") >= 0 && text_x("wide") >= 0);      /* the narrow nested cell wraps */
    /* outer */
    {
        int t1 = rb_find_id("t1");
        int n = rb_dom_get_html(t1, 1, out, sizeof(out));
        CHECK(n > 20 && strncmp(out, "<table id=\"t1\">", 15) == 0 && strcmp(out + n - 8, "</table>") == 0);
        CHECK(rb_dom_get_html(rb_find_id("after"), 1, out, sizeof(out)) == (int)strlen("<p id=\"after\">z</p>") && strcmp(out, "<p id=\"after\">z</p>") == 0);
        /* snprintf-style length and truncation */
        char tiny[8];
        int full = rb_dom_get_html(rb_find_id("after"), 1, tiny, sizeof(tiny));
        CHECK(full == 19 && strcmp(tiny, "<p id=\"") == 0);
        CHECK(rb_dom_get_html(rb_find_id("after"), 1, 0, 0) == 19);
        CHECK(rb_dom_get_html(0, 1, tiny, sizeof(tiny)) == 0);
        /* a text node, outer */
        CHECK(rb_dom_get_html(rb_dom_first(rb_find_id("after")), 1, out, sizeof(out)) == 1 && strcmp(out, "z") == 0);
        /* the document */
        CHECK(rb_dom_get_html(1, 1, out, sizeof(out)) > 20 && has_str(out, "<p id=\"after\">z</p>"));
    }
    /* escaping: text and attributes */
    {
        int e = rb_dom_create_element_named("div", 3);
        int tx = rb_dom_create_text("a < b && c > d \"q\"", -1);
        rb_dom_append_child(e, tx);
        rb_dom_set_attr(e, "title", 5, "x \"y\" <z> & w", -1);
        rb_dom_get_html(e, 1, out, sizeof(out));
        CHECK(strcmp(out, "<div title=\"x &quot;y&quot; &lt;z> &amp; w\">a &lt; b &amp;&amp; c &gt; d \"q\"</div>") == 0);
    }
    /* raw text elements are not escaped, in either direction */
    {
        int st = rb_dom_create_element(RB_TAG_STYLE);
        int tx = rb_dom_create_text("a > b { c: \"d\" } /* & */", -1);
        rb_dom_append_child(st, tx);
        rb_dom_get_html(st, 1, out, sizeof(out));
        CHECK(strcmp(out, "<style>a > b { c: \"d\" } /* & */</style>") == 0);
        int sc = rb_dom_create_element(RB_TAG_SCRIPT);
        rb_dom_append_child(sc, rb_dom_create_text("if (a<b && c>d) x();", -1));
        rb_dom_get_html(sc, 1, out, sizeof(out));
        CHECK(strcmp(out, "<script>if (a<b && c>d) x();</script>") == 0);
    }
    /* textarea content */
    CHECK(rb_dom_set_inner_html(d, "<textarea id=ta name=m>a &amp; b &lt;c&gt;</textarea>", -1) == 1);
    rb_dom_get_html(d, 0, out, sizeof(out));
    CHECK(has_str(out, "<textarea id=\"ta\" name=\"m\">a &amp; b &lt;c&gt;</textarea>"));
    /* fragments are not documents: html/head/body dropped, scripts and styles inert */
    rb_set_scripting(1);
    web("<script>one()</script><div id=d></div><style>p{color:#ff0000}</style><title>T</title>");
    CHECK(rb_script_count() == 1);
    d = rb_find_id("d");
    CHECK(rb_dom_set_inner_html(d, "<html><head><title>Z</title></head><body class=bb><p id=np>kept</p><script>two()</script><style>p{color:#00ff00}</style></body></html>", -1) == 1);
    CHECK(rb_script_count() == 1);
    rb_dom_get_html(d, 0, out, sizeof(out));
    CHECK(has_str(out, "<p id=\"np\">kept</p>") && !has_str(out, "<body") && !has_str(out, "<html") && !has_str(out, "<head"));
    {
        char t[16];
        CHECK(rb_title(t, sizeof(t)) == 1 && strcmp(t, "T") == 0);
    }
    rb_relayout();
    CHECK(text_color("kept") == rb_map_rgb(255, 0, 0));      /* the fragment's <style> did not apply */
    rb_set_scripting(0);
    /* bad inputs, and failure leaves children alone */
    CHECK(rb_dom_set_inner_html(0, "x", 1) == 0);
    CHECK(rb_dom_set_inner_html(1, "x", 1) == 0);
    CHECK(rb_dom_set_inner_html(rb_dom_first(rb_find_id("np")), "x", 1) == 0);       /* a text node */
    CHECK(rb_dom_set_inner_html(d, 0, 3) == 0);
    CHECK(rb_find_id("np") != 0);
    CHECK(rb_dom_set_inner_html(d, "", 0) == 1 && rb_dom_first(d) == 0);
    CHECK(rb_dom_set_inner_html(d, "plain", -1) == 1);
    rb_dom_text_content(d, v, sizeof(v));
    CHECK(strcmp(v, "plain") == 0);
    /* node exhaustion in a fragment: reported, old content kept */
    {
        static char many[200000];
        int w = 0;
        CHECK(rb_dom_set_inner_html(d, "<b id=keep>k</b>", -1) == 1);
        while (w < (int)sizeof(many) - 8) { memcpy(many + w, "<i></i>", 7); w += 7; }
        CHECK(rb_dom_set_inner_html(d, many, w) == 0);
        CHECK(rb_find_id("keep") != 0 && rb_dom_first(d) == rb_find_id("keep"));
        CHECK(rb_dom_set_inner_html(d, "<b id=again>k</b>", -1) == 1 && rb_find_id("again") != 0);   /* no slots leaked */
    }
    /* repeated replacement does not leak nodes */
    {
        int nodes0 = rb_debug_stat(0);
        for (int i = 0; i < 3000; i++) CHECK(rb_dom_set_inner_html(d, "<p>a</p><p>b<b>c</b></p>", -1) == 1);
        CHECK(rb_debug_stat(0) <= nodes0 + 8);
    }
    /* a deep fragment */
    {
        static char deep[40000];
        int w = 0;
        for (int i = 0; i < 3000 && w < (int)sizeof(deep) - 16; i++) { memcpy(deep + w, "<span>", 6); w += 6; }
        CHECK(rb_dom_set_inner_html(d, deep, w) == 1);
        static char dout[40000];
        int n = rb_dom_get_html(d, 0, dout, sizeof(dout));
        CHECK(n > 3000 * 13 - 100);
        CHECK(rb_dom_text_content(d, v, sizeof(v)) == 0);
        rb_dom_query(d, "span", -1, 0, 0);
    }
}

static void test_insert_adjacent(void) {
    char out[512];
    web("<div id=wrap><p id=p>mid</p></div>");
    int p = rb_find_id("p"), wrap = rb_find_id("wrap");
    CHECK(rb_dom_insert_adjacent_html(p, 0, "<i>B</i>t1", -1) == 1);
    CHECK(rb_dom_insert_adjacent_html(p, 1, "<i>AB</i>", -1) == 1);
    CHECK(rb_dom_insert_adjacent_html(p, 2, "<i>BE</i>t2", -1) == 1);
    CHECK(rb_dom_insert_adjacent_html(p, 3, "t3<i>A</i>", -1) == 1);
    rb_dom_get_html(wrap, 0, out, sizeof(out));
    CHECK(strcmp(out, "<i>B</i>t1<p id=\"p\"><i>AB</i>mid<i>BE</i>t2</p>t3<i>A</i>") == 0);
    CHECK(rb_find_id("p") == p);                    /* the anchor is the same node */
    CHECK(rb_dom_insert_adjacent_html(p, 4, "x", 1) == 0);
    CHECK(rb_dom_insert_adjacent_html(p, -1, "x", 1) == 0);
    CHECK(rb_dom_insert_adjacent_html(0, 0, "x", 1) == 0);
    CHECK(rb_dom_insert_adjacent_html(rb_dom_first(rb_dom_first(p)), 2, "x", 1) == 0);   /* a text node */
    CHECK(rb_dom_insert_adjacent_html(wrap, 1, "", 0) == 1);               /* empty is fine */
    /* on an empty element, and at the ends of the parent */
    rb_dom_set_inner_html(wrap, "<b id=e></b>", -1);
    int e = rb_find_id("e");
    CHECK(rb_dom_insert_adjacent_html(e, 1, "1", 1) == 1);
    CHECK(rb_dom_insert_adjacent_html(e, 2, "2", 1) == 1);
    CHECK(rb_dom_insert_adjacent_html(e, 0, "<u>0</u>", -1) == 1);
    CHECK(rb_dom_insert_adjacent_html(e, 3, "<u>3</u>", -1) == 1);
    rb_dom_get_html(wrap, 0, out, sizeof(out));
    CHECK(strcmp(out, "<u>0</u><b id=\"e\">12</b><u>3</u>") == 0);
    /* before/after on the topmost element needs a parent that is an element */
    CHECK(rb_dom_insert_adjacent_html(wrap, 0, "<hr>", -1) == 1);
    int dh = rb_dom_create_element(RB_TAG_DIV);
    CHECK(rb_dom_insert_adjacent_html(dh, 0, "x", 1) == 0);                 /* detached: no parent */
    CHECK(rb_dom_insert_adjacent_html(dh, 2, "<b>x</b>", -1) == 1);
    rb_dom_get_html(dh, 1, out, sizeof(out));
    CHECK(strcmp(out, "<div><b>x</b></div>") == 0);
    /* table context keeps cells */
    web("<table><tr id=r><td>a</td></tr></table>");
    CHECK(rb_dom_insert_adjacent_html(rb_find_id("r"), 2, "<td>b</td><td>c</td>", -1) == 1);
    CHECK(rb_dom_query(1, "td", -1, 0, 0) == 3);
    rb_relayout();
    CHECK(text_x("c") > text_x("b") && text_x("b") > text_x("a"));
}

static void test_detach_and_move(void) {
    char out[256];
    web("<div id=a><p id=a1>1</p><p id=a2>2</p></div><div id=b><p id=b1>x</p></div><input id=in>");
    int a = rb_find_id("a"), b = rb_find_id("b"), a1 = rb_find_id("a1"), a2 = rb_find_id("a2"), b1 = rb_find_id("b1");
    int g1 = rb_dom_generation(a1);
    /* append moves: unlinked from the old parent, same node, same generation */
    CHECK(rb_dom_append_child(b, a1));
    CHECK(rb_dom_parent(a1) == b && rb_dom_first(a) == a2 && rb_dom_next(a1) == 0);
    CHECK(rb_dom_generation(a1) == g1 && rb_find_id("a1") == a1);
    CHECK(rb_dom_prev(a1) == b1 && rb_dom_last(b) == a1);
    /* insert_before moves too, including within the same parent */
    CHECK(rb_dom_insert_before(b, a2, b1));
    CHECK(rb_dom_first(b) == a2 && rb_dom_next(a2) == b1 && rb_dom_next(b1) == a1);
    CHECK(rb_dom_children_count(a) == 0);
    CHECK(rb_dom_insert_before(b, a1, a2));              /* last to first */
    CHECK(rb_dom_first(b) == a1 && rb_dom_last(b) == b1);
    CHECK(rb_dom_insert_before(b, a1, a1) == 1 && rb_dom_first(b) == a1);
    CHECK(rb_dom_insert_before(b, a1, rb_find_id("in")) == 0);       /* ref is not a child of b */
    /* replace moves the new child and recycles the replaced one */
    CHECK(rb_dom_replace_child(b, a2, b1));
    CHECK(rb_dom_generation(b1) == 0 && rb_find_id("b1") == 0);
    CHECK(rb_dom_generation(a2) != 0 && rb_dom_parent(a2) == b);
    rb_dom_get_html(b, 0, out, sizeof(out));
    CHECK(strcmp(out, "<p id=\"a1\">1</p><p id=\"a2\">2</p>") == 0);
    /* cycles and the root are refused */
    CHECK(rb_dom_append_child(a1, b) == 0);                    /* b is an ancestor of a1 */
    CHECK(rb_dom_append_child(b, b) == 0);
    CHECK(rb_dom_insert_before(a1, b, 0) == 0);
    CHECK(rb_dom_replace_child(a1, b, rb_dom_first(a1)) == 0);
    CHECK(rb_dom_append_child(a, 1) == 0 && rb_dom_insert_before(a, 1, 0) == 0);
    CHECK(rb_dom_append_child(rb_dom_first(a1), a2) == 0);     /* a text node is no parent */
    rb_dom_get_html(b, 0, out, sizeof(out));
    CHECK(strcmp(out, "<p id=\"a1\">1</p><p id=\"a2\">2</p>") == 0);   /* unchanged by the refusals */
    CHECK(rb_dom_parent(b) != 0);
    /* detach keeps the nodes alive and reinsertable */
    int g2 = rb_dom_generation(b);
    int g3 = rb_dom_generation(a1);
    CHECK(rb_dom_detach(b) == 1);
    CHECK(rb_dom_parent(b) == 0 && rb_dom_generation(b) == g2 && rb_dom_generation(a1) == g3);
    CHECK(rb_dom_first(b) == a1 && rb_dom_parent(a1) == b);          /* the subtree is intact */
    CHECK(rb_dom_detach(b) == 0);                                    /* no parent any more */
    CHECK(rb_dom_detach(1) == 0 && rb_dom_detach(0) == 0 && rb_dom_detach(99999) == 0);
    CHECK(rb_find_id("a1") == 0 && rb_find_id("b") == 0);
    rb_relayout();
    CHECK(text_x("1") < 0);
    CHECK(rb_dom_append_child(a, b));
    CHECK(rb_find_id("a1") == a1);
    rb_relayout();
    CHECK(text_x("1") >= 0);
    CHECK(rb_dom_insert_before(a, b, 0) == 1);
    /* a detached node can be placed anywhere, and a detached subtree dies with remove_child */
    CHECK(rb_dom_detach(a1) == 1 && rb_dom_append_child(a, a1) && rb_dom_parent(a1) == a);
    /* focus and active element inside a detached subtree are cleared */
    web("<div id=w><input id=f1></div><input id=f2>");
    int f1 = rb_find_id("f1"), w = rb_find_id("w");
    CHECK(rb_dom_focus(f1) == 1 && rb_dom_active_element() == f1);
    CHECK(rb_dom_detach(w) == 1);
    CHECK(rb_dom_active_element() == 0);
    for (int i = 0; i < rb_focus_count(); i++) CHECK(rb_focus_node(i) != f1);
    CHECK(rb_dom_generation(f1) != 0);
    /* a detached subtree can come back where it was */
    CHECK(rb_dom_append_child(rb_find_id("f2") ? rb_dom_parent(rb_find_id("f2")) : 1, w) == 1);
    CHECK(rb_find_id("f1") == f1);
    /* moved nodes keep their attributes and text */
    web("<ul id=u1><li id=m class=k data-z=9>moved</li></ul><ul id=u2></ul>");
    int m = rb_find_id("m");
    CHECK(rb_dom_append_child(rb_find_id("u2"), m));
    CHECK(rb_dom_class_contains(m, "k", 1) && rb_dom_has_attr(m, "data-z", 6));
    rb_relayout();
    CHECK(text_x("moved") >= 0);
}

static int g_nav_seen;
static void on_click_nav(int type, int target, int current, void *user) {
    (void)type; (void)target; (void)current; (void)user;
    rb_request_nav("/next?x=1", -1);
    g_nav_seen++;
}

static void test_nav_and_forms(void) {
    char v[64];
    web("<a id=a href=/x>l</a>");
    rb_request_nav("/go", 3);
    CHECK(rb_take_nav(v, sizeof(v)) == 1 && strcmp(v, "/go") == 0);
    CHECK(rb_take_nav(v, sizeof(v)) == 0);
    rb_request_nav("/abcdef", 3);
    CHECK(rb_take_nav(v, sizeof(v)) == 1 && strcmp(v, "/ab") == 0);
    rb_request_nav("http://h/p?q=1", -1);
    CHECK(rb_take_nav(v, 5) == 1 && strcmp(v, "http") == 0);
    rb_request_nav("", 0);
    CHECK(rb_take_nav(v, sizeof(v)) == 0);
    rb_request_nav(0, 0);
    CHECK(rb_take_nav(v, sizeof(v)) == 0);
    /* from inside a listener: the dispatch reports a navigation */
    rb_listen(rb_find_id("a"), RB_EV_CLICK, on_click_nav, 0);
    CHECK(rb_dispatch(rb_find_id("a"), RB_EV_CLICK) == 2 && g_nav_seen == 1);
    CHECK(rb_take_nav(v, sizeof(v)) == 1);
    /* the first request wins over the link's default only if made earlier; later ones replace */
    {
        static char longurl[600];
        memset(longurl, 'u', sizeof(longurl) - 1);
        longurl[sizeof(longurl) - 1] = 0;
        rb_request_nav(longurl, -1);
        char big[600];
        CHECK(rb_take_nav(big, sizeof(big)) == 1 && strlen(big) == 479);
    }

    /* values */
    web("<form id=f><input id=t value=\"abc\"><input id=e><textarea id=ta>hi there</textarea>"
        "<input id=cb type=checkbox><input id=cv type=checkbox value=yes checked>"
        "<input id=r1 type=radio name=g checked><input id=r2 type=radio name=g>"
        "<select id=s><option value=1>One</option><option value=2 selected>Two</option><option>Three</option></select>"
        "<select id=s2><option>Alpha</option><option>Beta</option></select>"
        "<button id=bt value=go>b</button></form><div id=dv>x</div>");
    int t = rb_find_id("t");
    CHECK(rb_dom_get_value(t, v, sizeof(v)) == 3 && strcmp(v, "abc") == 0);
    CHECK(rb_dom_get_value(rb_find_id("e"), v, sizeof(v)) == 0 && v[0] == 0);
    CHECK(rb_dom_get_value(rb_find_id("ta"), v, sizeof(v)) == 8 && strcmp(v, "hi there") == 0);
    CHECK(rb_dom_get_value(rb_find_id("cb"), v, sizeof(v)) == 2 && strcmp(v, "on") == 0);
    CHECK(rb_dom_get_value(rb_find_id("cv"), v, sizeof(v)) == 3 && strcmp(v, "yes") == 0);
    CHECK(rb_dom_get_value(rb_find_id("s"), v, sizeof(v)) == 1 && strcmp(v, "2") == 0);
    CHECK(rb_dom_get_value(rb_find_id("s2"), v, sizeof(v)) == 5 && strcmp(v, "Alpha") == 0);     /* option text, first */
    CHECK(rb_dom_get_value(rb_find_id("bt"), v, sizeof(v)) == 2 && strcmp(v, "go") == 0);
    CHECK(rb_dom_get_value(rb_find_id("dv"), v, sizeof(v)) == -1);
    CHECK(rb_dom_get_value(0, v, sizeof(v)) == -1 && rb_dom_get_value(rb_dom_first(rb_find_id("dv")), v, sizeof(v)) == -1);
    CHECK(rb_dom_get_value(t, v, 2) == 3 && strcmp(v, "a") == 0);                              /* full length, truncated copy */
    /* typing changes what get_value sees */
    CHECK(rb_type_char(t, 'd') == 1);
    CHECK(rb_dom_get_value(t, v, sizeof(v)) == 4 && strcmp(v, "abcd") == 0);
    /* set */
    CHECK(rb_dom_set_value(t, "new value", -1) == 1);
    CHECK(rb_dom_get_value(t, v, sizeof(v)) == 9 && strcmp(v, "new value") == 0);
    CHECK(rb_dom_set_value(t, "", 0) == 1 && rb_dom_get_value(t, v, sizeof(v)) == 0);
    {
        static char lng[500];
        memset(lng, 'q', sizeof(lng));
        CHECK(rb_dom_set_value(t, lng, sizeof(lng)) == 1);
        static char back[600];
        CHECK(rb_dom_get_value(t, back, sizeof(back)) == 500 && back[499] == 'q' && back[500] == 0);
    }
    CHECK(rb_dom_set_value(rb_find_id("ta"), "multi\nline", -1) == 1);
    CHECK(rb_dom_get_value(rb_find_id("ta"), v, sizeof(v)) == 10 && strcmp(v, "multi\nline") == 0);
    CHECK(rb_dom_set_value(rb_find_id("dv"), "x", 1) == 0 && rb_dom_set_value(0, "x", 1) == 0);
    CHECK(rb_dom_set_value(t, 0, 4) == 0);
    int s = rb_find_id("s");
    CHECK(rb_dom_set_value(s, "1", 1) == 1);
    CHECK(rb_dom_get_value(s, v, sizeof(v)) == 1 && strcmp(v, "1") == 0);
    CHECK(rb_dom_set_value(s, "Three", -1) == 1);                       /* by the option's text when it has no value */
    CHECK(rb_dom_get_value(s, v, sizeof(v)) == 5 && strcmp(v, "Three") == 0);
    CHECK(rb_dom_set_value(s, "nope", -1) == 0);
    CHECK(rb_dom_get_value(s, v, sizeof(v)) == 1 && strcmp(v, "1") == 0);       /* nothing selected: the first shows */
    CHECK(rb_dom_set_value(s, "2", 1) == 1 && rb_dom_query(s, "option[selected]", -1, 0, 0) == 1);
    /* the select's selection is rendered */
    rb_relayout();
    CHECK(text_x("Two") >= 0);
    /* it is submitted-visible state: the attribute selector sees it */
    CHECK(rb_dom_matches(rb_dom_first(s) ? rb_dom_next(rb_dom_first(s)) : 0, "option[selected]", -1) == 1);
    /* the button */
    CHECK(rb_dom_set_value(rb_find_id("bt"), "stop", 4) == 1 && rb_dom_get_value(rb_find_id("bt"), v, sizeof(v)) == 4);

    /* checked */
    int cb = rb_find_id("cb"), cv = rb_find_id("cv"), r1 = rb_find_id("r1"), r2 = rb_find_id("r2");
    CHECK(rb_dom_get_checked(cb) == 0 && rb_dom_get_checked(cv) == 1 && rb_dom_get_checked(r1) == 1);
    CHECK(rb_dom_set_checked(cb, 1) == 1 && rb_dom_get_checked(cb) == 1);
    CHECK(rb_dom_has_attr(cb, "checked", 7) && rb_dom_matches(cb, ":checked", -1) == 1);
    CHECK(rb_dom_set_checked(cb, 0) == 1 && rb_dom_get_checked(cb) == 0 && !rb_dom_has_attr(cb, "checked", 7));
    CHECK(rb_dom_set_checked(r2, 1) == 1 && rb_dom_get_checked(r2) == 1 && rb_dom_get_checked(r1) == 0);   /* group */
    CHECK(rb_dom_set_checked(r1, 1) == 1 && rb_dom_get_checked(r1) == 1 && rb_dom_get_checked(r2) == 0);
    CHECK(rb_dom_get_checked(t) == 0 && rb_dom_set_checked(t, 1) == 0 && rb_dom_get_checked(t) == 0);
    CHECK(rb_dom_set_checked(0, 1) == 0 && rb_dom_get_checked(0) == 0);
    /* a click toggle is visible to the script too */
    rb_relayout();
    rb_dispatch(cb, RB_EV_CLICK);
    CHECK(rb_dom_get_checked(cb) == 1);
}

/* The space between two words of a link belongs to the link: each word is
   its own box, and a click exactly on the boundary used to fall through. */
static void test_link_word_gap(void) {
    web("<p>Go: <a id=l href=next.html>Next page here</a> end <a id=m href=o.html>other</a></p>");
    int link = rb_find_id("l"), first = -1, second = -1, last = -1;
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        if (b->kind != RB_BOX_TEXT || b->node != link) continue;
        if (first < 0) first = i; else if (second < 0) second = i;
        last = i;
    }
    CHECK(first >= 0 && second >= 0 && last > second);
    if (first < 0 || second < 0) return;
    const RbBox *a = rb_box(first), *b = rb_box(second), *z = rb_box(last);
    CHECK(b->x > a->x + a->w);                              /* there is a gap */
    CHECK(rb_hit(a->x + a->w - 1, a->y + 1) == link);       /* last pixel of "Next" */
    CHECK(rb_hit(a->x + a->w, a->y + 1) == link);           /* the boundary itself */
    CHECK(rb_hit(b->x - 1, a->y + a->h - 1) == link);
    CHECK(rb_hit(a->x - 1, a->y + 1) != link);              /* before the link: "Go:" is not it */
    CHECK(rb_hit(z->x + z->w, z->y + 1) != link);           /* after it: the gap to "end" */
    CHECK(rb_hit(a->x + a->w, a->y - 1) != link && rb_hit(a->x + a->w, a->y + a->h) != link);
    CHECK(rb_hit(rb_view_width() - 2, a->y + 1) != link);   /* far right of the row */
}

static void test_text_content(void) {
    char v[128];
    web("<div id=d>a<b>b<i>c</i></b><script id=s></script><style id=st></style>d</div><p id=e></p>");
    int d = rb_find_id("d");
    int sc = rb_find_id("s");
    rb_dom_append_child(sc, rb_dom_create_text("S()", -1));
    rb_dom_append_child(rb_find_id("st"), rb_dom_create_text("ST", -1));
    CHECK(rb_dom_text_content(d, v, sizeof(v)) == 9 && strcmp(v, "abcS()STd") == 0);
    CHECK(rb_dom_text_content(d, 0, 0) == (int)strlen(v));
    /* full length even when truncated, like snprintf */
    char tiny[3];
    int full = rb_dom_text_content(d, tiny, sizeof(tiny));
    CHECK(full == (int)strlen(v) && strcmp(tiny, "ab") == 0);
    /* rb_dom_get_text is the display text: truncated length, no script/style */
    int g = rb_dom_get_text(d, tiny, sizeof(tiny));
    CHECK(g == 2 && strcmp(tiny, "ab") == 0);
    rb_dom_get_text(d, v, sizeof(v));
    CHECK(strcmp(v, "abcd") == 0);
    CHECK(rb_dom_text_content(rb_find_id("e"), v, sizeof(v)) == 0 && v[0] == 0);
    CHECK(rb_dom_text_content(rb_dom_first(d), v, sizeof(v)) == 1 && strcmp(v, "a") == 0);
    CHECK(rb_dom_text_content(0, v, sizeof(v)) == 0);
    /* a large tree */
    {
        char *big = malloc(70000);
        int w = 0;
        for (int i = 0; i < 4000; i++) w += snprintf(big + w, 70000 - (size_t)w, "<p>%04d</p>", i % 10000);
        rb_dom_set_inner_html(rb_find_id("e"), big, w);
        static char buf[20000];
        CHECK(rb_dom_text_content(rb_find_id("e"), buf, sizeof(buf)) == 16000 && strncmp(buf, "000000010002", 12) == 0);
        free(big);
    }
}

/* Random mutations through the scripting API under ASan/UBSan: nothing may crash,
   the serializer must agree with itself, and the tables must stay consistent. */
static unsigned long long srng = 0x9E3779B97F4A7C15ULL;
static unsigned srnd(void) {
    srng ^= srng << 13; srng ^= srng >> 7; srng ^= srng << 17;
    return (unsigned)(srng >> 16);
}

static void test_script_stress(void) {
    static const char *frags[] = {
        "<div class=a><span>t</span></div>", "<p>x<b>y</b>z", "<table><tr><td>1<td>2</table>", "text &amp; more",
        "<ul><li>1<li>2<li><ul><li>deep</ul></ul>", "<my-el data-x=1 aria-hidden=true>c</my-el>", "<input value=v><br><img alt=a>",
        "<select><option value=1>a<option selected>b</select>", "<textarea>t</textarea><button>b</button>", "<<>>&&<!-- c -->",
        "<script>alert(1)</script><style>p{}</style>", "<html><body><p>b</p></body></html>", "", "<a href=/x class='c d'>l</a>",
        "<div><div><div><div><div>n</div></div></div></div></div>", "</p></div></table>stray"
    };
    static const char *sels[] = { "div", ".a", "li", "td, th", "p b", "[data-x]", "ul > li", "li:nth-child(2)", "my-el", "*",
        "input:checked", "a[href^=\"/\"]", ":not(div)", "div div div", "p + p", "x[" };
    static const char *names[] = { "data-a", "aria-label", "class", "id", "style", "href", "value", "checked", "disabled", "hidden", "type", "title", "onclick", "x" };
    rb_set_scripting(1);
    web("<html><body><div id=root class=a><p>a</p><ul><li>1</li><li>2</li></ul><form><input value=q><select><option>1</option></select></form></div></body></html>");
    static char buf[16384];
    static char buf2[16384];
    int all[256];
    for (int it = 0; it < 4000; it++) {
        int n = rb_dom_query(1, "*", -1, all, 256);
        CHECK(n >= 0);
        if (n > 256) n = 256;
        if (n == 0) { rb_dom_set_inner_html(rb_find_id("root") ? rb_find_id("root") : 1, "<p>again</p>", -1); continue; }
        int a = all[srnd() % (unsigned)n], b = all[srnd() % (unsigned)n];
        switch (srnd() % 14) {
        case 0: rb_dom_set_inner_html(a, frags[srnd() % 16], -1); break;
        case 1: rb_dom_insert_adjacent_html(a, (int)(srnd() % 4), frags[srnd() % 16], -1); break;
        case 2: rb_dom_set_attr(a, names[srnd() % 14], -1, (srnd() & 1) ? "v&\"<" : "", -1); break;
        case 3: rb_dom_remove_attr(a, names[srnd() % 14], -1); break;
        case 4: if (rb_dom_detach(a) && (srnd() & 1)) rb_dom_append_child(b, a); break;
        case 5: rb_dom_append_child(b, a); break;
        case 6: rb_dom_insert_before(rb_dom_parent(b), a, b); break;
        case 7: if (rb_dom_parent(b) && rb_dom_parent(a)) rb_dom_replace_child(rb_dom_parent(b), a, b); break;
        case 8: rb_dom_set_value(a, "vv", 2); rb_dom_set_checked(a, (int)(srnd() & 1)); break;
        case 9: {
            int m = rb_dom_query(a, sels[srnd() % 16], -1, all + 128, 64);
            CHECK(m >= -1);
            rb_dom_closest(a, sels[srnd() % 16], -1);
            rb_dom_matches(a, sels[srnd() % 16], -1);
            break;
        }
        case 10: {
            int full = rb_dom_get_html(a, (int)(srnd() & 1), buf, sizeof(buf));
            CHECK(full >= 0);
            if (full < (int)sizeof(buf)) CHECK(full == (int)strlen(buf));
            break;
        }
        case 11: {
            int full = rb_dom_text_content(a, buf, sizeof(buf));
            if (full < (int)sizeof(buf)) CHECK(full == (int)strlen(buf));
            int cnt = rb_dom_attr_count(a);
            for (int i = 0; i < cnt; i++) CHECK(rb_dom_attr_at(a, i, buf, 64, buf2, 64) == 1);
            break;
        }
        case 12: {
            /* get -> set -> get on a node that is attached is a fixed point */
            int full = rb_dom_get_html(a, 0, buf, sizeof(buf));
            if (full < (int)sizeof(buf) && rb_dom_tag_name(a, buf2, 8) > 0 && rb_node_tag(a) == RB_TAG_DIV && a != 1 && rb_dom_parent(a)) {
                int ok = rb_dom_set_inner_html(a, buf, full);
                if (ok) {
                    int f2 = rb_dom_get_html(a, 0, buf2, sizeof(buf2));
                    if (f2 < (int)sizeof(buf2)) CHECK(f2 == full ? strcmp(buf, buf2) == 0 : 1);
                }
            }
            break;
        }
        default: rb_relayout(); rb_dom_rect(a, &all[0], &all[1], &all[2], &all[3]); break;
        }
        if (it % 50 == 0) rb_relayout();
    }
    rb_relayout();
    rb_set_scripting(0);
}

int main(void) {
    test_selectors();
    test_important_and_order();
    test_media_and_supports();
    test_custom_properties();
    test_colors();
    test_truecolor();
    test_viewport();
    test_units();
    test_font_scale();
    test_transform();
    test_parser();
    test_deep_nesting();
    test_lists();
    test_margin_collapse();
    test_inline_block_and_replaced();
    test_tables();
    test_floats();
    test_text_indent();
    test_generated_content();
    test_details_and_hidden();
    test_flex_min_content();
    test_stylesheet_loading();
    test_review_fixes();
    test_nested_flex_budget();
    test_profiles();
    test_sizing_fixes();
    test_script_table();
    test_tag_names();
    test_generic_attrs();
    test_selector_api();
    test_markup_api();
    test_insert_adjacent();
    test_detach_and_move();
    test_nav_and_forms();
    test_text_content();
    test_link_word_gap();
    test_script_stress();
    if (fails) {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    printf("rbrowse web engine checks passed\n");
    return 0;
}
