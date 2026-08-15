// Avfs.c - Simple RAM filesystem with directory support

#include <stdint.h>
#include "../terminal/terminal.h"
#include "../utility/utility.h"
#include "Avfs.h"
#define AVFS_NUM_BLOCKS   32768     /* 32768 * 512 = 16MB storage  */
#define AVFS_MAX_FILES    512
#define AVFS_BLOCK_SIZE   512

#define AVFS_DATA_SIZE  (AVFS_BLOCK_SIZE * AVFS_NUM_BLOCKS)   // ← AFTER the constants
#define AVFS_PATH_MAX     512
typedef enum {
    AVFS_TYPE_FILE = 0,
    AVFS_TYPE_DIR = 1
} avfs_entry_type_t;

typedef struct {
    char name[AVFS_FILENAME_MAX];
    uint32_t size;
    uint32_t start_block;
    uint8_t used;
    avfs_entry_type_t type;
    int parent_index;  // -1 for root entries
} avfs_file_entry_t;

typedef struct {
    uint8_t data[AVFS_BLOCK_SIZE * AVFS_NUM_BLOCKS];
    avfs_file_entry_t files[AVFS_MAX_FILES];
    uint8_t block_bitmap[AVFS_NUM_BLOCKS]; // 0 = free, 1 = used
    char current_dir[AVFS_PATH_MAX];  // Current working directory
} avfs_t;

static avfs_t avfs;

// ===== HELPER FUNCTIONS =====
const char* avfs_get_cwd(void) {
    return avfs.current_dir;
}
// Normalize path (remove .., ., trailing slashes)
static void normalize_path(const char* path, char* output) {
    char temp[AVFS_PATH_MAX];
    strncpy(temp, path, AVFS_PATH_MAX - 1);
    temp[AVFS_PATH_MAX - 1] = '\0';

    // Handle empty path
    if (temp[0] == '\0') {
        strcpy(output, "/");
        return;
    }

    // Start from root or current directory
    char result[AVFS_PATH_MAX] = {0};
    if (temp[0] == '/') {
        strcpy(result, "/");
    } else {
        strcpy(result, avfs.current_dir);
    }

    // We'll collect path components in a flat buffer to avoid dangling pointers
    // after strtok calls on temp (which modifies in-place).
    char parts_buf[AVFS_PATH_MAX];
    char* parts[64];
    int part_count = 0;

    // Split current result into parts first
    if (strcmp(result, "/") != 0) {
        strncpy(parts_buf, result + 1, AVFS_PATH_MAX - 1); // skip leading /
        parts_buf[AVFS_PATH_MAX - 1] = '\0';
        char* p = strtok(parts_buf, "/");
        while (p && part_count < 64) {
            parts[part_count++] = p;
            p = strtok(NULL, "/");
        }
    }

    // Process new path components from temp
    // Note: temp is already a separate copy so strtok here is safe
    char* token = strtok(temp, "/");
    while (token && part_count < 64) {
        if (strcmp(token, ".") == 0) {
            // Current directory - do nothing
        } else if (strcmp(token, "..") == 0) {
            if (part_count > 0) {
                part_count--;
            }
        } else {
            parts[part_count++] = token;
        }
        token = strtok(NULL, "/");
    }

    // Rebuild path
int current_len = 0;
if (part_count == 0) {
    strcpy(output, "/");
    current_len = 1;
} else {
    strcpy(output, "/");
    current_len = 1;
    for (int i = 0; i < part_count; i++) {
        int part_len = strlen(parts[i]);
        if (current_len + 1 + part_len >= AVFS_PATH_MAX) {
            break; // Buffer full
        }
        if (i > 0) {
            strcat(output, "/");
            current_len++;
        }
        strcat(output, parts[i]);
        current_len += part_len;
    }
}
}

// Get parent directory of a path
static void get_parent_path(const char* path, char* parent) {
    char normalized[AVFS_PATH_MAX];
    normalize_path(path, normalized);

    if (strcmp(normalized, "/") == 0) {
        strcpy(parent, "/");
        return;
    }

    strcpy(parent, normalized);
    char* last_slash = strrchr(parent, '/');
    if (last_slash && last_slash != parent) {
        *last_slash = '\0';
    } else {
        strcpy(parent, "/");
    }
}

// Get filename from path (last component)
static void get_filename(const char* path, char* filename) {
    char normalized[AVFS_PATH_MAX];
    normalize_path(path, normalized);

    if (strcmp(normalized, "/") == 0) {
        strcpy(filename, "/");
        return;
    }

    char* last_slash = strrchr(normalized, '/');
    if (last_slash) {
        strcpy(filename, last_slash + 1);
    } else {
        strcpy(filename, normalized);
    }
}

// ===== CORE FUNCTIONS =====

void avfs_init() {
    memset(&avfs, 0, sizeof(avfs));
    strcpy(avfs.current_dir, "/");

    // Create root directory entry
    avfs.files[0].used = 1;
    avfs.files[0].type = AVFS_TYPE_DIR;
    strcpy(avfs.files[0].name, "/");
    avfs.files[0].parent_index = -1;
    avfs.files[0].size = 0;
    avfs.files[0].start_block = 0;
}

// Returns the index of the first block in a run of `count` free blocks,
// or -1 if no such run exists.
static int avfs_find_free_blocks(int count) {
    if (count <= 0) return -1;  // FIX: guard zero/negative count

    int consecutive = 0;
    for (int i = 0; i < AVFS_NUM_BLOCKS; i++) {
        if (avfs.block_bitmap[i] == 0) {
            consecutive++;
            if (consecutive == count) {
                return i - count + 1;
            }
        } else {
            consecutive = 0;
        }
    }
    return -1; // no space
}

// Find entry by full path
static int avfs_find_entry(const char* path) {
    char normalized[AVFS_PATH_MAX];
    normalize_path(path, normalized);

    // Root directory
    if (strcmp(normalized, "/") == 0) {
        return 0;
    }

    // Parse path components
    char path_copy[AVFS_PATH_MAX];
    strcpy(path_copy, normalized + 1);  // Skip leading /

    char* components[64];
    int component_count = 0;

    char* token = strtok(path_copy, "/");
    while (token && component_count < 64) {
        components[component_count++] = token;
        token = strtok(NULL, "/");
    }

    // Traverse from root
    int current_parent = 0;

    for (int i = 0; i < component_count; i++) {
        bool found = false;

        for (int j = 0; j < AVFS_MAX_FILES; j++) {
            if (avfs.files[j].used &&
                avfs.files[j].parent_index == current_parent &&
                strcmp(avfs.files[j].name, components[i]) == 0) {

                current_parent = j;
                found = true;
                break;
            }
        }

        if (!found) {
            return -1;
        }
    }

    return current_parent;
}

// Legacy function for backward compatibility
static int avfs_find_file(const char* name) {
    if (strchr(name, '/')) {
        return avfs_find_entry(name);
    }

    char full_path[AVFS_PATH_MAX];
    if (strcmp(avfs.current_dir, "/") == 0) {
        snprintf(full_path, AVFS_PATH_MAX, "/%s", name);
    } else {
        snprintf(full_path, AVFS_PATH_MAX, "%s/%s", avfs.current_dir, name);
    }

    return avfs_find_entry(full_path);
}

// Returns the size of the file with given name, or -1 if not found
int avfs_get_filesize(const char* name) {
    int file_index = avfs_find_file(name);
    if (file_index == -1) {
        return -1;
    }

    if (avfs.files[file_index].type == AVFS_TYPE_DIR) {
        return -1;
    }

    return (int)avfs.files[file_index].size;
}

// Create directory
int avfs_create_dir(const char* path) {
    char normalized[AVFS_PATH_MAX];
    normalize_path(path, normalized);

    if (avfs_find_entry(normalized) != -1) {
        return -1;  // Already exists
    }

    char parent_path[AVFS_PATH_MAX];
    get_parent_path(normalized, parent_path);

    int parent_index = avfs_find_entry(parent_path);
    if (parent_index == -1) {
        return -2;  // Parent directory doesn't exist
    }

    if (avfs.files[parent_index].type != AVFS_TYPE_DIR) {
        return -3;  // Parent is not a directory
    }

    int entry_index = -1;
    for (int i = 0; i < AVFS_MAX_FILES; i++) {
        if (!avfs.files[i].used) {
            entry_index = i;
            break;
        }
    }

    if (entry_index == -1) {
        return -4;  // No free entries
    }

    char dir_name[AVFS_FILENAME_MAX];
    get_filename(normalized, dir_name);

    avfs_file_entry_t* dir = &avfs.files[entry_index];
    strncpy(dir->name, dir_name, AVFS_FILENAME_MAX - 1);
    dir->name[AVFS_FILENAME_MAX - 1] = '\0';
    dir->type = AVFS_TYPE_DIR;
    dir->parent_index = parent_index;
    dir->used = 1;
    dir->size = 0;
    dir->start_block = 0;

    return 0;
}

int avfs_create_file(const char* name, uint32_t size) {
    char normalized[AVFS_PATH_MAX];
    normalize_path(name, normalized);

    if (avfs_find_entry(normalized) != -1) {
        return -1; // file exists
    }

    // Allocate at least 1 block to ensure start_block is always valid
    int blocks_needed = (size + AVFS_BLOCK_SIZE - 1) / AVFS_BLOCK_SIZE;
    if (blocks_needed == 0) blocks_needed = 1;

    int start_block = avfs_find_free_blocks(blocks_needed);
    if (start_block < 0) {
        return -2; // no space
    }

    char parent_path[AVFS_PATH_MAX];
    get_parent_path(normalized, parent_path);

    int parent_index = avfs_find_entry(parent_path);
    if (parent_index == -1) {
        return -5;  // Parent directory doesn't exist
    }

    int file_index = -1;
    for (int i = 0; i < AVFS_MAX_FILES; i++) {
        if (!avfs.files[i].used) {
            file_index = i;
            break;
        }
    }
    if (file_index == -1) {
        return -3; // no file entries free
    }

    // Mark blocks as used
    for (int i = start_block; i < start_block + blocks_needed; i++) {
        avfs.block_bitmap[i] = 1;
    }

    char filename[AVFS_FILENAME_MAX];
    get_filename(normalized, filename);

    avfs_file_entry_t* f = &avfs.files[file_index];
    strncpy(f->name, filename, AVFS_FILENAME_MAX - 1);
    f->name[AVFS_FILENAME_MAX - 1] = 0;
    
    // FIX: Apply the minimum-1-block logic to size as well?
    // No, keep 'size' accurate (0), but ensure 'start_block' reflects the allocation.
    f->size = size; 
    
    // FIX: Always set start_block. 
    // Since blocks_needed is >= 1 here, start_block is valid.
    f->start_block = (uint32_t)start_block; 
    
    f->used = 1;
    f->type = AVFS_TYPE_FILE;
    f->parent_index = parent_index;

    return 0;
}

#define MAX_CHECK_BUFFER_SIZE 4096
bool insideFile(const char* name, const char* search_str) {
    if (!name || !search_str) {
        return false;
    }
    uint32_t search_len = strlen(search_str);
    if (search_len == 0) {
        return false;
    }
    int file_size = avfs_get_filesize(name);
    if (file_size == -1) {
        return false;
    }
    if ((uint32_t)file_size < search_len) {
        return false;
    }
    if (file_size > MAX_CHECK_BUFFER_SIZE) {
        return false;
    }
    char buffer[MAX_CHECK_BUFFER_SIZE + 1];
    int read_result = avfs_read_file(name, buffer, file_size, 0);
    if (read_result != 0) {
        return false;
    }
    buffer[file_size] = '\0';
    return (strstr(buffer, search_str) != NULL);
}

int avfs_get_content(const char* name, char* buffer, uint32_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        return -1;
    }

    int file_size = avfs_get_filesize(name);
    if (file_size == -1) {
        return -1;
    }

    if ((uint32_t)file_size >= buffer_size) {
        return -1;
    }

    int read_result = avfs_read_file(name, buffer, file_size, 0);
    if (read_result != 0) {
        return -1;
    }

    buffer[file_size] = '\0';
    return 0;
}

bool avfs_file_exists(const char* name) {
    return avfs_find_file(name) != -1;
}

bool avfs_is_directory(const char* path) {
    int index = avfs_find_entry(path);
    if (index == -1) {
        return false;
    }
    return avfs.files[index].type == AVFS_TYPE_DIR;
}



int avfs_write_file(const char* name, const void* buffer, uint32_t size, uint32_t offset) {
    int file_index = avfs_find_file(name);
    if (file_index == -1) return -1;

    avfs_file_entry_t* f = &avfs.files[file_index];
    if (f->type != AVFS_TYPE_FILE) return -3;
    
    // Check 1: Ensure the requested write fits within the file's declared size
    if (offset + size > f->size) return -2;

    uint32_t start_addr = f->start_block * AVFS_BLOCK_SIZE + offset;
    
    // FIX: Check 2: Ensure the write operation does not overflow the global data buffer
    // This prevents the crash if the file metadata says the file is big, but the buffer is small.
    if (start_addr + size > AVFS_DATA_SIZE) {
        return -4; // New error code for buffer overflow
    }

    memcpy(&avfs.data[start_addr], buffer, size);
    return 0;
}

int avfs_read_file(const char* name, void* buffer, uint32_t size, uint32_t offset) {
    int file_index = avfs_find_file(name);
    if (file_index == -1) return -1;

    avfs_file_entry_t* f = &avfs.files[file_index];
    if (f->type != AVFS_TYPE_FILE) return -3;
    if (offset + size > f->size) return -2;

    uint32_t start_addr = f->start_block * AVFS_BLOCK_SIZE + offset;
    memcpy(buffer, &avfs.data[start_addr], size);
    return 0;
}

void avfs_list_files() {
    int current_dir_index = avfs_find_entry(avfs.current_dir);

    printr("Directory: %s\n", avfs.current_dir);

    for (int i = 0; i < AVFS_MAX_FILES; i++) {
        if (avfs.files[i].used &&
            avfs.files[i].parent_index == current_dir_index &&
            avfs.files[i].name[0] != '.') {

            if (avfs.files[i].type == AVFS_TYPE_DIR) {
                printr(" [DIR]  %s\n", avfs.files[i].name);
            } else {
                printr(" [FILE] %s ", avfs.files[i].name);
                printr("(size: ");
                print_capacity(avfs.files[i].size);
                printr(")\n");
            }
        }
    }
}

// List files in specific directory
void avfs_list_dir(const char* path) {
    char normalized[AVFS_PATH_MAX];
    normalize_path(path, normalized);

    int dir_index = avfs_find_entry(normalized);
    if (dir_index == -1) {
        printr("Directory not found: %s\n", path);
        return;
    }

    if (avfs.files[dir_index].type != AVFS_TYPE_DIR) {
        printr("Not a directory: %s\n", path);
        return;
    }

    printr("Directory: %s\n", normalized);

    for (int i = 0; i < AVFS_MAX_FILES; i++) {
        if (avfs.files[i].used &&
            avfs.files[i].parent_index == dir_index &&
            avfs.files[i].name[0] != '.') {

            if (avfs.files[i].type == AVFS_TYPE_DIR) {
                printr(" [DIR]  %s\n", avfs.files[i].name);
            } else {
                printr(" [FILE] %s ", avfs.files[i].name);
                printr("(size: ");
                print_capacity(avfs.files[i].size);
                printr(")\n");
            }
        }
    }
}

int avfs_remove_file(const char* name) {
    int file_index = avfs_find_file(name);
    if (file_index == -1) {
        return -1;
    }

    avfs_file_entry_t* f = &avfs.files[file_index];

    if (f->type == AVFS_TYPE_DIR) {
        for (int i = 0; i < AVFS_MAX_FILES; i++) {
            if (avfs.files[i].used && avfs.files[i].parent_index == file_index) {
                return -2;  // Directory not empty
            }
        }
    } else {
        // FIX: use same minimum-1-block logic as create so we free the right count
        int blocks_used = (f->size + AVFS_BLOCK_SIZE - 1) / AVFS_BLOCK_SIZE;
        if (blocks_used == 0) blocks_used = 1;
        for (int i = (int)f->start_block; i < (int)f->start_block + blocks_used; i++) {
            avfs.block_bitmap[i] = 0;
        }
    }

    memset(f, 0, sizeof(avfs_file_entry_t));
    return 0;
}

// Remove directory recursively
int avfs_remove_dir_recursive(const char* path) {
    char normalized[AVFS_PATH_MAX];
    normalize_path(path, normalized);

    int dir_index = avfs_find_entry(normalized);
    if (dir_index == -1) {
        return -1;
    }

    if (avfs.files[dir_index].type != AVFS_TYPE_DIR) {
        return -2;
    }

    for (int i = 0; i < AVFS_MAX_FILES; i++) {
        if (avfs.files[i].used && avfs.files[i].parent_index == dir_index) {
            if (avfs.files[i].type == AVFS_TYPE_DIR) {
                char child_path[AVFS_PATH_MAX];
                if (strcmp(normalized, "/") == 0) {
                    snprintf(child_path, AVFS_PATH_MAX, "/%s", avfs.files[i].name);
                } else {
                    snprintf(child_path, AVFS_PATH_MAX, "%s/%s", normalized, avfs.files[i].name);
                }
                avfs_remove_dir_recursive(child_path);
            } else {
                // FIX: use full path, not bare name, so avfs_find_file locates it correctly
                char child_path[AVFS_PATH_MAX];
                if (strcmp(normalized, "/") == 0) {
                    snprintf(child_path, AVFS_PATH_MAX, "/%s", avfs.files[i].name);
                } else {
                    snprintf(child_path, AVFS_PATH_MAX, "%s/%s", normalized, avfs.files[i].name);
                }
                avfs_remove_file(child_path);
            }
        }
    }

    memset(&avfs.files[dir_index], 0, sizeof(avfs_file_entry_t));
    return 0;
}

// FIX: avfs_append_file now reallocates blocks when the new content exceeds
// the originally allocated capacity, instead of writing out-of-bounds.
int avfs_append_file(const char* name, const void* buffer, uint32_t size) {
    int file_index = avfs_find_file(name);
    if (file_index == -1) return -1;

    avfs_file_entry_t* f = &avfs.files[file_index];
    if (f->type != AVFS_TYPE_FILE) return -3;

    uint32_t old_size  = f->size;
    uint32_t new_size  = old_size + size;

    // Use the same minimum-1-block rule as create/remove
    int old_blocks = (old_size + AVFS_BLOCK_SIZE - 1) / AVFS_BLOCK_SIZE;
    int new_blocks = (new_size + AVFS_BLOCK_SIZE - 1) / AVFS_BLOCK_SIZE;
    if (old_blocks == 0) old_blocks = 1;
    if (new_blocks == 0) new_blocks = 1;

    if (new_blocks > old_blocks) {
        // Need more blocks — find a new contiguous run and relocate
        for (int i = (int)f->start_block; i < (int)f->start_block + old_blocks; i++)
            avfs.block_bitmap[i] = 0;

        int new_start = avfs_find_free_blocks(new_blocks);
        if (new_start < 0) {
            // Re-mark old blocks; can't grow
            for (int i = (int)f->start_block; i < (int)f->start_block + old_blocks; i++)
                avfs.block_bitmap[i] = 1;
            return -2;
        }

        // Copy existing data to the new location
        memcpy(&avfs.data[new_start * AVFS_BLOCK_SIZE],
               &avfs.data[f->start_block * AVFS_BLOCK_SIZE],
               old_size);

        for (int i = new_start; i < new_start + new_blocks; i++)
            avfs.block_bitmap[i] = 1;

        f->start_block = (uint32_t)new_start;
    }

    // Append new data immediately after existing content
    memcpy(&avfs.data[f->start_block * AVFS_BLOCK_SIZE + old_size], buffer, size);
    f->size = new_size;

    return 0;
}

int avfs_get_file_info(int index, char* name_out, uint32_t* size_out) {
    if (index < 0 || index >= AVFS_MAX_FILES) return -1;
    if (!avfs.files[index].used) return -1;

    if (name_out) {
        /* build full path using existing avfs_get_full_path */
        avfs_get_full_path(index, name_out);
    }

    if (size_out) {
        *size_out = avfs.files[index].size;
    }

    return 0;
}

// Change current directory
int avfs_chdir(const char* path) {
    char normalized[AVFS_PATH_MAX];
    normalize_path(path, normalized);

    int dir_index = avfs_find_entry(normalized);
    if (dir_index == -1) {
        return -1;
    }

    if (avfs.files[dir_index].type != AVFS_TYPE_DIR) {
        return -2;
    }

    strcpy(avfs.current_dir, normalized);
    return 0;
}

// Get current directory
const char* avfs_getcwd(void) {
    return avfs.current_dir;
}

int avfs_get_full_path(int index, char* path_out) {
    if (index < 0 || index >= AVFS_MAX_FILES || !avfs.files[index].used) {
        return -1;
    }

    /* root directory */
    if (index == 0) {
        strcpy(path_out, "/");
        return 0;
    }

    char components[32][AVFS_FILENAME_MAX];
    int  component_count = 0;

    int current = index;
    while (current > 0 && current < AVFS_MAX_FILES) {
        if (component_count >= 32) break;
        strcpy(components[component_count++], avfs.files[current].name);
        current = avfs.files[current].parent_index;
    }

    /* rebuild: root slash + components in reverse */
    strcpy(path_out, "/");
    for (int i = component_count - 1; i >= 0; i--) {
        strcat(path_out, components[i]);
        if (i > 0) strcat(path_out, "/");  /* slash between components */
    }

    return 0;
}

// Get information about a file or directory entry
int avfs_get_entry_info(const char* path, DirectoryEntry* entry_out) {
    if (!entry_out) {
        return -1;
    }

    char normalized[AVFS_PATH_MAX];
    normalize_path(path, normalized);

    int index = avfs_find_entry(normalized);
    if (index == -1) {
        return -1;
    }

    avfs_file_entry_t* file = &avfs.files[index];

    strncpy(entry_out->name, file->name, AVFS_FILENAME_MAX - 1);
    entry_out->name[AVFS_FILENAME_MAX - 1] = '\0';

    entry_out->size = file->size;
    entry_out->is_directory = (file->type == AVFS_TYPE_DIR) ? 1 : 0;
    entry_out->index = index;

    return 0;
}

// List directory contents into an array
int avfs_list_directory(const char* path, DirectoryEntry* entries, int max_entries) {
    if (!entries || max_entries <= 0) {
        return -1;
    }

    char normalized[AVFS_PATH_MAX];
    normalize_path(path, normalized);

    int dir_index = avfs_find_entry(normalized);
    if (dir_index == -1) {
        return -1;
    }

    if (avfs.files[dir_index].type != AVFS_TYPE_DIR) {
        return -2;
    }

    int count = 0;

    // Add . and .. entries
    if (count < max_entries) {
        strcpy(entries[count].name, ".");
        entries[count].size = 0;
        entries[count].is_directory = 1;
        entries[count].index = dir_index;
        count++;
    }

    if (count < max_entries && dir_index != 0) {
        strcpy(entries[count].name, "..");
        entries[count].size = 0;
        entries[count].is_directory = 1;
        entries[count].index = avfs.files[dir_index].parent_index;
        count++;
    }

    for (int i = 0; i < AVFS_MAX_FILES && count < max_entries; i++) {
        if (avfs.files[i].used && avfs.files[i].parent_index == dir_index) {
            strncpy(entries[count].name, avfs.files[i].name, 31);
            entries[count].name[31] = '\0';
            entries[count].size = avfs.files[i].size;
            entries[count].is_directory = (avfs.files[i].type == AVFS_TYPE_DIR) ? 1 : 0;
            entries[count].index = i;
            count++;
        }
    }

    return count;
}

int avfs_truncate(const char* name, int newsize) {
    if (newsize < 0) return -5;

    int file_index = avfs_find_file(name);
    if (file_index == -1) return -1;

    avfs_file_entry_t* f = &avfs.files[file_index];
    if (f->type != AVFS_TYPE_FILE) return -3;

    uint32_t old_size = f->size;
    uint32_t new_size = (uint32_t)newsize;

    if (new_size == old_size) return 0;

    int old_blocks = (old_size + AVFS_BLOCK_SIZE - 1) / AVFS_BLOCK_SIZE;
    int new_blocks = (new_size + AVFS_BLOCK_SIZE - 1) / AVFS_BLOCK_SIZE;
    if (old_blocks == 0) old_blocks = 1;
    if (new_blocks == 0) new_blocks = 1;

    if (new_blocks < old_blocks) {
        // Shrinking - free the trailing blocks we no longer need
        for (int i = (int)f->start_block + new_blocks; i < (int)f->start_block + old_blocks; i++)
            avfs.block_bitmap[i] = 0;
    } else if (new_blocks > old_blocks) {
        // Growing - try to extend in place first
        int can_extend_in_place = 1;
        for (int i = (int)f->start_block + old_blocks; i < (int)f->start_block + new_blocks; i++) {
            if (i >= AVFS_NUM_BLOCKS || avfs.block_bitmap[i] != 0) { can_extend_in_place = 0; break; }
        }

        if (can_extend_in_place) {
            for (int i = (int)f->start_block + old_blocks; i < (int)f->start_block + new_blocks; i++)
                avfs.block_bitmap[i] = 1;
        } else {
            // Relocate: free old run, find a new contiguous run, copy data over
            for (int i = (int)f->start_block; i < (int)f->start_block + old_blocks; i++)
                avfs.block_bitmap[i] = 0;

            int new_start = avfs_find_free_blocks(new_blocks);
            if (new_start < 0) {
                // Can't grow - restore old blocks and fail
                for (int i = (int)f->start_block; i < (int)f->start_block + old_blocks; i++)
                    avfs.block_bitmap[i] = 1;
                return -2;
            }

            memcpy(&avfs.data[new_start * AVFS_BLOCK_SIZE],
                   &avfs.data[f->start_block * AVFS_BLOCK_SIZE],
                   old_size);

            for (int i = new_start; i < new_start + new_blocks; i++)
                avfs.block_bitmap[i] = 1;

            f->start_block = (uint32_t)new_start;
        }

        // Zero-fill the newly grown region
        memset(&avfs.data[f->start_block * AVFS_BLOCK_SIZE + old_size], 0, new_size - old_size);
    }

    f->size = new_size;
    return 0;
}