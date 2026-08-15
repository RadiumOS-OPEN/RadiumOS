// nic.c
#include "nic.h"
#include "../terminal/terminal.h"
#include "../utility/utility.h"

// Global NIC management
NIC* nics[MAX_NICS] = {NULL};
NIC* selectedNIC = NULL;
uint32_t nic_count = 0;

// Initialize NIC subsystem
void nic_init_subsystem(void) {
    for (int i = 0; i < MAX_NICS; i++) {
        nics[i] = NULL;
    }
    selectedNIC = NULL;
    nic_count = 0;
    print("[nic] NIC subsystem initialized\n");
}

// Create a new NIC
NIC* nic_create(void) {
    if (nic_count >= MAX_NICS) {
        print("[nic] ERROR: Maximum number of NICs reached\n");
        return NULL;
    }
    
    // Allocate NIC structure
    NIC* nic = (NIC*)malloc(sizeof(NIC));
    if (!nic) {
        print("[nic] ERROR: Failed to allocate memory for NIC\n");
        return NULL;
    }
    
    // Initialize NIC structure
    memset(nic, 0, sizeof(NIC));
    nic->id = nic_count;
    nic->type = NIC_TYPE_UNKNOWN;
    nic->state = NIC_STATE_UNINITIALIZED;
    nic->mtu = 1500;
    nic->mintu = 60;
    nic->initialized = false;
    nic->link_up = false;
    nic->promiscuous = false;
    nic->broadcast = true;
    nic->multicast = false;
    
    // Set default name
    char default_name[NIC_MAX_NAME];
    memset(default_name, 0, NIC_MAX_NAME);
    strcpy(default_name, "eth");
    char id_str[8];
    itoa(nic->id, id_str, 10);
    strcat(default_name, id_str);
    nic_set_name(nic, default_name);
    
    // Add to NIC array
    nics[nic_count] = nic;
    nic_count++;
    
    // Set as selected if first NIC
    if (nic_count == 1) {
        selectedNIC = nic;
    }
    
    print("[nic] Created NIC ");
    print(nic->name);
    print(" (ID: ");
    print_decimal(nic->id);
    print(")\n");
    
    return nic;
}

// Destroy a NIC
void nic_destroy(NIC* nic) {
    if (!nic) {
        return;
    }
    
    // Remove from array
    for (uint32_t i = 0; i < nic_count; i++) {
        if (nics[i] == nic) {
            nics[i] = NULL;
            // Shift remaining NICs
            for (uint32_t j = i; j < nic_count - 1; j++) {
                nics[j] = nics[j + 1];
                if (nics[j]) {
                    nics[j]->id = j;
                }
            }
            nics[nic_count - 1] = NULL;
            nic_count--;
            break;
        }
    }
    
    // Clear selected if it was this NIC
    if (selectedNIC == nic) {
        selectedNIC = (nic_count > 0) ? nics[0] : NULL;
    }
    
    print("[nic] Destroyed NIC ");
    print(nic->name);
    print("\n");
    
    free(nic);
}

// Get NIC by ID
NIC* nic_get_by_id(uint32_t id) {
    if (id >= nic_count) {
        return NULL;
    }
    return nics[id];
}

// Get NIC by name
NIC* nic_get_by_name(const char* name) {
    if (!name) {
        return NULL;
    }
    
    for (uint32_t i = 0; i < nic_count; i++) {
        if (nics[i] && strcmp(nics[i]->name, name) == 0) {
            return nics[i];
        }
    }
    
    return NULL;
}

// Set selected NIC
void nic_set_selected(NIC* nic) {
    selectedNIC = nic;
    if (nic) {
        print("[nic] Selected NIC: ");
        print(nic->name);
        print("\n");
    }
}

// Get selected NIC
NIC* nic_get_selected(void) {
    return selectedNIC;
}

// Set MAC address
void nic_set_mac(NIC* nic, const uint8_t* mac) {
    if (!nic || !mac) {
        return;
    }
    memcpy(nic->MAC, mac, NIC_MAC_SIZE);
}

// Get MAC address
void nic_get_mac(NIC* nic, uint8_t* mac) {
    if (!nic || !mac) {
        return;
    }
    memcpy(mac, nic->MAC, NIC_MAC_SIZE);
}

// Set IP address
void nic_set_ip(NIC* nic, const uint8_t* ip) {
    if (!nic || !ip) {
        return;
    }
    memcpy(nic->ip, ip, NIC_IP_SIZE);
}

// Get IP address
void nic_get_ip(NIC* nic, uint8_t* ip) {
    if (!nic || !ip) {
        return;
    }
    memcpy(ip, nic->ip, NIC_IP_SIZE);
}

// Set subnet mask
void nic_set_subnet(NIC* nic, const uint8_t* subnet) {
    if (!nic || !subnet) {
        return;
    }
    memcpy(nic->subnet, subnet, NIC_IP_SIZE);
}

// Set gateway
void nic_set_gateway(NIC* nic, const uint8_t* gateway) {
    if (!nic || !gateway) {
        return;
    }
    memcpy(nic->gateway, gateway, NIC_IP_SIZE);
}

// Set NIC name
void nic_set_name(NIC* nic, const char* name) {
    if (!nic || !name) {
        return;
    }
    strncpy(nic->name, name, NIC_MAX_NAME - 1);
    nic->name[NIC_MAX_NAME - 1] = '\0';
}

// Set NIC type
void nic_set_type(NIC* nic, nic_type_t type) {
    if (!nic) {
        return;
    }
    nic->type = type;
}

// Set NIC state
void nic_set_state(NIC* nic, nic_state_t state) {
    if (!nic) {
        return;
    }
    nic->state = state;
}

// Check if NIC is ready
bool nic_is_ready(NIC* nic) {
    return nic && nic->state == NIC_STATE_READY && nic->initialized;
}

// Check if link is up
bool nic_is_link_up(NIC* nic) {
    return nic && nic->link_up;
}

// Enable NIC
void nic_enable(NIC* nic) {
    if (!nic) {
        return;
    }
    nic->state = NIC_STATE_READY;
    print("[nic] Enabled NIC ");
    print(nic->name);
    print("\n");
}

// Disable NIC
void nic_disable(NIC* nic) {
    if (!nic) {
        return;
    }
    nic->state = NIC_STATE_DISABLED;
    print("[nic] Disabled NIC ");
    print(nic->name);
    print("\n");
}

// Reset statistics
void nic_reset_statistics(NIC* nic) {
    if (!nic) {
        return;
    }
    nic->tx_packets = 0;
    nic->rx_packets = 0;
    nic->tx_bytes = 0;
    nic->rx_bytes = 0;
    nic->tx_errors = 0;
    nic->rx_errors = 0;
    nic->tx_dropped = 0;
    nic->rx_dropped = 0;
}

// Update TX statistics
void nic_update_tx_stats(NIC* nic, uint32_t bytes) {
    if (!nic) {
        return;
    }
    nic->tx_packets++;
    nic->tx_bytes += bytes;
}

// Update RX statistics
void nic_update_rx_stats(NIC* nic, uint32_t bytes) {
    if (!nic) {
        return;
    }
    nic->rx_packets++;
    nic->rx_bytes += bytes;
}

// Update TX error
void nic_update_tx_error(NIC* nic) {
    if (!nic) {
        return;
    }
    nic->tx_errors++;
}

// Update RX error
void nic_update_rx_error(NIC* nic) {
    if (!nic) {
        return;
    }
    nic->rx_errors++;
}

// Update TX dropped
void nic_update_tx_dropped(NIC* nic) {
    if (!nic) {
        return;
    }
    nic->tx_dropped++;
}

// Update RX dropped
void nic_update_rx_dropped(NIC* nic) {
    if (!nic) {
        return;
    }
    nic->rx_dropped++;
}

// Convert NIC type to string
const char* nic_type_to_string(nic_type_t type) {
    switch (type) {
        case NIC_TYPE_RTL8139: return "RTL8139";
        case NIC_TYPE_E1000: return "E1000";
        case NIC_TYPE_PCNET: return "PCNet";
        case NIC_TYPE_VIRTIO: return "VirtIO";
        default: return "Unknown";
    }
}

// Convert NIC state to string
const char* nic_state_to_string(nic_state_t state) {
    switch (state) {
        case NIC_STATE_UNINITIALIZED: return "Uninitialized";
        case NIC_STATE_INITIALIZING: return "Initializing";
        case NIC_STATE_READY: return "Ready";
        case NIC_STATE_ERROR: return "Error";
        case NIC_STATE_DISABLED: return "Disabled";
        default: return "Unknown";
    }
}

// Format MAC address to string
void nic_format_mac(const uint8_t* mac, char* buffer, size_t size) {
    if (!mac || !buffer || size < 18) {
        return;
    }
    
    char hex[3];
    buffer[0] = '\0';
    
    for (int i = 0; i < NIC_MAC_SIZE; i++) {
        itoa(mac[i], hex, 16);
        if (mac[i] < 16) {
            strcat(buffer, "0");
        }
        strcat(buffer, hex);
        if (i < NIC_MAC_SIZE - 1) {
            strcat(buffer, ":");
        }
    }
}

// Format IP address to string
void nic_format_ip(const uint8_t* ip, char* buffer, size_t size) {
    if (!ip || !buffer || size < 16) {
        return;
    }
    
    char num[4];
    buffer[0] = '\0';
    
    for (int i = 0; i < NIC_IP_SIZE; i++) {
        itoa(ip[i], num, 10);
        strcat(buffer, num);
        if (i < NIC_IP_SIZE - 1) {
            strcat(buffer, ".");
        }
    }
}

// Print NIC information
void nic_print_info(NIC* nic) {
    if (!nic) {
        print("NIC is NULL\n");
        return;
    }
    
    print("\n=== NIC Information ===\n");
    print("Name: ");
    print(nic->name);
    print("\n");
    
    print("ID: ");
    print_decimal(nic->id);
    print("\n");
    
    print("Type: ");
    print(nic_type_to_string(nic->type));
    print("\n");
    
    print("State: ");
    print(nic_state_to_string(nic->state));
    print("\n");
    
    print("Link: ");
    print(nic->link_up ? "UP" : "DOWN");
    print("\n");
    
    char mac_str[18];
    nic_format_mac(nic->MAC, mac_str, sizeof(mac_str));
    print("MAC: ");
    print(mac_str);
    print("\n");
    
    char ip_str[16];
    nic_format_ip(nic->ip, ip_str, sizeof(ip_str));
    print("IP: ");
    print(ip_str);
    print("\n");
    
    nic_format_ip(nic->subnet, ip_str, sizeof(ip_str));
    print("Subnet: ");
    print(ip_str);
    print("\n");
    
    nic_format_ip(nic->gateway, ip_str, sizeof(ip_str));
    print("Gateway: ");
    print(ip_str);
    print("\n");
    
    print("MTU: ");
    print_decimal(nic->mtu);
    print("\n");
    
    if (nic->link_speed > 0) {
        print("Speed: ");
        print_decimal(nic->link_speed);
        print(" Mbps\n");
    }
}

// Print NIC statistics
void nic_print_statistics(NIC* nic) {
    if (!nic) {
        print("NIC is NULL\n");
        return;
    }
    
    print("\n=== NIC Statistics: ");
    print(nic->name);
    print(" ===\n");
    
    print("TX Packets: ");
    print_decimal(nic->tx_packets);
    print(" (");
    print_decimal(nic->tx_bytes);
    print(" bytes)\n");
    
    print("RX Packets: ");
    print_decimal(nic->rx_packets);
    print(" (");
    print_decimal(nic->rx_bytes);
    print(" bytes)\n");
    
    print("TX Errors: ");
    print_decimal(nic->tx_errors);
    print(" Dropped: ");
    print_decimal(nic->tx_dropped);
    print("\n");
    
    print("RX Errors: ");
    print_decimal(nic->rx_errors);
    print(" Dropped: ");
    print_decimal(nic->rx_dropped);
    print("\n");
}

// List all NICs
void nic_list_all(void) {
    print("\n=== Network Interfaces ===\n");
    print("Total NICs: ");
    print_decimal(nic_count);
    print("\n\n");
    
    for (uint32_t i = 0; i < nic_count; i++) {
        NIC* nic = nics[i];
        if (nic) {
            print(nic->name);
            print(": ");
            print(nic_type_to_string(nic->type));
            print(" [");
            print(nic_state_to_string(nic->state));
            print("]");
            
            if (nic == selectedNIC) {
                print(" *");
            }
            
            print("\n");
        }
    }
}