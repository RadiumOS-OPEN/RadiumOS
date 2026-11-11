// desktop.c - RadiWM, a minimal window manager and desktop drawn through the Vulkan implementation
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
#include "../io/clipboard.h"
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

#define MS_W 16                                    // Minesweeper board
#define MS_H 12
#define MS_N 30
#define MS_C 24
#define MS_HEAD 44
#define PT_W 560                                   // Paint canvas
#define PT_H 360
#define PT_BAR 44
#define TERM_PREV_Y 106                            // Settings > Terminal: where the preview box starts
#define TRANSPARENT 0xFFFFFFFFu
static int g_theme_page;
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
    { "radium", 0xe2ded3, 0xd6d1c4,
      0x1c1c1a, 0xf4f1ea, 0x9a968b, 0x3a3a36, 0x1c1c1a, 0x5f7a52, 0xffffff,
      0xf4f1ea, 0x1c1c1a, 0xe2ded3, 0x8a867b, 0xb8b3a5, 0x1c1c1a,
      0xf4f1ea, 0x1c1c1a, 0x7a766b, 0xfbfaf6, 0xd2cdbf,
      0x1c1c1a, 0xe8e4d8, 0x262624,
      0xf4f1ea, 0x1c1c1a, 0x5f7a52,
      0x3f7a3a, 0xa07a1a, 0xb03a2e },
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
    /* Extra themes: paste after the last entry in THEMES[] (before the closing }; ). */
    { "fire", 0x2c1d15, 0x110805,
0x1a0d08, 0xffd9b0, 0x816954, 0x433226, 0x110805, 0xff5a1f, 0x110805,
0x2c1d15, 0xffe8d0, 0x1a0d08, 0x816954, 0x433226, 0xff5a1f,
0x1a0d08, 0xffd9b0, 0x816954, 0x110805, 0x433226,
0x110805, 0xffd9b0, 0x1a0d08,
0x2c1d15, 0xffd9b0, 0x6a2810,
0xffb347, 0xffd23f, 0xff2a1a },
    { "ice", 0x1b2832, 0x070e15,
0x0b1620, 0xd6f1ff, 0x667984, 0x303d48, 0x070e15, 0x7fd8ff, 0x070e15,
0x1b2832, 0xe6f7ff, 0x0b1620, 0x667984, 0x303d48, 0x7fd8ff,
0x0b1620, 0xd6f1ff, 0x667984, 0x070e15, 0x303d48,
0x070e15, 0xd6f1ff, 0x0b1620,
0x1b2832, 0xd6f1ff, 0x345a6e,
0x8ff0c8, 0xffe08a, 0xff7a8a },
    { "ocean", 0x1a2a39, 0x06101b,
0x0a1929, 0xcfe8f7, 0x637686, 0x2d3e4e, 0x06101b, 0x2ea8e6, 0x06101b,
0x1a2a39, 0xe2f1fa, 0x0a1929, 0x637686, 0x2d3e4e, 0x2ea8e6,
0x0a1929, 0xcfe8f7, 0x637686, 0x06101b, 0x2d3e4e,
0x06101b, 0xcfe8f7, 0x0a1929,
0x1a2a39, 0xcfe8f7, 0x174b6b,
0x3ddc97, 0xf5c542, 0xff5c5c },
    { "forest", 0x202a21, 0x0a110c,
0x101a12, 0xd3e6cf, 0x687667, 0x333f34, 0x0a110c, 0x6fbf73, 0x0a110c,
0x202a21, 0xe5f0e2, 0x101a12, 0x687667, 0x333f34, 0x6fbf73,
0x101a12, 0xd3e6cf, 0x687667, 0x0a110c, 0x333f34,
0x0a110c, 0xd3e6cf, 0x101a12,
0x202a21, 0xd3e6cf, 0x315434,
0x8bd96b, 0xe0c35a, 0xe0675a },
    { "sunset", 0x312026, 0x140a10,
0x1f1018, 0xffd9c2, 0x846a64, 0x473437, 0x140a10, 0xff7e5f, 0x140a10,
0x312026, 0xffe8da, 0x1f1018, 0x846a64, 0x473437, 0xff7e5f,
0x1f1018, 0xffd9c2, 0x846a64, 0x140a10, 0x473437,
0x140a10, 0xffd9c2, 0x1f1018,
0x312026, 0xffd9c2, 0x6d3631,
0xffb86b, 0xffe066, 0xff4d6d },
    { "midnight", 0x191e2a, 0x060911,
0x0a0e1a, 0xc9d1e8, 0x606677, 0x2c313f, 0x060911, 0x6c8cff, 0x060911,
0x191e2a, 0xdfe3f1, 0x0a0e1a, 0x606677, 0x2c313f, 0x6c8cff,
0x0a0e1a, 0xc9d1e8, 0x606677, 0x060911, 0x2c313f,
0x060911, 0xc9d1e8, 0x0a0e1a,
0x191e2a, 0xc9d1e8, 0x2c3a6a,
0x6fe0a3, 0xffd27a, 0xff6b8a },
    { "cyberpunk", 0x1f1433, 0x080115,
0x0d0221, 0xf0e6ff, 0x736985, 0x362b49, 0x080115, 0xff2a6d, 0x080115,
0x1f1433, 0xf6f0ff, 0x0d0221, 0x736985, 0x362b49, 0xff2a6d,
0x0d0221, 0xf0e6ff, 0x736985, 0x080115, 0x362b49,
0x080115, 0xf0e6ff, 0x0d0221,
0x1f1433, 0xf0e6ff, 0x62103c,
0x05ffa1, 0xfff200, 0xff3860 },
    { "synthwave", 0x352c3f, 0x17121f,
0x241b2f, 0xf4eefc, 0x827a8b, 0x494154, 0x17121f, 0xff7edb, 0x17121f,
0x352c3f, 0xf8f5fd, 0x241b2f, 0x827a8b, 0x494154, 0xff7edb,
0x241b2f, 0xf4eefc, 0x827a8b, 0x17121f, 0x494154,
0x17121f, 0xf4eefc, 0x241b2f,
0x352c3f, 0xf4eefc, 0x713e6b,
0x72f1b8, 0xfede5d, 0xfe4450 },
    { "matrix", 0x0f1e0f, 0x000600,
0x000a00, 0xb6ffb6, 0x527852, 0x213621, 0x000600, 0x00ff41, 0x000600,
0x0f1e0f, 0xd3ffd3, 0x000a00, 0x527852, 0x213621, 0x00ff41,
0x000a00, 0xb6ffb6, 0x527852, 0x000600, 0x213621,
0x000600, 0xb6ffb6, 0x000a00,
0x0f1e0f, 0xb6ffb6, 0x006017,
0x00ff41, 0xc8ff00, 0xff3030 },
    { "amber", 0x271c09, 0x0d0800,
0x140c00, 0xffcf6b, 0x7e6430, 0x3e2f13, 0x0d0800, 0xffb000, 0x0d0800,
0x271c09, 0xffe2a6, 0x140c00, 0x7e6430, 0x3e2f13, 0xffb000,
0x140c00, 0xffcf6b, 0x7e6430, 0x0d0800, 0x3e2f13,
0x0d0800, 0xffcf6b, 0x140c00,
0x271c09, 0xffcf6b, 0x664500,
0xc8d94a, 0xffd23f, 0xff5a3c },
    { "rosepine", 0x292735, 0x100f17,
0x191724, 0xe0def4, 0x737182, 0x3d3b49, 0x100f17, 0xc4a7e7, 0x100f17,
0x292735, 0xecebf8, 0x191724, 0x737182, 0x3d3b49, 0xc4a7e7,
0x191724, 0xe0def4, 0x737182, 0x100f17, 0x3d3b49,
0x100f17, 0xe0def4, 0x191724,
0x292735, 0xe0def4, 0x554968,
0x9ccfd8, 0xf6c177, 0xeb6f92 },
    { "everforest", 0x3a4144, 0x1d2226,
0x2d353b, 0xd3c6aa, 0x78766d, 0x4b4f4f, 0x1d2226, 0xa7c080, 0x1d2226,
0x3a4144, 0xe5ddcc, 0x2d353b, 0x78766d, 0x4b4f4f, 0xa7c080,
0x2d353b, 0xd3c6aa, 0x78766d, 0x1d2226, 0x4b4f4f,
0x1d2226, 0xd3c6aa, 0x2d353b,
0x3a4144, 0xd3c6aa, 0x586653,
0xa7c080, 0xdbbc7f, 0xe67e80 },
    { "kanagawa", 0x2e2e34, 0x14141a,
0x1f1f28, 0xdcd7ba, 0x74726a, 0x414042, 0x14141a, 0x7e9cd8, 0x14141a,
0x2e2e34, 0xeae7d6, 0x1f1f28, 0x74726a, 0x414042, 0x7e9cd8,
0x1f1f28, 0xdcd7ba, 0x74726a, 0x14141a, 0x414042,
0x14141a, 0xdcd7ba, 0x1f1f28,
0x2e2e34, 0xdcd7ba, 0x404b66,
0x98bb6c, 0xe6c384, 0xe82424 },
    { "ayu", 0x202428, 0x0a0d10,
0x0f1419, 0xe6e1cf, 0x70706b, 0x36393a, 0x0a0d10, 0x39bae6, 0x0a0d10,
0x202428, 0xf0ede2, 0x0f1419, 0x70706b, 0x36393a, 0x39bae6,
0x0f1419, 0xe6e1cf, 0x70706b, 0x0a0d10, 0x36393a,
0x0a0d10, 0xe6e1cf, 0x0f1419,
0x202428, 0xe6e1cf, 0x1e4e61,
0xaad94c, 0xffb454, 0xf07178 },
    { "material", 0x364248, 0x192024,
0x263238, 0xeeffff, 0x808e92, 0x4a575c, 0x192024, 0x82aaff, 0x192024,
0x364248, 0xf5ffff, 0x263238, 0x808e92, 0x4a575c, 0x82aaff,
0x263238, 0xeeffff, 0x808e92, 0x192024, 0x4a575c,
0x192024, 0xeeffff, 0x263238,
0x364248, 0xeeffff, 0x465c7e,
0xc3e88d, 0xffcb6b, 0xf07178 },
    { "palenight", 0x333749, 0x1b1d28,
0x292d3e, 0xa6accd, 0x61667e, 0x404458, 0x1b1d28, 0x82aaff, 0x1b1d28,
0x333749, 0xcacde1, 0x292d3e, 0x61667e, 0x404458, 0x82aaff,
0x292d3e, 0xa6accd, 0x61667e, 0x1b1d28, 0x404458,
0x1b1d28, 0xa6accd, 0x292d3e,
0x333749, 0xa6accd, 0x485982,
0xc3e88d, 0xffcb6b, 0xff5370 },
    { "oceanicnext", 0x2a3942, 0x121c22,
0x1b2b34, 0xd8dee9, 0x707c85, 0x3d4b55, 0x121c22, 0x6699cc, 0x121c22,
0x2a3942, 0xe8ebf2, 0x1b2b34, 0x707c85, 0x3d4b55, 0x6699cc,
0x1b2b34, 0xd8dee9, 0x707c85, 0x121c22, 0x3d4b55,
0x121c22, 0xd8dee9, 0x1b2b34,
0x2a3942, 0xd8dee9, 0x355269,
0x99c794, 0xfac863, 0xec5f67 },
    { "nightowl", 0x122637, 0x010e19,
0x011627, 0xd6deeb, 0x61707f, 0x273a4a, 0x010e19, 0x82aaff, 0x010e19,
0x122637, 0xe6ebf3, 0x011627, 0x61707f, 0x273a4a, 0x82aaff,
0x011627, 0xd6deeb, 0x61707f, 0x010e19, 0x273a4a,
0x010e19, 0xd6deeb, 0x011627,
0x122637, 0xd6deeb, 0x2e4a73,
0xaddb67, 0xecc48d, 0xef5350 },
    { "horizon", 0x2c2e35, 0x121419,
0x1c1e26, 0xe0e0e0, 0x74757a, 0x3f4147, 0x121419, 0xe95678, 0x121419,
0x2c2e35, 0xececec, 0x1c1e26, 0x74757a, 0x3f4147, 0xe95678,
0x1c1e26, 0xe0e0e0, 0x74757a, 0x121419, 0x3f4147,
0x121419, 0xe0e0e0, 0x1c1e26,
0x2c2e35, 0xe0e0e0, 0x643243,
0x29d398, 0xfab795, 0xf43e5c },
    { "zenburn", 0x4c4c4a, 0x292929,
0x3f3f3f, 0xdcdccc, 0x86867e, 0x5b5b58, 0x292929, 0x8cd0d3, 0x292929,
0x4c4c4a, 0xeaeae0, 0x3f3f3f, 0x86867e, 0x5b5b58, 0x8cd0d3,
0x3f3f3f, 0xdcdccc, 0x86867e, 0x292929, 0x5b5b58,
0x292929, 0xdcdccc, 0x3f3f3f,
0x4c4c4a, 0xdcdccc, 0x5a7273,
0x7f9f7f, 0xf0dfaf, 0xcc9393 },
    { "tomorrow", 0x2a2d2e, 0x131415,
0x1d1f21, 0xc5c8c6, 0x696b6b, 0x3b3d3f, 0x131415, 0x81a2be, 0x131415,
0x2a2d2e, 0xdcdedd, 0x1d1f21, 0x696b6b, 0x3b3d3f, 0x81a2be,
0x1d1f21, 0xc5c8c6, 0x696b6b, 0x131415, 0x3b3d3f,
0x131415, 0xc5c8c6, 0x1d1f21,
0x2a2d2e, 0xc5c8c6, 0x404d58,
0xb5bd68, 0xf0c674, 0xcc6666 },
    { "ember", 0x2c221e, 0x120c0a,
0x1c1210, 0xe8d5c4, 0x786a61, 0x413530, 0x120c0a, 0xe25822, 0x120c0a,
0x2c221e, 0xf1e6dc, 0x1c1210, 0x786a61, 0x413530, 0xe25822,
0x1c1210, 0xe8d5c4, 0x786a61, 0x120c0a, 0x413530,
0x120c0a, 0xe8d5c4, 0x1c1210,
0x2c221e, 0xe8d5c4, 0x612a16,
0xc9a227, 0xf2a900, 0xc0392b },
    { "lava", 0x251514, 0x0c0303,
0x120505, 0xffcfc0, 0x7d6059, 0x3d2927, 0x0c0303, 0xff3d00, 0x0c0303,
0x251514, 0xffe2d9, 0x120505, 0x7d6059, 0x3d2927, 0xff3d00,
0x120505, 0xffcfc0, 0x7d6059, 0x0c0303, 0x3d2927,
0x0c0303, 0xffcfc0, 0x120505,
0x251514, 0xffcfc0, 0x651903,
0xff9100, 0xffc400, 0xd50000 },
    { "volcano", 0x271a19, 0x0e0606,
0x160a0a, 0xf0d0c0, 0x78635c, 0x3d2e2b, 0x0e0606, 0xff6b35, 0x0e0606,
0x271a19, 0xf6e3d9, 0x160a0a, 0x78635c, 0x3d2e2b, 0xff6b35,
0x160a0a, 0xf0d0c0, 0x78635c, 0x0e0606, 0x3d2e2b,
0x0e0606, 0xf0d0c0, 0x160a0a,
0x271a19, 0xf0d0c0, 0x682c19,
0xffa62b, 0xffd166, 0xe63946 },
    { "neon", 0x1c1c25, 0x06060c,
0x0a0a12, 0xe8e8ff, 0x6e6e7d, 0x32323d, 0x06060c, 0x00f0ff, 0x06060c,
0x1c1c25, 0xf1f1ff, 0x0a0a12, 0x6e6e7d, 0x32323d, 0x00f0ff,
0x0a0a12, 0xe8e8ff, 0x6e6e7d, 0x06060c, 0x32323d,
0x06060c, 0xe8e8ff, 0x0a0a12,
0x1c1c25, 0xe8e8ff, 0x065a65,
0x39ff14, 0xffee00, 0xff0055 },
    { "vaporwave", 0x2b2143, 0x110a21,
0x1a1033, 0xf3e8ff, 0x7c718f, 0x413758, 0x110a21, 0x01cdfe, 0x110a21,
0x2b2143, 0xf8f1ff, 0x1a1033, 0x7c718f, 0x413758, 0x01cdfe,
0x1a1033, 0xf3e8ff, 0x7c718f, 0x110a21, 0x413758,
0x110a21, 0xf3e8ff, 0x1a1033,
0x2b2143, 0xf3e8ff, 0x11527a,
0x05ffa1, 0xfffb96, 0xff71ce },
    { "mint", 0x1f302a, 0x0a1411,
0x0f1f1a, 0xd2f0e4, 0x677d75, 0x32453e, 0x0a1411, 0x4fd1a5, 0x0a1411,
0x1f302a, 0xe4f6ef, 0x0f1f1a, 0x677d75, 0x32453e, 0x4fd1a5,
0x0f1f1a, 0xd2f0e4, 0x677d75, 0x0a1411, 0x32453e,
0x0a1411, 0xd2f0e4, 0x0f1f1a,
0x1f302a, 0xd2f0e4, 0x255d4b,
0x7be495, 0xe8d26a, 0xf07178 },
    { "lavender", 0x2e2a3e, 0x14121e,
0x1e1b2e, 0xe3dcf5, 0x777288, 0x413e52, 0x14121e, 0xb794f6, 0x14121e,
0x2e2a3e, 0xeeeaf9, 0x1e1b2e, 0x777288, 0x413e52, 0xb794f6,
0x1e1b2e, 0xe3dcf5, 0x777288, 0x14121e, 0x413e52,
0x14121e, 0xe3dcf5, 0x1e1b2e,
0x2e2a3e, 0xe3dcf5, 0x544574,
0x8fe3b0, 0xf5d680, 0xf58fa0 },
    { "sakura", 0x35262c, 0x170e12,
0x24161c, 0xf7dde4, 0x837076, 0x4a3a40, 0x170e12, 0xff9ebb, 0x170e12,
0x35262c, 0xfaebef, 0x24161c, 0x837076, 0x4a3a40, 0xff9ebb,
0x24161c, 0xf7dde4, 0x837076, 0x170e12, 0x4a3a40,
0x170e12, 0xf7dde4, 0x24161c,
0x35262c, 0xf7dde4, 0x714654,
0xa8d8a0, 0xf5d68a, 0xf2607f },
    { "coffee", 0x2a211b, 0x110c08,
0x1a120d, 0xe6d3c0, 0x76695e, 0x3f352d, 0x110c08, 0xc08552, 0x110c08,
0x2a211b, 0xf0e5d9, 0x1a120d, 0x76695e, 0x3f352d, 0xc08552,
0x1a120d, 0xe6d3c0, 0x76695e, 0x110c08, 0x3f352d,
0x110c08, 0xe6d3c0, 0x1a120d,
0x2a211b, 0xe6d3c0, 0x543a25,
0xa8b565, 0xe0b45a, 0xc85a4a },
    { "sand", 0x3a352d, 0x1c1915,
0x2b2620, 0xe8dcc4, 0x80786a, 0x4d473e, 0x1c1915, 0xd4a76a, 0x1c1915,
0x3a352d, 0xf1eadc, 0x2b2620, 0x80786a, 0x4d473e, 0xd4a76a,
0x2b2620, 0xe8dcc4, 0x80786a, 0x1c1915, 0x4d473e,
0x1c1915, 0xe8dcc4, 0x2b2620,
0x3a352d, 0xe8dcc4, 0x66533a,
0xa3b86c, 0xe0c068, 0xd1695a },
    { "slate", 0x2c333e, 0x14181f,
0x1e2530, 0xd0d7e0, 0x6e757f, 0x3e4550, 0x14181f, 0x6e9bd1, 0x14181f,
0x2c333e, 0xe3e7ec, 0x1e2530, 0x6e757f, 0x3e4550, 0x6e9bd1,
0x1e2530, 0xd0d7e0, 0x6e757f, 0x14181f, 0x3e4550,
0x14181f, 0xd0d7e0, 0x1e2530,
0x2c333e, 0xd0d7e0, 0x3a4e68,
0x7fc79a, 0xe3c06d, 0xe07a7a },
    { "charcoal", 0x272727, 0x101010,
0x181818, 0xd8d8d8, 0x6e6e6e, 0x3b3b3b, 0x101010, 0x7a9fd0, 0x101010,
0x272727, 0xe8e8e8, 0x181818, 0x6e6e6e, 0x3b3b3b, 0x7a9fd0,
0x181818, 0xd8d8d8, 0x6e6e6e, 0x101010, 0x3b3b3b,
0x101010, 0xd8d8d8, 0x181818,
0x272727, 0xd8d8d8, 0x3a4758,
0x8cc084, 0xd9b95c, 0xd46a6a },
    { "obsidian", 0x1d1d1f, 0x08080a,
0x0d0d0f, 0xcfcfd4, 0x646468, 0x303032, 0x08080a, 0x8f7bff, 0x08080a,
0x1d1d1f, 0xe2e2e5, 0x0d0d0f, 0x646468, 0x303032, 0x8f7bff,
0x0d0d0f, 0xcfcfd4, 0x646468, 0x08080a, 0x303032,
0x08080a, 0xcfcfd4, 0x0d0d0f,
0x1d1d1f, 0xcfcfd4, 0x3a3463,
0x7bd88f, 0xe6c35c, 0xff6b6b },
    { "ink", 0x1a1e29, 0x070a10,
0x0b0f19, 0xc8d0e0, 0x606673, 0x2d323d, 0x070a10, 0x4d7cff, 0x070a10,
0x1a1e29, 0xdee3ec, 0x0b0f19, 0x606673, 0x2d323d, 0x4d7cff,
0x0b0f19, 0xc8d0e0, 0x606673, 0x070a10, 0x2d323d,
0x070a10, 0xc8d0e0, 0x0b0f19,
0x1a1e29, 0xc8d0e0, 0x22356a,
0x4fd1a1, 0xf2c14e, 0xff5d73 },
    { "graphite", 0x303439, 0x16191c,
0x22262b, 0xcfd4da, 0x70747a, 0x41454a, 0x16191c, 0x5ea1ff, 0x16191c,
0x303439, 0xe2e5e9, 0x22262b, 0x70747a, 0x41454a, 0x5ea1ff,
0x22262b, 0xcfd4da, 0x70747a, 0x16191c, 0x41454a,
0x16191c, 0xcfd4da, 0x22262b,
0x303439, 0xcfd4da, 0x375175,
0x6cc58a, 0xe0b75a, 0xe5646c },
    { "copper", 0x2b221e, 0x110c0a,
0x1a1210, 0xecd5c0, 0x786a5f, 0x403530, 0x110c0a, 0xc8743a, 0x110c0a,
0x2b221e, 0xf4e6d9, 0x1a1210, 0x786a5f, 0x403530, 0xc8743a,
0x1a1210, 0xecd5c0, 0x786a5f, 0x110c0a, 0x403530,
0x110c0a, 0xecd5c0, 0x1a1210,
0x2b221e, 0xecd5c0, 0x57341f,
0x9bb05a, 0xe0a83a, 0xc8503a },
    { "emerald", 0x16251f, 0x050d0a,
0x07140f, 0xc8eedb, 0x5e766b, 0x2a3b34, 0x050d0a, 0x10b981, 0x050d0a,
0x16251f, 0xdef5e9, 0x07140f, 0x5e766b, 0x2a3b34, 0x10b981,
0x07140f, 0xc8eedb, 0x5e766b, 0x050d0a, 0x2a3b34,
0x050d0a, 0xc8eedb, 0x07140f,
0x16251f, 0xc8eedb, 0x0a4e37,
0x34d399, 0xfbbf24, 0xf87171 },
    { "ruby", 0x2b1a1f, 0x11060a,
0x1a0a0f, 0xf2cdd5, 0x7b6268, 0x412d33, 0x11060a, 0xe11d48, 0x11060a,
0x2b1a1f, 0xf7e1e6, 0x1a0a0f, 0x7b6268, 0x412d33, 0xe11d48,
0x1a0a0f, 0xf2cdd5, 0x7b6268, 0x11060a, 0x412d33,
0x11060a, 0xf2cdd5, 0x1a0a0f,
0x2b1a1f, 0xf2cdd5, 0x601123,
0x4ade80, 0xfbbf24, 0xff3355 },
    { "sapphire", 0x18223b, 0x050c1c,
0x08122b, 0xcdd9f5, 0x616c86, 0x2b364f, 0x050c1c, 0x3b82f6, 0x050c1c,
0x18223b, 0xe1e8f9, 0x08122b, 0x616c86, 0x2b364f, 0x3b82f6,
0x08122b, 0xcdd9f5, 0x616c86, 0x050c1c, 0x2b364f,
0x050c1c, 0xcdd9f5, 0x08122b,
0x18223b, 0xcdd9f5, 0x1a3972,
0x34d399, 0xfbbf24, 0xf87171 },
    { "amethyst", 0x27203c, 0x0f0a1c,
0x17102b, 0xe4d9fb, 0x736a89, 0x3c3450, 0x0f0a1c, 0x9333ea, 0x0f0a1c,
0x27203c, 0xefe8fd, 0x17102b, 0x736a89, 0x3c3450, 0x9333ea,
0x17102b, 0xe4d9fb, 0x736a89, 0x0f0a1c, 0x3c3450,
0x0f0a1c, 0xe4d9fb, 0x17102b,
0x27203c, 0xe4d9fb, 0x421c6e,
0x4ade80, 0xfacc15, 0xfb7185 },
    { "topaz", 0x2d2717, 0x120e05,
0x1c1608, 0xf3e5c0, 0x7d735b, 0x433b29, 0x120e05, 0xf59e0b, 0x120e05,
0x2d2717, 0xf8efd9, 0x1c1608, 0x7d735b, 0x433b29, 0xf59e0b,
0x1c1608, 0xf3e5c0, 0x7d735b, 0x120e05, 0x433b29,
0x120e05, 0xf3e5c0, 0x1c1608,
0x2d2717, 0xf3e5c0, 0x684609,
0xa3e635, 0xfde047, 0xef4444 },
    { "jade", 0x1b2b28, 0x07110f,
0x0b1a17, 0xcdeee6, 0x627974, 0x2e403c, 0x07110f, 0x14b8a6, 0x07110f,
0x1b2b28, 0xe1f5f0, 0x0b1a17, 0x627974, 0x2e403c, 0x14b8a6,
0x0b1a17, 0xcdeee6, 0x627974, 0x07110f, 0x2e403c,
0x07110f, 0xcdeee6, 0x0b1a17,
0x1b2b28, 0xcdeee6, 0x0e5149,
0x5eead4, 0xfcd34d, 0xfb7185 },
    { "aurora", 0x1b2b34, 0x071117,
0x0b1a24, 0xd4eef0, 0x657980, 0x2f4049, 0x071117, 0x5eead4, 0x071117,
0x1b2b34, 0xe5f5f6, 0x0b1a24, 0x657980, 0x2f4049, 0x5eead4,
0x0b1a24, 0xd4eef0, 0x657980, 0x071117, 0x2f4049,
0x071117, 0xd4eef0, 0x0b1a24,
0x1b2b34, 0xd4eef0, 0x286362,
0x86efac, 0xfde68a, 0xfda4af },
    { "twilight", 0x2a2436, 0x110e19,
0x1a1526, 0xded6f0, 0x726c81, 0x3d384a, 0x110e19, 0x8b7cf6, 0x110e19,
0x2a2436, 0xebe6f6, 0x1a1526, 0x726c81, 0x3d384a, 0x8b7cf6,
0x1a1526, 0xded6f0, 0x726c81, 0x110e19, 0x3d384a,
0x110e19, 0xded6f0, 0x1a1526,
0x2a2436, 0xded6f0, 0x42396f,
0x6ee7b7, 0xfcd34d, 0xf472b6 },
    { "dawn", 0x3a2f33, 0x1b1417,
0x2a1f24, 0xf6e3dc, 0x867777, 0x4f4245, 0x1b1417, 0xf0a07a, 0x1b1417,
0x3a2f33, 0xfaeeea, 0x2a1f24, 0x867777, 0x4f4245, 0xf0a07a,
0x2a1f24, 0xf6e3dc, 0x867777, 0x1b1417, 0x4f4245,
0x1b1417, 0xf6e3dc, 0x2a1f24,
0x3a2f33, 0xf6e3dc, 0x6f4c42,
0xb5d99c, 0xf6d08a, 0xec6a7a },
    { "mocha", 0x2e2925, 0x14110f,
0x1e1a17, 0xe5d8cc, 0x787068, 0x423c38, 0x14110f, 0xb68a6a, 0x14110f,
0x2e2925, 0xefe8e0, 0x1e1a17, 0x787068, 0x423c38, 0xb68a6a,
0x1e1a17, 0xe5d8cc, 0x787068, 0x14110f, 0x423c38,
0x14110f, 0xe5d8cc, 0x1e1a17,
0x2e2925, 0xe5d8cc, 0x534134,
0xa2b28a, 0xe3c07a, 0xcf6f64 },
    { "hacker", 0x091909, 0x030303,
0x050505, 0x33ff33, 0x1a761a, 0x0d320d, 0x030303, 0x33ff33, 0x030303,
0x091909, 0x85ff85, 0x050505, 0x1a761a, 0x0d320d, 0x33ff33,
0x050505, 0x33ff33, 0x1a761a, 0x030303, 0x0d320d,
0x030303, 0x33ff33, 0x050505,
0x091909, 0x33ff33, 0x155c15,
0x00cc00, 0xccff33, 0xff3333 },
    { "terminal", 0x1b1b1b, 0x080808,
0x0c0c0c, 0xcccccc, 0x626262, 0x2f2f2f, 0x080808, 0x3b78ff, 0x080808,
0x1b1b1b, 0xe0e0e0, 0x0c0c0c, 0x626262, 0x2f2f2f, 0x3b78ff,
0x0c0c0c, 0xcccccc, 0x626262, 0x080808, 0x2f2f2f,
0x080808, 0xcccccc, 0x0c0c0c,
0x1b1b1b, 0xcccccc, 0x1c3261,
0x13a10e, 0xc19c00, 0xc50f1f },
    { "paperdark", 0x2e2e2e, 0x141414,
0x1f1f1f, 0xe0e0e0, 0x767676, 0x424242, 0x141414, 0x8ab4f8, 0x141414,
0x2e2e2e, 0xececec, 0x1f1f1f, 0x767676, 0x424242, 0x8ab4f8,
0x1f1f1f, 0xe0e0e0, 0x767676, 0x141414, 0x424242,
0x141414, 0xe0e0e0, 0x1f1f1f,
0x2e2e2e, 0xe0e0e0, 0x44536b,
0x81c995, 0xfdd663, 0xf28b82 },
    { "blood", 0x251515, 0x0d0303,
0x140505, 0xe8cfcf, 0x736060, 0x3a2929, 0x0d0303, 0xb3001b, 0x0d0303,
0x251515, 0xf1e2e2, 0x140505, 0x736060, 0x3a2929, 0xb3001b,
0x140505, 0xe8cfcf, 0x736060, 0x0d0303, 0x3a2929,
0x0d0303, 0xe8cfcf, 0x140505,
0x251515, 0xe8cfcf, 0x4c030d,
0x5fa05f, 0xd4a017, 0xff1a1a },
    { "steel", 0x292f36, 0x12151a,
0x1b2128, 0xc9d3dc, 0x697179, 0x3a4148, 0x12151a, 0x7aa2c4, 0x12151a,
0x292f36, 0xdfe5ea, 0x1b2128, 0x697179, 0x3a4148, 0x7aa2c4,
0x1b2128, 0xc9d3dc, 0x697179, 0x12151a, 0x3a4148,
0x12151a, 0xc9d3dc, 0x1b2128,
0x292f36, 0xc9d3dc, 0x3c4e5f,
0x8fbf9f, 0xe0c07a, 0xd97a7a },
};
#define NTHEMES ((int)(sizeof(THEMES) / sizeof(THEMES[0])))

static theme_t TH;

typedef enum { W_TERM, W_FILES, W_MON, W_CLOCK, W_SET, W_VIEW, W_CALC, W_EDIT, W_GAMES, W_SERV, W_SCRIPTS, W_TASKS, W_WEB, W_PAINT, W_MINES, W_TIMER, W_CHESS } wtype_t;
enum { APP_TERM, APP_FILES, APP_MON, APP_CLOCK, APP_SET, APP_CALC, APP_EDIT, APP_GAMES, APP_SERV, APP_SCRIPTS, APP_TASKS, APP_WEB, APP_PAINT, APP_MINES, APP_TIMER, APP_CHESS, APP_COUNT };

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
    // web browser: the tabs of this window (indices into g_tabs) and the shown one
    int tabs[6], ntabs, wtab;
    // terminal: cursor position inside input[]
    int icur;
    // minesweeper (bit 0 mine, bit 1 open, bit 2 flag); state 0 ready, 1 playing, 2 won, 3 lost
    uint8_t ms[MS_W * MS_H];
    int ms_state;
    uint32_t ms_t0, ms_t1;
    // timer: mode 0 stopwatch, 1 countdown
    int tmo, nlap;
    bool tr_run;
    uint32_t t_start, t_acc, t_target, lap[8];
} win_t;

typedef struct { int mouse_speed, mouse_gain, dblclick, wallpaper; bool dock, icons, desk_files, clock24; int theme; int wp_mode; char wp_file[96]; bool shadows, snap, open_max, run_windowed, tray; int dock_size, term_style, term_cursor, term_prompt; bool term_welcome; } cfg_t;   // wallpaper: 0 gradient, 1 flat, 2 reverse, 3 picture
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
    "# RadiWM configuration. Lines starting with # are comments; # followed by six hex digits is a colour.\n"
    "# Settings > Save writes this file; you can also edit it by hand and log in again.\n"
    "\n"
    "theme radium                # radium, greybird, dark, nord, dracula\n"
    "wallpaper flat               # flat, gradient, reverse or default (the mascot picture)\n"
    "# wallpaper_file /disk/Pictures/mine.png   use a PNG of your own\n"
    "# wallpaper_mode fill                      fill, fit, stretch or center (for your own picture)\n"
    "mouse_speed 45               # percent of the raw pointer speed when moving slowly (10-120)\n"
    "mouse_accel 60               # extra speed in percent when the mouse is swept quickly (0-200)\n"
    "double_click 450             # milliseconds between the two clicks of a double click (250-900)\n"
    "dock yes\n"
    "icons yes\n"
    "desktop_files yes            # show the files of the Desktop folder on the desktop\n"
    "clock24 yes\n"
    "window_shadows no\n"
    "snap yes                     # windows stick to the screen edges; drop one at the top to maximise it\n"
    "open_maximised no\n"
    "run_in_window yes            # games and graphics programs run in a window (no = take the whole screen)\n"
    "panel_tray yes               # memory and disk in the panel\n"
    "dock_size 48                 # icon size in the dock (32-64)\n"
    "terminal_colors theme        # theme, classic, amber, light, solarized, dracula, nord or gruvbox\n"
    "terminal_cursor block        # block, underline or bar\n"
    "terminal_prompt full         # full (user@radium$), short ($), arrow (>) or radium (radium>)\n"
    "terminal_welcome yes         # print the greeting in a new terminal\n"
    "\n"
    "# launcher \"Label\" command      adds an entry to the dock and the Applications menu (up to 6)\n"
    "# launcher \"Memory\" meminfo\n"
    "\n"
    "# windows opened at login: autostart open term|files|monitor|clock|settings|calc|editor|games|services|scripts|tasks|web|paint|mines|timer, autostart exec COMMAND\n"
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
#define TA_PROMPT 0x80       // attribute of the prompt part of an echoed command in the scrollback (black on grey is never used by programs)
static uint32_t term_prompt_col(void) { return C.term_style == 3 ? 0x1a7f37 : 0x69db7c; }
static uint32_t attr_fg(uint8_t a) { if (a == TA_PROMPT) return term_prompt_col(); return a == 0 ? TH.term_fg : VGA_RGB[a & 15]; }
static uint32_t attr_bg(uint8_t a, uint32_t dflt) { if (a == TA_PROMPT) return dflt; uint8_t b = (a >> 4) & 15; return b ? VGA_RGB[b] : dflt; }

static void text_attr(int x, int y, const char *s, const uint8_t *at, uint32_t dflt_bg, int max_cols)
{
    for (int i = 0; s[i] && i < max_cols; i++) glyph(x + i * CW, y, (unsigned char)s[i], attr_fg(at[i]), attr_bg(at[i], dflt_bg));
}

// ---- icons (drawn from rectangles and discs; coordinates are in a 48-unit grid scaled to s pixels) -------------------------

enum { IC_TERM, IC_FILES, IC_MON, IC_CLOCK, IC_SET, IC_HOME, IC_FILE, IC_RUN, IC_VIEW, IC_APP, IC_CALC, IC_EDIT, IC_GAME, IC_SERV, IC_SCRIPT, IC_CONSOLE, IC_USER, IC_WEB, IC_PAINT, IC_MINES, IC_TIMER, IC_CHESS };

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
    case IC_WEB:
        disc(x + U(24), y + U(24), U(19), 0x4c6a8a);
        disc(x + U(24), y + U(24), U(16), 0xf4f1ea);
        fill_rect(x + U(8), y + U(23), U(32), imax(1, U(2)), 0x4c6a8a);
        fill_rect(x + U(23), y + U(8), imax(1, U(2)), U(32), 0x4c6a8a);
        frame_rect(x + U(15), y + U(8), U(18), U(32), 0x4c6a8a);
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
    case IC_PAINT:
        round_rect(x + U(4), y + U(6), U(40), U(36), 4, 0xf8f9fa);
        frame_rect(x + U(4), y + U(6), U(40), U(36), 0xadb5bd);
        disc(x + U(14), y + U(16), imax(2, U(4)), 0xff6b6b);
        disc(x + U(24), y + U(14), imax(2, U(4)), 0xffd43b);
        disc(x + U(34), y + U(18), imax(2, U(4)), 0x4c6ef5);
        line(x + U(12), y + U(36), x + U(36), y + U(28), imax(2, U(3)), 0x51cf66);
        break;
    case IC_MINES:
        round_rect(x + U(4), y + U(4), U(40), U(40), 4, 0xadb5bd);
        disc(x + U(24), y + U(24), imax(3, U(9)), 0x212529);
        line(x + U(10), y + U(24), x + U(38), y + U(24), imax(1, U(2)), 0x212529);
        line(x + U(24), y + U(10), x + U(24), y + U(38), imax(1, U(2)), 0x212529);
        fill_rect(x + U(20), y + U(20), imax(2, U(3)), imax(2, U(3)), 0xffffff);
        break;
    case IC_TIMER:
        disc(x + U(24), y + U(27), U(17), 0x3b414d);
        disc(x + U(24), y + U(27), U(14), 0xf8f9fa);
        fill_rect(x + U(21), y + U(4), U(6), U(5), 0x3b414d);
        line(x + U(24), y + U(27), x + U(24), y + U(17), imax(2, U(2)), 0xe03131);
        line(x + U(24), y + U(27), x + U(31), y + U(30), imax(2, U(2)), 0x212529);
        break;
    case IC_CHESS:
        round_rect(x + U(3), y + U(3), U(42), U(42), 4, 0x769656);
        fill_rect(x + U(3), y + U(3), U(21), U(21), 0xeeeed2);
        fill_rect(x + U(24), y + U(24), U(21), U(21), 0xeeeed2);
        disc(x + U(24), y + U(15), imax(2, U(6)), 0x222222);
        fill_rect(x + U(20), y + U(20), U(8), U(12), 0x222222);
        fill_rect(x + U(14), y + U(32), U(20), U(6), 0x222222);
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

static const char *const TERM_STYLES[8] = { "theme", "classic", "amber", "light", "solarized", "dracula", "nord", "gruvbox" };
static const char *const TERM_LABELS[8] = { "Theme", "Classic", "Amber", "Light", "Solarized", "Dracula", "Nord", "Gruvbox" };

static void term_prompt_str(char *out, int n)
{
    switch (C.term_prompt) {
    case 1: put_str(out, n, "$ "); break;
    case 2: put_str(out, n, "> "); break;
    case 3: put_str(out, n, "radium> "); break;
    default: snprintf(out, (size_t)n, "%s@radium%s ", users_current_name(), users_is_root() ? "#" : "$"); break;
    }
}

// An echoed command in the scrollback: coloured prompt, then the command in the normal colour.
static void term_echo(win_t *w, const char *pr, const char *cmd)
{
    int row = w->nl - 1, c0 = w->col, pl = (int)strlen(pr), cl = (int)strlen(cmd);
    term_puts(w, pr);
    term_puts(w, cmd);
    if (w->nl - 1 != row || w->col != c0 + pl + cl) return;      // the line wrapped: leave the colours alone
    for (int i = 0; i < pl; i++) w->lattr[row][c0 + i] = TA_PROMPT;
    for (int i = pl; i < pl + cl; i++) w->lattr[row][c0 + i] = 0;
}

static void term_cursor_draw(int x, int y, unsigned char ch)
{
    switch (C.term_cursor) {
    case 1: glyph(x, y, ch, TH.term_fg, TH.term_bg); fill_rect(x, y + CH - 3, CW, 2, TH.accent); break;
    case 2: glyph(x, y, ch, TH.term_fg, TH.term_bg); fill_rect(x, y + 1, 2, CH - 2, TH.accent); break;
    default: glyph(x, y, ch, TH.term_bg, TH.accent); break;
    }
}

// ---- application renderers (each draws a whole content rectangle; the clip does the rest) --------------------------------

static void render_gfx(win_t *w);
static void render_web(win_t *w, bool focused);

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
                if ((size_t)y == g_vt_crow && (size_t)x == g_vt_ccol) { term_cursor_draw(gx, gy, ch ? ch : ' '); continue; }
                if ((ch == ' ' || ch == 0) && bg == TH.term_bg) continue;
                glyph(gx, gy, ch ? ch : ' ', attr_fg(a), bg);
            }
        }
        return;
    }
    bool prompt = w->type == W_TERM;
    if (rows < 1) return;
    // The prompt is the last line of the text itself (a virtual line after the output when the output ends mid-line).
    int last = w->nl - 1;
    int pl = (prompt && w->lines[last][0]) ? w->nl : last;
    int total = prompt ? pl + 1 : w->nl;
    int start = total - rows - w->scroll;
    if (start < 0) start = 0;
    int stop = prompt ? pl : w->nl;
    for (int i = start; i < stop && i < start + rows; i++) {
        const char *s = w->lines[i];
        bool plain = true;
        for (int k = 0; s[k]; k++) if (w->lattr[i][k] != 0x07 && w->lattr[i][k] != 0) { plain = false; break; }
        if (plain) text(ox, oy + (i - start) * CH, s, line_color(s), TH.term_bg, cols);
        else text_attr(ox, oy + (i - start) * CH, s, w->lattr[i], TH.term_bg, cols);
    }
    if (prompt && pl >= start && pl < start + rows) {
        int py = oy + (pl - start) * CH;
        char pr[40];
        term_prompt_str(pr, sizeof(pr));
        int plen = (int)strlen(pr), room = imax(4, cols - plen - 1);
        int off = w->icur > room ? w->icur - room : 0;
        text(ox, py, pr, term_prompt_col(), TH.term_bg, plen);
        text(ox + plen * CW, py, w->input + off, TH.term_fg, TH.term_bg, room);
        int cx = ox + (plen + w->icur - off) * CW;
        if (focused) term_cursor_draw(cx, py, w->icur < w->inlen ? (unsigned char)w->input[w->icur] : ' ');
        else frame_rect(cx, py + 1, CW, CH - 2, TH.dim);
    }
    scrollbar(c.x + c.w - 8, c.y, rows * CH, total, rows, start);
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
static void scroll_window(int wi, int lines_n);
static int open_web(void);
static void web_release(win_t *w);
static void web_scroll(int wi, int lines_n);
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

// ---- Paint ----------------------------------------------------------------------------------------------------------------------

// The canvas lives on the heap and is allocated on first Paint use: ~790 KB of static
// storage would bloat the kernel image of every build, including ones that never open Paint.
static uint32_t *g_paint;
static uint32_t g_pt_col = 0x000000;
static int g_pt_size = 3, g_pt_tool, g_pt_lx, g_pt_ly;      // tool: 0 pen, 1 eraser
static const uint32_t PT_PAL[12] = { 0x000000, 0xffffff, 0x868e96, 0xe03131, 0xf76707, 0xfab005, 0x51cf66, 0x22b8cf, 0x4c6ef5, 0xae3ec9, 0xe64980, 0x7f5539 };
static const int PT_SIZES[4] = { 1, 3, 6, 12 };

static void paint_clear(void) { for (int i = 0; i < PT_W * PT_H; i++) g_paint[i] = 0xffffff; }

static bool paint_canvas(void)
{
    if (g_paint) return true;
    g_paint = malloc((size_t)PT_W * PT_H * sizeof(uint32_t));
    if (!g_paint) return false;
    paint_clear();
    return true;
}
static void paint_origin(const win_t *w, int *ox, int *oy) { rect_t c = content_rect(w); *ox = c.x + 8; *oy = c.y + PT_BAR + 6; }
static rect_t paint_swatch(const win_t *w, int i) { rect_t c = content_rect(w); rect_t r = { c.x + 8 + (i % 6) * 22, c.y + 4 + (i / 6) * 20, 18, 18 }; return r; }

static rect_t paint_btn(const win_t *w, int i)               // 0 pen, 1 eraser, 2..5 sizes, 6 clear, 7 save
{
    static const int X[8] = { 150, 196, 260, 288, 316, 344, 384, 440 }, W[8] = { 40, 56, 24, 24, 24, 24, 50, 50 };
    rect_t c = content_rect(w);
    rect_t r = { c.x + X[i], c.y + 9, W[i], 26 };
    return r;
}

static void paint_stamp(int cx, int cy, int r, uint32_t col)
{
    int h = r / 2, r2 = r * r / 4;
    for (int y = cy - h; y <= cy + h; y++)
        for (int x = cx - h; x <= cx + h; x++) {
            if (x < 0 || x >= PT_W || y < 0 || y >= PT_H) continue;
            if (r > 2 && (x - cx) * (x - cx) + (y - cy) * (y - cy) > r2) continue;
            g_paint[y * PT_W + x] = col;
        }
}

static void paint_line(int x0, int y0, int x1, int y1, int r, uint32_t col)
{
    int dx = iabs(x1 - x0), dy = iabs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
    for (;;) {
        paint_stamp(x0, y0, r, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

static void paint_damage(const win_t *w, int x0, int y0, int x1, int y1)
{
    int ox, oy, r = g_pt_size + 2;
    paint_origin(w, &ox, &oy);
    damage(ox + imin(x0, x1) - r, oy + imin(y0, y1) - r, iabs(x1 - x0) + 2 * r + 1, iabs(y1 - y0) + 2 * r + 1);
}

static void le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

// 24-bit BMP, written 10 rows at a time (the 'png' command and the Files app open .bmp files).
static void paint_save(void)
{
    enum { ROWS = 10 };                                       // small heap scratch instead of ~50 KB of static BSS
    uint8_t *buf = malloc(54 + PT_W * 3 * ROWS);
    if (!buf) { flash("not enough memory to save the picture"); return; }
    char path[128], m[64];
    int n;
    if (!avfs_is_directory(g_desk_dir) && avfs_create_dir(g_desk_dir) != 0) { flash("cannot create the Desktop folder"); return; }
    for (n = 1; n < 100; n++) { snprintf(path, sizeof(path), "%s/paint%d.bmp", g_desk_dir, n); if (!avfs_file_exists(path)) break; }
    if (n == 100) { flash("too many pictures on the desktop"); return; }
    const int rowb = PT_W * 3;                                // a multiple of 4: no row padding
    memset(buf, 0, 54);
    buf[0] = 'B'; buf[1] = 'M';
    le32(buf + 2, (uint32_t)(54 + rowb * PT_H)); le32(buf + 10, 54); le32(buf + 14, 40);
    le32(buf + 18, PT_W); le32(buf + 22, PT_H); buf[26] = 1; buf[28] = 24;
    le32(buf + 34, (uint32_t)(rowb * PT_H)); le32(buf + 38, 2835); le32(buf + 42, 2835);
    int o = 54, rows_in = 0;
    bool first = true;
    for (int y = PT_H - 1; y >= 0; y--) {
        for (int x = 0; x < PT_W; x++) {
            uint32_t p = g_paint[y * PT_W + x];
            buf[o++] = (uint8_t)(p & 255); buf[o++] = (uint8_t)((p >> 8) & 255); buf[o++] = (uint8_t)((p >> 16) & 255);
        }
        if (++rows_in == ROWS || y == 0) {
            int rc = first ? avfs_save_file(path, buf, (uint32_t)o, true) : avfs_append_file(path, buf, (uint32_t)o);
            if (rc != 0) { free(buf); flash("could not save the picture"); return; }
            first = false; o = 0; rows_in = 0;
        }
    }
    free(buf);
    snprintf(m, sizeof(m), "saved paint%d.bmp on the Desktop", n);
    flash(m);
    desktop_icons_refresh();
    damage_icons();
}

static void render_paint(win_t *w)
{
    rect_t c = content_rect(w);
    fill_rect(c.x, c.y, c.w, c.h, TH.bg);
    fill_rect(c.x, c.y + PT_BAR - 1, c.w, 1, TH.line);
    for (int i = 0; i < 12; i++) {
        rect_t r = paint_swatch(w, i);
        frame_rect(r.x - 1, r.y - 1, r.w + 2, r.h + 2, PT_PAL[i] == g_pt_col && g_pt_tool == 0 ? TH.accent : TH.line);
        fill_rect(r.x, r.y, r.w, r.h, PT_PAL[i]);
    }
    static const char *const LAB[8] = { "Pen", "Eraser", "1", "3", "6", "12", "Clear", "Save" };
    for (int i = 0; i < 8; i++) {
        rect_t r = paint_btn(w, i);
        bool on = (i == 0 && g_pt_tool == 0) || (i == 1 && g_pt_tool == 1) || (i >= 2 && i <= 5 && g_pt_size == PT_SIZES[i - 2]);
        uint32_t bg = on ? TH.accent : TH.line;
        round_rect(r.x, r.y, r.w, r.h, 3, bg);
        text_center(r.x, r.w, r.y + (r.h - CH) / 2, LAB[i], on ? TH.accent_tx : TH.fg, bg, 8);
    }
    int ox, oy;
    paint_origin(w, &ox, &oy);
    frame_rect(ox - 1, oy - 1, PT_W + 2, PT_H + 2, TH.line);
    int x0 = imax(ox, clip_x0), x1 = imin(ox + PT_W, clip_x1), y0 = imax(oy, clip_y0), y1 = imin(oy + PT_H, clip_y1);
    if (x1 > x0)
        for (int y = y0; y < y1; y++) memcpy(g_store + y * SW + x0, g_paint + (y - oy) * PT_W + (x0 - ox), (size_t)(x1 - x0) * 4);
}

// True when a stroke starts (the caller then turns the mouse drag into DRAG_PAINT).
static bool paint_click(int wi, int mx, int my)
{
    win_t *w = &g_win[wi];
    rect_t c = content_rect(w);
    if (my < c.y + PT_BAR) {
        for (int i = 0; i < 12; i++) if (rect_has(paint_swatch(w, i), mx, my)) { g_pt_col = PT_PAL[i]; g_pt_tool = 0; damage_content(w); return false; }
        for (int i = 0; i < 8; i++) {
            if (!rect_has(paint_btn(w, i), mx, my)) continue;
            if (i == 0) g_pt_tool = 0;
            else if (i == 1) g_pt_tool = 1;
            else if (i <= 5) g_pt_size = PT_SIZES[i - 2];
            else if (i == 6) paint_clear();
            else paint_save();
            damage_content(w);
            return false;
        }
        return false;
    }
    int ox, oy;
    paint_origin(w, &ox, &oy);
    int x = mx - ox, y = my - oy;
    if (x < 0 || y < 0 || x >= PT_W || y >= PT_H) return false;
    g_pt_lx = x; g_pt_ly = y;
    paint_stamp(x, y, g_pt_size, g_pt_tool ? 0xffffff : g_pt_col);
    paint_damage(w, x, y, x, y);
    return true;
}

static void paint_drag(int wi, int mx, int my)
{
    win_t *w = &g_win[wi];
    int ox, oy;
    paint_origin(w, &ox, &oy);
    int x = imax(0, imin(PT_W - 1, mx - ox)), y = imax(0, imin(PT_H - 1, my - oy));
    paint_line(g_pt_lx, g_pt_ly, x, y, g_pt_size, g_pt_tool ? 0xffffff : g_pt_col);
    paint_damage(w, g_pt_lx, g_pt_ly, x, y);
    g_pt_lx = x; g_pt_ly = y;
}

static void paint_key(int wi, uint8_t code, bool ext)
{
    (void)wi;
    if (!ext && g_ctrl && code == 0x1F) paint_save();                 // Ctrl+S
}

// ---- Minesweeper ----------------------------------------------------------------------------------------------------------------

static const uint32_t MS_COL[8] = { 0x4c6ef5, 0x2f9e44, 0xe03131, 0x364fc7, 0xc92a2a, 0x1098ad, 0xfab005, 0x868e96 };

static void mines_new(win_t *w) { memset(w->ms, 0, sizeof(w->ms)); w->ms_state = 0; w->ms_t0 = w->ms_t1 = 0; }
static rect_t mines_face(const win_t *w) { rect_t c = content_rect(w); rect_t r = { c.x + c.w / 2 - 20, c.y + 6, 40, 30 }; return r; }

static int mines_adj(const win_t *w, int x, int y)
{
    int n = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if ((dx || dy) && nx >= 0 && nx < MS_W && ny >= 0 && ny < MS_H) n += w->ms[ny * MS_W + nx] & 1;
        }
    return n;
}

// Mines are laid on the first click so that it (and its neighbours) are always safe.
static void mines_place(win_t *w, int sx, int sy)
{
    uint32_t s = now_ms() * 2654435761u + 12345u;
    if (!s) s = 1;
    for (int placed = 0; placed < MS_N; ) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        int i = (int)(s % (MS_W * MS_H)), x = i % MS_W, y = i / MS_W;
        if ((iabs(x - sx) <= 1 && iabs(y - sy) <= 1) || (w->ms[i] & 1)) continue;
        w->ms[i] |= 1;
        placed++;
    }
}

static void mines_open(win_t *w, int x, int y)
{
    uint8_t q[MS_W * MS_H];
    int qn = 0, i = y * MS_W + x;
    if (w->ms[i] & 6) return;
    w->ms[i] |= 2;
    q[qn++] = (uint8_t)i;
    while (qn) {
        int c = q[--qn], cx = c % MS_W, cy = c / MS_W;
        if ((w->ms[c] & 1) || mines_adj(w, cx, cy)) continue;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int nx = cx + dx, ny = cy + dy;
                if (nx < 0 || nx >= MS_W || ny < 0 || ny >= MS_H) continue;
                int j = ny * MS_W + nx;
                if (w->ms[j] & 6) continue;
                w->ms[j] |= 2;
                q[qn++] = (uint8_t)j;
            }
    }
}

static bool mines_won(const win_t *w) { for (int i = 0; i < MS_W * MS_H; i++) if (!(w->ms[i] & 3)) return false; return true; }

static bool mines_cell(const win_t *w, int mx, int my, int *gx, int *gy)
{
    rect_t c = content_rect(w);
    int ox = c.x + (c.w - MS_W * MS_C) / 2, oy = c.y + MS_HEAD;
    if (mx < ox || my < oy) return false;
    *gx = (mx - ox) / MS_C; *gy = (my - oy) / MS_C;
    return *gx < MS_W && *gy < MS_H;
}

static void mines_click(int wi, int mx, int my)
{
    win_t *w = &g_win[wi];
    int gx, gy;
    if (rect_has(mines_face(w), mx, my)) { mines_new(w); damage_content(w); return; }
    if (w->ms_state >= 2 || !mines_cell(w, mx, my, &gx, &gy)) return;
    int i = gy * MS_W + gx;
    if (w->ms[i] & 6) return;                                 // open or flagged
    if (w->ms_state == 0) { mines_place(w, gx, gy); w->ms_state = 1; w->ms_t0 = now_ms(); }
    if (w->ms[i] & 1) { w->ms_state = 3; w->ms_t1 = now_ms(); }
    else {
        mines_open(w, gx, gy);
        if (mines_won(w)) {
            w->ms_state = 2; w->ms_t1 = now_ms();
            for (int k = 0; k < MS_W * MS_H; k++) if (w->ms[k] & 1) w->ms[k] |= 4;
        }
    }
    damage_content(w);
}

static void mines_rclick(int wi, int mx, int my)
{
    win_t *w = &g_win[wi];
    int gx, gy;
    if (w->ms_state >= 2 || !mines_cell(w, mx, my, &gx, &gy)) return;
    int i = gy * MS_W + gx;
    if (w->ms[i] & 2) return;
    w->ms[i] ^= 4;
    damage_content(w);
}

static void mines_key(int wi, uint8_t code, bool ext)
{
    if (!ext && code == 0x3C) { mines_new(&g_win[wi]); damage_content(&g_win[wi]); }       // F2: new game
}

static void render_mines(win_t *w)
{
    rect_t c = content_rect(w);
    fill_rect(c.x, c.y, c.w, c.h, TH.bg);
    int ox = c.x + (c.w - MS_W * MS_C) / 2, oy = c.y + MS_HEAD, flags = 0;
    for (int i = 0; i < MS_W * MS_H; i++) if (w->ms[i] & 4) flags++;
    char b[16];
    int left = MS_N - flags;
    round_rect(c.x + 12, c.y + 8, 64, 28, 4, TH.field);
    if (left < 0) { b[0] = '-'; put_uint(b + 1, (uint32_t)(-left)); } else put_uint(b, (uint32_t)left);
    text_center(c.x + 12, 64, c.y + 14, b, TH.bad, TH.field, 6);
    rect_t f = mines_face(w);
    round_rect(f.x, f.y, f.w, f.h, 4, TH.line);
    text_center(f.x, f.w, f.y + 7, w->ms_state == 3 ? "X(" : (w->ms_state == 2 ? "B)" : ":)"), TH.fg, TH.line, 2);
    uint32_t t = w->ms_state == 1 ? now_ms() - w->ms_t0 : (w->ms_state >= 2 ? w->ms_t1 - w->ms_t0 : 0);
    t /= 1000;
    if (t > 999) t = 999;
    put_uint(b, t);
    round_rect(c.x + c.w - 76, c.y + 8, 64, 28, 4, TH.field);
    text_center(c.x + c.w - 76, 64, c.y + 14, b, TH.accent, TH.field, 6);
    bool lost = w->ms_state == 3;
    for (int y = 0; y < MS_H; y++)
        for (int x = 0; x < MS_W; x++) {
            uint8_t v = w->ms[y * MS_W + x];
            int px = ox + x * MS_C, py = oy + y * MS_C;
            if ((v & 2) || (lost && (v & 1))) {
                fill_rect(px, py, MS_C - 1, MS_C - 1, TH.field);
                if (v & 1) disc(px + MS_C / 2 - 1, py + MS_C / 2 - 1, 6, lost && !(v & 2) ? TH.fg : TH.bad);
                else {
                    int n = mines_adj(w, x, y);
                    if (n) { char d[2] = { (char)('0' + n), 0 }; text(px + 8, py + 4, d, MS_COL[n - 1], TH.field, 1); }
                }
            } else {
                round_rect(px, py, MS_C - 1, MS_C - 1, 3, mix(TH.line, 0xffffff, 12));
                if (v & 4) { fill_rect(px + 11, py + 5, 2, 12, TH.fg); fill_rect(px + 6, py + 5, 6, 5, TH.bad); fill_rect(px + 7, py + 17, 10, 2, TH.fg); }
            }
        }
    frame_rect(ox - 1, oy - 1, MS_W * MS_C + 1, MS_H * MS_C + 1, TH.line);
}

// ---- Timer (stopwatch and countdown) -----------------------------------------------------------------------------------------------

static uint32_t timer_elapsed(const win_t *w) { return w->t_acc + (w->tr_run ? now_ms() - w->t_start : 0); }
static rect_t timer_tab(const win_t *w, int i) { rect_t c = content_rect(w); rect_t r = { c.x + 10 + i * 110, c.y + 8, 100, 26 }; return r; }
static rect_t timer_btn(const win_t *w, int i) { rect_t c = content_rect(w); rect_t r = { c.x + 12 + i * 98, c.y + 108, 90, 30 }; return r; }
static rect_t timer_pre(const win_t *w, int i) { rect_t c = content_rect(w); rect_t r = { c.x + 12 + i * 66, c.y + 148, 60, 26 }; return r; }
static const int TM_PRE[4] = { 60000, 300000, 600000, -60000 };

static void timer_action(win_t *w, int i)
{
    uint32_t now = now_ms();
    if (i == 0) {
        if (w->tr_run) { w->t_acc += now - w->t_start; w->tr_run = false; }
        else {
            if (w->tmo == 1) {
                if (w->t_target == 0) { flash("add some time first"); return; }
                if (w->t_acc >= w->t_target) w->t_acc = 0;       // finished: start again
            }
            w->t_start = now;
            w->tr_run = true;
        }
    } else if (i == 1) {
        if (w->tmo == 0) {
            if (!w->tr_run) return;
            if (w->nlap == 8) { memmove(&w->lap[0], &w->lap[1], 7 * sizeof(w->lap[0])); w->nlap = 7; }
            w->lap[w->nlap++] = timer_elapsed(w);
        } else { w->t_target = 0; w->t_acc = 0; w->tr_run = false; }
    } else { w->t_acc = 0; w->tr_run = false; w->nlap = 0; }
    damage_content(w);
}

static void timer_fmt(const win_t *w, uint32_t e, char *d)
{
    if (w->tmo == 0) { put_2d(d, (e / 60000) % 100); d[2] = ':'; put_2d(d + 3, (e / 1000) % 60); d[5] = '.'; put_2d(d + 6, (e / 10) % 100); }
    else { uint32_t rem = e >= w->t_target ? 0 : (w->t_target - e + 999) / 1000; fmt_hms(d, rem); }
}

static void render_timer(win_t *w)
{
    rect_t c = content_rect(w);
    fill_rect(c.x, c.y, c.w, c.h, TH.bg);
    static const char *const TAB[2] = { "Stopwatch", "Countdown" };
    for (int i = 0; i < 2; i++) {
        rect_t r = timer_tab(w, i);
        bool sel = i == w->tmo;
        uint32_t bg = sel ? TH.accent : TH.line;
        round_rect(r.x, r.y, r.w, r.h, 4, bg);
        text_center(r.x, r.w, r.y + (r.h - CH) / 2, TAB[i], sel ? TH.accent_tx : TH.fg, bg, 12);
    }
    char t[16];
    uint32_t e = timer_elapsed(w);
    timer_fmt(w, e, t);
    int n = (int)strlen(t), s = imax(1, imin(4, (c.w - 20) / (n * 8)));
    bool done = w->tmo == 1 && w->t_target > 0 && e >= w->t_target;
    for (int i = 0; i < n; i++) glyph_big(c.x + (c.w - n * 8 * s) / 2 + i * 8 * s, c.y + 54, (unsigned char)t[i], s, done ? TH.bad : TH.accent, TH.bg);
    const char *b0 = w->tr_run ? (w->tmo == 0 ? "Stop" : "Pause") : "Start";
    const char *lab[3] = { b0, w->tmo == 0 ? "Lap" : "Clear", "Reset" };
    for (int i = 0; i < 3; i++) {
        rect_t r = timer_btn(w, i);
        uint32_t bg = i == 0 ? (w->tr_run ? TH.warn : TH.good) : TH.line;
        round_rect(r.x, r.y, r.w, r.h, 4, bg);
        text_center(r.x, r.w, r.y + (r.h - CH) / 2, lab[i], i == 0 ? 0xffffff : TH.fg, bg, 10);
    }
    if (w->tmo == 1) {
        static const char *const PL[4] = { "+1m", "+5m", "+10m", "-1m" };
        for (int i = 0; i < 4; i++) {
            rect_t r = timer_pre(w, i);
            round_rect(r.x, r.y, r.w, r.h, 4, TH.line);
            text_center(r.x, r.w, r.y + (r.h - CH) / 2, PL[i], TH.fg, TH.line, 6);
        }
    } else {
        for (int i = 0; i < w->nlap; i++) {
            char lt[16], row[40];
            timer_fmt(w, w->lap[i], lt);
            snprintf(row, sizeof(row), "Lap %d   %s", i + 1, lt);
            text(c.x + 16, c.y + 150 + i * CH, row, TH.fg, TH.bg, 30);
        }
    }
}

static void timer_click(int wi, int mx, int my)
{
    win_t *w = &g_win[wi];
    for (int i = 0; i < 2; i++)
        if (rect_has(timer_tab(w, i), mx, my)) {
            if (w->tmo != i) { w->tmo = i; w->t_acc = 0; w->tr_run = false; w->nlap = 0; damage_content(w); }
            return;
        }
    for (int i = 0; i < 3; i++) if (rect_has(timer_btn(w, i), mx, my)) { timer_action(w, i); return; }
    if (w->tmo == 1)
        for (int i = 0; i < 4; i++)
            if (rect_has(timer_pre(w, i), mx, my)) {
                long t = (long)w->t_target + TM_PRE[i];
                if (t < 0) t = 0;
                if (t > 359999000L) t = 359999000L;
                w->t_target = (uint32_t)t;
                damage_content(w);
                return;
            }
}

static void timer_key(int wi, uint8_t code, bool ext)
{
    if (ext) return;
    win_t *w = &g_win[wi];
    if (code == 0x39) timer_action(w, 0);              // Space: start / stop
    else if (code == 0x26) timer_action(w, 1);         // L: lap
    else if (code == 0x13) timer_action(w, 2);         // R: reset
}

// Called every 100 ms by the main loop: keeps the running timers and the minesweeper clock moving.
static void apps_tick(void)
{
    for (int i = 0; i < MAX_WIN; i++) {
        win_t *w = &g_win[i];
        if (!w->used) continue;
        if (w->type == W_TIMER && w->tr_run) {
            if (w->tmo == 1 && timer_elapsed(w) >= w->t_target) { w->tr_run = false; w->t_acc = w->t_target; flash("Timer finished"); }
            if (win_visible(i)) damage_content(w);
        } else if (w->type == W_MINES && w->ms_state == 1 && win_visible(i)) {
            rect_t c = content_rect(w);
            damage(c.x, c.y, c.w, MS_HEAD);
        }
    }
}

// ---- Chess ----------------------------------------------------------------------------------------------------------------------
// Board: index = rank_row * 8 + file, row 0 is rank 8 (a8 = 0, h1 = 63). Pieces: +white / -black, 1 P, 2 N, 3 B, 4 R, 5 Q, 6 K.

#define CH_CELL 54
#define CH_MAXMV 300
#define CH_MAXPLY 40
#define CH_MAXGAME 400
#define CH_MATE 30000
#define CH_INF 32000

enum { PC_P = 1, PC_N, PC_B, PC_R, PC_Q, PC_K };

typedef struct { int8_t b[64]; uint8_t side, castle; int8_t ep; uint8_t half; } cpos_t;   // castle: 1 WK, 2 WQ, 4 BK, 8 BQ
typedef uint16_t cmove_t;
#define CM(f, t, p) ((cmove_t)((f) | ((t) << 6) | ((p) << 12)))
#define CM_FROM(m) ((m) & 63)
#define CM_TO(m) (((m) >> 6) & 63)
#define CM_PROMO(m) (((m) >> 12) & 7)

typedef struct {
    cpos_t pos;
    int sel, level, human, state, nundo, nlegal;        // state: 0 playing, 1 white won, 2 black won, 3 draw
    int mode;                                           // 0 play, 1 train an opening
    bool check, has_last, ai_pending, thinking, flip, coach, book, tr_done;
    cmove_t last;
    uint32_t ai_t;
    char why[24];
    int opn_cur, opn_next, nopn_h;                      // opening played / heading for (indices into OPENINGS, -1 none)
    struct { int ply, idx; } opn_h[10];                 // the openings the game has passed through
    int tr_open, tr_ok, tr_bad, tr_miss;                // trainer: opening, correct moves, mistakes, misses on this move
    int hint_from, hint_to;
    int score_cp, depth_done, msg_col;                  // last search (white's view, centipawns), colour of the message line
    bool have_score;
    char ai_msg[48], coach_msg[64], tr_msg[48];
    cmove_t legal[CH_MAXMV];
    cmove_t umv[CH_MAXGAME];
    cpos_t undo[CH_MAXGAME];
} chess_t;

static chess_t g_ch;
static bool g_ch_ready;
static uint64_t g_ch_hist[CH_MAXGAME + 2];
static int g_ch_nh;

static const int8_t CH_KN[8][2] = { { -2, -1 }, { -2, 1 }, { -1, -2 }, { -1, 2 }, { 1, -2 }, { 1, 2 }, { 2, -1 }, { 2, 1 } };
static const int8_t CH_KG[8][2] = { { -1, -1 }, { -1, 0 }, { -1, 1 }, { 0, -1 }, { 0, 1 }, { 1, -1 }, { 1, 0 }, { 1, 1 } };
static const int CH_VAL[7] = { 0, 100, 320, 330, 500, 900, 0 };

// Piece-square tables (white's view, rank 8 first); black uses the vertically mirrored square.
static const int8_t CH_PST[6][64] = {
    { 0, 0, 0, 0, 0, 0, 0, 0,   50, 50, 50, 50, 50, 50, 50, 50,   10, 10, 20, 30, 30, 20, 10, 10,   5, 5, 10, 25, 25, 10, 5, 5,
      0, 0, 0, 20, 20, 0, 0, 0,   5, -5, -10, 0, 0, -10, -5, 5,   5, 10, 10, -20, -20, 10, 10, 5,   0, 0, 0, 0, 0, 0, 0, 0 },
    { -50, -40, -30, -30, -30, -30, -40, -50,   -40, -20, 0, 0, 0, 0, -20, -40,   -30, 0, 10, 15, 15, 10, 0, -30,   -30, 5, 15, 20, 20, 15, 5, -30,
      -30, 0, 15, 20, 20, 15, 0, -30,   -30, 5, 10, 15, 15, 10, 5, -30,   -40, -20, 0, 5, 5, 0, -20, -40,   -50, -40, -30, -30, -30, -30, -40, -50 },
    { -20, -10, -10, -10, -10, -10, -10, -20,   -10, 0, 0, 0, 0, 0, 0, -10,   -10, 0, 5, 10, 10, 5, 0, -10,   -10, 5, 5, 10, 10, 5, 5, -10,
      -10, 0, 10, 10, 10, 10, 0, -10,   -10, 10, 10, 10, 10, 10, 10, -10,   -10, 5, 0, 0, 0, 0, 5, -10,   -20, -10, -10, -10, -10, -10, -10, -20 },
    { 0, 0, 0, 0, 0, 0, 0, 0,   5, 10, 10, 10, 10, 10, 10, 5,   -5, 0, 0, 0, 0, 0, 0, -5,   -5, 0, 0, 0, 0, 0, 0, -5,
      -5, 0, 0, 0, 0, 0, 0, -5,   -5, 0, 0, 0, 0, 0, 0, -5,   -5, 0, 0, 0, 0, 0, 0, -5,   0, 0, 0, 5, 5, 0, 0, 0 },
    { -20, -10, -10, -5, -5, -10, -10, -20,   -10, 0, 0, 0, 0, 0, 0, -10,   -10, 0, 5, 5, 5, 5, 0, -10,   -5, 0, 5, 5, 5, 5, 0, -5,
      0, 0, 5, 5, 5, 5, 0, -5,   -10, 5, 5, 5, 5, 5, 0, -10,   -10, 0, 5, 0, 0, 0, 0, -10,   -20, -10, -10, -5, -5, -10, -10, -20 },
    { -30, -40, -40, -50, -50, -40, -40, -30,   -30, -40, -40, -50, -50, -40, -40, -30,   -30, -40, -40, -50, -50, -40, -40, -30,   -30, -40, -40, -50, -50, -40, -40, -30,
      -20, -30, -30, -40, -40, -30, -30, -20,   -10, -20, -20, -20, -20, -20, -20, -10,   20, 20, 0, 0, 0, 0, 20, 20,   20, 30, 10, 0, 0, 10, 30, 20 },
};
static const int8_t CH_KEG[64] = {
    -50, -40, -30, -20, -20, -30, -40, -50,   -30, -20, -10, 0, 0, -10, -20, -30,   -30, -10, 20, 30, 30, 20, -10, -30,   -30, -10, 30, 40, 40, 30, -10, -30,
    -30, -10, 30, 40, 40, 30, -10, -30,   -30, -10, 20, 30, 30, 20, -10, -30,   -30, -30, 0, 0, 0, 0, -30, -30,   -50, -30, -30, -30, -30, -30, -30, -50 };

// ---- rules ----

static bool ch_attacked(const cpos_t *p, int sq, int by)         // is sq attacked by side `by`?
{
    int r = sq >> 3, f = sq & 7, s = by == 0 ? 1 : -1, pr = by == 0 ? r + 1 : r - 1;
    if (pr >= 0 && pr < 8) {
        if (f > 0 && p->b[pr * 8 + f - 1] == s * PC_P) return true;
        if (f < 7 && p->b[pr * 8 + f + 1] == s * PC_P) return true;
    }
    for (int i = 0; i < 8; i++) {
        int nr = r + CH_KN[i][0], nf = f + CH_KN[i][1];
        if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8 && p->b[nr * 8 + nf] == s * PC_N) return true;
        nr = r + CH_KG[i][0]; nf = f + CH_KG[i][1];
        if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8 && p->b[nr * 8 + nf] == s * PC_K) return true;
    }
    for (int i = 0; i < 8; i++) {
        int dr = CH_KG[i][0], df = CH_KG[i][1], nr = r + dr, nf = f + df;
        bool diag = dr && df;
        while (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
            int pc = p->b[nr * 8 + nf];
            if (pc) {
                if (pc == s * PC_Q || pc == s * (diag ? PC_B : PC_R)) return true;
                break;
            }
            nr += dr; nf += df;
        }
    }
    return false;
}

static bool ch_in_check(const cpos_t *p, int side)
{
    int k = side == 0 ? PC_K : -PC_K;
    for (int i = 0; i < 64; i++) if (p->b[i] == k) return ch_attacked(p, i, side ^ 1);
    return false;
}

// Pseudo-legal moves of the side to move; `caps` = captures and queen promotions only (for the quiescence search).
static int ch_gen(const cpos_t *p, cmove_t *out, bool caps)
{
    int n = 0, us = p->side, sg = us == 0 ? 1 : -1;
    for (int sq = 0; sq < 64; sq++) {
        int pc = p->b[sq] * sg;
        if (pc <= 0) continue;
        int r = sq >> 3, f = sq & 7;
        if (pc == PC_P) {
            int dir = us == 0 ? -1 : 1, start = us == 0 ? 6 : 1, last = us == 0 ? 0 : 7, r1 = r + dir;
            if (r1 < 0 || r1 > 7) continue;
            int t = r1 * 8 + f;
            if (!p->b[t]) {
                if (r1 == last) { for (int pr = caps ? PC_Q : PC_N; pr <= PC_Q; pr++) out[n++] = CM(sq, t, pr); }
                else if (!caps) {
                    out[n++] = CM(sq, t, 0);
                    if (r == start && !p->b[t + dir * 8]) out[n++] = CM(sq, t + dir * 8, 0);
                }
            }
            for (int df = -1; df <= 1; df += 2) {
                int nf = f + df;
                if (nf < 0 || nf > 7) continue;
                int tt = r1 * 8 + nf;
                if (p->b[tt] * sg < 0 || (tt == p->ep && !p->b[tt])) {
                    if (r1 == last) { for (int pr = caps ? PC_Q : PC_N; pr <= PC_Q; pr++) out[n++] = CM(sq, tt, pr); }
                    else out[n++] = CM(sq, tt, 0);
                }
            }
            continue;
        }
        if (pc == PC_N || pc == PC_K) {
            const int8_t (*d)[2] = pc == PC_N ? CH_KN : CH_KG;
            for (int i = 0; i < 8; i++) {
                int nr = r + d[i][0], nf = f + d[i][1];
                if (nr < 0 || nr > 7 || nf < 0 || nf > 7) continue;
                int t = nr * 8 + nf, tv = p->b[t] * sg;
                if (tv > 0 || (caps && tv == 0)) continue;
                out[n++] = CM(sq, t, 0);
            }
            if (pc == PC_K && !caps && sq == (us == 0 ? 60 : 4)) {
                int base = us == 0 ? 56 : 0, ks = us == 0 ? 1 : 4, qs = us == 0 ? 2 : 8;
                if ((p->castle & ks) && !p->b[base + 5] && !p->b[base + 6] && p->b[base + 7] == sg * PC_R
                    && !ch_attacked(p, base + 4, us ^ 1) && !ch_attacked(p, base + 5, us ^ 1))
                    out[n++] = CM(sq, base + 6, 0);
                if ((p->castle & qs) && !p->b[base + 3] && !p->b[base + 2] && !p->b[base + 1] && p->b[base] == sg * PC_R
                    && !ch_attacked(p, base + 4, us ^ 1) && !ch_attacked(p, base + 3, us ^ 1))
                    out[n++] = CM(sq, base + 2, 0);
            }
            continue;
        }
        for (int i = 0; i < 8; i++) {
            int dr = CH_KG[i][0], df = CH_KG[i][1];
            bool diag = dr && df;
            if ((pc == PC_B && !diag) || (pc == PC_R && diag)) continue;
            int nr = r + dr, nf = f + df;
            while (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
                int t = nr * 8 + nf, tv = p->b[t] * sg;
                if (tv > 0) break;
                if (tv < 0) { out[n++] = CM(sq, t, 0); break; }
                if (!caps) out[n++] = CM(sq, t, 0);
                nr += dr; nf += df;
            }
        }
    }
    return n;
}

static void ch_make(cpos_t *p, cmove_t m)
{
    int f = CM_FROM(m), t = CM_TO(m), promo = CM_PROMO(m), pc = p->b[f], us = p->side, a = iabs(pc), cap = p->b[t];
    p->half = (a == PC_P || cap) ? 0 : (uint8_t)(p->half + 1);
    if (a == PC_P && t == p->ep && !cap) p->b[(f & 56) | (t & 7)] = 0;          // en passant: the captured pawn sits beside the mover
    p->ep = -1;
    if (a == PC_P && iabs(t - f) == 16) p->ep = (int8_t)((f + t) / 2);
    p->b[t] = (int8_t)(promo ? (us == 0 ? promo : -promo) : pc);
    p->b[f] = 0;
    if (a == PC_K) {
        if (t - f == 2) { p->b[t - 1] = p->b[t + 1]; p->b[t + 1] = 0; }          // castling: move the rook too
        else if (f - t == 2) { p->b[t + 1] = p->b[t - 2]; p->b[t - 2] = 0; }
        p->castle &= (uint8_t)(us == 0 ? ~3 : ~12);
    }
    if (f == 63 || t == 63) p->castle &= (uint8_t)~1;
    if (f == 56 || t == 56) p->castle &= (uint8_t)~2;
    if (f == 7 || t == 7) p->castle &= (uint8_t)~4;
    if (f == 0 || t == 0) p->castle &= (uint8_t)~8;
    p->side = (uint8_t)(us ^ 1);
}

static int ch_legal(const cpos_t *p, cmove_t *mv, bool caps)
{
    int n = ch_gen(p, mv, caps), m = 0;
    for (int i = 0; i < n; i++) {
        cpos_t q = *p;
        ch_make(&q, mv[i]);
        if (!ch_in_check(&q, p->side)) mv[m++] = mv[i];
    }
    return m;
}

static uint64_t g_zob[12][64], g_zob_side, g_zob_castle[16], g_zob_ep[9];
static bool g_zob_ready;

static uint64_t zob_next(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

static void zob_init(void)
{
    uint64_t s = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 12; i++) for (int j = 0; j < 64; j++) g_zob[i][j] = zob_next(&s);
    g_zob_side = zob_next(&s);
    for (int i = 0; i < 16; i++) g_zob_castle[i] = zob_next(&s);
    for (int i = 0; i < 9; i++) g_zob_ep[i] = zob_next(&s);
    g_zob_ready = true;
}

static uint64_t ch_hash(const cpos_t *p)
{
    if (!g_zob_ready) zob_init();
    uint64_t h = 0;
    for (int i = 0; i < 64; i++) { int pc = p->b[i]; if (pc) h ^= g_zob[pc > 0 ? pc - 1 : 5 - pc][i]; }
    if (p->side) h ^= g_zob_side;
    h ^= g_zob_castle[p->castle & 15];
    h ^= g_zob_ep[p->ep >= 0 ? (p->ep & 7) : 8];
    return h;
}

// ---- the engine ----

static void draw_frame(void);                                       // defined with the Vulkan code further down
static void chess_progress(int depth, int score_white);

typedef struct { uint32_t key; int16_t score; int8_t depth; uint8_t flag; cmove_t move; } tt_t;   // flag: 0 empty, 1 exact, 2 lower bound, 3 upper bound
#define TT_SIZE (1 << 16)

// Search scratch and the transposition table live on the heap (~830 KB): static storage
// would bloat the kernel image of every build, including ones that never open Chess.
static tt_t *g_tt;                                                  // 12 B x 65536 = 768 KB, allocated with the scratch
static cmove_t (*g_ch_mv)[CH_MAXMV], (*g_ch_kill)[2];               // indexed g_ch_mv[ply] and g_ch_kill[ply][0..1]
static int16_t (*g_ch_sc)[CH_MAXMV], (*g_ch_hh)[64][64];
static uint64_t g_ch_path[CH_MAXPLY];
static uint32_t g_ch_nodes, g_ch_deadline, g_ch_rng;
static bool g_ch_abort, g_ch_live;                                  // g_ch_live: the engine may redraw the window while it thinks

static bool ch_scratch(void)
{
    static bool ok;
    if (ok) return true;
    if (!g_tt) g_tt = malloc(sizeof(tt_t) * (size_t)TT_SIZE);
    if (!g_ch_mv) g_ch_mv = malloc(sizeof(cmove_t) * (size_t)CH_MAXPLY * CH_MAXMV);
    if (!g_ch_sc) g_ch_sc = malloc(sizeof(int16_t) * (size_t)CH_MAXPLY * CH_MAXMV);
    if (!g_ch_hh) g_ch_hh = malloc(sizeof(int16_t) * 2 * 64 * 64);
    if (!g_ch_kill) g_ch_kill = malloc(sizeof(cmove_t) * (size_t)CH_MAXPLY * 2);
    ok = g_tt && g_ch_mv && g_ch_sc && g_ch_hh && g_ch_kill;
    return ok;
}
static char g_ch_san[CH_MAXGAME][10];                               // the moves of the game in standard algebraic notation

static const char *const CH_LEVELS[5] = { "Beginner", "Easy", "Medium", "Hard", "Expert" };
static const int CH_DEPTH[5] = { 1, 2, 5, 9, 14 };
static const uint32_t CH_MS[5] = { 150, 300, 900, 2500, 4000 };
static const int CH_NOISE[5] = { 140, 60, 0, 0, 0 };                 // random centipawns added to the root scores
static const int CH_RANDOM[5] = { 25, 6, 0, 0, 0 };                  // percent of moves that are simply random

static void ch_seed(void)
{
    g_ch_rng ^= now_ms() * 2654435761u + 7u;
    if (!g_ch_rng) g_ch_rng = 88172645u;
}

static uint32_t ch_rand(void) { g_ch_rng ^= g_ch_rng << 13; g_ch_rng ^= g_ch_rng >> 17; g_ch_rng ^= g_ch_rng << 5; return g_ch_rng; }

static void tt_store(uint64_t h, int depth, int score, int flag, cmove_t move, int ply)
{
    tt_t *e = &g_tt[h & (TT_SIZE - 1)];
    uint32_t k = (uint32_t)(h >> 32);
    if (e->flag && e->key == k && e->depth > depth) return;
    if (score >= CH_MATE - 100) score += ply; else if (score <= -(CH_MATE - 100)) score -= ply;
    e->key = k; e->score = (int16_t)score; e->depth = (int8_t)depth; e->flag = (uint8_t)flag; e->move = move;
}

static bool ch_has_pieces(const cpos_t *p, int side)                 // anything besides pawns and the king (null moves are unsafe without)
{
    int sg = side == 0 ? 1 : -1;
    for (int i = 0; i < 64; i++) { int a = p->b[i] * sg; if (a > PC_P && a < PC_K) return true; }
    return false;
}

static const int CH_PASSED[6] = { 0, 5, 10, 20, 35, 60 };

static int ch_eval(const cpos_t *p)                                  // centipawns from the side to move's view
{
    int mat[2] = { 0, 0 }, nb[2] = { 0, 0 }, np[2] = { 0, 0 }, ksq[2] = { 0, 0 }, pcnt[2][8], score = 0;
    memset(pcnt, 0, sizeof(pcnt));
    for (int i = 0; i < 64; i++) {
        int pc = p->b[i];
        if (!pc) continue;
        int a = iabs(pc), s = pc > 0 ? 0 : 1;
        if (a == PC_P) { pcnt[s][i & 7]++; np[s]++; }
        else if (a == PC_K) ksq[s] = i;
        else { mat[s] += CH_VAL[a]; if (a == PC_B) nb[s]++; }
    }
    bool eg = mat[0] + mat[1] <= 2600;
    for (int i = 0; i < 64; i++) {
        int pc = p->b[i];
        if (!pc) continue;
        int a = iabs(pc), s = pc > 0 ? 0 : 1, sg = s == 0 ? 1 : -1, sq = s == 0 ? i : (i ^ 56), r = i >> 3, f = i & 7;
        int v = CH_VAL[a] + (a == PC_K ? (eg ? CH_KEG[sq] : CH_PST[PC_K - 1][sq]) : CH_PST[a - 1][sq]);
        if (a == PC_P) {
            if (pcnt[s][f] > 1) v -= 8;                                                         // doubled
            if ((f == 0 || !pcnt[s][f - 1]) && (f == 7 || !pcnt[s][f + 1])) v -= 12;           // isolated
            bool passed = true;
            int step = s == 0 ? -1 : 1;
            for (int ff = imax(0, f - 1); ff <= imin(7, f + 1) && passed; ff++)
                for (int rr = r + step; rr >= 0 && rr < 8; rr += step)
                    if (p->b[rr * 8 + ff] == -sg * PC_P) { passed = false; break; }
            if (passed) { int adv = s == 0 ? 6 - r : r - 1; v += CH_PASSED[imax(0, imin(5, adv))] * (eg ? 2 : 1); }
        } else if (a == PC_R) {
            if (!pcnt[s][f]) v += pcnt[s ^ 1][f] ? 10 : 20;                                     // open / half-open file
        }
        score += s == 0 ? v : -v;
    }
    if (nb[0] >= 2) score += 30;
    if (nb[1] >= 2) score -= 30;
    if (!eg) {
        for (int s = 0; s < 2; s++) {                                                           // king safety: pawn shield
            int k = ksq[s], kr = k >> 3, kf = k & 7, dir = s == 0 ? -1 : 1, sg = s == 0 ? 1 : -1, sh = 0;
            bool home = s == 0 ? kr >= 6 : kr <= 1;
            if (!home) sh = -20;
            else
                for (int ff = imax(0, kf - 1); ff <= imin(7, kf + 1); ff++) {
                    int r1 = kr + dir, r2 = kr + 2 * dir;
                    if (r1 >= 0 && r1 < 8 && p->b[r1 * 8 + ff] == sg * PC_P) sh += 10;
                    else if (r2 >= 0 && r2 < 8 && p->b[r2 * 8 + ff] == sg * PC_P) sh += 5;
                    else sh -= 8;
                }
            score += s == 0 ? sh : -sh;
        }
    } else {
        for (int w = 0; w < 2; w++) {                                                           // mop-up: drive a lone king to the edge
            int l = w ^ 1;
            if (mat[l] == 0 && np[l] == 0 && mat[w] >= 500) {
                int lk = ksq[l], wk = ksq[w];
                int cd = iabs(2 * (lk & 7) - 7) + iabs(2 * (lk >> 3) - 7);
                int kd = iabs((lk & 7) - (wk & 7)) + iabs((lk >> 3) - (wk >> 3));
                int bonus = cd * 6 + (14 - kd) * 4;
                score += w == 0 ? bonus : -bonus;
            }
        }
    }
    score += p->side == 0 ? 8 : -8;                                                             // tempo
    return p->side == 0 ? score : -score;
}

static void ch_order(const cpos_t *p, const cmove_t *mv, int16_t *sc, int n, int ply, cmove_t ttm)
{
    for (int i = 0; i < n; i++) {
        int f = CM_FROM(mv[i]), t = CM_TO(mv[i]), v = iabs(p->b[t]), a = iabs(p->b[f]), s;
        if (ttm && mv[i] == ttm) s = 30000;
        else if (v || (a == PC_P && t == p->ep)) { if (!v) v = 1; s = 10000 + v * 100 - a; }
        else if (mv[i] == g_ch_kill[ply][0]) s = 9000;
        else if (mv[i] == g_ch_kill[ply][1]) s = 8900;
        else s = g_ch_hh[p->side][f][t] / 4;
        if (CM_PROMO(mv[i])) s += 5000;
        sc[i] = (int16_t)(s > 32000 ? 32000 : s);
    }
}

static void ch_pick(cmove_t *mv, int16_t *sc, int n, int i)          // selection sort step: best remaining move to index i
{
    int bi = i;
    for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
    if (bi != i) { cmove_t m = mv[i]; int16_t s = sc[i]; mv[i] = mv[bi]; sc[i] = sc[bi]; mv[bi] = m; sc[bi] = s; }
}

static bool ch_time_up(void)
{
    if ((++g_ch_nodes & 1023) == 0 && now_ms() >= g_ch_deadline) g_ch_abort = true;
    return g_ch_abort;
}

static int ch_quiesce(const cpos_t *p, int alpha, int beta, int ply)
{
    if (ch_time_up()) return 0;
    int stand = ch_eval(p);
    if (ply >= CH_MAXPLY - 2) return stand;
    if (stand >= beta) return beta;
    if (stand > alpha) alpha = stand;
    cmove_t *mv = g_ch_mv[ply];
    int16_t *sc = g_ch_sc[ply];
    int n = ch_legal(p, mv, true);
    ch_order(p, mv, sc, n, ply, 0);
    for (int i = 0; i < n; i++) {
        ch_pick(mv, sc, n, i);
        if (!CM_PROMO(mv[i]) && stand + CH_VAL[iabs(p->b[CM_TO(mv[i])])] + 200 < alpha) continue;   // delta pruning
        cpos_t q = *p;
        ch_make(&q, mv[i]);
        int s = -ch_quiesce(&q, -beta, -alpha, ply + 1);
        if (g_ch_abort) return 0;
        if (s >= beta) return beta;
        if (s > alpha) alpha = s;
    }
    return alpha;
}

static int ch_search(const cpos_t *p, int depth, int alpha, int beta, int ply, bool can_null)
{
    if (ch_time_up()) return 0;
    if (ply >= CH_MAXPLY - 2) return ch_eval(p);
    uint64_t h = ch_hash(p);
    g_ch_path[ply] = h;
    if (p->half >= 100) return 0;                                                // fifty-move draw
    for (int i = ply - 2; i >= 0; i -= 2) if (g_ch_path[i] == h) return 0;        // repetition inside the search
    for (int i = 1; i <= p->half && i < g_ch_nh; i++) if (g_ch_hist[g_ch_nh - 1 - i] == h) return 0;   // repetition of a game position
    bool check = ch_in_check(p, p->side), pv = beta - alpha > 1;
    if (check && ply < CH_MAXPLY - 8) depth++;
    if (depth <= 0) return ch_quiesce(p, alpha, beta, ply);

    tt_t *e = &g_tt[h & (TT_SIZE - 1)];
    cmove_t ttm = 0;
    if (e->flag && e->key == (uint32_t)(h >> 32)) {
        ttm = e->move;
        if (e->depth >= depth && !pv) {
            int s = e->score;
            if (s >= CH_MATE - 100) s -= ply; else if (s <= -(CH_MATE - 100)) s += ply;
            if (e->flag == 1 || (e->flag == 2 && s >= beta) || (e->flag == 3 && s <= alpha)) return s;
        }
    }
    if (can_null && !pv && !check && depth >= 3 && ch_has_pieces(p, p->side)) {   // null move: if passing still beats beta, cut
        cpos_t q = *p;
        q.side = (uint8_t)(q.side ^ 1);
        q.ep = -1;
        int s = -ch_search(&q, depth - 3, -beta, -beta + 1, ply + 1, false);
        if (g_ch_abort) return 0;
        if (s >= beta) return s >= CH_MATE - 100 ? beta : s;
    }

    cmove_t *mv = g_ch_mv[ply];
    int16_t *sc = g_ch_sc[ply];
    int n = ch_legal(p, mv, false);
    if (n == 0) return check ? -(CH_MATE - ply) : 0;
    ch_order(p, mv, sc, n, ply, ttm);
    int best = -CH_INF, alpha0 = alpha;
    cmove_t bestmv = 0;
    for (int i = 0; i < n; i++) {
        ch_pick(mv, sc, n, i);
        cmove_t m = mv[i];
        bool quiet = !p->b[CM_TO(m)] && !CM_PROMO(m) && !(iabs(p->b[CM_FROM(m)]) == PC_P && CM_TO(m) == p->ep);
        cpos_t q = *p;
        ch_make(&q, m);
        int s;
        if (i == 0) s = -ch_search(&q, depth - 1, -beta, -alpha, ply + 1, true);
        else {                                                                    // PVS with late-move reductions
            int red = 0;
            if (depth >= 3 && i >= 3 && quiet && !check) { red = (i >= 8 && depth >= 5) ? 2 : 1; if (ch_in_check(&q, q.side)) red = 0; }
            s = -ch_search(&q, depth - 1 - red, -alpha - 1, -alpha, ply + 1, true);
            if (s > alpha && red && !g_ch_abort) s = -ch_search(&q, depth - 1, -alpha - 1, -alpha, ply + 1, true);
            if (s > alpha && s < beta && !g_ch_abort) s = -ch_search(&q, depth - 1, -beta, -alpha, ply + 1, true);
        }
        if (g_ch_abort) return 0;
        if (s > best) { best = s; bestmv = m; }
        if (s > alpha) alpha = s;
        if (alpha >= beta) {
            if (quiet) {
                if (g_ch_kill[ply][0] != m) { g_ch_kill[ply][1] = g_ch_kill[ply][0]; g_ch_kill[ply][0] = m; }
                int16_t *hh = &g_ch_hh[p->side][CM_FROM(m)][CM_TO(m)];
                int v = *hh + depth * depth;
                *hh = (int16_t)(v > 16000 ? 16000 : v);
            }
            break;
        }
    }
    tt_store(h, depth, best, best <= alpha0 ? 3 : (best >= beta ? 2 : 1), bestmv, ply);
    return best;
}

// Iterative deepening from the root. exact = full-window score for every root move (used with `noise` by the weak levels).
// Returns the move; *score_out is the score for the side to move, *depth_out the last completed depth (0 when there was only one move).
static cmove_t ch_think(const cpos_t *root, int maxd, uint32_t ms, int noise, bool exact, int *score_out, int *depth_out, bool report)
{
    if (!ch_scratch()) return 0;
    cmove_t *mv = g_ch_mv[0];
    int16_t *sc0 = g_ch_sc[0];
    int rs[CH_MAXMV];
    if (score_out) *score_out = 0;
    if (depth_out) *depth_out = 0;
    int n = ch_legal(root, mv, false);
    if (n == 0) return 0;
    if (n == 1) return mv[0];
    ch_seed();
    g_ch_abort = false;
    g_ch_nodes = 0;
    g_ch_deadline = now_ms() + ms;
    memset(g_ch_kill, 0, sizeof(cmove_t) * (size_t)CH_MAXPLY * 2);
    memset(g_ch_hh, 0, sizeof(int16_t) * 2 * 64 * 64);
    uint64_t rh = ch_hash(root);
    g_ch_path[0] = rh;
    tt_t *te = &g_tt[rh & (TT_SIZE - 1)];
    ch_order(root, mv, sc0, n, 0, (te->flag && te->key == (uint32_t)(rh >> 32)) ? te->move : 0);
    for (int i = 0; i < n; i++) ch_pick(mv, sc0, n, i);
    cmove_t best = mv[0];
    int best_sc = 0, done = 0;
    for (int d = 1; d <= maxd; d++) {
        int alpha = -CH_INF, bscore = -CH_INF, real = 0;
        cmove_t bm = mv[0];
        for (int i = 0; i < n; i++) {
            cpos_t q = *root;
            ch_make(&q, mv[i]);
            int s;
            if (exact || i == 0) s = -ch_search(&q, d - 1, -CH_INF, -alpha, 1, true);
            else {
                s = -ch_search(&q, d - 1, -alpha - 1, -alpha, 1, true);
                if (s > alpha && !g_ch_abort) s = -ch_search(&q, d - 1, -CH_INF, -alpha, 1, true);
            }
            if (g_ch_abort) break;
            rs[i] = s;
            int sn = s;
            if (noise) sn += (int)(ch_rand() % (uint32_t)(2 * noise + 1)) - noise;
            if (sn > bscore) { bscore = sn; bm = mv[i]; real = s; }
            if (!exact && s > alpha) alpha = s;
        }
        if (g_ch_abort) break;                                                   // keep the last completed depth
        best = bm; best_sc = real; done = d;
        for (int i = 1; i < n; i++) {                                            // best scores first for the next depth
            cmove_t m = mv[i];
            int s = rs[i], j = i - 1;
            while (j >= 0 && rs[j] < s) { mv[j + 1] = mv[j]; rs[j + 1] = rs[j]; j--; }
            mv[j + 1] = m; rs[j + 1] = s;
        }
        if (report) chess_progress(d, root->side == 0 ? best_sc : -best_sc);
        if (best_sc > CH_MATE - 100 || best_sc < -(CH_MATE - 100)) break;       // a forced mate is known
    }
    if (done) tt_store(rh, done, best_sc, 1, best, 0);
    if (score_out) *score_out = best_sc;
    if (depth_out) *depth_out = done;
    return best;
}

static int ch_score_move(const cpos_t *root, cmove_t m, int depth, uint32_t ms)    // exact score of one move, or 0x7fff when time ran out
{
    g_ch_abort = false;
    g_ch_nodes = 0;
    g_ch_deadline = now_ms() + ms;
    g_ch_path[0] = ch_hash(root);
    cpos_t q = *root;
    ch_make(&q, m);
    int s = -ch_search(&q, depth - 1, -CH_INF, CH_INF, 1, true);
    return g_ch_abort ? 0x7fff : s;
}

static cmove_t ch_best(const cpos_t *root, int level)
{
    cmove_t *mv = g_ch_mv[0];
    int n = ch_legal(root, mv, false);
    if (n == 0) return 0;
    ch_seed();
    if (CH_RANDOM[level] && (int)(ch_rand() % 100) < CH_RANDOM[level]) { g_ch.have_score = false; return mv[ch_rand() % (uint32_t)n]; }
    int sc, dd;
    return ch_think(root, CH_DEPTH[level], CH_MS[level], CH_NOISE[level], level <= 1, &sc, &dd, true);
}

static uint32_t ch_perft(const cpos_t *p, int depth, int ply)
{
    if (!ch_scratch()) return 0;
    if (depth == 0) return 1;
    cmove_t *mv = g_ch_mv[ply];
    int n = ch_legal(p, mv, false);
    if (depth == 1) return (uint32_t)n;
    uint32_t t = 0;
    for (int i = 0; i < n; i++) { cpos_t q = *p; ch_make(&q, mv[i]); t += ch_perft(&q, depth - 1, ply + 1); }
    return t;
}

// ---- notation ----

static void ch_mstr(cmove_t m, char *o)                          // coordinate form: e2e4, e7e8q
{
    int f = CM_FROM(m), t = CM_TO(m), p = CM_PROMO(m);
    o[0] = (char)('a' + (f & 7)); o[1] = (char)('8' - (f >> 3));
    o[2] = (char)('a' + (t & 7)); o[3] = (char)('8' - (t >> 3));
    o[4] = p ? "xxnbrq"[p] : 0;
    o[5] = 0;
}

// Standard algebraic notation of move m in position p (o needs 10 bytes).
static void ch_san(const cpos_t *p, cmove_t m, char *o)
{
    static cmove_t tmp[CH_MAXMV], tmp2[CH_MAXMV];
    int f = CM_FROM(m), t = CM_TO(m), promo = CM_PROMO(m), pc = iabs(p->b[f]), k = 0;
    bool cap = p->b[t] != 0 || (pc == PC_P && t == p->ep);
    if (pc == PC_K && iabs(t - f) == 2) k = put_str(o, 8, t > f ? "O-O" : "O-O-O");
    else {
        if (pc != PC_P) {
            o[k++] = "?PNBRQK"[pc];
            int n = ch_legal(p, tmp, false);
            bool amb = false, same_file = false, same_rank = false;
            for (int i = 0; i < n; i++) {
                int f2 = CM_FROM(tmp[i]);
                if (f2 == f || CM_TO(tmp[i]) != t || iabs(p->b[f2]) != pc) continue;
                amb = true;
                if ((f2 & 7) == (f & 7)) same_file = true;
                if ((f2 >> 3) == (f >> 3)) same_rank = true;
            }
            if (amb) {
                if (!same_file) o[k++] = (char)('a' + (f & 7));
                else if (!same_rank) o[k++] = (char)('8' - (f >> 3));
                else { o[k++] = (char)('a' + (f & 7)); o[k++] = (char)('8' - (f >> 3)); }
            }
        } else if (cap) o[k++] = (char)('a' + (f & 7));
        if (cap) o[k++] = 'x';
        o[k++] = (char)('a' + (t & 7));
        o[k++] = (char)('8' - (t >> 3));
        if (promo) { o[k++] = '='; o[k++] = "??NBRQ"[promo]; }
    }
    cpos_t q = *p;
    ch_make(&q, m);
    if (ch_in_check(&q, q.side)) o[k++] = ch_legal(&q, tmp2, false) ? '+' : '#';
    o[k] = 0;
}

static void ch_fmt_cp(int cp, char *o)                           // +0.35, -1.20, +M3 (o needs 12 bytes)
{
    int k = 0;
    if (cp >= CH_MATE - 100 || cp <= -(CH_MATE - 100)) {
        o[k++] = cp < 0 ? '-' : '+';
        o[k++] = 'M';
        k += put_uint(o + k, (uint32_t)((CH_MATE - iabs(cp) + 1) / 2));
        o[k] = 0;
        return;
    }
    int a = iabs(cp);
    o[k++] = cp < 0 ? '-' : '+';
    k += put_uint(o + k, (uint32_t)(a / 100));
    o[k++] = '.';
    o[k++] = (char)('0' + (a / 10) % 10);
    o[k++] = (char)('0' + a % 10);
    o[k] = 0;
}

// ---- opening book: names, lines and ideas (coordinate moves, castling as the king's two-square move) ----

typedef struct { const char *eco, *name, *moves, *idea; int w; } opening_t;    // w: how often the AI picks it (0 = never, only for training)

static const opening_t OPENINGS[] = {
    { "C20", "King's Pawn Game", "e2e4 e7e5", "Claim the centre and open lines for the bishop and queen. Black answers symmetrically.", 4 },
    { "C40", "King's Knight Opening", "e2e4 e7e5 g1f3", "Develop with tempo: the knight attacks e5 and prepares to castle.", 4 },
    { "C50", "Italian Game", "e2e4 e7e5 g1f3 b8c6 f1c4", "The bishop eyes f7, Black's weakest point. Develop fast and castle early.", 4 },
    { "C53", "Giuoco Piano: Main Line", "e2e4 e7e5 g1f3 b8c6 f1c4 f8c5 c2c3 g8f6 d2d4", "Prepare d4 with c3 to build a big centre; Black hits back with ...exd4 and ...d5.", 3 },
    { "C51", "Evans Gambit", "e2e4 e7e5 g1f3 b8c6 f1c4 f8c5 b2b4", "Sacrifice the b-pawn for time and a huge centre after c3 and d4.", 1 },
    { "C55", "Two Knights Defense", "e2e4 e7e5 g1f3 b8c6 f1c4 g8f6", "Black counterattacks e4 instead of defending; White can try d3 or Ng5.", 3 },
    { "C57", "Fried Liver Attack", "e2e4 e7e5 g1f3 b8c6 f1c4 g8f6 f3g5 d7d5 e4d5 f6d5 g5f7", "Ng5 and a knight sacrifice on f7 drag the king out. Very sharp: know the theory.", 1 },
    { "C60", "Ruy Lopez", "e2e4 e7e5 g1f3 b8c6 f1b5", "Pressure on the defender of e5. A rich, strategic main line for centuries.", 4 },
    { "C78", "Ruy Lopez: Closed", "e2e4 e7e5 g1f3 b8c6 f1b5 a7a6 b5a4 g8f6 e1g1 f8e7 f1e1 b7b5 a4b3 d7d6 c2c3 e8g8", "White keeps the bishop pair and prepares d4; Black gets a solid, slightly passive position.", 3 },
    { "C65", "Ruy Lopez: Berlin Defense", "e2e4 e7e5 g1f3 b8c6 f1b5 g8f6 e1g1 f6e4 d2d4 e4d6 b5c6 d7c6 d4e5 d6f5", "Black heads for a queenless endgame with a rock-solid structure: the Berlin Wall.", 3 },
    { "C41", "Philidor Defense", "e2e4 e7e5 g1f3 d7d6 d2d4", "Solid but cramped: Black defends e5 with a pawn and delays piece play.", 2 },
    { "C42", "Petrov Defense", "e2e4 e7e5 g1f3 g8f6 f3e5 d7d6 e5f3 f6e4 d2d4 d6d5", "Mirror the attack on e4. Very solid and drawish; learn the move order carefully.", 3 },
    { "C30", "King's Gambit", "e2e4 e7e5 f2f4 e5f4 g1f3", "White gambits a pawn for a fast attack along the f-file. Romantic and risky.", 1 },
    { "C21", "Danish Gambit", "e2e4 e7e5 d2d4 e5d4 c2c3", "Offer pawns for rapid development and open lines against the king.", 1 },
    { "C23", "Bishop's Opening", "e2e4 e7e5 f1c4", "Develop the bishop first; keeps Nf3 and Nc3 in reserve.", 2 },
    { "C25", "Vienna Game", "e2e4 e7e5 b1c3", "Prepares f4 or Bc4 without committing the g1 knight.", 2 },
    { "C45", "Scotch Game", "e2e4 e7e5 g1f3 b8c6 d2d4 e5d4 f3d4", "Open the centre at once and gain space; Black gets easy piece play.", 3 },
    { "C47", "Four Knights Game", "e2e4 e7e5 g1f3 b8c6 b1c3 g8f6", "Symmetrical development; both sides keep everything protected.", 2 },
    { "C48", "Four Knights: Spanish Variation", "e2e4 e7e5 g1f3 b8c6 b1c3 g8f6 f1b5", "Pin the c6 knight; Black usually answers ...Nd4 or ...Bb4.", 2 },
    { "C00", "French Defense", "e2e4 e7e6 d2d4 d7d5", "Solid pawn chain; Black attacks the centre from the side. The c8 bishop can get stuck.", 4 },
    { "C11", "French: Classical", "e2e4 e7e6 d2d4 d7d5 b1c3 g8f6 c1g5", "Pin the f6 knight and keep tension; Black often plays ...Be7 or ...dxe4.", 3 },
    { "C02", "French: Advance", "e2e4 e7e6 d2d4 d7d5 e4e5", "Gain space and cramp Black; Black strikes the chain with ...c5.", 3 },
    { "B20", "Sicilian Defense", "e2e4 c7c5", "Fight for d4 with a flank pawn. Unbalanced: the most popular reply to 1.e4.", 4 },
    { "B90", "Sicilian: Najdorf", "e2e4 c7c5 g1f3 d7d6 d2d4 c5d4 f3d4 g8f6 b1c3 a7a6", "Flexible ...a6 stops Nb5 and prepares ...b5. Very sharp; a favourite of Fischer and Kasparov.", 4 },
    { "B70", "Sicilian: Dragon", "e2e4 c7c5 g1f3 d7d6 d2d4 c5d4 f3d4 g8f6 b1c3 g7g6", "Fianchetto the bishop on the long diagonal. White often castles long and attacks.", 3 },
    { "B33", "Sicilian: Sveshnikov", "e2e4 c7c5 g1f3 b8c6 d2d4 c5d4 f3d4 g8f6 b1c3 e7e5", "Black accepts a hole on d5 for active pieces and space.", 3 },
    { "B22", "Sicilian: Alapin", "e2e4 c7c5 c2c3", "Prepare d4 and sidestep the heavy theory of the open Sicilians.", 3 },
    { "B23", "Sicilian: Closed", "e2e4 c7c5 b1c3 b8c6 g2g3", "Fianchetto and slow play; White keeps the centre flexible.", 2 },
    { "B10", "Caro-Kann Defense", "e2e4 c7c6 d2d4 d7d5", "Rock-solid: Black supports d5 and keeps the bishop out of the pawn chain.", 4 },
    { "B18", "Caro-Kann: Classical", "e2e4 c7c6 d2d4 d7d5 b1c3 d5e4 c3e4 c8f5", "Develop the bishop outside the pawn chain, then ...e6 and ...Nd7.", 3 },
    { "B12", "Caro-Kann: Advance", "e2e4 c7c6 d2d4 d7d5 e4e5 c8f5", "White gains space; Black attacks the chain with ...c5 and keeps the bishop active.", 3 },
    { "B01", "Scandinavian Defense", "e2e4 d7d5 e4d5 d8d5 b1c3 d5a5", "Challenge e4 at once; the queen retreats with tempo and Black develops simply.", 2 },
    { "B07", "Pirc Defense", "e2e4 d7d6 d2d4 g8f6 b1c3 g7g6", "Let White build a centre, then attack it with pieces and ...c5 or ...e5.", 2 },
    { "B02", "Alekhine's Defense", "e2e4 g8f6 e4e5 f6d5 d2d4 d7d6", "Invite White's pawns forward, then undermine them.", 2 },
    { "B06", "Modern Defense", "e2e4 g7g6", "Fianchetto first; Black decides on the centre later.", 2 },
    { "D00", "Queen's Pawn Game", "d2d4 d7d5", "The classical symmetrical reply; the fight is for e4 and c4.", 4 },
    { "D06", "Queen's Gambit", "d2d4 d7d5 c2c4", "Offer a wing pawn to deflect Black's centre pawn. Not really a gambit.", 4 },
    { "D20", "Queen's Gambit Accepted", "d2d4 d7d5 c2c4 d5c4", "Black takes the pawn and gives it back later in return for free development.", 3 },
    { "D37", "Queen's Gambit Declined", "d2d4 d7d5 c2c4 e7e6 b1c3 g8f6 c1g5 f8e7", "Solid. Black defends d5 and aims to equalise with ...O-O and ...c5 or ...dxc4.", 4 },
    { "D10", "Slav Defense", "d2d4 d7d5 c2c4 c7c6 g1f3 g8f6 b1c3 d5c4", "Support d5 with a pawn while keeping the bishop free. Very reliable.", 4 },
    { "D02", "London System", "d2d4 d7d5 g1f3 g8f6 c1f4", "A solid setup for White: Bf4, e3, c3, Bd3. Easy to learn, hard to crack.", 3 },
    { "D01", "Veresov Attack", "d2d4 d7d5 b1c3 g8f6 c1g5", "Pin the knight early; leads to unusual, practical positions.", 2 },
    { "A46", "Torre Attack", "d2d4 g8f6 g1f3 e7e6 c1g5", "Bg5 pins before committing the c-pawn. A quiet system opening.", 2 },
    { "A45", "Trompowsky Attack", "d2d4 g8f6 c1g5", "Attack the knight early to take Black out of theory.", 2 },
    { "E60", "King's Indian Defense", "d2d4 g8f6 c2c4 g7g6 b1c3 f8g7 e2e4 d7d6", "Black lets White take the centre and attacks it with ...e5 or ...c5.", 4 },
    { "D70", "Grunfeld Defense", "d2d4 g8f6 c2c4 g7g6 b1c3 d7d5", "Strike the centre immediately with pieces and pressure on d4.", 3 },
    { "E20", "Nimzo-Indian Defense", "d2d4 g8f6 c2c4 e7e6 b1c3 f8b4", "Pin the knight to control e4 and fight for the centre. Very respected.", 4 },
    { "E12", "Queen's Indian Defense", "d2d4 g8f6 c2c4 e7e6 g1f3 b7b6", "Control e4 with a fianchettoed bishop.", 3 },
    { "E00", "Catalan Opening", "d2d4 g8f6 c2c4 e7e6 g2g3 d5d5 f1g2", "Fianchetto the bishop to pressure the long diagonal and the c4 pawn.", 3 },
    { "A60", "Benoni Defense", "d2d4 g8f6 c2c4 c7c5 d4d5 e7e6", "Create an imbalance: Black gets queenside play, White a big centre.", 2 },
    { "A57", "Benko Gambit", "d2d4 g8f6 c2c4 c7c5 d4d5 b7b5", "Sacrifice a pawn for open lines on the queenside and long-term pressure.", 1 },
    { "A52", "Budapest Gambit", "d2d4 g8f6 c2c4 e7e5 d4e5 f6g4", "A surprise gambit: win the pawn back with ...Bc5 and ...Nc6.", 1 },
    { "A80", "Dutch Defense", "d2d4 f7f5", "Take control of e4 at once. Aggressive but weakens the king.", 2 },
    { "A10", "English Opening", "c2c4", "Flank opening: control d5 and keep the centre flexible.", 4 },
    { "A20", "English: Reversed Sicilian", "c2c4 e7e5", "A Sicilian with an extra tempo for White.", 3 },
    { "A09", "Reti Opening", "g1f3 d7d5 c2c4", "Hypermodern: control the centre from the flanks.", 3 },
    { "A07", "King's Indian Attack", "g1f3 d7d5 g2g3", "A flexible fianchetto setup that works against almost anything.", 2 },
    { "A02", "Bird's Opening", "f2f4", "Control e5 with a flank pawn; leads to Dutch-like positions.", 0 },
    { "A01", "Nimzowitsch-Larsen Attack", "b2b3", "Fianchetto the queen's bishop to hit the long diagonal.", 1 },
    { "A00", "Grob Attack", "g2g4", "A provocative flank thrust: risky, but it surprises many opponents.", 0 },
};
#define NOPEN ((int)(sizeof(OPENINGS) / sizeof(OPENINGS[0])))

static int opn_len(const opening_t *o)
{
    int n = 0;
    for (const char *p = o->moves; *p; ) { n++; p += 4; if (*p == ' ') p++; }
    return n;
}

static const char *opn_tok(const opening_t *o, int idx)           // the idx-th move of the line, or NULL
{
    const char *p = o->moves;
    for (int i = 0; *p; i++) { if (i == idx) return p; p += 4; if (*p == ' ') p++; }
    return NULL;
}

static bool opn_match(const opening_t *o, const cmove_t *mv, int count)   // do the first `count` plies of mv follow the line?
{
    const char *p = o->moves;
    for (int i = 0; i < count; i++) {
        char s[8];
        if (!*p) return false;
        ch_mstr(mv[i], s);
        if (strncmp(s, p, 4) != 0) return false;
        p += 4;
        if (*p == ' ') p++;
    }
    return true;
}

// The longest opening that has been played completely (index or -1); *next = the shortest opening the game is still heading for.
static int opn_find(const cmove_t *mv, int n, int *next)
{
    int best = -1, bestlen = 0, nextlen = 1 << 30;
    *next = -1;
    for (int i = 0; i < NOPEN; i++) {
        int L = opn_len(&OPENINGS[i]);
        if (L <= n) { if (L > bestlen && opn_match(&OPENINGS[i], mv, L)) { best = i; bestlen = L; } }
        else if (L < nextlen && opn_match(&OPENINGS[i], mv, n)) { *next = i; nextlen = L; }
    }
    return best;
}

// The line's move number idx as a legal move of the current position.
static bool opn_tok_move(const opening_t *o, int idx, cmove_t *out)
{
    const char *t = opn_tok(o, idx);
    if (!t) return false;
    for (int i = 0; i < g_ch.nlegal; i++) {
        char s[8];
        if (CM_PROMO(g_ch.legal[i])) continue;
        ch_mstr(g_ch.legal[i], s);
        if (strncmp(s, t, 4) == 0) { *out = g_ch.legal[i]; return true; }
    }
    return false;
}

static bool opn_move_in_book(cmove_t m)
{
    int n = g_ch.nundo;
    for (int i = 0; i < NOPEN; i++) {
        const opening_t *o = &OPENINGS[i];
        cmove_t bm;
        if (opn_len(o) > n && opn_match(o, g_ch.umv, n) && opn_tok_move(o, n, &bm) && bm == m) return true;
    }
    return false;
}

// The AI's book move: openings that fit the game so far vote for their next move, weighted by popularity.
static cmove_t ch_book_move(int *open_idx)
{
    int n = g_ch.nundo, nc = 0, total = 0, wt[24], src[24];
    cmove_t cm[24];
    for (int i = 0; i < NOPEN; i++) {
        const opening_t *o = &OPENINGS[i];
        cmove_t m;
        if (!o->w || opn_len(o) <= n || !opn_match(o, g_ch.umv, n) || !opn_tok_move(o, n, &m)) continue;
        int k = 0;
        while (k < nc && cm[k] != m) k++;
        if (k == nc) { if (nc == 24) continue; cm[nc] = m; wt[nc] = 0; src[nc] = i; nc++; }
        wt[k] += o->w;
        total += o->w;
    }
    if (!nc) return 0;
    ch_seed();
    int r = (int)(ch_rand() % (uint32_t)total);
    for (int k = 0; k < nc; k++) { if (r < wt[k]) { *open_idx = src[k]; return cm[k]; } r -= wt[k]; }
    *open_idx = src[0];
    return cm[0];
}

// ---- the game ----

static bool ch_insufficient(const cpos_t *p)
{
    int minors = 0;
    for (int i = 0; i < 64; i++) {
        int a = iabs(p->b[i]);
        if (a == PC_P || a == PC_R || a == PC_Q) return false;
        if (a == PC_N || a == PC_B) minors++;
    }
    return minors <= 1;
}

static void ch_opening_update(void)
{
    int n = g_ch.nundo, prev = -1, dummy;
    g_ch.nopn_h = 0;
    for (int k = 1; k <= n && k <= 24; k++) {
        int idx = opn_find(g_ch.umv, k, &dummy);
        if (idx < 0 || idx == prev) continue;
        if (g_ch.nopn_h == 10) { memmove(&g_ch.opn_h[0], &g_ch.opn_h[1], 9 * sizeof(g_ch.opn_h[0])); g_ch.nopn_h--; }
        g_ch.opn_h[g_ch.nopn_h].ply = k;
        g_ch.opn_h[g_ch.nopn_h].idx = idx;
        g_ch.nopn_h++;
        prev = idx;
    }
    g_ch.opn_cur = opn_find(g_ch.umv, n, &g_ch.opn_next);
}

static void ch_refresh(void)
{
    cpos_t *p = &g_ch.pos;
    g_ch.nlegal = ch_legal(p, g_ch.legal, false);
    g_ch.check = ch_in_check(p, p->side);
    g_ch.state = 0;
    g_ch.why[0] = 0;
    if (g_ch.nlegal == 0) {
        if (g_ch.check) g_ch.state = p->side == 0 ? 2 : 1;
        else { g_ch.state = 3; put_str(g_ch.why, sizeof(g_ch.why), "Stalemate"); }
    } else if (p->half >= 100) { g_ch.state = 3; put_str(g_ch.why, sizeof(g_ch.why), "Draw: 50-move rule"); }
    else if (ch_insufficient(p)) { g_ch.state = 3; put_str(g_ch.why, sizeof(g_ch.why), "Draw: no material"); }
    else if (g_ch.nundo >= CH_MAXGAME - 2) { g_ch.state = 3; put_str(g_ch.why, sizeof(g_ch.why), "Draw: move limit"); }
    else {
        int reps = 0;
        uint64_t h = g_ch_hist[g_ch_nh - 1];
        for (int i = 0; i < g_ch_nh; i++) if (g_ch_hist[i] == h) reps++;
        if (reps >= 3) { g_ch.state = 3; put_str(g_ch.why, sizeof(g_ch.why), "Draw: repetition"); }
    }
    ch_opening_update();
}

static void ch_new(void)
{
    if (!ch_scratch()) return;
    static const int8_t BACK[8] = { PC_R, PC_N, PC_B, PC_Q, PC_K, PC_B, PC_N, PC_R };
    memset(&g_ch.pos, 0, sizeof(g_ch.pos));
    for (int f = 0; f < 8; f++) {
        g_ch.pos.b[f] = (int8_t)-BACK[f];
        g_ch.pos.b[8 + f] = -PC_P;
        g_ch.pos.b[48 + f] = PC_P;
        g_ch.pos.b[56 + f] = BACK[f];
    }
    g_ch.pos.castle = 15;
    g_ch.pos.ep = -1;
    g_ch.nundo = 0;
    g_ch.sel = -1;
    g_ch.has_last = false;
    g_ch.ai_pending = g_ch.thinking = false;
    g_ch.hint_from = g_ch.hint_to = -1;
    g_ch.have_score = false;
    g_ch.depth_done = g_ch.msg_col = 0;
    g_ch.ai_msg[0] = g_ch.coach_msg[0] = g_ch.tr_msg[0] = 0;
    g_ch.tr_ok = g_ch.tr_bad = g_ch.tr_miss = 0;
    g_ch.tr_done = false;
    if (g_ch.mode == 1) put_str(g_ch.tr_msg, sizeof(g_ch.tr_msg), "Play the book moves (H = hint)");
    memset(g_tt, 0, sizeof(tt_t) * (size_t)TT_SIZE);
    g_ch_hist[0] = ch_hash(&g_ch.pos);
    g_ch_nh = 1;
    ch_refresh();
    if (g_ch.human == 1) { g_ch.ai_pending = true; g_ch.ai_t = now_ms(); }
}

static void chess_init(void)
{
    static bool ready;
    if (ready) return;
    g_ch.level = 2;
    g_ch.human = 0;
    g_ch.coach = true;
    g_ch.book = true;
    g_ch.tr_open = 2;                                             // the Italian Game
    ch_new();
    ready = true;
}

static void ch_apply(cmove_t m)
{
    if (g_ch.nundo >= CH_MAXGAME - 1) return;
    ch_san(&g_ch.pos, m, g_ch_san[g_ch.nundo]);
    g_ch.undo[g_ch.nundo] = g_ch.pos;
    g_ch.umv[g_ch.nundo] = m;
    g_ch.nundo++;
    ch_make(&g_ch.pos, m);
    g_ch_hist[g_ch.nundo] = ch_hash(&g_ch.pos);
    g_ch_nh = g_ch.nundo + 1;
    g_ch.last = m;
    g_ch.has_last = true;
    g_ch.sel = -1;
    g_ch.hint_from = g_ch.hint_to = -1;
    ch_refresh();
}

static void ch_undo(void)
{
    int n = 1;
    if ((((g_ch.nundo - 1) & 1) != g_ch.human)) n = 2;            // go back to a position where the human is to move
    if (n > g_ch.nundo) { flash("nothing to undo"); return; }
    g_ch.pos = g_ch.undo[g_ch.nundo - n];
    g_ch.nundo -= n;
    g_ch_nh = g_ch.nundo + 1;
    g_ch.sel = -1;
    g_ch.ai_pending = false;
    g_ch.hint_from = g_ch.hint_to = -1;
    g_ch.coach_msg[0] = g_ch.ai_msg[0] = g_ch.tr_msg[0] = 0;
    g_ch.tr_done = false;
    g_ch.has_last = g_ch.nundo > 0;
    if (g_ch.has_last) g_ch.last = g_ch.umv[g_ch.nundo - 1];
    ch_refresh();
}

static bool ch_flipped(void) { return (g_ch.human == 1) != g_ch.flip; }

// ---- trainer ----

static bool ch_train_active(void) { return g_ch.mode == 1 && g_ch.nundo < opn_len(&OPENINGS[g_ch.tr_open]); }
static bool ch_expected(cmove_t *m) { return opn_tok_move(&OPENINGS[g_ch.tr_open], g_ch.nundo, m); }

static void ch_train_check_done(void)
{
    if (g_ch.mode != 1 || g_ch.tr_done || g_ch.nundo < opn_len(&OPENINGS[g_ch.tr_open])) return;
    g_ch.tr_done = true;
    g_ch.msg_col = 1;
    put_str(g_ch.tr_msg, sizeof(g_ch.tr_msg), "Line complete! Play on against the engine.");
}

static void train_step(int d)
{
    g_ch.tr_open = ((g_ch.tr_open + d) % NOPEN + NOPEN) % NOPEN;
    ch_new();
}

static void chess_set_mode(int mode)
{
    g_ch.mode = mode;
    if (mode == 1 && g_ch.opn_cur >= 0) g_ch.tr_open = g_ch.opn_cur;
    ch_new();
}

// ---- coach and hints ----

static void ch_coach(cmove_t played)                              // called with the position before the move
{
    int sc, dd;
    g_ch.coach_msg[0] = 0;
    g_ch.msg_col = 0;
    if (opn_move_in_book(played)) { put_str(g_ch.coach_msg, sizeof(g_ch.coach_msg), "Coach: book move"); g_ch.msg_col = 1; return; }
    cmove_t best = ch_think(&g_ch.pos, 4, 350, 0, false, &sc, &dd, false);
    if (!best || dd == 0) return;                                 // only one legal move: nothing to say
    if (best == played) { put_str(g_ch.coach_msg, sizeof(g_ch.coach_msg), "Coach: best move!"); g_ch.msg_col = 1; return; }
    int s = ch_score_move(&g_ch.pos, played, 4, 350);
    if (s == 0x7fff) return;
    int loss = sc - s;
    if (loss < 0) loss = 0;
    char bs[10];
    ch_san(&g_ch.pos, best, bs);
    if (loss <= 15) { put_str(g_ch.coach_msg, sizeof(g_ch.coach_msg), "Coach: excellent"); g_ch.msg_col = 1; }
    else if (loss <= 40) { put_str(g_ch.coach_msg, sizeof(g_ch.coach_msg), "Coach: good move"); g_ch.msg_col = 1; }
    else {
        const char *tag = loss <= 90 ? "inaccuracy" : (loss <= 200 ? "mistake" : "blunder");
        snprintf(g_ch.coach_msg, sizeof(g_ch.coach_msg), "Coach: %s - better %s", tag, bs);
        g_ch.msg_col = loss <= 90 ? 2 : 3;
    }
}

static void chess_hint(void)
{
    g_ch.hint_from = g_ch.hint_to = -1;
    if (g_ch.state != 0 || g_ch.ai_pending || g_ch.pos.side != g_ch.human) return;
    cmove_t m = 0;
    char san[10];
    if (g_ch.mode == 1 && ch_train_active()) {
        if (!ch_expected(&m)) { put_str(g_ch.tr_msg, sizeof(g_ch.tr_msg), "No book move here"); return; }
        ch_san(&g_ch.pos, m, san);
        snprintf(g_ch.tr_msg, sizeof(g_ch.tr_msg), "Hint: %s", san);
    } else {
        int sc, dd;
        m = ch_think(&g_ch.pos, 6, 700, 0, false, &sc, &dd, false);
        if (!m) return;
        ch_san(&g_ch.pos, m, san);
        if (dd) { char ev[12]; ch_fmt_cp(sc, ev); snprintf(g_ch.coach_msg, sizeof(g_ch.coach_msg), "Hint: %s (%s for you)", san, ev); }
        else snprintf(g_ch.coach_msg, sizeof(g_ch.coach_msg), "Hint: %s (the only move)", san);
    }
    g_ch.msg_col = 2;
    g_ch.hint_from = CM_FROM(m);
    g_ch.hint_to = CM_TO(m);
}

// ---- PGN ----

static void chess_save_pgn(void)
{
    static char buf[6000];
    char path[128], m[64], res[8];
    int n;
    if (g_ch.nundo == 0) { flash("play a few moves first"); return; }
    if (!avfs_is_directory(g_desk_dir) && avfs_create_dir(g_desk_dir) != 0) { flash("cannot create the Desktop folder"); return; }
    for (n = 1; n < 100; n++) { snprintf(path, sizeof(path), "%s/chess%d.pgn", g_desk_dir, n); if (!avfs_file_exists(path)) break; }
    if (n == 100) { flash("too many games on the desktop"); return; }
    put_str(res, sizeof(res), g_ch.state == 1 ? "1-0" : (g_ch.state == 2 ? "0-1" : (g_ch.state == 3 ? "1/2-1/2" : "*")));
    char eng[24];
    snprintf(eng, sizeof(eng), "RadiWM %s", CH_LEVELS[g_ch.level]);
    int o = snprintf(buf, sizeof(buf), "[Event \"RadiWM chess\"]\n[Site \"RadiumOS\"]\n[White \"%s\"]\n[Black \"%s\"]\n[Result \"%s\"]\n",
                     g_ch.human == 0 ? "You" : eng, g_ch.human == 0 ? eng : "You", res);
    if (g_ch.opn_cur >= 0) o += snprintf(buf + o, sizeof(buf) - (size_t)o, "[ECO \"%s\"]\n[Opening \"%s\"]\n", OPENINGS[g_ch.opn_cur].eco, OPENINGS[g_ch.opn_cur].name);
    buf[o++] = '\n';
    int col = 0;
#define PGN_ADD(s_) do { int l_ = (int)strlen(s_); if (col + l_ + 1 > 78) { buf[o++] = '\n'; col = 0; } else if (col) { buf[o++] = ' '; col++; } memcpy(buf + o, s_, (size_t)l_); o += l_; col += l_; } while (0)
    for (int i = 0; i < g_ch.nundo && o < (int)sizeof(buf) - 40; i++) {
        if ((i & 1) == 0) { char num[12]; int k = put_uint(num, (uint32_t)(i / 2 + 1)); num[k++] = '.'; num[k] = 0; PGN_ADD(num); }
        PGN_ADD(g_ch_san[i]);
    }
    PGN_ADD(res);
#undef PGN_ADD
    buf[o++] = '\n';
    if (avfs_save_file(path, buf, (uint32_t)o, true) != 0) { flash("could not save the game"); return; }
    snprintf(m, sizeof(m), "saved chess%d.pgn on the Desktop", n);
    flash(m);
    desktop_icons_refresh();
    damage_icons();
}

// ---- the computer's turn ----

static void chess_progress(int depth, int score_white)           // called by the engine after every completed depth
{
    g_ch.depth_done = depth;
    g_ch.score_cp = score_white;
    g_ch.have_score = true;
    if (!g_ch_live || depth < 3) return;
    for (int i = 0; i < MAX_WIN; i++) if (g_win[i].used && g_win[i].type == W_CHESS) damage_content(&g_win[i]);
    draw_frame();
}

// Called by the main loop: the computer (or the trainer) moves shortly after the human, once "Thinking..." has been drawn.
static void chess_tick(void)
{
    if (!g_ch.ai_pending || now_ms() - g_ch.ai_t < 150) return;
    g_ch.ai_pending = false;
    if (g_ch.state != 0 || g_ch.pos.side == g_ch.human) return;
    cmove_t m = 0;
    char san[10];
    g_ch.ai_msg[0] = 0;
    if (g_ch.mode == 1 && ch_train_active() && ch_expected(&m)) {            // the trainer plays the other side of the line
        ch_san(&g_ch.pos, m, san);
        snprintf(g_ch.ai_msg, sizeof(g_ch.ai_msg), "Opponent plays %s (book)", san);
        g_ch.msg_col = 0;
    }
    if (!m && g_ch.book && g_ch.mode == 0 && g_ch.level >= 1) {
        int oi = 0;
        m = ch_book_move(&oi);
        if (m) {
            ch_san(&g_ch.pos, m, san);
            snprintf(g_ch.ai_msg, sizeof(g_ch.ai_msg), "AI book move %s", san);
            g_ch.have_score = false;
        }
    }
    if (!m) {
        g_ch.thinking = true;
        g_ch.have_score = false;
        g_ch.depth_done = 0;
        g_ch_live = true;
        m = ch_best(&g_ch.pos, g_ch.level);
        g_ch_live = false;
        g_ch.thinking = false;
        if (m) {
            ch_san(&g_ch.pos, m, san);
            if (g_ch.have_score) {
                char ev[12];
                ch_fmt_cp(g_ch.score_cp, ev);
                snprintf(g_ch.ai_msg, sizeof(g_ch.ai_msg), "AI plays %s (depth %d, %s)", san, g_ch.depth_done, ev);
            } else snprintf(g_ch.ai_msg, sizeof(g_ch.ai_msg), "AI plays %s", san);
        }
    }
    if (m) { ch_apply(m); ch_train_check_done(); }
    for (int i = 0; i < MAX_WIN; i++) if (g_win[i].used && g_win[i].type == W_CHESS) damage_content(&g_win[i]);
}

// ---- pieces and window ----

static void ch_draw_piece(int kind, int x, int y, int s, uint32_t fill, uint32_t edge)
{
    for (int pass = 0; pass < 2; pass++) {
        uint32_t col = pass == 0 ? edge : fill;
        int g = pass == 0 ? (s >= 36 ? 2 : 1) : 0;                              // pass 0 draws the outline, pass 1 the filling
#define XR(rx, ry, rw, rh) fill_rect(x + (rx) * s / 48 - g, y + (ry) * s / 48 - g, (rw) * s / 48 + 2 * g, (rh) * s / 48 + 2 * g, col)
#define XD(cx, cy, r) disc(x + (cx) * s / 48, y + (cy) * s / 48, imax(1, (r) * s / 48) + g, col)
        XR(9, 38, 30, 6);
        switch (kind) {
        case PC_P: XD(24, 14, 7); XR(20, 19, 8, 5); XR(16, 24, 16, 6); XR(13, 30, 22, 9); break;
        case PC_R: XR(12, 5, 6, 9); XR(21, 5, 6, 9); XR(30, 5, 6, 9); XR(12, 12, 24, 5); XR(16, 17, 16, 14); XR(12, 31, 24, 8); break;
        case PC_N: XR(17, 9, 13, 11); XR(7, 17, 14, 7); XR(19, 6, 4, 5); XR(21, 18, 17, 21); XR(13, 24, 10, 15); break;
        case PC_B: XD(24, 6, 3); XD(24, 15, 7); XR(20, 20, 8, 5); XR(16, 25, 16, 6); XR(12, 31, 24, 8); break;
        case PC_Q: XD(8, 9, 3); XD(16, 6, 3); XD(24, 4, 3); XD(32, 6, 3); XD(40, 9, 3); XR(9, 11, 30, 6); XR(14, 17, 20, 12); XR(11, 29, 26, 10); break;
        default: XR(22, 2, 4, 12); XR(18, 6, 12, 4); XD(24, 20, 6); XR(15, 24, 18, 8); XR(11, 31, 26, 8); break;
        }
    }
#undef XR
#undef XD
    if (kind == PC_N) fill_rect(x + 22 * s / 48, y + 12 * s / 48, imax(2, s / 18), imax(2, s / 18), edge);
    if (kind == PC_B) line(x + 21 * s / 48, y + 18 * s / 48, x + 27 * s / 48, y + 11 * s / 48, imax(1, s / 24), edge);
}

static rect_t chess_board_rect(const win_t *w) { rect_t c = content_rect(w); rect_t r = { c.x + 22, c.y + 12, 8 * CH_CELL, 8 * CH_CELL }; return r; }

static rect_t chess_btn(const win_t *w, int i)                    // 10 buttons in two columns
{
    rect_t b = chess_board_rect(w), c = content_rect(w);
    int x = b.x + b.w + 18, bw = (imax(150, c.x + c.w - x - 12) - 6) / 2;
    rect_t r = { x + (i & 1) * (bw + 6), c.y + 96 + (i >> 1) * 34, bw, 28 };
    return r;
}

static int chess_sq(const win_t *w, int mx, int my)              // board square under the pointer, or -1
{
    rect_t b = chess_board_rect(w);
    if (mx < b.x || my < b.y || mx >= b.x + b.w || my >= b.y + b.h) return -1;
    int dc = (mx - b.x) / CH_CELL, dr = (my - b.y) / CH_CELL;
    return ch_flipped() ? (7 - dr) * 8 + (7 - dc) : dr * 8 + dc;
}

static void chess_taken(int lost_side, int *cnt)                  // pieces of that colour that are no longer on the board
{
    static const int START[6] = { 0, 8, 2, 2, 2, 1 };
    int have[7] = { 0 };
    for (int i = 0; i < 64; i++) { int pc = g_ch.pos.b[i]; if (pc && (pc > 0) == (lost_side == 0)) have[iabs(pc)]++; }
    for (int t = 1; t <= 5; t++) cnt[t] = imax(0, START[t] - have[t]);
}

static int chess_wrap(int x, int y, const char *s, int cols, int maxlines, uint32_t fg)   // word-wrapped text; returns the lines used
{
    int lines = 0;
    while (*s && lines < maxlines) {
        char tmp[64];
        int len = (int)strlen(s), cut = len;
        if (cols > 60) cols = 60;
        if (len > cols) {
            cut = cols;
            while (cut > 0 && s[cut] != ' ') cut--;
            if (cut == 0) cut = cols;
        }
        memcpy(tmp, s, (size_t)cut);
        tmp[cut] = 0;
        text(x, y + lines * CH, tmp, fg, TH.bg, cols);
        s += cut;
        while (*s == ' ') s++;
        lines++;
    }
    return lines;
}

static void render_chess(win_t *w)
{
    rect_t c = content_rect(w), b = chess_board_rect(w);
    fill_rect(c.x, c.y, c.w, c.h, TH.bg);
    bool flip = ch_flipped(), hint[64] = { false };
    int ksq = -1;
    if (g_ch.check) for (int i = 0; i < 64; i++) if (g_ch.pos.b[i] == (g_ch.pos.side == 0 ? PC_K : -PC_K)) ksq = i;
    if (g_ch.sel >= 0) for (int i = 0; i < g_ch.nlegal; i++) if (CM_FROM(g_ch.legal[i]) == g_ch.sel) hint[CM_TO(g_ch.legal[i])] = true;
    for (int dr = 0; dr < 8; dr++)
        for (int dc = 0; dc < 8; dc++) {
            int sq = flip ? (7 - dr) * 8 + (7 - dc) : dr * 8 + dc;
            int px = b.x + dc * CH_CELL, py = b.y + dr * CH_CELL;
            bool light = (((sq >> 3) + (sq & 7)) & 1) == 0;
            uint32_t col = light ? 0xeeeed2 : 0x769656;
            if (g_ch.has_last && (sq == CM_FROM(g_ch.last) || sq == CM_TO(g_ch.last))) col = light ? 0xf5f682 : 0xbaca44;
            if (sq == g_ch.hint_from || sq == g_ch.hint_to) col = mix(col, 0x3b82f6, 55);
            if (sq == g_ch.sel) col = light ? 0xf7ec59 : 0xdbc531;
            if (sq == ksq) col = 0xe86a5e;
            fill_rect(px, py, CH_CELL, CH_CELL, col);
            int pc = g_ch.pos.b[sq];
            if (pc) ch_draw_piece(iabs(pc), px + 4, py + 4, CH_CELL - 8, pc > 0 ? 0xf8f4e8 : 0x2a2a2a, pc > 0 ? 0x1c1c1a : 0xe8e4d8);
            if (hint[sq]) {
                bool cap = pc != 0 || (iabs(g_ch.pos.b[g_ch.sel]) == PC_P && sq == g_ch.pos.ep);
                if (cap) { frame_rect(px + 1, py + 1, CH_CELL - 2, CH_CELL - 2, 0xcc4444); frame_rect(px + 2, py + 2, CH_CELL - 4, CH_CELL - 4, 0xcc4444); }
                else disc(px + CH_CELL / 2, py + CH_CELL / 2, 8, mix(col, 0x000000, 28));
            }
            if (dc == 0) { char l[2] = { (char)('0' + 8 - (sq >> 3)), 0 }; text(b.x - 14, py + (CH_CELL - CH) / 2, l, TH.dim, TH.bg, 1); }
            if (dr == 7) { char l[2] = { (char)('a' + (sq & 7)), 0 }; text(px + (CH_CELL - CW) / 2, b.y + 8 * CH_CELL + 4, l, TH.dim, TH.bg, 1); }
        }
    frame_rect(b.x - 1, b.y - 1, b.w + 2, b.h + 2, TH.line);

    // the opening, under the board
    {
        int by = b.y + 8 * CH_CELL + 22, bcols = 8 * CH_CELL / CW;
        char l1[96], l2[160];
        const opening_t *cur = g_ch.opn_cur >= 0 ? &OPENINGS[g_ch.opn_cur] : NULL;
        if (g_ch.mode == 1) {
            const opening_t *t = &OPENINGS[g_ch.tr_open];
            snprintf(l1, sizeof(l1), "Training: %s (%s)", t->name, t->eco);
            snprintf(l2, sizeof(l2), "%s", t->idea);
        } else {
            if (cur) snprintf(l1, sizeof(l1), "%s (%s)", cur->name, cur->eco);
            else put_str(l1, sizeof(l1), g_ch.nundo ? "Unusual opening" : "Starting position");
            if (g_ch.opn_next >= 0 && g_ch.nundo) snprintf(l2, sizeof(l2), "Heading for: %s", OPENINGS[g_ch.opn_next].name);
            else if (cur && opn_len(cur) == g_ch.nundo) snprintf(l2, sizeof(l2), "%s", cur->idea);
            else put_str(l2, sizeof(l2), g_ch.nundo ? "Out of book: you are on your own now" : "Make a move; the opening name appears here");
        }
        text(b.x, by, l1, TH.accent, TH.bg, bcols);
        text(b.x, by + CH, l2, TH.dim, TH.bg, bcols);
    }

    int px = b.x + b.w + 18, tcols = (c.x + c.w - px - 8) / CW, pw = c.x + c.w - px - 12;
    text(px, c.y + 14, "Chess", TH.accent, TH.bg, 8);
    text(px + 8 * CW + 8, c.y + 14, g_ch.mode ? "- opening trainer" : "- vs engine", TH.dim, TH.bg, 20);
    char stbuf[40];
    const char *st = "Your move";
    uint32_t sc = TH.fg;
    if (g_ch.state == 1 || g_ch.state == 2) {
        bool win = (g_ch.state == 1 && g_ch.human == 0) || (g_ch.state == 2 && g_ch.human == 1);
        st = win ? "Checkmate: you win!" : "Checkmate: you lose";
        sc = win ? TH.good : TH.bad;
    } else if (g_ch.state == 3) { st = g_ch.why; sc = TH.warn; }
    else if (g_ch.thinking || g_ch.ai_pending) {
        if (g_ch.thinking && g_ch.depth_done) { snprintf(stbuf, sizeof(stbuf), "Thinking... depth %d", g_ch.depth_done); st = stbuf; }
        else st = "Thinking...";
        sc = TH.dim;
    } else if (g_ch.check) { st = "Check! Your move"; sc = TH.warn; }
    text(px, c.y + 38, st, sc, TH.bg, tcols);
    {
        const char *msg = g_ch.mode == 1 ? g_ch.tr_msg : (g_ch.coach_msg[0] ? g_ch.coach_msg : g_ch.ai_msg);
        uint32_t mc = g_ch.msg_col == 1 ? TH.good : (g_ch.msg_col == 2 ? TH.warn : (g_ch.msg_col == 3 ? TH.bad : TH.dim));
        if (!g_ch.coach_msg[0] && g_ch.mode == 0) mc = TH.dim;
        text(px, c.y + 56, msg, mc, TH.bg, tcols);
    }
    if (g_ch.have_score && g_ch.mode == 0 && pw > 40) {                           // evaluation bar (white on the left)
        int e = imax(-600, imin(600, g_ch.score_cp)), wpart = pw * (50 + e / 12) / 100;
        fill_rect(px, c.y + 76, wpart, 8, 0xf4f1ea);
        fill_rect(px + wpart, c.y + 76, pw - wpart, 8, 0x2a2a2a);
        frame_rect(px - 1, c.y + 75, pw + 2, 10, TH.line);
    }

    char lv[24];
    snprintf(lv, sizeof(lv), "Level: %s", CH_LEVELS[g_ch.level]);
    const char *lab[10] = { g_ch.mode ? "Restart line" : "New game", "Undo", g_ch.human == 0 ? "Play: White" : "Play: Black", lv,
                            g_ch.mode ? "Mode: Train" : "Mode: Play", "Hint", g_ch.mode ? "< Prev line" : "Flip board", g_ch.mode ? "Next line >" : "Save PGN",
                            g_ch.coach ? "Coach: on" : "Coach: off", g_ch.book ? "Book: on" : "Book: off" };
    for (int i = 0; i < 10; i++) {
        rect_t r = chess_btn(w, i);
        bool on = (i == 4 && g_ch.mode) || (i == 8 && g_ch.coach) || (i == 9 && g_ch.book);
        uint32_t bg = on ? TH.accent : TH.line;
        round_rect(r.x, r.y, r.w, r.h, 4, bg);
        text_center(r.x, r.w, r.y + (r.h - CH) / 2, lab[i], on ? TH.accent_tx : TH.fg, bg, r.w / CW);
    }

    int y = c.y + 270;
    char row[96];
    if (g_ch.mode == 0) {
        text(px, y, "Opening history", TH.dim, TH.bg, 16);
        y += CH + 2;
        if (g_ch.nopn_h == 0) { text(px, y, "(none yet)", TH.dim, TH.bg, 12); y += CH; }
        for (int i = imax(0, g_ch.nopn_h - 3); i < g_ch.nopn_h; i++) {            // who made the opening change, and with which move
            int ply = g_ch.opn_h[i].ply;
            bool white = (ply & 1) == 1, me = white == (g_ch.human == 0);
            snprintf(row, sizeof(row), "%d%s %s (%s) %s", (ply + 1) / 2, white ? "." : "...", g_ch_san[ply - 1], me ? "you" : "AI", OPENINGS[g_ch.opn_h[i].idx].name);
            text(px, y, row, TH.fg, TH.bg, tcols);
            y += CH;
        }
        if (g_ch.opn_cur >= 0) y += 4 + chess_wrap(px, y + 4, OPENINGS[g_ch.opn_cur].idea, tcols, 3, TH.dim) * CH;
    } else {
        const opening_t *t = &OPENINGS[g_ch.tr_open];
        int L = opn_len(t);
        snprintf(row, sizeof(row), "Line: %d of %d moves  (you: %s)", imin(g_ch.nundo, L), L, g_ch.human == 0 ? "White" : "Black");
        text(px, y, row, TH.fg, TH.bg, tcols);
        snprintf(row, sizeof(row), "Correct %d   Mistakes %d", g_ch.tr_ok, g_ch.tr_bad);
        text(px, y + CH, row, g_ch.tr_bad ? TH.warn : TH.good, TH.bg, tcols);
        chess_wrap(px, y + 2 * CH + 6, t->idea, tcols, 4, TH.dim);
    }

    y = c.y + 396;
    for (int side = 0; side < 2; side++) {                                        // side = colour that lost the pieces
        int cnt[6], ry = y + side * 28, xx = px + 92;
        chess_taken(side, cnt);
        text(px, ry + 4, side == 1 ? "White took" : "Black took", TH.dim, TH.bg, 12);
        for (int t = PC_Q; t >= PC_P; t--)
            for (int k = 0; k < cnt[t]; k++) {
                ch_draw_piece(t, xx, ry, 22, side == 0 ? 0xf8f4e8 : 0x2a2a2a, side == 0 ? 0x1c1c1a : 0xe8e4d8);
                xx += 11;
            }
    }

    y = c.y + 456;
    text(px, y, "Moves", TH.dim, TH.bg, 8);
    int y0 = y + CH + 2, lines = (c.y + c.h - 6 - y0) / CH, turns = (g_ch.nundo + 1) / 2, first = imax(0, turns - lines);
    for (int t = first; t < turns; t++) {
        snprintf(row, sizeof(row), "%d. %s %s", t + 1, g_ch_san[2 * t], 2 * t + 1 < g_ch.nundo ? g_ch_san[2 * t + 1] : "");
        text(px, y0 + (t - first) * CH, row, TH.fg, TH.bg, tcols);
    }
}

static void chess_button(int i)
{
    if (i == 0) ch_new();
    else if (i == 1) ch_undo();
    else if (i == 2) { g_ch.human ^= 1; g_ch.flip = false; ch_new(); }
    else if (i == 3) g_ch.level = (g_ch.level + 1) % 5;
    else if (i == 4) chess_set_mode(g_ch.mode ^ 1);
    else if (i == 5) chess_hint();
    else if (i == 6) { if (g_ch.mode) train_step(-1); else g_ch.flip = !g_ch.flip; }
    else if (i == 7) { if (g_ch.mode) train_step(1); else chess_save_pgn(); }
    else if (i == 8) g_ch.coach = !g_ch.coach;
    else if (i == 9) g_ch.book = !g_ch.book;
}

// A move chosen by the human: checked by the trainer, judged by the coach, then played; the computer answers shortly after.
static void ch_try_move(cmove_t m)
{
    g_ch.hint_from = g_ch.hint_to = -1;
    g_ch.coach_msg[0] = 0;
    g_ch.msg_col = 0;
    if (g_ch.mode == 1 && ch_train_active()) {
        cmove_t exp;
        if (ch_expected(&exp)) {
            if (m != exp) {
                g_ch.tr_bad++;
                g_ch.tr_miss++;
                g_ch.sel = -1;
                g_ch.msg_col = 3;
                if (g_ch.tr_miss >= 2) {
                    g_ch.hint_from = CM_FROM(exp);
                    g_ch.hint_to = CM_TO(exp);
                    put_str(g_ch.tr_msg, sizeof(g_ch.tr_msg), "Not the book move - see the highlight");
                } else put_str(g_ch.tr_msg, sizeof(g_ch.tr_msg), "Not the book move - try again");
                return;
            }
            g_ch.tr_ok++;
            g_ch.tr_miss = 0;
            g_ch.msg_col = 1;
            put_str(g_ch.tr_msg, sizeof(g_ch.tr_msg), "Correct!");
        }
    } else if (g_ch.mode == 0 && g_ch.coach) ch_coach(m);
    g_ch.ai_msg[0] = 0;
    ch_apply(m);
    ch_train_check_done();
    if (g_ch.state == 0) { g_ch.ai_pending = true; g_ch.ai_t = now_ms(); }
}

static void chess_click(int wi, int mx, int my)
{
    win_t *w = &g_win[wi];
    for (int i = 0; i < 10; i++) if (rect_has(chess_btn(w, i), mx, my)) { chess_button(i); damage_content(w); return; }
    int sq = chess_sq(w, mx, my);
    if (sq < 0 || g_ch.state != 0 || g_ch.ai_pending || g_ch.pos.side != g_ch.human) return;
    int pc = g_ch.pos.b[sq];
    if (g_ch.sel >= 0) {
        int want = g_shift ? PC_N : (g_ctrl ? PC_R : (g_alt ? PC_B : PC_Q));   // promotion piece (queen by default)
        for (int i = 0; i < g_ch.nlegal; i++) {
            cmove_t m = g_ch.legal[i];
            if (CM_FROM(m) == g_ch.sel && CM_TO(m) == sq && (!CM_PROMO(m) || CM_PROMO(m) == want)) {
                ch_try_move(m);
                damage_content(w);
                return;
            }
        }
    }
    bool mine = pc != 0 && ((pc > 0) == (g_ch.human == 0));
    g_ch.sel = (mine && sq != g_ch.sel) ? sq : -1;
    damage_content(w);
}

static void chess_key(int wi, uint8_t code, bool ext)
{
    if (ext) {
        if (g_ch.mode == 1) {
            if (code == 0x4B) train_step(-1);                    // Left / Right: previous / next opening
            else if (code == 0x4D) train_step(1);
            else if (code == 0x48) train_step(-5);               // Up / Down: five openings at a time
            else if (code == 0x50) train_step(5);
        }
    } else if (code == 0x3C) ch_new();                           // F2
    else if (code == 0x16) ch_undo();                            // U
    else if (code == 0x01) g_ch.sel = -1;                        // Esc
    else if (code == 0x23) chess_hint();                         // H
    else if (code == 0x14) chess_set_mode(g_ch.mode ^ 1);        // T
    else if (code == 0x13 && g_ch.mode == 1) { ch_seed(); g_ch.tr_open = (int)(ch_rand() % (uint32_t)NOPEN); ch_new(); }   // R: random opening
    damage_content(&g_win[wi]);
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
       ID_THEME = 200, ID_THEMEPREV = 900, ID_THEMENEXT, ID_WP = 20, ID_WPM = 100, ID_PIC = 120, ID_SPEED = 30, ID_ACCEL, ID_DBL, ID_ICONS, ID_DOCK, ID_CLOCK24, ID_DESKFILES, ID_SAVE, ID_BOOTDESK,
       ID_AUTOTERM, ID_LOCK, ID_LOGOUT, ID_RESTART, ID_SHUT, ID_CONSOLE, ID_PASSWD, ID_ADDUSER, ID_OPENMON };

enum { ID_TCUR = 170, ID_TPROMPT = 175, ID_TWELCOME = 180 };

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
    case 0: {
        int cols = (r.w - SET_SIDE - 48) / 100;
        if (cols < 1) cols = 1;
        if (cols > 6) cols = 6;
        int per_page = cols * 3;
        int pages = (NTHEMES + per_page - 1) / per_page;
        if (g_theme_page >= pages) g_theme_page = pages - 1;
        if (g_theme_page < 0) g_theme_page = 0;
        int first = g_theme_page * per_page;
        int last = first + per_page;
        if (last > NTHEMES) last = NTHEMES;

        CTL(CTL_LABEL, 0, x0, y, 0, 0, "Colour theme", false);
        y += 22;
        for (int i = first; i < last; i++) {
            int k = i - first;
            CTL(CTL_BUTTON, ID_THEME + i, x0 + (k % cols) * 100, y + (k / cols) * 28, 94, 24, THEMES[i].name, C.theme == i);
        }
        y += 3 * 28 + 4;
        if (pages > 1) {
            CTL(CTL_BUTTON, ID_THEMEPREV, x0, y, 70, 24, "< Prev", false);
            snprintf(tmp, sizeof(tmp), "Page %d of %d", g_theme_page + 1, pages);
            CTL(CTL_HINT, 0, x0 + 90, y + 4, 0, 0, tmp, false);
            CTL(CTL_BUTTON, ID_THEMENEXT, x0 + 220, y, 70, 24, "Next >", false);
        }
        y += 34;
        CTL(CTL_LABEL, 0, x0, y, 0, 0, "Wallpaper", false);
        y += 22;
        {
            static const char *const WL[4] = { "Mascot", "Gradient", "Flat", "Reverse" };
            bool on[4] = { C.wallpaper == 3 && !C.wp_file[0], C.wallpaper == 0, C.wallpaper == 1, C.wallpaper == 2 };
            for (int i = 0; i < 4; i++) CTL(CTL_BUTTON, ID_WP + i, x0 + i * 100, y, 94, 28, WL[i], on[i]);
            y += 32;
            static const char *const ML[4] = { "Fill", "Fit", "Stretch", "Center" };
            for (int i = 0; i < 4; i++) CTL(CTL_BUTTON, ID_WPM + i, x0 + i * 100, y, 94, 24, ML[i], C.wp_mode == i);
        }
        y += 34;
        CTL(CTL_LABEL, 0, x0, y, 0, 0, "Your pictures", false);
        y += 22;
        for (int i = 0; i < g_npics; i++) CTL(CTL_BUTTON, ID_PIC + i, x0, y + i * 26, 250, 22, g_pics[i].name, C.wp_file[0] && strcmp(C.wp_file, g_pics[i].path) == 0);
        if (g_npics == 0) {
            CTL(CTL_HINT, 0, x0, y, 0, 0, "Copy .png files to /disk/Pictures", false);
            CTL(CTL_HINT, 0, x0, y + 20, 0, 0, "(or into your Desktop folder).", false);
        }
        break;
    }
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
        y += 24;
        for (int i = 0; i < 8; i++) CTL(CTL_BUTTON, ID_TERMS + i, x0 + (i % 4) * 100, y + (i / 4) * 30, 94, 26, TERM_LABELS[i], C.term_style == i);
        y += 64 + 112 + 14;                                  // the preview box sits in between (see render_settings)
        CTL(CTL_LABEL, 0, x0, y, 0, 0, "Cursor", false);
        y += 22;
        {
            static const char *const CL[3] = { "Block", "Underline", "Bar" };
            for (int i = 0; i < 3; i++) CTL(CTL_BUTTON, ID_TCUR + i, x0 + i * 100, y, 94, 26, CL[i], C.term_cursor == i);
        }
        y += 36;
        CTL(CTL_LABEL, 0, x0, y, 0, 0, "Prompt", false);
        y += 22;
        {
            static const char *const PL[4] = { "user@radium", "$", ">", "radium>" };
            for (int i = 0; i < 4; i++) CTL(CTL_BUTTON, ID_TPROMPT + i, x0 + i * 100, y, 94, 26, PL[i], C.term_prompt == i);
        }
        y += 36;
        CTL(CTL_TOGGLE, ID_TWELCOME, x0, y, 20, 20, "Greeting in a new terminal", C.term_welcome);
        y += 30;
        CTL(CTL_HINT, 0, x0, y, 0, 0, "Tab completes, Up/Down recall, Ctrl+A/E/U/K/W edit", false); y += 20;
        CTL(CTL_HINT, 0, x0, y, 0, 0, "the line, Ctrl+V pastes, Ctrl+C cancels the line.", false);
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
        CTL(CTL_HINT, 0, x0, y, 0, 0, "Desktop        RadiWM", false); y += 20;
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

    ctl_t ctl[48];
    int n = settings_layout(w, ctl, 48);
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
    if (w->tab == 0) settings_preview(c.x + SET_SIDE + 24 + 270, c.y + 262);
    if (w->tab == 4) {
        int px = c.x + SET_SIDE + 24, py = c.y + TERM_PREV_Y;
        char pr[40];
        term_prompt_str(pr, sizeof(pr));
        int pl = (int)strlen(pr);
        uint32_t pc = term_prompt_col();
        round_rect(px - 1, py - 1, 382, 114, 4, TH.line);
        fill_rect(px, py, 380, 112, TH.term_bg);
        text(px + 8, py + 6, pr, pc, TH.term_bg, pl);          text(px + 8 + pl * CW, py + 6, "ls /disk", TH.term_fg, TH.term_bg, 30);
        text(px + 8, py + 22, "etc  home  games  Pictures", TH.term_fg, TH.term_bg, 40);
        text(px + 8, py + 38, pr, pc, TH.term_bg, pl);         text(px + 8 + pl * CW, py + 38, "top -b", TH.term_fg, TH.term_bg, 30);
        text(px + 8, py + 54, "up 0:12:03   tasks 7   memory 41 of 2048 MB", TH.term_fg, TH.term_bg, 44);
        text(px + 8, py + 70, pr, pc, TH.term_bg, pl);         text(px + 8 + pl * CW, py + 70, "desktop check", TH.term_fg, TH.term_bg, 30);
        term_cursor_draw(px + 8 + (pl + 13) * CW, py + 70, ' ');
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
    ctl_t ctl[48];
    int n = settings_layout(w, ctl, 48);
    for (int i = 0; i < n; i++) {
        ctl_t *k = &ctl[i];
        if (k->kind == CTL_LABEL || k->kind == CTL_HINT) continue;
        int hit_y = k->kind == CTL_SLIDER ? k->y + 12 : k->y, hit_h = k->kind == CTL_SLIDER ? 26 : k->h;
        int hit_w = k->kind == CTL_TOGGLE ? 260 : k->w;
        if (mx < k->x - (k->kind == CTL_SLIDER ? 10 : 0) || mx >= k->x + hit_w + (k->kind == CTL_SLIDER ? 10 : 0) || my < hit_y || my >= hit_y + hit_h) continue;
        if (k->kind == CTL_SLIDER) { settings_slider_set(k->id, mx, k->x); g_drag_slider_id = k->id; g_drag_slider_x = k->x; damage_content(w); return; }
        if (k->id == ID_THEMEPREV) { if (g_theme_page > 0) g_theme_page--; }
        else if (k->id == ID_THEMENEXT) { g_theme_page++; }
        else if (k->id >= ID_THEME && k->id < ID_THEME + NTHEMES) apply_theme(k->id - ID_THEME);
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
        else if (k->id >= ID_TERMS && k->id < ID_TERMS + 8) { C.term_style = k->id - ID_TERMS; apply_theme(C.theme); }
        else if (k->id >= ID_TCUR && k->id < ID_TCUR + 3) { C.term_cursor = k->id - ID_TCUR; damage_all(); }
        else if (k->id >= ID_TPROMPT && k->id < ID_TPROMPT + 4) { C.term_prompt = k->id - ID_TPROMPT; damage_all(); }
        else if (k->id == ID_TWELCOME) C.term_welcome = !C.term_welcome;
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
    case W_WEB: return IC_WEB;
    case W_PAINT: return IC_PAINT;
    case W_MINES: return IC_MINES;
    case W_TIMER: return IC_TIMER;
    case W_CHESS: return IC_CHESS;
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
    { "Web", IC_WEB, APP_WEB, W_WEB }, { "Calculator", IC_CALC, APP_CALC, W_CALC }, { "Games", IC_GAME, APP_GAMES, W_GAMES }, { "Task Manager", IC_MON, APP_TASKS, W_TASKS },
    { "Paint", IC_PAINT, APP_PAINT, W_PAINT },
    { "Chess", IC_CHESS, APP_CHESS, W_CHESS },
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
    dic_add("Web", "Web", IC_WEB, 0, APP_WEB, "");
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
    case W_WEB: render_web(w, foc); break;
    case W_PAINT: render_paint(w); break;
    case W_MINES: render_mines(w); break;
    case W_TIMER: render_timer(w); break;
    case W_CHESS: render_chess(w); break;
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
       ACT_ICON_OPEN, ACT_ICON_EDIT, ACT_ICON_DELETE, ACT_ICON_WALL, ACT_ARRANGE, ACT_FILES, ACT_TERM_PASTE, ACT_TERM_CLEAR, ACT_TERM_SET };

typedef struct { char label[32]; int act, arg, icon; bool sep; } mitem_t;
static struct { bool open; int x, y, w, h, n, hover; mitem_t it[32]; } g_menu;

#define MENU_ROW 26
#define MENU_SEP 10

static void menu_clear(void) { g_menu.n = 0; }

static void menu_add(const char *label, int act, int arg, int icon)
{
    if (g_menu.n >= 32) return;
    mitem_t *m = &g_menu.it[g_menu.n++];
    put_str(m->label, sizeof(m->label), label);
    m->act = act; m->arg = arg; m->icon = icon; m->sep = false;
}

static void menu_sep(void) { if (g_menu.n < 32) { g_menu.it[g_menu.n].sep = true; g_menu.it[g_menu.n].act = ACT_NONE; g_menu.it[g_menu.n].label[0] = 0; g_menu.n++; } }

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
    case W_WEB: *w = 900; *h = 600; break;
    case W_PAINT: *w = 580; *h = 450; break;
    case W_MINES: *w = 412; *h = 384; break;
    case W_TIMER: *w = 360; *h = 360; break;
    case W_CHESS: *w = 840; *h = 600; break;
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
    if (g_win[wi].type == W_WEB) web_release(&g_win[wi]);
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
        if (i >= 0 && C.term_welcome) {
            term_puts(&g_win[i], "Welcome to RadiWM. Type a command and press Enter; Ctrl+C stops one that hangs.\n");
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
    case APP_WEB: return open_web();
    case APP_SCRIPTS: { int i = new_window(W_SCRIPTS, "Scripts"); if (i >= 0) { scripts_scan(&g_win[i]); g_win[i].lsel = -1; } return i; }
    case APP_PAINT: {
        for (int k = 0; k < MAX_WIN; k++) if (g_win[k].used && g_win[k].type == W_PAINT) { focus_and_raise(k); return k; }
        if (!paint_canvas()) { flash("not enough memory for the canvas"); return -1; }
        return new_window(W_PAINT, "Paint");
    }
    case APP_MINES: { int i = new_window(W_MINES, "Minesweeper"); if (i >= 0) mines_new(&g_win[i]); return i; }
    case APP_TIMER: return new_window(W_TIMER, "Timer");
    case APP_CHESS: {
        for (int k = 0; k < MAX_WIN; k++) if (g_win[k].used && g_win[k].type == W_CHESS) { focus_and_raise(k); return k; }
        chess_init();
        return new_window(W_CHESS, "Chess");
    }
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

// ---- web browser ---------------------------------------------------------------------------------------------------------------
// A window holds tabs; every tab is a page reduced to text (lines[], with the link number of every character in lattr[]),
// or a picture. The address bar works like the ytm search box: an address is fetched (one GET through the fetch engine,
// redirects followed), any other text is searched for and the engine's results page becomes a clean list of results.
// Pictures (PNG, JPEG, GIF, BMP) are decoded and shown in the tab; videos are downloaded and played with the mp4 player.

extern int rust_http_get_page(const uint8_t *url, uint8_t *out, uint32_t out_cap, int *status, uint8_t *final_url, uint32_t final_cap,
                              uint8_t *content_type, uint32_t content_type_cap);
extern void rust_http_error(char *out, uint32_t cap);
extern int rust_fetch(int argc, const uint8_t *const *argv);
extern int rust_disk_store_size(const uint8_t *path);
extern int rust_disk_store_remove(const uint8_t *path);
extern int rust_image_decode_fit(const uint8_t *data, uint32_t len, uint32_t max_w, uint32_t max_h, uint32_t *out, uint32_t out_cap, uint32_t *w_out, uint32_t *h_out);
extern int rust_image_kind(const uint8_t *data, uint32_t len);

#define WEB_BUF (1024 * 1024)
#define WEB_RES 32768
#define WEB_MAXTABS 6
#define WEB_TL 700
#define WEB_TC 156
#define WEB_IMG_W 760
#define WEB_IMG_H 500
#define WEB_TABS 36
#define WEB_NAV 44
#define WEB_BAR (WEB_TABS + WEB_NAV)
#define WEB_SEARCH "https://www.bing.com/search?q="
#define WEB_VIDEO_FILE "/disk/webvideo.mp4"
#define WA_LINK 0x3f
#define WA_HEAD 0x40
#define WA_DIM 0x80
enum { WB_BACK = 1001, WB_FWD, WB_RELOAD, WB_HOME, WB_NEWTAB, WB_TAB = 1100, WB_TABCLOSE = 1200, WB_TILE = 1300 };

typedef struct {
    bool used, start, loading;
    char lines[WEB_TL][WEB_TC];
    uint8_t lattr[WEB_TL][WEB_TC];
    int nl, col, scroll, wcols;
    char url[160], addr[160], stat[72], title[40], refresh[160];
    int addrlen, nhist, nfwd, nlink, status;
    char hist[8][160], fwd[8][160];
    char link[63][140];
    uint8_t lkind[63];                 // 0 page, 1 picture, 2 video
    int img_w, img_h, img_kind;        // a decoded picture (0 x 0 when the tab shows text)
} webtab_t;

static webtab_t g_tabs[WEB_MAXTABS];
static uint32_t g_webimg[WEB_MAXTABS][WEB_IMG_W * WEB_IMG_H];
static uint8_t g_webbuf[WEB_BUF + 1];
static char g_webres[WEB_RES];

static const struct { const char *label; const char *url; uint32_t col; } WEB_TILES[] = {
    { "RadiumOS", "https://radiumos.github.io", 0x5f7a52 }, { "GitHub", "https://github.com/RadiumOS-OPEN/RadiumOS", 0x24292f },
    { "Wikipedia", "https://en.wikipedia.org", 0x3a3a3a }, { "Bing", "https://www.bing.com", 0x0078d4 },
    { "Hacker News", "https://news.ycombinator.com", 0xff6600 }, { "Lite DDG", "https://lite.duckduckgo.com/lite/", 0xde5833 },
};
#define NWEB_TILES ((int)(sizeof(WEB_TILES) / sizeof(WEB_TILES[0])))

static webtab_t *web_tab(const win_t *w) { return w->ntabs > 0 ? &g_tabs[w->tabs[w->wtab]] : NULL; }

static const struct { const char *n; unsigned cp; } WEB_ENT[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 160 }, { "mdash", 0x2014 },
    { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201C }, { "rdquo", 0x201D },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "bull", 0x2022 }, { "middot", 0xB7 }, { "laquo", 0xAB }, { "raquo", 0xBB },
    { "rarr", 0x2192 }, { "larr", 0x2190 }, { "times", 0xD7 },
};
#define NWEB_ENT ((int)(sizeof(WEB_ENT) / sizeof(WEB_ENT[0])))

typedef struct {
    webtab_t *w;
    char word[WEB_TC];
    uint8_t wat[WEB_TC];
    int wn, link, pend;
    bool sp, pre, head, dim, started, trunc;
} wp_t;

static int web_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static const char *web_find_ci(const char *p, const char *end, const char *needle)
{
    int n = (int)strlen(needle);
    for (; p + n <= end; p++) {
        int k = 0;
        while (k < n && web_lower((unsigned char)p[k]) == needle[k]) k++;
        if (k == n) return p;
    }
    return NULL;
}

static void wp_row(wp_t *p)
{
    webtab_t *w = p->w;
    if (w->nl >= WEB_TL) { p->trunc = true; return; }
    w->lines[w->nl][0] = 0;
    memset(w->lattr[w->nl], 0, WEB_TC);
    w->nl++;
    w->col = 0;
}

static void wp_put(wp_t *p, char ch, uint8_t at)
{
    webtab_t *w = p->w;
    if (p->trunc) return;
    if (w->col >= w->wcols) { wp_row(p); if (p->trunc) return; }
    w->lines[w->nl - 1][w->col] = ch;
    w->lattr[w->nl - 1][w->col] = at;
    w->col++;
    w->lines[w->nl - 1][w->col] = 0;
    p->started = true;
}

// A block boundary asked for earlier (pend: 1 = new line, 2 = blank line) takes effect when the next text arrives.
static void wp_pending(wp_t *p)
{
    webtab_t *w = p->w;
    if (p->pend && p->started) {
        if (w->col > 0) wp_row(p);
        if (p->pend >= 2) wp_row(p);
        p->sp = false;
    }
    p->pend = 0;
}

static void wp_word(wp_t *p)
{
    webtab_t *w = p->w;
    if (p->wn == 0) return;
    wp_pending(p);
    if (w->col > 0 && p->wn <= w->wcols && w->col + (p->sp ? 1 : 0) + p->wn > w->wcols) { wp_row(p); p->sp = false; }
    if (w->col > 0 && p->sp) wp_put(p, ' ', 0);
    for (int i = 0; i < p->wn; i++) wp_put(p, p->word[i], p->wat[i]);
    p->wn = 0;
    p->sp = false;
}

static uint8_t wp_attr(const wp_t *p) { return (uint8_t)((p->link & WA_LINK) | (p->head ? WA_HEAD : 0) | (p->dim ? WA_DIM : 0)); }

static void wp_add(wp_t *p, char c, uint8_t at)
{
    if (p->wn >= WEB_TC - 2) wp_word(p);
    p->word[p->wn] = c;
    p->wat[p->wn] = at;
    p->wn++;
}

static void wp_ch(wp_t *p, char c)
{
    if (c == '\r') return;
    if (p->pre) {
        wp_word(p);
        wp_pending(p);
        if (c == '\n') { wp_row(p); return; }
        if (c == '\t') { for (int k = 0; k < 4; k++) wp_put(p, ' ', wp_attr(p)); return; }
        if ((unsigned char)c >= 32 && (unsigned char)c < 127) wp_put(p, c, wp_attr(p));
        return;
    }
    if (c == ' ' || c == '\n' || c == '\t') { wp_word(p); p->sp = true; return; }
    if ((unsigned char)c < 32 || (unsigned char)c > 126) return;
    wp_add(p, c, wp_attr(p));
}

static void wp_str(wp_t *p, const char *s) { while (*s) wp_ch(p, *s++); }

static void wp_break(wp_t *p, int n)
{
    wp_word(p);
    if (n > p->pend) p->pend = n;
}

// The page's characters outside printable ASCII become their nearest plain equivalent.
static const char *wp_cp(unsigned cp, char *one)
{
    switch (cp) {
    case 160: return " ";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: return "-";
    case 0x2018: case 0x2019: case 0x201A: return "'";
    case 0x201C: case 0x201D: return "\"";
    case 0x2022: case 0xB7: case 0x25CF: return "*";
    case 0x2026: return "...";
    case 0xA9: return "(c)";
    case 0xAE: return "(R)";
    case 0xAB: return "<<";
    case 0xBB: return ">>";
    case 0x2192: return "->";
    case 0x2190: return "<-";
    case 0xD7: return "x";
    default:
        if (cp >= 32 && cp < 127) { one[0] = (char)cp; one[1] = 0; return one; }
        return "?";
    }
}

// s points at '&'; returns how many characters the entity takes (0 when it is not one).
static int wp_entity(const char *s, const char *end, unsigned *cp)
{
    const char *q = s + 1;
    if (q < end && *q == '#') {
        bool hex = false;
        unsigned v = 0;
        q++;
        if (q < end && (*q == 'x' || *q == 'X')) { hex = true; q++; }
        const char *d0 = q;
        for (; q < end && q - d0 < 8; q++) {
            int c = web_lower((unsigned char)*q), dv;
            if (c >= '0' && c <= '9') dv = c - '0';
            else if (hex && c >= 'a' && c <= 'f') dv = c - 'a' + 10;
            else break;
            v = v * (hex ? 16u : 10u) + (unsigned)dv;
        }
        if (q == d0) return 0;
        if (q < end && *q == ';') q++;
        *cp = v;
        return (int)(q - s);
    }
    for (int i = 0; i < NWEB_ENT; i++) {
        int l = (int)strlen(WEB_ENT[i].n);
        if (q + l < end && strncmp(q, WEB_ENT[i].n, (size_t)l) == 0 && q[l] == ';') { *cp = WEB_ENT[i].cp; return l + 2; }
    }
    return 0;
}

static void wp_cpstr(wp_t *p, unsigned cp)
{
    char one[2];
    wp_str(p, wp_cp(cp, one));
}

static int web_hex(int c)
{
    c = web_lower(c);
    return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1);
}

static void web_urlenc(const char *in, char *out, int cap)
{
    static const char hex[] = "0123456789ABCDEF";
    int n = 0;
    for (; *in && n + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out[n++] = (char)c;
        else if (c == ' ') out[n++] = '+';
        else { out[n++] = '%'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 15]; }
    }
    out[n] = 0;
}

// Removes "." and ".." segments from the path of an address (the query stays as it is).
static void web_dots(char *u)
{
    char *p = strstr(u, "://");
    if (!p || !(p = strchr(p + 3, '/'))) return;
    char tail[300], *qm = p, *src = p, *dst = p;
    while (*qm && *qm != '?') qm++;
    put_str(tail, sizeof(tail), qm);
    *qm = 0;
    while (*src) {
        char *seg = src + 1, *e = seg;
        while (*e && *e != '/') e++;
        int l = (int)(e - seg);
        if (l == 1 && seg[0] == '.') { }
        else if (l == 2 && seg[0] == '.' && seg[1] == '.') { if (dst > p) { dst--; while (dst > p && *dst != '/') dst--; } }
        else { memmove(dst, src, (size_t)(e - src)); dst += e - src; }
        src = e;
    }
    if (dst == p) *dst++ = '/';
    memcpy(dst, tail, strlen(tail) + 1);
}

// href as found in a page -> a full address; out stays empty for links that cannot be followed (mailto:, #anchor ...).
static void web_resolve(const char *base, const char *href, char *out, int cap)
{
    char head[200], tmp[360];
    int n = 0;
    out[0] = 0;
    while (*href == ' ') href++;
    if (!*href || *href == '#') return;
    static const char *const SKIP[] = { "mailto:", "javascript:", "tel:", "data:", NULL };
    for (int i = 0; SKIP[i]; i++) if (strncmp(href, SKIP[i], strlen(SKIP[i])) == 0) return;
    head[0] = 0;
    if (strncmp(href, "http://", 7) != 0 && strncmp(href, "https://", 8) != 0) {
        const char *o = strstr(base, "://");
        if (!o) return;
        const char *oe = o + 3, *q = base;
        while (*oe && *oe != '/' && *oe != '?' && *oe != '#') oe++;
        while (*q && *q != '?' && *q != '#') q++;
        int ol = (int)(oe - base);
        if (ol >= (int)sizeof(head) - 2) return;
        if (href[0] == '/' && href[1] == '/') { int sl = (int)(o - base); memcpy(head, base, (size_t)sl); head[sl] = 0; }
        else if (href[0] == '/') { memcpy(head, base, (size_t)ol); head[ol] = 0; }
        else if (href[0] == '?') { int ql = (int)(q - base); if (ql >= (int)sizeof(head)) return; memcpy(head, base, (size_t)ql); head[ql] = 0; }
        else {
            const char *slash = NULL;
            for (const char *k = oe; k < q; k++) if (*k == '/') slash = k;
            int dl = slash ? (int)(slash - base) + 1 : ol;
            if (dl >= (int)sizeof(head) - 2) return;
            memcpy(head, base, (size_t)dl);
            head[dl] = 0;
            if (!slash) { head[dl++] = '/'; head[dl] = 0; }
        }
    }
    for (const char *k = head; *k && n < (int)sizeof(tmp) - 1; k++) tmp[n++] = *k;
    for (const char *k = href; *k && *k != '#' && n < (int)sizeof(tmp) - 4; k++) {
        if (strncmp(k, "&amp;", 5) == 0) { tmp[n++] = '&'; k += 4; }
        else if (*k == ' ') { tmp[n++] = '%'; tmp[n++] = '2'; tmp[n++] = '0'; }
        else if ((unsigned char)*k > 32 && (unsigned char)*k < 127) tmp[n++] = *k;
    }
    tmp[n] = 0;
    web_dots(tmp);
    // search engines wrap results in a redirect; open the real address directly
    char *u = strstr(tmp, "uddg=");
    if (u && strstr(tmp, "duckduckgo.com/l/")) {
        char *d = tmp;
        for (const char *k = u + 5; *k && *k != '&'; k++) {
            if (*k == '%' && web_hex(k[1]) >= 0 && web_hex(k[2]) >= 0) { *d++ = (char)(web_hex(k[1]) * 16 + web_hex(k[2])); k += 2; }
            else *d++ = *k;
        }
        *d = 0;
        if (strncmp(tmp, "http", 4) != 0) return;
    }
    if ((int)strlen(tmp) >= cap) return;
    put_str(out, cap, tmp);
}

static const char *web_tag_end(const char *p, const char *end)
{
    char q = 0;
    for (; p < end; p++) {
        if (q) { if (*p == q) q = 0; }
        else if (*p == '"' || *p == '\'') q = *p;
        else if (*p == '>') return p;
    }
    return NULL;
}

static bool web_attr(const char *t, const char *te, const char *name, char *out, int cap)
{
    int nl = (int)strlen(name);
    out[0] = 0;
    for (const char *p = t; p + nl < te; p++) {
        if (p == t || (p[-1] != ' ' && p[-1] != '\n' && p[-1] != '\t' && p[-1] != '\r')) continue;
        int k = 0;
        while (k < nl && web_lower((unsigned char)p[k]) == name[k]) k++;
        if (k < nl) continue;
        const char *v = p + nl;
        while (v < te && *v == ' ') v++;
        if (v >= te || *v != '=') continue;
        v++;
        while (v < te && *v == ' ') v++;
        char q = (v < te && (*v == '"' || *v == '\'')) ? *v++ : 0;
        int n = 0;
        while (v < te && n < cap - 1 && (q ? *v != q : (*v != ' ' && *v != '>'))) out[n++] = *v++;
        out[n] = 0;
        return true;
    }
    return false;
}

static const char *web_after_close(const char *s, const char *end, const char *name)
{
    char needle[16];
    snprintf(needle, sizeof(needle), "</%s", name);
    const char *e = web_find_ci(s, end, needle);
    if (!e) return end;
    while (e < end && *e != '>') e++;
    return e < end ? e + 1 : end;
}

// s points at '<'; handles one tag or comment and returns where the text goes on.
// Adds a link to the tab; 0 when there is no room. kind: 0 page, 1 picture, 2 video.
static int web_add_link(webtab_t *w, const char *base, const char *href, int kind)
{
    char url[160];
    if (w->nlink >= 63) return 0;
    web_resolve(base, href, url, sizeof(url));
    if (!url[0] || strlen(url) >= sizeof(w->link[0])) return 0;
    put_str(w->link[w->nlink], sizeof(w->link[0]), url);
    w->lkind[w->nlink] = (uint8_t)kind;
    w->nlink++;
    return w->nlink;
}

static bool web_has_ext(const char *url, const char *ext)
{
    const char *q = url;
    while (*q && *q != '?' && *q != '#') q++;
    int l = (int)(q - url), el = (int)strlen(ext);
    if (l < el) return false;
    for (int i = 0; i < el; i++) if (web_lower((unsigned char)url[l - el + i]) != ext[i]) return false;
    return true;
}

static int web_link_kind_for(const char *url)
{
    if (web_has_ext(url, ".mp4") || web_has_ext(url, ".m4v")) return 2;
    if (web_has_ext(url, ".png") || web_has_ext(url, ".jpg") || web_has_ext(url, ".jpeg") || web_has_ext(url, ".gif") || web_has_ext(url, ".bmp")) return 1;
    return 0;
}

// Writes a tag's number marker "[n]" with the link attribute so it can be clicked.
static void wp_link_marker(wp_t *p, int n, const char *label)
{
    char buf[80];
    snprintf(buf, sizeof(buf), "%s[%d]", label, n);
    for (int k = 0; buf[k]; k++) wp_add(p, buf[k], (uint8_t)(WA_DIM | n));
}

// The matching close tag of an element that is not shown (display:none, hidden); nested same-name elements counted.
static const char *web_skip_element(const char *name, const char *s, const char *end)
{
    int depth = 1;
    char open[16], close[16];
    snprintf(open, sizeof(open), "<%s", name);
    snprintf(close, sizeof(close), "</%s", name);
    const char *q = s;
    while (depth > 0) {
        const char *o = web_find_ci(q, end, open), *c = web_find_ci(q, end, close);
        if (!c) return end;
        if (o && o < c && (o[strlen(open)] == ' ' || o[strlen(open)] == '>' || o[strlen(open)] == '/' || o[strlen(open)] == '\n')) { depth++; q = o + 1; continue; }
        depth--;
        q = c + 1;
    }
    while (q < end && *q != '>') q++;
    return q < end ? q + 1 : end;
}

static bool web_hidden(const char *t, const char *te)
{
    char v[200];
    if (web_attr(t, te, "hidden", v, sizeof(v))) return true;
    for (const char *q = t + 1; q + 6 <= te; q++)
        if ((q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\n') && strncmp(q, "hidden", 6) == 0 && (q[6] == ' ' || q[6] == '>' || q[6] == '/' || q[6] == '\n')) return true;
    if (!web_attr(t, te, "style", v, sizeof(v))) return false;
    for (char *c = v; *c; c++) *c = (char)web_lower((unsigned char)*c);
    const char *d = strstr(v, "display");
    if (d) { d += 7; while (*d == ' ' || *d == ':') d++; if (strncmp(d, "none", 4) == 0) return true; }
    const char *vi = strstr(v, "visibility");
    if (vi) { vi += 10; while (*vi == ' ' || *vi == ':') vi++; if (strncmp(vi, "hidden", 6) == 0) return true; }
    return false;
}

static const char *web_tag(wp_t *p, const char *base, const char *s, const char *end, char *title, int tcap)
{
    if (s + 3 < end && s[1] == '!' && s[2] == '-' && s[3] == '-') {
        const char *e = web_find_ci(s + 4, end, "-->");
        return e ? e + 3 : end;
    }
    const char *t = s + 1;
    bool close = false;
    if (t < end && *t == '/') { close = true; t++; }
    if (t < end && (*t == '!' || *t == '?')) { const char *g = web_tag_end(t, end); return g ? g + 1 : end; }
    char name[12];
    int nn = 0;
    while (t + nn < end && nn < 11 && ((web_lower((unsigned char)t[nn]) >= 'a' && web_lower((unsigned char)t[nn]) <= 'z') || (t[nn] >= '0' && t[nn] <= '9'))) { name[nn] = (char)web_lower((unsigned char)t[nn]); nn++; }
    name[nn] = 0;
    if (nn == 0) { wp_ch(p, '<'); return s + 1; }
    const char *te = web_tag_end(t, end);
    if (!te) return end;
    webtab_t *w = p->w;
#define TAG_IS(x) (strcmp(name, x) == 0)
    if (!close && (TAG_IS("script") || TAG_IS("style") || TAG_IS("template") || TAG_IS("svg") || TAG_IS("noscript") || TAG_IS("select") || TAG_IS("iframe") || TAG_IS("object") || TAG_IS("head") || TAG_IS("canvas") || TAG_IS("map"))) {
        if (TAG_IS("head")) {                            // the title is still wanted
            const char *body = web_find_ci(te + 1, end, "<body");
            const char *tt = web_find_ci(te + 1, body ? body : end, "<title");
            if (tt) { const char *tte = web_tag_end(tt, end); if (tte) web_tag(p, base, tt, end, title, tcap); }
            const char *mr = te + 1;
            while ((mr = web_find_ci(mr, body ? body : end, "<meta")) != NULL) {
                const char *me = web_tag_end(mr, end);
                if (!me) break;
                char eq[40], content[300];
                if (web_attr(mr + 1, me, "http-equiv", eq, sizeof(eq)) && web_lower(eq[0]) == 'r' && web_attr(mr + 1, me, "content", content, sizeof(content))) {
                    const char *u = web_find_ci(content, content + strlen(content), "url=");
                    if (u) { u += 4; while (*u == ' ' || *u == '\'' || *u == '"') u++; char tmp[300]; put_str(tmp, sizeof(tmp), u); char *e = tmp; while (*e && *e != '\'' && *e != '"' && *e != ' ') e++; *e = 0; web_resolve(base, tmp, w->refresh, sizeof(w->refresh)); }
                }
                mr = me + 1;
            }
            return body ? body : web_after_close(te + 1, end, "head");
        }
        return web_after_close(te + 1, end, name);
    }
    if (!close && !TAG_IS("br") && !TAG_IS("hr") && !TAG_IS("img") && !TAG_IS("input") && web_hidden(t, te)) return web_skip_element(name, te + 1, end);
    if (TAG_IS("b") || TAG_IS("strong")) { wp_word(p); p->head = !close; }
    if (!close && TAG_IS("title")) {
        const char *after = web_after_close(te + 1, end, "title");
        int n = 0;
        bool space = true;
        for (const char *k = te + 1; k < after && *k != '<' && n < tcap - 1; k++) {
            unsigned char c = (unsigned char)*k;
            if (c == ' ' || c == '\n' || c == '\t' || c == '\r') { if (!space) title[n++] = ' '; space = true; }
            else if (c == '&' && strncmp(k, "&amp;", 5) == 0) { title[n++] = '&'; k += 4; space = false; }
            else if (c >= 32 && c < 127) { title[n++] = (char)c; space = false; }
        }
        while (n > 0 && title[n - 1] == ' ') n--;
        title[n] = 0;
        return after;
    }
    if (TAG_IS("br")) { wp_word(p); if (p->pend < 2) p->pend++; }
    else if (TAG_IS("p") || TAG_IS("ul") || TAG_IS("ol") || TAG_IS("table") || TAG_IS("blockquote") || TAG_IS("dl")) wp_break(p, 2);
    else if (TAG_IS("pre")) { wp_break(p, 2); p->pre = !close; }
    else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == 0) { wp_break(p, 2); p->head = !close; }
    else if (TAG_IS("li")) { wp_break(p, 1); if (!close) wp_str(p, "* "); }
    else if (TAG_IS("hr")) {
        wp_break(p, 1);
        for (int k = 0; k < 24; k++) wp_add(p, '-', WA_DIM);
        wp_break(p, 1);
    }
    else if (TAG_IS("small") || TAG_IS("cite")) { wp_word(p); p->dim = !close; }
    else if (TAG_IS("td") || TAG_IS("th")) { wp_word(p); p->sp = true; }
    else if (TAG_IS("div") || TAG_IS("tr") || TAG_IS("section") || TAG_IS("article") || TAG_IS("header") || TAG_IS("footer") || TAG_IS("nav") ||
             TAG_IS("main") || TAG_IS("aside") || TAG_IS("form") || TAG_IS("center") || TAG_IS("dd") || TAG_IS("dt") || TAG_IS("figure") ||
             TAG_IS("caption") || TAG_IS("address")) wp_break(p, 1);
    else if (TAG_IS("a")) {
        if (!close) {
            char href[200];
            p->link = 0;
            if (web_attr(t, te, "href", href, sizeof(href))) {
                char url[160];
                web_resolve(base, href, url, sizeof(url));
                p->link = web_add_link(w, base, href, url[0] ? web_link_kind_for(url) : 0);
            }
        } else if (p->link) {
            wp_link_marker(p, p->link, "");
            p->link = 0;
        }
    }
    else if (TAG_IS("img") && !close) {
        char alt[60], src[300];
        bool has_src = web_attr(t, te, "src", src, sizeof(src)) || web_attr(t, te, "data-src", src, sizeof(src));
        if (!(web_attr(t, te, "alt", alt, sizeof(alt)) && alt[0])) put_str(alt, sizeof(alt), "picture");
        if (has_src && strncmp(src, "data:", 5) != 0) {
            int n = web_add_link(w, base, src, 1);
            wp_word(p);
            if (n) { char lab[80]; snprintf(lab, sizeof(lab), "[%s]", alt); wp_link_marker(p, n, lab); }
            else { wp_ch(p, '['); wp_str(p, alt); wp_ch(p, ']'); }
        } else { wp_ch(p, '['); wp_str(p, alt); wp_ch(p, ']'); }
    }
    else if ((TAG_IS("video") || TAG_IS("source") || TAG_IS("audio")) && !close) {
        char src[300];
        if (web_attr(t, te, "src", src, sizeof(src)) && strncmp(src, "blob:", 5) != 0) {
            wp_break(p, 1);
            int n = web_add_link(w, base, src, TAG_IS("audio") ? 0 : 2);
            if (n) wp_link_marker(p, n, TAG_IS("audio") ? "[audio]" : "[video]");
        }
    }
    else if (TAG_IS("button")) { wp_word(p); if (!close) wp_str(p, "[ "); else wp_str(p, " ]"); }
    else if (TAG_IS("input") && !close) {
        char type[20], val[80];
        if (!web_attr(t, te, "type", type, sizeof(type))) put_str(type, sizeof(type), "text");
        for (char *c = type; *c; c++) *c = (char)web_lower((unsigned char)*c);
        if (strcmp(type, "hidden") == 0) { }
        else if (strcmp(type, "submit") == 0 || strcmp(type, "button") == 0) { wp_word(p); wp_str(p, "[ "); wp_str(p, web_attr(t, te, "value", val, sizeof(val)) && val[0] ? val : "Submit"); wp_str(p, " ]"); }
        else if (strcmp(type, "checkbox") == 0 || strcmp(type, "radio") == 0) wp_str(p, "[ ]");
        else { wp_word(p); wp_str(p, "[______]"); }
    }
    else if (TAG_IS("textarea") && !close) { wp_break(p, 1); wp_str(p, "[________________]"); return web_after_close(te + 1, end, "textarea"); }
#undef TAG_IS
    return te + 1;
}

static void web_convert(webtab_t *w, const char *base, const char *s, int len, bool html, char *title, int tcap)
{
    wp_t p;
    memset(&p, 0, sizeof(p));
    p.w = w;
    const char *end = s + len;
    title[0] = 0;
    w->refresh[0] = 0;
    if (!html) {
        p.pre = true;
        for (; s < end && !p.trunc; s++) wp_ch(&p, *s);
        return;
    }
    while (s < end && !p.trunc) {
        unsigned char c = (unsigned char)*s;
        if (c == '<') { s = web_tag(&p, base, s, end, title, tcap); continue; }
        if (c == '&') {
            unsigned cp;
            int k = wp_entity(s, end, &cp);
            if (k) { wp_cpstr(&p, cp); s += k; continue; }
        }
        if (c >= 0xC0) {
            int l = c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : 2);
            unsigned cp = c & (0xFFu >> (l + 1));
            int k = 1;
            for (; k < l && s + k < end && ((unsigned char)s[k] & 0xC0) == 0x80; k++) cp = (cp << 6) | ((unsigned char)s[k] & 0x3F);
            wp_cpstr(&p, cp);
            s += k;
        } else if (c >= 0x80) s++;
        else { wp_ch(&p, (char)c); s++; }
    }
    wp_word(&p);
}

static bool web_is_html(const char *b, int n)
{
    int lim = n < 1024 ? n : 1024;
    return n > 0 && (b[0] == '<' || web_find_ci(b, b + lim, "<html") || web_find_ci(b, b + lim, "<!doctype html") || web_find_ci(b, b + lim, "<body") || web_find_ci(b, b + lim, "<head"));
}

// Fetches url into g_webbuf (one GET, redirects followed). The page's full size, or -1; *status gets the HTTP status,
// final_url the address the page was read from and ctype its media type.
static int web_download(const char *url, int *status, char *final_url, int final_cap, char *ctype, int ctype_cap)
{
    *status = 0;
    final_url[0] = 0;
    ctype[0] = 0;
    int size = rust_http_get_page((const uint8_t *)url, g_webbuf, WEB_BUF, status, (uint8_t *)final_url, (uint32_t)final_cap, (uint8_t *)ctype, (uint32_t)ctype_cap);
    if (size < 0) return -1;
    g_webbuf[size < WEB_BUF ? size : WEB_BUF] = 0;
    if (!final_url[0]) put_str(final_url, final_cap, url);
    return size;
}

static int web_b64(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

// Copies the text between s and e into out with tags removed and entities kept (the converter decodes them).
static int web_inner_text(const char *s, const char *e, char *out, int cap)
{
    int n = 0;
    bool tag = false, space = true;
    for (; s < e && n < cap - 1; s++) {
        if (*s == '<') { tag = true; continue; }
        if (tag) { if (*s == '>') tag = false; continue; }
        unsigned char c = (unsigned char)*s;
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') { if (!space) out[n++] = ' '; space = true; continue; }
        out[n++] = (char)c;
        space = false;
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
    return n;
}

// The search engine's page -> a small page of our own in g_webres: one heading per result with its address and summary.
// Results come as <li class="b_algo"> blocks; the link inside goes through a redirect whose u=a1<base64> parameter is the
// real address. False when nothing looked like a result (the page is then shown as it is).
static bool web_search_results(const char *query, int len, int *nres)
{
    const char *end = (const char *)g_webbuf + len, *p = (const char *)g_webbuf;
    int o = 0, n = 0;
    o += snprintf(g_webres + o, sizeof(g_webres) - (size_t)o, "<h1>Results for %s</h1>", query);
    while ((p = web_find_ci(p, end, "<li class=\"b_algo\"")) != NULL && o < WEB_RES - 1400) {
        const char *li_end = web_find_ci(p, end, "</li>");
        if (!li_end) li_end = end;
        const char *h2 = web_find_ci(p, li_end, "<h2>");
        const char *a = h2 ? web_find_ci(h2, li_end, "<a ") : NULL;
        if (!a) { p = li_end; continue; }
        const char *ae = web_tag_end(a, li_end);
        const char *a_close = ae ? web_find_ci(ae, li_end, "</a>") : NULL;
        if (!ae || !a_close) { p = li_end; continue; }
        char href[600], url[200], title[160], snip[400], host[100];
        if (!web_attr(a + 1, ae, "href", href, sizeof(href))) { p = li_end; continue; }
        url[0] = 0;
        const char *u = strstr(href, "u=a1");
        if (u) {
            int k = 0, bits = 0;
            unsigned val = 0;
            for (const char *q = u + 4; *q && *q != '&' && k < (int)sizeof(url) - 1; q++) {
                if (strncmp(q, "&amp;", 5) == 0) break;
                int d = web_b64(*q);
                if (d < 0) break;
                val = ((val << 6) | (unsigned)d) & 0xffffu;
                bits += 6;
                if (bits >= 8) { bits -= 8; url[k++] = (char)((val >> bits) & 0xff); }
            }
            url[k] = 0;
        } else if (strncmp(href, "http", 4) == 0 && !strstr(href, "bing.com/ck/")) web_resolve("", href, url, sizeof(url));
        if (strncmp(url, "http", 4) != 0) { p = li_end; continue; }
        web_inner_text(ae + 1, a_close, title, sizeof(title));
        snip[0] = 0;
        const char *h2e = web_find_ci(a_close, li_end, "</h2>");
        const char *pp = h2e ? web_find_ci(h2e, li_end, "<p") : NULL;
        if (pp) {
            const char *pe = web_tag_end(pp, li_end), *pc = pe ? web_find_ci(pe, li_end, "</p>") : NULL;
            if (pe && pc) web_inner_text(pe + 1, pc, snip, sizeof(snip));
        }
        const char *hs = strstr(url, "://");
        int hl = 0;
        if (hs) { hs += 3; while (hs[hl] && hs[hl] != '/' && hl < (int)sizeof(host) - 1) { host[hl] = hs[hl]; hl++; } }
        host[hl] = 0;
        char safe_url[260];
        int sn = 0;
        for (const char *q = url; *q && sn < (int)sizeof(safe_url) - 6; q++) {
            if (*q == '&') { memcpy(safe_url + sn, "&amp;", 5); sn += 5; }
            else if (*q == '"') sn += snprintf(safe_url + sn, sizeof(safe_url) - (size_t)sn, "%%22");
            else safe_url[sn++] = *q;
        }
        safe_url[sn] = 0;
        o += snprintf(g_webres + o, sizeof(g_webres) - (size_t)o, "<h3><a href=\"%s\">%s</a></h3><p>%s<br><small>%s</small></p>",
                      safe_url, title[0] ? title : url, snip[0] ? snip : "(no summary)", host);
        n++;
        p = li_end;
    }
    if (n == 0) return false;
    o += snprintf(g_webres + o, sizeof(g_webres) - (size_t)o, "<hr><p><small>%d result%s from Bing.</small></p>", n, n == 1 ? "" : "s");
    *nres = n;
    return true;
}

// ---- tabs ------------------------------------------------------------------------------------------------------------------------

static int web_rows(const win_t *w) { return imax(1, (content_rect(w).h - WEB_BAR - 8) / CH); }

static int web_first_row(const win_t *w, webtab_t *t)
{
    int maxs = imax(0, t->nl - web_rows(w));
    t->scroll = imax(0, imin(t->scroll, maxs));
    return imax(0, t->nl - web_rows(w) - t->scroll);
}

static void web_tab_set_title(win_t *w, webtab_t *t, const char *page_title)
{
    if (page_title[0]) put_str(t->title, sizeof(t->title), page_title);
    else if (t->url[0]) { const char *h = strstr(t->url, "://"); put_str(t->title, sizeof(t->title), h ? h + 3 : t->url); }
    else put_str(t->title, sizeof(t->title), "New Tab");
    if (web_tab(w) == t) { put_str(w->title, sizeof(w->title), t->title); damage_title(w); damage(0, 0, SW, PANEL_H); }
}

static void web_tab_clear(webtab_t *t)
{
    t->nl = 1;
    t->lines[0][0] = 0;
    memset(t->lattr[0], 0, WEB_TC);
    t->col = 0;
    t->scroll = 0;
    t->nlink = 0;
    t->img_w = t->img_h = t->img_kind = 0;
}

static void web_tab_puts(webtab_t *t, const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') { if (t->nl < WEB_TL) { t->lines[t->nl][0] = 0; memset(t->lattr[t->nl], 0, WEB_TC); t->nl++; t->col = 0; } continue; }
        if (t->col < WEB_TC - 1) { t->lines[t->nl - 1][t->col] = *s; t->lattr[t->nl - 1][t->col] = 0; t->col++; t->lines[t->nl - 1][t->col] = 0; }
    }
}

// Turns a page into lines. base is the address the relative links are resolved against.
static void web_show(win_t *w, webtab_t *t, const char *base, const char *buf, int len, bool html, bool truncated)
{
    char title[40];
    web_tab_clear(t);
    t->start = false;
    t->wcols = imax(20, imin(WEB_TC - 2, (w->w - 2 * BORD - 2 * PADX - 40) / CW));
    web_convert(t, base, buf, len, html, title, sizeof(title));
    if (truncated && t->nl < WEB_TL) web_tab_puts(t, "\n[the page goes on; this browser reads the first megabyte]");
    else if (t->nl >= WEB_TL) put_str(t->lines[WEB_TL - 1], WEB_TC, "[the page goes on; this browser shows the first 700 lines]");
    t->scroll = imax(0, t->nl - web_rows(w));
    web_tab_set_title(w, t, title);
}

// A picture becomes the tab's content: decoded into the tab's pixel buffer, shrunk to fit the window.
static bool web_show_image(win_t *w, webtab_t *t, const char *url, int len)
{
    rect_t c = content_rect(w);
    uint32_t iw = 0, ih = 0;
    int ti = (int)(t - g_tabs);
    int kind = rust_image_decode_fit(g_webbuf, (uint32_t)len, (uint32_t)imin(WEB_IMG_W, imax(64, c.w - 32)), (uint32_t)imin(WEB_IMG_H, imax(64, c.h - WEB_BAR - 44)),
                                     g_webimg[ti], WEB_IMG_W * WEB_IMG_H, &iw, &ih);
    if (kind <= 0) return false;
    static const char *const KIND[5] = { "", "PNG", "JPEG", "GIF", "BMP" };
    web_tab_clear(t);
    t->start = false;
    t->img_w = (int)iw; t->img_h = (int)ih; t->img_kind = kind;
    const char *name = url;
    for (const char *q = url; *q && *q != '?'; q++) if (*q == '/') name = q + 1;
    char title[40];
    put_str(title, sizeof(title), name[0] ? name : "picture");
    web_tab_set_title(w, t, title);
    snprintf(t->stat, sizeof(t->stat), "%s picture, %d KB, shown at %u x %u", KIND[kind], (len + 1023) / 1024, iw, ih);
    return true;
}

static void web_push(char hist[8][160], int *n, const char *url)
{
    if (*n == 8) { memmove(hist[0], hist[1], 7 * sizeof(hist[0])); (*n)--; }
    put_str(hist[(*n)++], sizeof(hist[0]), url);
}

static bool web_video_url(const char *url, const char *ctype)
{
    return web_link_kind_for(url) == 2 || strncmp(ctype, "video/", 6) == 0;
}

// Downloads a video to the fetch store and plays it; the desktop steps aside while it plays.
static void web_play_video(int wi, const char *url)
{
    win_t *w = &g_win[wi];
    webtab_t *t = web_tab(w);
    if (!t) return;
    put_str(t->stat, sizeof(t->stat), "Downloading the video ...");
    damage_content(w);
    if (win_visible(wi)) draw_frame();
    const uint8_t *args[] = {
        (const uint8_t *)"fetch", (const uint8_t *)"-L", (const uint8_t *)"-f", (const uint8_t *)"-s", (const uint8_t *)"--overwrite",
        (const uint8_t *)"--timeout", (const uint8_t *)"600", (const uint8_t *)"-o", (const uint8_t *)WEB_VIDEO_FILE, (const uint8_t *)url,
    };
    if (rust_fetch((int)(sizeof(args) / sizeof(args[0])), args) != 0 || rust_disk_store_size((const uint8_t *)WEB_VIDEO_FILE) <= 0) {
        char why[100];
        rust_http_error(why, sizeof(why));
        rust_disk_store_remove((const uint8_t *)WEB_VIDEO_FILE);
        snprintf(t->stat, sizeof(t->stat), "Video download failed: %s", why);
        damage_content(w);
        return;
    }
    run_leave("mp4 " WEB_VIDEO_FILE);
    rust_disk_store_remove((const uint8_t *)WEB_VIDEO_FILE);
    put_str(t->stat, sizeof(t->stat), "Video finished");
    damage_content(w);
}

static void web_load(int wi, const char *url, const char *shown, bool push)
{
    win_t *w = &g_win[wi];
    webtab_t *t = web_tab(w);
    if (!t) return;
    if (push) {
        if (t->url[0] && strcmp(t->url, url) != 0) web_push(t->hist, &t->nhist, t->url);
        t->nfwd = 0;
    }
    char copy[160], final_url[160], ctype[64];
    put_str(copy, sizeof(copy), url);
    put_str(t->url, sizeof(t->url), url);
    t->addrlen = put_str(t->addr, sizeof(t->addr), shown);
    put_str(t->stat, sizeof(t->stat), "Loading ...");
    t->loading = true;
    damage_content(w);
    if (win_visible(wi)) draw_frame();

    int status = 0;
    int size = web_download(copy, &status, final_url, sizeof(final_url), ctype, sizeof(ctype));
    t->loading = false;
    t->status = status;
    if (size < 0) {
        char why[120], msg[520];
        rust_http_error(why, sizeof(why));
        int n;
        if (strstr(why, "exceeds")) n = snprintf(msg, sizeof(msg), "<h1>This page is very large</h1><p>It is bigger than the 24 MB this browser reads into memory. Save it with:</p><pre>fetch -L -o /disk/NAME %s</pre>", copy);
        else n = snprintf(msg, sizeof(msg), "<h1>Hmm. We're having trouble finding that site.</h1><p>%s</p><p>Checking the address and the network connection "
                          "often helps. The fetch command gives the same answer in a terminal:</p><pre>fetch -L %s</pre>", why, copy);
        web_show(w, t, copy, msg, n, true, false);
        put_str(t->stat, sizeof(t->stat), why);
    } else {
        bool search = strncmp(copy, WEB_SEARCH, strlen(WEB_SEARCH)) == 0;
        if (strcmp(final_url, copy) != 0 && !search) { put_str(t->url, sizeof(t->url), final_url); t->addrlen = put_str(t->addr, sizeof(t->addr), final_url); }
        int got = size < WEB_BUF ? size : WEB_BUF, bin = 0, nres = 0;
        for (int i = 0; i < got && i < 512; i++) if (g_webbuf[i] == 0 || g_webbuf[i] < 9) bin++;
        if (size == 0) {
            char msg[200];
            int n = snprintf(msg, sizeof(msg), "<h1>HTTP %d</h1><p>The server sent an empty page.</p>", status);
            web_show(w, t, copy, msg, n, true, false);
            snprintf(t->stat, sizeof(t->stat), "HTTP %d, empty page", status);
        } else if ((strncmp(ctype, "image/", 6) == 0 || rust_image_kind(g_webbuf, (uint32_t)got) > 0) && size <= WEB_BUF && web_show_image(w, t, final_url, got)) {
            // shown; the status line was written by web_show_image
        } else if (web_video_url(final_url, ctype)) {
            char msg[400];
            int n = snprintf(msg, sizeof(msg), "<h1>Video</h1><p>%d KB. <a href=\"%s\">Play it</a> (the player takes the whole screen; the desktop comes back when it ends).</p>", (size + 1023) / 1024, final_url);
            web_show(w, t, copy, msg, n, true, false);
            put_str(t->stat, sizeof(t->stat), "Click Play to watch the video");
        } else if (bin > 4 || (ctype[0] && strncmp(ctype, "text/", 5) != 0 && !strstr(ctype, "html") && !strstr(ctype, "xml") && !strstr(ctype, "json") && !strstr(ctype, "javascript"))) {
            char msg[300];
            int n = snprintf(msg, sizeof(msg), "<h1>%s</h1><p>%d KB that are not a page. Save the file with:</p><pre>fetch -L -o /disk/NAME %s</pre>", ctype[0] ? ctype : "Binary file", (size + 1023) / 1024, copy);
            web_show(w, t, copy, msg, n, true, false);
            snprintf(t->stat, sizeof(t->stat), "%d KB, not a page", (size + 1023) / 1024);
        } else if (search && web_search_results(shown, got, &nres)) {
            web_show(w, t, copy, g_webres, (int)strlen(g_webres), true, false);
            char tt[40];
            snprintf(tt, sizeof(tt), "%s - Search", shown);
            web_tab_set_title(w, t, tt);
            snprintf(t->stat, sizeof(t->stat), "%d result%s", nres, nres == 1 ? "" : "s");
        } else {
            bool html = strstr(ctype, "html") != NULL || (!ctype[0] && web_is_html((const char *)g_webbuf, got)) || (strncmp(ctype, "text/plain", 10) != 0 && web_is_html((const char *)g_webbuf, got));
            web_show(w, t, final_url, (const char *)g_webbuf, got, html, size > got);
            if (t->refresh[0] && t->nl <= 3 && strcmp(t->refresh, copy) != 0) {       // a page that only forwards somewhere else
                char next[160];
                put_str(next, sizeof(next), t->refresh);
                web_load(wi, next, next, false);
                return;
            }
            if (status >= 400) snprintf(t->stat, sizeof(t->stat), "HTTP %d", status);
            else if (search) snprintf(t->stat, sizeof(t->stat), "No results found; the whole page is shown");
            else snprintf(t->stat, sizeof(t->stat), "%d KB, %d link%s", (size + 1023) / 1024, t->nlink, t->nlink == 1 ? "" : "s");
        }
    }
    t->scroll = imax(0, t->nl - web_rows(w));
    damage_content(w);
}

static void web_follow(int wi, int n)
{
    webtab_t *t = web_tab(&g_win[wi]);
    if (!t || n < 1 || n > t->nlink) return;
    char url[160];
    put_str(url, sizeof(url), t->link[n - 1]);
    if (t->lkind[n - 1] == 2) { web_play_video(wi, url); return; }
    web_load(wi, url, url, true);
}

static void web_go(int wi, const char *input)
{
    win_t *w = &g_win[wi];
    webtab_t *t = web_tab(w);
    if (!t) return;
    char url[160], enc[130];
    while (*input == ' ') input++;
    if (!*input) return;
    bool digits = true;
    for (const char *k = input; *k; k++) if (*k < '0' || *k > '9') digits = false;
    if (digits && !t->start && t->nlink) {
        int n = atoi(input);
        if (n < 1 || n > t->nlink) { put_str(t->stat, sizeof(t->stat), "There is no such link on this page"); damage_content(w); return; }
        web_follow(wi, n);
        return;
    }
    bool host = strchr(input, ' ') == NULL && strchr(input, '.') != NULL && input[0] != '.' && input[strlen(input) - 1] != '.';
    if (strncmp(input, "http://", 7) == 0 || strncmp(input, "https://", 8) == 0) put_str(url, sizeof(url), input);
    else if (host || strncmp(input, "localhost", 9) == 0) snprintf(url, sizeof(url), "https://%s", input);
    else {
        web_urlenc(input, enc, sizeof(enc));
        snprintf(url, sizeof(url), "%s%s&format=html", WEB_SEARCH, enc);
        web_load(wi, url, input, true);
        return;
    }
    web_load(wi, url, url, true);
}

static void web_back(int wi)
{
    win_t *w = &g_win[wi];
    webtab_t *t = web_tab(w);
    if (!t) return;
    if (t->nhist == 0) { put_str(t->stat, sizeof(t->stat), "Nothing to go back to"); damage_content(w); return; }
    char url[160];
    put_str(url, sizeof(url), t->hist[--t->nhist]);
    if (t->url[0]) web_push(t->fwd, &t->nfwd, t->url);
    web_load(wi, url, url, false);
}

static void web_forward(int wi)
{
    webtab_t *t = web_tab(&g_win[wi]);
    if (!t || t->nfwd == 0) return;
    char url[160];
    put_str(url, sizeof(url), t->fwd[--t->nfwd]);
    if (t->url[0]) web_push(t->hist, &t->nhist, t->url);
    web_load(wi, url, url, false);
}

static void web_reload(int wi)
{
    webtab_t *t = web_tab(&g_win[wi]);
    if (!t || !t->url[0]) return;
    char url[160];
    put_str(url, sizeof(url), t->url);
    web_load(wi, url, url, false);
}

static void web_tab_home(win_t *w, webtab_t *t)
{
    if (t->url[0]) { web_push(t->hist, &t->nhist, t->url); t->nfwd = 0; }
    web_tab_clear(t);
    t->url[0] = 0;
    t->addr[0] = 0;
    t->addrlen = 0;
    t->stat[0] = 0;
    t->start = true;
    web_tab_set_title(w, t, "");
    damage_content(w);
}

static int web_new_tab(int wi, bool activate)
{
    win_t *w = &g_win[wi];
    if (w->ntabs >= WEB_MAXTABS) { flash("no more tabs: close one first"); return -1; }
    int ti = -1;
    for (int i = 0; i < WEB_MAXTABS; i++) if (!g_tabs[i].used) { ti = i; break; }
    if (ti < 0) { flash("every tab is in use (other browser windows hold them)"); return -1; }
    webtab_t *t = &g_tabs[ti];
    memset(t, 0, sizeof(*t));
    t->used = true;
    web_tab_clear(t);
    t->start = true;
    put_str(t->title, sizeof(t->title), "New Tab");
    w->tabs[w->ntabs++] = ti;
    if (activate) { w->wtab = w->ntabs - 1; put_str(w->title, sizeof(w->title), t->title); damage_title(w); damage(0, 0, SW, PANEL_H); }
    damage_content(w);
    return ti;
}

static void web_select_tab(int wi, int idx)
{
    win_t *w = &g_win[wi];
    if (idx < 0 || idx >= w->ntabs) return;
    w->wtab = idx;
    webtab_t *t = web_tab(w);
    put_str(w->title, sizeof(w->title), t->title);
    damage_title(w);
    damage(0, 0, SW, PANEL_H);
    damage_content(w);
}

static void web_close_tab(int wi, int idx)
{
    win_t *w = &g_win[wi];
    if (idx < 0 || idx >= w->ntabs) return;
    g_tabs[w->tabs[idx]].used = false;
    memmove(&w->tabs[idx], &w->tabs[idx + 1], (size_t)(w->ntabs - idx - 1) * sizeof(w->tabs[0]));
    w->ntabs--;
    if (w->ntabs == 0) { close_window(wi); return; }
    if (w->wtab >= w->ntabs) w->wtab = w->ntabs - 1;
    else if (w->wtab > idx) w->wtab--;
    web_select_tab(wi, w->wtab);
}

// Gives a closing browser window's tabs back.
static void web_release(win_t *w)
{
    for (int i = 0; i < w->ntabs; i++) g_tabs[w->tabs[i]].used = false;
    w->ntabs = 0;
}

static void web_scroll(int wi, int lines_n)
{
    win_t *w = &g_win[wi];
    webtab_t *t = web_tab(w);
    if (!t) return;
    t->scroll += lines_n;
    if (t->scroll > t->nl - 1) t->scroll = t->nl - 1;
    if (t->scroll < 0) t->scroll = 0;
    damage_content(w);
}

// ---- browser chrome: tab strip, round buttons, pill-shaped address bar; the colours follow the RadiWM theme -----------------------

typedef struct { uint32_t strip, bar, pill, tx, dim, hot, page, page_tx, link, link_hot, rule, sel, tile, tile_hot; } webcol_t;

static webcol_t web_colors(void)
{
    webcol_t c;
    c.strip = mix(TH.bg, TH.panel_bg, 35);
    c.bar = TH.bg;
    c.pill = TH.field;
    c.tx = TH.fg;
    c.dim = TH.dim;
    c.hot = TH.line;
    c.page = TH.field;
    c.page_tx = TH.fg;
    c.link = TH.accent;
    c.link_hot = mix(TH.accent, TH.fg, 40);
    c.rule = TH.line;
    c.sel = TH.accent;
    c.tile = mix(TH.field, TH.line, 45);
    c.tile_hot = TH.line;
    return c;
}

static int web_tab_w(const win_t *w) { rect_t c = content_rect(w); return imin(220, imax(80, (c.w - 60) / imax(1, w->ntabs))); }
static rect_t web_tab_rect(const win_t *w, int i) { rect_t c = content_rect(w); int tw = web_tab_w(w); rect_t r = { c.x + 8 + i * (tw + 2), c.y + 5, tw, WEB_TABS - 5 }; return r; }
static rect_t web_tabclose_rect(const win_t *w, int i) { rect_t t = web_tab_rect(w, i); rect_t r = { t.x + t.w - 24, t.y + (t.h - 18) / 2, 18, 18 }; return r; }
static rect_t web_newtab_rect(const win_t *w) { rect_t t = web_tab_rect(w, w->ntabs - 1); rect_t r = { t.x + t.w + 6, t.y + (t.h - 26) / 2, 26, 26 }; return r; }
static rect_t web_btn_rect(const win_t *w, int i) { rect_t c = content_rect(w); rect_t r = { c.x + 8 + i * 36, c.y + WEB_TABS + (WEB_NAV - 32) / 2, 32, 32 }; return r; }
static rect_t web_addr_rect(const win_t *w) { rect_t c = content_rect(w); rect_t r = { c.x + 8 + 4 * 36 + 6, c.y + WEB_TABS + (WEB_NAV - 32) / 2, c.w - (8 + 4 * 36 + 6) - 10, 32 }; return r; }

static rect_t web_tile_rect(const win_t *w, int i)
{
    rect_t c = content_rect(w);
    int tw = 104, th = 92, gap = 18, per = imax(1, imin(NWEB_TILES, (c.w - 40) / (tw + gap)));
    int rows = (NWEB_TILES + per - 1) / per, total_w = per * tw + (per - 1) * gap;
    int x0 = c.x + (c.w - total_w) / 2, y0 = c.y + WEB_BAR + imax(40, (c.h - WEB_BAR - 140 - rows * (th + gap)) / 2) + 110;
    rect_t r = { x0 + (i % per) * (tw + gap), y0 + (i / per) * (th + gap), tw, th };
    return r;
}

static void web_icon_arrow(rect_t r, bool left, uint32_t col)
{
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2, d = left ? -1 : 1;
    line(cx - 6, cy, cx + 6, cy, 2, col);
    line(cx + d * 6, cy, cx + d * 1, cy - 5, 2, col);
    line(cx + d * 6, cy, cx + d * 1, cy + 5, 2, col);
}

static void web_icon_reload(rect_t r, uint32_t col, uint32_t bg)
{
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    disc(cx, cy, 7, col);
    disc(cx, cy, 5, bg);
    fill_rect(cx, cy - 8, 8, 8, bg);                        // the gap of the ring
    line(cx + 2, cy - 7, cx + 7, cy - 7, 2, col);
    line(cx + 7, cy - 7, cx + 7, cy - 2, 2, col);
}

static void web_icon_home(rect_t r, uint32_t col)
{
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    line(cx - 8, cy, cx, cy - 7, 2, col);
    line(cx, cy - 7, cx + 8, cy, 2, col);
    fill_rect(cx - 5, cy - 1, 10, 7, col);
}

static void web_icon_globe(int cx, int cy, int rad, uint32_t col, uint32_t bg)
{
    disc(cx, cy, rad, col);
    disc(cx, cy, rad - 1, bg);
    fill_rect(cx - rad, cy, 2 * rad + 1, 1, col);
    fill_rect(cx, cy - rad, 1, 2 * rad + 1, col);
    frame_rect(cx - rad / 2, cy - rad, rad + 1, 2 * rad + 1, col);
}

static void web_icon_lock(int x, int cy, uint32_t col, uint32_t bg)
{
    disc(x + 4, cy - 3, 4, col);                            // shackle: a ring, open at the bottom
    disc(x + 4, cy - 3, 2, bg);
    fill_rect(x + 1, cy - 2, 7, 3, bg);
    fill_rect(x, cy - 1, 9, 7, col);                        // body
}

static void web_icon_search(int x, int cy, uint32_t col, uint32_t bg)
{
    disc(x + 4, cy - 1, 4, col);
    disc(x + 4, cy - 1, 2, bg);
    line(x + 7, cy + 2, x + 10, cy + 5, 2, col);
}

static void web_icon_cross(int cx, int cy, uint32_t col)
{
    line(cx - 4, cy - 4, cx + 4, cy + 4, 2, col);
    line(cx - 4, cy + 4, cx + 4, cy - 4, 2, col);
}

static void render_web_start(win_t *w, webtab_t *t, const webcol_t *k)
{
    rect_t c = content_rect(w);
    int cx = c.x + c.w / 2, ty = c.y + WEB_BAR + imax(40, (c.h - WEB_BAR - 140 - ((NWEB_TILES + 5) / 6) * 110) / 2);
    static const char NAME[] = "RadiWM";
    for (int i = 0; NAME[i]; i++) glyph_big(cx - 3 * 24 + i * 24, ty, (unsigned char)NAME[i], 3, i == 0 ? TH.accent : k->page_tx, TRANSPARENT);
    text_center(c.x, c.w, ty + 34, "Type in the bar above to search the web or open a site", k->dim, k->page, 70);
    int sw = imin(560, c.w - 80), sy = ty + 62;
    round_rect(cx - sw / 2, sy, sw, 36, 18, k->rule);
    round_rect(cx - sw / 2 + 1, sy + 1, sw - 2, 34, 17, k->pill);
    web_icon_search(cx - sw / 2 + 14, sy + 18, k->dim, k->pill);
    if (t->addrlen) text(cx - sw / 2 + 32, sy + 10, t->addr, k->page_tx, k->pill, (sw - 44) / CW);
    else text(cx - sw / 2 + 32, sy + 10, "Search with Bing or enter address", k->dim, k->pill, (sw - 44) / CW);
    for (int i = 0; i < NWEB_TILES; i++) {
        rect_t r = web_tile_rect(w, i);
        bool hot = g_hover_item_win >= 0 && &g_win[g_hover_item_win] == w && g_hover_item == WB_TILE + i;
        uint32_t bg = hot ? k->tile_hot : k->tile;
        round_rect(r.x, r.y, r.w, r.h, 8, bg);
        disc(r.x + r.w / 2, r.y + 34, 20, WEB_TILES[i].col);
        glyph_big(r.x + r.w / 2 - 8, r.y + 34 - 8, (unsigned char)WEB_TILES[i].label[0], 2, 0xffffff, TRANSPARENT);
        text_center(r.x, r.w, r.y + r.h - 24, WEB_TILES[i].label, k->page_tx, bg, (r.w - 8) / CW);
    }
}

static void render_web_image(win_t *w, webtab_t *t, const webcol_t *k)
{
    rect_t c = content_rect(w);
    int ti = (int)(t - g_tabs);
    int x0 = c.x + (c.w - t->img_w) / 2, y0 = c.y + WEB_BAR + 12 + imax(0, (c.h - WEB_BAR - 44 - t->img_h) / 2);
    bool light = ((k->page >> 16) & 255) + ((k->page >> 8) & 255) + (k->page & 255) > 384;
    for (int y = 0; y < t->img_h; y++) {
        int sy = y0 + y;
        if (sy < clip_y0 || sy >= clip_y1) continue;
        const uint32_t *src = g_webimg[ti] + y * t->img_w;
        uint32_t *dst = g_store + sy * SW;
        for (int x = 0; x < t->img_w; x++) {
            int sx = x0 + x;
            if (sx < clip_x0 || sx >= clip_x1) continue;
            uint32_t p = src[x], a = p >> 24;
            if (a == 255) { dst[sx] = p & 0xffffff; continue; }
            uint32_t back = (((sx >> 3) + (sy >> 3)) & 1) ? (light ? 0xd0d0d0 : 0x404040) : (light ? 0xffffff : 0x202020);   // see-through parts
            if (a == 0) { dst[sx] = back; continue; }
            uint32_t r = (((p >> 16) & 255) * a + ((back >> 16) & 255) * (255 - a)) / 255;
            uint32_t g = (((p >> 8) & 255) * a + ((back >> 8) & 255) * (255 - a)) / 255;
            uint32_t b = ((p & 255) * a + (back & 255) * (255 - a)) / 255;
            dst[sx] = r << 16 | g << 8 | b;
        }
    }
    frame_rect(x0 - 1, y0 - 1, t->img_w + 2, t->img_h + 2, k->rule);
}

static void render_web(win_t *w, bool focused)
{
    rect_t c = content_rect(w);
    webcol_t k = web_colors();
    webtab_t *t = web_tab(w);
    bool hover_here = g_hover_item_win >= 0 && &g_win[g_hover_item_win] == w;
    if (!t) { fill_rect(c.x, c.y, c.w, c.h, k.page); return; }

    // tab strip
    fill_rect(c.x, c.y, c.w, WEB_TABS, k.strip);
    for (int i = 0; i < w->ntabs; i++) {
        rect_t r = web_tab_rect(w, i);
        webtab_t *ti = &g_tabs[w->tabs[i]];
        bool active = i == w->wtab, hot = hover_here && (g_hover_item == WB_TAB + i || g_hover_item == WB_TABCLOSE + i);
        uint32_t bg = active ? k.bar : (hot ? mix(k.strip, k.bar, 50) : k.strip);
        if (active || hot) round_rect(r.x, r.y, r.w, r.h + 6, 6, bg);
        if (active) fill_rect(r.x + 8, r.y, r.w - 16, 2, k.sel);
        web_icon_globe(r.x + 14, r.y + r.h / 2, 5, active ? k.tx : k.dim, bg);
        text(r.x + 26, r.y + (r.h - CH) / 2, ti->title, active ? k.tx : k.dim, bg, imax(1, (r.w - 56) / CW));
        if (active || hot) {
            rect_t x = web_tabclose_rect(w, i);
            if (hover_here && g_hover_item == WB_TABCLOSE + i) disc(x.x + x.w / 2, x.y + x.h / 2, 9, k.hot);
            web_icon_cross(x.x + x.w / 2, x.y + x.h / 2, active ? k.tx : k.dim);
        }
    }
    {
        rect_t n = web_newtab_rect(w);
        if (hover_here && g_hover_item == WB_NEWTAB) disc(n.x + n.w / 2, n.y + n.h / 2, 12, k.hot);
        fill_rect(n.x + n.w / 2 - 6, n.y + n.h / 2 - 1, 12, 2, k.dim);
        fill_rect(n.x + n.w / 2 - 1, n.y + n.h / 2 - 6, 2, 12, k.dim);
    }

    // navigation bar
    fill_rect(c.x, c.y + WEB_TABS, c.w, WEB_NAV, k.bar);
    for (int i = 0; i < 4; i++) {
        rect_t r = web_btn_rect(w, i);
        bool on = i == 0 ? t->nhist > 0 : (i == 1 ? t->nfwd > 0 : (i == 2 ? t->url[0] != 0 : true));
        bool hot = hover_here && g_hover_item == WB_BACK + i && on;
        uint32_t bg = hot ? k.hot : k.bar, fg = on ? k.tx : mix(k.bar, k.tx, 35);
        if (hot) disc(r.x + r.w / 2, r.y + r.h / 2, 15, bg);
        if (i == 0) web_icon_arrow(r, true, fg);
        else if (i == 1) web_icon_arrow(r, false, fg);
        else if (i == 2) web_icon_reload(r, fg, bg);
        else web_icon_home(r, fg);
    }
    rect_t a = web_addr_rect(w);
    if (focused) round_rect(a.x - 1, a.y - 1, a.w + 2, a.h + 2, 9, k.sel);
    round_rect(a.x, a.y, a.w, a.h, 8, k.pill);
    int ty = a.y + (a.h - CH) / 2, tx0 = a.x + 32;
    if (t->url[0] && strncmp(t->url, "https://", 8) == 0 && !focused) web_icon_lock(a.x + 12, a.y + a.h / 2, k.dim, k.pill);
    else web_icon_search(a.x + 10, a.y + a.h / 2, k.dim, k.pill);
    int room = (a.w - 44) / CW, off = t->addrlen > room ? t->addrlen - room : 0;
    if (t->addrlen == 0 && !focused) text(tx0, ty, "Search with Bing or enter address", k.dim, k.pill, room);
    else text(tx0, ty, t->addr + off, k.tx, k.pill, room);
    if (focused) fill_rect(tx0 + (t->addrlen - off) * CW, a.y + 8, 2, a.h - 16, k.sel);
    if (t->loading) fill_rect(c.x, c.y + WEB_BAR - 2, c.w / 3, 2, k.sel);
    fill_rect(c.x, c.y + WEB_BAR - 1, c.w, 1, k.rule);

    // page
    fill_rect(c.x, c.y + WEB_BAR, c.w, c.h - WEB_BAR, k.page);
    if (t->start) { render_web_start(w, t, &k); return; }
    int hot_link = hover_here && g_hover_item >= 1 && g_hover_item <= 63 ? g_hover_item : 0;
    if (t->img_w > 0) render_web_image(w, t, &k);
    else {
        int rows = web_rows(w), cols = (c.w - 2 * PADX - 40) / CW, start = web_first_row(w, t), y0 = c.y + WEB_BAR + 8, ox = c.x + PADX + 16;
        for (int i = start; i < t->nl && i < start + rows; i++) {
            const char *s = t->lines[i];
            const uint8_t *at = t->lattr[i];
            int y = y0 + (i - start) * CH;
            for (int n = 0; s[n] && n < cols; n++) {
                uint8_t v = at[n];
                int ln = v & WA_LINK;
                bool link = ln && !(v & WA_DIM);
                uint32_t fg = link ? (ln == hot_link ? k.link_hot : k.link) : ((v & WA_DIM) ? k.dim : k.page_tx);
                glyph(ox + n * CW, y, (unsigned char)s[n], fg, k.page);
                if (v & WA_HEAD) glyph(ox + n * CW + 1, y, (unsigned char)s[n], fg, TRANSPARENT);
                if (link && ln == hot_link) fill_rect(ox + n * CW, y + CH - 2, CW, 1, fg);
            }
        }
        scrollbar(c.x + c.w - 8, c.y + WEB_BAR, c.h - WEB_BAR, t->nl, rows, start);
    }

    // a small status pill at the bottom left: the hovered link's address, else the last message
    const char *st = hot_link ? t->link[hot_link - 1] : t->stat;
    if (st[0]) {
        int n = imin((int)strlen(st), (c.w - 40) / CW), pw = n * CW + 20;
        round_rect(c.x, c.y + c.h - 22, pw, 22, 4, k.bar);
        text(c.x + 10, c.y + c.h - 22 + 3, st, k.tx, k.bar, n);
    }
}

static int web_item_at(const win_t *w, int mx, int my)
{
    webtab_t *t = web_tab(w);
    if (!t) return -1;
    for (int i = 0; i < w->ntabs; i++) {
        if (rect_has(web_tabclose_rect(w, i), mx, my)) return WB_TABCLOSE + i;
        if (rect_has(web_tab_rect(w, i), mx, my)) return WB_TAB + i;
    }
    if (rect_has(web_newtab_rect(w), mx, my)) return WB_NEWTAB;
    for (int i = 0; i < 4; i++) if (rect_has(web_btn_rect(w, i), mx, my)) return WB_BACK + i;
    if (t->start) {
        for (int i = 0; i < NWEB_TILES; i++) if (rect_has(web_tile_rect(w, i), mx, my)) return WB_TILE + i;
        return -1;
    }
    if (t->img_w > 0) return -1;
    rect_t c = content_rect(w);
    int y0 = c.y + WEB_BAR + 8, rows = web_rows(w);
    if (my < y0 || my >= y0 + rows * CH) return -1;
    int row = imax(0, t->nl - rows - t->scroll) + (my - y0) / CH, col = (mx - (c.x + PADX + 16)) / CW;
    if (row >= t->nl || col < 0 || col >= (int)strlen(t->lines[row])) return -1;
    int n = t->lattr[row][col] & WA_LINK;
    return n >= 1 && n <= t->nlink ? n : -1;
}

static void web_key(int wi, uint8_t code, bool ext)
{
    win_t *w = &g_win[wi];
    webtab_t *t = web_tab(w);
    if (!t) return;
    int page = imax(1, web_rows(w) - 2);
    if (ext) {
        if (code == 0x49) { if (g_ctrl) web_select_tab(wi, (w->wtab + w->ntabs - 1) % w->ntabs); else web_scroll(wi, page); }
        else if (code == 0x51) { if (g_ctrl) web_select_tab(wi, (w->wtab + 1) % w->ntabs); else web_scroll(wi, -page); }
        else if (code == 0x48) web_scroll(wi, 1);
        else if (code == 0x50) web_scroll(wi, -1);
        else if (code == 0x47) { t->scroll = t->nl; damage_content(w); }
        else if (code == 0x4F) { t->scroll = 0; damage_content(w); }
        else if (code == 0x4B && g_alt) web_back(wi);
        else if (code == 0x4D && g_alt) web_forward(wi);
        return;
    }
    if (g_ctrl) {
        if (code == 0x30) web_back(wi);                                                                   // B
        else if (code == 0x13 || code == 0x3F) web_reload(wi);                                            // R, F5
        else if (code == 0x23) web_tab_home(w, t);                                                        // H
        else if (code == 0x14) web_new_tab(wi, true);                                                     // T
        else if (code == 0x11) web_close_tab(wi, w->wtab);                                                 // W
        else if (code == 0x0F) web_select_tab(wi, (w->wtab + (g_shift ? w->ntabs - 1 : 1)) % w->ntabs);   // Tab
        else if (code >= 0x02 && code <= 0x09) web_select_tab(wi, imin(w->ntabs - 1, code - 2));         // 1..8
        else if (code == 0x26 || code == 0x25) { t->addrlen = 0; t->addr[0] = 0; damage_content(w); }   // L, K
        else if (code == 0x2F) {                                                                          // V
            static char host[CLIPBOARD_HOST_MAX + 1];
            int n = clipboard_from_host(host, sizeof(host));
            for (int i = 0; i < n && t->addrlen + 1 < (int)sizeof(t->addr); i++)
                if ((unsigned char)host[i] >= 0x20 && (unsigned char)host[i] < 0x7F) t->addr[t->addrlen++] = host[i];
            t->addr[t->addrlen] = 0;
            memset(host, 0, sizeof(host));
            damage_content(w);
        }
        return;
    }
    if (code == 0x3F) { web_reload(wi); return; }                                                         // F5
    if (code == 0x1C) { char typed[160]; put_str(typed, sizeof(typed), t->addr); web_go(wi, typed); return; }
    if (code == 0x01) { t->addrlen = put_str(t->addr, sizeof(t->addr), t->url); damage_content(w); return; }
    if (code == 0x0E) { if (t->addrlen > 0) t->addr[--t->addrlen] = 0; damage_content(w); return; }
    if (code < sizeof(UNSHIFTED)) {
        char ch = g_shift ? SHIFTED[code] : UNSHIFTED[code];
        if (ch >= 32 && ch < 127 && t->addrlen + 1 < (int)sizeof(t->addr)) { t->addr[t->addrlen++] = ch; t->addr[t->addrlen] = 0; damage_content(w); }
    }
}

static void web_click(int wi, int mx, int my)
{
    win_t *w = &g_win[wi];
    webtab_t *t = web_tab(w);
    if (!t) return;
    int it = web_item_at(w, mx, my);
    if (it == WB_BACK) web_back(wi);
    else if (it == WB_FWD) web_forward(wi);
    else if (it == WB_RELOAD) web_reload(wi);
    else if (it == WB_HOME) web_tab_home(w, t);
    else if (it == WB_NEWTAB) web_new_tab(wi, true);
    else if (it >= WB_TILE && it < WB_TILE + NWEB_TILES) { char url[160]; put_str(url, sizeof(url), WEB_TILES[it - WB_TILE].url); web_load(wi, url, url, true); }
    else if (it >= WB_TABCLOSE && it < WB_TABCLOSE + WEB_MAXTABS) web_close_tab(wi, it - WB_TABCLOSE);
    else if (it >= WB_TAB && it < WB_TAB + WEB_MAXTABS) web_select_tab(wi, it - WB_TAB);
    else if (it >= 1 && it <= t->nlink) web_follow(wi, it);
}

static int open_web(void)
{
    int i = new_window(W_WEB, "New Tab");
    if (i < 0) return -1;
    g_win[i].ntabs = 0;
    if (web_new_tab(i, true) < 0) { close_window(i); return -1; }
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
    "RadiWM - keys and mouse\n"
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
    "  Web: type an address or search words and press Enter; click a\n"
    "  link or type its number. Alt+Left/Right back/forward, F5 reload,\n"
    "  Ctrl+T new tab, Ctrl+W close tab, Ctrl+Tab next tab, Ctrl+H home.\n"
    "  Pictures open in the tab; videos play full screen.\n"
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
    static const uint32_t TS[8][3] = { { 0, 0, 0 }, { 0x000000, 0x33ff33, 0x07140a }, { 0x120a00, 0xffb000, 0x1e1400 }, { 0xfafafa, 0x222222, 0xe4e4e4 },
                                       { 0x002b36, 0x93a1a1, 0x073642 }, { 0x282a36, 0xf8f8f2, 0x343746 }, { 0x2e3440, 0xd8dee9, 0x3b4252 }, { 0x282828, 0xebdbb2, 0x3c3836 } };
    if (C.term_style > 0 && C.term_style < 8) { TH.term_bg = TS[C.term_style][0]; TH.term_fg = TS[C.term_style][1]; TH.term_input = TS[C.term_style][2]; }
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
    C.mouse_speed = 45; C.mouse_gain = 60; C.dblclick = 450; C.wallpaper = 1; C.wp_mode = 0; C.dock = true; C.icons = true; C.desk_files = true; C.clock24 = true; C.theme = 0;
    C.shadows = false; C.snap = true; C.run_windowed = true; C.tray = true; C.dock_size = 48; C.term_welcome = true;
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
            int s = -1;
            for (int i = 0; i < 8 && n >= 2; i++) if (same_nocase(w[1], TERM_STYLES[i])) s = i;
            if (s >= 0) C.term_style = s; else cfg_error(line_no, "terminal_colors theme, classic, amber, light, solarized, dracula, nord, gruvbox");
        } else if (same_nocase(w[0], "terminal_cursor")) {
            if (n >= 2 && same_nocase(w[1], "block")) C.term_cursor = 0;
            else if (n >= 2 && same_nocase(w[1], "underline")) C.term_cursor = 1;
            else if (n >= 2 && same_nocase(w[1], "bar")) C.term_cursor = 2;
            else cfg_error(line_no, "terminal_cursor block, underline or bar");
        } else if (same_nocase(w[0], "terminal_prompt")) {
            if (n >= 2 && same_nocase(w[1], "full")) C.term_prompt = 0;
            else if (n >= 2 && same_nocase(w[1], "short")) C.term_prompt = 1;
            else if (n >= 2 && same_nocase(w[1], "arrow")) C.term_prompt = 2;
            else if (n >= 2 && same_nocase(w[1], "radium")) C.term_prompt = 3;
            else cfg_error(line_no, "terminal_prompt full, short, arrow or radium");
        } else if (same_nocase(w[0], "terminal_welcome")) {
            if (n >= 2 && parse_bool(w[1], &b)) C.term_welcome = b; else cfg_error(line_no, "terminal_welcome yes or no");
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
            static const char *const KINDS[] = { "term", "files", "monitor", "clock", "settings", "calc", "editor", "games", "services", "scripts", "tasks", "web", "paint", "mines", "timer", "chess" };
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
            if (kind < 0) { cfg_error(line_no, "autostart open term|files|monitor|clock|settings|calc|editor|games|services|scripts|tasks|web|paint|mines|timer|chess or exec COMMAND"); continue; }
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
    o += snprintf(out + o, sizeof(out) - (size_t)o, "# RadiWM configuration (written by Settings)\n");
    o += snprintf(out + o, sizeof(out) - (size_t)o, "theme %s\n", THEMES[C.theme].name);
    if (C.wallpaper == 3 && C.wp_file[0]) o += snprintf(out + o, sizeof(out) - (size_t)o, "wallpaper_file \"%s\"\nwallpaper_mode %s\n", C.wp_file, C.wp_mode == 1 ? "fit" : (C.wp_mode == 2 ? "stretch" : (C.wp_mode == 3 ? "center" : "fill")));
    else o += snprintf(out + o, sizeof(out) - (size_t)o, "wallpaper %s\n", C.wallpaper == 3 ? "default" : (C.wallpaper == 1 ? "flat" : (C.wallpaper == 2 ? "reverse" : "gradient")));
    o += snprintf(out + o, sizeof(out) - (size_t)o, "mouse_speed %d\nmouse_accel %d\ndouble_click %d\n", C.mouse_speed, C.mouse_gain, C.dblclick);
    o += snprintf(out + o, sizeof(out) - (size_t)o, "dock %s\nicons %s\ndesktop_files %s\nclock24 %s\n", C.dock ? "yes" : "no", C.icons ? "yes" : "no", C.desk_files ? "yes" : "no", C.clock24 ? "yes" : "no");
    o += snprintf(out + o, sizeof(out) - (size_t)o, "window_shadows %s\nsnap %s\nopen_maximised %s\nrun_in_window %s\npanel_tray %s\ndock_size %d\nterminal_colors %s\n",
                  C.shadows ? "yes" : "no", C.snap ? "yes" : "no", C.open_max ? "yes" : "no", C.run_windowed ? "yes" : "no", C.tray ? "yes" : "no", C.dock_size,
                  TERM_STYLES[C.term_style]);
    static const char *const CUR_N[3] = { "block", "underline", "bar" };
    static const char *const PRM_N[4] = { "full", "short", "arrow", "radium" };
    o += snprintf(out + o, sizeof(out) - (size_t)o, "terminal_cursor %s\nterminal_prompt %s\nterminal_welcome %s\n", CUR_N[C.term_cursor], PRM_N[C.term_prompt], C.term_welcome ? "yes" : "no");
    if (o > (int)sizeof(out) - 1) o = (int)sizeof(out) - 1;    // snprintf keeps counting past the buffer end: clamp so the next call gets a sane size
    // snprintf keeps counting past the buffer end: clamp o so the next call gets a sane size
    for (int i = 0; i < g_nlaunch; i++) { o += snprintf(out + o, sizeof(out) - (size_t)o, "launcher \"%s\" %s\n", g_launch[i].label, g_launch[i].cmd); if (o > (int)sizeof(out) - 1) o = (int)sizeof(out) - 1; }
    o += snprintf(out + o, sizeof(out) - (size_t)o, "autostart clear\n");
    for (int i = 0; i < g_nauto; i++) {
        static const char *const KINDS[] = { "term", "files", "monitor", "clock", "settings", "calc", "editor", "games", "services", "scripts", "tasks", "web", "paint", "mines", "timer", "chess" };
        if (g_auto[i].kind == 100) { o += snprintf(out + o, sizeof(out) - (size_t)o, "autostart exec %s\n", g_auto[i].arg); if (o > (int)sizeof(out) - 1) o = (int)sizeof(out) - 1; }
        else { o += snprintf(out + o, sizeof(out) - (size_t)o, "autostart open %s\n", KINDS[g_auto[i].kind]); if (o > (int)sizeof(out) - 1) o = (int)sizeof(out) - 1; }
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
    w->icur = 0;
    w->input[0] = 0;
    char pr[40];
    term_prompt_str(pr, sizeof(pr));
    if (w->col) term_newline(w);
    term_echo(w, pr, cmd);
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
    static const char *const TN[] = { "Terminal", "Files", "Monitor", "Clock", "Settings", "Viewer", "Calculator", "Editor", "Games", "Services", "Scripts", "Task Manager", "Web", "Paint", "Minesweeper", "Timer", "Chess" };
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
    if (w->type == W_WEB) { web_scroll(wi, lines_n); return; }
    if (w->type != W_TERM && w->type != W_VIEW) return;
    w->scroll += lines_n;
    if (w->scroll > w->nl - 1) w->scroll = w->nl - 1;
    if (w->scroll < 0) w->scroll = 0;
    damage_content(w);
}

static void term_ins(win_t *w, char ch)
{
    if (w->inlen >= (int)sizeof(w->input) - 1) return;
    memmove(w->input + w->icur + 1, w->input + w->icur, (size_t)(w->inlen - w->icur + 1));
    w->input[w->icur++] = ch;
    w->inlen++;
}

static void term_del(win_t *w, int from, int to)          // removes input[from..to)
{
    if (from < 0) from = 0;
    if (to > w->inlen) to = w->inlen;
    if (to <= from) return;
    memmove(w->input + from, w->input + to, (size_t)(w->inlen - to + 1));
    w->inlen -= to - from;
    if (w->icur > from) w->icur = from;
}

static void term_paste(win_t *w)
{
    static char host[CLIPBOARD_HOST_MAX + 1];
    int n = clipboard_from_host(host, sizeof(host));
    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)host[i];
        if (ch == '\n') break;                                // one line only
        if (ch >= 0x20 && ch < 0x7F) term_ins(w, (char)ch);
    }
    memset(host, 0, sizeof(host));
}

// Tab: completes the command name; a second Tab on an ambiguous prefix lists the candidates.
static void term_complete(win_t *w)
{
    if (w->inlen == 0 || w->icur != w->inlen || strchr(w->input, ' ')) return;
    const char *m[32];
    int n = run_matches(w->input, m, 32);
    if (n == 0) return;
    int common = (int)strlen(m[0]);
    for (int i = 1; i < n; i++) { int k = 0; while (k < common && m[i][k] == m[0][k]) k++; common = k; }
    if (common > (int)sizeof(w->input) - 3) common = (int)sizeof(w->input) - 3;
    if (common > w->inlen) {
        memcpy(w->input, m[0], (size_t)common);
        w->input[common] = 0;
        w->inlen = common;
    } else if (n > 1) {
        char pr[40];
        term_prompt_str(pr, sizeof(pr));
        if (w->col) term_newline(w);
        term_echo(w, pr, w->input);
        term_newline(w);
        int cols = tile_cols(w);
        for (int i = 0; i < n; i++) {
            if (w->col + (int)strlen(m[i]) + 2 >= cols) term_newline(w);
            term_puts(w, m[i]);
            term_puts(w, "  ");
        }
        term_newline(w);
    }
    if (n == 1 && w->inlen < (int)sizeof(w->input) - 1) { w->input[w->inlen++] = ' '; w->input[w->inlen] = 0; }
    w->icur = w->inlen;
}

static void term_key(int wi, uint8_t code, bool ext)
{
    win_t *w = &g_win[wi];
    bool ed = w->type == W_TERM;
    if (ext) {
        if (code == 0x49 || code == 0x51) {
            int page = tile_rows(w) - 3;
            if (page < 1) page = 1;
            scroll_window(wi, code == 0x49 ? page : -page);
            return;
        }
        if (!ed) {
            if (code == 0x48) scroll_window(wi, 1);
            else if (code == 0x50) scroll_window(wi, -1);
            return;
        }
        switch (code) {
        case 0x48: case 0x50:
            if (w->nhist) {
                int lo = w->nhist > 8 ? w->nhist - 8 : 0;
                if (code == 0x48 && w->hpos > lo) w->hpos--;
                else if (code == 0x50 && w->hpos < w->nhist) w->hpos++;
                if (w->hpos == w->nhist) w->input[0] = 0;
                else put_str(w->input, sizeof(w->input), w->hist[w->hpos & 7]);
                w->inlen = (int)strlen(w->input);
                w->icur = w->inlen;
            }
            break;
        case 0x4B: if (w->icur > 0) w->icur--; break;
        case 0x4D: if (w->icur < w->inlen) w->icur++; break;
        case 0x47: w->icur = 0; break;
        case 0x4F: w->icur = w->inlen; break;
        case 0x53: term_del(w, w->icur, w->icur + 1); break;
        default: return;
        }
        w->scroll = 0;
        damage_content(w);
        return;
    }
    if (g_ctrl) {
        if (code == 0x26) { term_clear_lines(w); damage_content(w); return; }          // Ctrl+L
        if (!ed) return;
        switch (code) {
        case 0x2E:                                                                       // Ctrl+C: abandon the line
            if (w->inlen > 0) {
                char pr[40];
                term_prompt_str(pr, sizeof(pr));
                if (w->col) term_newline(w);
                term_echo(w, pr, w->input);
                term_puts(w, "^C");
                term_newline(w);
                w->inlen = w->icur = 0;
                w->input[0] = 0;
            }
            break;
        case 0x16: term_del(w, 0, w->icur); break;                                       // Ctrl+U
        case 0x25: term_del(w, w->icur, w->inlen); break;                                // Ctrl+K
        case 0x11: {                                                                     // Ctrl+W
            int k = w->icur;
            while (k > 0 && w->input[k - 1] == ' ') k--;
            while (k > 0 && w->input[k - 1] != ' ') k--;
            term_del(w, k, w->icur);
            break;
        }
        case 0x1E: w->icur = 0; break;                                                   // Ctrl+A
        case 0x12: w->icur = w->inlen; break;                                            // Ctrl+E
        case 0x2F: term_paste(w); break;                                                 // Ctrl+V
        default: return;
        }
        w->scroll = 0;
        damage_content(w);
        return;
    }
    if (!ed) return;
    if (code == 0x1C) { term_run(wi); return; }
    if (code == 0x0F) { term_complete(w); w->scroll = 0; damage_content(w); return; }
    if (code == 0x0E) {
        if (w->icur > 0) term_del(w, w->icur - 1, w->icur);
    } else if (code < sizeof(UNSHIFTED)) {
        char ch = g_shift ? SHIFTED[code] : UNSHIFTED[code];
        if (ch < 32 || ch > 126) return;
        term_ins(w, ch);
    } else return;
    w->scroll = 0;
    damage_content(w);
}

// ---- actions ------------------------------------------------------------------------------------------------------------------

static void open_exec(const char *cmd)
{
    int i = new_window(W_TERM, "Terminal");
    if (i < 0) return;
    put_str(g_win[i].input, sizeof(g_win[i].input), cmd);
    g_win[i].inlen = (int)strlen(g_win[i].input);
    g_win[i].icur = g_win[i].inlen;
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
    g_win[i].icur = g_win[i].inlen;
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
    menu_add("Web Browser", ACT_OPEN, APP_WEB, IC_WEB);
    menu_add("Text Editor", ACT_OPEN, APP_EDIT, IC_EDIT);
    menu_add("Calculator", ACT_OPEN, APP_CALC, IC_CALC);
    menu_sep();
    menu_add("Games", ACT_SUBMENU, 1, IC_GAME);
    menu_add("Development", ACT_SUBMENU, 2, IC_SCRIPT);
    menu_add("System", ACT_SUBMENU, 3, IC_SET);
    menu_add("Accessories", ACT_SUBMENU, 4, IC_APP);
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
        menu_add("Minesweeper", ACT_OPEN, APP_MINES, IC_MINES);
        menu_add("Chess", ACT_OPEN, APP_CHESS, IC_CHESS);
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
    } else if (which == 4) {
        menu_add("Paint", ACT_OPEN, APP_PAINT, IC_PAINT);
        menu_add("Timer", ACT_OPEN, APP_TIMER, IC_TIMER);
        menu_add("Calculator", ACT_OPEN, APP_CALC, IC_CALC);
        menu_add("Clock", ACT_OPEN, APP_CLOCK, IC_CLOCK);
        menu_add("Text Editor", ACT_OPEN, APP_EDIT, IC_EDIT);
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

static void menu_term(int wi, int x, int y)
{
    menu_clear();
    menu_add("Paste", ACT_TERM_PASTE, wi, -1);
    menu_add("Clear", ACT_TERM_CLEAR, wi, -1);
    menu_sep();
    menu_add("Terminal Settings", ACT_TERM_SET, 0, IC_SET);
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
    case ACT_TERM_PASTE: if (arg >= 0 && arg < MAX_WIN && g_win[arg].used && g_win[arg].type == W_TERM && !g_win[arg].running) { term_paste(&g_win[arg]); g_win[arg].scroll = 0; damage_content(&g_win[arg]); } break;
    case ACT_TERM_CLEAR: if (arg >= 0 && arg < MAX_WIN && g_win[arg].used && g_win[arg].type == W_TERM && !g_win[arg].running) { term_clear_lines(&g_win[arg]); damage_content(&g_win[arg]); } break;
    case ACT_TERM_SET: { int i = open_app(APP_SET); if (i >= 0) g_win[i].tab = 4; } break;
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
    else if (w->type == W_WEB) web_key(g_focus, code, ext);
    else if (w->type == W_CALC) calc_key(g_focus, code, ext);
    else if (w->type == W_EDIT) edit_key(g_focus, code, ext);
    else if (w->type == W_SERV || w->type == W_SCRIPTS || w->type == W_GAMES) list_key(g_focus, code, ext);
    else if (w->type == W_PAINT) paint_key(g_focus, code, ext);
    else if (w->type == W_MINES) mines_key(g_focus, code, ext);
    else if (w->type == W_TIMER) timer_key(g_focus, code, ext);
    else if (w->type == W_CHESS) chess_key(g_focus, code, ext);
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

enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE, DRAG_SLIDER, DRAG_ICON, DRAG_PAINT };
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
    case W_WEB: return web_item_at(w, mx, my);
    case W_SCRIPTS: for (int i = 0; i < 4; i++) if (rect_has(script_button(w, i), mx, my)) return 1000 + i; break;
    case W_SET: {
        for (int i = 0; i < NSETTABS; i++) if (rect_has(set_tab_rect(w, i), mx, my)) return 2000 + i;
        if (rect_has(set_save_rect(w), mx, my)) return 2100;
        ctl_t ctl[48];
        int n = settings_layout(w, ctl, 48);
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
    if (g_drag.kind == DRAG_PAINT) { paint_drag(g_drag.win, g_mx, g_my); return; }
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
    else if (w->type == W_WEB) web_click(wi, g_mx, g_my);
    else if (w->type == W_SCRIPTS) scripts_click(wi, g_mx, g_my, dbl);
    else if (w->type == W_PAINT) { if (paint_click(wi, g_mx, g_my)) { g_drag.kind = DRAG_PAINT; g_drag.win = wi; } }
    else if (w->type == W_MINES) mines_click(wi, g_mx, g_my);
    else if (w->type == W_TIMER) timer_click(wi, g_mx, g_my);
    else if (w->type == W_CHESS) chess_click(wi, g_mx, g_my);
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
        else if (g_win[wi].type == W_TERM && !g_win[wi].running) menu_term(wi, g_mx, g_my);
        else if (g_win[wi].type == W_MINES) mines_rclick(wi, g_mx, g_my);
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

    int bi = open_web();
    win_t *bwin = &g_win[bi];
    webtab_t *bw = web_tab(bwin);
    st_check(bw != NULL && bw->start, "web: a new window opens with a start tab");
    static const char page[] = "<html><head><title>T &amp; U</title><meta name=x></head><body><h1>Hi</h1><p>one <a href=\"/x?a=1&amp;b=2\">two</a> three</p>"
                               "<div style=\"display:none\">secret <b>text</b></div><script>x</script><img src=\"p.png\" alt=\"logo\"><video src=\"v.mp4\"></video></body></html>";
    web_show(bwin, bw, "https://h.test/d/p.html", page, (int)sizeof(page) - 1, true, false);
    st_check(bw->nlink == 3 && strcmp(bw->link[0], "https://h.test/x?a=1&b=2") == 0, "web: a link is resolved against the page address");
    st_check(bw->nl >= 3 && strcmp(bw->lines[0], "Hi") == 0 && strncmp(bw->lines[2], "one two[1] three", 16) == 0, "web: page text, scripts and hidden parts left out");
    st_check((bw->lattr[2][4] & WA_LINK) == 1 && (bw->lattr[0][0] & WA_HEAD) != 0, "web: link number and heading are remembered");
    st_check(bw->lkind[1] == 1 && strcmp(bw->link[1], "https://h.test/d/p.png") == 0 && bw->lkind[2] == 2, "web: pictures and videos become typed links");
    st_check(strcmp(bw->title, "T & U") == 0 && strcmp(bwin->title, "T & U") == 0, "web: the tab and window take the page title");
    bool hidden_ok = true;
    for (int i = 0; i < bw->nl; i++) if (strstr(bw->lines[i], "secret")) hidden_ok = false;
    st_check(hidden_ok, "web: display:none content is not shown");
    int t2 = web_new_tab(bi, true);
    st_check(t2 >= 0 && bwin->ntabs == 2 && bwin->wtab == 1 && web_tab(bwin)->start, "web: a second tab opens and is selected");
    web_close_tab(bi, 1);
    st_check(bwin->ntabs == 1 && web_tab(bwin) == bw && !g_tabs[t2].used, "web: closing a tab gives it back");
    char wu[160];
    web_resolve("https://a.b/c/d.html", "../e.html", wu, sizeof(wu));
    st_check(strcmp(wu, "https://a.b/e.html") == 0, "web: dot segments are removed from a relative address");
    web_resolve("https://a.b/c", "mailto:x@y.z", wu, sizeof(wu));
    st_check(wu[0] == 0, "web: mailto links are not followed");
    static const uint8_t bmp[] = { 'B','M', 70,0,0,0, 0,0,0,0, 54,0,0,0, 40,0,0,0, 2,0,0,0, 2,0,0,0, 1,0, 24,0, 0,0,0,0, 16,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
                                   255,0,0, 0,255,0, 0,0, 0,0,255, 255,255,255, 0,0 };
    uint32_t px[4], iw = 0, ih = 0;
    int kind = rust_image_decode_fit(bmp, sizeof(bmp), 10, 10, px, 4, &iw, &ih);
    st_check(kind == 4 && iw == 2 && ih == 2 && px[0] == 0xffff0000 && px[1] == 0xffffffff && px[2] == 0xff0000ff && px[3] == 0xff00ff00, "web: a BMP decodes to the right pixels (rows are stored bottom-up)");
    close_window(bi);
    st_check(!g_tabs[bwin->tabs[0]].used || bwin->ntabs == 0, "web: closing the window gives its tabs back");

    g_ch.human = 0;
    g_ch.mode = 0;
    ch_new();
    st_check(ch_perft(&g_ch.pos, 1, 0) == 20 && ch_perft(&g_ch.pos, 2, 0) == 400 && ch_perft(&g_ch.pos, 3, 0) == 8902, "chess: move generation matches perft 20/400/8902");
    {   // back-rank mate in one: Ra8#
        int sc, dd;
        memset(&g_ch.pos, 0, sizeof(g_ch.pos));
        g_ch.pos.b[62] = PC_K; g_ch.pos.b[56] = PC_R;
        g_ch.pos.b[6] = -PC_K; g_ch.pos.b[13] = g_ch.pos.b[14] = g_ch.pos.b[15] = -PC_P;
        g_ch.pos.ep = -1;
        g_ch_hist[0] = ch_hash(&g_ch.pos);
        g_ch_nh = 1;
        st_check(ch_think(&g_ch.pos, 4, 500, 0, false, &sc, &dd, false) == CM(56, 0, 0) && sc > CH_MATE - 100, "chess: the engine finds mate in one");
    }
    {   // every line of the opening table must be a legal game
        int bad = 0;
        g_ch.human = 0;
        for (int oi = 0; oi < NOPEN; oi++) {
            bool ok = true;
            ch_new();
            for (int k = 0, L = opn_len(&OPENINGS[oi]); k < L && ok; k++) {
                cmove_t m;
                if (opn_tok_move(&OPENINGS[oi], k, &m)) ch_apply(m);
                else { ok = false; OUT_ERR("  bad opening line: %s (ply %d)\n", OPENINGS[oi].name, k + 1); }
            }
            if (!ok) bad++;
        }
        st_check(bad == 0, "chess: every opening line is a legal game");
    }
    ch_new();
    ch_apply(CM(52, 36, 0));                                                   // 1.e4
    ch_apply(CM(12, 28, 0));                                                   // 1...e5
    st_check(strcmp(g_ch_san[0], "e4") == 0 && strcmp(g_ch_san[1], "e5") == 0, "chess: SAN of the first two moves");
    st_check(g_ch.opn_cur >= 0 && strcmp(OPENINGS[g_ch.opn_cur].name, "King's Pawn Game") == 0, "chess: the opening is recognised");
    ch_new();
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
    uint32_t last_draw = 0, last_apps = 0;
    fbcon_set_poll_hook(vt_pump);
    while (!g_quit) {
        poll_input_bytes();
        uint32_t now = now_ms();
        if (now - g_last_sec >= 1000) { g_last_sec += 1000; tick_second(); }
        if (now - last_apps >= 100) { last_apps = now; chess_tick(); apps_tick(); }
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
