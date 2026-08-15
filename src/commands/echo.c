// echo.c
#include "../terminal/terminal.h"
#include "../Avfs/Avfs.h"
#include "../utility/utility.h"
#include "echo.h"

/* ── Local string helpers ─────────────────────────────────────────── */
static int echo_str_len(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}

static int echo_str_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void echo_str_copy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void echo_str_cat(char* dst, const char* src, int max) {
    int i = 0;
    while (dst[i]) i++;
    int j = 0;
    while (src[j] && i < max - 1) { dst[i++] = src[j++]; }
    dst[i] = 0;
}

/* ── Path resolver ────────────────────────────────────────────────── */
/*
   ABSOLUTE (requires /~/ or ~/):
     /~/filename          -> /filename
     /~/dir/filename      -> /dir/filename
     ~/filename           -> /filename
     ~/dir/filename       -> /dir/filename

   RELATIVE (bare name, no /~/ or ~/):
     filename             -> CWD/filename

   Returns 2 if absolute syntax was used.
   Returns 1 if relative (CWD) was used.
   Returns 0 on error.
*/
static int echo_resolve_path(const char* arg, char* out, int out_sz) {
    if (!arg || !arg[0]) return 0;

    /* /~/ alone = root (absolute) */
    if (arg[0] == '/' && arg[1] == '~' && arg[2] == '/' && arg[3] == 0) {
        out[0] = '/'; out[1] = 0;
        return 2;
    }

    /* /~/path (absolute) */
    if (arg[0] == '/' && arg[1] == '~' && arg[2] == '/') {
        const char* inner = arg + 3;
        int len = echo_str_len(inner);
        while (len > 0 && inner[len - 1] == '/') len--;
        if (len == 0) { out[0] = '/'; out[1] = 0; return 2; }
        if (len + 2 > out_sz) return 0;
        out[0] = '/';
        for (int i = 0; i < len; i++) out[i + 1] = inner[i];
        out[len + 1] = 0;
        return 2;
    }

    /* ~/ alone = root (absolute) */
    if (arg[0] == '~' && arg[1] == '/' && arg[2] == 0) {
        out[0] = '/'; out[1] = 0;
        return 2;
    }

    /* ~/path (absolute) */
    if (arg[0] == '~' && arg[1] == '/') {
        const char* inner = arg + 2;
        int len = echo_str_len(inner);
        while (len > 0 && inner[len - 1] == '/') len--;
        if (len == 0) { out[0] = '/'; out[1] = 0; return 2; }
        if (len + 2 > out_sz) return 0;
        out[0] = '/';
        for (int i = 0; i < len; i++) out[i + 1] = inner[i];
        out[len + 1] = 0;
        return 2;
    }

    /* bare name — relative to CWD (returns 1, not 2) */
    {
        const char* cwd = avfs_get_cwd();
        int cwd_len = echo_str_len(cwd);
        int arg_len = echo_str_len(arg);
        while (arg_len > 0 && arg[arg_len - 1] == '/') arg_len--;
        if (arg_len == 0) return 0;

        if (cwd_len == 1 && cwd[0] == '/') {
            if (1 + arg_len + 1 > out_sz) return 0;
            out[0] = '/';
            for (int i = 0; i < arg_len; i++) out[i + 1] = arg[i];
            out[arg_len + 1] = 0;
        } else {
            if (cwd_len + 1 + arg_len + 1 > out_sz) return 0;
            for (int i = 0; i < cwd_len; i++) out[i] = cwd[i];
            out[cwd_len] = '/';
            for (int i = 0; i < arg_len; i++) out[cwd_len + 1 + i] = arg[i];
            out[cwd_len + 1 + arg_len] = 0;
        }
        return 1;
    }
}

/* ── Escape sequence processor ────────────────────────────────────── */
static void echo_process_escapes(const char* src, char* dst, int dst_sz) {
    int i = 0, j = 0;
    while (src[i] && j < dst_sz - 1) {
        if (src[i] == '\\' && src[i + 1]) {
            i++;
            switch (src[i]) {
                case 'n':  dst[j++] = '\n'; break;
                case 't':  dst[j++] = '\t'; break;
                case 'r':  dst[j++] = '\r'; break;
                case 'a':  dst[j++] = '\a'; break;
                case 'b':  dst[j++] = '\b'; break;
                case '\\': dst[j++] = '\\'; break;
                case '0':
                    dst[j] = 0;
                    return;
                default:
                    dst[j++] = '\\';
                    if (j < dst_sz - 1) dst[j++] = src[i];
                    break;
            }
        } else {
            dst[j++] = src[i];
        }
        i++;
    }
    dst[j] = 0;
}

/* ── Write to file helper ─────────────────────────────────────────── */
static void echo_write_file(const char* path, const char* content,
                            int append) {
    int content_len = echo_str_len(content);

    if (append && avfs_file_exists(path)) {
        char newline = '\n';
        avfs_append_file(path, &newline, 1);
        int r = avfs_append_file(path, content, content_len);
        if (r != 0) {
            print("echo: failed to append to '");
            print(path);
            print("'\n");
        }
        return;
    }

    if (avfs_file_exists(path)) {
        avfs_remove_file(path);
    }

    int r = avfs_create_file(path, content_len);
    if (r != 0) {
        print("echo: failed to create '");
        print(path);
        print("'\n");
        return;
    }

    r = avfs_write_file(path, content, content_len, 0);
    if (r != 0) {
        print("echo: failed to write to '");
        print(path);
        print("'\n");
        avfs_remove_file(path);
    }
}

/* ── Entry point ──────────────────────────────────────────────────── */
#define ECHO_BUF 4096

void echo_command(int argc, char* argv[]) {
    if (argc < 2) {
        print("\n");
        return;
    }

    /* ── parse flags ─────────────────────────────────────────────── */
    int escape     = 0;
    int no_escape  = 0;
    int no_newline = 0;
    int arg_start  = 1;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (a[0] != '-' || !a[1]) break;

        int is_flag = 1;
        for (int k = 1; a[k]; k++) {
            switch (a[k]) {
                case 'e': escape     = 1; no_escape = 0; break;
                case 'E': no_escape  = 1; escape    = 0; break;
                case 'n': no_newline = 1;                break;
                default:  is_flag    = 0;                break;
            }
        }
        if (!is_flag) break;
        arg_start = i + 1;
    }

    /* ── find redirection operator ───────────────────────────────── */
    int redirect_index = -1;
    int append_mode    = 0;

    for (int i = arg_start; i < argc; i++) {
        if (echo_str_eq(argv[i], ">>")) {
            redirect_index = i;
            append_mode    = 1;
            break;
        }
        if (echo_str_eq(argv[i], ">")) {
            redirect_index = i;
            append_mode    = 0;
            break;
        }
    }

    /* ── build output text ───────────────────────────────────────── */
    char raw[ECHO_BUF];
    raw[0] = 0;
    int pos = 0;

    int end_index = (redirect_index != -1) ? redirect_index : argc;

    for (int i = arg_start; i < end_index; i++) {
        int arg_len = echo_str_len(argv[i]);
        if (pos + arg_len + 2 >= ECHO_BUF) {
            print("echo: text too long\n");
            return;
        }
        echo_str_cat(raw, argv[i], ECHO_BUF);
        pos += arg_len;
        if (i < end_index - 1) {
            raw[pos++] = ' ';
            raw[pos]   = 0;
        }
    }

    /* ── process escape sequences ────────────────────────────────── */
    char processed[ECHO_BUF];
    if (escape && !no_escape) {
        echo_process_escapes(raw, processed, ECHO_BUF);
    } else {
        echo_str_copy(processed, raw, ECHO_BUF);
    }

    /* ── handle redirection ──────────────────────────────────────── */
    if (redirect_index != -1) {
        if (redirect_index + 1 >= argc) {
            print("echo: no filename after '");
            print(append_mode ? ">>" : ">");
            print("'\n");
            return;
        }

        const char* raw_path = argv[redirect_index + 1];
        char path[512];
        int resolved = echo_resolve_path(raw_path, path, sizeof(path));

        if (!resolved) {
            print("echo: invalid path '");
            print(raw_path);
            print("'\n");
            return;
        }

        /* enforce /~/ or ~/ for absolute paths outside CWD */
        if (resolved == 1) {
            /* relative path — only allowed if no directory separator */
            /* i.e. bare filename only, not subdir/file */
            int has_slash = 0;
            for (int i = 0; raw_path[i]; i++) {
                if (raw_path[i] == '/') { has_slash = 1; break; }
            }
            if (has_slash) {
                print("echo: use /~/ or ~/ for absolute paths\n");
                print("  e.g.  echo text > /~/dir/file.txt\n");
                print("        echo text > ~/dir/file.txt\n");
                return;
            }
        }

        if (avfs_is_directory(path)) {
            print("echo: '");
            print(path);
            print("' is a directory\n");
            return;
        }

        echo_write_file(path, processed, append_mode);

    } else {
        /* ── print to terminal ───────────────────────────────────── */
        print("\n");
        print(processed);
        if (!no_newline) print("\n");
    }
}