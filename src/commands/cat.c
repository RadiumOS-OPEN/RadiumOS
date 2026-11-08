// cat.c - Read text files
//
// Updated for the seamless /disk/ mount: no disk-specific logic here.
// Every path is forwarded to AVFS, which resolves it against the current
// working directory and routes /disk paths to FAT32 by itself — so
// "cat /disk/readme.txt", "cd /disk" + "cat readme.txt", and "cat note.txt"
// in the RAM fs all work identically.
//
// Usage:
//   cat <file> [file...]    print one or more files to the terminal

#include <stdint.h>
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../keyboard/keyboard.h"

#define CAT_MAX_BYTES 131072   /* 128 KB static read buffer */
#define CAT_MAX_PATH  256      /* Maximum path string length */

/* ── Path Resolution ────────────────────────────────────────────────────
   Handles:
     /disk/file.txt      → passes through as-is  (FAT32 via Avfs routing)
     ~/file.txt          → /file.txt
     /~/dir/file.txt     → /dir/file.txt
     bare name           → CWD/name
*/
int cat_resolve_path(const char* arg, char* out, int out_sz) {
    if (!arg || !arg[0]) return 0;

    /* ~/path or ~/ */
    if (arg[0] == '~' && arg[1] == '/') {
        const char* inner = arg + 2;
        int len = 0; while (inner[len]) len++;
        if (len == 0) { out[0] = '/'; out[1] = 0; return 1; }
        if (len + 2 > out_sz) return 0;
        out[0] = '/';
        for (int i = 0; i < len; i++) out[i + 1] = inner[i];
        out[len + 1] = 0;
        return 1;
    }

    /* /~/path or /~/ */
    if (arg[0] == '/' && arg[1] == '~' && arg[2] == '/') {
        const char* inner = arg + 3;
        int len = 0; while (inner[len]) len++;
        if (len == 0) { out[0] = '/'; out[1] = 0; return 1; }
        if (len + 2 > out_sz) return 0;
        out[0] = '/';
        for (int i = 0; i < len; i++) out[i + 1] = inner[i];
        out[len + 1] = 0;
        return 1;
    }

    /* Already absolute (includes /disk/...) — pass through */
    if (arg[0] == '/') {
        int i = 0;
        while (arg[i] && i < out_sz - 1) { out[i] = arg[i]; i++; }
        out[i] = 0;
        return 1;
    }

    /* Bare name — prepend CWD so /disk-relative names resolve correctly */
    {
        const char* cwd = avfs_get_cwd();
        int cwd_len = 0; while (cwd[cwd_len]) cwd_len++;
        int arg_len = 0; while (arg[arg_len]) arg_len++;
        if (cwd_len + 1 + arg_len + 1 > out_sz) return 0;
        int p = 0;
        if (cwd_len == 1 && cwd[0] == '/') {
            out[p++] = '/';
        } else {
            for (int i = 0; i < cwd_len; i++) out[p++] = cwd[i];
            out[p++] = '/';
        }
        for (int i = 0; i < arg_len; i++) out[p++] = arg[i];
        out[p] = 0;
        return 1;
    }
}

void cat_command(int argc, char *argv[])
{
    if (argc < 2) {
        const char *piped = shell_stdin_file();
        if (piped) {
            char *self_argv[2] = { argv[0], (char *)piped };
            cat_command(2, self_argv);
            return;
        }
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: cat <file> [file...]\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }

    static char buffer[CAT_MAX_BYTES + 1];
    char resolved_path[CAT_MAX_PATH];

    for (int i = 1; i < argc; i++) {
        if (!cat_resolve_path(argv[i], resolved_path, sizeof(resolved_path))) {
            terminal_setcolor(VGA_COLOR_LIGHT_RED);
            printr("cat: %s: path resolution failed or too long\n", argv[i]);
            terminal_setcolor(VGA_COLOR_WHITE);
            continue;
        }

        const char *path = resolved_path;

        // Directories are rejected up front (RAM fs and /disk alike).
        if (avfs_is_directory(path)) {
            terminal_setcolor(VGA_COLOR_LIGHT_RED);
            printr("cat: %s: is a directory\n", argv[i]);
            terminal_setcolor(VGA_COLOR_WHITE);
            continue;
        }

        int size = avfs_get_filesize(path);
        if (size == -1) {
            terminal_setcolor(VGA_COLOR_LIGHT_RED);
            printr("cat: %s: no such file or directory\n", argv[i]);
            terminal_setcolor(VGA_COLOR_WHITE);
            continue;
        }
        if ((uint32_t)size >= sizeof(buffer)) {
            terminal_setcolor(VGA_COLOR_LIGHT_RED);
            printr("cat: %s: file too large (", argv[i]);
            print_integer((uint32_t)size);
            print(" bytes, max ");
            print_integer((uint32_t)CAT_MAX_BYTES);
            print(")\n");
            terminal_setcolor(VGA_COLOR_WHITE);
            continue;
        }

        // avfs_get_content resolves the path (RAM or FAT32) and returns a
        // NUL-terminated copy of the whole file.
        if (avfs_get_content(path, buffer, sizeof(buffer)) != 0) {
            terminal_setcolor(VGA_COLOR_LIGHT_RED);
            printr("cat: %s: read error\n", argv[i]);
            terminal_setcolor(VGA_COLOR_WHITE);
            continue;
        }

        terminal_setcolor(VGA_COLOR_WHITE);
        print(buffer);
        if (size > 0 && buffer[size - 1] != '\n')
            print("\n");
    }
    terminal_setcolor(VGA_COLOR_WHITE);
}