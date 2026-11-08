// fsck.c - fsck [-r] [-q]: check the /disk volume; -r repairs (root only), -q prints only the summary
#include <stdint.h>
#include <stdbool.h>
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../keyboard/keyboard.h"
#include "../user/users.h"

void fsck_command(int argc, char *argv[])
{
    bool repair = false, quiet = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) repair = true;
        else if (strcmp(argv[i], "-q") == 0) quiet = true;
        else {
            terminal_setcolor(VGA_COLOR_LIGHT_RED);
            print("Usage: fsck [-r] [-q]     check /disk; -r repairs what it finds (root only), -q: summary only\n");
            terminal_setcolor(VGA_COLOR_WHITE);
            shell_set_status(2);
            return;
        }
    }
    if (repair && !users_is_root()) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("fsck: permission denied - repairing needs root (sudo fsck -r)\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        shell_set_status(1);
        return;
    }

    avfs_fsck_t r;
    printr("fsck: checking /disk%s...\n", repair ? " (repair mode)" : "");
    if (avfs_fsck(repair, true, !quiet, &r) != 0) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("fsck: /disk is not mounted\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        shell_set_status(1);
        return;
    }
    printr("  %u directories, %u files, %u clusters used, %u free\n", r.dirs, r.files, r.used_clusters, r.free_clusters);
    if (r.problems == 0) {
        terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
        print("fsck: the volume is clean\n");
    } else if (repair) {
        terminal_setcolor(VGA_COLOR_LIGHT_BROWN);
        printr("fsck: %u problem(s) found, %u repaired. Run fsck again to confirm.\n", r.problems, r.fixed);
    } else {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        printr("fsck: %u problem(s) found. Run: sudo fsck -r\n", r.problems);
        shell_set_status(1);
    }
    terminal_setcolor(VGA_COLOR_WHITE);
}
