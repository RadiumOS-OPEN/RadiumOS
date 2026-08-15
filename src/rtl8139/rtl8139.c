#include "rtl8139.h"
#include "../pci/pci.h"
#include "../network/nic.h"
#include "../io/io.h"
#include "../terminal/terminal.h"
#include "../memory/pmm.h"
#include "../utility/utility.h"

// Realtek RTL8139 network card support (10/100Mbit)

// RTL8139 Register offsets
#define RTL8139_REG_MAC0        0x00  // MAC address
#define RTL8139_REG_MAC4        0x04

// TX descriptor registers (4 descriptors)
uint8_t TSAD_array[4] = {0x20, 0x24, 0x28, 0x2C};  // TX start address
uint8_t TSD_array[4] = {0x10, 0x14, 0x18, 0x1C};   // TX status

// RX buffer configuration
#define RX_BUFFER_SIZE  (8192 + 16 + 1500)  // 8KB + 16B header + max packet size

// Command register bits
#define CMD_RESET       0x10
#define CMD_RX_ENABLE   0x08
#define CMD_TX_ENABLE   0x04
#define CMD_BUFFER_EMPTY 0x01

// Interrupt bits
#define INT_RXOK        0x01  // RX OK
#define INT_RXERR       0x02  // RX error
#define INT_TXOK        0x04  // TX OK
#define INT_TXERR       0x08  // TX error
#define INT_RX_OVERFLOW 0x10  // RX buffer overflow
#define INT_RX_UNDERRUN 0x20  // Packet underrun
#define INT_FIFO_OVER   0x40  // FIFO overflow
#define INT_CABLE       0x2000 // Cable length change
#define INT_TIMEOUT     0x4000 // Time out

// RX configuration bits
#define RCR_AAP         (1 << 0)  // Accept all packets
#define RCR_APM         (1 << 1)  // Accept physical match
#define RCR_AM          (1 << 2)  // Accept multicast
#define RCR_AB          (1 << 3)  // Accept broadcast
#define RCR_WRAP        (1 << 7)  // Wrap at end of buffer

#define RTL8139_DEBUG 0

// Check if device is RTL8139
bool isRTL8139(PCIdevice *device) {
    return (device->vendor_id == 0x10ec && device->device_id == 0x8139);
}

// Initialize RTL8139 device
bool initiateRTL8139(PCIdevice *device) {
    if (!isRTL8139(device)) {
        return false;
    }

    #if RTL8139_DEBUG
    print("RTL8139: Initializing...\n");
    #endif

    // Get device details
    PCIgeneralDevice details;
    GetGeneralDevice(device, &details);

    // Get I/O base address (BAR0)
    uint16_t iobase = details.bar[0] & ~0x3;
    
    #if RTL8139_DEBUG
    printr("RTL8139: I/O Base = 0x%x\n", iobase);
    #endif

    // Enable PCI Bus Mastering
    uint32_t command_status = COMBINE_WORD(device->status, device->command);
    if (!(command_status & (1 << 2))) {
        command_status |= (1 << 2);  // Enable bus mastering
        ConfigWriteDword(device->bus, device->slot, device->function, PCI_COMMAND, command_status);
        #if RTL8139_DEBUG
        print("RTL8139: Enabled PCI bus mastering\n");
        #endif
    }

    // Power on the device
    outb(iobase + RTL8139_REG_CONFIG1, 0x0);

    // Software reset
    outb(iobase + RTL8139_REG_CMD, CMD_RESET);
    while ((inb(iobase + RTL8139_REG_CMD) & CMD_RESET) != 0) {
        // Wait for reset to complete
    }

    #if RTL8139_DEBUG
    print("RTL8139: Reset complete\n");
    #endif

    // Allocate RX buffer (needs to be physically contiguous)
    void *rx_buffer_virt = pmm_alloc_pages((RX_BUFFER_SIZE + 4095) / 4096);
    if (!rx_buffer_virt) {
        print("RTL8139: Failed to allocate RX buffer\n");
        return false;
    }
    
    uint32_t rx_buffer_phys = (uint32_t)rx_buffer_virt;  // In identity-mapped region
    
    // Allocate TX buffers (4 buffers, 1 page each)
    void *tx_buffers[4];
    uint32_t tx_buffers_phys[4];
    
    for (int i = 0; i < 4; i++) {
        tx_buffers[i] = pmm_alloc_pages(1);
        if (!tx_buffers[i]) {
            print("RTL8139: Failed to allocate TX buffer\n");
            return false;
        }
        tx_buffers_phys[i] = (uint32_t)tx_buffers[i];
    }

    #if RTL8139_DEBUG
    printr("RTL8139: RX buffer at 0x%x\n", rx_buffer_phys);
    #endif

    // Set RX buffer address
    outl(iobase + 0x30, rx_buffer_phys);

    // Set IMR (Interrupt Mask Register) + ISR (Interrupt Status Register)
    outw(iobase + RTL8139_REG_IMR, INT_RXOK | INT_TXOK | INT_RXERR | INT_TXERR | INT_RX_OVERFLOW);
    outw(iobase + RTL8139_REG_ISR, 0xFFFF);  // Clear any pending interrupts

    // Configure RX
    // Accept broadcast, multicast, and physical match packets
    outl(iobase + RTL8139_REG_RCR, RCR_AAP | RCR_APM | RCR_AM | RCR_AB | RCR_WRAP);

    // Enable RX and TX
    outb(iobase + RTL8139_REG_CMD, CMD_RX_ENABLE | CMD_TX_ENABLE);

    // Read MAC address
    uint8_t mac[6];
    for (int i = 0; i < 6; i++) {
        mac[i] = inb(iobase + RTL8139_REG_MAC0 + i);
    }

    #if RTL8139_DEBUG
    printr("RTL8139: MAC Address = %x:%x:%x:%x:%x:%x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    print("RTL8139: Initialization complete\n");
    #endif

    // Allocate interface information structure
    rtl8139_interface *info = (rtl8139_interface *)pmm_alloc_pages(1);
    if (!info) {
        print("RTL8139: Failed to allocate interface info\n");
        return false;
    }

    info->iobase = iobase;
    info->rx_buff_virtual = (uint8_t *)rx_buffer_virt;
    info->rx_buff_physical = rx_buffer_phys;
    info->currentPacket = 0;
    info->tx_curr = 0;
    info->tok = 0;
    
    for (int i = 0; i < 4; i++) {
        info->tx_buff_virtual[i] = (uint8_t *)tx_buffers[i];
        info->tx_buff_physical[i] = tx_buffers_phys[i];
    }
    
    memcpy(info->mac, mac, 6);

    // Set up NIC structure (assuming selectedNIC is already allocated)
    if (selectedNIC) {
        selectedNIC->infoLocation = info;
        
        #if RTL8139_DEBUG
        print("RTL8139: NIC configured\n");
        #endif
    }

    return true;
}

// Send packet via RTL8139
void sendRTL8139(NIC *nic, void *packet, uint32_t packetSize) {
    if (!nic || !packet || packetSize == 0 || packetSize > 1792) {
        return;
    }

    rtl8139_interface *info = (rtl8139_interface *)nic->infoLocation;
    if (!info) {
        return;
    }

    uint16_t iobase = info->iobase;
    uint8_t tx_curr = info->tx_curr;

    // Wait for TX descriptor to be available
    uint32_t status;
    int timeout = 1000;
    do {
        status = inl(iobase + TSD_array[tx_curr]);
        if (status & (1 << 13)) {  // OWN bit - descriptor available
            break;
        }
        timeout--;
    } while (timeout > 0);

    if (timeout == 0) {
        #if RTL8139_DEBUG
        print("RTL8139: TX timeout\n");
        #endif
        return;
    }

    // Copy packet to TX buffer
    uint8_t *tx_buffer = info->tx_buff_virtual[tx_curr];
    memcpy(tx_buffer, packet, packetSize);

    // Write physical address of TX buffer
    outl(iobase + TSAD_array[tx_curr], info->tx_buff_physical[tx_curr]);

    // Write packet size and start transmission
    // Bits 0-12: packet size
    // Bit 13: OWN (0 = start transmission)
    outl(iobase + TSD_array[tx_curr], packetSize & 0x1FFF);

    #if RTL8139_DEBUG
    printr("RTL8139: Sent packet (size=%d, descriptor=%d)\n", packetSize, tx_curr);
    #endif

    // Update current TX descriptor (round-robin)
    info->tx_curr = (tx_curr + 1) % 4;
}

// Receive packets from RTL8139
void receiveRTL8139(NIC *nic) {
    if (!nic) {
        return;
    }

    rtl8139_interface *info = (rtl8139_interface *)nic->infoLocation;
    if (!info) {
        return;
    }

    uint16_t iobase = info->iobase;

    // Check command register - bit 0 = buffer empty
    while ((inb(iobase + RTL8139_REG_CMD) & CMD_BUFFER_EMPTY) == 0) {
        // Get packet header (status + length)
        uint16_t *buffer = (uint16_t *)(info->rx_buff_virtual + info->currentPacket);
        uint16_t packetStatus = buffer[0];
        uint16_t packetLength = buffer[1];

        // Check if packet is valid
        if (!(packetStatus & 0x01)) {  // ROK bit not set
            #if RTL8139_DEBUG
            print("RTL8139: Invalid packet received\n");
            #endif
            break;
        }

        // Validate packet length
        if (packetLength < 64 || packetLength > 1518) {
            #if RTL8139_DEBUG
            printr("RTL8139: Invalid packet length: %d\n", packetLength);
            #endif
            break;
        }

        // Get actual packet data (skip 4-byte header)
        uint8_t *packetData = (uint8_t *)(buffer + 2);

        #if RTL8139_DEBUG
        printr("RTL8139: Received packet (size=%d)\n", packetLength);
        #endif

        // Process packet here - you can call your network stack
        // Example: handle_ethernet_packet(nic, packetData, packetLength - 4);

        // Update read pointer (align to 4 bytes)
        info->currentPacket = (info->currentPacket + packetLength + 4 + 3) & ~3;
        
        // Wrap around if necessary
        if (info->currentPacket >= RX_BUFFER_SIZE) {
            info->currentPacket -= RX_BUFFER_SIZE;
        }

        // Update CAPR (Current Address of Packet Read) register
        // CAPR is offset by 0x10 from actual position
        outw(iobase + 0x38, info->currentPacket - 0x10);
    }
}

// Interrupt handler
void interruptHandler(AsmPassedInterrupt *regs) {
    if (!selectedNIC) {
        return;
    }

    rtl8139_interface *info = (rtl8139_interface *)selectedNIC->infoLocation;
    if (!info) {
        return;
    }

    uint16_t iobase = info->iobase;

    // Read interrupt status
    uint16_t status = inw(iobase + RTL8139_REG_ISR);

    if (status == 0 || status == 0xFFFF) {
        return;  // Not our interrupt
    }

    #if RTL8139_DEBUG
    printr("RTL8139: Interrupt (status=0x%x)\n", status);
    #endif

    // Handle RX OK
    if (status & INT_RXOK) {
        receiveRTL8139(selectedNIC);
    }

    // Handle TX OK
    if (status & INT_TXOK) {
        info->tok = 1;
        #if RTL8139_DEBUG
        print("RTL8139: TX complete\n");
        #endif
    }

    // Handle RX error
    if (status & INT_RXERR) {
        #if RTL8139_DEBUG
        print("RTL8139: RX error\n");
        #endif
    }

    // Handle TX error
    if (status & INT_TXERR) {
        #if RTL8139_DEBUG
        print("RTL8139: TX error\n");
        #endif
    }

    // Handle RX overflow
    if (status & INT_RX_OVERFLOW) {
        #if RTL8139_DEBUG
        print("RTL8139: RX overflow\n");
        #endif
    }

    // Clear interrupt status
    outw(iobase + RTL8139_REG_ISR, status);
}

// Utility function to read MAC address
void rtl8139_read_mac(uint16_t iobase, uint8_t *mac_out) {
    if (!mac_out) {
        return;
    }
    
    for (int i = 0; i < 6; i++) {
        mac_out[i] = inb(iobase + RTL8139_REG_MAC0 + i);
    }
}

// Utility function to reset device
void rtl8139_reset(uint16_t iobase) {
    outb(iobase + RTL8139_REG_CMD, CMD_RESET);
    while ((inb(iobase + RTL8139_REG_CMD) & CMD_RESET) != 0) {
        // Wait for reset
    }
}

// Enable interrupts
void rtl8139_enable_interrupts(uint16_t iobase) {
    outw(iobase + RTL8139_REG_IMR, INT_RXOK | INT_TXOK | INT_RXERR | INT_TXERR | INT_RX_OVERFLOW);
}

// Disable interrupts
void rtl8139_disable_interrupts(uint16_t iobase) {
    outw(iobase + RTL8139_REG_IMR, 0);
}

// Get interrupt status
uint16_t rtl8139_get_interrupt_status(uint16_t iobase) {
    return inw(iobase + RTL8139_REG_ISR);
}

// Clear interrupts
void rtl8139_clear_interrupts(uint16_t iobase, uint16_t mask) {
    outw(iobase + RTL8139_REG_ISR, mask);
}