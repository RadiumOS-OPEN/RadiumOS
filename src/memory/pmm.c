// pmm.c - Physical Memory Manager Implementation
#include "pmm.h"
#include "../terminal/terminal.h"
#include "../utility/utility.h"

#define PMM_MAX_PHYSICAL_ADDRESS 0x140000000ULL
#define PMM_IDENTITY_LIMIT       0x100000000ULL
#define PMM_HIGH_VIRTUAL_BASE    0xC0000000u
#define PMM_MAX_PAGES ((uint32_t)(PMM_MAX_PHYSICAL_ADDRESS / PAGE_SIZE))

// 1 bit per 4-KiB frame, covering 4 GiB of RAM including the QEMU remap.
static uint32_t page_bitmap[PMM_MAX_PAGES / 32];
static uint32_t total_pages = 0;
static uint32_t used_pages = 0;
static uint32_t bitmap_size = 0;

// Memory statistics
static uint64_t total_memory = 0;

// Bitmap operations
#define BITMAP_SET(bitmap, bit) ((bitmap)[(bit) / 32] |= (1u << ((bit) % 32)))
#define BITMAP_CLEAR(bitmap, bit) ((bitmap)[(bit) / 32] &= ~(1u << ((bit) % 32)))
#define BITMAP_TEST(bitmap, bit) ((bitmap)[(bit) / 32] & (1u << ((bit) % 32)))

static void* frame_to_pointer(uint32_t page) {
    uint64_t physical = (uint64_t)page * PAGE_SIZE;
    if (physical >= PMM_IDENTITY_LIMIT) {
        physical = PMM_HIGH_VIRTUAL_BASE + (physical - PMM_IDENTITY_LIMIT);
    }
    return (void*)(uintptr_t)physical;
}

static uint32_t pointer_to_frame(const void* pointer) {
    uint64_t address = (uintptr_t)pointer;
    if (address >= PMM_HIGH_VIRTUAL_BASE) {
        address = PMM_IDENTITY_LIMIT + (address - PMM_HIGH_VIRTUAL_BASE);
    }
    return (uint32_t)(address / PAGE_SIZE);
}

// Initialize the physical memory manager
void pmm_init(uint64_t physical_limit) {
    if (physical_limit > PMM_MAX_PHYSICAL_ADDRESS) {
        physical_limit = PMM_MAX_PHYSICAL_ADDRESS;
    }
    total_pages = (uint32_t)((physical_limit + PAGE_SIZE - 1) / PAGE_SIZE);
    bitmap_size = (total_pages + 31) / 32;

    // Treat frames as reserved until an available memory-map range frees them.
    for (uint32_t i = 0; i < bitmap_size; i++) {
        page_bitmap[i] = 0xFFFFFFFF;
    }
    if (total_pages % 32 != 0 && bitmap_size != 0) {
        page_bitmap[bitmap_size - 1] = (1u << (total_pages % 32)) - 1u;
    }
    used_pages = total_pages;
    total_memory = 0;

    print("PMM: Initialized frame map for ");
    printr("%d pages below 5 GiB\n", total_pages);
}

// Initialize a memory region as available
void pmm_init_region(uint64_t base, uint64_t length) {
    if (length == 0 || base >= PMM_MAX_PHYSICAL_ADDRESS) return;
    uint64_t end = base + length;
    if (end < base || end > PMM_MAX_PHYSICAL_ADDRESS) {
        end = PMM_MAX_PHYSICAL_ADDRESS;
    }
    uint32_t start_page = (uint32_t)((base + PAGE_SIZE - 1) / PAGE_SIZE);
    uint32_t end_page = (uint32_t)(end / PAGE_SIZE);

    for (uint32_t page = start_page; page < end_page; page++) {
        if (page < total_pages && BITMAP_TEST(page_bitmap, page)) {
            BITMAP_CLEAR(page_bitmap, page);
            used_pages--;
            total_memory += PAGE_SIZE;
        }
    }
}

// Mark a memory region as unavailable
void pmm_deinit_region(uint64_t base, uint64_t length) {
    if (length == 0 || base >= PMM_MAX_PHYSICAL_ADDRESS) return;
    uint64_t end = base + length;
    if (end < base || end > PMM_MAX_PHYSICAL_ADDRESS) {
        end = PMM_MAX_PHYSICAL_ADDRESS;
    }
    uint32_t start_page = (uint32_t)(base / PAGE_SIZE);
    uint32_t end_page = (uint32_t)((end + PAGE_SIZE - 1) / PAGE_SIZE);

    for (uint32_t page = start_page; page < end_page; page++) {
        if (page < total_pages && !BITMAP_TEST(page_bitmap, page)) {
            BITMAP_SET(page_bitmap, page);
            used_pages++;
            total_memory -= PAGE_SIZE;
        }
    }
}

// pmm.c - Better approach

// Allocate a single physical page (no zeroing)
void* pmm_alloc_page(void) {
    // Find first free page
    for (uint32_t i = 0; i < total_pages; i++) {
        if (!BITMAP_TEST(page_bitmap, i)) {
            BITMAP_SET(page_bitmap, i);
            used_pages++;
            
            return frame_to_pointer(i);
        }
    }
    
    // Out of memory
    print("PMM: ERROR - Out of physical memory!\n");
    return NULL;
}

// Allocate multiple contiguous physical pages (no zeroing)
void* pmm_alloc_pages(uint32_t count) {
    if (count == 0) {
        return NULL;
    }
    
    if (count == 1) {
        return pmm_alloc_page();
    }
    
    // Find contiguous free pages
    uint32_t found = 0;
    uint32_t start_page = 0;
    
    for (uint32_t i = 0; i < total_pages; i++) {
        if (!BITMAP_TEST(page_bitmap, i)) {
            if (found == 0) {
                start_page = i;
            }
            found++;
            
            if (found == count) {
                // Found enough contiguous pages
                for (uint32_t j = 0; j < count; j++) {
                    BITMAP_SET(page_bitmap, start_page + j);
                }
                used_pages += count;
                
                return frame_to_pointer(start_page);
            }
        } else {
            found = 0;
        }
    }
    
    // Could not find enough contiguous pages
    printr("PMM: ERROR - Could not allocate %d contiguous pages!\n", count);
    printr("PMM: Free pages: %d, Total pages: %d\n", total_pages - used_pages, total_pages);
    return NULL;
}

// Allocate and zero a page
void* pmm_alloc_page_zero(void) {
    void* page = pmm_alloc_page();
    if (page) {
        memset(page, 0, PAGE_SIZE);
    }
    return page;
}

// Allocate and zero multiple pages
void* pmm_alloc_pages_zero(uint32_t count) {
    void* pages = pmm_alloc_pages(count);
    if (pages) {
        memset(pages, 0, PAGE_SIZE * count);
    }
    return pages;
}
// Free a single physical page
void pmm_free_page(void* addr) {
    if (!addr) {
        return;
    }
    
    uint32_t page = pointer_to_frame(addr);
    
    if (page >= total_pages) {
        printr("PMM: ERROR - Invalid page address: 0x%x\n", addr);
        return;
    }
    
    if (!BITMAP_TEST(page_bitmap, page)) {
        printr("PMM: WARNING - Double free of page: 0x%x\n", addr);
        return;
    }
    
    BITMAP_CLEAR(page_bitmap, page);
    used_pages--;
}

// Free multiple contiguous physical pages
void pmm_free_pages(void* addr, uint32_t count) {
    if (!addr || count == 0) {
        return;
    }
    
    uint32_t start_page = pointer_to_frame(addr);
    
    for (uint32_t i = 0; i < count; i++) {
        uint32_t page = start_page + i;
        
        if (page >= total_pages) {
            break;
        }
        
        if (BITMAP_TEST(page_bitmap, page)) {
            BITMAP_CLEAR(page_bitmap, page);
            used_pages--;
        }
    }
}

// Mark a page as used
void pmm_mark_page_used(uint32_t page) {
    if (page < total_pages && !BITMAP_TEST(page_bitmap, page)) {
        BITMAP_SET(page_bitmap, page);
        used_pages++;
    }
}

// Mark a page as free
void pmm_mark_page_free(uint32_t page) {
    if (page < total_pages && BITMAP_TEST(page_bitmap, page)) {
        BITMAP_CLEAR(page_bitmap, page);
        used_pages--;
    }
}

// Check if a page is free
bool pmm_is_page_free(uint32_t page) {
    if (page >= total_pages) {
        return false;
    }
    return !BITMAP_TEST(page_bitmap, page);
}

// Get total memory in bytes
uint64_t pmm_get_total_memory(void) {
    return total_memory;
}

// Get used memory in bytes
uint64_t pmm_get_used_memory(void) {
    return (uint64_t)used_pages * PAGE_SIZE;
}

// Get free memory in bytes
uint64_t pmm_get_free_memory(void) {
    return (uint64_t)(total_pages - used_pages) * PAGE_SIZE;
}

// Get total pages
uint32_t pmm_get_total_pages(void) {
    return total_pages;
}

// Get used pages
uint32_t pmm_get_used_pages(void) {
    return used_pages;
}

// Get free pages
uint32_t pmm_get_free_pages(void) {
    return total_pages - used_pages;
}