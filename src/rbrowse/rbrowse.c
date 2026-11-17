// rbrowse — 640x360 truecolor browser (Bochs/QEMU DISPI), with a VGA mode 13h
// (320x200, 256-color) fallback when DISPI is not there.
//
// Page layout, hit testing, and the small script subset live in engine.c;
// painting is paint.c. This file owns the video mode, PS/2 mouse, and
// keyboard focus.

#include "engine.h"
#include "paint.h"
#include "image.h"
#include "../vga/vga_gfx.h"
#include "../vga/bga.h"
#include "../terminal/terminal.h"
#include "../Avfs/Avfs.h"
#include "../io/io.h"
#include "../timers/timer.h"
#include "../keyboard/keyboard.h"
#include "../kernel/fault_guard.h"
#include "../scheduler/task.h"
#include "../quickjs/rjs.h"
#include "../quickjs/rjs_dom.h"

#include <stdint.h>

extern uint8_t vga_font_8x8[256][8];
extern int rbrowse_http_get(const uint8_t *url, uint8_t *out, int cap);
extern void watchdog_hud_force_redraw(void);

/* The back buffer every frame is painted into (rb_ui.w x rb_ui.h used). */
static uint32_t g_fb[RB_FB_MAX_W * RB_FB_MAX_H];
/* Mode 13h fallback: the quantized frame. */
static uint8_t g_fb8[VGA_GFX_SIZE];
/* DISPI linear framebuffer (32bpp), or 0 in the mode 13h fallback. */
static uint8_t *g_lfb;
static int g_lfb_pitch;
/* Full response, before script/style are stripped into g_page. */
static char g_fetch[1024 * 1024];
/* One external stylesheet at a time. */
static char g_css[384 * 1024];
static char g_current[384];
static char g_startbuf[384];
static char g_url[384];
static int g_url_len;
static char g_status[80];
static char g_hover[160];
static char g_hist[8][384];
static int g_hist_n;
static int g_hist_i;
static int g_scroll;
static int g_url_focus;
static int g_focus;
static int g_hover_node;   /* node under the pointer, 0 when none/hover left it */
static uint16_t g_hover_gen; /* g_hover_node's generation, so a recycled slot is not mistaken for it */
static int g_mm_x = -1;    /* last MOUSEMOVE position (-1: none dispatched yet) */
static int g_mm_y = -1;
static int g_mx = 160;
static int g_my = 100;
static int g_left;
static int g_left_was;
/* Raw PS/2 button bits from the packet: bit0 left, bit1 right, bit2 middle.
   g_left stays the left-only flag the press/drag/release flow uses; the mask
   rides along so MOUSEDOWN/MOUSEUP can carry which button actually changed. */
static uint8_t g_btn;
static uint8_t g_btn_was;
static int g_press_x, g_press_y, g_press_scroll;
static int g_moved;
static int g_press_zone;
static int g_wheel_bytes = 3;
static int g_mouse_ok;
static uint8_t g_pkt[4];
static int g_pkt_n;
static int g_shift;
static int g_ext;
/* KEYDOWN must and KEYUP must report the same character and target. The break
   code is decoded with the shift state, which has often already changed (the
   shift break routinely arrives before a letter break), so what was seen at
   make time is memoised and replayed on break. The memo is keyed by the
   (extended, scancode) pair: main '/' (0x35) and numpad '/' (E0 0x35) share a
   scancode and must not collide, so the E0 bit picks the upper half. Bounded:
   one byte plus one target per (ext,sc); ch 0 means "none pending" (a break
   without a make, e.g. after drain_ps2, reports key 0). The target is captured
   once at make time so KEYDOWN/KEYPRESS/KEYUP all fire on the same element
   even if a KEYDOWN listener moved focus. */
typedef struct { uint8_t ch; uint16_t target; } KeyMemo;
static KeyMemo g_keydn[0x100];
static int g_dirty = 1;

/* The focused node: the DOM active element, or the tab-cursor fallback.
   Defined with the input seams; paint() reads it too. */
static int focus_node(void);

static void set_status(const char *s) {
    int i = 0;
    if (!s) s = "";
    while (s[i] && i < (int)sizeof(g_status) - 1) {
        g_status[i] = s[i];
        i++;
    }
    g_status[i] = 0;
}

static int starts(const char *s, const char *lit) {
    int i = 0;
    if (!s) return 0;
    while (lit[i]) {
        if (s[i] != lit[i]) return 0;
        i++;
    }
    return 1;
}

static void copy_str(char *dst, int cap, const char *src) {
    int i = 0;
    if (!src) src = "";
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void set_url_bar(const char *s) {
    copy_str(g_url, (int)sizeof(g_url), s);
    g_url_len = (int)strlen(g_url);
}

/* Mode 13h fallback palette: chrome colours at 0..15 (rb_ui_palette), the
   6x6x6 cube at 16..231 and a gray ramp at 232..255, the layout
   rb_quantize_8bpp maps the back buffer onto. */
static void install_palette(void) {
    for (int i = 0; i < 16; i++) {
        uint32_t c = rb_ui_palette[i];
        vga_gfx_set_palette_color_rgb888((uint8_t)i, (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c);
    }
    for (int i = 16; i <= 231; i++) {
        int r, g, b;
        rb_cube_rgb(i, &r, &g, &b);
        vga_gfx_set_palette_color_rgb888((uint8_t)i, (uint8_t)r, (uint8_t)g, (uint8_t)b);
    }
    for (int i = 232; i < 256; i++) {
        int v = (i - 232) * 10;
        if (v > 255) v = 255;
        vga_gfx_set_palette_color_rgb888((uint8_t)i, (uint8_t)v, (uint8_t)v, (uint8_t)v);
    }
}

/* ---- video ------------------------------------------------------------------
   640x360x32 through DISPI when the adapter has it, else mode 13h. Mode 13h
   is entered first either way: vga_gfx_set_mode13h snapshots the text-mode
   registers, font, screen and DAC (DISPI's framebuffer shares that VRAM) and
   vga_gfx_set_text_mode puts them back on the way out. */
static void video_enter(int force_vga) {
    vga_gfx_set_mode13h();
    install_palette();
    g_lfb = 0;
    uint8_t *lfb = 0;
    int pitch = 0;
    if (!force_vga && bga_set_mode(RB_FB_MAX_W, RB_FB_MAX_H, 32, &lfb, &pitch) == 0 &&
        pitch >= RB_FB_MAX_W * 4) {
        g_lfb = lfb;
        g_lfb_pitch = pitch;
        rb_ui_layout(RB_FB_MAX_W, RB_FB_MAX_H);
    } else {
        rb_ui_layout(VGA_GFX_WIDTH, VGA_GFX_HEIGHT);
    }
    rb_set_viewport(rb_ui.view_w, rb_ui.content_h);
    g_mx = rb_ui.w / 2;
    g_my = rb_ui.h / 2;
}

static void video_leave(void) {
    if (g_lfb) bga_disable();
    g_lfb = 0;
    vga_gfx_set_text_mode();
}

static void copy_dwords(void *dst, const void *src, int n) {
    __asm__ volatile("cld; rep movsl" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
}

static void present(void) {
    if (g_lfb) {
        for (int y = 0; y < rb_ui.h; y++)
            copy_dwords(g_lfb + y * g_lfb_pitch, &g_fb[y * rb_ui.w], rb_ui.w);
        return;
    }
    rb_quantize_8bpp(g_fb, g_fb8, rb_ui.w, rb_ui.h);
    vga_gfx_present(g_fb8);
}

/* Arrow-key scroll step: two text lines of the 320x200 page, three at 640x360. */
static int line_step(void) {
    return rb_ui.content_h >= 300 ? 24 : 16;
}

static void clamp_scroll(void) {
    int maxs = rb_page_height() - rb_ui.content_h;
    if (maxs < 0) maxs = 0;
    if (g_scroll < 0) g_scroll = 0;
    if (g_scroll > maxs) g_scroll = maxs;
}

static void thumb_geom(int *ty, int *th) {
    int ph = rb_page_height();
    int ch = rb_ui.content_h;
    if (ph <= ch || ph <= 0) {
        *ty = rb_ui.content_y;
        *th = ch;
        return;
    }
    int h = ch * ch / ph;
    if (h < 10) h = 10;
    if (h > ch) h = ch;
    int travel = ch - h;
    int maxs = ph - ch;
    *th = h;
    *ty = rb_ui.content_y + (maxs ? (int)((long long)g_scroll * travel / maxs) : 0);
}

static void draw_cursor(void) {
    static const uint8_t bits[12] = {
        0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xF0, 0xD8, 0x98, 0x08, 0x18, 0x10
    };
    /* black drop shadow first, then the white arrow on top of it (drawn the
       other way round the shadow covered all but the arrow's left edge) */
    for (int pass = 0; pass < 2; pass++) {
        int ox = pass ? g_mx : g_mx + 1;
        int oy = pass ? g_my : g_my + 1;
        uint32_t col = pass ? UI_WHITE : UI_BLACK;
        for (int row = 0; row < 12; row++) {
            for (int colb = 0; colb < 8; colb++) {
                if (bits[row] & (0x80 >> colb)) rb_px(ox + colb, oy + row, col);
            }
        }
    }
}

static void paint(void) {
    const RbUi *u = &rb_ui;
    int W = u->w, H = u->h;
    rb_paint_begin(g_fb, W, H);
    rb_rect(0, 0, W, u->chrome_h, UI_CHROME, 0, 0, W, H);
    rb_rect(0, u->content_y, u->scroll_x, u->content_h, (uint32_t)rb_page_bg(), 0, 0, W, H);
    rb_rect(u->scroll_x, u->content_y, u->scroll_w, u->content_h, UI_SCROLL, 0, 0, W, H);
    rb_rect(0, u->status_y, W, u->status_h, UI_STATUS, 0, 0, W, H);
    rb_rect(0, u->chrome_h - 1, W, 1, UI_LINE, 0, 0, W, H);

    /* back button, address field, Go */
    rb_rect(u->back_x, u->btn_y, u->back_w, u->btn_h, UI_BTN, 0, 0, W, H);
    rb_text_at(u->back_x + (u->back_w - 8) / 2 + 1, u->text_y, "<", UI_BTN_TEXT, 0, 0, W, H);
    rb_rect(u->url_x, u->btn_y, u->url_w, u->btn_h, UI_URL, 0, 0, W, H);
    if (g_url_focus) rb_frame_rect(u->url_x, u->btn_y, u->url_w, u->btn_h, UI_FOCUS);
    rb_rect(u->go_x, u->btn_y, u->go_w, u->btn_h, UI_BTN, 0, 0, W, H);
    rb_text_at(u->go_x + (u->go_w - 16) / 2, u->text_y, "Go", UI_BTN_TEXT, 0, 0, W, H);

    char shown[RB_FB_MAX_W / 8 + 1];
    int maxc = (u->url_w - 8) / 8;
    if (maxc > RB_FB_MAX_W / 8) maxc = RB_FB_MAX_W / 8;
    if (maxc < 1) maxc = 1;
    int start = 0;
    if (g_url_len > maxc) start = g_url_len - maxc;
    int n = 0;
    while (g_url[start + n] && n < maxc) {
        shown[n] = g_url[start + n];
        n++;
    }
    shown[n] = 0;
    int tx = u->url_x + 4;
    rb_text_at(tx, u->text_y, shown, UI_URL_TEXT, u->url_x + 1, u->btn_y + 1,
               u->url_x + u->url_w - 2, u->btn_y + u->btn_h);
    if (g_url_focus && ((get_ticks() / 400) & 1)) {
        int cx = tx + n * 8;
        if (cx < u->url_x + u->url_w - 4) rb_rect(cx, u->text_y, 1, 8, UI_FOCUS, 0, 0, W, H);
    }

    rb_paint_page(g_scroll, g_url_focus ? 0 : focus_node());

    int ty, th;
    thumb_geom(&ty, &th);
    rb_rect(u->scroll_x + 1, ty, u->scroll_w - 2, th, UI_THUMB, 0, 0, W, H);

    int sy = u->status_y + (u->status_h - 8) / 2;
    int sx1 = g_mouse_ok ? W : W - 76;
    rb_text_at(2, sy, g_hover[0] ? g_hover : g_status, UI_STATUS_TEXT, 0, u->status_y, sx1, H);
    if (!g_mouse_ok) rb_text_at(W - 70, sy, "no mouse", UI_STATUS_TEXT, 0, u->status_y, W, H);
    draw_cursor();
    present();
}

/* ---- start-up warning ----------------------------------------------------
   Shown for RB_BANNER_MS every time the browser opens. The engine runs in
   ring 0 with paging off: a bad page can still corrupt kernel memory, and the
   fault guard only catches what the CPU itself reports. */
#define RB_BANNER_MS 2500

static void banner_triangle(int cx, int top, int h, uint32_t c) {
    for (int r = 0; r < h; r++) {
        int hw = (r * 11) / 20;   /* ~1.1 wide per row: a squat warning sign */
        rb_rect(cx - hw, top + r, hw * 2 + 1, 1, c, 0, 0, rb_ui.w, rb_ui.h);
    }
}

static void banner_center(int y, const char *s, uint32_t c, int scale) {
    int w = (int)strlen(s) * 8 * scale;
    rb_text_scaled((rb_ui.w - w) / 2, y, s, c, scale);
}

/* The card is designed at 300x156 for 320x200 and drawn at rb_ui.glyph
   times that size (2x at 640x360), centred. `filled` is 0..238 design px. */
static void banner_frame(int filled) {
    uint32_t amber = UI_FOCUS;
    uint32_t red = 0xFF6633u;
    int W = rb_ui.w, H = rb_ui.h, s = rb_ui.glyph;
    int ox = (W - 300 * s) / 2, oy = (H - 156 * s) / 2;
    rb_paint_begin(g_fb, W, H);
    rb_rect(0, 0, W, H, UI_STATUS, 0, 0, W, H);
    /* Hazard tape, top and bottom. */
    for (int y = 0; y < 8 * s; y++) {
        for (int x = 0; x < W; x++) {
            uint32_t c = (((x + y) / (8 * s)) & 1) ? amber : UI_BLACK;
            g_fb[y * W + x] = c;
            g_fb[(H - 1 - y) * W + x] = c;
        }
    }
    /* Card. */
    rb_rect(ox, oy, 300 * s, 156 * s, UI_CHROME, 0, 0, W, H);
    rb_frame_rect(ox, oy, 300 * s, 156 * s, UI_LINE);
    rb_rect(ox + s, oy + s, 298 * s, 2 * s, amber, 0, 0, W, H);
    /* Warning sign: dark rim, amber body, black '!'. */
    banner_triangle(W / 2, oy + 11 * s, 38 * s, UI_LINE);
    banner_triangle(W / 2, oy + 14 * s, 33 * s, amber);
    rb_rect(W / 2 - 2 * s, oy + 25 * s, 5 * s, 12 * s, UI_BLACK, 0, 0, W, H);
    rb_rect(W / 2 - 2 * s, oy + 40 * s, 5 * s, 4 * s, UI_BLACK, 0, 0, W, H);
    banner_center(oy + 58 * s, "UNSAFE BROWSER", red, 2 * s);
    banner_center(oy + 82 * s, "This browser is not safe.", UI_URL_TEXT, s);
    banner_center(oy + 94 * s, "Do not run it on important hardware.", UI_URL_TEXT, s);
    banner_center(oy + 112 * s, "Pages can crash or hang the system.", UI_STATUS_TEXT, s);
    /* Countdown bar. */
    int bx = ox + 30 * s, by = oy + 134 * s;
    rb_rect(bx, by, 240 * s, 6 * s, UI_URL, 0, 0, W, H);
    rb_frame_rect(bx, by, 240 * s, 6 * s, UI_LINE);
    if (filled > 0) {
        if (filled > 238) filled = 238;
        rb_rect(bx + 1, by + 1, filled * (240 * s - 2) / 238, 6 * s - 2, amber, 0, 0, W, H);
    }
    present();
}

static void show_unsafe_banner(void) {
    uint32_t start = get_time_ms();
    int last = -1;
    for (;;) {
        uint32_t el = get_time_ms() - start;
        if (el >= RB_BANNER_MS) break;
        int filled = (int)(el * 238 / RB_BANNER_MS);
        if (filled != last) {
            last = filled;
            banner_frame(filled);
        }
    }
    banner_frame(238);
}

static int ps2_wait(int want_full) {
    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb(0x64);
        if (want_full) {
            if (st & 1) return 1;
        } else if ((st & 2) == 0) {
            return 1;
        }
    }
    return 0;
}

static int mouse_write(uint8_t v) {
    if (!ps2_wait(0)) return 0;
    outb(0x64, 0xD4);
    if (!ps2_wait(0)) return 0;
    outb(0x60, v);
    return 1;
}

static int mouse_read(uint8_t *v) {
    if (!ps2_wait(1)) return 0;
    *v = inb(0x60);
    return 1;
}

static void mouse_init(void) {
    g_mouse_ok = 0;
    g_wheel_bytes = 3;
    uint8_t ack = 0;
    /* A key byte still queued (the release of the Enter that launched us, or
       anything typed during the banner) would be read below as the config
       byte, and written back: 0x9C has bit 4 set, which disables the
       keyboard. Empty the controller first. */
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) inb(0x60);
    if (!ps2_wait(0)) return;
    outb(0x64, 0xA8);
    if (!ps2_wait(0)) return;
    outb(0x64, 0x20);
    uint8_t cmd = 0;
    if (!mouse_read(&cmd)) return;
    cmd = (uint8_t)((cmd | 0x02) & (uint8_t)~0x20);
    /* Bit 1 requests IRQ12. The kernel IRQ handler does not drain the
       controller, so leave mouse interrupts off and poll instead. */
    cmd = (uint8_t)(cmd & (uint8_t)~0x02);
    if (!ps2_wait(0)) return;
    outb(0x64, 0x60);
    if (!ps2_wait(0)) return;
    outb(0x60, cmd);
    if (!mouse_write(0xF6) || !mouse_read(&ack)) return;
    int wheel = 1;
    const uint8_t seq[3] = {200, 100, 80};
    for (int i = 0; i < 3; i++) {
        if (!mouse_write(0xF3) || !mouse_read(&ack) || !mouse_write(seq[i]) || !mouse_read(&ack)) {
            wheel = 0;
            break;
        }
    }
    if (wheel && mouse_write(0xF2) && mouse_read(&ack)) {
        uint8_t id = 0;
        if (mouse_read(&id) && id == 3) g_wheel_bytes = 4;
    }
    if (!mouse_write(0xF4) || !mouse_read(&ack)) return;
    g_mouse_ok = 1;
    g_pkt_n = 0;
}

static void mouse_off(void) {
    uint8_t ack = 0;
    if (g_mouse_ok) mouse_write(0xF5);
    (void)ack;
    while (inb(0x64) & 1) inb(0x60);
}

static void clamp_mouse(void) {
    if (g_mx < 0) g_mx = 0;
    if (g_my < 0) g_my = 0;
    if (g_mx > rb_ui.w - 1) g_mx = rb_ui.w - 1;
    if (g_my > rb_ui.h - 1) g_my = rb_ui.h - 1;
}

/* Fires RB_EV_SCROLL on the node under the pointer with content-space
    coordinates and applies the wheel delta unless a listener prevents it;
    defined after content_hit(). z is the raw wheel tick (negative = up). */
static void dispatch_wheel(int z);

static void on_mouse_packet(void) {
    if (g_pkt[0] & 0xC0) return;
    int dx = (int)g_pkt[1];
    int dy = (int)g_pkt[2];
    if (g_pkt[0] & 0x10) dx -= 256;
    if (g_pkt[0] & 0x20) dy -= 256;
    /* a runaway packet moves the pointer an eighth of the screen at most */
    int lim = rb_ui.w / 8;
    if (dx > lim) dx = lim;
    if (dx < -lim) dx = -lim;
    if (dy > lim) dy = lim;
    if (dy < -lim) dy = -lim;
    g_mx += dx;
    g_my -= dy;
    clamp_mouse();
    g_left = (g_pkt[0] & 1) ? 1 : 0;
    /* The full 3-bit button state rides along for the event payload. */
    g_btn = (uint8_t)(g_pkt[0] & 7);
    g_dirty = 1;
    if (g_wheel_bytes == 4) {
        int z = (int)(int8_t)g_pkt[3];
        /* Only a real wheel tick is a SCROLL event (a pure move, z==0, must
           not fire one). The scroll delta is applied by dispatch_wheel after
           the event so a preventing listener can cancel it. */
        if (z) dispatch_wheel(z);
    }
}

static void mouse_byte(uint8_t b) {
    if (g_pkt_n == 0 && (b & 0x08) == 0) return;
    g_pkt[g_pkt_n++] = b;
    if (g_pkt_n >= g_wheel_bytes) {
        on_mouse_packet();
        g_pkt_n = 0;
    }
}

static char scancode_char(uint8_t sc) {
    if (g_shift) {
        switch (sc) {
        case 0x02: return '!'; case 0x03: return '@'; case 0x04: return '#';
        case 0x05: return '$'; case 0x06: return '%'; case 0x07: return '^';
        case 0x08: return '&'; case 0x09: return '*'; case 0x0A: return '(';
        case 0x0B: return ')'; case 0x0C: return '_'; case 0x0D: return '+';
        case 0x10: return 'Q'; case 0x11: return 'W'; case 0x12: return 'E';
        case 0x13: return 'R'; case 0x14: return 'T'; case 0x15: return 'Y';
        case 0x16: return 'U'; case 0x17: return 'I'; case 0x18: return 'O';
        case 0x19: return 'P'; case 0x1A: return '{'; case 0x1B: return '}';
        case 0x1E: return 'A'; case 0x1F: return 'S'; case 0x20: return 'D';
        case 0x21: return 'F'; case 0x22: return 'G'; case 0x23: return 'H';
        case 0x24: return 'J'; case 0x25: return 'K'; case 0x26: return 'L';
        case 0x27: return ':'; case 0x28: return '"'; case 0x29: return '~';
        case 0x2B: return '|';
        case 0x2C: return 'Z'; case 0x2D: return 'X'; case 0x2E: return 'C';
        case 0x2F: return 'V'; case 0x30: return 'B'; case 0x31: return 'N';
        case 0x32: return 'M'; case 0x33: return '<'; case 0x34: return '>';
        case 0x35: return '?';
        default: return 0;
        }
    }
    switch (sc) {
    case 0x02: return '1'; case 0x03: return '2'; case 0x04: return '3';
    case 0x05: return '4'; case 0x06: return '5'; case 0x07: return '6';
    case 0x08: return '7'; case 0x09: return '8'; case 0x0A: return '9';
    case 0x0B: return '0'; case 0x0C: return '-'; case 0x0D: return '=';
    case 0x10: return 'q'; case 0x11: return 'w'; case 0x12: return 'e';
    case 0x13: return 'r'; case 0x14: return 't'; case 0x15: return 'y';
    case 0x16: return 'u'; case 0x17: return 'i'; case 0x18: return 'o';
    case 0x19: return 'p'; case 0x1A: return '['; case 0x1B: return ']';
    case 0x1E: return 'a'; case 0x1F: return 's'; case 0x20: return 'd';
    case 0x21: return 'f'; case 0x22: return 'g'; case 0x23: return 'h';
    case 0x24: return 'j'; case 0x25: return 'k'; case 0x26: return 'l';
    case 0x27: return ';'; case 0x28: return '\''; case 0x29: return '`';
    case 0x2B: return '\\';
    case 0x2C: return 'z'; case 0x2D: return 'x'; case 0x2E: return 'c';
    case 0x2F: return 'v'; case 0x30: return 'b'; case 0x31: return 'n';
    case 0x32: return 'm'; case 0x33: return ','; case 0x34: return '.';
    case 0x35: return '/';
    case 0x39: return ' ';
    default: return 0;
    }
}

static void hist_push(const char *url) {
    if (g_hist_i + 1 < g_hist_n) g_hist_n = g_hist_i + 1;
    if (g_hist_n > 0 && strcmp(g_hist[g_hist_n - 1], url) == 0) {
        g_hist_i = g_hist_n - 1;
        return;
    }
    if (g_hist_n >= 8) {
        memmove(g_hist[0], g_hist[1], 7 * 384);
        g_hist_n = 7;
        g_hist_i = 6;
    }
    copy_str(g_hist[g_hist_n], 384, url);
    g_hist_i = g_hist_n;
    g_hist_n++;
}

/* The UI caret slot (g_focus) is only a projection of the DOM active element:
   the two must never disagree, or the seams that read focus_node() and the
   ones that read rb_focus_node(g_focus) diverge. Every path that changes
   either one calls this, so the ring and the element move together. The
   fallback is coherent because it is derived: with no active element there is
   no slot to fall back to (g_focus becomes -1). */
static void sync_focus(void) {
    int a = rb_dom_active_element();
    g_focus = a > 0 ? rb_focus_index(a) : -1;
}

static void load_styles(const char *base);
static void run_page_scripts(const char *canonical);
static void images_new_page(void);
static void follow(const char *href, int push, int user);
static int g_js_errors;   /* uncaught JS errors on this page (see js_console) */
static char g_js_err_status[80];      /* the status line of the latest one */
static int g_js_pending;              /* script changes waiting for a relayout */
static int g_img_scan;                /* look for <img> elements still to load (img_turn) */
static uint32_t g_js_last_layout;     /* get_time_ms() of the last one */

/* web: a real page from the network (white paper, standard UA stylesheet,
   external CSS); otherwise the native RadiumOS look. */
/* The status line for a page at rest: its title, unless a JS error or a
   truncated page is more important. */
static void page_status(void) {
    char title[80];
    if (g_js_errors) set_status(g_js_err_status);    /* the JS error stays visible */
    else if (rb_title(title, (int)sizeof(title)) > 0) set_status(title);
    else set_status("Tab focus  Enter open  Esc quit");
    if (rb_oom()) set_status("Page truncated: engine memory full");
}

static void open_html(const char *html, int len, const char *canonical, int web) {
    /* The old page's images go first: every slot and pixel buffer is for
       one document. Nothing paints between here and the new layout. */
    images_new_page();
    if (web) {
        rb_open_web(html, len);
        load_styles(canonical);
    } else {
        rb_open(html, len);
    }
    copy_str(g_current, (int)sizeof(g_current), canonical);
    set_url_bar(canonical);
    g_scroll = 0;
    g_url_focus = 0;
    run_page_scripts(canonical);
    /* Put the caret on the first tab stop through the DOM, so the active
       element is authoritative (rb_open cleared any previous one). rb_open
       also cleared all listeners, so the FOCUS dispatch below is inert and
       the ring lands on the same node as before. g_focus is then derived from
       whatever actually became active. */
    if (rb_focus_count() > 0) rb_dom_focus(rb_focus_node(0));
    sync_focus();
    g_hover_node = 0;
    g_hover_gen = 0;
    /* A document loaded under a stationary pointer must still get its first
       MOUSEMOVE: clear both the hover node and the memoised pointer position. */
    g_mm_x = -1;
    g_mm_y = -1;
    /* The status bar shows g_hover while it is set; a sticky link from the
       previous page must not mask this page's own status text. */
    g_hover[0] = 0;
    page_status();
    /* The document was successfully replaced: fire LOAD on its root. */
    rb_dispatch(1, RB_EV_LOAD);
}

/* ---- page JavaScript -------------------------------------------------------
   Each document gets a fresh QuickJS runtime (the previous one is abandoned
   wholesale: its arena is reset, nothing is walked). Scripts run in source
   order after the document is parsed and styled, then DOMContentLoaded,
   then the engine's load. External scripts come from the network for remote
   pages and from AVFS for local ones. Console output goes to the QEMU debug
   port; uncaught errors also show in the status bar. */
static char g_script[1024 * 1024];
static char g_js_line[160];
static int g_js_line_n;

static void js_console(const char *s, int len) {
    for (int i = 0; i < len; i++) {
        outb(0xE9, (uint8_t)s[i]);
        char c = s[i];
        if (c == '\n') {
            g_js_line[g_js_line_n] = 0;
            if (starts(g_js_line, "Uncaught")) {
                g_js_errors++;
                char st[80];
                int w = 0;
                const char *pre = "JS: ";
                while (*pre) st[w++] = *pre++;
                for (int k = 9; g_js_line[k] && w < 79; k++) st[w++] = g_js_line[k];
                st[w] = 0;
                set_status(st);
                copy_str(g_js_err_status, (int)sizeof(g_js_err_status), st);
            }
            g_js_line_n = 0;
        } else if (g_js_line_n < (int)sizeof(g_js_line) - 1) {
            g_js_line[g_js_line_n++] = c;
        }
    }
}

/* fetch()/XHR from scripts. */
static int js_fetch(const char *url, char *out, int cap) {
    fg_watchdog_pause();
    int n = rbrowse_http_get((const uint8_t *)url, (uint8_t *)out, cap);
    fg_watchdog_resume();
    return n;
}

static int script_is_classic(int node) {
    char type[64];
    if (!rb_dom_has_attr(node, "type", 4)) return 1;
    int n = rb_dom_get_attr(node, "type", 4, type, (int)sizeof(type));
    if (n <= 0) return 1;
    for (int i = 0; type[i]; i++) if (type[i] >= 'A' && type[i] <= 'Z') type[i] = (char)(type[i] + 32);
    /* Modules need an import loader QuickJS is not given here; pages ship a
       nomodule fallback for exactly this case. */
    return strstr(type, "javascript") || strstr(type, "ecmascript") || strcmp(type, "text/jscript") == 0;
}

/* Load an external script into g_script. Returns its length or -1. */
static int load_script(const char *page, const char *src, char *url, int ucap) {
    if (!rb_resolve(page, src, url, ucap)) return -1;
    int page_remote = starts(page, "http://") || starts(page, "https://");
    if (starts(url, "http://") || starts(url, "https://")) {
        fg_watchdog_pause();
        int n = rbrowse_http_get((const uint8_t *)url, (uint8_t *)g_script, (int)sizeof(g_script));
        fg_watchdog_resume();
        return n;
    }
    if (page_remote || url[0] != '/') return -1;   /* a web page never reads local files */
    int size = avfs_get_filesize(url);
    if (size < 0 || size >= (int)sizeof(g_script)) return -1;
    if (avfs_read_file(url, g_script, (uint32_t)size, 0) != 0) return -1;
    return size;
}

static void run_page_scripts(const char *canonical) {
    g_js_errors = 0;
    g_js_line_n = 0;
    rjs_set_output(js_console);
    if (rjs_start() != RJS_OK || rjs_dom_install(canonical, js_fetch) != 0) {
        set_status("JS: engine failed to start");
        return;
    }
    int count = rb_script_count();
    for (int i = 0; i < count; i++) {
        int node, len;
        const char *text;
        if (!rb_script_get(i, &node, &text, &len) || !script_is_classic(node)) continue;
        fg_pet();
        char src[384];
        if (rb_dom_has_attr(node, "src", 3) && rb_dom_get_attr(node, "src", 3, src, (int)sizeof(src)) > 0) {
            char url[384];
            set_status("Loading script...");
            paint();
            int n = load_script(canonical, src, url, (int)sizeof(url));
            if (n >= 0) rjs_dom_exec(node, g_script, n, url);
        } else if (len > 0) {
            rjs_dom_exec(node, text, len, canonical);
        }
    }
    fg_pet();
    rjs_dom_loaded();
    if (rjs_dom_take_dirty()) rb_relayout();
    g_js_pending = 0;
    g_js_last_layout = get_time_ms();
}

/* Timers and promise jobs, then whatever they changed: relayout, repaint,
   and a navigation a script asked for. */
#define JS_RELAYOUT_MS 250   /* a big page's relayout takes seconds: batch script changes */

static void js_turn(void) {
    if (!rjs_running()) return;
    rjs_dom_tick();
    if (rjs_dom_take_dirty()) {
        g_js_pending = 1;
        g_img_scan = 1;          /* a script may have added images or changed a src */
    }
    if (g_js_pending && get_time_ms() - g_js_last_layout >= JS_RELAYOUT_MS) {
        g_js_pending = 0;
        rb_relayout();
        g_js_last_layout = get_time_ms();
        clamp_scroll();
        sync_focus();
        g_dirty = 1;
    }
    char nav[384];
    if (rb_take_nav(nav, (int)sizeof(nav))) follow(nav, 1, 0);
}

/* Message text is network/user-influenced (URLs, socket errors); '<' and '&'
   would otherwise become markup or entities inside the generated page. */
static void text_escape(char *html, int cap, int *n, const char *s) {
    while (*s) {
        const char *rep = 0;
        switch (*s) {
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '&': rep = "&amp;"; break;
        default:
            if (*n < cap - 1) html[(*n)++] = *s;
            s++;
            continue;
        }
        for (int i = 0; rep[i]; i++)
            if (*n < cap - 1) html[(*n)++] = rep[i];
        s++;
    }
}

static void show_message(const char *canonical, const char *heading, const char *detail) {
    char html[700];
    int n = 0;
    const char *a = "<html><body style=\"background-color:#120C06;color:#DDCCBB\"><h1>";
    const char *b = "</h1><p>";
    const char *c = "</p></body></html>";
    while (*a && n < 680) html[n++] = *a++;
    text_escape(html, (int)sizeof(html), &n, heading);
    while (*b && n < 680) html[n++] = *b++;
    text_escape(html, (int)sizeof(html), &n, detail);
    while (*c && n < 690) html[n++] = *c++;
    html[n] = 0;
    open_html(html, n, canonical, 0);
}

static void append_str(char *dst, int cap, int *w, const char *src);

/* PNG, JPEG, GIF or BMP by its first bytes. */
static int is_image_data(const uint8_t *b, int n) {
    if (n < 30) return 0;
    if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return 1;
    if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return 1;
    if (b[0] == 'G' && b[1] == 'I' && b[2] == 'F' && b[3] == '8') return 1;
    return b[0] == 'B' && b[1] == 'M' && (b[14] == 12 || b[14] == 40 || b[14] == 56 || b[14] == 108 ||
                                          b[14] == 124) && !b[15] && !b[16] && !b[17];
}

/* s with & < > " escaped, for text or a quoted attribute. */
static void html_escape(char *out, int cap, int *w, const char *s) {
    for (; *s && *w < cap - 7; s++) {
        const char *rep = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '"' ? "&quot;" : 0;
        if (!rep) { out[(*w)++] = *s; continue; }
        while (*rep) out[(*w)++] = *rep++;
    }
    out[*w] = 0;
}

static void open_fetched(const char *canonical, int n) {
    int web = starts(canonical, "http://") || starts(canonical, "https://");
    if (is_image_data((const uint8_t *)g_fetch, n)) {
        /* The address is an image: show it on a generated page. The bytes
           are already here, so they go straight into the new page's image
           table instead of being fetched again. */
        static char page[2600];
        int w = 0;
        const char *name = canonical;
        for (const char *p = canonical; *p && *p != '?' && *p != '#'; p++) if (*p == '/' && p[1]) name = p + 1;
        append_str(page, (int)sizeof(page), &w, "<html><head><title>");
        html_escape(page, 400, &w, name);
        append_str(page, (int)sizeof(page), &w, "</title></head><body style=\"margin:8px;background:#222\"><img src=\"");
        html_escape(page, (int)sizeof(page) - 40, &w, canonical);
        append_str(page, (int)sizeof(page), &w, "\"></body></html>");
        open_html(page, w, canonical, 1);
        int slot = rb_image_add(canonical);
        if (slot >= 0 && rb_image_decode(slot, (const uint8_t *)g_fetch, n) != RB_IMAGE_OK) rb_image_fail(slot);
        return;
    }
    open_html(g_fetch, n, canonical, web);
}

/* Fetch the document's external stylesheets (<link rel=stylesheet> and the
   @imports they pull in) and relayout once. A sheet that fails to load is
   skipped: the page still renders with whatever did arrive. */
static char g_sheet_url[24][900];     /* resolved URL of each stylesheet, the base for its @imports */

static void load_styles(const char *base) {
    int fetched = 0;
    for (int i = 0; i < rb_css_pending() && i < 24; i++) {
        char href[700];
        char *url = g_sheet_url[i];
        url[0] = 0;
        rb_css_href(i, href, (int)sizeof(href));
        int par = rb_css_parent(i);
        const char *from = (par >= 0 && par < 24 && g_sheet_url[par][0]) ? g_sheet_url[par] : base;
        if (!rb_resolve(from, href, url, (int)sizeof(g_sheet_url[0]))) { url[0] = 0; continue; }
        if (!starts(url, "http://") && !starts(url, "https://")) continue;
        char msg[48];
        int w = 0;
        const char *pre = "Styles ";
        while (*pre) msg[w++] = *pre++;
        msg[w++] = (char)('1' + (i < 9 ? i : 8));
        msg[w++] = '/';
        int tot = rb_css_pending();
        msg[w++] = (char)('0' + (tot < 9 ? tot : 9));
        msg[w] = 0;
        set_status(msg);
        paint();
        fg_watchdog_pause();
        int n = rbrowse_http_get((const uint8_t *)url, (uint8_t *)g_css, (int)sizeof(g_css));
        fg_watchdog_resume();
        if (n > 0) { rb_css_provide(i, g_css, n); fetched++; }
    }
    if (fetched) rb_relayout();
}

/* ---- images -----------------------------------------------------------------
   The page is shown first; images load afterwards, one network fetch per
   main-loop turn (img_turn), so input and repaints keep going between
   downloads. Each <img> resolves to one URL (<picture><source>, srcset or
   src); a URL already in the page's image table is shared, so a repeated
   icon is fetched and decoded once. Decoded sizes are batched into one
   relayout (when the queue drains, or every IMG_RELAYOUT_MS while it runs);
   an image laid out at an explicit size just repaints. Remote pages load
   http(s) and data: images; local pages also read AVFS files. Images that
   are never inserted (a script's new Image()) load too. Caps: image.h
   (RB_IMAGE_MAX images per page, file size, dimensions, page memory). */
#define IMG_RELAYOUT_MS 1500

static uint8_t g_imgfile[RB_IMAGE_FILE_MAX];
static char g_img_url[RB_IMAGE_URL_MAX];
static char g_img_raw[RB_IMAGE_URL_MAX];
static char g_img_set[RB_IMAGE_URL_MAX];
static int g_img_relayout;            /* decoded sizes waiting for a relayout */
static uint32_t g_img_since;          /* when the oldest of those arrived */
static int g_img_fetched;             /* fetches this page (status line) */
static int g_img_busy;                /* the status line shows image progress */

static void images_new_page(void) {
    rb_image_reset();
    g_img_scan = 1;
    g_img_relayout = 0;
    g_img_fetched = 0;
    g_img_busy = 0;
}

/* An attribute into out, NUL-terminated. Its length, or -1 when it is
   absent or does not fit. */
static int attr_into(int node, const char *name, char *out, int cap) {
    int nl = (int)strlen(name);
    int n = rb_dom_attr_len(node, name, nl);
    if (n < 0 || n >= cap || !rb_dom_get_attr(node, name, nl, out, cap)) return -1;
    out[n] = 0;
    return n;
}

/* A <source type> the decoder reads (no type counts as yes). */
static int type_ok(const char *t) {
    static const char *const ok[] = { "image/png", "image/jpeg", "image/jpg", "image/gif", "image/bmp", 0 };
    if (!t[0]) return 1;
    for (int i = 0; ok[i]; i++) {
        int k = 0;
        while (ok[i][k] && (t[k] | 32) == ok[i][k]) k++;
        if (!ok[i][k] && (!t[k] || t[k] == ';' || t[k] == ' ')) return 1;
    }
    return 0;
}

/* The (unresolved) URL an <img> shows: the first usable <source> of its
   <picture> (media queries are not evaluated: a source with media= is
   skipped), else its srcset when that uses width descriptors, else src,
   else any srcset candidate. srcset widths are matched against the image's
   laid-out width, or the viewport. 0 when it names nothing. */
static int img_pick(int node, char *out, int cap) {
    int want = rb_ui.view_w, x, y, w, h;
    if (rb_dom_rect(node, &x, &y, &w, &h) && w > 0) want = w;
    int is_w = 0;
    int par = rb_dom_parent(node);
    if (par > 0 && rb_node_tag(par) == RB_TAG_PICTURE) {
        for (int c = rb_dom_first(par); c && c != node; c = rb_dom_next(c)) {
            if (rb_dom_is_text(c) || rb_node_tag(c) != RB_TAG_SOURCE || rb_dom_has_attr(c, "media", 5)) continue;
            char type[64];
            if (attr_into(c, "type", type, (int)sizeof(type)) < 0) type[0] = 0;
            if (!type_ok(type)) continue;
            int n = attr_into(c, "srcset", g_img_set, (int)sizeof(g_img_set));
            if (n > 0 && rb_image_pick_srcset(g_img_set, n, want, out, cap, &is_w)) return 1;
        }
    }
    int sn = attr_into(node, "srcset", g_img_set, (int)sizeof(g_img_set));
    int picked = sn > 0 && rb_image_pick_srcset(g_img_set, sn, want, out, cap, &is_w);
    if (picked && is_w) return 1;
    char *src = picked ? g_img_url : out;           /* keep the srcset choice while looking at src */
    int n = attr_into(node, "src", src, cap);
    while (n > 0 && (src[0] == ' ' || src[0] == '\t' || src[0] == '\n')) { src++; n--; }
    if (n > 0) {
        if (src != out) copy_str(out, cap, src);
        return 1;
    }
    return picked;
}

/* The encoded bytes of an image URL (absolute) into buf: length or -1. */
static int img_fetch(const char *url, uint8_t *buf, int cap) {
    if (starts(url, "data:")) return rb_image_data_url(url, buf, cap);
    if (starts(url, "http://") || starts(url, "https://")) {
        fg_watchdog_pause();
        int n = rbrowse_http_get((const uint8_t *)url, buf, cap);
        fg_watchdog_resume();
        return n >= cap - 1 ? -1 : n;           /* filled the buffer: truncated, over the cap */
    }
    int page_remote = starts(g_current, "http://") || starts(g_current, "https://");
    if (page_remote || url[0] != '/') return -1;    /* a web page never reads local files */
    int size = avfs_get_filesize(url);
    if (size <= 0 || size > cap) return -1;
    if (avfs_read_file(url, buf, (uint32_t)size, 0) != 0) return -1;
    return size;
}

/* Fetch and decode a new image into its slot. */
static void img_load(int slot, const char *url) {
    char msg[48];
    int w = 0;
    const char *pre = "Loading image ";
    while (*pre) msg[w++] = *pre++;
    int k = ++g_img_fetched;
    char num[8];
    int nn = 0;
    do { num[nn++] = (char)('0' + k % 10); k /= 10; } while (k && nn < 7);
    while (nn) msg[w++] = num[--nn];
    msg[w] = 0;
    set_status(msg);
    g_img_busy = 1;
    paint();
    if (rb_image_unsupported(url)) { rb_image_fail(slot); return; }
    fg_pet();
    int n = img_fetch(url, g_imgfile, (int)sizeof(g_imgfile));
    fg_pet();
    if (n <= 0 || rb_image_decode(slot, g_imgfile, n) != RB_IMAGE_OK) {
        rb_image_fail(slot);
        js_console("[img] ", 6);
        js_console(n <= 0 ? "fetch failed" : rb_image_error(), n <= 0 ? 12 : (int)strlen(rb_image_error()));
        js_console(": ", 2);
        int ul = 0;
        while (ul < 200 && url[ul]) ul++;
        js_console(url, ul);
        js_console("\n", 1);
    }
    fg_pet();
}

/* The absolute URL an <img> shows into g_img_url: 1, or 0 when it names
   none (no source at all, or one that does not resolve: *empty tells). */
static int img_url(int node, int *empty) {
    *empty = 0;
    g_img_url[0] = 0;
    if (!img_pick(node, g_img_raw, (int)sizeof(g_img_raw))) { *empty = 1; return 0; }
    const char *base = g_current[0] ? g_current : "/";
    if (starts(g_img_raw, "data:")) copy_str(g_img_url, (int)sizeof(g_img_url), g_img_raw);
    else if (!rb_resolve(base, g_img_raw, g_img_url, (int)sizeof(g_img_url))) g_img_url[0] = 0;
    return g_img_url[0] != 0;
}

/* Store an outcome, note whether it needs a relayout, tell the page. */
static void img_settle(int node, int st, int slot, int nw, int nh) {
    if (rb_dom_img_set(node, st, slot, nw, nh) > 0 && !g_img_relayout) {
        g_img_relayout = 1;
        g_img_since = get_time_ms();
    }
    g_dirty = 1;
    if (st != RB_IMG_EMPTY && rjs_running()) rjs_dom_image_event(node, st == RB_IMG_OK);
}

/* One step. Every <img> still unresolved whose URL is already known (or
   that has none) gets its outcome at once; then ONE new image is fetched:
   the one whose box is nearest the visible part of the page, else the
   first in document order. Its other users resolve on the next step. */
static void img_turn(void) {
    if (g_img_scan) {
        int first = 0, empty;
        for (int node = rb_dom_img_next(0); node; node = rb_dom_img_next(node)) {
            if (rb_dom_img_state(node, 0, 0, 0) != RB_IMG_NONE) continue;
            if (!img_url(node, &empty)) {
                img_settle(node, empty ? RB_IMG_EMPTY : RB_IMG_BROKEN, 0, 0, 0);
                continue;
            }
            int slot = rb_image_find(g_img_url), nw = 0, nh = 0;
            if (slot >= 0) {
                int ok = rb_image_state(slot, &nw, &nh) == RB_IMAGE_OK;
                img_settle(node, ok ? RB_IMG_OK : RB_IMG_BROKEN, slot, nw, nh);
            } else if (rb_image_count() >= RB_IMAGE_MAX) {
                img_settle(node, RB_IMG_BROKEN, 0, 0, 0);          /* over the per-page cap */
            } else if (!first) {
                first = node;
            }
            /* an event handler may have navigated: a new page restarted the scan */
            if (!g_img_scan) return;
        }
        if (!first) {
            g_img_scan = 0;
        } else {
            /* nearest the viewport among the boxes still waiting (one walk) */
            int best = 0, best_d = 0x7FFFFFFF;
            int top = g_scroll, bot = g_scroll + rb_ui.content_h;
            for (int i = 0; i < rb_box_count(); i++) {
                const RbBox *b = rb_box(i);
                /* boxes are from the last layout: the node may have changed since */
                if (!b || b->kind != RB_BOX_IMAGE || !rb_dom_generation((int)b->toff) ||
                    rb_node_tag((int)b->toff) != RB_TAG_IMG || rb_dom_img_state((int)b->toff, 0, 0, 0) != RB_IMG_NONE) continue;
                int y0 = b->y, y1 = b->y + b->h;
                if (b->flags & RB_BOX_FIXED) { y0 += g_scroll; y1 += g_scroll; }
                int d = y1 < top ? top - y1 : y0 > bot ? y0 - bot : 0;
                if (d < best_d) { best_d = d; best = (int)b->toff; }
            }
            int node = best ? best : first, empty;
            if (img_url(node, &empty) && rb_image_find(g_img_url) < 0) {
                int slot = rb_image_add(g_img_url);
                if (slot >= 0) {
                    img_load(slot, g_img_url);
                    int nw = 0, nh = 0;
                    int ok = rb_image_state(slot, &nw, &nh) == RB_IMAGE_OK;
                    /* a script may have removed it meanwhile; others share the slot next step */
                    if (rb_dom_img_state(node, 0, 0, 0) == RB_IMG_NONE)
                        img_settle(node, ok ? RB_IMG_OK : RB_IMG_BROKEN, slot, nw, nh);
                }
            }
        }
    }
    if (g_img_relayout && (!g_img_scan || get_time_ms() - g_img_since >= IMG_RELAYOUT_MS)) {
        g_img_relayout = 0;
        rb_relayout();
        g_js_last_layout = get_time_ms();
        clamp_scroll();
        sync_focus();
        g_dirty = 1;
    }
    if (g_img_busy && !g_img_scan) {
        g_img_busy = 0;
        page_status();
        g_dirty = 1;
    }
}

static int load_canonical(const char *canonical) {
    /* Internal pages. about:crash and about:hang exercise the fault guard. */
    if (starts(canonical, "about:")) {
        if (strcmp(canonical, "about:blank") == 0) {
            open_html("", 0, canonical, 0);
            return 0;
        }
        if (strcmp(canonical, "about:crash") == 0) __builtin_trap();
        if (strcmp(canonical, "about:hang") == 0) for (;;) __asm__ volatile("");
        show_message(canonical, "Not found", canonical);
        return -1;
    }
    if (starts(canonical, "http://") || starts(canonical, "https://")) {
        set_status("Loading...");
        paint();
        fg_watchdog_pause();
        int n = rbrowse_http_get((const uint8_t *)canonical, (uint8_t *)g_fetch, (int)sizeof(g_fetch));
        fg_watchdog_resume();
        if (n < 0) {
            const char *detail = g_fetch[0]
                ? g_fetch
                : "HTTP needs the network stack. Check the URL, NIC, and DNS.";
            show_message(canonical, "Load failed", detail);
            return -1;
        }
        open_fetched(canonical, n);
        return 0;
    }
    int size = avfs_get_filesize(canonical);
    if (size < 0) {
        show_message(canonical, "Not found", canonical);
        return -1;
    }
    if (size >= (int)sizeof(g_fetch)) size = (int)sizeof(g_fetch) - 1;
    if (avfs_read_file(canonical, g_fetch, (uint32_t)size, 0) != 0) {
        show_message(canonical, "Read error", canonical);
        return -1;
    }
    g_fetch[size] = 0;
    open_fetched(canonical, size);
    return 0;
}

/* user: 1 when the href was typed by the human (address bar, command line),
   0 when it came from page markup or a script. */
static void follow(const char *href, int push, int user) {
    char resolved[384];
    if (!href) return;
    while (*href == ' ' || *href == '\t') href++;
    if (!href[0]) return;
    const char *base = g_current[0] ? g_current : "/";
    int base_remote = starts(base, "http://") || starts(base, "https://");
    if (starts(href, "http://") || starts(href, "https://") || starts(href, "about:")) {
        copy_str(resolved, (int)sizeof(resolved), href);
    } else if (!push && href[0] == '/') {
        /* History (push == 0) stores already-resolved canonicals. Re-resolving
           a local path such as /home/user/rbrowse.html against a remote base
           would fetch it from the remote origin, so Back from a website to the
           local start page would load the wrong URL. */
        copy_str(resolved, (int)sizeof(resolved), href);
    } else if (!rb_resolve(base, href, resolved, (int)sizeof(resolved))) {
        /* Everything else (root-relative /x, scheme-relative //x, file:// on a
           local page, plain relative) keeps the origin of the current page. */
        return;
    }
    /* A remote page must not be able to read local files. Testing the resolved
       URL (not the raw href) closes the " file://…" whitespace bypass, since
       rb_resolve trims before it picks the scheme. Local pages may still
       follow file:// links. History (push == 0) is user-driven and only holds
       canonicals that were vetted when first followed, so it is exempt, and a
       hand-typed href (address bar / command line) carries the user's own
       intent, like rbrowse file://-… on the command line. */
    if (push && !user && base_remote && !starts(resolved, "http://") &&
        !starts(resolved, "https://")) {
        set_status("blocked: remote page cannot open local files");
        return;
    }
    if (push) hist_push(resolved);
    char cur[384];
    copy_str(cur, (int)sizeof(cur), resolved);
    for (int hop = 0; hop < 4; hop++) {
        load_canonical(cur);
        char next[384];
        char step[384];
        if (!rb_take_nav(next, (int)sizeof(next))) break;
        if (!rb_resolve(cur, next, step, (int)sizeof(step))) break;
        /* Scripts on a loaded page can also request local files — same block. */
        if ((starts(cur, "http://") || starts(cur, "https://")) &&
            !starts(step, "http://") && !starts(step, "https://")) {
            set_status("blocked: remote page cannot open local files");
            break;
        }
        copy_str(cur, (int)sizeof(cur), step);
        if (g_hist_n > 0) copy_str(g_hist[g_hist_i], 384, cur);
    }
}

static int host_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == ':' || c == '_';
}

static void append_str(char *dst, int cap, int *w, const char *src) {
    for (int i = 0; src[i] && *w + 1 < cap; i++) dst[(*w)++] = src[i];
    if (*w < cap) dst[*w] = 0;
}

/* Percent-encode src into dst (space -> %20). */
static void append_pct(char *dst, int cap, int *w, const char *src) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; src[i]; i++) {
        unsigned char c = (unsigned char)src[i];
        int safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            if (*w + 1 < cap) dst[(*w)++] = (char)c;
        } else if (*w + 3 < cap) {
            dst[(*w)++] = '%';
            dst[(*w)++] = hex[c >> 4];
            dst[(*w)++] = hex[c & 15];
        }
    }
    if (*w < cap) dst[*w] = 0;
}

/* Address-bar normalization. A scheme-less host ("google.com", "www.google.com/x")
   gets an http:// prefix; anything else that is neither a path nor a scheme
   becomes a Google search. Paths (/x, ./x, ../x) and explicit schemes pass
   through untouched. A bare relative filename is no longer resolved against the
   current directory — that is the trade for search-by-default. */
static void normalize_address(const char *in, char *out, int cap) {
    while (*in == ' ' || *in == '\t') in++;
    if (!in[0] || starts(in, "http://") || starts(in, "https://") ||
        starts(in, "file://") || starts(in, "about:") || in[0] == '/' || in[0] == '.') {
        copy_str(out, cap, in);
        return;
    }
    /* The authority ends at the first '/'. A dot in it (or "localhost") marks a
       host; a non-host char or a space rules that out. */
    int dot = 0, ok = 1, space = 0;
    for (int i = 0; in[i]; i++) {
        char c = in[i];
        if (c == ' ' || c == '\t') { space = 1; break; }
        if (c == '/') break;
        if (c == '.') dot = 1;
        else if (!host_char(c)) ok = 0;
    }
    int w = 0;
    if (!space && ok && (dot || starts(in, "localhost"))) {
        append_str(out, cap, &w, "http://");
        append_str(out, cap, &w, in);
    } else {
        append_str(out, cap, &w, "http://www.google.com/search?q=");
        append_pct(out, cap, &w, in);
    }
}

/* Local pages only: check whether the typed input names a file next to the
   current page and return that absolute path, or 0 when it does not exist.
   "hello.html" and "docs/readme.txt" stay addressable while browsing local
   pages; anything with a scheme or a leading '/' or '.' is handled elsewhere,
   and a remote page never probes the local filesystem. */
static int try_local_probe(const char *in, char *out, int cap) {
    if (!in[0] || in[0] == '/' || in[0] == '.') return 0;
    int local = !g_current[0] || g_current[0] == '/';
    if (!local) return 0;
    int n = 0;
    while (in[n]) {
        if (in[n] == ' ' || in[n] == '\t') return 0;
        n++;
    }
    /* Directory part of the current page, stopping at its query (a local page
       reached through a GET submit has "?..." in its canonical). */
    int page_n = 0;
    while (g_current[page_n] && g_current[page_n] != '?' && g_current[page_n] != '#') page_n++;
    int dir = 0;
    for (int i = 0; i < page_n; i++) if (g_current[i] == '/') dir = i + 1;
    int w = 0;
    if (dir == 0) {
        /* No current page or a bare file at the AVFS root. */
        append_str(out, cap, &w, "/");
    }
    for (int i = 0; i < dir; i++) {
        if (w + 1 >= cap) return 0;
        out[w++] = g_current[i];
    }
    append_str(out, cap, &w, in);
    return avfs_get_filesize(out) >= 0;
}

static void go_url(void) {
    if (g_url_len <= 0) return;
    char target[384];
    g_url[g_url_len] = 0;
    normalize_address(g_url, target, (int)sizeof(target));
    /* Bare dotted names classify as hosts and bare paths as searches. When
       browsing locally, a file with the exact typed name wins: the address
       bar then still opens "hello.html" instead of http://hello.html, while
       "google.com" keeps going to the site (no local probe on remote pages). */
    char probe[384];
    if (try_local_probe(g_url, probe, (int)sizeof(probe)))
        copy_str(target, (int)sizeof(target), probe);
    follow(target, 1, 1);
}

/* POST forms are refuted by the engine (rb_activate returns a no-op for their
   controls); say so instead of leaving Enter/click silent. */
static void note_post_form(int node, int r) {
    if (r == 0 && rb_form_post(node)) set_status("unsupported: form method POST");
}

static void activate_focus(void) {
    if (g_url_focus) {
        go_url();
        return;
    }
    int node = focus_node();
    if (node <= 0) return;
    int r = rb_activate(node);
    if (r == 0) note_post_form(node, r);
    if (r == 2) {
        char href[384];
        if (rb_take_nav(href, (int)sizeof(href))) follow(href, 1, 0);
        return;
    }
    if (r == 1) {
        rb_relayout();
        set_status("Updated");
    }
    /* Keep the cursor slot derived from the active element after any default
       action (a relayout can prune it) so g_focus can never drift. */
    sync_focus();
}

static void type_char(char ch) {
    if (g_url_focus) {
        if (g_url_len + 1 < (int)sizeof(g_url)) {
            g_url[g_url_len++] = ch;
            g_url[g_url_len] = 0;
        }
        return;
    }
    int node = focus_node();
    if (node > 0 && rb_is_field(node) && rb_type_char(node, ch)) {
        rb_relayout();
        return;
    }
    if (ch == ' ') activate_focus();
}

static void do_backspace(void) {
    if (g_url_focus) {
        if (g_url_len > 0) g_url[--g_url_len] = 0;
        return;
    }
    int node = focus_node();
    if (node > 0 && rb_is_field(node) && rb_backspace(node)) rb_relayout();
}

static void focus_tab(int backward) {
    /* The active element is authoritative, so the tab cursor is derived from
       its slot; a listener that focused or blurred something is picked up
       here, and a stale g_focus can never diverge. */
    sync_focus();
    int n = rb_focus_count();
    int slots = n + 1;
    int pos = g_url_focus ? 0 : (g_focus < 0 ? 0 : g_focus + 1);
    if (backward) pos = (pos - 1 + slots) % slots;
    else pos = (pos + 1) % slots;
    if (pos == 0) {
        /* The address bar is not a DOM element: blur the page's element and
           derive the (now empty) cursor from the cleared active element. */
        g_url_focus = 1;
        rb_dom_blur();
        sync_focus();
    } else {
        g_url_focus = 0;
        g_focus = pos - 1;
        /* rb_dom_focus fires BLUR on the old element and FOCUS on the new. */
        if (!rb_dom_focus(rb_focus_node(g_focus))) g_focus = -1;
    }
}

static int zone_at(int x, int y) {
    if (y < rb_ui.chrome_h) return 3;
    if (y >= rb_ui.status_y) return 0;
    if (x >= rb_ui.scroll_x) return 2;
    return 1;
}

static void click_at(int x, int y) {
    if (y < rb_ui.chrome_h) {
        if (x < rb_ui.url_x) {
            if (g_hist_i > 0) {
                g_hist_i--;
                follow(g_hist[g_hist_i], 0, 0);
            }
            return;
        }
        if (x >= rb_ui.go_x) {
            g_url_focus = 1;
            rb_dom_blur();       /* the address bar takes focus off the page */
            sync_focus();
            go_url();
            return;
        }
        g_url_focus = 1;
        rb_dom_blur();
        sync_focus();
        return;
    }
    if (y >= rb_ui.status_y) return;
    if (x >= rb_ui.scroll_x) {
        int ty, th;
        thumb_geom(&ty, &th);
        if (y < ty) g_scroll -= rb_ui.content_h / 2;
        else if (y >= ty + th) g_scroll += rb_ui.content_h / 2;
        clamp_scroll();
        return;
    }
    int dx = x - rb_ui.view_x;
    int dy = y - rb_ui.content_y + g_scroll;
    int node = rb_hit_at(dx, dy, g_scroll);
    if (node < 0) {
        /* Blank content: the address bar no longer owns focus, and there is
           no page element either, so keep the cursor coherent with the (still
           unchanged) active element rather than leaving a stale slot behind. */
        g_url_focus = 0;
        sync_focus();
        return;
    }
    int idx = rb_focus_index(node);
    if (idx >= 0) {
        g_url_focus = 0;
        g_focus = idx;
        /* Focus the control in the DOM too, so BLUR/FOCUS fire and the
           active element matches the ring. No-op when it already is active. */
        rb_dom_focus(node);
    }
    /* A physical click is a mouse CLICK: it carries the pointer payload and
       uses the enter=0 path, whose default action toggles checkboxes/radios
       and navigates links. rb_activate is the keyboard/Enter path (enter=1),
       which submits the field's form instead of toggling — routing a click
       through it made a click on a checkbox a no-op and left the CLICK payload
       empty while MOUSEDOWN/UP carried one. */
    int r = 0;
    if (rb_is_field(node)) {
        r = 0;       /* a click only focuses a text field; Enter submits it */
    } else {
        r = rb_dispatch_ex(node, RB_EV_CLICK, 0, dx, dy, 0, g_btn, 0);
        if (r == 0) note_post_form(node, r);
    }
    /* A CLICK listener or default action may have moved the active element;
       keep the cursor slot derived from it. */
    if (r == 2) {
        char href[384];
        if (rb_take_nav(href, (int)sizeof(href))) follow(href, 1, 0);
        return;
    }
    if (r == 1) rb_relayout();
    /* After a relayout too: it can prune an active element that became
       non-focusable, so derive the slot once more from the final element. */
    sync_focus();
    char href[160];
    if (rb_node_href(node, href, (int)sizeof(href))) set_status(href);
}

/* Content-space coordinates of the pointer's current hit, or -1 when the
   pointer is over chrome / the scrollbar (no content node there). Coordinates
   are still written in that case so MOUSEOUT can carry the leaving position. */
static int content_hit(int *cx, int *cy) {
    *cx = g_mx - rb_ui.view_x;
    *cy = g_my - rb_ui.content_y + g_scroll;
    if (g_my < rb_ui.content_y || g_my >= rb_ui.status_y || g_mx >= rb_ui.scroll_x) return -1;
    return rb_hit_at(*cx, *cy, g_scroll);
}

static void dispatch_wheel(int z) {
    int cx = 0, cy = 0;
    int node = content_hit(&cx, &cy);
    /* content_hit misses over chrome/scrollbar and returns -1, but it still
       writes the pointer's real content-space coordinates (cx/cy may be
       negative above the content area); the fallback target is the node the
       pointer last hit, and cx/cy ride along as payload only. */
    if (node <= 0) node = g_hover_node;
    if (node > 0) {
        rb_dispatch_ex(node, RB_EV_SCROLL, 0, cx, cy, -1, g_btn, 0);
        /* A SCROLL listener that prevents the event cancels the scroll, like a
           real page. With no listener rb_last_dispatch_prevented() is 0 and
           the delta is applied exactly as before. */
        if (rb_last_dispatch_prevented()) return;
    }
    g_scroll += -z * (line_step() * 3 / 4);
    clamp_scroll();
}

/* The UI's caret/ring is exactly the DOM active element. Every seam that
   wants a caret (page load, Tab, click, activate) sets the active element
   through rb_dom_focus, so the two cannot drift. */
static int focus_node(void) {
    return rb_dom_active_element();
}

static void update_hover(void) {
    char href[160];
    href[0] = 0;
    int cx = 0, cy = 0;
    int node = content_hit(&cx, &cy);
    if (node < 0) node = 0;
    if (node > 0) rb_node_href(node, href, (int)sizeof(href));
    /* MOUSEOUT on the node we are leaving, then MOUSEOVER on the node we
       entered. With no listeners both dispatches are inert. The leaving node
       is validated by generation: if a listener removed it and its slot was
       recycled before this pass, the handle now names the NEW occupant, so the
       MOUSEOUT is skipped instead of being delivered to an unrelated node. */
    if (node != g_hover_node) {
        if (g_hover_node > 0 && rb_dom_generation(g_hover_node) == g_hover_gen)
            rb_dispatch_ex(g_hover_node, RB_EV_MOUSEOUT, 0, cx, cy, -1, g_btn, 0);
        if (node > 0) rb_dispatch_ex(node, RB_EV_MOUSEOVER, 0, cx, cy, -1, g_btn, 0);
        g_hover_node = node;
        g_hover_gen = node > 0 ? (uint16_t)rb_dom_generation(node) : 0;
    }
    /* MOUSEMOVE only when the pointer's screen position changed: the packet
       handler runs far more often than the pointer actually moves, and
       update_hover runs every main-loop pass, so without this guard the same
       event position floods the page. Over chrome/scrollbar there is no
       target node, so nothing is dispatched there. */
    if (g_mx != g_mm_x || g_my != g_mm_y) {
        int first = (g_mm_x < 0);
        g_mm_x = g_mx;
        g_mm_y = g_my;
        if (!first && node > 0)
            rb_dispatch_ex(node, RB_EV_MOUSEMOVE, 0, cx, cy,
                           g_left ? 0 : -1, g_btn, 0);
    }
    if (strcmp(href, g_hover) != 0) {
        copy_str(g_hover, (int)sizeof(g_hover), href);
        g_dirty = 1;
    }
}

/* PS/2 packet bit -> rb_event_button index: bit0 left=0, bit1 right=2,
   bit2 middle=1 (0 when no or several bits are set — MOUSEDOWN/MOUSEUP are
   then dispatched with -1 and the mask still tells the truth). */
static int btn_index(uint8_t bits) {
    if (bits == 1) return 0;
    if (bits == 4) return 1;
    if (bits == 2) return 2;
    return -1;
}

static void on_press(void) {
    g_press_x = g_mx;
    g_press_y = g_my;
    g_press_scroll = g_scroll;
    g_moved = 0;
    g_press_zone = zone_at(g_mx, g_my);
    if (g_press_zone == 1) {
        int cx = 0, cy = 0;
        int node = content_hit(&cx, &cy);
        if (node > 0) rb_dispatch_ex(node, RB_EV_MOUSEDOWN, 0, cx, cy, 0, g_btn, 0);
    }
}

static void on_drag(void) {
    int dx = g_mx - g_press_x;
    int dy = g_my - g_press_y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx + dy > 3) g_moved = 1;
    if (!g_moved) return;
    if (g_press_zone == 1) {
        g_scroll = g_press_scroll - (g_my - g_press_y);
        clamp_scroll();
    } else if (g_press_zone == 2) {
        int ph = rb_page_height();
        int maxs = ph - rb_ui.content_h;
        if (maxs < 1) maxs = 1;
        g_scroll = g_press_scroll + (int)((long long)(g_my - g_press_y) * maxs / rb_ui.content_h);
        clamp_scroll();
    }
}

static void on_release(void) {
    int cx = 0, cy = 0;
    int node = content_hit(&cx, &cy);
    if (node > 0) rb_dispatch_ex(node, RB_EV_MOUSEUP, 0, cx, cy, 0, g_btn, 0);
    /* Preventing MOUSEDOWN or MOUSEUP does not suppress the subsequent
       CLICK: this matches the real DOM, where preventDefault on
       mousedown/mouseup affects selection and drag but leaves click delivery
       untouched. Pinned engine-side in test_click_prevent_semantics; a page
       that wants to cancel a click from the press must prevent the CLICK
       event itself (or the click's own listener must check its state). */
    if (!g_moved) click_at(g_mx, g_my);
    g_press_zone = 0;
}

/* Right/middle button make/break: event dispatch only, no press/drag/click
   state (the browser's drag-scroll belongs to the left button, matching what
   on_press/on_release guard). g_left never sees these bits. */
static void on_btn_edges(void) {
    uint8_t pressed = (uint8_t)(g_btn & (uint8_t)~g_btn_was & 0x06);
    uint8_t released = (uint8_t)(g_btn_was & (uint8_t)~g_btn & 0x06);
    if (!pressed && !released) return;
    int cx = 0, cy = 0;
    int node = content_hit(&cx, &cy);
    if (node <= 0) return;
    for (int bit = 2; bit <= 4; bit <<= 1) {
        if (pressed & bit)
            rb_dispatch_ex(node, RB_EV_MOUSEDOWN, 0, cx, cy, btn_index((uint8_t)bit), g_btn, 0);
        if (released & bit)
            rb_dispatch_ex(node, RB_EV_MOUSEUP, 0, cx, cy, btn_index((uint8_t)bit), g_btn, 0);
    }
}

/* Keyboard events target the focused DOM element; the document root (1) is
   the target when nothing is focused or the address bar owns focus. Only the
   shift modifier is tracked (the PS/2 firmware handles ctrl/alt chords
   without reporting them), so mods carries RB_MOD_SHIFT at most. */
static int key_target(void) {
    if (g_url_focus) return 1;
    int n = focus_node();
    return n > 0 ? n : 1;
}

static void key_ev(int type, int ch) {
    rb_dispatch_ex(key_target(), type, ch, 0, 0, -1, 0,
                   g_shift ? RB_MOD_SHIFT : 0);
}

/* Make-code event for one already-decoded key on a fixed target: fires
   KEYDOWN (always) then KEYPRESS (only for a printing character that survived
   keydown). Returns 1 when a listener called preventDefault on either, so the
   caller must skip the key's default action. The target is passed in (captured
   once by the caller) so KEYDOWN, KEYPRESS and the later KEYUP share it even
   if a listener moves focus mid-sequence.

   Auto-repeat: the PS/2 firmware sends bare make codes while a key is held,
   so KEYDOWN (and KEYPRESS) repeat with no KEYUP in between. This is kept
   deliberately: the real DOM also fires a stream of keydown/keypress events
   for a held key (event.repeat) and a single keyup on release. A handler that
   must run once per physical press can filter on its own make/break state. */
static int key_make(int target, int ch) {
    int mods = g_shift ? RB_MOD_SHIFT : 0;
    rb_dispatch_ex(target, RB_EV_KEYDOWN, ch, 0, 0, -1, 0, mods);
    if (rb_last_dispatch_prevented()) return 1;
    if (ch) {
        rb_dispatch_ex(target, RB_EV_KEYPRESS, ch, 0, 0, -1, 0, mods);
        /* A KEYPRESS listener can cancel the key's default action too (the
           character insertion); honour it the same way KEYDOWN is honoured. */
        if (rb_last_dispatch_prevented()) return 1;
    }
    return 0;
}

static int handle_key(uint8_t sc) {
    if (sc == 0xE0) {
        g_ext = 1;
        return 0;
    }
    if (sc == 0x2A || sc == 0x36) {
        g_shift = 1;
        key_ev(RB_EV_KEYDOWN, 0);
        g_ext = 0;
        return 0;
    }
    if (sc == 0xAA || sc == 0xB6) {
        g_shift = 0;
        key_ev(RB_EV_KEYUP, 0);
        g_ext = 0;
        return 0;
    }
    if (sc & 0x80) {
        /* Break code: KEYUP with the CHARACTER and TARGET seen at make time,
           not a fresh decode — the shift state at break time often differs
           (the shift break arrives before a letter break a fast typist
           produces). Keyed by (extended, scancode) so main and numpad '/'
           cannot collide; ch 0 = no make seen (drain_ps2). */
        int mi = (g_ext ? 0x80 : 0) | (sc & 0x7F);
        char up = (char)g_keydn[mi].ch;
        int t = g_keydn[mi].target;
        g_keydn[mi].ch = 0;
        g_keydn[mi].target = 0;
        rb_dispatch_ex(t > 0 ? t : key_target(), RB_EV_KEYUP, up, 0, 0, -1, 0,
                       g_shift ? RB_MOD_SHIFT : 0);
        g_ext = 0;
        return 0;
    }
    int ext = g_ext;
    g_ext = 0;
    g_dirty = 1;
    char ch = scancode_char(sc);
    /* Memoise the decoded character and the target for the matching break. */
    int mi = (ext ? 0x80 : 0) | sc;
    int target = key_target();
    g_keydn[mi].ch = (uint8_t)ch;
    g_keydn[mi].target = (uint16_t)target;
    int kd_prevented = key_make(target, ch);
    /* Esc (sc 0x01) is the browser's own escape hatch: the page still sees its
       KEYDOWN, but the quit below always runs — a page must never be able to
       trap the user inside the browser. */
    if (sc == 0x01) return 1;

    /* UI chrome runs OUTSIDE the preventDefault gate, for the same reason Esc
       is unpreventable: the address bar and Tab traversal are the browser's
       own UI, and a page that prevents every KEYDOWN must not be able to lock
       the user out of the only ways to navigate away. Only the page-directed
       defaults below honour preventDefault. */
    if (sc == 0x0F) {
        focus_tab(g_shift);
        return 0;
    }
    /* Address-bar editing is chrome: submit, edit and type run outside the
       gate (a page must not be able to lock the URL bar). */
    if (g_url_focus && sc == 0x1C) { go_url(); return 0; }
    if (g_url_focus && sc == 0x0E) { do_backspace(); return 0; }
    if (g_url_focus && ch) { type_char(ch); return 0; }
    /* Everything below is page-directed (typing into a page field, Enter
       submit, Backspace, arrow/Page scrolling) and honours preventDefault. */
    if (kd_prevented) return 0;

    if (sc == 0x1C) {
        activate_focus();
        return 0;
    }
    if (sc == 0x0E) {
        do_backspace();
        return 0;
    }
    if (sc == 0x48 || (ext && sc == 0x48)) {
        g_scroll -= line_step();
        clamp_scroll();
        return 0;
    }
    if (sc == 0x50 || (ext && sc == 0x50)) {
        g_scroll += line_step();
        clamp_scroll();
        return 0;
    }
    if (sc == 0x49) {
        g_scroll -= rb_ui.content_h - line_step();
        clamp_scroll();
        return 0;
    }
    if (sc == 0x51) {
        g_scroll += rb_ui.content_h - line_step();
        clamp_scroll();
        return 0;
    }
    if (ch) type_char(ch);
    return 0;
}

static void reset_input(void) {
    /* Esc stores the quit signal in g_left. It is static, so a later
       launch would see it and leave on the first poll. */
    g_left = 0;
    g_left_was = 0;
    g_shift = 0;
    g_ext = 0;
    g_pkt_n = 0;
    g_moved = 0;
    g_press_zone = 0;
    g_btn = 0;
    g_btn_was = 0;
    /* A stale make-memo from a previous run must not leak into a KEYUP. */
    for (int i = 0; i < 0x100; i++) { g_keydn[i].ch = 0; g_keydn[i].target = 0; }
    g_mm_x = -1;
    g_mm_y = -1;
    g_dirty = 1;
}

static void drain_ps2(void) {
    for (int i = 0; i < 64; i++) {
        if ((inb(0x64) & 1) == 0) break;
        inb(0x60);
    }
    g_pkt_n = 0;
    g_left = 0;
    g_left_was = 0;
}

static void poll_input(void) {
    for (int n = 0; n < 8; n++) {
        uint8_t st = inb(0x64);
        if ((st & 1) == 0) break;
        uint8_t b = inb(0x60);
        if (st & 0x20) mouse_byte(b);
        else if (handle_key(b)) {
            g_left = -1;
            return;
        }
    }
    if (g_left < 0) return;
    if (g_left && !g_left_was) on_press();
    if (g_left && g_left_was) on_drag();
    if (!g_left && g_left_was) on_release();
    g_left_was = g_left;
    if (g_btn != g_btn_was) {
        on_btn_edges();
        g_btn_was = g_btn;
    }
}

/* ---- fault recovery ---------------------------------------------------------
   The browser runs under a fault guard: a CPU exception inside it, or the
   main loop not coming back for RB_WATCHDOG_MS, resumes at the fg_setjmp in
   rbrowse() instead of freezing the machine. Network waits pause the
   watchdog. Three crashes in quick succession (say, the error page itself
   faulting) give up and leave the browser. */
#define RB_WATCHDOG_MS 10000
#define RB_CRASH_BURST 3
#define RB_CRASH_WINDOW_MS 3000

static FgJmp g_guard;
static int g_crash_n;
static uint32_t g_crash_ms;

static int recover_from_crash(void) {
    uint32_t now = get_time_ms();
    if (g_crash_n > 0 && now - g_crash_ms < RB_CRASH_WINDOW_MS) g_crash_n++;
    else g_crash_n = 1;
    g_crash_ms = now;
    if (g_crash_n > RB_CRASH_BURST) return 0;
    rb_crash_reset();
    rjs_abandon();
    drain_ps2();
    g_left = 0;
    g_btn = g_btn_was = 0;
    char why[120];
    fg_describe(why, (int)sizeof(why));
    char detail[260];
    int w = 0;
    append_str(detail, (int)sizeof(detail), &w, why);
    append_str(detail, (int)sizeof(detail), &w,
               " rbrowse caught it and reset the page. Press Back or open another address.");
    /* The page that was loading is the current history entry. */
    const char *where = g_hist_n > 0 ? g_hist[g_hist_i] : (g_current[0] ? g_current : "about:blank");
    int hang = fg_last_report()->kind == FG_KIND_HANG;
    show_message(where, hang ? "Page stopped responding" : "Page crashed", detail);
    set_status(hang ? "Recovered from a hang" : "Recovered from a crash");
    g_dirty = 1;
    return 1;
}

void rbrowse(int argc, char *argv[]) {
    if (argc >= 2 && argv[1] && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print("rbrowse [--vga] [url|path]\n");
        print("  640x360 truecolor browser (Bochs/QEMU VBE), 320x200 VGA otherwise. Esc quits.\n");
        print("  --vga forces the 320x200 mode 13h fallback.\n");
        print("  Tab / Shift+Tab move focus. Enter or Space activates.\n");
        print("  Examples: rbrowse  |  rbrowse /home/user/hello.html  |  rbrowse http://example.com/\n");
        return;
    }
    int force_vga = 0;
    if (argc >= 2 && argv[1] && strcmp(argv[1], "--vga") == 0) {
        force_vga = 1;
        argc--;
        argv++;
    }
    print("rbrowse: Esc quits.\n");
    /* The shell runs us inside the timer interrupt with IRQs off, which would
       freeze the clock (banner countdown, caret blink, fault-guard watchdog).
       Turn them on for the session and hold the scheduler so the nested ticks
       only count time; the caller's IF is restored on the way out. */
    uint32_t saved_flags;
    __asm__ volatile("pushf; pop %0" : "=r"(saved_flags));
    sched_hold_push();
    __asm__ volatile("sti");
    reset_input();
    rb_set_scripting(1);
    rb_set_images(1);
    rb_paint_set_images(rb_image_view_node);
    const char *start = "/home/user/rbrowse.html";
    if (argc >= 2 && argv[1] && argv[1][0]) {
        const char *arg = argv[1];
        if (starts(arg, "http://") || starts(arg, "https://") || starts(arg, "file://") ||
            starts(arg, "about:") || arg[0] == '/') {
            start = arg;
        } else {
            const char *cwd = avfs_getcwd();
            if (!cwd || !cwd[0]) cwd = "/";
            int w = 0;
            while (cwd[w] && w + 1 < (int)sizeof(g_startbuf)) {
                g_startbuf[w] = cwd[w];
                w++;
            }
            if (w == 0 || g_startbuf[w - 1] != '/') {
                if (w + 1 < (int)sizeof(g_startbuf)) g_startbuf[w++] = '/';
            }
            int i = 0;
            while (arg[i] && w + 1 < (int)sizeof(g_startbuf)) g_startbuf[w++] = arg[i++];
            g_startbuf[w] = 0;
            start = g_startbuf;
        }
    } else if (!avfs_file_exists("/home/user/rbrowse.html")) {
        start = "/home/user/hello.html";
    }

    video_enter(force_vga);
    show_unsafe_banner();
    mouse_init();
    g_hist_n = 0;
    g_hist_i = 0;
    g_current[0] = 0;
    g_crash_n = 0;
    set_status(g_mouse_ok ? "Tab focus  Enter open  Esc quit" : "Keyboard only  Tab focus  Esc quit");
    volatile int gave_up = 0;
    if (fg_setjmp(&g_guard)) {
        /* Resumed here after a fault or hang anywhere below. */
        fg_arm(&g_guard, RB_WATCHDOG_MS);
        if (!recover_from_crash()) gave_up = 1;
    } else {
        fg_arm(&g_guard, RB_WATCHDOG_MS);
        follow(start, 1, 1);
    }
    if (!gave_up) {
        paint();
        drain_ps2();
    }

    int quit = gave_up;
    int last_blink = 0;
    while (!quit) {
        fg_pet();
        poll_input();
        if (g_left >= 0) js_turn();
        if (g_left >= 0) img_turn();
        if (g_left < 0) {
            quit = 1;
            break;
        }
        update_hover();
        int blink = (int)((get_ticks() / 400) & 1);
        if (g_dirty || blink != last_blink) {
            last_blink = blink;
            g_dirty = 0;
            paint();
        }
    }
    fg_disarm();
    rjs_abandon();
    rb_set_scripting(0);
    rb_set_images(0);
    rb_image_reset();
    mouse_off();
    reset_input();
    video_leave();
    watchdog_hud_force_redraw();
    if (gave_up) {
        char why[120];
        fg_describe(why, (int)sizeof(why));
        print("rbrowse: crashed repeatedly, closed. Last: ");
        print(why);
        print("\n");
    }
    print("rbrowse closed.\n");
    if (!(saved_flags & 0x200)) __asm__ volatile("cli");
    sched_hold_pop();
}
