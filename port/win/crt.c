/* crt.c - msvcrt.dll (and the UCRT names that map to it): start-up hooks Windows programs call, the stdio FILE objects they
 * expect (_iob), and the functions that are the same in musl. musl's FILE and Windows' are different things, so every
 * function that takes a FILE* first turns the Windows pointer into the right one. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <locale.h>
#include <malloc.h>
#include <signal.h>
#include <sys/stat.h>
#include "winapi.h"

#define CRT(n) { "msvcrt", #n, (void *)n }
#define CRTW(n, f) { "msvcrt", #n, (void *)f }

/* ---- the three standard streams ---------------------------------------------------------------------------------------------- */

static char g_iob[3][32];                               /* what a program sees for stdin, stdout, stderr (sizeof FILE is 32) */
static FILE *F(void *p)
{
    if ((char *)p >= g_iob[0] && (char *)p < g_iob[0] + sizeof g_iob) return p == g_iob[0] ? stdin : (p == g_iob[1] ? stdout : stderr);
    return p;
}

static void *__iob_func_w(void) { return g_iob; }

/* ---- start-up ---------------------------------------------------------------------------------------------------------------- */

static char **g_envp;
static char **g_initenv_var;
static int g_fmode, g_commode, g_mbcur = 1, g_doserrno;

static int w__getmainargs(int *argc, char ***argv, char ***env, int wild, void *si)
{
    (void)wild; (void)si;
    extern char **environ;
    *argc = win_argc; *argv = win_argv; g_envp = environ; *env = environ;
    g_initenv_var = environ;
    return 0;
}
static int *w__p__fmode(void) { return &g_fmode; }
static int *w__p__commode(void) { return &g_commode; }
static char ***w__p___initenv(void) { return &g_initenv_var; }
static char ***w__p__environ(void) { extern char **environ; g_envp = environ; return &g_envp; }
static int *w__p___argc(void) { return &win_argc; }
static char ***w__p___argv(void) { return &win_argv; }
static void w__set_app_type(int t) { (void)t; }
static void w__initterm(void (**s)(void), void (**e)(void)) { for (; s < e; s++) if (*s) (*s)(); }
static int w__initterm_e(int (**s)(void), int (**e)(void)) { for (; s < e; s++) if (*s) { int r = (*s)(); if (r) return r; } return 0; }
static void w__amsg_exit(int n) { fprintf(stderr, "winrun: runtime error R%04d\n", n); fflush(NULL); exit(255); }
static void w__cexit(void) { fflush(NULL); }
static void w_exit_exit(int c) { fflush(NULL); _exit(c); }
static int w__onexit(int (*f)(void)) { atexit((void (*)(void))f); return (int)(intptr_t)f; }
static int w_atexit(void (*f)(void)) { return atexit(f); }
static int *w__errno(void) { return &errno; }
static int *w___doserrno(void) { return &g_doserrno; }
static void w__lock(int n) { (void)n; }
static void w__unlock(int n) { (void)n; }
static int w__setusermatherr(void *f) { (void)f; return 0; }
static unsigned w__controlfp(unsigned n, unsigned m) { (void)n; (void)m; return 0x9001F; }
static void w__fpreset(void) { __asm__ volatile("fninit"); }
static void *w_signal(int sig, void *h) { (void)sig; (void)h; return NULL; }
static int w_raise(int sig) { (void)sig; return 0; }
static void w_abort(void) { fprintf(stderr, "\nwinrun: abort() called\n"); fflush(NULL); exit(3); }
static int w___mb_cur_max_func(void) { return 1; }
static int w__setmode(int fd, int m) { (void)fd; (void)m; return 0x4000; }
static void w__set_invalid_parameter_handler(void *f) { (void)f; }

/* ---- stdio ------------------------------------------------------------------------------------------------------------------- */

static int w_fprintf(void *f, const char *fmt, ...) { va_list a; va_start(a, fmt); int r = vfprintf(F(f), fmt, a); va_end(a); return r; }
static int w_vfprintf(void *f, const char *fmt, va_list a) { return vfprintf(F(f), fmt, a); }
static int w_fputs(const char *s, void *f) { return fputs(s, F(f)); }
static int w_fputc(int c, void *f) { return fputc(c, F(f)); }
static int w_fgetc(void *f) { return fgetc(F(f)); }
static char *w_fgets(char *b, int n, void *f) { return fgets(b, n, F(f)); }
static size_t w_fwrite(const void *p, size_t s, size_t n, void *f) { return fwrite(p, s, n, F(f)); }
static size_t w_fread(void *p, size_t s, size_t n, void *f) { return fread(p, s, n, F(f)); }
static int w_fflush(void *f) { return f ? fflush(F(f)) : fflush(NULL); }
static int w_fclose(void *f) { return fclose(F(f)); }
static int w_fseek(void *f, long o, int w) { return fseek(F(f), o, w); }
static long w_ftell(void *f) { return ftell(F(f)); }
static void w_rewind(void *f) { rewind(F(f)); }
static int w_feof(void *f) { return feof(F(f)); }
static int w_ferror(void *f) { return ferror(F(f)); }
static void w_clearerr(void *f) { clearerr(F(f)); }
static int w_ungetc(int c, void *f) { return ungetc(c, F(f)); }
static int w_fscanf(void *f, const char *fmt, ...) { va_list a; va_start(a, fmt); int r = vfscanf(F(f), fmt, a); va_end(a); return r; }
static void *w_fopen(const char *p, const char *m) { return fopen(win_path(p), m); }
static void *w_freopen(const char *p, const char *m, void *f) { return freopen(win_path(p), m, F(f)); }
static int w_remove(const char *p) { return remove(win_path(p)); }
static int w_rename(const char *a, const char *b) { char t[320]; strcpy(t, win_path(a)); return rename(t, win_path(b)); }
static int w__fileno(void *f) { return fileno(F(f)); }
static int w__isatty(int fd) { return fd >= 0 && fd <= 2; }
static int w_setvbuf(void *f, char *b, int m, size_t n) { return setvbuf(F(f), b, m, n); }
static int w__open(const char *p, int fl, ...) { return open(win_path(p), fl & ~0x8000, 0644); }
static int w__mkdir(const char *p) { return mkdir(win_path(p), 0755); }
static int w__chdir(const char *p) { return chdir(win_path(p)); }
static int w__access(const char *p, int m) { return access(win_path(p), m); }
static int w_vsnprintf(char *b, size_t n, const char *f, va_list a) { return vsnprintf(b, n, f, a); }
static int w_snprintf(char *b, size_t n, const char *f, ...) { va_list a; va_start(a, f); int r = vsnprintf(b, n, f, a); va_end(a); return r; }
static int w__vscprintf(const char *f, va_list a) { return vsnprintf(NULL, 0, f, a); }
static int w__scprintf(const char *f, ...) { va_list a; va_start(a, f); int r = vsnprintf(NULL, 0, f, a); va_end(a); return r; }

/* ---- strings ------------------------------------------------------------------------------------------------------------------ */

static char *w__strlwr(char *s) { for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p); return s; }
static char *w__strupr(char *s) { for (char *p = s; *p; p++) *p = (char)toupper((unsigned char)*p); return s; }
static char *w__itoa(int v, char *b, int r) { if (r == 10) sprintf(b, "%d", v); else if (r == 16) sprintf(b, "%x", (unsigned)v); else if (r == 8) sprintf(b, "%o", (unsigned)v); else { unsigned u = (unsigned)v; char t[40]; int n = 0; do { t[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[u % (unsigned)r]; u /= (unsigned)r; } while (u); for (int i = 0; i < n; i++) b[i] = t[n - 1 - i]; b[n] = 0; } return b; }
static char *w__ultoa(unsigned long v, char *b, int r) { if (r == 10) sprintf(b, "%lu", v); else if (r == 16) sprintf(b, "%lx", v); else sprintf(b, "%lu", v); return b; }
static size_t w_wcslen(const WCHAR *s) { size_t n = 0; while (s[n]) n++; return n; }
static WCHAR *w_wcscpy(WCHAR *d, const WCHAR *s) { WCHAR *r = d; while ((*d++ = *s++)) {} return r; }
static int w_wcscmp(const WCHAR *a, const WCHAR *b) { while (*a && *a == *b) { a++; b++; } return (int)*a - (int)*b; }
static void *w__aligned_malloc(size_t n, size_t a) { return memalign(a, n); }

/* ---- time and misc --------------------------------------------------------------------------------------------------------- */

static void *w_localeconv(void) { return localeconv(); }
static char *w_setlocale(int c, const char *l) { (void)c; (void)l; return "C"; }
static int64_t w__time64(int64_t *t) { time_t v = time(NULL); if (t) *t = v; return v; }
static void *w_getenv(const char *n) { return getenv(n); }
static char *w__getcwd(char *b, int n) { return getcwd(b, (size_t)n); }

const win_export_t win_crt_exports[] = {
    CRTW(__getmainargs, w__getmainargs), CRTW(_fmode, &g_fmode), CRTW(_commode, &g_commode), CRTW(__p__fmode, w__p__fmode), CRTW(__p__commode, w__p__commode),
    CRTW(__initenv, &g_initenv_var), CRTW(__p___initenv, w__p___initenv), CRTW(__p__environ, w__p__environ), CRTW(__p___argc, w__p___argc), CRTW(__p___argv, w__p___argv),
    CRTW(__set_app_type, w__set_app_type), CRTW(_initterm, w__initterm), CRTW(_initterm_e, w__initterm_e), CRTW(_amsg_exit, w__amsg_exit), CRTW(_cexit, w__cexit),
    CRTW(_exit, w_exit_exit), CRT(exit), CRTW(_onexit, w__onexit), CRTW(atexit, w_atexit), CRTW(_errno, w__errno), CRTW(__doserrno, w___doserrno), CRTW(_lock, w__lock),
    CRTW(_unlock, w__unlock), CRTW(__setusermatherr, w__setusermatherr), CRTW(_controlfp, w__controlfp), CRTW(_fpreset, w__fpreset), CRTW(signal, w_signal),
    CRTW(raise, w_raise), CRTW(abort, w_abort), CRTW(__mb_cur_max, &g_mbcur), CRTW(___mb_cur_max_func, w___mb_cur_max_func), CRTW(_setmode, w__setmode),
    CRTW(_set_invalid_parameter_handler, w__set_invalid_parameter_handler), CRTW(_iob, g_iob), CRTW(__iob_func, __iob_func_w), CRTW(localeconv, w_localeconv),
    CRTW(setlocale, w_setlocale),
    CRT(printf), CRTW(fprintf, w_fprintf), CRT(sprintf), CRTW(_snprintf, w_snprintf), CRTW(snprintf, w_snprintf), CRTW(vfprintf, w_vfprintf), CRT(vprintf), CRT(vsprintf),
    CRTW(_vsnprintf, w_vsnprintf), CRTW(vsnprintf, w_vsnprintf), CRTW(_scprintf, w__scprintf), CRTW(_vscprintf, w__vscprintf), CRT(sscanf), CRT(scanf), CRTW(fscanf, w_fscanf),
    CRT(puts), CRTW(fputs, w_fputs), CRT(putchar), CRTW(fputc, w_fputc), CRTW(putc, w_fputc), CRTW(fgetc, w_fgetc), CRTW(getc, w_fgetc), CRT(getchar), CRTW(fgets, w_fgets),
    CRTW(fwrite, w_fwrite), CRTW(fread, w_fread), CRTW(fopen, w_fopen), CRTW(freopen, w_freopen), CRTW(fclose, w_fclose), CRTW(fflush, w_fflush), CRTW(fseek, w_fseek),
    CRTW(ftell, w_ftell), CRTW(rewind, w_rewind), CRTW(feof, w_feof), CRTW(ferror, w_ferror), CRTW(clearerr, w_clearerr), CRTW(ungetc, w_ungetc), CRTW(remove, w_remove),
    CRTW(rename, w_rename), CRTW(_fileno, w__fileno), CRTW(_isatty, w__isatty), CRTW(setvbuf, w_setvbuf), CRTW(_open, w__open), CRT(close), CRT(read), CRT(write),
    CRT(lseek), CRTW(_close, close), CRTW(_read, read), CRTW(_write, write), CRTW(_lseek, lseek), CRTW(_mkdir, w__mkdir), CRTW(_chdir, w__chdir), CRTW(_access, w__access),
    CRTW(_getcwd, w__getcwd),
    CRT(memcpy), CRT(memmove), CRT(memset), CRT(memcmp), CRT(memchr), CRT(strlen), CRT(strcpy), CRT(strncpy), CRT(strcat), CRT(strncat), CRT(strcmp), CRT(strncmp),
    CRTW(_stricmp, strcasecmp), CRTW(_strnicmp, strncasecmp), CRTW(stricmp, strcasecmp), CRTW(strcmpi, strcasecmp), CRT(strchr), CRT(strrchr), CRT(strstr),
    CRTW(_strdup, strdup), CRT(strdup), CRT(strtok), CRT(strerror), CRT(strtol), CRT(strtoul), CRT(strtod), CRT(strspn), CRT(strcspn), CRT(strpbrk), CRT(atoi), CRT(atol),
    CRT(atof), CRTW(_atoi64, atoll), CRTW(_strlwr, w__strlwr), CRTW(_strupr, w__strupr), CRTW(_itoa, w__itoa), CRTW(_ultoa, w__ultoa), CRT(toupper), CRT(tolower), CRT(isalpha),
    CRT(isdigit), CRT(isalnum), CRT(isspace), CRT(isupper), CRT(islower), CRT(isxdigit), CRT(isprint), CRT(ispunct), CRT(iscntrl), CRTW(wcslen, w_wcslen), CRTW(wcscpy, w_wcscpy),
    CRTW(wcscmp, w_wcscmp),
    CRT(malloc), CRT(calloc), CRT(realloc), CRT(free), CRTW(_aligned_malloc, w__aligned_malloc), CRTW(_aligned_free, free),
    CRT(sin), CRT(cos), CRT(tan), CRT(asin), CRT(acos), CRT(atan), CRT(atan2), CRT(sinh), CRT(cosh), CRT(tanh), CRT(sqrt), CRT(pow), CRT(exp), CRT(log), CRT(log10),
    CRT(floor), CRT(ceil), CRT(fabs), CRT(fmod), CRT(ldexp), CRT(frexp), CRT(modf), CRT(sinf), CRT(cosf), CRT(tanf), CRT(sqrtf), CRT(powf), CRT(floorf), CRT(ceilf),
    CRT(fabsf), CRT(fmodf), CRT(atan2f), CRT(expf), CRT(logf),
    CRT(rand), CRT(srand), CRT(time), CRTW(_time64, w__time64), CRT(clock), CRT(qsort), CRT(bsearch), CRT(abs), CRT(labs), CRT(div), CRT(ldiv), CRTW(getenv, w_getenv),
    CRT(gmtime), CRT(localtime), CRT(mktime), CRT(strftime), CRT(difftime),
    { NULL, NULL, NULL }
};
