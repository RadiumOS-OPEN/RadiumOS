/* The interface between the RadiumOS kernel (src/commands/apprun.c) and an "app" (a static ELF built with
 * port/Makefile). The kernel passes a pointer to this table to radium_app_main(); the app glue (port/rt/glue.c)
 * turns musl's system calls into calls through it. Both sides include this file, so keep it plain C. */
#ifndef RADIUM_API_H
#define RADIUM_API_H

#include <stdint.h>

#define RADIUM_API_VERSION 1

typedef struct radium_api {
    uint32_t version;
    uint32_t arena_base, arena_size;                 /* RAM the app may hand out with mmap/brk */

    uint32_t cols, rows;                             /* text console size */
    void     (*screen_blit)(const uint16_t *cells, uint32_t cols, uint32_t rows);   /* cells: glyph | attr << 8 */
    void     (*console_write)(const char *s, uint32_t n);
    void     (*debug_write)(const char *s, uint32_t n);   /* QEMU debug console (port 0xE9) */
    int      (*key_poll)(void);                      /* next raw PS/2 scancode (bit 7 = release), or -1 */
    uint32_t (*millis)(void);                        /* milliseconds since boot */
    uint32_t (*unix_time)(void);                     /* seconds since 1970 (UTC) */
    uint32_t (*random32)(void);
    void     (*sleep_ms)(uint32_t ms);
    uint32_t (*tls_selector)(uint32_t base);         /* GDT selector whose segment starts at `base` (for %gs) */

    /* files: /disk (FAT32 and the fetch store) and the packed data archive; -1 when missing */
    int      (*file_stat)(const char *path, uint32_t *size, int *is_dir);
    int      (*file_read)(const char *path, uint32_t off, void *buf, uint32_t n);   /* bytes read, or -1 */
    int      (*file_write)(const char *path, const void *buf, uint32_t n, int append);
    int      (*file_remove)(const char *path);
    int      (*dir_entry)(const char *dir, int index, char name[64], uint32_t *size, int *is_dir); /* 1 = entry, 0 = end */
    int      (*make_dir)(const char *path);

    /* pixel display (appended later; older apps never touch these) */
    int      (*gfx_open)(uint32_t w, uint32_t h);                         /* takes the screen, enables the PS/2 mouse; 0 = ok */
    void     (*gfx_present)(const uint32_t *px, uint32_t w, uint32_t h);  /* 0x00RRGGBB, enlarged by a whole factor and centred */
    void     (*gfx_close)(void);
    int      (*mouse_poll)(int *dx, int *dy, int *buttons);               /* movement since the last call; 1 if anything changed */

    /* network: UDP sockets and name lookup (appended later); IPv4 addresses are in host byte order */
    int      (*net_udp_open)(void);                                       /* socket handle, or -1 */
    int      (*net_udp_bind)(int h, uint16_t port);                       /* port 0 = any free port; returns the port, or -1 if taken */
    int      (*net_udp_sendto)(int h, uint32_t ip, uint16_t port, const void *data, uint32_t len);   /* bytes sent, or -1 */
    int      (*net_udp_recvfrom)(int h, void *buf, uint32_t cap, uint32_t *ip, uint16_t *port);      /* length, or -1 if nothing waits */
    int      (*net_udp_pending)(int h);                                   /* length of the next datagram, 0 if none (polls the NIC) */
    void     (*net_udp_close)(int h);
    int      (*net_resolve)(const char *host, uint32_t *ip);              /* 0 = ok */
    uint32_t (*net_local_ip)(void);

    /* Helios mods (src/commands/acmods.c): *.hls files in /disk/mods that start with "// acmod:" */
    int      (*mod_scan)(void);                                           /* rescans the folder; number of mods */
    int      (*mod_info)(int i, char name[32], char desc[48], int *enabled);   /* 1 if mod i exists */
    int      (*mod_toggle)(int i);                                        /* 1 = now on, 0 = now off, -1 = build failed */
    void     (*mod_run)(uint32_t *screen, uint32_t *api);                 /* run every enabled mod once (before the frame is presented) */

    /* a second flat data segment for %fs (Windows programs keep their thread block there); same idea as tls_selector */
    uint32_t (*fs_selector)(uint32_t base);
} radium_api_t;

typedef int (*radium_app_main_fn)(const radium_api_t *api, int argc, char **argv);

#endif
