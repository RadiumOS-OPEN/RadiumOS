// moreutils.c - test, [, find, tree, stat, du, diff, tee, seq, whoami, hostname
//
//   test EXPR | [ EXPR ]        succeed or fail for use with && and ||:
//                               -e -f -d -s -z -n <arg>, <a> = <b>, <a> != <b>,
//                               <n> -eq|-ne|-lt|-le|-gt|-ge <n>, and a leading ! to negate
//   find [dir] [-name glob] [-type f|d] [-maxdepth N]
//   tree [dir]                  directory tree
//   stat <path>...              type and size
//   du [-s] [dir]               space used, in KB
//   diff <a> <b>                line differences; exit status 1 when the files differ
//   tee [-a] <file>             copy piped input to the screen and a file
//   seq [first] last            print a range of numbers
//   whoami, hostname

#include <stdint.h>
#include <stdbool.h>
#include "moreutils.h"
#include "mv.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../keyboard/keyboard.h"
#include "../user/users.h"

#define MU_PATH_MAX   512
#define MU_LIST_MAX   256
#define MU_MAX_DEPTH  12

#define MU_ERR(...) do { \
    terminal_setcolor(VGA_COLOR_LIGHT_RED); \
    printr(__VA_ARGS__); \
    terminal_setcolor(VGA_COLOR_WHITE); \
} while (0)

static bool mu_parse_int(const char *s, int *out)
{
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    if (!*s) return false;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (*s - '0');
    }
    *out = neg ? -v : v;
    return true;
}

// ---- test / [ ----------------------------------------------------------------

static bool test_unary(const char *op, const char *arg, bool *res)
{
    if      (strcmp(op, "-e") == 0) *res = mv_exists(arg);
    else if (strcmp(op, "-f") == 0) *res = avfs_file_exists(arg) && !avfs_is_directory(arg);
    else if (strcmp(op, "-d") == 0) *res = avfs_is_directory(arg);
    else if (strcmp(op, "-s") == 0) *res = avfs_file_exists(arg) && !avfs_is_directory(arg) && avfs_get_filesize(arg) > 0;
    else if (strcmp(op, "-z") == 0) *res = (arg[0] == '\0');
    else if (strcmp(op, "-n") == 0) *res = (arg[0] != '\0');
    else return false;
    return true;
}

// 1 recognised, 0 unknown operator, -1 operands are not integers
static int test_binary(const char *a, const char *op, const char *b, bool *res)
{
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) { *res = strcmp(a, b) == 0; return 1; }
    if (strcmp(op, "!=") == 0)                         { *res = strcmp(a, b) != 0; return 1; }

    static const char *const NUM_OPS[] = { "-eq", "-ne", "-lt", "-le", "-gt", "-ge" };
    for (int k = 0; k < 6; k++) {
        if (strcmp(op, NUM_OPS[k]) != 0) continue;
        int x, y;
        if (!mu_parse_int(a, &x) || !mu_parse_int(b, &y)) return -1;
        switch (k) {
        case 0: *res = x == y; break;
        case 1: *res = x != y; break;
        case 2: *res = x <  y; break;
        case 3: *res = x <= y; break;
        case 4: *res = x >  y; break;
        default: *res = x >= y; break;
        }
        return 1;
    }
    return 0;
}

static void test_eval(int n, char **v)
{
    bool negate = false;
    if (n >= 1 && strcmp(v[0], "!") == 0) { negate = true; v++; n--; }

    bool res = false;
    bool ok = true;

    if (n == 0) {
        res = false;
    } else if (n == 1) {
        res = v[0][0] != '\0';
    } else if (n == 2) {
        ok = test_unary(v[0], v[1], &res);
        if (!ok) MU_ERR("test: unknown operator '%s'\n", v[0]);
    } else if (n == 3) {
        int r = test_binary(v[0], v[1], v[2], &res);
        ok = (r == 1);
        if (r == 0)  MU_ERR("test: unknown operator '%s'\n", v[1]);
        if (r == -1) MU_ERR("test: integer expression expected\n");
    } else {
        ok = false;
        MU_ERR("test: too many arguments\n");
    }

    if (!ok) { shell_set_status(2); return; }
    if (res == negate) shell_set_status(1);
}

void test_command(int argc, char *argv[])
{
    test_eval(argc - 1, argv + 1);
}

void bracket_command(int argc, char *argv[])
{
    if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
        MU_ERR("[: missing ']'\n");
        shell_set_status(2);
        return;
    }
    test_eval(argc - 2, argv + 1);
}

// ---- directory walking helpers ------------------------------------------------

static DirectoryEntry *mu_list(const char *dir, int *count)
{
    DirectoryEntry *list = (DirectoryEntry *)malloc(sizeof(DirectoryEntry) * MU_LIST_MAX);
    if (!list) return NULL;

    int n = avfs_list_directory(dir, list, MU_LIST_MAX);
    if (n < 0) { free(list); return NULL; }

    // drop "." and "..", then sort by name
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].name, ".") == 0 || strcmp(list[i].name, "..") == 0) continue;
        list[kept++] = list[i];
    }
    for (int i = 1; i < kept; i++) {
        DirectoryEntry key = list[i];
        int j = i - 1;
        while (j >= 0 && strcmp(list[j].name, key.name) > 0) { list[j + 1] = list[j]; j--; }
        list[j + 1] = key;
    }
    *count = kept;
    return list;
}

static bool mu_glob(const char *p, const char *s)
{
    if (!*p) return !*s;
    if (*p == '*') {
        while (*p == '*') p++;
        for (;; s++) {
            if (mu_glob(p, s)) return true;
            if (!*s) return false;
        }
    }
    if (!*s) return false;
    if (*p == '?' || *p == *s) return mu_glob(p + 1, s + 1);
    return false;
}

// ---- find ----------------------------------------------------------------------

typedef struct { const char *pattern; char type; int max_depth; int hits; } find_opts_t;

static void find_visit(const find_opts_t *o, const char *path, const char *name, bool is_dir)
{
    if (o->pattern && !mu_glob(o->pattern, name)) return;
    if (o->type == 'f' && is_dir) return;
    if (o->type == 'd' && !is_dir) return;
    print(path);
    print("\n");
}

static void find_walk(find_opts_t *o, const char *dir, int depth)
{
    if (depth >= o->max_depth || depth >= MU_MAX_DEPTH) return;

    int n;
    DirectoryEntry *list = mu_list(dir, &n);
    if (!list) return;

    for (int i = 0; i < n; i++) {
        char child[MU_PATH_MAX];
        if (!mv_join(dir, list[i].name, child)) continue;
        find_visit(o, child, list[i].name, list[i].is_directory != 0);
        if (list[i].is_directory) find_walk(o, child, depth + 1);
    }
    free(list);
}

void find_command(int argc, char *argv[])
{
    find_opts_t o = { NULL, 0, MU_MAX_DEPTH, 0 };
    const char *dir = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            o.pattern = argv[++i];
        } else if (strcmp(argv[i], "-type") == 0 && i + 1 < argc) {
            o.type = argv[++i][0];
            if (o.type != 'f' && o.type != 'd') { MU_ERR("find: -type takes f or d\n"); return; }
        } else if (strcmp(argv[i], "-maxdepth") == 0 && i + 1 < argc) {
            if (!mu_parse_int(argv[++i], &o.max_depth) || o.max_depth < 0) { MU_ERR("find: bad -maxdepth\n"); return; }
        } else if (argv[i][0] != '-' && !dir) {
            dir = argv[i];
        } else {
            MU_ERR("Usage: find [dir] [-name glob] [-type f|d] [-maxdepth N]\n");
            return;
        }
    }
    if (!dir) dir = ".";

    if (!mv_exists(dir)) {
        MU_ERR("find: '%s': no such file or directory\n", dir);
        return;
    }

    char base[AVFS_FILENAME_MAX];
    if (!mv_basename(dir, base)) strcpy(base, dir);
    find_visit(&o, dir, base, avfs_is_directory(dir));
    if (avfs_is_directory(dir)) find_walk(&o, dir, 0);
}

// ---- tree ----------------------------------------------------------------------

static void tree_walk(const char *dir, const char *prefix, int depth, int *dirs, int *files)
{
    if (depth >= MU_MAX_DEPTH) return;

    int n;
    DirectoryEntry *list = mu_list(dir, &n);
    if (!list) return;

    for (int i = 0; i < n; i++) {
        bool last = (i == n - 1);
        bool is_dir = list[i].is_directory != 0;

        print(prefix);
        print(last ? "`-- " : "|-- ");
        terminal_setcolor(is_dir ? VGA_COLOR_LIGHT_CYAN : VGA_COLOR_LIGHT_GREY);
        print(list[i].name);
        if (is_dir) print("/");
        terminal_setcolor(VGA_COLOR_LIGHT_GREY);
        print("\n");

        if (!is_dir) { (*files)++; continue; }
        (*dirs)++;

        char child[MU_PATH_MAX], next_prefix[128];
        if (!mv_join(dir, list[i].name, child)) continue;
        if (strlen(prefix) + 5 >= sizeof(next_prefix)) continue;
        strcpy(next_prefix, prefix);
        strcat(next_prefix, last ? "    " : "|   ");
        tree_walk(child, next_prefix, depth + 1, dirs, files);
    }
    free(list);
}

void tree_command(int argc, char *argv[])
{
    const char *dir = (argc >= 2) ? argv[1] : ".";
    if (argc > 2) { MU_ERR("Usage: tree [dir]\n"); return; }
    if (!avfs_is_directory(dir)) {
        MU_ERR("tree: '%s': not a directory\n", dir);
        return;
    }

    int dirs = 0, files = 0;
    print(dir);
    print("\n");
    tree_walk(dir, "", 0, &dirs, &files);
    printr("\n%d director%s, %d file%s\n", dirs, dirs == 1 ? "y" : "ies", files, files == 1 ? "" : "s");
}

// ---- stat ----------------------------------------------------------------------

void stat_command(int argc, char *argv[])
{
    if (argc < 2) { MU_ERR("Usage: stat <path>...\n"); return; }

    for (int i = 1; i < argc; i++) {
        DirectoryEntry e;
        if (avfs_get_entry_info(argv[i], &e) != 0) {
            MU_ERR("stat: cannot stat '%s': no such file or directory\n", argv[i]);
            continue;
        }
        char resolved[MU_PATH_MAX];
        mv_resolve(argv[i], resolved);

        printr("  Path: %s\n", resolved);
        printr("  Type: %s\n", e.is_directory ? "directory" : "file");
        if (!e.is_directory) printr("  Size: %u bytes\n", e.size);
    }
}

// ---- du ------------------------------------------------------------------------

static uint32_t du_walk(const char *dir, bool summary, bool top, int depth)
{
    uint32_t total = 0;

    if (depth < MU_MAX_DEPTH) {
        int n;
        DirectoryEntry *list = mu_list(dir, &n);
        if (list) {
            for (int i = 0; i < n; i++) {
                if (!list[i].is_directory) { total += list[i].size; continue; }
                char child[MU_PATH_MAX];
                if (mv_join(dir, list[i].name, child)) total += du_walk(child, summary, false, depth + 1);
            }
            free(list);
        }
    }

    if (!summary || top) printr("%7u  %s\n", (total + 1023) / 1024, dir);
    return total;
}

void du_command(int argc, char *argv[])
{
    bool summary = false;
    const char *target = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) summary = true;
        else if (argv[i][0] != '-' && !target) target = argv[i];
        else { MU_ERR("Usage: du [-s] [dir]\n"); return; }
    }
    if (!target) target = ".";

    if (!mv_exists(target)) {
        MU_ERR("du: cannot access '%s': no such file or directory\n", target);
        return;
    }
    if (!avfs_is_directory(target)) {
        int size = avfs_get_filesize(target);
        printr("%7u  %s\n", size < 0 ? 0u : ((uint32_t)size + 1023) / 1024, target);
        return;
    }
    du_walk(target, summary, true, 0);
}

// ---- diff ----------------------------------------------------------------------

#define DIFF_MAX_BYTES 65536
#define DIFF_MAX_LINES 2048
#define DIFF_WINDOW    10

static char  diff_buf_a[DIFF_MAX_BYTES + 1], diff_buf_b[DIFF_MAX_BYTES + 1];
static char *diff_lines_a[DIFF_MAX_LINES], *diff_lines_b[DIFF_MAX_LINES];

// Loads the file and cuts it into lines. Returns the line count or -1.
static int diff_load(const char *path, char *buf, char **lines)
{
    if (!avfs_file_exists(path) || avfs_is_directory(path)) {
        MU_ERR("diff: %s: no such file\n", path);
        return -1;
    }
    int size = avfs_get_filesize(path);
    if (size < 0 || size > DIFF_MAX_BYTES) {
        MU_ERR("diff: %s: file too large (max %d bytes)\n", path, DIFF_MAX_BYTES);
        return -1;
    }
    if (size == 0) { buf[0] = '\0'; return 0; }
    if (avfs_get_content(path, buf, DIFF_MAX_BYTES + 1) != 0) {
        MU_ERR("diff: %s: read error\n", path);
        return -1;
    }

    int n = 0;
    char *p = buf, *end = buf + size;
    while (p < end) {
        if (n == DIFF_MAX_LINES) { MU_ERR("diff: %s: too many lines (max %d)\n", path, DIFF_MAX_LINES); return -1; }
        char *line = p;
        while (p < end && *p != '\n') p++;
        char *line_end = p;
        if (p < end) p++;
        if (line_end > line && line_end[-1] == '\r') line_end--;
        *line_end = '\0';
        lines[n++] = line;
    }
    return n;
}

static void diff_print(char sign, int number, const char *text)
{
    terminal_setcolor(sign == '-' ? VGA_COLOR_LIGHT_RED : VGA_COLOR_LIGHT_GREEN);
    printr("%c %4d  %s\n", sign, number, text);
    terminal_setcolor(VGA_COLOR_WHITE);
}

void diff_command(int argc, char *argv[])
{
    if (argc != 3) { MU_ERR("Usage: diff <file1> <file2>\n"); return; }

    int na = diff_load(argv[1], diff_buf_a, diff_lines_a);
    if (na < 0) return;
    int nb = diff_load(argv[2], diff_buf_b, diff_lines_b);
    if (nb < 0) return;

    bool differ = false;
    int i = 0, j = 0;
    while (i < na || j < nb) {
        if (i < na && j < nb && strcmp(diff_lines_a[i], diff_lines_b[j]) == 0) { i++; j++; continue; }
        differ = true;

        if (i >= na) { diff_print('+', j + 1, diff_lines_b[j]); j++; continue; }
        if (j >= nb) { diff_print('-', i + 1, diff_lines_a[i]); i++; continue; }

        // Find the nearest place where the two files line up again.
        int drop = 0, add = 0;
        for (int k = 1; k <= DIFF_WINDOW && !drop && !add; k++) {
            if (i + k < na && strcmp(diff_lines_a[i + k], diff_lines_b[j]) == 0) drop = k;
            else if (j + k < nb && strcmp(diff_lines_b[j + k], diff_lines_a[i]) == 0) add = k;
        }

        if (drop) {
            for (int k = 0; k < drop; k++) diff_print('-', i + k + 1, diff_lines_a[i + k]);
            i += drop;
        } else if (add) {
            for (int k = 0; k < add; k++) diff_print('+', j + k + 1, diff_lines_b[j + k]);
            j += add;
        } else {
            diff_print('-', i + 1, diff_lines_a[i]);
            diff_print('+', j + 1, diff_lines_b[j]);
            i++; j++;
        }
    }

    if (differ) shell_set_status(1);
}

// ---- tee -----------------------------------------------------------------------

#define TEE_MAX_BYTES 131072
static char tee_buf[TEE_MAX_BYTES + 1];

void tee_command(int argc, char *argv[])
{
    bool append = false;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) append = true;
        else if (argv[i][0] != '-' && !file) file = argv[i];
        else { MU_ERR("Usage: tee [-a] <file>\n"); return; }
    }
    if (!file) { MU_ERR("Usage: tee [-a] <file>\n"); return; }

    const char *in = shell_stdin_file();
    if (!in) { MU_ERR("tee: nothing is piped in (use: command | tee file)\n"); return; }

    int size = avfs_get_filesize(in);
    if (size < 0 || size > TEE_MAX_BYTES) { MU_ERR("tee: input too large (max %d bytes)\n", TEE_MAX_BYTES); return; }
    if (size > 0 && avfs_get_content(in, tee_buf, sizeof(tee_buf)) != 0) { MU_ERR("tee: cannot read the piped input\n"); return; }
    if (size == 0) tee_buf[0] = '\0';

    print(tee_buf);

    int rc;
    if (append && avfs_file_exists(file)) rc = avfs_append_file(file, tee_buf, (uint32_t)size);
    else                                  rc = avfs_save_file(file, tee_buf, (uint32_t)size, true);
    if (rc != 0) MU_ERR("tee: cannot write '%s'\n", file);
}

// ---- seq, whoami, hostname ------------------------------------------------------

void seq_command(int argc, char *argv[])
{
    int first = 1, last = 0;
    bool ok = false;
    if (argc == 2)      { ok = mu_parse_int(argv[1], &last); }
    else if (argc == 3) { ok = mu_parse_int(argv[1], &first) && mu_parse_int(argv[2], &last); }

    if (!ok) { MU_ERR("Usage: seq [first] last\n"); return; }
    if ((last >= first ? last - first : first - last) >= 10000) { MU_ERR("seq: range too large (max 10000 numbers)\n"); return; }

    if (first <= last) for (int v = first; v <= last; v++) printr("%d\n", v);
    else               for (int v = first; v >= last; v--) printr("%d\n", v);
}

void whoami_command(int argc, char *argv[])
{
    (void)argc; (void)argv;
    printr("%s\n", users_current_name());
}

void hostname_command(int argc, char *argv[])
{
    (void)argc; (void)argv;
    print("radiumos\n");
}
