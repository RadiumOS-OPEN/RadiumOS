// bga.c - Bochs/QEMU DISPI linear framebuffer modes. See bga.h.

#include "bga.h"
#include "../io/io.h"

#define DISPI_INDEX_PORT 0x01CE
#define DISPI_DATA_PORT  0x01CF

#define DISPI_ID          0
#define DISPI_XRES        1
#define DISPI_YRES        2
#define DISPI_BPP         3
#define DISPI_ENABLE      4
#define DISPI_BANK        5
#define DISPI_VIRT_WIDTH  6
#define DISPI_VIRT_HEIGHT 7
#define DISPI_X_OFFSET    8
#define DISPI_Y_OFFSET    9

#define DISPI_DISABLED    0x00
#define DISPI_ENABLED     0x01
#define DISPI_LFB_ENABLED 0x40

#define BGA_LFB_FALLBACK  0xFD000000u

static void dispi_write(uint16_t index, uint16_t value) {
    outw(DISPI_INDEX_PORT, index);
    outw(DISPI_DATA_PORT, value);
}

static uint16_t dispi_read(uint16_t index) {
    outw(DISPI_INDEX_PORT, index);
    return inw(DISPI_DATA_PORT);
}

int bga_present(void) {
    uint16_t id = dispi_read(DISPI_ID);
    return id >= 0xB0C0 && id <= 0xB0C5;
}

/* PCI devices known to put the DISPI framebuffer at BAR0: QEMU/Bochs std VGA
   and VirtualBox's VBoxVGA. */
static int dispi_device(uint16_t vendor, uint16_t device) {
    return (vendor == 0x1234 && device == 0x1111) || (vendor == 0x80EE && device == 0xBEEF);
}

uint32_t bga_lfb_address(void) {
    for (int bus = 0; bus < 8; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read_config_dword((uint8_t)bus, (uint8_t)dev, 0, 0x00);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            if (!dispi_device((uint16_t)(id & 0xFFFF), (uint16_t)(id >> 16))) continue;
            uint32_t bar = pci_read_config_dword((uint8_t)bus, (uint8_t)dev, 0, 0x10);
            /* must be a 32-bit memory BAR that firmware assigned */
            if ((bar & 1) == 0 && (bar & 0xFFFFFFF0u) != 0) return bar & 0xFFFFFFF0u;
        }
    }
    return BGA_LFB_FALLBACK;
}

void bga_disable(void) {
    dispi_write(DISPI_ENABLE, DISPI_DISABLED);
}

int bga_set_mode(int width, int height, int bpp, uint8_t **lfb, int *pitch) {
    if (!bga_present()) return -1;
    if (width <= 0 || height <= 0 || width > 1920 || height > 1200) return -1;
    if (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) return -1;
    uint32_t base = bga_lfb_address();

    dispi_write(DISPI_ENABLE, DISPI_DISABLED);
    dispi_write(DISPI_XRES, (uint16_t)width);
    dispi_write(DISPI_YRES, (uint16_t)height);
    dispi_write(DISPI_BPP, (uint16_t)bpp);
    dispi_write(DISPI_VIRT_WIDTH, (uint16_t)width);
    dispi_write(DISPI_X_OFFSET, 0);
    dispi_write(DISPI_Y_OFFSET, 0);
    dispi_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED);

    /* The adapter clamps or refuses what it cannot do (too little VRAM, an
       unsupported depth): read the mode back rather than trusting it. */
    if (dispi_read(DISPI_XRES) != width || dispi_read(DISPI_YRES) != height ||
        dispi_read(DISPI_BPP) != bpp || (dispi_read(DISPI_ENABLE) & DISPI_ENABLED) == 0) {
        bga_disable();
        return -1;
    }
    int vw = dispi_read(DISPI_VIRT_WIDTH);
    if (vw < width) vw = width;
    int bytes = (bpp + 7) / 8;

    /* The LFB must answer like memory. Paging is off, so a wrong guess reads
       back open bus (0xFFFFFFFF) instead of faulting. */
    volatile uint32_t *probe = (volatile uint32_t *)(uintptr_t)base;
    uint32_t keep = probe[0];
    probe[0] = 0x5AA5C33Cu;
    int ok = probe[0] == 0x5AA5C33Cu;
    probe[0] = 0xA55A3CC3u;
    ok = ok && probe[0] == 0xA55A3CC3u;
    probe[0] = keep;
    if (!ok) {
        bga_disable();
        return -1;
    }
    *lfb = (uint8_t *)(uintptr_t)base;
    *pitch = vw * bytes;
    return 0;
}
