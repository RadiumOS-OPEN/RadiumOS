/* Host checks for the rbrowse painter (src/rbrowse/paint.c): the screen
   layout, clipping, the RGB blit entry point, opacity blending, page painting
   in exact RGB and the mode 13h quantizer.

   Build (font is lifted from terminal.c so there is one copy):
     (echo '#include <stdint.h>'; sed -n '/^uint8_t vga_font_8x8\[256\]\[8\] = {/,/^};/p' \
         src/terminal/terminal.c) > $T/font.c
     clang -g -O1 -fsanitize=address,undefined -DRBROWSE_HOST -I src/rbrowse \
         tools/rbrowse/test_paint.c src/rbrowse/engine.c src/rbrowse/paint.c $T/font.c \
         -o $T/rbtest_paint && $T/rbtest_paint
*/
#include "engine.h"
#include "paint.h"
#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("fail %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        fails++; \
    } \
} while (0)

static uint32_t fb[RB_FB_MAX_W * RB_FB_MAX_H];
static uint8_t fb8[RB_FB_MAX_W * RB_FB_MAX_H];

static uint32_t px(int x, int y) { return fb[y * rb_ui.w + x]; }

static void test_layout(void) {
    /* 320x200 is the original mode 13h chrome, pixel for pixel */
    rb_ui_layout(320, 200);
    CHECK(rb_ui.w == 320 && rb_ui.h == 200);
    CHECK(rb_ui.chrome_h == 16 && rb_ui.content_y == 16 && rb_ui.content_h == 176);
    CHECK(rb_ui.status_y == 192 && rb_ui.status_h == 8);
    CHECK(rb_ui.scroll_x == 312 && rb_ui.scroll_w == 8);
    CHECK(rb_ui.view_x == 4 && rb_ui.view_w == RB_VIEW_W_DEFAULT);
    CHECK(rb_ui.back_x == 2 && rb_ui.back_w == 14 && rb_ui.url_x == 18 && rb_ui.url_w == 250);
    CHECK(rb_ui.go_x == 272 && rb_ui.go_w == 38 && rb_ui.btn_y == 2 && rb_ui.btn_h == 12);
    CHECK(rb_ui.text_y == 4 && rb_ui.glyph == 1);

    rb_ui_layout(640, 360);
    const RbUi *u = &rb_ui;
    CHECK(u->w == 640 && u->h == 360 && u->glyph == 2);
    CHECK(u->content_y == u->chrome_h && u->content_y + u->content_h == u->status_y);
    CHECK(u->status_y + u->status_h == 360);
    CHECK(u->scroll_x + u->scroll_w == 640);
    CHECK(u->view_x + u->view_w + u->view_x == u->scroll_x);
    CHECK(u->view_w == 622 && u->content_h == 332);
    /* the chrome widgets do not overlap and stay on screen */
    CHECK(u->back_x + u->back_w < u->url_x);
    CHECK(u->url_x + u->url_w < u->go_x);
    CHECK(u->go_x + u->go_w <= 640);
    CHECK(u->btn_y + u->btn_h < u->chrome_h);
    CHECK(u->text_y >= u->btn_y && u->text_y + 8 <= u->btn_y + u->btn_h);

    /* nothing larger than the maximum */
    rb_ui_layout(4000, 4000);
    CHECK(rb_ui.w == RB_FB_MAX_W && rb_ui.h == RB_FB_MAX_H);
}

static void test_rect_and_text(void) {
    rb_ui_layout(640, 360);
    memset(fb, 0, sizeof(fb));
    rb_paint_begin(fb, 640, 360);
    rb_rect(-5, -5, 10, 10, 0x112233, 0, 0, 4, 4);
    CHECK(px(0, 0) == 0x112233 && px(3, 3) == 0x112233);
    CHECK(px(4, 4) == 0 && px(4, 0) == 0 && px(0, 4) == 0);
    /* off the framebuffer: clipped, no write past the end */
    rb_rect(630, 350, 50, 50, 0x445566, 0, 0, 10000, 10000);
    CHECK(px(639, 359) == 0x445566 && px(629, 359) == 0);
    rb_px(-1, 0, 0xFFFFFF);
    rb_px(640, 0, 0xFFFFFF);
    CHECK(px(0, 0) == 0x112233);
    /* text: the 8x8 glyph in an exact RGB colour, clipped */
    memset(fb, 0, sizeof(fb));
    rb_text_at(100, 100, "H", 0xABCDEF, 0, 0, 640, 360);
    int lit = 0, other = 0;
    for (int y = 100; y < 108; y++)
        for (int x = 100; x < 108; x++) {
            if (px(x, y) == 0xABCDEF) lit++;
            else if (px(x, y) != 0) other++;
        }
    CHECK(lit > 8 && other == 0);
    memset(fb, 0, sizeof(fb));
    rb_text_at(100, 100, "H", 0xABCDEF, 0, 0, 104, 104);
    for (int y = 100; y < 108; y++)
        for (int x = 100; x < 108; x++)
            if (x >= 104 || y >= 104) CHECK(px(x, y) == 0);
}

static void test_blit(void) {
    rb_ui_layout(640, 360);
    memset(fb, 0, sizeof(fb));
    rb_paint_begin(fb, 640, 360);
    uint32_t img[3 * 4];
    for (int i = 0; i < 12; i++) img[i] = 0x010101u * (uint32_t)(i + 1);
    /* 3x3 image out of a 4-wide stride, clipped to [11,100) x [11,100) */
    rb_blit_rgb(10, 10, img, 3, 3, 4, 0, 11, 11, 100, 100);
    CHECK(px(10, 10) == 0 && px(10, 11) == 0 && px(11, 10) == 0);
    CHECK(px(11, 11) == img[1 * 4 + 1]);
    CHECK(px(12, 12) == img[2 * 4 + 2]);
    CHECK(px(13, 12) == 0 && px(12, 13) == 0);
    /* negative origin and the framebuffer edge */
    rb_blit_rgb(-2, -2, img, 3, 3, 4, 0, -100, -100, 1000, 1000);
    CHECK(px(0, 0) == img[2 * 4 + 2]);
    rb_blit_rgb(639, 359, img, 3, 3, 4, 0, 0, 0, 1000, 1000);
    CHECK(px(639, 359) == img[0]);
    /* straight alpha: 0 skips, 255 overwrites, 128 is about half */
    uint32_t a[3] = { 0x00FF0000u, 0xFF00FF00u, 0x800000FFu };
    for (int x = 50; x < 53; x++) fb[60 * 640 + x] = 0xFFFFFF;
    rb_blit_rgb(50, 60, a, 3, 1, 3, 1, 0, 0, 640, 360);
    CHECK(px(50, 60) == 0xFFFFFF);
    CHECK(px(51, 60) == 0x00FF00);
    uint32_t h = px(52, 60);
    CHECK(RB_B(h) >= 250 && RB_R(h) >= 120 && RB_R(h) <= 135 && RB_G(h) == RB_R(h));
    /* a null or empty image is ignored */
    rb_blit_rgb(0, 0, 0, 3, 3, 3, 0, 0, 0, 640, 360);
    rb_blit_rgb(0, 0, img, 0, 3, 3, 0, 0, 0, 640, 360);
}

static void open_page(const char *html) {
    rb_set_viewport(rb_ui.view_w, rb_ui.content_h);
    rb_open_web(html, (int)strlen(html));
}

static void test_page(void) {
    rb_ui_layout(640, 360);
    const RbUi *u = &rb_ui;
    open_page("<body style=\"margin:0;background:#123456\">"
              "<div style=\"background:#abcdef;height:20px\"></div>"
              "<div style=\"background:#ff0000;height:20px;opacity:0.5\"></div>"
              "<div style=\"overflow:hidden;width:50px;height:20px\">"
              "<div id=wide style=\"background:#00ff00;width:400px;height:20px\"></div></div>"
              "<p style=\"color:#fe8000\">Hello</p></body>");
    memset(fb, 0, sizeof(fb));
    rb_paint_begin(fb, u->w, u->h);
    rb_paint_page(0, 0);
    int y0 = u->content_y;
    CHECK(px(u->view_x + 1, y0 + 1) == 0xabcdef);          /* 20 css px tall = 10 rows */
    CHECK(px(u->view_x + 600, y0 + 9) == 0xabcdef);         /* the full 622 px viewport */
    uint32_t half = px(u->view_x + 1, y0 + 11);             /* red at 50% over #123456 */
    CHECK(RB_R(half) > 0x80 && RB_R(half) < 0x90 && RB_B(half) > 0x25 && RB_B(half) < 0x30);
    CHECK(px(u->view_x + 10, y0 + 21) == 0x00ff00);         /* inside the clip */
    CHECK(px(u->view_x + 60, y0 + 21) == 0x123456);         /* clipped by overflow:hidden */
    int text = 0;
    for (int y = y0; y < u->status_y; y++)
        for (int x = 0; x < u->scroll_x; x++) if (px(x, y) == 0xfe8000) text++;
    CHECK(text > 10);
    CHECK(px(u->scroll_x, y0 + 1) == 0);                    /* the page never paints the scrollbar */

    /* rb_box_paint_rect reports the clipped bounds for the wide box */
    int wide = rb_find_id("wide"), found = 0;
    for (int i = 0; i < rb_box_count(); i++) {
        if (rb_box(i)->node != wide) continue;
        int x, y, x0, y0b, x1, y1;
        CHECK(rb_box_paint_rect(i, 0, &x, &y, &x0, &y0b, &x1, &y1));
        CHECK(x == u->view_x && x1 == u->view_x + 50 && y0b >= u->content_y && y1 <= u->status_y);
        found = 1;
    }
    CHECK(found);
    int d;
    CHECK(!rb_box_paint_rect(-1, 0, &d, &d, &d, &d, &d, &d));
    CHECK(!rb_box_paint_rect(rb_box_count(), 0, &d, &d, &d, &d, &d, &d));

    /* scrolled far past the page: only the background */
    memset(fb, 0, sizeof(fb));
    rb_paint_page(100000, 0);
    CHECK(px(u->view_x + 1, y0 + 1) == 0x123456);
    /* the chrome rows are left alone */
    CHECK(px(5, 0) == 0 && px(5, u->status_y) == 0);
    rb_set_viewport(RB_VIEW_W_DEFAULT, RB_VIEW_H_DEFAULT);
}

static void test_quantize(void) {
    rb_ui_layout(320, 200);
    uint32_t src[16 * 4];
    for (int i = 0; i < 16; i++) src[i] = rb_ui_palette[i < 15 ? i : 0];
    for (int i = 16; i < 32; i++) src[i] = 0xFF0000;            /* exactly on the cube */
    for (int i = 32; i < 48; i++) src[i] = 0x646464;            /* gray (100,100,100) */
    for (int i = 48; i < 64; i++) src[i] = 0x800000;            /* between two cube reds */
    rb_quantize_8bpp(src, fb8, 16, 4);
    for (int i = 0; i < 15; i++) CHECK(fb8[i] == i || rb_ui_palette[fb8[i]] == rb_ui_palette[i]);
    for (int i = 16; i < 32; i++) CHECK(fb8[i] == 16 + 5 * 36);
    for (int i = 32; i < 48; i++) CHECK(fb8[i] == 232 + 10);
    int lo = 0, hi = 0;
    for (int i = 48; i < 64; i++) {
        if (fb8[i] == 16 + 2 * 36) lo++;
        else if (fb8[i] == 16 + 3 * 36) hi++;
    }
    CHECK(lo > 0 && hi > 0 && lo + hi == 16);              /* ordered dither between 102 and 153 */
}

int main(void) {
    test_layout();
    test_rect_and_text();
    test_blit();
    test_page();
    test_quantize();
    if (fails) {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    printf("rbrowse paint checks passed\n");
    return 0;
}
