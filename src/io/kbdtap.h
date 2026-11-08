#ifndef KBDTAP_H
#define KBDTAP_H

#include <stdint.h>
#include <stdbool.h>

// While a command runs the timer tick moves keyboard controller bytes into a small FIFO so Ctrl+C can be seen even when the
// command never reads the keyboard. io.c serves reads of ports 0x60/0x64 from that FIFO first, so nothing is lost or reordered.

void    kbdtap_reset(bool ctrl_down);
void    kbdtap_drain(void);                 // call with interrupts off
bool    kbdtap_pop(uint8_t *byte);          // false when the FIFO is empty (read the hardware)
uint8_t kbdtap_fix_status(uint8_t status);  // port 0x64 as the reader should see it
bool    kbdtap_take_hit(void);              // Ctrl+C was seen since the last call
void    kbdtap_clear(void);

// Window manager mode (a program runs in a desktop window): mouse bytes go to their own FIFO for the desktop instead of
// reaching the program, and the window hotkeys are noticed (1 Alt+F4, 2 F11 or Alt+Enter, 3 Alt+F10).
void    kbdtap_wm(bool on);
bool    kbdtap_wm_active(void);
bool    kbdtap_mouse_pop(uint8_t *byte);
int     kbdtap_take_hotkey(void);

#endif
