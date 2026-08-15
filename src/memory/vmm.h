// vmm.h - Virtual Memory Manager
#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stdbool.h>

// Page directory/table entry flags
#define PTE_PRESENT     (1 << 0)   // Page is present in memory
#define PTE_WRITABLE    (1 << 1)   // Page is writable
#define PTE_USER        (1 << 2)   // Page is accessible from user mode
#define PTE_WRITETHROUGH (1 << 3)  // Write-through caching
#define PTE_NOCACHE     (1 << 4)   // Disable caching
#define PTE_ACCESSED    (1 << 5)   // Page has been accessed
#define PTE_DIRTY       (1 << 6)   // Page has been written to
#define PTE_PAT         (1 << 7)   // Page attribute table
#define PTE_GLOBAL      (1 << 8)   // Global page (not flushed from TLB)
#define PTE_FRAME       0xFFFFF000 // Frame address (bits 12-31)

// Virtual memory regions
#define KERNEL_VIRTUAL_BASE 0xC0000000  // 3GB
#define USER_SPACE_END      0xC0000000  // End of user space

// Page directory/table sizes
#define PAGE_DIRECTORY_SIZE 1024
#define PAGE_TABLE_SIZE     1024

// Page directory entry
typedef uint32_t pde_t;

// Page table entry
typedef uint32_t pte_t;

// Page directory structure
typedef struct {
    pde_t entries[PAGE_DIRECTORY_SIZE];
} page_directory_t;

// Page table structure
typedef struct {
    pte_t entries[PAGE_TABLE_SIZE];
} page_table_t;

// Initialize virtual memory manager
void vmm_init(void);

// Page directory management
page_directory_t* vmm_create_page_directory(void);
void vmm_destroy_page_directory(page_directory_t* dir);
void vmm_switch_page_directory(page_directory_t* dir);
page_directory_t* vmm_get_current_directory(void);

// Virtual memory mapping
bool vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
bool vmm_unmap_page(uint32_t virt);
uint32_t vmm_get_physical_address(uint32_t virt);
bool vmm_is_page_mapped(uint32_t virt);

// Allocate/free virtual memory
void* vmm_alloc_page(uint32_t flags);
void* vmm_alloc_pages(uint32_t count, uint32_t flags);
void vmm_free_page(void* virt);
void vmm_free_pages(void* virt, uint32_t count);

// Identity mapping (virtual = physical)
void vmm_identity_map(uint32_t phys, uint32_t count, uint32_t flags);
void vmm_identity_unmap(uint32_t phys, uint32_t count);

// TLB management
void vmm_flush_tlb_entry(uint32_t virt);
void vmm_flush_tlb(void);

// Page fault handler
void vmm_page_fault_handler(uint32_t error_code, uint32_t faulting_address);

#endif // VMM_H