// pmm.h - Physical Memory Manager
#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stdbool.h>

// Page size (4KB)
#define PAGE_SIZE 4096
#define PAGE_ALIGN(addr) (((addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(addr) ((addr) & ~(PAGE_SIZE - 1))

// Convert between pages and addresses
#define ADDR_TO_PAGE(addr) ((addr) / PAGE_SIZE)
#define PAGE_TO_ADDR(page) ((page) * PAGE_SIZE)

// Memory region structure
typedef struct {
    uint32_t base;
    uint32_t length;
    uint32_t type;
} memory_region_t;

// Memory map types
#define MEMORY_TYPE_USABLE      1
#define MEMORY_TYPE_RESERVED    2
#define MEMORY_TYPE_ACPI        3
#define MEMORY_TYPE_NVS         4
#define MEMORY_TYPE_BADRAM      5

// Initialize physical memory manager
void pmm_init(uint32_t mem_size);
void pmm_init_region(uint32_t base, uint32_t length);
void pmm_deinit_region(uint32_t base, uint32_t length);

// Allocate/free physical pages
void* pmm_alloc_page(void);
void* pmm_alloc_pages(uint32_t count);
void pmm_free_page(void* addr);
void pmm_free_pages(void* addr, uint32_t count);

// Query memory information
uint32_t pmm_get_total_memory(void);
uint32_t pmm_get_used_memory(void);
uint32_t pmm_get_free_memory(void);
uint32_t pmm_get_total_pages(void);
uint32_t pmm_get_used_pages(void);
uint32_t pmm_get_free_pages(void);

// Mark pages as used/free
void pmm_mark_page_used(uint32_t page);
void pmm_mark_page_free(uint32_t page);
bool pmm_is_page_free(uint32_t page);

#endif // PMM_H