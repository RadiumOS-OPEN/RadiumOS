// rm.h - Remove files or directories
//
// Updated for the seamless /disk/ mount: no disk-specific logic here.
// Every path is forwarded to AVFS, which resolves it against the current
// working directory and routes /disk paths to FAT32 — deleting on the
// real disk and deleting in the RAM fs use the exact same code path.
//
// Safety rails (evaluated BEFORE anything is deleted):
//   - "." and ".." are refused outright (POSIX rm behaviour)
//   - anything that resolves to "/"     is refused (would wipe the RAM fs root)
//   - anything that resolves to "/disk" is refused (the mount point itself;
//     AVFS refuses it too, this is a second lock on the same door)
//
// Usage:
//   rm <file>...          remove files (also empty directories)
//   rm -r <dir>...        remove directories recursively
//   rm -f <path>...       force: silently ignore nonexistent targets

#pragma once

#include <stdint.h>
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"

#define RM_PATH_MAX 512

// Resolve `path` against the AVFS cwd WITHOUT touching AVFS internals.
// Used purely to detect dangerous targets before deleting.  Mirrors the
// same rules AVFS applies: leading '/' is absolute, otherwise the cwd is
// prefixed, then "." components are dropped and ".." pops one component.
static void rm_resolve_for_guard(const char *path, char *out, int outsz)
{
    char abs[RM_PATH_MAX];

    if (path[0] == '/') {
        snprintf(abs, sizeof(abs), "%s", path);
    } else {
        const char *cwd = avfs_get_cwd();
        snprintf(abs, sizeof(abs), "%s%s%s",
                 cwd, (strcmp(cwd, "/") == 0) ? "" : "/", path);
    }

    // component-wise normalization
    char norm[RM_PATH_MAX];
    strcpy(norm, "/");

    char tmp[RM_PATH_MAX];
    strcpy(tmp, abs);
    char *tok = strtok(tmp, "/");
    while (tok) {
        if (strcmp(tok, ".") == 0) {
            // skip
        } else if (strcmp(tok, "..") == 0) {
            if (strcmp(norm, "/") != 0) {
                char *slash = strrchr(norm, '/');
                if (slash == norm) norm[1] = '\0';   // "/x"  -> "/"
                else               *slash  = '\0';   // "/a/b" -> "/a"
            }
        } else {
            if (strcmp(norm, "/") != 0) strcat(norm, "/");
            strcat(norm, tok);
        }
        tok = strtok(NULL, "/");
    }

    snprintf(out, outsz, "%s", norm);
}

// True when the target must never be removed (see header comment).
static bool rm_target_is_protected(const char *path)
{
    if (strcmp(path, ".") == 0 || strcmp(path, "..") == 0)
        return true;

    char resolved[RM_PATH_MAX];
    rm_resolve_for_guard(path, resolved, sizeof(resolved));

    if (strcmp(resolved, "/") == 0)      return true;   // RAM fs root
    if (strcmp(resolved, "/disk") == 0)  return true;   // disk mount point
    return false;
}

void rm_command(int argc, char *argv[])
{
    bool recursive = false;
    bool force     = false;
    int  targets   = 0;

    // ---- parse arguments -------------------------------------------------
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (const char *p = argv[i] + 1; *p != '\0'; p++) {
                if      (*p == 'r') recursive = true;
                else if (*p == 'f') force     = true;
                else {
                    terminal_setcolor(VGA_COLOR_LIGHT_RED);
                    printr("rm: unknown flag '-%c'\n", *p);
                    print("Usage: rm [-r] [-f] <file|dir>...\n");
                    terminal_setcolor(VGA_COLOR_WHITE);
                    return;
                }
            }
        } else {
            targets++;
        }
    }

    if (targets == 0) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: rm [-r] [-f] <file|dir>...\n");
        print("       rm -r removes directories recursively\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }

    // ---- remove each target (AVFS routes /disk paths to FAT32) ----------
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0')
            continue;
        const char *path = argv[i];

        if (rm_target_is_protected(path)) {
            terminal_setcolor(VGA_COLOR_LIGHT_RED);
            printr("rm: refusing to remove '%s'\n", path);
            terminal_setcolor(VGA_COLOR_WHITE);
            continue;
        }

        if (avfs_is_directory(path)) {
            if (!recursive) {
                terminal_setcolor(VGA_COLOR_LIGHT_RED);
                printr("rm: %s: is a directory (use rm -r)\n", path);
                terminal_setcolor(VGA_COLOR_WHITE);
                continue;
            }
            if (avfs_remove_dir_recursive(path) != 0) {
                terminal_setcolor(VGA_COLOR_LIGHT_RED);
                printr("rm: could not remove '%s'\n", path);
                terminal_setcolor(VGA_COLOR_WHITE);
            }
        } else {
            int rc = avfs_remove_file(path);
            if (rc == 0)
                continue;                       // silent success, like real rm
            if (rc == -1 && force)
                continue;                       // -f: ignore missing targets
            terminal_setcolor(VGA_COLOR_LIGHT_RED);
            if (rc == -2)
                printr("rm: %s: directory not empty\n", path);
            else
                printr("rm: %s: no such file or directory\n", path);
            terminal_setcolor(VGA_COLOR_WHITE);
        }
    }
    terminal_setcolor(VGA_COLOR_WHITE);
}
