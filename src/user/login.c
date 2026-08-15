// modern_login.c - Username/Password Login System for RadiumOS
// Features: Username + Password authentication with Tab switching

#include "../terminal/terminal.h"
#include "../keyboard/keyboard.h"
#include "../Avfs/Avfs.h"
#include "../timers/timer.h"
#include "../utility/utility.h"
#include "../memory/memory.h"
#include "../vga/vga.h"
#include <stdint.h>
#include <stdbool.h>

// Login configuration
#define MAX_USERNAME_LENGTH 32
#define MAX_PASSWORD_LENGTH 32
#define MAX_LOGIN_ATTEMPTS 3
#define LOCK_DURATION_MS 5000
#define DEFAULT_USERNAME "root"
#define DEFAULT_PASSWORD "radium"

// Visual effects
#define CURSOR_BLINK_RATE_MS 500

// Color scheme
#define LOGIN_BG_COLOR VGA_COLOR_BLACK
#define LOGIN_PRIMARY_COLOR VGA_COLOR_CYAN
#define LOGIN_SECONDARY_COLOR VGA_COLOR_LIGHT_CYAN
#define LOGIN_TEXT_COLOR VGA_COLOR_WHITE
#define LOGIN_MUTED_COLOR VGA_COLOR_LIGHT_GREY
#define LOGIN_ERROR_COLOR VGA_COLOR_LIGHT_RED
#define LOGIN_SUCCESS_COLOR VGA_COLOR_LIGHT_GREEN
#define LOGIN_HIGHLIGHT_COLOR VGA_COLOR_LIGHT_BLUE

// Field selection
typedef enum {
    FIELD_USERNAME = 0,
    FIELD_PASSWORD = 1
} login_field_t;

// Login state
typedef struct {
    char username[MAX_USERNAME_LENGTH + 1];
    char password[MAX_PASSWORD_LENGTH + 1];
    int username_len;
    int password_len;
    login_field_t active_field;
    int attempts;
    bool locked;
    uint32_t lock_time;
} login_state_t;

static uint32_t login_timer_ms = 0;

static uint32_t get_current_time_ms(void) {
    return login_timer_ms;
}

static void update_login_timer(uint32_t delta_ms) {
    login_timer_ms += delta_ms;
}

// Verify credentials from files
bool verify_credentials(const char* username, const char* password) {
    char stored_username[MAX_USERNAME_LENGTH + 1] = {0};
    char stored_password[MAX_PASSWORD_LENGTH + 1] = {0};
    bool use_defaults = false;
    
    // Try to read username from .username file
    if (avfs_file_exists("/etc/.username.cfg")) {
        int file_size = avfs_get_filesize("/etc/.username.cfg");
        if (file_size > 0 && file_size <= MAX_USERNAME_LENGTH) {
            if (avfs_read_file("/etc/.username.cfg", stored_username, file_size, 0) == 0) {
                stored_username[file_size] = '\0';
                
                // Clean the string - remove non-printable characters
                int write_pos = 0;
                for (int read_pos = 0; read_pos < file_size; read_pos++) {
                    char c = stored_username[read_pos];
                    if (c >= 32 && c <= 126) { // printable ASCII only
                        stored_username[write_pos++] = c;
                    }
                }
                stored_username[write_pos] = '\0';
            }
        }
    }
    
    // Use default if file doesn't exist or is empty
    if (stored_username[0] == '\0') {
        strcpy(stored_username, DEFAULT_USERNAME);
        use_defaults = false;
    }
    
    // Try to read password from .password file
    if (avfs_file_exists("/etc/.password.cfg")) {
        int file_size = avfs_get_filesize("/etc/.password.cfg");
        if (file_size > 0 && file_size <= MAX_PASSWORD_LENGTH) {
            if (avfs_read_file("/etc/.password.cfg", stored_password, file_size, 0) == 0) {
                stored_password[file_size] = '\0';
                
                // Clean the string - remove non-printable characters
                int write_pos = 0;
                for (int read_pos = 0; read_pos < file_size; read_pos++) {
                    char c = stored_password[read_pos];
                    if (c >= 32 && c <= 126) { // printable ASCII only
                        stored_password[write_pos++] = c;
                    }
                }
                stored_password[write_pos] = '\0';
            }
        }
    }
    
    // Use default if file doesn't exist or is empty
    if (stored_password[0] == '\0') {
        strcpy(stored_password, DEFAULT_PASSWORD);
    }
    
    // Compare credentials
    bool username_match = (strcmp(stored_username, username) == 0);
    bool password_match = (strcmp(stored_password, password) == 0);
    
    return username_match && password_match;
}

void draw_light_box(vga_window_t* win, int x, int y, int width, int height, uint8_t color) {
    vga_win_putc_colored(win, x, y, 0xDA, color);
    vga_win_putc_colored(win, x + width - 1, y, 0xBF, color);
    vga_win_putc_colored(win, x, y + height - 1, 0xC0, color);
    vga_win_putc_colored(win, x + width - 1, y + height - 1, 0xD9, color);
    
    for (int i = 1; i < width - 1; i++) {
        vga_win_putc_colored(win, x + i, y, 0xC4, color);
        vga_win_putc_colored(win, x + i, y + height - 1, 0xC4, color);
    }
    
    for (int i = 1; i < height - 1; i++) {
        vga_win_putc_colored(win, x, y + i, 0xB3, color);
        vga_win_putc_colored(win, x + width - 1, y + i, 0xB3, color);
    }
}

void draw_logo(vga_window_t* win, int start_y) {
    uint8_t color = vga_entry_color(LOGIN_PRIMARY_COLOR, LOGIN_BG_COLOR);
    
    vga_win_puts_colored(win, 13, start_y + 0, "  ____            _ _                 ___  ____  ", color);
    vga_win_puts_colored(win, 13, start_y + 1, " |  _ \\ __ _  __| (_)_   _ _ __ ___ / _ \\/ ___| ", color);
    vga_win_puts_colored(win, 13, start_y + 2, " | |_) / _` |/ _` | | | | | '_ ` _ \\ | | \\___ \\ ", color);
    vga_win_puts_colored(win, 13, start_y + 3, " |  _ < (_| | (_| | | |_| | | | | | | |_| |___) |", color);
    vga_win_puts_colored(win, 13, start_y + 4, " |_| \\_\\__,_|\\__,_|_|\\__,_|_| |_| |_|\\___/|____/ ", color);
}

void draw_progress_bar(vga_window_t* win, int x, int y, int width, int percent, uint8_t color) {
    int filled = (width * percent) / 100;
    
    vga_win_putc_colored(win, x, y, '[', color);
    for (int i = 0; i < width; i++) {
        if (i < filled) {
            vga_win_putc_colored(win, x + 1 + i, y, 0xDB, color);
        } else {
            vga_win_putc_colored(win, x + 1 + i, y, 0xB0, color);
        }
    }
    vga_win_putc_colored(win, x + width + 1, y, ']', color);
}

void show_loading_animation(vga_window_t* win, int y, const char* message) {
    uint8_t color = vga_entry_color(LOGIN_SECONDARY_COLOR, LOGIN_BG_COLOR);
    
    for (int i = 0; i <= 100; i += 10) {
        vga_win_puts_colored(win, 10, y, message, color);
        draw_progress_bar(win, 15, y + 1, 40, i, color);
        
        char percent[8];
        itoa(i, percent, 10);
        strcat(percent, "%");
        vga_win_puts_colored(win, 58, y + 1, percent, color);
        
        vga_win_refresh(win);
        sleep_ms(50);
    }
}

void render_input_field(vga_window_t* win, int x, int y, int width, 
                        const char* label, const char* value, int value_len,
                        bool is_password, bool is_active, bool show_cursor) {
    uint8_t label_color = vga_entry_color(LOGIN_MUTED_COLOR, LOGIN_BG_COLOR);
    uint8_t border_color = is_active ? 
        vga_entry_color(LOGIN_HIGHLIGHT_COLOR, LOGIN_BG_COLOR) :
        vga_entry_color(LOGIN_PRIMARY_COLOR, LOGIN_BG_COLOR);
    uint8_t text_color = vga_entry_color(LOGIN_TEXT_COLOR, LOGIN_BG_COLOR);
    
    // Draw label
    vga_win_puts_colored(win, x, y, label, label_color);
    
    // Draw box
    draw_light_box(win, x, y + 1, width, 3, border_color);
    
    // Clear field interior
    vga_win_fill_rect(win, x + 1, y + 2, width - 2, 1, ' ', 
                      vga_entry_color(LOGIN_TEXT_COLOR, LOGIN_BG_COLOR));
    
    // Draw content (masked if password)
    for (int i = 0; i < value_len && i < width - 4; i++) {
        char display_char = is_password ? '*' : value[i];
        vga_win_putc_colored(win, x + 2 + i, y + 2, display_char, text_color);
    }
    
    // Draw cursor if active
    if (is_active && show_cursor) {
        int cursor_pos = value_len < width - 4 ? value_len : width - 5;
        vga_win_putc_colored(win, x + 2 + cursor_pos, y + 2, '_', 
                           vga_entry_color(LOGIN_HIGHLIGHT_COLOR, LOGIN_BG_COLOR));
    }
    
    // Draw active indicator
    if (is_active) {
        vga_win_puts_colored(win, x + width + 2, y + 2, "<", 
                           vga_entry_color(LOGIN_HIGHLIGHT_COLOR, LOGIN_BG_COLOR));
    }
}

void show_message(vga_window_t* win, int y, const char* message, bool is_error) {
    uint8_t color = is_error ? 
        vga_entry_color(LOGIN_ERROR_COLOR, LOGIN_BG_COLOR) :
        vga_entry_color(LOGIN_SUCCESS_COLOR, LOGIN_BG_COLOR);
    
    int msg_len = strlen(message);
    int x = (win->width - msg_len) / 2;
    
    vga_win_fill_rect(win, 2, y, win->width - 4, 1, ' ', 
                      vga_entry_color(LOGIN_TEXT_COLOR, LOGIN_BG_COLOR));
    
    if (is_error) {
        vga_win_puts_colored(win, x - 2, y, "[!]", color);
    } else {
        vga_win_puts_colored(win, x - 2, y, "[+]", color);
    }
    vga_win_puts_colored(win, x + 2, y, message, color);
}

bool modern_login_screen(void) {
    vga_window_t login_win = vga_create_centered_window(70, 24, 
                                                         LOGIN_TEXT_COLOR, 
                                                         LOGIN_BG_COLOR);
    vga_win_set_title(&login_win, "RadiumOS Authentication");
    
    login_state_t state = {0};
    state.active_field = FIELD_USERNAME;
    
    uint32_t last_cursor_blink = 0;
    bool cursor_visible = true;
    bool need_redraw = true;
    
    login_timer_ms = 0;
    
    while (1) {
        uint32_t current_time = get_current_time_ms();
        update_login_timer(10);
        
        // Handle lock timeout
        if (state.locked) {
            if (current_time - state.lock_time >= LOCK_DURATION_MS) {
                state.locked = false;
                state.attempts = 0;
                need_redraw = true;
            }
        }
        
        // Handle cursor blink
        if (current_time - last_cursor_blink >= CURSOR_BLINK_RATE_MS) {
            cursor_visible = !cursor_visible;
            last_cursor_blink = current_time;
            need_redraw = true;
        }
        
        // Redraw screen
        if (need_redraw) {
            vga_win_clear(&login_win);
            
            // Draw logo
            draw_logo(&login_win, 2);
            
            vga_win_puts_colored(&login_win, 18, 7, "User Authentication System", 
                               vga_entry_color(LOGIN_MUTED_COLOR, LOGIN_BG_COLOR));
            
            // Draw separator line
            for (int i = 5; i < 65; i++) {
                vga_win_putc_colored(&login_win, i, 8, 0xC4, 
                                   vga_entry_color(LOGIN_PRIMARY_COLOR, LOGIN_BG_COLOR));
            }
            
            if (state.locked) {
                vga_win_puts_colored(&login_win, 23, 12, "ACCESS LOCKED", 
                                   vga_entry_color(LOGIN_ERROR_COLOR, LOGIN_BG_COLOR));
                vga_win_puts_colored(&login_win, 10, 14, "Too many failed attempts. Please wait...", 
                                   vga_entry_color(LOGIN_MUTED_COLOR, LOGIN_BG_COLOR));
                
                uint32_t remaining = (LOCK_DURATION_MS - (current_time - state.lock_time)) / 1000;
                char time_str[32];
                itoa(remaining, time_str, 10);
                strcat(time_str, " seconds remaining");
                
                int x = (login_win.width - strlen(time_str)) / 2;
                vga_win_puts_colored(&login_win, x, 16, time_str, 
                                   vga_entry_color(LOGIN_ERROR_COLOR, LOGIN_BG_COLOR));
                
                vga_win_refresh(&login_win);
                sleep_ms(100);
                continue;
            }
            
            // Draw username field
            render_input_field(&login_win, 15, 10, 40, 
                             "Username:", 
                             state.username, state.username_len,
                             false, // not password
                             state.active_field == FIELD_USERNAME,
                             cursor_visible);
            
            // Draw password field
            render_input_field(&login_win, 15, 15, 40, 
                             "Password:", 
                             state.password, state.password_len,
                             true, // is password
                             state.active_field == FIELD_PASSWORD,
                             cursor_visible);
            
            // Instructions
            vga_win_puts_colored(&login_win, 16, 20, "TAB: Switch field  |  ENTER: Login", 
                               vga_entry_color(LOGIN_MUTED_COLOR, LOGIN_BG_COLOR));
            
            // Attempt counter
            char attempt_str[32];
            itoa(state.attempts, attempt_str, 10);
            strcat(attempt_str, " / ");
            char max_str[8];
            itoa(MAX_LOGIN_ATTEMPTS, max_str, 10);
            strcat(attempt_str, max_str);
            strcat(attempt_str, " attempts");
            
            vga_win_puts_colored(&login_win, 2, 23, attempt_str, 
                               state.attempts >= 2 ? 
                               vga_entry_color(LOGIN_ERROR_COLOR, LOGIN_BG_COLOR) :
                               vga_entry_color(LOGIN_MUTED_COLOR, LOGIN_BG_COLOR));
            
            vga_win_puts_colored(&login_win, 55, 23, "RadiumOS v1.0", 
                               vga_entry_color(LOGIN_MUTED_COLOR, LOGIN_BG_COLOR));
            
            vga_win_refresh(&login_win);
            need_redraw = false;
        }
        
        // Handle input
        int key = keyboard_key();
        
        if (key == 0) {
            sleep_ms(10);
            continue;
        }
        
        // Tab key - switch fields
        if (key == 0x0F) {
            state.active_field = (state.active_field == FIELD_USERNAME) ? 
                                 FIELD_PASSWORD : FIELD_USERNAME;
            need_redraw = true;
            continue;
        }
        
        // Enter key - attempt login
        if (key == 0x1C) {
            if (state.username_len == 0) {
                show_message(&login_win, 21, "Please enter a username", true);
                vga_win_refresh(&login_win);
                sleep_ms(2000);
                need_redraw = true;
                continue;
            }
            
            if (state.password_len == 0) {
                show_message(&login_win, 21, "Please enter a password", true);
                vga_win_refresh(&login_win);
                sleep_ms(2000);
                need_redraw = true;
                continue;
            }
            
            show_message(&login_win, 21, "Verifying credentials...", false);
            vga_win_refresh(&login_win);
            sleep_ms(500);
            
            if (verify_credentials(state.username, state.password)) {
                show_message(&login_win, 21, "Authentication successful!", false);
                vga_win_refresh(&login_win);
                sleep_ms(1000);
                
                vga_win_clear(&login_win);
                show_loading_animation(&login_win, 11, "Loading system...");
                sleep_ms(500);
                
                vga_destroy_window(&login_win);
                return true;
            } else {
                state.attempts++;
                
                if (state.attempts >= MAX_LOGIN_ATTEMPTS) {
                    state.locked = true;
                    state.lock_time = current_time;
                    show_message(&login_win, 21, "Access denied! Too many failed attempts.", true);
                } else {
                    show_message(&login_win, 21, "Invalid username or password", true);
                }
                
                vga_win_refresh(&login_win);
                sleep_ms(2000);
                
                // Clear fields
                state.username_len = 0;
                state.password_len = 0;
                memset(state.username, 0, MAX_USERNAME_LENGTH + 1);
                memset(state.password, 0, MAX_PASSWORD_LENGTH + 1);
                state.active_field = FIELD_USERNAME;
                need_redraw = true;
            }
            continue;
        }
        
        // Backspace
        if (key == 0x0E) {
            if (state.active_field == FIELD_USERNAME && state.username_len > 0) {
                state.username[--state.username_len] = '\0';
            } else if (state.active_field == FIELD_PASSWORD && state.password_len > 0) {
                state.password[--state.password_len] = '\0';
            }
            need_redraw = true;
            continue;
        }
        
        // Character input
        if (key >= 0x02 && key <= 0x39) {
            extern bool shift_active;
            extern bool caps_lock_active;
            char ch = keyboard_to_char(key, shift_active, caps_lock_active);
            
            if (ch != '\0') {
                if (state.active_field == FIELD_USERNAME && 
                    state.username_len < MAX_USERNAME_LENGTH) {
                    state.username[state.username_len++] = ch;
                    state.username[state.username_len] = '\0';
                    need_redraw = true;
                } else if (state.active_field == FIELD_PASSWORD && 
                           state.password_len < MAX_PASSWORD_LENGTH) {
                    state.password[state.password_len++] = ch;
                    state.password[state.password_len] = '\0';
                    need_redraw = true;
                }
            }
        }
    }
    
    return false;
}

void login(void) {
    terminal_clear();
    
    if (modern_login_screen()) {
        terminal_clear();
        terminal_setcolor(VGA_COLOR_LIGHT_GREEN);
        printr("\nWelcome to RadiumOS!\n\n");
        terminal_setcolor(VGA_COLOR_WHITE);
    } else {
        terminal_clear();
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        printr("\nLogin failed.\n");
        terminal_setcolor(VGA_COLOR_WHITE);
    }
}