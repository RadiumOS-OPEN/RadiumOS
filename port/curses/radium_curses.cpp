// radium_curses.cpp - the curses.h subset used by Cataclysm: DDA, drawn on the RadiumOS text console
#include "curses.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include "../rt/radium_api.h"

extern "C" const radium_api_t *radium_api(void);

extern "C" {
WINDOW *stdscr, *curscr, *newscr;
int COLORS = 8, COLOR_PAIRS = 512, LINES = 90, COLS = 160;
}

namespace
{
struct cell { uint32_t ch = ' '; uint16_t attr = 0; };   // attr: pair (9 bits) | flags >> 17 (see pack())

short pair_fg[512], pair_bg[512];
std::vector<cell> vscr;
int g_cols = 160, g_rows = 90;
int g_delay = -1;
int g_ungot[8], g_nungot = 0;

// ---- Unicode <-> the console's CP437 glyphs ------------------------------------------------------------------

uint16_t cp437[256];
uint8_t rev[0x2600];

void init_cp437()
{
    static const uint16_t low[32] = {0x0020,0x263A,0x263B,0x2665,0x2666,0x2663,0x2660,0x2022,0x25D8,0x25CB,0x25D9,0x2642,0x2640,0x266A,0x266B,0x263C,
                                     0x25BA,0x25C4,0x2195,0x203C,0x00B6,0x00A7,0x25AC,0x21A8,0x2191,0x2193,0x2192,0x2190,0x221F,0x2194,0x25B2,0x25BC};
    static const uint16_t high[129] = {0x2302,
        0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
        0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
        0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
        0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
        0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
        0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
        0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
        0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0};
    for (int i = 0; i < 32; i++) cp437[i] = low[i];
    for (int i = 32; i < 127; i++) cp437[i] = (uint16_t)i;
    for (int i = 127; i < 256; i++) cp437[i] = high[i - 127];
    cp437[0] = 0x0020;
    for (int i = 255; i >= 1; i--) if (cp437[i] < 0x2600) rev[cp437[i]] = (uint8_t)i;
    for (int i = 32; i < 127; i++) rev[i] = (uint8_t)i;
    rev[0x20] = 0x20;
}

uint8_t glyph_for(uint32_t cp)
{
    if (cp < 0x2600) {
        if (rev[cp]) return rev[cp];
        if (cp == 0) return ' ';
        if (cp >= 0x2500 && cp < 0x2580) return 0xC5;           // heavy/dashed lines: nearest plain box piece
        if (cp >= 0xC0 && cp < 0x100) {                          // accented Latin: drop the accent
            static const char *base = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
            return (uint8_t)base[cp - 0xC0];
        }
        return '?';
    }
    for (int i = 1; i < 256; i++) if (cp437[i] == cp) return (uint8_t)i;
    return '?';
}

uint32_t utf8_next(const char *&s)
{
    unsigned char c = (unsigned char)*s++;
    if (c < 0x80) return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = n == 3 ? (c & 7u) : n == 2 ? (c & 15u) : n == 1 ? (c & 31u) : 0xFFFDu;
    while (n-- > 0 && (*s & 0xC0) == 0x80) cp = (cp << 6) | ((unsigned char)*s++ & 63u);
    return cp;
}

uint32_t acs_to_unicode(char c)
{
    switch (c) {
    case 'x': return 0x2502; case 'q': return 0x2500; case 'm': return 0x2514; case 'l': return 0x250C;
    case 'k': return 0x2510; case 'j': return 0x2518; case 't': return 0x251C; case 'v': return 0x2534;
    case 'u': return 0x2524; case 'w': return 0x252C; case 'n': return 0x253C; case 'a': return 0x2592;
    case '`': return 0x2666; case 'f': return 0xB0; case 'g': return 0xB1; case '~': return 0xB7;
    default: return (uint32_t)(unsigned char)c;
    }
}
}

struct radium_win {
    int y, x, h, w, cy, cx;
    unsigned attrs;                    // A_* flags and pair from wattron
    std::vector<cell> cells;
    bool is_stdscr;
};

namespace
{
uint16_t pack(unsigned attrs)
{
    unsigned pair = (attrs & A_COLOR) >> 17;
    unsigned flags = ((attrs & A_BOLD) ? 1u : 0u) | ((attrs & A_BLINK) ? 2u : 0u) | ((attrs & A_REVERSE) ? 4u : 0u);
    return (uint16_t)(pair | (flags << 9));
}

void put(WINDOW *w, uint32_t cp)
{
    if (cp == '\n') { w->cx = 0; if (w->cy + 1 < w->h) w->cy++; return; }
    if (cp == '\r') { w->cx = 0; return; }
    if (cp == '\t') { int n = 8 - (w->cx % 8); while (n-- > 0 && w->cx < w->w) { cell &c = w->cells[(size_t)w->cy * w->w + w->cx++]; c.ch = ' '; c.attr = pack(w->attrs); } return; }
    if (w->cx >= w->w) { w->cx = 0; if (w->cy + 1 < w->h) w->cy++; else return; }
    cell &c = w->cells[(size_t)w->cy * w->w + w->cx++];
    c.ch = cp;
    c.attr = pack(w->attrs);
}

void put_chtype(WINDOW *w, chtype ch)
{
    uint32_t cp = ch & 0x1FFFFFu;
    if (ch & A_ALTCHARSET) cp = acs_to_unicode((char)(ch & 0xFF));
    put(w, cp ? cp : ' ');
}

void blit()
{
    const radium_api_t *A = radium_api();
    static std::vector<uint16_t> out;
    out.resize((size_t)g_cols * g_rows);
    static const uint8_t vga[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    for (size_t i = 0; i < out.size(); i++) {
        const cell &c = vscr[i];
        unsigned pair = c.attr & 0x1FF, flags = c.attr >> 9;
        unsigned fg = vga[pair_fg[pair] & 7], bg = vga[pair_bg[pair] & 7];
        if (pair == 0) { fg = 7; bg = 0; }
        if (flags & 1) fg |= 8;
        if (flags & 4) { unsigned t = fg & 7; fg = bg | (fg & 8); bg = t; }
        out[i] = (uint16_t)(glyph_for(c.ch) | ((fg | (bg << 4)) << 8));
    }
    A->screen_blit(out.data(), (uint32_t)g_cols, (uint32_t)g_rows);
}

// ---- keyboard --------------------------------------------------------------------------------------------------

// US layout, scancode set 1: unshifted and shifted characters for 0x00..0x39
const char NORMAL[0x3A] = {0,27,'1','2','3','4','5','6','7','8','9','0','-','=',8,9,'q','w','e','r','t','y','u','i','o','p','[',']',10,0,
                           'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' '};
const char SHIFTED[0x3A] = {0,27,'!','@','#','$','%','^','&','*','(',')','_','+',8,9,'Q','W','E','R','T','Y','U','I','O','P','{','}',10,0,
                            'A','S','D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '};
bool k_shift, k_ctrl, k_alt, k_caps, k_e0;

int translate(int sc)
{
    if (sc == 0xE0) { k_e0 = true; return -1; }
    bool ext = k_e0;
    k_e0 = false;
    bool release = sc & 0x80;
    sc &= 0x7F;
    if (sc == 0x2A || sc == 0x36) { k_shift = !release; return -1; }
    if (sc == 0x1D) { k_ctrl = !release; return -1; }
    if (sc == 0x38) { k_alt = !release; return -1; }
    if (release) return -1;
    if (sc == 0x3A) { k_caps = !k_caps; return -1; }

    if (ext) {
        switch (sc) {
        case 0x48: return KEY_UP;    case 0x50: return KEY_DOWN;  case 0x4B: return KEY_LEFT; case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;  case 0x4F: return KEY_END;   case 0x49: return KEY_PPAGE; case 0x51: return KEY_NPAGE;
        case 0x52: return KEY_IC;    case 0x53: return KEY_DC;    case 0x1C: return 10;        case 0x35: return '/';
        default: return -1;
        }
    }
    if (sc >= 0x3B && sc <= 0x44) return KEY_F(sc - 0x3B + 1);
    if (sc == 0x57) return KEY_F(11);
    if (sc == 0x58) return KEY_F(12);
    if (sc >= 0x47 && sc <= 0x53) {
        static const char pad[] = {'7','8','9','-','4','5','6','+','1','2','3','0','.'};   // 0x47..0x53, NumLock on
        return pad[sc - 0x47];
    }
    if (sc >= 0x3A) return -1;
    char c = (k_shift ? SHIFTED : NORMAL)[sc];
    if (!c) return -1;
    if (k_caps && c >= 'a' && c <= 'z') c = (char)(c - 32);
    else if (k_caps && c >= 'A' && c <= 'Z' && !k_shift) c = (char)(c + 32);
    if (k_ctrl && c >= 'a' && c <= 'z') return c - 'a' + 1;
    if (k_ctrl && c >= 'A' && c <= 'Z') return c - 'A' + 1;
    return (unsigned char)c;
}
}

extern "C" {

WINDOW *newwin(int lines, int cols, int begin_y, int begin_x)
{
    if (lines <= 0) lines = g_rows - begin_y;
    if (cols <= 0) cols = g_cols - begin_x;
    WINDOW *w = new radium_win();
    w->y = begin_y; w->x = begin_x; w->h = lines; w->w = cols; w->cy = w->cx = 0; w->attrs = 0; w->is_stdscr = false;
    w->cells.assign((size_t)lines * cols, cell());
    return w;
}

int delwin(WINDOW *win) { if (win && !win->is_stdscr) delete win; return OK; }

WINDOW *initscr(void)
{
    const radium_api_t *A = radium_api();
    init_cp437();
    g_cols = (int)A->cols; g_rows = (int)A->rows;
    COLS = g_cols; LINES = g_rows;
    vscr.assign((size_t)g_cols * g_rows, cell());
    stdscr = newwin(g_rows, g_cols, 0, 0);
    stdscr->is_stdscr = true;
    curscr = newscr = stdscr;
    return stdscr;
}

int endwin(void) { return OK; }
int start_color(void) { return OK; }
int init_pair(short pair, short fg, short bg) { if (pair >= 0 && pair < 512) { pair_fg[pair] = fg; pair_bg[pair] = bg; } return OK; }
int noecho(void) { return OK; }
int cbreak(void) { return OK; }
int keypad(WINDOW *, int) { return OK; }
int set_escdelay(int) { return OK; }
int curs_set(int) { return 1; }
int mouseinterval(int) { return OK; }
mmask_t mousemask(mmask_t, mmask_t *old) { if (old) *old = 0; return 0; }
int getmouse(MEVENT *) { return ERR; }
int is_term_resized(int, int) { return 0; }

int timeout(int delay) { g_delay = delay; return OK; }
int wtimeout(WINDOW *, int delay) { g_delay = delay; return OK; }
int ungetch(int ch) { if (g_nungot < 8) g_ungot[g_nungot++] = ch; return OK; }

int getch(void)
{
    if (g_nungot) return g_ungot[--g_nungot];
    const radium_api_t *A = radium_api();
    uint32_t start = A->millis();
    for (;;) {
        int sc = A->key_poll();
        if (sc >= 0) {
            int k = translate(sc);
            if (k >= 0) return k;
            continue;
        }
        if (g_delay == 0) return ERR;
        if (g_delay > 0 && A->millis() - start >= (uint32_t)g_delay) return ERR;
        A->sleep_ms(1);
    }
}
int wgetch(WINDOW *) { return getch(); }

int werase(WINDOW *w)
{
    if (!w) return ERR;
    for (auto &c : w->cells) { c.ch = ' '; c.attr = 0; }
    w->cy = w->cx = 0;
    return OK;
}
int wclear(WINDOW *w) { return werase(w); }
int erase(void) { return werase(stdscr); }
int clear(void) { return werase(stdscr); }

int wmove(WINDOW *w, int y, int x)
{
    if (!w || y < 0 || x < 0 || y >= w->h || x >= w->w) return ERR;
    w->cy = y; w->cx = x;
    return OK;
}

int waddch(WINDOW *w, chtype ch) { if (!w) return ERR; put_chtype(w, ch); return OK; }
int mvwaddch(WINDOW *w, int y, int x, chtype ch) { if (wmove(w, y, x) == ERR) return ERR; return waddch(w, ch); }

static int vprint(WINDOW *w, const char *fmt, va_list ap)
{
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    const char *s = buf;
    while (*s) put(w, utf8_next(s));
    return OK;
}
int wprintw(WINDOW *w, const char *fmt, ...) { if (!w) return ERR; va_list ap; va_start(ap, fmt); int r = vprint(w, fmt, ap); va_end(ap); return r; }
int mvwprintw(WINDOW *w, int y, int x, const char *fmt, ...)
{
    if (!w || wmove(w, y, x) == ERR) return ERR;
    va_list ap; va_start(ap, fmt); int r = vprint(w, fmt, ap); va_end(ap);
    return r;
}

int wattron(WINDOW *w, int attrs)
{
    if (!w) return ERR;
    unsigned a = (unsigned)attrs;
    if (a & A_COLOR) w->attrs = (w->attrs & ~A_COLOR) | (a & A_COLOR);
    w->attrs |= a & (A_BOLD | A_BLINK | A_REVERSE);
    return OK;
}
int wattroff(WINDOW *w, int attrs)
{
    if (!w) return ERR;
    unsigned a = (unsigned)attrs;
    if (a & A_COLOR) w->attrs &= ~A_COLOR;
    w->attrs &= ~(a & (A_BOLD | A_BLINK | A_REVERSE));
    return OK;
}

static void put_at(WINDOW *w, int y, int x, chtype ch, chtype fallback)
{
    if (y < 0 || x < 0 || y >= w->h || x >= w->w) return;
    int sy = w->cy, sx = w->cx;
    w->cy = y; w->cx = x;
    put_chtype(w, ch ? ch : fallback);
    w->cy = sy; w->cx = sx;
}

int wborder(WINDOW *w, chtype ls, chtype rs, chtype ts, chtype bs, chtype tl, chtype tr, chtype bl, chtype br)
{
    if (!w) return ERR;
    for (int x = 1; x < w->w - 1; x++) { put_at(w, 0, x, ts, ACS_HLINE); put_at(w, w->h - 1, x, bs, ACS_HLINE); }
    for (int y = 1; y < w->h - 1; y++) { put_at(w, y, 0, ls, ACS_VLINE); put_at(w, y, w->w - 1, rs, ACS_VLINE); }
    put_at(w, 0, 0, tl, ACS_ULCORNER); put_at(w, 0, w->w - 1, tr, ACS_URCORNER);
    put_at(w, w->h - 1, 0, bl, ACS_LLCORNER); put_at(w, w->h - 1, w->w - 1, br, ACS_LRCORNER);
    return OK;
}
int mvwhline(WINDOW *w, int y, int x, chtype ch, int n) { if (!w) return ERR; for (int i = 0; i < n; i++) put_at(w, y, x + i, ch, ACS_HLINE); return OK; }
int mvwvline(WINDOW *w, int y, int x, chtype ch, int n) { if (!w) return ERR; for (int i = 0; i < n; i++) put_at(w, y + i, x, ch, ACS_VLINE); return OK; }
int wredrawln(WINDOW *, int, int) { return OK; }

int wnoutrefresh(WINDOW *w)
{
    if (!w) return ERR;
    for (int y = 0; y < w->h; y++) {
        int sy = w->y + y;
        if (sy < 0 || sy >= g_rows) continue;
        for (int x = 0; x < w->w; x++) {
            int sx = w->x + x;
            if (sx < 0 || sx >= g_cols) continue;
            vscr[(size_t)sy * g_cols + sx] = w->cells[(size_t)y * w->w + x];
        }
    }
    return OK;
}
int doupdate(void) { blit(); return OK; }
int wrefresh(WINDOW *w) { wnoutrefresh(w); return doupdate(); }
int refresh(void) { return wrefresh(stdscr); }

int waddwstr(WINDOW *w, const wchar_t *str) { if (!w) return ERR; for (; *str; str++) put(w, (uint32_t)*str); return OK; }

int getmaxx(const WINDOW *w) { return w ? w->w : 0; }
int getmaxy(const WINDOW *w) { return w ? w->h : 0; }
int getbegx(const WINDOW *w) { return w ? w->x : 0; }
int getbegy(const WINDOW *w) { return w ? w->y : 0; }
int getcurx(const WINDOW *w) { return w ? w->cx : 0; }
int getcury(const WINDOW *w) { return w ? w->cy : 0; }

}
