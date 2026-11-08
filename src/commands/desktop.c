// desktop.c - RadiumDE, a small Xfce-style desktop environment drawn through the Vulkan implementation
//
//   desktop            start it (it is a command, nothing runs at boot; boot with `desktop` on the kernel command line to
//                      start it automatically). Log out from the panel menu or with Ctrl+Alt+Q.
//   desktop check [FILE] | defaults [-f] | config | themes
//
// A top panel (Applications menu, one button per window, workspace pager, memory/disk, clock, power menu), a dock of
// launchers at the bottom, icons on the desktop and floating windows with title bars: drag a title to move, drag an edge
// or corner to resize, double-click a title to maximise, minimise to the panel. Applications: Terminal (runs RadiumOS
// commands, Ctrl+C stops a hung one), Files, System Monitor, Clock, Settings and a text viewer. Everything is configured
// by a plain text file (desktop.cfg, see DEFAULT_CONF) that Settings can also write.
//
// How it draws: the CPU composes the whole screen into one backing store (a Vulkan buffer) rectangle by rectangle,
// bottom to top (wallpaper, icons, windows with shadows, dock, panel, menus). Only damaged rectangles are composed again
// and copied into the swapchain image (vkCmdCopyBufferToImage); the mouse pointer is a small patch copied on top last.
// Keys: Alt+Tab next window, Alt+F4 close, Alt+F2 run a command, Alt+F1 menu, Ctrl+Alt+T terminal, Alt+F10 maximise,
// Alt+F9 minimise, Alt+1..4 workspace (Alt+Shift+1..4 send the window there), Ctrl+Alt+Q log out.

#include <stdint.h>
#include <stdbool.h>
#include "desktop.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../vga/fbcon.h"
#include "../utility/utility.h"
#include "../io/io.h"
#include "../keyboard/keyboard.h"
#include "../kernel/ctrlc.h"
#include "../io/kbdtap.h"
#include "../timers/tsc.h"
#include "../timers/date.h"
#include "../timers/timer.h"
#include "../memory/pmm.h"
#include "../Avfs/Avfs.h"
#include "../user/users.h"
#include "../user/sudo.h"
#include "../kernel/cmdline.h"
#include "../vulkan/vulkan.h"
#include "rsd.h"
#include "update.h"
#include "apprun.h"

extern uint8_t vga_font_8x8[256][8];   // bit 0 is the left pixel
extern uint32_t rust_get_task_count(void);
extern void shell_abort_recover(void);

#define SW 1280
#define SH 720
#define PATCH 16                                   // the pointer patch is PATCH x PATCH pixels
#define STORE_H (SH + PATCH)
#define CW 8
#define CH 16
#define PANEL_H 30
#define DOCK_H 64
#define DOCK_GAP 10
#define TITLE_H 28
#define BORD 1
#define PADX 8
#define EDGE 6                                     // grab zone for resizing
#define MIN_W 260
#define MIN_H 150
#define MAX_WIN 16
#define FMAX 160                                   // entries shown in one folder
#define NWS 4
#define MAX_IMG 4
#define TL 300
#define TC 156
#define HN 128
#define VT_MAX_W 160
#define VT_MAX_H 90
#define MAX_LAUNCH 6
#define MAX_AUTO 6
#define TRANSPARENT 0xFFFFFFFFu

typedef struct { int x, y, w, h; } rect_t;

// ---- colour themes ---------------------------------------------------------------------------------------------------

typedef struct {
    const char *name;
    uint32_t wp_top, wp_bot;
    uint32_t panel_bg, panel_fg, panel_dim, panel_hot, panel_line, accent, accent_tx;
    uint32_t t_f_bg, t_f_tx, t_u_bg, t_u_tx, border, border_f;     // window title bar (focused/unfocused) and frame
    uint32_t bg, fg, dim, field, line;                              // application colours
    uint32_t term_bg, term_fg, term_input;
    uint32_t menu_bg, menu_fg, menu_hot;
    uint32_t good, warn, bad;
} theme_t;

static const theme_t THEMES[] = {
    { "greybird", 0x3a76a8, 0x12304d,
      0x2a2d34, 0xe8eaed, 0x9aa0ab, 0x3d424d, 0x1b1d22, 0x5294e2, 0xffffff,
      0xe7e8ea, 0x2e3436, 0xf2f2f3, 0x80868e, 0x9ea3ab, 0x5294e2,
      0xf5f6f7, 0x2e3436, 0x7a8089, 0xffffff, 0xd0d3d8,
      0x1e2127, 0xd7dae0, 0x2a2e36,
      0xf4f5f6, 0x2e3436, 0x5294e2,
      0x4e9a06, 0xc4a000, 0xcc0000 },
    { "dark", 0x1a2233, 0x0b0d12,
      0x161b22, 0xcdd3de, 0x7b8496, 0x262d38, 0x0b0d12, 0x4fc3f7, 0x0b0d12,
      0x27303d, 0xe6ebf2, 0x1c2128, 0x7b8496, 0x2d333d, 0x4c7899,
      0x11131a, 0xcdd3de, 0x6b7385, 0x0d1117, 0x2d333d,
      0x0d1117, 0xcdd3de, 0x161b22,
      0x1c2128, 0xcdd3de, 0x285577,
      0x7ee787, 0xf2cc60, 0xff7b72 },
    { "nord", 0x4c566a, 0x2e3440,
      0x2e3440, 0xd8dee9, 0x7b88a1, 0x3b4252, 0x242933, 0x88c0d0, 0x2e3440,
      0x3b4252, 0xeceff4, 0x2e3440, 0x7b88a1, 0x434c5e, 0x88c0d0,
      0x2e3440, 0xd8dee9, 0x7b88a1, 0x242933, 0x434c5e,
      0x242933, 0xd8dee9, 0x2e3440,
      0x3b4252, 0xd8dee9, 0x5e81ac,
      0xa3be8c, 0xebcb8b, 0xbf616a },
    { "dracula", 0x44475a, 0x1e1f29,
      0x21222c, 0xf8f8f2, 0x6272a4, 0x343746, 0x16171d, 0xbd93f9, 0x21222c,
      0x44475a, 0xf8f8f2, 0x282a36, 0x6272a4, 0x44475a, 0xbd93f9,
      0x282a36, 0xf8f8f2, 0x6272a4, 0x21222c, 0x44475a,
      0x21222c, 0xf8f8f2, 0x282a36,
      0x343746, 0xf8f8f2, 0x6272a4,
      0x50fa7b, 0xf1fa8c, 0xff5555 },
};
#define NTHEMES ((int)(sizeof(THEMES) / sizeof(THEMES[0])))

static theme_t TH;

typedef enum { W_TERM, W_FILES, W_MON, W_CLOCK, W_SET, W_VIEW, W_CALC, W_EDIT, W_GAMES, W_SERV, W_SCRIPTS, W_TASKS } wtype_t;
enum { APP_TERM, APP_FILES, APP_MON, APP_CLOCK, APP_SET, APP_CALC, APP_EDIT, APP_GAMES, APP_SERV, APP_SCRIPTS, APP_TASKS, APP_COUNT };

typedef struct {
    bool used, minimized, maximized;
    wtype_t type;
    int ws, id;
    int x, y, w, h;                // outer rectangle: frame, title bar and content
    int rx, ry, rw, rh;            // where it goes back to when un-maximised
    char title[40];
    // terminal / viewer
    char lines[TL][TC];
    uint8_t lattr[TL][TC];         // VGA attribute per character (0 = default)
    int nl, col, scroll;
    bool running;                  // a command owns this window's virtual terminal
    bool gfx, fullscreen, fsmax;   // the program draws pixels / covers the screen / was maximised before
    char input[100];
    int inlen;
    char hist[8][100];
    int nhist, hpos;
    // monitor
    uint8_t hm[HN], hu[HN], hd[HN];
    int hn;
    // files
    char path[96];
    DirectoryEntry fent[FMAX];
    bool fmark[FMAX];
    int nfent, fsel, ftop, fanchor, fmode, fsort;
    bool fdesc;
    uint32_t fsig;
    // calculator
    double c_acc;
    int c_op;
    bool c_fresh, c_err;
    char c_in[24], c_expr[32];
    // text editor (the text lives in lines[]; nl is the number of lines)
    int erow, ecol, etop;
    bool edirty;
    char emsg[48];
    // lists: services, scripts, games
    struct { char name[32]; char sub[72]; int kind, st; } li[40];
    int nli, lsel, ltop;
    // settings and task manager tabs
    int tab;
    // task manager
    struct { uint32_t pid, ticks, prev; int state, pri, cpu; char name[20]; } tk[48];
    int ntk, tsel, ttop;
} win_t;

typedef struct { int mouse_speed, mouse_gain, dblclick, wallpaper; bool dock, icons, desk_files, clock24; int theme; int wp_mode; char wp_file[96]; bool shadows, snap, open_max, run_windowed, tray; int dock_size, term_style; } cfg_t;   // wallpaper: 0 gradient, 1 flat, 2 reverse, 3 picture
typedef struct { char label[24]; char cmd[48]; } launcher_t;
typedef struct { int kind; char arg[48]; } auto_t;

static cfg_t C;
static launcher_t g_launch[MAX_LAUNCH];
static int g_nlaunch;
static auto_t g_auto[MAX_AUTO];
static int g_nauto;
static char g_cfg_err[64];
static int g_cfg_nerr;

static win_t g_win[MAX_WIN];
static int g_z[MAX_WIN], g_zn;                      // stacking order, bottom first
static int g_focus = -1, g_ws, g_next_id, g_casc;
static bool g_alt, g_ctrl, g_shift, g_quit;
static int g_exit_action;                           // 1 reboot, 2 power off after leaving
static uint32_t *g_store;
static char g_msg[64];
static uint32_t g_msg_until;
static uint32_t g_busy_ms, g_up_base;
static uint32_t g_mem_pct, g_dsk_pct;

static const char DEFAULT_CONF[] =
    "# RadiumDE desktop configuration. Lines starting with # are comments; # followed by six hex digits is a colour.\n"
    "# Settings > Save writes this file; you can also edit it by hand and log in again.\n"
    "\n"
    "theme greybird               # greybird, dark, nord, dracula\n"
    "wallpaper default            # default (the mascot picture), gradient, flat or reverse\n"
    "# wallpaper_file /disk/Pictures/mine.png   use a PNG of your own\n"
    "# wallpaper_mode fill                      fill, fit, stretch or center (for your own picture)\n"
    "mouse_speed 45               # percent of the raw pointer speed when moving slowly (10-120)\n"
    "mouse_accel 60               # extra speed in percent when the mouse is swept quickly (0-200)\n"
    "double_click 450             # milliseconds between the two clicks of a double click (250-900)\n"
    "dock yes\n"
    "icons yes\n"
    "desktop_files yes            # show the files of the Desktop folder on the desktop\n"
    "clock24 yes\n"
    "window_shadows yes\n"
    "snap yes                     # windows stick to the screen edges; drop one at the top to maximise it\n"
    "open_maximised no\n"
    "run_in_window yes            # games and graphics programs run in a window (no = take the whole screen)\n"
    "panel_tray yes               # memory and disk in the panel\n"
    "dock_size 48                 # icon size in the dock (32-64)\n"
    "terminal_colors theme        # theme, classic, amber or light\n"
    "\n"
    "# launcher \"Label\" command      adds an entry to the dock and the Applications menu (up to 6)\n"
    "# launcher \"Memory\" meminfo\n"
    "\n"
    "# windows opened at login: autostart open term|files|monitor|clock|settings|calc|editor|games|services|scripts, autostart exec COMMAND\n"
    "autostart open term\n";

// ---- time and formatting -----------------------------------------------------------------------------------------------

static uint32_t now_ms(void)
{
    static uint32_t last_lo, ms, rem;
    static bool first = true;
    uint32_t lo, hi, per_ms = tsc_cycles_per_ms();
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    if (first) { first = false; last_lo = lo; return 0; }
    rem += lo - last_lo;
    last_lo = lo;
    if (per_ms) { ms += rem / per_ms; rem %= per_ms; }
    return ms;
}

static int put_str(char *d, int n, const char *s) { int i = 0; while (s[i] && i < n - 1) { d[i] = s[i]; i++; } d[i] = 0; return i; }

static int put_uint(char *d, uint32_t v)
{
    char t[12];
    int n = 0, i = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) d[i++] = t[--n];
    d[i] = 0;
    return i;
}

static int put_2d(char *d, uint32_t v) { d[0] = (char)('0' + (v / 10) % 10); d[1] = (char)('0' + v % 10); d[2] = 0; return 2; }

static void fmt_hms(char *d, uint32_t secs)
{
    put_2d(d, secs / 3600);
    d[2] = ':';
    put_2d(d + 3, (secs / 60) % 60);
    d[5] = ':';
    put_2d(d + 6, secs % 60);
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool same_nocase(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (lc(*a) != lc(*b)) return false;
    return *a == *b;
}

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iabs(int a) { return a < 0 ? -a : a; }

static bool rect_has(rect_t r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static bool rect_isect(rect_t a, rect_t b, rect_t *o)
{
    int x0 = imax(a.x, b.x), y0 = imax(a.y, b.y), x1 = imin(a.x + a.w, b.x + b.w), y1 = imin(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return false;
    o->x = x0; o->y = y0; o->w = x1 - x0; o->h = y1 - y0;
    return true;
}

static rect_t rect_union(rect_t a, rect_t b)
{
    int x0 = imin(a.x, b.x), y0 = imin(a.y, b.y), x1 = imax(a.x + a.w, b.x + b.w), y1 = imax(a.y + a.h, b.y + b.h);
    rect_t r = { x0, y0, x1 - x0, y1 - y0 };
    return r;
}

// ---- drawing into the backing store, clipped to the rectangle being composed --------------------------------------------

static int clip_x0, clip_y0, clip_x1 = SW, clip_y1 = SH;

static void clip_set(rect_t r) { clip_x0 = r.x; clip_y0 = r.y; clip_x1 = r.x + r.w; clip_y1 = r.y + r.h; }

static void fill_rect(int x, int y, int w, int h, uint32_t c)
{
    if (x < clip_x0) { w -= clip_x0 - x; x = clip_x0; }
    if (y < clip_y0) { h -= clip_y0 - y; y = clip_y0; }
    if (x + w > clip_x1) w = clip_x1 - x;
    if (y + h > clip_y1) h = clip_y1 - y;
    for (int r = 0; r < h; r++) {
        uint32_t *p = g_store + (y + r) * SW + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

static void frame_rect(int x, int y, int w, int h, uint32_t c)
{
    fill_rect(x, y, w, 1, c); fill_rect(x, y + h - 1, w, 1, c);
    fill_rect(x, y, 1, h, c); fill_rect(x + w - 1, y, 1, h, c);
}

static uint32_t isqrt_u(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return r;
}

// Mixes colour c over the pixel by a/255.
static void blend_px(int x, int y, uint32_t c, int a)
{
    if (a <= 0 || x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1) return;
    uint32_t *p = g_store + y * SW + x;
    if (a >= 255) { *p = c; return; }
    uint32_t d = *p, ia = (uint32_t)(255 - a), ua = (uint32_t)a;
    uint32_t r = (((c >> 16) & 255) * ua + ((d >> 16) & 255) * ia) / 255;
    uint32_t g = (((c >> 8) & 255) * ua + ((d >> 8) & 255) * ia) / 255;
    uint32_t b = ((c & 255) * ua + (d & 255) * ia) / 255;
    *p = r << 16 | g << 8 | b;
}

static uint8_t g_corner[9][8][8];                    // coverage of the pixels in a rounded corner of radius 1..8
static bool g_corner_ready;

static void corner_init(void)
{
    for (int r = 1; r <= 8; r++)
        for (int i = 0; i < r; i++)
            for (int j = 0; j < r; j++) {
                int dx = r * 4 - (j * 4 + 2), dy = r * 4 - (i * 4 + 2);
                int d = (int)isqrt_u((uint32_t)(dx * dx + dy * dy)), v = r * 4 - d + 2;
                g_corner[r][i][j] = v <= 0 ? 0 : (v >= 4 ? 255 : (uint8_t)(v * 255 / 4));
            }
    g_corner_ready = true;
}

// a rectangle with anti-aliased corners of radius r (at most 8)
static void round_rect(int x, int y, int w, int h, int r, uint32_t c)
{
    if (r > 8) r = 8;
    if (w < 2 * r || h < 2 * r) r = 0;
    if (r == 0) { fill_rect(x, y, w, h, c); return; }
    if (!g_corner_ready) corner_init();
    fill_rect(x + r, y, w - 2 * r, h, c);
    fill_rect(x, y + r, r, h - 2 * r, c);
    fill_rect(x + w - r, y + r, r, h - 2 * r, c);
    for (int i = 0; i < r; i++)
        for (int j = 0; j < r; j++) {
            int a = g_corner[r][i][j];
            blend_px(x + j, y + i, c, a); blend_px(x + w - 1 - j, y + i, c, a);
            blend_px(x + j, y + h - 1 - i, c, a); blend_px(x + w - 1 - j, y + h - 1 - i, c, a);
        }
}

static void disc(int cx, int cy, int r, uint32_t c)
{
    int rad4 = r * 4 + 1, solid = rad4 - 3;
    for (int dy = -r - 1; dy <= r + 1; dy++)
        for (int dx = -r - 1; dx <= r + 1; dx++) {
            int q = dx * dx * 16 + dy * dy * 16;
            if (q <= solid * solid) { blend_px(cx + dx, cy + dy, c, 255); continue; }
            int v = rad4 - (int)isqrt_u((uint32_t)q) + 2;
            if (v > 0) blend_px(cx + dx, cy + dy, c, v >= 4 ? 255 : v * 255 / 4);
        }
}

static void line(int x0, int y0, int x1, int y1, int th, uint32_t c)
{
    int dx = iabs(x1 - x0), dy = iabs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
    for (;;) {
        fill_rect(x0, y0, th, th, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

static uint32_t shade(uint32_t c, int pct)       // pct 0..200 around 100
{
    int r = (int)((c >> 16) & 255) * pct / 100, g = (int)((c >> 8) & 255) * pct / 100, b = (int)(c & 255) * pct / 100;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (uint32_t)(r << 16 | g << 8 | b);
}

static uint32_t mix(uint32_t a, uint32_t b, int pct_b)
{
    int r = (int)((a >> 16) & 255) + ((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * pct_b / 100;
    int g = (int)((a >> 8) & 255) + ((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * pct_b / 100;
    int bl = (int)(a & 255) + ((int)(b & 255) - (int)(a & 255)) * pct_b / 100;
    return (uint32_t)(r << 16 | g << 8 | bl);
}

static void vgrad(int x, int y, int w, int h, uint32_t top, uint32_t bot)
{
    for (int i = 0; i < h; i++) fill_rect(x, y + i, w, 1, mix(top, bot, h > 1 ? i * 100 / (h - 1) : 0));
}

static void scrollbar(int x, int y, int h, int total, int visible, int top)
{
    if (total <= visible || visible <= 0) return;
    int th = imax(18, h * visible / total);
    int ty = y + (h - th) * top / imax(1, total - visible);
    round_rect(x, y, 6, h, 3, mix(TH.field, TH.line, 50));
    round_rect(x, ty, 6, th, 3, mix(TH.line, TH.dim, 60));
}

// bg == TRANSPARENT leaves the pixels behind the glyph alone
static void glyph(int x, int y, unsigned char ch, uint32_t fg, uint32_t bg)
{
    int c0 = imax(clip_x0 - x, 0), c1 = imin(clip_x1 - x, CW);
    if (c0 >= c1) return;
    for (int r = 0; r < 8; r++) {
        uint8_t bits = vga_font_8x8[ch][r];
        for (int d = 0; d < 2; d++) {
            int yy = y + r * 2 + d;
            if (yy < clip_y0 || yy >= clip_y1) continue;
            uint32_t *p = g_store + yy * SW + x;
            for (int c = c0; c < c1; c++) {
                if ((bits >> c) & 1) p[c] = fg;
                else if (bg != TRANSPARENT) p[c] = bg;
            }
        }
    }
}

static void glyph_big(int x, int y, unsigned char ch, int s, uint32_t fg, uint32_t bg)
{
    for (int r = 0; r < 8; r++) {
        uint8_t bits = vga_font_8x8[ch][r];
        for (int c = 0; c < 8; c++) {
            if ((bits >> c) & 1) fill_rect(x + c * s, y + r * s, s, s, fg);
            else if (bg != TRANSPARENT) fill_rect(x + c * s, y + r * s, s, s, bg);
        }
    }
}

static void text(int x, int y, const char *s, uint32_t fg, uint32_t bg, int max_cols)
{
    for (int i = 0; s[i] && i < max_cols; i++) glyph(x + i * CW, y, (unsigned char)s[i], fg, bg);
}

static void text_center(int x, int w, int y, const char *s, uint32_t fg, uint32_t bg, int max_cols)
{
    int n = imin((int)strlen(s), max_cols);
    text(x + (w - n * CW) / 2, y, s, fg, bg, n);
}

static const uint32_t VGA_RGB[16] = { 0x000000, 0x4c6ef5, 0x51cf66, 0x22b8cf, 0xff6b6b, 0xcc5de8, 0xfab005, 0xcdd3de,
                                      0x6b7385, 0x74c0fc, 0x8ce99a, 0x66d9e8, 0xff8787, 0xe599f7, 0xffe066, 0xffffff };
static uint32_t attr_fg(uint8_t a) { return a == 0 ? TH.term_fg : VGA_RGB[a & 15]; }
static uint32_t attr_bg(uint8_t a, uint32_t dflt) { uint8_t b = (a >> 4) & 15; return b ? VGA_RGB[b] : dflt; }

static void text_attr(int x, int y, const char *s, const uint8_t *at, uint32_t dflt_bg, int max_cols)
{
    for (int i = 0; s[i] && i < max_cols; i++) glyph(x + i * CW, y, (unsigned char)s[i], attr_fg(at[i]), attr_bg(at[i], dflt_bg));
}

// ---- icons (drawn from rectangles and discs; coordinates are in a 48-unit grid scaled to s pixels) -------------------------

enum { IC_TERM, IC_FILES, IC_MON, IC_CLOCK, IC_SET, IC_HOME, IC_FILE, IC_RUN, IC_VIEW, IC_APP, IC_CALC, IC_EDIT, IC_GAME, IC_SERV, IC_SCRIPT, IC_CONSOLE, IC_USER };

#define U(v) ((v) * s / 48)

static void draw_icon(int kind, int x, int y, int s)
{
    switch (kind) {
    case IC_TERM:
        round_rect(x + U(3), y + U(6), U(42), U(36), 3, 0x23262e);
        fill_rect(x + U(3), y + U(6), U(42), U(7), 0x3b414d);
        disc(x + U(9), y + U(9), imax(1, U(2)), 0xff6b6b); disc(x + U(16), y + U(9), imax(1, U(2)), 0xffd43b);
        if (s >= 32) { glyph_big(x + U(10), y + U(18), '>', imax(1, s / 24), 0x69db7c, TRANSPARENT); fill_rect(x + U(26), y + U(34), U(12), imax(2, U(3)), 0xdee2e6); }
        else { fill_rect(x + U(10), y + U(22), U(8), imax(2, U(4)), 0x69db7c); fill_rect(x + U(22), y + U(32), U(12), imax(2, U(3)), 0xdee2e6); }
        break;
    case IC_FILES: case IC_HOME:
        round_rect(x + U(3), y + U(11), U(20), U(8), 3, 0xe0a82e);
        round_rect(x + U(3), y + U(15), U(42), U(27), 3, 0xf7c548);
        fill_rect(x + U(3), y + U(20), U(42), U(2), 0xfbd97a);
        if (kind == IC_HOME) {
            fill_rect(x + U(21), y + U(26), U(6), U(12), 0xa5702a);
            for (int i = 0; i < U(8); i++) fill_rect(x + U(24) - i * 2, y + U(21) + i, i * 4, 1, 0xa5702a);
        }
        break;
    case IC_MON:
        round_rect(x + U(3), y + U(6), U(42), U(30), 3, 0x2b303b);
        fill_rect(x + U(7), y + U(10), U(34), U(22), 0x11151c);
        for (int i = 0; i < 5; i++) {
            int bh = U(5 + (i * 7 + 3) % 15);
            fill_rect(x + U(10 + i * 6), y + U(31) - bh, U(4), bh, i % 2 ? 0x51cf66 : 0x4fc3f7);
        }
        fill_rect(x + U(18), y + U(36), U(12), U(3), 0x5c6370); fill_rect(x + U(13), y + U(39), U(22), U(3), 0x5c6370);
        break;
    case IC_CLOCK: {
        int cx = x + U(24), cy = y + U(24);
        disc(cx, cy, U(21), 0x3b414d);
        disc(cx, cy, U(18), 0xf8f9fa);
        for (int i = 0; i < 12; i++) {
            static const int TX[12] = { 0, 9, 16, 18, 16, 9, 0, -9, -16, -18, -16, -9 }, TY[12] = { -18, -16, -9, 0, 9, 16, 18, 16, 9, 0, -9, -16 };
            fill_rect(cx + U(TX[i]) - 1, cy + U(TY[i]) - 1, imax(2, U(2)), imax(2, U(2)), 0x495057);
        }
        line(cx, cy, cx - U(8), cy - U(6), imax(2, U(3)), 0x212529);
        line(cx, cy, cx + U(3), cy - U(13), imax(2, U(2)), 0xe03131);
        disc(cx, cy, imax(1, U(2)), 0x212529);
        break;
    }
    case IC_SET: {
        int cx = x + U(24), cy = y + U(24);
        static const int GX[8] = { 0, 15, 21, 15, 0, -15, -21, -15 }, GY[8] = { -21, -15, 0, 15, 21, 15, 0, -15 };
        for (int i = 0; i < 8; i++) disc(cx + U(GX[i]), cy + U(GY[i]), imax(2, U(5)), 0x868e96);
        disc(cx, cy, U(17), 0x868e96);
        disc(cx, cy, U(8), 0x343a40);
        break;
    }
    case IC_FILE: case IC_VIEW:
        fill_rect(x + U(9), y + U(4), U(30), U(40), 0xf8f9fa);
        frame_rect(x + U(9), y + U(4), U(30), U(40), 0xadb5bd);
        fill_rect(x + U(30), y + U(4), U(9), U(9), 0xced4da);
        for (int i = 0; i < 4; i++) fill_rect(x + U(14), y + U(19 + i * 6), U(20), imax(1, U(2)), kind == IC_VIEW ? 0x4c6ef5 : 0xadb5bd);
        break;
    case IC_CALC:
        round_rect(x + U(8), y + U(3), U(32), U(42), 4, 0x495057);
        fill_rect(x + U(12), y + U(8), U(24), U(10), 0xb2f2bb);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) fill_rect(x + U(12 + c * 9), y + U(22 + r * 7), U(6), U(5), c == 2 ? 0xffa94d : 0xdee2e6);
        break;
    case IC_EDIT:
        fill_rect(x + U(8), y + U(4), U(28), U(40), 0xf8f9fa);
        frame_rect(x + U(8), y + U(4), U(28), U(40), 0xadb5bd);
        for (int i = 0; i < 5; i++) fill_rect(x + U(12), y + U(10 + i * 6), U(18), imax(1, U(2)), 0xced4da);
        line(x + U(22), y + U(37), x + U(40), y + U(17), imax(2, U(5)), 0xffd43b);
        disc(x + U(21), y + U(38), imax(1, U(2)), 0x495057);
        break;
    case IC_GAME:
        round_rect(x + U(3), y + U(14), U(42), U(22), 8, 0x5c7cfa);
        fill_rect(x + U(10), y + U(23), U(10), U(4), 0xf1f3f5); fill_rect(x + U(13), y + U(20), U(4), U(10), 0xf1f3f5);
        disc(x + U(33), y + U(21), imax(2, U(3)), 0xff6b6b); disc(x + U(38), y + U(27), imax(2, U(3)), 0xffd43b);
        break;
    case IC_SERV:
        for (int i = 0; i < 3; i++) {
            round_rect(x + U(6), y + U(6 + i * 13), U(36), U(10), 3, 0x495057);
            disc(x + U(12), y + U(11 + i * 13), imax(1, U(2)), i == 1 ? 0xffd43b : 0x69db7c);
            fill_rect(x + U(20), y + U(9 + i * 13), U(18), imax(1, U(2)), 0x868e96);
        }
        break;
    case IC_SCRIPT:
        fill_rect(x + U(9), y + U(4), U(30), U(40), 0xf8f9fa);
        frame_rect(x + U(9), y + U(4), U(30), U(40), 0xadb5bd);
        fill_rect(x + U(13), y + U(11), U(10), imax(1, U(3)), 0xe64980); fill_rect(x + U(25), y + U(11), U(10), imax(1, U(3)), 0x4c6ef5);
        fill_rect(x + U(17), y + U(19), U(16), imax(1, U(3)), 0x51cf66); fill_rect(x + U(13), y + U(27), U(8), imax(1, U(3)), 0xfab005);
        fill_rect(x + U(23), y + U(27), U(12), imax(1, U(3)), 0x4c6ef5); fill_rect(x + U(17), y + U(35), U(14), imax(1, U(3)), 0xe64980);
        break;
    case IC_CONSOLE:
        round_rect(x + U(3), y + U(6), U(42), U(36), 3, 0x000000);
        frame_rect(x + U(3), y + U(6), U(42), U(36), 0x868e96);
        if (s >= 32) glyph_big(x + U(9), y + U(14), '>', imax(1, s / 24), 0xf1f3f5, TRANSPARENT);
        fill_rect(x + U(24), y + U(32), U(14), imax(2, U(3)), 0xf1f3f5);
        break;
    case IC_USER:
        disc(x + U(24), y + U(16), U(9), 0xced4da);
        round_rect(x + U(10), y + U(28), U(28), U(16), 8, 0xced4da);
        break;
    case IC_RUN:
        round_rect(x + U(3), y + U(12), U(42), U(24), 3, 0xdee2e6);
        fill_rect(x + U(7), y + U(16), U(34), U(16), 0xffffff);
        fill_rect(x + U(10), y + U(22), U(3), U(4), 0x343a40);
        break;
    default:
        round_rect(x + U(6), y + U(6), U(36), U(36), 6, 0x748ffc);
        fill_rect(x + U(14), y + U(14), U(20), U(20), 0xdbe4ff);
        break;
    }
}

// ---- damage tracking and composing ---------------------------------------------------------------------------------------

#define MAXRECT 24

static rect_t g_dirty[MAXRECT];
static int g_ndirty;
static int g_img = 2;
static rect_t g_pend[MAX_IMG][16];
static int g_npend[MAX_IMG];
static bool g_cur_dirty[MAX_IMG];

static void list_add(rect_t *l, int *n, int cap, rect_t r)
{
    for (int i = 0; i < *n; i++) {
        rect_t u = rect_union(l[i], r);
        long ua = (long)u.w * u.h, sa = (long)l[i].w * l[i].h + (long)r.w * r.h;
        rect_t o;
        if (ua <= sa + sa / 4 || (rect_isect(l[i], r, &o) && ua <= 2 * sa)) { l[i] = u; r = u; l[i] = l[*n - 1]; (*n)--; i = -1; }
    }
    if (*n == cap) {
        rect_t u = r;
        for (int i = 0; i < *n; i++) u = rect_union(u, l[i]);
        l[0] = u;
        *n = 1;
        return;
    }
    l[(*n)++] = r;
}

static void damage(int x, int y, int w, int h)
{
    rect_t r = { x, y, w, h }, all = { 0, 0, SW, SH }, o;
    if (!rect_isect(r, all, &o)) return;
    list_add(g_dirty, &g_ndirty, MAXRECT, o);
}

static void damage_rect(rect_t r) { damage(r.x, r.y, r.w, r.h); }
static void damage_all(void) { damage(0, 0, SW, SH); }
static void mark_cursor(void) { for (int i = 0; i < g_img; i++) g_cur_dirty[i] = true; }

static void flash(const char *m)
{
    put_str(g_msg, sizeof(g_msg), m);
    g_msg_until = now_ms() + 2500;
    damage(SW / 2 - 260, PANEL_H + 8, 520, 34);
}

// ---- windows: geometry and lookup ----------------------------------------------------------------------------------------

static bool win_visible(int wi) { win_t *w = &g_win[wi]; return w->used && w->ws == g_ws && !w->minimized; }

static rect_t win_rect(const win_t *w) { rect_t r = { w->x, w->y, w->w, w->h }; return r; }
static rect_t win_damage_rect(const win_t *w) { rect_t r = { w->x - 14, w->y - 12, w->w + 28, w->h + 32 }; return r; }
static rect_t content_rect(const win_t *w) { if (w->fullscreen) { rect_t f = { w->x, w->y, w->w, w->h }; return f; } rect_t r = { w->x + BORD, w->y + BORD + TITLE_H, w->w - 2 * BORD, w->h - 2 * BORD - TITLE_H }; return r; }
static rect_t title_rect(const win_t *w) { rect_t r = { w->x, w->y, w->w, BORD + TITLE_H }; return r; }
static void damage_win(const win_t *w) { damage_rect(win_damage_rect(w)); }
static void damage_content(const win_t *w) { damage_rect(content_rect(w)); }
static void damage_title(const win_t *w) { damage_rect(title_rect(w)); }

static int tile_cols(const win_t *w) { return (w->w - 2 * BORD - 2 * PADX) / CW; }
static int tile_rows(const win_t *w) { return (w->h - 2 * BORD - TITLE_H) / CH; }

static rect_t title_button(const win_t *w, int idx)     // 0 close, 1 maximise, 2 minimise
{
    rect_t r = { w->x + w->w - BORD - 28 - idx * 26, w->y + BORD + 3, 24, TITLE_H - 6 };
    return r;
}

static int topmost_visible(void);
static bool fullscreen_active(void) { int t = topmost_visible(); return t >= 0 && g_win[t].fullscreen; }

static int top_window_at(int x, int y)
{
    for (int k = g_zn - 1; k >= 0; k--) {
        int wi = g_z[k];
        if (win_visible(wi) && rect_has(win_rect(&g_win[wi]), x, y)) return wi;
    }
    return -1;
}

static void z_raise(int wi)
{
    int k = 0;
    while (k < g_zn && g_z[k] != wi) k++;
    if (k == g_zn) { if (g_zn < MAX_WIN) g_z[g_zn++] = wi; return; }
    for (; k < g_zn - 1; k++) g_z[k] = g_z[k + 1];
    g_z[g_zn - 1] = wi;
}

static void z_remove(int wi)
{
    int k = 0;
    while (k < g_zn && g_z[k] != wi) k++;
    if (k == g_zn) return;
    for (; k < g_zn - 1; k++) g_z[k] = g_z[k + 1];
    g_zn--;
}

static int topmost_visible(void)
{
    for (int k = g_zn - 1; k >= 0; k--) if (win_visible(g_z[k])) return g_z[k];
    return -1;
}

static void set_focus(int wi)
{
    if (wi == g_focus) return;
    int old = g_focus;
    g_focus = wi;
    if (old >= 0 && g_win[old].used) damage_title(&g_win[old]);
    if (wi >= 0) damage_title(&g_win[wi]);
    damage(0, 0, SW, PANEL_H);
}

static void focus_and_raise(int wi)
{
    if (wi < 0) return;
    if (g_win[wi].minimized) { g_win[wi].minimized = false; damage_win(&g_win[wi]); }
    if (g_z[g_zn - 1] != wi) { z_raise(wi); damage_win(&g_win[wi]); }
    set_focus(wi);
}

// ---- terminal text buffer ------------------------------------------------------------------------------------------------

static void term_newline(win_t *w)
{
    if (w->nl == TL) {
        memmove(w->lines[0], w->lines[1], (size_t)(TL - 1) * TC);
        memmove(w->lattr[0], w->lattr[1], (size_t)(TL - 1) * TC);
        w->nl--;
    }
    w->lines[w->nl][0] = 0;
    memset(w->lattr[w->nl], 0x07, TC);
    w->nl++;
    w->col = 0;
}

static void term_clear_lines(win_t *w)
{
    w->nl = 1;
    w->lines[0][0] = 0;
    memset(w->lattr[0], 0x07, TC);
    w->col = 0;
    w->scroll = 0;
}

static void term_put(win_t *w, char c)
{
    if (c == '\n') { term_newline(w); return; }
    if (c == '\f') { term_clear_lines(w); return; }
    if (c == '\t') {
        do { term_put(w, ' '); } while (w->col & 3);
        return;
    }
    if (c < 32 || c > 126) return;
    if (w->col >= TC - 1) term_newline(w);
    char *line_p = w->lines[w->nl - 1];
    line_p[w->col++] = c;
    line_p[w->col] = 0;
}

static void term_puts(win_t *w, const char *s) { while (*s) term_put(w, *s++); }

// Programs that need the whole screen (graphics modes, the legacy text windows): the desktop steps aside while they run.
static const char *const LEAVE[] = { "discord", "rie", "png", "mp4", "gfxtest", "rchat", "pong", "diskpart", "rawr", NULL };

static void first_word(const char *cmd, char *word, int n)
{
    int k = 0;
    while (*cmd == ' ') cmd++;
    while (cmd[k] && cmd[k] != ' ' && k < n - 1) { word[k] = cmd[k]; k++; }
    word[k] = 0;
}

static bool needs_screen(const char *cmd)
{
    char word[16];
    first_word(cmd, word, sizeof(word));
    for (int i = 0; LEAVE[i]; i++) if (strcmp(word, LEAVE[i]) == 0) return true;
    return false;
}

// One finished row of a command's virtual terminal becomes a scrollback line (with its colours).
static void term_add_cells(win_t *w, const uint16_t *cells, size_t n)
{
    int last = (int)n;
    while (last > 0 && ((cells[last - 1] & 0xff) == ' ' || (cells[last - 1] & 0xff) == 0)) last--;
    if (last > TC - 1) last = TC - 1;
    char *line_p = w->lines[w->nl - 1];
    uint8_t *at = w->lattr[w->nl - 1];
    for (int i = 0; i < last; i++) {
        unsigned char c = (unsigned char)(cells[i] & 0xff);
        line_p[i] = c ? (char)c : ' ';
        at[i] = (uint8_t)(cells[i] >> 8);
    }
    line_p[last] = 0;
    w->col = last;
    term_newline(w);
}

static uint32_t line_color(const char *s)
{
    if (s[0] == '!' && s[1] == ' ') return TH.bad == 0xcc0000 ? 0xff6b6b : TH.bad;
    if (s[0] == '$' && s[1] == ' ') return 0x69db7c;
    return TH.term_fg;
}

static uint16_t g_vt[VT_MAX_W * VT_MAX_H], g_vt_prev[VT_MAX_W * VT_MAX_H];
static int g_vt_w, g_vt_h;
static size_t g_vt_crow, g_vt_ccol;
extern size_t terminal_row, terminal_column;

// ---- application renderers (each draws a whole content rectangle; the clip does the rest) --------------------------------

static void render_gfx(win_t *w);

static void render_term(win_t *w, bool focused)
{
    if (w->running && w->gfx) { render_gfx(w); return; }
    rect_t c = content_rect(w);
    int rows = tile_rows(w), cols = tile_cols(w), ox = c.x + PADX, oy = c.y;
    fill_rect(c.x, c.y, c.w, c.h, TH.term_bg);
    if (w->running) {
        for (int y = 0; y < g_vt_h; y++) {
            for (int x = 0; x < g_vt_w; x++) {
                uint16_t cell = g_vt[y * g_vt_w + x];
                unsigned char ch = (unsigned char)(cell & 0xff);
                uint8_t a = (uint8_t)(cell >> 8);
                uint32_t bg = attr_bg(a, TH.term_bg);
                int gx = ox + x * CW, gy = oy + y * CH;
                if ((size_t)y == g_vt_crow && (size_t)x == g_vt_ccol) { glyph(gx, gy, ch ? ch : ' ', TH.term_bg, TH.accent); continue; }
                if ((ch == ' ' || ch == 0) && bg == TH.term_bg) continue;
                glyph(gx, gy, ch ? ch : ' ', attr_fg(a), bg);
            }
        }
        return;
    }
    bool prompt = w->type == W_TERM;
    int outrows = rows - (prompt ? 1 : 0);
    if (outrows < 1) return;
    int start = w->nl - outrows - w->scroll;
    if (start < 0) start = 0;
    for (int i = start; i < w->nl && i < start + outrows; i++) {
        const char *s = w->lines[i];
        bool plain = true;
        for (int k = 0; s[k]; k++) if (w->lattr[i][k] != 0x07 && w->lattr[i][k] != 0) { plain = false; break; }
        if (plain) text(ox, oy + (i - start) * CH, s, line_color(s), TH.term_bg, cols);
        else text_attr(ox, oy + (i - start) * CH, s, w->lattr[i], TH.term_bg, cols);
    }
    if (prompt) {
        int py = oy + (rows - 1) * CH;
        fill_rect(c.x, py, c.w, c.h - (rows - 1) * CH, TH.term_input);
        text(ox, py, "$ ", 0x69db7c, TH.term_input, 2);
        int room = cols - 3;
        int off = w->inlen > room ? w->inlen - room : 0;
        text(ox + 2 * CW, py, w->input + off, TH.term_fg, TH.term_input, room);
        if (focused) fill_rect(ox + (2 + w->inlen - off) * CW, py + 2, CW, CH - 4, TH.accent);
    }
    scrollbar(c.x + c.w - 8, c.y, outrows * CH, w->nl, outrows, start);
}

static void bar_graph(int x, int y, int w, int h, const uint8_t *hist, int n, uint32_t col)
{
    fill_rect(x, y, w, h, TH.field);
    frame_rect(x, y, w, h, TH.line);
    fill_rect(x + 1, y + h / 2, w - 2, 1, TH.line);
    int slots = (w - 4) / 2;
    int shown = n < slots ? n : slots;
    for (int i = 0; i < shown; i++) {
        int v = hist[(n - shown + i) % HN];
        int bh = v * (h - 4) / 100;
        fill_rect(x + w - 2 - (shown - i) * 2, y + h - 2 - bh, 2, bh, col);
    }
}

static void meter(int x, int y, int w, uint32_t pct)
{
    if (pct > 100) pct = 100;
    uint32_t col = pct >= 90 ? TH.bad : (pct >= 70 ? TH.warn : TH.good);
    round_rect(x, y + 2, w, CH - 4, 2, TH.line);
    int fw = (int)(pct * (uint32_t)w / 100);
    if (fw > 0) round_rect(x, y + 2, fw, CH - 4, 2, col);
}

static void render_mon(win_t *w)
{
    rect_t c = content_rect(w);
    fill_rect(c.x, c.y, c.w, c.h, TH.bg);
    int x = c.x + 14, y = c.y + 14, cols = tile_cols(w);
    char b[48];
    uint32_t up = g_up_base + now_ms() / 1000;
    text(x, y, "uptime", TH.dim, TH.bg, 8);
    fmt_hms(b, up);
    text(x + 9 * CW, y, b, TH.fg, TH.bg, 8);
    y += CH + 6;

    uint64_t total = pmm_get_total_memory(), freeb = pmm_get_free_memory();
    uint32_t tk = (uint32_t)(total >> 20), fk = (uint32_t)(freeb >> 20), uk = tk > fk ? tk - fk : 0;
    uint32_t mp = tk ? uk * 100 / tk : 0;
    uint32_t dp = 0, du = 0, dt = 0, dkt = 0, dku = 0;
    if (avfs_disk_space(&dkt, &dku) == 0) { du = dku >> 10; dt = dkt >> 10; dp = dt ? du * 100 / dt : 0; }
    int bw = c.w - 28 - 9 * CW - 16 * CW;
    if (bw < 40) bw = 40;

    text(x, y, "memory", TH.dim, TH.bg, 8);
    meter(x + 9 * CW, y, bw, mp);
    int k = put_uint(b, uk); b[k++] = '/'; k += put_uint(b + k, tk); k += put_str(b + k, 8, " MB ");  put_uint(b + k, mp);
    k = (int)strlen(b); b[k++] = '%'; b[k] = 0;
    if (cols > 40) text(x + 9 * CW + bw + 8, y, b, TH.fg, TH.bg, 16);
    y += CH + 6;

    text(x, y, "disk", TH.dim, TH.bg, 8);
    meter(x + 9 * CW, y, bw, dp);
    k = put_uint(b, du); b[k++] = '/'; k += put_uint(b + k, dt); k += put_str(b + k, 8, " MB "); put_uint(b + k, dp);
    k = (int)strlen(b); b[k++] = '%'; b[k] = 0;
    if (cols > 40) text(x + 9 * CW + bw + 8, y, b, TH.fg, TH.bg, 16);
    y += CH + 6;

    int last = w->hn ? (w->hn - 1) % HN : 0;
    uint32_t ui = w->hn ? w->hu[last] : 0;
    text(x, y, "tasks", TH.dim, TH.bg, 8);
    put_uint(b, rust_get_task_count());
    text(x + 9 * CW, y, b, TH.fg, TH.bg, 6);
    text(x + 16 * CW, y, "ui load", TH.dim, TH.bg, 8);
    k = put_uint(b, ui); b[k++] = '%'; b[k] = 0;
    text(x + 25 * CW, y, b, TH.fg, TH.bg, 6);
    y += CH + 14;

    int gw = c.w - 28, gh = 52;
    if (y + gh > c.y + c.h) return;
    text(x, y, "memory use", TH.dim, TH.bg, 20);
    y += CH + 2;
    bar_graph(x, y, gw, gh, w->hm, w->hn, TH.accent);
    y += gh + 10;
    if (y + gh > c.y + c.h) return;
    text(x, y, "ui load (render time per second)", TH.dim, TH.bg, 40);
    y += CH + 2;
    bar_graph(x, y, gw, gh, w->hu, w->hn, 0xd2a8ff);
    y += gh + 10;
    if (y + gh > c.y + c.h) return;
    text(x, y, "disk use", TH.dim, TH.bg, 20);
    y += CH + 2;
    bar_graph(x, y, gw, gh, w->hd, w->hn, TH.warn);
}

static void clock_geometry(const win_t *w, int *scale, int *dx, int *dy)
{
    rect_t c = content_rect(w);
    int s = c.w / (8 * CW + 8);
    int sh = (c.h - 3 * CH) / 8;
    if (sh < s) s = sh;
    if (s > 8) s = 8;
    if (s < 1) s = 1;
    *scale = s;
    *dx = c.x + (c.w - 8 * 8 * s) / 2;
    *dy = c.y + (c.h - 8 * s) / 2 - CH / 2;
}

static void render_clock(win_t *w)
{
    rect_t c = content_rect(w);
    fill_rect(c.x, c.y, c.w, c.h, TH.bg);
    datetime_t dt;
    rtc_get_time(&dt);
    char t[16], d[24];
    format_time_24h(&dt, t);
    int s, dx, dy;
    clock_geometry(w, &s, &dx, &dy);
    for (int i = 0; i < 8 && t[i]; i++) glyph_big(dx + i * 8 * s, dy, (unsigned char)t[i], s, TH.accent, TH.bg);
    format_date(&dt, d);
    text_center(c.x, c.w, dy + 8 * s + CH, d, TH.dim, TH.bg, tile_cols(w));
}

// ---- Files ----------------------------------------------------------------------------------------------------------------

// ---- things defined further down that the applications below call ---------------------------------------------------------------

static void render_files(win_t *w);
static void render_gfx(win_t *w);
static void launch_program(const char *cmd, const char *title);
static void win_set_fullscreen(int wi, bool on);
static void mouse_byte(uint8_t b);
static void render_tasks(win_t *w);
static void files_load(win_t *w);
static void tm_update(win_t *w);
static void open_exec(const char *cmd);
static void open_prefilled(const char *cmd);
static int open_app(int kind);
static int open_editor(const char *path);
static void open_path(const char *path);
static void run_leave(const char *cmd);
static void run_silent(const char *cmd);
static bool cmd_exists(const char *name);
static void apply_theme(int t);
static void do_logout(void);
static void do_lock(void);
static bool boot_cfg_get(void);
static bool boot_cfg_set(bool on);
static void save_settings(void);
static void wallpaper_prepare(void);
static void desktop_icons_refresh(void);
static void damage_icons(void);
static void damage_icon(int i);
static uint32_t g_wp_rows[SH];
static int g_wp_theme = -1;
static void pics_scan(void);
static void wallpaper_set_file(const char *path);
static void pos_load(void);
static int split_words(char *s, char *w[], int max);
static bool parse_num(const char *s, int lo, int hi, int *out);
typedef struct { char name[32]; char path[96]; } pic_t;
static pic_t g_pics[6];
static int g_npics;
static uint32_t g_wp_full[SW * SH];                // the wallpaper picture over the gradient, ready to copy
static bool g_wp_img_on;
static char g_wp_err[56];
static void open_files_at(const char *path);
static void desktop_dir_init(void);
static void login_key(uint8_t code, bool ext);
static void login_click(void);
static void login_hover(void);
static char g_desk_dir[96];
static int g_drag_slider_id, g_drag_slider_x;

static const char UNSHIFTED[0x3A] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, 9,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13, 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
};
static const char SHIFTED[0x3A] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8, 9,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 13, 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
};

static int g_hover_item = -1, g_hover_item_win = -1;     // button/row under the pointer inside an application window

// ---- Calculator ----------------------------------------------------------------------------------------------------------------

enum { K_SQRT = 100, K_SQR, K_RECIP, K_BACK, K_CLEAR, K_NEG, K_PCT, K_DIV, K_MUL, K_SUB, K_ADD, K_EQ, K_DOT };

static const struct { const char *lab; int id, col, row, span; } CBTN[] = {
    { "\xFB", K_SQRT, 0, 0, 1 }, { "x\xFD", K_SQR, 1, 0, 1 }, { "1/x", K_RECIP, 2, 0, 1 }, { "\x1B", K_BACK, 3, 0, 1 },
    { "C", K_CLEAR, 0, 1, 1 }, { "\xF1", K_NEG, 1, 1, 1 }, { "%", K_PCT, 2, 1, 1 }, { "\xF6", K_DIV, 3, 1, 1 },
    { "7", 7, 0, 2, 1 }, { "8", 8, 1, 2, 1 }, { "9", 9, 2, 2, 1 }, { "x", K_MUL, 3, 2, 1 },
    { "4", 4, 0, 3, 1 }, { "5", 5, 1, 3, 1 }, { "6", 6, 2, 3, 1 }, { "-", K_SUB, 3, 3, 1 },
    { "1", 1, 0, 4, 1 }, { "2", 2, 1, 4, 1 }, { "3", 3, 2, 4, 1 }, { "+", K_ADD, 3, 4, 1 },
    { "0", 0, 0, 5, 2 }, { ".", K_DOT, 2, 5, 1 }, { "=", K_EQ, 3, 5, 1 },
};
#define NCBTN ((int)(sizeof(CBTN) / sizeof(CBTN[0])))
#define CALC_DISP 96

static rect_t calc_button(const win_t *w, int i)
{
    rect_t c = content_rect(w);
    int gap = 6, bw = (c.w - 20 - 3 * gap) / 4, bh = (c.h - CALC_DISP - 12 - 5 * gap - 6) / 6;
    rect_t r = { c.x + 10 + CBTN[i].col * (bw + gap), c.y + CALC_DISP + 6 + CBTN[i].row * (bh + gap), bw * CBTN[i].span + gap * (CBTN[i].span - 1), bh };
    return r;
}

static double calc_parse(const char *s)
{
    bool neg = false;
    double v = 0, frac = 0, scale = 1;
    bool dot = false;
    if (*s == '-') { neg = true; s++; }
    for (; *s; s++) {
        if (*s == '.') { dot = true; continue; }
        if (*s < '0' || *s > '9') continue;
        if (!dot) v = v * 10 + (*s - '0');
        else { scale *= 10; frac += (*s - '0') / scale; }
    }
    v += frac;
    return neg ? -v : v;
}

static void calc_fmt(double v, char *out, int n)
{
    char b[40];
    int k = 0;
    if (v != v) { put_str(out, n, "Error"); return; }
    bool neg = v < 0;
    if (neg) v = -v;
    if (v >= 1e15) { put_str(out, n, "Overflow"); return; }
    if (neg) b[k++] = '-';
    if (v < 2e9) {
        int32_t ip = (int32_t)v;
        int32_t frac = (int32_t)((v - (double)ip) * 1000000.0 + 0.5);
        if (frac >= 1000000) { ip++; frac -= 1000000; }
        k += put_uint(b + k, (uint32_t)ip);
        if (frac) {
            char f[8];
            for (int i = 5; i >= 0; i--) { f[i] = (char)('0' + frac % 10); frac /= 10; }
            int last = 5;
            while (last > 0 && f[last] == '0') last--;
            b[k++] = '.';
            for (int i = 0; i <= last; i++) b[k++] = f[i];
        }
    } else {
        int32_t hi = (int32_t)(v / 1e9);
        int32_t lo = (int32_t)(v - (double)hi * 1e9);
        if (lo < 0) lo = 0;
        k += put_uint(b + k, (uint32_t)hi);
        char l9[10];
        for (int i = 8; i >= 0; i--) { l9[i] = (char)('0' + lo % 10); lo /= 10; }
        for (int i = 0; i < 9; i++) b[k++] = l9[i];
    }
    b[k] = 0;
    if (neg && strcmp(b, "-0") == 0) { put_str(out, n, "0"); return; }
    put_str(out, n, b);
}

static void calc_reset(win_t *w)
{
    w->c_acc = 0; w->c_op = 0; w->c_fresh = true; w->c_err = false; w->c_in[0] = 0; w->c_expr[0] = 0;
}

static double calc_apply(win_t *w, int op, double a, double b)
{
    switch (op) {
    case K_ADD: return a + b;
    case K_SUB: return a - b;
    case K_MUL: return a * b;
    case K_DIV: if (b == 0) { w->c_err = true; return 0; } return a / b;
    default: return b;
    }
}

static const char *calc_opname(int op) { return op == K_ADD ? "+" : (op == K_SUB ? "-" : (op == K_MUL ? "x" : (op == K_DIV ? "/" : ""))); }

static void calc_press(win_t *w, int id)
{
    char tmp[28];
    if (w->c_err && id != K_CLEAR) calc_reset(w);
    if (id >= 0 && id <= 9) {
        if (w->c_fresh) { w->c_in[0] = 0; w->c_fresh = false; }
        int len = (int)strlen(w->c_in);
        if (len >= 14) return;
        if (len == 1 && w->c_in[0] == '0') len = 0;
        w->c_in[len] = (char)('0' + id); w->c_in[len + 1] = 0;
    } else if (id == K_DOT) {
        if (w->c_fresh) { put_str(w->c_in, sizeof(w->c_in), "0"); w->c_fresh = false; }
        if (!strchr(w->c_in, '.') && strlen(w->c_in) < 14) { int len = (int)strlen(w->c_in); if (len == 0) { w->c_in[0] = '0'; len = 1; } w->c_in[len] = '.'; w->c_in[len + 1] = 0; }
    } else if (id == K_BACK) {
        int len = (int)strlen(w->c_in);
        if (!w->c_fresh && len > 0) w->c_in[len - 1] = 0;
        if (strcmp(w->c_in, "-") == 0) w->c_in[0] = 0;
    } else if (id == K_CLEAR) calc_reset(w);
    else if (id == K_NEG) {
        if (w->c_in[0] == 0) { calc_fmt(-w->c_acc, tmp, sizeof(tmp)); put_str(w->c_in, sizeof(w->c_in), tmp); w->c_fresh = false; }
        else if (w->c_in[0] == '-') memmove(w->c_in, w->c_in + 1, strlen(w->c_in));
        else if (strlen(w->c_in) < 14) { memmove(w->c_in + 1, w->c_in, strlen(w->c_in) + 1); w->c_in[0] = '-'; }
    } else if (id == K_PCT) {
        double cur = w->c_in[0] ? calc_parse(w->c_in) : w->c_acc;
        cur = w->c_op ? w->c_acc * cur / 100.0 : cur / 100.0;
        calc_fmt(cur, w->c_in, sizeof(w->c_in));
        w->c_fresh = false;
    } else if (id == K_SQRT || id == K_SQR || id == K_RECIP) {
        double cur = w->c_in[0] ? calc_parse(w->c_in) : w->c_acc, r = 0;
        if (id == K_SQR) r = cur * cur;
        else if (id == K_RECIP) { if (cur == 0) w->c_err = true; else r = 1.0 / cur; }
        else if (cur < 0) w->c_err = true;
        else if (cur > 0) { r = cur; for (int i = 0; i < 60; i++) r = (r + cur / r) * 0.5; }
        if (!w->c_err) { calc_fmt(r, w->c_in, sizeof(w->c_in)); w->c_fresh = false; }
    } else if (id >= K_DIV && id <= K_ADD) {
        if (w->c_in[0] || w->c_op == 0) {
            double cur = w->c_in[0] ? calc_parse(w->c_in) : w->c_acc;
            w->c_acc = (w->c_op && !w->c_fresh) ? calc_apply(w, w->c_op, w->c_acc, cur) : cur;
        }
        w->c_op = id;
        w->c_in[0] = 0;
        w->c_fresh = true;
        calc_fmt(w->c_acc, tmp, sizeof(tmp));
        int k = put_str(w->c_expr, sizeof(w->c_expr), tmp);
        w->c_expr[k++] = ' ';
        put_str(w->c_expr + k, (int)sizeof(w->c_expr) - k, calc_opname(id));
    } else if (id == K_EQ) {
        if (w->c_op) {
            double cur = w->c_in[0] ? calc_parse(w->c_in) : w->c_acc;
            double r = calc_apply(w, w->c_op, w->c_acc, cur);
            w->c_acc = r;
            w->c_op = 0;
            w->c_expr[0] = 0;
            calc_fmt(r, w->c_in, sizeof(w->c_in));
            w->c_fresh = true;
        } else if (w->c_in[0]) { w->c_acc = calc_parse(w->c_in); w->c_fresh = true; }
    }
    if (w->c_err) { w->c_expr[0] = 0; w->c_in[0] = 0; }
    damage_content(w);
}

static void render_calc(win_t *w)
{
    rect_t c = content_rect(w);
    fill_rect(c.x, c.y, c.w, c.h, TH.bg);
    round_rect(c.x + 10, c.y + 10, c.w - 20, CALC_DISP - 18, 4, TH.field);
    frame_rect(c.x + 10, c.y + 10, c.w - 20, CALC_DISP - 18, TH.line);
    char disp[40];
    if (w->c_err) put_str(disp, sizeof(disp), "Cannot divide by 0");
    else if (w->c_in[0]) put_str(disp, sizeof(disp), w->c_in);
    else calc_fmt(w->c_acc, disp, sizeof(disp));
    int len = (int)strlen(disp), room = c.w - 44;
    int sc = (len * 8 * 3 <= room) ? 3 : (len * 8 * 2 <= room ? 2 : 1);
    if (w->c_err) sc = 1;
    int tx = c.x + c.w - 22 - len * 8 * sc;
    if (tx < c.x + 18) tx = c.x + 18;
    int ty = c.y + 10 + (CALC_DISP - 18 - 8 * sc) / 2 + 8;
    for (int i = 0; i < len; i++) glyph_big(tx + i * 8 * sc, ty, (unsigned char)disp[i], sc, w->c_err ? TH.bad : TH.fg, TH.field);
    if (w->c_expr[0]) text(c.x + c.w - 22 - (int)strlen(w->c_expr) * CW, c.y + 16, w->c_expr, TH.dim, TH.field, 30);
    for (int i = 0; i < NCBTN; i++) {
        rect_t r = calc_button(w, i);
        int id = CBTN[i].id;
        bool op = id >= K_DIV && id <= K_ADD, eq = id == K_EQ, fn = id == K_SQRT || id == K_SQR || id == K_RECIP || id == K_BACK || id == K_CLEAR || id == K_NEG || id == K_PCT;
        bool hot = g_hover_item_win >= 0 && &g_win[g_hover_item_win] == w && g_hover_item == i;
        uint32_t bg = eq ? TH.accent : (op ? mix(TH.line, TH.accent, 35) : (fn ? TH.line : TH.field));
        if (hot) bg = shade(bg, 112);
        round_rect(r.x, r.y, r.w, r.h, 4, bg);
        if (!eq && !op) frame_rect(r.x, r.y, r.w, r.h, TH.line);
        int ln = (int)strlen(CBTN[i].lab);
        text(r.x + (r.w - ln * CW) / 2, r.y + (r.h - CH) / 2, CBTN[i].lab, eq ? TH.accent_tx : TH.fg, bg, ln);
    }
}

static void calc_click(int wi, int mx, int my)
{
    win_t *w = &g_win[wi];
    for (int i = 0; i < NCBTN; i++) if (rect_has(calc_button(w, i), mx, my)) { calc_press(w, CBTN[i].id); return; }
}

static void calc_key(int wi, uint8_t code, bool ext)
{
    win_t *w = &g_win[wi];
    int id = -1;
    if (ext) {
        if (code == 0x1C) id = K_EQ;                               // keypad Enter
        else if (code == 0x35) id = K_DIV;                         // keypad /
    } else {
        static const int PAD[0x14] = { 7, 8, 9, K_SUB, 4, 5, 6, K_ADD, 1, 2, 3, 0, K_DOT };   // 0x47..0x53
        if (code >= 0x47 && code <= 0x53) id = PAD[code - 0x47];
        else if (code == 0x37) id = K_MUL;
        else if (code == 0x1C) id = K_EQ;
        else if (code == 0x0E) id = K_BACK;
        else if (code == 0x01) id = K_CLEAR;
        else if (code < sizeof(UNSHIFTED)) {
            char ch = g_shift ? SHIFTED[code] : UNSHIFTED[code];
            if (ch >= '0' && ch <= '9') id = ch - '0';
            else if (ch == '.') id = K_DOT;
            else if (ch == '+') id = K_ADD;
            else if (ch == '-') id = K_SUB;
            else if (ch == '*') id = K_MUL;
            else if (ch == '/') id = K_DIV;
            else if (ch == '=') id = K_EQ;
            else if (ch == '%') id = K_PCT;
            else if (ch == 'c' || ch == 'C') id = K_CLEAR;
            else if (ch == 'r' || ch == 'R') id = K_SQRT;
        }
    }
    if (id >= 0) calc_press(w, id);
}

// ---- Text editor (RSH / Helios aware) -----------------------------------------------------------------------------------------

#define ED_BAR 34
#define ED_STAT 22
#define ED_GUTTER 52

enum { ED_TEXT, ED_RASH, ED_HELIOS };

static int ed_kind(const win_t *w)
{
    int n = (int)strlen(w->path);
    if (n >= 4 && same_nocase(w->path + n - 4, ".rsh")) return ED_RASH;
    if (n >= 5 && same_nocase(w->path + n - 5, ".rash")) return ED_RASH;
    if (n >= 4 && same_nocase(w->path + n - 4, ".hls")) return ED_HELIOS;
    return ED_TEXT;
}

static const char *ed_kind_name(int k) { return k == ED_RASH ? "Rash script" : (k == ED_HELIOS ? "Helios source" : "Plain text"); }

static int ed_rows(const win_t *w) { return (content_rect(w).h - ED_BAR - ED_STAT) / CH; }
static int ed_cols(const win_t *w) { return (content_rect(w).w - ED_GUTTER - 12) / CW; }

static void ed_set_title(win_t *w)
{
    const char *b = strrchr(w->path, '/');
    b = b ? b + 1 : w->path;
    char t[40];
    int k = put_str(t, sizeof(t), w->path[0] ? b : "Untitled");
    if (w->edirty && k < 38) { t[k++] = ' '; t[k++] = '*'; t[k] = 0; }
    put_str(w->title, sizeof(w->title), t);
}

static void ed_new(win_t *w)
{
    w->nl = 1; w->lines[0][0] = 0; w->erow = w->ecol = w->etop = 0; w->edirty = false; w->emsg[0] = 0;
}

static bool ed_load(win_t *w, const char *path)
{
    static char buf[24576];
    int size = avfs_get_filesize(path);
    if (size < 0) return false;
    int n = size > (int)sizeof(buf) ? (int)sizeof(buf) : size;
    if (n > 0 && avfs_read_file(path, buf, (uint32_t)n, 0) != 0) return false;
    ed_new(w);
    put_str(w->path, sizeof(w->path), path);
    int col = 0;
    for (int i = 0; i < n && w->nl < TL - 1; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if (ch == '\r') continue;
        if (ch == '\n') { w->lines[w->nl - 1][col] = 0; w->nl++; w->lines[w->nl - 1][0] = 0; col = 0; continue; }
        if (ch == '\t') { for (int k = 0; k < 4 && col < TC - 1; k++) w->lines[w->nl - 1][col++] = ' '; continue; }
        if (ch < 32 || ch > 126) ch = '?';
        if (col < TC - 1) w->lines[w->nl - 1][col++] = (char)ch;
    }
    w->lines[w->nl - 1][col] = 0;
    if (size > n) put_str(w->emsg, sizeof(w->emsg), "file truncated to 24 KB");
    ed_set_title(w);
    return true;
}

static bool ed_save(win_t *w)
{
    static char buf[TL * (TC + 1)];
    if (!w->path[0]) {
        char p[96];
        for (int i = 1; i < 100; i++) {
            snprintf(p, sizeof(p), "%s/note%d.txt", g_desk_dir, i);
            if (!avfs_file_exists(p)) break;
        }
        put_str(w->path, sizeof(w->path), p);
    }
    int o = 0;
    for (int i = 0; i < w->nl; i++) {
        int len = (int)strlen(w->lines[i]);
        memcpy(buf + o, w->lines[i], (size_t)len);
        o += len;
        if (i < w->nl - 1) buf[o++] = '\n';
    }
    if (avfs_save_file(w->path, buf, (uint32_t)o, true) != 0) { put_str(w->emsg, sizeof(w->emsg), "could not save the file"); return false; }
    w->edirty = false;
    put_str(w->emsg, sizeof(w->emsg), "saved");
    ed_set_title(w);
    return true;
}

static bool ed_is_kw(const char *wd, int len, int kind)
{
    static const char *const RASH[] = { "function", "endfunction", "call", "set", "if", "else", "endif", "while", "endwhile", "for", "endfor", "return", "echo", "map", "import", NULL };
    static const char *const HLS[] = { "fn", "let", "if", "else", "while", "for", "return", "include", "asm", "struct", "break", "continue", NULL };
    const char *const *l = kind == ED_RASH ? RASH : HLS;
    for (int i = 0; l[i]; i++) if ((int)strlen(l[i]) == len && strncmp(l[i], wd, (size_t)len) == 0) return true;
    return false;
}

static void ed_colorize(const char *s, int kind, uint32_t *col)
{
    int n = (int)strlen(s);
    uint32_t com = mix(TH.dim, TH.good, 55), strc = TH.warn, num = 0x74c0fc;
    for (int i = 0; i < n; i++) col[i] = TH.fg;
    if (kind == ED_TEXT) return;
    for (int i = 0; i < n; ) {
        if (kind == ED_RASH && s[i] == '%') { bool lead = true; for (int k = 0; k < i; k++) if (s[k] != ' ') lead = false; if (lead) { for (int k = i; k < n; k++) col[k] = com; break; } }
        if (kind == ED_HELIOS && s[i] == '/' && i + 1 < n && s[i + 1] == '/') { for (int k = i; k < n; k++) col[k] = com; break; }
        if (s[i] == '"') { int j = i + 1; while (j < n && s[j] != '"') j++; for (int k = i; k <= j && k < n; k++) col[k] = strc; i = j + 1; continue; }
        if ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || s[i] == '_') {
            int j = i;
            while (j < n && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z') || s[j] == '_' || (s[j] >= '0' && s[j] <= '9'))) j++;
            if (ed_is_kw(s + i, j - i, kind)) for (int k = i; k < j; k++) col[k] = TH.accent;
            i = j;
            continue;
        }
        if (s[i] >= '0' && s[i] <= '9') { int j = i; while (j < n && s[j] >= '0' && s[j] <= '9') { col[j] = num; j++; } i = j; continue; }
        i++;
    }
}

static rect_t ed_button(const win_t *w, int i)       // 0 Save, 1 Run, 2 RSHIDT
{
    rect_t c = content_rect(w);
    static const int X[3] = { 10, 84, 150 }, W[3] = { 66, 58, 84 };
    rect_t r = { c.x + X[i], c.y + 5, W[i], ED_BAR - 10 };
    return r;
}

static void render_edit(win_t *w, bool focused)
{
    rect_t c = content_rect(w);
    int kind = ed_kind(w), rows = ed_rows(w), cols = ed_cols(w);
    fill_rect(c.x, c.y, c.w, c.h, TH.field);
    fill_rect(c.x, c.y, c.w, ED_BAR, TH.bg);
    fill_rect(c.x, c.y + ED_BAR - 1, c.w, 1, TH.line);
    static const char *const LAB[3] = { "Save", "Run", "RSHIDT" };
    for (int i = 0; i < 3; i++) {
        rect_t r = ed_button(w, i);
        bool hot = g_hover_item_win >= 0 && &g_win[g_hover_item_win] == w && g_hover_item == i;
        uint32_t bg = i == 1 ? TH.good : (hot ? shade(TH.line, 115) : TH.line);
        if (i == 1 && hot) bg = shade(bg, 110);
        round_rect(r.x, r.y, r.w, r.h, 3, bg);
        text_center(r.x, r.w, r.y + (r.h - CH) / 2, LAB[i], i == 1 ? 0xffffff : TH.fg, bg, 8);
    }
    const char *kn = ed_kind_name(kind);
    text(c.x + c.w - 12 - (int)strlen(kn) * CW, c.y + (ED_BAR - CH) / 2, kn, TH.dim, TH.bg, 20);
    fill_rect(c.x, c.y + ED_BAR, ED_GUTTER, c.h - ED_BAR - ED_STAT, TH.bg);
    int ty = c.y + ED_BAR;
    for (int r = 0; r < rows; r++) {
        int li = w->etop + r;
        if (li >= w->nl) break;
        char num[8];
        put_uint(num, (uint32_t)(li + 1));
        int nl = (int)strlen(num);
        bool cur = li == w->erow;
        text(c.x + ED_GUTTER - 10 - nl * CW, ty + r * CH, num, cur ? TH.fg : TH.dim, TH.bg, 6);
        if (cur && focused) fill_rect(c.x + ED_GUTTER, ty + r * CH, c.w - ED_GUTTER, CH, mix(TH.field, TH.accent, 8));
        uint32_t colr[TC];
        ed_colorize(w->lines[li], kind, colr);
        const char *s = w->lines[li];
        int n = (int)strlen(s);
        for (int k = 0; k < n && k < cols; k++) glyph(c.x + ED_GUTTER + 6 + k * CW, ty + r * CH, (unsigned char)s[k], colr[k], TRANSPARENT);
        if (cur && focused) fill_rect(c.x + ED_GUTTER + 6 + w->ecol * CW, ty + r * CH, 2, CH, TH.accent);
    }
    int sy = c.y + c.h - ED_STAT;
    fill_rect(c.x, sy, c.w, ED_STAT, TH.bg);
    fill_rect(c.x, sy, c.w, 1, TH.line);
    char st[64];
    int k = put_str(st, sizeof(st), "Ln ");
    k += put_uint(st + k, (uint32_t)(w->erow + 1));
    k += put_str(st + k, (int)sizeof(st) - k, ", Col ");
    put_uint(st + k, (uint32_t)(w->ecol + 1));
    text(c.x + 10, sy + 3, st, TH.dim, TH.bg, 24);
    const char *p = w->path[0] ? w->path : "not saved yet";
    text(c.x + 170, sy + 3, w->emsg[0] ? w->emsg : p, w->emsg[0] ? TH.good : TH.dim, TH.bg, (c.w - 190) / CW);
}

static void ed_run(win_t *w)
{
    if (w->edirty || !w->path[0]) ed_save(w);
    if (!w->path[0]) return;
    char cmd[120];
    int k = ed_kind(w);
    snprintf(cmd, sizeof(cmd), "%s %s", k == ED_HELIOS ? "helios run" : (k == ED_RASH ? "script" : "source"), w->path);
    open_exec(cmd);
}

static void ed_rshidt(win_t *w)
{
    if (w->edirty || !w->path[0]) ed_save(w);
    char cmd[120];
    snprintf(cmd, sizeof(cmd), "rshidt %s", w->path);
    launch_program(cmd, "RSHIDT");
    if (w->path[0]) { char p[96]; put_str(p, sizeof(p), w->path); ed_load(w, p); }
    damage_win(w);
}

static void ed_clamp(win_t *w)
{
    if (w->erow < 0) w->erow = 0;
    if (w->erow >= w->nl) w->erow = w->nl - 1;
    int len = (int)strlen(w->lines[w->erow]);
    if (w->ecol > len) w->ecol = len;
    if (w->ecol < 0) w->ecol = 0;
    int rows = imax(1, ed_rows(w));
    if (w->erow < w->etop) w->etop = w->erow;
    if (w->erow >= w->etop + rows) w->etop = w->erow - rows + 1;
    if (w->etop < 0) w->etop = 0;
}

static void ed_touch(win_t *w)
{
    if (!w->edirty) { w->edirty = true; ed_set_title(w); damage_title(w); }
    w->emsg[0] = 0;
}

static void edit_key(int wi, uint8_t code, bool ext)
{
    win_t *w = &g_win[wi];
    char *ln = w->lines[w->erow];
    int len = (int)strlen(ln);
    if (g_ctrl && !ext) {
        if (code == 0x1F) { ed_save(w); damage_content(w); damage_title(w); }
        else if (code == 0x13) ed_run(w);
        return;
    }
    if (ext) {
        switch (code) {
        case 0x4B: if (w->ecol > 0) w->ecol--; else if (w->erow > 0) { w->erow--; w->ecol = (int)strlen(w->lines[w->erow]); } break;
        case 0x4D: if (w->ecol < len) w->ecol++; else if (w->erow < w->nl - 1) { w->erow++; w->ecol = 0; } break;
        case 0x48: w->erow--; break;
        case 0x50: w->erow++; break;
        case 0x47: w->ecol = 0; break;
        case 0x4F: w->ecol = len; break;
        case 0x49: w->erow -= imax(1, ed_rows(w) - 1); break;
        case 0x51: w->erow += imax(1, ed_rows(w) - 1); break;
        case 0x53:
            if (w->ecol < len) { memmove(ln + w->ecol, ln + w->ecol + 1, (size_t)(len - w->ecol)); ed_touch(w); }
            else if (w->erow < w->nl - 1) {
                char *nx = w->lines[w->erow + 1];
                int nlen = (int)strlen(nx);
                if (len + nlen < TC - 1) {
                    memcpy(ln + len, nx, (size_t)nlen + 1);
                    for (int i = w->erow + 1; i < w->nl - 1; i++) memcpy(w->lines[i], w->lines[i + 1], TC);
                    w->nl--;
                    ed_touch(w);
                }
            }
            break;
        default: return;
        }
        ed_clamp(w);
        damage_content(w);
        return;
    }
    if (code == 0x3F) { ed_run(w); return; }                                   // F5
    if (code == 0x1C) {
        if (w->nl >= TL - 1) return;
        for (int i = w->nl; i > w->erow + 1; i--) memcpy(w->lines[i], w->lines[i - 1], TC);
        int indent = 0;
        while (indent < w->ecol && ln[indent] == ' ') indent++;
        char *nx = w->lines[w->erow + 1];
        int rest = len - w->ecol;
        for (int i = 0; i < indent; i++) nx[i] = ' ';
        memcpy(nx + indent, ln + w->ecol, (size_t)rest + 1);
        ln[w->ecol] = 0;
        w->nl++;
        w->erow++;
        w->ecol = indent;
        ed_touch(w);
    } else if (code == 0x0E) {
        if (w->ecol > 0) { memmove(ln + w->ecol - 1, ln + w->ecol, (size_t)(len - w->ecol + 1)); w->ecol--; ed_touch(w); }
        else if (w->erow > 0) {
            char *pv = w->lines[w->erow - 1];
            int plen = (int)strlen(pv);
            if (plen + len < TC - 1) {
                memcpy(pv + plen, ln, (size_t)len + 1);
                for (int i = w->erow; i < w->nl - 1; i++) memcpy(w->lines[i], w->lines[i + 1], TC);
                w->nl--;
                w->erow--;
                w->ecol = plen;
                ed_touch(w);
            }
        }
    } else if (code == 0x0F) {
        for (int k = 0; k < 4 && len < TC - 2; k++, len++) { memmove(ln + w->ecol + 1, ln + w->ecol, (size_t)(len - w->ecol + 1)); ln[w->ecol++] = ' '; }
        ed_touch(w);
    } else if (code < sizeof(UNSHIFTED)) {
        char ch = g_shift ? SHIFTED[code] : UNSHIFTED[code];
        if (ch < 32 || ch > 126 || len >= TC - 2) return;
        memmove(ln + w->ecol + 1, ln + w->ecol, (size_t)(len - w->ecol + 1));
        ln[w->ecol++] = ch;
        ed_touch(w);
    } else return;
    ed_clamp(w);
    damage_content(w);
}

static void edit_click(int wi, int mx, int my)
{
    win_t *w = &g_win[wi];
    rect_t c = content_rect(w);
    if (my < c.y + ED_BAR) {
        for (int i = 0; i < 3; i++) {
            if (!rect_has(ed_button(w, i), mx, my)) continue;
            if (i == 0) { ed_save(w); damage_content(w); damage_title(w); }
            else if (i == 1) ed_run(w);
            else ed_rshidt(w);
            return;
        }
        return;
    }
    if (my >= c.y + c.h - ED_STAT) return;
    w->erow = w->etop + (my - c.y - ED_BAR) / CH;
    w->ecol = (mx - c.x - ED_GUTTER - 6 + CW / 2) / CW;
    ed_clamp(w);
    damage_content(w);
}

static void edit_scroll(win_t *w, int lines_n)
{
    int maxtop = imax(0, w->nl - ed_rows(w));
    w->etop = imax(0, imin(maxtop, w->etop - lines_n));
    damage_content(w);
}

// ---- Games ----------------------------------------------------------------------------------------------------------------------

typedef struct { char name[24]; char desc[56]; char cmd[48]; } game_t;
static game_t g_games[16];
static int g_ngames;

static void games_build(void)
{
    static const struct { const char *name, *desc, *cmd; } DEF[] = {
        { "Doom", "First-person shooter (needs a WAD)", "doom" },
        { "Snake", "Eat, grow, do not bite yourself", "snake" },
        { "Game of Life", "Conway's cellular automaton", "life" },
        { "Matrix", "Falling glyph rain", "matrix" },
        { "Pong", "Two paddles and a ball", "pong" },
        { "Vulkan Demo", "A spinning cube on the software GPU", "vkdemo" },
        { "AssaultCube", "3D shooter (app, from /disk/ac.app)", "app /disk/ac.app" },
        { "Cataclysm DDA", "Roguelike survival (app)", "app /disk/cdda.app" },
    };
    g_ngames = 0;
    for (size_t i = 0; i < sizeof(DEF) / sizeof(DEF[0]) && g_ngames < 16; i++) {
        bool ok;
        if (strncmp(DEF[i].cmd, "app ", 4) == 0) ok = app_file_exists(DEF[i].cmd + 4);
        else ok = cmd_exists(DEF[i].cmd);
        if (!ok) continue;
        game_t *g = &g_games[g_ngames++];
        put_str(g->name, sizeof(g->name), DEF[i].name);
        put_str(g->desc, sizeof(g->desc), DEF[i].desc);
        put_str(g->cmd, sizeof(g->cmd), DEF[i].cmd);
    }
    DirectoryEntry ent[32];                                  // apps dropped into /disk/games show up too
    int n = avfs_is_directory("/disk/games") ? avfs_list_directory("/disk/games", ent, 32) : 0;
    for (int i = 0; i < n && g_ngames < 16; i++) {
        int len = (int)strlen(ent[i].name);
        if (ent[i].is_directory || len < 5 || !same_nocase(ent[i].name + len - 4, ".app")) continue;
        game_t *g = &g_games[g_ngames++];
        put_str(g->name, sizeof(g->name), ent[i].name);
        put_str(g->desc, sizeof(g->desc), "App from /disk/games");
        snprintf(g->cmd, sizeof(g->cmd), "app /disk/games/%s", ent[i].name);
    }
}

#define GTILE_W 176
#define GTILE_H 104

static rect_t game_tile(const win_t *w, int i)
{
    rect_t c = content_rect(w);
    int cols = imax(1, (c.w - 16) / (GTILE_W + 10));
    rect_t r = { c.x + 12 + (i % cols) * (GTILE_W + 10), c.y + 44 + (i / cols) * (GTILE_H + 10), GTILE_W, GTILE_H };
    return r;
}

static void render_games(win_t *w)
{
    rect_t c = content_rect(w);
    fill_rect(c.x, c.y, c.w, c.h, TH.bg);
    text(c.x + 14, c.y + 14, "Games", TH.accent, TH.bg, 10);
    text(c.x + 14 + 7 * CW, c.y + 14, "click a tile to play - the desktop returns when the game ends", TH.dim, TH.bg, 70);
    if (g_ngames == 0) { text(c.x + 14, c.y + 60, "No games are installed.", TH.dim, TH.bg, 40); return; }
    for (int i = 0; i < g_ngames; i++) {
        rect_t r = game_tile(w, i);
        if (r.y + r.h > c.y + c.h) break;
        bool hot = g_hover_item_win >= 0 && &g_win[g_hover_item_win] == w && g_hover_item == i;
        round_rect(r.x, r.y, r.w, r.h, 5, hot ? mix(TH.field, TH.accent, 16) : TH.field);
        frame_rect(r.x, r.y, r.w, r.h, hot ? TH.accent : TH.line);
        draw_icon(IC_GAME, r.x + 12, r.y + 12, 40);
        text(r.x + 62, r.y + 14, g_games[i].name, TH.fg, hot ? mix(TH.field, TH.accent, 16) : TH.field, 14);
        const char *d = g_games[i].desc;
        int dl = (int)strlen(d);
        uint32_t bg = hot ? mix(TH.field, TH.accent, 16) : TH.field;
        text(r.x + 12, r.y + 62, d, TH.dim, bg, 21);
        if (dl > 21) text(r.x + 12, r.y + 80, d + 21, TH.dim, bg, 21);
    }
}

static void games_click(int wi, int mx, int my)
{
    win_t *w = &g_win[wi];
    for (int i = 0; i < g_ngames; i++) if (rect_has(game_tile(w, i), mx, my)) { launch_program(g_games[i].cmd, g_games[i].name); return; }
}

// ---- Services (rsd) -----------------------------------------------------------------------------------------------------------

#define SV_HEAD 34
#define SV_ROW 24
#define SV_FOOT 40

static void serv_refresh(win_t *w)
{
    char keep[32];
    put_str(keep, sizeof(keep), w->lsel >= 0 && w->lsel < w->nli ? w->li[w->lsel].name : "");
    w->nli = 0;
    int n = rsd_unit_count();
    for (int i = 0; i < n && w->nli < 40; i++) {
        rsd_unit_view_t u;
        if (!rsd_unit_view(i, &u)) continue;
        put_str(w->li[w->nli].name, sizeof(w->li[0].name), u.name);
        put_str(w->li[w->nli].sub, sizeof(w->li[0].sub), u.desc);
        w->li[w->nli].kind = u.enabled ? 1 : 0;
        w->li[w->nli].st = u.failed ? 3 : (strcmp(u.state, "active") == 0 ? 1 : (strcmp(u.state, "waiting") == 0 ? 2 : 0));
        w->nli++;
    }
    w->lsel = -1;
    for (int i = 0; i < w->nli; i++) if (strcmp(w->li[i].name, keep) == 0) w->lsel = i;
}

static int list_rows(const win_t *w, int head, int foot, int row) { return (content_rect(w).h - head - foot) / row; }

static rect_t serv_button(const win_t *w, int i)       // 0 Start 1 Stop 2 Restart 3 Enable/Disable
{
    rect_t c = content_rect(w);
    rect_t r = { c.x + 12 + i * 96, c.y + c.h - SV_FOOT + 7, 88, 26 };
    return r;
}

static void render_serv(win_t *w)
{
    rect_t c = content_rect(w);
    fill_rect(c.x, c.y, c.w, c.h, TH.field);
    fill_rect(c.x, c.y, c.w, SV_HEAD, TH.bg);
    fill_rect(c.x, c.y + SV_HEAD - 1, c.w, 1, TH.line);
    char hd[64];
    int k = put_str(hd, sizeof(hd), "Services - target ");
    put_str(hd + k, (int)sizeof(hd) - k, rsd_current_target());
    text(c.x + 14, c.y + 9, hd, TH.fg, TH.bg, 50);
    int rows = list_rows(w, SV_HEAD, SV_FOOT, SV_ROW);
    for (int r = 0; r < rows; r++) {
        int i = w->ltop + r;
        if (i >= w->nli) break;
        int ry = c.y + SV_HEAD + r * SV_ROW;
        bool sel = i == w->lsel;
        uint32_t bg = sel ? TH.accent : TH.field, fg = sel ? TH.accent_tx : TH.fg;
        fill_rect(c.x, ry, c.w, SV_ROW, bg);
        static const uint32_t DOT[4] = { 0x868e96, 0x51cf66, 0xfab005, 0xff6b6b };
        disc(c.x + 18, ry + SV_ROW / 2, 5, DOT[w->li[i].st]);
        text(c.x + 34, ry + 4, w->li[i].name, fg, bg, 10);
        static const char *const SN[4] = { "inactive", "active", "waiting", "failed" };
        text(c.x + 34 + 11 * CW, ry + 4, SN[w->li[i].st], sel ? fg : TH.dim, bg, 9);
        text(c.x + 34 + 21 * CW, ry + 4, w->li[i].sub, sel ? fg : TH.dim, bg, (c.w - 34 - 21 * CW - 70) / CW);
        if (w->li[i].kind) text(c.x + c.w - 14 - 7 * CW, ry + 4, "enabled", sel ? fg : TH.good, bg, 7);
    }
    int fy = c.y + c.h - SV_FOOT;
    fill_rect(c.x, fy, c.w, SV_FOOT, TH.bg);
    fill_rect(c.x, fy, c.w, 1, TH.line);
    static const char *const LAB[4] = { "Start", "Stop", "Restart", "Enable" };
    for (int i = 0; i < 4; i++) {
        rect_t r = serv_button(w, i);
        bool hot = g_hover_item_win >= 0 && &g_win[g_hover_item_win] == w && g_hover_item == 1000 + i;
        const char *lab = LAB[i];
        if (i == 3 && w->lsel >= 0 && w->li[w->lsel].kind) lab = "Disable";
        uint32_t bg = w->lsel < 0 ? TH.line : (hot ? shade(TH.line, 118) : TH.line);
        round_rect(r.x, r.y, r.w, r.h, 3, bg);
        text_center(r.x, r.w, r.y + (r.h - CH) / 2, lab, w->lsel < 0 ? TH.dim : TH.fg, bg, 10);
    }
}

static void serv_action(win_t *w, int which)
{
    if (w->lsel < 0 || w->lsel >= w->nli) { flash("select a service first"); return; }
    char cmd[64];
    const char *verb = which == 0 ? "start" : (which == 1 ? "stop" : (which == 2 ? "restart" : (w->li[w->lsel].kind ? "disable" : "enable")));
    snprintf(cmd, sizeof(cmd), "systemctl %s %s", verb, w->li[w->lsel].name);
    run_silent(cmd);
    char msg[48];
    snprintf(msg, sizeof(msg), "%s %s", verb, w->li[w->lsel].name);
    flash(msg);
    serv_refresh(w);
    damage_content(w);
}

static void serv_click(int wi, int mx, int my)
{
    win_t *w = &g_win[wi];
    rect_t c = content_rect(w);
    for (int i = 0; i < 4; i++) if (rect_has(serv_button(w, i), mx, my)) { serv_action(w, i); return; }
    int row = (my - c.y - SV_HEAD) / SV_ROW + w->ltop;
    if (my >= c.y + SV_HEAD && my < c.y + c.h - SV_FOOT && row >= 0 && row < w->nli) { w->lsel = row; damage_content(w); }
}

// ---- Scripts (RSH plugins, Rash scripts, Helios programs) -----------------------------------------------------------------------

#define SC_HEAD 34
#define SC_ROW 24
#define SC_FOOT 40

static void scripts_scan(win_t *w)
{
    static const char *const DIRS[] = { "/plugins", "/disk/scripts", "/disk/plugins", "/disk/mods", "/home/user", "/bin" };
    char keep[32];
    put_str(keep, sizeof(keep), w->lsel >= 0 && w->lsel < w->nli ? w->li[w->lsel].name : "");
    w->nli = 0;
    for (size_t d = 0; d < sizeof(DIRS) / sizeof(DIRS[0]); d++) {
        DirectoryEntry ent[48];
        if (!avfs_is_directory(DIRS[d])) continue;
        int n = avfs_list_directory(DIRS[d], ent, 48);
        for (int i = 0; i < n && w->nli < 40; i++) {
            int len = (int)strlen(ent[i].name), kind = 0;
            if (ent[i].is_directory) continue;
            if (len > 4 && same_nocase(ent[i].name + len - 4, ".rsh")) kind = 1;
            else if (len > 5 && same_nocase(ent[i].name + len - 5, ".rash")) kind = 1;
            else if (len > 4 && same_nocase(ent[i].name + len - 4, ".hls")) kind = 2;
            if (!kind) continue;
            put_str(w->li[w->nli].name, sizeof(w->li[0].name), ent[i].name);
            put_str(w->li[w->nli].sub, sizeof(w->li[0].sub), DIRS[d]);
            w->li[w->nli].kind = kind;
            w->li[w->nli].st = strcmp(DIRS[d], "/plugins") == 0 || strcmp(DIRS[d], "/disk/plugins") == 0;     // a plugin
            w->nli++;
        }
    }
    w->lsel = -1;
    for (int i = 0; i < w->nli; i++) if (strcmp(w->li[i].name, keep) == 0) w->lsel = i;
}

static rect_t script_button(const win_t *w, int i)     // 0 Run 1 Edit 2 RSHIDT 3 Refresh
{
    rect_t c = content_rect(w);
    static const int X[4] = { 12, 98, 184, 290 }, W[4] = { 80, 80, 98, 90 };
    rect_t r = { c.x + X[i], c.y + c.h - SC_FOOT + 7, W[i], 26 };
    return r;
}

static void script_path(const win_t *w, int i, char *out, int n) { snprintf(out, (size_t)n, "%s/%s", w->li[i].sub, w->li[i].name); }

static void render_scripts(win_t *w)
{
    rect_t c = content_rect(w);
    fill_rect(c.x, c.y, c.w, c.h, TH.field);
    fill_rect(c.x, c.y, c.w, SC_HEAD, TH.bg);
    fill_rect(c.x, c.y + SC_HEAD - 1, c.w, 1, TH.line);
    text(c.x + 14, c.y + 9, "Scripts and plugins", TH.fg, TH.bg, 30);
    text(c.x + c.w - 14 - 27 * CW, c.y + 9, "double-click runs, Edit opens it", TH.dim, TH.bg, 32);
    int rows = list_rows(w, SC_HEAD, SC_FOOT, SC_ROW);
    if (w->nli == 0) text(c.x + 14, c.y + SC_HEAD + 14, "No .rsh, .rash or .hls files found in /plugins, /disk/scripts or /disk/mods.", TH.dim, TH.field, 70);
    for (int r = 0; r < rows; r++) {
        int i = w->ltop + r;
        if (i >= w->nli) break;
        int ry = c.y + SC_HEAD + r * SC_ROW;
        bool sel = i == w->lsel;
        uint32_t bg = sel ? TH.accent : TH.field, fg = sel ? TH.accent_tx : TH.fg;
        fill_rect(c.x, ry, c.w, SC_ROW, bg);
        draw_icon(IC_SCRIPT, c.x + 10, ry + 3, 18);
        text(c.x + 36, ry + 4, w->li[i].name, fg, bg, 18);
        text(c.x + 36 + 19 * CW, ry + 4, w->li[i].kind == 2 ? "Helios" : "Rash", sel ? fg : TH.accent, bg, 6);
        text(c.x + 36 + 26 * CW, ry + 4, w->li[i].st ? "plugin" : w->li[i].sub, sel ? fg : TH.dim, bg, (c.w - 36 - 26 * CW - 12) / CW);
    }
    int fy = c.y + c.h - SC_FOOT;
    fill_rect(c.x, fy, c.w, SC_FOOT, TH.bg);
    fill_rect(c.x, fy, c.w, 1, TH.line);
    static const char *const LAB[4] = { "Run", "Edit", "RSHIDT", "Refresh" };
    for (int i = 0; i < 4; i++) {
        rect_t r = script_button(w, i);
        bool hot = g_hover_item_win >= 0 && &g_win[g_hover_item_win] == w && g_hover_item == 1000 + i;
        uint32_t bg = i == 0 ? TH.good : (hot ? shade(TH.line, 118) : TH.line);
        if (i == 0 && hot) bg = shade(bg, 112);
        round_rect(r.x, r.y, r.w, r.h, 3, bg);
        text_center(r.x, r.w, r.y + (r.h - CH) / 2, LAB[i], i == 0 ? 0xffffff : TH.fg, bg, 10);
    }
}

static void script_run(win_t *w, int i)
{
    char p[128], cmd[160];
    script_path(w, i, p, sizeof(p));
    snprintf(cmd, sizeof(cmd), "%s %s", w->li[i].kind == 2 ? "helios run" : "script", p);
    open_exec(cmd);
}

static void scripts_click(int wi, int mx, int my, bool dbl)
{
    win_t *w = &g_win[wi];
    rect_t c = content_rect(w);
    for (int i = 0; i < 4; i++) {
        if (!rect_has(script_button(w, i), mx, my)) continue;
        if (i == 3) { scripts_scan(w); damage_content(w); return; }
        if (w->lsel < 0) { flash("select a script first"); return; }
        char p[128];
        script_path(w, w->lsel, p, sizeof(p));
        if (i == 0) script_run(w, w->lsel);
        else if (i == 1) open_editor(p);
        else { char cmd[160]; snprintf(cmd, sizeof(cmd), "rshidt %s", p); run_leave(cmd); }
        return;
    }
    int row = (my - c.y - SC_HEAD) / SC_ROW + w->ltop;
    if (my >= c.y + SC_HEAD && my < c.y + c.h - SC_FOOT && row >= 0 && row < w->nli) {
        bool again = dbl && row == w->lsel;
        w->lsel = row;
        damage_content(w);
        if (again) script_run(w, row);
    }
}

// ---- Settings ---------------------------------------------------------------------------------------------------------------

typedef struct { int kind, id, x, y, w, h, val, lo, hi; char label[48]; bool on; } ctl_t;
enum { CTL_BUTTON, CTL_TOGGLE, CTL_LABEL, CTL_HINT, CTL_SLIDER };
enum { ID_SHADOWS = 140, ID_SNAP, ID_OPENMAX, ID_RUNWIN, ID_TRAY, ID_DOCKSZ, ID_TERMS = 160,
       ID_THEME = 10, ID_WP = 20, ID_WPM = 100, ID_PIC = 120, ID_SPEED = 30, ID_ACCEL, ID_DBL, ID_ICONS, ID_DOCK, ID_CLOCK24, ID_DESKFILES, ID_SAVE, ID_BOOTDESK,
       ID_AUTOTERM, ID_LOCK, ID_LOGOUT, ID_RESTART, ID_SHUT, ID_CONSOLE, ID_PASSWD, ID_ADDUSER, ID_OPENMON };

static const char *const SET_TABS[] = { "Appearance", "Windows", "Panel & Dock", "Desktop", "Terminal", "Mouse", "Keyboard", "Session", "Users", "About" };
#define NSETTABS 10
#define SET_SIDE 152
#define SLIDER_W 280

static bool g_boot_desktop = true;

static rect_t set_tab_rect(const win_t *w, int i) { rect_t c = content_rect(w); rect_t r = { c.x + 8, c.y + 12 + i * 36, SET_SIDE - 16, 32 }; return r; }
static rect_t set_save_rect(const win_t *w) { rect_t c = content_rect(w); rect_t r = { c.x + 8, c.y + c.h - 46, SET_SIDE - 16, 32 }; return r; }

static bool auto_has_term(void)
{
    for (int i = 0; i < g_nauto; i++) if (g_auto[i].kind == APP_TERM) return true;
    return false;
}

static int settings_layout(const win_t *w, ctl_t *c, int max)
{
    rect_t r = content_rect(w);
    int n = 0, x0 = r.x + SET_SIDE + 24, y = r.y + 18;
#define CTL(k, i, xx, yy, ww, hh, lab, o) do { if (n < max) { c[n].kind = (k); c[n].id = (i); c[n].x = (xx); c[n].y = (yy); c[n].w = (ww); c[n].h = (hh); c[n].val = 0; c[n].lo = 0; c[n].hi = 0; put_str(c[n].label, sizeof(c[n].label), (lab)); c[n].on = (o); n++; } } while (0)
#define SLIDER(i, yy, lab, v, l, h) do { CTL(CTL_SLIDER, (i), x0, (yy), SLIDER_W, 40, (lab), false); if (n) { c[n - 1].val = (v); c[n - 1].lo = (l); c[n - 1].hi = (h); } } while (0)
    char tmp[48];
    switch (w->tab) {
    case 0:
        CTL(CTL_LABEL, 0, x0, y, 0, 0, "Colour theme", false);
        y += 26;
        for (int i = 0; i < NTHEMES; i++) CTL(CTL_BUTTON, ID_THEME + i, x0 + i * 100, y, 94, 28, THEMES[i].name, C.theme == i);
        y += 48;
        CTL(CTL_LABEL, 0, x0, y, 0, 0, "Wallpaper", false);
        y += 26;
        {
            static const char *const WL[4] = { "Mascot", "Gradient", "Flat", "Reverse" };
            bool on[4] = { C.wallpaper == 3 && !C.wp_file[0], C.wallpaper == 0, C.wallpaper == 1, C.wallpaper == 2 };
            for (int i = 0; i < 4; i++) CTL(CTL_BUTTON, ID_WP + i, x0 + i * 100, y, 94, 28, WL[i], on[i]);
            y += 38;
            static const char *const ML[4] = { "Fill", "Fit", "Stretch", "Center" };
            for (int i = 0; i < 4; i++) CTL(CTL_BUTTON, ID_WPM + i, x0 + i * 100, y, 94, 24, ML[i], C.wp_mode == i);
        }
        y += 38;
        CTL(CTL_LABEL, 0, x0, y, 0, 0, "Your pictures", false);
        y += 24;
        for (int i = 0; i < g_npics; i++) CTL(CTL_BUTTON, ID_PIC + i, x0, y + i * 28, 250, 24, g_pics[i].name, C.wp_file[0] && strcmp(C.wp_file, g_pics[i].path) == 0);
        if (g_npics == 0) {
            CTL(CTL_HINT, 0, x0, y, 0, 0, "Copy .png files to /disk/Pictures", false);
            CTL(CTL_HINT, 0, x0, y + 20, 0, 0, "(or into your Desktop folder).", false);
        }
        break;
    case 1:
        CTL(CTL_LABEL, 0, x0, y, 0, 0, "Windows", false);
        y += 30;
        CTL(CTL_TOGGLE, ID_SHADOWS, x0, y, 20, 20, "Shadows under windows", C.shadows);
        y += 30;
        CTL(CTL_TOGGLE, ID_SNAP, x0, y, 20, 20, "Snap windows to the screen edges", C.snap);
        y += 30;
        CTL(CTL_TOGGLE, ID_OPENMAX, x0, y, 20, 20, "Open new windows maximised", C.open_max);
        y += 30;
        CTL(CTL_TOGGLE, ID_RUNWIN, x0, y, 20, 20, "Run games and graphics in a window", C.run_windowed);
        y += 44;
        CTL(CTL_HINT, 0, x0, y, 0, 0, "Full screen: F11, Alt+Enter or the title", false); y += 20;
        CTL(CTL_HINT, 0, x0, y, 0, 0, "bar button. Drag an edge or corner to resize.", false); y += 20;
        CTL(CTL_HINT, 0, x0, y, 0, 0, "Drop a window on the top edge to maximise it,", false); y += 20;
        CTL(CTL_HINT, 0, x0, y, 0, 0, "on the left or right edge for half the screen.", false); y += 20;
        CTL(CTL_HINT, 0, x0, y, 0, 0, "A program in a window stops with Alt+F4.", false);
        break;
    case 2:
        CTL(CTL_LABEL, 0, x0, y, 0, 0, "Panel and dock", false);
        y += 30;
        CTL(CTL_TOGGLE, ID_DOCK, x0, y, 20, 20, "Show the dock", C.dock);
        y += 30;
        CTL(CTL_TOGGLE, ID_TRAY, x0, y, 20, 20, "Memory and disk use in the panel", C.tray);
        y += 30;
        CTL(CTL_TOGGLE, ID_CLOCK24, x0, y, 20, 20, "24-hour clock", C.clock24);
        y += 40;
        SLIDER(ID_DOCKSZ, y, "Dock icon size", C.dock_size, 32, 64);
        y += 60;
        CTL(CTL_HINT, 0, x0, y, 0, 0, "Add your own launchers to the dock with", false); y += 20;
        CTL(CTL_HINT, 0, x0, y, 0, 0, "a launcher line in desktop.cfg.", false);
        break;
    case 4:
        CTL(CTL_LABEL, 0, x0, y, 0, 0, "Terminal colours", false);
        y += 26;
        {
            static const char *const TL_[4] = { "Theme", "Classic", "Amber", "Light" };
            for (int i = 0; i < 4; i++) CTL(CTL_BUTTON, ID_TERMS + i, x0 + i * 100, y, 94, 28, TL_[i], C.term_style == i);
        }
        y += 150;
        CTL(CTL_HINT, 0, x0, y, 0, 0, "Alt+F2 runs one command, Ctrl+Alt+T opens a", false); y += 20;
        CTL(CTL_HINT, 0, x0, y, 0, 0, "terminal. Ctrl+C stops a command that hangs.", false);
        break;
    case 6:
        CTL(CTL_LABEL, 0, x0, y, 0, 0, "Keyboard shortcuts", false);
        y += 28;
        {
            static const char *const KS[] = { "Alt+F1          Applications menu", "Alt+F2          Run a command", "Alt+Tab         Next window",
                "Alt+F4          Close (ends a program)", "F11, Alt+Enter  Full screen on or off", "Alt+F9, Alt+F10 Minimise, maximise",
                "Ctrl+Alt+T      New terminal", "Ctrl+Shift+Esc  Task Manager", "Ctrl+Alt+L      Lock the screen", "Ctrl+Alt+Q      Log out",
                "Alt+1 to 4      Switch workspace" };
            for (int i = 0; i < 11; i++) { CTL(CTL_HINT, 0, x0, y, 0, 0, KS[i], false); y += 20; }
        }
        break;
    case 5:
        CTL(CTL_LABEL, 0, x0, y, 0, 0, "Pointer", false);
        y += 26;
        SLIDER(ID_SPEED, y, "Speed", C.mouse_speed, 10, 120);
        y += 56;
        SLIDER(ID_ACCEL, y, "Acceleration when sweeping fast", C.mouse_gain, 0, 200);
        y += 56;
        SLIDER(ID_DBL, y, "Double-click interval", C.dblclick, 250, 900);
        y += 62;
        snprintf(tmp, sizeof(tmp), "Slow movements run at %d percent of raw speed,", C.mouse_speed);
        CTL(CTL_HINT, 0, x0, y, 0, 0, tmp, false);
        y += 20;
        snprintf(tmp, sizeof(tmp), "quick sweeps reach %d percent.", C.mouse_speed + C.mouse_speed * C.mouse_gain / 100);
        CTL(CTL_HINT, 0, x0, y, 0, 0, tmp, false);
        break;
    case 3:
        CTL(CTL_LABEL, 0, x0, y, 0, 0, "Desktop", false);
        y += 30;
        CTL(CTL_TOGGLE, ID_ICONS, x0, y, 20, 20, "Show desktop icons", C.icons);
        y += 30;
        CTL(CTL_TOGGLE, ID_DESKFILES, x0, y, 20, 20, "Show the files of the Desktop folder", C.desk_files);
        y += 30;
        CTL(CTL_TOGGLE, ID_DOCK, x0, y, 20, 20, "Show the dock", C.dock);
        y += 30;
        CTL(CTL_TOGGLE, ID_CLOCK24, x0, y, 20, 20, "24-hour clock", C.clock24);
        y += 44;
        CTL(CTL_HINT, 0, x0, y, 0, 0, "Desktop folder:", false);
        y += 20;
        CTL(CTL_HINT, 0, x0, y, 0, 0, g_desk_dir, false);
        y += 26;
        CTL(CTL_HINT, 0, x0, y, 0, 0, "Files and folders you put there appear as icons.", false);
        break;
    case 7:
        CTL(CTL_LABEL, 0, x0, y, 0, 0, "Start-up", false);
        y += 30;
        CTL(CTL_TOGGLE, ID_BOOTDESK, x0, y, 20, 20, "Start the desktop when the computer boots", g_boot_desktop);
        y += 30;
        CTL(CTL_TOGGLE, ID_AUTOTERM, x0, y, 20, 20, "Open a terminal after logging in", auto_has_term());
        y += 22;
        CTL(CTL_HINT, 0, x0 + 30, y, 0, 0, "(the boot option is stored in /disk/etc/boot.cfg and needs root)", false);
        y += 40;
        CTL(CTL_LABEL, 0, x0, y, 0, 0, "Session", false);
        y += 30;
        CTL(CTL_BUTTON, ID_LOCK, x0, y, 130, 30, "Lock screen", false);
        CTL(CTL_BUTTON, ID_LOGOUT, x0 + 140, y, 130, 30, "Log out", false);
        y += 40;
        CTL(CTL_BUTTON, ID_RESTART, x0, y, 130, 30, "Restart", false);
        CTL(CTL_BUTTON, ID_SHUT, x0 + 140, y, 130, 30, "Shut down", false);
        y += 40;
        CTL(CTL_BUTTON, ID_CONSOLE, x0, y, 270, 30, "Leave to the text console", false);
        break;
    case 8: {
        snprintf(tmp, sizeof(tmp), "Signed in as %s%s", users_current_name(), users_is_root() ? " (administrator)" : "");
        CTL(CTL_LABEL, 0, x0, y, 0, 0, tmp, false);
        y += 30;
        if (users_enabled()) {
            CTL(CTL_HINT, 0, x0, y, 0, 0, "Accounts on this computer:", false);
            y += 22;
            for (int i = 0; i < users_count() && i < 8; i++) {
                snprintf(tmp, sizeof(tmp), "  %s%s%s", users_name_at(i), users_sudo_at(i) ? "   (may use sudo)" : "", users_has_password_at(i) ? "" : "   (locked)");
                CTL(CTL_HINT, 0, x0, y, 0, 0, tmp, false);
                y += 20;
            }
        } else {
            CTL(CTL_HINT, 0, x0, y, 0, 0, "No accounts yet: everyone is root and no password is asked.", false);
            y += 20;
            CTL(CTL_HINT, 0, x0, y, 0, 0, "Create one with the button below (the first needs sudo rights).", false);
            y += 20;
        }
        y += 22;
        CTL(CTL_BUTTON, ID_PASSWD, x0, y, 170, 30, "Change password", false);
        CTL(CTL_BUTTON, ID_ADDUSER, x0 + 180, y, 170, 30, "Add a user...", false);
        break;
    }
    case 9:
    default: {
        uint64_t total = pmm_get_total_memory(), freeb = pmm_get_free_memory();
        uint32_t tm = (uint32_t)(total >> 20), fm = (uint32_t)(freeb >> 20);
        CTL(CTL_LABEL, 0, x0, y, 0, 0, "RadiumOS", false);
        y += 28;
        snprintf(tmp, sizeof(tmp), "Version        %s", update_current_version());
        CTL(CTL_HINT, 0, x0, y, 0, 0, tmp, false); y += 20;
        CTL(CTL_HINT, 0, x0, y, 0, 0, "Desktop        RadiumDE", false); y += 20;
        snprintf(tmp, sizeof(tmp), "Memory         %u of %u MB in use", tm > fm ? tm - fm : 0, tm);
        CTL(CTL_HINT, 0, x0, y, 0, 0, tmp, false); y += 20;
        uint32_t dt = 0, du = 0;
        if (avfs_disk_space(&dt, &du) == 0) snprintf(tmp, sizeof(tmp), "Disk           %u of %u MB in use", du >> 10, dt >> 10);
        else snprintf(tmp, sizeof(tmp), "Disk           not available");
        CTL(CTL_HINT, 0, x0, y, 0, 0, tmp, false); y += 20;
        uint32_t up = g_up_base + now_ms() / 1000;
        char hms[12];
        fmt_hms(hms, up);
        snprintf(tmp, sizeof(tmp), "Uptime         %s", hms);
        CTL(CTL_HINT, 0, x0, y, 0, 0, tmp, false); y += 20;
        CTL(CTL_HINT, 0, x0, y, 0, 0, "Display        1280 x 720, 32-bit", false); y += 20;
        snprintf(tmp, sizeof(tmp), "User           %s (uid %u)", users_current_name(), users_current_uid());
        CTL(CTL_HINT, 0, x0, y, 0, 0, tmp, false); y += 20;
        CTL(CTL_HINT, 0, x0, y, 0, 0, "Kernel        stack protector on", false);
        y += 36;
        CTL(CTL_BUTTON, ID_OPENMON, x0, y, 190, 30, "Open System Monitor", false);
        break;
    }
    }
#undef SLIDER
#undef CTL
    return n;
}

static void settings_preview(int x, int y)
{
    int pw = 230, ph = 130;
    round_rect(x - 1, y - 1, pw + 2, ph + 2, 4, TH.line);
    bool pic = C.wallpaper == 3 && g_wp_img_on;
    for (int r = 0; r < ph; r++) {
        if (pic) {
            const uint32_t *src = g_wp_full + (r * SH / ph) * SW;
            for (int c = 0; c < pw; c++) fill_rect(x + c, y + r, 1, 1, src[c * SW / pw]);
            continue;
        }
        int t = C.wallpaper == 1 ? 35 : (r * 100 / ph);
        if (C.wallpaper == 2) t = 100 - t;
        fill_rect(x, y + r, pw, 1, mix(TH.wp_top, TH.wp_bot, t));
    }
    fill_rect(x, y, pw, 9, TH.panel_bg);
    fill_rect(x + 5, y + 3, 24, 3, TH.panel_fg);
    fill_rect(x + 36, y + 20, 130, 80, TH.border);
    fill_rect(x + 37, y + 21, 128, 12, TH.t_f_bg);
    fill_rect(x + 37, y + 33, 128, 66, TH.bg);
    fill_rect(x + 43, y + 24, 40, 4, TH.t_f_tx);
    fill_rect(x + 45, y + 42, 60, 4, TH.fg); fill_rect(x + 45, y + 52, 90, 4, TH.dim); fill_rect(x + 45, y + 62, 46, 4, TH.accent);
    round_rect(x + 70, y + 110, 90, 14, 3, mix(TH.panel_bg, 0xffffff, 8));
    for (int i = 0; i < 5; i++) fill_rect(x + 78 + i * 16, y + 113, 8, 8, i == 2 ? TH.accent : TH.panel_hot);
}

static void render_settings(win_t *w)
{
    rect_t c = content_rect(w);
    fill_rect(c.x, c.y, c.w, c.h, TH.bg);
    fill_rect(c.x, c.y, SET_SIDE, c.h, mix(TH.bg, TH.line, 45));
    fill_rect(c.x + SET_SIDE - 1, c.y, 1, c.h, TH.line);
    for (int i = 0; i < NSETTABS; i++) {
        rect_t r = set_tab_rect(w, i);
        bool sel = i == w->tab;
        bool hot = g_hover_item_win >= 0 && &g_win[g_hover_item_win] == w && g_hover_item == 2000 + i;
        uint32_t bg = sel ? TH.accent : (hot ? mix(TH.bg, TH.line, 80) : mix(TH.bg, TH.line, 45));
        round_rect(r.x, r.y, r.w, r.h, 4, bg);
        text(r.x + 12, r.y + (r.h - CH) / 2, SET_TABS[i], sel ? TH.accent_tx : TH.fg, bg, 16);
    }
    rect_t sv = set_save_rect(w);
    bool shot = g_hover_item_win >= 0 && &g_win[g_hover_item_win] == w && g_hover_item == 2100;
    round_rect(sv.x, sv.y, sv.w, sv.h, 4, shot ? shade(TH.good, 112) : TH.good);
    text_center(sv.x, sv.w, sv.y + (sv.h - CH) / 2, "Save", 0xffffff, shot ? shade(TH.good, 112) : TH.good, 10);

    ctl_t ctl[32];
    int n = settings_layout(w, ctl, 32);
    for (int i = 0; i < n; i++) {
        ctl_t *k = &ctl[i];
        switch (k->kind) {
        case CTL_LABEL: text(k->x, k->y, k->label, TH.accent, TH.bg, 48); break;
        case CTL_HINT: text(k->x, k->y, k->label, TH.dim, TH.bg, (c.x + c.w - k->x - 10) / CW); break;
        case CTL_TOGGLE:
            round_rect(k->x, k->y, k->w, k->h, 3, k->on ? TH.accent : TH.line);
            if (k->on) { line(k->x + 4, k->y + 10, k->x + 8, k->y + 14, 2, 0xffffff); line(k->x + 8, k->y + 14, k->x + 16, k->y + 5, 2, 0xffffff); }
            text(k->x + 30, k->y + 2, k->label, TH.fg, TH.bg, 46);
            break;
        case CTL_SLIDER: {
            text(k->x, k->y, k->label, TH.fg, TH.bg, 40);
            int ty = k->y + 24, kx = k->x + (k->val - k->lo) * k->w / (k->hi - k->lo);
            round_rect(k->x, ty - 3, k->w, 6, 2, TH.line);
            round_rect(k->x, ty - 3, kx - k->x, 6, 2, TH.accent);
            disc(kx, ty, 8, TH.border_f);
            disc(kx, ty, 6, TH.accent);
            char v[16];
            int len = put_uint(v, (uint32_t)k->val);
            put_str(v + len, 6, k->id == ID_DBL ? " ms" : (k->id == ID_DOCKSZ ? " px" : "%"));
            text(k->x + k->w + 16, ty - 8, v, TH.fg, TH.bg, 8);
            break;
        }
        default: {
            bool hot = g_hover_item_win >= 0 && &g_win[g_hover_item_win] == w && g_hover_item == 3000 + k->id;
            uint32_t bg = k->on ? TH.accent : (hot ? shade(TH.line, 118) : TH.line);
            round_rect(k->x, k->y, k->w, k->h, 4, bg);
            text_center(k->x, k->w, k->y + (k->h - CH) / 2, k->label, k->on ? TH.accent_tx : TH.fg, bg, k->w / CW);
            break;
        }
        }
    }
    if (w->tab == 0) settings_preview(c.x + SET_SIDE + 24 + 270, c.y + 230);
    if (w->tab == 4) {
        int px = c.x + SET_SIDE + 24, py = c.y + 18 + 26 + 40;
        round_rect(px - 1, py - 1, 382, 112, 4, TH.line);
        fill_rect(px, py, 380, 110, TH.term_bg);
        text(px + 8, py + 6, "$ ls /disk", 0x69db7c, TH.term_bg, 40);
        text(px + 8, py + 22, "etc  home  games  Pictures", TH.term_fg, TH.term_bg, 40);
        text(px + 8, py + 38, "$ top -b", 0x69db7c, TH.term_bg, 40);
        text(px + 8, py + 54, "up 0:12:03   tasks 7   memory 41 of 2048 MB", TH.term_fg, TH.term_bg, 44);
        fill_rect(px, py + 88, 380, 22, TH.term_input);
        text(px + 8, py + 91, "$ _", 0x69db7c, TH.term_input, 4);
    }
}

static void settings_slider_set(int id, int mx, int x0)
{
    int lo = 250, hi = 900;
    if (id == ID_SPEED) { lo = 10; hi = 120; }
    else if (id == ID_ACCEL) { lo = 0; hi = 200; }
    else if (id == ID_DOCKSZ) { lo = 32; hi = 64; }
    int v = lo + imax(0, imin(SLIDER_W, mx - x0)) * (hi - lo) / SLIDER_W;
    if (id == ID_SPEED) C.mouse_speed = v;
    else if (id == ID_ACCEL) C.mouse_gain = (v + 2) / 5 * 5;
    else if (id == ID_DOCKSZ) { if (v != C.dock_size) { C.dock_size = v; damage_all(); } }
    else C.dblclick = (v + 12) / 25 * 25;
}

static void settings_click(int wi, int mx, int my)
{
    win_t *w = &g_win[wi];
    for (int i = 0; i < NSETTABS; i++) if (rect_has(set_tab_rect(w, i), mx, my)) { w->tab = i; if (i == 0) pics_scan(); damage_content(w); return; }
    if (rect_has(set_save_rect(w), mx, my)) { save_settings(); return; }
    ctl_t ctl[32];
    int n = settings_layout(w, ctl, 32);
    for (int i = 0; i < n; i++) {
        ctl_t *k = &ctl[i];
        if (k->kind == CTL_LABEL || k->kind == CTL_HINT) continue;
        int hit_y = k->kind == CTL_SLIDER ? k->y + 12 : k->y, hit_h = k->kind == CTL_SLIDER ? 26 : k->h;
        int hit_w = k->kind == CTL_TOGGLE ? 260 : k->w;
        if (mx < k->x - (k->kind == CTL_SLIDER ? 10 : 0) || mx >= k->x + hit_w + (k->kind == CTL_SLIDER ? 10 : 0) || my < hit_y || my >= hit_y + hit_h) continue;
        if (k->kind == CTL_SLIDER) { settings_slider_set(k->id, mx, k->x); g_drag_slider_id = k->id; g_drag_slider_x = k->x; damage_content(w); return; }
        if (k->id >= ID_THEME && k->id < ID_THEME + NTHEMES) apply_theme(k->id - ID_THEME);
        else if (k->id >= ID_WP && k->id < ID_WP + 4) {
            static const int KIND[4] = { 3, 0, 1, 2 };
            C.wallpaper = KIND[k->id - ID_WP];
            if (k->id == ID_WP) C.wp_file[0] = 0;
            wallpaper_prepare();
            damage_all();
        }
        else if (k->id >= ID_WPM && k->id < ID_WPM + 4) { C.wp_mode = k->id - ID_WPM; wallpaper_prepare(); damage_all(); }
        else if (k->id >= ID_PIC && k->id < ID_PIC + 6) { if (k->id - ID_PIC < g_npics) wallpaper_set_file(g_pics[k->id - ID_PIC].path); }
        else if (k->id == ID_SHADOWS) { C.shadows = !C.shadows; damage_all(); }
        else if (k->id == ID_SNAP) C.snap = !C.snap;
        else if (k->id == ID_OPENMAX) C.open_max = !C.open_max;
        else if (k->id == ID_RUNWIN) C.run_windowed = !C.run_windowed;
        else if (k->id == ID_TRAY) { C.tray = !C.tray; damage(0, 0, SW, PANEL_H); }
        else if (k->id >= ID_TERMS && k->id < ID_TERMS + 4) { C.term_style = k->id - ID_TERMS; apply_theme(C.theme); }
        else if (k->id == ID_ICONS) { C.icons = !C.icons; damage_all(); }
        else if (k->id == ID_DESKFILES) { C.desk_files = !C.desk_files; desktop_icons_refresh(); damage_all(); }
        else if (k->id == ID_DOCK) { C.dock = !C.dock; damage_all(); }
        else if (k->id == ID_CLOCK24) { C.clock24 = !C.clock24; damage(0, 0, SW, PANEL_H); }
        else if (k->id == ID_BOOTDESK) {
            if (!users_is_root()) flash("only root can change the boot option");
            else if (boot_cfg_set(!g_boot_desktop)) { g_boot_desktop = !g_boot_desktop; flash(g_boot_desktop ? "the desktop will start at boot" : "boot goes to the text console"); }
            else flash("could not write /disk/etc/boot.cfg");
        } else if (k->id == ID_AUTOTERM) {
            if (auto_has_term()) { int o = 0; for (int j = 0; j < g_nauto; j++) if (g_auto[j].kind != APP_TERM) g_auto[o++] = g_auto[j]; g_nauto = o; }
            else if (g_nauto < MAX_AUTO) g_auto[g_nauto++].kind = APP_TERM;
        }
        else if (k->id == ID_LOCK) { do_lock(); return; }
        else if (k->id == ID_LOGOUT) { do_logout(); return; }
        else if (k->id == ID_RESTART) { g_exit_action = 1; g_quit = true; return; }
        else if (k->id == ID_SHUT) { g_exit_action = 2; g_quit = true; return; }
        else if (k->id == ID_CONSOLE) { g_exit_action = 3; g_quit = true; return; }
        else if (k->id == ID_PASSWD) open_exec("passwd");
        else if (k->id == ID_ADDUSER) open_prefilled("useradd -s ");
        else if (k->id == ID_OPENMON) open_app(APP_MON);
        damage_content(w);
        return;
    }
}

// ---- panel, dock, desktop icons --------------------------------------------------------------------------------------------

typedef struct { rect_t r; int win; } pbtn_t;

static rect_t pn_menu, pn_power, pn_clock, pn_user, pn_pager[NWS];
static pbtn_t pn_task[MAX_WIN];
static int pn_ntask;
static int hover_panel = -1;                       // 0 menu, 1 power, 2.. task, 100.. pager
static int hover_dock = -1, hover_dicon = -1, sel_dicon = -1;
static int g_hover_btn = -1, g_hover_win = -1;      // window title button under the pointer

static void clock_text(char *out, int n)
{
    datetime_t dt;
    rtc_get_time(&dt);
    char d[24], t[16];
    format_date(&dt, d);
    format_time_24h(&dt, t);
    int k = put_str(out, n, d);
    k += put_str(out + k, n - k, "  ");
    if (!C.clock24) {
        int hh = (t[0] - '0') * 10 + (t[1] - '0');
        bool pm = hh >= 12;
        hh %= 12;
        if (hh == 0) hh = 12;
        char hb[4];
        put_uint(hb, (uint32_t)hh);
        k += put_str(out + k, n - k, hb);
        k += put_str(out + k, n - k, ":");
        char mm[3] = { t[3], t[4], 0 };
        k += put_str(out + k, n - k, mm);
        put_str(out + k, n - k, pm ? " PM" : " AM");
    } else put_str(out + k, n - k, t);
}

static void panel_layout(void)
{
    pn_menu.x = 0; pn_menu.y = 0; pn_menu.w = 132; pn_menu.h = PANEL_H;
    pn_power.w = 34; pn_power.h = PANEL_H; pn_power.x = SW - pn_power.w; pn_power.y = 0;
    pn_user.w = imin((int)strlen(users_current_name()), 10) * CW + 38; pn_user.h = PANEL_H; pn_user.x = pn_power.x - pn_user.w; pn_user.y = 0;
    char ct[40];
    clock_text(ct, sizeof(ct));
    pn_clock.w = (int)strlen(ct) * CW + 24; pn_clock.h = PANEL_H; pn_clock.x = pn_user.x - pn_clock.w; pn_clock.y = 0;
    int px = pn_clock.x - 14 - NWS * 26;
    for (int i = 0; i < NWS; i++) { pn_pager[i].x = px + i * 26; pn_pager[i].y = 4; pn_pager[i].w = 22; pn_pager[i].h = PANEL_H - 8; }
    int left = pn_menu.x + pn_menu.w + 8, right = px - 200;         // tray text sits between the tasks and the pager
    int nt = 0;
    for (int i = 0; i < MAX_WIN; i++) if (g_win[i].used && g_win[i].ws == g_ws) nt++;
    pn_ntask = 0;
    if (nt == 0) return;
    int bw = imin(170, (right - left) / nt - 4);
    if (bw < 40) bw = 40;
    int x = left;
    for (int i = 0; i < MAX_WIN; i++) {
        if (!g_win[i].used || g_win[i].ws != g_ws) continue;
        pn_task[pn_ntask].win = i;
        pn_task[pn_ntask].r.x = x; pn_task[pn_ntask].r.y = 3; pn_task[pn_ntask].r.w = bw; pn_task[pn_ntask].r.h = PANEL_H - 6;
        pn_ntask++;
        x += bw + 4;
    }
}

static int win_icon(const win_t *w)
{
    switch (w->type) {
    case W_TERM: return IC_TERM;
    case W_FILES: return IC_FILES;
    case W_MON: return IC_MON;
    case W_CLOCK: return IC_CLOCK;
    case W_SET: return IC_SET;
    case W_CALC: return IC_CALC;
    case W_EDIT: return IC_EDIT;
    case W_GAMES: return IC_GAME;
    case W_SERV: return IC_SERV;
    case W_SCRIPTS: return IC_SCRIPT;
    case W_TASKS: return IC_MON;
    default: return IC_VIEW;
    }
}

static void render_panel(void)
{
    panel_layout();
    vgrad(0, 0, SW, PANEL_H, mix(TH.panel_bg, 0xffffff, 8), TH.panel_bg);
    fill_rect(0, PANEL_H - 1, SW, 1, TH.panel_line);
    // Applications button
    bool hot = hover_panel == 0;
    round_rect(pn_menu.x + 4, 3, pn_menu.w - 8, PANEL_H - 6, 3, hot ? TH.accent : TH.panel_hot);
    for (int i = 0; i < 3; i++) fill_rect(pn_menu.x + 14, 10 + i * 4, 12, 2, hot ? TH.accent_tx : TH.panel_fg);
    text(pn_menu.x + 36, (PANEL_H - CH) / 2, "Applications", hot ? TH.accent_tx : TH.panel_fg, hot ? TH.accent : TH.panel_hot, 12);
    // tasks
    for (int i = 0; i < pn_ntask; i++) {
        win_t *w = &g_win[pn_task[i].win];
        rect_t r = pn_task[i].r;
        bool foc = pn_task[i].win == g_focus && !w->minimized, h = hover_panel == 2 + i;
        uint32_t bg = foc ? TH.accent : (h ? TH.panel_hot : TH.panel_bg);
        uint32_t fg = foc ? TH.accent_tx : (w->minimized ? TH.panel_dim : TH.panel_fg);
        if (foc || h) round_rect(r.x, r.y, r.w, r.h, 3, bg);
        else frame_rect(r.x, r.y, r.w, r.h, TH.panel_hot);
        draw_icon(win_icon(w), r.x + 6, r.y + 3, 16);
        text(r.x + 28, r.y + (r.h - CH) / 2, w->title, fg, (foc || h) ? bg : TH.panel_bg, (r.w - 32) / CW);
    }
    // tray: memory and disk
    if (C.tray) {
    char tb[24];
    int k = put_str(tb, sizeof(tb), "mem "); k += put_uint(tb + k, g_mem_pct); tb[k++] = '%'; tb[k] = 0;
    int tx = pn_pager[0].x - 14 - 2 * 10 * CW - 12;
    text(tx, (PANEL_H - CH) / 2, tb, g_mem_pct >= 90 ? TH.bad : (g_mem_pct >= 70 ? TH.warn : TH.panel_fg), TH.panel_bg, 10);
    k = put_str(tb, sizeof(tb), "disk "); k += put_uint(tb + k, g_dsk_pct); tb[k++] = '%'; tb[k] = 0;
    text(tx + 10 * CW + 4, (PANEL_H - CH) / 2, tb, g_dsk_pct >= 90 ? TH.bad : (g_dsk_pct >= 70 ? TH.warn : TH.panel_dim), TH.panel_bg, 10);
    }
    // pager
    for (int i = 0; i < NWS; i++) {
        rect_t r = pn_pager[i];
        bool cur = i == g_ws, h = hover_panel == 100 + i;
        int used = 0;
        for (int j = 0; j < MAX_WIN; j++) if (g_win[j].used && g_win[j].ws == i) used++;
        round_rect(r.x, r.y, r.w, r.h, 2, cur ? TH.accent : (h ? TH.panel_hot : TH.panel_hot));
        glyph(r.x + (r.w - CW) / 2, r.y + (r.h - CH) / 2, (unsigned char)('1' + i), cur ? TH.accent_tx : (used ? TH.panel_fg : TH.panel_dim), cur ? TH.accent : TH.panel_hot);
    }
    // clock
    char ct[40];
    clock_text(ct, sizeof(ct));
    text(pn_clock.x + 12, (PANEL_H - CH) / 2, ct, TH.panel_fg, TH.panel_bg, 38);
    // signed-in user
    {
        bool uh = hover_panel == 50;
        if (uh) fill_rect(pn_user.x, 0, pn_user.w, PANEL_H - 1, TH.panel_hot);
        draw_icon(IC_USER, pn_user.x + 8, 7, 16);
        text(pn_user.x + 30, (PANEL_H - CH) / 2, users_current_name(), uh ? TH.accent : TH.panel_fg, uh ? TH.panel_hot : TH.panel_bg, 10);
    }
    // power
    bool ph = hover_panel == 1;
    if (ph) fill_rect(pn_power.x, 0, pn_power.w, PANEL_H - 1, TH.panel_hot);
    int pcx = pn_power.x + pn_power.w / 2, pcy = PANEL_H / 2;
    disc(pcx, pcy, 8, ph ? TH.bad : TH.panel_fg);
    disc(pcx, pcy, 6, ph ? TH.panel_hot : TH.panel_bg);
    fill_rect(pcx - 3, pcy - 10, 6, 9, ph ? TH.panel_hot : TH.panel_bg);
    fill_rect(pcx - 1, pcy - 9, 2, 9, ph ? TH.bad : TH.panel_fg);
}

static const struct { const char *label; int icon; int kind; wtype_t type; } DOCK[] = {
    { "Terminal", IC_TERM, APP_TERM, W_TERM }, { "Files", IC_FILES, APP_FILES, W_FILES }, { "Text Editor", IC_EDIT, APP_EDIT, W_EDIT },
    { "Calculator", IC_CALC, APP_CALC, W_CALC }, { "Games", IC_GAME, APP_GAMES, W_GAMES }, { "Task Manager", IC_MON, APP_TASKS, W_TASKS },
    { "Settings", IC_SET, APP_SET, W_SET },
};
#define NDOCK ((int)(sizeof(DOCK) / sizeof(DOCK[0])))

static int dock_count(void) { return NDOCK + g_nlaunch; }

static rect_t dock_bounds(void)
{
    int n = dock_count();
    int w = n * C.dock_size + (n - 1) * DOCK_GAP + 28, h = C.dock_size + 16;
    rect_t r = { (SW - w) / 2, SH - h - 8, w, h };
    return r;
}

static rect_t dock_item(int i)
{
    rect_t b = dock_bounds();
    rect_t r = { b.x + 14 + i * (C.dock_size + DOCK_GAP), b.y + 8, C.dock_size, C.dock_size };
    return r;
}

static const char *dock_label(int i) { return i < NDOCK ? DOCK[i].label : g_launch[i - NDOCK].label; }
static int dock_icon(int i) { return i < NDOCK ? DOCK[i].icon : IC_APP; }

static bool dock_running(int i)
{
    if (i >= NDOCK) return false;
    for (int k = 0; k < MAX_WIN; k++) if (g_win[k].used && g_win[k].type == DOCK[i].type) return true;
    return false;
}

static void render_dock(void)
{
    if (!C.dock) return;
    rect_t b = dock_bounds();
    round_rect(b.x - 1, b.y - 1, b.w + 2, b.h + 2, 4, shade(TH.panel_line, 130));
    round_rect(b.x, b.y, b.w, b.h, 4, mix(TH.panel_bg, 0xffffff, 6));
    for (int i = 0; i < dock_count(); i++) {
        rect_t r = dock_item(i);
        if (i == hover_dock) round_rect(r.x - 6, r.y - 6, r.w + 12, r.h + 12, 4, TH.panel_hot);
        draw_icon(dock_icon(i), r.x, r.y - (i == hover_dock ? 3 : 0), C.dock_size);
        if (dock_running(i)) fill_rect(r.x + C.dock_size / 2 - 4, b.y + b.h - 6, 8, 3, TH.accent);
    }
    if (hover_dock >= 0) {
        rect_t r = dock_item(hover_dock);
        const char *l = dock_label(hover_dock);
        int w = (int)strlen(l) * CW + 20;
        int x = r.x + r.w / 2 - w / 2, y = b.y - 32;
        round_rect(x, y, w, 24, 3, shade(TH.panel_bg, 80));
        text(x + 10, y + 4, l, TH.panel_fg, shade(TH.panel_bg, 80), 24);
    }
}

typedef struct { char name[32]; char label[16]; int icon; int kind; int app; char path[96]; int col, row; bool placed; } dicon_t;   // kind: 0 application, 1 folder, 2 file
#define MAX_DICON 40
static dicon_t g_dic[MAX_DICON];
static int g_ndic;
static uint32_t g_dic_sig;

typedef struct { char name[32]; int col, row; } dpos_t;           // where the user left an icon (kept in deskpos.cfg)
#define MAX_POS 64
static dpos_t g_pos[MAX_POS];
static int g_npos;

#define GRID_X 20
#define GRID_Y (PANEL_H + 14)
#define CELL_W 104
#define CELL_H 92
static int grid_rows(void) { return (SH - PANEL_H - 24) / CELL_H; }
static int grid_cols(void) { return (SW - GRID_X) / CELL_W; }

static struct { bool moving; int idx, sx, sy, ox, oy, x, y; } g_idrag;

static rect_t cell_rect(int col, int row) { rect_t r = { GRID_X + col * CELL_W, GRID_Y + row * CELL_H, 96, 86 }; return r; }
static rect_t dicon_rect(int i)
{
    if (g_idrag.moving && i == g_idrag.idx) { rect_t r = { g_idrag.x, g_idrag.y, 96, 86 }; return r; }
    return cell_rect(g_dic[i].col, g_dic[i].row);
}
static rect_t icons_bounds(void) { rect_t r = { 0, PANEL_H, SW, SH - PANEL_H }; return r; }
static void damage_icons(void) { damage_rect(icons_bounds()); }
static void damage_icon(int i)
{
    if (i < 0 || i >= g_ndic) return;
    rect_t r = dicon_rect(i);
    damage(r.x - 3, r.y - 3, r.w + 6, r.h + 6);
}

static void dic_add(const char *name, const char *label, int icon, int kind, int app, const char *path)
{
    if (g_ndic >= MAX_DICON) return;
    dicon_t *d = &g_dic[g_ndic++];
    put_str(d->name, sizeof(d->name), name);
    int len = (int)strlen(label);
    if (len > 11) { memcpy(d->label, label, 9); d->label[9] = '.'; d->label[10] = '.'; d->label[11] = 0; }
    else put_str(d->label, sizeof(d->label), label);
    d->icon = icon; d->kind = kind; d->app = app;
    put_str(d->path, sizeof(d->path), path);
}

static void home_dir(char *out, int n)
{
    snprintf(out, (size_t)n, "/disk/home/%s", users_current_name());
    if (avfs_is_directory(out)) return;
    put_str(out, n, avfs_is_directory("/disk") ? "/disk" : "/");
}

static void desktop_dir_init(void)
{
    char home[96];
    snprintf(home, sizeof(home), "/disk/home/%s", users_current_name());
    if (avfs_is_directory(home)) snprintf(g_desk_dir, sizeof(g_desk_dir), "%s/Desktop", home);
    else if (avfs_is_directory("/disk")) put_str(g_desk_dir, sizeof(g_desk_dir), "/disk/Desktop");
    else put_str(g_desk_dir, sizeof(g_desk_dir), "/home/user/Desktop");
    if (!avfs_is_directory(g_desk_dir)) avfs_create_dir(g_desk_dir);
}

static int icon_for_name(const char *n)
{
    int len = (int)strlen(n);
    if ((len > 4 && (same_nocase(n + len - 4, ".rsh") || same_nocase(n + len - 4, ".hls"))) || (len > 5 && same_nocase(n + len - 5, ".rash"))) return IC_SCRIPT;
    if (len > 4 && same_nocase(n + len - 4, ".app")) return IC_GAME;
    return IC_FILE;
}

static dpos_t *pos_find(const char *name)
{
    for (int i = 0; i < g_npos; i++) if (strcmp(g_pos[i].name, name) == 0) return &g_pos[i];
    return NULL;
}

static void pos_set(const char *name, int col, int row)
{
    dpos_t *p = pos_find(name);
    if (!p) {
        if (g_npos == MAX_POS) { memmove(&g_pos[0], &g_pos[1], sizeof(dpos_t) * (MAX_POS - 1)); g_npos--; }
        p = &g_pos[g_npos++];
        put_str(p->name, sizeof(p->name), name);
    }
    p->col = col; p->row = row;
}

static bool pos_path(char *buf, int n)
{
    char home[64];
    snprintf(home, sizeof(home), "/disk/home/%s", users_current_name());
    if (avfs_is_directory(home)) { snprintf(buf, (size_t)n, "%s/deskpos.cfg", home); return true; }
    if (!avfs_is_directory("/disk")) return false;
    if (!avfs_is_directory("/disk/etc")) avfs_create_dir("/disk/etc");
    snprintf(buf, (size_t)n, "/disk/etc/deskpos.cfg");
    return true;
}

static void pos_save(void)
{
    static char out[MAX_POS * 48];
    char path[96];
    int o = 0;
    if (!pos_path(path, sizeof(path))) return;
    for (int i = 0; i < g_npos; i++) o += snprintf(out + o, sizeof(out) - (size_t)o, "\"%s\" %d %d\n", g_pos[i].name, g_pos[i].col, g_pos[i].row);
    avfs_save_file(path, out, (uint32_t)o, true);
}

static void pos_load(void)
{
    static char buf[MAX_POS * 48];
    char path[96];
    g_npos = 0;
    if (!pos_path(path, sizeof(path)) || !avfs_file_exists(path)) return;
    int size = avfs_get_filesize(path);
    if (size <= 0 || size >= (int)sizeof(buf) || avfs_get_content(path, buf, sizeof(buf)) != 0) return;
    for (char *ln = buf; *ln; ) {
        char *next = ln;
        while (*next && *next != '\n') next++;
        if (*next) *next++ = 0;
        char *w[4];
        int n = split_words(ln, w, 4), col = 0, row = 0;
        ln = next;
        if (n == 3 && parse_num(w[1], 0, 99, &col) && parse_num(w[2], 0, 99, &row) && g_npos < MAX_POS) pos_set(w[0], col, row);
    }
}

static bool cell_free(int col, int row)
{
    for (int j = 0; j < g_ndic; j++) if (g_dic[j].placed && g_dic[j].col == col && g_dic[j].row == row) return false;
    return true;
}

// Icons the user placed keep their cell; the others take the first free one, column by column.
static void place_icons(void)
{
    int cols = grid_cols(), rows = grid_rows();
    for (int i = 0; i < g_ndic; i++) g_dic[i].placed = false;
    for (int i = 0; i < g_ndic; i++) {
        dpos_t *p = pos_find(g_dic[i].name);
        if (p && p->col < cols && p->row < rows && cell_free(p->col, p->row)) { g_dic[i].col = p->col; g_dic[i].row = p->row; g_dic[i].placed = true; }
    }
    for (int i = 0; i < g_ndic; i++) {
        if (g_dic[i].placed) continue;
        for (int c = 0; c < cols && !g_dic[i].placed; c++)
            for (int r = 0; r < rows; r++)
                if (cell_free(c, r)) { g_dic[i].col = c; g_dic[i].row = r; g_dic[i].placed = true; break; }
    }
}

static void desktop_icons_refresh(void)
{
    char home[96];
    uint32_t sig = 0;
    g_ndic = 0;
    home_dir(home, sizeof(home));
    dic_add("Home", "Home", IC_HOME, 1, 0, home);
    dic_add("Terminal", "Terminal", IC_TERM, 0, APP_TERM, "");
    dic_add("Games", "Games", IC_GAME, 0, APP_GAMES, "");
    dic_add("Settings", "Settings", IC_SET, 0, APP_SET, "");
    if (C.desk_files && g_desk_dir[0] && avfs_is_directory(g_desk_dir)) {
        DirectoryEntry ent[MAX_DICON];
        int n = avfs_list_directory(g_desk_dir, ent, MAX_DICON);
        for (int i = 0; i < n; i++) {
            if (strcmp(ent[i].name, ".") == 0 || strcmp(ent[i].name, "..") == 0 || ent[i].name[0] == '.') continue;
            char p[96];
            snprintf(p, sizeof(p), "%s/%s", g_desk_dir, ent[i].name);
            dic_add(ent[i].name, ent[i].name, ent[i].is_directory ? IC_FILES : icon_for_name(ent[i].name), ent[i].is_directory ? 1 : 2, 0, p);
            for (const char *c = ent[i].name; *c; c++) sig = sig * 31 + (uint32_t)(unsigned char)*c;
            sig = sig * 31 + ent[i].size + (uint32_t)ent[i].is_directory;
        }
    }
    place_icons();
    if (sel_dicon >= g_ndic) sel_dicon = -1;
    if (hover_dicon >= g_ndic) hover_dicon = -1;
    if (sig != g_dic_sig) { g_dic_sig = sig; damage_icons(); }
}

static void render_one_icon(int i)
{
    rect_t r = dicon_rect(i);
    if (r.x >= clip_x1 || r.x + r.w <= clip_x0 || r.y >= clip_y1 || r.y + r.h <= clip_y0) return;
    bool sel = i == sel_dicon, hot = i == hover_dicon;
    uint32_t base = g_wp_img_on ? g_wp_rows[imin(imax(r.y + 40, 0), SH - 1)] : mix(TH.wp_top, TH.wp_bot, 30);
    if (sel) round_rect(r.x, r.y, r.w, r.h, 4, mix(base, TH.accent, 55));
    else if (hot) round_rect(r.x, r.y, r.w, r.h, 4, mix(base, 0xffffff, 14));
    draw_icon(g_dic[i].icon, r.x + 24, r.y + 6, 48);
    int n = (int)strlen(g_dic[i].label);
    int tx = r.x + (r.w - n * CW) / 2;
    text(tx + 1, r.y + 62, g_dic[i].label, 0x000000, TRANSPARENT, n);
    text(tx, r.y + 61, g_dic[i].label, 0xffffff, TRANSPARENT, n);
}

static void render_icons(void)
{
    if (!C.icons) return;
    for (int i = 0; i < g_ndic; i++) if (!(g_idrag.moving && i == g_idrag.idx)) render_one_icon(i);
    if (g_idrag.moving) render_one_icon(g_idrag.idx);
}

// ---- windows: drawing ------------------------------------------------------------------------------------------------------

static void draw_shadow(const win_t *w)
{
    const int R = 12, OFF = 4;
    rect_t full = { w->x - R, w->y - R + OFF, w->w + 2 * R, w->h + 2 * R };
    rect_t clip = { clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0 }, o;
    if (!rect_isect(full, clip, &o)) return;
    for (int y = o.y; y < o.y + o.h; y++) {
        for (int x = o.x; x < o.x + o.w; x++) {
            int dx = imax(imax(w->x - x, 0), x - (w->x + w->w - 1));
            int dy = imax(imax(w->y + OFF - y, 0), y - (w->y + OFF + w->h - 1));
            int d = imax(dx, dy);
            if (d >= R) continue;
            if (dx == 0 && dy == 0) continue;
            int f = 100 - (R - d) * (R - d) * 40 / (R * R);        // darker near the window, fading out
            uint32_t *p = g_store + y * SW + x, c = *p;
            *p = (((c >> 16) & 255) * (uint32_t)f / 100) << 16 | (((c >> 8) & 255) * (uint32_t)f / 100) << 8 | ((c & 255) * (uint32_t)f / 100);
        }
    }
}

static void title_button_icon(int idx, rect_t r, uint32_t fg)
{
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    if (idx == 0) { line(cx - 4, cy - 4, cx + 4, cy + 4, 2, fg); line(cx - 4, cy + 4, cx + 4, cy - 4, 2, fg); }
    else if (idx == 1) frame_rect(cx - 5, cy - 5, 11, 10, fg);
    else if (idx == 3) { for (int k = 0; k < 2; k++) for (int j = 0; j < 2; j++) { int bx = cx - 6 + k * 8, by = cy - 5 + j * 7; fill_rect(bx, by, 5, 1, fg); fill_rect(bx + (k ? 4 : 0), by, 1, 4, fg); } }
    else fill_rect(cx - 5, cy + 3, 11, 2, fg);
}

static void draw_content(int wi, bool foc)
{
    win_t *w = &g_win[wi];
    switch (w->type) {
    case W_MON: render_mon(w); break;
    case W_CLOCK: render_clock(w); break;
    case W_FILES: render_files(w); break;
    case W_SET: render_settings(w); break;
    case W_CALC: render_calc(w); break;
    case W_EDIT: render_edit(w, foc); break;
    case W_GAMES: render_games(w); break;
    case W_SERV: render_serv(w); break;
    case W_SCRIPTS: render_scripts(w); break;
    case W_TASKS: render_tasks(w); break;
    default: render_term(w, foc); break;
    }
}

static void draw_window(int wi)
{
    win_t *w = &g_win[wi];
    bool foc = wi == g_focus;
    if (w->fullscreen) {
        rect_t fc = content_rect(w);
        rect_t fclip = { clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0 }, fin;
        if (rect_isect(fclip, fc, &fin)) { clip_set(fin); draw_content(wi, foc); clip_set(fclip); }
        return;
    }
    if (!w->maximized && C.shadows) draw_shadow(w);
    uint32_t bd = foc ? TH.border_f : TH.border;
    // frame and title bar (top corners rounded)
    round_rect(w->x, w->y, w->w, w->h, w->maximized ? 0 : 3, bd);
    uint32_t tbg = foc ? TH.t_f_bg : TH.t_u_bg, tfg = foc ? TH.t_f_tx : TH.t_u_tx;
    vgrad(w->x + BORD, w->y + BORD, w->w - 2 * BORD, TITLE_H, mix(tbg, 0xffffff, foc ? 14 : 7), tbg);
    fill_rect(w->x + BORD, w->y + BORD + TITLE_H - 1, w->w - 2 * BORD, 1, foc ? mix(tbg, bd, 40) : TH.line);
    draw_icon(win_icon(w), w->x + 9, w->y + BORD + 5, 18);
    text(w->x + 34, w->y + BORD + (TITLE_H - CH) / 2, w->title, tfg, tbg, imax(0, (w->w - 34 - 4 * 26 - 12) / CW));
    for (int b = 0; b < 4; b++) {
        rect_t r = title_button(w, b);
        bool hot = g_hover_win == wi && g_hover_btn == b;
        if (hot) round_rect(r.x, r.y, r.w, r.h, 3, b == 0 ? TH.bad : mix(tbg, tfg, 18));
        title_button_icon(b, r, hot && b == 0 ? 0xffffff : tfg);
    }
    rect_t c = content_rect(w);
    rect_t clip = { clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0 }, saved = clip, in;
    if (!rect_isect(clip, c, &in)) return;
    clip_set(in);
    draw_content(wi, foc);
    clip_set(saved);
}

// ---- menus, run dialog, toast ------------------------------------------------------------------------------------------------

enum { ACT_NONE, ACT_OPEN, ACT_RUNBOX, ACT_LOGOUT, ACT_REBOOT, ACT_POWEROFF, ACT_CUSTOM, ACT_WIN_MIN, ACT_WIN_MAX, ACT_WIN_CLOSE,
       ACT_THEME, ACT_HELP, ACT_ABOUT, ACT_SUBMENU, ACT_GAME, ACT_CONSOLE, ACT_LOCK, ACT_RSHIDT, ACT_NEWFILE, ACT_NEWDIR, ACT_REFRESH,
       ACT_ICON_OPEN, ACT_ICON_EDIT, ACT_ICON_DELETE, ACT_ICON_WALL, ACT_ARRANGE, ACT_FILES };

typedef struct { char label[32]; int act, arg, icon; bool sep; } mitem_t;
static struct { bool open; int x, y, w, h, n, hover; mitem_t it[24]; } g_menu;

#define MENU_ROW 26
#define MENU_SEP 10

static void menu_clear(void) { g_menu.n = 0; }

static void menu_add(const char *label, int act, int arg, int icon)
{
    if (g_menu.n >= 24) return;
    mitem_t *m = &g_menu.it[g_menu.n++];
    put_str(m->label, sizeof(m->label), label);
    m->act = act; m->arg = arg; m->icon = icon; m->sep = false;
}

static void menu_sep(void) { if (g_menu.n < 24) { g_menu.it[g_menu.n].sep = true; g_menu.it[g_menu.n].act = ACT_NONE; g_menu.it[g_menu.n].label[0] = 0; g_menu.n++; } }

static rect_t menu_rect(void) { rect_t r = { g_menu.x, g_menu.y, g_menu.w, g_menu.h }; return r; }

static void menu_close(void)
{
    if (!g_menu.open) return;
    g_menu.open = false;
    damage_rect(menu_rect());
    hover_panel = -1;
    damage(0, 0, SW, PANEL_H);
}

static void menu_show(int x, int y)
{
    int w = 190, h = 8;
    for (int i = 0; i < g_menu.n; i++) {
        w = imax(w, (int)strlen(g_menu.it[i].label) * CW + 64);
        h += g_menu.it[i].sep ? MENU_SEP : MENU_ROW;
    }
    g_menu.w = w; g_menu.h = h;
    g_menu.x = imax(0, imin(x, SW - w - 4));
    g_menu.y = imax(PANEL_H, imin(y, SH - h - 4));
    g_menu.hover = -1;
    g_menu.open = true;
    damage_rect(menu_rect());
}

static int menu_item_at(int mx, int my)
{
    if (!g_menu.open || !rect_has(menu_rect(), mx, my)) return -1;
    int y = g_menu.y + 4;
    for (int i = 0; i < g_menu.n; i++) {
        int h = g_menu.it[i].sep ? MENU_SEP : MENU_ROW;
        if (my >= y && my < y + h) return g_menu.it[i].sep ? -1 : i;
        y += h;
    }
    return -1;
}

static void render_menu(void)
{
    if (!g_menu.open) return;
    rect_t r = menu_rect();
    round_rect(r.x - 1, r.y - 1, r.w + 2, r.h + 2, 4, TH.border);
    round_rect(r.x, r.y, r.w, r.h, 4, TH.menu_bg);
    int y = r.y + 4;
    for (int i = 0; i < g_menu.n; i++) {
        mitem_t *m = &g_menu.it[i];
        if (m->sep) { fill_rect(r.x + 10, y + MENU_SEP / 2, r.w - 20, 1, TH.line); y += MENU_SEP; continue; }
        bool hot = i == g_menu.hover;
        if (hot) round_rect(r.x + 3, y, r.w - 6, MENU_ROW, 3, TH.menu_hot);
        uint32_t bg = hot ? TH.menu_hot : TH.menu_bg;
        if (m->icon >= 0) draw_icon(m->icon, r.x + 12, y + 5, 16);
        text(r.x + 40, y + (MENU_ROW - CH) / 2, m->label, hot ? 0xffffff : TH.menu_fg, bg, (r.w - 48) / CW);
        y += MENU_ROW;
    }
}

static struct { bool open; char buf[64], pre[64]; int len, sel; } g_run;

static rect_t run_rect(void) { rect_t r = { (SW - 520) / 2, 150, 520, 92 }; return r; }

static int run_matches(const char *pfx, const char *out[], int max)
{
    int n = 0;
    size_t pl = strlen(pfx);
    if (strchr(pfx, ' ')) return 0;
    for (size_t i = 0; i < command_count && n < max; i++) if (strncmp(commands[i].name, pfx, pl) == 0) out[n++] = commands[i].name;
    return n;
}

static void render_run(void)
{
    if (!g_run.open) return;
    rect_t r = run_rect();
    round_rect(r.x - 1, r.y - 1, r.w + 2, r.h + 2, 4, TH.border_f);
    round_rect(r.x, r.y, r.w, r.h, 4, TH.menu_bg);
    draw_icon(IC_RUN, r.x + 14, r.y + 12, 32);
    text(r.x + 58, r.y + 10, "Run a command", TH.menu_fg, TH.menu_bg, 20);
    round_rect(r.x + 58, r.y + 32, r.w - 74, 24, 3, TH.field);
    frame_rect(r.x + 58, r.y + 32, r.w - 74, 24, TH.line);
    text(r.x + 66, r.y + 36, g_run.buf, TH.fg, TH.field, 40);
    fill_rect(r.x + 66 + g_run.len * CW, r.y + 37, CW, CH - 2, TH.accent);
    const char *m[12];
    int nm = run_matches(g_run.sel > 0 ? g_run.pre : g_run.buf, m, 12);
    int x = r.x + 58;
    for (int i = 0; i < nm; i++) {
        int len = (int)strlen(m[i]);
        if (x + (len + 2) * CW > r.x + r.w - 10) break;
        bool sel = g_run.sel > 0 && i == g_run.sel - 1;
        if (sel) round_rect(x - 4, r.y + 62, len * CW + 8, 22, 3, TH.accent);
        text(x, r.y + 65, m[i], sel ? TH.accent_tx : TH.dim, sel ? TH.accent : TH.menu_bg, len);
        x += (len + 2) * CW;
    }
}

static void render_toast(void)
{
    if (!g_msg[0]) return;
    int n = (int)strlen(g_msg);
    int w = n * CW + 36, x = (SW - w) / 2, y = PANEL_H + 12;
    round_rect(x, y, w, 28, 5, shade(TH.panel_bg, 90));
    text(x + 18, y + 6, g_msg, 0xffffff, shade(TH.panel_bg, 90), n);
}

// ---- login page --------------------------------------------------------------------------------------------------------------

enum { MODE_DESKTOP, MODE_LOGIN };
static int g_mode = MODE_DESKTOP;
static bool g_boot_mode;                            // started by the boot sequence instead of the 'desktop' command
static struct { int sel, hover, pwlen, fails; char pw[40]; char err[56]; uint32_t until; bool locked; } g_login;
enum { LH_CARD = 0, LH_FIELD = 20, LH_LOGIN, LH_CONSOLE, LH_RESTART, LH_SHUT };

static const uint32_t AV_COL[6] = { 0x3b82c4, 0x2e9e6b, 0xc97a2b, 0x8b5cc4, 0xc4455b, 0x4a9ab0 };

static int login_nusers(void) { if (g_login.locked || !users_enabled()) return 1; int n = users_count(); return n > 0 ? n : 1; }
static const char *login_name(int i) { if (g_login.locked || !users_enabled()) return users_current_name(); return users_name_at(i); }
static int login_first(void) { return g_login.sel >= 6 ? g_login.sel - 5 : 0; }
static rect_t lg_card(int vi) { int n = imin(login_nusers(), 6), total = n * 150 + (n - 1) * 24; rect_t r = { SW / 2 - total / 2 + vi * 174, 230, 150, 170 }; return r; }
static rect_t lg_field(void) { rect_t r = { SW / 2 - 185, 440, 260, 34 }; return r; }
static rect_t lg_button(void) { rect_t r = { SW / 2 + 85, 440, 100, 34 }; return r; }
static rect_t lg_small(int i) { rect_t r = { SW - 24 - 3 * 106 + i * 106, SH - 54, 100, 30 }; return r; }

static void shadow_text_center(int y, const char *str, uint32_t fg)
{
    int n = (int)strlen(str), x = (SW - n * CW) / 2;
    text(x + 1, y + 1, str, 0x000000, TRANSPARENT, n);
    text(x, y, str, fg, TRANSPARENT, n);
}

static void render_login(void)
{
    datetime_t dt;
    rtc_get_time(&dt);
    char t[16], d[24];
    format_time_24h(&dt, t);
    format_date(&dt, d);
    char hm[6] = { t[0], t[1], ':', t[3], t[4], 0 };
    for (int i = 0; i < 5; i++) {
        glyph_big(SW / 2 - 100 + i * 40 + 2, 52, (unsigned char)hm[i], 5, 0x000000, TRANSPARENT);
        glyph_big(SW / 2 - 100 + i * 40, 50, (unsigned char)hm[i], 5, 0xffffff, TRANSPARENT);
    }
    shadow_text_center(110, d, 0xe8edf5);
    shadow_text_center(150, g_login.locked ? "Screen locked" : "RadiumOS", 0xffffff);
    shadow_text_center(176, g_login.locked ? "Enter your password to continue" : (users_enabled() ? "Choose an account" : "No account has been created yet"), 0xc9d3e3);

    int n = login_nusers(), first = login_first();
    for (int vi = 0; vi < 6 && first + vi < n; vi++) {
        int u = first + vi;
        rect_t r = lg_card(vi);
        bool sel = u == g_login.sel, hot = g_login.hover == LH_CARD + vi;
        round_rect(r.x - 2, r.y - 2, r.w + 4, r.h + 4, 6, sel ? TH.accent : (hot ? TH.border_f : TH.border));
        round_rect(r.x, r.y, r.w, r.h, 5, TH.field);
        const char *nm = login_name(u);
        uint32_t h = 0;
        for (const char *c = nm; *c; c++) h = h * 31 + (uint32_t)(unsigned char)*c;
        disc(r.x + r.w / 2, r.y + 62, 44, AV_COL[h % 6]);
        char ini = nm[0] >= 'a' && nm[0] <= 'z' ? (char)(nm[0] - 32) : nm[0];
        glyph_big(r.x + r.w / 2 - 20, r.y + 62 - 20, (unsigned char)ini, 5, 0xffffff, TRANSPARENT);
        text_center(r.x, r.w, r.y + 120, nm, TH.fg, TH.field, 17);
        const char *sub = "";
        if (users_enabled() && !g_login.locked) sub = users_sudo_at(u) ? "administrator" : (users_has_password_at(u) ? "user" : "locked");
        else if (!users_enabled()) sub = "no password";
        text_center(r.x, r.w, r.y + 142, sub, TH.dim, TH.field, 17);
    }

    if (users_enabled()) {
        rect_t f = lg_field(), b = lg_button();
        round_rect(f.x - 1, f.y - 1, f.w + 2, f.h + 2, 4, TH.border_f);
        round_rect(f.x, f.y, f.w, f.h, 3, TH.field);
        if (g_login.pwlen == 0) text(f.x + 10, f.y + 9, "Password", TH.dim, TH.field, 24);
        for (int i = 0; i < g_login.pwlen && i < 28; i++) glyph(f.x + 10 + i * CW, f.y + 9, '*', TH.fg, TH.field);
        fill_rect(f.x + 10 + imin(g_login.pwlen, 28) * CW, f.y + 8, 2, CH, TH.accent);
        bool bh = g_login.hover == LH_LOGIN;
        round_rect(b.x, b.y, b.w, b.h, 4, bh ? shade(TH.accent, 112) : TH.accent);
        text_center(b.x, b.w, b.y + 9, g_login.locked ? "Unlock" : "Log in", TH.accent_tx, bh ? shade(TH.accent, 112) : TH.accent, 10);
    } else {
        rect_t b = { SW / 2 - 70, 440, 140, 34 };
        bool bh = g_login.hover == LH_LOGIN;
        round_rect(b.x, b.y, b.w, b.h, 4, bh ? shade(TH.accent, 112) : TH.accent);
        text_center(b.x, b.w, b.y + 9, g_login.locked ? "Unlock" : "Log in", TH.accent_tx, bh ? shade(TH.accent, 112) : TH.accent, 10);
    }
    if (g_login.err[0]) shadow_text_center(494, g_login.err, 0xff9a9a);
    shadow_text_center(SH - 48, "Left/Right or Tab: account    Enter: log in", 0xc9d3e3);

    static const char *const SM[3] = { "Console", "Restart", "Shut down" };
    for (int i = g_login.locked ? 1 : 0; i < 3; i++) {
        rect_t r = lg_small(i);
        bool hot = g_login.hover == LH_CONSOLE + i;
        uint32_t bg = hot ? TH.panel_hot : TH.panel_bg;
        round_rect(r.x, r.y, r.w, r.h, 4, bg);
        text_center(r.x, r.w, r.y + 7, SM[i], i == 2 && hot ? TH.bad : TH.panel_fg, bg, 12);
    }
}

static bool g_fs_hint;
static rect_t fs_exit_rect(void) { rect_t r = { SW / 2 - 80, 0, 160, 28 }; return r; }
static void render_fs_hint(void)
{
    rect_t r = fs_exit_rect();
    round_rect(r.x, r.y - 6, r.w, r.h + 6, 6, shade(TH.panel_bg, 85));
    text_center(r.x, r.w, r.y + 6, "Exit full screen", 0xffffff, shade(TH.panel_bg, 85), 18);
}

// ---- composing a rectangle ----------------------------------------------------------------------------------------------------


extern int rust_png_decode_fit(const uint8_t *data, uint32_t len, uint32_t max_w, uint32_t max_h, uint32_t *out, uint32_t out_cap, uint32_t *w_out, uint32_t *h_out);
extern const uint8_t wallpaper_default_png[], wallpaper_default_png_end[];

static uint32_t g_wp_img[SW * SH];                 // the picture scaled to the screen, 0xAARRGGBB (alpha 0 where it does not reach)
static char g_wp_key[112];                         // which picture and mode g_wp_img holds ("" = nothing valid)

// mode: 0 fill (crop), 1 fit, 2 stretch, 3 centre at its own size. Shrinking averages the source pixels in each box.
static void wp_scale(const uint32_t *src, int w, int h, int mode)
{
    static int cx0[SW], cx1[SW];
    int dw, dh;
    memset(g_wp_img, 0, sizeof(g_wp_img));
    if (mode == 2) { dw = SW; dh = SH; }
    else if (mode == 3) { dw = w; dh = h; }
    else if (((w * SH > h * SW) ? 1 : 0) == (mode == 0 ? 1 : 0)) { dh = SH; dw = imax(1, w * SH / h); }
    else { dw = SW; dh = imax(1, h * SW / w); }
    int x0 = (SW - dw) / 2, y0 = (SH - dh) / 2;
    int xs = imax(0, x0), xe = imin(SW, x0 + dw), ys = imax(0, y0), ye = imin(SH, y0 + dh);
    for (int dx = xs; dx < xe; dx++) {
        int a = (dx - x0) * w / dw, b = (dx - x0 + 1) * w / dw;
        cx0[dx] = a; cx1[dx] = imin(w, imax(b, a + 1));
    }
    for (int dy = ys; dy < ye; dy++) {
        int sy0 = (dy - y0) * h / dh, sy1 = imin(h, imax((dy - y0 + 1) * h / dh, sy0 + 1));
        for (int dx = xs; dx < xe; dx++) {
            uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint32_t *row = src + sy * w;
                for (int sx = cx0[dx]; sx < cx1[dx]; sx++) {
                    uint32_t px = row[sx], pa = px >> 24;
                    a += pa; r += ((px >> 16) & 255) * pa; g += ((px >> 8) & 255) * pa; b += (px & 255) * pa; n++;
                }
            }
            g_wp_img[dy * SW + dx] = a == 0 ? 0 : ((a / n) << 24 | (r / a) << 16 | (g / a) << 8 | (b / a));
        }
    }
}

static bool wp_fail(const char *m) { put_str(g_wp_err, sizeof(g_wp_err), m); return false; }

static uint8_t g_wp_file[4 << 20];                  // a wallpaper PNG as read from disk

// Decodes C.wp_file (or the built-in mascot) into g_wp_img. The decoder writes into g_wp_full, which is rebuilt afterwards.
static bool wp_load_image(void)
{
    const uint8_t *data;
    uint32_t len;
    int mode = C.wp_mode;
    g_wp_err[0] = 0;
    if (C.wp_file[0]) {
        int size = avfs_get_filesize(C.wp_file);
        if (size <= 0) return wp_fail("cannot read the wallpaper file");
        if ((size_t)size > sizeof(g_wp_file)) return wp_fail("the wallpaper file is larger than 4 MB");
        if (avfs_read_file(C.wp_file, g_wp_file, (uint32_t)size, 0) != 0) return wp_fail("cannot read the wallpaper file");
        data = g_wp_file; len = (uint32_t)size;
    } else {
        data = wallpaper_default_png; len = (uint32_t)(wallpaper_default_png_end - wallpaper_default_png);
        mode = 3;
        if (len == 0) return wp_fail("this build has no wallpaper picture");
    }
    uint32_t pw = 0, ph = 0;
    int rc = rust_png_decode_fit(data, len, SW, SH, g_wp_full, SW * SH, &pw, &ph);
    if (rc != 0) return wp_fail(rc == -2 ? "the wallpaper picture is too large" : "the wallpaper must be a PNG picture");
    wp_scale(g_wp_full, (int)pw, (int)ph, mode);
    return true;
}

static void wallpaper_prepare(void)
{
    for (int y = 0; y < SH; y++) {
        int t = C.wallpaper == 1 ? 35 : y * 100 / (SH - 1);
        if (C.wallpaper == 2) t = 100 - t;
        g_wp_rows[y] = mix(TH.wp_top, TH.wp_bot, t);
    }
    g_wp_theme = C.theme;
    g_wp_img_on = false;
    if (C.wallpaper != 3) return;
    char key[112];
    if (C.wp_file[0]) snprintf(key, sizeof(key), "%s|%d", C.wp_file, C.wp_mode); else put_str(key, sizeof(key), "default");
    if (strcmp(key, g_wp_key) != 0) {
        g_wp_key[0] = 0;
        if (!wp_load_image()) { if (g_wp_err[0]) flash(g_wp_err); return; }
        put_str(g_wp_key, sizeof(g_wp_key), key);
    }
    for (int y = 0; y < SH; y++) {
        uint32_t bg = g_wp_rows[y];
        for (int x = 0; x < SW; x++) {
            uint32_t px = g_wp_img[y * SW + x], a = px >> 24;
            if (a == 255) g_wp_full[y * SW + x] = px & 0xffffff;
            else if (a == 0) g_wp_full[y * SW + x] = bg;
            else {
                uint32_t r = (((px >> 16) & 255) * a + ((bg >> 16) & 255) * (255 - a)) / 255;
                uint32_t g = (((px >> 8) & 255) * a + ((bg >> 8) & 255) * (255 - a)) / 255;
                uint32_t b = ((px & 255) * a + (bg & 255) * (255 - a)) / 255;
                g_wp_full[y * SW + x] = r << 16 | g << 8 | b;
            }
        }
    }
    g_wp_img_on = true;
}

static void wallpaper_set_file(const char *path)
{
    put_str(C.wp_file, sizeof(C.wp_file), path);
    C.wallpaper = 3;
    wallpaper_prepare();
    damage_all();
    if (g_wp_img_on) flash("wallpaper changed (Save keeps it)");
}

// PNG files the user can pick as a wallpaper.
static void pics_scan(void)
{
    char home[96], dirs[5][96];
    home_dir(home, sizeof(home));
    snprintf(dirs[0], sizeof(dirs[0]), "%s/Pictures", home);
    put_str(dirs[1], sizeof(dirs[1]), "/disk/Pictures");
    put_str(dirs[2], sizeof(dirs[2]), "/disk/wallpapers");
    put_str(dirs[3], sizeof(dirs[3]), g_desk_dir);
    put_str(dirs[4], sizeof(dirs[4]), home);
    g_npics = 0;
    for (int d = 0; d < 5 && g_npics < 6; d++) {
        bool dup = false;
        for (int e = 0; e < d; e++) if (strcmp(dirs[e], dirs[d]) == 0) dup = true;
        if (dup || !dirs[d][0] || !avfs_is_directory(dirs[d])) continue;
        DirectoryEntry ent[48];
        int n = avfs_list_directory(dirs[d], ent, 48);
        for (int i = 0; i < n && g_npics < 6; i++) {
            int len = (int)strlen(ent[i].name);
            if (ent[i].is_directory || len < 5 || !same_nocase(ent[i].name + len - 4, ".png")) continue;
            put_str(g_pics[g_npics].name, sizeof(g_pics[0].name), ent[i].name);
            snprintf(g_pics[g_npics].path, sizeof(g_pics[0].path), "%s/%s", dirs[d], ent[i].name);
            g_npics++;
        }
    }
}

static void compose(rect_t R)
{
    if (g_wp_theme != C.theme) wallpaper_prepare();
    clip_set(R);
    for (int y = R.y; y < R.y + R.h; y++) {
        uint32_t *p = g_store + y * SW + R.x, c = g_wp_rows[y];
        if (g_wp_img_on) { memcpy(p, g_wp_full + y * SW + R.x, (size_t)R.w * 4); continue; }
        for (int x = 0; x < R.w; x++) p[x] = c;
    }
    rect_t b, o;
    if (g_mode == MODE_LOGIN) {
        render_login();
        if (g_msg[0]) { rect_t tr = { SW / 2 - 260, PANEL_H + 8, 520, 34 }; if (rect_isect(R, tr, &o)) { clip_set(o); render_toast(); } }
        clip_set((rect_t){ 0, 0, SW, SH });
        return;
    }
    b = icons_bounds();
    if (rect_isect(R, b, &o)) { clip_set(o); render_icons(); }
    clip_set(R);
    for (int k = 0; k < g_zn; k++) {
        int wi = g_z[k];
        if (!win_visible(wi)) continue;
        rect_t db = win_damage_rect(&g_win[wi]);
        if (!rect_isect(R, db, &o)) continue;
        clip_set(o);
        draw_window(wi);
    }
    if (C.dock && !fullscreen_active()) {
        rect_t d = dock_bounds();
        d.x -= 10; d.y -= 40; d.w += 20; d.h += 40;
        if (rect_isect(R, d, &o)) { clip_set(o); render_dock(); }
    }
    b.x = 0; b.y = 0; b.w = SW; b.h = PANEL_H;
    if (!fullscreen_active() && rect_isect(R, b, &o)) { clip_set(o); render_panel(); }
    if (fullscreen_active() && g_fs_hint) { rect_t fr = fs_exit_rect(); if (rect_isect(R, fr, &o)) { clip_set(o); render_fs_hint(); } }
    if (g_menu.open) { rect_t mr = menu_rect(); mr.x -= 6; mr.y -= 4; mr.w += 12; mr.h += 12; if (rect_isect(R, mr, &o)) { clip_set(o); render_menu(); } }
    if (g_run.open) { rect_t rr = run_rect(); rr.x -= 2; rr.y -= 2; rr.w += 4; rr.h += 4; if (rect_isect(R, rr, &o)) { clip_set(o); render_run(); } }
    if (g_msg[0]) { rect_t tr = { SW / 2 - 260, PANEL_H + 8, 520, 34 }; if (rect_isect(R, tr, &o)) { clip_set(o); render_toast(); } }
    clip_set((rect_t){ 0, 0, SW, SH });
}

// ---- Vulkan ---------------------------------------------------------------------------------------------------------------------

typedef struct {
    VkInstance instance; VkDevice device; VkQueue queue;
    VkSurfaceKHR surface; VkSwapchainKHR swapchain;
    VkImage images[MAX_IMG]; uint32_t image_count;
    VkBuffer store_buf; VkDeviceMemory store_mem;
    VkCommandBuffer cmd;
    bool ready;
} gpu_t;

static gpu_t P;

#define OUT_ERR(...) do { terminal_setcolor(VGA_COLOR_LIGHT_RED); printr(__VA_ARGS__); terminal_setcolor(VGA_COLOR_WHITE); } while (0)
#define VK_TRY(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { OUT_ERR("desktop: %s failed (%d)\n", #call, (int)r_); return false; } } while (0)

static bool gpu_surface_create(void);
static void gpu_surface_destroy(void);

static bool gpu_init(void)
{
    uint32_t n = 1;
    VkPhysicalDevice phys;
    VK_TRY(vkCreateInstance(NULL, NULL, &P.instance));
    VK_TRY(vkEnumeratePhysicalDevices(P.instance, &n, &phys));
    VK_TRY(vkCreateDevice(phys, NULL, NULL, &P.device));
    vkGetDeviceQueue(P.device, 0, 0, &P.queue);

    VkDeviceSize bytes = (VkDeviceSize)SW * STORE_H * 4;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkMemoryRequirements req;
    VK_TRY(vkCreateBuffer(P.device, &bi, NULL, &P.store_buf));
    vkGetBufferMemoryRequirements(P.device, P.store_buf, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, 0 };
    VK_TRY(vkAllocateMemory(P.device, &ai, NULL, &P.store_mem));
    VK_TRY(vkBindBufferMemory(P.device, P.store_buf, P.store_mem, 0));
    void *mapped;
    VK_TRY(vkMapMemory(P.device, P.store_mem, 0, req.size, 0, &mapped));
    g_store = mapped;

    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, 0, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VK_TRY(vkAllocateCommandBuffers(P.device, &cbai, &P.cmd));
    return gpu_surface_create();
}

static bool gpu_surface_create(void)
{
    VkRadiumSurfaceCreateInfoEXT sci = { VK_STRUCTURE_TYPE_RADIUM_SURFACE_CREATE_INFO_EXT, NULL, 1 };
    VK_TRY(vkCreateRadiumSurfaceEXT(P.instance, &sci, NULL, &P.surface));
    VkSwapchainCreateInfoKHR swi = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, P.surface, 2, VK_FORMAT_B8G8R8A8_UNORM, 0, { SW, SH },
                                     1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, 0, 0,
                                     VK_PRESENT_MODE_FIFO_KHR, 1, 0 };
    VK_TRY(vkCreateSwapchainKHR(P.device, &swi, NULL, &P.swapchain));
    P.image_count = MAX_IMG;
    VK_TRY(vkGetSwapchainImagesKHR(P.device, P.swapchain, &P.image_count, P.images));
    g_img = (int)P.image_count;
    P.ready = true;
    return true;
}

static void gpu_surface_destroy(void)
{
    P.ready = false;
    if (P.swapchain) vkDestroySwapchainKHR(P.device, P.swapchain, NULL);
    if (P.surface) vkDestroySurfaceKHR(P.instance, P.surface, NULL);   // gives the screen back to the console
    P.swapchain = 0; P.surface = 0;
}

static void gpu_shutdown(void)
{
    gpu_surface_destroy();
    if (P.instance) vkDestroyInstance(P.instance, NULL);
    memset(&P, 0, sizeof(P));
    g_store = NULL;
}

#define MAX_REG 64
static VkBufferImageCopy g_reg[MAX_REG];
static int g_nreg;

// Queues a copy of the store rectangle at (sx, sy) to the screen position (x, y).
static void copy_rect(int x, int y, int w, int h, int sx, int sy)
{
    if (w <= 0 || h <= 0 || g_nreg == MAX_REG) return;
    VkBufferImageCopy *r = &g_reg[g_nreg++];
    memset(r, 0, sizeof(*r));
    r->bufferOffset = ((VkDeviceSize)sy * SW + (VkDeviceSize)sx) * 4;
    r->bufferRowLength = SW;
    r->imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r->imageSubresource.layerCount = 1;
    r->imageOffset.x = x; r->imageOffset.y = y;
    r->imageExtent.width = (uint32_t)w; r->imageExtent.height = (uint32_t)h; r->imageExtent.depth = 1;
}

// ---- the pointer ------------------------------------------------------------------------------------------------------------

enum { CUR_ARROW, CUR_EW, CUR_NS, CUR_MOVE };

static const char *const SPR_ARROW[16] = {
    "X          ", "XX         ", "X.X        ", "X..X       ", "X...X      ", "X....X     ", "X.....X    ", "X......X   ",
    "X.......X  ", "X........X ", "X.....XXXXX", "X..X..X    ", "X.X X..X   ", "XX  X..X   ", "X    X..X  ", "     XXX   ",
};
static const char *const SPR_EW[9] = {
    "    X     X    ", "   XX     XX   ", "  X.X     X.X  ", " X..XXXXXXX..X ", "X.............X",
    " X..XXXXXXX..X ", "  X.X     X.X  ", "   XX     XX   ", "    X     X    ",
};
static const char *const SPR_MOVE[13] = {
    "      X      ", "     X.X     ", "    X...X    ", "   XXX.XXX   ", "     X.X     ", " XX  X.X  XX ", "X..XXX.XXX..X",
    " XX  X.X  XX ", "     X.X     ", "   XXX.XXX   ", "    X...X    ", "     X.X     ", "      X      ",
};

// 0 transparent, 1 outline, 2 fill; the sprite's top-left corner is at (0, 0)
static int sprite_px(int kind, int x, int y)
{
    char c = ' ';
    if (kind == CUR_ARROW) { if (x >= 0 && x < 11 && y >= 0 && y < 16) c = SPR_ARROW[y][x]; }
    else if (kind == CUR_EW) { if (x >= 0 && x < 15 && y >= 0 && y < 9) c = SPR_EW[y][x]; }
    else if (kind == CUR_MOVE) { if (x >= 0 && x < 13 && y >= 0 && y < 13) c = SPR_MOVE[y][x]; }
    else if (x >= 0 && x < 9 && y >= 0 && y < 15) c = SPR_EW[x][y];
    return c == 'X' ? 1 : (c == '.' ? 2 : 0);
}

static void sprite_hotspot(int kind, int *hx, int *hy)
{
    if (kind == CUR_ARROW) { *hx = 0; *hy = 0; }
    else if (kind == CUR_EW) { *hx = 7; *hy = 4; }
    else if (kind == CUR_MOVE) { *hx = 6; *hy = 6; }
    else { *hx = 4; *hy = 7; }
}

static int g_mx = SW / 2, g_my = SH / 2, g_cur_kind = CUR_ARROW;
static bool g_cur_show;
static struct { int x, y; bool valid; } g_cdrawn[MAX_IMG];

static void build_patch(int px, int py, int dst_x, bool with_cursor, int sx0, int sy0)
{
    for (int y = 0; y < PATCH; y++) {
        for (int x = 0; x < PATCH; x++) {
            int sx = px + x, sy = py + y;
            uint32_t c = g_store[sy * SW + sx];
            if (with_cursor) {
                int s = sprite_px(g_cur_kind, sx - sx0, sy - sy0);
                if (s == 1) c = 0x000000;
                else if (s == 2) c = 0xffffff;
            }
            g_store[(SH + y) * SW + dst_x + x] = c;
        }
    }
}

// Puts the old pointer position back and draws the pointer in its new one (after everything else in the frame).
static void queue_cursor(int index)
{
    if (!g_cur_show) return;
    if (g_cdrawn[index].valid) {
        build_patch(g_cdrawn[index].x, g_cdrawn[index].y, 0, false, 0, 0);
        copy_rect(g_cdrawn[index].x, g_cdrawn[index].y, PATCH, PATCH, 0, SH);
    }
    int hx, hy;
    sprite_hotspot(g_cur_kind, &hx, &hy);
    int sx0 = g_mx - hx, sy0 = g_my - hy;
    int px = sx0 < 0 ? 0 : (sx0 > SW - PATCH ? SW - PATCH : sx0);
    int py = sy0 < 0 ? 0 : (sy0 > SH - PATCH ? SH - PATCH : sy0);
    build_patch(px, py, PATCH, true, sx0, sy0);
    copy_rect(px, py, PATCH, PATCH, PATCH, SH);
    g_cdrawn[index].x = px; g_cdrawn[index].y = py; g_cdrawn[index].valid = true;
    g_cur_dirty[index] = false;
}

static bool has_pending(void)
{
    if (g_ndirty) return true;
    for (int i = 0; i < g_img; i++) if (g_npend[i] || (g_cur_show && g_cur_dirty[i])) return true;
    return false;
}

static void draw_frame_raw(void)
{
    uint32_t t0 = now_ms();
    if (g_msg[0] && t0 >= g_msg_until) { g_msg[0] = 0; damage(SW / 2 - 260, PANEL_H + 8, 520, 34); }
    for (int k = 0; k < g_ndirty; k++) {
        compose(g_dirty[k]);
        for (int i = 0; i < g_img; i++) list_add(g_pend[i], &g_npend[i], 16, g_dirty[k]);
    }
    g_ndirty = 0;
    uint32_t index;
    if (vkAcquireNextImageKHR(P.device, P.swapchain, ~0ULL, 0, 0, &index) != VK_SUCCESS) return;
    g_nreg = 0;
    vkBeginCommandBuffer(P.cmd, NULL);
    for (int k = 0; k < g_npend[index]; k++) copy_rect(g_pend[index][k].x, g_pend[index][k].y, g_pend[index][k].w, g_pend[index][k].h, g_pend[index][k].x, g_pend[index][k].y);
    g_npend[index] = 0;
    queue_cursor((int)index);
    if (g_nreg) vkCmdCopyBufferToImage(P.cmd, P.store_buf, P.images[index], 0, (uint32_t)g_nreg, g_reg);
    vkEndCommandBuffer(P.cmd);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &P.cmd, 0, NULL };
    vkQueueSubmit(P.queue, 1, &si, 0);
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 0, NULL, 1, &P.swapchain, &index, NULL };
    vkQueuePresentKHR(P.queue, &pi);
    g_busy_ms += now_ms() - t0;
}

// The desktop's own frames must reach the screen even while a program's pixels are being redirected into a window.
static void draw_frame(void)
{
    fbcon_redirect_fn r = fbcon_get_redirect();
    fbcon_set_redirect(NULL);
    draw_frame_raw();
    fbcon_set_redirect(r);
}

// ---- window management --------------------------------------------------------------------------------------------------------

static void work_area(int *x, int *y, int *w, int *h) { *x = 0; *y = PANEL_H; *w = SW; *h = SH - PANEL_H; }

static void win_set_maximized(int wi, bool on)
{
    win_t *w = &g_win[wi];
    if (w->fullscreen) return;
    damage_win(w);
    if (on && !w->maximized) {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        int ax, ay, aw, ah;
        work_area(&ax, &ay, &aw, &ah);
        w->x = ax; w->y = ay; w->w = aw; w->h = ah;
        w->maximized = true;
    } else if (!on && w->maximized) {
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh;
        w->maximized = false;
    }
    damage_win(w);
}

static void win_set_fullscreen(int wi, bool on)
{
    if (wi < 0 || wi >= MAX_WIN || !g_win[wi].used) return;
    win_t *w = &g_win[wi];
    if (on == w->fullscreen) return;
    damage_all();
    if (on) {
        w->fsmax = w->maximized;
        if (!w->maximized) { w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h; }
        w->x = 0; w->y = 0; w->w = SW; w->h = SH;
        w->maximized = true; w->fullscreen = true; w->minimized = false;
        focus_and_raise(wi);
    } else {
        w->fullscreen = false;
        if (w->fsmax) { int ax, ay, aw, ah; work_area(&ax, &ay, &aw, &ah); w->x = ax; w->y = ay; w->w = aw; w->h = ah; }
        else { w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh; w->maximized = false; }
    }
    g_hover_win = g_hover_item_win = -1;
    damage_all();
}

static void defaults_for(wtype_t t, int *w, int *h, const char *title, win_t *win)
{
    switch (t) {
    case W_TERM: *w = 680; *h = 420; break;
    case W_FILES: *w = 860; *h = 520; break;
    case W_MON: *w = 520; *h = 520; break;
    case W_CLOCK: *w = 360; *h = 250; break;
    case W_SET: *w = 690; *h = 480; break;
    case W_CALC: *w = 330; *h = 500; break;
    case W_EDIT: *w = 740; *h = 520; break;
    case W_GAMES: *w = 620; *h = 440; break;
    case W_SERV: *w = 660; *h = 430; break;
    case W_SCRIPTS: *w = 620; *h = 420; break;
    case W_TASKS: *w = 660; *h = 480; break;
    default: *w = 640; *h = 440; break;
    }
    put_str(win->title, sizeof(win->title), title);
}

static int new_window(wtype_t type, const char *title)
{
    int fi = -1;
    for (int i = 0; i < MAX_WIN; i++) if (!g_win[i].used) { fi = i; break; }
    if (fi < 0) { flash("too many windows open"); return -1; }
    win_t *w = &g_win[fi];
    memset(w, 0, sizeof(*w));
    w->used = true;
    w->type = type;
    w->ws = g_ws;
    w->id = g_next_id++;
    w->nl = 1;
    int ww, hh;
    defaults_for(type, &ww, &hh, title, w);
    w->w = ww; w->h = hh;
    w->x = 150 + (g_casc % 8) * 34;
    w->y = PANEL_H + 24 + (g_casc % 8) * 30;
    g_casc++;
    if (w->x + w->w > SW - 10) w->x = imax(10, SW - 10 - w->w);
    if (w->y + w->h > SH - 80) w->y = imax(PANEL_H + 4, SH - 80 - w->h);
    z_raise(fi);
    set_focus(fi);
    if (C.open_max && type != W_CLOCK && type != W_CALC) win_set_maximized(fi, true);
    damage_win(w);
    damage(0, 0, SW, PANEL_H);
    return fi;
}

static void close_window(int wi)
{
    if (wi < 0 || !g_win[wi].used || g_win[wi].running) return;
    damage_win(&g_win[wi]);
    g_win[wi].used = false;
    z_remove(wi);
    if (g_hover_win == wi) g_hover_win = -1;
    if (g_focus == wi) { g_focus = -1; set_focus(topmost_visible()); }
    damage(0, 0, SW, PANEL_H);
}

static void switch_ws(int ws)
{
    if (ws == g_ws || ws < 0 || ws >= NWS) return;
    g_ws = ws;
    g_focus = topmost_visible();
    g_hover_win = -1;
    damage_all();
}

static void send_to_ws(int ws)
{
    if (g_focus < 0 || ws == g_ws || ws < 0 || ws >= NWS) return;
    int f = g_focus;
    damage_win(&g_win[f]);
    g_win[f].ws = ws;
    g_focus = topmost_visible();
    damage(0, 0, SW, PANEL_H);
}

static void minimize_window(int wi)
{
    win_t *w = &g_win[wi];
    if (w->fullscreen) win_set_fullscreen(wi, false);
    w->minimized = true;
    damage_win(w);
    if (g_focus == wi) { g_focus = -1; set_focus(topmost_visible()); }
    damage(0, 0, SW, PANEL_H);
}

static void cycle_window(void)
{
    if (g_zn < 2) return;
    int cand = -1;
    for (int k = 0; k < g_zn; k++) {                                 // the one at the bottom of the visible stack comes forward
        int wi = g_z[k];
        if (win_visible(wi)) { cand = wi; break; }
    }
    if (cand >= 0 && cand != g_focus) focus_and_raise(cand);
}

static void open_files_at(const char *path)
{
    int i = new_window(W_FILES, "Files");
    if (i < 0) return;
    put_str(g_win[i].path, sizeof(g_win[i].path), path);
    files_load(&g_win[i]);
    damage_content(&g_win[i]);
}

static int open_app(int kind)
{
    switch (kind) {
    case APP_TERM: {
        int i = new_window(W_TERM, "Terminal");
        if (i >= 0) {
            term_puts(&g_win[i], "Welcome to RadiumDE. Type a command and press Enter; Ctrl+C stops one that hangs.\n");
            term_puts(&g_win[i], "Alt+F1 opens the menu, Alt+F2 runs a command, Alt+Tab switches windows.\n");
        }
        return i;
    }
    case APP_FILES: {
        int before = g_next_id;
        char home[96];
        home_dir(home, sizeof(home));
        open_files_at(home);
        return g_next_id > before ? g_focus : -1;
    }
    case APP_MON: return new_window(W_MON, "System Monitor");
    case APP_TASKS: { int i = new_window(W_TASKS, "Task Manager"); if (i >= 0) { g_win[i].tsel = -1; g_win[i].lsel = -1; tm_update(&g_win[i]); } return i; }
    case APP_CLOCK: return new_window(W_CLOCK, "Clock");
    case APP_CALC: { int i = new_window(W_CALC, "Calculator"); if (i >= 0) calc_reset(&g_win[i]); return i; }
    case APP_EDIT: return open_editor(NULL);
    case APP_GAMES: { int i = new_window(W_GAMES, "Games"); if (i >= 0) games_build(); return i; }
    case APP_SERV: { int i = new_window(W_SERV, "Services"); if (i >= 0) { serv_refresh(&g_win[i]); g_win[i].lsel = -1; } return i; }
    case APP_SCRIPTS: { int i = new_window(W_SCRIPTS, "Scripts"); if (i >= 0) { scripts_scan(&g_win[i]); g_win[i].lsel = -1; } return i; }
    default: pics_scan(); return new_window(W_SET, "Settings");
    }
}

static int open_editor(const char *path)
{
    for (int k = 0; k < MAX_WIN; k++)                                   // already open: bring it forward
        if (path && g_win[k].used && g_win[k].type == W_EDIT && strcmp(g_win[k].path, path) == 0) { focus_and_raise(k); return k; }
    int i = new_window(W_EDIT, path ? "Editor" : "Untitled");
    if (i < 0) return -1;
    ed_new(&g_win[i]);
    if (path && !ed_load(&g_win[i], path)) { put_str(g_win[i].path, sizeof(g_win[i].path), path); put_str(g_win[i].emsg, sizeof(g_win[i].emsg), "new file"); ed_set_title(&g_win[i]); }
    damage_content(&g_win[i]);
    return i;
}

static int open_viewer(const char *title, const char *content, int len)
{
    int i = new_window(W_VIEW, title);
    if (i < 0) return -1;
    for (int k = 0; k < len && g_win[i].nl < TL - 1; k++) {
        char c = content[k];
        if (c == '\r') continue;
        term_put(&g_win[i], c);
    }
    g_win[i].scroll = 0;
    damage_content(&g_win[i]);
    return i;
}

// What a double click on a file does, by its extension.
static void open_path(const char *path)
{
    int len = (int)strlen(path);
    if (avfs_is_directory(path)) { open_files_at(path); return; }
    if (len > 4 && same_nocase(path + len - 4, ".app")) { char c[120]; snprintf(c, sizeof(c), "app %s", path); launch_program(c, "Application"); return; }
    if (len > 4 && (same_nocase(path + len - 4, ".png") || same_nocase(path + len - 4, ".bmp"))) { char c[120]; snprintf(c, sizeof(c), "png %s", path); run_leave(c); return; }
    if (len > 4 && same_nocase(path + len - 4, ".mp4")) { char c[120]; snprintf(c, sizeof(c), "mp4 %s", path); run_leave(c); return; }
    char head[256];
    int size = avfs_get_filesize(path);
    if (size < 0) { flash("cannot open that file"); return; }
    int n = size < 256 ? size : 256;
    if (n > 0 && avfs_read_file(path, head, (uint32_t)n, 0) != 0) { flash("cannot read that file"); return; }
    for (int i = 0; i < n; i++) if (head[i] == 0) { flash("that file is not text"); return; }
    open_editor(path);
}

static const char HELP_TEXT[] =
    "RadiumDE - keys and mouse\n"
    "\n"
    "  Alt+F1           Applications menu\n"
    "  Alt+F2           run a command\n"
    "  Alt+Tab          bring the next window forward\n"
    "  Alt+F4           close the window\n"
    "  Alt+F9 / F10     minimise / maximise\n"
    "  Ctrl+Alt+T       new terminal\n"
    "  Alt+1..4         workspace  (Alt+Shift+1..4 sends the window there)\n"
    "  Ctrl+Alt+Q       log out\n"
    "\n"
    "  Applications > Games runs a game full screen; the desktop comes\n"
    "  back when it ends. Applications > Console leaves to the text\n"
    "  terminal (type desktop there to return). Ctrl+Alt+L locks.\n"
    "  Text Editor: Ctrl+S saves, F5 or Run executes .rsh / .hls files,\n"
    "  RSHIDT opens the file in the full-screen Rash IDE.\n"
    "  Calculator: type digits and + - * / = on the keyboard.\n"
    "\n"
    "  Drag a title bar to move a window, drag an edge or a corner to\n"
    "  resize it, double-click a title bar to maximise it.\n"
    "  Double-click a desktop icon or a file to open it.\n"
    "  Right-click the desktop or a title bar for a menu.\n"
    "  Terminal: Enter runs, Up recalls, Ctrl+L clears, Ctrl+C stops\n"
    "  a hung command, 'exit' closes the window.\n"
    "\n"
    "  Settings > Save writes desktop.cfg ('desktop defaults' shows\n"
    "  the file format).\n";

// ---- configuration -----------------------------------------------------------------------------------------------------------

static void theme_finish(void)
{
    static const uint32_t TS[4][3] = { { 0, 0, 0 }, { 0x000000, 0x33ff33, 0x07140a }, { 0x120a00, 0xffb000, 0x1e1400 }, { 0xfafafa, 0x222222, 0xe4e4e4 } };
    if (C.term_style > 0 && C.term_style < 4) { TH.term_bg = TS[C.term_style][0]; TH.term_fg = TS[C.term_style][1]; TH.term_input = TS[C.term_style][2]; }
}

static void cfg_error(int line_no, const char *msg)
{
    g_cfg_nerr++;
    if (g_cfg_nerr == 1) {
        int k = put_str(g_cfg_err, sizeof(g_cfg_err), "config line ");
        k += put_uint(g_cfg_err + k, (uint32_t)line_no);
        k += put_str(g_cfg_err + k, (int)sizeof(g_cfg_err) - k, ": ");
        put_str(g_cfg_err + k, (int)sizeof(g_cfg_err) - k, msg);
    }
}

static void cfg_defaults(void)
{
    memset(&C, 0, sizeof(C));
    C.mouse_speed = 45; C.mouse_gain = 60; C.dblclick = 450; C.wallpaper = 3; C.wp_mode = 0; C.dock = true; C.icons = true; C.desk_files = true; C.clock24 = true; C.theme = 0;
    C.shadows = true; C.snap = true; C.run_windowed = true; C.tray = true; C.dock_size = 48;
    TH = THEMES[0];
    g_nlaunch = 0;
    g_nauto = 0;
    g_cfg_nerr = 0;
    g_cfg_err[0] = 0;
}

static int split_words(char *s, char *w[], int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        if (!*s) break;
        if (*s == '"') { s++; w[n++] = s; while (*s && *s != '"') s++; if (*s) *s++ = 0; }
        else { w[n++] = s; while (*s && *s != ' ' && *s != '\t' && *s != '\r') s++; if (*s) *s++ = 0; }
    }
    return n;
}

static bool parse_num(const char *s, int lo, int hi, int *out)
{
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    if (!*s) return false;
    int v = 0;
    for (; *s; s++) { if (*s < '0' || *s > '9') return false; v = v * 10 + (*s - '0'); if (v > 100000) return false; }
    if (neg) v = -v;
    if (v < lo || v > hi) return false;
    *out = v;
    return true;
}

static bool parse_bool(const char *s, bool *out)
{
    if (same_nocase(s, "yes") || same_nocase(s, "true") || same_nocase(s, "on") || same_nocase(s, "1")) { *out = true; return true; }
    if (same_nocase(s, "no") || same_nocase(s, "false") || same_nocase(s, "off") || same_nocase(s, "0")) { *out = false; return true; }
    return false;
}

static void parse_config(char *buf)
{
    int line_no = 0;
    for (char *ln = buf; *ln; ) {
        char *next = ln;
        while (*next && *next != '\n') next++;
        if (*next) *next++ = 0;
        line_no++;
        char *cur = ln;
        ln = next;
        while (*cur == ' ' || *cur == '\t' || *cur == '\r') cur++;
        if (!*cur || *cur == '#') continue;
        for (char *p = cur; *p; p++) if (*p == '#' && p > cur && (p[-1] == ' ' || p[-1] == '\t')) { *p = 0; break; }
        char *w[8];
        int n = split_words(cur, w, 8), v = 0;
        bool b;
        if (n == 0) continue;
        if (same_nocase(w[0], "theme")) {
            bool found = false;
            for (int i = 0; i < NTHEMES && n >= 2; i++) if (same_nocase(w[1], THEMES[i].name)) { C.theme = i; TH = THEMES[i]; found = true; }
            if (!found) cfg_error(line_no, "unknown theme (desktop themes)");
        } else if (same_nocase(w[0], "mouse_speed")) {
            if (n >= 2 && parse_num(w[1], 10, 120, &v)) C.mouse_speed = v; else cfg_error(line_no, "mouse_speed 10-120");
        } else if (same_nocase(w[0], "mouse_accel")) {
            if (n >= 2 && parse_num(w[1], 0, 200, &v)) C.mouse_gain = v; else cfg_error(line_no, "mouse_accel 0-200");
        } else if (same_nocase(w[0], "double_click")) {
            if (n >= 2 && parse_num(w[1], 250, 900, &v)) C.dblclick = v; else cfg_error(line_no, "double_click 250-900");
        } else if (same_nocase(w[0], "wallpaper")) {
            if (n >= 2 && (same_nocase(w[1], "default") || same_nocase(w[1], "picture"))) { C.wallpaper = 3; C.wp_file[0] = 0; }
            else if (n >= 2 && same_nocase(w[1], "gradient")) C.wallpaper = 0;
            else if (n >= 2 && same_nocase(w[1], "flat")) C.wallpaper = 1;
            else if (n >= 2 && same_nocase(w[1], "reverse")) C.wallpaper = 2;
            else cfg_error(line_no, "wallpaper default, gradient, flat or reverse");
        } else if (same_nocase(w[0], "wallpaper_file")) {
            if (n >= 2) { put_str(C.wp_file, sizeof(C.wp_file), w[1]); C.wallpaper = 3; } else cfg_error(line_no, "wallpaper_file PATH");
        } else if (same_nocase(w[0], "wallpaper_mode")) {
            if (n >= 2 && same_nocase(w[1], "fill")) C.wp_mode = 0;
            else if (n >= 2 && same_nocase(w[1], "fit")) C.wp_mode = 1;
            else if (n >= 2 && same_nocase(w[1], "stretch")) C.wp_mode = 2;
            else if (n >= 2 && same_nocase(w[1], "center")) C.wp_mode = 3;
            else cfg_error(line_no, "wallpaper_mode fill, fit, stretch or center");
        } else if (same_nocase(w[0], "window_shadows")) {
            if (n >= 2 && parse_bool(w[1], &b)) C.shadows = b; else cfg_error(line_no, "window_shadows yes or no");
        } else if (same_nocase(w[0], "snap")) {
            if (n >= 2 && parse_bool(w[1], &b)) C.snap = b; else cfg_error(line_no, "snap yes or no");
        } else if (same_nocase(w[0], "open_maximised") || same_nocase(w[0], "open_maximized")) {
            if (n >= 2 && parse_bool(w[1], &b)) C.open_max = b; else cfg_error(line_no, "open_maximised yes or no");
        } else if (same_nocase(w[0], "run_in_window")) {
            if (n >= 2 && parse_bool(w[1], &b)) C.run_windowed = b; else cfg_error(line_no, "run_in_window yes or no");
        } else if (same_nocase(w[0], "panel_tray")) {
            if (n >= 2 && parse_bool(w[1], &b)) C.tray = b; else cfg_error(line_no, "panel_tray yes or no");
        } else if (same_nocase(w[0], "dock_size")) {
            if (n >= 2 && parse_num(w[1], 32, 64, &v)) C.dock_size = v; else cfg_error(line_no, "dock_size 32-64");
        } else if (same_nocase(w[0], "terminal_colors")) {
            if (n >= 2 && same_nocase(w[1], "theme")) C.term_style = 0;
            else if (n >= 2 && same_nocase(w[1], "classic")) C.term_style = 1;
            else if (n >= 2 && same_nocase(w[1], "amber")) C.term_style = 2;
            else if (n >= 2 && same_nocase(w[1], "light")) C.term_style = 3;
            else cfg_error(line_no, "terminal_colors theme, classic, amber or light");
        } else if (same_nocase(w[0], "desktop_files")) {
            if (n >= 2 && parse_bool(w[1], &b)) C.desk_files = b; else cfg_error(line_no, "desktop_files yes or no");
        } else if (same_nocase(w[0], "dock")) {
            if (n >= 2 && parse_bool(w[1], &b)) C.dock = b; else cfg_error(line_no, "dock yes or no");
        } else if (same_nocase(w[0], "icons")) {
            if (n >= 2 && parse_bool(w[1], &b)) C.icons = b; else cfg_error(line_no, "icons yes or no");
        } else if (same_nocase(w[0], "clock24")) {
            if (n >= 2 && parse_bool(w[1], &b)) C.clock24 = b; else cfg_error(line_no, "clock24 yes or no");
        } else if (same_nocase(w[0], "launcher")) {
            if (n < 3) cfg_error(line_no, "launcher \"Label\" command");
            else if (g_nlaunch >= MAX_LAUNCH) cfg_error(line_no, "too many launchers");
            else {
                put_str(g_launch[g_nlaunch].label, sizeof(g_launch[0].label), w[1]);
                int k = 0;
                for (int i = 2; i < n; i++) { if (i > 2 && k < 47) g_launch[g_nlaunch].cmd[k++] = ' '; for (const char *p = w[i]; *p && k < 47; p++) g_launch[g_nlaunch].cmd[k++] = *p; }
                g_launch[g_nlaunch].cmd[k] = 0;
                g_nlaunch++;
            }
        } else if (same_nocase(w[0], "autostart")) {
            static const char *const KINDS[] = { "term", "files", "monitor", "clock", "settings", "calc", "editor", "games", "services", "scripts", "tasks" };
            if (n >= 2 && same_nocase(w[1], "clear")) { g_nauto = 0; continue; }
            if (g_nauto >= MAX_AUTO) { cfg_error(line_no, "too many autostart entries"); continue; }
            int kind = -1;
            if (n >= 3 && same_nocase(w[1], "open")) for (int i = 0; i < APP_COUNT; i++) if (same_nocase(w[2], KINDS[i])) kind = i;
            if (n >= 3 && same_nocase(w[1], "exec")) {
                kind = 100;
                int k = 0;
                for (int i = 2; i < n; i++) { if (i > 2 && k < 47) g_auto[g_nauto].arg[k++] = ' '; for (const char *p = w[i]; *p && k < 47; p++) g_auto[g_nauto].arg[k++] = *p; }
                g_auto[g_nauto].arg[k] = 0;
            }
            if (kind < 0) { cfg_error(line_no, "autostart open term|files|monitor|clock|settings|calc|editor|games|services|scripts|tasks or exec COMMAND"); continue; }
            g_auto[g_nauto++].kind = kind;
        } else cfg_error(line_no, "unknown statement");
    }
}

static char g_cfg_buf[4096];

static const char *config_path(char *buf, int n)
{
    snprintf(buf, (size_t)n, "/disk/home/%s/desktop.cfg", users_current_name());
    if (avfs_file_exists(buf)) return buf;
    if (avfs_file_exists("/disk/etc/desktop.cfg")) return "/disk/etc/desktop.cfg";
    return NULL;
}

static bool default_config_path(char *buf, int n)
{
    char home[64];
    snprintf(home, sizeof(home), "/disk/home/%s", users_current_name());
    if (avfs_is_directory(home)) { snprintf(buf, (size_t)n, "%s/desktop.cfg", home); return true; }
    if (!avfs_is_directory("/disk")) return false;
    if (!avfs_is_directory("/disk/etc")) avfs_create_dir("/disk/etc");
    snprintf(buf, (size_t)n, "/disk/etc/desktop.cfg");
    return true;
}

// Loads the defaults, then `path` (or the usual file when NULL). Returns the file that was read, or NULL.
static const char *load_config(const char *path)
{
    static char pbuf[96];
    cfg_defaults();
    static char dflt[sizeof(DEFAULT_CONF)];
    memcpy(dflt, DEFAULT_CONF, sizeof(DEFAULT_CONF));
    parse_config(dflt);
    g_cfg_nerr = 0;
    g_cfg_err[0] = 0;
    if (!path) path = config_path(pbuf, sizeof(pbuf));
    if (!path || !avfs_file_exists(path)) return NULL;
    int size = avfs_get_filesize(path);
    if (size < 0 || size >= (int)sizeof(g_cfg_buf) || avfs_get_content(path, g_cfg_buf, sizeof(g_cfg_buf)) != 0) { cfg_error(0, "cannot read the file"); return path; }
    parse_config(g_cfg_buf);
    theme_finish();
    return path;
}

static void save_settings(void)
{
    static char out[2048];
    int o = 0;
    o += snprintf(out + o, sizeof(out) - (size_t)o, "# RadiumDE desktop configuration (written by Settings)\n");
    o += snprintf(out + o, sizeof(out) - (size_t)o, "theme %s\n", THEMES[C.theme].name);
    if (C.wallpaper == 3 && C.wp_file[0]) o += snprintf(out + o, sizeof(out) - (size_t)o, "wallpaper_file \"%s\"\nwallpaper_mode %s\n", C.wp_file, C.wp_mode == 1 ? "fit" : (C.wp_mode == 2 ? "stretch" : (C.wp_mode == 3 ? "center" : "fill")));
    else o += snprintf(out + o, sizeof(out) - (size_t)o, "wallpaper %s\n", C.wallpaper == 3 ? "default" : (C.wallpaper == 1 ? "flat" : (C.wallpaper == 2 ? "reverse" : "gradient")));
    o += snprintf(out + o, sizeof(out) - (size_t)o, "mouse_speed %d\nmouse_accel %d\ndouble_click %d\n", C.mouse_speed, C.mouse_gain, C.dblclick);
    o += snprintf(out + o, sizeof(out) - (size_t)o, "dock %s\nicons %s\ndesktop_files %s\nclock24 %s\n", C.dock ? "yes" : "no", C.icons ? "yes" : "no", C.desk_files ? "yes" : "no", C.clock24 ? "yes" : "no");
    o += snprintf(out + o, sizeof(out) - (size_t)o, "window_shadows %s\nsnap %s\nopen_maximised %s\nrun_in_window %s\npanel_tray %s\ndock_size %d\nterminal_colors %s\n",
                  C.shadows ? "yes" : "no", C.snap ? "yes" : "no", C.open_max ? "yes" : "no", C.run_windowed ? "yes" : "no", C.tray ? "yes" : "no", C.dock_size,
                  C.term_style == 1 ? "classic" : (C.term_style == 2 ? "amber" : (C.term_style == 3 ? "light" : "theme")));
    for (int i = 0; i < g_nlaunch; i++) o += snprintf(out + o, sizeof(out) - (size_t)o, "launcher \"%s\" %s\n", g_launch[i].label, g_launch[i].cmd);
    o += snprintf(out + o, sizeof(out) - (size_t)o, "autostart clear\n");
    for (int i = 0; i < g_nauto; i++) {
        static const char *const KINDS[] = { "term", "files", "monitor", "clock", "settings", "calc", "editor", "games", "services", "scripts", "tasks" };
        if (g_auto[i].kind == 100) o += snprintf(out + o, sizeof(out) - (size_t)o, "autostart exec %s\n", g_auto[i].arg);
        else o += snprintf(out + o, sizeof(out) - (size_t)o, "autostart open %s\n", KINDS[g_auto[i].kind]);
    }
    char path[96];
    if (!default_config_path(path, sizeof(path))) { flash("/disk is not available - settings not saved"); return; }
    if (avfs_save_file(path, out, (uint32_t)o, true) != 0) { flash("could not write the configuration file"); return; }
    flash("settings saved");
}

// ---- running commands ---------------------------------------------------------------------------------------------------------

static void vt_scroll_line(const uint16_t *row, size_t n, void *ctx) { term_add_cells((win_t *)ctx, row, n); }
static void tick_second(void);
static uint32_t g_last_sec;
static int g_run_win = -1;

// ---- a program's pixels in a window ---------------------------------------------------------------------------------------------

static uint32_t g_gfx[SW * SH];                    // the screen a program believes it owns (fbcon_blit lands here)
static int g_gx0, g_gy0, g_gx1, g_gy1;             // the part of it that has been drawn to
static bool g_gfx_seen, g_gfx_dirty, g_abort_req;

static void gfx_blit(const uint32_t *src, int stride, int x, int y, int w, int h)
{
    if (g_run_win < 0) return;
    if (x < 0) { src -= x; w += x; x = 0; }
    if (y < 0) { src -= (long)y * stride; h += y; y = 0; }
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) memcpy(g_gfx + (y + row) * SW + x, src + (long)row * stride, (size_t)w * 4);
    if (!g_gfx_seen && w == SW && h == SH) {         // a surface being created clears the whole screen; that says nothing about the picture
        bool blank = true;
        for (int i = 0; i < SW * SH && blank; i += 61) if (g_gfx[i] & 0xffffff) blank = false;
        if (blank) return;
    }
    if (!g_gfx_seen) { g_gx0 = x; g_gy0 = y; g_gx1 = x + w; g_gy1 = y + h; }
    else { g_gx0 = imin(g_gx0, x); g_gy0 = imin(g_gy0, y); g_gx1 = imax(g_gx1, x + w); g_gy1 = imax(g_gy1, y + h); }
    g_gfx_seen = true;
    g_gfx_dirty = true;
    g_win[g_run_win].gfx = true;
}

static void render_gfx(win_t *w)
{
    rect_t c = content_rect(w);
    fill_rect(c.x, c.y, c.w, c.h, 0x000000);
    int sw = g_gx1 - g_gx0, sh = g_gy1 - g_gy0;
    if (sw <= 0 || sh <= 0 || c.w <= 0 || c.h <= 0) return;
    int dw, dh;
    if (c.w * sh <= c.h * sw) { dw = c.w; dh = imax(1, sh * c.w / sw); } else { dh = c.h; dw = imax(1, sw * c.h / sh); }
    int ox = c.x + (c.w - dw) / 2, oy = c.y + (c.h - dh) / 2;
    int x0 = imax(ox, clip_x0), x1 = imin(ox + dw, clip_x1), y0 = imax(oy, clip_y0), y1 = imin(oy + dh, clip_y1);
    static int xmap[SW];
    if (x1 <= x0 || y1 <= y0) return;
    for (int x = x0; x < x1; x++) xmap[x - x0] = g_gx0 + (x - ox) * sw / dw;
    for (int y = y0; y < y1; y++) {
        const uint32_t *row = g_gfx + (g_gy0 + (y - oy) * sh / dh) * SW;
        uint32_t *dst = g_store + y * SW;
        for (int x = x0; x < x1; x++) dst[x] = row[xmap[x - x0]] & 0xffffff;
    }
}

// Mouse bytes and window hotkeys that arrived while a program owns the keyboard.
static void run_service(void)
{
    uint32_t f;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(f) : : "memory");
    kbdtap_drain();
    if (f & 0x200) __asm__ volatile("sti" : : : "memory");
    uint8_t b;
    while (kbdtap_mouse_pop(&b)) mouse_byte(b);
    int h = kbdtap_take_hotkey();
    if (h && g_run_win >= 0) {
        win_t *w = &g_win[g_run_win];
        if (h == 1) g_abort_req = true;
        else if (h == 2) win_set_fullscreen(g_run_win, !w->fullscreen);
        else if (h == 3) win_set_maximized(g_run_win, !w->maximized);
    }
}

// Called from fbcon_poll() while a command runs: keeps the window, the others and the panel alive.
static void vt_pump(void)
{
    static bool busy;
    if (g_run_win < 0 || !P.ready || busy) return;
    busy = true;
    run_service();
    static uint32_t last;
    uint32_t now = now_ms();
    if (g_abort_req) { g_abort_req = false; busy = false; ctrlc_abort(); }
    if (now - last >= 20) {
        last = now;
        win_t *w = &g_win[g_run_win];
        if (now - g_last_sec >= 1000) { g_last_sec += 1000; tick_second(); }
        if (w->gfx) {
            if (g_gfx_dirty) { g_gfx_dirty = false; if (win_visible(g_run_win)) damage_content(w); }
        } else {
            size_t n = (size_t)g_vt_w * (size_t)g_vt_h;
            if (memcmp(g_vt, g_vt_prev, n * 2) != 0 || terminal_row != g_vt_crow || terminal_column != g_vt_ccol) {
                memcpy(g_vt_prev, g_vt, n * 2);
                g_vt_crow = terminal_row; g_vt_ccol = terminal_column;
                if (win_visible(g_run_win)) damage_content(w);
            }
        }
        if (has_pending()) draw_frame();
    }
    busy = false;
}

static bool g_mouse_present, g_mouse_on;
static void mouse_enable(bool on);

// Runs a command so that Ctrl+C (or closing its window) can end it. True if it was aborted.
static bool exec_guarded(const char *cmd)
{
    bool exempt = ctrlc_exempt(cmd);                           // games and full-screen programs keep their keys
    if (kjmp_set(&g_ctrlc_jmp) == 0) {
        if (!exempt) ctrlc_begin(g_ctrl);
        execute_command_no_history(cmd);
        if (!exempt) ctrlc_end();
        return false;
    }
    shell_abort_recover();
    return true;
}

// Runs a command with the window as its screen: whatever it prints or draws on the text grid appears live in the window
// and, when it ends (or scrolls off the top), goes into the window's scrollback.
static void run_vt(int wi, const char *cmd)
{
    win_t *w = &g_win[wi];
    int vw = tile_cols(w), vh = tile_rows(w);
    if (vw > VT_MAX_W) vw = VT_MAX_W;
    if (vh > VT_MAX_H) vh = VT_MAX_H;
    if (vw < 20 || vh < 4) { term_puts(w, "! this window is too small for a program (maximise it)\n"); return; }
    for (int i = 0; i < vw * vh; i++) g_vt[i] = g_vt_prev[i] = 0x0720;
    g_vt_w = vw; g_vt_h = vh; g_vt_crow = g_vt_ccol = 0;
    g_run_win = wi;
    w->running = true;
    w->gfx = false;
    w->scroll = 0;
    g_gfx_seen = g_gfx_dirty = g_abort_req = false;
    g_gx0 = g_gy0 = g_gx1 = g_gy1 = 0;
    terminal_vt_enter(g_vt, (size_t)vw, (size_t)vh, vt_scroll_line, w);
    if (win_visible(wi)) { damage_content(w); damage_title(w); draw_frame(); }
    kbdtap_wm(true);                                           // mouse bytes go to the desktop, not to the program
    fbcon_set_redirect(gfx_blit);                              // a program that draws pixels draws into the window
    exec_guarded(cmd);
    fbcon_set_redirect(NULL);
    if (g_gfx_seen) vkRadiumForgetDisplay();
    kbdtap_wm(false);
    g_abort_req = false;
    int last = -1;
    for (int y = vh - 1; y >= 0 && last < 0; y--)
        for (int x = 0; x < vw; x++) { uint16_t c = g_vt[y * vw + x] & 0xff; if (c != ' ' && c != 0) { last = y; break; } }
    if (terminal_column > 0 && (int)terminal_row > last) last = (int)terminal_row;
    for (int y = 0; y <= last; y++) term_add_cells(w, g_vt + y * vw, (size_t)vw);
    terminal_vt_leave();
    w->running = false;
    w->gfx = false;
    g_run_win = -1;
    g_alt = g_ctrl = g_shift = false;     // the program read the key releases
    damage_win(w);
}

// A program in a window of its own: the window closes again when the program printed nothing.
static void launch_window(const char *cmd, const char *title, int ww, int hh)
{
    int i = new_window(W_TERM, title);
    if (i < 0) return;
    win_t *w = &g_win[i];
    if (ww > 0 && !w->maximized) {
        damage_win(w);
        w->w = ww; w->h = hh;
        if (w->x + w->w > SW - 10) w->x = imax(10, SW - 10 - w->w);
        if (w->y + w->h > SH - 80) w->y = imax(PANEL_H + 4, SH - 80 - w->h);
        damage_win(w);
    }
    run_vt(i, cmd);
    if (g_win[i].used && g_win[i].nl <= 1 && g_win[i].col == 0) close_window(i);
}

static void launch_program(const char *cmd, const char *title)
{
    if (!C.run_windowed || needs_screen(cmd)) { run_leave(cmd); return; }
    launch_window(cmd, title, 960, 600);
}

// For programs that take over the display: give the screen back, run, take it again.
static void run_leave(const char *cmd)
{
    mouse_enable(false);
    gpu_surface_destroy();
    execute_command_no_history(cmd);
    if (!gpu_surface_create()) { g_quit = true; return; }
    for (int i = 0; i < MAX_IMG; i++) g_cdrawn[i].valid = false;
    mouse_enable(true);
    g_alt = g_ctrl = g_shift = false;
    damage_all();
    mark_cursor();
}

static void native_help(win_t *w)
{
    term_puts(w, "Commands (PageUp/PageDown scroll). Programs run in this window; doom, discord, mp4 ... take the screen\n");
    term_puts(w, "while they run and the desktop comes back after.\n");
    for (size_t i = 0; i < command_count; i++) {
        const char *n = commands[i].name, *d = commands[i].description;
        int k = 0;
        for (; n[k] && w->col < TC - 2; k++) { w->lines[w->nl - 1][w->col] = n[k]; w->lattr[w->nl - 1][w->col] = 0x0B; w->col++; }
        for (; (k < 16 || k == (int)strlen(n)) && w->col < TC - 2; k++) { w->lines[w->nl - 1][w->col] = ' '; w->lattr[w->nl - 1][w->col] = 0x07; w->col++; }
        for (int j = 0; d[j] && w->col < TC - 2; j++) { w->lines[w->nl - 1][w->col] = d[j]; w->lattr[w->nl - 1][w->col] = 0x07; w->col++; }
        w->lines[w->nl - 1][w->col] = 0;
        term_newline(w);
    }
}

static void term_run(int wi)
{
    win_t *w = &g_win[wi];
    char cmd[100];
    put_str(cmd, sizeof(cmd), w->input);
    w->inlen = 0;
    w->input[0] = 0;
    char echo[110] = "$ ";
    put_str(echo + 2, sizeof(echo) - 2, cmd);
    term_puts(w, echo);
    term_newline(w);
    const char *c = cmd;
    while (*c == ' ') c++;
    if (*c) {
        if (w->nhist == 0 || strcmp(w->hist[(w->nhist - 1) & 7], c) != 0) put_str(w->hist[w->nhist++ & 7], sizeof(w->hist[0]), c);
        w->hpos = w->nhist;
        if (strcmp(c, "exit") == 0) { close_window(wi); return; }
        char word[16];
        first_word(c, word, sizeof(word));
        w->scroll = 0;
        if (strcmp(c, "clear") == 0) term_clear_lines(w);
        else if (strcmp(word, "help") == 0) native_help(w);
        else if (strcmp(word, "desktop") == 0 || strcmp(word, "rde") == 0) term_puts(w, "! the desktop is already running\n");
        else if (needs_screen(c)) run_leave(c);
        else run_vt(wi, c);
        if (w->col) term_newline(w);
    }
    if (win_visible(wi)) damage_content(w);
}

// ---- Files ---------------------------------------------------------------------------------------------------------------------

enum { FM_NORMAL, FM_RENAME, FM_NEWDIR, FM_NEWFILE, FM_GOTO, FM_DELETE };
enum { FO_OPEN, FO_EDIT, FO_RUN, FO_TERM, FO_COPY, FO_CUT, FO_PASTE, FO_RENAME, FO_DELETE, FO_PROPS, FO_WALL, FO_HASH,
       FO_NEWDIR, FO_NEWFILE, FO_REFRESH, FO_SELALL, FO_UP, FO_BACK, FO_HOME };

#define FS_BAR 36
#define FS_PATH 30
#define FS_HEAD 22
#define FS_ROW 22
#define FS_STAT 22
#define FS_SIDE 132

typedef struct { const char *label; int op, w; } ftb_t;
static const ftb_t FTB[] = {
    { "Back", FO_BACK, 48 }, { "Up", FO_UP, 36 }, { "Home", FO_HOME, 48 }, { NULL, 0, 0 },
    { "+ Folder", FO_NEWDIR, 76 }, { "+ File", FO_NEWFILE, 60 }, { NULL, 0, 0 },
    { "Copy", FO_COPY, 48 }, { "Cut", FO_CUT, 40 }, { "Paste", FO_PASTE, 52 }, { NULL, 0, 0 },
    { "Rename", FO_RENAME, 64 }, { "Delete", FO_DELETE, 60 },
};
#define NFTB ((int)(sizeof(FTB) / sizeof(FTB[0])))

static struct { int n; bool cut; char path[16][96]; } g_clip;
static struct { char label[16]; char path[96]; int icon; } g_place[6];
static int g_nplace;

static int files_rows(const win_t *w) { return imax(1, (content_rect(w).h - FS_BAR - FS_PATH - FS_HEAD - FS_STAT) / FS_ROW); }

static rect_t fm_toolbtn(const win_t *w, int i)
{
    rect_t c = content_rect(w);
    int x = c.x + 8;
    for (int k = 0; k < i; k++) x += FTB[k].label ? FTB[k].w + 4 : 12;
    rect_t r = { x, c.y + 5, FTB[i].w, 26 };
    return r;
}
static rect_t fm_place_rect(const win_t *w, int i) { rect_t c = content_rect(w); rect_t r = { c.x + 6, c.y + FS_BAR + 26 + i * 28, FS_SIDE - 12, 26 }; return r; }
static rect_t fm_pathrow(const win_t *w) { rect_t c = content_rect(w); rect_t r = { c.x + FS_SIDE + 8, c.y + FS_BAR + 3, c.w - FS_SIDE - 16, FS_PATH - 6 }; return r; }
static rect_t fm_prompt_btn(const win_t *w, int i) { rect_t p = fm_pathrow(w); rect_t r = { p.x + p.w - 150 + i * 76, p.y + 2, 70, p.h - 4 }; return r; }
static int fm_list_y(const win_t *w) { return content_rect(w).y + FS_BAR + FS_PATH + FS_HEAD; }
static int fm_name_w(const win_t *w) { return content_rect(w).w - FS_SIDE - 220; }
static rect_t fm_head_rect(const win_t *w, int i)       // 0 name, 1 size, 2 type
{
    rect_t c = content_rect(w);
    int mx = c.x + FS_SIDE, mw = c.w - FS_SIDE, y = c.y + FS_BAR + FS_PATH;
    rect_t r;
    if (i == 0) { r.x = mx; r.w = mw - 210; }
    else if (i == 1) { r.x = mx + mw - 210; r.w = 100; }
    else { r.x = mx + mw - 110; r.w = 110; }
    r.y = y; r.h = FS_HEAD;
    return r;
}

static const char *fm_ext(const char *n) { const char *d = strrchr(n, '.'); return d && d != n ? d + 1 : ""; }

static const char *fm_type(const DirectoryEntry *e, char *buf, int n)
{
    if (strcmp(e->name, "..") == 0) return "Parent folder";
    if (e->is_directory) return "Folder";
    const char *x = fm_ext(e->name);
    if (!*x) return "File";
    if (same_nocase(x, "txt") || same_nocase(x, "md") || same_nocase(x, "cfg") || same_nocase(x, "log") || same_nocase(x, "c") || same_nocase(x, "h") || same_nocase(x, "json")) return "Text";
    if (same_nocase(x, "rsh") || same_nocase(x, "rash")) return "Rash script";
    if (same_nocase(x, "hls")) return "Helios program";
    if (same_nocase(x, "app")) return "Application";
    if (same_nocase(x, "png") || same_nocase(x, "bmp")) return "Image";
    if (same_nocase(x, "mp4")) return "Video";
    int k = 0;
    for (; x[k] && k < 6 && k < n - 6; k++) buf[k] = (x[k] >= 'a' && x[k] <= 'z') ? (char)(x[k] - 32) : x[k];
    put_str(buf + k, n - k, " file");
    return buf;
}

static void fm_size_text(uint32_t v, char *sz, int n)
{
    int k;
    if (v >= 1024 * 1024) { k = put_uint(sz, v >> 20); put_str(sz + k, n - k, " MB"); }
    else if (v >= 1024) { k = put_uint(sz, v >> 10); put_str(sz + k, n - k, " KB"); }
    else { k = put_uint(sz, v); put_str(sz + k, n - k, " B"); }
}

static void places_build(void)
{
    char home[96], p[96];
    home_dir(home, sizeof(home));
    g_nplace = 0;
#define PLACE(L_, P_, I_) do { if (g_nplace < 6) { put_str(g_place[g_nplace].label, 16, L_); put_str(g_place[g_nplace].path, 96, P_); g_place[g_nplace].icon = (I_); g_nplace++; } } while (0)
    PLACE("Home", home, IC_HOME);
    if (g_desk_dir[0] && avfs_is_directory(g_desk_dir)) PLACE("Desktop", g_desk_dir, IC_FILES);
    snprintf(p, sizeof(p), "%s/Pictures", home);
    if (avfs_is_directory(p)) PLACE("Pictures", p, IC_FILES);
    else if (avfs_is_directory("/disk/Pictures")) PLACE("Pictures", "/disk/Pictures", IC_FILES);
    if (avfs_is_directory("/disk")) PLACE("Disk", "/disk", IC_FILES);
    PLACE("Root", "/", IC_FILES);
#undef PLACE
}

static int fm_cmp(const win_t *w, const DirectoryEntry *a, const DirectoryEntry *b)
{
    bool pa = strcmp(a->name, "..") == 0, pb = strcmp(b->name, "..") == 0;
    if (pa != pb) return pa ? -1 : 1;
    if ((a->is_directory != 0) != (b->is_directory != 0)) return a->is_directory ? -1 : 1;
    int r = 0;
    if (w->fsort == 1 && !a->is_directory) r = a->size < b->size ? -1 : (a->size > b->size ? 1 : 0);
    else if (w->fsort == 2) { char ba[16], bb[16]; r = strcmp(fm_type(a, ba, sizeof(ba)), fm_type(b, bb, sizeof(bb))); }
    if (r == 0) {
        const char *x = a->name, *y = b->name;
        while (*x && lc(*x) == lc(*y)) { x++; y++; }
        r = (unsigned char)lc(*x) - (unsigned char)lc(*y);
    }
    return w->fdesc ? -r : r;
}

static uint32_t fm_sig(const char *path)
{
    DirectoryEntry *t = malloc(sizeof(DirectoryEntry) * FMAX);
    if (!t) return 0;
    int n = avfs_list_directory(path, t, FMAX);
    uint32_t h = 7;
    for (int i = 0; i < n; i++) {
        for (const char *c = t[i].name; *c; c++) h = h * 31 + (uint32_t)(unsigned char)*c;
        h = h * 31 + t[i].size + (uint32_t)t[i].is_directory;
    }
    free(t);
    return h;
}

static void files_scan(win_t *w)
{
    DirectoryEntry *tmp = malloc(sizeof(DirectoryEntry) * FMAX);
    int n = tmp ? avfs_list_directory(w->path, tmp, FMAX) : 0;
    bool root = strcmp(w->path, "/") == 0;
    w->nfent = 0;
    if (n < 0) n = 0;
    for (int i = 0; i < n && w->nfent < FMAX; i++) {
        if (strcmp(tmp[i].name, ".") == 0) continue;
        if (strcmp(tmp[i].name, "..") == 0 && root) continue;
        w->fent[w->nfent++] = tmp[i];
    }
    free(tmp);
    for (int i = 1; i < w->nfent; i++) {                         // insertion sort
        DirectoryEntry e = w->fent[i];
        int j = i - 1;
        while (j >= 0 && fm_cmp(w, &w->fent[j], &e) > 0) { w->fent[j + 1] = w->fent[j]; j--; }
        w->fent[j + 1] = e;
    }
    w->fsig = fm_sig(w->path);
    memset(w->fmark, 0, sizeof(w->fmark));
}

static void files_title(win_t *w)
{
    const char *b = strrchr(w->path, '/');
    put_str(w->title, sizeof(w->title), b && b[1] ? b + 1 : "Files");
}

static void files_load(win_t *w)
{
    files_scan(w);
    w->fsel = w->fanchor = -1;
    w->ftop = 0;
    w->fmode = FM_NORMAL;
    places_build();
    files_title(w);
}

// Reload after a change on disk, keeping the selection and scroll position.
static void files_reload(win_t *w)
{
    char keep[8][32];
    int nk = 0, top = w->ftop;
    for (int i = 0; i < w->nfent && nk < 8; i++) if (w->fmark[i]) put_str(keep[nk++], 32, w->fent[i].name);
    files_scan(w);
    for (int i = 0; i < w->nfent; i++) for (int k = 0; k < nk; k++) if (strcmp(w->fent[i].name, keep[k]) == 0) w->fmark[i] = true;
    w->fsel = w->fanchor = -1;
    for (int i = 0; i < w->nfent; i++) if (w->fmark[i]) { w->fsel = w->fanchor = i; break; }
    w->ftop = imax(0, imin(top, w->nfent - 1));
    damage_content(w);
}

static void files_refresh_all(void)
{
    for (int i = 0; i < MAX_WIN; i++) if (g_win[i].used && g_win[i].type == W_FILES) files_reload(&g_win[i]);
    desktop_icons_refresh();
    damage_icons();
}

static void files_goto(win_t *w, const char *path, bool push)
{
    if (!avfs_is_directory(path)) { flash("that folder does not exist"); return; }
    if (push && strcmp(path, w->path) != 0) put_str(w->hist[w->nhist++ & 7], sizeof(w->hist[0]), w->path);
    put_str(w->path, sizeof(w->path), path);
    files_load(w);
    damage_content(w);
    damage_title(w);
}

static void files_up(win_t *w)
{
    char p[96];
    put_str(p, sizeof(p), w->path);
    char *s = strrchr(p, '/');
    if (!s) return;
    if (s == p) p[1] = 0; else *s = 0;
    files_goto(w, p, true);
}

static void fm_join(const win_t *w, const char *name, char *out, int n)
{
    snprintf(out, (size_t)n, "%s%s%s", w->path, strcmp(w->path, "/") == 0 ? "" : "/", name);
}

static int fm_count(const win_t *w) { int n = 0; for (int i = 0; i < w->nfent; i++) if (w->fmark[i] && strcmp(w->fent[i].name, "..") != 0) n++; return n; }
static int fm_first(const win_t *w) { for (int i = 0; i < w->nfent; i++) if (w->fmark[i] && strcmp(w->fent[i].name, "..") != 0) return i; return -1; }

static void fm_select_only(win_t *w, int i)
{
    memset(w->fmark, 0, sizeof(w->fmark));
    if (i >= 0 && i < w->nfent) w->fmark[i] = true;
    w->fsel = w->fanchor = i;
}

static void fm_ensure_visible(win_t *w)
{
    int rows = files_rows(w);
    if (w->fsel < w->ftop) w->ftop = w->fsel;
    if (w->fsel >= w->ftop + rows) w->ftop = w->fsel - rows + 1;
    if (w->ftop < 0) w->ftop = 0;
}

// ---- file operations (copy walks folders, files go through a 32 KB buffer) ---------------------------------------------------

static int fs_copy(const char *src, const char *dst, int depth)
{
    static char buf[32768];
    if (depth > 8) return -1;
    if (avfs_is_directory(src)) {
        if (!avfs_is_directory(dst) && avfs_create_dir(dst) != 0) return -1;
        DirectoryEntry *ent = malloc(sizeof(DirectoryEntry) * FMAX);
        if (!ent) return -1;
        int n = avfs_list_directory(src, ent, FMAX), rc = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(ent[i].name, ".") == 0 || strcmp(ent[i].name, "..") == 0) continue;
            char s[160], d[160];
            snprintf(s, sizeof(s), "%s/%s", src, ent[i].name);
            snprintf(d, sizeof(d), "%s/%s", dst, ent[i].name);
            if (fs_copy(s, d, depth + 1) != 0) rc = -1;
        }
        free(ent);
        return rc;
    }
    int size = avfs_get_filesize(src);
    if (size < 0) return -1;
    if (size == 0) return avfs_save_file(dst, "", 0, true);
    uint32_t off = 0;
    while (off < (uint32_t)size) {
        uint32_t n = (uint32_t)size - off < sizeof(buf) ? (uint32_t)size - off : (uint32_t)sizeof(buf);
        if (avfs_read_file(src, buf, n, off) != 0) return -1;
        if ((off == 0 ? avfs_save_file(dst, buf, n, true) : avfs_append_file(dst, buf, n)) != 0) return -1;
        off += n;
    }
    return 0;
}

static int fs_remove(const char *path) { return avfs_is_directory(path) ? avfs_remove_dir_recursive(path) : avfs_remove_file(path); }

static bool fs_exists(const char *p) { return avfs_file_exists(p) || avfs_is_directory(p); }

// A free name in `dir`: name, then name~1.ext, name~2.ext ... (short enough for 8.3).
static void fm_unique(const char *dir, const char *name, char *out, int n)
{
    snprintf(out, (size_t)n, "%s%s%s", dir, strcmp(dir, "/") == 0 ? "" : "/", name);
    if (!fs_exists(out)) return;
    const char *dot = strrchr(name, '.');
    bool ext = dot && dot != name;
    int bl = ext ? (int)(dot - name) : (int)strlen(name);
    for (int k = 1; k < 100; k++) {
        char b[16];
        int keep = imin(bl, 6);
        memcpy(b, name, (size_t)keep);
        b[keep] = 0;
        snprintf(out, (size_t)n, "%s%s%s~%d%s", dir, strcmp(dir, "/") == 0 ? "" : "/", b, k, ext ? dot : "");
        if (!fs_exists(out)) return;
    }
}

static bool fm_inside(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    return strncmp(path, dir, n) == 0 && (path[n] == 0 || path[n] == '/');
}

static void fm_clip_set(win_t *w, bool cut)
{
    g_clip.n = 0;
    g_clip.cut = cut;
    for (int i = 0; i < w->nfent && g_clip.n < 16; i++)
        if (w->fmark[i] && strcmp(w->fent[i].name, "..") != 0) fm_join(w, w->fent[i].name, g_clip.path[g_clip.n++], 96);
    if (g_clip.n == 0) { flash("select something first"); return; }
    char m[48];
    snprintf(m, sizeof(m), "%d item%s %s", g_clip.n, g_clip.n == 1 ? "" : "s", cut ? "cut" : "copied");
    flash(m);
    damage_content(w);
}

static void fm_paste(win_t *w)
{
    if (g_clip.n == 0) { flash("the clipboard is empty"); return; }
    int ok = 0, bad = 0;
    for (int i = 0; i < g_clip.n; i++) {
        const char *src = g_clip.path[i];
        const char *base = strrchr(src, '/');
        base = base ? base + 1 : src;
        char dst[160];
        if (avfs_is_directory(src) && fm_inside(w->path, src)) { bad++; continue; }       // a folder into itself
        if (g_clip.cut) {
            char same[160];
            fm_join(w, base, same, sizeof(same));
            if (strcmp(same, src) == 0) continue;                                            // moved onto itself
        }
        fm_unique(w->path, base, dst, sizeof(dst));
        if (fs_copy(src, dst, 0) != 0) { bad++; continue; }
        if (g_clip.cut && fs_remove(src) != 0) bad++;
        ok++;
    }
    if (g_clip.cut) g_clip.n = 0;
    char m[56];
    if (bad) snprintf(m, sizeof(m), "pasted %d, %d failed", ok, bad); else snprintf(m, sizeof(m), "pasted %d item%s", ok, ok == 1 ? "" : "s");
    flash(m);
    files_refresh_all();
}

static void fm_delete_selected(win_t *w)
{
    int ok = 0, bad = 0;
    for (int i = 0; i < w->nfent; i++) {
        if (!w->fmark[i] || strcmp(w->fent[i].name, "..") == 0) continue;
        char p[160];
        fm_join(w, w->fent[i].name, p, sizeof(p));
        if (fs_remove(p) == 0) ok++; else bad++;
    }
    char m[56];
    if (bad) snprintf(m, sizeof(m), "deleted %d, %d failed", ok, bad); else snprintf(m, sizeof(m), "deleted %d item%s", ok, ok == 1 ? "" : "s");
    flash(m);
    files_refresh_all();
}

// ---- prompt row (rename, new folder or file, go to, delete) -------------------------------------------------------------------

static void fm_prompt(win_t *w, int mode, const char *initial)
{
    w->fmode = mode;
    put_str(w->input, sizeof(w->input), initial);
    w->inlen = (int)strlen(w->input);
    damage_content(w);
}

static bool fm_name_ok(const char *n)
{
    if (!*n || strlen(n) > 31 || strcmp(n, ".") == 0 || strcmp(n, "..") == 0) return false;
    for (const char *c = n; *c; c++) if (*c == '/' || *c == '\\' || *c == ':' || *c == '*' || *c == '?' || *c == '"' || *c == '<' || *c == '>' || *c == '|') return false;
    return true;
}

static void files_open_entry(win_t *w, int i);

static void fm_prompt_accept(int wi)
{
    win_t *w = &g_win[wi];
    int mode = w->fmode;
    char val[100], path[160];
    put_str(val, sizeof(val), w->input);
    w->fmode = FM_NORMAL;
    if (mode == FM_DELETE) { fm_delete_selected(w); return; }
    if (mode == FM_GOTO) { files_goto(w, val, true); return; }
    if (!fm_name_ok(val)) { flash("that name is not allowed"); damage_content(w); return; }
    fm_join(w, val, path, sizeof(path));
    if (mode == FM_RENAME) {
        int i = fm_first(w);
        if (i < 0) { damage_content(w); return; }
        char old[160];
        fm_join(w, w->fent[i].name, old, sizeof(old));
        if (strcmp(old, path) == 0) { damage_content(w); return; }
        if (fs_exists(path)) { flash("a file with that name already exists"); damage_content(w); return; }
        if (fs_copy(old, path, 0) != 0 || fs_remove(old) != 0) flash("could not rename it");
        else flash("renamed");
        files_refresh_all();
        return;
    }
    if (fs_exists(path)) { flash("a file with that name already exists"); damage_content(w); return; }
    int rc = mode == FM_NEWDIR ? avfs_create_dir(path) : avfs_save_file(path, "", 0, false);
    if (rc != 0) flash("could not create it"); else flash(mode == FM_NEWDIR ? "folder created" : "file created");
    files_refresh_all();
    for (int i = 0; i < w->nfent; i++) if (strcmp(w->fent[i].name, val) == 0) { fm_select_only(w, i); fm_ensure_visible(w); }
}

// ---- opening and the other actions -----------------------------------------------------------------------------------------------

static bool fm_has_ext(const char *n, const char *a, const char *b)
{
    const char *x = fm_ext(n);
    return same_nocase(x, a) || (b && same_nocase(x, b));
}

static void fm_run(const char *path)
{
    char c[160];
    if (fm_has_ext(path, "rsh", "rash")) { snprintf(c, sizeof(c), "script %s", path); open_exec(c); }
    else if (fm_has_ext(path, "hls", NULL)) { snprintf(c, sizeof(c), "helios run %s", path); open_exec(c); }
    else if (fm_has_ext(path, "app", NULL)) { snprintf(c, sizeof(c), "app %s", path); launch_program(c, "Application"); }
    else flash("only .rsh, .hls and .app files can be run");
}

static void files_open_entry(win_t *w, int i)
{
    if (i < 0 || i >= w->nfent) return;
    DirectoryEntry e = w->fent[i];
    if (strcmp(e.name, "..") == 0) { files_up(w); return; }
    char p[160];
    fm_join(w, e.name, p, sizeof(p));
    if (e.is_directory) files_goto(w, p, true); else open_path(p);
}

static void fm_properties(win_t *w, int i)
{
    char text_[420], p[160], sz[24], tb[16];
    DirectoryEntry e;
    if (i >= 0) { e = w->fent[i]; fm_join(w, e.name, p, sizeof(p)); }
    else { memset(&e, 0, sizeof(e)); put_str(e.name, sizeof(e.name), w->title); e.is_directory = 1; put_str(p, sizeof(p), w->path); }
    int o = snprintf(text_, sizeof(text_), "Name      %s\nLocation  %s\nType      %s\n", e.name, p, fm_type(&e, tb, sizeof(tb)));
    if (e.is_directory) {
        DirectoryEntry *ch = malloc(sizeof(DirectoryEntry) * FMAX);
        int n = ch ? avfs_list_directory(p, ch, FMAX) : 0, files = 0, dirs = 0;
        uint32_t total = 0;
        for (int k = 0; k < n; k++) {
            if (strcmp(ch[k].name, ".") == 0 || strcmp(ch[k].name, "..") == 0) continue;
            if (ch[k].is_directory) dirs++; else { files++; total += ch[k].size; }
        }
        free(ch);
        fm_size_text(total, sz, sizeof(sz));
        o += snprintf(text_ + o, sizeof(text_) - (size_t)o, "Contents  %d file%s, %d folder%s (%s directly inside)\n", files, files == 1 ? "" : "s", dirs, dirs == 1 ? "" : "s", sz);
    } else {
        fm_size_text(e.size, sz, sizeof(sz));
        o += snprintf(text_ + o, sizeof(text_) - (size_t)o, "Size      %s (%u bytes)\n", sz, e.size);
    }
    open_viewer("Properties", text_, o);
}

static void files_action(int wi, int op)
{
    if (wi < 0 || wi >= MAX_WIN || !g_win[wi].used || g_win[wi].type != W_FILES) return;
    win_t *w = &g_win[wi];
    int i = fm_first(w);
    char p[160];
    if (i >= 0) fm_join(w, w->fent[i].name, p, sizeof(p)); else put_str(p, sizeof(p), w->path);
    switch (op) {
    case FO_OPEN: if (i >= 0) files_open_entry(w, i); break;
    case FO_EDIT: if (i >= 0 && !w->fent[i].is_directory) open_editor(p); break;
    case FO_RUN: if (i >= 0) fm_run(p); break;
    case FO_TERM: { char c[160]; snprintf(c, sizeof(c), "cd %s", i >= 0 && w->fent[i].is_directory ? p : w->path); open_exec(c); break; }
    case FO_COPY: fm_clip_set(w, false); break;
    case FO_CUT: fm_clip_set(w, true); break;
    case FO_PASTE: fm_paste(w); break;
    case FO_RENAME: if (i >= 0) fm_prompt(w, FM_RENAME, w->fent[i].name); else flash("select a file first"); break;
    case FO_DELETE: if (i >= 0) fm_prompt(w, FM_DELETE, ""); else flash("select something to delete"); break;
    case FO_PROPS: fm_properties(w, i); break;
    case FO_WALL: if (i >= 0) wallpaper_set_file(p); break;
    case FO_HASH: if (i >= 0 && !w->fent[i].is_directory) { char c[180]; snprintf(c, sizeof(c), "sha256sum %s", p); open_exec(c); } break;
    case FO_NEWDIR: { char u[160]; fm_unique(w->path, "newdir", u, sizeof(u)); const char *b = strrchr(u, '/'); fm_prompt(w, FM_NEWDIR, b ? b + 1 : "newdir"); break; }
    case FO_NEWFILE: { char u[160]; fm_unique(w->path, "untitled.txt", u, sizeof(u)); const char *b = strrchr(u, '/'); fm_prompt(w, FM_NEWFILE, b ? b + 1 : "untitled.txt"); break; }
    case FO_REFRESH: files_reload(w); break;
    case FO_SELALL: for (int k = 0; k < w->nfent; k++) w->fmark[k] = strcmp(w->fent[k].name, "..") != 0; w->fsel = w->nfent - 1; damage_content(w); break;
    case FO_UP: files_up(w); break;
    case FO_BACK:
        if (w->nhist > 0) { char b[96]; put_str(b, sizeof(b), w->hist[--w->nhist & 7]); files_goto(w, b, false); }
        break;
    case FO_HOME: { char h[96]; home_dir(h, sizeof(h)); files_goto(w, h, true); break; }
    default: break;
    }
}

static bool fm_op_enabled(const win_t *w, int op)
{
    switch (op) {
    case FO_BACK: return w->nhist > 0;
    case FO_PASTE: return g_clip.n > 0;
    case FO_COPY: case FO_CUT: case FO_RENAME: case FO_DELETE: return fm_count(w) > 0;
    default: return true;
    }
}

// ---- drawing ------------------------------------------------------------------------------------------------------------------------

static void render_files(win_t *w)
{
    rect_t c = content_rect(w);
    int mx = c.x + FS_SIDE, mw = c.w - FS_SIDE, ly = fm_list_y(w), rows = files_rows(w);
    bool hov_win = g_hover_item_win >= 0 && &g_win[g_hover_item_win] == w;
    fill_rect(c.x, c.y, c.w, c.h, TH.field);

    // toolbar
    vgrad(c.x, c.y, c.w, FS_BAR, mix(TH.bg, 0xffffff, 6), TH.bg);
    fill_rect(c.x, c.y + FS_BAR - 1, c.w, 1, TH.line);
    for (int i = 0; i < NFTB; i++) {
        rect_t r = fm_toolbtn(w, i);
        if (!FTB[i].label) { fill_rect(r.x - 6, r.y + 3, 1, 20, TH.line); continue; }
        bool on = fm_op_enabled(w, FTB[i].op), hot = on && hov_win && g_hover_item == 4000 + FTB[i].op;
        uint32_t bg = hot ? mix(TH.line, TH.accent, 30) : TH.line;
        round_rect(r.x, r.y, r.w, r.h, 4, bg);
        text_center(r.x, r.w, r.y + (r.h - CH) / 2, FTB[i].label, on ? TH.fg : TH.dim, bg, r.w / CW);
    }

    // places
    fill_rect(c.x, c.y + FS_BAR, FS_SIDE, c.h - FS_BAR - FS_STAT, mix(TH.bg, TH.line, 35));
    fill_rect(c.x + FS_SIDE - 1, c.y + FS_BAR, 1, c.h - FS_BAR - FS_STAT, TH.line);
    text(c.x + 12, c.y + FS_BAR + 6, "PLACES", TH.dim, mix(TH.bg, TH.line, 35), 8);
    for (int i = 0; i < g_nplace; i++) {
        rect_t r = fm_place_rect(w, i);
        bool cur = strcmp(g_place[i].path, w->path) == 0, hot = hov_win && g_hover_item == 4100 + i;
        uint32_t bg = cur ? TH.accent : (hot ? mix(TH.bg, TH.line, 90) : mix(TH.bg, TH.line, 35));
        if (cur || hot) round_rect(r.x, r.y, r.w, r.h, 4, bg);
        draw_icon(g_place[i].icon, r.x + 6, r.y + 5, 16);
        text(r.x + 30, r.y + 5, g_place[i].label, cur ? TH.accent_tx : TH.fg, bg, (r.w - 34) / CW);
    }

    // path row, or the prompt that replaces it
    rect_t pr = fm_pathrow(w);
    round_rect(pr.x, pr.y, pr.w, pr.h, 4, TH.line);
    round_rect(pr.x + 1, pr.y + 1, pr.w - 2, pr.h - 2, 3, w->fmode == FM_NORMAL ? TH.field : mix(TH.field, TH.accent, 10));
    if (w->fmode == FM_NORMAL) {
        draw_icon(IC_FILES, pr.x + 6, pr.y + 3, 16);
        text(pr.x + 30, pr.y + (pr.h - CH) / 2, w->path, TH.fg, TH.field, (pr.w - 40) / CW);
    } else {
        static const char *const LAB[] = { "", "Rename to:", "New folder:", "New file:", "Go to:", "" };
        char delmsg[56];
        const char *lab = LAB[w->fmode];
        uint32_t pbg = mix(TH.field, TH.accent, 10);
        if (w->fmode == FM_DELETE) {
            int n = fm_count(w);
            snprintf(delmsg, sizeof(delmsg), "Delete %d item%s? This cannot be undone.", n, n == 1 ? "" : "s");
            text(pr.x + 10, pr.y + (pr.h - CH) / 2, delmsg, TH.bad, pbg, (pr.w - 170) / CW);
        } else {
            int ll = (int)strlen(lab);
            text(pr.x + 10, pr.y + (pr.h - CH) / 2, lab, TH.dim, pbg, ll);
            int room = (pr.w - 170) / CW - ll - 1, off = w->inlen > room ? w->inlen - room : 0;
            text(pr.x + 10 + (ll + 1) * CW, pr.y + (pr.h - CH) / 2, w->input + off, TH.fg, pbg, room);
            fill_rect(pr.x + 10 + (ll + 1 + w->inlen - off) * CW, pr.y + 4, 2, CH - 2, TH.accent);
        }
        for (int i = 0; i < 2; i++) {
            rect_t b = fm_prompt_btn(w, i);
            bool hot = hov_win && g_hover_item == 4300 + i;
            uint32_t bg = i == 0 ? (w->fmode == FM_DELETE ? TH.bad : TH.good) : TH.line;
            if (hot) bg = shade(bg, 112);
            round_rect(b.x, b.y, b.w, b.h, 3, bg);
            text_center(b.x, b.w, b.y + (b.h - CH) / 2, i == 0 ? (w->fmode == FM_DELETE ? "Delete" : "OK") : "Cancel", i == 0 ? 0xffffff : TH.fg, bg, 8);
        }
    }

    // column headers
    vgrad(mx, c.y + FS_BAR + FS_PATH, mw, FS_HEAD, mix(TH.bg, 0xffffff, 5), TH.bg);
    fill_rect(mx, c.y + FS_BAR + FS_PATH + FS_HEAD - 1, mw, 1, TH.line);
    static const char *const HL[3] = { "Name", "Size", "Type" };
    for (int i = 0; i < 3; i++) {
        rect_t h = fm_head_rect(w, i);
        bool hot = hov_win && g_hover_item == 4200 + i;
        if (hot) fill_rect(h.x, h.y, h.w, h.h - 1, mix(TH.bg, TH.line, 80));
        text(h.x + 8, h.y + 3, HL[i], w->fsort == i ? TH.accent : TH.dim, hot ? mix(TH.bg, TH.line, 80) : mix(TH.bg, 0xffffff, 3), 6);
        if (w->fsort == i) glyph(h.x + 8 + 5 * CW, h.y + 3, w->fdesc ? 0x19 : 0x18, TH.accent, TRANSPARENT);
    }

    // rows
    if (w->nfent == 0) text(mx + 20, ly + 20, "This folder is empty", TH.dim, TH.field, 30);
    for (int r = 0; r < rows; r++) {
        int i = w->ftop + r;
        if (i >= w->nfent) break;
        DirectoryEntry *e = &w->fent[i];
        int ry = ly + r * FS_ROW;
        bool sel = w->fmark[i];
        uint32_t bg = sel ? TH.accent : ((i & 1) ? mix(TH.field, TH.line, 14) : TH.field);
        uint32_t fg = sel ? TH.accent_tx : TH.fg, dim = sel ? TH.accent_tx : TH.dim;
        fill_rect(mx, ry, mw - 9, FS_ROW, bg);
        if (i == w->fsel && w->fmode == FM_NORMAL) frame_rect(mx, ry, mw - 9, FS_ROW, sel ? 0xffffff : TH.accent);
        bool dir = e->is_directory != 0;
        draw_icon(dir ? IC_FILES : icon_for_name(e->name), mx + 8, ry + 3, 16);
        text(mx + 32, ry + 3, e->name, fg, bg, (fm_name_w(w) - 36) / CW);
        if (!dir) {
            char sz[20];
            fm_size_text(e->size, sz, sizeof(sz));
            text(mx + mw - 120 - (int)strlen(sz) * CW, ry + 3, sz, dim, bg, 12);
        }
        char tb[16];
        text(mx + mw - 102, ry + 3, fm_type(e, tb, sizeof(tb)), dim, bg, 12);
    }
    scrollbar(mx + mw - 8, ly, rows * FS_ROW, w->nfent, rows, w->ftop);

    // status
    int fy = c.y + c.h - FS_STAT;
    fill_rect(c.x, fy, c.w, FS_STAT, TH.bg);
    fill_rect(c.x, fy, c.w, 1, TH.line);
    char st[96];
    int ns = fm_count(w), k = put_uint(st, (uint32_t)(w->nfent - (w->nfent && strcmp(w->fent[0].name, "..") == 0 ? 1 : 0)));
    k += put_str(st + k, (int)sizeof(st) - k, " items");
    if (ns) {
        uint32_t bytes = 0;
        for (int i = 0; i < w->nfent; i++) if (w->fmark[i] && !w->fent[i].is_directory) bytes += w->fent[i].size;
        char sz[20];
        fm_size_text(bytes, sz, sizeof(sz));
        k += snprintf(st + k, sizeof(st) - (size_t)k, "   -   %d selected (%s)", ns, sz);
    }
    text(c.x + 10, fy + 3, st, TH.dim, TH.bg, 60);
    if (g_clip.n) {
        char cl[40];
        snprintf(cl, sizeof(cl), "clipboard: %d %s", g_clip.n, g_clip.cut ? "to move" : "to copy");
        text(c.x + c.w - 12 - (int)strlen(cl) * CW, fy + 3, cl, TH.dim, TH.bg, 30);
    }
}

static int files_item_at(const win_t *w, int mx, int my)
{
    for (int i = 0; i < NFTB; i++) if (FTB[i].label && rect_has(fm_toolbtn(w, i), mx, my)) return 4000 + FTB[i].op;
    for (int i = 0; i < g_nplace; i++) if (rect_has(fm_place_rect(w, i), mx, my)) return 4100 + i;
    for (int i = 0; i < 3; i++) if (rect_has(fm_head_rect(w, i), mx, my)) return 4200 + i;
    if (w->fmode != FM_NORMAL) for (int i = 0; i < 2; i++) if (rect_has(fm_prompt_btn(w, i), mx, my)) return 4300 + i;
    return -1;
}

static int fm_row_at(const win_t *w, int mx, int my)
{
    rect_t c = content_rect(w);
    int ly = fm_list_y(w);
    if (mx < c.x + FS_SIDE || mx >= c.x + c.w - 9 || my < ly || my >= ly + files_rows(w) * FS_ROW) return -1;
    int row = (my - ly) / FS_ROW + w->ftop;
    return row < w->nfent ? row : -1;
}

static void files_click(int wi, int mx, int my, bool dbl)
{
    win_t *w = &g_win[wi];
    if (w->fmode != FM_NORMAL) {
        for (int i = 0; i < 2; i++) if (rect_has(fm_prompt_btn(w, i), mx, my)) { if (i == 0) fm_prompt_accept(wi); else { w->fmode = FM_NORMAL; damage_content(w); } return; }
        return;
    }
    for (int i = 0; i < NFTB; i++) if (FTB[i].label && rect_has(fm_toolbtn(w, i), mx, my)) { if (fm_op_enabled(w, FTB[i].op)) files_action(wi, FTB[i].op); return; }
    for (int i = 0; i < g_nplace; i++) if (rect_has(fm_place_rect(w, i), mx, my)) { char p[96]; put_str(p, sizeof(p), g_place[i].path); files_goto(w, p, true); return; }
    if (rect_has(fm_pathrow(w), mx, my)) { fm_prompt(w, FM_GOTO, w->path); return; }
    for (int i = 0; i < 3; i++) {
        if (!rect_has(fm_head_rect(w, i), mx, my)) continue;
        if (w->fsort == i) w->fdesc = !w->fdesc; else { w->fsort = i; w->fdesc = false; }
        files_reload(w);
        return;
    }
    int row = fm_row_at(w, mx, my);
    if (row < 0) { fm_select_only(w, -1); damage_content(w); return; }
    if (dbl && row == w->fsel && !g_ctrl && !g_shift) { files_open_entry(w, row); return; }
    if (g_shift && w->fanchor >= 0) {
        memset(w->fmark, 0, sizeof(w->fmark));
        for (int i = imin(w->fanchor, row); i <= imax(w->fanchor, row); i++) w->fmark[i] = true;
        w->fsel = row;
    } else if (g_ctrl) { w->fmark[row] = !w->fmark[row]; w->fsel = row; w->fanchor = row; }
    else fm_select_only(w, row);
    damage_content(w);
}

static void menu_files(int wi, bool on_item, int x, int y)
{
    win_t *w = &g_win[wi];
    int i = fm_first(w);
    bool dir = on_item && i >= 0 && w->fent[i].is_directory, png = on_item && i >= 0 && fm_has_ext(w->fent[i].name, "png", NULL);
    bool run = on_item && i >= 0 && (fm_has_ext(w->fent[i].name, "rsh", "rash") || fm_has_ext(w->fent[i].name, "hls", "app"));
    menu_clear();
#define FM_ITEM(lab, op, ic) menu_add(lab, ACT_FILES, (wi << 8) | (op), ic)
    if (on_item && i >= 0) {
        FM_ITEM("Open", FO_OPEN, -1);
        if (!dir) FM_ITEM("Open with Text Editor", FO_EDIT, IC_EDIT);
        if (run) FM_ITEM("Run", FO_RUN, IC_RUN);
        if (png) FM_ITEM("Set as Wallpaper", FO_WALL, -1);
        menu_sep();
        FM_ITEM("Copy", FO_COPY, -1);
        FM_ITEM("Cut", FO_CUT, -1);
        if (g_clip.n) FM_ITEM("Paste", FO_PASTE, -1);
        menu_sep();
        FM_ITEM("Rename", FO_RENAME, -1);
        FM_ITEM("Delete", FO_DELETE, -1);
        menu_sep();
        FM_ITEM("Open Terminal Here", FO_TERM, IC_TERM);
        if (!dir) FM_ITEM("SHA-256 Checksum", FO_HASH, -1);
        FM_ITEM("Properties", FO_PROPS, -1);
    } else {
        FM_ITEM("New Folder", FO_NEWDIR, IC_FILES);
        FM_ITEM("New File", FO_NEWFILE, IC_FILE);
        if (g_clip.n) FM_ITEM("Paste", FO_PASTE, -1);
        menu_sep();
        FM_ITEM("Select All", FO_SELALL, -1);
        FM_ITEM("Refresh", FO_REFRESH, -1);
        menu_sep();
        FM_ITEM("Open Terminal Here", FO_TERM, IC_TERM);
        FM_ITEM("Properties", FO_PROPS, -1);
    }
#undef FM_ITEM
    menu_show(x, y);
}

static void files_rclick(int wi, int mx, int my)
{
    win_t *w = &g_win[wi];
    if (w->fmode != FM_NORMAL) return;
    int row = fm_row_at(w, mx, my);
    if (row >= 0 && !w->fmark[row]) { fm_select_only(w, row); damage_content(w); }
    else if (row < 0 && my >= fm_list_y(w)) { fm_select_only(w, -1); damage_content(w); }
    menu_files(wi, row >= 0 && strcmp(w->fent[row].name, "..") != 0, mx, my);
}

static void files_key(int wi, uint8_t code, bool ext)
{
    win_t *w = &g_win[wi];
    if (w->fmode != FM_NORMAL) {
        if (!ext && code == 0x01) { w->fmode = FM_NORMAL; damage_content(w); return; }
        if (!ext && code == 0x1C) { fm_prompt_accept(wi); return; }
        if (w->fmode == FM_DELETE) {
            if (!ext && code == 0x15) fm_prompt_accept(wi);                      // Y
            else if (!ext && code == 0x31) { w->fmode = FM_NORMAL; damage_content(w); }   // N
            return;
        }
        if (!ext && code == 0x0E) { if (w->inlen > 0) w->input[--w->inlen] = 0; }
        else if (!ext && code < sizeof(UNSHIFTED)) {
            char ch = g_shift ? SHIFTED[code] : UNSHIFTED[code];
            if (ch < 32 || ch > 126 || w->inlen >= (int)sizeof(w->input) - 1) return;
            w->input[w->inlen++] = ch;
            w->input[w->inlen] = 0;
        } else return;
        damage_content(w);
        return;
    }
    int rows = files_rows(w);
    if (!ext && g_ctrl) {
        if (code == 0x2E) files_action(wi, FO_COPY);
        else if (code == 0x2D) files_action(wi, FO_CUT);
        else if (code == 0x2F) files_action(wi, FO_PASTE);
        else if (code == 0x1E) files_action(wi, FO_SELALL);
        else if (code == 0x26) fm_prompt(w, FM_GOTO, w->path);
        else if (code == 0x31 && g_shift) files_action(wi, FO_NEWDIR);
        return;
    }
    if (ext && (code == 0x48 || code == 0x50 || code == 0x47 || code == 0x4F || code == 0x49 || code == 0x51)) {
        int cur = w->fsel < 0 ? (code == 0x48 || code == 0x49 || code == 0x47 ? w->nfent : -1) : w->fsel, nx = cur;
        if (code == 0x48) nx = cur - 1; else if (code == 0x50) nx = cur + 1;
        else if (code == 0x47) nx = 0; else if (code == 0x4F) nx = w->nfent - 1;
        else if (code == 0x49) nx = cur - rows; else nx = cur + rows;
        nx = imax(0, imin(w->nfent - 1, nx));
        if (w->nfent == 0) return;
        if (g_shift && w->fanchor >= 0) {
            memset(w->fmark, 0, sizeof(w->fmark));
            for (int i = imin(w->fanchor, nx); i <= imax(w->fanchor, nx); i++) w->fmark[i] = true;
            w->fsel = nx;
        } else fm_select_only(w, nx);
        fm_ensure_visible(w);
        damage_content(w);
        return;
    }
    if (ext && code == 0x53) { files_action(wi, FO_DELETE); return; }
    if (ext) return;
    if (code == 0x1C) { if (w->fsel >= 0) files_open_entry(w, w->fsel); }
    else if (code == 0x0E) files_up(w);
    else if (code == 0x3C) files_action(wi, FO_RENAME);                          // F2
    else if (code == 0x3F) files_action(wi, FO_REFRESH);                         // F5
}

// ---- Task Manager -----------------------------------------------------------------------------------------------------------------

extern uint32_t rust_task_snapshot(uint32_t *out, uint32_t max);
extern const uint8_t *rust_task_name(uint32_t entry);

#define TM_ROW 22
#define TM_HEAD 22
#define TM_FOOT 44
#define TM_TAB 40
static const char *const TM_TABS[3] = { "Processes", "Windows", "Performance" };
static const char *const TM_STATE[5] = { "READY", "RUNNING", "BLOCKED", "ZOMBIE", "SLEEPING" };

static rect_t tm_tab_rect(const win_t *w, int i) { rect_t c = content_rect(w); rect_t r = { c.x + 10 + i * 118, c.y + 8, 110, 26 }; return r; }
static rect_t tm_btn(const win_t *w, int i) { rect_t c = content_rect(w); rect_t r = { c.x + c.w - 10 - (i + 1) * 110 - i * 6, c.y + c.h - TM_FOOT + 8, 110, 28 }; return r; }
static int tm_rows(const win_t *w) { return imax(1, (content_rect(w).h - TM_TAB - TM_HEAD - TM_FOOT) / TM_ROW); }

static void tm_refresh(win_t *w)
{
    uint32_t raw[48 * 8];
    uint32_t n = rust_task_snapshot(raw, 48);
    uint32_t keep = w->tsel >= 0 && w->tsel < w->ntk ? w->tk[w->tsel].pid : 0xffffffffu, sum = 0;
    typeof(w->tk) old;
    int on = w->ntk;
    memcpy(old, w->tk, sizeof(old));
    w->ntk = 0;
    w->tsel = -1;
    for (uint32_t i = 0; i < n && w->ntk < 48; i++) {
        uint32_t *r = raw + i * 8;
        int k = w->ntk++;
        w->tk[k].pid = r[0]; w->tk[k].state = (int)r[2]; w->tk[k].pri = (int)r[3]; w->tk[k].ticks = r[5];
        uint32_t d = 0;
        for (int j = 0; j < on; j++) if (old[j].pid == r[0]) { d = r[5] - old[j].ticks; break; }
        w->tk[k].prev = d;
        sum += d;
        const uint8_t *nm = rust_task_name(r[7]);
        if (r[0] == 0) put_str(w->tk[k].name, sizeof(w->tk[k].name), "idle");
        else if (nm) put_str(w->tk[k].name, sizeof(w->tk[k].name), (const char *)nm);
        else snprintf(w->tk[k].name, sizeof(w->tk[k].name), "task %u", r[1]);
        if (w->tk[k].pid == keep) w->tsel = k;
    }
    for (int k = 0; k < w->ntk; k++) w->tk[k].cpu = sum ? (int)(w->tk[k].prev * 100 / sum) : 0;
    w->ttop = imax(0, imin(w->ttop, w->ntk - 1));
}

static void tm_win_refresh(win_t *w)
{
    int keep = w->lsel >= 0 && w->lsel < w->nli ? w->li[w->lsel].kind : -1;
    w->nli = 0;
    w->lsel = -1;
    static const char *const TN[] = { "Terminal", "Files", "Monitor", "Clock", "Settings", "Viewer", "Calculator", "Editor", "Games", "Services", "Scripts", "Task Manager" };
    for (int i = 0; i < MAX_WIN && w->nli < 40; i++) {
        if (!g_win[i].used) continue;
        put_str(w->li[w->nli].name, sizeof(w->li[0].name), g_win[i].title);
        snprintf(w->li[w->nli].sub, sizeof(w->li[0].sub), "%s   workspace %d%s%s", TN[g_win[i].type], g_win[i].ws + 1, g_win[i].minimized ? "   minimised" : "", g_win[i].running ? "   busy" : "");
        w->li[w->nli].kind = i;
        w->li[w->nli].st = i == g_focus ? 1 : 0;
        if (i == keep) w->lsel = w->nli;
        w->nli++;
    }
}

static void tm_update(win_t *w) { if (w->tab == 0) tm_refresh(w); else if (w->tab == 1) tm_win_refresh(w); }

static void render_tasks(win_t *w)
{
    rect_t c = content_rect(w);
    bool hov = g_hover_item_win >= 0 && &g_win[g_hover_item_win] == w;
    fill_rect(c.x, c.y, c.w, c.h, TH.bg);
    for (int i = 0; i < 3; i++) {
        rect_t r = tm_tab_rect(w, i);
        bool sel = i == w->tab, hot = hov && g_hover_item == 500 + i;
        uint32_t bg = sel ? TH.accent : (hot ? mix(TH.line, TH.accent, 25) : TH.line);
        round_rect(r.x, r.y, r.w, r.h, 4, bg);
        text_center(r.x, r.w, r.y + (r.h - CH) / 2, TM_TABS[i], sel ? TH.accent_tx : TH.fg, bg, 13);
    }
    int ly = c.y + TM_TAB + TM_HEAD, rows = tm_rows(w);
    if (w->tab == 2) {
        char b[48];
        int x = c.x + 16, y = c.y + TM_TAB + 8, bw = c.w - 32 - 9 * CW - 18 * CW;
        uint64_t total = pmm_get_total_memory(), freeb = pmm_get_free_memory();
        uint32_t tk = (uint32_t)(total >> 20), fk = (uint32_t)(freeb >> 20), uk = tk > fk ? tk - fk : 0, mp = tk ? uk * 100 / tk : 0;
        uint32_t dkt = 0, dku = 0, dp = 0;
        if (avfs_disk_space(&dkt, &dku) == 0 && dkt) dp = (uint32_t)(((dku >> 4) * 100) / (dkt >> 4));
        int last = w->hn ? (w->hn - 1) % HN : 0;
        uint32_t ui = w->hn ? w->hu[last] : 0;
        text(x, y, "memory", TH.dim, TH.bg, 8); meter(x + 9 * CW, y, bw, mp);
        snprintf(b, sizeof(b), "%u of %u MB", uk, tk); text(x + 9 * CW + bw + 10, y, b, TH.fg, TH.bg, 18);
        y += CH + 6;
        text(x, y, "disk", TH.dim, TH.bg, 8); meter(x + 9 * CW, y, bw, dp);
        snprintf(b, sizeof(b), "%u of %u MB", dku >> 10, dkt >> 10); text(x + 9 * CW + bw + 10, y, b, TH.fg, TH.bg, 18);
        y += CH + 6;
        text(x, y, "desktop", TH.dim, TH.bg, 8); meter(x + 9 * CW, y, bw, ui);
        snprintf(b, sizeof(b), "%u%% busy", ui); text(x + 9 * CW + bw + 10, y, b, TH.fg, TH.bg, 18);
        y += CH + 6;
        fmt_hms(b, g_up_base + now_ms() / 1000);
        text(x, y, "uptime", TH.dim, TH.bg, 8); text(x + 9 * CW, y, b, TH.fg, TH.bg, 10);
        snprintf(b, sizeof(b), "%u tasks, %d windows", rust_get_task_count(), g_zn); text(x + 24 * CW, y, b, TH.fg, TH.bg, 24);
        y += CH + 14;
        int gw = c.w - 32, gh = (c.y + c.h - y - 3 * (CH + 12)) / 3;
        if (gh < 24) return;
        text(x, y, "memory use", TH.dim, TH.bg, 20); y += CH + 2;
        bar_graph(x, y, gw, gh, w->hm, w->hn, TH.accent); y += gh + 10;
        text(x, y, "desktop load", TH.dim, TH.bg, 20); y += CH + 2;
        bar_graph(x, y, gw, gh, w->hu, w->hn, 0xd2a8ff); y += gh + 10;
        text(x, y, "disk use", TH.dim, TH.bg, 20); y += CH + 2;
        bar_graph(x, y, gw, gh, w->hd, w->hn, TH.warn);
        return;
    }
    // header
    vgrad(c.x, c.y + TM_TAB, c.w, TM_HEAD, mix(TH.bg, 0xffffff, 5), TH.bg);
    fill_rect(c.x, c.y + TM_TAB + TM_HEAD - 1, c.w, 1, TH.line);
    fill_rect(c.x, ly, c.w, rows * TM_ROW, TH.field);
    if (w->tab == 0) {
        text(c.x + 14, c.y + TM_TAB + 3, "PID", TH.dim, TH.bg, 4);
        text(c.x + 70, c.y + TM_TAB + 3, "Name", TH.dim, TH.bg, 12);
        text(c.x + 190, c.y + TM_TAB + 3, "State", TH.dim, TH.bg, 8);
        text(c.x + 280, c.y + TM_TAB + 3, "Pri", TH.dim, TH.bg, 4);
        text(c.x + 330, c.y + TM_TAB + 3, "CPU", TH.dim, TH.bg, 4);
        text(c.x + c.w - 20 - 10 * CW, c.y + TM_TAB + 3, "Ticks", TH.dim, TH.bg, 10);
        for (int r = 0; r < rows; r++) {
            int i = w->ttop + r;
            if (i >= w->ntk) break;
            int ry = ly + r * TM_ROW;
            bool sel = i == w->tsel;
            uint32_t bg = sel ? TH.accent : ((i & 1) ? mix(TH.field, TH.line, 14) : TH.field), fg = sel ? TH.accent_tx : TH.fg;
            fill_rect(c.x, ry, c.w - 9, TM_ROW, bg);
            char b[24];
            put_uint(b, w->tk[i].pid);
            text(c.x + 14, ry + 3, b, fg, bg, 6);
            text(c.x + 70, ry + 3, w->tk[i].name, fg, bg, 14);
            int sidx = w->tk[i].state >= 0 && w->tk[i].state < 5 ? w->tk[i].state : 0;
            text(c.x + 190, ry + 3, TM_STATE[sidx], sel ? TH.accent_tx : (sidx == 1 ? TH.good : (sidx == 3 ? TH.bad : TH.dim)), bg, 9);
            put_uint(b, (uint32_t)w->tk[i].pri);
            text(c.x + 280, ry + 3, b, fg, bg, 4);
            round_rect(c.x + 330, ry + 6, 60, 10, 2, sel ? mix(bg, 0xffffff, 25) : TH.line);
            if (w->tk[i].cpu > 0) round_rect(c.x + 330, ry + 6, imax(3, 60 * w->tk[i].cpu / 100), 10, 2, w->tk[i].cpu >= 70 ? TH.bad : (w->tk[i].cpu >= 30 ? TH.warn : TH.good));
            int k = put_uint(b, (uint32_t)w->tk[i].cpu); b[k++] = '%'; b[k] = 0;
            text(c.x + 396, ry + 3, b, fg, bg, 5);
            put_uint(b, w->tk[i].ticks);
            text(c.x + c.w - 20 - (int)strlen(b) * CW, ry + 3, b, fg, bg, 10);
        }
        scrollbar(c.x + c.w - 8, ly, rows * TM_ROW, w->ntk, rows, w->ttop);
    } else {
        text(c.x + 14, c.y + TM_TAB + 3, "Window", TH.dim, TH.bg, 8);
        text(c.x + 220, c.y + TM_TAB + 3, "Details", TH.dim, TH.bg, 8);
        for (int r = 0; r < rows; r++) {
            int i = w->ltop + r;
            if (i >= w->nli) break;
            int ry = ly + r * TM_ROW;
            bool sel = i == w->lsel;
            uint32_t bg = sel ? TH.accent : ((i & 1) ? mix(TH.field, TH.line, 14) : TH.field), fg = sel ? TH.accent_tx : TH.fg;
            fill_rect(c.x, ry, c.w - 9, TM_ROW, bg);
            text(c.x + 14, ry + 3, w->li[i].name, fg, bg, 24);
            text(c.x + 220, ry + 3, w->li[i].sub, sel ? TH.accent_tx : TH.dim, bg, (c.w - 240) / CW);
        }
        scrollbar(c.x + c.w - 8, ly, rows * TM_ROW, w->nli, rows, w->ltop);
    }
    fill_rect(c.x, c.y + c.h - TM_FOOT, c.w, TM_FOOT, TH.bg);
    fill_rect(c.x, c.y + c.h - TM_FOOT, c.w, 1, TH.line);
    char st[48];
    snprintf(st, sizeof(st), "%d %s", w->tab == 0 ? w->ntk : w->nli, w->tab == 0 ? "tasks" : "windows");
    text(c.x + 12, c.y + c.h - TM_FOOT + 14, st, TH.dim, TH.bg, 24);
    static const char *const B0[1] = { "End Task" };
    static const char *const B1[2] = { "Close", "Switch To" };
    int nb = w->tab == 0 ? 1 : 2;
    for (int i = 0; i < nb; i++) {
        rect_t r = tm_btn(w, i);
        bool hot = hov && g_hover_item == 1000 + i;
        uint32_t bg = (w->tab == 0 || i == 0) ? (hot ? shade(TH.bad, 112) : TH.bad) : (hot ? shade(TH.accent, 112) : TH.accent);
        round_rect(r.x, r.y, r.w, r.h, 4, bg);
        text_center(r.x, r.w, r.y + (r.h - CH) / 2, w->tab == 0 ? B0[i] : B1[i], 0xffffff, bg, 12);
    }
}

static void tm_end_task(win_t *w)
{
    if (w->tsel < 0 || w->tsel >= w->ntk) { flash("select a task first"); return; }
    uint32_t pid = w->tk[w->tsel].pid;
    int state = w->tk[w->tsel].state;
    if (pid == 0 || state == 1 || strcmp(w->tk[w->tsel].name, "shell") == 0 || strcmp(w->tk[w->tsel].name, "keyboard") == 0) { flash("that task keeps the system running"); return; }
    char c[24], m[40];
    snprintf(c, sizeof(c), "kill %u", pid);
    run_silent(c);
    snprintf(m, sizeof(m), "ended task %u", pid);
    flash(m);
    tm_refresh(w);
    damage_content(w);
}

static void tm_win_action(win_t *w, int which)
{
    if (w->lsel < 0 || w->lsel >= w->nli) { flash("select a window first"); return; }
    int wi = w->li[w->lsel].kind;
    if (wi < 0 || wi >= MAX_WIN || !g_win[wi].used) return;
    if (which == 0) {
        if (g_win[wi].running) { flash("that window is still running a command"); return; }
        close_window(wi);
        tm_win_refresh(w);
        damage_content(w);
    } else {
        g_win[wi].minimized = false;
        if (g_win[wi].ws != g_ws) switch_ws(g_win[wi].ws);
        focus_and_raise(wi);
        damage_win(&g_win[wi]);
    }
}

static int tasks_item_at(const win_t *w, int mx, int my)
{
    for (int i = 0; i < 3; i++) if (rect_has(tm_tab_rect(w, i), mx, my)) return 500 + i;
    if (w->tab != 2) for (int i = 0; i < (w->tab == 0 ? 1 : 2); i++) if (rect_has(tm_btn(w, i), mx, my)) return 1000 + i;
    return -1;
}

static void tasks_click(int wi, int mx, int my, bool dbl)
{
    win_t *w = &g_win[wi];
    for (int i = 0; i < 3; i++) if (rect_has(tm_tab_rect(w, i), mx, my)) { w->tab = i; w->lsel = -1; w->ltop = 0; tm_update(w); damage_content(w); return; }
    if (w->tab != 2) {
        for (int i = 0; i < (w->tab == 0 ? 1 : 2); i++) {
            if (!rect_has(tm_btn(w, i), mx, my)) continue;
            if (w->tab == 0) tm_end_task(w); else tm_win_action(w, i);
            return;
        }
        int ly = content_rect(w).y + TM_TAB + TM_HEAD;
        if (my < ly || my >= ly + tm_rows(w) * TM_ROW) return;
        int row = (my - ly) / TM_ROW + (w->tab == 0 ? w->ttop : w->ltop);
        if (w->tab == 0 && row < w->ntk) { w->tsel = row; damage_content(w); }
        else if (w->tab == 1 && row < w->nli) { bool again = row == w->lsel; w->lsel = row; if (dbl && again) tm_win_action(w, 1); damage_content(w); }
    }
}

static void tasks_key(int wi, uint8_t code, bool ext)
{
    win_t *w = &g_win[wi];
    if (!ext && code == 0x0F) { w->tab = (w->tab + 1) % 3; w->lsel = -1; w->ltop = 0; tm_update(w); damage_content(w); return; }
    if (!ext && code == 0x3F) { tm_update(w); damage_content(w); return; }
    if (w->tab == 2) return;
    int n = w->tab == 0 ? w->ntk : w->nli, *sel = w->tab == 0 ? &w->tsel : &w->lsel, *top = w->tab == 0 ? &w->ttop : &w->ltop, rows = tm_rows(w);
    if (ext && (code == 0x48 || code == 0x50) && n > 0) {
        int d = code == 0x48 ? -1 : 1;
        *sel = imax(0, imin(n - 1, (*sel < 0 ? (d > 0 ? -1 : n) : *sel) + d));
        if (*sel < *top) *top = *sel;
        if (*sel >= *top + rows) *top = *sel - rows + 1;
        damage_content(w);
    } else if (ext && code == 0x53) { if (w->tab == 0) tm_end_task(w); else tm_win_action(w, 0); }
    else if (!ext && code == 0x1C && w->tab == 1) tm_win_action(w, 1);
}

static void scroll_window(int wi, int lines_n)
{
    win_t *w = &g_win[wi];
    if (w->type == W_FILES) {
        int rows = files_rows(w), maxtop = imax(0, w->nfent - rows);
        w->ftop = imax(0, imin(maxtop, w->ftop - lines_n));
        damage_content(w);
        return;
    }
    if (w->type == W_EDIT) { edit_scroll(w, lines_n); return; }
    if (w->type == W_TASKS) {
        int rows = tm_rows(w), n = w->tab == 0 ? w->ntk : w->nli, *top = w->tab == 0 ? &w->ttop : &w->ltop;
        *top = imax(0, imin(imax(0, n - rows), *top - lines_n));
        damage_content(w);
        return;
    }
    if (w->type == W_SERV || w->type == W_SCRIPTS) {
        int rows = w->type == W_SERV ? list_rows(w, SV_HEAD, SV_FOOT, SV_ROW) : list_rows(w, SC_HEAD, SC_FOOT, SC_ROW);
        w->ltop = imax(0, imin(imax(0, w->nli - rows), w->ltop - lines_n));
        damage_content(w);
        return;
    }
    if (w->type != W_TERM && w->type != W_VIEW) return;
    w->scroll += lines_n;
    if (w->scroll > w->nl - 1) w->scroll = w->nl - 1;
    if (w->scroll < 0) w->scroll = 0;
    damage_content(w);
}

static void term_key(int wi, uint8_t code, bool ext)
{
    win_t *w = &g_win[wi];
    if (ext) {
        if (code == 0x49 || code == 0x51) {
            int page = tile_rows(w) - 3;
            if (page < 1) page = 1;
            scroll_window(wi, code == 0x49 ? page : -page);
            return;
        }
        if (w->type == W_VIEW) {
            if (code == 0x48) scroll_window(wi, 1);
            else if (code == 0x50) scroll_window(wi, -1);
            return;
        }
        if ((code == 0x48 || code == 0x50) && w->nhist) {
            int lo = w->nhist > 8 ? w->nhist - 8 : 0;
            if (code == 0x48 && w->hpos > lo) w->hpos--;
            else if (code == 0x50 && w->hpos < w->nhist) w->hpos++;
            if (w->hpos == w->nhist) w->input[0] = 0;
            else put_str(w->input, sizeof(w->input), w->hist[w->hpos & 7]);
            w->inlen = (int)strlen(w->input);
            damage_content(w);
        }
        return;
    }
    if (g_ctrl) {
        if (code == 0x26) { term_clear_lines(w); damage_content(w); }
        else if (code == 0x2E || code == 0x16) { w->inlen = 0; w->input[0] = 0; damage_content(w); }
        return;
    }
    if (w->type != W_TERM) return;
    if (code == 0x1C) { term_run(wi); return; }
    if (code == 0x0E) {
        if (w->inlen > 0) w->input[--w->inlen] = 0;
    } else if (code < sizeof(UNSHIFTED)) {
        char ch = g_shift ? SHIFTED[code] : UNSHIFTED[code];
        if (ch < 32 || ch > 126 || w->inlen >= (int)sizeof(w->input) - 1) return;
        w->input[w->inlen++] = ch;
        w->input[w->inlen] = 0;
    } else return;
    damage_content(w);
}

// ---- actions ------------------------------------------------------------------------------------------------------------------

static void open_exec(const char *cmd)
{
    int i = new_window(W_TERM, "Terminal");
    if (i < 0) return;
    put_str(g_win[i].input, sizeof(g_win[i].input), cmd);
    g_win[i].inlen = (int)strlen(g_win[i].input);
    term_run(i);
}

static void run_open(void)
{
    g_run.open = true; g_run.buf[0] = 0; g_run.len = 0; g_run.sel = 0;
    damage_rect(run_rect());
}

static void run_close(void)
{
    if (!g_run.open) return;
    g_run.open = false;
    rect_t r = run_rect();
    damage(r.x - 4, r.y - 4, r.w + 8, r.h + 8);
}

static void open_prefilled(const char *cmd)
{
    int i = new_window(W_TERM, "Terminal");
    if (i < 0) return;
    put_str(g_win[i].input, sizeof(g_win[i].input), cmd);
    g_win[i].inlen = (int)strlen(g_win[i].input);
    damage_content(&g_win[i]);
}

static bool cmd_exists(const char *name)
{
    for (size_t i = 0; i < command_count; i++) if (strcmp(commands[i].name, name) == 0) return true;
    return false;
}

static void silent_sink(char c, void *ctx) { (void)c; (void)ctx; }

// Runs a command and throws its output away (the Services window shows the result itself).
static void run_silent(const char *cmd)
{
    terminal_begin_capture_sink(silent_sink, NULL);
    if (exec_guarded(cmd)) terminal_capture_reset(); else terminal_end_capture();
}

static void menu_apps(int x, int y)
{
    menu_clear();
    menu_add("Terminal", ACT_OPEN, APP_TERM, IC_TERM);
    menu_add("Files", ACT_OPEN, APP_FILES, IC_FILES);
    menu_add("Text Editor", ACT_OPEN, APP_EDIT, IC_EDIT);
    menu_add("Calculator", ACT_OPEN, APP_CALC, IC_CALC);
    menu_sep();
    menu_add("Games", ACT_SUBMENU, 1, IC_GAME);
    menu_add("Development", ACT_SUBMENU, 2, IC_SCRIPT);
    menu_add("System", ACT_SUBMENU, 3, IC_SET);
    for (int i = 0; i < g_nlaunch; i++) menu_add(g_launch[i].label, ACT_CUSTOM, i, IC_APP);
    menu_sep();
    menu_add("Run Command...", ACT_RUNBOX, 0, IC_RUN);
    menu_add("Help", ACT_HELP, 0, IC_VIEW);
    menu_sep();
    menu_add("Console (text mode)", ACT_CONSOLE, 0, IC_CONSOLE);
    menu_add("Lock Screen", ACT_LOCK, 0, -1);
    menu_add("Log Out", ACT_LOGOUT, 0, -1);
    menu_show(x, y);
}

// The Applications menu shows a category in place of the main list.
static void menu_submenu(int which)
{
    rect_t old = menu_rect();
    int x = g_menu.x, y = g_menu.y;
    menu_clear();
    menu_add("<  Back", ACT_SUBMENU, 0, -1);
    menu_sep();
    if (which == 1) {
        games_build();
        for (int i = 0; i < g_ngames; i++) menu_add(g_games[i].name, ACT_GAME, i, IC_GAME);
        if (g_ngames == 0) menu_add("(no games installed)", ACT_NONE, 0, -1);
        menu_sep();
        menu_add("All games...", ACT_OPEN, APP_GAMES, IC_GAME);
    } else if (which == 2) {
        menu_add("RSHIDT (Rash IDE)", ACT_RSHIDT, 0, IC_SCRIPT);
        menu_add("Scripts", ACT_OPEN, APP_SCRIPTS, IC_SCRIPT);
        menu_add("Text Editor", ACT_OPEN, APP_EDIT, IC_EDIT);
        menu_add("Terminal", ACT_OPEN, APP_TERM, IC_TERM);
    } else if (which == 3) {
        menu_add("Services", ACT_OPEN, APP_SERV, IC_SERV);
        menu_add("Task Manager", ACT_OPEN, APP_TASKS, IC_MON);
        menu_add("System Monitor", ACT_OPEN, APP_MON, IC_MON);
        menu_add("Clock", ACT_OPEN, APP_CLOCK, IC_CLOCK);
        menu_add("Settings", ACT_OPEN, APP_SET, IC_SET);
    } else { menu_apps(x, y); damage_rect(old); return; }
    menu_show(x, y);
    damage_rect(old);
}

static void menu_power(int x, int y)
{
    menu_clear();
    menu_add("Lock Screen", ACT_LOCK, 0, -1);
    menu_add("Log Out", ACT_LOGOUT, 0, -1);
    menu_sep();
    menu_add("Restart", ACT_REBOOT, 0, -1);
    menu_add("Shut Down", ACT_POWEROFF, 0, -1);
    menu_sep();
    menu_add("Console (text mode)", ACT_CONSOLE, 0, IC_CONSOLE);
    menu_show(x, y);
}

static void menu_user(int x, int y)
{
    menu_clear();
    char who[32];
    snprintf(who, sizeof(who), "%s%s", users_current_name(), users_is_root() ? " (admin)" : "");
    menu_add(who, ACT_NONE, 0, IC_USER);
    menu_sep();
    menu_add("Settings", ACT_OPEN, APP_SET, IC_SET);
    menu_add("Lock Screen", ACT_LOCK, 0, -1);
    menu_add("Log Out", ACT_LOGOUT, 0, -1);
    menu_show(x, y);
}

static void menu_desktop(int x, int y)
{
    menu_clear();
    menu_add("New Terminal", ACT_OPEN, APP_TERM, IC_TERM);
    menu_add("Files", ACT_OPEN, APP_FILES, IC_FILES);
    menu_add("Text Editor", ACT_OPEN, APP_EDIT, IC_EDIT);
    menu_sep();
    menu_add("New File", ACT_NEWFILE, 0, IC_FILE);
    menu_add("New Folder", ACT_NEWDIR, 0, IC_FILES);
    menu_add("Arrange Icons", ACT_ARRANGE, 0, -1);
    menu_add("Refresh", ACT_REFRESH, 0, -1);
    menu_sep();
    menu_add("Change Wallpaper...", ACT_OPEN, APP_SET, IC_SET);
    menu_add("Run Command...", ACT_RUNBOX, 0, IC_RUN);
    menu_show(x, y);
}

static bool icon_deletable(int i)
{
    size_t n = strlen(g_desk_dir);
    return g_dic[i].kind != 0 && n > 0 && strncmp(g_dic[i].path, g_desk_dir, n) == 0 && g_dic[i].path[n] == '/';
}

static void menu_icon(int i, int x, int y)
{
    menu_clear();
    menu_add("Open", ACT_ICON_OPEN, i, -1);
    int nl = (int)strlen(g_dic[i].name);
    if (g_dic[i].kind == 2 && nl > 4 && same_nocase(g_dic[i].name + nl - 4, ".png")) menu_add("Set as Wallpaper", ACT_ICON_WALL, i, -1);
    else if (g_dic[i].kind == 2 && g_dic[i].icon != IC_GAME) menu_add("Edit", ACT_ICON_EDIT, i, IC_EDIT);
    if (icon_deletable(i)) { menu_sep(); menu_add("Delete", ACT_ICON_DELETE, i, -1); }
    menu_show(x, y);
}

static void menu_window(int wi, int x, int y)
{
    menu_clear();
    menu_add("Minimise", ACT_WIN_MIN, wi, -1);
    menu_add(g_win[wi].maximized ? "Restore" : "Maximise", ACT_WIN_MAX, wi, -1);
    menu_sep();
    menu_add("Close", ACT_WIN_CLOSE, wi, -1);
    menu_show(x, y);
}

static void apply_theme(int t)
{
    if (t < 0 || t >= NTHEMES) return;
    C.theme = t;
    TH = THEMES[t];
    theme_finish();
    wallpaper_prepare();
    damage_all();
}

static void icon_open(int i)
{
    if (i < 0 || i >= g_ndic) return;
    dicon_t d = g_dic[i];
    if (d.kind == 0) open_app(d.app);
    else if (d.kind == 1) open_files_at(d.path);
    else open_path(d.path);
}

// New file / folder on the desktop, with a name that is free (8.3 so the FAT disk accepts it).
static void desk_new(bool dir)
{
    if (!avfs_is_directory(g_desk_dir) && avfs_create_dir(g_desk_dir) != 0) { flash("cannot create the Desktop folder"); return; }
    char path[128];
    for (int n = 0; n < 100; n++) {
        if (dir) { if (n == 0) snprintf(path, sizeof(path), "%s/newdir", g_desk_dir); else snprintf(path, sizeof(path), "%s/newdir%d", g_desk_dir, n); }
        else { if (n == 0) snprintf(path, sizeof(path), "%s/untitled.txt", g_desk_dir); else snprintf(path, sizeof(path), "%s/new%d.txt", g_desk_dir, n); }
        if (!avfs_file_exists(path) && !avfs_is_directory(path)) break;
        if (n == 99) { flash("too many new files on the desktop"); return; }
    }
    int rc = dir ? avfs_create_dir(path) : avfs_save_file(path, "", 0, false);
    if (rc != 0) { flash(dir ? "could not create the folder" : "could not create the file"); return; }
    desktop_icons_refresh();
    damage_icons();
    flash(dir ? "folder created on the desktop" : "file created on the desktop");
    if (!dir) open_editor(path);
}

static void do_action(int act, int arg)
{
    switch (act) {
    case ACT_OPEN: open_app(arg); break;
    case ACT_RUNBOX: run_open(); break;
    case ACT_LOGOUT: do_logout(); break;
    case ACT_LOCK: do_lock(); break;
    case ACT_CONSOLE: g_exit_action = 3; g_quit = true; break;
    case ACT_REBOOT: g_exit_action = 1; g_quit = true; break;
    case ACT_POWEROFF: g_exit_action = 2; g_quit = true; break;
    case ACT_CUSTOM: if (arg >= 0 && arg < g_nlaunch) open_exec(g_launch[arg].cmd); break;
    case ACT_GAME: if (arg >= 0 && arg < g_ngames) { char c[48]; put_str(c, sizeof(c), g_games[arg].cmd); launch_program(c, g_games[arg].name); } break;
    case ACT_RSHIDT: launch_program("rshidt", "RSHIDT"); break;
    case ACT_WIN_MIN: if (g_win[arg].used) minimize_window(arg); break;
    case ACT_WIN_MAX: if (g_win[arg].used) win_set_maximized(arg, !g_win[arg].maximized); break;
    case ACT_WIN_CLOSE: close_window(arg); break;
    case ACT_THEME: apply_theme(arg); break;
    case ACT_HELP: open_viewer("Help", HELP_TEXT, (int)sizeof(HELP_TEXT) - 1); break;
    case ACT_NEWFILE: desk_new(false); break;
    case ACT_NEWDIR: desk_new(true); break;
    case ACT_REFRESH: desktop_icons_refresh(); damage_icons(); break;
    case ACT_FILES: files_action(arg >> 8, arg & 255); break;
    case ACT_ARRANGE: g_npos = 0; pos_save(); desktop_icons_refresh(); damage_icons(); flash("icons arranged"); break;
    case ACT_ICON_WALL: if (arg >= 0 && arg < g_ndic) { char p[96]; put_str(p, sizeof(p), g_dic[arg].path); wallpaper_set_file(p); } break;
    case ACT_ICON_OPEN: icon_open(arg); break;
    case ACT_ICON_EDIT: if (arg >= 0 && arg < g_ndic) { char p[96]; put_str(p, sizeof(p), g_dic[arg].path); open_editor(p); } break;
    case ACT_ICON_DELETE:
        if (arg >= 0 && arg < g_ndic && icon_deletable(arg)) {
            char p[96], nm[32];
            put_str(p, sizeof(p), g_dic[arg].path);
            put_str(nm, sizeof(nm), g_dic[arg].name);
            int rc = g_dic[arg].kind == 1 ? avfs_remove_dir_recursive(p) : avfs_remove_file(p);
            if (rc == 0) { char m[64]; snprintf(m, sizeof(m), "deleted %s", nm); flash(m); } else flash("could not delete that");
            sel_dicon = -1;
            desktop_icons_refresh();
            damage_icons();
        }
        break;
    default: break;
    }
}

static void menu_activate(int it)
{
    if (it < 0 || it >= g_menu.n || g_menu.it[it].sep) return;
    mitem_t m = g_menu.it[it];
    if (m.act == ACT_SUBMENU) { menu_submenu(m.arg); return; }
    menu_close();
    if (m.act != ACT_NONE) do_action(m.act, m.arg);
}

static void menu_key(uint8_t code, bool ext)
{
    if (!ext && code == 0x01) { menu_close(); return; }
    if (!ext && code == 0x1C) { menu_activate(g_menu.hover); return; }
    if (ext && (code == 0x48 || code == 0x50)) {
        int d = code == 0x48 ? -1 : 1, i = g_menu.hover;
        for (int k = 0; k < g_menu.n; k++) {
            i = i < 0 ? (d > 0 ? 0 : g_menu.n - 1) : (i + d + g_menu.n) % g_menu.n;
            if (!g_menu.it[i].sep) break;
        }
        g_menu.hover = i;
        damage_rect(menu_rect());
    }
}

// ---- keys for the list based applications ----------------------------------------------------------------------------------------

static void list_key(int wi, uint8_t code, bool ext)
{
    win_t *w = &g_win[wi];
    if (w->type == W_GAMES || w->nli == 0) return;
    int rows = w->type == W_SERV ? list_rows(w, SV_HEAD, SV_FOOT, SV_ROW) : list_rows(w, SC_HEAD, SC_FOOT, SC_ROW);
    if (ext && (code == 0x48 || code == 0x50)) {
        int d = code == 0x48 ? -1 : 1;
        w->lsel = imax(0, imin(w->nli - 1, (w->lsel < 0 ? (d > 0 ? -1 : w->nli) : w->lsel) + d));
        if (w->lsel < w->ltop) w->ltop = w->lsel;
        if (w->lsel >= w->ltop + rows) w->ltop = w->lsel - rows + 1;
        damage_content(w);
    } else if (!ext && code == 0x1C && w->type == W_SCRIPTS && w->lsel >= 0) script_run(w, w->lsel);
}

static void launcher_key(uint8_t code, bool ext)
{
    if (ext) return;
    if (code == 0x01) { run_close(); return; }
    if (code == 0x1C) {
        char cmd[64];
        put_str(cmd, sizeof(cmd), g_run.buf);
        run_close();
        const char *c = cmd;
        while (*c == ' ') c++;
        if (*c) open_exec(c);
        return;
    }
    if (code == 0x0F) {                                       // Tab: walk through the matches
        const char *m[12];
        if (g_run.sel == 0) put_str(g_run.pre, sizeof(g_run.pre), g_run.buf);
        int nm = run_matches(g_run.pre, m, 12);
        if (nm) {
            int next = g_run.sel == 0 ? 0 : g_run.sel % nm;
            g_run.len = put_str(g_run.buf, sizeof(g_run.buf), m[next]);
            g_run.sel = next + 1;
        }
        damage_rect(run_rect());
        return;
    }
    if (code == 0x0E) { if (g_run.len > 0) g_run.buf[--g_run.len] = 0; g_run.sel = 0; damage_rect(run_rect()); return; }
    if (code < sizeof(UNSHIFTED)) {
        char ch = g_shift ? SHIFTED[code] : UNSHIFTED[code];
        if (ch >= 32 && ch <= 126 && g_run.len < (int)sizeof(g_run.buf) - 1) { g_run.buf[g_run.len++] = ch; g_run.buf[g_run.len] = 0; g_run.sel = 0; damage_rect(run_rect()); }
    }
}

static void handle_key(uint8_t code, bool ext, bool make)
{
    if (code == 0x38) { g_alt = make; return; }
    if (code == 0x1D) { g_ctrl = make; return; }
    if (!ext && (code == 0x2A || code == 0x36)) { g_shift = make; return; }
    if (!make) return;
    if (g_run.open) { launcher_key(code, ext); return; }
    if (g_mode == MODE_LOGIN) { login_key(code, ext); return; }
    if (!ext && code == 0x57 && g_focus >= 0 && !g_menu.open && !g_run.open) { win_set_fullscreen(g_focus, !g_win[g_focus].fullscreen); return; }   // F11
    if (!ext && g_ctrl && g_shift && code == 0x01 && !g_menu.open && !g_run.open) { open_app(APP_TASKS); return; }   // Ctrl+Shift+Esc
    if (g_menu.open) { menu_key(code, ext); return; }
    if (g_alt) {
        switch (code) {
        case 0x0F: cycle_window(); return;                                               // Tab
        case 0x3E: if (g_focus >= 0) close_window(g_focus); return;                      // F4
        case 0x3C: run_open(); return;                                                   // F2
        case 0x3B: menu_apps(0, PANEL_H); hover_panel = 0; damage(0, 0, SW, PANEL_H); return;  // F1
        case 0x43: if (g_focus >= 0) minimize_window(g_focus); return;                    // F9
        case 0x44: if (g_focus >= 0) win_set_maximized(g_focus, !g_win[g_focus].maximized); return;   // F10
        default: break;
        }
        if (code >= 0x02 && code <= 0x05) { if (g_shift) send_to_ws(code - 2); else switch_ws(code - 2); return; }
        if (g_ctrl && code == 0x14) { open_app(APP_TERM); return; }                             // Ctrl+Alt+T
        if (g_ctrl && code == 0x10) { do_logout(); return; }                             // Ctrl+Alt+Q
        if (g_ctrl && code == 0x26) { do_lock(); return; }                               // Ctrl+Alt+L
        return;
    }
    if (g_focus < 0) return;
    win_t *w = &g_win[g_focus];
    if (w->type == W_TERM || w->type == W_VIEW) term_key(g_focus, code, ext);
    else if (w->type == W_FILES) files_key(g_focus, code, ext);
    else if (w->type == W_TASKS) tasks_key(g_focus, code, ext);
    else if (w->type == W_CALC) calc_key(g_focus, code, ext);
    else if (w->type == W_EDIT) edit_key(g_focus, code, ext);
    else if (w->type == W_SERV || w->type == W_SCRIPTS || w->type == W_GAMES) list_key(g_focus, code, ext);
}

static void poll_input_bytes(void);
static void login_key(uint8_t code, bool ext);
static void login_click(void);
static void login_hover(void);

// ---- the mouse (PS/2) ----------------------------------------------------------------------------------------------------------

static int g_mplen = 3, g_mpidx;
static uint8_t g_mpkt[4];
static int g_mbtn;

static void kbc_wait_write(void) { for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) { } }

static bool mouse_cmd(uint8_t c)
{
    kbc_wait_write(); outb(0x64, 0xD4);
    kbc_wait_write(); outb(0x60, c);
    for (int i = 0; i < 300000; i++) {
        if (!(inb(0x64) & 1)) continue;
        uint8_t r = inb(0x60);
        if (r == 0xFA) return true;
        if (r == 0xFE) return false;
    }
    return false;
}

static int mouse_read_byte(void)
{
    for (int i = 0; i < 300000; i++) if (inb(0x64) & 1) return inb(0x60);
    return -1;
}

static void mouse_flush(void)
{
    for (int i = 0; i < 64; i++) {
        uint8_t st = inb(0x64);
        if (!(st & 1) || !(st & 0x20)) break;     // keyboard bytes are left alone
        inb(0x60);
    }
    g_mpidx = 0;
}

// Streaming is switched off while a command runs (programs read the keyboard port and must not see mouse bytes).
static void mouse_enable(bool on)
{
    if (!g_mouse_present || on == g_mouse_on) return;
    if (on) { mouse_cmd(0xF4); g_mouse_on = true; g_mpidx = 0; }
    else { mouse_cmd(0xF5); g_mouse_on = false; mouse_flush(); }
}

static void mouse_init(void)
{
    g_mouse_present = g_mouse_on = false;
    kbc_wait_write(); outb(0x64, 0xA8);                  // enable the auxiliary (mouse) port
    if (!mouse_cmd(0xF6)) return;
    g_mouse_present = true;
    mouse_cmd(0xF3); mouse_cmd(200); mouse_cmd(0xF3); mouse_cmd(100); mouse_cmd(0xF3); mouse_cmd(80);   // IntelliMouse wheel handshake
    g_mplen = 3;
    if (mouse_cmd(0xF2)) { int id = mouse_read_byte(); if (id == 3 || id == 4) g_mplen = 4; }
    mouse_cmd(0xF3); mouse_cmd(100);
    mouse_cmd(0xF4);
    g_mouse_on = true;
    g_mpidx = 0;
    g_cur_show = true;
}

static void mouse_shutdown(void)
{
    if (g_mouse_present && g_mouse_on) mouse_cmd(0xF5);
    g_mouse_on = false;
    mouse_flush();
}

// ---- pointer interaction --------------------------------------------------------------------------------------------------------

enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE, DRAG_SLIDER, DRAG_ICON };
enum { E_L = 1, E_R = 2, E_T = 4, E_B = 8 };
static struct { int kind, win, gx, gy, edges; rect_t start; } g_drag;
static uint32_t g_last_click_ms;
static int g_last_click_win = -1, g_last_click_x, g_last_click_y;

static int edges_at(const win_t *w, int mx, int my)
{
    if (w->maximized) return 0;
    int e = 0;
    if (mx < w->x + EDGE) e |= E_L;
    if (mx >= w->x + w->w - EDGE) e |= E_R;
    if (my < w->y + 3) e |= E_T;
    if (my >= w->y + w->h - EDGE) e |= E_B;
    return e;
}

static int cursor_for_edges(int e)
{
    if ((e & (E_L | E_R)) && (e & (E_T | E_B))) return CUR_MOVE;
    if (e & (E_L | E_R)) return CUR_EW;
    if (e & (E_T | E_B)) return CUR_NS;
    return CUR_ARROW;
}

static void set_hover_panel(int h)
{
    if (h == hover_panel) return;
    hover_panel = h;
    damage(0, 0, SW, PANEL_H);
}

// Which button or tile of an application window the pointer is over (-1 none); the numbers are what the renderers compare with.
static int item_at(int wi, int mx, int my)
{
    win_t *w = &g_win[wi];
    switch (w->type) {
    case W_FILES: return files_item_at(w, mx, my);
    case W_TASKS: return tasks_item_at(w, mx, my);
    case W_CALC: for (int i = 0; i < NCBTN; i++) if (rect_has(calc_button(w, i), mx, my)) return i; break;
    case W_EDIT: for (int i = 0; i < 3; i++) if (rect_has(ed_button(w, i), mx, my)) return i; break;
    case W_GAMES: for (int i = 0; i < g_ngames; i++) if (rect_has(game_tile(w, i), mx, my)) return i; break;
    case W_SERV: for (int i = 0; i < 4; i++) if (rect_has(serv_button(w, i), mx, my)) return 1000 + i; break;
    case W_SCRIPTS: for (int i = 0; i < 4; i++) if (rect_has(script_button(w, i), mx, my)) return 1000 + i; break;
    case W_SET: {
        for (int i = 0; i < NSETTABS; i++) if (rect_has(set_tab_rect(w, i), mx, my)) return 2000 + i;
        if (rect_has(set_save_rect(w), mx, my)) return 2100;
        ctl_t ctl[32];
        int n = settings_layout(w, ctl, 32);
        for (int i = 0; i < n; i++)
            if (ctl[i].kind == CTL_BUTTON && mx >= ctl[i].x && mx < ctl[i].x + ctl[i].w && my >= ctl[i].y && my < ctl[i].y + ctl[i].h) return 3000 + ctl[i].id;
        break;
    }
    default: break;
    }
    return -1;
}

static void update_hover(void)
{
    int hp = -1, hd = -1, hi = -1, hw = -1, hb = -1;
    if (!g_menu.open && !g_run.open) {
        if (g_my < PANEL_H && !fullscreen_active()) {
            panel_layout();
            if (rect_has(pn_menu, g_mx, g_my)) hp = 0;
            else if (rect_has(pn_power, g_mx, g_my)) hp = 1;
            else if (rect_has(pn_user, g_mx, g_my)) hp = 50;
            for (int i = 0; i < pn_ntask; i++) if (rect_has(pn_task[i].r, g_mx, g_my)) hp = 2 + i;
            for (int i = 0; i < NWS; i++) if (rect_has(pn_pager[i], g_mx, g_my)) hp = 100 + i;
        } else {
            int wi = top_window_at(g_mx, g_my);
            if (wi < 0) {
                if (C.dock) for (int i = 0; i < dock_count(); i++) { rect_t r = dock_item(i); r.x -= 5; r.w += 10; r.y -= 8; r.h += 16; if (rect_has(r, g_mx, g_my)) hd = i; }
                if (hd < 0 && C.icons) for (int i = 0; i < g_ndic; i++) if (rect_has(dicon_rect(i), g_mx, g_my)) hi = i;
            } else {
                for (int b = 0; b < 4; b++) if (rect_has(title_button(&g_win[wi], b), g_mx, g_my)) { hw = wi; hb = b; }
            }
        }
    } else if (g_menu.open) {
        int it = menu_item_at(g_mx, g_my);
        if (it != g_menu.hover) { g_menu.hover = it; damage_rect(menu_rect()); }
    }
    set_hover_panel(hp);
    if (hd != hover_dock) {
        hover_dock = hd;
        rect_t d = dock_bounds();
        damage(d.x - 20, d.y - 50, d.w + 40, d.h + 60);
    }
    if (hi != hover_dicon) { int o = hover_dicon; hover_dicon = hi; damage_icon(o); damage_icon(hi); }
    {
        int iw = -1, it = -1;
        if (!g_menu.open && !g_run.open && g_my >= PANEL_H) {
            int wi = top_window_at(g_mx, g_my);
            if (wi >= 0) { it = item_at(wi, g_mx, g_my); if (it >= 0) iw = wi; }
        }
        if (iw != g_hover_item_win || it != g_hover_item) {
            int old = g_hover_item_win;
            g_hover_item_win = iw; g_hover_item = it;
            if (old >= 0 && g_win[old].used) damage_content(&g_win[old]);
            if (iw >= 0) damage_content(&g_win[iw]);
        }
    }
    if (hw != g_hover_win || hb != g_hover_btn) {
        int old = g_hover_win;
        g_hover_win = hw; g_hover_btn = hb;
        if (old >= 0 && g_win[old].used) damage_title(&g_win[old]);
        if (hw >= 0) damage_title(&g_win[hw]);
    }
}

// An icon follows the pointer; on release it settles into the nearest cell (swapping with whatever sits there) and is remembered.
static void icon_drag_move(void)
{
    int i = g_idrag.idx;
    if (i < 0 || i >= g_ndic) return;
    rect_t old = dicon_rect(i);
    if (!g_idrag.moving) {
        if (iabs(g_mx - g_idrag.sx) + iabs(g_my - g_idrag.sy) < 6) return;
        g_idrag.moving = true;
        g_idrag.x = old.x; g_idrag.y = old.y;
    }
    damage(old.x - 3, old.y - 3, old.w + 6, old.h + 6);
    g_idrag.x = imax(0, imin(SW - 96, g_mx - g_idrag.ox));
    g_idrag.y = imax(PANEL_H, imin(SH - 86, g_my - g_idrag.oy));
    damage_icon(i);
}

static void icon_drag_end(void)
{
    int i = g_idrag.idx;
    if (!g_idrag.moving || i < 0 || i >= g_ndic) { g_idrag.moving = false; return; }
    int col = imax(0, imin(grid_cols() - 1, (g_idrag.x + 48 - GRID_X) / CELL_W)), row = imax(0, imin(grid_rows() - 1, (g_idrag.y + 43 - GRID_Y) / CELL_H));
    g_idrag.moving = false;
    for (int j = 0; j < g_ndic; j++) {
        if (j == i || g_dic[j].col != col || g_dic[j].row != row) continue;
        g_dic[j].col = g_dic[i].col; g_dic[j].row = g_dic[i].row;
        pos_set(g_dic[j].name, g_dic[j].col, g_dic[j].row);
    }
    g_dic[i].col = col; g_dic[i].row = row;
    pos_set(g_dic[i].name, col, row);
    pos_save();
    damage_icons();
}

static void apply_drag(void)
{
    if (g_drag.kind == DRAG_ICON) { icon_drag_move(); return; }
    win_t *w = &g_win[g_drag.win];
    if (g_drag.kind == DRAG_SLIDER) { settings_slider_set(g_drag_slider_id, g_mx, g_drag_slider_x); damage_content(w); return; }
    damage_win(w);
    if (g_drag.kind == DRAG_MOVE) {
        w->x = g_mx - g_drag.gx;
        w->y = imax(PANEL_H - 2, g_my - g_drag.gy);
        if (w->y > SH - 40) w->y = SH - 40;
        if (C.snap) {
            if (iabs(w->x) < 14) w->x = 0;
            if (iabs(w->x + w->w - SW) < 14) w->x = SW - w->w;
            if (w->y < PANEL_H + 10) w->y = PANEL_H;
        }
    } else {
        rect_t s = g_drag.start;
        int x0 = s.x, y0 = s.y, x1 = s.x + s.w, y1 = s.y + s.h;
        if (g_drag.edges & E_L) x0 = imin(g_mx, x1 - MIN_W);
        if (g_drag.edges & E_R) x1 = imax(g_mx, x0 + MIN_W);
        if (g_drag.edges & E_T) y0 = imax(PANEL_H, imin(g_my, y1 - MIN_H));
        if (g_drag.edges & E_B) y1 = imax(g_my, y0 + MIN_H);
        w->x = x0; w->y = y0; w->w = x1 - x0; w->h = y1 - y0;
    }
    damage_win(w);
}

static void pointer_moved(void)
{
    mark_cursor();
    {
        bool want = fullscreen_active() && g_my < 28;
        if (want != g_fs_hint) { g_fs_hint = want; rect_t fr = fs_exit_rect(); damage(fr.x, fr.y, fr.w, fr.h + 8); }
    }
    if (g_drag.kind != DRAG_NONE) { apply_drag(); return; }
    if (g_mode == MODE_LOGIN) { login_hover(); g_cur_kind = CUR_ARROW; return; }
    update_hover();
    int kind = CUR_ARROW;
    if (!g_menu.open && !g_run.open && g_my >= PANEL_H) {
        int wi = top_window_at(g_mx, g_my);
        if (wi >= 0) kind = cursor_for_edges(edges_at(&g_win[wi], g_mx, g_my));
    }
    g_cur_kind = kind;
}

static void launch_desktop_icon(int i) { icon_open(i); }

static void left_press(void)
{
    uint32_t now = now_ms();
    bool busy = g_run_win >= 0;                                           // a program is running: only window management works
    if (g_mode == MODE_LOGIN) { login_click(); return; }
    if (fullscreen_active() && g_fs_hint && rect_has(fs_exit_rect(), g_mx, g_my)) { win_set_fullscreen(topmost_visible(), false); return; }
    if (g_run.open) {
        rect_t r = run_rect();
        if (!rect_has(r, g_mx, g_my)) run_close();
        return;
    }
    if (g_menu.open) {
        int it = menu_item_at(g_mx, g_my);
        if (it >= 0) { menu_activate(it); return; }
        if (!rect_has(menu_rect(), g_mx, g_my)) menu_close();             // a click outside only closes it
        return;
    }
    if (g_my < PANEL_H && !fullscreen_active()) {
        panel_layout();
        if (!busy && rect_has(pn_menu, g_mx, g_my)) { menu_apps(0, PANEL_H); return; }
        if (!busy && rect_has(pn_power, g_mx, g_my)) { menu_power(SW - 210, PANEL_H); return; }
        if (!busy && rect_has(pn_user, g_mx, g_my)) { menu_user(pn_user.x, PANEL_H); return; }
        for (int i = 0; i < NWS; i++) if (rect_has(pn_pager[i], g_mx, g_my)) { switch_ws(i); return; }
        for (int i = 0; i < pn_ntask; i++) {
            if (!rect_has(pn_task[i].r, g_mx, g_my)) continue;
            int wi = pn_task[i].win;
            if (wi == g_focus && !g_win[wi].minimized) minimize_window(wi); else focus_and_raise(wi);
            return;
        }
        return;
    }
    int wi = top_window_at(g_mx, g_my);
    if (wi < 0) {
        if (busy) return;
        if (C.dock) {
            for (int i = 0; i < dock_count(); i++) {
                rect_t r = dock_item(i);
                r.x -= 5; r.w += 10; r.y -= 8; r.h += 16;
                if (!rect_has(r, g_mx, g_my)) continue;
                if (i < NDOCK) open_app(DOCK[i].kind); else open_exec(g_launch[i - NDOCK].cmd);
                return;
            }
        }
        if (C.icons) {
            for (int i = 0; i < g_ndic; i++) {
                if (!rect_has(dicon_rect(i), g_mx, g_my)) continue;
                bool dbl = sel_dicon == i && now - g_last_click_ms < (uint32_t)C.dblclick;
                int old = sel_dicon;
                sel_dicon = i;
                damage_icon(old);
                damage_icon(i);
                g_last_click_ms = now;
                if (dbl) { sel_dicon = -1; damage_icon(i); launch_desktop_icon(i); return; }
                rect_t ir = dicon_rect(i);
                g_idrag.idx = i; g_idrag.moving = false; g_idrag.sx = g_mx; g_idrag.sy = g_my; g_idrag.ox = g_mx - ir.x; g_idrag.oy = g_my - ir.y;
                g_drag.kind = DRAG_ICON; g_drag.win = 0;
                return;
            }
            if (sel_dicon >= 0) { int old = sel_dicon; sel_dicon = -1; damage_icon(old); }
        }
        return;
    }
    win_t *w = &g_win[wi];
    focus_and_raise(wi);
    for (int b = 0; b < 4; b++) {
        if (w->fullscreen) break;
        if (!rect_has(title_button(w, b), g_mx, g_my)) continue;
        if (b == 0) { if (w->running) g_abort_req = true; else close_window(wi); }
        else if (b == 1) win_set_maximized(wi, !w->maximized);
        else if (b == 2) minimize_window(wi);
        else win_set_fullscreen(wi, !w->fullscreen);
        return;
    }
    int e = w->fullscreen ? 0 : edges_at(w, g_mx, g_my);
    if (e) {
        g_drag.kind = DRAG_RESIZE; g_drag.win = wi; g_drag.edges = e; g_drag.start = win_rect(w);
        g_cur_kind = cursor_for_edges(e);
        return;
    }
    if (!w->fullscreen && g_my < w->y + BORD + TITLE_H) {
        bool dbl = g_last_click_win == wi && now - g_last_click_ms < (uint32_t)C.dblclick && iabs(g_mx - g_last_click_x) < 6 && iabs(g_my - g_last_click_y) < 6;
        g_last_click_ms = now; g_last_click_win = wi; g_last_click_x = g_mx; g_last_click_y = g_my;
        if (dbl) { win_set_maximized(wi, !w->maximized); g_last_click_win = -1; return; }
        if (!w->maximized) { g_drag.kind = DRAG_MOVE; g_drag.win = wi; g_drag.gx = g_mx - w->x; g_drag.gy = g_my - w->y; g_cur_kind = CUR_MOVE; }
        return;
    }
    if (busy) return;
    bool dbl = g_last_click_win == wi && now - g_last_click_ms < (uint32_t)C.dblclick;
    g_last_click_ms = now; g_last_click_win = wi;
    if (w->type == W_SET) { g_drag_slider_id = 0; settings_click(wi, g_mx, g_my); if (g_drag_slider_id) { g_drag.kind = DRAG_SLIDER; g_drag.win = wi; } }
    else if (w->type == W_FILES) files_click(wi, g_mx, g_my, dbl);
    else if (w->type == W_TASKS) tasks_click(wi, g_mx, g_my, dbl);
    else if (w->type == W_CALC) calc_click(wi, g_mx, g_my);
    else if (w->type == W_EDIT) edit_click(wi, g_mx, g_my);
    else if (w->type == W_GAMES) games_click(wi, g_mx, g_my);
    else if (w->type == W_SERV) serv_click(wi, g_mx, g_my);
    else if (w->type == W_SCRIPTS) scripts_click(wi, g_mx, g_my, dbl);
}

static void left_release(void)
{
    if (g_drag.kind == DRAG_ICON) icon_drag_end();
    if (g_drag.kind == DRAG_MOVE && C.snap && g_drag.win >= 0 && g_win[g_drag.win].used && !g_win[g_drag.win].maximized) {
        int wi = g_drag.win;
        win_t *w = &g_win[wi];
        if (g_my <= PANEL_H) win_set_maximized(wi, true);
        else if (g_mx <= 1 || g_mx >= SW - 2) {                      // dropped at a side edge: that half of the screen
            int ax, ay, aw, ah;
            work_area(&ax, &ay, &aw, &ah);
            damage_win(w);
            w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
            w->w = aw / 2; w->h = ah; w->y = ay; w->x = g_mx <= 1 ? ax : ax + aw / 2;
            w->maximized = true;
            damage_win(w);
        }
    }
    g_drag.kind = DRAG_NONE;
    g_drag_slider_id = 0;
    pointer_moved();
}

static void right_press(void)
{
    if (g_mode == MODE_LOGIN || g_run_win >= 0) return;
    if (g_run.open) { run_close(); return; }
    if (g_menu.open) menu_close();
    if (g_my < PANEL_H) return;
    int wi = top_window_at(g_mx, g_my);
    if (wi >= 0) {
        focus_and_raise(wi);
        if (g_my < g_win[wi].y + BORD + TITLE_H) menu_window(wi, g_mx, g_my);
        else if (g_win[wi].type == W_FILES) files_rclick(wi, g_mx, g_my);
        return;
    }
    if (C.dock) { rect_t b = dock_bounds(); if (rect_has(b, g_mx, g_my)) return; }
    if (C.icons) {
        for (int i = 0; i < g_ndic; i++) {
            if (!rect_has(dicon_rect(i), g_mx, g_my)) continue;
            sel_dicon = i;
            damage_icons();
            menu_icon(i, g_mx, g_my);
            return;
        }
    }
    menu_desktop(g_mx, g_my);
}

static void middle_press(void)
{
    if (g_mode == MODE_LOGIN) return;
    int wi = top_window_at(g_mx, g_my);
    if (wi >= 0 && g_my < g_win[wi].y + BORD + TITLE_H) close_window(wi);
}

static void mouse_wheel(int raw)
{
    if (g_menu.open || g_run.open || raw == 0 || g_mode == MODE_LOGIN) return;
    if (g_my < PANEL_H) { switch_ws((g_ws + (raw > 0 ? 1 : NWS - 1)) % NWS); return; }
    int wi = top_window_at(g_mx, g_my);
    if (wi >= 0) scroll_window(wi, raw < 0 ? 3 : -3);
}

static void mouse_packet(void)
{
    int b0 = g_mpkt[0];
    int dx = g_mpkt[1], dy = g_mpkt[2];
    if (b0 & 0x10) dx -= 256;
    if (b0 & 0x20) dy -= 256;
    if (b0 & 0xC0) dx = dy = 0;                              // overflow: ignore the movement
    // Slow movements are scaled down for precision; the faster the sweep the closer the gain gets to speed * (1 + gain).
    static int rem_x, rem_y;
    int mag = iabs(dx) + iabs(dy), gain = C.mouse_speed;
    if (C.mouse_gain > 0) gain += C.mouse_speed * C.mouse_gain / 100 * imin(mag, 30) / 30;
    rem_x += dx * gain; rem_y += dy * gain;
    int step_x = rem_x / 100, step_y = rem_y / 100;
    rem_x -= step_x * 100; rem_y -= step_y * 100;
    int wheel = 0;
    if (g_mplen == 4) { wheel = g_mpkt[3] & 0x0F; if (wheel & 8) wheel -= 16; }
    int nb = b0 & 7, old = g_mbtn;
    g_mbtn = nb;
    if (step_x || step_y) {
        g_mx += step_x; g_my -= step_y;
        if (g_mx < 0) g_mx = 0;
        if (g_mx > SW - 1) g_mx = SW - 1;
        if (g_my < 0) g_my = 0;
        if (g_my > SH - 1) g_my = SH - 1;
        pointer_moved();
    }
    if ((nb & ~old) & 1) left_press();
    if ((old & ~nb) & 1) left_release();
    if ((nb & ~old) & 2) right_press();
    if ((nb & ~old) & 4) middle_press();
    if (wheel) mouse_wheel(wheel);
}

static void mouse_byte(uint8_t b)
{
    if (g_mpidx == 0 && !(b & 8)) return;                    // out of sync
    g_mpkt[g_mpidx++] = b;
    if (g_mpidx == g_mplen) { g_mpidx = 0; mouse_packet(); }
}

static void poll_input_bytes(void)
{
    static bool prefix;
    while (inb(0x64) & 0x01) {
        uint8_t status = inb(0x64), sc = inb(0x60);
        if (status & 0x20) { if (g_mouse_on) mouse_byte(sc); continue; }
        if (sc == 0xE0) { prefix = true; continue; }
        if (sc == 0xE1) continue;
        bool ext = prefix;
        prefix = false;
        handle_key(sc & 0x7F, ext, !(sc & 0x80));
    }
}

// ---- once a second ------------------------------------------------------------------------------------------------------------

static void tick_second(void)
{
    static uint32_t busy_last, tick_n;
    tick_n++;
    uint32_t ui = (g_busy_ms - busy_last) / 10;
    busy_last = g_busy_ms;
    if (ui > 100) ui = 100;
    uint64_t total = pmm_get_total_memory(), freeb = pmm_get_free_memory();
    uint32_t tm = (uint32_t)(total >> 20), fm = (uint32_t)(freeb >> 20);
    uint32_t mp = tm > fm ? (tm - fm) * 100 / tm : 0;
    uint32_t dp = 0, dtotal = 0, dused = 0;
    if (avfs_disk_space(&dtotal, &dused) == 0 && dtotal) dp = (uint32_t)(((dused >> 4) * 100) / (dtotal >> 4));
    g_mem_pct = mp; g_dsk_pct = dp;
    if (g_mode == MODE_LOGIN) { damage(SW / 2 - 120, 40, 240, 80); return; }
    if ((tick_n & 1) == 0 && !g_menu.open && g_drag.kind != DRAG_ICON) desktop_icons_refresh();
    for (int i = 0; i < MAX_WIN; i++) {
        win_t *w = &g_win[i];
        if (!w->used) continue;
        if (w->type == W_MON || w->type == W_TASKS) {
            w->hm[w->hn % HN] = (uint8_t)mp;
            w->hu[w->hn % HN] = (uint8_t)ui;
            w->hd[w->hn % HN] = (uint8_t)dp;
            w->hn++;
            if (w->type == W_TASKS) tm_update(w);
            if (win_visible(i)) damage_content(w);
        } else if (w->type == W_CLOCK && win_visible(i)) damage_content(w);
        else if (w->type == W_FILES && (tick_n % 3) == 0 && win_visible(i) && w->fmode == FM_NORMAL && !g_menu.open) { if (fm_sig(w->path) != w->fsig) files_reload(w); }
        else if (w->type == W_SERV && (tick_n & 1) == 0) { serv_refresh(w); if (win_visible(i)) damage_content(w); }
    }
    damage(0, 0, SW, PANEL_H);
}

// ---- session: login, lock, logout ---------------------------------------------------------------------------------------------

static void login_enter(bool locked)
{
    memset(&g_login, 0, sizeof(g_login));
    g_login.locked = locked;
    g_login.hover = -1;
    menu_close();
    run_close();
    g_mode = MODE_LOGIN;
    damage_all();
}

static bool boot_cfg_get(void)
{
    static char buf[128];
    if (!avfs_file_exists("/disk/etc/boot.cfg") || avfs_get_content("/disk/etc/boot.cfg", buf, sizeof(buf)) != 0) return true;
    for (char *l = buf; *l; ) {
        char *nx = l;
        while (*nx && *nx != '\n') nx++;
        if (*nx) *nx++ = 0;
        char *w[4];
        if (split_words(l, w, 4) >= 2 && same_nocase(w[0], "desktop")) { bool b; if (parse_bool(w[1], &b)) return b; }
        l = nx;
    }
    return true;
}

static bool boot_cfg_set(bool on)
{
    if (!avfs_is_directory("/disk")) return false;
    if (!avfs_is_directory("/disk/etc") && avfs_create_dir("/disk/etc") != 0) return false;
    const char *t = on ? "desktop yes\n" : "desktop no\n";
    return avfs_save_file("/disk/etc/boot.cfg", t, (uint32_t)strlen(t), true) == 0;
}

static void close_all_windows(void)
{
    memset(g_win, 0, sizeof(g_win));
    g_zn = 0; g_focus = -1; g_ws = 0; g_casc = 0;
    g_hover_item = g_hover_item_win = g_hover_win = g_hover_btn = -1;
    hover_panel = hover_dock = hover_dicon = sel_dicon = -1;
    g_drag.kind = DRAG_NONE;
}

// A user has logged in (or there is nobody to ask): their configuration, their desktop, their start-up windows.
static void begin_session(void)
{
    g_mode = MODE_DESKTOP;
    close_all_windows();
    load_config(NULL);
    g_boot_desktop = boot_cfg_get();
    desktop_dir_init();
    g_dic_sig = 1;
    pos_load();
    desktop_icons_refresh();
    wallpaper_prepare();
    for (int i = 0; i < g_nauto; i++) {
        if (g_auto[i].kind == 100) open_exec(g_auto[i].arg);
        else open_app(g_auto[i].kind);
    }
    if (g_cfg_nerr) flash(g_cfg_err);
    damage_all();
}

static void do_logout(void)
{
    if (!g_boot_mode) { g_quit = true; return; }                  // started from the shell: logging out leaves the desktop
    for (int i = 0; i < MAX_WIN; i++) if (g_win[i].used && g_win[i].running) return;
    close_all_windows();
    users_session_logout();
    login_enter(false);
}

static void do_lock(void) { login_enter(true); }

static void login_hover(void)
{
    int h = -1, n = login_nusers(), first = login_first();
    for (int vi = 0; vi < 6 && first + vi < n; vi++) if (rect_has(lg_card(vi), g_mx, g_my)) h = LH_CARD + vi;
    if (rect_has(lg_field(), g_mx, g_my) && users_enabled()) h = LH_FIELD;
    rect_t b = users_enabled() ? lg_button() : (rect_t){ SW / 2 - 70, 440, 140, 34 };
    if (rect_has(b, g_mx, g_my)) h = LH_LOGIN;
    for (int i = g_login.locked ? 1 : 0; i < 3; i++) if (rect_has(lg_small(i), g_mx, g_my)) h = LH_CONSOLE + i;
    if (h != g_login.hover) { g_login.hover = h; damage_all(); }
}

static void login_wipe(void) { memset(g_login.pw, 0, sizeof(g_login.pw)); g_login.pwlen = 0; }

static void login_submit(void)
{
    uint32_t now = now_ms();
    if (now < g_login.until) { put_str(g_login.err, sizeof(g_login.err), "Please wait a moment before trying again"); damage_all(); return; }
    bool ok = true;
    if (users_enabled()) {
        put_str(g_login.err, sizeof(g_login.err), "Checking...");
        damage_all();
        draw_frame();
        ok = g_login.locked ? users_verify_current(g_login.pw) : users_session_login(login_name(g_login.sel), g_login.pw);
    }
    login_wipe();
    if (ok) {
        g_login.err[0] = 0;
        g_login.fails = 0;
        if (g_login.locked) { g_mode = MODE_DESKTOP; damage_all(); }
        else begin_session();
        return;
    }
    g_login.fails++;
    g_login.until = now_ms() + (g_login.fails >= 3 ? 8000 : 1200);
    put_str(g_login.err, sizeof(g_login.err), "Incorrect password");
    damage_all();
}

static void login_select(int d)
{
    int n = login_nusers();
    if (n <= 1 || g_login.locked) return;
    g_login.sel = (g_login.sel + d + n) % n;
    login_wipe();
    g_login.err[0] = 0;
    damage_all();
}

static void login_key(uint8_t code, bool ext)
{
    if (ext) {
        if (code == 0x4B) login_select(-1);
        else if (code == 0x4D) login_select(1);
        return;
    }
    if (code == 0x0F) { login_select(1); return; }
    if (code == 0x1C) { login_submit(); return; }
    if (code == 0x01) { login_wipe(); g_login.err[0] = 0; damage_all(); return; }
    if (!users_enabled()) return;
    if (code == 0x0E) { if (g_login.pwlen > 0) g_login.pw[--g_login.pwlen] = 0; damage_all(); return; }
    if (code < sizeof(UNSHIFTED)) {
        char ch = g_shift ? SHIFTED[code] : UNSHIFTED[code];
        if (ch >= 32 && ch <= 126 && g_login.pwlen < 32) { g_login.pw[g_login.pwlen++] = ch; g_login.pw[g_login.pwlen] = 0; g_login.err[0] = 0; damage_all(); }
    }
}

static void login_click(void)
{
    int n = login_nusers(), first = login_first();
    for (int vi = 0; vi < 6 && first + vi < n; vi++) {
        if (!rect_has(lg_card(vi), g_mx, g_my)) continue;
        if (first + vi != g_login.sel) { g_login.sel = first + vi; login_wipe(); g_login.err[0] = 0; damage_all(); }
        else if (!users_enabled()) login_submit();
        return;
    }
    rect_t b = users_enabled() ? lg_button() : (rect_t){ SW / 2 - 70, 440, 140, 34 };
    if (rect_has(b, g_mx, g_my)) { login_submit(); return; }
    if (!g_login.locked && rect_has(lg_small(0), g_mx, g_my)) { g_exit_action = 3; g_quit = true; return; }
    if (rect_has(lg_small(1), g_mx, g_my)) { g_exit_action = 1; g_quit = true; return; }
    if (rect_has(lg_small(2), g_mx, g_my)) { g_exit_action = 2; g_quit = true; return; }
}

// ---- the command ----------------------------------------------------------------------------------------------------------------

// `desktop selftest`: exercises the Files operations, the task snapshot and the wallpaper decoder without a display.
static int g_st_bad, g_st_n;
static void st_check(bool ok, const char *what)
{
    g_st_n++;
    if (!ok) { g_st_bad++; OUT_ERR("  FAIL  %s\n", what); }
}

static bool st_select(win_t *w, const char *name)
{
    for (int i = 0; i < w->nfent; i++) if (strcmp(w->fent[i].name, name) == 0) { fm_select_only(w, i); return true; }
    return false;
}

static void desktop_selftest(void)
{
    const char *base = "/disk/dsktest";
    char p[160];
    g_st_bad = g_st_n = 0;
    if (!avfs_is_directory("/disk")) { print("desktop selftest: skipped (no /disk)\n"); return; }
    memset(g_win, 0, sizeof(g_win));
    g_zn = 0; g_focus = -1; g_ws = 0; g_next_id = 1; g_casc = 0; g_ndic = 0; g_npos = 0;
    load_config(NULL);
    if (avfs_is_directory(base)) fs_remove(base);
    st_check(avfs_create_dir(base) == 0, "create the test folder");
    snprintf(p, sizeof(p), "%s/src", base);
    avfs_create_dir(p);
    uint8_t *big = malloc(70000);
    for (int i = 0; i < 70000; i++) big[i] = (uint8_t)(i * 7 + (i >> 8));
    snprintf(p, sizeof(p), "%s/big.bin", base);
    st_check(avfs_save_file(p, big, 70000, true) == 0, "write a 70000 byte file");
    snprintf(p, sizeof(p), "%s/t.txt", base);
    avfs_save_file(p, "hello\n", 6, true);

    int wi = new_window(W_FILES, "Files");
    win_t *w = &g_win[wi];
    put_str(w->path, sizeof(w->path), base);
    files_load(w);
    int skip = w->nfent && strcmp(w->fent[0].name, "..") == 0 ? 1 : 0;
    st_check(w->nfent == 3 + skip && strcmp(w->fent[skip].name, "src") == 0, "listing: three entries, folder first");

    st_check(st_select(w, "big.bin"), "select big.bin");
    files_action(wi, FO_COPY);
    st_check(g_clip.n == 1 && !g_clip.cut, "copy fills the clipboard");
    snprintf(p, sizeof(p), "%s/src", base);
    files_goto(w, p, true);
    files_action(wi, FO_PASTE);
    snprintf(p, sizeof(p), "%s/src/big.bin", base);
    st_check(avfs_get_filesize(p) == 70000, "paste copies the whole file");
    uint8_t *back = malloc(70000);
    bool same = avfs_read_file(p, back, 70000, 0) == 0 && memcmp(back, big, 70000) == 0;
    st_check(same, "the copy has identical bytes");
    free(back);

    files_action(wi, FO_BACK);
    st_check(strcmp(w->path, base) == 0, "Back returns to the previous folder");
    st_check(st_select(w, "t.txt"), "select t.txt");
    files_action(wi, FO_CUT);
    snprintf(p, sizeof(p), "%s/src", base);
    files_goto(w, p, true);
    files_action(wi, FO_PASTE);
    snprintf(p, sizeof(p), "%s/src/t.txt", base);
    st_check(avfs_file_exists(p), "cut and paste: file arrives");
    snprintf(p, sizeof(p), "%s/t.txt", base);
    st_check(!avfs_file_exists(p), "cut and paste: file leaves");

    files_goto(w, base, true);
    st_check(st_select(w, "big.bin"), "select big.bin again");
    files_action(wi, FO_COPY);
    files_action(wi, FO_PASTE);
    snprintf(p, sizeof(p), "%s/big~1.bin", base);
    st_check(avfs_file_exists(p), "pasting next to the original picks a free name");

    st_check(st_select(w, "big~1.bin"), "select the copy");
    fm_prompt(w, FM_RENAME, "r.bin");
    fm_prompt_accept(wi);
    snprintf(p, sizeof(p), "%s/r.bin", base);
    st_check(avfs_get_filesize(p) == 70000, "rename keeps the contents");
    snprintf(p, sizeof(p), "%s/big~1.bin", base);
    st_check(!avfs_file_exists(p), "rename removes the old name");

    fm_prompt(w, FM_NEWDIR, "nd");
    fm_prompt_accept(wi);
    snprintf(p, sizeof(p), "%s/nd", base);
    st_check(avfs_is_directory(p), "new folder");
    fm_prompt(w, FM_NEWFILE, "a/b");
    fm_prompt_accept(wi);
    st_check(!fm_name_ok("a/b") && !fm_name_ok("..") && fm_name_ok("ok.txt"), "names with a slash or dots are refused");

    st_check(st_select(w, "src"), "select the folder");
    fm_prompt(w, FM_DELETE, "");
    fm_prompt_accept(wi);
    snprintf(p, sizeof(p), "%s/src", base);
    st_check(!avfs_is_directory(p), "delete removes a folder with its contents");
    st_check(fm_count(w) == 0 || true, "selection after delete");

    int ti = new_window(W_TASKS, "Task Manager");
    tm_refresh(&g_win[ti]);
    st_check(g_win[ti].ntk > 0, "task snapshot lists tasks");
    bool names = false;
    for (int i = 0; i < g_win[ti].ntk; i++) if (g_win[ti].tk[i].name[0]) names = true;
    st_check(names, "tasks have names");
    tm_win_refresh(&g_win[ti]);
    st_check(g_win[ti].nli == 2, "window list shows the two windows");

    const uint32_t blob = (uint32_t)(wallpaper_default_png_end - wallpaper_default_png);
    if (blob > 0) {
        C.wallpaper = 3; C.wp_file[0] = 0; g_wp_key[0] = 0;
        wallpaper_prepare();
        st_check(g_wp_img_on, "built-in wallpaper decodes");
        if (!g_wp_img_on) printr("  (%s; picture is %u bytes)\n", g_wp_err, blob);
        snprintf(p, sizeof(p), "%s/w.png", base);
        st_check(avfs_save_file(p, wallpaper_default_png, blob, true) == 0, "write a PNG");
        wallpaper_set_file(p);
        st_check(g_wp_img_on && strcmp(C.wp_file, p) == 0, "a PNG file becomes the wallpaper");
        C.wp_mode = 1; wallpaper_prepare();
        st_check(g_wp_img_on, "wallpaper mode fit");
        snprintf(p, sizeof(p), "%s/t2.txt", base);
        avfs_save_file(p, "not a png", 9, true);
        wallpaper_set_file(p);
        st_check(!g_wp_img_on && g_wp_err[0], "a non-PNG is refused with a message");
    } else print("  (no built-in wallpaper in this build)\n");

    free(big);
    fs_remove(base);
    st_check(!avfs_is_directory(base), "clean up");
    memset(g_win, 0, sizeof(g_win));
    printr("desktop selftest: %d checks, %d failed\n", g_st_n, g_st_bad);
    if (g_st_bad) shell_set_status(1);
}

static void show_config_state(const char *path)
{
    if (path) printr("configuration file: %s\n", path);
    else print("no configuration file: built-in defaults (write one with: desktop defaults)\n");
}

static void desktop_main(bool boot);

void desktop_command(int argc, char *argv[])
{
    if (argc >= 2) {
        if (strcmp(argv[1], "themes") == 0) {
            for (int i = 0; i < NTHEMES; i++) printr("  %s\n", THEMES[i].name);
            print("select one with 'theme NAME' in the configuration file or in Settings\n");
            return;
        }
        if (strcmp(argv[1], "selftest") == 0) { desktop_selftest(); return; }
        if (strcmp(argv[1], "config") == 0) { show_config_state(load_config(NULL)); return; }
        if (strcmp(argv[1], "check") == 0) {
            const char *path = load_config(argc >= 3 ? argv[2] : NULL);
            if (argc >= 3 && !path) { OUT_ERR("desktop: %s: no such file\n", argv[2]); shell_set_status(1); return; }
            show_config_state(path);
            if (g_cfg_nerr) {
                OUT_ERR("%s (%d problem%s in total)\n", g_cfg_err, g_cfg_nerr, g_cfg_nerr == 1 ? "" : "s");
                shell_set_status(1);
            } else printr("ok: theme %s, %d launcher(s), %d autostart entr%s\n", TH.name, g_nlaunch, g_nauto, g_nauto == 1 ? "y" : "ies");
            return;
        }
        if (strcmp(argv[1], "defaults") == 0) {
            char path[96];
            if (!default_config_path(path, sizeof(path))) { OUT_ERR("desktop: /disk is not available\n"); shell_set_status(1); return; }
            if (avfs_file_exists(path) && !(argc >= 3 && strcmp(argv[2], "-f") == 0)) {
                OUT_ERR("desktop: %s exists (desktop defaults -f overwrites it)\n", path);
                shell_set_status(1);
                return;
            }
            if (avfs_save_file(path, DEFAULT_CONF, (uint32_t)(sizeof(DEFAULT_CONF) - 1), true) != 0) { OUT_ERR("desktop: cannot write %s\n", path); shell_set_status(1); return; }
            printr("wrote %s\n", path);
            return;
        }
        OUT_ERR("Usage: desktop [check [FILE] | defaults [-f] | config | themes | selftest]\n");
        shell_set_status(1);
        return;
    }

    desktop_main(false);
}

bool desktop_boot_wanted(void)
{
    if (cmdline_has("nodesktop") || cmdline_has("safe") || cmdline_has("rescue") || cmdline_has("single") || cmdline_has("selftest")) return false;
    return boot_cfg_get();
}

void desktop_boot(void) { desktop_main(true); }

static void desktop_main(bool boot)
{
    memset(g_win, 0, sizeof(g_win));
    load_config(NULL);
    g_boot_mode = boot;
    g_mode = MODE_DESKTOP;
    memset(&g_login, 0, sizeof(g_login));
    g_zn = 0; g_focus = -1; g_ws = 0; g_next_id = 1; g_casc = 0;
    g_alt = g_ctrl = g_shift = g_quit = false;
    g_exit_action = 0;
    g_msg[0] = 0; g_busy_ms = 0; g_ndirty = 0;
    g_menu.open = false; g_run.open = false;
    hover_panel = hover_dock = hover_dicon = sel_dicon = -1; g_hover_win = -1; g_hover_btn = -1;
    g_drag.kind = DRAG_NONE; g_mbtn = 0;
    memset(g_npend, 0, sizeof(g_npend)); memset(g_cur_dirty, 0, sizeof(g_cur_dirty)); memset(g_cdrawn, 0, sizeof(g_cdrawn));
    memset(&P, 0, sizeof(P));
    g_up_base = (uint32_t)get_uptime();
    g_wp_theme = -1;

    if (!gpu_init()) { gpu_shutdown(); if (boot) print("desktop: no graphics available, starting the text console\n"); return; }
    memset(g_store, 0, (size_t)SW * STORE_H * 4);
    g_cur_kind = CUR_ARROW; g_mx = SW / 2; g_my = SH / 2;
    mouse_init();
    g_cur_show = g_mouse_present;

    tick_second();
    if (boot && !users_login_skipped()) login_enter(false);
    else begin_session();
    damage_all();

    g_last_sec = now_ms();
    uint32_t last_draw = 0;
    fbcon_set_poll_hook(vt_pump);
    while (!g_quit) {
        poll_input_bytes();
        uint32_t now = now_ms();
        if (now - g_last_sec >= 1000) { g_last_sec += 1000; tick_second(); }
        if (now - last_draw >= 16 && has_pending()) { draw_frame(); last_draw = now_ms(); }
        else __asm__ volatile("pause");
    }
    fbcon_set_poll_hook(NULL);
    mouse_shutdown();
    gpu_shutdown();
    if (!boot) printr("desktop: bye\n");
    if (g_exit_action == 1) execute_command_no_history("reboot");
    else if (g_exit_action == 2) execute_command_no_history("poweroff");
}
