// vmm.c - Virtual Memory Manager Implementation
#include "vmm.h"
#include "pmm.h"
#include "../terminal/terminal.h"
#include "../utility/utility.h"

// Current page directory
static page_directory_t* current_directory = NULL;
static page_directory_t* kernel_directory = NULL;

// Helper: Get page directory index from virtual address
static inline uint32_t pde_index(uint32_t virt) {
    return virt >> 22;
}

// Helper: Get page table index from virtual address
static inline uint32_t pte_index(uint32_t virt) {
    return (virt >> 12) & 0x3FF;
}

// Helper: Get page table from directory entry
static page_table_t* vmm_get_page_table(page_directory_t* dir, uint32_t virt, bool create) {
    uint32_t pde_idx = pde_index(virt);
    
    // Check if page table exists
    if (!(dir->entries[pde_idx] & PTE_PRESENT)) {
        if (!create) {
            return NULL;
        }
        
        // Allocate new page table
        page_table_t* table = (page_table_t*)pmm_alloc_page();
        if (!table) {
            return NULL;
        }
        
        // Clear page table
        memset(table, 0, sizeof(page_table_t));
        
        // Add to directory
        dir->entries[pde_idx] = ((uint32_t)table & PTE_FRAME) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }
    
    return (page_table_t*)(dir->entries[pde_idx] & PTE_FRAME);
}

// Flush TLB entry
void vmm_flush_tlb_entry(uint32_t virt) {
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

// Flush entire TLB
void vmm_flush_tlb(void) {
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

// Initialize virtual memory manager
void vmm_init(void) {
    print("VMM: Initializing virtual memory manager...\n");
    
    // Create kernel page directory
    kernel_directory = (page_directory_t*)pmm_alloc_page();
    if (!kernel_directory) {
        print("VMM: ERROR - Failed to allocate kernel page directory!\n");
        return;
    }
    
    memset(kernel_directory, 0, sizeof(page_directory_t));
    
    // Identity map first 4MB (kernel space)
    vmm_identity_map(0, 1024, PTE_PRESENT | PTE_WRITABLE);
    
    // Switch to new page directory
    vmm_switch_page_directory(kernel_directory);
    
    // Enable paging
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;  // Set PG bit
    asm volatile("mov %0, %%cr0" :: "r"(cr0));
    
    print("VMM: Paging enabled\n");
}

// Create a new page directory
page_directory_t* vmm_create_page_directory(void) {
    page_directory_t* dir = (page_directory_t*)pmm_alloc_page();
    if (!dir) {
        return NULL;
    }
    
    memset(dir, 0, sizeof(page_directory_t));
    
    // Copy kernel mappings (upper 1GB)
    if (kernel_directory) {
        for (uint32_t i = 768; i < 1024; i++) {  // 768 = 3GB / 4MB
            dir->entries[i] = kernel_directory->entries[i];
        }
    }
    
    return dir;
}

// Destroy a page directory
void vmm_destroy_page_directory(page_directory_t* dir) {
    if (!dir || dir == kernel_directory) {
        return;
    }
    
    // Free all user-space page tables
    for (uint32_t i = 0; i < 768; i++) {
        if (dir->entries[i] & PTE_PRESENT) {
            page_table_t* table = (page_table_t*)(dir->entries[i] & PTE_FRAME);
            pmm_free_page(table);
        }
    }
    
    // Free the directory itself
    pmm_free_page(dir);
}

// Switch to a different page directory
void vmm_switch_page_directory(page_directory_t* dir) {
    if (!dir) {
        return;
    }
    
    current_directory = dir;
    
    // Load CR3 with physical address of page directory
    asm volatile("mov %0, %%cr3" :: "r"((uint32_t)dir) : "memory");
}

// Get current page directory
page_directory_t* vmm_get_current_directory(void) {
    return current_directory;
}

// Map a virtual page to a physical page
bool vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    if (!current_directory) {
        return false;
    }
    
    // Align addresses
    virt &= PTE_FRAME;
    phys &= PTE_FRAME;
    
    // Get or create page table
    page_table_t* table = vmm_get_page_table(current_directory, virt, true);
    if (!table) {
        return false;
    }
    
    // Set page table entry
    uint32_t pte_idx = pte_index(virt);
    table->entries[pte_idx] = phys | flags | PTE_PRESENT;
    
    // Flush TLB
    vmm_flush_tlb_entry(virt);
    
    return true;
}

// Unmap a virtual page
bool vmm_unmap_page(uint32_t virt) {
    if (!current_directory) {
        return false;
    }
    
    virt &= PTE_FRAME;
    
    page_table_t* table = vmm_get_page_table(current_directory, virt, false);
    if (!table) {
        return false;
    }
    
    uint32_t pte_idx = pte_index(virt);
    table->entries[pte_idx] = 0;
    
    vmm_flush_tlb_entry(virt);
    
    return true;
}

// Get physical address from virtual address
uint32_t vmm_get_physical_address(uint32_t virt) {
    if (!current_directory) {
        return 0;
    }
    
    page_table_t* table = vmm_get_page_table(current_directory, virt, false);
    if (!table) {
        return 0;
    }
    
    uint32_t pte_idx = pte_index(virt);
    if (!(table->entries[pte_idx] & PTE_PRESENT)) {
        return 0;
    }
    
    return (table->entries[pte_idx] & PTE_FRAME) | (virt & 0xFFF);
}

// Check if a page is mapped
bool vmm_is_page_mapped(uint32_t virt) {
    return vmm_get_physical_address(virt) != 0;
}

// Allocate a virtual page
void* vmm_alloc_page(uint32_t flags) {
    // Allocate physical page
    void* phys = pmm_alloc_page();
    if (!phys) {
        return NULL;
    }
    
    // For now, use identity mapping
    // TODO: Implement proper virtual address allocation
    if (!vmm_map_page((uint32_t)phys, (uint32_t)phys, flags)) {
        pmm_free_page(phys);
        return NULL;
    }
    
    return phys;
}

// Allocate multiple virtual pages
void* vmm_alloc_pages(uint32_t count, uint32_t flags) {
    void* phys = pmm_alloc_pages(count);
    if (!phys) {
        return NULL;
    }
    
    // Map all pages
    for (uint32_t i = 0; i < count; i++) {
        uint32_t virt = (uint32_t)phys + (i * PAGE_SIZE);
        uint32_t physical = (uint32_t)phys + (i * PAGE_SIZE);
        
        if (!vmm_map_page(virt, physical, flags)) {
            // Unmap what we've mapped so far
            for (uint32_t j = 0; j < i; j++) {
                vmm_unmap_page((uint32_t)phys + (j * PAGE_SIZE));
            }
            pmm_free_pages(phys, count);
            return NULL;
        }
    }
    
    return phys;
}

// Free a virtual page
void vmm_free_page(void* virt) {
    if (!virt) {
        return;
    }
    
    uint32_t phys = vmm_get_physical_address((uint32_t)virt);
    if (phys) {
        vmm_unmap_page((uint32_t)virt);
        pmm_free_page((void*)phys);
    }
}

// Free multiple virtual pages
void vmm_free_pages(void* virt, uint32_t count) {
    if (!virt) {
        return;
    }
    
    for (uint32_t i = 0; i < count; i++) {
        uint32_t addr = (uint32_t)virt + (i * PAGE_SIZE);
        vmm_free_page((void*)addr);
    }
}

// Identity map physical pages (virtual = physical)
void vmm_identity_map(uint32_t phys, uint32_t count, uint32_t flags) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t addr = phys + (i * PAGE_SIZE);
        vmm_map_page(addr, addr, flags);
    }
}

// Remove identity mapping
void vmm_identity_unmap(uint32_t phys, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t addr = phys + (i * PAGE_SIZE);
        vmm_unmap_page(addr);
    }
}

// Page fault handler
void vmm_page_fault_handler(uint32_t error_code, uint32_t faulting_address) {
    print("PAGE FAULT: ");
    
    if (!(error_code & 0x1)) {
        print("Page not present ");
    }
    if (error_code & 0x2) {
        print("Write violation ");
    }
    if (error_code & 0x4) {
        print("User mode ");
    }
    if (error_code & 0x8) {
        print("Reserved bit set ");
    }
    if (error_code & 0x10) {
        print("Instruction fetch ");
    }
    
    printr("at address 0x%x\n", faulting_address);
    
    // For now, halt
    print("System halted.\n");
    while(1) {
        asm volatile("hlt");
    }
}