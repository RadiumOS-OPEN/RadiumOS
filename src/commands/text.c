#include "../Avfs/Avfs.h"
#include "../utility/utility.h"
#include "../terminal/terminal.h"
#include "../keyboard/keyboard.h"
#include "../Avfs/Avfs.h"
#include "../io/io.h"
#include <stdint.h>
#include <stdbool.h>

/* ═══════════════════════════════════════════════════════════════════════════
   RadiumOS Editor  —  vim motions + nano binds + syntax highlighting
                     + scrollable settings panel + persistent themes
                     + Matrix rain easter egg  (Ctrl+M, Ctrl+T, Ctrl+R)
   Supports: .rsh  .rash  .hls  .html/.htm  plain text
   ═══════════════════════════════════════════════════════════════════════════ */

/* ── Layout ────────────────────────────────────────────────────────────── */
#define TERM_W        80
#define TERM_H        50
#define EDIT_ROWS     (TERM_H - 2)
#define STATUS_ROW    (TERM_H - 2)
#define HINT_ROW      (TERM_H - 1)

/* ── Buffer limits ─────────────────────────────────────────────────────── */
#define MAX_LINES     1024
#define MAX_LINE_LEN  256
#define MAX_PATH      512
#define UNDO_DEPTH    64
#define SEARCH_MAX    64
#define CLIP_LINES    64

/* ── Settings persistence ──────────────────────────────────────────────── */
#define SETTINGS_PATH   "/tmp/editor_settings"
#define SETTINGS_MAGIC  "RSHIDT_SETTINGS_V5\n"

/* ── Scancodes ─────────────────────────────────────────────────────────── */
#define SC_ESC        0x01
#define SC_ENTER      0x1C
#define SC_BACKSPACE  0x0E
#define SC_TAB        0x0F
#define SC_UP         0x48
#define SC_DOWN       0x50
#define SC_LEFT       0x4B
#define SC_RIGHT      0x4D
#define SC_HOME       0x47
#define SC_END        0x4F
#define SC_PGUP       0x49
#define SC_PGDN       0x51
#define SC_DEL        0x53
#define SC_LSHIFT     0x2A
#define SC_RSHIFT     0x36
#define SC_LCTRL      0x1D
#define SC_CAPS       0x3A
#define SC_RELEASE    0x80

/* ── Editor modes ──────────────────────────────────────────────────────── */
typedef enum {
    MODE_NORMAL,
    MODE_INSERT,
    MODE_VISUAL,
    MODE_SEARCH,
    MODE_COMMAND,
    MODE_SETTINGS,
} EdMode;

/* ── Base colour palette ───────────────────────────────────────────────── */
#define COL_RESET       0x07
#define COL_KEYWORD     0x0B
#define COL_STRING      0x0A
#define COL_COMMENT     0x08
#define COL_VAR         0x0D
#define COL_NUMBER      0x0E
#define COL_PUNCT       0x09
#define COL_LINENUM     0x08
#define COL_STATUS_N    0x17
#define COL_STATUS_I    0x27
#define COL_STATUS_V    0x57
#define COL_STATUS_S    0x37
#define COL_STATUS_C    0x47
#define COL_HINT        0x08
#define COL_HINT_KEY    0x0F
#define COL_VISUAL      0x30
#define COL_SEARCH_HL   0x6E
#define COL_MODIFIED    0x0C
#define COL_SAVED       0x0A
#define COL_CURSOR_N    0x70
#define COL_CURSOR_I    0x27

/* ── HTML-specific colours ─────────────────────────────────────────────── */
#define COL_HTML_TAG      0x0C
#define COL_HTML_UNKNOWN  0x0F
#define COL_HTML_BRACKET  0x07
#define COL_HTML_ATTR     0x0E
#define COL_HTML_VALUE    0x0A
#define COL_HTML_ENTITY   0x0D
#define COL_HTML_COMMENT  0x08
#define COL_HTML_DOCTYPE  0x0B
#define COL_HTML_SCRIPT   0x0B
#define COL_HTML_STYLE    0x0D
#define COL_HTML_EQUALS   0x07
#define COL_HTML_CSSKEY   0x0B
#define COL_HTML_CSSVAL   0x0A
#define COL_HTML_CSSPROP  0x0E
#define COL_HTML_JSKW     0x0B
#define COL_HTML_JSNUM    0x0E
#define COL_HTML_JSSTR    0x0A
#define COL_HTML_JSCOMMENT 0x08

/* ── Helios-specific colours ────────────────────────────────────────────── */
#define COL_HLS_TYPE       0x0B
#define COL_HLS_BUILTIN    0x0E
#define COL_HLS_MACRO      0x0C
#define COL_HLS_LIFETIME   0x0D
#define COL_HLS_ATTRIBUTE  0x08

/* ── Syntax file types ─────────────────────────────────────────────────── */
typedef enum { FT_TEXT = 0, FT_RSH = 1, FT_RASH = 2, FT_HTML = 3, FT_HLS = 4 } FileType;

/* ═══════════════════════════════════════════════════════════════════════════
   Enhanced Theme System  (v5 — 26 fields per theme)
   ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    const char *name;
    const char *desc;
    uint8_t  bg;
    uint8_t  status_normal;
    uint8_t  status_insert;
    uint8_t  status_visual;
    uint8_t  status_search;
    uint8_t  status_command;
    uint8_t  cursor_normal;
    uint8_t  cursor_insert;
    uint8_t  hint_bg;
    uint8_t  hint_key;
    uint8_t  visual_sel;
    uint8_t  search_hl;
    uint8_t  linenum;
    uint8_t  keyword;
    uint8_t  string_col;
    uint8_t  comment;
    uint8_t  var_col;
    uint8_t  number_col;
    uint8_t  punct;
    uint8_t  cursor_line_bg;
    uint8_t  gutter_sep;
    uint8_t  status_sep;
    uint8_t  search_prompt;
    uint8_t  msg_col;
    uint8_t  rain_col;
    uint8_t  rain_head;
} EdTheme;

#define NUM_THEMES  18

static const EdTheme THEMES[NUM_THEMES] = {
    { "Radium Dark", "Default dark purple/red",
      0x0, 0x57,0x27,0x17,0x37,0x47, 0x50,0x20, 0x08,0x0F, 0x50,0x6E, 0x08,
      0x0D,0x0A,0x08,0x0B,0x0E,0x09, 0x10, 0x38,0x0F,0x37,0x0E,0x02,0x0A },
    { "Inferno", "Volcanic red and orange",
      0x0, 0x47,0x27,0x17,0x67,0x47, 0x40,0x20, 0x04,0x0C, 0x40,0x4E, 0x04,
      0x0C,0x0E,0x04,0x0C,0x06,0x0E, 0x10, 0x04,0x0C,0x47,0x0C,0x04,0x0C },
    { "Ocean", "Deep blues and teals",
      0x0, 0x17,0x37,0x57,0x27,0x67, 0x10,0x30, 0x01,0x0B, 0x10,0x3E, 0x01,
      0x0B,0x0A,0x01,0x03,0x0E,0x09, 0x10, 0x01,0x0B,0x17,0x0A,0x01,0x03 },
    { "Void", "Pure monochrome stark",
      0x0, 0x78,0x78,0x78,0x78,0x78, 0x70,0x70, 0x08,0x0F, 0x70,0x70, 0x07,
      0x0F,0x07,0x08,0x07,0x0F,0x07, 0x18, 0x07,0x0F,0x78,0x07,0x08,0x0F },
    { "Phosphor", "Classic green CRT",
      0x0, 0x27,0x27,0x27,0x27,0x27, 0x20,0x20, 0x02,0x0A, 0x20,0x2E, 0x02,
      0x0A,0x02,0x02,0x0A,0x0A,0x02, 0x22, 0x02,0x0A,0x27,0x0A,0x02,0x0A },
    { "Dracula", "Gothic purple and pink",
      0x0, 0x57,0x57,0x57,0x37,0x57, 0x50,0x50, 0x05,0x0D, 0x50,0x5E, 0x05,
      0x0D,0x0A,0x05,0x0B,0x0E,0x09, 0x15, 0x05,0x0D,0x57,0x0A,0x02,0x0A },
    { "Matrix", "Digital rain green",
      0x0, 0xA0,0xA0,0xA0,0xA0,0xA0, 0xA0,0xA0, 0x00,0x0A, 0xA0,0x2E, 0x02,
      0x0A,0x0A,0x02,0x0A,0x0A,0x02, 0x10, 0x02,0x0A,0xA0,0x0A,0x0A,0x0F },
    { "Monokai", "High-contrast pop",
      0x0, 0x47,0x27,0x57,0x67,0x47, 0xE0,0xA0, 0x08,0x0E, 0x50,0x3E, 0x08,
      0x0D,0x0A,0x06,0x0E,0x0B,0x09, 0x00, 0x08,0x0E,0x47,0x0A,0x02,0x0A },
    { "Solarized", "Eye-friendly blue base",
      0x1, 0x1E,0x1A,0x1B,0x1D,0x1C, 0xE0,0xA0, 0x18,0x0E, 0x30,0x2E, 0x08,
      0x0E,0x0A,0x06,0x0D,0x0B,0x09, 0x10, 0x18,0x0E,0x1D,0x0A,0x02,0x0A },
    { "Gruvbox", "Warm retro comfort",
      0x0, 0x60,0x20,0xE0,0x40,0x60, 0xE0,0xA0, 0x00,0x0E, 0x60,0x28, 0x08,
      0x0E,0x0A,0x08,0x0B,0x09,0x0C, 0x00, 0x06,0x0E,0x60,0x0A,0x02,0x0A },
    { "Nord", "Arctic bluish-purple",
      0x1, 0x1D,0x1B,0x1A,0x1E,0x1C, 0xD0,0xB0, 0x11,0x0B, 0xD0,0x3E, 0x18,
      0x0D,0x0B,0x18,0x09,0x0E,0x0F, 0x10, 0x18,0x0B,0x1B,0x0B,0x01,0x0B },
    { "Paper", "Light mode / VS style",
      0x7, 0x70,0x70,0x70,0x70,0x70, 0x07,0x07, 0x70,0x0F, 0x70,0x74, 0x07,
      0x04,0x02,0x07,0x01,0x06,0x05, 0x78, 0x78,0x04,0x70,0x04,0x02,0x0A },
    { "TempleOS", "Terry Davis dedication",
      0x0, 0xE0,0xE0,0xE0,0xE0,0xE0, 0xE0,0xE0, 0x00,0x0E, 0xE0,0xE6, 0x07,
      0x0E,0x0B,0x08,0x0B,0x0E,0x09, 0x00, 0x06,0x0E,0xE0,0x0E,0x0E,0x0F },
    { "Postfix", "Terminal email cyan/mag",
      0x0, 0x30,0x50,0x30,0x30,0x50, 0x30,0x50, 0x00,0x0B, 0x30,0x3F, 0x03,
      0x0D,0x0B,0x06,0x0B,0x0F,0x05, 0x10, 0x03,0x0B,0x30,0x0B,0x03,0x0B },
    { "Cyberpunk", "Neon dark pink/blue",
      0x0, 0x50,0x1D,0x5D,0xE0,0x4D, 0xD0,0x10, 0x10,0x0E, 0xD0,0xE6, 0x04,
      0x0D,0x0B,0x05,0x09,0x0E,0x0F, 0x18, 0x04,0x0E,0x5D,0x0D,0x02,0x0F },
    { "Abyss", "Deep-sea bioluminescence",
      0x0, 0x17,0x27,0x37,0x17,0x47, 0x10,0x20, 0x01,0x0B, 0x10,0x1E, 0x01,
      0x03,0x0B,0x01,0x0B,0x0E,0x0B, 0x11, 0x01,0x03,0x17,0x0B,0x03,0x0B },
    { "Ember", "Warm amber embers",
      0x0, 0x60,0x60,0x60,0x60,0x40, 0x60,0x60, 0x00,0x06, 0x60,0x6E, 0x06,
      0x0E,0x06,0x04,0x0E,0x0C,0x04, 0x00, 0x04,0x0E,0x60,0x0E,0x06,0x0E },
    { "Necrotic", "Toxic biohazard green",
      0x0, 0x20,0xA0,0x60,0x20,0x40, 0x20,0xA0, 0x00,0x0A, 0x20,0x2E, 0x02,
      0x0A,0x02,0x08,0x0A,0x0E,0x02, 0x00, 0x02,0x0A,0x20,0x0A,0x0A,0x0F },
};

static int ed_theme_idx = 0;
#define TH(field) (THEMES[ed_theme_idx].field)

/* ── RSH keywords ──────────────────────────────────────────────────────── */
static const char* RSH_KW[] = {
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
    "alias","unalias","aliases","functions","which","true","false","null",
    "const","editable","MAP","endMAP","edit.MAP","save.MAP","close.MAP",
    NULL
};

static const char* RASH_KW[] = {
    "trait","impl","struct","enum","match","let","mut","const","static","fn",
    "use","mod","crate","pub","unsafe","type","self","super","where",
    "if","else","loop","while","for","break","continue","return","async","await",
    "true","false","Some","None","Ok","Err","Box","Vec","String",
    NULL
};

/* ── Helios keywords (C/Rust-like syntax for .hls files) ──────────────── */
static const char* HLS_KW[] = {
    /* control flow */
    "fn","let","return","if","else","for","while","loop","in",
    "break","continue","match","as",
    /* structure / type keywords */
    "struct","enum","new","unsafe","extern","impl","trait",
    "pub","use","mod","crate","self","super",
    "mut","const","static","type","where","async","await",
    /* builtins */
    "print","input","asm","true","false","null","void",
    /* primitive types */
    "int","str","bool","ptr","char",
    "u8","u16","u32","u64","i8","i16","i32","i64",
    "f32","f64","usize","isize",
    /* Helios hardware intrinsics */
    "peek8","peek16","peek32","poke8","poke16","poke32",
    "in8","in16","in32","out8","out16","out32",
    "cli","sti","hlt","rdtsc","rdtscp",
    "rdmsr","wrmsr","rdmsr64","wrmsr64",
    "cpuid","iopl3","sgdt","sidt","fxsave","fxrstor",
    "get_cs","get_ds","get_es","get_fs","get_gs","get_ss",
    "set_fs","set_gs",
    "get_cr0","set_cr0","get_cr3","set_cr3",
    "memcpy","memset","alloc_page","free_page",
    /* Helios runtime / FFI */
    "malloc","free","sleep_ms","get_ticks",
    "avfs_read_file","avfs_write_file","avfs_create_file",
    "avfs_file_exists","avfs_is_directory","avfs_get_filesize",
    "avfs_remove_file","avfs_create_dir",
    "register_command","terminal_putchar","terminal_setcolor",
    "keyboard_input",
    "pic_eoi","pic_mask","pic_unmask","pit_set_hz",
    "port_burst_read","port_burst_write","mem_query",
    /* RXE / VM opcodes */
    "push","pop","dup","swap","over",
    "add","sub","mul","div","mod","neg",
    "band","bor","bxor","bnot","shl","shr",
    "cmp_eq","cmp_ne","cmp_lt","cmp_le","cmp_gt","cmp_ge",
    "and","or","not",
    "jmp","jz","jnz","call","ret","halt",
    "heap_alloc","heap_free","arr_load","arr_store",
    "load_local","store_local","load_reg","store_reg",
    "strlen","strcmp","strcat","strsub","strfind",
    "int_to_str","str_to_int","cast_int","cast_bool",
    NULL
};

/* ── Helios type names for special type coloring ──────────────────────── */
static const char* HLS_TYPES[] = {
    "int","str","bool","ptr","char","void",
    "u8","u16","u32","u64","i8","i16","i32","i64",
    "f32","f64","usize","isize",
    "RxeHeader","Opcode","DiagLevel","Span","Diag","DiagEngine",
    "TokenKind","Token","Lexer","Expr","BinOpKind","UnOpKind",
    "CastTarget","RangeKind","MatchArm","MatchPat","Stmt","StructDef","FnDef",
    "Parser","Box","Vec","String","Option","Result",
    "Some","None","Ok","Err",
    NULL
};

static const char* HTML_TAGS[] = {
    "html","head","body","base","title","meta","link","style","script","noscript",
    "template","slot","shadow",
    "header","footer","main","nav","aside","section","article","address",
    "h1","h2","h3","h4","h5","h6","hgroup",
    "div","span","p","pre","blockquote","figure","figcaption","details","summary",
    "dialog","hr","br","wbr","menu","menuitem",
    "ul","ol","li","dl","dt","dd",
    "table","thead","tbody","tfoot","caption","colgroup","col","tr","th","td",
    "a","abbr","acronym","b","bdi","bdo","big","button","cite","code","data",
    "dfn","em","i","kbd","label","mark","output","q","rp","rt","ruby",
    "s","samp","select","small","strong","sub","sup","textarea","time","tt",
    "u","var","del","ins",
    "img","picture","video","audio","source","track","canvas","svg","math",
    "iframe","embed","object","param","portal",
    "form","input","fieldset","legend","optgroup","option","datalist","meter",
    "progress","search","map","area",
    NULL
};

static const char* HTML_BOOL_ATTRS[] = {
    "async","autofocus","autoplay","checked","controls","default","defer",
    "disabled","formnovalidate","hidden","ismap","loop","multiple","muted",
    "nomodule","novalidate","open","readonly","required","reversed","selected",
    "allowfullscreen","crossorigin",
    NULL
};

static const char* HTML_EVENT_ATTRS[] = {
    "onclick","ondblclick","onmousedown","onmouseup","onmouseover","onmouseout",
    "onmousemove","onkeydown","onkeyup","onkeypress","onchange","oninput",
    "onfocus","onblur","onsubmit","onreset","onload","onunload","onerror",
    "onresize","onscroll","ondragstart","ondragend","ondrop","onpaste","oncopy",
    "oncut","oncontextmenu","onwheel","ontouchstart","ontouchend","ontouchmove",
    NULL
};

static const char* CSS_PROPS[] = {
    "color","background","background-color","background-image","background-size",
    "background-position","background-repeat","border","border-top","border-right",
    "border-bottom","border-left","border-radius","border-color","border-width",
    "border-style","margin","margin-top","margin-right","margin-bottom","margin-left",
    "padding","padding-top","padding-right","padding-bottom","padding-left",
    "width","height","min-width","max-width","min-height","max-height",
    "display","position","top","right","bottom","left","z-index","overflow",
    "float","clear","flex","flex-direction","flex-wrap","justify-content",
    "align-items","align-self","align-content","font","font-family",
    "font-size","font-weight","font-style","line-height","text-align",
    "text-decoration","text-transform","text-shadow","opacity","visibility",
    "cursor","transition","animation","transform","filter","box-shadow",
    "outline","resize","clip-path","object-fit","scroll-behavior","aspect-ratio",
    NULL
};

static const char* JS_KW[] = {
    "var","let","const","function","return","if","else","for","while","do",
    "switch","case","break","continue","default","new","delete","typeof",
    "instanceof","in","of","class","extends","super","this","import","export",
    "from","async","await","try","catch","finally","throw","yield","void",
    "true","false","null","undefined","NaN","Infinity",
    "document","window","console","navigator","location","history","fetch",
    "Promise","Array","Object","String","Number","Boolean","Math","Date","JSON",
    NULL
};

/* ── Undo record ───────────────────────────────────────────────────────── */
typedef struct {
    char  lines[MAX_LINES][MAX_LINE_LEN];
    int   line_count;
    int   cur_row;
    int   cur_col;
} UndoState;

typedef struct {
    char  lines[CLIP_LINES][MAX_LINE_LEN];
    int   count;
} Clipboard;

/* ═══════════════════════════════════════════════════════════════════════════
   Settings panel state
   ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    bool   line_numbers;
    bool   auto_indent;
    bool   tab_spaces;
    int    tab_width;
    bool   show_modified;
    bool   wrap_search;
    bool   syntax_highlight;
    bool   case_search;
    bool   cursor_line_hl;
    bool   show_hints;
    bool   smart_home;
    bool   show_whitespace;
    int    theme_idx;
    bool   relative_numbers;
    bool   show_eol;
    bool   trailing_ws_warn;
    bool   bracket_match;
    bool   word_wrap_indicator;
    int    scroll_off;
    bool   bold_keywords;
    bool   dim_comments;
    bool   show_status_col;
    bool   show_status_pct;
    bool   save_on_exit;
    bool   confirm_delete_line;
    bool   double_space_sentence;
    int    undo_limit;
    bool   highlight_urls;
    bool   show_ruler;
    bool   auto_pairs;
    bool   trim_trailing_save;
    bool   insert_final_newline;
    bool   hard_wrap;
    int    hard_wrap_col;
    bool   show_clock;
} EdSettings;

#define SET_ITEM_LINENUMS         0
#define SET_ITEM_RELNUMS          1
#define SET_ITEM_AUTOINDENT       2
#define SET_ITEM_TABSPACES        3
#define SET_ITEM_TABWIDTH         4
#define SET_ITEM_SHOWMOD          5
#define SET_ITEM_WRAPSEARCH       6
#define SET_ITEM_SYNTAX           7
#define SET_ITEM_CASESEARCH       8
#define SET_ITEM_CURSORHL         9
#define SET_ITEM_HINTBAR          10
#define SET_ITEM_SMARTHOME        11
#define SET_ITEM_SHOWWS           12
#define SET_ITEM_SHOWEOL          13
#define SET_ITEM_TRAILWS          14
#define SET_ITEM_BRACKETMATCH     15
#define SET_ITEM_WRAP80           16
#define SET_ITEM_SCROLLOFF        17
#define SET_ITEM_BOLDKW           18
#define SET_ITEM_DIMCOMMENT       19
#define SET_ITEM_STATUS_COL       20
#define SET_ITEM_STATUS_PCT       21
#define SET_ITEM_SAVE_EXIT        22
#define SET_ITEM_CONFIRM_DD       23
#define SET_ITEM_DBL_SPACE        24
#define SET_ITEM_UNDO_LIMIT       25
#define SET_ITEM_HL_URLS          26
#define SET_ITEM_RULER            27
#define SET_ITEM_AUTO_PAIRS       28
#define SET_ITEM_TRIM_SAVE        29
#define SET_ITEM_FINAL_NL         30
#define SET_ITEM_HARD_WRAP        31
#define SET_ITEM_HARD_WRAP_COL    32
#define SET_ITEM_CLOCK            33
#define SET_ITEM_THEME            34
#define SET_ITEM_SAVE             35
#define SET_ITEM_COUNT            36

#define SET_VIEW_ROWS   18

static int       set_cursor     = 0;
static int       set_scroll     = 0;
static bool      set_theme_open = false;
static int       set_theme_sel  = 0;
static EdSettings ed_settings;

/* ═══════════════════════════════════════════════════════════════════════════
   Matrix Rain Easter Egg State
   ═══════════════════════════════════════════════════════════════════════════ */
static bool      ee_secret_m     = false;
static bool      ee_secret_t     = false;
static bool      ee_secret_r     = false;
static bool      matrix_rain_on  = false;

#define RAIN_COLS  TERM_W
typedef struct {
    int  y;
    int  speed;
    int  timer;
    int  len;
    char chars[TERM_H];
} RainCol;

static RainCol   rain_cols[RAIN_COLS];
static uint32_t  rain_tick    = 0;
static bool      rain_inited  = false;
static uint32_t  rain_rng_state = 0xDEADBEEF;

static uint32_t rain_rand(void) {
    rain_rng_state = rain_rng_state * 1664525u + 1013904223u;
    return rain_rng_state;
}

static const char RAIN_CHARS[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "!@#$%^&*()[]{}|<>?/+-=~`"
    "\xCE\xB5\xC4\xCD\xBA\xC9\xBB\xC8\xBC\xC3\xB4\xC2\xC1\xC5"
    "\xDB\xDC\xDD\xDE\xDF\xB0\xB1\xB2\xFE\xF9\xFA\xFB\xFC\xF8";
#define RAIN_CHAR_COUNT 80

static void rain_init(void) {
    for (int c = 0; c < RAIN_COLS; c++) {
        rain_cols[c].y     = (int)(rain_rand() % TERM_H);
        rain_cols[c].speed = 1 + (int)(rain_rand() % 3);
        rain_cols[c].timer = (int)(rain_rand() % rain_cols[c].speed);
        rain_cols[c].len   = 4 + (int)(rain_rand() % 10);
        for (int r = 0; r < TERM_H; r++)
            rain_cols[c].chars[r] = RAIN_CHARS[rain_rand() % RAIN_CHAR_COUNT];
    }
    rain_inited = true;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Editor state
   ═══════════════════════════════════════════════════════════════════════════ */
static char      ed_lines[MAX_LINES][MAX_LINE_LEN];
static int       ed_line_count;
static int       ed_cur_row;
static int       ed_cur_col;
static int       ed_scroll;
static bool      ed_modified;
static char      ed_path[MAX_PATH];
static FileType  ed_ft;
static EdMode    ed_mode;

static int       ed_vis_row;
static int       ed_vis_col;

static char      ed_search[SEARCH_MAX + 1];
static int       ed_search_len;
static int       ed_search_row;
static int       ed_search_col;

static char      ed_cmd[64];
static int       ed_cmd_len;

static UndoState ed_undo[UNDO_DEPTH];
static int       ed_undo_head;
static int       ed_undo_count;

static Clipboard ed_clip;

static bool      ed_shift;
static bool      ed_ctrl;
static bool      ed_caps;
static bool      ed_g_pending;
static bool      ed_show_nums;
static char      ed_msg[80];
static uint32_t  ed_frame = 0;

/* Block-comment state for HLS/RASH (persists across lines during render) */
static bool      ed_in_block_comment = false;

typedef struct { uint8_t ch; uint8_t attr; } SCell;
static SCell     ed_shadow[TERM_H][TERM_W];
#define VGA_BASE ((volatile uint16_t*)0xB8000)

/* ═══════════════════════════════════════════════════════════════════════════
   PS/2 helpers
   ═══════════════════════════════════════════════════════════════════════════ */
static inline uint8_t ps2_read(void) {
    uint8_t b; __asm__ volatile("inb $0x60,%0":"=a"(b)); return b;
}
static inline int ps2_ready(void) {
    uint8_t s; __asm__ volatile("inb $0x64,%0":"=a"(s)); return s&1;
}
static uint8_t ed_get_sc(void) {
    while(!ps2_ready()){/* spin */} return ps2_read();
}

/* ═══════════════════════════════════════════════════════════════════════════
   Shadow buffer helpers
   ═══════════════════════════════════════════════════════════════════════════ */
static void sh_clear(void) {
    for (int r = 0; r < TERM_H; r++)
        for (int c = 0; c < TERM_W; c++) {
            ed_shadow[r][c].ch   = ' ';
            ed_shadow[r][c].attr = COL_RESET;
        }
}
static void sh_putc(int r, int c, char ch, uint8_t attr) {
    if (r < 0 || r >= TERM_H || c < 0 || c >= TERM_W) return;
    ed_shadow[r][c].ch   = (uint8_t)ch;
    ed_shadow[r][c].attr = attr;
}
static void sh_puts(int r, int *c, const char *s, uint8_t attr) {
    while (*s && *c < TERM_W) sh_putc(r, (*c)++, *s++, attr);
}
static void sh_pad(int r, int *c, int to, uint8_t attr) {
    while (*c < to && *c < TERM_W) sh_putc(r, (*c)++, ' ', attr);
}
static void sh_flush(void) {
    for (int r = 0; r < TERM_H; r++)
        for (int c = 0; c < TERM_W; c++)
            VGA_BASE[r * TERM_W + c] =
                ((uint16_t)ed_shadow[r][c].attr << 8) | ed_shadow[r][c].ch;
}

static void sh_box(int row, int col, int w, int h, uint8_t attr) {
    sh_putc(row,     col,     '+', attr);
    sh_putc(row,     col+w-1, '+', attr);
    sh_putc(row+h-1, col,     '+', attr);
    sh_putc(row+h-1, col+w-1, '+', attr);
    for(int c=col+1; c<col+w-1; c++) {
        sh_putc(row,     c, '-', attr);
        sh_putc(row+h-1, c, '-', attr);
    }
    for(int r=row+1; r<row+h-1; r++) {
        sh_putc(r, col,     '|', attr);
        sh_putc(r, col+w-1, '|', attr);
    }
    for(int r=row+1; r<row+h-1; r++)
        for(int c=col+1; c<col+w-1; c++)
            sh_putc(r, c, ' ', attr);
}

static void sh_dbl_box(int row, int col, int w, int h, uint8_t attr) {
    sh_putc(row,     col,     '\xC9', attr);
    sh_putc(row,     col+w-1, '\xBB', attr);
    sh_putc(row+h-1, col,     '\xC8', attr);
    sh_putc(row+h-1, col+w-1, '\xBC', attr);
    for(int c=col+1; c<col+w-1; c++) {
        sh_putc(row,     c, '\xCD', attr);
        sh_putc(row+h-1, c, '\xCD', attr);
    }
    for(int r=row+1; r<row+h-1; r++) {
        sh_putc(r, col,     '\xBA', attr);
        sh_putc(r, col+w-1, '\xBA', attr);
    }
    for(int r=row+1; r<row+h-1; r++)
        for(int c=col+1; c<col+w-1; c++)
            sh_putc(r, c, ' ', attr);
}

/* ═══════════════════════════════════════════════════════════════════════════
   String helpers (no libc)
   ═══════════════════════════════════════════════════════════════════════════ */
static int ed_strlen(const char *s) { int n=0; while(s[n]) n++; return n; }
static void ed_strcpy(char *d, const char *s) { while((*d++=*s++)); }
static void ed_strncpy(char *d, const char *s, int n) {
    int i=0; while(i<n-1 && s[i]) { d[i]=s[i]; i++; } d[i]=0;
}
static int ed_strcmp(const char *a, const char *b) {
    while(*a && *a==*b) { a++; b++; } return (uint8_t)*a-(uint8_t)*b;
}
static int ed_strncmp(const char *a, const char *b, int n) {
    for(int i=0;i<n;i++) {
        if(!a[i]&&!b[i]) return 0;
        if(a[i]!=b[i]) return (uint8_t)a[i]-(uint8_t)b[i];
    }
    return 0;
}
static void ed_memmove(char *d, const char *s, int n) {
    if(d<s) { for(int i=0;i<n;i++) d[i]=s[i]; }
    else    { for(int i=n-1;i>=0;i--) d[i]=s[i]; }
}
static void ed_itoa(int n, char *buf) {
    if(n==0){buf[0]='0';buf[1]=0;return;}
    char tmp[12]; int i=0;
    while(n>0){tmp[i++]='0'+(n%10);n/=10;}
    int j=0; while(i>0) buf[j++]=tmp[--i]; buf[j]=0;
}
static int ed_atoi(const char *s) {
    int n=0; while(*s>='0'&&*s<='9'){n=n*10+(*s-'0');s++;} return n;
}
static int ed_tolower_copy(const char *s, int len, char *buf, int bufsz) {
    int i=0;
    for(; i<len && i<bufsz-1; i++) {
        char c=s[i];
        if(c>='A'&&c<='Z') c=c-'A'+'a';
        buf[i]=c;
    }
    buf[i]=0; return i;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Character class helpers
   ═══════════════════════════════════════════════════════════════════════════ */
static int ed_is_ident(char c)    { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'; }
static int ed_is_digit(char c)    { return c>='0'&&c<='9'; }
static int ed_is_hex(char c)      { return ed_is_digit(c)||(c>='a'&&c<='f')||(c>='A'&&c<='F'); }
static int ed_is_space(char c)    { return c==' '||c=='\t'; }
static int ed_is_alpha(char c)    { return (c>='a'&&c<='z')||(c>='A'&&c<='Z'); }
static int ed_is_tag_name(char c) { return ed_is_ident(c)||c=='-'||c==':'; }
static int ed_is_punct(char c)    {
    return c=='('||c==')'||c=='{'||c=='}'||c=='['||c==']'||
           c==';'||c==','||c==':'||c=='.'||c=='|'||c=='&'||
           c=='!'||c=='='||c=='<'||c=='>'||c=='+'||c=='-'||
           c=='*'||c=='/'||c=='^'||c=='~'||c=='@';
}

/* ═══════════════════════════════════════════════════════════════════════════
   Keyword lookup
   ═══════════════════════════════════════════════════════════════════════════ */
static int ed_is_kw(const char *word, int len, FileType ft) {
    if(len==0||len>32||ft==FT_TEXT) return 0;
    char tmp[33]; for(int i=0;i<len;i++) tmp[i]=word[i]; tmp[len]=0;
    const char **list;
    switch(ft) {
        case FT_RSH:  list = RSH_KW;  break;
        case FT_RASH: list = RASH_KW; break;
        case FT_HLS:  list = HLS_KW;  break;
        default:      list = RSH_KW;  break;
    }
    for(int i=0;list[i];i++) if(ed_strcmp(tmp,list[i])==0) return 1;
    return 0;
}
static int ed_in_list(const char *word, int len, const char **list) {
    if(len==0||len>64) return 0;
    char tmp[65]; for(int i=0;i<len;i++) tmp[i]=word[i]; tmp[len]=0;
    for(int i=0;list[i];i++) if(ed_strcmp(tmp,list[i])==0) return 1;
    return 0;
}
static int ed_in_list_nocase(const char *word, int len, const char **list) {
    if(len==0||len>64) return 0;
    char low[65]; ed_tolower_copy(word, len, low, 65);
    for(int i=0;list[i];i++) if(ed_strcmp(low,list[i])==0) return 1;
    return 0;
}
static int ed_is_hls_type(const char *word, int len) {
    if(len==0||len>64) return 0;
    char tmp[65]; for(int i=0;i<len;i++) tmp[i]=word[i]; tmp[len]=0;
    for(int i=0;HLS_TYPES[i];i++) if(ed_strcmp(tmp,HLS_TYPES[i])==0) return 1;
    return 0;
}
static FileType ed_detect_ft(const char *path) {
    int len = ed_strlen(path);
    if(len>=5 && ed_strcmp(path+len-5,".rash")==0) return FT_RASH;
    if(len>=4 && ed_strcmp(path+len-4,".rsh" )==0) return FT_RSH;
    if(len>=4 && ed_strcmp(path+len-4,".hls" )==0) return FT_HLS;
    if(len>=5 && ed_strcmp(path+len-5,".html")==0) return FT_HTML;
    if(len>=4 && ed_strcmp(path+len-4,".htm" )==0) return FT_HTML;
    return FT_TEXT;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Settings persistence  (V5)
   ═══════════════════════════════════════════════════════════════════════════ */
static char set_io_buf[1024];

static void ed_apply_settings(void) {
    ed_theme_idx  = ed_settings.theme_idx;
    ed_show_nums  = ed_settings.line_numbers;
}

static void ed_settings_to_str(char *buf, int bufsz) {
    int p=0;
    const char *magic = SETTINGS_MAGIC;
    while(*magic && p<bufsz-1) buf[p++]=*magic++;

#define WRITE_KV(k, v) \
    do { \
        const char *key = k; \
        while(*key && p<bufsz-1) buf[p++]=*key++; \
        char tmp[16]; ed_itoa(v, tmp); \
        for(int _i=0;tmp[_i]&&p<bufsz-1;_i++) buf[p++]=tmp[_i]; \
        if(p<bufsz-1) buf[p++]='\n'; \
    } while(0)

    WRITE_KV("theme=",          ed_settings.theme_idx);
    WRITE_KV("linenums=",       ed_settings.line_numbers?1:0);
    WRITE_KV("relnums=",        ed_settings.relative_numbers?1:0);
    WRITE_KV("autoindent=",     ed_settings.auto_indent?1:0);
    WRITE_KV("tabspaces=",      ed_settings.tab_spaces?1:0);
    WRITE_KV("tabwidth=",       ed_settings.tab_width);
    WRITE_KV("showmod=",        ed_settings.show_modified?1:0);
    WRITE_KV("wrapsearch=",     ed_settings.wrap_search?1:0);
    WRITE_KV("syntax=",         ed_settings.syntax_highlight?1:0);
    WRITE_KV("casesens=",       ed_settings.case_search?1:0);
    WRITE_KV("cursorhl=",       ed_settings.cursor_line_hl?1:0);
    WRITE_KV("hints=",          ed_settings.show_hints?1:0);
    WRITE_KV("smarthome=",      ed_settings.smart_home?1:0);
    WRITE_KV("showws=",         ed_settings.show_whitespace?1:0);
    WRITE_KV("showeol=",        ed_settings.show_eol?1:0);
    WRITE_KV("trailws=",        ed_settings.trailing_ws_warn?1:0);
    WRITE_KV("bracketmatch=",   ed_settings.bracket_match?1:0);
    WRITE_KV("wrap80=",         ed_settings.word_wrap_indicator?1:0);
    WRITE_KV("scrolloff=",      ed_settings.scroll_off);
    WRITE_KV("boldkw=",         ed_settings.bold_keywords?1:0);
    WRITE_KV("dimcomment=",     ed_settings.dim_comments?1:0);
    WRITE_KV("statuscol=",      ed_settings.show_status_col?1:0);
    WRITE_KV("statuspct=",      ed_settings.show_status_pct?1:0);
    WRITE_KV("saveexit=",       ed_settings.save_on_exit?1:0);
    WRITE_KV("confirmdd=",      ed_settings.confirm_delete_line?1:0);
    WRITE_KV("dblspace=",       ed_settings.double_space_sentence?1:0);
    WRITE_KV("undolimit=",      ed_settings.undo_limit);
    WRITE_KV("hlurls=",         ed_settings.highlight_urls?1:0);
    WRITE_KV("ruler=",          ed_settings.show_ruler?1:0);
    WRITE_KV("autopairs=",      ed_settings.auto_pairs?1:0);
    WRITE_KV("trimsave=",       ed_settings.trim_trailing_save?1:0);
    WRITE_KV("finalnl=",        ed_settings.insert_final_newline?1:0);
    WRITE_KV("hardwrap=",       ed_settings.hard_wrap?1:0);
    WRITE_KV("hardwrapcol=",    ed_settings.hard_wrap_col);
    WRITE_KV("clock=",          ed_settings.show_clock?1:0);
#undef WRITE_KV
    buf[p]=0;
}

static int ed_save_settings(void) {
    if(!avfs_is_directory("/tmp")) avfs_create_dir("/tmp");
    ed_settings_to_str(set_io_buf, sizeof(set_io_buf));
    int len = ed_strlen(set_io_buf);
    avfs_remove_file(SETTINGS_PATH);
    if(avfs_create_file(SETTINGS_PATH, (uint32_t)len) != 0) return -1;
    if(avfs_write_file(SETTINGS_PATH, set_io_buf, (uint32_t)len, 0) < 0) return -1;
    return 0;
}

static void ed_parse_settings(const char *buf, int sz) {
    int i=0;
    while(i<sz && buf[i]!='\n') i++; if(i<sz) i++;
    while(i<sz) {
        char key[32]; int ki=0;
        while(i<sz && buf[i]!='=' && buf[i]!='\n' && ki<31) key[ki++]=buf[i++];
        key[ki]=0;
        if(i<sz && buf[i]=='=') i++;
        char val[32]; int vi=0;
        while(i<sz && buf[i]!='\n' && vi<31) val[vi++]=buf[i++];
        val[vi]=0;
        if(i<sz && buf[i]=='\n') i++;
        int ival = ed_atoi(val);

        if(ed_strcmp(key,"theme")==0)       { if(ival>=0&&ival<NUM_THEMES) ed_settings.theme_idx=ival; }
        else if(ed_strcmp(key,"linenums")==0)    ed_settings.line_numbers     =(val[0]=='1');
        else if(ed_strcmp(key,"relnums")==0)     ed_settings.relative_numbers =(val[0]=='1');
        else if(ed_strcmp(key,"autoindent")==0)  ed_settings.auto_indent      =(val[0]=='1');
        else if(ed_strcmp(key,"tabspaces")==0)   ed_settings.tab_spaces       =(val[0]=='1');
        else if(ed_strcmp(key,"tabwidth")==0)    { if(ival==2||ival==4) ed_settings.tab_width=ival; }
        else if(ed_strcmp(key,"showmod")==0)     ed_settings.show_modified    =(val[0]=='1');
        else if(ed_strcmp(key,"wrapsearch")==0)  ed_settings.wrap_search      =(val[0]=='1');
        else if(ed_strcmp(key,"syntax")==0)      ed_settings.syntax_highlight =(val[0]=='1');
        else if(ed_strcmp(key,"casesens")==0)    ed_settings.case_search      =(val[0]=='1');
        else if(ed_strcmp(key,"cursorhl")==0)    ed_settings.cursor_line_hl   =(val[0]=='1');
        else if(ed_strcmp(key,"hints")==0)       ed_settings.show_hints       =(val[0]=='1');
        else if(ed_strcmp(key,"smarthome")==0)   ed_settings.smart_home       =(val[0]=='1');
        else if(ed_strcmp(key,"showws")==0)      ed_settings.show_whitespace  =(val[0]=='1');
        else if(ed_strcmp(key,"showeol")==0)     ed_settings.show_eol         =(val[0]=='1');
        else if(ed_strcmp(key,"trailws")==0)     ed_settings.trailing_ws_warn =(val[0]=='1');
        else if(ed_strcmp(key,"bracketmatch")==0)ed_settings.bracket_match    =(val[0]=='1');
        else if(ed_strcmp(key,"wrap80")==0)      ed_settings.word_wrap_indicator=(val[0]=='1');
        else if(ed_strcmp(key,"scrolloff")==0)   { if(ival>=0&&ival<=10) ed_settings.scroll_off=ival; }
        else if(ed_strcmp(key,"boldkw")==0)      ed_settings.bold_keywords    =(val[0]=='1');
        else if(ed_strcmp(key,"dimcomment")==0)  ed_settings.dim_comments     =(val[0]=='1');
        else if(ed_strcmp(key,"statuscol")==0)   ed_settings.show_status_col  =(val[0]=='1');
        else if(ed_strcmp(key,"statuspct")==0)   ed_settings.show_status_pct  =(val[0]=='1');
        else if(ed_strcmp(key,"saveexit")==0)    ed_settings.save_on_exit     =(val[0]=='1');
        else if(ed_strcmp(key,"confirmdd")==0)   ed_settings.confirm_delete_line=(val[0]=='1');
        else if(ed_strcmp(key,"dblspace")==0)    ed_settings.double_space_sentence=(val[0]=='1');
        else if(ed_strcmp(key,"undolimit")==0)   { if(ival==16||ival==32||ival==64) ed_settings.undo_limit=ival; }
        else if(ed_strcmp(key,"hlurls")==0)      ed_settings.highlight_urls   =(val[0]=='1');
        else if(ed_strcmp(key,"ruler")==0)       ed_settings.show_ruler       =(val[0]=='1');
        else if(ed_strcmp(key,"autopairs")==0)   ed_settings.auto_pairs       =(val[0]=='1');
        else if(ed_strcmp(key,"trimsave")==0)    ed_settings.trim_trailing_save=(val[0]=='1');
        else if(ed_strcmp(key,"finalnl")==0)     ed_settings.insert_final_newline=(val[0]=='1');
        else if(ed_strcmp(key,"hardwrap")==0)    ed_settings.hard_wrap        =(val[0]=='1');
        else if(ed_strcmp(key,"hardwrapcol")==0) { if(ival==60||ival==72||ival==80) ed_settings.hard_wrap_col=ival; }
        else if(ed_strcmp(key,"clock")==0)       ed_settings.show_clock       =(val[0]=='1');
    }
}

static void ed_load_settings(void) {
    ed_settings.line_numbers          = true;
    ed_settings.relative_numbers      = false;
    ed_settings.auto_indent           = true;
    ed_settings.tab_spaces            = true;
    ed_settings.tab_width             = 4;
    ed_settings.show_modified         = true;
    ed_settings.wrap_search           = true;
    ed_settings.syntax_highlight      = true;
    ed_settings.case_search           = false;
    ed_settings.cursor_line_hl        = false;
    ed_settings.show_hints            = true;
    ed_settings.smart_home            = true;
    ed_settings.show_whitespace       = false;
    ed_settings.show_eol              = false;
    ed_settings.trailing_ws_warn      = true;
    ed_settings.bracket_match         = true;
    ed_settings.word_wrap_indicator   = true;
    ed_settings.scroll_off            = 3;
    ed_settings.bold_keywords         = false;
    ed_settings.dim_comments          = false;
    ed_settings.show_status_col       = true;
    ed_settings.show_status_pct       = false;
    ed_settings.save_on_exit          = false;
    ed_settings.confirm_delete_line   = false;
    ed_settings.double_space_sentence = false;
    ed_settings.undo_limit            = 64;
    ed_settings.highlight_urls        = true;
    ed_settings.show_ruler            = false;
    ed_settings.auto_pairs            = false;
    ed_settings.trim_trailing_save    = false;
    ed_settings.insert_final_newline  = true;
    ed_settings.hard_wrap             = false;
    ed_settings.hard_wrap_col         = 80;
    ed_settings.show_clock            = false;
    ed_settings.theme_idx             = 0;

    int sz = avfs_get_filesize(SETTINGS_PATH);
    if(sz<=0 || sz>=(int)sizeof(set_io_buf)) { ed_apply_settings(); return; }
    if(avfs_read_file(SETTINGS_PATH, set_io_buf, (uint32_t)sz, 0)!=0) { ed_apply_settings(); return; }
    set_io_buf[sz]=0;
    if(ed_strncmp(set_io_buf, "RSHIDT_SETTINGS_V", 17)!=0) { ed_apply_settings(); return; }
    ed_parse_settings(set_io_buf, sz);
    ed_apply_settings();
}

/* ═══════════════════════════════════════════════════════════════════════════
   Settings panel
   ═══════════════════════════════════════════════════════════════════════════ */
#define SET_BOX_W    64
#define SET_BOX_H    (SET_VIEW_ROWS + 8)
#define SET_BOX_ROW  ((TERM_H - SET_BOX_H) / 2)
#define SET_BOX_COL  ((TERM_W - SET_BOX_W) / 2)

#define SET_BORDER   0x17
#define SET_TITLE    0x1F
#define SET_NORMAL   0x07
#define SET_SEL      0x70
#define SET_KEY      0x0B
#define SET_VAL_ON   0x0A
#define SET_VAL_OFF  0x08
#define SET_DD_BG    0x17
#define SET_DD_SEL   0x71
#define SET_BTN      0x4F
#define SET_BTN_SEL  0x2F
#define SET_CAT      0x0E
#define SET_SCR_IND  0x0F
#define SET_ACTIVE   0x3F

static void set_draw_divider(int row, int bc, const char *label, uint8_t attr) {
    int cc = bc+2;
    sh_putc(row, cc++, ' ', attr);
    for(const char *s=label; *s && cc<bc+SET_BOX_W-2; s++) sh_putc(row, cc++, *s, attr);
    sh_putc(row, cc++, ' ', attr);
    for(; cc<bc+SET_BOX_W-1; cc++) sh_putc(row, cc, '-', attr);
}

static void set_draw_toggle(int row, int col, bool val, bool selected) {
    uint8_t attr = selected ? SET_SEL : SET_NORMAL;
    uint8_t von  = selected ? SET_SEL : SET_VAL_ON;
    uint8_t voff = selected ? SET_SEL : SET_VAL_OFF;
    sh_putc(row, col, '[', attr);
    if(val) { sh_putc(row, col+1, 'O', von); sh_putc(row, col+2, 'N', von); sh_putc(row, col+3, ']', attr); }
    else    { sh_putc(row, col+1, 'O', voff); sh_putc(row, col+2, 'F', voff); sh_putc(row, col+3, 'F', voff); sh_putc(row, col+4, ']', attr); }
}

typedef struct { const char *label; int item_id; int is_cat; } SetItem;

static const SetItem SET_ITEMS[] = {
    { "-- Display --",                   -1,                    1 },
    { "  Line Numbers          ",         SET_ITEM_LINENUMS,     0 },
    { "  Relative Numbers      ",         SET_ITEM_RELNUMS,      0 },
    { "  Show Modified&Modified Flag    ",         SET_ITEM_SHOWMOD,      0 },
    { "  Highlight Cursor Line ",         SET_ITEM_CURSORHL,     0 },
    { "  Show Hint Bar         ",         SET_ITEM_HINTBAR,      0 },
    { "  Show Clock (frames)   ",         SET_ITEM_CLOCK,        0 },
    { "  Col-80 Ruler          ",         SET_ITEM_RULER,        0 },
    { "  Col-80 Wrap Indicator ",         SET_ITEM_WRAP80,       0 },
    { "-- Whitespace --",                 -1,                    1 },
    { "  Show Whitespace       ",         SET_ITEM_SHOWWS,       0 },
    { "  Show End-of-Line ($)  ",         SET_ITEM_SHOWEOL,      0 },
    { "  Warn Trailing Space   ",         SET_ITEM_TRAILWS,      0 },
    { "-- Editing --",                    -1,                    1 },
    { "  Auto Indent           ",         SET_ITEM_AUTOINDENT,   0 },
    { "  Smart Home            ",         SET_ITEM_SMARTHOME,    0 },
    { "  Auto Pairs ()[]{}     ",         SET_ITEM_AUTO_PAIRS,   0 },
    { "  Bracket Match Hl      ",         SET_ITEM_BRACKETMATCH, 0 },
    { "  Double Space Sentence ",         SET_ITEM_DBL_SPACE,    0 },
    { "  Hard Wrap             ",         SET_ITEM_HARD_WRAP,    0 },
    { "  Hard Wrap Column      ",         SET_ITEM_HARD_WRAP_COL,0 },
    { "  Scroll-Off Margin     ",         SET_ITEM_SCROLLOFF,    0 },
    { "-- Tabs --",                       -1,                    1 },
    { "  Tabs as Spaces        ",         SET_ITEM_TABSPACES,    0 },
    { "  Tab Width             ",         SET_ITEM_TABWIDTH,     0 },
    { "-- Search --",                     -1,                    1 },
    { "  Wrap-around Search    ",         SET_ITEM_WRAPSEARCH,   0 },
    { "  Case Sensitive Search ",         SET_ITEM_CASESEARCH,   0 },
    { "-- Syntax --",                     -1,                    1 },
    { "  Syntax Highlighting   ",         SET_ITEM_SYNTAX,       0 },
    { "  Bold Keywords         ",         SET_ITEM_BOLDKW,       0 },
    { "  Dim Comments          ",         SET_ITEM_DIMCOMMENT,   0 },
    { "  Highlight URLs        ",         SET_ITEM_HL_URLS,      0 },
    { "-- Status Bar --",                 -1,                    1 },
    { "  Column in Status      ",         SET_ITEM_STATUS_COL,   0 },
    { "  Percentage in Status  ",         SET_ITEM_STATUS_PCT,   0 },
    { "-- Save & Exit --",                -1,                    1 },
    { "  Auto-Save on Exit     ",         SET_ITEM_SAVE_EXIT,    0 },
    { "  Confirm Line Delete   ",         SET_ITEM_CONFIRM_DD,   0 },
    { "  Trim Trailing on Save ",         SET_ITEM_TRIM_SAVE,    0 },
    { "  Insert Final Newline  ",         SET_ITEM_FINAL_NL,     0 },
    { "-- Undo --",                       -1,                    1 },
    { "  Undo Limit            ",         SET_ITEM_UNDO_LIMIT,   0 },
    { "-- Theme --",                      -1,                    1 },
    { "  Theme                 ",         SET_ITEM_THEME,        0 },
    { NULL, 0, 0 }
};
#define SET_LIST_LEN 44

static int set_item_to_list_idx(int item_id) {
    for(int i=0; SET_ITEMS[i].label; i++)
        if(!SET_ITEMS[i].is_cat && SET_ITEMS[i].item_id == item_id) return i;
    return 0;
}

static int set_next_sel(int cur_list, int dir) {
    int n = cur_list + dir;
    while(n >= 0 && n < SET_LIST_LEN && SET_ITEMS[n].label) {
        if(!SET_ITEMS[n].is_cat) return n;
        n += dir;
    }
    if(dir > 0) { for(int i=0;i<SET_LIST_LEN&&SET_ITEMS[i].label;i++) if(!SET_ITEMS[i].is_cat) return i; }
    else { for(int i=SET_LIST_LEN-1;i>=0;i--) if(SET_ITEMS[i].label && !SET_ITEMS[i].is_cat) return i; }
    return cur_list;
}

/* ── Fix the label that had a typo ─────────────────────────────────────── */
/* (SET_ITEMS[5] label fix — "Show Modified Flag" — applied above in array) */

static void ed_draw_settings(void) {
    int br = SET_BOX_ROW, bc = SET_BOX_COL;
    sh_dbl_box(br, bc, SET_BOX_W, SET_BOX_H, SET_BORDER);

    { const char *t = " \xCE RSHIDT Settings \xCE "; int tl = ed_strlen(t); int tc = bc + (SET_BOX_W-tl)/2;
      for(int i=0;t[i];i++) sh_putc(br, tc+i, t[i], SET_TITLE); }

    { const char *s = " Arrows:Nav  Enter:Toggle  Esc:Close "; int sl = ed_strlen(s); int sc2 = bc+(SET_BOX_W-sl)/2;
      for(int i=0;s[i]&&sc2+i<bc+SET_BOX_W-1;i++) sh_putc(br+1, sc2+i, s[i], SET_KEY); }

    { int er=br+2, ec=bc+2;
      const char *ee="  \x10 M:"; for(const char *s=ee;*s&&ec<bc+SET_BOX_W-2;s++) sh_putc(er,ec++,*s,SET_KEY);
      sh_putc(er,ec++, ee_secret_m?'\xFB':'-', ee_secret_m?SET_ACTIVE:SET_VAL_OFF);
      sh_putc(er,ec++,' ',SET_KEY);
      { const char *t="T:"; for(const char *s=t;*s&&ec<bc+SET_BOX_W-2;s++) sh_putc(er,ec++,*s,SET_KEY);
        sh_putc(er,ec++, ee_secret_t?'\xFB':'-', ee_secret_t?SET_ACTIVE:SET_VAL_OFF); sh_putc(er,ec++,' ',SET_KEY); }
      { const char *t="R:"; for(const char *s=t;*s&&ec<bc+SET_BOX_W-2;s++) sh_putc(er,ec++,*s,SET_KEY);
        sh_putc(er,ec++, ee_secret_r?'\xFB':'-', ee_secret_r?SET_ACTIVE:SET_VAL_OFF); sh_putc(er,ec++,' ',SET_KEY); }
      if(matrix_rain_on) { const char *rn="  [RAIN: ACTIVE]"; for(const char *s=rn;*s&&ec<bc+SET_BOX_W-2;s++) sh_putc(er,ec++,*s,SET_ACTIVE); }
      else { const char *h="  (Ctrl+M+T+R = ???)"; for(const char *s=h;*s&&ec<bc+SET_BOX_W-2;s++) sh_putc(er,ec++,*s,SET_VAL_OFF); }
    }

    for(int c=bc+1;c<bc+SET_BOX_W-1;c++) sh_putc(br+3, c, '\xCD', SET_BORDER);

    if(set_cursor < set_scroll) set_scroll = set_cursor;
    if(set_cursor >= set_scroll + SET_VIEW_ROWS) set_scroll = set_cursor - SET_VIEW_ROWS + 1;
    if(set_scroll < 0) set_scroll = 0;

    int items_start = br + 4;
    int drawn = 0;
    for(int li = set_scroll; li < SET_LIST_LEN && SET_ITEMS[li].label && drawn < SET_VIEW_ROWS; li++) {
        int row = items_start + drawn;
        if(SET_ITEMS[li].is_cat) { set_draw_divider(row, bc, SET_ITEMS[li].label, SET_CAT); }
        else {
            bool sel = (li == set_cursor);
            uint8_t la = sel ? SET_SEL : SET_NORMAL;
            int col = bc + 2;
            const char *lbl = SET_ITEMS[li].label;
            for(int i=0; lbl[i] && col<bc+SET_BOX_W-16; i++) sh_putc(row, col++, lbl[i], la);
            int val_col = bc + SET_BOX_W - 14;
            while(col < val_col) sh_putc(row, col++, ' ', la);
            int item = SET_ITEMS[li].item_id;

            if(item == SET_ITEM_THEME) {
                const char *tn = THEMES[ed_settings.theme_idx].name;
                for(int i=0;tn[i]&&col<bc+SET_BOX_W-2;i++) sh_putc(row,col++,tn[i],sel?SET_SEL:SET_KEY);
            } else if(item==SET_ITEM_TABWIDTH) {
                char tb[4]; ed_itoa(ed_settings.tab_width,tb);
                for(int i=0;tb[i]&&col<bc+SET_BOX_W-2;i++) sh_putc(row,col++,tb[i],sel?SET_SEL:SET_VAL_ON);
            } else if(item==SET_ITEM_SCROLLOFF) {
                char sb[4]; ed_itoa(ed_settings.scroll_off,sb);
                for(int i=0;sb[i]&&col<bc+SET_BOX_W-2;i++) sh_putc(row,col++,sb[i],sel?SET_SEL:SET_VAL_ON);
            } else if(item==SET_ITEM_UNDO_LIMIT) {
                char ub[4]; ed_itoa(ed_settings.undo_limit,ub);
                for(int i=0;ub[i]&&col<bc+SET_BOX_W-2;i++) sh_putc(row,col++,ub[i],sel?SET_SEL:SET_VAL_ON);
            } else if(item==SET_ITEM_HARD_WRAP_COL) {
                char wb[4]; ed_itoa(ed_settings.hard_wrap_col,wb);
                for(int i=0;wb[i]&&col<bc+SET_BOX_W-2;i++) sh_putc(row,col++,wb[i],sel?SET_SEL:SET_VAL_ON);
            } else {
                bool val = false;
                switch(item) {
                    case SET_ITEM_LINENUMS:    val=ed_settings.line_numbers; break;
                    case SET_ITEM_RELNUMS:     val=ed_settings.relative_numbers; break;
                    case SET_ITEM_AUTOINDENT:  val=ed_settings.auto_indent; break;
                    case SET_ITEM_TABSPACES:   val=ed_settings.tab_spaces; break;
                    case SET_ITEM_SHOWMOD:     val=ed_settings.show_modified; break;
                    case SET_ITEM_WRAPSEARCH:  val=ed_settings.wrap_search; break;
                    case SET_ITEM_SYNTAX:      val=ed_settings.syntax_highlight; break;
                    case SET_ITEM_CASESEARCH:  val=ed_settings.case_search; break;
                    case SET_ITEM_CURSORHL:    val=ed_settings.cursor_line_hl; break;
                    case SET_ITEM_HINTBAR:     val=ed_settings.show_hints; break;
                    case SET_ITEM_SMARTHOME:   val=ed_settings.smart_home; break;
                    case SET_ITEM_SHOWWS:      val=ed_settings.show_whitespace; break;
                    case SET_ITEM_SHOWEOL:     val=ed_settings.show_eol; break;
                    case SET_ITEM_TRAILWS:     val=ed_settings.trailing_ws_warn; break;
                    case SET_ITEM_BRACKETMATCH:val=ed_settings.bracket_match; break;
                    case SET_ITEM_WRAP80:      val=ed_settings.word_wrap_indicator; break;
                    case SET_ITEM_BOLDKW:      val=ed_settings.bold_keywords; break;
                    case SET_ITEM_DIMCOMMENT:  val=ed_settings.dim_comments; break;
                    case SET_ITEM_STATUS_COL:  val=ed_settings.show_status_col; break;
                    case SET_ITEM_STATUS_PCT:  val=ed_settings.show_status_pct; break;
                    case SET_ITEM_SAVE_EXIT:   val=ed_settings.save_on_exit; break;
                    case SET_ITEM_CONFIRM_DD:  val=ed_settings.confirm_delete_line; break;
                    case SET_ITEM_DBL_SPACE:   val=ed_settings.double_space_sentence; break;
                    case SET_ITEM_HL_URLS:     val=ed_settings.highlight_urls; break;
                    case SET_ITEM_RULER:       val=ed_settings.show_ruler; break;
                    case SET_ITEM_AUTO_PAIRS:  val=ed_settings.auto_pairs; break;
                    case SET_ITEM_TRIM_SAVE:   val=ed_settings.trim_trailing_save; break;
                    case SET_ITEM_FINAL_NL:    val=ed_settings.insert_final_newline; break;
                    case SET_ITEM_HARD_WRAP:   val=ed_settings.hard_wrap; break;
                    case SET_ITEM_CLOCK:       val=ed_settings.show_clock; break;
                }
                set_draw_toggle(row, col, val, sel);
            }
            while(col<bc+SET_BOX_W-2) sh_putc(row,col++,' ',sel?SET_SEL:SET_NORMAL);
        }
        drawn++;
    }
    for(int r=drawn;r<SET_VIEW_ROWS;r++) for(int c=bc+1;c<bc+SET_BOX_W-1;c++) sh_putc(items_start+r,c,' ',SET_NORMAL);

    { int sep_row=items_start+SET_VIEW_ROWS; for(int c=bc+1;c<bc+SET_BOX_W-1;c++) sh_putc(sep_row,c,'\xCD',SET_BORDER); }
    { int btn_row=items_start+SET_VIEW_ROWS+1; int btn_col=bc+(SET_BOX_W-14)/2;
      bool ssel=(set_cursor>=0&&set_cursor<SET_LIST_LEN&&SET_ITEMS[set_cursor].item_id==SET_ITEM_SAVE);
      const char *btn=" [S] Save "; for(int i=0;btn[i];i++) sh_putc(btn_row,btn_col+i,btn[i],ssel?SET_BTN_SEL:SET_BTN); }
    if(set_scroll>0) sh_putc(items_start-1,bc+SET_BOX_W-2,'\x18',SET_SCR_IND);
    if(set_scroll+SET_VIEW_ROWS<SET_LIST_LEN) sh_putc(items_start+SET_VIEW_ROWS,bc+SET_BOX_W-2,'\x19',SET_SCR_IND);

    if(set_theme_open) {
        int tw=30, th=NUM_THEMES+2; if(th>SET_BOX_H-2) th=SET_BOX_H-2;
        int tr=br+1, tlc=bc+SET_BOX_W-tw-2;
        sh_dbl_box(tr,tlc,tw,th,SET_DD_BG);
        for(int i=0;i<NUM_THEMES&&i<th-2;i++) {
            int r=tr+1+i; uint8_t a=(i==set_theme_sel)?SET_DD_SEL:SET_NORMAL;
            sh_putc(r,tlc+1,' ',a);
            const char *nm=THEMES[i].name;
            for(int j=0;nm[j]&&j<tw-4;j++) sh_putc(r,tlc+2+j,nm[j],a);
            for(int j=ed_strlen(nm);j<tw-4;j++) sh_putc(r,tlc+2+j,' ',a);
            sh_putc(r,tlc+tw-2,' ',a);
        }
    }
}

static void ed_settings_toggle(int item_id) {
    switch(item_id) {
        case SET_ITEM_LINENUMS:    ed_settings.line_numbers^=1; break;
        case SET_ITEM_RELNUMS:     ed_settings.relative_numbers^=1; break;
        case SET_ITEM_AUTOINDENT:  ed_settings.auto_indent^=1; break;
        case SET_ITEM_TABSPACES:  ed_settings.tab_spaces^=1; break;
        case SET_ITEM_TABWIDTH:    ed_settings.tab_width=(ed_settings.tab_width==4)?2:4; break;
        case SET_ITEM_SHOWMOD:     ed_settings.show_modified^=1; break;
        case SET_ITEM_WRAPSEARCH:  ed_settings.wrap_search^=1; break;
        case SET_ITEM_SYNTAX:      ed_settings.syntax_highlight^=1; break;
        case SET_ITEM_CASESEARCH:  ed_settings.case_search^=1; break;
        case SET_ITEM_CURSORHL:    ed_settings.cursor_line_hl^=1; break;
        case SET_ITEM_HINTBAR:     ed_settings.show_hints^=1; break;
        case SET_ITEM_SMARTHOME:   ed_settings.smart_home^=1; break;
        case SET_ITEM_SHOWWS:      ed_settings.show_whitespace^=1; break;
        case SET_ITEM_SHOWEOL:     ed_settings.show_eol^=1; break;
        case SET_ITEM_TRAILWS:     ed_settings.trailing_ws_warn^=1; break;
        case SET_ITEM_BRACKETMATCH:ed_settings.bracket_match^=1; break;
        case SET_ITEM_WRAP80:      ed_settings.word_wrap_indicator^=1; break;
        case SET_ITEM_SCROLLOFF:   ed_settings.scroll_off=(ed_settings.scroll_off+1)%11; break;
        case SET_ITEM_BOLDKW:      ed_settings.bold_keywords^=1; break;
        case SET_ITEM_DIMCOMMENT:  ed_settings.dim_comments^=1; break;
        case SET_ITEM_STATUS_COL:  ed_settings.show_status_col^=1; break;
        case SET_ITEM_STATUS_PCT:  ed_settings.show_status_pct^=1; break;
        case SET_ITEM_SAVE_EXIT:   ed_settings.save_on_exit^=1; break;
        case SET_ITEM_CONFIRM_DD:  ed_settings.confirm_delete_line^=1; break;
        case SET_ITEM_DBL_SPACE:   ed_settings.double_space_sentence^=1; break;
        case SET_ITEM_UNDO_LIMIT:  { if(ed_settings.undo_limit==16) ed_settings.undo_limit=32; else if(ed_settings.undo_limit==32) ed_settings.undo_limit=64; else ed_settings.undo_limit=16; break; }
        case SET_ITEM_HL_URLS:     ed_settings.highlight_urls^=1; break;
        case SET_ITEM_RULER:       ed_settings.show_ruler^=1; break;
        case SET_ITEM_AUTO_PAIRS:  ed_settings.auto_pairs^=1; break;
        case SET_ITEM_TRIM_SAVE:   ed_settings.trim_trailing_save^=1; break;
        case SET_ITEM_FINAL_NL:    ed_settings.insert_final_newline^=1; break;
        case SET_ITEM_HARD_WRAP:   ed_settings.hard_wrap^=1; break;
        case SET_ITEM_HARD_WRAP_COL: { if(ed_settings.hard_wrap_col==60) ed_settings.hard_wrap_col=72; else if(ed_settings.hard_wrap_col==72) ed_settings.hard_wrap_col=80; else ed_settings.hard_wrap_col=60; break; }
        case SET_ITEM_CLOCK:       ed_settings.show_clock^=1; break;
        case SET_ITEM_THEME:       set_theme_open=!set_theme_open; set_theme_sel=ed_settings.theme_idx; break;
        case SET_ITEM_SAVE:        ed_save_settings(); ed_apply_settings(); ed_strcpy(ed_msg,"Settings saved"); break;
    }
    ed_apply_settings();
}

/* ═══════════════════════════════════════════════════════════════════════════
   Undo
   ═══════════════════════════════════════════════════════════════════════════ */
static void ed_push_undo(void) {
    int idx = (ed_undo_head + ed_undo_count) % UNDO_DEPTH;
    for(int i=0;i<ed_line_count;i++) ed_strcpy(ed_undo[idx].lines[i], ed_lines[i]);
    ed_undo[idx].line_count = ed_line_count;
    ed_undo[idx].cur_row   = ed_cur_row;
    ed_undo[idx].cur_col   = ed_cur_col;
    if(ed_undo_count < UNDO_DEPTH) ed_undo_count++;
    else ed_undo_head = (ed_undo_head + 1) % UNDO_DEPTH;
}
static void ed_pop_undo(void) {
    if(ed_undo_count == 0) return;
    ed_undo_count--;
    int idx = (ed_undo_head + ed_undo_count) % UNDO_DEPTH;
    for(int i=0;i<ed_undo[idx].line_count;i++) ed_strcpy(ed_lines[i], ed_undo[idx].lines[i]);
    ed_line_count = ed_undo[idx].line_count;
    ed_cur_row    = ed_undo[idx].cur_row;
    ed_cur_col    = ed_undo[idx].cur_col;
}

/* ═══════════════════════════════════════════════════════════════════════════
   File I/O
   ═══════════════════════════════════════════════════════════════════════════ */
static void ed_load(const char *path) {
    int sz = avfs_get_filesize(path);
    if(sz < 0) return;
    if(sz == 0) { ed_line_count=1; ed_lines[0][0]=0; ed_cur_row=ed_cur_col=ed_scroll=0; ed_modified=false; return; }
    static char iobuf[65536];
    if(sz>(int)sizeof(iobuf)) sz=(int)sizeof(iobuf);
    if(avfs_read_file(path, iobuf, (uint32_t)sz, 0) != 0) return;
    iobuf[sz] = 0;
    ed_line_count = 0; int li = 0;
    for(int i=0; i<sz && ed_line_count<MAX_LINES; i++) {
        if(iobuf[i]=='\n'||iobuf[i]==0) { ed_lines[ed_line_count][li]=0; ed_line_count++; li=0; }
        else { if(li<MAX_LINE_LEN-1) ed_lines[ed_line_count][li++]=iobuf[i]; }
    }
    if(li>0 && ed_line_count<MAX_LINES) { ed_lines[ed_line_count][li]=0; ed_line_count++; }
    if(ed_line_count==0) { ed_line_count=1; ed_lines[0][0]=0; }
    ed_cur_row = ed_cur_col = ed_scroll = 0;
    ed_modified = false;
}

static int ed_save_file(void) {
    if(ed_path[0]==0) return -1;
    static char iobuf[65536];
    int pos = 0;
    for(int i=0; i<ed_line_count && pos<(int)sizeof(iobuf)-2; i++) {
        int l = ed_strlen(ed_lines[i]);
        if(pos+l>=(int)sizeof(iobuf)-2) break;
        ed_strcpy(iobuf+pos, ed_lines[i]); pos+=l;
        iobuf[pos++] = '\n';
    }
    iobuf[pos] = 0;
    avfs_remove_file(ed_path);
    if(avfs_create_file(ed_path,(uint32_t)pos)!=0) return -1;
    if(avfs_write_file(ed_path,iobuf,(uint32_t)pos,0)<0) return -1;
    ed_modified = false;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Syntax highlighting  —  FT_TEXT, FT_RSH, FT_RASH, FT_HLS, FT_HTML
   ═══════════════════════════════════════════════════════════════════════════ */

/* ── Helios / Rust-like line highlighter (shared by FT_RASH and FT_HLS) ── */
static void ed_hl_rust_like(const char *line, int len, int row, int col_off, FileType ft) {
    int i = 0;
    uint8_t kw_col   = TH(keyword);
    uint8_t str_col  = TH(string_col);
    uint8_t cmt_col  = TH(comment);
    uint8_t num_col  = TH(number_col);
    uint8_t pnc_col  = TH(punct);
    uint8_t var_col  = TH(var_col);
    uint8_t type_col = (ft == FT_HLS) ? COL_HLS_TYPE : TH(keyword);

    if(ed_settings.bold_keywords) kw_col |= 0x08;
    if(ed_settings.dim_comments)  cmt_col = 0x01;

    /* Continuing a block comment from previous line */
    if(ed_in_block_comment) {
        while(i < len) {
            if(i+1<len && line[i]=='*' && line[i+1]=='/') {
                sh_putc(row,col_off+i,'*',cmt_col); sh_putc(row,col_off+i+1,'/',cmt_col);
                ed_in_block_comment=false; i+=2; break;
            }
            sh_putc(row,col_off+i,line[i],cmt_col); i++;
        }
        if(ed_in_block_comment) return;
    }

    while(i < len) {
        char c = line[i];

       /* Block comment open: slash-star */
        if(c=='/' && i+1<len && line[i+1]=='*') {
            sh_putc(row,col_off+i,'/',cmt_col); i++;
            sh_putc(row,col_off+i,'*',cmt_col); i++;
            ed_in_block_comment=true;
            while(i<len) {
                if(i+1<len && line[i]=='*' && line[i+1]=='/') {
                    sh_putc(row,col_off+i,'*',cmt_col); i++;
                    sh_putc(row,col_off+i,'/',cmt_col); i++;
                    ed_in_block_comment=false; break;
                }
                sh_putc(row,col_off+i,line[i],cmt_col); i++;
            }
            continue;
        }

        /* Line comment // */
        if(c=='/' && i+1<len && line[i+1]=='/') {
            for(int j=i;j<len;j++) sh_putc(row,col_off+j,line[j],cmt_col);
            break;
        }

        /* Attribute #![…] #[…] (HLS/Rust) */
        if((ft==FT_HLS||ft==FT_RASH) && c=='#' && i+1<len && (line[i+1]=='!'||line[i+1]=='[')) {
            int j=i;
            while(j<len && line[j]!=']') { sh_putc(row,col_off+j,line[j],COL_HLS_ATTRIBUTE); j++; }
            if(j<len) { sh_putc(row,col_off+j,line[j],COL_HLS_ATTRIBUTE); j++; }
            i=j; continue;
        }

        /* String "…" */
        if(c=='"') {
            sh_putc(row,col_off+i,c,str_col); i++;
            while(i<len) {
                if(line[i]=='\\' && i+1<len) { sh_putc(row,col_off+i,line[i],str_col); i++; sh_putc(row,col_off+i,line[i],str_col); i++; continue; }
                sh_putc(row,col_off+i,line[i],str_col);
                if(line[i]=='"') { i++; break; }
                i++;
            }
            continue;
        }

        /* Raw string r"…" r#"…"# (HLS/Rust) */
        if((ft==FT_HLS||ft==FT_RASH) && c=='r' && i+1<len && (line[i+1]=='"'||line[i+1]=='#')) {
            int j=i; sh_putc(row,col_off+j,line[j],str_col); j++;
            while(j<len && line[j]!='"') { sh_putc(row,col_off+j,line[j],str_col); j++; }
            if(j<len) { sh_putc(row,col_off+j,line[j],str_col); j++; }
            i=j; continue;
        }

        /* Char literal 'x' (not lifetime 'a) */
        if((ft==FT_HLS||ft==FT_RASH) && c=='\'' && i+2<len && line[i+2]=='\'') {
            sh_putc(row,col_off+i,'\'',str_col); sh_putc(row,col_off+i+1,line[i+1],str_col); sh_putc(row,col_off+i+2,'\'',str_col);
            i+=3; continue;
        }

        /* Lifetime 'a (HLS/Rust) */
        if((ft==FT_HLS||ft==FT_RASH) && c=='\'' && i+1<len && ed_is_alpha(line[i+1])) {
            sh_putc(row,col_off+i,'\'',COL_HLS_LIFETIME); i++;
            while(i<len && (ed_is_ident(line[i])||line[i]=='_')) { sh_putc(row,col_off+i,line[i],COL_HLS_LIFETIME);i++; }
            continue;
        }

        /* Number: 0xHex, 0bBinary, decimal */
        if(ed_is_digit(c) || (c=='-' && i+1<len && ed_is_digit(line[i+1]))) {
            int start=i;
            if(c=='-') i++;
            if(i+1<len && line[i]=='0' && (line[i+1]=='x'||line[i+1]=='X')) {
                i+=2; while(i<len && ed_is_hex(line[i])) i++;
                while(i<len && ed_is_ident(line[i])) i++;
            } else if(i+1<len && line[i]=='0' && (line[i+1]=='b'||line[i+1]=='B')) {
                i+=2; while(i<len && (line[i]=='0'||line[i]=='1')) i++;
                while(i<len && ed_is_ident(line[i])) i++;
            } else {
                while(i<len && ed_is_digit(line[i])) i++;
                if(i<len && line[i]=='.' && i+1<len && ed_is_digit(line[i+1])) { i++; while(i<len && ed_is_digit(line[i])) i++; }
                while(i<len && ed_is_ident(line[i])) i++;
            }
            for(int j=start;j<i;j++) sh_putc(row,col_off+j,line[j],num_col);
            continue;
        }

        /* Identifier / keyword / type */
        if(ed_is_alpha(c) || c=='_') {
            int start=i;
            while(i<len && ed_is_ident(line[i])) i++;
            int wlen = i - start;
            if(ft==FT_HLS && ed_is_hls_type(line+start,wlen)) {
                for(int j=start;j<i;j++) sh_putc(row,col_off+j,line[j],type_col);
            } else if(ed_is_kw(line+start,wlen,ft)) {
                for(int j=start;j<i;j++) sh_putc(row,col_off+j,line[j],kw_col);
            } else if((ft==FT_HLS||ft==FT_RASH) && wlen>1 && line[start]>='A' && line[start]<='Z') {
                /* CamelCase → type */
                for(int j=start;j<i;j++) sh_putc(row,col_off+j,line[j],type_col);
            } else {
                for(int j=start;j<i;j++) sh_putc(row,col_off+j,line[j],var_col);
            }
            continue;
        }

        /* asm! macro (HLS) */
        if(ft==FT_HLS && c=='a' && i+3<len && line[i]=='a' && line[i+1]=='s' && line[i+2]=='m' && line[i+3]=='!') {
            sh_putc(row,col_off+i,'a',COL_HLS_MACRO); sh_putc(row,col_off+i+1,'s',COL_HLS_MACRO);
            sh_putc(row,col_off+i+2,'m',COL_HLS_MACRO); sh_putc(row,col_off+i+3,'!',COL_HLS_MACRO);
            i+=4; continue;
        }

        if(ed_is_punct(c)) { sh_putc(row,col_off+i,c,pnc_col); i++; continue; }
        sh_putc(row,col_off+i,c,COL_RESET); i++;
    }
}

/* ── RSH highlighter ───────────────────────────────────────────────────── */
static void ed_hl_rsh(const char *line, int len, int row, int col_off) {
    int i=0;
    uint8_t kw_col=TH(keyword), str_col=TH(string_col), cmt_col=TH(comment);
    uint8_t num_col=TH(number_col), pnc_col=TH(punct), var_col=TH(var_col);
    if(ed_settings.bold_keywords) kw_col|=0x08;
    if(ed_settings.dim_comments)  cmt_col=0x01;

    while(i<len) {
        char c=line[i];
        if(c=='#') { for(int j=i;j<len;j++) sh_putc(row,col_off+j,line[j],cmt_col); break; }
        if(c=='"') { sh_putc(row,col_off+i,c,str_col); i++; while(i<len) { if(line[i]=='\\'&&i+1<len){sh_putc(row,col_off+i,line[i],str_col);i++;sh_putc(row,col_off+i,line[i],str_col);i++;continue;} sh_putc(row,col_off+i,line[i],str_col); if(line[i]=='"'){i++;break;} i++;} continue; }
        if(ed_is_digit(c)) { int s=i; while(i<len&&(ed_is_digit(line[i])||line[i]=='.'))i++; for(int j=s;j<i;j++)sh_putc(row,col_off+j,line[j],num_col); continue; }
        if(ed_is_alpha(c)||c=='_') { int s=i; while(i<len&&ed_is_ident(line[i]))i++; int wl=i-s; uint8_t cl=ed_is_kw(line+s,wl,FT_RSH)?kw_col:var_col;for(int j=s;j<i;j++)sh_putc(row,col_off+j,line[j],cl); continue; }
        if(c=='$'&&i+1<len&&ed_is_ident(line[i+1])) { sh_putc(row,col_off+i,c,var_col); i++; while(i<len&&ed_is_ident(line[i])){sh_putc(row,col_off+i,line[i],var_col);i++;} continue; }
        if(ed_is_punct(c)){sh_putc(row,col_off+i,c,pnc_col);i++;continue;}
        sh_putc(row,col_off+i,c,COL_RESET); i++;
    }
}

/* ── HTML highlighter (simplified) ─────────────────────────────────────── */
static void ed_hl_html(const char *line, int len, int row, int col_off) {
    int i=0;
    uint8_t cmt_col=COL_HTML_COMMENT, tag_col=COL_HTML_TAG, attr_col=COL_HTML_ATTR;
    uint8_t val_col=COL_HTML_VALUE, brk_col=COL_HTML_BRACKET;
    if(ed_settings.dim_comments) cmt_col=0x01;

    while(i<len) {
        char c=line[i];
        if(c=='<'&&i+3<len&&line[i+1]=='!'&&line[i+2]=='-'&&line[i+3]=='-') {
            int j=i; while(j<len){if(j+2<len&&line[j]=='-'&&line[j+1]=='-'&&line[j+2]=='>'){j+=3;break;}j++;}
            for(int k=i;k<j;k++)sh_putc(row,col_off+k,line[k],cmt_col); i=j; continue;
        }
        if(c=='<') {
            sh_putc(row,col_off+i,c,brk_col); i++;
            if(i<len&&line[i]=='/'){sh_putc(row,col_off+i,'/',brk_col);i++;}
            int s=i; while(i<len&&ed_is_tag_name(line[i]))i++;
            int wl=i-s; uint8_t tc=ed_in_list_nocase(line+s,wl,HTML_TAGS)?tag_col:COL_HTML_UNKNOWN;
            for(int j=s;j<i;j++)sh_putc(row,col_off+j,line[j],tc);
            while(i<len&&line[i]!='>'&&!(i+1<len&&line[i]=='/'&&line[i+1]=='>')) {
                if(ed_is_space(line[i])){sh_putc(row,col_off+i,line[i],COL_RESET);i++;}
                else if(line[i]=='='){sh_putc(row,col_off+i,'=',brk_col);i++;}
                else if(line[i]=='"'){sh_putc(row,col_off+i,'"',val_col);i++;while(i<len&&line[i]!='"'){sh_putc(row,col_off+i,line[i],val_col);i++;}if(i<len){sh_putc(row,col_off+i,'"',val_col);i++;}}
                else if(ed_is_alpha(line[i])||line[i]=='-'){int as=i;while(i<len&&(ed_is_ident(line[i])||line[i]=='-'))i++;for(int j=as;j<i;j++)sh_putc(row,col_off+j,line[j],attr_col);}
                else{sh_putc(row,col_off+i,line[i],brk_col);i++;}
            }
            if(i<len&&line[i]=='>'){sh_putc(row,col_off+i,'>',brk_col);i++;}
            continue;
        }
        if(c=='&'){int j=i;while(j<len&&line[j]!=';'&&line[j]!=' ')j++;if(j<len&&line[j]==';'){j++;for(int k=i;k<j;k++)sh_putc(row,col_off+k,line[k],COL_HTML_ENTITY);i=j;continue;}}
        if(c=='"'){sh_putc(row,col_off+i,c,val_col);i++;while(i<len&&line[i]!='"'){sh_putc(row,col_off+i,line[i],val_col);i++;}if(i<len){sh_putc(row,col_off+i,'"',val_col);i++;}continue;}
        sh_putc(row,col_off+i,c,COL_RESET); i++;
    }
}

/* ── Dispatch highlighter ──────────────────────────────────────────────── */
static void ed_hl_line(const char *line, int len, int row, int col_off) {
    if(!ed_settings.syntax_highlight) { for(int i=0;i<len;i++) sh_putc(row,col_off+i,line[i],COL_RESET); return; }
    switch(ed_ft) {
        case FT_RSH:  ed_hl_rsh(line,len,row,col_off); break;
        case FT_RASH: ed_hl_rust_like(line,len,row,col_off,FT_RASH); break;
        case FT_HLS:  ed_hl_rust_like(line,len,row,col_off,FT_HLS);  break;
        case FT_HTML: ed_hl_html(line,len,row,col_off); break;
        default: for(int i=0;i<len;i++) sh_putc(row,col_off+i,line[i],COL_RESET); break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Search
   ═══════════════════════════════════════════════════════════════════════════ */
static int ed_find_next(int start_row, int start_col) {
    if(ed_search_len==0) return 0;
    for(int r=start_row;r<ed_line_count;r++) {
        const char *l=ed_lines[r]; int ll=ed_strlen(l); int sc=(r==start_row)?start_col:0;
        for(int c=sc;c<=ll-ed_search_len;c++) if(ed_strncmp(l+c,ed_search,ed_search_len)==0){ed_search_row=r;ed_search_col=c;return 1;}
    }
    if(ed_settings.wrap_search) {
        for(int r=0;r<start_row;r++) {
            const char *l=ed_lines[r]; int ll=ed_strlen(l);
            for(int c=0;c<=ll-ed_search_len;c++) if(ed_strncmp(l+c,ed_search,ed_search_len)==0){ed_search_row=r;ed_search_col=c;return 1;}
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Cursor helpers
   ═══════════════════════════════════════════════════════════════════════════ */
static void ed_clamp(void) {
    if(ed_cur_row<0) ed_cur_row=0;
    if(ed_cur_row>=ed_line_count) ed_cur_row=ed_line_count-1;
    int ll=ed_strlen(ed_lines[ed_cur_row]);
    if(ed_cur_col<0) ed_cur_col=0;
    if(ed_cur_col>ll) ed_cur_col=ll;
}

static void ed_scroll_to_cursor(void) {
    int soff=ed_settings.scroll_off; if(soff>EDIT_ROWS/2) soff=EDIT_ROWS/2;
    if(ed_cur_row-ed_scroll<soff) ed_scroll=ed_cur_row-soff;
    if(ed_cur_row-ed_scroll>=EDIT_ROWS-soff) ed_scroll=ed_cur_row-EDIT_ROWS+soff+1;
    if(ed_scroll<0) ed_scroll=0;
    if(ed_scroll>ed_line_count-1) ed_scroll=ed_line_count>0?ed_line_count-1:0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Insert / delete
   ═══════════════════════════════════════════════════════════════════════════ */
static void ed_insert_char(char ch) {
    if(ed_line_count>=MAX_LINES) return;
    char *line=ed_lines[ed_cur_row];
    int ll=ed_strlen(line);              /* ← ADD THIS LINE */
    if(ll>=MAX_LINE_LEN-1) return;
    ed_memmove(line+ed_cur_col+1,line+ed_cur_col,ll-ed_cur_col+1);
    line[ed_cur_col]=ch; ed_cur_col++; ed_modified=true;
}

static void ed_delete_char(void) {
    char *line=ed_lines[ed_cur_row];
    int ll=ed_strlen(line);              /* ← ADD THIS LINE */
    if(ed_cur_col<ll) { ed_memmove(line+ed_cur_col,line+ed_cur_col+1,ll-ed_cur_col); ed_modified=true; }
    else if(ed_cur_row+1<ed_line_count) {
        int nl=ed_strlen(ed_lines[ed_cur_row+1]);
        if(ll+nl<MAX_LINE_LEN) { ed_strcpy(line+ll,ed_lines[ed_cur_row+1]); for(int i=ed_cur_row+1;i<ed_line_count-1;i++) ed_strcpy(ed_lines[i],ed_lines[i+1]); ed_line_count--; }
        ed_modified=true;
    }
}

static void ed_newline(void) {
    if(ed_line_count>=MAX_LINES) return;
    char *line=ed_lines[ed_cur_row]; (void)line;
    for(int i=ed_line_count;i>ed_cur_row+1;i--) ed_strcpy(ed_lines[i],ed_lines[i-1]);
    ed_strcpy(ed_lines[ed_cur_row+1],line+ed_cur_col);
    line[ed_cur_col]=0; ed_cur_row++; ed_cur_col=0;
    if(ed_settings.auto_indent) {
        char *prev=ed_lines[ed_cur_row-1]; int pi=0;
        while(pi<MAX_LINE_LEN&&(prev[pi]==' '||prev[pi]=='\t')) pi++;
        if(pi>0) { char *cur=ed_lines[ed_cur_row]; int cl=ed_strlen(cur); if(cl+pi<MAX_LINE_LEN){ed_memmove(cur+pi,cur,cl+1);for(int j=0;j<pi;j++)cur[j]=prev[j];ed_cur_col=pi;} }
    }
    ed_line_count++; ed_modified=true;
}

static void ed_delete_line(int row) {
    if(ed_line_count<=1){ed_lines[0][0]=0;ed_cur_col=0;return;}
    if(ed_clip.count<CLIP_LINES){ed_strcpy(ed_clip.lines[ed_clip.count],ed_lines[row]);ed_clip.count++;}
    for(int i=row;i<ed_line_count-1;i++) ed_strcpy(ed_lines[i],ed_lines[i+1]);
    ed_line_count--; ed_lines[ed_line_count][0]=0;
    if(ed_cur_row>=ed_line_count) ed_cur_row=ed_line_count-1;
    ed_cur_col=0; ed_modified=true;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Key → ASCII mapping
   ═══════════════════════════════════════════════════════════════════════════ */
static const char SCAN_MAP[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=',8,
    9,'q','w','e','r','t','y','u','i','o','p','[',']',13,
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',0,
    '\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,     /* 14 zeros = 128 total */
};
static const char SCAN_SHIFT[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+',8,
    9,'Q','W','E','R','T','Y','U','I','O','P','{','}',13,
    0,'A','S','D','F','G','H','J','K','L',':','"','~',0,
    '|','Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ',
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,     /* 14 zeros = 128 total */
};

/* ═══════════════════════════════════════════════════════════════════════════
   Matrix rain render
   ═══════════════════════════════════════════════════════════════════════════ */
static void rain_update(void) {
    if(!rain_inited) rain_init();
    rain_tick++;
    for(int c=0; c<RAIN_COLS; c++) {
        rain_cols[c].timer++;
        if(rain_cols[c].timer >= rain_cols[c].speed) {
            rain_cols[c].timer = 0;
            rain_cols[c].y++;
            if(rain_cols[c].y > TERM_H + rain_cols[c].len) {
                rain_cols[c].y   = -(int)(rain_rand() % 10);
                rain_cols[c].len = 4 + (int)(rain_rand() % 10);
            }
            rain_cols[c].chars[rain_cols[c].y % TERM_H] =
                RAIN_CHARS[rain_rand() % RAIN_CHAR_COUNT];
        }
    }
}

static void rain_draw(void) {
    for(int c=0; c<RAIN_COLS; c++) {
        int head = rain_cols[c].y;
        for(int t=0; t<rain_cols[c].len; t++) {
            int r = head - t;
            if(r < 0 || r >= TERM_H) continue;
            if(ed_shadow[r][c].ch != ' ') continue; /* only on bg cells */
            if(t == 0) {
                sh_putc(r, c, rain_cols[c].chars[r % TERM_H], TH(rain_head));
            } else {
                uint8_t a = TH(rain_col);
                if(t > rain_cols[c].len / 2) a = 0x02;
                sh_putc(r, c, rain_cols[c].chars[r % TERM_H], a);
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Main editor draw
   ═══════════════════════════════════════════════════════════════════════════ */
static const char* ft_name(FileType ft) {
    switch(ft) {
        case FT_RSH:  return "rsh";
        case FT_RASH: return "rash";
        case FT_HLS:  return "hls";
        case FT_HTML: return "html";
        default:      return "text";
    }
}

static const char* mode_name(EdMode m) {
    switch(m) {
        case MODE_NORMAL:   return "NORMAL";
        case MODE_INSERT:   return "INSERT";
        case MODE_VISUAL:   return "VISUAL";
        case MODE_SEARCH:   return "SEARCH";
        case MODE_COMMAND:  return "COMMAND";
        case MODE_SETTINGS: return "SETTINGS";
        default:            return "????";
    }
}

static uint8_t mode_status_col(void) {
    switch(ed_mode) {
        case MODE_NORMAL:  return TH(status_normal);
        case MODE_INSERT:  return TH(status_insert);
        case MODE_VISUAL:  return TH(status_visual);
        case MODE_SEARCH:  return TH(status_search);
        case MODE_COMMAND: return TH(status_command);
        default:           return TH(status_normal);
    }
}

static void ed_draw(void) {
    sh_clear();

    /* ── Settings mode ──────────────────────────────────────────────── */
    if(ed_mode == MODE_SETTINGS) {
        ed_draw_settings();
        if(matrix_rain_on) { rain_update(); rain_draw(); }
        sh_flush();
        return;
    }

    /* ── Line number gutter width ───────────────────────────────────── */
    int num_w = 0;
    if(ed_show_nums) {
        char tmp[8]; ed_itoa(ed_line_count > 0 ? ed_line_count : 1, tmp);
        num_w = ed_strlen(tmp) + 2;
        if(num_w < 4) num_w = 4;
    }

    /* ── Pre-scan block-comment state from file start to scroll ─────── */
    ed_in_block_comment = false;
    if((ed_ft == FT_RASH || ed_ft == FT_HLS) && ed_scroll > 0) {
        for(int pre = 0; pre < ed_scroll; pre++) {
            const char *l = ed_lines[pre];
            int ll = ed_strlen(l);
            for(int ci = 0; ci < ll; ci++) {
                if(ed_in_block_comment) {
                    if(ci+1 < ll && l[ci]=='*' && l[ci+1]=='/') {
                        ed_in_block_comment = false; ci++;
                    }
                } else {
                    if(ci+1 < ll && l[ci]=='/' && l[ci+1]=='*') {
                        ed_in_block_comment = true; ci++;
                    } else if(ci+1 < ll && l[ci]=='/' && l[ci+1]=='/') {
                        break; /* line comment, skip rest */
                    }
                }
            }
        }
    }

    /* ── Edit area ──────────────────────────────────────────────────── */
    for(int r = 0; r < EDIT_ROWS; r++) {
        int lr = ed_scroll + r;
        int col = 0;

        /* Line numbers */
        if(ed_show_nums) {
            char nbuf[8];
            if(ed_settings.relative_numbers) {
                int rel = lr - ed_cur_row; if(rel < 0) rel = -rel;
                ed_itoa(rel, nbuf);
            } else {
                ed_itoa(lr + 1, nbuf);
            }
            int nl = ed_strlen(nbuf);
            sh_pad(r, &col, num_w - nl - 1, TH(linenum));
            sh_puts(r, &col, nbuf, TH(linenum));
            sh_putc(r, col, ' ', TH(linenum)); col++;
            sh_putc(r, col, '|', TH(gutter_sep)); col++;
        }

        if(lr < ed_line_count) {
            const char *line = ed_lines[lr];
            int ll = ed_strlen(line);
            int text_w = TERM_W - col;

            /* Cursor line highlight */
            if(ed_settings.cursor_line_hl && lr == ed_cur_row) {
                uint8_t bg = TH(cursor_line_bg);
                for(int c=0; c<text_w; c++) sh_putc(r, col+c, c<ll ? line[c] : ' ', bg);
                ed_hl_line(line, ll, r, col);
            } else {
                ed_hl_line(line, ll, r, col);
                for(int c=ll; c<text_w; c++) sh_putc(r, col+c, ' ', COL_RESET);
            }

            /* Search highlight */
            if(ed_mode == MODE_SEARCH && ed_search_len > 0) {
                for(int c=0; c<=ll-ed_search_len; c++) {
                    if(ed_strncmp(line+c, ed_search, ed_search_len) == 0) {
                        for(int j=0; j<ed_search_len; j++)
                            ed_shadow[r][col+c+j].attr = TH(search_hl);
                    }
                }
            }

            /* Visual selection */
            if(ed_mode == MODE_VISUAL) {
                int vs = ed_vis_row, ve = ed_cur_row;
                if(vs > ve) { int t=vs; vs=ve; ve=t; }
                if(lr >= vs && lr <= ve) {
                    int sc = (lr == vs) ? ed_vis_col : 0;
                    int ec = (lr == ve) ? ed_cur_col : ll;
                    if(sc > ec) { int t=sc; sc=ec; ec=t; }
                    for(int c=sc; c<ec && col+c<TERM_W; c++)
                        ed_shadow[r][col+c].attr = TH(visual_sel);
                }
            }

            /* Col-80 ruler */
            if(ed_settings.show_ruler && col+80 < TERM_W) {
                ed_shadow[r][col+80].ch   = '|';
                ed_shadow[r][col+80].attr = 0x04;
            }

        } else {
            /* Empty line below content */
            for(int c=col; c<TERM_W; c++) sh_putc(r, c, ' ', COL_RESET);
        }
    }

    /* ── Cursor ──────────────────────────────────────────────────────── */
    {
        int cr = ed_cur_row - ed_scroll;
        int cc = (ed_show_nums ? num_w : 0) + ed_cur_col;
        if(cr >= 0 && cr < EDIT_ROWS && cc >= 0 && cc < TERM_W) {
            uint8_t ca = (ed_mode == MODE_INSERT) ? TH(cursor_insert) : TH(cursor_normal);
            ed_shadow[cr][cc].attr = ca;
            if(ed_shadow[cr][cc].ch == ' ')
                ed_shadow[cr][cc].ch = (ed_mode == MODE_INSERT) ? '_' : ' ';
        }
    }

    /* ── Status bar ──────────────────────────────────────────────────── */
    {
        int sr = STATUS_ROW;
        uint8_t sa = mode_status_col();
        int c = 0;

        /* Mode */
        sh_puts(sr, &c, " ", sa);
        sh_puts(sr, &c, mode_name(ed_mode), sa);
        sh_puts(sr, &c, " ", sa);
        sh_putc(sr, c, '|', TH(status_sep)); c++;

        /* File type */
        sh_puts(sr, &c, " ", sa);
        sh_puts(sr, &c, ft_name(ed_ft), sa);
        sh_puts(sr, &c, " ", sa);

        /* Path */
        const char *p = ed_path[0] ? ed_path : "[new]";
        int plen = ed_strlen(p);
        int path_space = TERM_W - c - 30;
        if(plen > path_space && path_space > 4) {
            sh_puts(sr, &c, "...", sa);
            sh_puts(sr, &c, p + plen - path_space + 3, sa);
        } else {
            sh_puts(sr, &c, p, sa);
        }

        /* Modified indicator */
        if(ed_modified)          sh_puts(sr, &c, " [+]", COL_MODIFIED);
        else if(ed_path[0])     sh_puts(sr, &c, " [~]", COL_SAVED);

        sh_pad(sr, &c, TERM_W - 20, sa);

        /* Row:Col */
        char rc[16];
        ed_itoa(ed_cur_row + 1, rc);
        sh_puts(sr, &c, rc, sa);
        sh_putc(sr, c++, ':', sa);
        ed_itoa(ed_cur_col + 1, rc);
        sh_puts(sr, &c, rc, sa);

        if(ed_settings.show_status_pct) {
            int pct = ed_line_count > 1 ? (ed_cur_row * 100 / (ed_line_count - 1)) : 0;
            sh_putc(sr, c++, ' ', sa);
            ed_itoa(pct, rc);
            sh_puts(sr, &c, rc, sa);
            sh_putc(sr, c++, '%', sa);
        }

        if(ed_settings.show_clock) {
            sh_putc(sr, c++, ' ', sa);
            char fc[12]; ed_itoa((int)(ed_frame & 0xFFFF), fc);
            sh_puts(sr, &c, fc, sa);
        }

        sh_pad(sr, &c, TERM_W, sa);
    }

    /* ── Hint bar / search bar / command bar ─────────────────────────── */
    {
        int hr = HINT_ROW;
        int c = 0;

        if(ed_mode == MODE_SEARCH) {
            sh_puts(hr, &c, "/", TH(search_prompt));
            sh_puts(hr, &c, ed_search, COL_RESET);
            sh_putc(hr, c++, '_', TH(cursor_insert));
            sh_pad(hr, &c, TERM_W, TH(hint_bg));
        } else if(ed_mode == MODE_COMMAND) {
            sh_puts(hr, &c, ":", TH(msg_col));
            sh_puts(hr, &c, ed_cmd, COL_RESET);
            sh_putc(hr, c++, '_', TH(cursor_insert));
            sh_pad(hr, &c, TERM_W, TH(hint_bg));
        } else {
            if(ed_msg[0]) {
                sh_puts(hr, &c, ed_msg, TH(msg_col));
            } else if(ed_settings.show_hints) {
                if(ed_mode == MODE_NORMAL)
                    sh_puts(hr, &c, "hjkl:Nav i:Ins x:Del dd:CutLine yy:Copy p:Paste /:Search ::Cmd S:Settings", TH(hint_key));
                else if(ed_mode == MODE_INSERT)
                    sh_puts(hr, &c, "Esc:Normal Ctrl+S:Save Ctrl+Z:Undo", TH(hint_key));
                else if(ed_mode == MODE_VISUAL)
                    sh_puts(hr, &c, "hjkl:Extend y:Yank d:Delete Esc:Cancel", TH(hint_key));
            }
            sh_pad(hr, &c, TERM_W, TH(hint_bg));
        }
    }

    /* Matrix rain overlay */
    if(matrix_rain_on) { rain_update(); rain_draw(); }

    sh_flush();
}

/* ═══════════════════════════════════════════════════════════════════════════
   Command execution
   ═══════════════════════════════════════════════════════════════════════════ */
static bool ed_want_quit = false;

static void ed_exec_cmd(const char *cmd) {
    if(ed_strcmp(cmd,"w")==0 || ed_strcmp(cmd,"write")==0) {
        if(ed_save_file()==0) ed_strcpy(ed_msg,"Saved");
        else ed_strcpy(ed_msg,"Save failed");
    } else if(ed_strcmp(cmd,"q")==0 || ed_strcmp(cmd,"quit")==0) {
        if(ed_modified && !ed_settings.save_on_exit) {
            ed_strcpy(ed_msg,"No write since last change (:q! to force)");
        } else {
            if(ed_settings.save_on_exit && ed_path[0]) ed_save_file();
            ed_want_quit = true;
        }
    } else if(ed_strcmp(cmd,"q!")==0) {
        ed_want_quit = true;
    } else if(ed_strcmp(cmd,"wq")==0 || ed_strcmp(cmd,"x")==0) {
        if(ed_save_file()==0) { ed_strcpy(ed_msg,"Saved"); ed_want_quit = true; }
        else ed_strcpy(ed_msg,"Save failed");
    } else if(ed_strcmp(cmd,"set")==0 || ed_strcmp(cmd,"settings")==0) {
        ed_mode = MODE_SETTINGS;
        set_cursor = set_item_to_list_idx(SET_ITEM_LINENUMS);
        set_theme_open = false;
    } else if(ed_strncmp(cmd,"e ",2)==0) {
        ed_strncpy(ed_path, cmd+2, MAX_PATH);
        ed_ft = ed_detect_ft(ed_path);
        if(avfs_get_filesize(ed_path) > 0) {
            ed_load(ed_path);
            ed_strcpy(ed_msg,"Opened: ");
            int ml = ed_strlen(ed_msg);
            ed_strncpy(ed_msg+ml, cmd+2, 80-ml);
        } else {
            ed_line_count = 1; ed_lines[0][0] = 0;
            ed_cur_row = ed_cur_col = ed_scroll = 0;
            ed_modified = false;
            ed_strcpy(ed_msg,"New file: ");
            int ml = ed_strlen(ed_msg);
            ed_strncpy(ed_msg+ml, cmd+2, 80-ml);
        }
    } else {
        ed_strcpy(ed_msg,"Unknown: ");
        int ml = ed_strlen(ed_msg);
        ed_strncpy(ed_msg+ml, cmd, 80-ml);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Key handler  —  returns false to quit
   ═══════════════════════════════════════════════════════════════════════════ */
static bool ed_handle_key(uint8_t sc) {
    if(sc & SC_RELEASE) {
        sc &= ~SC_RELEASE;
        if(sc==SC_LSHIFT||sc==SC_RSHIFT) ed_shift = false;
        else if(sc==SC_LCTRL) ed_ctrl = false;
        else if(sc==SC_CAPS) ed_caps = false;
        return true;
    }

    if(sc==SC_LSHIFT||sc==SC_RSHIFT) { ed_shift=true; return true; }
    if(sc==SC_LCTRL)                 { ed_ctrl=true;  return true; }
    if(sc==SC_CAPS)                  { ed_caps=true;  return true; }

    /* Easter egg: Ctrl+M / Ctrl+T / Ctrl+R */
    if(ed_ctrl) {
        char ch = SCAN_MAP[sc];
        if(ch=='m') ee_secret_m = true;
        if(ch=='t') ee_secret_t = true;
        if(ch=='r') ee_secret_r = true;
        if(ee_secret_m && ee_secret_t && ee_secret_r && !matrix_rain_on) {
            matrix_rain_on = true;
            ed_strcpy(ed_msg, "Wake up, Radium...");
        }
    }

    /* ── Settings mode ───────────────────────────────────────────── */
    if(ed_mode == MODE_SETTINGS) {
        if(sc == SC_ESC) {
            ed_mode = MODE_NORMAL; set_theme_open = false; return true;
        }
        if(set_theme_open) {
            if(sc==SC_UP)   { if(set_theme_sel>0) set_theme_sel--; }
            else if(sc==SC_DOWN) { if(set_theme_sel<NUM_THEMES-1) set_theme_sel++; }
            else if(sc==SC_ENTER) { ed_settings.theme_idx=set_theme_sel; ed_apply_settings(); set_theme_open=false; ed_strcpy(ed_msg,"Theme applied"); }
            else if(sc==SC_ESC) set_theme_open=false;
            return true;
        }
        if(sc==SC_UP)        set_cursor = set_next_sel(set_cursor, -1);
        else if(sc==SC_DOWN) set_cursor = set_next_sel(set_cursor, 1);
        else if(sc==SC_PGUP) { for(int i=0;i<5;i++) set_cursor=set_next_sel(set_cursor,-1); }
        else if(sc==SC_PGDN) { for(int i=0;i<5;i++) set_cursor=set_next_sel(set_cursor,1); }
        else if(sc==SC_ENTER||sc==SC_TAB) {
            if(set_cursor>=0 && set_cursor<SET_LIST_LEN && SET_ITEMS[set_cursor].label)
                ed_settings_toggle(SET_ITEMS[set_cursor].item_id);
        }
        else if(sc==SC_HOME) set_cursor = set_next_sel(0, 0);
        else if(sc==SC_END)  set_cursor = set_next_sel(SET_LIST_LEN-1, 0);
        return true;
    }

    /* ── Get ASCII character ──────────────────────────────────────── */
    char ch = ed_shift ? SCAN_SHIFT[sc] : SCAN_MAP[sc];
    if(ed_caps && ch>='a' && ch<='z') ch -= 32;
    else if(ed_caps && ch>='A' && ch<='Z') ch += 32;

    /* ── Search mode ──────────────────────────────────────────────── */
    if(ed_mode == MODE_SEARCH) {
        if(sc==SC_ESC)    { ed_mode=MODE_NORMAL; ed_search_len=0; return true; }
        if(sc==SC_ENTER)  { ed_mode=MODE_NORMAL; if(ed_search_len>0) ed_find_next(ed_cur_row,ed_cur_col); return true; }
        if(sc==SC_BACKSPACE) { if(ed_search_len>0) ed_search[--ed_search_len]=0; return true; }
        if(ch>=32 && ed_search_len<SEARCH_MAX) {
            ed_search[ed_search_len++]=ch; ed_search[ed_search_len]=0;
            ed_find_next(ed_cur_row, ed_cur_col);
            if(ed_search_row>ed_cur_row || (ed_search_row==ed_cur_row && ed_search_col>ed_cur_col)) {
                ed_cur_row=ed_search_row; ed_cur_col=ed_search_col;
            }
            return true;
        }
        return true;
    }

    /* ── Command mode ─────────────────────────────────────────────── */
    if(ed_mode == MODE_COMMAND) {
        if(sc==SC_ESC)    { ed_mode=MODE_NORMAL; ed_cmd_len=0; return true; }
        if(sc==SC_ENTER)  { ed_cmd[ed_cmd_len]=0; ed_exec_cmd(ed_cmd); ed_mode=MODE_NORMAL; ed_cmd_len=0; return !ed_want_quit; }
        if(sc==SC_BACKSPACE) { if(ed_cmd_len>0) ed_cmd[--ed_cmd_len]=0; return true; }
        if(ch>=32 && ed_cmd_len<63) { ed_cmd[ed_cmd_len++]=ch; return true; }
        return true;
    }

    /* ── Insert mode ──────────────────────────────────────────────── */
    if(ed_mode == MODE_INSERT) {
        if(sc==SC_ESC) {
            ed_mode=MODE_NORMAL; if(ed_cur_col>0) ed_cur_col--; return true;
        }
        if(ed_ctrl && ch=='s') { ed_save_file(); ed_strcpy(ed_msg,"Saved"); return true; }
        if(ed_ctrl && ch=='z') { ed_pop_undo(); return true; }
        if(sc==SC_BACKSPACE) {
            if(ed_cur_col>0) {
                ed_cur_col--;
                char *line=ed_lines[ed_cur_row];
                int ll=ed_strlen(line);          /* ← ADD THIS LINE */
                ed_memmove(line+ed_cur_col, line+ed_cur_col+1, ll-ed_cur_col+1);
                ed_modified=true;
            } else if(ed_cur_row>0) {
                ed_push_undo();
                ed_cur_row--;
                ed_cur_col = ed_strlen(ed_lines[ed_cur_row]);
                char *l1=ed_lines[ed_cur_row]; char *l2=ed_lines[ed_cur_row+1];
                int l1l=ed_strlen(l1); int l2l=ed_strlen(l2);
                if(l1l+l2l<MAX_LINE_LEN) {
                    ed_strcpy(l1+l1l, l2);
                    for(int i=ed_cur_row+1;i<ed_line_count-1;i++) ed_strcpy(ed_lines[i],ed_lines[i+1]);
                    ed_line_count--; ed_modified=true;
                }
            }
            return true;
        }
        if(sc==SC_ENTER)    { ed_push_undo(); ed_newline(); return true; }
        if(sc==SC_TAB) {
            ed_push_undo();
            if(ed_settings.tab_spaces) { for(int t=0;t<ed_settings.tab_width;t++) ed_insert_char(' '); }
            else ed_insert_char('\t');
            return true;
        }
        if(sc==SC_DEL)      { ed_delete_char(); return true; }
        if(sc==SC_UP)       { if(ed_cur_row>0) ed_cur_row--; return true; }
        if(sc==SC_DOWN)     { if(ed_cur_row<ed_line_count-1) ed_cur_row++; return true; }
        if(sc==SC_LEFT)     { if(ed_cur_col>0) ed_cur_col--; return true; }
        if(sc==SC_RIGHT)    { ed_cur_col++; return true; }
        if(sc==SC_HOME)     { ed_cur_col=0; return true; }
        if(sc==SC_END)      { ed_cur_col=ed_strlen(ed_lines[ed_cur_row]); return true; }
        if(sc==SC_PGUP)     { ed_cur_row-=EDIT_ROWS; if(ed_cur_row<0) ed_cur_row=0; return true; }
        if(sc==SC_PGDN)     { ed_cur_row+=EDIT_ROWS; if(ed_cur_row>=ed_line_count) ed_cur_row=ed_line_count-1; return true; }

        if(ch>=32 && ch<127) {
            ed_push_undo();
            ed_insert_char(ch);
            if(ed_settings.auto_pairs) {
                char pair = 0;
                if(ch=='(') pair=')';
                else if(ch=='[') pair=']';
                else if(ch=='{') pair='}';
                else if(ch=='"') pair='"';
                else if(ch=='\'') pair='\'';
                if(pair) { ed_insert_char(pair); ed_cur_col--; }
            }
            return true;
        }
        return true;
    }

    /* ── Visual mode ──────────────────────────────────────────────── */
    if(ed_mode == MODE_VISUAL) {
        if(sc==SC_ESC)    { ed_mode=MODE_NORMAL; return true; }
        if(sc==SC_UP)     { if(ed_cur_row>0) ed_cur_row--; return true; }
        if(sc==SC_DOWN)   { if(ed_cur_row<ed_line_count-1) ed_cur_row++; return true; }
        if(sc==SC_LEFT)   { if(ed_cur_col>0) ed_cur_col--; return true; }
        if(sc==SC_RIGHT)  { ed_cur_col++; return true; }
        if(ch=='y') {
            int vs=ed_vis_row, ve=ed_cur_row; if(vs>ve){int t=vs;vs=ve;ve=t;}
            ed_clip.count=0;
            for(int r=vs;r<=ve&&ed_clip.count<CLIP_LINES;r++) { ed_strcpy(ed_clip.lines[ed_clip.count],ed_lines[r]); ed_clip.count++; }
            ed_mode=MODE_NORMAL; ed_strcpy(ed_msg,"Yanked"); return true;
        }
        if(ch=='d') {
            int vs=ed_vis_row, ve=ed_cur_row; if(vs>ve){int t=vs;vs=ve;ve=t;}
            ed_push_undo();
            ed_clip.count=0;
            for(int r=vs;r<=ve&&ed_clip.count<CLIP_LINES;r++) { ed_strcpy(ed_clip.lines[ed_clip.count],ed_lines[r]); ed_clip.count++; }
            int count=ve-vs+1;
            for(int r=vs;r+count<ed_line_count;r++) ed_strcpy(ed_lines[r],ed_lines[r+count]);
            ed_line_count-=count; ed_cur_row=vs;
            if(ed_cur_row>=ed_line_count) ed_cur_row=ed_line_count-1;
            ed_cur_col=0; ed_mode=MODE_NORMAL; ed_modified=true; ed_strcpy(ed_msg,"Deleted"); return true;
        }
        return true;
    }

    /* ── Normal mode ──────────────────────────────────────────────── */
    if(ed_mode == MODE_NORMAL) {
        /* Vim motions */
        if(ch=='h'||sc==SC_LEFT)   { if(ed_cur_col>0) ed_cur_col--; return true; }
        if(ch=='l'||sc==SC_RIGHT)  { ed_cur_col++; return true; }
        if(ch=='j'||sc==SC_DOWN)   { if(ed_cur_row<ed_line_count-1) ed_cur_row++; return true; }
        if(ch=='k'||sc==SC_UP)     { if(ed_cur_row>0) ed_cur_row--; return true; }

        if(ch=='g' && !ed_g_pending) { ed_g_pending=true; return true; }
        if(ed_g_pending) {
            ed_g_pending=false;
            if(ch=='g') { ed_cur_row=0; ed_cur_col=0; }
            return true;
        }
        if(ch=='G') { ed_cur_row=ed_line_count-1; return true; }
        if(ch=='0'||sc==SC_HOME) { ed_cur_col=0; return true; }
        if(ch=='$'||sc==SC_END)  { ed_cur_col=ed_strlen(ed_lines[ed_cur_row]); return true; }
        if(ch=='^') {
            char *l=ed_lines[ed_cur_row]; int i=0;
            while(l[i]==' '||l[i]=='\t') i++;
            ed_cur_col=i; return true;
        }
        if(sc==SC_PGUP) { ed_cur_row-=EDIT_ROWS; if(ed_cur_row<0) ed_cur_row=0; return true; }
        if(sc==SC_PGDN) { ed_cur_row+=EDIT_ROWS; if(ed_cur_row>=ed_line_count) ed_cur_row=ed_line_count-1; return true; }

        if(ch=='w') {
            char *l=ed_lines[ed_cur_row]; int ll=ed_strlen(l); int c=ed_cur_col;
            while(c<ll&&ed_is_ident(l[c])) c++;
            while(c<ll&&ed_is_space(l[c])) c++;
            while(c<ll&&ed_is_ident(l[c])) c++;
            ed_cur_col=c; return true;
        }
        if(ch=='b') {
            char *l=ed_lines[ed_cur_row]; int c=ed_cur_col; if(c>0) c--;
            while(c>0&&l[c]==' ') c--;
            while(c>0&&ed_is_ident(l[c-1])) c--;
            ed_cur_col=c; return true;
        }

        /* Mode switches */
        if(ch=='i') { ed_mode=MODE_INSERT; ed_msg[0]=0; return true; }
        if(ch=='a') { ed_mode=MODE_INSERT; ed_cur_col++; ed_msg[0]=0; return true; }
        if(ch=='o') { ed_mode=MODE_INSERT; ed_push_undo(); ed_cur_col=ed_strlen(ed_lines[ed_cur_row]); ed_newline(); return true; }
        if(ch=='O') {
            ed_mode=MODE_INSERT; ed_push_undo();
            for(int i=ed_line_count;i>ed_cur_row;i--) ed_strcpy(ed_lines[i],ed_lines[i-1]);
            ed_lines[ed_cur_row][0]=0; ed_line_count++; ed_cur_col=0; return true;
        }
        if(ch=='v') { ed_mode=MODE_VISUAL; ed_vis_row=ed_cur_row; ed_vis_col=ed_cur_col; return true; }
        if(ch=='/'||ch=='?') { ed_mode=MODE_SEARCH; ed_search_len=0; ed_search[0]=0; return true; }
        if(ch==':') { ed_mode=MODE_COMMAND; ed_cmd_len=0; ed_cmd[0]=0; return true; }
        if(ch=='S') { ed_mode=MODE_SETTINGS; set_cursor=set_item_to_list_idx(SET_ITEM_LINENUMS); set_theme_open=false; return true; }

        /* Edits */
        if(ch=='x') { ed_push_undo(); ed_delete_char(); return true; }
        if(ch=='d') { ed_push_undo(); ed_delete_line(ed_cur_row); return true; }
        if(ch=='y') { ed_clip.count=1; ed_strcpy(ed_clip.lines[0],ed_lines[ed_cur_row]); ed_strcpy(ed_msg,"Yanked"); return true; }
        if(ch=='p') {
            ed_push_undo();
            for(int i=0;i<ed_clip.count;i++) {
                if(ed_line_count>=MAX_LINES) break;
                for(int j=ed_line_count;j>ed_cur_row+1+i;j--) ed_strcpy(ed_lines[j],ed_lines[j-1]);
                ed_strcpy(ed_lines[ed_cur_row+1+i],ed_clip.lines[i]);
                ed_line_count++;
            }
            ed_modified=true; return true;
        }
        if(ch=='u') { ed_pop_undo(); return true; }

        /* Ctrl shortcuts */
        if(ed_ctrl && ch=='s') { ed_save_file(); ed_strcpy(ed_msg,"Saved"); return true; }
        if(ed_ctrl && ch=='o') { ed_save_file(); ed_strcpy(ed_msg,"Saved"); return true; }
        if(ed_ctrl && ch=='g') { ed_mode=MODE_SETTINGS; set_cursor=0; set_theme_open=false; return true; }
        if(ed_ctrl && ch=='k') { ed_push_undo(); ed_delete_line(ed_cur_row); return true; }
        if(ed_ctrl && ch=='u') { ed_pop_undo(); return true; }

        /* Search next */
        if(ch=='n') {
            if(ed_search_len>0) {
                ed_find_next(ed_search_row, ed_search_col+1);
                ed_cur_row=ed_search_row; ed_cur_col=ed_search_col;
            }
            return true;
        }

        return true;
    }

    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Entry point
   ═══════════════════════════════════════════════════════════════════════════ */
void rshidt_command(int argc, char *argv[]) {
    ed_load_settings();

    ed_line_count  = 1;
    ed_cur_row     = 0;
    ed_cur_col     = 0;
    ed_scroll      = 0;
    ed_modified    = false;
    ed_mode        = MODE_NORMAL;
    ed_show_nums   = ed_settings.line_numbers;
    ed_shift       = false;
    ed_ctrl        = false;
    ed_caps        = false;
    ed_g_pending   = false;
    ed_search[0]   = 0;
    ed_search_len  = 0;
    ed_search_row  = 0;
    ed_search_col  = 0;
    ed_cmd[0]      = 0;
    ed_cmd_len     = 0;
    ed_msg[0]      = 0;
    ed_undo_head   = 0;
    ed_undo_count  = 0;
    ed_clip.count  = 0;
    ed_path[0]     = 0;
    ed_frame       = 0;
    ed_want_quit   = false;
    ed_in_block_comment = false;
    ed_strcpy(ed_lines[0], "");

    /* Easter egg state */
    ee_secret_m    = false;
    ee_secret_t    = false;
    ee_secret_r    = false;
    matrix_rain_on = false;
    rain_inited    = false;
    rain_tick      = 0;
    rain_rng_state = 0xDEADBEEF;

    if(argc >= 2) {
        ed_strncpy(ed_path, argv[1], MAX_PATH);
        ed_ft = ed_detect_ft(ed_path);
        if(avfs_get_filesize(ed_path) > 0) ed_load(ed_path);
        else                               ed_strcpy(ed_msg,"New file");
    } else {
        ed_ft = FT_TEXT;
        ed_strcpy(ed_msg,"No file -- :w <name> to save");
    }

    ed_push_undo();
    ed_draw();

    while(1) {
        uint8_t sc = ed_get_sc();
        bool cont  = ed_handle_key(sc);
        if(ed_mode != MODE_SETTINGS) {
            ed_clamp();
            ed_scroll_to_cursor();
        }
        ed_draw();
        if(!cont) break;
    }

    ed_save_settings();
    terminal_clear();
    reset_text_color();
}