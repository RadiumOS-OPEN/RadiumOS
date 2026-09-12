#include "../vga/vga_gfx.h"
#include "../terminal/terminal.h"

extern void watchdog_hud_force_redraw();

// Switches to 320x200x256, draws a test pattern, waits for a keypress,
// then restores 80x25 text mode. Confirms the mode-13h driver works
// before anything tries to render real frames into it.
//
// vga_gfx_set_text_mode() already restores the exact prior screen
// content (font, on-screen characters/colors, and DAC palette) as
// part of leaving mode 13h, so there's deliberately no terminal_clear()
// here — calling it would re-blank the whole 80-column screen
// (terminal_clear() covers the full width, HUD included) right after
// it was correctly restored. watchdog_hud_force_redraw() is still
// worth calling so any periodic/ticking HUD fields look fresh rather
// than showing the instant they were last drawn before mode13h.
void gfxtest(int argc, char* argv[]) {
    vga_gfx_test_pattern();
    watchdog_hud_force_redraw();
}