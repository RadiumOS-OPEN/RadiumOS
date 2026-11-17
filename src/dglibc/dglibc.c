// dglibc.c - the C library slice behind Doom (see dglibc.h). Files live on AVFS (/disk is FAT) or in the fetch store,
// memory comes from the Rust heap, and text output goes to the terminal.

#include "../Avfs/Avfs.h"
#include "../terminal/terminal.h"
#include "../utility/utility.h"
#include "dglibc.h"

extern uint8_t *rust_c_alloc(size_t size);
extern void rust_c_free(uint8_t *p);
extern int rust_disk_store_size(const uint8_t *path);
extern int rust_disk_store_read(const uint8_t *path, uint32_t offset, uint8_t *buffer, uint32_t len);

int dg_errno;

// ---- memory ------------------------------------------------------------------------------------------------

// Every block is linked into a list so that everything Doom allocated can be released when it exits.
typedef struct blk { struct blk *next, *prev; size_t size, pad; } blk_t;
static blk_t *g_blocks;

void *dg_malloc(size_t n)
{
    blk_t *b = (blk_t *)rust_c_alloc(n + sizeof(blk_t));
    if (!b) return NULL;
    b->size = n;
    b->prev = NULL;
    b->next = g_blocks;
    if (g_blocks) g_blocks->prev = b;
    g_blocks = b;
    return b + 1;
}

void *dg_calloc(size_t n, size_t size)
{
    size_t total = n * size;
    if (size && total / size != n) return NULL;
    void *p = dg_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void dg_free(void *p)
{
    if (!p) return;
    blk_t *b = (blk_t *)p - 1;
    if (b->prev) b->prev->next = b->next; else g_blocks = b->next;
    if (b->next) b->next->prev = b->prev;
    rust_c_free((uint8_t *)b);
}

void *dg_realloc(void *p, size_t n)
{
    if (!p) return dg_malloc(n);
    size_t old = ((blk_t *)p - 1)->size;
    if (n <= old) return p;
    void *q = dg_malloc(n);
    if (!q) return NULL;
    memcpy(q, p, old);
    dg_free(p);
    return q;
}

void dg_release_all(void) { while (g_blocks) dg_free(g_blocks + 1); }

// ---- exit ----------------------------------------------------------------------------------------------------

static void *g_exit_jmp[5];
static int g_exit_code;
static bool g_in_run;

int __attribute__((noinline)) dg_run(int (*main_fn)(void))
{
    if (__builtin_setjmp(g_exit_jmp) == 0) {
        g_in_run = true;
        int code = main_fn();
        g_in_run = false;
        return code;
    }
    g_in_run = false;
    return g_exit_code;
}

void dg_exit(int code)
{
    g_exit_code = code;
    if (g_in_run) __builtin_longjmp(g_exit_jmp, 1);
    for (;;) __asm__ volatile("hlt");
}

void dg_abort(void) { dg_exit(134); }
int dg_atexit(void (*fn)(void)) { (void)fn; return 0; }
char *dg_getenv(const char *name) { (void)name; return NULL; }
int dg_system(const char *cmd) { (void)cmd; return -1; }

void dg_assert_fail(const char *expr, const char *file, int line)
{
    dg_printf("assertion failed: %s (%s:%d)\n", expr, file, line);
    dg_exit(134);
}

// ---- strings and characters ------------------------------------------------------------------------------------

size_t dg_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
char *dg_strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *dg_strncpy(char *d, const char *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
char *dg_strcat(char *d, const char *s) { dg_strcpy(d + dg_strlen(d), s); return d; }
char *dg_strncat(char *d, const char *s, size_t n) { char *e = d + dg_strlen(d); size_t i = 0; for (; i < n && s[i]; i++) e[i] = s[i]; e[i] = 0; return d; }
int dg_strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int dg_strncmp(const char *a, const char *b, size_t n) { for (; n; n--, a++, b++) { if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b; } return 0; }
int dg_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int dg_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int dg_isupper(int c) { return c >= 'A' && c <= 'Z'; }
int dg_islower(int c) { return c >= 'a' && c <= 'z'; }
int dg_isalpha(int c) { return dg_isupper(c) || dg_islower(c); }
int dg_isdigit(int c) { return c >= '0' && c <= '9'; }
int dg_isalnum(int c) { return dg_isalpha(c) || dg_isdigit(c); }
int dg_isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
int dg_isprint(int c) { return c >= 32 && c < 127; }
int dg_isxdigit(int c) { return dg_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int dg_strcasecmp(const char *a, const char *b) { while (*a && dg_tolower(*a) == dg_tolower(*b)) { a++; b++; } return dg_tolower((unsigned char)*a) - dg_tolower((unsigned char)*b); }
int dg_strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int d = dg_tolower((unsigned char)*a) - dg_tolower((unsigned char)*b);
        if (d || !*a) return d;
    }
    return 0;
}
char *dg_strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return NULL; } }
char *dg_strrchr(const char *s, int c) { const char *r = NULL; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; } }
char *dg_strstr(const char *h, const char *n)
{
    size_t nl = dg_strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++) if (*h == *n && dg_strncmp(h, n, nl) == 0) return (char *)h;
    return NULL;
}
char *dg_strpbrk(const char *s, const char *set) { for (; *s; s++) if (dg_strchr(set, *s)) return (char *)s; return NULL; }
void *dg_memchr(const void *s, int c, size_t n) { const unsigned char *p = s; for (; n; n--, p++) if (*p == (unsigned char)c) return (void *)p; return NULL; }
char *dg_strdup(const char *s) { size_t n = dg_strlen(s) + 1; char *p = dg_malloc(n); if (p) memcpy(p, s, n); return p; }
char *dg_strerror(int e) { (void)e; return "error"; }
int dg_abs(int x) { return x < 0 ? -x : x; }

static unsigned g_rand = 1;
int dg_rand(void) { g_rand = g_rand * 1103515245u + 12345u; return (int)((g_rand >> 16) & 0x7FFF); }
void dg_srand(unsigned seed) { g_rand = seed; }

void dg_qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    char *a = base, tmp[64];
    if (size > sizeof(tmp)) return;
    for (size_t i = 1; i < n; i++) {
        memcpy(tmp, a + i * size, size);
        size_t j = i;
        while (j > 0 && cmp(a + (j - 1) * size, tmp) > 0) { memcpy(a + j * size, a + (j - 1) * size, size); j--; }
        memcpy(a + j * size, tmp, size);
    }
}

static int digit_of(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

unsigned long dg_strtoul(const char *s, char **end, int base)
{
    const char *p = s;
    while (dg_isspace(*p)) p++;
    bool neg = false;
    if (*p == '-') { neg = true; p++; } else if (*p == '+') p++;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; base = 16; }
    else if (base == 0) base = (p[0] == '0') ? 8 : 10;
    unsigned long v = 0;
    const char *start = p;
    while (digit_of(*p) < base) { v = v * (unsigned long)base + (unsigned long)digit_of(*p); p++; }
    if (end) *end = (char *)(p == start ? s : p);
    return neg ? (unsigned long)(-(long)v) : v;
}

long dg_strtol(const char *s, char **end, int base) { return (long)dg_strtoul(s, end, base); }
int dg_atoi(const char *s) { return (int)dg_strtol(s, NULL, 10); }

double dg_atof(const char *s)
{
    while (dg_isspace(*s)) s++;
    bool neg = false;
    if (*s == '-') { neg = true; s++; } else if (*s == '+') s++;
    double v = 0;
    while (dg_isdigit(*s)) v = v * 10 + (*s++ - '0');
    if (*s == '.') {
        s++;
        double scale = 0.1;
        while (dg_isdigit(*s)) { v += (*s++ - '0') * scale; scale *= 0.1; }
    }
    return neg ? -v : v;
}

// ---- formatted output ------------------------------------------------------------------------------------------

typedef struct { char *buf; size_t cap, len; } sink_t;

static void put(sink_t *s, char c) { if (s->len + 1 < s->cap) s->buf[s->len] = c; s->len++; }

static void put_number(sink_t *s, unsigned long v, int base, bool upper, bool neg, int width, int prec, bool left, bool zero, bool plus, bool space, bool alt)
{
    char digits[24];
    int n = 0;
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0 && prec != 0) digits[n++] = '0';
    while (v) { digits[n++] = set[v % (unsigned long)base]; v /= (unsigned long)base; }
    int zeros = prec > n ? prec - n : 0;
    const char *prefix = "";
    if (neg) prefix = "-"; else if (plus) prefix = "+"; else if (space) prefix = " ";
    if (alt && base == 16 && n) prefix = upper ? "0X" : "0x";
    int len = n + zeros + (int)dg_strlen(prefix);
    int pad = width > len ? width - len : 0;
    if (!left && !zero) for (int i = 0; i < pad; i++) put(s, ' ');
    for (const char *p = prefix; *p; p++) put(s, *p);
    if (!left && zero && prec < 0) for (int i = 0; i < pad; i++) put(s, '0');
    else if (!left && zero) for (int i = 0; i < pad; i++) put(s, ' ');
    for (int i = 0; i < zeros; i++) put(s, '0');
    while (n) put(s, digits[--n]);
    if (left) for (int i = 0; i < pad; i++) put(s, ' ');
}

int dg_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    sink_t s = { buf, cap, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&s, *fmt); continue; }
        fmt++;
        bool left = false, zero = false, plus = false, space = false, alt = false;
        for (;; fmt++) {
            if (*fmt == '-') left = true; else if (*fmt == '0') zero = true; else if (*fmt == '+') plus = true;
            else if (*fmt == ' ') space = true; else if (*fmt == '#') alt = true; else break;
        }
        int width = 0, prec = -1;
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = true; width = -width; } fmt++; }
        else while (dg_isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (dg_isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        int longs = 0;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 'j' || *fmt == 't') { if (*fmt == 'l' || *fmt == 'z') longs++; fmt++; }
        (void)longs;
        switch (*fmt) {
        case 'd': case 'i': {
            long v = va_arg(ap, int);
            put_number(&s, v < 0 ? (unsigned long)(-v) : (unsigned long)v, 10, false, v < 0, width, prec, left, zero, plus, space, false);
            break;
        }
        case 'u': put_number(&s, va_arg(ap, unsigned), 10, false, false, width, prec, left, zero, false, false, false); break;
        case 'x': put_number(&s, va_arg(ap, unsigned), 16, false, false, width, prec, left, zero, false, false, alt); break;
        case 'X': put_number(&s, va_arg(ap, unsigned), 16, true, false, width, prec, left, zero, false, false, alt); break;
        case 'o': put_number(&s, va_arg(ap, unsigned), 8, false, false, width, prec, left, zero, false, false, false); break;
        case 'p': put_number(&s, (unsigned long)va_arg(ap, void *), 16, false, false, width, prec, left, zero, false, false, true); break;
        case 'c': { char c = (char)va_arg(ap, int); int pad = width > 1 ? width - 1 : 0; if (!left) while (pad--) put(&s, ' '); put(&s, c); if (left) while (pad--) put(&s, ' '); break; }
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            int len = 0;
            while (str[len] && (prec < 0 || len < prec)) len++;
            int pad = width > len ? width - len : 0;
            if (!left) for (int i = 0; i < pad; i++) put(&s, ' ');
            for (int i = 0; i < len; i++) put(&s, str[i]);
            if (left) for (int i = 0; i < pad; i++) put(&s, ' ');
            break;
        }
        case 'f': {
            double d = va_arg(ap, double);
            bool neg = d < 0;
            if (neg) d = -d;
            if (prec < 0) prec = 6;
            double scale = 1;
            for (int i = 0; i < prec; i++) scale *= 10;
            unsigned long whole = (unsigned long)d;
            unsigned long frac = (unsigned long)((d - (double)whole) * scale + 0.5);
            if (frac >= (unsigned long)scale) { whole++; frac -= (unsigned long)scale; }
            if (neg) put(&s, '-');
            put_number(&s, whole, 10, false, false, 0, -1, false, false, false, false, false);
            if (prec > 0) { put(&s, '.'); put_number(&s, frac, 10, false, false, prec, -1, false, true, false, false, false); }
            break;
        }
        case '%': put(&s, '%'); break;
        case 0: fmt--; break;
        default: put(&s, '%'); put(&s, *fmt); break;
        }
    }
    if (cap) buf[s.len < cap ? s.len : cap - 1] = '\0';
    return (int)s.len;
}

int dg_snprintf(char *buf, size_t n, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = dg_vsnprintf(buf, n, fmt, ap); va_end(ap); return r; }
int dg_sprintf(char *buf, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = dg_vsnprintf(buf, 1 << 20, fmt, ap); va_end(ap); return r; }

// ---- stdio -------------------------------------------------------------------------------------------------------

struct dg_file {
    int kind;                 // 0 stdin, 1 stdout, 2 stderr, 3 FAT read, 4 fetch-store read, 5 write buffer
    char path[96];
    uint32_t pos, size;
    bool eof, err;
    uint8_t *wbuf;
    uint32_t wcap;
    int ungot;
};

static struct dg_file g_in = { 0 }, g_out = { 1 }, g_err = { 2 };
FILE *dg_stdin = &g_in, *dg_stdout = &g_out, *dg_stderr = &g_err;

static void out_text(const char *s, size_t n)
{
    char chunk[200];
    while (n) {
        size_t k = n < sizeof(chunk) - 1 ? n : sizeof(chunk) - 1;
        memcpy(chunk, s, k);
        chunk[k] = '\0';
        print(chunk);
        s += k; n -= k;
    }
}

static int write_bytes(FILE *f, const void *p, size_t n);

int dg_vfprintf(FILE *f, const char *fmt, va_list ap)
{
    char buf[512];
    int len = dg_vsnprintf(buf, sizeof(buf), fmt, ap);
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    write_bytes(f, buf, (size_t)len);
    return len;
}

int dg_vprintf(const char *fmt, va_list ap) { return dg_vfprintf(dg_stdout, fmt, ap); }
int dg_printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = dg_vfprintf(dg_stdout, fmt, ap); va_end(ap); return r; }
int dg_fprintf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = dg_vfprintf(f, fmt, ap); va_end(ap); return r; }
int dg_puts(const char *s) { out_text(s, dg_strlen(s)); out_text("\n", 1); return 0; }
int dg_putchar(int c) { char ch = (char)c; out_text(&ch, 1); return c; }
void dg_perror(const char *s) { dg_printf("%s: error\n", s); }
void dg_setbuf(FILE *f, char *buf) { (void)f; (void)buf; }
int dg_fileno(FILE *f) { return f->kind; }

// Relative names live in /disk (FAT keeps 8.3 names, so Doom's own file names fit).
static void resolve(const char *name, char *out, size_t cap)
{
    while (name[0] == '.' && name[1] == '/') name += 2;
    if (name[0] == '/') dg_snprintf(out, cap, "%s", name);
    else dg_snprintf(out, cap, "/disk/%s", name);
}

FILE *dg_fopen(const char *name, const char *mode)
{
    char path[96];
    resolve(name, path, sizeof(path));
    bool writing = mode[0] == 'w' || mode[0] == 'a';
    FILE *f = dg_calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->ungot = -1;
    dg_strncpy(f->path, path, sizeof(f->path) - 1);
    if (!writing) {
        int size = avfs_get_filesize(path);
        if (size >= 0 && !avfs_is_directory(path)) { f->kind = 3; f->size = (uint32_t)size; return f; }
        size = rust_disk_store_size((const uint8_t *)path);
        if (size >= 0) { f->kind = 4; f->size = (uint32_t)size; return f; }
        dg_errno = ENOENT;
        dg_free(f);
        return NULL;
    }
    f->kind = 5;
    if (mode[0] == 'a') {
        int size = avfs_get_filesize(path);
        if (size > 0) {
            f->wcap = (uint32_t)size + 4096;
            f->wbuf = dg_malloc(f->wcap);
            if (!f->wbuf || avfs_read_file(path, f->wbuf, (uint32_t)size, 0) != 0) { dg_free(f->wbuf); dg_free(f); return NULL; }
            f->size = f->pos = (uint32_t)size;
        }
    }
    return f;
}

static int flush_file(FILE *f)
{
    if (f->kind != 5) return 0;
    static const uint8_t empty = 0;
    if (avfs_save_file(f->path, f->size ? f->wbuf : &empty, f->size, true) != 0) { f->err = true; return EOF; }
    return 0;
}

int dg_fflush(FILE *f) { return f ? flush_file(f) : 0; }

int dg_fclose(FILE *f)
{
    if (!f || f->kind < 3) return 0;
    int r = flush_file(f);
    dg_free(f->wbuf);
    dg_free(f);
    return r;
}

static size_t read_bytes(FILE *f, void *dst, size_t n)
{
    if (f->kind != 3 && f->kind != 4) return 0;
    if (f->pos >= f->size) { f->eof = true; return 0; }
    if (n > f->size - f->pos) { n = f->size - f->pos; f->eof = true; }
    uint8_t *out = dst;
    size_t done = 0;
    while (done < n) {
        uint32_t chunk = (uint32_t)(n - done > 32768 ? 32768 : n - done);
        if (f->kind == 3) {
            if (avfs_read_file(f->path, out + done, chunk, f->pos + (uint32_t)done) != 0) { f->err = true; break; }
        } else if (rust_disk_store_read((const uint8_t *)f->path, f->pos + (uint32_t)done, out + done, chunk) != (int)chunk) { f->err = true; break; }
        done += chunk;
    }
    f->pos += (uint32_t)done;
    return done;
}

size_t dg_fread(void *p, size_t size, size_t n, FILE *f)
{
    if (!size || !n) return 0;
    return read_bytes(f, p, size * n) / size;
}

static int write_bytes(FILE *f, const void *p, size_t n)
{
    if (f->kind == 1 || f->kind == 2) { out_text(p, n); return (int)n; }
    if (f->kind != 5) return 0;
    if (f->pos + n > f->wcap) {
        uint32_t cap = f->wcap ? f->wcap : 4096;
        while (cap < f->pos + n) cap *= 2;
        uint8_t *nb = dg_realloc(f->wbuf, cap);
        if (!nb) { f->err = true; return 0; }
        f->wbuf = nb; f->wcap = cap;
    }
    memcpy(f->wbuf + f->pos, p, n);
    f->pos += (uint32_t)n;
    if (f->pos > f->size) f->size = f->pos;
    return (int)n;
}

size_t dg_fwrite(const void *p, size_t size, size_t n, FILE *f) { return size ? (size_t)write_bytes(f, p, size * n) / size : 0; }
int dg_fputs(const char *s, FILE *f) { write_bytes(f, s, dg_strlen(s)); return 0; }
int dg_fputc(int c, FILE *f) { char ch = (char)c; write_bytes(f, &ch, 1); return c; }

int dg_fgetc(FILE *f)
{
    if (f->ungot >= 0) { int c = f->ungot; f->ungot = -1; return c; }
    uint8_t c;
    return read_bytes(f, &c, 1) == 1 ? c : EOF;
}

int dg_ungetc(int c, FILE *f) { f->ungot = c; f->eof = false; return c; }

char *dg_fgets(char *s, int n, FILE *f)
{
    int i = 0;
    while (i < n - 1) {
        int c = dg_fgetc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    s[i] = '\0';
    return s;
}

int dg_fseek(FILE *f, long off, int whence)
{
    long base = whence == SEEK_SET ? 0 : (whence == SEEK_CUR ? (long)f->pos : (long)f->size);
    long to = base + off;
    if (to < 0) return -1;
    f->pos = (uint32_t)to;
    f->eof = false;
    f->ungot = -1;
    return 0;
}

long dg_ftell(FILE *f) { return (long)f->pos; }
void dg_rewind(FILE *f) { dg_fseek(f, 0, SEEK_SET); }
int dg_feof(FILE *f) { return f->eof; }
int dg_ferror(FILE *f) { return f->err; }

int dg_remove(const char *name)
{
    char path[96];
    resolve(name, path, sizeof(path));
    return avfs_remove_file(path) == 0 ? 0 : -1;
}

int dg_rename(const char *from, const char *to)
{
    char a[96], b[96];
    resolve(from, a, sizeof(a));
    resolve(to, b, sizeof(b));
    int size = avfs_get_filesize(a);
    if (size < 0) return -1;
    uint8_t *data = dg_malloc((size_t)size + 1);
    if (!data) return -1;
    int rc = (size == 0 || avfs_read_file(a, data, (uint32_t)size, 0) == 0) ? avfs_save_file(b, data, (uint32_t)size, true) : -1;
    dg_free(data);
    if (rc != 0) return -1;
    avfs_remove_file(a);
    return 0;
}

int dg_mkdir(const char *name) { char path[96]; resolve(name, path, sizeof(path)); return avfs_create_dir(path) == 0 ? 0 : -1; }
int dg_access(const char *name)
{
    char path[96];
    resolve(name, path, sizeof(path));
    return (avfs_file_exists(path) || avfs_is_directory(path) || rust_disk_store_size((const uint8_t *)path) >= 0) ? 0 : -1;
}

// ---- sscanf: %d %i %u %x %o %s %c and literals ---------------------------------------------------------------------

int dg_sscanf(const char *str, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int assigned = 0;
    const char *p = str;
    for (; *fmt; fmt++) {
        if (dg_isspace(*fmt)) { while (dg_isspace(*p)) p++; continue; }
        if (*fmt != '%') { if (*p != *fmt) break; p++; continue; }
        fmt++;
        int width = 0;
        while (dg_isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'h') fmt++;
        if (*fmt == 'd' || *fmt == 'i' || *fmt == 'u' || *fmt == 'x' || *fmt == 'o') {
            while (dg_isspace(*p)) p++;
            int base = *fmt == 'x' ? 16 : (*fmt == 'o' ? 8 : (*fmt == 'i' ? 0 : 10));
            char *end;
            long v = dg_strtol(p, &end, base);
            if (end == p) break;
            *va_arg(ap, int *) = (int)v;
            p = end;
            assigned++;
        } else if (*fmt == 's') {
            while (dg_isspace(*p)) p++;
            if (!*p) break;
            char *out = va_arg(ap, char *);
            int n = 0;
            while (*p && !dg_isspace(*p) && (!width || n < width)) out[n++] = *p++;
            out[n] = '\0';
            assigned++;
        } else if (*fmt == 'c') {
            if (!*p) break;
            *va_arg(ap, char *) = *p++;
            assigned++;
        } else break;
    }
    va_end(ap);
    return assigned;
}
