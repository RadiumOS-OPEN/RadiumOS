// rtl8139.h
#ifndef RTL8139_H
#define RTL8139_H

#include <stdint.h>
#include <stdbool.h>

// RTL8139 Register Definitions
#define RTL8139_REG_MAC0        0x00    // MAC address byte 0
#define RTL8139_REG_MAC1        0x01    // MAC address byte 1
#define RTL8139_REG_MAC2        0x02    // MAC address byte 2
#define RTL8139_REG_MAC3        0x03    // MAC address byte 3
#define RTL8139_REG_MAC4        0x04    // MAC address byte 4
#define RTL8139_REG_MAC5        0x05    // MAC address byte 5
#define RTL8139_REG_RBSTART     0x30    // Receive buffer start
#define RTL8139_REG_CMD         0x37    // Command register
#define RTL8139_REG_CAPR        0x38    // Current address of packet read
#define RTL8139_REG_IMR         0x3C    // Interrupt mask register
#define RTL8139_REG_ISR         0x3E    // Interrupt status register
#define RTL8139_REG_RCR         0x44    // Receive configuration register
#define RTL8139_REG_CONFIG1     0x52    // Configuration register 1
#define RTL8139_REG_POWERUP     0x52    // Power management register

// Command Register Bits
#define RTL8139_CMD_RESET       0x10    // Software reset
#define RTL8139_CMD_RX_ENABLE   0x08    // Receive enable
#define RTL8139_CMD_TX_ENABLE   0x04    // Transmit enable
#define RTL8139_CMD_BUFFER_EMPTY 0x01   // RX buffer empty

// Interrupt Status/Mask Bits
#define RTL8139_INT_RXOK        0x01    // Receive OK
#define RTL8139_INT_RXERR       0x02    // Receive error
#define RTL8139_INT_TXOK        0x04    // Transmit OK
#define RTL8139_INT_TXERR       0x08    // Transmit error
#define RTL8139_INT_RX_OVERFLOW 0x10    // RX buffer overflow
#define RTL8139_INT_RX_UNDERRUN 0x20    // Packet underrun
#define RTL8139_INT_FIFO_OVER   0x40    // RX FIFO overflow
#define RTL8139_INT_CABLE       0x2000  // Cable length change
#define RTL8139_INT_TIMEOUT     0x4000  // Time out

// Receive Configuration Register Bits
#define RTL8139_RCR_AAP         (1 << 0)  // Accept all packets
#define RTL8139_RCR_APM         (1 << 1)  // Accept physical match
#define RTL8139_RCR_AM          (1 << 2)  // Accept multicast
#define RTL8139_RCR_AB          (1 << 3)  // Accept broadcast
#define RTL8139_RCR_WRAP        (1 << 7)  // Wrap at end of buffer

// Transmit Status Register Bits
#define RTL8139_TSD_OWN         (1 << 13) // Descriptor owned by NIC
#define RTL8139_TSD_TOK         (1 << 15) // Transmit OK

// RTL8139 Status Bits (legacy compatibility)
#define RTL8139_STATUS_ROK      0x01    // Receive OK
#define RTL8139_STATUS_TOK      0x04    // Transmit OK

// PCI Vendor and Device IDs
#define RTL8139_VENDOR_ID       0x10ec
#define RTL8139_DEVICE_ID       0x8139

// Buffer Configuration
#define RTL8139_RX_BUFFER_SIZE  (8192 + 16 + 1500)  // 8KB + header + max packet
#define RTL8139_TX_BUFFER_SIZE  4096                 // 4KB per TX buffer
#define RTL8139_MAX_PACKET_SIZE 1792                 // Maximum packet size

// TX Descriptor Count
#define RTL8139_TX_DESCRIPTORS  4

// Forward declarations
typedef struct PCIdevice PCIdevice;
typedef struct NIC NIC;
typedef struct AsmPassedInterrupt AsmPassedInterrupt;

// RTL8139 Interface Structure
typedef struct {
    uint16_t iobase;              // Base I/O port address
    uint8_t tx_curr;              // Current TX descriptor (0-3)
    uint8_t tok;                  // Transmit OK flags
    uint8_t *rx_buff_virtual;     // RX buffer virtual address
    uint32_t rx_buff_physical;    // RX buffer physical address
    uint16_t currentPacket;       // Current packet offset in RX buffer
    uint8_t *tx_buff_virtual[4];  // TX buffer virtual addresses
    uint32_t tx_buff_physical[4]; // TX buffer physical addresses
    uint8_t mac[6];               // MAC address
} rtl8139_interface;

// TX/RX Descriptor Arrays
extern uint8_t TSAD_array[4];     // TX start address descriptors (offsets: 0x20, 0x24, 0x28, 0x2C)
extern uint8_t TSD_array[4];      // TX status descriptors (offsets: 0x10, 0x14, 0x18, 0x1C)

// Core RTL8139 Functions
bool initiateRTL8139(PCIdevice *device);
bool isRTL8139(PCIdevice *device);
void sendRTL8139(NIC *nic, void *packet, uint32_t packetSize);
void receiveRTL8139(NIC *nic);
void rtl8139_interrupt_handler(AsmPassedInterrupt *regs);

// Legacy compatibility
void interruptHandler(AsmPassedInterrupt *regs);

// Utility Functions
void rtl8139_read_mac(uint16_t iobase, uint8_t *mac_out);
void rtl8139_reset(uint16_t iobase);
void rtl8139_enable_interrupts(uint16_t iobase);
void rtl8139_disable_interrupts(uint16_t iobase);
uint16_t rtl8139_get_interrupt_status(uint16_t iobase);
void rtl8139_clear_interrupts(uint16_t iobase, uint16_t mask);

#endif // RTL8139_H