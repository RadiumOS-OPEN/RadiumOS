// fileutils.c - mkdir, touch, cp, pwd
//
//   mkdir [-p] <dir>...             create directories (-p: also create missing parents, no error if it exists)
//   touch <file>...                 create empty files that do not exist yet
//   cp [-r] [-n] [-v] <src> <dest>  copy a file (or a directory tree with -r)
//   cp [-r] [-n] [-v] <src>... <dir>
//   pwd                             print the current directory
//
// Copying goes through the same read-write-verify helpers as mv, so it works
// between the RAM filesystem and /disk.

#include <stdint.h>
#include <stdbool.h>
#include "fileutils.h"
#include "mv.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"

#define FU_PATH_MAX 512
#define FU_MAX_ARGS 32

#define FU_ERR(...) do { \
    terminal_setcolor(VGA_COLOR_LIGHT_RED); \
    printr(__VA_ARGS__); \
    terminal_setcolor(VGA_COLOR_WHITE); \
} while (0)

void pwd_command(int argc, char *argv[])
{
    (void)argc; (void)argv;
    print(avfs_get_cwd());
    print("\n");
}

// Creates every missing directory along `path`. 0 ok, -1 create failed, -2 a file is in the way.
static int mkdir_parents(const char *path)
{
    char resolved[FU_PATH_MAX], partial[FU_PATH_MAX];
    mv_resolve(path, resolved);

    size_t len = strlen(resolved);
    for (size_t i = 1; i <= len; i++) {
        if (resolved[i] != '/' && resolved[i] != '\0') continue;

        memcpy(partial, resolved, i);
        partial[i] = '\0';

        if (avfs_is_directory(partial)) continue;
        if (avfs_file_exists(partial)) return -2;
        if (avfs_create_dir(partial) != 0) return -1;
    }
    return 0;
}

void mkdir_command(int argc, char *argv[])
{
    bool parents = false;
    int targets = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (const char *p = argv[i] + 1; *p; p++) {
                if (*p == 'p') parents = true;
                else {
                    FU_ERR("mkdir: unknown flag '-%c'\n", *p);
                    return;
                }
            }
        } else {
            targets++;
        }
    }
    if (targets == 0) {
        FU_ERR("Usage: mkdir [-p] <dir>...\n");
        return;
    }

    for (int i = 1; i < argc; i++) {
        const char *path = argv[i];
        if (path[0] == '-' && path[1] != '\0') continue;

        if (parents) {
            int rc = mkdir_parents(path);
            if (rc == -2) FU_ERR("mkdir: cannot create directory '%s': a file is in the way\n", path);
            else if (rc != 0) FU_ERR("mkdir: cannot create directory '%s'\n", path);
            continue;
        }

        if (mv_exists(path)) {
            FU_ERR("mkdir: cannot create directory '%s': already exists\n", path);
        } else if (avfs_create_dir(path) != 0) {
            FU_ERR("mkdir: cannot create directory '%s': parent missing or invalid name (try mkdir -p)\n", path);
        }
    }
}

void touch_command(int argc, char *argv[])
{
    if (argc < 2) {
        FU_ERR("Usage: touch <file>...\n");
        return;
    }

    for (int i = 1; i < argc; i++) {
        const char *path = argv[i];
        if (mv_exists(path)) continue;   // AVFS keeps no timestamps, so there is nothing to update

        if (avfs_save_file(path, NULL, 0, false) != 0)
            FU_ERR("touch: cannot touch '%s': no such directory or invalid name\n", path);
    }
}

// Returns 0 on success, 1 on any failure.
static int cp_one(const char *src, const char *dst, bool recursive, bool no_clobber, bool verbose)
{
    char rs[FU_PATH_MAX], rd[FU_PATH_MAX];
    mv_resolve(src, rs);
    mv_resolve(dst, rd);

    if (!mv_exists(src)) {
        FU_ERR("cp: cannot stat '%s': no such file or directory\n", src);
        return 1;
    }
    if (strcmp(rs, rd) == 0) {
        FU_ERR("cp: '%s' and '%s' are the same file\n", src, dst);
        return 1;
    }

    bool src_is_dir = avfs_is_directory(src);
    if (src_is_dir) {
        if (!recursive) {
            FU_ERR("cp: -r not specified; omitting directory '%s'\n", src);
            return 1;
        }
        if (mv_is_inside(rd, rs)) {
            FU_ERR("cp: cannot copy '%s' into itself\n", src);
            return 1;
        }
    }

    bool dst_existed = mv_exists(dst);
    if (dst_existed) {
        if (no_clobber) return 0;
        if (src_is_dir || avfs_is_directory(dst)) {
            FU_ERR("cp: cannot overwrite '%s'\n", dst);
            return 1;
        }
    }

    int rc = src_is_dir ? mv_copy_tree(src, dst, 0) : mv_copy_file(src, dst);
    if (rc != 0) {
        if (!dst_existed) {
            if (src_is_dir) avfs_remove_dir_recursive(dst);
            else            avfs_remove_file(dst);
        }
        FU_ERR("cp: could not copy '%s' to '%s' (%s)\n", src, dst,
               rc == -2 ? "out of memory" : "read/write error, disk full or directory too large");
        return 1;
    }

    if (verbose) printr("'%s' -> '%s'\n", src, dst);
    return 0;
}

void cp_command(int argc, char *argv[])
{
    bool recursive = false, no_clobber = false, verbose = false;
    const char *paths[FU_MAX_ARGS];
    int np = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (const char *p = argv[i] + 1; *p; p++) {
                if      (*p == 'r' || *p == 'R') recursive = true;
                else if (*p == 'n') no_clobber = true;
                else if (*p == 'v') verbose = true;
                else if (*p == 'f') { /* overwrites by default */ }
                else {
                    FU_ERR("cp: unknown flag '-%c'\n", *p);
                    return;
                }
            }
        } else if (np < FU_MAX_ARGS) {
            paths[np++] = argv[i];
        } else {
            FU_ERR("cp: too many arguments\n");
            return;
        }
    }

    if (np < 2) {
        FU_ERR("Usage: cp [-r] [-n] [-v] <source> <dest>\n");
        FU_ERR("       cp [-r] [-n] [-v] <source>... <directory>\n");
        return;
    }

    const char *dest = paths[np - 1];
    bool dest_is_dir = avfs_is_directory(dest);

    if (np > 2 && !dest_is_dir) {
        FU_ERR("cp: target '%s' is not a directory\n", dest);
        return;
    }

    for (int i = 0; i < np - 1; i++) {
        char final_path[FU_PATH_MAX];

        if (dest_is_dir) {
            char base[AVFS_FILENAME_MAX];
            if (!mv_basename(paths[i], base) || !mv_join(dest, base, final_path)) {
                FU_ERR("cp: invalid name '%s'\n", paths[i]);
                continue;
            }
        } else {
            if (strlen(dest) >= FU_PATH_MAX) {
                FU_ERR("cp: destination path too long\n");
                return;
            }
            strcpy(final_path, dest);
        }

        cp_one(paths[i], final_path, recursive, no_clobber, verbose);
    }
}
