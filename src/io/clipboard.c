// clipboard.c - see clipboard.h
#include <stdint.h>
#include <stdbool.h>
#include "clipboard.h"
#include "io.h"
#include "../timers/tsc.h"

#define COM1 0x3F8

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void com_put(uint8_t c)
{
    for (int i = 0; i < 200000 && !(inb(COM1 + 5) & 0x20); i++) { }
    outb(COM1, c);
}

static void com_puts(const char *s) { while (*s) com_put((uint8_t)*s++); }

static void com_drain(void)
{
    for (int i = 0; i < 4096 && (inb(COM1 + 5) & 0x01); i++) (void)inb(COM1);
}

void clipboard_to_host(const char *text, size_t len)
{
    if (len > 32768) len = 32768;
    com_puts("\033]52;c;");
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)(uint8_t)text[i] << 16;
        if (i + 1 < len) v |= (uint32_t)(uint8_t)text[i + 1] << 8;
        if (i + 2 < len) v |= (uint8_t)text[i + 2];
        com_put((uint8_t)B64[(v >> 18) & 63]);
        com_put((uint8_t)B64[(v >> 12) & 63]);
        com_put(i + 1 < len ? (uint8_t)B64[(v >> 6) & 63] : '=');
        com_put(i + 2 < len ? (uint8_t)B64[v & 63] : '=');
    }
    com_put(7);
}

static int b64_value(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int clipboard_from_host(char *out, size_t cap)
{
    if (cap < 2) return -1;
    com_drain();
    com_puts("\033]52;c;?\a");

    // Wait for ESC ] 5 2 ; c ; then decode base64 until BEL.
    static const char start[] = "\033]52;c;";
    size_t matched = 0;
    bool body = false;
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;

    uint64_t deadline = tsc_deadline_ms(1500);
    while (!tsc_reached(deadline)) {
        if (!(inb(COM1 + 5) & 0x01)) continue;
        uint8_t c = inb(COM1);
        if (!body) {
            if ((char)c == start[matched]) {
                if (++matched == sizeof(start) - 1) body = true;
            } else {
                matched = ((char)c == start[0]) ? 1 : 0;
            }
            continue;
        }
        if (c == 7) { out[n] = '\0'; return (int)n; }
        int v = b64_value(c);
        if (v < 0) continue;   // padding
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n + 1 < cap) out[n++] = (char)((acc >> bits) & 0xFF);
        }
    }
    return -1;
}
