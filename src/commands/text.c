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
   Supports: .rsh  .rash  .html/.htm  plain text
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

/* ── Syntax file types ─────────────────────────────────────────────────── */
typedef enum { FT_TEXT = 0, FT_RSH = 1, FT_RASH = 2, FT_HTML = 3 } FileType;

/* ═══════════════════════════════════════════════════════════════════════════
   Enhanced Theme System  (v5 — 26 fields per theme)
   ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    const char *name;           /* display name                    */
    const char *desc;           /* short description               */
    uint8_t  bg;                /* default background nibble       */
    uint8_t  status_normal;     /* status bar NORMAL colour        */
    uint8_t  status_insert;     /* status bar INSERT colour        */
    uint8_t  status_visual;     /* status bar VISUAL colour        */
    uint8_t  status_search;     /* status bar SEARCH colour        */
    uint8_t  status_command;    /* status bar COMMAND colour       */
    uint8_t  cursor_normal;     /* cursor in NORMAL mode           */
    uint8_t  cursor_insert;     /* cursor in INSERT  mode          */
    uint8_t  hint_bg;           /* hint bar background             */
    uint8_t  hint_key;          /* hint bar key colour             */
    uint8_t  visual_sel;        /* visual selection colour         */
    uint8_t  search_hl;         /* search highlight colour         */
    uint8_t  linenum;           /* line number colour              */
    uint8_t  keyword;           /* keyword colour                  */
    uint8_t  string_col;        /* string literal colour           */
    uint8_t  comment;           /* comment colour                  */
    uint8_t  var_col;           /* variable colour                 */
    uint8_t  number_col;        /* number colour                   */
    uint8_t  punct;             /* punctuation colour              */
    uint8_t  cursor_line_bg;    /* cursor line highlight bg nibble */
    /* ── v5 new fields ─────────────────────────────────────────── */
    uint8_t  gutter_sep;        /* '|' separator colour            */
    uint8_t  status_sep;        /* status bar separator colour     */
    uint8_t  search_prompt;     /* /search prompt colour           */
    uint8_t  msg_col;           /* status message text colour      */
    uint8_t  rain_col;          /* Matrix rain character colour    */
    uint8_t  rain_head;         /* Matrix rain leading char colour */
} EdTheme;

/* ── Built-in themes ─────────────────────────────────────────────────── */
#define NUM_THEMES  18

static const EdTheme THEMES[NUM_THEMES] = {
    /* 0: Radium Dark (default — fiery purple/red) */
    {
        "Radium Dark", "Default dark purple/red",
        0x0,
        0x57, 0x27, 0x17, 0x37, 0x47,
        0x50, 0x20,
        0x08, 0x0F,
        0x50, 0x6E,
        0x08,
        0x0D, 0x0A, 0x08, 0x0B, 0x0E, 0x09,
        0x10,
        0x38, 0x0F, 0x37, 0x0E, 0x02, 0x0A,
    },
    /* 1: Inferno (deep red/orange) */
    {
        "Inferno", "Volcanic red and orange",
        0x0,
        0x47, 0x27, 0x17, 0x67, 0x47,
        0x40, 0x20,
        0x04, 0x0C,
        0x40, 0x4E,
        0x04,
        0x0C, 0x0E, 0x04, 0x0C, 0x06, 0x0E,
        0x10,
        0x04, 0x0C, 0x47, 0x0C, 0x04, 0x0C,
    },
    /* 2: Ocean (blue/teal) */
    {
        "Ocean", "Deep blues and teals",
        0x0,
        0x17, 0x37, 0x57, 0x27, 0x67,
        0x10, 0x30,
        0x01, 0x0B,
        0x10, 0x3E,
        0x01,
        0x0B, 0x0A, 0x01, 0x03, 0x0E, 0x09,
        0x10,
        0x01, 0x0B, 0x17, 0x0A, 0x01, 0x03,
    },
    /* 3: Void (monochrome high-contrast) */
    {
        "Void", "Pure monochrome stark",
        0x0,
        0x78, 0x78, 0x78, 0x78, 0x78,
        0x70, 0x70,
        0x08, 0x0F,
        0x70, 0x70,
        0x07,
        0x0F, 0x07, 0x08, 0x07, 0x0F, 0x07,
        0x18,
        0x07, 0x0F, 0x78, 0x07, 0x08, 0x0F,
    },
    /* 4: Phosphor (green-screen retro) */
    {
        "Phosphor", "Classic green CRT",
        0x0,
        0x27, 0x27, 0x27, 0x27, 0x27,
        0x20, 0x20,
        0x02, 0x0A,
        0x20, 0x2E,
        0x02,
        0x0A, 0x02, 0x02, 0x0A, 0x0A, 0x02,
        0x22,
        0x02, 0x0A, 0x27, 0x0A, 0x02, 0x0A,
    },
    /* 5: Dracula (purple/pink) */
    {
        "Dracula", "Gothic purple and pink",
        0x0,
        0x57, 0x57, 0x57, 0x37, 0x57,
        0x50, 0x50,
        0x05, 0x0D,
        0x50, 0x5E,
        0x05,
        0x0D, 0x0A, 0x05, 0x0B, 0x0E, 0x09,
        0x15,
        0x05, 0x0D, 0x57, 0x0A, 0x02, 0x0A,
    },
    /* 6: Matrix */
    {
        "Matrix", "Digital rain green",
        0x0,
        0xA0, 0xA0, 0xA0, 0xA0, 0xA0,
        0xA0, 0xA0,
        0x00, 0x0A,
        0xA0, 0x2E,
        0x02,
        0x0A, 0x0A, 0x02, 0x0A, 0x0A, 0x02,
        0x10,
        0x02, 0x0A, 0xA0, 0x0A, 0x0A, 0x0F,
    },
    /* 7: Monokai */
    {
        "Monokai", "High-contrast pop",
        0x0,
        0x47, 0x27, 0x57, 0x67, 0x47,
        0xE0, 0xA0,
        0x08, 0x0E,
        0x50, 0x3E,
        0x08,
        0x0D, 0x0A, 0x06, 0x0E, 0x0B, 0x09,
        0x00,
        0x08, 0x0E, 0x47, 0x0A, 0x02, 0x0A,
    },
    /* 8: Solarized Dark */
    {
        "Solarized", "Eye-friendly blue base",
        0x1,
        0x1E, 0x1A, 0x1B, 0x1D, 0x1C,
        0xE0, 0xA0,
        0x18, 0x0E,
        0x30, 0x2E,
        0x08,
        0x0E, 0x0A, 0x06, 0x0D, 0x0B, 0x09,
        0x10,
        0x18, 0x0E, 0x1D, 0x0A, 0x02, 0x0A,
    },
    /* 9: Gruvbox Dark */
    {
        "Gruvbox", "Warm retro comfort",
        0x0,
        0x60, 0x20, 0xE0, 0x40, 0x60,
        0xE0, 0xA0,
        0x00, 0x0E,
        0x60, 0x28,
        0x08,
        0x0E, 0x0A, 0x08, 0x0B, 0x09, 0x0C,
        0x00,
        0x06, 0x0E, 0x60, 0x0A, 0x02, 0x0A,
    },
    /* 10: Nord */
    {
        "Nord", "Arctic bluish-purple",
        0x1,
        0x1D, 0x1B, 0x1A, 0x1E, 0x1C,
        0xD0, 0xB0,
        0x11, 0x0B,
        0xD0, 0x3E,
        0x18,
        0x0D, 0x0B, 0x18, 0x09, 0x0E, 0x0F,
        0x10,
        0x18, 0x0B, 0x1B, 0x0B, 0x01, 0x0B,
    },
    /* 11: Paper (light mode) */
    {
        "Paper", "Light mode / VS style",
        0x7,
        0x70, 0x70, 0x70, 0x70, 0x70,
        0x07, 0x07,
        0x70, 0x0F,
        0x70, 0x74,
        0x07,
        0x04, 0x02, 0x07, 0x01, 0x06, 0x05,
        0x78,
        0x78, 0x04, 0x70, 0x04, 0x02, 0x0A,
    },
    /* 12: TempleOS */
    {
        "TempleOS", "Terry Davis dedication",
        0x0,
        0xE0, 0xE0, 0xE0, 0xE0, 0xE0,
        0xE0, 0xE0,
        0x00, 0x0E,
        0xE0, 0xE6,
        0x07,
        0x0E, 0x0B, 0x08, 0x0B, 0x0E, 0x09,
        0x00,
        0x06, 0x0E, 0xE0, 0x0E, 0x0E, 0x0F,
    },
    /* 13: Postfix */
    {
        "Postfix", "Terminal email cyan/mag",
        0x0,
        0x30, 0x50, 0x30, 0x30, 0x50,
        0x30, 0x50,
        0x00, 0x0B,
        0x30, 0x3F,
        0x03,
        0x0D, 0x0B, 0x06, 0x0B, 0x0F, 0x05,
        0x10,
        0x03, 0x0B, 0x30, 0x0B, 0x03, 0x0B,
    },
    /* 14: Cyberpunk */
    {
        "Cyberpunk", "Neon dark pink/blue",
        0x0,
        0x50, 0x1D, 0x5D, 0xE0, 0x4D,
        0xD0, 0x10,
        0x10, 0x0E,
        0xD0, 0xE6,
        0x04,
        0x0D, 0x0B, 0x05, 0x09, 0x0E, 0x0F,
        0x18,
        0x04, 0x0E, 0x5D, 0x0D, 0x02, 0x0F,
    },
    /* 15: Abyss (deep-sea dark) */
    {
        "Abyss", "Deep-sea bioluminescence",
        0x0,
        0x17, 0x27, 0x37, 0x17, 0x47,
        0x10, 0x20,
        0x01, 0x0B,
        0x10, 0x1E,
        0x01,
        0x03, 0x0B, 0x01, 0x0B, 0x0E, 0x0B,
        0x11,
        0x01, 0x03, 0x17, 0x0B, 0x03, 0x0B,
    },
    /* 16: Ember (dimly lit amber) */
    {
        "Ember", "Warm amber embers",
        0x0,
        0x60, 0x60, 0x60, 0x60, 0x40,
        0x60, 0x60,
        0x00, 0x06,
        0x60, 0x6E,
        0x06,
        0x0E, 0x06, 0x04, 0x0E, 0x0C, 0x04,
        0x00,
        0x04, 0x0E, 0x60, 0x0E, 0x06, 0x0E,
    },
    /* 17: Necrotic (toxic sickly green) */
    {
        "Necrotic", "Toxic biohazard green",
        0x0,
        0x20, 0xA0, 0x60, 0x20, 0x40,
        0x20, 0xA0,
        0x00, 0x0A,
        0x20, 0x2E,
        0x02,
        0x0A, 0x02, 0x08, 0x0A, 0x0E, 0x02,
        0x00,
        0x02, 0x0A, 0x20, 0x0A, 0x0A, 0x0F,
    },
};

/* ── Active theme (index into THEMES) ─────────────────────────────────── */
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
    "progress","search",
    "map","area",
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
    "overflow-x","overflow-y","float","clear","flex","flex-direction","flex-wrap",
    "flex-flow","flex-grow","flex-shrink","flex-basis","justify-content",
    "align-items","align-self","align-content","grid","grid-template","grid-area",
    "grid-column","grid-row","gap","column-gap","row-gap","font","font-family",
    "font-size","font-weight","font-style","font-variant","line-height",
    "letter-spacing","word-spacing","text-align","text-decoration","text-transform",
    "text-shadow","text-overflow","white-space","vertical-align","list-style",
    "opacity","visibility","cursor","pointer-events","transition","animation",
    "transform","filter","box-shadow","content","counter-increment","counter-reset",
    "outline","resize","clip-path","mask","object-fit","object-position",
    "scroll-behavior","scroll-snap-type","aspect-ratio","writing-mode",
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
    "parseInt","parseFloat","isNaN","isFinite","setTimeout","setInterval",
    "clearTimeout","clearInterval","addEventListener","querySelector",
    "querySelectorAll","getElementById","createElement","appendChild",
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
   Settings panel state  (expanded — 35 total settings items)
   ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    /* ── Original 13 ─────────────────────────────────────────────────── */
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
    /* ── New 22 ──────────────────────────────────────────────────────── */
    bool   relative_numbers;      /* vim-style relative line numbers   */
    bool   show_eol;              /* show $ at end of line             */
    bool   trailing_ws_warn;      /* highlight trailing whitespace     */
    bool   bracket_match;         /* highlight matching bracket        */
    bool   word_wrap_indicator;   /* show > at column 80               */
    int    scroll_off;            /* scroll-off margin (0-10)          */
    bool   bold_keywords;         /* emulate bold via bright bit       */
    bool   dim_comments;          /* dim comments further              */
    bool   show_status_col;       /* col position in status bar        */
    bool   show_status_pct;       /* percentage in status bar          */
    bool   save_on_exit;          /* auto-save unsaved on :q           */
    bool   confirm_delete_line;   /* prompt before dd                  */
    bool   double_space_sentence; /* enter inserts two spaces after .  */
    int    undo_limit;            /* 16 / 32 / 64                      */
    bool   highlight_urls;        /* colour http:// sequences          */
    bool   show_ruler;            /* col-80 vertical separator         */
    bool   auto_pairs;            /* insert matching ) ] }             */
    bool   trim_trailing_save;    /* strip trailing spaces on save     */
    bool   insert_final_newline;  /* ensure file ends with \n          */
    bool   hard_wrap;             /* hard-wrap at col 80               */
    int    hard_wrap_col;         /* column for hard wrap (60/72/80)   */
    bool   show_clock;            /* display frame counter in status   */
} EdSettings;

/* ── Settings item IDs ─────────────────────────────────────────────────── */
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

/* ── Settings panel viewport ────────────────────────────────────────────── */
#define SET_VIEW_ROWS   18          /* visible item rows in the panel    */
static int       set_cursor     = 0;
static int       set_scroll     = 0;   /* top item index of viewport       */
static bool      set_theme_open = false;
static int       set_theme_sel  = 0;
static EdSettings ed_settings;

/* ═══════════════════════════════════════════════════════════════════════════
   Matrix Rain Easter Egg State
   Activated by: Ctrl+M AND Ctrl+T AND Ctrl+R (in any order)
   The rain draws on BACKGROUND cells only (empty space in gutter/line tails)
   ═══════════════════════════════════════════════════════════════════════════ */
static bool      ee_secret_m     = false;   /* Ctrl+M pressed at least once */
static bool      ee_secret_t     = false;   /* Ctrl+T pressed at least once */
static bool      ee_secret_r     = false;   /* Ctrl+R pressed at least once */
static bool      matrix_rain_on  = false;   /* all three unlocked           */

/* Rain column state */
#define RAIN_COLS  TERM_W
typedef struct {
    int  y;          /* current head row (0..TERM_H)   */
    int  speed;      /* 1 = every tick, 2 = every 2…   */
    int  timer;      /* countdown to next drop          */
    int  len;        /* tail length                     */
    char chars[TERM_H]; /* column character buffer      */
} RainCol;

static RainCol   rain_cols[RAIN_COLS];
static uint32_t  rain_tick    = 0;
static bool      rain_inited  = false;

/* simple LCG so we don't need stdlib rand */
static uint32_t rain_rng_state = 0xDEADBEEF;
static uint32_t rain_rand(void) {
    rain_rng_state = rain_rng_state * 1664525u + 1013904223u;
    return rain_rng_state;
}

/* Katakana-ish ASCII approximations for VGA codepage 437 */
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
static uint32_t  ed_frame = 0;    /* frame counter for clock */

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
    while(!ps2_ready()); return ps2_read();
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

/* ── Box-drawing using ASCII fallbacks ─────────────────────────────────── */
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

/* ── Double-line box using CP437 characters ─────────────────────────────── */
static void sh_dbl_box(int row, int col, int w, int h, uint8_t attr) {
    /* corners: C9 BB C8 BC   sides: CD BA */
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
static int ed_is_css_ident(char c){ return ed_is_ident(c)||c=='-'; }
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
    const char **list = (ft==FT_RSH) ? RSH_KW : RASH_KW;
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
static FileType ed_detect_ft(const char *path) {
    int len = ed_strlen(path);
    if(len>=5 && ed_strcmp(path+len-5,".rash")==0) return FT_RASH;
    if(len>=4 && ed_strcmp(path+len-4,".rsh" )==0) return FT_RSH;
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
        else if(ed_strcmp(key,"undolimit")==0)   {
            if(ival==16||ival==32||ival==64) ed_settings.undo_limit=ival;
        }
        else if(ed_strcmp(key,"hlurls")==0)      ed_settings.highlight_urls   =(val[0]=='1');
        else if(ed_strcmp(key,"ruler")==0)       ed_settings.show_ruler       =(val[0]=='1');
        else if(ed_strcmp(key,"autopairs")==0)   ed_settings.auto_pairs       =(val[0]=='1');
        else if(ed_strcmp(key,"trimsave")==0)    ed_settings.trim_trailing_save=(val[0]=='1');
        else if(ed_strcmp(key,"finalnl")==0)     ed_settings.insert_final_newline=(val[0]=='1');
        else if(ed_strcmp(key,"hardwrap")==0)    ed_settings.hard_wrap        =(val[0]=='1');
        else if(ed_strcmp(key,"hardwrapcol")==0) {
            if(ival==60||ival==72||ival==80) ed_settings.hard_wrap_col=ival;
        }
        else if(ed_strcmp(key,"clock")==0)       ed_settings.show_clock       =(val[0]=='1');
    }
}

static void ed_load_settings(void) {
    /* defaults */
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
    if(avfs_read_file(SETTINGS_PATH, set_io_buf, (uint32_t)sz, 0)!=0) {
        ed_apply_settings(); return;
    }
    set_io_buf[sz]=0;
    if(ed_strncmp(set_io_buf, "RSHIDT_SETTINGS_V", 17)!=0) {
        ed_apply_settings(); return;
    }
    ed_parse_settings(set_io_buf, sz);
    ed_apply_settings();
}

/* ═══════════════════════════════════════════════════════════════════════════
   Settings panel draw  (scrollable, double-border, themed)
   ═══════════════════════════════════════════════════════════════════════════ */
#define SET_BOX_W    64
#define SET_BOX_H    (SET_VIEW_ROWS + 8)   /* header(4) + items + sep + btn + hint */
#define SET_BOX_ROW  ((TERM_H - SET_BOX_H) / 2)
#define SET_BOX_COL  ((TERM_W - SET_BOX_W) / 2)

/* Settings panel colour scheme — uses active theme colours */
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
#define SET_CAT      0x0E   /* category divider colour */
#define SET_SCR_IND  0x0F   /* scroll indicator colour */
#define SET_ACTIVE   0x3F   /* active/unlocked easter egg colour */

/* ── Category divider helper ────────────────────────────────────────────── */
static void set_draw_divider(int row, int bc, const char *label, uint8_t attr) {
    int cc = bc+2;
    sh_putc(row, cc++, ' ', attr);
    for(const char *s=label; *s && cc<bc+SET_BOX_W-2; s++)
        sh_putc(row, cc++, *s, attr);
    sh_putc(row, cc++, ' ', attr);
    for(; cc<bc+SET_BOX_W-1; cc++) sh_putc(row, cc, '-', attr);
}

/* ── Toggle widget ──────────────────────────────────────────────────────── */
static void set_draw_toggle(int row, int col, bool val, bool selected) {
    uint8_t attr = selected ? SET_SEL : SET_NORMAL;
    uint8_t von  = selected ? SET_SEL : SET_VAL_ON;
    uint8_t voff = selected ? SET_SEL : SET_VAL_OFF;
    sh_putc(row, col,   '[', attr);
    if(val) {
        sh_putc(row, col+1, 'O', von);
        sh_putc(row, col+2, 'N', von);
        sh_putc(row, col+3, ']', attr);
    } else {
        sh_putc(row, col+1, 'O', voff);
        sh_putc(row, col+2, 'F', voff);
        sh_putc(row, col+3, 'F', voff);
        sh_putc(row, col+4, ']', attr);
    }
}

/* ── All settings items (label + id) ──────────────────────────────────── */
typedef struct { const char *label; int item_id; int is_cat; } SetItem;

static const SetItem SET_ITEMS[] = {
    /* Categories are drawn as dividers; item_id ignored for cats */
    { "-- Display --",                   -1,                    1 },
    { "  Line Numbers          ",         SET_ITEM_LINENUMS,     0 },
    { "  Relative Numbers      ",         SET_ITEM_RELNUMS,      0 },
    { "  Show Modified Flag    ",         SET_ITEM_SHOWMOD,      0 },
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
#define SET_LIST_LEN 44   /* total entries including category rows */

/* Count only actual selectable items */
static int set_item_to_list_idx(int item_id) {
    for(int i=0; SET_ITEMS[i].label; i++)
        if(!SET_ITEMS[i].is_cat && SET_ITEMS[i].item_id == item_id) return i;
    return 0;
}

/* Navigate: find next selectable item in direction */
static int set_next_sel(int cur_list, int dir) {
    int n = cur_list + dir;
    while(n >= 0 && n < SET_LIST_LEN && SET_ITEMS[n].label) {
        if(!SET_ITEMS[n].is_cat) return n;
        n += dir;
    }
    /* wrap */
    if(dir > 0) {
        for(int i=0;i<SET_LIST_LEN&&SET_ITEMS[i].label;i++)
            if(!SET_ITEMS[i].is_cat) return i;
    } else {
        for(int i=SET_LIST_LEN-1;i>=0;i--)
            if(SET_ITEMS[i].label && !SET_ITEMS[i].is_cat) return i;
    }
    return cur_list;
}

/* set_cursor stores list index directly */

static void ed_draw_settings(void) {
    int br = SET_BOX_ROW;
    int bc = SET_BOX_COL;

    /* Draw double-line box */
    sh_dbl_box(br, bc, SET_BOX_W, SET_BOX_H, SET_BORDER);

    /* ── Title ──────────────────────────────────────────────────────── */
    const char *title = " \xCE RSHIDT Settings \xCE ";
    int tlen = ed_strlen(title);
    int tc   = bc + (SET_BOX_W - tlen) / 2;
    for(int i=0;title[i];i++) sh_putc(br, tc+i, title[i], SET_TITLE);

    /* ── Subtitle ───────────────────────────────────────────────────── */
    const char *sub = " Arrows: Nav   Enter: Toggle   Esc: Close ";
    int slen = ed_strlen(sub);
    int sc2  = bc + (SET_BOX_W - slen) / 2;
    if(sc2<bc+1) sc2=bc+1;
    for(int i=0;sub[i]&&sc2+i<bc+SET_BOX_W-1;i++)
        sh_putc(br+1, sc2+i, sub[i], SET_KEY);

    /* ── Easter egg status row ──────────────────────────────────────── */
    {
        int er = br+2; int ec = bc+2;
        const char *ee = "  \x10 M:";
        for(const char *s=ee;*s&&ec<bc+SET_BOX_W-2;s++) sh_putc(er,ec++,*s,SET_KEY);
        sh_putc(er,ec++, ee_secret_m?'\xFB':'-', ee_secret_m?SET_ACTIVE:SET_VAL_OFF);
        sh_putc(er,ec++,' ',SET_KEY);
        const char *ee2="T:";
        for(const char *s=ee2;*s&&ec<bc+SET_BOX_W-2;s++) sh_putc(er,ec++,*s,SET_KEY);
        sh_putc(er,ec++, ee_secret_t?'\xFB':'-', ee_secret_t?SET_ACTIVE:SET_VAL_OFF);
        sh_putc(er,ec++,' ',SET_KEY);
        const char *ee3="R:";
        for(const char *s=ee3;*s&&ec<bc+SET_BOX_W-2;s++) sh_putc(er,ec++,*s,SET_KEY);
        sh_putc(er,ec++, ee_secret_r?'\xFB':'-', ee_secret_r?SET_ACTIVE:SET_VAL_OFF);
        sh_putc(er,ec++,' ',SET_KEY);
        if(matrix_rain_on) {
            const char *rn="  [RAIN: ACTIVE";
            for(const char *s=rn;*s&&ec<bc+SET_BOX_W-2;s++) sh_putc(er,ec++,*s,SET_ACTIVE);
        } else {
            const char *hint="  (Ctrl+M+T+R = ???)";
            for(const char *s=hint;*s&&ec<bc+SET_BOX_W-2;s++) sh_putc(er,ec++,*s,SET_VAL_OFF);
        }
    }

    /* ── Header separator ───────────────────────────────────────────── */
    for(int c=bc+1;c<bc+SET_BOX_W-1;c++) sh_putc(br+3,c,'\xCD',SET_BORDER);
    sh_putc(br+3,bc,'\xCC',SET_BORDER);
    sh_putc(br+3,bc+SET_BOX_W-1,'\xB9',SET_BORDER);

    /* ── Scroll viewport ────────────────────────────────────────────── */
    int view_start_row = br + 4;
    int n_list = 0;
    while(SET_ITEMS[n_list].label) n_list++;

    /* Ensure set_scroll is in range */
    if(set_scroll < 0) set_scroll = 0;
    if(set_scroll > n_list - SET_VIEW_ROWS && n_list > SET_VIEW_ROWS)
        set_scroll = n_list - SET_VIEW_ROWS;
    if(set_scroll < 0) set_scroll = 0;

    /* Scroll arrow indicators */
    if(set_scroll > 0) {
        sh_putc(view_start_row - 1, bc + SET_BOX_W/2, '\x1E', SET_SCR_IND); /* up arrow */
    }
    if(set_scroll + SET_VIEW_ROWS < n_list) {
        sh_putc(view_start_row + SET_VIEW_ROWS, bc + SET_BOX_W/2, '\x1F', SET_SCR_IND); /* down arrow */
    }

    for(int vi = 0; vi < SET_VIEW_ROWS; vi++) {
        int li = set_scroll + vi;
        if(li >= n_list || !SET_ITEMS[li].label) break;
        int row = view_start_row + vi;

        if(SET_ITEMS[li].is_cat) {
            /* Category divider */
            for(int c=bc+1;c<bc+SET_BOX_W-1;c++) sh_putc(row,c,' ',SET_VAL_OFF);
            set_draw_divider(row, bc, SET_ITEMS[li].label, SET_CAT);
            continue;
        }

        bool sel = (!set_theme_open && set_cursor == li);
        uint8_t attr = sel ? SET_SEL : SET_NORMAL;

        /* Row background */
        for(int c=bc+1; c<bc+SET_BOX_W-1; c++) sh_putc(row, c, ' ', attr);

        /* Label */
        int cc = bc+2;
        if(sel) { sh_putc(row, cc++, '\x10', attr); } /* arrow indicator */
        else    { sh_putc(row, cc++, ' ',    attr); }
        for(const char *s=SET_ITEMS[li].label; *s && cc<bc+38; s++)
            sh_putc(row, cc++, *s, attr);

        /* Value widget at column 40 */
        int vc = bc + 40;
        int item_id = SET_ITEMS[li].item_id;

        switch(item_id) {
            /* ── Simple toggles ──────────────────────────────────────── */
            case SET_ITEM_LINENUMS:     set_draw_toggle(row,vc,ed_settings.line_numbers,sel); break;
            case SET_ITEM_RELNUMS:      set_draw_toggle(row,vc,ed_settings.relative_numbers,sel); break;
            case SET_ITEM_AUTOINDENT:   set_draw_toggle(row,vc,ed_settings.auto_indent,sel); break;
            case SET_ITEM_TABSPACES:    set_draw_toggle(row,vc,ed_settings.tab_spaces,sel); break;
            case SET_ITEM_SHOWMOD:      set_draw_toggle(row,vc,ed_settings.show_modified,sel); break;
            case SET_ITEM_WRAPSEARCH:   set_draw_toggle(row,vc,ed_settings.wrap_search,sel); break;
            case SET_ITEM_SYNTAX:       set_draw_toggle(row,vc,ed_settings.syntax_highlight,sel); break;
            case SET_ITEM_CASESEARCH:   set_draw_toggle(row,vc,ed_settings.case_search,sel); break;
            case SET_ITEM_CURSORHL:     set_draw_toggle(row,vc,ed_settings.cursor_line_hl,sel); break;
            case SET_ITEM_HINTBAR:      set_draw_toggle(row,vc,ed_settings.show_hints,sel); break;
            case SET_ITEM_SMARTHOME:    set_draw_toggle(row,vc,ed_settings.smart_home,sel); break;
            case SET_ITEM_SHOWWS:       set_draw_toggle(row,vc,ed_settings.show_whitespace,sel); break;
            case SET_ITEM_SHOWEOL:      set_draw_toggle(row,vc,ed_settings.show_eol,sel); break;
            case SET_ITEM_TRAILWS:      set_draw_toggle(row,vc,ed_settings.trailing_ws_warn,sel); break;
            case SET_ITEM_BRACKETMATCH: set_draw_toggle(row,vc,ed_settings.bracket_match,sel); break;
            case SET_ITEM_WRAP80:       set_draw_toggle(row,vc,ed_settings.word_wrap_indicator,sel); break;
            case SET_ITEM_BOLDKW:       set_draw_toggle(row,vc,ed_settings.bold_keywords,sel); break;
            case SET_ITEM_DIMCOMMENT:   set_draw_toggle(row,vc,ed_settings.dim_comments,sel); break;
            case SET_ITEM_STATUS_COL:   set_draw_toggle(row,vc,ed_settings.show_status_col,sel); break;
            case SET_ITEM_STATUS_PCT:   set_draw_toggle(row,vc,ed_settings.show_status_pct,sel); break;
            case SET_ITEM_SAVE_EXIT:    set_draw_toggle(row,vc,ed_settings.save_on_exit,sel); break;
            case SET_ITEM_CONFIRM_DD:   set_draw_toggle(row,vc,ed_settings.confirm_delete_line,sel); break;
            case SET_ITEM_DBL_SPACE:    set_draw_toggle(row,vc,ed_settings.double_space_sentence,sel); break;
            case SET_ITEM_HL_URLS:      set_draw_toggle(row,vc,ed_settings.highlight_urls,sel); break;
            case SET_ITEM_RULER:        set_draw_toggle(row,vc,ed_settings.show_ruler,sel); break;
            case SET_ITEM_AUTO_PAIRS:   set_draw_toggle(row,vc,ed_settings.auto_pairs,sel); break;
            case SET_ITEM_TRIM_SAVE:    set_draw_toggle(row,vc,ed_settings.trim_trailing_save,sel); break;
            case SET_ITEM_FINAL_NL:     set_draw_toggle(row,vc,ed_settings.insert_final_newline,sel); break;
            case SET_ITEM_HARD_WRAP:    set_draw_toggle(row,vc,ed_settings.hard_wrap,sel); break;
            case SET_ITEM_CLOCK:        set_draw_toggle(row,vc,ed_settings.show_clock,sel); break;

            /* ── Numeric cyclers ──────────────────────────────────────── */
            case SET_ITEM_TABWIDTH: {
                char tw[4]; ed_itoa(ed_settings.tab_width, tw);
                sh_putc(row,vc,'<',attr); sh_putc(row,vc+1,' ',attr);
                sh_putc(row,vc+2,tw[0],sel?SET_SEL:SET_VAL_ON);
                sh_putc(row,vc+3,' ',attr); sh_putc(row,vc+4,'>',attr);
                break;
            }
            case SET_ITEM_SCROLLOFF: {
                char sv[4]; ed_itoa(ed_settings.scroll_off, sv);
                sh_putc(row,vc,'<',attr); sh_putc(row,vc+1,' ',attr);
                sh_putc(row,vc+2,sv[0],sel?SET_SEL:SET_VAL_ON);
                sh_putc(row,vc+3,' ',attr); sh_putc(row,vc+4,'>',attr);
                break;
            }
            case SET_ITEM_UNDO_LIMIT: {
                char uv[8]; ed_itoa(ed_settings.undo_limit, uv);
                sh_putc(row,vc,'<',attr); sh_putc(row,vc+1,' ',attr);
                int nc2=vc+2;
                for(int k=0;uv[k]&&nc2<bc+SET_BOX_W-3;k++)
                    sh_putc(row,nc2++,uv[k],sel?SET_SEL:SET_VAL_ON);
                sh_putc(row,nc2++,' ',attr); sh_putc(row,nc2,'>',attr);
                break;
            }
            case SET_ITEM_HARD_WRAP_COL: {
                char wv[8]; ed_itoa(ed_settings.hard_wrap_col, wv);
                sh_putc(row,vc,'<',attr); sh_putc(row,vc+1,' ',attr);
                int nc2=vc+2;
                for(int k=0;wv[k]&&nc2<bc+SET_BOX_W-3;k++)
                    sh_putc(row,nc2++,wv[k],sel?SET_SEL:SET_VAL_ON);
                sh_putc(row,nc2++,' ',attr); sh_putc(row,nc2,'>',attr);
                break;
            }

            /* ── Theme selector ──────────────────────────────────────── */
            case SET_ITEM_THEME: {
                const char *tn = THEMES[ed_settings.theme_idx].name;
                const char *td = THEMES[ed_settings.theme_idx].desc;
                sh_putc(row,vc,'[',attr);
                int nc2=vc+1;
                for(const char *s=tn;*s&&nc2<bc+SET_BOX_W-12;s++)
                    sh_putc(row,nc2++,*s,sel?SET_SEL:SET_VAL_ON);
                sh_putc(row,nc2++,']',attr);
                if(sel) {
                    sh_putc(row,nc2++,' ',attr);
                    for(const char *s=td;*s&&nc2<bc+SET_BOX_W-2;s++)
                        sh_putc(row,nc2++,*s,SET_VAL_OFF);
                }
                break;
            }
        }
    }

    /* ── Bottom separator ───────────────────────────────────────────── */
    int sep2 = br + 4 + SET_VIEW_ROWS;
    for(int c=bc+1;c<bc+SET_BOX_W-1;c++) sh_putc(sep2,c,'\xCD',SET_BORDER);
    sh_putc(sep2,bc,'\xCC',SET_BORDER);
    sh_putc(sep2,bc+SET_BOX_W-1,'\xB9',SET_BORDER);

    /* ── Save button ────────────────────────────────────────────────── */
    int btn_row = sep2 + 1;
    bool save_sel    = false; /* save is triggered by S key, not cursor */
    uint8_t save_col_a = SET_BTN;
    const char *save_lbl = "  [ S - Save Settings ]  [ Esc - Close ]  ";
    int slc = bc + (SET_BOX_W - ed_strlen(save_lbl)) / 2;
    if(slc < bc+1) slc = bc+1;
    for(const char *s=save_lbl;*s&&slc<bc+SET_BOX_W-1;s++)
        sh_putc(btn_row, slc++, *s, save_col_a);

    /* ── Hints row ──────────────────────────────────────────────────── */
    int hint_row = sep2 + 2;
    int hc = bc+2;
    const char *hints2[] = {
        "\x1E\x1F","Nav ",
        "Enter","Toggle ",
        "Left/Right","Cycle ",
        "S","Save ",
        "Esc","Close",
        NULL
    };
    for(int k=0;hints2[k];k+=2) {
        for(const char *s=hints2[k];*s&&hc<bc+SET_BOX_W-2;s++)
            sh_putc(hint_row,hc++,*s,SET_KEY);
        sh_putc(hint_row,hc++,':',SET_NORMAL);
        for(const char *s=hints2[k+1];*s&&hc<bc+SET_BOX_W-2;s++)
            sh_putc(hint_row,hc++,*s,SET_NORMAL);
        sh_putc(hint_row,hc++,' ',SET_NORMAL);
    }

    /* ── Theme dropdown overlay ─────────────────────────────────────── */
    if(set_theme_open) {
        int sel_li = set_item_to_list_idx(SET_ITEM_THEME);
        int dd_row = view_start_row + (sel_li - set_scroll);
        int dd_col = bc + 40;
        int dd_h   = NUM_THEMES + 2;
        int dd_w   = 30;

        if(dd_row + dd_h > TERM_H - 1) dd_row = TERM_H - 1 - dd_h;
        if(dd_row < 0) dd_row = 0;

        sh_dbl_box(dd_row, dd_col, dd_w, dd_h, SET_BORDER);
        const char *ddt = " \xCE Theme \xCE ";
        int ddtc = dd_col + (dd_w - ed_strlen(ddt))/2;
        for(const char *s=ddt;*s;s++) sh_putc(dd_row, ddtc++, *s, SET_TITLE);

        for(int t=0; t<NUM_THEMES; t++) {
            int tr = dd_row + 1 + t;
            bool tsel = (t == set_theme_sel);
            uint8_t ta = tsel ? SET_DD_SEL : SET_DD_BG;
            for(int c=dd_col+1; c<dd_col+dd_w-1; c++) sh_putc(tr, c, ' ', ta);
            int tc2 = dd_col+2;
            sh_putc(tr, tc2++, tsel?'\x10':' ', ta);
            sh_putc(tr, tc2++, ' ', ta);
            for(const char *s=THEMES[t].name;*s&&tc2<dd_col+dd_w-2;s++)
                sh_putc(tr, tc2++, *s, ta);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Matrix Rain — background layer  (only overwrites ' ' cells)
   ═══════════════════════════════════════════════════════════════════════════ */
static void rain_tick_and_draw(void) {
    if(!matrix_rain_on) return;
    if(!rain_inited) rain_init();

    rain_tick++;

    for(int c = 0; c < RAIN_COLS; c++) {
        RainCol *rc = &rain_cols[c];
        rc->timer--;
        if(rc->timer <= 0) {
            rc->timer = rc->speed;
            rc->y++;
            if(rc->y >= TERM_H + rc->len) {
                rc->y     = -(int)(rain_rand() % 8);
                rc->speed = 1 + (int)(rain_rand() % 3);
                rc->len   = 4 + (int)(rain_rand() % 12);
            }
            /* randomise a char in the column */
            int mutate = (int)(rain_rand() % TERM_H);
            rc->chars[mutate] = RAIN_CHARS[rain_rand() % RAIN_CHAR_COUNT];
        }

        /* Draw the column — head bright, tail fading */
        for(int segment = 0; segment < rc->len; segment++) {
            int ry = rc->y - segment;
            if(ry < 0 || ry >= TERM_H) continue;

            /* Only paint cells that are currently empty (space) */
            if(ed_shadow[ry][c].ch != ' ') continue;

            char  rch  = rc->chars[ry % TERM_H];
            uint8_t col;
            if(segment == 0)          col = TH(rain_head);
            else if(segment < 3)      col = TH(rain_col);
            else                      col = (TH(rain_col) & 0x07) | 0x00; /* dim tail */

            ed_shadow[ry][c].ch   = (uint8_t)rch;
            ed_shadow[ry][c].attr = col;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   CSS / JS / HTML sub-highlighters
   ═══════════════════════════════════════════════════════════════════════════ */
static void ed_colourise_css(const char *line, uint8_t *out, int len, int *pi) {
    int i = *pi;
    while(i < len) {
        char c = line[i];
        if(c=='<' && i+7<=len && ed_strncmp(line+i,"</style",7)==0) break;
        if(c=='/' && i+1<len && line[i+1]=='*') {
            while(i<len) {
                out[i]=COL_HTML_COMMENT;
                if(line[i]=='*' && i+1<len && line[i+1]=='/') { out[i+1]=COL_HTML_COMMENT; i+=2; break; }
                i++;
            } continue;
        }
        if(c=='@') {
            out[i++]=COL_HTML_CSSPROP;
            while(i<len && (ed_is_ident(line[i])||line[i]=='-')) out[i++]=COL_HTML_CSSPROP;
            continue;
        }
        if(c=='"'||c=='\'') {
            char d=c; out[i++]=COL_HTML_CSSVAL;
            while(i<len && line[i]!=d) out[i++]=COL_HTML_CSSVAL;
            if(i<len) out[i++]=COL_HTML_CSSVAL; continue;
        }
        if(c=='#' && i+1<len && ed_is_hex(line[i+1])) {
            out[i++]=COL_HTML_CSSVAL;
            while(i<len && ed_is_hex(line[i])) out[i++]=COL_HTML_CSSVAL; continue;
        }
        if(ed_is_digit(c)||(c=='-'&&i+1<len&&ed_is_digit(line[i+1]))) {
            out[i++]=COL_HTML_CSSVAL;
            while(i<len&&(ed_is_digit(line[i])||line[i]=='.')) out[i++]=COL_HTML_CSSVAL;
            if(i<len && (ed_is_alpha(line[i])||line[i]=='%'))
                while(i<len && (ed_is_alpha(line[i])||line[i]=='%')) out[i++]=COL_HTML_CSSVAL;
            continue;
        }
        if(ed_is_css_ident(c)) {
            int s=i;
            while(i<len && ed_is_css_ident(line[i])) i++;
            int pk=i; while(pk<len && ed_is_space(line[pk])) pk++;
            uint8_t col = (pk<len && line[pk]==':' && (pk+1>=len||line[pk+1]!=':'))
                          ? COL_HTML_CSSKEY : COL_HTML_CSSPROP;
            for(int k=s;k<i;k++) out[k]=col; continue;
        }
        if(c=='{'||c=='}'||c==':'||c==';'||c==',') { out[i++]=COL_PUNCT; continue; }
        out[i++]=COL_RESET;
    }
    *pi = i;
}

static void ed_colourise_js(const char *line, uint8_t *out, int len, int *pi) {
    int i = *pi;
    while(i < len) {
        char c = line[i];
        if(c=='<' && i+8<=len && ed_strncmp(line+i,"</script",8)==0) break;
        if(c=='/' && i+1<len && line[i+1]=='/') { while(i<len) out[i++]=COL_HTML_JSCOMMENT; break; }
        if(c=='/' && i+1<len && line[i+1]=='*') {
            while(i<len) {
                out[i]=COL_HTML_JSCOMMENT;
                if(line[i]=='*' && i+1<len && line[i+1]=='/') { out[i+1]=COL_HTML_JSCOMMENT; i+=2; break; }
                i++;
            } continue;
        }
        if(c=='`') {
            out[i++]=COL_HTML_JSSTR;
            while(i<len && line[i]!='`') out[i++]=COL_HTML_JSSTR;
            if(i<len) out[i++]=COL_HTML_JSSTR; continue;
        }
        if(c=='"'||c=='\'') {
            char d=c; out[i++]=COL_HTML_JSSTR;
            while(i<len) {
                if(line[i]=='\\' && i+1<len) { out[i++]=COL_HTML_JSSTR; out[i++]=COL_HTML_JSSTR; }
                else if(line[i]==d) { out[i++]=COL_HTML_JSSTR; break; }
                else out[i++]=COL_HTML_JSSTR;
            } continue;
        }
        if(c=='0' && i+1<len && line[i+1]=='x') {
            out[i++]=COL_HTML_JSNUM; out[i++]=COL_HTML_JSNUM;
            while(i<len && ed_is_hex(line[i])) out[i++]=COL_HTML_JSNUM; continue;
        }
        if(ed_is_digit(c)) { while(i<len && (ed_is_digit(line[i])||line[i]=='.')) out[i++]=COL_HTML_JSNUM; continue; }
        if(ed_is_ident(c)) {
            int s=i; while(i<len && ed_is_ident(line[i])) i++;
            uint8_t col = ed_in_list(line+s, i-s, JS_KW) ? COL_HTML_JSKW : COL_RESET;
            for(int k=s;k<i;k++) out[k]=col; continue;
        }
        if(ed_is_punct(c)) { out[i++]=COL_PUNCT; continue; }
        out[i++]=COL_RESET;
    }
    *pi = i;
}

static void ed_colourise_html(const char *line, uint8_t *out, int len) {
    int i=0;
    for(int k=0;k<len;k++) out[k]=COL_RESET;
    while(i<len) {
        char c=line[i];
        if(c=='<' && i+3<len && line[i+1]=='!' && line[i+2]=='-' && line[i+3]=='-') {
            while(i<len) {
                out[i]=COL_HTML_COMMENT;
                if(i+2<len && line[i]=='-' && line[i+1]=='-' && line[i+2]=='>') {
                    out[i+1]=COL_HTML_COMMENT; out[i+2]=COL_HTML_COMMENT; i+=3; break;
                } i++;
            } continue;
        }
        if(c=='<' && i+1<len && line[i+1]=='!') {
            while(i<len) { out[i]=COL_HTML_DOCTYPE; if(line[i]=='>'){i++;break;} i++; } continue;
        }
        if(c=='<' && i+6<len && ed_strncmp(line+i+1,"script",6)==0 &&
           (ed_is_space(line[i+7])||line[i+7]=='>'||line[i+7]=='/')) {
            out[i++]=COL_HTML_BRACKET;
            int ts=i; while(i<len && ed_is_tag_name(line[i])) i++;
            for(int k=ts;k<i;k++) out[k]=COL_HTML_TAG;
            while(i<len && line[i]!='>') {
                if(line[i]=='"'||line[i]=='\'') {
                    char d=line[i]; out[i++]=COL_HTML_VALUE;
                    while(i<len && line[i]!=d) out[i++]=COL_HTML_VALUE;
                    if(i<len) out[i++]=COL_HTML_VALUE;
                } else if(ed_is_ident(line[i])) {
                    int as=i; while(i<len && ed_is_ident(line[i])) i++;
                    for(int k=as;k<i;k++) out[k]=COL_HTML_ATTR;
                } else if(line[i]=='=') { out[i++]=COL_HTML_EQUALS; }
                else { out[i++]=COL_RESET; }
            }
            if(i<len) out[i++]=COL_HTML_BRACKET;
            ed_colourise_js(line, out, len, &i);
            if(i<len && line[i]=='<') {
                out[i++]=COL_HTML_BRACKET;
                if(i<len && line[i]=='/') out[i++]=COL_HTML_BRACKET;
                int ts2=i; while(i<len && ed_is_tag_name(line[i])) i++;
                for(int k=ts2;k<i;k++) out[k]=COL_HTML_TAG;
                if(i<len && line[i]=='>') out[i++]=COL_HTML_BRACKET;
            }
            continue;
        }
        if(c=='<' && i+5<len && ed_strncmp(line+i+1,"style",5)==0 &&
           (ed_is_space(line[i+6])||line[i+6]=='>'||line[i+6]=='/')) {
            out[i++]=COL_HTML_BRACKET;
            int ts=i; while(i<len && ed_is_tag_name(line[i])) i++;
            for(int k=ts;k<i;k++) out[k]=COL_HTML_TAG;
            while(i<len && line[i]!='>') {
                if(line[i]=='"'||line[i]=='\'') {
                    char d=line[i]; out[i++]=COL_HTML_VALUE;
                    while(i<len && line[i]!=d) out[i++]=COL_HTML_VALUE;
                    if(i<len) out[i++]=COL_HTML_VALUE;
                } else if(ed_is_ident(line[i])) {
                    int as=i; while(i<len && ed_is_ident(line[i])) i++;
                    for(int k=as;k<i;k++) out[k]=COL_HTML_ATTR;
                } else if(line[i]=='=') { out[i++]=COL_HTML_EQUALS; }
                else { out[i++]=COL_RESET; }
            }
            if(i<len) out[i++]=COL_HTML_BRACKET;
            ed_colourise_css(line, out, len, &i);
            if(i<len && line[i]=='<') {
                out[i++]=COL_HTML_BRACKET;
                if(i<len && line[i]=='/') out[i++]=COL_HTML_BRACKET;
                int ts2=i; while(i<len && ed_is_tag_name(line[i])) i++;
                for(int k=ts2;k<i;k++) out[k]=COL_HTML_TAG;
                if(i<len && line[i]=='>') out[i++]=COL_HTML_BRACKET;
            }
            continue;
        }
        if(c=='<') {
            out[i++]=COL_HTML_BRACKET;
            if(i<len && line[i]=='/') out[i++]=COL_HTML_BRACKET;
            int ts=i; while(i<len && ed_is_tag_name(line[i])) i++;
            int tlen2=i-ts;
            uint8_t tcol = ed_in_list_nocase(line+ts,tlen2,HTML_TAGS) ? COL_HTML_TAG : COL_HTML_UNKNOWN;
            for(int k=ts;k<i;k++) out[k]=tcol;
            while(i<len && line[i]!='>') {
                if(line[i]=='/') { out[i++]=COL_HTML_BRACKET; continue; }
                if(line[i]=='"'||line[i]=='\'') {
                    char d=line[i]; out[i++]=COL_HTML_VALUE;
                    while(i<len && line[i]!=d) {
                        if(line[i]=='\\' && i+1<len) out[i++]=COL_HTML_VALUE;
                        out[i++]=COL_HTML_VALUE;
                    }
                    if(i<len) out[i++]=COL_HTML_VALUE; continue;
                }
                if(line[i]=='=') { out[i++]=COL_HTML_EQUALS; continue; }
                if(ed_is_tag_name(line[i])) {
                    int as=i; while(i<len && ed_is_tag_name(line[i])) i++;
                    int alen=i-as;
                    uint8_t acol;
                    if(ed_in_list(line+as,alen,HTML_EVENT_ATTRS))      acol=COL_HTML_JSSTR;
                    else if(ed_in_list(line+as,alen,HTML_BOOL_ATTRS))  acol=COL_HTML_ENTITY;
                    else                                                 acol=COL_HTML_ATTR;
                    for(int k=as;k<i;k++) out[k]=acol; continue;
                }
                out[i++]=COL_RESET;
            }
            if(i<len) out[i++]=COL_HTML_BRACKET; continue;
        }
        if(c=='&') {
            out[i++]=COL_HTML_ENTITY;
            while(i<len && line[i]!=';' && !ed_is_space(line[i]) && line[i]!='<')
                out[i++]=COL_HTML_ENTITY;
            if(i<len && line[i]==';') out[i++]=COL_HTML_ENTITY; continue;
        }
        /* URL highlighting */
        if(ed_settings.highlight_urls && c=='h' && i+7<len &&
           (ed_strncmp(line+i,"http://",7)==0 || ed_strncmp(line+i,"https://",8)==0)) {
            while(i<len && !ed_is_space(line[i]) && line[i]!='<' && line[i]!='"')
                out[i++] = 0x0B; /* cyan */
            continue;
        }
        out[i++]=COL_RESET;
    }
}

static void ed_colourise_code(const char *line, uint8_t *out, int len, FileType ft) {
    int i = 0;
    while (i < len) {
        char c = line[i];
        /* URL highlighting in code */
        if(ed_settings.highlight_urls && c=='h' && i+7<len &&
           (ed_strncmp(line+i,"http://",7)==0 || ed_strncmp(line+i,"https://",8)==0)) {
            while(i<len && !ed_is_space(line[i])) out[i++] = 0x0B;
            continue;
        }
        if (c == '#' || (ft == FT_RASH && c == '/' && i + 1 < len && line[i + 1] == '/')) {
            while (i < len) {
                uint8_t cc = ed_settings.dim_comments ? (COL_COMMENT & 0x07) : COL_COMMENT;
                out[i++] = cc;
            }
            break;
        }
        if (ft == FT_RASH && c == '/' && i + 1 < len && line[i + 1] == '*') {
            while (i < len) {
                uint8_t cc = ed_settings.dim_comments ? (COL_COMMENT & 0x07) : COL_COMMENT;
                out[i++] = cc;
            }
            break;
        }
        if (c == '"' || c == '\'') {
            char delim = c; out[i++] = COL_STRING;
            while (i < len) {
                if (line[i] == '\\' && i + 1 < len) { out[i++] = COL_STRING; out[i++] = COL_STRING; }
                else if (line[i] == delim) { out[i++] = COL_STRING; break; }
                else { out[i++] = COL_STRING; }
            } continue;
        }
        if (c == '$') {
            out[i++] = COL_VAR;
            if (i < len && line[i] == '{') {
                out[i++] = COL_VAR;
                while (i < len && line[i] != '}') out[i++] = COL_VAR;
                if (i < len) out[i++] = COL_VAR;
            } else {
                while (i < len && ed_is_ident(line[i])) out[i++] = COL_VAR;
            } continue;
        }
        if (c == '0' && i + 1 < len && line[i + 1] == 'x') {
            out[i++] = COL_NUMBER; out[i++] = COL_NUMBER;
            while (i < len && (ed_is_hex(line[i]) || line[i] == '_')) out[i++] = COL_NUMBER;
            continue;
        }
        if (ed_is_digit(c)) {
            while (i < len && (ed_is_digit(line[i]) || line[i] == '.' || line[i] == '_'))
                out[i++] = COL_NUMBER;
            continue;
        }
        if (c == '^' && ft == FT_RSH) {
            int s = i; i++;
            while (i < len && ed_is_ident(line[i])) i++;
            uint8_t col = ed_is_kw(line + s, i - s, ft) ? COL_KEYWORD : COL_RESET;
            for (int j = s; j < i; j++) out[j] = col; continue;
        }
        if (ed_is_ident(c)) {
            int s = i;
            while (i < len && (ed_is_ident(line[i]) || line[i] == '.')) i++;
            int id_len = i - s;
            uint8_t col = COL_RESET;
            if (ed_is_kw(line + s, id_len, ft)) {
                col = COL_KEYWORD;
                /* bold emulation: set bright bit on keyword fg */
                if(ed_settings.bold_keywords) col = col | 0x08;
            } else {
                int next = i;
                while (next < len && line[next] == ' ') next++;
                if (next < len && line[next] == ':') col = COL_VAR;
            }
            for (int j = s; j < i; j++) out[j] = col; continue;
        }
        if (ed_is_punct(c)) { out[i++] = COL_PUNCT; continue; }
        out[i++] = COL_RESET;
    }
    /* Trailing whitespace warning */
    if(ed_settings.trailing_ws_warn && len > 0) {
        int te = len - 1;
        while(te >= 0 && (line[te]==' '||line[te]=='\t')) te--;
        for(int k=te+1;k<len;k++) out[k] = 0x4F; /* red bg */
    }
}

static void ed_colourise(const char *line, uint8_t *out, int len, FileType ft) {
    if(!ed_settings.syntax_highlight) {
        for(int i=0;i<len;i++) out[i]=COL_RESET;
        return;
    }
    if(ft==FT_HTML) ed_colourise_html(line, out, len);
    else            ed_colourise_code(line, out, len, ft);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Undo
   ═══════════════════════════════════════════════════════════════════════════ */
static void ed_undo_push(void) {
    UndoState *u = &ed_undo[ed_undo_head % UNDO_DEPTH];
    for(int i=0;i<ed_line_count;i++) ed_strcpy(u->lines[i], ed_lines[i]);
    u->line_count = ed_line_count;
    u->cur_row    = ed_cur_row;
    u->cur_col    = ed_cur_col;
    ed_undo_head++;
    if(ed_undo_count < UNDO_DEPTH) ed_undo_count++;
}
static void ed_undo_pop(void) {
    if(ed_undo_count == 0) { ed_strcpy(ed_msg, "Nothing to undo"); return; }
    ed_undo_head--; ed_undo_count--;
    UndoState *u = &ed_undo[ed_undo_head % UNDO_DEPTH];
    for(int i=0;i<u->line_count;i++) ed_strcpy(ed_lines[i], u->lines[i]);
    ed_line_count = u->line_count;
    ed_cur_row    = u->cur_row;
    ed_cur_col    = u->cur_col;
    ed_modified   = true;
    ed_strcpy(ed_msg, "Undo");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Cursor / scroll
   ═══════════════════════════════════════════════════════════════════════════ */
static void ed_clamp(void) {
    if(ed_cur_row < 0)              ed_cur_row = 0;
    if(ed_cur_row >= ed_line_count) ed_cur_row = ed_line_count-1;
    int ll = ed_strlen(ed_lines[ed_cur_row]);
    int max_col = (ed_mode==MODE_INSERT) ? ll : (ll>0?ll-1:0);
    if(ed_cur_col < 0)       ed_cur_col = 0;
    if(ed_cur_col > max_col) ed_cur_col = max_col;
}
static void ed_scroll_to_cursor(void) {
    int soff = ed_settings.scroll_off;
    if(ed_cur_row - soff < ed_scroll)              ed_scroll = ed_cur_row - soff;
    if(ed_cur_row + soff >= ed_scroll + EDIT_ROWS) ed_scroll = ed_cur_row + soff - EDIT_ROWS + 1;
    if(ed_scroll < 0) ed_scroll = 0;
}
static int ed_line_len(int r) { return ed_strlen(ed_lines[r]); }

/* ═══════════════════════════════════════════════════════════════════════════
   Visual / search helpers
   ═══════════════════════════════════════════════════════════════════════════ */
static void ed_vis_ordered(int *r0, int *c0, int *r1, int *c1) {
    if(ed_vis_row < ed_cur_row || (ed_vis_row==ed_cur_row && ed_vis_col<=ed_cur_col)) {
        *r0=ed_vis_row; *c0=ed_vis_col; *r1=ed_cur_row; *c1=ed_cur_col;
    } else {
        *r0=ed_cur_row; *c0=ed_cur_col; *r1=ed_vis_row; *c1=ed_vis_col;
    }
}
static bool ed_in_visual(int r, int c) {
    if(ed_mode!=MODE_VISUAL) return false;
    int r0,c0,r1,c1; ed_vis_ordered(&r0,&c0,&r1,&c1);
    if(r<r0||r>r1) return false;
    if(r==r0&&r==r1) return c>=c0&&c<=c1;
    if(r==r0) return c>=c0;
    if(r==r1) return c<=c1;
    return true;
}
static bool ed_in_search_match(int r, int c) {
    if(ed_search_len==0) return false;
    int sl=ed_strlen(ed_search);
    if(r!=ed_search_row) return false;
    return c>=ed_search_col && c<ed_search_col+sl;
}

/* ── Gutter width calculation ───────────────────────────────────────────── */
static int ed_gutter_width(void) {
    if(!ed_show_nums) return 0;
    int n = ed_line_count; int g = 2;
    while(n>0){g++;n/=10;}
    return g;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Draw (main editor)
   ═══════════════════════════════════════════════════════════════════════════ */
static void ed_draw(void) {
    ed_frame++;
    sh_clear();

    int gutter = ed_gutter_width();

    static uint8_t col_buf[MAX_LINE_LEN];

    /* ── Editor rows ───────────────────────────────────────────────────── */
    for(int sr=0; sr<EDIT_ROWS; sr++) {
        int br = ed_scroll + sr;
        int sc = 0;

        if(ed_show_nums) {
            if(br < ed_line_count) {
                int display_num;
                if(ed_settings.relative_numbers && br != ed_cur_row)
                    display_num = (br > ed_cur_row) ? (br - ed_cur_row) : (ed_cur_row - br);
                else
                    display_num = br + 1;

                char nb[8]; ed_itoa(display_num, nb);
                int nlen = ed_strlen(nb);
                int pad  = gutter-2-nlen;
                for(int p=0;p<pad;p++) sh_putc(sr, sc++, ' ', TH(linenum));
                for(int p=0;p<nlen;p++) sh_putc(sr, sc++, nb[p], TH(linenum));
                sh_putc(sr, sc++, ' ', TH(linenum));
                sh_putc(sr, sc++, TH(gutter_sep), TH(linenum));
            } else {
                sh_putc(sr, sc++, '~', TH(linenum));
                for(int p=1;p<gutter;p++) sh_putc(sr, sc++, ' ', TH(linenum));
            }
        }

        if(br >= ed_line_count) continue;

        const char *line = ed_lines[br];
        int ll = ed_strlen(line);

        /* col-80 ruler */
        if(ed_settings.show_ruler) {
            int ruler_sc = gutter + 79;
            if(ruler_sc < TERM_W)
                sh_putc(sr, ruler_sc, '\xB3', 0x08);
        }

        for(int p=0;p<ll&&p<(int)sizeof(col_buf);p++) col_buf[p]=COL_RESET;
        ed_colourise(line, col_buf, ll, ed_ft);

        for(int lc=0; lc<ll && sc<TERM_W; lc++, sc++) {
            /* col-80 wrap indicator */
            if(ed_settings.word_wrap_indicator && lc == 79) {
                uint8_t wattr = 0x4E;
                sh_putc(sr, sc, '>', wattr);
                /* still render char under it in remaining space */
                continue;
            }

            char    ch   = line[lc];
            uint8_t attr = col_buf[lc];
            bool is_cur  = (br==ed_cur_row && lc==ed_cur_col);
            bool is_vis  = ed_in_visual(br, lc);
            bool is_sh   = ed_in_search_match(br, lc);

            if(is_cur && (ed_mode==MODE_NORMAL||ed_mode==MODE_VISUAL))
                attr = TH(cursor_normal);
            else if(is_cur && ed_mode==MODE_INSERT)
                attr = TH(cursor_insert);
            else if(is_vis)
                attr = TH(visual_sel);
            else if(is_sh)
                attr = TH(search_hl);
            else if(ed_settings.cursor_line_hl && br == ed_cur_row)
                attr = (attr & 0x0F) | (TH(cursor_line_bg) << 4);

            char disp_ch = ch;
            if(ed_settings.show_whitespace) {
                if(ch == ' ') {
                    disp_ch = '\xB7';
                    if(!is_cur && !is_vis && !is_sh) attr = COL_HINT;
                } else if(ch == '\t') {
                    disp_ch = '\xBB';
                    if(!is_cur && !is_vis && !is_sh) attr = COL_HINT;
                }
            }

            sh_putc(sr, sc, disp_ch, attr);
        }

        /* EOL marker */
        if(ed_settings.show_eol && sc < TERM_W && br < ed_line_count) {
            bool is_cur_eol = (br == ed_cur_row && ed_cur_col == ll && ed_mode==MODE_INSERT);
            uint8_t eol_attr = is_cur_eol ? TH(cursor_insert) : COL_HINT;
            sh_putc(sr, sc++, '$', eol_attr);
        }

        if(br==ed_cur_row && ed_cur_col==ll && ed_mode==MODE_INSERT && sc<TERM_W) {
            uint8_t attr = TH(cursor_insert);
            if(ed_settings.cursor_line_hl) attr = (attr & 0x0F) | (TH(cursor_line_bg) << 4);
            sh_putc(sr, sc++, ' ', attr);
        }
    }

    /* ── Apply rain BEFORE drawing status/hints ────────────────────────── */
    rain_tick_and_draw();

    /* ── Status bar ────────────────────────────────────────────────────── */
    {
        uint8_t sattr;
        const char *mode_str;
        switch(ed_mode) {
            case MODE_NORMAL:   sattr=TH(status_normal);  mode_str=" NORMAL  "; break;
            case MODE_INSERT:   sattr=TH(status_insert);  mode_str=" INSERT  "; break;
            case MODE_VISUAL:   sattr=TH(status_visual);  mode_str=" VISUAL  "; break;
            case MODE_SEARCH:   sattr=TH(status_search);  mode_str=" SEARCH  "; break;
            case MODE_COMMAND:  sattr=TH(status_command); mode_str=" COMMAND "; break;
            case MODE_SETTINGS: sattr=SET_BORDER;          mode_str=" SETTINGS"; break;
            default:            sattr=TH(status_normal);  mode_str=" NORMAL  "; break;
        }
        int c=0;
        sh_pad(STATUS_ROW, &c, TERM_W, sattr);
        c=0;
        sh_puts(STATUS_ROW, &c, mode_str, sattr);

        /* separator */
        sh_putc(STATUS_ROW, c++, ' ', TH(status_sep));
        sh_putc(STATUS_ROW, c++, '\xBA', TH(status_sep));
        sh_putc(STATUS_ROW, c++, ' ', sattr);

        const char *fname = ed_path[0] ? ed_path : "[No Name]";
        sh_puts(STATUS_ROW, &c, fname, sattr);
        if(ed_modified && ed_settings.show_modified)
            sh_puts(STATUS_ROW, &c, " [+]", sattr);

        const char *ft_str = "";
        switch(ed_ft) {
            case FT_RSH:  ft_str=" \xB3rsh\xB3";  break;
            case FT_RASH: ft_str=" \xB3rash\xB3"; break;
            case FT_HTML: ft_str=" \xB3html\xB3"; break;
            default: break;
        }
        sh_puts(STATUS_ROW, &c, ft_str, TH(status_sep));

        sh_putc(STATUS_ROW, c++, ' ', sattr);
        sh_puts(STATUS_ROW, &c, THEMES[ed_theme_idx].name, sattr);

        /* matrix rain indicator */
        if(matrix_rain_on) sh_puts(STATUS_ROW, &c, " \x0E\x0A[\xCE]", TH(rain_head));

        if(ed_msg[0]) {
            sh_putc(STATUS_ROW, c++, ' ', sattr);
            sh_putc(STATUS_ROW, c++, '\xB3', TH(status_sep));
            sh_putc(STATUS_ROW, c++, ' ', sattr);
            sh_puts(STATUS_ROW, &c, ed_msg, TH(msg_col));
        }

        /* Right side: position */
        char pos[48];
        int ri=0;
        if(ed_settings.show_clock) {
            char fc[12]; ed_itoa((int)(ed_frame & 0xFFFF), fc);
            pos[ri++]='[';
            for(int k=0;fc[k];k++) pos[ri++]=fc[k];
            pos[ri++]=']'; pos[ri++]=' ';
        }
        char rstr[8], cstr[8], tstr[8];
        ed_itoa(ed_cur_row+1, rstr);
        ed_itoa(ed_cur_col+1, cstr);
        ed_itoa(ed_line_count, tstr);
        for(int k=0;rstr[k];k++) pos[ri++]=rstr[k];
        pos[ri++]=':';
        for(int k=0;cstr[k];k++) pos[ri++]=cstr[k];
        if(ed_settings.show_status_pct && ed_line_count > 0) {
            int pct = (ed_cur_row * 100) / ed_line_count;
            char pstr[8]; ed_itoa(pct, pstr);
            pos[ri++]=' '; pos[ri++]='(';
            for(int k=0;pstr[k];k++) pos[ri++]=pstr[k];
            pos[ri++]='%'; pos[ri++]=')';
        }
        pos[ri++]=' '; pos[ri++]='/'; pos[ri++]=' ';
        for(int k=0;tstr[k];k++) pos[ri++]=tstr[k];
        pos[ri]=0;
        int rc2 = TERM_W - ri - 1;
        if(rc2<0) rc2=0;
        int tmp=rc2;
        sh_puts(STATUS_ROW, &tmp, pos, sattr);

        if(ed_mode==MODE_SEARCH) {
            c=0; sh_pad(STATUS_ROW, &c, TERM_W, TH(status_search));
            c=0; sh_puts(STATUS_ROW, &c, " /", TH(search_prompt));
            sh_puts(STATUS_ROW, &c, ed_search, TH(status_search));
            sh_putc(STATUS_ROW, c++, '_', TH(status_search));
        }
        if(ed_mode==MODE_COMMAND) {
            c=0; sh_pad(STATUS_ROW, &c, TERM_W, TH(status_command));
            c=0; sh_putc(STATUS_ROW, c++, ':', TH(status_command));
            sh_puts(STATUS_ROW, &c, ed_cmd, TH(status_command));
            sh_putc(STATUS_ROW, c++, '_', TH(status_command));
        }
    }

    /* ── Hint bar ──────────────────────────────────────────────────────── */
    if(ed_settings.show_hints) {
        int c=0;
        sh_pad(HINT_ROW, &c, TERM_W, TH(hint_bg));
        c=0;
        const char *hints[] = {
            "^S","Save ", "^X","Exit ", "^Z","Undo ",
            "^K","Cut  ", "^U","Paste", " i","Ins  ",
            " v","Vis  ", " /","Find ", " :","Cmd  ",
            "^G","Setup", " ?","Nums ", "^D","Dupe ",
            NULL
        };
        for(int k=0; hints[k]; k+=2) {
            sh_puts(HINT_ROW, &c, hints[k],   TH(hint_key));
            sh_puts(HINT_ROW, &c, hints[k+1], TH(hint_bg));
            if(c < TERM_W-1) sh_putc(HINT_ROW, c++, ' ', TH(hint_bg));
        }
    }

    /* ── Settings overlay (on top if active) ───────────────────────────── */
    if(ed_mode == MODE_SETTINGS) {
        ed_draw_settings();
    }

    sh_flush();

    if(ed_mode != MODE_SETTINGS) {
        int sr = ed_cur_row - ed_scroll;
        int sc_col = ed_cur_col + gutter;
        if(sr >= 0 && sr < EDIT_ROWS) {
            extern size_t terminal_row;
            extern size_t terminal_column;
            terminal_row    = (size_t)sr;
            terminal_column = (size_t)sc_col;
            terminal_update_cursor();
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   File I/O
   ═══════════════════════════════════════════════════════════════════════════ */
#define ED_IO_BUF_SIZE   8192
#define BACKUP_BUF_SIZE  16384
#define AVFS_PATH_MAX    512

static char ed_io_buf[ED_IO_BUF_SIZE];

void resolve_path(const char* input, char* out) {
    if (!input || !input[0]) { out[0]='/'; out[1]=0; return; }
    const char* ptr = input;
    if (ptr[0] == '~') {
        ptr++; if (ptr[0] == '/') ptr++;
        out[0] = '/'; out[1] = 0;
        if (ptr[0]) {
            int len = ed_strlen(ptr);
            int olen = ed_strlen(out);
            for(int i=0; i<len && olen+i<AVFS_PATH_MAX-1; i++) out[olen+i]=ptr[i];
            out[olen+len]=0;
        }
    } else if (ptr[0] == '/' && ptr[1] == '~') {
        ptr += 2; if (ptr[0] == '/') ptr++;
        out[0] = '/'; out[1] = 0;
        if (ptr[0]) {
            int len = ed_strlen(ptr);
            int olen = ed_strlen(out);
            for(int i=0; i<len && olen+i<AVFS_PATH_MAX-1; i++) out[olen+i]=ptr[i];
            out[olen+len]=0;
        }
    } else {
        ed_strncpy(out, ptr, AVFS_PATH_MAX - 1);
        out[AVFS_PATH_MAX - 1] = 0;
    }
    int len = ed_strlen(out);
    while (len > 1 && out[len-1] == '/') out[--len] = 0;
}

void ed_load(const char *path) {
    ed_line_count = 0; ed_strcpy(ed_lines[0], ""); ed_line_count = 1;
    int sz = avfs_get_filesize(path);
    if(sz <= 0) return;
    if(sz >= (int)sizeof(ed_io_buf)) sz = (int)sizeof(ed_io_buf)-1;
    if(avfs_read_file(path, ed_io_buf, (uint32_t)sz, 0) != 0) return;
    ed_io_buf[sz] = 0;
    ed_line_count = 0;
    int li=0, ci=0;
    for(int i=0; i<=sz; i++) {
        char ch = ed_io_buf[i];
        if(ch=='\n'||ch==0) {
            ed_lines[li][ci] = 0; li++; ci=0;
            if(li >= MAX_LINES) break;
            if(ch==0) break;
        } else if(ch!='\r') {
            if(ci < MAX_LINE_LEN-1) ed_lines[li][ci++] = ch;
        }
    }
    ed_line_count = li>0 ? li : 1;
}

static void get_filename_only(const char* path, char* out) {
    int len = ed_strlen(path);
    int last_slash = -1;
    for(int i=0; i<len; i++) if(path[i]=='/') last_slash = i;
    if (last_slash >= 0) ed_strcpy(out, path + last_slash + 1);
    else                 ed_strcpy(out, path);
}

static int backup_file_to_tmp(const char *avfs_path) {
    if (!avfs_file_exists(avfs_path)) return 0;
    if (ed_strncmp(avfs_path, "/tmp/", 5) == 0) return 0;
    if (!avfs_is_directory("/tmp")) {
        if (avfs_create_dir("/tmp") != 0) {
            ed_strcpy(ed_msg, "Backup: Failed to create /tmp"); return -1;
        }
    }
    int file_size = avfs_get_filesize(avfs_path);
    if (file_size < 0) return -1;
    if (file_size >= BACKUP_BUF_SIZE) { ed_strcpy(ed_msg, "Backup: File too large"); return -1; }
    static char backup_buf[BACKUP_BUF_SIZE];
    if (avfs_read_file(avfs_path, backup_buf, (uint32_t)file_size, 0) != 0) {
        ed_strcpy(ed_msg, "Backup: Read failed"); return -1;
    }
    char filename[128]; char backup_path[AVFS_PATH_MAX];
    get_filename_only(avfs_path, filename);
    ed_strcpy(backup_path, "/tmp/");
    int blen = ed_strlen(backup_path);
    int flen = ed_strlen(filename);
    for(int i=0; i<flen && blen+i<AVFS_PATH_MAX-1; i++) backup_path[blen+i]=filename[i];
    backup_path[blen+flen]=0;

    avfs_remove_file(backup_path);
    if (avfs_create_file(backup_path, (uint32_t)file_size) != 0) {
        ed_strcpy(ed_msg, "Backup: Create failed"); return -1;
    }
    if (avfs_write_file(backup_path, backup_buf, (uint32_t)file_size, 0) != 0) {
        ed_strcpy(ed_msg, "Backup: Write failed");
        avfs_remove_file(backup_path); return -1;
    }
    return 0;
}

/* trim trailing whitespace helper */
static void ed_trim_line(char *line) {
    int len = ed_strlen(line);
    while(len > 0 && (line[len-1]==' '||line[len-1]=='\t')) { line[--len]=0; }
}

int ed_save(const char *path) {
    char resolved_path[AVFS_PATH_MAX];
    resolve_path(path, resolved_path);
    if (backup_file_to_tmp(resolved_path) != 0) return -1;

    /* Optional: trim trailing whitespace */
    if(ed_settings.trim_trailing_save) {
        for(int i=0;i<ed_line_count;i++) ed_trim_line(ed_lines[i]);
    }

    int out = 0;
    int write_count = ed_line_count;

    /* Optional: insert final newline means we always end with \n */
    for(int i = 0; i < write_count; i++) {
        int ll = ed_strlen(ed_lines[i]);
        if(out + ll + 1 >= (int)sizeof(ed_io_buf)) {
            ed_strcpy(ed_msg, "File too large for buffer"); return -1;
        }
        for(int j = 0; j < ll; j++) ed_io_buf[out++] = ed_lines[i][j];
        if(i < write_count - 1 || ed_settings.insert_final_newline)
            ed_io_buf[out++] = '\n';
    }
    ed_io_buf[out] = 0;
    avfs_remove_file(resolved_path);
    int r = avfs_create_file(resolved_path, (uint32_t)out);
    if(r != 0) { ed_strcpy(ed_msg, "Create failed (disk full?)"); return -1; }
    r = avfs_write_file(resolved_path, ed_io_buf, (uint32_t)out, 0);
    if(r >= 0) {
        ed_modified = false;
        char filename[128];
        get_filename_only(resolved_path, filename);
        char success_msg[128];
        ed_strcpy(success_msg, "Saved: ");
        int slen = ed_strlen(success_msg);
        int flen = ed_strlen(filename);
        for(int i=0; i<flen && slen+i<126; i++) success_msg[slen+i]=filename[i];
        success_msg[slen+flen]=0;
        ed_strcpy(ed_msg, success_msg);
        return 0;
    }
    ed_strcpy(ed_msg, "Write failed");
    return -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Edit operations
   ═══════════════════════════════════════════════════════════════════════════ */
static void ed_insert_char(char c) {
    ed_undo_push();
    char *line = ed_lines[ed_cur_row];
    int ll = ed_strlen(line);
    if(ll >= MAX_LINE_LEN-1) return;

    /* Auto pairs */
    if(ed_settings.auto_pairs) {
        const char *pairs = "()[]{}<>\"\"''``";
        for(int p=0;pairs[p];p+=2) {
            if(c==pairs[p]) {
                ed_memmove(line+ed_cur_col+2, line+ed_cur_col, ll-ed_cur_col+1);
                line[ed_cur_col]   = c;
                line[ed_cur_col+1] = pairs[p+1];
                ed_cur_col++; ed_modified=true; return;
            }
        }
    }

    ed_memmove(line+ed_cur_col+1, line+ed_cur_col, ll-ed_cur_col+1);
    line[ed_cur_col] = c;
    ed_cur_col++; ed_modified = true;

    /* Hard wrap */
    if(ed_settings.hard_wrap && ed_cur_col >= ed_settings.hard_wrap_col) {
        /* find last space */
        int sp = ed_cur_col - 1;
        while(sp > 0 && line[sp] != ' ') sp--;
        if(sp > 0) {
            /* split at sp */
            if(ed_line_count < MAX_LINES) {
                for(int i=ed_line_count-1; i>ed_cur_row; i--)
                    ed_strcpy(ed_lines[i+1], ed_lines[i]);
                ed_line_count++;
                ed_strcpy(ed_lines[ed_cur_row+1], line+sp+1);
                line[sp]=0;
                ed_cur_row++; ed_cur_col=ed_strlen(ed_lines[ed_cur_row]);
            }
        }
    }
}
static void ed_backspace(void) {
    if(ed_cur_col > 0) {
        ed_undo_push();
        char *line = ed_lines[ed_cur_row];
        int ll = ed_strlen(line);
        ed_memmove(line+ed_cur_col-1, line+ed_cur_col, ll-ed_cur_col+1);
        ed_cur_col--; ed_modified = true;
    } else if(ed_cur_row > 0) {
        ed_undo_push();
        int prev_len = ed_strlen(ed_lines[ed_cur_row-1]);
        int cur_len  = ed_strlen(ed_lines[ed_cur_row]);
        if(prev_len+cur_len < MAX_LINE_LEN-1) {
            ed_strcpy(ed_lines[ed_cur_row-1]+prev_len, ed_lines[ed_cur_row]);
            for(int i=ed_cur_row; i<ed_line_count-1; i++)
                ed_strcpy(ed_lines[i], ed_lines[i+1]);
            ed_line_count--; ed_cur_row--; ed_cur_col=prev_len; ed_modified=true;
        }
    }
}
static void ed_delete_char(void) {
    char *line = ed_lines[ed_cur_row];
    int ll = ed_strlen(line);
    if(ed_cur_col < ll) {
        ed_undo_push();
        ed_memmove(line+ed_cur_col, line+ed_cur_col+1, ll-ed_cur_col);
        ed_modified = true;
        if(ed_cur_col >= ed_strlen(line) && ed_cur_col>0) ed_cur_col--;
    }
}
static void ed_newline(void) {
    if(ed_line_count >= MAX_LINES) return;
    ed_undo_push();
    char *cur = ed_lines[ed_cur_row];
    for(int i=ed_line_count-1; i>ed_cur_row; i--)
        ed_strcpy(ed_lines[i+1], ed_lines[i]);
    ed_line_count++;
    ed_strcpy(ed_lines[ed_cur_row+1], cur+ed_cur_col);
    cur[ed_cur_col] = 0;
    int indent=0;
    if(ed_settings.auto_indent)
        while(cur[indent]==' '||cur[indent]=='\t') indent++;
    char new_line[MAX_LINE_LEN];
    int ni=0;
    if(ed_settings.auto_indent)
        for(int i=0;i<indent&&ni<MAX_LINE_LEN-1;i++) new_line[ni++]=cur[i];
    int rest_len = ed_strlen(ed_lines[ed_cur_row+1]);
    if(ni+rest_len < MAX_LINE_LEN-1) {
        ed_memmove(ed_lines[ed_cur_row+1]+ni, ed_lines[ed_cur_row+1], rest_len+1);
        for(int i=0;i<ni;i++) ed_lines[ed_cur_row+1][i]=new_line[i];
    }
    ed_cur_row++; ed_cur_col=ni; ed_modified=true;
}
static void ed_delete_line(void) {
    ed_undo_push();
    if(ed_clip.count < CLIP_LINES) {
        ed_strcpy(ed_clip.lines[ed_clip.count++], ed_lines[ed_cur_row]);
    } else {
        ed_memmove((char*)ed_clip.lines[0], (char*)ed_clip.lines[1],
                   (CLIP_LINES-1)*MAX_LINE_LEN);
        ed_strcpy(ed_clip.lines[CLIP_LINES-1], ed_lines[ed_cur_row]);
    }
    if(ed_line_count > 1) {
        for(int i=ed_cur_row; i<ed_line_count-1; i++)
            ed_strcpy(ed_lines[i], ed_lines[i+1]);
        ed_line_count--;
        if(ed_cur_row >= ed_line_count) ed_cur_row=ed_line_count-1;
    } else { ed_strcpy(ed_lines[0], ""); }
    ed_modified=true; ed_strcpy(ed_msg,"Cut line");
}
static void ed_yank_line(void) {
    if(ed_clip.count < CLIP_LINES) {
        ed_strcpy(ed_clip.lines[ed_clip.count++], ed_lines[ed_cur_row]);
    } else {
        ed_memmove((char*)ed_clip.lines[0], (char*)ed_clip.lines[1],
                   (CLIP_LINES-1)*MAX_LINE_LEN);
        ed_strcpy(ed_clip.lines[CLIP_LINES-1], ed_lines[ed_cur_row]);
    }
    ed_strcpy(ed_msg,"Yanked line");
}
static void ed_paste(void) {
    if(ed_clip.count == 0) return;
    ed_undo_push();
    if(ed_line_count >= MAX_LINES) return;
    for(int i=ed_line_count-1; i>ed_cur_row; i--)
        ed_strcpy(ed_lines[i+1], ed_lines[i]);
    ed_line_count++; ed_cur_row++;
    ed_strcpy(ed_lines[ed_cur_row], ed_clip.lines[ed_clip.count-1]);
    ed_cur_col=0; ed_modified=true; ed_strcpy(ed_msg,"Pasted");
}
static void ed_visual_yank(void) {
    int r0,c0,r1,c1; ed_vis_ordered(&r0,&c0,&r1,&c1);
    ed_clip.count=0;
    for(int r=r0; r<=r1&&ed_clip.count<CLIP_LINES; r++) {
        if(r0==r1) {
            int ll=ed_strlen(ed_lines[r]);
            int e=c1<ll?c1:ll-1;
            int len=e-c0+1; if(len<0)len=0;
            if(len>MAX_LINE_LEN-1)len=MAX_LINE_LEN-1;
            for(int k=0;k<len;k++) ed_clip.lines[ed_clip.count][k]=ed_lines[r][c0+k];
            ed_clip.lines[ed_clip.count][len]=0;
        } else { ed_strcpy(ed_clip.lines[ed_clip.count], ed_lines[r]); }
        ed_clip.count++;
    }
    ed_strcpy(ed_msg,"Yanked selection");
}
static void ed_visual_delete(void) {
    ed_undo_push(); ed_visual_yank();
    int r0,c0,r1,c1; ed_vis_ordered(&r0,&c0,&r1,&c1);
    if(r0==r1) {
        char *line=ed_lines[r0];
        int ll=ed_strlen(line);
        int e=c1<ll?c1:ll-1;
        int len=e-c0+1; if(len<0)len=0;
        ed_memmove(line+c0, line+e+1, ll-e);
        ed_cur_row=r0; ed_cur_col=c0;
    } else {
        for(int i=r0;i<=r1&&i<ed_line_count;i++) ed_strcpy(ed_lines[r0],"");
        int del=r1-r0+1;
        for(int i=r0;i<ed_line_count-del;i++)
            ed_strcpy(ed_lines[i],ed_lines[i+del]);
        ed_line_count-=del;
        if(ed_line_count<1) ed_line_count=1;
        ed_cur_row=r0; ed_cur_col=0;
    }
    ed_modified=true; ed_strcpy(ed_msg,"Deleted selection");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Search
   ═══════════════════════════════════════════════════════════════════════════ */
static bool ed_find_next(int from_row, int from_col, bool wrap) {
    if(ed_search_len==0) return false;
    int sl=ed_strlen(ed_search);
    char low_search[SEARCH_MAX+1];
    char* cmp_search = (char*)ed_search;
    if(!ed_settings.case_search) {
        ed_tolower_copy(ed_search, sl, low_search, SEARCH_MAX+1);
        cmp_search = low_search;
    }
    int passes = (wrap || ed_settings.wrap_search) ? 2 : 1;
    for(int pass=0; pass<passes; pass++) {
        int sr=(pass==0)?from_row:0;
        int sc=(pass==0)?from_col:0;
        for(int r=sr; r<ed_line_count; r++) {
            const char *line=ed_lines[r];
            int ll=ed_strlen(line);
            int sc2=(r==sr)?sc:0;
            char low_line[MAX_LINE_LEN];
            const char* cmp_line = line;
            if(!ed_settings.case_search) {
                ed_tolower_copy(line, ll, low_line, MAX_LINE_LEN);
                cmp_line = low_line;
            }
            for(int c=sc2; c<=ll-sl; c++) {
                if(ed_strncmp(cmp_line+c, cmp_search, sl)==0) {
                    ed_search_row=r; ed_search_col=c;
                    ed_cur_row=r; ed_cur_col=c;
                    ed_scroll_to_cursor(); return true;
                }
            }
        }
        if(!wrap && !ed_settings.wrap_search) break;
    }
    ed_strcpy(ed_msg,"Not found"); return false;
}

/* ═══════════════════════════════════════════════════════════════════════════
   :command handler
   ═══════════════════════════════════════════════════════════════════════════ */
static bool ed_run_command(void) {
    const char *cmd = ed_cmd;
    if(ed_strcmp(cmd,"w")==0)        { ed_save(ed_path); return true; }
    if(ed_strncmp(cmd,"w ",2)==0)    { ed_strncpy(ed_path,cmd+2,MAX_PATH); ed_save(ed_path); return true; }
    if(ed_strcmp(cmd,"q")==0)  {
        if(ed_modified) {
            if(ed_settings.save_on_exit) { ed_save(ed_path); return false; }
            ed_strcpy(ed_msg,"Unsaved! :q! or save"); return true;
        }
        return false;
    }
    if(ed_strcmp(cmd,"q!")==0)       return false;
    if(ed_strcmp(cmd,"wq")==0)       { ed_save(ed_path); return false; }
    if(ed_strcmp(cmd,"set nu")==0||ed_strcmp(cmd,"set number")==0)
        { ed_show_nums=true;  ed_settings.line_numbers=true;  ed_strcpy(ed_msg,"Line numbers on"); return true; }
    if(ed_strcmp(cmd,"set nonu")==0||ed_strcmp(cmd,"set nonumber")==0)
        { ed_show_nums=false; ed_settings.line_numbers=false; ed_strcpy(ed_msg,"Line numbers off"); return true; }
    if(ed_strcmp(cmd,"ft=rsh" )==0)  { ed_ft=FT_RSH;  ed_strcpy(ed_msg,"ft: rsh");  return true; }
    if(ed_strcmp(cmd,"ft=rash")==0)  { ed_ft=FT_RASH; ed_strcpy(ed_msg,"ft: rash"); return true; }
    if(ed_strcmp(cmd,"ft=html")==0)  { ed_ft=FT_HTML; ed_strcpy(ed_msg,"ft: html"); return true; }
    if(ed_strcmp(cmd,"ft=text")==0)  { ed_ft=FT_TEXT; ed_strcpy(ed_msg,"ft: text"); return true; }
    if(ed_strncmp(cmd,"theme=",6)==0) {
        int t=ed_atoi(cmd+6);
        if(t>=0&&t<NUM_THEMES) {
            ed_theme_idx=t; ed_settings.theme_idx=t;
            char msg[32]; ed_strcpy(msg,"Theme: ");
            ed_strncpy(msg+7, THEMES[t].name, 24);
            ed_strcpy(ed_msg, msg);
        } else { ed_strcpy(ed_msg,"Invalid theme index"); }
        return true;
    }
    if(ed_strcmp(cmd,"saveset")==0) {
        if(ed_save_settings()==0) ed_strcpy(ed_msg,"Settings saved");
        else                       ed_strcpy(ed_msg,"Failed to save settings");
        return true;
    }
    if(ed_strcmp(cmd,"rain")==0) {
        matrix_rain_on = !matrix_rain_on;
        if(matrix_rain_on && !rain_inited) rain_init();
        ed_strcpy(ed_msg, matrix_rain_on ? "Rain ON" : "Rain OFF");
        return true;
    }
    ed_strcpy(ed_msg,"Unknown command"); return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Settings activate helper
   ═══════════════════════════════════════════════════════════════════════════ */
static void ed_settings_activate(void) {
    if(set_theme_open) {
        ed_settings.theme_idx = set_theme_sel;
        ed_theme_idx          = set_theme_sel;
        set_theme_open        = false;
        ed_strcpy(ed_msg, "Theme applied");
        return;
    }

    int item_id = SET_ITEMS[set_cursor].item_id;
    switch(item_id) {
        case SET_ITEM_LINENUMS:     ed_settings.line_numbers        = !ed_settings.line_numbers;        break;
        case SET_ITEM_RELNUMS:      ed_settings.relative_numbers    = !ed_settings.relative_numbers;    break;
        case SET_ITEM_AUTOINDENT:   ed_settings.auto_indent         = !ed_settings.auto_indent;         break;
        case SET_ITEM_TABSPACES:    ed_settings.tab_spaces          = !ed_settings.tab_spaces;          break;
        case SET_ITEM_TABWIDTH:     ed_settings.tab_width           = (ed_settings.tab_width==2)?4:2;   break;
        case SET_ITEM_SHOWMOD:      ed_settings.show_modified       = !ed_settings.show_modified;       break;
        case SET_ITEM_WRAPSEARCH:   ed_settings.wrap_search         = !ed_settings.wrap_search;         break;
        case SET_ITEM_SYNTAX:       ed_settings.syntax_highlight    = !ed_settings.syntax_highlight;    break;
        case SET_ITEM_CASESEARCH:   ed_settings.case_search         = !ed_settings.case_search;         break;
        case SET_ITEM_CURSORHL:     ed_settings.cursor_line_hl      = !ed_settings.cursor_line_hl;      break;
        case SET_ITEM_HINTBAR:      ed_settings.show_hints          = !ed_settings.show_hints;          break;
        case SET_ITEM_SMARTHOME:    ed_settings.smart_home          = !ed_settings.smart_home;          break;
        case SET_ITEM_SHOWWS:       ed_settings.show_whitespace     = !ed_settings.show_whitespace;     break;
        case SET_ITEM_SHOWEOL:      ed_settings.show_eol            = !ed_settings.show_eol;            break;
        case SET_ITEM_TRAILWS:      ed_settings.trailing_ws_warn    = !ed_settings.trailing_ws_warn;    break;
        case SET_ITEM_BRACKETMATCH: ed_settings.bracket_match       = !ed_settings.bracket_match;       break;
        case SET_ITEM_WRAP80:       ed_settings.word_wrap_indicator = !ed_settings.word_wrap_indicator; break;
        case SET_ITEM_SCROLLOFF:    ed_settings.scroll_off = (ed_settings.scroll_off+1)%11;             break;
        case SET_ITEM_BOLDKW:       ed_settings.bold_keywords       = !ed_settings.bold_keywords;       break;
        case SET_ITEM_DIMCOMMENT:   ed_settings.dim_comments        = !ed_settings.dim_comments;        break;
        case SET_ITEM_STATUS_COL:   ed_settings.show_status_col     = !ed_settings.show_status_col;     break;
        case SET_ITEM_STATUS_PCT:   ed_settings.show_status_pct     = !ed_settings.show_status_pct;     break;
        case SET_ITEM_SAVE_EXIT:    ed_settings.save_on_exit        = !ed_settings.save_on_exit;        break;
        case SET_ITEM_CONFIRM_DD:   ed_settings.confirm_delete_line = !ed_settings.confirm_delete_line; break;
        case SET_ITEM_DBL_SPACE:    ed_settings.double_space_sentence=!ed_settings.double_space_sentence;break;
        case SET_ITEM_UNDO_LIMIT:   {
            if(ed_settings.undo_limit==16)       ed_settings.undo_limit=32;
            else if(ed_settings.undo_limit==32)  ed_settings.undo_limit=64;
            else                                 ed_settings.undo_limit=16;
            break;
        }
        case SET_ITEM_HL_URLS:      ed_settings.highlight_urls      = !ed_settings.highlight_urls;      break;
        case SET_ITEM_RULER:        ed_settings.show_ruler           = !ed_settings.show_ruler;          break;
        case SET_ITEM_AUTO_PAIRS:   ed_settings.auto_pairs           = !ed_settings.auto_pairs;          break;
        case SET_ITEM_TRIM_SAVE:    ed_settings.trim_trailing_save   = !ed_settings.trim_trailing_save;  break;
        case SET_ITEM_FINAL_NL:     ed_settings.insert_final_newline = !ed_settings.insert_final_newline;break;
        case SET_ITEM_HARD_WRAP:    ed_settings.hard_wrap            = !ed_settings.hard_wrap;           break;
        case SET_ITEM_HARD_WRAP_COL:{
            if(ed_settings.hard_wrap_col==60)       ed_settings.hard_wrap_col=72;
            else if(ed_settings.hard_wrap_col==72)  ed_settings.hard_wrap_col=80;
            else                                     ed_settings.hard_wrap_col=60;
            break;
        }
        case SET_ITEM_CLOCK:        ed_settings.show_clock           = !ed_settings.show_clock;          break;
        case SET_ITEM_THEME:
            set_theme_open = true;
            set_theme_sel  = ed_settings.theme_idx;
            break;
        default: break;
    }
}

/* ── Settings left/right cycle ──────────────────────────────────────────── */
static void ed_settings_cycle(int dir) {
    if(set_theme_open) {
        set_theme_sel = (set_theme_sel + dir + NUM_THEMES) % NUM_THEMES;
        ed_settings.theme_idx = set_theme_sel;
        ed_theme_idx          = set_theme_sel;
        return;
    }
    int item_id = SET_ITEMS[set_cursor].item_id;
    switch(item_id) {
        case SET_ITEM_THEME: {
            ed_settings.theme_idx = (ed_settings.theme_idx + dir + NUM_THEMES) % NUM_THEMES;
            ed_theme_idx = ed_settings.theme_idx;
            break;
        }
        case SET_ITEM_TABWIDTH:
            ed_settings.tab_width = (dir>0)?4:2;
            break;
        case SET_ITEM_SCROLLOFF:
            ed_settings.scroll_off = (ed_settings.scroll_off + dir + 11) % 11;
            break;
        case SET_ITEM_UNDO_LIMIT: {
            int vals[] = {16,32,64};
            for(int k=0;k<3;k++) {
                if(ed_settings.undo_limit==vals[k]) {
                    ed_settings.undo_limit = vals[(k+dir+3)%3];
                    break;
                }
            }
            break;
        }
        case SET_ITEM_HARD_WRAP_COL: {
            int vals[] = {60,72,80};
            for(int k=0;k<3;k++) {
                if(ed_settings.hard_wrap_col==vals[k]) {
                    ed_settings.hard_wrap_col = vals[(k+dir+3)%3];
                    break;
                }
            }
            break;
        }
        /* booleans: right=ON, left=OFF */
        default: {
            /* find the bool field via activate — but just toggle */
            ed_settings_activate();
            break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Settings panel input handler
   ═══════════════════════════════════════════════════════════════════════════ */
static bool ed_handle_settings_key(uint8_t sc) {
    if(sc == 0xE0) {
        uint8_t ext = ed_get_sc();
        if(ext & 0x80) return true;

        if(ext == SC_UP) {
            if(set_theme_open) {
                set_theme_sel = (set_theme_sel-1+NUM_THEMES)%NUM_THEMES;
            } else {
                set_cursor = set_next_sel(set_cursor, -1);
                /* scroll viewport to keep cursor visible */
                if(set_cursor < set_scroll) set_scroll = set_cursor;
                if(set_cursor >= set_scroll+SET_VIEW_ROWS) set_scroll = set_cursor-SET_VIEW_ROWS+1;
            }
            return true;
        }
        if(ext == SC_DOWN) {
            if(set_theme_open) {
                set_theme_sel = (set_theme_sel+1)%NUM_THEMES;
            } else {
                set_cursor = set_next_sel(set_cursor, 1);
                if(set_cursor < set_scroll) set_scroll = set_cursor;
                if(set_cursor >= set_scroll+SET_VIEW_ROWS) set_scroll = set_cursor-SET_VIEW_ROWS+1;
            }
            return true;
        }
        if(ext == SC_LEFT)  { ed_settings_cycle(-1); return true; }
        if(ext == SC_RIGHT) { ed_settings_cycle(+1); return true; }
        if(ext == SC_PGUP) { set_scroll -= SET_VIEW_ROWS; if(set_scroll<0)set_scroll=0; return true; }
        if(ext == SC_PGDN) {
            int n=0; while(SET_ITEMS[n].label) n++;
            set_scroll += SET_VIEW_ROWS;
            if(set_scroll>n-SET_VIEW_ROWS) set_scroll=n-SET_VIEW_ROWS;
            if(set_scroll<0) set_scroll=0;
            return true;
        }
        return true;
    }

    if(sc & 0x80) return true;

    /* Ctrl+G or ESC closes */
    if(sc == SC_ESC) {
        if(set_theme_open) { set_theme_open=false; return true; }
        ed_mode = MODE_NORMAL;
        ed_apply_settings();
        ed_strcpy(ed_msg,"Settings closed");
        return false;
    }

    if(sc == SC_ENTER) {
        ed_settings_activate();
        return true;
    }

    /* S = save */
    if(ed_ctrl) return true;
    char c = 0;
    /* manual scancode -> char just for s */
    if(sc == 0x1F) { /* s scancode */
        if(ed_save_settings()==0) ed_strcpy(ed_msg,"Settings saved");
        else                       ed_strcpy(ed_msg,"Failed to save settings");
        return true;
    }

    /* Non-extended up/down (numpad etc.) */
    if(sc == SC_UP) {
        if(set_theme_open) set_theme_sel=(set_theme_sel-1+NUM_THEMES)%NUM_THEMES;
        else {
            set_cursor = set_next_sel(set_cursor,-1);
            if(set_cursor<set_scroll) set_scroll=set_cursor;
        }
        return true;
    }
    if(sc == SC_DOWN) {
        if(set_theme_open) set_theme_sel=(set_theme_sel+1)%NUM_THEMES;
        else {
            set_cursor = set_next_sel(set_cursor,1);
            if(set_cursor>=set_scroll+SET_VIEW_ROWS) set_scroll=set_cursor-SET_VIEW_ROWS+1;
        }
        return true;
    }

    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Main key dispatch
   ═══════════════════════════════════════════════════════════════════════════ */
static bool ed_handle_key(uint8_t sc) {
    if(ed_mode == MODE_SETTINGS) {
        return ed_handle_settings_key(sc);
    }

    if(sc == 0xE0) {
        uint8_t ext = ed_get_sc();
        if(ext&0x80) {
            uint8_t k=ext&0x7F;
            if(k==SC_LSHIFT||k==SC_RSHIFT) ed_shift=false;
            if(k==SC_LCTRL)                ed_ctrl=false;
            return true;
        }
        if(ext==SC_LSHIFT||ext==SC_RSHIFT){ed_shift=true;return true;}
        if(ext==SC_LCTRL)                 {ed_ctrl=true;return true;}
        switch(ext) {
            case SC_UP:    ed_cur_row--; ed_clamp(); ed_scroll_to_cursor(); break;
            case SC_DOWN:  ed_cur_row++; ed_clamp(); ed_scroll_to_cursor(); break;
            case SC_LEFT:
                if(ed_cur_col>0) ed_cur_col--;
                else if(ed_cur_row>0){ed_cur_row--;ed_cur_col=ed_line_len(ed_cur_row);}
                ed_scroll_to_cursor(); break;
            case SC_RIGHT:
                if(ed_cur_col<ed_line_len(ed_cur_row)) ed_cur_col++;
                else if(ed_cur_row<ed_line_count-1){ed_cur_row++;ed_cur_col=0;}
                ed_scroll_to_cursor(); break;
            case SC_HOME:
                if(ed_settings.smart_home) {
                    char *line = ed_lines[ed_cur_row];
                    int fnw = 0;
                    while(line[fnw]==' '||line[fnw]=='\t') fnw++;
                    if(ed_cur_col==0)       ed_cur_col=fnw;
                    else if(ed_cur_col==fnw) ed_cur_col=0;
                    else                     ed_cur_col=fnw;
                } else { ed_cur_col=0; }
                break;
            case SC_END:   ed_cur_col=ed_line_len(ed_cur_row); break;
            case SC_PGUP:  ed_cur_row-=EDIT_ROWS; ed_clamp(); ed_scroll_to_cursor(); break;
            case SC_PGDN:  ed_cur_row+=EDIT_ROWS; ed_clamp(); ed_scroll_to_cursor(); break;
            case SC_DEL:   ed_delete_char(); break;
        }
        return true;
    }

    if(sc&0x80) {
        uint8_t k=sc&0x7F;
        if(k==SC_LSHIFT||k==SC_RSHIFT) ed_shift=false;
        if(k==SC_LCTRL)                ed_ctrl=false;
        return true;
    }

    if(sc==SC_LSHIFT||sc==SC_RSHIFT){ed_shift=true;return true;}
    if(sc==SC_LCTRL)                {ed_ctrl=true;return true;}
    if(sc==SC_CAPS)                 {ed_caps=!ed_caps;return true;}

    ed_msg[0]=0;

    /* ── Ctrl combos ─────────────────────────────────────────────────── */
    if(ed_ctrl) {
        switch(sc) {
            case 0x1F: ed_save(ed_path); return true;                          /* Ctrl+S */
            case 0x2D: {                                                        /* Ctrl+X */
                if(ed_modified) {
                    ed_save(ed_path);
                    if(!ed_modified) return false;
                    ed_strcpy(ed_msg,"Save failed, not exiting"); return true;
                }
                return false;
            }
            case 0x2C: ed_undo_pop(); return true;                             /* Ctrl+Z */
            case 0x25: ed_delete_line(); return true;                          /* Ctrl+K */
            case 0x16: ed_paste(); return true;                                /* Ctrl+U */
            case 0x2E: ed_yank_line(); return true;                            /* Ctrl+C */
            case 0x26: return true;                                             /* Ctrl+L redraw */
            case 0x22: {                                                        /* Ctrl+G — settings */
                ed_mode        = MODE_SETTINGS;
                set_cursor     = set_next_sel(0, 1);
                set_scroll     = 0;
                set_theme_open = false;
                set_theme_sel  = ed_settings.theme_idx;
                return true;
            }
            case 0x20: {                                                        /* Ctrl+D — duplicate line */
                if(ed_line_count < MAX_LINES) {
                    ed_undo_push();
                    for(int i=ed_line_count-1;i>ed_cur_row;i--)
                        ed_strcpy(ed_lines[i+1], ed_lines[i]);
                    ed_line_count++;
                    ed_strcpy(ed_lines[ed_cur_row+1], ed_lines[ed_cur_row]);
                    ed_cur_row++;
                    ed_modified=true;
                    ed_strcpy(ed_msg,"Duplicated line");
                }
                return true;
            }
            case 0x2F: {                                                        /* Ctrl+/ — comment toggle */
                ed_undo_push();
                char *line=ed_lines[ed_cur_row];
                int ll=ed_strlen(line);
                bool is_comment=(ll>=1&&line[0]=='#')||(ll>=2&&line[0]=='/'&&line[1]=='/');
                if(is_comment) {
                    int skip=(line[0]=='#')?1:2;
                    if(line[skip]==' ') skip++;
                    ed_memmove(line, line+skip, ll-skip+1);
                    ed_strcpy(ed_msg,"Uncommented");
                } else {
                    const char *pfx=(ed_ft==FT_RASH)?"// ":"# ";
                    int plen=ed_strlen(pfx);
                    if(ll+plen<MAX_LINE_LEN-1) {
                        ed_memmove(line+plen, line, ll+1);
                        for(int i=0;i<plen;i++) line[i]=pfx[i];
                        ed_strcpy(ed_msg,"Commented");
                    }
                }
                ed_modified=true;
                return true;
            }
            case 0x31: ed_find_next(ed_cur_row, ed_cur_col+1, true); return true;  /* Ctrl+N */
            case 0x19: ed_find_next(0,0,false); return true;                       /* Ctrl+P */

            /* ══════════════════════════════════════════════════════════
               EASTER EGG KEYS
               Ctrl+M (0x32), Ctrl+T (0x14), Ctrl+R (0x13)
               All three must be activated to unlock Matrix rain
               ══════════════════════════════════════════════════════════ */
            case 0x32: {   /* Ctrl+M */
                ee_secret_m = true;
                if(ee_secret_m && ee_secret_t && ee_secret_r) {
                    matrix_rain_on = true;
                    if(!rain_inited) rain_init();
                    ed_strcpy(ed_msg, "\x0A\x0E[RAIN ACTIVATED]");
                } else {
                    ed_strcpy(ed_msg, "...");
                }
                return true;
            }
            case 0x14: {   /* Ctrl+T */
                ee_secret_t = true;
                if(ee_secret_m && ee_secret_t && ee_secret_r) {
                    matrix_rain_on = true;
                    if(!rain_inited) rain_init();
                    ed_strcpy(ed_msg, "\x0A\x0E[RAIN ACTIVATED]");
                } else {
                    ed_strcpy(ed_msg, "...");
                }
                return true;
            }
            case 0x13: {   /* Ctrl+R */
                ee_secret_r = true;
                if(ee_secret_m && ee_secret_t && ee_secret_r) {
                    matrix_rain_on = true;
                    if(!rain_inited) rain_init();
                    ed_strcpy(ed_msg, "\x0A\x0E[RAIN ACTIVATED]");
                } else {
                    ed_strcpy(ed_msg, "...");
                }
                return true;
            }
        }
        return true;
    }

    /* ── MODE_SEARCH ──────────────────────────────────────────────────── */
    if(ed_mode==MODE_SEARCH) {
        if(sc==SC_ESC) { ed_mode=MODE_NORMAL; }
        else if(sc==SC_BACKSPACE) { if(ed_search_len>0) ed_search[--ed_search_len]=0; }
        else if(sc==SC_ENTER) { ed_mode=MODE_NORMAL; ed_find_next(ed_cur_row,ed_cur_col+1,true); }
        else {
            char c=keyboard_to_char(sc,ed_shift,ed_caps);
            if(c>=32&&c<127&&ed_search_len<SEARCH_MAX) {
                ed_search[ed_search_len++]=c; ed_search[ed_search_len]=0;
                ed_find_next(0,0,false);
            }
        }
        return true;
    }

    /* ── MODE_COMMAND ─────────────────────────────────────────────────── */
    if(ed_mode==MODE_COMMAND) {
        if(sc==SC_ESC) { ed_mode=MODE_NORMAL; }
        else if(sc==SC_BACKSPACE) {
            if(ed_cmd_len>0) ed_cmd[--ed_cmd_len]=0;
            else ed_mode=MODE_NORMAL;
        } else if(sc==SC_ENTER) {
            ed_mode=MODE_NORMAL;
            bool cont=ed_run_command();
            ed_cmd[0]=0; ed_cmd_len=0;
            return cont;
        } else {
            char c=keyboard_to_char(sc,ed_shift,ed_caps);
            if(c>=32&&c<127&&ed_cmd_len<63) { ed_cmd[ed_cmd_len++]=c; ed_cmd[ed_cmd_len]=0; }
        }
        return true;
    }

    /* ── MODE_INSERT ──────────────────────────────────────────────────── */
    if(ed_mode==MODE_INSERT) {
        if(sc==SC_ESC) {
            ed_mode=MODE_NORMAL;
            if(ed_cur_col>0) ed_cur_col--;
            ed_clamp(); return true;
        }
        if(sc==SC_ENTER)     { ed_newline(); return true; }
        if(sc==SC_BACKSPACE) { ed_backspace(); return true; }
        if(sc==SC_TAB) {
            if(ed_settings.tab_spaces)
                for(int i=0;i<ed_settings.tab_width;i++) ed_insert_char(' ');
            else
                ed_insert_char('\t');
            return true;
        }
        char c=keyboard_to_char(sc,ed_shift,ed_caps);
        if(c>=32&&c<127) { ed_insert_char(c); return true; }
        return true;
    }

    /* ── MODE_VISUAL ──────────────────────────────────────────────────── */
    if(ed_mode==MODE_VISUAL) {
        char c=keyboard_to_char(sc,ed_shift,ed_caps);
        switch(c) {
            case 'h': ed_cur_col--; ed_clamp(); ed_scroll_to_cursor(); break;
            case 'l': ed_cur_col++; ed_clamp(); ed_scroll_to_cursor(); break;
            case 'k': ed_cur_row--; ed_clamp(); ed_scroll_to_cursor(); break;
            case 'j': ed_cur_row++; ed_clamp(); ed_scroll_to_cursor(); break;
            case '0': ed_cur_col=0; break;
            case '$': ed_cur_col=ed_line_len(ed_cur_row); break;
            case 'y': ed_visual_yank(); ed_mode=MODE_NORMAL; break;
            case 'd': ed_visual_delete(); ed_mode=MODE_NORMAL; break;
        }
        if(sc==SC_ESC) ed_mode=MODE_NORMAL;
        return true;
    }

    /* ── MODE_NORMAL ──────────────────────────────────────────────────── */
    {
        char c=keyboard_to_char(sc,ed_shift,ed_caps);

        if(c=='i') { ed_mode=MODE_INSERT; ed_g_pending=false; return true; }
        if(c=='I') { ed_mode=MODE_INSERT; ed_cur_col=0; ed_g_pending=false; return true; }
        if(c=='a') { ed_mode=MODE_INSERT; if(ed_cur_col<ed_line_len(ed_cur_row))ed_cur_col++; ed_g_pending=false; return true; }
        if(c=='A') { ed_mode=MODE_INSERT; ed_cur_col=ed_line_len(ed_cur_row); ed_g_pending=false; return true; }
        if(c=='o') { ed_cur_col=ed_line_len(ed_cur_row); ed_newline(); ed_mode=MODE_INSERT; ed_g_pending=false; return true; }
        if(c=='O') {
            ed_undo_push();
            if(ed_line_count>=MAX_LINES) return true;
            for(int i=ed_line_count-1;i>=ed_cur_row;i--)
                ed_strcpy(ed_lines[i+1],ed_lines[i]);
            ed_line_count++;
            ed_strcpy(ed_lines[ed_cur_row],"");
            ed_cur_col=0; ed_mode=MODE_INSERT; ed_modified=true; ed_g_pending=false; return true;
        }
        if(c=='v') { ed_mode=MODE_VISUAL; ed_vis_row=ed_cur_row; ed_vis_col=ed_cur_col; ed_g_pending=false; return true; }
        if(c=='/') { ed_mode=MODE_SEARCH; ed_search[0]=0; ed_search_len=0; ed_g_pending=false; return true; }
        if(c=='n') { ed_find_next(ed_cur_row,ed_cur_col+1,true);  ed_g_pending=false; return true; }
        if(c=='N') { ed_find_next(0,0,false);                      ed_g_pending=false; return true; }
        if(c==':') { ed_mode=MODE_COMMAND; ed_cmd[0]=0; ed_cmd_len=0; ed_g_pending=false; return true; }

        if(c=='h'||sc==SC_LEFT)  { ed_cur_col--; ed_clamp(); ed_scroll_to_cursor(); ed_g_pending=false; return true; }
        if(c=='l'||sc==SC_RIGHT) { ed_cur_col++; ed_clamp(); ed_scroll_to_cursor(); ed_g_pending=false; return true; }
        if(c=='k'||sc==SC_UP)    { ed_cur_row--; ed_clamp(); ed_scroll_to_cursor(); ed_g_pending=false; return true; }
        if(c=='j'||sc==SC_DOWN)  { ed_cur_row++; ed_clamp(); ed_scroll_to_cursor(); ed_g_pending=false; return true; }

        if(c=='0') {
            if(ed_settings.smart_home) {
                char *line = ed_lines[ed_cur_row];
                int fnw = 0;
                while(line[fnw]==' '||line[fnw]=='\t') fnw++;
                if(ed_cur_col==0)        ed_cur_col=fnw;
                else if(ed_cur_col==fnw) ed_cur_col=0;
                else                     ed_cur_col=fnw;
            } else { ed_cur_col=0; }
            ed_g_pending=false; return true;
        }
        if(c=='$') { ed_cur_col=ed_line_len(ed_cur_row); ed_clamp(); ed_g_pending=false; return true; }
        if(c=='w') {
            char *line=ed_lines[ed_cur_row]; int ll=ed_line_len(ed_cur_row);
            while(ed_cur_col<ll&&ed_is_ident(line[ed_cur_col])) ed_cur_col++;
            while(ed_cur_col<ll&&!ed_is_ident(line[ed_cur_col])) ed_cur_col++;
            ed_clamp(); ed_g_pending=false; return true;
        }
        if(c=='b') {
            if(ed_cur_col>0) ed_cur_col--;
            char *line=ed_lines[ed_cur_row];
            while(ed_cur_col>0&&!ed_is_ident(line[ed_cur_col])) ed_cur_col--;
            while(ed_cur_col>0&&ed_is_ident(line[ed_cur_col-1])) ed_cur_col--;
            ed_clamp(); ed_g_pending=false; return true;
        }
        if(c=='g') {
            if(ed_g_pending){ed_cur_row=0;ed_cur_col=0;ed_scroll_to_cursor();ed_g_pending=false;}
            else            {ed_g_pending=true;}
            return true;
        }
        if(c=='G') { ed_cur_row=ed_line_count-1; ed_cur_col=0; ed_scroll_to_cursor(); ed_g_pending=false; return true; }

        static bool d_pending=false;
        if(c=='d') {
            if(d_pending){ed_delete_line();d_pending=false;}
            else         {d_pending=true;}
            ed_g_pending=false; return true;
        }
        d_pending=false;
        static bool y_pending=false;
        if(c=='y') {
            if(y_pending){ed_yank_line();y_pending=false;}
            else         {y_pending=true;}
            ed_g_pending=false; return true;
        }
        y_pending=false;

        if(c=='p') { ed_paste();       ed_g_pending=false; return true; }
        if(c=='x') { ed_delete_char(); ed_g_pending=false; return true; }
        if(c=='u') { ed_undo_pop();    ed_g_pending=false; return true; }
        if(c=='?') { ed_show_nums=!ed_show_nums; ed_settings.line_numbers=ed_show_nums; ed_g_pending=false; return true; }
        if(sc==SC_ESC) { ed_g_pending=false; return true; }
        ed_g_pending=false;
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