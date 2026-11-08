// update.c - update RadiumOS without rebuilding it
//
//   update                  show the running build, where it booted from and what is installed
//   update check [SOURCE]   list what an update package contains (nothing is changed)
//   update apply [SOURCE]   copy the package onto /disk after checking every file's SHA-256
//   update install [SOURCE] apply, then reboot into the new kernel after a short countdown
//   update rollback         remove the updated kernel from /disk; the built-in kernel boots again
//   update rollback previous  put the kernel that was installed before the last update back
//
// SOURCE is a folder (default /disk/update, or $UPDATE_SRC), an http:// or https:// address of a folder that
// holds manifest.txt, or the word `online` (the project's release page, or $UPDATE_URL). Remote files are
// downloaded with the fetch engine (TLS, redirects, any size up to the disk) into /disk/update first, then
// verified and installed from there. Upload the contents of dist/update (make update-package) to
//   https://radium.global/update/        manifest.txt and the F0001.BIN files
// A folder source needs no network:
//   host:   make update-disk        writes dist/update/ into fetchdisk.img as /disk/update/
//   RadiumOS: update apply && reboot
//
// manifest.txt:
//   radiumos-update 1
//   version <text>
//   file <sha256> <size> <target path under /disk> <file name in the package> [keep]
// "keep" installs the file only when the target does not exist yet (for files the user edits).
//
// Signing: manifest.txt carries a PRP signature trailer (tools/prp_sign.py, `make update-package`). The manifest holds
// every file's SHA-256, so the signature covers the whole package, kernel included. A signature that does not check
// out against the key built into the kernel (update_key.h) is always refused. An online package without a signature is
// refused unless --unsigned is given; any unsigned install marks the system "(unverified)" (yellow text down the right
// edge of the screen on every boot) until a signed package that carries a kernel is installed or the kernel is rolled back.
//
// The kernel is installed as /disk/radium/kernel.bin; the one it replaces is kept as kernel.old. The GRUB menu on
// the ISO boots kernel.bin when present and falls back to the built-in kernel if it does not load; hold Shift at
// power-on for the menu, where the previous and built-in kernels are under "Advanced options".

#include <stdint.h>
#include <stdbool.h>
#include "update.h"
#include "shellvars.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../http/http.h"
#include "../kernel/createFiles.h"
#include "../timers/tsc.h"
#include "../io/io.h"
#include "fsapp.h"
#include "reboot.h"
#include "update_key.h"
#include "../vga/fbcon.h"
#include "../timers/date.h"

extern int rust_prp_sha256(const uint8_t *data, uint32_t len, uint8_t *output);
extern int rust_prp_verify_mem(const uint8_t *data, uint32_t len, const uint8_t *trusted);
extern int rust_fetch(int argc, const uint8_t *const *argv);
extern int rust_disk_store_size(const uint8_t *path);
extern int rust_disk_store_read(const uint8_t *path, uint32_t offset, uint8_t *buffer, uint32_t len);
extern int rust_disk_store_remove(const uint8_t *path);
extern const char *kernel_boot_source(void);

#define UPDATE_DEFAULT_SRC  "/disk/update"
#define UPDATE_ONLINE_SRC   "https://radium.global/update"
#define UPDATE_KERNEL_PATH  "/disk/radium/kernel.bin"
#define UPDATE_OLD_KERNEL   "/disk/radium/kernel.old"
#define UPDATE_CHUNK        (32u * 1024u)
#define UPDATE_MAX_ENTRIES  48
#define UPDATE_VERSION_FILE "/disk/radium/version.txt"   // version of the last installed package
#ifndef RADIUM_VERSION
#define RADIUM_VERSION "0"
#endif
#define UPDATE_BUF_MAX      (10u * 1024u * 1024u)
#define UPDATE_MANIFEST_MAX 8192

typedef struct {
    char     sha[65];
    uint32_t size;
    char     target[96];
    char     name[40];
    bool     keep;
} entry_t;

static entry_t  g_entries[UPDATE_MAX_ENTRIES];
static int      g_count;
static char     g_version[48];
static char     g_manifest[UPDATE_MANIFEST_MAX + 1];
static uint8_t  g_buf[UPDATE_BUF_MAX];
static uint8_t  g_chunk[UPDATE_CHUNK];
static bool     g_quiet;   // no progress lines (the boot-time check)
static bool     g_signed;  // the manifest just loaded carries a valid PRP signature

#define UPDATE_UNVERIFIED_FILE "/disk/radium/unverif.txt"
#define UPDATE_LASTCHK_FILE    "/disk/radium/lastchk.txt"   // unix time of the last boot-time check
#define UPDATE_CHECK_EVERY_S   3600u
#define SIG_TRAILER_LEN        103   // "PRPSIG1" + public key + R + S

#define OK(...)   do { terminal_setcolor(VGA_COLOR_LIGHT_GREEN); printr(__VA_ARGS__); terminal_setcolor(VGA_COLOR_WHITE); } while (0)
#define ERR(...)  do { terminal_setcolor(VGA_COLOR_LIGHT_RED);   printr(__VA_ARGS__); terminal_setcolor(VGA_COLOR_WHITE); } while (0)
#define DIM(...)  do { terminal_setcolor(VGA_COLOR_LIGHT_GREY);  printr(__VA_ARGS__); terminal_setcolor(VGA_COLOR_WHITE); } while (0)

static bool is_url(const char *s) { return strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0; }

// Package file names become file names under /disk/update, so keep them plain.
static bool name_ok(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len > 12) return false;
    for (const char *p = n; *p; p++) {
        bool good = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
                    *p == '.' || *p == '_' || *p == '-';
        if (!good || (p[0] == '.' && p[1] == '.')) return false;
    }
    return true;
}

static void join_path(char *out, size_t size, const char *base, const char *name)
{
    size_t n = strlen(base);
    if (n + 1 + strlen(name) + 1 > size) { out[0] = '\0'; return; }
    strcpy(out, base);
    if (n > 0 && out[n - 1] != '/') strcat(out, "/");
    strcat(out, name);
}

// ---- fetching ----------------------------------------------------------------------

static uint32_t disk_free_kb(void)
{
    avfs_disk_info_t info;
    return (avfs_disk_query(&info) == 0 && info.mounted) ? info.free_kb : 0;
}

// /disk is the small FAT volume (about 15 MB); downloads are staged in the fetch store that fills the rest of the
// disk (fetch -o /disk/NAME), so a download is only refused when it does not fit on the whole disk.
static int download(const char *src, const char *name)
{
    char url[220], dest[64];
    join_path(url, sizeof(url), src, name);
    join_path(dest, sizeof(dest), UPDATE_DEFAULT_SRC, name);
    if (url[0] == '\0' || dest[0] == '\0') return -1;

    const uint8_t *args[] = {
        (const uint8_t *)"fetch", (const uint8_t *)"-L", (const uint8_t *)"-f", (const uint8_t *)"-s",
        (const uint8_t *)"--overwrite", (const uint8_t *)"--timeout", (const uint8_t *)(g_quiet ? "4" : "120"),
        (const uint8_t *)"-o", (const uint8_t *)dest, (const uint8_t *)url,
    };
    if (!g_quiet) DIM("  get   %s\n", url);
    if (rust_fetch((int)(sizeof(args) / sizeof(args[0])), args) != 0 || rust_disk_store_size((const uint8_t *)dest) < 0) {
        rust_disk_store_remove((const uint8_t *)dest);
        return -7;
    }
    return 0;
}

// Reads a staged download into g_buf. 0 on success.
static int read_staged(const char *name, uint32_t *len)
{
    char path[64];
    join_path(path, sizeof(path), UPDATE_DEFAULT_SRC, name);
    int size = rust_disk_store_size((const uint8_t *)path);
    if (size < 0) return -1;
    if ((uint32_t)size > sizeof(g_buf)) return -5;
    for (uint32_t off = 0; off < (uint32_t)size; ) {
        uint32_t n = (uint32_t)size - off < UPDATE_CHUNK ? (uint32_t)size - off : UPDATE_CHUNK;
        if (rust_disk_store_read((const uint8_t *)path, off, g_buf + off, n) != (int)n) return -6;
        off += n;
    }
    *len = (uint32_t)size;
    return 0;
}

static void drop_staged(const char *name)
{
    char path[64];
    join_path(path, sizeof(path), UPDATE_DEFAULT_SRC, name);
    if (path[0]) rust_disk_store_remove((const uint8_t *)path);
}

// 0 on success; the file is left in g_buf with its length in *len.
static int fetch(const char *src, const char *name, uint32_t *len)
{
    if (!name_ok(name)) return -1;
    if (is_url(src)) {
        int rc = download(src, name);
        return rc != 0 ? rc : read_staged(name, len);
    }

    char where[220];
    join_path(where, sizeof(where), src, name);
    if (where[0] == '\0') return -1;

    int size = avfs_get_filesize(where);
    if (size < 0) return -1;
    if ((uint32_t)size > sizeof(g_buf)) return -5;
    if (size > 0 && avfs_read_file(where, g_buf, (uint32_t)size, 0) != 0) return -6;
    *len = (uint32_t)size;
    return 0;
}

static const char *fetch_error(int rc)
{
    switch (rc) {
    case -1: return "not found";
    case -5: return "file too large for this source";
    case -7: return "download failed (see the fetch message above)";
    default: return "read error";
    }
}

// ---- manifest ----------------------------------------------------------------------

static void sha_hex(const uint8_t *data, uint32_t len, char out[65])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t digest[32];
    rust_prp_sha256(data, len, digest);
    for (int i = 0; i < 32; i++) { out[i * 2] = hex[digest[i] >> 4]; out[i * 2 + 1] = hex[digest[i] & 15]; }
    out[64] = '\0';
}

static bool target_ok(const char *t)
{
    if (strncmp(t, "/disk/", 6) != 0 || t[6] == '\0') return false;
    for (const char *p = t; *p; p++) {
        if (p[0] == '.' && p[1] == '.') return false;
        if ((uint8_t)*p < 32) return false;
    }
    return t[strlen(t) - 1] != '/';
}

static int next_word(const char **p, char *out, size_t size)
{
    while (**p == ' ' || **p == '\t') (*p)++;
    size_t n = 0;
    while (**p && **p != ' ' && **p != '\t') { if (n + 1 < size) out[n++] = **p; (*p)++; }
    out[n] = '\0';
    return (int)n;
}

static uint32_t parse_uint(const char *s)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}

// Reads and parses manifest.txt from `src`. Returns 0 or a negative error (message already printed).
static int load_manifest(const char *src, bool allow_unsigned)
{
    uint32_t len = 0;
    int rc = fetch(src, "manifest.txt", &len);
    if (rc != 0) { ERR("update: cannot read manifest.txt from %s: %s\n", src, fetch_error(rc)); return -1; }

    g_signed = false;
    if (len >= SIG_TRAILER_LEN && memcmp(g_buf + len - SIG_TRAILER_LEN, "PRPSIG1", 7) == 0) {
        int vr = rust_prp_verify_mem(g_buf, len, UPDATE_TRUSTED_KEY);
        if (vr != 0) {
            ERR("update: REFUSED - the PRP signature on manifest.txt is %s\n",
                vr == -8 ? "from a key this system does not trust" : "invalid (the package was altered)");
            return -1;
        }
        g_signed = true;
        len -= SIG_TRAILER_LEN;
    }
    if (g_signed) {
        OK("Signature: PRP verified (key %02x%02x%02x%02x...)\n", UPDATE_TRUSTED_KEY[0], UPDATE_TRUSTED_KEY[1],
           UPDATE_TRUSTED_KEY[2], UPDATE_TRUSTED_KEY[3]);
    } else if (is_url(src) && !allow_unsigned) {
        ERR("update: REFUSED - online updates must be signed with PRP and this package is not.\n");
        DIM("        (--unsigned installs it anyway and marks the system (unverified))\n");
        return -1;
    } else {
        terminal_setcolor(VGA_COLOR_LIGHT_BROWN);
        print("Signature: NONE - this package is unverified\n");
        terminal_setcolor(VGA_COLOR_WHITE);
    }

    if (len > UPDATE_MANIFEST_MAX) { ERR("update: manifest.txt is too large\n"); return -1; }
    memcpy(g_manifest, g_buf, len);
    g_manifest[len] = '\0';

    g_count = 0;
    g_version[0] = '\0';
    bool header = false;

    char *line = g_manifest;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n') end++;
        bool last = (*end == '\0');
        *end = '\0';
        size_t n = strlen(line);
        if (n > 0 && line[n - 1] == '\r') line[n - 1] = '\0';

        const char *p = line;
        char word[100];
        if (next_word(&p, word, sizeof(word)) > 0 && word[0] != '#') {
            if (strcmp(word, "radiumos-update") == 0) {
                header = true;
            } else if (strcmp(word, "version") == 0) {
                next_word(&p, g_version, sizeof(g_version));
            } else if (strcmp(word, "file") == 0) {
                if (g_count >= UPDATE_MAX_ENTRIES) { ERR("update: too many files in the manifest\n"); return -1; }
                entry_t *e = &g_entries[g_count];
                char size_s[16], flag[16];
                if (next_word(&p, e->sha, sizeof(e->sha)) != 64 || next_word(&p, size_s, sizeof(size_s)) == 0 ||
                    next_word(&p, e->target, sizeof(e->target)) == 0 || next_word(&p, e->name, sizeof(e->name)) == 0) {
                    ERR("update: bad manifest line: %s\n", line);
                    return -1;
                }
                e->size = parse_uint(size_s);
                e->keep = (next_word(&p, flag, sizeof(flag)) > 0 && strcmp(flag, "keep") == 0);
                if (!target_ok(e->target)) { ERR("update: refusing target %s (must be a file under /disk/)\n", e->target); return -1; }
                if (!name_ok(e->name)) { ERR("update: refusing package file name %s\n", e->name); return -1; }
                g_count++;
            }
        }
        if (last) break;
        line = end + 1;
    }

    if (!header) { ERR("update: manifest.txt is not a RadiumOS update manifest\n"); return -1; }
    return 0;
}

// ---- installing --------------------------------------------------------------------

static void make_parent_dirs(const char *target)
{
    char path[100];
    strncpy(path, target, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    for (char *p = path + 6; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (!avfs_is_directory(path)) avfs_create_dir(path);
        *p = '/';
    }
}

static bool is_kernel(const entry_t *e) { return strcmp(e->target, UPDATE_KERNEL_PATH) == 0; }

// Copies a file piece by piece (g_buf holds the update being installed, so it cannot be used here). 0 on success.
static int copy_file_chunked(const char *from, const char *to)
{
    int size = avfs_get_filesize(from);
    if (size < 0) return -1;
    if (avfs_file_exists(to)) avfs_remove_file(to);
    if (avfs_create_file(to, 0) != 0) return -2;

    for (uint32_t off = 0; off < (uint32_t)size; off += UPDATE_CHUNK) {
        uint32_t n = (uint32_t)size - off < UPDATE_CHUNK ? (uint32_t)size - off : UPDATE_CHUNK;
        if (avfs_read_file(from, g_chunk, n, off) != 0) { avfs_remove_file(to); return -3; }
        if (avfs_append_file(to, g_chunk, n) != 0) { avfs_remove_file(to); return -4; }
    }
    if (avfs_get_filesize(to) != size) { avfs_remove_file(to); return -5; }
    return 0;
}

static bool looks_like_kernel(const uint8_t *data, uint32_t len)
{
    if (len < 4096 || data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') return false;
    for (uint32_t off = 0; off + 12 <= len && off < 8192; off += 4) {
        if (data[off] == 0x02 && data[off + 1] == 0xB0 && data[off + 2] == 0xAD && data[off + 3] == 0x1B) return true;
    }
    return false;
}

// Returns 1 installed, 0 skipped, -1 failed.
static int install_entry(const char *src, const entry_t *e, bool dry_run)
{
    if (e->keep && avfs_file_exists(e->target)) {
        DIM("  keep  %s (already there)\n", e->target);
        return 0;
    }

    uint32_t len = 0;
    int rc = fetch(src, e->name, &len);
    if (rc != 0) { ERR("  FAIL  %s: %s: %s\n", e->target, e->name, fetch_error(rc)); return -1; }
    if (e->size != 0 && len != e->size) { ERR("  FAIL  %s: size %u, manifest says %u\n", e->target, len, e->size); return -1; }

    char sha[65];
    sha_hex(g_buf, len, sha);
    if (strcmp(sha, e->sha) != 0) { ERR("  FAIL  %s: SHA-256 does not match the manifest\n", e->target); return -1; }

    if (is_kernel(e) && !looks_like_kernel(g_buf, len)) {
        ERR("  FAIL  %s: not a multiboot kernel image\n", e->target);
        return -1;
    }

    if (dry_run) {
        OK("  ok    %s (%u bytes)\n", e->target, len);
        return 1;
    }

    make_parent_dirs(e->target);
    if (is_kernel(e) && strcmp(src, UPDATE_DEFAULT_SRC) == 0) {
        // The kernel is in memory now; drop the staged copy so the backup below fits on /disk.
        char staged[64];
        join_path(staged, sizeof(staged), UPDATE_DEFAULT_SRC, e->name);
        if (staged[0] && avfs_file_exists(staged)) avfs_remove_file(staged);
    }
    if (is_kernel(e) && avfs_file_exists(UPDATE_KERNEL_PATH)) {
        if (copy_file_chunked(UPDATE_KERNEL_PATH, UPDATE_OLD_KERNEL) == 0) DIM("  keep  previous kernel as %s\n", UPDATE_OLD_KERNEL);
        else ERR("  warn  could not keep the previous kernel (disk full?); continuing without it\n");
    }
    if (avfs_save_file(e->target, g_buf, len, true) != 0 || avfs_get_filesize(e->target) != (int)len) {
        ERR("  FAIL  %s: could not write it (disk full or not mounted?)\n", e->target);
        return -1;
    }
    OK("  done  %s (%u bytes)\n", e->target, len);
    return 1;
}

static const char *default_source(void)
{
    const char *env = shell_getenv("UPDATE_SRC");
    return (env && env[0]) ? env : UPDATE_DEFAULT_SRC;
}

// ---- the "(unverified)" marker --------------------------------------------------------------------

static void draw_unverified(fbcon_canvas_t *c, uint32_t t_ms, void *arg)
{
    (void)t_ms; (void)arg;
    static const char text[] = "(unverified)";
    fbc_rect(c, 0, 0, c->w, c->h, 0x000000, 190);
    for (int i = 0; text[i]; i++) fbc_glyph(c, 0, 2 + i * 10, (uint8_t)text[i], 0xFFFF55, 1, 255);
}

bool update_is_unverified(void)
{
    return avfs_file_exists(UPDATE_UNVERIFIED_FILE);
}

// Shows or removes the yellow vertical "(unverified)" overlay to match the marker file. Safe to call any time.
void update_refresh_marker(void)
{
    if (!fbcon_enabled()) return;
    if (update_is_unverified()) fbcon_overlay_add("!unverified", FBCON_WIDTH - 8, 16, 8, 124, draw_unverified, NULL);
    else fbcon_overlay_remove("!unverified");
}

static void set_unverified(bool on)
{
    if (on && !update_is_unverified()) {
        make_parent_dirs(UPDATE_UNVERIFIED_FILE);
        avfs_save_file(UPDATE_UNVERIFIED_FILE, "unsigned\n", 9, true);
    } else if (!on && update_is_unverified()) {
        avfs_remove_file(UPDATE_UNVERIFIED_FILE);
    }
    update_refresh_marker();
}

static const char *online_source(void)
{
    const char *env = shell_getenv("UPDATE_URL");
    return (env && env[0]) ? env : UPDATE_ONLINE_SRC;
}

// Removes the staged copies once they are installed (or verified) so they do not look like a waiting package.
// remote: the downloads in the fetch store; otherwise the package folder on /disk.
static void clean_staging(bool remote)
{
    char path[64];
    for (int i = 0; i < g_count; i++) {
        if (remote) { drop_staged(g_entries[i].name); continue; }
        join_path(path, sizeof(path), UPDATE_DEFAULT_SRC, g_entries[i].name);
        if (path[0] && avfs_file_exists(path)) avfs_remove_file(path);
    }
    if (remote) { drop_staged("manifest.txt"); return; }
    join_path(path, sizeof(path), UPDATE_DEFAULT_SRC, "manifest.txt");
    if (avfs_file_exists(path)) avfs_remove_file(path);
}

static void do_status(void)
{
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("RadiumOS update status\n");
    terminal_setcolor(VGA_COLOR_WHITE);

    printr("  Running build   %s\n", build_stamp());
    printr("  Version         v%s\n", update_current_version());
    printr("  Booted from     %s kernel\n", strcmp(kernel_boot_source(), "disk") == 0 ? "the updated (disk)" :
                                          (strcmp(kernel_boot_source(), "old") == 0 ? "the previous (disk)" : "the built-in (ISO)"));

    int ksize = avfs_get_filesize(UPDATE_KERNEL_PATH);
    if (ksize >= 0) printr("  Disk kernel     %s (%d bytes) - boots next time\n", UPDATE_KERNEL_PATH, ksize);
    else            printr("  Disk kernel     none installed\n");
    int osize = avfs_get_filesize(UPDATE_OLD_KERNEL);
    if (osize >= 0) printr("  Previous kernel %s (%d bytes) - GRUB: Advanced options\n", UPDATE_OLD_KERNEL, osize);
    printr("  Free on /disk   %u KB\n", disk_free_kb());
    if (update_is_unverified()) {
        terminal_setcolor(VGA_COLOR_LIGHT_BROWN);
        print("  Trust           (unverified) - an unsigned package was installed; a signed update clears this\n");
        terminal_setcolor(VGA_COLOR_WHITE);
    } else {
        DIM("  Trust           no unsigned package installed\n");
    }

    if (!avfs_is_directory("/disk")) {
        terminal_setcolor(VGA_COLOR_LIGHT_BROWN);
        print("  /disk is not mounted, updates cannot be installed\n");
        terminal_setcolor(VGA_COLOR_WHITE);
    }

    const char *src = default_source();
    printr("  Default source  %s%s\n", src, avfs_file_exists(UPDATE_DEFAULT_SRC "/manifest.txt") ? "  (package waiting)" : "");
    printr("  Online source   %s\n", online_source());
    DIM("  Commands: update check|apply|install [SOURCE|online] [--unsigned] | update rollback [previous]\n");
}

// Counts down so the user can cancel with a key press (key releases are ignored); true if the countdown ran out.
static bool reboot_countdown(int seconds)
{
    print("Rebooting into the new kernel in ");
    for (int s = seconds; s > 0; s--) {
        printr("%d... ", s);
        if (fs_wait(1000)) { print("cancelled\n"); return false; }
    }
    print("\n");
    return true;
}

// "v5.2.1" -> {5, 2, 1, 0}. False unless it is up to four dot-separated numbers.
static bool parse_version(const char *s, uint32_t out[4])
{
    int part = 0;
    for (int i = 0; i < 4; i++) out[i] = 0;
    if (*s == 'v' || *s == 'V') s++;
    if (*s < '0' || *s > '9') return false;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') { if (out[part] < 100000) out[part] = out[part] * 10 + (uint32_t)(*s - '0'); }
        else if (*s == '.' && part < 3 && s[1] >= '0' && s[1] <= '9') part++;
        else return false;
    }
    return true;
}

static int version_cmp(const uint32_t a[4], const uint32_t b[4])
{
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    return 0;
}

// The newer of the built-in version and the last package installed (a package can be newer than the running kernel).
static void current_version(char *text, size_t size, uint32_t out[4])
{
    char have[48] = "";
    uint32_t installed[4];
    parse_version(RADIUM_VERSION, out);
    strncpy(text, RADIUM_VERSION, size - 1);
    text[size - 1] = '\0';
    if (!avfs_file_exists(UPDATE_VERSION_FILE) || avfs_get_content(UPDATE_VERSION_FILE, have, sizeof(have) - 1) != 0) return;
    have[sizeof(have) - 1] = '\0';
    size_t n = strlen(have);
    while (n > 0 && (have[n - 1] == '\n' || have[n - 1] == '\r' || have[n - 1] == ' ')) have[--n] = '\0';
    if (parse_version(have, installed) && version_cmp(installed, out) > 0) {
        for (int i = 0; i < 4; i++) out[i] = installed[i];
        strncpy(text, have[0] == 'v' || have[0] == 'V' ? have + 1 : have, size - 1);
        text[size - 1] = '\0';
    }
}

const char *update_current_version(void)
{
    static char text[48];
    uint32_t parts[4];
    current_version(text, sizeof(text), parts);
    return text;
}

// Boot-time check: one small request for the online manifest. True (with its version in `version`) when that
// version is newer than the current one. Prints nothing; false on any failure.
bool update_online_available(char *version, size_t size)
{
    uint32_t len = 0;
    char online[48] = "", current[48];
    uint32_t on[4], cur[4];

    // Boot must not wait on the network every time: at most one check an hour (failed ones count too).
    uint32_t now = get_unix_timestamp(), last = 0;
    char stamp[16] = "";
    if (avfs_file_exists(UPDATE_LASTCHK_FILE) && avfs_get_content(UPDATE_LASTCHK_FILE, stamp, sizeof(stamp) - 1) == 0) {
        stamp[sizeof(stamp) - 1] = '\0';
        last = parse_uint(stamp);
    }
    if (now != 0 && last != 0 && now >= last && now - last < UPDATE_CHECK_EVERY_S) return false;
    if (now != 0) {
        char text[16];
        snprintf(text, sizeof(text), "%u\n", now);
        make_parent_dirs(UPDATE_LASTCHK_FILE);
        avfs_save_file(UPDATE_LASTCHK_FILE, text, strlen(text), true);
    }

    g_quiet = true;
    int rc = fetch(online_source(), "manifest.txt", &len);
    g_quiet = false;
    drop_staged("manifest.txt");
    if (rc != 0 || len == 0) return false;

    const char *text = (const char *)g_buf;
    for (uint32_t i = 0; i + 8 < len; i++) {
        if (strncmp(text + i, "version ", 8) != 0 || (i > 0 && text[i - 1] != '\n')) continue;
        size_t n = 0;
        for (i += 8; i < len && text[i] != '\n' && text[i] != '\r' && n + 1 < sizeof(online); i++) online[n++] = text[i];
        online[n] = '\0';
        break;
    }
    if (!parse_version(online, on)) return false;

    current_version(current, sizeof(current), cur);
    if (version_cmp(on, cur) <= 0) return false;
    strncpy(version, online[0] == 'v' || online[0] == 'V' ? online + 1 : online, size - 1);
    version[size - 1] = '\0';
    return true;
}

void update_command(int argc, char *argv[])
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) { do_status(); return; }

    bool install = strcmp(argv[1], "install") == 0;
    bool apply = strcmp(argv[1], "apply") == 0 || install;
    bool check = strcmp(argv[1], "check") == 0;

    if (strcmp(argv[1], "rollback") == 0) {
        if (argc > 2 && strcmp(argv[2], "previous") == 0) {
            if (!avfs_file_exists(UPDATE_OLD_KERNEL)) { ERR("update: there is no previous kernel (%s)\n", UPDATE_OLD_KERNEL); return; }
            if (copy_file_chunked(UPDATE_OLD_KERNEL, UPDATE_KERNEL_PATH) != 0) { ERR("update: could not restore the previous kernel\n"); return; }
            OK("Restored the previous kernel as %s. Reboot to run it.\n", UPDATE_KERNEL_PATH);
            return;
        }
        if (!avfs_file_exists(UPDATE_KERNEL_PATH)) { ERR("update: no updated kernel is installed\n"); return; }
        avfs_remove_file(UPDATE_KERNEL_PATH);
        set_unverified(false);
        OK("Removed %s. The built-in kernel boots next time.\n", UPDATE_KERNEL_PATH);
        return;
    }

    if (!apply && !check) {
        ERR("Usage: update [status] | check [SOURCE] | apply [SOURCE] | install [SOURCE] | rollback [previous]\n");
        return;
    }
    bool allow_unsigned = false;
    const char *src = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--unsigned") == 0) allow_unsigned = true;
        else if (!src) src = argv[i];
        else { ERR("Usage: update %s [SOURCE] [--unsigned]\n", argv[1]); return; }
    }
    if (apply && !avfs_is_directory("/disk")) { ERR("update: /disk is not mounted\n"); return; }

    if (!src) src = default_source();
    if (strcmp(src, "online") == 0) src = online_source();
    DIM("Source: %s\n", src);
    if (load_manifest(src, allow_unsigned) != 0) return;

    printr("Package version %s, %d file%s\n", g_version[0] ? g_version : "(unnamed)", g_count, g_count == 1 ? "" : "s");

    int installed = 0, skipped = 0;
    bool kernel_changed = false;
    for (int i = 0; i < g_count; i++) {
        int r = install_entry(src, &g_entries[i], check);
        if (r < 0) {
            ERR("Update stopped at %s; files before it were installed.\n", g_entries[i].target);
            return;
        }
        if (r == 1) { installed++; if (is_kernel(&g_entries[i])) kernel_changed = true; }
        else skipped++;
    }

    if (check) {
        OK("All %d file%s verified. Run `update apply` with the same source to install.\n", installed, installed == 1 ? "" : "s");
        if (is_url(src)) clean_staging(true);
        return;
    }

    if (is_url(src)) clean_staging(true);
    else if (strcmp(src, UPDATE_DEFAULT_SRC) == 0) clean_staging(false);

    OK("Installed %d file%s, kept %d.\n", installed, installed == 1 ? "" : "s", skipped);
    if (!g_signed) set_unverified(true);
    else if (kernel_changed) set_unverified(false);
    if (g_version[0]) {
        make_parent_dirs(UPDATE_VERSION_FILE);
        avfs_save_file(UPDATE_VERSION_FILE, g_version, (uint32_t)strlen(g_version), true);
    }
    if (kernel_changed) {
        terminal_setcolor(VGA_COLOR_LIGHT_BROWN);
        print("New kernel installed: reboot to run it. If it fails to start, hold Shift at power-on and pick the\n");
        print("built-in or previous kernel under Advanced options, then run `update rollback`.\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        if (install && reboot_countdown(5)) {
            char *args[] = { "reboot", NULL };
            reboot_command(1, args);
        }
    } else if (installed > 0) {
        print("Run `rrc` or `bootpage` to pick up a changed startup script.\n");
    } else if (install) {
        print("Nothing new was installed, so there is no need to reboot.\n");
    }
}
