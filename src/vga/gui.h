// gui.h - a small pixel toolkit for programs that draw their own 1280x720 picture (the Discord client)
//
// Everything is drawn into a back buffer and copied to the screen by gui_present(). Colours are 0xRRGGBB. All
// drawing honours the clip rectangle set by gui_clip().
#ifndef GUI_TOOLKIT_H
#define GUI_TOOLKIT_H

#include <stdint.h>
#include <stdbool.h>

#define GUI_W 1280
#define GUI_H 720

typedef struct {
    const uint32_t *px;   // 0xAARRGGBB
    int w, h;
} gui_image_t;

// Takes the display from the console and clears the back buffer; false if there is no 1280x720 console.
bool gui_begin(void);
// Gives the display back to the console.
void gui_end(void);
void gui_present(void);

void gui_clip(int x, int y, int w, int h);
void gui_unclip(void);

void gui_fill(int x, int y, int w, int h, uint32_t rgb);
// Filled rounded rectangle; r = min(w, h) / 2 makes a pill or a circle.
void gui_round(int x, int y, int w, int h, int r, uint32_t rgb);
void gui_circle(int cx, int cy, int radius, uint32_t rgb);
// Draws the 8x8 font in size x size pixel cells (8 and 16 are crisp, other sizes are scaled); bold adds a one-pixel
// smear. Returns the x after the text.
int  gui_text(int x, int y, const char *s, uint32_t rgb, int size, bool bold);
int  gui_text_width(const char *s, int size);
// Draws the image stretched to w x h with rounded corners (radius 0 = square, w/2 = circle).
void gui_image(const gui_image_t *img, int x, int y, int w, int h, int radius);

#endif // GUI_H
