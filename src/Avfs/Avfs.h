#ifndef AVFS_H
#define AVFS_H

#include <stdint.h>
#include <stdbool.h>

// Directory entry structure for listing
typedef struct {
    char name[32];          // Entry name
    uint32_t size;          // Size (0 for directories)
    int is_directory;       // 1 if directory, 0 if file
    int index;              // Internal index in filesystem
} DirectoryEntry;
#define AVFS_FILENAME_MAX 128
// Initialize filesystem
void avfs_init(void);

// File operations
int avfs_create_file(const char* name, uint32_t size);
int avfs_write_file(const char* name, const void* buffer, uint32_t size, uint32_t offset);
int avfs_read_file(const char* name, void* buffer, uint32_t size, uint32_t offset);
int avfs_append_file(const char* name, const void* buffer, uint32_t size);
int avfs_remove_file(const char* name);
int avfs_get_filesize(const char* name);
int avfs_get_content(const char* name, char* buffer, uint32_t buffer_size);
int avfs_truncate(const char* name, int newsize);
bool avfs_file_exists(const char* name);
bool insideFile(const char* name, const char* search_str);

// Directory operations
int avfs_create_dir(const char* path);
int avfs_remove_dir_recursive(const char* path);
bool avfs_is_directory(const char* path);
int avfs_chdir(const char* path);
const char* avfs_getcwd(void);
void avfs_list_files(void);
void avfs_list_dir(const char* path);

// Info operations
int avfs_get_file_info(int index, char* name_out, uint32_t* size_out);
int avfs_get_full_path(int index, char* path_out);

// New functions for directory traversal (rm -rf support)
int avfs_get_entry_info(const char* path, DirectoryEntry* entry_out);
int avfs_list_directory(const char* path, DirectoryEntry* entries, int max_entries);
const char* avfs_get_cwd(void);

#endif // AVFS_H