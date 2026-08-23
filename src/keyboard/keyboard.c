#include "keyboard.h"
#include <stddef.h>
#include "../terminal/terminal.h"
#include "../utility/utility.h"
#include "../errors/error.h"
#include "../io/io.h"
#include "../scheduler/task.h"
#include "../Avfs/Avfs.h"
#include <stdint.h>

// --- Backscroll Configuration ---
#define HISTORY_ROWS    2000
#define VGA_WIDTH       80
#define VGA_HEIGHT      50
#define VGA_BUFFER_SIZE (VGA_WIDTH * VGA_HEIGHT)
#define SCROLL_STEP     5   // lines to scroll per keypress

typedef struct {
    char    chars[VGA_WIDTH];
    uint8_t attrs[VGA_WIDTH];
} HistoryLine;

static HistoryLine history_buffer[HISTORY_ROWS];
static int history_total_lines = 0;
static int history_view_offset = 0;

// --- Global State ---
char   command_history[MAX_HISTORY][COMMAND_BUFFER_SIZE];
size_t history_count        = 0;
int    current_history_index = -1;

Command commands[MAX_COMMANDS];
size_t  command_count = 0;

bool shift_active     = false;
bool g_ctrl_pressed   = false;
bool caps_lock_active = false;
bool alt_active       = false;

static size_t cursor_position = 0;
static size_t selection_start = (size_t)-1;
static size_t selection_end   = (size_t)-1;

static char   clipboard[COMMAND_BUFFER_SIZE];
static size_t clipboard_len = 0;
#define FREEZE_CLIP_SIZE (VGA_WIDTH * VGA_HEIGHT * 3) // chars + \r\n per row, worst case
static char   freeze_clipboard[FREEZE_CLIP_SIZE];
static size_t freeze_clipboard_len = 0;
// --- Undo Snapshot ---
typedef struct {
    char   buf[COMMAND_BUFFER_SIZE];
    size_t len;
    size_t cursor;
    bool   valid;
} UndoSnapshot;

static UndoSnapshot undo_snapshot = { .valid = false };

// ============================================================
// Forward Declarations (Fixes compile errors)
// ============================================================
static void undo_save(const char *buf, size_t len, size_t cursor);
static bool undo_restore(char *buf, size_t *len);
static void freeze_enter(void);
static void freeze_exit(void);
static void freeze_move(int dcol, int drow);
static void freeze_repaint(void);
static void freeze_copy_selection(void);
static void display_prompt(void);
static void update_cursor_display_simple(const char *command_buffer, size_t command_length);
static void render_from_history(void);
static void scroll_by(int delta, char *command_buffer, size_t command_length);
static void scroll_byy(int delta);
static void redraw_command_line(const char *command, size_t *command_length);
static void delete_char_forward(char *command_buffer, size_t *command_length);
static void delete_word_backward(char *command_buffer, size_t *command_length);
static void delete_word_forward(char *command_buffer, size_t *command_length);
static void transpose_chars(char *command_buffer, size_t *command_length);
static bool is_word_char(char c);
static size_t word_left(const char *buf, size_t pos);
static size_t word_right(const char *buf, size_t pos, size_t len);
static bool process_extended_byte(uint8_t ext, char *command_buffer, size_t *command_length);
static bool freeze_handle_key(uint8_t scan_code, char *command_buffer, size_t *command_length);

// ============================================================
// Implementation of Helpers
// ============================================================

static void undo_save(const char *buf, size_t len, size_t cursor) {
    memcpy(undo_snapshot.buf, buf, len + 1);
    undo_snapshot.len    = len;
    undo_snapshot.cursor = cursor;
    undo_snapshot.valid  = true;
}

static bool undo_restore(char *buf, size_t *len) {
    if (!undo_snapshot.valid) return false;
    memcpy(buf, undo_snapshot.buf, undo_snapshot.len + 1);
    *len            = undo_snapshot.len;
    cursor_position = undo_snapshot.cursor;
    undo_snapshot.valid = false;
    return true;
}

// --- PS/2 Controller Helpers ---
static void kbd_wait_cmd(void) {
    int t = 100000;
    while (t-- && (inb(0x64) & 0x02));
}
static void kbd_wait_data(void) {
    int t = 100000;
    while (t-- && !(inb(0x64) & 0x01));
}
static void drain_keyboard_buffer(void) {
    while (inb(0x64) & 0x01) inb(0x60);
}

void display_prompt(void) {
    char username[COMMAND_BUFFER_SIZE];
    const char *cwd = avfs_getcwd();
    terminal_setcolor(VGA_COLOR_LIGHT_BROWN);
    if (avfs_file_exists("/etc/.username.cfg")) {
        if (avfs_get_content("/etc/.username.cfg", username, sizeof(username)) == 0)
            printr(username);
    } else {
        printr("default");
    }
    terminal_setcolor(VGA_COLOR_LIGHT_GREY); print(":");
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    if (strncmp(cwd, "/home/user", 10) == 0) {
        print("/~/user");
        if (strlen(cwd) > 10) print(cwd + 10);
    } else {
        print(cwd);
    }
    terminal_setcolor(VGA_COLOR_CYAN); print("$ ");
    terminal_setcolor(VGA_COLOR_LIGHT_GREY);
    terminal_update_cursor();
}

void update_cursor_display_simple(const char *command_buffer, size_t command_length) {
    terminal_putchar('\r');
    for (int i = 0; i < 79; i++) terminal_putchar(' ');
    terminal_putchar('\r');

    display_prompt();

    size_t sel_min = selection_start;
    size_t sel_max = selection_end;
    if (selection_start != (size_t)-1 && selection_end != (size_t)-1) {
        if (sel_min > sel_max) { size_t t = sel_min; sel_min = sel_max; sel_max = t; }
    }
    bool has_selection = (selection_start != (size_t)-1 &&
                          selection_start != selection_end);

    for (size_t i = 0; i < command_length; i++) {
        if (has_selection && i >= sel_min && i <= sel_max)
            terminal_setcolor(VGA_COLOR_BLACK | (VGA_COLOR_LIGHT_GREY << 4));
        else
            terminal_setcolor(VGA_COLOR_LIGHT_GREY);

        if (i == cursor_position) {
            char c = command_buffer[i];
            terminal_setcolor(VGA_COLOR_CYAN);
            if      (c >= 'a' && c <= 'z') terminal_putchar(c - 'a' + 'A');
            else if (c >= 'A' && c <= 'Z') terminal_putchar(c - 'A' + 'a');
            else                           terminal_putchar(c);
            terminal_setcolor(VGA_COLOR_WHITE);
        } else {
            terminal_putchar(command_buffer[i]);
        }
    }
    if (cursor_position == command_length) {
        terminal_setcolor(VGA_COLOR_CYAN);
        
    }
    terminal_setcolor(VGA_COLOR_LIGHT_GREY);
    terminal_update_cursor();
}

#define COM1_PORT 0x3F8
static void    serial_outb_local(uint16_t port, uint8_t data) { port_byte_out(port, data); }
static uint8_t serial_inb_local(uint16_t port)                { return port_byte_in(port); }

void init_serial_port(uint16_t port) {
    serial_outb_local(port + 1, 0x00);
    serial_outb_local(port + 3, 0x80);
    serial_outb_local(port + 0, 0x03);
    serial_outb_local(port + 1, 0x00);
    serial_outb_local(port + 3, 0x03);
    serial_outb_local(port + 2, 0xC7);
    serial_outb_local(port + 4, 0x0B);
}

static void serial_flush(uint16_t port) {
    // Wait until transmitter holding register and shift register are both empty
    // Bit 5 = THR empty, bit 6 = THR + TSR both empty (fully transmitted)
    int timeout = 100000;
    while (timeout-- && (serial_inb_local(port + 5) & 0x40) == 0);
}

void serial_write_string(uint16_t port, const char *str) {
    while (*str) {
        while ((serial_inb_local(port + 5) & 0x20) == 0);
        serial_outb_local(port, *str++);
    }
    // Terminating newline
    while ((serial_inb_local(port + 5) & 0x20) == 0);
    serial_outb_local(port, '\n');
    // Blank line — triggers the makefile bridge flush accumulator
    while ((serial_inb_local(port + 5) & 0x20) == 0);
    serial_outb_local(port, '\n');
    // Wait for shift register to fully drain before returning
    serial_flush(port);
}
// ============================================================
// VGA buffer — single declaration, used everywhere
// ============================================================
static volatile uint16_t *vga_buf = (volatile uint16_t *)0xB8000;

static uint16_t frozen_screen[VGA_BUFFER_SIZE];
static bool     freeze_active     = false;
static int      freeze_cursor_col = 0;
static int      freeze_cursor_row = 0;
static int      freeze_sel_start  = -1;
static int      freeze_sel_end    = -1;
static bool blur_active = false;
// add alongside blur_active
static bool freeze_copy_mode = false;  // true = Insert mode, false = Ctrl+Insert blur mode


static void freeze_pixelate_selection(void) {
    static const uint8_t px_chars[] = { 0xDB, 0xB2, 0xB1, 0xB0, 0xDC, 0xDD };
    static const uint8_t px_attrs[] = { 0x08, 0x07, 0x08, 0x07, 0x08, 0x07 };
    const int px_len = 6;

    int s, e;
    if (freeze_sel_start == -1 || freeze_sel_end == -1) {
        s = 0;
        e = VGA_BUFFER_SIZE - 1;
    } else {
        s = freeze_sel_start;
        e = freeze_sel_end;
        if (s > e) { int t = s; s = e; e = t; }
        if (s >= VGA_BUFFER_SIZE) s = VGA_BUFFER_SIZE - 1;
        if (e >= VGA_BUFFER_SIZE) e = VGA_BUFFER_SIZE - 1;
    }

    for (int i = s; i <= e; i++) {
        int idx = (i * 3 + (i / VGA_WIDTH) * 7) % px_len;
        frozen_screen[i] = ((uint16_t)px_attrs[idx] << 8) | px_chars[idx];
    }
    blur_active = true;
    freeze_repaint();
}

static void freeze_pixelate_clear(void) {
    // Restore from a fresh snapshot — re-enter freeze to reset
    blur_active = false;
    for (int i = 0; i < VGA_BUFFER_SIZE; i++)
        frozen_screen[i] = vga_buf[i];
    freeze_sel_start = -1;
    freeze_sel_end   = -1;
    freeze_repaint();
}


// ============================================================
// SCREEN FREEZE / HIGHLIGHT MODE
// ============================================================


#define FREEZE_CURSOR_ATTR 0x70
#define FREEZE_SEL_ATTR    0x30

bool g_just_froze_copy = false;

static inline int freeze_flat(int col, int row) {
    return row * VGA_WIDTH + col;
}

static void freeze_repaint(void) {
    for (int i = 0; i < VGA_BUFFER_SIZE; i++)
        vga_buf[i] = frozen_screen[i];

    // Only draw selection highlight when not blurred — blur writes
    // directly into frozen_screen so no overlay is needed or wanted
    if (!blur_active && freeze_sel_start != -1 && freeze_sel_end != -1) {
        int s = freeze_sel_start, e = freeze_sel_end;
        if (s > e) { int t = s; s = e; e = t; }
        for (int i = s; i <= e; i++) {
            uint8_t ch = frozen_screen[i] & 0xFF;
            vga_buf[i] = ((uint16_t)FREEZE_SEL_ATTR << 8) | ch;
        }
    }

    int     cidx = freeze_flat(freeze_cursor_col, freeze_cursor_row);
    uint8_t ch   = frozen_screen[cidx] & 0xFF;
    vga_buf[cidx] = ((uint16_t)FREEZE_CURSOR_ATTR << 8) | ch;
}

static void freeze_move(int dcol, int drow) {
    int old_flat = freeze_flat(freeze_cursor_col, freeze_cursor_row);

    freeze_cursor_col += dcol;
    freeze_cursor_row += drow;

    if (freeze_cursor_col < 0)           freeze_cursor_col = 0;
    if (freeze_cursor_col >= VGA_WIDTH)  freeze_cursor_col = VGA_WIDTH  - 1;
    if (freeze_cursor_row < 0)           freeze_cursor_row = 0;
    if (freeze_cursor_row >= VGA_HEIGHT) freeze_cursor_row = VGA_HEIGHT - 1;

    int new_flat = freeze_flat(freeze_cursor_col, freeze_cursor_row);

    if (shift_active) {
        if (freeze_sel_start == -1) freeze_sel_start = old_flat;
        freeze_sel_end = new_flat;
    } else {
        freeze_sel_start = -1;
        freeze_sel_end   = -1;
    }
    freeze_repaint();
}

static void freeze_copy_selection(void) {
    if (freeze_sel_start == -1 || freeze_sel_end == -1) return;
    int s = freeze_sel_start, e = freeze_sel_end;
    if (s > e) { int t = s; s = e; e = t; }
    if (s >= VGA_BUFFER_SIZE) s = VGA_BUFFER_SIZE - 1;
    if (e >= VGA_BUFFER_SIZE) e = VGA_BUFFER_SIZE - 1;

    size_t out = 0;
    for (int i = s; i <= e; i++) {
        if (out >= FREEZE_CLIP_SIZE - 3) break;
        uint8_t ch = (uint8_t)(frozen_screen[i] & 0xFF);
        if (ch == '\0' || ch < 0x20) ch = ' '; // replace non-printable with space
        freeze_clipboard[out++] = ch;
        // Insert line break at end of each VGA row within the selection
        if ((i % VGA_WIDTH) == (VGA_WIDTH - 1)) {
            freeze_clipboard[out++] = '\r';
            freeze_clipboard[out++] = '\n';
        }
    }
    freeze_clipboard[out] = '\0';
    freeze_clipboard_len  = out;
    g_just_froze_copy     = true;

    serial_write_string(COM1_PORT, freeze_clipboard);
}



static void freeze_enter(void) {
    freeze_active     = true;
    g_just_froze_copy = false;
    for (int i = 0; i < VGA_BUFFER_SIZE; i++)
        frozen_screen[i] = vga_buf[i];
    freeze_cursor_col = 0;
    freeze_cursor_row = 0;
    freeze_sel_start  = -1;
    freeze_sel_end    = -1;
    freeze_repaint();
}

static void freeze_exit(void) {
    freeze_active = false;
    for (int i = 0; i < VGA_BUFFER_SIZE; i++)
        vga_buf[i] = frozen_screen[i];
}

static bool process_extended_byte(uint8_t ext, char *command_buffer, size_t *command_length) {
    bool ctrl  = g_ctrl_pressed;
    bool shift = shift_active;

    // Break codes
    if (ext & 0x80) {
        uint8_t key = ext & 0x7F;
        if (key == 0x1D) g_ctrl_pressed = false;  // Right Ctrl release
        if (key == 0x38) alt_active     = false;   // Right Alt release
        if (key == 0x2A) shift_active   = false;   // Shift release
        if (key == 0x36) shift_active   = false;   // Right Shift release
        return true;
    }

    // Modifiers
    if (ext == 0x1D) { g_ctrl_pressed = true; return true; }
    if (ext == 0x38) { alt_active     = true;  return true; }
    if (ext == 0x2A) { shift_active   = true;  return true; }
    if (ext == 0x36) { shift_active   = true;  return true; }

    // Ctrl + Shift (Word selection / small scroll)
    if (ctrl && shift) {
        if (ext == 0x4B) {
            size_t old = cursor_position;
            cursor_position = word_left(command_buffer, cursor_position);
            if (selection_start == (size_t)-1) selection_start = old;
            selection_end = cursor_position;
            update_cursor_display_simple(command_buffer, *command_length);
            return true;
        }
        if (ext == 0x4D) {
            size_t old = cursor_position;
            cursor_position = word_right(command_buffer, cursor_position, *command_length);
            if (selection_start == (size_t)-1) selection_start = old;
            selection_end = cursor_position;
            update_cursor_display_simple(command_buffer, *command_length);
            return true;
        }
        // Scroll by step (Ctrl+Shift+Up/Down) if you want line-by-line
        if (ext == 0x48) { 
            if (history_view_offset < history_total_lines - VGA_HEIGHT) {
                history_view_offset += SCROLL_STEP;
                render_from_history();
            }
            return true; 
        }
        if (ext == 0x50) { 
            if (history_view_offset > 0) {
                history_view_offset -= SCROLL_STEP;
                if (history_view_offset < 0) history_view_offset = 0;
                render_from_history();
            }
            return true; 
        }
    }

    // Ctrl only (Word movement)
    if (ctrl) {
        if (ext == 0x4B) {
            selection_start = (size_t)-1; selection_end = (size_t)-1;
            cursor_position = word_left(command_buffer, cursor_position);
            update_cursor_display_simple(command_buffer, *command_length);
            return true;
        }
        if (ext == 0x4D) {
            selection_start = (size_t)-1; selection_end = (size_t)-1;
            cursor_position = word_right(command_buffer, cursor_position, *command_length);
            update_cursor_display_simple(command_buffer, *command_length);
            return true;
        }
        if (ext == 0x47) { // Home (Top of history)
            int max_offset = history_total_lines - VGA_HEIGHT;
            if (max_offset < 0) max_offset = 0;
            history_view_offset = max_offset;
            render_from_history();
            return true;
        }
        if (ext == 0x4F) { // End (Bottom of history)
            history_view_offset = 0;
            render_from_history();
            // When returning to bottom, we must redraw the prompt
            update_cursor_display_simple(command_buffer, *command_length);
            return true;
        }
        return true;
    }

    // ==========================================
    // PAGE UP / PAGE DOWN (History Scrolling)
    // ==========================================
    if (ext == 0x49) { // Page Up
        int max_offset = history_total_lines - VGA_HEIGHT;
        if (max_offset < 0) max_offset = 0;

        // Scroll up by a full page (or remaining lines)
        if (history_view_offset < max_offset) {
            history_view_offset += VGA_HEIGHT;
            if (history_view_offset > max_offset) history_view_offset = max_offset;
            render_from_history();
        }
        return true;
    }

    if (ext == 0x51) { // Page Down
        if (history_view_offset > 0) {
            history_view_offset -= VGA_HEIGHT;
            if (history_view_offset < 0) history_view_offset = 0;
            render_from_history();
            
            // If we returned to the bottom, refresh the prompt line
            if (history_view_offset == 0) {
                update_cursor_display_simple(command_buffer, *command_length);
            }
        }
        return true;
    }

    // Shift only (Selection)
    if (shift) {
        if (ext == 0x4B) { // Left
            if (cursor_position > 0) {
                if (selection_start == (size_t)-1) selection_start = cursor_position;
                cursor_position--;
                selection_end = cursor_position;
                update_cursor_display_simple(command_buffer, *command_length);
            }
            return true;
        }
        if (ext == 0x4D) { // Right
            if (cursor_position < *command_length) {
                if (selection_start == (size_t)-1) selection_start = cursor_position;
                cursor_position++;
                selection_end = cursor_position;
                update_cursor_display_simple(command_buffer, *command_length);
            }
            return true;
        }
        if (ext == 0x47) { // Home
            if (selection_start == (size_t)-1) selection_start = cursor_position;
            cursor_position = 0;
            selection_end = cursor_position;
            update_cursor_display_simple(command_buffer, *command_length);
            return true;
        }
        if (ext == 0x4F) { // End
            if (selection_start == (size_t)-1) selection_start = cursor_position;
            cursor_position = *command_length;
            selection_end = cursor_position;
            update_cursor_display_simple(command_buffer, *command_length);
            return true;
        }
        // Pass arrow keys to standard logic for command history navigation
        if (ext == 0x48) goto plain_up;
        if (ext == 0x50) goto plain_down;
    }

    // No modifier (Navigation)
    if (ext == 0x4B) { // Left
        selection_start = (size_t)-1; selection_end = (size_t)-1;
        if (cursor_position > 0) {
            cursor_position--;
            update_cursor_display_simple(command_buffer, *command_length);
        }
        return true;
    }
    if (ext == 0x4D) { // Right
        selection_start = (size_t)-1; selection_end = (size_t)-1;
        if (cursor_position < *command_length) {
            cursor_position++;
            update_cursor_display_simple(command_buffer, *command_length);
        }
        return true;
    }
    if (ext == 0x47) { // Home
        selection_start = (size_t)-1; selection_end = (size_t)-1;
        cursor_position = 0;
        update_cursor_display_simple(command_buffer, *command_length);
        return true;
    }
    if (ext == 0x4F) { // End
        selection_start = (size_t)-1; selection_end = (size_t)-1;
        cursor_position = *command_length;
        update_cursor_display_simple(command_buffer, *command_length);
        return true;
    }
    if (ext == 0x53) { // Delete
        delete_char_forward(command_buffer, command_length);
        return true;
    }

plain_up:
    // Standard Up Arrow: Command History
    if (ext == 0x48) {
        if (history_count > 0) {
            if (current_history_index == -1)
                current_history_index = (int)history_count - 1;
            else if (current_history_index > 0)
                current_history_index--;
            strncpy(command_buffer, command_history[current_history_index], COMMAND_BUFFER_SIZE);
            command_buffer[COMMAND_BUFFER_SIZE - 1] = '\0';
            redraw_command_line(command_buffer, command_length);
        }
        return true;
    }

plain_down:
    // Standard Down Arrow: Command History
    if (ext == 0x50) {
        if (history_count > 0 && current_history_index != -1) {
            if (current_history_index < (int)history_count - 1) {
                current_history_index++;
                strncpy(command_buffer, command_history[current_history_index], COMMAND_BUFFER_SIZE);
            } else {
                current_history_index = -1;
                command_buffer[0] = '\0';
            }
            redraw_command_line(command_buffer, command_length);
        }
        return true;
    }

    return true;
}


static bool freeze_handle_key(uint8_t scan_code, char *command_buffer, size_t *command_length) {
    // F7: enter copy mode / exit
    if (scan_code == 0x41) {
        if (!freeze_active) { freeze_copy_mode = true;  freeze_enter(); }
        else                { freeze_exit(); }
        return true;
    }
    // F8: enter blur mode / exit
    if (scan_code == 0x42) {
        if (!freeze_active) { freeze_copy_mode = false; freeze_enter(); }
        else                { blur_active = false; freeze_exit(); }
        return true;
    }

    if (scan_code == 0xE0) {
        int timeout = 100000;
        while (timeout-- && !(inb(0x64) & 0x01));
        if (timeout <= 0) return true;
        uint8_t ext = inb(0x60);

        if (freeze_active) {
            if (ext & 0x80) {
                uint8_t key = ext & 0x7F;
                if (key == 0x2A || key == 0x36) shift_active   = false;
                if (key == 0x1D)                g_ctrl_pressed  = false;
                return true;
            }
            if (ext == 0x2A || ext == 0x36) { shift_active   = true;  return true; }
            if (ext == 0x1D)                { g_ctrl_pressed = true;  return true; }
            if (ext == 0x4B) { freeze_move(-1,  0); return true; }
            if (ext == 0x4D) { freeze_move( 1,  0); return true; }
            if (ext == 0x48) { freeze_move( 0, -1); return true; }
            if (ext == 0x50) { freeze_move( 0,  1); return true; }
            if (ext == 0x47) {
                int old = freeze_flat(freeze_cursor_col, freeze_cursor_row);
                freeze_cursor_col = 0;
                if (shift_active) {
                    if (freeze_sel_start == -1) freeze_sel_start = old;
                    freeze_sel_end = freeze_flat(freeze_cursor_col, freeze_cursor_row);
                } else { freeze_sel_start = -1; freeze_sel_end = -1; }
                freeze_repaint();
                return true;
            }
            if (ext == 0x4F) {
                int old = freeze_flat(freeze_cursor_col, freeze_cursor_row);
                freeze_cursor_col = VGA_WIDTH - 1;
                if (shift_active) {
                    if (freeze_sel_start == -1) freeze_sel_start = old;
                    freeze_sel_end = freeze_flat(freeze_cursor_col, freeze_cursor_row);
                } else { freeze_sel_start = -1; freeze_sel_end = -1; }
                freeze_repaint();
                return true;
            }
            return true;
        }
        return process_extended_byte(ext, command_buffer, command_length);
    }

    if (freeze_active) {
        if (scan_code & 0x80) {
            uint8_t key = scan_code & 0x7F;
            if (key == 0x2A || key == 0x36) shift_active   = false;
            if (key == 0x1D)                g_ctrl_pressed  = false;
            return true;
        }
        if (scan_code == 0x2A || scan_code == 0x36) { shift_active   = true;  return true; }
        if (scan_code == 0x1D)                      { g_ctrl_pressed = true;  return true; }

        // Copy mode: F7 already entered, Ctrl+Shift+C copies
        if (freeze_copy_mode && g_ctrl_pressed && shift_active && scan_code == 0x2E) {
            freeze_copy_selection();
            return true;
        }
        // Blur mode: F6 toggles pixelation
        if (!freeze_copy_mode && scan_code == 0x40) {
            if (blur_active) freeze_pixelate_clear();
            else             freeze_pixelate_selection();
            return true;
        }
        // Escape exits either mode
        if (scan_code == 0x01) {
            blur_active      = false;
            freeze_copy_mode = false;
            freeze_exit();
            return true;
        }
        return true;
    }

    return false;
}
// ============================================================
// History
// ============================================================
void history_init(void) {
    history_total_lines = 0;
    history_view_offset = 0;
    for (int i = 0; i < HISTORY_ROWS; i++) {
        memset(history_buffer[i].chars, ' ', VGA_WIDTH);
        memset(history_buffer[i].attrs, 0,   VGA_WIDTH);
    }
}

void history_append_line(const char *str, uint8_t color) {
    HistoryLine *target;
    if (history_total_lines < HISTORY_ROWS) {
        target = &history_buffer[history_total_lines++];
    } else {
        memmove(&history_buffer[0], &history_buffer[1],
                sizeof(HistoryLine) * (HISTORY_ROWS - 1));
        target = &history_buffer[HISTORY_ROWS - 1];
    }
    memset(target->chars, ' ', VGA_WIDTH);
    memset(target->attrs, color, VGA_WIDTH);
    if (str) {
        for (int i = 0; i < VGA_WIDTH && str[i]; i++)
            target->chars[i] = str[i];
    }
}

static void render_from_history(void) {
    // We need to fill the whole screen
    for (int row = 0; row < VGA_HEIGHT; row++) {
        
        // Calculate which index in the history buffer corresponds to this screen row
        // history_view_offset = 0 means we are at the bottom (newest)
        // history_view_offset > 0 means we are looking back (older)
        
        int history_index = (history_total_lines - VGA_HEIGHT + row) - history_view_offset;

        // Check bounds to ensure we don't read outside the history buffer
        if (history_index >= 0 && history_index < history_total_lines) {
            HistoryLine *line = &history_buffer[history_index];
            for (int col = 0; col < VGA_WIDTH; col++) {
                uint8_t ch   = (uint8_t)line->chars[col];
                uint8_t attr = line->attrs[col];
                if (ch == 0) ch = ' ';
                vga_buf[row * VGA_WIDTH + col] = ((uint16_t)attr << 8) | ch;
            }
        } else {
            // If history_index is out of bounds (not enough history to fill screen), 
            // fill with blank lines.
            uint16_t blank = (' ' | (VGA_COLOR_LIGHT_GREY << 8));
            for (int col = 0; col < VGA_WIDTH; col++)
                vga_buf[row * VGA_WIDTH + col] = blank;
        }
    }
}


// ============================================================
// Screen shift helpers (physical VGA scroll, no history)
// ============================================================
static void scroll_screen_down(void) {
    for (int row = VGA_HEIGHT - 2; row >= 0; row--)
        for (int col = 0; col < VGA_WIDTH; col++)
            vga_buf[(row + 1) * VGA_WIDTH + col] = vga_buf[row * VGA_WIDTH + col];
    uint16_t blank = (' ' | (VGA_COLOR_LIGHT_GREY << 8));
    for (int col = 0; col < VGA_WIDTH; col++)
        vga_buf[col] = blank;
}

static void scroll_screen_up(void) {
    for (int row = 0; row < VGA_HEIGHT - 1; row++)
        for (int col = 0; col < VGA_WIDTH; col++)
            vga_buf[row * VGA_WIDTH + col] = vga_buf[(row + 1) * VGA_WIDTH + col];
    uint16_t blank = (' ' | (VGA_COLOR_LIGHT_GREY << 8));
    for (int col = 0; col < VGA_WIDTH; col++)
        vga_buf[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = blank;
}
static void scroll_by(int delta, char *command_buffer, size_t command_length) {
    if (delta > 0) {
        for (int i = 0; i < delta; i++) {
            scroll_screen_down();
        }
    } else if (delta < 0) {
        for (int i = 0; i < -delta; i++) {
            scroll_screen_up();
        }
    }
}
static void scroll_byy(int delta) {
    if (delta > 0) {
        for (int i = 0; i < delta; i++)
            scroll_screen_down();
    } else if (delta < 0) {
        for (int i = 0; i < -delta; i++)
            scroll_screen_up();
    }
}
// ============================================================
// Terminal / prompt helpers
// ============================================================
void init_keyboard(void) {
    kbd_wait_cmd(); outb(0x60, 0xAD);
    drain_keyboard_buffer();
    kbd_wait_cmd(); outb(0x64, 0xAA);
    kbd_wait_data();
    uint8_t status = inb(0x60);
    if (status != 0x55) print("Keyboard Self-Test Failed\n");
    drain_keyboard_buffer();
    kbd_wait_cmd(); outb(0x60, 0xAE);
}



int register_command(const char *name, const char *description,
                     void (*execute)(int, char *[])) {
    if (command_count >= MAX_COMMANDS) return 0;
    if (!name || !description || !execute) return 0;
    for (size_t i = 0; i < command_count; i++)
        if (strcmp(commands[i].name, name) == 0) return 0;
    commands[command_count].name        = name;
    commands[command_count].description = description;
    commands[command_count].execute     = execute;
    command_count++;
    return 1;
}

void add_to_history(const char *command) {
    if (strlen(command) == 0) return;
    if (history_count >= MAX_HISTORY) {
        for (int i = 0; i < MAX_HISTORY - 1; i++)
            strncpy(command_history[i], command_history[i + 1], COMMAND_BUFFER_SIZE);
        strncpy(command_history[MAX_HISTORY - 1], command, COMMAND_BUFFER_SIZE);
    } else {
        strncpy(command_history[history_count++], command, COMMAND_BUFFER_SIZE);
    }
    current_history_index = -1;
}



static void redraw_command_line(const char *command, size_t *command_length) {
    *command_length = strlen(command);
    cursor_position = *command_length;
    update_cursor_display_simple(command, *command_length);
}

__attribute__((weak))
int tab_complete(char *command_buffer, size_t *command_length) {
    (void)command_buffer; (void)command_length; return 0;
}

bool is_key_pressed(void) { return (port_byte_in(0x64) & 0x01) != 0; }

void keyboard_await(const char *message, bool clear_screen) {
    if (clear_screen) terminal_clear();
    if (message != NULL) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        print(message);
        for (int i = 0; i < 1000; i++) port_byte_in(0x80);
    }
    uint8_t scancode;
    while (1) {
        while ((port_byte_in(0x64) & 0x01) == 0) {}
        scancode = port_byte_in(0x60);
        if (scancode & 0x80) continue;
        if (scancode == 0xE0) continue;
        break;
    }
}

void execute_command_extern(const char *cmd) {
    if (!cmd || cmd[0] == '\0') return;

    // Make a mutable copy since strtok modifies the string
    char buf[COMMAND_BUFFER_SIZE];
    strncpy(buf, cmd, COMMAND_BUFFER_SIZE - 1);
    buf[COMMAND_BUFFER_SIZE - 1] = '\0';

    // Tokenize into argc/argv
    char *argv[MAX_ARGUMENTS];
    int   argc  = 0;
    char *token = strtok(buf, " ");
    while (token && argc < MAX_ARGUMENTS) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }
    if (argc == 0) return;

    // Search registered command table
    for (size_t i = 0; i < command_count; i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            commands[i].execute(argc, argv);
            return;
        }
    }

    // Not found — print error
    print("! unknown command: ");
    print(argv[0]);
    print(" !\n");
}

void execute_command(const char *command) {
    if (strcmp(command, "") == 0) return;
    add_to_history(command);
    char *argv[MAX_ARGUMENTS];
    int   argc  = 0;
    char *token = strtok((char *)command, " ");
    while (token && argc < MAX_ARGUMENTS) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }
    if (argc == 0) return;

    // "> file" sends a command's output to a file, ">> file" appends
    // (echo handles its own redirection)
    int redirect_index = -1;
    int append_mode = 0;
    if (strcmp(argv[0], "echo") != 0) {
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], ">") == 0) { redirect_index = i; break; }
            if (strcmp(argv[i], ">>") == 0) { redirect_index = i; append_mode = 1; break; }
        }
    }

    for (size_t i = 0; i < command_count; i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            if (redirect_index == -1) {
                commands[i].execute(argc, argv);
            } else {
                if (redirect_index + 1 >= argc) {
                    print("\n! missing filename after ");
                    print(append_mode ? ">>" : ">");
                    print(" !\n");
                    return;
                }

                char *file = argv[redirect_index + 1];

                if (avfs_is_directory(file)) {
                    print("\n! ");
                    print(file);
                    print(" is a directory !\n");
                    return;
                }

                if (append_mode) {
                    // >> keeps existing contents; only create if missing
                    if (!avfs_file_exists(file) && avfs_create_file(file, 0) != 0) {
                        print("\n! could not create ");
                        print(file);
                        print(" !\n");
                        return;
                    }
                } else {
                    if (avfs_file_exists(file) && avfs_remove_file(file) != 0) {
                        print("\n! could not replace ");
                        print(file);
                        print(" !\n");
                        return;
                    }

                    if (avfs_create_file(file, 0) != 0) {
                        print("\n! could not create ");
                        print(file);
                        print(" !\n");
                        return;
                    }
                }

                if (terminal_begin_capture(file) != 0) {
                    print("\n! could not start capture for ");
                    print(file);
                    print(" !\n");
                    return;
                }
                commands[i].execute(redirect_index, argv);
                if (terminal_end_capture() != 0) {
                    print("\n! disk full while writing ");
                    print(file);
                    print(" !\n");
                }
            }
            return;
        }
    }
    print("\n! unknown command !\n");
}

// ============================================================
// Keyboard maps
// ============================================================
const char keyboard_map[128] = {
    0, 0, '1','2','3','4','5','6','7','8','9','0','-','=', 0, 0,
    'q','w','e','r','t','y','u','i','o','p','[',']', 0, 0,'a','s',
    'd','f','g','h','j','k','l',';','\'','`', 0,'\\','z','x','c',
    'v','b','n','m',',','.','/', 0, 0, 0,' ', 0, 0, 0, 0, 0, 0,
};
const char shifted_keyboard_map[128] = {
    0, 0,'!','@','#','$','%','^','&','*','(',')','_','+', 0, 0,
    'Q','W','E','R','T','Y','U','I','O','P','{','}', 0, 0,'A','S',
    'D','F','G','H','J','K','L',':','"','~', 0,'|','Z','X','C',
    'V','B','N','M','<','>','?', 0, 0, 0,' ', 0, 0, 0, 0, 0, 0,
};

// ============================================================
// Edit helpers
// ============================================================
void perform_copy(char *command_buffer) {
    if (selection_start == (size_t)-1 || selection_start == selection_end) return;
    size_t mn = (selection_start < selection_end) ? selection_start : selection_end;
    size_t mx = (selection_start > selection_end) ? selection_start : selection_end;
    size_t len = mx - mn;
    if (len > 0 && len < COMMAND_BUFFER_SIZE) {
        memcpy(clipboard, command_buffer + mn, len);
        clipboard_len  = len;
        clipboard[len] = '\0';
        serial_write_string(COM1_PORT, clipboard);
    }
}

void perform_cut(char *command_buffer, size_t *command_length) {
    if (selection_start == (size_t)-1 || selection_start == selection_end) return;
    undo_save(command_buffer, *command_length, cursor_position);
    perform_copy(command_buffer);
    size_t mn  = (selection_start < selection_end) ? selection_start : selection_end;
    size_t mx  = (selection_start > selection_end) ? selection_start : selection_end;
    size_t len = mx - mn;
    if (len > 0) {
        memmove(command_buffer + mn, command_buffer + mx, *command_length - mx);
        *command_length -= len;
        command_buffer[*command_length] = '\0';
        cursor_position = mn;
        selection_start = (size_t)-1;
        selection_end   = (size_t)-1;
        update_cursor_display_simple(command_buffer, *command_length);
    }
}

void perform_paste(char *command_buffer, size_t *command_length) {
    if (clipboard_len == 0) return;
    if (*command_length + clipboard_len >= COMMAND_BUFFER_SIZE - 1) return;
    undo_save(command_buffer, *command_length, cursor_position);
    for (size_t i = *command_length; i > cursor_position; i--)
        command_buffer[i + clipboard_len - 1] = command_buffer[i - 1];
    memcpy(command_buffer + cursor_position, clipboard, clipboard_len);
    *command_length += clipboard_len;
    command_buffer[*command_length] = '\0';
    cursor_position += clipboard_len;
    selection_start  = (size_t)-1;
    selection_end    = (size_t)-1;
    update_cursor_display_simple(command_buffer, *command_length);
}

void insert_char_at_cursor(char *command_buffer, size_t *command_length, char c) {
    if (*command_length >= COMMAND_BUFFER_SIZE - 1) return;
    undo_save(command_buffer, *command_length, cursor_position);
    for (size_t i = *command_length; i > cursor_position; i--)
        command_buffer[i] = command_buffer[i - 1];
    command_buffer[cursor_position] = c;
    (*command_length)++;
    command_buffer[*command_length] = '\0';
    cursor_position++;
    selection_start = (size_t)-1;
    selection_end   = (size_t)-1;
    update_cursor_display_simple(command_buffer, *command_length);
}

void delete_char_at_cursor(char *command_buffer, size_t *command_length) {
    if (cursor_position == 0 || *command_length == 0) return;
    undo_save(command_buffer, *command_length, cursor_position);
    for (size_t i = cursor_position - 1; i < *command_length - 1; i++)
        command_buffer[i] = command_buffer[i + 1];
    (*command_length)--;
    cursor_position--;
    command_buffer[*command_length] = '\0';
    selection_start = (size_t)-1;
    selection_end   = (size_t)-1;
    update_cursor_display_simple(command_buffer, *command_length);
    terminal_update_cursor();
}

static void delete_char_forward(char *command_buffer, size_t *command_length) {
    if (cursor_position >= *command_length) return;
    undo_save(command_buffer, *command_length, cursor_position);
    for (size_t i = cursor_position; i < *command_length - 1; i++)
        command_buffer[i] = command_buffer[i + 1];
    (*command_length)--;
    command_buffer[*command_length] = '\0';
    selection_start = (size_t)-1;
    selection_end   = (size_t)-1;
    update_cursor_display_simple(command_buffer, *command_length);
    terminal_update_cursor();
}

static void delete_word_backward(char *command_buffer, size_t *command_length) {
    if (cursor_position == 0) return;
    undo_save(command_buffer, *command_length, cursor_position);
    size_t del_start = cursor_position;
    while (del_start > 0 &&
           (command_buffer[del_start-1]==' ' || command_buffer[del_start-1]=='\t'))
        del_start--;
    while (del_start > 0 &&
           command_buffer[del_start-1]!=' ' && command_buffer[del_start-1]!='\t')
        del_start--;
    size_t del_len = cursor_position - del_start;
    if (del_len > 0) {
        memmove(command_buffer + del_start,
                command_buffer + cursor_position,
                *command_length - cursor_position);
        *command_length -= del_len;
        command_buffer[*command_length] = '\0';
        cursor_position = del_start;
        selection_start = (size_t)-1; selection_end = (size_t)-1;
        update_cursor_display_simple(command_buffer, *command_length);
    }
}

static void delete_word_forward(char *command_buffer, size_t *command_length) {
    if (cursor_position >= *command_length) return;
    undo_save(command_buffer, *command_length, cursor_position);
    size_t del_end = cursor_position;
    while (del_end < *command_length &&
           (command_buffer[del_end]==' ' || command_buffer[del_end]=='\t'))
        del_end++;
    while (del_end < *command_length &&
           command_buffer[del_end]!=' ' && command_buffer[del_end]!='\t')
        del_end++;
    size_t del_len = del_end - cursor_position;
    if (del_len > 0) {
        memmove(command_buffer + cursor_position,
                command_buffer + del_end,
                *command_length - del_end);
        *command_length -= del_len;
        command_buffer[*command_length] = '\0';
        selection_start = (size_t)-1; selection_end = (size_t)-1;
        update_cursor_display_simple(command_buffer, *command_length);
    }
}

static bool is_word_char(char c) {
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';
}
static size_t word_left(const char *buf, size_t pos) {
    if (pos == 0) return 0;
    pos--;
    while (pos > 0 && !is_word_char(buf[pos])) pos--;
    while (pos > 0 &&  is_word_char(buf[pos-1])) pos--;
    return pos;
}
static size_t word_right(const char *buf, size_t pos, size_t len) {
    while (pos < len && !is_word_char(buf[pos])) pos++;
    while (pos < len &&  is_word_char(buf[pos])) pos++;
    return pos;
}

static void transpose_chars(char *command_buffer, size_t *command_length) {
    if (cursor_position < 2 || cursor_position > *command_length) return;
    undo_save(command_buffer, *command_length, cursor_position);
    char temp = command_buffer[cursor_position - 1];
    command_buffer[cursor_position - 1] = command_buffer[cursor_position - 2];
    command_buffer[cursor_position - 2] = temp;
    update_cursor_display_simple(command_buffer, *command_length);
}

// Wrapper now just calls the shared process_extended_byte
bool handle_arrow_keys(uint8_t scan_code, char *command_buffer, size_t *command_length) {
    if (scan_code != 0xE0) return false;

    int timeout = 100000;
    while (timeout > 0 && !(inb(0x64) & 0x01)) timeout--;
    if (timeout == 0) return true;

    uint8_t ext = inb(0x60);
    return process_extended_byte(ext, command_buffer, command_length);
}

// ============================================================
// Status / reboot
// ============================================================

/* ── Helpers for the Expanded Info ───────────────────────────────────── */

/* Helper to scroll the terminal up by 'lines' amount */
static void status_scroll_up(int lines) {
    for(int i=0; i<lines; i++) {
        terminal_putchar('\n'); 
    }
}

/* Helper to print a standard line, auto-padding with spaces to maintain the box shape 
   assuming a slightly slimmer width of 76 columns to be safe */
static void print_status_line(const char* prefix, const char* content, int prefix_len) {
    print("| "); print(prefix); print(content);
    
    /* Calculate padding to reach column 77 (so '|' closes at 78) */
    int len = prefix_len + strlen(content);
    int padding = 77 - len; 
    if(padding < 0) padding = 0;
    
    for(int i=0; i<padding; i++) print(" ");
    print("|\n");
}

static uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static uint8_t bcd_to_dec(uint8_t bcd) {
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

static void cpuid(int code, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(code));
}

/* ── Expanded Status Function (Non-Overlapping) ──────────────────────── */
static void show_status_info(void) {
    /* 1. IDTR */
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) idtr;
    __asm__ volatile ("sidt %0" : : "m" (idtr));

    /* 2. PIT */
    outb(0x43, 0x00);
    uint8_t  pit_low   = inb(0x40);
    uint8_t  pit_high  = inb(0x40);
    uint16_t pit_count = (pit_high << 8) | pit_low;

    /* 3. VGA Cursor */
    outb(0x3D4, 0x0E); uint8_t cursor_high = inb(0x3D5);
    outb(0x3D4, 0x0F); uint8_t cursor_low  = inb(0x3D5);
    uint16_t cursor_pos = (cursor_high << 8) | cursor_low;

    /* 4. CPU Flags */
    unsigned long eflags;
    __asm__ volatile ("pushf; pop %0" : "=r" (eflags));
    bool interrupts_enabled = (eflags & 0x200) != 0;

    /* 5. CPUID */
    uint32_t eax, ebx, ecx, edx;
    char vendor_str[13];
    cpuid(0, &eax, &ebx, &ecx, &edx);
    *((uint32_t*)vendor_str)     = ebx;
    *((uint32_t*)(vendor_str+4)) = edx;
    *((uint32_t*)(vendor_str+8)) = ecx;
    vendor_str[12] = '\0';
    
    cpuid(1, &eax, &ebx, &ecx, &edx);
    uint8_t stepping = eax & 0xF;
    uint8_t model    = (eax >> 4) & 0xF;
    uint8_t family   = (eax >> 8) & 0xF;

    /* 6. Control Registers */
    uint32_t cr0, cr3, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    bool paging_enabled   = (cr0 & 0x80000000) ? true : false;
    bool protected_mode   = (cr0 & 0x00000001) ? true : false;
    bool pae_enabled      = (cr4 & 0x00000020) ? true : false;

    /* 7. Segments */
    uint16_t cs, ds, ss, es;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    __asm__ volatile ("mov %%ds, %0" : "=r"(ds));
    __asm__ volatile ("mov %%ss, %0" : "=r"(ss));
    __asm__ volatile ("mov %%es, %0" : "=r"(es));

    /* 8. RTC */
    while(cmos_read(0x0A) & 0x80);
    uint8_t second = bcd_to_dec(cmos_read(0x00));
    uint8_t minute = bcd_to_dec(cmos_read(0x02));
    uint8_t hour   = bcd_to_dec(cmos_read(0x04));

    /* ──────────────────────────────────────────────────────────────────
       SCROLL TO MAKE ROOM (22 lines for the box)
       ────────────────────────────────────────────────────────────────── */
    status_scroll_up(22);
    print("\n"); 

    /* ──────────────────────────────────────────────────────────────────
       PRINT OUTPUT
       ────────────────────────────────────────────────────────────────── */
    
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("|-----------------------------------------------------------------------------|\n");
    print("|                              SYSTEM STATUS                                   |\n");
    print("|-----------------------------------------------------------------------------|\n");
    terminal_setcolor(VGA_COLOR_LIGHT_GREY);

    /* --- CPU --- */
    print("| [CPU CORE]                                                                          |\n");
    
    print_status_line("Vendor:     ", vendor_str, 13);
    
    /* Manually constructing the Model line for alignment */
    print("|   Family:     "); print_uint(family);
    print("  Model: "); print_uint(model); 
    print("  Stepping: "); print_uint(stepping);
    /* Pad to 77 */
    int f_len = 3 + (family<10?1:2) + 7 + (model<10?1:2) + 9 + (stepping<10?1:2);
    for(int k=f_len; k<75; k++) print(" ");
    print("|\n");
    
    /* --- SYSTEM STATE --- */
    print("| [SYSTEM STATE]                                                                      |\n");
    print("|   Mode:       ");
    terminal_setcolor(protected_mode ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_RED);
    print(protected_mode ? "PROTECTED" : "REAL");
    terminal_setcolor(VGA_COLOR_LIGHT_GREY);
    
    print("    Paging:   ");
    terminal_setcolor(paging_enabled ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_RED);
    print(paging_enabled ? "ENABLED" : "DISABLED");
    terminal_setcolor(VGA_COLOR_LIGHT_GREY);

    print("    PAE:      ");
    terminal_setcolor(pae_enabled ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_RED);
    print(pae_enabled ? "ON" : "OFF");
    
    /* Pad rest of line */
    for(int k=0; k<36; k++) print(" ");
    print("|\n");

    print("|   CR3 (PDBR): 0x"); print_hex(cr3); 
    for(int k=0; k<49; k++) print(" "); print("|\n");

    /* --- INTERRUPTS & TIMING --- */
    print("| [INTERRUPTS & TIMING]                                                                |\n");
    print("|   Interrupts:  ");
    if (interrupts_enabled) { terminal_setcolor(VGA_COLOR_LIGHT_GREEN); print("ENABLED (IF=1)"); }
    else                    { terminal_setcolor(VGA_COLOR_LIGHT_RED);   print("DISABLED (IF=0)"); }
    terminal_setcolor(VGA_COLOR_LIGHT_GREY);
    
    /* Pad */
    for(int k=0; k<47; k++) print(" ");
    print("|\n");

    print("|   IDT Base: 0x"); print_hex(idtr.base); 
    for(int k=0; k<54; k++) print(" "); print("|\n");

    print("|   IDT Limit:  "); print_uint(idtr.limit); print(" bytes");
    for(int k=0; k<44; k++) print(" "); print("|\n");
    
    print("|   PIT Count:  "); print_uint(pit_count); 
    print(" (Freq: ~"); print_uint(1193180 / (pit_count ? pit_count : 1)); print(" Hz)");
    for(int k=0; k<32; k++) print(" "); print("|\n");

    /* --- TIME --- */
    print("| [REAL TIME CLOCK]                                                                  |\n");
    print("|   Time:        ");
    terminal_setcolor(VGA_COLOR_WHITE);
    if(hour < 10) print("0"); print_uint(hour); print(":");
    if(minute < 10) print("0"); print_uint(minute); print(":");
    if(second < 10) print("0"); print_uint(second);
    terminal_setcolor(VGA_COLOR_LIGHT_GREY);
    print(" (UTC)");
    for(int k=0; k<48; k++) print(" "); print("|\n");

    /* --- SEGMENTS --- */
    print("| [SEGMENT REGISTERS]                                                                |\n");
    print("|   CS:0x"); print_hex(cs); print("  DS:0x"); print_hex(ds); 
    print("  SS:0x"); print_hex(ss); print("  ES:0x"); print_hex(es);
    for(int k=0; k<41; k++) print(" "); print("|\n");

    /* --- FOOTER --- */
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("|-----------------------------------------------------------------------------|\n");
    terminal_setcolor(VGA_COLOR_LIGHT_GREY);
    
    display_prompt();
}

static void trigger_reboot(void) {
    print("\n\n[System]: Rebooting...\n");
    uint8_t good = 0x02;
    while (good & 0x02) good = inb(0x64);
    outb(0x64, 0xFE);
    __asm__ __volatile__("hlt");
}

// ============================================================
// keyboard_handler (non-blocking, called from scheduler)
// ============================================================


void keyboard_handler(void) {
    static char   command_buffer[COMMAND_BUFFER_SIZE];
    static size_t command_length = 0;

    if (!is_key_pressed()) return;
    uint8_t scan_code = inb(0x60);

    // 1. Handle Freeze Mode (Screen Selection/Copy)
    // Pass buffer to allow freeze mode to interact with history/rendering logic
    if (freeze_handle_key(scan_code, command_buffer, &command_length)) return;

    // 2. Handle Extended Keys (Arrows, Page Up/Down, Home/ End, etc.)
    // This calls the fixed process_extended_byte which handles Backscroll (PageUp/Down)
    // and Command History navigation (Up/Down).
    if (handle_arrow_keys(scan_code, command_buffer, &command_length)) return;

    // 3. Handle Break Codes (Key Release)
    if (scan_code & 0x80) {
        uint8_t key = scan_code & 0x7F;
        if (key == 0x2A || key == 0x36) shift_active   = false;
        if (key == 0x1D)                g_ctrl_pressed  = false;
        if (key == 0x38)                alt_active      = false;
        return;
    }

    // 4. Handle Modifier Presses (Shift, Ctrl, Alt)
    if (scan_code == 0x2A || scan_code == 0x36) { shift_active   = true; return; }
    if (scan_code == 0x1D)                      { g_ctrl_pressed  = true; return; }
    if (scan_code == 0x38)                      { alt_active      = true; return; }

    // 5. Snap back to live view if user types while scrolled back in history
    // If history_view_offset is > 0, we are looking at past logs.
    // Typing a character should immediately return to the live prompt.
    if (history_view_offset > 0) {
        history_view_offset = 0;
        render_from_history(); // Restore the screen background
        update_cursor_display_simple(command_buffer, command_length); // Redraw prompt
        // Fall through to process the character that was just pressed
    }

    // 6. Ctrl Key Combinations
    if (g_ctrl_pressed) {
        // Ctrl+A / Ctrl+Shift+A
        if (scan_code == 0x1E) {
            if (shift_active) {
                if (command_length > 0) {
                    selection_start = 0; selection_end = command_length;
                    cursor_position = command_length;
                    update_cursor_display_simple(command_buffer, command_length);
                }
            } else {
                cursor_position = 0;
                selection_start = (size_t)-1; selection_end = (size_t)-1;
                update_cursor_display_simple(command_buffer, command_length);
            }
            return;
        }
        // Ctrl+E (End of line)
        if (scan_code == 0x12) {
            cursor_position = command_length;
            selection_start = (size_t)-1; selection_end = (size_t)-1;
            update_cursor_display_simple(command_buffer, command_length);
            return;
        }
        // Ctrl+W (Delete Word Backward - usually mapped here or Ctrl+Backspace)
        if (scan_code == 0x11) { delete_word_backward(command_buffer, &command_length); return; }
        
        // Ctrl+Y (Paste) - Assuming 0x15 is Y
        if (scan_code == 0x15) { perform_paste(command_buffer, &command_length); return; }
        
        // Ctrl+T (Transpose)
        if (scan_code == 0x14) { transpose_chars(command_buffer, &command_length); return; }
        
        // Ctrl+P (Previous History - Alternative to Up Arrow)
        if (scan_code == 0x19) {
            if (history_count > 0) {
                if (current_history_index == -1)
                    current_history_index = (int)history_count - 1;
                else if (current_history_index > 0)
                    current_history_index--;
                strncpy(command_buffer,
                        command_history[current_history_index],
                        COMMAND_BUFFER_SIZE);
                command_buffer[COMMAND_BUFFER_SIZE - 1] = '\0';
                redraw_command_line(command_buffer, &command_length);
            }
            return;
        }
        // Ctrl+N (Next History - Alternative to Down Arrow)
        if (scan_code == 0x31) {
            if (history_count > 0 && current_history_index != -1) {
                if (current_history_index < (int)history_count - 1) {
                    current_history_index++;
                    strncpy(command_buffer,
                            command_history[current_history_index],
                            COMMAND_BUFFER_SIZE);
                } else {
                    current_history_index = -1;
                    command_buffer[0] = '\0';
                }
                redraw_command_line(command_buffer, &command_length);
            }
            return;
        }

        // Cut/Copy/Paste with Shift combos
        if (shift_active) {
            if (scan_code == 0x2F) { perform_paste(command_buffer, &command_length); return; }
            if (scan_code == 0x2E) { perform_copy(command_buffer);                   return; }
            if (scan_code == 0x2D) { perform_cut(command_buffer, &command_length);   return; }
        }

        // Ctrl+C (Break/SIGINT)
        if (scan_code == 0x2E) {
            terminal_putchar('^'); terminal_putchar('C'); terminal_putchar('\n');
            command_length = 0; cursor_position = 0;
            selection_start = (size_t)-1; selection_end = (size_t)-1; 
            command_buffer[0] = '\0';
            display_prompt();
            return;
        }
        // Ctrl+Z (Undo)
        if (scan_code == 0x2C) {
            if (undo_restore(command_buffer, &command_length)) {
                selection_start = (size_t)-1; selection_end = (size_t)-1;
                update_cursor_display_simple(command_buffer, command_length);
            }
            return;
        }
        // Ctrl+L (Clear Screen)
        if (scan_code == 0x26) {
            print("\033[2J\033[H");
            display_prompt();
            update_cursor_display_simple(command_buffer, command_length);
            return;
        }
        // Ctrl+K (Kill to end of line)
        if (scan_code == 0x25) {
            if (cursor_position < command_length) {
                undo_save(command_buffer, command_length, cursor_position);
                size_t len = command_length - cursor_position;
                memcpy(clipboard, command_buffer + cursor_position, len);
                clipboard[len] = '\0'; clipboard_len = len;
                command_length = cursor_position;
                command_buffer[command_length] = '\0';
                selection_start = (size_t)-1; selection_end = (size_t)-1;
                update_cursor_display_simple(command_buffer, command_length);
            }
            return;
        }
        // Ctrl+U (Kill to start of line)
        if (scan_code == 0x16) {
            if (cursor_position > 0) {
                undo_save(command_buffer, command_length, cursor_position);
                size_t len = cursor_position;
                memcpy(clipboard, command_buffer, len);
                clipboard[len] = '\0'; clipboard_len = len;
                size_t rem = command_length - cursor_position;
                memmove(command_buffer, command_buffer + cursor_position, rem);
                command_length -= cursor_position;
                cursor_position = 0;
                command_buffer[command_length] = '\0';
                selection_start = (size_t)-1; selection_end = (size_t)-1;
                update_cursor_display_simple(command_buffer, command_length);
            }
            return;
        }
    }

    // 7. Special Keys
    if (scan_code == 0x3A) { // Caps Lock
        caps_lock_active = !caps_lock_active;
        set_keyboard_leds(caps_lock_active ? 0x04 : 0);
        return;
    }
    if (scan_code == 0x0F) { // Tab
        int n = tab_complete(command_buffer, &command_length);
        if (n > 0) {
            cursor_position = command_length;
            update_cursor_display_simple(command_buffer, command_length);
        }
        return;
    }
    if (scan_code == 0x3D) { show_status_info(); return; } // F12 Status
    if (scan_code == 0x3E) { trigger_reboot();   return; } // SysRq/Reboot

    // 8. Alt Key Combinations (Meta keys)
    if (alt_active && scan_code < sizeof(keyboard_map)) {
        if (scan_code == 0x30) { // Alt+B (Word Back)
            cursor_position = word_left(command_buffer, cursor_position);
            update_cursor_display_simple(command_buffer, command_length);
            return;
        }
        if (scan_code == 0x21) { // Alt+F (Word Forward)
            cursor_position = word_right(command_buffer, cursor_position, command_length);
            update_cursor_display_simple(command_buffer, command_length);
            return;
        }
    }

    // 9. Standard ASCII and Function Keys
    if (scan_code < sizeof(keyboard_map)) {
        if (scan_code == 0x1C) { // Enter
            command_buffer[command_length] = '\0';
            terminal_putchar('\n');
            execute_command(command_buffer);
            command_length = 0; cursor_position = 0;
            command_buffer[0] = '\0'; current_history_index = -1;
            selection_start = (size_t)-1; selection_end = (size_t)-1;
            display_prompt();
            terminal_update_cursor();
            return;
        }
        if (scan_code == 0x0E) { // Backspace
            if (g_ctrl_pressed) delete_word_backward(command_buffer, &command_length);
            else                delete_char_at_cursor(command_buffer, &command_length);
            current_history_index = -1;
            return;
        }
        
        // Regular Character Typing
        char base_char    = keyboard_map[scan_code];
        char shifted_char = shifted_keyboard_map[scan_code];
        char c = 0;
        if (base_char != 0) {
            if (base_char >= 'a' && base_char <= 'z')
                c = (caps_lock_active ^ shift_active) ? (base_char-'a'+'A') : base_char;
            else
                c = shift_active ? shifted_char : base_char;
            
            if (c != 0 && command_length < COMMAND_BUFFER_SIZE - 1) {
                insert_char_at_cursor(command_buffer, &command_length, c);
                current_history_index = -1;
            }
        }
    }
}


void keyboard_read_input(void) {
    display_prompt();
    while (true) keyboard_handler();
}

// ============================================================
// keyboard_input (blocking single-line read)
// ============================================================
int keyboard_input(char *userinput) {
    static char command_buffer[COMMAND_BUFFER_SIZE];
    size_t command_length = 0;

    cursor_position       = 0;
    current_history_index = -1;
    selection_start       = (size_t)-1;
    selection_end         = (size_t)-1;
    memset(command_buffer, 0, COMMAND_BUFFER_SIZE);

    while (true) {
        if (!is_key_pressed()) continue;
        uint8_t scan_code = port_byte_in(0x60);

        // Update call to pass buffer args
        if (freeze_handle_key(scan_code, command_buffer, &command_length)) continue;
        if (handle_arrow_keys(scan_code, command_buffer, &command_length)) continue;

        if (scan_code & 0x80) {
            uint8_t key = scan_code & 0x7F;
            if (key == 0x2A || key == 0x36) shift_active   = false;
            if (key == 0x1D)                g_ctrl_pressed  = false;
            if (key == 0x38)                alt_active      = false;
            continue;
        }

        if (scan_code == 0x2A || scan_code == 0x36) { shift_active   = true; continue; }
        if (scan_code == 0x1D)                      { g_ctrl_pressed  = true; continue; }
        if (scan_code == 0x38)                      { alt_active      = true; continue; }

        if (g_ctrl_pressed && shift_active) {
            if (scan_code == 0x2F) { perform_paste(command_buffer, &command_length); continue; }
            if (scan_code == 0x2E) { perform_copy(command_buffer);                   continue; }
            if (scan_code == 0x2D) { perform_cut(command_buffer, &command_length);   continue; }
        }

        if (g_ctrl_pressed) {
            if (scan_code == 0x2E) {
                terminal_putchar('^'); terminal_putchar('C'); terminal_putchar('\n');
                return -1;
            }
            if (scan_code == 0x1E) {
                if (command_length > 0) { terminal_putchar('\n'); userinput[0] = '\0'; return -3; }
                continue;
            }
            if (scan_code == 0x2C) {
                if (undo_restore(command_buffer, &command_length)) {
                    selection_start = (size_t)-1; selection_end = (size_t)-1;
                    update_cursor_display_simple(command_buffer, command_length);
                }
                continue;
            }
            if (scan_code == 0x26) {
                print("\033[2J\033[H");
                update_cursor_display_simple(command_buffer, command_length);
                continue;
            }
            if (scan_code == 0x25) {
                if (cursor_position < command_length) {
                    undo_save(command_buffer, command_length, cursor_position);
                    size_t len = command_length - cursor_position;
                    memcpy(clipboard, command_buffer + cursor_position, len);
                    clipboard[len] = '\0'; clipboard_len = len;
                    command_length = cursor_position;
                    command_buffer[command_length] = '\0';
                    selection_start = (size_t)-1; selection_end = (size_t)-1;
                    update_cursor_display_simple(command_buffer, command_length);
                }
                continue;
            }
            if (scan_code == 0x16) {
                if (cursor_position > 0) {
                    undo_save(command_buffer, command_length, cursor_position);
                    size_t len = cursor_position;
                    memcpy(clipboard, command_buffer, len);
                    clipboard[len] = '\0'; clipboard_len = len;
                    size_t rem = command_length - cursor_position;
                    memmove(command_buffer, command_buffer + cursor_position, rem);
                    command_length -= cursor_position;
                    cursor_position = 0;
                    command_buffer[command_length] = '\0';
                    selection_start = (size_t)-1; selection_end = (size_t)-1;
                    update_cursor_display_simple(command_buffer, command_length);
                }
                continue;
            }
            if (scan_code == 0x11) { delete_word_backward(command_buffer, &command_length); continue; }
            if (scan_code == 0x15) { perform_paste(command_buffer, &command_length);         continue; }
        }

        if (scan_code == 0x3A) {
            caps_lock_active = !caps_lock_active;
            set_keyboard_leds(caps_lock_active ? 0x04 : 0);
            continue;
        }
        if (scan_code == 0x0F) {
            int n = tab_complete(command_buffer, &command_length);
            if (n > 0) {
                cursor_position = command_length;
                update_cursor_display_simple(command_buffer, command_length);
            }
            continue;
        }
        if (scan_code == 0x1C) { // Enter
            command_buffer[command_length] = '\0';
            print("\n");
            add_to_history(command_buffer);
            strncpy(userinput, command_buffer, COMMAND_BUFFER_SIZE);
            userinput[COMMAND_BUFFER_SIZE - 1] = '\0';
            return 0;
        }
        if (scan_code == 0x0E) { // Backspace
            if (g_ctrl_pressed) {
                delete_word_backward(command_buffer, &command_length);
            } else if (command_length > 0 && cursor_position > 0) {
                undo_save(command_buffer, command_length, cursor_position);
                for (size_t i = cursor_position - 1; i < command_length - 1; i++)
                    command_buffer[i] = command_buffer[i + 1];
                command_length--;
                cursor_position--;
                command_buffer[command_length] = '\0';
                selection_start = (size_t)-1; selection_end = (size_t)-1;
                update_cursor_display_simple(command_buffer, command_length);
            }
            continue;
        }
        
        if (scan_code < sizeof(keyboard_map)) {
    char base_char = keyboard_map[scan_code];
    char c = 0;

    if (base_char >= 'a' && base_char <= 'z') {
        // letter — respect caps lock XOR shift
        c = (caps_lock_active ^ shift_active) ? (base_char - 'a' + 'A') : base_char;
    } else {
        // symbol / number — just use shift map
        c = shift_active ? shifted_keyboard_map[scan_code] : base_char;
    }

    if (c != 0 && command_length < COMMAND_BUFFER_SIZE - 1) {
        insert_char_at_cursor(command_buffer, &command_length, c);
        current_history_index = -1;
    }
}
    }
}

// ============================================================
// keyboard_input_secure (password input)
// ============================================================
void keyboard_input_secure(char *userinput) {
    static char command_buffer[COMMAND_BUFFER_SIZE] = {0};
    size_t command_length = 0;
    cursor_position = 0;
    selection_start = (size_t)-1;

    while (true) {
        if (!is_key_pressed()) continue;
        uint8_t scan_code = port_byte_in(0x60);
        if (scan_code >= sizeof(keyboard_map)) continue;
        if (scan_code & 0x80) {
            if (scan_code == 0xAA || scan_code == 0xB6) shift_active = false;
            continue;
        }
        if (scan_code == 0x2A || scan_code == 0x36) { shift_active = true; continue; }
        char c = shift_active ? shifted_keyboard_map[scan_code] : keyboard_map[scan_code];
        if (scan_code == 0x1C) {
            command_buffer[command_length] = '\0';
            strncpy(userinput, command_buffer, COMMAND_BUFFER_SIZE);
            userinput[COMMAND_BUFFER_SIZE - 1] = '\0';
            command_length = 0;
            terminal_putchar('\n');
            return;
        } else if (scan_code == 0x0E) {
            if (command_length > 0) {
                command_length--;
                cursor_position--;
                terminal_putchar('\b');
                terminal_putchar(' ');
                terminal_putchar('\b');
            }
        } else if (c != 0 && command_length < COMMAND_BUFFER_SIZE - 1) {
            command_buffer[command_length++] = c;
            cursor_position++;
            terminal_putchar('*');
            terminal_update_cursor();
        }
    }
}

// ============================================================
// keyboard_wait_for_key
// ============================================================
uint8_t keyboard_wait_for_key(bool dump_scancode) {
    while (true) {
        if (!is_key_pressed()) continue;
        uint8_t scan_code = port_byte_in(0x60);
        if (dump_scancode) {
            print("Scan code: ");
            print_hex(scan_code);
            print("\n");
        }
        if (scan_code < sizeof(keyboard_map) && !(scan_code & 0x80)) {
            if (scan_code == 0x2A || scan_code == 0x36) {
                shift_active = true;
            } else {
                return shift_active
                    ? shifted_keyboard_map[scan_code]
                    : keyboard_map[scan_code];
            }
        }
        // handle key-up for shift
        if (scan_code & 0x80) {
            uint8_t key = scan_code & 0x7F;
            if (key == 0x2A || key == 0x36) shift_active = false;
        }
    }
}

uint8_t keyboard_key(void) {
    uint8_t c = keyboard_wait_for_key(false);
    print("\n");
    return c;
}

// ============================================================
// LED / low-level helpers
// ============================================================
static void keyboard_wait_for_input(void) {
    uint32_t t = 100000;
    while (t--) { if (!(inb(0x64) & 0x02)) return; io_wait(); }
}

static void keyboard_wait_for_output(void) {
    uint32_t t = 100000;
    while (t--) { if (inb(0x64) & 0x01) return; io_wait(); }
}

#define MAX_RETRIES 5
static int keyboard_send_command(uint8_t command) {
    for (int i = 0; i < MAX_RETRIES; i++) {
        keyboard_wait_for_input();
        outb(0x60, command);
        keyboard_wait_for_output();
        uint8_t ack = inb(0x60);
        if (ack == 0xFA) return 0;
        if (ack != 0xFE) break;
    }
    return -1;
}

static uint8_t led_status = 0;

void set_keyboard_leds(uint8_t leds) {
    if (keyboard_send_command(0xED) != 0) return;
    io_wait();
    if (keyboard_send_command(leds) != 0) return;
    led_status = leds;
}

void toggle_caps_lock(void) {
    led_status ^= 0x04;
    set_keyboard_leds(led_status);
}

char keyboard_to_char(uint8_t scancode, bool shift, bool caps_lock) {
    if (scancode >= sizeof(keyboard_map)) return 0;
    char c = shift ? shifted_keyboard_map[scancode] : keyboard_map[scancode];
    if (c >= 'a' && c <= 'z' && caps_lock) return c - 32;
    if (c >= 'A' && c <= 'Z' && caps_lock) return c + 32;
    return c;
}

bool is_key_down(uint8_t scancode) {
    if (scancode == 0x1D) return true;
    return false;
}

// keyboard.c

void history_reset(void) {
    history_total_lines = 0;
    history_view_offset = 0;
    memset(history_buffer, 0, sizeof(history_buffer));
}