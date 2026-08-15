#include "../terminal/terminal.h"
#include "../keyboard/keyboard.h"
#include "help.h"
#include "../utility/utility.h"

/* ── Colours ──────────────────────────────────────────────────────── */
#define COL_RESET    0x07
#define COL_HEADER   0x0E
#define COL_SELECTED 0x1F
#define COL_NAME     0x0B
#define COL_DESC     0x07
#define COL_BORDER   0x08
#define COL_SEARCH   0x0A
#define COL_HINT     0x08
#define COL_MATCH    0x0E
#define COL_SCROLLBAR 0x07
#define COL_SCROLLTHUMB 0x0F

/* ── 80x50 Layout ─────────────────────────────────────────────────── */
#define TERM_W        80
#define TERM_H        50
#define VISIBLE_ROWS  36          /* rows available for command list   */
#define COL_NAME_W    18
#define SEARCH_MAX    48
#define SCROLLBAR_COL 79          /* rightmost column                  */

/* ── Scancodes ────────────────────────────────────────────────────── */
#define SC_ESC        0x01
#define SC_ENTER      0x1C
#define SC_BACKSPACE  0x0E
#define SC_UP         0x48
#define SC_DOWN       0x50
#define SC_PGUP       0x49
#define SC_PGDN       0x51
#define SC_HOME       0x47
#define SC_END        0x4F
#define SC_LSHIFT     0x2A
#define SC_RSHIFT     0x36
#define SC_LSHIFT_REL 0xAA
#define SC_RSHIFT_REL 0xB6
#define SC_CAPS       0x3A
#define SC_LCTRL      0x1D
#define SC_LCTRL_REL  0x9D
#define SC_RELEASE    0x80

/* ── Key repeat tuning ────────────────────────────────────────────── */
#define REPEAT_DELAY  1800     /* loops before first repeat fires   */
#define REPEAT_RATE   60000       /* loops between subsequent repeats  */

/* ── VGA direct write (no flicker) ───────────────────────────────── */
/*
   We write the entire screen into a local shadow buffer first,
   then blast it to VGA memory in one pass. This eliminates the
   visible redraw flicker that comes from clearing then rewriting.
*/
#define VGA_BASE  ((volatile uint16_t*)0xB8000)

typedef struct {
    uint8_t ch;
    uint8_t attr;
} Cell;

static Cell shadow[TERM_H][TERM_W];

static void sc_clear(void) {
    for (int r = 0; r < TERM_H; r++)
        for (int c = 0; c < TERM_W; c++) {
            shadow[r][c].ch   = ' ';
            shadow[r][c].attr = COL_RESET;
        }
}

static void sc_putc(int row, int col, char ch, uint8_t attr) {
    if (row < 0 || row >= TERM_H || col < 0 || col >= TERM_W) return;
    shadow[row][col].ch   = (uint8_t)ch;
    shadow[row][col].attr = attr;
}

static void sc_puts(int row, int* col, const char* s, uint8_t attr) {
    while (*s && *col < TERM_W) {
        sc_putc(row, (*col)++, *s++, attr);
    }
}

static void sc_puts_n(int row, int* col, const char* s, int n, uint8_t attr) {
    for (int i = 0; i < n && *col < TERM_W && s[i]; i++)
        sc_putc(row, (*col)++, s[i], attr);
}

static void sc_pad(int row, int* col, int target, uint8_t attr) {
    while (*col < target && *col < TERM_W)
        sc_putc(row, (*col)++, ' ', attr);
}

static void sc_int(int row, int* col, int n, uint8_t attr) {
    if (n == 0) { sc_putc(row, (*col)++, '0', attr); return; }
    char buf[12]; int i = 11; buf[i] = 0;
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    sc_puts(row, col, &buf[i], attr);
}

static void sc_flush(void) {
    for (int r = 0; r < TERM_H; r++)
        for (int c = 0; c < TERM_W; c++) {
            int idx = r * TERM_W + c;
            VGA_BASE[idx] = ((uint16_t)shadow[r][c].attr << 8)
                          |  (uint16_t)shadow[r][c].ch;
        }
}

/* ── Local helpers ────────────────────────────────────────────────── */
static int h_str_len(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}

static int h_contains(const char* haystack, const char* needle) {
    if (!needle[0]) return 1;
    for (int i = 0; haystack[i]; i++) {
        int ok = 1;
        for (int j = 0; needle[j]; j++) {
            char hc = haystack[i + j];
            char nc = needle[j];
            if (!hc) { ok = 0; break; }
            if (hc >= 'A' && hc <= 'Z') hc += 32;
            if (nc >= 'A' && nc <= 'Z') nc += 32;
            if (hc != nc) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

/* ── Filtered index table ─────────────────────────────────────────── */
#define MAX_FILTERED 256
static int filtered[MAX_FILTERED];
static int filtered_count = 0;

static void h_rebuild_filter(const char* query) {
    filtered_count = 0;
    for (int i = 0; i < (int)command_count && filtered_count < MAX_FILTERED; i++) {
        if (h_contains(commands[i].name,        query) ||
            h_contains(commands[i].description, query)) {
            filtered[filtered_count++] = i;
        }
    }
}

/* ── Scrollbar ────────────────────────────────────────────────────── */
/*
   Draws a scrollbar in column SCROLLBAR_COL between rows 2 and
   2+VISIBLE_ROWS-1. The thumb position reflects selected/total.
*/
static void h_draw_scrollbar(int selected) {
    int track_h = VISIBLE_ROWS;
    int top_row = 2;

    for (int i = 0; i < track_h; i++)
        sc_putc(top_row + i, SCROLLBAR_COL, '|', COL_SCROLLBAR);

    if (filtered_count > 0) {
        int thumb = (selected * (track_h - 1)) / (filtered_count - 1 > 0
                    ? filtered_count - 1 : 1);
        sc_putc(top_row + thumb, SCROLLBAR_COL, '#', COL_SCROLLTHUMB);
    }
}

/* ── Draw ─────────────────────────────────────────────────────────── */
static void h_draw(int selected, int scroll,
                   const char* query, int search_mode,
                   bool shift, bool caps) {
    h_rebuild_filter(query); 
    sc_clear();

    /* ── Row 0: title bar ─────────────────────────────────────────── */
    {
        int c = 0;
        sc_pad(0, &c, TERM_W, COL_HEADER);   /* fill row with colour  */
        c = 0;
        sc_puts(0, &c, "  RadiumOS Help  ", COL_HEADER);
        /* right-align count */
        char cnt[32]; int ci = 0;
        cnt[ci++] = '[';
        int n = filtered_count;
        if (n == 0) { cnt[ci++] = '0'; }
        else {
            char nb[8]; int ni = 7; nb[ni] = 0;
            while (n > 0) { nb[--ni] = '0' + (n % 10); n /= 10; }
            for (int k = ni; nb[k]; k++) cnt[ci++] = nb[k];
        }
        const char* rest = " cmds]";
        for (int k = 0; rest[k]; k++) cnt[ci++] = rest[k];
        cnt[ci] = 0;
        int rc = TERM_W - ci - 1;
        if (rc < 0) rc = 0;
        int tmp = rc;
        sc_puts(0, &tmp, cnt, COL_HEADER);
    }

    /* ── Row 1: search bar ────────────────────────────────────────── */
    {
        int c = 0;
        sc_puts(1, &c, "  Search: /", search_mode ? COL_SEARCH : COL_HINT);
        sc_puts(1, &c, query,         search_mode ? COL_SEARCH : COL_HINT);
        if (search_mode) sc_putc(1, c++, '_', COL_SEARCH);
        if (query[0] && !search_mode) {
            sc_puts(1, &c, "  (", COL_HINT);
            sc_int(1, &c, filtered_count, COL_MATCH);
            sc_puts(1, &c, " results)", COL_HINT);
        }
        sc_pad(1, &c, TERM_W - 1, COL_HINT);
    }

    /* ── Rows 2..2+VISIBLE_ROWS-1: command list ───────────────────── */
    int end = scroll + VISIBLE_ROWS;
    if (end > filtered_count) end = filtered_count;

    for (int i = scroll; i < end; i++) {
        int row    = 2 + (i - scroll);
        int idx    = filtered[i];
        int is_sel = (i == selected);
        uint8_t row_attr = is_sel ? COL_SELECTED : COL_RESET;

        /* fill entire row with background colour */
        int c = 0;
        sc_pad(row, &c, TERM_W - 1, row_attr);
        c = 0;

        /* cursor indicator */
        if (is_sel) sc_puts(row, &c, " > ", COL_SELECTED);
        else        sc_puts(row, &c, "   ", COL_BORDER);

        /* command name */
        uint8_t name_attr = is_sel ? COL_SELECTED : COL_NAME;
        sc_puts(row, &c, commands[idx].name, name_attr);
        sc_pad(row, &c, 3 + COL_NAME_W, name_attr);

        /* description — truncate to fit */
        uint8_t desc_attr = is_sel ? COL_SELECTED : COL_DESC;
        const char* desc  = commands[idx].description;
        int dl            = h_str_len(desc);
        int max_desc      = TERM_W - 3 - COL_NAME_W - 2; /* -2 for scrollbar */
        if (dl > max_desc) {
            sc_puts_n(row, &c, desc, max_desc - 3, desc_attr);
            sc_puts(row, &c, "...", COL_BORDER);
        } else {
            sc_puts(row, &c, desc, desc_attr);
        }
    }

    /* ── Empty rows ───────────────────────────────────────────────── */
    for (int i = end - scroll; i < VISIBLE_ROWS; i++) {
        int row = 2 + i;
        int c   = 0;
        sc_pad(row, &c, TERM_W - 1, COL_RESET);
    }

    /* ── Scrollbar ────────────────────────────────────────────────── */
    h_draw_scrollbar(selected);

    /* ── Divider row ──────────────────────────────────────────────── */
    {
        int divrow = 2 + VISIBLE_ROWS;
        int c = 0;
        for (int i = 0; i < TERM_W; i++)
            sc_putc(divrow, c++, '-', COL_BORDER);
    }

    /* ── Status row ───────────────────────────────────────────────── */
    {
        int row = 2 + VISIBLE_ROWS + 1;
        int c   = 0;
        sc_puts(row, &c, "  [", COL_BORDER);
        sc_int(row, &c, filtered_count > 0 ? selected + 1 : 0, COL_SEARCH);
        sc_puts(row, &c, "/", COL_BORDER);
        sc_int(row, &c, filtered_count, COL_SEARCH);
        sc_puts(row, &c, "]  ", COL_BORDER);

        /* show active modifiers */
        if (shift) sc_puts(row, &c, "[SHIFT] ", COL_MATCH);
        if (caps)  sc_puts(row, &c, "[CAPS] ",  COL_MATCH);
    }

    /* ── Keybind rows ─────────────────────────────────────────────── */
    {
        int row = 2 + VISIBLE_ROWS + 2;
        int c   = 0;
        sc_puts(row, &c, "  ", COL_HINT);
        sc_puts(row, &c, "UP/DOWN",  COL_HEADER); sc_puts(row, &c, " navigate  ", COL_HINT);
        sc_puts(row, &c, "PgUp/Dn",  COL_HEADER); sc_puts(row, &c, " page  ",     COL_HINT);
        sc_puts(row, &c, "Home/End", COL_HEADER); sc_puts(row, &c, " jump  ",     COL_HINT);
        sc_puts(row, &c, "/",        COL_HEADER); sc_puts(row, &c, " search  ",   COL_HINT);
        sc_puts(row, &c, "ESC",      COL_HEADER); sc_puts(row, &c, " clear  ",    COL_HINT);
        sc_puts(row, &c, "ENTER",    COL_HEADER); sc_puts(row, &c, " run  ",      COL_HINT);
        sc_puts(row, &c, "Q",        COL_HEADER); sc_puts(row, &c, " quit",       COL_HINT);
    }

    /* ── Flush shadow to VGA in one shot ──────────────────────────── */
    sc_flush();
}

/* ── PS/2 port read ───────────────────────────────────────────────── */
static inline uint8_t ps2_read(void) {
    uint8_t sc;
    __asm__ volatile ("inb $0x60, %0" : "=a"(sc));
    return sc;
}

static inline int ps2_ready(void) {
    uint8_t st;
    __asm__ volatile ("inb $0x64, %0" : "=a"(st));
    return st & 0x01;
}

/* ── Key state tracking ───────────────────────────────────────────── */
/*
   We track every key's pressed state in a 128-entry bitmap.
   This lets us detect held keys without relying on the BIOS
   repeat mechanism (which causes the initial delay + stutter).
*/
static uint8_t key_held[128];   /* 1 = currently held down */

/* repeat counters per key */
static uint32_t key_repeat_counter[128];

/* ── Poll one scancode (non-blocking) ────────────────────────────── */
/*
   Returns 1 and sets *sc if a new scancode is available.
   Returns 0 if the port is empty.
*/
static int ps2_poll(uint8_t* sc) {
    if (!ps2_ready()) return 0;
    *sc = ps2_read();
    return 1;
}

/* ── Entry point ──────────────────────────────────────────────────── */
void help_command(int argc, char* argv[]) {
    char query[SEARCH_MAX + 1];
    query[0]  = 0;
    int query_len   = 0;
    int search_mode = 0;
    int selected    = 0;
    int scroll      = 0;

    bool shift = false;
    bool caps  = false;
    bool ctrl  = false;

    /* clear key state */
    for (int i = 0; i < 128; i++) {
        key_held[i]           = 0;
        key_repeat_counter[i] = 0;
    }

    /* optional initial query */
    if (argc > 1) {
        int al = h_str_len(argv[1]);
        if (al > SEARCH_MAX) al = SEARCH_MAX;
        for (int i = 0; i < al; i++) query[i] = argv[1][i];
        query[al] = 0;
        query_len = al;
    }

    h_rebuild_filter(query);
    h_draw(selected, scroll, query, search_mode, shift, caps);

    /* ── Main loop ────────────────────────────────────────────────── */
    while (1) {
        uint8_t sc = 0;
        int     got_new = ps2_poll(&sc);

        if (got_new) {
            if (sc & SC_RELEASE) {
                /* key release */
                uint8_t key = sc & ~SC_RELEASE;
                if (key < 128) {
                    key_held[key]           = 0;
                    key_repeat_counter[key] = 0;
                }
                if (key == SC_LSHIFT || key == SC_RSHIFT) shift = false;
                if (key == SC_LCTRL)                      ctrl  = false;
                continue;
            }

            /* key press */
            if (sc < 128) {
                if (!key_held[sc]) {
                    /* fresh press — reset repeat counter */
                    key_held[sc]           = 1;
                    key_repeat_counter[sc] = 0;
                }
                if (sc == SC_LSHIFT || sc == SC_RSHIFT) { shift = true;  continue; }
                if (sc == SC_LCTRL)                     { ctrl  = true;  continue; }
                if (sc == SC_CAPS)                      { caps  = !caps; continue; }
            }
        }

        /* ── Synthetic repeat for held keys ───────────────────────── */
        /*
           For each held key we increment its counter every loop.
           When it crosses REPEAT_DELAY we fire the first repeat,
           then fire again every REPEAT_RATE ticks after that.
           This gives smooth, immediate-feeling repeat with no
           OS-level delay artefacts.
        */
        int fire_sc = -1;
        for (int k = 0; k < 128; k++) {
            if (!key_held[k]) continue;
            key_repeat_counter[k]++;
            if (key_repeat_counter[k] == 1) {
                /* first press — fire immediately */
                fire_sc = k;
            } else if (key_repeat_counter[k] > REPEAT_DELAY &&
                       (key_repeat_counter[k] - REPEAT_DELAY) % REPEAT_RATE == 0) {
                fire_sc = k;
            }
        }

        if (fire_sc < 0) continue;
        sc = (uint8_t)fire_sc;

        /* ── Translate scancode to char ───────────────────────────── */
        char c = keyboard_to_char(sc, shift, caps);

        /* ── Search mode ──────────────────────────────────────────── */
        if (search_mode) {
            if (sc == SC_ESC) {
                search_mode = 0;
            } else if (sc == SC_BACKSPACE) {
                if (query_len > 0) {
                    query[--query_len] = 0;
                    h_rebuild_filter(query);
                    selected = 0; scroll = 0;
                }
            } else if (sc == SC_ENTER) {
                search_mode = 0;
            } else if (c >= 32 && c < 127 && query_len < SEARCH_MAX) {
                query[query_len++] = c;
                query[query_len]   = 0;
                h_rebuild_filter(query);
                selected = 0; scroll = 0;
            }
            h_draw(selected, scroll, query, search_mode, shift, caps);
            continue;
        }

        /* ── Normal mode ──────────────────────────────────────────── */

        /* Q — quit */
        if (c == 'q' || c == 'Q') break;

        /* / — search */
        if (c == '/') {
            search_mode = 1;
            h_draw(selected, scroll, query, search_mode, shift, caps);
            continue;
        }

        /* ESC — clear search */
        if (sc == SC_ESC) {
            query[0] = 0; query_len = 0;
            h_rebuild_filter(query);
            selected = 0; scroll = 0;
            h_draw(selected, scroll, query, search_mode, shift, caps);
            continue;
        }

        /* UP / k */
        if (sc == SC_UP || c == 'k') {
            if (selected > 0) {
                selected--;
                if (selected < scroll) scroll = selected;
            }
            h_draw(selected, scroll, query, search_mode, shift, caps);
            continue;
        }

        /* DOWN / j */
        if (sc == SC_DOWN || c == 'j') {
            if (selected < filtered_count - 1) {
                selected++;
                if (selected >= scroll + VISIBLE_ROWS)
                    scroll = selected - VISIBLE_ROWS + 1;
            }
            h_draw(selected, scroll, query, search_mode, shift, caps);
            continue;
        }

        /* PAGE UP / u */
        if (sc == SC_PGUP || c == 'u') {
            selected -= VISIBLE_ROWS;
            if (selected < 0) selected = 0;
            scroll = selected;
            if (scroll < 0) scroll = 0;
            h_draw(selected, scroll, query, search_mode, shift, caps);
            continue;
        }

        /* PAGE DOWN / d */
        if (sc == SC_PGDN || c == 'd') {
            selected += VISIBLE_ROWS;
            if (selected >= filtered_count) selected = filtered_count - 1;
            if (selected < 0) selected = 0;
            scroll = selected - VISIBLE_ROWS + 1;
            if (scroll < 0) scroll = 0;
            h_draw(selected, scroll, query, search_mode, shift, caps);
            continue;
        }

        /* HOME — jump to top */
        if (sc == SC_HOME) {
            selected = 0; scroll = 0;
            h_draw(selected, scroll, query, search_mode, shift, caps);
            continue;
        }

        /* END — jump to bottom */
        if (sc == SC_END) {
            selected = filtered_count > 0 ? filtered_count - 1 : 0;
            scroll   = selected - VISIBLE_ROWS + 1;
            if (scroll < 0) scroll = 0;
            h_draw(selected, scroll, query, search_mode, shift, caps);
            continue;
        }

        /* ENTER — run */
        if (sc == SC_ENTER && filtered_count > 0) {
            terminal_clear();
            reset_text_color();
            print("\nRunning: ");
            print(commands[filtered[selected]].name);
            print("\n");
            /* TODO: shell_execute(commands[filtered[selected]].name, 0, 0); */
            return;
        }

        h_draw(selected, scroll, query, search_mode, shift, caps);
    }

    terminal_clear();
    reset_text_color();
}