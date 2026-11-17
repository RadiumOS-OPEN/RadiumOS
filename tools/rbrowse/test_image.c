/* Host checks for rbrowse images: the stb decoder behind image.c (formats,
   caps, truncated input), the per-page table and its arenas, scaling, the
   srcset/data: helpers, replaced-element layout in image mode (engine.c)
   and painting an image box (paint.c). Run from the repository root: the
   sample images are read from tools/rbrowse/img.

     (echo '#include <stdint.h>'; sed -n '/^uint8_t vga_font_8x8\[256\]\[8\] = {/,/^};/p' \
         src/terminal/terminal.c) > $T/font.c
     clang -g -O1 -fsanitize=address,undefined -fwrapv -DRBROWSE_HOST -I src/rbrowse \
         tools/rbrowse/test_image.c src/rbrowse/engine.c src/rbrowse/paint.c \
         src/rbrowse/image.c src/stb/stb_image.c $T/font.c -o $T/rbtest_image && $T/rbtest_image
*/
#include "engine.h"
#include "paint.h"
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("fail %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        fails++; \
    } \
} while (0)

static uint8_t *slurp(const char *name, int *len) {
    char path[256];
    snprintf(path, sizeof(path), "tools/rbrowse/img/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("missing %s (run from the repository root)\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { printf("short read %s\n", path); exit(1); }
    fclose(f);
    *len = (int)n;
    return b;
}

/* A 24-bit BMP of w x h filled by fn (bottom-up rows, padded to 4 bytes). */
static uint8_t *make_bmp(int w, int h, uint32_t (*fn)(int x, int y), int *len) {
    int row = (w * 3 + 3) & ~3;
    int n = 54 + row * h;
    uint8_t *b = calloc(1, (size_t)n);
    b[0] = 'B'; b[1] = 'M';
    memcpy(b + 2, &n, 4);
    int off = 54, hs = 40, planes = 1, bpp = 24;
    memcpy(b + 10, &off, 4);
    memcpy(b + 14, &hs, 4);
    memcpy(b + 18, &w, 4);
    memcpy(b + 22, &h, 4);
    memcpy(b + 26, &planes, 2);
    memcpy(b + 28, &bpp, 2);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = fn(x, y);
            uint8_t *p = b + 54 + (size_t)(h - 1 - y) * row + x * 3;
            p[0] = (uint8_t)c; p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)(c >> 16);
        }
    *len = n;
    return b;
}

static uint32_t xor_fn(int x, int y) { return (uint32_t)((x ^ y) & 255) * 0x010101u; }

/* An 8-bit grey-palette BMP (1 byte per pixel, so a big one stays small). */
static uint8_t *make_bmp8(int w, int h, int *len) {
    int row = (w + 3) & ~3;
    int off = 54 + 256 * 4;
    int n = off + row * h;
    uint8_t *b = calloc(1, (size_t)n);
    b[0] = 'B'; b[1] = 'M';
    memcpy(b + 2, &n, 4);
    int hs = 40, planes = 1, bpp = 8, ncol = 256;
    memcpy(b + 10, &off, 4);
    memcpy(b + 14, &hs, 4);
    memcpy(b + 18, &w, 4);
    memcpy(b + 22, &h, 4);
    memcpy(b + 26, &planes, 2);
    memcpy(b + 28, &bpp, 2);
    memcpy(b + 46, &ncol, 4);
    for (int i = 0; i < 256; i++) memset(b + 54 + i * 4, i, 3);
    for (int y = 0; y < h; y++) memset(b + off + (size_t)y * row, (y * 7) & 255, (size_t)w);
    *len = n;
    return b;
}

/* A PNG whose IHDR claims w x h, followed by an empty IDAT (no pixels): stb
   reads the size from it, decoding fails. CRCs are left 0 (stb skips them). */
static int make_png_header(uint8_t *b, uint32_t w, uint32_t h) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    memset(b, 0, 45);
    memcpy(b, sig, 8);
    uint8_t ihdr[25] = {0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    for (int i = 0; i < 4; i++) { ihdr[8 + i] = (uint8_t)(w >> (24 - 8 * i)); ihdr[12 + i] = (uint8_t)(h >> (24 - 8 * i)); }
    ihdr[16] = 8; ihdr[17] = 6;
    memcpy(b + 8, ihdr, 25);
    memcpy(b + 37, "IDAT", 4);
    return 45;
}

static int near(int a, int b, int tol) { return a - b <= tol && b - a <= tol; }

static void test_decode_formats(void) {
    static const struct { const char *name; int w, h; } files[] = {
        {"sun.png", 96, 64}, {"lake.jpg", 160, 100}, {"lake_prog.jpg", 160, 100},
        {"stripes.gif", 40, 24}, {"ramp.bmp", 30, 20}, {"check.png", 16, 16},
    };
    rb_image_reset();
    for (int i = 0; i < 6; i++) {
        int len;
        uint8_t *b = slurp(files[i].name, &len);
        int s = rb_image_add(files[i].name);
        CHECK(s == i);
        CHECK(rb_image_decode(s, b, len) == RB_IMAGE_OK);
        int nw = 0, nh = 0;
        CHECK(rb_image_state(s, &nw, &nh) == RB_IMAGE_OK);
        CHECK(nw == files[i].w && nh == files[i].h);
        free(b);
    }
    RbImageView v;
    /* GIF: first stripe is palette entry 2 (0,120,255); the diagonal is white */
    CHECK(rb_image_view(3, 40, 24, &v) && v.w == 40 && v.h == 24 && !v.alpha);
    CHECK(v.px[0] == 0xFF0078FFu);
    CHECK(v.px[1 * 40 + 9] == 0xFFFFFFFFu);
    CHECK(v.px[6 * 40 + 0] == 0xFFFFC800u);
    /* BMP: B = 255 - 8x, G = 8x, R = 40 on every row */
    CHECK(rb_image_view(4, 30, 20, &v) && v.px[0] == 0xFF2800FFu && v.px[19 * 30 + 2] == 0xFF2810EFu);
    /* PNG with alpha: transparent corner, opaque centre */
    CHECK(rb_image_view(0, 96, 64, &v) && v.alpha);
    CHECK((v.px[0] >> 24) == 0 && (v.px[30 * 96 + 48] >> 24) == 255);
    /* JPEG: the sky at the top left is about (70, 130, 255) */
    CHECK(rb_image_view(1, 160, 100, &v) && !v.alpha);
    uint32_t p = v.px[2 * 160 + 2];
    CHECK(near((int)(p >> 16 & 255), 70, 30) && near((int)(p >> 8 & 255), 130, 30) && near((int)(p & 255), 255, 30));
    uint32_t q;
    CHECK(rb_image_view(2, 160, 100, &v));
    q = v.px[2 * 160 + 2];
    CHECK(near((int)(q >> 16 & 255), (int)(p >> 16 & 255), 12));
    CHECK(rb_image_count() == 6);
    CHECK(rb_image_find("lake.jpg") == 1 && rb_image_find("lake.png") == -1);
}

static void test_caps(void) {
    rb_image_reset();
    uint8_t buf[64];
    int n = make_png_header(buf, 5000, 10);        /* stb's own 4096 limit refuses it */
    int s = rb_image_add("wide");
    CHECK(rb_image_decode(s, buf, n) == RB_IMAGE_BROKEN);
    n = make_png_header(buf, 2000, 2000);          /* 4 Mpx: over the pixel budget */
    s = rb_image_add("big");
    CHECK(rb_image_decode(s, buf, n) == RB_IMAGE_BROKEN);
    CHECK(strstr(rb_image_error(), "cap") != 0);
    n = make_png_header(buf, 1000, 1000);          /* allowed size, no data: decoder error */
    s = rb_image_add("nodata");
    CHECK(rb_image_decode(s, buf, n) == RB_IMAGE_BROKEN);
    s = rb_image_add("empty");
    CHECK(rb_image_decode(s, buf, 0) == RB_IMAGE_BROKEN);
    CHECK(rb_image_decode(s, (const uint8_t *)"hello world, not an image", 25) == RB_IMAGE_BROKEN);
    CHECK(rb_image_decode(s, buf, RB_IMAGE_FILE_MAX + 1) == RB_IMAGE_BROKEN);
    CHECK(rb_image_state(s, 0, 0) == RB_IMAGE_BROKEN);
    CHECK(rb_image_state(99, 0, 0) == RB_IMAGE_BROKEN);
    CHECK(rb_image_decode(99, buf, n) == RB_IMAGE_BROKEN);
    CHECK(rb_image_store_used() == 0);

    /* truncated files never crash (ASan watches) and leave no store behind on failure */
    static const char *names[] = {"sun.png", "lake.jpg", "lake_prog.jpg", "stripes.gif", "ramp.bmp"};
    for (int f = 0; f < 5; f++) {
        int len;
        uint8_t *b = slurp(names[f], &len);
        for (int cut = 0; cut < len; cut += cut < 64 ? 1 : 37) {
            rb_image_reset();
            uint8_t *t = malloc((size_t)(cut ? cut : 1));
            memcpy(t, b, (size_t)cut);
            int r = rb_image_decode(rb_image_add("t"), t, cut);
            CHECK(r == RB_IMAGE_OK || r == RB_IMAGE_BROKEN);
            if (r == RB_IMAGE_BROKEN) CHECK(rb_image_store_used() == 0);
            free(t);
        }
        free(b);
    }

    /* larger than the stored-copy cap: reduced by an integer factor */
    rb_image_reset();
    int len;
    uint8_t *b = make_bmp(3000, 10, xor_fn, &len);
    s = rb_image_add("long");
    CHECK(rb_image_decode(s, b, len) == RB_IMAGE_OK);
    int nw, nh;
    CHECK(rb_image_state(s, &nw, &nh) == RB_IMAGE_OK && nw == 3000 && nh == 10);
    RbImageView v;
    CHECK(rb_image_view(s, 1000, 4, &v) && v.w == 1000 && v.h == 4);
    free(b);

    /* the table holds RB_IMAGE_MAX images */
    rb_image_reset();
    char u[16];
    for (int i = 0; i < RB_IMAGE_MAX; i++) {
        snprintf(u, sizeof(u), "u%d", i);
        CHECK(rb_image_add(u) == i);
    }
    CHECK(rb_image_add("one more") == -1);
    CHECK(rb_image_find("u17") == 17);
    rb_image_reset();
    CHECK(rb_image_count() == 0 && rb_image_find("u17") == -1);

    /* page memory: 1024x1024 copies (4 MB each) until the store refuses */
    b = make_bmp8(1024, 1024, &len);
    int ok = 0, broken = 0;
    for (int i = 0; i < 10; i++) {
        snprintf(u, sizeof(u), "m%d", i);
        if (rb_image_decode(rb_image_add(u), b, len) == RB_IMAGE_OK) ok++;
        else broken++;
    }
    CHECK(ok == (int)(RB_IMAGE_STORE_ARENA / (1024u * 1024 * 4)) && broken == 10 - ok);
    CHECK(strstr(rb_image_error(), "memory") != 0);
    free(b);
    rb_image_reset();
}

static void test_scale(void) {
    uint32_t src[100], dst[625], tmp[2500];
    for (int i = 0; i < 100; i++) src[i] = 0xFF336699u;
    rb_image_scale(src, 10, 10, 10, dst, 3, 7, tmp, 0);
    int same = 1;
    for (int i = 0; i < 21; i++) same &= dst[i] == 0xFF336699u;
    CHECK(same);
    rb_image_scale(src, 10, 10, 10, dst, 25, 25, tmp, 0);
    same = 1;
    for (int i = 0; i < 625; i++) same &= dst[i] == 0xFF336699u;
    CHECK(same);
    /* box filter: a 2x2 black/white checker averages to grey */
    uint32_t chk[4] = {0xFF000000u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFF000000u};
    rb_image_scale(chk, 2, 2, 2, dst, 1, 1, tmp, 0);
    CHECK(near((int)(dst[0] & 255), 127, 2) && (dst[0] >> 24) == 255);
    /* alpha-weighted: a transparent green neighbour does not tint red */
    uint32_t al[2] = {0x0000FF00u, 0xFFFF0000u};
    rb_image_scale(al, 2, 1, 2, dst, 1, 1, tmp, 1);
    CHECK((dst[0] & 0xFFFFFF) == 0xFF0000u && near((int)(dst[0] >> 24), 127, 2));
    /* bilinear enlarge keeps the end pixels and ramps in between */
    uint32_t ramp[2] = {0xFF000000u, 0xFF0000FFu};
    rb_image_scale(ramp, 2, 1, 2, dst, 8, 1, tmp, 0);
    CHECK((dst[0] & 255) == 0 && (dst[7] & 255) == 255 && (dst[3] & 255) > 60 && (dst[3] & 255) < 160);
    /* the view cache: same size, same buffer */
    rb_image_reset();
    int len;
    uint8_t *b = slurp("sun.png", &len);
    int s = rb_image_add("sun");
    CHECK(rb_image_decode(s, b, len) == RB_IMAGE_OK);
    RbImageView v1, v2;
    CHECK(rb_image_view(s, 48, 32, &v1) && rb_image_view(s, 48, 32, &v2) && v1.px == v2.px);
    CHECK(v1.w == 48 && v1.h == 32 && v1.alpha);
    CHECK(rb_image_view(s, 200, 130, &v2) && v2.w == 200 && v2.h == 130);
    /* a box too big for the cache gets the stored copy to sample */
    CHECK(rb_image_view(s, 4000, 3000, &v2) && v2.w == 96 && v2.h == 64);
    CHECK(!rb_image_view(s, 0, 10, &v2));
    /* wider than the scaler handles but small enough for the cache: the
       stored copy, not an unwritten block of the cache */
    CHECK(rb_image_view(s, 20000, 20, &v2) && v2.w == 96 && v2.h == 64);
    CHECK(rb_image_view(s, 20, 20000, &v2) && v2.w == 96 && v2.h == 64);
    free(b);
    rb_image_reset();
}

static void test_srcset_and_urls(void) {
    char out[256];
    int is_w = -1;
    const char *s1 = "a.jpg 320w, b.jpg 640w, c.jpg 1280w";
    CHECK(rb_image_pick_srcset(s1, (int)strlen(s1), 600, out, sizeof(out), &is_w) && !strcmp(out, "b.jpg") && is_w == 1);
    CHECK(rb_image_pick_srcset(s1, (int)strlen(s1), 2000, out, sizeof(out), 0) && !strcmp(out, "c.jpg"));
    CHECK(rb_image_pick_srcset(s1, (int)strlen(s1), 100, out, sizeof(out), 0) && !strcmp(out, "a.jpg"));
    const char *s2 = "big.jpg 2x, mid.jpg 1.5x";
    CHECK(rb_image_pick_srcset(s2, (int)strlen(s2), 600, out, sizeof(out), &is_w) && !strcmp(out, "mid.jpg") && is_w == 0);
    const char *s3 = "one.jpg, two.jpg 2x";
    CHECK(rb_image_pick_srcset(s3, (int)strlen(s3), 600, out, sizeof(out), 0) && !strcmp(out, "one.jpg"));
    const char *s4 = "//cdn.example/x,y.png 1x, z.png 2x";       /* commas inside a URL */
    CHECK(rb_image_pick_srcset(s4, (int)strlen(s4), 600, out, sizeof(out), 0) && !strcmp(out, "//cdn.example/x,y.png"));
    const char *s5 = " , ,, ";
    CHECK(!rb_image_pick_srcset(s5, (int)strlen(s5), 600, out, sizeof(out), 0));
    const char *s6 = "q.jpg 10q, r.jpg zzw";
    CHECK(!rb_image_pick_srcset(s6, (int)strlen(s6), 600, out, sizeof(out), 0));
    CHECK(!rb_image_pick_srcset(s1, (int)strlen(s1), 600, out, 3, 0));   /* does not fit */

    uint8_t b[64];
    CHECK(rb_image_data_url("data:image/png;base64,iVBORw0KGgo=", b, sizeof(b)) == 8);
    CHECK(b[0] == 0x89 && b[1] == 'P' && b[7] == 0x0A);
    CHECK(rb_image_data_url("DATA:image/png;BASE64, iVBO Rw0K\nGgo=", b, sizeof(b)) == 8 && b[3] == 'G');
    CHECK(rb_image_data_url("data:,a%20b%zz", b, sizeof(b)) == 6 && !memcmp(b, "a b%zz", 6));
    CHECK(rb_image_data_url("data:image/png;base64,iV*BO", b, sizeof(b)) == -1);
    CHECK(rb_image_data_url("data:image/png;base64", b, sizeof(b)) == -1);
    CHECK(rb_image_data_url("http://x/y.png", b, sizeof(b)) == -1);
    CHECK(rb_image_data_url("data:image/png;base64,iVBORw0KGgo=", b, 4) == -1);

    CHECK(rb_image_unsupported("http://x/a.svg") && rb_image_unsupported("/a.SVG?v=2") &&
          rb_image_unsupported("x.webp#f") && rb_image_unsupported("data:image/svg+xml;utf8,<svg/>"));
    CHECK(!rb_image_unsupported("http://x/a.png") && !rb_image_unsupported("a.svg.png") &&
          !rb_image_unsupported("data:image/gif;base64,R0lG") && !rb_image_unsupported("http://x/img?id=3"));
}

static int find_box(int node, int kind) {
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        if (b->kind == kind && (kind == RB_BOX_IMAGE ? (int)b->toff == node : b->node == node)) return i;
    }
    return -1;
}

static int rect_wh(int node, int *w, int *h) {
    int x, y;
    return rb_dom_rect(node, &x, &y, w, h);
}

static void test_layout(void) {
    rb_set_viewport(622, 332);
    rb_set_images(1);
    const char *html =
        "<html><body style=\"margin:0\">"
        "<img id=a src=a.png width=200 height=100>"
        "<p><img id=b src=b.png></p>"
        "<img id=c src=c.png width=60>"
        "<img id=d src=d.png style=\"height:40px\">"
        "<div style=\"width:100px\"><img id=e src=e.png></div>"
        "<img id=f src=f.png width=120 height=30 alt=\"Broken image here\">"
        "<p><a id=lnk href=\"/x\"><img id=g src=g.png width=40 height=20></a></p>"
        "<img id=h src=h.png style=\"max-width:50%\">"
        "<img id=i>"
        "<table><tr><td><img id=t src=t.png></td><td>words words words words words words words words "
        "words words words words words words words words words words words words words</td></tr></table>"
        "</body></html>";
    rb_open_web(html, (int)strlen(html));
    int a = rb_find_id("a"), b = rb_find_id("b"), c = rb_find_id("c"), d = rb_find_id("d"),
        e = rb_find_id("e"), f = rb_find_id("f"), g = rb_find_id("g"), h = rb_find_id("h"),
        i = rb_find_id("i"), t = rb_find_id("t"), lnk = rb_find_id("lnk");
    int w = 0, hh = 0;
    /* loading: an explicitly sized image already has its (unhalved) box */
    int ba = find_box(a, RB_BOX_IMAGE);
    CHECK(ba >= 0 && rb_box(ba)->w == 200 && rb_box(ba)->h == 100);
    CHECK(rb_dom_img_state(a, 0, 0, 0) == RB_IMG_NONE);
    CHECK(find_box(b, RB_BOX_IMAGE) < 0);              /* unsized, no alt: nothing yet */
    /* every <img>, attached or not */
    int det = rb_dom_create_element(RB_TAG_IMG);
    int seen = 0;
    for (int n = rb_dom_img_next(0); n; n = rb_dom_img_next(n)) seen++;
    CHECK(seen == 11 && det);
    /* outcomes: explicit size repaints, anything else needs a relayout */
    CHECK(rb_dom_img_set(a, RB_IMG_OK, 3, 200, 100) == 0);
    CHECK(rb_dom_img_set(b, RB_IMG_OK, 4, 120, 80) == 1);
    CHECK(rb_dom_img_set(c, RB_IMG_OK, 5, 120, 80) == 1);
    CHECK(rb_dom_img_set(d, RB_IMG_OK, 6, 120, 80) == 1);
    CHECK(rb_dom_img_set(e, RB_IMG_OK, 7, 300, 150) == 1);
    CHECK(rb_dom_img_set(f, RB_IMG_BROKEN, 0, 0, 0) == 1);
    CHECK(rb_dom_img_set(g, RB_IMG_OK, 8, 40, 20) == 0);
    CHECK(rb_dom_img_set(h, RB_IMG_OK, 9, 1000, 500) == 1);
    CHECK(rb_dom_img_set(i, RB_IMG_EMPTY, 0, 0, 0) == 1);
    CHECK(rb_dom_img_set(t, RB_IMG_OK, 10, 200, 100) == 1);
    CHECK(rb_dom_img_set(lnk, RB_IMG_OK, 1, 1, 1) == -1);
    rb_relayout();
    int slot = -1, nw = 0, nh = 0;
    CHECK(rb_dom_img_state(b, &slot, &nw, &nh) == RB_IMG_OK && slot == 4 && nw == 120 && nh == 80);
    CHECK(rect_wh(b, &w, &hh) && w == 120 && hh == 80);               /* natural size, not halved */
    CHECK(rect_wh(c, &w, &hh) && w == 60 && hh == 40);                /* width + ratio */
    CHECK(rect_wh(d, &w, &hh) && w == 60 && hh == 40);                /* CSS height + ratio */
    CHECK(rect_wh(e, &w, &hh) && w == 100 && hh == 50);               /* capped to the container */
    CHECK(rect_wh(h, &w, &hh) && w == 311 && hh == 155);              /* max-width: 50% */
    CHECK(rect_wh(t, &w, &hh) && w == 200 && hh == 100);              /* a table cell does not squeeze it */
    int bf = find_box(f, RB_BOX_BG);
    CHECK(bf >= 0 && find_box(f, RB_BOX_IMAGE) < 0);
    int found_alt = 0;
    for (int k = 0; k < rb_box_count(); k++) {
        int len;
        const char *tx = rb_box_text(k, &len);
        if (rb_box(k)->node == f && len >= 6 && !memcmp(tx, "Broken", 6)) found_alt = 1;
    }
    CHECK(found_alt);
    /* the image inside a link: the link owns the box (click), the image its rect */
    int bg = find_box(g, RB_BOX_IMAGE);
    CHECK(bg >= 0 && rb_box(bg)->node == lnk);
    CHECK(rect_wh(g, &w, &hh) && w == 40 && hh == 20);
    CHECK(rb_hit(rb_box(bg)->x + 5, rb_box(bg)->y + 5) == lnk);
    /* a new src forgets the outcome */
    CHECK(rb_dom_set_attr(b, "src", 3, "other.png", 9));
    CHECK(rb_dom_img_state(b, 0, 0, 0) == RB_IMG_NONE);
    CHECK(rb_dom_img_set(c, RB_IMG_OK, 5, 120, 80) == 1);
    CHECK(rb_dom_set_attr(c, "alt", 3, "x", 1) && rb_dom_img_state(c, 0, 0, 0) == RB_IMG_OK);
    CHECK(rb_dom_remove_attr(c, "srcset", 6) && rb_dom_img_state(c, 0, 0, 0) == RB_IMG_NONE);

    /* image mode off: the old placeholder geometry (height halved) */
    rb_set_images(0);
    const char *old = "<html><body style=\"margin:0\"><img id=o src=o.png width=200 height=100 alt=hi></body></html>";
    rb_open_web(old, (int)strlen(old));
    int o = rb_find_id("o");
    rb_dom_img_set(o, RB_IMG_OK, 1, 10, 10);
    rb_relayout();
    CHECK(find_box(o, RB_BOX_IMAGE) < 0 && find_box(o, RB_BOX_BG) >= 0);
    CHECK(rect_wh(o, &w, &hh) && w == 200 && hh == 50);

    /* native profile: only a decoded image replaces the "[alt]" control */
    rb_set_images(1);
    const char *nat = "<html><body><p>x <img id=n src=n.png alt=pic> y</p></body></html>";
    rb_open(nat, (int)strlen(nat));
    int nn = rb_find_id("n");
    CHECK(find_box(nn, RB_BOX_IMAGE) < 0 && find_box(nn, RB_BOX_CONTROL) >= 0);
    CHECK(rb_dom_img_set(nn, RB_IMG_OK, 2, 32, 16) == 1);
    rb_relayout();
    int bn = find_box(nn, RB_BOX_IMAGE);
    CHECK(bn >= 0 && rb_box(bn)->w == 32 && rb_box(bn)->h == 16);
    rb_set_images(0);
    rb_set_viewport(RB_VIEW_W_DEFAULT, RB_VIEW_H_DEFAULT);
}

static uint32_t g_fb[RB_FB_MAX_W * RB_FB_MAX_H];

static uint32_t px_at(int x, int y) { return g_fb[y * rb_ui.w + x]; }

static void test_paint(void) {
    rb_ui_layout(640, 360);
    rb_set_viewport(rb_ui.view_w, rb_ui.content_h);
    rb_set_images(1);
    rb_paint_set_images(rb_image_view_node);
    rb_image_reset();
    const char *html =
        "<html><body style=\"margin:0\">"
        "<img id=s src=stripes.gif><br>"
        "<img id=z src=stripes.gif width=80>"
        "<img id=p src=pending.png width=50 height=40>"
        "</body></html>";
    rb_open_web(html, (int)strlen(html));
    int s = rb_find_id("s"), z = rb_find_id("z"), p = rb_find_id("p");
    int len;
    uint8_t *b = slurp("stripes.gif", &len);
    int slot = rb_image_add("stripes.gif");
    CHECK(rb_image_decode(slot, b, len) == RB_IMAGE_OK);
    free(b);
    rb_dom_img_set(s, RB_IMG_OK, slot, 40, 24);
    rb_dom_img_set(z, RB_IMG_OK, slot, 40, 24);
    rb_relayout();
    rb_paint_begin(g_fb, rb_ui.w, rb_ui.h);
    rb_paint_page(0, 0);
    int bs = find_box(s, RB_BOX_IMAGE), bz = find_box(z, RB_BOX_IMAGE), bp = find_box(p, RB_BOX_IMAGE);
    CHECK(bs >= 0 && bz >= 0 && bp >= 0);
    if (bs >= 0 && bz >= 0 && bp >= 0) {
        const RbBox *x = rb_box(bs);
        int sx = rb_ui.view_x + x->x, sy = rb_ui.content_y + x->y;
        CHECK(px_at(sx, sy) == 0x0078FFu);                  /* 1:1 */
        CHECK(px_at(sx + 9, sy + 1) == 0xFFFFFFu);
        CHECK(px_at(sx, sy + 7) == 0xFFC800u);
        const RbBox *zb = rb_box(bz);
        CHECK(zb->w == 80 && zb->h == 48);
        uint32_t zp = px_at(rb_ui.view_x + zb->x + 2, rb_ui.content_y + zb->y + 2);
        CHECK(near((int)(zp & 255), 255, 8) && near((int)(zp >> 8 & 255), 120, 8));   /* scaled 2x */
        const RbBox *pb = rb_box(bp);
        CHECK(px_at(rb_ui.view_x + pb->x + 10, rb_ui.content_y + pb->y + 10) == 0xE8E8E8u);   /* still loading */
    }
    /* canvas API: decoded pixels of an <img> */
    const uint32_t *px = 0;
    int w = 0, h = 0, nw = 0, nh = 0;
    CHECK(rb_image_node_pixels(s, &px, &w, &h, &nw, &nh) && px && w == 40 && h == 24 && nw == 40 && px[0] == 0xFF0078FFu);
    CHECK(!rb_image_node_pixels(p, &px, &w, &h, &nw, &nh));
    rb_paint_set_images(0);
    rb_paint_page(0, 0);                                     /* no hook: placeholder, no crash */
    rb_set_images(0);
    rb_image_reset();
}

int main(void) {
    test_decode_formats();
    test_caps();
    test_scale();
    test_srcset_and_urls();
    test_layout();
    test_paint();
    if (fails) {
        printf("%d rbrowse image checks failed\n", fails);
        return 1;
    }
    printf("rbrowse image checks passed\n");
    return 0;
}
