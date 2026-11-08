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
#include "../user/users.h"
#include "../commands/desktop.h"
#include "stackguard.h"
#include "../network/spinlock.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "../pci/pci.h"
#include "../rtl8139/rtl8139.h"
#include "../vga/fbcon.h"
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
#include "../commands/bootsplash.h"
#include "../commands/shellvars.h"
#include "../commands/bootpage.h"
#include "bootprofile.h"
#include "cmdline.h"
#include "../timers/tsc.h"

// How long the boot pauses on its status screens before carrying on by itself (any key skips it).
#define BOOT_WAIT_MS 250
#include "../rshplugin/rshplugin.h"
#include "../http/http.h"

extern void helios_init();

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

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002u
#define MULTIBOOT_INFO_MEMORY      (1u << 0)
#define MULTIBOOT_INFO_CMDLINE     (1u << 2)
#define MULTIBOOT_INFO_MMAP        (1u << 6)

typedef struct __attribute__((packed)) {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t symbols[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
} multiboot_info_v1_t;

typedef struct __attribute__((packed)) {
    uint32_t size;
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
} multiboot_mmap_entry_v1_t;

extern uint8_t _kernel_end;

static void initialize_pmm_from_map(const MemoryMap* map) {
    pmm_init(0x140000000ULL);
    for (int i = 0; i < map->entry_count; i++) {
        if (map->entries[i].type == MEMORY_TYPE_AVAILABLE) {
            pmm_init_region(map->entries[i].base_addr, map->entries[i].length);
        }
    }

    // The kernel, boot tables, stack, and heap occupy the image below this symbol.
    pmm_deinit_region(0, (uint64_t)(uintptr_t)&_kernel_end);

    // These virtual ranges retain their physical MMIO/ROM mappings over the high-RAM window.
    pmm_deinit_region(0x13D000000ULL, 0x01000000ULL);
    pmm_deinit_region(0x13EA00000ULL, 0x00200000ULL);
    pmm_deinit_region(0x13EC00000ULL, 0x00400000ULL);
    pmm_deinit_region(0x13FE00000ULL, 0x00200000ULL);
}

// GRUB passes "source=disk" when it loaded the updated kernel from /disk (see `update`), else the ISO's kernel runs.
// The whole command line is kept for cmdline.h (target, safe, nonet, ...).
static char g_boot_source[8] = "iso";
const char *kernel_boot_source(void) { return g_boot_source; }

static void capture_boot_source(uint32_t magic, uint32_t info_address) {
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC || info_address == 0) return;
    const multiboot_info_v1_t* info = (const multiboot_info_v1_t*)(uintptr_t)info_address;
    if (!(info->flags & MULTIBOOT_INFO_CMDLINE) || info->cmdline == 0) return;
    cmdline_set((const char*)(uintptr_t)info->cmdline);
    if (cmdline_has("source=disk")) strcpy(g_boot_source, "disk");
    else if (cmdline_has("source=old")) strcpy(g_boot_source, "old");
}

static bool load_multiboot_memory_map(uint32_t magic, uint32_t info_address) {
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC || info_address == 0) return false;

    const multiboot_info_v1_t* info = (const multiboot_info_v1_t*)(uintptr_t)info_address;
    MemoryMap map = {0};

    if ((info->flags & MULTIBOOT_INFO_MMAP) && info->mmap_length >= 24
        && info->mmap_addr <= UINT32_MAX - info->mmap_length) {
        uint32_t cursor = info->mmap_addr;
        uint32_t remaining = info->mmap_length;
        while (remaining >= 24 && map.entry_count < MEMORY_MAP_MAX_ENTRIES) {
            const multiboot_mmap_entry_v1_t* entry =
                (const multiboot_mmap_entry_v1_t*)(uintptr_t)cursor;
            if (entry->size < 20 || entry->size > remaining - 4) break;

            MemoryMapEntry* output = &map.entries[map.entry_count++];
            output->base_addr = entry->base_addr;
            output->length = entry->length;
            output->type = entry->type;

            uint32_t step = entry->size + 4;
            cursor += step;
            remaining -= step;
        }
    } else if (info->flags & MULTIBOOT_INFO_MEMORY) {
        map.entries[0] = (MemoryMapEntry){ 0, (uint64_t)info->mem_lower * 1024, MEMORY_TYPE_AVAILABLE };
        map.entries[1] = (MemoryMapEntry){ 0x100000, (uint64_t)info->mem_upper * 1024, MEMORY_TYPE_AVAILABLE };
        map.entry_count = 2;
    }

    if (map.entry_count == 0) return false;
    set_memory_map_from_bootloader(&map);
    initialize_pmm_from_map(&map);
    return true;
}

void kernel_main(uint32_t multiboot_magic, uint32_t multiboot_info_address) {
    stackguard_init();   // before anything protected is running; kernel_main itself never returns

    fbcon_init();   // 1280x720 console when VBE is available; plain text mode otherwise
    terminal_initialize();
    tsc_calibrate();
    boot_mark("start");
    capture_boot_source(multiboot_magic, multiboot_info_address);
    { extern int g_tty_mirror; g_tty_mirror = cmdline_has("ttylog"); }
    if (!load_multiboot_memory_map(multiboot_magic, multiboot_info_address)) {
        print("WARNING: Multiboot memory map unavailable\n");
    }
    if (!cmdline_has("nosplash") && !cmdline_has("safe")) show_boot_splash();
    if (cmdline_has("verbose") || cmdline_has("debug")) printr("Kernel command line: %s\n", cmdline_get());
    boot_mark("splash");
    history_init();
    avfs_init();
    boot_mark("avfs_init");
    script_init();
    registerCommands();
    boot_mark("commands");
    createFiles();
    boot_mark("createFiles");
    
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
    boot_mark("await1");
    // Initialize network
    if (!cmdline_has("nonet") && !cmdline_has("safe") && scan_and_init_rtl8139()) {
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

            // QEMU's user-mode network only delivers replies (DNS, TCP) to a guest whose MAC it has learned from
            // an ARP request, and nothing here answers its own ARP request. One broadcast ARP for the gateway
            // costs no waiting (the reply is ignored).
            uint8_t arp[60] = {0};
            memset(arp, 0xFF, 6);
            memcpy(arp + 6, mac, 6);
            arp[12] = 0x08; arp[13] = 0x06;                  // ARP
            arp[14] = 0x00; arp[15] = 0x01;                  // Ethernet
            arp[16] = 0x08; arp[17] = 0x00;                  // IPv4
            arp[18] = 6;    arp[19] = 4;
            arp[20] = 0x00; arp[21] = 0x01;                  // request
            memcpy(arp + 22, mac, 6);
            memcpy(arp + 28, local_ip, 4);
            memcpy(arp + 38, gateway, 4);
            rust_rtl8139_send(arp, sizeof(arp));
        } else {
            print("RTL8139 not initialized!\n");
        }
    } else {
        print(cmdline_has("nonet") || cmdline_has("safe") ? "Network disabled on the kernel command line\n" : "No network card found\n");
    }
    
    // Initialize Rust multitasking
    boot_mark("network");
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

keyboard_await_timeout("Switching to 80x50 text mode - press any key to skip the wait", true, cmdline_has("fastboot") ? 0 : BOOT_WAIT_MS);
    boot_mark("await2");
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
    helios_init();
    boot_mark("pre-finish");
    bool desk = desktop_boot_wanted();
    if (!desk) users_boot_login();
    boot_mark("login");
    boot_finish();
    boot_mark("done");
    if (desk) {
        keyboard_pause(true);
        desktop_boot();
        keyboard_pause(false);
        if (users_enabled() && !users_has_session()) users_login_prompt();
        terminal_clear();
        print("RadiumOS text console - type desktop to go back to the desktop\n");
        shell_show_prompt();
    }
    while (1) {
        halt();
    }
}
