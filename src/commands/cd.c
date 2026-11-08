// cd.h - Change directory command
//
// Updated for the seamless /disk/ mount: this command contains NO
// disk-specific logic at all.  The raw user path is forwarded straight to
// AVFS, which resolves it against the current working directory (so
// absolute paths, relative names, "." and ".." all work) and transparently
// routes it to FAT32 when it lands under /disk.  Works identically from
// inside and outside the disk.
//
// Usage:
//   cd            print the current directory (pwd behaviour)
//   cd <path>     change into <path> (RAM fs or /disk, absolute or relative)

#pragma once

#include <stdint.h>
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"

void cd_command(int argc, char *argv[])
{
    // "cd" with no argument: show where we are.
    if (argc < 2 || argv[1][0] == '\0') {
        terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
        print(avfs_get_cwd());
        print("\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }

    // Forward the raw path — AVFS resolves it against the cwd and routes
    // /disk paths to the FAT32 volume by itself.
    int rc = avfs_chdir(argv[1]);
    if (rc == 0)
        return;

    terminal_setcolor(VGA_COLOR_LIGHT_RED);
    if (rc == -2)
        printr("cd: %s: not a directory\n", argv[1]);
    else
        printr("cd: %s: no such file or directory\n", argv[1]);
    terminal_setcolor(VGA_COLOR_WHITE);
}
