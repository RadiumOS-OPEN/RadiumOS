// net.c
#include "net.h"
#include "nic.h"
#include "../rtl8139/rtl8139.h"  // or wherever NIC is fully defined
// if NIC is defined here instead
#include "../terminal/terminal.h"
#include "../utility/utility.h"
#include "spinlock.h"


// Network queue management for received packets
#define NET_QUEUE_SIZE 32
#define NET_QUEUE_BUFFER_SIZE 2048

typedef struct {
    uint8_t data[NET_QUEUE_BUFFER_SIZE];
    uint16_t length;
    bool valid;
} net_packet_t;

typedef struct {
    net_packet_t packets[NET_QUEUE_SIZE];
    uint32_t read_index;
    uint32_t write_index;
    uint32_t count;
    Spinlock lock;
} net_queue_t;

// Global network queue per NIC
static net_queue_t net_queues[MAX_NICS];

// Initialize network queue for a NIC
void net_queue_init(NIC* nic) {
    if (!nic || nic->id >= MAX_NICS) {
        return;
    }
    
    net_queue_t* queue = &net_queues[nic->id];
    memset(queue, 0, sizeof(net_queue_t));
    queue->read_index = 0;
    queue->write_index = 0;
    queue->count = 0;
    queue->lock = (Spinlock){0};
}

// Add packet to network queue
bool net_queue_add(NIC* nic, uint8_t* data, uint16_t length) {
    if (!nic || !data || length == 0 || length > NET_QUEUE_BUFFER_SIZE || nic->id >= MAX_NICS) {
        return false;
    }
    
    net_queue_t* queue = &net_queues[nic->id];
    
    spinlockAcquire(&queue->lock);
    
    // Check if queue is full
    if (queue->count >= NET_QUEUE_SIZE) {
        spinlockRelease(&queue->lock);
        print("[net_queue] Queue full, dropping packet\n");
        return false;
    }
    
    // Add packet to queue
    net_packet_t* packet = &queue->packets[queue->write_index];
    memcpy(packet->data, data, length);
    packet->length = length;
    packet->valid = true;
    
    queue->write_index = (queue->write_index + 1) % NET_QUEUE_SIZE;
    queue->count++;
    
    spinlockRelease(&queue->lock);
    return true;
}

// Get packet from network queue
int net_queue_get(NIC* nic, uint8_t* buffer, uint16_t buffer_size) {
    if (!nic || !buffer || buffer_size == 0 || nic->id >= MAX_NICS) {
        return -1;
    }
    
    net_queue_t* queue = &net_queues[nic->id];
    
    spinlockAcquire(&queue->lock);
    
    // Check if queue is empty
    if (queue->count == 0) {
        spinlockRelease(&queue->lock);
        return 0;
    }
    
    // Get packet from queue
    net_packet_t* packet = &queue->packets[queue->read_index];
    
    if (!packet->valid) {
        spinlockRelease(&queue->lock);
        return -1;
    }
    
    // Check if buffer is large enough
    if (packet->length > buffer_size) {
        spinlockRelease(&queue->lock);
        print("[net_queue] Buffer too small for packet\n");
        return -1;
    }
    
    // Copy packet data
    uint16_t length = packet->length;
    memcpy(buffer, packet->data, length);
    
    // Mark packet as invalid and move read index
    packet->valid = false;
    queue->read_index = (queue->read_index + 1) % NET_QUEUE_SIZE;
    queue->count--;
    
    spinlockRelease(&queue->lock);
    return length;
}

// Check if queue has packets
bool net_queue_has_packets(NIC* nic) {
    if (!nic || nic->id >= MAX_NICS) {
        return false;
    }
    
    net_queue_t* queue = &net_queues[nic->id];
    
    spinlockAcquire(&queue->lock);
    bool has_packets = (queue->count > 0);
    spinlockRelease(&queue->lock);
    
    return has_packets;
}

// Get number of packets in queue
uint32_t net_queue_count(NIC* nic) {
    if (!nic || nic->id >= MAX_NICS) {
        return 0;
    }
    
    net_queue_t* queue = &net_queues[nic->id];
    
    spinlockAcquire(&queue->lock);
    uint32_t count = queue->count;
    spinlockRelease(&queue->lock);
    
    return count;
}

// Clear network queue
void net_queue_clear(NIC* nic) {
    if (!nic || nic->id >= MAX_NICS) {
        return;
    }
    
    net_queue_t* queue = &net_queues[nic->id];
    
    spinlockAcquire(&queue->lock);
    
    for (uint32_t i = 0; i < NET_QUEUE_SIZE; i++) {
        queue->packets[i].valid = false;
        queue->packets[i].length = 0;
    }
    
    queue->read_index = 0;
    queue->write_index = 0;
    queue->count = 0;
    
    spinlockRelease(&queue->lock);
}

// Get queue statistics
void net_queue_stats(NIC* nic) {
    if (!nic || nic->id >= MAX_NICS) {
        print("Invalid NIC\n");
        return;
    }
    
    net_queue_t* queue = &net_queues[nic->id];
    
    spinlockAcquire(&queue->lock);
    
    print("Network Queue Statistics:\n");
    print("  Packets in queue: ");
    print_decimal(queue->count);
    print("/");
    print_decimal(NET_QUEUE_SIZE);
    print("\n");
    
    print("  Read index: ");
    print_decimal(queue->read_index);
    print("\n");
    
    print("  Write index: ");
    print_decimal(queue->write_index);
    print("\n");
    
    uint32_t usage = (queue->count * 100) / NET_QUEUE_SIZE;
    print("  Queue usage: ");
    print_decimal(usage);
    print("%\n");
    
    spinlockRelease(&queue->lock);
}