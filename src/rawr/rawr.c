#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../Avfs/Avfs.h"

#include "../terminal/terminal.h"
#include "../keyboard/keyboard.h"
#include "../scheduler/task.h"
#include "../timers/timer.h"
#include "../timers/date.h"
#include "../errors/error.h"
#include "../sound/sound.h"
#include "../io/io.h"
#include "../mpop/mpop.h"
#include "../Avfs/Avfs.h"
#include "../memory/memory.h"
#include "../rawr/rawr.h"

#include "../user/login.h"
#include "../user/sudo.h"

#include "../cpu/cpu.h"
#include "../utility/utility.h"
#include "../utility/random.h"
#include "../savestate/savestate.h"
#include "../games/pong.h"


#include "../commands/mempop.h"
#include "../commands/brainz.h"
#include "../commands/clear.h"
#include "../commands/echo.h"
#include "../commands/exit.h"
#include "../commands/reboot.h"
#include "../commands/help.h"
#include "../commands/text.h"
#include "../commands/meow.h"
#include "../commands/rm.h"
#include "../commands/cat.h"
#include "../commands/settings.h"
#include "../commands/ls.h"
#include "../commands/tui.h"
#include "../commands/radifetch.h"
#include "../commands/cowsay.h"
#include "../timers/timer.h"
#include "../keyboard/keyboard.h"
#ifndef AVFS_FILENAME_MAX
#define AVFS_FILENAME_MAX 64
#endif

#ifndef AVFS_MAX_FILES
#define AVFS_MAX_FILES 128
#endif
// File info structure for file manager
typedef struct {
    char name[AVFS_FILENAME_MAX];
    char full_path[512];
    uint32_t size;
    int blocks;
    char type[8];
    bool is_dir;
} file_info_t;

// Global/static arrays for file manager - declared outside function to avoid stack overflow
static file_info_t fm_files[AVFS_MAX_FILES];
static char fm_current_path[256];
static char fm_clipboard_file[512];

void file_manager() {
    // File manager state
    int selected_index = 0;
    int scroll_offset = 0;
    int view_mode = 1; // 0=compact, 1=details, 2=preview
    int sort_mode = 0; // 0=name, 1=size, 2=type
    bool sort_reverse = false;
    bool show_help = false;
    bool need_refresh = true;
    bool need_redraw = true;
    bool show_hidden = false;
    bool clipboard_is_cut = false;
    
    // Initialize
    strcpy(fm_current_path, "/");
    fm_clipboard_file[0] = '\0';
    int file_count = 0;
    
    // Main file manager window
    vga_window_t fm_win = vga_create_centered_window(76, 24, VGA_COLOR_WHITE, VGA_COLOR_BLUE);
    vga_win_set_title(&fm_win, "Advanced File Manager v2.0 - Recursive Directories");
    
    int frame = 0;
    bool running = true;
    
    while (running) {
        // Calculate list height based on view mode
        int list_height = view_mode == 2 ? 7 : 11;
        
        // Refresh file list when needed
        if (need_refresh) {
            file_count = 0;
            
            // Normalize current_path (ensure it doesn't end with / except for root)
            int path_len = strlen(fm_current_path);
            if (path_len > 1 && fm_current_path[path_len - 1] == '/') {
                fm_current_path[path_len - 1] = '\0';
                path_len--;
            }
            
            // Add parent directory entry if not at root
            if (strcmp(fm_current_path, "/") != 0) {
                strcpy(fm_files[file_count].name, "..");
                strcpy(fm_files[file_count].full_path, "..");
                fm_files[file_count].size = 0;
                fm_files[file_count].blocks = 0;
                strcpy(fm_files[file_count].type, "DIR");
                fm_files[file_count].is_dir = true;
                file_count++;
            }
            
            // Temporary array to track seen directories
            static char seen_dirs[128][64];
            int seen_dir_count = 0;
            
            // Iterate through all AVFS files
            for (int i = 0; i < AVFS_MAX_FILES; i++) {
                char filename[AVFS_FILENAME_MAX];
                uint32_t filesize;
                
                if (avfs_get_file_info(i, filename, &filesize) == 0) {
                    // Skip empty filenames
                    if (filename[0] == '\0') continue;
                    
                    // Skip hidden files if not showing them
                    if (!show_hidden && filename[0] == '.' && strcmp(filename, "..") != 0) {
                        continue;
                    }
                    
                    // For root directory
                    if (strcmp(fm_current_path, "/") == 0) {
                        // Files must start with /
                        if (filename[0] != '/') continue;
                        
                        // Skip the leading /
                        char* name_part = filename + 1;
                        
                        // Find if there's another slash
                        char* slash_pos = strchr(name_part, '/');
                        
                        if (slash_pos != NULL) {
                            // This is in a subdirectory
                            int subdir_len = slash_pos - name_part;
                            char subdir_name[AVFS_FILENAME_MAX];
                            strncpy(subdir_name, name_part, subdir_len);
                            subdir_name[subdir_len] = '\0';
                            
                            // Skip if empty or .dir marker
                            if (subdir_len == 0 || strcmp(subdir_name, ".dir") == 0) continue;
                            
                            // Check if already added
                            bool already_added = false;
                            for (int j = 0; j < seen_dir_count; j++) {
                                if (strcmp(seen_dirs[j], subdir_name) == 0) {
                                    already_added = true;
                                    break;
                                }
                            }
                            
                            if (!already_added && file_count < AVFS_MAX_FILES && seen_dir_count < 128) {
                                strcpy(fm_files[file_count].name, subdir_name);
                                strcpy(fm_files[file_count].full_path, "/");
                                strcat(fm_files[file_count].full_path, subdir_name);
                                fm_files[file_count].size = 0;
                                fm_files[file_count].blocks = 0;
                                strcpy(fm_files[file_count].type, "DIR");
                                fm_files[file_count].is_dir = true;
                                
                                strncpy(seen_dirs[seen_dir_count], subdir_name, 63);
                                seen_dirs[seen_dir_count][63] = '\0';
                                seen_dir_count++;
                                file_count++;
                            }
                        } else {
                            // File directly in root
                            if (strlen(name_part) == 0) continue;
                            
                            strcpy(fm_files[file_count].name, name_part);
                            strcpy(fm_files[file_count].full_path, filename);
                            fm_files[file_count].size = filesize;
                            fm_files[file_count].blocks = (filesize + 511) / 512;
                            fm_files[file_count].is_dir = false;
                            
                            // Extract file type
                            char* ext = strrchr(name_part, '.');
                            if (ext && ext != name_part) {
                                strncpy(fm_files[file_count].type, ext + 1, 7);
                                fm_files[file_count].type[7] = '\0';
                            } else {
                                strcpy(fm_files[file_count].type, "file");
                            }
                            
                            file_count++;
                        }
                    } else {
                        // We're in a subdirectory
                        // Build the prefix we're looking for
                        char current_prefix[512];
                        strcpy(current_prefix, fm_current_path);
                        strcat(current_prefix, "/");
                        int prefix_len = strlen(current_prefix);
                        
                        // File must start with our prefix
                        if (strncmp(filename, current_prefix, prefix_len) != 0) {
                            continue;
                        }
                        
                        // Get the part after our prefix
                        char* remainder = filename + prefix_len;
                        
                        // Skip if empty
                        if (remainder[0] == '\0') continue;
                        
                        // Check for another slash
                        char* slash_pos = strchr(remainder, '/');
                        
                        if (slash_pos != NULL) {
                            // This is in a subdirectory
                            int subdir_len = slash_pos - remainder;
                            char subdir_name[AVFS_FILENAME_MAX];
                            strncpy(subdir_name, remainder, subdir_len);
                            subdir_name[subdir_len] = '\0';
                            
                            // Skip if empty or .dir marker
                            if (subdir_len == 0 || strcmp(subdir_name, ".dir") == 0) continue;
                            
                            // Check if already added
                            bool already_added = false;
                            for (int j = 0; j < seen_dir_count; j++) {
                                if (strcmp(seen_dirs[j], subdir_name) == 0) {
                                    already_added = true;
                                    break;
                                }
                            }
                            
                            if (!already_added && file_count < AVFS_MAX_FILES && seen_dir_count < 128) {
                                strcpy(fm_files[file_count].name, subdir_name);
                                strcpy(fm_files[file_count].full_path, fm_current_path);
                                strcat(fm_files[file_count].full_path, "/");
                                strcat(fm_files[file_count].full_path, subdir_name);
                                fm_files[file_count].size = 0;
                                fm_files[file_count].blocks = 0;
                                strcpy(fm_files[file_count].type, "DIR");
                                fm_files[file_count].is_dir = true;
                                
                                strncpy(seen_dirs[seen_dir_count], subdir_name, 63);
                                seen_dirs[seen_dir_count][63] = '\0';
                                seen_dir_count++;
                                file_count++;
                            }
                        } else {
                            // File directly in this directory
                            strcpy(fm_files[file_count].name, remainder);
                            strcpy(fm_files[file_count].full_path, filename);
                            fm_files[file_count].size = filesize;
                            fm_files[file_count].blocks = (filesize + 511) / 512;
                            fm_files[file_count].is_dir = false;
                            
                            // Extract file type
                            char* ext = strrchr(remainder, '.');
                            if (ext && ext != remainder) {
                                strncpy(fm_files[file_count].type, ext + 1, 7);
                                fm_files[file_count].type[7] = '\0';
                            } else {
                                strcpy(fm_files[file_count].type, "file");
                            }
                            
                            file_count++;
                        }
                    }
                }
            }
            
            // Sort files - directories first, then by sort mode
            for (int i = 0; i < file_count - 1; i++) {
                for (int j = i + 1; j < file_count; j++) {
                    bool should_swap = false;
                    
                    // Always keep ".." at top
                    if (strcmp(fm_files[i].name, "..") == 0) continue;
                    if (strcmp(fm_files[j].name, "..") == 0) {
                        should_swap = true;
                    } else {
                        // Sort directories before files
                        if (fm_files[i].is_dir && !fm_files[j].is_dir) {
                            continue;
                        } else if (!fm_files[i].is_dir && fm_files[j].is_dir) {
                            should_swap = true;
                        } else {
                            // Both are same type, sort by selected mode
                            if (sort_mode == 0) { // Name
                                int cmp = strcmp(fm_files[i].name, fm_files[j].name);
                                should_swap = sort_reverse ? (cmp < 0) : (cmp > 0);
                            } else if (sort_mode == 1) { // Size
                                should_swap = sort_reverse ? 
                                    (fm_files[i].size < fm_files[j].size) : 
                                    (fm_files[i].size > fm_files[j].size);
                            } else if (sort_mode == 2) { // Type
                                int cmp = strcmp(fm_files[i].type, fm_files[j].type);
                                should_swap = sort_reverse ? (cmp < 0) : (cmp > 0);
                            }
                        }
                    }
                    
                    if (should_swap) {
                        file_info_t temp = fm_files[i];
                        fm_files[i] = fm_files[j];
                        fm_files[j] = temp;
                    }
                }
            }
            
            // Ensure selected_index is valid after refresh
            if (selected_index >= file_count && file_count > 0) {
                selected_index = file_count - 1;
            }
            if (selected_index < 0) selected_index = 0;
            
            // Adjust scroll_offset to keep selection visible
            if (selected_index < scroll_offset) {
                scroll_offset = selected_index;
            } else if (selected_index >= scroll_offset + list_height) {
                scroll_offset = selected_index - list_height + 1;
            }
            
            need_refresh = false;
            need_redraw = true;
        }
        
        // Only redraw if needed
        if (need_redraw) {
            vga_win_clear(&fm_win);
            
            // === HEADER ===
            vga_win_puts_colored(&fm_win, 2, 2,
                "RadiumOS File Manager - Recursive",
                vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLUE));
            
            // Current path display
            char path_display[60];
            strcpy(path_display, "Path: ");
            if (strlen(fm_current_path) > 45) {
                strcat(path_display, "...");
                strncat(path_display, fm_current_path + strlen(fm_current_path) - 42, 42);
            } else {
                strncat(path_display, fm_current_path, 50);
            }
            vga_win_puts_colored(&fm_win, 2, 3, path_display,
                vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLUE));
            
            // System info
            char info_str[40];
            itoa(file_count, info_str, 10);
            strcat(info_str, " items | ");
            
            // Calculate total size
            uint32_t total_size = 0;
            int dir_count = 0;
            for (int i = 0; i < file_count; i++) {
                if (fm_files[i].is_dir) {
                    dir_count++;
                } else {
                    total_size += fm_files[i].size;
                }
            }
            
            char size_buf[20];
            if (total_size < 1024) {
                itoa(total_size, size_buf, 10);
                strcat(size_buf, "B");
            } else if (total_size < 1024 * 1024) {
                itoa(total_size / 1024, size_buf, 10);
                strcat(size_buf, "KB");
            } else {
                itoa(total_size / (1024 * 1024), size_buf, 10);
                strcat(size_buf, "MB");
            }
            strcat(info_str, size_buf);
            
            vga_win_puts_colored(&fm_win, 50, 2, info_str,
                vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_BLUE));
            
            // Dir count
            char dir_info[20];
            itoa(dir_count, dir_info, 10);
            strcat(dir_info, " dirs");
            vga_win_puts_colored(&fm_win, 60, 3, dir_info,
                vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_BLUE));
            
            vga_win_draw_line_h(&fm_win, 2, 4, 72, 0xC4);
            
            // === TOOLBAR ===
            vga_win_puts_colored(&fm_win, 2, 5, "F1", 
                vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_CYAN));
            vga_win_puts(&fm_win, 5, 5, "Help");
            
            vga_win_puts_colored(&fm_win, 12, 5, "F2", 
                vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_CYAN));
            vga_win_puts(&fm_win, 15, 5, "View");
            
            vga_win_puts_colored(&fm_win, 22, 5, "F3", 
                vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_CYAN));
            vga_win_puts(&fm_win, 25, 5, "Open");
            
            vga_win_puts_colored(&fm_win, 32, 5, "F5", 
                vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_CYAN));
            vga_win_puts(&fm_win, 35, 5, "Copy");
            
            vga_win_puts_colored(&fm_win, 42, 5, "F7", 
                vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_CYAN));
            vga_win_puts(&fm_win, 45, 5, "MkDir");
            
            vga_win_puts_colored(&fm_win, 52, 5, "F8", 
                vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_CYAN));
            vga_win_puts(&fm_win, 55, 5, "Delete");
            
            vga_win_puts_colored(&fm_win, 62, 5, "F9", 
                vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_CYAN));
            vga_win_puts(&fm_win, 65, 5, "Rename");
            
            vga_win_draw_line_h(&fm_win, 2, 6, 72, 0xC4);
            
            // === SORT INDICATOR ===
            const char* sort_names[] = {"Name", "Size", "Type"};
            char sort_str[30] = "Sort: ";
            strcat(sort_str, sort_names[sort_mode]);
            strcat(sort_str, sort_reverse ? " v" : " ^");
            
            vga_win_puts_colored(&fm_win, 2, 7, sort_str,
                vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLUE));
            
            // View mode indicator
            const char* view_names[] = {"Compact", "Details", "Preview"};
            vga_win_puts_colored(&fm_win, 50, 7, "View: ",
                vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLUE));
            vga_win_puts_colored(&fm_win, 56, 7, view_names[view_mode],
                vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE));
            
            // Clipboard indicator
            if (fm_clipboard_file[0] != '\0') {
                vga_win_puts_colored(&fm_win, 66, 7, clipboard_is_cut ? "[CUT]" : "[CPY]",
                    vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLUE));
            }
            
            vga_win_draw_line_h(&fm_win, 2, 8, 72, 0xC4);
            
            // === FILE LIST ===
            int list_start_y = 9;
            
            // Column headers for details view
            if (view_mode >= 1) {
                vga_win_puts_colored(&fm_win, 4, list_start_y,
                    "Name",
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLUE));
                vga_win_puts_colored(&fm_win, 35, list_start_y,
                    "Size",
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLUE));
                vga_win_puts_colored(&fm_win, 48, list_start_y,
                    "Blocks",
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLUE));
                vga_win_puts_colored(&fm_win, 60, list_start_y,
                    "Type",
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLUE));
                list_start_y++;
            }
            
            // Draw files
            for (int i = scroll_offset; i < file_count && (i - scroll_offset) < list_height; i++) {
                int y = list_start_y + (i - scroll_offset);
                bool is_selected = (i == selected_index);
                
                // Selection highlight
                if (is_selected) {
                    vga_win_fill_rect(&fm_win, 2, y, 72, 1, ' ',
                        vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_CYAN));
                }
                
                uint8_t text_color = is_selected ? 
                    vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_CYAN) :
                    (fm_files[i].is_dir ? 
                        vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLUE) :
                        vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE));
                
                // Selection indicator
                if (is_selected) {
                    vga_win_puts_colored(&fm_win, 2, y, ">",
                        vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_CYAN));
                }
                
                // Directory indicator
                if (fm_files[i].is_dir) {
                    vga_win_puts_colored(&fm_win, 3, y, "[",
                        is_selected ? vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_CYAN) :
                                     vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLUE));
                }
                
                // File name
                char display_name[32];
                strncpy(display_name, fm_files[i].name, 26);
                display_name[26] = '\0';
                if (strlen(fm_files[i].name) > 26) {
                    strcat(display_name, "...");
                }
                
                int name_x = fm_files[i].is_dir ? 4 : 4;
                vga_win_puts_colored(&fm_win, name_x, y, display_name, text_color);
                
                // Directory closing bracket
                if (fm_files[i].is_dir) {
                    int bracket_x = name_x + strlen(display_name);
                    if (bracket_x < 34) {
                        vga_win_puts_colored(&fm_win, bracket_x, y, "]",
                            is_selected ? vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_CYAN) :
                                         vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLUE));
                    }
                }
                
                // Details view
                if (view_mode >= 1 && !fm_files[i].is_dir) {
                    // File size
                    char size_str[16];
                    if (fm_files[i].size < 1024) {
                        itoa(fm_files[i].size, size_str, 10);
                        strcat(size_str, " B");
                    } else if (fm_files[i].size < 1024 * 1024) {
                        itoa(fm_files[i].size / 1024, size_str, 10);
                        strcat(size_str, " KB");
                    } else {
                        itoa(fm_files[i].size / (1024 * 1024), size_str, 10);
                        strcat(size_str, " MB");
                    }
                    vga_win_puts_colored(&fm_win, 35, y, size_str, text_color);
                    
                    // Blocks
                    char block_str[16];
                    itoa(fm_files[i].blocks, block_str, 10);
                    vga_win_puts_colored(&fm_win, 48, y, block_str, text_color);
                    
                    // Type
                    vga_win_puts_colored(&fm_win, 60, y, fm_files[i].type, text_color);
                } else if (view_mode >= 1 && fm_files[i].is_dir) {
                    vga_win_puts_colored(&fm_win, 35, y, "<DIR>", text_color);
                    vga_win_puts_colored(&fm_win, 60, y, "DIR", text_color);
                }
            }
            
            // === FILE PREVIEW ===
            if (view_mode == 2 && file_count > 0 && selected_index < file_count && !fm_files[selected_index].is_dir) {
                vga_win_draw_line_h(&fm_win, 2, 18, 72, 0xC4);
                vga_win_puts_colored(&fm_win, 2, 19,
                    "Preview:",
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLUE));
                vga_win_puts_colored(&fm_win, 11, 19, fm_files[selected_index].name,
                    vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_BLUE));
                
                char preview_buf[256];
                int read_size = fm_files[selected_index].size > 255 ? 255 : fm_files[selected_index].size;
                
                if (read_size > 0 && avfs_read_file(fm_files[selected_index].full_path, preview_buf, read_size, 0) == 0) {
                    // Show first 3 lines
                    int line = 0;
                    int col = 2;
                    for (int i = 0; i < read_size && line < 3; i++) {
                        char ch = preview_buf[i];
                        
                        if (ch == '\n') {
                            line++;
                            col = 2;
                            continue;
                        }
                        
                        if (col >= 72) continue;
                        
                        if (ch < 32 || ch > 126) ch = '.';
                        vga_win_putc_colored(&fm_win, col++, 20 + line, ch,
                            vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_BLUE));
                    }
                } else {
                    vga_win_puts_colored(&fm_win, 2, 20, "[Binary or empty file]",
                        vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_BLUE));
                }
            }
            
            // === HELP PANEL ===
            if (show_help) {
                vga_window_t help_win = vga_create_centered_window(60, 20, VGA_COLOR_BLACK, VGA_COLOR_BLACK);
                vga_win_set_title(&help_win, "Keyboard Shortcuts");
                
                vga_win_puts(&help_win, 2, 2,  "Navigation:");
                vga_win_puts(&help_win, 4, 3,  "UP/DN      - Move selection");
                vga_win_puts(&help_win, 4, 4,  "PgUp/PgDn  - Page up/down");
                vga_win_puts(&help_win, 4, 5,  "Home/End   - First/last item");
                vga_win_puts(&help_win, 4, 6,  "ENTER      - Open file/Enter directory");
                vga_win_puts(&help_win, 4, 7,  "BACKSPACE  - Go to parent directory");
                
                vga_win_puts(&help_win, 2, 9,  "File Operations:");
                vga_win_puts(&help_win, 4, 10, "F3/ENTER   - Open/View");
                vga_win_puts(&help_win, 4, 11, "F4         - Edit file");
                vga_win_puts(&help_win, 4, 12, "F5         - Copy");
                vga_win_puts(&help_win, 4, 13, "F6         - Move/Cut");
                vga_win_puts(&help_win, 4, 14, "F7         - Create directory");
                vga_win_puts(&help_win, 4, 15, "F8/DEL     - Delete");
                vga_win_puts(&help_win, 4, 16, "F9         - Rename");
                
                vga_win_puts(&help_win, 30, 9,  "View & Sort:");
                vga_win_puts(&help_win, 32, 10, "F2   - Change view");
                vga_win_puts(&help_win, 32, 11, "S    - Sort mode");
                vga_win_puts(&help_win, 32, 12, "R    - Reverse sort");
                vga_win_puts(&help_win, 32, 13, "H    - Show hidden");
                
                vga_win_puts_centered(&help_win, 18, "Press any key to close");
                vga_win_refresh(&help_win);
                
                keyboard_wait_for_key(0);
                vga_destroy_window(&help_win);
                show_help = false;
                need_redraw = true;
            }
            
            // === FOOTER / STATUS BAR ===
            vga_win_draw_line_h(&fm_win, 2, 21, 72, 0xC4);
            
            if (file_count > 0 && selected_index < file_count) {
                char status[70];
                strcpy(status, fm_files[selected_index].name);
                
                if (fm_files[selected_index].is_dir) {
                    strcat(status, " - <Directory>");
                } else {
                    strcat(status, " - ");
                    
                    char size_str[20];
                    if (fm_files[selected_index].size < 1024) {
                        itoa(fm_files[selected_index].size, size_str, 10);
                        strcat(size_str, " bytes");
                    } else {
                        itoa(fm_files[selected_index].size / 1024, size_str, 10);
                        strcat(size_str, " KB");
                    }
                    strcat(status, size_str);
                }
                
                vga_win_puts_colored(&fm_win, 2, 22, status,
                    vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE));
            } else if (file_count == 0) {
                vga_win_puts_colored(&fm_win, 2, 22, "Empty directory",
                    vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_BLUE));
            }
            
            vga_win_puts_colored(&fm_win, 2, 23, "F1:Help | ENTER:Open | BACKSPACE:Up | ESC:Exit",
                vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLUE));
            
            vga_win_refresh(&fm_win);
            need_redraw = false;
        }
        
        // === KEYBOARD INPUT ===
        int key = keyboard_key();
        
        if (key == 0) {
            sleep_ms(10);
            continue;
        }
        
        if (key == 0x48) { // UP
            if (selected_index > 0) {
                selected_index--;
                if (selected_index < scroll_offset) {
                    scroll_offset = selected_index;
                }
                need_redraw = true;
            }
        }
        else if (key == 0x50) { // DOWN
            if (selected_index < file_count - 1) {
                selected_index++;
                if (selected_index >= scroll_offset + list_height) {
                    scroll_offset = selected_index - list_height + 1;
                }
                need_redraw = true;
            }
        }
        else if (key == 0x49) { // PAGE UP
            selected_index -= list_height;
            if (selected_index < 0) selected_index = 0;
            scroll_offset = selected_index;
            if (scroll_offset < 0) scroll_offset = 0;
            need_redraw = true;
        }
        else if (key == 0x51) { // PAGE DOWN
            selected_index += list_height;
            if (selected_index >= file_count) {
                selected_index = file_count > 0 ? file_count - 1 : 0;
            }
            scroll_offset = selected_index - list_height + 1;
            if (scroll_offset < 0) scroll_offset = 0;
            if (scroll_offset + list_height > file_count) {
                scroll_offset = file_count - list_height;
                if (scroll_offset < 0) scroll_offset = 0;
            }
            need_redraw = true;
        }
        else if (key == 0x47) { // HOME
            selected_index = 0;
            scroll_offset = 0;
            need_redraw = true;
        }
        else if (key == 0x4F) { // END
            selected_index = file_count > 0 ? file_count - 1 : 0;
            scroll_offset = file_count - list_height;
            if (scroll_offset < 0) scroll_offset = 0;
            need_redraw = true;
        }
        else if (key == 0x0E && strcmp(fm_current_path, "/") != 0) { // BACKSPACE
            char* last_slash = strrchr(fm_current_path, '/');
            if (last_slash && last_slash != fm_current_path) {
                *last_slash = '\0';
            } else {
                strcpy(fm_current_path, "/");
            }
            selected_index = 0;
            scroll_offset = 0;
            need_refresh = true;
        }
        else if ((key == 0x3D || key == 0x1C) && file_count > 0) { // F3 or ENTER
            if (fm_files[selected_index].is_dir) {
                if (strcmp(fm_files[selected_index].name, "..") == 0) {
                    char* last_slash = strrchr(fm_current_path, '/');
                    if (last_slash && last_slash != fm_current_path) {
                        *last_slash = '\0';
                    } else {
                        strcpy(fm_current_path, "/");
                    }
                } else {
                    strcpy(fm_current_path, fm_files[selected_index].full_path);
                }
                selected_index = 0;
                scroll_offset = 0;
                need_refresh = true;
            } else {
                fm_win.visible = false;
                terminal_clear();
                
                char* argv[] = {"cat", fm_files[selected_index].full_path};
                cat_command(2, argv);
                
                printr("\nPress any key to return...\n");
                keyboard_wait_for_key(0);
                
                terminal_clear();
                fm_win.visible = true;
                need_redraw = true;
            }
        }
        else if (key == 0x3E && file_count > 0 && !fm_files[selected_index].is_dir) { // F4
            fm_win.visible = false;
            terminal_clear();
            
            printr("Opening editor for: %s\n", fm_files[selected_index].full_path);
            printr("(Editor integration here)\n");
            printr("\nPress any key to return...\n");
            keyboard_wait_for_key(0);
            
            terminal_clear();
            fm_win.visible = true;
            need_redraw = true;
        }
        else if (key == 0x3F && file_count > 0) { // F5
            strcpy(fm_clipboard_file, fm_files[selected_index].full_path);
            clipboard_is_cut = false;
            need_redraw = true;
        }
        else if (key == 0x40 && file_count > 0) { // F6
            strcpy(fm_clipboard_file, fm_files[selected_index].full_path);
            clipboard_is_cut = true;
            need_redraw = true;
        }
        else if (key == 0x41) { // F7
            fm_win.visible = false;
            terminal_clear();
            
            printr("Create New Directory\n");
            printr("====================\n\n");
            printr("Current path: %s\n", fm_current_path);
            printr("Directory name: ");
            
            char dirname[64];
            keyboard_input(dirname);
            
            if (strlen(dirname) > 0) {
                char dir_path[512];
                if (strcmp(fm_current_path, "/") == 0) {
                    strcpy(dir_path, "/");
                    strcat(dir_path, dirname);
                } else {
                    strcpy(dir_path, fm_current_path);
                    strcat(dir_path, "/");
                    strcat(dir_path, dirname);
                }
                strcat(dir_path, "/.dir");
                
                if (avfs_create_file(dir_path, 1) == 0) {
                    printr("\nDirectory created: %s\n", dir_path);
                    need_refresh = true;
                } else {
                    printr("\nError: Failed to create directory!\n");
                }
            }
            
            printr("\nPress any key to continue...\n");
            keyboard_wait_for_key(0);
            
            terminal_clear();
            fm_win.visible = true;
            need_redraw = true;
        }
        else if ((key == 0x42 || key == 0x53) && file_count > 0) { // F8 or DELETE
            vga_window_t confirm_win = vga_create_centered_window(50, 8, VGA_COLOR_WHITE, VGA_COLOR_RED);
            vga_win_set_title(&confirm_win, fm_files[selected_index].is_dir ? "Delete Directory" : "Delete File");
            
            vga_win_puts(&confirm_win, 2, 2, fm_files[selected_index].is_dir ? "Delete this directory?" : "Delete this file?");
            vga_win_puts_colored(&confirm_win, 2, 3, fm_files[selected_index].name,
                vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_RED));
            vga_win_puts(&confirm_win, 2, 5, "This action cannot be undone!");
            vga_win_puts_centered(&confirm_win, 7, "[Y] Yes  [N] No");
            vga_win_refresh(&confirm_win);
            
            int confirm = keyboard_wait_for_key(0);
            vga_destroy_window(&confirm_win);
            
            if (confirm == 0x15) { // Y key
                if (fm_files[selected_index].is_dir) {
                    char dir_prefix[512];
                    strcpy(dir_prefix, fm_files[selected_index].full_path);
                    strcat(dir_prefix, "/");
                    int dir_prefix_len = strlen(dir_prefix);
                    
                    for (int i = 0; i < AVFS_MAX_FILES; i++) {
                        char check_name[AVFS_FILENAME_MAX];
                        uint32_t check_size;
                        if (avfs_get_file_info(i, check_name, &check_size) == 0) {
                            if (strncmp(check_name, dir_prefix, dir_prefix_len) == 0) {
                                avfs_remove_file(check_name);
                            }
                        }
                    }
                    
                    need_refresh = true;
                    if (selected_index >= file_count - 1 && selected_index > 0) {
                        selected_index--;
                    }
                } else {
                    if (avfs_remove_file(fm_files[selected_index].full_path) == 0) {
                        need_refresh = true;
                        if (selected_index >= file_count - 1 && selected_index > 0) {
                            selected_index--;
                        }
                    }
                }
            }
            need_redraw = true;
        }
        else if (key == 0x43 && file_count > 0) { // F9
            fm_win.visible = false;
            terminal_clear();
            
            printr(fm_files[selected_index].is_dir ? "Rename Directory\n" : "Rename File\n");
            printr("================\n\n");
            printr("Old name: %s\n", fm_files[selected_index].name);
            printr("New name: ");
            
            char new_name[AVFS_FILENAME_MAX];
            keyboard_input(new_name);
            
            if (strlen(new_name) > 0 && strcmp(new_name, fm_files[selected_index].name) != 0) {
                if (fm_files[selected_index].is_dir) {
                    char old_prefix[512], new_prefix[512];
                    strcpy(old_prefix, fm_files[selected_index].full_path);
                    strcat(old_prefix, "/");
                    
                    if (strcmp(fm_current_path, "/") == 0) {
                        strcpy(new_prefix, "/");
                        strcat(new_prefix, new_name);
                    } else {
                        strcpy(new_prefix, fm_current_path);
                        strcat(new_prefix, "/");
                        strcat(new_prefix, new_name);
                    }
                    strcat(new_prefix, "/");
                    
                    int old_prefix_len = strlen(old_prefix);
                    
                    for (int i = 0; i < AVFS_MAX_FILES; i++) {
                        char check_name[AVFS_FILENAME_MAX];
                        uint32_t check_size;
                        if (avfs_get_file_info(i, check_name, &check_size) == 0) {
                            if (strncmp(check_name, old_prefix, old_prefix_len) == 0) {
                                char new_path[512];
                                strcpy(new_path, new_prefix);
                                strcat(new_path, check_name + old_prefix_len);
                                
                                char* temp_buf = (char*)malloc(check_size);
                                if (temp_buf && avfs_read_file(check_name, temp_buf, check_size, 0) == 0) {
                                    if (avfs_create_file(new_path, check_size) == 0) {
                                        if (avfs_write_file(new_path, temp_buf, check_size, 0) == 0) {
                                            avfs_remove_file(check_name);
                                        }
                                    }
                                    free(temp_buf);
                                }
                            }
                        }
                    }
                    
                    printr("\nDirectory renamed successfully!\n");
                    need_refresh = true;
                } else {
                    char new_path[512];
                    if (strcmp(fm_current_path, "/") == 0) {
                        strcpy(new_path, "/");
                        strcat(new_path, new_name);
                    } else {
                        strcpy(new_path, fm_current_path);
                        strcat(new_path, "/");
                        strcat(new_path, new_name);
                    }
                    
                    int old_size = fm_files[selected_index].size;
                    char* temp_buf = (char*)malloc(old_size);
                    
                    if (temp_buf && avfs_read_file(fm_files[selected_index].full_path, temp_buf, old_size, 0) == 0) {
                        if (avfs_create_file(new_path, old_size) == 0) {
                            if (avfs_write_file(new_path, temp_buf, old_size, 0) == 0) {
                                if (avfs_remove_file(fm_files[selected_index].full_path) == 0) {
                                    printr("\nRenamed successfully!\n");
                                    need_refresh = true;
                                } else {
                                    printr("\nError: Failed to remove old!\n");
                                }
                            } else {
                                printr("\nError: Failed to write new!\n");
                            }
                        } else {
                            printr("\nError: Failed to create new!\n");
                        }
                        free(temp_buf);
                    } else {
                        printr("\nError: Failed to read old!\n");
                        if (temp_buf) free(temp_buf);
                    }
                }
            }
            
            printr("\nPress any key to continue...\n");
            keyboard_wait_for_key(0);
            
            terminal_clear();
            fm_win.visible = true;
            need_redraw = true;
        }
        else if (key == 0x3C) { // F2
            view_mode = (view_mode + 1) % 3;
            list_height = view_mode == 2 ? 7 : 11;
            if (selected_index >= scroll_offset + list_height) {
                scroll_offset = selected_index - list_height + 1;
            }
            if (scroll_offset < 0) scroll_offset = 0;
            need_redraw = true;
        }
        else if (key == 0x1F) { // S
            sort_mode = (sort_mode + 1) % 3;
            need_refresh = true;
        }
        else if (key == 0x13) { // R
            sort_reverse = !sort_reverse;
            need_refresh = true;
        }
        else if (key == 0x23) { // H
            show_hidden = !show_hidden;
            need_refresh = true;
        }
        else if (key == 0x3B) { // F1
            show_help = true;
            need_redraw = true;
        }
        else if (key == 0x01) { // ESC
            running = false;
        }
    }
    
    vga_destroy_window(&fm_win);
    terminal_clear();
}
typedef enum {
    PAGE_MAIN,
    PAGE_HELP,
    PAGE_NETWORK,
    PAGE_ABOUT,
    PAGE_COMMANDS,
    PAGE_SYSTEM_INFO,
    PAGE_FEATURES,
    PAGE_CREDITS,
    PAGE_DRIVERS,
    PAGE_FILESYSTEM,
    PAGE_ADVANCED,      // NEW: Advanced settings
    PAGE_MEMORY,        // NEW: Memory information
    PAGE_NETWORKING,    // NEW: Detailed network info
    PAGE_SECURITY,      // NEW: Security features
    PAGE_TOOLS,          // NEW: Development tools
    PAGE_SECRET
} welcome_page_t;

void rawr(void) {
    welcome_page_t current_page = PAGE_MAIN;
    bool running = true;
    int frame = 0;
    
    while (running) {
        vga_window_t main_win;
        
        switch (current_page) {
            case PAGE_MAIN: {
    main_win = vga_create_centered_window(72, 24, VGA_COLOR_WHITE, VGA_COLOR_CYAN);
    vga_win_set_title(&main_win, "Welcome to RadiumOS");
    
    // ASCII Logo
    vga_win_puts_colored(&main_win, 11, 2, "  ____            _ _                 ___  ____  ", 
        vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_CYAN));
    vga_win_puts_colored(&main_win, 11, 3, " |  _ \\ __ _  __| (_)_   _ _ __ ___ / _ \\/ ___| ", 
        vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_CYAN));
    vga_win_puts_colored(&main_win, 11, 4, " | |_) / _` |/ _` | | | | | '_ ` _ \\ | | \\___ \\ ", 
        vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_CYAN));
    vga_win_puts_colored(&main_win, 11, 5, " |  _ < (_| | (_| | | |_| | | | | | | |_| |___) |", 
        vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_CYAN));
    vga_win_puts_colored(&main_win, 11, 6, " |_| \\_\\__,_|\\__,_|_|\\__,_|_| |_| |_|\\___/|____/ ", 
        vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_CYAN));
    
    vga_win_puts_centered(&main_win, 8, "A Modern Hobby Operating System");
    
    vga_win_puts_colored(&main_win, 3, 10, "Version: Alpha 1.0", 
        vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_CYAN));
    vga_win_puts_colored(&main_win, 3, 11, "Build: 2024.11.25", 
        vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_CYAN));
    vga_win_puts_colored(&main_win, 3, 12, "Developed by: Jose (Cube)", 
        vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_CYAN));
    
    vga_win_puts_colored(&main_win, 3, 14, "Quick Start:", 
        vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_CYAN));
    vga_win_puts(&main_win, 3, 15, "  * Press [T] to launch terminal");
    vga_win_puts(&main_win, 3, 16, "  * Press [S] for live shell");
    vga_win_puts(&main_win, 3, 17, "  * Press [H] for keyboard shortcuts");
    vga_win_puts(&main_win, 3, 18, "  * Press [C] to see all commands");
    
    vga_win_draw_line_h(&main_win, 2, 20, 68, 0xC4);
    vga_win_puts_colored(&main_win, 2, 21, "Navigation:", 
        vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_CYAN));
    vga_win_puts(&main_win, 2, 22, "[H]Help [N]Network [A]About [I]Info [V]Advanced [ESC]Exit");
    
    // Display time in top-right corner
    display_menu_time(&main_win, 0);
    
    // Initial refresh
    vga_win_refresh(&main_win);
    

    

    break;
}
            
            case PAGE_HELP: {
                main_win = vga_create_centered_window(72, 24, VGA_COLOR_WHITE, VGA_COLOR_BLUE);
                vga_win_set_title(&main_win, "Help - Keyboard Shortcuts");
                
                vga_win_puts_colored(&main_win, 2, 2, "KEYBOARD SHORTCUTS", 
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLUE));
                vga_win_draw_line_h(&main_win, 2, 3, 68, 0xC4);
                
                vga_win_puts_colored(&main_win, 2, 5, "System Controls:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLUE));
                vga_win_puts(&main_win, 4, 6,  "[T] Terminal       - Launch interactive shell");
                vga_win_puts(&main_win, 4, 7,  "[S] Live Shell     - Interactive command preview");
                vga_win_puts(&main_win, 4, 8,  "[R] Reboot         - Restart the system");
                vga_win_puts(&main_win, 4, 9,  "[ESC] Skip         - Exit welcome screen");
                
                vga_win_puts_colored(&main_win, 2, 11, "Information Pages:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLUE));
                vga_win_puts(&main_win, 4, 12, "[H] Help           - Show this page");
                vga_win_puts(&main_win, 4, 13, "[N] Network        - Network status");
                vga_win_puts(&main_win, 4, 14, "[A] About          - About RadiumOS");
                vga_win_puts(&main_win, 4, 15, "[I] System Info    - Hardware information");
                vga_win_puts(&main_win, 4, 16, "[D] Drivers        - Driver status");
                vga_win_puts(&main_win, 4, 17, "[G] Memory         - Memory details");
                
                vga_win_puts_colored(&main_win, 2, 19, "Applications:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLUE));
                vga_win_puts(&main_win, 4, 20, "[F] File Manager   - Browse files");
                vga_win_puts(&main_win, 4, 21, "[V] Advanced       - Advanced settings");
                
                vga_win_draw_line_h(&main_win, 2, 22, 68, 0xC4);
                vga_win_puts_centered(&main_win, 23, "Press [M] to return to main page");
                
                vga_win_refresh(&main_win);
                break;
            }
            
            case PAGE_NETWORK: {
                main_win = vga_create_centered_window(72, 24, VGA_COLOR_WHITE, VGA_COLOR_MAGENTA);
                vga_win_set_title(&main_win, "Network Status");
                
                vga_win_puts_colored(&main_win, 2, 2, "NETWORK CONFIGURATION", 
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_MAGENTA));
                vga_win_draw_line_h(&main_win, 2, 3, 68, 0xC4);
                
                vga_win_puts_colored(&main_win, 2, 5, "Network Interface:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_MAGENTA));
                vga_win_puts(&main_win, 4, 6,  "Interface:       eth0 (RTL8139)");
                vga_win_puts(&main_win, 4, 7,  "IP Address:      192.168.1.50");
                vga_win_puts(&main_win, 4, 8,  "Subnet Mask:     255.255.255.0");
                vga_win_puts(&main_win, 4, 9,  "Gateway:         192.168.1.1");
                vga_win_puts(&main_win, 4, 10, "MAC Address:     52:54:00:12:34:56");
                vga_win_puts_colored(&main_win, 4, 11, "Status:          ACTIVE", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_MAGENTA));
                
                vga_win_puts_colored(&main_win, 2, 13, "Protocol Stack:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_MAGENTA));
                vga_win_puts_colored(&main_win, 4, 14, "[OK] Ethernet (Layer 2)", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_MAGENTA));
                vga_win_puts_colored(&main_win, 4, 15, "[OK] ARP (Address Resolution)", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_MAGENTA));
                vga_win_puts_colored(&main_win, 4, 16, "[OK] IP (Internet Protocol)", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_MAGENTA));
                vga_win_puts_colored(&main_win, 4, 17, "[OK] ICMP (Ping)", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_MAGENTA));
                vga_win_puts_colored(&main_win, 4, 18, "[OK] TCP (Transmission Control)", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_MAGENTA));
                
                vga_win_draw_line_h(&main_win, 2, 20, 68, 0xC4);
                vga_win_puts(&main_win, 2, 21, "Commands: ping, ifconfig, arp, curl, network");
                vga_win_puts_centered(&main_win, 22, "Press [M] Main | [W] Detailed Network Info");
                
                vga_win_refresh(&main_win);
                break;
            }
            
            case PAGE_ABOUT: {
                main_win = vga_create_centered_window(72, 24, VGA_COLOR_WHITE, VGA_COLOR_GREEN);
                vga_win_set_title(&main_win, "About RadiumOS");
                
                vga_win_puts_colored(&main_win, 2, 2, "ABOUT RADIUMOS", 
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_GREEN));
                vga_win_draw_line_h(&main_win, 2, 3, 68, 0xC4);
                
                vga_win_puts_colored(&main_win, 2, 5, "Project Information:", 
                    vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_GREEN));
                vga_win_puts(&main_win, 4, 6,  "RadiumOS is a hobby operating system built from scratch");
                vga_win_puts(&main_win, 4, 7,  "in C and x86 assembly. It features a custom kernel with");
                vga_win_puts(&main_win, 4, 8,  "multitasking, networking, and a graphical window manager.");
                
                vga_win_puts_colored(&main_win, 2, 10, "Key Technologies:", 
                    vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_GREEN));
                vga_win_puts(&main_win, 4, 11, "* x86 Protected Mode (32-bit)");
                vga_win_puts(&main_win, 4, 12, "* Custom bootloader (GRUB compatible)");
                vga_win_puts(&main_win, 4, 13, "* Preemptive multitasking scheduler");
                vga_win_puts(&main_win, 4, 14, "* TCP/IP network stack");
                vga_win_puts(&main_win, 4, 15, "* VGA text mode graphics");
                vga_win_puts(&main_win, 4, 16, "* Virtual filesystem (AVFS)");
                vga_win_puts(&main_win, 4, 17, "* Script interpreter (RSH)");
                
                vga_win_puts_colored(&main_win, 2, 19, "License:", 
                    vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_GREEN));
                vga_win_puts(&main_win, 4, 20, "Educational/Hobbyist Project - 2024");
                
                vga_win_draw_line_h(&main_win, 2, 21, 68, 0xC4);
                vga_win_puts_centered(&main_win, 22, "Press [M] Main | [X] Credits | [E] Security");
                
                vga_win_refresh(&main_win);
                break;
            }
            
            case PAGE_COMMANDS: {
                main_win = vga_create_centered_window(72, 24, VGA_COLOR_WHITE, VGA_COLOR_RED);
                vga_win_set_title(&main_win, "Command Reference");
                
                vga_win_puts_colored(&main_win, 2, 2, "AVAILABLE COMMANDS", 
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_RED));
                vga_win_draw_line_h(&main_win, 2, 3, 68, 0xC4);
                
                vga_win_puts_colored(&main_win, 2, 5, "File Operations:", 
                    vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_RED));
                vga_win_puts(&main_win, 4, 6,  "ls        - List files");
                vga_win_puts(&main_win, 4, 7,  "cat       - Display file contents");
                vga_win_puts(&main_win, 4, 8,  "rm        - Remove file");
                vga_win_puts(&main_win, 4, 9,  "onan      - Text editor");
                
                vga_win_puts_colored(&main_win, 36, 5, "Network:", 
                    vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_RED));
                vga_win_puts(&main_win, 38, 6,  "ping      - Test connectivity");
                vga_win_puts(&main_win, 38, 7,  "ifconfig  - Network config");
                vga_win_puts(&main_win, 38, 8,  "arp       - ARP table");
                vga_win_puts(&main_win, 38, 9,  "curl      - HTTP client");
                
                vga_win_puts_colored(&main_win, 2, 11, "System:", 
                    vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_RED));
                vga_win_puts(&main_win, 4, 12, "help      - Show help");
                vga_win_puts(&main_win, 4, 13, "clear     - Clear screen");
                vga_win_puts(&main_win, 4, 14, "date      - Show date/time");
                vga_win_puts(&main_win, 4, 15, "radifetch - System info");
                vga_win_puts(&main_win, 4, 16, "reboot    - Restart system");
                vga_win_puts(&main_win, 4, 17, "settings  - Configuration");
                
                vga_win_puts_colored(&main_win, 36, 11, "Utilities:", 
                    vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_RED));
                vga_win_puts(&main_win, 38, 12, "echo      - Print text");
                vga_win_puts(&main_win, 38, 13, "cowsay    - ASCII cow");
                vga_win_puts(&main_win, 38, 14, "script    - Run scripts");
                vga_win_puts(&main_win, 38, 15, "mpop      - Programming");
                vga_win_puts(&main_win, 38, 16, "brainfuck - BF interpreter");
                vga_win_puts(&main_win, 38, 17, "tui       - GUI launcher");
                
                vga_win_draw_line_h(&main_win, 2, 19, 68, 0xC4);
                vga_win_puts(&main_win, 2, 20, "Type 'help' in the terminal for full descriptions");
                vga_win_puts_centered(&main_win, 22, "Press [M] Main | [O] Development Tools");
                
                vga_win_refresh(&main_win);
                break;
            }
            
            case PAGE_SYSTEM_INFO: {
                main_win = vga_create_centered_window(72, 24, VGA_COLOR_WHITE, VGA_COLOR_BROWN);
                vga_win_set_title(&main_win, "System Information");
                
                vga_win_puts_colored(&main_win, 2, 2, "HARDWARE INFORMATION", 
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BROWN));
                vga_win_draw_line_h(&main_win, 2, 3, 68, 0xC4);
                
                vga_win_puts_colored(&main_win, 2, 5, "CPU:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BROWN));
                vga_win_puts(&main_win, 4, 6,  "Architecture:    x86 (32-bit)");
                vga_win_puts(&main_win, 4, 7,  "Mode:            Protected Mode");
                vga_win_puts(&main_win, 4, 8,  "Features:        FPU, TSC, MSR");
                
                vga_win_puts_colored(&main_win, 2, 10, "Memory:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BROWN));
                vga_win_puts(&main_win, 4, 11, "Available:       128 MB");
                vga_win_puts(&main_win, 4, 12, "Kernel:          1 MB");
                vga_win_puts(&main_win, 4, 13, "User Space:      127 MB");
                vga_win_puts(&main_win, 4, 14, "Allocator:       Dynamic");
                
                vga_win_puts_colored(&main_win, 2, 16, "Display:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BROWN));
                vga_win_puts(&main_win, 4, 17, "Mode:            VGA Text 80x25");
                vga_win_puts(&main_win, 4, 18, "Colors:          16 color palette");
                vga_win_puts(&main_win, 4, 19, "Windows:         Yes (custom WM)");
                
                vga_win_draw_line_h(&main_win, 2, 21, 68, 0xC4);
                vga_win_puts_centered(&main_win, 22, "Press [M] Main | [D] Drivers | [G] Memory Details");
                
                vga_win_refresh(&main_win);
                break;
            }
            
            case PAGE_FEATURES: {
                main_win = vga_create_centered_window(72, 24, VGA_COLOR_WHITE, VGA_COLOR_DARK_GREY);
                vga_win_set_title(&main_win, "Features & Capabilities");
                
                vga_win_puts_colored(&main_win, 2, 2, "RADIUMOS FEATURES", 
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_DARK_GREY));
                vga_win_draw_line_h(&main_win, 2, 3, 68, 0xC4);
                
                vga_win_puts_colored(&main_win, 2, 5, "[OK] Kernel Features:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_DARK_GREY));
                vga_win_puts(&main_win, 4, 6,  "* Protected mode with GDT/IDT");
                vga_win_puts(&main_win, 4, 7,  "* Preemptive multitasking (round-robin)");
                vga_win_puts(&main_win, 4, 8,  "* Hardware interrupt handling");
                vga_win_puts(&main_win, 4, 9,  "* Timer-based scheduling (1000Hz)");
                
                vga_win_puts_colored(&main_win, 2, 11, "[OK] I/O & Drivers:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_DARK_GREY));
                vga_win_puts(&main_win, 4, 12, "* PS/2 Keyboard driver");
                vga_win_puts(&main_win, 4, 13, "* VGA text mode driver");
                vga_win_puts(&main_win, 4, 14, "* RTC (Real-Time Clock)");
                vga_win_puts(&main_win, 4, 15, "* RTL8139 network card");
                vga_win_puts(&main_win, 4, 16, "* PC speaker (beep)");
                
                vga_win_puts_colored(&main_win, 2, 18, "[OK] Software:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_DARK_GREY));
                vga_win_puts(&main_win, 4, 19, "* Shell with 25+ commands");
                vga_win_puts(&main_win, 4, 20, "* Script interpreter (RSH)");
                vga_win_puts(&main_win, 4, 21, "* Programming languages (MPOP, Brainfuck)");
                
                vga_win_draw_line_h(&main_win, 2, 22, 68, 0xC4);
                vga_win_puts_centered(&main_win, 23, "Press [M] to return to main page");
                
                vga_win_refresh(&main_win);
                break;
            }
            
            case PAGE_CREDITS: {
                main_win = vga_create_centered_window(72, 24, VGA_COLOR_WHITE, VGA_COLOR_LIGHT_MAGENTA);
                vga_win_set_title(&main_win, "Credits & Acknowledgments");
                
                vga_win_puts_colored(&main_win, 2, 2, "CREDITS", 
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_LIGHT_MAGENTA));
                vga_win_draw_line_h(&main_win, 2, 3, 68, 0xC4);
                
                vga_win_puts_centered(&main_win, 5, "RadiumOS Development Team");
                
                vga_win_puts_colored(&main_win, 2, 7, "Lead Developer:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_LIGHT_MAGENTA));
                vga_win_puts(&main_win, 4, 8,  "Jose (Cube) - @scp_2801");
                vga_win_puts(&main_win, 4, 9,  "Kernel, drivers, networking, and system design");
                
                vga_win_puts_colored(&main_win, 2, 11, "Special Thanks:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_LIGHT_MAGENTA));
                vga_win_puts(&main_win, 4, 12, "* OSDev Community - Documentation and support");
                vga_win_puts(&main_win, 4, 13, "* Intel & AMD - CPU architecture documentation");
                vga_win_puts(&main_win, 4, 14, "* Realtek - RTL8139 network card specs");
                
                vga_win_puts_colored(&main_win, 2, 16, "Technologies Used:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_LIGHT_MAGENTA));
                vga_win_puts(&main_win, 4, 17, "* Clang/GCC - C compiler");
                vga_win_puts(&main_win, 4, 18, "* NASM - Assembler");
                vga_win_puts(&main_win, 4, 19, "* GRUB - Bootloader");
                vga_win_puts(&main_win, 4, 20, "* QEMU - Testing and emulation");
                
                vga_win_draw_line_h(&main_win, 2, 22, 68, 0xC4);
                vga_win_puts_centered(&main_win, 23, "Press [M] Main | [A] About");
                
                vga_win_refresh(&main_win);
                break;
            }
            
            case PAGE_DRIVERS: {
                main_win = vga_create_centered_window(72, 24, VGA_COLOR_BLACK, VGA_COLOR_LIGHT_CYAN);
                vga_win_set_title(&main_win, "Driver Status");
                
                vga_win_puts_colored(&main_win, 2, 2, "SYSTEM DRIVERS", 
                    vga_entry_color(VGA_COLOR_BLUE, VGA_COLOR_LIGHT_CYAN));
                vga_win_draw_line_h(&main_win, 2, 3, 68, 0xC4);
                
                vga_win_puts_colored(&main_win, 2, 5, "Core Drivers:", 
                    vga_entry_color(VGA_COLOR_BLUE, VGA_COLOR_LIGHT_CYAN));
                vga_win_puts_colored(&main_win, 4, 6,  "[RUNNING] rtl8139.drv   - RTL8139 Network Card", 
                    vga_entry_color(VGA_COLOR_GREEN, VGA_COLOR_LIGHT_CYAN));
                vga_win_puts_colored(&main_win, 4, 7,  "[RUNNING] rtc.drv       - Real-Time Clock", 
                    vga_entry_color(VGA_COLOR_GREEN, VGA_COLOR_LIGHT_CYAN));
                vga_win_puts_colored(&main_win, 4, 8,  "[RUNNING] gdt.drv       - Global Descriptor Table", 
                    vga_entry_color(VGA_COLOR_GREEN, VGA_COLOR_LIGHT_CYAN));
                vga_win_puts_colored(&main_win, 4, 9,  "[RUNNING] interrupts.drv - IRQ Handler", 
                    vga_entry_color(VGA_COLOR_GREEN, VGA_COLOR_LIGHT_CYAN));
                vga_win_puts_colored(&main_win, 4, 10, "[RUNNING] pit.drv       - Programmable Timer", 
                    vga_entry_color(VGA_COLOR_GREEN, VGA_COLOR_LIGHT_CYAN));
                vga_win_puts_colored(&main_win, 4, 11, "[RUNNING] scheduler.drv - Task Scheduler", 
                    vga_entry_color(VGA_COLOR_GREEN, VGA_COLOR_LIGHT_CYAN));
                
                vga_win_puts_colored(&main_win, 2, 13, "Peripheral Drivers:", 
                    vga_entry_color(VGA_COLOR_BLUE, VGA_COLOR_LIGHT_CYAN));
                vga_win_puts_colored(&main_win, 4, 14, "[LOADED]  keyboard      - PS/2 Keyboard", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_LIGHT_CYAN));
                vga_win_puts_colored(&main_win, 4, 15, "[LOADED]  vga           - VGA Text Mode", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_LIGHT_CYAN));
                vga_win_puts_colored(&main_win, 4, 16, "[LOADED]  speaker       - PC Speaker", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_LIGHT_CYAN));
                
                vga_win_puts_colored(&main_win, 2, 18, "Driver Statistics:", 
                    vga_entry_color(VGA_COLOR_BLUE, VGA_COLOR_LIGHT_CYAN));
                vga_win_puts(&main_win, 4, 19, "Total Drivers:   9");
                vga_win_puts_colored(&main_win, 4, 20, "Running:         6", 
                    vga_entry_color(VGA_COLOR_GREEN, VGA_COLOR_LIGHT_CYAN));
                vga_win_puts_colored(&main_win, 4, 21, "Loaded:          3", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_LIGHT_CYAN));
                
                vga_win_draw_line_h(&main_win, 2, 22, 68, 0xC4);
                vga_win_puts_centered(&main_win, 23, "Press [M] Main | [I] System Info");
                
                vga_win_refresh(&main_win);
                break;
            }
            
            case PAGE_FILESYSTEM: {
                main_win = vga_create_centered_window(72, 24, VGA_COLOR_WHITE, VGA_COLOR_LIGHT_RED);
                vga_win_set_title(&main_win, "Filesystem Information");
                
                vga_win_puts_colored(&main_win, 2, 2, "AVFS - RADIUMOS VIRTUAL FILESYSTEM", 
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_LIGHT_RED));
                vga_win_draw_line_h(&main_win, 2, 3, 68, 0xC4);
                
                vga_win_puts_colored(&main_win, 2, 5, "Filesystem Specifications:", 
                    vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_RED));
                vga_win_puts(&main_win, 4, 6,  "Type:            AVFS (RadiumOS Virtual FS)");
                vga_win_puts(&main_win, 4, 7,  "Block Size:      512 bytes");
                vga_win_puts(&main_win, 4, 8,  "Total Blocks:    1024");
                vga_win_puts(&main_win, 4, 9,  "Capacity:        512 KB");
                vga_win_puts(&main_win, 4, 10, "Max Files:       64");
                vga_win_puts(&main_win, 4, 11, "Max Filename:    32 characters");
                
                vga_win_puts_colored(&main_win, 2, 13, "Features:", 
                    vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_RED));
                vga_win_puts(&main_win, 4, 14, "* Dynamic file allocation");
                vga_win_puts(&main_win, 4, 15, "* File append support");
                vga_win_puts(&main_win, 4, 16, "* Block-level storage");
                vga_win_puts(&main_win, 4, 17, "* Content search (insideFile)");
                vga_win_puts(&main_win, 4, 18, "* Atomic operations");
                
                vga_win_puts_colored(&main_win, 2, 20, "Mounted Files:", 
                    vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_RED));
                vga_win_puts(&main_win, 4, 21, "See 'ls' command for current files");
                
                vga_win_draw_line_h(&main_win, 2, 22, 68, 0xC4);
                vga_win_puts_centered(&main_win, 23, "Press [M] Main | [I] System Info | [F] File Manager");
                
                vga_win_refresh(&main_win);
                break;
            }
            
            // ================================================================
            // NEW PAGE: ADVANCED SETTINGS
            // ================================================================
            case PAGE_ADVANCED: {
                main_win = vga_create_centered_window(72, 24, VGA_COLOR_WHITE, VGA_COLOR_LIGHT_BLUE);
                vga_win_set_title(&main_win, "Advanced Settings");
                
                vga_win_puts_colored(&main_win, 2, 2, "ADVANCED CONFIGURATION", 
                    vga_entry_color(VGA_COLOR_BLUE, VGA_COLOR_LIGHT_BLUE));
                vga_win_draw_line_h(&main_win, 2, 3, 68, 0xC4);
                
                vga_win_puts_colored(&main_win, 2, 5, "Kernel Parameters:", 
                    vga_entry_color(VGA_COLOR_BLUE, VGA_COLOR_LIGHT_BLUE));
                vga_win_puts(&main_win, 4, 6,  "Scheduler:       Round-Robin (1000 Hz)");
                vga_win_puts(&main_win, 4, 7,  "Time Slice:      10 ms per task");
                vga_win_puts(&main_win, 4, 8,  "Max Tasks:       256");
                vga_win_puts(&main_win, 4, 9,  "Stack Size:      4 KB per task");
                vga_win_puts_colored(&main_win, 4, 10, "Preemption:      ENABLED", 
                    vga_entry_color(VGA_COLOR_GREEN, VGA_COLOR_LIGHT_BLUE));
                
                vga_win_puts_colored(&main_win, 2, 12, "Memory Configuration:", 
                    vga_entry_color(VGA_COLOR_BLUE, VGA_COLOR_LIGHT_BLUE));
                vga_win_puts(&main_win, 4, 13, "Heap Start:      0x100000 (1 MB)");
                vga_win_puts(&main_win, 4, 14, "Heap End:        0x8000000 (128 MB)");
                vga_win_puts(&main_win, 4, 15, "Page Size:       4 KB");
                vga_win_puts_colored(&main_win, 4, 16, "Paging:          DISABLED (Flat memory)", 
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_LIGHT_BLUE));
                
                vga_win_puts_colored(&main_win, 2, 18, "Boot Options:", 
                    vga_entry_color(VGA_COLOR_BLUE, VGA_COLOR_LIGHT_BLUE));
                vga_win_puts_colored(&main_win, 4, 19, "Autoexec.rsh:    ENABLED", 
                    vga_entry_color(VGA_COLOR_GREEN, VGA_COLOR_LIGHT_BLUE));
                vga_win_puts_colored(&main_win, 4, 20, "Welcome Screen:  ENABLED", 
                    vga_entry_color(VGA_COLOR_GREEN, VGA_COLOR_LIGHT_BLUE));
                vga_win_puts_colored(&main_win, 4, 21, "Debug Mode:      DISABLED", 
                    vga_entry_color(VGA_COLOR_DARK_GREY, VGA_COLOR_LIGHT_BLUE));
                
                vga_win_draw_line_h(&main_win, 2, 22, 68, 0xC4);
                vga_win_puts_centered(&main_win, 23, "Press [M] Main | [G] Memory | [E] Security");
                
                vga_win_refresh(&main_win);
                break;
            }
            
            // ================================================================
            // NEW PAGE: MEMORY DETAILS
            // ================================================================
            case PAGE_MEMORY: {
                main_win = vga_create_centered_window(72, 24, VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREEN);
                vga_win_set_title(&main_win, "Memory Information");
                
                vga_win_puts_colored(&main_win, 2, 2, "MEMORY LAYOUT & USAGE", 
                    vga_entry_color(VGA_COLOR_BLUE, VGA_COLOR_LIGHT_GREEN));
                vga_win_draw_line_h(&main_win, 2, 3, 68, 0xC4);
                
                vga_win_puts_colored(&main_win, 2, 5, "Memory Map:", 
                    vga_entry_color(VGA_COLOR_BLUE, VGA_COLOR_LIGHT_GREEN));
                vga_win_puts(&main_win, 4, 6,  "0x00000000 - 0x000003FF   Real Mode IVT (1 KB)");
                vga_win_puts(&main_win, 4, 7,  "0x00000400 - 0x000004FF   BIOS Data Area (256 B)");
                vga_win_puts(&main_win, 4, 8,  "0x00007C00 - 0x00007DFF   Bootloader (512 B)");
                vga_win_puts(&main_win, 4, 9,  "0x00100000 - 0x001FFFFF   Kernel (~1 MB)");
                vga_win_puts(&main_win, 4, 10, "0x00200000 - 0x07FFFFFF   User Space (126 MB)");
                vga_win_puts(&main_win, 4, 11, "0xB8000    - 0xB8FA0      VGA Text Buffer (4 KB)");
                
                vga_win_puts_colored(&main_win, 2, 13, "Heap Allocator:", 
                    vga_entry_color(VGA_COLOR_BLUE, VGA_COLOR_LIGHT_GREEN));
                vga_win_puts(&main_win, 4, 14, "Algorithm:       First-fit");
                vga_win_puts(&main_win, 4, 15, "Block Header:    16 bytes");
                vga_win_puts(&main_win, 4, 16, "Alignment:       16 bytes");
                vga_win_puts(&main_win, 4, 17, "Fragmentation:   Coalescing on free");
                
                vga_win_puts_colored(&main_win, 2, 19, "Current Usage:", 
                    vga_entry_color(VGA_COLOR_BLUE, VGA_COLOR_LIGHT_GREEN));
                vga_win_puts(&main_win, 4, 20, "Kernel Memory:   ~1 MB");
                vga_win_puts(&main_win, 4, 21, "Available:       ~127 MB");
                
                vga_win_draw_line_h(&main_win, 2, 22, 68, 0xC4);
                vga_win_puts_centered(&main_win, 23, "Press [M] Main | [I] System Info | [V] Advanced");
                
                vga_win_refresh(&main_win);
                break;
            }
            
            // ================================================================
            // NEW PAGE: DETAILED NETWORKING
            // ================================================================
            case PAGE_NETWORKING: {
                main_win = vga_create_centered_window(72, 24, VGA_COLOR_WHITE, VGA_COLOR_DARK_GREY);
                vga_win_set_title(&main_win, "Network Stack Details");
                
                vga_win_puts_colored(&main_win, 2, 2, "TCP/IP PROTOCOL STACK", 
                    vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_DARK_GREY));
                vga_win_draw_line_h(&main_win, 2, 3, 68, 0xC4);
                
                vga_win_puts_colored(&main_win, 2, 5, "Layer 2 - Data Link:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_DARK_GREY));
                vga_win_puts(&main_win, 4, 6,  "Driver:          RTL8139 NIC");
                vga_win_puts(&main_win, 4, 7,  "Frame Format:    Ethernet II");
                vga_win_puts(&main_win, 4, 8,  "MTU:             1500 bytes");
                vga_win_puts_colored(&main_win, 4, 9,  "Status:          ACTIVE", 
                    vga_entry_color(VGA_COLOR_GREEN, VGA_COLOR_DARK_GREY));
                
                vga_win_puts_colored(&main_win, 2, 11, "Layer 3 - Network:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_DARK_GREY));
                vga_win_puts(&main_win, 4, 12, "Protocol:        IPv4");
                vga_win_puts(&main_win, 4, 13, "ARP Cache:       Enabled");
                vga_win_puts(&main_win, 4, 14, "ICMP:            Ping/Echo Reply");
                vga_win_puts(&main_win, 4, 15, "Routing:         Single interface");
                
                vga_win_puts_colored(&main_win, 2, 17, "Layer 4 - Transport:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_DARK_GREY));
                vga_win_puts(&main_win, 4, 18, "TCP:             3-way handshake");
                vga_win_puts(&main_win, 4, 19, "UDP:             Not implemented");
                vga_win_puts(&main_win, 4, 20, "Ports:           Dynamic allocation");
                
                vga_win_draw_line_h(&main_win, 2, 21, 68, 0xC4);
                vga_win_puts(&main_win, 2, 22, "Commands: ping, ifconfig, arp, curl");
                vga_win_puts_centered(&main_win, 23, "Press [M] Main | [N] Network Status");
                
                vga_win_refresh(&main_win);
                break;
            }
            
            // ================================================================
            // NEW PAGE: SECURITY
            // ================================================================
            case PAGE_SECURITY: {
                main_win = vga_create_centered_window(72, 24, VGA_COLOR_WHITE, VGA_COLOR_RED);
                vga_win_set_title(&main_win, "Security Features");
                
                vga_win_puts_colored(&main_win, 2, 2, "SECURITY & PROTECTION", 
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_RED));
                vga_win_draw_line_h(&main_win, 2, 3, 68, 0xC4);
                
                vga_win_puts_colored(&main_win, 2, 5, "Memory Protection:", 
                    vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_RED));
                vga_win_puts_colored(&main_win, 4, 6,  "Paging:          DISABLED", 
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_RED));
                vga_win_puts(&main_win, 4, 7,  "Privilege Levels: Ring 0 only (Kernel mode)");
                vga_win_puts_colored(&main_win, 4, 8,  "User Mode:       NOT IMPLEMENTED", 
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_RED));
                vga_win_puts(&main_win, 4, 9,  "Note: All code runs in kernel space");
                
                vga_win_puts_colored(&main_win, 2, 11, "Authentication:", 
                    vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_RED));
                vga_win_puts(&main_win, 4, 12, "Username:        Stored in /bin/username.cfg");
                vga_win_puts(&main_win, 4, 13, "Password:        Stored in /bin/password.cfg");
                vga_win_puts_colored(&main_win, 4, 14, "Encryption:      Basic XOR (educational)", 
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_RED));
                vga_win_puts(&main_win, 4, 15, "Login Required:  Configurable");
                
                vga_win_puts_colored(&main_win, 2, 17, "Network Security:", 
                    vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_RED));
                vga_win_puts_colored(&main_win, 4, 18, "Firewall:        NONE", 
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_RED));
                vga_win_puts_colored(&main_win, 4, 19, "TLS/SSL:         NOT SUPPORTED", 
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_RED));
                vga_win_puts(&main_win, 4, 20, "Note: This is an educational OS");
                
                vga_win_draw_line_h(&main_win, 2, 21, 68, 0xC4);
                vga_win_puts_centered(&main_win, 22, "WARNING: Not suitable for production use!");
                vga_win_puts_centered(&main_win, 23, "Press [M] Main | [A] About | [V] Advanced");
                
                vga_win_refresh(&main_win);
                break;
            }
            
            // ================================================================
            // NEW PAGE: DEVELOPMENT TOOLS
            // ================================================================
            case PAGE_TOOLS: {
                main_win = vga_create_centered_window(72, 24, VGA_COLOR_WHITE, VGA_COLOR_MAGENTA);
                vga_win_set_title(&main_win, "Development Tools");
                
                vga_win_puts_colored(&main_win, 2, 2, "DEVELOPMENT & PROGRAMMING", 
                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_MAGENTA));
                vga_win_draw_line_h(&main_win, 2, 3, 68, 0xC4);
                
                vga_win_puts_colored(&main_win, 2, 5, "Programming Languages:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_MAGENTA));
                vga_win_puts(&main_win, 4, 6,  "MPOP             - Stack-based programming language");
                vga_win_puts(&main_win, 4, 7,  "Brainfuck        - Esoteric language interpreter");
                vga_win_puts(&main_win, 4, 8,  "RSH              - RadiumOS Shell scripting");
                vga_win_puts(&main_win, 4, 9,  "Usage: mpop <code>, brainfuck <file>, script <file>");
                
                vga_win_puts_colored(&main_win, 2, 11, "Text Editors:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_MAGENTA));
                vga_win_puts(&main_win, 4, 12, "onan             - Built-in text editor");
                vga_win_puts(&main_win, 4, 13, "Features:        Create, edit, save files");
                vga_win_puts(&main_win, 4, 14, "Usage:           onan <filename>");
                
                vga_win_puts_colored(&main_win, 2, 16, "Debugging Tools:", 
                    vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_MAGENTA));
                vga_win_puts(&main_win, 4, 17, "radifetch        - System information display");
                vga_win_puts(&main_win, 4, 18, "mempop/mpop      - Memory operations");
                vga_win_puts(&main_win, 4, 19, "cat              - View file contents");
                vga_win_puts(&main_win, 4, 20, "ls               - List files");
                
                vga_win_draw_line_h(&main_win, 2, 21, 68, 0xC4);
                vga_win_puts(&main_win, 2, 22, "All tools accessible from terminal");
                vga_win_puts_centered(&main_win, 23, "Press [M] Main | [C] Commands");
                
                vga_win_refresh(&main_win);
                break;
            } 
            case PAGE_SECRET: {
                main_win = vga_create_centered_window(72, 24, VGA_COLOR_WHITE, VGA_COLOR_MAGENTA);
                vga_win_set_title(&main_win, "Secret Menu");
                vga_win_puts_colored(&main_win, 2, 2, "ADVANCED SYSTEM CONTROLS", 
                                    vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_MAGENTA));
                vga_win_draw_line_h(&main_win, 2, 3, 68, 0xC4);
                
                vga_win_puts_colored(&main_win, 2, 5, "Performance Tuning:", 
                                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_MAGENTA));
                vga_win_puts(&main_win, 4, 6,  "overclock        - CPU frequency adjustment");
                vga_win_puts(&main_win, 4, 7,  "turbo            - Enable/disable turbo mode");
                vga_win_puts(&main_win, 4, 8,  "Governor:        performance, powersave, ondemand");
                vga_win_puts(&main_win, 4, 9,  "Usage:           overclock [freq] | turbo [on/off]");
                
                vga_win_puts_colored(&main_win, 2, 11, "System Diagnostics:", 
                                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_MAGENTA));
                vga_win_puts(&main_win, 4, 12, "memtest          - RAM diagnostic and stress test");
                vga_win_puts(&main_win, 4, 13, "cpuburn          - CPU stress testing utility");
                vga_win_puts(&main_win, 4, 14, "sensors          - Hardware temperature monitoring");
                vga_win_puts(&main_win, 4, 15, "benchmark        - System performance benchmark");
                
                vga_win_puts_colored(&main_win, 2, 17, "Low-Level Access:", 
                                    vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_MAGENTA));
                vga_win_puts(&main_win, 4, 18, "ioport           - Direct I/O port read/write");
                vga_win_puts(&main_win, 4, 19, "msr              - Model-Specific Register access");
                vga_win_puts(&main_win, 4, 20, "dmidecode        - BIOS/SMBIOS information dump");
                
                vga_win_draw_line_h(&main_win, 2, 21, 68, 0xC4);
                vga_win_puts_colored(&main_win, 2, 22, "WARNING: Use with caution - may cause instability!", 
                                    vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_MAGENTA));
                vga_win_puts_centered(&main_win, 23, "Press [M] Main | [C] Commands");
                vga_win_refresh(&main_win);
                break;
            }
        }
        
        // Handle keyboard input
        int key = keyboard_key();
        
        // Global keys (work on any page)
        if (key == 0x14) { // T - Terminal
            vga_destroy_window(&main_win);
            running = false;
            terminal_clear();
            //script_run_autoexec();
            enable_interrupts();
            return;
        }
        else if (key == 0x01) { // ESC - Exit
            play_pong();
            
        }
        else if (key == 0x32) { // M - Main page
            vga_destroy_window(&main_win);
            current_page = PAGE_MAIN;
            frame = 0;
        }
        // Page-specific navigation
        else if (key == 0x23) { // H - Help
            vga_destroy_window(&main_win);
            current_page = PAGE_HELP;
        }
        else if (key == 0x31) { // N - Network
            vga_destroy_window(&main_win);
            current_page = PAGE_NETWORK;
        }
        else if (key == 0x1E) { // A - About
            vga_destroy_window(&main_win);
            current_page = PAGE_ABOUT;
        }
        else if (key == 0x2E) { // C - Commands
            vga_destroy_window(&main_win);
            current_page = PAGE_COMMANDS;
        }
        else if (key == 0x17) { // I - System Info
            vga_destroy_window(&main_win);
            current_page = PAGE_SYSTEM_INFO;
        }
        else if (key == 0x58) { // = - System Info
            vga_destroy_window(&main_win);
            current_page = PAGE_SECRET;
        }
        else if (key == 0x21) { // F - File Manager or Filesystem
            vga_destroy_window(&main_win);
            if (current_page == PAGE_SYSTEM_INFO || current_page == PAGE_FILESYSTEM) {
                terminal_clear();
                file_manager();
                printr("\nPress any key to return...\n");
                keyboard_wait_for_key(0);
                terminal_clear();
                current_page = PAGE_MAIN;
            } else {
                current_page = PAGE_FILESYSTEM;
            }
        }
        else if (key == 0x15) { // Y - Casino
            stomp();
        }
        else if (key == 0x20) { // D - Drivers
            vga_destroy_window(&main_win);
            current_page = PAGE_DRIVERS;
        }
        else if (key == 0x2D) { // X - Credits
            vga_destroy_window(&main_win);
            current_page = PAGE_CREDITS;
        }
        else if (key == 0x2F) { // V - Advanced
            vga_destroy_window(&main_win);
            current_page = PAGE_ADVANCED;
        }
        else if (key == 0x22) { // G - Memory
            vga_destroy_window(&main_win);
            current_page = PAGE_MEMORY;
        }
        else if (key == 0x11) { // W - Detailed Networking
            vga_destroy_window(&main_win);
            current_page = PAGE_NETWORKING;
        }
        else if (key == 0x12) { // E - Security
            vga_destroy_window(&main_win);
            current_page = PAGE_SECURITY;
        }
        else if (key == 0x18) { // O - Tools
            vga_destroy_window(&main_win);
            current_page = PAGE_TOOLS;
        }
        else if (key == 0x1F) { // S - Shell Preview
            vga_destroy_window(&main_win);
          //  show_interactive_shell_page(&main_win);
            current_page = PAGE_MAIN;
            frame = 0;
        }
        else if (key == 0x13) { // R - Reboot
            vga_destroy_window(&main_win);
            terminal_clear();
            printr("Reboot system? (Y/N): ");
            int confirm = keyboard_wait_for_key(0);
            if (confirm == 0x15) { // Y
                reboot_command(0, NULL);
            }
            terminal_clear();
            current_page = PAGE_MAIN;
        }
        
        frame++;
        sleep_ms(50); // 20 FPS
    }
}