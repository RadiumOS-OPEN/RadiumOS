#ifndef DESKTOP_H
#define DESKTOP_H
#include <stdbool.h>

// desktop: RadiumDE, an Xfce-style desktop environment drawn through the Vulkan implementation (see desktop.c)
void desktop_command(int argc, char *argv[]);
bool desktop_boot_wanted(void);   // false for nodesktop/safe/rescue/single/selftest or boot.cfg "desktop no"
void desktop_boot(void);          // login page and desktop; returns when the user leaves to the console

#endif // DESKTOP_H
