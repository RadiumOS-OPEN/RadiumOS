// secure.c - security tasks
//
//   secaudit              security audit of this system: sudo, update trust, startup files, secrets, crypto self-test
//   integrity init        remember the SHA-256 of the files that decide what runs at boot (kernel, startup script, units, plugins)
//   integrity check [-q]  compare them with that record: MODIFIED / MISSING / NEW (-q: print only problems)
//   prp hash|keygen|fingerprint|sign|verify|seal|open   PRP signatures and envelopes from the shell
//   sha256sum FILE...     SHA-256 of files
//   vault set|get|list|rm NAME   named secrets sealed under a passphrase in /disk/vault (PRP envelope, PBKDF2 key)
//   shred FILE...         overwrite a file three times, then delete it
//   genpw [LENGTH] [N]    random passwords from the hardware random generator
//
// Everything here uses the PRP code in rust_lib/src/prp; no secret is ever written to disk unsealed.

#include <stdint.h>
#include <stdbool.h>
#include "secure.h"
#include "update.h"
#include "shellvars.h"
#include "mv.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../keyboard/keyboard.h"
#include "../user/sudo.h"
#include "../cpu/cpu.h"

extern int rust_prp_sha256_file(const uint8_t *filename, uint8_t *output);
extern int rust_prp_keygen(const uint8_t *name);
extern int rust_prp_fingerprint(const uint8_t *keyfile, uint8_t *output);
extern int rust_prp_sign(const uint8_t *file, const uint8_t *private_key);
extern int rust_prp_verify(const uint8_t *file, const uint8_t *expected_pub);
extern int rust_prp_seal_file_pub(const uint8_t *input, const uint8_t *output, const uint8_t *recipient_pub);
extern int rust_prp_open_file_prv(const uint8_t *input, const uint8_t *output, const uint8_t *private_key);
extern int rust_prp_random(uint8_t *output, uint32_t len);
extern int rust_prp_selftest(void);
extern int rust_prp_vault_save(const uint8_t *password, const uint8_t *secret, uint32_t len, const uint8_t *path);
extern int rust_prp_vault_load(const uint8_t *password, const uint8_t *path, uint8_t *out, uint32_t cap);
extern const char *kernel_boot_source(void);

#define COL(c) terminal_setcolor(c)
#define WHITE  VGA_COLOR_WHITE
#define ERR(...) do { COL(VGA_COLOR_LIGHT_RED); printr(__VA_ARGS__); COL(WHITE); } while (0)

static void pad(const char *s, size_t width)
{
    print(s);
    for (size_t n = strlen(s); n < width; n++) print(" ");
}

static void wipe(void *p, size_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    while (n--) *b++ = 0;
}

static void hex(const uint8_t *in, int n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[i * 2] = digits[in[i] >> 4]; out[i * 2 + 1] = digits[in[i] & 15]; }
    out[n * 2] = '\0';
}

// ---- audit report lines ----------------------------------------------------------------------------

typedef struct { int ok, warn, fail; bool quiet; } report_t;

static void line(report_t *r, int level, const char *name, const char *detail)
{
    if (level == 0) r->ok++; else if (level == 1) r->warn++; else r->fail++;
    if (r->quiet && level == 0) return;
    COL(WHITE); print("[");
    COL(level == 0 ? VGA_COLOR_LIGHT_GREEN : (level == 1 ? VGA_COLOR_LIGHT_BROWN : VGA_COLOR_LIGHT_RED));
    print(level == 0 ? "  OK  " : (level == 1 ? " WARN " : " FAIL "));
    COL(WHITE);
    print("] ");
    pad(name, 18);
    print(detail);
    print("\n");
}

// ---- integrity -------------------------------------------------------------------------------------

#define INTEG_DB    "/disk/radium/integ.db"
#define INTEG_MAX   64
#define INTEG_PATH  64

static char g_paths[INTEG_MAX][INTEG_PATH];
static char g_sums[INTEG_MAX][65];
static int  g_npaths;

static void add_path(const char *path)
{
    if (g_npaths >= INTEG_MAX || strlen(path) >= INTEG_PATH || !avfs_file_exists(path) || avfs_is_directory(path)) return;
    strcpy(g_paths[g_npaths++], path);
}

static void add_dir(const char *dir, const char *suffix)
{
    static DirectoryEntry entries[48];
    if (!avfs_is_directory(dir)) return;
    int n = avfs_list_directory(dir, entries, 48);
    for (int i = 0; i < n; i++) {
        if (entries[i].is_directory) continue;
        size_t len = strlen(entries[i].name), sl = suffix ? strlen(suffix) : 0;
        if (sl && (len < sl || strcmp(entries[i].name + len - sl, suffix) != 0)) continue;
        char path[INTEG_PATH];
        if (strlen(dir) + 1 + len >= sizeof(path)) continue;
        strcpy(path, dir); strcat(path, "/"); strcat(path, entries[i].name);
        add_path(path);
    }
}

// Collects the watched files and their hashes into g_paths / g_sums.
static void collect(void)
{
    g_npaths = 0;
    add_path("/disk/radium/kernel.bin");
    add_path("/disk/radium/kernel.old");
    add_path(shell_rc_path());
    add_path("/disk/.sudoInfo");
    add_path("/disk/units/enabled.txt");
    add_dir("/disk/units", ".svc");
    add_dir("/disk/dcplug", NULL);
    add_dir("/disk/apps", NULL);
    for (int i = 0; i < g_npaths; i++) {
        uint8_t digest[32];
        if (rust_prp_sha256_file((const uint8_t *)g_paths[i], digest) != 0) strcpy(g_sums[i], "unreadable");
        else hex(digest, 32, g_sums[i]);
    }
}

static const char *db_lookup(const char *db, const char *path, char sum[65])
{
    size_t plen = strlen(path);
    for (const char *p = db; *p; ) {
        const char *end = p;
        while (*end && *end != '\n') end++;
        if ((size_t)(end - p) == 65 + plen && p[64] == ' ' && strncmp(p + 65, path, plen) == 0) {
            memcpy(sum, p, 64);
            sum[64] = '\0';
            return sum;
        }
        p = *end ? end + 1 : end;
    }
    return NULL;
}

static int integrity_init(void)
{
    static char db[INTEG_MAX * (65 + INTEG_PATH + 2)];
    if (!avfs_is_directory("/disk")) { ERR("integrity: /disk is not mounted\n"); return 1; }
    if (!avfs_is_directory("/disk/radium")) avfs_create_dir("/disk/radium");
    collect();
    db[0] = '\0';
    for (int i = 0; i < g_npaths; i++) {
        strcat(db, g_sums[i]); strcat(db, " "); strcat(db, g_paths[i]); strcat(db, "\n");
    }
    if (avfs_save_file(INTEG_DB, db, (uint32_t)strlen(db), true) != 0) { ERR("integrity: could not write %s\n", INTEG_DB); return 1; }
    COL(VGA_COLOR_LIGHT_GREEN);
    printr("integrity: recorded %d file%s in %s\n", g_npaths, g_npaths == 1 ? "" : "s", INTEG_DB);
    COL(WHITE);
    return 0;
}

// Returns the number of problems, or -1 when there is no record. Prints each one (and each OK file unless quiet).
int integrity_check(bool quiet)
{
    static char db[INTEG_MAX * (65 + INTEG_PATH + 2)];
    int size = avfs_get_filesize(INTEG_DB);
    if (size <= 0 || size >= (int)sizeof(db) || avfs_get_content(INTEG_DB, db, sizeof(db)) != 0) return -1;

    collect();
    int problems = 0;
    char old[65];
    for (int i = 0; i < g_npaths; i++) {
        if (!db_lookup(db, g_paths[i], old)) {
            problems++;
            COL(VGA_COLOR_LIGHT_BROWN); print("  NEW       "); COL(WHITE); printr("%s\n", g_paths[i]);
        } else if (strcmp(old, g_sums[i]) != 0) {
            problems++;
            COL(VGA_COLOR_LIGHT_RED); print("  MODIFIED  "); COL(WHITE); printr("%s\n", g_paths[i]);
        } else if (!quiet) {
            COL(VGA_COLOR_LIGHT_GREEN); print("  ok        "); COL(WHITE); printr("%s\n", g_paths[i]);
        }
    }
    for (const char *p = db; *p; ) {
        const char *end = p;
        while (*end && *end != '\n') end++;
        if (end - p > 65) {
            char path[INTEG_PATH];
            size_t n = (size_t)(end - p) - 65;
            if (n < sizeof(path)) {
                memcpy(path, p + 65, n);
                path[n] = '\0';
                bool present = false;
                for (int i = 0; i < g_npaths; i++) if (strcmp(g_paths[i], path) == 0) present = true;
                if (!present) { problems++; COL(VGA_COLOR_LIGHT_RED); print("  MISSING   "); COL(WHITE); printr("%s\n", path); }
            }
        }
        p = *end ? end + 1 : end;
    }
    return problems;
}

void integrity_command(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "init") == 0) { shell_set_status(integrity_init()); return; }
    if (argc >= 2 && strcmp(argv[1], "check") == 0) {
        bool quiet = argc > 2 && strcmp(argv[2], "-q") == 0;
        int problems = integrity_check(quiet);
        if (problems < 0) {
            if (!quiet) { ERR("integrity: no record yet - run `integrity init` on a system you trust\n"); }
            shell_set_status(quiet ? 0 : 1);
            return;
        }
        if (problems == 0) { if (!quiet) { COL(VGA_COLOR_LIGHT_GREEN); print("integrity: everything matches the record\n"); COL(WHITE); } }
        else ERR("integrity: %d change%s since the record (`integrity init` accepts them)\n", problems, problems == 1 ? "" : "s");
        shell_set_status(problems ? 1 : 0);
        return;
    }
    ERR("Usage: integrity init | check [-q]\n");
}

// ---- secaudit --------------------------------------------------------------------------------------

static bool file_has(const char *path, const char *needle)
{
    static char buf[4096];
    int size = avfs_get_filesize(path);
    if (size <= 0 || size >= (int)sizeof(buf) || avfs_get_content(path, buf, sizeof(buf)) != 0) return false;
    return strstr(buf, needle) != NULL;
}

static bool vault_magic(const char *path)
{
    char head[8];
    return avfs_get_filesize(path) > 24 && avfs_read_file(path, head, 8, 0) == 0 && memcmp(head, "PRPVLT01", 8) == 0;
}

void secaudit_command(int argc, char *argv[])
{
    (void)argc; (void)argv;
    report_t r = { 0, 0, 0, false };
    char text[120];

    COL(VGA_COLOR_LIGHT_CYAN);
    print("RadiumOS security audit\n");
    COL(WHITE);

    line(&r, sudo_is_configured() ? 0 : 1, "sudo password",
         sudo_is_configured() ? (sudo_is_unlocked() ? "set (currently unlocked: sudo -k locks it)" : "set, locked") : "none: privileged commands are open to anyone - run cpw");

    const char *src = kernel_boot_source();
    bool from_disk = strcmp(src, "disk") == 0 || strcmp(src, "old") == 0;
    if (update_is_unverified())
        line(&r, from_disk ? 2 : 1, "update trust", from_disk ? "(unverified) kernel from an unsigned update is what is running" : "(unverified) unsigned files were installed from an update");
    else
        line(&r, 0, "update trust", from_disk ? "running a kernel installed by a signed update" : "built-in kernel, no unsigned update installed");

    const char *url = shell_getenv("UPDATE_URL");
    const char *dsrc = shell_getenv("UPDATE_SRC");
    if (url && strncmp(url, "http://", 7) == 0) line(&r, 1, "update source", "UPDATE_URL is plain http (signatures still protect the package, not privacy)");
    else if (dsrc && strncmp(dsrc, "http://", 7) == 0) line(&r, 1, "update source", "UPDATE_SRC is plain http");
    else line(&r, 0, "update source", "default or https");

    const char *api = shell_getenv("DISCORD_API");
    line(&r, api && api[0] && strncmp(api, "https://", 8) != 0 ? 1 : 0, "discord endpoint",
         api && api[0] && strncmp(api, "https://", 8) != 0 ? "DISCORD_API points at a non-https address" : "default (https)");

    const char *rc = shell_rc_path();
    if (avfs_file_exists(rc) && (file_has(rc, "fetch ") || file_has(rc, "http://")))
        line(&r, 1, "startup script", "the startup script downloads or contacts a server on every boot");
    else
        line(&r, 0, "startup script", avfs_file_exists(rc) ? "present, makes no network calls" : "none");

    static const char *const SECRETS[] = { "/disk/discord.tok", "/disk/ytm.vlt" };
    for (size_t i = 0; i < sizeof(SECRETS) / sizeof(SECRETS[0]); i++) {
        if (!avfs_file_exists(SECRETS[i])) continue;
        snprintf(text, sizeof(text), "%s is %s", SECRETS[i], vault_magic(SECRETS[i]) ? "PRP-sealed" : "NOT sealed (plain text?)");
        line(&r, vault_magic(SECRETS[i]) ? 0 : 2, "stored secret", text);
    }

    int problems = integrity_check(true);
    if (problems < 0) line(&r, 1, "file integrity", "no record: run `integrity init` on a system you trust");
    else if (problems) { snprintf(text, sizeof(text), "%d watched file(s) changed - run `integrity check`", problems); line(&r, 2, "file integrity", text); }
    else line(&r, 0, "file integrity", "all watched files match the record");

    uint32_t rnd;
    bool has_rdrand = cpu_rdrand32(&rnd) != 0;
    line(&r, has_rdrand ? 0 : 1, "random numbers", has_rdrand ? "hardware RDRAND available" : "no RDRAND: keys use a timer-based fallback");
    bool selftest_ok = rust_prp_selftest() == 0;
    line(&r, selftest_ok ? 0 : 2, "crypto self-test", selftest_ok ? "PRP (ChaCha20-Poly1305, X25519, Ed25519, SHA-2) passed" : "FAILED - do not trust encrypted data");

    printr("\n%d ok, %d warning%s, %d failure%s\n", r.ok, r.warn, r.warn == 1 ? "" : "s", r.fail, r.fail == 1 ? "" : "s");
    shell_set_status(r.fail ? 2 : (r.warn ? 1 : 0));
}

// ---- sha256sum / prp -------------------------------------------------------------------------------

static bool read_key(const char *arg, const char *magic, uint8_t key[32])
{
    char path[512];
    uint8_t blob[39];
    mv_resolve(arg, path);
    if (avfs_get_filesize(path) != 39 || avfs_read_file(path, blob, 39, 0) != 0 || memcmp(blob, magic, 7) != 0) return false;
    memcpy(key, blob + 7, 32);
    wipe(blob, sizeof(blob));
    return true;
}

static int hash_files(int argc, char *argv[], int first)
{
    int bad = 0;
    for (int i = first; i < argc; i++) {
        char path[512], text[65];
        uint8_t digest[32];
        mv_resolve(argv[i], path);
        if (rust_prp_sha256_file((const uint8_t *)path, digest) != 0) { ERR("sha256sum: %s: cannot read\n", argv[i]); bad = 1; continue; }
        hex(digest, 32, text);
        printr("%s  %s\n", text, argv[i]);
    }
    return bad;
}

void sha256sum_command(int argc, char *argv[])
{
    if (argc < 2) { ERR("Usage: sha256sum FILE...\n"); return; }
    shell_set_status(hash_files(argc, argv, 1));
}

static void prp_usage(void)
{
    ERR("Usage: prp hash FILE...\n"
        "       prp keygen NAME                 writes NAME.prv (secret) and NAME.pub\n"
        "       prp fingerprint KEY.pub|KEY.prv\n"
        "       prp sign FILE KEY.prv           appends a PRPSIG1 signature to FILE\n"
        "       prp verify FILE [KEY.pub]       checks it (and that KEY.pub signed it)\n"
        "       prp seal FILE OUT RECIPIENT.pub / prp open FILE OUT KEY.prv\n");
}

void prp_command(int argc, char *argv[])
{
    if (argc < 2) { prp_usage(); return; }
    const char *verb = argv[1];
    char a[512], b[512];
    uint8_t key[32];
    int rc = 1;

    if (strcmp(verb, "hash") == 0) {
        if (argc < 3) { prp_usage(); return; }
        shell_set_status(hash_files(argc, argv, 2));
        return;
    }
    if (strcmp(verb, "keygen") == 0 && argc == 3) {
        mv_resolve(argv[2], a);
        rc = rust_prp_keygen((const uint8_t *)a);
        if (rc == 0) { COL(VGA_COLOR_LIGHT_GREEN); printr("wrote %s.prv (keep it secret) and %s.pub\n", argv[2], argv[2]); COL(WHITE); }
        else ERR("prp keygen: failed (%d)\n", rc);
    } else if (strcmp(verb, "fingerprint") == 0 && argc == 3) {
        uint8_t digest[32];
        char text[65];
        mv_resolve(argv[2], a);
        rc = rust_prp_fingerprint((const uint8_t *)a, digest);
        if (rc == 0) { hex(digest, 32, text); printr("%s\n", text); }
        else ERR("prp fingerprint: not a PRP key file (%d)\n", rc);
    } else if (strcmp(verb, "sign") == 0 && argc == 4) {
        mv_resolve(argv[2], a);
        if (!read_key(argv[3], "PRPPRV1", key)) { ERR("prp sign: %s is not a PRP private key\n", argv[3]); shell_set_status(1); return; }
        rc = rust_prp_sign((const uint8_t *)a, key);
        wipe(key, sizeof(key));
        if (rc == 0) { COL(VGA_COLOR_LIGHT_GREEN); printr("signed %s\n", argv[2]); COL(WHITE); }
        else ERR("prp sign: %s\n", rc == -5 ? "already signed" : "failed");
    } else if (strcmp(verb, "verify") == 0 && (argc == 3 || argc == 4)) {
        mv_resolve(argv[2], a);
        if (argc == 4 && !read_key(argv[3], "PRPPUB1", key)) { ERR("prp verify: %s is not a PRP public key\n", argv[3]); shell_set_status(1); return; }
        rc = rust_prp_verify((const uint8_t *)a, argc == 4 ? key : NULL);
        if (rc == 0) { COL(VGA_COLOR_LIGHT_GREEN); print("signature OK\n"); COL(WHITE); }
        else if (rc == -6) ERR("not signed (or something was added after the signature)\n");
        else if (rc == -8) ERR("signed, but by a different key\n");
        else if (rc == -7) ERR("BAD signature - the file was changed after signing\n");
        else ERR("prp verify: could not read the file (%d)\n", rc);
    } else if (strcmp(verb, "seal") == 0 && argc == 5) {
        mv_resolve(argv[2], a); mv_resolve(argv[3], b);
        if (!read_key(argv[4], "PRPPUB1", key)) { ERR("prp seal: %s is not a PRP public key\n", argv[4]); shell_set_status(1); return; }
        rc = rust_prp_seal_file_pub((const uint8_t *)a, (const uint8_t *)b, key);
        if (rc == 0) { COL(VGA_COLOR_LIGHT_GREEN); printr("sealed %s -> %s\n", argv[2], argv[3]); COL(WHITE); }
        else ERR("prp seal: failed (%d)\n", rc);
    } else if (strcmp(verb, "open") == 0 && argc == 5) {
        mv_resolve(argv[2], a); mv_resolve(argv[3], b);
        if (!read_key(argv[4], "PRPPRV1", key)) { ERR("prp open: %s is not a PRP private key\n", argv[4]); shell_set_status(1); return; }
        rc = rust_prp_open_file_prv((const uint8_t *)a, (const uint8_t *)b, key);
        wipe(key, sizeof(key));
        if (rc == 0) { COL(VGA_COLOR_LIGHT_GREEN); printr("opened %s -> %s\n", argv[2], argv[3]); COL(WHITE); }
        else ERR("prp open: wrong key or damaged file (%d)\n", rc);
    } else {
        prp_usage();
    }
    shell_set_status(rc == 0 ? 0 : 1);
}

// ---- vault -----------------------------------------------------------------------------------------

#define VAULT_DIR "/disk/vault"
static char g_in1[COMMAND_BUFFER_SIZE], g_in2[COMMAND_BUFFER_SIZE];

static bool vault_name_ok(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len > 8) return false;
    for (; *n; n++)
        if (!((*n >= 'a' && *n <= 'z') || (*n >= 'A' && *n <= 'Z') || (*n >= '0' && *n <= '9') || *n == '_' || *n == '-')) return false;
    return true;
}

static void ask(const char *prompt, char *out)
{
    COL(VGA_COLOR_LIGHT_CYAN); print(prompt); COL(WHITE);
    keyboard_input_secure(out);
}

void vault_command(int argc, char *argv[])
{
    char path[64];
    if (argc >= 2 && strcmp(argv[1], "list") == 0) {
        static DirectoryEntry entries[32];
        int n = avfs_is_directory(VAULT_DIR) ? avfs_list_directory(VAULT_DIR, entries, 32) : 0, shown = 0;
        for (int i = 0; i < n; i++) {
            size_t len = strlen(entries[i].name);
            if (entries[i].is_directory || len < 5 || strcmp(entries[i].name + len - 4, ".vlt") != 0) continue;
            printr("  %s\n", entries[i].name);
            shown++;
        }
        if (!shown) print("vault: empty\n");
        return;
    }
    if (argc != 3 || !vault_name_ok(argv[2])) {
        ERR("Usage: vault set NAME | get NAME | rm NAME | list      (NAME: up to 8 letters, digits, _ -)\n");
        return;
    }
    snprintf(path, sizeof(path), "%s/%s.vlt", VAULT_DIR, argv[2]);

    if (strcmp(argv[1], "set") == 0) {
        if (!avfs_is_directory("/disk")) { ERR("vault: /disk is not mounted\n"); return; }
        if (!avfs_is_directory(VAULT_DIR)) avfs_create_dir(VAULT_DIR);
        ask("Secret (hidden): ", g_in1);
        if (g_in1[0] == '\0' || strlen(g_in1) > 500) { ERR("vault: the secret must be 1 to 500 characters\n"); wipe(g_in1, sizeof(g_in1)); return; }
        ask("Passphrase (hidden): ", g_in2);
        size_t pl = strlen(g_in2);
        if (pl < 6 || pl > 100) { ERR("vault: use a passphrase of 6 to 100 characters\n"); wipe(g_in1, sizeof(g_in1)); wipe(g_in2, sizeof(g_in2)); return; }
        char again[COMMAND_BUFFER_SIZE];
        ask("Passphrase again: ", again);
        bool same = strcmp(again, g_in2) == 0;
        wipe(again, sizeof(again));
        int rc = same ? rust_prp_vault_save((const uint8_t *)g_in2, (const uint8_t *)g_in1, (uint32_t)strlen(g_in1), (const uint8_t *)path) : 99;
        wipe(g_in1, sizeof(g_in1)); wipe(g_in2, sizeof(g_in2));
        if (!same) ERR("vault: the passphrases differ, nothing saved\n");
        else if (rc != 0) ERR("vault: could not save (%d)\n", rc);
        else { COL(VGA_COLOR_LIGHT_GREEN); printr("vault: %s sealed in %s\n", argv[2], path); COL(WHITE); }
        return;
    }
    if (strcmp(argv[1], "get") == 0) {
        if (!avfs_file_exists(path)) { ERR("vault: no secret named %s\n", argv[2]); return; }
        ask("Passphrase (hidden): ", g_in2);
        int n = rust_prp_vault_load((const uint8_t *)g_in2, (const uint8_t *)path, (uint8_t *)g_in1, sizeof(g_in1) - 1);
        wipe(g_in2, sizeof(g_in2));
        if (n > 0) { g_in1[n] = '\0'; printr("%s\n", g_in1); }
        else ERR("vault: %s\n", n == -2 ? "wrong passphrase" : "the vault file is damaged");
        wipe(g_in1, sizeof(g_in1));
        return;
    }
    if (strcmp(argv[1], "rm") == 0) {
        if (!avfs_file_exists(path)) { ERR("vault: no secret named %s\n", argv[2]); return; }
        avfs_remove_file(path);
        print("vault: removed\n");
        return;
    }
    ERR("Usage: vault set NAME | get NAME | rm NAME | list\n");
}

// ---- shred -----------------------------------------------------------------------------------------

void shred_command(int argc, char *argv[])
{
    if (argc < 2) { ERR("Usage: shred FILE...\n"); return; }
    static uint8_t block[4096];
    for (int i = 1; i < argc; i++) {
        char path[512];
        mv_resolve(argv[i], path);
        int size = avfs_get_filesize(path);
        if (size < 0 || avfs_is_directory(path)) { ERR("shred: %s: not a file\n", argv[i]); shell_set_status(1); continue; }
        bool ok = true;
        for (int pass = 0; pass < 3 && ok; pass++) {
            for (uint32_t off = 0; off < (uint32_t)size; off += sizeof(block)) {
                uint32_t n = (uint32_t)size - off < sizeof(block) ? (uint32_t)size - off : (uint32_t)sizeof(block);
                if (pass == 0) memset(block, 0x00, n);
                else if (pass == 1) memset(block, 0xFF, n);
                else rust_prp_random(block, n);
                if (avfs_write_file(path, block, n, off) != 0) { ok = false; break; }
            }
        }
        wipe(block, sizeof(block));
        if (ok && avfs_remove_file(path) == 0) printr("shred: %s overwritten 3 times and removed\n", argv[i]);
        else { ERR("shred: %s: could not overwrite it\n", argv[i]); shell_set_status(1); }
    }
}

// ---- genpw -----------------------------------------------------------------------------------------

void genpw_command(int argc, char *argv[])
{
    static const char SET[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";   // 64: every random byte maps evenly
    int length = argc > 1 ? atoi(argv[1]) : 20;
    int count = argc > 2 ? atoi(argv[2]) : 1;
    if (length < 8 || length > 128 || count < 1 || count > 20) { ERR("Usage: genpw [LENGTH 8-128] [COUNT 1-20]\n"); return; }
    for (int c = 0; c < count; c++) {
        uint8_t raw[128];
        char out[129];
        rust_prp_random(raw, (uint32_t)length);
        for (int i = 0; i < length; i++) out[i] = SET[raw[i] & 63];
        out[length] = '\0';
        printr("%s\n", out);
        wipe(raw, sizeof(raw));
        wipe(out, sizeof(out));
    }
}
