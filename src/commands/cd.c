#include "../Avfs/Avfs.h"
#include "../terminal/terminal.h"

/* ── Path resolver (mirrors ls/cat logic) ─────────────────────────── */
static int cd_resolve_path(const char* arg, char* out, int out_sz) {
    if (!arg || !arg[0]) return 0;

    /* /~/ alone = root */
    if (arg[0] == '/' && arg[1] == '~' && arg[2] == '/' && arg[3] == 0) {
        out[0] = '/'; out[1] = 0;
        return 1;
    }

    /* /~/dirname  or  /~/a/b  or  /~/a/b/ */
    if (arg[0] == '/' && arg[1] == '~' && arg[2] == '/') {
        const char* inner = arg + 3;
        int len = 0;
        while (inner[len]) len++;
        while (len > 0 && inner[len - 1] == '/') len--;
        if (len == 0) { out[0] = '/'; out[1] = 0; return 1; }
        if (len + 2 > out_sz) return 0;
        out[0] = '/';
        for (int i = 0; i < len; i++) out[i + 1] = inner[i];
        out[len + 1] = 0;
        return 1;
    }

    /* ~/  alone = root */
    if (arg[0] == '~' && arg[1] == '/' && arg[2] == 0) {
        out[0] = '/'; out[1] = 0;
        return 1;
    }

    /* ~/dirname  or  ~/a/b/ */
    if (arg[0] == '~' && arg[1] == '/') {
        const char* inner = arg + 2;
        int len = 0;
        while (inner[len]) len++;
        while (len > 0 && inner[len - 1] == '/') len--;
        if (len == 0) { out[0] = '/'; out[1] = 0; return 1; }
        if (len + 2 > out_sz) return 0;
        out[0] = '/';
        for (int i = 0; i < len; i++) out[i + 1] = inner[i];
        out[len + 1] = 0;
        return 1;
    }

    /* bare name or .. — relative to CWD */
    {
        const char* cwd = avfs_get_cwd();
        int cwd_len = 0;
        while (cwd[cwd_len]) cwd_len++;

        /* handle ".." — strip last component from cwd */
        if (arg[0] == '.' && arg[1] == '.' && arg[2] == 0) {
            if (cwd_len == 1 && cwd[0] == '/') {
                /* already at root */
                out[0] = '/'; out[1] = 0;
                return 1;
            }
            /* copy cwd and strip last component */
            for (int i = 0; i < cwd_len && i < out_sz - 1; i++) out[i] = cwd[i];
            out[cwd_len] = 0;
            /* find last slash */
            int last = 0;
            for (int i = 0; i < cwd_len; i++) if (out[i] == '/') last = i;
            if (last == 0) { out[0] = '/'; out[1] = 0; }
            else           { out[last] = 0; }
            return 1;
        }

        /* handle "." — stay in CWD */
        if (arg[0] == '.' && arg[1] == 0) {
            for (int i = 0; i < cwd_len && i < out_sz - 1; i++) out[i] = cwd[i];
            out[cwd_len] = 0;
            return 1;
        }

        int arg_len = 0;
        while (arg[arg_len]) arg_len++;
        while (arg_len > 0 && arg[arg_len - 1] == '/') arg_len--;
        if (arg_len == 0) return 0;

        /* build CWD + "/" + arg */
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

/* ── Entry point ──────────────────────────────────────────────────── */
void cd_command(int argc, char* argv[]) {
    /* no argument — go to root */
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        avfs_chdir("/");
        return;
    }

    if (argc > 2) {
        print("\nUsage:\n");
        print("  cd                   go to root\n");
        print("  cd /~/               go to root\n");
        print("  cd /~/dirname        go to absolute directory\n");
        print("  cd ~/dirname         go to absolute directory\n");
        print("  cd dirname           go to directory in CWD\n");
        print("  cd ..                go up one level\n");
        return;
    }

    char path[512];
    if (!cd_resolve_path(argv[1], path, sizeof(path))) {
        print("\ncd: invalid path '");
        print(argv[1]);
        print("'\n");
        return;
    }

    /* verify it exists and is a directory */
    if (!avfs_is_directory(path)) {
        print("\ncd: not a directory: '");
        print(path);
        print("'\n");
        return;
    }

    int r = avfs_chdir(path);
    if (r != 0) {
        print("\ncd: failed to change directory to '");
        print(path);
        print("'\n");
    }
}