#include "engine.h"
#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("fail %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        fails++; \
    } \
} while (0)

static void test_focus_hit_and_nav(void) {
    const char *html =
        "<a href=\"/next\">Go</a><button>B</button><input value=\"ab\">";
    rb_open(html, (int)strlen(html));
    CHECK(rb_focus_count() == 3);
    CHECK(rb_node_tag(rb_focus_node(0)) == RB_TAG_A);
    CHECK(rb_node_tag(rb_focus_node(1)) == RB_TAG_BUTTON);
    CHECK(rb_node_tag(rb_focus_node(2)) == RB_TAG_INPUT);
    CHECK(rb_tab(0, 0) == 1);
    CHECK(rb_tab(2, 0) == 0);
    CHECK(rb_tab(0, 1) == 2);
    int x, y, w, h;
    int link = rb_focus_node(0);
    CHECK(rb_first_box(link, &x, &y, &w, &h));
    CHECK(rb_hit(x + 1, y + 1) == link);
    CHECK(rb_hit(x + w + 30, y + h + 40) != link);
    CHECK(rb_click(x + 1, y + 1) == link);
    char nav[64];
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/next") == 0);
    int field = rb_focus_node(2);
    CHECK(rb_is_field(field));
    CHECK(rb_type_char(field, 'c'));
    char text[16];
    CHECK(rb_collect_text(field, text, (int)sizeof(text)) == 3);
    CHECK(strcmp(text, "abc") == 0);
    CHECK(rb_backspace(field));
    CHECK(rb_collect_text(field, text, (int)sizeof(text)) == 2);
    CHECK(strcmp(text, "ab") == 0);

    const char *block = "<a href=\"/b\" style=\"display:block\">Block</a>";
    rb_open(block, (int)strlen(block));
    link = rb_focus_node(0);
    CHECK(rb_first_box(link, &x, &y, &w, &h));
    CHECK(rb_hit(x + 1, y + 1) == link);
}

static void test_css_cascade(void) {
    const char *html =
        "<style>p{color:#0000ff}.hot{color:#ff0000}#name{color:#00ff00}</style>"
        "<p id=\"name\" class=\"hot\">Hi</p>";
    rb_open(html, (int)strlen(html));
    int node = rb_find_id("name");
    CHECK(node > 0);
    CHECK(rb_node_color(node) == rb_map_rgb(0, 255, 0));
    char text[16];
    CHECK(rb_collect_text(node, text, (int)sizeof(text)) == 2);
    CHECK(strcmp(text, "Hi") == 0);

    const char *inline_html =
        "<style>.hot{color:#ff0000}</style>"
        "<p id=\"p\" class=\"hot\" style=\"color:#0000ff\">X</p>";
    rb_open(inline_html, (int)strlen(inline_html));
    node = rb_find_id("p");
    CHECK(rb_node_color(node) == rb_map_rgb(0, 0, 255));
}

static void test_script_and_hidden(void) {
    const char *html =
        "<p id=\"t\">old</p>"
        "<a href=\"/hide\" style=\"display:none\">Hidden</a>"
        "<a href=\"/show\">Shown</a>"
        "<script>document.getElementById('t').textContent = 'new';</script>";
    rb_open(html, (int)strlen(html));
    int node = rb_find_id("t");
    char text[16];
    CHECK(rb_collect_text(node, text, (int)sizeof(text)) == 3);
    CHECK(strcmp(text, "new") == 0);
    CHECK(rb_focus_count() == 1);
    CHECK(rb_node_href(rb_focus_node(0), text, (int)sizeof(text)));
    CHECK(strcmp(text, "/show") == 0);
}

static void test_script_heavy_page(void) {
    char src[8000];
    int n = 0;
    const char *pre = "<html><head><title>Hi</title><script>";
    while (*pre) src[n++] = *pre++;
    for (int i = 0; i < 5000; i++) src[n++] = 'x';
    const char *post =
        "</script></head><style>body{color:#000000;background:#000000}p{display:none}</style>"
        "<body><h1>Hello</h1><p>Visible copy</p><a href=\"/go\">Open</a></body></html>";
    while (*post) src[n++] = *post++;
    src[n] = 0;

    rb_open(src, 2000);
    int hidden = 0;
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        if (b->kind == RB_BOX_TEXT && b->tlen) hidden++;
    }
    CHECK(hidden == 0);

    char dst[2048];
    int m = rb_compact_markup(src, n, dst, (int)sizeof(dst));
    CHECK(m > 0);
    CHECK(strstr(dst, "Visible copy") != 0);
    CHECK(strstr(dst, "<script") == 0);
    CHECK(strstr(dst, "<style") == 0);
    rb_open(dst, m);
    char text[160];
    CHECK(rb_collect_text(1, text, (int)sizeof(text)) > 0);
    CHECK(strstr(text, "Hello") != 0);
    CHECK(strstr(text, "Visible copy") != 0);
    CHECK(rb_focus_count() >= 1);

    int bg = rb_page_bg();
    /* colours are 0xRRGGBB (they were palette indices before the truecolor
       painter; this used rb_cube_rgb to decode one) */
    int br = RB_R((RbColor)bg), bgg = RB_G((RbColor)bg), bb = RB_B((RbColor)bg);
    CHECK(br + bgg + bb > 40);
    int painted = 0;
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        int len = 0;
        const char *t = rb_box_text(i, &len);
        if (b->kind != RB_BOX_TEXT || len <= 0) continue;
        CHECK(b->color != (RbColor)bg);
        CHECK(b->w > 0 && b->h > 0);
        if (len >= 5 && memcmp(t, "Hello", 5) == 0) painted++;
    }
    CHECK(painted == 1);
}

static int box_eq(int i, const char *s) {
    int len = 0;
    const char *t = rb_box_text(i, &len);
    int n = (int)strlen(s);
    return len == n && memcmp(t, s, (size_t)n) == 0;
}

static int find_text(const char *s, int *x, int *y, int *w, int *h) {
    for (int i = 0; i < rb_box_count(); i++) {
        if (!box_eq(i, s)) continue;
        const RbBox *b = rb_box(i);
        *x = b->x;
        *y = b->y;
        *w = b->w;
        *h = b->h;
        return 1;
    }
    *x = *y = *w = *h = -1;
    return 0;
}

static void test_inline_row_fields(void) {
    const char *html =
        "<div style=\"padding:4px\">"
        "<div><div><a href=\"/g\">Gmail</a></div><div><a href=\"/i\">Grafika</a></div></div>"
        "<a href=\"/z\">Zaloguj si&#281;</a>"
        "</div>"
        "<input type=\"hidden\" name=\"ie\" value=\"ISO-8859-1\">"
        "<input value=\"pl\" name=\"hl\" type=\"hidden\">"
        "<input name=\"source\" type=\"hidden\" value=\"hp\">"
        "<input name=\"biw\" type=\"hidden\">"
        "<div hidden>Secret</div>"
        "<div style=\"display:none\">Nope</div>"
        "<span style=\"visibility:hidden\">Invis</span>"
        "<div style=\"height:8px\">"
        "<input name=\"q\" style=\"padding-right:38px;height:400px;width:5000px\">"
        "</div>"
        "<input type=\"submit\" value=\"Szukaj w Google\">";
    rb_open(html, (int)strlen(html));

    int x1, y1, w1, h1, x2, y2, w2, h2, x3, y3, w3, h3;
    CHECK(find_text("Gmail", &x1, &y1, &w1, &h1));
    CHECK(find_text("Grafika", &x2, &y2, &w2, &h2));
    CHECK(find_text("Zaloguj", &x3, &y3, &w3, &h3));
    CHECK(y1 == y2 && y2 == y3);
    CHECK(x2 >= x1 + w1);
    CHECK(x3 >= x2 + w2);
    CHECK(y1 + h1 <= y2 + h2);

    for (int i = 0; i < rb_box_count(); i++) {
        CHECK(!box_eq(i, "ISO-8859-1"));
        CHECK(!box_eq(i, "pl"));
        CHECK(!box_eq(i, "hp"));
        CHECK(!box_eq(i, "Secret"));
        CHECK(!box_eq(i, "Nope"));
        CHECK(!box_eq(i, "Invis"));
        const RbBox *b = rb_box(i);
        if (b->w > 0) CHECK(b->x + b->w <= RB_VIEW_W);
    }

    int field = 0;
    int submit = 0;
    for (int i = 0; i < rb_focus_count(); i++) {
        int n = rb_focus_node(i);
        if (rb_node_tag(n) == RB_TAG_INPUT && rb_is_field(n)) field = n;
        if (rb_node_tag(n) == RB_TAG_INPUT && !rb_is_field(n)) submit = n;
    }
    CHECK(field > 0 && submit > 0);
    int fx, fy, fw, fh, sx, sy, sw, sh;
    CHECK(rb_first_box(field, &fx, &fy, &fw, &fh));
    CHECK(rb_first_box(submit, &sx, &sy, &sw, &sh));
    CHECK(fh >= 8 && fh <= 16);
    CHECK(fw > 8 && fw <= RB_VIEW_W);
    CHECK(sy >= fy + fh);
    CHECK(find_text("Szukaj", &x1, &y1, &w1, &h1));
    CHECK(find_text("Google", &x2, &y2, &w2, &h2));
    CHECK(y1 >= fy + fh && y2 >= fy + fh);
    CHECK(y1 + h1 <= y2 || (y1 == y2 && x2 >= x1 + w1));

    const char *wrap =
        "<p>abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz</p>";
    rb_open(wrap, (int)strlen(wrap));
    CHECK(rb_box_count() >= 2);
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *a = rb_box(i);
        if (a->kind != RB_BOX_TEXT || a->h <= 0) continue;
        CHECK(a->x + a->w <= RB_VIEW_W);
        for (int j = i + 1; j < rb_box_count(); j++) {
            const RbBox *b = rb_box(j);
            if (b->kind != RB_BOX_TEXT || b->h <= 0) continue;
            int overlap_x = a->x < b->x + b->w && b->x < a->x + a->w;
            int overlap_y = a->y < b->y + b->h && b->y < a->y + a->h;
            CHECK(!(overlap_x && overlap_y));
        }
    }

    const char *pct =
        "<input style=\"width:100%;height:900px\">"
        "<div style=\"width:50%\"><p>abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz</p></div>";
    rb_open(pct, (int)strlen(pct));
    int px, py, pw, ph;
    CHECK(rb_first_box(rb_focus_node(0), &px, &py, &pw, &ph));
    CHECK(ph >= 8 && ph <= 16);
    CHECK(pw > RB_VIEW_W / 2 && pw <= RB_VIEW_W);
    int slices = 0;
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        if (b->kind != RB_BOX_TEXT || b->tlen <= 0) continue;
        CHECK(b->x + b->w <= RB_VIEW_W / 2 + 8);
        slices++;
    }
    CHECK(slices >= 2);
}

static void test_resolve(void) {
    char out[128];
    CHECK(rb_resolve("/home/user/rbrowse.html", "hello.html", out, (int)sizeof(out)));
    CHECK(strcmp(out, "/home/user/hello.html") == 0);
    CHECK(rb_resolve("http://example.com/a/b.html", "/x", out, (int)sizeof(out)));
    CHECK(strcmp(out, "http://example.com/x") == 0);
    CHECK(rb_resolve("http://example.com/a/b.html", "c.html", out, (int)sizeof(out)));
    CHECK(strcmp(out, "http://example.com/a/c.html") == 0);
    CHECK(rb_resolve("/home/user/a.html", "#top", out, (int)sizeof(out)) == 0);
    CHECK(rb_resolve("/home/user/a.html", "file://-/home/user/hello.html", out, (int)sizeof(out)));
    CHECK(strcmp(out, "/home/user/hello.html") == 0);
    CHECK(rb_resolve("http://example.com/a/b.html", "//other.example/x", out, (int)sizeof(out)));
    CHECK(strcmp(out, "http://other.example/x") == 0);
    CHECK(rb_resolve("https://example.com/a", "//other.example:8443/y", out, (int)sizeof(out)));
    CHECK(strcmp(out, "https://other.example:8443/y") == 0);
    /* Scheme-relative keeps the query too (it used to be dropped by the early
       return) while the fragment is stripped like every other resolve. */
    CHECK(rb_resolve("http://example.com/a/b.html", "//other.example/x?q=2#f", out, (int)sizeof(out)));
    CHECK(strcmp(out, "http://other.example/x?q=2") == 0);
    CHECK(rb_resolve("http://example.com/a/b.html", "//other.example/x#f", out, (int)sizeof(out)));
    CHECK(strcmp(out, "http://other.example/x") == 0);
    /* Query-only hrefs keep the base path: a form with no action, or an
       <a href="?page=2">, must not truncate the page name away. */
    CHECK(rb_resolve("http://example.com/a/b.html", "?q=1", out, (int)sizeof(out)));
    CHECK(strcmp(out, "http://example.com/a/b.html?q=1") == 0);
    CHECK(rb_resolve("http://example.com/a/b.html?x=2#f", "?q=1", out, (int)sizeof(out)));
    CHECK(strcmp(out, "http://example.com/a/b.html?q=1") == 0);
    CHECK(rb_resolve("/home/user/a.html", "?page=2", out, (int)sizeof(out)));
    CHECK(strcmp(out, "/home/user/a.html?page=2") == 0);
}

static void test_style_cache(void) {
    const char *html =
        "<style>p{color:#ff0000;font-size:20px}.hot{color:#aa0000}#cool{color:#00ff00}</style>"
        "<div><div class=\"hot\"><p>one</p><p>two</p><p class=\"hot\">three</p></div>"
        "<p id=\"cool\">four</p><a href=\"/z\">five</a><div><span>six</span></div></div>";
    rb_open(html, (int)strlen(html));
    int hits = 0, misses = 0;
    rb_style_stats(&hits, &misses);
    printf("style cache: hits=%d misses=%d\n", hits, misses);
    CHECK(misses > 0);
    CHECK(hits > misses);
    /* Cache must not change results: cascade and inheritance as before. */
    CHECK(rb_node_color(rb_find_id("cool")) == rb_map_rgb(0, 255, 0));
    CHECK(rb_node_color(rb_find_id("bogus-id")) == 0);
    rb_relayout();
    rb_style_stats(&hits, &misses);
    printf("style cache after relayout: hits=%d misses=%d\n", hits, misses);
    CHECK(hits > misses);
}

static void test_named_entities(void) {
    /* Malformed 6-digit hex (hex_val=-1 at the shift) must be rejected, and a
       3-digit hex still parses. */
    const char *html =
        "<style>.a{color:#ff0000}.b{color:#f00}.c{color:#ff00zz}</style>"
        "<p class=\"a\">A</p><p class=\"b\">B</p><p class=\"c\">C</p>";
    rb_open(html, (int)strlen(html));
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        int len = 0;
        const char *t = rb_box_text(i, &len);
        if (b->kind != RB_BOX_TEXT || len != 1) continue;
        if (t[0] == 'C') CHECK(b->color != rb_map_rgb(255, 0, 0));
        else if (t[0] == 'A' || t[0] == 'B') CHECK(b->color == rb_map_rgb(255, 0, 0));
    }
    const char *ents =
        "<p id=\"e\">A&mdash;B&ndash;C&hellip;D&lsquo;q&rsquo;E&bull;F&divide;G&times;H</p>";
    rb_open(ents, (int)strlen(ents));
    char text[48];
    CHECK(rb_collect_text(rb_find_id("e"), text, (int)sizeof(text)));
    CHECK(strcmp(text, "A-B-C...D'q'E*F/GxH") == 0);
    const char *attr_html =
        "<a id=\"&#65;&#66;\" href=\"/x\">L</a>"
        "<style>.c{color:#00ff00}</style>"
        "<span class=\"&#99;\">y</span>"
        "<p id=\"s\" style=\"&#99;olor:#0000ff\">z</p>";
    rb_open(attr_html, (int)strlen(attr_html));
    CHECK(rb_find_id("AB") > 0);
    CHECK(rb_node_color(rb_find_id("s")) == rb_map_rgb(0, 0, 255));
    int seen_class = 0;
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        if (b->kind == RB_BOX_TEXT && b->tlen == 1 && rb_box_text(i, 0)[0] == 'y') {
            CHECK(b->color == rb_map_rgb(0, 255, 0));
            seen_class = 1;
        }
    }
    CHECK(seen_class == 1);
}

static void test_oom_flag(void) {
    static char page[1 << 20];
    int n = 0;
    const char *frag = "<div><span>x</span></div>";
    int flen = (int)strlen(frag);
    while (n + flen < (int)sizeof(page) - 1) {
        memcpy(page + n, frag, (size_t)flen);
        n += flen;
    }
    page[n] = 0;
    rb_open(page, n);
    CHECK(rb_oom() == 1);
    rb_open("<p>hi</p>", 8);
    CHECK(rb_oom() == 0);
    CHECK(rb_page_height() > 0);
}

static void test_entity_bounds(void) {
    const char *html =
        "<style>#a{position:absolute}</style>"
        "<p id=\"e\">&#65;&#x42;&#999999999999999999999999999;</p>";
    rb_open(html, (int)strlen(html));
    char text[32];
    int n = rb_collect_text(rb_find_id("e"), text, (int)sizeof(text));
    CHECK(n == 3);
    CHECK(strcmp(text, "AB?") == 0);
}

static void test_resolve_bounds(void) {
    /* collapse_path used to write a fixed 480 bytes regardless of the cap,
       and it prepends a '/' to a base that has none, growing by one byte.
       Guard with canaries after small destination buffers. */
    char href[400];
    for (int i = 0; i < 399; i++) href[i] = 'a';
    href[399] = 0;

    struct { char out[64]; char canary[8]; } b;
    memset(b.canary, 0x7f, sizeof(b.canary));
    /* Non-http base without a leading slash: resolved path keeps no slash,
       so collapse_path's leading '/' pushes the terminator past the cap. */
    CHECK(rb_resolve("root/sub/dir/file.html", href, b.out, (int)sizeof(b.out)));
    CHECK(strlen(b.out) < sizeof(b.out));
    for (int i = 0; i < (int)sizeof(b.canary); i++) CHECK(b.canary[i] == 0x7f);

    struct { char out[32]; char canary[8]; } c;
    memset(c.canary, 0x55, sizeof(c.canary));
    CHECK(rb_resolve("file://-/home/user/deep/nested/path/index.html", "../../x", c.out,
                     (int)sizeof(c.out)));
    CHECK(strlen(c.out) < sizeof(c.out));
    for (int i = 0; i < (int)sizeof(c.canary); i++) CHECK(c.canary[i] == 0x55);
}

/* Compound selectors: [tag][.class][#id] with summed specificity. Combinators,
   attribute selectors, pseudo-classes and '*' must be dropped, not mis-targeted
   to RB_TAG_OTHER as they were before. */
static void test_compound_selectors(void) {
    const char *html =
        "<style>"
        "p{color:#000000}"
        "p.hot{color:#ff0000}"
        ".hot{color:#00ff00}"
        "p#name{color:#0000ff}"
        "#name.big{color:#ffff00}"
        ".absent{color:#123456}"
        "p.extra.dup{color:#654321}"
        "div p{color:#999999}"
        "p[data-x]{color:#aaaaaa}"
        "*{color:#bbbbbb}"
        "</style>"
        "<div><p id=\"name\" class=\"hot big\">A</p></div>"
        "<p id=\"b\" class=\"hot\">B</p>"
        "<p id=\"c\" class=\"extra dup\">C</p>"
        "<p id=\"d\" class=\"nomatch\">D</p>"
        "<span id=\"s\" class=\"hot\">S</span>"
        "<p id=\"e\" data-x=\"1\">E</p>";
    rb_open(html, (int)strlen(html));
    /* #name.big (100+10) beats p#name (101), p.hot (11) and .hot (10). */
    CHECK(rb_node_color(rb_find_id("name")) == rb_map_rgb(255, 255, 0));
    /* p.hot (11) beats .hot (10) on a <p>. */
    CHECK(rb_node_color(rb_find_id("b")) == rb_map_rgb(255, 0, 0));
    /* Two classes now match: p.extra.dup (21) beats p. */
    CHECK(rb_node_color(rb_find_id("c")) == rb_map_rgb(0x65, 0x43, 0x21));
    /* .absent matches nothing; the universal rule loses to p (spec 1 > 0). */
    CHECK(rb_node_color(rb_find_id("d")) == rb_map_rgb(0, 0, 0));
    /* Descendant (div p, 2) beats p, and p[data-x] (11) beats p. */
    CHECK(rb_node_color(rb_find_id("name")) == rb_map_rgb(255, 255, 0));
    CHECK(rb_node_color(rb_find_id("e")) == rb_map_rgb(0xaa, 0xaa, 0xaa));
    /* p.hot is anchored to <p>: a <span> only gets .hot. */
    CHECK(rb_node_color(rb_find_id("s")) == rb_map_rgb(0, 255, 0));
}

/* The per-pass style memo must not leak across layout passes: a script that
   mutates style between passes has to be reflected on the next rb_relayout. */
static void test_style_memo_mutation(void) {
    const char *html =
        "<style>#t{color:#ff0000}</style>"
        "<p id=\"btn\" onclick=\"document.getElementById('t').style.color='#0000ff'\">go</p>"
        "<p id=\"t\">text</p>";
    rb_open(html, (int)strlen(html));
    CHECK(rb_node_color(rb_find_id("t")) == rb_map_rgb(255, 0, 0));
    /* Run the click handler, then lay out again. A stale memo would keep red. */
    CHECK(rb_activate(rb_find_id("btn")) == 1);
    rb_relayout();
    CHECK(rb_node_color(rb_find_id("t")) == rb_map_rgb(0, 0, 255));
}

/* Google-style GET form: hidden inputs and the edited text field must end up
   in the query string, percent-encoded, in document order. */
static void test_form_submit(void) {
    const char *html =
        "<form action=\"/search\" method=\"get\">"
        "<input type=\"hidden\" name=\"ie\" value=\"ISO-8859-1\">"
        "<input type=\"hidden\" name=\"hl\" value=\"pl\">"
        "<input name=\"q\">"
        "<input type=\"submit\" value=\"Szukaj w Google\">"
        "</form>";
    rb_open(html, (int)strlen(html));
    int field = 0, submit = 0;
    for (int i = 0; i < rb_focus_count(); i++) {
        int n = rb_focus_node(i);
        if (rb_node_tag(n) == RB_TAG_INPUT && rb_is_field(n)) field = n;
        if (rb_node_tag(n) == RB_TAG_INPUT && !rb_is_field(n)) submit = n;
    }
    CHECK(field > 0 && submit > 0);
    const char *query = "cats & dogs";
    for (int i = 0; query[i]; i++) CHECK(rb_type_char(field, query[i]));
    /* Clicking/pressing the submit control queues the navigation. */
    CHECK(rb_activate(submit) == 2);
    char nav[256];
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/search?ie=ISO-8859-1&hl=pl&q=cats%20%26%20dogs") == 0);
    /* Enter in the text field does the same. */
    CHECK(rb_activate(field) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/search?ie=ISO-8859-1&hl=pl&q=cats%20%26%20dogs") == 0);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));

    /* No form ancestor: a submit control is a no-op. */
    const char *orphan = "<input type=\"submit\" value=\"Go\">";
    rb_open(orphan, (int)strlen(orphan));
    CHECK(rb_focus_count() == 1);
    CHECK(rb_activate(rb_focus_node(0)) == 0);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));

    /* <button name=...> carries its own name=value when it submits. */
    const char *btn =
        "<form action=\"/go\"><input name=\"a\" value=\"1\">"
        "<button name=\"b\" value=\"2\">Send</button></form>";
    rb_open(btn, (int)strlen(btn));
    int b = 0;
    for (int i = 0; i < rb_focus_count(); i++)
        if (rb_node_tag(rb_focus_node(i)) == RB_TAG_BUTTON) b = rb_focus_node(i);
    CHECK(b > 0);
    CHECK(rb_activate(b) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/go?a=1&b=2") == 0);

    /* No action: the query hangs off the current page. The engine returns a
       query-only href and rb_resolve keeps the base path. */
    const char *noact = "<form><input name=\"q\"><input type=\"submit\"></form>";
    rb_open(noact, (int)strlen(noact));
    int nf = 0, ns = 0;
    for (int i = 0; i < rb_focus_count(); i++) {
        int n = rb_focus_node(i);
        if (rb_node_tag(n) == RB_TAG_INPUT && rb_is_field(n)) nf = n;
        if (rb_node_tag(n) == RB_TAG_INPUT && !rb_is_field(n)) ns = n;
    }
    CHECK(nf > 0 && ns > 0);
    CHECK(rb_type_char(nf, 'a'));
    CHECK(rb_activate(ns) == 2);
    char q[64];
    CHECK(rb_take_nav(q, (int)sizeof(q)));
    CHECK(strcmp(q, "?q=a") == 0);
    char out[128];
    CHECK(rb_resolve("http://example.com/search?hl=pl", q, out, (int)sizeof(out)));
    CHECK(strcmp(out, "http://example.com/search?q=a") == 0);
}

/* Small helper: activate the Nth non-hidden control in document order. */
static int control_at(int index) {
    int seen = -1;
    for (int i = 0; i < rb_focus_count(); i++) {
        int n = rb_focus_node(i);
        if (rb_node_tag(n) == RB_TAG_INPUT || rb_node_tag(n) == RB_TAG_BUTTON ||
            rb_node_tag(n) == RB_TAG_TEXTAREA) {
            if (++seen == index) return n;
        }
    }
    return -1;
}

static void test_form_action_variants(void) {
    char nav[256];

    /* Action that already carries a query: pairs join with '&'. */
    rb_open("<form action=\"/s?ie=1\"><input name=\"q\" value=\"x y\">"
            "<input type=\"submit\"></form>", (int)strlen(
            "<form action=\"/s?ie=1\"><input name=\"q\" value=\"x y\">"
            "<input type=\"submit\"></form>"));
    CHECK(rb_activate(rb_focus_node(1)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/s?ie=1&q=x%20y") == 0);

    /* Fragment is stripped; query+fragment keeps the query as join base. */
    rb_open("<form action=\"/s#f\"><input name=\"q\" value=\"1\">"
            "<input type=\"submit\"></form>", (int)strlen(
            "<form action=\"/s#f\"><input name=\"q\" value=\"1\">"
            "<input type=\"submit\"></form>"));
    CHECK(rb_activate(rb_focus_node(1)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/s?q=1") == 0);
    rb_open("<form action=\"/s?a=1#f\"><input name=\"q\" value=\"2\">"
            "<input type=\"submit\"></form>", (int)strlen(
            "<form action=\"/s?a=1#f\"><input name=\"q\" value=\"2\">"
            "<input type=\"submit\"></form>"));
    CHECK(rb_activate(rb_focus_node(1)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/s?a=1&q=2") == 0);

    /* Characters that must not leak unencoded: = + % & and a space in the
       control name. */
    rb_open("<form action=\"/s\"><input name=\"a b\" value=\"x=y+z%w&q\">"
            "<input type=\"submit\"></form>", (int)strlen(
            "<form action=\"/s\"><input name=\"a b\" value=\"x=y+z%w&q\">"
            "<input type=\"submit\"></form>"));
    CHECK(rb_activate(rb_focus_node(1)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/s?a%20b=x%3Dy%2Bz%25w%26q") == 0);

    /* Empty value still yields the named pair; unnamed controls are skipped. */
    rb_open("<form action=\"/s\"><input value=\"ghost\"><input name=\"q\">"
            "<input type=\"submit\"></form>", (int)strlen(
            "<form action=\"/s\"><input value=\"ghost\"><input name=\"q\">"
            "<input type=\"submit\"></form>"));
    CHECK(rb_activate(rb_focus_node(2)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/s?q=") == 0);

    /* Textarea carries its edited value. */
    rb_open("<form action=\"/t\"><textarea name=\"msg\"></textarea>"
            "<input type=\"submit\" value=\"send\"></form>", (int)strlen(
            "<form action=\"/t\"><textarea name=\"msg\"></textarea>"
            "<input type=\"submit\" value=\"send\"></form>"));
    int tj = control_at(0);
    CHECK(rb_is_field(tj));
    CHECK(rb_type_char(tj, 'a') && rb_type_char(tj, 'b'));
    CHECK(rb_backspace(tj) && rb_type_char(tj, 'c'));
    CHECK(rb_activate(control_at(1)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/t?msg=ac") == 0);
}

static void test_form_forms_and_scopes(void) {
    char nav[256];

    /* Two sibling forms: each submit uses only its own form's controls. */
    const char *two =
        "<form action=\"/a\"><input name=\"a1\" value=\"1\"><input type=\"submit\"></form>"
        "<form action=\"/b\"><input name=\"b1\" value=\"2\"><input type=\"submit\"></form>";
    rb_open(two, (int)strlen(two));
    CHECK(rb_activate(control_at(1)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/a?a1=1") == 0);
    CHECK(rb_activate(control_at(3)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/b?b1=2") == 0);

    /* Nested <form>: the inner form owns its controls; the outer submission
       excludes the inner subtree (a nested form's controls belong to the
       nearest form ancestor) and still carries its own second field. */
    const char *nested =
        "<form action=\"/outer\"><input name=\"o1\" value=\"o\">"
        "<form action=\"/inner\"><input name=\"i1\" value=\"i\">"
        "<input type=\"submit\" value=\"in\"></form>"
        "<input name=\"o2\" value=\"p\">"
        "<input type=\"submit\" value=\"out\"></form>";
    rb_open(nested, (int)strlen(nested));
    CHECK(rb_activate(control_at(2)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/inner?i1=i") == 0);
    CHECK(rb_activate(control_at(4)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/outer?o1=o&o2=p") == 0);

    /* A control after the form is not part of it. */
    rb_open("<form action=\"/x\"><input name=\"a\"></form><input name=\"b\">", (int)strlen(
            "<form action=\"/x\"><input name=\"a\"></form><input name=\"b\">"));
    CHECK(rb_activate(control_at(1)) == 0);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));

    /* Resolved hostile actions: what follow() tests on a remote page. The
       file:// schemes resolve to a local path (blocked there), while
       uppercase trickery stays a harmless remote URL. */
    char out[384];
    CHECK(rb_resolve("http://example.com/a", "file://-/etc/passwd", out, (int)sizeof(out)));
    CHECK(strcmp(out, "/etc/passwd") == 0);
    CHECK(rb_resolve("https://example.com/a", "/etc/passwd", out, (int)sizeof(out)));
    CHECK(strcmp(out, "https://example.com/etc/passwd") == 0);
    CHECK(rb_resolve("http://example.com/a", "FILE://-/etc/passwd", out, (int)sizeof(out)));
    CHECK(strcmp(out, "http://example.com/FILE:/-/etc/passwd") == 0);
    CHECK(rb_resolve("https://example.com/a", " HTTP://other/x", out, (int)sizeof(out)));
    CHECK(strcmp(out, "https://example.com/HTTP:/other/x") == 0);

    /* Query-only against a scheme root gains the implicit '/' path. */
    CHECK(rb_resolve("http://example.com", "?q=1", out, (int)sizeof(out)));
    CHECK(strcmp(out, "http://example.com/?q=1") == 0);
    CHECK(rb_resolve("https://example.com#f", "?q=1", out, (int)sizeof(out)));
    CHECK(strcmp(out, "https://example.com/?q=1") == 0);
}

static void test_form_post_unsupported(void) {
    char nav[256];
    const char *post =
        "<form action=\"/do\" method=\"POST\"><input name=\"q\" value=\"x\">"
        "<input type=\"submit\"></form>";
    rb_open(post, (int)strlen(post));
    int submit = control_at(1);
    CHECK(submit > 0);
    CHECK(rb_form_post(submit) == 1);
    CHECK(rb_form_post(rb_focus_node(0)) == 1);
    /* Refused: no navigation queued, activation stays a no-op. */
    CHECK(rb_activate(submit) == 0);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(rb_activate(rb_focus_node(0)) == 0);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));

    rb_open("<form action=\"/do\" method=\"post\"><input name=\"q\" value=\"x\">"
            "<input type=\"submit\"></form>", (int)strlen(
            "<form action=\"/do\" method=\"post\"><input name=\"q\" value=\"x\">"
            "<input type=\"submit\"></form>"));
    CHECK(rb_form_post(control_at(1)) == 1);

    /* Explicit get keeps submitting. */
    rb_open("<form action=\"/do\" method=\"get\"><input name=\"q\" value=\"x\">"
            "<input type=\"submit\"></form>", (int)strlen(
            "<form action=\"/do\" method=\"get\"><input name=\"q\" value=\"x\">"
            "<input type=\"submit\"></form>"));
    CHECK(rb_form_post(control_at(1)) == 0);
    CHECK(rb_activate(control_at(1)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/do?q=x") == 0);
}

/* Field policy: disabled controls are untabbable and unsubmitted; reset and
   type=button never submit; an image input submits without a pair; and
   checkbox/radio contribute only their checked markup state. */
static void test_form_field_policy(void) {
    char nav[256];

    const char *dis =
        "<form action=\"/s\"><input name=\"a\" value=\"1\" disabled>"
        "<input name=\"b\" value=\"2\"><input type=\"submit\" disabled>"
        "<input type=\"submit\" value=\"go\"></form>";
    rb_open(dis, (int)strlen(dis));
    /* Disabled controls leave the focus order: only b and the live submit. */
    CHECK(rb_focus_count() == 2);
    CHECK(rb_activate(control_at(1)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/s?b=2") == 0);

    const char *off = "<form action=\"/s\"><input name=\"a\" value=\"1\" disabled></form>";
    rb_open(off, (int)strlen(off));
    CHECK(rb_focus_count() == 0);
    CHECK(rb_type_char(control_at(0), 'x') == 0);

    /* reset and type=button are no-ops and are never submitted. */
    const char *rst =
        "<form action=\"/s\"><input name=\"a\" value=\"1\">"
        "<input type=\"reset\" value=\"Clear\">"
        "<input type=\"button\" value=\"B\">"
        "<input type=\"submit\"></form>";
    rb_open(rst, (int)strlen(rst));
    CHECK(rb_activate(control_at(1)) == 0);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(rb_activate(control_at(2)) == 0);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(rb_activate(control_at(3)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/s?a=1") == 0);

    /* image submits, but its value never becomes a pair. */
    const char *img =
        "<form action=\"/s\"><input name=\"a\" value=\"1\">"
        "<input type=\"image\" name=\"go\" value=\"x\"></form>";
    rb_open(img, (int)strlen(img));
    CHECK(rb_activate(control_at(1)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/s?a=1") == 0);

    /* checkbox/radio: named ones submit only when the checked attr is set. */
    const char *chk =
        "<form action=\"/s\">"
        "<input type=\"checkbox\" name=\"c1\" value=\"on\" checked>"
        "<input type=\"checkbox\" name=\"c2\" value=\"on\">"
        "<input type=\"radio\" name=\"r\" value=\"1\" checked>"
        "<input type=\"radio\" name=\"r\" value=\"2\">"
        "<input name=\"q\" value=\"x\"><input type=\"submit\"></form>";
    rb_open(chk, (int)strlen(chk));
    CHECK(rb_is_field(control_at(0)) == 0);
    CHECK(rb_type_char(control_at(0), 'z') == 0);
    CHECK(rb_activate(control_at(5)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/s?c1=on&r=1&q=x") == 0);
    /* Enter on a checkbox/radio is a no-op: it neither toggles nor submits
       its form (pointer clicks still toggle; see test_ev_checkbox_radio). */
    CHECK(rb_activate(control_at(0)) == 0);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(rb_dom_has_attr(control_at(0), "checked", 7));

    /* The same policy applies to the <button> element: reset/button are
       no-ops, a bare <button> (default type=submit) still submits. */
    const char *bref =
        "<form action=\"/s\"><input name=\"a\" value=\"1\">"
        "<button type=\"reset\">R</button><button type=\"button\">B</button>"
        "<button type=\"submit\">S</button></form>";
    rb_open(bref, (int)strlen(bref));
    CHECK(rb_activate(control_at(1)) == 0);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(rb_activate(control_at(2)) == 0);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(rb_activate(control_at(3)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/s?a=1") == 0);

    const char *bdef =
        "<form action=\"/s\"><input name=\"a\" value=\"1\"><button>Go</button></form>";
    rb_open(bdef, (int)strlen(bdef));
    CHECK(rb_activate(control_at(1)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/s?a=1") == 0);
}

/* Unquoted attribute values keep '/' (action=/search, href=/x, value=a/b);
   a self-closing '/>' still ends the value, so <input name=q/> stays q. */
static void test_unquoted_attrs(void) {
    char nav[256];
    const char *page =
        "<form action=/search><input type=hidden name=ie value=ISO-8859-1>"
        "<input name=q><input type=submit value=Szukaj></form>";
    rb_open(page, (int)strlen(page));
    int field = rb_focus_node(0), submit = rb_focus_node(1);
    const char *query = "cats & dogs";
    for (int i = 0; query[i]; i++) CHECK(rb_type_char(field, query[i]));
    CHECK(rb_activate(submit) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/search?ie=ISO-8859-1&q=cats%20%26%20dogs") == 0);

    /* A later unquoted attribute must still be parsed: unquoted method=post
       is seen, so submission is refused. */
    rb_open("<form action=/s method=post><input name=q><input type=submit></form>",
            (int)strlen("<form action=/s method=post><input name=q><input type=submit></form>"));
    CHECK(rb_form_post(rb_focus_node(1)) == 1);
    CHECK(rb_activate(rb_focus_node(1)) == 0);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));

    /* Plain links with unquoted href work again. */
    rb_open("<a href=/x>go</a>", (int)strlen("<a href=/x>go</a>"));
    CHECK(rb_activate(rb_focus_node(0)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/x") == 0);

    /* '/' inside a value is literal. */
    rb_open("<form action=/s><input name=n value=a/b><input type=submit></form>",
            (int)strlen("<form action=/s><input name=n value=a/b><input type=submit></form>"));
    CHECK(rb_activate(rb_focus_node(1)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/s?n=a%2Fb") == 0);

    /* Self-closing slash still ends an unquoted value, so the name stays q. */
    rb_open("<form action=/s><input name=q/><input type=submit></form>",
            (int)strlen("<form action=/s><input name=q/><input type=submit></form>"));
    CHECK(rb_activate(rb_focus_node(1)) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/s?q=") == 0);

    /* <br/> between texts keeps both texts (structure unaffected). */
    rb_open("<p>one<br/>two</p>", (int)strlen("<p>one<br/>two</p>"));
    int seen_one = 0, seen_two = 0;
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        int len = 0;
        const char *t = rb_box_text(i, &len);
        if (b->kind != RB_BOX_TEXT) continue;
        if (len == 3 && memcmp(t, "one", 3) == 0) seen_one = 1;
        if (len == 3 && memcmp(t, "two", 3) == 0) seen_two = 1;
    }
    CHECK(seen_one && seen_two);
}

/* --- mutable DOM core ------------------------------------------------- */

static void test_dom_tree_ops(void) {
    const char *html = "<div id=\"root\"></div>";
    rb_open(html, (int)strlen(html));
    int root = rb_find_id("root");
    CHECK(root > 0);

    int a = rb_dom_create_element(RB_TAG_P);
    int b = rb_dom_create_element(RB_TAG_SPAN);
    int c = rb_dom_create_element(RB_TAG_DIV);
    CHECK(a > 0 && b > 0 && c > 0);
    /* A create is detached until attached. */
    CHECK(rb_dom_parent(a) == 0);
    CHECK(rb_dom_is_text(a) == 0);

    int t = rb_dom_create_text("hi", -1);
    CHECK(t > 0 && rb_dom_is_text(t));
    char tbuf[8];
    CHECK(rb_dom_get_text(t, tbuf, (int)sizeof(tbuf)) == 2);
    CHECK(strcmp(tbuf, "hi") == 0);

    CHECK(rb_dom_append_child(root, a));
    CHECK(rb_dom_append_child(root, b));
    CHECK(rb_dom_insert_before(root, c, b));       /* c before b */
    CHECK(rb_dom_parent(a) == root);
    CHECK(rb_dom_first(root) == a);
    CHECK(rb_dom_next(a) == c);
    CHECK(rb_dom_next(c) == b);
    CHECK(rb_dom_next(b) == 0);
    CHECK(rb_dom_children_count(root) == 3);

    /* replace b with d, in place */
    int d = rb_dom_create_element(RB_TAG_SPAN);
    CHECK(rb_dom_replace_child(root, d, b));
    CHECK(rb_dom_children_count(root) == 3);
    CHECK(rb_dom_next(c) == d);
    CHECK(rb_dom_next(d) == 0);

    /* appendChild moves an existing node to the tail. */
    CHECK(rb_dom_append_child(root, a));
    CHECK(rb_dom_children_count(root) == 3);
    CHECK(rb_dom_next(d) == a);

    /* insert_before with ref 0 appends. */
    int e = rb_dom_create_element(RB_TAG_B);
    CHECK(rb_dom_insert_before(root, e, 0));
    CHECK(rb_dom_next(a) == e);
    CHECK(rb_dom_children_count(root) == 4);

    /* remove unlinks and recycles. */
    CHECK(rb_dom_remove_child(c));
    CHECK(rb_dom_children_count(root) == 3);
    CHECK(rb_dom_first(root) == d);

    /* Guards: bad handles, cycles, wrong parent, text parent. */
    CHECK(rb_dom_append_child(0, a) == 0);
    CHECK(rb_dom_append_child(root, 0) == 0);
    CHECK(rb_dom_append_child(99999, a) == 0);
    CHECK(rb_dom_append_child(root, root) == 0);
    CHECK(rb_dom_append_child(a, root) == 0);      /* ancestor cycle */
    CHECK(rb_dom_append_child(t, a) == 0);         /* text has no children */
    CHECK(rb_dom_remove_child(0) == 0);
    CHECK(rb_dom_remove_child(1) == 0);            /* never the root */
    CHECK(rb_dom_remove_child(99999) == 0);
    CHECK(rb_dom_insert_before(root, a, 12345) == 0);
    CHECK(rb_dom_replace_child(root, a, 12345) == 0);
    CHECK(rb_dom_parent(99999) == 0);
    CHECK(rb_dom_first(99999) == 0);
    CHECK(rb_dom_next(99999) == 0);
    CHECK(rb_dom_children_count(99999) == 0);
}

static void test_dom_recycle(void) {
    const char *html = "<div id=\"root\"></div>";
    rb_open(html, (int)strlen(html));
    int root = rb_find_id("root");
    CHECK(root > 0);
    int ok = 1;
    for (int i = 0; i < 5000; i++) {
        int n = rb_dom_create_element(RB_TAG_DIV);
        if (!n) { ok = 0; break; }
        if (!rb_dom_append_child(root, n)) { ok = 0; break; }
        if (!rb_dom_remove_child(n)) { ok = 0; break; }
    }
    CHECK(ok);                       /* never exhausted MAX_NODES */
    CHECK(rb_dom_children_count(root) == 0);

    /* Deep subtree churn also recycles. */
    ok = 1;
    for (int i = 0; i < 500; i++) {
        int p = rb_dom_create_element(RB_TAG_DIV);
        int q = rb_dom_create_element(RB_TAG_SPAN);
        int r = rb_dom_create_text("x", -1);
        if (!p || !q || !r) { ok = 0; break; }
        if (!rb_dom_append_child(p, q) || !rb_dom_append_child(q, r)) { ok = 0; break; }
        if (!rb_dom_append_child(root, p)) { ok = 0; break; }
        if (!rb_dom_remove_child(p)) { ok = 0; break; }
    }
    CHECK(ok);
    CHECK(rb_dom_children_count(root) == 0);
}

static void test_dom_subtree_remove(void) {
    const char *html = "<div id=\"wrap\"><span>hello</span><b>bye</b></div>";
    rb_open(html, (int)strlen(html));
    int wrap = rb_find_id("wrap");
    int span = rb_dom_first(wrap);
    int b = rb_dom_next(span);
    int tspan = rb_dom_first(span);
    int tb = rb_dom_first(b);
    CHECK(wrap > 0 && span > 0 && b > 0 && tspan > 0 && tb > 0);

    rb_relayout();
    int before = 0;
    for (int i = 0; i < rb_box_count(); i++) {
        int n = rb_box(i)->node;
        if (n == wrap || n == span || n == b || n == tspan || n == tb) before++;
    }
    CHECK(before > 0);

    CHECK(rb_dom_remove_child(wrap));
    /* Recycled slots read back as dead through the public guards. */
    CHECK(rb_dom_first(wrap) == 0);
    CHECK(rb_dom_parent(span) == 0);
    CHECK(rb_dom_get_text(wrap, (char[8]){0}, 8) == 0);

    rb_relayout();
    for (int i = 0; i < rb_box_count(); i++) {
        int n = rb_box(i)->node;
        CHECK(n != wrap && n != span && n != b && n != tspan && n != tb);
    }

    /* Removing a focused node drops it from the tab order immediately. */
    const char *links = "<a href=\"/a\">A</a><a href=\"/b\">B</a>";
    rb_open(links, (int)strlen(links));
    CHECK(rb_focus_count() == 2);
    int f0 = rb_focus_node(0);
    CHECK(f0 > 0);
    CHECK(rb_dom_remove_child(f0));
    CHECK(rb_focus_count() == 1);
    CHECK(rb_focus_node(0) != f0);
}

static void test_dom_set_text_renders(void) {
    const char *html = "<p id=\"t\">old</p>";
    rb_open(html, (int)strlen(html));
    int t = rb_find_id("t");
    char buf[32];
    CHECK(rb_dom_get_text(t, buf, (int)sizeof(buf)) == 3);
    CHECK(strcmp(buf, "old") == 0);
    CHECK(find_text("old", &(int){0}, &(int){0}, &(int){0}, &(int){0}));

    CHECK(rb_dom_set_text(t, "newtext", -1));
    CHECK(rb_dom_get_text(t, buf, (int)sizeof(buf)) == 7);
    CHECK(strcmp(buf, "newtext") == 0);
    rb_relayout();
    int x, y, w, h;
    CHECK(find_text("newtext", &x, &y, &w, &h));
    CHECK(w > 0 && h > 0);

    /* Text node data round-trips, and existing children fall to one text. */
    int tn = rb_dom_create_text("abc", -1);
    CHECK(rb_dom_set_text(tn, "z", -1));
    CHECK(rb_dom_get_text(tn, buf, (int)sizeof(buf)) == 1);
    CHECK(strcmp(buf, "z") == 0);

    const char *multi = "<div id=\"m\"><span>a</span><span>b</span></div>";
    rb_open(multi, (int)strlen(multi));
    int m = rb_find_id("m");
    CHECK(rb_dom_children_count(m) == 2);
    CHECK(rb_dom_set_text(m, "solo", -1));
    CHECK(rb_dom_children_count(m) == 1);
    CHECK(rb_dom_is_text(rb_dom_first(m)));
    CHECK(rb_dom_get_text(m, buf, (int)sizeof(buf)) == 4);
    CHECK(strcmp(buf, "solo") == 0);
}

static void test_dom_id_and_attr(void) {
    const char *html = "<div id=\"host\"></div>";
    rb_open(html, (int)strlen(html));
    int host = rb_find_id("host");
    int n = rb_dom_create_element(RB_TAG_DIV);
    CHECK(rb_dom_append_child(host, n));

    CHECK(rb_dom_set_id(n, "made", -1));
    CHECK(rb_dom_has_attr(n, "id", 2));
    rb_relayout();
    CHECK(rb_find_id("made") == n);

    CHECK(rb_dom_remove_attr(n, "id", 2));
    CHECK(!rb_dom_has_attr(n, "id", 2));
    rb_relayout();
    CHECK(rb_find_id("made") == 0);

    /* Generic attribute path maps to the stored fields. */
    CHECK(rb_dom_set_attr(n, "href", 4, "/x", 2));
    char val[32];
    CHECK(rb_dom_get_attr(n, "href", 4, val, (int)sizeof(val)) == 1);
    CHECK(strcmp(val, "/x") == 0);
    CHECK(rb_dom_has_attr(n, "href", 4));
    CHECK(rb_dom_set_attr(n, "name", 4, "q", 1));
    CHECK(rb_dom_get_attr(n, "name", 4, val, (int)sizeof(val)));
    CHECK(strcmp(val, "q") == 0);
    /* Unknown attributes are stored in the element's attribute list. */
    CHECK(rb_dom_set_attr(n, "data-x", 6, "1", 1) == 1);
    CHECK(rb_dom_get_attr(n, "data-x", 6, val, (int)sizeof(val)) == 1 && strcmp(val, "1") == 0);
    /* Guards. */
    CHECK(rb_dom_get_attr(0, "id", 2, val, (int)sizeof(val)) == 0);
    CHECK(rb_dom_get_attr(n, "id", 2, 0, 0) == 0);
    CHECK(rb_dom_set_attr(0, "id", 2, "x", 1) == 0);
    CHECK(rb_dom_has_attr(99999, "id", 2) == 0);
    CHECK(rb_dom_remove_attr(99999, "id", 2) == 0);
}

static void test_dom_class_ops(void) {
    const char *html = "<div id=\"c\"></div>";
    rb_open(html, (int)strlen(html));
    int n = rb_find_id("c");

    CHECK(rb_dom_add_class(n, "hot", 3));
    CHECK(rb_dom_class_contains(n, "hot", 3));
    CHECK(rb_dom_add_class(n, "big", 3));
    CHECK(rb_dom_class_contains(n, "hot", 3));
    CHECK(rb_dom_class_contains(n, "big", 3));
    /* Idempotent add. */
    CHECK(rb_dom_add_class(n, "hot", 3));
    char cls[32];
    CHECK(rb_dom_get_attr(n, "class", 5, cls, (int)sizeof(cls)));
    CHECK(strcmp(cls, "hot big") == 0);
    CHECK(rb_dom_class_contains(n, "bi", 2) == 0);
    CHECK(rb_dom_class_contains(n, "hot ", 4) == 0);

    CHECK(rb_dom_remove_class(n, "hot", 3));
    CHECK(!rb_dom_class_contains(n, "hot", 3));
    CHECK(rb_dom_class_contains(n, "big", 3));
    CHECK(rb_dom_get_attr(n, "class", 5, cls, (int)sizeof(cls)));
    CHECK(strcmp(cls, "big") == 0);

    CHECK(rb_dom_remove_class(n, "big", 3));
    CHECK(!rb_dom_has_attr(n, "class", 5));

    CHECK(rb_dom_class_contains(99999, "x", 1) == 0);
    CHECK(rb_dom_add_class(0, "x", 1) == 0);
    CHECK(rb_dom_remove_class(0, "x", 1) == 0);
}

static void test_dom_rect(void) {
    const char *html = "<a href=\"/x\">link</a>";
    rb_open(html, (int)strlen(html));
    int link = rb_focus_node(0);
    CHECK(link > 0);
    rb_relayout();
    int x, y, w, h;
    CHECK(rb_dom_rect(link, &x, &y, &w, &h));
    CHECK(w > 0 && h > 0);
    CHECK(x >= 0 && x + w <= RB_VIEW_W + 8);
    CHECK(y >= 0);
    int ow, oh;
    CHECK(rb_dom_offset_width(link, &ow));
    CHECK(rb_dom_offset_height(link, &oh));
    CHECK(ow == w && oh == h);

    /* A detached / boxless node has no rect. */
    int det = rb_dom_create_element(RB_TAG_DIV);
    CHECK(rb_dom_rect(det, &x, &y, &w, &h) == 0);
    CHECK(rb_dom_offset_width(det, &ow) == 0);
    CHECK(rb_dom_offset_height(det, &oh) == 0);
    CHECK(rb_dom_rect(0, &x, &y, &w, &h) == 0);
    CHECK(rb_dom_rect(99999, &x, &y, &w, &h) == 0);
    /* NULL out-pointers are allowed. */
    CHECK(rb_dom_rect(link, 0, 0, 0, 0));
}

static void test_dom_set_text_no_oom(void) {
    const char *html = "<div id=\"t\">hello</div>";
    rb_open(html, (int)strlen(html));
    int t = rb_find_id("t");
    CHECK(!rb_oom());
    for (int i = 0; i < 5000; i++) CHECK(rb_dom_set_text(t, "world", -1));
    CHECK(rb_oom() == 0);
    char buf[16];
    CHECK(rb_dom_get_text(t, buf, (int)sizeof(buf)) == 5);
    CHECK(strcmp(buf, "world") == 0);

    /* Also on a bare text node, repeated in place. */
    int tn = rb_dom_create_text("12345678", -1);
    CHECK(!rb_oom());
    for (int i = 0; i < 5000; i++) CHECK(rb_dom_set_text(tn, "abcdefgh", -1));
    CHECK(rb_oom() == 0);
}

/* ==================== Part 2: events + focus ==================== */

static int ev_log_type[16], ev_log_target[16], ev_log_current[16], ev_log_user[16];
static int ev_log_n;

static void ev_log_reset(void) { ev_log_n = 0; }

static void ev_log_cb(int type, int target, int current, void *user) {
    if (ev_log_n < 16) {
        ev_log_type[ev_log_n] = type;
        ev_log_target[ev_log_n] = target;
        ev_log_current[ev_log_n] = current;
        ev_log_user[ev_log_n] = (int)(intptr_t)user;
        ev_log_n++;
    }
}

static void ev_stop_cb(int type, int target, int current, void *user) {
    (void)type; (void)target; (void)current; (void)user;
    rb_event_stop();
}

static void ev_stop_immediate_cb(int type, int target, int current, void *user) {
    (void)type; (void)target; (void)current; (void)user;
    rb_event_stop_immediate();
}

static void ev_prevent_cb(int type, int target, int current, void *user) {
    (void)type; (void)target; (void)current;
    (*(int *)user)++;
    rb_event_prevent();
}

static void test_ev_registration(void) {
    rb_open("<div><p id=\"a\">x</p></div>", (int)strlen("<div><p id=\"a\">x</p></div>"));
    int a = rb_find_id("a");
    CHECK(rb_listener_count() == 0);
    CHECK(rb_listen(a, RB_EV_CLICK, ev_log_cb, (void *)1));
    CHECK(rb_listen(a, RB_EV_CLICK, ev_log_cb, (void *)2)); /* same node+type twice */
    CHECK(rb_listen(a, RB_EV_KEYDOWN, ev_log_cb, (void *)3));
    CHECK(rb_listener_count() == 3);
    CHECK(rb_event_depth() == 0);
    /* Bad registrations change nothing. */
    CHECK(!rb_listen(0, RB_EV_CLICK, ev_log_cb, 0));
    CHECK(!rb_listen(a, 99, ev_log_cb, 0));
    CHECK(!rb_listen(a, RB_EV_COUNT, ev_log_cb, 0));
    CHECK(!rb_listen(a, RB_EV_CLICK, 0, 0));
    CHECK(rb_listener_count() == 3);
    CHECK(rb_listen_remove(a, RB_EV_KEYDOWN) == 1);
    CHECK(rb_listener_count() == 2);
    CHECK(rb_listen_remove(a, RB_EV_KEYDOWN) == 0);
    /* Removing by node+type drops both click listeners. */
    CHECK(rb_listen_remove(a, RB_EV_CLICK) == 2);
    CHECK(rb_listener_count() == 0);
    CHECK(rb_listen(a, RB_EV_CLICK, ev_log_cb, 0));
    CHECK(rb_listen(a, RB_EV_MOUSEDOWN, ev_log_cb, 0));
    CHECK(rb_listen_remove_all(a) == 2);
    CHECK(rb_listener_count() == 0);
    CHECK(rb_listen_remove_all(a) == 0);
    CHECK(rb_listen_remove_all(0) == 0);
    CHECK(rb_listen_remove(0, RB_EV_CLICK) == 0);
}

static void test_ev_bubble_order(void) {
    const char *html = "<div id=\"r\"><div id=\"p\"><div id=\"t\"></div></div></div>";
    rb_open(html, (int)strlen(html));
    int r = rb_find_id("r"), p = rb_find_id("p"), t = rb_find_id("t");
    ev_log_reset();
    CHECK(rb_listen(r, RB_EV_CLICK, ev_log_cb, (void *)1));
    CHECK(rb_listen(p, RB_EV_CLICK, ev_log_cb, (void *)2));
    CHECK(rb_listen(t, RB_EV_CLICK, ev_log_cb, (void *)3));
    CHECK(rb_dispatch(t, RB_EV_CLICK) == 0);
    CHECK(ev_log_n == 3);
    CHECK(ev_log_target[0] == t && ev_log_current[0] == t && ev_log_user[0] == 3);
    CHECK(ev_log_target[1] == t && ev_log_current[1] == p && ev_log_user[1] == 2);
    CHECK(ev_log_target[2] == t && ev_log_current[2] == r && ev_log_user[2] == 1);
    /* No listeners ran elsewhere and no nav was queued by a plain div click. */
    char none[64];
    CHECK(!rb_take_nav(none, (int)sizeof(none)));
}

static void test_ev_capture_order(void) {
    const char *html = "<div id=\"r\"><div id=\"p\"><div id=\"t\"></div></div></div>";
    rb_open(html, (int)strlen(html));
    int r = rb_find_id("r"), p = rb_find_id("p"), t = rb_find_id("t");
    ev_log_reset();
    CHECK(rb_listen_capture(r, RB_EV_CLICK, ev_log_cb, (void *)1));
    CHECK(rb_listen_capture(p, RB_EV_CLICK, ev_log_cb, (void *)2));
    CHECK(rb_listen(t, RB_EV_CLICK, ev_log_cb, (void *)3));
    CHECK(rb_listen(p, RB_EV_CLICK, ev_log_cb, (void *)4));
    CHECK(rb_dispatch(t, RB_EV_CLICK) == 0);
    CHECK(ev_log_n == 4);
    CHECK(ev_log_user[0] == 1 && ev_log_current[0] == r);   /* capture root */
    CHECK(ev_log_user[1] == 2 && ev_log_current[1] == p);   /* capture parent */
    CHECK(ev_log_user[2] == 3 && ev_log_current[2] == t);   /* bubble target */
    CHECK(ev_log_user[3] == 4 && ev_log_current[3] == p);   /* bubble parent */

    /* A capture listener on the target runs too, before the target's bubble
       listeners: root -> parent capture, target capture, then bubble out. */
    rb_open(html, (int)strlen(html));
    r = rb_find_id("r"); p = rb_find_id("p"); t = rb_find_id("t");
    ev_log_reset();
    CHECK(rb_listen_capture(p, RB_EV_CLICK, ev_log_cb, (void *)6));
    CHECK(rb_listen_capture(t, RB_EV_CLICK, ev_log_cb, (void *)5));
    CHECK(rb_listen(t, RB_EV_CLICK, ev_log_cb, (void *)7));
    CHECK(rb_listen(p, RB_EV_CLICK, ev_log_cb, (void *)8));
    CHECK(rb_dispatch(t, RB_EV_CLICK) == 0);
    CHECK(ev_log_n == 4);
    CHECK(ev_log_user[0] == 6 && ev_log_current[0] == p);   /* capture parent */
    CHECK(ev_log_user[1] == 5 && ev_log_current[1] == t);   /* capture target */
    CHECK(ev_log_user[2] == 7 && ev_log_current[2] == t);   /* bubble target */
    CHECK(ev_log_user[3] == 8 && ev_log_current[3] == p);   /* bubble parent */
}

static void test_ev_stop(void) {
    const char *html = "<div id=\"r\"><div id=\"p\"><div id=\"t\"></div></div></div>";
    rb_open(html, (int)strlen(html));
    int r = rb_find_id("r"), p = rb_find_id("p"), t = rb_find_id("t");

    /* stop(): the current node finishes (both handlers on t), then nothing
       after it runs — the parent's handler never fires. */
    ev_log_reset();
    CHECK(rb_listen(t, RB_EV_CLICK, ev_stop_cb, (void *)1));
    CHECK(rb_listen(t, RB_EV_CLICK, ev_log_cb, (void *)2));
    CHECK(rb_listen(p, RB_EV_CLICK, ev_log_cb, (void *)3));
    CHECK(rb_dispatch(t, RB_EV_CLICK) == 0);
    CHECK(ev_log_n == 1);
    CHECK(ev_log_user[0] == 2 && ev_log_current[0] == t);

    /* stopImmediate(): the second handler on the same node does not run. */
    CHECK(rb_listen_remove_all(t) == 2);
    CHECK(rb_listen_remove_all(p) == 1);
    ev_log_reset();
    CHECK(rb_listen(t, RB_EV_CLICK, ev_log_cb, (void *)1));
    CHECK(rb_listen(t, RB_EV_CLICK, ev_stop_immediate_cb, (void *)2));
    CHECK(rb_listen(t, RB_EV_CLICK, ev_log_cb, (void *)3));
    CHECK(rb_dispatch(t, RB_EV_CLICK) == 0);
    CHECK(ev_log_n == 1);
    CHECK(ev_log_user[0] == 1 && ev_log_current[0] == t);
    (void)r;
}

static void test_ev_prevent(void) {
    const char *html = "<a id=\"a\" href=\"/x\">l</a>";
    rb_open(html, (int)strlen(html));
    int a = rb_find_id("a");
    char nav[64];
    int ran = 0;

    /* A listener still runs when it prevents, but the <a href> click does
       not queue the navigation. */
    CHECK(rb_listen(a, RB_EV_CLICK, ev_prevent_cb, &ran));
    CHECK(rb_dispatch(a, RB_EV_CLICK) == 0);
    CHECK(ran == 1);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(rb_listen_remove_all(a) == 1);

    /* Without prevention the same click queues the nav and returns 2. */
    CHECK(rb_dispatch(a, RB_EV_CLICK) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/x") == 0);
}

static void test_ev_inline_onclick(void) {
    char nav[64];

    /* The script's nav short-circuits: /js-nav wins, the <a href>'s
       /href-nav default is skipped, and the parent's bubble handler (which
       would run after the target's) never fires. */
    const char *html =
        "<div id=\"p\"><a id=\"a\" href=\"/href-nav\" "
        "onclick=\"location.href='/js-nav'\">l</a></div>";
    rb_open(html, (int)strlen(html));
    int a = rb_find_id("a"), p = rb_find_id("p");
    ev_log_reset();
    CHECK(rb_listen(p, RB_EV_CLICK, ev_log_cb, (void *)1));
    CHECK(rb_dispatch(a, RB_EV_CLICK) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/js-nav") == 0);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(ev_log_n == 0);

    /* Inline mutation: onclick that only touches text returns 1. */
    const char *html2 =
        "<div id=\"d\" onclick=\"document.getElementById('o').textContent='m'\">"
        "pre<span id=\"o\">old</span></div>";
    rb_open(html2, (int)strlen(html2));
    int d = rb_find_id("d"), o = rb_find_id("o");
    char buf[16];
    CHECK(rb_dispatch(d, RB_EV_CLICK) == 1);
    CHECK(rb_collect_text(o, buf, (int)sizeof(buf)) == 1);
    CHECK(strcmp(buf, "m") == 0);
}

static void test_ev_checkbox_radio(void) {
    char nav[64];
    const char *html =
        "<form action=\"/s\"><input type=\"checkbox\" id=\"c\">"
        "<input type=\"radio\" name=\"g\" id=\"r1\" checked>"
        "<input type=\"radio\" name=\"g\" id=\"r2\">"
        "<input type=\"submit\"></form>";
    rb_open(html, (int)strlen(html));
    int c = rb_find_id("c"), r1 = rb_find_id("r1"), r2 = rb_find_id("r2");
    CHECK(!rb_dom_has_attr(c, "checked", 7));
    CHECK(rb_dispatch(c, RB_EV_CLICK) == 1);
    CHECK(rb_dom_has_attr(c, "checked", 7));
    CHECK(rb_dispatch(c, RB_EV_CLICK) == 1);
    CHECK(!rb_dom_has_attr(c, "checked", 7));

    /* Radio click picks its group: r2 checked, sibling r1 of the same name
       cleared. */
    CHECK(rb_dom_has_attr(r1, "checked", 7));
    CHECK(!rb_dom_has_attr(r2, "checked", 7));
    CHECK(rb_dispatch(r2, RB_EV_CLICK) == 1);
    CHECK(rb_dom_has_attr(r2, "checked", 7));
    CHECK(!rb_dom_has_attr(r1, "checked", 7));

    /* The submit reflects the engine's radio state: only r2 (no value, so a
       bare name) is in the query. */
    int submit = 0;
    for (int i = 0; i < rb_focus_count(); i++) {
        int n = rb_focus_node(i);
        if (rb_node_tag(n) == RB_TAG_INPUT && !rb_is_field(n)) submit = n;
    }
    CHECK(rb_activate(submit) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/s?g=") == 0);

    /* A disabled checkbox fires nothing and does not toggle. */
    CHECK(rb_dom_set_attr(c, "disabled", 8, "", 0));
    CHECK(rb_dispatch(c, RB_EV_CLICK) == 0);
    CHECK(!rb_dom_has_attr(c, "checked", 7));
}

static void test_ev_dispatch_guards(void) {
    const char *html = "<div id=\"a\">x</div>";
    rb_open(html, (int)strlen(html));
    int a = rb_find_id("a");
    CHECK(rb_listen(a, RB_EV_CLICK, ev_log_cb, 0));
    ev_log_reset();
    CHECK(rb_dispatch(0, RB_EV_CLICK) == 0);
    CHECK(rb_dispatch(a, -1) == 0);
    CHECK(rb_dispatch(a, 99) == 0);
    CHECK(rb_dispatch(a, RB_EV_COUNT) == 0);
    CHECK(ev_log_n == 0);
    CHECK(rb_listener_count() == 1);

    /* A removed node loses its listeners and can never fire again. */
    int d = rb_dom_create_element(RB_TAG_DIV);
    CHECK(rb_dom_append_child(a, d));
    CHECK(rb_listen(d, RB_EV_CLICK, ev_log_cb, 0));
    CHECK(rb_listener_count() == 2);
    CHECK(rb_dom_remove_child(d));
    CHECK(rb_listener_count() == 1);
    CHECK(rb_dispatch(d, RB_EV_CLICK) == 0);
    CHECK(ev_log_n == 0);
    CHECK(rb_dispatch(99999, RB_EV_CLICK) == 0);
    CHECK(ev_log_n == 0);
}

/* Captured before dispatch so the callback compares rb_event_target() against
   the handle the test chose, not against the value it was handed. */
static int ev_expect_target;

static void ev_reenter_cb(int type, int target, int current, void *user) {
    (void)type; (void)current;
    CHECK(target == ev_expect_target);
    if (ev_log_n < 16) {
        ev_log_type[ev_log_n] = RB_EV_KEYUP; /* marker */
        ev_log_target[ev_log_n] = target;
        ev_log_current[ev_log_n] = rb_event_current();
        ev_log_user[ev_log_n] = (int)(intptr_t)user;
        ev_log_n++;
    }
    CHECK(rb_event_depth() == 1);
    CHECK(rb_event_type() == RB_EV_CLICK);
    CHECK(rb_event_target() == target);
    CHECK(rb_dispatch(target, RB_EV_CHANGE) == 0);   /* nested dispatch */
    /* The outer event state is untouched after the nested dispatch. */
    CHECK(rb_event_depth() == 1);
    CHECK(rb_event_type() == RB_EV_CLICK);
    CHECK(rb_event_target() == target);
    CHECK(rb_event_current() == target);
    CHECK(rb_event_prevented() == 0);
}

static void test_ev_reentrancy(void) {
    const char *html = "<div id=\"r\"><div id=\"t\">x</div></div>";
    rb_open(html, (int)strlen(html));
    int r = rb_find_id("r"), t = rb_find_id("t");
    ev_log_reset();
    CHECK(rb_listen(t, RB_EV_CHANGE, ev_log_cb, (void *)1));
    CHECK(rb_listen(t, RB_EV_CLICK, ev_reenter_cb, (void *)2));
    CHECK(rb_listen(r, RB_EV_CLICK, ev_log_cb, (void *)3));
    ev_expect_target = t;
    CHECK(rb_dispatch(t, RB_EV_CLICK) == 0);
    /* The nested CHANGE ran inside the CLICK handler, then the outer event
       continued: the root's bubble handler fired last. */
    CHECK(ev_log_n == 3);
    CHECK(ev_log_target[0] == t);                        /* pre-dispatch target */
    CHECK(ev_log_user[0] == 2);                          /* re-entrant marker */
    CHECK(ev_log_type[1] == RB_EV_CHANGE && ev_log_user[1] == 1);
    CHECK(ev_log_type[2] == RB_EV_CLICK && ev_log_user[2] == 3);
    CHECK(ev_log_current[2] == r);
    CHECK(rb_event_depth() == 0);
}

/* Removes the dispatch target mid-event. */
static void ev_remove_target_cb(int type, int target, int current, void *user) {
    (void)type; (void)current; (void)user;
    rb_dom_remove_child(target);
}

/* Removes the target's parent mid-bubble; the target dies with it. */
static void ev_remove_parent_cb(int type, int target, int current, void *user) {
    (void)type; (void)current; (void)user;
    rb_dom_remove_child(rb_dom_parent(target));
}

/* The listener snapshot is per-dispatch-depth: the nested CHANGE dispatch has
   two matching listeners, so if it shared the outer snapshot it would clobber
   the outer node's second CLICK entry and drop that handler. */
static void test_ev_reentrant_match(void) {
    const char *html = "<div id=\"r\"><div id=\"t\">x</div></div>";
    rb_open(html, (int)strlen(html));
    int r = rb_find_id("r"), t = rb_find_id("t");
    ev_log_reset();
    CHECK(rb_listen(t, RB_EV_CHANGE, ev_log_cb, (void *)5));
    CHECK(rb_listen(t, RB_EV_CHANGE, ev_log_cb, (void *)6));
    CHECK(rb_listen(t, RB_EV_CLICK, ev_reenter_cb, (void *)2));
    CHECK(rb_listen(t, RB_EV_CLICK, ev_log_cb, (void *)4));
    CHECK(rb_listen(r, RB_EV_CLICK, ev_log_cb, (void *)3));
    ev_expect_target = t;
    CHECK(rb_dispatch(t, RB_EV_CLICK) == 0);
    CHECK(ev_log_n == 5);
    CHECK(ev_log_user[0] == 2);                 /* re-entrant marker */
    CHECK(ev_log_user[1] == 5 && ev_log_user[2] == 6);
    CHECK(ev_log_user[3] == 4);                 /* second CLICK on t */
    CHECK(ev_log_user[4] == 3);                 /* root */
    CHECK(rb_event_depth() == 0);
}

/* stop() finishes the node, inline onclick included; stopImmediate() must
   suppress the inline onclick too. A <div onclick> with no href isolates the
   inline handler from any tag default action. */
static void test_ev_inline_and_stop(void) {
    const char *html = "<div id=\"a\" onclick=\"location.href='/js'\">x</div>";
    char nav[64];

    rb_open(html, (int)strlen(html));
    int a = rb_find_id("a");
    CHECK(rb_listen(a, RB_EV_CLICK, ev_stop_immediate_cb, 0));
    CHECK(rb_dispatch(a, RB_EV_CLICK) == 0);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));

    rb_open(html, (int)strlen(html));
    a = rb_find_id("a");
    CHECK(rb_listen(a, RB_EV_CLICK, ev_stop_cb, 0));
    CHECK(rb_dispatch(a, RB_EV_CLICK) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/js") == 0);
}

/* A listener that removes the target, or an ancestor on the path, must not
   make the walk or the default action read a dead node. */
static void test_ev_mutation_during_dispatch(void) {
    char nav[64];

    rb_open("<a id=\"a\" href=\"/gone\">l</a>", (int)strlen("<a id=\"a\" href=\"/gone\">l</a>"));
    int a = rb_find_id("a");
    CHECK(rb_listen(a, RB_EV_CLICK, ev_remove_target_cb, 0));
    CHECK(rb_dispatch(a, RB_EV_CLICK) == 0);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));

    const char *html = "<div id=\"p\"><a id=\"a\" href=\"/gone\">l</a></div>";
    rb_open(html, (int)strlen(html));
    a = rb_find_id("a");
    int p = rb_find_id("p");
    ev_log_reset();
    CHECK(rb_listen(a, RB_EV_CLICK, ev_remove_parent_cb, 0));
    CHECK(rb_listen(p, RB_EV_CLICK, ev_log_cb, (void *)7));
    CHECK(rb_dispatch(a, RB_EV_CLICK) == 0);
    CHECK(ev_log_n == 0);              /* the dying parent's handler never ran */
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));
}

/* Recycling a node drops it from the active-element bookkeeping; the reused
   slot must not come back as the active element. */
static void test_ev_active_recycle(void) {
    rb_open("<div id=\"p\"><input id=\"f\"></div>",
            (int)strlen("<div id=\"p\"><input id=\"f\"></div>"));
    int f = rb_find_id("f");
    CHECK(rb_dom_focus(f) == 1);
    CHECK(rb_dom_active_element() == f);
    CHECK(rb_dom_remove_child(f));
    CHECK(rb_dom_active_element() == 0);
    int n = rb_dom_create_element(RB_TAG_INPUT);
    CHECK(n == f);                     /* LIFO free list hands back the slot */
    CHECK(rb_dom_active_element() == 0);
    CHECK(rb_dom_is_focusable(n) == 1);
}

/* RB_EV_SUBMIT on a form: default action navigates (GET only), prevent
   suppresses it, and a post form stays a silent no-op. */
static void test_ev_submit_event(void) {
    char nav[64];
    const char *html =
        "<form id=\"fm\" action=\"/s\"><input id=\"i\" name=\"q\" value=\"v\"></form>";
    rb_open(html, (int)strlen(html));
    int fm = rb_find_id("fm");

    CHECK(rb_dispatch(fm, RB_EV_SUBMIT) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/s?q=v") == 0);

    int ran = 0;
    CHECK(rb_listen(fm, RB_EV_SUBMIT, ev_prevent_cb, &ran));
    CHECK(rb_dispatch(fm, RB_EV_SUBMIT) == 0);
    CHECK(ran == 1);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(rb_listen_remove_all(fm) == 1);

    rb_open("<form id=\"fm\" method=post action=\"/s\"><input name=q></form>",
            (int)strlen("<form id=\"fm\" method=post action=\"/s\"><input name=q></form>"));
    fm = rb_find_id("fm");
    CHECK(rb_dispatch(fm, RB_EV_SUBMIT) == 0);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));
}

/* --- event payload (rb_dispatch_ex / rb_event_key|x|y|button) --- */

static int ev_pl_type[8], ev_pl_key[8], ev_pl_x[8], ev_pl_y[8], ev_pl_btn[8];
static int ev_pl_btns[8], ev_pl_mods[8];
static int ev_pl_n;
static void ev_pl_reset(void) { ev_pl_n = 0; }

static void ev_pl_cb(int type, int target, int current, void *user) {
    (void)target; (void)current; (void)user;
    if (ev_pl_n < 8) {
        ev_pl_type[ev_pl_n] = type;
        ev_pl_key[ev_pl_n] = rb_event_key();
        ev_pl_x[ev_pl_n] = rb_event_x();
        ev_pl_y[ev_pl_n] = rb_event_y();
        ev_pl_btn[ev_pl_n] = rb_event_button();
        ev_pl_btns[ev_pl_n] = rb_event_buttons();
        ev_pl_mods[ev_pl_n] = rb_event_mods();
        ev_pl_n++;
    }
}

/* Re-reads the outer KEYDOWN payload after a nested dispatch carried its own. */
static int ev_pl_outer_ok;
static void ev_pl_nest_cb(int type, int target, int current, void *user) {
    (void)user; (void)current;
    int k0 = rb_event_key(), x0 = rb_event_x(), y0 = rb_event_y(), b0 = rb_event_button();
    ev_pl_cb(type, target, current, 0);            /* record the outer payload */
    rb_dispatch_ex(target, RB_EV_CHANGE, 'Z', 7, 8, 2, 0, 0);  /* nested, own payload */
    ev_pl_outer_ok = (rb_event_key() == k0 && rb_event_x() == x0 &&
                      rb_event_y() == y0 && rb_event_button() == b0 &&
                      rb_event_type() == RB_EV_KEYDOWN);
}

static void test_ev_payload(void) {
    const char *html = "<div id=\"a\"><p id=\"b\">x</p></div>";
    rb_open(html, (int)strlen(html));
    int b = rb_find_id("b");

    /* Outside a dispatch the getters report the empty payload. */
    CHECK(rb_event_key() == 0 && rb_event_x() == 0 && rb_event_y() == 0);
    CHECK(rb_event_button() == -1);
    CHECK(rb_event_buttons() == 0 && rb_event_mods() == 0);

    /* rb_dispatch is exactly rb_dispatch_ex(t, ty, 0, 0, 0, -1, 0, 0). */
    ev_pl_reset();
    CHECK(rb_listen(b, RB_EV_KEYDOWN, ev_pl_cb, 0));
    CHECK(rb_dispatch(b, RB_EV_KEYDOWN) == 0);
    CHECK(ev_pl_n == 1);
    CHECK(ev_pl_type[0] == RB_EV_KEYDOWN);
    CHECK(ev_pl_key[0] == 0 && ev_pl_x[0] == 0 && ev_pl_y[0] == 0 && ev_pl_btn[0] == -1);
    CHECK(rb_listen_remove_all(b) == 1);

    /* A key payload reaches the listener. */
    ev_pl_reset();
    CHECK(rb_listen(b, RB_EV_KEYDOWN, ev_pl_cb, 0));
    CHECK(rb_dispatch_ex(b, RB_EV_KEYDOWN, 'q', 0, 0, -1, 0, RB_MOD_SHIFT) == 0);
    CHECK(ev_pl_n == 1);
    CHECK(ev_pl_key[0] == 'q' && ev_pl_x[0] == 0 && ev_pl_y[0] == 0 && ev_pl_btn[0] == -1);
    CHECK(ev_pl_btns[0] == 0 && ev_pl_mods[0] == RB_MOD_SHIFT);
    /* Keep this KEYDOWN listener: the next block asserts a 2-listener table. */

    /* A mouse payload: content-space coordinates (unclamped), a button index
       and the raw PS/2 button mask. */
    ev_pl_reset();
    CHECK(rb_listen(b, RB_EV_MOUSEDOWN, ev_pl_cb, 0));
    CHECK(rb_dispatch_ex(b, RB_EV_MOUSEDOWN, 0, 200, 150, 2, 5, 0) == 0);
    CHECK(ev_pl_n == 1);
    CHECK(ev_pl_type[0] == RB_EV_MOUSEDOWN);
    CHECK(ev_pl_key[0] == 0 && ev_pl_x[0] == 200 && ev_pl_y[0] == 150 && ev_pl_btn[0] == 2);
    CHECK(ev_pl_btns[0] == 5 && ev_pl_mods[0] == 0);
    CHECK(rb_listen_remove_all(b) == 2);

    /* Nesting: the inner dispatch owns the next state slot, so the outer
       payload and type survive it. */
    ev_pl_reset();
    ev_pl_outer_ok = 0;
    CHECK(rb_listen(b, RB_EV_CHANGE, ev_pl_cb, 0));
    CHECK(rb_listen(b, RB_EV_KEYDOWN, ev_pl_nest_cb, 0));
    CHECK(rb_dispatch_ex(b, RB_EV_KEYDOWN, 'A', 11, 22, 1, 0, 0) == 0);
    CHECK(ev_pl_outer_ok == 1);
    CHECK(ev_pl_n == 2);
    CHECK(ev_pl_key[0] == 'A' && ev_pl_x[0] == 11 && ev_pl_y[0] == 22 && ev_pl_btn[0] == 1);
    CHECK(ev_pl_type[1] == RB_EV_CHANGE && ev_pl_key[1] == 'Z' &&
          ev_pl_x[1] == 7 && ev_pl_y[1] == 8 && ev_pl_btn[1] == 2);
    CHECK(rb_listen_remove_all(b) == 2);
    /* And the empty payload is restored once everything unwinds. */
    CHECK(rb_event_key() == 0 && rb_event_x() == 0 && rb_event_button() == -1);
    CHECK(rb_event_buttons() == 0 && rb_event_mods() == 0);

    /* preventDefault is observable after the dispatch returns, so a UI seam
       that owns a default action (typing) can suppress it. */
    int ran = 0;
    CHECK(rb_listen(b, RB_EV_KEYDOWN, ev_prevent_cb, &ran));
    CHECK(rb_dispatch_ex(b, RB_EV_KEYDOWN, 'x', 0, 0, -1, 0, 0) == 0);
    CHECK(ran == 1);
    CHECK(rb_last_dispatch_prevented() == 1);
    CHECK(rb_listen_remove_all(b) == 1);
    CHECK(rb_dispatch_ex(b, RB_EV_KEYDOWN, 'x', 0, 0, -1, 0, 0) == 0);
    CHECK(rb_last_dispatch_prevented() == 0);
    /* A dispatch rejected up front is not "prevented". */
    CHECK(rb_dispatch_ex(0, RB_EV_KEYDOWN, 'x', 0, 0, -1, 0, 0) == 0);
    CHECK(rb_last_dispatch_prevented() == 0);
}

/* RB_EV_MOUSEMOVE was appended at the end of the enum: pin the ABI position
   and make sure the type dispatches like any other with a full payload. */
static void test_ev_mousemove_abi(void) {
    const char *html = "<div id=\"a\"><p id=\"b\">x</p></div>";
    rb_open(html, (int)strlen(html));
    int b = rb_find_id("b");
    CHECK(RB_EV_MOUSEMOVE == 15);          /* appended, older values untouched */
    CHECK(RB_EV_COUNT == 16);
    ev_pl_reset();
    CHECK(rb_listen(b, RB_EV_MOUSEMOVE, ev_pl_cb, 0));
    CHECK(rb_dispatch_ex(b, RB_EV_MOUSEMOVE, 0, 40, 50, -1, 1, 0) == 0);
    CHECK(ev_pl_n == 1 && ev_pl_type[0] == RB_EV_MOUSEMOVE);
    CHECK(ev_pl_x[0] == 40 && ev_pl_y[0] == 50 && ev_pl_btn[0] == -1);
    CHECK(ev_pl_btns[0] == 1 && ev_pl_mods[0] == 0);
    CHECK(rb_listen_remove_all(b) == 1);
}

/* Click-cancellation rule at the engine level: preventing MOUSEDOWN does NOT
   suppress the subsequent CLICK. The seamed sequence is on_press (MOUSEDOWN)
   -> on_release (MOUSEUP, then click_at -> CLICK); only the CLICK's own
   listener can cancel its default action. This pins the engine contract
   rbrowse.c's on_release relies on; the on_release ordering itself lives in
   rbrowse.c, which is not part of this host build (compile-verified only). */
static void test_click_after_prevented_mousedown(void) {
    const char *html = "<input id=\"c\" type=\"checkbox\">";
    rb_open(html, (int)strlen(html));
    int c = rb_find_id("c");
    int prevented = 0;
    CHECK(rb_listen(c, RB_EV_MOUSEDOWN, ev_prevent_cb, &prevented));
    /* on_press: a prevented MOUSEDOWN leaves the checkbox untouched. */
    CHECK(rb_dispatch_ex(c, RB_EV_MOUSEDOWN, 0, 3, 4, 0, 1, 0) == 0);
    CHECK(prevented == 1 && rb_last_dispatch_prevented() == 1);
    CHECK(!rb_dom_has_attr(c, "checked", 7));
    /* on_release: MOUSEUP, then the CLICK click_at dispatches. The click still
       runs its listener and its default action (the toggle), even though the
       earlier MOUSEDOWN was prevented. */
    CHECK(rb_dispatch_ex(c, RB_EV_MOUSEUP, 0, 3, 4, 0, 1, 0) == 0);
    ev_pl_reset();
    CHECK(rb_listen(c, RB_EV_CLICK, ev_pl_cb, 0));
    CHECK(rb_dispatch_ex(c, RB_EV_CLICK, 0, 3, 4, 0, 1, 0) == 1);
    CHECK(ev_pl_n == 1 && ev_pl_type[0] == RB_EV_CLICK);
    CHECK(ev_pl_x[0] == 3 && ev_pl_y[0] == 4 && ev_pl_btn[0] == 0);
    CHECK(rb_dom_has_attr(c, "checked", 7));
}

/* Submit helper callback: the f1-SUBMIT listener submits f2 through the real
   click path; the second variant also prevents the outer submission. */
static int g_xsubmit_b2;
static void submit_cross_prevent_cb(int type, int target, int current, void *user) {
    (void)type; (void)target; (void)current; (void)user;
    rb_activate(g_xsubmit_b2);
    rb_event_prevent();
}

/* Nested submission: f1's SUBMIT listener submits f2 through the real click
   path. The inner submission must carry its own submitter pair (who=B), and
   preventing the OUTER submit event must leave only the inner navigation
   queued. NOTE the engine's nav slot is single: without the prevent, the
   outer build (finishing last) overwrites the inner navigation — the same
   "final navigation wins" outcome a nested form.submit() has in real
   browsers. */
static void test_submit_nested_cross(void) {
    char nav[64];
    const char *html =
        "<form id=\"f1\" action=\"/one\"><input name=\"a\" value=\"1\">"
        "<button id=\"b1\">s1</button></form>"
        "<form id=\"f2\" action=\"/two\"><input name=\"b\" value=\"2\">"
        "<button id=\"b2\" name=\"who\" value=\"B\">s2</button></form>";
    rb_open(html, (int)strlen(html));
    int f1 = rb_find_id("f1");
    g_xsubmit_b2 = rb_find_id("b2");
    CHECK(rb_listen(f1, RB_EV_SUBMIT, submit_cross_prevent_cb, 0));
    CHECK(rb_activate(rb_find_id("b1")) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/two?b=2&who=B") == 0);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));   /* outer was prevented */
}

/* A real submission (activating a submit control) fires RB_EV_SUBMIT on the
   form before anything is queued; a preventing listener aborts it. */
static void test_ev_submit_via_form(void) {
    char nav[64];
    const char *html =
        "<form id=\"fm\" action=\"/s\"><input id=\"i\" name=\"q\" value=\"v\">"
        "<input id=\"go\" type=\"submit\"></form>";
    rb_open(html, (int)strlen(html));
    int fm = rb_find_id("fm"), go = rb_find_id("go");

    ev_pl_reset();
    CHECK(rb_listen(fm, RB_EV_SUBMIT, ev_pl_cb, 0));
    CHECK(rb_activate(go) == 2);
    CHECK(ev_pl_n == 1 && ev_pl_type[0] == RB_EV_SUBMIT);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/s?q=v") == 0);
    CHECK(rb_listen_remove_all(fm) == 1);

    /* preventDefault on the submit event: no navigation is queued at all. */
    int ran = 0;
    CHECK(rb_listen(fm, RB_EV_SUBMIT, ev_prevent_cb, &ran));
    CHECK(rb_activate(go) == 0);
    CHECK(ran == 1);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(rb_listen_remove_all(fm) == 1);

    /* The whole thing still submits exactly once (one build, one nav). */
    CHECK(rb_activate(go) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/s?q=v") == 0);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));

    /* A GET form with no action and no named controls builds nothing: the
       event-path submit must report no navigation, not "the default ran". */
    rb_open("<form id=\"e\"><input id=\"ego\" type=\"submit\"></form>",
            (int)strlen("<form id=\"e\"><input id=\"ego\" type=\"submit\"></form>"));
    int ego = rb_find_id("ego");
    CHECK(rb_activate(ego) == 0);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));
}

/* P0-1: a listener that reloads the document (rb_open) while an outer dispatch
   is still walking must not corrupt the dispatch stack. ev_reset used to zero
   g_ev_depth mid-walk, so the outer exit drove the counter to -1 and the next
   dispatch wrote g_ev_states[-1] (ASan global-buffer-overflow). The entry
   depth is now restored on exit and the state wipe is deferred. */
static void ev_reopen_mid_cb(int type, int target, int current, void *user) {
    (void)type; (void)target; (void)current; (void)user;
    const char *h = "<div id=\"z\">z</div>";
    rb_open(h, (int)strlen(h));
}

static void test_ev_open_mid_dispatch(void) {
    const char *html = "<div id=\"x\"><p id=\"y\">y</p></div>";
    rb_open(html, (int)strlen(html));
    int x = rb_find_id("x");
    CHECK(rb_listen(x, RB_EV_CLICK, ev_reopen_mid_cb, 0));
    /* The listener replaces the document mid-walk; the walk finishes, the
       default action is skipped (x is gone), and the stack unwinds to 0. */
    CHECK(rb_dispatch(x, RB_EV_CLICK) == 0);
    CHECK(rb_event_depth() == 0);
    CHECK(rb_listener_count() == 0);           /* rb_open cleared the table */
    /* The next dispatch must use a valid state row (was index -1). */
    CHECK(rb_dispatch(1, RB_EV_CLICK) == 0);
    CHECK(rb_event_depth() == 0);
    CHECK(rb_find_id("z") > 0);                /* the reloaded doc is live */
}

/* P0-3: a mouse CLICK is the enter=0 path (rb_dispatch/rb_dispatch_ex), whose
   default action toggles a checkbox and navigates a link; rb_activate is the
   keyboard enter=1 path, where a checkbox does NOT toggle. This is the
   contract rbrowse.c's click_at now uses — it routed through rb_activate
   before, so a physical click on a checkbox did nothing and CLICK carried no
   payload while MOUSEDOWN/UP did. */
static void test_ev_click_mouse_vs_activate(void) {
    char nav[64];
    const char *html =
        "<input id=\"c\" type=\"checkbox\"><a id=\"lk\" href=\"/next\">go</a>";
    rb_open(html, (int)strlen(html));
    int c = rb_find_id("c"), lk = rb_find_id("lk");

    /* Mouse path: enter=0 toggles the checkbox and carries the payload. */
    ev_pl_reset();
    CHECK(rb_listen(c, RB_EV_CLICK, ev_pl_cb, 0));
    CHECK(rb_dispatch_ex(c, RB_EV_CLICK, 0, 7, 9, 0, 1, 0) == 1);
    CHECK(rb_dom_has_attr(c, "checked", 7));
    CHECK(ev_pl_n == 1 && ev_pl_x[0] == 7 && ev_pl_y[0] == 9 && ev_pl_btn[0] == 0);
    CHECK(rb_listen_remove_all(c) == 1);
    CHECK(rb_dispatch_ex(c, RB_EV_CLICK, 0, 7, 9, 0, 1, 0) == 1);
    CHECK(!rb_dom_has_attr(c, "checked", 7));

    /* Keyboard path: rb_activate (enter=1) never toggles a checkbox. */
    CHECK(rb_activate(c) == 0);
    CHECK(!rb_dom_has_attr(c, "checked", 7));

    /* Mouse click on a link queues the navigation. */
    CHECK(rb_dispatch_ex(lk, RB_EV_CLICK, 0, 7, 9, 0, 1, 0) == 2);
    CHECK(rb_take_nav(nav, (int)sizeof(nav)));
    CHECK(strcmp(nav, "/next") == 0);
}

/* P0-2 (engine half): a SCROLL listener's preventDefault is observable after
   the dispatch returns, the primitive rbrowse.c's wheel seam gates the scroll
   delta on. The application itself lives in rbrowse.c (compile-verified only). */
static void test_ev_scroll_prevent_observable(void) {
    rb_open("<div id=\"a\">x</div>", (int)strlen("<div id=\"a\">x</div>"));
    int a = rb_find_id("a");
    int ran = 0;
    CHECK(rb_listen(a, RB_EV_SCROLL, ev_prevent_cb, &ran));
    CHECK(rb_dispatch_ex(a, RB_EV_SCROLL, 0, 5, 6, -1, 0, 0) == 0);
    CHECK(ran == 1 && rb_last_dispatch_prevented() == 1);
    CHECK(rb_listen_remove_all(a) == 1);
    CHECK(rb_dispatch_ex(a, RB_EV_SCROLL, 0, 5, 6, -1, 0, 0) == 0);
    CHECK(rb_last_dispatch_prevented() == 0);
}

/* P0-10: a SUBMIT dispatch rejected by the depth cap must be treated as
   undelivered: no URL is built and no navigation happens. Before the fix the
   fallback submit_build navigated silently, without the page ever seeing the
   event. */
static void ev_sub_depth_cb(int type, int target, int current, void *user) {
    (void)type; (void)current; (void)target;
    int next = (int)(intptr_t)user;
    if (next > 0) rb_dispatch(next, RB_EV_CLICK);
}

static void test_ev_submit_depth_capped(void) {
    char nav[64];
    const char *html =
        "<form id=\"fm\" action=\"/s\"><input name=\"q\" value=\"v\">"
        "<input id=\"go\" type=\"submit\"></form>"
        "<div id=\"a\"></div><div id=\"b\"></div><div id=\"c\"></div>";
    rb_open(html, (int)strlen(html));
    int a = rb_find_id("a"), b = rb_find_id("b"), c = rb_find_id("c");
    int go = rb_find_id("go");
    /* a -> b -> c -> go: the deepest dispatch's CLICK default action runs
       submit_form while g_ev_depth is already EV_MAX_DEPTH, so its SUBMIT
       dispatch is rejected by the cap before any listener runs. */
    CHECK(rb_listen(a, RB_EV_CLICK, ev_sub_depth_cb, (void *)(intptr_t)b));
    CHECK(rb_listen(b, RB_EV_CLICK, ev_sub_depth_cb, (void *)(intptr_t)c));
    CHECK(rb_listen(c, RB_EV_CLICK, ev_sub_depth_cb, (void *)(intptr_t)go));
    CHECK(rb_event_dropped() == 0);
    rb_dispatch(a, RB_EV_CLICK);
    CHECK(rb_event_dropped() >= 1);                  /* the SUBMIT was dropped */
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));      /* and nothing navigated */
    CHECK(rb_event_depth() == 0);
}

/* rb_dom_generation: 0 for a dead/invalid node, bumped when a slot is
   recycled. The hover seam validates a stored handle with it. */
static void test_dom_generation(void) {
    rb_open("<div id=\"a\"></div><div id=\"b\"></div>",
            (int)strlen("<div id=\"a\"></div><div id=\"b\"></div>"));
    int a = rb_find_id("a");
    int g = rb_dom_generation(a);
    CHECK(g > 0);
    CHECK(rb_dom_generation(0) == 0);
    CHECK(rb_dom_generation(99999) == 0);
    CHECK(rb_dom_remove_child(a) == 1);
    CHECK(rb_dom_generation(a) == 0);                /* dead */
    int n = rb_dom_create_element(RB_TAG_DIV);
    CHECK(n == a);                                   /* slot reused */
    CHECK(rb_dom_generation(n) > 0 && rb_dom_generation(n) != g);
}

static void test_ev_focusability(void) {
    /* display:none removes a control from the focus list; programmatic focus
       must agree. */
    const char *html =
        "<style>#h{display:none}</style><input id=\"h\"><input id=\"v\">"
        "<div style=\"display:none\"><input id=\"n\"></div>";
    rb_open(html, (int)strlen(html));
    int h = rb_find_id("h"), v = rb_find_id("v"), n = rb_find_id("n");
    rb_relayout();
    CHECK(rb_focus_index(h) < 0);
    CHECK(rb_focus_index(v) >= 0);
    CHECK(!rb_dom_is_focusable(h));
    CHECK(rb_dom_is_focusable(v));
    CHECK(!rb_dom_is_focusable(n));
    CHECK(rb_dom_focus(h) == 0);
    CHECK(rb_dom_focus(n) == 0);
    CHECK(rb_dom_focus(v) == 1);
    CHECK(rb_dom_active_element() == v);

    /* A relayout drops an active element that became non-focusable. */
    rb_open("<input id=\"x\">", (int)strlen("<input id=\"x\">"));
    int x = rb_find_id("x");
    CHECK(rb_dom_focus(x) == 1);
    CHECK(rb_dom_active_element() == x);
    CHECK(rb_dom_set_attr(x, "style", 5, "display:none", 12));
    rb_relayout();
    CHECK(rb_dom_active_element() == 0);
    CHECK(!rb_dom_is_focusable(x));
}

static void test_ev_focus(void) {
    const char *html = "<div id=\"d\"><input id=\"f\" value=\"v\">"
                       "<textarea id=\"t\"></textarea></div>";
    rb_open(html, (int)strlen(html));
    int d = rb_find_id("d"), f = rb_find_id("f"), t = rb_find_id("t");
    ev_log_reset();
    (void)d;

    CHECK(rb_dom_active_element() == 0);
    CHECK(!rb_dom_focus(0));
    CHECK(!rb_dom_focus(99999));
    CHECK(!rb_dom_focus(d));          /* a plain div is not focusable */
    CHECK(rb_listen(f, RB_EV_FOCUS, ev_log_cb, (void *)1));
    CHECK(rb_listen(t, RB_EV_FOCUS, ev_log_cb, (void *)2));
    CHECK(rb_listen(f, RB_EV_BLUR, ev_log_cb, (void *)3));
    CHECK(rb_listen(t, RB_EV_BLUR, ev_log_cb, (void *)4));

    CHECK(rb_dom_focus(f) == 1);
    CHECK(rb_dom_active_element() == f);
    CHECK(rb_dom_focus(f) == 1);      /* re-focusing is quiet */
    CHECK(ev_log_n == 1);
    CHECK(ev_log_type[0] == RB_EV_FOCUS && ev_log_target[0] == f);

    /* Focusing t blurs f, then focuses t. */
    ev_log_reset();
    CHECK(rb_dom_focus(t) == 1);
    CHECK(rb_dom_active_element() == t);
    CHECK(ev_log_n == 2);
    CHECK(ev_log_type[0] == RB_EV_BLUR && ev_log_target[0] == f);
    CHECK(ev_log_type[1] == RB_EV_FOCUS && ev_log_target[1] == t);

    CHECK(rb_dom_blur() == 1);
    CHECK(rb_dom_active_element() == 0);
    CHECK(rb_dom_blur() == 0);
    /* The blur above recorded t's BLUR listener; start the click check clean. */
    ev_log_reset();

    /* A plain click on a text field focuses it (and dispatches FOCUS).
       Keep a live probe so ev_log_n == 0 means "no FOCUS dispatched", not
       "every listener was removed". */
    CHECK(rb_listen_remove_all(f) == 2); /* its FOCUS and BLUR listeners */
    CHECK(rb_dom_focus(f) == 1);
    CHECK(rb_listen(f, RB_EV_FOCUS, ev_log_cb, (void *)9));
    int x, y, w, h;
    CHECK(rb_first_box(f, &x, &y, &w, &h));
    CHECK(rb_click(x + 1, y + 1) == f);
    CHECK(rb_dom_active_element() == f);
    CHECK(ev_log_n == 0);            /* already active: no new FOCUS fired */
}

static void test_ev_type_events(void) {
    const char *html = "<form action=\"/s\"><input id=\"f\" name=\"q\" value=\"ab\"></form>";
    char text[16];

    /* INPUT and CHANGE fire, in that order. */
    rb_open(html, (int)strlen(html));
    int f = rb_find_id("f");
    ev_log_reset();
    CHECK(rb_listen(f, RB_EV_INPUT, ev_log_cb, (void *)1));
    CHECK(rb_listen(f, RB_EV_CHANGE, ev_log_cb, (void *)2));
    CHECK(rb_type_char(f, 'c') == 1);
    CHECK(ev_log_n == 2);
    CHECK(ev_log_type[0] == RB_EV_INPUT && ev_log_target[0] == f);
    CHECK(ev_log_type[1] == RB_EV_CHANGE && ev_log_target[1] == f);
    CHECK(rb_collect_text(f, text, (int)sizeof(text)) == 3);
    CHECK(strcmp(text, "abc") == 0);

    /* A listener preventing INPUT reverts the edit: no text change and no
       CHANGE dispatch, but the INPUT listeners (log + prevent) still ran. */
    rb_open(html, (int)strlen(html));
    f = rb_find_id("f");
    int ran = 0;
    CHECK(rb_listen(f, RB_EV_INPUT, ev_log_cb, 0));
    CHECK(rb_listen(f, RB_EV_INPUT, ev_prevent_cb, &ran));
    ev_log_reset();
    CHECK(rb_type_char(f, 'd') == 0);
    CHECK(ran == 1);
    CHECK(ev_log_n == 1);
    CHECK(ev_log_type[0] == RB_EV_INPUT);
    CHECK(rb_collect_text(f, text, (int)sizeof(text)) == 2);
    CHECK(strcmp(text, "ab") == 0);

    /* A preventing CHANGE listener keeps the value: the edit stands. INPUT
       still fires first (the emitter dispatches the pair after the mutation);
       only the CHANGE prevent is ignored, since rb_type_char's revert is tied
       to the INPUT result. */
    rb_open(html, (int)strlen(html));
    f = rb_find_id("f");
    int ran2 = 0;
    CHECK(rb_listen(f, RB_EV_INPUT, ev_log_cb, 0));
    CHECK(rb_listen(f, RB_EV_CHANGE, ev_log_cb, 0));
    CHECK(rb_listen(f, RB_EV_CHANGE, ev_prevent_cb, &ran2));
    ev_log_reset();
    CHECK(rb_type_char(f, 'e') == 1);
    CHECK(ran2 == 1);
    CHECK(ev_log_n == 2);            /* INPUT and CHANGE both dispatched */
    CHECK(ev_log_type[0] == RB_EV_INPUT && ev_log_type[1] == RB_EV_CHANGE);
    CHECK(rb_collect_text(f, text, (int)sizeof(text)) == 3);
    CHECK(strcmp(text, "abe") == 0);

    /* Backspace can be reverted the same way. */
    rb_open(html, (int)strlen(html));
    f = rb_find_id("f");
    CHECK(rb_listen(f, RB_EV_INPUT, ev_prevent_cb, &ran));
    CHECK(rb_backspace(f) == 0);
    CHECK(rb_collect_text(f, text, (int)sizeof(text)) == 2);
    CHECK(strcmp(text, "ab") == 0);

    /* Backspace fires the pair too. */
    rb_open(html, (int)strlen(html));
    f = rb_find_id("f");
    ev_log_reset();
    CHECK(rb_listen(f, RB_EV_INPUT, ev_log_cb, 0));
    CHECK(rb_listen(f, RB_EV_CHANGE, ev_log_cb, 0));
    CHECK(rb_backspace(f) == 1);
    CHECK(ev_log_n == 2);
    CHECK(ev_log_type[0] == RB_EV_INPUT && ev_log_type[1] == RB_EV_CHANGE);
    CHECK(rb_collect_text(f, text, (int)sizeof(text)) == 1);
    CHECK(strcmp(text, "a") == 0);

    /* No change, no events: backspacing an empty field stays quiet. */
    rb_open("<input id=\"g\">", (int)strlen("<input id=\"g\">"));
    int g = rb_find_id("g");
    ev_log_reset();
    CHECK(rb_listen(g, RB_EV_INPUT, ev_log_cb, 0));
    CHECK(rb_backspace(g) == 0);
    CHECK(ev_log_n == 0);
}

/* --- P0.3: a failed set_text must not have destroyed the subtree --- */

static void test_dom_set_text_atomic(void) {
    /* Burn the bump-only pool with a text node whose content keeps growing. */
    int t = rb_dom_create_text("x", 1);
    CHECK(t > 0);
    char buf[512];
    int grew = 0;
    for (int i = 2; i < 512; i++) {
        memset(buf, 'a', (size_t)i);
        if (!rb_dom_set_text(t, buf, i)) break;
        grew++;
    }
    CHECK(grew > 0);
    /* An element with two children. */
    int el = rb_dom_create_element(RB_TAG_DIV);
    int c1 = rb_dom_create_element(RB_TAG_SPAN);
    int c2 = rb_dom_create_element(RB_TAG_SPAN);
    CHECK(el > 0 && c1 > 0 && c2 > 0);
    CHECK(rb_dom_append_child(el, c1));
    CHECK(rb_dom_append_child(el, c2));
    CHECK(rb_dom_children_count(el) == 2);
    /* The growth loop stopped on a large request; soak up the last few bytes
       with tiny text nodes so even a 3-byte set_text cannot allocate. */
    int guard = 0;
    static char big[4000];
    memset(big, 'b', sizeof(big));
    while (rb_dom_create_text(big, (int)sizeof(big)) && guard++ < 100000) { }
    while (rb_dom_create_text("aa", 2) && guard++ < 100000) { }
    CHECK(rb_dom_set_text(el, "new", 3) == 0);
    CHECK(rb_dom_children_count(el) == 2);   /* subtree intact */
    CHECK(rb_dom_first(el) == c1);
    CHECK(rb_dom_next(c1) == c2);
}

/* --- P0.1: recycled slots must not alias a different node --- */

static int g_repro_r, g_repro_p, g_foreign_fired;

static void ev_foreign_cb(int type, int target, int current, void *user) {
    (void)type; (void)target; (void)current; (void)user;
    g_foreign_fired++;
}

/* Removes the target's parent, then creates nodes so the LIFO free list hands
   the dead path slots back out; the node that lands on the old parent slot
   gets a listener. The outer walk must not deliver to it. */
static void ev_gen_repro_cb(int type, int target, int current, void *user) {
    (void)type; (void)target; (void)current; (void)user;
    CHECK(rb_dom_remove_child(g_repro_p));
    int d1 = rb_dom_create_element(RB_TAG_DIV);
    int d2 = rb_dom_create_element(RB_TAG_DIV);
    int d3 = rb_dom_create_element(RB_TAG_DIV);
    CHECK(rb_dom_append_child(g_repro_r, d1));
    CHECK(rb_dom_append_child(g_repro_r, d2));
    CHECK(rb_dom_append_child(g_repro_r, d3));
    /* Removal of p frees p, t and t's text child; LIFO hands them to d1..d3,
       so d3 owns p's old slot (the one the bubble walk will reach). */
    CHECK(d3 == g_repro_p);
    CHECK(rb_listen(d3, RB_EV_CLICK, ev_foreign_cb, 0));
}

static void test_ev_gen_alias(void) {
    const char *html =
        "<div id=\"r\"><div id=\"p\"><div id=\"t\">x</div></div></div>";
    rb_open(html, (int)strlen(html));
    g_repro_r = rb_find_id("r");
    g_repro_p = rb_find_id("p");
    int t = rb_find_id("t");
    g_foreign_fired = 0;
    ev_log_reset();
    CHECK(rb_listen(t, RB_EV_CLICK, ev_gen_repro_cb, 0));
    CHECK(rb_listen(g_repro_r, RB_EV_CLICK, ev_log_cb, (void *)8));
    /* Bubble reaches t, then the recycled p slot (now d3), then r. The d3
       entry must be skipped by {slot, gen}; only r's own handler runs. */
    CHECK(rb_dispatch(t, RB_EV_CLICK) == 0);
    CHECK(g_foreign_fired == 0);
    CHECK(ev_log_n == 1 && ev_log_user[0] == 8);
    CHECK(ev_log_target[0] == t);
}

/* The default action must not run on a node that recycled the target's slot. */
static int g_da_target, g_da_parent;

static void ev_recycle_target_cb(int type, int target, int current, void *user) {
    (void)type; (void)target; (void)current; (void)user;
    CHECK(rb_dom_remove_child(g_da_target));
    int n = rb_dom_create_element(RB_TAG_A);
    CHECK(n == g_da_target);           /* empty target: its slot comes back */
    CHECK(rb_dom_set_attr(n, "href", 4, "/new", 4));
    CHECK(rb_dom_append_child(g_da_parent, n));
}

static void test_ev_default_action_gen(void) {
    char nav[64];
    const char *html = "<div id=\"p\"><a id=\"t\" href=\"/gone\"></a></div>";
    rb_open(html, (int)strlen(html));
    g_da_target = rb_find_id("t");
    g_da_parent = rb_find_id("p");
    CHECK(rb_listen(g_da_target, RB_EV_CLICK, ev_recycle_target_cb, 0));
    /* Acting on the recycled <a href="/new"> would queue a foreign nav. */
    CHECK(rb_dispatch(g_da_target, RB_EV_CLICK) == 0);
    CHECK(!rb_take_nav(nav, (int)sizeof(nav)));
}

/* The type/backspace revert must not write into a slot recycled during the
   INPUT dispatch. */
static int g_rv_field, g_rv_parent;

static void ev_replace_field_cb(int type, int target, int current, void *user) {
    (void)type; (void)target; (void)current; (void)user;
    CHECK(rb_dom_remove_child(g_rv_field));
    int n = rb_dom_create_element(RB_TAG_INPUT);
    CHECK(n == g_rv_field);            /* same slot, new generation */
    CHECK(rb_dom_append_child(g_rv_parent, n));
    CHECK(rb_dom_set_text(n, "ZZ", 2));
    rb_event_prevent();
}

static void test_ev_input_revert_gen(void) {
    const char *html = "<div id=\"p\"><input id=\"f\" value=\"ab\"></div>";
    char text[16];
    rb_open(html, (int)strlen(html));
    g_rv_field = rb_find_id("f");
    g_rv_parent = rb_find_id("p");
    CHECK(rb_listen(g_rv_field, RB_EV_INPUT, ev_replace_field_cb, 0));
    CHECK(rb_type_char(g_rv_field, 'c') == 0);   /* prevented */
    /* The prevent-revert stayed out of the reused slot. */
    CHECK(rb_collect_text(g_rv_field, text, (int)sizeof(text)) == 2);
    CHECK(strcmp(text, "ZZ") == 0);
}

/* Public accessors report nothing for a dead slot that has not been reused. */
static void test_node_dead_accessors(void) {
    const char *html = "<div id=\"q\"><a id=\"l\" href=\"/x\">l</a></div>";
    char href[16];
    rb_open(html, (int)strlen(html));
    int l = rb_find_id("l");
    CHECK(rb_node_tag(l) == RB_TAG_A);
    CHECK(rb_node_href(l, href, (int)sizeof(href)) == 1);
    CHECK(rb_dom_remove_child(l));
    CHECK(rb_node_tag(l) == RB_TAG_OTHER);
    CHECK(rb_node_href(l, href, (int)sizeof(href)) == 0);
    CHECK(rb_node_color(l) == 0);
}

/* --- P0.2: mutating the listener table mid-walk --- */

/* Two handlers on one node; the first frees the node. The second registered
   when the dispatch started and must still run. */
static void test_ev_remove_keeps_listeners(void) {
    const char *html = "<div id=\"p\"><div id=\"x\">x</div></div>";
    rb_open(html, (int)strlen(html));
    int x = rb_find_id("x");
    ev_log_reset();
    CHECK(rb_listen(x, RB_EV_CLICK, ev_remove_target_cb, 0));
    CHECK(rb_listen(x, RB_EV_CLICK, ev_log_cb, (void *)42));
    CHECK(rb_dispatch(x, RB_EV_CLICK) == 0);
    CHECK(ev_log_n == 1 && ev_log_user[0] == 42);
}

/* --- P1.5: listener-table overflow is not page truncation --- */

static void test_ev_listener_overflow(void) {
    const char *html = "<div id=\"x\">x</div>";
    rb_open(html, (int)strlen(html));
    int x = rb_find_id("x");
    for (int i = 0; i < 4096; i++)     /* EV_LISTEN_MAX in engine.c */
        CHECK(rb_listen(x, RB_EV_CLICK, ev_log_cb, (void *)(intptr_t)0));
    CHECK(rb_listener_count() == 4096);
    CHECK(rb_listen(x, RB_EV_CLICK, ev_log_cb, 0) == 0);
    CHECK(rb_oom() == 0);              /* not the "memory full" banner */
    CHECK(rb_listener_count() == 4096);
}

/* --- P1.7: scripted style writes invalidate the style memo --- */

static void test_ev_script_style_touch(void) {
    const char *html =
        "<input id=\"f\" onclick=\"document.getElementById('f').style.display='none'\">";
    rb_open(html, (int)strlen(html));
    int f = rb_find_id("f");
    rb_relayout();                     /* populate the memo */
    CHECK(rb_dom_is_focusable(f));
    CHECK(rb_dispatch(f, RB_EV_CLICK) == 1);
    /* No relayout in between: the memo must have been invalidated by run_one. */
    CHECK(!rb_dom_is_focusable(f));
}

/* --- P1.8: MAX_FOCUS overflow must not evict a still-focusable active --- */

static void test_ev_focus_overflow(void) {
    const char *html = "<div id=\"r\"></div>";
    int inputs[RB_MAX_FOCUS + 6];
    rb_open(html, (int)strlen(html));
    int r = rb_find_id("r");
    for (int i = 0; i < RB_MAX_FOCUS + 6; i++) {
        inputs[i] = rb_dom_create_element(RB_TAG_INPUT);
        CHECK(rb_dom_append_child(r, inputs[i]));
    }
    rb_relayout();
    CHECK(rb_focus_count() == RB_MAX_FOCUS);     /* the list caps below the input count */
    int last = inputs[RB_MAX_FOCUS + 5];
    CHECK(rb_dom_focus(last) == 1);
    CHECK(rb_dom_active_element() == last);
    rb_relayout();
    /* It no longer fits the 64-entry list, but it is still focusable: keep it
       active rather than evicting it without a BLUR. */
    CHECK(rb_dom_active_element() == last);
    CHECK(rb_dom_set_attr(last, "style", 5, "display:none", 12));
    rb_relayout();
    CHECK(rb_dom_active_element() == 0);
}

/* --- P1.9: type= is only meaningful on a form control --- */

static void test_type_attr_non_input(void) {
    char v[16];
    const char *html = "<div id=\"d\" type=\"checkbox\">x</div>";
    rb_open(html, (int)strlen(html));
    int d = rb_find_id("d");
    /* the attribute is kept (getAttribute) but means nothing on a <div> */
    CHECK(rb_dom_has_attr(d, "type", 4));
    CHECK(rb_dom_get_attr(d, "type", 4, v, (int)sizeof(v)) == 1 && strcmp(v, "checkbox") == 0);
    CHECK(!rb_dom_has_attr(d, "checked", 7));
    CHECK(rb_dispatch(d, RB_EV_CLICK) == 0);   /* no toggle, no submit */
    CHECK(!rb_dom_has_attr(d, "checked", 7));
    CHECK(rb_dom_set_attr(d, "type", 4, "checkbox", 8) == 1);   /* stored, but no control behaviour */
    CHECK(rb_dispatch(d, RB_EV_CLICK) == 0);
    CHECK(!rb_dom_has_attr(d, "checked", 7));

    /* The same path still works on a real input. */
    rb_open("<input id=\"i\">", (int)strlen("<input id=\"i\">"));
    int i = rb_find_id("i");
    CHECK(rb_dom_set_attr(i, "type", 4, "checkbox", 8));
    CHECK(rb_dom_has_attr(i, "type", 4));
    CHECK(rb_dom_get_attr(i, "type", 4, v, (int)sizeof(v)) == 1);
    CHECK(strcmp(v, "checkbox") == 0);
    CHECK(rb_dispatch(i, RB_EV_CLICK) == 1);
    CHECK(rb_dom_has_attr(i, "checked", 7));
}

/* --- P2.11: rb_open clears the listener registry --- */

static void test_ev_open_clears_listeners(void) {
    rb_open("<div id=\"a\">x</div>", (int)strlen("<div id=\"a\">x</div>"));
    int a = rb_find_id("a");
    CHECK(rb_listen(a, RB_EV_CLICK, ev_log_cb, 0));
    CHECK(rb_listener_count() == 1);
    rb_open("<div id=\"b\">y</div>", (int)strlen("<div id=\"b\">y</div>"));
    CHECK(rb_listener_count() == 0);
}

/* --- P2 nit: dropped nested dispatches are observable --- */

static int g_chain_max;

static void ev_chain_cb(int type, int target, int current, void *user) {
    (void)type; (void)current; (void)user;
    int d = rb_event_depth();
    if (d > g_chain_max) g_chain_max = d;
    if (d < 8) rb_dispatch(target, RB_EV_CHANGE);   /* runs past the cap */
}

static void test_ev_depth_dropped(void) {
    rb_open("<div id=\"a\">x</div>", (int)strlen("<div id=\"a\">x</div>"));
    int a = rb_find_id("a");
    g_chain_max = 0;
    CHECK(rb_event_dropped() == 0);            /* rb_open reset it */
    CHECK(rb_listen(a, RB_EV_CHANGE, ev_chain_cb, 0));
    CHECK(rb_dispatch(a, RB_EV_CHANGE) == 0);  /* outer result unaffected */
    CHECK(g_chain_max == 4);                   /* reached the depth cap */
    CHECK(rb_event_dropped() >= 1);            /* the lost nesting is visible */
    CHECK(rb_event_depth() == 0);
}

/* --- P2 nit: interleaved removal must not shift the walk's indices --- */

static int g_ord_y;

static void ev_ord_a(int type, int target, int current, void *user) {
    ev_log_cb(type, target, current, user);    /* log A */
    CHECK(rb_listen_remove(g_ord_y, RB_EV_CLICK) == 1);
}

static void test_ev_remove_interleaved_order(void) {
    const char *html = "<div id=\"x\">x</div><div id=\"y\">y</div>";
    rb_open(html, (int)strlen(html));
    int x = rb_find_id("x");
    g_ord_y = rb_find_id("y");
    ev_log_reset();
    CHECK(rb_listen(x, RB_EV_CLICK, ev_ord_a, (void *)1));       /* A */
    CHECK(rb_listen(g_ord_y, RB_EV_CLICK, ev_log_cb, (void *)9)); /* interleaved D */
    CHECK(rb_listen(x, RB_EV_CLICK, ev_log_cb, (void *)2));       /* B */
    CHECK(rb_listen(x, RB_EV_CLICK, ev_log_cb, (void *)3));       /* C */
    CHECK(rb_dispatch(x, RB_EV_CLICK) == 0);
    /* A removes only D; B and C must still run, in order. Compacting the
       table mid-dispatch would drop B and fire C early. */
    CHECK(ev_log_n == 3);
    CHECK(ev_log_user[0] == 1 && ev_log_user[1] == 2 && ev_log_user[2] == 3);
}

/* =====================================================================
   PART B1: flexbox core. Geometry is asserted exactly.
   ===================================================================== */

static void rb_rect(const char *id, int *x, int *y, int *w, int *h) {
    int node = rb_find_id(id);
    if (node <= 0) { *x = *y = *w = *h = -9999; return; }
    if (!rb_dom_rect(node, x, y, w, h)) { *x = *y = *w = *h = -9999; }
}

static void expect_rect(const char *id, int ex, int ey, int ew, int eh) {
    int x, y, w, h;
    rb_rect(id, &x, &y, &w, &h);
    if (x != ex || y != ey || w != ew || h != eh) {
        printf("flex rect %s: got %d,%d %dx%d want %d,%d %dx%d\n",
               id, x, y, w, h, ex, ey, ew, eh);
        fails++;
    }
}

/* Three fixed-width children must land on one row, in order, sharing a y. */
static void test_flex_row(void) {
    const char *html =
        "<div id=r style=\"display:flex;width:300px;height:40px;"
        "background:#222222;align-items:flex-start\">"
        "<div id=a style=\"width:40px;height:20px;background:#ff0000\"></div>"
        "<div id=b style=\"width:50px;height:20px;background:#00ff00\"></div>"
        "<div id=c style=\"width:60px;height:20px;background:#0000ff\"></div>"
        "</div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    expect_rect("a", 0, 0, 40, 20);
    expect_rect("b", 40, 0, 50, 20);
    expect_rect("c", 90, 0, 60, 20);
    int ax, ay, aw, ah, bx, by, bw, bh, cx, cy, cw, ch;
    rb_rect("a", &ax, &ay, &aw, &ah);
    rb_rect("b", &bx, &by, &bw, &bh);
    rb_rect("c", &cx, &cy, &cw, &ch);
    CHECK(ay == by && by == cy);           /* a row, not stacked */
    CHECK(ax + aw <= bx && bx + bw <= cx); /* source order left to right */
    expect_rect("r", 0, 0, 300, 40);       /* container keeps its height */
}

/* justify-content places the three 50px items exactly, container 300. */
static void test_flex_justify(void) {
    static const char *jv[6] = {
        "flex-start", "center", "flex-end", "space-between", "space-around", "space-evenly"
    };
    static const int axr[6] = {0, 75, 150, 0, 25, 37};
    static const int bxr[6] = {50, 125, 200, 125, 125, 124};
    static const int cxr[6] = {100, 175, 250, 250, 225, 211};
    for (int i = 0; i < 6; i++) {
        char html[512];
        snprintf(html, sizeof(html),
            "<div style=\"display:flex;width:300px;justify-content:%s\">"
            "<div id=a style=\"width:50px;height:10px;background:#aa0000\"></div>"
            "<div id=b style=\"width:50px;height:10px;background:#00aa00\"></div>"
            "<div id=c style=\"width:50px;height:10px;background:#0000aa\"></div>"
            "</div>", jv[i]);
        rb_open(html, (int)strlen(html));
        rb_relayout();
        expect_rect("a", axr[i], 0, 50, 10);
        expect_rect("b", bxr[i], 0, 50, 10);
        expect_rect("c", cxr[i], 0, 50, 10);
    }
}

/* gap sits exactly between adjacent items, never before the first or after
   the last. */
static void test_flex_gap(void) {
    const char *html =
        "<div style=\"display:flex;width:300px;gap:10px\">"
        "<div id=a style=\"width:50px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"width:50px;height:10px;background:#00aa00\"></div>"
        "<div id=c style=\"width:50px;height:10px;background:#0000aa\"></div>"
        "</div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    expect_rect("a", 0, 0, 50, 10);
    expect_rect("b", 60, 0, 50, 10);
    expect_rect("c", 120, 0, 50, 10);
    int x, y, w, h;
    rb_rect("c", &x, &y, &w, &h);
    CHECK(x + w == 170);   /* 50+10+50+10+50, no trailing gap */

    /* PART B2: row-gap sits between WRAPPED LINES (cross axis), so on a
       single line it changes nothing; column-gap is the on-line gap. */
    const char *html2 =
        "<div style=\"display:flex;width:300px;row-gap:10px\">"
        "<div id=a style=\"width:50px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"width:50px;height:10px;background:#00aa00\"></div>"
        "</div>";
    rb_open(html2, (int)strlen(html2));
    rb_relayout();
    expect_rect("b", 50, 0, 50, 10);       /* row-gap is not the on-line gap */
    const char *html3 =
        "<div style=\"display:flex;width:300px;column-gap:10px\">"
        "<div id=a style=\"width:50px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"width:50px;height:10px;background:#00aa00\"></div>"
        "</div>";
    rb_open(html3, (int)strlen(html3));
    rb_relayout();
    expect_rect("b", 60, 0, 50, 10);       /* column-gap is */
}

/* flex-direction: column stacks vertically with gaps between. */
static void test_flex_column(void) {
    const char *html =
        "<div id=r style=\"display:flex;flex-direction:column;width:120px;"
        "gap:5px;background:#222222\">"
        "<div id=a style=\"height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"height:20px;background:#00aa00\"></div>"
        "<div id=c style=\"height:30px;background:#0000aa\"></div>"
        "</div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    expect_rect("a", 0, 0, 120, 10);
    expect_rect("b", 0, 15, 120, 20);
    expect_rect("c", 0, 40, 120, 30);
    expect_rect("r", 0, 0, 120, 70);   /* 10+5+20+5+30 */
}

/* flex-grow shares free space in the right proportions. */
static void test_flex_grow(void) {
    const char *html =
        "<div style=\"display:flex;width:300px\">"
        "<div id=a style=\"flex:1 1 0;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"flex:2 1 0;height:10px;background:#00aa00\"></div>"
        "<div id=c style=\"flex:1 1 0;height:10px;background:#0000aa\"></div>"
        "</div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    expect_rect("a", 0, 0, 75, 10);
    expect_rect("b", 75, 0, 150, 10);
    expect_rect("c", 225, 0, 75, 10);

    /* a single flex:1 child fills the container (basis 0, grow absorbs all) */
    const char *one =
        "<div style=\"display:flex;width:300px;height:40px\">"
        "<div id=s style=\"flex:1;background:#aa0000\"></div>"
        "</div>";
    rb_open(one, (int)strlen(one));
    rb_relayout();
    expect_rect("s", 0, 0, 300, 40);

    /* gap is subtracted before the free space is shared */
    const char *g =
        "<div style=\"display:flex;width:300px;gap:20px\">"
        "<div id=a style=\"flex:1 1 0;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"flex:1 1 0;height:10px;background:#00aa00\"></div>"
        "</div>";
    rb_open(g, (int)strlen(g));
    rb_relayout();
    expect_rect("a", 0, 0, 140, 10);      /* (300-20)/2 */
    expect_rect("b", 160, 0, 140, 10);
}

/* flex-shrink shrinks over-wide items; flex:none does not shrink. */
static void test_flex_shrink(void) {
    const char *eq =
        "<div style=\"display:flex;width:300px\">"
        "<div id=a style=\"width:200px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"width:200px;height:10px;background:#00aa00\"></div>"
        "</div>";
    rb_open(eq, (int)strlen(eq));
    rb_relayout();
    expect_rect("a", 0, 0, 150, 10);
    expect_rect("b", 150, 0, 150, 10);

    const char *nn =
        "<div style=\"display:flex;width:300px\">"
        "<div id=a style=\"width:200px;flex:none;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"width:200px;height:10px;background:#00aa00\"></div>"
        "</div>";
    rb_open(nn, (int)strlen(nn));
    rb_relayout();
    expect_rect("a", 0, 0, 200, 10);       /* flex:none -> 0 0 auto: no shrink */
    expect_rect("b", 200, 0, 100, 10);
}

/* align-items on the cross axis; stretch fills an auto cross size. */
static void test_flex_align(void) {
    const char *center =
        "<div style=\"display:flex;width:300px;height:60px;align-items:center\">"
        "<div id=a style=\"width:50px;height:10px;background:#aa0000\"></div>"
        "</div>";
    rb_open(center, (int)strlen(center));
    rb_relayout();
    expect_rect("a", 0, 25, 50, 10);       /* (60-10)/2 */

    const char *end =
        "<div style=\"display:flex;width:300px;height:60px;align-items:flex-end\">"
        "<div id=a style=\"width:50px;height:10px;background:#aa0000\"></div>"
        "</div>";
    rb_open(end, (int)strlen(end));
    rb_relayout();
    expect_rect("a", 0, 50, 50, 10);

    const char *stretch =
        "<div style=\"display:flex;width:300px;height:60px;align-items:stretch\">"
        "<div id=a style=\"width:50px;background:#aa0000\"></div>"
        "</div>";
    rb_open(stretch, (int)strlen(stretch));
    rb_relayout();
    expect_rect("a", 0, 0, 50, 60);        /* auto cross size stretches */

    /* baseline maps to flex-start */
    const char *base =
        "<div style=\"display:flex;width:300px;height:60px;align-items:baseline\">"
        "<div id=a style=\"width:50px;height:10px;background:#aa0000\"></div>"
        "</div>";
    rb_open(base, (int)strlen(base));
    rb_relayout();
    expect_rect("a", 0, 0, 50, 10);

    /* an explicit cross size is not overridden by stretch */
    const char *fixed =
        "<div style=\"display:flex;width:300px;height:60px;align-items:stretch\">"
        "<div id=a style=\"width:50px;height:10px;background:#aa0000\"></div>"
        "</div>";
    rb_open(fixed, (int)strlen(fixed));
    rb_relayout();
    expect_rect("a", 0, 0, 50, 10);
}

/* align-self overrides the container's align-items for one item. */
static void test_flex_align_self(void) {
    const char *html =
        "<div style=\"display:flex;width:300px;height:60px;align-items:flex-start\">"
        "<div id=a style=\"width:50px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"width:50px;height:10px;background:#00aa00;align-self:flex-end\"></div>"
        "<div id=c style=\"width:50px;height:10px;background:#0000aa;align-self:center\"></div>"
        "</div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    expect_rect("a", 0, 0, 50, 10);
    expect_rect("b", 50, 50, 50, 10);
    expect_rect("c", 100, 25, 50, 10);
}

/* A display:none child is not an item and must not create a gap. */
static void test_flex_none_child(void) {
    const char *html =
        "<div style=\"display:flex;width:300px;gap:10px\">"
        "<div id=a style=\"width:50px;height:10px;background:#aa0000\"></div>"
        "<div id=n style=\"display:none;width:50px\"></div>"
        "<div id=c style=\"width:50px;height:10px;background:#0000aa\"></div>"
        "</div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    expect_rect("a", 0, 0, 50, 10);
    expect_rect("c", 60, 0, 50, 10);
    int x, y, w, h;
    rb_rect("n", &x, &y, &w, &h);
    CHECK(x == -9999);   /* no box at all */
}

/* flex shorthand and flex-basis forms. */
static void test_flex_shorthand(void) {
    /* flex: 1 is 1 1 0, not 1 1 auto */
    const char *one =
        "<div style=\"display:flex;width:200px\">"
        "<div id=a style=\"flex:1;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"flex:1;height:10px;background:#00aa00\"></div>"
        "</div>";
    rb_open(one, (int)strlen(one));
    rb_relayout();
    expect_rect("a", 0, 0, 100, 10);
    expect_rect("b", 100, 0, 100, 10);

    /* flex: 1 30px is 1 1 30px */
    const char *two =
        "<div style=\"display:flex;width:300px\">"
        "<div id=a style=\"flex:1 30px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"flex:1 30px;height:10px;background:#00aa00\"></div>"
        "</div>";
    rb_open(two, (int)strlen(two));
    rb_relayout();
    expect_rect("a", 0, 0, 150, 10);
    expect_rect("b", 150, 0, 150, 10);

    /* flex: initial is 0 1 auto: no growth */
    const char *init =
        "<div style=\"display:flex;width:300px\">"
        "<div id=a style=\"flex:initial;width:200px;height:10px;background:#aa0000\"></div>"
        "</div>";
    rb_open(init, (int)strlen(init));
    rb_relayout();
    expect_rect("a", 0, 0, 200, 10);

    /* flex-basis percentage resolves against the container, fill rest */
    const char *pct =
        "<div style=\"display:flex;width:200px\">"
        "<div id=a style=\"flex-basis:50%;flex-grow:0;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"flex-basis:50px;flex-grow:0;height:10px;background:#00aa00\"></div>"
        "<div id=c style=\"flex-grow:1;height:10px;background:#0000aa\"></div>"
        "</div>";
    rb_open(pct, (int)strlen(pct));
    rb_relayout();
    expect_rect("a", 0, 0, 100, 10);
    expect_rect("b", 100, 0, 50, 10);
    expect_rect("c", 150, 0, 50, 10);
}

/* All four flex-direction values are real in PART B2: the reverse variants
   swap the main axis start/end, so items flow from the opposite edge and
   (column-reverse) an auto-height container is still content-sized.
   inline-flex is treated as flex (documented fallback). */
static void test_flex_fallbacks(void) {
    /* row-reverse: main start is the RIGHT edge; a lands at 200-40 and b
       further left. */
    const char *rev =
        "<div style=\"display:flex;flex-direction:row-reverse;width:200px\">"
        "<div id=a style=\"width:40px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"width:40px;height:10px;background:#00aa00\"></div>"
        "</div>";
    rb_open(rev, (int)strlen(rev));
    rb_relayout();
    expect_rect("a", 160, 0, 40, 10);
    expect_rect("b", 120, 0, 40, 10);

    /* column-reverse: main start is the BOTTOM edge; the 30px content block
       stacks bottom-up, so b (20px) is on top. */
    const char *crev =
        "<div id=r style=\"display:flex;flex-direction:column-reverse;width:120px;background:#222222\">"
        "<div id=a style=\"height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"height:20px;background:#00aa00\"></div>"
        "</div>";
    rb_open(crev, (int)strlen(crev));
    rb_relayout();
    expect_rect("a", 0, 20, 120, 10);
    expect_rect("b", 0, 0, 120, 20);
    int ax, ay, aw, ah, bx, by, bw, bh;
    rb_rect("a", &ax, &ay, &aw, &ah);
    rb_rect("b", &bx, &by, &bw, &bh);
    CHECK(ay > by);                        /* first item below second */
    expect_rect("r", 0, 0, 120, 30);       /* content-sized, not flipped */

    const char *inl =
        "<div style=\"display:inline-flex;width:200px\">"
        "<div id=a style=\"width:40px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"width:40px;height:10px;background:#00aa00\"></div>"
        "</div>";
    rb_open(inl, (int)strlen(inl));
    rb_relayout();
    expect_rect("a", 0, 0, 40, 10);
    expect_rect("b", 40, 0, 40, 10);
}

/* A flex container nested in a block and a block nested inside a flex item. */
static void test_flex_nested(void) {
    const char *html =
        "<div style=\"padding:4px\">"
        "<div id=r style=\"display:flex;gap:8px\">"
        "<div id=a style=\"width:40px;background:#aa0000\">"
        "<div id=inner style=\"height:9px;background:#111111\"></div></div>"
        "<div id=b style=\"flex:1;background:#00aa00\"></div>"
        "</div></div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    expect_rect("a", 4, 4, 40, 9);
    expect_rect("inner", 4, 4, 40, 9);
    expect_rect("b", 52, 4, 248, 9);       /* 296 - 40 - 8 */
}

/* =====================================================================
    PART B2: wrap, align-content, reverse, min/max, order, two-axis gap.
    Geometry is asserted exactly.
    ===================================================================== */

/* flex-wrap: wrap. Three 80px items in a 200px container: a+b fill line 1,
   c wraps to line 2. align-content variants then move the second line; with
   an explicit 60px height the default (stretch) grows both lines by 20. */
static void test_flex_wrap(void) {
    static const char *av[7] = {
        "stretch", "flex-start", "flex-end", "center", "space-between",
        "space-around", "space-evenly"
    };
    /* the second line's y for each variant: free = 60-20 = 40 */
    static const int ay[7] = {0, 0, 40, 20, 0, 10, 13};
    static const int cy[7] = {30, 10, 50, 30, 50, 40, 36};
    for (int i = 0; i < 7; i++) {
        char html[512];
        snprintf(html, sizeof(html),
            "<div style=\"display:flex;flex-wrap:wrap;align-content:%s;"
            "width:200px;height:60px\">"
            "<div id=a style=\"width:80px;height:10px;background:#aa0000\"></div>"
            "<div id=b style=\"width:80px;height:10px;background:#00aa00\"></div>"
            "<div id=c style=\"width:80px;height:10px;background:#0000aa\"></div>"
            "</div>", av[i]);
        rb_open(html, (int)strlen(html));
        rb_relayout();
        expect_rect("a", 0, ay[i], 80, 10);
        expect_rect("b", 80, ay[i], 80, 10);
        expect_rect("c", 0, cy[i], 80, 10);
    }
}

/* flex-wrap: wrap-reverse stacks the lines bottom-up INSIDE the same block:
   with an auto-height 20px container line 0 sits at the bottom. With an
   explicit height and the default align-content:stretch the mirrored block
   is full height; align-content: flex-end keeps line 0 at the bottom. */
static void test_flex_wrap_reverse(void) {
    const char *html =
        "<div id=r style=\"display:flex;flex-wrap:wrap-reverse;width:200px;"
        "background:#222222\">"
        "<div id=a style=\"width:80px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"width:80px;height:10px;background:#00aa00\"></div>"
        "<div id=c style=\"width:80px;height:10px;background:#0000aa\"></div>"
        "</div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    expect_rect("a", 0, 10, 80, 10);       /* first line at the cross end */
    expect_rect("b", 80, 10, 80, 10);
    expect_rect("c", 0, 0, 80, 10);        /* second line towards the start */
    expect_rect("r", 0, 0, 200, 20);

    /* explicit height 60, align-content:flex-start: the mirrored block is
       packed from the bottom (both lines 10px, free 40 below not visible:
       stretch is replaced, so line 0 sits at 60-10=50). */
    const char *h60 =
        "<div style=\"display:flex;flex-wrap:wrap-reverse;align-content:flex-start;"
        "width:200px;height:60px\">"
        "<div id=a style=\"width:80px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"width:80px;height:10px;background:#00aa00\"></div>"
        "<div id=c style=\"width:80px;height:10px;background:#0000aa\"></div>"
        "</div>";
    rb_open(h60, (int)strlen(h60));
    rb_relayout();
    expect_rect("a", 0, 50, 80, 10);
    expect_rect("c", 0, 40, 80, 10);

    /* wrap-reverse also swaps the cross-start of the alignment INSIDE a
       line: align-items:flex-start hugs the line's BOTTOM edge. The 30px
       item wraps; align-content:flex-start (the top in forward terms)
       consequently packs the mirrored block to the bottom. */
    const char *al =
        "<div style=\"display:flex;flex-wrap:wrap-reverse;align-content:flex-start;"
        "align-items:flex-start;width:200px;height:60px\">"
        "<div id=a style=\"width:80px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"width:80px;height:10px;background:#00aa00\"></div>"
        "<div id=c style=\"width:45px;height:30px;background:#0000aa\"></div>"
        "</div>";
    rb_open(al, (int)strlen(al));
    rb_relayout();
    /* cross is 60: line 0 {a,b} h10 mirrors to y50, line 1 {c} h30 to y20 —
       the packed block (10 + 30) hugs the bottom, lines bottom-up;
       flex-start inside a reversed line hugs the line's bottom edge. */
    expect_rect("a", 0, 50, 80, 10);
    expect_rect("b", 80, 50, 80, 10);
    expect_rect("c", 0, 20, 45, 30);
}

/* min/max-width clamps a growing flex item, a shrinking one and a plain
   block; min/max-height clamps a plain block's inner height. */
static void test_flex_min_max(void) {
    /* grow + max-width: a wants all 300 but tops out at 100. */
    const char *grow =
        "<div style=\"display:flex;width:300px\">"
        "<div id=a style=\"flex:1;max-width:100px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"flex:1;height:10px;background:#00aa00\"></div>"
        "</div>";
    rb_open(grow, (int)strlen(grow));
    rb_relayout();
    expect_rect("a", 0, 0, 100, 10);
    expect_rect("b", 100, 0, 200, 10);

    /* shrink + min-width: a must not shrink below 170, so b gives more. */
    const char *shrink =
        "<div style=\"display:flex;width:200px\">"
        "<div id=a style=\"width:180px;min-width:170px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"width:40px;height:10px;background:#00aa00\"></div>"
        "</div>";
    rb_open(shrink, (int)strlen(shrink));
    rb_relayout();
    expect_rect("a", 0, 0, 170, 10);
    expect_rect("b", 170, 0, 30, 10);

    /* plain blocks: max-width caps an over-wide block, min-width floors one
       whose container is narrower, min-height floors short content,
       max-height caps an explicit height. */
    const char *plain =
        "<div style=\"width:200px\">"
        "<div id=m style=\"max-width:150px;background:#aa0000\">hello world long text here</div>"
        "<div id=p style=\"min-height:50px;background:#0000aa\">x</div>"
        "<div id=q style=\"height:100px;max-height:30px;background:#00aaaa\"></div>"
        "</div>"
        "<div style=\"width:150px\">"
        "<div id=n style=\"min-width:180px;background:#00aa00\">x</div>"
        "</div>";
    rb_open(plain, (int)strlen(plain));
    rb_relayout();
    int x, y, w, h;
    rb_rect("m", &x, &y, &w, &h);
    CHECK(w == 150);                       /* capped despite wide content */
    rb_rect("n", &x, &y, &w, &h);
    CHECK(w == 180);                       /* floored above its 150 container */
    rb_rect("p", &x, &y, &w, &h);
    CHECK(h == 50);
    rb_rect("q", &x, &y, &w, &h);
    CHECK(h == 30);
}

/* order sorts items visually; equal order keeps SOURCE order (stability). */
static void test_flex_order(void) {
    const char *html =
        "<div style=\"display:flex;width:300px\">"
        "<div id=a style=\"order:2;width:40px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"order:0;width:40px;height:10px;background:#00aa00\"></div>"
        "<div id=c style=\"order:0;width:40px;height:10px;background:#0000aa\"></div>"
        "</div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    expect_rect("b", 0, 0, 40, 10);        /* b and c tie: source order wins */
    expect_rect("c", 40, 0, 40, 10);
    expect_rect("a", 80, 0, 40, 10);       /* order:2 goes last */

    /* negative order sorts before unset (0). */
    const char *neg =
        "<div style=\"display:flex;width:300px\">"
        "<div id=a style=\"width:40px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"order:-1;width:40px;height:10px;background:#00aa00\"></div>"
        "</div>";
    rb_open(neg, (int)strlen(neg));
    rb_relayout();
    expect_rect("b", 0, 0, 40, 10);
    expect_rect("a", 40, 0, 40, 10);
}

/* row-gap separates lines, column-gap separates items on a line — in a
   wrapped row container each axis must use its own value. */
static void test_flex_gap_axes(void) {
    const char *html =
        "<div style=\"display:flex;flex-wrap:wrap;width:120px;"
        "row-gap:5px;column-gap:10px\">"
        "<div id=a style=\"width:50px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"width:50px;height:10px;background:#00aa00\"></div>"
        "<div id=c style=\"width:50px;height:10px;background:#0000aa\"></div>"
        "</div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    expect_rect("a", 0, 0, 50, 10);
    expect_rect("b", 60, 0, 50, 10);       /* column-gap on the line */
    expect_rect("c", 0, 15, 50, 10);       /* row-gap between lines */
}

/* A column container wraps only against an explicit height; its wrapped
   lines stack HORIZONTALLY (that is the column cross axis) and overflow the
   120px box, exactly like a real browser would overflow it. */
static void test_flex_column_wrap(void) {
    const char *html =
        "<div id=r style=\"display:flex;flex-direction:column;flex-wrap:wrap;"
        "width:120px;height:25px;background:#222222\">"
        "<div id=a style=\"height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"height:20px;background:#00aa00\"></div>"
        "</div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    /* a (10) fits on line 1; b (10+20 > 25) wraps to a second column at
       x=120. Both items stretch to the full line cross (120). */
    expect_rect("a", 0, 0, 120, 10);
    expect_rect("b", 120, 0, 120, 20);
    expect_rect("r", 0, 0, 120, 25);       /* height honoured, not exceeded */
}

/* B1/B2 integration: a flex:1 item (flex-basis:0) must measure its cross size
   at the width it is actually laid out at. B1's Pass A measured at the base
   size, so basis 0 pinned the item to 0px wide -> an 8px-floored layout whose
   content height hit the 200px cap. The same staleness made a grown
   fixed-width item keep the height it had at its pre-grow width. */
static void test_flex_basis0_cross(void) {
    const char *one =
        "<div style=\"display:flex;width:200px\">"
        "<div id=a style=\"flex:1;background:#aa0000\">hello world this is some text</div>"
        "</div>"
        "<div style=\"display:flex;width:200px\">"
        "<div id=b style=\"width:200px;background:#00aa00\">hello world this is some text</div>"
        "</div>";
    rb_open(one, (int)strlen(one));
    rb_relayout();
    int ax, ay, aw, ah, bx, by, bw, bh;
    rb_rect("a", &ax, &ay, &aw, &ah);
    rb_rect("b", &bx, &by, &bw, &bh);
    CHECK(aw == 200);
    CHECK(ah == bh);          /* same width -> same wrapped height */
    CHECK(ah < 40);           /* never the 200px cap */

    const char *grow =
        "<div style=\"display:flex;width:200px\">"
        "<div id=c style=\"flex-grow:1;width:100px;background:#aa0000\">hello world this is some text</div>"
        "</div>"
        "<div style=\"display:flex;width:200px\">"
        "<div id=d style=\"width:200px;background:#00aa00\">hello world this is some text</div>"
        "</div>";
    rb_open(grow, (int)strlen(grow));
    rb_relayout();
    int cx, cy, cw, ch, dx, dy, dw, dh;
    rb_rect("c", &cx, &cy, &cw, &ch);
    rb_rect("d", &dx, &dy, &dw, &dh);
    CHECK(cw == 200);
    CHECK(ch == dh);          /* re-measured after growing to the full width */
    CHECK(ch < 40);
}

/* B1/B2 integration: line bookkeeping is indexed from the container's slice
   base (off), so a WRAPPING container nested inside another flex item must
   not read the parent's items at relative index 0. Before the fix the inner
   lines resolved against the outer item's sizes and items collapsed. */
static void test_flex_nested_wrap(void) {
    const char *html =
        "<div id=outer style=\"display:flex;width:200px;background:#202020\">"
        "<div id=inner style=\"display:flex;flex-wrap:wrap;width:60px;background:#303030\">"
        "<div id=a style=\"width:40px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"width:40px;height:10px;background:#00aa00\"></div>"
        "<div id=c style=\"width:40px;height:10px;background:#0000aa\"></div>"
        "</div></div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    expect_rect("a", 0, 0, 40, 10);
    expect_rect("b", 0, 10, 40, 10);       /* three stacked lines, full width */
    expect_rect("c", 0, 20, 40, 10);
    expect_rect("inner", 0, 0, 60, 30);
    expect_rect("outer", 0, 0, 200, 30);

    /* the same nesting with wrap-reverse: lines mirror to the container end
       but must still be the inner container's own items. */
    const char *rev =
        "<div id=outer style=\"display:flex;width:200px;background:#202020\">"
        "<div id=inner style=\"display:flex;flex-wrap:wrap;flex-direction:row-reverse;"
        "width:60px;background:#303030\">"
        "<div id=a style=\"width:40px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"width:40px;height:10px;background:#00aa00\"></div>"
        "</div></div>";
    rb_open(rev, (int)strlen(rev));
    rb_relayout();
    expect_rect("a", 20, 0, 40, 10);       /* row-reverse: x = 60-40 */
    expect_rect("b", 20, 10, 40, 10);
    expect_rect("inner", 0, 0, 60, 20);
}

/* order + wrap + reverse simultaneously: the sort happens before line
   breaking, lines pack from the reversed main start, and the reverse must not
   be applied twice. */
static void test_flex_order_wrap_reverse(void) {
    const char *html =
        "<div id=r style=\"display:flex;flex-wrap:wrap;flex-direction:row-reverse;"
        "width:100px;background:#202020\">"
        "<div id=a style=\"order:2;width:60px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"order:1;width:60px;height:10px;background:#00aa00\"></div>"
        "<div id=c style=\"width:60px;height:10px;background:#0000aa\"></div>"
        "</div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    /* sorted c(0),b(1),a(2) -> c on line 0, b on line 1, a on line 2; each
       line starts at the reversed main start (right edge, x = 100-60). */
    expect_rect("c", 40, 0, 60, 10);
    expect_rect("b", 40, 10, 60, 10);
    expect_rect("a", 40, 20, 60, 10);
    expect_rect("r", 0, 0, 100, 30);

    /* no order: the first SOURCE child still leads the reversed row (right). */
    const char *plain =
        "<div id=r style=\"display:flex;flex-wrap:wrap;flex-direction:row-reverse;"
        "width:100px;background:#202020\">"
        "<div id=a style=\"width:60px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"width:60px;height:10px;background:#00aa00\"></div>"
        "</div>";
    rb_open(plain, (int)strlen(plain));
    rb_relayout();
    expect_rect("a", 40, 0, 60, 10);
    expect_rect("b", 40, 10, 60, 10);
}

/* Seam guard: a stretched auto-cross item fills its OWN line's cross size,
   including the last line, whose cross is the tallest item on it. */
static void test_flex_wrap_stretch_cross(void) {
    const char *html =
        "<div id=r style=\"display:flex;flex-wrap:wrap;width:100px;"
        "align-items:stretch;background:#202020\">"
        "<div id=a style=\"width:50px;background:#aa0000\">xx</div>"
        "<div id=b style=\"width:50px;background:#00aa00\">xx<br>xx<br>xx</div>"
        "<div id=c style=\"width:50px;background:#0000aa\">xx</div>"
        "</div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    /* a+b share line 0 (100px exactly): its cross is b's 27px, so a stretches
       to 27 too. c wraps to line 1, whose cross is only 9. */
    expect_rect("a", 0, 0, 50, 27);
    expect_rect("b", 50, 0, 50, 27);
    expect_rect("c", 0, 27, 50, 9);
    expect_rect("r", 0, 0, 100, 36);
}

/* Seam guard: a flex container inside a plain block, a plain block inside a
   flex item, a display:none item (no gap reserved) and controls as flex items
   all coexist with the focus list. */
static void test_flex_interactions(void) {
    /* display:none item consumes no gap, flex row inside a plain block. */
    const char *html =
        "<div id=o style=\"width:200px;background:#111111\">"
        "<div id=r style=\"display:flex;width:120px;background:#202020\">"
        "<div id=a style=\"width:30px;height:10px;background:#aa0000\"></div>"
        "<div id=none style=\"display:none;width:30px;height:10px\"></div>"
        "<div id=b style=\"width:30px;height:10px;background:#00aa00\"></div>"
        "</div></div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    expect_rect("o", 0, 0, 200, 10);       /* block contains the flex row */
    expect_rect("r", 0, 0, 120, 10);
    expect_rect("a", 0, 0, 30, 10);
    expect_rect("b", 30, 0, 30, 10);       /* display:none item left no gap */
    int x, y, w, h;
    rb_rect("none", &x, &y, &w, &h);
    CHECK(x == -9999);                     /* no boxes for the hidden child */

    /* controls as flex items still land in the focus list, in source order,
       sharing the row. */
    const char *ctl =
        "<div id=r style=\"display:flex;width:200px;background:#202020\">"
        "<button id=k>OK</button><input id=in value=\"hi\">"
        "</div>";
    rb_open(ctl, (int)strlen(ctl));
    rb_relayout();
    CHECK(rb_focus_count() == 2);
    CHECK(rb_node_tag(rb_focus_node(0)) == RB_TAG_BUTTON);
    CHECK(rb_node_tag(rb_focus_node(1)) == RB_TAG_INPUT);
    int kx, ky, kw, kh, ix, iy, iw, ih;
    rb_rect("k", &kx, &ky, &kw, &kh);
    rb_rect("in", &ix, &iy, &iw, &ih);
    CHECK(ky == iy);                       /* one flex row */
    CHECK(kx + kw <= ix);                  /* source order, no overlap */

    /* a plain block with a <p> inside a column flex item keeps both boxes. */
    const char *col =
        "<div id=r2 style=\"display:flex;flex-direction:column;width:100px;"
        "background:#202020\">"
        "<div id=w style=\"background:#aa0000\">"
        "<p id=p style=\"background:#00aa00\">hello</p></div>"
        "</div>";
    rb_open(col, (int)strlen(col));
    rb_relayout();
    expect_rect("r2", 0, 0, 100, 13);
    expect_rect("w", 0, 0, 100, 13);
    expect_rect("p", 0, 0, 100, 9);
}

/* Pass A2 re-measures a grown row item by laying it out again; if that item
   is a WRAPPING flex container it recycles the shared line scratch. The line
   cross must then be recovered from each item's own line, not from that
   scratch, or the parent's line collapses to the nested container's numbers. */
static void test_flex_nested_remeasure(void) {
    const char *html =
        "<div id=outer style=\"display:flex;width:200px;background:#202020;"
        "align-items:stretch\">"
        "<div id=g style=\"flex-grow:1;width:50px;background:#303030\">"
        "<div id=inner style=\"display:flex;flex-wrap:wrap;width:100px;background:#404040\">"
        "<div id=a style=\"width:60px;height:10px;background:#aa0000\"></div>"
        "<div id=b style=\"width:60px;height:10px;background:#00aa00\"></div>"
        "</div></div>"
        "<div id=other style=\"width:20px;height:40px;background:#0000aa\"></div>"
        "</div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    /* g grows 50 -> 180, so its cross is re-measured (inner becomes one line);
       the line cross is still max(g=10, other=40) = 40, so g stretches to 40. */
    expect_rect("outer", 0, 0, 200, 40);
    expect_rect("g", 0, 0, 180, 40);
    expect_rect("other", 180, 0, 20, 40);
    expect_rect("inner", 0, 0, 100, 20);
    expect_rect("a", 0, 0, 60, 10);
    expect_rect("b", 0, 10, 60, 10);
}

/* P0 #1: a stretched item's rendered box must fill its line, not exceed it by
   its own padding+margins. Line 0 is 19 tall; the padded item (padding:4px)
   must be 19 tall too, so line 1 (y=19) does not overlap it. */
static void test_flex_stretch_padding(void) {
    const char *html =
        "<div id=r style=\"display:flex;flex-wrap:wrap;width:100px;height:40px;"
        "align-items:stretch;background:#202020\">"
        "<div id=a style=\"width:50px;padding:4px;background:#aa0000\"></div>"
        "<div id=b style=\"width:50px;background:#00aa00\">x</div>"
        "<div id=c style=\"width:100px;height:10px;background:#0000aa\"></div>"
        "</div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    expect_rect("a", 0, 0, 50, 19);
    expect_rect("b", 50, 0, 50, 19);
    expect_rect("c", 0, 19, 100, 10);      /* starts where line 0 ends */
    int ax, ay, aw, ah, cx, cy, cw, ch;
    rb_rect("a", &ax, &ay, &aw, &ah);
    rb_rect("c", &cx, &cy, &cw, &ch);
    CHECK(ay + ah <= cy);                  /* no overlap into line 1 */
}

/* P0 #2: an auto-basis control must be measured at its intrinsic size, not the
   whole flex slot. Otherwise an empty <input> claims the row and shrinks the
   links to the 8px floor (one glyph per line). */
static void test_flex_control_autobasis(void) {
    const char *html =
        "<div id=r style=\"display:flex;background:#202020\">"
        "<input id=in><input type=submit id=s value=Go>"
        "<a id=a href=/x>Home</a><a id=b href=/y>About</a></div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    int x, y, w, h;
    rb_rect("in", &x, &y, &w, &h);
    CHECK(w <= 210);                       /* intrinsic, not ~303 */
    rb_rect("a", &x, &y, &w, &h);
    CHECK(w >= 24);                        /* "Home" is not crushed to 8px */
    rb_rect("b", &x, &y, &w, &h);
    CHECK(w >= 30);                        /* "About" (5 glyphs) intact */

    /* The minimal reviewer case: anchor keeps its own width. */
    const char *min =
        "<div style=\"display:flex\"><input id=i2><a id=a2 href=/x>Home</a></div>";
    rb_open(min, (int)strlen(min));
    rb_relayout();
    rb_rect("i2", &x, &y, &w, &h);
    CHECK(w <= 210);
    rb_rect("a2", &x, &y, &w, &h);
    CHECK(w >= 24);
}

/* P1 #3: a percentage cross clamp resolves against the container's DEFINITE
   cross extent (here an explicit 100px height), so max-height:50% caps a tall
   item at 50, not at 50% of the width (a silent no-op). */
static void test_flex_cross_pct(void) {
    const char *html =
        "<div id=r style=\"display:flex;width:200px;height:100px;"
        "align-items:flex-start;background:#202020\">"
        "<div id=a style=\"width:60px;max-height:50%;background:#aa0000\">"
        "x<br>x<br>x<br>x<br>x<br>x<br>x<br>x<br>x<br>x</div>"
        "<div id=b style=\"width:60px;background:#00aa00\">x</div></div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    expect_rect("a", 0, 0, 60, 50);
    expect_rect("b", 60, 0, 60, 9);

    /* An INDEFINITE (auto-height) row leaves the percentage unresolved. */
    const char *auto_h =
        "<div id=r style=\"display:flex;width:200px;align-items:flex-start\">"
        "<div id=c style=\"width:60px;max-height:50%;background:#aa0000\">"
        "x<br>x<br>x<br>x<br>x</div></div>";
    rb_open(auto_h, (int)strlen(auto_h));
    rb_relayout();
    int x, y, w, h;
    rb_rect("c", &x, &y, &w, &h);
    CHECK(h > 20);                         /* not capped against an unknown extent */
}

/* P1 #4: a content-sized column is sized BY its items, so a tall stack must
   not be shrunk to a 2000px cap. Three 800px items -> a 2400px page. */
static void test_flex_auto_column_tall(void) {
    const char *html =
        "<div id=r style=\"display:flex;flex-direction:column;width:100px;"
        "background:#202020\">"
        "<div id=a style=\"height:800px;background:#aa0000\"></div>"
        "<div id=b style=\"height:800px;background:#00aa00\"></div>"
        "<div id=c style=\"height:800px;background:#0000aa\"></div></div>";
    rb_open(html, (int)strlen(html));
    rb_relayout();
    CHECK(rb_page_height() == 2400);
    expect_rect("r", 0, 0, 100, 2400);
    int x, y, w, h;
    rb_rect("a", &x, &y, &w, &h);
    CHECK(y == 0);
    rb_rect("b", &x, &y, &w, &h);
    CHECK(y == 800);
    rb_rect("c", &x, &y, &w, &h);
    CHECK(y == 1600);
}

/* P2 #5: more children than FLEX_ITEMS_PER are dropped, and the truncation is
   reported through rb_oom() (it used to be silent). */
static void test_flex_item_cap_oom(void) {
    static char html[32768];
    int n = 0;
    const char *pre = "<div style=\"display:flex\">";
    while (*pre) html[n++] = *pre++;
    for (int i = 0; i < 270; i++) {
        n += snprintf(html + n, sizeof(html) - (size_t)n,
                      "<div id=f%d style=\"width:4px;height:4px;background:#aa0000\"></div>", i);
    }
    html[n++] = '<'; html[n++] = '/'; html[n++] = 'd'; html[n++] = 'i'; html[n++] = 'v'; html[n++] = '>';
    html[n] = 0;
    rb_open(html, n);
    rb_relayout();
    CHECK(rb_oom() == 1);                  /* the cap is observable */
    CHECK(rb_find_id("f0") > 0);           /* first still laid out */
    int x, y, w, h;
    CHECK(rb_dom_rect(rb_find_id("f255"), &x, &y, &w, &h));   /* slot 256 kept */
    CHECK(!rb_dom_rect(rb_find_id("f256"), &x, &y, &w, &h));  /* 257th dropped */
}

/* =====================================================================
   PART C1: positioning, stacking, overflow, visibility, opacity, cursor.
   Geometry is asserted exactly; coordinates are document space.
   ===================================================================== */

static int c1_open(const char *html) {
    rb_open(html, (int)strlen(html));
    rb_relayout();
    return rb_oom();
}

static int c1_count(const char *id) {
    int node = rb_find_id(id), n = 0;
    for (int i = 0; i < rb_box_count(); i++) if (rb_box(i)->node == node) n++;
    return n;
}

/* Index of the first box owned by `id`, or -1. */
static int c1_first(const char *id) {
    int node = rb_find_id(id);
    for (int i = 0; i < rb_box_count(); i++) if (rb_box(i)->node == node) return i;
    return -1;
}

static void test_c1_relative(void) {
    CHECK(!c1_open(
        "<div id=p style=\"width:200px;background:#111111\">"
        "<div id=a style=\"height:10px;background:#ff0000\"></div>"
        "<div id=b style=\"position:relative;top:5px;left:7px;width:50px;height:10px;"
        "background:#00ff00\"></div>"
        "<div id=c style=\"height:10px;background:#0000ff\"></div></div>"));
    expect_rect("a", 0, 0, 200, 10);
    expect_rect("b", 7, 15, 50, 10);          /* moved by the offsets only */
    expect_rect("c", 0, 20, 200, 10);         /* siblings do not notice */
    expect_rect("p", 0, 0, 200, 30);          /* parent height unchanged */
    CHECK(rb_style_position(rb_find_id("b")) == RB_POS_RELATIVE);

    /* bottom/right with top/left auto move the other way. */
    CHECK(!c1_open(
        "<div id=b style=\"position:relative;bottom:3px;right:4px;width:50px;height:10px;"
        "background:#00ff00\"></div>"));
    expect_rect("b", -4, -3, 50, 10);
    /* top wins over bottom, left over right. */
    CHECK(!c1_open(
        "<div id=b style=\"position:relative;top:2px;bottom:9px;left:3px;right:9px;width:50px;"
        "height:10px;background:#00ff00\"></div>"));
    expect_rect("b", 3, 2, 50, 10);
    /* Negative px offsets survive parsing. */
    CHECK(!c1_open(
        "<div id=b style=\"position:relative;top:-4px;left:-6px;width:50px;height:10px;"
        "background:#00ff00\"></div>"));
    expect_rect("b", -6, -4, 50, 10);
    /* Horizontal % is the parent's width. */
    CHECK(!c1_open(
        "<div style=\"width:200px\"><div id=b style=\"position:relative;left:50%;width:50px;"
        "height:10px;background:#00ff00\"></div></div>"));
    expect_rect("b", 100, 0, 50, 10);
    /* sticky is relative, and says so. */
    CHECK(!c1_open("<div id=b style=\"position:sticky;top:6px;width:20px;height:10px;"
                   "background:#00ff00\"></div>"));
    expect_rect("b", 0, 6, 20, 10);
    CHECK(rb_style_position(rb_find_id("b")) == RB_POS_STICKY);
    /* An inline relative element moves its text, not its neighbours. */
    CHECK(!c1_open("<p>ab<span id=s style=\"position:relative;top:4px\">cd</span>ef</p>"));
    int x, y, w, h, x2, y2;
    int cdx0 = 0, efx0 = 0;
    CHECK(c1_open("<p>ab<span id=s>cd</span>ef</p>") == 0);
    CHECK(find_text("cd", &cdx0, &y2, &w, &h) && find_text("ef", &efx0, &y2, &w, &h));
    CHECK(!c1_open("<p>ab<span id=s style=\"position:relative;top:4px\">cd</span>ef</p>"));
    CHECK(find_text("ab", &x, &y, &w, &h));
    CHECK(find_text("cd", &x2, &y2, &w, &h));
    CHECK(y2 == y + 4 && x2 == cdx0);          /* moved down, not sideways */
    CHECK(find_text("ef", &x2, &y2, &w, &h));
    CHECK(y2 == y && x2 == efx0);              /* the next word does not notice */
}

static void test_c1_absolute(void) {
    /* Out of flow: the sibling closes up and the parent ignores it. */
    CHECK(!c1_open(
        "<div id=p style=\"position:relative;width:200px;background:#111111\">"
        "<div id=x style=\"position:absolute;top:4px;left:6px;width:30px;height:10px;"
        "background:#ff0000\"></div>"
        "<div id=s style=\"height:12px;background:#00ff00\"></div></div>"));
    expect_rect("x", 6, 4, 30, 10);
    expect_rect("s", 0, 0, 200, 12);
    expect_rect("p", 0, 0, 200, 12);

    /* Offsets are from the padding box of the nearest positioned ancestor. */
    CHECK(!c1_open(
        "<div style=\"margin-left:20px;margin-top:10px\">"
        "<div id=p style=\"position:relative;padding:5px;width:200px;height:40px;"
        "background:#111111\">"
        "<div id=x style=\"position:absolute;top:2px;left:3px;width:30px;height:10px;"
        "background:#ff0000\"></div></div></div>"));
    expect_rect("p", 20, 10, 200, 50);
    expect_rect("x", 23, 12, 30, 10);

    /* No positioned ancestor: the initial containing block (document origin). */
    CHECK(!c1_open(
        "<div style=\"margin-left:20px;margin-top:10px\">"
        "<div id=x style=\"position:absolute;top:4px;left:6px;width:30px;height:10px;"
        "background:#ff0000\"></div></div>"));
    expect_rect("x", 6, 4, 30, 10);
    /* ... and bottom anchors to the viewport height. */
    CHECK(!c1_open("<div id=x style=\"position:absolute;bottom:0;right:0;width:30px;height:10px;"
                   "background:#ff0000\"></div>"));
    expect_rect("x", RB_VIEW_W - 30, RB_VIEW_H - 10, 30, 10);

    /* Far-edge anchors. */
    CHECK(!c1_open(
        "<div id=p style=\"position:relative;width:200px;height:60px;background:#111111\">"
        "<div id=x style=\"position:absolute;right:10px;bottom:5px;width:30px;height:10px;"
        "background:#ff0000\"></div></div>"));
    expect_rect("x", 160, 45, 30, 10);
    /* left+right stretches an auto width; top+bottom an auto height. */
    CHECK(!c1_open(
        "<div id=p style=\"position:relative;width:200px;height:60px;background:#111111\">"
        "<div id=x style=\"position:absolute;left:10px;right:20px;height:8px;"
        "background:#ff0000\"></div>"
        "<div id=y style=\"position:absolute;top:5px;bottom:5px;left:0;width:30px;"
        "background:#00ff00\"></div></div>"));
    expect_rect("x", 10, 0, 170, 8);
    expect_rect("y", 0, 5, 30, 50);
    /* Percent offsets against the containing block. */
    CHECK(!c1_open(
        "<div id=p style=\"position:relative;width:200px;height:60px;background:#111111\">"
        "<div id=x style=\"position:absolute;left:50%;top:50%;width:10px;height:10px;"
        "background:#ff0000\"></div></div>"));
    expect_rect("x", 100, 30, 10, 10);
    /* Negative offsets. */
    CHECK(!c1_open(
        "<div id=p style=\"position:relative;width:200px;height:60px;background:#111111\">"
        "<div id=x style=\"position:absolute;left:-6px;top:-4px;width:10px;height:10px;"
        "background:#ff0000\"></div></div>"));
    expect_rect("x", -6, -4, 10, 10);
    /* Static position: no offsets means where it would have sat in flow. */
    CHECK(!c1_open(
        "<div id=p style=\"position:relative;width:200px;background:#111111\">"
        "<div style=\"height:12px\"></div>"
        "<div id=x style=\"position:absolute;width:30px;height:10px;background:#ff0000\"></div>"
        "<div id=s style=\"height:6px;background:#00ff00\"></div></div>"));
    expect_rect("x", 0, 12, 30, 10);
    expect_rect("s", 0, 12, 200, 6);
    /* An absolute chain resolves against the nearest positioned ancestor. */
    CHECK(!c1_open(
        "<div style=\"position:relative;margin-left:10px;width:200px;height:80px\">"
        "<div id=o style=\"position:absolute;left:20px;top:10px;width:100px;height:50px;"
        "background:#111111\">"
        "<div id=i style=\"position:absolute;left:5px;top:6px;width:10px;height:10px;"
        "background:#ff0000\"></div></div></div>"));
    expect_rect("o", 30, 10, 100, 50);
    expect_rect("i", 35, 16, 10, 10);
}

static void test_c1_shrink_to_fit(void) {
    CHECK(!c1_open(
        "<div id=p style=\"position:relative;width:200px;height:60px\">"
        "<div id=x style=\"position:absolute;top:0;left:0;background:#ff0000\">abc</div></div>"));
    expect_rect("x", 0, 0, 24, 9);
    /* Bounded by the containing block: long text wraps inside it. */
    CHECK(!c1_open(
        "<div id=p style=\"position:relative;width:200px;height:60px\">"
        "<div id=x style=\"position:absolute;top:0;left:0;background:#ff0000\">"
        "aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj kkkk llll mmmm</div></div>"));
    int x, y, w, h;
    rb_rect("x", &x, &y, &w, &h);
    CHECK(x == 0 && w > 100 && w <= 200 && h > 9);   /* bounded, wraps */
    /* A right-anchored shrink-to-fit box hugs the right edge by its own width. */
    CHECK(!c1_open(
        "<div id=p style=\"position:relative;width:200px;height:60px\">"
        "<div id=x style=\"position:absolute;top:0;right:10px;background:#ff0000\">abc</div></div>"));
    expect_rect("x", 166, 0, 24, 9);
}

static void test_c1_fixed(void) {
    CHECK(!c1_open(
        "<div style=\"height:400px\"></div>"
        "<div id=f style=\"position:fixed;top:10px;left:20px;width:30px;height:12px;"
        "background:#ff0000\"><a id=l href=\"/x\">go</a></div>"));
    CHECK(c1_first("f") >= 0);
    const RbBox *b = rb_box(c1_first("f"));
    CHECK(b->flags & RB_BOX_FIXED);
    CHECK(b->flags & RB_BOX_POS);
    expect_rect("f", 20, 10, 30, 12);          /* viewport space */
    int ph_fixed = rb_page_height();
    CHECK(!c1_open("<div style=\"height:400px\"></div>"));
    CHECK(ph_fixed == rb_page_height());       /* fixed boxes add no height */
    CHECK(!c1_open(
        "<div style=\"height:400px\"></div>"
        "<div id=f style=\"position:fixed;top:10px;left:20px;width:30px;height:12px;"
        "background:#ff0000\"><a id=l href=\"/x\">go</a></div>"));
    /* The link inside is fixed too, and hit-testable at any scroll. */
    int link = rb_find_id("l");
    CHECK(rb_hit_at(22, 12, 0) == link);
    CHECK(rb_hit_at(22, 12 + 100, 100) == link);   /* scrolled 100px */
    CHECK(rb_hit_at(22, 12, 100) != link);         /* doc space misses it */
    /* Fixed ignores a positioned ancestor. */
    CHECK(!c1_open(
        "<div style=\"position:relative;margin-left:50px;margin-top:30px;width:100px;height:50px\">"
        "<div id=f style=\"position:fixed;bottom:0;left:0;width:30px;height:12px;"
        "background:#ff0000\"></div></div>"));
    expect_rect("f", 0, RB_VIEW_H - 12, 30, 12);
    /* A static box is not flagged. */
    CHECK(!c1_open("<div id=s style=\"width:20px;height:10px;background:#ff0000\"></div>"));
    CHECK((rb_box(c1_first("s"))->flags & (RB_BOX_FIXED | RB_BOX_POS)) == 0);
}

static void test_c1_zindex(void) {
    /* Later source order but lower z-index must paint first. */
    CHECK(!c1_open(
        "<div id=hi style=\"position:absolute;top:0;left:0;width:20px;height:10px;"
        "z-index:5;background:#ff0000\"></div>"
        "<div id=lo style=\"position:absolute;top:0;left:0;width:20px;height:10px;"
        "z-index:1;background:#00ff00\"></div>"
        "<div id=mid style=\"position:absolute;top:0;left:0;width:20px;height:10px;"
        "z-index:3;background:#0000ff\"></div>"));
    CHECK(c1_first("lo") < c1_first("mid"));
    CHECK(c1_first("mid") < c1_first("hi"));
    int z = 0, is_auto = 0;
    z = rb_style_zindex(rb_find_id("hi"), &is_auto);
    CHECK(z == 5 && !is_auto);
    CHECK(rb_style_zindex(rb_find_id("lo"), &is_auto) == 1);
    /* z-index:auto positioned boxes paint above static ones, in source order
       among themselves, even when they come first in the source. */
    CHECK(!c1_open(
        "<div id=rel style=\"position:relative;width:20px;height:10px;background:#ff0000\"></div>"
        "<div id=st style=\"width:20px;height:10px;background:#00ff00\"></div>"
        "<div id=abs style=\"position:absolute;top:0;left:0;width:20px;height:10px;"
        "background:#0000ff\"></div>"));
    CHECK(c1_first("st") < c1_first("rel"));
    CHECK(c1_first("rel") < c1_first("abs"));
    rb_style_zindex(rb_find_id("rel"), &is_auto);
    CHECK(is_auto);
    /* Extremes and negatives sort without wrapping. */
    CHECK(!c1_open(
        "<div id=a style=\"position:absolute;z-index:99999999;width:9px;height:9px;"
        "background:#ff0000\"></div>"
        "<div id=b style=\"position:absolute;z-index:-5;width:9px;height:9px;"
        "background:#00ff00\"></div>"));
    CHECK(c1_first("b") < c1_first("a"));
    /* A positioned parent's z-index carries its in-flow children with it. */
    CHECK(!c1_open(
        "<div id=p style=\"position:absolute;z-index:7;width:40px;height:20px;"
        "background:#111111\"><div id=k style=\"height:5px;background:#ff0000\"></div></div>"
        "<div id=q style=\"position:absolute;z-index:2;width:40px;height:20px;"
        "background:#222222\"></div>"));
    CHECK(c1_first("q") < c1_first("p"));
    CHECK(c1_first("p") < c1_first("k"));
    CHECK(rb_box(c1_first("k"))->z == 7);
}

static void test_c1_overflow(void) {
    const char *text =
        "line one<br>line two<br>line three<br>line four<br>line five<br>line six";
    char html[1024];
    snprintf(html, sizeof html,
             "<div id=o style=\"overflow:hidden;width:100px;height:20px;background:#eeeeee\">%s</div>"
             "<div id=n style=\"height:10px;background:#00ff00\"></div>", text);
    CHECK(!c1_open(html));
    expect_rect("o", 0, 0, 100, 20);            /* the box is its declared height */
    expect_rect("n", 0, 20, 304, 10);           /* the sibling follows the clipped box */
    int clipped = 0, text_boxes = 0;
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        if (b->kind != RB_BOX_TEXT) continue;
        text_boxes++;
        int cx, cy, cw, ch;
        if (rb_box_clip(i, &cx, &cy, &cw, &ch)) {
            CHECK(cx == 0 && cy == 0 && cw == 100 && ch == 20);
            clipped++;
        }
    }
    CHECK(text_boxes >= 10 && clipped == text_boxes);
    CHECK(rb_style_overflow(rb_find_id("o")) == RB_OVF_HIDDEN);
    CHECK(rb_box_clip(c1_first("n"), 0, 0, 0, 0) == 0);

    /* visible does not clip and does not fix the height. */
    snprintf(html, sizeof html,
             "<div id=o style=\"height:20px;width:100px\">%s</div>", text);
    CHECK(!c1_open(html));
    for (int i = 0; i < rb_box_count(); i++) CHECK(rb_box_clip(i, 0, 0, 0, 0) == 0);

    /* auto, scroll and clip clip like hidden (no scrollbars exist). */
    const char *kinds[3] = { "auto", "scroll", "clip" };
    for (int k = 0; k < 3; k++) {
        snprintf(html, sizeof html,
                 "<div id=o style=\"overflow:%s;height:20px;width:100px\">%s</div>", kinds[k], text);
        CHECK(!c1_open(html));
        int any = 0;
        for (int i = 0; i < rb_box_count(); i++)
            if (rb_box(i)->kind == RB_BOX_TEXT && rb_box_clip(i, 0, 0, 0, 0)) any++;
        CHECK(any > 0);
    }
    /* Clipped text cannot be hit. */
    CHECK(!c1_open(
        "<div style=\"overflow:hidden;width:100px;height:10px\">"
        "<a id=l href=\"/x\">one two three four five six seven eight nine ten eleven twelve"
        "</a></div>"));
    int link = rb_find_id("l");
    CHECK(rb_hit(2, 2) == link);
    int clipped_hit = 0;
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        if (b->kind != RB_BOX_TEXT || b->y < 10) continue;
        if (rb_hit(b->x + 1, b->y + 1) == link) clipped_hit++;
    }
    CHECK(clipped_hit == 0);
    /* Nested clippers intersect. */
    CHECK(!c1_open(
        "<div style=\"overflow:hidden;width:100px;height:40px\">"
        "<div style=\"margin-left:30px;overflow:hidden;width:200px;height:20px\">"
        "<a id=l href=\"/x\">wide text that runs far past both clippers for sure and more</a>"
        "</div></div>"));
    int cx = 0, cy = 0, cw = 0, ch = 0;
    int fi = c1_first("l");
    CHECK(fi >= 0 && rb_box_clip(fi, &cx, &cy, &cw, &ch));
    CHECK(cx == 30 && cy == 0 && cw == 70 && ch == 20);
    /* An absolute child is clipped when the clipper is its containing block. */
    CHECK(!c1_open(
        "<div style=\"position:relative;overflow:hidden;width:50px;height:20px\">"
        "<div id=x style=\"position:absolute;left:30px;top:0;width:60px;height:10px;"
        "background:#ff0000\"></div></div>"));
    CHECK(rb_box_clip(c1_first("x"), &cx, &cy, &cw, &ch) && cx == 0 && cw == 50 && ch == 20);
    /* ... and escapes a clipper that is not (static clipper, outer CB). */
    CHECK(!c1_open(
        "<div style=\"position:relative;width:200px;height:50px\">"
        "<div style=\"overflow:hidden;width:50px;height:20px\">"
        "<div id=x style=\"position:absolute;left:30px;top:0;width:60px;height:10px;"
        "background:#ff0000\"></div></div></div>"));
    CHECK(rb_box_clip(c1_first("x"), 0, 0, 0, 0) == 0);
    /* A relative clipper carries its clip when it moves. */
    CHECK(!c1_open(
        "<div id=o style=\"position:relative;left:10px;top:5px;overflow:hidden;width:50px;height:20px\">"
        "<a id=l href=\"/x\">some quite long text here</a></div>"));
    CHECK(rb_box_clip(c1_first("l"), &cx, &cy, &cw, &ch) && cx == 10 && cy == 5 && cw == 50 && ch == 20);
}

static void test_c1_visibility(void) {
    CHECK(!c1_open(
        "<div id=a style=\"height:10px;background:#ff0000\"></div>"
        "<div id=h style=\"visibility:hidden;height:10px;background:#00ff00\"></div>"
        "<div id=c style=\"height:10px;background:#0000ff\"></div>"));
    expect_rect("a", 0, 0, 304, 10);
    expect_rect("c", 0, 20, 304, 10);          /* hidden keeps its space */
    CHECK(c1_count("h") == 0);                 /* ... and paints nothing */
    CHECK(rb_style_visibility(rb_find_id("h")) == RB_VIS_HIDDEN);
    CHECK(rb_style_visibility(rb_find_id("a")) == RB_VIS_VISIBLE);
    /* display:none still removes the space. */
    CHECK(!c1_open(
        "<div id=h style=\"display:none;height:10px\"></div>"
        "<div id=c style=\"height:10px;background:#0000ff\"></div>"));
    expect_rect("c", 0, 0, 304, 10);
    /* A visible descendant of a hidden element shows again. */
    CHECK(!c1_open(
        "<div style=\"visibility:hidden\"><p>gone</p>"
        "<p id=k style=\"visibility:visible\">back</p></div>"));
    int x, y, w, h;
    CHECK(!find_text("gone", &x, &y, &w, &h));
    CHECK(find_text("back", &x, &y, &w, &h));
    /* A hidden inline span keeps its width. */
    int bx0 = 0;
    CHECK(!c1_open("<p>a<span>xyz</span>b</p>"));
    CHECK(find_text("b", &bx0, &y, &w, &h));
    CHECK(!c1_open("<p>a<span style=\"visibility:hidden\">xyz</span>b</p>"));
    int bx = 0, by = 0;
    CHECK(find_text("b", &bx, &by, &w, &h));
    CHECK(bx == bx0 && by == y);
    CHECK(!find_text("xyz", &x, &y, &w, &h));
    /* Hidden elements are not hit targets. */
    CHECK(!c1_open("<a id=l href=\"/x\" style=\"visibility:hidden\">link</a>"));
    CHECK(rb_hit(2, 2) < 0);
    CHECK(rb_focus_count() == 0);              /* not a tab stop either */
    CHECK(!c1_open("<div style=\"visibility:hidden\"><a id=l href=\"/x\" "
                   "style=\"visibility:visible\">link</a></div>"));
    CHECK(rb_focus_count() == 1);
}

static void test_c1_opacity(void) {
    CHECK(!c1_open(
        "<div id=h style=\"opacity:0.5;height:10px;background:#ff0000\"></div>"
        "<div id=f style=\"opacity:1;height:10px;background:#ff0000\"></div>"
        "<div id=z style=\"opacity:0;height:10px;background:#ff0000\"></div>"
        "<div id=t style=\"opacity:0.03;height:10px;background:#ff0000\"></div>"
        "<div id=p style=\"opacity:50%;height:10px;background:#ff0000\"></div>"));
    int ob = rb_box(c1_first("h"))->opacity;
    CHECK(ob >= 120 && ob <= 135);
    CHECK(rb_box(c1_first("f"))->opacity == 255);
    CHECK(c1_count("z") == 0);                 /* nothing painted ... */
    CHECK(c1_count("t") == 0);                 /* ... below the threshold */
    CHECK(rb_box(c1_first("p"))->opacity == ob);
    CHECK(rb_style_opacity(rb_find_id("h")) == ob);
    CHECK(rb_style_opacity(rb_find_id("f")) == 255);
    /* Space is kept either way. */
    expect_rect("f", 0, 10, 304, 10);
    /* Opacity multiplies down the tree and reaches text. */
    CHECK(!c1_open("<div style=\"opacity:0.5\"><div style=\"opacity:0.5\"><b id=t>hi</b></div></div>"));
    int tb = -1;
    for (int i = 0; i < rb_box_count(); i++)
        if (rb_box(i)->kind == RB_BOX_TEXT) tb = rb_box(i)->opacity;
    CHECK(tb >= 55 && tb <= 70);
}

static void test_c1_cursor(void) {
    CHECK(!c1_open(
        "<div id=a style=\"cursor:pointer\">x</div><div id=b style=\"cursor:text\">x</div>"
        "<div id=c style=\"cursor:not-allowed\">x</div><div id=d>x</div>"
        "<div id=e style=\"cursor:bogus\">x</div>"));
    CHECK(rb_style_cursor(rb_find_id("a")) == RB_CURSOR_POINTER);
    CHECK(rb_style_cursor(rb_find_id("b")) == RB_CURSOR_TEXT);
    CHECK(rb_style_cursor(rb_find_id("c")) == RB_CURSOR_NOT_ALLOWED);
    CHECK(rb_style_cursor(rb_find_id("d")) == RB_CURSOR_AUTO);
    CHECK(rb_style_cursor(rb_find_id("e")) == RB_CURSOR_AUTO);
    CHECK(rb_style_cursor(0) == RB_CURSOR_AUTO);
    CHECK(rb_style_cursor(9999) == RB_CURSOR_AUTO);
}

static void test_c1_nested(void) {
    /* absolute inside relative inside a flex item; siblings unaffected. */
    CHECK(!c1_open(
        "<div style=\"display:flex;width:300px\">"
        "<div id=it style=\"position:relative;width:100px;height:30px;background:#111111\">"
        "<div id=x style=\"position:absolute;right:0;bottom:0;width:20px;height:10px;"
        "background:#ff0000\"></div></div>"
        "<div id=it2 style=\"width:50px;height:30px;background:#222222\"></div></div>"));
    expect_rect("it", 0, 0, 100, 30);
    expect_rect("x", 80, 20, 20, 10);
    expect_rect("it2", 100, 0, 50, 30);
    /* An absolute flex child leaves the flex flow: flex math is unchanged. */
    CHECK(!c1_open(
        "<div id=f style=\"display:flex;position:relative;width:300px;height:30px\">"
        "<div id=a style=\"flex:1;background:#111111\"></div>"
        "<div id=ab style=\"position:absolute;top:0;right:0;width:10px;height:10px;"
        "background:#ff0000\"></div>"
        "<div id=b style=\"flex:1;background:#222222\"></div></div>"));
    expect_rect("a", 0, 0, 150, 30);
    expect_rect("b", 150, 0, 150, 30);
    expect_rect("ab", 290, 0, 10, 10);
    /* A relative flex item moves without disturbing the line. */
    CHECK(!c1_open(
        "<div style=\"display:flex;width:300px\">"
        "<div id=a style=\"width:50px;height:20px;background:#111111\"></div>"
        "<div id=b style=\"position:relative;top:4px;left:3px;width:50px;height:20px;"
        "background:#222222\"></div>"
        "<div id=c style=\"width:50px;height:20px;background:#333333\"></div></div>"));
    expect_rect("a", 0, 0, 50, 20);
    expect_rect("b", 53, 4, 50, 20);
    expect_rect("c", 100, 0, 50, 20);
    /* An absolute element inside a measured flex item is re-deferred on each
       pass without duplicating. */
    CHECK(!c1_open(
        "<div style=\"display:flex;width:300px\">"
        "<div style=\"position:relative;flex:1\"><div id=x style=\"position:absolute;left:3px;"
        "top:2px;width:10px;height:10px;background:#ff0000\"></div>text</div>"
        "<div style=\"flex:1\">more</div></div>"));
    CHECK(c1_count("x") == 1);
    expect_rect("x", 3, 2, 10, 10);
}

static void test_c1_extent(void) {
    /* The document grows to hold an absolute box below the fold. */
    CHECK(!c1_open(
        "<p>hi</p><div id=x style=\"position:absolute;top:500px;left:0;width:20px;height:10px;"
        "background:#ff0000\"></div>"));
    CHECK(rb_page_height() >= 510);
    /* A silly offset does not make the page enormous. */
    CHECK(!c1_open(
        "<p>hi</p><div style=\"position:absolute;top:8000px;left:0;width:20px;height:10px;"
        "background:#ff0000\"></div>"));
    CHECK(rb_page_height() < 4100);
    /* Huge offsets stay representable. */
    CHECK(!c1_open(
        "<div style=\"position:relative;top:99999px;left:-99999px;width:20px;height:10px;"
        "background:#ff0000\"></div>"));
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        CHECK(b->x >= -16000 && b->x <= 16000 && b->y >= -16000 && b->y <= 16000);
    }
}

/* A page that uses no C1 property lays out the same with explicit defaults. */
static void test_c1_default_identity(void) {
    const char *plain =
        "<div style=\"padding:4px;background:#222222\"><p>one two three</p>"
        "<div style=\"display:flex;width:200px\"><div style=\"flex:1\">a</div><div>b</div></div>"
        "<a href=\"/x\">link</a><br>tail</div>";
    const char *dflt =
        "<div style=\"padding:4px;background:#222222;position:static;overflow:visible;"
        "visibility:visible;opacity:1;z-index:auto;cursor:auto\"><p>one two three</p>"
        "<div style=\"display:flex;width:200px\"><div style=\"flex:1\">a</div><div>b</div></div>"
        "<a href=\"/x\">link</a><br>tail</div>";
    CHECK(!c1_open(plain));
    int n = rb_box_count();
    RbBox saved[64];
    CHECK(n > 0 && n <= 64);
    for (int i = 0; i < n && i < 64; i++) saved[i] = *rb_box(i);
    int ph = rb_page_height();
    CHECK(!c1_open(dflt));
    CHECK(rb_box_count() == n && rb_page_height() == ph);
    for (int i = 0; i < n && i < 64 && i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        CHECK(b->x == saved[i].x && b->y == saved[i].y && b->w == saved[i].w && b->h == saved[i].h);
        CHECK((b->flags & (RB_BOX_FIXED | RB_BOX_POS)) == 0 && b->clip == 0 && b->opacity == 255 && b->z == 0);
    }
}

/* Relayout must neither leak the text pool nor duplicate deferred elements. */
static void test_c1_relayout_stable(void) {
    CHECK(!c1_open(
        "<div style=\"position:relative;width:200px;height:50px\">some text here"
        "<div id=x style=\"position:absolute;top:2px;left:3px;background:#ff0000\">abs words</div>"
        "</div><p>tail text</p>"));
    int n = rb_box_count();
    int ph = rb_page_height();
    for (int i = 0; i < 3000; i++) rb_relayout();
    CHECK(!rb_oom());
    CHECK(rb_box_count() == n && rb_page_height() == ph);
}


/* =====================================================================
   PART C2: box model, borders, radius, text.   PART C3: grid.
   ===================================================================== */

/* Is there a box of `kind` owned by `id` at exactly this rectangle? */
static int c2_has_box(const char *id, int kind, int x, int y, int w, int h) {
    int node = rb_find_id(id);
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        if (b->node == node && b->kind == kind && b->x == x && b->y == y && b->w == w && b->h == h)
            return 1;
    }
    return 0;
}

static int c2_kind_count(const char *id, int kind) {
    int node = rb_find_id(id), n = 0;
    for (int i = 0; i < rb_box_count(); i++)
        if (rb_box(i)->node == node && rb_box(i)->kind == kind) n++;
    return n;
}

static const RbBox *c2_text_box(const char *s) {
    for (int i = 0; i < rb_box_count(); i++)
        if (rb_box(i)->kind == RB_BOX_TEXT && box_eq(i, s)) return rb_box(i);
    return 0;
}

static void test_c2_margin_auto(void) {
    CHECK(!c1_open("<div id=a style=\"width:100px;margin:0 auto;height:10px;background:#ff0000\"></div>"));
    expect_rect("a", 102, 0, 100, 10);               /* (304 - 100) / 2 */
    CHECK(!c1_open("<div id=a style=\"width:100px;margin-left:auto;height:10px;background:#ff0000\"></div>"));
    expect_rect("a", 204, 0, 100, 10);
    CHECK(!c1_open("<div id=a style=\"width:100px;margin-right:auto;height:10px;background:#ff0000\"></div>"));
    expect_rect("a", 0, 0, 100, 10);
    CHECK(!c1_open("<div id=a style=\"margin:0 auto;height:10px;background:#ff0000\"></div>"));
    expect_rect("a", 0, 0, 304, 10);                 /* auto width fills: auto margins are 0 */
    CHECK(!c1_open("<div id=a style=\"width:100px;margin:5px auto;height:10px;background:#ff0000\"></div>"));
    expect_rect("a", 102, 5, 100, 10);
    /* A fixed margin on one side leaves the rest to the auto side. */
    CHECK(!c1_open("<div id=a style=\"width:100px;margin-left:auto;margin-right:4px;height:10px;"
                   "background:#ff0000\"></div>"));
    expect_rect("a", 200, 0, 100, 10);
}

static void test_c2_boxsizing(void) {
    const char *base = "padding:10px;width:100px;height:60px;background:#ff0000";
    char html[256];
    snprintf(html, sizeof html, "<div id=a style=\"%s\"></div>", base);       /* legacy */
    CHECK(!c1_open(html));
    expect_rect("a", 0, 0, 100, 80);          /* width is the border box, height the content box */
    snprintf(html, sizeof html, "<div id=a style=\"%s;box-sizing:content-box\"></div>", base);
    CHECK(!c1_open(html));
    expect_rect("a", 0, 0, 120, 80);          /* CSS content-box: both axes add the padding */
    snprintf(html, sizeof html, "<div id=a style=\"%s;box-sizing:border-box\"></div>", base);
    CHECK(!c1_open(html));
    expect_rect("a", 0, 0, 100, 60);          /* CSS border-box: both axes include it */
    /* A border counts as padding for the box. */
    CHECK(!c1_open("<div id=a style=\"border:2px solid #000000;padding:3px;width:50px;height:20px;"
                   "box-sizing:content-box;background:#ff0000\"></div>"));
    expect_rect("a", 0, 0, 60, 30);
}

static void test_c2_border(void) {
    CHECK(!c1_open("<div id=b style=\"border:2px solid #ff0000;padding:3px;width:100px;height:10px\">"
                   "<div id=k style=\"height:5px;background:#00ff00\"></div></div>"));
    CHECK(c2_kind_count("b", RB_BOX_BG) == 4);
    CHECK(c2_has_box("b", RB_BOX_BG, 0, 0, 100, 2));      /* top */
    CHECK(c2_has_box("b", RB_BOX_BG, 98, 0, 2, 20));      /* right */
    CHECK(c2_has_box("b", RB_BOX_BG, 0, 18, 100, 2));     /* bottom */
    CHECK(c2_has_box("b", RB_BOX_BG, 0, 0, 2, 20));       /* left */
    expect_rect("b", 0, 0, 100, 20);                      /* 10 + 2 * (3 + 2) */
    expect_rect("k", 5, 5, 90, 5);                        /* content inside border + padding */
    int bc = rb_map_rgb(255, 0, 0);
    for (int i = 0; i < rb_box_count(); i++)
        if (rb_box(i)->node == rb_find_id("b")) CHECK(rb_box(i)->bg == bc);
    /* The element's own background is under the border strips. */
    CHECK(!c1_open("<div id=b style=\"border:1px solid #0000ff;width:50px;height:10px;background:#eeeeee\"></div>"));
    CHECK(c1_first("b") >= 0 && rb_box(c1_first("b"))->kind == RB_BOX_BG &&
          rb_box(c1_first("b"))->bg == rb_map_rgb(0xee, 0xee, 0xee));
    CHECK(c2_kind_count("b", RB_BOX_BG) == 5);
    /* currentColor when no colour is given. */
    CHECK(!c1_open("<div id=b style=\"color:#0000ff;border:1px solid;width:50px;height:10px\"></div>"));
    CHECK(rb_box(c1_first("b"))->bg == rb_map_rgb(0, 0, 255));
    /* none / 0 draw nothing and take no room. */
    CHECK(!c1_open("<div id=b style=\"border:none;width:50px;height:10px;background:#eeeeee\"></div>"));
    CHECK(c2_kind_count("b", RB_BOX_BG) == 1);
    CHECK(!c1_open("<div id=b style=\"border:0 solid #ff0000;width:50px;height:10px;background:#eeeeee\"></div>"));
    CHECK(c2_kind_count("b", RB_BOX_BG) == 1);
    /* One side only: the border takes its own room (CSS), below the content. */
    CHECK(!c1_open("<div id=b style=\"border-bottom:1px solid #cccccc;width:50px;height:10px\"></div>"
                   "<div id=n style=\"height:5px;background:#00ff00\"></div>"));
    CHECK(c2_has_box("b", RB_BOX_BG, 0, 10, 50, 1));
    expect_rect("n", 0, 11, 304, 5);
    /* border-width / border-color longhands. */
    CHECK(!c1_open("<div id=b style=\"border:1px solid #ff0000;border-width:3px;border-color:#0000ff;"
                   "width:50px;height:10px\"></div>"));
    CHECK(c2_has_box("b", RB_BOX_BG, 0, 0, 50, 3));
    CHECK(rb_box(c1_first("b"))->bg == rb_map_rgb(0, 0, 255));
    /* The containing block of an absolute child is inside the border. */
    CHECK(!c1_open("<div style=\"position:relative;border:3px solid #000000;width:100px;height:50px\">"
                   "<div id=x style=\"position:absolute;left:0;top:0;width:10px;height:10px;"
                   "background:#ff0000\"></div></div>"));
    expect_rect("x", 3, 3, 10, 10);
    /* overflow clips to the padding box (inside the border); the border is not clipped. */
    CHECK(!c1_open("<div id=o style=\"overflow:hidden;border:2px solid #ff0000;width:50px;height:20px\">"
                   "<a id=l href=\"/x\">some long text that overflows</a></div>"));
    int cx = 0, cy = 0, cw = 0, ch = 0;
    CHECK(rb_box_clip(c1_first("l"), &cx, &cy, &cw, &ch));
    CHECK(cx == 2 && cy == 2 && cw == 46 && ch == 20);
    for (int i = 0; i < rb_box_count(); i++)
        if (rb_box(i)->node == rb_find_id("o")) CHECK(rb_box_clip(i, 0, 0, 0, 0) == 0);
    /* Borders inside a flex item reserve room in its measure. */
    CHECK(!c1_open("<div style=\"display:flex;width:200px\">"
                   "<div id=a style=\"border:2px solid #ff0000;width:50px;height:10px\"></div>"
                   "<div id=b style=\"width:50px;height:10px;background:#00ff00\"></div></div>"));
    expect_rect("b", 50, 0, 50, 10);
}

static void test_c2_radius(void) {
    CHECK(!c1_open("<div id=b style=\"border:1px solid #0000ff;border-radius:4px;width:40px;height:20px;"
                   "background:#eeeeee\"></div>"));
    CHECK(c2_kind_count("b", RB_BOX_BORDER) == 1);
    int ri = -1, bi = -1;
    for (int i = 0; i < rb_box_count(); i++) {
        if (rb_box(i)->kind == RB_BOX_BORDER) ri = i;
        if (rb_box(i)->kind == RB_BOX_BG) bi = i;
    }
    CHECK(ri >= 0 && bi >= 0 && bi < ri);                 /* ring above the fill */
    CHECK(rb_box(ri)->radius == 4 && rb_box(ri)->scale == 1);
    CHECK(rb_box(ri)->x == 0 && rb_box(ri)->y == 0 && rb_box(ri)->w == 40 && rb_box(ri)->h == 22);
    CHECK(rb_box(bi)->radius == 4);
    /* A radius larger than half the box is capped; 50% is a pill. */
    CHECK(!c1_open("<div id=b style=\"border-radius:50%;width:40px;height:20px;background:#eeeeee\"></div>"));
    CHECK(rb_box(c1_first("b"))->radius == 10);           /* min(40, 20) / 2 */
    CHECK(!c1_open("<div id=b style=\"border-radius:999px;width:40px;height:20px;background:#eeeeee\"></div>"));
    CHECK(rb_box(c1_first("b"))->radius == 10);
    CHECK(!c1_open("<div id=b style=\"border-radius:30px;width:100px;height:20px;background:#eeeeee\"></div>"));
    CHECK(rb_box(c1_first("b"))->radius == 10);
    /* A rounded ring is not a hit target, a rounded link still is. */
    CHECK(!c1_open("<a id=l href=\"/x\" style=\"display:block;border:1px solid #ff0000;border-radius:3px;"
                   "width:60px;height:10px\">go</a>"));
    CHECK(rb_hit(5, 3) == rb_find_id("l"));
}

static void test_c2_text(void) {
    CHECK(!c1_open("<p><b>bold</b> <span style=\"font-weight:bold\">also</span> plain</p>"));
    CHECK(c2_text_box("bold") && (c2_text_box("bold")->flags & RB_BOX_BOLD));
    CHECK(c2_text_box("also") && (c2_text_box("also")->flags & RB_BOX_BOLD));
    CHECK(c2_text_box("plain") && !(c2_text_box("plain")->flags & RB_BOX_BOLD));
    /* font-weight overrides the tag; headings are bold; inheritance flows down. */
    CHECK(!c1_open("<b style=\"font-weight:normal\">light</b><h2>head</h2>"
                   "<div style=\"font-weight:700\"><p>deep <i>nest</i></p></div>"
                   "<div style=\"font-weight:bold\"><p style=\"font-weight:normal\">back</p></div>"));
    CHECK(!(c2_text_box("light")->flags & RB_BOX_BOLD));
    CHECK(c2_text_box("head")->flags & RB_BOX_BOLD);
    CHECK(c2_text_box("deep")->flags & RB_BOX_BOLD);
    CHECK(c2_text_box("nest")->flags & RB_BOX_BOLD);
    CHECK(!(c2_text_box("back")->flags & RB_BOX_BOLD));
    /* Underline: links by default, u, none to remove, line-through. */
    CHECK(!c1_open("<a href=\"/x\">lnk</a> <a href=\"/y\" style=\"text-decoration:none\">bare</a> "
                   "<u>und</u> <span style=\"text-decoration:line-through\">cut</span> "
                   "<a href=\"/z\"><span>inner</span></a> <p>none</p>"));
    CHECK(c2_text_box("lnk")->flags & RB_BOX_UNDER);
    CHECK(!(c2_text_box("bare")->flags & (RB_BOX_UNDER | RB_BOX_STRIKE)));
    CHECK(c2_text_box("und")->flags & RB_BOX_UNDER);
    CHECK(c2_text_box("cut")->flags & RB_BOX_STRIKE);
    CHECK(!(c2_text_box("cut")->flags & RB_BOX_UNDER));
    CHECK(c2_text_box("inner")->flags & RB_BOX_UNDER);
    CHECK(!(c2_text_box("none")->flags & (RB_BOX_UNDER | RB_BOX_STRIKE)));
    /* text-transform acts on the box copy, not the DOM. */
    CHECK(!c1_open("<p id=a style=\"text-transform:uppercase\">Hello</p>"
                   "<p style=\"text-transform:lowercase\">WORLD</p>"
                   "<p style=\"text-transform:capitalize\">foo bar</p>"));
    CHECK(c2_text_box("HELLO") && c2_text_box("world") && c2_text_box("Foo") && c2_text_box("Bar"));
    char buf[32];
    CHECK(rb_collect_text(rb_find_id("a"), buf, (int)sizeof buf) == 5 && strcmp(buf, "Hello") == 0);
    /* The transform inherits. */
    CHECK(!c1_open("<div style=\"text-transform:uppercase\"><p>abc <b>def</b></p></div>"));
    CHECK(c2_text_box("ABC") && c2_text_box("DEF"));
    /* letter-spacing widens the glyph pitch and the box. */
    CHECK(!c1_open("<p style=\"letter-spacing:2px\">abc def</p>"));
    const RbBox *b1 = c2_text_box("abc"), *b2 = c2_text_box("def");
    CHECK(b1 && b2 && b1->w == 30 && b1->ls == 2);        /* 3 * (8 + 2) */
    CHECK(b2->x == b1->x + 30 + 10);                      /* the space is one pitch */
    CHECK(!c1_open("<p>abc def</p>"));
    CHECK(c2_text_box("abc")->ls == 0 && c2_text_box("abc")->w == 24);
    /* line-height: the line box grows and the glyphs sit in its middle. */
    CHECK(!c1_open("<div style=\"line-height:20px\"><span id=s>abc</span></div>"
                   "<div id=n style=\"height:5px;background:#00ff00\"></div>"));
    CHECK(c2_text_box("abc")->y == 6);
    expect_rect("n", 0, 21, 304, 5);                      /* 20px line + the 1px engine leading */
    CHECK(!c1_open("<div style=\"line-height:2\">abc</div><div id=n style=\"height:5px;background:#00ff00\"></div>"));
    CHECK(c2_text_box("abc")->y == 4);
    expect_rect("n", 0, 17, 304, 5);
    CHECK(!c1_open("<div style=\"line-height:200%\">abc</div><div id=n style=\"height:5px;background:#00ff00\"></div>"));
    expect_rect("n", 0, 17, 304, 5);
    CHECK(!c1_open("<div style=\"line-height:normal\">abc</div><div id=n style=\"height:5px;background:#00ff00\"></div>"));
    expect_rect("n", 0, 9, 304, 5);
    /* white-space: nowrap keeps one line, normal wraps. */
    CHECK(!c1_open("<div style=\"width:80px;white-space:nowrap\">aaaa bbbb cccc dddd</div>"));
    int ys = 0, first_y = -1;
    for (int i = 0; i < rb_box_count(); i++) {
        if (rb_box(i)->kind != RB_BOX_TEXT) continue;
        if (first_y < 0) first_y = rb_box(i)->y;
        if (rb_box(i)->y != first_y) ys++;
    }
    CHECK(ys == 0 && rb_box_count() >= 4);
    CHECK(!c1_open("<div style=\"width:80px\">aaaa bbbb cccc dddd</div>"));
    ys = 0; first_y = -1;
    for (int i = 0; i < rb_box_count(); i++) {
        if (rb_box(i)->kind != RB_BOX_TEXT) continue;
        if (first_y < 0) first_y = rb_box(i)->y;
        if (rb_box(i)->y != first_y) ys++;
    }
    CHECK(ys > 0);
    /* Text properties do not leak out of their element. */
    CHECK(!c1_open("<div style=\"font-weight:bold;letter-spacing:3px\">in</div><p>out</p>"));
    CHECK((c2_text_box("in")->flags & RB_BOX_BOLD) && c2_text_box("in")->ls == 3);
    CHECK(!(c2_text_box("out")->flags & RB_BOX_BOLD) && c2_text_box("out")->ls == 0);
}

static void test_c3_grid_tracks(void) {
    CHECK(!c1_open(
        "<div id=g style=\"display:grid;grid-template-columns:50px 1fr 2fr;gap:4px;width:204px\">"
        "<div id=a style=\"background:#ff0000\">A</div><div id=b style=\"background:#00ff00\">B</div>"
        "<div id=c style=\"background:#0000ff\">C</div>"
        "<div id=d style=\"background:#ffff00;grid-column:span 2\">D</div>"
        "<div id=e style=\"background:#00ffff\">E</div></div>"));
    expect_rect("a", 0, 0, 50, 9);
    expect_rect("b", 54, 0, 48, 9);           /* (204 - 50 - 2*4) = 146 -> 48 / 98 */
    expect_rect("c", 106, 0, 98, 9);
    expect_rect("d", 0, 13, 102, 9);          /* spans the first two tracks and the gap */
    expect_rect("e", 106, 13, 98, 9);
    /* repeat(), and rows stretch to the tallest item. */
    CHECK(!c1_open(
        "<div style=\"display:grid;grid-template-columns:repeat(3,1fr);width:300px\">"
        "<div id=a style=\"background:#ff0000\">A</div><div id=b style=\"background:#00ff00\">B</div>"
        "<div id=c style=\"background:#0000ff;height:20px\"></div>"
        "<div id=d style=\"background:#ffff00\">D</div></div>"));
    expect_rect("a", 0, 0, 100, 20);
    expect_rect("b", 100, 0, 100, 20);
    expect_rect("c", 200, 0, 100, 20);
    expect_rect("d", 0, 20, 100, 9);
    /* percentages, and no lost pixels in 1fr columns that do not divide evenly. */
    CHECK(!c1_open(
        "<div style=\"display:grid;grid-template-columns:25% 75%;width:200px\">"
        "<div id=a style=\"background:#ff0000\">A</div><div id=b style=\"background:#00ff00\">B</div></div>"));
    expect_rect("a", 0, 0, 50, 9);
    expect_rect("b", 50, 0, 150, 9);
    CHECK(!c1_open(
        "<div style=\"display:grid;grid-template-columns:1fr 1fr 1fr;width:100px\">"
        "<div id=a style=\"background:#ff0000\">A</div><div id=b style=\"background:#00ff00\">B</div>"
        "<div id=c style=\"background:#0000ff\">C</div></div>"));
    expect_rect("a", 0, 0, 33, 9);
    expect_rect("b", 33, 0, 33, 9);
    expect_rect("c", 66, 0, 34, 9);
    /* auto-fill with minmax(100px, 1fr) counts the columns that fit. */
    CHECK(!c1_open(
        "<div style=\"display:grid;grid-template-columns:repeat(auto-fill,minmax(100px,1fr));width:304px\">"
        "<div id=a style=\"background:#ff0000\">A</div><div id=b style=\"background:#00ff00\">B</div>"
        "<div id=c style=\"background:#0000ff\">C</div><div id=d style=\"background:#ffff00\">D</div></div>"));
    expect_rect("a", 0, 0, 101, 9);
    expect_rect("b", 101, 0, 101, 9);
    expect_rect("c", 202, 0, 102, 9);
    expect_rect("d", 0, 9, 101, 9);
    CHECK(!c1_open(
        "<div style=\"display:grid;grid-template-columns:repeat(auto-fit,minmax(100px,1fr));width:150px;gap:10px\">"
        "<div id=a style=\"background:#ff0000\">A</div><div id=b style=\"background:#00ff00\">B</div></div>"));
    expect_rect("a", 0, 0, 150, 9);           /* only one 100px track fits with a 10px gap */
    expect_rect("b", 0, 19, 150, 9);
    /* gap shorthand and the two longhands. */
    CHECK(!c1_open(
        "<div style=\"display:grid;grid-template-columns:50px 50px;row-gap:3px;column-gap:7px;width:200px\">"
        "<div id=a style=\"background:#ff0000\">A</div><div id=b style=\"background:#00ff00\">B</div>"
        "<div id=c style=\"background:#0000ff\">C</div></div>"));
    expect_rect("b", 57, 0, 50, 9);
    expect_rect("c", 0, 12, 50, 9);
    /* No template: one column. A hidden child takes no cell. */
    CHECK(!c1_open(
        "<div style=\"display:grid;width:100px\"><div id=a style=\"background:#ff0000\">A</div>"
        "<div style=\"display:none\">X</div><div id=b style=\"background:#00ff00\">B</div></div>"));
    expect_rect("a", 0, 0, 100, 9);
    expect_rect("b", 0, 9, 100, 9);
    /* An empty grid has no height. */
    CHECK(!c1_open("<div id=g style=\"display:grid;grid-template-columns:1fr 1fr;background:#ff0000\"></div>"
                   "<div id=n style=\"height:5px;background:#00ff00\"></div>"));
    expect_rect("n", 0, 0, 304, 5);
}

static void test_c3_grid_placement(void) {
    /* Explicit columns by line number, "a / b", and "1 / -1". */
    CHECK(!c1_open(
        "<div style=\"display:grid;grid-template-columns:repeat(4,50px);width:200px\">"
        "<div id=a style=\"background:#ff0000;grid-column:2\">A</div>"
        "<div id=b style=\"background:#00ff00;grid-column:1 / 3\">B</div>"
        "<div id=c style=\"background:#0000ff;grid-column:1 / -1\">C</div>"
        "<div id=d style=\"background:#ffff00;grid-column:3 / span 2\">D</div></div>"));
    expect_rect("a", 50, 0, 50, 9);           /* line 2 = the second track */
    expect_rect("b", 0, 9, 100, 9);           /* 1 / 3 spans two tracks, next row (a took col 2) */
    expect_rect("c", 0, 18, 200, 9);
    expect_rect("d", 100, 27, 100, 9);
    /* Row spans: a tall item beside two stacked ones. */
    CHECK(!c1_open(
        "<div style=\"display:grid;grid-template-columns:50px 50px;width:100px\">"
        "<div id=a style=\"background:#ff0000;grid-row:span 2;height:30px\">A</div>"
        "<div id=b style=\"background:#00ff00\">B</div><div id=c style=\"background:#0000ff\">C</div></div>"));
    expect_rect("a", 0, 0, 50, 30);
    expect_rect("b", 50, 0, 50, 15);          /* the 12px deficit is shared by the two spanned rows */
    expect_rect("c", 50, 15, 50, 15);
    /* Sparse packing: a wide item that does not fit leaves a hole, later items do not back-fill. */
    CHECK(!c1_open(
        "<div style=\"display:grid;grid-template-columns:50px 50px 50px;width:150px\">"
        "<div id=a style=\"background:#ff0000;grid-column:span 2\">A</div>"
        "<div id=b style=\"background:#00ff00;grid-column:span 2\">B</div>"
        "<div id=c style=\"background:#0000ff\">C</div></div>"));
    expect_rect("a", 0, 0, 100, 9);
    expect_rect("b", 0, 9, 100, 9);
    expect_rect("c", 100, 9, 50, 9);
    /* A span wider than the grid is clamped. */
    CHECK(!c1_open(
        "<div style=\"display:grid;grid-template-columns:50px 50px;width:100px\">"
        "<div id=a style=\"background:#ff0000;grid-column:span 9\">A</div></div>"));
    expect_rect("a", 0, 0, 100, 9);
    /* Bare text is a grid item too. */
    CHECK(!c1_open(
        "<div style=\"display:grid;grid-template-columns:50px 50px;width:100px\">one<b>two</b></div>"));
    const RbBox *t1 = c2_text_box("one"), *t2 = c2_text_box("two");
    CHECK(t1 && t2 && t1->x == 0 && t2->x == 50 && t1->y == t2->y);
}

static void test_c3_grid_align(void) {
    const char *cells =
        "<div id=a style=\"background:#ff0000;width:20px;height:10px\"></div>"
        "<div id=b style=\"background:#00ff00;height:30px\"></div></div>";
    char html[512];
    snprintf(html, sizeof html,
             "<div style=\"display:grid;grid-template-columns:100px 100px;width:200px;%s\">%s", "", cells);
    CHECK(!c1_open(html));
    expect_rect("a", 0, 0, 20, 10);           /* an explicit height is not stretched; width 20 stays */
    expect_rect("b", 100, 0, 100, 30);
    snprintf(html, sizeof html,
             "<div style=\"display:grid;grid-template-columns:100px 100px;width:200px;"
             "align-items:center;justify-items:center\">%s", cells);
    CHECK(!c1_open(html));
    expect_rect("a", 40, 10, 20, 10);         /* centred in a 100 x 30 cell */
    snprintf(html, sizeof html,
             "<div style=\"display:grid;grid-template-columns:100px 100px;width:200px;"
             "align-items:flex-end;justify-items:end\">%s", cells);
    CHECK(!c1_open(html));
    expect_rect("a", 80, 20, 20, 10);
    snprintf(html, sizeof html,
             "<div style=\"display:grid;grid-template-columns:100px 100px;width:200px;"
             "align-items:start;justify-items:start\">%s", cells);
    CHECK(!c1_open(html));
    expect_rect("a", 0, 0, 20, 10);
    /* align-self overrides align-items for one cell. */
    CHECK(!c1_open(
        "<div style=\"display:grid;grid-template-columns:100px 100px;width:200px;align-items:start\">"
        "<div id=a style=\"background:#ff0000;height:10px;align-self:end\"></div>"
        "<div id=b style=\"background:#00ff00;height:30px\"></div></div>"));
    expect_rect("a", 0, 20, 100, 10);
    /* justify-items:center shrinks an auto-width item to its content. */
    CHECK(!c1_open(
        "<div style=\"display:grid;grid-template-columns:100px;width:100px;justify-items:center\">"
        "<div id=a style=\"background:#ff0000\">abcd</div></div>"));
    expect_rect("a", 34, 0, 32, 9);
}

static void test_c3_grid_interactions(void) {
    /* Nested grids and a grid inside a flex item. */
    CHECK(!c1_open(
        "<div style=\"display:grid;grid-template-columns:100px 100px;width:200px\">"
        "<div id=o style=\"display:grid;grid-template-columns:1fr 1fr\">"
        "<div id=i1 style=\"background:#ff0000\">1</div><div id=i2 style=\"background:#00ff00\">2</div></div>"
        "<div id=b style=\"background:#0000ff\">B</div></div>"));
    expect_rect("i1", 0, 0, 50, 9);
    expect_rect("i2", 50, 0, 50, 9);
    expect_rect("b", 100, 0, 100, 9);
    CHECK(!c1_open(
        "<div style=\"display:flex;width:300px\">"
        "<div style=\"display:grid;grid-template-columns:50px 50px;width:100px\">"
        "<div id=a style=\"background:#ff0000\">A</div><div id=b style=\"background:#00ff00\">B</div></div>"
        "<div id=c style=\"width:50px;height:10px;background:#0000ff\"></div></div>"));
    expect_rect("b", 50, 0, 50, 9);
    expect_rect("c", 100, 0, 50, 10);
    /* Absolute and positioned items. */
    CHECK(!c1_open(
        "<div style=\"display:grid;grid-template-columns:100px 100px;width:200px;position:relative\">"
        "<div id=a style=\"background:#ff0000\">A</div>"
        "<div id=x style=\"position:absolute;right:0;top:0;width:10px;height:10px;background:#ffffff\"></div>"
        "<div id=b style=\"background:#00ff00;position:relative;left:3px\">B</div></div>"));
    expect_rect("a", 0, 0, 100, 9);
    expect_rect("b", 103, 0, 100, 9);         /* the absolute child took no cell */
    expect_rect("x", 190, 0, 10, 10);
    /* Border and padding on the grid container offset the tracks. */
    CHECK(!c1_open(
        "<div style=\"display:grid;grid-template-columns:50px 50px;border:2px solid #ff0000;padding:3px\">"
        "<div id=a style=\"background:#ff0000\">A</div></div>"));
    expect_rect("a", 5, 5, 50, 9);
    /* Overflowing the item cap or the row cap degrades, never crashes. */
    char big[8192];
    int n = snprintf(big, sizeof big, "<div style=\"display:grid;grid-template-columns:1fr\">");
    for (int i = 0; i < 150 && n < (int)sizeof big - 64; i++)
        n += snprintf(big + n, sizeof big - (size_t)n, "<div>x</div>");
    snprintf(big + n, sizeof big - (size_t)n, "</div>");
    rb_open(big, (int)strlen(big));
    rb_relayout();
    CHECK(rb_oom());
    CHECK(rb_box_count() > 0);
}


int main(void) {
    test_focus_hit_and_nav();
    test_css_cascade();
    test_compound_selectors();
    test_script_and_hidden();
    test_script_heavy_page();
    test_inline_row_fields();
    test_resolve();
    test_entity_bounds();
    test_resolve_bounds();
    test_style_cache();
    test_style_memo_mutation();
    test_form_submit();
    test_form_action_variants();
    test_form_forms_and_scopes();
    test_form_post_unsupported();
    test_form_field_policy();
    test_unquoted_attrs();
    test_named_entities();
    test_oom_flag();
    test_dom_tree_ops();
    test_dom_recycle();
    test_dom_subtree_remove();
    test_dom_set_text_renders();
    test_dom_id_and_attr();
    test_dom_class_ops();
    test_dom_rect();
    test_dom_set_text_no_oom();
    test_ev_registration();
    test_ev_bubble_order();
    test_ev_capture_order();
    test_ev_stop();
    test_ev_prevent();
    test_ev_inline_onclick();
    test_ev_checkbox_radio();
    test_ev_dispatch_guards();
    test_ev_reentrancy();
    test_ev_reentrant_match();
    test_ev_inline_and_stop();
    test_ev_mutation_during_dispatch();
    test_ev_active_recycle();
    test_ev_submit_event();
    test_ev_payload();
    test_ev_mousemove_abi();
    test_click_after_prevented_mousedown();
    test_submit_nested_cross();
    test_ev_submit_via_form();
    test_ev_open_mid_dispatch();
    test_ev_click_mouse_vs_activate();
    test_ev_scroll_prevent_observable();
    test_ev_submit_depth_capped();
    test_dom_generation();
    test_ev_focusability();
    test_ev_focus();
    test_ev_type_events();
    test_dom_set_text_atomic();
    test_ev_gen_alias();
    test_ev_default_action_gen();
    test_ev_input_revert_gen();
    test_node_dead_accessors();
    test_ev_remove_keeps_listeners();
    test_ev_listener_overflow();
    test_ev_script_style_touch();
    test_ev_focus_overflow();
    test_type_attr_non_input();
    test_ev_open_clears_listeners();
    test_ev_depth_dropped();
    test_ev_remove_interleaved_order();
    test_flex_row();
    test_flex_justify();
    test_flex_gap();
    test_flex_column();
    test_flex_grow();
    test_flex_shrink();
    test_flex_align();
    test_flex_align_self();
    test_flex_none_child();
    test_flex_shorthand();
    test_flex_fallbacks();
    test_flex_nested();
    test_flex_wrap();
    test_flex_wrap_reverse();
    test_flex_min_max();
    test_flex_order();
    test_flex_gap_axes();
    test_flex_column_wrap();
    test_flex_basis0_cross();
    test_flex_nested_wrap();
    test_flex_order_wrap_reverse();
    test_flex_wrap_stretch_cross();
    test_flex_interactions();
    test_flex_nested_remeasure();
    test_flex_stretch_padding();
    test_flex_control_autobasis();
    test_flex_cross_pct();
    test_flex_auto_column_tall();
    test_flex_item_cap_oom();
    test_c1_relative();
    test_c1_absolute();
    test_c1_shrink_to_fit();
    test_c1_fixed();
    test_c1_zindex();
    test_c1_overflow();
    test_c1_visibility();
    test_c1_opacity();
    test_c1_cursor();
    test_c1_nested();
    test_c1_extent();
    test_c1_default_identity();
    test_c1_relayout_stable();
    test_c2_margin_auto();
    test_c2_boxsizing();
    test_c2_border();
    test_c2_radius();
    test_c2_text();
    test_c3_grid_tracks();
    test_c3_grid_placement();
    test_c3_grid_align();
    test_c3_grid_interactions();
    if (fails) {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    printf("rbrowse engine checks passed\n");
    return 0;
}
