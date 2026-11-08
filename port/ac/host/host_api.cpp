// Host (Linux x86-64) test build of the AssaultCube port: the same engine + softgl.cpp + sdl_radium.cpp, with a fake radium_api_t that
// writes the picture to /tmp/achost/frameN.ppm and quits after a number of frames. Much faster to debug than a QEMU run.
//   make -C port/ac host && (cd port/ac/assaultcube && /tmp/achost/ac_host)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include "../../rt/radium_api.h"

extern "C" { const radium_api_t *radium_api_ptr; }

static uint32_t now_ms() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }
static int g_frame = 0, g_secs = 20;
static uint32_t g_t0 = 0;

static int gopen(uint32_t, uint32_t) { return 0; }
static void gclose(void) {}
static void gpresent(const uint32_t *px, uint32_t w, uint32_t h)
{
    ++g_frame;
    if (!g_t0) g_t0 = now_ms();
    uint32_t el = now_ms() - g_t0;
    static int dumped = 0;
    usleep(4000);
    static int extra = 0;
    if (const char *d = getenv("AC_HOST_DUMP")) {   // "9,12,15": also write dN.ppm at those seconds
        int idx = 0, t; const char *p = d;
        while (sscanf(p, "%d", &t) == 1) {
            if (idx == extra && el >= (uint32_t)t * 1000) {
                char name[64]; snprintf(name, sizeof(name), "/tmp/achost/d%d.ppm", extra++);
                FILE *f = fopen(name, "wb"); fprintf(f, "P6\n%u %u\n255\n", w, h);
                for (uint32_t i = 0; i < w * h; i++) { fputc(px[i] >> 16, f); fputc(px[i] >> 8, f); fputc(px[i], f); }
                fclose(f);
            }
            idx++; p = strchr(p, ','); if (!p) break; p++;
        }
    }
    if ((dumped == 0 && el > (uint32_t)g_secs * 500) || el > (uint32_t)g_secs * 1000) {
        char name[64]; snprintf(name, sizeof(name), "/tmp/achost/frame%d.ppm", dumped++);
        FILE *f = fopen(name, "wb");
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (uint32_t i = 0; i < w * h; i++) { fputc(px[i] >> 16, f); fputc(px[i] >> 8, f); fputc(px[i], f); }
        fclose(f);
        if (dumped == 2) { fprintf(stderr, "host: %d frames in %us, stopping\n", g_frame, el / 1000); exit(0); }
    }
}
// AC_HOST_KEYS="down,ret,ret": one key every 700 ms starting at 4 s, to drive the menus without a keyboard
// AC_HOST_INPUT="wm:10:16": hold W (w), move the mouse (m) or both (wm) from second 10 to second 16
static const char *in_mode() { static const char *e = getenv("AC_HOST_INPUT"); return e; }
static bool in_active(char which)
{
    const char *e = in_mode(); if (!e) return false;
    char mode[8]; int t0 = 0, t1 = 0;
    if (sscanf(e, "%7[^:]:%d:%d", mode, &t0, &t1) != 3 || !strchr(mode, which)) return false;
    uint32_t el = g_t0 ? now_ms() - g_t0 : 0;
    return el >= (uint32_t)t0 * 1000 && el < (uint32_t)t1 * 1000;
}
static int key_poll(void)
{
    static int q[16], qn = 0, qi = 0;
    static bool wdown = false;
    if (in_active('w') != wdown) { wdown = !wdown; return wdown ? 0x11 : 0x91; }
    static uint32_t last = 0;
    static const char *keys = getenv("AC_HOST_KEYS");
    if (qi < qn) return q[qi++];
    if (!keys || !*keys) return -1;
    uint32_t now = now_ms();
    if (!last) last = now + 4000;
    if (now < last) return -1;
    last = now + 700;
    qn = qi = 0;
    const char *e = strchr(keys, ',');
    size_t n = e ? (size_t)(e - keys) : strlen(keys);
    if (!strncmp(keys, "down", n)) { q[qn++] = 0xE0; q[qn++] = 0x50; q[qn++] = 0xE0; q[qn++] = 0xD0; }
    else if (!strncmp(keys, "up", n)) { q[qn++] = 0xE0; q[qn++] = 0x48; q[qn++] = 0xE0; q[qn++] = 0xC8; }
    else if (!strncmp(keys, "ret", n)) { q[qn++] = 0x1C; q[qn++] = 0x9C; }
    else if (!strncmp(keys, "esc", n)) { q[qn++] = 0x01; q[qn++] = 0x81; }
    keys = e ? e + 1 : "";
    return qn ? q[qi++] : -1;
}
static uint32_t millis(void) { return now_ms(); }
static void sleep_ms(uint32_t ms) { usleep(ms * 1000); }
static int mouse_poll(int *dx, int *dy, int *b)
{
    static uint32_t last = 0;
    *dx = *dy = *b = 0;
    uint32_t now = now_ms();
    { uint32_t el = g_t0 ? now - g_t0 : 0; static int lastb = 0; int cb = in_mode() && strchr(in_mode(), 'c') && el > 8000 && el < 8300; *b = cb; int ch = cb != lastb; lastb = cb; if (ch) return 1; }
    if (in_active('m') && last && now > last) { *dx = (int)(now - last) / 4; last = now - (now - last) % 4; return *dx != 0; }
    last = now;
    return 0;
}
static void dbg(const char *s, uint32_t n) { fwrite(s, 1, n, stderr); }
static uint32_t rnd(void) { return (uint32_t)rand(); }
static uint32_t unix_time(void) { return (uint32_t)time(NULL); }

static radium_api_t g_api;

// the engine's real main (renamed ac_main) is reached through launch.cpp's main(); this constructor installs the fake API first
__attribute__((constructor)) static void install()
{
    g_api.millis = millis; g_api.sleep_ms = sleep_ms; g_api.key_poll = key_poll; g_api.mouse_poll = mouse_poll;
    g_api.gfx_open = gopen; g_api.gfx_close = gclose; g_api.gfx_present = gpresent; g_api.debug_write = dbg;
    g_api.random32 = rnd; g_api.unix_time = unix_time;
    radium_api_ptr = &g_api;
    if (getenv("AC_HOST_SECONDS")) g_secs = atoi(getenv("AC_HOST_SECONDS"));
}
