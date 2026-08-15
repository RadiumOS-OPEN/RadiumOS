#include "../Avfs/Avfs.h"
#include "../terminal/terminal.h"
#include "../keyboard/keyboard.h"
#include "../utility/utility.h"
#include "../io/io.h"
#include <stdint.h>

// ============================================================
// RadiumGeiger v2 - interactive byte inspector for RadiumOS
// Author: scp_2801
//
// Run `geiger -m` for the full feature index.
// Run `geiger -m <feature>` for a single feature's manual page.
// ============================================================

extern const char keyboard_map[128];
extern const char shifted_keyboard_map[128];

// If avfs_truncate doesn't exist in your AVFS, tell me and I'll implement
// --truncate by read-full/rewrite instead (slower, but needs no new AVFS call).

// ============================================================
// SECTION 0: shared formatting primitives
// ============================================================

static void hex2(unsigned char v, char* out) {
    out[0] = "0123456789ABCDEF"[(v >> 4) & 0xF];
    out[1] = "0123456789ABCDEF"[v & 0xF];
    out[2] = '\0';
}
static void hex8(unsigned int v, char* out) {
    for (int i = 7; i >= 0; i--) { out[i] = "0123456789ABCDEF"[v & 0xF]; v >>= 4; }
    out[8] = '\0';
}
static void dec_u32(unsigned int v, char* out) {
    char tmp[11]; int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = '\0'; return; }
    while (v > 0) { tmp[i++] = '0' + (v % 10); v /= 10; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = '\0';
}
static void dec_i32(int v, char* out) {
    unsigned int uv; int p = 0;
    if (v < 0) { out[p++] = '-'; uv = (unsigned int)(-v); } else uv = (unsigned int)v;
    dec_u32(uv, out + p);
}
static void bin8(unsigned char v, char* out) {
    for (int i = 7; i >= 0; i--) out[7 - i] = ((v >> i) & 1) ? '1' : '0';
    out[8] = '\0';
}
static int str_to_int(const char* s) {
    int neg = 0, v = 0, i = 0;
    if (s[0] == '-') { neg = 1; i = 1; }
    if (s[i] == '0' && (s[i+1] == 'x' || s[i+1] == 'X')) {
        i += 2;
        while (s[i]) {
            char c = s[i];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = v * 16 + d;
            i++;
        }
        return neg ? -v : v;
    }
    while (s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
    return neg ? -v : v;
}
static int str_to_byte(const char* s) {
    int v = str_to_int(s);
    return v & 0xFF;
}

// ============================================================
// SECTION 1: MAN PAGE SYSTEM  (feature #41)
// ============================================================

typedef struct {
    const char* name;
    const char* usage;
    const char* desc;
    const char* example;
} ManEntry;

static const ManEntry man_pages[] = {
    { "dump",            "geiger <file>",
      "Default mode. Full hex+ASCII dump of the whole file, streamed in\n"
      "512-byte chunks so file size is unbounded.",
      "geiger boot.log" },
    { "interactive",     "geiger -i|--interactive <file>",
      "Full-screen live byte inspector. Arrow keys move the cursor,\n"
      "Shift+Arrow extends a selection, PgUp/PgDn pages, Home/End jumps\n"
      "to file start/end. Live description panel updates per byte.\n"
      "Extra keys inside: 1-9 set/jump bookmark slots, 's' inline search,\n"
      "'n' find next match, 'g' goto offset, Esc/Q quit.",
      "geiger -i kernel.bin" },
    { "man",              "geiger -m|--man [feature]",
      "Show this manual. With no argument, lists all feature names.\n"
      "With a name, shows that feature's full page.",
      "geiger -m entropy" },
    { "offset",           "geiger -o|--offset N <file>",
      "Start the dump at byte offset N instead of 0. Accepts decimal\n"
      "or 0x-prefixed hex.",
      "geiger -o 0x100 firmware.bin" },
    { "length",           "geiger -l|--length N <file>",
      "Limit the dump to N bytes from the start offset.",
      "geiger -o 0 -l 64 firmware.bin" },
    { "width",            "geiger -w|--width N <file>",
      "Bytes shown per row. Common values: 8, 16, 32. Default 16.",
      "geiger -w 8 small.dat" },
    { "group",            "geiger -g|--group N <file>",
      "Insert an extra space every N hex bytes for readability.\n"
      "Default 8 (matches the classic xxd look).",
      "geiger -g 4 small.dat" },
    { "color",            "geiger -c|--color off <file>",
      "Disables highlight coloring in interactive mode (plain mono).\n"
      "Useful over serial-redirected sessions.",
      "geiger -c off -i file.bin" },
    { "ascii-only",       "geiger -a|--ascii-only <file>",
      "Dump only the ASCII column, no hex bytes. Non-printables shown\n"
      "as '.' Good for eyeballing text embedded in binary blobs.",
      "geiger -a config.dat" },
    { "hex-only",         "geiger -x|--hex-only <file>",
      "Dump only hex bytes, no ASCII column.",
      "geiger -x config.dat" },
    { "reverse",          "geiger -r|--reverse <file>",
      "Dump the file from end to start.",
      "geiger -r log.bin" },
    { "search",           "geiger -s|--search STRING <file>",
      "Find every occurrence of an ASCII string, print offsets and a\n"
      "one-line hex+ascii preview centered on each hit.",
      "geiger -s \"PASSWORD\" dump.bin" },
    { "search-hex",       "geiger -sh|--search-hex \"AA BB CC\" <file>",
      "Find every occurrence of a raw byte pattern given as space\n"
      "separated hex pairs.",
      "geiger -sh \"4D 5A\" firmware.bin" },
    { "search-regex",     "geiger -sr|--search-regex \"AA ?? CC\" <file>",
      "Byte-pattern search with '??' as a single-byte wildcard.",
      "geiger -sr \"7F 45 4C 46 ??\" elf.bin" },
    { "strings",          "geiger --strings <file>",
      "Extract runs of printable ASCII (default min length 4), one per\n"
      "line with its starting offset. Same idea as unix `strings`.",
      "geiger --strings unknown.bin" },
    { "strings-min",      "geiger --strings-min N <file>",
      "Set the minimum run length for --strings. Must be used together\n"
      "with --strings.",
      "geiger --strings --strings-min 8 unknown.bin" },
    { "unicode-strings",  "geiger --unicode-strings <file>",
      "Like --strings but scans for UTF-16LE printable runs (every\n"
      "other byte zero, ASCII range).",
      "geiger --unicode-strings notes.bin" },
    { "diff",             "geiger -d|--diff FILE2 <file>",
      "Byte-level diff against a second file. Prints offsets where the\n"
      "two files differ plus both byte values. Stops after 512 diffs.",
      "geiger -d backup.img live.img" },
    { "diff-blocks",      "geiger --diff-blocks FILE2 <file>",
      "Like --diff but collapses runs of consecutive differing bytes\n"
      "into ranges instead of listing every byte.",
      "geiger --diff-blocks v1.bin v2.bin" },
    { "compare-region",   "geiger --compare-region FILE2 OFF1 OFF2 LEN <file>",
      "Compare LEN bytes starting at OFF1 in <file> against LEN bytes\n"
      "starting at OFF2 in FILE2. Reports match/mismatch and first\n"
      "diverging byte if any.",
      "geiger --compare-region old.bin 0 0x400 256 new.bin" },
    { "patch",            "geiger -p|--patch OFFSET=VALUE <file>",
      "Overwrite a single byte at OFFSET with VALUE (hex or decimal).\n"
      "Writes directly to the file - no undo.",
      "geiger -p 0x10=0xEB boot.bin" },
    { "patch-range",      "geiger --patch-range OFFSET \"AA BB CC\" <file>",
      "Overwrite a run of bytes starting at OFFSET with the given hex\n"
      "byte sequence.",
      "geiger --patch-range 0x200 \"90 90 90 90\" boot.bin" },
    { "fill",             "geiger --fill OFFSET LEN BYTE <file>",
      "Fill LEN bytes starting at OFFSET with a repeated BYTE value.",
      "geiger --fill 0x1000 256 0x00 scratch.bin" },
    { "xor",              "geiger --xor KEYBYTE <file>",
      "XOR every byte in the file with KEYBYTE, in place. Running it\n"
      "twice with the same key restores the original.",
      "geiger --xor 0xAA payload.bin" },
    { "rot13",             "geiger --rot13 <file>",
      "ROT13-transforms alphabetic ASCII bytes in place, non-letters\n"
      "untouched. Symmetric - run twice to undo.",
      "geiger --rot13 notes.txt" },
    { "crc32",            "geiger --checksum crc32 <file>",
      "Computes the standard CRC32 (poly 0xEDB88320) over the whole\n"
      "file, streamed in chunks.",
      "geiger --checksum crc32 image.bin" },
    { "sum8",             "geiger --checksum sum8 <file>",
      "8-bit additive checksum, all bytes summed mod 256.",
      "geiger --checksum sum8 image.bin" },
    { "sum16",            "geiger --checksum sum16 <file>",
      "16-bit additive checksum, all bytes summed mod 65536.",
      "geiger --checksum sum16 image.bin" },
    { "rhash",            "geiger --checksum rhash <file>",
      "RadiumOS's own 32-bit non-cryptographic hash (FNV-1a variant).\n"
      "Not a security hash - use for quick file-identity comparison\n"
      "only, e.g. spotting duplicate build artifacts.",
      "geiger --checksum rhash image.bin" },
    { "entropy",          "geiger --entropy <file>",
      "Estimates Shannon entropy (bits/byte, 0-8) using an integer\n"
      "fixed-point log2 APPROXIMATION (no FPU dependency). High values\n"
      "(~7.5-8) suggest compressed or encrypted data; low values\n"
      "suggest structured/repetitive data. Not exact - estimate only.",
      "geiger --entropy suspicious.bin" },
    { "histogram",        "geiger --histogram <file>",
      "ASCII bar chart of byte-value frequency across the whole file,\n"
      "16 buckets (0x00-0x0F, 0x10-0x1F, ... 0xF0-0xFF).",
      "geiger --histogram image.bin" },
    { "stats",            "geiger --stats <file>",
      "Quick summary: file size, printable-byte %, null-byte %, and\n"
      "the single most common byte value.",
      "geiger --stats config.dat" },
    { "magic",            "geiger --magic <file>",
      "Detects common file types by leading magic bytes (ELF, BMP,\n"
      "PNG, GIF, GZIP, ZIP, MZ/PE, RIFF/WAV, Ogg). Reports 'unknown'\n"
      "if nothing matches.",
      "geiger --magic mystery.bin" },
    { "export-c",         "geiger --export c <file>",
      "Prints the file as a C `unsigned char[]` array literal you can\n"
      "paste straight into a RadiumOS source file.",
      "geiger --export c logo.bmp" },
    { "export-rsh",       "geiger --export rsh <file>",
      "Prints the file as an RSH byte-array literal for use in RSH\n"
      "scripts.",
      "geiger --export rsh logo.bmp" },
    { "export-base64",    "geiger --export base64 <file>",
      "Prints the file as a base64 text block to stdout.",
      "geiger --export base64 logo.bmp" },
    { "import-base64",    "geiger --import-base64 OUTFILE <file>",
      "Reads <file> as base64 TEXT and writes the decoded binary to\n"
      "OUTFILE.",
      "geiger --import-base64 logo.bmp logo.b64.txt" },
    { "bookmark-add",     "geiger --bookmark add NAME OFFSET <file>",
      "Saves a named offset bookmark for this file. Bookmarks persist\n"
      "in a sidecar file '<file>.rxbm' via AVFS.",
      "geiger --bookmark add header 0x0 elf.bin" },
    { "bookmark-list",    "geiger --bookmark list <file>",
      "Lists all saved bookmarks for this file.",
      "geiger --bookmark list elf.bin" },
    { "bookmark-goto",    "geiger --bookmark goto NAME <file>",
      "Prints the offset stored under NAME and dumps 64 bytes there.",
      "geiger --bookmark goto header elf.bin" },
    { "goto",             "geiger --goto OFFSET <file>",
      "Dumps 128 bytes centered on OFFSET. In -i mode, sets the\n"
      "starting cursor position instead of 0.",
      "geiger --goto 0x2000 firmware.bin" },
    { "annotate",         "geiger --annotate OFFSET \"note text\" <file>",
      "Attaches a text note to a byte offset, persisted in\n"
      "'<file>.rxan' via AVFS.",
      "geiger --annotate 0x40 \"start of section header\" elf.bin" },
    { "annotate-list",    "geiger --annotate-list <file>",
      "Lists all saved annotations for this file, sorted by offset.",
      "geiger --annotate-list elf.bin" },
    { "extract",          "geiger --extract OFFSET LEN OUTFILE <file>",
      "Carves LEN bytes starting at OFFSET out of <file> into a new\n"
      "file OUTFILE.",
      "geiger --extract 0x36 40960 icon.bmp resource.pak" },
    { "truncate",         "geiger --truncate LEN <file>",
      "Truncates the file to LEN bytes. Destructive, no confirmation -\n"
      "this is a low-level tool, not a shell rm-guard.",
      "geiger --truncate 1024 scratch.bin" },
    { "append-hex",       "geiger --append-hex \"AA BB CC\" <file>",
      "Appends raw bytes given as hex pairs to the end of the file.",
      "geiger --append-hex \"00 00 FF FF\" scratch.bin" },
    { "zero-fill",        "geiger --zero-fill LEN <file>",
      "Appends LEN zero bytes to the end of the file.",
      "geiger --zero-fill 512 scratch.bin" },
    { "watch",            "geiger --watch <file>",
      "Interactive mode that polls the file's size once per loop\n"
      "iteration and refreshes the view if it changes - useful for\n"
      "watching a file another RSH script or task is actively writing.\n"
      "Still a busy-poll loop on the calling core, same tradeoff as\n"
      "keyboard_input().",
      "geiger --watch /var/log/live.bin" },
    { "no-stamp",         "geiger --no-stamp <file>",
      "Skips appending the RadiumGeiger signature footer for this run.",
      "geiger --no-stamp -i readonly_reference.bin" },
    { "stamp-custom",     "geiger --stamp-custom \"text\" <file>",
      "Uses custom text instead of the default 'observed by\n"
      "RadiumGeiger' footer, still written as a HEX line.",
      "geiger --stamp-custom \"reviewed by scp_2801\" file.bin" },
};
#define MAN_COUNT (sizeof(man_pages) / sizeof(man_pages[0]))

static void print_man(const char* feature) {
    if (!feature) {
        printr("RadiumGeiger -- feature index (");
        char cnt[8]; dec_u32(MAN_COUNT, cnt); printr(cnt);
        printr(" features)\n");
        printr("Run 'geiger -m <name>' for details on any entry.\n\n");
        for (unsigned int i = 0; i < MAN_COUNT; i++) {
            printr("  ");
            printr(man_pages[i].name);
            printr("\n");
        }
        return;
    }
    for (unsigned int i = 0; i < MAN_COUNT; i++) {
        if (strcmp(man_pages[i].name, feature) == 0) {
            printr("=== "); printr(man_pages[i].name); printr(" ===\n");
            printr("usage:   "); printr(man_pages[i].usage);  printr("\n\n");
            printr(man_pages[i].desc); printr("\n\n");
            printr("example: "); printr(man_pages[i].example); printr("\n");
            return;
        }
    }
    printr("No such feature: ");
    printr(feature);
    printr("\nRun 'geiger -m' with no argument to list all features.\n");
}

// ============================================================
// SECTION 2: signature stamp footer (features #48, #49)
// ============================================================

static const char DEFAULT_SIGNATURE[] = "observed by RadiumGeiger";

static void stamp_file_signature(const char* filename, const char* custom_text, int skip) {
    if (skip) return;
    int fsize = avfs_get_filesize(filename);
    if (fsize < 0) return;

    const char* text = custom_text ? custom_text : DEFAULT_SIGNATURE;
    static char hexbuf[512];
    int pos = 0;
    hexbuf[pos++] = '\n';

    int len = 0;
    while (text[len]) len++;
    for (int i = 0; i < len && pos < (int)sizeof(hexbuf) - 4; i++) {
        unsigned char c = (unsigned char)text[i];
        hexbuf[pos++] = "0123456789ABCDEF"[(c >> 4) & 0xF];
        hexbuf[pos++] = "0123456789ABCDEF"[c & 0xF];
        if (i != len - 1) hexbuf[pos++] = ' ';
    }
    hexbuf[pos++] = '\n';

    avfs_write_file(filename, hexbuf, pos, fsize);
}

// ============================================================
// SECTION 3: generalized dump engine
// backs: dump, offset, length, width, group, ascii-only,
//        hex-only, reverse, goto, and search highlight previews
// ============================================================

#define DUMP_CHUNK 512

typedef struct {
    int start;
    int length;      // -1 = to end of file
    int width;        // bytes per row
    int group;         // extra space every N bytes
    int ascii_only;
    int hex_only;
    int reverse;
    int highlight_lo;   // -1 = none
    int highlight_hi;
} DumpOpts;

static DumpOpts default_dump_opts(void) {
    DumpOpts o;
    o.start = 0; o.length = -1; o.width = 16; o.group = 8;
    o.ascii_only = 0; o.hex_only = 0; o.reverse = 0;
    o.highlight_lo = -1; o.highlight_hi = -1;
    return o;
}

static void dump_range(const char* filename, int filesize, DumpOpts opt) {
    if (opt.width < 1) opt.width = 16;
    if (opt.width > 64) opt.width = 64;

    int start = opt.start;
    if (start < 0) start = 0;
    if (start >= filesize) { printr("(offset beyond end of file)\n"); return; }

    int length = opt.length;
    if (length < 0 || start + length > filesize) length = filesize - start;

    if (!opt.hex_only) {
        printr("Offset    ");
        for (int i = 0; i < opt.width; i++) { print_hex_byte(i); printr(" "); if (opt.group && (i + 1) % opt.group == 0) printr(" "); }
        if (!opt.ascii_only) printr(" ASCII");
        printr("\n");
    }

    static char buf[DUMP_CHUNK];
    char ascii_row[65];

    if (!opt.reverse) {
        int offset = start;
        int remaining = length;
        int col = 0;
        int row_start_off = start;
        while (remaining > 0) {
            int to_read = remaining;
            if (to_read > DUMP_CHUNK) to_read = DUMP_CHUNK;
            int res = avfs_read_file(filename, buf, to_read, offset);
            if (res < 0) { printr("Error: Failed to read file\n"); return; }

            for (int i = 0; i < to_read; i++) {
                int g = offset + i;
                if (col == 0) {
                    row_start_off = g;
                    if (!opt.ascii_only) { char o[9]; hex8((unsigned int)g, o); printr(o); printr("  "); }
                }
                unsigned char byte = (unsigned char)buf[i];
                int hl = (opt.highlight_lo >= 0 && g >= opt.highlight_lo && g <= opt.highlight_hi);

                if (!opt.ascii_only) {
                    if (hl) printr("[");
                    print_hex_byte(byte);
                    if (hl) printr("]"); else printr(" ");
                    if (opt.group && (col + 1) % opt.group == 0) printr(" ");
                }
                ascii_row[col] = (byte >= 0x20 && byte < 0x7F) ? (char)byte : '.';
                col++;

                if (col == opt.width || g == start + length - 1) {
                    ascii_row[col] = '\0';
                    if (!opt.hex_only) {
                        if (!opt.ascii_only) printr(" ");
                        printr(ascii_row);
                    }
                    printr("\n");
                    col = 0;
                }
            }
            offset += to_read;
            remaining -= to_read;
        }
    } else {
        // reverse: walk backward from start+length-1 down to start, still
        // rendered left-to-right per row for readability
        int last = start + length - 1;
        int rows = (length + opt.width - 1) / opt.width;
        for (int r = rows - 1; r >= 0; r--) {
            int row_off = start + r * opt.width;
            int row_len = opt.width;
            if (row_off + row_len > start + length) row_len = start + length - row_off;
            int res = avfs_read_file(filename, buf, row_len, row_off);
            if (res < 0) { printr("Error: Failed to read file\n"); return; }
            if (!opt.ascii_only) { char o[9]; hex8((unsigned int)row_off, o); printr(o); printr("  "); }
            for (int i = row_len - 1; i >= 0; i--) {
                unsigned char byte = (unsigned char)buf[i];
                if (!opt.ascii_only) { print_hex_byte(byte); printr(" "); }
                ascii_row[row_len - 1 - i] = (byte >= 0x20 && byte < 0x7F) ? (char)byte : '.';
            }
            ascii_row[row_len] = '\0';
            if (!opt.hex_only) { if (!opt.ascii_only) printr(" "); printr(ascii_row); }
            printr("\n");
        }
        (void)last;
    }
    printr("\n");
}

// ============================================================
// SECTION 4: search (features #11-14)
// ============================================================

static void print_hit_preview(const char* filename, int hit_off, int hit_len, int filesize) {
    int ctx_start = hit_off - 8;
    if (ctx_start < 0) ctx_start = 0;
    int ctx_len = 32;
    if (ctx_start + ctx_len > filesize) ctx_len = filesize - ctx_start;

    static char buf[64];
    avfs_read_file(filename, buf, ctx_len, ctx_start);

    char off[9]; hex8((unsigned int)hit_off, off);
    printr("  0x"); printr(off); printr(": ");
    for (int i = 0; i < ctx_len; i++) {
        int g = ctx_start + i;
        int hl = (g >= hit_off && g < hit_off + hit_len);
        if (hl) printr("[");
        print_hex_byte((unsigned char)buf[i]);
        if (hl) printr("]"); else printr(" ");
    }
    printr("  \"");
    for (int i = 0; i < ctx_len; i++) {
        unsigned char b = (unsigned char)buf[i];
        char c = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        char cs[2] = { c, '\0' };
        printr(cs);
    }
    printr("\"\n");
}

static void search_ascii(const char* filename, int filesize, const char* needle) {
    int nlen = 0;
    while (needle[nlen]) nlen++;
    if (nlen == 0) { printr("empty search string\n"); return; }

    static char buf[DUMP_CHUNK + 64];
    int overlap = nlen - 1;
    int offset = 0;
    int hits = 0;

    while (offset < filesize) {
        int to_read = filesize - offset;
        if (to_read > DUMP_CHUNK) to_read = DUMP_CHUNK;
        int res = avfs_read_file(filename, buf, to_read, offset);
        if (res < 0) break;

        for (int i = 0; i <= to_read - nlen; i++) {
            int match = 1;
            for (int j = 0; j < nlen; j++) if (buf[i + j] != needle[j]) { match = 0; break; }
            if (match) {
                print_hit_preview(filename, offset + i, nlen, filesize);
                hits++;
            }
        }
        if (to_read < DUMP_CHUNK || offset + to_read >= filesize) break;
        offset += (to_read - overlap);
    }
    char cnt[11]; dec_u32(hits, cnt);
    printr("Total matches: "); printr(cnt); printr("\n");
}

// pattern: array of ints, -1 = wildcard byte, else 0-255
static void search_pattern(const char* filename, int filesize, int* pattern, int plen) {
    static char buf[DUMP_CHUNK + 64];
    int overlap = plen - 1;
    int offset = 0;
    int hits = 0;

    while (offset < filesize) {
        int to_read = filesize - offset;
        if (to_read > DUMP_CHUNK) to_read = DUMP_CHUNK;
        int res = avfs_read_file(filename, buf, to_read, offset);
        if (res < 0) break;

        for (int i = 0; i <= to_read - plen; i++) {
            int match = 1;
            for (int j = 0; j < plen; j++) {
                if (pattern[j] != -1 && (unsigned char)buf[i + j] != pattern[j]) { match = 0; break; }
            }
            if (match) {
                print_hit_preview(filename, offset + i, plen, filesize);
                hits++;
            }
        }
        if (to_read < DUMP_CHUNK || offset + to_read >= filesize) break;
        offset += (to_read - overlap);
    }
    char cnt[11]; dec_u32(hits, cnt);
    printr("Total matches: "); printr(cnt); printr("\n");
}

// parses "AA BB CC" / "AA ?? CC" into pattern array, returns length
static int parse_hex_pattern(const char* s, int* out, int maxlen) {
    int n = 0;
    int i = 0;
    while (s[i] && n < maxlen) {
        while (s[i] == ' ') i++;
        if (!s[i]) break;
        if (s[i] == '?' && s[i+1] == '?') { out[n++] = -1; i += 2; continue; }
        int hi = s[i], lo = s[i+1];
        int hv = (hi >= '0' && hi <= '9') ? hi - '0' : (hi | 0x20) - 'a' + 10;
        int lv = (lo >= '0' && lo <= '9') ? lo - '0' : (lo | 0x20) - 'a' + 10;
        out[n++] = (hv << 4) | lv;
        i += 2;
    }
    return n;
}

// ============================================================
// SECTION 5: strings extraction (features #15-17)
// ============================================================

static void do_strings(const char* filename, int filesize, int min_len) {
    static char buf[DUMP_CHUNK];
    static char run[256];
    int run_len = 0;
    int run_start = 0;
    int offset = 0;

    while (offset < filesize) {
        int to_read = filesize - offset;
        if (to_read > DUMP_CHUNK) to_read = DUMP_CHUNK;
        int res = avfs_read_file(filename, buf, to_read, offset);
        if (res < 0) break;

        for (int i = 0; i < to_read; i++) {
            unsigned char b = (unsigned char)buf[i];
            if (b >= 0x20 && b < 0x7F) {
                if (run_len == 0) run_start = offset + i;
                if (run_len < 255) run[run_len++] = (char)b;
            } else {
                if (run_len >= min_len) {
                    run[run_len] = '\0';
                    char off[9]; hex8((unsigned int)run_start, off);
                    printr("0x"); printr(off); printr(": "); printr(run); printr("\n");
                }
                run_len = 0;
            }
        }
        offset += to_read;
    }
    if (run_len >= min_len) {
        run[run_len] = '\0';
        char off[9]; hex8((unsigned int)run_start, off);
        printr("0x"); printr(off); printr(": "); printr(run); printr("\n");
    }
}

static void do_unicode_strings(const char* filename, int filesize) {
    static char buf[DUMP_CHUNK];
    static char run[256];
    int run_len = 0;
    int run_start = 0;
    int offset = 0;
    const int min_len = 4;

    while (offset < filesize - 1) {
        int to_read = filesize - offset;
        if (to_read > DUMP_CHUNK) to_read = DUMP_CHUNK;
        if (to_read % 2 != 0) to_read--;
        int res = avfs_read_file(filename, buf, to_read, offset);
        if (res < 0) break;

        for (int i = 0; i + 1 < to_read; i += 2) {
            unsigned char lo = (unsigned char)buf[i];
            unsigned char hi = (unsigned char)buf[i + 1];
            if (hi == 0 && lo >= 0x20 && lo < 0x7F) {
                if (run_len == 0) run_start = offset + i;
                if (run_len < 255) run[run_len++] = (char)lo;
            } else {
                if (run_len >= min_len) {
                    run[run_len] = '\0';
                    char off[9]; hex8((unsigned int)run_start, off);
                    printr("0x"); printr(off); printr(": "); printr(run); printr("\n");
                }
                run_len = 0;
            }
        }
        offset += to_read;
    }
    if (run_len >= min_len) {
        run[run_len] = '\0';
        char off[9]; hex8((unsigned int)run_start, off);
        printr("0x"); printr(off); printr(": "); printr(run); printr("\n");
    }
}

// ============================================================
// SECTION 6: diff / compare (features #18-20)
// ============================================================

static void do_diff(const char* fa, int sizea, const char* fb, int sizeb, int blocks_mode) {
    static char bufa[DUMP_CHUNK];
    static char bufb[DUMP_CHUNK];
    int minsize = sizea < sizeb ? sizea : sizeb;
    int offset = 0;
    int diffs = 0;
    int in_block = 0;
    int block_start = 0;

    while (offset < minsize) {
        int to_read = minsize - offset;
        if (to_read > DUMP_CHUNK) to_read = DUMP_CHUNK;
        avfs_read_file(fa, bufa, to_read, offset);
        avfs_read_file(fb, bufb, to_read, offset);

        for (int i = 0; i < to_read; i++) {
            int g = offset + i;
            int differs = (bufa[i] != bufb[i]);

            if (!blocks_mode) {
                if (differs && diffs < 512) {
                    char off[9]; hex8((unsigned int)g, off);
                    char h1[3], h2[3];
                    hex2((unsigned char)bufa[i], h1);
                    hex2((unsigned char)bufb[i], h2);
                    printr("0x"); printr(off); printr(": ");
                    printr(h1); printr(" != "); printr(h2); printr("\n");
                    diffs++;
                }
            } else {
                if (differs && !in_block) { in_block = 1; block_start = g; }
                if (!differs && in_block) {
                    in_block = 0;
                    char lo[9], hi[9], cnt[11];
                    hex8((unsigned int)block_start, lo); hex8((unsigned int)(g - 1), hi);
                    dec_u32((unsigned int)(g - block_start), cnt);
                    printr("0x"); printr(lo); printr(" - 0x"); printr(hi);
                    printr("  ("); printr(cnt); printr(" bytes differ)\n");
                    diffs++;
                }
            }
        }
        offset += to_read;
    }
    if (blocks_mode && in_block) {
        char lo[9], hi[9], cnt[11];
        hex8((unsigned int)block_start, lo); hex8((unsigned int)(minsize - 1), hi);
        dec_u32((unsigned int)(minsize - block_start), cnt);
        printr("0x"); printr(lo); printr(" - 0x"); printr(hi);
        printr("  ("); printr(cnt); printr(" bytes differ)\n");
        diffs++;
    }
    if (sizea != sizeb) {
        printr("Note: files differ in size (");
        char a[11], b[11]; dec_u32(sizea, a); dec_u32(sizeb, b);
        printr(a); printr(" vs "); printr(b); printr(" bytes) - compared common prefix only.\n");
    }
    if (diffs == 0) printr("Files identical over compared range.\n");
}

static void do_compare_region(const char* fa, const char* fb, int offa, int offb, int len) {
    static char bufa[512], bufb[512];
    if (len > 512) { printr("compare-region limited to 512 bytes\n"); len = 512; }
    avfs_read_file(fa, bufa, len, offa);
    avfs_read_file(fb, bufb, len, offb);
    for (int i = 0; i < len; i++) {
        if (bufa[i] != bufb[i]) {
            char off[9]; hex8((unsigned int)i, off);
            printr("MISMATCH at relative offset 0x"); printr(off);
            printr(" (byte "); 
            char h1[3], h2[3]; hex2((unsigned char)bufa[i], h1); hex2((unsigned char)bufb[i], h2);
            printr(h1); printr(" != "); printr(h2); printr(")\n");
            return;
        }
    }
    printr("Regions match exactly.\n");
}

// ============================================================
// SECTION 7: patch / fill / xor / rot13 (features #21-25)
// ============================================================

static void do_patch_byte(const char* filename, int offset, int value) {
    char b = (char)(value & 0xFF);
    avfs_write_file(filename, &b, 1, offset);
    printr("Patched 1 byte.\n");
}

static void do_patch_range(const char* filename, int offset, int* bytes, int n) {
    static char buf[256];
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++) buf[i] = (char)(bytes[i] & 0xFF);
    avfs_write_file(filename, buf, n, offset);
    char cnt[11]; dec_u32(n, cnt);
    printr("Patched "); printr(cnt); printr(" bytes.\n");
}

static void do_fill(const char* filename, int offset, int len, int value) {
    static char buf[512];
    for (int i = 0; i < 512; i++) buf[i] = (char)(value & 0xFF);
    int remaining = len;
    int off = offset;
    while (remaining > 0) {
        int chunk = remaining > 512 ? 512 : remaining;
        avfs_write_file(filename, buf, chunk, off);
        off += chunk;
        remaining -= chunk;
    }
    printr("Fill complete.\n");
}

static void do_xor(const char* filename, int filesize, int key) {
    static char buf[DUMP_CHUNK];
    int offset = 0;
    while (offset < filesize) {
        int to_read = filesize - offset;
        if (to_read > DUMP_CHUNK) to_read = DUMP_CHUNK;
        avfs_read_file(filename, buf, to_read, offset);
        for (int i = 0; i < to_read; i++) buf[i] = (char)((unsigned char)buf[i] ^ key);
        avfs_write_file(filename, buf, to_read, offset);
        offset += to_read;
    }
    printr("XOR transform complete.\n");
}

static void do_rot13(const char* filename, int filesize) {
    static char buf[DUMP_CHUNK];
    int offset = 0;
    while (offset < filesize) {
        int to_read = filesize - offset;
        if (to_read > DUMP_CHUNK) to_read = DUMP_CHUNK;
        avfs_read_file(filename, buf, to_read, offset);
        for (int i = 0; i < to_read; i++) {
            unsigned char c = (unsigned char)buf[i];
            if (c >= 'a' && c <= 'z') c = 'a' + (c - 'a' + 13) % 26;
            else if (c >= 'A' && c <= 'Z') c = 'A' + (c - 'A' + 13) % 26;
            buf[i] = (char)c;
        }
        avfs_write_file(filename, buf, to_read, offset);
        offset += to_read;
    }
    printr("ROT13 transform complete.\n");
}

// ============================================================
// SECTION 8: checksums / hash (features #26-29)
// ============================================================

static unsigned int crc32_update(unsigned int crc, const unsigned char* buf, int len) {
    for (int i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            unsigned int mask = -(int)(crc & 1);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

static void do_checksum(const char* filename, int filesize, const char* mode) {
    static char buf[DUMP_CHUNK];
    int offset = 0;

    unsigned int crc = 0xFFFFFFFFu;
    unsigned int sum8 = 0;
    unsigned int sum16 = 0;
    unsigned int rhash = 2166136261u; // FNV offset basis

    while (offset < filesize) {
        int to_read = filesize - offset;
        if (to_read > DUMP_CHUNK) to_read = DUMP_CHUNK;
        avfs_read_file(filename, buf, to_read, offset);

        if (strcmp(mode, "crc32") == 0) {
            crc = crc32_update(crc, (unsigned char*)buf, to_read);
        } else if (strcmp(mode, "sum8") == 0) {
            for (int i = 0; i < to_read; i++) sum8 = (sum8 + (unsigned char)buf[i]) & 0xFF;
        } else if (strcmp(mode, "sum16") == 0) {
            for (int i = 0; i < to_read; i++) sum16 = (sum16 + (unsigned char)buf[i]) & 0xFFFF;
        } else if (strcmp(mode, "rhash") == 0) {
            for (int i = 0; i < to_read; i++) {
                rhash ^= (unsigned char)buf[i];
                rhash *= 16777619u; // FNV prime
            }
        }
        offset += to_read;
    }

    char out[9];
    if (strcmp(mode, "crc32") == 0) { hex8(crc ^ 0xFFFFFFFFu, out); printr("CRC32:  0x"); printr(out); printr("\n"); }
    else if (strcmp(mode, "sum8") == 0) { hex2((unsigned char)sum8, out); printr("SUM8:   0x"); printr(out); printr("\n"); }
    else if (strcmp(mode, "sum16") == 0) { hex8(sum16, out); printr("SUM16:  0x"); printr(out + 4); printr("\n"); }
    else if (strcmp(mode, "rhash") == 0) { hex8(rhash, out); printr("RHASH:  0x"); printr(out); printr("\n"); }
    else printr("Unknown checksum mode. Use crc32, sum8, sum16, or rhash.\n");
}

// ============================================================
// SECTION 9: entropy (feature #30) - integer-only approximation
// ============================================================

// approximate log2 of a Q16.16 fixed-point value in (0, 65536]
// via bit-normalize + linear interpolation within the octave.
// NOT exact - documented as an estimate in the man page.
static int log2_q16_approx(unsigned int x_q16) {
    if (x_q16 == 0) return -20 << 16;
    int msb = 31;
    unsigned int t = x_q16;
    while (!(t & 0x80000000u)) { t <<= 1; msb--; }
    // msb = bit position of the highest set bit in the original 32-bit value
    int int_part = msb - 16; // relative to Q16.16 (bit 16 = value 1.0)
    unsigned int frac_bits = (t << 1) >> 16; // next 16 bits after the msb, as Q16.16 fraction [0,1)
    return (int_part << 16) + (int)frac_bits;
}

static void do_entropy(const char* filename, int filesize) {
    static unsigned int counts[256];
    for (int i = 0; i < 256; i++) counts[i] = 0;

    static char buf[DUMP_CHUNK];
    int offset = 0;
    while (offset < filesize) {
        int to_read = filesize - offset;
        if (to_read > DUMP_CHUNK) to_read = DUMP_CHUNK;
        avfs_read_file(filename, buf, to_read, offset);
        for (int i = 0; i < to_read; i++) counts[(unsigned char)buf[i]]++;
        offset += to_read;
    }

    long long entropy_q16 = 0; // accumulates -sum(p*log2(p)) in Q16.16
    for (int i = 0; i < 256; i++) {
        if (counts[i] == 0) continue;
        unsigned int p_q16 = (unsigned int)(((unsigned long long)counts[i] << 16) / (unsigned int)filesize);
        if (p_q16 == 0) p_q16 = 1;
        int lg = log2_q16_approx(p_q16); // negative
        long long term = -((long long)p_q16 * lg) >> 16; // p * (-log2 p), Q16.16
        entropy_q16 += term;
    }

    int whole = (int)(entropy_q16 >> 16);
    int frac = (int)(((entropy_q16 & 0xFFFF) * 100) >> 16);
    if (frac < 0) frac = -frac;

    char w[11], f[4];
    dec_u32((unsigned int)whole, w);
    dec_u32((unsigned int)frac, f);
    printr("Estimated entropy: ~"); printr(w); printr(".");
    if (frac < 10) printr("0");
    printr(f);
    printr(" bits/byte (0=uniform/repetitive, 8=maximally random)\n");
    printr("(integer fixed-point approximation - see 'geiger -m entropy')\n");
}

// ============================================================
// SECTION 10: histogram / stats (features #31-32)
// ============================================================

static void do_histogram(const char* filename, int filesize) {
    static unsigned int buckets[16];
    for (int i = 0; i < 16; i++) buckets[i] = 0;

    static char buf[DUMP_CHUNK];
    int offset = 0;
    while (offset < filesize) {
        int to_read = filesize - offset;
        if (to_read > DUMP_CHUNK) to_read = DUMP_CHUNK;
        avfs_read_file(filename, buf, to_read, offset);
        for (int i = 0; i < to_read; i++) buckets[((unsigned char)buf[i]) >> 4]++;
        offset += to_read;
    }

    unsigned int maxb = 1;
    for (int i = 0; i < 16; i++) if (buckets[i] > maxb) maxb = buckets[i];

    for (int i = 0; i < 16; i++) {
        char range[8];
        range[0] = '0'; range[1] = 'x';
        range[2] = "0123456789ABCDEF"[i]; range[3] = '0'; range[4] = '\0';
        printr(range); printr("-");
        range[2] = "0123456789ABCDEF"[i]; range[3] = 'F'; range[4] = '\0';
        printr(range); printr("  ");

        int bars = (int)(((unsigned long long)buckets[i] * 40) / maxb);
        for (int b = 0; b < bars; b++) printr("#");
        printr(" ");
        char c[11]; dec_u32(buckets[i], c); printr(c);
        printr("\n");
    }
}

static void do_stats(const char* filename, int filesize) {
    static unsigned int counts[256];
    for (int i = 0; i < 256; i++) counts[i] = 0;

    static char buf[DUMP_CHUNK];
    int offset = 0;
    while (offset < filesize) {
        int to_read = filesize - offset;
        if (to_read > DUMP_CHUNK) to_read = DUMP_CHUNK;
        avfs_read_file(filename, buf, to_read, offset);
        for (int i = 0; i < to_read; i++) counts[(unsigned char)buf[i]]++;
        offset += to_read;
    }

    unsigned int printable = 0, nulls = 0, most_common_val = 0, most_common_cnt = 0;
    for (int i = 0; i < 256; i++) {
        if (i >= 0x20 && i < 0x7F) printable += counts[i];
        if (i == 0) nulls = counts[i];
        if (counts[i] > most_common_cnt) { most_common_cnt = counts[i]; most_common_val = i; }
    }

    char sz[11], pct1[5], pct2[5], mc[3];
    dec_u32((unsigned int)filesize, sz);
    dec_u32((unsigned int)(((unsigned long long)printable * 100) / filesize), pct1);
    dec_u32((unsigned int)(((unsigned long long)nulls * 100) / filesize), pct2);
    hex2((unsigned char)most_common_val, mc);

    printr("size:          "); printr(sz); printr(" bytes\n");
    printr("printable:     "); printr(pct1); printr("%\n");
    printr("null bytes:    "); printr(pct2); printr("%\n");
    printr("most common:   0x"); printr(mc);
    char mcc[11]; dec_u32(most_common_cnt, mcc);
    printr(" ("); printr(mcc); printr(" occurrences)\n");
}

// ============================================================
// SECTION 11: magic byte detection (feature #33)
// ============================================================

static void do_magic(const char* filename, int filesize) {
    static char buf[16];
    int n = filesize < 16 ? filesize : 16;
    avfs_read_file(filename, buf, n, 0);

    const char* type = "unknown";
    if (n >= 4 && buf[0] == 0x7F && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F') type = "ELF executable/object";
    else if (n >= 2 && buf[0] == 'B' && buf[1] == 'M') type = "BMP image";
    else if (n >= 8 && (unsigned char)buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G') type = "PNG image";
    else if (n >= 6 && buf[0] == 'G' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == '8') type = "GIF image";
    else if (n >= 2 && (unsigned char)buf[0] == 0x1F && (unsigned char)buf[1] == 0x8B) type = "GZIP compressed data";
    else if (n >= 4 && buf[0] == 'P' && buf[1] == 'K' && buf[2] == 0x03 && buf[3] == 0x04) type = "ZIP archive";
    else if (n >= 2 && buf[0] == 'M' && buf[1] == 'Z') type = "MZ/PE executable";
    else if (n >= 4 && buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F') type = "RIFF container (WAV/AVI)";
    else if (n >= 4 && buf[0] == 'O' && buf[1] == 'g' && buf[2] == 'g' && buf[3] == 'S') type = "Ogg media";

    printr("Detected type: "); printr(type); printr("\n");
    if (strcmp(type, "unknown") == 0) {
        printr("First bytes: ");
        for (int i = 0; i < n; i++) { print_hex_byte((unsigned char)buf[i]); printr(" "); }
        printr("\n");
    }
}

// ============================================================
// SECTION 12: export / import (features #34-37)
// ============================================================

static void do_export_c(const char* filename, int filesize) {
    printr("unsigned char data[] = {\n    ");
    static char buf[DUMP_CHUNK];
    int offset = 0;
    int col = 0;
    while (offset < filesize) {
        int to_read = filesize - offset;
        if (to_read > DUMP_CHUNK) to_read = DUMP_CHUNK;
        avfs_read_file(filename, buf, to_read, offset);
        for (int i = 0; i < to_read; i++) {
            printr("0x"); char h[3]; hex2((unsigned char)buf[i], h); printr(h);
            if (offset + i != filesize - 1) printr(", ");
            col++;
            if (col == 12) { printr("\n    "); col = 0; }
        }
        offset += to_read;
    }
    printr("\n};\n");
    char sz[11]; dec_u32((unsigned int)filesize, sz);
    printr("unsigned int data_len = "); printr(sz); printr(";\n");
}

static void do_export_rsh(const char* filename, int filesize) {
    printr("bytes data = [");
    static char buf[DUMP_CHUNK];
    int offset = 0;
    while (offset < filesize) {
        int to_read = filesize - offset;
        if (to_read > DUMP_CHUNK) to_read = DUMP_CHUNK;
        avfs_read_file(filename, buf, to_read, offset);
        for (int i = 0; i < to_read; i++) {
            char h[3]; hex2((unsigned char)buf[i], h); printr(h);
            if (offset + i != filesize - 1) printr(" ");
        }
        offset += to_read;
    }
    printr("]\n");
}

static const char B64_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void do_export_base64(const char* filename, int filesize) {
    static char buf[3];
    int offset = 0;
    int linecol = 0;
    while (offset < filesize) {
        int n = filesize - offset;
        if (n > 3) n = 3;
        avfs_read_file(filename, buf, n, offset);
        unsigned char b0 = buf[0], b1 = n > 1 ? buf[1] : 0, b2 = n > 2 ? buf[2] : 0;

        char out[5];
        out[0] = B64_CHARS[b0 >> 2];
        out[1] = B64_CHARS[((b0 & 0x3) << 4) | (b1 >> 4)];
        out[2] = n > 1 ? B64_CHARS[((b1 & 0xF) << 2) | (b2 >> 6)] : '=';
        out[3] = n > 2 ? B64_CHARS[b2 & 0x3F] : '=';
        out[4] = '\0';
        printr(out);
        linecol += 4;
        if (linecol >= 76) { printr("\n"); linecol = 0; }
        offset += n;
    }
    printr("\n");
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void do_import_base64(const char* infile, int insize, const char* outfile) {
    static char inbuf[DUMP_CHUNK];
    static char outbuf[DUMP_CHUNK];
    int offset = 0;
    int outpos = 0;
    int quad[4]; int qn = 0;
    int written = 0;

    while (offset < insize) {
        int to_read = insize - offset;
        if (to_read > DUMP_CHUNK) to_read = DUMP_CHUNK;
        avfs_read_file(infile, inbuf, to_read, offset);

        for (int i = 0; i < to_read; i++) {
            char c = inbuf[i];
            if (c == '\n' || c == '\r' || c == ' ') continue;
            if (c == '=') continue;
            int v = b64_val(c);
            if (v < 0) continue;
            quad[qn++] = v;
            if (qn == 4) {
                outbuf[outpos++] = (char)((quad[0] << 2) | (quad[1] >> 4));
                outbuf[outpos++] = (char)(((quad[1] & 0xF) << 4) | (quad[2] >> 2));
                outbuf[outpos++] = (char)(((quad[2] & 0x3) << 6) | quad[3]);
                qn = 0;
                if (outpos >= DUMP_CHUNK - 4) {
                    avfs_write_file(outfile, outbuf, outpos, written);
                    written += outpos;
                    outpos = 0;
                }
            }
        }
        offset += to_read;
    }
    if (qn >= 2) {
        outbuf[outpos++] = (char)((quad[0] << 2) | (quad[1] >> 4));
        if (qn >= 3) outbuf[outpos++] = (char)(((quad[1] & 0xF) << 4) | (quad[2] >> 2));
    }
    if (outpos > 0) { avfs_write_file(outfile, outbuf, outpos, written); written += outpos; }

    char cnt[11]; dec_u32((unsigned int)written, cnt);
    printr("Decoded "); printr(cnt); printr(" bytes to "); printr(outfile); printr("\n");
}

// ============================================================
// SECTION 13: bookmarks / annotations (features #38-42)
// AVFS sidecar files: "<file>.rxbm" and "<file>.rxan"
// line format bookmarks:   NAME OFFSET\n
// line format annotations: OFFSET|note text\n
// ============================================================

static void sidecar_path(const char* filename, const char* suffix, char* out) {
    int i = 0;
    while (filename[i]) { out[i] = filename[i]; i++; }
    int j = 0;
    while (suffix[j]) out[i++] = suffix[j++];
    out[i] = '\0';
}

static void bookmark_add(const char* filename, const char* name, int offset) {
    char path[128];
    sidecar_path(filename, ".rxbm", path);

    static char line[128];
    int p = 0;
    int i = 0;
    while (name[i]) line[p++] = name[i++];
    line[p++] = ' ';
    char o[9]; hex8((unsigned int)offset, o);
    for (int k = 0; o[k]; k++) line[p++] = o[k];
    line[p++] = '\n';

    int existing = avfs_get_filesize(path);
    int append_at = existing > 0 ? existing : 0;
    avfs_write_file(path, line, p, append_at);
    printr("Bookmark '"); printr(name); printr("' saved.\n");
}

static void bookmark_list(const char* filename) {
    char path[128];
    sidecar_path(filename, ".rxbm", path);
    if (!avfs_file_exists(path)) { printr("No bookmarks for this file.\n"); return; }

    static char buf[2048];
    avfs_get_content(path, buf, sizeof(buf));
    printr(buf);
}

// returns 1 and fills *offset_out if found
static int bookmark_find(const char* filename, const char* name, int* offset_out) {
    char path[128];
    sidecar_path(filename, ".rxbm", path);
    if (!avfs_file_exists(path)) return 0;

    static char buf[2048];
    avfs_get_content(path, buf, sizeof(buf));

    int i = 0;
    while (buf[i]) {
        int start = i;
        while (buf[i] && buf[i] != ' ') i++;
        int nlen = i - start;
        int match = 1;
        for (int k = 0; k < nlen; k++) if (name[k] != buf[start + k]) { match = 0; break; }
        if (match && name[nlen] == '\0') {
            i++; // skip space
            int oval = 0;
            // parse 8 hex chars
            for (int k = 0; k < 8 && buf[i]; k++, i++) {
                char c = buf[i];
                int d = (c >= '0' && c <= '9') ? c - '0' : (c | 0x20) - 'a' + 10;
                oval = (oval << 4) | d;
            }
            *offset_out = oval;
            return 1;
        }
        while (buf[i] && buf[i] != '\n') i++;
        if (buf[i] == '\n') i++;
    }
    return 0;
}

static void annotate_add(const char* filename, int offset, const char* note) {
    char path[128];
    sidecar_path(filename, ".rxan", path);

    static char line[256];
    int p = 0;
    char o[9]; hex8((unsigned int)offset, o);
    for (int k = 0; o[k]; k++) line[p++] = o[k];
    line[p++] = '|';
    int i = 0;
    while (note[i] && p < 250) line[p++] = note[i++];
    line[p++] = '\n';

    int existing = avfs_get_filesize(path);
    int append_at = existing > 0 ? existing : 0;
    avfs_write_file(path, line, p, append_at);
    printr("Annotation saved at 0x"); printr(o); printr(".\n");
}

static void annotate_list(const char* filename) {
    char path[128];
    sidecar_path(filename, ".rxan", path);
    if (!avfs_file_exists(path)) { printr("No annotations for this file.\n"); return; }

    static char buf[2048];
    avfs_get_content(path, buf, sizeof(buf));
    printr(buf);
}

// ============================================================
// SECTION 14: extract / truncate / append / zero-fill (#43-46)
// ============================================================

static void do_extract(const char* filename, int filesize, int offset, int len, const char* outfile) {
    if (offset < 0 || offset >= filesize) { printr("offset out of range\n"); return; }
    if (offset + len > filesize) len = filesize - offset;

    static char buf[DUMP_CHUNK];
    int remaining = len;
    int src = offset;
    int dst = 0;
    while (remaining > 0) {
        int chunk = remaining > DUMP_CHUNK ? DUMP_CHUNK : remaining;
        avfs_read_file(filename, buf, chunk, src);
        avfs_write_file(outfile, buf, chunk, dst);
        src += chunk; dst += chunk; remaining -= chunk;
    }
    char cnt[11]; dec_u32((unsigned int)len, cnt);
    printr("Extracted "); printr(cnt); printr(" bytes to "); printr(outfile); printr("\n");
}

static void do_truncate(const char* filename, int newsize) {
    avfs_truncate(filename, newsize);
    printr("Truncated.\n");
}

static void do_append_hex(const char* filename, int filesize, const char* hexstr) {
    int pattern[128];
    int n = parse_hex_pattern(hexstr, pattern, 128);
    static char buf[128];
    for (int i = 0; i < n; i++) buf[i] = (char)(pattern[i] == -1 ? 0 : pattern[i]);
    avfs_write_file(filename, buf, n, filesize);
    char cnt[11]; dec_u32((unsigned int)n, cnt);
    printr("Appended "); printr(cnt); printr(" bytes.\n");
}

static void do_zero_fill(const char* filename, int filesize, int len) {
    do_fill(filename, filesize, len, 0x00);
}

// ============================================================
// SECTION 15: interactive mode (feature #2), with bookmarks,
// inline search, goto, and watch (feature #47)
// ============================================================

#define GVGA_COLS 80
#define GVGA_ROWS 50
#define VGA_MEM   ((volatile uint16_t*)0xB8000)

#define COL_NORMAL     0x07
#define COL_HEADER     0x0B
#define COL_CURSOR     0x4F
#define COL_SEL        0x6F
#define COL_DESC       0x0A
#define COL_STATUS     0x70
#define COL_BOOKMARK   0x2F

static int g_color_enabled = 1;

static void g_putc(int row, int col, char c, uint8_t color) {
    if (row < 0 || row >= GVGA_ROWS || col < 0 || col >= GVGA_COLS) return;
    if (!g_color_enabled) color = COL_NORMAL;
    VGA_MEM[row * GVGA_COLS + col] = ((uint16_t)color << 8) | (uint8_t)c;
}
static void g_puts(int row, int col, const char* s, uint8_t color) {
    while (*s && col < GVGA_COLS) g_putc(row, col++, *s++, color);
}
static void g_clear_row(int row, uint8_t color) { for (int c = 0; c < GVGA_COLS; c++) g_putc(row, c, ' ', color); }
static void g_clear_all(uint8_t color) { for (int r = 0; r < GVGA_ROWS; r++) g_clear_row(r, color); }

#define SC_UP 0x48
#define SC_DOWN 0x50
#define SC_LEFT 0x4B
#define SC_RIGHT 0x4D
#define SC_PGUP 0x49
#define SC_PGDN 0x51
#define SC_HOME 0x47
#define SC_END 0x4F
#define SC_ESC 0x01
#define SC_Q 0x10
#define SC_S 0x1F
#define SC_N 0x31
#define SC_G 0x22
#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_LCTRL 0x1D

typedef struct { int scancode; int is_ext; int shift_down; } GKey;

static GKey geiger_read_key(int* shift_state, int timeout_iters) {
    GKey k = { -1, 0, *shift_state };
    for (;;) {
        if (!is_key_pressed()) {
            if (timeout_iters > 0) { timeout_iters--; if (timeout_iters == 0) { k.scancode = -2; return k; } }
            continue;
        }
        uint8_t sc = port_byte_in(0x60);
        if (sc == 0xE0) {
            while (!is_key_pressed());
            uint8_t ext = port_byte_in(0x60);
            if (ext & 0x80) continue;
            k.scancode = ext; k.is_ext = 1; k.shift_down = *shift_state;
            return k;
        }
        if (sc & 0x80) {
            uint8_t key = sc & 0x7F;
            if (key == SC_LSHIFT || key == SC_RSHIFT) *shift_state = 0;
            continue;
        }
        if (sc == SC_LSHIFT || sc == SC_RSHIFT) { *shift_state = 1; continue; }
        k.scancode = sc; k.is_ext = 0; k.shift_down = *shift_state;
        return k;
    }
}

// blocking single-line text entry on a given status row, used by
// inline search ('s') and goto ('g') inside interactive mode
static int mini_text_input(char* buf, int maxlen, int row, int col_start, const char* prompt) {
    g_clear_row(row, COL_STATUS);
    g_puts(row, 0, prompt, COL_STATUS);
    int len = 0;
    int shift_state = 0;
    for (;;) {
        buf[len] = '\0';
        g_puts(row, col_start, buf, COL_STATUS);
        g_putc(row, col_start + len, '_', COL_STATUS);

        if (!is_key_pressed()) continue;
        uint8_t sc = port_byte_in(0x60);
        if (sc & 0x80) {
            uint8_t key = sc & 0x7F;
            if (key == SC_LSHIFT || key == SC_RSHIFT) shift_state = 0;
            continue;
        }
        if (sc == SC_LSHIFT || sc == SC_RSHIFT) { shift_state = 1; continue; }
        if (sc == 0x1C) { buf[len] = '\0'; return 1; }        // Enter
        if (sc == SC_ESC) return 0;                            // Abort
        if (sc == 0x0E) { if (len > 0) len--; continue; }       // Backspace
        if (sc < 128) {
            char c = shift_state ? shifted_keyboard_map[sc] : keyboard_map[sc];
            if (c != 0 && len < maxlen - 1) buf[len++] = c;
        }
    }
}

#define PAGE_ROWS 34
#define PAGE_BYTES (PAGE_ROWS * 16)
#define DUMP_TOP_ROW 3
#define DESC_SEP_ROW (DUMP_TOP_ROW + PAGE_ROWS)
#define DESC_TOP_ROW (DESC_SEP_ROW + 1)
#define STATUS_ROW (GVGA_ROWS - 1)
#define ASCII_COL 61

static int in_selection(int off, int a, int b) {
    if (a < 0 || b < 0) return 0;
    int lo = a < b ? a : b, hi = a < b ? b : a;
    return off >= lo && off <= hi;
}

static void draw_static_chrome(const char* filename, int filesize, int watch_mode) {
    g_clear_all(COL_NORMAL);
    g_puts(0, 0, "RadiumGeiger v2 -- interactive byte inspector", COL_HEADER);
    g_puts(0, 55, filename, COL_HEADER);
    g_puts(1, 0, "Offset    00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F  ASCII", COL_NORMAL);
    for (int c = 0; c < GVGA_COLS; c++) g_putc(2, c, '-', COL_NORMAL);
    for (int c = 0; c < GVGA_COLS; c++) g_putc(DESC_SEP_ROW, c, '-', COL_NORMAL);

    char sizebuf[16]; dec_u32((unsigned int)filesize, sizebuf);
    g_puts(STATUS_ROW, 0,
        "ARROWS move  SHIFT+ARROW select  PGUP/PGDN page  1-9 bookmark  s search  n next  g goto  Esc/Q quit",
        COL_STATUS);
    (void)watch_mode;
}

static void draw_page(const char* filename, int page_offset, int page_len,
                       int cur_offset, int sel_a, int sel_b, int* bookmarks) {
    static char buf[PAGE_BYTES];
    if (avfs_read_file(filename, buf, page_len, page_offset) < 0) return;

    for (int row = 0; row < PAGE_ROWS; row++) {
        int row_start = row * 16;
        if (row_start >= page_len) { g_clear_row(DUMP_TOP_ROW + row, COL_NORMAL); continue; }

        char off[9]; hex8((unsigned int)(page_offset + row_start), off);
        g_puts(DUMP_TOP_ROW + row, 0, off, COL_NORMAL);

        int col = 10;
        char asc[17]; int asc_len = 0;

        for (int i = 0; i < 16; i++) {
            int idx = row_start + i;
            int g = page_offset + idx;
            uint8_t color = COL_NORMAL;
            if (g == cur_offset) color = COL_CURSOR;
            else if (in_selection(g, sel_a, sel_b)) color = COL_SEL;
            else { for (int bm = 1; bm <= 9; bm++) if (bookmarks[bm] == g) color = COL_BOOKMARK; }

            if (idx < page_len) {
                unsigned char b = (unsigned char)buf[idx];
                char h[3]; h[0] = "0123456789ABCDEF"[b >> 4]; h[1] = "0123456789ABCDEF"[b & 0xF]; h[2] = '\0';
                g_puts(DUMP_TOP_ROW + row, col, h, color);
                asc[asc_len++] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
            } else { g_puts(DUMP_TOP_ROW + row, col, "  ", COL_NORMAL); asc[asc_len++] = ' '; }
            col += 3; if (i == 7) col += 1;
        }
        asc[asc_len] = '\0';
        for (int i = 0; i < 16; i++) {
            int g = page_offset + row_start + i;
            uint8_t color = COL_NORMAL;
            if (g == cur_offset) color = COL_CURSOR;
            else if (in_selection(g, sel_a, sel_b)) color = COL_SEL;
            g_putc(DUMP_TOP_ROW + row, ASCII_COL + i, asc[i], color);
        }
    }
}

static void draw_description(const char* filename, int filesize, int page_offset,
                              int page_len, int cur_offset, int sel_a, int sel_b) {
    for (int r = DESC_TOP_ROW; r < STATUS_ROW; r++) g_clear_row(r, COL_NORMAL);

    int local = cur_offset - page_offset;
    static char page_buf[PAGE_BYTES];
    avfs_read_file(filename, page_buf, page_len, page_offset);
    unsigned char b = (unsigned char)page_buf[local];

    char off_hex[9], off_dec[11], hex2b[3], dec[5], sdec[6], bin[9];
    hex8((unsigned int)cur_offset, off_hex);
    dec_u32((unsigned int)cur_offset, off_dec);
    hex2((unsigned char)b, hex2b);
    dec_u32(b, dec);
    dec_i32((int8_t)b, sdec);
    bin8(b, bin);

    g_puts(DESC_TOP_ROW, 0, "offset: 0x", COL_DESC); g_puts(DESC_TOP_ROW, 10, off_hex, COL_DESC);
    g_puts(DESC_TOP_ROW, 20, "(", COL_DESC); g_puts(DESC_TOP_ROW, 21, off_dec, COL_DESC); g_puts(DESC_TOP_ROW, 31, ")", COL_DESC);

    g_puts(DESC_TOP_ROW+1, 0, "byte:   0x", COL_DESC); g_puts(DESC_TOP_ROW+1, 10, hex2b, COL_DESC);
    g_puts(DESC_TOP_ROW+1, 14, "unsigned:", COL_DESC); g_puts(DESC_TOP_ROW+1, 24, dec, COL_DESC);
    g_puts(DESC_TOP_ROW+1, 30, "signed:", COL_DESC); g_puts(DESC_TOP_ROW+1, 38, sdec, COL_DESC);
    g_puts(DESC_TOP_ROW+1, 46, "binary:", COL_DESC); g_puts(DESC_TOP_ROW+1, 54, bin, COL_DESC);

    if (b >= 0x20 && b < 0x7F) {
        char c[2] = { (char)b, '\0' };
        g_puts(DESC_TOP_ROW+2, 0, "char:   '", COL_DESC); g_puts(DESC_TOP_ROW+2, 9, c, COL_DESC); g_puts(DESC_TOP_ROW+2, 10, "'", COL_DESC);
    } else {
        g_puts(DESC_TOP_ROW+2, 0, "char:   (non-printable)", COL_DESC);
    }

    static const char* ctrl_names[32] = {
        "NUL","SOH","STX","ETX","EOT","ENQ","ACK","BEL","BS","TAB","LF","VT","FF","CR","SO","SI",
        "DLE","DC1","DC2","DC3","DC4","NAK","SYN","ETB","CAN","EM","SUB","ESC","FS","GS","RS","US"
    };
    const char* cls = b < 0x20 ? ctrl_names[b] : (b == 0x7F ? "DEL" : (b < 0x7F ? "printable ASCII" : "extended/binary"));
    g_puts(DESC_TOP_ROW+3, 0, "type:   ", COL_DESC); g_puts(DESC_TOP_ROW+3, 8, cls, COL_DESC);

    if (sel_a >= 0 && sel_b >= 0 && sel_a != sel_b) {
        int lo = sel_a < sel_b ? sel_a : sel_b, hi = sel_a < sel_b ? sel_b : sel_a;
        int count = hi - lo + 1;
        char lo_hex[9], hi_hex[9], cnt_dec[11];
        hex8((unsigned int)lo, lo_hex); hex8((unsigned int)hi, hi_hex); dec_u32((unsigned int)count, cnt_dec);
        g_puts(DESC_TOP_ROW+4, 0, "select: 0x", COL_DESC); g_puts(DESC_TOP_ROW+4, 10, lo_hex, COL_DESC);
        g_puts(DESC_TOP_ROW+4, 18, "-> 0x", COL_DESC); g_puts(DESC_TOP_ROW+4, 23, hi_hex, COL_DESC);
        g_puts(DESC_TOP_ROW+4, 31, "(", COL_DESC); g_puts(DESC_TOP_ROW+4, 32, cnt_dec, COL_DESC);
    } else if (cur_offset + 1 == filesize) {
        g_puts(DESC_TOP_ROW+4, 0, "(last byte in file)", COL_DESC);
    } else if (cur_offset == 0) {
        g_puts(DESC_TOP_ROW+4, 0, "(first byte in file)", COL_DESC);
    }
}

static void interactive_view(const char* filename, int start_offset, int color_off, int watch_mode) {
    int filesize = avfs_get_filesize(filename);
    int cur_offset = start_offset >= 0 && start_offset < filesize ? start_offset : 0;
    int sel_a = -1, sel_b = -1;
    int shift_state = 0;
    int bookmarks[10]; for (int i = 0; i < 10; i++) bookmarks[i] = -1;
    static char search_needle[64];
    search_needle[0] = '\0';
    g_color_enabled = !color_off;

    draw_static_chrome(filename, filesize, watch_mode);

    for (;;) {
        int cur_size = watch_mode ? avfs_get_filesize(filename) : filesize;
        if (cur_size != filesize) {
            filesize = cur_size;
            if (cur_offset >= filesize) cur_offset = filesize > 0 ? filesize - 1 : 0;
            draw_static_chrome(filename, filesize, watch_mode);
        }

        int page_offset = (cur_offset / PAGE_BYTES) * PAGE_BYTES;
        int page_len = filesize - page_offset;
        if (page_len > PAGE_BYTES) page_len = PAGE_BYTES;
        if (page_len < 0) page_len = 0;

        draw_page(filename, page_offset, page_len, cur_offset, sel_a, sel_b, bookmarks);
        draw_description(filename, filesize, page_offset, page_len, cur_offset, sel_a, sel_b);

        GKey k = geiger_read_key(&shift_state, watch_mode ? 200000 : 0);
        if (k.scancode == -2) continue; // watch-mode poll timeout, loop to re-check size

        if (!k.is_ext) {
            if (k.scancode == SC_ESC || k.scancode == SC_Q) { g_clear_all(0x07); return; }

            if (k.scancode >= 0x02 && k.scancode <= 0x0A) { // '1'-'9'
                int slot = k.scancode - 0x01;
                if (bookmarks[slot] == cur_offset) bookmarks[slot] = -1;
                else if (bookmarks[slot] >= 0) cur_offset = bookmarks[slot];
                else bookmarks[slot] = cur_offset;
                continue;
            }
            if (k.scancode == SC_S) {
                static char inbuf[64];
                if (mini_text_input(inbuf, 64, STATUS_ROW, 8, "search: ")) {
                    int i = 0; while (inbuf[i]) { search_needle[i] = inbuf[i]; i++; } search_needle[i] = '\0';
                    // fall through to find-next below
                    k.scancode = SC_N;
                } else { draw_static_chrome(filename, filesize, watch_mode); continue; }
            }
            if (k.scancode == SC_N && search_needle[0] != '\0') {
                int nlen = 0; while (search_needle[nlen]) nlen++;
                static char sbuf[DUMP_CHUNK];
                int found = -1;
                int scan_from = cur_offset + 1;
                for (int pass = 0; pass < 2 && found < 0; pass++) {
                    int end = pass == 0 ? filesize : scan_from;
                    int start = pass == 0 ? scan_from : 0;
                    int off = start;
                    while (off < end && found < 0) {
                        int to_read = end - off; if (to_read > DUMP_CHUNK) to_read = DUMP_CHUNK;
                        avfs_read_file(filename, sbuf, to_read, off);
                        for (int i = 0; i <= to_read - nlen && found < 0; i++) {
                            int m = 1;
                            for (int j = 0; j < nlen; j++) if (sbuf[i+j] != search_needle[j]) { m = 0; break; }
                            if (m) found = off + i;
                        }
                        off += to_read - (nlen - 1);
                    }
                    scan_from = 0;
                }
                if (found >= 0) { cur_offset = found; sel_a = found; sel_b = found + nlen - 1; }
                draw_static_chrome(filename, filesize, watch_mode);
                continue;
            }
            if (k.scancode == SC_G) {
                static char inbuf[32];
                if (mini_text_input(inbuf, 32, STATUS_ROW, 6, "goto 0x: ")) {
                    int v = 0, i = 0;
                    while (inbuf[i]) { char c = inbuf[i]; int d = (c>='0'&&c<='9')?c-'0':(c|0x20)-'a'+10; v = v*16+d; i++; }
                    if (v >= 0 && v < filesize) cur_offset = v;
                }
                draw_static_chrome(filename, filesize, watch_mode);
                continue;
            }
            continue;
        }

        int old_offset = cur_offset;
        switch (k.scancode) {
            case SC_LEFT:  if (cur_offset > 0) cur_offset--; break;
            case SC_RIGHT: if (cur_offset < filesize - 1) cur_offset++; break;
            case SC_UP:    if (cur_offset - 16 >= 0) cur_offset -= 16; break;
            case SC_DOWN:  if (cur_offset + 16 < filesize) cur_offset += 16; break;
            case SC_PGUP:  cur_offset -= PAGE_BYTES; if (cur_offset < 0) cur_offset = 0; break;
            case SC_PGDN:  cur_offset += PAGE_BYTES; if (cur_offset > filesize - 1) cur_offset = filesize - 1; break;
            case SC_HOME:  cur_offset = 0; break;
            case SC_END:   cur_offset = filesize - 1; break;
            default: break;
        }
        if (k.shift_down) { if (sel_a == -1) sel_a = old_offset; sel_b = cur_offset; }
        else { sel_a = -1; sel_b = -1; }
    }
}

// ============================================================
// SECTION 16: entry point / argument dispatch
// ============================================================

static void usage(void) {
    printr("Usage: geiger [flag] [flag-args...] <file>\n");
    printr("       geiger -m [feature]        (no file needed)\n");
    printr("Run 'geiger -m' for the full feature list.\n");
}

void geiger_command(int argc, char* argv[]) {
    if (argc < 2) { usage(); return; }

    // -m / --man doesn't need a file
    if (strcmp(argv[1], "-m") == 0 || strcmp(argv[1], "--man") == 0) {
        print_man(argc >= 3 ? argv[2] : (const char*)0);
        return;
    }

    const char* flag = argv[1];
    const char* filename = argv[argc - 1];
    int filesize = avfs_get_filesize(filename);
    if (filesize < 0) { printr("Error: File not found\n"); return; }

    int no_stamp = 0;
    const char* custom_stamp = (const char*)0;

    if (strcmp(flag, "--no-stamp") == 0) { no_stamp = 1; flag = argc > 3 ? argv[2] : "dump"; }

    if (filesize == 0 && strcmp(flag, "--append-hex") != 0 && strcmp(flag, "--zero-fill") != 0) {
        printr("(empty file)\n");
        stamp_file_signature(filename, custom_stamp, no_stamp);
        return;
    }

    if (strcmp(flag, "dump") == 0 || filename == argv[1]) {
        DumpOpts o = default_dump_opts();
        dump_range(filename, filesize, o);
    }
    else if (strcmp(flag, "-i") == 0 || strcmp(flag, "--interactive") == 0) {
        int color_off = 0, watch = 0, start = 0;
        for (int i = 2; i < argc - 1; i++) {
            if (strcmp(argv[i], "off") == 0) color_off = 1;
        }
        interactive_view(filename, start, color_off, watch);
    }
    else if (strcmp(flag, "--watch") == 0) {
        interactive_view(filename, 0, 0, 1);
    }
    else if (strcmp(flag, "-o") == 0 || strcmp(flag, "--offset") == 0) {
        DumpOpts o = default_dump_opts();
        o.start = str_to_int(argv[2]);
        dump_range(filename, filesize, o);
    }
    else if (strcmp(flag, "-l") == 0 || strcmp(flag, "--length") == 0) {
        DumpOpts o = default_dump_opts();
        o.length = str_to_int(argv[2]);
        dump_range(filename, filesize, o);
    }
    else if (strcmp(flag, "-w") == 0 || strcmp(flag, "--width") == 0) {
        DumpOpts o = default_dump_opts();
        o.width = str_to_int(argv[2]);
        dump_range(filename, filesize, o);
    }
    else if (strcmp(flag, "-g") == 0 || strcmp(flag, "--group") == 0) {
        DumpOpts o = default_dump_opts();
        o.group = str_to_int(argv[2]);
        dump_range(filename, filesize, o);
    }
    else if (strcmp(flag, "-a") == 0 || strcmp(flag, "--ascii-only") == 0) {
        DumpOpts o = default_dump_opts(); o.ascii_only = 1;
        dump_range(filename, filesize, o);
    }
    else if (strcmp(flag, "-x") == 0 || strcmp(flag, "--hex-only") == 0) {
        DumpOpts o = default_dump_opts(); o.hex_only = 1;
        dump_range(filename, filesize, o);
    }
    else if (strcmp(flag, "-r") == 0 || strcmp(flag, "--reverse") == 0) {
        DumpOpts o = default_dump_opts(); o.reverse = 1;
        dump_range(filename, filesize, o);
    }
    else if (strcmp(flag, "-s") == 0 || strcmp(flag, "--search") == 0) {
        search_ascii(filename, filesize, argv[2]);
    }
    else if (strcmp(flag, "-sh") == 0 || strcmp(flag, "--search-hex") == 0) {
        int pattern[64]; int n = parse_hex_pattern(argv[2], pattern, 64);
        search_pattern(filename, filesize, pattern, n);
    }
    else if (strcmp(flag, "-sr") == 0 || strcmp(flag, "--search-regex") == 0) {
        int pattern[64]; int n = parse_hex_pattern(argv[2], pattern, 64);
        search_pattern(filename, filesize, pattern, n);
    }
    else if (strcmp(flag, "--strings") == 0) {
        int min_len = 4;
        for (int i = 2; i < argc - 2; i++)
            if (strcmp(argv[i], "--strings-min") == 0) min_len = str_to_int(argv[i+1]);
        do_strings(filename, filesize, min_len);
    }
    else if (strcmp(flag, "--unicode-strings") == 0) {
        do_unicode_strings(filename, filesize);
    }
    else if (strcmp(flag, "-d") == 0 || strcmp(flag, "--diff") == 0) {
        int sizeb = avfs_get_filesize(argv[2]);
        if (sizeb < 0) { printr("second file not found\n"); return; }
        do_diff(argv[2], sizeb, filename, filesize, 0);
    }
    else if (strcmp(flag, "--diff-blocks") == 0) {
        int sizeb = avfs_get_filesize(argv[2]);
        if (sizeb < 0) { printr("second file not found\n"); return; }
        do_diff(argv[2], sizeb, filename, filesize, 1);
    }
    else if (strcmp(flag, "--compare-region") == 0) {
        do_compare_region(argv[2], filename, str_to_int(argv[3]), str_to_int(argv[4]), str_to_int(argv[5]));
    }
    else if (strcmp(flag, "-p") == 0 || strcmp(flag, "--patch") == 0) {
        char* eq = argv[2];
        char offbuf[16]; int oi = 0;
        while (*eq && *eq != '=') offbuf[oi++] = *eq++;
        offbuf[oi] = '\0';
        int off = str_to_int(offbuf);
        int val = str_to_int(eq + 1);
        do_patch_byte(filename, off, val);
    }
    else if (strcmp(flag, "--patch-range") == 0) {
        int off = str_to_int(argv[2]);
        int pattern[256]; int n = parse_hex_pattern(argv[3], pattern, 256);
        do_patch_range(filename, off, pattern, n);
    }
    else if (strcmp(flag, "--fill") == 0) {
        do_fill(filename, str_to_int(argv[2]), str_to_int(argv[3]), str_to_int(argv[4]));
    }
    else if (strcmp(flag, "--xor") == 0) {
        do_xor(filename, filesize, str_to_byte(argv[2]));
    }
    else if (strcmp(flag, "--rot13") == 0) {
        do_rot13(filename, filesize);
    }
    else if (strcmp(flag, "--checksum") == 0) {
        do_checksum(filename, filesize, argv[2]);
    }
    else if (strcmp(flag, "--entropy") == 0) {
        do_entropy(filename, filesize);
    }
    else if (strcmp(flag, "--histogram") == 0) {
        do_histogram(filename, filesize);
    }
    else if (strcmp(flag, "--stats") == 0) {
        do_stats(filename, filesize);
    }
    else if (strcmp(flag, "--magic") == 0) {
        do_magic(filename, filesize);
    }
    else if (strcmp(flag, "--export") == 0) {
        if (strcmp(argv[2], "c") == 0) do_export_c(filename, filesize);
        else if (strcmp(argv[2], "rsh") == 0) do_export_rsh(filename, filesize);
        else if (strcmp(argv[2], "base64") == 0) do_export_base64(filename, filesize);
        else printr("unknown export format: use c, rsh, or base64\n");
    }
    else if (strcmp(flag, "--import-base64") == 0) {
        do_import_base64(filename, filesize, argv[2]);
    }
    else if (strcmp(flag, "--bookmark") == 0) {
        if (strcmp(argv[2], "add") == 0) bookmark_add(filename, argv[3], str_to_int(argv[4]));
        else if (strcmp(argv[2], "list") == 0) bookmark_list(filename);
        else if (strcmp(argv[2], "goto") == 0) {
            int off;
            if (bookmark_find(filename, argv[3], &off)) {
                DumpOpts o = default_dump_opts(); o.start = off; o.length = 64;
                dump_range(filename, filesize, o);
            } else printr("bookmark not found\n");
        }
    }
    else if (strcmp(flag, "--goto") == 0) {
        int center = str_to_int(argv[2]);
        DumpOpts o = default_dump_opts();
        o.start = center - 32 > 0 ? center - 32 : 0;
        o.length = 128;
        o.highlight_lo = center; o.highlight_hi = center;
        dump_range(filename, filesize, o);
    }
    else if (strcmp(flag, "--annotate") == 0) {
        annotate_add(filename, str_to_int(argv[2]), argv[3]);
    }
    else if (strcmp(flag, "--annotate-list") == 0) {
        annotate_list(filename);
    }
    else if (strcmp(flag, "--extract") == 0) {
        do_extract(filename, filesize, str_to_int(argv[2]), str_to_int(argv[3]), argv[4]);
    }
    else if (strcmp(flag, "--truncate") == 0) {
        do_truncate(filename, str_to_int(argv[2]));
    }
    else if (strcmp(flag, "--append-hex") == 0) {
        do_append_hex(filename, filesize, argv[2]);
    }
    else if (strcmp(flag, "--zero-fill") == 0) {
        do_zero_fill(filename, filesize, str_to_int(argv[2]));
    }
    else if (strcmp(flag, "--stamp-custom") == 0) {
        custom_stamp = argv[2];
        DumpOpts o = default_dump_opts();
        dump_range(filename, filesize, o);
    }
    else {
        printr("Unknown flag: "); printr(flag); printr("\n");
        usage();
        return;
    }

    stamp_file_signature(filename, custom_stamp, no_stamp);
}