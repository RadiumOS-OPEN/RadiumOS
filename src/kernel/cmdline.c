// cmdline.c - see cmdline.h
#include "cmdline.h"

static char g_cmdline[160];

void cmdline_set(const char *text)
{
    size_t n = 0;
    while (text && text[n] && n + 1 < sizeof(g_cmdline)) { g_cmdline[n] = text[n]; n++; }
    g_cmdline[n] = '\0';
}

const char *cmdline_get(void) { return g_cmdline; }

// Next word starting at *p; returns its length (0 at the end) and leaves *p after it.
static size_t next_word(const char **p, const char **start)
{
    while (**p == ' ' || **p == '\t') (*p)++;
    *start = *p;
    while (**p && **p != ' ' && **p != '\t') (*p)++;
    return (size_t)(*p - *start);
}

bool cmdline_has(const char *word)
{
    size_t want = 0;
    while (word[want]) want++;

    const char *p = g_cmdline, *w;
    size_t n;
    while ((n = next_word(&p, &w)) > 0) {
        if (n != want) continue;
        size_t i = 0;
        while (i < n && w[i] == word[i]) i++;
        if (i == n) return true;
    }
    return false;
}

bool cmdline_value(const char *key, char *out, size_t size)
{
    size_t klen = 0;
    while (key[klen]) klen++;

    const char *p = g_cmdline, *w;
    size_t n;
    while ((n = next_word(&p, &w)) > 0) {
        if (n <= klen || w[klen] != '=') continue;
        size_t i = 0;
        while (i < klen && w[i] == key[i]) i++;
        if (i != klen) continue;

        size_t len = n - klen - 1;
        if (len + 1 > size) len = size - 1;
        for (size_t k = 0; k < len; k++) out[k] = w[klen + 1 + k];
        out[len] = '\0';
        return true;
    }
    return false;
}
