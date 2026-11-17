// rsd.c - RadiumOS service manager, a small systemd work-alike
//
// Units are text files named NAME.svc (FAT keeps 8.3 names, so NAME is at most 8 characters) in /disk/units
// (or /etc/units when there is no disk). Format, as in systemd:
//
//   [Unit]      Description=  After=  Before=  Requires=  Wants=     (space separated unit names)
//   [Service]   Type=oneshot|simple   ExecStartPre=  ExecStart=  ExecStop=   (any shell command line)
//               RemainAfterExit=yes   Restart=no|on-failure|always   RestartSec=2s
//   [Timer]     OnBootSec=20s   OnUnitActiveSec=30s              (run ExecStart later / repeatedly)
//   [Install]   WantedBy=multi-user.target                       (the target that pulls the unit in when enabled)
//
// Targets are the boot stages, in order: sysinit, basic, multi-user, graphical (rescue is a separate, minimal
// stage that only runs sysinit and the units wanted by rescue.target). Boot reaches the default target
// (systemctl get-default / set-default) unless the kernel command line picks another (radium.target=,
// rescue, safe; see kernel/cmdline.h). `systemctl isolate TARGET` moves a running system between them.
// reboot and poweroff stop everything in reverse order first (rsd_shutdown).
//
// A command runs inside the shell, so "processes" are shell command lines: oneshot units run once and may
// stay "active (exited)"; simple units stay "active (running)" and are re-run by Restart= or a timer. Output
// of every run goes to the journal (journalctl). Enabled units (the list in enabled.txt next to the unit
// files) are started at boot in After=/Before=/Requires= order. Timers and restarts fire from rsd_poll(), which the
// shell calls while it waits for keys, so they never interrupt a command that is running.

#include <stdint.h>
#include <stdbool.h>
#include "rsd.h"
#include "reboot.h"
#include "sysutils.h"
#include "../kernel/bootprofile.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../keyboard/keyboard.h"
#include "../timers/timer.h"
#include "../kernel/cmdline.h"

#define MAX_UNITS    24
#define MAX_DEPS     4
#define NAME_LEN     16
#define JOURNAL_MAX  300
#define RUN_OUT_FILE "/.rsdout"
#define UNIT_TEXT_MAX 3000

typedef enum { ST_INACTIVE, ST_ACTIVE, ST_WAITING, ST_FAILED } unit_state_t;

// Boot stages, lowest first.
enum { TGT_RESCUE = 0, TGT_SYSINIT, TGT_BASIC, TGT_MULTI, TGT_GRAPHICAL, TGT_COUNT };
static const struct { const char *name; const char *desc; } TARGETS[TGT_COUNT] = {
    { "rescue",     "Rescue Mode" },
    { "sysinit",    "System Initialization" },
    { "basic",      "Basic System" },
    { "multi-user", "Multi-User System" },
    { "graphical",  "Graphical Interface" },
};

typedef struct {
    bool used, seen;
    char name[NAME_LEN];
    char path[48];
    char desc[64];
    char after[MAX_DEPS][NAME_LEN];    int n_after;
    char before[MAX_DEPS][NAME_LEN];   int n_before;
    char requires[MAX_DEPS][NAME_LEN]; int n_requires;
    char wants[MAX_DEPS][NAME_LEN];    int n_wants;
    uint8_t stage;                     // target that pulls the unit in (TGT_*)
    bool oneshot, remain;
    uint8_t restart;                   // 0 no, 1 on-failure, 2 always
    uint32_t restart_ms, boot_ms, active_ms;
    char exec_pre[128], exec_start[160], exec_stop[128];
    bool enabled;

    unit_state_t state;
    bool starting;
    int  last_status;
    uint32_t since_ms, next_run, interval_ms, runs, restarts, took_ms, finished_ms, seq;
} unit_t;

static unit_t g_units[MAX_UNITS];
static bool   g_booted = false;
static int    g_target = TGT_MULTI;     // the target the system is running at
static uint32_t g_start_seq = 0;        // orders starts so shutdown can run them backwards
static bool   g_quiet = false, g_verbose = false;
static uint32_t g_boot_start_ms = 0, g_boot_end_ms = 0;

typedef struct { uint32_t ms; char unit[NAME_LEN]; char text[112]; uint8_t level; } journal_t;
static journal_t g_journal[JOURNAL_MAX];
static int g_journal_count = 0;

#define COL(c) terminal_setcolor(c)
#define WHITE  VGA_COLOR_WHITE
#define ERR(...) do { COL(VGA_COLOR_LIGHT_RED); printr(__VA_ARGS__); COL(WHITE); } while (0)

// ---- small helpers -----------------------------------------------------------------------------

static void copy_str(char *dst, size_t size, const char *src)
{
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool iequals(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return *a == *b;
}

static void pad(const char *s, size_t width)
{
    print(s);
    for (size_t n = strlen(s); n < width; n++) print(" ");
}

static const char *units_dir(void) { return avfs_is_directory("/disk") ? "/disk/units" : "/etc/units"; }

static void enabled_path(char *out, size_t size)
{
    copy_str(out, size, units_dir());
    strcat(out, "/enabled.txt");
}

// "motd", "motd.svc" and "motd.service" all name the same unit.
static void normalize_name(const char *in, char *out)
{
    copy_str(out, NAME_LEN, in);
    size_t n = strlen(out);
    if (n > 8 && strcmp(out + n - 8, ".service") == 0) out[n - 8] = '\0';
    else if (n > 4 && strcmp(out + n - 4, ".svc") == 0) out[n - 4] = '\0';
}

static unit_t *find_unit(const char *name)
{
    char n[NAME_LEN];
    normalize_name(name, n);
    for (int i = 0; i < MAX_UNITS; i++)
        if (g_units[i].used && strcmp(g_units[i].name, n) == 0) return &g_units[i];
    return NULL;
}

static void print_uptime_stamp(uint32_t ms)
{
    printr("[%5u.%03u]", ms / 1000, ms % 1000);
}

// ---- journal -----------------------------------------------------------------------------------

static void journal_add(const char *unit, const char *text, uint8_t level)
{
    if (g_journal_count == JOURNAL_MAX) {
        memmove(&g_journal[0], &g_journal[1], sizeof(journal_t) * (JOURNAL_MAX - 1));
        g_journal_count--;
    }
    journal_t *e = &g_journal[g_journal_count++];
    e->ms = get_time_ms();
    e->level = level;
    copy_str(e->unit, sizeof(e->unit), unit);
    copy_str(e->text, sizeof(e->text), text);
}

// ---- unit files --------------------------------------------------------------------------------

static void split_deps(char list[][NAME_LEN], int *count, const char *value)
{
    *count = 0;
    const char *p = value;
    while (*p && *count < MAX_DEPS) {
        while (*p == ' ' || *p == '\t') p++;
        char word[NAME_LEN];
        size_t n = 0;
        while (*p && *p != ' ' && *p != '\t') { if (n + 1 < sizeof(word)) word[n++] = *p; p++; }
        word[n] = '\0';
        if (n == 0) break;
        size_t len = strlen(word);
        if (len > 7 && strcmp(word + len - 7, ".target") == 0) continue;   // targets are not units here
        normalize_name(word, list[*count]);
        (*count)++;
    }
}

static uint32_t parse_time_ms(const char *v)
{
    uint32_t n = 0;
    while (*v >= '0' && *v <= '9') n = n * 10 + (uint32_t)(*v++ - '0');
    while (*v == ' ') v++;
    if (strcmp(v, "ms") == 0) return n;
    if (strcmp(v, "min") == 0) return n * 60000;
    return n * 1000;
}

static bool yes(const char *v) { return iequals(v, "yes") || iequals(v, "true") || iequals(v, "1") || iequals(v, "on"); }

// "multi-user", "multi-user.target", "default", "3" ... -> TGT_*, or -1.
static int target_from_name(const char *in)
{
    char n[NAME_LEN];
    copy_str(n, sizeof(n), in);
    size_t len = strlen(n);
    if (len > 7 && strcmp(n + len - 7, ".target") == 0) n[len - 7] = '\0';

    for (int i = 0; i < TGT_COUNT; i++) if (iequals(n, TARGETS[i].name)) return i;
    if (iequals(n, "default") || iequals(n, "3")) return TGT_MULTI;
    if (iequals(n, "single") || iequals(n, "emergency") || iequals(n, "1")) return TGT_RESCUE;
    if (iequals(n, "5")) return TGT_GRAPHICAL;
    return -1;
}

static int target_from_list(const char *value)
{
    const char *p = value;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        char word[NAME_LEN];
        size_t n = 0;
        while (*p && *p != ' ' && *p != '\t') { if (n + 1 < sizeof(word)) word[n++] = *p; p++; }
        word[n] = '\0';
        int t = n ? target_from_name(word) : -1;
        if (t >= 0) return t;
    }
    return -1;
}

static void parse_unit(unit_t *u, char *text)
{
    u->desc[0] = '\0';
    u->n_after = u->n_before = u->n_requires = u->n_wants = 0;
    u->stage = TGT_MULTI;
    u->oneshot = true; u->remain = false; u->restart = 0;
    u->restart_ms = 1000; u->boot_ms = u->active_ms = 0;
    u->exec_pre[0] = u->exec_start[0] = u->exec_stop[0] = '\0';

    char *line = text;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n') end++;
        bool last = (*end == '\0');
        *end = '\0';
        size_t n = strlen(line);
        if (n > 0 && line[n - 1] == '\r') line[--n] = '\0';

        const char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s && *s != '#' && *s != ';' && *s != '[') {
            char key[32];
            size_t k = 0;
            while (*s && *s != '=' && k + 1 < sizeof(key)) key[k++] = *s++;
            while (k > 0 && (key[k - 1] == ' ' || key[k - 1] == '\t')) k--;
            key[k] = '\0';
            if (*s == '=') {
                s++;
                while (*s == ' ' || *s == '\t') s++;
                if (iequals(key, "Description"))         copy_str(u->desc, sizeof(u->desc), s);
                else if (iequals(key, "After"))          split_deps(u->after, &u->n_after, s);
                else if (iequals(key, "Before"))         split_deps(u->before, &u->n_before, s);
                else if (iequals(key, "WantedBy"))       { int t = target_from_list(s); if (t >= 0) u->stage = (uint8_t)t; }
                else if (iequals(key, "Requires"))       split_deps(u->requires, &u->n_requires, s);
                else if (iequals(key, "Wants"))          split_deps(u->wants, &u->n_wants, s);
                else if (iequals(key, "Type"))           u->oneshot = !iequals(s, "simple") && !iequals(s, "forking") && !iequals(s, "notify");
                else if (iequals(key, "ExecStartPre"))   copy_str(u->exec_pre, sizeof(u->exec_pre), s);
                else if (iequals(key, "ExecStart"))      copy_str(u->exec_start, sizeof(u->exec_start), s);
                else if (iequals(key, "ExecStop"))       copy_str(u->exec_stop, sizeof(u->exec_stop), s);
                else if (iequals(key, "RemainAfterExit")) u->remain = yes(s);
                else if (iequals(key, "Restart"))        u->restart = iequals(s, "always") ? 2 : (iequals(s, "on-failure") ? 1 : 0);
                else if (iequals(key, "RestartSec"))     u->restart_ms = parse_time_ms(s);
                else if (iequals(key, "OnBootSec"))      u->boot_ms = parse_time_ms(s);
                else if (iequals(key, "OnUnitActiveSec")) u->active_ms = parse_time_ms(s);
            }
        }
        if (last) break;
        line = end + 1;
    }
    if (u->desc[0] == '\0') copy_str(u->desc, sizeof(u->desc), u->name);
}

static bool is_timer(const unit_t *u) { return u->boot_ms > 0 || u->active_ms > 0; }

static void load_enabled_flags(void)
{
    static char list[1024];
    char path[64];
    enabled_path(path, sizeof(path));
    list[0] = '\0';
    if (avfs_file_exists(path)) avfs_get_content(path, list, sizeof(list));

    for (int i = 0; i < MAX_UNITS; i++) g_units[i].enabled = false;
    char *line = list;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n') end++;
        bool last = (*end == '\0');
        *end = '\0';
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';
        unit_t *u = n ? find_unit(line) : NULL;
        if (u) u->enabled = true;
        if (last) break;
        line = end + 1;
    }
}

static void save_enabled_flags(void)
{
    static char list[1024];
    list[0] = '\0';
    for (int i = 0; i < MAX_UNITS; i++) {
        if (!g_units[i].used || !g_units[i].enabled) continue;
        if (strlen(list) + strlen(g_units[i].name) + 2 >= sizeof(list)) break;
        strcat(list, g_units[i].name);
        strcat(list, "\n");
    }
    char path[64];
    enabled_path(path, sizeof(path));
    avfs_save_file(path, list, (uint32_t)strlen(list), true);
}

// Re-reads every unit file, keeping the run state of units that are still there.
static int load_units(void)
{
    static DirectoryEntry entries[64];
    static char text[UNIT_TEXT_MAX + 1];
    const char *dir = units_dir();

    for (int i = 0; i < MAX_UNITS; i++) g_units[i].seen = false;
    int count = avfs_is_directory(dir) ? avfs_list_directory(dir, entries, 64) : 0;

    for (int i = 0; i < count; i++) {
        size_t n = strlen(entries[i].name);
        if (entries[i].is_directory || n < 5 || strcmp(entries[i].name + n - 4, ".svc") != 0) continue;

        char name[NAME_LEN];
        normalize_name(entries[i].name, name);

        char path[48];
        copy_str(path, sizeof(path), dir);
        strcat(path, "/");
        strcat(path, entries[i].name);
        int size = avfs_get_filesize(path);
        if (size <= 0 || size > UNIT_TEXT_MAX || avfs_get_content(path, text, sizeof(text)) != 0) continue;

        unit_t *u = find_unit(name);
        if (!u) {
            for (int k = 0; k < MAX_UNITS && !u; k++) if (!g_units[k].used) u = &g_units[k];
            if (!u) break;
            memset(u, 0, sizeof(*u));
            u->used = true;
            copy_str(u->name, sizeof(u->name), name);
        }
        copy_str(u->path, sizeof(u->path), path);
        u->seen = true;
        parse_unit(u, text);
    }

    int total = 0;
    for (int i = 0; i < MAX_UNITS; i++) {
        if (g_units[i].used && !g_units[i].seen && g_units[i].state == ST_INACTIVE) g_units[i].used = false;
        if (g_units[i].used) total++;
    }
    load_enabled_flags();
    return total;
}

// ---- stock units (written once, never overwritten, so edits survive) -----------------------------------

static const struct { const char *file; const char *text; } STOCK_UNITS[] = {
    { "motd.svc",
      "[Unit]\n"
      "Description=Message of the day\n"
      "\n"
      "[Service]\n"
      "Type=oneshot\n"
      "ExecStart=echo Welcome to RadiumOS, $USER. Up $UPTIME, memory $MEM_INFO.\n"
      "\n"
      "[Install]\n"
      "WantedBy=default.target\n" },
    { "network.svc",
      "[Unit]\n"
      "Description=Network link status\n"
      "After=motd\n"
      "\n"
      "[Service]\n"
      "Type=oneshot\n"
      "RemainAfterExit=yes\n"
      "ExecStart=echo link: $NET, MAC $MAC\n"
      "ExecStop=echo link left up\n"
      "\n"
      "[Install]\n"
      "WantedBy=default.target\n" },
    { "pulse.svc",
      "[Unit]\n"
      "Description=Pulse logger (a timer: runs every 30 seconds)\n"
      "After=network\n"
      "\n"
      "[Service]\n"
      "Type=simple\n"
      "ExecStart=echo alive: up $UPTIME, memory $MEM_INFO, disk $DISK_INFO\n"
      "\n"
      "[Timer]\n"
      "OnBootSec=20s\n"
      "OnUnitActiveSec=30s\n"
      "\n"
      "[Install]\n"
      "WantedBy=default.target\n" },
    { "diskchk.svc",
      "[Unit]\n"
      "Description=Check the /disk filesystem\n"
      "\n"
      "[Service]\n"
      "Type=oneshot\n"
      "ExecStart=echo /disk: $DISK_INFO\n"
      "\n"
      "[Install]\n"
      "WantedBy=sysinit.target\n" },
    { "hostname.svc",
      "[Unit]\n"
      "Description=Set the system hostname\n"
      "After=diskchk\n"
      "\n"
      "[Service]\n"
      "Type=oneshot\n"
      "ExecStart=echo hostname: $HOSTNAME\n"
      "\n"
      "[Install]\n"
      "WantedBy=sysinit.target\n" },
    { "clock.svc",
      "[Unit]\n"
      "Description=Read the real-time clock\n"
      "\n"
      "[Service]\n"
      "Type=oneshot\n"
      "ExecStart=date\n"
      "\n"
      "[Install]\n"
      "WantedBy=basic.target\n" },
    { "updchk.svc",
      "[Unit]\n"
      "Description=Look for a pending RadiumOS update\n"
      "After=network\n"
      "\n"
      "[Service]\n"
      "Type=oneshot\n"
      "ExecStart=update status\n"
      "\n"
      "[Install]\n"
      "WantedBy=multi-user.target\n" },
    { "sample.svc",
      "[Unit]\n"
      "Description=Sample memory, disk and task counts for sysstat (a timer: every 10 seconds)\n"
      "\n"
      "[Service]\n"
      "Type=simple\n"
      "ExecStart=sysstat sample\n"
      "\n"
      "[Timer]\n"
      "OnBootSec=5s\n"
      "OnUnitActiveSec=10s\n"
      "\n"
      "[Install]\n"
      "WantedBy=default.target\n" },
    { "health.svc",
      "[Unit]\n"
      "Description=System health check (a timer: every 2 minutes, logs only problems)\n"
      "After=network\n"
      "\n"
      "[Service]\n"
      "Type=simple\n"
      "ExecStart=syshealth -q\n"
      "\n"
      "[Timer]\n"
      "OnBootSec=30s\n"
      "OnUnitActiveSec=120s\n"
      "\n"
      "[Install]\n"
      "WantedBy=default.target\n" },
    { "intchk.svc",
      "[Unit]\n"
      "Description=Check boot-critical files against the integrity record\n"
      "After=diskchk\n"
      "\n"
      "[Service]\n"
      "Type=oneshot\n"
      "ExecStart=integrity check -q\n"
      "\n"
      "[Install]\n"
      "WantedBy=multi-user.target\n" },
};
#define STOCK_COUNT ((int)(sizeof(STOCK_UNITS) / sizeof(STOCK_UNITS[0])))

static bool enabled_list_has(const char *list, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = list; *p; ) {
        const char *end = p;
        while (*end && *end != '\n') end++;
        size_t len = (size_t)(end - p);
        if (len > 0 && p[len - 1] == '\r') len--;
        if (len == n && strncmp(p, name, n) == 0) return true;
        p = *end ? end + 1 : end;
    }
    return false;
}

// Writes the stock unit files that are missing and enables each one it just created, so a disk made by an
// older build picks up new stock units without losing the user's choices for the old ones.
static void install_stock_units(void)
{
    static char enabled[1024];
    const char *dir = units_dir();
    if (!avfs_is_directory(dir)) avfs_create_dir(dir);

    char list_path[64];
    enabled_path(list_path, sizeof(list_path));
    bool had_list = avfs_file_exists(list_path);
    enabled[0] = '\0';
    if (had_list) avfs_get_content(list_path, enabled, sizeof(enabled));

    bool changed = false;
    char path[64];
    for (int i = 0; i < STOCK_COUNT; i++) {
        copy_str(path, sizeof(path), dir);
        strcat(path, "/");
        strcat(path, STOCK_UNITS[i].file);
        if (avfs_file_exists(path)) continue;
        avfs_save_file(path, STOCK_UNITS[i].text, (uint32_t)strlen(STOCK_UNITS[i].text), true);

        char name[NAME_LEN];
        normalize_name(STOCK_UNITS[i].file, name);
        if (!enabled_list_has(enabled, name) && strlen(enabled) + strlen(name) + 2 < sizeof(enabled)) {
            strcat(enabled, name);
            strcat(enabled, "\n");
            changed = true;
        }
    }
    if (changed || !had_list) avfs_save_file(list_path, enabled, (uint32_t)strlen(enabled), true);
}

// ---- running units ------------------------------------------------------------------------------

// Runs a command line with its output captured into the journal. Returns the exit status.
static int run_command(const char *unit, const char *cmd)
{
    static char out[2048];
    int saved = shell_last_status();

    if (avfs_file_exists(RUN_OUT_FILE)) avfs_remove_file(RUN_OUT_FILE);
    bool captured = avfs_create_file(RUN_OUT_FILE, 0) == 0 && terminal_begin_capture(RUN_OUT_FILE) == 0;

    terminal_error_flag_take();
    execute_command_no_history(cmd);
    int status = shell_last_status();
    if (captured) terminal_end_capture();
    if (terminal_error_flag_take() && status == 0) status = 1;

    if (captured) {
        out[0] = '\0';
        int size = avfs_get_filesize(RUN_OUT_FILE);
        if (size > 0 && size < (int)sizeof(out)) avfs_get_content(RUN_OUT_FILE, out, sizeof(out));
        avfs_remove_file(RUN_OUT_FILE);

        char *line = out;
        int lines = 0;
        while (*line && lines < 10) {
            char *end = line;
            while (*end && *end != '\n') end++;
            bool last = (*end == '\0');
            *end = '\0';
            size_t n = strlen(line);
            while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';
            if (n > 0) { journal_add(unit, line, status != 0); lines++; }
            if (last) break;
            line = end + 1;
        }
    }
    shell_set_status(saved);
    return status;
}

// One execution of the unit's commands. keep_waiting leaves a timer unit in the waiting state afterwards.
static int execute_unit(unit_t *u, bool keep_waiting)
{
    uint32_t t0 = get_time_ms();
    int status = 0;

    if (u->exec_pre[0]) status = run_command(u->name, u->exec_pre);
    if (status == 0 && u->exec_start[0]) status = run_command(u->name, u->exec_start);

    u->runs++;
    u->last_status = status;
    u->took_ms = get_time_ms() - t0;
    u->finished_ms = get_time_ms();

    if (status != 0) {
        u->state = ST_FAILED;
        char msg[96];
        strcpy(msg, "Failed with result 'exit-code' (status ");
        char num[8];
        itoa(status, num, 10);
        strcat(msg, num);
        strcat(msg, ").");
        journal_add(u->name, msg, 1);
        if (u->restart != 0 && u->restarts < 5) u->next_run = get_time_ms() + u->restart_ms;
        else u->next_run = 0;
        return status;
    }

    if (keep_waiting) u->state = ST_WAITING;
    else u->state = (u->oneshot && !u->remain) ? ST_INACTIVE : ST_ACTIVE;
    u->since_ms = get_time_ms();
    return 0;
}

static int start_unit(unit_t *u);

static int start_dependencies(unit_t *u)
{
    for (int i = 0; i < u->n_requires; i++) {
        unit_t *dep = find_unit(u->requires[i]);
        if (!dep || start_unit(dep) != 0) {
            char msg[96];
            strcpy(msg, "Dependency failed: ");
            strcat(msg, u->requires[i]);
            journal_add(u->name, msg, 1);
            u->state = ST_FAILED;
            u->last_status = 1;
            return -1;
        }
    }
    for (int i = 0; i < u->n_wants; i++) {
        unit_t *dep = find_unit(u->wants[i]);
        if (dep) start_unit(dep);
    }
    return 0;
}

static int start_unit(unit_t *u)
{
    if (u->state == ST_ACTIVE || u->state == ST_WAITING) return 0;
    if (u->starting) return -1;   // dependency loop
    u->starting = true;
    int result = 0;

    if (start_dependencies(u) != 0) { u->starting = false; return -1; }
    if (u->exec_start[0] == '\0' && u->exec_pre[0] == '\0') {
        journal_add(u->name, "No ExecStart= in the unit file.", 1);
        u->state = ST_FAILED;
        u->starting = false;
        return -1;
    }

    uint32_t now = get_time_ms();
    u->interval_ms = 0;
    u->next_run = 0;

    if (is_timer(u)) {
        u->state = ST_WAITING;
        u->since_ms = now;
        u->interval_ms = u->active_ms;
        u->next_run = now + (u->boot_ms ? u->boot_ms : u->active_ms);
        journal_add(u->name, "Timer armed.", 0);
    } else {
        result = execute_unit(u, false);
        if (result == 0) {
            char msg[96];
            strcpy(msg, "Started ");
            strncat(msg, u->desc, 80);
            journal_add(u->name, msg, 0);
            if (!u->oneshot && u->restart == 2) { u->interval_ms = u->restart_ms; u->next_run = get_time_ms() + u->restart_ms; }
        }
    }
    if (result == 0) u->seq = ++g_start_seq;
    u->starting = false;
    return result;
}

static void stop_unit(unit_t *u)
{
    if (u->state == ST_INACTIVE) return;

    for (int i = 0; i < MAX_UNITS; i++) {   // units that require this one go down with it
        unit_t *d = &g_units[i];
        if (!d->used || d == u || (d->state != ST_ACTIVE && d->state != ST_WAITING)) continue;
        for (int k = 0; k < d->n_requires; k++) if (strcmp(d->requires[k], u->name) == 0) { stop_unit(d); break; }
    }

    if ((u->state == ST_ACTIVE || u->state == ST_WAITING) && u->exec_stop[0]) run_command(u->name, u->exec_stop);
    u->state = ST_INACTIVE;
    u->next_run = 0;
    u->interval_ms = 0;
    u->seq = 0;
    char msg[96];
    strcpy(msg, "Stopped ");
    strncat(msg, u->desc, 80);
    journal_add(u->name, msg, 0);
}

void rsd_poll(void)
{
    static bool busy = false;
    static uint32_t last_check = 0;
    if (busy || !g_booted) return;

    uint32_t now = get_time_ms();
    if (now - last_check < 250) return;
    last_check = now;

    busy = true;
    for (int i = 0; i < MAX_UNITS; i++) {
        unit_t *u = &g_units[i];
        if (!u->used || u->next_run == 0 || (int32_t)(now - u->next_run) < 0) continue;

        u->next_run = 0;
        if (u->state == ST_FAILED) {
            u->restarts++;
            if (execute_unit(u, false) == 0) {
                journal_add(u->name, "Restarted after failure.", 0);
                if (u->restart == 2) { u->interval_ms = u->restart_ms; u->next_run = get_time_ms() + u->restart_ms; }
            }
        } else {
            bool timer = is_timer(u);
            if (execute_unit(u, timer) == 0) {
                if (u->interval_ms) u->next_run = get_time_ms() + u->interval_ms;
                else if (timer) u->state = (u->oneshot && !u->remain) ? ST_INACTIVE : ST_ACTIVE;
            }
        }
        now = get_time_ms();
    }
    busy = false;
}

// ---- targets and boot ------------------------------------------------------------------------------

static void show_journal_for(const char *unit, int max_lines);

static bool unit_before(const unit_t *a, const unit_t *b)   // true if a must start before b
{
    for (int i = 0; i < b->n_after; i++) if (strcmp(b->after[i], a->name) == 0) return true;
    for (int i = 0; i < b->n_requires; i++) if (strcmp(b->requires[i], a->name) == 0) return true;
    for (int i = 0; i < b->n_wants; i++) if (strcmp(b->wants[i], a->name) == 0) return true;
    for (int i = 0; i < a->n_before; i++) if (strcmp(a->before[i], b->name) == 0) return true;
    return false;
}

// Is a unit wanted by `stage` part of a system running at `target`?
static bool stage_in_target(int stage, int target)
{
    if (target == TGT_RESCUE) return stage == TGT_SYSINIT || stage == TGT_RESCUE;
    return stage >= TGT_SYSINIT && stage <= target;
}

static void default_target_path(char *out, size_t size)
{
    copy_str(out, size, units_dir());
    strcat(out, "/default.txt");
}

static int default_target(void)
{
    char path[64], text[24];
    default_target_path(path, sizeof(path));
    if (!avfs_file_exists(path) || avfs_get_content(path, text, sizeof(text)) != 0) return TGT_MULTI;
    size_t n = strlen(text);
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r' || text[n - 1] == ' ')) text[--n] = '\0';
    int t = target_from_name(text);
    return t >= 0 ? t : TGT_MULTI;
}

static void status_line(bool ok, const char *text, const char *tail)
{
    COL(WHITE);
    print(ok ? "[ " : "[");
    COL(ok ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_RED);
    print(ok ? " OK " : "FAILED");
    COL(WHITE);
    print(ok ? " ] " : "] ");
    print(text);
    print(tail);
    print("\n");
}

static void announce_start(unit_t *u, int rc)
{
    if (rc == 0) {
        if (!g_quiet) status_line(true, is_timer(u) ? "Scheduled " : "Started ", u->desc);
        if (g_verbose) show_journal_for(u->name, 3);
        return;
    }
    COL(WHITE); print("[");
    COL(VGA_COLOR_LIGHT_RED); print("FAILED");
    COL(WHITE); print("] Failed to start ");
    print(u->desc);
    COL(VGA_COLOR_LIGHT_GREY); print("  (systemctl status "); print(u->name); print(")");
    COL(WHITE); print("\n");
}

// Orders `list` so each unit comes after the ones that must start before it; a loop keeps the rest in file order.
static void sort_by_deps(unit_t **list, int n)
{
    unit_t *sorted[MAX_UNITS];
    bool placed[MAX_UNITS] = { false };
    int m = 0;
    while (m < n) {
        bool progress = false;
        for (int i = 0; i < n && m < n; i++) {
            if (placed[i]) continue;
            bool ready = true;
            for (int k = 0; k < n; k++) if (k != i && !placed[k] && unit_before(list[k], list[i])) { ready = false; break; }
            if (!ready) continue;
            placed[i] = true;
            sorted[m++] = list[i];
            progress = true;
        }
        if (!progress) for (int i = 0; i < n; i++) if (!placed[i]) { placed[i] = true; sorted[m++] = list[i]; }
    }
    for (int i = 0; i < n; i++) list[i] = sorted[i];
}

// Starts the enabled units wanted by `stage`, then reports the target. Units already started (by a dependency,
// or earlier in this boot) are only announced during boot itself.
static void run_stage(int stage)
{
    unit_t *list[MAX_UNITS];
    int n = 0;
    for (int i = 0; i < MAX_UNITS; i++)
        if (g_units[i].used && g_units[i].enabled && g_units[i].stage == stage) list[n++] = &g_units[i];
    sort_by_deps(list, n);

    for (int i = 0; i < n; i++) {
        unit_t *u = list[i];
        if (u->seq && g_booted) continue;
        int rc = u->seq ? (u->state == ST_FAILED ? -1 : 0) : start_unit(u);
        announce_start(u, rc);
    }

    char tail[48];
    strcpy(tail, TARGETS[stage].desc);
    strcat(tail, ".");
    if (!g_quiet) status_line(true, "Reached target ", tail);
    char msg[64];
    strcpy(msg, "Reached target ");
    strcat(msg, tail);
    journal_add("rsd", msg, 0);
}

static void run_target(int target)
{
    if (target == TGT_RESCUE) {
        run_stage(TGT_SYSINIT);
        run_stage(TGT_RESCUE);
    } else {
        for (int stage = TGT_SYSINIT; stage <= target; stage++) run_stage(stage);
    }
    g_target = target;
}

void rsd_boot(void)
{
    install_stock_units();
    load_units();

    g_quiet = cmdline_has("quiet");
    g_verbose = cmdline_has("verbose") || cmdline_has("debug");

    int target = default_target();
    char value[NAME_LEN];
    if ((cmdline_value("radium.target", value, sizeof(value)) || cmdline_value("systemd.unit", value, sizeof(value))) &&
        target_from_name(value) >= 0)
        target = target_from_name(value);
    if (cmdline_has("rescue") || cmdline_has("single") || cmdline_has("emergency") || cmdline_has("safe")) target = TGT_RESCUE;

    g_boot_start_ms = get_time_ms();
    journal_add("rsd", "RadiumOS service manager starting.", 0);
    if (target != TGT_MULTI) {
        COL(VGA_COLOR_LIGHT_CYAN);
        printr("Booting to the %s target.\n", TARGETS[target].desc);
        COL(WHITE);
    }

    run_target(target);

    if (target == TGT_RESCUE) {
        COL(VGA_COLOR_LIGHT_BROWN);
        print("Rescue mode: only the early boot units ran. `systemctl isolate multi-user` continues the boot.\n");
        COL(WHITE);
    }

    g_boot_end_ms = get_time_ms();
    g_booted = true;
}

// Stops, newest first, every running unit that is not part of `target` (all of them when target < 0).
static void stop_outside(int target, bool announce)
{
    for (;;) {
        unit_t *last = NULL;
        for (int i = 0; i < MAX_UNITS; i++) {
            unit_t *u = &g_units[i];
            if (!u->used || (u->state != ST_ACTIVE && u->state != ST_WAITING)) continue;
            if (target >= 0 && stage_in_target(u->stage, target)) continue;
            if (!last || u->seq > last->seq) last = u;
        }
        if (!last) break;
        char desc[64];
        copy_str(desc, sizeof(desc), last->desc);
        stop_unit(last);
        if (announce) status_line(true, "Stopped ", desc);
    }
}

static void stopped_target(int t)
{
    char tail[48];
    strcpy(tail, TARGETS[t].desc);
    strcat(tail, ".");
    status_line(true, "Stopped target ", tail);
}

void rsd_shutdown(const char *action)
{
    if (!g_booted) return;
    g_booted = false;   // no more timers or restarts
    journal_add("rsd", "Shutting down.", 0);

    stop_outside(-1, true);
    if (g_target == TGT_RESCUE) {
        stopped_target(TGT_RESCUE);
        stopped_target(TGT_SYSINIT);
    } else {
        for (int t = g_target; t >= TGT_SYSINIT; t--) stopped_target(t);
    }
    char tail[48];
    strcpy(tail, "System ");
    strcat(tail, action);
    strcat(tail, ".");
    status_line(true, "Reached target ", tail);
}

// Moves a running system to `target`: stops what does not belong to it, starts what does.
static void isolate_target(int target)
{
    if (!g_booted) { ERR("systemctl: the service manager has not finished booting\n"); return; }
    stop_outside(target, true);
    run_target(target);
    if (target == TGT_RESCUE) {
        COL(VGA_COLOR_LIGHT_BROWN);
        print("Rescue mode: services are stopped. `systemctl isolate multi-user` brings them back.\n");
        COL(WHITE);
    }
}

// ---- views for other programs (sysmon) ------------------------------------------------------------

int rsd_unit_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_UNITS; i++) if (g_units[i].used) n++;
    return n;
}

bool rsd_unit_view(int n, rsd_unit_view_t *out)
{
    for (int i = 0; i < MAX_UNITS; i++) {
        const unit_t *u = &g_units[i];
        if (!u->used || n-- > 0) continue;
        copy_str(out->name, sizeof(out->name), u->name);
        copy_str(out->desc, sizeof(out->desc), u->desc);
        copy_str(out->target, sizeof(out->target), TARGETS[u->stage].name);
        out->state = u->state == ST_ACTIVE ? "active" : (u->state == ST_WAITING ? "waiting" : (u->state == ST_FAILED ? "failed" : "inactive"));
        out->enabled = u->enabled;
        out->failed = u->state == ST_FAILED;
        out->running = u->state == ST_ACTIVE || u->state == ST_WAITING;
        out->runs = u->runs;
        return true;
    }
    return false;
}

const char *rsd_current_target(void) { return TARGETS[g_target].name; }

bool rsd_journal_view(int back, uint32_t *ms, char *unit, size_t unit_size, char *text, size_t text_size, int *level)
{
    int index = g_journal_count - 1 - back;
    if (index < 0 || index >= g_journal_count) return false;
    *ms = g_journal[index].ms;
    copy_str(unit, unit_size, g_journal[index].unit);
    copy_str(text, text_size, g_journal[index].text);
    *level = g_journal[index].level;
    return true;
}

// ---- systemctl -----------------------------------------------------------------------------------

static void state_words(const unit_t *u, const char **active, const char **sub)
{
    switch (u->state) {
    case ST_ACTIVE:   *active = "active";   *sub = (u->oneshot && u->remain) ? "exited" : "running"; break;
    case ST_WAITING:  *active = "active";   *sub = "waiting"; break;
    case ST_FAILED:   *active = "failed";   *sub = "failed"; break;
    default:          *active = "inactive"; *sub = "dead"; break;
    }
}

static uint8_t state_color(const unit_t *u)
{
    return u->state == ST_FAILED ? VGA_COLOR_LIGHT_RED : (u->state == ST_INACTIVE ? VGA_COLOR_LIGHT_GREY : VGA_COLOR_LIGHT_GREEN);
}

static void cmd_list_units(void)
{
    COL(VGA_COLOR_LIGHT_CYAN);
    pad("UNIT", 14); pad("LOAD", 8); pad("ACTIVE", 10); pad("SUB", 9); print("DESCRIPTION\n");
    int shown = 0, failed = 0;
    for (int i = 0; i < MAX_UNITS; i++) {
        unit_t *u = &g_units[i];
        if (!u->used) continue;
        const char *active, *sub;
        state_words(u, &active, &sub);
        COL(state_color(u));
        print(u->state == ST_FAILED ? "x " : (u->state == ST_INACTIVE ? "  " : "* "));
        COL(WHITE);
        char name[NAME_LEN + 4];
        strcpy(name, u->name);
        strcat(name, ".svc");
        pad(name, 14); pad("loaded", 8);
        COL(state_color(u)); pad(active, 10); pad(sub, 9);
        COL(WHITE); print(u->desc); print("\n");
        shown++;
        if (u->state == ST_FAILED) failed++;
    }
    COL(VGA_COLOR_LIGHT_GREY);
    printr("%d loaded unit%s, %d failed. Directory: %s\n", shown, shown == 1 ? "" : "s", failed, units_dir());
    COL(WHITE);
}

static void cmd_list_unit_files(void)
{
    COL(VGA_COLOR_LIGHT_CYAN);
    pad("UNIT FILE", 14); pad("STATE", 10); print("WANTED BY\n");
    for (int i = 0; i < MAX_UNITS; i++) {
        unit_t *u = &g_units[i];
        if (!u->used) continue;
        COL(WHITE);
        char name[NAME_LEN + 4];
        strcpy(name, u->name);
        strcat(name, ".svc");
        pad(name, 14);
        COL(u->enabled ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_GREY);
        pad(u->enabled ? "enabled" : "disabled", 10);
        COL(WHITE);
        print(TARGETS[u->stage].name);
        print(".target\n");
    }
    COL(WHITE);
}

static void cmd_list_targets(void)
{
    COL(VGA_COLOR_LIGHT_CYAN);
    pad("UNIT", 20); pad("LOAD", 8); pad("ACTIVE", 10); print("DESCRIPTION\n");
    for (int t = TGT_SYSINIT; t < TGT_COUNT; t++) {
        bool up = g_booted && stage_in_target(t, g_target);
        COL(up ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_GREY);
        print(up ? "* " : "  ");
        COL(WHITE);
        char name[NAME_LEN + 8];
        strcpy(name, TARGETS[t].name);
        strcat(name, ".target");
        pad(name, 18); pad("loaded", 8);
        COL(up ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_GREY);
        pad(up ? "active" : "inactive", 10);
        COL(WHITE);
        print(TARGETS[t].desc);
        print("\n");
    }
    bool rescue = g_booted && g_target == TGT_RESCUE;
    COL(rescue ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_GREY);
    print(rescue ? "* " : "  ");
    COL(WHITE);
    pad("rescue.target", 18); pad("loaded", 8);
    COL(rescue ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_GREY);
    pad(rescue ? "active" : "inactive", 10);
    COL(WHITE);
    print(TARGETS[TGT_RESCUE].desc);
    print("\n");
}

// The target and every unit it pulls in, by stage.
static void cmd_list_dependencies(int target)
{
    COL(WHITE);
    printr("%s.target\n", TARGETS[target].name);
    for (int stage = TGT_RESCUE; stage < TGT_COUNT; stage++) {
        if (!stage_in_target(stage, target)) continue;
        if (stage != TGT_RESCUE) printr("|-%s.target\n", TARGETS[stage].name);
        for (int i = 0; i < MAX_UNITS; i++) {
            const unit_t *u = &g_units[i];
            if (!u->used || u->stage != stage || !u->enabled) continue;
            COL(state_color(u));
            print(u->state == ST_FAILED ? "x" : (u->state == ST_INACTIVE ? "o" : "*"));
            COL(WHITE);
            printr(" |  %s.svc\n", u->name);
        }
    }
}

static void print_duration(uint32_t ms)
{
    if (ms < 1000) printr("%ums", ms);
    else if (ms < 60000) printr("%u.%us", ms / 1000, (ms % 1000) / 100);
    else printr("%umin %us", ms / 60000, (ms / 1000) % 60);
}

static void cmd_list_timers(void)
{
    COL(VGA_COLOR_LIGHT_CYAN);
    pad("NEXT", 12); pad("INTERVAL", 12); pad("RUNS", 7); print("UNIT\n");
    COL(WHITE);
    uint32_t now = get_time_ms();
    int n = 0;
    for (int i = 0; i < MAX_UNITS; i++) {
        unit_t *u = &g_units[i];
        if (!u->used || !is_timer(u)) continue;
        n++;

        char next[24] = "-", every[16] = "once", runs[12], num[12];
        if (u->state == ST_WAITING && u->next_run) {
            if ((int32_t)(u->next_run - now) > 0) {
                itoa((int)((u->next_run - now) / 1000), num, 10);
                strcpy(next, "in ");
                strcat(next, num);
                strcat(next, "s");
            } else {
                strcpy(next, "now");
            }
        }
        if (u->active_ms) {
            itoa((int)(u->active_ms / 1000), num, 10);
            strcpy(every, num);
            strcat(every, "s");
        }
        itoa((int)u->runs, runs, 10);
        pad(next, 12); pad(every, 12); pad(runs, 7);
        print(u->name);
        print("\n");
    }
    if (n == 0) print("No timers (add a [Timer] section with OnBootSec= / OnUnitActiveSec= to a unit).\n");
}

static void show_journal_for(const char *unit, int max_lines)
{
    int shown = 0;
    for (int i = g_journal_count - 1; i >= 0 && shown < max_lines; i--)
        if (!unit || strcmp(g_journal[i].unit, unit) == 0) shown++;
    int skip = 0;
    int total = 0;
    for (int i = 0; i < g_journal_count; i++) if (!unit || strcmp(g_journal[i].unit, unit) == 0) total++;
    skip = total - shown;

    int idx = 0;
    for (int i = 0; i < g_journal_count; i++) {
        if (unit && strcmp(g_journal[i].unit, unit) != 0) continue;
        if (idx++ < skip) continue;
        COL(VGA_COLOR_LIGHT_GREY);
        print_uptime_stamp(g_journal[i].ms);
        print(" ");
        COL(VGA_COLOR_LIGHT_CYAN);
        print(g_journal[i].unit);
        print(": ");
        COL(g_journal[i].level ? VGA_COLOR_LIGHT_RED : WHITE);
        print(g_journal[i].text);
        print("\n");
    }
    COL(WHITE);
}

static void cmd_status(unit_t *u)
{
    const char *active, *sub;
    state_words(u, &active, &sub);

    COL(state_color(u));
    print(u->state == ST_INACTIVE ? "o " : "* ");
    COL(WHITE);
    printr("%s.svc - %s\n", u->name, u->desc);
    printr("     Loaded: loaded (%s; %s)\n", u->path, u->enabled ? "enabled" : "disabled");
    print("     Active: ");
    COL(state_color(u));
    printr("%s (%s)", active, sub);
    COL(WHITE);
    if (u->since_ms) { print(" since "); print_uptime_stamp(u->since_ms); }
    print("\n");
    printr("       Type: %s%s\n", u->oneshot ? "oneshot" : "simple", is_timer(u) ? " (timer)" : "");
    if (u->exec_pre[0])   printr("    Process: ExecStartPre=%s\n", u->exec_pre);
    if (u->exec_start[0]) printr("    Process: ExecStart=%s (status=%d)\n", u->exec_start, u->last_status);
    if (u->exec_stop[0])  printr("    Process: ExecStop=%s\n", u->exec_stop);
    if (u->n_requires || u->n_wants || u->n_after) {
        print("       Deps:");
        for (int i = 0; i < u->n_requires; i++) printr(" Requires=%s", u->requires[i]);
        for (int i = 0; i < u->n_wants; i++)    printr(" Wants=%s", u->wants[i]);
        for (int i = 0; i < u->n_after; i++)    printr(" After=%s", u->after[i]);
        print("\n");
    }
    printr("       Runs: %u, restarts: %u", u->runs, u->restarts);
    if (u->runs) { print(", last run took "); print_duration(u->took_ms); }
    print("\n\n");
    show_journal_for(u->name, 6);
}

static void cmd_cat(unit_t *u)
{
    static char text[UNIT_TEXT_MAX + 1];
    if (avfs_get_content(u->path, text, sizeof(text)) != 0) { ERR("systemctl: cannot read %s\n", u->path); return; }
    COL(VGA_COLOR_LIGHT_GREY); printr("# %s\n", u->path); COL(WHITE);
    print(text);
    if (text[0] && text[strlen(text) - 1] != '\n') print("\n");
}

static bool is_flag(const char *s) { return s[0] == '-' && s[1] == '-'; }

void systemctl_command(int argc, char *argv[])
{
    if (!g_booted && !avfs_is_directory(units_dir())) { install_stock_units(); load_units(); }
    if (g_units[0].used == false && !g_booted) load_units();

    if (argc < 2) { cmd_list_units(); return; }
    const char *verb = argv[1];

    // flags that may follow the verb
    bool now_flag = false;
    int names_end = argc;
    for (int i = 2; i < argc; i++) if (is_flag(argv[i])) { if (strcmp(argv[i], "--now") == 0) now_flag = true; names_end = names_end > i ? i : names_end; }

    if (strcmp(verb, "list-units") == 0) {
        bool targets = false;
        for (int i = 2; i < argc; i++) if (strcmp(argv[i], "--type=target") == 0) targets = true;
        if (targets) cmd_list_targets(); else cmd_list_units();
        return;
    }
    if (strcmp(verb, "list-unit-files") == 0)  { cmd_list_unit_files(); return; }
    if (strcmp(verb, "list-timers") == 0)      { cmd_list_timers(); return; }
    if (strcmp(verb, "daemon-reload") == 0)    { int n = load_units(); journal_add("rsd", "Reloaded unit files.", 0); printr("Reloaded %d unit%s from %s\n", n, n == 1 ? "" : "s", units_dir()); return; }
    if (strcmp(verb, "get-default") == 0)      { printr("%s.target\n", TARGETS[default_target()].name); return; }
    if (strcmp(verb, "reboot") == 0)           { reboot_command(1, argv); return; }
    if (strcmp(verb, "poweroff") == 0 || strcmp(verb, "halt") == 0) { poweroff_command(1, argv); return; }

    if (strcmp(verb, "set-default") == 0) {
        int t = names_end > 2 ? target_from_name(argv[2]) : -1;
        if (t < 0) { ERR("Usage: systemctl set-default rescue|multi-user|graphical\n"); shell_set_status(1); return; }
        char path[64], text[24];
        default_target_path(path, sizeof(path));
        strcpy(text, TARGETS[t].name);
        strcat(text, "\n");
        if (avfs_save_file(path, text, (uint32_t)strlen(text), true) != 0) { ERR("systemctl: cannot write %s\n", path); shell_set_status(1); return; }
        printr("Default target is now %s.target (used at the next boot).\n", TARGETS[t].name);
        return;
    }
    if (strcmp(verb, "isolate") == 0 || strcmp(verb, "rescue") == 0 || strcmp(verb, "emergency") == 0 || strcmp(verb, "default") == 0) {
        int t = TGT_RESCUE;
        if (strcmp(verb, "isolate") == 0) {
            t = names_end > 2 ? target_from_name(argv[2]) : -1;
            if (t < 0) { ERR("Usage: systemctl isolate rescue|multi-user|graphical\n"); shell_set_status(1); return; }
        } else if (strcmp(verb, "default") == 0) {
            t = default_target();
        }
        isolate_target(t);
        return;
    }
    if (strcmp(verb, "list-dependencies") == 0) {
        int t = names_end > 2 ? target_from_name(argv[2]) : g_target;
        if (t < 0) { ERR("systemctl: unknown target %s\n", argv[2]); shell_set_status(1); return; }
        cmd_list_dependencies(t);
        return;
    }
    if (strcmp(verb, "is-system-running") == 0) {
        int failed = 0;
        for (int i = 0; i < MAX_UNITS; i++) if (g_units[i].used && g_units[i].state == ST_FAILED) failed++;
        const char *s = !g_booted ? "starting" : (failed ? "degraded" : "running");
        print(s); print("\n");
        shell_set_status(g_booted && !failed ? 0 : 1);
        return;
    }

    if (strcmp(verb, "reset-failed") == 0) {
        for (int i = 0; i < MAX_UNITS; i++) if (g_units[i].used && g_units[i].state == ST_FAILED) { g_units[i].state = ST_INACTIVE; g_units[i].restarts = 0; g_units[i].next_run = 0; }
        return;
    }

    static const char *const NAME_VERBS[] = { "status", "start", "stop", "restart", "reload", "enable", "disable",
                                              "is-active", "is-enabled", "is-failed", "cat" };
    bool known = false;
    for (size_t i = 0; i < sizeof(NAME_VERBS) / sizeof(NAME_VERBS[0]); i++) if (strcmp(verb, NAME_VERBS[i]) == 0) known = true;
    if (!known) {
        ERR("systemctl: unknown command '%s'. Try: list-units [--type=target] list-unit-files list-timers list-dependencies\n"
            "           status start stop restart enable disable is-active is-enabled cat daemon-reload reset-failed\n"
            "           get-default set-default isolate rescue default is-system-running reboot poweroff\n", verb);
        return;
    }
    if (names_end <= 2) {
        if (strcmp(verb, "status") == 0) {
            int total = 0, failed = 0;
            for (int i = 0; i < MAX_UNITS; i++) if (g_units[i].used) { total++; if (g_units[i].state == ST_FAILED) failed++; }
            printr("State: %s\n   Units: %d loaded, %d failed\n  Booted: %s\n  Target: %s.target\n",
                   failed ? "degraded" : "running", total, failed, g_booted ? "yes" : "no", TARGETS[g_target].name);
            return;
        }
        ERR("systemctl %s: which unit? (systemctl list-unit-files shows them)\n", verb);
        return;
    }

    int exit_code = 0;
    for (int i = 2; i < names_end; i++) {
        unit_t *u = find_unit(argv[i]);
        if (!u) { ERR("Unit %s not found. Units live in %s as NAME.svc; run `systemctl daemon-reload` after adding one.\n", argv[i], units_dir()); exit_code = 4; continue; }

        if (strcmp(verb, "status") == 0)        cmd_status(u);
        else if (strcmp(verb, "cat") == 0)      cmd_cat(u);
        else if (strcmp(verb, "is-active") == 0) {
            const char *active, *sub;
            state_words(u, &active, &sub);
            print(active); print("\n");
            if (u->state != ST_ACTIVE && u->state != ST_WAITING) exit_code = 3;
        }
        else if (strcmp(verb, "is-failed") == 0) { print(u->state == ST_FAILED ? "failed\n" : "active\n"); if (u->state != ST_FAILED) exit_code = 1; }
        else if (strcmp(verb, "is-enabled") == 0) { print(u->enabled ? "enabled\n" : "disabled\n"); if (!u->enabled) exit_code = 1; }
        else if (strcmp(verb, "start") == 0) {
            if (start_unit(u) != 0) { ERR("Job for %s.svc failed. See `systemctl status %s` and `journalctl -u %s`.\n", u->name, u->name, u->name); exit_code = 1; }
        }
        else if (strcmp(verb, "stop") == 0)     stop_unit(u);
        else if (strcmp(verb, "restart") == 0 || strcmp(verb, "reload") == 0) {
            stop_unit(u);
            u->restarts = 0;
            if (start_unit(u) != 0) { ERR("Job for %s.svc failed. See `systemctl status %s`.\n", u->name, u->name); exit_code = 1; }
        }
        else if (strcmp(verb, "enable") == 0) {
            u->enabled = true;
            save_enabled_flags();
            printr("Enabled %s.svc (starts at boot).\n", u->name);
            if (now_flag && start_unit(u) != 0) exit_code = 1;
        }
        else if (strcmp(verb, "disable") == 0) {
            u->enabled = false;
            save_enabled_flags();
            printr("Disabled %s.svc.\n", u->name);
            if (now_flag) stop_unit(u);
        }
    }
    shell_set_status(exit_code);
}

// ---- journalctl ----------------------------------------------------------------------------------

void journalctl_command(int argc, char *argv[])
{
    const char *unit = NULL;
    int lines = 25;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            char name[NAME_LEN];
            normalize_name(argv[++i], name);
            static char keep[NAME_LEN];
            copy_str(keep, sizeof(keep), name);
            unit = keep;
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            lines = 0;
            for (const char *p = argv[++i]; *p >= '0' && *p <= '9'; p++) lines = lines * 10 + (*p - '0');
            if (lines <= 0) lines = 1;
        } else if (strcmp(argv[i], "--no-pager") == 0 || strcmp(argv[i], "-b") == 0) {
            continue;
        } else {
            ERR("Usage: journalctl [-u UNIT] [-n LINES]\n");
            return;
        }
    }
    if (g_journal_count == 0) { print("-- No journal entries --\n"); return; }
    show_journal_for(unit, lines);
}

// ---- systemd-analyze -----------------------------------------------------------------------------

void analyze_command(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "blame") == 0) {
        unit_t *order[MAX_UNITS];
        int n = 0;
        for (int i = 0; i < MAX_UNITS; i++) if (g_units[i].used && g_units[i].runs) order[n++] = &g_units[i];
        for (int i = 1; i < n; i++)   // slowest first
            for (int k = i; k > 0 && order[k]->took_ms > order[k - 1]->took_ms; k--) { unit_t *t = order[k]; order[k] = order[k - 1]; order[k - 1] = t; }
        for (int i = 0; i < n; i++) {
            COL(VGA_COLOR_LIGHT_BROWN);
            char buf[16];
            if (order[i]->took_ms < 1000) { itoa((int)order[i]->took_ms, buf, 10); strcat(buf, "ms"); }
            else { itoa((int)(order[i]->took_ms / 1000), buf, 10); strcat(buf, "s"); }
            pad(buf, 9);
            COL(WHITE);
            print(order[i]->name);
            print(".svc\n");
        }
        if (n == 0) print("No unit has run yet.\n");
        return;
    }

    uint32_t started = g_boot_end_ms >= g_boot_start_ms ? g_boot_end_ms - g_boot_start_ms : 0;
    int active = 0, failed = 0;
    for (int i = 0; i < MAX_UNITS; i++) if (g_units[i].used) { if (g_units[i].state == ST_FAILED) failed++; else if (g_units[i].state != ST_INACTIVE || g_units[i].runs) active++; }
    printr("Startup finished in %u ms (kernel) + %u ms (units); %s.target reached at %u ms.\n",
           g_boot_start_ms, started, TARGETS[g_target].name, g_boot_end_ms);
    printr("%d unit%s ran, %d failed. `systemd-analyze blame` lists them by time.\n", active, active == 1 ? "" : "s", failed);
}
