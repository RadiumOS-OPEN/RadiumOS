// wipe.h - Wipe / format disk command header
//
// Declares the wipe command function for the terminal shell.
//
// Usage:
//   wipe --confirm     wipes and reformats the /disk/ partition
//   wipe -y            alternative confirmation flag

#pragma once

#include <stdint.h>

// Command entry point
void diskpart_command(int argc, char *argv[]);