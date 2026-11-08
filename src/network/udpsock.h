// udpsock.h - UDP sockets on top of the kernel network stack (used by apps through radium_api_t)
#ifndef UDPSOCK_H
#define UDPSOCK_H

#include <stdint.h>
#include <stdbool.h>

// IP addresses are in host byte order, like everywhere in netstack.c.
void udpsock_ensure_config(void);                          // IP/gateway/DNS defaults if nothing configured the C stack
int  udpsock_open(void);                                   // handle, or -1
int  udpsock_bind(int h, uint16_t port);                   // port 0 = any free port; returns the port, or -1 if taken
int  udpsock_sendto(int h, uint32_t ip, uint16_t port, const void *data, uint32_t len);   // bytes sent, or -1
int  udpsock_recvfrom(int h, void *buf, uint32_t cap, uint32_t *ip, uint16_t *port);      // length, or -1 if nothing waits
int  udpsock_pending(int h);                               // length of the next datagram, 0 if none (polls the NIC)
void udpsock_close(int h);
void udpsock_close_all(void);                              // when an app exits

// called by udp_process() for every received datagram; true when a socket took it
bool udpsock_deliver(uint16_t dest_port, uint32_t src_ip, uint16_t src_port, const uint8_t *data, uint16_t len);

extern int g_net_quiet;   // 1 = the stack prints nothing for packets

#endif
