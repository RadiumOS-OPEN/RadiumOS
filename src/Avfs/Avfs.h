// Avfs.h - Virtual filesystem + persistent /disk/ mount
// Author: scp_2801
//
// All functions below transparently handle /disk/* paths via FAT32 on
// fetchdisk.img.  No new public API is needed — existing callers just work.

#ifndef AVFS_H
#define AVFS_H

#include <stdint.h>
#include <stdbool.h>

#define AVFS_FILENAME_MAX  64

// Used by avfs_list_directory / avfs_get_entry_info
typedef struct {
    char     name[32];
    uint32_t size;
    int      is_directory;
    int      index;           // -1 for /disk entries (no AVFS slot)
} DirectoryEntry;

// =====================================================================
// AVFS RAM Filesystem constants
// =====================================================================
#define AVFS_NUM_BLOCKS   32768     /* 32768 * 512 = 16MB storage  */
#define AVFS_MAX_FILES    512
#define AVFS_BLOCK_SIZE   512

#define AVFS_DATA_SIZE  (AVFS_BLOCK_SIZE * AVFS_NUM_BLOCKS)
#define AVFS_PATH_MAX     512

typedef struct {
    bool     mounted;          /* FAT32 backend ready                     */
    uint16_t ata_bus;          /* 0x1F0 primary / 0x170 secondary         */
    uint8_t  ata_drive;        /* 0xE0 master / 0xF0 slave                */
    uint32_t sector_size;      /* 512                                     */
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;     /* sector_size * sectors_per_cluster       */
    uint32_t reserved_sectors; /* from BPB                                */
    uint32_t fat_sectors;      /* per FAT copy, from BPB                  */
    uint32_t data_start_lba;   /* first sector of the data area           */
    uint32_t total_sectors;    /* raw device size (BPB total_sectors_32)  */
    uint32_t total_clusters;   /* addressable data clusters               */
    uint32_t used_clusters;    /* FAT entries != free                     */
    uint32_t free_clusters;
    uint32_t total_bytes;      /* total_clusters * cluster_size           */
    uint32_t used_bytes;
    uint32_t free_bytes;
    uint32_t total_kb;         /* exact sizes in KiB (the *_bytes fields stop at 4 GiB - 1) */
    uint32_t used_kb;
    uint32_t free_kb;
    uint32_t file_count;       /* recursive                               */
    uint32_t dir_count;        /* recursive                               */
    char     volume_label[12]; /* BPB label, trimmed, NUL-terminated      */
} avfs_disk_info_t;

typedef struct {
    bool     ready;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t used_blocks;
    uint32_t total_bytes;
    uint32_t used_bytes;
    uint32_t free_bytes;
    uint32_t file_count;
    uint32_t dir_count;
} avfs_ramfs_info_t;



// ── Lifecycle ────────────────────────────────────────────────────────────────
// avfs_init now also probes the ATA drive and mounts /disk/ automatically.
// No extra call needed in kernel_main.
void avfs_init(void);

// ── Path queries ─────────────────────────────────────────────────────────────
const char* avfs_get_cwd(void);
const char* avfs_getcwd(void);
bool        avfs_file_exists(const char* name);
bool        avfs_is_directory(const char* path);
int         avfs_get_filesize(const char* name);

// ── Directory operations ──────────────────────────────────────────────────────
int  avfs_chdir(const char* path);
int  avfs_create_dir(const char* path);
void avfs_list_files(void);                   // lists current directory
void avfs_list_dir(const char* path);         // lists any path (RAM or /disk)
int  avfs_list_directory(const char* path, DirectoryEntry* entries, int max_entries);
int  avfs_get_entry_info(const char* path, DirectoryEntry* entry_out);
int  avfs_remove_dir_recursive(const char* path);

// ── File operations ───────────────────────────────────────────────────────────
int  avfs_create_file(const char* name, uint32_t size);
int  avfs_save_file(const char* name, const void* buffer, uint32_t size, bool overwrite);
int  avfs_write_file(const char* name, const void* buffer, uint32_t size, uint32_t offset);
int  avfs_read_file(const char* name, void* buffer, uint32_t size, uint32_t offset);
int  avfs_get_content(const char* name, char* buffer, uint32_t buffer_size);
int  avfs_append_file(const char* name, const void* buffer, uint32_t size);
int  avfs_remove_file(const char* name);
int  avfs_truncate(const char* name, int newsize);

// ── Internal helpers (used by shell / file manager) ───────────────────────────
int  avfs_get_file_info(int index, char* name_out, uint32_t* size_out);
int  avfs_get_full_path(int index, char* path_out);
bool insideFile(const char* name, const char* search_str);
int avfs_format_disk(void);

/* Query the FAT32 /disk/ backend.  0 = ok, -1 = bad args, -2 = not mounted. */
int avfs_disk_query(avfs_disk_info_t* out);
/* Cheap (no directory walk): size and use of /disk in KiB. 0 = ok. */
int avfs_disk_space(uint32_t* total_kb, uint32_t* used_kb);

/* Per-cell usage of the FAT data area: 0 free, 1 partial, 2 full. */
int avfs_disk_usage_map(uint8_t* cells, int ncells);

/* Consistency check of the /disk FAT32 volume. repair: fix what is found; check_fat_copies: also compare the
 * two FAT copies with memory (slower); verbose: print every problem. 0 = ran, -2 = not mounted. */
typedef struct {
    uint32_t dirs, files, used_clusters, free_clusters;
    uint32_t bad_entries;     /* entries that start on a free or invalid cluster (removed when repairing) */
    uint32_t bad_chains;      /* chains that run into a free or invalid cluster (cut) */
    uint32_t cross_linked;    /* clusters claimed twice (the second claim is cut) */
    uint32_t size_fixes;      /* sizes that do not match the chain (corrected) */
    uint32_t lost_clusters;   /* used but unreferenced clusters (freed) */
    uint32_t fat_mismatch;    /* FAT sectors that differ between memory and a disk copy (rewritten) */
    uint32_t problems, fixed;
} avfs_fsck_t;
int  avfs_fsck(bool repair, bool check_fat_copies, bool verbose, avfs_fsck_t* out);
/* False when the previous session never marked the volume clean (power cut, window closed). */
bool avfs_was_clean(void);
/* Sets the clean flag; call right before powering off or rebooting. */
void avfs_mark_clean(void);
/* Damages a scratch folder on purpose and checks fsck finds and repairs it (see selftest.c). 0 = ok. */
int avfs_fsck_selftest(void);
/* While on, the data of every cluster that gets freed is overwritten with zeros first. */
void avfs_wipe_freed(bool on);

/* Query the AVFS RAM filesystem (shown as Volume 0).  Returns 0. */
int avfs_ramfs_query(avfs_ramfs_info_t* out);


#endif // AVFS_H