// vga_gfx.h - Linear framebuffer (VGA Mode 13h) graphics driver
//
// Fills the gap in the existing vga.c/vga.h toolkit, which is entirely
// text-mode (80x25 character cells). This driver switches the hardware
// into 320x200x256 linear-framebuffer mode by programming the VGA
// registers directly (Misc Output, Sequencer, CRTC, Graphics Controller,
// Attribute Controller) — no BIOS int 0x10 calls, which is required
// because the kernel is entered directly in 32-bit protected mode via
// Multiboot and real-mode BIOS services are not available.
//
// This is the piece a generic DOOM-style port needs for pixel output:
// a linear byte-per-pixel buffer at a fixed physical address (0xA0000)
// plus a way to program the 256-color DAC palette (Doom loads its
// palette from the WAD's PLAYPAL lump and expects to push it to hardware
// exactly like this).

#ifndef VGA_GFX_H
#define VGA_GFX_H

#include <stdint.h>
#include <stddef.h>

#define VGA_GFX_WIDTH   320
#define VGA_GFX_HEIGHT  200
#define VGA_GFX_SIZE    (VGA_GFX_WIDTH * VGA_GFX_HEIGHT) // 64000 bytes
#define VGA_GFX_FB_ADDR 0xA0000u

// ===== MODE SWITCHING =====
// Switch hardware into 320x200x256 linear framebuffer mode.
void vga_gfx_set_mode13h(void);

// Restore standard 80x25 16-color text mode (mode 0x03).
void vga_gfx_set_text_mode(void);

// ===== FRAMEBUFFER ACCESS =====
// Direct pointer to the 64000-byte linear framebuffer at 0xA0000.
// Already mapped: the kernel's boot page tables identity-map the first
// 4MB of physical memory, which covers 0xA0000-0xAFFFF.
uint8_t* vga_gfx_get_framebuffer(void);

// Plot a single indexed-color pixel. Bounds-checked (out-of-range x/y
// is a no-op).
void vga_gfx_putpixel(int x, int y, uint8_t color_index);

// Fill the entire framebuffer with one color index.
void vga_gfx_clear(uint8_t color_index);

// Copy a full off-screen 320x200 buffer to the hardware framebuffer in
// one shot (simple page flip / present for double-buffered rendering).
// `buffer` must point to at least VGA_GFX_SIZE bytes of palette-indexed
// pixels.
void vga_gfx_present(const uint8_t* buffer);

// ===== PALETTE / DAC =====
// Set one palette entry using native 6-bit VGA DAC values (0-63 per
// channel). This is the raw hardware format.
void vga_gfx_set_palette_color(uint8_t index, uint8_t r6, uint8_t g6, uint8_t b6);

// Convenience wrapper: set one palette entry from standard 8-bit (0-255)
// color values, e.g. straight out of a WAD's PLAYPAL lump.
void vga_gfx_set_palette_color_rgb888(uint8_t index, uint8_t r8, uint8_t g8, uint8_t b8);

// Load all 256 palette entries at once from a tightly packed array of
// 256 * 3 bytes (R,G,B,R,G,B,...) in 8-bit-per-channel format — exactly
// the layout of a Doom PLAYPAL lump entry.
void vga_gfx_load_palette_rgb888(const uint8_t* rgb_768);

// ===== DEMO / SELF-TEST =====
// Draws a simple test pattern, waits for a keypress, then restores text
// mode. Useful for confirming the mode switch works in QEMU/hardware
// before wiring up an actual renderer.
void vga_gfx_test_pattern(void);

#endif // VGA_GFX_H
