// shellvars.c - environment variables, aliases, history and the startup script
//
//   export NAME=value     set a variable (export alone, or env, lists them)
//   unset NAME...         remove variables
//   alias name=command    make `name` run `command` (alias alone lists them; commands only, no pipes)
//   unalias name | -a     remove one alias or all of them
//   history [N | -c]      show the last N commands, or clear the history
//
// Variables are expanded by the shell as $NAME / ${NAME} / $? before a command runs.
// PATH (default /disk/bin) is the colon-separated list searched for programs by bare name.
// /disk/.radiumrc runs once after boot; put export and alias lines there.

#include <stdint.h>
#include <stdbool.h>
#include "shellvars.h"
#include "bootpage.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../keyboard/keyboard.h"
#include "../shell/shsyntax.h"

#define KV_MAX        64
#define KV_NAME_MAX   32
#define KV_VALUE_MAX  256

typedef struct {
    bool used;
    char name[KV_NAME_MAX];
    char value[KV_VALUE_MAX];
} kv_t;

static kv_t g_env[KV_MAX];
static kv_t g_alias[KV_MAX];
static bool g_defaults_set = false;

#define SV_ERR(...) do { \
    terminal_setcolor(VGA_COLOR_LIGHT_RED); \
    printr(__VA_ARGS__); \
    terminal_setcolor(VGA_COLOR_WHITE); \
} while (0)

static kv_t *kv_find(kv_t *table, const char *name)
{
    for (int i = 0; i < KV_MAX; i++)
        if (table[i].used && strcmp(table[i].name, name) == 0) return &table[i];
    return NULL;
}

static int kv_set(kv_t *table, const char *name, const char *value)
{
    if (strlen(name) >= KV_NAME_MAX || strlen(value) >= KV_VALUE_MAX) return -1;

    kv_t *slot = kv_find(table, name);
    if (!slot) {
        for (int i = 0; i < KV_MAX && !slot; i++)
            if (!table[i].used) slot = &table[i];
        if (!slot) return -1;
        slot->used = true;
        strcpy(slot->name, name);
    }
    strcpy(slot->value, value);
    return 0;
}

static void ensure_defaults(void)
{
    if (g_defaults_set) return;
    g_defaults_set = true;
    kv_set(g_env, "PATH", "/disk/bin");
    kv_set(g_env, "HOME", "/home/user");
}

static bool is_name_start(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static bool is_name_char(char c)  { return is_name_start(c) || (c >= '0' && c <= '9'); }

static bool valid_env_name(const char *s)
{
    if (!is_name_start(*s)) return false;
    for (; *s; s++) if (!is_name_char(*s)) return false;
    return true;
}

static bool valid_alias_name(const char *s)
{
    if (*s == '\0') return false;
    for (; *s; s++) if (*s == ' ' || *s == '=' || *s == '|' || *s == '&' || *s == '"' || *s == '$') return false;
    return true;
}

const char *shell_getenv(const char *name)
{
    ensure_defaults();
    if (strcmp(name, "PWD") == 0) return avfs_get_cwd();
    kv_t *v = kv_find(g_env, name);
    return v ? v->value : bootpage_var(name);
}

int shell_setenv(const char *name, const char *value)
{
    ensure_defaults();
    if (!valid_env_name(name)) return -1;
    return kv_set(g_env, name, value);
}

const char *shell_alias_lookup(const char *name)
{
    kv_t *a = kv_find(g_alias, name);
    return a ? a->value : NULL;
}

const char *shell_alias_name_at(int n)
{
    int seen = 0;
    for (int i = 0; i < KV_MAX; i++) {
        if (!g_alias[i].used) continue;
        if (seen++ == n) return g_alias[i].name;
    }
    return NULL;
}

static size_t append_uint(char *out, size_t o, size_t cap, uint32_t v)
{
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n > 0 && o + 1 < cap) out[o++] = tmp[--n];
    return o;
}

void shell_expand_vars(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    size_t i = 0;

    while (in[i] && o + 1 < out_size) {
        if (in[i] == '\\' && in[i + 1] == '$') {
            out[o++] = '$';
            i += 2;
            continue;
        }
        if (in[i] != '$') {
            out[o++] = in[i++];
            continue;
        }

        if (in[i + 1] == '?') {
            int status = shell_last_status();
            if (status < 0) status = 0;
            o = append_uint(out, o, out_size, (uint32_t)status);
            i += 2;
            continue;
        }

        bool braced = (in[i + 1] == '{');
        size_t start = i + 1 + (braced ? 1 : 0);
        size_t n = 0;
        if (is_name_start(in[start])) {
            while (is_name_char(in[start + n])) n++;
        }
        size_t end = start + n + (braced ? 1 : 0);

        const char *value = NULL;
        if (n > 0 && n < KV_NAME_MAX && (!braced || in[start + n] == '}')) {
            char name[KV_NAME_MAX];
            memcpy(name, in + start, n);
            name[n] = '\0';
            value = shell_getenv(name);
        }

        if (!value) {
            // Not a defined variable: keep the text exactly as typed.
            out[o++] = '$';
            i++;
            continue;
        }

        for (size_t k = 0; value[k] && o + 1 < out_size; k++) out[o++] = value[k];
        i = end;
    }
    out[o] = '\0';
}

// Joins argv[start..] with single spaces.
static void join_args(int argc, char *argv[], int start, char *out, size_t size)
{
    size_t o = 0;
    for (int i = start; i < argc; i++) {
        if (i > start && o + 1 < size) out[o++] = ' ';
        for (const char *p = argv[i]; *p && o + 1 < size; p++) out[o++] = *p;
    }
    out[o] = '\0';
}

static void strip_quotes(char *s)
{
    size_t n = strlen(s);
    if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0]) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static void list_table(const kv_t *table, const char *prefix, const char *quote)
{
    bool any = false;
    for (int i = 0; i < KV_MAX; i++) {
        if (!table[i].used) continue;
        any = true;
        printr("%s%s=%s%s%s\n", prefix, table[i].name, quote, table[i].value, quote);
    }
    if (!any) print("(none)\n");
}

void env_command(int argc, char *argv[])
{
    (void)argc; (void)argv;
    ensure_defaults();
    list_table(g_env, "", "");
}

void export_command(int argc, char *argv[])
{
    ensure_defaults();
    if (argc == 1) {
        list_table(g_env, "export ", "");
        return;
    }

    char line[KV_NAME_MAX + KV_VALUE_MAX + 2];
    join_args(argc, argv, 1, line, sizeof(line));

    char *eq = strchr(line, '=');
    if (!eq) {
        if (!valid_env_name(line)) { SV_ERR("export: '%s': not a valid name\n", line); return; }
        if (!kv_find(g_env, line)) kv_set(g_env, line, "");
        return;
    }

    *eq = '\0';
    char *name = line;
    char *value = eq + 1;
    strip_quotes(value);

    if (!valid_env_name(name)) {
        SV_ERR("export: '%s': not a valid name (letters, digits and _, not starting with a digit)\n", name);
        return;
    }
    if (kv_set(g_env, name, value) != 0)
        SV_ERR("export: cannot set '%s': value too long or too many variables\n", name);
}

void unset_command(int argc, char *argv[])
{
    if (argc < 2) {
        SV_ERR("Usage: unset NAME...\n");
        return;
    }
    for (int i = 1; i < argc; i++) {
        kv_t *v = kv_find(g_env, argv[i]);
        if (v) v->used = false;
    }
}

void alias_command(int argc, char *argv[])
{
    if (argc == 1) {
        list_table(g_alias, "alias ", "'");
        return;
    }

    char line[KV_NAME_MAX + KV_VALUE_MAX + 2];
    join_args(argc, argv, 1, line, sizeof(line));

    char *eq = strchr(line, '=');
    if (!eq) {
        const char *v = shell_alias_lookup(line);
        if (v) printr("alias %s='%s'\n", line, v);
        else   SV_ERR("alias: %s: not found\n", line);
        return;
    }

    *eq = '\0';
    char *name = line;
    char *value = eq + 1;
    strip_quotes(value);

    if (!valid_alias_name(name)) {
        SV_ERR("alias: '%s': invalid name\n", name);
        return;
    }
    if (*value == '\0') {
        SV_ERR("alias: %s: give a command, e.g. alias ll=ls -l\n", name);
        return;
    }
    if (kv_set(g_alias, name, value) != 0)
        SV_ERR("alias: cannot define '%s': too long or too many aliases\n", name);
}

void unalias_command(int argc, char *argv[])
{
    if (argc < 2) {
        SV_ERR("Usage: unalias <name> | -a\n");
        return;
    }
    if (strcmp(argv[1], "-a") == 0) {
        for (int i = 0; i < KV_MAX; i++) g_alias[i].used = false;
        return;
    }
    for (int i = 1; i < argc; i++) {
        kv_t *a = kv_find(g_alias, argv[i]);
        if (a) a->used = false;
        else   SV_ERR("unalias: %s: not found\n", argv[i]);
    }
}

extern char   command_history[][COMMAND_BUFFER_SIZE];
extern size_t history_count;
extern int    current_history_index;

void history_command(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "-c") == 0) {
        history_count = 0;
        current_history_index = -1;
        return;
    }

    size_t first = 0;
    if (argc == 2) {
        size_t want = 0;
        for (const char *p = argv[1]; *p; p++) {
            if (*p < '0' || *p > '9') { SV_ERR("Usage: history [N | -c]\n"); return; }
            want = want * 10 + (size_t)(*p - '0');
        }
        if (want < history_count) first = history_count - want;
    } else if (argc > 2) {
        SV_ERR("Usage: history [N | -c]\n");
        return;
    }

    for (size_t i = first; i < history_count; i++)
        printr("%4d  %s\n", (int)(i + 1), command_history[i]);
}

#define RC_MAX_BYTES 32768

// Runs a script: the whole file goes through the shell syntax layer, so if/for/while blocks may span lines.
// Returns the number of command lines, or -1 if the file can't be read.
int shell_run_script(const char *path)
{
    static char buf[RC_MAX_BYTES + 1];

    if (!avfs_file_exists(path) || avfs_is_directory(path)) return -1;
    int size = avfs_get_filesize(path);
    if (size < 0 || size > RC_MAX_BYTES) return -1;
    if (size == 0) return 0;
    if (avfs_get_content(path, buf, sizeof(buf)) != 0) return -1;

    int ran = 0;
    for (const char *line = buf; *line; ) {
        const char *end = line;
        while (*end && *end != '\n') end++;
        const char *s = line;
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        if (s < end && *s != '#') ran++;
        line = *end ? end + 1 : end;
    }
    sh_execute(buf);
    return ran;
}

const char *shell_rc_path(void)
{
    if (avfs_file_exists("/disk/.radiumrc")) return "/disk/.radiumrc";
    if (avfs_file_exists("/home/user/.radiumrc")) return "/home/user/.radiumrc";
    return NULL;
}

int shell_run_rc(void)
{
    const char *path = shell_rc_path();
    return path ? shell_run_script(path) : -1;
}

// rrc [file]: run the startup script again (or another script of commands)
void rrc_command(int argc, char *argv[])
{
    if (argc > 2) {
        SV_ERR("Usage: rrc [file]\n");
        return;
    }

    const char *path = (argc == 2) ? argv[1] : shell_rc_path();
    if (!path) {
        SV_ERR("rrc: no startup script found (create /disk/.radiumrc)\n");
        return;
    }

    int ran = shell_run_script(path);
    if (ran < 0) SV_ERR("rrc: cannot read %s\n", path);
    else         printr("rrc: ran %d command%s from %s\n", ran, ran == 1 ? "" : "s", path);
}
