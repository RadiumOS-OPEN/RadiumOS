// udpsock.c - a small table of UDP sockets; datagrams for a bound port wait in a ring until the app reads them
#include "udpsock.h"
#include "netstack.h"
#include "nic.h"
#include "net.h"
#include "networkrecievethread.h"
#include "../rtl8139/rtl8139.h"
#include "../timers/timer.h"
#include "../utility/utility.h"
#include "dns.h"

extern int rust_rtl8139_get_mac(uint8_t *mac);

#define MAX_SOCKS 6
#define QLEN      32
#define DGRAM_MAX 1472

typedef struct { uint32_t ip; uint16_t port, len; uint8_t data[DGRAM_MAX]; } dgram_t;
typedef struct { bool used; uint16_t port; volatile uint32_t head, tail; dgram_t q[QLEN]; } usock_t;

static usock_t g_socks[MAX_SOCKS];

// Packets are only received when somebody asks the NIC, so every read polls it first.
static void pump(void) { netstack_pump(); }

static bool port_taken(uint16_t port)
{
    for (int i = 0; i < MAX_SOCKS; i++) if (g_socks[i].used && g_socks[i].port == port) return true;
    return false;
}

static bool valid(int h) { return h >= 0 && h < MAX_SOCKS && g_socks[h].used; }

// The C stack is never configured at boot (the boot code only sets up the Rust one): QEMU user-mode defaults.
void udpsock_ensure_config(void)
{
    if (net_config.ip_address) return;
    netstack_init(0x0A00020Fu, 0xFFFFFF00u, 0x0A000202u);     // 10.0.2.15/24, gateway 10.0.2.2
    uint8_t mac[6];
    if (rust_rtl8139_get_mac(mac) == 0) netstack_set_mac(mac);
    dns_set_server(0x0A000203u);                              // 10.0.2.3
}

int udpsock_open(void)
{
    udpsock_ensure_config();
    for (int i = 0; i < MAX_SOCKS; i++) {
        if (g_socks[i].used) continue;
        memset(&g_socks[i], 0, sizeof(g_socks[i]));
        g_socks[i].used = true;
        return i;
    }
    return -1;
}

int udpsock_bind(int h, uint16_t port)
{
    if (!valid(h)) return -1;
    if (port == 0) {
        static uint16_t next = 0;
        if (!next) next = (uint16_t)(49152 + (get_time_ms() % 8000));
        for (int tries = 0; tries < 16000; tries++) {
            port = next++;
            if (next < 49152) next = 49152;
            if (!port_taken(port)) break;
        }
    } else if (port_taken(port)) return -1;
    g_socks[h].port = port;
    return port;
}

bool udpsock_deliver(uint16_t dest_port, uint32_t src_ip, uint16_t src_port, const uint8_t *data, uint16_t len)
{
    if (len > DGRAM_MAX) len = DGRAM_MAX;
    for (int i = 0; i < MAX_SOCKS; i++) {
        usock_t *s = &g_socks[i];
        if (!s->used || s->port != dest_port) continue;
        uint32_t next = (s->head + 1) % QLEN;
        if (next == s->tail) return true;                  // queue full: drop, UDP allows that
        dgram_t *d = &s->q[s->head];
        d->ip = src_ip; d->port = src_port; d->len = len;
        memcpy(d->data, data, len);
        s->head = next;
        return true;
    }
    return false;
}

int udpsock_sendto(int h, uint32_t ip, uint16_t port, const void *data, uint32_t len)
{
    if (!valid(h) || len > DGRAM_MAX) return -1;
    if (g_socks[h].port == 0 && udpsock_bind(h, 0) < 0) return -1;
    if (ip == 0x7F000001u || ip == net_config.ip_address) {
        udpsock_deliver(port, ip, g_socks[h].port, data, (uint16_t)len);
        return (int)len;
    }
    udp_send(ip, g_socks[h].port, port, (uint8_t *)data, (uint16_t)len);
    return (int)len;
}

int udpsock_pending(int h)
{
    if (!valid(h)) return 0;
    usock_t *s = &g_socks[h];
    if (s->head == s->tail) pump();
    return s->head == s->tail ? 0 : (s->q[s->tail].len ? s->q[s->tail].len : 1);
}

int udpsock_recvfrom(int h, void *buf, uint32_t cap, uint32_t *ip, uint16_t *port)
{
    if (!valid(h)) return -1;
    usock_t *s = &g_socks[h];
    if (s->head == s->tail) pump();
    if (s->head == s->tail) return -1;
    dgram_t *d = &s->q[s->tail];
    uint32_t n = d->len < cap ? d->len : cap;
    memcpy(buf, d->data, n);
    if (ip) *ip = d->ip;
    if (port) *port = d->port;
    s->tail = (s->tail + 1) % QLEN;
    return (int)n;
}

void udpsock_close(int h)
{
    if (valid(h)) g_socks[h].used = false;
}

void udpsock_close_all(void)
{
    for (int i = 0; i < MAX_SOCKS; i++) g_socks[i].used = false;
}
