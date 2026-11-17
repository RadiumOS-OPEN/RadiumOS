// scaler.h - dynamic resolution scaler
//
// Every size the graphics code needs is derived from the terminal grid that terminal.c reports
// (terminal_width x terminal_height cells of 8x8 pixels), so nothing has to hard-code 1280x720 or 80x25.
//
//   term_*  the terminal's own grid, drawn 1:1.
//   win_*   the grid windows (vga.c) and programs that write 0xB8000 draw on: at most 80x50. On a
//           wide terminal each of its cells is stretched to cell_w x cell_h pixels so it fills the
//           screen; on an 80x50 terminal cells are 8x8 pixels.
#ifndef SCALER_H
#define SCALER_H

#include <stdint.h>

// The layout most pixel positions in the code were written for.
#define SCALER_REF_W 1280
#define SCALER_REF_H 720

// Largest window grid (the 0xB8000 text window holds 80x50 cells).
#define SCALER_WIN_COLS_MAX 80
#define SCALER_WIN_ROWS_MAX 50

typedef struct {
    int term_cols, term_rows;   // terminal.c grid
    int px_w, px_h;             // pixels the terminal grid covers
    int win_cols, win_rows;     // grid for windows and 0xB8000 programs
    int cell_w, cell_h;         // pixels per window cell
    int left, top;              // offset of the window grid on screen (letterbox)
} scaler_info_t;

// Re-reads the terminal size. terminal.c calls this whenever the grid changes.
void scaler_refresh(void);
const scaler_info_t *scaler_info(void);
// Bumped by every refresh that changed something, so a renderer knows to redraw from scratch.
uint32_t scaler_generation(void);

int scaler_term_cols(void);
int scaler_term_rows(void);
int scaler_win_cols(void);
int scaler_win_rows(void);

// Reference pixel -> current pixel. scaler_right/scaler_bottom keep a margin from the right/bottom edge.
int scaler_x(int ref_x);
int scaler_y(int ref_y);
int scaler_right(int ref_margin);
int scaler_bottom(int ref_margin);

// pct percent of the window grid, never less than min.
int scaler_win_pct_cols(int pct, int min);
int scaler_win_pct_rows(int pct, int min);

// Grows a window designed for the old 80x25 screen so it uses the same share of the current window grid.
void scaler_fit_window(int *w, int *h);

#endif // SCALER_H
