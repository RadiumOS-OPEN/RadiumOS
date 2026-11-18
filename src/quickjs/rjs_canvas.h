/* rjs_canvas.h - the host side of the <canvas> 2D context (rjs_canvas.c).
 *
 * Drawing is queued: scripts record commands and return, and the host
 * rasterizes them between input polls, so an expensive canvas slows only
 * its own frames. Reading pixels or resizing a canvas flushes the queue
 * first, so scripts never see the difference.
 */
#ifndef RJS_CANVAS_H
#define RJS_CANVAS_H

/* Rasterize queued drawing for about budget_ms (in bands of rows). Returns
   1 while work is left. */
int rjs_canvas_pump(int budget_ms);
/* Rasterize everything queued now. */
void rjs_canvas_flush(void);
/* 1 while drawing is queued. */
int rjs_canvas_pending(void);
/* Forget everything queued (a new page; the canvases are gone). */
void rjs_canvas_discard(void);

#endif
