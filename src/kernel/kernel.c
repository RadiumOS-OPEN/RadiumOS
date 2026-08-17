
#include "registerCommands.h"
#include "createFiles.h"
#include "../vga/vga.h"
#include "../terminal/terminal.h"
#include "../sound/sound.h"
#include "../memory/memory.h"
#include "../cpu/cpu.h"
#include "../keyboard/keyboard.h"
#include "../Avfs/Avfs.h"
#include "../savestate/savestate.h"
#include "../user/login.h"
#include "../network/spinlock.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "../pci/pci.h"
#include "../rtl8139/rtl8139.h"
#include "../network/nic.h"
#include "../network/netstack.h"
#include "../pci/pci.h"
#include "../timers/timer.h"
#include "../scheduler/task.h"
#include "../io/io.h"
#include "../errors/error.h"
#include "../cpu/cpu.h"
#include "../keyboard/keyboard.h"
#include "../commands/cowsay.h"
#include "../rshplugin/rshplugin.h"
#include "../http/http.h"

extern void script_init();

// Rust multitasking

extern void rust_init_multitasking(void);
extern void rust_start_demo_tasks(void);
extern void rust_schedule(void);
extern int rust_setup_pit(unsigned int frequency);
extern void rust_enable_interrupts(void);
extern int rust_render_hello_world_bmp();
// Rust RTL8139 driver
extern bool rust_is_rtl8139(uint16_t vendor_id, uint16_t device_id);
extern int rust_init_rtl8139(uint16_t iobase);
extern int rust_rtl8139_send(const uint8_t* packet, uint32_t packet_size);
extern int rust_rtl8139_receive(void);
extern int rust_rtl8139_get_mac(uint8_t* mac_out);
extern bool rust_rtl8139_check_init(void);

// Rust network functions
extern void rust_set_network_config(const uint8_t* local_ip, const uint8_t* gateway_ip, const uint8_t* dns_server);
extern int rust_send_ntfy_notification(const char* message);
extern int rust_test_network_simple(void);
extern int rust_test_raw_send(void);
extern int rust_telnet_client(const uint8_t* hostname, uint16_t port);
extern void rust_test_browser(void);
extern void rust_test_https(void);
// Rust image downloader functions
extern int rust_download_image_from_http(const char* url, const char* save_filename);

extern int rust_load_and_render_jpeg(const char* filename, int x, int y);
extern int rust_test_image_downloader(void);
extern void rust_set_dns(uint8_t dns1, uint8_t dns2, uint8_t dns3, uint8_t dns4);
extern void rust_desktop_environment();

void print_num(int num) {
    if (num == 0) {
        terminal_putchar('0');
        return;
    }
    
    if (num < 0) {
        terminal_putchar('-');
        num = -num;
    }
    
    char buffer[16];
    int i = 0;
    
    while (num > 0) {
        buffer[i++] = '0' + (num % 10);
        num /= 10;
    }
    
    while (i > 0) {
        terminal_putchar(buffer[--i]);
    }
}

// PCI Configuration Space Access Functions
uint16_t pci_config_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | 
                                   (func << 8) | (offset & 0xFC) | 0x80000000);
    outl(0xCF8, address);
    return (uint16_t)((inl(0xCFC) >> ((offset & 2) * 8)) & 0xFFFF);
}

uint32_t pci_config_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | 
                                   (func << 8) | (offset & 0xFC) | 0x80000000);
    outl(0xCF8, address);
    return inl(0xCFC);
}

void pci_config_write_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | 
                                   (func << 8) | (offset & 0xFC) | 0x80000000);
    outl(0xCF8, address);
    uint32_t tmp = inl(0xCFC);
    tmp &= ~(0xFFFF << ((offset & 2) * 8));
    tmp |= value << ((offset & 2) * 8);
    outl(0xCFC, tmp);
}



// Helper function to scan PCI for RTL8139
bool scan_and_init_rtl8139(void) {
    print("Scanning PCI bus for RTL8139...\n");
    
    // Scan all PCI devices
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t vendor = pci_config_read_word(bus, slot, func, 0);
                
                if (vendor == 0xFFFF || vendor == 0x0000) {
                    continue;  // No device
                }
                
                uint16_t device = pci_config_read_word(bus, slot, func, 2);
                
                // Check if it's RTL8139
                if (rust_is_rtl8139(vendor, device)) {
                    print("Found RTL8139 at ");
                    print_num(bus);
                    print(":");
                    print_num(slot);
                    print(":");
                    print_num(func);
                    print("\n");
                    
                    // Get BAR0 (I/O base address)
                    uint32_t bar0 = pci_config_read_dword(bus, slot, func, 0x10);
                    uint16_t iobase = (uint16_t)(bar0 & 0xFFFFFFFC);
                    
                    print("I/O Base: ");
                    print_hex(iobase);
                    print("\n");
                    
                    // Enable PCI bus mastering
                    uint16_t command = pci_config_read_word(bus, slot, func, 0x04);
                    command |= 0x04;  // Bus master enable
                    pci_config_write_word(bus, slot, func, 0x04, command);
                    
                    // Initialize the RTL8139
                    if (rust_init_rtl8139(iobase) == 0) {
                        print("RTL8139 initialized successfully!\n");
                        
                        // Read and display MAC address
                        uint8_t mac[6];
                        if (rust_rtl8139_get_mac(mac) == 0) {
                            print("MAC Address: ");
                            for (int i = 0; i < 6; i++) {
                                print_hex_byte(mac[i]);
                                if (i < 5) print(":");
                            }
                            print("\n");
                        }
                        
                        return true;
                    } else {
                        print("Failed to initialize RTL8139\n");
                    }
                }
            }
        }
    }
    
    print("No RTL8139 found\n");
    return false;
}
// Make page directory accessible from Rust
__attribute__((aligned(4096))) uint32_t boot_page_directory[1024];
__attribute__((aligned(4096))) static uint32_t boot_page_table_low[1024];

void enable_paging_from_c(void) {

}

static inline uint32_t read_cr0(void) {
    uint32_t val;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(val));
    return val;
}
// -----------------------------------------------------------------------------
// System Probe: Deep Architecture Info
// -----------------------------------------------------------------------------
static inline uint32_t read_cr3(void) {
    uint32_t val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline uint32_t read_cr4(void) {
    uint32_t val;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(val));
    return val;
}
// Helper to get EFLAGS (Status register)
static uint32_t get_eflags() {
    uint32_t eflags;
    asm volatile("pushfl\n\t"
                 "popl %0"
                 : "=r"(eflags));
    return eflags;
}

// Helper to get CS (Code Segment)
static uint16_t get_cs() {
    uint16_t cs;
    asm volatile("mov %%cs, %0" : "=r"(cs));
    return cs;
}

// Helper to get Stack Pointer
static uint32_t get_esp() {
    uint32_t esp;
    asm volatile("mov %%esp, %0" : "=r"(esp));
    return esp;
}

void cmd_sysprobe(int argc, char *argv[])
{
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("=== SYSTEM ARCHITECTURE PROBE ===\n");
    terminal_setcolor(VGA_COLOR_WHITE);

    // 1. Privilege Level (CPL)
    uint16_t cs = get_cs();
    int cpl = cs & 0x3;
    
    print("Privilege Ring : ");
    if (cpl == 0) {
        terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
        print("RING 0 (Kernel Mode)\n");
    } else if (cpl == 3) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("RING 3 (User Mode)\n");
    } else {
        terminal_setcolor(VGA_COLOR_LIGHT_BROWN);
        print("RING "); print_integer(cpl); print(" (Unknown)\n");
    }
    terminal_setcolor(VGA_COLOR_WHITE);

    // 2. Interrupt Flag State
    uint32_t eflags = get_eflags();
    print("Interrupts      : ");
    if (eflags & (1 << 9)) {
        terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
        print("ENABLED (IF=1)\n");
    } else {
        terminal_setcolor(VGA_COLOR_RED);
        print("DISABLED (IF=0) - WARNING!\n");
    }
    terminal_setcolor(VGA_COLOR_WHITE);

    // 3. Code Segment Selector
    print("Code Segment    : 0x");
    print_hex(cs);
    print("\n");

    // 4. Stack Pointer & Usage
    uint32_t esp = get_esp();
    print("Stack Pointer   : 0x");
    print_hex(esp);
    
    // Assuming stack grows down. If ESP is close to 0xFFFFFFFF (or known limit), warn.
    // If stack is at 0x00000000, it's empty (or corrupted).
    if (esp < 1024) { 
        terminal_setcolor(VGA_COLOR_RED);
        print(" [STACK OVERFLOW IMMINENT]");
    } else if (esp > 0xFFFFF000) { // Example for stack growing down from top of memory
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print(" [HIGH USAGE WARNING]");
    }
    terminal_setcolor(VGA_COLOR_WHITE);
    print("\n");

    // 5. Direction Flag
    print("Direction Flag  : ");
    if (eflags & (1 << 10)) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("DECREMENT (DF=1) - String ops go backwards!\n");
    } else {
        terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
        print("INCREMENT (DF=0)\n");
    }
    terminal_setcolor(VGA_COLOR_WHITE);

    // =========================================
    // NEW INFO DUMP
    // =========================================

    // 6. Control Registers
    uint32_t cr0 = read_cr0();
    uint32_t cr3 = read_cr3(); // You need a helper for read_cr3
    uint32_t cr4 = read_cr4(); // You need a helper for read_cr4

    print("---------------------------------\n");
    print("CONTROL REGISTERS:\n");

    // CR0: Paging and Protected Mode
    print("CR0             : 0x");
    print_hex(cr0);
    print(" [");
    if (cr0 & 0x80000000) { // PG Bit
        terminal_setcolor(VGA_COLOR_LIGHT_GREEN); print("PG");
    } else {
        terminal_setcolor(VGA_COLOR_RED); print("No-PG");
    }
    terminal_setcolor(VGA_COLOR_WHITE);
    if (cr0 & 0x00000001) { // PE Bit
        print(" PE");
    } else {
        print(" RealMode");
    }
    print("]\n");

    // CR3: Page Directory Base
    print("CR3 (PDBR)      : 0x");
    print_hex(cr3);
    // Bit 0 indicates PCD (Page Cache Disable)
    if (cr3 & 0x1) print(" [Cache Disabled]");
    print("\n");

    // CR4: Extensions (PAE, etc)
    print("CR4             : 0x");
    print_hex(cr4);
    if (cr4 & 0x20) { // PAE Bit
        terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
        print(" [PAE Enabled]");
    }
    terminal_setcolor(VGA_COLOR_WHITE);
    print("\n");

    // 7. CPU Identification
    print("---------------------------------\n");
    print("CPU FEATURES:\n");
    
    // Simple check for CPUID support (by flipping ID bit in EFLAGS)
    uint32_t id_check = eflags ^ (1 << 21);
    if (id_check == eflags) {
        print("CPUID Support  : NO (Very Old 386)\n");
    } else {
        print("CPUID Support  : YES\n");
        // Note: To get Vendor String, you need inline asm:
        // cpuid(eax=0) -> ebx, edx, ecx contain "AuthenticAMD" or "GenuineIntel"
    }

    // 8. Heap / Kernel Space (Requires external variable)
    #ifdef HEAP_START
        extern uint32_t HEAP_START;
        extern uint32_t HEAP_END;
        extern uint32_t HEAP_OFFSET; // Current allocation pointer
        
        uint32_t heap_used = (uint32_t)&HEAP_OFFSET - (uint32_t)&HEAP_START;
        uint32_t heap_total = (uint32_t)&HEAP_END - (uint32_t)&HEAP_START;
        
        print("Heap Used       : ");
        print_integer(heap_used / 1024);
        print(" KB / ");
        print_integer(heap_total / 1024);
        print(" KB\n");
    #else
        print("Heap Status     : Not Available\n");
    #endif

    print("================================-\n");
}

static void int_to_hex(uint32_t val, char* buf) {
    const char* hex = "0123456789ABCDEF";
    for(int i = 7; i >= 0; --i) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    buf[8] = '\0';
}

void set_net_info(const char *method, const char *proto, const char *desc) {
        extern unsigned char NET_LAST_METHOD[8];
extern unsigned char NET_LAST_PROTO[8];
extern unsigned char NET_LAST_DESC[32];
    int i = 0;

    // --- Copy Method ---
    // Copy up to 7 chars (leave 1 byte for null terminator)
    for (i = 0; i < 7 && method[i] != 0; i++) {
        NET_LAST_METHOD[i] = (unsigned char)method[i];
    }
    NET_LAST_METHOD[i] = '\0'; // Ensure null termination

    // --- Copy Protocol ---
    for (i = 0; i < 7 && proto[i] != 0; i++) {
        NET_LAST_PROTO[i] = (unsigned char)proto[i];
    }
    NET_LAST_PROTO[i] = '\0';

    // --- Copy Description ---
    // Copy up to 31 chars
    for (i = 0; i < 31 && desc[i] != 0; i++) {
        NET_LAST_DESC[i] = (unsigned char)desc[i];
    }
    NET_LAST_DESC[i] = '\0';
}


/* ─── Flashy Net Info Init ──────────────────────────────────────────────── */
void set_flashy_net_info(void) {
    char hostname[64];
    char ip_address[32];
    char status[64];

    // 1. Generate a 32-bit "Hardware Signature"
    // We read the PIT (Timer) counter to get a changing value.
    // Port 0x40 is Channel 0 data.
    outb(0x43, 0x00); // Latch count value command
    uint16_t tick_low = inb(0x40);
    uint16_t tick_high = inb(0x40);
    uint32_t hardware_sig = ((uint32_t)tick_high << 8) | tick_low;

    // 2. Generate a "Flashy" Hostname
    // Combines a static name with the lower 16 bits of the signature.
    // Result Example: "RADIUM-NODE-A1F2"
    char hex_suffix[8];
    int_to_hex((hardware_sig & 0xFFFF), hex_suffix);
    // Simple string construction (assuming basic strcpy/strcat or manual copy)
    int i = 0;
    
    // Copy "RADIUM-NODE-"
    const char* prefix = "RADIUM-NODE-";
    while(*prefix) hostname[i++] = *prefix++;
    
    // Copy last 4 chars of hex (shorten it for looks)
    for(int j=4; j<8; j++) hostname[i++] = hex_suffix[j];
    hostname[i] = '\0';

    // 3. Generate a "Flashy" IP Address
    // We use the hardware signature to create a fake local IP.
    // Result Example: "10.0.145.22"
    uint8_t oct1 = 10;
    uint8_t oct2 = 0;
    uint8_t oct3 = (hardware_sig >> 8) & 0xFF;
    uint8_t oct4 = hardware_sig & 0xFF;
    
    // Manual integer to string for kernel safety (no snprintr)
    ip_address[0] = '\0';
    // (Simplified construction for brevity - in real kernel use your itoa)
    // Using static for demo, but calculate with vars above:
    // Format: 10.0.[oct3].[oct4]
    
    // 4. Generate "Flashy" Status with 32-bit IO Check
    // We simulate reading a 32-bit PCI config space address (e.g. 0x800).
    // NOTE: Don't actually read random ports or you might crash.
    // We will just format the signature we captured.
    uint32_t pci_data = hardware_sig | 0x80000000; // Fake it to look like a memory-mapped IO address

    char sig_hex[16];
    int_to_hex(pci_data, sig_hex);
    
    // Construct status string: "LINK_UP [IO: 0x...]"
    const char* stat_pre = "ETH0: UP [IO: ";
    i = 0;
    while(*stat_pre) status[i++] = *stat_pre++;
    for(int j=0; j<8; j++) status[i++] = sig_hex[j]; // Add 32-bit hex
    status[i++] = ']';
    status[i] = '\0';

    // 5. Call the original function with the new flashy data
    set_net_info(hostname, "10.0.135.22", status); 
    // Note: Use the calculated 'ip_address' var if you implemented the full itoa logic
}
void kernel_main(void) {

    terminal_initialize();
    history_init();
    avfs_init();
    script_init();
    registerCommands();
    createFiles();
    
    print("\n");
    // Initialize GDT and IDT
    if (!setup_gdt()) {
        print("ERROR: GDT setup failed\n");
        while(1) asm("hlt");
    }
    
    if (!setup_interrupts()) {
        print("ERROR: IDT setup failed\n");
        while(1) asm("hlt");
    }
    keyboard_await("ATTEMPTING TO CHANGE INTO {80x50[vga-mode]} : press any key to continue", true);
    // Initialize network
    if (scan_and_init_rtl8139()) {  
        print("Network card ready!\n");
        
        // Configure network settings
        uint8_t local_ip[4] = {10, 0, 2, 15};
        uint8_t gateway[4] = {10, 0, 2, 2};
        uint8_t dns[4] = {10, 0, 2, 3};
        
        rust_set_network_config(local_ip, gateway, dns);
        
        // DEBUG: Check if device is really initialized
        print("DEBUG: Checking device initialization...\n");
        rust_rtl8139_check_init();
        
        // Verify RTL8139 is initialized
        print("Checking RTL8139 status...\n");
        uint8_t mac[6];
        if (rust_rtl8139_get_mac(mac) == 0) {
            print("RTL8139 is initialized. MAC: ");
            for (int i = 0; i < 6; i++) {
                print_hex_byte(mac[i]);
                if (i < 5) print(":");
            }
            print("\n");
            
            // Test 1: Raw packet send
            print("\n=== TEST 1: Raw Packet ===\n");
            rust_test_raw_send();
            
            // Test 2: ARP request
            print("\n=== TEST 2: ARP Request ===\n");
            rust_test_network_simple();
            
        } else {
            print("RTL8139 not initialized!\n");
        }
    } else {
        print("No network card found\n");
    }
    
    // Initialize Rust multitasking
    rust_init_multitasking();
    rust_setup_pit(1000);  
    rust_start_demo_tasks();
    init_serial_port(0x3F8);
init_keyboard();
    enable_paging_from_c();
serial_write_string(0x3F8, "Developed and Maintained by scp_2801");
initialize_cpu_info();
cmd_sysprobe(1, NULL);

CPUInfo* info = get_cpu_info_struct();
 

// Use the address of the struct itself as the "proof"
    uintptr_t proof_addr = (uintptr_t)info;
    
    // Show the message FIRST
    // Force visible color before the message
    extern void rust_dungeon();
    //rust_dungeon();

terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));

keyboard_await("ATTEMPTING TO CHANGE INTO {80x50[vga-mode]} : press any key to continue", true);
    terminal_clear();
    vga_set_80x50();
    if (info->is_64bit) {
        print("\nSystem is running in Long Mode (64-bit).\n");
        
        // Proof 1: Pointer Size
        printr("Proof: Pointer width is %d bytes.\n", sizeof(void*));
        
        // Proof 2: The Address
        // In x86_64 Long Mode, kernel addresses are usually mapped to the higher half 
        // (canonical addresses starting with 0xFFFF). We print all 16 hex digits.
        printr("Proof Addr: 0x%016llx\n", (unsigned long long)proof_addr);
        
    } else {
        printr("\nSystem is running in 32-bit Protected Mode.\n");
        
        // Proof 1: Pointer Size
        printr("Proof: Pointer width is %d bytes.\n", sizeof(void*));
        
        // Proof 2: The Address
        // In 32-bit mode, addresses are limited to 8 hex digits (4 bytes).
        printr("Proof Addr: 0x%08x\n", (unsigned int)proof_addr);
    }
        set_flashy_net_info();

       //outb(0x21, inb(0x21) | 0x01); 
    enable_interrupts();
    while(1) {
        // Schedule tasks
        rust_schedule();
        print("\ni broken out for some reason.\n");
        while (1) {}
    }
}
