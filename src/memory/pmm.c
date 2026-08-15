// pmm.c - Physical Memory Manager Implementation
#include "pmm.h"
#include "../terminal/terminal.h"
#include "../utility/utility.h"

// Bitmap to track page allocation
static uint32_t* page_bitmap = NULL;
static uint32_t total_pages = 0;
static uint32_t used_pages = 0;
static uint32_t bitmap_size = 0;

// Memory statistics
static uint32_t total_memory = 0;

// Bitmap operations
#define BITMAP_SET(bitmap, bit) ((bitmap)[(bit) / 32] |= (1 << ((bit) % 32)))
#define BITMAP_CLEAR(bitmap, bit) ((bitmap)[(bit) / 32] &= ~(1 << ((bit) % 32)))
#define BITMAP_TEST(bitmap, bit) ((bitmap)[(bit) / 32] & (1 << ((bit) % 32)))
static uint32_t bitmap_start_page = 0;

static uint32_t bitmap_end_page = 0;

// Initialize the physical memory manager
void pmm_init(uint32_t mem_size) {
    total_memory = mem_size;
    total_pages = mem_size / PAGE_SIZE;
    
    // Calculate bitmap size (1 bit per page)
    bitmap_start_page = ADDR_TO_PAGE((uint32_t)page_bitmap);

    bitmap_end_page = ADDR_TO_PAGE((uint32_t)page_bitmap + (bitmap_size * 4) - 1);

    
    // Mark all pages as used initially
    for (uint32_t i = 0; i < bitmap_size; i++) {
        page_bitmap[i] = 0xFFFFFFFF;
    }
    used_pages = total_pages;
    
    print("PMM: Initialized with ");
    print_capacity(mem_size);
    print(" of memory\n");
    
    printr("PMM: Total pages: %d\n", total_pages);
    printr("PMM: Bitmap at: 0x%x (size: %d bytes)\n", page_bitmap, bitmap_size * 4);
}

// Initialize a memory region as available
void pmm_init_region(uint32_t base, uint32_t length) {
    uint32_t start_page = ADDR_TO_PAGE(PAGE_ALIGN(base));
    uint32_t end_page = ADDR_TO_PAGE(PAGE_ALIGN_DOWN(base + length));
    
    for (uint32_t page = start_page; page < end_page; page++) {

    if (page >= bitmap_start_page && page <= bitmap_end_page) {

        continue;  // Skip bitmap pages to prevent freeing them

    }

    if (page < total_pages && BITMAP_TEST(page_bitmap, page)) {

        BITMAP_CLEAR(page_bitmap, page);

        used_pages--;

    }

}
    
    printr("PMM: Initialized region 0x%x - 0x%x (%d pages)\n", 
           base, base + length, end_page - start_page);
}

// Mark a memory region as unavailable
void pmm_deinit_region(uint32_t base, uint32_t length) {
    uint32_t start_page = ADDR_TO_PAGE(PAGE_ALIGN_DOWN(base));
    uint32_t end_page = ADDR_TO_PAGE(PAGE_ALIGN(base + length));
    
    for (uint32_t page = start_page; page < end_page; page++) {
        if (page < total_pages && !BITMAP_TEST(page_bitmap, page)) {
            BITMAP_SET(page_bitmap, page);
            used_pages++;
        }
    }
    
    printr("PMM: Reserved region 0x%x - 0x%x (%d pages)\n", 
           base, base + length, end_page - start_page);
}

// pmm.c - Better approach

// Allocate a single physical page (no zeroing)
void* pmm_alloc_page(void) {
    // Find first free page
    for (uint32_t i = 0; i < total_pages; i++) {
        if (!BITMAP_TEST(page_bitmap, i)) {
            BITMAP_SET(page_bitmap, i);
            used_pages++;
            
            void* addr = (void*)PAGE_TO_ADDR(i);
            return addr;
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
                
                void* addr = (void*)PAGE_TO_ADDR(start_page);
                return addr;
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
    
    uint32_t page = ADDR_TO_PAGE((uint32_t)addr);
    
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
    
    uint32_t start_page = ADDR_TO_PAGE((uint32_t)addr);
    
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
uint32_t pmm_get_total_memory(void) {
    return total_memory;
}

// Get used memory in bytes
uint32_t pmm_get_used_memory(void) {
    return used_pages * PAGE_SIZE;
}

// Get free memory in bytes
uint32_t pmm_get_free_memory(void) {
    return (total_pages - used_pages) * PAGE_SIZE;
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