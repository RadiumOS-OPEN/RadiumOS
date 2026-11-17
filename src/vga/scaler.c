// scaler.c - see scaler.h
#include "scaler.h"
#include "../terminal/terminal.h"

static scaler_info_t g_info = {
    80, 50, 640, 400, 80, 50, 8, 8, 0, 0
};
static uint32_t g_generation = 1;

static int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void scaler_refresh(void)
{
    scaler_info_t n;
    n.term_cols = clamp((int)terminal_get_width(), 20, 400);
    n.term_rows = clamp((int)terminal_get_height(), 10, 200);
    n.px_w = n.term_cols * 8;
    n.px_h = n.term_rows * 8;

    n.win_cols = n.term_cols < SCALER_WIN_COLS_MAX ? n.term_cols : SCALER_WIN_COLS_MAX;
    n.win_rows = n.term_rows < SCALER_WIN_ROWS_MAX ? n.term_rows : SCALER_WIN_ROWS_MAX;

    n.cell_w = n.px_w / n.win_cols;
    n.cell_h = n.px_h / n.win_rows;
    if (n.cell_w < 1) n.cell_w = 1;
    if (n.cell_h < 1) n.cell_h = 1;
    n.left = (n.px_w - n.win_cols * n.cell_w) / 2;
    n.top  = (n.px_h - n.win_rows * n.cell_h) / 2;

    if (n.term_cols != g_info.term_cols || n.term_rows != g_info.term_rows || n.cell_w != g_info.cell_w ||
        n.cell_h != g_info.cell_h || n.win_cols != g_info.win_cols || n.win_rows != g_info.win_rows)
        g_generation++;
    g_info = n;
}

const scaler_info_t *scaler_info(void) { return &g_info; }
uint32_t scaler_generation(void) { return g_generation; }

int scaler_term_cols(void) { return g_info.term_cols; }
int scaler_term_rows(void) { return g_info.term_rows; }
int scaler_win_cols(void)  { return g_info.win_cols; }
int scaler_win_rows(void)  { return g_info.win_rows; }

int scaler_x(int ref_x) { return ref_x * g_info.px_w / SCALER_REF_W; }
int scaler_y(int ref_y) { return ref_y * g_info.px_h / SCALER_REF_H; }
int scaler_right(int ref_margin)  { return g_info.px_w - scaler_x(ref_margin); }
int scaler_bottom(int ref_margin) { return g_info.px_h - scaler_y(ref_margin); }

int scaler_win_pct_cols(int pct, int min)
{
    int v = g_info.win_cols * pct / 100;
    return v < min ? min : v;
}

int scaler_win_pct_rows(int pct, int min)
{
    int v = g_info.win_rows * pct / 100;
    return v < min ? min : v;
}

void scaler_fit_window(int *w, int *h)
{
    if (w) { int v = *w * g_info.win_cols / 80; *w = clamp(v, *w, g_info.win_cols); }
    if (h) { int v = *h * g_info.win_rows / 25; *h = clamp(v, *h, g_info.win_rows); }
}
