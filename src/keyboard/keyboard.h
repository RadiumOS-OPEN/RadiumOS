#ifndef KEYBOARD_H
#define KEYBOARD_H
#include <stdbool.h> // For bool type
#include <stddef.h>  // For size_t
#include <stdint.h> 

typedef struct {
    const char *name;
    const char* description;
    void (*execute)(int argc, char *argv[]);
    bool requires_sudo;  // Command must run as sudo
    bool allow_sudo;     // Command can run as sudo
} Command;

// Shift state variables - DECLARE as extern, don't DEFINE here
extern bool shift_active;
extern bool caps_lock_active;

#define MAX_HISTORY 50 // Maximum number of commands to store in history
#define COMMAND_BUFFER_SIZE 2048 
#define MAX_COMMANDS 200 // Maximum number of commands
#define MAX_ARGUMENTS 128 // Maximum number of arguments per command
#define PRIVILEGE_PORT 0x60  // Example port for privilege management
#define SUDO_LEVEL 0x01      // Privilege level for sudo/root
#define USER_LEVEL 0x00      // Normal user privilege level

bool is_key_down(uint8_t scancode);

// Function prototypes
bool is_key_pressed();
void execute_command(const char* command);
void execute_command_no_history(const char *command);
// Runs one pipeline (stages joined by |) or one command. Used by the shell syntax layer (src/shell/shsyntax.c).
void shell_exec_pipeline(char *line);
void shell_exec_stage(char *line);
void shell_show_prompt(void);
void which_command(int argc, char *argv[]);
// File holding the data piped into the running command, or NULL when stdin is not a pipe.
const char *shell_stdin_file(void);
// Exit status of the last command (0 = success). A command can call shell_set_status(1) to report failure.
void shell_set_status(int status);
int  shell_last_status(void);
void keyboard_handler();
void keyboard_pause(bool on);
void keyboard_task();
void keyboard_await(const char* message, bool clear_screen);
// Like keyboard_await, but carries on by itself after timeout_ms (usable before the PIT timer runs).
void keyboard_await_timeout(const char *message, bool clear_screen, uint32_t timeout_ms);
int register_command(const char* name, const char* description, void (*execute)(int, char*[]));
int keyboard_input(char* userinput);
void keyboard_input_secure(char* userinput);
void keyboard_read_input();
uint8_t keyboard_wait_for_key(bool dump_scancode);
uint8_t keyboard_key();
void history_reset(void);
void init_keyboard();
void init_serial_port(uint16_t port);
void toggle_caps_lock();
void set_keyboard_leds(uint8_t leds);
char keyboard_to_char(uint8_t scancode, bool shift, bool caps_lock);
void execute_c(int argc, char *argv[]);
void execute_command_as_sudo_user(const char *command);
void serial_write_string(uint16_t port, const char *str);

void history_init(void);
void history_append_line(const char *str, uint8_t color);
void history_trigger_update(void);

// External variables for commands
extern Command commands[MAX_COMMANDS]; // Array to hold commands
extern size_t command_count; // Number of registered commands

#endif // KEYBOARD_H