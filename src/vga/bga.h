// bga.h - Bochs/QEMU "DISPI" display interface (Bochs Graphics Adapter)
//
// QEMU's -vga std (and Bochs, VirtualBox's VBoxVGA) expose a simple linear
// framebuffer mode switch through two I/O ports (index 0x1CE, data 0x1CF).
// The framebuffer itself is PCI BAR0 of the display device (1234:1111 on
// QEMU), normally 0xFD000000. The kernel's page tables identity-map only
// 0xFD000000 + 16 MiB of that area (boot.asm), so bga_set_mode refuses a
// framebuffer anywhere else.
//
// Text mode state lives in the same VRAM the LFB covers, so callers enter
// through vga_gfx_set_mode13h() first (it snapshots registers, font, text
// and DAC) and leave with bga_disable() followed by vga_gfx_set_text_mode().

#ifndef BGA_H
#define BGA_H

#include <stdint.h>

// 1 when the DISPI ID register answers with a known version.
int bga_present(void);

// Physical address of the linear framebuffer: BAR0 of the first PCI display
// device that speaks DISPI, or 0xFD000000 when none is found.
uint32_t bga_lfb_address(void);

// Switch to width x height x bpp with the linear framebuffer enabled.
// Returns 0 and fills *lfb and *pitch (bytes per row) on success; on any
// failure (no DISPI, mode rejected, LFB outside the mapped window or not
// answering) DISPI is left disabled and -1 is returned.
int bga_set_mode(int width, int height, int bpp, uint8_t **lfb, int *pitch);

// Turn DISPI off again (VGA registers then drive the display).
void bga_disable(void);

#endif
