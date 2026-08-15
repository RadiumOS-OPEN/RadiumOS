// net.h
#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stdbool.h>

#define NET_QUEUE_SIZE 32
#define NET_QUEUE_BUFFER_SIZE 2048
#define MAX_NICS 4

// Forward declaration
typedef struct NIC NIC;

// Network queue functions
void net_queue_init(NIC* nic);
bool net_queue_add(NIC* nic, uint8_t* data, uint16_t length);
int net_queue_get(NIC* nic, uint8_t* buffer, uint16_t buffer_size);
bool net_queue_has_packets(NIC* nic);
uint32_t net_queue_count(NIC* nic);
void net_queue_clear(NIC* nic);
void net_queue_stats(NIC* nic);

#endif // NET_H