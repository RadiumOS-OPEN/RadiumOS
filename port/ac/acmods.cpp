// acmods.cpp - the bridge between AssaultCube and the Helios mods (see src/commands/acmods.c and update/files/mods/ac.hls).
// Once per frame, before the picture is shown: the game state is copied into a table of ints, the kernel runs the enabled mods
// (they read the table, draw on the frame and write "outputs" back into it), then the outputs are applied to the game.
// The menu is AC's own: F12 or "hmods" opens a list of the *.hls mods with an [X] for the ones that are on.
#include "cube.h"
#include "bot/bot.h"
#include "radium_api.h"

extern "C" const radium_api_t *radium_api_ptr;
extern void fovchanged();
extern bool IsVisible(vec v1, vec v2, dynent *tracer, bool SkipTags);

// ---- the table (indices are mirrored by the constants in update/files/mods/ac.hls) ---------------------------------------
enum {
    A_SCRW = 0, A_SCRH, A_MS, A_INGAME, A_HEALTH, A_ARMOUR, A_GUN, A_AMMO, A_MAG, A_MAGSIZE,
    A_X, A_Y, A_Z, A_YAW, A_PITCH, A_SPEED, A_ONFLOOR, A_FRAGS, A_DEATHS, A_TEAMMODE, A_TEAM, A_FOV,
    A_NPL = 30,
    A_PL = 32, PL_STRIDE = 10, PL_MAX = 24,
    OUT_HEALTH = 300, OUT_ARMOUR, OUT_AMMO_FULL, OUT_RAPID, OUT_AIM, OUT_AIM_YAW, OUT_AIM_PITCH, OUT_FIRE,
    OUT_SPEED, OUT_JUMP, OUT_GRAV, OUT_FLY, OUT_TIMESCALE, OUT_FOV, OUT_SAT, OUT_CON, OUT_VIG, OUT_SCAN,
    OUT_TINT_R, OUT_TINT_G, OUT_TINT_B, OUT_TINT_A,
    API_SIZE = 340
};
// per player entry: 0 forward, 1 right, 2 up (all x16), 3 distance x16, 4 yaw to it x10, 5 pitch to it x10, 6 health, 7 enemy (1) or team mate (0), 8 visible

static int g_api[API_SIZE];
static bool g_init = false;

static const radium_api_t *A() { return radium_api_ptr; }

static void defaults()
{
    memset(g_api, 0, sizeof(g_api));
    g_api[OUT_HEALTH] = g_api[OUT_ARMOUR] = -1;
    g_api[OUT_SAT] = g_api[OUT_CON] = 100;
}

static int x16(float v) { return (int)floorf(v * 16.0f + 0.5f); }

static void fill_inputs(int w, int h)
{
    int *a = g_api;
    a[A_SCRW] = w; a[A_SCRH] = h; a[A_MS] = (int)A()->millis();
    playerent *p = player1;
    bool live = p && p->state == CS_ALIVE && !intermission;
    a[A_INGAME] = live ? 1 : 0;
    a[A_NPL] = 0;
    if(!p) return;
    a[A_HEALTH] = p->health; a[A_ARMOUR] = p->armour; a[A_GUN] = p->gunselect;
    a[A_AMMO] = p->ammo[p->gunselect]; a[A_MAG] = p->mag[p->gunselect]; a[A_MAGSIZE] = guns[p->gunselect].magsize;
    a[A_X] = x16(p->o.x); a[A_Y] = x16(p->o.y); a[A_Z] = x16(p->o.z);
    a[A_YAW] = (int)floorf(p->yaw * 10.0f + 0.5f); a[A_PITCH] = (int)floorf(p->pitch * 10.0f + 0.5f);
    a[A_SPEED] = (int)(sqrtf(p->vel.x * p->vel.x + p->vel.y * p->vel.y) * 100.0f);
    a[A_ONFLOOR] = p->onfloor ? 1 : 0;
    a[A_FRAGS] = p->frags; a[A_DEATHS] = p->deaths; a[A_TEAMMODE] = m_teammode ? 1 : 0; a[A_TEAM] = p->team;
    extern float fov;
    a[A_FOV] = (int)(fov + 0.5f);
    if(!live) return;

    const float sy = sinf(RAD * p->yaw), cy = cosf(RAD * p->yaw);
    int n = 0;
    for(int pass = 0; pass < 2; pass++)
    {
        int count = pass ? bots.length() : players.length();
        for(int i = 0; i < count && n < PL_MAX; i++)
        {
            playerent *d = pass ? (playerent *)bots[i] : players[i];
            if(!d || d == p || d->state != CS_ALIVE) continue;
            vec t = d->o;
            t.z -= d->eyeheight * 0.3f;                        // the chest, not the eyes
            float dx = t.x - p->o.x, dy = t.y - p->o.y, dz = t.z - p->o.z;
            float dxy = sqrtf(dx * dx + dy * dy);
            int *e = &a[A_PL + n * PL_STRIDE];
            e[0] = x16(dx * sy - dy * cy); e[1] = x16(dx * cy + dy * sy); e[2] = x16(dz);
            e[3] = x16(sqrtf(dxy * dxy + dz * dz));
            float yaw = atan2f(dx, -dy) / RAD; if(yaw < 0) yaw += 360.0f;
            e[4] = (int)floorf(yaw * 10.0f + 0.5f); e[5] = (int)floorf(atan2f(dz, dxy) / RAD * 10.0f + 0.5f);
            e[6] = d->health;
            e[7] = isteam(d->team, p->team) ? 0 : 1;
            e[8] = IsVisible(p->o, t, NULL, false) ? 1 : 0;
            n++;
        }
    }
    a[A_NPL] = n;
}

// ---- image effects on the frame, from the outputs of the previous frame -----------------------------------------------------
static unsigned char *g_vig = NULL; static int g_vigw = 0, g_vigh = 0, g_vigs = -1;

static void effects(uint32_t *fb, int w, int h)
{
    const int sat = g_api[OUT_SAT], con = g_api[OUT_CON], vig = g_api[OUT_VIG], scan = g_api[OUT_SCAN], ta = g_api[OUT_TINT_A];
    if(sat == 100 && con == 100 && vig <= 0 && scan <= 0 && ta <= 0) return;

    if(vig > 0 && (g_vigw != w || g_vigh != h || g_vigs != vig))
    {
        delete[] g_vig; g_vig = new unsigned char[w * h];
        g_vigw = w; g_vigh = h; g_vigs = vig;
        for(int y = 0; y < h; y++) for(int x = 0; x < w; x++)
        {
            float nx = (x - w * 0.5f) / (w * 0.5f), ny = (y - h * 0.5f) / (h * 0.5f);
            float d = (nx * nx + ny * ny) * 0.5f;                       // 0 in the middle .. 1 in the corners
            int f = 255 - (int)(d * d * vig * 2.55f); g_vig[y * w + x] = (unsigned char)(f < 0 ? 0 : f);
        }
    }
    unsigned char lut[256];
    for(int i = 0; i < 256; i++) { int v = (i - 128) * con / 100 + 128; lut[i] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v)); }
    const int s256 = sat * 256 / 100, tr = g_api[OUT_TINT_R] * ta / 100, tg = g_api[OUT_TINT_G] * ta / 100, tb = g_api[OUT_TINT_B] * ta / 100;

    for(int y = 0; y < h; y++)
    {
        uint32_t *row = fb + (size_t)y * w;
        const bool dark = scan > 0 && (y & 1);
        const unsigned char *vr = vig > 0 ? g_vig + (size_t)y * w : NULL;
        for(int x = 0; x < w; x++)
        {
            uint32_t c = row[x];
            int r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
            if(sat != 100) { int gr = (r * 77 + g * 151 + b * 28) >> 8; r = gr + ((r - gr) * s256 >> 8); g = gr + ((g - gr) * s256 >> 8); b = gr + ((b - gr) * s256 >> 8); r = r < 0 ? 0 : (r > 255 ? 255 : r); g = g < 0 ? 0 : (g > 255 ? 255 : g); b = b < 0 ? 0 : (b > 255 ? 255 : b); }
            if(con != 100) { r = lut[r]; g = lut[g]; b = lut[b]; }
            if(ta > 0) { r += tr; g += tg; b += tb; if(r > 255) r = 255; if(g > 255) g = 255; if(b > 255) b = 255; r = r * (100 - ta / 2) / 100; g = g * (100 - ta / 2) / 100; b = b * (100 - ta / 2) / 100; }
            int f = vr ? vr[x] : 256;
            if(dark) f = f * (100 - scan) / 100;
            if(f != 256) { r = r * f >> 8; g = g * f >> 8; b = b * f >> 8; }
            row[x] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        }
    }
}

// ---- applying what the mods asked for ------------------------------------------------------------------------------------------
static void apply_outputs()
{
    playerent *p = player1;
    if(!p) return;
    int *a = g_api;
    const bool live = p->state == CS_ALIVE && !intermission;

    if(live)
    {
        if(a[OUT_HEALTH] >= 0) p->health = a[OUT_HEALTH];
        if(a[OUT_ARMOUR] >= 0) p->armour = a[OUT_ARMOUR];
        if(a[OUT_AMMO_FULL])
        {
            int owned[4] = { GUN_PISTOL, p->primary, GUN_GRENADE, p->akimbo ? (int)GUN_AKIMBO : (int)GUN_PISTOL };
            loopi(4) { int g = owned[i]; if(!valid_weapon(g) || g == GUN_KNIFE) continue; if(g != GUN_GRENADE) p->mag[g] = guns[g].magsize; p->ammo[g] = ammostats[g].max; }
        }
        if(a[OUT_RAPID]) p->gunwait[p->gunselect] = 0;
        if(a[OUT_AIM])
        {
            p->yaw = a[OUT_AIM_YAW] / 10.0f; p->pitch = a[OUT_AIM_PITCH] / 10.0f;
            if(p->pitch > 89.0f) p->pitch = 89.0f; if(p->pitch < -89.0f) p->pitch = -89.0f;
        }
    }
    a[OUT_HEALTH] = a[OUT_ARMOUR] = -1; a[OUT_AIM] = 0;

    static bool firing = false;
    if((a[OUT_FIRE] != 0) != firing) { firing = a[OUT_FIRE] != 0; attack(firing); }

    // movement: AC kills the client when maxspeed is changed, so the extra distance is added after the physics step instead
    static vec prev; static bool prevfloor = false, havePrev = false;
    const int spd = a[OUT_SPEED] ? a[OUT_SPEED] : 100, jmp = a[OUT_JUMP] ? a[OUT_JUMP] : 100, grav = a[OUT_GRAV] ? a[OUT_GRAV] : 100;
    if(live)
    {
        if(havePrev && spd != 100 && p->onfloor && !a[OUT_FLY])
        {
            vec moved(p->o.x - prev.x, p->o.y - prev.y, 0);
            if(moved.magnitudexy() > 0.0001f && moved.magnitudexy() < 3.0f)
            {
                const float k = spd / 100.0f - 1.0f;
                vec old = p->o;
                p->o.x += moved.x * k; p->o.y += moved.y * k;
                if(collide(p, false, 0, 0)) p->o = old;
            }
        }
        if(prevfloor && p->vel.z > 1.9f && p->vel.z < 2.1f && jmp != 100) p->vel.z = 2.0f * jmp / 100.0f;
        if(grav != 100 && !p->onfloor) p->timeinair = (int)(p->timeinair * grav / 100.0f);
        if(a[OUT_FLY])
        {
            p->vel.z = p->move ? sinf(RAD * p->pitch) * p->move * 1.0f : 0.0f;
            if(p->jumpnext) { p->vel.z = 1.0f; p->jumpnext = false; }
            p->timeinair = 0;
        }
        prev = p->o; prevfloor = p->onfloor; havePrev = true;
    }
    else havePrev = false;

    static int origspeed = 0;
    const int ts = a[OUT_TIMESCALE] ? a[OUT_TIMESCALE] : 100;
    if(ts != 100) { if(!origspeed) origspeed = 100; setvar("gamespeed", ts, true); }
    else if(origspeed) { setvar("gamespeed", 100, true); origspeed = 0; }

    static float origfov = 0;
    if(a[OUT_FOV] >= 40)
    {
        extern float fov;
        if(!origfov) origfov = fov;
        if((int)(fov + 0.5f) != a[OUT_FOV]) setfvar("fov", (float)min(a[OUT_FOV], 120), true);
    }
    else if(origfov) { setfvar("fov", origfov, true); origfov = 0; }
}

extern "C" void acmods_frame(uint32_t *fb, int w, int h)
{
    const radium_api_t *api = A();
    if(!api || !api->mod_run) return;
    if(!g_init) { defaults(); g_init = true; }
    fill_inputs(w, h);
    effects(fb, w, h);
    api->mod_run(fb, (uint32_t *)g_api);
    apply_outputs();
}

// ---- the menu ---------------------------------------------------------------------------------------------------------------------
// one checkbox per mod; its state lives in the alias hmodonN so the menu stays open while a mod is switched
static void setonalias(int i, int on)
{
    defformatstring(name)("hmodon%d", i);
    alias(name, on ? "1" : "0");
}

static void hmodtoggle(int *idx)
{
    const radium_api_t *api = A();
    if(!api || !api->mod_toggle) return;
    static bool scanned = false;
    if(!scanned) { api->mod_scan(); scanned = true; }
    int r = api->mod_toggle(*idx);
    if(r < 0) conoutf("\f3mod could not be built (run: helios build /disk/mods/FILE)");
    setonalias(*idx, r > 0);
}
COMMAND(hmodtoggle, "i");

static void hmods()
{
    const radium_api_t *api = A();
    if(!api || !api->mod_scan) { conoutf("\f3no mods available"); return; }
    int n = api->mod_scan();
    execute("delmenu [HELIOS MODS]");
    execute("newmenu [HELIOS MODS]");
    if(!n) execute("menuitem [no mods found in /disk/mods (run: make update-disk)] []");
    for(int i = 0; i < n && i < 30; i++)
    {
        char name[32], desc[48]; int on = 0;
        if(!api->mod_info(i, name, desc, &on)) break;
        setonalias(i, on);
        defformatstring(cmd)("menuitemcheckbox [%s] [$hmodon%d] [hmodtoggle %d]", desc, i, i);
        execute(cmd);
    }
    execute("showmenu [HELIOS MODS]");
}
COMMAND(hmods, "");
