// selftest.c - selftest: regression checks for the security features, run inside the guest
//
// Every result line is also written to the QEMU debug port (0xE9) as "SELFTEST name PASS|FAIL detail" and the run ends with
// "SELFTEST RESULT PASS|FAIL passed/total", so tools/test/run_tests.py can read it from -debugcon. With `selftest` on the
// kernel command line the checks run at boot and the VM powers off afterwards (see boot_finish).

#include <stdint.h>
#include <stdbool.h>
#include "selftest.h"
#include "apprun.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../keyboard/keyboard.h"
#include "../kernel/stackguard.h"
#include "../user/users.h"
#include "../io/io.h"

extern int rust_prp_selftest(void);
extern int rust_crypt_selftest(void);
extern int rust_crypt_file(const uint8_t *src, const uint8_t *dst, const uint8_t *password, int mode);
extern int rust_crypt_probe(const uint8_t *path);
extern int rust_pw_hash(const uint8_t *password, uint8_t *out, uint32_t cap);
extern int rust_pw_verify(const uint8_t *password, const uint8_t *record);
extern int rust_prp_random(uint8_t *output, uint32_t len);

static int g_pass, g_total;

static void dbg(const char *s) { for (; *s; s++) outb(0xE9, (uint8_t)*s); }

static void report(const char *name, bool ok, const char *detail)
{
    char line[160];
    g_total++;
    if (ok) g_pass++;
    snprintf(line, sizeof(line), "SELFTEST %s %s %s\n", name, ok ? "PASS" : "FAIL", detail ? detail : "");
    dbg(line);
    terminal_setcolor(ok ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_RED);
    printr("  %s  %s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
    terminal_setcolor(VGA_COLOR_WHITE);
}

static void report_code(const char *name, int code)
{
    char d[24];
    snprintf(d, sizeof(d), "code=%d", code);
    report(name, code == 0, code ? d : "");
}

static bool bytes_equal(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return false;
    return true;
}

static void test_canary(void)
{
    uint32_t g = stackguard_value();
    report("stack-canary-randomised", g != 0x000A0DFFu && g != 0 && (g & 0xFF) == 0, "");

    // The guard really protects this frame: a protected function built with the same flags calls __stack_chk_fail; the host
    // test (run_tests.py) also checks the call sites in the binary. Here: two reads agree and differ from a stale value.
    volatile char buf[32];
    for (int i = 0; i < 32; i++) buf[i] = (char)i;
    report("stack-canary-stable", stackguard_value() == g && buf[31] == 31, "");
}

static void test_crypto(void)
{
    report_code("prp-selftest", rust_prp_selftest());
    report_code("account-hash-selftest", rust_crypt_selftest());

    uint8_t a[128], b[128];
    bool ok = rust_pw_hash((const uint8_t *)"same-password", a, sizeof(a)) == 0 && rust_pw_hash((const uint8_t *)"same-password", b, sizeof(b)) == 0 &&
              !bytes_equal(a, b, 40) && rust_pw_verify((const uint8_t *)"same-password", a) == 0 && rust_pw_verify((const uint8_t *)"same-password", b) == 0;
    report("account-hash-salted", ok, "");
}

static void test_crypt_files(void)
{
    if (!avfs_is_directory("/disk")) { report("crypt-files", false, "no /disk"); return; }
    avfs_remove_dir_recursive("/disk/crypttst");
    avfs_create_dir("/disk/crypttst");
    static uint8_t plain[3000], got[3000];
    rust_prp_random(plain, sizeof(plain));
    const char *f = "/disk/crypttst/data.bin";
    if (avfs_save_file(f, plain, sizeof(plain), true) != 0) { report("crypt-files", false, "cannot write test file"); return; }

    avfs_wipe_freed(true);
    int rc = rust_crypt_file((const uint8_t *)f, (const uint8_t *)f, (const uint8_t *)"correct horse battery", 0);
    avfs_wipe_freed(false);
    report("crypt-lock", rc == 0 && rust_crypt_probe((const uint8_t *)f) == 1 && avfs_get_filesize(f) == (int)sizeof(plain) + 56, "");

    avfs_read_file(f, got, sizeof(plain), 0);
    report("crypt-hides-plaintext", !bytes_equal(got, plain, 64), "");
    rc = rust_crypt_file((const uint8_t *)f, (const uint8_t *)f, (const uint8_t *)"correct horse battery", 0);
    { char d[24]; snprintf(d, sizeof(d), "rc=%d", rc); report("crypt-no-double-lock", rc == -5, d); }

    report("crypt-wrong-passphrase", rust_crypt_file((const uint8_t *)f, (const uint8_t *)f, (const uint8_t *)"wrong horse battery!", 1) == -6 &&
                                     rust_crypt_probe((const uint8_t *)f) == 1, "file must stay locked");

    // flip one ciphertext byte: the Poly1305 tag must reject it
    uint8_t one;
    avfs_read_file(f, &one, 1, 1000);
    uint8_t flipped = (uint8_t)(one ^ 0x01);
    static uint8_t whole[3052];
    avfs_read_file(f, whole, sizeof(whole), 0);
    whole[1000] = flipped;
    avfs_save_file("/disk/crypttst/bad.bin", whole, sizeof(whole), true);
    report("crypt-detects-tampering", rust_crypt_file((const uint8_t *)"/disk/crypttst/bad.bin", (const uint8_t *)"/disk/crypttst/out.bin",
                                                       (const uint8_t *)"correct horse battery", 1) == -6 && !avfs_file_exists("/disk/crypttst/out.bin"), "");

    rc = rust_crypt_file((const uint8_t *)f, (const uint8_t *)f, (const uint8_t *)"correct horse battery", 1);
    memset(got, 0, sizeof(got));
    avfs_read_file(f, got, sizeof(plain), 0);
    report("crypt-unlock-roundtrip", rc == 0 && rust_crypt_probe((const uint8_t *)f) == 0 && avfs_get_filesize(f) == (int)sizeof(plain) &&
                                     bytes_equal(got, plain, sizeof(plain)), "");

    avfs_save_file("/disk/crypttst/empty.bin", NULL, 0, true);
    rc = rust_crypt_file((const uint8_t *)"/disk/crypttst/empty.bin", (const uint8_t *)"/disk/crypttst/empty.bin", (const uint8_t *)"another passphrase", 0);
    int rc2 = rust_crypt_file((const uint8_t *)"/disk/crypttst/empty.bin", (const uint8_t *)"/disk/crypttst/empty.bin", (const uint8_t *)"another passphrase", 1);
    report("crypt-empty-file", rc == 0 && rc2 == 0 && avfs_get_filesize("/disk/crypttst/empty.bin") == 0, "");
    avfs_remove_dir_recursive("/disk/crypttst");
}

void selftest_command(int argc, char *argv[])
{
    (void)argc; (void)argv;
    g_pass = g_total = 0;
    dbg("SELFTEST BEGIN\n");
    print("Running the security self tests...\n");

    test_canary();
    test_crypto();
    report_code("accounts-and-sudo-policy", users_selftest());
    report_code("app-capabilities", app_selftest());
    int fs = avfs_fsck_selftest();
    if (fs == 1) report("fsck-and-safe-writes", false, "no /disk");
    else report_code("fsck-and-safe-writes", fs);
    test_crypt_files();

    char line[64];
    snprintf(line, sizeof(line), "SELFTEST RESULT %s %d/%d\n", g_pass == g_total ? "PASS" : "FAIL", g_pass, g_total);
    dbg(line);
    terminal_setcolor(g_pass == g_total ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_RED);
    printr("%d of %d checks passed\n", g_pass, g_total);
    terminal_setcolor(VGA_COLOR_WHITE);
    if (g_pass != g_total) shell_set_status(1);
}
