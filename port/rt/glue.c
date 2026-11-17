/* RadiumOS app glue: runs inside an app ELF, between musl and the kernel's radium_api_t table.
 * It implements the Linux system calls musl issues (single thread, one address space, flat memory),
 * the thread pointer (%gs), and the entry point radium_app_main(). */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <elf.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include "radium_api.h"

extern int main(int, char **, char **);
const radium_api_t *radium_api_ptr;   /* for app-side shims (port/ac/sdl_radium.cpp) */
extern int __libc_start_main(int (*)(int, char **, char **), int, char **, void (*)(void), void (*)(void), void (*)(void));
extern char __ehdr_start;

static const radium_api_t *A;
const radium_api_t *radium_api(void) { return A; }
static jmp_buf g_exit;
static int g_status;

/* ---- the arena: page-granular first-fit allocator over the RAM the kernel gave us -------------------------------- */

#define PAGE 4096u
#define MAX_FREE 2048
static struct { uint32_t a, n; } fr[MAX_FREE];     /* free ranges sorted by address */
static int nfr;

static void arena_init(void)
{
    uint32_t base = (A->arena_base + PAGE - 1) & ~(PAGE - 1);
    uint32_t end = (A->arena_base + A->arena_size) & ~(PAGE - 1);
    fr[0].a = base;
    fr[0].n = end - base;
    nfr = 1;
}

static uint32_t arena_alloc(uint32_t len)
{
    len = (len + PAGE - 1) & ~(PAGE - 1);
    for (int i = 0; i < nfr; i++) {
        if (fr[i].n < len) continue;
        uint32_t p = fr[i].a;
        fr[i].a += len;
        fr[i].n -= len;
        if (fr[i].n == 0) { memmove(&fr[i], &fr[i + 1], (size_t)(nfr - i - 1) * sizeof(fr[0])); nfr--; }
        return p;
    }
    return 0;
}

static void arena_free(uint32_t p, uint32_t len)
{
    len = (len + PAGE - 1) & ~(PAGE - 1);
    int i = 0;
    while (i < nfr && fr[i].a < p) i++;
    if (i > 0 && fr[i - 1].a + fr[i - 1].n == p) { fr[i - 1].n += len; i--; }
    else if (nfr < MAX_FREE) {
        memmove(&fr[i + 1], &fr[i], (size_t)(nfr - i) * sizeof(fr[0]));
        fr[i].a = p; fr[i].n = len; nfr++;
    } else return;
    if (i + 1 < nfr && fr[i].a + fr[i].n == fr[i + 1].a) {
        fr[i].n += fr[i + 1].n;
        memmove(&fr[i + 1], &fr[i + 2], (size_t)(nfr - i - 2) * sizeof(fr[0]));
        nfr--;
    }
}

static uint32_t g_brk_base, g_brk_cur, g_brk_end;

static long sys_brk(uint32_t want)
{
    if (!g_brk_base) {
        g_brk_end = 8u << 20;
        g_brk_base = arena_alloc(g_brk_end);
        if (!g_brk_base) return -ENOMEM;
        memset((void *)g_brk_base, 0, g_brk_end);
        g_brk_cur = g_brk_base;
        g_brk_end += g_brk_base;
    }
    if (want >= g_brk_base && want <= g_brk_end) {
        if (want > g_brk_cur) memset((void *)g_brk_cur, 0, want - g_brk_cur);
        g_brk_cur = want;
    }
    return (long)g_brk_cur;
}

static long sys_mmap2(uint32_t addr, uint32_t len, int prot, int flags, int fd, uint32_t pgoff)
{
    (void)addr; (void)prot; (void)fd; (void)pgoff;
    if (!(flags & MAP_ANONYMOUS)) return -ENODEV;
    if (len == 0) return -EINVAL;
    uint32_t p = arena_alloc(len);
    if (!p) return -ENOMEM;
    memset((void *)p, 0, (len + PAGE - 1) & ~(PAGE - 1));
    return (long)p;
}

static long sys_mremap(uint32_t old, uint32_t oldlen, uint32_t newlen, int flags)
{
    (void)flags;
    uint32_t ol = (oldlen + PAGE - 1) & ~(PAGE - 1), nl = (newlen + PAGE - 1) & ~(PAGE - 1);
    if (nl <= ol) { if (nl < ol) arena_free(old + nl, ol - nl); return (long)old; }
    uint32_t p = arena_alloc(nl);
    if (!p) return -ENOMEM;
    memcpy((void *)p, (void *)old, ol);
    memset((void *)(p + ol), 0, nl - ol);
    arena_free(old, ol);
    return (long)p;
}

/* ---- thread pointer ---------------------------------------------------------------------------------------------- */

int __set_thread_area(void *p)
{
    uint32_t sel = A->tls_selector((uint32_t)p);
    __asm__ volatile("movw %w0, %%gs" : : "r"(sel));
    return 0;
}

int __clone(int (*fn)(void *), void *stack, int flags, void *arg, ...) { (void)fn; (void)stack; (void)flags; (void)arg; return -ENOSYS; }
void __unmapself(void *base, size_t len) { (void)base; (void)len; for (;;) { } }
long __syscall_cp_asm(volatile void *cancel, long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
    (void)cancel;
    extern long __radium_syscall(long, long, long, long, long, long, long);
    return __radium_syscall(nr, a1, a2, a3, a4, a5, a6);
}
void __cp_begin(void) {}
void __cp_end(void) {}
void __cp_cancel(void) {}

/* ---- files: see vfs.c ---------------------------------------------------------------------------------------------- */
long vfs_open(const char *path, int flags);
long vfs_close(int fd);
int vfs_sock_alloc(int handle);
int vfs_sock_handle(int fd);
long sock_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6, int *handled) __attribute__((weak));
long vfs_read(int fd, void *buf, uint32_t n);
long vfs_write(int fd, const void *buf, uint32_t n);
long vfs_lseek(int fd, int64_t off, int whence);
long vfs_fstat(int fd, void *kst);
long vfs_stat(const char *path, void *kst);
long vfs_getdents(int fd, uint8_t *buf, uint32_t cap);
long vfs_getcwd(char *buf, size_t n);
long vfs_chdir(const char *path);
long vfs_mkdir(const char *path);
long vfs_unlink(const char *path);
long vfs_rename(const char *from, const char *to);
long vfs_ftruncate(int fd, uint32_t len);
void vfs_flush(void);
void vfs_init(const radium_api_t *api);
void sock_init(const radium_api_t *api) __attribute__((weak));

/* ---- time -------------------------------------------------------------------------------------------------------- */

/* Monotonic time with at least microsecond steps: the clock has millisecond resolution but callers (ImGui) need dt > 0. */
static void ts_from_ms(uint32_t ms, int64_t *s, int64_t *ns)
{
    static uint64_t last_us;
    uint64_t us = (uint64_t)ms * 1000u;
    if (us <= last_us) us = last_us + 1;
    last_us = us;
    *s = (int64_t)(us / 1000000u);
    *ns = (int64_t)(us % 1000000u) * 1000;
}

/* ---- the dispatcher ------------------------------------------------------------------------------------------------ */

long __radium_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    switch (n) {
    case SYS_exit:
    case SYS_exit_group:
        g_status = (int)a1;
        vfs_flush();
        longjmp(g_exit, 1);
    case SYS_read:   return vfs_read((int)a1, (void *)a2, (uint32_t)a3);
    case SYS_write:
        if (a1 == 1 || a1 == 2) { A->console_write((const char *)a2, (uint32_t)a3); A->debug_write((const char *)a2, (uint32_t)a3); return a3; }
        return vfs_write((int)a1, (const void *)a2, (uint32_t)a3);
    case SYS_readv: case SYS_writev: {
        const struct { void *base; size_t len; } *v = (const void *)a2;
        long total = 0;
        for (long i = 0; i < a3; i++) {
            long r;
            if (n == SYS_writev && (a1 == 1 || a1 == 2)) { A->console_write((const char *)v[i].base, (uint32_t)v[i].len); A->debug_write((const char *)v[i].base, (uint32_t)v[i].len); r = (long)v[i].len; }
            else r = n == SYS_readv ? vfs_read((int)a1, v[i].base, (uint32_t)v[i].len) : vfs_write((int)a1, v[i].base, (uint32_t)v[i].len);
            if (r < 0) return total ? total : r;
            total += r;
            if ((size_t)r < v[i].len) break;
        }
        return total;
    }
    case SYS_open:   return vfs_open((const char *)a1, (int)a2);
    case SYS_openat: return vfs_open((const char *)a2, (int)a3);
    case SYS_close:  return vfs_close((int)a1);
    case SYS_lseek:  return vfs_lseek((int)a1, (int32_t)a2, (int)a3);
    case SYS__llseek: {
        int64_t off = ((int64_t)(uint32_t)a2 << 32) | (uint32_t)a3;
        long r = vfs_lseek((int)a1, off, (int)a5);
        if (r < 0) return r;
        *(int64_t *)a4 = r;
        return 0;
    }
    case SYS_fstat64:   return vfs_fstat((int)a1, (void *)a2);
    case SYS_stat64: case SYS_lstat64: return vfs_stat((const char *)a1, (void *)a2);
    case SYS_fstatat64:
        if (((const char *)a2)[0] == '\0') return vfs_fstat((int)a1, (void *)a3);
        return vfs_stat((const char *)a2, (void *)a3);
    case SYS_access: return vfs_stat((const char *)a1, NULL);
    case SYS_faccessat: return vfs_stat((const char *)a2, NULL);
    case SYS_getdents64: return vfs_getdents((int)a1, (uint8_t *)a2, (uint32_t)a3);
    case SYS_getcwd:  return vfs_getcwd((char *)a1, (size_t)a2);
    case SYS_chdir:   return vfs_chdir((const char *)a1);
    case SYS_mkdir:   return vfs_mkdir((const char *)a1);
    case SYS_mkdirat: return vfs_mkdir((const char *)a2);
    case SYS_unlink: case SYS_rmdir: return vfs_unlink((const char *)a1);
    case SYS_unlinkat: return vfs_unlink((const char *)a2);
    case SYS_rename:   return vfs_rename((const char *)a1, (const char *)a2);
    case SYS_renameat: return vfs_rename((const char *)a2, (const char *)a4);
    case 353 /* renameat2 */: return vfs_rename((const char *)a2, (const char *)a4);
    case SYS_ftruncate: case SYS_ftruncate64: return vfs_ftruncate((int)a1, (uint32_t)a2);
    case SYS_fsync: case SYS_fdatasync: case SYS_flock: return 0;
    case SYS_fcntl64: case SYS_fcntl: {
        int handled = 0;
        long r = sock_syscall ? sock_syscall(n, a1, a2, a3, a4, a5, a6, &handled) : 0;
        return handled ? r : 0;
    }
    case SYS_readlink: case SYS_readlinkat: return -EINVAL;
    case SYS_ioctl: {
        int handled = 0;
        long r = sock_syscall ? sock_syscall(n, a1, a2, a3, a4, a5, a6, &handled) : 0;
        if (handled) return r;
    }
        if (a2 == TCGETS && a1 >= 0 && a1 < 3) { memset((void *)a3, 0, sizeof(struct termios)); return 0; }
        if (a2 == TIOCGWINSZ) { struct winsize *w = (struct winsize *)a3; w->ws_row = (unsigned short)A->rows; w->ws_col = (unsigned short)A->cols; w->ws_xpixel = w->ws_ypixel = 0; return 0; }
        return -ENOTTY;
    case SYS_brk:    return sys_brk((uint32_t)a1);
    case SYS_mmap2:  return sys_mmap2((uint32_t)a1, (uint32_t)a2, (int)a3, (int)a4, (int)a5, (uint32_t)a6);
    case SYS_munmap: arena_free((uint32_t)a1, (uint32_t)a2); return 0;
    case SYS_mremap: return sys_mremap((uint32_t)a1, (uint32_t)a2, (uint32_t)a3, (int)a4);
    case SYS_mprotect: case SYS_madvise: case SYS_mlock: case SYS_munlock: return 0;
    case SYS_clock_gettime64: {
        int64_t *t = (int64_t *)a2;
        uint32_t ms = A->millis();
        if (a1 == 0) { t[0] = A->unix_time(); t[1] = (int64_t)(ms % 1000) * 1000000; }
        else ts_from_ms(ms, &t[0], &t[1]);
        return 0;
    }
    case SYS_clock_gettime32: {
        int32_t *t = (int32_t *)a2;
        uint32_t ms = A->millis();
        if (a1 == 0) { t[0] = (int32_t)A->unix_time(); t[1] = (int32_t)(ms % 1000) * 1000000; }
        else { int64_t s64, ns64; ts_from_ms(ms, &s64, &ns64); t[0] = (int32_t)s64; t[1] = (int32_t)ns64; }
        return 0;
    }
    case 78 /* gettimeofday */: {
        int32_t *t = (int32_t *)a1;
        if (t) { t[0] = (int32_t)A->unix_time(); t[1] = (int32_t)(A->millis() % 1000) * 1000; }
        return 0;
    }
    case 162 /* nanosleep */: { const int32_t *t = (const int32_t *)a1; A->sleep_ms((uint32_t)(t[0] * 1000 + t[1] / 1000000)); return 0; }
    case SYS_clock_nanosleep_time64: {
        const int64_t *t = (const int64_t *)a3;
        A->sleep_ms((uint32_t)(t[0] * 1000 + t[1] / 1000000));
        return 0;
    }
    case SYS_clock_nanosleep_time32: {
        const int32_t *t = (const int32_t *)a3;
        A->sleep_ms((uint32_t)(t[0] * 1000 + t[1] / 1000000));
        return 0;
    }
    case SYS_sched_yield: return 0;
    case SYS_futex: case SYS_futex_time64: return (a2 & 0x7f) == 0 ? -EAGAIN : 0;    /* single thread: never wait */
    case SYS_getpid: case SYS_gettid: case SYS_set_tid_address: return 1;
    case SYS_getuid32: case SYS_geteuid32: case SYS_getgid32: case SYS_getegid32: return 0;
    case SYS_rt_sigaction: case SYS_rt_sigprocmask: case SYS_sigaltstack: return 0;
    case SYS_tkill: case SYS_tgkill: case SYS_kill:
        g_status = 134;                                  /* abort() */
        longjmp(g_exit, 1);
    case SYS_getrandom: {
        uint8_t *b = (uint8_t *)a1;
        for (long i = 0; i < a2; i++) b[i] = (uint8_t)A->random32();
        return a2;
    }
    case SYS_uname: {
        char *u = (char *)a1;
        memset(u, 0, 6 * 65);
        strcpy(u, "RadiumOS"); strcpy(u + 65, "radiumos"); strcpy(u + 130, "1"); strcpy(u + 195, "1"); strcpy(u + 260, "i686");
        return 0;
    }
    default: {
        int handled = 0;
        long r = sock_syscall ? sock_syscall(n, a1, a2, a3, a4, a5, a6, &handled) : 0;   /* weak: only apps linked with sock.c have sockets */
        return handled ? r : -ENOSYS;
    }
    }
}

/* ---- entry --------------------------------------------------------------------------------------------------------- */

static void (*g_dummy)(void);

int radium_app_main(const radium_api_t *api, int argc, char **argv)
{
    A = api;
    radium_api_ptr = api;
    __asm__ volatile("fninit");
    arena_init();
    vfs_init(api);
    if (sock_init) sock_init(api);

    /* the block musl expects at the top of the stack: argv, envp, auxv */
    static char *blk[256];
    int n = 0;
    for (int i = 0; i < argc && n < 100; i++) blk[n++] = argv[i];
    blk[n++] = NULL;
    static char home[] = "HOME=/user", term[] = "TERM=xterm", lang[] = "LANG=C.UTF-8", lcall[] = "LC_ALL=C.UTF-8";
    blk[n++] = home; blk[n++] = term; blk[n++] = lang; blk[n++] = lcall;
    blk[n++] = NULL;
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)&__ehdr_start;
    static uint32_t rnd[4];
    for (int i = 0; i < 4; i++) rnd[i] = A->random32();
    uint32_t *aux = (uint32_t *)&blk[n];
    int a = 0;
    aux[a++] = AT_PHDR;   aux[a++] = (uint32_t)&__ehdr_start + eh->e_phoff;
    aux[a++] = AT_PHENT;  aux[a++] = eh->e_phentsize;
    aux[a++] = AT_PHNUM;  aux[a++] = eh->e_phnum;
    aux[a++] = AT_PAGESZ; aux[a++] = 4096;
    aux[a++] = AT_RANDOM; aux[a++] = (uint32_t)rnd;
    aux[a++] = AT_SECURE; aux[a++] = 0;
    aux[a++] = AT_NULL;   aux[a++] = 0;

    if (setjmp(g_exit) == 0) __libc_start_main(main, argc, blk, g_dummy, g_dummy, g_dummy);
    return g_status;
}
