// shsyntax.c - Linux-style shell syntax on top of the one-command runner in keyboard.c
//
//   cmd1; cmd2   cmd1 && cmd2   cmd1 || cmd2   ! cmd   cmd | cmd   cmd > f   cmd >> f   cmd < f   cmd >/dev/null
//   NAME=value   "double"  'single'  \escape   $VAR  ${VAR}  ${VAR:-default}  $?  $RANDOM  $(cmd)  `cmd`  $((1+2))
//   *.txt  ?  {a,b}  {1..5}   # comment (at the start of a command)
//   if c; then ..; elif c; then ..; else ..; fi      for x in a b *.txt; do ..; done
//   while c; do ..; done     until c; do ..; done    case $x in a|b) ..;; *) ..;; esac     { a; b; }
//   break [n]   continue [n]   read [-p text] VAR   source FILE   (Esc or Ctrl+C stops a running loop)
//
// The text is scanned in place: compound commands are found by matching their keywords, and loop bodies are
// simply run again from the same text.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "shsyntax.h"
#include "../keyboard/keyboard.h"
#include "../commands/shellvars.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../vga/fbcon.h"
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../io/io.h"

#define SH_MAX_DEPTH   12
#define SUBST_MAX      4096
#define LOOP_ITEMS     512
#define LOOP_BUF       8192
#define GLOB_ENTRIES   256
#define GLOB_MATCHES   128

static int  g_depth;           // nesting of run_list
static int  g_brk, g_cont;     // pending break / continue levels
static int  g_loops;           // loops currently running
static bool g_abort;           // Esc / Ctrl+C pressed during a loop
static bool g_ctrl_down;

static void sh_error(const char *msg)
{
    terminal_setcolor(VGA_COLOR_LIGHT_RED);
    printr("\n! %s !\n", msg);
    terminal_setcolor(VGA_COLOR_WHITE);
    shell_set_status(2);
}

static bool is_blank(char c)      { return c == ' ' || c == '\t' || c == '\r'; }
static bool is_name_start(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static bool is_name_char(char c)  { return is_name_start(c) || (c >= '0' && c <= '9'); }

// ---- Esc / Ctrl+C -------------------------------------------------------------------------------------------

static bool sh_poll_abort(void)
{
    if (g_abort) return true;
    fbcon_poll();
    for (int n = 0; n < 16 && (inb(0x64) & 0x21) == 0x01; n++) {
        uint8_t sc = inb(0x60);
        if (sc == 0x1D) g_ctrl_down = true;
        else if (sc == 0x9D) g_ctrl_down = false;
        else if (sc == 0x01 || (g_ctrl_down && sc == 0x2E)) {
            g_abort = true;
            print("^C\n");
        }
    }
    return g_abort;
}

// ---- scanning -----------------------------------------------------------------------------------------------

static const char *skip_paren(const char *p, const char *end);

static const char *skip_sq(const char *p, const char *end)       // p is after the opening '
{
    while (p < end && *p != '\'') p++;
    return p < end ? p + 1 : NULL;
}

static const char *skip_dq(const char *p, const char *end)       // p is after the opening "
{
    while (p < end) {
        if (*p == '\\' && p + 1 < end) { p += 2; continue; }
        if (*p == '"') return p + 1;
        if (*p == '$' && p + 1 < end && p[1] == '(') {
            const char *q = skip_paren(p + 1, end);
            if (q) { p = q; continue; }
        }
        p++;
    }
    return NULL;
}

static const char *skip_paren(const char *p, const char *end)    // p is at '('; returns after the matching ')'
{
    int depth = 0;
    for (; p < end; p++) {
        char c = *p;
        if (c == '(') depth++;
        else if (c == ')') { if (--depth == 0) return p + 1; }
        else if (c == '\'') { const char *q = skip_sq(p + 1, end); if (q) p = q - 1; }
        else if (c == '"')  { const char *q = skip_dq(p + 1, end); if (q) p = q - 1; }
        else if (c == '\\' && p + 1 < end) p++;
    }
    return NULL;
}

static const char *skip_brace(const char *p, const char *end)    // p is at '{' of ${; returns after '}'
{
    for (; p < end; p++) if (*p == '}') return p + 1;
    return NULL;
}

// End of the word starting at p. Quotes, escapes and $(...) keep their contents inside the word.
static const char *scan_word_end(const char *p, const char *end)
{
    while (p < end) {
        char c = *p;
        if (is_blank(c) || c == '\n' || c == ';' || c == '|') break;
        if (c == '&' && p + 1 < end && p[1] == '&') break;
        if (c == '\'') { const char *q = skip_sq(p + 1, end); p = q ? q : p + 1; continue; }
        if (c == '"')  { const char *q = skip_dq(p + 1, end); p = q ? q : p + 1; continue; }
        if (c == '\\' && p + 1 < end) { p += 2; continue; }
        if (c == '$' && p + 1 < end && p[1] == '(') { const char *q = skip_paren(p + 1, end); if (q) { p = q; continue; } }
        if (c == '$' && p + 1 < end && p[1] == '{') { const char *q = skip_brace(p + 1, end); if (q) { p = q; continue; } }
        if (c == '`') {
            const char *q = p + 1;
            while (q < end && *q != '`') q++;
            if (q < end) { p = q + 1; continue; }
        }
        p++;
    }
    return p;
}

typedef enum { T_END, T_NL, T_SEMI, T_DSEMI, T_AND, T_OR, T_PIPE, T_WORD } tkind_t;
typedef struct { tkind_t kind; const char *s, *e; } token_t;

static const char *skip_blanks(const char *p, const char *end)
{
    while (p < end) {
        if (is_blank(*p)) p++;
        else if (*p == '\\' && p + 1 < end && p[1] == '\n') p += 2;
        else break;
    }
    return p;
}

// A '#' at the start of a word begins a comment that runs to the end of the line.
static void next_token(const char **pp, const char *end, bool cmdpos, token_t *t)
{
    (void)cmdpos;
    const char *p = *pp;
    for (;;) {
        p = skip_blanks(p, end);
        if (p < end && *p == '#') { while (p < end && *p != '\n') p++; continue; }
        break;
    }
    t->s = p;
    if (p >= end) { t->kind = T_END; t->e = p; *pp = p; return; }

    char c = *p;
    if (c == '\n')      { t->kind = T_NL; p++; }
    else if (c == ';')  { if (p + 1 < end && p[1] == ';') { t->kind = T_DSEMI; p += 2; } else { t->kind = T_SEMI; p++; } }
    else if (c == '&' && p + 1 < end && p[1] == '&') { t->kind = T_AND; p += 2; }
    else if (c == '|')  { if (p + 1 < end && p[1] == '|') { t->kind = T_OR; p += 2; } else { t->kind = T_PIPE; p++; } }
    else {
        const char *q = scan_word_end(p, end);
        if (q == p) q = p + 1;
        t->kind = T_WORD;
        p = q;
    }
    t->e = p;
    *pp = p;
}

static bool tok_is(const token_t *t, const char *w)
{
    size_t n = strlen(w);
    return (size_t)(t->e - t->s) == n && memcmp(t->s, w, n) == 0;
}

enum { KW_NONE, KW_IF, KW_THEN, KW_ELIF, KW_ELSE, KW_FI, KW_WHILE, KW_UNTIL, KW_FOR, KW_DO, KW_DONE,
       KW_CASE, KW_ESAC, KW_BANG, KW_LBRACE, KW_RBRACE };

static const struct { const char *word; int kw; } KEYWORDS[] = {
    { "if", KW_IF }, { "then", KW_THEN }, { "elif", KW_ELIF }, { "else", KW_ELSE }, { "fi", KW_FI },
    { "while", KW_WHILE }, { "until", KW_UNTIL }, { "for", KW_FOR }, { "do", KW_DO }, { "done", KW_DONE },
    { "case", KW_CASE }, { "esac", KW_ESAC }, { "!", KW_BANG }, { "{", KW_LBRACE }, { "}", KW_RBRACE },
};

static int keyword_of(const token_t *t)
{
    size_t n = (size_t)(t->e - t->s);
    if (n == 0 || n > 5) return KW_NONE;
    for (size_t i = 0; i < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); i++)
        if (strlen(KEYWORDS[i].word) == n && memcmp(t->s, KEYWORDS[i].word, n) == 0) return KEYWORDS[i].kw;
    return KW_NONE;
}

// A case pattern like "a|b)" ends the word with ')': what follows is a command.
static bool ends_pattern(const token_t *t)
{
    if (t->e == t->s || t->e[-1] != ')') return false;
    for (const char *p = t->s; p < t->e; p++) if (*p == '(') return false;
    return true;
}

enum { M_THEN = 1, M_ELIF = 2, M_ELSE = 4, M_FI = 8, M_DO = 16, M_DONE = 32, M_ESAC = 64, M_IN = 128,
       M_DSEMI = 256, M_RBRACE = 512 };

typedef struct { int which; const char *start, *after; } found_t;   // which == 0: not found / mismatched

// Scans from p for the first keyword in `mask` at nesting depth 0.
static found_t scan_block(const char *p, const char *end, int mask, bool cmdpos)
{
    found_t f = { 0, end, end };
    int depth = 0;
    token_t t;
    for (;;) {
        next_token(&p, end, cmdpos, &t);
        if (t.kind == T_END) return f;
        if (t.kind != T_WORD) {
            if (t.kind == T_DSEMI && depth == 0 && (mask & M_DSEMI)) {
                f.which = M_DSEMI; f.start = t.s; f.after = t.e;
                return f;
            }
            cmdpos = true;
            continue;
        }
        if ((mask & M_IN) && depth == 0 && tok_is(&t, "in")) {
            f.which = M_IN; f.start = t.s; f.after = t.e;
            return f;
        }

        int kw = cmdpos ? keyword_of(&t) : KW_NONE;
        int bit = 0;
        bool closer = false;
        switch (kw) {
        case KW_IF: case KW_WHILE: case KW_UNTIL: case KW_LBRACE: depth++; cmdpos = true; continue;
        case KW_FOR: case KW_CASE: depth++; cmdpos = false; continue;
        case KW_BANG: continue;
        case KW_FI:     bit = M_FI;     closer = true; break;
        case KW_DONE:   bit = M_DONE;   closer = true; break;
        case KW_ESAC:   bit = M_ESAC;   closer = true; break;
        case KW_RBRACE: bit = M_RBRACE; closer = true; break;
        case KW_THEN:   bit = M_THEN; break;
        case KW_ELIF:   bit = M_ELIF; break;
        case KW_ELSE:   bit = M_ELSE; break;
        case KW_DO:     bit = M_DO;   break;
        default: cmdpos = ends_pattern(&t); continue;
        }
        if (depth == 0) {
            if (mask & bit) { f.which = bit; f.start = t.s; f.after = t.e; return f; }
            if (closer) return f;
            cmdpos = true;
            continue;
        }
        if (closer) depth--;
        cmdpos = !closer;
    }
}

// End of the command starting at p (a compound command, or a simple command up to ; && || or a newline).
// NULL for a syntax error.
static const char *item_end(const char *p, const char *end)
{
    token_t t;
    const char *q = p;
    for (;;) {
        next_token(&q, end, true, &t);
        if (t.kind == T_WORD && keyword_of(&t) == KW_BANG) continue;
        break;
    }
    if (t.kind != T_WORD) return NULL;

    int kw = keyword_of(&t);
    int mask = 0;
    switch (kw) {
    case KW_IF:    mask = M_FI; break;
    case KW_WHILE: case KW_UNTIL: case KW_FOR: mask = M_DONE; break;
    case KW_CASE:  mask = M_ESAC; break;
    case KW_LBRACE: mask = M_RBRACE; break;
    case KW_NONE: break;
    default: return NULL;                       // stray then / fi / done / ...
    }
    if (mask) {
        found_t f = scan_block(q, end, mask, !(kw == KW_FOR || kw == KW_CASE));
        return f.which ? f.after : NULL;
    }

    if (tok_is(&t, "helios")) {                 // inline Helios source may contain ; && | and braces
        const char *r = q;
        token_t t2;
        next_token(&r, end, false, &t2);
        if (t2.kind == T_WORD && tok_is(&t2, "eval")) {
            while (r < end && *r != '\n') r++;
            return r;
        }
    }

    for (;;) {
        const char *b = skip_blanks(q, end);
        if (b < end && *b == '#') return b;           // the comment is not part of the command
        next_token(&q, end, false, &t);
        if (t.kind == T_END) return end;
        if (t.kind == T_NL || t.kind == T_SEMI || t.kind == T_DSEMI || t.kind == T_AND || t.kind == T_OR) return t.s;
    }
}

static const char *skip_seps(const char *p, const char *end)
{
    while (p < end) {
        if (is_blank(*p) || *p == '\n' || *p == ';') p++;
        else if (*p == '#') { while (p < end && *p != '\n') p++; }
        else break;
    }
    return p;
}

// ---- expansion ----------------------------------------------------------------------------------------------

typedef struct {
    char *buf; size_t cap, len;
    char **argv; unsigned char *kind; int max, argc;
    size_t wstart;
    bool in_word, plain, special, brace, nosplit, overflow;
} xs_t;

static int  run_list(const char *p, const char *end);
static int  arith_eval(const char *text, bool *bad);

static void xs_put(xs_t *x, char c)
{
    if (x->len + 1 >= x->cap) { x->overflow = true; return; }
    x->buf[x->len++] = c;
}

static void xs_begin(xs_t *x)
{
    if (x->in_word) return;
    x->in_word = true;
    x->wstart = x->len;
    x->plain = true;
    x->special = x->brace = false;
}

static void xs_char(xs_t *x, char c)          // unquoted text
{
    xs_begin(x);
    xs_put(x, c);
    if (c == '*' || c == '?') x->special = true;
    if (c == '{') x->brace = true;
}

static void xs_quoted(xs_t *x, char c)        // quoted or escaped text: never a wildcard
{
    xs_begin(x);
    x->plain = false;
    xs_put(x, c);
}

static void xs_add(xs_t *x, const char *s, unsigned char kind)
{
    size_t n = strlen(s);
    if (x->argc >= x->max || x->len + n + 1 >= x->cap) { x->overflow = true; return; }
    x->argv[x->argc] = x->buf + x->len;
    x->kind[x->argc] = kind;
    memcpy(x->buf + x->len, s, n + 1);
    x->len += n + 1;
    x->argc++;
}

static bool glob_match(const char *p, const char *s, bool icase)
{
    while (*p) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return true;
            for (;; s++) {
                if (glob_match(p, s, icase)) return true;
                if (!*s) return false;
            }
        }
        if (!*s) return false;
        if (*p != '?') {
            char a = *p, b = *s;
            if (icase) { a = tolower(a); b = tolower(b); }
            if (a != b) return false;
        }
        p++; s++;
    }
    return *s == '\0';
}

// Adds the files matching a pattern like dir/*.txt (sorted). Returns how many were added.
static int glob_add(xs_t *x, const char *pat)
{
    const char *slash = strrchr(pat, '/');
    size_t plen = slash ? (size_t)(slash - pat) + 1 : 0;
    const char *base = pat + plen;
    char dir[128];
    if (plen >= sizeof(dir)) return 0;
    for (size_t i = 0; i < plen; i++) if (pat[i] == '*' || pat[i] == '?') return 0;
    if (slash) {
        if (plen == 1) strcpy(dir, "/");
        else { memcpy(dir, pat, plen - 1); dir[plen - 1] = '\0'; }
    } else {
        strcpy(dir, ".");
    }

    DirectoryEntry *list = malloc(sizeof(DirectoryEntry) * GLOB_ENTRIES);
    if (!list) return 0;
    int n = avfs_list_directory(dir, list, GLOB_ENTRIES);
    int idx[GLOB_MATCHES], count = 0;
    for (int i = 0; i < n && count < GLOB_MATCHES; i++) {
        const char *name = list[i].name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (name[0] == '.' && base[0] != '.') continue;
        if (strcmp(name, "store.bin") == 0 || strcmp(name, "STORE.BIN") == 0) continue;   // the raw fetch store
        if (glob_match(base, name, true)) idx[count++] = i;
    }
    for (int i = 1; i < count; i++) {
        int key = idx[i], j = i - 1;
        while (j >= 0 && strcmp(list[idx[j]].name, list[key].name) > 0) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = key;
    }
    for (int i = 0; i < count; i++) {
        char path[256];
        size_t nl = strlen(list[idx[i]].name);
        if (plen + nl + 1 > sizeof(path)) continue;
        memcpy(path, pat, plen);
        memcpy(path + plen, list[idx[i]].name, nl + 1);
        xs_add(x, path, 0);
    }
    free(list);
    return count;
}

static void expand_one(xs_t *x, const char *w, int depth);

static bool brace_has_alts(const char *lb, const char *rb)
{
    for (const char *p = lb + 1; p < rb; p++) {
        if (*p == ',') return true;
        if (*p == '.' && p + 1 < rb && p[1] == '.') return true;
    }
    return false;
}

static void brace_variant(xs_t *x, const char *w, const char *lb, const char *rb, const char *alt, size_t alt_len, int depth)
{
    char out[512];
    size_t pre = (size_t)(lb - w), post = strlen(rb + 1);
    if (pre + alt_len + post + 1 > sizeof(out)) return;
    memcpy(out, w, pre);
    memcpy(out + pre, alt, alt_len);
    memcpy(out + pre + alt_len, rb + 1, post + 1);
    expand_one(x, out, depth + 1);
}

static bool parse_range_int(const char *s, size_t n, int *v)
{
    size_t i = 0;
    bool neg = false;
    if (i < n && s[i] == '-') { neg = true; i++; }
    if (i >= n) return false;
    int r = 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        r = r * 10 + (s[i] - '0');
    }
    *v = neg ? -r : r;
    return true;
}

static void brace_emit(xs_t *x, const char *w, const char *lb, const char *rb, int depth)
{
    const char *body = lb + 1;
    size_t blen = (size_t)(rb - body);
    bool comma = false;
    for (size_t i = 0; i < blen; i++) if (body[i] == ',') comma = true;

    if (comma) {
        const char *seg = body;
        for (const char *p = body; p <= rb; p++) {
            if (p == rb || *p == ',') {
                brace_variant(x, w, lb, rb, seg, (size_t)(p - seg), depth);
                seg = p + 1;
            }
        }
        return;
    }

    size_t dots = 0;
    while (dots + 1 < blen && !(body[dots] == '.' && body[dots + 1] == '.')) dots++;
    int a, b;
    char numbuf[16];
    if (parse_range_int(body, dots, &a) && parse_range_int(body + dots + 2, blen - dots - 2, &b)) {
        int step = a <= b ? 1 : -1;
        for (int v = a, guard = 0; guard < 1000; v += step, guard++) {
            bool neg = v < 0;
            unsigned u = neg ? 0u - (unsigned)v : (unsigned)v;
            int n = 0;
            char tmp[12];
            do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
            int o = 0;
            if (neg) numbuf[o++] = '-';
            while (n) numbuf[o++] = tmp[--n];
            numbuf[o] = '\0';
            brace_variant(x, w, lb, rb, numbuf, (size_t)o, depth);
            if (v == b) break;
        }
    } else if (dots == 1 && blen == 4) {                   // {a..e}
        char c1 = body[0], c2 = body[3];
        int step = c1 <= c2 ? 1 : -1;
        for (int c = c1;; c += step) {
            char one = (char)c;
            brace_variant(x, w, lb, rb, &one, 1, depth);
            if (c == c2) break;
        }
    } else {
        xs_add(x, w, 0);
    }
}

// One finished word that contains a wildcard or brace group: expand it into the argument list.
static void expand_one(xs_t *x, const char *w, int depth)
{
    if (depth < 3) {
        const char *lb = strchr(w, '{');
        while (lb) {
            const char *rb = strchr(lb + 1, '}');
            if (!rb) break;
            if (brace_has_alts(lb, rb)) { brace_emit(x, w, lb, rb, depth); return; }
            lb = strchr(lb + 1, '{');
        }
    }
    if ((strchr(w, '*') || strchr(w, '?')) && glob_add(x, w) > 0) return;
    xs_add(x, w, 0);
}

static void xs_finish(xs_t *x)
{
    if (!x->in_word) return;
    x->in_word = false;
    if (x->len + 1 >= x->cap) { x->overflow = true; return; }
    x->buf[x->len++] = '\0';
    char *w = x->buf + x->wstart;

    if (x->special || x->brace) {
        size_t n = x->len - 1 - x->wstart;
        if (n < 512) {
            char tmp[512];
            memcpy(tmp, w, n + 1);
            x->len = x->wstart;
            expand_one(x, tmp, 0);
            return;
        }
    }
    if (x->argc >= x->max) { x->overflow = true; return; }
    x->argv[x->argc] = w;
    x->kind[x->argc] = 0;
    x->argc++;
}

// Adds a variable's value or a command's output: quoted stays one piece, unquoted splits at blanks.
static void xs_value(xs_t *x, const char *val, bool quoted)
{
    if (quoted || x->nosplit) {
        xs_begin(x);
        x->plain = false;
        for (; *val; val++) xs_put(x, *val);
        return;
    }
    for (; *val; val++) {
        if (*val == ' ' || *val == '\t' || *val == '\n' || *val == '\r') { xs_finish(x); continue; }
        xs_begin(x);
        x->plain = false;
        xs_put(x, *val);
    }
}

typedef struct { char *buf; size_t cap, len; } capbuf_t;

static void cap_sink(char c, void *ctx)
{
    capbuf_t *b = ctx;
    if (b->len + 1 < b->cap) b->buf[b->len++] = c;
}

static void xs_subst(xs_t *x, const char *cmd, size_t n, bool quoted)
{
    char *out = malloc(SUBST_MAX);
    char *src = malloc(n + 1);
    if (!out || !src) { free(out); free(src); x->overflow = true; return; }
    memcpy(src, cmd, n);
    src[n] = '\0';

    capbuf_t b = { out, SUBST_MAX, 0 };
    bool captured = terminal_begin_capture_sink(cap_sink, &b) == 0;
    run_list(src, src + n);
    if (captured) terminal_end_capture();

    while (b.len > 0 && (out[b.len - 1] == '\n' || out[b.len - 1] == '\r')) b.len--;
    out[b.len] = '\0';
    const char *shown = out;
    while (*shown == '\n' || *shown == '\r') shown++;     // many commands start their output with a blank line
    xs_value(x, shown, quoted);
    free(out);
    free(src);
}

static void fmt_int(int v, char *out)
{
    char tmp[12];
    int n = 0, o = 0;
    unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) out[o++] = '-';
    while (n) out[o++] = tmp[--n];
    out[o] = '\0';
}

static const char *lookup_var(const char *name)
{
    static char num[16];
    if (strcmp(name, "RANDOM") == 0) { fmt_int(returnRandomInteger() & 0x7FFF, num); return num; }
    return shell_getenv(name);
}

static const char *xs_dollar(xs_t *x, const char *p, const char *end, bool quoted)
{
    char n = (p + 1 < end) ? p[1] : '\0';

    if (n == '(') {
        const char *e = skip_paren(p + 1, end);
        if (e) {
            if (p + 2 < end && p[2] == '(' && e - p >= 6 && e[-2] == ')') {            // $(( math ))
                char inner[256], ex[256];
                size_t len = (size_t)((e - 2) - (p + 3));
                if (len < sizeof(inner)) {
                    memcpy(inner, p + 3, len);
                    inner[len] = '\0';
                    if (sh_expand_string(inner, ex, sizeof(ex)) == 0) {
                        bool bad = false;
                        char num[16];
                        fmt_int(arith_eval(ex, &bad), num);
                        if (bad) sh_error("bad arithmetic expression");
                        xs_value(x, num, quoted);
                    }
                }
            } else {
                xs_subst(x, p + 2, (size_t)((e - 1) - (p + 2)), quoted);
            }
            return e;
        }
    } else if (n == '{') {
        const char *e = skip_brace(p + 1, end);
        if (e) {
            char name[40], word[160];
            const char *q = p + 2, *close = e - 1;
            size_t k = 0;
            while (q < close && is_name_char(*q) && k + 1 < sizeof(name)) name[k++] = *q++;
            name[k] = '\0';
            const char *val = k ? lookup_var(name) : NULL;
            if (q + 1 < close && q[0] == ':' && q[1] == '-') {
                size_t dl = (size_t)(close - (q + 2));
                if (dl >= sizeof(word)) dl = sizeof(word) - 1;
                memcpy(word, q + 2, dl);
                word[dl] = '\0';
                if (!val || !*val) val = word;
            }
            xs_value(x, val ? val : "", quoted);
            return e;
        }
    } else if (n == '?') {
        char num[16];
        fmt_int(shell_last_status(), num);
        xs_value(x, num, quoted);
        return p + 2;
    } else if (is_name_start(n)) {
        char name[40];
        size_t k = 0;
        const char *q = p + 1;
        while (q < end && is_name_char(*q) && k + 1 < sizeof(name)) name[k++] = *q++;
        name[k] = '\0';
        while (q < end && is_name_char(*q)) q++;
        const char *val = lookup_var(name);
        xs_value(x, val ? val : "", quoted);
        return q;
    }

    if (quoted) xs_quoted(x, '$'); else xs_char(x, '$');
    return p + 1;
}

static const char *xs_backtick(xs_t *x, const char *p, const char *end, bool quoted)
{
    const char *q = p + 1;
    while (q < end && *q != '`') q++;
    if (q >= end) { if (quoted) xs_quoted(x, '`'); else xs_char(x, '`'); return p + 1; }
    xs_subst(x, p + 1, (size_t)(q - (p + 1)), quoted);
    return q + 1;
}

static void xs_dquote(xs_t *x, const char *p, const char *to)    // [p, to) is the text between the quotes
{
    xs_begin(x);
    x->plain = false;
    while (p < to && !x->overflow) {
        char c = *p;
        if (c == '\\' && p + 1 < to && strchr("\"\\$`", p[1])) { xs_put(x, p[1]); p += 2; continue; }
        if (c == '$') { p = xs_dollar(x, p, to, true); continue; }
        if (c == '`') { p = xs_backtick(x, p, to, true); continue; }
        xs_put(x, c);
        p++;
    }
}

static const char *xs_redirect(xs_t *x, const char *p)
{
    char op[8];
    int n = 0;
    if (x->in_word && x->plain && x->len - x->wstart == 1 &&
        (x->buf[x->wstart] == '1' || x->buf[x->wstart] == '2' || x->buf[x->wstart] == '&')) {
        op[n++] = x->buf[x->wstart];                      // 2> and &> : the digit or & belongs to the operator
        x->len = x->wstart;
        x->in_word = false;
    } else {
        xs_finish(x);
    }
    char c = *p++;
    op[n++] = c;
    if (c == '>' && *p == '>') op[n++] = *p++;
    if (c == '>' && *p == '&' && p[1] >= '0' && p[1] <= '9') { op[n++] = '&'; op[n++] = p[1]; p += 2; }
    op[n] = '\0';
    xs_add(x, op, 1);
    return p;
}

static void xs_scan(xs_t *x, const char *p)
{
    const char *end = p + strlen(p);
    while (p < end && !x->overflow) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (x->nosplit) xs_char(x, c); else xs_finish(x);
            p++;
            continue;
        }
        if (c == '\'') {
            const char *q = skip_sq(p + 1, end);
            if (q) {
                xs_begin(x);
                x->plain = false;
                for (p++; p < q - 1; p++) xs_put(x, *p);
                p = q;
                continue;
            }
        } else if (c == '"') {
            const char *q = skip_dq(p + 1, end);
            if (q) { xs_dquote(x, p + 1, q - 1); p = q; continue; }
        } else if (c == '\\') {
            char n = p[1];
            if (n == '\n') { p += 2; continue; }
            if (n && strchr(" \t$\"'\\;&|<>()*?#`{}!~", n)) { xs_quoted(x, n); p += 2; continue; }
        } else if (c == '$') {
            p = xs_dollar(x, p, end, false);
            continue;
        } else if (c == '`') {
            p = xs_backtick(x, p, end, false);
            continue;
        } else if ((c == '>' || c == '<') && !x->nosplit) {
            p = xs_redirect(x, p);
            continue;
        }
        xs_char(x, c);
        p++;
    }
}

// Inline Helios source: split at blanks outside quotes, keep everything else exactly as typed.
static void xs_raw(xs_t *x, const char *p)
{
    const char *end = p + strlen(p);
    while (p < end && !x->overflow) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { xs_finish(x); p++; continue; }
        if (c == '"' || c == '\'') {
            const char *q = p + 1;
            while (q < end && *q != c) { if (*q == '\\' && c == '"' && q + 1 < end) q++; q++; }
            if (q < end) {
                xs_begin(x);
                for (; p <= q; p++) xs_put(x, *p);
                continue;
            }
        }
        xs_begin(x);
        xs_put(x, c);
        p++;
    }
}

int sh_split(const char *text, char *buf, size_t cap, char *argv[], unsigned char kind[], int max, bool raw)
{
    xs_t x = { buf, cap, 0, argv, kind, max, 0, 0, false, false, false, false, false, false };
    if (raw) xs_raw(&x, text); else xs_scan(&x, text);
    xs_finish(&x);
    return x.overflow ? -1 : x.argc;
}

int sh_expand_string(const char *text, char *out, size_t cap)
{
    xs_t x = { out, cap, 0, NULL, NULL, 0, 0, 0, false, false, false, false, true, false };
    xs_scan(&x, text);
    if (x.overflow || x.len >= cap) { if (cap) out[0] = '\0'; return -1; }
    out[x.len] = '\0';
    return 0;
}

int sh_split_pipes(char *cmd, char *stages[], int max)
{
    int n = 0;
    char *start = cmd;
    const char *end = cmd + strlen(cmd);
    char *p = cmd;
    while (p < end) {
        const char *q = scan_word_end(p, end);
        if (q > p) { p = (char *)q; continue; }
        if (*p == '|') {
            *p = '\0';
            if (n == max) return -1;
            stages[n++] = start;
            start = p + 1;
        }
        p++;
    }
    if (n == max) return -1;
    stages[n++] = start;
    return n;
}

// ---- arithmetic -----------------------------------------------------------------------------------------------

typedef struct { const char *p; bool err; } ar_t;

static void ar_ws(ar_t *a) { while (*a->p == ' ' || *a->p == '\t') a->p++; }
static int  ar_or(ar_t *a);

static int ar_primary(ar_t *a)
{
    ar_ws(a);
    char c = *a->p;
    if (c == '(') {
        a->p++;
        int v = ar_or(a);
        ar_ws(a);
        if (*a->p == ')') a->p++; else a->err = true;
        return v;
    }
    if (c == '-') { a->p++; return -ar_primary(a); }
    if (c == '+') { a->p++; return ar_primary(a); }
    if (c == '!') { a->p++; return !ar_primary(a); }
    if (c == '~') { a->p++; return ~ar_primary(a); }
    if (c >= '0' && c <= '9') {
        unsigned v = 0;
        if (c == '0' && (a->p[1] == 'x' || a->p[1] == 'X')) {
            a->p += 2;
            for (;; a->p++) {
                char h = *a->p;
                if (h >= '0' && h <= '9') v = v * 16 + (unsigned)(h - '0');
                else if (h >= 'a' && h <= 'f') v = v * 16 + (unsigned)(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') v = v * 16 + (unsigned)(h - 'A' + 10);
                else break;
            }
        } else {
            for (; *a->p >= '0' && *a->p <= '9'; a->p++) v = v * 10 + (unsigned)(*a->p - '0');
        }
        return (int)v;
    }
    if (is_name_start(c)) {
        char name[40];
        size_t k = 0;
        while (is_name_char(*a->p)) { if (k + 1 < sizeof(name)) name[k++] = *a->p; a->p++; }
        name[k] = '\0';
        const char *val = lookup_var(name);
        if (!val) return 0;
        while (*val == ' ') val++;
        bool neg = false;
        if (*val == '-') { neg = true; val++; }
        unsigned v = 0;
        for (; *val >= '0' && *val <= '9'; val++) v = v * 10 + (unsigned)(*val - '0');
        return neg ? -(int)v : (int)v;
    }
    a->err = true;
    return 0;
}

static int ar_mul(ar_t *a)
{
    int v = ar_primary(a);
    for (;;) {
        ar_ws(a);
        char c = *a->p;
        if (c != '*' && c != '/' && c != '%') return v;
        a->p++;
        int r = ar_primary(a);
        if (c == '*') v = (int)((unsigned)v * (unsigned)r);
        else if (r == 0) { a->err = true; return 0; }
        else if (c == '/') v = v / r;
        else v = v % r;
    }
}

static int ar_add(ar_t *a)
{
    int v = ar_mul(a);
    for (;;) {
        ar_ws(a);
        char c = *a->p;
        if (c != '+' && c != '-') return v;
        a->p++;
        int r = ar_mul(a);
        v = c == '+' ? (int)((unsigned)v + (unsigned)r) : (int)((unsigned)v - (unsigned)r);
    }
}

static int ar_rel(ar_t *a)
{
    int v = ar_add(a);
    for (;;) {
        ar_ws(a);
        char c = a->p[0], d = a->p[1];
        if (c == '<' && d == '=') { a->p += 2; v = v <= ar_add(a); }
        else if (c == '>' && d == '=') { a->p += 2; v = v >= ar_add(a); }
        else if (c == '<' && d != '<') { a->p++; v = v < ar_add(a); }
        else if (c == '>' && d != '>') { a->p++; v = v > ar_add(a); }
        else return v;
    }
}

static int ar_eq(ar_t *a)
{
    int v = ar_rel(a);
    for (;;) {
        ar_ws(a);
        if (a->p[0] == '=' && a->p[1] == '=') { a->p += 2; v = v == ar_rel(a); }
        else if (a->p[0] == '!' && a->p[1] == '=') { a->p += 2; v = v != ar_rel(a); }
        else return v;
    }
}

static int ar_and(ar_t *a)
{
    int v = ar_eq(a);
    for (;;) {
        ar_ws(a);
        if (a->p[0] == '&' && a->p[1] == '&') { a->p += 2; int r = ar_eq(a); v = v && r; }
        else return v;
    }
}

static int ar_or(ar_t *a)
{
    int v = ar_and(a);
    for (;;) {
        ar_ws(a);
        if (a->p[0] == '|' && a->p[1] == '|') { a->p += 2; int r = ar_and(a); v = v || r; }
        else return v;
    }
}

static int arith_eval(const char *text, bool *bad)
{
    ar_t a = { text, false };
    int v = ar_or(&a);
    ar_ws(&a);
    if (*a.p != '\0') a.err = true;
    if (a.err) { *bad = true; return 0; }
    return v;
}

// ---- running ------------------------------------------------------------------------------------------------

static int exec_simple(const char *s, const char *e)
{
    while (e > s && (is_blank(e[-1]) || e[-1] == '\n')) e--;
    while (s < e && is_blank(*s)) s++;
    if (s >= e) return 0;

    size_t n = (size_t)(e - s);
    char *buf = malloc(n + 1);
    if (!buf) { sh_error("out of memory"); return 2; }
    memcpy(buf, s, n);
    buf[n] = '\0';

    char *p = buf;
    for (;;) {                                           // NAME=value NAME2=value ...
        while (is_blank(*p)) p++;
        char *ns = p;
        while (is_name_char(*p)) p++;
        if (p == ns || !is_name_start(*ns) || *p != '=') { p = ns; break; }

        char name[40], val[512];
        size_t nl = (size_t)(p - ns);
        if (nl >= sizeof(name)) { sh_error("variable name too long"); free(buf); return 2; }
        memcpy(name, ns, nl);
        name[nl] = '\0';

        char *ve = (char *)scan_word_end(p + 1, buf + n);
        char saved = *ve;
        *ve = '\0';
        int rc = sh_expand_string(p + 1, val, sizeof(val));
        *ve = saved;
        if (rc != 0 || shell_setenv(name, val) != 0) { sh_error("cannot set variable (value too long or too many variables)"); free(buf); return 2; }
        p = ve;
    }
    while (is_blank(*p)) p++;

    int st;
    if (*p == '\0') {
        shell_set_status(0);
        st = 0;
    } else if (strncmp(p, "helios eval", 11) == 0 && (p[11] == ' ' || p[11] == '\t' || p[11] == '\0')) {
        shell_exec_stage(p);
        st = shell_last_status();
    } else {
        shell_exec_pipeline(p);
        st = shell_last_status();
    }
    free(buf);
    return st;
}

static int do_if(const char *r, const char *ie)
{
    for (;;) {
        found_t th = scan_block(r, ie, M_THEN, true);
        if (th.which != M_THEN) { sh_error("syntax error: 'if' without 'then'"); return 2; }
        found_t br = scan_block(th.after, ie, M_ELIF | M_ELSE | M_FI, true);
        if (!br.which) { sh_error("syntax error: missing 'fi'"); return 2; }

        int cs = run_list(r, th.start);
        if (g_abort || g_brk || g_cont) return cs;
        if (cs == 0) return run_list(th.after, br.start);
        if (br.which == M_ELIF) { r = br.after; continue; }
        if (br.which == M_ELSE) {
            found_t fi = scan_block(br.after, ie, M_FI, true);
            if (!fi.which) { sh_error("syntax error: missing 'fi'"); return 2; }
            return run_list(br.after, fi.start);
        }
        shell_set_status(0);
        return 0;
    }
}

// After a loop body: true when the loop has to stop.
static bool loop_stop(void)
{
    if (g_abort) return true;
    if (g_brk) { g_brk--; return true; }
    if (g_cont) { g_cont--; return g_cont > 0; }
    return false;
}

static int do_while(const char *r, const char *ie, bool until)
{
    found_t d = scan_block(r, ie, M_DO, true);
    if (d.which != M_DO) { sh_error("syntax error: loop without 'do'"); return 2; }
    found_t e = scan_block(d.after, ie, M_DONE, true);
    if (!e.which) { sh_error("syntax error: missing 'done'"); return 2; }

    int st = 0;
    g_loops++;
    for (;;) {
        if (sh_poll_abort()) break;
        int cs = run_list(r, d.start);
        if (g_abort || g_brk || g_cont) { loop_stop(); break; }
        if ((cs == 0) == until) break;
        st = run_list(d.after, e.start);
        if (loop_stop()) break;
    }
    g_loops--;
    return st;
}

static bool valid_name(const token_t *t)
{
    if (t->kind != T_WORD || t->e == t->s || !is_name_start(*t->s)) return false;
    for (const char *p = t->s; p < t->e; p++) if (!is_name_char(*p)) return false;
    return true;
}

static int do_for(const char *r, const char *ie)
{
    token_t t, t2;
    const char *q = r;
    next_token(&q, ie, false, &t);
    if (!valid_name(&t) || (size_t)(t.e - t.s) >= 40) { sh_error("syntax error: bad variable after 'for'"); return 2; }
    char name[40];
    memcpy(name, t.s, (size_t)(t.e - t.s));
    name[t.e - t.s] = '\0';

    const char *q2 = q;
    next_token(&q2, ie, false, &t2);
    const char *list_s = NULL, *list_e = NULL;
    found_t d;
    if (t2.kind == T_WORD && tok_is(&t2, "in")) {
        d = scan_block(q2, ie, M_DO, false);
        list_s = q2;
        list_e = d.start;
    } else {
        d = scan_block(q, ie, M_DO, false);
    }
    if (d.which != M_DO) { sh_error("syntax error: 'for' without 'do'"); return 2; }
    found_t e = scan_block(d.after, ie, M_DONE, true);
    if (!e.which) { sh_error("syntax error: missing 'done'"); return 2; }

    char *wbuf = malloc(LOOP_BUF);
    char **av = malloc(sizeof(char *) * LOOP_ITEMS);
    unsigned char *kind = malloc(LOOP_ITEMS);
    char *text = NULL;
    int argc = 0, st = 0;
    if (!wbuf || !av || !kind) { sh_error("out of memory"); st = 2; goto out; }

    if (list_s) {
        while (list_e > list_s && (is_blank(list_e[-1]) || list_e[-1] == '\n' || list_e[-1] == ';')) list_e--;
        size_t n = (size_t)(list_e - list_s);
        text = malloc(n + 1);
        if (!text) { sh_error("out of memory"); st = 2; goto out; }
        memcpy(text, list_s, n);
        text[n] = '\0';
        argc = sh_split(text, wbuf, LOOP_BUF, av, kind, LOOP_ITEMS, false);
        if (argc < 0) { sh_error("for: too many items"); st = 2; goto out; }
    }

    g_loops++;
    for (int i = 0; i < argc; i++) {
        if (sh_poll_abort()) break;
        if (shell_setenv(name, av[i]) != 0) { sh_error("for: cannot set the loop variable"); st = 2; break; }
        st = run_list(d.after, e.start);
        if (loop_stop()) break;
    }
    g_loops--;
out:
    free(text);
    free(kind);
    free(av);
    free(wbuf);
    return st;
}

static const char *find_rparen(const char *p, const char *end)
{
    while (p < end) {
        if (*p == '\'') { const char *q = skip_sq(p + 1, end); p = q ? q : p + 1; continue; }
        if (*p == '"')  { const char *q = skip_dq(p + 1, end); p = q ? q : p + 1; continue; }
        if (*p == '\\' && p + 1 < end) { p += 2; continue; }
        if (*p == ')') return p;
        p++;
    }
    return NULL;
}

static bool case_matches(const char *ps, const char *pe, const char *subject)
{
    const char *alt = ps;
    for (const char *p = ps; p <= pe; p++) {
        if (p < pe && *p == '\'') { const char *q = skip_sq(p + 1, pe); if (q) { p = q - 1; } continue; }
        if (p < pe && *p == '"')  { const char *q = skip_dq(p + 1, pe); if (q) { p = q - 1; } continue; }
        if (p == pe || *p == '|') {
            const char *a = alt, *b = p;
            while (a < b && is_blank(*a)) a++;
            while (b > a && is_blank(b[-1])) b--;
            char tmp[256], pat[256];
            size_t n = (size_t)(b - a);
            if (n < sizeof(tmp)) {
                memcpy(tmp, a, n);
                tmp[n] = '\0';
                if (sh_expand_string(tmp, pat, sizeof(pat)) == 0 && glob_match(pat, subject, false)) return true;
            }
            alt = p + 1;
        }
    }
    return false;
}

static int do_case(const char *r, const char *ie)
{
    found_t in = scan_block(r, ie, M_IN, false);
    if (in.which != M_IN) { sh_error("syntax error: 'case' without 'in'"); return 2; }
    found_t es = scan_block(in.after, ie, M_ESAC, true);
    if (!es.which) { sh_error("syntax error: missing 'esac'"); return 2; }

    char word[256], subject[256];
    while (r < in.start && is_blank(*r)) r++;
    size_t wl = (size_t)(in.start - r);
    while (wl > 0 && is_blank(r[wl - 1])) wl--;
    if (wl >= sizeof(word)) { sh_error("case: word too long"); return 2; }
    memcpy(word, r, wl);
    word[wl] = '\0';
    if (sh_expand_string(word, subject, sizeof(subject)) != 0) { sh_error("case: word too long"); return 2; }

    int st = 0;
    bool matched = false;
    const char *q = in.after, *cend = es.start;
    while (q < cend && !matched) {
        q = skip_seps(q, cend);
        if (q >= cend) break;
        if (*q == '(') q++;
        const char *pe = find_rparen(q, cend);
        if (!pe) { sh_error("syntax error in case pattern"); return 2; }
        const char *bs = pe + 1;
        found_t b = scan_block(bs, cend, M_DSEMI, true);
        const char *be = b.which == M_DSEMI ? b.start : cend;
        if (case_matches(q, pe, subject)) { st = run_list(bs, be); matched = true; }
        q = b.which == M_DSEMI ? b.after : cend;
    }
    if (!matched) shell_set_status(0);
    return st;
}

static int run_item(const char *p, const char *ie)
{
    bool neg = false;
    token_t t;
    const char *q = p;
    for (;;) {
        next_token(&q, ie, true, &t);
        if (t.kind == T_WORD && keyword_of(&t) == KW_BANG) { neg = !neg; p = q; continue; }
        break;
    }

    int kw = t.kind == T_WORD ? keyword_of(&t) : KW_NONE;
    int st;
    switch (kw) {
    case KW_IF:     st = do_if(q, ie); break;
    case KW_WHILE:  st = do_while(q, ie, false); break;
    case KW_UNTIL:  st = do_while(q, ie, true); break;
    case KW_FOR:    st = do_for(q, ie); break;
    case KW_CASE:   st = do_case(q, ie); break;
    case KW_LBRACE: {
        found_t f = scan_block(q, ie, M_RBRACE, true);
        st = f.which ? run_list(q, f.start) : 2;
        break;
    }
    default:        st = exec_simple(p, ie); break;
    }
    if (neg) { st = st == 0 ? 1 : 0; shell_set_status(st); }
    return st;
}

static int run_list(const char *p, const char *end)
{
    int st = 0;
    if (g_depth >= SH_MAX_DEPTH) { sh_error("shell commands nested too deeply"); return 2; }
    g_depth++;

    while (p < end) {
        p = skip_seps(p, end);
        if (p >= end) break;

        const char *ie = item_end(p, end);
        if (!ie || ie <= p) { sh_error("syntax error"); st = 2; break; }
        st = run_item(p, ie);
        p = ie;

        for (;;) {
            if (g_brk || g_cont || g_abort) goto out;
            token_t t;
            const char *q = p;
            next_token(&q, end, false, &t);
            if (t.kind != T_AND && t.kind != T_OR) break;
            p = q;
            while (p < end && (is_blank(*p) || *p == '\n')) p++;
            const char *je = item_end(p, end);
            if (!je || je <= p) { sh_error("syntax error"); st = 2; goto out; }
            if (t.kind == T_AND ? st == 0 : st != 0) st = run_item(p, je);
            p = je;
        }
    }
out:
    g_depth--;
    return st;
}

void sh_execute(const char *text)
{
    bool top = g_depth == 0;
    if (top) { g_brk = g_cont = 0; g_abort = false; g_ctrl_down = false; }
    run_list(text, text + strlen(text));
    if (top) g_brk = g_cont = 0;
}

// ---- commands ---------------------------------------------------------------------------------------------

static void loop_ctl(int argc, char *argv[], int *flag)
{
    int n = argc > 1 ? atoi(argv[1]) : 1;
    if (n < 1) n = 1;
    if (g_loops == 0) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        printr("%s: only meaningful inside a for, while or until loop\n", argv[0]);
        terminal_setcolor(VGA_COLOR_WHITE);
        shell_set_status(1);
        return;
    }
    *flag = n > g_loops ? g_loops : n;
}

void break_command(int argc, char *argv[])    { loop_ctl(argc, argv, &g_brk); }
void continue_command(int argc, char *argv[]) { loop_ctl(argc, argv, &g_cont); }

// read [-p prompt] [VAR...]: one line from the keyboard; the last variable gets the rest of the line.
void read_command(int argc, char *argv[])
{
    int first = 1;
    if (argc >= 3 && strcmp(argv[1], "-p") == 0) { print(argv[2]); first = 3; }

    char *line = malloc(COMMAND_BUFFER_SIZE);
    if (!line) { shell_set_status(1); return; }
    line[0] = '\0';
    if (keyboard_input(line) != 0) { free(line); shell_set_status(1); return; }

    if (first >= argc) {
        shell_setenv("REPLY", line);
    } else {
        char *p = line;
        for (int i = first; i < argc; i++) {
            while (*p == ' ' || *p == '\t') p++;
            char *w = p;
            if (i < argc - 1) { while (*p && *p != ' ' && *p != '\t') p++; if (*p) *p++ = '\0'; }
            shell_setenv(argv[i], w);
        }
    }
    free(line);
    shell_set_status(0);
}

// source FILE: runs the commands in FILE (including if/for/while blocks) in the current shell.
void source_command(int argc, char *argv[])
{
    if (argc != 2) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        print("Usage: source FILE\n");
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    int size = avfs_get_filesize(argv[1]);
    if (size < 0 || avfs_is_directory(argv[1])) {
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        printr("source: %s: no such file\n", argv[1]);
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    char *text = malloc((size_t)size + 1);
    if (!text) return;
    text[0] = '\0';
    if (size > 0 && avfs_get_content(argv[1], text, (size_t)size + 1) != 0) {
        free(text);
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        printr("source: cannot read %s\n", argv[1]);
        terminal_setcolor(VGA_COLOR_WHITE);
        return;
    }
    text[size] = '\0';
    sh_execute(text);
    free(text);
}

// Plain literal first word of a stage (no expansion needed), quotes removed. False when it has to be expanded.
bool sh_plain_word(const char *stage, char *out, size_t cap)
{
    while (*stage == ' ' || *stage == '\t') stage++;
    size_t n = 0;
    for (; *stage && *stage != ' ' && *stage != '\t'; stage++) {
        if (strchr("$`*?{}\\()<>;", *stage)) return false;
        if (*stage == '"' || *stage == '\'') continue;
        if (n + 1 >= cap) return false;
        out[n++] = *stage;
    }
    out[n] = '\0';
    return n > 0;
}

// Ctrl+C threw a command away: forget the loop and nesting state it left behind.
void sh_abort_reset(void)
{
    g_depth = 0;
    g_brk = g_cont = 0;
    g_abort = false;
    g_ctrl_down = false;
}
