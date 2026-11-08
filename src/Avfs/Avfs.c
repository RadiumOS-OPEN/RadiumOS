// Avfs.c - Simple RAM filesystem with directory support + persistent /disk/ mount
// Author: scp_2801
//
// /disk/* paths are transparently routed to FAT32 on fetchdisk.img via ATA PIO.
// All other paths use the original AVFS RAM filesystem unchanged.
// No new source files — everything lives here.
//
// Seamless routing: every public function resolves its argument against the
// current directory FIRST and only then checks for the /disk prefix, so
// `cd disk`, `cd sub`, `ls ..`, `cat file.txt` etc. all work through the
// regular AVFS API — from anywhere, inside or outside the disk.

#include <stdint.h>
#include "../terminal/terminal.h"
#include "../utility/utility.h"
#include "Avfs.h"
#include "../kernel/ctrlc.h"



typedef enum {
    AVFS_TYPE_FILE = 0,
    AVFS_TYPE_DIR  = 1
} avfs_entry_type_t;

typedef struct {
    char name[AVFS_FILENAME_MAX];
    uint32_t size;
    uint32_t start_block;
    uint8_t used;
    avfs_entry_type_t type;
    int parent_index;
} avfs_file_entry_t;

typedef struct {
    uint8_t data[AVFS_BLOCK_SIZE * AVFS_NUM_BLOCKS];
    avfs_file_entry_t files[AVFS_MAX_FILES];
    uint8_t block_bitmap[AVFS_NUM_BLOCKS];
    char current_dir[AVFS_PATH_MAX];
} avfs_t;

static avfs_t avfs;

// =====================================================================
// ATA PIO DRIVER  (private, static)
// =====================================================================

#define ATA_REG_DATA        0x00
#define ATA_REG_ERROR       0x01
#define ATA_REG_FEATURES    0x01
#define ATA_REG_SECCOUNT    0x02
#define ATA_REG_LBA0        0x03
#define ATA_REG_LBA1        0x04
#define ATA_REG_LBA2        0x05
#define ATA_REG_HDDEVSEL    0x06
#define ATA_REG_STATUS      0x07
#define ATA_REG_COMMAND     0x07

#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_FLUSH       0xE7
#define ATA_CMD_IDENTIFY    0xEC

#define ATA_SR_BSY          0x80
#define ATA_SR_DRDY         0x40
#define ATA_SR_DRQ          0x08
#define ATA_SR_ERR          0x01

#define ATA_BUS_PRIMARY     0x1F0
#define ATA_BUS_SECONDARY   0x170
#define ATA_DRIVE_MASTER    0xE0
#define ATA_DRIVE_SLAVE     0xF0

#define DISK_SECTOR_SIZE    512

static uint16_t s_ata_bus   = ATA_BUS_PRIMARY;
static uint8_t  s_ata_drive = ATA_DRIVE_MASTER;
static bool     s_ata_ready = false;

static inline void     s_outb(uint16_t port, uint8_t  v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port)); }
static inline uint8_t  s_inb (uint16_t port)            { uint8_t  v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }
static inline uint16_t s_inw (uint16_t port)            { uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(port)); return v; }
static inline void     s_outw(uint16_t port, uint16_t v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(port)); }
// One REP INSW/OUTSW moves a whole sector with a single VM exit under QEMU/KVM.
static inline void s_insw_sector (uint16_t port, void* dst)       { uint32_t n = 256; __asm__ volatile("rep insw"  : "+D"(dst), "+c"(n) : "d"(port) : "memory"); }
static inline void s_outsw_sector(uint16_t port, const void* src) { uint32_t n = 128; __asm__ volatile("rep outsl" : "+S"(src), "+c"(n) : "d"(port) : "memory"); }
static inline void s_outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(port)); }

// Polling the status register back to back keeps an emulated drive from finishing (every read is a trap that
// holds the emulator's lock), so the loop spins a little between reads.
static void s_ata_spin(void) { for (int k = 0; k < 150; k++) __asm__ volatile("pause"); }

static int s_ata_wait_not_busy(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(s_inb(s_ata_bus + ATA_REG_STATUS) & ATA_SR_BSY)) return 0;
        s_ata_spin();
    }
    return -1;
}

static int s_ata_wait_drq(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t st = s_inb(s_ata_bus + ATA_REG_STATUS);
        if (st & ATA_SR_ERR) return -1;
        if (st & ATA_SR_DRQ) return  0;
    }
    return -1;
}

static void s_ata_delay400(void) {
    s_inb(s_ata_bus + 0x0C); s_inb(s_ata_bus + 0x0C);
    s_inb(s_ata_bus + 0x0C); s_inb(s_ata_bus + 0x0C);
}

static int s_ata_select_lba(uint32_t lba, uint32_t count) {
    if (s_ata_wait_not_busy() != 0) return -1;
    s_outb(s_ata_bus + ATA_REG_HDDEVSEL, s_ata_drive | ((lba >> 24) & 0x0F));
    s_ata_delay400();
    s_outb(s_ata_bus + ATA_REG_FEATURES, 0);
    s_outb(s_ata_bus + ATA_REG_SECCOUNT, (uint8_t)count);
    s_outb(s_ata_bus + ATA_REG_LBA0, (uint8_t)( lba        & 0xFF));
    s_outb(s_ata_bus + ATA_REG_LBA1, (uint8_t)((lba >>  8) & 0xFF));
    s_outb(s_ata_bus + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    return 0;
}

static uint32_t s_ata_total_secs;   // IDENTIFY words 60-61 (LBA28 capacity)
static bool s_ata_multi;            // READ/WRITE MULTIPLE with 16-sector blocks (one drive round trip per block)

static bool s_ata_probe(uint16_t bus, uint8_t drive) {
    s_outb(bus + ATA_REG_HDDEVSEL, drive);
    for (int i = 0; i < 4; i++) s_inb(bus + ATA_REG_STATUS);
    s_outb(bus + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    for (int i = 0; i < 4; i++) s_inb(bus + ATA_REG_STATUS);
    uint8_t st = s_inb(bus + ATA_REG_STATUS);
    if (st == 0x00 || st == 0xFF) return false;
    for (int i = 0; i < 100000; i++) {
        st = s_inb(bus + ATA_REG_STATUS);
        if (!(st & ATA_SR_BSY)) break;
    }
    // ATAPI drives abort IDENTIFY (ERR). The LBA mid/high registers are no signature check here: they keep
    // whatever the last command left in them, e.g. after GRUB loaded the kernel from this very disk.
    if ((st & ATA_SR_ERR) || !(st & ATA_SR_DRQ)) return false;
    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = s_inw(bus + ATA_REG_DATA);
    s_ata_total_secs = ((uint32_t)id[61] << 16) | id[60];
    return true;
}

static void s_dma_init(void);

static void s_ata_init(void) {
    static const struct { uint16_t bus; uint8_t drv; } cands[4] = {
        { ATA_BUS_PRIMARY,   ATA_DRIVE_MASTER },
        { ATA_BUS_PRIMARY,   ATA_DRIVE_SLAVE  },
        { ATA_BUS_SECONDARY, ATA_DRIVE_MASTER },
        { ATA_BUS_SECONDARY, ATA_DRIVE_SLAVE  },
    };
    for (int i = 0; i < 4; i++) {
        if (s_ata_probe(cands[i].bus, cands[i].drv)) {
            s_ata_bus   = cands[i].bus;
            s_ata_drive = cands[i].drv;
            s_ata_ready = true;
            // SET MULTIPLE MODE: the drive then moves 16 sectors per block instead of completing every sector separately
            s_outb(s_ata_bus + ATA_REG_HDDEVSEL, s_ata_drive);
            s_ata_delay400();
            s_outb(s_ata_bus + ATA_REG_SECCOUNT, 16);
            s_outb(s_ata_bus + ATA_REG_COMMAND, 0xC6);
            s_ata_delay400();
            s_ata_multi = s_ata_wait_not_busy() == 0 && !(s_inb(s_ata_bus + ATA_REG_STATUS) & (ATA_SR_ERR | 0x20));
            s_dma_init();
            return;
        }
    }
}


// ---- bus-master DMA ----------------------------------------------------------------------------------------------------
// A PIO write costs one trapped port access per 4 bytes (~1 ms per sector under QEMU). The PIIX IDE controller can move
// the data itself: READ DMA / WRITE DMA through a bounce buffer, falling back to PIO if anything looks wrong.

extern uint32_t pci_config_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
extern uint16_t pci_config_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
extern void     pci_config_write_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);

static uint16_t s_bm_base;                                        // bus-master I/O base of the channel (0 = no DMA)
static uint32_t s_prd[16] __attribute__((aligned(8)));            // physical region descriptors (an entry may not cross 64 KiB)
static uint8_t  s_dma_buf[255 * 512 + 1] __attribute__((aligned(4096)));

static void s_dma_init(void) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        for (uint8_t fn = 0; fn < 8; fn++) {
            uint32_t id = pci_config_read_dword(0, slot, fn, 0);
            if (id == 0xFFFFFFFFu) { if (fn == 0) break; continue; }
            uint32_t cls = pci_config_read_dword(0, slot, fn, 8) >> 16;
            if (cls != 0x0101) continue;
            uint32_t bar4 = pci_config_read_dword(0, slot, fn, 0x20);
            if (!(bar4 & 1) || !(bar4 & 0xFFFC)) continue;
            pci_config_write_word(0, slot, fn, 4, (uint16_t)(pci_config_read_word(0, slot, fn, 4) | 0x0005));
            s_bm_base = (uint16_t)((bar4 & 0xFFFC) + (s_ata_bus == ATA_BUS_SECONDARY ? 8 : 0));
            return;
        }
    }
}

// 1..255 sectors. 0 = done, -1 = failed (the caller then uses PIO)
static int s_dma_xfer(uint32_t lba, uint32_t count, void* buf, bool write) {
    uint32_t bytes = count * DISK_SECTOR_SIZE;
    if (write) memcpy(s_dma_buf, buf, bytes);
    uint32_t phys = (uint32_t)(uintptr_t)s_dma_buf;
    int np = 0;
    for (uint32_t addr = phys, left = bytes; left; ) {
        uint32_t n = 65536 - (addr & 0xFFFFu);
        if (n > left) n = left;
        s_prd[np * 2] = addr;
        s_prd[np * 2 + 1] = (n & 0xFFFFu) | (n == left ? 0x80000000u : 0);   // a size field of 0 means 64 KiB
        np++; addr += n; left -= n;
    }
    uint8_t dir = write ? 0x00 : 0x08;
    s_outl(s_bm_base + 4, (uint32_t)(uintptr_t)s_prd);
    s_outb(s_bm_base + 2, 0x06);                                            // clear error and interrupt bits
    s_outb(s_bm_base + 0, dir);
    if (s_ata_select_lba(lba, count) != 0) return -1;
    s_outb(s_ata_bus + ATA_REG_COMMAND, write ? 0xCA : 0xC8);
    s_outb(s_bm_base + 0, (uint8_t)(dir | 1));                                // start
    int ok = -1;
    for (int i = 0; i < 2000000; i++) {
        uint8_t st = s_inb(s_bm_base + 2);
        if (st & 0x02) break;                                                // transfer error
        if ((st & 0x04) && !(st & 0x01)) { ok = 0; break; }                  // interrupt raised, engine idle
        if (i > 100) s_ata_spin();
    }
    s_outb(s_bm_base + 0, 0);
    uint8_t ata = s_inb(s_ata_bus + ATA_REG_STATUS);
    s_outb(s_bm_base + 2, 0x06);
    if (ok != 0 || (ata & (ATA_SR_ERR | 0x20 | ATA_SR_BSY))) return -1;
    if (!write) memcpy(buf, s_dma_buf, bytes);
    return 0;
}

// Reads `count` (1..255) sectors with a single ATA command.
static int s_disk_read_sectors(uint32_t lba, uint32_t count, void* buf) {
    if (!s_ata_ready || count == 0 || count > 255) return -1;
    if (s_bm_base) {
        if (s_dma_xfer(lba, count, buf, false) == 0) return 0;
        s_bm_base = 0;                                    // DMA misbehaved: PIO from now on
    }
    if (s_ata_select_lba(lba, count) != 0) return -1;
    uint8_t* dst = (uint8_t*)buf;
    if (s_ata_multi && count >= 16) {
        s_outb(s_ata_bus + ATA_REG_COMMAND, 0xC4);                 // READ MULTIPLE
        s_ata_delay400();
        for (uint32_t done = 0; done < count; ) {
            uint32_t n = count - done < 16 ? count - done : 16;
            if (s_ata_wait_drq() != 0) return -1;
            for (uint32_t k = 0; k < n; k++, dst += DISK_SECTOR_SIZE) s_insw_sector(s_ata_bus + ATA_REG_DATA, dst);
            done += n;
        }
        return 0;
    }
    s_outb(s_ata_bus + ATA_REG_COMMAND, ATA_CMD_READ_PIO);
    s_ata_delay400();
    for (uint32_t i = 0; i < count; i++, dst += DISK_SECTOR_SIZE) {
        if (s_ata_wait_drq() != 0) return -1;
        s_insw_sector(s_ata_bus + ATA_REG_DATA, dst);
    }
    return 0;
}

static int s_disk_read_sector(uint32_t lba, void* buf) { return s_disk_read_sectors(lba, 1, buf); }

// Writes sectors without asking the drive to flush its cache; follow a run of these with s_disk_flush().
static int s_disk_write_sectors_nf(uint32_t lba, uint32_t count, const void* buf) {
    if (!s_ata_ready || count == 0 || count > 255) return -1;
    if (s_bm_base) {
        if (s_dma_xfer(lba, count, (void*)buf, true) == 0) return 0;
        s_bm_base = 0;
    }
    if (s_ata_select_lba(lba, count) != 0) return -1;
    const uint8_t* src = (const uint8_t*)buf;
    if (s_ata_multi && count >= 16) {
        s_outb(s_ata_bus + ATA_REG_COMMAND, 0xC5);                 // WRITE MULTIPLE
        s_ata_delay400();
        for (uint32_t done = 0; done < count; ) {
            uint32_t n = count - done < 16 ? count - done : 16;
            if (s_ata_wait_drq() != 0) return -1;
            for (uint32_t k = 0; k < n; k++, src += DISK_SECTOR_SIZE) s_outsw_sector(s_ata_bus + ATA_REG_DATA, src);
            done += n;
        }
        if (s_ata_wait_not_busy() != 0) return -1;
        return 0;
    }
    s_outb(s_ata_bus + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);
    s_ata_delay400();
    for (uint32_t i = 0; i < count; i++, src += DISK_SECTOR_SIZE) {
        if (s_ata_wait_drq() != 0) return -1;
        s_outsw_sector(s_ata_bus + ATA_REG_DATA, src);
    }
    if (s_ata_wait_not_busy() != 0) return -1;
    return 0;
}

static int s_disk_write_sector_nf(uint32_t lba, const void* buf) { return s_disk_write_sectors_nf(lba, 1, buf); }

static void s_disk_flush(void) {
    if (!s_ata_ready) return;
    s_outb(s_ata_bus + ATA_REG_COMMAND, ATA_CMD_FLUSH);
    s_ata_wait_not_busy();
}

static int s_disk_write_sector(uint32_t lba, const void* buf) {
    if (s_disk_write_sector_nf(lba, buf) != 0) return -1;
    s_disk_flush();
    return 0;
}

// =====================================================================
// FAT32 LAYER  (private, static)
// =====================================================================

#define FAT_SECTOR_SIZE       512
#define FAT_SECTORS_PER_CLUS  16
#define FAT_CLUSTER_SIZE      (FAT_SECTOR_SIZE * FAT_SECTORS_PER_CLUS)
#define FAT_NUM_FATS          2
#define FAT_RESERVED_SECTORS  32
#define FAT_FIRST_FAT_LBA     FAT_RESERVED_SECTORS
#define FAT_CAP_ENTRIES       656384u        // room for a 5 GiB volume (5128 FAT sectors)
#define VOLUME_SECTORS        10485760u      // 5 GiB
// The raw fetch store (rust_lib/src/disk_store.rs) lives at these sectors; the volume keeps them as the hidden file
// STORE.BIN so that nothing else is ever allocated there and the image still mounts as ordinary FAT32 on the host.
#define STORE_LBA             65536u
#define STORE_SECTORS         458752u        // keep equal to DISK_SECTORS in disk_store.rs
#define STORE_CLUSTERS        (STORE_SECTORS / FAT_SECTORS_PER_CLUS)
#define FAT_ROOT_CLUSTER      2
#define FAT_EOC               0x0FFFFFF8u
#define FAT_FREE              0x00000000u
#define FAT_ATTR_DIRECTORY    0x10
#define FAT_ATTR_ARCHIVE      0x20
#define FAT_ATTR_VOLUME_ID    0x08
#define FAT_ATTR_LFN          0x0F
#define FAT_DIR_ENT_PER_SEC   (FAT_SECTOR_SIZE / 32)

typedef struct __attribute__((packed)) {
    uint8_t  jump[3];
    uint8_t  oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
} fat32_bpb_t;

typedef struct __attribute__((packed)) {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t acc_date;
    uint16_t first_cluster_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} fat32_dirent_t;

static bool     s_fat_ready = false;
static uint32_t s_fat[FAT_CAP_ENTRIES];
static bool     s_fat_dirty = false;
static uint8_t  s_fat_dirty_map[FAT_CAP_ENTRIES / 128 / 8 + 1];   // one bit per FAT sector that changed
static bool     s_alloc_no_zero = false;   // set by s_chain_write, which writes every cluster it allocates
static uint8_t  s_force_attr = 0;          // directory entry attribute for the next s_dir_write_entry (0 = archive)

// Volume geometry, set from the BPB at mount (or computed when formatting)
static uint32_t s_total_secs, s_fat_secs, s_fat_lba2, s_data_lba, s_nent;
static uint32_t s_used_clusters, s_free_hint, s_store_clusters;
static bool     s_was_clean = true;        // FAT[1] bit 27 as found at mount (0 = the last session did not shut down cleanly)
#define FAT_CLEAN_BIT 0x08000000u

static uint32_t s_clus_to_lba(uint32_t c) {
    return s_data_lba + (c - 2) * FAT_SECTORS_PER_CLUS;
}

static void s_fat_set(uint32_t i, uint32_t v) {
    s_fat[i] = v;
    s_fat_dirty_map[i >> 10] |= (uint8_t)(1u << ((i >> 7) & 7));
    s_fat_dirty = true;
}

static void s_set_layout(uint32_t total, uint32_t fat_secs) {
    s_total_secs = total;
    s_fat_secs   = fat_secs;
    s_fat_lba2   = FAT_FIRST_FAT_LBA + fat_secs;
    s_data_lba   = FAT_FIRST_FAT_LBA + 2 * fat_secs;
    uint32_t n   = (total - s_data_lba) / FAT_SECTORS_PER_CLUS;
    if (n > FAT_CAP_ENTRIES - 2) n = FAT_CAP_ENTRIES - 2;
    s_nent = n + 2;
}

// FAT sectors rounded to a multiple of 8 keep the data area (and so STORE_LBA) cluster-aligned.
static void s_compute_layout(uint32_t total) {
    uint32_t est = (total - FAT_RESERVED_SECTORS) / FAT_SECTORS_PER_CLUS;
    uint32_t fs  = ((est + 2) * 4 + FAT_SECTOR_SIZE - 1) / FAT_SECTOR_SIZE;
    s_set_layout(total, (fs + 7) & ~7u);
}

static uint32_t s_store_first_cluster(void) { return 2 + (STORE_LBA - s_data_lba) / FAT_SECTORS_PER_CLUS; }

static int s_fat_load(void) {
    for (uint32_t i = 0; i < s_fat_secs; i += 128) {
        uint32_t n = s_fat_secs - i < 128 ? s_fat_secs - i : 128;
        if (s_disk_read_sectors(FAT_FIRST_FAT_LBA + i, n, (uint8_t*)s_fat + i * FAT_SECTOR_SIZE) != 0) return -1;
    }
    memset(s_fat_dirty_map, 0, sizeof(s_fat_dirty_map));
    s_used_clusters = 0;
    for (uint32_t i = 2; i < s_nent; i++) if (s_fat[i] != FAT_FREE) s_used_clusters++;
    s_free_hint = 2;
    uint32_t c0 = s_store_first_cluster();
    s_store_clusters = (s_data_lba <= STORE_LBA && c0 + STORE_CLUSTERS <= s_nent && s_fat[c0] != FAT_FREE) ? STORE_CLUSTERS : 0;
    return 0;
}

// Writes only the FAT sectors that changed (to both copies), runs of neighbours in one command.
static int s_fat_flush(void) {
    if (!s_fat_dirty) return 0;
    const uint8_t* src = (const uint8_t*)s_fat;
    for (uint32_t i = 0; i < s_fat_secs; ) {
        if (!(s_fat_dirty_map[i >> 3] & (1u << (i & 7)))) { i++; continue; }
        uint32_t j = i + 1;
        while (j < s_fat_secs && j - i < 128 && (s_fat_dirty_map[j >> 3] & (1u << (j & 7)))) j++;
        if (s_disk_write_sectors_nf(FAT_FIRST_FAT_LBA + i, j - i, src + i * FAT_SECTOR_SIZE) != 0) return -1;
        if (s_disk_write_sectors_nf(s_fat_lba2        + i, j - i, src + i * FAT_SECTOR_SIZE) != 0) return -1;
        i = j;
    }
    s_disk_flush();
    memset(s_fat_dirty_map, 0, sizeof(s_fat_dirty_map));
    s_fat_dirty = false;
    return 0;
}

static uint32_t s_fat_alloc(void) {
    for (int pass = 0; pass < 2; pass++) {
        uint32_t from = pass == 0 ? (s_free_hint < 2 ? 2 : s_free_hint) : 2;
        uint32_t to   = pass == 0 ? s_nent : (s_free_hint < s_nent ? s_free_hint : s_nent);
        for (uint32_t i = from; i < to; i++) {
            if (s_fat[i] != FAT_FREE) continue;
            s_fat_set(i, FAT_EOC);
            s_used_clusters++;
            s_free_hint = i + 1;
            if (s_alloc_no_zero) return i;
            static uint8_t zero[FAT_CLUSTER_SIZE];   // stays all zero
            s_disk_write_sectors_nf(s_clus_to_lba(i), FAT_SECTORS_PER_CLUS, zero);
            return i;
        }
    }
    return 0;
}

static bool s_wipe_freed = false;   // zero the data of freed clusters (crypt: no plaintext left behind)

static void s_fat_free_chain(uint32_t c) {
    static uint8_t zero[FAT_CLUSTER_SIZE];   // stays all zero
    while (c >= 2 && c < 0x0FFFFFF7u) {
        uint32_t next = s_fat[c];
        if (s_wipe_freed) s_disk_write_sectors_nf(s_clus_to_lba(c), FAT_SECTORS_PER_CLUS, zero);
        s_fat_set(c, FAT_FREE);
        if (s_used_clusters) s_used_clusters--;
        if (c < s_free_hint) s_free_hint = c;
        c = next;
    }
}

static uint32_t s_fat_extend(uint32_t last) {
    uint32_t n = s_fat_alloc();
    if (!n) return 0;
    s_fat_set(last, n);
    return n;
}

// 8.3 name encode/decode
static void s_to83(const char* name, uint8_t n[8], uint8_t e[3]) {
    memset(n, ' ', 8); memset(e, ' ', 3);
    int len = 0; while (name[len]) len++;
    int dot = -1;
    for (int i = len - 1; i >= 0; i--) { if (name[i] == '.') { dot = i; break; } }
    int ni = 0;
    for (int i = 0; i < len && i != dot && ni < 8; i++) {
        char c = name[i]; if (c >= 'a' && c <= 'z') c -= 32;
        n[ni++] = (uint8_t)c;
    }
    if (dot >= 0) {
        int ei = 0;
        for (int i = dot + 1; i < len && ei < 3; i++) {
            char c = name[i]; if (c >= 'a' && c <= 'z') c -= 32;
            e[ei++] = (uint8_t)c;
        }
    }
}

static void s_from83(const uint8_t n[8], const uint8_t e[3], char* out) {
    int ni = 7; while (ni >= 0 && n[ni] == ' ') ni--;
    int oi = 0;
    for (int i = 0; i <= ni; i++) { char c = (char)n[i]; if (c >= 'A' && c <= 'Z') c += 32; out[oi++] = c; }
    int ei = 2; while (ei >= 0 && e[ei] == ' ') ei--;
    if (ei >= 0) {
        out[oi++] = '.';
        for (int i = 0; i <= ei; i++) { char c = (char)e[i]; if (c >= 'A' && c <= 'Z') c += 32; out[oi++] = c; }
    }
    out[oi] = '\0';
}

static bool s_name83_match(const char* name, const fat32_dirent_t* e) {
    uint8_t n[8], x[3]; s_to83(name, n, x);
    for (int i = 0; i < 8; i++) if (n[i] != e->name[i]) return false;
    for (int i = 0; i < 3; i++) if (x[i] != e->ext[i])  return false;
    return true;
}

// Path split: fills parts[] with pointers into path_buf, returns count
static int s_split(const char* path, char path_buf[AVFS_PATH_MAX],
                   char* parts[], int maxp) {
    strncpy(path_buf, path, AVFS_PATH_MAX - 1);
    path_buf[AVFS_PATH_MAX - 1] = '\0';
    int cnt = 0;
    char* p = path_buf;
    if (*p == '/') p++;
    while (*p && cnt < maxp) {
        parts[cnt++] = p;
        while (*p && *p != '/') p++;
        if (*p == '/') { *p = '\0'; p++; }
    }
    return cnt;
}

// Iterate non-deleted, non-LFN, non-volume entries in a cluster chain.
// State: *cluster, *sec, *idx — all 0 on first call.
// Returns pointer to static entry copy on hit, NULL when exhausted.
static bool s_dir_next(uint32_t* cluster, int* sec, int* idx,
                        fat32_dirent_t* out) {
    uint8_t buf[FAT_SECTOR_SIZE];
    while (*cluster >= 2 && *cluster < 0x0FFFFFF7u) {
        uint32_t lba = s_clus_to_lba(*cluster) + (uint32_t)*sec;
        if (s_disk_read_sector(lba, buf) != 0) return false;
        fat32_dirent_t* ents = (fat32_dirent_t*)buf;
        while (*idx < FAT_DIR_ENT_PER_SEC) {
            fat32_dirent_t* e = &ents[(*idx)++];
            uint8_t tag = (uint8_t)e->name[0];
            if (tag == 0x00) return false;
            if (tag == 0xE5) continue;
            if (e->attr == FAT_ATTR_LFN)         continue;
            if (e->attr & FAT_ATTR_VOLUME_ID)    continue;
            if (e->name[0] == '.')               continue;
            memcpy(out, e, sizeof(*e));
            return true;
        }
        *idx = 0;
        (*sec)++;
        if (*sec >= FAT_SECTORS_PER_CLUS) {
            *sec = 0;
            *cluster = s_fat[*cluster];
        }
    }
    return false;
}

// Resolve absolute FAT path to cluster.  Returns 0 on failure.
// is_dir_out and entry_out are optional.
static uint32_t s_fat_resolve(const char* path, bool* is_dir_out,
                               fat32_dirent_t* entry_out) {
    if (!path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        if (is_dir_out) *is_dir_out = true;
        return FAT_ROOT_CLUSTER;
    }
    char pb[AVFS_PATH_MAX]; char* parts[32];
    int np = s_split(path, pb, parts, 32);
    uint32_t cluster = FAT_ROOT_CLUSTER;
    for (int pi = 0; pi < np; pi++) {
        uint32_t c2 = cluster; int sec = 0, idx = 0;
        fat32_dirent_t e; bool found = false;
        while (s_dir_next(&c2, &sec, &idx, &e)) {
            if (s_name83_match(parts[pi], &e)) {
                uint32_t fc = ((uint32_t)e.first_cluster_hi << 16) | e.first_cluster_lo;
                bool isdir = (e.attr & FAT_ATTR_DIRECTORY) != 0;
                if (pi == np - 1) {
                    if (is_dir_out) *is_dir_out = isdir;
                    if (entry_out) memcpy(entry_out, &e, sizeof(e));
                    return fc ? fc : FAT_ROOT_CLUSTER;
                }
                if (!isdir) return 0;
                cluster = fc;
                found = true; break;
            }
        }
        if (!found) return 0;
    }
    return 0;
}

// Resolve parent cluster and set *leaf to the final path component
static uint32_t s_fat_parent(const char* path, const char** leaf) {
    const char* last = path;
    for (const char* p = path; *p; p++) if (*p == '/') last = p;
    *leaf = (*last == '/') ? last + 1 : path;
    if (last == path) return FAT_ROOT_CLUSTER;
    char pbuf[AVFS_PATH_MAX];
    int len = (int)(last - path); if (len == 0) len = 1;
    strncpy(pbuf, path, len < AVFS_PATH_MAX ? (uint32_t)len : AVFS_PATH_MAX - 1);
    pbuf[len] = '\0';
    bool id; uint32_t pc = s_fat_resolve(pbuf, &id, NULL);
    return (pc && id) ? pc : 0;
}

static void s_dirent_fill(fat32_dirent_t* e, const uint8_t n83[8], const uint8_t e83[3], bool is_dir, uint32_t fc, uint32_t size) {
    memcpy(e->name, n83, 8); memcpy(e->ext, e83, 3);
    e->attr             = is_dir ? FAT_ATTR_DIRECTORY : (s_force_attr ? s_force_attr : FAT_ATTR_ARCHIVE);
    e->nt_reserved      = 0; e->crt_time_tenth = 0;
    e->crt_time         = 0; e->crt_date = 0x4A21;
    e->acc_date         = 0x4A21; e->wrt_time = 0; e->wrt_date = 0x4A21;
    e->first_cluster_hi = (uint16_t)(fc >> 16);
    e->first_cluster_lo = (uint16_t)(fc & 0xFFFF);
    e->file_size        = is_dir ? 0 : size;
}

// Write or update a directory entry in a cluster chain. An entry that already has this name is replaced in place;
// only a new name takes the first free slot (taking a free slot in front of the old entry left a stale duplicate
// pointing at clusters that had been freed).
static int s_dir_write_entry(uint32_t pclus, const char* name,
                              bool is_dir, uint32_t fc, uint32_t size) {
    uint8_t n83[8], e83[3]; s_to83(name, n83, e83);
    uint8_t buf[FAT_SECTOR_SIZE];
    uint32_t cluster = pclus;
    uint32_t free_lba = 0; int free_idx = -1;
    bool end = false;
    while (!end && cluster >= 2 && cluster < 0x0FFFFFF7u) {
        uint32_t lba = s_clus_to_lba(cluster);
        for (int s = 0; s < FAT_SECTORS_PER_CLUS && !end; s++) {
            if (s_disk_read_sector(lba + s, buf) != 0) return -1;
            fat32_dirent_t* ents = (fat32_dirent_t*)buf;
            for (int i = 0; i < FAT_DIR_ENT_PER_SEC; i++) {
                fat32_dirent_t* e = &ents[i];
                uint8_t tag = (uint8_t)e->name[0];
                if (tag == 0x00 || tag == 0xE5) {
                    if (free_idx < 0) { free_lba = lba + s; free_idx = i; }
                    if (tag == 0x00) { end = true; break; }   // nothing is used behind the end marker
                    continue;
                }
                if (e->attr != FAT_ATTR_LFN && !(e->attr & FAT_ATTR_VOLUME_ID) &&
                    memcmp(e->name, n83, 8) == 0 && memcmp(e->ext, e83, 3) == 0) {
                    s_dirent_fill(e, n83, e83, is_dir, fc, size);
                    return s_disk_write_sector(lba + s, buf);
                }
            }
        }
        if (!end) cluster = s_fat[cluster];
    }
    if (free_idx >= 0) {
        if (s_disk_read_sector(free_lba, buf) != 0) return -1;
        s_dirent_fill(&((fat32_dirent_t*)buf)[free_idx], n83, e83, is_dir, fc, size);
        return s_disk_write_sector(free_lba, buf);
    }
    {
        // Directory is full: append a fresh cluster at the END of the chain.
        // (Extending from pclus directly would overwrite the first link and
        // orphan every cluster behind it once the dir spans several.)
        uint32_t tail = pclus;
        while (s_fat[tail] >= 2 && s_fat[tail] < 0x0FFFFFF7u) tail = s_fat[tail];
        uint32_t nc = s_fat_extend(tail);
        if (!nc) return -3;
        s_fat_flush();
        return s_dir_write_entry(pclus, name, is_dir, fc, size);
    }
}

// Mark entry deleted
static int s_dir_del_entry(uint32_t pclus, const char* name) {
    uint8_t n83[8], e83[3]; s_to83(name, n83, e83);
    uint8_t buf[FAT_SECTOR_SIZE];
    uint32_t cluster = pclus;
    while (cluster >= 2 && cluster < 0x0FFFFFF7u) {
        uint32_t lba = s_clus_to_lba(cluster);
        for (int s = 0; s < FAT_SECTORS_PER_CLUS; s++) {
            if (s_disk_read_sector(lba + s, buf) != 0) return -1;
            fat32_dirent_t* ents = (fat32_dirent_t*)buf;
            for (int i = 0; i < FAT_DIR_ENT_PER_SEC; i++) {
                fat32_dirent_t* e = &ents[i];
                if ((uint8_t)e->name[0] == 0x00) return -1;
                if ((uint8_t)e->name[0] == 0xE5) continue;
                if (e->attr == FAT_ATTR_LFN)     continue;
                if (memcmp(e->name, n83, 8) == 0 && memcmp(e->ext, e83, 3) == 0) {
                    e->name[0] = 0xE5;
                    s_disk_write_sector(lba + s, buf);
                    return 0;
                }
            }
        }
        cluster = s_fat[cluster];
    }
    return -1;
}

// Write dot/dotdot into freshly allocated dir cluster
static void s_dir_write_dots(uint32_t clus, uint32_t parent) {
    uint8_t buf[FAT_SECTOR_SIZE]; memset(buf, 0, FAT_SECTOR_SIZE);
    fat32_dirent_t* e = (fat32_dirent_t*)buf;
    memset(e[0].name, ' ', 8); memset(e[0].ext, ' ', 3);
    e[0].name[0] = '.'; e[0].attr = FAT_ATTR_DIRECTORY;
    e[0].first_cluster_hi = (uint16_t)(clus >> 16);
    e[0].first_cluster_lo = (uint16_t)(clus & 0xFFFF);
    e[0].wrt_date = 0x4A21;
    memset(e[1].name, ' ', 8); memset(e[1].ext, ' ', 3);
    e[1].name[0] = '.'; e[1].name[1] = '.'; e[1].attr = FAT_ATTR_DIRECTORY;
    if (parent == FAT_ROOT_CLUSTER) parent = 0;     // FAT convention: '..' of a top-level directory is 0
    e[1].first_cluster_hi = (uint16_t)(parent >> 16);
    e[1].first_cluster_lo = (uint16_t)(parent & 0xFFFF);
    e[1].wrt_date = 0x4A21;
    s_disk_write_sector(s_clus_to_lba(clus), buf);
}

// Chain read: read `size` bytes starting from first_cluster. Runs of consecutive clusters are read with one command.
static int s_chain_read(uint32_t fc, void* buf, uint32_t size) {
    uint8_t* dst = (uint8_t*)buf;
    uint32_t rem = size, c = fc;
    uint8_t sb[FAT_SECTOR_SIZE];
    while (rem > 0 && c >= 2 && c < 0x0FFFFFF7u) {
        uint32_t run = 1;
        uint32_t last = c;
        while ((run + 1) * FAT_SECTORS_PER_CLUS <= 248 && run * FAT_CLUSTER_SIZE < rem &&
               s_fat[last] == last + 1) { last++; run++; }

        uint32_t lba = s_clus_to_lba(c);
        uint32_t want = rem < run * FAT_CLUSTER_SIZE ? rem : run * FAT_CLUSTER_SIZE;
        uint32_t whole = want / FAT_SECTOR_SIZE;
        if (whole > 0) {
            if (s_disk_read_sectors(lba, whole, dst) != 0) return -1;
            dst += whole * FAT_SECTOR_SIZE; rem -= whole * FAT_SECTOR_SIZE; want -= whole * FAT_SECTOR_SIZE;
        }
        if (want > 0) {
            if (s_disk_read_sector(lba + whole, sb) != 0) return -1;
            memcpy(dst, sb, want); dst += want; rem -= want;
        }
        c = s_fat[last];
    }
    return rem == 0 ? 0 : -1;
}

static int s_chain_read_at(uint32_t fc, void* buf, uint32_t size, uint32_t offset) {
    uint32_t c = fc;
    for (uint32_t skip = offset / FAT_CLUSTER_SIZE; skip > 0; skip--) {
        if (c < 2 || c >= 0x0FFFFFF7u) return -1;
        c = s_fat[c];
    }
    uint32_t in = offset % FAT_CLUSTER_SIZE;
    uint8_t* dst = (uint8_t*)buf;
    if (in) {
        static uint8_t cbuf[FAT_CLUSTER_SIZE];
        uint32_t n = FAT_CLUSTER_SIZE - in < size ? FAT_CLUSTER_SIZE - in : size;
        if (c < 2 || c >= 0x0FFFFFF7u || s_chain_read(c, cbuf, FAT_CLUSTER_SIZE) != 0) return -1;
        memcpy(dst, cbuf + in, n);
        dst += n; size -= n;
        c = s_fat[c];
    }
    return size ? s_chain_read(c, dst, size) : 0;
}

// Chain write: allocate clusters and write `size` bytes. *fc is updated.
static int s_chain_write(uint32_t* fc, const void* buf, uint32_t size) {
    static uint8_t cbuf[FAT_CLUSTER_SIZE];
    const uint8_t* src = (const uint8_t*)buf;
    uint32_t rem = size;
    s_alloc_no_zero = true;
    if (*fc == 0) { *fc = s_fat_alloc(); if (!*fc) { s_alloc_no_zero = false; return -1; } }
    uint32_t cluster = *fc, prev = 0;
    while (rem > 0) {
        if (!cluster) { cluster = s_fat_extend(prev); if (!cluster) { s_alloc_no_zero = false; return -1; } }
        uint32_t chunk = rem < FAT_CLUSTER_SIZE ? rem : FAT_CLUSTER_SIZE;
        uint32_t sectors = (chunk + FAT_SECTOR_SIZE - 1) / FAT_SECTOR_SIZE;
        memcpy(cbuf, src, chunk);
        if (chunk < sectors * FAT_SECTOR_SIZE) memset(cbuf + chunk, 0, sectors * FAT_SECTOR_SIZE - chunk);
        if (s_disk_write_sectors_nf(s_clus_to_lba(cluster), sectors, cbuf) != 0) { s_alloc_no_zero = false; return -1; }
        src += chunk; rem -= chunk;
        prev = cluster;
        uint32_t nx = s_fat[cluster];
        cluster = (nx >= 2 && nx < 0x0FFFFFF7u) ? nx : 0;
    }
    s_alloc_no_zero = false;
    return s_fat_flush();
}

// Writing is far slower than reading, and a fresh disk is already zero: only chunks that hold something are overwritten.
static int s_zero_sectors(uint32_t lba, uint32_t count) {
    static uint8_t z[128 * FAT_SECTOR_SIZE];   // stays all zero
    static uint8_t chunk[128 * FAT_SECTOR_SIZE];
    while (count) {
        uint32_t n = count < 128 ? count : 128;
        if (s_disk_read_sectors(lba, n, chunk) != 0) return -1;
        if (memcmp(chunk, z, n * FAT_SECTOR_SIZE) != 0 && s_disk_write_sectors_nf(lba, n, z) != 0) return -1;
        lba += n; count -= n;
    }
    return 0;
}

// Lays out a fresh volume across the whole disk (5 GiB at most): both FATs, the root directory, the hidden STORE.BIN
// that keeps the raw fetch store's sectors, and last of all the boot sector, so an interrupted format leaves no volume.
static int s_fat_format(void) {
    uint32_t total = (s_ata_total_secs && s_ata_total_secs < VOLUME_SECTORS) ? s_ata_total_secs : VOLUME_SECTORS;
    s_compute_layout(total);
    if (s_zero_sectors(FAT_FIRST_FAT_LBA, s_fat_secs) != 0 || s_zero_sectors(s_fat_lba2, s_fat_secs) != 0) return -1;
    memset(s_fat, 0, sizeof(s_fat));
    memset(s_fat_dirty_map, 0, sizeof(s_fat_dirty_map));
    s_fat_set(0, 0x0FFFFFF8); s_fat_set(1, 0x0FFFFFFF); s_fat_set(2, FAT_EOC);
    s_used_clusters = 1; s_free_hint = 3; s_store_clusters = 0;
    if (s_zero_sectors(s_clus_to_lba(FAT_ROOT_CLUSTER), FAT_SECTORS_PER_CLUS) != 0) return -1;
    if (s_fat_flush() != 0) return -1;

    uint32_t c0 = s_store_first_cluster();
    if (s_data_lba <= STORE_LBA && c0 + STORE_CLUSTERS <= s_nent) {
        for (uint32_t k = 0; k < STORE_CLUSTERS; k++) s_fat_set(c0 + k, k + 1 < STORE_CLUSTERS ? c0 + k + 1 : FAT_EOC);
        s_used_clusters += STORE_CLUSTERS;
        s_store_clusters = STORE_CLUSTERS;
        s_force_attr = 0x06;                       // hidden + system
        int r = s_dir_write_entry(FAT_ROOT_CLUSTER, "store.bin", false, c0, STORE_CLUSTERS * FAT_CLUSTER_SIZE);
        s_force_attr = 0;
        if (r != 0 || s_fat_flush() != 0) return -1;
    }

    uint8_t sec[FAT_SECTOR_SIZE]; memset(sec, 0, FAT_SECTOR_SIZE);
    fat32_bpb_t* bpb = (fat32_bpb_t*)sec;
    bpb->jump[0] = 0xEB; bpb->jump[1] = 0x58; bpb->jump[2] = 0x90;
    memcpy(bpb->oem_name, "RADIUMOS", 8);
    bpb->bytes_per_sector    = FAT_SECTOR_SIZE;
    bpb->sectors_per_cluster = FAT_SECTORS_PER_CLUS;
    bpb->reserved_sectors    = FAT_RESERVED_SECTORS;
    bpb->num_fats            = FAT_NUM_FATS;
    bpb->media_type          = 0xF8;
    bpb->sectors_per_track   = 63;
    bpb->num_heads           = 255;
    bpb->total_sectors_32    = s_total_secs;
    bpb->fat_size_32         = s_fat_secs;
    bpb->root_cluster        = FAT_ROOT_CLUSTER;
    bpb->drive_number        = 0x80;
    bpb->boot_signature      = 0x29;
    bpb->volume_id           = 0xDEADBEEF;
    memcpy(bpb->volume_label, "RADIUMDISK ", 11);
    memcpy(bpb->fs_type,      "FAT32   ",   8);
    sec[510] = 0x55; sec[511] = 0xAA;
    return s_disk_write_sector(0, sec);
}

// ---- moving a disk from the old 16 MB layout -----------------------------------------------------------------------------
// The first versions kept a 1-sector header, two 32-sector FATs and 4 KB clusters from sector 65. The new FATs sit on top
// of that old data, so everything is read into memory first and written back through the normal file calls.

extern uint8_t* rust_c_alloc(size_t size);
extern void rust_c_free(uint8_t* p);

#define OLD_FAT_SECS   32
#define OLD_SPC        8
#define OLD_DATA_LBA   65
#define OLD_ENTRIES    (OLD_FAT_SECS * FAT_SECTOR_SIZE / 4)
#define MIG_MAX        400

typedef struct { char path[88]; uint32_t size, off; uint8_t dir; } mig_rec_t;
static mig_rec_t s_mig[MIG_MAX];
static int s_mig_count;
static uint32_t s_old_fat[OLD_ENTRIES];
static uint8_t* s_mig_blob;
static uint32_t s_mig_blob_size, s_mig_used;
static bool s_mig_fail;

static void s_mig_read_cluster(uint32_t c, uint8_t* out) {
    if (s_disk_read_sectors(OLD_DATA_LBA + (c - 2) * OLD_SPC, OLD_SPC, out) != 0) s_mig_fail = true;
}

static void s_mig_walk(uint32_t dir_cluster, const char* prefix, int depth) {
    static uint8_t cl[OLD_SPC * FAT_SECTOR_SIZE];   // directories are walked one at a time; recursion reads again below
    if (depth > 6) return;
    uint32_t c = dir_cluster;
    for (int guard = 0; c >= 2 && c < OLD_ENTRIES && guard < 64 && !s_mig_fail; guard++) {
        uint8_t local[OLD_SPC * FAT_SECTOR_SIZE];
        s_mig_read_cluster(c, local);
        for (int i = 0; i < (int)(sizeof(local) / 32); i++) {
            fat32_dirent_t* e = (fat32_dirent_t*)(local + i * 32);
            uint8_t tag = e->name[0];
            if (tag == 0x00) { c = 0; break; }
            if (tag == 0xE5 || e->attr == FAT_ATTR_LFN || (e->attr & FAT_ATTR_VOLUME_ID) || tag == '.') continue;
            if (s_mig_count >= MIG_MAX) { s_mig_fail = true; return; }
            mig_rec_t* r = &s_mig[s_mig_count];
            char leaf[16];
            s_from83(e->name, e->ext, leaf);
            size_t pl = strlen(prefix), ll = strlen(leaf);
            if (pl + 1 + ll >= sizeof(r->path)) continue;
            memcpy(r->path, prefix, pl); r->path[pl] = '/'; memcpy(r->path + pl + 1, leaf, ll + 1);
            uint32_t fc = ((uint32_t)e->first_cluster_hi << 16) | e->first_cluster_lo;
            r->dir = (e->attr & FAT_ATTR_DIRECTORY) != 0;
            r->size = r->dir ? 0 : e->file_size;
            r->off = s_mig_used;
            s_mig_count++;
            if (r->dir) {
                if (fc >= 2) s_mig_walk(fc, r->path, depth + 1);
            } else if (r->size) {
                if (s_mig_used + r->size + FAT_CLUSTER_SIZE > s_mig_blob_size) { s_mig_fail = true; return; }
                uint32_t left = r->size, cc = fc;
                uint8_t* dst = s_mig_blob + r->off;
                for (int g2 = 0; left && cc >= 2 && cc < OLD_ENTRIES && g2 < 5000; g2++) {
                    uint32_t n = left < OLD_SPC * FAT_SECTOR_SIZE ? left : OLD_SPC * FAT_SECTOR_SIZE;
                    if (n == OLD_SPC * FAT_SECTOR_SIZE) s_disk_read_sectors(OLD_DATA_LBA + (cc - 2) * OLD_SPC, OLD_SPC, dst);
                    else { s_mig_read_cluster(cc, cl); memcpy(dst, cl, n); }
                    dst += n; left -= n; cc = s_old_fat[cc];
                }
                if (left) s_mig_fail = true;
                s_mig_used += (r->size + 15) & ~15u;
            }
        }
        if (c >= 2) c = s_old_fat[c];
    }
}

// Reads the whole old volume into memory. false: not enough memory or a damaged tree (the old volume is then left alone).
static bool s_mig_stage(void) {
    for (int i = 0; i < OLD_FAT_SECS; i++)
        if (s_disk_read_sector(1 + i, (uint8_t*)s_old_fat + i * FAT_SECTOR_SIZE) != 0) return false;
    uint32_t used = 0;
    for (uint32_t i = 2; i < OLD_ENTRIES; i++) if (s_old_fat[i]) used++;
    s_mig_blob_size = used * OLD_SPC * FAT_SECTOR_SIZE + 65536;
    s_mig_blob = rust_c_alloc(s_mig_blob_size);
    if (!s_mig_blob) return false;
    s_mig_count = 0; s_mig_used = 0; s_mig_fail = false;
    s_mig_walk(FAT_ROOT_CLUSTER, "/disk", 0);
    if (s_mig_fail) { rust_c_free(s_mig_blob); s_mig_blob = NULL; return false; }
    return true;
}

static void s_mig_replay(void) {
    int bad = 0;
    for (int i = 0; i < s_mig_count; i++) {
        mig_rec_t* r = &s_mig[i];
        int rc = r->dir ? avfs_create_dir(r->path) : avfs_save_file(r->path, s_mig_blob + r->off, r->size, true);
        if (rc != 0) bad++;
    }
    rust_c_free(s_mig_blob);
    s_mig_blob = NULL;
    printr("[disk] moved %d entries to the new 5 GB volume%s\n", s_mig_count, bad ? " (some failed)" : "");
}

static void s_fat_init(void) {
    s_ata_init();
    if (!s_ata_ready) { printr("[disk] No ATA drive found — /disk/ unavailable\n"); return; }
    uint8_t sec0[FAT_SECTOR_SIZE];
    if (s_disk_read_sector(0, sec0) != 0) { printr("[disk] Sector 0 read failed\n"); return; }
    fat32_bpb_t* b = (fat32_bpb_t*)sec0;
    bool ours = sec0[510] == 0x55 && sec0[511] == 0xAA && b->bytes_per_sector == FAT_SECTOR_SIZE &&
                b->num_fats == FAT_NUM_FATS && memcmp(b->oem_name, "RADIUMOS", 8) == 0;
    bool is_new = ours && b->sectors_per_cluster == FAT_SECTORS_PER_CLUS && b->reserved_sectors == FAT_RESERVED_SECTORS &&
                  b->fat_size_32 && b->total_sectors_32 > b->fat_size_32 * 2 + FAT_RESERVED_SECTORS;
    bool is_old = ours && b->sectors_per_cluster == OLD_SPC && b->reserved_sectors == 1 && b->fat_size_32 == OLD_FAT_SECS;
    bool migrate = false;
    if (is_old) {
        if (!s_mig_stage()) { printr("[disk] The old /disk volume needs ~20 MB of free memory to move to the new layout — /disk/ unavailable\n"); return; }
        migrate = true;
    }
    if (is_new) {
        s_set_layout(b->total_sectors_32, b->fat_size_32);
    } else {
        printr("[disk] Formatting the disk as a %u MB FAT32 volume...\n", (s_ata_total_secs && s_ata_total_secs < VOLUME_SECTORS ? s_ata_total_secs : VOLUME_SECTORS) >> 11);
        if (s_fat_format() != 0) { printr("[disk] Format failed\n"); return; }
    }
    if (s_fat_load() != 0) { printr("[disk] FAT load failed\n"); return; }
    s_fat_ready = true;
    s_was_clean = (s_fat[1] & FAT_CLEAN_BIT) != 0;
    if (s_was_clean) { s_fat_set(1, s_fat[1] & ~FAT_CLEAN_BIT); s_fat_flush(); }   // dirty until a clean shutdown
    printr("[disk] /disk/ mounted (FAT32, persistent, %u MB%s)\n", (s_nent - 2 - s_store_clusters) * (FAT_CLUSTER_SIZE >> 10) >> 10, s_bm_base ? ", DMA" : (s_ata_multi ? ", multi-sector I/O" : ""));
    if (migrate) s_mig_replay();
    if (!s_was_clean) {
        avfs_fsck_t r;
        avfs_fsck(false, false, false, &r);
        printr("[disk] the last session did not shut down cleanly: checked %u files, %s - run fsck for details\n",
               r.files, r.problems ? "PROBLEMS FOUND (fsck -r repairs them)" : "no problems");
    }
}

// =====================================================================
// /disk/ PATH HELPERS
// =====================================================================

#define DISK_PFX     "/disk"
#define DISK_PFX_LEN 5

static bool s_is_disk_path(const char* p) {
    if (!p || p[0] != '/' || p[1] != 'd' || p[2] != 'i' || p[3] != 's' || p[4] != 'k') return false;
    return p[5] == '\0' || p[5] == '/';
}

// Strip /disk prefix → FAT-absolute path ("/disk/a/b" → "/a/b", "/disk" → "/")
static const char* s_strip_disk(const char* p) {
    const char* after = p + DISK_PFX_LEN;
    return (*after == '\0') ? "/" : after;
}

// =====================================================================
// AVFS HELPER FUNCTIONS  (unchanged from original)
// =====================================================================

const char* avfs_get_cwd(void) { return avfs.current_dir; }

static void normalize_path(const char* path, char* output) {
    char temp[AVFS_PATH_MAX];
    strncpy(temp, path, AVFS_PATH_MAX - 1);
    temp[AVFS_PATH_MAX - 1] = '\0';
    if (temp[0] == '\0') { strcpy(output, "/"); return; }
    char result[AVFS_PATH_MAX] = {0};
    if (temp[0] == '/') strcpy(result, "/");
    else                strcpy(result, avfs.current_dir);
    char parts_buf[AVFS_PATH_MAX];
    char* parts[64]; int part_count = 0;
    if (strcmp(result, "/") != 0) {
        strncpy(parts_buf, result + 1, AVFS_PATH_MAX - 1);
        parts_buf[AVFS_PATH_MAX - 1] = '\0';
        char* p = strtok(parts_buf, "/");
        while (p && part_count < 64) { parts[part_count++] = p; p = strtok(NULL, "/"); }
    }
    char* token = strtok(temp, "/");
    while (token && part_count < 64) {
        if (strcmp(token, ".") == 0) {
        } else if (strcmp(token, "..") == 0) {
            if (part_count > 0) part_count--;
        } else {
            parts[part_count++] = token;
        }
        token = strtok(NULL, "/");
    }
    int current_len = 0;
    if (part_count == 0) { strcpy(output, "/"); current_len = 1; }
    else {
        strcpy(output, "/"); current_len = 1;
        for (int i = 0; i < part_count; i++) {
            int pl = strlen(parts[i]);
            if (current_len + 1 + pl >= AVFS_PATH_MAX) break;
            if (i > 0) { strcat(output, "/"); current_len++; }
            strcat(output, parts[i]); current_len += pl;
        }
    }
}

// ---------------------------------------------------------------------
// SEAMLESS /disk ROUTING
//
// Every public AVFS entry point resolves its argument against the current
// working directory FIRST (so relative names, "." and ".." behave the same
// everywhere), and only then checks whether the resolved path lives under
// /disk.  If it does — and the disk is up — the call is transparently
// executed against persistent FAT32 instead of the RAM filesystem: same
// functions, same semantics, no separate disk API.
//
// When no ATA drive is present the check simply fails and /disk behaves
// as an ordinary (empty) RAM directory.
// ---------------------------------------------------------------------
static bool s_routes_to_disk(const char* name, char resolved[AVFS_PATH_MAX]) {
    if (!resolved) return false;
    if (!name) { resolved[0] = '\0'; return false; }
    normalize_path(name, resolved);
    return s_fat_ready && s_is_disk_path(resolved);
}

// True when a stripped FAT path refers to the disk root itself.
static bool s_dp_is_root(const char* dp) {
    return dp[0] == '/' && dp[1] == '\0';
}

static void get_parent_path(const char* path, char* parent) {
    char normalized[AVFS_PATH_MAX]; normalize_path(path, normalized);
    if (strcmp(normalized, "/") == 0) { strcpy(parent, "/"); return; }
    strcpy(parent, normalized);
    char* last_slash = strrchr(parent, '/');
    if (last_slash && last_slash != parent) *last_slash = '\0';
    else strcpy(parent, "/");
}

static void get_filename(const char* path, char* filename) {
    char normalized[AVFS_PATH_MAX]; normalize_path(path, normalized);
    if (strcmp(normalized, "/") == 0) { strcpy(filename, "/"); return; }
    char* last_slash = strrchr(normalized, '/');
    if (last_slash) strcpy(filename, last_slash + 1);
    else            strcpy(filename, normalized);
}

static int avfs_find_free_blocks(int count) {
    if (count <= 0) return -1;
    int consecutive = 0;
    for (int i = 0; i < AVFS_NUM_BLOCKS; i++) {
        if (avfs.block_bitmap[i] == 0) {
            consecutive++;
            if (consecutive == count) return i - count + 1;
        } else { consecutive = 0; }
    }
    return -1;
}

static int avfs_find_entry(const char* path) {
    char normalized[AVFS_PATH_MAX]; normalize_path(path, normalized);
    if (strcmp(normalized, "/") == 0) return 0;
    char path_copy[AVFS_PATH_MAX]; strcpy(path_copy, normalized + 1);
    char* components[64]; int component_count = 0;
    char* token = strtok(path_copy, "/");
    while (token && component_count < 64) { components[component_count++] = token; token = strtok(NULL, "/"); }
    int current_parent = 0;
    for (int i = 0; i < component_count; i++) {
        bool found = false;
        for (int j = 0; j < AVFS_MAX_FILES; j++) {
            if (avfs.files[j].used && avfs.files[j].parent_index == current_parent &&
                strcmp(avfs.files[j].name, components[i]) == 0) {
                current_parent = j; found = true; break;
            }
        }
        if (!found) return -1;
    }
    return current_parent;
}

static int avfs_find_file(const char* name) {
    if (strchr(name, '/')) return avfs_find_entry(name);
    char full_path[AVFS_PATH_MAX];
    if (strcmp(avfs.current_dir, "/") == 0) snprintf(full_path, AVFS_PATH_MAX, "/%s", name);
    else                                     snprintf(full_path, AVFS_PATH_MAX, "%s/%s", avfs.current_dir, name);
    return avfs_find_entry(full_path);
}

// =====================================================================
// avfs_init  — boots AVFS then mounts /disk/
// =====================================================================

void avfs_init() {
    memset(&avfs, 0, sizeof(avfs));
    strcpy(avfs.current_dir, "/");
    avfs.files[0].used = 1;
    avfs.files[0].type = AVFS_TYPE_DIR;
    strcpy(avfs.files[0].name, "/");
    avfs.files[0].parent_index = -1;
    avfs.files[0].size = 0;
    avfs.files[0].start_block = 0;

    // Boot the persistent disk driver.
    s_fat_init();
    // Create the RAM-visible /disk mount point unconditionally: it shows up
    // in 'ls /' and, when no ATA drive came up, /disk gracefully degrades to
    // an ordinary (empty) RAM directory.  When the disk IS ready, every
    // resolved /disk path is routed to FAT32 instead — the shadow entry is
    // then just a visible anchor and is never actually traversed.
    if (avfs_find_entry("/disk") == -1) {
        int slot = -1;
        for (int i = 1; i < AVFS_MAX_FILES; i++) if (!avfs.files[i].used) { slot = i; break; }
        if (slot != -1) {
            strcpy(avfs.files[slot].name, "disk");
            avfs.files[slot].type         = AVFS_TYPE_DIR;
            avfs.files[slot].parent_index = 0;
            avfs.files[slot].used         = 1;
            avfs.files[slot].size         = 0;
            avfs.files[slot].start_block  = 0;
        }
    }
}

// =====================================================================
// PUBLIC API — every function resolves its path first, then transparently
// routes /disk paths to FAT32. RAM filesystem behaviour is unchanged.
// =====================================================================

int avfs_get_filesize(const char* name) {
    char resolved[AVFS_PATH_MAX];
    if (s_routes_to_disk(name, resolved)) {
        const char* dp = s_strip_disk(resolved);
        if (s_dp_is_root(dp)) return -1;               // "/disk" itself is a directory
        fat32_dirent_t e; uint32_t c = s_fat_resolve(dp, NULL, &e);
        if (!c || (e.attr & FAT_ATTR_DIRECTORY)) return -1;
        return (int)e.file_size;
    }
    int fi = avfs_find_file(name);
    if (fi == -1 || avfs.files[fi].type == AVFS_TYPE_DIR) return -1;
    return (int)avfs.files[fi].size;
}

int avfs_create_dir(const char* path) {
    CTRLC_SCOPE;
    char resolved[AVFS_PATH_MAX];
    if (s_routes_to_disk(path, resolved)) {
        const char* dp = s_strip_disk(resolved);
        if (s_dp_is_root(dp)) return 0;  // disk root already exists
        if (s_fat_resolve(dp, NULL, NULL)) return -1;
        const char* leaf; uint32_t parent = s_fat_parent(dp, &leaf);
        if (!parent || !*leaf) return -2;
        uint32_t nc = s_fat_alloc(); if (!nc) return -3;
        s_dir_write_dots(nc, parent);
        s_fat_flush();
        return s_dir_write_entry(parent, leaf, true, nc, 0);
    }
    char normalized[AVFS_PATH_MAX]; normalize_path(path, normalized);
    if (avfs_find_entry(normalized) != -1) return -1;
    char parent_path[AVFS_PATH_MAX]; get_parent_path(normalized, parent_path);
    int parent_index = avfs_find_entry(parent_path);
    if (parent_index == -1) return -2;
    if (avfs.files[parent_index].type != AVFS_TYPE_DIR) return -3;
    int entry_index = -1;
    for (int i = 0; i < AVFS_MAX_FILES; i++) if (!avfs.files[i].used) { entry_index = i; break; }
    if (entry_index == -1) return -4;
    char dir_name[AVFS_FILENAME_MAX]; get_filename(normalized, dir_name);
    avfs_file_entry_t* dir = &avfs.files[entry_index];
    strncpy(dir->name, dir_name, AVFS_FILENAME_MAX - 1); dir->name[AVFS_FILENAME_MAX - 1] = '\0';
    dir->type = AVFS_TYPE_DIR; dir->parent_index = parent_index;
    dir->used = 1; dir->size = 0; dir->start_block = 0;
    return 0;
}

int avfs_create_file(const char* name, uint32_t size) {
    CTRLC_SCOPE;
    char resolved[AVFS_PATH_MAX];
    if (s_routes_to_disk(name, resolved)) {
        const char* dp = s_strip_disk(resolved);
        if (s_fat_resolve(dp, NULL, NULL)) return -1; // already exists (matches RAM behaviour)
        uint32_t fc = 0;
        int r = s_chain_write(&fc, NULL, 0);
        if (r != 0 && fc == 0) fc = s_fat_alloc();
        const char* leaf; uint32_t parent = s_fat_parent(dp, &leaf);
        if (!parent || !*leaf) return -2;
        return s_dir_write_entry(parent, leaf, false, fc, size);
    }
    char normalized[AVFS_PATH_MAX]; normalize_path(name, normalized);
    if (avfs_find_entry(normalized) != -1) return -1;
    int blocks_needed = (size + AVFS_BLOCK_SIZE - 1) / AVFS_BLOCK_SIZE;
    if (blocks_needed == 0) blocks_needed = 1;
    int start_block = avfs_find_free_blocks(blocks_needed);
    if (start_block < 0) return -2;
    char parent_path[AVFS_PATH_MAX]; get_parent_path(normalized, parent_path);
    int parent_index = avfs_find_entry(parent_path);
    if (parent_index == -1) return -5;
    int file_index = -1;
    for (int i = 0; i < AVFS_MAX_FILES; i++) if (!avfs.files[i].used) { file_index = i; break; }
    if (file_index == -1) return -3;
    for (int i = start_block; i < start_block + blocks_needed; i++) avfs.block_bitmap[i] = 1;
    char filename[AVFS_FILENAME_MAX]; get_filename(normalized, filename);
    avfs_file_entry_t* f = &avfs.files[file_index];
    strncpy(f->name, filename, AVFS_FILENAME_MAX - 1); f->name[AVFS_FILENAME_MAX - 1] = 0;
    f->size = size; f->start_block = (uint32_t)start_block;
    f->used = 1; f->type = AVFS_TYPE_FILE; f->parent_index = parent_index;
    return 0;
}

bool avfs_file_exists(const char* name) {
    char resolved[AVFS_PATH_MAX];
    if (s_routes_to_disk(name, resolved))
        return s_fat_resolve(s_strip_disk(resolved), NULL, NULL) != 0;
    return avfs_find_file(name) != -1;
}

bool avfs_is_directory(const char* path) {
    char resolved[AVFS_PATH_MAX];
    if (s_routes_to_disk(path, resolved)) {
        const char* dp = s_strip_disk(resolved);
        if (s_dp_is_root(dp)) return true;
        bool isd = false; uint32_t c = s_fat_resolve(dp, &isd, NULL);
        return c != 0 && isd;
    }
    int index = avfs_find_entry(path);
    if (index == -1) return false;
    return avfs.files[index].type == AVFS_TYPE_DIR;
}

int avfs_write_file(const char* name, const void* buffer, uint32_t size, uint32_t offset) {
    CTRLC_SCOPE;
    char resolved[AVFS_PATH_MAX];
    if (s_routes_to_disk(name, resolved)) {
        if (offset != 0) return -1;
        const char* dp = s_strip_disk(resolved);
        if (s_dp_is_root(dp)) return -1;               // never write over the mount point
        // Crash-safe overwrite: the new data and its FAT chain reach the disk first, then one sector write switches the
        // directory entry over, and only then is the old chain released. A power cut at any point leaves either the
        // old file or the new one (plus at worst some unreferenced clusters, which fsck frees).
        // (Never free the root cluster: entries created by other tools can legitimately carry first_cluster == 0.)
        fat32_dirent_t old_e; uint32_t old_c = s_fat_resolve(dp, NULL, &old_e);
        uint32_t old_chain = (old_c && old_c != FAT_ROOT_CLUSTER && !(old_e.attr & FAT_ATTR_DIRECTORY)) ? old_c : 0;
        uint32_t fc = 0;
        for (int attempt = 0; ; attempt++) {
            fc = 0;
            int wr = 0;
            if (size > 0) wr = s_chain_write(&fc, buffer, size);
            else { fc = s_fat_alloc(); if (!fc) wr = -1; }
            if (wr == 0) break;
            if (fc) { s_fat_free_chain(fc); s_fat_flush(); }
            if (attempt > 0 || !old_chain) return -3;
            // Out of space: give the old data up first (the pre-journal behaviour) and try again
            s_fat_free_chain(old_chain); s_fat_flush();
            old_chain = 0;
        }
        s_fat_flush();
        const char* leaf; uint32_t parent = s_fat_parent(dp, &leaf);
        if (!parent || !*leaf) { s_fat_free_chain(fc); s_fat_flush(); return -2; }
        int r = s_dir_write_entry(parent, leaf, false, fc, size);
        if (r != 0) { s_fat_free_chain(fc); s_fat_flush(); return r; }
        if (old_chain) { s_fat_free_chain(old_chain); s_fat_flush(); }
        return 0;
    }
    int file_index = avfs_find_file(name);
    if (file_index == -1) return -1;
    avfs_file_entry_t* f = &avfs.files[file_index];
    if (f->type != AVFS_TYPE_FILE) return -3;
    if (offset + size > f->size) return -2;
    uint32_t start_addr = f->start_block * AVFS_BLOCK_SIZE + offset;
    if (start_addr + size > AVFS_DATA_SIZE) return -4;
    memcpy(&avfs.data[start_addr], buffer, size);
    return 0;
}

int avfs_read_file(const char* name, void* buffer, uint32_t size, uint32_t offset) {
    char resolved[AVFS_PATH_MAX];
    if (s_routes_to_disk(name, resolved)) {
        const char* dp = s_strip_disk(resolved);
        if (s_dp_is_root(dp)) return -1;
        fat32_dirent_t e; uint32_t c = s_fat_resolve(dp, NULL, &e);
        if (!c || (e.attr & FAT_ATTR_DIRECTORY)) return -1;
        if (offset + size > e.file_size) return -2;
        return s_chain_read_at(c, buffer, size, offset);
    }
    int file_index = avfs_find_file(name);
    if (file_index == -1) return -1;
    avfs_file_entry_t* f = &avfs.files[file_index];
    if (f->type != AVFS_TYPE_FILE) return -3;
    if (offset + size > f->size) return -2;
    memcpy(buffer, &avfs.data[f->start_block * AVFS_BLOCK_SIZE + offset], size);
    return 0;
}

int avfs_save_file(const char* name, const void* buffer, uint32_t size, bool overwrite) {
    CTRLC_SCOPE;
    char resolved[AVFS_PATH_MAX];
    if (s_routes_to_disk(name, resolved)) {
        if (!buffer && size) return -1;
        const char* dp = s_strip_disk(resolved);
        if (s_dp_is_root(dp)) return -1;
        fat32_dirent_t e; uint32_t c = s_fat_resolve(dp, NULL, &e);
        if (c && (!overwrite || (e.attr & FAT_ATTR_DIRECTORY))) return -1;
        return avfs_write_file(resolved, buffer, size, 0);
    }
    if (!name || (!buffer && size) || size > AVFS_DATA_SIZE) return -1;
    uint32_t length = 0, component = 0, components = 0;
    while (name[length]) {
        if (length >= AVFS_PATH_MAX - 1) return -1;
        if ((uint8_t)name[length] < 32 || (uint8_t)name[length] == 127) return -1;
        if (name[length] == '/') { component = 0; if (++components >= 64) return -1; }
        else if (++component >= AVFS_FILENAME_MAX) return -1;
        length++;
    }
    if (!length || name[length - 1] == '/') return -1;
    if (name[0] != '/' && length + strlen(avfs.current_dir) + 1 >= AVFS_PATH_MAX) return -1;
    if (name[0] != '/') {
        for (uint32_t i = 0; avfs.current_dir[i]; i++)
            if (avfs.current_dir[i] == '/' && ++components >= 64) return -1;
    }
    char normalized[AVFS_PATH_MAX]; normalize_path(name, normalized);
    int index = avfs_find_entry(normalized);
    if (index < 0) {
        char parent[AVFS_PATH_MAX]; get_parent_path(normalized, parent);
        int pi = avfs_find_entry(parent);
        if (pi < 0 || avfs.files[pi].type != AVFS_TYPE_DIR) return -1;
        int r = avfs_create_file(normalized, size);
        if (r != 0) return r;
        r = avfs_write_file(normalized, buffer, size, 0);
        if (r != 0) avfs_remove_file(normalized);
        return r;
    }
    avfs_file_entry_t* file = &avfs.files[index];
    if (!overwrite || file->type != AVFS_TYPE_FILE) return -1;
    uint32_t blocks = (size + AVFS_BLOCK_SIZE - 1) / AVFS_BLOCK_SIZE;
    if (!blocks) blocks = 1;
    int start = avfs_find_free_blocks((int)blocks);
    if (start < 0) return -2;
    if (size) memcpy(&avfs.data[(uint32_t)start * AVFS_BLOCK_SIZE], buffer, size);
    for (uint32_t b = (uint32_t)start; b < (uint32_t)start + blocks; b++) avfs.block_bitmap[b] = 1;
    uint32_t old_blocks = (file->size + AVFS_BLOCK_SIZE - 1) / AVFS_BLOCK_SIZE;
    if (!old_blocks) old_blocks = 1;
    for (uint32_t b = file->start_block; b < file->start_block + old_blocks; b++) avfs.block_bitmap[b] = 0;
    file->start_block = (uint32_t)start; file->size = size;
    return 0;
}

int avfs_remove_file(const char* name) {
    CTRLC_SCOPE;
    char resolved[AVFS_PATH_MAX];
    if (s_routes_to_disk(name, resolved)) {
        const char* dp = s_strip_disk(resolved);
        if (s_dp_is_root(dp)) return -1;               // never delete the mount point
        bool isd = false; fat32_dirent_t e; uint32_t c = s_fat_resolve(dp, &isd, &e);
        if (!c) return -1;
        if (isd) {
            // Refuse to delete non-empty dirs
            uint32_t tc = c; int ts = 0, ti = 0; fat32_dirent_t te;
            if (s_dir_next(&tc, &ts, &ti, &te)) return -2;
        }
        // Free the data chain — or, for an empty dir, its directory cluster
        // (entries with first_cluster == 0 resolve to the root cluster and
        // must never be freed).
        // The directory entry goes first: freeing the chain first would leave an entry pointing at free clusters
        // if the power failed in between.
        const char* leaf; uint32_t parent = s_fat_parent(dp, &leaf);
        int r = s_dir_del_entry(parent, leaf);
        if (r == 0 && c != FAT_ROOT_CLUSTER) { s_fat_free_chain(c); s_fat_flush(); }
        return r;
    }
    int file_index = avfs_find_file(name);
    if (file_index == -1) return -1;
    avfs_file_entry_t* f = &avfs.files[file_index];
    if (f->type == AVFS_TYPE_DIR) {
        for (int i = 0; i < AVFS_MAX_FILES; i++)
            if (avfs.files[i].used && avfs.files[i].parent_index == file_index) return -2;
    } else {
        int blocks_used = (f->size + AVFS_BLOCK_SIZE - 1) / AVFS_BLOCK_SIZE;
        if (blocks_used == 0) blocks_used = 1;
        for (int i = (int)f->start_block; i < (int)f->start_block + blocks_used; i++) avfs.block_bitmap[i] = 0;
    }
    memset(f, 0, sizeof(avfs_file_entry_t));
    return 0;
}

int avfs_remove_dir_recursive(const char* path) {
    CTRLC_SCOPE;
    char resolved[AVFS_PATH_MAX];
    if (s_routes_to_disk(path, resolved)) {
        const char* dp = s_strip_disk(resolved);
        if (s_dp_is_root(dp)) return -1;               // refuse to wipe the whole disk
        bool isd = false; uint32_t c = s_fat_resolve(dp, &isd, NULL);
        if (!c || !isd || c == FAT_ROOT_CLUSTER) return -1;
        // Recursive: recurse into children via absolute /disk paths so
        // relative callers resolve correctly at every level.
        fat32_dirent_t e; uint32_t tc = c; int ts = 0, ti = 0;
        while (s_dir_next(&tc, &ts, &ti, &e)) {
            char child[AVFS_PATH_MAX];
            char ename[32]; s_from83(e.name, e.ext, ename);
            snprintf(child, AVFS_PATH_MAX, "%s/%s", resolved, ename);
            // files used to be skipped here, which leaked their clusters
            if (e.attr & FAT_ATTR_DIRECTORY) avfs_remove_dir_recursive(child);
            else avfs_remove_file(child);
        }
        const char* leaf; uint32_t parent = s_fat_parent(dp, &leaf);
        int r = s_dir_del_entry(parent, leaf);
        if (r == 0) { s_fat_free_chain(c); s_fat_flush(); }
        return r;
    }
    char normalized[AVFS_PATH_MAX]; normalize_path(path, normalized);
    int dir_index = avfs_find_entry(normalized);
    if (dir_index == -1) return -1;
    if (avfs.files[dir_index].type != AVFS_TYPE_DIR) return -2;
    for (int i = 0; i < AVFS_MAX_FILES; i++) {
        if (avfs.files[i].used && avfs.files[i].parent_index == dir_index) {
            if (avfs.files[i].type == AVFS_TYPE_DIR) {
                char child_path[AVFS_PATH_MAX];
                if (strcmp(normalized, "/") == 0) snprintf(child_path, AVFS_PATH_MAX, "/%s", avfs.files[i].name);
                else                               snprintf(child_path, AVFS_PATH_MAX, "%s/%s", normalized, avfs.files[i].name);
                avfs_remove_dir_recursive(child_path);
            } else {
                char child_path[AVFS_PATH_MAX];
                if (strcmp(normalized, "/") == 0) snprintf(child_path, AVFS_PATH_MAX, "/%s", avfs.files[i].name);
                else                               snprintf(child_path, AVFS_PATH_MAX, "%s/%s", normalized, avfs.files[i].name);
                avfs_remove_file(child_path);
            }
        }
    }
    memset(&avfs.files[dir_index], 0, sizeof(avfs_file_entry_t));
    return 0;
}

int avfs_chdir(const char* path) {
    char resolved[AVFS_PATH_MAX];
    if (s_routes_to_disk(path, resolved)) {
        const char* dp = s_strip_disk(resolved);
        bool isd = false; uint32_t c = s_fat_resolve(dp, &isd, NULL);
        if (!c || !isd) return -1;
        // Store the normalized absolute path so later relative arguments
        // ("cd sub", "cd ..", "cat file") keep resolving correctly.
        strcpy(avfs.current_dir, resolved);
        return 0;
    }
    char normalized[AVFS_PATH_MAX]; normalize_path(path, normalized);
    int dir_index = avfs_find_entry(normalized);
    if (dir_index == -1) return -1;
    if (avfs.files[dir_index].type != AVFS_TYPE_DIR) return -2;
    strcpy(avfs.current_dir, normalized);
    return 0;
}

const char* avfs_getcwd(void) { return avfs.current_dir; }

void avfs_list_files(void) {
    avfs_list_dir(avfs.current_dir);
}

void avfs_list_dir(const char* path) {
    char resolved[AVFS_PATH_MAX];
    if (s_routes_to_disk(path, resolved)) {
        const char* dp = s_strip_disk(resolved);
        bool isd = false; uint32_t c = s_fat_resolve(dp, &isd, NULL);
        if (!c || !isd) { printr("Directory not found: %s\n", resolved); return; }
        printr("Directory: %s\n", resolved);
        fat32_dirent_t e; uint32_t tc = c; int ts = 0, ti = 0;
        bool any = false;
        while (s_dir_next(&tc, &ts, &ti, &e)) {
            any = true;
            char ename[32]; s_from83(e.name, e.ext, ename);
            if (e.attr & FAT_ATTR_DIRECTORY) {
                printr(" [DIR]  %s\n", ename);
            } else {
                printr(" [FILE] %s ", ename);
                printr("(size: "); print_capacity(e.file_size); printr(")\n");
            }
        }
        if (!any) printr("  (empty)\n");
        return;
    }
    char normalized[AVFS_PATH_MAX]; normalize_path(path, normalized);
    int dir_index = avfs_find_entry(normalized);
    if (dir_index == -1) { printr("Directory not found: %s\n", path); return; }
    if (avfs.files[dir_index].type != AVFS_TYPE_DIR) { printr("Not a directory: %s\n", path); return; }
    printr("Directory: %s\n", normalized);
    for (int i = 0; i < AVFS_MAX_FILES; i++) {
        if (avfs.files[i].used && avfs.files[i].parent_index == dir_index && avfs.files[i].name[0] != '.') {
            if (avfs.files[i].type == AVFS_TYPE_DIR) {
                printr(" [DIR]  %s\n", avfs.files[i].name);
            } else {
                printr(" [FILE] %s ", avfs.files[i].name);
                printr("(size: "); print_capacity(avfs.files[i].size); printr(")\n");
            }
        }
    }
}

int avfs_list_directory(const char* path, DirectoryEntry* entries, int max_entries) {
    if (!entries || max_entries <= 0) return -1;
    char resolved[AVFS_PATH_MAX];
    if (s_routes_to_disk(path, resolved)) {
        const char* dp = s_strip_disk(resolved);
        bool isd = false; uint32_t c = s_fat_resolve(dp, &isd, NULL);
        if (!c || !isd) return -1;
        int count = 0;
        // Expose "." / ".." exactly like the RAM backend so callers see a
        // uniform directory shape on both sides of /disk.
        if (count < max_entries) {
            strcpy(entries[count].name, "."); entries[count].size = 0;
            entries[count].is_directory = 1; entries[count].index = -1; count++;
        }
        if (count < max_entries && !s_dp_is_root(dp)) {
            strcpy(entries[count].name, ".."); entries[count].size = 0;
            entries[count].is_directory = 1; entries[count].index = -1; count++;
        }
        fat32_dirent_t e; uint32_t tc = c; int ts = 0, ti = 0;
        while (count < max_entries && s_dir_next(&tc, &ts, &ti, &e)) {
            s_from83(e.name, e.ext, entries[count].name);
            entries[count].size         = e.file_size;
            entries[count].is_directory = (e.attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
            entries[count].index        = -1;
            count++;
        }
        return count;
    }
    char normalized[AVFS_PATH_MAX]; normalize_path(path, normalized);
    int dir_index = avfs_find_entry(normalized);
    if (dir_index == -1) return -1;
    if (avfs.files[dir_index].type != AVFS_TYPE_DIR) return -2;
    int count = 0;
    if (count < max_entries) {
        strcpy(entries[count].name, "."); entries[count].size = 0;
        entries[count].is_directory = 1; entries[count].index = dir_index; count++;
    }
    if (count < max_entries && dir_index != 0) {
        strcpy(entries[count].name, ".."); entries[count].size = 0;
        entries[count].is_directory = 1; entries[count].index = avfs.files[dir_index].parent_index; count++;
    }
    for (int i = 0; i < AVFS_MAX_FILES && count < max_entries; i++) {
        if (avfs.files[i].used && avfs.files[i].parent_index == dir_index) {
            strncpy(entries[count].name, avfs.files[i].name, 31); entries[count].name[31] = '\0';
            entries[count].size         = avfs.files[i].size;
            entries[count].is_directory = (avfs.files[i].type == AVFS_TYPE_DIR) ? 1 : 0;
            entries[count].index        = i; count++;
        }
    }
    return count;
}

int avfs_append_file(const char* name, const void* buffer, uint32_t size) {
    CTRLC_SCOPE;
    char resolved[AVFS_PATH_MAX];
    if (s_routes_to_disk(name, resolved)) {
        if (!buffer && size) return -1;
        const char* dp = s_strip_disk(resolved);
        if (s_dp_is_root(dp)) return -1;
        fat32_dirent_t e; uint32_t c = s_fat_resolve(dp, NULL, &e);
        if (!c || (e.attr & FAT_ATTR_DIRECTORY)) return -1;  // must be an existing file
        uint32_t old_size = e.file_size;
        uint32_t new_size = old_size + size;
        static uint8_t abuf[524288];  // 512 KB max
        if (new_size > sizeof(abuf)) return -4;
        if (old_size > 0 && s_chain_read(c, abuf, old_size) != 0) return -2;
        if (size > 0) memcpy(abuf + old_size, buffer, size);
        return avfs_write_file(resolved, abuf, new_size, 0);
    }
    int file_index = avfs_find_file(name);
    if (file_index == -1) return -1;
    avfs_file_entry_t* f = &avfs.files[file_index];
    if (f->type != AVFS_TYPE_FILE) return -3;
    uint32_t old_size = f->size, new_size = old_size + size;
    int old_blocks = (old_size + AVFS_BLOCK_SIZE - 1) / AVFS_BLOCK_SIZE;
    int new_blocks = (new_size + AVFS_BLOCK_SIZE - 1) / AVFS_BLOCK_SIZE;
    if (old_blocks == 0) old_blocks = 1;
    if (new_blocks == 0) new_blocks = 1;
    if (new_blocks > old_blocks) {
        for (int i = (int)f->start_block; i < (int)f->start_block + old_blocks; i++) avfs.block_bitmap[i] = 0;
        int ns = avfs_find_free_blocks(new_blocks);
        if (ns < 0) {
            for (int i = (int)f->start_block; i < (int)f->start_block + old_blocks; i++) avfs.block_bitmap[i] = 1;
            return -2;
        }
        memcpy(&avfs.data[ns * AVFS_BLOCK_SIZE], &avfs.data[f->start_block * AVFS_BLOCK_SIZE], old_size);
        for (int i = ns; i < ns + new_blocks; i++) avfs.block_bitmap[i] = 1;
        f->start_block = (uint32_t)ns;
    }
    memcpy(&avfs.data[f->start_block * AVFS_BLOCK_SIZE + old_size], buffer, size);
    f->size = new_size;
    return 0;
}

int avfs_truncate(const char* name, int newsize) {
    CTRLC_SCOPE;
    char resolved[AVFS_PATH_MAX];
    if (s_routes_to_disk(name, resolved)) {
        // For disk files: read, truncate in memory, rewrite
        if (newsize < 0) return -1;
        const char* dp = s_strip_disk(resolved);
        if (s_dp_is_root(dp)) return -1;
        fat32_dirent_t e; uint32_t c = s_fat_resolve(dp, NULL, &e);
        if (!c || (e.attr & FAT_ATTR_DIRECTORY)) return -1;
        uint32_t new_size = (uint32_t)newsize;
        if (new_size == e.file_size) return 0;
        if (new_size > e.file_size) {
            static uint8_t grow_buf[65536];
            if (new_size > sizeof(grow_buf)) return -5;
            memset(grow_buf, 0, new_size);
            if (e.file_size > 0) s_chain_read(c, grow_buf, e.file_size);
            return avfs_write_file(resolved, grow_buf, new_size, 0);
        }
        // Shrink: read prefix and rewrite
        static uint8_t shrink_buf[65536];
        if (new_size > sizeof(shrink_buf)) return -5;
        if (s_chain_read(c, shrink_buf, new_size) != 0) return -3;
        return avfs_write_file(resolved, shrink_buf, new_size, 0);
    }
    if (newsize < 0) return -5;
    int file_index = avfs_find_file(name);
    if (file_index == -1) return -1;
    avfs_file_entry_t* f = &avfs.files[file_index];
    if (f->type != AVFS_TYPE_FILE) return -3;
    uint32_t old_size = f->size, new_size = (uint32_t)newsize;
    if (new_size == old_size) return 0;
    int old_blocks = (old_size + AVFS_BLOCK_SIZE - 1) / AVFS_BLOCK_SIZE;
    int new_blocks = (new_size + AVFS_BLOCK_SIZE - 1) / AVFS_BLOCK_SIZE;
    if (old_blocks == 0) old_blocks = 1;
    if (new_blocks == 0) new_blocks = 1;
    if (new_blocks < old_blocks) {
        for (int i = (int)f->start_block + new_blocks; i < (int)f->start_block + old_blocks; i++) avfs.block_bitmap[i] = 0;
    } else if (new_blocks > old_blocks) {
        int can = 1;
        for (int i = (int)f->start_block + old_blocks; i < (int)f->start_block + new_blocks; i++)
            if (i >= AVFS_NUM_BLOCKS || avfs.block_bitmap[i] != 0) { can = 0; break; }
        if (can) {
            for (int i = (int)f->start_block + old_blocks; i < (int)f->start_block + new_blocks; i++) avfs.block_bitmap[i] = 1;
        } else {
            for (int i = (int)f->start_block; i < (int)f->start_block + old_blocks; i++) avfs.block_bitmap[i] = 0;
            int ns = avfs_find_free_blocks(new_blocks);
            if (ns < 0) {
                for (int i = (int)f->start_block; i < (int)f->start_block + old_blocks; i++) avfs.block_bitmap[i] = 1;
                return -2;
            }
            memcpy(&avfs.data[ns * AVFS_BLOCK_SIZE], &avfs.data[f->start_block * AVFS_BLOCK_SIZE], old_size);
            for (int i = ns; i < ns + new_blocks; i++) avfs.block_bitmap[i] = 1;
            f->start_block = (uint32_t)ns;
        }
        memset(&avfs.data[f->start_block * AVFS_BLOCK_SIZE + old_size], 0, new_size - old_size);
    }
    f->size = new_size;
    return 0;
}

int avfs_get_content(const char* name, char* buffer, uint32_t buffer_size) {
    if (!buffer || buffer_size == 0) return -1;
    char resolved[AVFS_PATH_MAX];
    if (s_routes_to_disk(name, resolved)) {
        // Same contract as the RAM branch below: fail when the file does
        // not fit the buffer, otherwise return it NUL-terminated.
        int file_size = avfs_get_filesize(resolved);
        if (file_size == -1) return -1;
        if ((uint32_t)file_size >= buffer_size) return -1;
        if (avfs_read_file(resolved, buffer, (uint32_t)file_size, 0) != 0) return -1;
        buffer[file_size] = '\0';
        return 0;
    }
    int file_size = avfs_get_filesize(name);
    if (file_size == -1) return -1;
    if ((uint32_t)file_size >= buffer_size) return -1;
    int read_result = avfs_read_file(name, buffer, file_size, 0);
    if (read_result != 0) return -1;
    buffer[file_size] = '\0';
    return 0;
}

#define MAX_CHECK_BUFFER_SIZE 4096
bool insideFile(const char* name, const char* search_str) {
    if (!name || !search_str) return false;
    uint32_t search_len = strlen(search_str);
    if (search_len == 0) return false;
    int file_size = avfs_get_filesize(name);
    if (file_size == -1 || (uint32_t)file_size < search_len) return false;
    if (file_size > MAX_CHECK_BUFFER_SIZE) return false;
    char buffer[MAX_CHECK_BUFFER_SIZE + 1];
    if (avfs_read_file(name, buffer, file_size, 0) != 0) return false;
    buffer[file_size] = '\0';
    return strstr(buffer, search_str) != NULL;
}

int avfs_get_file_info(int index, char* name_out, uint32_t* size_out) {
    if (index < 0 || index >= AVFS_MAX_FILES) return -1;
    if (!avfs.files[index].used) return -1;
    if (name_out) avfs_get_full_path(index, name_out);
    if (size_out) *size_out = avfs.files[index].size;
    return 0;
}

int avfs_get_full_path(int index, char* path_out) {
    if (index < 0 || index >= AVFS_MAX_FILES || !avfs.files[index].used) return -1;
    if (index == 0) { strcpy(path_out, "/"); return 0; }
    char components[32][AVFS_FILENAME_MAX]; int cc = 0;
    int current = index;
    while (current > 0 && current < AVFS_MAX_FILES) {
        if (cc >= 32) break;
        strcpy(components[cc++], avfs.files[current].name);
        current = avfs.files[current].parent_index;
    }
    strcpy(path_out, "/");
    for (int i = cc - 1; i >= 0; i--) {
        strcat(path_out, components[i]);
        if (i > 0) strcat(path_out, "/");
    }
    return 0;
}

int avfs_get_entry_info(const char* path, DirectoryEntry* entry_out) {
    if (!entry_out) return -1;
    char resolved[AVFS_PATH_MAX];
    if (s_routes_to_disk(path, resolved)) {
        const char* dp = s_strip_disk(resolved);
        if (s_dp_is_root(dp)) {                         // the mount point itself
            strcpy(entry_out->name, "disk");
            entry_out->size         = 0;
            entry_out->is_directory = 1;
            entry_out->index        = -1;
            return 0;
        }
        bool isd = false; fat32_dirent_t e;
        uint32_t c = s_fat_resolve(dp, &isd, &e);
        if (!c) return -1;
        s_from83(e.name, e.ext, entry_out->name);
        entry_out->size         = e.file_size;
        entry_out->is_directory = isd ? 1 : 0;
        entry_out->index        = -1;
        return 0;
    }
    char normalized[AVFS_PATH_MAX]; normalize_path(path, normalized);
    int index = avfs_find_entry(normalized);
    if (index == -1) return -1;
    avfs_file_entry_t* file = &avfs.files[index];
    strncpy(entry_out->name, file->name, AVFS_FILENAME_MAX - 1);
    entry_out->name[AVFS_FILENAME_MAX - 1] = '\0';
    entry_out->size         = file->size;
    entry_out->is_directory = (file->type == AVFS_TYPE_DIR) ? 1 : 0;
    entry_out->index        = index;
    return 0;
}

// Format addition for `commands/wipe.c` to allow wiping and reformatting the /disk/ filesystem
// Raw sector access for the Rust fetch store (it sits inside STORE.BIN); count 1..255.
int  avfs_raw_read(uint32_t lba, uint32_t count, void* buf)        { CTRLC_SCOPE; return s_disk_read_sectors(lba, count, buf); }
int  avfs_raw_write(uint32_t lba, uint32_t count, const void* buf) { CTRLC_SCOPE; return s_disk_write_sectors_nf(lba, count, buf); }
void avfs_raw_flush(void)                                           { CTRLC_SCOPE; s_disk_flush(); }

int avfs_format_disk(void) {
    return s_fat_format();
}

// =====================================================================
// DISK / RAMFS STATISTICS  (public — consumed by the diskpart command)
//
// Exposes allocation-level usage of both volumes so diskpart can draw
// its '#' usage bars without knowing FAT internals:
//   avfs_disk_query()     - FAT32 /disk/ capacity, geometry, object counts
//   avfs_disk_usage_map() - per-cell used/partial/free cluster map
//   avfs_ramfs_query()    - AVFS RAM volume stats (listed as Volume 0)
// All math is plain 32-bit (no 64-bit division): the FAT32 data area
// (~16 MB) fits comfortably in uint32_t.
//
// Types + prototypes live in Avfs.h (see the ADDITIONS block at the end
// of that header).
// =====================================================================

#define FAT_RESERVED_CLUSTERS 2u
#define FAT_CHAIN_END         0x0FFFFFF7u

// Depth-limited recursive walk: count files and directories under a
// cluster chain. "." / ".." are already skipped by s_dir_next; the depth
// cap guards against malformed loops in the FAT chain.
static void s_disk_count_objects(uint32_t cluster, int depth,
                                 uint32_t* files, uint32_t* dirs) {
    if (depth > 8) return;
    if (cluster < FAT_RESERVED_CLUSTERS || cluster >= FAT_CHAIN_END) return;
    fat32_dirent_t e;
    int sec = 0, idx = 0;
    while (s_dir_next(&cluster, &sec, &idx, &e)) {
        if (e.attr & FAT_ATTR_DIRECTORY) {
            (*dirs)++;
            uint32_t fc = ((uint32_t)e.first_cluster_hi << 16) | e.first_cluster_lo;
            s_disk_count_objects(fc, depth + 1, files, dirs);
        } else {
            (*files)++;
        }
    }
}

// Byte counts saturate at 4 GiB - 1 in the 32-bit fields; the *_kb fields carry the real sizes.
static uint32_t s_cluster_bytes(uint32_t clusters) {
    return clusters >= 0xFFFFFFFFu / FAT_CLUSTER_SIZE ? 0xFFFFFFFFu : clusters * FAT_CLUSTER_SIZE;
}

int avfs_disk_space(uint32_t* total_kb, uint32_t* used_kb) {
    if (!s_fat_ready) return -2;
    uint32_t total = s_nent - FAT_RESERVED_CLUSTERS - s_store_clusters;
    uint32_t used  = s_used_clusters > s_store_clusters ? s_used_clusters - s_store_clusters : 0;
    if (total_kb) *total_kb = total * (FAT_CLUSTER_SIZE >> 10);
    if (used_kb)  *used_kb  = used * (FAT_CLUSTER_SIZE >> 10);
    return 0;
}

int avfs_disk_query(avfs_disk_info_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!s_fat_ready) return -2;

    out->mounted      = true;
    out->ata_bus      = s_ata_bus;
    out->ata_drive    = s_ata_drive;
    out->sector_size  = FAT_SECTOR_SIZE;
    out->cluster_size = FAT_CLUSTER_SIZE;

    // Allocation-level usage straight from the in-memory FAT: every entry
    // that is not FREE belongs to some file/dir chain (or slack), which is
    // exactly what "used" means for a defrag-style visual.
    // The cluster range kept for the raw fetch store (STORE.BIN) is not counted: it is not space for files.
    uint32_t total = s_nent - FAT_RESERVED_CLUSTERS - s_store_clusters;
    uint32_t used  = s_used_clusters > s_store_clusters ? s_used_clusters - s_store_clusters : 0;
    out->total_clusters = total;
    out->used_clusters = used;
    out->free_clusters = total - used;
    out->total_bytes   = s_cluster_bytes(total);
    out->used_bytes    = s_cluster_bytes(used);
    out->free_bytes    = s_cluster_bytes(total - used);
    out->total_kb      = total * (FAT_CLUSTER_SIZE >> 10);
    out->used_kb       = used * (FAT_CLUSTER_SIZE >> 10);
    out->free_kb       = (total - used) * (FAT_CLUSTER_SIZE >> 10);

    out->data_start_lba = s_data_lba;

    // Pull geometry + volume label from the on-disk BPB; fall back to the
    // format constants if sector 0 cannot be read.
    uint8_t sec0[FAT_SECTOR_SIZE];
    if (s_disk_read_sector(0, sec0) == 0) {
        fat32_bpb_t* b = (fat32_bpb_t*)sec0;
        out->sectors_per_cluster = b->sectors_per_cluster;
        out->reserved_sectors    = b->reserved_sectors;
        out->fat_sectors         = b->fat_size_32;
        out->total_sectors       = b->total_sectors_32 ? b->total_sectors_32
                                                       : (uint32_t)b->total_sectors_16;
        memcpy(out->volume_label, b->volume_label, 11);
        out->volume_label[11] = '\0';
        for (int i = 10; i >= 0; i--) {
            if (out->volume_label[i] == ' ') out->volume_label[i] = '\0';
            else if (out->volume_label[i] != '\0') break;
        }
    }
    if (out->sectors_per_cluster == 0) out->sectors_per_cluster = FAT_SECTORS_PER_CLUS;
    if (out->reserved_sectors    == 0) out->reserved_sectors    = FAT_RESERVED_SECTORS;
    if (out->fat_sectors         == 0) out->fat_sectors         = s_fat_secs;
    if (out->volume_label[0]     == '\0') strcpy(out->volume_label, "NO NAME");

    s_disk_count_objects(FAT_ROOT_CLUSTER, 0, &out->file_count, &out->dir_count);
    return 0;
}

int avfs_disk_usage_map(uint8_t* cells, int ncells) {
    if (!cells || ncells <= 0) return -1;
    if (!s_fat_ready) return -2;
    const uint32_t total = s_nent - FAT_RESERVED_CLUSTERS;
    for (int i = 0; i < ncells; i++) {
        uint32_t lo = FAT_RESERVED_CLUSTERS + (uint32_t)i       * total / (uint32_t)ncells;
        uint32_t hi = FAT_RESERVED_CLUSTERS + (uint32_t)(i + 1) * total / (uint32_t)ncells;
        if (hi <= lo) hi = lo + 1;
        uint32_t span = hi - lo, usedc = 0;
        for (uint32_t c = lo; c < hi; c++) {
            if (c >= s_nent) break;
            if (s_fat[c] != FAT_FREE) usedc++;
        }
        cells[i] = (usedc == 0) ? 0 : (usedc >= span) ? 2 : 1;
    }
    return 0;
}

int avfs_ramfs_query(avfs_ramfs_info_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->ready        = true;
    out->block_size   = (uint32_t)AVFS_BLOCK_SIZE;
    out->total_blocks = (uint32_t)AVFS_NUM_BLOCKS;
    uint32_t used = 0;
    for (int i = 0; i < AVFS_NUM_BLOCKS; i++)
        if (avfs.block_bitmap[i]) used++;
    out->used_blocks = used;
    out->total_bytes = (uint32_t)AVFS_DATA_SIZE;
    out->used_bytes  = used * (uint32_t)AVFS_BLOCK_SIZE;
    out->free_bytes  = out->total_bytes - out->used_bytes;
    for (int i = 1; i < AVFS_MAX_FILES; i++) {
        if (!avfs.files[i].used) continue;
        if (avfs.files[i].type == AVFS_TYPE_DIR) out->dir_count++;
        else                                     out->file_count++;
    }
    return 0;
}

// =====================================================================
// FSCK + CLEAN-SHUTDOWN FLAG  (public - see Avfs.h)
//
// Every operation writes data first, then the FAT, then the directory entry, so a power cut leaves at worst
// clusters that nothing refers to. fsck walks the whole tree, claims each cluster chain once and reports (and with
// `repair` fixes): entries starting on a free or invalid cluster, chains that run into free clusters, clusters
// owned by two files, sizes larger than the chain, unreferenced clusters, and FAT copies that disagree.
// =====================================================================

static uint8_t s_ck_seen[FAT_CAP_ENTRIES / 8 + 1];
static avfs_fsck_t* s_ck;
static bool s_ck_repair, s_ck_verbose;

static bool s_ck_test(uint32_t c) { return (s_ck_seen[c >> 3] >> (c & 7)) & 1; }
static void s_ck_set(uint32_t c)  { s_ck_seen[c >> 3] |= (uint8_t)(1u << (c & 7)); }

#define CK_PROBLEM(...) do { s_ck->problems++; if (s_ck_verbose) { printr("  fsck: " __VA_ARGS__); printr("\n"); } } while (0)

// Marks the chain from `fc` (a valid, unclaimed cluster) as owned and returns its length; a chain that leaves the
// volume, runs into a free cluster or into one that is already owned is cut there.
static uint32_t s_ck_claim(uint32_t fc, const char* path) {
    uint32_t n = 0, c = fc;
    for (;;) {
        s_ck_set(c); n++;
        uint32_t next = s_fat[c] & 0x0FFFFFFFu;
        if (next >= 0x0FFFFFF8u) break;
        bool in_range = next >= 2 && next < s_nent && next != 0x0FFFFFF7u;
        bool is_free = in_range && (s_fat[next] & 0x0FFFFFFFu) == FAT_FREE;
        bool bad = !in_range || is_free;
        bool crossed = !bad && s_ck_test(next);
        if (bad || crossed) {
            if (crossed) { s_ck->cross_linked++; CK_PROBLEM("%s: cluster %u is also used elsewhere (cross-linked or looping)", path, next); }
            else         { s_ck->bad_chains++;   CK_PROBLEM("%s: the chain runs into %s cluster %u", path, is_free ? "free" : "invalid", next); }
            if (s_ck_repair) { s_fat_set(c, FAT_EOC); s_ck->fixed++; }
            break;
        }
        c = next;
    }
    return n;
}

static void s_ck_dir(uint32_t first, int depth, const char* path) {
    uint8_t buf[FAT_SECTOR_SIZE];
    uint32_t c = first;
    for (uint32_t guard = 0; c >= 2 && c < s_nent && guard < s_nent; guard++) {
        for (int sec = 0; sec < FAT_SECTORS_PER_CLUS; sec++) {
            uint32_t lba = s_clus_to_lba(c) + (uint32_t)sec;
            if (s_disk_read_sector(lba, buf) != 0) { CK_PROBLEM("%s: unreadable directory sector %u", path, lba); return; }
            fat32_dirent_t* ents = (fat32_dirent_t*)buf;
            bool dirty = false, end = false;
            for (int i = 0; i < FAT_DIR_ENT_PER_SEC && !end; i++) {
                fat32_dirent_t* e = &ents[i];
                uint8_t tag = e->name[0];
                if (tag == 0x00) { end = true; break; }
                if (tag == 0xE5 || e->attr == FAT_ATTR_LFN || (e->attr & FAT_ATTR_VOLUME_ID) || tag == '.') continue;

                char nm[32], full[AVFS_PATH_MAX];
                s_from83(e->name, e->ext, nm);
                snprintf(full, sizeof(full), "%s/%s", path, nm);
                bool isdir = (e->attr & FAT_ATTR_DIRECTORY) != 0;
                uint32_t fc = ((uint32_t)e->first_cluster_hi << 16) | e->first_cluster_lo;

                if (fc == 0) {                       // other tools write "0" for the root cluster / an empty file
                    if (isdir) { s_ck->dirs++; continue; }
                    s_ck->files++;
                    if (e->file_size) {
                        s_ck->size_fixes++; CK_PROBLEM("%s: has size %u but no clusters", full, e->file_size);
                        if (s_ck_repair) { e->file_size = 0; dirty = true; s_ck->fixed++; }
                    }
                    continue;
                }
                if (fc < 2 || fc >= s_nent || s_fat[fc] == FAT_FREE || s_ck_test(fc)) {
                    bool crossed = fc >= 2 && fc < s_nent && s_fat[fc] != FAT_FREE;
                    if (crossed) s_ck->cross_linked++; else s_ck->bad_entries++;
                    CK_PROBLEM("%s: starts on %s cluster %u", full, crossed ? "an already used" : (fc < 2 || fc >= s_nent ? "an invalid" : "a free"), fc);
                    if (s_ck_repair) { e->name[0] = 0xE5; dirty = true; s_ck->fixed++; }
                    continue;
                }
                uint32_t n = s_ck_claim(fc, full);
                if (isdir) {
                    s_ck->dirs++;
                    if (depth < 24) s_ck_dir(fc, depth + 1, full);
                } else {
                    s_ck->files++;
                    uint32_t need = (e->file_size + FAT_CLUSTER_SIZE - 1) / FAT_CLUSTER_SIZE;
                    if (n < need) {
                        s_ck->size_fixes++;
                        CK_PROBLEM("%s: size %u does not fit its %u cluster(s)", full, e->file_size, n);
                        if (s_ck_repair) { e->file_size = n * FAT_CLUSTER_SIZE; dirty = true; s_ck->fixed++; }
                    }
                }
            }
            if (dirty && s_disk_write_sector(lba, buf) != 0) CK_PROBLEM("%s: could not write the repaired directory", path);
            if (end) return;
        }
        uint32_t next = s_fat[c] & 0x0FFFFFFFu;
        c = (next >= 2 && next < s_nent && next < 0x0FFFFFF7u) ? next : 0;
    }
}

// FAT sectors where the on-disk copy `lba` differs from memory (and, when repairing, queued for rewriting).
static uint32_t s_ck_fat_copy(uint32_t lba, bool repair) {
    static uint8_t tmp[128 * FAT_SECTOR_SIZE];
    uint32_t bad = 0;
    for (uint32_t i = 0; i < s_fat_secs; i += 128) {
        uint32_t n = s_fat_secs - i < 128 ? s_fat_secs - i : 128;
        const uint8_t* mem = (const uint8_t*)s_fat + (size_t)i * FAT_SECTOR_SIZE;
        if (s_disk_read_sectors(lba + i, n, tmp) != 0) { bad += n; continue; }
        for (uint32_t j = 0; j < n; j++) {
            if (memcmp(tmp + j * FAT_SECTOR_SIZE, mem + j * FAT_SECTOR_SIZE, FAT_SECTOR_SIZE) == 0) continue;
            bad++;
            if (repair) { uint32_t sct = i + j; s_fat_dirty_map[sct >> 3] |= (uint8_t)(1u << (sct & 7)); s_fat_dirty = true; }
        }
    }
    return bad;
}

int avfs_fsck(bool repair, bool check_fat_copies, bool verbose, avfs_fsck_t* out) {
    CTRLC_SCOPE;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!s_fat_ready) return -2;
    s_fat_flush();
    s_ck = out; s_ck_repair = repair; s_ck_verbose = verbose;
    memset(s_ck_seen, 0, sizeof(s_ck_seen));

    s_ck_claim(FAT_ROOT_CLUSTER, "/");
    s_ck_dir(FAT_ROOT_CLUSTER, 0, "/disk");

    uint32_t lost = 0, used = 0;
    for (uint32_t c = 2; c < s_nent; c++) {
        uint32_t v = s_fat[c] & 0x0FFFFFFFu;
        if (v != FAT_FREE) used++;
        if (v == FAT_FREE || v == 0x0FFFFFF7u || s_ck_test(c)) continue;
        lost++;
        if (repair) { s_fat_set(c, FAT_FREE); used--; }
    }
    if (lost) {
        out->lost_clusters = lost;
        CK_PROBLEM("%u cluster(s) are marked used but belong to no file", lost);
        if (repair) out->fixed++;
    }
    if (repair) { s_used_clusters = used; s_free_hint = 2; }
    out->used_clusters = used;
    out->free_clusters = (s_nent - 2) - used;

    if (check_fat_copies) {
        uint32_t m1 = s_ck_fat_copy(FAT_FIRST_FAT_LBA, repair), m2 = s_ck_fat_copy(s_fat_lba2, repair);
        out->fat_mismatch = m1 + m2;
        if (m1 + m2) {
            CK_PROBLEM("%u FAT sector(s) differ between memory and the two FAT copies", m1 + m2);
            if (repair) out->fixed++;
        }
    }
    if (repair && s_fat_flush() != 0) CK_PROBLEM("could not write the repaired FAT");
    return 0;
}

bool avfs_was_clean(void) { return s_was_clean; }

void avfs_wipe_freed(bool on) { s_wipe_freed = on; }

void avfs_mark_clean(void) {
    if (!s_fat_ready || (s_fat[1] & FAT_CLEAN_BIT)) return;
    s_fat_set(1, s_fat[1] | FAT_CLEAN_BIT);
    s_fat_flush();
}

// Damages a scratch folder on purpose and checks that fsck notices and repairs each kind of damage.
// 0 = ok, 1 = no disk mounted, otherwise the number of the failed check.
int avfs_fsck_selftest(void) {
    if (!s_fat_ready) return 1;
    avfs_fsck_t r;
    avfs_fsck(false, false, false, &r);
    if (r.problems) return 2;                                           // the disk must be consistent to begin with

    uint32_t lost = s_fat_alloc(); s_fat_flush();                        // 1: a cluster nothing refers to
    avfs_fsck(false, false, false, &r);
    if (r.lost_clusters != 1) return 3;
    avfs_fsck(true, false, false, &r);
    avfs_fsck(false, false, false, &r);
    if (r.problems || s_fat[lost] != FAT_FREE) return 4;

    avfs_remove_dir_recursive("/disk/fscktest");
    if (avfs_create_dir("/disk/fscktest") != 0) return 5;
    static uint8_t blob[100];
    memset(blob, 0x5A, sizeof(blob));
    if (avfs_save_file("/disk/fscktest/f.bin", blob, sizeof(blob), true) != 0) return 6;
    uint32_t fc = s_fat_resolve("/fscktest/f.bin", NULL, NULL);
    uint32_t dirc = s_fat_resolve("/fscktest", NULL, NULL);
    if (!fc || !dirc) return 7;

    uint32_t extra = s_fat_extend(fc); s_fat_flush();                    // 2: a chain that ends in a free cluster
    s_fat_set(extra, FAT_FREE); s_fat_flush();
    avfs_fsck(false, false, false, &r);
    if (r.bad_chains != 1) return 8;
    avfs_fsck(true, false, false, &r);
    avfs_fsck(false, false, false, &r);
    if (r.problems) return 9;

    s_dir_write_entry(dirc, "f.bin", false, fc, 3 * FAT_CLUSTER_SIZE);   // 3: a size larger than the chain
    avfs_fsck(false, false, false, &r);
    if (r.size_fixes != 1) return 10;
    avfs_fsck(true, false, false, &r);
    avfs_fsck(false, false, false, &r);
    if (r.problems) return 11;
    if (avfs_get_filesize("/disk/fscktest/f.bin") != (int)FAT_CLUSTER_SIZE) return 22 + (avfs_get_filesize("/disk/fscktest/f.bin") << 8);

    if (avfs_save_file("/disk/fscktest/g.bin", blob, sizeof(blob), true) != 0) return 12;   // 4: two files on one chain
    uint32_t gc = s_fat_resolve("/fscktest/g.bin", NULL, NULL);
    s_dir_write_entry(dirc, "g.bin", false, fc, sizeof(blob));
    s_fat_free_chain(gc); s_fat_flush();
    avfs_fsck(false, false, false, &r);
    if (r.cross_linked != 1) return 13;
    avfs_fsck(true, false, false, &r);
    avfs_fsck(false, false, false, &r);
    if (r.problems || avfs_file_exists("/disk/fscktest/g.bin")) return 14;

    s_fat_free_chain(fc); s_fat_flush();                                 // 5: an entry on a free cluster
    avfs_fsck(false, false, false, &r);
    if (r.bad_entries != 1) return 15;
    avfs_fsck(true, false, false, &r);
    avfs_fsck(false, true, false, &r);
    if (r.problems || avfs_file_exists("/disk/fscktest/f.bin")) return 16;

    // a rewritten file must leave no stale duplicate entry behind (a deleted slot in front of it used to take the new entry)
    for (int i = 0; i < 4; i++) {
        char n[40]; snprintf(n, sizeof(n), "/disk/fscktest/d%d.bin", i);
        if (avfs_save_file(n, blob, sizeof(blob), true) != 0) return 17;
    }
    avfs_remove_file("/disk/fscktest/d1.bin");
    if (avfs_save_file("/disk/fscktest/d3.bin", blob, 50, true) != 0) return 18;
    DirectoryEntry ents[16];
    int n = avfs_list_directory("/disk/fscktest", ents, 16), seen = 0;
    for (int i = 0; i < n; i++) if (strcmp(ents[i].name, "D3.BIN") == 0 || strcmp(ents[i].name, "d3.bin") == 0) seen++;
    if (seen != 1 || avfs_get_filesize("/disk/fscktest/d3.bin") != 50) return 19;
    avfs_remove_file("/disk/fscktest/d3.bin");
    if (avfs_file_exists("/disk/fscktest/d3.bin")) return 20;           // would find the old copy again

    avfs_remove_dir_recursive("/disk/fscktest");
    avfs_fsck(false, true, true, &r);
    if (r.problems) { printr("fsck selftest: bad=%u chains=%u cross=%u size=%u lost=%u fatcopy=%u\n", r.bad_entries, r.bad_chains, r.cross_linked, r.size_fixes, r.lost_clusters, r.fat_mismatch); return 21; }
    return 0;
}
