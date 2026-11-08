// ytm.c - text-based YouTube Music client
//
//   ytm [SEARCH TEXT]   open the player (and search straight away when text is given)
//   ytm play ID|URL     play one video by its id or a youtube.com / youtu.be address (audio)
//   ytm video ID|URL    the same as a video (needs the whole screen, so not inside the desktop)
//   ytm liked|history   print the liked songs / play history
//   ytm settings        print the saved settings
//   ytm login | logout  save / delete a Google account name and password, sealed with a passphrase (PRP vault)
//   ytm status          helper address, saved account, what is stored
//
// Inside the player (? shows this list):
//   / search (Up/Down recall earlier searches)   Enter play   Space pause   Left/Right seek 10 s   + - volume
//   n next   p previous   z shuffle   r repeat (off / track / list)   t sleep timer   v watch as video
//   a queue   d drop / delete   c clear queue   f or l like   Tab views (Results, Queue, Liked, History, Playlists)
//   S save the queue as a playlist   P add the selected track to a playlist   s settings   u sign in   q quit
//
// How it works: search results, audio and video come from tools/ytm_helper.py, which runs on the host (the guest reaches it
// at http://10.0.2.2:8787, or $YTM_API, or the address set in the settings). The helper uses yt-dlp, because YouTube only
// hands media to clients that pass its bot check and RadiumOS cannot. Audio is AAC .m4a in the fetch store
// (/disk/ytm.m4a) decoded by the Rust music player (rust_lib/src/mp4.rs); video is a small H.264/AAC .mp4 played by the
// `mp4` player.
//
// Saved on /disk: liked songs ytmfav.txt, history ytmhist.txt (with play counts), settings ytmset.txt, recent searches
// ytmq.txt, playlists ytp0.txt-ytp7.txt.
//
// Account: the name and password are kept in /disk/ytm.vlt, a PRP envelope (ChaCha20-Poly1305) under a key stretched from
// your passphrase. Google does not accept a password from third-party players, so they do not sign YouTube in by
// themselves: the player shows the saved account and keeps the credentials safe. Personal library, age-restricted and
// Premium tracks come through the helper's --cookies-from-browser option.

#include <stdint.h>
#include <stdbool.h>
#include "ytm.h"
#include "fsapp.h"
#include "shellvars.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../timers/tsc.h"
#include "../timers/date.h"
#include "../io/clipboard.h"

extern int rust_http_request(const uint8_t *method, const uint8_t *url, const uint8_t *headers, const uint8_t *body,
                             uint32_t body_len, uint8_t *out, uint32_t out_cap, int *status);
extern void rust_http_error(char *out, uint32_t cap);
extern int rust_fetch(int argc, const uint8_t *const *argv);
extern int rust_disk_store_size(const uint8_t *path);
extern int rust_disk_store_remove(const uint8_t *path);
extern int rust_prp_vault_save(const uint8_t *password, const uint8_t *secret, uint32_t len, const uint8_t *path);
extern int rust_prp_vault_load(const uint8_t *password, const uint8_t *path, uint8_t *out, uint32_t cap);
extern int rust_music_open(const uint8_t *path, uint32_t volume);
extern int rust_music_pump(void);
extern void rust_music_pause(int paused);
extern void rust_music_seek(uint32_t ms);
extern void rust_music_set_volume(uint32_t volume);
extern uint32_t rust_music_pos_ms(void);
extern uint32_t rust_music_len_ms(void);
extern void rust_music_close(void);
extern int rust_mp4_play_file_ex(const char *args, const char *unused);

#define YTM_BASE      "http://10.0.2.2:8787"
#define VAULT_FILE    "/disk/ytm.vlt"
#define FAV_FILE      "/disk/ytmfav.txt"
#define HIST_FILE     "/disk/ytmhist.txt"
#define SET_FILE      "/disk/ytmset.txt"
#define QRY_FILE      "/disk/ytmq.txt"
#define STAGE_FILE    "/disk/ytm.m4a"
#define VIDEO_FILE    "/disk/ytmv.mp4"
#define MAX_RES       30
#define MAX_FAV       400
#define MAX_HIST      150
#define MAX_Q         60
#define MAX_PL        8
#define MAX_PLT       80
#define MAX_SH        12
#define RESP_MAX      (24 * 1024)
#define FILEBUF       (64 * 1024)

typedef struct { char id[12]; uint32_t secs; char artist[40]; char title[72]; uint32_t plays, when; } track_t;
enum { V_RES, V_QUEUE, V_FAV, V_HIST, V_PL, NVIEWS };

static track_t g_res[MAX_RES], g_fav[MAX_FAV], g_hist[MAX_HIST], g_queue[MAX_Q];
static int  g_nres, g_nfav, g_nhist, g_nq;
static int  g_view, g_sel[NVIEWS], g_top[NVIEWS];
static char g_query[120], g_last_query[120];
static char g_sh[MAX_SH][80];
static int  g_nsh, g_shpos;
static bool g_searching;
static char g_base[96];
static char g_user[80];
static char g_status[140];
static uint8_t g_status_attr;
static char g_resp[RESP_MAX];
static char g_filebuf[FILEBUF];

static char g_plname[MAX_PL][28];
static int  g_plcount[MAX_PL];
static bool g_plused[MAX_PL];

static struct { int vol, repeat, vq; bool shuffle, autoplay, history; char helper[96]; } S = { 80, 0, 240, false, true, true, "" };

static track_t g_now;
static bool g_has_now, g_open, g_paused;
static int  g_vol = 80;
static int  g_ctx_view = -1, g_ctx_idx;
static bool g_dirty;
static int  g_sleep_min;
static uint64_t g_sleep_deadline;

// colours (VGA palette indexes)
#define A_BASE  fs_attr(7, 0)
#define A_TITLE fs_attr(0, 11)
#define A_HEAD  fs_attr(11, 0)
#define A_DIM   fs_attr(8, 0)
#define A_SEL   fs_attr(15, 1)
#define A_OK    fs_attr(10, 0)
#define A_WARN  fs_attr(14, 0)
#define A_ERR   fs_attr(12, 0)
#define A_HI    fs_attr(15, 0)
#define A_INPUT fs_attr(15, 8)

static void status(uint8_t attr, const char *text)
{
    strncpy(g_status, text, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = '\0';
    g_status_attr = attr;
    g_dirty = true;
}

static void copy(char *dst, size_t cap, const char *src, size_t n)
{
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static uint32_t rnd(uint32_t n)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    lo ^= lo >> 7; lo ^= lo << 3;
    return n ? lo % n : 0;
}

// ---- http ----------------------------------------------------------------------------------------------

static int http_get(const char *path, int *st)
{
    char url[360];
    snprintf(url, sizeof(url), "%s%s", g_base, path);
    int n = rust_http_request((const uint8_t *)"GET", (const uint8_t *)url, (const uint8_t *)"User-Agent: RadiumOS-ytm/2", NULL, 0,
                              (uint8_t *)g_resp, RESP_MAX - 1, st);
    if (n < 0) { char why[100]; rust_http_error(why, sizeof(why)); status(A_ERR, why); g_resp[0] = '\0'; return -1; }
    g_resp[n] = '\0';
    return n;
}

static void url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *in && n + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out[n++] = (char)c;
        else { out[n++] = '%'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 15]; }
    }
    out[n] = '\0';
}

static bool helper_ping(void)
{
    int st = 0;
    return http_get("/ping", &st) > 0 && st == 200 && strncmp(g_resp, "ytm-helper", 10) == 0;
}

// ---- track lists: "id|seconds|artist|title" lines, saved ones carry "\tplays\twhen" after the title ------------

static const char *find_bar(const char *p, const char *end)
{
    for (; p < end; p++) if (*p == '|') return p;
    return NULL;
}

static int parse_text(const char *text, track_t *out, int max, bool extras)
{
    int n = 0;
    for (const char *p = text; *p && n < max; ) {
        const char *end = p;
        while (*end && *end != '\n') end++;
        const char *f1 = find_bar(p, end);
        const char *f2 = f1 ? find_bar(f1 + 1, end) : NULL;
        const char *f3 = f2 ? find_bar(f2 + 1, end) : NULL;
        if (f3 && f1 - p == 11) {
            track_t *t = &out[n++];
            memset(t, 0, sizeof(*t));
            copy(t->id, sizeof(t->id), p, 11);
            t->secs = (uint32_t)atoi(f1 + 1);
            copy(t->artist, sizeof(t->artist), f2 + 1, (size_t)(f3 - f2 - 1));
            const char *tab = f3 + 1;
            while (tab < end && *tab != '\t') tab++;
            copy(t->title, sizeof(t->title), f3 + 1, (size_t)(tab - f3 - 1));
            for (char *c = t->title; *c; c++) if (*c == '\t') *c = ' ';
            if (extras && tab < end) {
                t->plays = (uint32_t)atoi(tab + 1);
                const char *tab2 = tab + 1;
                while (tab2 < end && *tab2 != '\t') tab2++;
                if (tab2 < end) t->when = (uint32_t)atoi(tab2 + 1);
            }
        }
        p = *end ? end + 1 : end;
    }
    return n;
}

static int parse_tracks(track_t *out, int max) { return parse_text(g_resp, out, max, false); }

static int load_tracks(const char *file, track_t *out, int max)
{
    int size = avfs_get_filesize(file);
    if (size <= 0 || size >= FILEBUF || avfs_get_content(file, g_filebuf, FILEBUF) != 0) return 0;
    g_filebuf[size] = '\0';
    return parse_text(g_filebuf, out, max, true);
}

static void save_tracks(const char *file, const char *header, const track_t *list, int n)
{
    size_t used = 0;
    g_filebuf[0] = '\0';
    if (header) snprintf(g_filebuf, FILEBUF, "%s\n", header);
    used = strlen(g_filebuf);
    for (int i = 0; i < n && used + 220 < FILEBUF; i++) {
        snprintf(g_filebuf + used, FILEBUF - used, "%s|%u|%s|%s\t%u\t%u\n", list[i].id, list[i].secs, list[i].artist,
                 list[i].title, list[i].plays, list[i].when);
        used += strlen(g_filebuf + used);
    }
    if (n == 0 && !header) { if (avfs_file_exists(file)) avfs_remove_file(file); return; }
    avfs_save_file(file, g_filebuf, (uint32_t)strlen(g_filebuf), true);
}

static void load_favs(void) { g_nfav = load_tracks(FAV_FILE, g_fav, MAX_FAV); }
static void save_favs(void) { save_tracks(FAV_FILE, NULL, g_fav, g_nfav); }
static void load_hist(void) { g_nhist = load_tracks(HIST_FILE, g_hist, MAX_HIST); }
static void save_hist(void) { save_tracks(HIST_FILE, NULL, g_hist, g_nhist); }

static int fav_index(const char *id)
{
    for (int i = 0; i < g_nfav; i++) if (strcmp(g_fav[i].id, id) == 0) return i;
    return -1;
}

static void remove_at(track_t *list, int *n, int i)
{
    if (i < 0 || i >= *n) return;
    memmove(&list[i], &list[i + 1], sizeof(track_t) * (size_t)(*n - i - 1));
    (*n)--;
}

// Records a play: the track moves to the top of the history with its count raised.
static void record_history(const track_t *t)
{
    if (!S.history) return;
    uint32_t plays = 1;
    for (int i = 0; i < g_nhist; i++) {
        if (strcmp(g_hist[i].id, t->id) == 0) { plays = g_hist[i].plays + 1; remove_at(g_hist, &g_nhist, i); break; }
    }
    if (g_nhist >= MAX_HIST) g_nhist = MAX_HIST - 1;
    memmove(&g_hist[1], &g_hist[0], sizeof(track_t) * (size_t)g_nhist);
    g_hist[0] = *t;
    g_hist[0].plays = plays;
    g_hist[0].when = get_unix_timestamp();
    g_nhist++;
    save_hist();
}

static void ago_text(uint32_t when, char *out)
{
    uint32_t now = get_unix_timestamp();
    out[0] = '\0';
    if (!when || now < when) return;
    uint32_t d = now - when;
    if (d < 90) snprintf(out, 14, "just now");
    else if (d < 5400) snprintf(out, 14, "%um ago", d / 60);
    else if (d < 129600) snprintf(out, 14, "%uh ago", d / 3600);
    else snprintf(out, 14, "%ud ago", d / 86400);
}

// ---- settings and recent searches ------------------------------------------------------------------------

static void load_settings(void)
{
    int size = avfs_get_filesize(SET_FILE);
    if (size <= 0 || size >= FILEBUF || avfs_get_content(SET_FILE, g_filebuf, FILEBUF) != 0) return;
    g_filebuf[size] = '\0';
    for (char *p = g_filebuf; *p; ) {
        char *end = p;
        while (*end && *end != '\n') end++;
        char saved = *end;
        *end = '\0';
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = '\0';
            const char *v = eq + 1;
            int n = atoi(v);
            if (strcmp(p, "volume") == 0) S.vol = n < 0 ? 0 : (n > 200 ? 200 : n);
            else if (strcmp(p, "repeat") == 0) S.repeat = n < 0 || n > 2 ? 0 : n;
            else if (strcmp(p, "shuffle") == 0) S.shuffle = n != 0;
            else if (strcmp(p, "autoplay") == 0) S.autoplay = n != 0;
            else if (strcmp(p, "history") == 0) S.history = n != 0;
            else if (strcmp(p, "videoq") == 0) S.vq = (n == 144 || n == 240 || n == 360 || n == 480) ? n : 240;
            else if (strcmp(p, "helper") == 0) copy(S.helper, sizeof(S.helper), v, strlen(v));
        }
        if (!saved) break;
        p = end + 1;
    }
    g_vol = S.vol;
}

static void save_settings(void)
{
    snprintf(g_filebuf, FILEBUF, "volume=%d\nrepeat=%d\nshuffle=%d\nautoplay=%d\nhistory=%d\nvideoq=%d\nhelper=%s\n",
             S.vol, S.repeat, S.shuffle, S.autoplay, S.history, S.vq, S.helper);
    avfs_save_file(SET_FILE, g_filebuf, (uint32_t)strlen(g_filebuf), true);
}

static void load_searches(void)
{
    g_nsh = 0;
    int size = avfs_get_filesize(QRY_FILE);
    if (size <= 0 || size >= FILEBUF || avfs_get_content(QRY_FILE, g_filebuf, FILEBUF) != 0) return;
    g_filebuf[size] = '\0';
    for (char *p = g_filebuf; *p && g_nsh < MAX_SH; ) {
        char *end = p;
        while (*end && *end != '\n') end++;
        if (end > p) copy(g_sh[g_nsh++], sizeof(g_sh[0]), p, (size_t)(end - p));
        p = *end ? end + 1 : end;
    }
}

static void remember_search(const char *text)
{
    for (int i = 0; i < g_nsh; i++) {
        if (strcmp(g_sh[i], text) == 0) { memmove(&g_sh[i], &g_sh[i + 1], sizeof(g_sh[0]) * (size_t)(g_nsh - i - 1)); g_nsh--; break; }
    }
    if (g_nsh >= MAX_SH) g_nsh = MAX_SH - 1;
    memmove(&g_sh[1], &g_sh[0], sizeof(g_sh[0]) * (size_t)g_nsh);
    copy(g_sh[0], sizeof(g_sh[0]), text, strlen(text));
    g_nsh++;
    g_filebuf[0] = '\0';
    for (int i = 0; i < g_nsh; i++) { strcat(g_filebuf, g_sh[i]); strcat(g_filebuf, "\n"); }
    avfs_save_file(QRY_FILE, g_filebuf, (uint32_t)strlen(g_filebuf), true);
}

// ---- search ----------------------------------------------------------------------------------------------

static void do_search(const char *text)
{
    char enc[300], path[360];
    int st = 0;
    if (!text[0]) return;
    status(A_WARN, "Searching ...");
    g_dirty = true;
    url_encode(text, enc, sizeof(enc));
    snprintf(path, sizeof(path), "/search?q=%s&n=25", enc);
    if (http_get(path, &st) < 0) {
        char why[140];
        snprintf(why, sizeof(why), "%s (is tools/ytm_helper.py running on the host?)", g_status);
        status(A_ERR, why);
        return;
    }
    if (st != 200) { snprintf(g_status, sizeof(g_status), "Search failed (HTTP %d): %s", st, g_resp); g_status_attr = A_ERR; return; }
    g_nres = parse_tracks(g_res, MAX_RES);
    g_view = V_RES; g_sel[V_RES] = 0; g_top[V_RES] = 0;
    strncpy(g_last_query, text, sizeof(g_last_query) - 1);
    remember_search(text);
    snprintf(g_status, sizeof(g_status), "%d result%s for \"%s\"", g_nres, g_nres == 1 ? "" : "s", text);
    g_status_attr = g_nres ? A_OK : A_WARN;
}

// ---- modal dialogs -----------------------------------------------------------------------------------------

static void draw_all(void);

// A modal single-line prompt; out may hold a starting text when keep is true. secret shows stars. False on Esc.
static bool prompt_ex(const char *title, const char *label, char *out, size_t cap, bool secret, bool keep)
{
    size_t len = keep ? strlen(out) : 0;
    if (!keep) out[0] = '\0';
    bool redraw = true;
    for (;;) {
        if (redraw) {
            int W = fs_cols(), H = fs_rows(), w = 84, h = 8;
            if (w > W - 2) w = W - 2;
            int bx = (W - w) / 2, by = (H - h) / 2;
            fs_fill(bx, by, w, h, ' ', A_BASE);
            fs_box(bx, by, w, h, A_HEAD, title);
            fs_text(bx + 3, by + 2, label, A_BASE);
            fs_fill(bx + 3, by + 4, w - 6, 1, ' ', A_INPUT);
            int shown = 0;
            for (size_t i = (len > (size_t)(w - 10) ? len - (size_t)(w - 10) : 0); i < len; i++, shown++)
                fs_put(bx + 4 + shown, by + 4, secret ? '*' : out[i], A_INPUT);
            fs_put(bx + 4 + shown, by + 4, '_', fs_attr(14, 8));
            fs_text(bx + 3, by + 6, "Enter accept   Ctrl+V paste from the host clipboard   Esc cancel", A_DIM);
            fs_present();
            redraw = false;
        }
        fs_event_t ev;
        if (!fs_get_event(&ev)) { __asm__ volatile("pause"); continue; }
        redraw = true;
        if (ev.sc == 0x01) return false;
        if (ev.sc == 0x1C) return true;
        if (ev.sc == 0x0E) { if (len) out[--len] = '\0'; continue; }
        if (ev.ctrl && ev.sc == 0x2F) {
            static char host[CLIPBOARD_HOST_MAX + 1];
            int n = clipboard_from_host(host, sizeof(host));
            for (int i = 0; i < n && len + 1 < cap; i++)
                if ((unsigned char)host[i] >= 0x20 && (unsigned char)host[i] < 0x7F) out[len++] = host[i];
            out[len] = '\0';
            memset(host, 0, sizeof(host));
            continue;
        }
        if (ev.ch >= 0x20 && ev.ch < 0x7F && len + 1 < cap) { out[len++] = ev.ch; out[len] = '\0'; }
    }
}

static bool prompt(const char *title, const char *label, char *out, size_t cap, bool secret)
{
    return prompt_ex(title, label, out, cap, secret, false);
}

// A list to pick from; returns the chosen index, or -1 on Esc.
static int choose(const char *title, const char *const *labels, int n)
{
    int sel = 0;
    for (;;) {
        int W = fs_cols(), H = fs_rows(), w = 60, h = n + 5;
        if (w > W - 2) w = W - 2;
        int bx = (W - w) / 2, by = (H - h) / 2;
        fs_fill(bx, by, w, h, ' ', A_BASE);
        fs_box(bx, by, w, h, A_HEAD, title);
        for (int i = 0; i < n; i++) {
            fs_fill(bx + 2, by + 2 + i, w - 4, 1, ' ', i == sel ? A_SEL : A_BASE);
            fs_text(bx + 3, by + 2 + i, labels[i], i == sel ? A_SEL : A_BASE);
        }
        fs_text(bx + 3, by + h - 2, "Up/Down  Enter choose  Esc cancel", A_DIM);
        fs_present();
        fs_event_t ev;
        while (!fs_get_event(&ev)) __asm__ volatile("pause");
        if (ev.sc == 0x01) return -1;
        if (ev.sc == 0x1C) return sel;
        if (ev.sc == 0x48 && sel > 0) sel--;
        if (ev.sc == 0x50 && sel + 1 < n) sel++;
    }
}

static bool confirm(const char *question)
{
    static const char *const yn[] = { "No", "Yes" };
    return choose(question, yn, 2) == 1;
}

static void show_help(void)
{
    static const char *const LINES[] = {
        "Playing",
        "  Enter play / open        Space pause        Left/Right seek 10 s        + - volume",
        "  n next   p previous      z shuffle          r repeat: off / track / list",
        "  t sleep timer (off, 15, 30, 60 min)         v watch the selected track as a video",
        "Lists",
        "  Tab views: Results, Queue, Liked, History, Playlists      / search (Up/Down: earlier searches)",
        "  a add to the queue       d drop / delete    c clear the queue     f or l like / unlike",
        "  S save the queue as a playlist              P add the selected track to a playlist",
        "Other",
        "  s settings               u sign in          ? this help           q quit",
        "",
        "Liked songs, history, playlists and settings are saved on /disk and survive reboots.",
    };
    int n = (int)(sizeof(LINES) / sizeof(LINES[0]));
    int W = fs_cols(), H = fs_rows(), w = 94, h = n + 4;
    if (w > W - 2) w = W - 2;
    int bx = (W - w) / 2, by = (H - h) / 2;
    fs_fill(bx, by, w, h, ' ', A_BASE);
    fs_box(bx, by, w, h, A_HEAD, "Keys");
    for (int i = 0; i < n; i++) {
        bool head = i == 0 || i == 4 || i == 8;
        fs_text(bx + 3, by + 2 + i, LINES[i], head ? A_HEAD : (i == n - 1 ? A_DIM : A_BASE));
    }
    fs_text(bx + 3, by + h - 1, " press any key ", A_DIM);
    fs_present();
    fs_event_t ev;
    while (!fs_get_event(&ev)) __asm__ volatile("pause");
}

static void settings_menu(void)
{
    static const char *const REPEAT[] = { "off", "this track", "the list" };
    int sel = 0;
    enum { N = 10 };
    for (;;) {
        char val[N][100];
        snprintf(val[0], sizeof(val[0]), "%d%%", S.vol);
        snprintf(val[1], sizeof(val[1]), "%s", REPEAT[S.repeat]);
        snprintf(val[2], sizeof(val[2]), "%s", S.shuffle ? "on" : "off");
        snprintf(val[3], sizeof(val[3]), "%s", S.autoplay ? "on" : "off");
        snprintf(val[4], sizeof(val[4]), "%dp", S.vq);
        snprintf(val[5], sizeof(val[5]), "%s", S.history ? "on" : "off");
        snprintf(val[6], sizeof(val[6]), "%s", S.helper[0] ? S.helper : "(default, or $YTM_API)");
        snprintf(val[7], sizeof(val[7]), "%d entr%s", g_nhist, g_nhist == 1 ? "y" : "ies");
        snprintf(val[8], sizeof(val[8]), "%d song%s", g_nfav, g_nfav == 1 ? "" : "s");
        val[9][0] = '\0';
        static const char *const NAMES[N] = { "Starting volume", "Repeat", "Shuffle", "Play the next track on its own", "Video quality",
                                              "Record history", "Helper address", "Clear history", "Clear liked songs", "Close" };
        int W = fs_cols(), H = fs_rows(), w = 84, h = N + 6;
        if (w > W - 2) w = W - 2;
        int bx = (W - w) / 2, by = (H - h) / 2;
        fs_fill(bx, by, w, h, ' ', A_BASE);
        fs_box(bx, by, w, h, A_HEAD, "Settings");
        for (int i = 0; i < N; i++) {
            uint8_t a = i == sel ? A_SEL : A_BASE;
            fs_fill(bx + 2, by + 2 + i, w - 4, 1, ' ', a);
            fs_text(bx + 3, by + 2 + i, NAMES[i], a);
            fs_text(bx + 38, by + 2 + i, val[i], a);
        }
        fs_text(bx + 3, by + h - 3, "Up/Down choose   Left/Right or Enter change   Esc close (saved)", A_DIM);
        fs_text(bx + 3, by + h - 2, "Volume, repeat and shuffle apply to the next track as well as the one playing.", A_DIM);
        fs_present();
        fs_event_t ev;
        while (!fs_get_event(&ev)) __asm__ volatile("pause");
        int dir = ev.sc == 0x4D ? 1 : (ev.sc == 0x4B ? -1 : 0);
        if (ev.sc == 0x01 || ev.ch == 's') break;
        if (ev.sc == 0x48 && sel > 0) sel--;
        else if (ev.sc == 0x50 && sel + 1 < N) sel++;
        else if (dir || ev.sc == 0x1C) {
            int d = dir ? dir : 1;
            switch (sel) {
            case 0: S.vol += 10 * d; if (S.vol < 0) S.vol = 0; if (S.vol > 200) S.vol = 200; g_vol = S.vol; rust_music_set_volume((uint32_t)g_vol); break;
            case 1: S.repeat = (S.repeat + d + 3) % 3; break;
            case 2: S.shuffle = !S.shuffle; break;
            case 3: S.autoplay = !S.autoplay; break;
            case 4: { static const int Q[] = { 144, 240, 360, 480 }; int k = 0; while (k < 3 && Q[k] != S.vq) k++; k = (k + d + 4) % 4; S.vq = Q[k]; break; }
            case 5: S.history = !S.history; break;
            case 6: {
                char addr[96];
                copy(addr, sizeof(addr), S.helper, strlen(S.helper));
                if (prompt_ex("Helper address", "http://host:port of tools/ytm_helper.py (empty: the default):", addr, sizeof(addr), false, true)) {
                    if (addr[0] && strncmp(addr, "http", 4) != 0) status(A_ERR, "The address must start with http");
                    else { copy(S.helper, sizeof(S.helper), addr, strlen(addr)); if (S.helper[0]) copy(g_base, sizeof(g_base), S.helper, strlen(S.helper)); }
                }
                break;
            }
            case 7: if (g_nhist && confirm("Clear the whole history?")) { g_nhist = 0; save_hist(); } break;
            case 8: if (g_nfav && confirm("Remove every liked song?")) { g_nfav = 0; save_favs(); } break;
            default: goto done;
            }
        }
    }
done:
    save_settings();
    status(A_OK, "Settings saved.");
}

// ---- account -------------------------------------------------------------------------------------------

// Asks for a name, a password and a passphrase and seals name + password into the vault. True when saved.
static bool do_login(void)
{
    char name[100], pass[100], phrase[100], again[100], secret[220];
    bool ok = false;
    if (!avfs_is_directory("/disk")) { status(A_ERR, "Sign in needs /disk (it is not mounted)."); return false; }
    if (!prompt("Sign in - account", "Google account (email address):", name, sizeof(name), false) || name[0] == '\0') return false;
    if (!prompt("Sign in - password", "Account password (hidden, kept only in the PRP vault):", pass, sizeof(pass), true) || pass[0] == '\0') goto done;
    for (;;) {
        if (!prompt("Sign in - passphrase", "Choose a passphrase that encrypts them on the disk (6+ characters):", phrase, sizeof(phrase), true)) goto done;
        if (strlen(phrase) < 6) { status(A_ERR, "Passphrase too short."); continue; }
        if (!prompt("Sign in - passphrase", "Type the passphrase again:", again, sizeof(again), true)) goto done;
        if (strcmp(phrase, again) != 0) { status(A_ERR, "The passphrases did not match."); continue; }
        break;
    }
    snprintf(secret, sizeof(secret), "%s\n%s", name, pass);
    if (rust_prp_vault_save((const uint8_t *)phrase, (const uint8_t *)secret, (uint32_t)strlen(secret), (const uint8_t *)VAULT_FILE) == 0) {
        copy(g_user, sizeof(g_user), name, strlen(name));
        status(A_OK, "Account saved, sealed with your passphrase.");
        ok = true;
    } else {
        status(A_ERR, "Could not save the account.");
    }
done:
    memset(pass, 0, sizeof(pass)); memset(phrase, 0, sizeof(phrase)); memset(again, 0, sizeof(again)); memset(secret, 0, sizeof(secret));
    return ok;
}

// Unlocks the saved account (three tries); the password is wiped right away, only the name is kept for display.
static void do_unlock(void)
{
    char phrase[100], secret[240];
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!prompt("YouTube Music account", "Passphrase for the saved account (Esc: continue without it):", phrase, sizeof(phrase), true)) return;
        int n = rust_prp_vault_load((const uint8_t *)phrase, (const uint8_t *)VAULT_FILE, (uint8_t *)secret, sizeof(secret) - 1);
        memset(phrase, 0, sizeof(phrase));
        if (n > 0) {
            secret[n] = '\0';
            char *nl = strchr(secret, '\n');
            size_t len = nl ? (size_t)(nl - secret) : strlen(secret);
            copy(g_user, sizeof(g_user), secret, len);
            memset(secret, 0, sizeof(secret));
            status(A_OK, "Signed in (account unlocked).");
            return;
        }
        status(A_ERR, n == -2 ? "Wrong passphrase." : "The saved account file is damaged (ytm logout removes it).");
        if (n != -2) return;
    }
}

// ---- playlists -----------------------------------------------------------------------------------------

static void pl_path(int slot, char *out, size_t cap) { snprintf(out, cap, "/disk/ytp%d.txt", slot); }

// Reads the name and the track count of every saved playlist.
static void pl_scan(void)
{
    for (int i = 0; i < MAX_PL; i++) {
        char path[32];
        pl_path(i, path, sizeof(path));
        g_plused[i] = false; g_plcount[i] = 0; g_plname[i][0] = '\0';
        int size = avfs_get_filesize(path);
        if (size <= 0 || size >= FILEBUF || avfs_get_content(path, g_filebuf, FILEBUF) != 0) continue;
        g_filebuf[size] = '\0';
        g_plused[i] = true;
        if (g_filebuf[0] == '#') {
            const char *nl = strchr(g_filebuf, '\n');
            copy(g_plname[i], sizeof(g_plname[0]), g_filebuf + 1, nl ? (size_t)(nl - g_filebuf - 1) : strlen(g_filebuf + 1));
        } else snprintf(g_plname[i], sizeof(g_plname[0]), "Playlist %d", i + 1);
        for (const char *p = g_filebuf; *p; p++) if (*p == '\n' && p[1] && p[1] != '#') g_plcount[i]++;
        if (g_filebuf[0] != '#') g_plcount[i]++;
    }
}

static int pl_count(void) { int n = 0; for (int i = 0; i < MAX_PL; i++) if (g_plused[i]) n++; return n; }

// Slot of the k-th used playlist.
static int pl_slot(int k)
{
    for (int i = 0; i < MAX_PL; i++) if (g_plused[i] && k-- == 0) return i;
    return -1;
}

static int pl_read(int slot, track_t *out, int max)
{
    char path[32];
    pl_path(slot, path, sizeof(path));
    return load_tracks(path, out, max);
}

static void pl_write(int slot, const char *name, const track_t *list, int n)
{
    char path[32], header[40];
    pl_path(slot, path, sizeof(path));
    snprintf(header, sizeof(header), "#%s", name);
    save_tracks(path, header, list, n);
}

// Slot for a playlist called name: the same name, else a free slot, else -1.
static int pl_slot_for(const char *name)
{
    for (int i = 0; i < MAX_PL; i++) if (g_plused[i] && strcmp(g_plname[i], name) == 0) return i;
    for (int i = 0; i < MAX_PL; i++) if (!g_plused[i]) return i;
    return -1;
}

// ---- playback ------------------------------------------------------------------------------------------

static void stop_playback(void)
{
    rust_music_close();
    g_open = false;
    g_paused = false;
    if (rust_disk_store_size((const uint8_t *)STAGE_FILE) >= 0) rust_disk_store_remove((const uint8_t *)STAGE_FILE);
}

static bool fetch_to(const char *path, const char *file)
{
    char url[260];
    snprintf(url, sizeof(url), "%s%s", g_base, path);
    const uint8_t *args[] = {
        (const uint8_t *)"fetch", (const uint8_t *)"-f", (const uint8_t *)"-s", (const uint8_t *)"--overwrite",
        (const uint8_t *)"--timeout", (const uint8_t *)"300", (const uint8_t *)"-o", (const uint8_t *)file, (const uint8_t *)url,
    };
    if (rust_fetch((int)(sizeof(args) / sizeof(args[0])), args) != 0 || rust_disk_store_size((const uint8_t *)file) <= 0) {
        rust_disk_store_remove((const uint8_t *)file);
        return false;
    }
    return true;
}

static bool play(const track_t *t)
{
    char path[80];
    status(A_WARN, "Downloading (the host fetches it with yt-dlp, this can take a few seconds) ...");
    g_now = *t; g_has_now = true;
    stop_playback();
    draw_all();
    fs_present();

    snprintf(path, sizeof(path), "/audio?id=%s", t->id);
    if (!fetch_to(path, STAGE_FILE)) {
        status(A_ERR, "Download failed. Is tools/ytm_helper.py running, and is yt-dlp up to date? (see the helper's window)");
        return false;
    }
    int rc = rust_music_open((const uint8_t *)STAGE_FILE, (uint32_t)g_vol);
    if (rc != 0) {
        status(A_ERR, rc == -4 ? "No sound card found (the AC97 codec is missing)." :
                      rc == -2 ? "That download is not an AAC audio file." : "The audio could not be opened.");
        stop_playback();
        return false;
    }
    g_open = true; g_paused = false;
    record_history(t);
    snprintf(g_status, sizeof(g_status), "Playing: %s", t->title);
    g_status_attr = A_OK;
    return true;
}

// Downloads a small H.264/AAC mp4 and plays it with the mp4 player, which owns the whole screen until it ends.
static void play_video(const track_t *t)
{
    if (terminal_vt_active()) { status(A_ERR, "Video needs the whole screen: log out of the desktop and run ytm there."); return; }
    char path[80], args[120];
    status(A_WARN, "Downloading the video (the host fetches it with yt-dlp) ...");
    draw_all();
    g_now = *t; g_has_now = true;
    stop_playback();
    snprintf(path, sizeof(path), "/video?id=%s&q=%d", t->id, S.vq);
    if (!fetch_to(path, VIDEO_FILE)) {
        status(A_ERR, "Video download failed (the helper needs yt-dlp and ffmpeg; try a lower quality in the settings).");
        return;
    }
    record_history(t);
    fs_end();
    snprintf(args, sizeof(args), "--volume=%d %s", g_vol, VIDEO_FILE);
    rust_mp4_play_file_ex(args, "");
    rust_disk_store_remove((const uint8_t *)VIDEO_FILE);
    if (!fs_begin()) return;
    status(A_DIM, "Video finished.");
    g_dirty = true;
}

static int list_count(int view)
{
    return view == V_RES ? g_nres : (view == V_QUEUE ? g_nq : (view == V_FAV ? g_nfav : (view == V_HIST ? g_nhist : pl_count())));
}

static const track_t *list_at(int view, int idx)
{
    if (view == V_PL || idx < 0 || idx >= list_count(view)) return NULL;
    return view == V_RES ? &g_res[idx] : (view == V_FAV ? &g_fav[idx] : (view == V_HIST ? &g_hist[idx] : &g_queue[idx]));
}

static void play_from(int view, int idx)
{
    const track_t *t = list_at(view, idx);
    if (!t) return;
    track_t copy_of = *t;
    if (view == V_QUEUE) {       // a played queue entry leaves the queue
        remove_at(g_queue, &g_nq, idx);
        g_ctx_view = -1;
    } else if (view == V_HIST) {
        g_ctx_view = -1;         // playing moves the entry to the top, so the list cannot be walked
    } else {
        g_ctx_view = view; g_ctx_idx = idx;
    }
    play(&copy_of);
}

static void next_track(bool from_end)
{
    if (from_end && S.repeat == 1 && g_has_now) { track_t again = g_now; play(&again); return; }
    if (from_end && !S.autoplay) { stop_playback(); status(A_DIM, "Track finished."); return; }
    if (g_nq > 0) { play_from(V_QUEUE, S.shuffle ? (int)rnd((uint32_t)g_nq) : 0); return; }
    if (g_ctx_view >= 0) {
        int n = list_count(g_ctx_view);
        if (S.shuffle && n > 1) {
            int pick;
            do { pick = (int)rnd((uint32_t)n); } while (pick == g_ctx_idx);
            play_from(g_ctx_view, pick);
            return;
        }
        if (g_ctx_idx + 1 < n) { play_from(g_ctx_view, g_ctx_idx + 1); return; }
        if (S.repeat == 2 && n > 0) { play_from(g_ctx_view, 0); return; }
    }
    stop_playback();
    status(A_DIM, "End of the list.");
}

static void prev_track(void)
{
    if (!g_open) return;
    if (rust_music_pos_ms() > 5000 || g_ctx_view < 0 || !list_at(g_ctx_view, g_ctx_idx - 1)) { rust_music_seek(0); return; }
    play_from(g_ctx_view, g_ctx_idx - 1);
}

static void load_playlist_to_queue(int slot, bool start)
{
    g_nq = pl_read(slot, g_queue, MAX_Q);
    snprintf(g_status, sizeof(g_status), "Playlist \"%s\": %d track%s queued.", g_plname[slot], g_nq, g_nq == 1 ? "" : "s");
    g_status_attr = g_nq ? A_OK : A_WARN;
    g_dirty = true;
    if (start && g_nq) { g_view = V_QUEUE; g_sel[V_QUEUE] = 0; play_from(V_QUEUE, 0); }
}

static void save_queue_as_playlist(void)
{
    const track_t *src = g_queue;
    int n = g_nq;
    if (n == 0 && g_ctx_view >= 0 && g_ctx_view != V_PL) {      // no queue: the list being played
        n = list_count(g_ctx_view);
        src = g_ctx_view == V_RES ? g_res : (g_ctx_view == V_FAV ? g_fav : g_hist);
    }
    if (n == 0) { status(A_WARN, "Nothing to save: the queue is empty (add tracks with a)."); return; }
    char name[28];
    copy(name, sizeof(name), g_last_query, strlen(g_last_query));
    if (!prompt_ex("Save playlist", "Name for the playlist (an existing name is replaced):", name, sizeof(name), false, true) || !name[0]) return;
    pl_scan();
    int slot = pl_slot_for(name);
    if (slot < 0) { status(A_ERR, "All 8 playlist slots are used (delete one in the Playlists view with d)."); return; }
    pl_write(slot, name, src, n > MAX_PLT ? MAX_PLT : n);
    pl_scan();
    snprintf(g_status, sizeof(g_status), "Saved %d track%s as \"%s\".", n, n == 1 ? "" : "s", name);
    g_status_attr = A_OK;
}

static void add_to_playlist(const track_t *t)
{
    pl_scan();
    const char *labels[MAX_PL + 1];
    int slots[MAX_PL + 1], n = 0;
    for (int i = 0; i < MAX_PL; i++) if (g_plused[i]) { labels[n] = g_plname[i]; slots[n++] = i; }
    labels[n] = "New playlist ...";
    int pick = choose("Add to playlist", labels, n + 1);
    if (pick < 0) return;
    int slot;
    char name[28];
    if (pick == n) {
        name[0] = '\0';
        if (!prompt("New playlist", "Name for the playlist:", name, sizeof(name), false) || !name[0]) return;
        slot = pl_slot_for(name);
        if (slot < 0) { status(A_ERR, "All 8 playlist slots are used."); return; }
    } else { slot = slots[pick]; copy(name, sizeof(name), g_plname[slot], strlen(g_plname[slot])); }
    static track_t tmp[MAX_PLT];
    int count = g_plused[slot] ? pl_read(slot, tmp, MAX_PLT) : 0;
    for (int i = 0; i < count; i++) if (strcmp(tmp[i].id, t->id) == 0) { status(A_WARN, "That track is already in the playlist."); return; }
    if (count >= MAX_PLT) { status(A_ERR, "That playlist is full."); return; }
    tmp[count++] = *t;
    pl_write(slot, name, tmp, count);
    pl_scan();
    snprintf(g_status, sizeof(g_status), "Added to \"%s\" (%d track%s).", name, count, count == 1 ? "" : "s");
    g_status_attr = A_OK;
}

// ---- screen --------------------------------------------------------------------------------------------

static void put_trunc(int x, int y, const char *s, int width, uint8_t attr)
{
    if (width <= 0) return;
    int len = (int)strlen(s);
    for (int i = 0; i < width; i++) fs_put(x + i, y, i < len ? s[i] : ' ', attr);
    if (len > width && width > 2) { fs_put(x + width - 2, y, '.', attr); fs_put(x + width - 1, y, '.', attr); }
}

static void clock_text(uint32_t secs, char *out)
{
    snprintf(out, 12, "%u:%02u", secs / 60, secs % 60);
}

static void draw_player(void)
{
    static const char *const REP[] = { "off", "track", "list" };
    int W = fs_cols(), H = fs_rows(), y = H - 8;
    char a[16], b[16], line[220];
    fs_fill(0, y, W, 8, ' ', A_BASE);
    fs_fill(0, y, W, 1, (char)0xC4, A_DIM);

    if (g_has_now) {
        snprintf(line, sizeof(line), "%s  -  %s", g_now.title, g_now.artist[0] ? g_now.artist : "unknown artist");
        put_trunc(2, y + 1, line, W - 4, A_HI);
        uint32_t pos = g_open ? rust_music_pos_ms() / 1000 : 0, len = g_open ? rust_music_len_ms() / 1000 : g_now.secs;
        clock_text(pos, a); clock_text(len, b);
        fs_text(2, y + 2, a, A_BASE);
        int bar_x = 9, bar_w = W - 20;
        fs_bar(bar_x, y + 2, bar_w, len ? (int)(pos * 100 / len) : 0, fs_attr(10, 0), fs_attr(8, 0));
        fs_text(bar_x + bar_w + 2, y + 2, b, A_BASE);
    } else {
        fs_text(2, y + 1, "Nothing playing. Press / to search, ? for the keys.", A_DIM);
    }
    char sleep[24] = "";
    if (g_sleep_min) snprintf(sleep, sizeof(sleep), "   sleep %dm", g_sleep_min);
    snprintf(line, sizeof(line), "%s   volume %d%%   repeat %s   shuffle %s   queue %d   liked %d%s",
             !g_has_now ? "idle" : (!g_open ? "stopped" : (g_paused ? "paused" : "playing")), g_vol, REP[S.repeat],
             S.shuffle ? "on" : "off", g_nq, g_nfav, sleep);
    fs_text(2, y + 3, line, A_HEAD);
    put_trunc(2, y + 4, g_status, W - 4, g_status_attr);
    fs_text(2, y + 6, "/ search  Enter play  Space pause  <- -> seek  + - volume  n/p track  z shuffle  r repeat  v video  Tab view  ? help", A_DIM);
    fs_text(2, y + 7, "a queue  d drop  c clear  f like  S save queue  P playlist  s settings  t sleep  u sign in  q quit", A_DIM);
}

static void draw_all(void)
{
    int W = fs_cols(), H = fs_rows();
    static const char *const TITLES[NVIEWS] = { "Results", "Queue", "Liked", "History", "Playlists" };
    fs_clear(A_BASE);

    fs_fill(0, 0, W, 1, ' ', A_TITLE);
    fs_text(1, 0, "RadiumOS Music", A_TITLE);
    char acct[120];
    snprintf(acct, sizeof(acct), "%s", g_user[0] ? g_user : "not signed in (u)");
    fs_text_right(W - 2, 0, acct, A_TITLE);

    fs_text(2, 2, g_searching ? "Search:" : "Search (/):", g_searching ? A_WARN : A_DIM);
    fs_fill(14, 2, W - 16, 1, ' ', g_searching ? A_INPUT : fs_attr(8, 0));
    fs_text(15, 2, g_searching ? g_query : g_last_query, g_searching ? A_INPUT : fs_attr(7, 8));
    if (g_searching) fs_put(15 + (int)strlen(g_query), 2, '_', fs_attr(14, 8));

    int tabx = 2;
    for (int v = 0; v < NVIEWS; v++) {
        char tab[40];
        snprintf(tab, sizeof(tab), " %s (%d) ", TITLES[v], list_count(v));
        tabx = fs_text(tabx, 4, tab, v == g_view ? A_SEL : A_DIM) + 1;
    }

    int list_y = 6, rows = H - 8 - list_y;
    int n = list_count(g_view);
    if (g_sel[g_view] >= n) g_sel[g_view] = n ? n - 1 : 0;
    if (g_sel[g_view] < g_top[g_view]) g_top[g_view] = g_sel[g_view];
    if (g_sel[g_view] >= g_top[g_view] + rows) g_top[g_view] = g_sel[g_view] - rows + 1;
    for (int r = 0; r < rows && g_top[g_view] + r < n; r++) {
        int i = g_top[g_view] + r;
        uint8_t attr_sel = A_SEL;
        char idx[8];
        snprintf(idx, sizeof(idx), "%d", i + 1);
        if (g_view == V_PL) {
            int slot = pl_slot(i);
            char cnt[24];
            uint8_t attr = i == g_sel[g_view] ? attr_sel : A_BASE;
            snprintf(cnt, sizeof(cnt), "%d track%s", slot >= 0 ? g_plcount[slot] : 0, slot >= 0 && g_plcount[slot] == 1 ? "" : "s");
            fs_fill(1, list_y + r, W - 2, 1, ' ', attr);
            fs_text(2, list_y + r, idx, attr);
            put_trunc(7, list_y + r, slot >= 0 ? g_plname[slot] : "", W - 30, attr);
            fs_text_right(W - 3, list_y + r, cnt, attr);
            continue;
        }
        const track_t *t = list_at(g_view, i);
        bool cur = g_has_now && strcmp(t->id, g_now.id) == 0;
        uint8_t attr = i == g_sel[g_view] ? attr_sel : (cur ? A_OK : A_BASE);
        char dur[12], extra[24];
        clock_text(t->secs, dur);
        extra[0] = '\0';
        if (g_view == V_HIST) { char ago[16]; ago_text(t->when, ago); snprintf(extra, sizeof(extra), "%ux  %s", t->plays, ago); }
        int extra_w = g_view == V_HIST ? 22 : 0;
        fs_fill(1, list_y + r, W - 2, 1, ' ', attr);
        fs_text(2, list_y + r, idx, attr);
        put_trunc(7, list_y + r, t->title, W - 7 - 38 - 10 - extra_w, attr);
        put_trunc(W - 38 - 6 - extra_w, list_y + r, t->artist, 32, attr);
        if (extra_w) fs_text(W - 6 - 22, list_y + r, extra, attr);
        fs_text_right(W - 3, list_y + r, t->secs ? dur : "", attr);
        if (fav_index(t->id) >= 0) fs_put(5, list_y + r, '*', i == g_sel[g_view] ? attr : fs_attr(14, 0));
    }
    if (n == 0) {
        const char *msg = g_view == V_RES ? "No results yet - press / and type something to search." :
                          g_view == V_QUEUE ? "The queue is empty: select a track and press a." :
                          g_view == V_FAV ? "No liked songs yet: press f on a track." :
                          g_view == V_HIST ? "Nothing played yet." :
                          "No playlists: press S to save the queue, or P to add a track to one.";
        fs_text(4, list_y + 1, msg, A_DIM);
    }
    draw_player();
    fs_present();
    g_dirty = false;
}

// ---- main loop -----------------------------------------------------------------------------------------

static const track_t *selected(void)
{
    return list_at(g_view, g_sel[g_view]);
}

static void handle_search_key(const fs_event_t *ev)
{
    size_t len = strlen(g_query);
    if (ev->sc == 0x01) { g_searching = false; g_query[0] = '\0'; }
    else if (ev->sc == 0x1C) { g_searching = false; do_search(g_query); }
    else if (ev->sc == 0x0E) { if (len) g_query[len - 1] = '\0'; }
    else if (ev->sc == 0x48 || ev->sc == 0x50) {                    // recall earlier searches
        if (ev->sc == 0x48) {
            if (g_shpos < g_nsh) { const char *q = g_sh[g_shpos++]; copy(g_query, sizeof(g_query), q, strlen(q)); }
        } else if (g_shpos > 1) {
            const char *q = g_sh[--g_shpos - 1];
            copy(g_query, sizeof(g_query), q, strlen(q));
        } else { g_shpos = 0; g_query[0] = '\0'; }
    } else if (ev->ctrl && ev->sc == 0x2F) {
        static char host[CLIPBOARD_HOST_MAX + 1];
        int n = clipboard_from_host(host, sizeof(host));
        for (int i = 0; i < n && len + 1 < sizeof(g_query); i++)
            if ((unsigned char)host[i] >= 0x20 && (unsigned char)host[i] < 0x7F) g_query[len++] = host[i];
        g_query[len] = '\0';
    } else if (ev->ch >= 0x20 && ev->ch < 0x7F && len + 1 < sizeof(g_query)) { g_query[len] = ev->ch; g_query[len + 1] = '\0'; }
    g_dirty = true;
}

static void move_sel(int delta)
{
    int n = list_count(g_view);
    int s = g_sel[g_view] + delta;
    if (s < 0) s = 0;
    if (s >= n) s = n ? n - 1 : 0;
    g_sel[g_view] = s;
    g_dirty = true;
}

static void toggle_like(const track_t *t)
{
    int at = fav_index(t->id);
    if (at >= 0) { remove_at(g_fav, &g_nfav, at); status(A_DIM, "Removed from liked songs."); }
    else if (g_nfav < MAX_FAV) { g_fav[g_nfav] = *t; g_fav[g_nfav].plays = 0; g_fav[g_nfav].when = get_unix_timestamp(); g_nfav++; status(A_OK, "Added to liked songs."); }
    else status(A_ERR, "The liked list is full.");
    save_favs();
}

// Returns false to quit.
static bool handle_key(const fs_event_t *ev)
{
    if (g_searching) { handle_search_key(ev); return true; }

    switch (ev->sc) {
    case 0x48: move_sel(-1); return true;                  // up
    case 0x50: move_sel(1); return true;                   // down
    case 0x49: move_sel(-10); return true;                 // page up
    case 0x51: move_sel(10); return true;                  // page down
    case 0x47: move_sel(-1000); return true;               // home
    case 0x4F: move_sel(1000); return true;                // end
    case 0x4B: case 0x4D:                                  // left / right: seek 10 s
        if (g_open) {
            uint32_t pos = rust_music_pos_ms(), len = rust_music_len_ms();
            uint32_t to = ev->sc == 0x4D ? pos + 10000 : (pos > 10000 ? pos - 10000 : 0);
            if (to + 500 < len) rust_music_seek(to);
        }
        return true;
    case 0x0F: g_view = (g_view + 1) % NVIEWS; if (g_view == V_PL) pl_scan(); g_dirty = true; return true;   // Tab
    case 0x1C:                                             // Enter
        if (g_view == V_PL) { int slot = pl_slot(g_sel[V_PL]); if (slot >= 0) load_playlist_to_queue(slot, true); }
        else if (selected()) play_from(g_view, g_sel[g_view]);
        g_dirty = true;
        return true;
    case 0x39:                                             // Space
        if (g_open) { g_paused = !g_paused; rust_music_pause(g_paused); status(A_DIM, g_paused ? "Paused." : "Playing."); }
        return true;
    case 0x01: return false;
    default: break;
    }

    switch (ev->ch) {
    case 'q': case 'Q': return false;
    case '/': g_searching = true; g_query[0] = '\0'; g_shpos = 0; g_dirty = true; break;
    case '?': case 'h': case 'H': show_help(); g_dirty = true; break;
    case 's': settings_menu(); g_dirty = true; break;
    case '+': case '=': g_vol = g_vol >= 190 ? 200 : g_vol + 10; rust_music_set_volume((uint32_t)g_vol); g_dirty = true; break;
    case '-': case '_': g_vol = g_vol <= 10 ? 0 : g_vol - 10; rust_music_set_volume((uint32_t)g_vol); g_dirty = true; break;
    case 'n': case 'N': if (g_has_now) next_track(false); g_dirty = true; break;
    case 'p': prev_track(); g_dirty = true; break;
    case 'z': case 'Z': S.shuffle = !S.shuffle; save_settings(); status(A_DIM, S.shuffle ? "Shuffle on" : "Shuffle off"); break;
    case 'r': case 'R': S.repeat = (S.repeat + 1) % 3; save_settings();
        status(A_DIM, S.repeat == 0 ? "Repeat off" : (S.repeat == 1 ? "Repeat: this track" : "Repeat: the whole list")); break;
    case 't': case 'T': {
        static const int STEPS[] = { 0, 15, 30, 60 };
        int k = 0;
        while (k < 3 && STEPS[k] != g_sleep_min) k++;
        g_sleep_min = STEPS[(k + 1) % 4];
        if (g_sleep_min) { g_sleep_deadline = tsc_deadline_ms((uint32_t)g_sleep_min * 60000u); status(A_OK, "Sleep timer set: playback stops after it."); }
        else status(A_DIM, "Sleep timer off.");
        break;
    }
    case 'v': case 'V': {
        const track_t *t = selected();
        if (!t && g_has_now) t = &g_now;
        if (t) { track_t tt = *t; play_video(&tt); } else status(A_WARN, "Select a track first.");
        g_dirty = true;
        break;
    }
    case 'a': case 'A': {
        const track_t *t = selected();
        if (t && g_view != V_QUEUE) {
            if (g_nq < MAX_Q) { g_queue[g_nq++] = *t; status(A_OK, "Added to the queue."); }
            else status(A_ERR, "The queue is full.");
        } else if (g_view == V_PL) {
            int slot = pl_slot(g_sel[V_PL]);
            if (slot >= 0) { static track_t tmp[MAX_PLT]; int c = pl_read(slot, tmp, MAX_PLT); for (int i = 0; i < c && g_nq < MAX_Q; i++) g_queue[g_nq++] = tmp[i]; status(A_OK, "Playlist added to the queue."); }
        }
        break;
    }
    case 'c': case 'C': g_nq = 0; g_sel[V_QUEUE] = 0; status(A_DIM, "Queue cleared."); break;
    case 'd': case 'D':
        if (g_view == V_QUEUE && g_nq > 0) { remove_at(g_queue, &g_nq, g_sel[V_QUEUE]); g_dirty = true; }
        else if (g_view == V_FAV && g_nfav > 0) { remove_at(g_fav, &g_nfav, g_sel[V_FAV]); save_favs(); g_dirty = true; }
        else if (g_view == V_HIST && g_nhist > 0) { remove_at(g_hist, &g_nhist, g_sel[V_HIST]); save_hist(); g_dirty = true; }
        else if (g_view == V_PL) {
            int slot = pl_slot(g_sel[V_PL]);
            if (slot >= 0 && confirm("Delete this playlist?")) { char path[32]; pl_path(slot, path, sizeof(path)); avfs_remove_file(path); pl_scan(); status(A_DIM, "Playlist deleted."); }
            g_dirty = true;
        }
        break;
    case 'f': case 'F': case 'l': case 'L': {
        const track_t *t = selected();
        if (t) toggle_like(t);
        break;
    }
    case 'S': save_queue_as_playlist(); g_dirty = true; break;
    case 'P': {
        const track_t *t = selected();
        if (t) add_to_playlist(t); else status(A_WARN, "Select a track first.");
        g_dirty = true;
        break;
    }
    case 'u': case 'U':
        do_login();
        g_dirty = true;
        break;
    default: break;
    }
    return true;
}

static void run_player(const char *initial_query, const char *initial_id, bool as_video)
{
    if (!fs_begin()) { print("ytm: the display is too small for the player\n"); return; }
    g_dirty = true;
    g_status[0] = '\0';
    g_status_attr = A_DIM;
    load_favs();
    load_hist();
    load_searches();
    pl_scan();
    g_user[0] = '\0';
    if (avfs_file_exists(VAULT_FILE)) do_unlock();
    if (!helper_ping()) status(A_WARN, "The helper is not reachable: run tools/ytm_helper.py on the host (set YTM_API or the settings to change the address).");
    else if (!g_status[0]) status(A_DIM, "Connected to the helper. Press / to search, ? for the keys.");
    if (initial_query && initial_query[0]) do_search(initial_query);
    if (initial_id && initial_id[0]) {
        memset(&g_res[0], 0, sizeof(g_res[0]));
        strcpy(g_res[0].id, initial_id);
        strcpy(g_res[0].title, "YouTube video ");
        strcat(g_res[0].title, initial_id);
        char path[40];
        int st = 0;
        snprintf(path, sizeof(path), "/info?id=%s", initial_id);
        if (http_get(path, &st) > 0 && st == 200) { track_t info[1]; if (parse_tracks(info, 1) == 1) g_res[0] = info[0]; }
        g_nres = 1; g_view = V_RES;
        if (as_video) { track_t tt = g_res[0]; play_video(&tt); }
        else play_from(V_RES, 0);
    }

    uint64_t next_tick = tsc_deadline_ms(250);
    for (;;) {
        if (g_open && !g_paused) {
            if (rust_music_pump() == 0) { next_track(true); g_dirty = true; }
        }
        if (g_sleep_min && tsc_reached(g_sleep_deadline)) {
            g_sleep_min = 0;
            stop_playback();
            status(A_OK, "Sleep timer: playback stopped.");
        }
        fs_event_t ev;
        if (fs_get_event(&ev)) {
            if (!handle_key(&ev)) break;
        }
        if (g_dirty) { draw_all(); next_tick = tsc_deadline_ms(250); }
        else if (tsc_reached(next_tick)) { draw_player(); fs_present(); next_tick = tsc_deadline_ms(250); }
        else __asm__ volatile("pause");
    }
    stop_playback();
    fs_end();
}

static void print_list(const track_t *list, int n, bool extras)
{
    for (int i = 0; i < n; i++) {
        char dur[12], ago[16];
        clock_text(list[i].secs, dur);
        ago_text(list[i].when, ago);
        if (extras) printr("%3d  %s  %s  %ux  %s  -  %s  (%s)\n", i + 1, list[i].id, dur, list[i].plays, list[i].title, list[i].artist, ago);
        else printr("%3d  %s  %s  %s  -  %s\n", i + 1, list[i].id, dur, list[i].title, list[i].artist);
    }
    if (n == 0) print("(empty)\n");
}

static bool parse_video_id(const char *a, char *vid)
{
    const char *id = NULL;
    const char *v = strstr(a, "v=");
    if (v) id = v + 2; else if (strstr(a, "youtu.be/")) id = strstr(a, "youtu.be/") + 9; else id = a;
    size_t n = 0;
    while (n < 11 && ((id[n] >= 'A' && id[n] <= 'Z') || (id[n] >= 'a' && id[n] <= 'z') || (id[n] >= '0' && id[n] <= '9') || id[n] == '-' || id[n] == '_')) { vid[n] = id[n]; n++; }
    vid[n] = '\0';
    return n == 11;
}

void ytm_command(int argc, char *argv[])
{
    load_settings();
    const char *env = shell_getenv("YTM_API");
    const char *base = (env && strncmp(env, "http", 4) == 0) ? env : (S.helper[0] ? S.helper : YTM_BASE);
    strncpy(g_base, base, sizeof(g_base) - 1);
    g_base[sizeof(g_base) - 1] = '\0';

    if (argc >= 2 && strcmp(argv[1], "login") == 0) {
        g_status[0] = '\0';
        if (!fs_begin()) return;
        bool ok = do_login();
        fs_end();
        printr("%s\n", ok ? "ytm: account saved, sealed with your passphrase (/disk/ytm.vlt)" : (g_status[0] ? g_status : "ytm: cancelled"));
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "logout") == 0) {
        if (avfs_file_exists(VAULT_FILE)) { avfs_remove_file(VAULT_FILE); print("ytm: saved account deleted\n"); }
        else print("ytm: no saved account\n");
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "status") == 0) {
        printr("helper   %s  (%s)\n", g_base, helper_ping() ? "reachable" : "NOT reachable - run tools/ytm_helper.py on the host");
        printr("account  %s\n", avfs_file_exists(VAULT_FILE) ? "saved in /disk/ytm.vlt (PRP-sealed)" : "none (ytm login)");
        load_favs(); load_hist(); pl_scan();
        printr("liked %d   history %d   playlists %d\n", g_nfav, g_nhist, pl_count());
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "liked") == 0) { load_favs(); print_list(g_fav, g_nfav, false); return; }
    if (argc >= 2 && strcmp(argv[1], "history") == 0) { load_hist(); print_list(g_hist, g_nhist, true); return; }
    if (argc >= 2 && strcmp(argv[1], "settings") == 0) {
        static const char *const REP[] = { "off", "track", "list" };
        printr("volume %d%%  repeat %s  shuffle %s  autoplay %s  history %s  video %dp\nhelper %s\n", S.vol, REP[S.repeat], S.shuffle ? "on" : "off",
               S.autoplay ? "on" : "off", S.history ? "on" : "off", S.vq, S.helper[0] ? S.helper : "(default)");
        print("change them in the player: press s\n");
        return;
    }

    char query[120];
    query[0] = '\0';
    if (argc >= 3 && (strcmp(argv[1], "play") == 0 || strcmp(argv[1], "video") == 0)) {
        char vid[12];
        if (!parse_video_id(argv[2], vid)) { print("Usage: ytm play|video VIDEO_ID | youtube address\n"); return; }
        run_player(NULL, vid, strcmp(argv[1], "video") == 0);
        return;
    }
    for (int i = 1; i < argc; i++) {
        if (strlen(query) + strlen(argv[i]) + 2 >= sizeof(query)) break;
        if (i > 1) strcat(query, " ");
        strcat(query, argv[i]);
    }
    run_player(query, NULL, false);
}
