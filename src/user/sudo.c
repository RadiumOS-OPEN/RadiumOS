#include "../Avfs/Avfs.h"
#include "../terminal/terminal.h"
#include "../keyboard/keyboard.h"
#include "../utility/utility.h"
#include "../timers/timer.h"
#include "../vga/vga.h"
#include "../commands/mv.h"
#include "sudo.h"
#include "users.h"

// sudo.c - cpw (create/change the sudo password) and sudo (run a command with privileges)
//
//   cpw                    create the sudo password, or change it (asks for the current one first)
//   sudo <command...>      run a command with privileges; asks for the password when sudo is locked
//   sudo -k                lock sudo now
//   sudo -l                show whether sudo is set up and locked
//
// The password is never stored. /disk/.sudoInfo is a PRP-sealed (ChaCha20-Poly1305) envelope whose
// key is derived from the password, so the file can only be opened with the right one (see prp/mod.rs).
// Once a password exists sudo starts every boot locked, unlocks for SUDO_TIMEOUT_MS after a correct
// password, and locks again by itself. While it is locked, privileged commands and /disk/.sudoInfo
// are refused (sudo_gate, called by the shell before every command).

extern int rust_sudo_create(const uint8_t *password, const uint8_t *path);
extern int rust_sudo_verify(const uint8_t *password, const uint8_t *path);

#define SUDO_FILE         "/disk/.sudoInfo"
#define SUDO_TIMEOUT_MS   (5u * 60u * 1000u)
#define SUDO_MIN_PASSWORD 4
#define SUDO_MAX_PASSWORD 64
#define SUDO_MAX_TRIES    3

static bool     g_unlocked    = false;
static uint32_t g_last_used   = 0;
static int      g_elevated    = 0;   // > 0 while a sudo command is running

// Commands that need sudo once a sudo password exists (or, with accounts, for everyone but root).
static const char *const PRIVILEGED[] = {
    "reboot", "poweroff", "shutdown", "exit", "diskpart", "kill", "rawsend", "tcpreset", "setdns", "rpkg",
    "update", "useradd", "userdel", "usermod",
};

// Files a normal user may not touch (accounts mode). prefix: match any name that starts with it (FAT folds
// dot-file names to 8.3, so ".sudoInfo" is also reachable as ".sud*").
typedef struct { const char *path; bool prefix; } protected_t;
static const protected_t PROTECTED[] = {
    { "/disk/.sud", true }, { "/disk/etc", false }, { "/disk/vault", false }, { "/disk/radium", false },
    { "/disk/discord.tok", false }, { "/etc/.password.cfg", false }, { "/etc/.authkey.cfg", false },
};

#define SUDO_ERR(...) do { \
    terminal_setcolor(VGA_COLOR_LIGHT_RED); \
    printr(__VA_ARGS__); \
    terminal_setcolor(VGA_COLOR_WHITE); \
} while (0)

static bool legacy_configured(void)
{
    return avfs_file_exists(SUDO_FILE);
}

bool sudo_is_configured(void)
{
    return users_enabled() || legacy_configured();
}

bool sudo_is_elevated(void)
{
    return g_elevated > 0;
}

void sudo_reset_elevation(void)
{
    g_elevated = 0;
}

void sudo_lock(void)
{
    g_unlocked = false;
}

bool sudo_is_unlocked(void)
{
    if (g_elevated > 0) return true;
    if (!g_unlocked) return false;
    if (get_time_ms() - g_last_used >= SUDO_TIMEOUT_MS) {
        g_unlocked = false;
        return false;
    }
    return true;
}

uint32_t sudo_unlocked_seconds_left(void)
{
    if (g_elevated > 0 || !sudo_is_unlocked()) return 0;
    return (SUDO_TIMEOUT_MS - (get_time_ms() - g_last_used)) / 1000;
}

static bool is_privileged(const char *command)
{
    for (size_t i = 0; i < sizeof(PRIVILEGED) / sizeof(PRIVILEGED[0]); i++)
        if (strcmp(command, PRIVILEGED[i]) == 0) return true;
    return false;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool starts_with_nocase(const char *s, const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) if (lower(s[i]) != lower(p[i])) return false;
    return true;
}

// resolved: absolute normalized path. destructive: rm/mv/shred also may not remove a folder that contains a protected path.
bool sudo_path_protected(const char *resolved, bool destructive)
{
    if (!users_enabled()) return legacy_configured() && strcmp(resolved, SUDO_FILE) == 0;
    size_t rl = strlen(resolved);
    for (size_t i = 0; i < sizeof(PROTECTED) / sizeof(PROTECTED[0]); i++) {
        const char *p = PROTECTED[i].path;
        size_t n = strlen(p);
        if (rl >= n && starts_with_nocase(resolved, p, n) && (PROTECTED[i].prefix || resolved[n] == '\0' || resolved[n] == '/')) return true;
        if (destructive && rl < n && (rl == 1 || p[rl] == '/') && starts_with_nocase(p, resolved, rl)) return true;
    }
    return false;
}

static bool is_destructive(const char *command)
{
    return strcmp(command, "rm") == 0 || strcmp(command, "mv") == 0 || strcmp(command, "shred") == 0;
}

static bool path_arg_blocked(const char *command, const char *arg)
{
    char resolved[512];
    mv_resolve(arg, resolved);
    return sudo_path_protected(resolved, is_destructive(command));
}

static bool gate_blocked(void)
{
    if (users_enabled()) return !users_is_root();
    return legacy_configured() && !sudo_is_unlocked();
}

bool sudo_gate(const char *command, int argc, char *argv[])
{
    if (!gate_blocked()) return true;

    if (is_privileged(command)) {
        if (users_enabled())
            SUDO_ERR("%s: permission denied - run: sudo %s\n", command, command);
        else
            SUDO_ERR("%s: permission denied - sudo is locked. Run: sudo %s\n", command, command);
        return false;
    }

    // `sudo` re-checks its own command once it is running elevated.
    if (strcmp(command, "sudo") == 0) return true;

    for (int i = 1; i < argc; i++) {
        if (path_arg_blocked(command, argv[i])) {
            SUDO_ERR("%s: permission denied - %s is protected. %s\n", command, argv[i],
                     users_enabled() ? "Only root can touch it." : "Unlock sudo first.");
            return false;
        }
    }
    return true;
}

// Same check for the files a command redirects from/to (cmd < in > out).
bool sudo_gate_files(const char *command, const char *in_file, const char *out_file)
{
    if (!gate_blocked()) return true;
    const char *files[2] = { in_file, out_file };
    for (int i = 0; i < 2; i++) {
        if (files[i] && path_arg_blocked(i == 1 ? "redirect" : command, files[i])) {
            SUDO_ERR("%s: permission denied - %s is protected\n", command, files[i]);
            return false;
        }
    }
    return true;
}

static char g_password[COMMAND_BUFFER_SIZE];

static void wipe_password(void)
{
    for (size_t i = 0; i < sizeof(g_password); i++) ((volatile char *)g_password)[i] = 0;
}

// Reads a hidden line into g_password. False if it is empty or too long.
static bool ask_password(const char *prompt)
{
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print(prompt);
    terminal_setcolor(VGA_COLOR_WHITE);
    keyboard_input_secure(g_password);
    size_t n = strlen(g_password);
    if (n == 0) return false;
    if (n > SUDO_MAX_PASSWORD) {
        SUDO_ERR("password too long (max %d characters)\n", SUDO_MAX_PASSWORD);
        wipe_password();
        return false;
    }
    return true;
}

// Asks for the password up to SUDO_MAX_TRIES times. True when it opens .sudoInfo.
static bool authenticate(const char *prompt)
{
    for (int attempt = 1; attempt <= SUDO_MAX_TRIES; attempt++) {
        if (!ask_password(prompt)) {
            SUDO_ERR("sudo: a password is required\n");
            continue;
        }
        int rc = rust_sudo_verify((const uint8_t *)g_password, (const uint8_t *)SUDO_FILE);
        wipe_password();

        if (rc == 0) return true;
        if (rc == -3) {
            SUDO_ERR("sudo: %s is damaged. Remove it from another system and run cpw again.\n", SUDO_FILE);
            return false;
        }
        if (rc == -1) {
            SUDO_ERR("sudo: cannot read %s (is /disk mounted?)\n", SUDO_FILE);
            return false;
        }
        SUDO_ERR("Sorry, try again.\n");
        sleep_ms(1000);
    }
    SUDO_ERR("sudo: %d incorrect password attempts\n", SUDO_MAX_TRIES);
    return false;
}

// Accounts mode: the password is the session user's own.
static bool authenticate_account(void)
{
    char prompt[64];
    snprintf(prompt, sizeof(prompt), "[sudo] password for %s: ", users_current_name());
    for (int attempt = 1; attempt <= SUDO_MAX_TRIES; attempt++) {
        terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
        print(prompt);
        terminal_setcolor(VGA_COLOR_WHITE);
        bool got = users_read_line(g_password, SUDO_MAX_PASSWORD + 2, false);
        bool ok = got && g_password[0] && users_verify_current(g_password);
        wipe_password();
        if (ok) return true;
        if (!got) return false;
        SUDO_ERR("Sorry, try again.\n");
        sleep_ms(1000);
    }
    SUDO_ERR("sudo: %d incorrect password attempts\n", SUDO_MAX_TRIES);
    return false;
}

static void wipe_buffer(char *buf, size_t size)
{
    for (size_t i = 0; i < size; i++) ((volatile char *)buf)[i] = 0;
}

void cpw_command(int argc, char *argv[])
{
    if (users_enabled()) {                    // with accounts, sudo asks for the user's own password
        print("Accounts are enabled: sudo uses your account password, so this is passwd.\n");
        passwd_command(argc > 1 ? 2 : 1, argv);
        return;
    }

    if (!avfs_is_directory("/disk")) {
        SUDO_ERR("cpw: /disk is not available, so the password cannot be saved\n");
        shell_set_status(1);
        return;
    }

    if (sudo_is_configured()) {
        print("Changing the sudo password.\n");
        if (!authenticate("Current sudo password: ")) {
            shell_set_status(1);
            return;
        }
    } else {
        print("Creating the sudo password. It is stored encrypted with PRP in " SUDO_FILE ".\n");
    }

    static char first[SUDO_MAX_PASSWORD + 1];

    if (!ask_password("New sudo password: ")) {
        SUDO_ERR("cpw: cancelled\n");
        shell_set_status(1);
        return;
    }
    if (strlen(g_password) < SUDO_MIN_PASSWORD) {
        SUDO_ERR("cpw: use at least %d characters\n", SUDO_MIN_PASSWORD);
        wipe_password();
        shell_set_status(1);
        return;
    }
    strcpy(first, g_password);
    wipe_password();

    bool same = ask_password("Retype new password: ") && strcmp(first, g_password) == 0;
    wipe_password();
    if (!same) {
        SUDO_ERR("cpw: the passwords did not match; nothing changed\n");
        wipe_buffer(first, sizeof(first));
        shell_set_status(1);
        return;
    }

    int rc = rust_sudo_create((const uint8_t *)first, (const uint8_t *)SUDO_FILE);
    wipe_buffer(first, sizeof(first));

    if (rc != 0) {
        SUDO_ERR("cpw: could not save %s (error %d)\n", SUDO_FILE, rc);
        shell_set_status(1);
        return;
    }

    sudo_lock();
    terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
    print("sudo password saved to " SUDO_FILE " (sealed with PRP).\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    print("sudo is now locked. Unlock it by running a command with sudo, e.g. sudo reboot\n");
}

static void sudo_usage(void)
{
    print("Usage: sudo <command...>   run a command with privileges\n");
    print("       sudo -k             lock sudo now\n");
    print("       sudo -l             show the sudo state\n");
    print("Create or change the password with: cpw\n");
}

static void sudo_status(void)
{
    if (users_enabled()) {
        terminal_setcolor(VGA_COLOR_LIGHT_BROWN);
        printr("user %s (uid %u): %s", users_current_name(), users_current_uid(),
               users_is_root() ? "root, no sudo needed\n" : (users_can_sudo() ? "may use sudo" : "may NOT use sudo\n"));
        if (!users_is_root() && users_can_sudo()) {
            uint32_t left = sudo_unlocked_seconds_left();
            if (left) printr(" (password remembered for %u:%02u more)\n", left / 60, left % 60);
            else print(" (password needed)\n");
        }
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    if (!sudo_is_configured()) {
        terminal_setcolor(VGA_COLOR_LIGHT_BROWN);
        print("sudo is not set up. Create the password with: cpw\n");
    } else if (sudo_is_unlocked()) {
        uint32_t left = sudo_unlocked_seconds_left();
        terminal_setcolor(VGA_COLOR_LIGHT_BROWN);
        printr("sudo is unlocked (locks again in %u:%02u)\n", left / 60, left % 60);
    } else {
        terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
        print("sudo is locked\n");
    }
    terminal_setcolor(VGA_COLOR_WHITE);
}

void sudo_command(int argc, char *argv[])
{
    if (argc < 2) { sudo_usage(); return; }

    if (strcmp(argv[1], "-k") == 0) { sudo_lock(); print("sudo: locked\n"); return; }
    if (strcmp(argv[1], "-l") == 0) { sudo_status(); return; }

    if (users_enabled()) {
        if (!users_can_sudo()) {
            SUDO_ERR("sudo: %s is not allowed to use sudo (ask root: usermod --sudo %s)\n", users_current_name(), users_current_name());
            shell_set_status(1);
            return;
        }
        if (!users_is_root() && !sudo_is_unlocked()) {
            if (!authenticate_account()) {
                shell_set_status(1);
                return;
            }
            g_unlocked = true;
        }
    } else if (!sudo_is_configured()) {
        SUDO_ERR("sudo: no sudo password is set up yet. Create one with: cpw\n");
        shell_set_status(1);
        return;
    } else if (!sudo_is_unlocked()) {
        if (!authenticate("[sudo] password: ")) {
            shell_set_status(1);
            return;
        }
        g_unlocked = true;
    }
    g_last_used = get_time_ms();

    static char line[COMMAND_BUFFER_SIZE];
    size_t o = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1 && o + 1 < sizeof(line)) line[o++] = ' ';
        for (const char *p = argv[i]; *p && o + 1 < sizeof(line); p++) line[o++] = *p;
    }
    line[o] = '\0';

    g_elevated++;
    execute_command_no_history(line);
    g_elevated--;

    if (g_unlocked) g_last_used = get_time_ms();
}