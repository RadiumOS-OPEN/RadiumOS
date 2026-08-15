#include "terminal.h"
#include <stddef.h> // For size_t
#include "../utility/utility.h" // For strlen
#include "../timers/timer.h"
#include "../io/io.h"
#include "../commands/cowsay.h"
#include "../keyboard/keyboard.h"
#include <stdbool.h> // For bool type
#include <stdarg.h>  // For va_list
#include <limits.h> 

// Default terminal size
extern size_t terminal_width = 80;
extern size_t terminal_height = 25;
static char line_buffer[80];
static int line_index = 0;
size_t terminal_row;
size_t terminal_column;
uint8_t terminal_color;
uint16_t* terminal_buffer;

char _binary_font_psf_start[];
char _binary_font_psf_end[];
typedef struct {
    uint32_t magic;       /* magic bytes to identify PSF */
    uint32_t version;     /* zero */
    uint32_t headersize;  /* offset of bitmaps in file, 32 */
    uint32_t flags;       /* 0 if there's no unicode table */
    uint32_t numglyph;    /* number of glyphs */
    uint32_t bytesperglyph; /* size of each glyph */
    uint32_t height;      /* height in pixels */
    uint32_t width;       /* width in pixels */
} PSF_font;
#define PSF_FONT_MAGIC 0x864ab572
uint16_t* unicode_table = NULL;

/* ── VGA colour helpers ───────────────────────────────────────────── */

/* Set the foreground colour, preserving the current background. */
void set_text_color(uint8_t fg) {
    uint8_t bg = (terminal_color >> 4) & 0x0F;
    terminal_color = vga_entry_color((enum vga_color)fg, (enum vga_color)bg);
}

/* Set background colour, preserving current foreground. */
void set_bg_color(uint8_t bg) {
    uint8_t fg = terminal_color & 0x0F;
    terminal_color = vga_entry_color((enum vga_color)fg, (enum vga_color)bg);
}

/* Set both fg and bg at once. */
void set_color(uint8_t fg, uint8_t bg) {
    terminal_color = vga_entry_color((enum vga_color)fg, (enum vga_color)bg);
}

/* Reset to white on black. */
void reset_text_color(void) {
    terminal_color = vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

/* Save/restore color around a block of colored output. */
static uint8_t _saved_color = 0;
void push_color(void) { _saved_color = terminal_color; }
void pop_color(void)  { terminal_color = _saved_color; }

void psf_init() {
    uint16_t glyph = 0;
    PSF_font* font = (PSF_font*)_binary_font_psf_start;
    if (font->magic != PSF_FONT_MAGIC) {
        unicode_table = NULL;
        return;
    }
    if (font->flags == 0) {
        unicode_table = NULL;
        return;
    }
    char* s = (char*)(_binary_font_psf_start + font->headersize + font->numglyph * font->bytesperglyph);
    unicode_table = calloc(USHRT_MAX, sizeof(uint16_t));
    while (s < _binary_font_psf_end) {
        uint16_t uc = (uint8_t)s[0];
        if (uc == 0xFF) {
            glyph++;
            s++;
            continue;
        } else if (uc & 128) {
            // UTF-8 to unicode conversion
            if ((uc & 32) == 0) {
                uc = ((s[0] & 0x1F) << 6) + (s[1] & 0x3F);
                s++;
            } else if ((uc & 16) == 0) {
                uc = ((((s[0] & 0xF) << 6) + (s[1] & 0x3F)) << 6) + (s[2] & 0x3F);
                s += 2;
            } else if ((uc & 8) == 0) {
                uc = (((((s[0] & 0x7) << 6) + (s[1] & 0x3F)) << 6) + (s[2] & 0x3F)) << 6 + (s[3] & 0x3F);
                s += 3;
            } else {
                uc = 0;
            }
        }
        unicode_table[uc] = glyph;
        s++;
    }
}

// 8x8 bitmap font for ASCII 0-255
// Each glyph is 8 bytes, one byte per row, MSB = leftmost pixel
uint8_t vga_font_8x8[256][8] = {
    // 0x00 - 0x1F: control chars, blank glyphs
    [0 ... 31] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },

    // 0x20 space
    [0x20] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    // 0x21 !
    [0x21] = { 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x18, 0x00 },
    // 0x22 "
    [0x22] = { 0x66, 0x66, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00 },
    // 0x23 #
    [0x23] = { 0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00 },
    // 0x24 $
    [0x24] = { 0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00 },
    // 0x25 %
    [0x25] = { 0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00 },
    // 0x26 &
    [0x26] = { 0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00 },
    // 0x27 '
    [0x27] = { 0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00 },
    // 0x28 (
    [0x28] = { 0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00 },
    // 0x29 )
    [0x29] = { 0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00 },
    // 0x2A *
    [0x2A] = { 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00 },
    // 0x2B +
    [0x2B] = { 0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00 },
    // 0x2C ,
    [0x2C] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06 },
    // 0x2D -
    [0x2D] = { 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00 },
    // 0x2E .
    [0x2E] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00 },
    // 0x2F /
    [0x2F] = { 0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00 },

    // 0x30 - 0x39 digits
    [0x30] = { 0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00 }, // 0
    [0x31] = { 0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00 }, // 1
    [0x32] = { 0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00 }, // 2
    [0x33] = { 0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00 }, // 3
    [0x34] = { 0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00 }, // 4
    [0x35] = { 0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00 }, // 5
    [0x36] = { 0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00 }, // 6
    [0x37] = { 0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00 }, // 7
    [0x38] = { 0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00 }, // 8
    [0x39] = { 0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00 }, // 9

    // 0x3A - 0x40
    [0x3A] = { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00 }, // :
    [0x3B] = { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06 }, // ;
    [0x3C] = { 0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00 }, // <
    [0x3D] = { 0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00 }, // =
    [0x3E] = { 0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00 }, // >
    [0x3F] = { 0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00 }, // ?
    [0x40] = { 0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00 }, // @

    // 0x41 - 0x5A uppercase
    [0x41] = { 0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00 }, // A
    [0x42] = { 0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00 }, // B
    [0x43] = { 0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00 }, // C
    [0x44] = { 0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00 }, // D
    [0x45] = { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00 }, // E
    [0x46] = { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00 }, // F
    [0x47] = { 0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00 }, // G
    [0x48] = { 0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00 }, // H
    [0x49] = { 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, // I
    [0x4A] = { 0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00 }, // J
    [0x4B] = { 0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00 }, // K
    [0x4C] = { 0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00 }, // L
    [0x4D] = { 0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00 }, // M
    [0x4E] = { 0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00 }, // N
    [0x4F] = { 0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00 }, // O
    [0x50] = { 0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00 }, // P
    [0x51] = { 0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00 }, // Q
    [0x52] = { 0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00 }, // R
    [0x53] = { 0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00 }, // S
    [0x54] = { 0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, // T
    [0x55] = { 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00 }, // U
    [0x56] = { 0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00 }, // V
    [0x57] = { 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00 }, // W
    [0x58] = { 0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00 }, // X
    [0x59] = { 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00 }, // Y
    [0x5A] = { 0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00 }, // Z

    // 0x5B - 0x60
    [0x5B] = { 0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00 }, // [
    [0x5C] = { 0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00 }, // backslash
    [0x5D] = { 0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00 }, // ]
    [0x5E] = { 0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00 }, // ^
    [0x5F] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF }, // _
    [0x60] = { 0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00 }, // `

    // 0x61 - 0x7A lowercase
    [0x61] = { 0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00 }, // a
    [0x62] = { 0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00 }, // b
    [0x63] = { 0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00 }, // c
    [0x64] = { 0x38, 0x30, 0x30, 0x3e, 0x33, 0x33, 0x6E, 0x00 }, // d
    [0x65] = { 0x00, 0x00, 0x1E, 0x33, 0x3f, 0x03, 0x1E, 0x00 }, // e
    [0x66] = { 0x1C, 0x36, 0x06, 0x0f, 0x06, 0x06, 0x0F, 0x00 }, // f
    [0x67] = { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F }, // g
    [0x68] = { 0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00 }, // h
    [0x69] = { 0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, // i
    [0x6A] = { 0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E }, // j
    [0x6B] = { 0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00 }, // k
    [0x6C] = { 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, // l
    [0x6D] = { 0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00 }, // m
    [0x6E] = { 0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00 }, // n
    [0x6F] = { 0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00 }, // o
    [0x70] = { 0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F }, // p
    [0x71] = { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78 }, // q
    [0x72] = { 0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00 }, // r
    [0x73] = { 0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00 }, // s
    [0x74] = { 0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00 }, // t
    [0x75] = { 0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00 }, // u
    [0x76] = { 0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00 }, // v
    [0x77] = { 0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00 }, // w
    [0x78] = { 0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00 }, // x
    [0x79] = { 0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F }, // y
    [0x7A] = { 0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00 }, // z

    // 0x7B - 0x7E
    [0x7B] = { 0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00 }, // {
    [0x7C] = { 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00 }, // |
    [0x7D] = { 0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00 }, // }
    [0x7E] = { 0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // ~

    // 0x7F - 0xFF: blank
    [0x7F ... 0xFF] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
};
static uint8_t mirror_byte(uint8_t b) {
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4);
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
    return b;
}
void vga_set_80x50(void) {
    // ── Step 1: unlock CRTC registers 0-7 ────────────────────────────────
    outb(0x3D4, 0x03);
    outb(0x3D5, inb(0x3D5) | 0x80);    // set bit 7 of reg 3 (enable retrace)
    outb(0x3D4, 0x11);
    outb(0x3D5, inb(0x3D5) & ~0x80);   // clear bit 7 of reg 11 (unlock 0-7)

    // ── Step 2: set 8x8 font (character height = 8 lines) ────────────────
    outb(0x3D4, 0x09);                  // max scan line
    outb(0x3D5, (inb(0x3D5) & ~0x1F) | 0x07); // bits 0-4 = 7 (8 lines, 0-indexed)

    // ── Step 3: reprogram CRTC for 400 scan lines / 8 = 50 rows ──────────
    // Vertical total = 449
    outb(0x3D4, 0x06); outb(0x3D5, 0xBF);

    // Overflow register — bits for vertical total, display end, sync start
    outb(0x3D4, 0x07); outb(0x3D5, 0x1F);

    // Vertical display end = 399 (400 lines, 0-indexed)
    outb(0x3D4, 0x12); outb(0x3D5, 0x8F);

    // Vertical blank start = 400
    outb(0x3D4, 0x15); outb(0x3D5, 0x96);

    // Vertical blank end = 409
    outb(0x3D4, 0x16); outb(0x3D5, 0xB9);

    // Vertical sync start = 412
    outb(0x3D4, 0x10); outb(0x3D5, 0x9C);

    // Vertical sync end = 414
    outb(0x3D4, 0x11);
    outb(0x3D5, (inb(0x3D5) & ~0x0F) | 0x0E);

    // ── Step 4: cursor shape for 8x8 font ────────────────────────────────
    outb(0x3D4, 0x0A); outb(0x3D5, 0x06); // cursor start line 6
    outb(0x3D4, 0x0B); outb(0x3D5, 0x07); // cursor end   line 7

    // ── Step 5: load 8x8 font into plane 2 via BIOS-style font load ──────
    // Tell the sequencer and graphics controller to access font plane
    outb(0x3C4, 0x02); outb(0x3C5, 0x04); // sequencer: write plane 2
    outb(0x3C4, 0x04); outb(0x3C5, 0x07); // sequencer: sequential, extended mem
    outb(0x3CE, 0x04); outb(0x3CF, 0x02); // graphics: read plane 2
    outb(0x3CE, 0x05); outb(0x3CF, 0x00); // graphics: write mode 0, read mode 0
    outb(0x3CE, 0x06); outb(0x3CF, 0x00); // graphics: map at A000, not B800

    // Copy 8x8 font data into plane 2
    // The VGA font plane is at 0xA0000
    uint8_t* font_plane = (uint8_t*)0xA0000;

    // Built-in 8x8 VGA ROM font — read it from the BIOS area
    // The 8x8 font lives at 0xC000:0x0050 in real mode, but in
    // protected mode we read it from the VGA BIOS shadow at 0xC0050
    // Safer: use our own minimal 8x8 font for printable ASCII
    // We copy 256 glyphs x 8 bytes each = 2048 bytes
    extern uint8_t vga_font_8x8[256][8]; // defined below
    for (int i = 0; i < 256; i++) {
        for (int row = 0; row < 8; row++) {
            font_plane[i * 32 + row] = mirror_byte(vga_font_8x8[i][row]);
        }
        for (int row = 8; row < 32; row++) {
            font_plane[i * 32 + row] = 0;
        }
    }
    // ── Step 6: restore normal text mode plane access ─────────────────────
    outb(0x3C4, 0x02); outb(0x3C5, 0x03); // sequencer: write planes 0+1
    outb(0x3C4, 0x04); outb(0x3C5, 0x03); // sequencer: odd/even, extended mem
    outb(0x3CE, 0x04); outb(0x3CF, 0x00); // graphics: read plane 0
    outb(0x3CE, 0x05); outb(0x3CF, 0x10); // graphics: odd/even mode
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E); // graphics: map at B800
}



void terminal_initialize(void) {
                // must come first — reprograms hardware
    terminal_width  = 80;
    terminal_height = 50;       // 50 rows now
    terminal_row    = 0;
    terminal_column = 0;
    terminal_color  = vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    terminal_buffer = (uint16_t*)0xB8000;
    terminal_clear_inFunction();
}

void terminal_set_cursor_position(size_t position) {
    // VGA CRT Controller ports
    // 0x3D4 is the index register, 0x3D5 is the data register
    
    // Send low byte of cursor position
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(position & 0xFF));
    
    // Send high byte of cursor position
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((position >> 8) & 0xFF));
}


/* ── Terminal Scroll ───────────────────────────────────────────────────── */
void terminal_scroll(void) {
    // 1. Move the entire buffer content up by one row.
    // We loop through every row except the last one, and copy it to the row above.
    for (size_t y = 0; y < terminal_height - 1; y++) {
        for (size_t x = 0; x < terminal_width; x++) {
            terminal_buffer[y * terminal_width + x] = terminal_buffer[(y + 1) * terminal_width + x];
        }
    }

    // 2. Clear the last row (the bottom line) so it's ready for new text.
    for (size_t x = 0; x < terminal_width; x++) {
        terminal_buffer[(terminal_height - 1) * terminal_width + x] = vga_entry(' ', terminal_color);
    }
}




void terminal_update_cursor(void) {
    size_t position = terminal_row * terminal_width + terminal_column;
    terminal_set_cursor_position(position);
}

void terminal_setsize(size_t width, size_t height) {
    terminal_width = width;
    terminal_height = height;

    // Clear the terminal buffer
    for (size_t y = 0; y < terminal_height; y++) {
        for (size_t x = 0; x < terminal_width; x++) {
            const size_t index = y * terminal_width + x;
            terminal_buffer[index] = vga_entry(' ', terminal_color);
        }
    }
}

void terminal_setcolor(uint8_t color) 
{
    terminal_color = color;
}

void terminal_putentryat(char c, uint8_t color, size_t x, size_t y) 
{
    const size_t index = y * terminal_width + x;
    terminal_buffer[index] = vga_entry(c, color);
}
// Helper to extract a line from the VGA buffer and send it to history
void history_append_line_from_terminal(int row) {
    char temp_line[80];
    uint8_t temp_attrs[80];
    
    // Extract characters and attributes from the current row
    for (int x = 0; x < 80; x++) {
        uint16_t entry = terminal_buffer[row * 80 + x];
        temp_line[x] = (char)(entry & 0xFF);
        temp_attrs[x] = (uint8_t)((entry >> 8) & 0xFF);
    }
    
    // Call the function defined in keyboard.c
    // Note: If you want per-char color history, you'd need to modify history_append_line in keyboard.c
    // For now, we just pass the color of the first character as the "line color" to keep it simple.
    history_append_line(temp_line, temp_attrs[0]);
}
void terminal_putchar(char c) {
    // Check for Screen Clear (Ctrl+L sends 0x0C)
    if (c == '\f') {
        terminal_clear();
        return;
    }

    switch (c) {
        case '\n':
            // Save the current line content to history before moving
            history_append_line_from_terminal(terminal_row);
            
            terminal_column = 0;
            if (++terminal_row == terminal_height) {
                terminal_row--;
                terminal_scroll(); // <--- USE NEW FUNCTION
            }
            break;

        case '\r':
            terminal_column = 0;
            break;

        case '\t':
            // Calculate next tab stop (align to 4)
            terminal_column = (terminal_column + 4) & ~3;
            
            // Handle wrapping if Tab goes past screen width
            if (terminal_column >= terminal_width) {
                terminal_column = 0;
                if (++terminal_row == terminal_height) {
                    terminal_row--;
                    terminal_scroll(); // <--- USE NEW FUNCTION
                }
            }
            break;

        case '\b':
            if (terminal_column > 0) {
                terminal_column--;
                terminal_putentryat(' ', terminal_color, terminal_column, terminal_row);
            } else if (terminal_row > 0) {
                terminal_row--;
                terminal_column = terminal_width - 1;
                terminal_putentryat(' ', terminal_color, terminal_column, terminal_row);
            }
            break;

        default:
            terminal_putentryat(c, terminal_color, terminal_column, terminal_row);
            if (++terminal_column == terminal_width) {
                terminal_column = 0;
                if (++terminal_row == terminal_height) {
                    terminal_row--;
                    terminal_scroll(); // <--- USE NEW FUNCTION
                }
            }
            break;
    }
    terminal_update_cursor(); // Ensure hardware cursor follows software cursor
}
void terminal_write(const char* data, size_t size) 
{
    for (size_t i = 0; i < size; i++) {
        terminal_putchar(data[i]);
    }
}

void print(const char* data) 
{
    terminal_write(data, strlen(data));
}


static void int_to_string_base(long long value, char* buffer, int base, int width, char pad_char, bool uppercase) {
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char temp[64];
    int i = 0;
    bool negative = false;
    
    if (value < 0 && base == 10) {
        negative = true;
        value = -value;
    }
    
    if (value == 0) {
        temp[i++] = '0';
    } else {
        while (value > 0) {
            temp[i++] = digits[value % base];
            value /= base;
        }
    }
    
    // Calculate padding
    int num_len = i + (negative ? 1 : 0);
    int padding = (width > num_len) ? width - num_len : 0;
    int pos = 0;
    
    // Add padding (before number for space padding, after sign for zero padding)
    if (pad_char == ' ') {
        for (int j = 0; j < padding; j++) {
            buffer[pos++] = ' ';
        }
    }
    
    // Add negative sign
    if (negative) {
        buffer[pos++] = '-';
    }
    
    // Add zero padding after sign
    if (pad_char == '0') {
        for (int j = 0; j < padding; j++) {
            buffer[pos++] = '0';
        }
    }
    
    // Add digits (reverse order)
    while (i > 0) {
        buffer[pos++] = temp[--i];
    }
    
    buffer[pos] = '\0';
}

// Main printf function (renamed from printr to printf)
void printr(const char* format, ...) {
    va_list args;
    va_start(args, format);
    
    char buffer[1024];
    char* buf_ptr = buffer;
    const char* fmt_ptr = format;
    
    while (*fmt_ptr && (buf_ptr - buffer) < 1023) {
        if (*fmt_ptr == '%' && *(fmt_ptr + 1)) {
            fmt_ptr++; // Skip %
            
            // Parse flags and width
            int width = 0;
            char pad_char = ' ';
            bool left_align = false;
            bool force_sign = false;
            bool space_sign = false;
            bool alternate_form = false;
            
            // Parse flags
            while (1) {
                if (*fmt_ptr == '-') {
                    left_align = true;
                    fmt_ptr++;
                } else if (*fmt_ptr == '+') {
                    force_sign = true;
                    fmt_ptr++;
                } else if (*fmt_ptr == ' ') {
                    space_sign = true;
                    fmt_ptr++;
                } else if (*fmt_ptr == '#') {
                    alternate_form = true;
                    fmt_ptr++;
                } else if (*fmt_ptr == '0') {
                    pad_char = '0';
                    fmt_ptr++;
                } else {
                    break;
                }
            }
            
            // Parse width
            while (*fmt_ptr >= '0' && *fmt_ptr <= '9') {
                width = width * 10 + (*fmt_ptr - '0');
                fmt_ptr++;
            }
            
            // Handle format specifiers
            switch (*fmt_ptr) {
                case 'd':
                case 'i': {
                    int value = va_arg(args, int);
                    char num_buf[32];
                    int_to_string_base(value, num_buf, 10, width, pad_char, false);
                    char* num_ptr = num_buf;
                    while (*num_ptr && (buf_ptr - buffer) < 1023) {
                        *buf_ptr++ = *num_ptr++;
                    }
                    break;
                }
                case 'u': {
                    unsigned int value = va_arg(args, unsigned int);
                    char num_buf[32];
                    int_to_string_base(value, num_buf, 10, width, pad_char, false);
                    char* num_ptr = num_buf;
                    while (*num_ptr && (buf_ptr - buffer) < 1023) {
                        *buf_ptr++ = *num_ptr++;
                    }
                    break;
                }
                case 'x': {
                    unsigned int value = va_arg(args, unsigned int);
                    if (alternate_form && value != 0) {
                        *buf_ptr++ = '0';
                        *buf_ptr++ = 'x';
                    }
                    char num_buf[32];
                    int_to_string_base(value, num_buf, 16, width, pad_char, false);
                    char* num_ptr = num_buf;
                    while (*num_ptr && (buf_ptr - buffer) < 1023) {
                        *buf_ptr++ = *num_ptr++;
                    }
                    break;
                }
                case 'X': {
                    unsigned int value = va_arg(args, unsigned int);
                    if (alternate_form && value != 0) {
                        *buf_ptr++ = '0';
                        *buf_ptr++ = 'X';
                    }
                    char num_buf[32];
                    int_to_string_base(value, num_buf, 16, width, pad_char, true);
                    char* num_ptr = num_buf;
                    while (*num_ptr && (buf_ptr - buffer) < 1023) {
                        *buf_ptr++ = *num_ptr++;
                    }
                    break;
                }
                case 'o': {
                    unsigned int value = va_arg(args, unsigned int);
                    if (alternate_form && value != 0) {
                        *buf_ptr++ = '0';
                    }
                    char num_buf[32];
                    int_to_string_base(value, num_buf, 8, width, pad_char, false);
                    char* num_ptr = num_buf;
                    while (*num_ptr && (buf_ptr - buffer) < 1023) {
                        *buf_ptr++ = *num_ptr++;
                    }
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    *buf_ptr++ = c;
                    break;
                }
                case 's': {
                    const char* str = va_arg(args, const char*);
                    if (!str) str = "(null)";
                    while (*str && (buf_ptr - buffer) < 1023) {
                        *buf_ptr++ = *str++;
                    }
                    break;
                }
                case 'p': {
                    void* ptr = va_arg(args, void*);
                    *buf_ptr++ = '0';
                    *buf_ptr++ = 'x';
                    char num_buf[32];
                    int_to_string_base((unsigned long)ptr, num_buf, 16, 8, '0', false);
                    char* num_ptr = num_buf;
                    while (*num_ptr && (buf_ptr - buffer) < 1023) {
                        *buf_ptr++ = *num_ptr++;
                    }
                    break;
                }
                case '%': {
                    *buf_ptr++ = '%';
                    break;
                }
                default: {
                    *buf_ptr++ = '%';
                    *buf_ptr++ = *fmt_ptr;
                    break;
                }
            }
        } else {
            *buf_ptr++ = *fmt_ptr;
        }
        fmt_ptr++;
    }
    
    *buf_ptr = '\0';
    print(buffer);
    
    va_end(args);
    terminal_update_cursor();
}



// Helper function to convert unsigned integer to string
static int uitoa(unsigned int value, char* str, int base) {
    char* ptr = str;
    char* ptr1 = str;
    char tmp_char;
    unsigned int tmp_value;
    int len = 0;

    // Convert to string (reversed)
    do {
        tmp_value = value;
        value /= base;
        *ptr++ = "0123456789abcdef"[tmp_value - value * base];
        len++;
    } while (value);

    *ptr-- = '\0';

    // Reverse the string
    while (str < ptr) {
        tmp_char = *ptr;
        *ptr-- = *str;
        *str++ = tmp_char;
    }

    return len;
}

// Simple string length function
static int strlen_local(const char* str) {
    int len = 0;
    while (str[len])
        len++;
    return len;
}

// Main sprintf implementation
int sprintf(char* buf, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    char* str = buf;
    const char* ptr;
    
    for (ptr = fmt; *ptr != '\0'; ptr++) {
        if (*ptr != '%') {
            *str++ = *ptr;
            continue;
        }
        
        ptr++; // Move past '%'
        
        // Handle format specifiers
        switch (*ptr) {
            case 'd': // Signed decimal integer
            case 'i': {
                int val = va_arg(args, int);
                char num_buf[32];
                itoa(val, num_buf, 10);
                int len = strlen_local(num_buf);
                for (int i = 0; i < len; i++) {
                    *str++ = num_buf[i];
                }
                break;
            }
            
            case 'u': { // Unsigned decimal integer
                unsigned int val = va_arg(args, unsigned int);
                char num_buf[32];
                uitoa(val, num_buf, 10);
                int len = strlen_local(num_buf);
                for (int i = 0; i < len; i++) {
                    *str++ = num_buf[i];
                }
                break;
            }
            
            case 'x': { // Unsigned hexadecimal (lowercase)
                unsigned int val = va_arg(args, unsigned int);
                char num_buf[32];
                uitoa(val, num_buf, 16);
                int len = strlen_local(num_buf);
                for (int i = 0; i < len; i++) {
                    *str++ = num_buf[i];
                }
                break;
            }
            
            case 'X': { // Unsigned hexadecimal (uppercase)
                unsigned int val = va_arg(args, unsigned int);
                char num_buf[32];
                uitoa(val, num_buf, 16);
                int len = strlen_local(num_buf);
                for (int i = 0; i < len; i++) {
                    char c = num_buf[i];
                    if (c >= 'a' && c <= 'f')
                        c = c - 'a' + 'A';
                    *str++ = c;
                }
                break;
            }
            
            case 's': { // String
                char* s = va_arg(args, char*);
                if (s == NULL)
                    s = "(null)";
                while (*s) {
                    *str++ = *s++;
                }
                break;
            }
            
            case 'c': { // Character
                char c = (char)va_arg(args, int);
                *str++ = c;
                break;
            }
            
            case '%': { // Literal '%'
                *str++ = '%';
                break;
            }
            
            default: { // Unknown format specifier
                *str++ = '%';
                *str++ = *ptr;
                break;
            }
        }
    }
    
    *str = '\0'; // Null terminate
    va_end(args);
    
    return str - buf; // Return number of characters written
}



void print_integer(int value) {
    char buffer[12];
    int index = 0;
    
    if (value < 0) {
        buffer[index++] = '-';
        value = -value;
    }
    
    if (value == 0) {
        buffer[index++] = '0';
    } else {
        char temp[12];
        int temp_index = 0;
        while (value > 0) {
            temp[temp_index++] = (value % 10) + '0';
            value /= 10;
        }
        // Reverse
        for (int i = temp_index - 1; i >= 0; i--) {
            buffer[index++] = temp[i];
        }
    }
    
    buffer[index] = '\0';
    print(buffer);
    terminal_update_cursor();
}

void print_decimal(int num) 
{
    print_integer(num); // Use the fixed print_integer function
    terminal_update_cursor();
}

void print_hex(int num) 
{
    char buffer[9];
    int i = 0;

    if (num == 0) {
        print("0");
        return;
    }

    while (num > 0) {
        int digit = num % 16;
        buffer[i++] = (digit < 10) ? (digit + '0') : (digit - 10 + 'A');
        num /= 16;
    }
    
    buffer[i] = '\0';
    for (int j = 0; j < i / 2; j++) {
        char temp = buffer[j];
        buffer[j] = buffer[i - j - 1];
        buffer[i - j - 1] = temp;
    }

    print(buffer);
    terminal_update_cursor();
}

void print_octal(int num) 
{
    char buffer[12];
    int i = 0;

    if (num == 0) {
        print("0");
        return;
    }

    while (num > 0) {
        buffer[i++] = (num % 8) + '0';
        num /= 8;
    }
    
    buffer[i] = '\0';
    for (int j = 0; j < i / 2; j++) {
        char temp = buffer[j];
        buffer[j] = buffer[i - j - 1];
        buffer[i - j - 1] = temp;
    }

    print(buffer);
    terminal_update_cursor();
}

void print_slow(const char* data, uint32_t delay_time) {
    for (size_t i = 0; data[i] != '\0'; i++) {
        terminal_putchar(data[i]);
        delay(delay_time);
    }
    terminal_update_cursor();
}

void print_hex_byte(uint8_t value) {
    const char hex_chars[] = "0123456789ABCDEF";
    terminal_putchar(hex_chars[(value >> 4) & 0xF]);
    terminal_putchar(hex_chars[value & 0xF]);
    terminal_update_cursor();
}

void print_uint64(uint64_t value) {
    char buffer[21];
    int i = 0;
    
    if (value == 0) {
        print("0");
        return;
    }
    
    while (value > 0) {
        buffer[i++] = '0' + (value % 10);
        value /= 10;
    }
    
    // Reverse the buffer
    for (int j = 0; j < i / 2; j++) {
        char temp = buffer[j];
        buffer[j] = buffer[i - j - 1];
        buffer[i - j - 1] = temp;
    }
    buffer[i] = '\0';
    
    print(buffer);
    terminal_update_cursor();
}

void print_uint(unsigned int value) {
    char buffer[11]; // Maximum digits for unsigned int (32-bit) is 10 + null terminator
    int i = 0;
    
    if (value == 0) {
        print("0");
        return;
    }
    
    while (value > 0) {
        buffer[i++] = '0' + (value % 10);
        value /= 10;
    }
    
    // Reverse the buffer
    for (int j = 0; j < i / 2; j++) {
        char temp = buffer[j];
        buffer[j] = buffer[i - j - 1];
        buffer[i - j - 1] = temp;
    }
    buffer[i] = '\0';
    
    print(buffer);
    terminal_update_cursor();
}

void print_capacity(uint64_t bytes) {
    if (bytes < 1024) {
        print_uint64(bytes);
        print("B");
        terminal_update_cursor();
    } else if (bytes < 1024 * 1024) {
        print_uint64(bytes / 1024);
        print("KB");
        terminal_update_cursor();
    } else if (bytes < 1024ULL * 1024 * 1024) {
        print_uint64(bytes / (1024 * 1024));
        print("MB");
        terminal_update_cursor();
    } else if (bytes < 1024ULL * 1024 * 1024 * 1024) {
        print_uint64(bytes / (1024ULL * 1024 * 1024));
        print("GB");
        terminal_update_cursor();
    } else {
        print_uint64(bytes / (1024ULL * 1024 * 1024 * 1024));
        print("TB");
        terminal_update_cursor();
    }
}

void print_char(char c) {
    terminal_putchar(c);
    terminal_update_cursor();
}

void print_qemu(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    
    // Simple format string parser
    char* buf_ptr = buffer;
    const char* fmt_ptr = format;
    
    while (*fmt_ptr) {
        if (*fmt_ptr == '%') {
            fmt_ptr++;
            switch (*fmt_ptr) {
                case 'd': {
                    int value = va_arg(args, int);
                    char num_buf[32];
                    itoa(value, num_buf, 10);
                    strcpy(buf_ptr, num_buf);
                    buf_ptr += strlen(num_buf);
                    break;
                }
                case 'x': {
                    unsigned int value = va_arg(args, unsigned int);
                    char num_buf[32];
                    itoa(value, num_buf, 16);
                    strcpy(buf_ptr, num_buf);
                    buf_ptr += strlen(num_buf);
                    break;
                }
                case 's': {
                    char* str = va_arg(args, char*);
                    strcpy(buf_ptr, str);
                    buf_ptr += strlen(str);
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    *buf_ptr++ = c;
                    break;
                }
                case '%': {
                    *buf_ptr++ = '%';
                    break;
                }
                default:
                    *buf_ptr++ = '%';
                    *buf_ptr++ = *fmt_ptr;
                    break;
            }
            fmt_ptr++;
        } else {
            *buf_ptr++ = *fmt_ptr++;
        }
    }
    *buf_ptr = '\0';
    
    va_end(args);
    
    // Output to QEMU debug port (0xE9)
    for (char* p = buffer; *p; p++) {
        outb(0xE9, *p);
    }
}

void terminal_clear_inFunction(void) 
{
    for (size_t y = 0; y < terminal_height; y++) {
        for (size_t x = 0; x < terminal_width; x++) {
            terminal_putentryat(' ', terminal_color, x, y);
        }
    }
    terminal_row = 0;
    terminal_column = 0;
}

void terminal_clear(void) 
{
    for (size_t y = 0; y < terminal_height; y++) {
        for (size_t x = 0; x < terminal_width; x++) {
            terminal_putentryat(' ', terminal_color, x, y);
        }
    }
    terminal_row = 0;
    terminal_column = 0;
    
}
int snprintf(char* buffer, size_t size, const char* format, ...) {
    if (!buffer || size == 0) return 0;
    
    va_list args;
    va_start(args, format);
    
    char* buf_ptr = buffer;
    const char* fmt_ptr = format;
    size_t remaining = size - 1; // Leave space for null terminator
    
    while (*fmt_ptr && remaining > 0) {
        if (*fmt_ptr == '%' && *(fmt_ptr + 1)) {
            fmt_ptr++; // Skip %
            
            // Parse flags and width (simplified version)
            int width = 0;
            char pad_char = ' ';
            bool left_align = false;
            bool force_sign = false;
            bool space_sign = false;
            bool alternate_form = false;
            
            // Parse flags
            while (1) {
                if (*fmt_ptr == '-') {
                    left_align = true;
                    fmt_ptr++;
                } else if (*fmt_ptr == '+') {
                    force_sign = true;
                    fmt_ptr++;
                } else if (*fmt_ptr == ' ') {
                    space_sign = true;
                    fmt_ptr++;
                } else if (*fmt_ptr == '#') {
                    alternate_form = true;
                    fmt_ptr++;
                } else if (*fmt_ptr == '0') {
                    pad_char = '0';
                    fmt_ptr++;
                } else {
                    break;
                }
            }
            
            // Parse width
            while (*fmt_ptr >= '0' && *fmt_ptr <= '9') {
                width = width * 10 + (*fmt_ptr - '0');
                fmt_ptr++;
            }
            
            // Handle format specifiers
            switch (*fmt_ptr) {
                case 'd':
                case 'i': {
                    int value = va_arg(args, int);
                    char num_buf[32];
                    int_to_string_base(value, num_buf, 10, width, pad_char, false);
                    char* num_ptr = num_buf;
                    while (*num_ptr && remaining > 0) {
                        *buf_ptr++ = *num_ptr++;
                        remaining--;
                    }
                    break;
                }
                case 'u': {
                    unsigned int value = va_arg(args, unsigned int);
                    char num_buf[32];
                    int_to_string_base(value, num_buf, 10, width, pad_char, false);
                    char* num_ptr = num_buf;
                    while (*num_ptr && remaining > 0) {
                        *buf_ptr++ = *num_ptr++;
                        remaining--;
                    }
                    break;
                }
                case 'x': {
                    unsigned int value = va_arg(args, unsigned int);
                    if (alternate_form && value != 0 && remaining >= 2) {
                        *buf_ptr++ = '0';
                        *buf_ptr++ = 'x';
                        remaining -= 2;
                    }
                    char num_buf[32];
                    int_to_string_base(value, num_buf, 16, width, pad_char, false);
                    char* num_ptr = num_buf;
                    while (*num_ptr && remaining > 0) {
                        *buf_ptr++ = *num_ptr++;
                        remaining--;
                    }
                    break;
                }
                case 'X': {
                    unsigned int value = va_arg(args, unsigned int);
                    if (alternate_form && value != 0 && remaining >= 2) {
                        *buf_ptr++ = '0';
                        *buf_ptr++ = 'X';
                        remaining -= 2;
                    }
                    char num_buf[32];
                    int_to_string_base(value, num_buf, 16, width, pad_char, true);
                    char* num_ptr = num_buf;
                    while (*num_ptr && remaining > 0) {
                        *buf_ptr++ = *num_ptr++;
                        remaining--;
                    }
                    break;
                }
                case 'o': {
                    unsigned int value = va_arg(args, unsigned int);
                    if (alternate_form && value != 0 && remaining > 0) {
                        *buf_ptr++ = '0';
                        remaining--;
                    }
                    char num_buf[32];
                    int_to_string_base(value, num_buf, 8, width, pad_char, false);
                    char* num_ptr = num_buf;
                    while (*num_ptr && remaining > 0) {
                        *buf_ptr++ = *num_ptr++;
                        remaining--;
                    }
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    if (remaining > 0) {
                        *buf_ptr++ = c;
                        remaining--;
                    }
                    break;
                }
                case 's': {
                    const char* str = va_arg(args, const char*);
                    if (!str) str = "(null)";
                    while (*str && remaining > 0) {
                        *buf_ptr++ = *str++;
                        remaining--;
                    }
                    break;
                }
                case 'p': {
                    void* ptr = va_arg(args, void*);
                    if (remaining >= 2) {
                        *buf_ptr++ = '0';
                        *buf_ptr++ = 'x';
                        remaining -= 2;
                    }
                    char num_buf[32];
                    int_to_string_base((unsigned long)ptr, num_buf, 16, 8, '0', false);
                    char* num_ptr = num_buf;
                    while (*num_ptr && remaining > 0) {
                        *buf_ptr++ = *num_ptr++;
                        remaining--;
                    }
                    break;
                }
                case '%': {
                    if (remaining > 0) {
                        *buf_ptr++ = '%';
                        remaining--;
                    }
                    break;
                }
                default: {
                    if (remaining > 1) {
                        *buf_ptr++ = '%';
                        *buf_ptr++ = *fmt_ptr;
                        remaining -= 2;
                    }
                    break;
                }
            }
        } else {
            *buf_ptr++ = *fmt_ptr;
            remaining--;
        }
        fmt_ptr++;
    }
    
    *buf_ptr = '\0';
    va_end(args);
    
    // Return the number of characters that would have been written
    // if buffer was large enough (not including null terminator)
    return (int)(buf_ptr - buffer);
}

