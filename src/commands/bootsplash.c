#include "bootsplash.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../timers/timer.h"

// How long the splash stays on screen before boot continues (ms).
#define BOOTSPLASH_HOLD_MS 8500

extern size_t terminal_row;
extern size_t terminal_column;

// Centered radiation-trefoil logo. Each row is centered independently
// against VGA_WIDTH, so it doesn't need to be a perfect rectangle.
static const char *const RADIUM_LOGO[] = {
    "          \xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB         ",
    "       \xDB\xDB\xDB\xDB  \xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB  \xDB\xDB\xDB\xDB      ",
    "     \xDB\xDB\xDB    \xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB    \xDB\xDB\xDB    ",
    "    \xDB\xDB       \xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB       \xDB\xDB   ",
    "   \xDB\xDB         \xDB\xDB\xDB\xDB\xDB\xDB         \xDB\xDB  ",
    "  \xDB\xDB           \xDB\xDB\xDB\xDB           \xDB\xDB ",
    "  \xDB\xDB           \xDB\xDB\xDB\xDB           \xDB\xDB ",
    "  \xDB\xDB \xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB \xDB\xDB\xDB\xDB \xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB \xDB\xDB ",
    "  \xDB\xDB \xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB    \xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB \xDB\xDB ",
    "   \xDB\xDB \xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB      \xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB \xDB\xDB  ",
    "    \xDB\xDB \xDB\xDB\xDB\xDB\xDB          \xDB\xDB\xDB\xDB\xDB \xDB\xDB   ",
    "     \xDB\xDB\xDB \xDB\xDB            \xDB\xDB \xDB\xDB\xDB    ",
    "       \xDB\xDB\xDB\xDB            \xDB\xDB\xDB\xDB      ",
    "          \xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB         ",
};

#define RADIUM_LOGO_LINES (sizeof(RADIUM_LOGO) / sizeof(RADIUM_LOGO[0]))

#define BOOTSPLASH_PUBLISHER "(c) 2026 AT Products LLC"
#define BOOTSPLASH_COPYRIGHT "(c) 2026 scp_2801 / RadiumOS Project. All rights reserved."

// Print a single line horizontally centered on the current row.
static void print_centered(const char *line) {
    size_t len = strlen(line);
    size_t pad = (len < VGA_WIDTH) ? (VGA_WIDTH - len) / 2 : 0;
    for (size_t i = 0; i < pad; i++) {
        terminal_putchar(' ');
    }
    print(line);
    terminal_putchar('\n');
}

void show_boot_splash(void) {
    terminal_clear();

    // A little breathing room above the logo.
    print("\n\n");

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK));
    for (size_t i = 0; i < RADIUM_LOGO_LINES; i++) {
        print_centered(RADIUM_LOGO[i]);
    }

    print("\n");

    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    print_centered("RadiumOS");

    // Pin the copyright block to the bottom-left corner, regardless of
    // where the logo/wordmark left the cursor. Publisher line sits
    // directly above the project/author line.
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    terminal_row = VGA_HEIGHT - 2;
    terminal_column = 0;
    print(BOOTSPLASH_PUBLISHER);
    terminal_row = VGA_HEIGHT - 1;
    terminal_column = 0;
    print(BOOTSPLASH_COPYRIGHT);

    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));

    sleep_ms(BOOTSPLASH_HOLD_MS);
}

void bootsplash_command(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    show_boot_splash();
}