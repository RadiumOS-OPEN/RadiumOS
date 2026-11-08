// fsapp.h - toolkit for programs that own the whole terminal grid
//
// The grid is whatever terminal.c reports (160x90 on the 1280x720 console, 80x50 in plain text mode), so these
// programs fill the screen at any size. fs_begin() saves what was on screen and fs_end() puts it back.
#ifndef FSAPP_H
#define FSAPP_H

#include <stdint.h>
#include <stdbool.h>

bool fs_begin(void);
void fs_end(void);

int fs_cols(void);
int fs_rows(void);

void fs_clear(uint8_t attr);
void fs_put(int x, int y, char c, uint8_t attr);
// Returns the next column after the text.
int  fs_text(int x, int y, const char *s, uint8_t attr);
int  fs_text_right(int right_x, int y, const char *s, uint8_t attr);
void fs_fill(int x, int y, int w, int h, char c, uint8_t attr);
// Double-line box with an optional title in its top edge.
void fs_box(int x, int y, int w, int h, uint8_t attr, const char *title);
// Horizontal bar: percent (0-100) of w cells filled.
void fs_bar(int x, int y, int w, int percent, uint8_t fill_attr, uint8_t empty_attr);

// Shows what was drawn (the console redraws changed cells at most every ~12 ms).
void fs_present(void);

// Scancode of the next key press (0 if none). Never blocks; arrows and keypad arrows both give 0x48/0x50/0x4B/0x4D.
uint8_t fs_poll_key(void);

// Waits about `ms` while still returning early on a key press; returns that key (0 if the time ran out).
uint8_t fs_wait(uint32_t ms);

// A key press with its modifiers, for programs that take typed text. ch is the ASCII character (0 for keys that
// do not type one, and with Ctrl held); ext is set for the grey cursor-block keys (arrows, Home, Delete ...).
typedef struct { uint8_t sc; char ch; bool shift, ctrl, alt, ext; } fs_event_t;
bool fs_get_event(fs_event_t *ev);

#define FS_KEY_ESC   0x01
#define FS_KEY_Q     0x10
#define FS_KEY_UP    0x48
#define FS_KEY_DOWN  0x50
#define FS_KEY_LEFT  0x4B
#define FS_KEY_RIGHT 0x4D

// Attribute from colour indexes (0-15).
static inline uint8_t fs_attr(uint8_t fg, uint8_t bg) { return (uint8_t)(fg | (bg << 4)); }

// The full-screen programs; each runs until Esc or Q.
void sysmon_command(int argc, char *argv[]);
void life_command(int argc, char *argv[]);
void matrix_command(int argc, char *argv[]);
void screeninfo_command(int argc, char *argv[]);

#endif // FSAPP_H
