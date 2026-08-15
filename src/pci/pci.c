// pci.c
#include "pci.h"
#include "../io/io.h"
#include "../terminal/terminal.h"
#include "../utility/utility.h"


// PCI I/O Ports
#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

// Global PCI device storage
PCIdevice pci_devices[MAX_PCI_DEVICES];
PCI pci_drivers[MAX_PCI_DEVICES];
uint32_t pci_device_count = 0;

// Read byte from PCI configuration space
uint8_t ConfigReadByte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = PCI_ADDR(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    return (inl(PCI_CONFIG_DATA) >> ((offset & 3) * 8)) & 0xFF;
}

// Read word from PCI configuration space
uint16_t ConfigReadWord(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = PCI_ADDR(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    return (inl(PCI_CONFIG_DATA) >> ((offset & 2) * 8)) & 0xFFFF;
}

// Read dword from PCI configuration space
uint32_t ConfigReadDword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = PCI_ADDR(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

// Write byte to PCI configuration space
void ConfigWriteByte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value) {
    uint32_t address = PCI_ADDR(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    outb(PCI_CONFIG_DATA + (offset & 3), value);
}

// Write word to PCI configuration space
void ConfigWriteWord(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    uint32_t address = PCI_ADDR(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    outw(PCI_CONFIG_DATA + (offset & 2), value);
}

// Write dword to PCI configuration space
void ConfigWriteDword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = PCI_ADDR(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

// Remove 'static' from these functions:
bool pci_device_exists(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t vendor = ConfigReadWord(bus, slot, func, PCI_VENDOR_ID);
    return (vendor != 0xFFFF);
}

void pci_read_device(uint8_t bus, uint8_t slot, uint8_t func, PCIdevice* device) {
    device->bus = bus;
    device->slot = slot;
    device->function = func;
    device->vendor_id = ConfigReadWord(bus, slot, func, PCI_VENDOR_ID);
    device->device_id = ConfigReadWord(bus, slot, func, PCI_DEVICE_ID);
    device->command = ConfigReadWord(bus, slot, func, PCI_COMMAND);
    device->status = ConfigReadWord(bus, slot, func, PCI_STATUS);
    device->revision_id = ConfigReadByte(bus, slot, func, PCI_REVISION_ID);
    device->prog_if = ConfigReadByte(bus, slot, func, PCI_PROG_IF);
    device->subclass = ConfigReadByte(bus, slot, func, PCI_SUBCLASS);
    device->class_code = ConfigReadByte(bus, slot, func, PCI_CLASS_CODE);
    device->header_type = ConfigReadByte(bus, slot, func, PCI_HEADER_TYPE);
    device->interrupt_line = ConfigReadByte(bus, slot, func, PCI_INTERRUPT_LINE);
    device->interrupt_pin = ConfigReadByte(bus, slot, func, PCI_INTERRUPT_PIN);
}

// Initialize PCI subsystem
void pci_init(void) {
    print("Initializing PCI subsystem...\n");
    pci_device_count = 0;
    memset(pci_devices, 0, sizeof(pci_devices));
    memset(pci_drivers, 0, sizeof(pci_drivers));
    pci_scan_bus();
    print("Found ");
    print_decimal(pci_device_count);
    print(" PCI devices\n");
}

// Scan all PCI buses
void pci_scan_bus(void) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                if (pci_device_exists(bus, slot, func)) {
                    if (pci_device_count < MAX_PCI_DEVICES) {
                        pci_read_device(bus, slot, func, &pci_devices[pci_device_count]);
                        pci_device_count++;
                    }
                    
                    // If function 0 doesn't indicate multifunction, skip other functions
                    if (func == 0) {
                        uint8_t header_type = ConfigReadByte(bus, slot, func, PCI_HEADER_TYPE);
                        if ((header_type & 0x80) == 0) {
                            break; // Not multifunction
                        }
                    }
                }
            }
        }
    }
}

// Get device by location
PCIdevice* pci_get_device(uint8_t bus, uint8_t slot, uint8_t func) {
    for (uint32_t i = 0; i < pci_device_count; i++) {
        PCIdevice* dev = &pci_devices[i];
        if (dev->bus == bus && dev->slot == slot && dev->function == func) {
            return dev;
        }
    }
    return NULL;
}

// Find device by vendor and device ID
PCIdevice* pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    for (uint32_t i = 0; i < pci_device_count; i++) {
        PCIdevice* dev = &pci_devices[i];
        if (dev->vendor_id == vendor_id && dev->device_id == device_id) {
            return dev;
        }
    }
    return NULL;
}

// Enable bus mastering for a device
void pci_enable_bus_mastering(PCIdevice* device) {
    if (!device) return;
    
    uint16_t command = ConfigReadWord(device->bus, device->slot, device->function, PCI_COMMAND);
    command |= PCI_COMMAND_MASTER;
    ConfigWriteWord(device->bus, device->slot, device->function, PCI_COMMAND, command);
}

// Enable interrupts for a device
void pci_enable_interrupts(PCIdevice* device) {
    if (!device) return;
    
    uint16_t command = ConfigReadWord(device->bus, device->slot, device->function, PCI_COMMAND);
    command &= ~PCI_COMMAND_INTX_DISABLE;
    ConfigWriteWord(device->bus, device->slot, device->function, PCI_COMMAND, command);
}

// Get general device information (including BARs)
void GetGeneralDevice(PCIdevice* device, PCIgeneralDevice* general) {
    if (!device || !general) return;
    
    // Copy base device info
    general->base = *device;
    
    // Read BARs
    for (int i = 0; i < 6; i++) {
        general->bar[i] = ConfigReadDword(device->bus, device->slot, device->function, PCI_BAR0 + (i * 4));
    }
    
    // Read additional fields
    general->cardbus_cis = ConfigReadDword(device->bus, device->slot, device->function, PCI_CARDBUS_CIS);
    general->subsystem_vendor_id = ConfigReadWord(device->bus, device->slot, device->function, PCI_SUBSYSTEM_VENDOR_ID);
    general->subsystem_id = ConfigReadWord(device->bus, device->slot, device->function, PCI_SUBSYSTEM_ID);
    general->expansion_rom = ConfigReadDword(device->bus, device->slot, device->function, PCI_EXPANSION_ROM);
    general->capabilities_ptr = ConfigReadByte(device->bus, device->slot, device->function, PCI_CAPABILITIES);
    general->interruptLine = ConfigReadByte(device->bus, device->slot, device->function, PCI_INTERRUPT_LINE);
    general->interruptPin = ConfigReadByte(device->bus, device->slot, device->function, PCI_INTERRUPT_PIN);
    general->min_grant = ConfigReadByte(device->bus, device->slot, device->function, PCI_MIN_GRANT);
    general->max_latency = ConfigReadByte(device->bus, device->slot, device->function, PCI_MAX_LATENCY);
}

// Lookup PCI driver structure for a device
PCI* lookupPCIdevice(PCIdevice* device) {
    if (!device) return NULL;
    
    // Find existing driver or create new one
    for (uint32_t i = 0; i < pci_device_count; i++) {
        if (pci_drivers[i].device == device) {
            return &pci_drivers[i];
        }
    }
    
    // Find empty slot
    for (uint32_t i = 0; i < MAX_PCI_DEVICES; i++) {
        if (pci_drivers[i].device == NULL) {
            pci_drivers[i].device = device;
            pci_drivers[i].driver_type = PCI_DRIVER_NONE;
            pci_drivers[i].driver_category = PCI_DRIVER_CATEGORY_NONE;
            pci_drivers[i].driver_data = NULL;
            pci_drivers[i].irqHandler = NULL;
            return &pci_drivers[i];
        }
    }
    
    return NULL;
}

// Setup PCI device driver
void setupPCIdeviceDriver(PCI* pci, uint8_t driver_type, uint8_t driver_category) {
    if (!pci) return;
    
    pci->driver_type = driver_type;
    pci->driver_category = driver_category;
}

// Register IRQ handler (stub - implement based on your IRQ system)
IRQHandler registerIRQhandler(uint8_t irq, IRQHandler handler) {
    // TODO: Implement IRQ handler registration based on your interrupt system
    return handler;
}

// Print device information
void pci_print_device(PCIdevice* device) {
    if (!device) return;
    
    char buffer[16];
    
    print("PCI Device: ");
    itoa(device->bus, buffer, 10);
    print(buffer);
    print(":");
    itoa(device->slot, buffer, 10);
    print(buffer);
    print(".");
    itoa(device->function, buffer, 10);
    print(buffer);
    print("\n");
    
    print("  Vendor: 0x");
    itoa(device->vendor_id, buffer, 16);
    print(buffer);
    print(" Device: 0x");
    itoa(device->device_id, buffer, 16);
    print(buffer);
    print("\n");
    
    print("  Class: ");
    print(pci_class_string(device->class_code));
    print(" (0x");
    itoa(device->class_code, buffer, 16);
    print(buffer);
    print(")\n");
}

// List all PCI devices
void pci_list_devices(void) {
    print("\n=== PCI Devices ===\n");
    for (uint32_t i = 0; i < pci_device_count; i++) {
        pci_print_device(&pci_devices[i]);
        print("\n");
    }
}

// Get class string
const char* pci_class_string(uint8_t class_code) {
    switch (class_code) {
        case PCI_CLASS_UNCLASSIFIED: return "Unclassified";
        case PCI_CLASS_STORAGE: return "Storage";
        case PCI_CLASS_NETWORK: return "Network";
        case PCI_CLASS_DISPLAY: return "Display";
        case PCI_CLASS_MULTIMEDIA: return "Multimedia";
        case PCI_CLASS_MEMORY: return "Memory";
        case PCI_CLASS_BRIDGE: return "Bridge";
        case PCI_CLASS_COMMUNICATION: return "Communication";
        case PCI_CLASS_SYSTEM: return "System";
        case PCI_CLASS_INPUT: return "Input";
        case PCI_CLASS_DOCKING: return "Docking";
        case PCI_CLASS_PROCESSOR: return "Processor";
        case PCI_CLASS_SERIAL: return "Serial";
        default: return "Unknown";
    }
}

// Get vendor string
const char* pci_vendor_string(uint16_t vendor_id) {
    switch (vendor_id) {
        case 0x10ec: return "Realtek";
        case 0x8086: return "Intel";
        case 0x1022: return "AMD";
        case 0x10de: return "NVIDIA";
        case 0x1002: return "ATI/AMD";
        case 0x1af4: return "Red Hat (VirtIO)";
        default: return "Unknown";
    }
}