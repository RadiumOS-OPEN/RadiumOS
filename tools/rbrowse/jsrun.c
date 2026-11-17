/* Host page runner: loads a real URL the way rbrowse does (page, stylesheets,
   scripts in order, DOMContentLoaded, load, then timers for a few simulated
   seconds) and prints the console plus the visible text, so a site's
   scripts can be debugged without booting the OS. Network goes through curl.

     T=/tmp; Q=src/quickjs   # $T/font.c: made as in test_paint.c
     clang -g -O1 -DRBROWSE_HOST '-DCONFIG_VERSION="host"' -I src/rbrowse -I $Q -w \
         tools/rbrowse/jsrun.c src/rbrowse/engine.c $Q/quickjs.c $Q/libregexp.c \
         $Q/libunicode.c $Q/cutils.c $Q/dtoa.c $Q/rjs.c $Q/rjs_dom.c $Q/rjs_canvas.c $Q/dom_prelude.c \
         src/rbrowse/canvas.c src/rbrowse/image.c src/stb/stb_image.c $T/font.c \
         -lm -o /tmp/jsrun
     /tmp/jsrun https://example.com/ [seconds] [nojs]
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include "engine.h"
#include "rjs.h"
#include "rjs_dom.h"
#include "compat/qjs_port.h"
#include "compat/qjs_alloc.h"

volatile uint32_t ticks = 1000;
static QjsSink g_sink;
void qjs_set_sink(QjsSink s) { g_sink = s; }
void qjs_write(const char *s, int len) { if (g_sink) g_sink(s, len); else fwrite(s, 1, (size_t)len, stderr); }
void qjs_alloc_reset(void) {}
void qjs_alloc_stats(size_t *a, size_t *b, size_t *c) { if (a) *a = 0; if (b) *b = 0; if (c) *c = 256u << 20; }

static void console_out(const char *s, int len) { fprintf(stderr, "\033[33m%.*s\033[0m", len, s); }

/* Length of "scheme://host[:port]" at the start of url. */
static int origin_len(const char *u) {
    const char *p = strstr(u, "://");
    if (!p) return 0;
    p += 3;
    while (*p && *p != '/' && *p != '?' && *p != '#') p++;
    return (int)(p - u);
}

/* curl into out; returns length or -1. same_origin (a script's fetch):
   redirects are followed by hand and must keep the origin, as in the
   kernel's rbrowse_http_get_same_origin. */
static int http_get(const char *url, char *out, int cap, int same_origin) {
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) return -1;
    for (const char *p = url; *p; p++) if (*p == '\'' || *p == '\\') return -1;
    char cmd[2048];
    if (same_origin) {
        char cur[1024], loc[1024];
        snprintf(cur, sizeof(cur), "%s", url);
        for (int hop = 0; hop <= 5; hop++) {
            snprintf(cmd, sizeof(cmd), "curl -s -o /dev/null --max-time 20 -w '%%{redirect_url}' '%s'", cur);
            FILE *p = popen(cmd, "r");
            if (!p) return -1;
            size_t n = fread(loc, 1, sizeof(loc) - 1, p);
            pclose(p);
            loc[n] = 0;
            if (!loc[0]) break;                       /* not a redirect: fetch cur below */
            int ol = origin_len(url);
            if (hop == 5 || origin_len(loc) != ol || strncasecmp(loc, url, (size_t)ol) != 0) {
                fprintf(stderr, "  fetch %s -> redirect to another origin refused\n", url);
                return -1;
            }
            for (const char *q = loc; *q; q++) if (*q == '\'' || *q == '\\') return -1;
            snprintf(cur, sizeof(cur), "%s", loc);
        }
        snprintf(cmd, sizeof(cmd), "curl -s --max-time 20 -A 'Mozilla/5.0 (X11; Linux i686; rv:140.0) Gecko/20100101 Firefox/140.0' -H 'Accept-Language: en-US,en;q=0.5' '%s'", cur);
    } else {
        snprintf(cmd, sizeof(cmd), "curl -sL --max-time 20 -A 'Mozilla/5.0 (X11; Linux i686; rv:140.0) Gecko/20100101 Firefox/140.0' -H 'Accept-Language: en-US,en;q=0.5' '%s'", url);
    }
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    int n = (int)fread(out, 1, (size_t)cap - 1, p);
    int rc = pclose(p);
    if (rc != 0 && n <= 0) return -1;
    out[n] = 0;
    fprintf(stderr, "  fetch %s -> %d bytes\n", url, n);
    return n;
}

static char g_page[4 << 20], g_buf[4 << 20];

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: jsrun url [seconds] [nojs]\n"); return 2; }
    const char *url = argv[1];
    int seconds = argc > 2 ? atoi(argv[2]) : 3;
    int js = !(argc > 3 && strcmp(argv[3], "nojs") == 0);
    int n = http_get(url, g_page, sizeof(g_page), 0);
    if (n < 0) { fprintf(stderr, "page fetch failed\n"); return 1; }
    rb_set_scripting(js);
    rb_set_viewport(622, 332);      /* the kernel's 640x360 screen (paint.c rb_ui_layout) */
    rb_open_web(g_page, n);
    for (int i = 0; i < rb_css_pending() && i < 24; i++) {
        char href[900], abs[900];
        rb_css_href(i, href, (int)sizeof(href));
        if (!rb_resolve(url, href, abs, (int)sizeof(abs))) continue;
        int cn = http_get(abs, g_buf, sizeof(g_buf), 0);
        if (cn > 0) rb_css_provide(i, g_buf, cn);
    }
    rb_relayout();
    if (js) {
        rjs_set_output(console_out);
        rjs_start();
        if (rjs_dom_install(url, http_get) != 0) return 1;
        for (int i = 0; i < rb_script_count(); i++) {
            int node, len;
            const char *text;
            if (!rb_script_get(i, &node, &text, &len)) continue;
            char src[900], type[64] = "";
            if (rb_dom_has_attr(node, "type", 4)) rb_dom_get_attr(node, "type", 4, type, sizeof(type));
            if (type[0] && !strstr(type, "javascript")) { fprintf(stderr, "  skip script type=%s\n", type); continue; }
            if (rb_dom_has_attr(node, "src", 3) && rb_dom_get_attr(node, "src", 3, src, sizeof(src))) {
                char abs[900];
                if (!rb_resolve(url, src, abs, sizeof(abs))) continue;
                int sn = http_get(abs, g_buf, sizeof(g_buf), 0);
                if (sn >= 0) rjs_dom_exec(node, g_buf, sn, abs);
            } else if (len > 0) {
                fprintf(stderr, "  inline script %d bytes\n", len);
                rjs_dom_exec(node, text, len, url);
            }
        }
        rjs_dom_loaded();
        rb_dispatch(1, RB_EV_LOAD);
        for (int t = 0; t < seconds * 100; t++) {
            ticks += 10;
            rjs_dom_tick();
            if (rjs_dom_take_dirty()) rb_relayout();
        }
        if (rjs_dom_take_dirty()) rb_relayout();
        char nav[512];
        if (rb_take_nav(nav, sizeof(nav))) fprintf(stderr, "  script navigates to %s\n", nav);
        /* JSRUN_EVAL="expr": print an expression's value after the run */
        const char *ev = getenv("JSRUN_EVAL");
        if (ev) {
            static char out[8192];
            rjs_eval(ev, -1, "<jsrun>", 2000, out, sizeof(out));
            printf("eval: %s\n", out);
        }
    }
    printf("page height %d, %d boxes, oom %d\n", rb_page_height(), rb_box_count(), rb_oom());
    int shown = 0;
    for (int i = 0; i < rb_box_count() && shown < 60; i++) {
        int len;
        const char *t = rb_box_text(i, &len);
        const RbBox *b = rb_box(i);
        if (!t || len <= 0) continue;
        printf("%5d,%-5d %.*s\n", b->x, b->y, len > 70 ? 70 : len, t);
        shown++;
    }
    return 0;
}
