// doomport.c - Doom (ozkl/doomgeneric) on RadiumOS, presented through the Vulkan implementation
//
//   doom [-vkscale N] [-aspect] [-iwad FILE] [doom options...]
//
// Bring your own WAD (doom1.wad, doom.wad, doom2.wad, plutonia.wad or tnt.wad as /disk/NAME). Doom reads it from the FAT
// volume or from the fetch store:
//   make doom-wad WAD=~/doom1.wad     writes it into fetchdisk.img's fetch store (any size; close QEMU first)
//   fetch -o /disk/doom1.wad http://10.0.2.2:8000/doom1.wad     the same from inside RadiumOS
// (A WAD under 10 MB can also go in update/files/ and travel with `make update-disk`.)
//
// How the picture gets to the screen: Doom renders its 320x200 frame in software as usual. By default the port enlarges it
// (nearest neighbour) to fill the 1280x720 screen, or to 960x720 with -aspect (4:3), copies it into a Vulkan buffer, records one
// vkCmdCopyBufferToImage into the swapchain image (src/vulkan/vulkan.h) and presents. With -vkscale N (1 to 3) the swapchain is the
// 320x200 frame and presenting enlarges it by N instead. No shader runs: interpreting one for every pixel cost more than Doom's own rendering.
// Sound: sound effects and music are mixed by src/doom/i_dgsound.c and streamed to the AC97 codec (needs a sound card;
// without one Doom runs silent). The game files are the GPL'd doomgeneric sources in src/doom with the C library in src/dglibc.
//
// Keys: W/S forward/back, A/D strafe, left/right arrows turn (up/down also move), E or Ctrl fires, Q or Space uses/opens,
// Shift runs, Alt+arrows strafe, 1-7 weapons, Tab map, Esc menu (arrows and Enter there; quit from it).

#include <stdint.h>
#include <stdbool.h>
#include "doomport.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../keyboard/keyboard.h"
#include "../io/io.h"
#include "../timers/tsc.h"
#include "../vulkan/vulkan.h"

#define DOOM_W 320
#define DOOM_H 200

// doomgeneric.h (not included: its headers would pull in the C library renames)
extern uint32_t *DG_ScreenBuffer;
void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick(void);
extern int dg_run(int (*main_fn)(void));
extern void dg_release_all(void);
extern int rust_disk_store_size(const uint8_t *path);
extern void DG_SoundPump(void);
extern void DG_SoundReset(void);
extern int menuactive;

#define OUT_ERR(...) do { terminal_setcolor(VGA_COLOR_LIGHT_RED); printr(__VA_ARGS__); terminal_setcolor(VGA_COLOR_WHITE); } while (0)

// ---- platform callbacks ------------------------------------------------------------------------------------------

// Milliseconds since the first call. The timer interrupt does not run while a command does, so this counts TSC cycles
// (32-bit steps: it is called many times per frame).
static uint32_t elapsed_ms(void)
{
    static uint32_t last_lo, ms, rem;
    static bool first = true;
    uint32_t lo, hi, per_ms = tsc_cycles_per_ms();
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    if (first) { first = false; last_lo = lo; return 0; }
    rem += lo - last_lo;
    last_lo = lo;
    if (per_ms) { ms += rem / per_ms; rem %= per_ms; }
    return ms;
}

static uint32_t g_time_scale = 100;   // percent; mods change it through the API (OUT_TIMESCALE)

uint32_t DG_GetTicksMs(void)
{
    static uint32_t last, virt, rem;
    uint32_t now = elapsed_ms(), acc = (now - last) * g_time_scale + rem;
    last = now;
    virt += acc / 100;
    rem = acc % 100;
    return virt;
}

void DG_SleepMs(uint32_t ms)
{
    uint64_t deadline = tsc_deadline_ms(ms);
    while (!tsc_reached(deadline)) { DG_SoundPump(); __asm__ volatile("pause"); }
}

void DG_SetWindowTitle(const char *title) { (void)title; }

// Doom key codes (doomkeys.h)
#define K_RIGHT 0xae
#define K_LEFT  0xac
#define K_UP    0xad
#define K_DOWN  0xaf
#define K_FN(n) (0x80 + (n))

// scan code set 1, unshifted main block
static const char MAIN_KEYS[0x36] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0x7f, 9,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13, 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/',
};

static unsigned char translate(uint8_t code)
{
    switch (code) {
    case 0x48: return menuactive ? K_UP : 'w';
    case 0x50: return menuactive ? K_DOWN : 's';
    case 0x4B: return K_LEFT;
    case 0x4D: return K_RIGHT;
    case 0x1D: return 'e';            // Ctrl: fire
    case 0x2A: case 0x36: return K_FN(0x36);   // Shift: run
    case 0x38: return K_FN(0x38);     // Alt: strafe
    case 0x39: return menuactive ? ' ' : 'q';   // Space: use
    case 0x57: return K_FN(0x57);
    case 0x58: return K_FN(0x58);
    default: break;
    }
    if (code >= 0x3B && code <= 0x44) return K_FN(code);   // F1-F10
    if (code < sizeof(MAIN_KEYS)) return (unsigned char)MAIN_KEYS[code];
    return 0;
}

// ---- Helios mods ---------------------------------------------------------------------------------------------------
//
// Every *.hls in /disk/mods whose first line is "// mod: description" is listed under MODS in Doom's own menu (Esc). Enter
// toggles a mod. An enabled mod is compiled once to /tmp/modN.rxe, then its main() runs before every frame is presented
// (the pre_frame hook) with these arguments:
//   argv(1) frame number (-1 = the mod was just switched off: undo your changes)   argv(2) address of the 320x200 XRGB frame
//   argv(3) address of 256 bytes of per-mod state (zeroed when enabled)   argv(4) address of vga_font_8x8 (8 bytes per glyph,
//   bit 0 = left pixel)   argv(5) address of the game API table (indices and struct offsets: update/files/mods/doom.hls)
// Nothing in src/doom is modified: the menu entry is patched into Doom's menu tables at run time and the API reads Doom's globals.

extern int helios_exec_file_args(const uint8_t *path, uint8_t verbosity, int argc, char **argv);
extern uint8_t vga_font_8x8[256][8];

#define MOD_DIR  "/disk/mods"
#define MAX_MODS 40
#define MODS_PER_PAGE 7   // Doom's menu has room for 8 rows: 7 mods and a "more" row

typedef struct {
    char file[32], desc[48], rxe[20];
    bool enabled;
    uint32_t state[64];
} mod_t;

static mod_t g_mods[MAX_MODS];
static int g_nmods;
static char g_menu_msg[48];
static uint32_t g_frames;

// Game API handed to mods as argv(5): addresses/values and struct offsets of this build of Doom (offsets from player_t, mobj_t,
// mobjinfo_t and thinker_t in src/doom). The indices are mirrored by the constants in update/files/mods/doom.hls.
extern uint8_t players[], mobjinfo[], thinkercap[];
extern int consoleplayer, leveltime, gamemap, gameepisode, gamestate, gametic, paused;
extern void P_MobjThinker(void *mobj);

#define PLAYER_SIZE 288   // sizeof(player_t)
#define MOBJINFO_SIZE 92  // sizeof(mobjinfo_t)

static uint32_t g_api[160];

// g_api[44..54] are more offsets and live values (see api_update); g_api[60..75] are written by mods and read back by the port:
#define OUT_WANT_TARGET 60   // 1: find the nearest visible monster (API 52 = mobj, 53 = angle to it, 54 = distance)
#define OUT_FIRE        61   // 1: hold the fire key
#define OUT_FOV         62   // field of view in degrees (40..120), 0 = Doom's 90
#define OUT_SAT         63   // colour saturation in percent (100 = unchanged)
#define OUT_CON         64   // contrast in percent (100 = unchanged)
#define OUT_VIG         65   // vignette strength 0..100
#define OUT_SHARP       66   // sharpening 0..100
#define OUT_TIMESCALE   67   // game speed in percent (100 = normal)
#define OUT_KILL0       68   // 8 slots: mobj addresses to kill; the port clears them
#define OUT_KILLS       8
#define OUT_DMG         76   // damage of the kill slots (0 = 10000)
#define OUT_UPSCALE     77   // 0 nearest, 1 smooth, 2 crisp smooth, 3 CRT (only when Doom fills the screen)

extern uint8_t states[], weaponinfo[];
extern const int finesine[];

extern int viewwindowx, viewwindowy, viewwidth, viewheight, detailshift, centerxfrac, projection, setsizeneeded;
extern int viewangletox[], yslope[], distscale[];
extern uint32_t xtoviewangle[], clipangle;
extern const int finetangent[];
extern const int *finecosine;
extern int FixedMul(int a, int b);
extern int FixedDiv(int a, int b);
extern void R_ExecuteSetViewSize(void);
extern int P_CheckSight(void *t1, void *t2);
extern uint32_t R_PointToAngle2(int x1, int y1, int x2, int y2);
extern void P_DamageMobj(void *target, void *inflictor, void *source, int damage);

static void mods_defaults(void)
{
    memset(&g_api[60], 0, 20 * sizeof(uint32_t));
    g_api[OUT_SAT] = 100;
    g_api[OUT_CON] = 100;
    g_api[OUT_TIMESCALE] = 100;
    g_time_scale = 100;
}
static uint32_t g_modkeys;   // bit n set while key Z, X, C, V (n = 0..3) is held; those keys are not passed on to Doom while a mod is on
static bool g_fire_down;

static void api_update(void)
{
    uint8_t *p = players + consoleplayer * PLAYER_SIZE;
    bool in_level = gamestate == 0;   // GS_LEVEL
    static const uint32_t OFFSETS[] = {
        /* 12 */ 40, 44, 48, 52, 76, 120, 128, 164, 180, 100, 212,   // player: health armor armortype powers cards readyweapon weaponowned ammo maxammo backpack killcount
        /* 23 */ 108, 72, 76, 80, 104, 88, 12, 16, 20,               // mobj: health momx momy momz flags type x y z
        /* 32 */ 8, 60, 32,                                          // mobjinfo: spawnhealth speed painchance
        /* 35 */ 4, 8,                                               // thinker: next function
    };
    static const uint32_t OFFSETS2[] = {
        /* 39 */ 56,                                                 // mobj: floorz
        /* 40 */ 228, 232, 240, 244,                                 // player: damagecount bonuscount extralight fixedcolormap
    };
    g_api[0] = (uint32_t)(uintptr_t)p;
    g_api[1] = in_level ? *(uint32_t *)p : 0;   // player->mo
    g_api[2] = (uint32_t)leveltime;
    g_api[3] = (uint32_t)gamemap;
    g_api[4] = (uint32_t)gameepisode;
    g_api[5] = (uint32_t)gamestate;
    g_api[6] = (uint32_t)(menuactive || paused);
    g_api[7] = (uint32_t)gametic;
    g_api[8] = (uint32_t)(uintptr_t)mobjinfo;
    g_api[9] = MOBJINFO_SIZE;
    g_api[10] = (uint32_t)(uintptr_t)thinkercap;
    g_api[11] = (uint32_t)(uintptr_t)P_MobjThinker;
    for (size_t i = 0; i < sizeof(OFFSETS) / sizeof(OFFSETS[0]); i++) g_api[12 + i] = OFFSETS[i];
    g_api[37] = g_modkeys;
    g_api[38] = elapsed_ms();
    for (size_t i = 0; i < sizeof(OFFSETS2) / sizeof(OFFSETS2[0]); i++) g_api[39 + i] = OFFSETS2[i];
    g_api[44] = 204;   // player: cheats
    g_api[45] = 32;    // mobj: angle
    g_api[46] = 132;   // mobj: player
    g_api[48] = (uint32_t)viewwindowx;
    g_api[49] = (uint32_t)viewwindowy;
    g_api[50] = (uint32_t)viewwidth;
    g_api[51] = (uint32_t)viewheight;
    g_api[52] = g_api[53] = g_api[54] = 0;
    g_api[55] = (uint32_t)(uintptr_t)states;
    g_api[56] = 28;    // sizeof(state_t)
    g_api[57] = (uint32_t)(uintptr_t)weaponinfo;
    g_api[58] = 24;    // sizeof(weaponinfo_t)
    g_api[59] = 252;   // player: psprites
    g_api[95] = 0;
    if (g_api[OUT_WANT_TARGET] && g_api[1]) {
        // WANT = 1: nearest monster the player can see (counted kill, shootable, alive). WANT = 2 also lists every visible one
        // in API 96.. as (address, sideways distance from the view line, distance ahead), at most 12 entries.
        typedef struct th { struct th *prev, *next; void *func; } th_t;
        th_t *cap = (th_t *)thinkercap;
        uint8_t *mo = (uint8_t *)(uintptr_t)g_api[1], *best = NULL;
        bool list = g_api[OUT_WANT_TARGET] == 2;
        int best_d = 0, guard = 0, nlist = 0;
        int px = *(int *)(mo + 12), py = *(int *)(mo + 16);
        uint32_t pang = *(uint32_t *)(mo + 32);
        int cosv = finesine[(pang >> 19) + 2048], sinv = finesine[pang >> 19];
        for (th_t *t = cap->next; t != cap && t && guard < 4096; t = t->next, guard++) {
            if (t->func != (void *)P_MobjThinker) continue;
            uint8_t *m = (uint8_t *)t;
            int flags = *(int *)(m + 104), hp = *(int *)(m + 108);
            if (m == mo || !(flags & 0x400000) || !(flags & 4) || hp <= 0) continue;
            int dx = (*(int *)(m + 12) - px) >> 16, dy = (*(int *)(m + 16) - py) >> 16;
            int d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            if (!list && best && d >= best_d) continue;
            if (!P_CheckSight(mo, m)) continue;
            if (!best || d < best_d) { best = m; best_d = d; }
            if (list && nlist < 12) {
                g_api[96 + nlist * 3] = (uint32_t)(uintptr_t)m;
                g_api[97 + nlist * 3] = (uint32_t)((dy * cosv - dx * sinv) >> 16);   // sideways
                g_api[98 + nlist * 3] = (uint32_t)((dx * cosv + dy * sinv) >> 16);   // ahead
                nlist++;
            }
        }
        g_api[95] = (uint32_t)nlist;
        if (best) {
            g_api[52] = (uint32_t)(uintptr_t)best;
            g_api[53] = R_PointToAngle2(px, py, *(int *)(best + 12), *(int *)(best + 16));
            g_api[54] = (uint32_t)best_d;
        }
    }
}

// Applies what mods asked for through the API outputs. Runs after the mods, once per frame.
static int g_fov_applied, g_fov_proj;

static void fov_update(uint32_t want)
{
    if (want < 40 || want > 120) want = 0;
    if (!want) {
        if (g_fov_applied) { R_ExecuteSetViewSize(); g_fov_applied = 0; }   // back to Doom's own 90 degrees
        return;
    }
    if ((int)want == g_fov_applied && projection == g_fov_proj) return;      // the engine resets projection when the view size changes
    // R_InitTextureMapping / R_ExecuteSetViewSize from src/doom/r_main.c with another focal length: 8192 fine angles = 360 degrees
    int idx = 2048 + (int)want * 8192 / 360 / 2;
    if (idx > 4095) idx = 4095;
    int focal = FixedDiv(centerxfrac, finetangent[idx]);
    for (int i = 0; i < 4096; i++) {
        int t;
        if (finetangent[i] > 2 * 65536) t = -1;
        else if (finetangent[i] < -2 * 65536) t = viewwidth + 1;
        else {
            t = FixedMul(finetangent[i], focal);
            t = (centerxfrac - t + 65535) >> 16;
            if (t < -1) t = -1; else if (t > viewwidth + 1) t = viewwidth + 1;
        }
        viewangletox[i] = t;
    }
    for (int x = 0; x <= viewwidth; x++) {
        int i = 0;
        while (viewangletox[i] > x) i++;
        xtoviewangle[x] = ((uint32_t)i << 19) - 0x40000000u;
    }
    for (int i = 0; i < 4096; i++) {
        if (viewangletox[i] == -1) viewangletox[i] = 0;
        else if (viewangletox[i] == viewwidth + 1) viewangletox[i] = viewwidth;
    }
    clipangle = xtoviewangle[0];
    projection = focal;
    for (int i = 0; i < viewheight; i++) {
        int dy = ((i - viewheight / 2) << 16) + 32768;
        yslope[i] = FixedDiv(focal << detailshift, dy < 0 ? -dy : dy);
    }
    for (int i = 0; i < viewwidth; i++) {
        int c = finecosine[xtoviewangle[i] >> 19];
        distscale[i] = FixedDiv(65536, c < 0 ? -c : c);
    }
    g_fov_applied = (int)want;
    g_fov_proj = focal;
}

static void mods_post_frame(void)
{
    uint8_t *mo = (uint8_t *)(uintptr_t)g_api[1];
    for (int i = 0; i < OUT_KILLS; i++) {
        uint32_t t = g_api[OUT_KILL0 + i];
        g_api[OUT_KILL0 + i] = 0;
        if (t && mo && gamestate == 0) P_DamageMobj((void *)(uintptr_t)t, NULL, mo, g_api[OUT_DMG] ? (int)g_api[OUT_DMG] : 10000);
    }
    uint32_t ts = g_api[OUT_TIMESCALE];
    g_time_scale = ts < 10 ? 10 : (ts > 400 ? 400 : ts);
    fov_update(g_api[OUT_FOV]);
}

// Colour grading on the 320x200 frame: saturation, contrast, vignette and sharpening (OUT_SAT, OUT_CON, OUT_VIG, OUT_SHARP).
static void fx_apply(void)
{
    static uint32_t src[DOOM_W * DOOM_H];
    static uint16_t vtab[DOOM_W * DOOM_H];
    static int vtab_for = -1;
    int sat = (int)g_api[OUT_SAT], con = (int)g_api[OUT_CON], vig = (int)g_api[OUT_VIG], sharp = (int)g_api[OUT_SHARP];
    if (sat < 0) sat = 0; if (sat > 300) sat = 300;
    if (con < 20) con = 20; if (con > 300) con = 300;
    if (vig < 0) vig = 0; if (vig > 100) vig = 100;
    if (sharp < 0) sharp = 0; if (sharp > 100) sharp = 100;
    if (sat == 100 && con == 100 && !vig && !sharp) return;
    uint32_t *px = DG_ScreenBuffer;

    if (sharp) {
        memcpy(src, px, sizeof(src));
        for (int y = 1; y < DOOM_H - 1; y++)
            for (int x = 1; x < DOOM_W - 1; x++) {
                uint32_t c = src[y * DOOM_W + x], n = src[(y - 1) * DOOM_W + x], s = src[(y + 1) * DOOM_W + x];
                uint32_t w = src[y * DOOM_W + x - 1], e = src[y * DOOM_W + x + 1], out = 0;
                for (int sh = 0; sh < 24; sh += 8) {
                    int cc = (c >> sh) & 255;
                    int v = cc + ((4 * cc - (int)((n >> sh) & 255) - (int)((s >> sh) & 255) - (int)((w >> sh) & 255) - (int)((e >> sh) & 255)) * sharp) / 200;
                    out |= (uint32_t)(v < 0 ? 0 : (v > 255 ? 255 : v)) << sh;
                }
                px[y * DOOM_W + x] = out;
            }
    }
    if (vig && vtab_for != vig) {
        for (int y = 0; y < DOOM_H; y++)
            for (int x = 0; x < DOOM_W; x++) {
                int dx = (x - DOOM_W / 2) * 100 / (DOOM_W / 2), dy = (y - DOOM_H / 2);
                int m = 256 - vig * ((dx * dx + dy * dy) / 2) * 256 / (100 * 14000);
                vtab[y * DOOM_W + x] = (uint16_t)(m < 0 ? 0 : m);
            }
        vtab_for = vig;
    }
    int con256 = con * 256 / 100;
    for (int i = 0; i < DOOM_W * DOOM_H; i++) {
        uint32_t p = px[i];
        int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        r = (((r - 128) * con256) >> 8) + 128; g = (((g - 128) * con256) >> 8) + 128; b = (((b - 128) * con256) >> 8) + 128;
        int gray = (r * 77 + g * 150 + b * 29) >> 8;
        r = gray + (r - gray) * sat / 100; g = gray + (g - gray) * sat / 100; b = gray + (b - gray) * sat / 100;
        if (vig) { int m = vtab[i]; r = r * m >> 8; g = g * m >> 8; b = b * m >> 8; }
        r = r < 0 ? 0 : (r > 255 ? 255 : r); g = g < 0 ? 0 : (g > 255 ? 255 : g); b = b < 0 ? 0 : (b > 255 ? 255 : b);
        px[i] = (uint32_t)(r << 16 | g << 8 | b);
    }
}

static bool ends_with_hls(const char *s)
{
    size_t n = strlen(s);
    return n > 4 && s[n - 4] == '.' && (s[n - 3] | 32) == 'h' && (s[n - 2] | 32) == 'l' && (s[n - 1] | 32) == 's';
}

static void mods_scan(void)
{
    static DirectoryEntry ents[32];
    static mod_t old[MAX_MODS];
    int nold = g_nmods;
    memcpy(old, g_mods, sizeof(old));
    memset(g_mods, 0, sizeof(g_mods));
    g_nmods = 0;
    int n = avfs_list_directory(MOD_DIR, ents, 32);
    for (int i = 0; i < n && g_nmods < MAX_MODS; i++) {
        if (ents[i].is_directory || !ends_with_hls(ents[i].name)) continue;
        char path[64], head[96];
        snprintf(path, sizeof(path), MOD_DIR "/%s", ents[i].name);
        memset(head, 0, sizeof(head));
        avfs_read_file(path, head, ents[i].size < sizeof(head) - 1 ? ents[i].size : sizeof(head) - 1, 0);
        if (strncmp(head, "// mod:", 7) != 0) continue;
        mod_t *m = &g_mods[g_nmods];
        strncpy(m->file, ents[i].name, sizeof(m->file) - 1);
        const char *d = head + 7;
        while (*d == ' ') d++;
        size_t l = 0;
        while (d[l] && d[l] != '\n' && d[l] != '\r' && l < sizeof(m->desc) - 1) { m->desc[l] = d[l]; l++; }
        for (int j = 0; j < nold; j++)
            if (strcmp(old[j].file, m->file) == 0) { m->enabled = old[j].enabled; memcpy(m->state, old[j].state, sizeof(m->state)); }
        char base[12];
        size_t bl = strlen(m->file) - 4;
        if (bl > 8) bl = 8;
        memcpy(base, m->file, bl);
        base[bl] = 0;
        snprintf(m->rxe, sizeof(m->rxe), "/tmp/%s.rxe", base);
        g_nmods++;
    }
    for (int i = 1; i < g_nmods; i++) {   // alphabetical, whatever order the directory lists them in
        static mod_t tmp;
        int j = i;
        while (j > 0 && strcmp(g_mods[j - 1].file, g_mods[i].file) > 0) j--;
        if (j == i) continue;
        tmp = g_mods[i];
        memmove(&g_mods[j + 1], &g_mods[j], (size_t)(i - j) * sizeof(mod_t));
        g_mods[j] = tmp;
    }
}

static void discard_char(char c, void *ctx) { (void)c; (void)ctx; }

static void mod_call(mod_t *m, const char *frame)
{
    char a_screen[12], a_state[12], a_font[12], a_api[12];
    char *argv[6];
    snprintf(a_screen, sizeof(a_screen), "%u", (unsigned)(uintptr_t)DG_ScreenBuffer);
    snprintf(a_state, sizeof(a_state), "%u", (unsigned)(uintptr_t)m->state);
    snprintf(a_font, sizeof(a_font), "%u", (unsigned)(uintptr_t)vga_font_8x8);
    snprintf(a_api, sizeof(a_api), "%u", (unsigned)(uintptr_t)g_api);
    argv[0] = m->rxe; argv[1] = (char *)frame; argv[2] = a_screen; argv[3] = a_state; argv[4] = a_font; argv[5] = a_api;
    helios_exec_file_args((const uint8_t *)m->rxe, 0, 6, argv);
}

static void mod_toggle(mod_t *m)
{
    if (m->enabled) {
        api_update();
        mod_call(m, "-1");
        m->enabled = false;
        snprintf(g_menu_msg, sizeof(g_menu_msg), "%s OFF", m->file);
        return;
    }
    char cmd[128];
    avfs_remove_file(m->rxe);
    snprintf(cmd, sizeof(cmd), "helios build " MOD_DIR "/%s -o %s", m->file, m->rxe);
    terminal_begin_capture_sink(discard_char, NULL);   // the compiler's diagnostics would only pile up on the hidden console
    execute_command(cmd);
    terminal_end_capture();
    if (!avfs_file_exists(m->rxe)) { snprintf(g_menu_msg, sizeof(g_menu_msg), "%s: BUILD FAILED", m->file); return; }
    memset(m->state, 0, sizeof(m->state));
    m->enabled = true;
    snprintf(g_menu_msg, sizeof(g_menu_msg), "%s ON", m->file);
}

// the pre_frame hook: runs every enabled mod just before the frame is presented
static void mods_pre_frame(void)
{
    char a_frame[12];
    api_update();
    snprintf(a_frame, sizeof(a_frame), "%u", g_frames);
    for (int i = 0; i < g_nmods; i++)
        if (g_mods[i].enabled) mod_call(&g_mods[i], a_frame);
}

// ---- the MODS entry in Doom's menu -------------------------------------------------------------------------------------------
// Same layout as menuitem_t / menu_t in src/doom/m_menu.c; Doom's own tables are edited at run time (see menu_patch).

typedef struct { short status; char name[10]; void (*routine)(int choice); char key; } dm_item_t;
typedef struct dm_menu { short numitems; struct dm_menu *prev; dm_item_t *items; void (*draw)(void); short x, y, last_on; } dm_menu_t;

extern dm_menu_t MainDef, *currentMenu;
extern dm_item_t MainMenu[];
extern short itemOn;
extern void M_SetupNextMenu(dm_menu_t *menu);
extern void M_WriteText(int x, int y, char *string);
extern void M_DrawMainMenu(void);

#define MAIN_MODS_ROW 4   // New Game, Options, Load, Save, MODS, Quit (replaces "Read This!")

static dm_item_t g_main_snap[6], g_mod_items[MODS_PER_PAGE + 1];
static dm_menu_t g_def_snap, g_mods_def;
static bool g_snapped, g_patched;
static int g_page;

static bool mods_any_enabled(void)
{
    for (int i = 0; i < g_nmods; i++) if (g_mods[i].enabled) return true;
    return false;
}

static int mods_pages(void) { return (g_nmods + MODS_PER_PAGE - 1) / MODS_PER_PAGE; }

static int mods_on_page(void)
{
    int n = g_nmods - g_page * MODS_PER_PAGE;
    return n > MODS_PER_PAGE ? MODS_PER_PAGE : (n > 0 ? n : 0);
}

static void mods_page_setup(void)
{
    int n = mods_on_page() + (mods_pages() > 1 ? 1 : 0);
    g_mods_def.numitems = n ? n : 1;
}

static void draw_main(void)
{
    M_DrawMainMenu();
    M_WriteText(MainDef.x, MainDef.y + 16 * MAIN_MODS_ROW + 4, "MODS");
}

static void draw_mods(void)
{
    char line[40];
    int shown = mods_on_page();
    M_WriteText(48, 12, "HELIOS MODS");
    if (g_menu_msg[0]) M_WriteText(136, 12, g_menu_msg);
    if (!g_nmods) { M_WriteText(48, 34, "NO MODS IN " MOD_DIR); return; }
    for (int i = 0; i < shown; i++) {
        const char *file = g_mods[g_page * MODS_PER_PAGE + i].file;
        size_t n = strlen(file) - 4;   // without ".hls"
        char name[16];
        size_t k = 0;
        for (; k < n && k < sizeof(name) - 1; k++) name[k] = file[k];
        name[k] = 0;
        snprintf(line, sizeof(line), "[%c] %s", g_mods[g_page * MODS_PER_PAGE + i].enabled ? 'X' : ' ', name);
        M_WriteText(48, 34 + 16 * i, line);
    }
    if (mods_pages() > 1) {
        snprintf(line, sizeof(line), "MORE MODS...  %d/%d", g_page + 1, mods_pages());
        M_WriteText(48, 34 + 16 * shown, line);
    }
    if (itemOn < shown) M_WriteText(16, 160, g_mods[g_page * MODS_PER_PAGE + itemOn].desc);
}

static void mod_pick(int choice)
{
    if (choice < mods_on_page()) { mod_toggle(&g_mods[g_page * MODS_PER_PAGE + choice]); return; }
    g_page = (g_page + 1) % mods_pages();
    mods_page_setup();
    itemOn = 0;
}

static void mods_open(int choice)
{
    (void)choice;
    mods_scan();
    g_menu_msg[0] = 0;
    g_page = 0;
    mods_page_setup();
    g_mods_def.last_on = 0;
    M_SetupNextMenu(&g_mods_def);
}

// Runs once per game start, after Doom's M_Init: puts the MODS entry in the main menu where "Read This!" was.
static void menu_patch(void)
{
    if (g_patched || currentMenu != &MainDef) return;
    if (MainDef.numitems == 5) {          // Doom II dropped "Read This!": make room again
        MainMenu[5] = MainMenu[4];
        MainDef.numitems = 6;
        MainDef.y -= 8;
    }
    memset(&MainMenu[MAIN_MODS_ROW], 0, sizeof(dm_item_t));
    MainMenu[MAIN_MODS_ROW].status = 1;
    MainMenu[MAIN_MODS_ROW].routine = mods_open;
    MainMenu[MAIN_MODS_ROW].key = 'm';
    MainDef.draw = draw_main;
    for (int i = 0; i < MODS_PER_PAGE + 1; i++) { g_mod_items[i].status = 1; g_mod_items[i].routine = mod_pick; }
    g_mods_def.numitems = 1;
    g_mods_def.prev = &MainDef;
    g_mods_def.items = g_mod_items;
    g_mods_def.draw = draw_mods;
    g_mods_def.x = 48; g_mods_def.y = 30;
    g_patched = true;
}

// Doom's tables live in the kernel image and survive between `doom` runs, so every run starts from the pristine copy.
static void menu_reset(void)
{
    if (!g_snapped) { memcpy(g_main_snap, MainMenu, sizeof(g_main_snap)); g_def_snap = MainDef; g_snapped = true; }
    else { memcpy(MainMenu, g_main_snap, sizeof(g_main_snap)); MainDef = g_def_snap; }
    g_patched = false;
}


int DG_GetKey(int *pressed, unsigned char *key)
{
    static bool prefix;
    DG_SoundPump();
    bool want_fire = g_api[OUT_FIRE] && !menuactive && gamestate == 0;   // a mod (aimbot) holds the fire key
    if (want_fire != g_fire_down) {
        g_fire_down = want_fire;
        *pressed = want_fire;
        *key = 'e';
        return 1;
    }
    while (inb(0x64) & 0x01) {
        uint8_t status = inb(0x64), sc = inb(0x60);
        if (status & 0x20) continue;                 // mouse byte
        if (sc == 0xE0 || sc == 0xE1) { prefix = true; continue; }
        prefix = false;
        int mk = (sc & 0x7F) >= 0x2C && (sc & 0x7F) <= 0x2F ? (sc & 0x7F) - 0x2C : (sc & 0x7F) == 0x21 ? 4 : (sc & 0x7F) == 0x22 ? 5 : -1;
        if (mk >= 0) {   // Z X C V F G belong to mods (bits 0..5)
            uint32_t bit = 1u << mk;
            if (sc & 0x80) { if (g_modkeys & bit) { g_modkeys &= ~bit; continue; } }
            else if (!menuactive && mods_any_enabled()) { g_modkeys |= bit; continue; }
        }
        unsigned char k = translate(sc & 0x7F);
        if (!k) continue;
        *pressed = !(sc & 0x80);
        *key = k;
        return 1;
    }
    return 0;
}

// ---- the Vulkan presenter ----------------------------------------------------------------------------------------

typedef struct {
    VkInstance instance; VkDevice device; VkQueue queue;
    VkSurfaceKHR surface; VkSwapchainKHR swapchain;
    VkImage images[4]; uint32_t image_count;
    VkBuffer frame_buf; VkDeviceMemory frame_mem; uint8_t *frame_ptr;
    VkCommandBuffer cmd;
    uint32_t scale, width, height;
    bool ready, failed;
} presenter_t;

static presenter_t P;

#define VK_TRY(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { OUT_ERR("doom: %s failed (%d)\n", #call, (int)r_); return false; } } while (0)

static uint32_t g_scale;    // 0 = fit the screen, 1..3 = integer enlargement by the presenter
static uint32_t g_fit_w;    // output width when fitting: 1280 (fill) or 960 (-aspect, 4:3)
#define FIT_H 720

static bool presenter_init(uint32_t scale)
{
    P.scale = scale;
    // fitting: the swapchain is the whole 1280x720 (or 960x720) picture; otherwise it is the Doom frame and presenting enlarges it by `scale`
    P.width = g_scale ? DOOM_W : g_fit_w;
    P.height = g_scale ? DOOM_H : FIT_H;
    uint32_t n = 1;
    VkPhysicalDevice phys;
    VK_TRY(vkCreateInstance(NULL, NULL, &P.instance));
    VK_TRY(vkEnumeratePhysicalDevices(P.instance, &n, &phys));
    VK_TRY(vkCreateDevice(phys, NULL, NULL, &P.device));
    vkGetDeviceQueue(P.device, 0, 0, &P.queue);

    // the frame: one 32-bit XRGB word per pixel
    VkDeviceSize bytes = (VkDeviceSize)P.width * P.height * 4;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkMemoryRequirements req;
    VK_TRY(vkCreateBuffer(P.device, &bi, NULL, &P.frame_buf));
    vkGetBufferMemoryRequirements(P.device, P.frame_buf, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, 0 };
    VK_TRY(vkAllocateMemory(P.device, &ai, NULL, &P.frame_mem));
    VK_TRY(vkBindBufferMemory(P.device, P.frame_buf, P.frame_mem, 0));
    VK_TRY(vkMapMemory(P.device, P.frame_mem, 0, req.size, 0, (void **)&P.frame_ptr));

    VkRadiumSurfaceCreateInfoEXT sci = { VK_STRUCTURE_TYPE_RADIUM_SURFACE_CREATE_INFO_EXT, NULL, g_scale ? scale : 1 };
    VK_TRY(vkCreateRadiumSurfaceEXT(P.instance, &sci, NULL, &P.surface));
    VkSwapchainCreateInfoKHR swi = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, P.surface, 2, VK_FORMAT_B8G8R8A8_UNORM, 0, { P.width, P.height },
                                     1, VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, 0, 0, VK_PRESENT_MODE_FIFO_KHR, 1, 0 };
    VK_TRY(vkCreateSwapchainKHR(P.device, &swi, NULL, &P.swapchain));
    P.image_count = 4;
    VK_TRY(vkGetSwapchainImagesKHR(P.device, P.swapchain, &P.image_count, P.images));

    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, 0, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VK_TRY(vkAllocateCommandBuffers(P.device, &cbai, &P.cmd));
    P.ready = true;
    return true;
}

static void presenter_shutdown(void)
{
    if (P.swapchain) vkDestroySwapchainKHR(P.device, P.swapchain, NULL);
    if (P.surface) vkDestroySurfaceKHR(P.instance, P.surface, NULL);   // gives the screen back to the console
    if (P.instance) vkDestroyInstance(P.instance, NULL);
    memset(&P, 0, sizeof(P));
}

// Nearest-neighbour enlargement of Doom's 320x200 frame to ow x oh (ow is a multiple of 320): each source row is expanded once
// and repeated for the output rows that map to it.
static void fit_frame(uint32_t *dst, int ow, int oh)
{
    int sx = ow / DOOM_W, last = -1;
    for (int y = 0; y < oh; y++) {
        uint32_t *d = dst + (size_t)y * ow;
        int sy = y * DOOM_H / oh;
        if (sy == last) { memcpy(d, d - ow, (size_t)ow * 4); continue; }
        last = sy;
        const uint32_t *s = DG_ScreenBuffer + sy * DOOM_W;
        for (int x = 0; x < DOOM_W; x++) {
            uint32_t v = s[x];
            for (int k = 0; k < sx; k++) *d++ = v;
        }
    }
}

// Called by Doom after every rendered frame.
void DG_Init(void) {}

static inline uint32_t lerp_px(uint32_t a, uint32_t b, uint32_t w)   // w = 0..255 towards b
{
    uint32_t rb = (((a & 0xFF00FF) * (256 - w) + (b & 0xFF00FF) * w) >> 8) & 0xFF00FF;
    uint32_t g = (((a & 0x00FF00) * (256 - w) + (b & 0x00FF00) * w) >> 8) & 0x00FF00;
    return rb | g;
}

static inline uint32_t dim_px(uint32_t p, uint32_t f)   // f = 0..256
{
    return ((((p & 0xFF00FF) * f) >> 8) & 0xFF00FF) | ((((p & 0x00FF00) * f) >> 8) & 0x00FF00);
}

// Smooth enlargement: every Doom pixel is split into the output's smaller pixels, blended with its neighbours (bilinear). mode 2
// keeps the blend to the pixel borders (crisper), mode 3 adds CRT scanlines.
static void smooth_frame(uint32_t *dst, int ow, int oh, int mode)
{
    static uint32_t hb[2][1280];
    static uint16_t xi[1280];
    static uint8_t xw[1280];
    static int tab_ow = -1, tab_mode = -1, crow[2];
    if (ow > 1280) { fit_frame(dst, ow, oh); return; }
    if (tab_ow != ow || tab_mode != mode) {
        for (int x = 0; x < ow; x++) {
            int fp = ((2 * x + 1) * DOOM_W * 256) / (2 * ow) - 128, w;
            if (fp < 0) fp = 0;
            xi[x] = (uint16_t)(fp >> 8 < DOOM_W - 1 ? fp >> 8 : DOOM_W - 1);
            w = fp & 255;
            if (mode >= 2) { w = (w - 128) * 2 + 128; if (w < 0) w = 0; if (w > 255) w = 255; }
            xw[x] = (uint8_t)(xi[x] >= DOOM_W - 1 ? 0 : w);
        }
        tab_ow = ow; tab_mode = mode;
    }
    crow[0] = crow[1] = -1;   // the source frame changes on every call
    for (int y = 0; y < oh; y++) {
        int fp = ((2 * y + 1) * DOOM_H * 256) / (2 * oh) - 128;
        if (fp < 0) fp = 0;
        int r0 = fp >> 8, wy = fp & 255;
        if (r0 >= DOOM_H - 1) { r0 = DOOM_H - 1; wy = 0; }
        int r1 = r0 + 1 < DOOM_H ? r0 + 1 : r0;
        int s0 = crow[0] == r0 ? 0 : (crow[1] == r0 ? 1 : -1);
        int s1 = r1 == r0 ? -2 : (crow[0] == r1 ? 0 : (crow[1] == r1 ? 1 : -1));
        for (int pass = 0; pass < 2; pass++) {
            int need = pass ? r1 : r0, *slot = pass ? &s1 : &s0;
            if (*slot != -1) continue;
            *slot = (pass ? s0 : s1) == 0 ? 1 : 0;   // never evict the other wanted row
            const uint32_t *s = DG_ScreenBuffer + need * DOOM_W;
            for (int x = 0; x < ow; x++) hb[*slot][x] = lerp_px(s[xi[x]], s[xi[x] + 1 < DOOM_W ? xi[x] + 1 : xi[x]], xw[x]);
            crow[*slot] = need;
        }
        uint32_t *rows[2] = { hb[s0], hb[s1 < 0 ? s0 : s1] };
        uint32_t *d = dst + (size_t)y * ow;
        if (wy == 0 || r1 == r0) memcpy(d, rows[0], (size_t)ow * 4);
        else for (int x = 0; x < ow; x++) d[x] = lerp_px(rows[0][x], rows[1][x], (uint32_t)wy);
        if (mode == 3 && y % 3 == 2) for (int x = 0; x < ow; x++) d[x] = dim_px(d[x], 150);
    }
}

void DG_DrawFrame(void)
{
    if (P.failed) return;
    if (!P.ready && !presenter_init(g_scale)) { P.failed = true; presenter_shutdown(); P.failed = true; return; }

    menu_patch();
    mods_pre_frame();
    mods_post_frame();
    fx_apply();
    if (g_scale) memcpy(P.frame_ptr, DG_ScreenBuffer, (size_t)DOOM_W * DOOM_H * 4);
    else if (g_api[OUT_UPSCALE] >= 1 && g_api[OUT_UPSCALE] <= 3) smooth_frame((uint32_t *)P.frame_ptr, (int)P.width, (int)P.height, (int)g_api[OUT_UPSCALE]);
    else fit_frame((uint32_t *)P.frame_ptr, (int)P.width, (int)P.height);
    uint32_t index;
    if (vkAcquireNextImageKHR(P.device, P.swapchain, ~0ULL, 0, 0, &index) != VK_SUCCESS) return;
    vkBeginCommandBuffer(P.cmd, NULL);
    VkBufferImageCopy region;
    memset(&region, 0, sizeof(region));
    region.bufferRowLength = P.width;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = P.width; region.imageExtent.height = P.height; region.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(P.cmd, P.frame_buf, P.images[index], 0, 1, &region);
    vkEndCommandBuffer(P.cmd);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &P.cmd, 0, NULL };
    vkQueueSubmit(P.queue, 1, &si, 0);
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 0, NULL, 1, &P.swapchain, &index, NULL };
    vkQueuePresentKHR(P.queue, &pi);
    g_frames++;
    DG_SoundPump();
}

// ---- the command ---------------------------------------------------------------------------------------------------

static int g_argc;
static char *g_argv[24];

static int doom_main(void)
{
    doomgeneric_Create(g_argc, g_argv);
    for (;;) doomgeneric_Tick();
    return 0;
}

static bool wad_exists(const char *path)
{
    return avfs_file_exists(path) || rust_disk_store_size((const uint8_t *)path) >= 0;
}

void doom_command(int argc, char *argv[])
{
    static const char *const CANDIDATES[] = { "/disk/doom1.wad", "/disk/doom.wad", "/disk/doom2.wad", "/disk/plutonia.wad", "/disk/tnt.wad" };
    const char *iwad = NULL;
    g_argc = 0;
    g_argv[g_argc++] = "doom";
    g_scale = 0;
    g_fit_w = 1280;
    for (int i = 1; i < argc && g_argc < 20; i++) {
        if (strcmp(argv[i], "-vkscale") == 0 && i + 1 < argc) { g_scale = (uint32_t)atoi(argv[++i]); continue; }
        if (strcmp(argv[i], "-aspect") == 0) { g_fit_w = 960; continue; }
        if (strcmp(argv[i], "-iwad") == 0 && i + 1 < argc) iwad = argv[i + 1];
        g_argv[g_argc++] = argv[i];
    }
    if (g_scale > 3) { OUT_ERR("doom: -vkscale takes 1, 2 or 3 (the picture is 320x200 times that); without it Doom fills the screen\n"); return; }

    if (!iwad) {
        for (size_t i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]) && !iwad; i++) if (wad_exists(CANDIDATES[i])) iwad = CANDIDATES[i];
        if (!iwad) {
            OUT_ERR("doom: no WAD found. Bring your own doom1.wad:\n");
            print("  - make doom-wad WAD=/path/to/doom1.wad   (on the host, with QEMU closed), or\n");
            print("  - fetch -o /disk/doom1.wad http://10.0.2.2:8000/doom1.wad   (python3 -m http.server 8000 on the host)\n");
            return;
        }
        g_argv[g_argc++] = "-iwad";
        g_argv[g_argc++] = (char *)iwad;
    }
    g_argv[g_argc] = NULL;

    memset(&P, 0, sizeof(P));
    g_frames = 0;
    mods_defaults();
    g_fov_applied = 0;
    g_fire_down = false;
    menu_reset();
    mods_scan();
    uint32_t t0 = elapsed_ms();
    int code = dg_run(doom_main);
    uint32_t ms = elapsed_ms() - t0;
    DG_SoundReset();
    presenter_shutdown();
    dg_release_all();
    DG_ScreenBuffer = NULL;
    if (g_frames && ms) printr("doom: left with code %d after %u frames in %u ms (%u.%u frames/s at %ux%u)\n", code, g_frames, ms, g_frames * 1000 / ms,
                               (g_frames * 10000 / ms) % 10, g_scale ? DOOM_W * g_scale : g_fit_w, g_scale ? DOOM_H * g_scale : (uint32_t)FIT_H);
    else printr("doom: left with code %d\n", code);
}
