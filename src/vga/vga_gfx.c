// vga_gfx.c - Linear framebuffer (VGA Mode 13h) graphics driver
//
// See vga_gfx.h for background. All register tables below are the
// standard, widely-documented VGA hardware initialization values for
// mode 0x13 (320x200x256) and mode 0x03 (80x25 text) — the same
// hardware register layout described in the VGA/CRTC/Sequencer/Graphics
// Controller/Attribute Controller documentation (freevga "VGA Hardware"
// reference). Programming the controller directly like this is the
// standard way to set a video mode from protected-mode kernel code that
// has no access to real-mode BIOS interrupts.

#include "vga_gfx.h"
#include "../io/io.h"
#include "../timers/timer.h"
#include "../keyboard/keyboard.h"

#define VGA_MISC_WRITE   0x3C2
#define VGA_MISC_READ    0x3CC
#define VGA_SEQ_INDEX    0x3C4
#define VGA_SEQ_DATA     0x3C5
#define VGA_CRTC_INDEX   0x3D4
#define VGA_CRTC_DATA    0x3D5
#define VGA_GC_INDEX     0x3CE
#define VGA_GC_DATA      0x3CF
#define VGA_AC_INDEX     0x3C0
#define VGA_AC_WRITE     0x3C0
#define VGA_AC_READ      0x3C1
#define VGA_INSTAT_READ  0x3DA
#define VGA_DAC_WRITE_IDX 0x3C8
#define VGA_DAC_READ_IDX   0x3C7
#define VGA_DAC_DATA      0x3C9

// ----- Mode 0x13: 320x200x256 linear framebuffer -----
static const uint8_t g_mode13h[] = {
    // MISC
    0x63,
    // SEQ (5 regs)
    0x03, 0x01, 0x0F, 0x00, 0x0E,
    // CRTC (25 regs)
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
    0xFF,
    // GC (9 regs)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,
    0xFF,
    // AC (21 regs)
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00
};

// ----- Mode 0x03: 80x25 16-color text mode -----
static const uint8_t g_mode03h[] = {
    // MISC
    0x67,
    // SEQ (5 regs)
    0x03, 0x00, 0x03, 0x00, 0x02,
    // CRTC (25 regs)
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x50,
    0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
    0xFF,
    // GC (9 regs)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00,
    0xFF,
    // AC (21 regs)
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x0C, 0x00, 0x0F, 0x08, 0x00
};

// Program the full register set for a given table (same layout as
// g_mode13h/g_mode03h above: 1 MISC + 5 SEQ + 25 CRTC + 9 GC + 21 AC).
static void vga_apply_register_table(const uint8_t* regs) {
    size_t i = 0;

    // Misc Output Register
    outb(VGA_MISC_WRITE, regs[i++]);

    // Sequencer registers
    for (uint8_t idx = 0; idx < 5; idx++) {
        outb(VGA_SEQ_INDEX, idx);
        outb(VGA_SEQ_DATA, regs[i++]);
    }

    // Unlock CRTC registers 0-7 (clear the protect bit in index 0x11)
    outb(VGA_CRTC_INDEX, 0x11);
    outb(VGA_CRTC_DATA, inb(VGA_CRTC_DATA) & 0x7F);

    // CRTC registers
    for (uint8_t idx = 0; idx < 25; idx++) {
        outb(VGA_CRTC_INDEX, idx);
        outb(VGA_CRTC_DATA, regs[i++]);
    }

    // Graphics Controller registers
    for (uint8_t idx = 0; idx < 9; idx++) {
        outb(VGA_GC_INDEX, idx);
        outb(VGA_GC_DATA, regs[i++]);
    }

    // Attribute Controller registers.
    // Reading the input status register resets the address/data
    // flip-flop so the next write to 0x3C0 is treated as an index.
    (void)inb(VGA_INSTAT_READ);
    for (uint8_t idx = 0; idx < 21; idx++) {
        outb(VGA_AC_INDEX, idx);
        outb(VGA_AC_WRITE, regs[i++]);
    }

    // Re-enable video output (index bit 5 set) and leave the flip-flop
    // in a clean state.
    (void)inb(VGA_INSTAT_READ);
    outb(VGA_AC_INDEX, 0x20);
}

// ----- Live register capture (for restoring the ORIGINAL text mode) -----
//
// g_mode03h above is a generic, standard 80x25 text-mode register set —
// valid on its own, but not guaranteed to be bit-for-bit identical to
// whatever this specific BIOS/QEMU actually configured at boot (e.g.
// dot-clock/horizontal-timing choices can legally vary between BIOS
// implementations while both count as "text mode 3"). That mismatch is
// what made restored text look subtly wider/bolder than the original.
// The fix: capture the real, live registers before ever touching mode
// 13h, and restore those exact values instead of reapplying a
// hardcoded table. Same 1+5+25+9+21 byte layout as g_mode13h/g_mode03h,
// so it can be fed straight into vga_apply_register_table().
#define VGA_REG_TABLE_SIZE (1 + 5 + 25 + 9 + 21)
static uint8_t s_saved_text_mode_regs[VGA_REG_TABLE_SIZE];
static bool s_regs_saved = false;

static void vga_capture_register_table(uint8_t* out) {
    size_t i = 0;

    out[i++] = inb(VGA_MISC_READ);

    for (uint8_t idx = 0; idx < 5; idx++) {
        outb(VGA_SEQ_INDEX, idx);
        out[i++] = inb(VGA_SEQ_DATA);
    }

    for (uint8_t idx = 0; idx < 25; idx++) {
        outb(VGA_CRTC_INDEX, idx);
        out[i++] = inb(VGA_CRTC_DATA);
    }

    for (uint8_t idx = 0; idx < 9; idx++) {
        outb(VGA_GC_INDEX, idx);
        out[i++] = inb(VGA_GC_DATA);
    }

    // AC index/data (0x3C0) is a flip-flop: alternating writes are
    // read as "set index" then "set data". vga_apply_register_table's
    // write loop stays synced for free because it does an index+data
    // WRITE pair every iteration (2 writes = flip-flop back where it
    // started). Reading is different: only the index is a write here
    // (the data read from 0x3C1 doesn't touch the flip-flop), so
    // without resetting it before every single iteration, every write
    // after the first gets misread as a data write instead of an
    // index — silently corrupting the live AC registers while also
    // capturing garbage back.
    for (uint8_t idx = 0; idx < 21; idx++) {
        (void)inb(VGA_INSTAT_READ); // reset flip-flop before EACH index write
        outb(VGA_AC_INDEX, idx);
        out[i++] = inb(VGA_AC_READ); // read data comes back on 0x3C1, not 0x3C0
    }
}

// Byte offsets into the captured table, used by the font-access dance
// below so it restores the REAL SEQ4/GC4/GC5/GC6 values instead of
// assuming g_mode03h's.
#define REG_GC4_OFFSET  (1 + 5 + 25 + 4)
#define REG_GC5_OFFSET  (1 + 5 + 25 + 5)
#define REG_GC6_OFFSET  (1 + 5 + 25 + 6)

// ----- DAC (color palette) preservation -----
//
// The Attribute Controller registers (already reprogrammed correctly
// by vga_apply_register_table) only map a 4-bit text attribute to a
// DAC palette index. The actual RGB values live in the DAC itself
// (ports 0x3C7/0x3C8/0x3C9), which is separate hardware state that
// mode-table application never touches. vga_gfx_test_pattern()
// overwrites all 256 DAC entries for its gradient demo; without this,
// text mode comes back with correct characters and correct index
// mapping, but the palette those indices point to is still the
// graphics-mode gradient — e.g. whatever index "cyan" maps to ends up
// some dark, mostly-invisible color instead of actual cyan.
static uint8_t s_saved_dac[256 * 3];
static bool s_dac_saved = false;

static void vga_dac_save(void) {
    for (int i = 0; i < 256; i++) {
        outb(VGA_DAC_READ_IDX, (uint8_t)i);
        s_saved_dac[i * 3 + 0] = inb(VGA_DAC_DATA);
        s_saved_dac[i * 3 + 1] = inb(VGA_DAC_DATA);
        s_saved_dac[i * 3 + 2] = inb(VGA_DAC_DATA);
    }
}

static void vga_dac_restore(void) {
    for (int i = 0; i < 256; i++) {
        vga_gfx_set_palette_color((uint8_t)i, s_saved_dac[i * 3 + 0],
                                   s_saved_dac[i * 3 + 1], s_saved_dac[i * 3 + 2]);
    }
}

// ----- On-screen text buffer preservation -----
//
// Even with the font (plane 2) and DAC restored, mode 13h's Chain4
// writes also clobber the actual on-screen character+attribute cells
// (planes 0/1, addressed as 0xB8000 in normal text mode). Content that
// gets redrawn every tick by the HUD recovers on its own once
// watchdog_hud_force_redraw() runs — but fields that only redraw on an
// actual state change (e.g. a network-status line that only updates
// on link events) never get touched again and stay corrupted forever.
// Saving/restoring the whole buffer here, at the driver level, doesn't
// depend on every caller's redraw logic covering every line.
#define VGA_TEXT_ADDR   0xB8000u
// RadiumOS boots in a custom 80x50 text mode (8x8 font loaded by
// terminal_initialize(), not the BIOS-default 8x16/80x25 mode 3), so
// the on-screen buffer is twice the size a "standard" text mode would
// suggest. Using 80x25 here silently only saved/restored the top half
// of the real screen, leaving the bottom half showing whatever mode
// 13h had left behind.
#define VGA_TEXT_CELLS  (80 * 50)

static uint16_t s_saved_text[VGA_TEXT_CELLS];
static bool s_text_saved = false;

static void vga_text_save(void) {
    volatile uint16_t* src = (volatile uint16_t*)VGA_TEXT_ADDR;
    for (int i = 0; i < VGA_TEXT_CELLS; i++) {
        s_saved_text[i] = src[i];
    }
}

static void vga_text_restore(void) {
    volatile uint16_t* dst = (volatile uint16_t*)VGA_TEXT_ADDR;
    for (int i = 0; i < VGA_TEXT_CELLS; i++) {
        dst[i] = s_saved_text[i];
    }
}

// ----- Text-mode font preservation -----
//
// Mode 13h and 80x25 text mode share the same physical VRAM, just
// addressed differently. Text mode's character glyphs live in plane 2,
// read/written through a special "font access" addressing mode. Mode
// 13h's linear (Chain4) addressing spreads every byte we write across
// all 4 planes round-robin, which clobbers plane 2's font data as a
// side effect. Every "set mode 13h without BIOS" implementation needs
// to save the font before entering graphics mode and restore it before
// returning to text mode, or text mode comes back displaying garbage
// glyphs instead of characters. 8KB covers the standard 256-glyph
// layout at 32 bytes/slot (real glyphs are 16 bytes each; the extra
// space is the conventional slot padding).
#define VGA_FONT_SAVE_SIZE 8192
static uint8_t s_saved_font[VGA_FONT_SAVE_SIZE];
static bool s_font_saved = false;

// Reconfigure the Sequencer/Graphics Controller so 0xA0000 addresses
// plane 2 linearly (the character generator), for save/restore only.
// Does not touch CRTC/AC/Misc, so it doesn't change what's displayed.
static void vga_font_access_enter(void) {
    outb(VGA_SEQ_INDEX, 0x00); outb(VGA_SEQ_DATA, 0x01); // stop sequencer
    outb(VGA_SEQ_INDEX, 0x02); outb(VGA_SEQ_DATA, 0x04); // map mask: plane 2 only
    outb(VGA_SEQ_INDEX, 0x04); outb(VGA_SEQ_DATA, 0x07); // sequential addressing
    outb(VGA_SEQ_INDEX, 0x00); outb(VGA_SEQ_DATA, 0x03); // restart sequencer

    outb(VGA_GC_INDEX, 0x04); outb(VGA_GC_DATA, 0x02); // read map select: plane 2
    outb(VGA_GC_INDEX, 0x05); outb(VGA_GC_DATA, 0x00); // mode 0
    outb(VGA_GC_INDEX, 0x06); outb(VGA_GC_DATA, 0x04); // graphics mode, A0000-AFFFF 64K (linear plane access, no odd/even split)
}

// Restore the Sequencer/Graphics Controller registers to whatever the
// real captured text mode needs (SEQ4/GC4/GC5/GC6), after
// font_access_enter() left them in the plane-2-only state. Falls back
// to standard text-mode values if we somehow haven't captured yet.
static void vga_font_access_exit_to_text(void) {
    if (s_regs_saved) {
        outb(VGA_SEQ_INDEX, 0x02); outb(VGA_SEQ_DATA, 0x03); // map mask: planes 0+1
        outb(VGA_SEQ_INDEX, 0x04); outb(VGA_SEQ_DATA, s_saved_text_mode_regs[5]); // real SEQ4

        outb(VGA_GC_INDEX, 0x04); outb(VGA_GC_DATA, s_saved_text_mode_regs[REG_GC4_OFFSET]);
        outb(VGA_GC_INDEX, 0x05); outb(VGA_GC_DATA, s_saved_text_mode_regs[REG_GC5_OFFSET]);
        outb(VGA_GC_INDEX, 0x06); outb(VGA_GC_DATA, s_saved_text_mode_regs[REG_GC6_OFFSET]);
    } else {
        outb(VGA_SEQ_INDEX, 0x02); outb(VGA_SEQ_DATA, 0x03);
        outb(VGA_SEQ_INDEX, 0x04); outb(VGA_SEQ_DATA, 0x03);

        outb(VGA_GC_INDEX, 0x04); outb(VGA_GC_DATA, 0x00);
        outb(VGA_GC_INDEX, 0x05); outb(VGA_GC_DATA, 0x10);
        outb(VGA_GC_INDEX, 0x06); outb(VGA_GC_DATA, 0x0E);
    }
}

void vga_gfx_set_mode13h(void) {
    if (!s_regs_saved) {
        vga_capture_register_table(s_saved_text_mode_regs);
        s_regs_saved = true;
    }

    if (!s_text_saved) {
        vga_text_save();
        s_text_saved = true;
    }

    if (!s_dac_saved) {
        vga_dac_save();
        s_dac_saved = true;
    }

    // Snapshot the font out of plane 2 before Chain4 writes can
    // clobber it. Only need to do this once — the saved copy stays
    // valid across repeated mode13h<->text switches.
    if (!s_font_saved) {
        vga_font_access_enter();
        uint8_t* fb = vga_gfx_get_framebuffer();
        for (uint32_t i = 0; i < VGA_FONT_SAVE_SIZE; i++) {
            s_saved_font[i] = fb[i];
        }
        s_font_saved = true;
    }
    vga_apply_register_table(g_mode13h);
}

void vga_gfx_set_text_mode(void) {
    // Restore the exact registers this machine's BIOS actually had
    // configured before we ever touched mode 13h, rather than a
    // generic hand-typed table — guarantees identical dot clock and
    // horizontal timing, so text comes back the same width/weight it
    // started at. Falls back to the generic table only if we
    // somehow reach this without ever having entered mode13h first.
    if (s_regs_saved) {
        vga_apply_register_table(s_saved_text_mode_regs);
    } else {
        vga_apply_register_table(g_mode03h);
    }

    if (s_font_saved) {
        vga_font_access_enter();
        uint8_t* fb = vga_gfx_get_framebuffer();
        for (uint32_t i = 0; i < VGA_FONT_SAVE_SIZE; i++) {
            fb[i] = s_saved_font[i];
        }
        vga_font_access_exit_to_text();
    }

    if (s_text_saved) {
        vga_text_restore();
    }

    if (s_dac_saved) {
        vga_dac_restore();
    }
}

uint8_t* vga_gfx_get_framebuffer(void) {
    return (uint8_t*)VGA_GFX_FB_ADDR;
}

void vga_gfx_putpixel(int x, int y, uint8_t color_index) {
    if (x < 0 || y < 0 || x >= VGA_GFX_WIDTH || y >= VGA_GFX_HEIGHT) {
        return;
    }
    uint8_t* fb = vga_gfx_get_framebuffer();
    fb[(uint32_t)y * VGA_GFX_WIDTH + (uint32_t)x] = color_index;
}

void vga_gfx_clear(uint8_t color_index) {
    uint8_t* fb = vga_gfx_get_framebuffer();
    for (uint32_t i = 0; i < VGA_GFX_SIZE; i++) {
        fb[i] = color_index;
    }
}

void vga_gfx_present(const uint8_t* buffer) {
    uint8_t* fb = vga_gfx_get_framebuffer();
    for (uint32_t i = 0; i < VGA_GFX_SIZE; i++) {
        fb[i] = buffer[i];
    }
}

void vga_gfx_set_palette_color(uint8_t index, uint8_t r6, uint8_t g6, uint8_t b6) {
    outb(VGA_DAC_WRITE_IDX, index);
    outb(VGA_DAC_DATA, r6 & 0x3F);
    outb(VGA_DAC_DATA, g6 & 0x3F);
    outb(VGA_DAC_DATA, b6 & 0x3F);
}

void vga_gfx_set_palette_color_rgb888(uint8_t index, uint8_t r8, uint8_t g8, uint8_t b8) {
    // Scale 8-bit (0-255) down to the VGA DAC's native 6-bit (0-63) range.
    vga_gfx_set_palette_color(index, r8 >> 2, g8 >> 2, b8 >> 2);
}

void vga_gfx_load_palette_rgb888(const uint8_t* rgb_768) {
    for (int i = 0; i < 256; i++) {
        uint8_t r = rgb_768[i * 3 + 0];
        uint8_t g = rgb_768[i * 3 + 1];
        uint8_t b = rgb_768[i * 3 + 2];
        vga_gfx_set_palette_color_rgb888((uint8_t)i, r, g, b);
    }
}

void vga_gfx_test_pattern(void) {
    vga_gfx_set_mode13h();

    // Identity-ish palette so color index roughly tracks intensity for
    // the first 64 entries, then a simple ramp for the rest.
    for (int i = 0; i < 256; i++) {
        vga_gfx_set_palette_color((uint8_t)i, (uint8_t)(i & 0x3F),
                                   (uint8_t)((i * 2) & 0x3F),
                                   (uint8_t)((i * 3) & 0x3F));
    }

    // Vertical color bars, plus a diagonal line, so both putpixel and
    // full-buffer addressing can be eyeballed.
    for (int y = 0; y < VGA_GFX_HEIGHT; y++) {
        for (int x = 0; x < VGA_GFX_WIDTH; x++) {
            uint8_t bar_color = (uint8_t)((x * 256) / VGA_GFX_WIDTH);
            vga_gfx_putpixel(x, y, bar_color);
        }
    }
    for (int d = 0; d < VGA_GFX_HEIGHT; d++) {
        vga_gfx_putpixel(d, d, 255);
    }

    keyboard_wait_for_key(false);
    vga_gfx_set_text_mode();
}