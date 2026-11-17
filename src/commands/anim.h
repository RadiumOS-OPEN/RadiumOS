#ifndef ANIM_H
#define ANIM_H

#include <stdbool.h>

// anim [list] | anim start NAME [x y | text...] | anim stop NAME|all | anim demo
// Animated widgets pinned to the screen: they keep moving while the terminal scrolls.
void anim_command(int argc, char *argv[]);

// Starts / stops one by name with its default placement. False if unknown or there is no 1280x720 console.
bool anim_start(const char *name);
bool anim_stop(const char *name);

#endif // ANIM_H
