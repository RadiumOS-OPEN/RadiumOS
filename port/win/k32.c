/* k32.c - the part of kernel32 that console and simple GUI programs use. One process, one thread: critical sections and
 * events do nothing, CreateThread fails, files are the app VFS (/disk) through musl. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <malloc.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include "winapi.h"

#define K32(n) { "kernel32", #n, (void *)n }

/* ---- errors, process, modules ------------------------------------------------------------------------------------------------ */

static DWORD WINAPI GetLastError(void) { extern uint8_t *win_teb; return *(DWORD *)(win_teb + 0x34); }
static void WINAPI SetLastError(DWORD e) { win_set_last_error(e); }

static void WINAPI ExitProcess(UINT code) { fflush(NULL); win_gui_shutdown(); exit((int)code); }
static BOOL WINAPI TerminateProcess(HANDLE h, UINT code) { (void)h; ExitProcess(code); return TRUE; }
static HANDLE WINAPI GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
static HANDLE WINAPI GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
static DWORD WINAPI GetCurrentProcessId(void) { return 1; }
static DWORD WINAPI GetCurrentThreadId(void) { return 1; }
static void *WINAPI SetUnhandledExceptionFilter(void *f) { (void)f; return NULL; }
static LONG WINAPI UnhandledExceptionFilter(void *p) { (void)p; return 0; }
static UINT WINAPI SetErrorMode(UINT m) { (void)m; return 0; }
static BOOL WINAPI IsDebuggerPresent(void) { return FALSE; }
static void WINAPI RaiseException(DWORD code, DWORD flags, DWORD n, const void *args) { (void)flags; (void)n; (void)args; fprintf(stderr, "winrun: unhandled Windows exception 0x%08X\n", code); fflush(NULL); exit(3); }
static void WINAPI DebugBreak(void) { fprintf(stderr, "winrun: DebugBreak\n"); exit(3); }
static DWORD WINAPI GetVersion(void) { return 0x1DB10106u; }       /* Windows 7 SP1 */

typedef struct { DWORD size, major, minor, build, platform; char csd[128]; } osversioninfoa;
static BOOL WINAPI GetVersionExA(osversioninfoa *v)
{
    v->major = 6; v->minor = 1; v->build = 7601; v->platform = 2; v->csd[0] = 0;
    return TRUE;
}

typedef struct { WORD arch, reserved; DWORD page, minaddr, maxaddr, mask, nproc, type, granularity; WORD level, rev; } sysinfo;
static void WINAPI GetSystemInfo(sysinfo *s)
{
    memset(s, 0, sizeof *s);
    s->page = 4096; s->minaddr = 0x10000; s->maxaddr = 0x7FFEFFFF; s->mask = 1; s->nproc = 1; s->type = 586; s->granularity = 0x10000;
}
typedef struct { DWORD len, load, totphys, availphys, totpage, availpage, totvirt, availvirt; } memstatus;
static void WINAPI GlobalMemoryStatus(memstatus *m)
{
    m->len = sizeof *m; m->load = 10; m->totphys = m->totpage = m->totvirt = 256u << 20; m->availphys = m->availpage = m->availvirt = 128u << 20;
}

static HMODULE WINAPI GetModuleHandleA(LPCSTR name)
{
    if (!name) return pe_main_base();
    pe_module_t *m = pe_find(name);
    if (!m) m = pe_builtin(name);
    return m ? m->base : NULL;
}
static HMODULE WINAPI GetModuleHandleW(LPCWSTR name)
{
    if (!name) return pe_main_base();
    char b[64]; int i = 0;
    for (; name[i] && i < 63; i++) b[i] = (char)name[i];
    b[i] = 0;
    return GetModuleHandleA(b);
}
static pe_module_t *by_handle(HMODULE h)
{
    if (!h) return NULL;
    extern pe_module_t *pe_all(void);
    for (pe_module_t *m = pe_all(); m; m = m->next) if (m->base == (uint8_t *)h) return m;
    return NULL;
}
static HMODULE WINAPI LoadLibraryA(LPCSTR name)
{
    pe_module_t *m = pe_find(name);
    if (!m) m = pe_builtin(name);
    if (!m) {
        char p[300];
        snprintf(p, sizeof p, "%s", win_path(name));
        if (!strrchr(p, '.')) strcat(p, ".dll");
        m = pe_load(p, 0);
    }
    if (m) { m->refs++; return m->base; }
    win_set_last_error(126);
    return NULL;
}
static HMODULE WINAPI LoadLibraryExA(LPCSTR name, HANDLE f, DWORD flags) { (void)f; (void)flags; return LoadLibraryA(name); }
static HMODULE WINAPI LoadLibraryW(LPCWSTR name)
{
    char b[260]; int i = 0;
    for (; name[i] && i < 259; i++) b[i] = (char)name[i];
    b[i] = 0;
    return LoadLibraryA(b);
}
static BOOL WINAPI FreeLibrary(HMODULE h) { pe_unload(by_handle(h)); return TRUE; }
static FARPROC WINAPI GetProcAddress(HMODULE h, LPCSTR name)
{
    pe_module_t *m = by_handle(h);
    if (!m) return NULL;
    char nm[24];
    if ((uintptr_t)name < 0x10000) { snprintf(nm, sizeof nm, "#%u", (unsigned)(uintptr_t)name); name = nm; }
    void *a = pe_get_proc(m, name);
    if (!a) win_set_last_error(127);
    return (FARPROC)a;
}
static DWORD WINAPI GetModuleFileNameA(HMODULE h, LPSTR buf, DWORD n)
{
    (void)h;
    extern const char *win_exe_path;
    strncpy(buf, win_exe_path, n);
    if (n) buf[n - 1] = 0;
    return (DWORD)strlen(buf);
}
static BOOL WINAPI DisableThreadLibraryCalls(HMODULE h) { (void)h; return TRUE; }

static LPSTR WINAPI GetCommandLineA(void) { return win_cmdline; }
static LPWSTR WINAPI GetCommandLineW(void)
{
    static WCHAR w[1024];
    int i = 0;
    for (; win_cmdline[i] && i < 1023; i++) w[i] = (WCHAR)(unsigned char)win_cmdline[i];
    w[i] = 0;
    return w;
}
static void WINAPI GetStartupInfoA(void *si) { memset(si, 0, 68); *(DWORD *)si = 68; }
static void WINAPI GetStartupInfoW(void *si) { memset(si, 0, 68); *(DWORD *)si = 68; }
static DWORD WINAPI GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD n)
{
    const char *v = getenv(name);
    if (!v) { win_set_last_error(203); return 0; }
    DWORD l = (DWORD)strlen(v);
    if (l + 1 > n) return l + 1;
    memcpy(buf, v, l + 1);
    return l;
}
static BOOL WINAPI SetEnvironmentVariableA(LPCSTR n, LPCSTR v) { return v ? setenv(n, v, 1) == 0 : unsetenv(n) == 0; }

/* ---- memory ------------------------------------------------------------------------------------------------------------------ */

static HANDLE WINAPI HeapCreate(DWORD o, SIZE_T i, SIZE_T m) { (void)o; (void)i; (void)m; return (HANDLE)0x00100000; }
static BOOL WINAPI HeapDestroy(HANDLE h) { (void)h; return TRUE; }
static HANDLE WINAPI GetProcessHeap(void) { return (HANDLE)0x00100000; }
static LPVOID WINAPI HeapAlloc(HANDLE h, DWORD flags, SIZE_T n) { (void)h; return flags & 8 ? calloc(1, n ? n : 1) : malloc(n ? n : 1); }
static BOOL WINAPI HeapFree(HANDLE h, DWORD flags, LPVOID p) { (void)h; (void)flags; free(p); return TRUE; }
static LPVOID WINAPI HeapReAlloc(HANDLE h, DWORD flags, LPVOID p, SIZE_T n)
{
    (void)h;
    size_t old = p ? malloc_usable_size(p) : 0;
    void *q = realloc(p, n ? n : 1);
    if (q && (flags & 8) && n > old) memset((uint8_t *)q + old, 0, n - old);
    return q;
}
static SIZE_T WINAPI HeapSize(HANDLE h, DWORD flags, LPCVOID p) { (void)h; (void)flags; return p ? malloc_usable_size((void *)p) : 0; }
static HANDLE WINAPI LocalAlloc(UINT flags, SIZE_T n) { return flags & 0x40 ? calloc(1, n ? n : 1) : malloc(n ? n : 1); }
static HANDLE WINAPI LocalFree(HANDLE p) { free(p); return NULL; }
static HANDLE WINAPI GlobalAlloc(UINT flags, SIZE_T n) { return flags & 0x40 ? calloc(1, n ? n : 1) : malloc(n ? n : 1); }
static HANDLE WINAPI GlobalFree(HANDLE p) { free(p); return NULL; }
static LPVOID WINAPI GlobalLock(HANDLE p) { return p; }
static BOOL WINAPI GlobalUnlock(HANDLE p) { (void)p; return FALSE; }
static SIZE_T WINAPI GlobalSize(HANDLE p) { return p ? malloc_usable_size(p) : 0; }

static struct { uint8_t *p; SIZE_T n; } g_valloc[64];
static LPVOID WINAPI VirtualAlloc(LPVOID addr, SIZE_T n, DWORD type, DWORD prot)
{
    (void)prot;
    if (addr) {                                       /* committing inside something reserved earlier */
        for (int i = 0; i < 64; i++) if (g_valloc[i].p && (uint8_t *)addr >= g_valloc[i].p && (uint8_t *)addr < g_valloc[i].p + g_valloc[i].n) return addr;
        return NULL;
    }
    (void)type;
    SIZE_T sz = (n + 0xFFF) & ~0xFFFu;
    uint8_t *p = memalign(4096, sz ? sz : 4096);
    if (!p) { win_set_last_error(8); return NULL; }
    memset(p, 0, sz);
    for (int i = 0; i < 64; i++) if (!g_valloc[i].p) { g_valloc[i].p = p; g_valloc[i].n = sz; break; }
    return p;
}
static BOOL WINAPI VirtualFree(LPVOID p, SIZE_T n, DWORD type)
{
    (void)n;
    if (!(type & 0x8000)) return TRUE;                /* MEM_DECOMMIT */
    for (int i = 0; i < 64; i++) if (g_valloc[i].p == p) { free(p); g_valloc[i].p = NULL; break; }
    return TRUE;
}
static BOOL WINAPI VirtualProtect(LPVOID p, SIZE_T n, DWORD prot, DWORD *old) { (void)p; (void)n; (void)prot; if (old) *old = 4; return TRUE; }
typedef struct { void *base, *allocbase; DWORD allocprot; SIZE_T size; DWORD state, protect, type; } meminfo;
static SIZE_T WINAPI VirtualQuery(LPCVOID a, meminfo *mi, SIZE_T len)
{
    (void)len;
    uint32_t p = (uint32_t)a & ~0xFFFu;
    mi->base = (void *)p; mi->allocbase = (void *)((uint32_t)a & ~0xFFFFu); mi->allocprot = 0x40; mi->size = 0x1000;
    mi->state = 0x1000; mi->protect = 0x40; mi->type = 0x20000;
    extern pe_module_t *pe_all(void);
    for (pe_module_t *m = pe_all(); m; m = m->next)
        if (!m->builtin && p >= (uint32_t)m->base && p < (uint32_t)m->base + m->size) { mi->allocbase = m->base; break; }
    return sizeof *mi;
}

/* ---- synchronisation (single thread) ----------------------------------------------------------------------------------------- */

static void WINAPI InitializeCriticalSection(void *cs) { memset(cs, 0, 24); }
static BOOL WINAPI InitializeCriticalSectionAndSpinCount(void *cs, DWORD c) { (void)c; memset(cs, 0, 24); return TRUE; }
static void WINAPI EnterCriticalSection(void *cs) { (void)cs; }
static void WINAPI LeaveCriticalSection(void *cs) { (void)cs; }
static BOOL WINAPI TryEnterCriticalSection(void *cs) { (void)cs; return TRUE; }
static void WINAPI DeleteCriticalSection(void *cs) { (void)cs; }
static HANDLE WINAPI CreateMutexA(void *a, BOOL o, LPCSTR n) { (void)a; (void)o; (void)n; return (HANDLE)0x5000; }
static BOOL WINAPI ReleaseMutex(HANDLE h) { (void)h; return TRUE; }
static HANDLE WINAPI CreateEventA(void *a, BOOL m, BOOL i, LPCSTR n) { (void)a; (void)m; (void)i; (void)n; return (HANDLE)0x5004; }
static BOOL WINAPI SetEvent(HANDLE h) { (void)h; return TRUE; }
static BOOL WINAPI ResetEvent(HANDLE h) { (void)h; return TRUE; }
static DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms) { (void)h; if (ms && ms != 0xFFFFFFFFu) usleep(ms * 1000); return 0; }
static HANDLE WINAPI CreateThread(void *a, SIZE_T s, void *f, void *p, DWORD fl, DWORD *id) { (void)a; (void)s; (void)f; (void)p; (void)fl; (void)id; win_set_last_error(8); return NULL; }
static LONG WINAPI InterlockedIncrement(volatile LONG *p) { return ++*p; }
static LONG WINAPI InterlockedDecrement(volatile LONG *p) { return --*p; }
static LONG WINAPI InterlockedExchange(volatile LONG *p, LONG v) { LONG o = *p; *p = v; return o; }
static LONG WINAPI InterlockedExchangeAdd(volatile LONG *p, LONG v) { LONG o = *p; *p += v; return o; }
static LONG WINAPI InterlockedCompareExchange(volatile LONG *p, LONG x, LONG c) { LONG o = *p; if (o == c) *p = x; return o; }
static void WINAPI Sleep(DWORD ms) { win_gui_tick(); usleep(ms * 1000); }
static BOOL WINAPI SwitchToThread(void) { return TRUE; }

static void *g_tls[64]; static int g_tls_used[64];
static DWORD WINAPI TlsAlloc(void) { for (int i = 0; i < 64; i++) if (!g_tls_used[i]) { g_tls_used[i] = 1; g_tls[i] = NULL; return (DWORD)i; } return 0xFFFFFFFFu; }
static BOOL WINAPI TlsFree(DWORD i) { if (i < 64) g_tls_used[i] = 0; return TRUE; }
static LPVOID WINAPI TlsGetValue(DWORD i) { win_set_last_error(0); return i < 64 ? g_tls[i] : NULL; }
static BOOL WINAPI TlsSetValue(DWORD i, LPVOID v) { if (i >= 64) return FALSE; g_tls[i] = v; return TRUE; }

/* ---- time -------------------------------------------------------------------------------------------------------------------- */

static uint64_t mono_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u; }
static DWORD WINAPI GetTickCount(void) { return (DWORD)(mono_us() / 1000u); }
static uint64_t WINAPI GetTickCount64(void) { return mono_us() / 1000u; }
static BOOL WINAPI QueryPerformanceCounter(int64_t *c) { *c = (int64_t)mono_us(); return TRUE; }
static BOOL WINAPI QueryPerformanceFrequency(int64_t *f) { *f = 1000000; return TRUE; }
static void WINAPI GetSystemTimeAsFileTime(uint64_t *ft) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); *ft = ((uint64_t)t.tv_sec * 10000000u) + (uint64_t)t.tv_nsec / 100u + 116444736000000000ull; }
typedef struct { WORD y, mo, dow, d, h, mi, s, ms; } systemtime;
static void fill_time(systemtime *st)
{
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    struct tm tm; gmtime_r(&t.tv_sec, &tm);
    st->y = (WORD)(tm.tm_year + 1900); st->mo = (WORD)(tm.tm_mon + 1); st->dow = (WORD)tm.tm_wday; st->d = (WORD)tm.tm_mday;
    st->h = (WORD)tm.tm_hour; st->mi = (WORD)tm.tm_min; st->s = (WORD)tm.tm_sec; st->ms = (WORD)(t.tv_nsec / 1000000);
}
static void WINAPI GetSystemTime(systemtime *st) { fill_time(st); }
static void WINAPI GetLocalTime(systemtime *st) { fill_time(st); }

/* ---- files and console ------------------------------------------------------------------------------------------------------- */

#define H_STDIN  ((HANDLE)0x100)
#define H_STDOUT ((HANDLE)0x104)
#define H_STDERR ((HANDLE)0x108)
static int h2fd(HANDLE h)
{
    uint32_t v = (uint32_t)(uintptr_t)h;
    if (v == 0x100) return 0;
    if (v == 0x104) return 1;
    if (v == 0x108) return 2;
    return v >= 0x200 ? (int)((v - 0x200) / 4) : -1;
}
static HANDLE fd2h(int fd) { return (HANDLE)(uintptr_t)(0x200 + fd * 4); }

static HANDLE WINAPI GetStdHandle(DWORD n) { return n == (DWORD)-10 ? H_STDIN : (n == (DWORD)-11 ? H_STDOUT : (n == (DWORD)-12 ? H_STDERR : INVALID_HANDLE_VALUE)); }
static BOOL WINAPI SetStdHandle(DWORD n, HANDLE h) { (void)n; (void)h; return TRUE; }
static DWORD WINAPI GetFileType(HANDLE h) { int fd = h2fd(h); return fd >= 0 && fd <= 2 ? 2 : (fd > 2 ? 1 : 0); }
static BOOL WINAPI GetConsoleMode(HANDLE h, DWORD *m) { if (h2fd(h) > 2) return FALSE; *m = 7; return TRUE; }
static BOOL WINAPI SetConsoleMode(HANDLE h, DWORD m) { (void)h; (void)m; return TRUE; }
static BOOL WINAPI SetConsoleCtrlHandler(void *f, BOOL a) { (void)f; (void)a; return TRUE; }
static BOOL WINAPI SetConsoleTitleA(LPCSTR t) { (void)t; return TRUE; }
static BOOL WINAPI SetConsoleTextAttribute(HANDLE h, WORD a) { (void)h; (void)a; return TRUE; }
static UINT WINAPI GetConsoleOutputCP(void) { return 65001; }
static BOOL WINAPI SetConsoleOutputCP(UINT c) { (void)c; return TRUE; }
typedef struct { int16_t sx, sy, cx, cy; WORD attr; int16_t l, t, r, b, mx, my; } csbi_t;
static BOOL WINAPI GetConsoleScreenBufferInfo(HANDLE h, csbi_t *c)
{
    (void)h; memset(c, 0, sizeof *c);
    c->sx = 160; c->sy = 90; c->attr = 7; c->r = 159; c->b = 89; c->mx = 160; c->my = 90;
    return TRUE;
}

static BOOL WINAPI WriteFile(HANDLE h, LPCVOID buf, DWORD n, DWORD *written, void *ov)
{
    (void)ov;
    int fd = h2fd(h);
    if (fd < 0) { win_set_last_error(6); return FALSE; }
    ssize_t r = write(fd, buf, n);
    if (r < 0) { win_set_last_error(5); return FALSE; }
    if (written) *written = (DWORD)r;
    return TRUE;
}
static BOOL WINAPI WriteConsoleA(HANDLE h, LPCVOID buf, DWORD n, DWORD *written, void *r) { return WriteFile(h, buf, n, written, r); }
static BOOL WINAPI ReadFile(HANDLE h, LPVOID buf, DWORD n, DWORD *got, void *ov)
{
    (void)ov;
    int fd = h2fd(h);
    if (fd < 0) { win_set_last_error(6); return FALSE; }
    ssize_t r = fd == 0 ? 0 : read(fd, buf, n);    /* there is no console input to read yet */
    if (r < 0) { win_set_last_error(5); return FALSE; }
    if (got) *got = (DWORD)r;
    return TRUE;
}
static HANDLE WINAPI CreateFileA(LPCSTR name, DWORD access, DWORD share, void *sec, DWORD disp, DWORD attr, HANDLE tmpl)
{
    (void)share; (void)sec; (void)attr; (void)tmpl;
    int fl = (access & 0x40000000) ? ((access & 0x80000000) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
    case 1: fl |= O_CREAT | O_EXCL; break;
    case 2: fl |= O_CREAT | O_TRUNC; break;
    case 4: fl |= O_CREAT; break;
    case 5: fl |= O_TRUNC; break;
    default: break;
    }
    int fd = open(win_path(name), fl, 0644);
    if (fd < 0) { win_set_last_error(errno == EEXIST ? 80 : 2); return INVALID_HANDLE_VALUE; }
    return fd2h(fd);
}
static BOOL WINAPI CloseHandle(HANDLE h) { int fd = h2fd(h); if (fd > 2) close(fd); return TRUE; }
static DWORD WINAPI GetFileSize(HANDLE h, DWORD *hi)
{
    struct stat st;
    if (hi) *hi = 0;
    if (fstat(h2fd(h), &st) != 0) return 0xFFFFFFFFu;
    return (DWORD)st.st_size;
}
static DWORD WINAPI SetFilePointer(HANDLE h, LONG lo, LONG *hi, DWORD how)
{
    (void)hi;
    off_t r = lseek(h2fd(h), lo, how == 0 ? SEEK_SET : (how == 1 ? SEEK_CUR : SEEK_END));
    return r < 0 ? 0xFFFFFFFFu : (DWORD)r;
}
static BOOL WINAPI FlushFileBuffers(HANDLE h) { (void)h; return TRUE; }
static BOOL WINAPI SetEndOfFile(HANDLE h) { return ftruncate(h2fd(h), lseek(h2fd(h), 0, SEEK_CUR)) == 0; }
static DWORD WINAPI GetFileAttributesA(LPCSTR p)
{
    struct stat st;
    if (stat(win_path(p), &st) != 0) { win_set_last_error(2); return 0xFFFFFFFFu; }
    return S_ISDIR(st.st_mode) ? 0x10 : 0x80;
}
static BOOL WINAPI DeleteFileA(LPCSTR p) { return unlink(win_path(p)) == 0; }
static BOOL WINAPI CreateDirectoryA(LPCSTR p, void *s) { (void)s; return mkdir(win_path(p), 0755) == 0; }
static BOOL WINAPI MoveFileA(LPCSTR a, LPCSTR b) { char t[320]; strcpy(t, win_path(a)); return rename(t, win_path(b)) == 0; }
static DWORD WINAPI GetCurrentDirectoryA(DWORD n, LPSTR buf) { char t[320]; if (!getcwd(t, sizeof t)) return 0; DWORD l = (DWORD)strlen(t); if (l + 1 > n) return l + 1; strcpy(buf, t); return l; }
static BOOL WINAPI SetCurrentDirectoryA(LPCSTR p) { return chdir(win_path(p)) == 0; }
static DWORD WINAPI GetFullPathNameA(LPCSTR p, DWORD n, LPSTR buf, LPSTR *fp)
{
    char t[640];
    const char *w = win_path(p);
    if (w[0] == '/') snprintf(t, sizeof t, "%s", w); else { char cwd[320]; if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/disk"); snprintf(t, sizeof t, "%s/%s", cwd, w); }
    DWORD l = (DWORD)strlen(t);
    if (l + 1 > n) return l + 1;
    strcpy(buf, t);
    if (fp) { char *s = strrchr(buf, '/'); *fp = s ? s + 1 : buf; }
    return l;
}
static DWORD WINAPI GetTempPathA(DWORD n, LPSTR buf) { const char *t = "/tmp/"; if (n < 6) return 6; strcpy(buf, t); return 5; }

typedef struct { DWORD attr; uint64_t ct, at, wt; DWORD sh, sl, r0, r1; char name[260]; char alt[14]; } finddata;
typedef struct { DIR *d; char pat[128]; char dir[300]; } findh;
static BOOL fill_next(findh *f, finddata *fd)
{
    struct dirent *e;
    while ((e = readdir(f->d))) {
        if (fnmatch(f->pat, e->d_name, FNM_CASEFOLD) != 0 && strcmp(f->pat, "*.*") != 0) continue;
        char full[640]; struct stat st;
        snprintf(full, sizeof full, "%s/%s", f->dir, e->d_name);
        memset(fd, 0, sizeof *fd);
        fd->attr = 0x80;
        if (stat(full, &st) == 0) { if (S_ISDIR(st.st_mode)) fd->attr = 0x10; fd->sl = (DWORD)st.st_size; }
        strncpy(fd->name, e->d_name, 259);
        return TRUE;
    }
    return FALSE;
}
static HANDLE WINAPI FindFirstFileA(LPCSTR path, finddata *fd)
{
    const char *w = win_path(path);
    findh *f = calloc(1, sizeof *f);
    const char *s = strrchr(w, '/');
    if (s) { size_t n = (size_t)(s - w); memcpy(f->dir, w, n); f->dir[n] = 0; if (!n) strcpy(f->dir, "/"); strncpy(f->pat, s + 1, 127); }
    else { strcpy(f->dir, "."); strncpy(f->pat, w, 127); }
    f->d = opendir(f->dir);
    if (!f->d || !fill_next(f, fd)) { if (f->d) closedir(f->d); free(f); win_set_last_error(2); return INVALID_HANDLE_VALUE; }
    return f;
}
static BOOL WINAPI FindNextFileA(HANDLE h, finddata *fd) { return fill_next((findh *)h, fd); }
static BOOL WINAPI FindClose(HANDLE h) { findh *f = h; if (f && f != INVALID_HANDLE_VALUE) { closedir(f->d); free(f); } return TRUE; }

/* ---- strings and code pages --------------------------------------------------------------------------------------------------- */

static int WINAPI MultiByteToWideChar(UINT cp, DWORD fl, LPCSTR s, int n, LPWSTR d, int dn)
{
    (void)fl;
    const unsigned char *p = (const unsigned char *)s;
    if (n < 0) n = (int)strlen(s) + 1;
    int o = 0;
    for (int i = 0; i < n;) {
        uint32_t c = p[i++];
        if (cp == 65001 && c >= 0xC0) {
            int extra = c >= 0xF0 ? 3 : (c >= 0xE0 ? 2 : 1);
            c &= 0x3F >> extra;
            while (extra-- && i < n) c = (c << 6) | (p[i++] & 0x3F);
        }
        int units = c >= 0x10000 ? 2 : 1;
        if (d) {
            if (o + units > dn) { win_set_last_error(122); return 0; }
            if (units == 2) { c -= 0x10000; d[o] = (WCHAR)(0xD800 + (c >> 10)); d[o + 1] = (WCHAR)(0xDC00 + (c & 0x3FF)); } else d[o] = (WCHAR)c;
        }
        o += units;
    }
    return o;
}
static int WINAPI WideCharToMultiByte(UINT cp, DWORD fl, LPCWSTR s, int n, LPSTR d, int dn, LPCSTR def, BOOL *used)
{
    (void)fl; (void)def;
    if (used) *used = FALSE;
    if (n < 0) { n = 0; while (s[n]) n++; n++; }
    int o = 0;
    for (int i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00u);
        unsigned char b[4]; int l;
        if (cp == 65001) {
            if (c < 0x80) { b[0] = (unsigned char)c; l = 1; }
            else if (c < 0x800) { b[0] = (unsigned char)(0xC0 | c >> 6); b[1] = (unsigned char)(0x80 | (c & 63)); l = 2; }
            else if (c < 0x10000) { b[0] = (unsigned char)(0xE0 | c >> 12); b[1] = (unsigned char)(0x80 | (c >> 6 & 63)); b[2] = (unsigned char)(0x80 | (c & 63)); l = 3; }
            else { b[0] = (unsigned char)(0xF0 | c >> 18); b[1] = (unsigned char)(0x80 | (c >> 12 & 63)); b[2] = (unsigned char)(0x80 | (c >> 6 & 63)); b[3] = (unsigned char)(0x80 | (c & 63)); l = 4; }
        } else { b[0] = c < 256 ? (unsigned char)c : '?'; l = 1; }
        if (d) { if (o + l > dn) { win_set_last_error(122); return 0; } memcpy(d + o, b, (size_t)l); }
        o += l;
    }
    return o;
}
static BOOL WINAPI IsDBCSLeadByteEx(UINT cp, BYTE b) { (void)cp; (void)b; return FALSE; }
static BOOL WINAPI IsDBCSLeadByte(BYTE b) { (void)b; return FALSE; }
static UINT WINAPI GetACP(void) { return 1252; }
static UINT WINAPI GetOEMCP(void) { return 437; }
static BOOL WINAPI IsValidCodePage(UINT cp) { (void)cp; return TRUE; }
static BOOL WINAPI GetCPInfo(UINT cp, void *info) { (void)cp; memset(info, 0, 20); *(DWORD *)info = 1; return TRUE; }
static int WINAPI lstrlenA(LPCSTR s) { return s ? (int)strlen(s) : 0; }
static LPSTR WINAPI lstrcpyA(LPSTR d, LPCSTR s) { return strcpy(d, s); }
static LPSTR WINAPI lstrcpynA(LPSTR d, LPCSTR s, int n) { if (n > 0) { strncpy(d, s, (size_t)n - 1); d[n - 1] = 0; } return d; }
static LPSTR WINAPI lstrcatA(LPSTR d, LPCSTR s) { return strcat(d, s); }
static int WINAPI lstrcmpA(LPCSTR a, LPCSTR b) { return strcmp(a, b); }
static int WINAPI lstrcmpiA(LPCSTR a, LPCSTR b) { return strcasecmp(a, b); }
static void WINAPI OutputDebugStringA(LPCSTR s) { fputs(s, stderr); }
static DWORD WINAPI FormatMessageA(DWORD fl, LPCVOID src, DWORD id, DWORD lang, LPSTR buf, DWORD n, void *args)
{
    (void)src; (void)lang; (void)args;
    char msg[64]; snprintf(msg, sizeof msg, "Windows error %u\r\n", id);
    if (fl & 0x100) { char *p = malloc(64); strcpy(p, msg); *(char **)buf = p; return (DWORD)strlen(msg); }
    strncpy(buf, msg, n); if (n) buf[n - 1] = 0;
    return (DWORD)strlen(buf);
}

const win_export_t win_k32_exports[] = {
    K32(GetLastError), K32(SetLastError), K32(ExitProcess), K32(TerminateProcess), K32(GetCurrentProcess), K32(GetCurrentThread),
    K32(GetCurrentProcessId), K32(GetCurrentThreadId), K32(SetUnhandledExceptionFilter), K32(UnhandledExceptionFilter), K32(SetErrorMode),
    K32(IsDebuggerPresent), K32(RaiseException), K32(DebugBreak), K32(GetVersion), K32(GetVersionExA), K32(GetSystemInfo), K32(GlobalMemoryStatus),
    K32(GetModuleHandleA), K32(GetModuleHandleW), K32(LoadLibraryA), K32(LoadLibraryW), K32(LoadLibraryExA), K32(FreeLibrary), K32(GetProcAddress),
    K32(GetModuleFileNameA), K32(DisableThreadLibraryCalls), K32(GetCommandLineA), K32(GetCommandLineW), K32(GetStartupInfoA), K32(GetStartupInfoW),
    K32(GetEnvironmentVariableA), K32(SetEnvironmentVariableA),
    K32(HeapCreate), K32(HeapDestroy), K32(GetProcessHeap), K32(HeapAlloc), K32(HeapFree), K32(HeapReAlloc), K32(HeapSize), K32(LocalAlloc), K32(LocalFree),
    K32(GlobalAlloc), K32(GlobalFree), K32(GlobalLock), K32(GlobalUnlock), K32(GlobalSize), K32(VirtualAlloc), K32(VirtualFree), K32(VirtualProtect), K32(VirtualQuery),
    K32(InitializeCriticalSection), K32(InitializeCriticalSectionAndSpinCount), K32(EnterCriticalSection), K32(LeaveCriticalSection), K32(TryEnterCriticalSection),
    K32(DeleteCriticalSection), K32(CreateMutexA), K32(ReleaseMutex), K32(CreateEventA), K32(SetEvent), K32(ResetEvent), K32(WaitForSingleObject), K32(CreateThread),
    K32(InterlockedIncrement), K32(InterlockedDecrement), K32(InterlockedExchange), K32(InterlockedExchangeAdd), K32(InterlockedCompareExchange), K32(Sleep),
    K32(SwitchToThread), K32(TlsAlloc), K32(TlsFree), K32(TlsGetValue), K32(TlsSetValue),
    K32(GetTickCount), K32(GetTickCount64), K32(QueryPerformanceCounter), K32(QueryPerformanceFrequency), K32(GetSystemTimeAsFileTime), K32(GetSystemTime), K32(GetLocalTime),
    K32(GetStdHandle), K32(SetStdHandle), K32(GetFileType), K32(GetConsoleMode), K32(SetConsoleMode), K32(SetConsoleCtrlHandler), K32(SetConsoleTitleA),
    K32(SetConsoleTextAttribute), K32(GetConsoleOutputCP), K32(SetConsoleOutputCP), K32(GetConsoleScreenBufferInfo),
    K32(WriteFile), K32(WriteConsoleA), K32(ReadFile), K32(CreateFileA), K32(CloseHandle), K32(GetFileSize), K32(SetFilePointer), K32(FlushFileBuffers), K32(SetEndOfFile),
    K32(GetFileAttributesA), K32(DeleteFileA), K32(CreateDirectoryA), K32(MoveFileA), K32(GetCurrentDirectoryA), K32(SetCurrentDirectoryA), K32(GetFullPathNameA),
    K32(GetTempPathA), K32(FindFirstFileA), K32(FindNextFileA), K32(FindClose),
    K32(MultiByteToWideChar), K32(WideCharToMultiByte), K32(IsDBCSLeadByteEx), K32(IsDBCSLeadByte), K32(GetACP), K32(GetOEMCP), K32(IsValidCodePage), K32(GetCPInfo),
    K32(lstrlenA), K32(lstrcpyA), K32(lstrcpynA), K32(lstrcatA), K32(lstrcmpA), K32(lstrcmpiA), K32(OutputDebugStringA), K32(FormatMessageA),
    { NULL, NULL, NULL }
};
