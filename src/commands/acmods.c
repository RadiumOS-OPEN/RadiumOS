// acmods.c - Helios mods for AssaultCube (port/ac). Same idea as the Doom mods in doomport.c, but the game is an app, so the
// app asks the kernel to run them through radium_api_t (mod_*) once per frame, just before the picture is presented.
//
// Every *.hls in /disk/mods whose first line is "// acmod: description" is a mod (Doom ignores those; its mods start with
// "// mod:"). An enabled mod is compiled once to /tmp/NAME.rxe and its main() runs every frame with
//   argv(1) frame number (-1 = the mod was just switched off: undo your changes)   argv(2) address of the XRGB frame
//   argv(3) address of 256 bytes of per-mod state (zeroed when enabled)            argv(4) address of vga_font_8x8
//   argv(5) address of the game API table kept by the app (indices: update/files/mods/ac.hls)
#include <stdint.h>
#include <stdbool.h>
#include "acmods.h"
#include "../terminal/terminal.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../keyboard/keyboard.h"

extern int helios_exec_file_args(const uint8_t *path, uint8_t verbosity, int argc, char **argv);
extern uint8_t vga_font_8x8[256][8];

#define MOD_DIR  "/disk/mods"
#define MAX_MODS 40

typedef struct {
    char file[32], desc[48], rxe[20];
    bool enabled;
    uint32_t state[64];
} mod_t;

static mod_t g_mods[MAX_MODS];
static int g_nmods;
static uint32_t g_frames;
static uint32_t *g_last_api, *g_last_screen;

static void discard_char(char c, void *ctx) { (void)c; (void)ctx; }

static bool ends_with_hls(const char *s)
{
    size_t n = strlen(s);
    return n > 4 && s[n - 4] == '.' && (s[n - 3] | 32) == 'h' && (s[n - 2] | 32) == 'l' && (s[n - 1] | 32) == 's';
}

int acmods_scan(void)
{
    static DirectoryEntry ents[48];
    static mod_t old[MAX_MODS];
    int nold = g_nmods;
    memcpy(old, g_mods, sizeof(old));
    memset(g_mods, 0, sizeof(g_mods));
    g_nmods = 0;
    int n = avfs_list_directory(MOD_DIR, ents, 48);
    for (int i = 0; i < n && g_nmods < MAX_MODS; i++) {
        if (ents[i].is_directory || !ends_with_hls(ents[i].name)) continue;
        char path[64], head[96];
        snprintf(path, sizeof(path), MOD_DIR "/%s", ents[i].name);
        memset(head, 0, sizeof(head));
        avfs_read_file(path, head, ents[i].size < sizeof(head) - 1 ? ents[i].size : sizeof(head) - 1, 0);
        if (strncmp(head, "// acmod:", 9) != 0) continue;
        mod_t *m = &g_mods[g_nmods];
        strncpy(m->file, ents[i].name, sizeof(m->file) - 1);
        const char *d = head + 9;
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
    return g_nmods;
}

int acmods_info(int i, char name[32], char desc[48], int *enabled)
{
    if (i < 0 || i >= g_nmods) return 0;
    strncpy(name, g_mods[i].file, 31); name[31] = 0;
    strncpy(desc, g_mods[i].desc, 47); desc[47] = 0;
    *enabled = g_mods[i].enabled;
    return 1;
}

static void mod_call(mod_t *m, const char *frame, uint32_t *screen, uint32_t *api)
{
    char a_screen[12], a_state[12], a_font[12], a_api[12];
    char *argv[6];
    snprintf(a_screen, sizeof(a_screen), "%u", (unsigned)(uintptr_t)screen);
    snprintf(a_state, sizeof(a_state), "%u", (unsigned)(uintptr_t)m->state);
    snprintf(a_font, sizeof(a_font), "%u", (unsigned)(uintptr_t)vga_font_8x8);
    snprintf(a_api, sizeof(a_api), "%u", (unsigned)(uintptr_t)api);
    argv[0] = m->rxe; argv[1] = (char *)frame; argv[2] = a_screen; argv[3] = a_state; argv[4] = a_font; argv[5] = a_api;
    helios_exec_file_args((const uint8_t *)m->rxe, 0, 6, argv);
}

int acmods_toggle(int i)
{
    if (i < 0 || i >= g_nmods) return -1;
    mod_t *m = &g_mods[i];
    if (m->enabled) {
        if (g_last_api) mod_call(m, "-1", g_last_screen, g_last_api);
        m->enabled = false;
        return 0;
    }
    char cmd[128];
    avfs_remove_file(m->rxe);
    snprintf(cmd, sizeof(cmd), "helios build " MOD_DIR "/%s -o %s", m->file, m->rxe);
    terminal_begin_capture_sink(discard_char, NULL);   // the compiler's diagnostics would only pile up on the hidden console
    execute_command(cmd);
    terminal_end_capture();
    if (!avfs_file_exists(m->rxe)) return -1;
    memset(m->state, 0, sizeof(m->state));
    m->enabled = true;
    return 1;
}

void acmods_run(uint32_t *screen, uint32_t *api)
{
    char a_frame[12];
    g_last_api = api; g_last_screen = screen;
    snprintf(a_frame, sizeof(a_frame), "%u", g_frames++);
    for (int i = 0; i < g_nmods; i++)
        if (g_mods[i].enabled) mod_call(&g_mods[i], a_frame, screen, api);
}
