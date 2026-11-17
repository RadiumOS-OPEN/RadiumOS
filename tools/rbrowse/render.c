/* Host page renderer: paints a page with the kernel's own painter (paint.c) and
   writes the screenfuls as a PPM, so layout can be looked at instead of guessed.

   Build (font is lifted from terminal.c so there is one copy):
     (echo '#include <stdint.h>'; sed -n '/^uint8_t vga_font_8x8\[256\]\[8\] = {/,/^};/p' \
         src/terminal/terminal.c) > $T/font.c
     clang -g -O1 -DRBROWSE_HOST -I src/rbrowse tools/rbrowse/render.c \
         src/rbrowse/engine.c src/rbrowse/paint.c src/rbrowse/image.c src/stb/stb_image.c \
         $T/font.c -o $T/render
   Run: render page.html out.ppm [frames] [first_scroll_frame] [cssdir]
   Each frame is one viewport (page area plus scrollbar column) of the
   640x360 screen; frames are stacked vertically. Environment:
     RB_SIZE=320x200  lay out and paint for another screen size (the mode 13h
                      fallback is 320x200; the UI supports up to 640x360)
     RB_8BPP=1        show what the mode 13h fallback displays: the frame goes
                      through rb_quantize_8bpp and the fallback palette
     RB_BASE=url      the page's own URL: <img> URLs resolve against it
                      (default: the input file's path)
     RB_CURL=1        fetch http(s) images with curl (otherwise only local
                      files and data: URLs load; the rest show as broken)
     RB_NOIMG=1       no images at all (the placeholders a host without a
                      loader shows)
   Images load like rbrowse.c's loader: <img src>, srcset width descriptors
   against the laid-out width, the same caps (image.c). */
#include "engine.h"
#include "paint.h"
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t fb[RB_FB_MAX_W * RB_FB_MAX_H];
static uint8_t imgbuf[RB_IMAGE_FILE_MAX + 1];

/* The encoded bytes of an absolute image URL, or -1. */
static int fetch_image(const char *url) {
    if (!strncmp(url, "data:", 5)) return rb_image_data_url(url, imgbuf, RB_IMAGE_FILE_MAX);
    FILE *f;
    int curl = !strncmp(url, "http://", 7) || !strncmp(url, "https://", 8);
    if (curl) {
        if (!getenv("RB_CURL") || strchr(url, '\'') || strchr(url, '\\')) return -1;
        char cmd[RB_IMAGE_URL_MAX + 128];
        snprintf(cmd, sizeof(cmd), "curl -sfL -m 20 --max-filesize %d '%s'", RB_IMAGE_FILE_MAX, url);
        f = popen(cmd, "r");
    } else {
        f = fopen(url, "rb");
    }
    if (!f) return -1;
    int n = (int)fread(imgbuf, 1, sizeof(imgbuf), f);
    if (curl) pclose(f); else fclose(f);
    return n > 0 && n <= RB_IMAGE_FILE_MAX ? n : -1;
}

/* Every <img>: pick, resolve, fetch (shared per URL), decode; then one relayout. */
static void load_images(const char *base) {
    static char raw[RB_IMAGE_URL_MAX], url[RB_IMAGE_URL_MAX], set[RB_IMAGE_URL_MAX];
    int ok = 0, bad = 0;
    for (int node = rb_dom_img_next(0); node; node = rb_dom_img_next(node)) {
        int x, y, w = 0, h, is_w = 0, st = RB_IMG_EMPTY, slot = -1, nw = 0, nh = 0;
        if (!rb_dom_rect(node, &x, &y, &w, &h) || w <= 0) w = rb_ui.view_w;
        int got = 0;
        if (rb_dom_get_attr(node, "srcset", 6, set, (int)sizeof(set)) &&
            rb_image_pick_srcset(set, (int)strlen(set), w, raw, (int)sizeof(raw), &is_w) && is_w) got = 1;
        if (!got && rb_dom_get_attr(node, "src", 3, raw, (int)sizeof(raw)) && raw[0]) got = 1;
        if (got) {
            st = RB_IMG_BROKEN;
            if (!strncmp(raw, "data:", 5)) snprintf(url, sizeof(url), "%s", raw);
            else if (!rb_resolve(base, raw, url, (int)sizeof(url))) url[0] = 0;
            if (url[0]) {
                slot = rb_image_find(url);
                if (slot < 0 && (slot = rb_image_add(url)) >= 0) {
                    int n = rb_image_unsupported(url) ? -1 : fetch_image(url);
                    if (n <= 0 || rb_image_decode(slot, imgbuf, n) != RB_IMAGE_OK) {
                        rb_image_fail(slot);
                        fprintf(stderr, "   img failed (%s): %.120s\n", n <= 0 ? "fetch" : rb_image_error(), url);
                    }
                }
                if (slot >= 0 && rb_image_state(slot, &nw, &nh) == RB_IMAGE_OK) st = RB_IMG_OK;
            }
        }
        rb_dom_img_set(node, st, slot, nw, nh);
        if (st == RB_IMG_OK) ok++;
        else if (st == RB_IMG_BROKEN) bad++;
    }
    rb_relayout();
    fprintf(stderr, "   images: %d shown, %d broken, %d distinct, %u KB stored\n", ok, bad, rb_image_count(),
            rb_image_store_used() / 1024);
}
static uint8_t fb8[RB_FB_MAX_W * RB_FB_MAX_H];

/* The mode 13h fallback palette (rbrowse.c install_palette). */
static uint32_t palette(int i) {
    if (i < 16) return rb_ui_palette[i];
    int r, g, b;
    if (i <= 231) { rb_cube_rgb(i, &r, &g, &b); return RB_RGB(r, g, b); }
    int v = (i - 232) * 10;
    if (v > 255) v = 255;
    return RB_RGB(v, v, v);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: render in.html out.ppm [frames] [skip]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    static char raw[1 << 20];
    int n = (int)fread(raw, 1, sizeof(raw) - 1, f);
    fclose(f);
    int frames = argc > 3 ? atoi(argv[3]) : 3;
    int skip = argc > 4 ? atoi(argv[4]) : 0;
    int m = n;
    int sw = RB_FB_MAX_W, sh = RB_FB_MAX_H;
    if (getenv("RB_SIZE")) sscanf(getenv("RB_SIZE"), "%dx%d", &sw, &sh);
    if (sw < 160 || sw > RB_FB_MAX_W || sh < 100 || sh > RB_FB_MAX_H) { fprintf(stderr, "bad RB_SIZE\n"); return 2; }
    int q8 = getenv("RB_8BPP") && atoi(getenv("RB_8BPP"));
    rb_ui_layout(sw, sh);
    rb_set_viewport(rb_ui.view_w, rb_ui.content_h);
    const RbUi *u = &rb_ui;
    int images = !(getenv("RB_NOIMG") && atoi(getenv("RB_NOIMG")));
    rb_set_images(images);
    rb_paint_set_images(rb_image_view_node);
    rb_open_web(raw, m);
    /* optional: a directory of fetched stylesheets, named after the href with
       every non-alphanumeric turned into '_' (first 100 chars) */
    const char *cssdir = argc > 5 ? argv[5] : 0;
    for (int i = 0; cssdir && i < rb_css_pending() && i < 24; i++) {
        char href[400], name[128], path[600];
        rb_css_href(i, href, (int)sizeof(href));
        int w = 0;
        for (int k = 0; href[k] && w < 100; k++)
            name[w++] = (href[k] >= '0' && href[k] <= '9') || (href[k] >= 'a' && href[k] <= 'z') ||
                        (href[k] >= 'A' && href[k] <= 'Z') ? href[k] : '_';
        name[w] = 0;
        snprintf(path, sizeof(path), "%s/%s", cssdir, name);
        FILE *cf = fopen(path, "rb");
        if (!cf) { fprintf(stderr, "   css %d missing: %s\n", i, href); continue; }
        static char cssbuf[1 << 20];
        int cn = (int)fread(cssbuf, 1, sizeof(cssbuf) - 1, cf);
        fclose(cf);
        rb_css_provide(i, cssbuf, cn);
        fprintf(stderr, "   css %d: %d bytes (%s)\n", i, cn, href);
    }
    if (cssdir) rb_relayout();
    if (images) load_images(getenv("RB_BASE") ? getenv("RB_BASE") : argv[1]);
    fprintf(stderr, "%s: %d bytes -> %d compact, %d boxes, page height %d%s\n", argv[1], n, m,
            rb_box_count(), rb_page_height(), rb_oom() ? " [OOM: content dropped]" : "");

    fprintf(stderr, "   nodes %d boxes %d pool %d atoms %d rules %d decls %d intern %d/%d work %d\n",
            rb_debug_stat(0), rb_debug_stat(1), rb_debug_stat(2), rb_debug_stat(3), rb_debug_stat(4),
            rb_debug_stat(5), rb_debug_stat(6), rb_debug_stat(7), rb_debug_stat(8));
    fprintf(stderr, "   css vars %d%s\n", rb_debug_stat(10), "");
    if (rb_oom()) fprintf(stderr, "   OOM first at %s:%d\n", rb_debug_oom_file(), rb_debug_oom_line());
    {
        int hot = rb_debug_stat(11);
        char sty[512], cls[96] = "";
        rb_debug_style(hot, sty, (int)sizeof(sty));
        rb_dom_get_attr(hot, "class", 5, cls, (int)sizeof(cls));
        fprintf(stderr, "   most laid out: node %d x%d class='%s' %s\n", hot, rb_debug_stat(12), cls, sty);
    }
    fprintf(stderr, "   layout stack (host frames) %d bytes\n", rb_debug_stat(9));
    /* RB_DUMP=id1,id2: print the computed style of those element ids */
    const char *dump = getenv("RB_DUMP");
    while (dump && *dump) {
        char id[64];
        int k = 0;
        while (*dump && *dump != ',' && k < 63) id[k++] = *dump++;
        id[k] = 0;
        if (*dump == ',') dump++;
        int nd = rb_find_id(id);
        char sty[512];
        rb_debug_style(nd, sty, (int)sizeof(sty));
        fprintf(stderr, "   #%s (node %d): %s\n", id, nd, sty);
    }
    /* RB_DUMPTAG=<enum value>: the first few elements of that tag */
    if (getenv("RB_DUMPTAG")) {
        int want = atoi(getenv("RB_DUMPTAG")), shown = 0;
        for (int nd = 1; nd < rb_debug_stat(0) && shown < 4; nd++) {
            if (rb_dom_is_text(nd) || rb_node_tag(nd) != want) continue;
            char sty[512];
            rb_debug_style(nd, sty, (int)sizeof(sty));
            fprintf(stderr, "   node %d: %s\n", nd, sty);
            shown++;
        }
    }
    /* RB_DUMPTEXT=word: the computed style of the first box showing that word,
       and of its ancestors */
    if (getenv("RB_DUMPTEXT")) {
        const char *want = getenv("RB_DUMPTEXT");
        int wl = (int)strlen(want);
        for (int i = 0; i < rb_box_count(); i++) {
            int tl = 0;
            const char *t = rb_box_text(i, &tl);
            if (tl < wl || memcmp(t, want, (size_t)wl) != 0) continue;
            const RbBox *b = rb_box(i);
            fprintf(stderr, "   box %d at %d,%d %dx%d node %d\n", i, b->x, b->y, b->w, b->h, b->node);
            int nd = b->node;
            for (int up = 0; up < 9 && nd > 1; up++) {
                char sty[512], cls[96] = "";
                rb_debug_style(nd, sty, (int)sizeof(sty));
                rb_dom_get_attr(nd, "class", 5, cls, (int)sizeof(cls));
                fprintf(stderr, "     <%d class='%s'> %s\n", rb_node_tag(nd), cls, sty);
                nd = rb_dom_parent(nd);
            }
            break;
        }
    }
    FILE *o = fopen(argv[2], "wb");
    if (!o) { perror(argv[2]); return 1; }
    fprintf(o, "P6\n%d %d\n255\n", u->w, u->content_h * frames);
    for (int fr = 0; fr < frames; fr++) {
        memset(fb, 0, sizeof(fb));
        rb_paint_begin(fb, u->w, u->h);
        rb_rect(u->scroll_x, u->content_y, u->scroll_w, u->content_h, UI_SCROLL, 0, 0, u->w, u->h);
        rb_paint_page((skip + fr) * u->content_h, 0);
        if (q8) rb_quantize_8bpp(fb, fb8, u->w, u->h);
        for (int y = u->content_y; y < u->content_y + u->content_h; y++) {
            for (int x = 0; x < u->w; x++) {
                uint32_t c = q8 ? palette(fb8[y * u->w + x]) : fb[y * u->w + x];
                fputc(RB_R(c), o); fputc(RB_G(c), o); fputc(RB_B(c), o);
            }
        }
    }
    fclose(o);
    return 0;
}
