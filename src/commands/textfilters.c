// textfilters.c - grep, head, tail, wc, sort, uniq
//
// Each filter reads the file named on its command line, or - when none is
// given - the data piped in by the shell ("ls | grep txt").
// Arguments are split on spaces by the shell, so a grep pattern is one word.
//
//   grep [-i] [-v] [-n] [-c] <text> [file]   lines containing text (plain substring)
//   head [-n N | -N] [file]                  first N lines (default 10)
//   tail [-n N | -N] [file]                  last N lines (default 10)
//   wc   [-l] [-w] [-c] [file]               lines / words / bytes
//   sort [-r] [-n] [-u] [file]               sorted lines (-n numeric, -u unique)
//   uniq [-c] [file]                         collapse adjacent duplicate lines

#include <stdint.h>
#include <stdbool.h>
#include "textfilters.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../keyboard/keyboard.h"

#define TF_MAX_BYTES 131072
#define TF_MAX_LINES 8192

static char  tf_buf[TF_MAX_BYTES + 1];
static char *tf_lines[TF_MAX_LINES];

#define TF_ERR(...) do { \
    terminal_setcolor(VGA_COLOR_LIGHT_RED); \
    printr(__VA_ARGS__); \
    terminal_setcolor(VGA_COLOR_WHITE); \
} while (0)

// Loads `file` (or the piped input when NULL) into tf_buf. Returns its length, or -1 after printing why.
static int tf_load(const char *cmd, const char *file)
{
    const char *path = file ? file : shell_stdin_file();
    if (!path) {
        TF_ERR("%s: no input (name a file or pipe something in)\n", cmd);
        return -1;
    }
    if (avfs_is_directory(path)) {
        TF_ERR("%s: %s: is a directory\n", cmd, path);
        return -1;
    }

    int size = avfs_get_filesize(path);
    if (size < 0) {
        TF_ERR("%s: %s: no such file or directory\n", cmd, path);
        return -1;
    }
    if (size > TF_MAX_BYTES) {
        TF_ERR("%s: %s: input too large (max %d bytes)\n", cmd, path, TF_MAX_BYTES);
        return -1;
    }
    if (size == 0) {
        tf_buf[0] = '\0';
        return 0;
    }
    if (avfs_get_content(path, tf_buf, sizeof(tf_buf)) != 0) {
        TF_ERR("%s: %s: read error\n", cmd, path);
        return -1;
    }
    tf_buf[size] = '\0';
    return size;
}

// Cuts tf_buf into lines in place. Returns the line count, or -1 after printing why.
static int tf_split(const char *cmd, int len)
{
    int n = 0;
    char *p = tf_buf;
    char *end = tf_buf + len;

    while (p < end) {
        if (n == TF_MAX_LINES) {
            TF_ERR("%s: too many lines (max %d)\n", cmd, TF_MAX_LINES);
            return -1;
        }
        char *line = p;
        while (p < end && *p != '\n') p++;
        char *line_end = p;
        if (p < end) p++;                                   // skip the '\n'
        if (line_end > line && line_end[-1] == '\r') line_end--;
        *line_end = '\0';
        tf_lines[n++] = line;
    }
    return n;
}

static void tf_print_line(const char *line)
{
    print(line);
    print("\n");
}

static char tf_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool tf_contains(const char *line, const char *pat, bool icase)
{
    size_t pl = strlen(pat);
    if (pl == 0) return true;
    size_t ll = strlen(line);
    if (pl > ll) return false;

    for (size_t i = 0; i + pl <= ll; i++) {
        size_t j = 0;
        while (j < pl) {
            char a = line[i + j], b = pat[j];
            if (icase) { a = tf_lower(a); b = tf_lower(b); }
            if (a != b) break;
            j++;
        }
        if (j == pl) return true;
    }
    return false;
}

static bool tf_is_number(const char *s)
{
    if (!*s) return false;
    for (; *s; s++) if (*s < '0' || *s > '9') return false;
    return true;
}

static int tf_number(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    else if (*s == '+') s++;
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

// Parses -n N / -N / -nN into *count; the remaining operand (file) goes to *file.
// Returns false (after printing usage) on bad arguments.
static bool tf_parse_count(const char *cmd, int argc, char *argv[], int *count, const char **file)
{
    *count = 10;
    *file = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] != '\0') {
            const char *num = NULL;
            if (a[1] == 'n') {
                if (a[2] != '\0') num = a + 2;
                else if (i + 1 < argc) num = argv[++i];
            } else {
                num = a + 1;
            }
            if (!num || !tf_is_number(num)) {
                TF_ERR("Usage: %s [-n N] [file]\n", cmd);
                return false;
            }
            *count = tf_number(num);
        } else if (!*file) {
            *file = a;
        } else {
            TF_ERR("%s: too many arguments\n", cmd);
            return false;
        }
    }
    return true;
}

void grep_command(int argc, char *argv[])
{
    bool icase = false, invert = false, number = false, count_only = false;
    const char *pattern = NULL;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!pattern && a[0] == '-' && a[1] != '\0') {
            for (const char *p = a + 1; *p; p++) {
                if      (*p == 'i') icase = true;
                else if (*p == 'v') invert = true;
                else if (*p == 'n') number = true;
                else if (*p == 'c') count_only = true;
                else {
                    TF_ERR("grep: unknown flag '-%c'\n", *p);
                    return;
                }
            }
        } else if (!pattern) {
            pattern = a;
        } else if (!file) {
            file = a;
        } else {
            TF_ERR("grep: too many arguments (patterns are one word; use grep <word> [file])\n");
            return;
        }
    }

    if (!pattern) {
        TF_ERR("Usage: grep [-i] [-v] [-n] [-c] <text> [file]\n");
        return;
    }

    int len = tf_load("grep", file);
    if (len < 0) return;
    int n = tf_split("grep", len);
    if (n < 0) return;

    int matched = 0;
    for (int i = 0; i < n; i++) {
        if (tf_contains(tf_lines[i], pattern, icase) == invert) continue;
        matched++;
        if (count_only) continue;
        if (number) printr("%d:", i + 1);
        tf_print_line(tf_lines[i]);
    }
    if (count_only) printr("%d\n", matched);
}

void head_command(int argc, char *argv[])
{
    int count;
    const char *file;
    if (!tf_parse_count("head", argc, argv, &count, &file)) return;

    int len = tf_load("head", file);
    if (len < 0) return;
    int n = tf_split("head", len);
    if (n < 0) return;

    for (int i = 0; i < n && i < count; i++) tf_print_line(tf_lines[i]);
}

void tail_command(int argc, char *argv[])
{
    int count;
    const char *file;
    if (!tf_parse_count("tail", argc, argv, &count, &file)) return;

    int len = tf_load("tail", file);
    if (len < 0) return;
    int n = tf_split("tail", len);
    if (n < 0) return;

    for (int i = (n > count ? n - count : 0); i < n; i++) tf_print_line(tf_lines[i]);
}

void wc_command(int argc, char *argv[])
{
    bool want_l = false, want_w = false, want_c = false;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] != '\0') {
            for (const char *p = a + 1; *p; p++) {
                if      (*p == 'l') want_l = true;
                else if (*p == 'w') want_w = true;
                else if (*p == 'c') want_c = true;
                else {
                    TF_ERR("wc: unknown flag '-%c'\n", *p);
                    return;
                }
            }
        } else if (!file) {
            file = a;
        } else {
            TF_ERR("wc: too many arguments\n");
            return;
        }
    }
    if (!want_l && !want_w && !want_c) want_l = want_w = want_c = true;

    int len = tf_load("wc", file);
    if (len < 0) return;

    int lines = 0, words = 0;
    bool in_word = false;
    for (int i = 0; i < len; i++) {
        char c = tf_buf[i];
        if (c == '\n') lines++;
        bool space = (c == ' ' || c == '\n' || c == '\t' || c == '\r');
        if (!space && !in_word) words++;
        in_word = !space;
    }

    if (want_l) printr("%7d", lines);
    if (want_w) printr("%7d", words);
    if (want_c) printr("%7d", len);
    if (file) printr(" %s", file);
    print("\n");
}

static int sort_compare(const char *a, const char *b, bool numeric, bool reverse)
{
    int r = 0;
    if (numeric) {
        int x = tf_number(a), y = tf_number(b);
        if (x != y) r = x < y ? -1 : 1;
    }
    if (r == 0) r = strcmp(a, b);
    return reverse ? -r : r;
}

void sort_command(int argc, char *argv[])
{
    bool reverse = false, numeric = false, unique = false;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] != '\0') {
            for (const char *p = a + 1; *p; p++) {
                if      (*p == 'r') reverse = true;
                else if (*p == 'n') numeric = true;
                else if (*p == 'u') unique  = true;
                else {
                    TF_ERR("sort: unknown flag '-%c'\n", *p);
                    return;
                }
            }
        } else if (!file) {
            file = a;
        } else {
            TF_ERR("sort: too many arguments\n");
            return;
        }
    }

    int len = tf_load("sort", file);
    if (len < 0) return;
    int n = tf_split("sort", len);
    if (n < 0) return;

    // Shell sort: no recursion and no extra memory.
    for (int gap = n / 2; gap > 0; gap /= 2) {
        for (int i = gap; i < n; i++) {
            char *item = tf_lines[i];
            int j = i;
            while (j >= gap && sort_compare(tf_lines[j - gap], item, numeric, reverse) > 0) {
                tf_lines[j] = tf_lines[j - gap];
                j -= gap;
            }
            tf_lines[j] = item;
        }
    }

    for (int i = 0; i < n; i++) {
        if (unique && i > 0 && strcmp(tf_lines[i], tf_lines[i - 1]) == 0) continue;
        tf_print_line(tf_lines[i]);
    }
}

void uniq_command(int argc, char *argv[])
{
    bool counts = false;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] != '\0') {
            for (const char *p = a + 1; *p; p++) {
                if (*p == 'c') counts = true;
                else {
                    TF_ERR("uniq: unknown flag '-%c'\n", *p);
                    return;
                }
            }
        } else if (!file) {
            file = a;
        } else {
            TF_ERR("uniq: too many arguments\n");
            return;
        }
    }

    int len = tf_load("uniq", file);
    if (len < 0) return;
    int n = tf_split("uniq", len);
    if (n < 0) return;

    int i = 0;
    while (i < n) {
        int run = 1;
        while (i + run < n && strcmp(tf_lines[i], tf_lines[i + run]) == 0) run++;
        if (counts) printr("%7d ", run);
        tf_print_line(tf_lines[i]);
        i += run;
    }
}
