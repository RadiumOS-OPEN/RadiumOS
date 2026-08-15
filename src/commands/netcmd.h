// netcmd.h - Network Stack Command Interface Header
#ifndef NETCMD_H
#define NETCMD_H

#include <stdint.h>
#include <stdbool.h>

// Main network command handler
// Usage: network <subcommand> [args...]
// Subcommands:
//   status       - Show network card and configuration status
//   init         - Initialize RTL8139 network card
//   config       - Configure IP address, subnet, and gateway
//   ping         - Send ICMP echo request to target IP
//   arp          - Display ARP cache
//   arpreq       - Send ARP request for IP address
//   listen       - Monitor incoming packets in real-time
//   test         - Run automated network stack tests
//   send         - Send test UDP packet
//   info         - Show network stack capabilities
//   dhcp         - Show DHCP status (not implemented)
//   route        - Display routing table
//   stats        - Show network statistics
//   clear        - Clear ARP cache
void networkk_command(int argc, char* argv[]);

// Initialize network subsystem
// Call this during kernel initialization after RTL8139 driver is loaded
// Sets up default network configuration:
//   IP: 192.168.1.100
//   Subnet: 255.255.255.0
//   Gateway: 192.168.1.1
void network_subsystem_init(void);

// Packet receive processing thread
// Call this periodically (e.g., every 50ms) or in network interrupt handler
// Checks for incoming packets and processes them through the network stack
void network_receive_thread(void);

// Quick ping command
// Usage: ping <ip_address>
// Sends 4 ICMP echo requests and waits for replies
// Shows round-trip time and packet loss statistics
void ping_command(int argc, char* argv[]);

// ARP utility command
// Usage: 
//   arp -a          - Show ARP cache with IP->MAC mappings
//   arp -d <ip>     - Delete specific ARP cache entry
//   arp -s <ip>     - Send ARP request for IP address
void arp_command(int argc, char* argv[]);

// Interface configuration command (ifconfig-style)
// Usage: ifconfig
// Displays network interface status including:
//   - Interface flags (UP/DOWN, BROADCAST, RUNNING)
//   - IP address, netmask, gateway
//   - MAC address
//   - RX/TX packet counts
void ifconfig_command(int argc, char* argv[]);

// Network command registration helper
// Call these in your command registration during kernel init:
//
// Example:
//   register_command("network", "Network utilities", network_command);
//   register_command("ping", "Ping a host", ping_command);
//   register_command("arp", "ARP utilities", arp_command);
//   register_command("ifconfig", "Interface configuration", ifconfig_command);

// Network subsystem status check
// Returns true if network card is initialized and ready
bool network_is_ready(void);

// Get network configuration as string (for status displays)
void network_get_config_string(char* buffer, int max_len);

#endif // NETCMD_H