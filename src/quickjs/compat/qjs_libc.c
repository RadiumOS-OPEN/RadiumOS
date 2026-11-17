/* qjs_libc.c - strings, formatted output, time and process stubs for
 * QuickJS on RadiumOS (declarations in include/qjs_libc.h). Output goes to
 * a replaceable sink, the kernel terminal by default. */
#include "include/qjs_libc.h"
#include "qjs_port.h"

/* Kernel side. Declared by hand: kernel headers would clash with the
   renamed libc prototypes. */
extern void print(const char *s);
extern volatile uint32_t ticks;           /* 1 kHz since boot */
extern uint32_t get_unix_timestamp(void); /* RTC, UTC seconds */

int qjs_errno;
static FILE g_out = {1}, g_err = {2};
FILE *qjs_stdout = &g_out, *qjs_stderr = &g_err;

/* ---- output sink ---- */

static void kernel_sink(const char *s, int len) {
    char buf[257];
    while (len > 0) {
        int n = len > 256 ? 256 : len;
        memcpy(buf, s, (size_t)n);
        buf[n] = 0;
        print(buf);
        s += n;
        len -= n;
    }
}

static QjsSink g_sink = kernel_sink;

void qjs_set_sink(QjsSink sink) {
    g_sink = sink ? sink : kernel_sink;
}

void qjs_write(const char *s, int len) {
    if (len > 0) g_sink(s, len);
}

/* ---- process ---- */

void qjs_abort(void) {
    qjs_write("quickjs: abort\n", 15);
    /* An invalid opcode: under the browser's fault guard this becomes a
       recoverable crash instead of a dead kernel. */
    for (;;) __builtin_trap();
}

void qjs_exit(int code) {
    (void)code;
    qjs_abort();
}

void qjs_assert_fail(const char *expr, const char *file, int line) {
    qjs_fprintf(stderr, "quickjs: assertion failed: %s (%s:%d)\n", expr, file, line);
    qjs_abort();
}

char *qjs_getenv(const char *name) {
    (void)name;
    return 0;
}

int qjs_abs(int v) { return v < 0 ? -v : v; }
long long qjs_llabs(long long v) { return v < 0 ? -v : v; }

/* ---- strings ---- */

size_t qjs_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

size_t qjs_strnlen(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

int qjs_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int qjs_strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b;
    }
    return 0;
}

char *qjs_strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}

char *qjs_strrchr(const char *s, int c) {
    const char *hit = 0;
    for (;; s++) {
        if (*s == (char)c) hit = s;
        if (!*s) return (char *)hit;
    }
}

char *qjs_strstr(const char *h, const char *n) {
    size_t nl = qjs_strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++) {
        if (*h == *n && qjs_strncmp(h, n, nl) == 0) return (char *)h;
    }
    return 0;
}

char *qjs_strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}

char *qjs_strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

char *qjs_strcat(char *d, const char *s) {
    qjs_strcpy(d + qjs_strlen(d), s);
    return d;
}

char *qjs_strdup(const char *s) {
    size_t n = qjs_strlen(s) + 1;
    char *d = qjs_malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

void *qjs_memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (; n; n--, p++) {
        if (*p == (unsigned char)c) return (void *)p;
    }
    return 0;
}

char *qjs_strerror(int e) {
    (void)e;
    return "error";
}

/* ---- number parsing (QuickJS parses JS numbers itself; these serve the
   odd C-level caller) ---- */

static int digit_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 32;
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    return 99;
}

unsigned long long qjs_strtoull(const char *s, char **end, int base) {
    const char *p = s;
    while (qjs_isspace(*p)) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 32) == 'x' && digit_val(p[2]) < 16) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    unsigned long long v = 0;
    const char *start = p;
    for (int d; (d = digit_val(*p)) < base; p++) v = v * (unsigned)base + (unsigned)d;
    if (end) *end = (char *)(p == start ? s : p);
    return neg ? -v : v;
}

long long qjs_strtoll(const char *s, char **end, int base) {
    return (long long)qjs_strtoull(s, end, base);
}

unsigned long qjs_strtoul(const char *s, char **end, int base) {
    return (unsigned long)qjs_strtoull(s, end, base);
}

long qjs_strtol(const char *s, char **end, int base) {
    return (long)qjs_strtoull(s, end, base);
}

int qjs_atoi(const char *s) {
    return (int)qjs_strtol(s, 0, 10);
}

/* Good enough for C callers; JS source numbers go through js_atod. */
double qjs_strtod(const char *s, char **end) {
    const char *p = s;
    while (qjs_isspace(*p)) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    double v = 0;
    int any = 0;
    for (; qjs_isdigit(*p); p++, any = 1) v = v * 10 + (*p - '0');
    if (*p == '.') {
        double scale = 0.1;
        for (p++; qjs_isdigit(*p); p++, any = 1, scale *= 0.1) v += (*p - '0') * scale;
    }
    if (any && (*p | 32) == 'e') {
        char *e2;
        long ex = qjs_strtol(p + 1, &e2, 10);
        if (e2 != p + 1) {
            p = e2;
            for (; ex > 0; ex--) v *= 10;
            for (; ex < 0; ex++) v /= 10;
        }
    }
    if (end) *end = (char *)(any ? p : s);
    return neg ? -v : v;
}

/* ---- qsort (heapsort: no recursion, no allocation) ---- */

static void swap_bytes(unsigned char *a, unsigned char *b, size_t n) {
    while (n--) {
        unsigned char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void sift(unsigned char *base, size_t root, size_t n, size_t size,
                 int (*cmp)(const void *, const void *)) {
    for (;;) {
        size_t child = root * 2 + 1;
        if (child >= n) return;
        if (child + 1 < n && cmp(base + child * size, base + (child + 1) * size) < 0) child++;
        if (cmp(base + root * size, base + child * size) >= 0) return;
        swap_bytes(base + root * size, base + child * size, size);
        root = child;
    }
}

void qjs_qsort(void *b, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    unsigned char *base = b;
    if (n < 2) return;
    for (size_t i = n / 2; i-- > 0;) sift(base, i, n, size, cmp);
    for (size_t end = n - 1; end > 0; end--) {
        swap_bytes(base, base + end * size, size);
        sift(base, 0, end, size, cmp);
    }
}

/* ---- formatted output ---- */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;   /* characters produced (may exceed cap) */
} Out;

static void out_ch(Out *o, char c) {
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

static void out_pad(Out *o, char c, int n) {
    while (n-- > 0) out_ch(o, c);
}

/* One converted field: sign/prefix, zero padding, digits, width. */
static void out_field(Out *o, const char *pre, const char *digits, int dlen,
                      int width, int prec, int left, int zero) {
    int plen = (int)qjs_strlen(pre);
    int zeros = prec > dlen ? prec - dlen : 0;
    int body = plen + zeros + dlen;
    int pad = width > body ? width - body : 0;
    if (!left && !zero) out_pad(o, ' ', pad);
    for (int i = 0; i < plen; i++) out_ch(o, pre[i]);
    if (!left && zero) out_pad(o, '0', pad);
    out_pad(o, '0', zeros);
    for (int i = 0; i < dlen; i++) out_ch(o, digits[i]);
    if (left) out_pad(o, ' ', pad);
}

static int utoa_base(unsigned long long v, unsigned base, int upper, char *tmp) {
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char rev[24];
    int n = 0;
    do {
        rev[n++] = dig[v % base];
        v /= base;
    } while (v);
    for (int i = 0; i < n; i++) tmp[i] = rev[n - 1 - i];
    return n;
}

/* %f/%g for the memory-usage dump: fixed point, rounded half up. */
static int ftoa_fixed(double v, int prec, char *tmp, int cap) {
    if (prec > 9) prec = 9;
    unsigned long long scale = 1;
    for (int i = 0; i < prec; i++) scale *= 10;
    if (v > 1e18) v = 1e18;
    unsigned long long whole = (unsigned long long)v;
    unsigned long long frac = (unsigned long long)((v - (double)whole) * (double)scale + 0.5);
    if (frac >= scale) {
        whole++;
        frac -= scale;
    }
    int n = utoa_base(whole, 10, 0, tmp);
    if (prec > 0 && n + 1 + prec < cap) {
        tmp[n++] = '.';
        char f[24];
        int fl = utoa_base(frac, 10, 0, f);
        for (int i = fl; i < prec; i++) tmp[n++] = '0';
        for (int i = 0; i < fl; i++) tmp[n++] = f[i];
    }
    return n;
}

int qjs_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    Out o = {buf, cap, 0};
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            out_ch(&o, *p);
            continue;
        }
        p++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; p++) {
            if (*p == '-') left = 1;
            else if (*p == '0') zero = 1;
            else if (*p == '+') plus = 1;
            else if (*p == ' ') space = 1;
            else if (*p == '#') alt = 1;
            else break;
        }
        int width = 0, prec = -1;
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            p++;
        } else {
            while (qjs_isdigit(*p)) width = width * 10 + (*p++ - '0');
        }
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') {
                prec = va_arg(ap, int);
                p++;
            } else {
                while (qjs_isdigit(*p)) prec = prec * 10 + (*p++ - '0');
            }
        }
        int lng = 0;   /* 0 int, 1 long, 2 long long, 3 size_t */
        for (;; p++) {
            if (*p == 'l') lng = lng == 1 ? 2 : (lng ? lng : 1);
            else if (*p == 'z' || *p == 't' || *p == 'j') lng = *p == 'j' ? 2 : 3;
            else if (*p == 'h') {}
            else break;
        }
        if (left) zero = 0;
        char tmp[40];
        char c = *p;
        if (!c) break;
        switch (c) {
        case 'd': case 'i': {
            long long v = lng == 2 ? va_arg(ap, long long)
                        : lng == 1 ? va_arg(ap, long)
                        : lng == 3 ? (long long)va_arg(ap, ptrdiff_t)
                        : va_arg(ap, int);
            const char *pre = v < 0 ? "-" : plus ? "+" : space ? " " : "";
            unsigned long long u = v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v;
            int n = (prec == 0 && u == 0) ? 0 : utoa_base(u, 10, 0, tmp);
            out_field(&o, pre, tmp, n, width, prec, left, zero && prec < 0);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = lng == 2 ? va_arg(ap, unsigned long long)
                                 : lng == 1 ? va_arg(ap, unsigned long)
                                 : lng == 3 ? va_arg(ap, size_t)
                                 : va_arg(ap, unsigned);
            unsigned base = c == 'o' ? 8 : c == 'u' ? 10 : 16;
            int n = (prec == 0 && v == 0) ? 0 : utoa_base(v, base, c == 'X', tmp);
            const char *pre = alt && v ? (c == 'x' ? "0x" : c == 'X' ? "0X" : c == 'o' ? "0" : "") : "";
            out_field(&o, pre, tmp, n, width, prec, left, zero && prec < 0);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            int n = utoa_base(v, 16, 0, tmp);
            out_field(&o, "0x", tmp, n, width, -1, left, 0);
            break;
        }
        case 'c': {
            tmp[0] = (char)va_arg(ap, int);
            out_field(&o, "", tmp, 1, width, -1, left, 0);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int n = prec >= 0 ? (int)qjs_strnlen(s, (size_t)prec) : (int)qjs_strlen(s);
            out_field(&o, "", s, n, width, -1, left, 0);
            break;
        }
        case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': {
            double v = va_arg(ap, double);
            const char *pre = (v < 0) ? "-" : plus ? "+" : space ? " " : "";
            if (v < 0) v = -v;
            int n;
            if (v != v) n = (qjs_strcpy(tmp, "nan"), 3);
            else if (v > 1e300) n = (qjs_strcpy(tmp, "inf"), 3);
            else n = ftoa_fixed(v, prec < 0 ? 6 : prec, tmp, (int)sizeof(tmp));
            out_field(&o, pre, tmp, n, width, -1, left, zero);
            break;
        }
        case '%':
            out_ch(&o, '%');
            break;
        default:
            out_ch(&o, '%');
            out_ch(&o, c);
            break;
        }
    }
    if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
    return (int)o.len;
}

int qjs_snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = qjs_vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

int qjs_sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = qjs_vsnprintf(buf, (size_t)1 << 30, fmt, ap);
    va_end(ap);
    return n;
}

int qjs_vfprintf(FILE *f, const char *fmt, va_list ap) {
    (void)f;
    char buf[512];
    va_list ap2;
    va_copy(ap2, ap);
    int n = qjs_vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < (int)sizeof(buf)) {
        qjs_write(buf, n);
    } else {
        char *big = qjs_malloc((size_t)n + 1);
        if (big) {
            qjs_vsnprintf(big, (size_t)n + 1, fmt, ap2);
            qjs_write(big, n);
            qjs_free(big);
        } else {
            qjs_write(buf, (int)sizeof(buf) - 1);
        }
    }
    va_end(ap2);
    return n;
}

int qjs_fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = qjs_vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int qjs_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = qjs_vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int qjs_fputc(int c, FILE *f) {
    (void)f;
    char ch = (char)c;
    qjs_write(&ch, 1);
    return c;
}

int qjs_putchar(int c) { return qjs_fputc(c, stdout); }

int qjs_fputs(const char *s, FILE *f) {
    (void)f;
    qjs_write(s, (int)qjs_strlen(s));
    return 0;
}

int qjs_puts(const char *s) {
    qjs_fputs(s, stdout);
    qjs_write("\n", 1);
    return 0;
}

size_t qjs_fwrite(const void *p, size_t size, size_t n, FILE *f) {
    (void)f;
    qjs_write(p, (int)(size * n));
    return n;
}

int qjs_fflush(FILE *f) {
    (void)f;
    return 0;
}

/* ---- time: RTC seconds at first use, PIT milliseconds after that ---- */

static time_t g_epoch_base;
static uint32_t g_tick_base;
static int g_time_ready;

int qjs_gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (!g_time_ready) {
        g_epoch_base = (time_t)get_unix_timestamp();
        g_tick_base = ticks;
        g_time_ready = 1;
    }
    uint32_t ms = ticks - g_tick_base;
    tv->tv_sec = g_epoch_base + ms / 1000;
    tv->tv_usec = (suseconds_t)(ms % 1000) * 1000;
    return 0;
}

time_t qjs_time(time_t *t) {
    struct timeval tv;
    qjs_gettimeofday(&tv, 0);
    if (t) *t = tv.tv_sec;
    return tv.tv_sec;
}

clock_t qjs_clock(void) {
    return (clock_t)ticks;
}

/* Days since 1970-01-01 -> civil date (Howard Hinnant's algorithm). */
struct tm *qjs_gmtime_r(const time_t *t, struct tm *out) {
    long long secs = *t;
    long long days = secs / 86400;
    long long rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    out->tm_hour = (int)(rem / 3600);
    out->tm_min = (int)(rem % 3600 / 60);
    out->tm_sec = (int)(rem % 60);
    out->tm_wday = (int)((days % 7 + 11) % 7);   /* 1970-01-01 was a Thursday */
    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    long long d = doy - (153 * mp + 2) / 5 + 1;
    long long m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    out->tm_year = (int)(y - 1900);
    out->tm_mon = (int)(m - 1);
    out->tm_mday = (int)d;
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    out->tm_yday = cum[m - 1] + (int)d - 1 + (leap && m > 2);
    out->tm_isdst = 0;
    out->tm_gmtoff = 0;
    out->tm_zone = "UTC";
    return out;
}

/* The machine clock is UTC and RadiumOS has no zone database. */
struct tm *qjs_localtime_r(const time_t *t, struct tm *out) {
    return qjs_gmtime_r(t, out);
}
