// clipboard.h - the host's clipboard, shared over the serial port (COM1) with OSC 52
//
// `make run` starts tools/serial_bridge.py, which turns these sequences into the host clipboard and back.
// Without it (or without a serial port) pasting simply finds nothing.
#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include <stddef.h>

#define CLIPBOARD_HOST_MAX 3000

// Sets the host clipboard.
void clipboard_to_host(const char *text, size_t len);
// Reads the host clipboard into out (NUL-terminated, at most cap-1 bytes). Returns its length, or -1 if no bridge
// answered within about a second and a half.
int clipboard_from_host(char *out, size_t cap);

#endif // CLIPBOARD_H
