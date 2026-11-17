// discord.c - a Discord client for the 1280x720 console, laid out like Discord's own dark theme
//
//   discord            log in (first run: paste the token, pick a passphrase) and open the client
//   discord logout     forget the saved token
//   discord status     is a token saved?
//
// It talks to https://canary.discord.com/api/v10 with the normal REST calls and polls for new messages every few
// seconds (there is no gateway websocket). Avatars, server icons and image attachments are downloaded from Discord's
// CDN (no token is sent there) and decoded as PNG. The token is kept on disk only as a PRP vault (/disk/discord.tok):
// ChaCha20-Poly1305 under a key stretched from the passphrase. It is never printed, and the memory that held it is
// wiped on exit. F1 inside the client lists every key.
//
// DISCORD_API and DISCORD_CDN (shell variables) replace the API and CDN base URLs, for a proxy or a test server.

#include <stdint.h>
#include <stdbool.h>
#include "discord.h"
#include "fsapp.h"
#include "shellvars.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../vga/gui.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../io/clipboard.h"
#include "../timers/tsc.h"
#include "../sound/sound.h"
#include "emoji_data.h"
#include "emoji_png.h"

extern int rust_http_request(const uint8_t *method, const uint8_t *url, const uint8_t *headers, const uint8_t *body,
                             uint32_t body_len, uint8_t *out, uint32_t out_cap, int *status);
extern void rust_http_error(char *out, uint32_t cap);
extern int rust_prp_vault_save(const uint8_t *password, const uint8_t *secret, uint32_t len, const uint8_t *path);
extern int rust_prp_vault_load(const uint8_t *password, const uint8_t *path, uint8_t *out, uint32_t cap);
extern int rust_png_decode_fit(const uint8_t *data, uint32_t len, uint32_t max_w, uint32_t max_h, uint32_t *out,
                               uint32_t out_cap, uint32_t *w_out, uint32_t *h_out);
extern int rust_sfx_init(void);
extern int rust_sfx_builtin(uint32_t kind, uint32_t volume);
extern int rust_sfx_wav(const uint8_t *data, uint32_t len, uint32_t volume);
extern int rust_disk_store_size(const uint8_t *path);
extern int rust_disk_store_read(const uint8_t *path, uint32_t offset, uint8_t *buf, uint32_t len);
extern int helios_exec_file_args(const uint8_t *path, uint8_t verbosity, int argc, char **argv);

#define API_BASE   "https://canary.discord.com/api/v10"
#define CDN_BASE   "https://cdn.discordapp.com"
#define TOKEN_FILE "/disk/discord.tok"
#define USER_AGENT "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
#define BOT_AGENT  "DiscordBot (https://radium.global, 1.0)"
#define EMOJI_CDN  "https://cdn.jsdelivr.net/gh/jdecked/twemoji@15.1.0/assets/72x72"
#define RESP_MAX   (768 * 1024)
#define BIN_MAX    (1024 * 1024)
#define REFRESH_MS 8000

// Client-side pacing, well inside Discord's limits (50 requests/s global, a few per second per route).
#define API_GAP_MS     250     // minimum spacing between any two API calls
#define IMG_GAP_MS     120     // spacing between CDN downloads
#define HIST_GAP_MS    1500    // minimum spacing between history pages
#define HIST_BURST     8       // history pages allowed per window
#define HIST_WINDOW_MS 30000
#define SEND_GAP_MS    1000    // spacing between messages sent
#define SEND_BURST     4       // messages per window (Discord allows 5 per 5 s per channel)
#define SEND_WINDOW_MS 5000
#define BACKOFF_MAX_MS 120000
#define DUP_WINDOW_MS  30000   // the same text twice within this looks like spam
#define IDLE_SLOW_MS   300000  // no key for this long: polling slows down
#define JOIN_GAP_MS    60000   // joining servers in bursts is a top reason accounts get flagged
#define JOIN_BURST     3
#define JOIN_WINDOW_MS 3600000

// Text-mode colours for the login screens.
enum { K = 0, B = 1, GR = 2, CY = 3, RD = 4, LG = 7, DG = 8, LB = 9, LGN = 10, LC = 11, LR = 12, YL = 14, WH = 15 };

#define A_BASE    fs_attr(LG, K)
#define A_DIM     fs_attr(DG, K)
#define A_ERR     fs_attr(LR, K)
#define A_BOX     fs_attr(LC, K)

static char g_resp[RESP_MAX + 1];
static uint8_t g_bin[BIN_MAX];
static char g_token[256];
static char g_err[96];
static char g_me[48];
static char g_me_id[24];
static char g_me_avatar[40];
static int  g_last_status;

// A bot token is kept (and sent) as "Bot <token>"; both kinds are used the same way afterwards.
static bool is_bot(void) { return strncmp(g_token, "Bot ", 4) == 0; }

static uint64_t g_api_next;        // earliest time of the next API call
static uint64_t g_backoff_until;   // set after a 429: no API calls before this
static int      g_backoff_streak;
static int      g_backoff_secs;
static bool     g_auth_dead;       // the token was rejected: stop all automatic requests

// ---- HTTP ------------------------------------------------------------------------------------------------

static const char *js_get(const char *obj, const char *key);

// Seconds with decimals ("1.337") -> milliseconds.
static int js_ms(const char *v)
{
    int whole = 0, frac = 0, digits = 0;
    if (!v) return 0;
    while (*v >= '0' && *v <= '9') whole = whole * 10 + (*v++ - '0');
    if (*v == '.') {
        v++;
        while (*v >= '0' && *v <= '9' && digits < 3) { frac = frac * 10 + (*v++ - '0'); digits++; }
        while (digits++ < 3) frac *= 10;
    }
    return whole * 1000 + frac;
}

// Returns the body length (in g_resp) or -1 (g_err says why). *status is the HTTP status.
static int dc_api(const char *method, const char *path, const char *body, int *status)
{
    char url[300], headers[700];
    const char *base = shell_getenv("DISCORD_API");

    if (!tsc_reached(g_backoff_until)) {   // still cooling down after a 429
        g_last_status = 429;
        if (status) *status = 429;
        snprintf(g_err, sizeof(g_err), "cooling down after a rate limit");
        g_resp[0] = '\0';
        return -1;
    }
    while (!tsc_reached(g_api_next)) __asm__ volatile("pause");

    snprintf(url, sizeof(url), "%s%s", (base && base[0]) ? base : API_BASE, path);
    snprintf(headers, sizeof(headers), "Authorization: %s\nUser-Agent: %s\n%s", g_token, is_bot() ? BOT_AGENT : USER_AGENT,
             body ? "Content-Type: application/json" : "");

    int st = 0;
    int n = rust_http_request((const uint8_t *)method, (const uint8_t *)url, (const uint8_t *)headers,
                              (const uint8_t *)body, body ? (uint32_t)strlen(body) : 0,
                              (uint8_t *)g_resp, RESP_MAX, &st);
    memset(headers, 0, sizeof(headers));
    g_api_next = tsc_deadline_ms(API_GAP_MS);
    g_last_status = st;
    if (status) *status = st;
    if (n < 0) { rust_http_error(g_err, sizeof(g_err)); g_resp[0] = '\0'; return -1; }
    g_resp[n] = '\0';

    if (st == 429) {
        // Honour retry_after, and back off further on repeated 429s.
        int ms = js_ms(js_get(g_resp, "retry_after")) + 250;
        if (ms < 1500) ms = 1500;
        if (++g_backoff_streak > 1) {
            int grown = 2000 << (g_backoff_streak < 7 ? g_backoff_streak - 1 : 6);
            if (grown > ms) ms = grown;
        }
        if (ms > BACKOFF_MAX_MS) ms = BACKOFF_MAX_MS;
        g_backoff_until = tsc_deadline_ms((uint32_t)ms);
        g_backoff_secs = (ms + 999) / 1000;
    } else if (st == 401) {
        g_auth_dead = true;
    } else if (st >= 200 && st < 300) {
        g_backoff_streak = 0;
    }
    return n;
}

// Downloads an image into g_bin without sending the token. Returns the length or -1.
static int http_bin(const char *url)
{
    char headers[160];
    int st = 0;
    snprintf(headers, sizeof(headers), "User-Agent: %s\nAccept: image/png,image/*", USER_AGENT);
    int n = rust_http_request((const uint8_t *)"GET", (const uint8_t *)url, (const uint8_t *)headers, NULL, 0,
                              g_bin, BIN_MAX, &st);
    return (n > 0 && st == 200) ? n : -1;
}

// ---- JSON (just enough to read Discord's replies) ------------------------------------------------------------

static const char *js_ws(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; return p; }

static const char *js_skip(const char *p)
{
    p = js_ws(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
        return *p ? p + 1 : p;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        do {
            if (*p == '"') { p = js_skip(p); continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') depth--;
            p++;
        } while (*p && depth > 0);
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n') p++;
    return p;
}

// Value of `key` in the object at obj, or NULL.
static const char *js_get(const char *obj, const char *key)
{
    if (!obj) return NULL;
    const char *p = js_ws(obj);
    if (*p != '{') return NULL;
    p++;
    size_t kl = strlen(key);
    for (;;) {
        p = js_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p != '"') return NULL;
        const char *k = p + 1;
        const char *after = js_skip(p);
        size_t len = (size_t)(after - k) - 1;
        p = js_ws(after);
        if (*p != ':') return NULL;
        p = js_ws(p + 1);
        if (len == kl && strncmp(k, key, kl) == 0) return p;
        p = js_skip(p);
    }
}

static const char *js_first(const char *arr)
{
    if (!arr) return NULL;
    const char *p = js_ws(arr);
    if (*p != '[') return NULL;
    p = js_ws(p + 1);
    return *p == ']' || !*p ? NULL : p;
}

static const char *js_next(const char *elem)
{
    const char *p = js_ws(js_skip(elem));
    return *p == ',' ? js_ws(p + 1) : NULL;
}

// Decodes one UTF-8 sequence and advances *p (bad bytes give U+FFFD; never reads past the terminator).
static unsigned utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    unsigned c = s[0], cp, extra, i;
    if (c < 0x80) { *p += 1; return c; }
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { *p += 1; return 0xFFFD; }
    for (i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p += i; return 0xFFFD; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p += extra + 1;
    return cp;
}

static int utf8_put(unsigned cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static unsigned hex4(const char *s)
{
    unsigned cp = 0;
    for (int i = 0; i < 4 && s[i]; i++) {
        char h = s[i];
        cp = (cp << 4) | (unsigned)((h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 :
                                    (h >= 'A' && h <= 'F') ? h - 'A' + 10 : 0);
    }
    return cp;
}

// Decodes the JSON string at v into out as UTF-8 (tabs become spaces, CRs vanish). Returns its length.
static size_t js_utf8(const char *v, char *out, size_t cap)
{
    size_t n = 0;
    if (cap == 0) return 0;
    out[0] = '\0';
    if (!v || *v != '"') return 0;
    v++;
    while (*v && *v != '"' && n + 5 < cap) {
        unsigned cp;
        if (*v == '\\') {
            v++;
            if (*v == 'n') { cp = '\n'; v++; }
            else if (*v == 't') { cp = ' '; v++; }
            else if (*v == 'r') { v++; continue; }
            else if (*v == 'u') {
                cp = hex4(v + 1);
                v += 5;
                if (cp >= 0xD800 && cp < 0xDC00 && v[0] == '\\' && v[1] == 'u') {
                    unsigned lo = hex4(v + 2);
                    if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); v += 6; }
                }
            } else if (*v) { cp = (unsigned char)*v; v++; }
            else break;
        } else cp = utf8_next(&v);
        n += (size_t)utf8_put(cp, out + n);
    }
    out[n] = '\0';
    return n;
}

// Decodes the JSON string at v into out as plain ASCII (other characters become '?'). Returns its length.
static size_t js_str(const char *v, char *out, size_t cap)
{
    size_t n = 0;
    if (cap == 0) return 0;
    if (!v || *v != '"') { out[0] = '\0'; return 0; }
    v++;
    while (*v && *v != '"') {
        unsigned char c = (unsigned char)*v;
        char put = 0;
        if (c == '\\') {
            v++;
            switch (*v) {
            case 'n': put = '\n'; break;
            case 't': put = ' '; break;
            case 'r': put = 0; break;
            case 'u': {
                unsigned cp = 0;
                for (int i = 1; i <= 4 && v[i]; i++) {
                    char h = v[i];
                    cp = (cp << 4) | (unsigned)((h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 :
                                                (h >= 'A' && h <= 'F') ? h - 'A' + 10 : 0);
                }
                v += 4;
                put = cp < 128 ? (char)cp : '?';
                if (cp >= 0xD800 && cp < 0xDC00 && v[1] == '\\' && v[2] == 'u') v += 6;
                break;
            }
            default: put = *v; break;
            }
            if (*v) v++;
        } else if (c >= 0x80) {
            put = '?';
            v++;
            while (((unsigned char)*v & 0xC0) == 0x80) v++;
        } else {
            put = (char)c;
            v++;
        }
        if (put && n + 1 < cap) out[n++] = put;
    }
    out[n] = '\0';
    return n;
}

static int js_int(const char *v)
{
    int n = 0;
    if (!v) return 0;
    if (*v == '"') v++;
    while (*v >= '0' && *v <= '9') n = n * 10 + (*v++ - '0');
    return n;
}

// Escapes UTF-8 text for a JSON string body (non-ASCII becomes \uXXXX, with surrogate pairs).
static void js_escape(const char *in, char *out, size_t cap)
{
    static const char HEX[] = "0123456789abcdef";
    size_t n = 0;
    while (*in && n + 13 < cap) {
        unsigned char c = (unsigned char)*in;
        if (c >= 0x80) {
            unsigned cp = utf8_next(&in);
            unsigned u[2] = { cp, 0 };
            int cnt = 1;
            if (cp >= 0x10000) { cp -= 0x10000; u[0] = 0xD800 + (cp >> 10); u[1] = 0xDC00 + (cp & 0x3FF); cnt = 2; }
            for (int k = 0; k < cnt; k++) {
                out[n++] = '\\'; out[n++] = 'u';
                for (int s = 12; s >= 0; s -= 4) out[n++] = HEX[(u[k] >> s) & 15];
            }
            continue;
        }
        in++;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
        else if (c >= 0x20 && c < 0x7F) out[n++] = (char)c;
    }
    out[n] = '\0';
}

// A string value, or "" for null / missing.
static void js_text(const char *obj, const char *key, char *out, size_t cap)
{
    const char *v = js_get(obj, key);
    if (v && *v == '"') js_str(v, out, cap);
    else out[0] = '\0';
}

// ---- small helpers -------------------------------------------------------------------------------------------------

static void copy_str(char *dst, size_t cap, const char *src)
{
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

// 12345 -> "12,345"
static void commas(int n, char *out, size_t cap)
{
    char digits[16];
    itoa(n, digits, 10);
    size_t len = strlen(digits), o = 0;
    for (size_t i = 0; i < len && o + 2 < cap; i++) {
        if (i > 0 && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = digits[i];
    }
    out[o] = '\0';
}

static bool contains_nocase(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0) return true;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i]) {
            char a = hay[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            i++;
        }
        if (i == nl) return true;
    }
    return false;
}

// "2026-10-31" -> "Oct 31, 2026"
static void pretty_date(const char *iso, char *out, size_t cap)
{
    static const char *const M[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (strlen(iso) < 10) { copy_str(out, cap, iso); return; }
    int mo = (iso[5] - '0') * 10 + (iso[6] - '0');
    if (mo < 1 || mo > 12) { copy_str(out, cap, iso); return; }
    char year[5] = { iso[0], iso[1], iso[2], iso[3], 0 };
    snprintf(out, cap, "%s %d, %s", M[mo - 1], (iso[8] - '0') * 10 + (iso[9] - '0'), year);
}

// ---- settings ----------------------------------------------------------------------------------------------------

#define SET_FILE   "/disk/dcset.txt"
#define SOUND_FILE "/disk/dcsound.wav"

typedef struct {
    bool notify, n_dm, n_mention, n_open;
    bool sound;
    int  sound_kind;    // 0 ping, 1 chime, 2 pop, 3 bell, 4 the WAV in SOUND_FILE
    int  volume;        // percent
    int  refresh_s;     // how often the open chat is refreshed
    int  dm_poll_s;     // how often DMs are checked for new messages; 0 = never
    bool images;        // show image attachments inline
    bool plugins;
} dcset_t;

static dcset_t g_set;

static void set_defaults(void)
{
    g_set = (dcset_t){ .notify = true, .n_dm = true, .n_mention = true, .n_open = false, .sound = true, .sound_kind = 0,
                       .volume = 60, .refresh_s = 8, .dm_poll_s = 60, .images = true, .plugins = false };
}

static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void set_load(void)
{
    static char buf[512];
    set_defaults();
    int n = avfs_file_exists(SET_FILE) ? avfs_get_filesize(SET_FILE) : 0;
    if (n <= 0 || n >= (int)sizeof(buf) || avfs_read_file(SET_FILE, buf, (uint32_t)n, 0) < 0) return;
    buf[n] = '\0';
    for (char *line = buf; *line; ) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = '\0';
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            int v = js_int(eq + 1);
            const char *k = line;
            if (!strcmp(k, "nt")) g_set.notify = v != 0;
            else if (!strcmp(k, "dm")) g_set.n_dm = v != 0;
            else if (!strcmp(k, "mn")) g_set.n_mention = v != 0;
            else if (!strcmp(k, "op")) g_set.n_open = v != 0;
            else if (!strcmp(k, "sn")) g_set.sound = v != 0;
            else if (!strcmp(k, "sk")) g_set.sound_kind = clamp_int(v, 0, 4);
            else if (!strcmp(k, "vol")) g_set.volume = clamp_int(v, 10, 100);
            else if (!strcmp(k, "rf")) g_set.refresh_s = clamp_int(v, 5, 60);
            else if (!strcmp(k, "dp")) g_set.dm_poll_s = v == 0 ? 0 : clamp_int(v, 30, 600);
            else if (!strcmp(k, "im")) g_set.images = v != 0;
            else if (!strcmp(k, "pl")) g_set.plugins = v != 0;
        }
        if (!eol) break;
        line = eol + 1;
    }
}

static void set_save(void)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "nt=%d\ndm=%d\nmn=%d\nop=%d\nsn=%d\nsk=%d\nvol=%d\nrf=%d\ndp=%d\nim=%d\npl=%d\n",
             g_set.notify, g_set.n_dm, g_set.n_mention, g_set.n_open, g_set.sound, g_set.sound_kind, g_set.volume,
             g_set.refresh_s, g_set.dm_poll_s, g_set.images, g_set.plugins);
    avfs_save_file(SET_FILE, buf, (uint32_t)strlen(buf), true);
}

// ---- sound effects (AC97 through the Rust library; PC speaker if there is no codec) -------------------------------

static const char *const SOUND_NAMES[] = { "Ping", "Chime", "Pop", "Bell", "Custom WAV" };

static uint64_t g_snd_until;

// Plays one effect (kind as in dcset_t); effects do not overlap.
static void play_sound(int kind)
{
    if (!tsc_reached(g_snd_until)) return;
    int ms;
    if (kind == 4) {
        // fetch -o /disk/dcsound.wav <url> stores into the raw fetch store; a file copied onto the FAT disk works too
        int n = 0;
        if (avfs_file_exists(SOUND_FILE)) {
            n = avfs_get_filesize(SOUND_FILE);
            if (n > BIN_MAX) n = BIN_MAX;
            if (n > 44 && avfs_read_file(SOUND_FILE, g_bin, (uint32_t)n, 0) < 0) n = 0;
        } else {
            n = rust_disk_store_size((const uint8_t *)SOUND_FILE);
            if (n > BIN_MAX) n = BIN_MAX;
            if (n > 44 && rust_disk_store_read((const uint8_t *)SOUND_FILE, 0, g_bin, (uint32_t)n) < 0) n = 0;
        }
        ms = n > 44 ? rust_sfx_wav(g_bin, (uint32_t)n, (uint32_t)g_set.volume) : rust_sfx_builtin(0, (uint32_t)g_set.volume);
        if (ms == -2) ms = rust_sfx_builtin(0, (uint32_t)g_set.volume);   // not a PCM WAV: fall back to the ping
    } else {
        ms = rust_sfx_builtin((uint32_t)kind, (uint32_t)g_set.volume);
    }
    if (ms < 0) { speaker_play_notification_sound(); ms = 250; }
    g_snd_until = tsc_deadline_ms((uint32_t)ms + 40);
}

// ---- data ------------------------------------------------------------------------------------------------------

#define MAX_GUILDS 400
#define MAX_DMS    200
#define MAX_ROWS   600
#define MAX_MSGS   600
#define PAGE_MSGS  50     // messages per request (the official client asks for 50)

typedef struct { char id[24]; char name[48]; char icon[40]; int members, online; } guild_t;
typedef struct { char id[24]; char name[48]; char uid[24]; char avatar[40]; char last[24]; char seen[24]; int unread; } dm_t;
typedef struct { char id[24]; char name[48]; int type; int pos; char parent[24]; char topic[100]; } chan_t;
enum { ROW_CAT, ROW_TEXT, ROW_VOICE };
typedef struct { uint8_t kind; char name[48]; char id[24]; char topic[100]; } lrow_t;
typedef struct { char url[300]; char name[48]; int w, h; bool image; } att_t;
#define MAX_EMO 10
#define EMO_MARK 1      // text holds EMO_MARK, 0x10 + index where an emoji image goes
typedef struct {
    char id[24], uid[24], avatar[40], who[32], when[6], date[11];
    char text[1024];
    char emo[MAX_EMO][48];   // "c:<id>" custom, "u:<hex code points>" unicode
    int n_emo;
    att_t att[2];
    int n_att;
    char etitle[96], edesc[300];
    bool has_embed;
    bool mentioned;          // pings me (or everyone)
    bool reply;
    char reply_who[32], reply_text[72];
} msg_t;

static guild_t g_guild[MAX_GUILDS];
static dm_t g_dm[MAX_DMS];
static lrow_t g_lrow[MAX_ROWS];
static chan_t g_chtmp[MAX_ROWS];
static msg_t g_msg[MAX_MSGS];        // oldest first
static msg_t g_batch[PAGE_MSGS];
static int n_guild, n_dm, n_lrow, n_msg;

#define MAX_GEMOJI 300
typedef struct { char id[24]; char name[32]; bool animated; } gemoji_t;
static gemoji_t g_gemoji[MAX_GEMOJI];   // custom emoji of the open server
static int n_gemoji;

static bool     g_hist_end;          // the start of the conversation is loaded
static bool     g_hist_want;         // the view is at the top and more could be loaded
static uint64_t g_hist_next, g_hist_win;
static int      g_hist_burst;
static uint64_t g_send_next, g_send_win;
static int      g_send_burst;
static int      g_refresh_fail;
static uint32_t g_dup_hash;
static uint64_t g_dup_until;
static uint64_t g_join_next, g_join_win;
static int      g_join_burst;

static bool can_load_older(void) { return !g_hist_end && n_msg > 0 && n_msg + PAGE_MSGS <= MAX_MSGS; }

static void name_of(const char *obj, char *out, size_t cap)
{
    char tmp[48];
    const char *g = js_get(obj, "global_name");
    if (g && *g == '"' && js_str(g, tmp, sizeof(tmp))) { copy_str(out, cap, tmp); return; }
    js_text(obj, "username", out, cap);
}

static bool dc_load_me(int *status)
{
    if (dc_api("GET", "/users/@me", NULL, status) < 0 || *status != 200) return false;
    name_of(g_resp, g_me, sizeof(g_me));
    js_text(g_resp, "id", g_me_id, sizeof(g_me_id));
    js_text(g_resp, "avatar", g_me_avatar, sizeof(g_me_avatar));
    return g_me[0] != '\0';
}

// Newer ids (snowflakes) are longer or sort higher as text.
static int id_cmp(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return la < lb ? -1 : 1;
    return strcmp(a, b);
}

static bool dc_load_dms(void)
{
    static dm_t old[MAX_DMS];
    int st, n_old = n_dm;
    memcpy(old, g_dm, (size_t)n_dm * sizeof(dm_t));
    n_dm = 0;
    if (dc_api("GET", "/users/@me/channels", NULL, &st) < 0 || st != 200) { memcpy(g_dm, old, (size_t)n_old * sizeof(dm_t)); n_dm = n_old; return false; }
    for (const char *ch = js_first(g_resp); ch && n_dm < MAX_DMS; ch = js_next(ch)) {
        dm_t *it = &g_dm[n_dm];
        memset(it, 0, sizeof(*it));
        js_text(ch, "id", it->id, sizeof(it->id));
        js_text(ch, "last_message_id", it->last, sizeof(it->last));
        copy_str(it->seen, sizeof(it->seen), it->last);   // a conversation seen for the first time is not news
        for (int k = 0; k < n_old; k++) {
            if (strcmp(old[k].id, it->id) == 0) { copy_str(it->seen, sizeof(it->seen), old[k].seen); it->unread = old[k].unread; break; }
        }
        const char *r = js_first(js_get(ch, "recipients"));
        if (r) { js_text(r, "id", it->uid, sizeof(it->uid)); js_text(r, "avatar", it->avatar, sizeof(it->avatar)); }
        js_text(ch, "name", it->name, sizeof(it->name));
        if (!it->name[0]) {
            char a[24] = "", b[24] = "";
            if (r) { name_of(r, a, sizeof(a)); const char *r2 = js_next(r); if (r2) name_of(r2, b, sizeof(b)); }
            snprintf(it->name, sizeof(it->name), "%s%s%s", a, b[0] ? ", " : "", b);
        }
        if (it->id[0] && it->name[0]) {
            int i = n_dm++;   // most recent conversation first
            while (i > 0 && id_cmp(g_dm[i - 1].last, g_dm[i].last) < 0) { dm_t t = g_dm[i]; g_dm[i] = g_dm[i - 1]; g_dm[i - 1] = t; i--; }
        }
    }
    return true;
}

// All servers, a page of 200 at a time, with member and online counts.
static bool dc_load_guilds(void)
{
    int st;
    char path[160];
    n_guild = 0;
    for (int page = 0; page < 4; page++) {
        if (n_guild == 0) snprintf(path, sizeof(path), "/users/@me/guilds?with_counts=true&limit=200");
        else snprintf(path, sizeof(path), "/users/@me/guilds?with_counts=true&limit=200&after=%s", g_guild[n_guild - 1].id);
        if (dc_api("GET", path, NULL, &st) < 0 || st != 200) return n_guild > 0;
        int got = 0;
        for (const char *g = js_first(g_resp); g && n_guild < MAX_GUILDS; g = js_next(g)) {
            guild_t *it = &g_guild[n_guild];
            memset(it, 0, sizeof(*it));
            js_text(g, "id", it->id, sizeof(it->id));
            js_text(g, "name", it->name, sizeof(it->name));
            js_text(g, "icon", it->icon, sizeof(it->icon));
            it->members = js_int(js_get(g, "approximate_member_count"));
            it->online = js_int(js_get(g, "approximate_presence_count"));
            got++;
            if (it->id[0]) n_guild++;
        }
        if (got < 200) break;
    }
    return true;
}

static int chan_group(const chan_t *c) { return (c->type == 2 || c->type == 13) ? 1 : 0; }   // voice after text

// Channels of a server in Discord's order: uncategorised first, then each category with its channels.
static bool dc_load_channels(const char *guild_id)
{
    int st, n = 0;
    char path[64];
    n_lrow = 0;
    snprintf(path, sizeof(path), "/guilds/%s/channels", guild_id);
    if (dc_api("GET", path, NULL, &st) < 0 || st != 200) return false;
    for (const char *c = js_first(g_resp); c && n < MAX_ROWS; c = js_next(c)) {
        chan_t *t = &g_chtmp[n];
        memset(t, 0, sizeof(*t));
        t->type = js_int(js_get(c, "type"));
        js_text(c, "id", t->id, sizeof(t->id));
        js_text(c, "name", t->name, sizeof(t->name));
        js_text(c, "parent_id", t->parent, sizeof(t->parent));
        js_text(c, "topic", t->topic, sizeof(t->topic));
        t->pos = js_int(js_get(c, "position"));
        if (t->id[0]) n++;
    }
    // sort by (group, position): categories are their own list
    for (int i = 1; i < n; i++) {
        chan_t t = g_chtmp[i];
        int j = i;
        while (j > 0 && (chan_group(&g_chtmp[j - 1]) > chan_group(&t) ||
                         (chan_group(&g_chtmp[j - 1]) == chan_group(&t) && g_chtmp[j - 1].pos > t.pos))) { g_chtmp[j] = g_chtmp[j - 1]; j--; }
        g_chtmp[j] = t;
    }
    // uncategorised channels
    for (int i = 0; i < n && n_lrow < MAX_ROWS; i++) {
        chan_t *t = &g_chtmp[i];
        if (t->type == 4 || t->parent[0]) continue;
        lrow_t *r = &g_lrow[n_lrow++];
        r->kind = chan_group(t) ? ROW_VOICE : ROW_TEXT;
        copy_str(r->name, sizeof(r->name), t->name); copy_str(r->id, sizeof(r->id), t->id); copy_str(r->topic, sizeof(r->topic), t->topic);
    }
    // categories by position, each followed by its children
    int cats[128], nc = 0;
    for (int i = 0; i < n && nc < 128; i++) if (g_chtmp[i].type == 4) cats[nc++] = i;
    for (int i = 1; i < nc; i++) { int v = cats[i], j = i; while (j > 0 && g_chtmp[cats[j - 1]].pos > g_chtmp[v].pos) { cats[j] = cats[j - 1]; j--; } cats[j] = v; }
    for (int k = 0; k < nc && n_lrow < MAX_ROWS; k++) {
        const chan_t *cat = &g_chtmp[cats[k]];
        lrow_t *h = &g_lrow[n_lrow++];
        h->kind = ROW_CAT;
        copy_str(h->name, sizeof(h->name), cat->name); copy_str(h->id, sizeof(h->id), cat->id); h->topic[0] = '\0';
        for (int i = 0; i < n && n_lrow < MAX_ROWS; i++) {
            chan_t *t = &g_chtmp[i];
            if (t->type == 4 || strcmp(t->parent, cat->id) != 0) continue;
            lrow_t *r = &g_lrow[n_lrow++];
            r->kind = chan_group(t) ? ROW_VOICE : ROW_TEXT;
            copy_str(r->name, sizeof(r->name), t->name); copy_str(r->id, sizeof(r->id), t->id); copy_str(r->topic, sizeof(r->topic), t->topic);
        }
    }
    return true;
}

// Replaces <@id>, <@&id>, <#id> and <:emoji:id> in text by readable names (the mentions array resolves users).
static void expand_text(const char *mentions, char *text, size_t cap)
{
    char out[1024];
    size_t o = 0;
    for (size_t i = 0; text[i] && o + 1 < sizeof(out); ) {
        if (text[i] == '<') {
            const char *end = strchr(text + i, '>');
            if (end && end - (text + i) < 100) {
                const char *p = text + i + 1;
                char name[48] = "";
                if (*p == '@' && p[1] != '&') {
                    p++;
                    if (*p == '!') p++;
                    char id[24];
                    size_t il = 0;
                    while (p < end && il + 1 < sizeof(id)) id[il++] = *p++;
                    id[il] = '\0';
                    snprintf(name, sizeof(name), "@user");
                    for (const char *m = js_first(mentions); m; m = js_next(m)) {
                        char mid[24];
                        js_text(m, "id", mid, sizeof(mid));
                        if (strcmp(mid, id) == 0) { char who[40]; name_of(m, who, sizeof(who)); snprintf(name, sizeof(name), "@%s", who); break; }
                    }
                } else if (*p == '@') snprintf(name, sizeof(name), "@role");
                else if (*p == '#') snprintf(name, sizeof(name), "#channel");
                if (name[0]) {
                    for (size_t k = 0; name[k] && o + 1 < sizeof(out); k++) out[o++] = name[k];
                    i = (size_t)(end - text) + 1;
                    continue;
                }
            }
        }
        out[o++] = text[i++];
    }
    out[o] = '\0';
    copy_str(text, cap, out);
}

static bool is_emoji_cp(unsigned c)
{
    return (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) || (c >= 0x2194 && c <= 0x2199) ||
           c == 0x21A9 || c == 0x21AA || c == 0x231A || c == 0x231B || c == 0x2328 || c == 0x23CF ||
           (c >= 0x23E9 && c <= 0x23F3) || (c >= 0x23F8 && c <= 0x23FA) || c == 0x24C2 || c == 0x25AA || c == 0x25AB ||
           c == 0x25B6 || c == 0x25C0 || (c >= 0x25FB && c <= 0x25FE) || c == 0x2934 || c == 0x2935 ||
           (c >= 0x2B05 && c <= 0x2B07) || c == 0x2B1B || c == 0x2B1C || c == 0x2B50 || c == 0x2B55 ||
           c == 0x3030 || c == 0x303D || c == 0x3297 || c == 0x3299 || c == 0xA9 || c == 0xAE || c == 0x203C ||
           c == 0x2049 || c == 0x2122 || c == 0x2139;
}

static void hex_add(char *hex, size_t cap, unsigned cp)
{
    static const char D[] = "0123456789abcdef";
    char t[10];
    int n = 0;
    size_t len = strlen(hex);
    do { t[n++] = D[cp & 15]; cp >>= 4; } while (cp);
    if (len + (size_t)n + 2 > cap) return;
    if (len) hex[len++] = '-';
    while (n) hex[len++] = t[--n];
    hex[len] = '\0';
}

static bool is_keycap_base(unsigned c) { return c == '#' || c == '*' || (c >= '0' && c <= '9'); }

// The 8x8 font has no accented letters: fold them to ASCII (or NULL when there is no sensible fold).
static const char *ascii_fold(unsigned cp)
{
    if (cp >= 0xC0 && cp <= 0xC5) return "A";
    if (cp == 0xC6) return "AE";
    if (cp == 0xC7) return "C";
    if (cp >= 0xC8 && cp <= 0xCB) return "E";
    if (cp >= 0xCC && cp <= 0xCF) return "I";
    if (cp == 0xD1) return "N";
    if ((cp >= 0xD2 && cp <= 0xD6) || cp == 0xD8) return "O";
    if (cp >= 0xD9 && cp <= 0xDC) return "U";
    if (cp == 0xDD) return "Y";
    if (cp == 0xDF) return "ss";
    if (cp >= 0xE0 && cp <= 0xE5) return "a";
    if (cp == 0xE6) return "ae";
    if (cp == 0xE7) return "c";
    if (cp >= 0xE8 && cp <= 0xEB) return "e";
    if (cp >= 0xEC && cp <= 0xEF) return "i";
    if (cp == 0xF1) return "n";
    if ((cp >= 0xF2 && cp <= 0xF6) || cp == 0xF8) return "o";
    if (cp >= 0xF9 && cp <= 0xFC) return "u";
    if (cp == 0xFD || cp == 0xFF) return "y";
    if (cp == 0x2018 || cp == 0x2019) return "'";
    if (cp == 0x201C || cp == 0x201D) return "\"";
    if (cp == 0x2013 || cp == 0x2014) return "-";
    if (cp == 0x2026) return "...";
    if (cp == 0x2022) return "*";
    if (cp == 0xA0) return " ";
    return NULL;
}

// Reads the emoji sequence at *pp (one emoji with its variation selectors, skin tone, ZWJ parts or flag pair) into hex.
static bool read_emoji(const char **pp, char *hex, size_t cap)
{
    const char *p = *pp;
    unsigned cp = utf8_next(&p);
    hex[0] = '\0';
    if (is_keycap_base(cp)) {
        const char *q = p;
        unsigned n = utf8_next(&q);
        if (n == 0xFE0F) n = utf8_next(&q);
        if (n != 0x20E3) return false;
        hex_add(hex, cap, cp);
        hex_add(hex, cap, 0xFE0F);
        hex_add(hex, cap, 0x20E3);
        *pp = q;
        return true;
    }
    if (!is_emoji_cp(cp)) return false;
    hex_add(hex, cap, cp);
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF) {
        const char *q = p;
        unsigned n = utf8_next(&q);
        if (n >= 0x1F1E6 && n <= 0x1F1FF) { hex_add(hex, cap, n); p = q; }
    }
    for (;;) {
        const char *q = p;
        unsigned n = utf8_next(&q);
        if (n == 0xFE0F || n == 0x20E3 || (n >= 0x1F3FB && n <= 0x1F3FF)) { hex_add(hex, cap, n); p = q; continue; }
        if (n == 0x200D) {
            unsigned m = utf8_next(&q);
            if (is_emoji_cp(m)) { hex_add(hex, cap, 0x200D); hex_add(hex, cap, m); p = q; continue; }
        }
        break;
    }
    *pp = p;
    return true;
}

// UTF-8 message text to ASCII with emoji markers: <:name:id> and unicode emoji become EMO_MARK + slot.
static void to_rich(const char *in, msg_t *m)
{
    size_t o = 0, cap = sizeof(m->text);
    m->n_emo = 0;
    while (*in && o + 3 < cap) {
        char key[48];
        bool found = false;
        if (*in == '<') {
            const char *p = in + 1;
            if (*p == 'a') p++;
            if (*p == ':') {
                p++;
                while (*p && *p != ':' && *p != '>' && *p != ' ') p++;
                if (*p == ':') {
                    const char *id = ++p;
                    while (*p >= '0' && *p <= '9') p++;
                    if (*p == '>' && p > id && (size_t)(p - id) < 24) {
                        snprintf(key, sizeof(key), "c:");
                        memcpy(key + 2, id, (size_t)(p - id));
                        key[2 + (p - id)] = '\0';
                        in = p + 1;
                        found = true;
                    }
                }
            }
        }
        if (!found && ((unsigned char)*in >= 0x80 || is_keycap_base((unsigned char)*in))) {
            char hex[44];
            const char *p = in;
            if (read_emoji(&p, hex, sizeof(hex))) {
                snprintf(key, sizeof(key), "u:%s", hex);
                in = p;
                found = true;
            }
        }
        if (found) {
            if (m->n_emo < MAX_EMO) {
                copy_str(m->emo[m->n_emo], sizeof(m->emo[0]), key);
                m->text[o++] = EMO_MARK;
                m->text[o++] = (char)(0x10 + m->n_emo++);
            } else m->text[o++] = '?';
            continue;
        }
        if ((unsigned char)*in >= 0x80) {
            unsigned cp = utf8_next(&in);
            const char *fold = ascii_fold(cp);
            if (fold) { while (*fold && o + 3 < cap) m->text[o++] = *fold++; }
            else if (cp != 0xFE0F && cp != 0x200D) m->text[o++] = '?';
        } else m->text[o++] = *in++;
    }
    m->text[o] = '\0';
}

static void parse_message(const char *m, msg_t *out)
{
    char ts[40];
    memset(out, 0, sizeof(*out));
    js_text(m, "id", out->id, sizeof(out->id));
    js_text(m, "timestamp", ts, sizeof(ts));
    strcpy(out->when, "--:--");
    if (strlen(ts) >= 16) { memcpy(out->when, ts + 11, 5); out->when[5] = '\0'; memcpy(out->date, ts, 10); out->date[10] = '\0'; }
    const char *au = js_get(m, "author");
    name_of(au, out->who, sizeof(out->who));
    js_text(au, "id", out->uid, sizeof(out->uid));
    js_text(au, "avatar", out->avatar, sizeof(out->avatar));
    {
        static char utf[1100];
        js_utf8(js_get(m, "content"), utf, sizeof(utf));
        expand_text(js_get(m, "mentions"), utf, sizeof(utf));
        to_rich(utf, out);
    }

    for (const char *a = js_first(js_get(m, "attachments")); a && out->n_att < 2; a = js_next(a)) {
        att_t *t = &out->att[out->n_att++];
        char type[40];
        js_text(a, "filename", t->name, sizeof(t->name));
        js_text(a, "content_type", type, sizeof(type));
        js_text(a, "proxy_url", t->url, sizeof(t->url));
        if (!t->url[0]) js_text(a, "url", t->url, sizeof(t->url));
        t->w = js_int(js_get(a, "width"));
        t->h = js_int(js_get(a, "height"));
        t->image = strncmp(type, "image/", 6) == 0 && t->w > 0 && t->h > 0 && t->url[0];
    }
    const char *e = js_first(js_get(m, "embeds"));
    if (e) {
        js_text(e, "title", out->etitle, sizeof(out->etitle));
        js_text(e, "description", out->edesc, sizeof(out->edesc));
        out->has_embed = out->etitle[0] || out->edesc[0];
    }
    const char *ref = js_get(m, "referenced_message");
    if (ref && *ref == '{') {
        out->reply = true;
        name_of(js_get(ref, "author"), out->reply_who, sizeof(out->reply_who));
        js_text(ref, "content", out->reply_text, sizeof(out->reply_text));
        if (!out->reply_text[0]) copy_str(out->reply_text, sizeof(out->reply_text), "Click to see attachment");
    }
    if (out->text[0] == '\0' && !out->n_att && !out->has_embed) copy_str(out->text, sizeof(out->text), "[message]");
    {
        const char *ev = js_get(m, "mention_everyone");
        out->mentioned = ev && strncmp(ev, "true", 4) == 0;
        for (const char *u = js_first(js_get(m, "mentions")); u && !out->mentioned; u = js_next(u)) {
            char mid[24];
            js_text(u, "id", mid, sizeof(mid));
            if (g_me_id[0] && strcmp(mid, g_me_id) == 0) out->mentioned = true;
        }
    }
}

// Parses a reply (newest first) into g_batch. Returns the count.
static int parse_batch(void)
{
    int count = 0;
    for (const char *m = js_first(g_resp); m && count < PAGE_MSGS; m = js_next(m)) parse_message(m, &g_batch[count++]);
    return count;
}

// Loads the newest page and merges it in: older pages already loaded are kept, edits and deletions in the page show up.
static bool dc_load_messages(const char *channel_id)
{
    int st;
    char path[96];
    snprintf(path, sizeof(path), "/channels/%s/messages?limit=%d", channel_id, PAGE_MSGS);
    if (dc_api("GET", path, NULL, &st) < 0 || st != 200) return false;

    int count = parse_batch();
    bool first = n_msg == 0;
    if (count == 0) { if (first) g_hist_end = true; return true; }

    int keep = 0;   // loaded messages older than this page
    while (keep < n_msg && id_cmp(g_msg[keep].id, g_batch[count - 1].id) < 0) keep++;
    if (keep + count > MAX_MSGS) {
        int drop = keep + count - MAX_MSGS;
        memmove(g_msg, g_msg + drop, (size_t)(keep - drop) * sizeof(msg_t));
        keep -= drop;
    }
    for (int i = 0; i < count; i++) g_msg[keep + i] = g_batch[count - 1 - i];
    n_msg = keep + count;
    if (first && count < PAGE_MSGS) g_hist_end = true;
    return true;
}

// Fetches only messages newer than the newest loaded one (?after=<id>); an idle channel costs a tiny "[]" reply.
static bool dc_load_newer(const char *channel_id)
{
    int st;
    char path[120];
    if (n_msg == 0) return dc_load_messages(channel_id);
    snprintf(path, sizeof(path), "/channels/%s/messages?limit=%d&after=%s", channel_id, PAGE_MSGS, g_msg[n_msg - 1].id);
    if (dc_api("GET", path, NULL, &st) < 0 || st != 200) return false;

    int count = parse_batch();
    if (count == 0) return true;
    if (n_msg + count > MAX_MSGS) {
        int drop = n_msg + count - MAX_MSGS;
        memmove(g_msg, g_msg + drop, (size_t)(n_msg - drop) * sizeof(msg_t));
        n_msg -= drop;
    }
    for (int i = 0; i < count; i++) g_msg[n_msg + i] = g_batch[count - 1 - i];
    n_msg += count;
    return true;
}

// Fetches the page before the oldest loaded message (?before=<id>). Returns false on a failed request.
static bool dc_load_older(const char *channel_id)
{
    int st;
    char path[120];
    snprintf(path, sizeof(path), "/channels/%s/messages?limit=%d&before=%s", channel_id, PAGE_MSGS, g_msg[0].id);
    if (dc_api("GET", path, NULL, &st) < 0 || st != 200) return false;

    int count = parse_batch();
    if (count > 0) {
        memmove(g_msg + count, g_msg, (size_t)n_msg * sizeof(msg_t));
        for (int i = 0; i < count; i++) g_msg[i] = g_batch[count - 1 - i];
        n_msg += count;
    }
    if (count < PAGE_MSGS) g_hist_end = true;
    return true;
}

static bool dc_send(const char *channel_id, const char *text, int *status)
{
    static char body[4300];
    static char esc[4100];
    char path[64];
    js_escape(text, esc, sizeof(esc));
    snprintf(body, sizeof(body), "{\"content\":\"%s\"}", esc);
    snprintf(path, sizeof(path), "/channels/%s/messages", channel_id);
    int n = dc_api("POST", path, body, status);
    return n >= 0 && *status == 200;
}

// Why the last call failed, for the status line.
static void explain_failure(char *out, size_t cap)
{
    int status = g_last_status;
    char why[80];
    js_text(g_resp, "message", why, sizeof(why));
    if (status == 0) snprintf(out, cap, "network error: %s", g_err);
    else if (status == 401) snprintf(out, cap, "Discord rejected the token (401); it may be wrong or expired");
    else if (status == 429) {
        if (g_backoff_secs > 0) snprintf(out, cap, "rate limited by Discord: pausing requests for up to %d s", g_backoff_secs);
        else snprintf(out, cap, "rate limited by Discord (429)");
    } else if (why[0]) snprintf(out, cap, "Discord says: %s (%d)", why, status);
    else if (status == 403) snprintf(out, cap, "forbidden (403): no access to that");
    else snprintf(out, cap, "Discord answered HTTP %d", status);
}

// The custom emoji of a server (needs a user in it, or a bot with access).
static void dc_load_emojis(const char *guild_id)
{
    int st;
    char path[64];
    n_gemoji = 0;
    snprintf(path, sizeof(path), "/guilds/%s/emojis", guild_id);
    if (dc_api("GET", path, NULL, &st) < 0 || st != 200) return;
    for (const char *e = js_first(g_resp); e && n_gemoji < MAX_GEMOJI; e = js_next(e)) {
        gemoji_t *it = &g_gemoji[n_gemoji];
        memset(it, 0, sizeof(*it));
        js_text(e, "id", it->id, sizeof(it->id));
        js_text(e, "name", it->name, sizeof(it->name));
        const char *a = js_get(e, "animated");
        it->animated = a && strncmp(a, "true", 4) == 0;
        const char *av = js_get(e, "available");
        if (it->id[0] && it->name[0] && !(av && strncmp(av, "false", 5) == 0)) n_gemoji++;
    }
}

// Pulls the invite code out of "discord.gg/abc", "https://discord.com/invite/abc?x=1" or a bare code.
static bool parse_invite(const char *in, char *code, size_t cap)
{
    const char *s = in;
    for (const char *p = in; *p; p++) if (*p == '/') s = p + 1;
    size_t n = 0;
    while (s[n] && s[n] != '?' && s[n] != '#' && s[n] != ' ') n++;
    if (n < 2 || n + 1 > cap || n > 32) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    memcpy(code, s, n);
    code[n] = '\0';
    return true;
}

// Looks an invite up without joining it. Fills the name, id and counts.
static bool dc_invite_preview(const char *code, char *gid, size_t gid_cap, char *name, size_t name_cap, int *members, int *online)
{
    int st;
    char path[96];
    snprintf(path, sizeof(path), "/invites/%s?with_counts=true", code);
    if (dc_api("GET", path, NULL, &st) < 0 || st != 200) return false;
    const char *g = js_get(g_resp, "guild");
    js_text(g, "id", gid, gid_cap);
    js_text(g, "name", name, name_cap);
    *members = js_int(js_get(g_resp, "approximate_member_count"));
    *online = js_int(js_get(g_resp, "approximate_presence_count"));
    return name[0] != '\0';
}

// Accepts an invite as the logged-in user. Discord may demand a captcha, which this client cannot show.
static bool dc_invite_join(const char *code, bool *captcha)
{
    int st;
    char path[64];
    *captcha = false;
    snprintf(path, sizeof(path), "/invites/%s", code);
    if (dc_api("POST", path, "{}", &st) < 0) return false;
    if (st == 200) return true;
    *captcha = strstr(g_resp, "captcha") != NULL;
    return false;
}

// ---- images (avatars, server icons, attachments) -------------------------------------------------------------------

#define IMG_SLOTS 400
#define IMG_POOL  (1600 * 1024)   // pixels
#define MAX_REQ   40

typedef struct { char key[80]; gui_image_t img; uint8_t state; } img_t;   // state: 0 free, 1 ready, 2 failed
typedef struct { char key[80]; char url[420]; int w, h; } req_t;

static img_t g_img[IMG_SLOTS];
static uint32_t g_pool[IMG_POOL];
static size_t g_pool_used;
static req_t g_req[MAX_REQ];
static int n_req;

static uint64_t g_img_next;
static void img_reset(void)
{
    memset(g_img, 0, sizeof(g_img));
    g_pool_used = 0;
}

static img_t *img_find(const char *key)
{
    for (int i = 0; i < IMG_SLOTS; i++) if (g_img[i].state && strcmp(g_img[i].key, key) == 0) return &g_img[i];
    return NULL;
}

// The decoded image if it is ready; otherwise queues the download (once) and returns NULL.
static const gui_image_t *img_get(const char *key, const char *url, int w, int h)
{
    img_t *i = img_find(key);
    if (i) return i->state == 1 ? &i->img : NULL;
    for (int k = 0; k < n_req; k++) if (strcmp(g_req[k].key, key) == 0) return NULL;
    if (n_req < MAX_REQ) {
        req_t *r = &g_req[n_req++];
        copy_str(r->key, sizeof(r->key), key);
        copy_str(r->url, sizeof(r->url), url);
        r->w = w;
        r->h = h;
    }
    return NULL;
}

// A free cache slot for key, marked failed until a decode fills it in.
static img_t *img_slot_for(const char *key)
{
    img_t *slot = NULL;
    for (int i = 0; i < IMG_SLOTS && !slot; i++) if (!g_img[i].state) slot = &g_img[i];
    if (!slot) { img_reset(); slot = &g_img[0]; }
    copy_str(slot->key, sizeof(slot->key), key);
    slot->state = 2;
    return slot;
}

// Decodes PNG data into the cache under key, scaled to fit w x h.
static void img_decode(const char *key, const uint8_t *data, uint32_t n, int w, int h)
{
    img_t *slot = img_slot_for(key);
    uint32_t pw = 0, ph = 0;
    int rc = rust_png_decode_fit(data, n, (uint32_t)w, (uint32_t)h, g_pool + g_pool_used,
                                 (uint32_t)(IMG_POOL - g_pool_used), &pw, &ph);
    if (rc == -2) {   // pool full: start over
        img_reset();
        slot = img_slot_for(key);
        rc = rust_png_decode_fit(data, n, (uint32_t)w, (uint32_t)h, g_pool, IMG_POOL, &pw, &ph);
    }
    if (rc == 0) {
        slot->img.px = g_pool + g_pool_used;
        slot->img.w = (int)pw;
        slot->img.h = (int)ph;
        slot->state = 1;
        g_pool_used += (size_t)pw * ph;
    }
}

// Downloads and decodes one queued image (the newest request first: it is the one on screen). True if one was done.
static bool img_service(void)
{
    if (n_req == 0) return false;
    req_t r = g_req[--n_req];
    g_img_next = tsc_deadline_ms(IMG_GAP_MS);

    int n = http_bin(r.url);
    if (n > 0) img_decode(r.key, g_bin, (uint32_t)n, r.w, r.h);
    else img_slot_for(r.key);
    return true;
}

static const char *cdn_base(void)
{
    const char *e = shell_getenv("DISCORD_CDN");
    return (e && e[0]) ? e : CDN_BASE;
}

static const gui_image_t *avatar_img(const char *uid, const char *hash, int size)
{
    char key[80], url[420];
    if (!hash[0] || !uid[0]) return NULL;
    snprintf(key, sizeof(key), "a:%s:%s:%d", uid, hash, size);
    snprintf(url, sizeof(url), "%s/avatars/%s/%s.png?size=64", cdn_base(), uid, hash);
    return img_get(key, url, size, size);
}

static const gui_image_t *guild_icon_img(const guild_t *g)
{
    char key[80], url[420];
    if (!g->icon[0]) return NULL;
    snprintf(key, sizeof(key), "g:%s:%s", g->id, g->icon);
    snprintf(url, sizeof(url), "%s/icons/%s/%s.png?size=64", cdn_base(), g->id, g->icon);
    return img_get(key, url, 48, 48);
}

// Image attachments come through the media proxy, which resizes and converts to PNG.
static const gui_image_t *attachment_img(const att_t *a, int w, int h)
{
    char key[80], url[420];
    const char *path = strstr(a->url, "/attachments/");
    {
        // cache key: width first, then the tail of the path (this printf has no precision support)
        const char *p = path ? path : a->url;
        size_t pl = strlen(p);
        if (pl > 60) p += pl - 60;
        snprintf(key, sizeof(key), "f:%d:%s", w, p);
    }
    snprintf(url, sizeof(url), "%s%cformat=png&width=%d&height=%d", a->url, strchr(a->url, '?') ? '&' : '?', w, h);
    return img_get(key, url, w, h);
}

// Twemoji file name for a code point list: variation selectors are dropped unless the sequence has a ZWJ.
static void twemoji_file(const char *hex, char *out, size_t cap)
{
    bool zwj = strstr(hex, "200d") != NULL;
    size_t o = 0;
    while (*hex && o + 1 < cap) {
        const char *end = hex;
        while (*end && *end != '-') end++;
        size_t len = (size_t)(end - hex);
        if (!(len == 4 && strncmp(hex, "fe0f", 4) == 0 && !zwj)) {
            if (o && o + 1 < cap) out[o++] = '-';
            for (size_t i = 0; i < len && o + 1 < cap; i++) out[o++] = hex[i];
        }
        hex = *end ? end + 1 : end;
    }
    out[o] = '\0';
}

// Embedded emoji PNG by Twemoji file name (the table is sorted).
static const emoji_png_t *emoji_png_find(const char *file)
{
    int lo = 0, hi = EMOJI_PNG_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(file, EMOJI_PNGS[mid].file);
        if (c == 0) return &EMOJI_PNGS[mid];
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return NULL;
}

// key is "c:<id>" (server emoji) or "u:<hex>" (unicode emoji).
static const gui_image_t *emoji_img(const char *key, int size)
{
    char ikey[80], url[300], file[48];
    const char *base = shell_getenv("DISCORD_EMOJI_CDN");
    snprintf(ikey, sizeof(ikey), "e:%s:%d", key, size);
    if (key[0] == 'c') snprintf(url, sizeof(url), "%s/emojis/%s.png?size=48", cdn_base(), key + 2);
    else {
        twemoji_file(key + 2, file, sizeof(file));
        const emoji_png_t *e = emoji_png_find(file);
        if (e) {   // shipped inside the kernel: no download
            img_t *i = img_find(ikey);
            if (!i) { img_decode(ikey, EMOJI_PNG_DATA + e->off, e->len, size, size); i = img_find(ikey); }
            return i && i->state == 1 ? &i->img : NULL;
        }
        twemoji_file(key + 2, file, sizeof(file));
        snprintf(url, sizeof(url), "%s/%s.png", (base && base[0]) ? base : EMOJI_CDN, file);
    }
    return img_get(ikey, url, size, size);
}


// ---- login (text mode) ---------------------------------------------------------------------------------------------

static void centered_box(int w, int h, const char *title, int *x, int *y)
{
    *x = (fs_cols() - w) / 2;
    *y = (fs_rows() - h) / 2;
    fs_fill(*x, *y, w, h, ' ', A_BASE);
    fs_box(*x, *y, w, h, A_BOX, title);
}

// A modal line prompt. secret shows only stars. Ctrl+V pastes the host clipboard. False if cancelled with Esc.
// With kind set, F1 / F2 choose between a user token (0) and a bot token (1).
static bool prompt_line(const char *title, const char *line1, const char *line2, const char *note, char *out,
                        size_t cap, bool secret, int *kind)
{
    size_t len = 0;
    out[0] = '\0';
    bool dirty = true;
    for (;;) {
        if (dirty) {
            int bx, by, w = 90, h = 11;
            if (w > fs_cols() - 2) w = fs_cols() - 2;
            fs_clear(A_BASE);
            centered_box(w, h, title, &bx, &by);
            fs_text(bx + 3, by + 2, line1, A_BASE);
            if (line2) fs_text(bx + 3, by + 3, line2, A_DIM);
            fs_fill(bx + 3, by + 5, w - 6, 1, ' ', fs_attr(WH, DG));
            int shown = 0;
            for (size_t i = (len > (size_t)(w - 8) ? len - (size_t)(w - 8) : 0); i < len; i++, shown++)
                fs_put(bx + 4 + shown, by + 5, secret ? '*' : out[i], fs_attr(WH, DG));
            fs_put(bx + 4 + shown, by + 5, '_', fs_attr(YL, DG));
            if (kind) {
                fs_text(bx + 3, by + 7, " F1  User token ", *kind == 0 ? fs_attr(K, LC) : A_DIM);
                fs_text(bx + 21, by + 7, " F2  Bot token ", *kind == 1 ? fs_attr(K, LC) : A_DIM);
            }
            if (note) fs_text(bx + 3, kind ? by + 8 : by + 7, note, A_ERR);
            fs_text(bx + 3, by + 9, "Enter accept   Ctrl+V paste from the host clipboard   Esc cancel", A_DIM);
            dirty = false;
            note = NULL;
        }
        fs_event_t ev;
        if (!fs_get_event(&ev)) { __asm__ volatile("pause"); continue; }
        dirty = true;
        if (ev.sc == 0x01) return false;
        if (ev.sc == 0x1C) return true;
        if (kind && ev.sc == 0x3B) { *kind = 0; continue; }
        if (kind && ev.sc == 0x3C) { *kind = 1; continue; }
        if (ev.sc == 0x0E) { if (len) out[--len] = '\0'; continue; }
        if (ev.ctrl && ev.sc == 0x2F) {
            static char host[CLIPBOARD_HOST_MAX + 1];
            int n = clipboard_from_host(host, sizeof(host));
            for (int i = 0; i < n && len + 1 < cap; i++)
                if ((unsigned char)host[i] > 0x20 && (unsigned char)host[i] < 0x7F) out[len++] = host[i];
            out[len] = '\0';
            memset(host, 0, sizeof(host));
            continue;
        }
        if (ev.ch >= 0x20 && ev.ch < 0x7F && len + 1 < cap) { out[len++] = ev.ch; out[len] = '\0'; }
    }
}

static bool message_box(const char *title, const char *l1, const char *l2, const char *l3, const char *keys)
{
    int bx, by, w = 96, h = 11;
    if (w > fs_cols() - 2) w = fs_cols() - 2;
    fs_clear(A_BASE);
    centered_box(w, h, title, &bx, &by);
    fs_text(bx + 3, by + 2, l1, A_BASE);
    if (l2) fs_text(bx + 3, by + 3, l2, A_BASE);
    if (l3) fs_text(bx + 3, by + 4, l3, A_BASE);
    fs_text(bx + 3, by + 8, keys, A_DIM);
    for (;;) {
        fs_event_t ev;
        if (!fs_get_event(&ev)) { __asm__ volatile("pause"); continue; }
        if (ev.sc == 0x01 || ev.ch == 'n' || ev.ch == 'N') return false;
        if (ev.sc == 0x1C || ev.ch == 'y' || ev.ch == 'Y') return true;
    }
}

// ---- login ---------------------------------------------------------------------------------------------------------

static void trim_token(char *t)
{
    size_t n = strlen(t);
    size_t start = 0;
    while (start < n && (t[start] == '"' || t[start] == '\'')) start++;
    while (n > start && (t[n - 1] == '"' || t[n - 1] == '\'')) n--;
    memmove(t, t + start, n - start);
    t[n - start] = '\0';
}

static void busy(const char *text)
{
    int bx, by;
    centered_box(60, 5, "Discord", &bx, &by);
    fs_text(bx + 3, by + 2, text, A_BASE);
    fs_present();
}

static bool vault_exists(void) { return avfs_file_exists(TOKEN_FILE); }

// Unlocks the saved token, or walks through first-time setup. True when g_token holds a working token.
static bool dc_login(void)
{
    char pass[130], pass2[130], note[100];
    int status = 0;
    note[0] = '\0';

    if (vault_exists()) {
        for (int attempt = 0; attempt < 3; attempt++) {
            if (!prompt_line("Discord", "Passphrase for the saved token:", NULL, note[0] ? note : NULL, pass, sizeof(pass), true, NULL)) return false;
            busy("Unlocking the token ...");
            int n = rust_prp_vault_load((const uint8_t *)pass, (const uint8_t *)TOKEN_FILE, (uint8_t *)g_token, sizeof(g_token) - 1);
            memset(pass, 0, sizeof(pass));
            if (n > 0) {
                g_token[n] = '\0';
                busy("Checking the token with Discord ...");
                if (dc_load_me(&status)) return true;
                char why[120];
                explain_failure(why, sizeof(why));
                message_box("Discord", why, "Run `discord logout` and log in again with a fresh token.", NULL, "Esc close");
                memset(g_token, 0, sizeof(g_token));
                return false;
            }
            strcpy(note, n == -2 ? "Wrong passphrase." : "The saved token file is damaged (discord logout removes it).");
            if (n != -2) break;
        }
        message_box("Discord", "Could not unlock the token.", NULL, NULL, "Esc close");
        return false;
    }

    if (!message_box("Discord - before you start",
                     "Using a USER token from a client other than Discord's own is against Discord's Terms",
                     "of Service and can get the account limited or banned (bot tokens are fine).",
                     "On the next screen F1 picks a user token, F2 a bot token. Continue?", "Y continue   N / Esc cancel"))
        return false;

    int kind = 0;
    for (;;) {
        if (!prompt_line("Discord - token", "Paste your Discord token (it is not shown).",
                         "Copy it on the host, then press Ctrl+V here.", note[0] ? note : NULL, g_token, sizeof(g_token), true, &kind))
            { memset(g_token, 0, sizeof(g_token)); return false; }
        trim_token(g_token);
        if (kind == 1 && !is_bot() && strlen(g_token) + 5 < sizeof(g_token)) {
            memmove(g_token + 4, g_token, strlen(g_token) + 1);
            memcpy(g_token, "Bot ", 4);
        }
        busy("Checking the token with Discord ...");
        if (dc_load_me(&status)) break;
        explain_failure(note, sizeof(note));
    }

    for (;;) {
        if (!prompt_line("Discord - passphrase", "Choose a passphrase to encrypt the token on the disk.",
                         "You enter it every time you open the client. 4 characters or more.", note[0] ? note : NULL, pass, sizeof(pass), true, NULL)) {
            message_box("Discord", "The token was not saved; it is only used for this session.", NULL, NULL, "Enter continue");
            return true;
        }
        if (strlen(pass) < 4) { strcpy(note, "Too short."); continue; }
        if (!prompt_line("Discord - passphrase", "Type the passphrase again.", NULL, NULL, pass2, sizeof(pass2), true, NULL)) { memset(pass, 0, sizeof(pass)); strcpy(note, "Cancelled."); continue; }
        if (strcmp(pass, pass2) != 0) { strcpy(note, "They did not match."); memset(pass2, 0, sizeof(pass2)); continue; }
        break;
    }
    memset(pass2, 0, sizeof(pass2));

    busy("Encrypting and saving the token ...");
    int rc = rust_prp_vault_save((const uint8_t *)pass, (const uint8_t *)g_token, (uint32_t)strlen(g_token), (const uint8_t *)TOKEN_FILE);
    memset(pass, 0, sizeof(pass));
    if (rc != 0) message_box("Discord", "Could not save the token (is /disk mounted?). It is only used for this session.", NULL, NULL, "Enter continue");
    return true;
}

// ---- the client ----------------------------------------------------------------------------------------------------

#define C_RAIL    0x1E1F22
#define C_LIST    0x2B2D31
#define C_PANEL   0x232428
#define C_CHAT    0x313338
#define C_INPUT   0x383A40
#define C_TEXT    0xDBDEE1
#define C_WHITE   0xFFFFFF
#define C_MUTED   0xA3A9B2
#define C_BLURPLE 0x5865F2
#define C_HOVER   0x35373C
#define C_SEL     0x404249
#define C_DIV     0x3F4147
#define C_GREEN   0x23A559
#define C_RED     0xF23F42
#define C_LINK    0x00A8FC
#define C_BORDER  0x26272B

// Text is drawn at 16 px (the 8x8 font doubled, edge-smoothed) or 24 px; nothing smaller is used.
#define RAIL_W    72
#define LIST_X    72
#define LIST_W    300
#define CHAT_X    372
#define CHAT_W    (GUI_W - CHAT_X)
#define HEAD_H    48
#define INPUT_H   48
#define INPUT_Y   (GUI_H - 16 - INPUT_H)
#define MSG_TOP   HEAD_H
#define MSG_BOT   (INPUT_Y - 8)
#define RAIL_Y0   80
#define RAIL_STEP 56
#define RAIL_ROWS 10
#define LIST_ROW  32
#define DM_ROW    44
#define PANEL_H   56
#define LINE_H    24
#define EMO_PX    22

enum { F_RAIL, F_LIST, F_INPUT };
enum { MOD_NONE, MOD_JOIN, MOD_CONFIRM };

static int  g_focus = F_RAIL;
static int  g_rail_cur, g_rail_top;      // rail cursor: 0 = Home, k = server k-1, n_guild + 1 = add server
static int  g_guild_open = -1;           // the open server, -1 = Home
static int  g_list_cur, g_list_top;
static bool g_open, g_open_dm;
static char g_open_id[24], g_open_name[48], g_open_topic[100];
static int  g_scroll;
static char g_input[1900];
static size_t g_input_len;
static char g_status[120];
static uint32_t g_status_rgb;
static uint64_t g_status_until;
static bool g_help, g_switch;
static bool g_dirty = true;

// notification card
static char g_notif_title[48], g_notif_text[160];
static uint64_t g_notif_until;
static uint32_t g_notif_rgb;

// settings panel
static bool g_settings, g_set_confirm;
static int  g_set_cur;

// HELIOS plugins found in PLUG_DIR
#define PLUG_DIR "/disk/dcplug"
#define PLUG_OUT "/disk/dcplug/out.txt"
#define MAX_PLUG 12
typedef struct { char name[12]; char path[48]; } plug_t;
static plug_t g_plug[MAX_PLUG];
static int n_plug;
static uint64_t g_plug_next;

static int  g_modal;                     // join-server dialog
static char g_mod_in[120];
static size_t g_mod_len;
static char g_inv_code[40], g_inv_gid[24], g_inv_name[64];
static int  g_inv_members, g_inv_online;

// emoji suggestions while typing :na
#define SUG_MAX 8
typedef struct { bool custom; int idx; } sug_t;
static sug_t g_sug[SUG_MAX];
static int  n_sug, g_sug_cur, g_sug_from;
static char g_sug_q[34], g_sug_last[34];
static bool g_sug_off;

static void toast(uint32_t rgb, const char *text)
{
    copy_str(g_status, sizeof(g_status), text);
    g_status_rgb = rgb;
    g_status_until = tsc_deadline_ms(6000);
    g_dirty = true;
}

// Text that fits max_px at the given glyph size, shortened with ".." if it does not.
static void fit_text(const char *s, int max_px, int size, char *out, size_t cap)
{
    int chars = max_px / size;
    size_t len = strlen(s);
    if ((int)len <= chars) { copy_str(out, cap, s); return; }
    if (chars < 3) { out[0] = '\0'; return; }
    size_t keep = (size_t)chars - 2;
    if (keep + 3 > cap) keep = cap - 3;
    memcpy(out, s, keep);
    strcpy(out + keep, "..");
}

static int text_center(int cx, int y, const char *s, uint32_t rgb, int size, bool bold)
{
    return gui_text(cx - gui_text_width(s, size) / 2, y, s, rgb, size, bold);
}

static uint32_t color_of(const char *s)
{
    static const uint32_t PAL[6] = { 0x5865F2, 0x747F8D, 0x3BA55D, 0xFAA61A, 0xED4245, 0xEB459E };
    unsigned sum = 0;
    for (; *s; s++) sum += (unsigned char)*s;
    return PAL[sum % 6];
}

static void initials(const char *name, char *out)
{
    int n = 0;
    bool word_start = true;
    for (const char *p = name; *p && n < 2; p++) {
        if (*p == ' ' || *p == '-' || *p == '_') { word_start = true; continue; }
        if (word_start) { out[n++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p; word_start = false; }
    }
    if (n == 1 && name[1]) { out[n++] = name[1]; }
    out[n] = '\0';
}

static void compact(int n, char *out, size_t cap)
{
    if (n < 10000) commas(n, out, cap);
    else if (n < 1000000) snprintf(out, cap, "%d.%dk", n / 1000, (n % 1000) / 100);
    else snprintf(out, cap, "%d.%dM", n / 1000000, (n % 1000000) / 100000);
}

// A round or rounded image, or a coloured placeholder with initials while it downloads.
static void draw_avatar(const gui_image_t *img, const char *name, uint32_t base, int x, int y, int size, int radius)
{
    if (img) { gui_image(img, x, y, size, size, radius); return; }
    char ini[4];
    gui_round(x, y, size, size, radius, base);
    initials(name, ini);
    if (size < 40) ini[1] = '\0';
    text_center(x + size / 2, y + (size - 16) / 2, ini, C_WHITE, 16, true);
}

// A wrapped line of message text; EMO_MARK pairs are drawn as emoji images (two columns wide).
static void draw_rich(int x, int y, const char *s, const msg_t *m, uint32_t rgb)
{
    char run[112];
    int rn = 0;
    for (;; s++) {
        if (*s != '\0' && *s != EMO_MARK) {
            if (rn + 1 < (int)sizeof(run)) run[rn++] = *s;
            continue;
        }
        run[rn] = '\0';
        if (rn) x = gui_text(x, y, run, rgb, 16, false);
        rn = 0;
        if (!*s) break;
        int idx = s[1] - 0x10;
        if (s[1]) s++;
        if (idx >= 0 && idx < m->n_emo) {
            const gui_image_t *img = emoji_img(m->emo[idx], EMO_PX);
            if (img) gui_image(img, x + 5, y - 3, EMO_PX, EMO_PX, 0);
        }
        x += 32;
    }
}

// ---- message layout --------------------------------------------------------------------------------------------------

typedef struct { char t[104]; } wl_t;
typedef struct { int y, h; bool divider, head; int l0, ln, el0, eln; int aw[2], ah[2]; } blk_t;

#define WL_MAX        4000
#define TOP_BANNER_H  36
static bool g_hist_loading;

static wl_t g_wl[WL_MAX];
static int n_wl;
static blk_t g_blk[MAX_MSGS];
static int g_total_h, g_cols;

static int wrap(const char *s, int cols)
{
    int count = 0;
    const char *p = s;
    if (cols < 8) cols = 8;
    do {
        int take = 0;
        while (p[take] && p[take] != '\n' && take < cols) take++;
        int cut = take;
        if (p[take] && p[take] != '\n') {
            int k = take;
            while (k > cols / 2 && p[k] != ' ') k--;
            if (k > cols / 2) cut = k;
        }
        if (cut > 0 && p[cut - 1] == EMO_MARK) cut--;   // never split an emoji marker from its slot byte
        if (n_wl >= WL_MAX) return count;
        int n = cut < 103 ? cut : 103;
        memcpy(g_wl[n_wl].t, p, (size_t)n);
        g_wl[n_wl].t[n] = '\0';
        n_wl++;
        count++;
        p += cut;
        while (*p == ' ') p++;
        if (*p == '\n') p++;
    } while (*p);
    return count;
}

static void fit_box(int w, int h, int mw, int mh, int *ow, int *oh)
{
    if (w <= 0 || h <= 0) { *ow = mw; *oh = mh * 2 / 3; return; }
    if (w <= mw && h <= mh) { *ow = w; *oh = h; return; }
    if (w * mh > h * mw) { *ow = mw; *oh = h * mw / w; } else { *oh = mh; *ow = w * mh / h; }
    if (*ow < 1) *ow = 1;
    if (*oh < 1) *oh = 1;
}

static int minute_of(const char *when) { return ((when[0] - '0') * 10 + (when[1] - '0')) * 60 + (when[3] - '0') * 10 + (when[4] - '0'); }

static void relayout(void)
{
    g_cols = (CHAT_W - 72 - 24) / 16;
    n_wl = 0;
    int y = 8 + TOP_BANNER_H;
    const char *pd = "", *pu = "";
    int pmin = -1000;
    for (int i = 0; i < n_msg; i++) {
        const msg_t *m = &g_msg[i];
        blk_t *b = &g_blk[i];
        memset(b, 0, sizeof(*b));
        b->divider = m->date[0] && strcmp(m->date, pd) != 0;
        int minute = minute_of(m->when);
        b->head = b->divider || strcmp(m->uid, pu) != 0 || m->reply || minute - pmin > 7 || minute < pmin;
        b->y = y;
        int h = b->divider ? 44 : (b->head ? 16 : 4);
        if (m->reply) h += 24;
        if (b->head) h += LINE_H;
        if (m->text[0]) { b->l0 = n_wl; b->ln = wrap(m->text, g_cols); h += b->ln * LINE_H; }
        for (int a = 0; a < m->n_att; a++) {
            if (m->att[a].image && g_set.images) { fit_box(m->att[a].w, m->att[a].h, 400, 300, &b->aw[a], &b->ah[a]); h += 6 + b->ah[a]; }
            else h += 36;
        }
        if (m->has_embed) {
            b->el0 = n_wl;
            if (m->etitle[0]) b->eln += wrap(m->etitle, g_cols - 3);
            if (m->edesc[0]) b->eln += wrap(m->edesc, g_cols - 3);
            h += 14 + b->eln * LINE_H;
        }
        b->h = h;
        y += h;
        pd = m->date;
        pu = m->uid;
        pmin = minute;
    }
    g_total_h = y + 8;
    int view = MSG_BOT - MSG_TOP;
    int max_scroll = g_total_h > view ? g_total_h - view : 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;
    g_hist_want = g_open && can_load_older() && g_scroll >= max_scroll - 40;
}

static void draw_block(int i, int y)
{
    const msg_t *m = &g_msg[i];
    const blk_t *b = &g_blk[i];
    int cx = CHAT_X + 72;
    if (b->divider) {
        char d[24];
        pretty_date(m->date, d, sizeof(d));
        gui_fill(CHAT_X + 16, y + 22, CHAT_W - 32, 1, C_DIV);
        int w = gui_text_width(d, 16) + 24;
        gui_fill(CHAT_X + CHAT_W / 2 - w / 2, y + 12, w, 20, C_CHAT);
        text_center(CHAT_X + CHAT_W / 2, y + 14, d, C_MUTED, 16, true);
        y += 44;
    } else {
        y += b->head ? 16 : 4;
    }
    if (m->reply) {
        char line[100], clip[100];
        snprintf(line, sizeof(line), "@%s  %s", m->reply_who, m->reply_text);
        fit_text(line, CHAT_W - 110, 16, clip, sizeof(clip));
        gui_fill(cx - 24, y + 9, 14, 2, C_DIV);
        gui_text(cx, y + 2, clip, C_MUTED, 16, false);
        y += 24;
    }
    if (b->head) {
        draw_avatar(avatar_img(m->uid, m->avatar, 40), m->who, color_of(m->uid), CHAT_X + 16, y, 40, 20);
        int nx = gui_text(cx, y, m->who, C_WHITE, 16, true);
        gui_text(nx + 12, y, m->when, C_MUTED, 16, false);
        y += LINE_H;
    }
    for (int l = 0; l < b->ln; l++, y += LINE_H) draw_rich(cx, y, g_wl[b->l0 + l].t, m, C_TEXT);
    for (int a = 0; a < m->n_att; a++) {
        const att_t *t = &m->att[a];
        if (t->image && g_set.images) {
            const gui_image_t *img = attachment_img(t, b->aw[a], b->ah[a]);
            y += 6;
            if (img) gui_image(img, cx, y, b->aw[a], b->ah[a], 8);
            else {
                gui_round(cx, y, b->aw[a], b->ah[a], 8, C_LIST);
                text_center(cx + b->aw[a] / 2, y + b->ah[a] / 2 - 8, "loading image ...", C_MUTED, 16, false);
            }
            y += b->ah[a];
        } else {
            char nm[60];
            fit_text(t->name, 380, 16, nm, sizeof(nm));
            gui_round(cx, y + 2, 400, 30, 4, C_LIST);
            gui_text(cx + 10, y + 9, nm, C_LINK, 16, false);
            y += 36;
        }
    }
    if (m->has_embed) {
        int eh = b->eln * LINE_H + 8;
        gui_fill(cx, y + 4, 4, eh, C_BLURPLE);
        gui_round(cx + 4, y + 4, CHAT_W - 72 - 40, eh, 4, C_LIST);
        for (int l = 0; l < b->eln; l++) {
            bool title = m->etitle[0] && l == 0;
            gui_text(cx + 16, y + 8 + l * LINE_H, g_wl[b->el0 + l].t, title ? C_LINK : C_TEXT, 16, title);
        }
        y += 14 + eh - 8;
    }
}

static void draw_messages(void)
{
    gui_clip(CHAT_X, MSG_TOP, CHAT_W, MSG_BOT - MSG_TOP);
    int base = MSG_BOT - g_total_h + g_scroll;
    {
        char note[96], clip[96];
        if (g_hist_end) snprintf(note, sizeof(note), "This is the start of %s%s", g_open_dm ? "your chat with " : "#", g_open_name);
        else if (g_hist_loading) snprintf(note, sizeof(note), "Loading older messages ...");
        else if (can_load_older()) snprintf(note, sizeof(note), "Scroll up for older messages");
        else snprintf(note, sizeof(note), "Showing the latest %d messages", n_msg);
        fit_text(note, CHAT_W - 40, 16, clip, sizeof(clip));
        text_center(CHAT_X + CHAT_W / 2, base + 12, clip, C_MUTED, 16, false);
    }
    for (int i = 0; i < n_msg; i++) {
        int top = base + g_blk[i].y;
        if (top > MSG_BOT || top + g_blk[i].h < MSG_TOP) continue;
        draw_block(i, top);
    }
    gui_unclip();
}

// ---- panels ------------------------------------------------------------------------------------------------------

static bool list_cursor_ok(int idx)
{
    if (g_guild_open < 0) return idx >= 0 && idx < n_dm;
    return idx >= 0 && idx < n_lrow && g_lrow[idx].kind == ROW_TEXT;
}

static int list_count(void) { return g_guild_open < 0 ? n_dm : n_lrow; }

static void draw_rail(void)
{
    gui_fill(0, 0, RAIL_W, GUI_H, C_RAIL);

    bool home_open = g_guild_open < 0, home_cur = g_focus == F_RAIL && g_rail_cur == 0;
    if (home_open) gui_round(0, 20, 4, 32, 2, C_WHITE);
    else if (home_cur) gui_round(0, 26, 4, 20, 2, C_WHITE);
    gui_round(12, 12, 48, 48, (home_open || home_cur) ? 16 : 24, (home_open || home_cur) ? C_BLURPLE : C_CHAT);
    text_center(36, 28, "DM", C_WHITE, 16, true);
    {
        int unread = 0;
        for (int i = 0; i < n_dm; i++) unread += g_dm[i].unread;
        if (unread > 0) {
            char b[8];
            snprintf(b, sizeof(b), "%d", unread > 9 ? 9 : unread);
            gui_circle(52, 18, 11, C_RAIL);
            gui_circle(52, 18, 9, C_RED);
            text_center(52, 10, unread > 9 ? "9+" : b, C_WHITE, 16, true);
        }
    }
    gui_fill(20, 68, 32, 2, 0x35363C);

    for (int k = 0; k < RAIL_ROWS; k++) {
        int idx = g_rail_top + k;
        if (idx > n_guild) break;
        int y = RAIL_Y0 + k * RAIL_STEP;
        bool cur = g_focus == F_RAIL && g_rail_cur == idx + 1;
        if (idx == n_guild) {   // the add-server tile
            gui_round(12, y, 48, 48, cur ? 16 : 24, cur ? C_GREEN : C_CHAT);
            text_center(36, y + 12, "+", cur ? C_WHITE : C_GREEN, 24, true);
            if (cur) gui_round(0, y + 14, 4, 20, 2, C_WHITE);
            continue;
        }
        bool open = idx == g_guild_open;
        draw_avatar(guild_icon_img(&g_guild[idx]), g_guild[idx].name, open ? C_BLURPLE : C_CHAT, 12, y, 48, (open || cur) ? 16 : 24);
        if (open) gui_round(0, y + 4, 4, 40, 2, C_WHITE);
        else if (cur) gui_round(0, y + 14, 4, 20, 2, C_WHITE);
    }
    if (g_rail_top > 0) text_center(36, 71, "^", C_MUTED, 16, true);
    if (g_rail_top + RAIL_ROWS <= n_guild) text_center(36, RAIL_Y0 + RAIL_ROWS * RAIL_STEP - 8, "v", C_MUTED, 16, true);

    // server counter
    char n[16];
    gui_round(8, GUI_H - 56, 56, 48, 10, C_LIST);
    commas(n_guild, n, sizeof(n));
    text_center(36, GUI_H - 50, n, C_WHITE, 16, true);
    text_center(36, GUI_H - 30, "srv", C_MUTED, 16, false);
}

static void draw_list(void)
{
    gui_fill(LIST_X, 0, LIST_W, GUI_H, C_LIST);
    gui_fill(LIST_X, HEAD_H - 1, LIST_W, 1, C_BORDER);
    int x = LIST_X;
    char clip[64];

    if (g_guild_open < 0) {
        gui_round(x + 12, 8, LIST_W - 24, 32, 6, C_RAIL);
        gui_text(x + 22, 16, "Ctrl+K  Find", C_MUTED, 16, false);
    } else {
        const guild_t *g = &g_guild[g_guild_open];
        fit_text(g->name, LIST_W - 32, 16, clip, sizeof(clip));
        gui_text(x + 16, 4, clip, C_WHITE, 16, true);
        char m[16], line[48];
        compact(g->members, m, sizeof(m));
        if (g->members > 0) { snprintf(line, sizeof(line), "%s members", m); gui_text(x + 16, 25, line, C_MUTED, 16, false); }
    }

    int top = HEAD_H;
    gui_clip(LIST_X, top, LIST_W, GUI_H - PANEL_H - top);
    if (g_guild_open < 0) {
        char label[40];
        snprintf(label, sizeof(label), "DIRECT MESSAGES %d", n_dm);
        gui_text(x + 16, top + 10, label, C_MUTED, 16, true);
        for (int k = 0; ; k++) {
            int idx = g_list_top + k, y = top + 36 + k * DM_ROW;
            if (idx >= n_dm || y > GUI_H) break;
            bool open = g_open && g_open_dm && strcmp(g_dm[idx].id, g_open_id) == 0;
            bool cur = g_focus == F_LIST && idx == g_list_cur;
            if (open || cur) gui_round(x + 8, y, LIST_W - 16, 40, 6, open ? C_SEL : C_HOVER);
            draw_avatar(avatar_img(g_dm[idx].uid, g_dm[idx].avatar, 32), g_dm[idx].name, color_of(g_dm[idx].uid), x + 14, y + 4, 32, 16);
            fit_text(g_dm[idx].name, LIST_W - 70, 16, clip, sizeof(clip));
            gui_text(x + 56, y + 12, clip, (open || cur) ? C_WHITE : C_MUTED, 16, open);
            if (g_dm[idx].unread > 0) {
                char b[8];
                snprintf(b, sizeof(b), "%d", g_dm[idx].unread);
                int bw = (int)strlen(b) * 16 + 14;
                gui_round(x + LIST_W - 16 - bw, y + 10, bw, 22, 11, C_RED);
                gui_text(x + LIST_W - 16 - bw + 7, y + 13, b, C_WHITE, 16, true);
            }
        }
    } else {
        for (int k = 0, y = top + 8; ; k++) {
            int idx = g_list_top + k;
            if (idx >= n_lrow || y > GUI_H) break;
            const lrow_t *r = &g_lrow[idx];
            if (r->kind == ROW_CAT) {
                char up[56];
                snprintf(up, sizeof(up), "v %s", r->name);
                for (char *c = up; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
                fit_text(up, LIST_W - 24, 16, clip, sizeof(clip));
                gui_text(x + 12, y + 12, clip, C_MUTED, 16, true);
                y += 36;
                continue;
            }
            bool open = g_open && !g_open_dm && strcmp(r->id, g_open_id) == 0;
            bool cur = g_focus == F_LIST && idx == g_list_cur;
            if (open || cur) gui_round(x + 8, y, LIST_W - 16, LIST_ROW, 6, open ? C_SEL : C_HOVER);
            gui_text(x + 18, y + 8, r->kind == ROW_VOICE ? ")" : "#", C_MUTED, 16, true);
            fit_text(r->name, LIST_W - 64, 16, clip, sizeof(clip));
            gui_text(x + 42, y + 8, clip, (open || cur) ? C_WHITE : (r->kind == ROW_VOICE ? 0x7D808A : C_MUTED), 16, open);
            y += LIST_ROW + 2;
        }
    }
    gui_unclip();

    // you
    gui_fill(LIST_X, GUI_H - PANEL_H, LIST_W, PANEL_H, C_PANEL);
    draw_avatar(avatar_img(g_me_id, g_me_avatar, 40), g_me, color_of(g_me_id), x + 8, GUI_H - PANEL_H + 8, 40, 20);
    gui_circle(x + 8 + 34, GUI_H - PANEL_H + 8 + 34, 7, C_PANEL);
    gui_circle(x + 8 + 34, GUI_H - PANEL_H + 8 + 34, 5, C_GREEN);
    fit_text(g_me, LIST_W - 70, 16, clip, sizeof(clip));
    gui_text(x + 58, GUI_H - PANEL_H + 8, clip, C_WHITE, 16, true);
    gui_text(x + 58, GUI_H - PANEL_H + 30, is_bot() ? "Bot - F3 Prefs" : "F3 Settings", C_MUTED, 16, false);
}

static void chip(int *right, const char *key, const char *label)
{
    int w = (int)(strlen(key) + strlen(label) + 1) * 16 + 16;
    *right -= w;
    gui_round(*right, 8, w, 32, 6, C_RAIL);
    int x = gui_text(*right + 8, 16, key, C_BLURPLE, 16, true);
    gui_text(x + 16, 16, label, C_MUTED, 16, false);
    *right -= 8;
}

static void draw_header(void)
{
    gui_fill(CHAT_X, 0, CHAT_W, HEAD_H, C_CHAT);
    gui_fill(CHAT_X, HEAD_H - 1, CHAT_W, 1, C_BORDER);
    int right = GUI_W - 16;
    chip(&right, "F1", "Help");
    chip(&right, "Ctrl+K", "Find");
    chip(&right, "Ctrl+J", "Join");

    if (!g_open) { gui_text(CHAT_X + 16, 16, "Discord", C_WHITE, 16, true); return; }
    char clip[100];
    gui_text(CHAT_X + 16, 16, g_open_dm ? "@" : "#", C_MUTED, 16, true);
    fit_text(g_open_name, right - (CHAT_X + 40) - 8, 16, clip, sizeof(clip));
    int x = gui_text(CHAT_X + 40, 16, clip, C_WHITE, 16, true);
    if (g_open_topic[0] && right - x - 40 >= 160) {
        gui_fill(x + 12, 12, 1, 24, C_DIV);
        fit_text(g_open_topic, right - x - 40, 16, clip, sizeof(clip));
        gui_text(x + 24, 16, clip, C_MUTED, 16, false);
    }
}

static void draw_input(void)
{
    int x = CHAT_X + 16, w = CHAT_W - 32;
    gui_round(x, INPUT_Y, w, INPUT_H, 8, C_INPUT);
    gui_circle(x + 26, INPUT_Y + INPUT_H / 2, 11, C_MUTED);
    text_center(x + 26, INPUT_Y + INPUT_H / 2 - 8, "+", C_INPUT, 16, true);
    if (!g_open) return;
    int room = (w - 64) / 16;
    int ty = INPUT_Y + (INPUT_H - 16) / 2;
    if (g_input_len == 0) {
        char ph[80], clip[80];
        snprintf(ph, sizeof(ph), "Message %s%s", g_open_dm ? "@" : "#", g_open_name);
        fit_text(ph, w - 64, 16, clip, sizeof(clip));
        gui_text(x + 52, ty, clip, C_MUTED, 16, false);
    } else {
        char shown[130];
        size_t from = g_input_len > (size_t)room ? g_input_len - (size_t)room : 0;
        size_t n = 0;
        for (size_t i = from; i < g_input_len && n + 1 < sizeof(shown); i++) shown[n++] = g_input[i] == '\n' ? ' ' : g_input[i];
        shown[n] = '\0';
        int ex = gui_text(x + 52, ty, shown, C_TEXT, 16, false);
        if (g_focus == F_INPUT) gui_fill(ex + 1, ty - 2, 2, 20, C_WHITE);
    }
    if (g_focus == F_INPUT && g_input_len == 0) gui_fill(x + 51, ty - 2, 2, 20, C_WHITE);
}

static void draw_empty(void)
{
    int cx = CHAT_X + CHAT_W / 2;
    char line[80], n[16];
    commas(n_guild, n, sizeof(n));
    text_center(cx, 200, "Discord for RadiumOS", C_WHITE, 24, true);
    snprintf(line, sizeof(line), "You are in %s server%s", n, n_guild == 1 ? "" : "s");
    text_center(cx, 252, line, C_TEXT, 16, false);
    text_center(cx, 300, "Pick a server, then a channel,", C_MUTED, 16, false);
    text_center(cx, 324, "or open Home for your DMs.", C_MUTED, 16, false);
    text_center(cx, 372, "Ctrl+J joins a server by invite.", C_MUTED, 16, false);
    text_center(cx, 396, "F1 lists every key.", C_MUTED, 16, false);
}

static const char *const HELP_LINES[][2] = {
    { "Tab / Shift+Tab", "Move between servers, channels, message box" },
    { "Up / Down", "Choose something, or scroll the messages" },
    { "Enter", "Open it, or send the message" },
    { "Alt+Up / Down", "Previous / next channel or DM" },
    { "Ctrl+Alt+Up / Down", "Previous / next server" },
    { "Ctrl+K", "Quick switcher: type to search" },
    { "Ctrl+J", "Join a server with an invite link" },
    { "F2", "Home (direct messages)" },
    { "F3", "User settings: alerts, sound, plugins" },
    { "PgUp / PgDn", "Scroll; Home / End = oldest / newest" },
    { "F5", "Refresh messages, servers and DMs" },
    { "F6", "Reload every image" },
    { "Ctrl+V", "Paste into the message box" },
    { ":name:", "Emoji. Type :sm, Up/Down, Tab or Enter picks" },
    { "/name", "Run a HELIOS plugin; /plugins lists them" },
    { "Esc", "Back out; from the server list it quits" },
};
#define HELP_N ((int)(sizeof(HELP_LINES) / sizeof(HELP_LINES[0])))

static void draw_help(void)
{
    int w = 1180, h = 110 + HELP_N * 30, x = (GUI_W - w) / 2, y = (GUI_H - h) / 2;
    gui_round(x - 2, y - 2, w + 4, h + 4, 10, C_BORDER);
    gui_round(x, y, w, h, 8, C_LIST);
    gui_text(x + 24, y + 16, "Keyboard shortcuts", C_WHITE, 24, true);
    for (int i = 0; i < HELP_N; i++) {
        int ly = y + 62 + i * 30;
        int kw = (int)strlen(HELP_LINES[i][0]) * 16 + 16;
        gui_round(x + 24, ly, kw, 26, 5, C_RAIL);
        gui_text(x + 32, ly + 5, HELP_LINES[i][0], C_BLURPLE, 16, true);
        gui_text(x + 24 + 340, ly + 5, HELP_LINES[i][1], C_TEXT, 16, false);
    }
    text_center(x + w / 2, y + h - 28, "Emoji art: Twemoji, CC-BY 4.0. Press any key to close", C_MUTED, 16, false);
}

// ---- quick switcher ----------------------------------------------------------------------------------------------

typedef struct { uint8_t kind; int idx; char label[64]; } sw_t;   // kind: 0 DM, 1 server, 2 channel of the open server
static sw_t g_sw[16];
static int n_sw, g_sw_cur;
static char g_sw_q[48];
static size_t g_sw_len;

static void sw_add(uint8_t kind, int idx, const char *prefix, const char *name)
{
    if (n_sw >= 16) return;
    g_sw[n_sw].kind = kind;
    g_sw[n_sw].idx = idx;
    snprintf(g_sw[n_sw].label, sizeof(g_sw[n_sw].label), "%s%s", prefix, name);
    n_sw++;
}

static void sw_rebuild(void)
{
    n_sw = 0;
    if (g_guild_open >= 0)
        for (int i = 0; i < n_lrow; i++) if (g_lrow[i].kind == ROW_TEXT && contains_nocase(g_lrow[i].name, g_sw_q)) sw_add(2, i, "#", g_lrow[i].name);
    for (int i = 0; i < n_dm; i++) if (contains_nocase(g_dm[i].name, g_sw_q)) sw_add(0, i, "@", g_dm[i].name);
    for (int i = 0; i < n_guild; i++) if (contains_nocase(g_guild[i].name, g_sw_q)) sw_add(1, i, "> ", g_guild[i].name);
    if (g_sw_cur >= n_sw) g_sw_cur = n_sw ? n_sw - 1 : 0;
}

static void draw_switcher(void)
{
    int w = 720, h = 80 + (n_sw ? n_sw : 1) * 34 + 16, x = (GUI_W - w) / 2, y = 60;
    gui_round(x - 2, y - 2, w + 4, h + 4, 10, C_BORDER);
    gui_round(x, y, w, h, 8, C_LIST);
    gui_round(x + 16, y + 16, w - 32, 44, 6, C_RAIL);
    if (!g_sw_len) gui_text(x + 28, y + 30, "Where would you like to go?", C_MUTED, 16, false);
    else {
        int ex = gui_text(x + 28, y + 30, g_sw_q, C_WHITE, 16, false);
        gui_fill(ex + 1, y + 28, 2, 20, C_WHITE);
    }
    if (n_sw == 0) gui_text(x + 28, y + 80, "Nothing matches.", C_MUTED, 16, false);
    for (int i = 0; i < n_sw; i++) {
        int ly = y + 72 + i * 34;
        if (i == g_sw_cur) gui_round(x + 12, ly, w - 24, 32, 6, C_SEL);
        char clip[64];
        fit_text(g_sw[i].label, w - 64, 16, clip, sizeof(clip));
        gui_text(x + 24, ly + 8, clip, i == g_sw_cur ? C_WHITE : C_TEXT, 16, false);
    }
}

// ---- emoji suggestions (type :na) ------------------------------------------------------------------------------

static bool name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-';
}

static bool starts_nocase(const char *hay, const char *needle)
{
    for (; *needle; hay++, needle++) {
        char a = *hay, b = *needle;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

static void sug_add(bool custom, int idx)
{
    if (n_sug < SUG_MAX) { g_sug[n_sug].custom = custom; g_sug[n_sug].idx = idx; n_sug++; }
}

// Finds the ":xx" being typed at the end of the message box and lists matching emoji.
static void sug_update(void)
{
    n_sug = 0;
    int e = (int)g_input_len, s = e;
    if (g_focus != F_INPUT || !g_open || g_help || g_switch || g_modal != MOD_NONE) { g_sug_last[0] = '\0'; return; }
    while (s > 0 && name_char(g_input[s - 1]) && e - s < 32) s--;
    int colon = s - 1;
    if (colon < 0 || g_input[colon] != ':' || e - s < 2 || (colon > 0 && g_input[colon - 1] != ' ' && g_input[colon - 1] != '\n')) {
        g_sug_last[0] = '\0';
        return;
    }
    memcpy(g_sug_q, g_input + s, (size_t)(e - s));
    g_sug_q[e - s] = '\0';
    if (strcmp(g_sug_q, g_sug_last) != 0) { copy_str(g_sug_last, sizeof(g_sug_last), g_sug_q); g_sug_cur = 0; g_sug_off = false; }
    if (g_sug_off) return;
    g_sug_from = colon;

    for (int i = 0; i < n_gemoji && n_sug < 4; i++) if (starts_nocase(g_gemoji[i].name, g_sug_q)) sug_add(true, i);
    for (int i = 0; i < EMOJI_N && n_sug < SUG_MAX; i++) if (starts_nocase(EMOJI_DEFS[i].name, g_sug_q)) sug_add(false, i);
    for (int i = 0; i < n_gemoji && n_sug < SUG_MAX; i++)
        if (!starts_nocase(g_gemoji[i].name, g_sug_q) && contains_nocase(g_gemoji[i].name, g_sug_q)) sug_add(true, i);
    for (int i = 0; i < EMOJI_N && n_sug < SUG_MAX; i++)
        if (!starts_nocase(EMOJI_DEFS[i].name, g_sug_q) && contains_nocase(EMOJI_DEFS[i].name, g_sug_q)) sug_add(false, i);
    if (g_sug_cur >= n_sug) g_sug_cur = 0;
}

static void sug_pick(void)
{
    if (n_sug == 0) return;
    const char *name = g_sug[g_sug_cur].custom ? g_gemoji[g_sug[g_sug_cur].idx].name : EMOJI_DEFS[g_sug[g_sug_cur].idx].name;
    if ((size_t)g_sug_from + strlen(name) + 4 >= sizeof(g_input)) return;
    g_input_len = (size_t)g_sug_from;
    g_input[g_input_len++] = ':';
    for (const char *c = name; *c; c++) g_input[g_input_len++] = *c;
    g_input[g_input_len++] = ':';
    g_input[g_input_len++] = ' ';
    g_input[g_input_len] = '\0';
    g_sug_last[0] = '\0';
}

static void draw_suggest(void)
{
    if (n_sug == 0) return;
    int w = 560, h = 44 + n_sug * 34 + 6, x = CHAT_X + 16, y = INPUT_Y - 8 - h;
    char head[64], clip[64];
    gui_round(x - 2, y - 2, w + 4, h + 4, 8, C_BORDER);
    gui_round(x, y, w, h, 6, C_LIST);
    snprintf(head, sizeof(head), "EMOJI MATCHING :%s", g_sug_q);
    fit_text(head, w - 24, 16, clip, sizeof(clip));
    gui_text(x + 12, y + 14, clip, C_MUTED, 16, true);
    for (int i = 0; i < n_sug; i++) {
        int ly = y + 44 + i * 34;
        char key[48], nm[48];
        if (g_sug[i].custom) {
            snprintf(key, sizeof(key), "c:%s", g_gemoji[g_sug[i].idx].id);
            snprintf(nm, sizeof(nm), ":%s:", g_gemoji[g_sug[i].idx].name);
        } else {
            snprintf(key, sizeof(key), "u:%s", EMOJI_DEFS[g_sug[i].idx].hex);
            snprintf(nm, sizeof(nm), ":%s:", EMOJI_DEFS[g_sug[i].idx].name);
        }
        if (i == g_sug_cur) gui_round(x + 8, ly, w - 16, 32, 6, C_SEL);
        const gui_image_t *img = emoji_img(key, 24);
        if (img) gui_image(img, x + 16, ly + 4, 24, 24, 0);
        fit_text(nm, w - 80, 16, clip, sizeof(clip));
        gui_text(x + 52, ly + 8, clip, i == g_sug_cur ? C_WHITE : C_TEXT, 16, g_sug[i].custom);
    }
}

// ---- join-server dialog --------------------------------------------------------------------------------------------

static void draw_modal(void)
{
    int w = 760, h = g_modal == MOD_JOIN ? 230 : 250, x = (GUI_W - w) / 2, y = (GUI_H - h) / 2 - 30;
    char clip[100], line[80];
    gui_round(x - 2, y - 2, w + 4, h + 4, 10, C_BORDER);
    gui_round(x, y, w, h, 8, C_LIST);
    if (g_modal == MOD_JOIN) {
        gui_text(x + 24, y + 20, "Join a Server", C_WHITE, 24, true);
        gui_text(x + 24, y + 64, "Paste an invite link or code", C_MUTED, 16, false);
        gui_round(x + 24, y + 96, w - 48, 44, 6, C_RAIL);
        int room = (w - 48 - 32) / 16;
        const char *shown = g_mod_in + (g_mod_len > (size_t)room ? g_mod_len - (size_t)room : 0);
        int ex = gui_text(x + 40, y + 110, shown, C_WHITE, 16, false);
        gui_fill(ex + 1, y + 108, 2, 20, C_WHITE);
        gui_text(x + 24, y + 164, "Enter look up   Ctrl+V paste   Esc cancel", C_MUTED, 16, false);
    } else {
        gui_text(x + 24, y + 20, "Join this server?", C_WHITE, 24, true);
        fit_text(g_inv_name, w - 48, 24, clip, sizeof(clip));
        gui_text(x + 24, y + 72, clip, C_TEXT, 24, true);
        char m[16], o[16];
        compact(g_inv_members, m, sizeof(m));
        compact(g_inv_online, o, sizeof(o));
        snprintf(line, sizeof(line), "%s members, %s online", m, o);
        gui_text(x + 24, y + 112, line, C_MUTED, 16, false);
        gui_text(x + 24, y + 150, "Join only servers you really want; many joins", C_MUTED, 16, false);
        gui_text(x + 24, y + 172, "in a short time can get an account flagged.", C_MUTED, 16, false);
        gui_text(x + 24, y + 208, "Y / Enter join   N / Esc cancel", C_WHITE, 16, true);
    }
}

// ---- drawing everything ------------------------------------------------------------------------------------------

// ---- notification card and settings panel -----------------------------------------------------------------------------

enum { S_NOTIFY, S_DM, S_MENTION, S_OPEN, S_SOUND, S_KIND, S_VOLUME, S_TEST, S_REFRESH, S_DMPOLL, S_IMAGES, S_PLUGINS, S_LOGOUT, S_COUNT };

static const char *onoff(bool b) { return b ? "On" : "Off"; }

static void set_row(int i, char *label, size_t lc, char *value, size_t vc)
{
    static const char *const L[S_COUNT] = {
        "Notifications", "  for direct messages", "  for mentions", "  for new messages in open chat",
        "Sound effects", "  sound", "  volume", "  play it now", "Refresh open chat every",
        "Check DMs for news every", "Show image attachments", "HELIOS plugins", "Log out and forget token" };
    copy_str(label, lc, L[i]);
    switch (i) {
    case S_NOTIFY: copy_str(value, vc, onoff(g_set.notify)); break;
    case S_DM: copy_str(value, vc, onoff(g_set.n_dm)); break;
    case S_MENTION: copy_str(value, vc, onoff(g_set.n_mention)); break;
    case S_OPEN: copy_str(value, vc, onoff(g_set.n_open)); break;
    case S_SOUND: copy_str(value, vc, onoff(g_set.sound)); break;
    case S_KIND: copy_str(value, vc, SOUND_NAMES[g_set.sound_kind]); break;
    case S_VOLUME: snprintf(value, vc, "%d percent", g_set.volume); break;
    case S_TEST: copy_str(value, vc, "Enter"); break;
    case S_REFRESH: snprintf(value, vc, "%d seconds", g_set.refresh_s); break;
    case S_DMPOLL: if (g_set.dm_poll_s) snprintf(value, vc, "%d seconds", g_set.dm_poll_s); else copy_str(value, vc, "Off"); break;
    case S_IMAGES: copy_str(value, vc, onoff(g_set.images)); break;
    case S_PLUGINS: snprintf(value, vc, "%s (%d found)", onoff(g_set.plugins), n_plug); break;
    default: copy_str(value, vc, g_set_confirm ? "Press Enter again" : "Enter"); break;
    }
}

static void draw_settings(void)
{
    int w = 900, h = 70 + S_COUNT * 30 + 104, x = (GUI_W - w) / 2, y = (GUI_H - h) / 2;
    char label[48], value[40], clip[64];
    gui_round(x - 2, y - 2, w + 4, h + 4, 10, C_BORDER);
    gui_round(x, y, w, h, 8, C_LIST);
    gui_text(x + 24, y + 16, "User settings", C_WHITE, 24, true);
    for (int i = 0; i < S_COUNT; i++) {
        int ly = y + 62 + i * 30;
        set_row(i, label, sizeof(label), value, sizeof(value));
        if (i == g_set_cur) gui_round(x + 12, ly - 2, w - 24, 28, 6, C_SEL);
        bool sub = label[0] == ' ';
        bool dim = (i >= S_DM && i <= S_OPEN && !g_set.notify) || (i >= S_KIND && i <= S_TEST && !g_set.sound);
        gui_text(x + 24, ly + 3, label, dim ? 0x6D6F78 : (sub ? C_MUTED : C_TEXT), 16, !sub);
        bool warn = i == S_LOGOUT || (i == S_PLUGINS && g_set.plugins);
        gui_text(x + w - 24 - gui_text_width(value, 16), ly + 3, value, i == g_set_cur ? C_WHITE : (warn ? 0xF0B232 : C_LINK), 16, true);
    }
    int fy = y + 62 + S_COUNT * 30 + 8;
    gui_text(x + 24, fy, "Custom sound: PCM WAV at /disk/dcsound.wav", C_MUTED, 16, false);
    gui_text(x + 24, fy + 24, "Plugins: HELIOS .hls/.rxe files in /disk/dcplug", C_MUTED, 16, false);
    char names[80] = "Found:";
    for (int i = 0; i < n_plug && strlen(names) + 14 < sizeof(names); i++) { strcat(names, " /"); strcat(names, g_plug[i].name); }
    if (n_plug == 0) copy_str(names, sizeof(names), "Found: none");
    fit_text(names, w - 48, 16, clip, sizeof(clip));
    gui_text(x + 24, fy + 48, clip, g_set.plugins ? C_TEXT : C_MUTED, 16, false);
    gui_text(x + 24, fy + 72, "Up/Down choose  Left/Right change  Esc close", C_MUTED, 16, false);
}

// A popup in the lower right for a new message; wrapped to two lines.
static void draw_notif(void)
{
    if (!g_notif_title[0] || tsc_reached(g_notif_until)) return;
    char l1[40], l2[40];
    size_t len = strlen(g_notif_text);
    l2[0] = '\0';
    if (len <= 30) copy_str(l1, sizeof(l1), g_notif_text);
    else {
        size_t cut = 30;
        while (cut > 12 && g_notif_text[cut] != ' ') cut--;
        memcpy(l1, g_notif_text, cut);
        l1[cut] = '\0';
        fit_text(g_notif_text + cut + (g_notif_text[cut] == ' ' ? 1 : 0), 30 * 16, 16, l2, sizeof(l2));
    }
    int w = 520, h = 20 + 24 + 24 * (l2[0] ? 2 : 1) + 8, x = GUI_W - 16 - w, y = INPUT_Y - 12 - h;
    char title[40];
    gui_round(x - 2, y - 2, w + 4, h + 4, 10, C_BORDER);
    gui_round(x, y, w, h, 8, 0x111214);
    gui_fill(x, y + 10, 5, h - 20, g_notif_rgb);
    fit_text(g_notif_title, w - 40, 16, title, sizeof(title));
    gui_text(x + 20, y + 14, title, C_WHITE, 16, true);
    gui_text(x + 20, y + 40, l1, C_TEXT, 16, false);
    if (l2[0]) gui_text(x + 20, y + 64, l2, C_TEXT, 16, false);
}

static void draw_toast(void)
{
    if (!g_status[0] || tsc_reached(g_status_until)) return;
    char l1[64], l2[64];
    size_t len = strlen(g_status);
    l2[0] = '\0';
    if (len <= 52) copy_str(l1, sizeof(l1), g_status);
    else {
        size_t cut = 52;
        while (cut > 20 && g_status[cut] != ' ') cut--;
        memcpy(l1, g_status, cut);
        l1[cut] = '\0';
        fit_text(g_status + cut + (g_status[cut] == ' ' ? 1 : 0), 52 * 16, 16, l2, sizeof(l2));
    }
    int n1 = (int)strlen(l1), n2 = (int)strlen(l2);
    int w = (n1 > n2 ? n1 : n2) * 16 + 32, h = l2[0] ? 56 : 36;
    int x = CHAT_X + CHAT_W / 2 - w / 2;
    gui_round(x, MSG_TOP + 8, w, h, 8, 0x111214);
    gui_text(x + 16, MSG_TOP + 18, l1, g_status_rgb, 16, true);
    if (l2[0]) gui_text(x + 16, MSG_TOP + 42, l2, g_status_rgb, 16, true);
}

static void draw_all(void)
{
    relayout();
    sug_update();
    gui_fill(0, 0, GUI_W, GUI_H, C_CHAT);
    draw_rail();
    draw_list();
    draw_header();
    if (g_open) draw_messages(); else draw_empty();
    draw_input();
    draw_suggest();
    draw_notif();
    draw_toast();
    if (g_help) draw_help();
    if (g_switch) draw_switcher();
    if (g_modal != MOD_NONE) draw_modal();
    if (g_settings) draw_settings();
    gui_present();
}

// ---- actions ---------------------------------------------------------------------------------------------------------

// ---- HELIOS plugins -------------------------------------------------------------------------------------------------
// A plugin is a HELIOS program (.hls source or .rxe bytecode) in /disk/dcplug. It is run as
//   plugin <event> <channel> <author> <text>      (event: dm, mention, msg, cmd)
// and whatever it prints is read back, one action per line:
//   notify: <text>   sound: ping|chime|pop|bell|custom   toast: <text>   send: <text> (bot tokens only)
// Typing /<name> <args> in the message box runs plugin <name> with the event "cmd".

static bool try_send(const char *text, bool clear_input);
static void notify(int kind, const char *chan, const char *who, const char *text);

// The sample plugin that "/plugins example" writes.
static const char DEMO_PLUGIN[] =
    "// dcdemo.hls - example plugin for the RadiumOS Discord client.\n"
    "// Run as: dcdemo.hls <event> <channel> <author> <text>\n"
    "//   event: dm, mention, msg (new message in the open chat), cmd (you typed /dcdemo <text>)\n"
    "// Whatever it prints is read back, one action per line:\n"
    "//   notify: <text>   popup + sound      sound: ping|chime|pop|bell|custom\n"
    "//   toast: <text>    a line at the top  send: <text>  (bot tokens only)\n"
    "fn main() {\n"
    "    let ev = argv(1);\n"
    "    match ev {\n"
    "        \"dm\" => {\n"
    "            println(\"notify: private message from \" + argv(3));\n"
    "            println(\"sound: bell\");\n"
    "        }\n"
    "        \"mention\" => {\n"
    "            println(\"toast: \" + argv(3) + \" mentioned you in \" + argv(2));\n"
    "            println(\"sound: chime\");\n"
    "        }\n"
    "        \"cmd\" => {\n"
    "            println(\"toast: dcdemo heard \" + argv(4) + \" in \" + argv(2));\n"
    "        }\n"
    "        _ => {\n"
    "            println(\"\");\n"
    "        }\n"
    "    }\n"
    "}\n";

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    if (n <= e) return false;
    for (size_t i = 0; i < e; i++) {
        char a = name[n - e + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (a != b) return false;
    }
    return true;
}

static void plugins_scan(void)
{
    static DirectoryEntry ents[32];
    n_plug = 0;
    int n = avfs_list_directory(PLUG_DIR, ents, 32);
    for (int i = 0; i < n && n_plug < MAX_PLUG; i++) {
        if (ents[i].is_directory || (!has_ext(ents[i].name, ".hls") && !has_ext(ents[i].name, ".rxe"))) continue;
        plug_t *p = &g_plug[n_plug];
        size_t stem = strlen(ents[i].name) - 4;
        if (stem == 0 || stem >= sizeof(p->name)) continue;
        for (size_t k = 0; k < stem; k++) { char c = ents[i].name[k]; p->name[k] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
        p->name[stem] = '\0';
        snprintf(p->path, sizeof(p->path), "%s/%s", PLUG_DIR, ents[i].name);
        bool dup = false;
        for (int k = 0; k < n_plug; k++) if (strcmp(g_plug[k].name, p->name) == 0) dup = true;
        if (!dup) n_plug++;
    }
}

static void show_notif(const char *title, const char *text, uint32_t rgb)
{
    copy_str(g_notif_title, sizeof(g_notif_title), title);
    copy_str(g_notif_text, sizeof(g_notif_text), text);
    g_notif_rgb = rgb;
    g_notif_until = tsc_deadline_ms(7000);
    g_dirty = true;
}

static void plugin_apply_output(const char *who)
{
    static char out[2048];
    int n = avfs_file_exists(PLUG_OUT) ? avfs_get_filesize(PLUG_OUT) : 0;
    if (n <= 0) return;
    if (n >= (int)sizeof(out)) n = (int)sizeof(out) - 1;
    if (avfs_read_file(PLUG_OUT, out, (uint32_t)n, 0) < 0) return;
    out[n] = '\0';
    int actions = 0;
    for (char *line = out; *line && actions < 8; ) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = '\0';
        char *arg = strchr(line, ':');
        if (arg) {
            *arg++ = '\0';
            while (*arg == ' ') arg++;
            if (!strcmp(line, "notify")) { show_notif(who, arg, C_BLURPLE); if (g_set.sound) play_sound(g_set.sound_kind); actions++; }
            else if (!strcmp(line, "sound")) {
                int k = !strcmp(arg, "chime") ? 1 : !strcmp(arg, "pop") ? 2 : !strcmp(arg, "bell") ? 3 : !strcmp(arg, "custom") ? 4 : 0;
                if (g_set.sound) play_sound(k);
                actions++;
            }
            else if (!strcmp(line, "toast")) { toast(C_TEXT, arg); actions++; }
            else if (!strcmp(line, "send")) {
                if (is_bot()) try_send(arg, false);
                else toast(0xF0B232, "plugins cannot send as a user account (spam protection)");
                actions++;
            }
        }
        if (!eol) break;
        line = eol + 1;
    }
}

static bool plugin_run(const plug_t *p, const char *ev, const char *chan, const char *who, const char *text)
{
    char path[48], a_ev[12], a_chan[48], a_who[48], a_text[200], why[80];
    char *argv[6];
    copy_str(path, sizeof(path), p->path);
    copy_str(a_ev, sizeof(a_ev), ev);
    copy_str(a_chan, sizeof(a_chan), chan);
    copy_str(a_who, sizeof(a_who), who);
    copy_str(a_text, sizeof(a_text), text);
    argv[0] = path; argv[1] = a_ev; argv[2] = a_chan; argv[3] = a_who; argv[4] = a_text; argv[5] = NULL;
    avfs_save_file(PLUG_OUT, "\n", 1, true);
    if (terminal_begin_capture(PLUG_OUT) != 0) return false;
    int rc = helios_exec_file_args((const uint8_t *)path, 0, 5, argv);
    terminal_end_capture();
    if (rc < 0) {
        snprintf(why, sizeof(why), "plugin /%s failed (see: helios run %s)", p->name, p->path);
        toast(0xF23F42, why);
        return false;
    }
    plugin_apply_output(p->name);
    return true;
}

// Tells every plugin about an event (at most one round every 2 seconds, first four plugins).
static void plugins_emit(const char *ev, const char *chan, const char *who, const char *text)
{
    if (!g_set.plugins || n_plug == 0 || !tsc_reached(g_plug_next)) return;
    g_plug_next = tsc_deadline_ms(2000);
    for (int i = 0; i < n_plug && i < 4; i++) plugin_run(&g_plug[i], ev, chan, who, text);
}

// "/name args" or "/plugins". True when the line was a plugin command.
static bool run_slash(const char *line)
{
    char name[12];
    size_t n = 0;
    const char *p = line + 1;
    while (*p && *p != ' ' && n + 1 < sizeof(name)) { char c = *p++; name[n++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
    name[n] = '\0';
    while (*p == ' ') p++;
    if (!strcmp(name, "plugins") && !strcmp(p, "example")) {
        avfs_create_dir(PLUG_DIR);
        if (avfs_save_file(PLUG_DIR "/dcdemo.hls", DEMO_PLUGIN, (uint32_t)strlen(DEMO_PLUGIN), true) == 0) {
            plugins_scan();
            toast(C_TEXT, g_set.plugins ? "Installed /dcdemo in " PLUG_DIR : "Installed /dcdemo; turn plugins on in F3");
        } else toast(0xF23F42, "could not write " PLUG_DIR " (is /disk mounted?)");
        return true;
    }
    if (!strcmp(name, "plugins")) {
        char list[100] = "Plugins:";
        if (!g_set.plugins) copy_str(list, sizeof(list), "Plugins are off (F3 turns them on)");
        else if (n_plug == 0) copy_str(list, sizeof(list), "No plugins in " PLUG_DIR);
        else for (int i = 0; i < n_plug && strlen(list) + 14 < sizeof(list); i++) { strcat(list, " /"); strcat(list, g_plug[i].name); }
        toast(C_TEXT, list);
        return true;
    }
    for (int i = 0; i < n_plug; i++) {
        if (!strcmp(g_plug[i].name, name)) { plugin_run(&g_plug[i], "cmd", g_open_name, g_me, p); return true; }
    }
    return false;
}

// ---- notifications ---------------------------------------------------------------------------------------------------

// Message text without the emoji markers, for popups and plugins.
static void plain_text(const msg_t *m, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *s = m->text; *s && o + 1 < cap; s++) {
        if (*s == EMO_MARK) { if (s[1]) s++; continue; }
        out[o++] = *s == '\n' ? ' ' : *s;
    }
    out[o] = '\0';
    if (!o) copy_str(out, cap, m->n_att ? "[attachment]" : "[message]");
}

// kind: 0 direct message, 1 mention, 2 message in the open chat.
static void notify(int kind, const char *chan, const char *who, const char *text)
{
    char title[48];
    if (!g_set.notify) return;
    if ((kind == 0 && !g_set.n_dm) || (kind == 1 && !g_set.n_mention) || (kind == 2 && !g_set.n_open)) return;
    if (kind == 1) snprintf(title, sizeof(title), "%s mentioned you", who);
    else if (kind == 0) copy_str(title, sizeof(title), who);
    else snprintf(title, sizeof(title), "%s in %s", who, chan);
    show_notif(title, text, kind == 1 ? 0xF0B232 : C_BLURPLE);
    if (g_set.sound) play_sound(g_set.sound_kind);
    plugins_emit(kind == 0 ? "dm" : kind == 1 ? "mention" : "msg", chan, who, text);
}

// After a refresh: announces the newest message from someone else that arrived since old_id.
static void notify_new_since(const char *old_id)
{
    const msg_t *pick = NULL;
    char text[160];
    if (!old_id[0]) return;
    for (int i = n_msg - 1; i >= 0 && id_cmp(g_msg[i].id, old_id) > 0; i--) {
        const msg_t *m = &g_msg[i];
        if (strcmp(m->uid, g_me_id) == 0) continue;
        if (!pick || (m->mentioned && !pick->mentioned)) pick = m;
    }
    if (!pick) return;
    plain_text(pick, text, sizeof(text));
    notify(pick->mentioned ? 1 : 2, g_open_name, pick->who, text);
}

// Looks for DMs with news: one request for the list, at most two more to read the newest messages.
static void dm_poll(void)
{
    int fetched = 0;
    if (is_bot() || g_auth_dead || !tsc_reached(g_backoff_until)) return;
    if (!dc_load_dms()) return;
    for (int i = 0; i < n_dm && fetched < 2; i++) {
        dm_t *d = &g_dm[i];
        char path[96], text[160];
        int st;
        if (id_cmp(d->last, d->seen) <= 0) continue;
        if (g_open && g_open_dm && strcmp(g_open_id, d->id) == 0) { copy_str(d->seen, sizeof(d->seen), d->last); continue; }
        snprintf(path, sizeof(path), "/channels/%s/messages?limit=1", d->id);
        if (dc_api("GET", path, NULL, &st) < 0 || st != 200) break;
        fetched++;
        copy_str(d->seen, sizeof(d->seen), d->last);
        if (parse_batch() < 1 || strcmp(g_batch[0].uid, g_me_id) == 0) continue;
        d->unread++;
        plain_text(&g_batch[0], text, sizeof(text));
        notify(0, d->name, d->name, text);
    }
    g_dirty = true;
}

static void reload_lists(void)
{
    char why[120];
    if (!is_bot() && !dc_load_dms()) { explain_failure(why, sizeof(why)); toast(0xF23F42, why); }
    if (!dc_load_guilds()) { explain_failure(why, sizeof(why)); toast(0xF23F42, why); }
}

static void refresh_messages(void)
{
    static int cycle;
    char why[120], old[24];
    if (!g_open || g_auth_dead) return;
    copy_str(old, sizeof(old), n_msg ? g_msg[n_msg - 1].id : "");
    // A full page every 10th refresh picks up edits and deletions; the rest only ask for new messages.
    bool ok = (++cycle % 10 == 0) ? dc_load_messages(g_open_id) : dc_load_newer(g_open_id);
    if (!ok) { explain_failure(why, sizeof(why)); toast(0xF23F42, why); g_refresh_fail++; }
    else { g_refresh_fail = 0; notify_new_since(old); }
}

// Loads one older page when the view sits at the top, within the history pacing limits.
static void service_history(void)
{
    char why[120];
    if (!g_hist_want || !g_open || g_auth_dead || !tsc_reached(g_backoff_until)) return;
    if (tsc_reached(g_hist_win)) g_hist_burst = 0;
    if (g_hist_burst >= HIST_BURST || !tsc_reached(g_hist_next)) return;

    if (g_hist_burst == 0) g_hist_win = tsc_deadline_ms(HIST_WINDOW_MS);
    g_hist_burst++;
    g_hist_next = tsc_deadline_ms(HIST_GAP_MS);
    g_hist_loading = true;
    draw_all();
    if (!dc_load_older(g_open_id)) {
        explain_failure(why, sizeof(why));
        toast(0xF23F42, why);
        g_hist_next = tsc_deadline_ms(g_last_status == 429 ? 10000 : 30000);   // do not hammer a failing route
    }
    g_hist_loading = false;
    g_dirty = true;
}

static void open_channel(const char *id, const char *name, const char *topic, bool dm)
{
    copy_str(g_open_id, sizeof(g_open_id), id);
    copy_str(g_open_name, sizeof(g_open_name), name);
    copy_str(g_open_topic, sizeof(g_open_topic), topic);
    g_open = true;
    g_open_dm = dm;
    g_scroll = 0;
    n_msg = 0;
    g_hist_end = false;
    g_hist_want = false;
    g_hist_burst = 0;
    toast(C_MUTED, "loading messages ...");
    draw_all();
    char why[120];
    if (!dc_load_messages(id)) { explain_failure(why, sizeof(why)); toast(0xF23F42, why); }
    else g_status[0] = '\0';
    g_focus = F_INPUT;
    g_dirty = true;
}

static void open_row(int idx)
{
    if (g_guild_open < 0) {
        if (idx >= 0 && idx < n_dm) {
            g_dm[idx].unread = 0;
            copy_str(g_dm[idx].seen, sizeof(g_dm[idx].seen), g_dm[idx].last);
            open_channel(g_dm[idx].id, g_dm[idx].name, "", true);
        }
    } else if (list_cursor_ok(idx)) {
        open_channel(g_lrow[idx].id, g_lrow[idx].name, g_lrow[idx].topic, false);
    }
}

static void keep_list_visible(void)
{
    int per = g_guild_open < 0 ? (GUI_H - PANEL_H - HEAD_H - 28) / DM_ROW : (GUI_H - PANEL_H - HEAD_H - 8) / LIST_ROW - 1;
    if (g_list_cur < g_list_top) g_list_top = g_list_cur;
    if (g_list_cur >= g_list_top + per) g_list_top = g_list_cur - per + 1;
    if (g_list_top < 0) g_list_top = 0;
}

static void go_home(void)
{
    g_guild_open = -1;
    n_gemoji = 0;
    g_rail_cur = 0;
    g_list_cur = g_list_top = 0;
    g_focus = F_LIST;
    n_lrow = 0;
    g_dirty = true;
}

static void select_guild(int idx)
{
    if (idx < 0 || idx >= n_guild) return;
    toast(C_MUTED, "loading channels ...");
    g_guild_open = idx;
    g_rail_cur = idx + 1;
    if (idx < g_rail_top) g_rail_top = idx;
    if (idx >= g_rail_top + RAIL_ROWS) g_rail_top = idx - RAIL_ROWS + 1;
    g_list_cur = g_list_top = 0;
    draw_all();
    char why[120];
    if (!dc_load_channels(g_guild[idx].id)) { explain_failure(why, sizeof(why)); toast(0xF23F42, why); n_lrow = 0; return; }
    dc_load_emojis(g_guild[idx].id);
    g_status[0] = '\0';
    g_focus = F_LIST;
    for (int i = 0; i < n_lrow; i++) if (g_lrow[i].kind == ROW_TEXT) { g_list_cur = i; break; }
    keep_list_visible();
    if (list_cursor_ok(g_list_cur)) open_row(g_list_cur);
    g_dirty = true;
}

static void move_rail(int dir)
{
    int n = g_rail_cur + dir;
    if (n < 0 || n > n_guild + 1) return;
    g_rail_cur = n;
    if (n > 0) {
        if (n - 1 < g_rail_top) g_rail_top = n - 1;
        if (n - 1 >= g_rail_top + RAIL_ROWS) g_rail_top = n - RAIL_ROWS;
    }
}

static void move_list(int dir, bool open)
{
    int i = g_list_cur;
    for (;;) {
        i += dir;
        if (i < 0 || i >= list_count()) return;
        if (list_cursor_ok(i)) break;
    }
    g_list_cur = i;
    keep_list_visible();
    if (open) open_row(i);
}

static void hex_to_utf8(const char *hex, char *out, size_t cap)
{
    size_t o = 0;
    while (*hex) {
        unsigned cp = 0;
        while (*hex && *hex != '-') {
            char h = *hex++;
            cp = (cp << 4) | (unsigned)((h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : 0);
        }
        if (*hex == '-') hex++;
        if (o + 5 >= cap) break;
        o += (size_t)utf8_put(cp, out + o);
    }
    out[o] = '\0';
}

// Turns :name: into <:name:id> (server emoji) or the unicode emoji; unknown names stay as typed.
static void expand_emoji(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < cap; ) {
        bool after_lt = i > 0 && (in[i - 1] == '<' || (in[i - 1] == 'a' && i > 1 && in[i - 2] == '<'));
        if (in[i] == ':' && !after_lt) {
            size_t j = i + 1;
            while (in[j] && name_char(in[j]) && j - i <= 33) j++;
            if (in[j] == ':' && j - i >= 3) {
                char name[34], rep[80];
                memcpy(name, in + i + 1, j - i - 1);
                name[j - i - 1] = '\0';
                rep[0] = '\0';
                for (int k = 0; k < n_gemoji && !rep[0]; k++)
                    if (strcmp(g_gemoji[k].name, name) == 0) snprintf(rep, sizeof(rep), "<%s:%s:%s>", g_gemoji[k].animated ? "a" : "", name, g_gemoji[k].id);
                for (int k = 0; k < EMOJI_N && !rep[0]; k++)
                    if (strcmp(EMOJI_DEFS[k].name, name) == 0) hex_to_utf8(EMOJI_DEFS[k].hex, rep, sizeof(rep));
                if (rep[0] && o + strlen(rep) + 1 < cap) {
                    for (const char *c = rep; *c; c++) out[o++] = *c;
                    i = j + 1;
                    continue;
                }
            }
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
}

static uint32_t text_hash(const char *s)
{
    uint32_t h = 5381;
    for (; *s; s++) h = h * 33 + (unsigned char)*s;
    return h;
}

// Sends text to the open channel (emoji shortcodes expanded) within the spam guards. True when it went out.
static bool try_send(const char *text, bool clear_input)
{
    static char out[2400];
    char why[120], old[24];
    int st = 0;
    if (!g_open || !text[0]) return false;
    expand_emoji(text, out, sizeof(out));
    if (strlen(out) > 2000) { toast(0xF0B232, "Too long: Discord messages are at most 2000 characters"); return false; }
    uint32_t hash = text_hash(out);
    if (hash == g_dup_hash && !tsc_reached(g_dup_until)) { toast(0xF0B232, "You just sent that exact message; repeats look like spam"); return false; }
    if (tsc_reached(g_send_win)) g_send_burst = 0;
    if (g_send_burst >= SEND_BURST || !tsc_reached(g_send_next)) { toast(0xF0B232, "Slow down: Discord limits how fast messages can be sent"); return false; }
    if (g_send_burst == 0) g_send_win = tsc_deadline_ms(SEND_WINDOW_MS);
    g_send_burst++;
    g_send_next = tsc_deadline_ms(SEND_GAP_MS);
    toast(C_MUTED, "sending ...");
    draw_all();
    if (dc_send(g_open_id, out, &st)) {
        g_dup_hash = hash;
        g_dup_until = tsc_deadline_ms(DUP_WINDOW_MS);
        if (clear_input) { g_input_len = 0; g_input[0] = '\0'; }
        g_scroll = 0;
        copy_str(old, sizeof(old), n_msg ? g_msg[n_msg - 1].id : "");
        if (dc_load_messages(g_open_id)) notify_new_since(old);
        g_status[0] = '\0';
        g_dirty = true;
        return true;
    }
    explain_failure(why, sizeof(why));
    toast(0xF23F42, why);
    g_dirty = true;
    return false;
}

static void send_input(void)
{
    if (!g_open || g_input_len == 0) return;
    if (g_input[0] == '/') {
        bool builtin = strncmp(g_input, "/plugins", 8) == 0;
        if ((g_set.plugins || builtin) && run_slash(g_input)) {
            g_input_len = 0;
            g_input[0] = '\0';
            g_dirty = true;
            return;
        }
        if (!g_set.plugins) {   // a command for a plugin that is switched off is not a chat message
            for (int i = 0; i < n_plug; i++) {
                size_t l = strlen(g_plug[i].name);
                if (strncmp(g_input + 1, g_plug[i].name, l) == 0 && (g_input[1 + l] == ' ' || g_input[1 + l] == '\0')) {
                    toast(0xF0B232, "Plugins are off: F3 turns them on");
                    return;
                }
            }
        }
    }
    try_send(g_input, true);
}

static void paste_into(char *buf, size_t *len, size_t cap, bool keep_newlines)
{
    static char host[CLIPBOARD_HOST_MAX + 1];
    int n = clipboard_from_host(host, sizeof(host));
    if (n < 0) { toast(0xF23F42, "no host clipboard (start RadiumOS with make run)"); return; }
    for (int i = 0; i < n && *len + 1 < cap; i++) {
        unsigned char c = (unsigned char)host[i];
        if (c == '\r') continue;
        if (c == '\t') c = ' ';
        if (c == '\n' && !keep_newlines) c = ' ';
        if (c == '\n' || (c >= 0x20 && c < 0x7F)) buf[(*len)++] = (char)c;
    }
    buf[*len] = '\0';
}

// ---- joining a server ----------------------------------------------------------------------------------------------

static void open_join(void)
{
    if (is_bot()) { toast(0xF0B232, "Bots cannot use invites: add the bot from the developer portal (OAuth2 URL)"); return; }
    g_modal = MOD_JOIN;
    g_mod_len = 0;
    g_mod_in[0] = '\0';
}

static void join_lookup(void)
{
    char code[40], why[120];
    if (!parse_invite(g_mod_in, code, sizeof(code))) { toast(0xF23F42, "That does not look like an invite link or code"); return; }
    toast(C_MUTED, "looking up the invite ...");
    draw_all();
    if (!dc_invite_preview(code, g_inv_gid, sizeof(g_inv_gid), g_inv_name, sizeof(g_inv_name), &g_inv_members, &g_inv_online)) {
        explain_failure(why, sizeof(why));
        toast(0xF23F42, why);
        return;
    }
    copy_str(g_inv_code, sizeof(g_inv_code), code);
    g_status[0] = '\0';
    g_modal = MOD_CONFIRM;
}

static void join_confirm(void)
{
    char why[120], gid[24], name[64];
    bool captcha = false;
    if (tsc_reached(g_join_win)) g_join_burst = 0;
    if (g_join_burst >= JOIN_BURST || !tsc_reached(g_join_next)) {
        g_modal = MOD_NONE;
        toast(0xF0B232, "Slow down: wait about a minute between joins, and at most 3 an hour");
        return;
    }
    if (g_join_burst == 0) g_join_win = tsc_deadline_ms(JOIN_WINDOW_MS);
    g_join_burst++;
    g_join_next = tsc_deadline_ms(JOIN_GAP_MS);
    toast(C_MUTED, "joining ...");
    draw_all();
    if (!dc_invite_join(g_inv_code, &captcha)) {
        g_modal = MOD_NONE;
        if (captcha) toast(0xF0B232, "Discord wants a captcha for this invite. Accept it in the Discord app or website, then press F5 here.");
        else { explain_failure(why, sizeof(why)); toast(0xF23F42, why); }
        return;
    }
    js_text(js_get(g_resp, "guild"), "id", gid, sizeof(gid));
    if (!gid[0]) copy_str(gid, sizeof(gid), g_inv_gid);
    copy_str(name, sizeof(name), g_inv_name);
    g_modal = MOD_NONE;
    dc_load_guilds();
    for (int i = 0; i < n_guild; i++) {
        if (strcmp(g_guild[i].id, gid) == 0) { select_guild(i); break; }
    }
    snprintf(why, sizeof(why), "Joined %s", name);
    toast(C_GREEN, why);
}

// ---- settings changes --------------------------------------------------------------------------------------------------

static int cycle_opt(const int *opts, int n, int cur, int dir)
{
    int at = 0;
    for (int i = 0; i < n; i++) if (opts[i] == cur) at = i;
    return opts[(at + dir + n) % n];
}

// Changes setting i (dir -1 / +1). True when the client should quit (after log out).
static bool set_change(int i, int dir)
{
    static const int REFRESH[] = { 5, 8, 15, 30 }, POLL[] = { 0, 30, 60, 120 }, VOLUME[] = { 20, 40, 60, 80, 100 };
    if (i != S_LOGOUT) g_set_confirm = false;
    switch (i) {
    case S_NOTIFY: g_set.notify = !g_set.notify; break;
    case S_DM: g_set.n_dm = !g_set.n_dm; break;
    case S_MENTION: g_set.n_mention = !g_set.n_mention; break;
    case S_OPEN: g_set.n_open = !g_set.n_open; break;
    case S_SOUND: g_set.sound = !g_set.sound; break;
    case S_KIND: g_set.sound_kind = (g_set.sound_kind + dir + 5) % 5; g_snd_until = 0; if (g_set.sound) play_sound(g_set.sound_kind); break;
    case S_VOLUME: g_set.volume = cycle_opt(VOLUME, 5, g_set.volume, dir); g_snd_until = 0; if (g_set.sound) play_sound(g_set.sound_kind); break;
    case S_TEST: g_snd_until = 0; play_sound(g_set.sound_kind); return false;
    case S_REFRESH: g_set.refresh_s = cycle_opt(REFRESH, 4, g_set.refresh_s, dir); break;
    case S_DMPOLL: g_set.dm_poll_s = cycle_opt(POLL, 4, g_set.dm_poll_s, dir); break;
    case S_IMAGES: g_set.images = !g_set.images; break;
    case S_PLUGINS: g_set.plugins = !g_set.plugins; if (g_set.plugins) plugins_scan(); break;
    default:
        if (dir < 0) return false;
        if (!g_set_confirm) { g_set_confirm = true; return false; }
        avfs_remove_file(TOKEN_FILE);
        return true;
    }
    set_save();
    return false;
}

// Returns true to quit.
static bool handle_key(const fs_event_t *ev)
{
    g_dirty = true;

    if (g_help) { g_help = false; return false; }

    if (g_modal == MOD_JOIN) {
        if (ev->sc == 0x01) { g_modal = MOD_NONE; return false; }
        if (ev->sc == 0x1C) { join_lookup(); return false; }
        if (ev->sc == 0x0E) { if (g_mod_len) g_mod_in[--g_mod_len] = '\0'; return false; }
        if (ev->ctrl && ev->sc == 0x2F) { paste_into(g_mod_in, &g_mod_len, sizeof(g_mod_in), false); return false; }
        if (ev->ch >= 0x20 && ev->ch < 0x7F && g_mod_len + 1 < sizeof(g_mod_in)) { g_mod_in[g_mod_len++] = ev->ch; g_mod_in[g_mod_len] = '\0'; }
        return false;
    }
    if (g_modal == MOD_CONFIRM) {
        if (ev->sc == 0x01 || ev->ch == 'n' || ev->ch == 'N') g_modal = MOD_NONE;
        else if (ev->sc == 0x1C || ev->ch == 'y' || ev->ch == 'Y') join_confirm();
        return false;
    }

    if (g_settings) {
        if (ev->sc == 0x01 || ev->sc == 0x3D) { g_settings = false; g_set_confirm = false; return false; }
        if (ev->sc == 0x48) { if (g_set_cur > 0) g_set_cur--; g_set_confirm = false; }
        else if (ev->sc == 0x50) { if (g_set_cur + 1 < S_COUNT) g_set_cur++; g_set_confirm = false; }
        else if (ev->sc == 0x4B) set_change(g_set_cur, -1);
        else if (ev->sc == 0x4D || ev->sc == 0x1C || ev->ch == ' ') { if (set_change(g_set_cur, 1)) return true; }
        return false;
    }

    if (g_focus == F_INPUT && !ev->ctrl && !ev->alt) {
        sug_update();
        if (n_sug > 0) {
            if (ev->sc == 0x48) { g_sug_cur = (g_sug_cur + n_sug - 1) % n_sug; return false; }
            if (ev->sc == 0x50) { g_sug_cur = (g_sug_cur + 1) % n_sug; return false; }
            if (ev->sc == 0x0F || ev->sc == 0x1C) { sug_pick(); return false; }
            if (ev->sc == 0x01) { g_sug_off = true; return false; }
        }
    }
    if (g_switch) {
        if (ev->sc == 0x01) { g_switch = false; return false; }
        if (ev->sc == 0x48) { if (g_sw_cur > 0) g_sw_cur--; return false; }
        if (ev->sc == 0x50) { if (g_sw_cur + 1 < n_sw) g_sw_cur++; return false; }
        if (ev->sc == 0x0E) { if (g_sw_len) g_sw_q[--g_sw_len] = '\0'; g_sw_cur = 0; sw_rebuild(); return false; }
        if (ev->sc == 0x1C) {
            g_switch = false;
            if (n_sw == 0) return false;
            sw_t s = g_sw[g_sw_cur];
            if (s.kind == 0) { g_guild_open = -1; n_gemoji = 0; g_rail_cur = 0; g_list_cur = s.idx; keep_list_visible(); open_row(s.idx); }
            else if (s.kind == 1) select_guild(s.idx);
            else { g_list_cur = s.idx; keep_list_visible(); open_row(s.idx); }
            return false;
        }
        if (ev->ch >= 0x20 && ev->ch < 0x7F && g_sw_len + 1 < sizeof(g_sw_q)) { g_sw_q[g_sw_len++] = ev->ch; g_sw_q[g_sw_len] = '\0'; g_sw_cur = 0; sw_rebuild(); }
        return false;
    }

    if (ev->sc == 0x3B || (ev->ctrl && ev->sc == 0x35)) { g_help = true; return false; }                 // F1, Ctrl+/
    if (ev->sc == 0x3D || (ev->ctrl && ev->sc == 0x33)) { g_settings = true; g_set_cur = 0; g_set_confirm = false; plugins_scan(); return false; }   // F3, Ctrl+,
    if (ev->ctrl && ev->sc == 0x25) { g_switch = true; g_sw_len = 0; g_sw_q[0] = '\0'; g_sw_cur = 0; sw_rebuild(); return false; }
    if (ev->ctrl && ev->sc == 0x10) return true;                                                           // Ctrl+Q
    if (ev->ctrl && ev->sc == 0x24) { open_join(); return false; }                                         // Ctrl+J
    if (ev->sc == 0x3C) { go_home(); return false; }                                                       // F2
    if (ev->sc == 0x3F) {                                                                                  // F5
        toast(C_MUTED, "refreshing ...");
        draw_all();
        reload_lists();
        refresh_messages();
        return false;
    }
    if (ev->sc == 0x40) { img_reset(); n_req = 0; toast(C_MUTED, "reloading images"); return false; }       // F6
    if (ev->sc == 0x49) { g_scroll += (MSG_BOT - MSG_TOP) * 2 / 3; return false; }                          // PgUp
    if (ev->sc == 0x51) { g_scroll -= (MSG_BOT - MSG_TOP) * 2 / 3; if (g_scroll < 0) g_scroll = 0; return false; }
    if (ev->sc == 0x47 && g_open) { g_scroll = 1 << 20; return false; }                                     // Home
    if (ev->sc == 0x4F && g_open) { g_scroll = 0; return false; }                                           // End

    if (ev->alt && ev->sc == 0x48) { if (ev->ctrl) { if (g_guild_open >= 0) { if (g_guild_open == 0) go_home(); else select_guild(g_guild_open - 1); } } else move_list(-1, true); return false; }
    if (ev->alt && ev->sc == 0x50) { if (ev->ctrl) { if (g_guild_open + 1 < n_guild) select_guild(g_guild_open + 1); } else move_list(1, true); return false; }

    if (ev->sc == 0x0F) {                                                                                  // Tab
        int order[3] = { F_RAIL, F_LIST, F_INPUT };
        int at = g_focus;
        for (int k = 0; k < 3; k++) {
            at = ev->shift ? (at + 2) % 3 : (at + 1) % 3;
            if (order[at] != F_INPUT || g_open) break;
        }
        g_focus = at;
        return false;
    }
    if (ev->sc == 0x01) {                                                                                  // Esc
        if (g_focus == F_INPUT) g_focus = F_LIST;
        else if (g_focus == F_LIST) g_focus = F_RAIL;
        else return true;
        return false;
    }

    if (g_focus == F_INPUT) {
        if (ev->sc == 0x48) { g_scroll += 22; return false; }
        if (ev->sc == 0x50) { g_scroll -= 22; if (g_scroll < 0) g_scroll = 0; return false; }
        if (ev->sc == 0x0E) { if (g_input_len) g_input[--g_input_len] = '\0'; return false; }
        if (ev->sc == 0x1C) { send_input(); return false; }
        if (ev->ctrl && ev->sc == 0x2F) { paste_into(g_input, &g_input_len, sizeof(g_input), true); return false; }
        if (ev->ch >= 0x20 && ev->ch < 0x7F && g_input_len + 1 < sizeof(g_input)) { g_input[g_input_len++] = ev->ch; g_input[g_input_len] = '\0'; }
        return false;
    }

    if (g_focus == F_RAIL) {
        if (ev->sc == 0x48) move_rail(-1);
        else if (ev->sc == 0x50) move_rail(1);
        else if (ev->sc == 0x1C) { if (g_rail_cur == 0) go_home(); else if (g_rail_cur == n_guild + 1) open_join(); else select_guild(g_rail_cur - 1); }
        return false;
    }

    // channel / DM list
    if (ev->sc == 0x48) move_list(-1, false);
    else if (ev->sc == 0x50) move_list(1, false);
    else if (ev->sc == 0x1C) open_row(g_list_cur);
    return false;
}

static void run_client(void)
{
    img_reset();
    n_req = 0;
    g_status[0] = '\0';
    toast(C_MUTED, "loading your servers and messages ...");
    draw_all();
    reload_lists();
    g_status[0] = '\0';
    g_dirty = true;

    uint64_t next_refresh = tsc_deadline_ms((uint32_t)g_set.refresh_s * 1000);
    uint64_t next_dm_poll = tsc_deadline_ms((uint32_t)g_set.dm_poll_s * 1000);
    uint64_t idle_since = tsc_deadline_ms(0);
    uint64_t last_input = tsc_deadline_ms(IDLE_SLOW_MS);   // reached = no key for IDLE_SLOW_MS
    plugins_scan();
    if (g_set.sound) rust_sfx_init();
    for (;;) {
        fs_event_t ev;
        bool had_key = false;
        while (fs_get_event(&ev)) {
            had_key = true;
            if (handle_key(&ev)) return;
        }
        if (had_key) { idle_since = tsc_deadline_ms(350); last_input = tsc_deadline_ms(IDLE_SLOW_MS); }
        int slow = tsc_reached(last_input) ? 4 : 1;   // nobody is looking: poll a quarter as often

        if (g_open && !g_switch && !g_help && !g_auth_dead && tsc_reached(next_refresh)) {
            int before = n_msg;
            refresh_messages();
            if (n_msg != before) g_dirty = true;
            next_refresh = tsc_deadline_ms((uint32_t)(g_set.refresh_s * 1000 * slow) << (g_refresh_fail < 3 ? g_refresh_fail : 3));
            g_dirty = true;
        }
        if (g_set.dm_poll_s && !g_auth_dead && tsc_reached(next_dm_poll)) {
            dm_poll();
            next_dm_poll = tsc_deadline_ms((uint32_t)g_set.dm_poll_s * 1000 * slow);
        }
        if (g_notif_title[0] && tsc_reached(g_notif_until)) { g_notif_title[0] = '\0'; g_dirty = true; }
        if (!had_key && !g_switch && !g_help) service_history();
        if (!had_key && n_req > 0 && tsc_reached(idle_since) && tsc_reached(g_img_next) && img_service()) g_dirty = true;
        if (g_status[0] && tsc_reached(g_status_until)) { g_status[0] = '\0'; g_dirty = true; }
        if (g_dirty) { g_dirty = false; draw_all(); }
        __asm__ volatile("pause");
    }
}

void discord_command(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "logout") == 0) {
        if (!vault_exists()) { print("discord: no saved token\n"); return; }
        avfs_remove_file(TOKEN_FILE);
        print("discord: saved token removed\n");
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "status") == 0) {
        print(vault_exists() ? "discord: a token is saved (encrypted) in " TOKEN_FILE "\n" : "discord: no token saved\n");
        return;
    }
    if (argc > 1) { print("Usage: discord | discord logout | discord status\n"); return; }

    if (!fs_begin()) { print("discord: cannot take over the screen\n"); return; }
    memset(g_token, 0, sizeof(g_token));
    g_open = false;
    g_guild_open = -1;
    g_focus = F_RAIL;
    g_rail_cur = g_rail_top = g_list_cur = g_list_top = 0;
    g_input_len = 0;
    g_input[0] = '\0';
    g_help = g_switch = false;
    g_settings = false;
    g_modal = MOD_NONE;
    g_notif_title[0] = '\0';
    set_load();
    n_gemoji = n_sug = 0;
    n_dm = n_guild = n_lrow = n_msg = 0;
    g_auth_dead = false;
    g_backoff_streak = g_backoff_secs = 0;
    g_backoff_until = 0;
    if (dc_login()) {
        if (gui_begin()) {
            run_client();
            gui_end();
        } else {
            message_box("Discord", "The client needs the 1280x720 console (this display is in plain text mode).", NULL, NULL, "Esc close");
        }
    }

    memset(g_token, 0, sizeof(g_token));
    memset(g_resp, 0, 4096);
    memset(g_input, 0, sizeof(g_input));
    fs_end();
}
