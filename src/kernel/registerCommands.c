#include "../errors/error.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../keyboard/keyboard.h"
#include "../rawr/rawr.h"
#include "../Avfs/Avfs.h"


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

    // -- Misc -----------------------------------------------------------------
    register_command("cat.hex", "Print file as hex",                   cmd_cat_hex);
    terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
    print("All commands registered successfully!\n");
    terminal_setcolor(VGA_COLOR_WHITE);
}