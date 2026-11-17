// mv.c - Move or rename files and directories
//
// AVFS has no rename primitive, so a move is copy -> verify -> delete. That
// also makes moves between the RAM fs and /disk work with the same code.
// The source is only deleted after the copy has been written and its size
// checked, so a failed move never loses data.
//
// Usage:
//   mv <src> <dest>          rename src to dest (dest is overwritten if it is a file)
//   mv <src>... <dir>        move every src into dir
//   mv -n ...                never overwrite an existing destination
//   mv -v ...                print each move
//   mv -f ...                accepted for compatibility (mv never prompts)

#include <stdint.h>
#include <stdbool.h>
#include "mv.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"

#define MV_PATH_MAX   512
#define MV_MAX_DEPTH  16
#define MV_MAX_ARGS   32
#define MV_BASE_MAX   (AVFS_FILENAME_MAX - 1)
#define MV_LIST_MAX   512

// DirectoryEntry.name holds 31 characters; a full-length name may be truncated.
#define MV_ENTRY_NAME_LIMIT 31

#define MV_ERR(...) do { \
    terminal_setcolor(VGA_COLOR_LIGHT_RED); \
    printr(__VA_ARGS__); \
    terminal_setcolor(VGA_COLOR_WHITE); \
} while (0)

// Absolute, normalized form of `path` (cwd applied, "." dropped, ".." popped).
void mv_resolve(const char *path, char *out)
{
    char abs[MV_PATH_MAX];

    if (path[0] == '/') {
        snprintf(abs, sizeof(abs), "%s", path);
    } else {
        const char *cwd = avfs_get_cwd();
        snprintf(abs, sizeof(abs), "%s%s%s",
                 cwd, (strcmp(cwd, "/") == 0) ? "" : "/", path);
    }

    char norm[MV_PATH_MAX];
    strcpy(norm, "/");

    char *tok = strtok(abs, "/");
    while (tok) {
        if (strcmp(tok, ".") == 0) {
            // skip
        } else if (strcmp(tok, "..") == 0) {
            if (strcmp(norm, "/") != 0) {
                char *slash = strrchr(norm, '/');
                if (slash == norm) norm[1] = '\0';
                else               *slash  = '\0';
            }
        } else {
            if (strcmp(norm, "/") != 0) strcat(norm, "/");
            strcat(norm, tok);
        }
        tok = strtok(NULL, "/");
    }

    snprintf(out, MV_PATH_MAX, "%s", norm);
}

// Last path component without trailing slashes.
bool mv_basename(const char *path, char *out)
{
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;

    size_t start = len;
    while (start > 0 && path[start - 1] != '/') start--;

    size_t n = len - start;
    if (n == 0 || n > MV_BASE_MAX) return false;

    memcpy(out, path + start, n);
    out[n] = '\0';
    return true;
}

bool mv_join(const char *dir, const char *name, char *out)
{
    size_t dl = strlen(dir);
    bool slash = dl > 0 && dir[dl - 1] == '/';
    if (dl + (slash ? 0 : 1) + strlen(name) >= MV_PATH_MAX) return false;

    strcpy(out, dir);
    if (!slash) strcat(out, "/");
    strcat(out, name);
    return true;
}

bool mv_exists(const char *path)
{
    return avfs_file_exists(path) || avfs_is_directory(path);
}

// 0 ok, -1 read/write failure, -2 out of memory
int mv_copy_file(const char *src, const char *dst)
{
    int size = avfs_get_filesize(src);
    if (size < 0) return -1;

    uint8_t *buf = NULL;
    if (size > 0) {
        buf = (uint8_t *)malloc((size_t)size);
        if (!buf) return -2;
        if (avfs_read_file(src, buf, (uint32_t)size, 0) != 0) {
            free(buf);
            return -1;
        }
    }

    int rc = avfs_save_file(dst, buf, (uint32_t)size, true);
    if (buf) free(buf);
    if (rc != 0) return -1;

    return avfs_get_filesize(dst) == size ? 0 : -1;
}

// Copies the directory tree at src into a new directory dst.
int mv_copy_tree(const char *src, const char *dst, int depth)
{
    if (depth > MV_MAX_DEPTH) return -1;
    if (avfs_create_dir(dst) != 0 && !avfs_is_directory(dst)) return -1;

    DirectoryEntry *list = (DirectoryEntry *)malloc(sizeof(DirectoryEntry) * MV_LIST_MAX);
    if (!list) return -2;

    int count = avfs_list_directory(src, list, MV_LIST_MAX);
    if (count < 0 || count >= MV_LIST_MAX) {
        free(list);
        return -1;
    }

    int rc = 0;
    for (int i = 0; i < count && rc == 0; i++) {
        const char *name = list[i].name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (strlen(name) >= MV_ENTRY_NAME_LIMIT) { rc = -1; break; }

        char s[MV_PATH_MAX], d[MV_PATH_MAX];
        if (!mv_join(src, name, s) || !mv_join(dst, name, d)) { rc = -1; break; }

        rc = list[i].is_directory ? mv_copy_tree(s, d, depth + 1)
                                  : mv_copy_file(s, d);
    }

    free(list);
    return rc;
}

bool mv_is_inside(const char *resolved_path, const char *resolved_dir)
{
    size_t n = strlen(resolved_dir);
    if (strcmp(resolved_dir, "/") == 0) return true;
    return strncmp(resolved_path, resolved_dir, n) == 0 &&
           (resolved_path[n] == '\0' || resolved_path[n] == '/');
}

// Returns 0 on success, 1 on any failure.
static int mv_one(const char *src, const char *dst, bool no_clobber, bool verbose)
{
    if (strcmp(src, ".") == 0 || strcmp(src, "..") == 0) {
        MV_ERR("mv: refusing to move '%s'\n", src);
        return 1;
    }

    char rs[MV_PATH_MAX], rd[MV_PATH_MAX];
    mv_resolve(src, rs);
    mv_resolve(dst, rd);

    if (strcmp(rs, "/") == 0 || strcmp(rs, "/disk") == 0) {
        MV_ERR("mv: refusing to move '%s'\n", src);
        return 1;
    }
    if (!mv_exists(src)) {
        MV_ERR("mv: cannot stat '%s': no such file or directory\n", src);
        return 1;
    }
    if (strcmp(rs, rd) == 0) {
        MV_ERR("mv: '%s' and '%s' are the same file\n", src, dst);
        return 1;
    }

    bool src_is_dir = avfs_is_directory(src);
    if (src_is_dir) {
        if (mv_is_inside(rd, rs)) {
            MV_ERR("mv: cannot move '%s' into itself\n", src);
            return 1;
        }
        if (mv_is_inside(avfs_get_cwd(), rs)) {
            MV_ERR("mv: '%s' contains the current directory\n", src);
            return 1;
        }
    }

    bool dst_existed = mv_exists(dst);
    if (dst_existed) {
        if (no_clobber) return 0;
        if (src_is_dir || avfs_is_directory(dst)) {
            MV_ERR("mv: cannot overwrite '%s'\n", dst);
            return 1;
        }
    }

    if (src_is_dir) {
        int rc = mv_copy_tree(src, dst, 0);
        if (rc != 0) {
            avfs_remove_dir_recursive(dst);   // dst did not exist before, so this only removes our partial copy
            MV_ERR("mv: could not copy '%s' (%s); source left in place\n", src,
                   rc == -2 ? "out of memory" : "read/write error or directory too large");
            return 1;
        }
        if (avfs_remove_dir_recursive(src) != 0) {
            MV_ERR("mv: copied '%s' but could not remove the original\n", src);
            return 1;
        }
    } else {
        int rc = mv_copy_file(src, dst);
        if (rc != 0) {
            if (!dst_existed) avfs_remove_file(dst);
            MV_ERR("mv: could not copy '%s' to '%s' (%s); source left in place\n", src, dst,
                   rc == -2 ? "out of memory" : "write failed or disk full");
            return 1;
        }
        if (avfs_remove_file(src) != 0) {
            MV_ERR("mv: copied '%s' but could not remove the original\n", src);
            return 1;
        }
    }

    if (verbose) printr("'%s' -> '%s'\n", src, dst);
    return 0;
}

static void mv_usage(void)
{
    terminal_setcolor(VGA_COLOR_LIGHT_RED);
    print("Usage: mv [-n] [-v] <source> <dest>\n");
    print("       mv [-n] [-v] <source>... <directory>\n");
    terminal_setcolor(VGA_COLOR_WHITE);
}

void mv_command(int argc, char *argv[])
{
    bool no_clobber = false;
    bool verbose    = false;
    const char *paths[MV_MAX_ARGS];
    int np = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (const char *p = argv[i] + 1; *p != '\0'; p++) {
                if      (*p == 'n') no_clobber = true;
                else if (*p == 'v') verbose    = true;
                else if (*p == 'f') { /* never prompts */ }
                else {
                    MV_ERR("mv: unknown flag '-%c'\n", *p);
                    mv_usage();
                    return;
                }
            }
        } else if (np < MV_MAX_ARGS) {
            paths[np++] = argv[i];
        } else {
            MV_ERR("mv: too many arguments\n");
            return;
        }
    }

    if (np < 2) {
        mv_usage();
        return;
    }

    const char *dest = paths[np - 1];
    bool dest_is_dir = avfs_is_directory(dest);

    if (np > 2 && !dest_is_dir) {
        MV_ERR("mv: target '%s' is not a directory\n", dest);
        return;
    }

    for (int i = 0; i < np - 1; i++) {
        char final_path[MV_PATH_MAX];

        if (dest_is_dir) {
            char base[AVFS_FILENAME_MAX];
            if (!mv_basename(paths[i], base) || !mv_join(dest, base, final_path)) {
                MV_ERR("mv: invalid name '%s'\n", paths[i]);
                continue;
            }
        } else {
            if (strlen(dest) >= MV_PATH_MAX) {
                MV_ERR("mv: destination path too long\n");
                return;
            }
            strcpy(final_path, dest);
        }

        mv_one(paths[i], final_path, no_clobber, verbose);
    }
}
