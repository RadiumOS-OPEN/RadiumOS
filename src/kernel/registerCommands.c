#include "../commands/guard.h"
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
#include "../commands/mv.h"
#include "../commands/fileutils.h"
#include "../commands/sysutils.h"
#include "../commands/shellvars.h"
#include "../commands/moreutils.h"
#include "../shell/shsyntax.h"
#include "../commands/apprun.h"
#include "../commands/bootpage.h"
#include "../commands/anim.h"
#include "../commands/fsapp.h"
#include "../games/pong.h"
#include "../vga/scaler.h"
#include "../commands/rsd.h"
#include "../commands/update.h"
#include "../commands/discord.h"
#include "../user/sudo.h"
#include "../user/users.h"
#include "../commands/fsck.h"
#include "../commands/crypt.h"
#include "../commands/selftest.h"
#include "../commands/reboot.h"
#include "../timers/date.h"
#include "../timers/timer.h"
#include "../memory/pmm.h"
#include <stdbool.h>
#include "../commands/textfilters.h"
#include "../commands/cat.h"
#include "../commands/settings.h"
#include "../commands/ls.h"
#include "../commands/tui.h"
#include "../commands/radifetch.h"
#include "../commands/cowsay.h"
#include "../commands/gogetter.h"
#include "../commands/cd.h"
#include "../commands/text.h"
#include "../commands/gfxtest.h"
#include "../commands/diskpart.h"
#include "../commands/sysops.h"
#include "../commands/secure.h"
#include "../commands/ytm.h"
#include "../commands/vkdemo.h"
#include "../commands/doomport.h"
#include "../commands/desktop.h"


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
extern int rust_mp4_play_file(const char *path);
extern int rust_mp4_test_audio(void);
extern int rust_render_test_png(void);
extern int rust_load_and_render_png(const char *filename, int x, int y);

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

extern int rust_mp4_play_file_ex(const char *path_or_args, const char *args);

void cmd_mp4(int argc, char *argv[]) {
    if (argc < 2) {
        // Pass empty string or handle help trigger
        rust_mp4_play_file_ex("", "");
        return;
    }
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        rust_mp4_play_file_ex("--help", "");
        return;
    }
    if (strcmp(argv[1], "--test-audio") == 0 || strcmp(argv[1], "-testaudio") == 0) {
        rust_mp4_test_audio();
        return;
    }

    // Combine all arguments starting from argv[1] into a single space-separated string
    char arg_string[256];
    join_args(arg_string, sizeof(arg_string), argv, 1, argc);
    
    rust_mp4_play_file_ex(arg_string, "");
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
    bool batch = argc > 1 && strcmp(argv[1], "-b") == 0;
    for (;;) {
        if (!batch) terminal_clear();
        uint32_t days, hours, mins, secs, ms;
        get_uptime_precise(&days, &hours, &mins, &secs, &ms);
        terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
        print("=== RadiumOS Task Monitor ===\n");
        terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
        printr("up %u:%02u:%02u   tasks %u   memory %u of %u MB used\n\n", hours + days * 24, mins, secs, rust_get_task_count(),
               (uint32_t)((pmm_get_total_memory() - pmm_get_free_memory()) >> 20), (uint32_t)(pmm_get_total_memory() >> 20));
        terminal_setcolor(VGA_COLOR_WHITE);
        rust_list_tasks();
        if (batch) return;
        terminal_setcolor(VGA_COLOR_LIGHT_GREY);
        print("\nRefreshing every 2 seconds - press any key to quit\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        if (fs_wait(2000)) break;
    }
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

void cmd_png(int argc, char *argv[]) {
    if (argc < 2) {
        rust_render_test_png();
        return;
    }
    rust_load_and_render_png(argv[1], 0, 0);
}



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
extern int rust_fetch(int argc, const uint8_t *const *argv);

void cmd_fetch(int argc, char *argv[])
{
    rust_fetch(argc, (const uint8_t *const *)argv);
}

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
    register_command("png",       "Render PNG (Mode 13h)",          cmd_png);
    register_command("mp4",       "Play local H.264/AAC MP4",       cmd_mp4);
    register_command("diskpart", "Disk partition management", diskpart_command);
    register_command("help",      "Displays this message",          help_command);
    register_command("ls",        "List directory",                 ls_command);
    register_command("cat",       "Read text file",                 cat_command);
    register_command("rm",        "Remove file",                    rm_command);
    register_command("mv",        "Move or rename files",           mv_command);
    register_command("cp",        "Copy files and directories",     cp_command);
    register_command("mkdir",     "Create directories",             mkdir_command);
    register_command("touch",     "Create empty files",             touch_command);
    register_command("pwd",       "Print working directory",        pwd_command);
    register_command("which",     "Locate a command",               which_command);
    register_command("export",     "Set an environment variable",    export_command);
    register_command("unset",      "Remove an environment variable", unset_command);
    register_command("env",        "List environment variables",     env_command);
    register_command("alias",      "Define a command alias",         alias_command);
    register_command("unalias",    "Remove a command alias",         unalias_command);
    register_command("history",    "Show command history",           history_command);
    register_command("rrc",        "Run the startup script (.radiumrc)", rrc_command);
    register_command("sudo",       "Run a command with privileges", sudo_command);
    register_command("cpw",        "Create or change the sudo password", cpw_command);
    register_command("useradd",    "Create an account (-s: with sudo rights)", useradd_command);
    register_command("userdel",    "Remove an account",              userdel_command);
    register_command("usermod",    "Change an account: --sudo, --nosudo, --lock", usermod_command);
    register_command("passwd",     "Change an account password",     passwd_command);
    register_command("su",         "Switch to another account",      su_command);
    register_command("id",         "Show the user id and groups",    id_command);
    register_command("users",      "List accounts",                  users_command);
    register_command("guard",      "Kernel guard: integrity checks, essential tasks, security events (guard selftest | log)", guard_command);
    register_command("fsck",       "Check the /disk volume (-r repairs it)", fsck_command);
    register_command("crypt",      "Encrypt or decrypt files with a passphrase (lock/unlock/status)", crypt_command);
    register_command("selftest",   "Run the security self tests", selftest_command);
    register_command("logout",     "End the session and show the login",  logout_command);
    register_command("bootpage",   "Show the boot screen again (-b: built-in)", bootpage_command);
    register_command("systemctl",  "Control services (units): list-units, status, start, stop, enable...", systemctl_command);
    register_command("rsd",        "Service manager (same as systemctl)", systemctl_command);
    register_command("journalctl", "Show the service journal (-u UNIT, -n N)", journalctl_command);
    register_command("systemd-analyze", "Boot timing of services (blame)", analyze_command);
    register_command("anim",       "Animated overlays that keep moving (list/start/stop/demo)", anim_command);
    register_command("sysmon",     "Full-screen system monitor (units, memory, journal)", sysmon_command);
    register_command("discord",    "Discord client (token saved encrypted); discord logout", discord_command);
    register_command("life",       "Full-screen Game of Life",       life_command);
    register_command("matrix",     "Full-screen falling-glyph rain", matrix_command);
    register_command("snake",      "Snake on the whole screen",      snake_command);
    register_command("files",      "Full-height file manager",       files_command);
    register_command("resinfo",    "Show what the display scaler derived from the terminal size", screeninfo_command);
    register_command("banner",     "Print big block letters",        banner_command);
    register_command("center",     "Print a centred line of text",   center_command);
    register_command("color",      "Set the text colour",            color_command);
    register_command("box",        "Draw boot screen panel parts",   box_command);
    register_command("update",     "Update RadiumOS without rebuilding", update_command);
    register_command("welcome",    "Show the boot page again",       bootpage_command);
    register_command("test",       "Test files and values (for && and ||)", test_command);
    register_command("[",          "Test files and values (ends with ])",  bracket_command);
    register_command("find",       "Search for files by name",       find_command);
    register_command("tree",       "Show a directory tree",          tree_command);
    register_command("stat",       "Show file type and size",        stat_command);
    register_command("du",         "Show disk usage of a directory", du_command);
    register_command("diff",       "Compare two text files",         diff_command);
    register_command("tee",        "Copy piped input to a file",     tee_command);
    register_command("seq",        "Print a range of numbers",       seq_command);
    register_command("whoami",     "Show the current user",          whoami_command);
    register_command("hostname",   "Show the system name",           hostname_command);
    register_command("date",       "Show the date and time",         date_command);
    register_command("time",       "Show the current time",          time_command);
    register_command("timestamp",  "Show the Unix timestamp",        timestamp_command);
    register_command("uptime",     "Show how long the system has run", uptime_command);
    register_command("free",       "Show memory usage",              free_command);
    register_command("df",         "Show filesystem usage",          df_command);
    register_command("uname",      "Show system information",        uname_command);
    register_command("sleep",      "Pause for some seconds",         sleep_command);
    register_command("app",        "Run a C++ app (static ELF)",     app_command);
    register_command("true",       "Do nothing, successfully",       true_command);
    register_command("false",      "Do nothing, unsuccessfully",     false_command);
    register_command(":",          "Do nothing, successfully",       true_command);
    register_command("break",      "Leave a for/while/until loop",   break_command);
    register_command("continue",   "Next round of a loop",           continue_command);
    register_command("read",       "Read a line into a variable",    read_command);
    register_command("source",     "Run the commands in a file",     source_command);
    register_command(".",          "Run the commands in a file",     source_command);
    register_command("reboot",     "Restart the computer",           reboot_command);
    register_command("poweroff",   "Shut the computer down",         poweroff_command);
    register_command("shutdown",   "Shut the computer down",         poweroff_command);
    register_command("grep",      "Search lines for text",          grep_command);
    register_command("head",      "Show first lines",               head_command);
    register_command("tail",      "Show last lines",                tail_command);
    register_command("wc",        "Count lines, words, bytes",      wc_command);
    register_command("sort",      "Sort lines",                     sort_command);
    register_command("uniq",      "Collapse repeated lines",        uniq_command);
    register_command("cowsay",    "Cowsay",                         cowsay_command);
    register_command("boot",      "Show welcome screen",            boot);
    register_command("echo",      "Echo message",                   echo_command);
    register_command("clear",     "Clear terminal",                 clear);
    register_command("hexdump",   "Hexdump toolkit",                geiger_command);
    register_command("cd",        "Change directory",               cd_command);;
    register_command("ps",        "List tasks",                     ps_command);
    register_command("kill",      "Kill task by PID",               kill_command);
    register_command("top",       "Live task monitor (any key quits; top -b prints once)", top_command);
    register_command("cpuinfo",   "Processor details",              cpuinfo_command);
    register_command("meminfo",   "Physical memory, RAM filesystem and disk use", meminfo_command);
    register_command("lspci",     "List PCI devices",               lspci_command);
    register_command("watch",     "Repeat a command every few seconds (watch [-n S] CMD)", watch_command);
    register_command("sysstat",   "Graphs of memory, disk and task history", sysstat_command);
    register_command("syshealth", "System health checks (-q: problems only)", syshealth_command);
    register_command("secaudit",  "Security audit of this system",  secaudit_command);
    register_command("integrity", "Record and check hashes of boot-critical files", integrity_command);
    register_command("prp",       "PRP signatures: hash, keygen, sign, verify, seal, open", prp_command);
    register_command("sha256sum", "SHA-256 of files",               sha256sum_command);
    register_command("vault",     "Passphrase-sealed secrets (set/get/list/rm)", vault_command);
    register_command("shred",     "Overwrite a file three times, then delete it", shred_command);
    register_command("genpw",     "Generate random passwords",      genpw_command);
    register_command("ytm",       "Text-based YouTube Music player (search, play, queue; ytm login)", ytm_command);
    register_command("vulkaninfo", "Show the software Vulkan implementation", vulkaninfo_command);
    register_command("vkdemo",    "Vulkan demo: rotating cube with SPIR-V shaders (vkdemo [-f N] [-s WxH])", vkdemo_command);
    register_command("desktop",   "RadiumDE desktop environment (panel, dock, windows, mouse; ~/desktop.cfg)", desktop_command);
    register_command("rde",       "Alias of desktop", desktop_command);
    register_command("doom",      "Doom (bring your own WAD in /disk) drawn through Vulkan", doom_command);
    register_command("exit",      "Exit the OS",                    exit_command);
    register_command("whd.diag",  "Watchdog task diagram",          watchdog_diagram);
    register_command("gfxtest",       "Graphics test",  gfxtest);

    // -- Script engine --------------------------------------------------------
    register_command("script",    "Run .rsh/.rash script",          cmd_script);
    register_command("rsh.line",  "Execute a single script line",   cmd_script_line);
    register_command("autoexec",  "Run autoexec script",            cmd_autoexec);
    register_command("rash",      "Run /bin/autoexec.rsh",          rash);

    // -- Networking -----------------------------------------------------------
    register_command("setdns",    "Set DNS server (a b c d)",       cmd_setdns);
    register_command("testdns",   "Test DNS resolution",            cmd_testdns);
    register_command("nettest",   "ARP/network test",               cmd_nettest);
    register_command("fetch",     "Fetch an HTTP(S) URL (fetch --help)", cmd_fetch);
    register_command("rchat",     "Open the RChat communications client", tui);
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
