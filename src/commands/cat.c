
#include "../Avfs/Avfs.h"
#include "../utility/utility.h"
#include <stdint.h>

/* 
 * ── Configuration ─────────────────────────────────────────────────────── 
 * We use external kernel variables instead of hardcoded VGA addresses 
 * to support 80x50 (and other) resolutions dynamically.
 */
extern volatile uint16_t* terminal_buffer;
extern size_t terminal_row;
extern size_t terminal_column;
extern size_t terminal_width;
extern size_t terminal_height;
extern void terminal_update_cursor(void);
extern void terminal_scroll(void); 

/* ── Themes ──────────────────────────────────────────────────────────── */
typedef struct {
    uint8_t reset;
    uint8_t keyword;
    uint8_t string;
    uint8_t comment;
    uint8_t var;
    uint8_t number;
    uint8_t punct;
    uint8_t error;
    uint8_t linenum;
} Theme;

/* default: dark VSCode-like (cyan kw, green str, grey comment, magenta var) */
static const Theme THEME_DEFAULT = {
    .reset   = 0x07,
    .keyword = 0x0B,
    .string  = 0x0A,
    .comment = 0x08,
    .var     = 0x0D,
    .number  = 0x0E,
    .punct   = 0x09,
    .error   = 0x0C,
    .linenum = 0x08,
};

/* monokai: yellow kw, green str, grey comment, red var, cyan number */
static const Theme THEME_MONOKAI = {
    .reset   = 0x07,
    .keyword = 0x0E,
    .string  = 0x0A,
    .comment = 0x08,
    .var     = 0x0C,
    .number  = 0x0B,
    .punct   = 0x09,
    .error   = 0x0C,
    .linenum = 0x08,
};

/* solarized: blue kw, green str, dark grey comment, yellow var */
static const Theme THEME_SOLARIZED = {
    .reset   = 0x07,
    .keyword = 0x01,
    .string  = 0x02,
    .comment = 0x08,
    .var     = 0x0E,
    .number  = 0x05,
    .punct   = 0x03,
    .error   = 0x0C,
    .linenum = 0x08,
};

/* plain: everything is COL_RESET */
static const Theme THEME_PLAIN = {
    .reset   = 0x07,
    .keyword = 0x07,
    .string  = 0x07,
    .comment = 0x07,
    .var     = 0x07,
    .number  = 0x07,
    .punct   = 0x07,
    .error   = 0x07,
    .linenum = 0x07,
};

/* ── Keywords ────────────────────────────────────────────────────────── */
static const char* RSH_KEYWORDS[] = {
    "if","else","elif","endif","then","do","done","fi","case","esac",
    "while","endwhile","for","endfor","in","function","endfunction",
    "def","enddef","return","call","break","continue","exit",
    "echo","print","set","export","unset","vars",
    "input","input_secure","read","getkey","getscancode","check_key",
    "read_key_noblock","flush_keyboard","wait_key",
    "win_create","win_create_centered","win_show","win_hide","win_clear",
    "win_refresh","win_set_title","win_move","win_print","win_print_centered",
    "win_draw_box","menu_create","menu_draw","menu_select_next",
    "menu_select_prev","menu_get_selected","button_create","button_draw",
    "progress_create","progress_set","progress_draw",
    "notify","notify_titled","toast","pause","sleep","delay_ms","if_exists",
    "math","inc","dec","strlen","concat","substr","toupper","tolower",
    "contains","trim_var","replace","startswith","endswith",
    "alias","unalias","aliases","functions","which","^include","true","false","null",
    NULL
};

static const char* RASH_KEYWORDS[] = {
    "trait","impl","struct","enum","match","let","mut","const","static","fn",
    "use","mod","crate","pub","unsafe","type","self","super","where",
    "if","else","loop","while","for","break","continue","return","async","await",
    "true","false","Some","None","Ok","Err","Box","Vec","String",
    NULL
};

/* ── Options parsed from argv ────────────────────────────────────────── */
typedef struct {
    const Theme* theme;
    int          no_color;
    int          line_nums;
} CatOpts;

/* ── State ───────────────────────────────────────────────────────────── */
static int file_type = 0;

/* ── Char classifiers ────────────────────────────────────────────────── */
static int is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_hex_char(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int is_punct(char c) {
    return c == '(' || c == ')' || c == '{' || c == '}' ||
           c == '[' || c == ']' || c == ';' || c == ',' ||
           c == ':' || c == '.' || c == '|' || c == '&' ||
           c == '!' || c == '=' || c == '<' || c == '>' ||
           c == '+' || c == '-' || c == '*' || c == '/' ||
           c == '^' || c == '~' || c == '@';
}

/* ── Keyword check ───────────────────────────────────────────────────── */
static int is_keyword(const char* word, int len) {
    if (len == 0 || len > 32 || file_type == 0) return 0;
    char temp[33];
    for (int i = 0; i < len; i++) temp[i] = word[i];
    temp[len] = '\0';
    const char** list = (file_type == 1) ? RSH_KEYWORDS : RASH_KEYWORDS;
    for (int i = 0; list[i] != NULL; i++)
        if (strcmp(temp, list[i]) == 0) return 1;
    return 0;
}

/* ── Extension detection ─────────────────────────────────────────────── */
static void detect_type(const char* path) {
    int len = 0;
    while (path[len]) len++;
    if (len >= 5 && strcmp(&path[len-5], ".rash") == 0) { file_type = 2; return; }
    if (len >= 4 && strcmp(&path[len-4], ".rsh")  == 0) { file_type = 1; return; }
    file_type = 0;
}

/* ── VGA primitives ──────────────────────────────────────────────────── */
/* 
 * New logic to handle writing to dynamic width/height and handling line wrapping 
 * relative to the current cursor position.
 */
static void vga_putc(uint8_t c, uint8_t color) {
    if (terminal_row >= terminal_height) {
        terminal_scroll();
        terminal_row = terminal_height - 1;
    }

    size_t index = terminal_row * terminal_width + terminal_column;
    terminal_buffer[index] = ((uint16_t)color << 8) | c;
    terminal_column++;

    if (terminal_column >= terminal_width) {
        terminal_column = 0;
        terminal_row++;
    }
}

static void vga_put_newline(void) {
    terminal_column = 0;
    terminal_row++;
}

static void vga_put_int(int n, uint8_t color) {
    char tmp[12];
    int i = 0;
    if (n == 0) { vga_putc('0', color); return; }
    while (n > 0) { tmp[i++] = '0' + (n % 10); n /= 10; }
    for (int j = i-1; j >= 0; j--) vga_putc((uint8_t)tmp[j], color);
}

/* ── Theme resolver ──────────────────────────────────────────────────── */
static const Theme* resolve_theme(const char* name) {
    if (!name) return &THEME_DEFAULT;
    if (strcmp(name, "monokai")   == 0) return &THEME_MONOKAI;
    if (strcmp(name, "solarized") == 0) return &THEME_SOLARIZED;
    if (strcmp(name, "plain")     == 0) return &THEME_PLAIN;
    if (strcmp(name, "default")   == 0) return &THEME_DEFAULT;
    return NULL;
}

/* ── Argument parser ─────────────────────────────────────────────────── */
static int strncmp_local(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 1;
        if (a[i] == '\0') return 0;
    }
    return 0;
}

static void print_help(void) {
    printr("Usage: cat [OPTIONS] <file>\n");
    printr("\n");
    printr("Display file contents with optional syntax highlighting.\n");
    printr("\n");
    printr("Options:\n");
    printr("  -h, --help            Show this help message\n");
    printr("  -n, --line-numbers    Show line numbers in gutter\n");
    printr("  --no-color            Disable all coloring\n");
    printr("  --plain               Alias for --no-color\n");
    printr("  --color=<theme>       Set color theme\n");
    printr("\n");
    printr("Themes:\n");
    printr("  default    VSCode-like  (cyan kw, green str, magenta var)\n");
    printr("  monokai    Monokai      (yellow kw, green str, red var)\n");
    printr("  solarized  Solarized    (blue kw, green str, yellow var)\n");
    printr("  plain      No color\n");
    printr("\n");
    printr("Examples:\n");
    printr("  cat file.rsh\n");
    printr("  cat -n file.rash\n");
    printr("  cat --color=monokai file.rsh\n");
    printr("  cat --no-color file.txt\n");
}

static int parse_args(int argc, char* argv[], CatOpts* opts) {
    opts->theme     = &THEME_DEFAULT;
    opts->no_color  = 0;
    opts->line_nums = 0;
    int file_idx = -1;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_help();
            return -2;
        }
        if (strcmp(a, "--no-color") == 0 || strcmp(a, "--plain") == 0) {
            opts->no_color = 1;
            continue;
        }
        if (strcmp(a, "--line-numbers") == 0 || strcmp(a, "-n") == 0) {
            opts->line_nums = 1;
            continue;
        }
        if (strncmp_local(a, "--color=", 8) == 0) {
            const char* theme_name = a + 8;
            const Theme* t = resolve_theme(theme_name);
            if (!t) {
                printr("cat: unknown theme '%s'\n", theme_name);
                printr("Themes: default  monokai  solarized  plain\n");
                return -1;
            }
            opts->theme = t;
            continue;
        }
        if (a[0] == '-') {
            printr("cat: unknown option '%s'\n", a);
            printr("Try 'cat --help' for usage.\n");
            return -1;
        }
        if (file_idx == -1)
            file_idx = i;
    }
    return file_idx;
}

/* ── Core renderer ───────────────────────────────────────────────────── */
static int print_highlighted(const char* buf, int size,
                              const char* path, const CatOpts* opts)
{
    detect_type(path);

    const Theme* T = opts->no_color ? &THEME_PLAIN : opts->theme;

    int i    = 0;
    int line = 1;

    int total_lines = 1;
    for (int k = 0; k < size; k++) if (buf[k] == '\n') total_lines++;

    int gutter = 0;
    if (opts->line_nums) {
        int n = total_lines;
        gutter = 2; 
        while (n > 0) { gutter++; n /= 10; }
    }

    #define EMIT_LINENUM() do { \
        if (opts->line_nums) { \
            int num_w = gutter - 2; \
            int tmp_n = line, digs = 1; \
            while (tmp_n >= 10) { digs++; tmp_n /= 10; } \
            for (int _p = digs; _p < num_w; _p++) \
                vga_putc(' ', T->linenum); \
            vga_put_int(line, T->linenum); \
            vga_putc(' ', T->linenum); \
            vga_putc('|', T->linenum); \
            vga_putc(' ', T->linenum); \
        } \
    } while(0)

    EMIT_LINENUM();

    while (i < size) {
        char c = buf[i];
        
        if (c == '\n') {
            vga_put_newline();
            i++;
            line++;
            EMIT_LINENUM();
            continue;
        }
        
        if (c == '\t') {
            int col = terminal_column;
            int spaces = 4 - (col % 4);
            for (int s = 0; s < spaces; s++)
                vga_putc(' ', T->reset);
            i++;
            continue;
        }

        /* line comment: # (RSH/text) or // (RASH) */
        if (c == '#' ||
            (c == '/' && i+1 < size && buf[i+1] == '/' && file_type == 2))
        {
            while (i < size && buf[i] != '\n')
                vga_putc((uint8_t)buf[i++], T->comment);
            continue;
        }

        if (file_type == 2 && c == '/' && i+1 < size && buf[i+1] == '*') {
            vga_putc('/', T->comment); i++;
            vga_putc('*', T->comment); i++;
            while (i < size) {
                if (buf[i] == '*' && i+1 < size && buf[i+1] == '/') {
                    vga_putc('*', T->comment); i++;
                    vga_putc('/', T->comment); i++;
                    break;
                }
                if (buf[i] == '\n') {
                    vga_put_newline();
                    i++; line++;
                    EMIT_LINENUM();
                } else {
                    vga_putc((uint8_t)buf[i], T->comment);
                    i++;
                }
            }
            continue;
        }

        /* string: " or ' */
        if (c == '"' || c == '\'') {
            char delim = c;
            vga_putc((uint8_t)c, T->string); i++;
            while (i < size && buf[i] != '\n') {
                if (buf[i] == '\\' && i+1 < size) {
                    vga_putc((uint8_t)buf[i],   T->string); i++;
                    vga_putc((uint8_t)buf[i],   T->string); i++;
                } else if (buf[i] == delim) {
                    vga_putc((uint8_t)buf[i], T->string); i++;
                    break;
                } else {
                    vga_putc((uint8_t)buf[i], T->string); i++;
                }
            }
            continue;
        }

        /* variable: $ident or ${...} */
        if (c == '$') {
            vga_putc('$', T->var); i++;
            if (i < size && buf[i] == '{') {
                vga_putc('{', T->var); i++;
                while (i < size && buf[i] != '}' && buf[i] != '\n')
                    vga_putc((uint8_t)buf[i++], T->var);
                if (i < size && buf[i] == '}') {
                    vga_putc('}', T->var); i++;
                }
            } else {
                while (i < size && is_ident_char(buf[i]))
                    vga_putc((uint8_t)buf[i++], T->var);
            }
            continue;
        }

        /* number literal */
        if (is_digit(c) || (c == '0' && i+1 < size && buf[i+1] == 'x')) {
            if (c == '0' && i+1 < size && buf[i+1] == 'x') {
                vga_putc('0', T->number); i++;
                vga_putc('x', T->number); i++;
                while (i < size && (is_hex_char(buf[i]) || buf[i] == '_'))
                    vga_putc((uint8_t)buf[i++], T->number);
            } else {
                while (i < size && (is_digit(buf[i]) || buf[i] == '.' || buf[i] == '_'))
                    vga_putc((uint8_t)buf[i++], T->number);
            }
            continue;
        }

        /* ^include */
        if (c == '^' && file_type == 1) {
            int start = i;
            i++;
            while (i < size && is_ident_char(buf[i])) i++;
            int len = i - start;
            uint8_t color = is_keyword(buf + start, len) ? T->keyword : T->reset;
            for (int j = start; j < i; j++)
                vga_putc((uint8_t)buf[j], color);
            continue;
        }

        /* keyword or plain identifier */
        if (is_ident_char(c)) {
            int start = i;
            while (i < size && is_ident_char(buf[i])) i++;
            int len = i - start;
            uint8_t color = is_keyword(buf + start, len) ? T->keyword : T->reset;
            for (int j = start; j < i; j++)
                vga_putc((uint8_t)buf[j], color);
            continue;
        }

        /* punctuation */
        if (is_punct(c)) {
            vga_putc((uint8_t)c, T->punct);
            i++;
            continue;
        }

        /* everything else */
        vga_putc((uint8_t)c, T->reset);
        i++;
    }
    #undef EMIT_LINENUM

    terminal_update_cursor();
    return 0;
}

/* ── Static buffers ──────────────────────────────────────────────────── */
// Increased size by 1 to safely hold the null terminator for a 1MB file
static char cat_buf[1024 * 1024 + 1]; 

/* Simple memory zeroing helper */
static void memzero(void* ptr, size_t len) {
    uint8_t* p = (uint8_t*)ptr;
    while (len--) *p++ = 0;
}

/* 
 * Path Resolution
 */
static int cat_resolve_path(const char* arg, char* out, int out_sz) {
    if (!arg || !arg[0]) return 0;
    
    /* Basic implementation: Copy path. 
     * If using a VFS with prefixes (root:/, 0:/), handle them here.
     */
    int i = 0;
    while (arg[i] && i < out_sz - 1) {
        out[i] = arg[i];
        i++;
    }
    out[i] = '\0';
    return 1;
}

/* ── Entry point ─────────────────────────────────────────────────────── */
void cat_command(int argc, char* argv[]) {
    // Clear the buffer immediately to ensure no stale data from previous runs
    memzero(cat_buf, sizeof(cat_buf));

    CatOpts opts;
    int file_idx = parse_args(argc, argv, &opts);

    if (file_idx <= 0) {
        if (file_idx == -2) return; /* Help printed, exit cleanly */
        if (file_idx == 0) {
             printr("Usage: cat [--no-color] [--plain] [--color=<theme>] [-n|--line-numbers] <file>\n");
             printr("Themes: default  monokai  solarized  plain\n");
        }
        return;
    }

    char path[512];
    if (!cat_resolve_path(argv[file_idx], path, sizeof(path))) {
        printr("cat: invalid path\n");
        return;
    }

    if (avfs_is_directory(path)) {
        printr("cat: is a directory\n");
        return;
    }

    int filesize = avfs_get_filesize(path);
    if (filesize < 0) {
        printr("cat: file not found\n");
        return;
    }
    if (filesize == 0) {
        printr("(empty)\n");
        return;
    }
    // Check against buffer size (minus 1 to ensure space for null terminator)
    if ((uint32_t)filesize >= sizeof(cat_buf) - 1) {
        printr("cat: file too large (max 1MB)\n");
        return;
    }

    if (avfs_read_file(path, cat_buf, (uint32_t)filesize, 0) != 0) {
        printr("cat: read failed\n");
        return;
    }
    
    // Null terminate the string at the end of the read data
    // This is safe now because we checked filesize < sizeof(cat_buf) - 1
    cat_buf[filesize] = '\0';

    print_highlighted(cat_buf, filesize, path, &opts);
}
