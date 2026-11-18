/* qjs_libc.h - the slice of the C library QuickJS needs, for RadiumOS.
 *
 * The kernel already defines some of these names (malloc, snprintf, pow,
 * sqrt...) with its own, non-standard behaviour, so everything here is
 * renamed to qjs_* by macro. Only QuickJS and this compat layer see these
 * headers; the rest of the kernel keeps its own functions. memcpy, memmove,
 * memset and memcmp are left alone: the compiler emits calls to them on its
 * own and the kernel's versions are standard.
 */
#ifndef QJS_LIBC_H
#define QJS_LIBC_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>
#include <float.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---- stdlib ---- */
#define malloc qjs_malloc
#define free qjs_free
#define realloc qjs_realloc
#define calloc qjs_calloc
#define malloc_usable_size qjs_malloc_usable_size
#define abort qjs_abort
#define exit qjs_exit
#define strtod qjs_strtod
#define strtol qjs_strtol
#define strtoul qjs_strtoul
#define strtoll qjs_strtoll
#define strtoull qjs_strtoull
#define atoi qjs_atoi
#define qsort qjs_qsort
#define getenv qjs_getenv
#define abs qjs_abs
#define llabs qjs_llabs

void *qjs_malloc(size_t size);
void qjs_free(void *ptr);
void *qjs_realloc(void *ptr, size_t size);
void *qjs_calloc(size_t n, size_t size);
size_t qjs_malloc_usable_size(void *ptr);
__attribute__((noreturn)) void qjs_abort(void);
__attribute__((noreturn)) void qjs_exit(int code);
double qjs_strtod(const char *s, char **end);
long qjs_strtol(const char *s, char **end, int base);
unsigned long qjs_strtoul(const char *s, char **end, int base);
long long qjs_strtoll(const char *s, char **end, int base);
unsigned long long qjs_strtoull(const char *s, char **end, int base);
int qjs_atoi(const char *s);
void qjs_qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
char *qjs_getenv(const char *name);
int qjs_abs(int v);
long long qjs_llabs(long long v);

/* ---- string ---- */
#define strlen qjs_strlen
#define strnlen qjs_strnlen
#define strcmp qjs_strcmp
#define strncmp qjs_strncmp
#define strchr qjs_strchr
#define strrchr qjs_strrchr
#define strstr qjs_strstr
#define strcpy qjs_strcpy
#define strncpy qjs_strncpy
#define strcat qjs_strcat
#define strdup qjs_strdup
#define memchr qjs_memchr
#define strerror qjs_strerror

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t qjs_strlen(const char *s);
size_t qjs_strnlen(const char *s, size_t max);
int qjs_strcmp(const char *a, const char *b);
int qjs_strncmp(const char *a, const char *b, size_t n);
char *qjs_strchr(const char *s, int c);
char *qjs_strrchr(const char *s, int c);
char *qjs_strstr(const char *h, const char *n);
char *qjs_strcpy(char *d, const char *s);
char *qjs_strncpy(char *d, const char *s, size_t n);
char *qjs_strcat(char *d, const char *s);
char *qjs_strdup(const char *s);
void *qjs_memchr(const void *s, int c, size_t n);
char *qjs_strerror(int e);

/* ---- ctype ---- */
static inline int qjs_isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int qjs_isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline int qjs_isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int qjs_islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int qjs_isalpha(int c) { return qjs_isupper(c) || qjs_islower(c); }
static inline int qjs_isalnum(int c) { return qjs_isalpha(c) || qjs_isdigit(c); }
static inline int qjs_isxdigit(int c) { return qjs_isdigit(c) || ((c | 32) >= 'a' && (c | 32) <= 'f'); }
static inline int qjs_toupper(int c) { return qjs_islower(c) ? c - 32 : c; }
static inline int qjs_tolower(int c) { return qjs_isupper(c) ? c + 32 : c; }
#define isdigit qjs_isdigit
#define isspace qjs_isspace
#define isupper qjs_isupper
#define islower qjs_islower
#define isalpha qjs_isalpha
#define isalnum qjs_isalnum
#define isxdigit qjs_isxdigit
#define toupper qjs_toupper
#define tolower qjs_tolower

/* ---- stdio: output goes to the kernel terminal ---- */
typedef struct QjsFile { int fd; } FILE;
extern FILE *qjs_stdout, *qjs_stderr;
#define stdout qjs_stdout
#define stderr qjs_stderr
#define EOF (-1)
#define printf qjs_printf
#define fprintf qjs_fprintf
#define vfprintf qjs_vfprintf
#define snprintf qjs_snprintf
#define vsnprintf qjs_vsnprintf
#define sprintf qjs_sprintf
#define putchar qjs_putchar
#define fputc qjs_fputc
#define putc qjs_fputc
#define fputs qjs_fputs
#define puts qjs_puts
#define fwrite qjs_fwrite
#define fflush qjs_fflush

int qjs_printf(const char *fmt, ...);
int qjs_fprintf(FILE *f, const char *fmt, ...);
int qjs_vfprintf(FILE *f, const char *fmt, va_list ap);
int qjs_snprintf(char *buf, size_t cap, const char *fmt, ...);
int qjs_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int qjs_sprintf(char *buf, const char *fmt, ...);
int qjs_putchar(int c);
int qjs_fputc(int c, FILE *f);
int qjs_fputs(const char *s, FILE *f);
int qjs_puts(const char *s);
size_t qjs_fwrite(const void *p, size_t size, size_t n, FILE *f);
int qjs_fflush(FILE *f);

/* ---- alloca ---- */
#define alloca(n) __builtin_alloca(n)

/* ---- errno ---- */
extern int qjs_errno;
#define errno qjs_errno
#define ERANGE 34
#define EINVAL 22
#define ENOMEM 12

/* ---- assert ---- */
__attribute__((noreturn)) void qjs_assert_fail(const char *expr, const char *file, int line);
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) ((x) ? (void)0 : qjs_assert_fail(#x, __FILE__, __LINE__))
#endif

/* ---- time ---- */
typedef long long time_t;
typedef long suseconds_t;
struct timeval { time_t tv_sec; suseconds_t tv_usec; };
struct timezone { int tz_minuteswest; int tz_dsttime; };
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};
typedef long clock_t;
#define CLOCKS_PER_SEC 1000
#define gettimeofday qjs_gettimeofday
#define time qjs_time
#define localtime_r qjs_localtime_r
#define gmtime_r qjs_gmtime_r
#define clock qjs_clock
int qjs_gettimeofday(struct timeval *tv, void *tz);
time_t qjs_time(time_t *t);
struct tm *qjs_localtime_r(const time_t *t, struct tm *out);
struct tm *qjs_gmtime_r(const time_t *t, struct tm *out);
clock_t qjs_clock(void);

/* ---- inttypes (i686: 64-bit is long long) ---- */
#define PRId64 "lld"
#define PRIi64 "lli"
#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRIX64 "llX"
#define PRId32 "d"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"

/* ---- fenv: only round-to-nearest exists here ---- */
#define FE_TONEAREST 0
#define FE_DOWNWARD 0x400
#define FE_UPWARD 0x800
#define FE_TOWARDZERO 0xc00
static inline int fesetround(int r) { return r == FE_TONEAREST ? 0 : -1; }
static inline int fegetround(void) { return FE_TONEAREST; }

#endif
