// users.c - accounts, login, su, passwd and friends (see users.h)
//
//   useradd [-s] NAME     create an account (-s: may use sudo)
//   userdel NAME          remove an account
//   usermod --sudo|--nosudo|--lock NAME
//   passwd [NAME]         change a password (root may change anyone's; a normal user needs the old one)
//   su [NAME]             switch user (root needs no password)
//   id [NAME], users, logout
//
// Passwords are never stored: /disk/etc/passwd keeps a salted PBKDF2-HMAC-SHA256 record per account. This is policy
// enforced by the shell and the app runner; everything here runs in ring 0, so it does not stop hostile native code.

#include <stdint.h>
#include <stdbool.h>
#include "users.h"
#include "sudo.h"
#include "../terminal/terminal.h"
#include "../keyboard/keyboard.h"
#include "../Avfs/Avfs.h"
#include "../timers/timer.h"
#include "../utility/utility.h"
#include "../vga/vga.h"
#include "../io/io.h"
#include "../kernel/cmdline.h"

extern int rust_pw_hash(const uint8_t *password, uint8_t *out, uint32_t cap);
extern int rust_pw_verify(const uint8_t *password, const uint8_t *record);

#define USERS_DIR   "/disk/etc"
#define USERS_FILE  "/disk/etc/passwd"
#define HOME_ROOT   "/disk/home"
#define U_MAX       16
#define U_NAME_MAX  12
#define PW_MIN      6
#define PW_MAX      64
#define HASH_MAX    128

typedef struct {
    char     name[U_NAME_MAX + 1];
    uint32_t uid;
    bool     sudo;
    char     hash[HASH_MAX];      // empty = locked
} user_t;

static user_t g_u[U_MAX];
static int    g_n;
static bool   g_loaded, g_enabled;
static int    g_cur = -1;         // session user, -1 before a login

#define UERR(...) do { terminal_setcolor(VGA_COLOR_LIGHT_RED); printr(__VA_ARGS__); terminal_setcolor(VGA_COLOR_WHITE); } while (0)
#define UOK(...)  do { terminal_setcolor(VGA_COLOR_LIGHT_GREEN); printr(__VA_ARGS__); terminal_setcolor(VGA_COLOR_WHITE); } while (0)

static void wipe(void *p, size_t n)
{
    for (size_t i = 0; i < n; i++) ((volatile char *)p)[i] = 0;
}

static bool name_ok(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len > U_NAME_MAX) return false;
    if (!(n[0] >= 'a' && n[0] <= 'z')) return false;
    for (size_t i = 1; i < len; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

static int find_user(const char *name)
{
    for (int i = 0; i < g_n; i++) if (strcmp(g_u[i].name, name) == 0) return i;
    return -1;
}

static int find_uid(uint32_t uid)
{
    for (int i = 0; i < g_n; i++) if (g_u[i].uid == uid) return i;
    return -1;
}

static void ensure_root(void)
{
    if (find_uid(0) >= 0 || g_n >= U_MAX) return;
    user_t *r = &g_u[g_n++];
    memset(r, 0, sizeof(*r));
    strcpy(r->name, "root");
    r->uid = 0;
    r->sudo = true;
}

static void parse_db(char *buf)
{
    g_n = 0;
    for (char *line = buf; *line && g_n < U_MAX; ) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        char *f_uid = strchr(line, ':');
        char *f_sudo = f_uid ? strchr(f_uid + 1, ':') : NULL;
        char *f_hash = f_sudo ? strchr(f_sudo + 1, ':') : NULL;
        if (f_hash) {
            *f_uid++ = '\0'; *f_sudo++ = '\0'; *f_hash++ = '\0';
            if (name_ok(line) && find_user(line) < 0 && strlen(f_hash) < HASH_MAX && isdigit(f_uid[0])) {
                user_t *u = &g_u[g_n++];
                memset(u, 0, sizeof(*u));
                strcpy(u->name, line);
                u->uid = (uint32_t)atoi(f_uid);
                u->sudo = f_sudo[0] == '1';
                strcpy(u->hash, f_hash);
            }
        }
        if (!next) break;
        line = next;
    }
    ensure_root();
}

static void load_db(void)
{
    static char buf[4096];
    g_n = 0;
    g_enabled = false;
    if (!avfs_is_directory("/disk") || !avfs_file_exists(USERS_FILE)) return;

    g_enabled = true;                            // a damaged file must not open the system
    int size = avfs_get_filesize(USERS_FILE);
    if (size < 0 || size >= (int)sizeof(buf) || (size > 0 && avfs_read_file(USERS_FILE, buf, (uint32_t)size, 0) != 0)) size = 0;
    buf[size] = '\0';
    parse_db(buf);
}

static void ensure_loaded(void)
{
    if (g_loaded) return;
    g_loaded = true;
    load_db();
}

static int save_db(void)
{
    static char out[4096];
    int o = 0;
    for (int i = 0; i < g_n; i++)
        o += snprintf(out + o, sizeof(out) - (size_t)o, "%s:%u:%d:%s\n", g_u[i].name, g_u[i].uid, g_u[i].sudo ? 1 : 0, g_u[i].hash);
    if (!avfs_is_directory(USERS_DIR)) avfs_create_dir(USERS_DIR);
    int rc = avfs_save_file(USERS_FILE, out, (uint32_t)o, true);
    if (rc == 0) g_enabled = true;
    return rc;
}

// Some account must be able to act as root, or nobody could ever administer the system again.
static bool admin_possible(void)
{
    for (int i = 0; i < g_n; i++)
        if (g_u[i].hash[0] && (g_u[i].uid == 0 || g_u[i].sudo)) return true;
    return false;
}

bool users_enabled(void)           { ensure_loaded(); return g_enabled; }

const char *users_current_name(void)
{
    static char legacy[48];
    ensure_loaded();
    if (g_enabled) return g_cur >= 0 ? g_u[g_cur].name : "root";
    legacy[0] = '\0';
    if (avfs_file_exists("/etc/.username.cfg") && avfs_get_content("/etc/.username.cfg", legacy, sizeof(legacy)) == 0) {
        size_t n = strlen(legacy);
        while (n > 0 && (legacy[n - 1] == '\n' || legacy[n - 1] == '\r' || legacy[n - 1] == ' ')) legacy[--n] = '\0';
    }
    if (!legacy[0]) strcpy(legacy, "root");
    return legacy;
}

uint32_t users_current_uid(void)
{
    ensure_loaded();
    if (!g_enabled) return 0;
    return g_cur >= 0 ? g_u[g_cur].uid : 0;
}

bool users_is_root(void)
{
    ensure_loaded();
    if (!g_enabled) return true;
    if (g_cur >= 0 && g_u[g_cur].uid == 0) return true;
    return sudo_is_elevated();
}

bool users_can_sudo(void)
{
    ensure_loaded();
    if (!g_enabled || g_cur < 0) return false;
    return g_u[g_cur].uid == 0 || g_u[g_cur].sudo;
}

static const char DUMMY_RECORD[] = "$rp1$10000$00000000000000000000000000000000$0000000000000000000000000000000000000000000000000000000000000000";

static bool check_password(int idx, const char *pw)
{
    if (idx < 0 || !g_u[idx].hash[0]) {
        rust_pw_verify((const uint8_t *)pw, (const uint8_t *)DUMMY_RECORD);   // same work for unknown and locked accounts
        return false;
    }
    return rust_pw_verify((const uint8_t *)pw, (const uint8_t *)g_u[idx].hash) == 0;
}

bool users_verify_current(const char *password)
{
    ensure_loaded();
    return g_cur >= 0 && check_password(g_cur, password);
}

// ---- keyboard ---------------------------------------------------------------------------------------------------

bool users_read_line(char *out, int cap, bool echo)
{
    static bool ctrl;
    int n = 0;
    bool ext = false;
    out[0] = '\0';
    for (;;) {
        if (!is_key_pressed()) continue;
        uint8_t sc = inb(0x60);
        if (sc == 0xE0) { ext = true; continue; }
        if (ext) { ext = false; continue; }
        if (sc & 0x80) {
            uint8_t k = sc & 0x7F;
            if (k == 0x2A || k == 0x36) shift_active = false;
            if (k == 0x1D) ctrl = false;
            continue;
        }
        if (sc == 0x2A || sc == 0x36) { shift_active = true; continue; }
        if (sc == 0x1D) { ctrl = true; continue; }
        if (sc == 0x3A) { caps_lock_active = !caps_lock_active; set_keyboard_leds(caps_lock_active ? 0x04 : 0); continue; }
        if (ctrl && sc == 0x2E) {
            terminal_putchar('\n');
            wipe(out, (size_t)cap);
            return false;
        }
        if (sc == 0x1C) { terminal_putchar('\n'); out[n] = '\0'; return true; }
        if (sc == 0x0E) {
            if (n > 0) { out[--n] = '\0'; terminal_putchar('\b'); terminal_putchar(' '); terminal_putchar('\b'); }
            continue;
        }
        char c = keyboard_to_char(sc, shift_active, caps_lock_active);
        if (c >= 32 && c < 127 && n < cap - 1) {
            out[n++] = c;
            out[n] = '\0';
            terminal_putchar(echo ? c : '*');
        }
    }
}

static bool prompt_secret(const char *prompt, char *out, int cap)
{
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print(prompt);
    terminal_setcolor(VGA_COLOR_WHITE);
    return users_read_line(out, cap, false);
}

// New password asked twice. out must hold PW_MAX + 2 bytes.
static bool ask_new_password(char *out)
{
    char again[PW_MAX + 2];
    if (!prompt_secret("New password: ", out, PW_MAX + 2)) return false;
    size_t n = strlen(out);
    if (n < PW_MIN || n > PW_MAX) {
        UERR("use %d to %d characters\n", PW_MIN, PW_MAX);
        wipe(out, PW_MAX + 2);
        return false;
    }
    if (!prompt_secret("Retype new password: ", again, sizeof(again))) { wipe(out, PW_MAX + 2); return false; }
    bool same = strcmp(out, again) == 0;
    wipe(again, sizeof(again));
    if (!same) {
        UERR("the passwords did not match; nothing changed\n");
        wipe(out, PW_MAX + 2);
        return false;
    }
    return true;
}

static bool set_password(int idx, const char *pw)
{
    char rec[HASH_MAX];
    if (rust_pw_hash((const uint8_t *)pw, (uint8_t *)rec, sizeof(rec)) != 0) return false;
    strcpy(g_u[idx].hash, rec);
    return true;
}

static void enter_home(int idx)
{
    char home[64];
    snprintf(home, sizeof(home), HOME_ROOT "/%s", g_u[idx].name);
    if (avfs_is_directory(home)) avfs_chdir(home);
}

// ---- login ------------------------------------------------------------------------------------------------------

void users_login_prompt(void)
{
    ensure_loaded();
    int fails = 0;
    keyboard_pause(true);                          // at boot the shell task is already running and would eat the keys
    for (;;) {
        char name[U_NAME_MAX + 8], pw[PW_MAX + 2];
        terminal_clear();
        terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
        print("\n  RadiumOS\n");
        terminal_setcolor(VGA_COLOR_LIGHT_GREY);
        print("  Log in with an account created by useradd.\n\n");
        terminal_setcolor(VGA_COLOR_WHITE);

        print("  login: ");
        if (!users_read_line(name, sizeof(name), true)) continue;
        print("  ");
        if (!prompt_secret("password: ", pw, sizeof(pw))) continue;

        int idx = find_user(name);
        bool ok = check_password(idx, pw);
        wipe(pw, sizeof(pw));
        if (ok) {
            g_cur = idx;
            sudo_lock();
            enter_home(idx);
            terminal_clear();
            keyboard_pause(false);
            return;
        }
        fails++;
        UERR("\n  Login incorrect\n");
        sleep_ms(fails >= 3 ? 10000 : 1500);      // slows guessing; the delay stays at 10 s from the third failure on
    }
}

int users_count(void) { ensure_loaded(); return g_enabled ? g_n : 0; }
const char *users_name_at(int i) { ensure_loaded(); return i >= 0 && i < g_n ? g_u[i].name : ""; }
bool users_has_password_at(int i) { ensure_loaded(); return i >= 0 && i < g_n && g_u[i].hash[0]; }
bool users_sudo_at(int i) { ensure_loaded(); return i >= 0 && i < g_n && (g_u[i].uid == 0 || g_u[i].sudo); }
bool users_has_session(void) { ensure_loaded(); return !g_enabled || g_cur >= 0; }

bool users_login_skipped(void)
{
    ensure_loaded();
    if (!cmdline_has("nologin")) return false;
    if (g_enabled) {
        int r = find_uid(0);
        g_cur = r >= 0 ? r : -1;
    }
    return true;
}

bool users_session_login(const char *name, const char *password)
{
    ensure_loaded();
    if (!g_enabled) return true;
    int idx = find_user(name);
    if (!check_password(idx, password)) return false;
    g_cur = idx;
    sudo_lock();
    enter_home(idx);
    return true;
}

void users_session_logout(void)
{
    g_cur = -1;
    sudo_lock();
}

void users_boot_login(void)
{
    ensure_loaded();
    if (!g_enabled) return;
    if (cmdline_has("nologin")) {
        int r = find_uid(0);
        g_cur = r >= 0 ? r : -1;
        terminal_setcolor(VGA_COLOR_LIGHT_BROWN);
        print("WARNING: nologin on the kernel command line - login skipped, session is root\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    users_login_prompt();
}

void logout_command(int argc, char *argv[])
{
    (void)argc; (void)argv;
    ensure_loaded();
    if (!g_enabled) { UERR("logout: login is not enabled. Create an account with useradd first.\n"); shell_set_status(1); return; }
    g_cur = -1;
    sudo_lock();
    users_login_prompt();
}

// ---- commands -----------------------------------------------------------------------------------------------------

static bool need_root(const char *cmd)
{
    if (users_is_root()) return true;
    UERR("%s: permission denied - only root (or sudo) may do that\n", cmd);
    shell_set_status(1);
    return false;
}

void useradd_command(int argc, char *argv[])
{
    ensure_loaded();
    if (!need_root("useradd")) return;
    bool sudo = false;
    const char *name = NULL;
    int extra = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) sudo = true;
        else if (!name) name = argv[i];
        else extra++;
    }
    if (extra) name = NULL;
    if (!name) { UERR("Usage: useradd [-s] NAME     (-s: the account may use sudo)\n"); shell_set_status(1); return; }
    if (!avfs_is_directory("/disk")) { UERR("useradd: /disk is not available, so accounts cannot be saved\n"); shell_set_status(1); return; }
    if (!name_ok(name)) { UERR("useradd: a name is 1-%d characters: a-z, 0-9, _ or -, starting with a letter\n", U_NAME_MAX); shell_set_status(1); return; }

    bool fresh = !g_enabled;
    if (find_user(name) >= 0 || strcmp(name, "root") == 0) { UERR("useradd: %s already exists\n", name); shell_set_status(1); return; }
    if (g_n >= U_MAX - 1) { UERR("useradd: too many accounts (max %d)\n", U_MAX); shell_set_status(1); return; }
    if (fresh && !sudo) {
        UERR("useradd: the first account must use -s: root has no password yet, so someone needs sudo\n");
        shell_set_status(1);
        return;
    }

    char pw[PW_MAX + 2];
    if (!ask_new_password(pw)) { shell_set_status(1); return; }

    if (fresh) { g_n = 0; ensure_root(); }
    uint32_t uid = 1000;
    for (int i = 0; i < g_n; i++) if (g_u[i].uid >= uid) uid = g_u[i].uid + 1;
    user_t *u = &g_u[g_n];
    memset(u, 0, sizeof(*u));
    strcpy(u->name, name);
    u->uid = uid;
    u->sudo = sudo;
    g_n++;
    bool ok = set_password(g_n - 1, pw);
    wipe(pw, sizeof(pw));
    if (!ok || save_db() != 0) {
        g_n--;
        if (fresh) g_n = 0;
        UERR("useradd: could not save the account database\n");
        shell_set_status(1);
        return;
    }
    char home[64];
    if (!avfs_is_directory(HOME_ROOT)) avfs_create_dir(HOME_ROOT);
    snprintf(home, sizeof(home), HOME_ROOT "/%s", name);
    if (!avfs_is_directory(home)) avfs_create_dir(home);

    UOK("account %s created (uid %u%s), home %s\n", name, uid, sudo ? ", sudo" : "", home);
    if (fresh) {
        g_cur = find_uid(0);                     // this session carries on as root until the next login
        terminal_setcolor(VGA_COLOR_LIGHT_BROWN);
        print("Login is now required at boot (or run logout). root is locked: log in as ");
        print(name);
        print(" and use sudo.\n");
        terminal_setcolor(VGA_COLOR_WHITE);
    }
}

void userdel_command(int argc, char *argv[])
{
    ensure_loaded();
    if (!need_root("userdel")) return;
    if (argc != 2) { UERR("Usage: userdel NAME\n"); shell_set_status(1); return; }
    int idx = find_user(argv[1]);
    if (!g_enabled || idx < 0) { UERR("userdel: no such account: %s\n", argv[1]); shell_set_status(1); return; }
    if (g_u[idx].uid == 0) { UERR("userdel: root cannot be removed\n"); shell_set_status(1); return; }
    if (idx == g_cur) { UERR("userdel: you are logged in as %s\n", argv[1]); shell_set_status(1); return; }

    user_t saved[U_MAX];
    int saved_n = g_n, saved_cur = g_cur;
    memcpy(saved, g_u, sizeof(saved));
    for (int i = idx; i < g_n - 1; i++) g_u[i] = g_u[i + 1];
    g_n--;
    if (g_cur > idx) g_cur--;
    if (!admin_possible() || save_db() != 0) {
        memcpy(g_u, saved, sizeof(saved)); g_n = saved_n; g_cur = saved_cur;
        UERR("userdel: refused - it would leave nobody able to administer the system\n");
        shell_set_status(1);
        return;
    }
    UOK("account %s removed (its files in %s/%s are kept)\n", argv[1], HOME_ROOT, argv[1]);
}

void usermod_command(int argc, char *argv[])
{
    ensure_loaded();
    if (!need_root("usermod")) return;
    if (argc != 3 || !(strcmp(argv[1], "--sudo") == 0 || strcmp(argv[1], "--nosudo") == 0 || strcmp(argv[1], "--lock") == 0)) {
        UERR("Usage: usermod --sudo|--nosudo|--lock NAME\n");
        shell_set_status(1);
        return;
    }
    int idx = find_user(argv[2]);
    if (!g_enabled || idx < 0) { UERR("usermod: no such account: %s\n", argv[2]); shell_set_status(1); return; }

    user_t saved = g_u[idx];
    if (strcmp(argv[1], "--sudo") == 0) g_u[idx].sudo = true;
    else if (strcmp(argv[1], "--nosudo") == 0) g_u[idx].sudo = false;
    else g_u[idx].hash[0] = '\0';
    if (!admin_possible() || save_db() != 0) {
        g_u[idx] = saved;
        UERR("usermod: refused - it would leave nobody able to administer the system\n");
        shell_set_status(1);
        return;
    }
    UOK("account %s updated\n", argv[2]);
}

void passwd_command(int argc, char *argv[])
{
    ensure_loaded();
    if (argc > 2) { UERR("Usage: passwd [NAME]\n"); shell_set_status(1); return; }
    if (!avfs_is_directory("/disk")) { UERR("passwd: /disk is not available, so the password cannot be saved\n"); shell_set_status(1); return; }

    bool fresh = !g_enabled;
    const char *target = argc == 2 ? argv[1] : (fresh ? "root" : users_current_name());
    if (fresh && strcmp(target, "root") != 0) { UERR("passwd: no accounts yet - set root's password first, or use useradd -s NAME\n"); shell_set_status(1); return; }
    if (fresh) { g_n = 0; ensure_root(); }
    int idx = find_user(target);
    if (idx < 0) { UERR("passwd: no such account: %s\n", target); shell_set_status(1); return; }

    bool own = !fresh && idx == g_cur;
    if (!own && !users_is_root()) {
        UERR("passwd: permission denied - you can only change your own password\n");
        shell_set_status(1);
        return;
    }
    if (own && g_u[idx].hash[0] && !sudo_is_elevated()) {
        char old[PW_MAX + 2];
        if (!prompt_secret("Current password: ", old, sizeof(old))) { shell_set_status(1); return; }
        bool ok = check_password(idx, old);
        wipe(old, sizeof(old));
        if (!ok) { UERR("passwd: wrong password\n"); sleep_ms(1500); shell_set_status(1); return; }
    }

    char pw[PW_MAX + 2];
    if (!ask_new_password(pw)) { if (fresh) g_n = 0; shell_set_status(1); return; }
    char old_hash[HASH_MAX];
    strcpy(old_hash, g_u[idx].hash);
    bool ok = set_password(idx, pw);
    wipe(pw, sizeof(pw));
    if (!ok || save_db() != 0) {
        strcpy(g_u[idx].hash, old_hash);
        if (fresh) { g_n = 0; g_enabled = false; }
        UERR("passwd: could not save the password\n");
        shell_set_status(1);
        return;
    }
    UOK("password for %s updated\n", target);
    if (fresh) {
        g_cur = idx;
        terminal_setcolor(VGA_COLOR_LIGHT_BROWN);
        print("Accounts are now enabled: a login is required at boot. Add a sudo user with: useradd -s NAME\n");
        terminal_setcolor(VGA_COLOR_WHITE);
    }
}

void su_command(int argc, char *argv[])
{
    ensure_loaded();
    if (!g_enabled) { UERR("su: no accounts yet - create one with useradd\n"); shell_set_status(1); return; }
    if (argc > 2) { UERR("Usage: su [NAME]\n"); shell_set_status(1); return; }
    const char *target = argc == 2 ? argv[1] : "root";
    int idx = find_user(target);
    if (idx < 0) { UERR("su: no such account: %s\n", target); shell_set_status(1); return; }

    if (!users_is_root()) {
        char pw[PW_MAX + 2];
        if (!prompt_secret("Password: ", pw, sizeof(pw))) { shell_set_status(1); return; }
        bool ok = check_password(idx, pw);
        wipe(pw, sizeof(pw));
        if (!ok) { UERR("su: authentication failure\n"); sleep_ms(1500); shell_set_status(1); return; }
    }
    g_cur = idx;
    sudo_lock();
    enter_home(idx);
}

void id_command(int argc, char *argv[])
{
    ensure_loaded();
    if (argc > 2) { UERR("Usage: id [NAME]\n"); shell_set_status(1); return; }
    if (!g_enabled) {
        if (argc == 2 && strcmp(argv[1], users_current_name()) != 0) { UERR("id: %s: no such user\n", argv[1]); shell_set_status(1); return; }
        printr("uid=0(%s) groups=0(root)  (no accounts: single-user mode)\n", users_current_name());
        return;
    }
    int idx = argc == 2 ? find_user(argv[1]) : g_cur;
    if (idx < 0 && argc == 1) idx = find_uid(0);
    if (idx < 0) { UERR("id: %s: no such user\n", argc == 2 ? argv[1] : "?"); shell_set_status(1); return; }
    printr("uid=%u(%s) groups=%u(%s)%s\n", g_u[idx].uid, g_u[idx].name, g_u[idx].uid, g_u[idx].name,
           (g_u[idx].sudo && g_u[idx].uid != 0) ? ",sudo" : "");
}

void users_command(int argc, char *argv[])
{
    (void)argc; (void)argv;
    ensure_loaded();
    if (!g_enabled) { print("no accounts yet (single-user mode); create one with: useradd -s NAME\n"); return; }
    print("NAME          UID   SUDO  PASSWORD\n");
    for (int i = 0; i < g_n; i++) {
        char uid[12];
        snprintf(uid, sizeof(uid), "%u", g_u[i].uid);
        print(g_u[i].name);
        for (size_t k = strlen(g_u[i].name); k < 14; k++) print(" ");
        print(uid);
        for (size_t k = strlen(uid); k < 6; k++) print(" ");
        print((g_u[i].sudo || g_u[i].uid == 0) ? "yes   " : "no    ");
        print(g_u[i].hash[0] ? "set" : "locked");
        if (i == g_cur) print("   <- you");
        print("\n");
    }
}

// ---- self test (see selftest.c) ---------------------------------------------------------------------------------

// Runs against an in-memory database and puts the real one back afterwards. 0 = ok, else the number of the failed check.
int users_selftest(void)
{
    ensure_loaded();
    static user_t keep_u[U_MAX];
    memcpy(keep_u, g_u, sizeof(g_u));
    int keep_n = g_n, keep_cur = g_cur;
    bool keep_enabled = g_enabled;
    int fail = 0;

    static char text[] =
        "root:0:1:\n"                                   // locked root
        "alice:1000:1:\n"                               // hash filled in below
        "bob:1001:0:\n"
        "Bad:1002:0:\n"                                 // upper case: rejected
        "bob:1003:0:\n"                                 // duplicate: rejected
        "broken line\n";
    parse_db(text);
    if (g_n != 3 || find_user("alice") < 0 || find_user("bob") < 0 || find_user("Bad") >= 0) fail = 1;
    int a = find_user("alice"), b = find_user("bob"), r = find_uid(0);
    if (!fail && (r < 0 || g_u[r].hash[0] || !g_u[a].sudo || g_u[b].sudo || g_u[a].uid != 1000)) fail = 2;

    if (!fail) {
        if (!set_password(a, "alice-secret") || !set_password(b, "bob-secret")) fail = 3;
        else if (strcmp(g_u[a].hash, g_u[b].hash) == 0 || strncmp(g_u[a].hash, "$rp1$", 5) != 0) fail = 4;
        else if (!check_password(a, "alice-secret") || check_password(a, "alice-secrex") || check_password(b, "alice-secret")) fail = 5;
        else if (check_password(r, "") || check_password(r, "anything") || check_password(-1, "x")) fail = 6;   // locked and unknown accounts never match
    }
    if (!fail) {
        g_enabled = true;
        g_cur = b;                                                        // bob: no sudo
        char *ls_args[] = { "ls", "/disk/etc/passwd" };
        char *ok_args[] = { "ls", "/disk/home/bob" };
        if (users_is_root() || users_can_sudo()) fail = 7;
        else if (!sudo_path_protected("/disk/etc/passwd", false) || !sudo_path_protected("/DISK/Etc/passwd", false) ||
                 !sudo_path_protected("/disk/.sudoInfo", false) || !sudo_path_protected("/disk/.sudx", false)) fail = 8;
        else if (sudo_path_protected("/disk/home/bob/notes.txt", false) || sudo_path_protected("/disk/etcetera", false)) fail = 9;
        else if (!sudo_path_protected("/disk", true) || sudo_path_protected("/disk", false)) fail = 10;   // rm -r /disk would take /disk/etc with it
        else if (sudo_gate("ls", 2, ls_args) || !sudo_gate("ls", 2, ok_args)) fail = 11;
        else if (sudo_gate("reboot", 1, ok_args)) fail = 12;
        else if (sudo_gate_files("cat", "/disk/etc/passwd", NULL) || sudo_gate_files("echo", NULL, "/disk/etc/passwd")) fail = 13;
        if (!fail) {
            g_cur = a;                                                    // alice: sudo, but not root by herself
            if (users_is_root() || !users_can_sudo()) fail = 14;
            g_cur = r;                                                    // root: everything allowed
            if (!users_is_root() || !sudo_gate("reboot", 1, ok_args) || !sudo_gate("ls", 2, ls_args)) fail = 15;
        }
        if (!fail) {                                                      // the system must keep an administrator
            g_u[a].sudo = false;
            if (admin_possible()) fail = 16;
            g_u[a].sudo = true;
            if (!admin_possible()) fail = 17;
        }
    }

    memcpy(g_u, keep_u, sizeof(g_u));
    g_n = keep_n; g_cur = keep_cur; g_enabled = keep_enabled;
    return fail;
}
