// crypt.c - passphrase encryption of files on /disk
//
//   crypt lock PATH...     encrypt files in place (a folder: everything below it)
//   crypt unlock PATH...   decrypt them again
//   crypt status PATH...   show which files are encrypted
//
// Each file becomes "RDCRYPT1" + salt + nonce + ChaCha20-Poly1305 tag + ciphertext, with the key stretched from the
// passphrase by PBKDF2-HMAC-SHA256 (rust_lib/src/prp/crypt.rs). Names, sizes and folder structure stay visible. The
// clusters that held the plaintext are overwritten with zeros when the file is replaced. The passphrase is not
// stored anywhere: lose it and the data is gone.

#include <stdint.h>
#include <stdbool.h>
#include "crypt.h"
#include "mv.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../keyboard/keyboard.h"
#include "../user/users.h"
#include "../user/sudo.h"

extern int rust_crypt_file(const uint8_t *src, const uint8_t *dst, const uint8_t *password, int mode);
extern int rust_crypt_probe(const uint8_t *path);

#define CR_ERR(...) do { terminal_setcolor(VGA_COLOR_LIGHT_RED); printr(__VA_ARGS__); terminal_setcolor(VGA_COLOR_WHITE); } while (0)
#define PASS_MIN 8
#define PASS_MAX 64

enum { M_LOCK, M_UNLOCK, M_STATUS };

static int  g_mode;
static char g_pass[PASS_MAX + 2];
static int  g_done, g_skipped, g_failed;
static bool g_stop;

static bool lower_prefix(const char *s, const char *p)
{
    for (; *p; s++, p++) {
        char a = *s, b = *p;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return false;
    }
    return true;
}

// Files that must stay readable for the system to start or to let anyone in.
static bool system_file(const char *p)
{
    return lower_prefix(p, "/disk/radium") || lower_prefix(p, "/disk/etc") || lower_prefix(p, "/disk/.sud") ||
           lower_prefix(p, "/disk/store.bin");
}

static void file_one(const char *path)
{
    int enc = rust_crypt_probe((const uint8_t *)path);
    if (enc < 0) { CR_ERR("crypt: %s: cannot read\n", path); g_failed++; return; }
    if (g_mode == M_STATUS) { printr("%s  %s\n", enc ? "encrypted" : "plain    ", path); g_done++; return; }
    if (system_file(path) || (!users_is_root() && sudo_path_protected(path, false))) { g_skipped++; return; }
    if ((g_mode == M_LOCK) == (enc == 1)) { g_skipped++; return; }      // already in the requested state

    int rc = rust_crypt_file((const uint8_t *)path, (const uint8_t *)path, (const uint8_t *)g_pass, g_mode == M_LOCK ? 0 : 1);
    if (rc == 0) { g_done++; return; }
    g_failed++;
    if (rc == -6) { CR_ERR("crypt: %s: wrong passphrase or damaged file\n", path); g_stop = g_mode == M_UNLOCK; }
    else if (rc == -3) CR_ERR("crypt: %s: too large (limit 48 MB) or out of memory\n", path);
    else CR_ERR("crypt: %s: failed (%d)\n", path, rc);
}

static void walk(const char *path, int depth)
{
    if (g_stop) return;
    if (!avfs_is_directory(path)) { file_one(path); return; }
    if (depth > 8) return;
    static DirectoryEntry lists[9][96];
    int n = avfs_list_directory(path, lists[depth], 96);
    for (int i = 0; i < n && !g_stop; i++) {
        const char *nm = lists[depth][i].name;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", strcmp(path, "/") == 0 ? "" : path, nm);
        if (lists[depth][i].is_directory) walk(child, depth + 1);
        else file_one(child);
    }
}

static void wipe_pass(void)
{
    for (size_t i = 0; i < sizeof(g_pass); i++) ((volatile char *)g_pass)[i] = 0;
}

static bool ask_pass(bool confirm)
{
    char again[PASS_MAX + 2];
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Passphrase: ");
    terminal_setcolor(VGA_COLOR_WHITE);
    if (!users_read_line(g_pass, sizeof(g_pass), false)) return false;
    size_t n = strlen(g_pass);
    if (n < (confirm ? PASS_MIN : 1) || n > PASS_MAX) {
        CR_ERR("crypt: use %d to %d characters\n", confirm ? PASS_MIN : 1, PASS_MAX);
        wipe_pass();
        return false;
    }
    if (!confirm) return true;
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Retype passphrase: ");
    terminal_setcolor(VGA_COLOR_WHITE);
    bool ok = users_read_line(again, sizeof(again), false) && strcmp(g_pass, again) == 0;
    for (size_t i = 0; i < sizeof(again); i++) ((volatile char *)again)[i] = 0;
    if (!ok) { CR_ERR("crypt: the passphrases did not match\n"); wipe_pass(); }
    return ok;
}

void crypt_command(int argc, char *argv[])
{
    if (argc < 3 || !(strcmp(argv[1], "lock") == 0 || strcmp(argv[1], "unlock") == 0 || strcmp(argv[1], "status") == 0)) {
        CR_ERR("Usage: crypt lock|unlock|status PATH...      (a folder is processed recursively)\n");
        shell_set_status(1);
        return;
    }
    g_mode = argv[1][0] == 'l' ? M_LOCK : (argv[1][0] == 'u' ? M_UNLOCK : M_STATUS);
    g_done = g_skipped = g_failed = 0;
    g_stop = false;

    for (int i = 2; i < argc; i++) {
        char resolved[512];
        mv_resolve(argv[i], resolved);
        if (!avfs_file_exists(resolved)) { CR_ERR("crypt: %s: no such file or folder\n", argv[i]); shell_set_status(1); return; }
        if (strncmp(resolved, "/disk", 5) != 0 || (resolved[5] != '\0' && resolved[5] != '/')) {
            CR_ERR("crypt: %s: only /disk is stored on the disk - files in RAM are not encrypted\n", argv[i]);
            shell_set_status(1);
            return;
        }
        if (g_mode != M_STATUS && system_file(resolved)) { CR_ERR("crypt: %s: system files stay readable so the system can start\n", argv[i]); shell_set_status(1); return; }
    }
    if (g_mode != M_STATUS && !ask_pass(g_mode == M_LOCK)) { shell_set_status(1); return; }

    if (g_mode == M_LOCK) avfs_wipe_freed(true);
    for (int i = 2; i < argc && !g_stop; i++) {
        char resolved[512];
        mv_resolve(argv[i], resolved);
        walk(resolved, 0);
    }
    avfs_wipe_freed(false);
    wipe_pass();

    terminal_setcolor(g_failed ? VGA_COLOR_LIGHT_RED : VGA_COLOR_LIGHT_GREEN);
    printr("crypt: %u file(s) %s, %u skipped, %u failed\n", (unsigned)g_done,
           g_mode == M_LOCK ? "locked" : (g_mode == M_UNLOCK ? "unlocked" : "listed"), (unsigned)g_skipped, (unsigned)g_failed);
    terminal_setcolor(VGA_COLOR_WHITE);
    if (g_failed) shell_set_status(1);
}
