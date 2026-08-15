#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Memory pool configuration
#define MEMORY_POOL_SIZE (1024 * 1024) // 1MB memory pool

// Page size constants
#define PAGE_SIZE 4096
#define PAGE_ALIGN(addr) (((addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

// Memory type definitions
#define MEMORY_TYPE_AVAILABLE 1
#define MEMORY_TYPE_RESERVED 2
#define MEMORY_TYPE_ACPI_RECLAIMABLE 3
#define MEMORY_TYPE_ACPI_NVS 4
#define MEMORY_TYPE_BAD 5

// Memory map configuration
#define MEMORY_MAP_MAX_ENTRIES 32

// Memory block structure for allocation tracking
typedef struct Block {
    size_t size;
    struct Block* next;
} Block;

// Memory map entry structure
typedef struct {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
} MemoryMapEntry;

// Memory map structure
typedef struct {
    MemoryMapEntry entries[MEMORY_MAP_MAX_ENTRIES];
    int entry_count;
} MemoryMap;

// Memory information structure
typedef struct {
    uint64_t total_memory;      // Total physical RAM
    uint64_t used_memory;       // Used physical RAM
    size_t pool_total;          // Memory pool total size
    size_t pool_used;           // Memory pool used size
    uint32_t total_pages;       // Total pages available
    uint32_t allocated_pages;   // Currently allocated pages
} MemoryInfo;

// Core memory management functions
void memory_init(void);
void* memory_alloc(size_t size);
void memory_free(void* ptr);
MemoryInfo get_memory_info(void);
void print_memory_info(void);

// Physical memory detection and management
void init_physical_memory(void);
int detect_memory_e820(MemoryMap* memory_map);
uint64_t detect_memory_cmos(void);
void set_memory_map_from_bootloader(MemoryMap* map);
void build_cmos_memory_map(MemoryMap* map, uint64_t total_mem);

// Page allocation functions
uint32_t allocate_physical_page(void);
void free_physical_page(uint32_t physical_addr);
uint32_t allocate_contiguous_pages(uint32_t num_pages);
void free_contiguous_pages(uint32_t physical_addr, uint32_t num_pages);

// Page tracking functions
void set_page_allocated(uint32_t page_number);
void set_page_free(uint32_t page_number);
bool is_page_allocated(uint32_t page_number);

// Virtual memory management
void* map_physical_memory(uint32_t phys_addr, uint32_t size);
void unmap_memory(void* virt_addr, uint32_t size);
int map_page(uint32_t virtual_addr, uint32_t physical_addr, uint32_t flags);
void unmap_page(uint32_t virtual_addr);
uint32_t virtual_to_physical(void* virt_addr);
bool is_mapped(void* virt_addr);

// Paging control
void enable_paging(void);

// Memory statistics
void get_memory_stats(uint32_t* total_kb, uint32_t* used_kb, uint32_t* free_kb);

#endif // MEMORY_H