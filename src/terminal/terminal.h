#ifndef TERMINAL_H
#define TERMINAL_H

#include <stddef.h>
#include <stdint.h>
#include "../vga/vga.h"
#include "../utility/utility.h"

// Function prototypes for terminal operations
void terminal_initialize(void);
void terminal_setcolor(uint8_t color);
void terminal_putchar(char c);
void terminal_write(const char* data, size_t size);
void print(const char* data);
int terminal_begin_capture(const char* path);
int terminal_end_capture(void);
void print_decimal(int num);
void print_hex(int num);
void print_octal(int num);
void print_integer(int value);
void terminal_clear(void);
void terminal_clear_inFunction(void);
void print_slow(const char* data, uint32_t delay_time);
void print_uint(unsigned int value);
void print_hex_byte(uint8_t value);
void print_capacity(uint64_t bytes);
void print_uint64(uint64_t value);
void print_qemu(const char* format, ...);
void printr(const char* format, ...);
int snprintf(char* buffer, size_t size, const char* format, ...);
void terminal_set_cursor_position(size_t position);
void terminal_update_cursor(void);
void psf_init(void);
void terminal_putentryat(char c, uint8_t color, size_t x, size_t y);
extern void rust_hello(void);
extern void rust_print_number(int num);
extern int rust_process_command(const char* cmd);
extern void vga_set_80x50(void);
void set_text_color(uint8_t fg);
void set_bg_color(uint8_t bg);
void set_color(uint8_t fg, uint8_t bg);
void reset_text_color(void);
void push_color(void);
void pop_color(void);
#endif // TERMINAL_H
