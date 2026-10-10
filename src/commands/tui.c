#include "tui.h"

#include "../io/io.h"
#include "../keyboard/keyboard.h"
#include "../terminal/terminal.h"
#include "../timers/date.h"
#include "../utility/utility.h"
#include "../vga/vga.h"

#include <stdbool.h>
#include <stdint.h>

#define RCHAT_ID_LEN 22
#define RCHAT_MESSAGE_MAX 4096
#define RCHAT_NICKNAME_MAX 24
/* https:// + 253-octet host + :port + optional slash */
#define RCHAT_SERVER_MAX 280
#define RCHAT_CONTACTS_MAX 4
#define RCHAT_SERVER_PROFILES_MAX 4
#define RCHAT_HISTORY_MAX 8
#define RCHAT_HISTORY_VISIBLE 4
#define RCHAT_REFRESH_SECONDS 15

extern int rust_rchat_use_server(const uint8_t *address, int trust_server_cert);
extern int rust_rchat_client_id(uint8_t *output, uint32_t capacity);
extern int rust_rchat_auth_public(uint8_t *output, uint32_t capacity);
extern int rust_rchat_encryption_public(uint8_t *output, uint32_t capacity);
extern int rust_rchat_enroll(const uint8_t *invite);
extern bool rust_rchat_is_enrolled(void);
extern int rust_rchat_check_contact(const uint8_t *client_id);
extern int rust_rchat_send(const uint8_t *client_id, const uint8_t *text);
extern int rust_rchat_poll(uint8_t *sender_output, uint32_t sender_capacity,
                           uint8_t *text_output, uint32_t text_capacity);
extern int rust_rchat_ack_pending(void);
extern bool rust_rchat_self_test(void);

typedef enum {
    RCHAT_HOME,
    RCHAT_CHAT,
    RCHAT_PROFILE,
    RCHAT_SERVERS,
    RCHAT_ERROR
} rchat_screen_t;

typedef struct {
    bool from_me;
    char text[RCHAT_MESSAGE_MAX + 1];
} rchat_message_t;

typedef struct {
    char client_id[RCHAT_ID_LEN + 1];
    char nickname[RCHAT_NICKNAME_MAX + 1];
    int message_count;
    rchat_message_t messages[RCHAT_HISTORY_MAX];
} rchat_contact_t;

typedef struct {
    char host[RCHAT_SERVER_MAX + 1];
    char address[RCHAT_SERVER_MAX + 1];
    bool trust_server_cert;
    int contact_count;
    int selection;
    bool ack_pending;
    rchat_contact_t contacts[RCHAT_CONTACTS_MAX];
} rchat_server_profile_t;

typedef struct {
    rchat_screen_t screen;
    rchat_screen_t return_screen;
    int server_count;
    int active_server;
    int history_offset;
    bool profile_revealed;
    char error[72];
    rchat_server_profile_t servers[RCHAT_SERVER_PROFILES_MAX];
} rchat_state_t;

static rchat_state_t state = {
    .screen = RCHAT_HOME,
    .return_screen = RCHAT_HOME,
    .active_server = -1,
};
static char draft_message[RCHAT_MESSAGE_MAX + 1];

static uint32_t pause_interrupts(void)
{
    uint32_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void restore_interrupts(uint32_t flags)
{
    if (flags & 0x200) __asm__ volatile("sti" ::: "memory");
}

static uint8_t color(enum vga_color foreground, enum vga_color background)
{
    return vga_entry_color(foreground, background);
}

static void put_clipped(vga_window_t *win, int x, int y, int width,
                        const char *text, uint8_t text_color)
{
    for (int i = 0; text[i] && i < width; i++)
        vga_win_putc_colored(win, x + i, y, text[i], text_color);
}

static void put_wrapped(vga_window_t *win, int x, int y, int width, int lines,
                        const char *text, uint8_t text_color)
{
    int row = 0;
    int column = 0;
    for (int i = 0; text[i] && row < lines; i++) {
        if (text[i] == '\n' || column == width) {
            row++;
            column = 0;
            if (row == lines) break;
            if (text[i] == '\n') continue;
        }
        vga_win_putc_colored(win, x + column++, y + row, text[i], text_color);
    }
}

static bool has_prefix(const char *text, const char *prefix, int length)
{
    for (int i = 0; i < length; i++)
        if (text[i] == '\0' || text[i] != prefix[i]) return false;
    return true;
}

/* 2 is HTTPS, 1 is HTTP, 0 is invalid. */
static int server_url_kind(const char *url)
{
    int start;
    if (has_prefix(url, "https://", 8)) start = 8;
    else if (has_prefix(url, "http://", 7)) start = 7;
    else return 0;

    if (url[start] == '\0' || url[start] == '/' || url[start] == '?' ||
        url[start] == '#') return 0;
    for (int i = start; url[i]; i++) {
        uint8_t byte = (uint8_t)url[i];
        if (byte <= 32 || byte == 127 || byte == '\\') return 0;
        if (byte == '?' || byte == '#') return 0;
        if (byte == '/' && url[i + 1] != '\0') return 0;
    }
    return start == 8 ? 2 : 1;
}

static bool server_host(const char *url, char *host)
{
    int kind = server_url_kind(url);
    if (!kind) return false;

    int start = kind == 2 ? 8 : 7;
    int end = strlen(url);
    if (end > start && url[end - 1] == '/') end--;
    for (int i = start; i < end; i++) {
        char c = url[i];
        host[i - start] = c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
    }
    host[end - start] = '\0';
    return true;
}

static rchat_server_profile_t *active_profile(void)
{
    if (state.active_server < 0 || state.active_server >= state.server_count)
        return NULL;
    return &state.servers[state.active_server];
}

/** Removes every trailing slash from a mutable server URL. */
static void strip_trailing_slash(char *url)
{
    size_t length = strlen(url);
    while (length > 0 && url[length - 1] == '/') {
        url[--length] = '\0';
    }
}

/** Removes the legacy insecure marker and reports whether it was present. */
static bool strip_legacy_insecure_suffix(char *url)
{
    static const char suffix[] = "#insecure";
    size_t suffix_len = strlen(suffix);
    size_t length = strlen(url);
    if (length >= suffix_len && strcmp(url + length - suffix_len, suffix) == 0) {
        url[length - suffix_len] = '\0';
        strip_trailing_slash(url);
        return true;
    }
    return false;
}

/** Synchronizes the active server address and certificate policy with Rust. */
static void sync_rust_server(void)
{
    rchat_server_profile_t *profile = active_profile();
    if (!profile) return;
    rust_rchat_use_server((const uint8_t *)profile->address,
                          profile->trust_server_cert ? 1 : 0);
}

static bool valid_client_id(const char *id)
{
    if (strlen(id) != RCHAT_ID_LEN) return false;
    for (int i = 0; i < RCHAT_ID_LEN; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    }
    return id[RCHAT_ID_LEN - 1] == 'A' || id[RCHAT_ID_LEN - 1] == 'Q' ||
           id[RCHAT_ID_LEN - 1] == 'g' || id[RCHAT_ID_LEN - 1] == 'w';
}

static bool valid_nickname(const char *nickname)
{
    int length = strlen(nickname);
    bool has_visible_character = false;
    if (length < 1 || length > RCHAT_NICKNAME_MAX) return false;
    for (int i = 0; i < length; i++) {
        uint8_t byte = (uint8_t)nickname[i];
        if (byte < 32 || byte > 126) return false;
        if (byte != ' ') has_visible_character = true;
    }
    return has_visible_character;
}

static bool valid_message(const char *message)
{
    size_t length = strlen(message);
    return length > 0 && length <= RCHAT_MESSAGE_MAX;
}

static bool deadline_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static bool read_key_until(uint32_t deadline, uint8_t *key)
{
    uint32_t polls = 0;
    while (true) {
        if (port_byte_in(0x64) & 1) {
            uint8_t scan = port_byte_in(0x60);
            if (scan != 0xE0 && !(scan & 0x80)) {
                *key = scan;
                return true;
            }
        } else {
            __asm__ volatile("pause");
        }
        if (++polls == (1u << 20)) {
            polls = 0;
            if (deadline_reached(get_unix_timestamp(), deadline)) return false;
        }
    }
}

static void draw_frame(vga_window_t *win, const char *section)
{
    rchat_server_profile_t *profile = active_profile();
    vga_win_clear(win);
    vga_win_set_title(win, " RCHAT ");
    if (profile)
        put_clipped(win, 3, 2, 68, profile->address, color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLUE));
    else
        put_clipped(win, 3, 2, 68, "No server configured. Press F8.", color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLUE));
    vga_win_draw_line_h(win, 2, 3, 72, 0xC4);
    if (section)
        put_clipped(win, 3, 4, 68, section, color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLUE));
}

static void draw_home(vga_window_t *win)
{
    rchat_server_profile_t *profile = active_profile();
    uint8_t normal = color(VGA_COLOR_WHITE, VGA_COLOR_BLUE);
    uint8_t muted = color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLUE);
    uint8_t selected = color(VGA_COLOR_BLACK, VGA_COLOR_CYAN);
    draw_frame(win, NULL);

    if (!profile) {
        put_clipped(win, 5, 8, 64, "Press F8 to add a server first.", normal);
    } else if (!rust_rchat_is_enrolled()) {
        put_clipped(win, 5, 8, 64, "Press F8, then I, to enroll this identity.", normal);
    } else if (profile->contact_count == 0) {
        put_clipped(win, 5, 8, 64, "Press N to add someone.", normal);
    } else {
        for (int i = 0; i < profile->contact_count; i++) {
            int y = 5 + i * 4;
            rchat_contact_t *contact = &profile->contacts[i];
            uint8_t name_color = i == profile->selection ? selected : normal;
            vga_win_fill_rect(win, 4, y, 66, 1, ' ', name_color);
            put_clipped(win, 5, y, 64, contact->nickname, name_color);
            if (contact->message_count > 0) {
                rchat_message_t *message = &contact->messages[contact->message_count - 1];
                if (message->from_me) {
                    put_clipped(win, 6, y + 1, 4, "ME: ", muted);
                    put_clipped(win, 10, y + 1, 58, message->text, normal);
                } else {
                    put_clipped(win, 6, y + 1, 24, contact->nickname, muted);
                    int start = 8 + strlen(contact->nickname);
                    put_clipped(win, start - 2, y + 1, 2, ": ", muted);
                    put_clipped(win, start, y + 1, 68 - start, message->text, normal);
                }
            } else {
                put_clipped(win, 6, y + 1, 62, "No messages yet.", muted);
            }
        }
    }

    vga_win_draw_line_h(win, 2, 20, 72, 0xC4);
    if (profile && rust_rchat_is_enrolled())
        put_clipped(win, 3, 21, 69, "N Add   R Refresh   ENTER Open   P Profile   F8 Servers", color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLUE));
    else
        put_clipped(win, 3, 21, 69, profile ? "P Profile   F8 Servers"
                                           : "P Profile   F8 Add server",
                    color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLUE));
    put_clipped(win, 3, 22, 69,
                profile && profile->contact_count > 0
                    ? "UP/DOWN Select   E Rename   ESC Exit"
                    : "UP/DOWN Select   ESC Exit",
                color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLUE));
}

static void draw_chat(vga_window_t *win)
{
    rchat_server_profile_t *profile = active_profile();
    if (!profile || profile->contact_count == 0) {
        state.screen = RCHAT_HOME;
        return;
    }

    rchat_contact_t *contact = &profile->contacts[profile->selection];
    uint8_t normal = color(VGA_COLOR_WHITE, VGA_COLOR_BLUE);
    uint8_t muted = color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLUE);
    draw_frame(win, contact->nickname);
    put_clipped(win, 5, 5, 64, contact->client_id, muted);

    if (contact->message_count == 0) {
        put_clipped(win, 5, 9, 64, "No messages yet. Press Enter to write.", normal);
    } else {
        for (int i = 0; i < RCHAT_HISTORY_VISIBLE &&
                        state.history_offset + i < contact->message_count; i++) {
            int y = 7 + i * 3;
            int message = state.history_offset + i;
            put_clipped(win, 5, y, 5,
                        contact->messages[message].from_me ? "ME:" : "THEM:", muted);
            put_wrapped(win, 11, y, 57, 2, contact->messages[message].text, normal);
        }
    }

    vga_win_draw_line_h(win, 2, 20, 72, 0xC4);
    put_clipped(win, 3, 21, 68,
                "UP/DOWN History   ENTER Message   R Refresh   E Rename   ESC Back",
                color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLUE));
}

static void draw_profile(vga_window_t *win)
{
    rchat_server_profile_t *profile = active_profile();
    char client_id[23] = "Unavailable";
    char signing_public[44] = "Unavailable";
    char encryption_public[44] = "Unavailable";
    const char *signing_value = "*******************************************";
    const char *encryption_value = "*******************************************";
    if (profile) rust_rchat_client_id((uint8_t *)client_id, sizeof(client_id));
    if (profile && state.profile_revealed) {
        if (rust_rchat_auth_public((uint8_t *)signing_public, sizeof(signing_public)) == 0)
            signing_value = signing_public;
        if (rust_rchat_encryption_public((uint8_t *)encryption_public,
                                         sizeof(encryption_public)) == 0)
            encryption_value = encryption_public;
    }
    draw_frame(win, "PROFILE");
    put_clipped(win, 5, 7, 64, "Server profile", color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLUE));
    put_clipped(win, 24, 7, 45, profile ? profile->host : "No server selected", color(VGA_COLOR_WHITE, VGA_COLOR_BLUE));
    put_clipped(win, 5, 9, 64, "Client ID", color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLUE));
    put_clipped(win, 24, 9, 45, client_id, color(VGA_COLOR_WHITE, VGA_COLOR_BLUE));
    put_clipped(win, 5, 12, 64, "Signing public", color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLUE));
    put_clipped(win, 24, 12, 45, signing_value, color(VGA_COLOR_WHITE, VGA_COLOR_BLUE));
    put_clipped(win, 5, 15, 64, "Encryption public", color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLUE));
    put_clipped(win, 24, 15, 45, encryption_value, color(VGA_COLOR_WHITE, VGA_COLOR_BLUE));
    put_clipped(win, 3, 21, 68,
                state.profile_revealed ? "R Hide   P, ENTER or ESC Back"
                                       : "R Reveal   P, ENTER or ESC Back",
                color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLUE));
}

/** Draws the server selection screen and its transport security warning. */
static void draw_servers(vga_window_t *win)
{
    rchat_server_profile_t *profile = active_profile();
    draw_frame(win, "SERVERS");
    if (profile) {
        put_clipped(win, 5, 7, 64, "Current", color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLUE));
        put_clipped(win, 5, 9, 64, profile->address, color(VGA_COLOR_WHITE, VGA_COLOR_BLUE));
        put_clipped(win, 5, 11, 64,
                    rust_rchat_is_enrolled() ? "Identity enrolled" : "Identity not enrolled",
                    color(VGA_COLOR_WHITE, VGA_COLOR_BLUE));
    } else {
        put_clipped(win, 5, 8, 64, "No server configured.", color(VGA_COLOR_WHITE, VGA_COLOR_BLUE));
    }

    if (profile && server_url_kind(profile->address) == 1)
        put_wrapped(win, 5, 12, 64, 3,
                    "HTTP exposes login and metadata. Messages remain end-to-end encrypted.",
                    color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLUE));
    else if (profile && profile->trust_server_cert)
        put_wrapped(win, 5, 12, 64, 3,
                    "TLS certificate is not verified. Use only for local development.",
                    color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLUE));
    else
        put_clipped(win, 5, 12, 64, "HTTPS is recommended.", color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLUE));

    put_clipped(win, 3, 21, 68,
                profile && rust_rchat_is_enrolled()
                    ? "T TLS trust   E/ENTER Edit   F8/ESC Back"
                    : "I Enroll   T TLS trust   E/ENTER Edit   F8/ESC Back",
                color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLUE));
}

static void draw_error(vga_window_t *win)
{
    draw_frame(win, "ERROR");
    put_wrapped(win, 5, 8, 64, 4, state.error, color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLUE));
    put_clipped(win, 3, 21, 68, "ENTER or ESC Back", color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLUE));
}

static void draw_loading(vga_window_t *win, const char *message)
{
    draw_frame(win, "WORKING");
    put_clipped(win, 5, 9, 64, message, color(VGA_COLOR_WHITE, VGA_COLOR_BLUE));
    vga_win_refresh(win);
}

static bool input(vga_window_t *win, const char *title, const char *prompt,
                  char *output, int max_length, bool masked)
{
    int length = 0;
    bool shift = false;
    bool caps = false;
    output[0] = '\0';

    while (true) {
        draw_frame(win, title);
        put_wrapped(win, 5, 7, 64, 2, prompt, color(VGA_COLOR_WHITE, VGA_COLOR_BLUE));
        vga_win_draw_box_colored(win, 4, 10, 68, 3, color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLUE));
        int shown = length > 64 ? length - 64 : 0;
        for (int i = shown; i < length; i++)
            vga_win_putc_colored(win, 6 + i - shown, 11, masked ? '*' : output[i], color(VGA_COLOR_WHITE, VGA_COLOR_BLUE));
        vga_win_putc_colored(win, 6 + length - shown, 11, '_', color(VGA_COLOR_BLACK, VGA_COLOR_CYAN));
        put_clipped(win, 5, 16, 64, "ENTER Save   BACKSPACE Delete   ESC Cancel", color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLUE));
        vga_win_refresh(win);

        while ((port_byte_in(0x64) & 1) == 0) {}
        uint8_t scan = port_byte_in(0x60);
        if (scan == 0xE0) continue;
        if (scan & 0x80) {
            scan &= 0x7F;
            if (scan == 0x2A || scan == 0x36) shift = false;
            continue;
        }
        if (scan == 0x2A || scan == 0x36) {
            shift = true;
            continue;
        }
        if (scan == 0x3A) {
            caps = !caps;
            continue;
        }
        if (scan == 0x01) {
            output[0] = '\0';
            return false;
        }
        if (scan == 0x1C) {
            output[length] = '\0';
            return true;
        }
        if (scan == 0x0E) {
            if (length > 0) output[--length] = '\0';
            continue;
        }
        char character = keyboard_to_char(scan, shift, caps);
        if (character && length < max_length) {
            output[length++] = character;
            output[length] = '\0';
        }
    }
}

static void fail(const char *message, rchat_screen_t return_screen)
{
    strncpy(state.error, message, sizeof(state.error) - 1);
    state.error[sizeof(state.error) - 1] = '\0';
    state.return_screen = return_screen;
    state.screen = RCHAT_ERROR;
}

static void add_person(vga_window_t *win)
{
    rchat_server_profile_t *profile = active_profile();
    char client_id[RCHAT_ID_LEN + 1];
    char nickname[RCHAT_NICKNAME_MAX + 1];
    if (!profile) {
        fail("Add a server with F8 before starting a DM.", RCHAT_HOME);
        return;
    }
    if (!rust_rchat_is_enrolled()) {
        fail("Enroll this server identity with F8, then I.", RCHAT_HOME);
        return;
    }
    if (profile->contact_count == RCHAT_CONTACTS_MAX) {
        fail("This build can hold four people.", RCHAT_HOME);
        return;
    }
    if (!input(win, "ADD PERSON", "Client ID, e.g. 123456789012345678901A",
               client_id, RCHAT_ID_LEN, false)) return;
    if (!valid_client_id(client_id)) {
        fail("ID needs 22 base64url chars and must end in A, Q, g, or w.", RCHAT_HOME);
        return;
    }
    for (int i = 0; i < profile->contact_count; i++) {
        if (strcmp(profile->contacts[i].client_id, client_id) == 0) {
            fail("That client ID is already in your DMs.", RCHAT_HOME);
            return;
        }
    }
    draw_loading(win, "Looking up and verifying recipient keys...");
    int lookup_result = rust_rchat_check_contact((const uint8_t *)client_id);
    if (lookup_result != 0) {
        fail(lookup_result == -4 ? "Could not reach or authenticate the server."
                                 : lookup_result == -5 ? "The server rejected recipient lookup."
                                                       : "Recipient key verification failed.",
             RCHAT_HOME);
        return;
    }
    if (!input(win, "ADD PERSON", "Nickname", nickname, RCHAT_NICKNAME_MAX, false)) return;
    if (!valid_nickname(nickname)) {
        fail("Nickname must contain 1 to 24 printable characters.", RCHAT_HOME);
        return;
    }

    rchat_contact_t *contact = &profile->contacts[profile->contact_count];
    strcpy(contact->client_id, client_id);
    strcpy(contact->nickname, nickname);
    contact->message_count = 0;
    profile->selection = profile->contact_count++;
}

static void rename_person(vga_window_t *win)
{
    rchat_server_profile_t *profile = active_profile();
    if (!profile || profile->contact_count == 0) return;

    char nickname[RCHAT_NICKNAME_MAX + 1];
    if (!input(win, "RENAME", "New nickname", nickname,
               RCHAT_NICKNAME_MAX, false)) return;
    if (!valid_nickname(nickname)) {
        fail("Nickname must contain 1 to 24 printable characters.", state.screen);
        return;
    }
    strcpy(profile->contacts[profile->selection].nickname, nickname);
}

static void append_message(rchat_contact_t *contact, const char *text, bool from_me)
{
    if (contact->message_count == RCHAT_HISTORY_MAX) {
        memmove(&contact->messages[0], &contact->messages[1],
                (RCHAT_HISTORY_MAX - 1) * sizeof(contact->messages[0]));
        contact->message_count--;
    }
    rchat_message_t *message = &contact->messages[contact->message_count++];
    message->from_me = from_me;
    strcpy(message->text, text);
}

/** Prompts for and sends a message to the selected contact. */
static void write_message(vga_window_t *win)
{
    rchat_server_profile_t *profile = active_profile();
    if (!profile || profile->contact_count == 0) return;
    if (!input(win, profile->contacts[profile->selection].nickname,
               "Message", draft_message, RCHAT_MESSAGE_MAX, false)) return;
    if (!valid_message(draft_message)) {
        fail("Message cannot be empty and may contain at most 4096 bytes.", RCHAT_CHAT);
        return;
    }

    rchat_contact_t *contact = &profile->contacts[profile->selection];
    sync_rust_server();
    draw_loading(win, "Sending message...");
    int send_result = rust_rchat_send((const uint8_t *)contact->client_id,
                                      (const uint8_t *)draft_message);
    if (send_result != 0) {
        memset(draft_message, 0, sizeof(draft_message));
        fail(send_result == -4 ? "Could not reach or authenticate the server."
                               : send_result == -5 ? "The server rejected the message."
                                                   : "Recipient keys or message encryption failed.",
             RCHAT_CHAT);
        return;
    }
    append_message(contact, draft_message, true);
    memset(draft_message, 0, sizeof(draft_message));
    state.history_offset = contact->message_count > RCHAT_HISTORY_VISIBLE
                               ? contact->message_count - RCHAT_HISTORY_VISIBLE
                               : 0;
}

/** Polls for messages and stores them in their matching contact histories. */
static void refresh_messages(rchat_screen_t return_screen, bool quiet)
{
    rchat_server_profile_t *profile = active_profile();
    if (!profile || !rust_rchat_is_enrolled()) {
        if (!quiet)
            fail("Select and enroll a server before refreshing.", return_screen);
        return;
    }
    if (profile->ack_pending) {
        if (rust_rchat_ack_pending() != 0) {
            fail("A saved message is still waiting for server acknowledgement.",
                 return_screen);
            return;
        }
        profile->ack_pending = false;
    }
    sync_rust_server();
    for (int count = 0; count < RCHAT_HISTORY_MAX; count++) {
        char sender[RCHAT_ID_LEN + 1];
        char message[RCHAT_MESSAGE_MAX + 1];
        int result = rust_rchat_poll((uint8_t *)sender, sizeof(sender),
                                     (uint8_t *)message, sizeof(message));
        if (result == 1) return;
        if (result != 0) {
            if (quiet && (result == -4 || result == -5)) return;
            fail(result == -4 ? "Could not reach or authenticate the server."
                              : result == -5 ? "The server rejected the refresh."
                              : result == -7 ? "A previous message still needs local storage."
                                             : "A received message failed verification.",
                 return_screen);
            return;
        }

        int contact_index = -1;
        for (int i = 0; i < profile->contact_count; i++) {
            if (strcmp(profile->contacts[i].client_id, sender) == 0) {
                contact_index = i;
                break;
            }
        }
        if (contact_index < 0) {
            if (profile->contact_count == RCHAT_CONTACTS_MAX) {
                memset(message, 0, sizeof(message));
                fail("A verified sender could not be added because the DM list is full.",
                     return_screen);
                return;
            }
            contact_index = profile->contact_count++;
            rchat_contact_t *contact = &profile->contacts[contact_index];
            strcpy(contact->client_id, sender);
            strcpy(contact->nickname, sender);
            contact->message_count = 0;
        }
        append_message(&profile->contacts[contact_index], message, false);
        memset(message, 0, sizeof(message));
        if (return_screen == RCHAT_CHAT && contact_index == profile->selection) {
            rchat_contact_t *contact = &profile->contacts[contact_index];
            state.history_offset = contact->message_count > RCHAT_HISTORY_VISIBLE
                                       ? contact->message_count - RCHAT_HISTORY_VISIBLE
                                       : 0;
        }
        profile->ack_pending = true;
        if (rust_rchat_ack_pending() != 0) {
            fail("Message saved locally; server acknowledgement is pending.",
                 return_screen);
            return;
        }
        profile->ack_pending = false;
    }
}

/** Adds or updates a server profile from interactive input. */
static void edit_server(vga_window_t *win)
{
    char server[RCHAT_SERVER_MAX + 1];
    char host[RCHAT_SERVER_MAX + 1];
    bool trust_server_cert = false;
    if (!input(win, "SERVER", "HTTP exposes client-server traffic; prefer HTTPS.",
               server, RCHAT_SERVER_MAX, false)) return;
    if (strip_legacy_insecure_suffix(server)) trust_server_cert = true;
    strip_trailing_slash(server);
    if (!server_host(server, host)) {
        fail("Use http:// or https:// followed by a domain or IP address.", RCHAT_SERVERS);
        return;
    }

    for (int i = 0; i < state.server_count; i++) {
        if (strcmp(state.servers[i].host, host) == 0) {
            trust_server_cert = state.servers[i].trust_server_cert;
            break;
        }
    }

    int identity_result = rust_rchat_use_server((const uint8_t *)server, trust_server_cert ? 1 : 0);
    if (identity_result == -2) {
        fail("This build can remember four server profiles.", RCHAT_SERVERS);
        return;
    }
    if (identity_result != 0) {
        fail("Could not create the server identity.", RCHAT_SERVERS);
        return;
    }

    for (int i = 0; i < state.server_count; i++) {
        if (strcmp(state.servers[i].host, host) == 0) {
            strcpy(state.servers[i].address, server);
            state.servers[i].trust_server_cert = trust_server_cert;
            state.active_server = i;
            return;
        }
    }
    if (state.server_count == RCHAT_SERVER_PROFILES_MAX) {
        fail("This build can remember four server profiles.", RCHAT_SERVERS);
        return;
    }

    rchat_server_profile_t *profile = &state.servers[state.server_count];
    strcpy(profile->host, host);
    strcpy(profile->address, server);
    profile->trust_server_cert = trust_server_cert;
    profile->contact_count = 0;
    profile->selection = 0;
    profile->ack_pending = false;
    state.active_server = state.server_count++;
}

/** Enrolls the active server identity using an invite code. */
static void enroll_identity(vga_window_t *win)
{
    if (!active_profile()) {
        fail("Add a server before enrolling.", RCHAT_SERVERS);
        return;
    }
    if (rust_rchat_is_enrolled()) {
        fail("This server identity is already enrolled.", RCHAT_SERVERS);
        return;
    }
    char invite[65];
    if (!input(win, "ENROLL", "Invite code", invite, 64, true)) return;
    sync_rust_server();
    draw_loading(win, "Enrolling identity with server...");
    int result = rust_rchat_enroll((const uint8_t *)invite);
    memset(invite, 0, sizeof(invite));
    if (result == 0) return;
    if (result == -1)
        fail("Invite code must be 12 to 64 printable characters.", RCHAT_SERVERS);
    else if (result == -4)
        fail("Could not reach or authenticate the server.", RCHAT_SERVERS);
    else if (result == -5)
        fail("The server rejected enrollment.", RCHAT_SERVERS);
    else if (result == -7)
        fail("The invite code is invalid or exhausted.", RCHAT_SERVERS);
    else if (result == -8)
        fail("Enrollment signature verification failed.", RCHAT_SERVERS);
    else
        fail("The server returned an invalid enrollment response.", RCHAT_SERVERS);
}

static int self_test(void)
{
    int failures = 0;
    char host[RCHAT_SERVER_MAX + 1];
    rchat_screen_t saved_screen = state.screen;
    rchat_screen_t saved_return_screen = state.return_screen;
    if (server_url_kind("https://chat.example") != 2) failures |= 1 << 0;
    if (server_url_kind("http://chat.example") != 1) failures |= 1 << 1;
    if (server_url_kind("https://bad host")) failures |= 1 << 2;
    if (server_url_kind(
            "https://rchat-server-abcdefghijklmnopqrstuvwxyz0123456789.vercel.app") != 2)
        failures |= 1 << 11;
    if (!valid_client_id("123456789012345678901A")) failures |= 1 << 3;
    if (valid_client_id("AAAAAAAAAAAAAAAAAAAAAB")) failures |= 1 << 4;
    if (!valid_nickname("Jedrek")) failures |= 1 << 5;
    if (!valid_message("hello") || valid_message("")) failures |= 1 << 6;
    if (!server_host("https://CHAT.EXAMPLE/", host) ||
        strcmp(host, "chat.example") != 0 ||
        !server_host("http://chat.example", host) ||
        strcmp(host, "chat.example") != 0) failures |= 1 << 7;
    fail("test", RCHAT_HOME);
    if (state.screen != RCHAT_ERROR || state.return_screen != RCHAT_HOME) failures |= 1 << 8;
    if (!rust_rchat_self_test()) failures |= 1 << 9;
    if (!deadline_reached(10, 10) || deadline_reached(9, 10) ||
        !deadline_reached(1, 0xFFFFFFFFu)) failures |= 1 << 10;
    state.screen = saved_screen;
    state.return_screen = saved_return_screen;

    if (failures == 0) {
        print("rchat self-test: PASS\n");
        return 0;
    }
    print("rchat self-test: FAIL 0x");
    print_hex(failures);
    print("\n");
    return -1;
}

static void print_help(void)
{
    print("Usage: rchat [--help | --self-test]\n");
    print("After adding a server, N adds a person. Enter opens the selected DM.\n");
    print("Inside a DM, Enter writes a message and Escape returns.\n");
    print("R checks for messages; RChat also refreshes every 15 seconds.\n");
    print("E renames the selected person from the DM list or inside a DM.\n");
    print("F8 adds or switches servers.\n");
    print("HTTP exposes credentials and metadata; message contents stay E2EE.\n");
}

/** Runs the interactive rChat terminal interface. */
void tui(int argc, char *argv[])
{
    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            print_help();
            return;
        }
        if (strcmp(argv[1], "--self-test") == 0) {
            self_test();
            return;
        }
        print("rchat: unknown option\n");
        print_help();
        return;
    }

    /* Scheduler HUD tasks write directly to VGA; this modal window owns it. */
    uint32_t interrupt_flags = pause_interrupts();
    vga_window_t win = vga_create_centered_window(76, 24, VGA_COLOR_WHITE, VGA_COLOR_BLUE);
    if (!win.buffer) {
        restore_interrupts(interrupt_flags);
        print("rchat: could not allocate VGA window\n");
        return;
    }
    state.screen = RCHAT_HOME;
    bool running = true;
    uint32_t next_refresh = get_unix_timestamp() + RCHAT_REFRESH_SECONDS;

    while (running) {
        if (state.screen == RCHAT_HOME) draw_home(&win);
        else if (state.screen == RCHAT_CHAT) draw_chat(&win);
        else if (state.screen == RCHAT_PROFILE) draw_profile(&win);
        else if (state.screen == RCHAT_SERVERS) draw_servers(&win);
        else draw_error(&win);
        vga_win_refresh(&win);

        uint8_t key;
        if (!read_key_until(next_refresh, &key)) {
            if ((state.screen == RCHAT_HOME || state.screen == RCHAT_CHAT) &&
                active_profile() && rust_rchat_is_enrolled())
                refresh_messages(state.screen, true);
            next_refresh = get_unix_timestamp() + RCHAT_REFRESH_SECONDS;
            continue;
        }
        if (state.screen == RCHAT_ERROR) {
            if (key == 0x01 || key == 0x1C) state.screen = state.return_screen;
            continue;
        }
        if (state.screen == RCHAT_PROFILE) {
            if (key == 0x13) state.profile_revealed = !state.profile_revealed; /* R */
            else if (key == 0x01 || key == 0x1C || key == 0x19) {
                state.profile_revealed = false;
                state.screen = RCHAT_HOME;
            }
            continue;
        }
        if (state.screen == RCHAT_CHAT) {
            rchat_server_profile_t *profile = active_profile();
            rchat_contact_t *contact = &profile->contacts[profile->selection];
            if (key == 0x01) state.screen = RCHAT_HOME;
            else if (key == 0x1C) write_message(&win);
            else if (key == 0x13) {
                refresh_messages(RCHAT_CHAT, false); /* R */
                next_refresh = get_unix_timestamp() + RCHAT_REFRESH_SECONDS;
            } else if (key == 0x12) rename_person(&win); /* E */
            else if (key == 0x48 && state.history_offset > 0) state.history_offset--;
            else if (key == 0x50 && state.history_offset + RCHAT_HISTORY_VISIBLE < contact->message_count)
                state.history_offset++;
            continue;
        }
        if (state.screen == RCHAT_SERVERS) {
            if (key == 0x01 || key == 0x42) state.screen = RCHAT_HOME;
            else if (key == 0x1C || key == 0x12) edit_server(&win);
            else if (key == 0x17) enroll_identity(&win); /* I */
            else if (key == 0x14) { /* T */
                rchat_server_profile_t *profile = active_profile();
                if (profile && server_url_kind(profile->address) == 2) {
                    profile->trust_server_cert = !profile->trust_server_cert;
                    sync_rust_server();
                }
            }
            continue;
        }

        rchat_server_profile_t *profile = active_profile();
        if (key == 0x01) running = false;
        else if (key == 0x48 && profile && profile->selection > 0) profile->selection--;
        else if (key == 0x50 && profile && profile->selection + 1 < profile->contact_count) profile->selection++;
        else if (key == 0x1C && profile && profile->contact_count > 0) {
            rchat_contact_t *contact = &profile->contacts[profile->selection];
            state.history_offset = contact->message_count > RCHAT_HISTORY_VISIBLE
                                       ? contact->message_count - RCHAT_HISTORY_VISIBLE
                                       : 0;
            state.screen = RCHAT_CHAT;
        } else if (key == 0x31) add_person(&win); /* N */
        else if (key == 0x13) {
            refresh_messages(RCHAT_HOME, false); /* R */
            next_refresh = get_unix_timestamp() + RCHAT_REFRESH_SECONDS;
        } else if (key == 0x12) rename_person(&win); /* E */
        else if (key == 0x19) {
            state.profile_revealed = false;
            state.screen = RCHAT_PROFILE;
        } /* P */
        else if (key == 0x42) state.screen = RCHAT_SERVERS; /* F8 */
    }

    vga_destroy_window(&win);
    terminal_clear();
    restore_interrupts(interrupt_flags);
}
