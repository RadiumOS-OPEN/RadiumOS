// fbcon.h - 1280x720x32 framebuffer console
//
// The shell terminal is a native 160x90 grid of 8x8-pixel cells drawn 1:1 into a 1280x720 linear
// framebuffer (fbcon_text_buffer()). Programs that still write an 80-column text screen straight to
// 0xB8000 keep working: that address is remapped onto a RAM buffer, and while such a program is
// painting, the console shows that buffer (scaled to 16x14-pixel cells) instead of the terminal.
#ifndef FBCON_H
#define FBCON_H

#include <stdint.h>
#include <stdbool.h>

#define FBCON_WIDTH  1280
#define FBCON_HEIGHT 720
#define FBCON_COLS   160
#define FBCON_ROWS   90

// Sets the 1280x720x32 mode. False (text mode untouched) if the VBE display is unavailable.
bool fbcon_init(void);
// The console was set up (it may be suspended while a graphics program owns the display).
bool fbcon_enabled(void);
// The console currently owns the display.
bool fbcon_active(void);

// The terminal's cell buffer: FBCON_COLS x FBCON_ROWS entries of (attribute << 8) | character.
uint16_t *fbcon_text_buffer(void);

// Draws changed cells now / at most about every 12 ms / from the timer interrupt.
void fbcon_flush(void);
void fbcon_poll(void);
// Called at the start of every fbcon_poll() (programs call it from their key loops); the window manager uses it to
// keep drawing while a command runs. NULL removes the hook.
void fbcon_set_poll_hook(void (*fn)(void));
void fbcon_tick(void);

// Text cursor position in the terminal buffer (row * FBCON_COLS + column).
void fbcon_set_cursor(uint32_t cell);

// A graphics program takes over the display. The _hw variant also turns the VBE mode off (for VGA register modes).
void fbcon_suspend(void);
void fbcon_suspend_hw(void);
// Takes the display back: sets 1280x720 again and repaints the whole console.
void fbcon_resume(void);

// Copies a w x h block of 0x00RRGGBB pixels (stride = pixels per source row) to the screen at x, y. For programs
// that drew their own picture after fbcon_suspend(); does nothing while the console is active.
void fbcon_blit(const uint32_t *src, int stride, int x, int y, int w, int h);

// While a redirect is set, fbcon_blit() hands the pixels to fn instead of the screen and fbcon_suspend()/fbcon_resume() do
// nothing: a program that thinks it owns the display draws into a desktop window instead.
typedef void (*fbcon_redirect_fn)(const uint32_t *src, int stride, int x, int y, int w, int h);
void fbcon_set_redirect(fbcon_redirect_fn fn);
fbcon_redirect_fn fbcon_get_redirect(void);

// ---- Overlays: animated widgets pinned to a fixed screen rectangle --------------------------------
//
// An overlay is drawn on top of the terminal about 30 times a second, from the same timer/keyboard-poll
// refresh as the text. It keeps moving no matter what the terminal does (scrolling, clearing, paging).
// The callback draws into a w x h canvas that already holds the terminal pixels behind it.
typedef struct {
    uint32_t *px;
    int w, h;
} fbcon_canvas_t;

typedef void (*fbcon_overlay_fn)(fbcon_canvas_t *canvas, uint32_t t_ms, void *arg);

#define FBCON_OVERLAY_MAX      8
#define FBCON_OVERLAY_MAX_PIXELS (400 * 130)

// Adds (or replaces, by name) an overlay. Returns 0, or -1 if there is no console, no room or it is too big.
int  fbcon_overlay_add(const char *name, int x, int y, int w, int h, fbcon_overlay_fn fn, void *arg);
// Hides the overlays (they come back when resumed); used by programs that own the whole screen.
void fbcon_overlays_pause(bool paused);

bool fbcon_overlay_remove(const char *name);
void fbcon_overlay_clear(void);
// n-th active overlay name (NULL past the end) and its rectangle.
const char *fbcon_overlay_at(int n, int *x, int *y, int *w, int *h);
// Milliseconds since the console started, the clock overlays animate against.
uint32_t fbcon_time_ms(void);

// Canvas drawing. Colors are 0xRRGGBB; alpha is 0 (transparent) .. 255 (opaque).
void fbc_pixel(fbcon_canvas_t *c, int x, int y, uint32_t color, uint8_t alpha);
void fbc_rect(fbcon_canvas_t *c, int x, int y, int w, int h, uint32_t color, uint8_t alpha);
// Draws text with the 8x8 font at integer scale; returns the width drawn in pixels.
int  fbc_text(fbcon_canvas_t *c, int x, int y, const char *text, uint32_t color, int scale, uint8_t alpha);
void fbc_glyph(fbcon_canvas_t *c, int x, int y, uint8_t ch, uint32_t color, int scale, uint8_t alpha);

#endif // FBCON_H
