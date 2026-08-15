#include "../terminal/terminal.h"
#include "../keyboard/keyboard.h"
#include "../io/io.h"


void savestate(void) {
    
    print("+============================+\n");
    print("|  1.)        |\n");
    print("+============================+\n");
    while (true) {
        int key = port_byte_in(0x60);
        if (key == 0x01) {
            return; // ESC
        }   else if (key == 0x02) {
            terminal_clear();
            //cpuInfo(); // NumberKey 1
        }
    }
}