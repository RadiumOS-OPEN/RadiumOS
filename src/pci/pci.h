// pci.h
#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include <stdbool.h>

// PCI Configuration Space Registers
#define PCI_VENDOR_ID           0x00
#define PCI_DEVICE_ID           0x02
#define PCI_COMMAND             0x04
#define PCI_STATUS              0x06
#define PCI_REVISION_ID         0x08
#define PCI_PROG_IF             0x09
#define PCI_SUBCLASS            0x0A
#define PCI_CLASS_CODE          0x0B
#define PCI_CACHE_LINE_SIZE     0x0C
#define PCI_LATENCY_TIMER       0x0D
#define PCI_HEADER_TYPE         0x0E
#define PCI_BIST                0x0F
#define PCI_BAR0                0x10
#define PCI_BAR1                0x14
#define PCI_BAR2                0x18
#define PCI_BAR3                0x1C
#define PCI_BAR4                0x20
#define PCI_BAR5                0x24
#define PCI_CARDBUS_CIS         0x28
#define PCI_SUBSYSTEM_VENDOR_ID 0x2C
#define PCI_SUBSYSTEM_ID        0x2E
#define PCI_EXPANSION_ROM       0x30
#define PCI_CAPABILITIES        0x34
#define PCI_INTERRUPT_LINE      0x3C
#define PCI_INTERRUPT_PIN       0x3D
#define PCI_MIN_GRANT           0x3E
#define PCI_MAX_LATENCY         0x3F

// PCI Command Register Bits
#define PCI_COMMAND_IO          0x0001
#define PCI_COMMAND_MEMORY      0x0002
#define PCI_COMMAND_MASTER      0x0004
#define PCI_COMMAND_SPECIAL     0x0008
#define PCI_COMMAND_INVALIDATE  0x0010
#define PCI_COMMAND_VGA_PALETTE 0x0020
#define PCI_COMMAND_PARITY      0x0040
#define PCI_COMMAND_WAIT        0x0080
#define PCI_COMMAND_SERR        0x0100
#define PCI_COMMAND_FAST_BACK   0x0200
#define PCI_COMMAND_INTX_DISABLE 0x0400

// PCI Class Codes
#define PCI_CLASS_UNCLASSIFIED  0x00
#define PCI_CLASS_STORAGE       0x01
#define PCI_CLASS_NETWORK       0x02
#define PCI_CLASS_DISPLAY       0x03
#define PCI_CLASS_MULTIMEDIA    0x04
#define PCI_CLASS_MEMORY        0x05
#define PCI_CLASS_BRIDGE        0x06
#define PCI_CLASS_COMMUNICATION 0x07
#define PCI_CLASS_SYSTEM        0x08
#define PCI_CLASS_INPUT         0x09
#define PCI_CLASS_DOCKING       0x0A
#define PCI_CLASS_PROCESSOR     0x0B
#define PCI_CLASS_SERIAL        0x0C

// PCI Driver Types
#define PCI_DRIVER_NONE         0
#define PCI_DRIVER_RTL8139      1
#define PCI_DRIVER_E1000        2
#define PCI_DRIVER_AHCI         3
#define PCI_DRIVER_USB          4

// PCI Driver Categories
#define PCI_DRIVER_CATEGORY_NONE    0
#define PCI_DRIVER_CATEGORY_NIC     1
#define PCI_DRIVER_CATEGORY_STORAGE 2
#define PCI_DRIVER_CATEGORY_USB     3
#define PCI_DRIVER_CATEGORY_AUDIO   4

// Maximum PCI devices
#define MAX_PCI_DEVICES 256

// Forward declarations
typedef struct AsmPassedInterrupt AsmPassedInterrupt;
typedef void (*IRQHandler)(AsmPassedInterrupt* regs);

// PCI Device Structure (minimal info from config space)
typedef struct PCIdevice {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision_id;
    uint8_t header_type;
    uint16_t command;
    uint16_t status;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
} PCIdevice;

// PCI General Device (with BARs)
typedef struct {
    PCIdevice base;
    uint32_t bar[6];
    uint32_t cardbus_cis;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;
    uint32_t expansion_rom;
    uint8_t capabilities_ptr;
    uint8_t interruptLine;
    uint8_t interruptPin;
    uint8_t min_grant;
    uint8_t max_latency;
} PCIgeneralDevice;

// PCI Driver Structure
typedef struct PCI {
    PCIdevice* device;
    uint8_t driver_type;
    uint8_t driver_category;
    void* driver_data;
    IRQHandler irqHandler;
} PCI;

// Global PCI device list
extern PCIdevice pci_devices[MAX_PCI_DEVICES];
extern PCI pci_drivers[MAX_PCI_DEVICES];
extern uint32_t pci_device_count;

// PCI Configuration Space Access
uint8_t ConfigReadByte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t ConfigReadWord(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint32_t ConfigReadDword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void ConfigWriteByte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value);
void ConfigWriteWord(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);
void ConfigWriteDword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

// PCI Device Functions
void pci_init(void);
void pci_scan_bus(void);
PCIdevice* pci_get_device(uint8_t bus, uint8_t slot, uint8_t func);
PCIdevice* pci_find_device(uint16_t vendor_id, uint16_t device_id);
void pci_enable_bus_mastering(PCIdevice* device);
void pci_enable_interrupts(PCIdevice* device);
void GetGeneralDevice(PCIdevice* device, PCIgeneralDevice* general);

// PCI Driver Management
PCI* lookupPCIdevice(PCIdevice* device);
void setupPCIdeviceDriver(PCI* pci, uint8_t driver_type, uint8_t driver_category);
IRQHandler registerIRQhandler(uint8_t irq, IRQHandler handler);

// PCI Information
void pci_print_device(PCIdevice* device);
void pci_list_devices(void);
const char* pci_class_string(uint8_t class_code);
const char* pci_vendor_string(uint16_t vendor_id);
// Add this with other function declarations
bool pci_device_exists(uint8_t bus, uint8_t slot, uint8_t func);
void pci_read_device(uint8_t bus, uint8_t slot, uint8_t func, PCIdevice* device);
// Helper macros
#define COMBINE_WORD(high, low) (((uint32_t)(high) << 16) | (uint32_t)(low))
#define PCI_ADDR(bus, slot, func, offset) \
    (0x80000000 | ((bus) << 16) | ((slot) << 11) | ((func) << 8) | ((offset) & 0xFC))

#endif // PCI_H