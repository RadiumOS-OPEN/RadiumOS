#ifndef RBROWSE_CANVAS_H
#define RBROWSE_CANVAS_H

/* rbrowse <canvas>: the per-page canvas table, its pixel memory and the 2D
   rasterizer. The JS side (src/quickjs/rjs_canvas.c) keeps the context state
   (transform, styles, the current path), flattens paths and strokes into
   polygons in device space and hands them to the functions below; the
   painter shows a canvas through rb_canvas_view_node (paint.h's canvas
   hook). Shared by the kernel and the host tools.

   Pixels are 0xAARRGGBB with straight (not premultiplied) alpha, the same
   format as decoded images, row stride = width. A new canvas, and one whose
   size is set again, is transparent black.

   Memory never comes from a general heap: every canvas of the page lives in
   one static arena (RB_CANVAS_ARENA), compacted when a resize does not fit
   at the top, and reset with the page (rb_canvas_reset). A canvas larger than
   RB_CANVAS_DIM_MAX on a side, or one that does not fit in what the page's
   other canvases leave, keeps its size but has no pixels: drawing on it does
   nothing and it paints as transparent. */

#include <stdint.h>
#include "image.h"     /* RbImageView */

#define RB_CANVAS_MAX 32                   /* canvases with a context per page */
#define RB_CANVAS_DIM_MAX 2048             /* width and height each */
#define RB_CANVAS_ARENA (24u * 1024 * 1024) /* pixel memory of all of a page's canvases */
#define RB_CANVAS_EDGES_MAX 65536          /* edges in one fill */

/* New page: forget every canvas and empty the arena. */
void rb_canvas_reset(void);
/* The slot of the canvas element `node` (made on first use; the slot of a
   node that has since died is reused), or -1 when the table is full. A new
   slot has no pixels until rb_canvas_resize. */
int rb_canvas_open(int node);
/* The live slot of node, or -1. */
int rb_canvas_find(int node);
/* Set the size (clamped to 0..65535) and clear to transparent black.
   Returns 1 when the canvas has pixels, 0 when it is empty (a side is 0),
   too big, or the arena is full. */
int rb_canvas_resize(int slot, int w, int h);
/* Size of a slot (0 for a bad one); returns 1 when it has pixels. */
int rb_canvas_size(int slot, int *w, int *h);
/* The pixels (w * h, stride w), or 0 when it has none. Valid until the next
   rb_canvas_resize of ANY slot (a resize may compact the arena). */
uint32_t *rb_canvas_pixels(int slot);
/* Arena bytes in use (tests, status). */
unsigned rb_canvas_used(void);
/* 1 when any canvas changed since the last call (the host repaints). */
int rb_canvas_take_dirty(void);
/* The screen shows a copy of each canvas (rb_canvas_view_node), taken by
   rb_canvas_present once the drawing of a frame is complete, so a frame
   being rasterized is never seen half done. Returns how many canvases it
   copied. */
int rb_canvas_present(void);
/* Per canvas: 1 when the shown copy of the canvas element node changed
   since the last rb_canvas_clean (the host repaints just those boxes). */
int rb_canvas_node_dirty(int node);
void rb_canvas_clean(void);

/* Painter hook (paint.h rb_paint_set_canvas): the shown copy of the canvas
   element `node` at its own size (the painter scales to the box). */
int rb_canvas_view_node(int node, int w, int h, RbImageView *out);

/* ---- rasterizer ----
   A fill is a set of line segments in device pixels (x right, y down, pixel
   (i, j) covers [i, i+1) x [j, j+1)), collected with rb_cv_line after
   rb_cv_begin and drawn by rb_cv_fill with the nonzero or even-odd rule.
   Coverage is antialiased: 4 sub-scanlines per row, exact horizontal
   coverage. Closing each polygon is the caller's job. */
void rb_cv_begin(void);
/* Add one segment. Returns 0 when the edge table is full (the segment is
   dropped; a stroker flushes with rb_cv_fill and starts again). */
int rb_cv_line(double x0, double y0, double x1, double y1);
/* Segments collected so far. */
int rb_cv_edges(void);

/* Compositing (globalCompositeOperation). CLEAR is clearRect: the covered
   pixels lose their alpha in proportion to the coverage. */
enum { RB_CV_OVER = 0, RB_CV_COPY, RB_CV_DEST_OUT, RB_CV_LIGHTER, RB_CV_CLEAR };

/* What a fill paints. Solid: argb (straight alpha). Linear gradient: the
   colour of the pixel whose centre is (cx, cy) = (X + .5, Y + .5) is
   lut[255 * t] with t = gx * cx + gy * cy + g0 clamped to 0..1. Radial
   gradient (two circles, as in the 2D canvas spec): the pixel centre maps
   to user space through inv (a b c d e f: x' = a x + c y + e,
   y' = b x + d y + f) and t solves
   |p - c(t)| = r(t) for c(t) = (x0,y0) + t ((x1,y1) - (x0,y0)), r(t) = r0 +
   t (r1 - r0); pixels with no solution stay untouched. */
enum { RB_CV_SOLID = 0, RB_CV_LINEAR, RB_CV_RADIAL };
typedef struct {
    int kind;
    uint32_t argb;
    const uint32_t *lut;          /* 256 entries, straight ARGB */
    double gx, gy, g0;            /* linear */
    double inv[6];                /* radial: device -> user */
    double x0, y0, r0, x1, y1, r1;
} RbCvPaint;

/* Clip rectangle in device pixels, half-open [x0,x1) x [y0,y1). */
typedef struct { int x0, y0, x1, y1; } RbCvClip;

/* Long fills and image draws poll fn every few rows and stop when it
   returns 1 (the script that asked has run out of time). 0 = never stop. */
void rb_cv_set_abort(int (*fn)(void));

/* Fill the collected segments into slot with paint p, times alpha
   (0..255, globalAlpha), composited with op, inside clip (0 = the whole
   canvas). Leaves the segments in place (rb_cv_begin clears them). */
void rb_cv_fill(int slot, int evenodd, const RbCvPaint *p, int alpha, int op, const RbCvClip *clip);

/* 1 when device point (x, y) is inside the collected segments under the
   rule (isPointInPath). */
int rb_cv_inside(double x, double y, int evenodd);

/* drawImage: source pixels src (sw x sh, stride, straight ARGB) are mapped
   into slot by the affine inv (device pixel centre -> source coordinates,
   same layout as RbCvPaint.inv). Only canvas pixels inside the device
   bounding box [bx0,bx1) x [by0,by1) (clamped to the canvas and clip) whose
   source coordinates fall inside [u0,u1) x [v0,v1) are drawn, nearest
   neighbour, or bilinear when smooth. src may be the slot's own pixels:
   they are copied first (up to 1024 x 1024 source pixels, else nothing is
   drawn). */
void rb_cv_image(int slot, const uint32_t *src, int sw, int sh, int stride,
                 double u0, double v0, double u1, double v1, const double inv[6],
                 int bx0, int by0, int bx1, int by1, int alpha, int op, int smooth, const RbCvClip *clip);

/* getImageData: the rectangle (x, y, w, h) as straight RGBA bytes (w * h *
   4); pixels outside the canvas read as 0. */
void rb_cv_get(int slot, int x, int y, int w, int h, uint8_t *rgba);
/* putImageData: write the part [x0,x1) x [y0,y1) of an iw x ih RGBA image
   with its top-left at canvas (dx, dy), replacing the pixels (no blending,
   no transform). */
void rb_cv_put(int slot, const uint8_t *rgba, int iw, int ih, int dx, int dy, int x0, int y0, int x1, int y1);

/* toDataURL: the canvas as a PNG (8-bit RGBA, zlib "stored" blocks, so no
   compression). Returns the byte count, or -1 when it has no pixels or the
   PNG would not fit in cap. */
int rb_cv_png(int slot, uint8_t *out, int cap);

#endif
