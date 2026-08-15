// nic.h
#ifndef NIC_H
#define NIC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
// NIC types
typedef enum {
    NIC_TYPE_UNKNOWN = 0,
    NIC_TYPE_RTL8139,
    NIC_TYPE_E1000,
    NIC_TYPE_PCNET,
    NIC_TYPE_VIRTIO
} nic_type_t;

// NIC states
typedef enum {
    NIC_STATE_UNINITIALIZED = 0,
    NIC_STATE_INITIALIZING,
    NIC_STATE_READY,
    NIC_STATE_ERROR,
    NIC_STATE_DISABLED
} nic_state_t;

// Maximum sizes
#define NIC_MAX_NAME 32
#define NIC_MAC_SIZE 6
#define NIC_IP_SIZE 4

// Forward declarations
typedef struct PCIdevice PCIdevice;
typedef struct PCI PCI;

// Network Interface Card structure
typedef struct NIC {
    uint32_t id;                        // NIC identifier
    char name[NIC_MAX_NAME];            // NIC name (e.g., "eth0")
    nic_type_t type;                    // NIC type
    nic_state_t state;                  // Current state
    
    // Hardware information
    uint8_t MAC[NIC_MAC_SIZE];          // MAC address
    uint8_t ip[NIC_IP_SIZE];            // IP address
    uint8_t subnet[NIC_IP_SIZE];        // Subnet mask
    uint8_t gateway[NIC_IP_SIZE];       // Gateway address
    
    // Network parameters
    uint16_t mintu;                     // Minimum transmission unit
    uint16_t mtu;                       // Maximum transmission unit
    uint32_t link_speed;                // Link speed in Mbps
    bool link_up;                       // Link status
    
    // Hardware specifics
    uint16_t iobase;                    // I/O base address
    uint32_t mmio_base;                 // Memory-mapped I/O base
    uint8_t irq;                        // IRQ number
    
    // Driver information
    void* infoLocation;                 // Driver-specific data
    void* driver_data;                  // Additional driver data
    
    // PCI information
    PCIdevice* pci_device;              // PCI device pointer
    PCI* pci;                           // PCI structure pointer
    
    // Statistics
    uint64_t tx_packets;                // Transmitted packets
    uint64_t rx_packets;                // Received packets
    uint64_t tx_bytes;                  // Transmitted bytes
    uint64_t rx_bytes;                  // Received bytes
    uint64_t tx_errors;                 // Transmission errors
    uint64_t rx_errors;                 // Reception errors
    uint64_t tx_dropped;                // Dropped TX packets
    uint64_t rx_dropped;                // Dropped RX packets
    
    // Flags
    bool initialized;                   // Is NIC initialized?
    bool promiscuous;                   // Promiscuous mode enabled?
    bool broadcast;                     // Broadcast enabled?
    bool multicast;                     // Multicast enabled?
    
} NIC;

// Global NIC management
#define MAX_NICS 4
extern NIC* nics[MAX_NICS];
extern NIC* selectedNIC;
extern uint32_t nic_count;

// NIC management functions
void nic_init_subsystem(void);
NIC* nic_create(void);
void nic_destroy(NIC* nic);
NIC* nic_get_by_id(uint32_t id);
NIC* nic_get_by_name(const char* name);
void nic_set_selected(NIC* nic);
NIC* nic_get_selected(void);

// NIC configuration functions
void nic_set_mac(NIC* nic, const uint8_t* mac);
void nic_get_mac(NIC* nic, uint8_t* mac);
void nic_set_ip(NIC* nic, const uint8_t* ip);
void nic_get_ip(NIC* nic, uint8_t* ip);
void nic_set_subnet(NIC* nic, const uint8_t* subnet);
void nic_set_gateway(NIC* nic, const uint8_t* gateway);
void nic_set_name(NIC* nic, const char* name);
void nic_set_type(NIC* nic, nic_type_t type);
void nic_set_state(NIC* nic, nic_state_t state);

// NIC operation functions
bool nic_is_ready(NIC* nic);
bool nic_is_link_up(NIC* nic);
void nic_enable(NIC* nic);
void nic_disable(NIC* nic);
void nic_reset_statistics(NIC* nic);

// NIC statistics functions
void nic_update_tx_stats(NIC* nic, uint32_t bytes);
void nic_update_rx_stats(NIC* nic, uint32_t bytes);
void nic_update_tx_error(NIC* nic);
void nic_update_rx_error(NIC* nic);
void nic_update_tx_dropped(NIC* nic);
void nic_update_rx_dropped(NIC* nic);

// NIC information functions
void nic_print_info(NIC* nic);
void nic_print_statistics(NIC* nic);
void nic_list_all(void);
const char* nic_type_to_string(nic_type_t type);
const char* nic_state_to_string(nic_state_t state);

// Helper functions
void nic_format_mac(const uint8_t* mac, char* buffer, size_t size);
void nic_format_ip(const uint8_t* ip, char* buffer, size_t size);
bool nic_parse_mac(const char* str, uint8_t* mac);
bool nic_parse_ip(const char* str, uint8_t* ip);

#endif // NIC_H