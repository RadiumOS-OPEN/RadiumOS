#ifndef RBROWSE_PAINT_H
#define RBROWSE_PAINT_H

#include <stdint.h>
#include "image.h"

/* The painter draws into a 32-bit back buffer (0x00RRGGBB per pixel, stride
   = width) of any size up to RB_FB_MAX_W x RB_FB_MAX_H. The host presents it:
   straight to a 32bpp linear framebuffer, or through rb_quantize_8bpp for
   VGA mode 13h. */
#define RB_FB_MAX_W 640
#define RB_FB_MAX_H 360

/* Screen layout, derived from the framebuffer size by rb_ui_layout(). Every
   coordinate is in framebuffer pixels. */
typedef struct {
    int w, h;                 /* framebuffer */
    int chrome_h;             /* address bar strip at the top */
    int content_y, content_h; /* page area: rows content_y .. status_y-1 */
    int status_y, status_h;   /* status line at the bottom */
    int scroll_x, scroll_w;   /* scrollbar column, right of the page */
    int view_x;               /* left inset of the page */
    int view_w;               /* page viewport width (rb_set_viewport) */
    /* chrome widgets: back button, address field, Go button */
    int btn_y, btn_h, text_y;
    int back_x, back_w;
    int url_x, url_w;
    int go_x, go_w;
    int glyph;                /* chrome/banner text scale (1 or 2) */
} RbUi;

extern RbUi rb_ui;
void rb_ui_layout(int w, int h);

/* Chrome colours (0xRRGGBB). In mode 13h they are palette entries 0..15:
   rb_ui_palette[i] is the RGB of entry i, and the quantizer maps these exact
   values back to their entries. */
#define UI_BLACK        0x000000u
#define UI_CHROME       0x1A1008u
#define UI_LINE         0x7A3010u
#define UI_URL          0x1E1008u
#define UI_URL_TEXT     0xFFDDBBu
#define UI_BTN          0x5C2200u
#define UI_BTN_TEXT     0xFFCCAAu
#define UI_STATUS       0x0F0804u
#define UI_STATUS_TEXT  0x996644u
#define UI_FOCUS        0xFFB428u
#define UI_SCROLL       0x1A0E06u
#define UI_THUMB        0x7A3A10u
#define UI_WHITE        0xFFFFFFu
extern const uint32_t rb_ui_palette[16];

void rb_paint_begin(uint32_t *fb, int w, int h);
void rb_px(int x, int y, uint32_t c);
/* Fill, clipped to [x0,x1) x [y0,y1) and the framebuffer. */
void rb_rect(int x, int y, int w, int h, uint32_t c, int x0, int y0, int x1, int y1);
void rb_frame_rect(int x, int y, int w, int h, uint32_t c);
void rb_text_at(int x, int y, const char *s, uint32_t color, int x0, int y0, int x1, int y1);
void rb_text_scaled(int x, int y, const char *s, uint32_t color, int scale);

/* Blit an RGB image: src holds sh rows of sw pixels (row stride src_stride
   pixels), drawn 1:1 with its top-left at (x, y), clipped to [x0,x1) x
   [y0,y1) and the framebuffer. Pixels are 0x00RRGGBB, or 0xAARRGGBB with
   straight alpha when has_alpha is set (alpha 0 skips the pixel, 255
   overwrites). For a page image, pass the box's paint bounds from
   rb_box_paint_rect so it is clipped to the content area and the box's
   overflow clip. */
void rb_blit_rgb(int x, int y, const uint32_t *src, int sw, int sh, int src_stride,
                 int has_alpha, int x0, int y0, int x1, int y1);

/* Images. The painter draws an RB_BOX_IMAGE box through this hook: given
   the <img> node (the box's toff) and the box size, it fills a view of the
   pixels (image.c's rb_image_view_node). A view of exactly the box size is
   blitted 1:1; any other size is sampled nearest-neighbour. With no hook, or
   while the image has not decoded, the box shows its placeholder fill (only
   when at least 12x12, so a loading icon does not flash a grey square) or,
   with RB_BOX_IMGBG, the element's background. */
typedef int (*RbImageHook)(int node, int w, int h, RbImageView *out);
void rb_paint_set_images(RbImageHook fn);
/* Canvases. An RB_BOX_CANVAS box (toff = the <canvas> node) is painted as
   its background (with RB_BOX_IMGBG) and then the pixels the hook returns
   (canvas.c's rb_canvas_view_node: the buffer at its own size, straight
   alpha), 1:1 when the box has the buffer's size, nearest-neighbour
   otherwise. No hook or no pixels: only the background. */
void rb_paint_set_canvas(RbImageHook fn);

/* Where box i lands on screen with the page scrolled by `scroll`: its
   top-left (x, y) and the paint bounds [x0,x1) x [y0,y1) (the content area
   narrowed by the box's overflow clip). Returns 0 for a bad index. */
int rb_box_paint_rect(int i, int scroll, int *x, int *y, int *x0, int *y0, int *x1, int *y1);

/* The page: background, every box in paint order, then the focus ring around
   the focused node. `scroll` is the document-space y at the top of the
   viewport. */
void rb_paint_page(int scroll, int focus_node);

/* 32-bit back buffer -> 8-bit mode 13h indices: chrome colours map to their
   entries, grays to the gray ramp, everything else to the 6x6x6 cube with a
   4x4 ordered dither. */
void rb_quantize_8bpp(const uint32_t *src, uint8_t *dst, int w, int h);

#endif
