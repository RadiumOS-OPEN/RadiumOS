#include "../errors/error.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../keyboard/keyboard.h"
#include "../rawr/rawr.h"
#include "../Avfs/Avfs.h"
#include "../utility/utility.h"


#include "../commands/mempop.h"
#include "../commands/brainz.h"
#include "../commands/clear.h"
#include "../commands/echo.h"
#include "../commands/exit.h"
#include "../commands/reboot.h"
#include "../commands/help.h"
#include "../commands/text.h"
#include "../commands/meow.h"
#include "../commands/rm.h"
#include "../commands/cat.h"
#include "../commands/settings.h"
#include "../commands/ls.h"
#include "../commands/tui.h"
#include "../commands/radifetch.h"
#include "../commands/cowsay.h"
#include "../commands/gogetter.h"
#include "../commands/cd.h"
#include "../commands/text.h"

#include <stdint.h>

// ============================================================
// Rust script engine entry points
// ============================================================
extern int32_t script_execute_file(const uint8_t *path);
extern int32_t script_execute_line_c(const uint8_t *line);
extern void    script_run_autoexec(void);
extern void    script_init(void);

// ============================================================
// Task management
// ============================================================
extern void     rust_list_tasks(void);
extern int32_t  rust_kill_task(uint32_t pid);
extern int32_t  rust_task_info(uint32_t pid);
extern uint32_t rust_get_task_count(void);
extern int32_t  rust_killall_tasks(void);

// ============================================================
// Notifications / image editor
// ============================================================
extern int rust_send_ntfy_notification(const uint8_t *message);
extern int rust_ntfy_post_complete(const uint8_t *message, uint32_t message_len);
extern int rust_image_editor(void);

// ============================================================
// Network / DNS / TCP
// ============================================================
extern void rust_set_dns(uint8_t dns1, uint8_t dns2, uint8_t dns3, uint8_t dns4);
extern int  rust_test_dns_direct(void);
extern int  rust_tcp_force_reset(void);
extern int  rust_test_network_simple(void);
extern int  rust_test_raw_send(void);
extern int  rust_network_diag(void);
extern int32_t net_raw_tcp_send(int32_t argc, const uint8_t *const *argv);
extern int32_t net_wol(int32_t argc, const uint8_t *const *argv);

// ============================================================
// Browser
// ============================================================
extern int graphical_browser(void);

// ============================================================
// JSON
// ============================================================
extern int rust_test_json(void);

// ============================================================
// Discord core
// ============================================================
extern int rust_discord_set_token(const uint8_t *token);
extern int rust_discord_get_user_info(void);
extern int rust_discord_get_guilds(void);
extern int rust_discord_get_channels(const uint8_t *guild_id);
extern int rust_discord_get_channel_messages(const uint8_t *channel_id, int32_t limit);
extern int rust_discord_send_message(const uint8_t *channel_id, const uint8_t *message);
extern int rust_discord_send_embed(const uint8_t *channel_id,
                                   const uint8_t *title,
                                   const uint8_t *description,
                                   uint32_t color);
extern int rust_discord_react(const uint8_t *channel_id,
                              const uint8_t *message_id,
                              const uint8_t *emoji);
extern int rust_discord_delete_message(const uint8_t *channel_id,
                                       const uint8_t *message_id);
extern int rust_discord_shell(const uint8_t *channel_id);
extern int rust_discord_dump_cache(void);
extern int rust_test_discord(void);

// ============================================================
// Discord module system v2
// ============================================================
extern int rust_discord_set_module(const uint8_t *name);
extern int rust_discord_config_module(const uint8_t *name);
extern int rust_discord_run_module(const uint8_t *name);
extern int rust_discord_list_modules(void);
extern int rust_discord_remove_module(const uint8_t *name);
extern int rust_discord_clone_module(const uint8_t *src, const uint8_t *dst);
extern int rust_discord_tag_module(const uint8_t *name, const uint8_t *tag);
extern int rust_discord_module_help(const uint8_t *name);

// ============================================================
// Misc externals
// ============================================================
extern int  download_simple(const char *url, const char *filename);
extern void files_browse(void);
extern void watchdog_diagram();
extern int32_t cmd_cat_hexx(const uint8_t *filename);

// ============================================================
// AES tracing
// ============================================================
extern void aes_trace_on(void);
extern void aes_trace_off(void);
extern void aes_trace_flip(void);
extern int  aes_trace_query(void);

// ============================================================
// AES encryption / decryption
// ============================================================
extern int rust_aes_init(const uint8_t *key);
extern int rust_aes_encrypt(uint8_t *data, uint32_t len);
extern int rust_aes_decrypt(uint8_t *data, uint32_t len);
extern int rust_aes_encrypt_file(const uint8_t *filename);
extern int rust_aes_decrypt_file(const uint8_t *filename);

// ===== PRP =====
extern int rust_prp_selftest(void);
extern int rust_prp_sha256_file(const uint8_t *filename, uint8_t *output);
extern int rust_prp_random(uint8_t *output, uint32_t len);
extern int rust_prp_seal_file(const uint8_t *input, const uint8_t *output, const uint8_t *key_hex);
extern int rust_prp_open_file(const uint8_t *input, const uint8_t *output, const uint8_t *key_hex);
extern int rust_prp_seal_text(const uint8_t *text, uint32_t len, const uint8_t *key_hex, const uint8_t *output);
extern int rust_prp_seal_text_pub(const uint8_t *text, uint32_t len, const uint8_t *recipient_pub, const uint8_t *output);
extern int rust_prp_seal_file_pub(const uint8_t *input, const uint8_t *output, const uint8_t *recipient_pub);
extern int rust_prp_open_file_prv(const uint8_t *input, const uint8_t *output, const uint8_t *private_key);
extern int rust_prp_keygen(const uint8_t *name);
extern int rust_prp_sign(const uint8_t *file, const uint8_t *private_key);
extern int rust_prp_verify(const uint8_t *file, const uint8_t *expected_pub);
extern int rust_prp_fingerprint(const uint8_t *keyfile, uint8_t *output);

#define DISCORD_BOT_TOKEN \
    "YouTHOUGHTT(add your token here then launch OS)-but server will start approx in 2 or 4 months :p"

// Convenience macro -- reset TCP state after every Discord network call
#define DISCORD_CALL(expr) do { (expr); rust_tcp_force_reset(); } while(0)

// ============================================================
// Helpers
// ============================================================
static int32_t simple_atoi(const char *str)
{
    int32_t result = 0;
    int sign = 1;
    if (*str == '-') { sign = -1; str++; }
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return result * sign;
}

static void join_args(char *dest, int dest_size, char **argv, int start, int argc)
{
    int offset = 0;
    for (int i = start; i < argc && offset < dest_size - 1; i++) {
        if (i > start && offset < dest_size - 1) dest[offset++] = ' ';
        for (int j = 0; argv[i][j] != '\0' && offset < dest_size - 1; j++)
            dest[offset++] = argv[i][j];
    }
    dest[offset] = '\0';
}

// ===== PRP commands =====
void cmd_prp_test(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (rust_prp_selftest() == 0) {
        terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
        print("PRP self-test passed.\n");
    } else {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("PRP self-test failed.\n");
    }
    terminal_setcolor(VGA_COLOR_WHITE);
}

static void prp_print_fingerprint(const char *keyfile)
{
    uint8_t digest[32];
    if (rust_prp_fingerprint((const uint8_t *)keyfile, digest) != 0) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Unknown key format.\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }

    static const char digits[] = "0123456789abcdef";
    terminal_setcolor(VGA_COLOR_LIGHT_BROWN);
    for (int i = 0; i < 4; i++) {
        if (i > 0) print(" ");
        char group[5];
        group[0] = digits[digest[i * 2] >> 4];
        group[1] = digits[digest[i * 2] & 0x0f];
        group[2] = digits[digest[i * 2 + 1] >> 4];
        group[3] = digits[digest[i * 2 + 1] & 0x0f];
        group[4] = '\0';
        print(group);
    }
    print("\n");
    terminal_setcolor(VGA_COLOR_WHITE);
}

void cmd_prp_fingerprint(int argc, char *argv[])
{
    if (argc != 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print("Usage: prp fingerprint <keyfile>\n");
        return;
    }

    prp_print_fingerprint(argv[1]);
}

static int prp_derive_name(char *dest, int dest_size, const char *src, int encrypt)
{
    int len = 0;
    while (src[len] != '\0') len++;

    if (encrypt) {
        if (len + 4 >= dest_size) return -1;
        for (int i = 0; i < len; i++) dest[i] = src[i];
        dest[len] = '.'; dest[len + 1] = 'p'; dest[len + 2] = 'r'; dest[len + 3] = 'p';
        dest[len + 4] = '\0';
        return 0;
    }

    if (len <= 4 || src[len - 4] != '.' || src[len - 3] != 'p' ||
        src[len - 2] != 'r' || src[len - 1] != 'p') {
        return -1;
    }
    if (len - 4 >= dest_size) return -1;
    for (int i = 0; i < len - 4; i++) dest[i] = src[i];
    dest[len - 4] = '\0';
    return 0;
}

static int prp_load_key(const char *path, char *hex)
{
    int size = avfs_get_filesize((const char *)path);
    if (size < 64 || size > 66) {
        return -1;
    }
    if (avfs_read_file(path, hex, 64, 0) != 0) {
        return -1;
    }
    hex[64] = '\0';

    for (int i = 0; i < 64; i++) {
        char c = hex[i];
        int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!ok) return -1;
    }
    return 0;
}

static int prp_read_raw_key(const char *path, const char *magic, uint8_t *key)
{
    char head[7];
    if (avfs_get_filesize((const char *)path) < 39) return -1;
    if (avfs_read_file(path, head, 7, 0) != 0) return -1;
    if (memcmp(head, magic, 7) != 0) return -1;
    if (avfs_read_file(path, key, 32, 7) != 0) return -1;
    return 0;
}

static int prp_flag(int argc, char *argv[], const char *short_flag, const char *long_flag)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], short_flag) == 0 || strcmp(argv[i], long_flag) == 0) {
            return 1;
        }
    }
    return 0;
}

static int prp_input_arg(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0 ||
            strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            continue;
        }
        return i;
    }
    return -1;
}

static void prp_usage(int encrypt)
{
    if (encrypt) {
        print("Usage: prp encrypt <file> <keyfile>\n");
        print("       prp encrypt \"text\" <keyfile>\n");
    } else {
        print("Usage: prp decrypt <file.prp> <keyfile>\n");
    }
    print("       keyfile: 64 hex characters, a .pub key (encrypt),\n");
    print("                or a .prv key (decrypt)\n");
    print("       -v, --verbose   show what was detected and done\n");
    print("       -h, --help      show this help\n");
}

static void prp_verbose_key_type(const char *path)
{
    int size = avfs_get_filesize((const char *)path);
    char head[8];
    int head_len = (size >= 7) ? 7 : size;
    if (head_len > 0 && avfs_read_file(path, head, head_len, 0) != 0) head_len = 0;

    print("  key type: ");
    if (head_len == 7 && memcmp(head, "PRPPUB1", 7) == 0) {
        print("public key");
    } else if (head_len == 7 && memcmp(head, "PRPPRV1", 7) == 0) {
        print("private key");
    } else if (size >= 64 && size <= 66) {
        print("symmetric key");
    } else {
        print("unknown");
    }
    print(" (");
    print(path);
    print(")\n");
}

static void prp_run_file_op(int encrypt, int argc, char *argv[])
{
    if (prp_flag(argc, argv, "-h", "--help")) {
        prp_usage(encrypt);
        return;
    }
    int verbose = prp_flag(argc, argv, "-v", "--verbose");

    int in = prp_input_arg(argc, argv);
    if (argc < 3 || in < 0 || in >= argc - 1) {
        prp_usage(encrypt);
        return;
    }

    char key_hex[65];
    int symmetric = 1;
    uint8_t raw_key[32];

    if (prp_load_key(argv[argc - 1], key_hex) == 0) {
        if (verbose) prp_verbose_key_type(argv[argc - 1]);
    } else if (prp_read_raw_key(argv[argc - 1], "PRPPUB1", raw_key) == 0 && encrypt) {
        symmetric = 0;
        if (verbose) {
            print("  key type: public key (");
            print(argv[argc - 1]);
            print(")\n");
        }
    } else if (prp_read_raw_key(argv[argc - 1], "PRPPRV1", raw_key) == 0 && !encrypt) {
        symmetric = 0;
        if (verbose) {
            print("  key type: private key (");
            print(argv[argc - 1]);
            print(")\n");
        }
    } else {
        print("Key file must be 64 hex characters or a PRP key file.\n");
        prp_usage(encrypt);
        return;
    }

    if (!encrypt) {
        char derived[128];
        if (prp_derive_name(derived, sizeof(derived), argv[in], 0) != 0) {
            print("Not a .prp file.\n");
            prp_usage(encrypt);
            return;
        }
        int result;
        if (symmetric) {
            result = rust_prp_open_file((const uint8_t *)argv[in], (const uint8_t *)derived, (const uint8_t *)key_hex);
        } else {
            result = rust_prp_open_file_prv((const uint8_t *)argv[in], (const uint8_t *)derived, raw_key);
        }
        if (verbose) {
            char head[4];
            print("  input: ");
            print(argv[in]);
            if (avfs_get_filesize((const char *)argv[in]) >= 4 &&
                avfs_read_file(argv[in], head, 4, 0) == 0 && memcmp(head, "PRP1", 4) == 0) {
                print(" (PRP1, ChaCha20-Poly1305)");
            }
            print("\n  output: ");
            print(derived);
            print("\n");
        }
        switch (result) {
        case 0:
            terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
            print("Decrypted to ");
            print(derived);
            print("\n");
            break;
        case -4:
            print("Could not create output file. Does it already exist?\n");
            break;
        case -6:
            print("Not a PRP file.\n");
            break;
        case -7:
            print("Authentication failed. Wrong key or modified file.\n");
            break;
        default:
            print("Could not process file.\n");
            break;
        }
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }

    if (avfs_file_exists(argv[in])) {
        char derived[128];
        if (prp_derive_name(derived, sizeof(derived), argv[in], 1) != 0) {
            print("File name too long.\n");
            prp_usage(encrypt);
            return;
        }
        if (avfs_file_exists(derived)) avfs_remove_file(derived);
        if (verbose) {
            print("  input: ");
            print(argv[in]);
            print("\n  output: ");
            print(derived);
            print("\n  cipher: ");
            if (symmetric) {
                print("ChaCha20-Poly1305\n");
            } else {
                print("X25519 + ChaCha20-Poly1305\n");
            }
        }
        int result;
        if (symmetric) {
            result = rust_prp_seal_file((const uint8_t *)argv[in], (const uint8_t *)derived, (const uint8_t *)key_hex);
        } else {
            result = rust_prp_seal_file_pub((const uint8_t *)argv[in], (const uint8_t *)derived, raw_key);
        }
        if (result == 0) {
            terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
            print("Encrypted to ");
            print(derived);
            print("\n");
        } else {
            print("Could not process file.\n");
        }
        terminal_setcolor(VGA_COLOR_WHITE);
    } else {
        if (argv[in][0] != '"') {
            print("File not found.\n");
            prp_usage(encrypt);
            return;
        }

        static char text[513];
        int len = 0;
        int too_long = 0;
        for (int i = in; i < argc - 1 && !too_long; i++) {
            const char *word = argv[i];
            int start = (i == in) ? 1 : 0;
            for (int j = start; word[j] != '\0' && word[j] != '"'; j++) {
                if (len >= 512) { too_long = 1; break; }
                text[len++] = word[j];
            }
            if (!too_long && i < argc - 2) {
                if (len >= 512) { too_long = 1; break; }
                text[len++] = ' ';
            }
        }
        text[len] = '\0';

        if (too_long) {
            print("Input too long (max 512 bytes). Use a file instead.\n");
            terminal_setcolor(VGA_COLOR_WHITE);
            return;
        }

        if (verbose) {
            char count[11];
            int value = len;
            int pos = 0;
            do { count[pos++] = '0' + value % 10; value /= 10; } while (value > 0);
            count[pos] = '\0';
            for (int i = 0; i < pos / 2; i++) {
                char t = count[i]; count[i] = count[pos - 1 - i]; count[pos - 1 - i] = t;
            }
            print("  input: ");
            print(count);
            print(" bytes of text\n  output: text.prp\n  cipher: ChaCha20-Poly1305\n");
        }

        if (avfs_file_exists("text.prp")) avfs_remove_file("text.prp");
        int result;
        if (symmetric) {
            result = rust_prp_seal_text((const uint8_t *)text, len, (const uint8_t *)key_hex, (const uint8_t *)"text.prp");
        } else {
            result = rust_prp_seal_text_pub((const uint8_t *)text, len, raw_key, (const uint8_t *)"text.prp");
        }
        if (result == 0) {
            terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
            print("Encrypted to text.prp\n");
        } else {
            print("Could not encrypt text.\n");
        }
        terminal_setcolor(VGA_COLOR_WHITE);
    }
}

void cmd_prp_encrypt(int argc, char *argv[])
{
    prp_run_file_op(1, argc, argv);
}

void cmd_prp_decrypt(int argc, char *argv[])
{
    prp_run_file_op(0, argc, argv);
}

void cmd_prp_keygen(int argc, char *argv[])
{
    if (argc != 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print("Usage: prp keygen <name>\n");
        print("       creates <name>.prv (keep secret) and <name>.pub (share)\n");
        return;
    }

    if (rust_prp_keygen((const uint8_t *)argv[1]) == 0) {
        terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
        print("Keypair generated: ");
        print(argv[1]);
        print(".prv and ");
        print(argv[1]);
        print(".pub\n");
    } else {
        print("Could not generate keypair.\n");
    }
    terminal_setcolor(VGA_COLOR_WHITE);
}

void cmd_prp_sign(int argc, char *argv[])
{
    if (argc != 3 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print("Usage: prp sign <file> <key.prv>\n");
        print("       appends a PRPSIG1 trailer to the file\n");
        return;
    }

    uint8_t prv[32];
    if (prp_read_raw_key(argv[2], "PRPPRV1", prv) != 0) {
        print("Key file must be a .prv key.\n");
        return;
    }

    int result = rust_prp_sign((const uint8_t *)argv[1], prv);
    switch (result) {
    case 0:
        terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
        print("Signed ");
        print(argv[1]);
        print("\n");
        break;
    case -5:
        print("File is already signed.\n");
        break;
    default:
        print("Could not sign file (code ");
        print_decimal(result);
        print(").\n");
        break;
    }
    terminal_setcolor(VGA_COLOR_WHITE);
}

void cmd_prp_verify(int argc, char *argv[])
{
    if (argc < 2 || argc > 3 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print("Usage: prp verify <file> [key.pub]\n");
        return;
    }

    uint8_t want_pub[32];
    const uint8_t *want_ptr = 0;
    if (argc >= 3) {
        if (prp_read_raw_key(argv[2], "PRPPUB1", want_pub) != 0) {
            print("Key file must be a .pub key.\n");
            return;
        }
        want_ptr = want_pub;
    }

    int result = rust_prp_verify((const uint8_t *)argv[1], want_ptr);
    switch (result) {
    case 0:
        terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
        print("Signature valid.\n");
        break;
    case -6:
        print("No signature found.\n");
        break;
    case -7:
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Signature INVALID.\n");
        break;
    case -8:
        print("Signed by a different key.\n");
        break;
    default:
        print("Could not verify file.\n");
        break;
    }
    terminal_setcolor(VGA_COLOR_WHITE);
}

void cmd_prp_hash(int argc, char *argv[])
{
    if (argc < 2) {
        print("Usage: prp hash <file>\n");
        return;
    }

    uint8_t digest[32];
    int result = rust_prp_sha256_file((const uint8_t *)argv[1], digest);
    if (result == -1) {
        print("File not found.\n");
        return;
    }
    if (result != 0) {
        print("Could not read file.\n");
        return;
    }

    static const char digits[] = "0123456789abcdef";
    char hex[65];
    for (int i = 0; i < 32; i++) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    hex[64] = '\0';

    print(hex);
    print("\n");
}

void cmd_prp_random(int argc, char *argv[])
{
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print("Usage: prp random [bytes]\n");
        print("       1 to 256 bytes, default 32 (one PRP key)\n");
        return;
    }

    static uint8_t random[256];
    int len = 32;

    if (argc > 2) {
        print("Usage: prp random [bytes]\n");
        print("       1 to 256 bytes, default 32 (one PRP key)\n");
        return;
    }
    if (argc == 2) {
        len = simple_atoi(argv[1]);
        if (len < 1 || len > (int)sizeof(random)) {
            print("Usage: prp random [bytes]\n");
            print("       1 to 256 bytes, default 32 (one PRP key)\n");
            return;
        }
    }

    if (rust_prp_random(random, len) != 0) {
        print("Secure randomness unavailable.\n");
        return;
    }

    static const char digits[] = "0123456789abcdef";
    char hex[513];
    for (int i = 0; i < len; i++) {
        hex[i * 2] = digits[random[i] >> 4];
        hex[i * 2 + 1] = digits[random[i] & 0x0f];
    }
    hex[len * 2] = '\0';

    print(hex);
    print("\n");
}

void cmd_prp_keyinfo(int argc, char *argv[])
{
    if (argc != 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print("Usage: prp keyinfo <keyfile>\n");
        return;
    }

    int size = avfs_get_filesize(argv[1]);
    if (size < 0) {
        print("File not found.\n");
        return;
    }

    char head[7];
    int head_len = (size >= 7) ? 7 : size;
    if (head_len > 0 && avfs_read_file(argv[1], head, head_len, 0) != 0) {
        print("Could not read file.\n");
        return;
    }

    terminal_setcolor(VGA_COLOR_WHITE);
    if (head_len == 7 && memcmp(head, "PRPPUB1", 7) == 0) {
        print("Public key\n  fingerprint: ");
        prp_print_fingerprint(argv[1]);
    } else if (head_len == 7 && memcmp(head, "PRPPRV1", 7) == 0) {
        print("Private key\n  fingerprint: ");
        prp_print_fingerprint(argv[1]);
    } else if (size >= 64 && size <= 66) {
        char probe[65];
        if (avfs_read_file(argv[1], probe, 64, 0) != 0) {
            print("Could not read file.\n");
            return;
        }
        probe[64] = '\0';
        int hex_ok = 1;
        for (int i = 0; i < 64; i++) {
            char c = probe[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                hex_ok = 0;
                break;
            }
        }
        if (hex_ok) {
            print("Symmetric key (64 hex characters)\n  fingerprint: ");
            prp_print_fingerprint(argv[1]);
        } else {
            terminal_setcolor(VGA_COLOR_LIGHT_RED);
            print("Unknown key format.\n");
        }
    } else {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Unknown key format.\n");
    }
    terminal_setcolor(VGA_COLOR_WHITE);
}

static void prp_banner(void)
{
    print(" .----------------.  .----------------.  .----------------. \n");
    print("| .--------------. || .--------------. || .--------------. |\n");
    print("| |   ______     | || |  _______     | || |   ______     | |\n");
    print("| |  |_   __ \\   | || | |_   __ \\    | || |  |_   __ \\   | |\n");
    print("| |    | |__) |  | || |   | |__) |   | || |    | |__) |  | |\n");
    print("| |    |  ___/   | || |   |  __ /    | || |    |  ___/   | |\n");
    print("| |   _| |_      | || |  _| |  \\ \\_  | || |   _| |_      | |\n");
    print("| |  |_____|     | || | |____| |___| | || |  |_____|     | |\n");
    print("| |              | || |              | || |              | |\n");
    print("| '--------------' || '--------------' || '--------------' |\n");
    print(" '----------------'  '----------------'  '----------------' \n");
}

void cmd_prp(int argc, char *argv[])
{
    const char *sub = (argc > 1) ? argv[1] : "";

    if (argc < 2 || strcmp(sub, "-h") == 0 || strcmp(sub, "--help") == 0) {
        prp_banner();
        print("\nUsage: prp <command> [args]\n\nCommands:\n");
        print("  encrypt <file|\"text\"> <keyfile>   Encrypt to <file>.prp or text.prp\n");
        print("  decrypt <file.prp> <keyfile>      Decrypt a .prp file\n");
        print("  keygen <name>                     Generate <name>.prv/.pub keypair\n");
        print("  sign <file> <key.prv>             Sign a file in place\n");
        print("  verify <file> [key.pub]           Verify a signed file\n");
        print("  fingerprint <keyfile>             Show a key's fingerprint\n");
        print("  keyinfo <keyfile>                 Identify a key file\n");
        print("  hash <file>                       SHA-256 hash of a file\n");
        print("  random [bytes]                    Print random bytes (default 32)\n");
        print("  test                              Run PRP self-test\n");
        return;
    }

    if (strcmp(sub, "encrypt") == 0) {
        cmd_prp_encrypt(argc - 1, argv + 1);
    } else if (strcmp(sub, "decrypt") == 0) {
        cmd_prp_decrypt(argc - 1, argv + 1);
    } else if (strcmp(sub, "keygen") == 0) {
        cmd_prp_keygen(argc - 1, argv + 1);
    } else if (strcmp(sub, "sign") == 0) {
        cmd_prp_sign(argc - 1, argv + 1);
    } else if (strcmp(sub, "verify") == 0) {
        cmd_prp_verify(argc - 1, argv + 1);
    } else if (strcmp(sub, "fingerprint") == 0) {
        cmd_prp_fingerprint(argc - 1, argv + 1);
    } else if (strcmp(sub, "keyinfo") == 0) {
        cmd_prp_keyinfo(argc - 1, argv + 1);
    } else if (strcmp(sub, "hash") == 0) {
        cmd_prp_hash(argc - 1, argv + 1);
    } else if (strcmp(sub, "random") == 0) {
        cmd_prp_random(argc - 1, argv + 1);
    } else if (strcmp(sub, "test") == 0) {
        cmd_prp_test(argc - 1, argv + 1);
    } else {
        print("Unknown PRP command: ");
        print(sub);
        print("\nType 'prp' for usage.\n");
        prp_banner();
    }
}

// ============================================================
// AES commands
// ============================================================
void cmd_aes_key(int argc, char *argv[])
{
    if (argc < 2) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: aes.key <key_string>\n");
        print("Note: AES keys must be 16, 24, or 32 bytes long.\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Setting AES key...\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    int res = rust_aes_init((const uint8_t *)argv[1]);
    if (res == 0) {
        terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
        print("AES Key initialized successfully.\n");
    } else {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Failed to initialize AES key. Check length (16/24/32 bytes).\n");
    }
    terminal_setcolor(VGA_COLOR_WHITE);
}

void cmd_aes_enc(int argc, char *argv[])
{
    if (argc < 2) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: aes.enc <filename>\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Encrypting file: "); print(argv[1]); print("...\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    int res = rust_aes_encrypt_file((const uint8_t *)argv[1]);
    if (res == 0) {
        terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
        print("Encryption successful.\n");
    } else {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Encryption failed. Did you set a key (aes.key)?\n");
    }
    terminal_setcolor(VGA_COLOR_WHITE);
}

void cmd_aes_dec(int argc, char *argv[])
{
    if (argc < 2) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: aes.dec <filename>\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Decrypting file: "); print(argv[1]); print("...\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    int res = rust_aes_decrypt_file((const uint8_t *)argv[1]);
    if (res == 0) {
        terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
        print("Decryption successful.\n");
    } else {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Decryption failed. Wrong key or corrupt file?\n");
    }
    terminal_setcolor(VGA_COLOR_WHITE);
}

// ============================================================
// AES trace commands
// ============================================================
void cmd_aes_trace_on(int argc, char *argv[])
{
    aes_trace_on();
    terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
    print("AES Tracing: ENABLED\n");
    terminal_setcolor(VGA_COLOR_WHITE);
}

void cmd_aes_trace_off(int argc, char *argv[])
{
    aes_trace_off();
    terminal_setcolor(VGA_COLOR_LIGHT_RED);
    print("AES Tracing: DISABLED\n");
    terminal_setcolor(VGA_COLOR_WHITE);
}

void cmd_aes_trace_flip(int argc, char *argv[])
{
    aes_trace_flip();
    int status = aes_trace_query();
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("AES Trace Toggled: ");
    if (status) {
        terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
        print("ON\n");
    } else {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("OFF\n");
    }
    terminal_setcolor(VGA_COLOR_WHITE);
}

void cmd_aes_trace_query(int argc, char *argv[])
{
    int status = aes_trace_query();
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("AES Trace Status: ");
    if (status) {
        terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
        print("ENABLED\n");
    } else {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("DISABLED\n");
    }
    terminal_setcolor(VGA_COLOR_WHITE);
}

// ============================================================
// Proxy commands
// ============================================================
void cmd_proxy_whoami(int argc, char *argv[])
{
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Fetching user info via proxy...\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    download_simple("http://10.0.2.2:8080/api/v10/users/@me", "user.json");
}

void cmd_proxy_guilds(int argc, char *argv[])
{
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Fetching guilds via proxy...\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    download_simple("http://10.0.2.2:8080/api/v10/users/@me/guilds", "guilds.json");
}

void cmd_proxy_channels(int argc, char *argv[])
{
    if (argc < 2) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: proxy.channels <guild_id>\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    char url[256];
    snprintf(url, sizeof(url),
             "http://10.0.2.2:8080/api/v10/guilds/%s/channels", argv[1]);
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Fetching channels via proxy...\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    download_simple(url, "channels.json");
}

void cmd_proxy_messages(int argc, char *argv[])
{
    if (argc < 2) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: proxy.messages <channel_id> [limit]\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    char url[256];
    int limit = (argc >= 3) ? simple_atoi(argv[2]) : 10;
    if (limit > 100) limit = 100;
    snprintf(url, sizeof(url),
             "http://10.0.2.2:8080/api/v10/channels/%s/messages?limit=%d",
             argv[1], limit);
    char filename[32];
    snprintf(filename, sizeof(filename), "msg_%s.json", argv[1]);
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Fetching messages via proxy...\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    download_simple(url, filename);
}

void cmd_proxy_send(int argc, char *argv[])
{
    if (argc < 3) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: proxy.send <channel_id> <message>\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    char url[256];
    snprintf(url, sizeof(url),
             "http://10.0.2.2:8080/api/v10/channels/%s/messages", argv[1]);
    char body[1024];
    snprintf(body, sizeof(body), "{\"content\":\"%s\"}", argv[2]);
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("POST via proxy: "); print(url); print("\n");
    print("Message: "); print(argv[2]); print("\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    print("Saved request to proxy_send.json (manual POST needed)\n");
}

void cmd_proxy_get(int argc, char *argv[])
{
    if (argc < 2) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: proxy.get <path> [filename]\n");
        print("Examples:\n");
        print("  proxy.get /api/v10/users/@me user.json\n");
        print("  proxy.get /api/v10/users/@me/guilds guilds.json\n");
        print("  proxy.get /test.malware malware.exe\n");
        print("  proxy.get /                         index.html\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    char proxy_url[512];
    snprintf(proxy_url, sizeof(proxy_url), "http://10.0.2.2:8080%s", argv[1]);
    const char *filename = (argc >= 3) ? argv[2] : "proxy_download";
    char full_filename[64];
    snprintf(full_filename, sizeof(full_filename), "%s_%s",
             strrchr(proxy_url, '/') + 1, filename);
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Proxy URL: "); print(proxy_url); print("\n");
    print("Saving as: "); print(full_filename); print("\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    download_simple(proxy_url, full_filename);
}

void cmd_files_proxy(int argc, char *argv[])
{
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("=== Proxy + Files Demo ===\n\n");
    print("1. Files:      http://10.0.2.2:8080/\n");
    print("2. User info:  http://10.0.2.2:8080/api/v10/users/@me\n");
    print("3. Guilds:     http://10.0.2.2:8080/api/v10/users/@me/guilds\n\n");
    print("Commands:\n");
    print("  files.browse          # Directory listing\n");
    print("  proxy.get /           # Save HTML index\n");
    print("  proxy.get /api/v10/users/@me user.json\n");
    print("  cat user.json         # View JSON\n");
    print("  proxy.get /test.malware malware.exe\n");
    terminal_setcolor(VGA_COLOR_WHITE);
}

// ============================================================
// Task commands
// ============================================================
void ps_command(int argc, char *argv[])
{
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'a')
        rust_list_tasks();
    else if (argc > 1)
        rust_task_info((uint32_t)simple_atoi(argv[1]));
    else
        rust_list_tasks();
}

void kill_command(int argc, char *argv[])
{
    if (argc < 2) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: kill <pid>\n       kill -9 <pid>\n       kill -all\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    if (argv[1][0] == '-' && argv[1][1] == 'a') {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("WARNING: Killing all tasks...\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    int pid_index = 1;
    if (argv[1][0] == '-' && argv[1][1] == '9') {
        if (argc < 3) {
            terminal_setcolor(VGA_COLOR_LIGHT_RED);
            print("Usage: kill -9 <pid>\n");
            terminal_setcolor(VGA_COLOR_WHITE);
            return;
        }
        pid_index = 2;
    }
    uint32_t pid = (uint32_t)simple_atoi(argv[pid_index]);
    terminal_setcolor(VGA_COLOR_LIGHT_BROWN);
    print("Killing PID "); print_integer(pid); print("...\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    if (rust_kill_task(pid) == 0) {
        terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
        print("Done\n");
        terminal_setcolor(VGA_COLOR_WHITE);
    }
}

void top_command(int argc, char *argv[])
{
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("=== RadiumOS Task Monitor ===\n\n");
    terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
    print("Active Tasks: "); print_integer(rust_get_task_count());
    terminal_setcolor(VGA_COLOR_WHITE);
    rust_list_tasks();
    print("\nPress any key to return...\n");
}

// ============================================================
// Misc commands
// ============================================================
void boot(int argc, char *argv[]) { terminal_clear(); rawr(); }

// ── Script commands ───────────────────────────────────────────
void cmd_script(int argc, char *argv[])
{
    if (argc < 2) {
        print("Usage: script <filename>\n");
        return;
    }
    terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
    print("Executing: "); print(argv[1]); print("\n\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    if (script_execute_file((const uint8_t *)argv[1]) != 0) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Script failed\n");
        terminal_setcolor(VGA_COLOR_WHITE);
    } else {
        terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
        print("Script completed\n");
        terminal_setcolor(VGA_COLOR_WHITE);
    }
}

void cmd_script_line(int argc, char *argv[])
{
    if (argc < 2) {
        print("Usage: rsh.line <line...>\n");
        return;
    }
    // Join all args back into a single line
    static char linebuf[512];
    join_args(linebuf, sizeof(linebuf), argv, 1, argc);
    int32_t r = script_execute_line_c((const uint8_t *)linebuf);
    if (r != 0) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Line execution failed\n");
        terminal_setcolor(VGA_COLOR_WHITE);
    }
}

void cmd_autoexec(int argc, char *argv[])
{
    terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
    print("Running autoexec...\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    script_execute_file(argv[2]);
}

void rash(int argc, char *argv[])
{
    if (avfs_file_exists("/bin/autoexec.rsh"))
        script_execute_file((const uint8_t *)"/bin/autoexec.rsh");
}

void rie(int argc, char *argv[]) { rust_image_editor(); }

// ============================================================
// Network commands
// ============================================================
void cmd_tcpreset(int argc, char *argv[]) { rust_tcp_force_reset(); }

void cmd_setdns(int argc, char *argv[])
{
    if (argc < 5) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: setdns <a> <b> <c> <d>\nExample: setdns 8 8 8 8\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    rust_set_dns((uint8_t)simple_atoi(argv[1]),
                 (uint8_t)simple_atoi(argv[2]),
                 (uint8_t)simple_atoi(argv[3]),
                 (uint8_t)simple_atoi(argv[4]));
}

void cmd_testdns(int argc, char *argv[]) { rust_test_dns_direct(); }

void cmd_nettest(int argc, char *argv[])
{
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Running network test...\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    rust_test_network_simple();
}

void cmd_rawsend(int argc, char *argv[])
{
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Testing raw packet send...\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    rust_test_raw_send();
}

void cmd_netdiag(int argc, char *argv[]) { rust_network_diag(); }

// ============================================================
// Browser
// ============================================================
void cmd_gbrowser(int argc, char *argv[]) { graphical_browser(); }

// ============================================================
// JSON
// ============================================================
void cmd_json_test(int argc, char *argv[]) { rust_test_json(); }

// ============================================================
// Discord commands
// ============================================================
void cmd_discord_init(int argc, char *argv[])
{
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Setting Discord token...\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    rust_discord_set_token((const uint8_t *)DISCORD_BOT_TOKEN);
    terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
    print("Token set. Use dwhoami to verify.\n");
    terminal_setcolor(VGA_COLOR_WHITE);
}

void cmd_discord_token(int argc, char *argv[])
{
    if (argc < 2) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: dtoken <bot_token>\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    rust_discord_set_token((const uint8_t *)argv[1]);
    terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
    print("Discord token updated!\n");
    terminal_setcolor(VGA_COLOR_WHITE);
}

void cmd_discord_whoami(int argc, char *argv[])
{
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Fetching Discord user info...\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    DISCORD_CALL(rust_discord_get_user_info());
}

void cmd_discord_guilds(int argc, char *argv[])
{
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Fetching guild list...\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    DISCORD_CALL(rust_discord_get_guilds());
}

void cmd_discord_channels(int argc, char *argv[])
{
    if (argc < 2) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: dchannels <guild_id>\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Fetching channels for guild "); print(argv[1]); print("...\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    DISCORD_CALL(rust_discord_get_channels((const uint8_t *)argv[1]));
}

void cmd_discord_messages(int argc, char *argv[])
{
    if (argc < 2) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: dmsg <channel_id> [limit]\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    int limit = (argc >= 3) ? simple_atoi(argv[2]) : 10;
    if (limit <= 0) limit = 10;
    if (limit > 100) limit = 100;
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Fetching "); print_integer(limit);
    print(" messages from "); print(argv[1]); print("...\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    DISCORD_CALL(rust_discord_get_channel_messages((const uint8_t *)argv[1], limit));
}

void cmd_discord_send(int argc, char *argv[])
{
    if (argc < 3) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: dsend <channel_id> <message...>\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    static char message[1024];
    join_args(message, sizeof(message), argv, 2, argc);
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Sending to "); print(argv[1]); print(": ");
    terminal_setcolor(VGA_COLOR_WHITE);
    print(message); print("\n");
    DISCORD_CALL(rust_discord_send_message((const uint8_t *)argv[1],
                                           (const uint8_t *)message));
}

void cmd_discord_embed(int argc, char *argv[])
{
    if (argc < 5) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: dembed <channel_id> <color_dec> <title> <description...>\n");
        print("       color decimal e.g. 5814783=#58B9FF\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    uint32_t color = (uint32_t)simple_atoi(argv[2]);
    static char desc[1024];
    join_args(desc, sizeof(desc), argv, 4, argc);
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Sending embed to "); print(argv[1]); print("...\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    DISCORD_CALL(rust_discord_send_embed((const uint8_t *)argv[1],
                                         (const uint8_t *)argv[3],
                                         (const uint8_t *)desc,
                                         color));
}

void cmd_discord_react(int argc, char *argv[])
{
    if (argc < 4) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: dreact <channel_id> <message_id> <emoji_url_encoded>\n");
        print("       e.g. dreact 123456 789012 %E2%9D%A4\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    DISCORD_CALL(rust_discord_react((const uint8_t *)argv[1],
                                    (const uint8_t *)argv[2],
                                    (const uint8_t *)argv[3]));
}

void cmd_discord_delete(int argc, char *argv[])
{
    if (argc < 3) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: ddel <channel_id> <message_id>\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    terminal_setcolor(VGA_COLOR_LIGHT_BROWN);
    print("Deleting message "); print(argv[2]);
    print(" from "); print(argv[1]); print("...\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    DISCORD_CALL(rust_discord_delete_message((const uint8_t *)argv[1],
                                              (const uint8_t *)argv[2]));
}

void cmd_discord_shell(int argc, char *argv[])
{
    if (argc < 2) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: dshell <channel_id>\n");
        print("Interactive shell. Enter=send  R=refresh  Q=quit\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    rust_discord_shell((const uint8_t *)argv[1]);
    rust_tcp_force_reset();
}

void cmd_discord_cache(int argc, char *argv[])
{
    rust_discord_dump_cache();
}

void cmd_discord_test(int argc, char *argv[])
{
    DISCORD_CALL(rust_test_discord());
}

// ============================================================
// Discord module system v2 commands
// ============================================================
void cmd_set_module(int argc, char *argv[])
{
    if (argc < 2) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: set.module <name>\n");
        print("Name must contain a type keyword:\n");
        print("  Original:\n");
        print("    send-emoji    send-message   send-embed\n");
        print("    fetch         delete         react\n");
        print("    auto-reply    pin            bulk-delete\n");
        print("    announce      poll           reminder    echo\n");
        print("  New v2:\n");
        print("    slowmode      nickname       thread\n");
        print("    webhook       status-watch   msg-search\n");
        print("    forward       roulette\n");
        print("Example: set.module my-roulette\n");
        print("Example: set.module morning-announce\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    rust_discord_set_module((const uint8_t *)argv[1]);
}

void cmd_config_module(int argc, char *argv[])
{
    if (argc < 2) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: config.module <name>\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    rust_discord_config_module((const uint8_t *)argv[1]);
}

void cmd_run_module(int argc, char *argv[])
{
    if (argc < 2) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: run.module <name>\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    rust_discord_run_module((const uint8_t *)argv[1]);
    rust_tcp_force_reset();
}

void cmd_list_modules(int argc, char *argv[])
{
    rust_discord_list_modules();
}

void cmd_remove_module(int argc, char *argv[])
{
    if (argc < 2) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: remove.module <name>\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    rust_discord_remove_module((const uint8_t *)argv[1]);
}

void cmd_tag_module(int argc, char *argv[])
{
    if (argc < 3) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: tag.module <name> <tag>\n");
        print("Attach a freeform label to a module (max 4 tags).\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    rust_discord_tag_module((const uint8_t *)argv[1],
                            (const uint8_t *)argv[2]);
}

void cmd_module_help(int argc, char *argv[])
{
    if (argc < 2)
        rust_discord_module_help(0);
    else
        rust_discord_module_help((const uint8_t *)argv[1]);
}

void cmd_cat_hex(int argc, char *argv[])
{
    if (argc < 2) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: cat.hex <filename>\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    cmd_cat_hexx((const uint8_t *)argv[1]);
}
extern int rshPKG(int argc, const char *const *argv);
void cmd_rpkg(int argc, char *argv[]) {
    if (argc < 2) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("error: no operation specified\n");
        print("Usage: rpkg <pkg>.rsh | rpkg -l | rpkg -ref | rpkg -s <query> | rpkg -up [pkg] | rpkg -rem <pkg>\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Radium Package Manager (RashSG Engine)\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    
    // Forward all arguments directly to the Rust FFI rshPKG function
    rshPKG(argc, (const char *const *)argv);
}

// ============================================================
// Registration
// ============================================================
extern void rshidt_command_start(int argc, char *argv[]);
void registerCommands(void)
{
    // -- Core OS --------------------------------------------------------------
    
    register_command("rshidt",       "Rash interactive development tool",              rshidt_command);
    register_command("rpkg", "Radium Package Manager", cmd_rpkg);
    register_command("rie",       "Rust image editor",              rie);
    register_command("help",      "Displays this message",          help_command);
    register_command("ls",        "List directory",                 ls_command);
    register_command("cat",       "Read text file",                 cat_command);
    register_command("rm",        "Remove file",                    rm_command);
    register_command("cowsay",    "Cowsay",                         cowsay_command);
    register_command("boot",      "Show welcome screen",            boot);
    register_command("echo",      "Echo message",                   echo_command);
    register_command("clear",     "Clear terminal",                 clear);
    register_command("hexdump",   "Hexdump toolkit",                geiger_command);
    register_command("cd",        "Change directory",               cd_command);
    register_command("ps",        "List tasks",                     ps_command);
    register_command("kill",      "Kill task by PID",               kill_command);
    register_command("top",       "Task monitor",                   top_command);
    register_command("exit",      "Exit the OS",                    exit_command);
    register_command("whd.diag",  "Watchdog task diagram",          watchdog_diagram);

    // -- Script engine --------------------------------------------------------
    register_command("script",    "Run .rsh/.rash script",          cmd_script);
    register_command("rsh.line",  "Execute a single script line",   cmd_script_line);
    register_command("autoexec",  "Run autoexec script",            cmd_autoexec);
    register_command("rash",      "Run /bin/autoexec.rsh",          rash);

    // -- Networking -----------------------------------------------------------
    register_command("setdns",    "Set DNS server (a b c d)",       cmd_setdns);
    register_command("testdns",   "Test DNS resolution",            cmd_testdns);
    register_command("nettest",   "ARP/network test",               cmd_nettest);
    register_command("rawsend",   "Send raw test packet",           cmd_rawsend);
    register_command("netdiag",   "Full network diagnostics",       cmd_netdiag);
    register_command("tcpreset",  "Force TCP state reset",          cmd_tcpreset);

    // -- Browser --------------------------------------------------------------
    register_command("gbrowser",  "Graphical web browser",          cmd_gbrowser);
    register_command("gb",        "Graphical browser (alias)",      cmd_gbrowser);

    // -- JSON -----------------------------------------------------------------
    register_command("jsontest",  "Test JSON parser",               cmd_json_test);

    // -- Discord --------------------------------------------------------------
    register_command("dinit",      "Init Discord (built-in token)", cmd_discord_init);
    register_command("dtoken",     "Set Discord bot token",         cmd_discord_token);
    register_command("dwhoami",    "Discord: who am I",             cmd_discord_whoami);
    register_command("dguilds",    "Discord: list servers",         cmd_discord_guilds);
    register_command("dchannels",  "Discord: list channels",        cmd_discord_channels);
    register_command("dmsg",       "Discord: fetch messages",       cmd_discord_messages);
    register_command("dsend",      "Discord: send message",         cmd_discord_send);
    register_command("dembed",     "Discord: send embed",           cmd_discord_embed);
    register_command("dreact",     "Discord: react to message",     cmd_discord_react);
    register_command("ddel",       "Discord: delete message",       cmd_discord_delete);
    register_command("dshell",     "Discord: interactive shell",    cmd_discord_shell);
    register_command("dcache",     "Discord: show message cache",   cmd_discord_cache);
    register_command("dtest",      "Discord: API test",             cmd_discord_test);

    // -- Discord modules v2 ---------------------------------------------------
    register_command("set.module",    "Create a Discord module",       cmd_set_module);
    register_command("config.module", "Configure a module",            cmd_config_module);
    register_command("run.module",    "Run a module",                  cmd_run_module);
    register_command("list.modules",  "List all modules",              cmd_list_modules);
    register_command("remove.module", "Remove a module",               cmd_remove_module);
    register_command("tag.module",    "Tag a module with a label",     cmd_tag_module);
    register_command("module.help",   "Module help (blank=all types)", cmd_module_help);

    // -- Proxy ----------------------------------------------------------------
    register_command("proxy.get",   "Download via proxy",              cmd_proxy_get);
    register_command("files.proxy", "Proxy + files demo",              cmd_files_proxy);

    // -- AES tracing ----------------------------------------------------------
    register_command("aes.trace.on",    "Enable full AES tracing",     cmd_aes_trace_on);
    register_command("aes.trace.off",   "Disable AES tracing",         cmd_aes_trace_off);
    register_command("aes.trace.flip",  "Toggle AES tracing state",    cmd_aes_trace_flip);
    register_command("aes.trace.query", "Check AES tracing status",    cmd_aes_trace_query);

    // -- AES encryption -------------------------------------------------------
    register_command("aes.key", "Set AES encryption key",              cmd_aes_key);
    register_command("aes.enc", "Encrypt a file",                      cmd_aes_enc);
    register_command("aes.dec", "Decrypt a file",                      cmd_aes_dec);

    // ===== PRP =====
    register_command("prp", "PRP crypto toolkit",           cmd_prp);

    // -- Misc -----------------------------------------------------------------
    register_command("cat.hex", "Print file as hex",                   cmd_cat_hex);
    terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
    print("All commands registered successfully!\n");
    terminal_setcolor(VGA_COLOR_WHITE);
}