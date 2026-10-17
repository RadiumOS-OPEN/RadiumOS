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
    int br, bgg, bb;
    rb_cube_rgb(bg, &br, &bgg, &bb);
    CHECK(br + bgg + bb > 40);
    int painted = 0;
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        int len = 0;
        const char *t = rb_box_text(i, &len);
        if (b->kind != RB_BOX_TEXT || len <= 0) continue;
        CHECK(b->color != (uint8_t)bg);
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
}

int main(void) {
    test_focus_hit_and_nav();
    test_css_cascade();
    test_script_and_hidden();
    test_script_heavy_page();
    test_inline_row_fields();
    test_resolve();
    if (fails) {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    printf("rbrowse engine checks passed\n");
    return 0;
}
