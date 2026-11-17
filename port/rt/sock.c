/* UDP sockets for RadiumOS apps (port/rt): musl's socket system calls on top of the kernel's UDP table (radium_api_t net_*),
 * plus getaddrinfo/getnameinfo (musl's resolver would want /etc/resolv.conf and a threaded poll loop). IPv4 and UDP only. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include "radium_api.h"

int vfs_sock_alloc(int handle);
int vfs_sock_handle(int fd);
void sock_init(const radium_api_t *api);

#define MAXH 8
#define DGRAM_MAX 1472

static const radium_api_t *A;
static struct { int used, nonblock; uint16_t lport; uint32_t peer_ip; uint16_t peer_port; } S[MAXH];

void sock_init(const radium_api_t *api) { A = api; }

static int hnd(int fd)
{
    int h = vfs_sock_handle(fd);
    return h >= 0 && h < MAXH && S[h].used ? h : -1;
}

static void fill_addr(struct sockaddr *sa, socklen_t *len, uint32_t ip, uint16_t port)
{
    struct sockaddr_in in; memset(&in, 0, sizeof in);
    in.sin_family = AF_INET; in.sin_port = htons(port); in.sin_addr.s_addr = htonl(ip);
    if (sa && len) { socklen_t n = *len < sizeof in ? *len : (socklen_t)sizeof in; memcpy(sa, &in, n); *len = sizeof in; }
}

static int bound_port(int h)
{
    if (!S[h].lport) { int p = A->net_udp_bind(h, 0); if (p < 0) return -1; S[h].lport = (uint16_t)p; }
    return S[h].lport;
}

static long do_sendto(int h, const void *buf, size_t len, const struct sockaddr_in *to)
{
    uint32_t ip; uint16_t port;
    if (to) { if (to->sin_family != AF_INET) return -EAFNOSUPPORT; ip = ntohl(to->sin_addr.s_addr); port = ntohs(to->sin_port); }
    else if (S[h].peer_port) { ip = S[h].peer_ip; port = S[h].peer_port; }
    else return -EDESTADDRREQ;
    if (len > DGRAM_MAX) return -EMSGSIZE;
    if (bound_port(h) < 0) return -EADDRINUSE;
    int r = A->net_udp_sendto(h, ip, port, buf, (uint32_t)len);
    return r < 0 ? -ENETUNREACH : r;
}

/* blocking sockets wait a little for a datagram; the apps here always set O_NONBLOCK */
static int wait_data(int h)
{
    uint32_t t0 = A->millis();
    while (!A->net_udp_pending(h)) {
        if (S[h].nonblock || A->millis() - t0 > 2000) return 0;
        A->sleep_ms(1);
    }
    return 1;
}

static long do_recvfrom(int h, void *buf, size_t len, uint32_t *ip, uint16_t *port)
{
    if (!wait_data(h)) return -EAGAIN;
    int r = A->net_udp_recvfrom(h, buf, (uint32_t)len, ip, port);
    return r < 0 ? -EAGAIN : r;
}

static int ready_read(int fd) { int h = hnd(fd); return h >= 0 ? A->net_udp_pending(h) > 0 : fd > 2; }

/* select/poll: sockets are readable when a datagram waits and always writable; other descriptors count as ready */
static long do_select(int n, fd_set *r, fd_set *w, fd_set *e, long timeout_ms)
{
    uint32_t t0 = A->millis();
    if (n > FD_SETSIZE) n = FD_SETSIZE;
    for (;;) {
        fd_set rr, ww; FD_ZERO(&rr); FD_ZERO(&ww);
        int cnt = 0;
        for (int fd = 0; fd < n; fd++) {
            if (r && FD_ISSET(fd, r) && ready_read(fd)) { FD_SET(fd, &rr); cnt++; }
            if (w && FD_ISSET(fd, w) && fd > 2) { FD_SET(fd, &ww); cnt++; }
        }
        if (cnt || (timeout_ms >= 0 && (long)(A->millis() - t0) >= timeout_ms)) {
            if (r) *r = rr;
            if (w) *w = ww;
            if (e) FD_ZERO(e);
            return cnt;
        }
        A->sleep_ms(1);
    }
}

static long do_poll(struct pollfd *p, unsigned n, long timeout_ms)
{
    uint32_t t0 = A->millis();
    for (;;) {
        int cnt = 0;
        for (unsigned i = 0; i < n; i++) {
            p[i].revents = 0;
            if (p[i].fd < 0) continue;
            if ((p[i].events & POLLIN) && ready_read(p[i].fd)) p[i].revents |= POLLIN;
            if ((p[i].events & POLLOUT) && p[i].fd > 2) p[i].revents |= POLLOUT;
            if (p[i].revents) cnt++;
        }
        if (cnt || (timeout_ms >= 0 && (long)(A->millis() - t0) >= timeout_ms)) return cnt;
        A->sleep_ms(1);
    }
}

long sock_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6, int *handled)
{
    (void)a6;
    *handled = 1;
    switch (n) {
    case SYS_socket: {
        if (a1 != AF_INET) return -EAFNOSUPPORT;
        if ((a2 & 0xff) != SOCK_DGRAM) return -EPROTONOSUPPORT;        /* no TCP yet */
        int h = A->net_udp_open();
        if (h < 0 || h >= MAXH) { if (h >= 0) A->net_udp_close(h); return -EMFILE; }
        int fd = vfs_sock_alloc(h);
        if (fd < 0) { A->net_udp_close(h); return -EMFILE; }
        memset(&S[h], 0, sizeof S[h]);
        S[h].used = 1;
        S[h].nonblock = (a2 & SOCK_NONBLOCK) != 0;
        return fd;
    }
    case SYS_bind: {
        int h = hnd((int)a1); if (h < 0) return -ENOTSOCK;
        const struct sockaddr_in *in = (const struct sockaddr_in *)a2;
        if (a3 < (long)sizeof *in || in->sin_family != AF_INET) return -EINVAL;
        int p = A->net_udp_bind(h, ntohs(in->sin_port));
        if (p < 0) return -EADDRINUSE;
        S[h].lport = (uint16_t)p;
        return 0;
    }
    case SYS_connect: {
        int h = hnd((int)a1); if (h < 0) return -ENOTSOCK;
        const struct sockaddr_in *in = (const struct sockaddr_in *)a2;
        if (a3 < (long)sizeof *in || in->sin_family != AF_INET) return -EAFNOSUPPORT;
        S[h].peer_ip = ntohl(in->sin_addr.s_addr); S[h].peer_port = ntohs(in->sin_port);
        return 0;
    }
    case SYS_getsockname: {
        int h = hnd((int)a1); if (h < 0) return -ENOTSOCK;
        fill_addr((struct sockaddr *)a2, (socklen_t *)a3, A->net_local_ip(), S[h].lport);
        return 0;
    }
    case SYS_getpeername: {
        int h = hnd((int)a1); if (h < 0) return -ENOTSOCK;
        if (!S[h].peer_port) return -ENOTCONN;
        fill_addr((struct sockaddr *)a2, (socklen_t *)a3, S[h].peer_ip, S[h].peer_port);
        return 0;
    }
    case SYS_setsockopt: return hnd((int)a1) < 0 ? -ENOTSOCK : 0;
    case SYS_getsockopt: {
        if (hnd((int)a1) < 0) return -ENOTSOCK;
        if (a4 && a5 && *(socklen_t *)a5 >= 4) { *(int *)a4 = 0; *(socklen_t *)a5 = 4; }
        return 0;
    }
    case SYS_shutdown: return hnd((int)a1) < 0 ? -ENOTSOCK : 0;
    case SYS_sendto: {
        int h = hnd((int)a1); if (h < 0) return -ENOTSOCK;
        return do_sendto(h, (const void *)a2, (size_t)a3, a5 ? (const struct sockaddr_in *)a5 : NULL);
    }
    case SYS_recvfrom: {
        int h = hnd((int)a1); if (h < 0) return -ENOTSOCK;
        uint32_t ip = 0; uint16_t port = 0;
        long r = do_recvfrom(h, (void *)a2, (size_t)a3, &ip, &port);
        if (r >= 0 && a5 && a6) fill_addr((struct sockaddr *)a5, (socklen_t *)a6, ip, port);
        return r;
    }
    case SYS_sendmsg: {
        int h = hnd((int)a1); if (h < 0) return -ENOTSOCK;
        const struct msghdr *m = (const struct msghdr *)a2;
        static uint8_t buf[DGRAM_MAX + 1];
        size_t tot = 0;
        for (int i = 0; i < (int)m->msg_iovlen; i++) {
            if (tot + m->msg_iov[i].iov_len > DGRAM_MAX) return -EMSGSIZE;
            memcpy(buf + tot, m->msg_iov[i].iov_base, m->msg_iov[i].iov_len);
            tot += m->msg_iov[i].iov_len;
        }
        return do_sendto(h, buf, tot, m->msg_name ? (const struct sockaddr_in *)m->msg_name : NULL);
    }
    case SYS_recvmsg: {
        int h = hnd((int)a1); if (h < 0) return -ENOTSOCK;
        struct msghdr *m = (struct msghdr *)a2;
        static uint8_t buf[DGRAM_MAX + 1];
        uint32_t ip = 0; uint16_t port = 0;
        long r = do_recvfrom(h, buf, sizeof buf, &ip, &port);
        if (r < 0) return r;
        size_t left = (size_t)r, at = 0;
        for (int i = 0; i < (int)m->msg_iovlen && left; i++) {
            size_t k = left < m->msg_iov[i].iov_len ? left : m->msg_iov[i].iov_len;
            memcpy(m->msg_iov[i].iov_base, buf + at, k);
            at += k; left -= k;
        }
        m->msg_flags = left ? MSG_TRUNC : 0;
        if (m->msg_name) { m->msg_namelen = m->msg_namelen < sizeof(struct sockaddr_in) ? m->msg_namelen : sizeof(struct sockaddr_in); fill_addr(m->msg_name, &m->msg_namelen, ip, port); }
        return r;
    }
    case SYS_ioctl: {
        int h = hnd((int)a1); if (h < 0) { *handled = 0; return 0; }
        if (a2 == FIONBIO) { S[h].nonblock = *(int *)a3 != 0; return 0; }
        if (a2 == FIONREAD) { *(int *)a3 = A->net_udp_pending(h); return 0; }
        return -ENOTTY;
    }
    case SYS_fcntl64: case SYS_fcntl: {
        int h = hnd((int)a1); if (h < 0) { *handled = 0; return 0; }
        if (a2 == F_SETFL) { S[h].nonblock = (a3 & O_NONBLOCK) != 0; return 0; }
        if (a2 == F_GETFL) return O_RDWR | (S[h].nonblock ? O_NONBLOCK : 0);
        return 0;
    }
    case SYS_select: case SYS__newselect: {        /* (n, r, w, e, {sec, usec}) */
        const long *tv = (const long *)a5;
        return do_select((int)a1, (fd_set *)a2, (fd_set *)a3, (fd_set *)a4, tv ? tv[0] * 1000 + tv[1] / 1000 : -1);
    }
    case SYS_pselect6: {                           /* (n, r, w, e, {sec, nsec}, sigmask) */
        const long *ts = (const long *)a5;
        return do_select((int)a1, (fd_set *)a2, (fd_set *)a3, (fd_set *)a4, ts ? ts[0] * 1000 + ts[1] / 1000000 : -1);
    }
    case SYS_poll: return do_poll((struct pollfd *)a1, (unsigned)a2, a3);
    default:
        *handled = 0;
        return 0;
    }
}

/* ---- name lookup --------------------------------------------------------------------------------------------------- */

static int resolve_v4(const char *host, uint32_t *ip /* host order */)
{
    struct in_addr a;
    if (inet_pton(AF_INET, host, &a) == 1) { *ip = ntohl(a.s_addr); return 0; }
    if (!strcmp(host, "localhost")) { *ip = 0x7F000001u; return 0; }
    return A && A->net_resolve ? A->net_resolve(host, ip) : -1;
}

struct gai_blk { struct addrinfo ai; struct sockaddr_in sa; };

int getaddrinfo(const char *host, const char *serv, const struct addrinfo *hint, struct addrinfo **res)
{
    if (hint && hint->ai_family != AF_UNSPEC && hint->ai_family != AF_INET) return EAI_FAMILY;
    uint32_t ip = 0;
    if (host) { if (resolve_v4(host, &ip) != 0) return EAI_NONAME; }
    else if (!(hint && (hint->ai_flags & AI_PASSIVE))) ip = 0x7F000001u;
    struct gai_blk *b = calloc(1, sizeof *b);
    if (!b) return EAI_MEMORY;
    b->sa.sin_family = AF_INET;
    b->sa.sin_port = htons(serv ? (uint16_t)atoi(serv) : 0);
    b->sa.sin_addr.s_addr = htonl(ip);
    b->ai.ai_family = AF_INET;
    b->ai.ai_socktype = hint && hint->ai_socktype ? hint->ai_socktype : SOCK_DGRAM;
    b->ai.ai_protocol = hint ? hint->ai_protocol : 0;
    b->ai.ai_addrlen = sizeof b->sa;
    b->ai.ai_addr = (struct sockaddr *)&b->sa;
    *res = &b->ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *p) { free(p); }   /* getaddrinfo above returns the start of one calloc block */

int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen, char *serv, socklen_t servlen, int flags)
{
    (void)salen; (void)flags;
    if (sa->sa_family != AF_INET) return EAI_FAMILY;
    const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
    if (host && hostlen && !inet_ntop(AF_INET, &in->sin_addr, host, hostlen)) return EAI_OVERFLOW;
    if (serv && servlen) snprintf(serv, servlen, "%u", ntohs(in->sin_port));
    return 0;
}

struct hostent *gethostbyname(const char *name)
{
    static struct hostent h;
    static char hname[256];
    static uint32_t addr;
    static char *list[2];
    uint32_t ip;
    if (resolve_v4(name, &ip) != 0) { fprintf(stderr, "net: cannot resolve %s\n", name); return NULL; }
    addr = htonl(ip);
    fprintf(stderr, "net: %s is %u.%u.%u.%u\n", name, ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
    strncpy(hname, name, sizeof hname - 1);
    list[0] = (char *)&addr; list[1] = NULL;
    h.h_name = hname; h.h_aliases = NULL; h.h_addrtype = AF_INET; h.h_length = 4; h.h_addr_list = list;
    return &h;
}
