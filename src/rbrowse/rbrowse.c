// rbrowse — mode 13h (320x200, 256-color) browser.
//
// Page layout, hit testing, and the small script subset live in engine.c.
// This file owns the framebuffer, PS/2 mouse, and keyboard focus.

#include "engine.h"
#include "../vga/vga_gfx.h"
#include "../terminal/terminal.h"
#include "../Avfs/Avfs.h"
#include "../io/io.h"
#include "../timers/timer.h"
#include "../keyboard/keyboard.h"

#include <stdint.h>

extern uint8_t vga_font_8x8[256][8];
extern int rbrowse_http_get(const uint8_t *url, uint8_t *out, int cap);
extern void watchdog_hud_force_redraw(void);

#define FB_W 320
#define FB_H 200
#define CHROME_H 16
#define STATUS_Y 192
#define CONTENT_Y 16
#define CONTENT_H 176
#define SCROLL_X 312
#define VIEW_X 4

enum {
    UI_BLACK = 0,
    UI_CHROME = 2,
    UI_LINE = 3,
    UI_URL = 4,
    UI_URL_TEXT = 5,
    UI_BTN = 6,
    UI_BTN_TEXT = 7,
    UI_STATUS = 8,
    UI_STATUS_TEXT = 9,
    UI_FOCUS = 10,
    UI_WHITE = 14,
    UI_SCROLL = 11,
    UI_THUMB = 12
};

static uint8_t g_fb[FB_W * FB_H];
static char g_page[40 * 1024];
/* Full response, before script/style are stripped into g_page. */
static char g_fetch[512 * 1024];
static char g_current[384];
static char g_startbuf[384];
static char g_url[384];
static int g_url_len;
static char g_status[80];
static char g_hist[8][384];
static int g_hist_n;
static int g_hist_i;
static int g_scroll;
static int g_url_focus;
static int g_focus;
static int g_mx = 160;
static int g_my = 100;
static int g_left;
static int g_left_was;
static int g_press_x, g_press_y, g_press_scroll;
static int g_moved;
static int g_press_zone;
static int g_wheel_bytes = 3;
static int g_mouse_ok;
static uint8_t g_pkt[4];
static int g_pkt_n;
static int g_shift;
static int g_ext;
static int g_dirty = 1;

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

static void install_palette(void) {
    vga_gfx_set_palette_color_rgb888(UI_BLACK, 0, 0, 0);
    vga_gfx_set_palette_color_rgb888(1, 18, 12, 6);
    vga_gfx_set_palette_color_rgb888(UI_CHROME, 26, 16, 8);
    vga_gfx_set_palette_color_rgb888(UI_LINE, 122, 48, 16);
    vga_gfx_set_palette_color_rgb888(UI_URL, 30, 16, 8);
    vga_gfx_set_palette_color_rgb888(UI_URL_TEXT, 255, 221, 187);
    vga_gfx_set_palette_color_rgb888(UI_BTN, 92, 34, 0);
    vga_gfx_set_palette_color_rgb888(UI_BTN_TEXT, 255, 204, 170);
    vga_gfx_set_palette_color_rgb888(UI_STATUS, 15, 8, 4);
    vga_gfx_set_palette_color_rgb888(UI_STATUS_TEXT, 153, 102, 68);
    vga_gfx_set_palette_color_rgb888(UI_FOCUS, 255, 180, 40);
    vga_gfx_set_palette_color_rgb888(UI_SCROLL, 26, 14, 6);
    vga_gfx_set_palette_color_rgb888(UI_THUMB, 122, 58, 16);
    vga_gfx_set_palette_color_rgb888(13, 80, 80, 80);
    vga_gfx_set_palette_color_rgb888(UI_WHITE, 255, 255, 255);
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

static void px(int x, int y, uint8_t c) {
    if ((unsigned)x >= FB_W || (unsigned)y >= FB_H) return;
    g_fb[y * FB_W + x] = c;
}

static void rect(int x, int y, int w, int h, uint8_t c, int x0, int y0, int x1, int y1) {
    for (int yy = y; yy < y + h; yy++) {
        if (yy < y0 || yy >= y1) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < x0 || xx >= x1) continue;
            g_fb[yy * FB_W + xx] = c;
        }
    }
}

static void frame_rect(int x, int y, int w, int h, uint8_t c) {
    for (int i = 0; i < w; i++) {
        px(x + i, y, c);
        px(x + i, y + h - 1, c);
    }
    for (int i = 0; i < h; i++) {
        px(x, y + i, c);
        px(x + w - 1, y + i, c);
    }
}

static void glyph(int x, int y, int ch, uint8_t color, int scale, int x0, int y0, int x1, int y1) {
    if (ch < 0 || ch > 255) ch = '?';
    if (scale < 1) scale = 1;
    const uint8_t *g = vga_font_8x8[ch];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        /* Font bytes are stored LSB = leftmost pixel. Text mode bit-reverses
           them into the VGA plane (which shifts MSB out first). */
        for (int col = 0; col < 8; col++) {
            if ((bits & (1u << col)) == 0) continue;
            for (int sy = 0; sy < scale; sy++) {
                int py = y + row * scale + sy;
                if (py < y0 || py >= y1) continue;
                for (int sx = 0; sx < scale; sx++) {
                    int px_ = x + col * scale + sx;
                    if (px_ < x0 || px_ >= x1) continue;
                    g_fb[py * FB_W + px_] = color;
                }
            }
        }
    }
}

static void text_at(int x, int y, const char *s, uint8_t color, int x0, int y0, int x1, int y1) {
    int cx = x;
    while (s && *s) {
        glyph(cx, y, (unsigned char)*s, color, 1, x0, y0, x1, y1);
        cx += 8;
        s++;
    }
}

static void clamp_scroll(void) {
    int maxs = rb_page_height() - CONTENT_H;
    if (maxs < 0) maxs = 0;
    if (g_scroll < 0) g_scroll = 0;
    if (g_scroll > maxs) g_scroll = maxs;
}

static void thumb_geom(int *ty, int *th) {
    int ph = rb_page_height();
    if (ph <= CONTENT_H || ph <= 0) {
        *ty = CONTENT_Y;
        *th = CONTENT_H;
        return;
    }
    int h = CONTENT_H * CONTENT_H / ph;
    if (h < 10) h = 10;
    if (h > CONTENT_H) h = CONTENT_H;
    int travel = CONTENT_H - h;
    int maxs = ph - CONTENT_H;
    *th = h;
    *ty = CONTENT_Y + (maxs ? g_scroll * travel / maxs : 0);
}

static void draw_cursor(void) {
    static const uint8_t bits[12] = {
        0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xF0, 0xD8, 0x98, 0x08, 0x18, 0x10
    };
    for (int pass = 0; pass < 2; pass++) {
        int ox = pass ? g_mx + 1 : g_mx;
        int oy = pass ? g_my + 1 : g_my;
        uint8_t col = pass ? UI_BLACK : UI_WHITE;
        for (int row = 0; row < 12; row++) {
            for (int colb = 0; colb < 8; colb++) {
                if (bits[row] & (0x80 >> colb)) px(ox + colb, oy + row, col);
            }
        }
    }
}

static void paint(void) {
    rect(0, 0, FB_W, CHROME_H, UI_CHROME, 0, 0, FB_W, FB_H);
    rect(0, CONTENT_Y, SCROLL_X, CONTENT_H, (uint8_t)rb_page_bg(), 0, 0, FB_W, FB_H);
    rect(SCROLL_X, CONTENT_Y, 8, CONTENT_H, UI_SCROLL, 0, 0, FB_W, FB_H);
    rect(0, STATUS_Y, FB_W, 8, UI_STATUS, 0, 0, FB_W, FB_H);
    px(0, CHROME_H - 1, UI_LINE);
    for (int x = 0; x < FB_W; x++) g_fb[(CHROME_H - 1) * FB_W + x] = UI_LINE;

    rect(2, 2, 14, 12, UI_BTN, 0, 0, FB_W, FB_H);
    text_at(5, 4, "<", UI_BTN_TEXT, 0, 0, FB_W, FB_H);
    rect(18, 2, 250, 12, UI_URL, 0, 0, FB_W, FB_H);
    if (g_url_focus) frame_rect(18, 2, 250, 12, UI_FOCUS);
    rect(272, 2, 38, 12, UI_BTN, 0, 0, FB_W, FB_H);
    text_at(280, 4, "Go", UI_BTN_TEXT, 0, 0, FB_W, FB_H);

    char shown[40];
    int maxc = 30;
    int start = 0;
    if (g_url_len > maxc) start = g_url_len - maxc;
    int n = 0;
    while (g_url[start + n] && n < maxc) {
        shown[n] = g_url[start + n];
        n++;
    }
    shown[n] = 0;
    text_at(22, 4, shown, UI_URL_TEXT, 19, 3, 266, 14);
    if (g_url_focus && ((get_ticks() / 400) & 1)) {
        int cx = 22 + n * 8;
        if (cx < 264) rect(cx, 4, 1, 8, UI_FOCUS, 0, 0, FB_W, FB_H);
    }

    int boxes = rb_box_count();
    for (int i = 0; i < boxes; i++) {
        const RbBox *b = rb_box(i);
        if (!b) continue;
        int x = VIEW_X + b->x;
        int y = CONTENT_Y + b->y - g_scroll;
        if (b->kind == RB_BOX_BG) {
            rect(x, y, b->w, b->h, b->bg, 0, CONTENT_Y, SCROLL_X, STATUS_Y);
        } else if (b->kind == RB_BOX_RULE) {
            rect(x, y, b->w, b->h > 0 ? b->h : 1, UI_LINE, 0, CONTENT_Y, SCROLL_X, STATUS_Y);
        } else if (b->kind == RB_BOX_CONTROL) {
            rect(x, y, b->w, b->h, b->bg, 0, CONTENT_Y, SCROLL_X, STATUS_Y);
            frame_rect(x, y, b->w, b->h, UI_LINE);
        } else if (b->kind == RB_BOX_TEXT) {
            int len = 0;
            const char *t = rb_box_text(i, &len);
            int scale = b->scale ? b->scale : 1;
            for (int k = 0; k < len; k++) {
                glyph(x + k * 8 * scale, y, (unsigned char)t[k], b->color, scale,
                      0, CONTENT_Y, SCROLL_X, STATUS_Y);
            }
            if (rb_node_tag(b->node) == RB_TAG_A) {
                rect(x, y + 8 * scale - 1, len * 8 * scale, 1, b->color, 0, CONTENT_Y, SCROLL_X, STATUS_Y);
            }
        }
    }

    if (!g_url_focus && g_focus >= 0 && g_focus < rb_focus_count()) {
        int node = rb_focus_node(g_focus);
        int minx = 1000, miny = 1000, maxx = -1, maxy = -1;
        for (int i = 0; i < boxes; i++) {
            const RbBox *b = rb_box(i);
            if (!b || b->node != node || b->kind == RB_BOX_BG) continue;
            int x = VIEW_X + b->x;
            int y = CONTENT_Y + b->y - g_scroll;
            if (x < minx) minx = x;
            if (y < miny) miny = y;
            if (x + b->w > maxx) maxx = x + b->w;
            if (y + b->h > maxy) maxy = y + b->h;
        }
        if (maxx > minx && maxy > miny) {
            if (miny < CONTENT_Y) miny = CONTENT_Y;
            if (maxy > STATUS_Y) maxy = STATUS_Y;
            frame_rect(minx - 1, miny - 1, (maxx - minx) + 2, (maxy - miny) + 2, UI_FOCUS);
        }
    }

    int ty, th;
    thumb_geom(&ty, &th);
    rect(SCROLL_X + 1, ty, 6, th, UI_THUMB, 0, 0, FB_W, FB_H);

    text_at(2, STATUS_Y, g_status, UI_STATUS_TEXT, 0, STATUS_Y, FB_W, FB_H);
    if (!g_mouse_ok) text_at(250, STATUS_Y, "no mouse", UI_STATUS_TEXT, 0, STATUS_Y, FB_W, FB_H);
    draw_cursor();
    vga_gfx_present(g_fb);
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
    if (g_mx > FB_W - 1) g_mx = FB_W - 1;
    if (g_my > FB_H - 1) g_my = FB_H - 1;
}

static void on_mouse_packet(void) {
    if (g_pkt[0] & 0xC0) return;
    int dx = (int)g_pkt[1];
    int dy = (int)g_pkt[2];
    if (g_pkt[0] & 0x10) dx -= 256;
    if (g_pkt[0] & 0x20) dy -= 256;
    if (dx > 40) dx = 40;
    if (dx < -40) dx = -40;
    if (dy > 40) dy = 40;
    if (dy < -40) dy = -40;
    g_mx += dx;
    g_my -= dy;
    clamp_mouse();
    g_left = (g_pkt[0] & 1) ? 1 : 0;
    g_dirty = 1;
    if (g_wheel_bytes == 4) {
        int z = (int)(int8_t)g_pkt[3];
        g_scroll -= z * 12;
        clamp_scroll();
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

static void open_html(const char *html, int len, const char *canonical) {
    rb_open(html, len);
    copy_str(g_current, (int)sizeof(g_current), canonical);
    set_url_bar(canonical);
    g_scroll = 0;
    g_url_focus = 0;
    g_focus = rb_focus_count() > 0 ? 0 : -1;
    char title[80];
    if (rb_title(title, (int)sizeof(title)) > 0) set_status(title);
    else set_status("Tab focus  Enter open  Esc quit");
}

static void show_message(const char *canonical, const char *heading, const char *detail) {
    char html[700];
    int n = 0;
    const char *a = "<html><body style=\"background-color:#120C06;color:#DDCCBB\"><h1>";
    const char *b = "</h1><p>";
    const char *c = "</p></body></html>";
    while (*a && n < 680) html[n++] = *a++;
    while (*heading && n < 680) html[n++] = *heading++;
    while (*b && n < 680) html[n++] = *b++;
    while (*detail && n < 680) html[n++] = *detail++;
    while (*c && n < 690) html[n++] = *c++;
    html[n] = 0;
    open_html(html, n, canonical);
}

static void open_fetched(const char *canonical, int n) {
    int m = rb_compact_markup(g_fetch, n, g_page, (int)sizeof(g_page));
    open_html(g_page, m, canonical);
}

static int load_canonical(const char *canonical) {
    if (starts(canonical, "http://") || starts(canonical, "https://")) {
        set_status("Loading...");
        paint();
        int n = rbrowse_http_get((const uint8_t *)canonical, (uint8_t *)g_fetch, (int)sizeof(g_fetch));
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

static void follow(const char *href, int push) {
    char resolved[384];
    if (!href || !href[0]) return;
    if (starts(href, "http://") || starts(href, "https://") || href[0] == '/' || starts(href, "file://")) {
        if (starts(href, "file://")) {
            if (!rb_resolve(g_current, href, resolved, (int)sizeof(resolved))) return;
        } else {
            copy_str(resolved, (int)sizeof(resolved), href);
            if (href[0] != '/' && !starts(href, "http://") && !starts(href, "https://")) {
                if (!rb_resolve(g_current, href, resolved, (int)sizeof(resolved))) return;
            }
        }
    } else if (!rb_resolve(g_current[0] ? g_current : "/", href, resolved, (int)sizeof(resolved))) {
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
        copy_str(cur, (int)sizeof(cur), step);
        if (g_hist_n > 0) copy_str(g_hist[g_hist_i], 384, cur);
    }
}

static void go_url(void) {
    if (g_url_len <= 0) return;
    g_url[g_url_len] = 0;
    follow(g_url, 1);
}

static void activate_focus(void) {
    if (g_url_focus) {
        go_url();
        return;
    }
    if (g_focus < 0) return;
    int node = rb_focus_node(g_focus);
    int r = rb_activate(node);
    if (r == 2) {
        char href[384];
        if (rb_take_nav(href, (int)sizeof(href))) follow(href, 1);
        return;
    }
    if (r == 1) {
        rb_relayout();
        set_status("Updated");
    }
}

static void type_char(char ch) {
    if (g_url_focus) {
        if (g_url_len + 1 < (int)sizeof(g_url)) {
            g_url[g_url_len++] = ch;
            g_url[g_url_len] = 0;
        }
        return;
    }
    if (g_focus >= 0) {
        int node = rb_focus_node(g_focus);
        if (rb_is_field(node) && rb_type_char(node, ch)) {
            rb_relayout();
            return;
        }
    }
    if (ch == ' ') activate_focus();
}

static void do_backspace(void) {
    if (g_url_focus) {
        if (g_url_len > 0) g_url[--g_url_len] = 0;
        return;
    }
    if (g_focus >= 0) {
        int node = rb_focus_node(g_focus);
        if (rb_is_field(node) && rb_backspace(node)) rb_relayout();
    }
}

static void focus_tab(int backward) {
    int n = rb_focus_count();
    int slots = n + 1;
    int pos = g_url_focus ? 0 : (g_focus < 0 ? 0 : g_focus + 1);
    if (backward) pos = (pos - 1 + slots) % slots;
    else pos = (pos + 1) % slots;
    if (pos == 0) {
        g_url_focus = 1;
    } else {
        g_url_focus = 0;
        g_focus = pos - 1;
    }
}

static int zone_at(int x, int y) {
    if (y < CHROME_H) return 3;
    if (y >= STATUS_Y) return 0;
    if (x >= SCROLL_X) return 2;
    return 1;
}

static void click_at(int x, int y) {
    if (y < CHROME_H) {
        if (x < 18) {
            if (g_hist_i > 0) {
                g_hist_i--;
                follow(g_hist[g_hist_i], 0);
            }
            return;
        }
        if (x >= 272) {
            g_url_focus = 1;
            go_url();
            return;
        }
        g_url_focus = 1;
        return;
    }
    if (y >= STATUS_Y) return;
    if (x >= SCROLL_X) {
        int ty, th;
        thumb_geom(&ty, &th);
        if (y < ty) g_scroll -= CONTENT_H / 2;
        else if (y >= ty + th) g_scroll += CONTENT_H / 2;
        clamp_scroll();
        return;
    }
    int dx = x - VIEW_X;
    int dy = y - CONTENT_Y + g_scroll;
    int node = rb_hit(dx, dy);
    if (node < 0) {
        g_url_focus = 0;
        return;
    }
    int idx = rb_focus_index(node);
    if (idx >= 0) {
        g_url_focus = 0;
        g_focus = idx;
    }
    int r = rb_activate(node);
    if (r == 2) {
        char href[384];
        if (rb_take_nav(href, (int)sizeof(href))) follow(href, 1);
        return;
    }
    if (r == 1) rb_relayout();
    char href[160];
    if (rb_node_href(node, href, (int)sizeof(href))) set_status(href);
}

static void hover_status(void) {
    if (g_my < CONTENT_Y || g_my >= STATUS_Y || g_mx >= SCROLL_X) return;
    int node = rb_hit(g_mx - VIEW_X, g_my - CONTENT_Y + g_scroll);
    char href[160];
    if (node > 0 && rb_node_href(node, href, (int)sizeof(href))) set_status(href);
}

static void on_press(void) {
    g_press_x = g_mx;
    g_press_y = g_my;
    g_press_scroll = g_scroll;
    g_moved = 0;
    g_press_zone = zone_at(g_mx, g_my);
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
        int maxs = ph - CONTENT_H;
        if (maxs < 1) maxs = 1;
        g_scroll = g_press_scroll + (g_my - g_press_y) * maxs / CONTENT_H;
        clamp_scroll();
    }
}

static void on_release(void) {
    if (!g_moved) click_at(g_mx, g_my);
    g_press_zone = 0;
}

static int handle_key(uint8_t sc) {
    if (sc == 0xE0) {
        g_ext = 1;
        return 0;
    }
    if (sc == 0x2A || sc == 0x36) {
        g_shift = 1;
        g_ext = 0;
        return 0;
    }
    if (sc == 0xAA || sc == 0xB6) {
        g_shift = 0;
        g_ext = 0;
        return 0;
    }
    if (sc & 0x80) {
        g_ext = 0;
        return 0;
    }
    int ext = g_ext;
    g_ext = 0;
    g_dirty = 1;
    if (sc == 0x01) return 1;
    if (sc == 0x0F) {
        focus_tab(g_shift);
        return 0;
    }
    if (sc == 0x1C) {
        activate_focus();
        return 0;
    }
    if (sc == 0x0E) {
        do_backspace();
        return 0;
    }
    if (sc == 0x48 || (ext && sc == 0x48)) {
        g_scroll -= 16;
        clamp_scroll();
        return 0;
    }
    if (sc == 0x50 || (ext && sc == 0x50)) {
        g_scroll += 16;
        clamp_scroll();
        return 0;
    }
    if (sc == 0x49) {
        g_scroll -= CONTENT_H - 16;
        clamp_scroll();
        return 0;
    }
    if (sc == 0x51) {
        g_scroll += CONTENT_H - 16;
        clamp_scroll();
        return 0;
    }
    char ch = scancode_char(sc);
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
}

void rbrowse(int argc, char *argv[]) {
    if (argc >= 2 && argv[1] && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print("rbrowse [url|path]\n");
        print("  VGA mode 13h browser. Esc quits.\n");
        print("  Tab / Shift+Tab move focus. Enter or Space activates.\n");
        print("  Examples: rbrowse  |  rbrowse /home/user/hello.html  |  rbrowse http://example.com/\n");
        return;
    }
    print("rbrowse: mode 13h. Esc quits.\n");
    reset_input();
    const char *start = "/home/user/rbrowse.html";
    if (argc >= 2 && argv[1] && argv[1][0]) {
        const char *arg = argv[1];
        if (starts(arg, "http://") || starts(arg, "https://") || starts(arg, "file://") || arg[0] == '/') {
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

    vga_gfx_set_mode13h();
    install_palette();
    mouse_init();
    g_hist_n = 0;
    g_hist_i = 0;
    g_current[0] = 0;
    set_status(g_mouse_ok ? "Tab focus  Enter open  Esc quit" : "Keyboard only  Tab focus  Esc quit");
    follow(start, 1);
    paint();
    drain_ps2();

    int quit = 0;
    int last_blink = 0;
    while (!quit) {
        poll_input();
        if (g_left < 0) {
            quit = 1;
            break;
        }
        hover_status();
        int blink = (int)((get_ticks() / 400) & 1);
        if (g_dirty || blink != last_blink) {
            last_blink = blink;
            g_dirty = 0;
            paint();
        }
    }
    mouse_off();
    reset_input();
    vga_gfx_set_text_mode();
    watchdog_hud_force_redraw();
    print("rbrowse closed.\n");
}
