#ifndef REBOOT_H
#define REBOOT_H

#include <stdint.h> // Include for uint8_t type

// Function prototype for rebooting the system
void reboot_command(int argc, char* argv[]);
void reboot();
#endif // REBOOT_H
