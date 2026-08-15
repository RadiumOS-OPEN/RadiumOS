// networkrecievethread.c - Complete implementation

#include "../rtl8139/rtl8139.h"
#include "nic.h"
#include "net.h"
#include "../terminal/terminal.h"
#include "../utility/utility.h"
#include "../timers/timer.h"
#include "netstack.h"
#include <stdint.h>
#include <stdbool.h>
#include "../utility/utility.h"

// Statistics counters
static uint32_t total_packets_received = 0;
static uint32_t total_bytes_received = 0;
static uint32_t packets_dropped = 0;
static uint32_t last_packet_time = 0;

// Packet receive thread - processes all incoming packets
void network_receive_thread(void) {
    static uint8_t rx_buffer[2048];
    int length = 0;
    
    // Check if NIC is initialized
    if (!selectedNIC || !nic_is_ready(selectedNIC)) {
        return;
    }
    
    // Process all available packets from network queue
    while ((length = net_queue_get(selectedNIC, rx_buffer, sizeof(rx_buffer))) > 0) {
        if (length < 14 || length > 1518) {
            // Invalid packet size
            packets_dropped++;
            nic_update_rx_dropped(selectedNIC);
            continue;
        }
        
        // Update statistics
        total_packets_received++;
        total_bytes_received += length;
        last_packet_time = get_time_ms();
        
        // Process packet through network stack
        netstack_process_packet(rx_buffer, length);
        
        // Clear buffer for next packet
        memset(rx_buffer, 0, sizeof(rx_buffer));
    }
}

// Continuous receive task (runs as a kernel task)
void network_receive_task(uint32_t task_id) {
    print("Network receive task started (ID: ");
    char id_str[16];
    itoa(task_id, id_str, 10);
    print(id_str);
    print(")\n");
    
    while (true) {
        // Trigger hardware receive
        if (selectedNIC) {
            receiveRTL8139(selectedNIC);
        }
        
        // Process queued packets
        network_receive_thread();
        
        // Sleep for 10ms to avoid consuming too much CPU
        delay(10);
    }
}

// Get network statistics
void network_get_stats(uint32_t* packets, uint32_t* bytes, uint32_t* dropped, uint32_t* last_time) {
    if (packets) *packets = total_packets_received;
    if (bytes) *bytes = total_bytes_received;
    if (dropped) *dropped = packets_dropped;
    if (last_time) *last_time = last_packet_time;
}

// Reset network statistics
void network_reset_stats(void) {
    total_packets_received = 0;
    total_bytes_received = 0;
    packets_dropped = 0;
    last_packet_time = 0;
    
    if (selectedNIC) {
        nic_reset_statistics(selectedNIC);
    }
}

// Print network statistics
void network_print_stats(void) {
    print("Network Statistics:\n");
    
    char buffer[32];
    
    print("  Total Packets: ");
    itoa(total_packets_received, buffer, 10);
    print(buffer);
    print("\n");
    
    print("  Total Bytes: ");
    itoa(total_bytes_received, buffer, 10);
    print(buffer);
    print(" bytes\n");
    
    print("  Packets Dropped: ");
    itoa(packets_dropped, buffer, 10);
    print(buffer);
    print("\n");
    
    if (last_packet_time > 0) {
        uint32_t elapsed = (get_time_ms() - last_packet_time) / 1000;
        print("  Last Packet: ");
        itoa(elapsed, buffer, 10);
        print(buffer);
        print(" seconds ago\n");
    } else {
        print("  Last Packet: Never\n");
    }
    
    // Calculate average packet size
    if (total_packets_received > 0) {
        uint32_t avg = total_bytes_received / total_packets_received;
        print("  Average Packet Size: ");
        itoa(avg, buffer, 10);
        print(buffer);
        print(" bytes\n");
    }
    
    // Print NIC statistics if available
    if (selectedNIC) {
        print("\n");
        nic_print_statistics(selectedNIC);
    }
}

// Advanced receive thread with filtering
void network_receive_thread_filtered(bool (*filter)(uint8_t* packet, uint16_t length)) {
    static uint8_t rx_buffer[2048];
    int length = 0;
    
    if (!selectedNIC || !nic_is_ready(selectedNIC)) {
        return;
    }
    
    // Trigger hardware receive
    receiveRTL8139(selectedNIC);
    
    // Process queued packets
    while ((length = net_queue_get(selectedNIC, rx_buffer, sizeof(rx_buffer))) > 0) {
        if (length < 14 || length > 1518) {
            packets_dropped++;
            nic_update_rx_dropped(selectedNIC);
            continue;
        }
        
        total_packets_received++;
        total_bytes_received += length;
        last_packet_time = get_time_ms();
        
        // Apply filter if provided
        if (filter == NULL || filter(rx_buffer, length)) {
            netstack_process_packet(rx_buffer, length);
        }
        
        memset(rx_buffer, 0, sizeof(rx_buffer));
    }
}

// Packet capture mode - save packets to buffer
#define MAX_CAPTURED_PACKETS 100
typedef struct {
    uint8_t data[1518];
    uint16_t length;
    uint32_t timestamp;
} captured_packet_t;

static captured_packet_t packet_capture_buffer[MAX_CAPTURED_PACKETS];
static uint32_t captured_packet_count = 0;
static bool capture_enabled = false;

// Start packet capture
void network_start_capture(void) {
    captured_packet_count = 0;
    capture_enabled = true;
    print("Packet capture started\n");
}

// Stop packet capture
void network_stop_capture(void) {
    capture_enabled = false;
    print("Packet capture stopped. Captured ");
    char count[16];
    itoa(captured_packet_count, count, 10);
    print(count);
    print(" packets\n");
}

// Get captured packets
uint32_t network_get_captured_packets(captured_packet_t** packets) {
    *packets = packet_capture_buffer;
    return captured_packet_count;
}

// Receive thread with capture support
void network_receive_thread_with_capture(void) {
    static uint8_t rx_buffer[2048];
    int length = 0;
    
    if (!selectedNIC || !nic_is_ready(selectedNIC)) {
        return;
    }
    
    // Trigger hardware receive
    receiveRTL8139(selectedNIC);
    
    // Process queued packets
    while ((length = net_queue_get(selectedNIC, rx_buffer, sizeof(rx_buffer))) > 0) {
        if (length < 14 || length > 1518) {
            packets_dropped++;
            nic_update_rx_dropped(selectedNIC);
            continue;
        }
        
        total_packets_received++;
        total_bytes_received += length;
        last_packet_time = get_time_ms();
        
        // Capture packet if enabled
        if (capture_enabled && captured_packet_count < MAX_CAPTURED_PACKETS) {
            memcpy(packet_capture_buffer[captured_packet_count].data, rx_buffer, length);
            packet_capture_buffer[captured_packet_count].length = length;
            packet_capture_buffer[captured_packet_count].timestamp = get_time_ms();
            captured_packet_count++;
        }
        
        // Process packet
        netstack_process_packet(rx_buffer, length);
        
        memset(rx_buffer, 0, sizeof(rx_buffer));
    }
}

// Dump captured packets
void network_dump_captured_packets(void) {
    if (captured_packet_count == 0) {
        print("No packets captured\n");
        return;
    }
    
    print("Captured Packets:\n");
    print("================\n\n");
    
    for (uint32_t i = 0; i < captured_packet_count; i++) {
        captured_packet_t* pkt = &packet_capture_buffer[i];
        
        print("Packet #");
        char num[16];
        itoa(i + 1, num, 10);
        print(num);
        print(" - Length: ");
        itoa(pkt->length, num, 10);
        print(num);
        print(" bytes - Time: ");
        itoa(pkt->timestamp, num, 10);
        print(num);
        print("ms\n");
        
        // Print first 64 bytes in hex
        print("Data: ");
        for (int j = 0; j < 64 && j < pkt->length; j++) {
            char hex[4];
            itoa(pkt->data[j], hex, 16);
            if (pkt->data[j] < 16) print("0");
            print(hex);
            print(" ");
            if ((j + 1) % 16 == 0) print("\n      ");
        }
        print("\n\n");
    }
}

// Enhanced network command with capture support
void network_capture_command(int argc, char* argv[]) {
    if (argc < 2) {
        print("Usage:\n");
        print("  capture start  - Start capturing packets\n");
        print("  capture stop   - Stop capturing packets\n");
        print("  capture dump   - Display captured packets\n");
        print("  capture clear  - Clear capture buffer\n");
        return;
    }
    
    if (strcmp(argv[1], "start") == 0) {
        network_start_capture();
    }
    else if (strcmp(argv[1], "stop") == 0) {
        network_stop_capture();
    }
    else if (strcmp(argv[1], "dump") == 0) {
        network_dump_captured_packets();
    }
    else if (strcmp(argv[1], "clear") == 0) {
        captured_packet_count = 0;
        print("Capture buffer cleared\n");
    }
    else {
        print("Unknown capture command: ");
        print(argv[1]);
        print("\n");
    }
}

// Example filters for filtered receive
bool filter_only_arp(uint8_t* packet, uint16_t length) {
    if (length < 14) return false;
    ethernet_frame_t* eth = (ethernet_frame_t*)packet;
    return ntohs(eth->ethertype) == ETHERTYPE_ARP;
}

bool filter_only_icmp(uint8_t* packet, uint16_t length) {
    if (length < 34) return false;
    ethernet_frame_t* eth = (ethernet_frame_t*)packet;
    if (ntohs(eth->ethertype) != ETHERTYPE_IPV4) return false;
    ipv4_header_t* ip = (ipv4_header_t*)eth->payload;
    return ip->protocol == IP_PROTOCOL_ICMP;
}

bool filter_only_udp(uint8_t* packet, uint16_t length) {
    if (length < 34) return false;
    ethernet_frame_t* eth = (ethernet_frame_t*)packet;
    if (ntohs(eth->ethertype) != ETHERTYPE_IPV4) return false;
    ipv4_header_t* ip = (ipv4_header_t*)eth->payload;
    return ip->protocol == IP_PROTOCOL_UDP;
}

// Trigger hardware packet reception and queue processing
void network_poll(void) {
    if (!selectedNIC || !nic_is_ready(selectedNIC)) {
        return;
    }
    
    // Trigger RTL8139 to move packets from hardware to queue
    receiveRTL8139(selectedNIC);
    
    // Process the queued packets
    network_receive_thread();
}

// Usage example for kernel_main.c:
/*
void kernel_main() {
    // ... initialization ...
    
    // Initialize NIC subsystem
    nic_init_subsystem();
    
    // Initialize network queue for NIC
    if (selectedNIC) {
        net_queue_init(selectedNIC);
    }
    
    // Option 1: Create dedicated network task (recommended)
    create_task(4, (uint32_t)network_receive_task, 0x980000, 0x900000, false);
    
    // Option 2: Call periodically in main loop
    while (true) {
        network_poll();
        // ... other tasks ...
        delay(50);
    }
    
    // Option 3: Call in timer interrupt (for very low latency)
    // In timer_interrupt_handler():
    //   if (ticks % 5 == 0) network_poll();
}
*/