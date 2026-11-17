// sdl_radium.cpp - the parts of SDL2 and SDL_image that AssaultCube uses, on top of radium_api_t (port/rt/radium_api.h):
// one fixed window drawn by softgl.cpp, PS/2 keyboard/mouse events, no threads, no audio device. Header files are the SDL2 ones
// that ship in AssaultCube's source/include.

#include <SDL.h>
#include <SDL_image.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#include "third_party/stb_image.h"

#include "../rt/radium_api.h"

extern "C" const radium_api_t *radium_api_ptr;
extern "C" void softgl_attach(unsigned int *pixels, int w, int h);
extern "C" int softgl_stats(char *buf, int n);
extern "C" int softgl_prof(char *buf, int n);
extern "C" void acmods_frame(uint32_t *fb, int w, int h);
extern "C" void softgl_debug(int calls);

static const radium_api_t *A() { return radium_api_ptr; }

struct SDL_Window { int w, h; uint32_t *fb; };

static char g_err[128] = "";
static SDL_Window g_win;
static bool g_gfx = false, g_relmouse = false, g_textinput = false;
static int g_mx = 0, g_my = 0;
static uint8_t g_keys[SDL_NUM_SCANCODES];
static uint32_t g_mod = 0;

// ---- core ---------------------------------------------------------------------------------------------------------------

extern "C" {

int SDL_Init(Uint32) { return 0; }
int SDL_InitSubSystem(Uint32) { return 0; }
void SDL_Quit(void) { if (g_gfx) { A()->gfx_close(); g_gfx = false; } }
const char *SDL_GetError(void) { return g_err; }
void SDL_ClearError(void) { g_err[0] = 0; }
void SDL_GetVersion(SDL_version *v) { v->major = 2; v->minor = 0; v->patch = 22; }
SDL_bool SDL_SetHintWithPriority(const char *, const char *, SDL_HintPriority) { return SDL_TRUE; }
void SDL_Delay(Uint32 ms) { A()->sleep_ms(ms); }
Uint32 SDL_GetTicks(void) { return A()->millis(); }
void SDL_free(void *p) { free(p); }
char *SDL_GetClipboardText(void) { return (char *)calloc(1, 1); }
int SDL_SetClipboardText(const char *) { return 0; }

// ---- window and GL ----------------------------------------------------------------------------------------------------

SDL_Window *SDL_CreateWindow(const char *, int, int, int w, int h, Uint32)
{
    if (w < 64) w = 64;
    if (h < 64) h = 64;
    if (w > 1280) w = 1280;
    if (h > 720) h = 720;
    free(g_win.fb);
    g_win.w = w; g_win.h = h;
    g_win.fb = (uint32_t *)calloc((size_t)w * h, 4);
    if (!g_gfx) { A()->gfx_open((uint32_t)w, (uint32_t)h); g_gfx = true; }
    softgl_attach(g_win.fb, w, h);
    g_mx = w / 2; g_my = h / 2;
    return &g_win;
}
void SDL_DestroyWindow(SDL_Window *) {}
void SDL_ShowWindow(SDL_Window *) {}
void SDL_HideWindow(SDL_Window *) {}
void SDL_SetWindowSize(SDL_Window *win, int w, int h)
{
    if (w > 1280) w = 1280;
    if (h > 720) h = 720;
    win->w = w; win->h = h;
    free(win->fb);
    win->fb = (uint32_t *)calloc((size_t)w * h, 4);
    softgl_attach(win->fb, w, h);
}
void SDL_SetWindowPosition(SDL_Window *, int, int) {}
void SDL_GetWindowSize(SDL_Window *win, int *w, int *h) { if (w) *w = win->w; if (h) *h = win->h; }
Uint32 SDL_GetWindowFlags(SDL_Window *) { return SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS; }
int SDL_SetWindowFullscreen(SDL_Window *, Uint32) { return 0; }
void SDL_SetWindowGrab(SDL_Window *, SDL_bool) {}
void SDL_SetWindowIcon(SDL_Window *, SDL_Surface *) {}
int SDL_SetWindowBrightness(SDL_Window *, float) { return 0; }
int SDL_GetNumDisplayModes(int) { return 3; }
int SDL_GetDisplayMode(int, int i, SDL_DisplayMode *m)
{
    static const int sizes[3][2] = { { 640, 360 }, { 960, 540 }, { 1280, 720 } };
    if (i < 0 || i > 2) return -1;
    m->format = SDL_PIXELFORMAT_RGB888; m->w = sizes[i][0]; m->h = sizes[i][1]; m->refresh_rate = 60; m->driverdata = NULL;
    return 0;
}
int SDL_GetDisplayBounds(int, SDL_Rect *r) { r->x = r->y = 0; r->w = 1280; r->h = 720; return 0; }

int SDL_GL_SetAttribute(SDL_GLattr, int) { return 0; }
SDL_GLContext SDL_GL_CreateContext(SDL_Window *) { return (SDL_GLContext)&g_win; }
void SDL_GL_DeleteContext(SDL_GLContext) {}
int SDL_GL_SetSwapInterval(int) { return 0; }
void *SDL_GL_GetProcAddress(const char *) { return NULL; }
void SDL_GL_SwapWindow(SDL_Window *win)
{
    static unsigned t_mods = 0, t_present = 0, t_start = 0;
    unsigned t0 = A()->millis();
    if (!t_start) t_start = t0;
    acmods_frame(win->fb, win->w, win->h);
    unsigned t1 = A()->millis();
    A()->gfx_present(win->fb, (uint32_t)win->w, (uint32_t)win->h);
    unsigned t2 = A()->millis();
    t_mods += t1 - t0; t_present += t2 - t1;
    static int frames = 0;
    if (getenv("SOFTGL_DEBUG_FRAME") && frames == atoi(getenv("SOFTGL_DEBUG_FRAME"))) softgl_debug(60);
    if (++frames % 120 == 0) {
        char b[320]; int n = snprintf(b, sizeof b, "frames: 120 in %ums, mods %ums, present %ums | ", t2 - t_start, t_mods, t_present);
        n += softgl_stats(b + n, (int)sizeof(b) - n - 2); b[n++] = '\n'; A()->debug_write(b, (uint32_t)n);
        { char pb[400]; int pn = softgl_prof(pb, sizeof pb - 2); pb[pn++] = '\n'; A()->debug_write(pb, (uint32_t)pn); }
        t_mods = t_present = 0; t_start = t2;
    }
}

// ---- input ------------------------------------------------------------------------------------------------------------

static const uint8_t SC1[0x59] = {   // PS/2 set 1 make code -> SDL scancode
    0, 41, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 45, 46, 42, 43,
    20, 26, 8, 21, 23, 28, 24, 12, 18, 19, 47, 48, 40, 224, 4, 22,
    7, 9, 10, 11, 13, 14, 15, 51, 52, 53, 225, 49, 29, 27, 6, 25,
    5, 17, 16, 54, 55, 56, 229, 85, 226, 44, 57, 58, 59, 60, 61, 62,
    63, 64, 65, 66, 67, 83, 71, 95, 96, 97, 86, 92, 93, 94, 87, 89,
    90, 91, 98, 99, 0, 0, 0, 68, 69,
};

static int ext_scancode(int make)
{
    switch (make) {
    case 0x1C: return 88; case 0x1D: return 228; case 0x35: return 84; case 0x38: return 230;
    case 0x47: return 74; case 0x48: return 82; case 0x49: return 75; case 0x4B: return 80; case 0x4D: return 79;
    case 0x4F: return 77; case 0x50: return 81; case 0x51: return 78; case 0x52: return 73; case 0x53: return 76;
    case 0x5B: return 227; case 0x5C: return 231;
    default: return 0;
    }
}

static SDL_Keycode keycode_of(int sc)
{
    if (sc >= 4 && sc <= 29) return 'a' + (sc - 4);
    if (sc >= 30 && sc <= 38) return '1' + (sc - 30);
    switch (sc) {
    case 39: return '0'; case 40: return '\r'; case 41: return 27; case 42: return 8; case 43: return 9; case 44: return ' ';
    case 45: return '-'; case 46: return '='; case 47: return '['; case 48: return ']'; case 49: return '\\';
    case 51: return ';'; case 52: return '\''; case 53: return '`'; case 54: return ','; case 55: return '.'; case 56: return '/';
    default: return (SDL_Keycode)(sc | SDLK_SCANCODE_MASK);
    }
}

static char text_of(SDL_Keycode k, bool shift, bool caps)
{
    if (k >= 'a' && k <= 'z') return shift != caps ? (char)(k - 32) : (char)k;
    if (k >= '0' && k <= '9') { static const char s[] = ")!@#$%^&*("; return shift ? s[k - '0'] : (char)k; }
    if (!shift) return (k == ' ' || k == '-' || k == '=' || k == '[' || k == ']' || k == '\\' || k == ';' || k == '\'' || k == '`' || k == ',' || k == '.' || k == '/') ? (char)k : 0;
    switch (k) {
    case ' ': return ' '; case '-': return '_'; case '=': return '+'; case '[': return '{'; case ']': return '}'; case '\\': return '|';
    case ';': return ':'; case '\'': return '"'; case '`': return '~'; case ',': return '<'; case '.': return '>'; case '/': return '?';
    default: return 0;
    }
}

static SDL_Event g_q[64];
static int g_qh = 0, g_qt = 0;
static void push(const SDL_Event &e) { int n = (g_qh + 1) & 63; if (n != g_qt) { g_q[g_qh] = e; g_qh = n; } }

static void set_mod(int sc, bool down)
{
    uint32_t bit = sc == 225 ? KMOD_LSHIFT : sc == 229 ? KMOD_RSHIFT : sc == 224 ? KMOD_LCTRL : sc == 228 ? KMOD_RCTRL : sc == 226 ? KMOD_LALT : sc == 230 ? KMOD_RALT : sc == 227 ? KMOD_LGUI : sc == 231 ? KMOD_RGUI : 0;
    if (down) g_mod |= bit; else g_mod &= ~bit;
}

static void pump()
{
    static bool ext = false;
    int c;
    while ((c = A()->key_poll()) >= 0) {
        if (c == 0xE0) { ext = true; continue; }
        if (c == 0xE1) continue;
        bool down = !(c & 0x80);
        int make = c & 0x7F, sc = ext ? ext_scancode(make) : (make < 0x59 ? SC1[make] : 0);
        ext = false;
        if (!sc) continue;
        bool repeat = down && g_keys[sc];
        g_keys[sc] = down;
        set_mod(sc, down);
        if (down && !repeat && sc == SDL_SCANCODE_CAPSLOCK) g_mod ^= KMOD_CAPS;
        SDL_Event e; memset(&e, 0, sizeof(e));
        e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
        e.key.repeat = repeat;
        e.key.keysym.scancode = (SDL_Scancode)sc;
        e.key.keysym.sym = keycode_of(sc);
        e.key.keysym.mod = (Uint16)g_mod;
        push(e);
        if (down && g_textinput && !(g_mod & (KMOD_CTRL | KMOD_ALT))) {
            char ch = text_of(keycode_of(sc), (g_mod & KMOD_SHIFT) != 0, (g_mod & KMOD_CAPS) != 0);
            if (ch) { SDL_Event t; memset(&t, 0, sizeof(t)); t.type = SDL_TEXTINPUT; t.text.timestamp = A()->millis(); t.text.text[0] = ch; push(t); }
        }
    }
    int dx, dy, btn;
    static int lastbtn = 0;
    if (A()->mouse_poll(&dx, &dy, &btn)) {
        if (dx || dy) {
            g_mx += dx; g_my += dy;
            if (g_mx < 0) g_mx = 0; if (g_mx >= g_win.w) g_mx = g_win.w - 1;
            if (g_my < 0) g_my = 0; if (g_my >= g_win.h) g_my = g_win.h - 1;
            SDL_Event e; memset(&e, 0, sizeof(e));
            e.type = SDL_MOUSEMOTION; e.motion.x = g_mx; e.motion.y = g_my; e.motion.xrel = dx; e.motion.yrel = dy;
            push(e);
        }
        for (int b = 0; b < 3; b++) {
            int mask = 1 << b, was = lastbtn & mask, is = btn & mask;
            if (was == is) continue;
            SDL_Event e; memset(&e, 0, sizeof(e));
            e.type = is ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
            e.button.button = b == 0 ? SDL_BUTTON_LEFT : (b == 1 ? SDL_BUTTON_RIGHT : SDL_BUTTON_MIDDLE);
            e.button.state = is ? SDL_PRESSED : SDL_RELEASED; e.button.x = g_mx; e.button.y = g_my; e.button.clicks = 1;
            push(e);
        }
        lastbtn = btn;
    }
}

int SDL_PollEvent(SDL_Event *ev)
{
    if (g_qh == g_qt) pump();
    if (g_qh == g_qt) return 0;
    if (ev) *ev = g_q[g_qt];
    g_qt = (g_qt + 1) & 63;
    return 1;
}
const Uint8 *SDL_GetKeyboardState(int *n) { if (n) *n = SDL_NUM_SCANCODES; return g_keys; }
SDL_Keymod SDL_GetModState(void) { return (SDL_Keymod)g_mod; }
int SDL_ShowCursor(int) { return 0; }
int SDL_SetRelativeMouseMode(SDL_bool on) { g_relmouse = on != SDL_FALSE; return 0; }
void SDL_WarpMouseInWindow(SDL_Window *, int, int) {}
void SDL_StartTextInput(void) { g_textinput = true; }
void SDL_StopTextInput(void) { g_textinput = false; }

const char *SDL_GetScancodeName(SDL_Scancode sc)
{
    static char buf[2][16]; static int flip = 0;
    char *b = buf[flip ^= 1];
    SDL_Keycode k = keycode_of((int)sc);
    if (sc >= 4 && sc <= 29) { b[0] = (char)('A' + (sc - 4)); b[1] = 0; return b; }
    if (sc >= 30 && sc <= 39) { b[0] = (char)(sc == 39 ? '0' : '1' + (sc - 30)); b[1] = 0; return b; }
    if (sc >= 58 && sc <= 69) { snprintf(b, 16, "F%d", (int)sc - 57); return b; }
    switch (sc) {
    case SDL_SCANCODE_RETURN: return "Return"; case SDL_SCANCODE_ESCAPE: return "Escape"; case SDL_SCANCODE_BACKSPACE: return "Backspace";
    case SDL_SCANCODE_TAB: return "Tab"; case SDL_SCANCODE_SPACE: return "Space"; case SDL_SCANCODE_LCTRL: return "Left Ctrl";
    case SDL_SCANCODE_LSHIFT: return "Left Shift"; case SDL_SCANCODE_LALT: return "Left Alt"; case SDL_SCANCODE_RCTRL: return "Right Ctrl";
    case SDL_SCANCODE_RSHIFT: return "Right Shift"; case SDL_SCANCODE_RALT: return "Right Alt"; case SDL_SCANCODE_UP: return "Up";
    case SDL_SCANCODE_DOWN: return "Down"; case SDL_SCANCODE_LEFT: return "Left"; case SDL_SCANCODE_RIGHT: return "Right";
    case SDL_SCANCODE_INSERT: return "Insert"; case SDL_SCANCODE_DELETE: return "Delete"; case SDL_SCANCODE_HOME: return "Home";
    case SDL_SCANCODE_END: return "End"; case SDL_SCANCODE_PAGEUP: return "PageUp"; case SDL_SCANCODE_PAGEDOWN: return "PageDown";
    case SDL_SCANCODE_CAPSLOCK: return "CapsLock";
    default: break;
    }
    if (k < 128 && k > 32) { b[0] = (char)k; b[1] = 0; return b; }
    return "";
}
SDL_Scancode SDL_GetScancodeFromName(const char *name)
{
    for (int i = 1; i < SDL_NUM_SCANCODES; i++) { const char *n = SDL_GetScancodeName((SDL_Scancode)i); if (n[0] && !strcasecmp(n, name)) return (SDL_Scancode)i; }
    return SDL_SCANCODE_UNKNOWN;
}

// ---- threads, mutexes: one thread only -------------------------------------------------------------------------------

SDL_Thread *SDL_CreateThread(SDL_ThreadFunction, const char *, void *) { return NULL; }
void SDL_WaitThread(SDL_Thread *, int *status) { if (status) *status = 0; }
void SDL_DetachThread(SDL_Thread *) {}
SDL_threadID SDL_GetThreadID(SDL_Thread *) { return 1; }
SDL_threadID SDL_ThreadID(void) { return 1; }
SDL_mutex *SDL_CreateMutex(void) { return (SDL_mutex *)calloc(1, 8); }
void SDL_DestroyMutex(SDL_mutex *m) { free(m); }
int SDL_LockMutex(SDL_mutex *) { return 0; }
int SDL_UnlockMutex(SDL_mutex *) { return 0; }
SDL_cond *SDL_CreateCond(void) { return (SDL_cond *)calloc(1, 8); }
int SDL_CondSignal(SDL_cond *) { return 0; }
int SDL_CondWait(SDL_cond *, SDL_mutex *) { return 0; }
int SDL_CondWaitTimeout(SDL_cond *, SDL_mutex *, Uint32) { return SDL_MUTEX_TIMEDOUT; }
SDL_sem *SDL_CreateSemaphore(Uint32 v) { Uint32 *s = (Uint32 *)malloc(4); *s = v; return (SDL_sem *)s; }
void SDL_DestroySemaphore(SDL_sem *s) { free(s); }
int SDL_SemPost(SDL_sem *s) { ++*(Uint32 *)s; return 0; }
int SDL_SemTryWait(SDL_sem *s) { Uint32 *v = (Uint32 *)s; if (*v) { --*v; return 0; } return SDL_MUTEX_TIMEDOUT; }
int SDL_SemWait(SDL_sem *s) { SDL_SemTryWait(s); return 0; }
int SDL_SemWaitTimeout(SDL_sem *s, Uint32) { return SDL_SemTryWait(s); }
Uint32 SDL_SemValue(SDL_sem *s) { return *(Uint32 *)s; }

// ---- surfaces and images -----------------------------------------------------------------------------------------------

SDL_Surface *SDL_CreateRGBSurfaceFrom(void *pixels, int w, int h, int depth, int pitch, Uint32 r, Uint32 g, Uint32 b, Uint32 a)
{
    SDL_Surface *s = (SDL_Surface *)calloc(1, sizeof(SDL_Surface));
    SDL_PixelFormat *f = (SDL_PixelFormat *)calloc(1, sizeof(SDL_PixelFormat));
    f->BitsPerPixel = (Uint8)depth; f->BytesPerPixel = (Uint8)((depth + 7) / 8);
    f->Rmask = r; f->Gmask = g; f->Bmask = b; f->Amask = a;
    s->format = f; s->w = w; s->h = h; s->pitch = pitch; s->pixels = pixels;
    s->clip_rect.w = w; s->clip_rect.h = h; s->refcount = 1;
    return s;
}
SDL_Surface *SDL_CreateRGBSurface(Uint32, int w, int h, int depth, Uint32 r, Uint32 g, Uint32 b, Uint32 a)
{
    int bpp = (depth + 7) / 8, pitch = (w * bpp + 3) & ~3;
    void *px = calloc(1, (size_t)pitch * (h > 0 ? h : 1));
    return SDL_CreateRGBSurfaceFrom(px, w, h, depth, pitch, r, g, b, a);
}
void SDL_FreeSurface(SDL_Surface *s)
{
    if (!s) return;
    if (!(s->flags & SDL_PREALLOC)) free(s->pixels);
    free(s->format);
    free(s);
}
int SDL_GetColorKey(SDL_Surface *, Uint32 *) { return -1; }
int SDL_SetSurfaceAlphaMod(SDL_Surface *, Uint8) { return 0; }
int SDL_UpperBlit(SDL_Surface *src, const SDL_Rect *sr, SDL_Surface *dst, SDL_Rect *dr)
{
    SDL_Rect s = { 0, 0, src->w, src->h }, d = { 0, 0, 0, 0 };
    if (sr) s = *sr;
    if (dr) { d.x = dr->x; d.y = dr->y; }
    int bpp = src->format->BytesPerPixel;
    if (bpp != dst->format->BytesPerPixel) return -1;
    for (int y = 0; y < s.h; y++) {
        int sy = s.y + y, dy = d.y + y;
        if (sy < 0 || sy >= src->h || dy < 0 || dy >= dst->h) continue;
        int x0 = d.x < 0 ? -d.x : 0, n = s.w - x0;
        if (d.x + x0 + n > dst->w) n = dst->w - d.x - x0;
        if (n > 0) memcpy((Uint8 *)dst->pixels + dy * dst->pitch + (d.x + x0) * bpp, (Uint8 *)src->pixels + sy * src->pitch + (s.x + x0) * bpp, (size_t)n * bpp);
    }
    return 0;
}
int SDL_SaveBMP_RW(SDL_Surface *, SDL_RWops *, int) { return -1; }
SDL_AudioSpec *SDL_LoadWAV_RW(SDL_RWops *, int, SDL_AudioSpec *, Uint8 **, Uint32 *) { return NULL; }
void SDL_FreeWAV(Uint8 *) {}
SDL_RWops *SDL_AllocRW(void) { return (SDL_RWops *)calloc(1, sizeof(SDL_RWops)); }
void SDL_FreeRW(SDL_RWops *rw) { free(rw); }

static SDL_Surface *surface_from_memory(const unsigned char *data, int len)
{
    int w, h, n;
    unsigned char *px = stbi_load_from_memory(data, len, &w, &h, &n, 0);
    if (!px) { snprintf(g_err, sizeof(g_err), "%s", stbi_failure_reason()); return NULL; }
    if (n == 2) {   // grey + alpha -> RGBA
        unsigned char *o = (unsigned char *)malloc((size_t)w * h * 4);
        for (int i = 0; i < w * h; i++) { o[i * 4] = o[i * 4 + 1] = o[i * 4 + 2] = px[i * 2]; o[i * 4 + 3] = px[i * 2 + 1]; }
        stbi_image_free(px); px = o; n = 4;
    } else if (n == 1) {
        unsigned char *o = (unsigned char *)malloc((size_t)w * h * 3);
        for (int i = 0; i < w * h; i++) o[i * 3] = o[i * 3 + 1] = o[i * 3 + 2] = px[i];
        stbi_image_free(px); px = o; n = 3;
    }
    SDL_Surface *s = SDL_CreateRGBSurface(0, w, h, n * 8, 0x0000FF, 0x00FF00, 0xFF0000, n == 4 ? 0xFF000000u : 0);
    for (int y = 0; y < h; y++) memcpy((Uint8 *)s->pixels + y * s->pitch, px + (size_t)y * w * n, (size_t)w * n);
    free(px);
    return s;
}

SDL_Surface *IMG_Load_RW(SDL_RWops *rw, int freesrc)
{
    Sint64 size = rw->size ? rw->size(rw) : -1;
    if (size <= 0) { if (freesrc) rw->close(rw); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)size);
    rw->seek(rw, 0, RW_SEEK_SET);
    size_t got = rw->read(rw, buf, 1, (size_t)size);
    SDL_Surface *s = surface_from_memory(buf, (int)got);
    free(buf);
    if (freesrc) rw->close(rw);
    return s;
}
SDL_Surface *IMG_Load(const char *file)
{
    FILE *f = fopen(file, "rb");
    if (!f) { snprintf(g_err, sizeof(g_err), "cannot open %s", file); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    SDL_Surface *s = surface_from_memory(buf, (int)got);
    free(buf);
    return s;
}

}   // extern "C"
