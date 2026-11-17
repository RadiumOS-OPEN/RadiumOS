/* RadiumOS app VFS (port/rt): three places files can live.
 *   /cdda/...  read-only data packed by port/tools/pack_dir.py into one file (/disk/cdda.pack on the fetch store or FAT)
 *   /user/...  a RAM filesystem for saves and settings, kept in the file /disk/cdda.sav (the FAT volume only allows
 *              8.3 names, the game uses long ones)
 *   anything else goes to the kernel (/disk and the fetch store) through the radium_api_t table. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "radium_api.h"

#ifndef PACK_PATH
#define PACK_PATH "/disk/cdda.pack"
#endif
#ifndef SAVE_PATH
#define SAVE_PATH "/disk/cdda.sav"
#endif
#ifndef PACK_MOUNT
#define PACK_MOUNT "cdda"       /* the pack appears as /PACK_MOUNT */
#endif
#define MOUNT_LEN (sizeof("/" PACK_MOUNT) - 1)
#define PATH_MAX_V 300
#define MAX_FD 128
#define CACHE_SZ (256u * 1024u)

static const radium_api_t *A;
static char g_cwd[PATH_MAX_V] = "/" PACK_MOUNT;

enum { K_NONE, K_HOST, K_PACK, K_USER, K_DIR, K_SOCK };

/* ---- the pack -------------------------------------------------------------------------------------------------- */

typedef struct { char *path; uint32_t off, size; } pent_t;
static pent_t *pk;
static uint32_t npk;
static int pack_ready;

static void pack_load(void)
{
    if (pack_ready) return;
    pack_ready = 1;
    uint8_t hdr[16];
    if (A->file_read(PACK_PATH, 0, hdr, 16) != 16 || memcmp(hdr, "RPACK001", 8) != 0) return;
    uint32_t n = *(uint32_t *)(hdr + 8), ioff = *(uint32_t *)(hdr + 12);
    uint32_t isz = 0; int d;
    if (A->file_stat(PACK_PATH, &isz, &d) != 0 || ioff > isz) return;
    uint32_t len = isz - ioff;
    uint8_t *idx = malloc(len);
    if (!idx) return;
    uint32_t got = 0;
    while (got < len) {
        uint32_t chunk = len - got > (1u << 20) ? (1u << 20) : len - got;
        int r = A->file_read(PACK_PATH, ioff + got, idx + got, chunk);
        if (r <= 0) { free(idx); return; }
        got += (uint32_t)r;
    }
    pk = malloc(sizeof(pent_t) * n);
    if (!pk) { free(idx); return; }
    uint8_t *p = idx;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t pl = *(uint16_t *)p; p += 2;
        pk[i].path = malloc((size_t)pl + 1);
        memcpy(pk[i].path, p, pl); pk[i].path[pl] = '\0'; p += pl;
        pk[i].off = *(uint32_t *)p; pk[i].size = *(uint32_t *)(p + 4); p += 8;
    }
    npk = n;
    free(idx);
}

static int pack_lower(const char *path)               /* first entry >= path */
{
    int lo = 0, hi = (int)npk;
    while (lo < hi) { int m = (lo + hi) / 2; if (strcmp(pk[m].path, path) < 0) lo = m + 1; else hi = m; }
    return lo;
}

static pent_t *pack_find(const char *path)
{
    pack_load();
    int i = pack_lower(path);
    return (i < (int)npk && strcmp(pk[i].path, path) == 0) ? &pk[i] : NULL;
}

static int pack_is_dir(const char *path)
{
    pack_load();
    if (path[0] == '\0') return 1;
    char pre[PATH_MAX_V];
    snprintf(pre, sizeof(pre), "%s/", path);
    int i = pack_lower(pre);
    return i < (int)npk && strncmp(pk[i].path, pre, strlen(pre)) == 0;
}

/* ---- the RAM filesystem for /user -------------------------------------------------------------------------------- */

typedef struct { char *path; uint8_t *data; uint32_t len, cap; int is_dir; } uf_t;
static uf_t *uf;
static int nuf, capuf, uf_dirty;
static uint32_t uf_last_flush;

static uf_t *uf_find(const char *p) { for (int i = 0; i < nuf; i++) if (uf[i].path && strcmp(uf[i].path, p) == 0) return &uf[i]; return NULL; }

static uf_t *uf_add(const char *p, int is_dir)
{
    int slot = -1;
    for (int i = 0; i < nuf; i++) if (!uf[i].path) { slot = i; break; }
    if (slot < 0) {
        if (nuf == capuf) { capuf = capuf ? capuf * 2 : 256; uf = realloc(uf, sizeof(uf_t) * (size_t)capuf); }
        slot = nuf++;
    }
    memset(&uf[slot], 0, sizeof(uf_t));
    uf[slot].path = strdup(p);
    uf[slot].is_dir = is_dir;
    uf_dirty = 1;
    return &uf[slot];
}

static void uf_remove(uf_t *f) { free(f->path); free(f->data); memset(f, 0, sizeof(*f)); uf_dirty = 1; }

static int uf_has_children(const char *p)
{
    size_t l = strlen(p);
    for (int i = 0; i < nuf; i++) if (uf[i].path && strncmp(uf[i].path, p, l) == 0 && uf[i].path[l] == '/') return 1;
    return 0;
}

static int uf_is_dir(const char *p) { uf_t *f = uf_find(p); return strcmp(p, "") == 0 || (f && f->is_dir) || (!f && uf_has_children(p)); }

static int uf_grow(uf_t *f, uint32_t need)
{
    if (need <= f->cap) return 0;
    uint32_t nc = f->cap ? f->cap : 4096;
    while (nc < need) nc *= 2;
    uint8_t *nd = realloc(f->data, nc);
    if (!nd) return -1;
    f->data = nd; f->cap = nc;
    return 0;
}

void vfs_flush(void)
{
    if (!uf_dirty) return;
    size_t total = 8;
    for (int i = 0; i < nuf; i++) if (uf[i].path) total += 2 + strlen(uf[i].path) + 5 + uf[i].len;
    uint8_t *buf = malloc(total), *p = buf;
    if (!buf) return;
    memcpy(p, "RSAV0001", 8); p += 8;
    for (int i = 0; i < nuf; i++) {
        if (!uf[i].path) continue;
        uint16_t pl = (uint16_t)strlen(uf[i].path);
        *(uint16_t *)p = pl; p += 2; memcpy(p, uf[i].path, pl); p += pl;
        *p++ = (uint8_t)uf[i].is_dir;
        *(uint32_t *)p = uf[i].len; p += 4;
        memcpy(p, uf[i].data, uf[i].len); p += uf[i].len;
    }
    if (A->file_write(SAVE_PATH, buf, (uint32_t)(p - buf), 0) == 0) uf_dirty = 0;
    free(buf);
    uf_last_flush = A->millis();
}

static void uf_load(void)
{
    uint32_t sz; int d;
    if (A->file_stat(SAVE_PATH, &sz, &d) != 0 || sz < 8) return;
    uint8_t *buf = malloc(sz);
    if (!buf) return;
    uint32_t got = 0;
    while (got < sz) {
        uint32_t chunk = sz - got > (1u << 20) ? (1u << 20) : sz - got;
        int r = A->file_read(SAVE_PATH, got, buf + got, chunk);
        if (r <= 0) { free(buf); return; }
        got += (uint32_t)r;
    }
    if (memcmp(buf, "RSAV0001", 8) == 0) {
        uint8_t *p = buf + 8, *end = buf + sz;
        while (p + 7 <= end) {
            uint16_t pl = *(uint16_t *)p; p += 2;
            char path[PATH_MAX_V];
            if (pl >= sizeof(path) || p + pl + 5 > end) break;
            memcpy(path, p, pl); path[pl] = '\0'; p += pl;
            int is_dir = *p++;
            uint32_t len = *(uint32_t *)p; p += 4;
            if (p + len > end) break;
            uf_t *f = uf_add(path, is_dir);
            if (len && uf_grow(f, len) == 0) { memcpy(f->data, p, len); f->len = len; }
            p += len;
        }
    }
    free(buf);
    uf_dirty = 0;
}

static void maybe_flush(void)
{
    if (uf_dirty && A->millis() - uf_last_flush > 20000u) vfs_flush();
}

/* ---- descriptors --------------------------------------------------------------------------------------------------- */

typedef struct dirent_v { char name[64]; int is_dir; } dirent_v;
typedef struct {
    int kind, flags, writable;
    char path[PATH_MAX_V];            /* inner path (pack/user) or host path */
    uint32_t off, size, base;         /* position, size, pack data offset */
    uint8_t *cache; uint32_t cache_off, cache_len;
    int uf_idx;
    dirent_v *list; int nlist, pos;
} fd_t;
static fd_t fds[MAX_FD];
#define FUF(f) (&uf[(f)->uf_idx])

static void norm_path(const char *in, char *out)
{
    char tmp[PATH_MAX_V * 2];
    if (in[0] == '/') { strncpy(tmp, in, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = '\0'; }
    else snprintf(tmp, sizeof(tmp), "%s/%s", g_cwd, in);
    char *parts[96];
    int n = 0;
    for (char *p = strtok(tmp, "/"); p && n < 96; p = strtok(NULL, "/")) {
        if (strcmp(p, ".") == 0) continue;
        if (strcmp(p, "..") == 0) { if (n) n--; continue; }
        parts[n++] = p;
    }
    size_t o = 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        size_t l = strlen(parts[i]);
        if (o + l + 2 >= PATH_MAX_V) break;
        out[o++] = '/';
        memcpy(out + o, parts[i], l);
        o += l;
    }
    if (o == 0) out[o++] = '/';
    out[o] = '\0';
}

/* Which backend owns `full` (a normalised absolute path), with the path inside it. */
static int route(const char *full, char *inner)
{
    if (strcmp(full, "/") == 0) { inner[0] = '\0'; return K_DIR; }
    if (strncmp(full, "/" PACK_MOUNT, MOUNT_LEN) == 0 && (full[MOUNT_LEN] == '/' || full[MOUNT_LEN] == '\0')) { strcpy(inner, full[MOUNT_LEN] ? full + MOUNT_LEN + 1 : ""); return K_PACK; }
    if (strncmp(full, "/user", 5) == 0 && (full[5] == '/' || full[5] == '\0')) { strcpy(inner, full[5] ? full + 6 : ""); return K_USER; }
    strcpy(inner, full);
    return K_HOST;
}

struct kstat {
    dev_t st_dev; int __st_dev_padding; long __st_ino_truncated; mode_t st_mode; nlink_t st_nlink; uid_t st_uid; gid_t st_gid;
    dev_t st_rdev; int __st_rdev_padding; off_t st_size; blksize_t st_blksize; blkcnt_t st_blocks;
    long st_atime_sec, st_atime_nsec, st_mtime_sec, st_mtime_nsec, st_ctime_sec, st_ctime_nsec; ino_t st_ino;
};

static void fill_kstat(void *kst, uint32_t size, int is_dir)
{
    struct kstat *k = kst;
    if (!k) return;
    memset(k, 0, sizeof(*k));
    k->st_mode = is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    k->st_nlink = 1;
    k->st_size = size;
    k->st_blksize = 4096;
    k->st_blocks = (size + 511) / 512;
    k->st_mtime_sec = k->st_atime_sec = k->st_ctime_sec = (long)A->unix_time();
}

/* stat by full path: 0 = exists */
static int stat_full(const char *full, uint32_t *size, int *is_dir)
{
    char in[PATH_MAX_V];
    *size = 0; *is_dir = 0;
    switch (route(full, in)) {
    case K_DIR: *is_dir = 1; return 0;
    case K_PACK: {
        if (in[0] == '\0') { *is_dir = 1; return 0; }
        pent_t *e = pack_find(in);
        if (e) { *size = e->size; return 0; }
        if (pack_is_dir(in)) { *is_dir = 1; return 0; }
        return -1;
    }
    case K_USER: {
        if (in[0] == '\0') { *is_dir = 1; return 0; }
        char key[PATH_MAX_V]; snprintf(key, sizeof(key), "%s", in);
        uf_t *f = uf_find(key);
        if (f) { *size = f->len; *is_dir = f->is_dir; return 0; }
        if (uf_has_children(key)) { *is_dir = 1; return 0; }
        return -1;
    }
    default: return A->file_stat(in, size, is_dir);
    }
}

void vfs_init(const radium_api_t *api)
{
    A = api;
    uf_load();
    uf_last_flush = A->millis();
}

static int alloc_fd(void)
{
    for (int fd = 3; fd < MAX_FD; fd++) if (fds[fd].kind == K_NONE) { memset(&fds[fd], 0, sizeof(fds[fd])); return fd; }
    return -1;
}

static void list_add(dirent_v **l, int *n, int *cap, const char *name, int is_dir)
{
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 64; *l = realloc(*l, sizeof(dirent_v) * (size_t)*cap); }
    strncpy((*l)[*n].name, name, 63);
    (*l)[*n].name[63] = '\0';
    (*l)[*n].is_dir = is_dir;
    (*n)++;
}

static void build_dir_list(fd_t *f, const char *full)
{
    char in[PATH_MAX_V];
    int cap = 0;
    f->list = NULL; f->nlist = 0;
    switch (route(full, in)) {
    case K_DIR:
        list_add(&f->list, &f->nlist, &cap, PACK_MOUNT, 1);
        list_add(&f->list, &f->nlist, &cap, "user", 1);
        list_add(&f->list, &f->nlist, &cap, "disk", 1);
        break;
    case K_PACK: {
        pack_load();
        char pre[PATH_MAX_V];
        size_t pl = in[0] ? (size_t)snprintf(pre, sizeof(pre), "%s/", in) : (pre[0] = '\0', 0);
        char last[64] = "";
        for (int i = pack_lower(pre); i < (int)npk && strncmp(pk[i].path, pre, pl) == 0; i++) {
            const char *rest = pk[i].path + pl;
            const char *slash = strchr(rest, '/');
            size_t nl = slash ? (size_t)(slash - rest) : strlen(rest);
            if (nl >= 64) continue;
            char name[64]; memcpy(name, rest, nl); name[nl] = '\0';
            if (strcmp(name, last) == 0) continue;
            strcpy(last, name);
            list_add(&f->list, &f->nlist, &cap, name, slash != NULL);
        }
        break;
    }
    case K_USER: {
        size_t pl = strlen(in);
        for (int i = 0; i < nuf; i++) {
            if (!uf[i].path) continue;
            if (pl && !(strncmp(uf[i].path, in, pl) == 0 && uf[i].path[pl] == '/')) continue;
            const char *rest = uf[i].path + (pl ? pl + 1 : 0);
            const char *slash = strchr(rest, '/');
            size_t nl = slash ? (size_t)(slash - rest) : strlen(rest);
            if (nl >= 64 || nl == 0) continue;
            char name[64]; memcpy(name, rest, nl); name[nl] = '\0';
            int dup = 0;
            for (int k = 0; k < f->nlist; k++) if (strcmp(f->list[k].name, name) == 0) { dup = 1; break; }
            if (!dup) list_add(&f->list, &f->nlist, &cap, name, slash != NULL || uf[i].is_dir);
        }
        break;
    }
    default: {
        char name[64]; uint32_t sz; int d;
        for (int i = 0; A->dir_entry(in, i, name, &sz, &d) == 1; i++) list_add(&f->list, &f->nlist, &cap, name, d);
    }
    }
}

long vfs_open(const char *path, int flags)
{
    char full[PATH_MAX_V], in[PATH_MAX_V];
    norm_path(path, full);
    int kind = route(full, in);
    uint32_t size; int is_dir;
    int exists = stat_full(full, &size, &is_dir) == 0;
    int acc = flags & O_ACCMODE;

    if (!exists) {
        if (!(flags & O_CREAT)) return -ENOENT;
        if (kind == K_PACK || kind == K_DIR) return -EROFS;
        if (kind == K_HOST && A->file_write(in, "", 0, 0) != 0) return -EACCES;
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        return -EEXIST;
    }
    if (exists && is_dir && acc != O_RDONLY) return -EISDIR;
    int fd = alloc_fd();
    if (fd < 0) return -EMFILE;
    fd_t *f = &fds[fd];
    f->flags = flags;
    strcpy(f->path, in);

    if (exists && is_dir) {
        f->kind = K_DIR;
        build_dir_list(f, full);
        return fd;
    }
    if (acc != O_RDONLY && (kind == K_PACK)) { f->kind = K_NONE; return -EROFS; }
    f->kind = kind == K_DIR ? K_HOST : kind;
    f->writable = acc != O_RDONLY;
    switch (kind) {
    case K_PACK: { pent_t *e = pack_find(in); f->base = e->off; f->size = e->size; break; }
    case K_USER: {
        uf_t *u = uf_find(in);
        if (!u) u = uf_add(in, 0);
        if ((flags & O_TRUNC) && f->writable) { u->len = 0; uf_dirty = 1; }
        f->uf_idx = (int)(u - uf);
        f->size = u->len;
        break;
    }
    default:
        if ((flags & O_TRUNC) && f->writable && exists) A->file_write(in, "", 0, 0);
        f->size = (flags & O_TRUNC) ? 0 : size;
    }
    f->off = (flags & O_APPEND) ? f->size : 0;
    return fd;
}

long vfs_close(int fd)
{
    if (fd < 3 || fd >= MAX_FD || fds[fd].kind == K_NONE) return fd < 3 ? 0 : -EBADF;
    fd_t *f = &fds[fd];
    if (f->kind == K_SOCK && A->net_udp_close) A->net_udp_close(f->uf_idx);
    free(f->cache);
    free(f->list);
    int was_user_write = f->kind == K_USER && f->writable;
    memset(f, 0, sizeof(*f));
    if (was_user_write) maybe_flush();
    return 0;
}

long vfs_read(int fd, void *buf, uint32_t n)
{
    if (fd == 0) return -EAGAIN;
    if (fd < 3 || fd >= MAX_FD || fds[fd].kind == K_NONE) return -EBADF;
    fd_t *f = &fds[fd];
    if (f->kind == K_DIR) return -EISDIR;
    if (f->kind == K_SOCK) return -EOPNOTSUPP;      /* sockets are read with recvfrom/recvmsg */
    if (f->off >= f->size) return 0;
    if (n > f->size - f->off) n = f->size - f->off;
    switch (f->kind) {
    case K_USER:
        memcpy(buf, FUF(f)->data + f->off, n);
        break;
    case K_PACK: {
        uint8_t *out = buf;
        uint32_t left = n, at = f->off;
        while (left) {
            if (!f->cache || at < f->cache_off || at >= f->cache_off + f->cache_len) {
                if (!f->cache) f->cache = malloc(CACHE_SZ);
                if (!f->cache) return -ENOMEM;
                uint32_t want = f->size - at < CACHE_SZ ? f->size - at : CACHE_SZ;
                int r = A->file_read(PACK_PATH, f->base + at, f->cache, want);
                if (r <= 0) return -EIO;
                f->cache_off = at; f->cache_len = (uint32_t)r;
            }
            uint32_t take = f->cache_off + f->cache_len - at;
            if (take > left) take = left;
            memcpy(out, f->cache + (at - f->cache_off), take);
            out += take; at += take; left -= take;
        }
        break;
    }
    default: {
        int r = A->file_read(f->path, f->off, buf, n);
        if (r < 0) return -EIO;
        n = (uint32_t)r;
    }
    }
    f->off += n;
    return (long)n;
}

long vfs_write(int fd, const void *buf, uint32_t n)
{
    if (fd < 3 || fd >= MAX_FD || fds[fd].kind == K_NONE) return -EBADF;
    fd_t *f = &fds[fd];
    if (f->kind == K_SOCK) return -EOPNOTSUPP;
    if (!f->writable || f->kind == K_DIR) return -EBADF;
    if (f->kind == K_USER) {
        size_t pl = strlen(f->path);
        if (pl >= 9 && strcmp(f->path + pl - 9, "debug.log") == 0) A->debug_write(buf, n);
        if (f->flags & O_APPEND) f->off = FUF(f)->len;
        if (uf_grow(FUF(f), f->off + n) != 0) return -ENOSPC;
        if (f->off > FUF(f)->len) memset(FUF(f)->data + FUF(f)->len, 0, f->off - FUF(f)->len);
        memcpy(FUF(f)->data + f->off, buf, n);
        f->off += n;
        if (f->off > FUF(f)->len) FUF(f)->len = f->off;
        f->size = FUF(f)->len;
        uf_dirty = 1;
        return (long)n;
    }
    if (A->file_write(f->path, buf, n, f->off > 0) != 0) return -ENOSPC;
    f->off += n;
    if (f->off > f->size) f->size = f->off;
    return (long)n;
}

long vfs_lseek(int fd, int64_t off, int whence)
{
    if (fd < 3 || fd >= MAX_FD || fds[fd].kind == K_NONE) return fd < 3 ? -ESPIPE : -EBADF;
    fd_t *f = &fds[fd];
    int64_t base = whence == SEEK_CUR ? (int64_t)f->off : whence == SEEK_END ? (int64_t)f->size : 0;
    if (base + off < 0) return -EINVAL;
    f->off = (uint32_t)(base + off);
    if (f->kind == K_DIR) f->pos = 0;
    return (long)f->off;
}

long vfs_fstat(int fd, void *kst)
{
    if (fd >= 0 && fd < 3) { fill_kstat(kst, 0, 0); ((struct kstat *)kst)->st_mode = S_IFCHR | 0620; return 0; }
    if (fd < 0 || fd >= MAX_FD || fds[fd].kind == K_NONE) return -EBADF;
    fill_kstat(kst, fds[fd].kind == K_USER ? FUF(&fds[fd])->len : fds[fd].size, fds[fd].kind == K_DIR);
    return 0;
}

long vfs_stat(const char *path, void *kst)
{
    char full[PATH_MAX_V];
    uint32_t size; int is_dir;
    norm_path(path, full);
    if (stat_full(full, &size, &is_dir) != 0) return -ENOENT;
    fill_kstat(kst, size, is_dir);
    return 0;
}

long vfs_getdents(int fd, uint8_t *buf, uint32_t cap)
{
    if (fd < 3 || fd >= MAX_FD || fds[fd].kind != K_DIR) return -EBADF;
    fd_t *f = &fds[fd];
    uint32_t used = 0;
    while (f->pos < f->nlist) {
        const char *name = f->list[f->pos].name;
        uint32_t nl = (uint32_t)strlen(name);
        uint32_t reclen = (19 + nl + 1 + 7) & ~7u;
        if (used + reclen > cap) break;
        memset(buf + used, 0, reclen);
        *(uint64_t *)(buf + used) = (uint64_t)f->pos + 1;
        *(int64_t *)(buf + used + 8) = f->pos + 1;
        *(uint16_t *)(buf + used + 16) = (uint16_t)reclen;
        buf[used + 18] = f->list[f->pos].is_dir ? 4 : 8;
        memcpy(buf + used + 19, name, nl + 1);
        used += reclen;
        f->pos++;
    }
    return (long)used;
}

long vfs_getcwd(char *buf, size_t n)
{
    size_t l = strlen(g_cwd) + 1;
    if (l > n) return -ERANGE;
    memcpy(buf, g_cwd, l);
    return (long)l;
}

long vfs_chdir(const char *path)
{
    char full[PATH_MAX_V];
    uint32_t size; int is_dir;
    norm_path(path, full);
    if (stat_full(full, &size, &is_dir) != 0 || !is_dir) return -ENOENT;
    strcpy(g_cwd, full);
    return 0;
}

long vfs_mkdir(const char *path)
{
    char full[PATH_MAX_V], in[PATH_MAX_V];
    uint32_t size; int is_dir;
    norm_path(path, full);
    if (stat_full(full, &size, &is_dir) == 0) return -EEXIST;
    switch (route(full, in)) {
    case K_USER: uf_add(in, 1); return 0;
    case K_HOST: return A->make_dir(in) == 0 ? 0 : -EACCES;
    default: return -EROFS;
    }
}

long vfs_unlink(const char *path)
{
    char full[PATH_MAX_V], in[PATH_MAX_V];
    norm_path(path, full);
    switch (route(full, in)) {
    case K_USER: {
        uf_t *f = uf_find(in);
        if (!f) return uf_has_children(in) ? -ENOTEMPTY : -ENOENT;
        if (f->is_dir && uf_has_children(in)) return -ENOTEMPTY;
        uf_remove(f);
        return 0;
    }
    case K_HOST: return A->file_remove(in) == 0 ? 0 : -ENOENT;
    default: return -EROFS;
    }
}

long vfs_rename(const char *from, const char *to)
{
    char f1[PATH_MAX_V], f2[PATH_MAX_V], i1[PATH_MAX_V], i2[PATH_MAX_V];
    norm_path(from, f1); norm_path(to, f2);
    if (route(f1, i1) != K_USER || route(f2, i2) != K_USER) return -EXDEV;
    uf_t *src = uf_find(i1);
    if (!src) return -ENOENT;
    uf_t *dst = uf_find(i2);
    if (dst) uf_remove(dst);
    char *np = strdup(i2);
    free(src->path);
    src->path = np;
    uf_dirty = 1;
    return 0;
}

long vfs_ftruncate(int fd, uint32_t len)
{
    if (fd < 3 || fd >= MAX_FD || fds[fd].kind != K_USER) return 0;
    uf_t *u = FUF(&fds[fd]);
    if (len > u->len) { if (uf_grow(u, len) != 0) return -ENOSPC; memset(u->data + u->len, 0, len - u->len); }
    u->len = len;
    fds[fd].size = len;
    uf_dirty = 1;
    return 0;
}

/* ---- sockets: port/rt/sock.c keeps the state, the descriptor lives here ---------------------------------------------- */

int vfs_sock_alloc(int handle)
{
    int fd = alloc_fd();
    if (fd < 0) return -1;
    fds[fd].kind = K_SOCK;
    fds[fd].uf_idx = handle;
    return fd;
}

int vfs_sock_handle(int fd)
{
    return fd >= 3 && fd < MAX_FD && fds[fd].kind == K_SOCK ? fds[fd].uf_idx : -1;
}
