/* A small curses for RadiumOS apps: just what Cataclysm: DDA's ncurses backend and ImTui use. The screen is a grid
 * of cells that radium_curses.cpp blits to the kernel's text console (CP437 glyphs, 8 colours + bold). */
#ifndef RADIUM_CURSES_H
#define RADIUM_CURSES_H

#include <stdarg.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct radium_win WINDOW;
typedef unsigned int chtype;
typedef chtype attr_t;
typedef unsigned char bool_t_unused;

#define OK  0
#define ERR (-1)

#define COLOR_BLACK   0
#define COLOR_RED     1
#define COLOR_GREEN   2
#define COLOR_YELLOW  3
#define COLOR_BLUE    4
#define COLOR_MAGENTA 5
#define COLOR_CYAN    6
#define COLOR_WHITE   7

/* attribute bits: colour pair in bits 17..25 (what CDDA's nc_color expects), flags above that */
#define A_COLOR      0x03FE0000u
#define COLOR_PAIR(n) ((((unsigned)(n)) & 0x1FFu) << 17)
#define PAIR_NUMBER(a) (((a) & A_COLOR) >> 17)
#define A_NORMAL     0u
#define A_ALTCHARSET 0x00400000u
#define A_BOLD       0x04000000u
#define A_BLINK      0x08000000u
#define A_REVERSE    0x10000000u
#define A_CHARTEXT   0x000000FFu

#ifndef KEY_DOWN
#define KEY_DOWN      0402
#define KEY_UP        0403
#define KEY_LEFT      0404
#define KEY_RIGHT     0405
#define KEY_HOME      0406
#define KEY_BACKSPACE 0407
#define KEY_F0        0410
#define KEY_F(n)      (KEY_F0 + (n))
#define KEY_DC        0512
#define KEY_IC        0513
#define KEY_NPAGE     0522
#define KEY_PPAGE     0523
#define KEY_ENTER     0527
#define KEY_END       0550
#define KEY_MOUSE     0631
#define KEY_SF 0541
#define KEY_SR 0542
#define KEY_PRINT 0532
#define KEY_A1 0534
#define KEY_A3 0535
#define KEY_B2 0536
#define KEY_C1 0537
#define KEY_C3 0540
#define KEY_SDC 0577
#define KEY_SEND 0602
#define KEY_SHOME 0605
#define KEY_SIC 0606
#define KEY_SLEFT 0611
#define KEY_SPRINT 0613
#define KEY_SRIGHT 0622
#define KEY_RESIZE    0632
#endif

/* ACS codes are 'x' etc. with A_ALTCHARSET, as ncurses does (CDDA's LINE_XOXO is 0x400000 | 'x') */
#define ACS_VLINE    (A_ALTCHARSET | 'x')
#define ACS_HLINE    (A_ALTCHARSET | 'q')
#define ACS_ULCORNER (A_ALTCHARSET | 'l')
#define ACS_URCORNER (A_ALTCHARSET | 'k')
#define ACS_LLCORNER (A_ALTCHARSET | 'm')
#define ACS_LRCORNER (A_ALTCHARSET | 'j')

#define BUTTON1_PRESSED  0x2u
#define BUTTON1_RELEASED 0x1u
#define BUTTON1_CLICKED  0x4u
#define BUTTON3_PRESSED  0x80u
#define BUTTON3_RELEASED 0x40u
#define BUTTON3_CLICKED  0x100u
#define ALL_MOUSE_EVENTS 0x0FFFFFFFu
#define REPORT_MOUSE_POSITION 0x10000000u
typedef unsigned int mmask_t;
typedef struct { short id; int x, y, z; mmask_t bstate; } MEVENT;

extern WINDOW *stdscr;
extern WINDOW *curscr;
extern WINDOW *newscr;
extern int COLORS, COLOR_PAIRS, LINES, COLS;

WINDOW *initscr(void);
int endwin(void);
int start_color(void);
int init_pair(short pair, short fg, short bg);
int noecho(void);
int cbreak(void);
int keypad(WINDOW *win, int on);
int set_escdelay(int ms);
int curs_set(int visibility);
int mouseinterval(int ms);
mmask_t mousemask(mmask_t newmask, mmask_t *oldmask);
int getmouse(MEVENT *event);
int is_term_resized(int lines, int cols);
int timeout(int delay);
int wtimeout(WINDOW *win, int delay);
int getch(void);
int wgetch(WINDOW *win);
int ungetch(int ch);

WINDOW *newwin(int lines, int cols, int begin_y, int begin_x);
int delwin(WINDOW *win);
int werase(WINDOW *win);
int wclear(WINDOW *win);
int erase(void);
int clear(void);
int wmove(WINDOW *win, int y, int x);
int waddch(WINDOW *win, chtype ch);
int mvwaddch(WINDOW *win, int y, int x, chtype ch);
int wprintw(WINDOW *win, const char *fmt, ...);
int mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...);
int wattron(WINDOW *win, int attrs);
int wattroff(WINDOW *win, int attrs);
int wborder(WINDOW *win, chtype ls, chtype rs, chtype ts, chtype bs, chtype tl, chtype tr, chtype bl, chtype br);
int mvwhline(WINDOW *win, int y, int x, chtype ch, int n);
int mvwvline(WINDOW *win, int y, int x, chtype ch, int n);
int wredrawln(WINDOW *win, int beg_line, int num_lines);
int wnoutrefresh(WINDOW *win);
int wrefresh(WINDOW *win);
int refresh(void);
int doupdate(void);
int getmaxx(const WINDOW *win);
int getmaxy(const WINDOW *win);
int getbegx(const WINDOW *win);
int getbegy(const WINDOW *win);
int getcurx(const WINDOW *win);
int getcury(const WINDOW *win);
int waddwstr(WINDOW *win, const wchar_t *str);
#define getmaxyx(w, y, x) ((y) = getmaxy(w), (x) = getmaxx(w))
#define getyx(w, y, x) ((y) = getcury(w), (x) = getcurx(w))

#ifdef __cplusplus
}
#endif

#endif
