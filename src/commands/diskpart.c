// diskpart.h - diskpart-style disk information & maintenance command
//
// Renamed/rebuilt from the old wipe.h.  `wipe` is no longer a standalone
// command: it is now a (still confirmation-guarded) subcommand of
// `diskpart`, which adds a Windows-diskpart-style reporting UI:
//   - '#' usage bars with VGA colors (green <50%, yellow <80%, red >=80%)
//   - a defrag-style FAT cluster map
//   - a boot/FAT/data area-layout bar
//   - a "largest files" ranking with mini bars
//
// Shell integration (command table in your shell file):
//   remove:                 { "wipe",     wipe_command }
//   add:                    { "diskpart", diskpart_command }
//   optional legacy alias:  { "wipe",     wipe_command_compat }
//     -> with the alias, old muscle memory keeps working:
//        `wipe`          => asks for confirmation (same as before)
//        `wipe --confirm` => wipes immediately (same as before)
//
// Subcommands:
//   diskpart                      disk 0 overview + usage bar
//   diskpart list disk            disk table + usage bar
//   diskpart list volume          volume table (AVFS RAMFS + FAT32 disk)
//   diskpart detail disk          layout bar, cluster map, top files
//   diskpart select disk <n>      select a disk (only disk 0 exists)
//   diskpart wipe --confirm|-y    wipe + reformat /disk/  (destructive)
//   diskpart clean --confirm|-y   alias of wipe (diskpart vocabulary)
//   diskpart help                 command listing
//
// Needs the disk-statistics API added to Avfs (see Avfs.h_ADDITIONS.txt):
//   avfs_disk_query(), avfs_disk_usage_map(), avfs_ramfs_query()
//   (the patched Avfs.c implements them; delete the old wipe.h)
//
// NOTE: all percentage/bar math is plain 32-bit on purpose - no 64-bit
//       division, so it links on freestanding i386 without libgcc.



#include <stdint.h>
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../vga/vga.h"
// Re-declared here (identical prototypes are legal C and never conflict)
// so the file still builds even before the Avfs.h additions are pasted.
int avfs_format_disk(void);
int avfs_list_directory(const char* path, DirectoryEntry* entries, int max_entries);

/* ------------------------------------------------------------------ */
/* Palette fallbacks - standard VGA attribute values (osdev naming).   */
/* Only compiled when vga.h does not already define the name.          */
/* ------------------------------------------------------------------ */


#define DP_BAR_CELLS   40    /* cells in a usage bar                */
#define DP_MAP_COLS    64    /* cluster map columns                 */
#define DP_MAP_ROWS    4     /* cluster map rows                    */
#define DP_STR_MAX     48    /* local string scratch size           */
#define DP_TOP_MAX     32    /* max root entries scanned for ranking */
#define DP_TOP_SHOW    8     /* max entries shown in "largest files" */

/* ================================================================== */
/* Small helpers (pure 32-bit math, no printf dependency)             */
/* ================================================================== */

// String equality; returns 0 when equal (same contract as strcmp).
static int dp_streq(const char* a, const char* b) {
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return *(const unsigned char*)a - *(const unsigned char*)b;
}

static char* dp_utoa(uint32_t v, char* buf) {
    char tmp[11];
    int i = 0, j = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return buf; }
    while (v > 0) { tmp[i++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

// Human-readable size: "512 B", "3.4 KB", "16.0 MB" (max "4095.9 MB").
static void dp_fmt_size(uint32_t bytes, char* out) {
    char n[12];
    if (bytes >= 1024u * 1024u) {
        uint32_t whole = bytes / (1024u * 1024u);
        uint32_t rem   = bytes % (1024u * 1024u);
        uint32_t frac  = (rem * 10u + (1024u * 1024u) / 2u) / (1024u * 1024u);
        if (frac >= 10u) { frac -= 10u; whole++; }
        dp_utoa(whole, n); strcpy(out, n); strcat(out, ".");
        dp_utoa(frac, n);  strcat(out, n); strcat(out, " MB");
    } else if (bytes >= 1024u) {
        uint32_t whole = bytes / 1024u;
        uint32_t rem   = bytes % 1024u;
        uint32_t frac  = (rem * 10u + 512u) / 1024u;
        if (frac >= 10u) { frac -= 10u; whole++; }
        dp_utoa(whole, n); strcpy(out, n); strcat(out, ".");
        dp_utoa(frac, n);  strcat(out, n); strcat(out, " KB");
    } else {
        dp_utoa(bytes, n); strcpy(out, n); strcat(out, " B");
    }
}

// Rounded percentage "NN.N%" (or "n/a"). num may equal den (=100.0%).
// Constraint: den < ~42.9e6 so num*100 fits 32 bits (both volumes are
// at most ~16.7 MB, far below the limit).
static void dp_pct_str(uint32_t num, uint32_t den, char* out) {
    if (den == 0)  { strcpy(out, "n/a");    return; }
    if (num >= den){ strcpy(out, "100.0%"); return; }
    uint32_t ip  = (num * 100u) / den;                  /* integer part   */
    uint32_t rem = (num * 100u) % den;
    uint32_t d1  = (rem * 10u) / den;                   /* first decimal  */
    uint32_t d2  = ((rem * 10u) % den) * 10u / den;     /* rounding digit */
    uint32_t x10 = ip * 10u + d1 + ((d2 >= 5u) ? 1u : 0u);
    if (x10 >= 1000u) { strcpy(out, "100.0%"); return; }
    char a[8], b[8];
    dp_utoa(x10 / 10u, a);
    dp_utoa(x10 % 10u, b);
    strcpy(out, a); strcat(out, "."); strcat(out, b); strcat(out, "%");
}

// Copy s into out, left-justified, space-padded to width (truncates if
// longer - callers keep every string shorter than the column width).
static void dp_pad(const char* s, int width, char* out) {
    int i = 0;
    for (; i < width && s[i]; i++) out[i] = s[i];
    for (; i < width; i++)         out[i] = ' ';
    out[i] = '\0';
}

static void dp_str(const char* s, uint8_t color) {
    terminal_setcolor(color);
    print(s);
}

static void dp_hr(void) {
    dp_str("  ------------------------------------------------------------\n", VGA_COLOR_DARK_GREY);
}

static void dp_title(const char* s) {
    print("\n");
    dp_str("  ", VGA_COLOR_DARK_GREY);
    dp_str(s, VGA_COLOR_LIGHT_CYAN);
    print("\n");
    dp_hr();
}

// "Key : value" row with aligned keys.
static void dp_kv(const char* key, const char* val) {
    char k[20];
    dp_pad(key, 15, k);
    dp_str("  ", VGA_COLOR_DARK_GREY);
    dp_str(k, VGA_COLOR_LIGHT_CYAN);
    dp_str(": ", VGA_COLOR_DARK_GREY);
    dp_str(val, VGA_COLOR_WHITE);
    print("\n");
}

static void dp_cell(const char* s, int width, uint8_t color) {
    char b[DP_STR_MAX];
    dp_pad(s, width, b);
    dp_str(b, color);
    dp_str("  ", VGA_COLOR_DARK_GREY);
}

static void dp_cell_last(const char* s, int width, uint8_t color) {
    char b[DP_STR_MAX];
    dp_pad(s, width, b);
    dp_str(b, color);
}

/* ================================================================== */
/* THE BAR SYSTEM                                                     */
/*                                                                    */
/* Renders:  [####------] 51.3% used                                  */
/* '#' cells are colored by load: green <50%, brown <80%, red >=80%;  */
/* the free track is drawn with '-' in dark grey.                     */
/* ================================================================== */
// Sizes of the disk volume come in KiB (they pass 4 GiB).
static void dp_fmt_kb(uint32_t kb, char* out) {
    if (kb >= 1024u * 1024u) {
        char n[12];
        uint32_t whole = kb / (1024u * 1024u);
        uint32_t frac  = ((kb % (1024u * 1024u)) * 10u + (1024u * 1024u) / 2u) / (1024u * 1024u);
        if (frac >= 10u) { frac -= 10u; whole++; }
        dp_utoa(whole, n); strcpy(out, n); strcat(out, ".");
        dp_utoa(frac, n);  strcat(out, n); strcat(out, " GB");
    } else {
        dp_fmt_size(kb * 1024u, out);
    }
}

static void dp_bar(uint32_t used, uint32_t total, int cells) {
    char seg[DP_BAR_CELLS + 1];
    char pct[DP_STR_MAX];

    if (cells < 1)            cells = 1;
    if (cells > DP_BAR_CELLS) cells = DP_BAR_CELLS;

    uint32_t filled;
    if (total == 0) {
        filled = 0;
    } else if (used >= total) {
        filled = (uint32_t)cells;
    } else {
        /* (used % total) == used here; used*cells < 2^32 for our volumes */
        filled = ((used % total) * (uint32_t)cells + total / 2u) / total;
        if (filled > (uint32_t)cells) filled = (uint32_t)cells;
    }

    uint8_t used_col = VGA_COLOR_LIGHT_GREEN;
    if (total == 0) {
        used_col = VGA_COLOR_DARK_GREY;
    } else {
        uint32_t p100 = (used >= total) ? 100u : (used * 100u) / total;
        if      (p100 >= 80u) used_col = VGA_COLOR_LIGHT_RED;
        else if (p100 >= 50u) used_col = VGA_COLOR_LIGHT_BROWN;
    }

    dp_str("[", VGA_COLOR_LIGHT_GREY);
    uint32_t i;
    for (i = 0; i < filled; i++) seg[i] = '#';
    seg[i] = '\0';
    dp_str(seg, used_col);
    for (i = 0; filled + i < (uint32_t)cells; i++) seg[i] = '-';
    seg[i] = '\0';
    dp_str(seg, VGA_COLOR_DARK_GREY);
    dp_str("] ", VGA_COLOR_LIGHT_GREY);

    if (total == 0) {
        dp_str("n/a\n", VGA_COLOR_DARK_GREY);
    } else {
        dp_pct_str(used, total, pct);
        dp_str(pct, VGA_COLOR_WHITE);
        dp_str(" used\n", VGA_COLOR_LIGHT_GREY);
    }
}

/* ================================================================== */
/* Text renderers                                                     */
/* ================================================================== */

static const char* dp_bus_name(uint16_t bus) {
    if (bus == 0x1F0) return "primary";
    if (bus == 0x170) return "secondary";
    return "ata?";
}

static const char* dp_drive_name(uint8_t sel) {
    if (sel == 0xE0) return "master";
    if (sel == 0xF0) return "slave";
    return "drive?";
}

static void dp_no_disk(void) {
    dp_str("  Disk 0 : OFFLINE - no ATA drive / FAT32 backend not mounted.\n", VGA_COLOR_LIGHT_RED);
    dp_str("  (the AVFS RAM volume still works - see: diskpart list volume)\n", VGA_COLOR_DARK_GREY);
}

// boot | FAT x2 | data : fixed-cell ruler, colors match the legend.
static void dp_area_layout(const avfs_disk_info_t* d) {
    char b[DP_STR_MAX];
    dp_str("  Area layout (not to scale):\n", VGA_COLOR_LIGHT_GREY);
    dp_str("  [", VGA_COLOR_LIGHT_GREY);
    dp_str("#", VGA_COLOR_LIGHT_MAGENTA);                                  /* boot   */
    for (int i = 0; i < 8; i++)  dp_str("#", VGA_COLOR_LIGHT_CYAN);        /* FAT #1 */
    for (int i = 0; i < 8; i++)  dp_str("#", VGA_COLOR_LIGHT_BLUE);        /* FAT #2 */
    for (int i = 0; i < 23; i++) dp_str("#", VGA_COLOR_GREEN);             /* data   */
    dp_str("]\n", VGA_COLOR_LIGHT_GREY);

    dp_str("  # ", VGA_COLOR_LIGHT_MAGENTA);
    dp_str("boot/reserved : ", VGA_COLOR_LIGHT_GREY);
    dp_utoa(d->reserved_sectors, b);
    strcat(b, " sector(s)");
    dp_str(b, VGA_COLOR_LIGHT_GREY);
    print("\n");

    dp_str("  # ", VGA_COLOR_LIGHT_CYAN);
    dp_str("FAT area      : ", VGA_COLOR_LIGHT_GREY);
    dp_utoa(d->fat_sectors, b);
    strcat(b, " sectors x 2 copies");
    dp_str(b, VGA_COLOR_LIGHT_GREY);
    print("\n");

    dp_str("  # ", VGA_COLOR_GREEN);
    dp_str("data area     : ", VGA_COLOR_LIGHT_GREY);
    dp_utoa(d->total_clusters * d->sectors_per_cluster, b);
    strcat(b, " sectors for clusters");
    dp_str(b, VGA_COLOR_LIGHT_GREY);
    print("\n");
}

// Defrag-style map of the whole FAT-addressable data area.
static void dp_cluster_map(uint32_t total_clusters) {
    uint8_t cells[DP_MAP_COLS * DP_MAP_ROWS];
    char row[DP_MAP_COLS + 1];
    char b[DP_STR_MAX];

    dp_str("  Cluster map", VGA_COLOR_LIGHT_GREY);
    if (total_clusters > 0) {
        uint32_t per = (total_clusters + (uint32_t)(DP_MAP_COLS * DP_MAP_ROWS) - 1u)
                     / (uint32_t)(DP_MAP_COLS * DP_MAP_ROWS);
        dp_str(" (1 cell = ", VGA_COLOR_LIGHT_GREY);
        dp_utoa(per, b);
        dp_str(b, VGA_COLOR_LIGHT_GREY);
        dp_str(" clusters)", VGA_COLOR_LIGHT_GREY);
    }
    print("\n");

    if (avfs_disk_usage_map(cells, DP_MAP_COLS * DP_MAP_ROWS) != 0) {
        dp_str("  (unavailable - disk not mounted)\n", VGA_COLOR_DARK_GREY);
        return;
    }

    for (int r = 0; r < DP_MAP_ROWS; r++) {
        dp_str("  [", VGA_COLOR_LIGHT_GREY);
        int c = 0;
        while (c < DP_MAP_COLS) {
            uint8_t v = cells[r * DP_MAP_COLS + c];
            int start = c;
            while (c < DP_MAP_COLS && cells[r * DP_MAP_COLS + c] == v) c++;
            int len = c - start;
            for (int k = 0; k < len; k++)
                row[k] = (v == 2) ? '#' : ((v == 1) ? '+' : '.');
            row[len] = '\0';
            dp_str(row, (v == 2) ? VGA_COLOR_LIGHT_GREEN
                       : ((v == 1) ? VGA_COLOR_LIGHT_BROWN
                                   : VGA_COLOR_DARK_GREY));
        }
        dp_str("]\n", VGA_COLOR_LIGHT_GREY);
    }

    dp_str("  # used   ", VGA_COLOR_LIGHT_GREEN);
    dp_str("+ partial   ", VGA_COLOR_LIGHT_BROWN);
    dp_str(". free\n", VGA_COLOR_DARK_GREY);
}

static DirectoryEntry s_dp_entries[DP_TOP_MAX];   /* static: keeps stack use low */

static void dp_top_files(void) {
    int n = avfs_list_directory("/disk", s_dp_entries, DP_TOP_MAX);
    if (n <= 0) {
        dp_str("  Largest files: (root listing unavailable)\n", VGA_COLOR_DARK_GREY);
        return;
    }
    uint32_t sizes[DP_TOP_MAX];
    uint8_t  order[DP_TOP_MAX];
    int m = 0;
    for (int i = 0; i < n && m < DP_TOP_MAX; i++) {
        if (s_dp_entries[i].name[0] == '.') continue;    /* "." and ".." */
        if (s_dp_entries[i].is_directory)      continue;
        sizes[m] = s_dp_entries[i].size;
        order[m] = (uint8_t)i;
        m++;
    }
    if (m == 0) {
        dp_str("  Largest files: (no files at /disk/ root)\n", VGA_COLOR_DARK_GREY);
        return;
    }
    for (int i = 1; i < m; i++) {                        /* insertion sort, big first */
        uint32_t ks = sizes[i];
        uint8_t  ki = order[i];
        int j = i - 1;
        while (j >= 0 && sizes[j] < ks) {
            sizes[j + 1] = sizes[j];
            order[j + 1] = order[j];
            j--;
        }
        sizes[j + 1] = ks;
        order[j + 1] = ki;
    }

    dp_str("  Largest files in /disk/:\n", VGA_COLOR_LIGHT_GREY);
    int show     = (m < DP_TOP_SHOW) ? m : DP_TOP_SHOW;
    uint32_t maxsz = sizes[0] ? sizes[0] : 1u;
    char nameb[20], sz[DP_STR_MAX], seg[21];

    for (int k = 0; k < show; k++) {
        dp_pad(s_dp_entries[order[k]].name, 15, nameb);
        dp_str("    ", VGA_COLOR_DARK_GREY);
        dp_str(nameb, VGA_COLOR_WHITE);
        uint32_t filled = (sizes[k] * 20u) / maxsz;      /* <= 20, fits 32-bit */
        if (filled > 20u) filled = 20u;
        uint32_t i2;
        for (i2 = 0; i2 < filled; i2++) seg[i2] = '#';
        seg[i2] = '\0';
        dp_str("[", VGA_COLOR_LIGHT_GREY);
        dp_str(seg, VGA_COLOR_LIGHT_CYAN);
        for (i2 = 0; filled + i2 < 20u; i2++) seg[i2] = '-';
        seg[i2] = '\0';
        dp_str(seg, VGA_COLOR_DARK_GREY);
        dp_str("] ", VGA_COLOR_LIGHT_GREY);
        dp_fmt_size(sizes[k], sz);
        dp_str(sz, VGA_COLOR_LIGHT_GREY);
        print("\n");
    }
    if (m > show) {
        char b[DP_STR_MAX];
        dp_str("    ... and ", VGA_COLOR_DARK_GREY);
        dp_utoa((uint32_t)(m - show), b);
        dp_str(b, VGA_COLOR_DARK_GREY);
        dp_str(" more\n", VGA_COLOR_DARK_GREY);
    }
}

/* ================================================================== */
/* Subcommands                                                        */
/* ================================================================== */

static void dp_cmd_list_disk(void) {
    avfs_disk_info_t d;
    bool has = (avfs_disk_query(&d) == 0);

    dp_title("DISKPART - Disks");

    dp_cell("Disk",   7, VGA_COLOR_LIGHT_CYAN);
    dp_cell("Status", 8, VGA_COLOR_LIGHT_CYAN);
    dp_cell("Size",   8, VGA_COLOR_LIGHT_CYAN);
    dp_cell("Used",   8, VGA_COLOR_LIGHT_CYAN);
    dp_cell("Free",   8, VGA_COLOR_LIGHT_CYAN);
    dp_cell("Info",  26, VGA_COLOR_LIGHT_CYAN);
    print("\n");
    dp_cell("-------",       7, VGA_COLOR_DARK_GREY);
    dp_cell("--------",      8, VGA_COLOR_DARK_GREY);
    dp_cell("--------",      8, VGA_COLOR_DARK_GREY);
    dp_cell("--------",      8, VGA_COLOR_DARK_GREY);
    dp_cell("--------",      8, VGA_COLOR_DARK_GREY);
    dp_cell("--------------------------", 26, VGA_COLOR_DARK_GREY);
    print("\n");

    char sz[DP_STR_MAX], info[DP_STR_MAX];
    if (has) {
        dp_cell("Disk 0",  7, VGA_COLOR_WHITE);
        dp_cell("Online",  8, VGA_COLOR_LIGHT_GREEN);
        dp_fmt_kb(d.total_kb, sz); dp_cell(sz, 8, VGA_COLOR_LIGHT_GREY);
        dp_fmt_kb(d.used_kb,   sz); dp_cell(sz, 8, VGA_COLOR_LIGHT_GREY);
        dp_fmt_kb(d.free_kb,   sz); dp_cell(sz, 8, VGA_COLOR_LIGHT_GREY);
        strcpy(info, "ATA ");
        strcat(info, dp_bus_name(d.ata_bus));
        strcat(info, " ");
        strcat(info, dp_drive_name(d.ata_drive));
        strcat(info, ", FAT32");
        dp_cell_last(info, 26, VGA_COLOR_LIGHT_GREY);
    } else {
        dp_cell("Disk 0",  7, VGA_COLOR_WHITE);
        dp_cell("Offline", 8, VGA_COLOR_LIGHT_RED);
        dp_cell("-", 8, VGA_COLOR_DARK_GREY);
        dp_cell("-", 8, VGA_COLOR_DARK_GREY);
        dp_cell("-", 8, VGA_COLOR_DARK_GREY);
        dp_cell_last("no ATA/FAT32 backend", 26, VGA_COLOR_LIGHT_GREY);
    }
    print("\n");

    if (has) {
        dp_str("  Disk 0 usage: ", VGA_COLOR_LIGHT_GREY);
        dp_bar(d.used_kb, d.total_kb, DP_BAR_CELLS);

        char b[DP_STR_MAX];
        dp_str("  Objects: ", VGA_COLOR_LIGHT_GREY);
        dp_utoa(d.file_count, b); dp_str(b, VGA_COLOR_WHITE);
        dp_str(" files, ",        VGA_COLOR_LIGHT_GREY);
        dp_utoa(d.dir_count,  b); dp_str(b, VGA_COLOR_WHITE);
        dp_str(" directories\n",  VGA_COLOR_LIGHT_GREY);
    }
    dp_str("  Type 'diskpart help' for commands.\n", VGA_COLOR_DARK_GREY);
    terminal_setcolor(VGA_COLOR_WHITE);
}

static void dp_cmd_list_volume(void) {
    avfs_disk_info_t d;
    bool has_disk = (avfs_disk_query(&d) == 0);
    avfs_ramfs_info_t r;
    avfs_ramfs_query(&r);

    dp_title("DISKPART - Volumes");

    char sz[DP_STR_MAX];
    dp_cell("Volume", 9,  VGA_COLOR_LIGHT_CYAN);
    dp_cell("Label",  11, VGA_COLOR_LIGHT_CYAN);
    dp_cell("FS",     5,  VGA_COLOR_LIGHT_CYAN);
    dp_cell("Size",   8,  VGA_COLOR_LIGHT_CYAN);
    dp_cell("Used",   8,  VGA_COLOR_LIGHT_CYAN);
    dp_cell("Free",   8,  VGA_COLOR_LIGHT_CYAN);
    dp_cell("Status", 8,  VGA_COLOR_LIGHT_CYAN);
    print("\n");
    dp_cell("---------",   9,  VGA_COLOR_DARK_GREY);
    dp_cell("-----------", 11, VGA_COLOR_DARK_GREY);
    dp_cell("-----",       5,  VGA_COLOR_DARK_GREY);
    dp_cell("--------",    8,  VGA_COLOR_DARK_GREY);
    dp_cell("--------",    8,  VGA_COLOR_DARK_GREY);
    dp_cell("--------",    8,  VGA_COLOR_DARK_GREY);
    dp_cell("--------",    8,  VGA_COLOR_DARK_GREY);
    print("\n");

    /* Volume 0: the AVFS RAM filesystem */
    dp_cell("Volume 0",  9,  VGA_COLOR_WHITE);
    dp_cell("(volatile)", 11, VGA_COLOR_LIGHT_GREY);
    dp_cell("AVFS",       5,  VGA_COLOR_LIGHT_GREY);
    dp_fmt_size(r.total_bytes, sz); dp_cell(sz, 8, VGA_COLOR_LIGHT_GREY);
    dp_fmt_size(r.used_bytes,  sz); dp_cell(sz, 8, VGA_COLOR_LIGHT_GREY);
    dp_fmt_size(r.free_bytes,  sz); dp_cell(sz, 8, VGA_COLOR_LIGHT_GREY);
    dp_cell("Ready", 8, VGA_COLOR_LIGHT_GREEN);
    print("\n");

    /* Volume 1: the persistent FAT32 /disk/ */
    if (has_disk) {
        dp_cell("Volume 1", 9, VGA_COLOR_WHITE);
        dp_cell(d.volume_label, 11, VGA_COLOR_WHITE);
        dp_cell("FAT32", 5, VGA_COLOR_LIGHT_GREY);
        dp_fmt_kb(d.total_kb, sz); dp_cell(sz, 8, VGA_COLOR_LIGHT_GREY);
        dp_fmt_kb(d.used_kb,   sz); dp_cell(sz, 8, VGA_COLOR_LIGHT_GREY);
        dp_fmt_kb(d.free_kb,   sz); dp_cell(sz, 8, VGA_COLOR_LIGHT_GREY);
        dp_cell("Online", 8, VGA_COLOR_LIGHT_GREEN);
    } else {
        dp_cell("Volume 1", 9, VGA_COLOR_WHITE);
        dp_cell("-", 11, VGA_COLOR_DARK_GREY);
        dp_cell("FAT32", 5, VGA_COLOR_LIGHT_GREY);
        dp_cell("-", 8, VGA_COLOR_DARK_GREY);
        dp_cell("-", 8, VGA_COLOR_DARK_GREY);
        dp_cell("-", 8, VGA_COLOR_DARK_GREY);
        dp_cell("Offline", 8, VGA_COLOR_LIGHT_RED);
    }
    print("\n");

    dp_str("  Vol 0 (RAMFS)  ", VGA_COLOR_LIGHT_GREY);
    dp_bar(r.used_bytes, r.total_bytes, DP_BAR_CELLS);
    dp_str("  Vol 1 (DISK)   ", VGA_COLOR_LIGHT_GREY);
    if (has_disk) dp_bar(d.used_kb, d.total_kb, DP_BAR_CELLS);
    else          dp_str("[offline]\n", VGA_COLOR_LIGHT_RED);

    terminal_setcolor(VGA_COLOR_WHITE);
}

static void dp_cmd_detail(void) {
    avfs_disk_info_t d;
    if (avfs_disk_query(&d) != 0) {
        dp_title("DISKPART - Detail: Disk 0");
        dp_no_disk();
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    dp_title("DISKPART - Detail: Disk 0");

    char b[DP_STR_MAX], b2[DP_STR_MAX];

    dp_kv("Filesystem", "FAT32");
    dp_kv("Volume label", d.volume_label);

    strcpy(b, "ATA ");
    strcat(b, dp_bus_name(d.ata_bus));
    strcat(b, " ");
    strcat(b, dp_drive_name(d.ata_drive));
    strcat(b, " (PIO)");
    dp_kv("Backend", b);

    dp_fmt_size(d.sector_size, b);
    dp_kv("Sector size", b);

    dp_fmt_size(d.cluster_size, b);
    dp_utoa(d.sectors_per_cluster, b2);
    strcat(b, " ("); strcat(b, b2); strcat(b, " sectors)");
    dp_kv("Cluster size", b);

    dp_utoa(d.total_sectors, b);
    strcat(b, " sectors (");
    dp_utoa(d.total_sectors / 2048u, b2);    /* sectors * 512B / 1MiB */
    strcat(b, b2); strcat(b, " MiB raw)");
    dp_kv("Device size", b);

    dp_utoa(d.total_clusters, b);
    strcat(b, " clusters (");
    dp_fmt_kb(d.total_kb, b2);
    strcat(b, b2); strcat(b, " usable)");
    dp_kv("Data area", b);

    dp_utoa(d.reserved_sectors, b);
    strcat(b, " boot + ");
    dp_utoa(d.fat_sectors, b2);
    strcat(b, b2); strcat(b, " x 2 FAT, data @ LBA ");
    dp_utoa(d.data_start_lba, b2);
    strcat(b, b2);
    dp_kv("Layout", b);

    print("\n");

    dp_str("  Usage: ", VGA_COLOR_LIGHT_GREY);
    dp_bar(d.used_kb, d.total_kb, DP_BAR_CELLS);

    dp_str("  Objects: ", VGA_COLOR_LIGHT_GREY);
    dp_utoa(d.file_count, b); dp_str(b, VGA_COLOR_WHITE);
    dp_str(" files, ",        VGA_COLOR_LIGHT_GREY);
    dp_utoa(d.dir_count,  b); dp_str(b, VGA_COLOR_WHITE);
    dp_str(" directories\n",  VGA_COLOR_LIGHT_GREY);
    print("\n");

    dp_area_layout(&d);
    print("\n");
    dp_cluster_map(d.total_clusters);
    print("\n");
    dp_top_files();
    terminal_setcolor(VGA_COLOR_WHITE);
}

static void dp_cmd_select(int argc, char* argv[]) {
    if (argc < 4 || dp_streq(argv[2], "disk") != 0) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("diskpart: usage - diskpart select disk <n>\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    const char* p = argv[3];
    if (*p == '\0') {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("diskpart: usage - diskpart select disk <n>\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    uint32_t n = 0;
    while (*p >= '0' && *p <= '9') { n = n * 10u + (uint32_t)(*p - '0'); p++; }
    if (*p != '\0') {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("diskpart: '"); print(argv[3]); print("' is not a disk number.\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    if (n != 0) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("diskpart: no such disk. This system has only Disk 0.\n");
    } else {
        terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
        print("diskpart: Disk 0 is now the selected disk.\n");
        terminal_setcolor(VGA_COLOR_DARK_GREY);
        print("  (single-disk system: selection is always Disk 0)\n");
    }
    terminal_setcolor(VGA_COLOR_WHITE);
}

static void dp_cmd_wipe(int argc, char* argv[], const char* verb) {
    bool confirmed = false;

    for (int i = 2; i < argc; i++) {
        if (dp_streq(argv[i], "--confirm") == 0 || dp_streq(argv[i], "-y") == 0) {
            confirmed = true;
        } else if (dp_streq(argv[i], "disk") == 0 || dp_streq(argv[i], "0") == 0) {
            /* tolerated filler, e.g. "diskpart clean disk 0" */
        } else {
            terminal_setcolor(VGA_COLOR_LIGHT_RED);
            print("diskpart: unknown option '");
            print(argv[i]);
            print("'\n");
            terminal_setcolor(VGA_COLOR_BROWN);
            print("Usage: diskpart "); print(verb); print(" --confirm   (or: diskpart ");
            print(verb); print(" -y)\n");
            terminal_setcolor(VGA_COLOR_WHITE);
            return;
        }
    }

    if (!confirmed) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("diskpart: "); print(verb);
        print(" requires explicit confirmation.\n");
        terminal_setcolor(VGA_COLOR_BROWN);
        print("Usage: diskpart "); print(verb); print(" --confirm   (or: diskpart ");
        print(verb); print(" -y)\n");
        print("This erases EVERY file on /disk/ and reformats it as FAT32.\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }

    avfs_disk_info_t d;
    if (avfs_disk_query(&d) != 0) {
        dp_str("  No mounted FAT32 disk - nothing to wipe.\n", VGA_COLOR_LIGHT_RED);
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    dp_str("  About to erase: ", VGA_COLOR_LIGHT_GREY);
    dp_bar(d.used_kb, d.total_kb, DP_BAR_CELLS);
    dp_str("  Wiping /disk/ filesystem (FAT32) ...\n", VGA_COLOR_LIGHT_RED);

    int rc = avfs_format_disk();
    if (rc == 0) {
        dp_str("  Success: /disk/ has been wiped and reformatted.\n", VGA_COLOR_LIGHT_GREEN);
        if (avfs_disk_query(&d) == 0) {
            dp_str("  Post-wipe verification: ", VGA_COLOR_LIGHT_GREY);
            dp_bar(d.used_kb, d.total_kb, DP_BAR_CELLS);
        }
    } else {
        char b[DP_STR_MAX];
        dp_str("  wipe failed - avfs_format_disk() returned ", VGA_COLOR_LIGHT_RED);
        dp_utoa((uint32_t)rc, b);
        dp_str(b, VGA_COLOR_LIGHT_RED);
        print("\n");
    }
    terminal_setcolor(VGA_COLOR_WHITE);
}

static void dp_helpline(const char* cmd, const char* desc) {
    char b[40];
    dp_str("  ", VGA_COLOR_DARK_GREY);
    dp_pad(cmd, 26, b);
    dp_str(b, VGA_COLOR_LIGHT_CYAN);
    dp_str(desc, VGA_COLOR_LIGHT_GREY);
    print("\n");
}

static void dp_cmd_help(void) {
    dp_title("DISKPART - Commands");
    dp_helpline("diskpart",                 "disk 0 overview + usage bar");
    dp_helpline("diskpart list disk",       "list disks (table + usage bar)");
    dp_helpline("diskpart list volume",     "list volumes (RAMFS + FAT32)");
    dp_helpline("diskpart detail disk",     "layout, cluster map, top files");
    dp_helpline("diskpart select disk <n>", "select a disk (only disk 0)");
    dp_helpline("diskpart wipe --confirm",  "wipe + reformat /disk/ (destructive)");
    dp_helpline("diskpart clean --confirm", "alias of wipe (diskpart spelling)");
    dp_helpline("diskpart help",            "show this help");
    print("\n");
    dp_str("  Confirmation flags: --confirm or -y\n", VGA_COLOR_LIGHT_GREY);
    dp_str("  WARNING: wipe/clean erase ALL data on /disk/.\n", VGA_COLOR_LIGHT_RED);
    terminal_setcolor(VGA_COLOR_WHITE);
}

/* ================================================================== */
/* Entry point                                                        */
/* ================================================================== */

void diskpart_command(int argc, char* argv[])
{
    const char* sub = (argc >= 2) ? argv[1] : "";

    if (argc < 2) {
        dp_cmd_list_disk();                 /* bare `diskpart` = overview */
        return;
    }

    if (dp_streq(sub, "list") == 0) {
        const char* what = (argc >= 3) ? argv[2] : "disk";
        if (dp_streq(what, "volume") == 0 || dp_streq(what, "volumes") == 0 ||
            dp_streq(what, "vol") == 0)
            dp_cmd_list_volume();
        else if (dp_streq(what, "disk") == 0 || dp_streq(what, "disks") == 0)
            dp_cmd_list_disk();
        else {
            terminal_setcolor(VGA_COLOR_LIGHT_RED);
            print("diskpart: unknown list target '"); print(what); print("'\n");
            terminal_setcolor(VGA_COLOR_BROWN);
            print("Try: diskpart list disk  |  diskpart list volume\n");
            terminal_setcolor(VGA_COLOR_WHITE);
        }
        return;
    }

    if (dp_streq(sub, "detail") == 0 || dp_streq(sub, "info") == 0) {
        dp_cmd_detail();                    /* tolerates a trailing "disk" */
        return;
    }

    if (dp_streq(sub, "select") == 0) { dp_cmd_select(argc, argv); return; }

    if (dp_streq(sub, "wipe")  == 0) { dp_cmd_wipe(argc, argv, "wipe");  return; }
    if (dp_streq(sub, "clean") == 0) { dp_cmd_wipe(argc, argv, "clean"); return; }

    if (dp_streq(sub, "help") == 0 || dp_streq(sub, "--help") == 0 ||
        dp_streq(sub, "-h") == 0     || dp_streq(sub, "/?") == 0) {
        dp_cmd_help();
        return;
    }

    if (dp_streq(sub, "exit") == 0 || dp_streq(sub, "quit") == 0) {
        dp_str("  diskpart runs one-shot in this shell - nothing to exit.\n", VGA_COLOR_DARK_GREY);
        return;
    }

    terminal_setcolor(VGA_COLOR_LIGHT_RED);
    print("diskpart: unknown command '"); print(sub); print("'\n");
    terminal_setcolor(VGA_COLOR_BROWN);
    print("Type 'diskpart help' to list available commands.\n");
    terminal_setcolor(VGA_COLOR_WHITE);
}

/* Optional legacy alias: register { "wipe", wipe_command_compat } in the
 * shell command table so the old `wipe [--confirm|-y]` syntax keeps
 * working on top of the new diskpart implementation.                   */
void wipe_command_compat(int argc, char* argv[])
{
    const char* nargv[16];
    int n = 0;
    nargv[n++] = "diskpart";
    nargv[n++] = "wipe";
    for (int i = 1; i < argc && n < 15; i++) nargv[n++] = argv[i];
    diskpart_command(n, (char**)nargv);
}
