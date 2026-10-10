#include "../terminal/terminal.h"
#include "../keyboard/keyboard.h"
extern void watchdog_hud_force_redraw();

void clear(int argc, char* argv[]) {
    history_reset();
    terminal_clear();
    watchdog_hud_force_redraw();

}