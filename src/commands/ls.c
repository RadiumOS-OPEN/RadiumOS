
#include "../Avfs/Avfs.h"
#include "../terminal/terminal.h"
#include "../utility/utility.h"
#include "../keyboard/keyboard.h"

/* ── Constants ────────────────────────────────────────────────────── */
#define COL_RESET   0x07
#define COL_DIR     0x09
#define COL_EXEC    0x0A
#define COL_HIDDEN  0x08
#define COL_SIZE    0x0B
#define COL_HEADER  0x0E

#define LS_COLS      4
#define LS_COL_WIDTH 18
#define MAX_LS_FILES  256

/* ── File entry storage ───────────────────────────────────────────── */
// These must be declared globally so all helper functions can see them
static DirectoryEntry ls_entries[MAX_LS_FILES];
static char ls_full_paths[MAX_LS_FILES][AVFS_FILENAME_MAX];
static int  ls_count = 0;

/* ── Option flags ─────────────────────────────────────────────────── */
typedef struct {
    int all;
    int almost_all;
    int long_fmt;
    int human;
    int reverse;
    int size_sort;
    int no_sort;
    int one_col;
    int classify;
    int inode;
    int color;
    int recursive;
    int hidden_secure;
    const char* target;
} LsOpts;

/* ── Local string helpers ─────────────────────────────────────────── */
static int ls_str_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int ls_str_len(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}

static int ls_char_in(const char* s, char c) {
    while (*s) { if (*s == c) return 1; s++; }
    return 0;
}

static const char* ls_basename(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') last = p + 1;
    }
    return last;
}

/* ── Print helpers ────────────────────────────────────────────────── */
static void ls_print_int(int n) {
    if (n < 0) { print("-"); n = -n; }
    if (n == 0) { print("0"); return; }
    char buf[16]; int i = 15; buf[i] = 0;
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    print(&buf[i]);
}

static void ls_print_field(int sz, int width) {
    char buf[16]; int i = 15; buf[i] = 0;
    int tmp = sz; if (tmp == 0) { buf[--i] = '0'; }
    while (tmp > 0 && i > 0) { buf[--i] = '0' + (tmp % 10); tmp /= 10; }
    int len = 15 - i;
    for (int p = len; p < width; p++) print(" ");
    print(&buf[i]);
}

static void ls_print_human(int sz) {
    if (sz < 1024)           { ls_print_int(sz);               print("B  "); }
    else if (sz < 1024*1024) { ls_print_int(sz / 1024);        print("K  "); }
    else                     { ls_print_int(sz / (1024*1024));  print("M  "); }
}

static void ls_print_padded(const char* s, int width) {
    int l = ls_str_len(s);
    print(s);
    for (int i = l; i < width; i++) print(" ");
}

/* ── Summary Implementation ───────────────────────────────────────── */
static void ls_summary(const char* target) {
    int dirs = 0, files = 0, total = 0;
    for (int i = 0; i < ls_count; i++) {
        if (ls_entries[i].is_directory) dirs++;
        else { 
            files++; 
            total += ls_entries[i].size; 
        }
    }
    set_text_color(COL_HEADER);
    print("-- "); print(target); print(": ");
    reset_text_color();
    ls_print_int(files); print(" files, ");
    ls_print_int(dirs);  print(" dirs  (");
    ls_print_human(total); print(")\n");
}

/* ── Path syntax resolver ─────────────────────────────────────────── */
static int ls_resolve_target(const char* arg, char* out, int out_sz) {
    if (!arg || !arg[0]) return 0;
    if (ls_str_eq(arg, "/~/")) {
        out[0] = '/'; out[1] = 0;
        return 1;
    }
    if ((arg[0] == '/' && arg[1] == '~' && arg[2] == '/') || (arg[0] == '~' && arg[1] == '/')) {
        const char* inner = (arg[0] == '/') ? arg + 3 : arg + 2;
        int len = ls_str_len(inner);
        while (len > 0 && inner[len - 1] == '/') len--;
        if (len == 0) {
            out[0] = '/'; out[1] = 0;
            return 1;
        }
        if (len + 2 > out_sz) return 0;
        out[0] = '/';
        for (int i = 0; i < len; i++) out[i + 1] = inner[i];
        out[len + 1] = 0;
        return 1;
    }
    return 0;
}

/* ── Direct child check ───────────────────────────────────────────── */
static int ls_is_direct_child(const char* dir, const char* path) {
    if (path[0] == '/' && path[1] == 0) return 0;
    int dir_len = ls_str_len(dir);
    if (dir_len > 1 && dir[dir_len - 1] == '/') dir_len--;
    for (int i = 0; i < dir_len; i++) {
        if (path[i] != dir[i]) return 0;
    }
    const char* rest = path + dir_len;
    if (dir_len == 1 && dir[0] == '/') {
        if (!*rest) return 0;
        while (*rest) { if (*rest == '/') return 0; rest++; }
        return 1;
    }
    if (rest[0] != '/') return 0;
    rest++;
    if (!*rest) return 0;
    while (*rest) { if (*rest == '/') return 0; rest++; }
    return 1;
}

/* ── Security: Dynamic Key Generation & Persistence ──────────────── */
static void ls_generate_key(char* out, int len) {
    const char* charset = "0123456789ABCDEF";
    static uint32_t seed = 0x1776C14; 
    for (int i = 0; i < len; i++) {
        seed = (1103515245 * seed + 12345) & 0x7fffffff;
        out[i] = charset[seed % 16];
    }
    out[len] = '\0';
}

static int ls_authenticate_system(void) {
    const char* key_file = "/!/.key.txt/";
    char saved_key[32] = {0};
    char user_input[COMMAND_BUFFER_SIZE] = {0};

    if (!avfs_file_exists(key_file)) {
        ls_generate_key(saved_key, 16);
        avfs_create_file(key_file, 16);
        avfs_write_file(key_file, saved_key, 16, 0);
        
        set_text_color(COL_HEADER);
        print("[SECURITY] Generated unique system key.\n");
        print("[NOTICE] Key written to "); print(key_file); print("\n");
        reset_text_color();
    } else {
        avfs_read_file(key_file, saved_key, 16, 0);
    }

    set_text_color(COL_HEADER);
    print("[SECURE] Enter Radium Passphrase: ");
    reset_text_color();
    
    keyboard_input_secure(user_input);
    print("\n");

    return ls_str_eq(user_input, saved_key);
}

/* ── Executable detection ─────────────────────────────────────────── */
static int ls_is_exec(const char* name) {
    int l = ls_str_len(name);
    if (l >= 4 && ls_str_eq(name + l - 4, ".rsh"))  return 1;
    if (l >= 4 && ls_str_eq(name + l - 4, ".bin"))  return 1;
    if (l >= 5 && ls_str_eq(name + l - 5, ".rash")) return 1;
    if (!ls_char_in(name, '.')) return 1;
    return 0;
}

static LsOpts ls_default_opts(void) {
    LsOpts o = {0};
    o.color = 1;
    return o;
}

/* ── Collect logic ────────────────────────────────────────────────── */
static void ls_collect(const LsOpts* o, const char* target_dir) {
    ls_count = 0;
    char path_buf[AVFS_FILENAME_MAX];

    for (int i = 0; ls_count < MAX_LS_FILES; i++) {
        path_buf[0] = 0;
        uint32_t size_tmp = 0;
        if (avfs_get_file_info(i, path_buf, &size_tmp) < 0) break;
        if (!path_buf[0]) continue;

        if (!ls_is_direct_child(target_dir, path_buf)) continue;

        const char* base = ls_basename(path_buf);
        int hidden = (base[0] == '.');

        if (hidden && !o->all && !o->almost_all) continue;
        if (hidden && o->almost_all && (ls_str_eq(base, ".") || ls_str_eq(base, ".."))) continue;

        DirectoryEntry* e = &ls_entries[ls_count];
        strncpy(e->name, base, 31);
        e->size = size_tmp;
        e->is_directory = avfs_is_directory(path_buf);
        e->index = i;

        strncpy(ls_full_paths[ls_count], path_buf, AVFS_FILENAME_MAX - 1);
        ls_count++;
    }
}

static void ls_collect_recursive(const LsOpts* o, const char* dir) {
    int saved = ls_count;
    ls_collect(o, dir);
    int added = ls_count;
    for (int i = saved; i < added && ls_count < MAX_LS_FILES; i++) {
        if (ls_entries[i].is_directory) {
            ls_collect_recursive(o, ls_full_paths[i]);
        }
    }
}

/* ── Sorting ──────────────────────────────────────────────────────── */
static void ls_sort(const LsOpts* o) {
    if (o->no_sort) return;
    for (int i = 1; i < ls_count; i++) {
        DirectoryEntry tmp_e = ls_entries[i];
        char tmp_p[AVFS_FILENAME_MAX];
        strncpy(tmp_p, ls_full_paths[i], AVFS_FILENAME_MAX);
        
        int j = i - 1;
        while (j >= 0) {
            int swap = 0;
            if (o->size_sort) {
                swap = ls_entries[j].size < tmp_e.size;
            } else {
                const char *s1 = ls_entries[j].name, *s2 = tmp_e.name;
                while (*s1 && (*s1 == *s2)) { s1++; s2++; }
                swap = (*(unsigned char*)s1 > *(unsigned char*)s2);
            }
            if (!swap) break;
            ls_entries[j + 1] = ls_entries[j];
            strncpy(ls_full_paths[j + 1], ls_full_paths[j], AVFS_FILENAME_MAX);
            j--;
        }
        ls_entries[j + 1] = tmp_e;
        strncpy(ls_full_paths[j + 1], tmp_p, AVFS_FILENAME_MAX);
    }
    if (o->reverse) {
        for (int i = 0, j = ls_count - 1; i < j; i++, j--) {
            DirectoryEntry te = ls_entries[i]; ls_entries[i] = ls_entries[j]; ls_entries[j] = te;
            char tp[AVFS_FILENAME_MAX];
            strncpy(tp, ls_full_paths[i], AVFS_FILENAME_MAX);
            strncpy(ls_full_paths[i], ls_full_paths[j], AVFS_FILENAME_MAX);
            strncpy(ls_full_paths[j], tp, AVFS_FILENAME_MAX);
        }
    }
}

/* ── Output ───────────────────────────────────────────────────────── */
static void ls_print_long(const LsOpts* o) {
    set_text_color(COL_HEADER);
    print("IDX  TYPE      SIZE  NAME\n");
    print("---  ----  --------  ----\n");
    reset_text_color();

    for (int i = 0; i < ls_count; i++) {
        DirectoryEntry* e = &ls_entries[i];
        ls_print_field(o->inode ? e->index : i, 3); print("  ");
        
        if (e->is_directory) { set_text_color(COL_DIR); print("dir   "); }
        else if (ls_is_exec(e->name)) { set_text_color(COL_EXEC); print("exec  "); }
        else { reset_text_color(); print("file  "); }
        reset_text_color();

        if (e->is_directory) print("       -  ");
        else {
            set_text_color(COL_SIZE);
            if (o->human) ls_print_human(e->size);
            else ls_print_field(e->size, 8);
            print("  ");
            reset_text_color();
        }

        if (o->color) {
            if (e->name[0] == '.') set_text_color(COL_HIDDEN);
            else if (e->is_directory) set_text_color(COL_DIR);
            else if (ls_is_exec(e->name)) set_text_color(COL_EXEC);
            else reset_text_color();
        }
        print(e->name);
        reset_text_color();
        if (o->classify) { if (e->is_directory) print("/"); else if (ls_is_exec(e->name)) print("*"); }
        print("\n");
    }
}

static void ls_print_short(const LsOpts* o) {
    int col = 0;
    for (int i = 0; i < ls_count; i++) {
        DirectoryEntry* e = &ls_entries[i];
        if (o->color) {
            if (e->name[0] == '.') set_text_color(COL_HIDDEN);
            else if (e->is_directory) set_text_color(COL_DIR);
            else if (ls_is_exec(e->name)) set_text_color(COL_EXEC);
        }
        if (o->one_col) {
            print(e->name); reset_text_color();
            if (o->classify) { if (e->is_directory) print("/"); else if (ls_is_exec(e->name)) print("*"); }
            print("\n");
        } else {
            ls_print_padded(e->name, LS_COL_WIDTH); reset_text_color();
            if (++col >= LS_COLS) { print("\n"); col = 0; }
        }
    }
    if (!o->one_col && col > 0) print("\n");
}

/* ── Argument Parser ──────────────────────────────────────────────── */
static LsOpts ls_parse_opts(int argc, char* argv[]) {
    LsOpts o = ls_default_opts();
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!a || !a[0]) continue;
        if ((a[0] == '/' && a[1] == '~') || a[0] == '~') { o.target = a; continue; }
        if (a[0] != '-') continue;

        if (ls_str_eq(a, "--all")) o.all = 1;
        else if (ls_str_eq(a, "--hidden")) o.hidden_secure = 1;
        else if (ls_str_eq(a, "--long")) o.long_fmt = 1;
        else {
            for (int k = 1; a[k]; k++) {
                switch (a[k]) {
                    case 'a': o.all = 1; break;
                    case 'l': o.long_fmt = 1; break;
                    case 'h': o.hidden_secure = 1; break;
                    case 'R': o.recursive = 1; break;
                    case 'S': o.size_sort = 1; break;
                    case 'r': o.reverse = 1; break;
                    case '1': o.one_col = 1; break;
                }
            }
        }
    }
    return o;
}

/* ── Main Command Entry ───────────────────────────────────────────── */
void ls_command(int argc, char* argv[]) {
    print("\n");
    LsOpts o = ls_parse_opts(argc, argv);

    if (o.hidden_secure) {
        if (!ls_authenticate_system()) {
            set_text_color(COL_HIDDEN);
            print("Access Denied.\n\n");
            reset_text_color();
            return;
        }
        o.all = 1;
        print("\n");
    }

    char target[AVFS_FILENAME_MAX];
    if (o.target) {
        if (!ls_resolve_target(o.target, target, AVFS_FILENAME_MAX)) return;
    } else {
        const char* cwd = avfs_get_cwd();
        strncpy(target, cwd, AVFS_FILENAME_MAX - 1);
        if (target[0] == 0) { target[0] = '/'; target[1] = 0; }
    }

    if (!avfs_is_directory(target)) {
        print("ls: cannot access "); print(target); print(": Not a directory\n");
        return;
    }

    if (o.recursive) ls_collect_recursive(&o, target);
    else ls_collect(&o, target);

    if (ls_count == 0) { print("(empty directory)\n"); return; }

    ls_sort(&o);

    ls_print_long(&o);
    

    ls_summary(target);
    print("\n");
}

