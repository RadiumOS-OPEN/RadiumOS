#include "engine.h"

#ifdef RBROWSE_HOST
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#else
#include "../utility/utility.h"
#endif

/* Host-only analysis builds (-DRB_BIG) lift the static budgets so a page far larger than the device
   can hold (GitHub ships ~3 MB of CSS) can still be laid out completely and compared with a reference
   browser. The device build never defines it. */
#ifdef RB_BIG
#define LIM(dev, big) (big)
#else
#define LIM(dev, big) (dev)
#endif

#define MAX_NODES 16384
#define MAX_BOX 16384
#define MAX_FOCUS RB_MAX_FOCUS
#define POOL_MAX LIM(1024 * 1024, 16 * 1024 * 1024)
#define RAW_MAX 262144
#define SCRIPT_KEEP_MAX 4096

/* Bits of Node.js_mask: which style properties a script has overridden. */
#define JS_COLOR 1u
#define JS_BG 2u
#define JS_DISP 8u

/* position values (mirror the RB_POS_* public enum; kept local so the layout
   code does not depend on the header being included first). */
#define POS_STATIC 0
#define POS_RELATIVE 1
#define POS_ABSOLUTE 2
#define POS_FIXED 3
#define POS_STICKY 4

/* overflow values (mirror RB_OVF_*) */
#define OVF_VISIBLE 0
#define OVF_HIDDEN 1
#define OVF_CLIP 2
#define OVF_AUTO 3
#define OVF_SCROLL 4

#define VIS_VISIBLE 0
#define VIS_HIDDEN 1
#define VIS_SHOWN 2     /* explicit visibility:visible: re-shows inside a hidden subtree */

/* cursor values (mirror RB_CURSOR_*) */
#define CUR_AUTO 0
#define CUR_DEFAULT 1
#define CUR_POINTER 2
#define CUR_TEXT 3
#define CUR_MOVE 4
#define CUR_WAIT 5
#define CUR_HELP 6
#define CUR_CROSSHAIR 7
#define CUR_NOT_ALLOWED 8
#define CUR_GRAB 9

/* A length-or-percentage property that also has an "auto" keyword and is not
   inherited. Sentinel for auto/absent: -32768 (same spirit as width). */
#define OFF_AUTO (-32768)
/* Percentage offsets are stored as OFF_PCT + percent (see parse_off). */
#define OFF_PCT 10000
#define MINW_ZERO (1 << 29)     /* declared min-width value for an explicit length 0 */
#define TR_MIX (1u << 28)      /* translate(): calc(p% + x px) */

/* opacity quantised to 0..255; below this it paints nothing at all. */
#define OPACITY_INVIS 32

#define DISP_INLINE 0
#define DISP_BLOCK 1
#define DISP_NONE 2
#define DISP_FLEX 3
#define DISP_GRID 4
#define DISP_INLINE_BLOCK 5
#define DISP_TABLE 6
#define DISP_TABLE_ROW 7
#define DISP_TABLE_CELL 8
#define DISP_LIST_ITEM 9
#define DISP_TABLE_ROWGROUP 10
#define DISP_TABLE_CAPTION 11
#define DISP_CONTENTS 12
#define DISP_INLINE_FLEX 13   /* flex container that sits in a line like an inline-block */

/* list-style-type */
#define LST_DISC 1
#define LST_CIRCLE 2
#define LST_SQUARE 3
#define LST_DECIMAL 4
#define LST_NONE 5
#define LST_LALPHA 6
#define LST_UALPHA 7
#define LST_LROMAN 8
#define LST_UROMAN 9

/* flex-direction. All four values are real: in a reverse direction the main
   axis start/end swap, so items flow from the opposite edge and
   justify-content mirrors with them (PART B2; PART B1 folded these). */
#define FLEX_ROW 0
#define FLEX_COLUMN 1
#define FLEX_ROW_REV 2
#define FLEX_COL_REV 3

/* flex-wrap */
#define FLEX_NOWRAP 0
#define FLEX_WRAP 1
#define FLEX_WRAP_REV 2

/* justify-content */
#define JUSTIFY_START 0
#define JUSTIFY_END 1
#define JUSTIFY_CENTER 2
#define JUSTIFY_BETWEEN 3
#define JUSTIFY_AROUND 4
#define JUSTIFY_EVENLY 5

/* align-items / align-self (baseline folds onto flex-start, documented) */
#define ALIGN_START 0
#define ALIGN_END 1
#define ALIGN_CENTER 2
#define ALIGN_STRETCH 3
#define ALIGN_AUTO 255   /* align-self: not set -> use the container's align-items */

/* box-sizing. LEGACY is what the engine always did: an explicit width is the
   border box, an explicit height is the content box. Pages that ask for
   content-box or border-box get the CSS meaning for both axes. */
#define BS_LEGACY 0
#define BS_CONTENT 1
#define BS_BORDER 2

/* text properties (0 = not set, inherited from the parent) */
#define TW_NORMAL 1
#define TW_BOLD 2
#define TD_NONE 1
#define TD_UNDERLINE 2
#define TD_LINETHROUGH 3
#define TT_NONE 1
#define TT_UPPER 2
#define TT_LOWER 3
#define TT_CAPITALIZE 4
#define WS_NORMAL 1
#define WS_NOWRAP 2
#define WS_PRE 3
#define WS_PREWRAP 4
#define WS_PRELINE 5
#define LH_UNSET 32767       /* line-height: >0 px, <0 -hundredths of the font height, 0 normal */
#define LS_UNSET 255         /* letter-spacing in px */
#define BC_CUR 0x1000000u    /* border colour: currentColor (above any 0xRRGGBB) */
#define RADIUS_FULL 255      /* border-radius: 50% (a circle / pill) */

/* justify-items for grid */
#define JI_START ALIGN_START     /* the same codes as align-items */
#define JI_END ALIGN_END
#define JI_CENTER ALIGN_CENTER
#define JI_STRETCH ALIGN_STRETCH

/* grid track encoding in an int16: px 0..4000; fit-content(px) = 0x2000|px;
   fr = 0x4000|hundredths (< 4096), plus GT_FR0 when its minimum is 0 (minmax(0, 1fr));
   percentage = 0x6000|percent; auto = 0x7FFF. */
#define GRID_MAX_TRACKS 12
#define GT_FIT 0x2000
#define GT_FR 0x4000
#define GT_FR0 0x1000
#define GT_PCT 0x6000
#define GT_AUTO 0x7FFF

/* align-content: positions a WRAPPED container's lines along the cross axis.
   Codes deliberately parallel justify-content plus stretch. A single-line
   (nowrap) container ignores it. Default is STRETCH, per CSS. */
#define ACNT_START 0
#define ACNT_END 1
#define ACNT_CENTER 2
#define ACNT_STRETCH 3
#define ACNT_BETWEEN 4
#define ACNT_AROUND 5
#define ACNT_EVENLY 6

/* flex-grow / flex-shrink are parsed as numbers and stored in hundredths
   (integers only; 0.5 -> 50). Clamped so the sizing math below stays in
   32-bit range on the i686 target. */
#define FLEX_SCALE 100
#define FLEX_NUM_MAX 600

/* <input type> classification for form submission and activation. Text/other
   controls carry their value; submit controls contribute only when they are the
   activated control; button and reset never submit; image submits without a
   pair (no coordinate support); checkbox and radio contribute only when the
   checked attribute is present. */
#define RB_CTL_TEXT 0
#define RB_CTL_SUBMIT 1
#define RB_CTL_BUTTON 2
#define RB_CTL_RESET 3
#define RB_CTL_IMAGE 4
#define RB_CTL_CHECKBOX 5
#define RB_CTL_RADIO 6

typedef struct {
    uint8_t kind;
    uint8_t tag;
    uint8_t dead;
    uint8_t preserve;
    uint8_t input_btn;
    uint8_t hidden;
    uint8_t method_post;    /* <form method="post">: submission unsupported */
    uint8_t disabled;       /* disabled attribute: not focusable, not submitted */
    uint8_t checked;        /* checked attribute on a checkbox/radio */
    uint8_t ctl;            /* RB_CTL_* classification of <input type> */
    RbColor color;
    RbColor js_color, js_bg;
    uint8_t js_display, js_scale;
    uint32_t js_mask;
    uint16_t parent, first, next, prev;
    uint32_t text_off, text_len;
    uint32_t id_off, id_len;
    uint32_t class_off, class_len;
    uint32_t href_off, href_len;
    uint32_t click_off, click_len;
    uint32_t style_off, style_len;
    uint32_t name_off, name_len;       /* form control name attribute */
    uint32_t attr_off, attr_len;       /* other attributes: "name\0value\0" pairs */
    uint16_t name_hash;                /* RB_TAG_OTHER only: hash of the tag name */
    uint8_t pseudo;                    /* 1/2: generated ::before / ::after element */
    uint8_t tname;                     /* RB_TAG_OTHER only: 1-based index into g_tn (the name itself), 0 = unknown */
    uint32_t action_off, action_len;   /* <form> action attribute */
    uint16_t gen;                      /* occupancy generation: a fresh value
                                          every time this slot is reused, so a
                                          stale handle can be told apart from
                                          the node that now owns the slot */
    uint8_t img_state;                 /* <img>: RB_IMG_* record filled in by the host */
    uint8_t img_fixed;                 /* <img>: last layout used an explicit width AND height */
    uint16_t img_slot;                 /* <img>: the host's image slot */
    uint16_t img_w, img_h;             /* <img>: natural size (RB_IMG_OK) */
} Node;

/* The C2/C3 properties, shared by Rule (one declaration) and Style (computed). */
typedef struct {
    uint8_t boxsz;
    uint8_t ml_auto, mr_auto;       /* margin-left/right: auto */
    uint8_t minw0;                  /* min-width is an explicit 0 (not auto) */
    uint8_t bw[4];                  /* border width: top, right, bottom, left */
    RbColor bcol[4];                /* border colour per side, or BC_CUR */
    uint8_t radius;                 /* px, or RADIUS_FULL */
    uint8_t weight, decor, transform, ws;
    uint8_t letsp;
    int16_t lineh;
    uint8_t jitems;
    uint8_t jself;                  /* justify-self on a block-level box: 0 auto, 1 start, 2 end, 3 centre */
    uint8_t gn;                     /* grid-template-columns track count (0 = none) */
    uint8_t gautokind;              /* 0 explicit, 1 auto-fill, 2 auto-fit */
    uint16_t gautomin;              /* min px for the auto-fill/fit repeat */
    int16_t gtrack[GRID_MAX_TRACKS];
    int16_t gcs, gce, grs, gre;     /* grid-column / grid-row start and end line, 0 = auto */
    uint8_t gcspan, grspan;         /* span, 0 = none given */
    int16_t gareas;                 /* grid-template-areas: g_aux index + 1, 0 = none */
    uint16_t garea;                 /* grid-area: name hash, 0 = none */
} CssExt;

typedef struct {
    RbColor color, bg;
    uint8_t scale, display, align;
    uint8_t has_bg;
    uint8_t forced_block;
    int16_t pt, pr, pb, pl;        /* content inset per side: padding + border (screen px) */
    int16_t mt, mr, mb, ml;        /* margins; ml/mr may be OFF_PCT + percent */
    uint8_t flex_dir, justify, align_items, align_self, flex_wrap, align_content;
    uint8_t row_gap, column_gap;
    uint16_t grow, shrink;         /* hundredths */
    int16_t basis;                 /* -32768 auto, >=0 px, <0 percentage */
    int16_t width, height;
    int16_t wadj, minw_adj, maxw_adj;   /* px added to a percentage width (calc(100% - 2em)) */
    int16_t min_w, max_w, min_h, max_h;
    int16_t order;
    /* PART C1 (not inherited, so they live on the element only) */
    uint8_t pos, overflow, visibility, cursor;
    uint8_t opacity;
    uint8_t z_auto;
    int16_t z_index;
    int16_t top, right, bottom, left;
    /* web layout */
    uint8_t flt, clr, valign, lstype, collapse, bspace, italic;
    int16_t indent;
    int32_t tx, ty;                /* transform: translate; px, OFF_PCT + percent of the box, or TR_MIX (see tr_px) */
    uint16_t fpx;                  /* computed font size, CSS px */
    CssExt x;
} Style;

typedef struct {
    int left, right, x, y, line_h, line_start, align, used;
    int pmb;                 /* bottom margin of the previous block sibling (collapsing) */
    int bleft, bright;       /* the block's content box, before floats narrow a line (0: off) */
} Lay;

static Node g_nodes[MAX_NODES];
static int g_node_n;
static int g_images;           /* image mode (rb_set_images), sticky across rb_open */
static int g_canvas;           /* canvas mode (rb_set_canvas), sticky across rb_open */
static RbBox g_box[MAX_BOX];
static int g_box_n;
static int g_focus[MAX_FOCUS];
static int g_focus_n;
static int g_focus_overflow;   /* focus_walk hit MAX_FOCUS this pass */
static char g_pool[POOL_MAX];
static int g_pool_n;
static int g_lay_pool_lo, g_lay_pool_hi;   /* pool span the last layout appended */
static char g_raw[RAW_MAX];
static char g_fold[RAW_MAX];
static char g_title[80];
static char g_nav[480];
static int g_nav_len;
static int g_page_h;
static int g_page_bg;
static int g_stop;
static int g_oom;
static const char *g_oom_file;   /* where the first overflow happened (for tools) */
static int g_oom_line;
#define SET_OOM() do { g_oom = 1; if (!g_oom_line) { g_oom_file = __FILE__; g_oom_line = __LINE__; } } while (0)
static int g_pseudo_dirty;   /* generated content must be rebuilt before the next layout */
static int g_quirks;        /* no doctype: tables do not inherit text-align */
#define MAX_LAY_DEPTH 100
#define MAX_INLINE_DEPTH 160
static int g_inl_depth;     /* nesting of layout_inline (inline elements) */
static int g_lay_depth;     /* nesting of layout_block calls (probes included) */
static int g_ext_right;   /* widest right edge of an empty box with an explicit size seen while measuring (-32768 = none) */
static int g_fit;         /* >0 while measuring an auto-basis flex item's natural width: justify/align/auto margins must not push content aside */
static int g_ext_skip;    /* the node whose size is being measured: it does not count itself */
static int g_intr;          /* >0 while laying out only to learn a size (probe/measure): offsets and transforms must not count */
#ifdef RBROWSE_HOST
static int g_dbg_cnt[MAX_NODES];
#endif
/* The border box each element was last really laid out at (not a measuring
   pass): element geometry for scripts (rb_dom_rect), and on the host the
   comparison against a reference browser. */
static int g_nbox[MAX_NODES][4];
static int g_nbox_set[MAX_NODES];
static int g_nbox_self[MAX_NODES][2];   /* offset (relative / translate) applied to the node's own box after layout */
#define NBOX(n, bx, by, bw, bh) do { if (!g_intr) { g_nbox[n][0] = (bx) + g_nbox_self[n][0]; g_nbox[n][1] = (by) + g_nbox_self[n][1]; g_nbox[n][2] = (bw); g_nbox[n][3] = (bh); g_nbox_set[n] = 1; g_nbox_self[n][0] = g_nbox_self[n][1] = 0; } } while (0)
#ifdef RBROWSE_HOST
static uintptr_t g_stack_lo = (uintptr_t)-1, g_stack_hi;   /* host: measure the stack instead of switching */
#endif
static int g_page_bg_html;   /* the page background came from <html> */
static int g_want_web;      /* profile of the next rb_open */
static int g_rules_dirty;

/* Free list of recycled node slots. new_node pops it before growing
   g_node_n, so repeated create/remove cycles reuse slots and never exhaust
   MAX_NODES. reset_doc clears it so node 1 is always the fresh root. */
static uint16_t g_free[MAX_NODES];
static int g_free_n;

/* Monotonic slot-occupancy counter. Handles are plain slot indices, so the
   generation must not live in the handle (RbBox.node and existing callers
   compare handles as ints); it is compared only where a stale handle could be
   dangerous. Assigned in new_node (a free_node bump would be wiped by the
   memset that reinitialises the slot).
   Bound: Node.gen is uint16_t, so a value repeats every 65,536 allocations and
   a stale {slot, gen} could alias again -- but only if ~65.5k allocations
   happened inside one dispatch walk. Path gens are re-snapshotted per
   dispatch, so page-lifetime churn does not accumulate; a wider field (or a
   64-bit counter in the handle) is not needed until that changes. */
static unsigned g_gen;

/* Per-pass compute_style memo. Filled during do_layout, cleared at its start. */
typedef struct {
    Style st;
    RbColor ic; uint8_t isc, ia;
    uint8_t valid;
} StyleCache;
static StyleCache g_style_cache[MAX_NODES];
static int g_style_hits;
static int g_style_misses;

/* collapse_path scratch. It is sequential code with no recursion, and 3KB of
   locals on the 16KB shell stack was too much alongside rb_resolve's frames. */
static char g_collapse_tmp[480];
static char g_collapse_parts[40][64];

/* --- flex layout scratch (PART B1) ---
   One flex container owns a contiguous slice [base, base+n) of a shared static
   stack, reserved for the whole time it lays out its items; a nested flex
   container reserves above that slice and releases it before returning, so no
   large arrays live on the ~16KB shell stack and nesting cannot clobber a
   parent's item list. Per container is capped at FLEX_ITEMS_PER (extra items
   are dropped rather than corrupting the slice). */
typedef struct {
    int node;
    int basis;        /* resolved main-axis base size, px (>=0 after measure), clamped to the container */
    int basis_u;      /* web: the same before that clamp (0 = was not clamped); shrink weights and targets use it */
    int basis_b;      /* web: flex base size before the min/max clamp (basis itself is the clamped hypothetical size) */
    int grow, shrink; /* hundredths */
    int main, cross;  /* resolved sizes */
    int off_main, off_cross;
    int weight;       /* shrink*basis scratch, normalized before use */
    int off_line;     /* aligned cross offset of the ITEM'S LINE */
    int line_cross;   /* cross size of the item's line (stretch target) */
    int min_main, max_main;     /* main clamps, px; 0 = unset */
    /* Cross clamps keep the RAW sign convention (0 unset, >0 px, <0 pct) so a
       percentage can be resolved against the container's final cross extent,
       which is unknown at gather time (P1 #3). */
    int min_cross, max_cross;
    int overhead;     /* cross-axis margin-box overhead (row: pad*2+mt+mb) */
    int meas_main;    /* main size used by the Pass A measure (row cross) */
    int16_t order;    /* visual sort key (hundredths, stable sort) */
    int16_t line;     /* 0-based line index in layout order */
    uint8_t align_self;   /* ALIGN_* or ALIGN_AUTO */
    int8_t stretch;       /* align resolved to stretch */
    int8_t cross_auto;    /* cross size is auto (stretch may apply) */
    int8_t fixed;         /* held at a clamp during the one re-pass */
} FlexItem;
#define FLEX_ITEMS_PER 256
#define MAX_FLEX_TOTAL 2048
static FlexItem g_flex_item[MAX_FLEX_TOTAL];
static int g_flex_top;

/* Wrapped-line book scratch (PART B2). Used only in the line-breaking and
   line-positioning phases, which are pure item arithmetic; no nested layout
   runs between writing and last reading them, and PASS B reads only
   FlexItem fields (off_main/off_cross/line), so a nested flex container
   reached from Pass B cannot clobber a parent's line state. */
static int g_flex_lstart[FLEX_ITEMS_PER];
static int g_flex_lcount[FLEX_ITEMS_PER];
static int g_flex_lcross[FLEX_ITEMS_PER];   /* per-line cross size */
static int g_flex_lpos[FLEX_ITEMS_PER];     /* aligned cross offset */

/* Forced content-box size for the next layout_block call, set by the flex item
   pass. layout_block consumes and clears them on entry, so a nested call never
   sees a stale override. -1 means "no override". */
static int g_force_w = -1;
static int g_force_h = -1;
/* Resolved cross clamp for the next layout_block call (px, -1 = none). A row
   item's cross is its height, a column item's its width; layout_block routes
   these to the matching min/max field so a percentage cross clamp survives as
   a rendered size, not just a line-cross number (P1 #3). */
static int g_force_max_w = -1;
static int g_force_min_w = -1;
static int g_force_max_h = -1;
static int g_force_min_h = -1;

/* --- PART C1 layout scratch -------------------------------------------------
   g_hide_vis / g_hide_op: while set, emit_box writes nothing, so the element
     keeps its flow space (the Lay / return-value accounting is untouched) yet
     paints nothing. visibility is inherited, so an explicit visibility:visible
     descendant clears g_hide_vis inside its own subtree (real CSS); opacity
     below OPACITY_INVIS hides the whole subtree (documented approximation).
   g_op_mult: running opacity 0..255, multiplied down the subtree.
   Clips: every box starts unclipped. A clipper (overflow != visible) knows its
     padding box only after its children return, so it intersects that rect into
     the clip of every box in its range at the END of layout_block. Descendant
     clippers finish first, so an ancestor's intersection lands on top of theirs
     and each box ends up carrying the intersection of all its clippers.
     g_clip_tab interns the rects; RbBox.clip is a 1-based index (0 = none). */
typedef struct { int16_t x, w; int32_t y, h; } ClipRect;

/* Vertical coordinates are int32 and bounded much more loosely. */
#define Y_LIMIT 4000000
static int clampy(int v) {
    if (v > Y_LIMIT) return Y_LIMIT;
    if (v < -Y_LIMIT) return -Y_LIMIT;
    return v;
}
#define MAX_CLIP 255
static ClipRect g_clip_tab[MAX_CLIP];
static int g_clip_n;

/* Containing-block frames: the stack of positioned elements currently being
   laid out (relative/absolute/fixed/sticky). An absolute element is deferred
   when the flow pass reaches it and placed when the frame of its nearest
   positioned ancestor finishes (the padding box is final then). A bounded
   array, no malloc; a frame that does not fit is skipped, so its absolute
   descendants resolve against the next frame out. */
#define MAX_CB 24
static int g_cb_node[MAX_CB];
static uint8_t g_cb_xf[MAX_CB];     /* the frame's element has a transform: it also contains fixed descendants */
static int g_cb_n;

/* Deferred out-of-flow elements. Re-deferring a node (flex re-lays its items
   in its measure passes) overwrites its entry, so repeats cannot duplicate. */
#define MAX_OOF 1024
typedef struct {
    int node;
    int cb;                 /* frame index, -1 = initial containing block */
    int sx, sy;             /* static position (flow origin when deferred) */
    RbColor ic; uint8_t isc, ia;    /* inherited triple, for compute_style */
    uint8_t hide_vis, hide_op;
    uint8_t op;
    uint8_t fixed;
    uint8_t pending;        /* 1 = waiting to be placed */
    uint8_t halign;         /* flex container child: static x is centred (1) / end-aligned (2) in [sx, sx + scw) */
    int16_t scw;
} OofEntry;
static int g_oof_halign, g_oof_scw;   /* set by layout_flex around its oof_defer call */
static OofEntry g_oof[MAX_OOF];
static int g_oof_n;
static int g_abs_depth;     /* nesting of place_oof, bounds the measure cost */

/* Computed C1 style probes, so rb_style_* can answer without re-deriving the
   inherited triple. Filled as a side effect of compute_style (like color). */
typedef struct {
    uint8_t pos, overflow, visibility, cursor, z_auto;
    uint8_t opacity;
    int16_t z_index;
} NodePos;
static NodePos g_nodepos[MAX_NODES];

/* Set while place_oof lays out one deferred element: the layout_block call it
   makes lays out the positioned root instead of deferring it again. Consumed
   (cleared) on entry so a nested absolute descendant is still deferred. */
static int g_abs_place;
static int g_hide_vis;
static int g_hide_op;
static int g_op_mult = 255;
/* Margin-box width of the block layout_block laid out last (set just before
   it returns), so an absolute element can be anchored by its right edge. */
static int g_last_outer_w;

/* Inherited text properties, in force while laying out. layout_block and the
   inline wrapper save/restore it around an element that sets any of them, and
   emit_box / place_text read it. (The ic/isc/ia triple that compute_style
   memoises is left alone; these do not go through it.) */
typedef struct {
    uint8_t weight, decor, transform, ws, letsp;
    int16_t lineh;
} TextState;
static TextState g_ts = { TW_NORMAL, TD_NONE, TT_NONE, WS_NORMAL, 0, 0 };

static int pool_add(const char *s, int n) {
    if (n < 0) n = 0;
    if (g_pool_n + n + 1 >= POOL_MAX) { SET_OOM(); return -1; }
    int off = g_pool_n;
    if (n > 0 && s) memcpy(g_pool + off, s, (size_t)n);
    g_pool[off + n] = 0;
    g_pool_n += n + 1;
    return off;
}

static void style_touch(int node) {
    g_pseudo_dirty = 1;                /* a class or text change can add or remove generated content */
    if (node > 0 && node < MAX_NODES) g_style_cache[node].valid = 0;
}

/* Part 2 forward declaration: dead nodes must not keep listeners. */
static void ev_free_node(int node);

/* Return a node slot to the free list. The slot is marked dead immediately so
   any stray scan over [1, g_node_n) skips it until it is reused. */
static void free_node(int node) {
    if (node <= 0 || node >= MAX_NODES) return;
    g_nodes[node].dead = 1;
    g_nodes[node].parent = 0;
    g_nodes[node].prev = 0;
    g_nodes[node].next = 0;
    g_nodes[node].first = 0;
    g_style_cache[node].valid = 0;
    ev_free_node(node);
    if (g_free_n < MAX_NODES) g_free[g_free_n++] = (uint16_t)node;
    else SET_OOM();
}

static int new_node(void) {
    int i;
    if (g_free_n > 0) {
        i = g_free[--g_free_n];
    } else if (g_node_n >= MAX_NODES) {
        SET_OOM();
        return 0;
    } else {
        i = g_node_n++;
    }
    memset(&g_nodes[i], 0, sizeof(g_nodes[i]));
    /* Fresh generation for every occupancy of the slot. Skip 0 so it can be
       read as "never assigned" during debugging. */
    /* gen is 16 bits: skip every value that truncates to 0, or the 65536th
       node made on a page would read as dead to rb_dom_generation */
    do g_gen++; while ((uint16_t)g_gen == 0);
    g_nodes[i].gen = (uint16_t)g_gen;
    return i;
}

static void add_child(int parent, int child) {
    if (!parent || !child) return;
    g_nodes[child].parent = (uint16_t)parent;
    g_nodes[child].next = 0;
    if (!g_nodes[parent].first) {
        g_nodes[parent].first = (uint16_t)child;
        g_nodes[child].prev = 0;
        return;
    }
    int c = g_nodes[parent].first;
    while (g_nodes[c].next) c = g_nodes[c].next;
    g_nodes[c].next = (uint16_t)child;
    g_nodes[child].prev = (uint16_t)c;
}

static int lower_ch(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

static int eq_lit(const char *s, int n, const char *lit) {
    int i = 0;
    while (lit[i]) {
        if (i >= n || lower_ch((unsigned char)s[i]) != lit[i]) return 0;
        i++;
    }
    return i == n;
}

static int starts_lit(const char *s, int n, const char *lit) {
    int i = 0;
    while (lit[i]) {
        if (i >= n || lower_ch((unsigned char)s[i]) != (unsigned char)lit[i]) return 0;
        i++;
    }
    return 1;
}

static const struct { const char *name; uint8_t tag; } g_tagnames[] = {
    {"html", RB_TAG_HTML},
    {"head", RB_TAG_HEAD},
    {"body", RB_TAG_BODY},
    {"title", RB_TAG_TITLE},
    {"style", RB_TAG_STYLE},
    {"script", RB_TAG_SCRIPT},
    {"div", RB_TAG_DIV},
    {"p", RB_TAG_P},
    {"span", RB_TAG_SPAN},
    {"a", RB_TAG_A},
    {"h1", RB_TAG_H1},
    {"h2", RB_TAG_H2},
    {"h3", RB_TAG_H3},
    {"h4", RB_TAG_H4},
    {"h5", RB_TAG_H5},
    {"h6", RB_TAG_H6},
    {"ul", RB_TAG_UL},
    {"ol", RB_TAG_OL},
    {"li", RB_TAG_LI},
    {"br", RB_TAG_BR},
    {"hr", RB_TAG_HR},
    {"img", RB_TAG_IMG},
    {"button", RB_TAG_BUTTON},
    {"input", RB_TAG_INPUT},
    {"textarea", RB_TAG_TEXTAREA},
    {"form", RB_TAG_FORM},
    {"b", RB_TAG_B},
    {"strong", RB_TAG_STRONG},
    {"i", RB_TAG_I},
    {"em", RB_TAG_EM},
    {"u", RB_TAG_U},
    {"pre", RB_TAG_PRE},
    {"code", RB_TAG_CODE},
    {"nav", RB_TAG_NAV},
    {"header", RB_TAG_HEADER},
    {"footer", RB_TAG_FOOTER},
    {"main", RB_TAG_MAIN},
    {"section", RB_TAG_SECTION},
    {"article", RB_TAG_ARTICLE},
    {"table", RB_TAG_TABLE},
    {"tr", RB_TAG_TR},
    {"td", RB_TAG_TD},
    {"th", RB_TAG_TH},
    {"center", RB_TAG_CENTER},
    {"dl", RB_TAG_DL},
    {"dt", RB_TAG_DT},
    {"dd", RB_TAG_DD},
    {"blockquote", RB_TAG_BLOCKQUOTE},
    {"figure", RB_TAG_FIGURE},
    {"figcaption", RB_TAG_FIGCAPTION},
    {"aside", RB_TAG_ASIDE},
    {"small", RB_TAG_SMALL},
    {"sub", RB_TAG_SUB},
    {"sup", RB_TAG_SUP},
    {"label", RB_TAG_LABEL},
    {"select", RB_TAG_SELECT},
    {"option", RB_TAG_OPTION},
    {"optgroup", RB_TAG_OPTGROUP},
    {"fieldset", RB_TAG_FIELDSET},
    {"legend", RB_TAG_LEGEND},
    {"thead", RB_TAG_THEAD},
    {"tbody", RB_TAG_TBODY},
    {"tfoot", RB_TAG_TFOOT},
    {"caption", RB_TAG_CAPTION},
    {"colgroup", RB_TAG_COLGROUP},
    {"col", RB_TAG_COL},
    {"address", RB_TAG_ADDRESS},
    {"details", RB_TAG_DETAILS},
    {"summary", RB_TAG_SUMMARY},
    {"abbr", RB_TAG_ABBR},
    {"cite", RB_TAG_CITE},
    {"q", RB_TAG_Q},
    {"s", RB_TAG_S},
    {"strike", RB_TAG_STRIKE},
    {"del", RB_TAG_DEL},
    {"ins", RB_TAG_INS},
    {"mark", RB_TAG_MARK},
    {"kbd", RB_TAG_KBD},
    {"samp", RB_TAG_SAMP},
    {"var", RB_TAG_VAR},
    {"tt", RB_TAG_TT},
    {"big", RB_TAG_BIG},
    {"font", RB_TAG_FONT},
    {"nobr", RB_TAG_NOBR},
    {"wbr", RB_TAG_WBR},
    {"iframe", RB_TAG_IFRAME},
    {"object", RB_TAG_OBJECT},
    {"embed", RB_TAG_EMBED},
    {"video", RB_TAG_VIDEO},
    {"audio", RB_TAG_AUDIO},
    {"canvas", RB_TAG_CANVAS},
    {"svg", RB_TAG_SVG},
    {"noscript", RB_TAG_NOSCRIPT},
    {"template", RB_TAG_TEMPLATE},
    {"meta", RB_TAG_META},
    {"link", RB_TAG_LINK},
    {"base", RB_TAG_BASE},
    {"picture", RB_TAG_PICTURE},
    {"source", RB_TAG_SOURCE},
    {"menu", RB_TAG_MENU},
    {"dir", RB_TAG_DIR},
    {"hgroup", RB_TAG_HGROUP},
    {"dialog", RB_TAG_DIALOG},
    {"dfn", RB_TAG_DFN},
    {"time", RB_TAG_TIME},
    {"data", RB_TAG_DATA},
    {"bdi", RB_TAG_BDI},
    {"bdo", RB_TAG_BDO},
    {"ruby", RB_TAG_RUBY},
    {"rt", RB_TAG_RT},
    {"rp", RB_TAG_RP},
    {"map", RB_TAG_MAP},
    {"area", RB_TAG_AREA},
    {"param", RB_TAG_PARAM},
    {"track", RB_TAG_TRACK},
    {"output", RB_TAG_OUTPUT},
    {"progress", RB_TAG_PROGRESS},
    {"meter", RB_TAG_METER},
    {"datalist", RB_TAG_DATALIST},
    {"search", RB_TAG_SEARCH},
    {"xmp", RB_TAG_XMP},
    {"listing", RB_TAG_LISTING},
    {"plaintext", RB_TAG_PLAINTEXT},
    {"frame", RB_TAG_FRAME},
    {"frameset", RB_TAG_FRAMESET},
    {"noframes", RB_TAG_NOFRAMES},
    {"applet", RB_TAG_APPLET},
    {"acronym", RB_TAG_ACRONYM},
    {"marquee", RB_TAG_MARQUEE},
    {"blink", RB_TAG_BLINK},
    {"spacer", RB_TAG_SPACER},
    {"math", RB_TAG_MATH},
};

static int tag_of(const char *s, int n) {
    if (n <= 0 || n > 12) return RB_TAG_OTHER;
    for (int i = 0; i < (int)(sizeof(g_tagnames) / sizeof(g_tagnames[0])); i++)
        if (eq_lit(s, n, g_tagnames[i].name)) return g_tagnames[i].tag;
    return RB_TAG_OTHER;
}

#include "css_value.inc"

static RbColor default_fg(void) {
    return g_web ? (RbColor)rb_map_rgb(0, 0, 0) : (RbColor)rb_map_rgb(0xFF, 0xE6, 0xC8);
}
#include "css_decl.inc"
#include "css_sel.inc"
#include "css_cascade.inc"

static int class_has(int node, const char *name, int nlen) {
    if (g_nodes[node].class_len == 0 || nlen <= 0) return 0;
    return token_in_list(g_pool + g_nodes[node].class_off, (int)g_nodes[node].class_len, name, nlen, 0);
}

/* Intern a clip rect (bounded linear search + append) and return its 1-based
   index. When the table is full the last entry is reused: the box is still
   clipped, just to a possibly different rect (documented bound). */
static int clip_intern(int x, int y, int w, int h) {
    for (int i = 0; i < g_clip_n; i++) {
        if (g_clip_tab[i].x == x && g_clip_tab[i].y == y &&
            g_clip_tab[i].w == w && g_clip_tab[i].h == h)
            return i + 1;
    }
    if (g_clip_n >= MAX_CLIP) return MAX_CLIP;
    g_clip_tab[g_clip_n].x = (int16_t)x;
    g_clip_tab[g_clip_n].y = clampy(y);
    g_clip_tab[g_clip_n].w = (int16_t)w;
    g_clip_tab[g_clip_n].h = clampy(h);
    return ++g_clip_n;
}

static int clamp16(int v) {
    if (v > 16000) return 16000;
    if (v < -16000) return -16000;
    return v;
}


static void rect_intersect(ClipRect *a, int x, int y, int w, int h) {
    int ax0 = a->x, ay0 = a->y;
    int ax1 = a->x + a->w, ay1 = a->y + a->h;
    int bx1 = x + w, by1 = y + h;
    if (x > ax0) ax0 = x;
    if (y > ay0) ay0 = y;
    if (bx1 < ax1) ax1 = bx1;
    if (by1 < ay1) ay1 = by1;
    a->x = (int16_t)clamp16(ax0);
    a->y = clampy(ay0);
    a->w = (int16_t)(ax1 > ax0 ? clamp16(ax1 - ax0) : 0);
    a->h = ay1 > ay0 ? clampy(ay1 - ay0) : 0;
}

/* Intersect the rect (x, y, w, h) into the clip of every box in
   [start, g_box_n). A box with no clip yet takes the rect itself. */
static void clip_apply_range(int start, int x, int y, int w, int h) {
    if (start < 0) start = 0;
    for (int i = start; i < g_box_n; i++) {
        ClipRect r;
        r.x = (int16_t)clamp16(x); r.y = clampy(y);
        r.w = (int16_t)clamp16(w < 0 ? 0 : w); r.h = clampy(h < 0 ? 0 : h);
        if (g_box[i].clip) {
            ClipRect c = g_clip_tab[g_box[i].clip - 1];
            rect_intersect(&r, c.x, c.y, c.w, c.h);
        }
        g_box[i].clip = (uint8_t)clip_intern(r.x, r.y, r.w, r.h);
    }
}

/* Move every box in [start, g_box_n) by (dx, dy), clip rects included. */
static void shift_range(int start, int dx, int dy) {
    if (!dx && !dy) return;
    if (start < 0) start = 0;
    for (int i = start; i < g_box_n; i++) {
        g_box[i].x = (int16_t)clamp16(g_box[i].x + dx);
        g_box[i].y = clampy(g_box[i].y + dy);
        if (g_box[i].clip) {
            ClipRect c = g_clip_tab[g_box[i].clip - 1];
            g_box[i].clip = (uint8_t)clip_intern(clamp16(c.x + dx), clampy(c.y + dy), c.w, c.h);
        }
    }
}

static int emit_box(int x, int y, int w, int h, int node, int color, int bg, int scale, int kind) {
    /* visibility:hidden / opacity:0 boxes paint nothing but still take space: a measuring pass (shrink-to-fit,
       flex basis, max-content) must see them or a hidden dropdown measures 0 wide */
    if ((g_hide_vis || g_hide_op) && !g_intr) return -1;
    if (g_box_n >= MAX_BOX) { SET_OOM(); return -1; }
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    RbBox *b = &g_box[g_box_n++];
    b->x = (int16_t)clamp16(x);
    b->y = clampy(y);
    b->w = (int16_t)clamp16(w);
    b->h = clampy(h);
    b->node = (uint16_t)node;
    b->color = (RbColor)color;
    b->bg = (RbColor)bg;
    b->scale = (uint8_t)(scale ? scale : 1);
    b->kind = (uint8_t)kind;
    b->toff = 0;
    b->tlen = 0;
    b->flags = 0;
    b->opacity = (uint8_t)g_op_mult;
    b->z = 0;
    b->clip = 0;
    b->radius = 0;
    b->ls = 0;
    if (kind == RB_BOX_TEXT) {
        if (g_ts.weight == TW_BOLD) b->flags |= RB_BOX_BOLD;
        if (g_ts.decor == TD_UNDERLINE) b->flags |= RB_BOX_UNDER;
        else if (g_ts.decor == TD_LINETHROUGH) b->flags |= RB_BOX_STRIKE;
        b->ls = g_ts.letsp;
    }
    return g_box_n - 1;
}

static void bind_text(int id, const char *s, int n) {
    if (id < 0 || n <= 0) return;
    int off = pool_add(s, n);
    if (off < 0) return;
    /* text-transform acts on the copy, so the DOM text is untouched. */
    if (g_ts.transform > TT_NONE) {
        char *d = g_pool + off;
        for (int i = 0; i < n; i++) {
            char c = d[i];
            if (g_ts.transform == TT_UPPER || (g_ts.transform == TT_CAPITALIZE && i == 0)) {
                if (c >= 'a' && c <= 'z') d[i] = (char)(c - 32);
            } else if (g_ts.transform == TT_LOWER) {
                if (c >= 'A' && c <= 'Z') d[i] = (char)(c + 32);
            }
        }
    }
    g_box[id].toff = (uint32_t)off;
    g_box[id].tlen = (uint32_t)n;
}

static void fl_adjust(Lay *L);

static void newline(Lay *L) {
    int lh = L->line_h > 0 ? L->line_h : (L->used ? 8 : 0);
    if (!L->used && L->line_h == 0) lh = 0;
    int line_w = L->x - L->left;
    int extra = (L->right - L->left) - line_w;
    if (extra < 0) extra = 0;
    int shift = 0;
#ifdef RBROWSE_HOST
#endif
    if (L->align == 1) shift = extra / 2;
    else if (L->align == 2) shift = extra;
    if (shift) {
        for (int i = L->line_start; i < g_box_n; i++) g_box[i].x = (int16_t)(g_box[i].x + shift);
    }
    /* One extra pixel so an 8px glyph and the focus ring do not sit on the next line. */
    if (lh > 0) L->y += lh + 1;
    L->x = L->left;
    L->line_h = 0;
    L->used = 0;
    L->pmb = 0;
    L->line_start = g_box_n;
    if (L->bright) {                       /* back to the full width, then around any floats */
        L->left = L->bleft;
        L->right = L->bright;
        L->x = L->left;
        fl_adjust(L);
    }
}

/* Web profile line breaking inside a word (UAX #14, the common cases): a line may
   break after a hyphen or slash that follows a letter/digit (or another hyphen)
   and is followed by a letter ("Strict-Transport-Security", "and/or"; not "-5",
   "2024/09"). True when s[i] is such a character. */
static int brk_after(const char *s, int n, int i) {
    if (!g_web || i <= 0 || i + 1 >= n) return 0;
    char c = s[i], p = s[i - 1], q = s[i + 1];
    if (c != '-' && c != '/') return 0;
    int pa = (p >= '0' && p <= '9') || (p >= 'a' && p <= 'z') || (p >= 'A' && p <= 'Z') || (c == '-' && p == '-');
    int qa = (q >= 'a' && q <= 'z') || (q >= 'A' && q <= 'Z');
    return pa && qa;
}

static void place_text(Lay *L, const char *s, int n, int node, int color, int scale, int preserve) {
    int step = 8 * (scale ? scale : 1);
    int ls = g_ts.letsp;
    int stepc = step + ls;                 /* advance per glyph, letter-spacing included */
    /* line-height: the line box is at least this tall and the glyphs sit in
       its middle. >0 px, <0 hundredths of the font height, 0/unset normal. */
    int eff = step;
    if (g_ts.lineh > 0) eff = g_ts.lineh;
    else if (g_ts.lineh < 0) eff = step * (-g_ts.lineh) / 100;
    if (eff < 4) eff = 4;
    int yoff = (eff - step) / 2;
    int nowrap = g_ts.ws == WS_NOWRAP || g_ts.ws == WS_PRE;
    if (g_ts.ws == WS_PRE || g_ts.ws == WS_PREWRAP) preserve = 1;
    int i = 0;
    while (i < n) {
        if (s[i] == '\n') {
            newline(L);
            i++;
            continue;
        }
        if (!preserve && s[i] == ' ') {
            if (L->x > L->left) {
                L->x += stepc;
                if (L->x >= L->right && !nowrap) newline(L);
                if (eff > L->line_h) L->line_h = eff;
                L->used = 1;
            }
            i++;
            continue;
        }
        int start = i;
        int limit = L->right - L->x;
        if (limit < stepc && L->x > L->left && !nowrap) {
            newline(L);
            limit = L->right - L->left;
        }
        int maxc = limit / stepc;
        if (maxc < 1) maxc = 1;
        if (nowrap) maxc = 4000;
        int count = 0;
        if (preserve) {
            while (i < n && s[i] != '\n' && count < maxc) { i++; count++; }
        } else {
            while (i < n && s[i] != ' ' && s[i] != '\n') { i++; count++; }
            if (count > maxc) {
                /* too long for the rest of the line: break after the last hyphen/slash that fits */
                int bk = 0;
                for (int k = maxc; k >= 1; k--) if (brk_after(s + start, count, k - 1)) { bk = k; break; }
                if (bk > 0) {
                    count = bk;
                    i = start + bk;
                } else if (L->x > L->left) {
                    newline(L);
                    i = start;
                    continue;
                } else {
                    i = start + maxc;
                    count = maxc;
                }
            }
        }
        if (count <= 0) { i++; continue; }
        int ww = count * stepc;
        bind_text(emit_box(L->x, L->y + yoff, ww, step, node, color, 0, scale, RB_BOX_TEXT), s + start, count);
        L->x += ww;
        if (eff > L->line_h) L->line_h = eff;
        L->used = 1;
    }
}

static int collect_rec(int node, char *out, int cap, int pos, int skip_raw);

static int text_of(int node, char *out, int cap) {
    if (cap <= 0) return 0;
    int n = collect_rec(node, out, cap, 0, 0);
    if (n < 0) n = 0;
    if (n >= cap) n = cap - 1;
    out[n] = 0;
    return n;
}

static int resolve_pct(int stored, int avail, int fill_if_tiny) {
    int pct = -stored;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (avail < 8 || pct < 1) return avail > 8 ? avail : 8;
    int w = avail * pct / 100;
    if (w < 8) return fill_if_tiny ? avail : 8;
    return w;
}

/* one_line: text field or image. Height stays one glyph row; CSS height and
   horizontal padding must not inflate it into a page-sized rectangle. */
static void shift_range_to(int a, int b, int dx, int dy);

static void place_control(Lay *L, int node, const Style *st, const char *label, int min_w, int min_h, int one_line) {
    int scale = st->scale ? st->scale : 1;
    int step = 8 * scale;
    int avail = L->right - L->left;
    if (avail < step) avail = step;
    if (avail > RB_VIEW_W) avail = RB_VIEW_W;
    int vpad = st->pt < st->pb ? st->pt : st->pb;
    if (vpad > 2) vpad = 2;
    /* The web profile honours the control's own padding (the UA gives buttons
       1px 6px); the native look keeps its old fixed inset. */
    int hpl = vpad, hpr = vpad;
    if (g_web) {
        hpl = st->pl > 24 ? 24 : st->pl;
        hpr = st->pr > 24 ? 24 : st->pr;
        vpad = st->pt < st->pb ? st->pt : st->pb;
        if (vpad > 12) vpad = 12;
    }
    int is_btn = g_nodes[node].tag == RB_TAG_BUTTON || g_nodes[node].input_btn;
    int lab_n = label ? (int)strlen(label) : 0;
    int bw = min_w;
    if (st->width < 0 && st->width != -32768) {
        bw = resolve_pct(st->width, avail, 1) + (g_web ? st->wadj : 0);
    } else if (st->width > 0) {
        bw = st->width;
        if (bw > RB_VIEW_W) bw = RB_VIEW_W;
    } else if (one_line && lab_n == 0) {
        bw = avail > 200 ? 200 : avail;
        if (g_web) { bw = 170; if (bw > avail) bw = avail; }
    } else if (g_web && one_line && !is_btn) {
        bw = 170;
    } else {
        int tw = lab_n * step + hpl + hpr;
        if (tw > bw) bw = tw;
    }
    if (bw < min_w) bw = min_w;
    if (bw > avail) bw = avail;
    if (bw < step) bw = step;

    int bh = step + vpad * 2;
    if (one_line) {
        bh = step + 4;
        if (bh < 12) bh = 12;
    } else {
        if (bh < min_h) bh = min_h;
        if (st->height > 0) {
            int h = st->height;
            if (h > (g_web ? 400 : 48)) h = g_web ? 400 : 48;
            if (h > bh || (g_web && is_btn)) bh = h;
        }
    }
    if (L->x > L->left && L->x + bw > L->right) newline(L);
    int bg = st->has_bg ? (int)st->bg : rb_map_rgb(240, 232, 220);
    int box = emit_box(L->x, L->y, bw, bh, node, st->color, bg, scale, RB_BOX_CONTROL);
    if (box >= 0 && st->x.radius && g_web) g_box[box].radius = st->x.radius == RADIUS_FULL ? (uint8_t)(bh / 2) : st->x.radius;
    if (lab_n > 0) {
        int inner = bw - hpl - hpr;
        if (inner < step) inner = step;
        int maxc = inner / step;
        if (maxc < 1) maxc = 1;
        if (one_line) {
            int n = lab_n > maxc ? maxc : lab_n;
            int ty = L->y + (bh - step) / 2;
            int tx = L->x + hpl;
            if (g_web && is_btn) tx = L->x + hpl + (inner - n * step) / 2;
            bind_text(emit_box(tx, ty, n * step, step, node, st->color, 0, scale, RB_BOX_TEXT),
                      label, n);
        } else {
            Lay t;
            memset(&t, 0, sizeof(t));
            t.left = t.x = L->x + hpl;
            t.right = t.left + inner;
            t.y = L->y + vpad;
            t.line_start = g_box_n;
            /* a button's label is centred in the button */
            if (g_web && is_btn) t.align = 1;
            place_text(&t, label, lab_n, node, st->color, scale, 0);
            if (t.used) newline(&t);
            int text_h = t.y - (L->y + vpad);
            int need = text_h + vpad * 2;
            if (need < step + vpad * 2) need = step + vpad * 2;
            if (need > (g_web ? 400 : 200)) need = g_web ? 400 : 200;
            if (need > bh && box >= 0) {
                bh = need;
                g_box[box].h = bh;
            } else if (g_web && is_btn && bh > need) {
                /* taller than its label: centre the label vertically */
                shift_range_to(t.line_start, g_box_n, 0, (bh - need) / 2);
            }
        }
    }
    L->x += bw + (g_web ? 0 : step / 2);
    if (bh > L->line_h) L->line_h = bh;
    L->used = 1;
}

static int layout_block(int node, int x, int y, int w, RbColor ic, uint8_t isc, uint8_t ia);

/* --- PART C1 helpers ------------------------------------------------------- */

/* A resolved offset: pixels, or (stored >= OFF_PCT - 200) a percentage of
   `basis`. */
static int off_px(int stored, int basis) {
    if (stored >= OFF_PCT - 200) return basis * (stored - OFF_PCT) / 100;
    return stored;
}

/* translate() operand: off_px(), plus the packed percent + px form */
static int tr_px(int32_t stored, int basis) {
    if (stored >= (int32_t)TR_MIX) {
        int pct = (int)(((uint32_t)stored & 511u)) - 256;
        int px = (int)((((uint32_t)stored >> 9) & 0x3fffu)) - 8192;
        return basis * pct / 100 + px;
    }
    return off_px((int)stored, basis);
}

/* Out of flow: removed from the flow pass and placed once its containing
   block is final. */
static int is_oof(const Style *st) {
    return st->pos == POS_ABSOLUTE || st->pos == POS_FIXED;
}

/* Any C1 property that needs a scope around the element's boxes. The static,
   visible, opaque, unclipped element (every legacy page) takes none of it. */
static int c1_active(const Style *st) {
    return st->pos != POS_STATIC || st->overflow != OVF_VISIBLE ||
           st->visibility != VIS_VISIBLE || st->opacity != 255 || st->tx || st->ty ||
           st->x.weight || st->x.decor || st->x.transform || st->x.ws ||
           st->x.lineh != LH_UNSET || st->x.letsp != LS_UNSET;
}

/* Fold an element's own text properties into the inherited state. */
static void ts_apply(const Style *st) {
    if (st->x.weight) g_ts.weight = st->x.weight;
    if (st->x.decor) g_ts.decor = st->x.decor;
    if (st->x.transform) g_ts.transform = st->x.transform;
    if (st->x.ws) g_ts.ws = st->x.ws;
    if (st->x.lineh != LH_UNSET) g_ts.lineh = st->x.lineh;
    if (st->x.letsp != LS_UNSET) g_ts.letsp = st->x.letsp;
}

/* Tag every box in [start, g_box_n) as belonging to a positioned element and
   give it the element's z-index (flat stacking context). A z-index:auto
   element only adds the flag, so a z-indexed descendant keeps its own z. */
static void mark_pos(int start, int z_auto, int z, int fixed) {
    if (start < 0) start = 0;
    for (int i = start; i < g_box_n; i++) {
        g_box[i].flags |= (uint8_t)(RB_BOX_POS | (fixed ? RB_BOX_FIXED : 0));
        if (!z_auto) g_box[i].z = (int16_t)z;
    }
}

/* Record a deferred absolute/fixed element. (sx, sy) is the static position:
   where the element would have started in flow. */
static void oof_defer(int node, const Style *st, RbColor ic, uint8_t isc, uint8_t ia,
                      int sx, int sy) {
    int slot = -1;
    for (int i = 0; i < g_oof_n; i++) {
        if (g_oof[i].node == node) { slot = i; break; }
    }
    /* A measuring pass (g_intr) that comes after the real one must not rewrite the real entry: its flow
       origin is a scratch one, and the measure's own entries are dropped when it rolls g_oof_n back. */
    if (slot >= 0 && g_intr) return;
    if (slot < 0) {
        if (g_oof_n >= MAX_OOF) { SET_OOM(); return; }
        slot = g_oof_n++;
    }
    OofEntry *e = &g_oof[slot];
    e->node = node;
    e->fixed = (uint8_t)(st->pos == POS_FIXED);
    e->cb = e->fixed ? -1 : g_cb_n - 1;
    if (e->fixed) for (int f = g_cb_n - 1; f >= 0; f--) if (g_cb_xf[f]) { e->cb = f; break; }
    e->sx = sx;
    e->sy = sy;
    e->ic = ic; e->isc = isc; e->ia = ia;
    e->halign = (uint8_t)g_oof_halign;
    e->scw = (int16_t)g_oof_scw;
    e->hide_vis = (uint8_t)g_hide_vis;
    e->hide_op = (uint8_t)g_hide_op;
    e->op = (uint8_t)g_op_mult;
    e->pending = 1;
}

/* Lay out one deferred element against the containing block
   (cbx, cby, cbw, cbh) and anchor it. The subtree is laid out at the origin
   and then moved, so only the final position depends on the anchors. */
#define DBG_SHIFT_KIDS(n, dx, dy) do { dbg_shift_kids((n), (dx), (dy)); g_nbox_self[n][0] += (dx); g_nbox_self[n][1] += (dy); } while (0)
/* An absolutely positioned subtree is laid out at (0,0) and then moved by
   shift_range; move the border boxes recorded for it too. */
static void dbg_shift_subtree(int node, int dx, int dy, int depth) {
    if (g_nbox_set[node]) { g_nbox[node][0] += dx; g_nbox[node][1] += dy; }
    if (depth > 200) return;
    for (int c = g_nodes[node].first; c; c = g_nodes[c].next) if (g_nodes[c].kind == 0) dbg_shift_subtree(c, dx, dy, depth + 1);
}
static void dbg_shift_kids(int node, int dx, int dy) {
    for (int c = g_nodes[node].first; c; c = g_nodes[c].next) if (g_nodes[c].kind == 0) dbg_shift_subtree(c, dx, dy, 1);
}

static int min_content_words(int node, RbColor ic, uint8_t isc, uint8_t ia, int sum, int depth);
static int row_sums(const Style *st);
static void place_oof(OofEntry *e, int cbx, int cby, int cbw, int cbh) {
    int node = e->node;
    if (g_nodes[node].dead || g_nodes[node].hidden) return;
    Style st = compute_style(node, e->ic, e->isc, e->ia);
    if (st.display == DISP_NONE) return;
    int has_l = st.left != OFF_AUTO, has_r = st.right != OFF_AUTO;
    int has_t = st.top != OFF_AUTO, has_b = st.bottom != OFF_AUTO;
    int L = has_l ? off_px(st.left, cbw) : 0;
    int R = has_r ? off_px(st.right, cbw) : 0;
    int T = has_t ? off_px(st.top, cbh) : 0;
    int B = has_b ? off_px(st.bottom, cbh) : 0;
    int avail = cbw - L - R;
    /* left and right both auto: the box starts at its static position, so only what is right of it is
       available to the shrink-to-fit width */
    if (g_web && !has_l && !has_r && !e->halign && e->sx > cbx && e->sx - cbx < cbw) avail = cbw - (e->sx - cbx);
    if (avail < 8) avail = 8;
    if (avail > 2000) avail = 2000;

    int save_vis = g_hide_vis, save_op = g_hide_op, save_mult = g_op_mult;
    g_hide_vis = e->hide_vis;
    g_hide_op = e->hide_op;
    g_op_mult = e->op;

    int start = g_box_n;
    int save_ext = g_ext_right;
    int w = avail;
    /* Shrink-to-fit: width auto and not stretched by left+right. The content
       is measured at the available width (boxes and pool text discarded) and
       the element is then laid out again at the width it needs. */
    int auto_w = (st.width == -32768) && !(has_l && has_r);
    if (auto_w && g_abs_depth < 3) {
        int pool_mark = g_pool_n;
        g_abs_depth++;
        g_abs_place = 1;
        g_intr++;
        layout_block(node, 0, 0, avail, e->ic, e->isc, e->ia);
        g_intr--;
        g_abs_depth--;
        int right = 0, seen = 0;
        for (int i = start; i < g_box_n; i++) {
            if (g_box[i].kind == RB_BOX_BG || g_box[i].kind == RB_BOX_BORDER || (g_box[i].flags & RB_BOX_OOF)) continue;
            int r = g_box[i].x + g_box[i].w;
            if (r > right) right = r;
            seen = 1;
        }
        g_box_n = start;
        g_pool_n = pool_mark;
        if (seen) {
            int need = right + st.pr + m_px(st.mr, cbw);
            /* shrink-to-fit = min(max-content, max(min-content, avail)): a
               longest word or fixed-size child wider than avail wins */
            w = need < 8 ? 8 : (need > 2000 ? 2000 : need);
            /* words are never broken in CSS: the longest one is a floor */
            int mn = min_content_words(node, st.color, st.scale, st.align, row_sums(&st), 0) + st.pl + st.pr + m_px(st.mr, cbw);
            if (mn > w && mn <= 2000) w = mn;
        }
    }
    /* top+bottom with auto height: stretch the content box between them. */
    if (has_t && has_b && st.height == -32768) {
        int hh = cbh - T - B - st.mt - st.mb - st.pt - st.pb;
        g_force_h = hh > 0 ? hh : 0;
    }
    g_abs_depth++;
    g_abs_place = 1;
    int outer_h = layout_block(node, 0, 0, w, e->ic, e->isc, e->ia);
    g_abs_depth--;
    int outer_w = g_last_outer_w;
    g_force_h = -1;

    int x, y;
    if (has_l) x = cbx + L;
    else if (has_r) x = cbx + cbw - R - outer_w;
    else {
        x = e->sx;
        /* the static position of a flex container's child is where it would sit as the only item */
        if (e->halign == 1) x += (e->scw - outer_w) / 2;
        else if (e->halign == 2) x += e->scw - outer_w;
    }
    if (has_t) y = cby + T;
    else if (has_b) y = cby + cbh - B - outer_h;
    else y = e->sy;
    shift_range(start, x, y);
    dbg_shift_subtree(node, x, y, 0);
    mark_pos(start, st.z_auto, st.z_index, e->fixed);
    for (int i = start; i < g_box_n; i++) g_box[i].flags |= RB_BOX_OOF;
    g_ext_right = save_ext;
    g_hide_vis = save_vis;
    g_hide_op = save_op;
    g_op_mult = save_mult;
}

/* Place every pending entry whose containing block is `frame` (a g_cb_node
   index, or -1 for the initial containing block). Placement can defer more
   entries (their frame is the placed element or an outer one), so the scan
   re-reads g_oof_n. */
static void oof_place_for(int frame, int cbx, int cby, int cbw, int cbh) {
    for (int i = 0; i < g_oof_n; i++) {
        if (!g_oof[i].pending || g_oof[i].cb != frame) continue;
        g_oof[i].pending = 0;
        OofEntry e = g_oof[i];
        place_oof(&e, cbx, cby, cbw, cbh);
    }
}

/* Stable z-order: positioned boxes float above the static flow, ordered by
   z-index and then source order. One flat stacking context. */
static RbBox g_box_tmp[MAX_BOX];
static uint16_t g_zidx[MAX_BOX];
static void sort_stacking(void) {
    int n = g_box_n, np = 0;
    for (int i = 0; i < n; i++) if (g_box[i].flags & RB_BOX_POS) np++;
    if (np == 0) return;
    int k = 0;
    for (int i = 0; i < n; i++) if (!(g_box[i].flags & RB_BOX_POS)) g_box_tmp[k++] = g_box[i];
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (!(g_box[i].flags & RB_BOX_POS)) continue;
        int j = m - 1;
        while (j >= 0 && g_box[g_zidx[j]].z > g_box[i].z) { g_zidx[j + 1] = g_zidx[j]; j--; }
        g_zidx[j + 1] = (uint16_t)i;
        m++;
    }
    for (int i = 0; i < m; i++) g_box_tmp[k++] = g_box[g_zidx[i]];
    memcpy(g_box, g_box_tmp, (size_t)n * sizeof(RbBox));
}

static int st_len_px(int stored, int avail);
static int tag_is_void(int tag);
static int raw_kind(int tag);
static int g_hot_in;       /* link node enclosing the block being laid out */
#include "layout_web.inc"
#include "layout_table.inc"

static void layout_inline(int node, Lay *L, RbColor ic, uint8_t isc, uint8_t ia, int hot);

/* A div with no box of its own and no direct text is a wrapper. Google's
   header is one link per div; flowing those children keeps the links on a row. */
static int g_fw_depth;
static int flow_wrapper_rec(int node, const Style *st);
static int flow_wrapper(int node, const Style *st) {
    if (g_fw_depth > 24) return 0;
    g_fw_depth++;
    int r = flow_wrapper_rec(node, st);
    g_fw_depth--;
    return r;
}
static int flow_wrapper_rec(int node, const Style *st) {
    if (g_nodes[node].tag != RB_TAG_DIV) return 0;
    if (st->forced_block || st->has_bg || st->pt || st->pr || st->pb || st->pl) return 0;
    if (st->ml || st->mr || st->mt || st->mb) return 0;
    if (st->width != -32768 || st->height != -32768) return 0;
    if (st->display == DISP_NONE) return 0;
    if (st->indent || st->clr || st->flt) return 0;
    if (c1_active(st)) return 0;
    for (int c = g_nodes[node].first; c; c = g_nodes[c].next) {
        if (g_nodes[c].dead) continue;
        if (g_nodes[c].kind == 1) {
            const char *s = g_pool + g_nodes[c].text_off;
            for (int i = 0; i < (int)g_nodes[c].text_len; i++) {
                char ch = s[i];
                if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') return 0;
            }
            continue;
        }
        if (g_nodes[c].hidden) continue;
        Style cs = compute_style(c, st->color, st->scale, st->align);
        if (cs.display == DISP_NONE) continue;
        /* inline-level content (a span, a link, an inline-block) needs its own line box: the div breaks the line around it */
        if (g_web && !is_block_level(cs.display)) return 0;
        if (is_block_level(cs.display) && !(cs.display == DISP_BLOCK && cs.align == st->align && flow_wrapper(c, &cs))) return 0;
    }
    return 1;
}

static void inline_gap(Lay *L) {
    int step = 8;
    if (g_web) return;           /* real pages put their spaces in the text */
    if (!L->used || L->x <= L->left) return;
    if (L->x + step > L->right) return;
    L->x += step;
    if (step > L->line_h) L->line_h = step;
}

/* A <select>'s selected (or first) option node, 0 when it has none. */
static int select_pick(int node) {
    int pick = 0, first = 0;
    for (int c = g_nodes[node].first; c; c = g_nodes[c].next) {
        if (g_nodes[c].kind != 0 || g_nodes[c].dead) continue;
        int opts[2] = { c, 0 };
        if (g_nodes[c].tag == RB_TAG_OPTGROUP) opts[0] = g_nodes[c].first;
        for (int o = opts[0]; o; o = g_nodes[o].next) {
            if (g_nodes[o].kind != 0 || g_nodes[o].tag != RB_TAG_OPTION) { if (g_nodes[c].tag != RB_TAG_OPTGROUP) break; continue; }
            if (!first) first = o;
            const char *v; int vn;
            if (attr_get(o, "selected", 8, &v, &vn)) pick = o;
            if (g_nodes[c].tag != RB_TAG_OPTGROUP) break;
        }
        (void)opts;
    }
    return pick ? pick : first;
}

/* The text of a <select>'s selected (or first) option. */
static int select_label(int node, char *out, int cap) {
    int o = select_pick(node);
    if (!o) { out[0] = 0; return 0; }
    int n = text_of(o, out, cap);
    return n;
}

static void layout_inline_el(int node, const Style *stp, Lay *L, RbColor ic, uint8_t isc, uint8_t ia, int hot) {
    Style st = *stp;
    int tag = g_nodes[node].tag;
    if (tag == RB_TAG_BR) { newline(L); return; }
    if (tag == RB_TAG_WBR) return;
    if (tag == RB_TAG_BUTTON || (tag == RB_TAG_INPUT && g_nodes[node].input_btn)) {
        char label[80];
        int n = text_of(node, label, (int)sizeof(label));
        if (n <= 0 && g_nodes[node].text_len) {
            n = g_nodes[node].text_len;
            if (n > 78) n = 78;
            memcpy(label, g_pool + g_nodes[node].text_off, (size_t)n);
        }
        label[n > 0 ? n : 0] = 0;
        if (n <= 0) {
            memcpy(label, tag == RB_TAG_BUTTON ? "button" : "ok", tag == RB_TAG_BUTTON ? 7 : 3);
        }
        if (!g_web && L->used) newline(L);
        place_control(L, node, &st, label, 24, 12, 0);
        if (!g_web) newline(L);
        return;
    }
    if (tag == RB_TAG_SELECT) {
        char label[80];
        int n = select_label(node, label, (int)sizeof(label) - 4);
        if (n <= 0) { memcpy(label, "  ", 3); n = 2; }
        label[n++] = ' '; label[n++] = 'v'; label[n] = 0;
        place_control(L, node, &st, label, 40, 12, 1);
        return;
    }
    if (tag == RB_TAG_INPUT || tag == RB_TAG_TEXTAREA) {
        char label[80];
        int n = g_nodes[node].text_len;
        if (n > 78) n = 78;
        if (n > 0) memcpy(label, g_pool + g_nodes[node].text_off, (size_t)n);
        label[n] = 0;
        int area = tag == RB_TAG_TEXTAREA;
        inline_gap(L);
        place_control(L, node, &st, label, area ? 140 : 72, area ? 24 : 12, area ? 0 : 1);
        return;
    }
    if (tag == RB_TAG_CANVAS && !g_web && g_canvas) {
        /* a canvas on a native page: the replaced-element path, as a block
           when its display says so (the native sheet knows nothing of it) */
        if (is_block_level(st.display) && L->used) newline(L);
        else inline_gap(L);
        place_replaced_inline(node, &st, L, hot);
        if (is_block_level(st.display)) newline(L);
        else if (L->x + 4 <= L->right) L->x += 4;
        return;
    }
    if (tag == RB_TAG_IMG && !g_web && img_as_box(node)) {
        /* a decoded image on a native page: the same replaced-element path,
           spaced like the native controls (that profile drops the spaces
           around inline elements) */
        inline_gap(L);
        place_replaced_inline(node, &st, L, hot);
        if (L->x + 4 <= L->right) L->x += 4;
        return;
    }
    if (tag == RB_TAG_IMG && !g_web) {
        char alt[48];
        int n = g_nodes[node].text_len;
        if (n > 46) n = 46;
        if (n > 0) memcpy(alt, g_pool + g_nodes[node].text_off, (size_t)n);
        alt[n] = 0;
        if (n <= 0) memcpy(alt, "[img]", 6);
        inline_gap(L);
        place_control(L, node, &st, alt, 24, 12, 1);
        return;
    }
    if (g_web && st.flt && !(is_oof(&st))) {
        place_float(node, &st, L, ic, isc, ia);
        return;
    }
    if (g_web && is_replaced_tag(tag) && !is_block_level(st.display)) {
        place_replaced_inline(node, &st, L, hot);
        return;
    }
    if (is_block_level(st.display)) {
        /* A flex container is block-level in its parent's flow and is never a
           transparent wrapper. */
        if (st.display == DISP_BLOCK && st.align == ia && flow_wrapper(node, &st)) {
            int nhot = hot;
            if (tag == RB_TAG_A && g_nodes[node].href_len) nhot = node;
            for (int c = g_nodes[node].first; c; c = g_nodes[c].next)
                layout_inline(c, L, st.color, st.scale, st.align, nhot);
            return;
        }
        if (L->used) newline(L);
        if (st.clr && g_fl_n > g_fl_base) {
            int cy = fl_clear_y(L->y, st.clr);
            if (cy > L->y) { L->y = cy; L->pmb = 0; if (L->bright) fl_adjust(L); }
        }
        /* adjoining vertical margins collapse: the smaller overlaps the larger */
        int ov = (L->pmb > 0 && st.mt > 0) ? (L->pmb < st.mt ? L->pmb : st.mt) : 0;
        int save_hot = g_hot_in;
        g_hot_in = hot;
        /* a block child spans the container's full width: only its lines, not
           its box, make room for floats */
        int bx0 = L->bright ? L->bleft : L->left, bx1 = L->bright ? L->bright : L->right;
        int h = layout_block(node, bx0, L->y - ov, bx1 - bx0, ic, isc, ia);
        g_hot_in = save_hot;
        L->y += h - ov;
        L->pmb = st.mb > 0 ? st.mb : 0;
        L->x = L->left;
        L->line_h = 0;
        L->used = 0;
        L->line_start = g_box_n;
        return;
    }
    if (st.display == DISP_INLINE_BLOCK || st.display == DISP_INLINE_FLEX) {
        place_inline_block(node, &st, L, ic, isc, ia);
        return;
    }
    int nhot = hot;
    if (tag == RB_TAG_A && g_nodes[node].href_len) nhot = node;
    inline_gap(L);
    for (int c = g_nodes[node].first; c; c = g_nodes[c].next)
        layout_inline(c, L, st.color, st.scale, st.align, nhot);
}

/* Inline-level layout of one node. Absolute/fixed elements are deferred (they
   take no room in the line); an inline element with C1 properties gets its
   scope here. Block and flex elements get theirs in layout_block. */
static void layout_inline_impl(int node, Lay *L, RbColor ic, uint8_t isc, uint8_t ia, int hot);

static void layout_inline(int node, Lay *L, RbColor ic, uint8_t isc, uint8_t ia, int hot) {
    /* inline elements nest without going through layout_block: bound them too */
    if (g_inl_depth >= MAX_INLINE_DEPTH) { SET_OOM(); return; }
    g_inl_depth++;
    layout_inline_impl(node, L, ic, isc, ia, hot);
    g_inl_depth--;
}

static void layout_inline_impl(int node, Lay *L, RbColor ic, uint8_t isc, uint8_t ia, int hot) {
    if (!node || g_nodes[node].dead || g_nodes[node].hidden) return;
    if (g_nodes[node].kind == 1) {
        int box_node = hot ? hot : node;
        place_text(L, g_pool + g_nodes[node].text_off, g_nodes[node].text_len,
                   box_node, ic, isc, g_nodes[node].preserve);
        return;
    }
    Style st = compute_style(node, ic, isc, ia);
    if (st.display == DISP_NONE) return;
    if (is_oof(&st)) {
        /* Static position: the start of the next line, or the cursor when the
           current line is empty. */
        /* A block-level one (display:block) would have started a new line at the left edge, not at the
           cursor the previous inline content left behind. */
        oof_defer(node, &st, ic, isc, ia, (L->used && !(g_web && is_block_level(st.display))) ? L->x : L->left,
                  L->used ? L->y + L->line_h : L->y);
        return;
    }
    if (!c1_active(&st) || is_block_level(st.display)) {
        layout_inline_el(node, &st, L, ic, isc, ia, hot);
        return;
    }
    int s_vis = g_hide_vis, s_op = g_hide_op, s_mult = g_op_mult;
    TextState s_ts = g_ts;
    int start = g_box_n;
    if (st.visibility == VIS_HIDDEN) g_hide_vis = 1;
    else if (st.visibility == VIS_SHOWN) g_hide_vis = 0;
    if (st.opacity != 255) {
        g_op_mult = g_op_mult * st.opacity / 255;
        if (g_op_mult < OPACITY_INVIS) g_hide_op = 1;
    }
    ts_apply(&st);
    layout_inline_el(node, &st, L, ic, isc, ia, hot);
    if (st.pos == POS_RELATIVE || st.pos == POS_STICKY) {
        int dx = 0, dy = 0;
        int avail = L->right - L->left;
        if (st.left != OFF_AUTO) dx = off_px(st.left, avail);
        else if (st.right != OFF_AUTO) dx = -off_px(st.right, avail);
        if (st.top != OFF_AUTO) dy = off_px(st.top, 0);
        else if (st.bottom != OFF_AUTO) dy = -off_px(st.bottom, 0);
        shift_range(start, dx, dy);
        mark_pos(start, st.z_auto, st.z_index, 0);
    }
    g_hide_vis = s_vis; g_hide_op = s_op; g_op_mult = s_mult;
    g_ts = s_ts;
}

/* Percentage flex-basis, resolved against an available size (0..100%). */
static int flex_pct(int basis, int avail) {
    int pct = -basis;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return avail * pct / 100;
}

/* Lay out a flex container's in-flow children (items) inside the content box
   (cx, cy, cw) and return the content height.
   Two passes fit the emitter: Pass A lays every item out once (boxes
   discarded) to learn its cross size and any content-based main size; the main
   axis is then resolved (basis + grow/shrink + justify); Pass B re-lays each
   item at its final position and size. Only the deliberate subset of PART B1
   is handled -- see the handoff notes. */
/* min/max stored value (0 unset, >0 px, <0 percentage) -> px. */
static int st_len_px(int stored, int avail) {
    if (stored == 0) return 0;
    if (stored > 0) return stored;
    return flex_pct(stored, avail);
}

/* Resolve an item's cross clamp to px. A percentage only resolves against a
   DEFINITE cross extent; against an indefinite (content-sized) extent CSS
   treats it as auto, i.e. no clamp (P1 #3). */
static int cross_len(int raw, int avail) {
    if (raw == 0) return 0;
    if (raw > 0) return raw;
    if (avail < 0) return 0;
    return flex_pct(raw, avail);
}

/* Resolve each item of ONE flex line [off, off+n) to its final main size:
   basis clamped to the line, then grow (positive free space, integer
   remainder to the first growing item) or shrink (negative free space,
   shift-normalized shrink*basis weights). PART B1's algorithm verbatim,
   applied per line (PART B2 wraps items into many lines). */
static void flex_fit_line(int off, int nitems, int container_main, int gap) {
    int used = gap * (nitems > 1 ? nitems - 1 : 0);
    for (int k = 0; k < nitems; k++) {
        FlexItem *it = &g_flex_item[off + k];
        if (it->basis > container_main) it->basis = container_main;
        /* The flex base size, not the min/max-clamped hypothetical size, is what free space is shared
           over (the clamp is applied afterwards by flex_clamp_line); web profile only. */
        int bs = g_web ? (it->basis_u > it->basis_b ? it->basis_u : it->basis_b) : it->basis;
        it->basis_u = bs;
        it->main = g_web ? (bs > container_main ? container_main : bs) : it->basis;
        used += bs;
    }
    int free = container_main - used;

    if (free > 0) {
        int sumg = 0;
        for (int k = 0; k < nitems; k++) sumg += g_flex_item[off + k].grow;
        if (sumg > 0) {
            int acc = 0;
            for (int k = 0; k < nitems; k++) {
                FlexItem *it = &g_flex_item[off + k];
                if (it->grow <= 0) continue;
                int add = free * it->grow / sumg;
                it->main = it->basis_u + add;
                acc += add;
            }
            int left = free - acc;   /* integer-division remainder */
            if (left != 0) {
                for (int k = 0; k < nitems; k++) {
                    if (g_flex_item[off + k].grow > 0) {
                        g_flex_item[off + k].main += left;
                        break;
                    }
                }
            }
        }
    } else if (free < 0) {
        /* shrink by shrink*basis; weights are normalized so the products stay
           well inside 32-bit on i686 (no 64-bit division helpers). */
        int mag = -free;
        int sumw = 0;
        for (int k = 0; k < nitems; k++) {
            FlexItem *it = &g_flex_item[off + k];
            int sh = it->shrink > 1000 ? 1000 : it->shrink;
            int wt = sh * it->basis_u;
            if (wt < 0) wt = 0;
            it->weight = wt;
            sumw += wt;
        }
        if (sumw > 0) {
            int shift = 0;
            while (sumw > 4096 && shift < 20) { sumw >>= 1; shift++; }
            int sumw2 = 0;
            for (int k = 0; k < nitems; k++) {
                FlexItem *it = &g_flex_item[off + k];
                int wt = it->weight >> shift;
                if (wt == 0 && it->weight > 0) wt = 1;
                it->weight = wt;
                sumw2 += wt;
            }
            if (sumw2 > 0) {
                for (int k = 0; k < nitems; k++) {
                    FlexItem *it = &g_flex_item[off + k];
                    int m = it->basis_u - mag * it->weight / sumw2;
                    it->main = m < 0 ? 0 : m;
                }
            }
        }
    }
}

/* PART B2: clamp each item's resolved main size to min/max, and if any clamp
   actually bit, re-run the distribution ONCE with the clamped items frozen at
   their clamp (documented approximation: one re-pass; if the re-pass itself
   violates a clamp, that second violation stands). Weights in the re-pass use
   plain shrink values — the basis-based weighting lost its meaning once sizes
   were clamped. No min/max set -> untouched, PART B1 arithmetic exactly. */
static void flex_clamp_line(int off, int nitems, int container_main, int gap) {
    int clamped = 0;
    for (int k = 0; k < nitems; k++) {
        FlexItem *it = &g_flex_item[off + k];
        it->fixed = 0;
        if (it->main > container_main) { it->main = container_main; it->fixed = 1; clamped = 1; }
        if (it->max_main > 0 && it->main > it->max_main) { it->main = it->max_main; it->fixed = 1; clamped = 1; }
        if (it->min_main > 0 && it->main < it->min_main) { it->main = it->min_main; it->fixed = 1; clamped = 1; }
    }
    if (!clamped) return;
    int used = gap * (nitems > 1 ? nitems - 1 : 0);
    for (int k = 0; k < nitems; k++) used += g_flex_item[off + k].main;
    int free = container_main - used;
    if (free > 0) {
        int sumg = 0;
        for (int k = 0; k < nitems; k++)
            if (!g_flex_item[off + k].fixed) sumg += g_flex_item[off + k].grow;
        if (sumg > 0) {
            int acc = 0;
            for (int k = 0; k < nitems; k++) {
                FlexItem *it = &g_flex_item[off + k];
                if (it->fixed || it->grow <= 0) continue;
                int add = free * it->grow / sumg;
                it->main += add;
                acc += add;
            }
            int left = free - acc;
            if (left != 0) {
                for (int k = 0; k < nitems; k++) {
                    FlexItem *it = &g_flex_item[off + k];
                    if (!it->fixed && it->grow > 0) { it->main += left; break; }
                }
            }
        }
    } else if (free < 0) {
        int mag = -free;
        int sumw = 0;
        for (int k = 0; k < nitems; k++) {
            FlexItem *it = &g_flex_item[off + k];
            int sh = (it->fixed || it->shrink <= 0) ? 0 :
                     (it->shrink > 1000 ? 1000 : it->shrink);
            it->weight = sh;
            sumw += sh;
        }
        if (sumw > 0) {
            for (int k = 0; k < nitems; k++) {
                FlexItem *it = &g_flex_item[off + k];
                if (it->weight == 0) continue;
                int m = it->main - mag * it->weight / sumw;
                int lo = it->min_main > 0 ? it->min_main : 0;
                it->main = m < lo ? lo : m;
            }
        }
    }
}

static int layout_flex(int node, const Style *st, int cx, int cy, int cw) {
    if (cw < 1) cw = 1;
    int dir = st->flex_dir;                       /* 0..3, all real (B2) */
    int row = (dir == FLEX_ROW || dir == FLEX_ROW_REV);
    int rowgap = st->row_gap, colgap = st->column_gap;
    /* A row's line is filled left-to-right (or right-to-left) with column-gap
       between items; its wrapped lines stack with row-gap between them. A
       column container swaps the roles. */
    int item_gap = row ? colgap : rowgap;     /* main axis, between items */
    int line_gap = row ? rowgap : colgap;     /* cross axis, between lines */
    RbColor ic = st->color; uint8_t isc = st->scale, ia = st->align;
    int base = g_flex_top;
    int off = base;
    int n = 0;

    /* Gather items: element and text children, skipping display:none/hidden
       and whitespace-only text (a removed item consumes no gap). The caps drop
       the excess items; flag it so rb_oom() reports the truncation. */
    for (int c = g_nodes[node].first; c; c = g_nodes[c].next) {
        if (n >= FLEX_ITEMS_PER || g_flex_top >= MAX_FLEX_TOTAL) { SET_OOM(); break; }
        if (g_nodes[c].dead || g_nodes[c].hidden) continue;
        int is_text = (g_nodes[c].kind == 1);
        if (is_text) {
            const char *s = g_pool + g_nodes[c].text_off;
            int ws = 1;
            for (int k = 0; k < (int)g_nodes[c].text_len; k++) {
                char ch = s[k];
                if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') { ws = 0; break; }
            }
            if (ws) continue;
        }
        Style cs;
        memset(&cs, 0, sizeof(cs));
        int wset = -32768, hset = -32768;
        if (!is_text) {
            cs = compute_style(c, ic, isc, ia);
            if (cs.display == DISP_NONE) continue;
            if (is_oof(&cs)) {
                /* Absolute/fixed children leave the flex flow entirely. Their static position is where
                   they would be as the container's only item: centred / end-aligned along the main axis
                   (justify-content) or the cross axis (align-self / align-items). */
                int ha = 0;
                if (g_web) {
                    int al = row ? 0 : (cs.align_self != ALIGN_AUTO ? cs.align_self : st->align_items);
                    int rev = (dir == FLEX_ROW_REV);
                    if (row) {
                        if (st->justify == JUSTIFY_CENTER || st->justify == JUSTIFY_AROUND || st->justify == JUSTIFY_EVENLY) ha = 1;
                        else if (st->justify == JUSTIFY_END) ha = rev ? 0 : 2;
                        else ha = rev ? 2 : 0;
                    } else {
                        if (al == ALIGN_CENTER) ha = 1;
                        else if (al == ALIGN_END) ha = 2;
                    }
                }
                g_oof_halign = ha;
                g_oof_scw = cw;
                oof_defer(c, &cs, ic, isc, ia, cx, cy);
                g_oof_halign = g_oof_scw = 0;
                continue;
            }
            wset = cs.width;
            hset = cs.height;
        }
        FlexItem *it = &g_flex_item[g_flex_top++];
        memset(it, 0, sizeof(*it));
        it->node = c;
        it->grow = is_text ? 0 : cs.grow;
        it->shrink = is_text ? FLEX_SCALE : cs.shrink;
        it->align_self = is_text ? ALIGN_AUTO : cs.align_self;
        it->order = is_text ? 0 : cs.order;
        /* Main clamps resolve now (percentages against the container's main
           extent, the best available at gather time). Cross clamps stay RAW:
           a cross percentage is resolved later against the container's final
           cross extent (P1 #3). Text items carry no min/max. The margin-box
           overhead (pad + vertical margins) is what a stretch target must
           subtract so the item's rendered box, not its content box, fills the
           line (P0 #1). */
        if (is_text) {
            it->min_main = it->max_main = 0;
            it->min_cross = it->max_cross = 0;
            it->overhead = 0;
            if (g_web && row) {
                const char *ts = g_pool + g_nodes[c].text_off;
                int run = 0, best = 0, tn = (int)g_nodes[c].text_len;
                for (int k = 0; k <= tn; k++) {
                    if (k == tn || ts[k] == ' ') { if (run > best) best = run; run = 0; }
                    else { run++; if (brk_after(ts, tn, k)) { if (run > best) best = run; run = 0; } }
                }
                it->min_main = best * 8 * (isc ? isc : 1);
            }
        } else if (row) {
            it->min_main = st_len_px(cs.min_w, cw);
            it->max_main = st_len_px(cs.max_w, cw);
            it->min_cross = cs.min_h;
            it->max_cross = cs.max_h;
            it->overhead = cs.pt + cs.pb + cs.mt + cs.mb;
            if (g_web && cs.min_w == 0 && !cs.x.minw0 && cs.overflow == OVF_VISIBLE) {
                /* min-width:auto: a flex item does not shrink below its
                   longest word (or its declared width, if that is smaller) */
                int mc = min_content_words(c, cs.color, cs.scale, cs.align, row_sums(&cs), 0) + cs.pl + cs.pr;
                if (wset > 0 && mc > wset) mc = wset;
                /* the item's main size is its margin box */
                int mlx = cs.x.ml_auto ? 0 : m_px(cs.ml, cw), mrx = cs.x.mr_auto ? 0 : m_px(cs.mr, cw);
                if (mlx > 0) mc += mlx;
                if (mrx > 0) mc += mrx;
                if (mc > it->min_main) it->min_main = mc;
            }
        } else {
            it->min_main = st_len_px(cs.min_h, cw);
            it->max_main = st_len_px(cs.max_h, cw);
            it->min_cross = cs.min_w;
            it->max_cross = cs.max_w;
            it->overhead = 0;   /* column cross is a box width, not a margin box */
        }
        int raw = is_text ? -32768 : cs.basis;
        if (row) {
            it->cross_auto = is_text ? 1 : (hset == -32768);
            if (raw != -32768) it->basis = (raw >= 0) ? raw : flex_pct(raw, cw);
            else if (wset != -32768) it->basis = (wset >= 0) ? wset : flex_pct(wset, cw);
            else it->basis = -1;                 /* auto -> measure content */
            /* main sizes are margin-box sizes (layout_block takes the margins back out of them): an
               item with a declared width/flex-basis and side margins needs room for the margins too */
            if (it->basis > 0 && !is_text) {
                it->basis += (cs.x.ml_auto ? 0 : m_px(cs.ml, cw)) + (cs.x.mr_auto ? 0 : m_px(cs.mr, cw));
                if (it->basis < 1) it->basis = 1;
            }
            if (it->basis > cw) { it->basis_u = it->basis; it->basis = cw; }
            if (it->basis < -1) it->basis = 0;
        } else {
            it->cross_auto = is_text ? 1 : (wset == -32768);
            int cmain = (st->height > 0) ? st->height - st->pt - st->pb : 0;
            if (cmain < 0) cmain = 0;
            if (raw != -32768) it->basis = (raw >= 0) ? raw : flex_pct(raw, cmain);
            else if (hset != -32768) it->basis = (hset >= 0) ? hset : flex_pct(hset, cmain);
            else it->basis = -1;                 /* auto -> measure content height */
            if (it->basis > 2000) it->basis = 2000;
            if (it->basis < -1) it->basis = 0;
        }
        n++;
    }
    if (n == 0) { g_flex_top = base; return 0; }

    /* ---- order: stable sort by the item's order property (PART B2). Items
       with equal order keep source order; implementation order is still
       source order, so a later order:0 item never jumps ahead of an earlier
       order:0 item. Ties therefore resolve like real CSS. */
    for (int i = 1; i < n; i++) {
        FlexItem key = g_flex_item[off + i];
        int j = i - 1;
        while (j >= 0 && g_flex_item[off + j].order > key.order) {
            g_flex_item[off + j + 1] = g_flex_item[off + j];
            j--;
        }
        g_flex_item[off + j + 1] = key;
    }

    /* ---- Pass A: measure (boxes discarded) ---- */
    for (int k = 0; k < n; k++) {
        FlexItem *it = &g_flex_item[off + k];
        int start = g_box_n;
        Style cs;
        memset(&cs, 0, sizeof(cs));
        int wset = -32768;
        if (g_nodes[it->node].kind != 1) {
            cs = compute_style(it->node, ic, isc, ia);
            wset = cs.width;
        }
        if (row) {
            /* Measure at the item's declared/resolved main size when it has
               one; an AUTO basis is measured at the container width as a text
               slot, but must NOT be forced into st.width: layout_block would
               write that slot width into a control's width and place_control
               would then treat it as an explicit control width, sizing an
               empty <input> to the whole slot and starving its siblings
               (P0 #2). With no forced width a control falls back to its
               intrinsic (legacy) size. A zero basis (flex:1) is likewise not
               forced, which also avoids measuring at the 8px-floored 0.
               This `cw` measure is NOT the load-bearing half of the stale-cross
               fix — Pass A2 below re-measures at the final main size; `cw` only
               keeps meas_main == main so that re-measure can be skipped. */
            int forced = it->basis > 0;
            int pw = forced ? it->basis : cw;
            it->meas_main = pw;
            int right;
            if (it->basis < 0) g_fit++;
            int h = measure_block(it->node, cx, cy, pw, ic, isc, ia, forced ? pw : -1, -1, &right);
            if (it->basis < 0) g_fit--;
            it->cross = h < 0 ? 0 : h;
            if (it->basis < 0) {
                /* content width = widest emitted box, ignoring the item's own
                   background rectangle (which always spans the provisional
                   slot and would otherwise pin every auto item to a full row). */
                int m = right < 0 ? 0 : right;
                if (g_nodes[it->node].kind == 0 && right >= 0 && (cs.display == DISP_FLEX || cs.display == DISP_INLINE_FLEX)) {
                    /* a nested flex container's text boxes end before its right padding/margin */
                    m += cs.pr + (cs.x.mr_auto ? 0 : m_px(cs.mr, cw));
                }
                int tg = g_nodes[it->node].tag;
                if (g_web && g_nodes[it->node].kind == 0 && !is_replaced_tag(tg) && tg != RB_TAG_INPUT &&
                    tg != RB_TAG_BUTTON && tg != RB_TAG_SELECT && tg != RB_TAG_TEXTAREA) {
                    /* The measure above wrapped at the container width (and justified/centred what it
                       laid out). The item's flex base size is its max-content width (text on one line);
                       a line that overflows takes the excess back in proportion to it, so a long title
                       gives up more than a short date. */
                    int mcw = block_max_content(it->node, ic, isc, ia) + (cs.x.ml_auto ? 0 : m_px(cs.ml, cw)) +
                              (cs.x.mr_auto ? 0 : m_px(cs.mr, cw));
                    m = mcw;
                    if (mcw > cw) it->basis_u = mcw;
                }
                if (m > cw) m = cw;
                it->basis = m;
            }
        } else {
            int cwid = cw;
            if (wset != -32768 && wset > 0) cwid = wset > cw ? cw : wset;
            else if (wset != -32768 && wset < 0) cwid = flex_pct(wset, cw);
            /* Same P0 #2 rule as a row: only a DECLARED width is forced into
               the measurement, so an auto-width control is sized intrinsically
               rather than to the container's whole cross extent. */
            int right;
            int h = measure_block(it->node, cx, cy, cwid, ic, isc, ia, (wset != -32768) ? cwid : -1, -1, &right);
            if (it->basis < 0) it->basis = h < 0 ? 0 : h;
            it->cross = cwid;
            /* A column item that does not stretch and has no width of its own
               is as wide as its content (align-items:center centres THAT). */
            if (wset == -32768 && right >= 0) {
                /* (loose text is an anonymous item and is shrink-wrapped the same way) */
                int al = (g_nodes[it->node].kind == 1 || cs.align_self == ALIGN_AUTO) ? st->align_items : cs.align_self;
                if (al != ALIGN_STRETCH) {
                    int fit = right + cs.pr + (cs.x.mr_auto ? 0 : m_px(cs.mr, cw));
                    if (fit < cwid) it->cross = fit < 1 ? 1 : fit;
                }
            }
        }
        g_force_w = g_force_h = -1;
        g_box_n = start;
    }

    /* ---- hypothetical main size: min/max clamp of the basis (B2) ---- */
    for (int k = 0; k < n; k++) {
        FlexItem *it = &g_flex_item[off + k];
        it->basis_b = it->basis;
        if (it->max_main > 0 && it->basis > it->max_main) it->basis = it->max_main;
        if (it->min_main > 0 && it->basis < it->min_main) it->basis = it->min_main;
    }

    /* ---- main axis of the container ---- */
    int container_main;
    if (row) {
        container_main = cw;
    } else if (st->height > 0) {
        container_main = st->height - st->pt - st->pb;
        if (container_main < 0) container_main = 0;
    } else {
        container_main = item_gap * (n > 1 ? n - 1 : 0);
        for (int k = 0; k < n; k++) container_main += g_flex_item[off + k].basis;
    }
    /* A content-sized column is sized BY its items, so a tall stack must not
       be capped: only an explicit-height (fixed) container gets the 2000px
       bound (P1 #4). */
    if (st->height > 0 && container_main > 2000) container_main = 2000;

    /* ---- break the items into lines (flex-wrap, B2) ----
       A row container always has a bounded main axis (cw), so it wraps. A
       column container wraps only against an explicit height — an
       auto-height column is sized by its own content and can never overflow,
       so no height means exactly one line (PART B1's layout). Wrapping
       breaks on the pre-distribution basis; an item wider than the
       container sits alone on its line at the clamped basis. */
    int lines = 1;
    if (st->flex_wrap != FLEX_NOWRAP) {
        /* li can reach at most n-1 <= FLEX_ITEMS_PER-1, so the arrays
           cannot overflow. */
        /* Line starts are ABSOLUTE indices into g_flex_item (off + slot), the
           same base flex_fit_line/clamp_line and the cross loop use; a nested
           wrapping container sits at off > 0 and must not read the parent's
           items at relative index 0. */
        int li = 0, lstart = off, lcnt = 0, lused = 0;
        for (int k = 0; k < n; k++) {
            FlexItem *it = &g_flex_item[off + k];
            int g = lcnt > 0 ? item_gap : 0;
            if (lcnt > 0 && lused + g + it->basis > container_main) {
                g_flex_lstart[li] = lstart;
                g_flex_lcount[li] = lcnt;
                li++;
                lstart = off + k; lcnt = 0; lused = 0; g = 0;
            }
            it->line = li;
            lcnt++;
            lused += g + it->basis;
        }
        g_flex_lstart[li] = lstart;
        g_flex_lcount[li] = lcnt;
        lines = li + 1;
    } else {
        for (int k = 0; k < n; k++) g_flex_item[off + k].line = 0;
        g_flex_lstart[0] = off;
        g_flex_lcount[0] = n;
    }

    /* ---- per line: main sizes (grow/shrink), one min/max re-pass,
       justify-content. Each wrapped line starts at the container's main
       start, so justify-content is applied per line with exactly PART B1's
       counters applied to the line's items. */
    for (int li = 0; li < lines; li++) {
        int lstart = g_flex_lstart[li];
        int cnt = g_flex_lcount[li];
        flex_fit_line(lstart, cnt, container_main, item_gap);
        flex_clamp_line(lstart, cnt, container_main, item_gap);
        int finish = item_gap * (cnt > 1 ? cnt - 1 : 0);
        for (int k = 0; k < cnt; k++) finish += g_flex_item[lstart + k].main;
        int leftover = container_main - finish;
        if (leftover < 0) leftover = 0;
        int first = 0, sp = 0;
        switch (g_fit ? JUSTIFY_START : st->justify) {
        case JUSTIFY_END: first = leftover; break;
        case JUSTIFY_CENTER: first = leftover / 2; break;
        case JUSTIFY_BETWEEN: if (cnt > 1) sp = leftover / (cnt - 1); break;
        case JUSTIFY_AROUND: sp = leftover / cnt; first = sp / 2; break;
        case JUSTIFY_EVENLY: sp = leftover / (cnt + 1); first = sp; break;
        default: break;
        }
        int cur = first;
        for (int k = 0; k < cnt; k++) {
            g_flex_item[lstart + k].off_main = cur;
            cur += g_flex_item[lstart + k].main + item_gap + sp;
        }
    }

    /* ---- Pass A2: re-measure the ROW item's cross at its FINAL main size.
       Grow/shrink/min-max can move an item's main size away from the width
       Pass A measured at (flex:1 measures at the container width, then lands
       elsewhere; a grown fixed-width item is now wider), and the line cross
       must come from the size the item is actually laid out at, not the
       provisional one. This re-measure is the LOAD-BEARING half of the
       stale-cross fix: the Pass A `cw` measure alone would leave an auto-basis
       item's cross wrong. That Pass A measure only keeps `meas_main` equal to
       `main` for the common case so this pass can be skipped (two passes for
       a flex container that does not resize its items). A column item's cross
       is its width, independent of the main extent, so only rows need this. */
    if (row) {
        for (int k = 0; k < n; k++) {
            FlexItem *it = &g_flex_item[off + k];
            if (it->main == it->meas_main) continue;
            int fwm = it->main > 0 ? it->main : 0;
            int h = measure_block(it->node, cx, cy, fwm, ic, isc, ia, fwm, -1, 0);
            it->cross = h < 0 ? 0 : h;
        }
    }

    /* ---- cross min/max: resolve percentages against the container's DEFINITE
       cross extent (an explicit height for a row, the content width for a
       column) and clamp BEFORE the line cross is taken, so both the line size
       and the alignment see the clamped cross (P1 #3). An indefinite row
       (auto height) leaves percentages unresolved, i.e. no clamp, as CSS
       does. */
    {
        int cross_avail = row ? ((st->height > 0) ? st->height - st->pt - st->pb : -1)
                              : cw;
        for (int k = 0; k < n; k++) {
            FlexItem *it = &g_flex_item[off + k];
            it->max_cross = cross_len(it->max_cross, cross_avail);
            it->min_cross = cross_len(it->min_cross, cross_avail);
            if (it->max_cross > 0 && it->cross > it->max_cross) it->cross = it->max_cross;
            if (it->min_cross > 0 && it->cross < it->min_cross) it->cross = it->min_cross;
        }
    }

    /* ---- cross axis: per-line cross size, then the container's ----
       Grouped by each item's own line index rather than walking
       g_flex_lstart/g_flex_lcount: Pass A2 above may have laid out a NESTED
       flex container, which recycles those shared arrays, so they are dead
       after the per-line main loop. it->line survives because it lives in the
       item slice. */
    for (int li = 0; li < lines; li++) g_flex_lcross[li] = 0;
    for (int k = 0; k < n; k++) {
        FlexItem *it = &g_flex_item[off + k];
        if (it->cross > g_flex_lcross[it->line])
            g_flex_lcross[it->line] = it->cross;
    }
    int cont_cross;
    if (row) {
        if (lines > 1) {
            cont_cross = line_gap * (lines - 1);
            for (int li = 0; li < lines; li++) cont_cross += g_flex_lcross[li];
        } else {
            cont_cross = g_flex_lcross[0];
        }
        if (st->height > 0 && st->height - st->pt - st->pb > cont_cross) cont_cross = st->height - st->pt - st->pb;
    } else {
        cont_cross = cw;
    }
    /* A single line fills the container's whole cross extent, exactly as
       PART B1's nowrap path did (align-content does not apply there). */
    if (lines == 1) g_flex_lcross[0] = cont_cross;

    /* ---- align-content: position the wrapped lines (B2) ----
       Ignored for a single line (nothing to distribute there). STRETCH (the
       CSS default) grows every line by the same share of the free space;
       the division remainder goes to the LAST line. */
    {
        int ltotal = line_gap * (lines > 1 ? lines - 1 : 0);
        for (int li = 0; li < lines; li++) ltotal += g_flex_lcross[li];
        int free_c = cont_cross - ltotal;
        if (free_c < 0) free_c = 0;
        int sp = 0, firstl = 0;
        if (lines > 1 && st->flex_wrap != FLEX_NOWRAP) {
            switch (st->align_content) {
            case ACNT_END: firstl = free_c; break;
            case ACNT_CENTER: firstl = free_c / 2; break;
            case ACNT_BETWEEN: sp = free_c / (lines - 1); break;
            case ACNT_AROUND: sp = free_c / lines; firstl = sp / 2; break;
            case ACNT_EVENLY: sp = free_c / (lines + 1); firstl = sp; break;
            case ACNT_STRETCH: {
                int extra = free_c / lines;
                for (int li = 0; li < lines; li++) g_flex_lcross[li] += extra;
                g_flex_lcross[lines - 1] += free_c - extra * lines;
                break;
            }
            default: break;   /* flex-start */
            }
        }
        int pos = firstl;
        for (int li = 0; li < lines; li++) {
            g_flex_lpos[li] = pos;
            pos += g_flex_lcross[li] + line_gap + sp;
        }
        /* wrap-reverse places the source-first line at the cross END: every
           line box is mirrored about the container's cross end
           (pos_rev[k] = end - pos_fwd[k] - cross[k]), which also stacks the
           source lines bottom-up. */
        if (st->flex_wrap == FLEX_WRAP_REV) {
            for (int li = 0; li < lines; li++)
                g_flex_lpos[li] = cont_cross - g_flex_lpos[li] -
                                  g_flex_lcross[li];
        }
    }

    /* ---- align-items / align-self within each item's own line ---- */
    for (int k = 0; k < n; k++) {
        FlexItem *it = &g_flex_item[off + k];
        /* it->cross was already clamped by the resolved cross min/max above. */
        int li = it->line;
        int lc = g_flex_lcross[li];
        uint8_t a = (it->align_self != ALIGN_AUTO) ? (uint8_t)it->align_self : st->align_items;
        /* measuring natural width at a huge available width: centring/end-aligning column items
           would push them to the far side of the measuring box */
        if (!row && g_probe_depth > 0 && (a == ALIGN_CENTER || a == ALIGN_END)) a = ALIGN_START;
        it->off_cross = 0;
        it->stretch = 0;
        if (a == ALIGN_STRETCH && it->cross_auto) it->stretch = 1;
        else if (a == ALIGN_END) it->off_cross = lc - it->cross;
        else if (a == ALIGN_CENTER) it->off_cross = (lc - it->cross) / 2;
        if (it->off_cross < 0) it->off_cross = 0;
        /* wrap-reverse swaps cross-start/cross-end, which also reverses the
           within-line alignment (stretch fills the line box either way). */
        if (st->flex_wrap == FLEX_WRAP_REV && lines > 1)
            it->off_cross = lc - it->off_cross - it->cross;
        it->off_line = g_flex_lpos[li];
        it->line_cross = lc;
    }

    /* ---- Pass B: lay each item out at its final position and size ----
       Reads ONLY FlexItem fields: a nested flex container laid out from here
       recycles the line scratch arrays above safely. */
    for (int k = 0; k < n; k++) {
        FlexItem *it = &g_flex_item[off + k];
        int x, y, fw, fh;
        if (row) {
            /* Reverse main axis: offsets count from the container's end
               edge, where each item occupies
               [start - off - size, start - off). */
            x = (dir == FLEX_ROW_REV)
                    ? cx + container_main - it->off_main - it->main
                    : cx + it->off_main;
            y = cy + it->off_line + it->off_cross;
            fw = it->main;
            /* The stretch target is the line's margin box; subtract this
               item's own margin+padding overhead so forcing its CONTENT height
               fills the line exactly instead of overflowing it by pad*2+mt+mb
               (P0 #1). */
            fh = -1;
            if (it->stretch) {
                fh = it->line_cross - it->overhead;
                if (fh < 0) fh = 0;
            }
        } else {
            x = cx + it->off_line + it->off_cross;
            y = (dir == FLEX_COL_REV)
                    ? cy + container_main - it->off_main - it->main
                    : cy + it->off_main;
            fw = it->stretch ? it->line_cross : it->cross;
            fh = it->main;
        }
        if (fw < 0) fw = 0;
        g_force_w = fw;
        g_force_h = fh;
        /* Make the resolved cross clamp bite the rendered box too: a row item
           is height-capped, a column item width-capped (P1 #3). */
        if (row) {
            g_force_max_h = it->max_cross > 0 ? it->max_cross : -1;
            g_force_min_h = it->min_cross > 0 ? it->min_cross : -1;
        } else {
            g_force_max_w = it->max_cross > 0 ? it->max_cross : -1;
            g_force_min_w = it->min_cross > 0 ? it->min_cross : -1;
        }
        layout_block(it->node, x, y, fw, ic, isc, ia);
        g_force_w = g_force_h = -1;
        g_force_max_w = g_force_min_w = -1;
        g_force_max_h = g_force_min_h = -1;
    }

    g_flex_top = base;
    return row ? cont_cross : container_main;
}

/* --- PART C3: grid ---------------------------------------------------------
   display:grid with grid-template-columns (px, %, fr, auto, minmax -> its
   maximum, repeat(N, ...), repeat(auto-fill|auto-fit, minmax(Npx, 1fr))),
   gap/row-gap/column-gap, grid-column / grid-row (line numbers, span, "a / b",
   negative lines), CSS-style sparse auto-placement, justify-items /
   align-items / align-self, stretch. Rows are always auto-sized from their
   items (grid-template-rows, grid-template-areas and dense packing are not
   supported). An `auto` track is sized like 1fr (no content-based sizing), and
   auto-fit does not collapse empty tracks. Integer math, bounded arrays. */
#define GRID_MAX_ITEMS 512
#define GRID_MAX_ROWS 96
typedef struct {
    int node;
    int16_t col, row;
    uint8_t cspan, rspan;
    uint8_t text;
    uint8_t align_self;
    int h;            /* measured margin-box height */
    int fitw;         /* measured shrink-to-fit margin-box width */
    int overhead;     /* pad*2 + vertical margins, for stretching */
    int auto_w, auto_h;
    int mn, mx;       /* min-/max-content margin-box widths (web profile track sizing) */
} GridItem;
static GridItem g_gitem[GRID_MAX_ITEMS];
static int g_grid_top;

/* A line number to a 0-based track index; negative lines count from the end
   (-1 is the last line). Returns -1 for "auto". */
static int grid_line(int line, int ntracks) {
    if (line == 0) return -1;
    if (line > 0) return line - 1 > ntracks ? ntracks : line - 1;
    int idx = ntracks + 1 + line;
    return idx < 0 ? 0 : idx;
}

static int grid_slot_free(const uint16_t *occ, int row, int col, int cspan, int rspan) {
    if (row + rspan > GRID_MAX_ROWS) return 0;
    uint16_t m = (uint16_t)(((1u << cspan) - 1u) << col);
    for (int r = row; r < row + rspan; r++) if (occ[r] & m) return 0;
    return 1;
}

/* Min- and max-content contribution (margin-box width) of a grid item to the
   tracks it sits in. */
static void grid_contrib(GridItem *it, int c, const Style *cs, int is_text, int cw, RbColor ic, uint8_t isc, uint8_t ia) {
    int mn, mx;
    if (is_text) {
        const char *ts = g_pool + g_nodes[c].text_off;
        int tn = (int)g_nodes[c].text_len, run = 0, best = 0, sc = isc ? isc : 1;
        for (int k = 0; k <= tn; k++) {
            if (k == tn || ts[k] == ' ' || ts[k] == '\n') { if (run > best) best = run; run = 0; }
            else { run++; if (brk_after(ts, tn, k)) { if (run > best) best = run; run = 0; } }
        }
        mn = best * 8 * sc;
        mx = tn * 8 * sc;
    } else {
        if (cs->width > 0) mn = mx = cs->width;
        else {
            mx = block_max_content(c, ic, isc, ia);
            mn = tbl_mincontent(c, cs);
            if (cs->width < 0 && cs->width != -32768) mn = 0;      /* percentage width: no intrinsic minimum */
            else if (cs->overflow != OVF_VISIBLE) mn = 0;          /* min-width:auto computes to 0 */
        }
        if (mn > mx) mx = mn;
        int ml = cs->x.ml_auto ? 0 : m_px(cs->ml, cw), mr = cs->x.mr_auto ? 0 : m_px(cs->mr, cw);
        mn += ml + mr;
        mx += ml + mr;
    }
    if (mn < 0) mn = 0;
    if (mx < mn) mx = mn;
    if (mn > cw) mn = cw;           /* one item never asks for more than the container */
    if (mx > 30000) mx = 30000;
    it->mn = mn;
    it->mx = mx;
}

/* Web-profile column sizing (CSS grid 11.x, simplified): each auto/fit-content/fr
   track gets a base size from its single-span items' min-content and a growth
   limit from their max-content; free space then grows the tracks up to their
   limits, fr tracks share what is left (never below their base), and auto tracks
   stretch to fill when no fr track exists. Spanning items add their excess to the
   spanned flexible tracks equally. */
static void grid_size_columns(const GridItem *items, int n, const int16_t *tr, int ncols, int colgap, int cw,
                              int *colw, int *colx) {
    int base[GRID_MAX_TRACKS], lim[GRID_MAX_TRACKS], kind[GRID_MAX_TRACKS];   /* kind: 0 fixed, 1 auto, 2 fit, 3 fr */
    int avail = cw - colgap * (ncols - 1);
    if (avail < 0) avail = 0;
    for (int i = 0; i < ncols; i++) {
        int t = tr[i], k = t & GT_PCT;
        base[i] = lim[i] = 0;
        if (t == GT_AUTO) kind[i] = 1;
        else if (k == GT_FIT) kind[i] = 2;
        else if (k == GT_FR) kind[i] = 3;
        else { kind[i] = 0; base[i] = lim[i] = (k == GT_PCT) ? cw * (t & 0xFF) / 100 : t; }
    }
    for (int pass = 0; pass < 2; pass++) {          /* single-span items, then spanning ones */
        for (int q = 0; q < n; q++) {
            const GridItem *it = &items[q];
            if (it->row < 0 || (pass == 0) != (it->cspan == 1)) continue;
            int c0 = it->col, c1 = it->col + it->cspan, nflex = 0, smin = 0, smax = 0, zero_min = 0;
            for (int i = c0; i < c1; i++) {
                smin += base[i]; smax += lim[i];
                if (kind[i]) nflex++;
                if (kind[i] == 3 && (tr[i] & GT_FR0)) zero_min = 1;
            }
            smin += colgap * (it->cspan - 1);
            smax += colgap * (it->cspan - 1);
            if (!nflex) continue;
            int mn = zero_min ? 0 : it->mn, mx = it->mx;
            int dmin = mn - smin, dmax = mx - smax;
            for (int i = c0, left = nflex; i < c1; i++) {
                if (!kind[i]) continue;
                if (dmin > 0) { int a = dmin / left; if (!(kind[i] == 3 && (tr[i] & GT_FR0))) base[i] += a; dmin -= a; }
                if (dmax > 0) { int a = dmax / left; lim[i] += a; dmax -= a; }
                left--;
            }
        }
    }
    int frsum = 0;
    for (int i = 0; i < ncols; i++) {
        if (kind[i] == 2) { int cap = tr[i] & 0x1FFF; if (lim[i] > cap) lim[i] = cap; }
        if (lim[i] < base[i]) lim[i] = base[i];
        if (kind[i] == 3) { lim[i] = base[i]; frsum += tr[i] & 0xFFF; }
        colw[i] = base[i];
    }
    /* maximize: free space grows non-fr tracks equally up to their limits */
    int used = 0;
    for (int i = 0; i < ncols; i++) used += colw[i];
    int free_ = avail - used;
    for (int round = 0; round < ncols && free_ > 0; round++) {
        int open = 0;
        for (int i = 0; i < ncols; i++) if (kind[i] && kind[i] != 3 && colw[i] < lim[i]) open++;
        if (!open) break;
        int per = free_ / open;
        if (per < 1) per = 1;
        for (int i = 0; i < ncols && free_ > 0; i++) {
            if (!kind[i] || kind[i] == 3 || colw[i] >= lim[i]) continue;
            int g = lim[i] - colw[i];
            if (g > per) g = per;
            if (g > free_) g = free_;
            colw[i] += g;
            free_ -= g;
        }
    }
    if (frsum > 0) {
        /* flexible tracks: find the fr size, freezing tracks whose base exceeds their share */
        int frozen[GRID_MAX_TRACKS];
        for (int i = 0; i < ncols; i++) frozen[i] = 0;
        for (int round = 0; round <= ncols; round++) {
            int space = avail, sum = 0;
            for (int i = 0; i < ncols; i++) {
                if (kind[i] == 3 && !frozen[i]) sum += tr[i] & 0xFFF;
                else space -= colw[i];
            }
            if (sum <= 0) break;
            int changed = 0;
            for (int i = 0; i < ncols; i++) {
                if (kind[i] != 3 || frozen[i]) continue;
                int w = mul_div(space > 0 ? space : 0, tr[i] & 0xFFF, sum);
                if (w < base[i]) { colw[i] = base[i]; frozen[i] = 1; changed = 1; }
                else colw[i] = w;
            }
            if (!changed) {
                /* integer slack goes to the last flexible track */
                int tot = 0, last = -1;
                for (int i = 0; i < ncols; i++) { tot += colw[i]; if (kind[i] == 3 && !frozen[i]) last = i; }
                if (last >= 0 && tot < avail) colw[last] += avail - tot;
                break;
            }
        }
    } else {
        /* stretch: leftover space goes to the auto tracks equally */
        int tot = 0, na = 0;
        for (int i = 0; i < ncols; i++) { tot += colw[i]; if (kind[i] == 1) na++; }
        int ex = avail - tot;
        for (int i = 0; i < ncols && ex > 0 && na > 0; i++) {
            if (kind[i] != 1) continue;
            int a = ex / na;
            colw[i] += a;
            ex -= a;
            na--;
        }
    }
    int x = 0;
    for (int i = 0; i < ncols; i++) { colx[i] = x; x += colw[i] + colgap; }
}

static int layout_grid(int node, const Style *st, int cx, int cy, int cw) {
    if (cw < 1) cw = 1;
    int colgap = st->column_gap, rowgap = st->row_gap;
    RbColor ic = st->color; uint8_t isc = st->scale, ia = st->align;

    /* ---- columns ---- */
    int ncols;
    int16_t tr[GRID_MAX_TRACKS];
    const int16_t *areas = st->x.gareas > 0 ? &g_aux[st->x.gareas - 1] : 0;
    if (st->x.gn == 0) {
        ncols = areas ? areas[0] : 1;
        if (ncols < 1) ncols = 1;
        if (ncols > GRID_MAX_TRACKS) ncols = GRID_MAX_TRACKS;     /* the table comes from the parser; do not rely on it */
        for (int i = 0; i < ncols; i++) tr[i] = (int16_t)(GT_FR | 100);
    } else if (st->x.gautokind) {
        int min = st->x.gautomin > 0 ? st->x.gautomin : 1;
        ncols = (cw + colgap) / (min + colgap);
        if (ncols < 1) ncols = 1;
        if (ncols > GRID_MAX_TRACKS) ncols = GRID_MAX_TRACKS;
        for (int i = 0; i < ncols; i++) tr[i] = st->x.gtrack[0];
    } else {
        ncols = st->x.gn;
        for (int i = 0; i < ncols; i++) tr[i] = st->x.gtrack[i];
        /* columns only named by grid-template-areas are implicit (auto) */
        while (areas && ncols < areas[0] && ncols < GRID_MAX_TRACKS) tr[ncols++] = GT_AUTO;
    }
    int colw[GRID_MAX_TRACKS], colx[GRID_MAX_TRACKS];
    /* The web profile sizes auto / fit-content / fr tracks from their items once
       the items are placed (below); the native profile keeps the original
       "auto is 1fr" split. */
    int need_content = 0;
    for (int i = 0; i < ncols; i++) {
        int k = tr[i] & GT_PCT;
        if (tr[i] == GT_AUTO || k == GT_FIT || k == GT_FR) need_content = 1;
    }
    need_content = need_content && g_web;
    if (!need_content) {
        int avail = cw - colgap * (ncols - 1);
        if (avail < 0) avail = 0;
        int fixed = 0, frsum = 0;
        for (int i = 0; i < ncols; i++) {
            int t = tr[i];
            if (t == GT_AUTO || (t & GT_PCT) == GT_FIT) { frsum += 100; colw[i] = -1; }
            else if ((t & GT_PCT) == GT_PCT) { colw[i] = cw * (t & 0xFF) / 100; fixed += colw[i]; }
            else if ((t & GT_PCT) == GT_FR) { frsum += (t & 0xFFF); colw[i] = -1; }
            else { colw[i] = t; fixed += t; }
        }
        int rem = avail - fixed;
        if (rem < 0) rem = 0;
        int given = 0, last_fr = -1;
        for (int i = 0; i < ncols; i++) {
            if (colw[i] >= 0) continue;
            int fr = (tr[i] == GT_AUTO || (tr[i] & GT_PCT) == GT_FIT) ? 100 : (tr[i] & 0xFFF);
            colw[i] = frsum ? rem * fr / frsum : 0;
            given += colw[i];
            last_fr = i;
        }
        if (last_fr >= 0 && frsum) colw[last_fr] += rem - given;   /* no lost pixels */
        int x = 0;
        for (int i = 0; i < ncols; i++) { colx[i] = x; x += colw[i] + colgap; }
    }

    /* ---- gather items (the same child rules as flex) ---- */
    int base = g_grid_top, n = 0;
    for (int c = g_nodes[node].first; c; c = g_nodes[c].next) {
        if (base + n >= GRID_MAX_ITEMS) { SET_OOM(); break; }
        if (g_nodes[c].dead || g_nodes[c].hidden) continue;
        int is_text = g_nodes[c].kind == 1;
        if (is_text) {
            const char *tx = g_pool + g_nodes[c].text_off;
            int ws = 1;
            for (int k = 0; k < (int)g_nodes[c].text_len; k++) {
                char ch = tx[k];
                if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') { ws = 0; break; }
            }
            if (ws) continue;
        }
        Style cs;
        memset(&cs, 0, sizeof(cs));
        if (!is_text) {
            cs = compute_style(c, ic, isc, ia);
            if (cs.display == DISP_NONE) continue;
            if (is_oof(&cs)) { oof_defer(c, &cs, ic, isc, ia, cx, cy); continue; }
        }
        GridItem *it = &g_gitem[base + n];
        memset(it, 0, sizeof(*it));
        it->node = c;
        it->text = (uint8_t)is_text;
        it->align_self = is_text ? ALIGN_AUTO : cs.align_self;
        it->overhead = is_text ? 0 : cs.pt + cs.pb + cs.mt + cs.mb;
        it->auto_w = is_text ? 1 : (cs.width == -32768);
        it->auto_h = is_text ? 1 : (cs.height == -32768);
        if (need_content) grid_contrib(it, c, &cs, is_text, cw, ic, isc, ia);
        int cspan = 1, rspan = 1, c0 = -1, r0 = -1;
        if (!is_text) {
            if (cs.x.gcspan) cspan = cs.x.gcspan;
            if (cs.x.grspan) rspan = cs.x.grspan;
            c0 = grid_line(cs.x.gcs, ncols);
            r0 = grid_line(cs.x.grs, GRID_MAX_ROWS);
            if (areas && cs.x.garea) {              /* grid-area: <name> */
                for (int a = 0; a < areas[2]; a++) {
                    const int16_t *ar = &areas[3 + a * 5];
                    if ((uint16_t)ar[0] != cs.x.garea) continue;
                    c0 = ar[1]; r0 = ar[2]; cspan = ar[3]; rspan = ar[4];
                    if (c0 < 0) c0 = -1;
                    if (r0 < 0 || r0 >= GRID_MAX_ROWS) r0 = -1;
                    cs.x.gce = cs.x.gre = 0;
                    break;
                }
            }
            if (cs.x.gce) {
                int e = grid_line(cs.x.gce, ncols);
                if (c0 >= 0 && e > c0) cspan = e - c0;
                else if (c0 < 0 && cs.x.gcspan == 0 && e > 0) { c0 = e - 1; cspan = 1; }
            }
            if (cs.x.gre) {
                int e = grid_line(cs.x.gre, GRID_MAX_ROWS);
                if (r0 >= 0 && e > r0) rspan = e - r0;
            }
        }
        if (cspan > ncols) cspan = ncols;
        if (cspan < 1) cspan = 1;
        if (rspan > 8) rspan = 8;
        if (rspan < 1) rspan = 1;
        if (c0 >= 0 && c0 + cspan > ncols) c0 = ncols - cspan;
        it->col = (int16_t)c0;
        it->row = (int16_t)r0;
        it->cspan = (uint8_t)cspan;
        it->rspan = (uint8_t)rspan;
        n++;
    }
    if (n == 0) return 0;
    g_grid_top = base + n;

    /* ---- auto-placement (sparse) ---- */
    uint16_t occ[GRID_MAX_ROWS];
    memset(occ, 0, sizeof(occ));
    int cur_r = 0, cur_c = 0, nrows = 0;
    for (int k = 0; k < n; k++) {
        GridItem *it = &g_gitem[base + k];
        int cs_ = it->cspan, rs_ = it->rspan;
        int fr_ = it->row, fc_ = it->col, pr = -1, pc = -1;
        if (fr_ >= 0 && fc_ >= 0) {
            pr = fr_; pc = fc_;           /* fully explicit: may overlap */
        } else if (fr_ >= 0) {
            for (int c = 0; c + cs_ <= ncols && pr < 0; c++)
                if (grid_slot_free(occ, fr_, c, cs_, rs_)) { pr = fr_; pc = c; }
            if (pr < 0) { pr = fr_; pc = 0; }
        } else if (fc_ >= 0) {
            for (int r = cur_r; r < GRID_MAX_ROWS && pr < 0; r++)
                if (grid_slot_free(occ, r, fc_, cs_, rs_)) { pr = r; pc = fc_; }
        } else {
            for (int r = cur_r; r < GRID_MAX_ROWS && pr < 0; r++)
                for (int c = (r == cur_r ? cur_c : 0); c + cs_ <= ncols; c++)
                    if (grid_slot_free(occ, r, c, cs_, rs_)) { pr = r; pc = c; break; }
        }
        if (pr < 0 || pr + rs_ > GRID_MAX_ROWS) {      /* out of rows: drop the item */
            SET_OOM();
            it->row = -1;
            continue;
        }
        it->row = (int16_t)pr;
        it->col = (int16_t)pc;
        uint16_t m = (uint16_t)(((1u << cs_) - 1u) << pc);
        for (int r = pr; r < pr + rs_; r++) occ[r] |= m;
        if (pr + rs_ > nrows) nrows = pr + rs_;
        if (fr_ < 0) { cur_r = pr; cur_c = pc + cs_; if (cur_c >= ncols) { cur_r++; cur_c = 0; } }
    }
    if (nrows == 0) { g_grid_top = base; return 0; }
    if (need_content) grid_size_columns(&g_gitem[base], n, tr, ncols, colgap, cw, colw, colx);

    /* ---- measure: each item's height (and shrink-to-fit width) at its span ---- */
    int pool_mark = g_pool_n;
    int rowh[GRID_MAX_ROWS], rowy[GRID_MAX_ROWS];
    for (int r = 0; r < nrows; r++) rowh[r] = 0;
    for (int k = 0; k < n; k++) {
        GridItem *it = &g_gitem[base + k];
        if (it->row < 0) continue;
        int spanw = colw[it->col + it->cspan - 1] + colx[it->col + it->cspan - 1] - colx[it->col];
        int right;
        int h = measure_block(it->node, 0, 0, spanw, ic, isc, ia, -1, -1, &right);
        it->h = h < 0 ? 0 : h;
        it->fitw = right >= 0 ? right : spanw;
        if (it->fitw > spanw) it->fitw = spanw;
        g_pool_n = pool_mark;
        if (it->rspan == 1 && it->h > rowh[it->row]) rowh[it->row] = it->h;
    }
    for (int k = 0; k < n; k++) {                      /* row spans fit the deficit */
        GridItem *it = &g_gitem[base + k];
        if (it->row < 0 || it->rspan <= 1) continue;
        int have = rowgap * (it->rspan - 1);
        for (int r = it->row; r < it->row + it->rspan; r++) have += rowh[r];
        if (it->h > have) {
            /* The deficit is shared equally between the spanned rows, with
               the remainder on the last (like CSS spreading across tracks). */
            int def = it->h - have, per = def / it->rspan;
            for (int r = it->row; r < it->row + it->rspan; r++) rowh[r] += per;
            rowh[it->row + it->rspan - 1] += def - per * it->rspan;
        }
    }
    {
        int y = 0;
        for (int r = 0; r < nrows; r++) { rowy[r] = y; y += rowh[r] + rowgap; }
    }

    /* ---- place ---- */
    for (int k = 0; k < n; k++) {
        GridItem *it = &g_gitem[base + k];
        if (it->row < 0) continue;
        int last = it->col + it->cspan - 1;
        int spanw = colw[last] + colx[last] - colx[it->col];
        int cellh = 0;
        for (int r = it->row; r < it->row + it->rspan; r++) cellh += rowh[r];
        cellh += rowgap * (it->rspan - 1);
        int jj = st->x.jitems;
        int aa = it->align_self != ALIGN_AUTO ? it->align_self : st->align_items;
        int w = spanw;
        if (jj != JI_STRETCH && it->auto_w) w = it->fitw < 1 ? 1 : it->fitw;
        if (aa == ALIGN_STRETCH && it->auto_h && !it->text) {
            int hh = cellh - it->overhead;
            g_force_h = hh > 0 ? hh : 0;
        }
        int start = g_box_n;
        int h = layout_block(it->node, cx + colx[it->col], cy + rowy[it->row], w, ic, isc, ia);
        g_force_h = -1;
        int ow = g_last_outer_w;
        int dx = 0, dy = 0;
        if (jj == JI_CENTER) dx = (spanw - ow) / 2;
        else if (jj == JI_END) dx = spanw - ow;
        if (aa == ALIGN_CENTER) dy = (cellh - h) / 2;
        else if (aa == ALIGN_END) dy = cellh - h;
        if (dx < 0) dx = 0;
        if (dy < 0) dy = 0;
        shift_range(start, dx, dy);
    }
    g_grid_top = base;
    int total = 0;
    for (int r = 0; r < nrows; r++) total += rowh[r];
    total += rowgap * (nrows - 1);
    return total;
}

/* The C1 end-of-block work, in this order: place the absolute descendants
   whose containing block this is (while its padding box is final), apply the
   relative offset to everything the element produced, clip to the padding
   box, and tag a positioned element's boxes for stacking. (ix, iy, iw, ph) is
   the padding box before any relative shift; `avail` is the width the parent
   offered, the basis for left/right percentages. */
static void c1_finish(const Style *st, int node, int box_start, int clip_start, int frame,
                      int ix, int iy, int iw, int ph, int avail, int bu) {
    /* The padding box is inside the border, side by side. */
    (void)bu;
    int px = ix + st->x.bw[3], py = iy + st->x.bw[0];
    int pw = iw - st->x.bw[3] - st->x.bw[1], phh = ph - st->x.bw[0] - st->x.bw[2];
    if (pw < 0) pw = 0;
    if (phh < 0) phh = 0;
    if (frame >= 0) {
        oof_place_for(frame, px, py, pw, phh);
        if (g_cb_n > frame) g_cb_n = frame;
    }
    if ((st->pos == POS_RELATIVE || st->pos == POS_STICKY) && !g_intr) {
        int dx = 0, dy = 0;
        if (st->left != OFF_AUTO) dx = off_px(st->left, avail);
        else if (st->right != OFF_AUTO) dx = -off_px(st->right, avail);
        /* A vertical percentage has no definite basis in an auto-height
           parent, so it resolves to 0 (like CSS, where it computes to auto). */
        if (st->top != OFF_AUTO) dy = off_px(st->top, 0);
        else if (st->bottom != OFF_AUTO) dy = -off_px(st->bottom, 0);
        shift_range(box_start, dx, dy);
        DBG_SHIFT_KIDS(node, dx, dy);
        px += dx;
        py += dy;
        mark_pos(box_start, st->z_auto, st->z_index, 0);
    }
    if ((st->tx || st->ty) && !g_intr) {
        /* transform: translate moves the painted box like a relative offset
           (percentages are of the element's own border box) */
        int dx = tr_px(st->tx, iw), dy = tr_px(st->ty, ph);
        shift_range(box_start, dx, dy);
        DBG_SHIFT_KIDS(node, dx, dy);
        px += dx;
        py += dy;
        mark_pos(box_start, st->z_auto, st->z_index, 0);
    }
    /* The clip starts after the border boxes, which belong to the element and
       sit outside its padding box. */
    if (st->overflow != OVF_VISIBLE) clip_apply_range(clip_start, px, py, pw, phh);
    (void)node;
}

static int layout_block_impl(int node, int x, int y, int w, RbColor ic, uint8_t isc, uint8_t ia);

static int layout_block(int node, int x, int y, int w, RbColor ic, uint8_t isc, uint8_t ia) {
    int save_hot = g_hot_in;
    /* A page that would take unbounded work (nested flex/tables) or nest past
       the stack is cut short rather than hanging or overflowing. */
    if (g_work > WORK_BUDGET || g_lay_depth >= MAX_LAY_DEPTH) {
        SET_OOM();
        /* the one-shot hints meant for this call must not reach the next one */
        g_force_w = g_force_h = -1;
        g_force_max_w = g_force_min_w = g_force_max_h = g_force_min_h = -1;
        g_cell_mode = 0;
        g_abs_place = 0;
        return 0;
    }
    g_work++;
#ifdef RBROWSE_HOST
    if (node > 0 && node < MAX_NODES) g_dbg_cnt[node]++;
#endif
    g_lay_depth++;
#ifdef RBROWSE_HOST
    { char m; uintptr_t a = (uintptr_t)&m; if (a < g_stack_lo) g_stack_lo = a; }
#endif
    int h = layout_block_impl(node, x, y, w, ic, isc, ia);
    g_lay_depth--;
    g_hot_in = save_hot;
    return h;
}

static int layout_block_impl(int node, int x, int y, int w, RbColor ic, uint8_t isc, uint8_t ia) {
    /* Forced content-box size from the flex item pass; consumed immediately so
       a nested layout never sees a stale override. */
    int fw = g_force_w, fh = g_force_h;
    int fxw = g_force_max_w, fnw = g_force_min_w;
    int fxh = g_force_max_h, fnh = g_force_min_h;
    g_force_w = g_force_h = -1;
    g_force_max_w = g_force_min_w = -1;
    g_force_max_h = g_force_min_h = -1;
    int placing = g_abs_place;     /* place_oof is laying out this very node */
    g_abs_place = 0;
    int cell_mode = g_cell_mode;   /* the table layout is placing this cell */
    g_cell_mode = 0;
    if (!node || g_nodes[node].dead || g_nodes[node].hidden) return 0;
    if (g_nodes[node].kind == 1) {
        Lay L;
        memset(&L, 0, sizeof(L));
        L.left = L.x = x;
        L.right = x + w;
        L.y = y;
        L.line_start = g_box_n;
        place_text(&L, g_pool + g_nodes[node].text_off, g_nodes[node].text_len, node, ic, isc, g_nodes[node].preserve);
        if (L.used) newline(&L);
        return L.y - y;
    }
    Style st = compute_style(node, ic, isc, ia);
    if (st.display == DISP_NONE) return 0;
    int ext0 = g_ext_right;
    if (!placing && is_oof(&st)) {
        /* Out of flow: no space here, placed when its containing block ends. */
        oof_defer(node, &st, ic, isc, ia, x, y);
        return 0;
    }
    /* PART C1 scope. A legacy page (static, visible, opaque, unclipped) skips
       all of it: c1 stays 0 and the code below is the pre-C1 path. */
    int c1 = c1_active(&st);
    int s_vis = g_hide_vis, s_op = g_hide_op, s_mult = g_op_mult;
    TextState s_ts = g_ts;
    int box_start = g_box_n;
    int frame = -1;
    if (c1) {
        ts_apply(&st);
        if (st.visibility == VIS_HIDDEN) g_hide_vis = 1;
        else if (st.visibility == VIS_SHOWN) g_hide_vis = 0;
        if (st.opacity != 255) {
            g_op_mult = g_op_mult * st.opacity / 255;
            if (g_op_mult < OPACITY_INVIS) g_hide_op = 1;
        }
        /* a transformed element is the containing block of its absolute AND fixed descendants too */
        if ((st.pos != POS_STATIC || (g_web && (st.tx || st.ty))) && g_cb_n < MAX_CB) {
            frame = g_cb_n;
            g_cb_xf[g_cb_n] = (uint8_t)(g_web && (st.tx || st.ty));
            g_cb_node[g_cb_n++] = node;
        }
    }
    if (g_probe_depth > 0 || g_fit) {
        /* measuring natural width: alignment and auto margins would push the
           content to the far side of the wide measuring box */
        st.align = 0;
        st.x.ml_auto = st.x.mr_auto = 0;
        st.justify = JUSTIFY_START;
    }
    if (cell_mode) {
        /* margins do not apply to table cells; a collapsed table draws each
           shared edge once */
        st.mt = st.mb = st.ml = st.mr = 0;
        st.x.ml_auto = st.x.mr_auto = 0;
        if ((cell_mode & 2) && st.x.bw[3]) { st.pl = (int16_t)(st.pl - st.x.bw[3]); st.x.bw[3] = 0; }
        if ((cell_mode & 4) && st.x.bw[0]) { st.pt = (int16_t)(st.pt - st.x.bw[0]); st.x.bw[0] = 0; }
    }
    if (fw >= 0) st.width = (int16_t)fw;
    if (fh >= 0) st.height = (int16_t)fh;
    if (fxw >= 0) st.max_w = (int16_t)fxw;
    if (fnw >= 0) st.min_w = (int16_t)fnw;
    if (fxh >= 0) st.max_h = (int16_t)fxh;
    if (fnh >= 0) st.min_h = (int16_t)fnh;
    if (st.has_bg && (g_nodes[node].tag == RB_TAG_HTML ||
                      (g_nodes[node].tag == RB_TAG_BODY && !g_page_bg_html))) {
        g_page_bg = st.bg;
        if (g_nodes[node].tag == RB_TAG_HTML) g_page_bg_html = 1;
    }
    int ml = m_px(st.ml, w), mr = m_px(st.mr, w), mt = st.mt, mb = st.mb;
    int iw = w - ml - mr;
    int iw_slot = iw;
    /* a forced slot already includes the margins; negative margins widen the content box beyond it */
    if (g_web && fw >= 0 && ml + mr < 0 && st.width == fw) st.width = (int16_t)(fw - ml - mr);
    /* The 8px floor is for a container squeezed by its parent. A flex item forced to a main size that
       leaves 0 for its content (an empty item, or one whose margins fill its basis) really is that narrow. */
    if (iw < 8 && !(g_web && fw >= 0 && iw >= 0)) iw = 8;
    if (st.width > 0) {
        int vw = st.width;
        if (g_web && fw >= 0 && iw_slot >= 1 && iw_slot < 8) {
            /* a flex item whose slot is narrower than the 8px floor (a 1px divider with margins) */
            iw = iw_slot < vw ? iw_slot : vw;
        } else {
            if (vw > iw) vw = iw;
            if (vw < iw) iw = vw;
        }
    } else if (st.width < 0 && st.width != -32768) {
        iw = resolve_pct(st.width, w > iw ? w : iw, 1) + st.wadj;
        if (iw > w - ml - mr && w - ml - mr >= 8) iw = w - ml - mr;
        if (iw < 8) iw = 8;
    }
    /* min/max-width (PART B2): applied after width resolution in every path,
       with the same sign convention (0 = unset, <0 percentage of the true
       available space). Flex items receive the same clamp later against
       their resolved item size. */
    if (st.max_w != 0) {
        int mx = st_len_px(st.max_w, w > iw ? w : iw) + (st.max_w < 0 ? st.maxw_adj : 0);
        if (mx > 0 && iw > mx) iw = mx;
    }
    int sized = st.width > 0 ? iw : 0;       /* width this box asks for by itself (measuring) */
    if (st.min_w != 0) {
        int mn = st_len_px(st.min_w, w > iw ? w : iw) + (st.min_w < 0 ? st.minw_adj : 0);
        if (st.min_w > 0 && mn > sized) sized = mn;
        if (iw < mn) iw = mn;
    }
    /* A table with an auto width is as wide as its columns want to be. */
    if (st.display == DISP_TABLE && st.width == -32768) {
        int pref = table_pref_width(node, &st, ic, isc, ia) + st.pl + st.pr;
        if (pref < iw) iw = pref < 8 ? 8 : pref;
    }
    /* A replaced element (image, svg, ...) is as wide as its content, not as
       wide as the space it sits in. */
    int rep_w = 0, rep_h = 0, is_rep = 0;
    if (g_web && is_replaced_tag(g_nodes[node].tag)) {
        is_rep = replaced_size(node, &st, iw, &rep_w, &rep_h);
        if (is_rep && (st.width == -32768 || st.width < 0)) {
            int need = rep_w + st.pl + st.pr;
            if (need < iw) iw = need;
        }
    }
    /* margin: auto on a block with a definite width takes the free space. */
    if (st.x.ml_auto || st.x.mr_auto) {
        int fixed_m = (st.x.ml_auto ? 0 : ml) + (st.x.mr_auto ? 0 : mr);
        int free_w = w - iw - fixed_m;
        if (free_w < 0) free_w = 0;
        ml = st.x.ml_auto ? 0 : ml;
        mr = st.x.mr_auto ? 0 : mr;
        if (st.x.ml_auto && st.x.mr_auto) { ml = free_w / 2; mr = free_w - free_w / 2; }
        else if (st.x.ml_auto) ml = free_w;
        else mr = free_w;
    }
    /* -webkit-center on the parent: a block narrower than its container (width, table, replaced)
       is centred when it has no auto margins */
    if (ia == 3 && g_probe_depth == 0 && !st.x.ml_auto && !st.x.mr_auto && !st.flt && !is_oof(&st) && iw + ml + mr < w) {
        ml += (w - iw - ml - mr) / 2;
    }
    /* justify-self on a block-level box with auto width: shrink to fit-content and align */
    if (st.x.jself && st.width == -32768 && !st.flt && !placing && fw < 0 && !cell_mode && g_probe_depth == 0 &&
        !is_oof(&st) && is_block_level(st.display) && st.display != DISP_TABLE_CELL && !st.x.ml_auto && !st.x.mr_auto) {
        int fit = block_max_content(node, ic, isc, ia);
        if (fit < iw) {
            int free_w = iw - fit;
            iw = fit;
            if (st.x.jself == 3) ml += free_w / 2;
            else if (st.x.jself == 2) ml += free_w;
        }
    }
    int ix = x + ml;
    int iy = y + mt;
    int bg_i = -1;
    if (st.has_bg) bg_i = emit_box(ix, iy, iw, 0, node, st.color, st.bg, 1, RB_BOX_BG);
    /* Borders. Four equal sides with a radius are one ring box; otherwise each
       side is a filled strip (BG kind, so hit testing ignores it). Geometry is
       finished once the height is known. Every border already took its room in
       the per-side inset (compute_style). */
    int bu = (st.x.bw[0] && st.x.bw[0] == st.x.bw[1] && st.x.bw[1] == st.x.bw[2] &&
              st.x.bw[2] == st.x.bw[3]) ? st.x.bw[0] : 0;
    int ring_i = -1, strip_i[4] = { -1, -1, -1, -1 };
    if (g_nodes[node].tag != RB_TAG_HR) {
        if (bu && st.x.radius) {
            int bc = st.x.bcol[0] == BC_CUR ? st.color : st.x.bcol[0];
            ring_i = emit_box(ix, iy, iw, 0, node, st.color, bc, bu, RB_BOX_BORDER);
        } else {
            for (int k = 0; k < 4; k++) {
                if (!st.x.bw[k]) continue;
                int bc = st.x.bcol[k] == BC_CUR ? st.color : st.x.bcol[k];
                strip_i[k] = emit_box(ix, iy, 0, 0, node, st.color, bc, 1, RB_BOX_BG);
            }
        }
    }
    int clip_start = g_box_n;
    if (g_nodes[node].tag == RB_TAG_HR) {
        /* the native look draws rules in the chrome's line colour (#7A3010) */
        emit_box(ix, iy + 2, iw, 1, node, g_web ? rb_map_rgb(136, 136, 136) : rb_map_rgb(122, 48, 16), 0, 1, RB_BOX_RULE);
        if (bg_i >= 0) g_box[bg_i].h = 6;
        g_last_outer_w = iw + ml + mr;
        if (c1) {
            c1_finish(&st, node, box_start, g_box_n, frame, ix, iy, iw, 6, w, 0);
            g_hide_vis = s_vis; g_hide_op = s_op; g_op_mult = s_mult;
            g_ts = s_ts;
        }
        NBOX(node, ix, iy, iw, 6);
        return mt + 6 + mb;
    }
    Lay L;
    memset(&L, 0, sizeof(L));
    L.left = L.x = ix + st.pl;
    L.right = ix + iw - st.pr;
    if (L.right < L.left + 8) L.right = L.left + 8;
    L.y = iy + st.pt;
    L.align = st.align == 3 ? 1 : st.align;   /* 3 = -webkit-center */
    L.line_start = g_box_n;
    /* formatting context: a root hides the outer floats and contains its own */
    int bfc = g_web && (st.overflow != OVF_VISIBLE || st.flt || st.display == DISP_INLINE_BLOCK ||
                        st.display == DISP_FLEX || st.display == DISP_INLINE_FLEX || st.display == DISP_GRID || st.display == DISP_TABLE ||
                        cell_mode || is_oof(&st) || placing);
    int fl_base_save = g_fl_base, fl_n_save = g_fl_n;
    if (bfc) g_fl_base = g_fl_n;
    L.bleft = L.left;
    L.bright = L.right;
    if (g_fl_n > g_fl_base) fl_adjust(&L);
    if (st.indent && st.display != DISP_FLEX && st.display != DISP_INLINE_FLEX && st.display != DISP_GRID && st.display != DISP_TABLE)
        L.x += st.indent;                              /* text-indent: the first line only */
    if (st.display == DISP_FLEX || st.display == DISP_INLINE_FLEX) {
        /* blockify children: each is laid out as a flex item, never flowed. */
        int fh_used = layout_flex(node, &st, L.left, L.y, L.right - L.left);
        L.y = (iy + st.pt) + fh_used;
        L.used = 0;
    } else if (st.display == DISP_GRID) {
        int gh_used = layout_grid(node, &st, L.left, L.y, L.right - L.left);
        L.y = (iy + st.pt) + gh_used;
        L.used = 0;
    } else if (st.display == DISP_TABLE) {
        int th = layout_table(node, &st, L.left, L.y, L.right - L.left, ic, isc, ia);
        L.y = (iy + st.pt) + th;
        L.used = 0;
    } else {
        if (g_nodes[node].tag == RB_TAG_LI && !g_web) {
            place_text(&L, "- ", 2, node, st.color, st.scale, 0);
        }
        if (g_web && st.display == DISP_LIST_ITEM) emit_list_marker(node, &st, ix, L.y);
        if (g_web && is_replaced_tag(g_nodes[node].tag)) {
            if (is_rep) {
                emit_replaced(node, g_hot_in, &st, L.left, L.y, rep_w, rep_h);
                L.y += rep_h;
            }
        } else if (g_nodes[node].tag == RB_TAG_INPUT || g_nodes[node].tag == RB_TAG_TEXTAREA ||
            g_nodes[node].tag == RB_TAG_BUTTON) {
            char label[80];
            int n = 0;
            if (g_nodes[node].tag == RB_TAG_BUTTON) n = text_of(node, label, (int)sizeof(label));
            else {
                n = g_nodes[node].text_len;
                if (n > 78) n = 78;
                if (n > 0) memcpy(label, g_pool + g_nodes[node].text_off, (size_t)n);
                label[n] = 0;
            }
            if (n == 0 && g_nodes[node].tag == RB_TAG_BUTTON) {
                memcpy(label, "button", 7);
            }
            int area = g_nodes[node].tag == RB_TAG_TEXTAREA;
            int btn = g_nodes[node].tag == RB_TAG_BUTTON || g_nodes[node].input_btn;
            int min_w = area ? 140 : (btn ? 24 : 72);
            int min_h = area ? 24 : 12;
            place_control(&L, node, &st, label, min_w, min_h, (area || btn) ? 0 : 1);
        } else {
            int hot = g_hot_in;
            if (g_nodes[node].tag == RB_TAG_A && g_nodes[node].href_len) { hot = node; g_hot_in = node; }
            for (int c = g_nodes[node].first; c; c = g_nodes[c].next)
                layout_inline(c, &L, st.color, st.scale, st.align, hot);
        }
        if (L.used) newline(&L);
    }
    if (bfc) {
        /* a root's height includes its floats; then they are gone */
        for (int fi = g_fl_base; fi < g_fl_n; fi++) if (g_fl[fi].y1 > L.y) L.y = g_fl[fi].y1;
        g_fl_n = fl_n_save;
        g_fl_base = fl_base_save;
    }
    int content_h = L.y - (iy + st.pt);
    if (content_h < 0) content_h = 0;
    int inner_h = content_h;
    if (st.height > 0) {
        int h = st.height;
        if (h > 8000) h = 8000;
        /* An explicit height is a minimum, except on a clipper: there it is
           the box height and the overflow is cut off (PART C1). */
        if (h > inner_h || st.overflow != OVF_VISIBLE) inner_h = h;
    }
    /* min/max-height (PART B2): clamps the block's inner height the same
       way an explicit height would (0 = unset). Percentage values resolve
       against the 200px height cap an explicit height is subject to. */
    if (st.max_h != 0) {
        int mx = st_len_px(st.max_h, RB_VIEW_H);
        if (mx > 8000) mx = 8000;
        if (mx > 0 && inner_h > mx) inner_h = mx;
    }
    if (st.min_h != 0) {
        int mn = st_len_px(st.min_h, RB_VIEW_H);
        if (mn > 8000) mn = 8000;
        if (inner_h < mn) inner_h = mn;
    }
    int total = inner_h + st.pt + st.pb;
    if (bg_i >= 0) g_box[bg_i].h = total;
    {
        int half = (iw < total ? iw : total) / 2;
        int rad = st.x.radius == RADIUS_FULL ? half : (st.x.radius < half ? st.x.radius : half);
        if (bg_i >= 0) g_box[bg_i].radius = (uint8_t)rad;
        if (ring_i >= 0) { g_box[ring_i].h = total; g_box[ring_i].radius = (uint8_t)rad; }
        if (strip_i[0] >= 0) { g_box[strip_i[0]].w = (int16_t)clamp16(iw); g_box[strip_i[0]].h = st.x.bw[0]; }
        if (strip_i[1] >= 0) {
            g_box[strip_i[1]].x = (int16_t)clamp16(ix + iw - st.x.bw[1]);
            g_box[strip_i[1]].w = st.x.bw[1];
            g_box[strip_i[1]].h = total;
        }
        if (strip_i[2] >= 0) {
            g_box[strip_i[2]].y = iy + total - st.x.bw[2];
            g_box[strip_i[2]].w = (int16_t)clamp16(iw);
            g_box[strip_i[2]].h = st.x.bw[2];
        }
        if (strip_i[3] >= 0) { g_box[strip_i[3]].w = st.x.bw[3]; g_box[strip_i[3]].h = total; }
    }
    g_last_outer_w = iw + ml + mr;
    if (g_intr && node != g_ext_skip && (st.width != -32768 || st.max_w != 0)) {
        /* measuring: content overflowing a box of limited width does not make
           the box (or its container) any wider */
        int lim = ix + iw;
        for (int b = box_start; b < g_box_n; b++) {
            if (g_box[b].x + g_box[b].w > lim) g_box[b].w = (int16_t)(g_box[b].x >= lim ? 0 : lim - g_box[b].x);
        }
        if (g_ext_right > ext0 && g_ext_right > lim) g_ext_right = lim > ext0 ? lim : ext0;
    }
    if (g_intr && g_ext_right > ext0 && node != g_ext_skip) g_ext_right += st.pr + mr;   /* margin-box edge of what this box held */
    if (g_intr && fw < 0 && !placing && sized && node != g_ext_skip) {
        /* a box with a definite width or min-width that paints nothing still
           makes its container at least that wide */
        int er = ix + sized + mr;
        if (er > g_ext_right) g_ext_right = er;
    }
    if (c1) {
        c1_finish(&st, node, box_start, clip_start, frame, ix, iy, iw, total, w, bu);
        g_hide_vis = s_vis; g_hide_op = s_op; g_op_mult = s_mult;
        g_ts = s_ts;
    }
    /* c1_finish places this element's absolute descendants, each a layout_block of its own: the
       width reported to the caller (place_oof, flex) is this element's again */
    g_last_outer_w = iw + ml + mr;
    NBOX(node, ix, iy, iw, total);
    return mt + total + mb;
}

static int is_focusable(int node) {
    if (g_nodes[node].hidden) return 0;
    int tag = g_nodes[node].tag;
    if (g_nodes[node].disabled && tag != RB_TAG_A) return 0;
    if (tag == RB_TAG_A) return g_nodes[node].href_len > 0;
    if (tag == RB_TAG_BUTTON) return 1;
    if (tag == RB_TAG_TEXTAREA) return 1;
    if (tag == RB_TAG_INPUT) return 1;
    return 0;
}

/* visibility:hidden removes an element from the tab order (a visible
   descendant of a hidden one is back in it), like it removes its paint. */
static int g_fw_hidden;

static void focus_walk(int node, RbColor ic, uint8_t isc, uint8_t ia) {
    if (!node || g_nodes[node].dead || g_nodes[node].kind == 1 || g_nodes[node].hidden) return;
    Style st = compute_style(node, ic, isc, ia);
    if (st.display == DISP_NONE) return;
    int saved = g_fw_hidden;
    if (st.visibility == VIS_HIDDEN) g_fw_hidden = 1;
    else if (st.visibility == VIS_SHOWN) g_fw_hidden = 0;
    if (!g_fw_hidden && is_focusable(node)) {
        if (g_focus_n < MAX_FOCUS) g_focus[g_focus_n++] = node;
        else g_focus_overflow = 1;
    }
    for (int c = g_nodes[node].first; c; c = g_nodes[c].next)
        focus_walk(c, st.color, st.scale, st.align);
    g_fw_hidden = saved;
}

/* Whether focus_walk would reach this node: it must have no hidden or
   display:none ancestor (or itself). Walks root -> node computing the same
   inherited triple focus_walk uses, so the style memo stays consistent.
   The chain is a static scratch (not a MAX_NODES local): rb_dom_is_focusable
   is reached from ev_focus on the dispatch path. It is not re-entrant and
   does not alias g_ev_walk (which only ev_radio_clear uses). */
static uint16_t g_focus_chain[MAX_NODES];
static int focus_reachable(int node) {
    int n = 0;
    for (int p = node; p > 0 && p < MAX_NODES && n < MAX_NODES; p = g_nodes[p].parent)
        g_focus_chain[n++] = (uint16_t)p;
    RbColor ic = default_fg(); uint8_t isc = 1, ia = 0;
    for (int i = n - 1; i >= 0; i--) {
        int nd = g_focus_chain[i];
        if (g_nodes[nd].dead || g_nodes[nd].hidden || g_nodes[nd].kind == 1) return 0;
        Style st = compute_style(nd, ic, isc, ia);
        if (st.display == DISP_NONE) return 0;
        ic = st.color; isc = st.scale; ia = st.align;
    }
    return 1;
}

/* Part 2: on relayout, drop the active element if it is no longer in the
   (rebuilt) focus list — e.g. a style change made it display:none. */
static void ev_prune_active(void);

/* Layout copies box text into the shared pool. Reclaim what the previous
   layout appended, unless something else was appended since (a DOM edit),
   in which case the earlier layout's text stays as a hole. Without this every
   relayout leaked the whole page's text and ~1000 of them set rb_oom. */
static void do_layout(void) {
    if (g_rules_dirty) { rules_index(); g_rules_dirty = 0; g_pseudo_dirty = 1; }
    for (int i = 0; i < MAX_NODES; i++) g_style_cache[i].valid = 0;
    g_style_hits = 0;
    g_style_misses = 0;
    if (g_web && g_pseudo_dirty) {
        /* generated content is real nodes, so it must exist before layout; the
           text pool reclaim below must not take what it appends */
        pseudo_materialize();
        g_pseudo_dirty = 0;
        for (int i = 0; i < MAX_NODES; i++) g_style_cache[i].valid = 0;
        g_lay_pool_hi = -1;
    }
    if (g_pool_n == g_lay_pool_hi && g_lay_pool_lo <= g_pool_n) g_pool_n = g_lay_pool_lo;
    g_lay_pool_lo = g_pool_n;
    g_clip_n = 0;
    g_cb_n = 0;
    g_oof_n = 0;
    g_work = 0;
    g_inl_depth = 0;
    g_ord_node = 0;
#ifdef RBROWSE_HOST
    memset(g_dbg_cnt, 0, sizeof(g_dbg_cnt));
#endif
    memset(g_nbox_set, 0, sizeof(g_nbox_set));
    g_fl_n = g_fl_base = 0;
    g_lay_depth = 0;
    g_probe_depth = 0;
    g_hot_in = 0;
    memset(g_mc, 0, sizeof(g_mc));
    memset(g_meas, 0, sizeof(g_meas));
    g_abs_depth = 0;
    g_abs_place = 0;
    g_grid_top = 0;
    g_ts.weight = TW_NORMAL; g_ts.decor = TD_NONE; g_ts.transform = TT_NONE;
    g_ts.ws = WS_NORMAL; g_ts.letsp = 0; g_ts.lineh = 0;
    g_hide_vis = g_hide_op = 0;
    g_op_mult = 255;
    g_last_outer_w = 0;
    g_ext_right = -32768;
    g_box_n = 0;
    g_focus_n = 0;
    g_focus_overflow = 0;
    g_page_h = 0;
    /* Defensive: layout_flex normally restores these, but an early return
       would otherwise prime stale flex state for the next layout. */
    g_flex_top = 0;
    g_force_w = -1;
    g_force_h = -1;
    g_page_bg = g_web ? rb_map_rgb(255, 255, 255) : rb_map_rgb(0x66, 0x3C, 0x22);
    g_page_bg_html = 0;
    if (g_node_n <= 1) return;
    RbColor ic = default_fg();
    g_page_h = layout_block(1, 0, 0, RB_VIEW_W, ic, 1, 0);
    if (g_page_h < 0) g_page_h = 0;
    /* Absolute elements with no positioned ancestor and every fixed element:
       placed against the initial containing block (the viewport at the
       document origin). A fixed element's boxes are in viewport space. */
    if (g_oof_n) {
        oof_place_for(-1, 0, 0, RB_VIEW_W, RB_VIEW_H);
        /* The document grows to hold non-fixed positioned boxes, so a modal
           or footer pushed below the fold is still reachable. Bounded so a
           top:9999px cannot make the page enormous. */
        for (int i = 0; i < g_box_n; i++) {
            if ((g_box[i].flags & RB_BOX_FIXED) || !(g_box[i].flags & RB_BOX_POS)) continue;
            int bot = g_box[i].y + g_box[i].h;
            if (bot > g_page_h && bot <= 4000) g_page_h = bot;
        }
    }
    sort_stacking();
    /* floats and spilled content can reach below what the flow reported */
    for (int i = 0; i < g_box_n; i++) {
        if (g_box[i].flags & (RB_BOX_FIXED | RB_BOX_POS)) continue;
        int bot = g_box[i].y + g_box[i].h;
        if (bot > g_page_h && bot <= 400000) g_page_h = bot;
    }
    g_lay_pool_hi = g_pool_n;
    g_fw_hidden = 0;
    focus_walk(1, ic, 1, 0);
    ev_prune_active();
}

static const char *skip_ws(const char *s, const char *e) {
    while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
    return s;
}

static int ident_is(const char *s, const char *e, const char *lit, const char **adv) {
    int n = (int)strlen(lit);
    if (s + n > e || memcmp(s, lit, (size_t)n) != 0) return 0;
    if (s + n < e) {
        char c = s[n];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9')) return 0;
    }
    *adv = s + n;
    return 1;
}

static const char *parse_js_string(const char *s, const char *e, char *out, int cap, int *olen) {
    s = skip_ws(s, e);
    *olen = 0;
    if (s >= e) return s;
    if (*s != '"' && *s != '\'') return s;
    char q = *s++;
    int n = 0;
    while (s < e && *s != q) {
        char c = *s++;
        if (c == '\\' && s < e) {
            char e2 = *s++;
            if (e2 == 'n') c = '\n';
            else if (e2 == 't') c = ' ';
            else c = e2;
        }
        if (n + 1 < cap) out[n++] = c;
    }
    if (s < e && *s == q) s++;
    out[n] = 0;
    *olen = n;
    return s;
}

static void kill_tree(int node) {
    if (!node) return;
    int c = g_nodes[node].first;
    g_nodes[node].first = 0;
    g_nodes[node].dead = 1;
    while (c) {
        int n = g_nodes[c].next;
        kill_tree(c);
        c = n;
    }
}

static int set_elem_text(int node, const char *s, int n) {
    if (!node || g_nodes[node].dead) return 0;
    if (g_nodes[node].tag == RB_TAG_INPUT || g_nodes[node].tag == RB_TAG_TEXTAREA) {
        int off = pool_add(s, n);
        if (off < 0) return 0;
        g_nodes[node].text_off = (uint32_t)off;
        g_nodes[node].text_len = (uint32_t)n;
        return 1;
    }
    int c = g_nodes[node].first;
    g_nodes[node].first = 0;
    while (c) {
        int nx = g_nodes[c].next;
        kill_tree(c);
        c = nx;
    }
    int off = pool_add(s, n);
    if (off < 0) return 0;
    int t = new_node();
    if (!t) return 0;
    g_nodes[t].kind = 1;
    g_nodes[t].text_off = (uint32_t)off;
    g_nodes[t].text_len = (uint32_t)n;
    add_child(node, t);
    return 1;
}

static void set_nav(const char *s, int n) {
    if (n >= (int)sizeof(g_nav)) n = (int)sizeof(g_nav) - 1;
    if (n < 0) n = 0;
    if (n > 0) memcpy(g_nav, s, (size_t)n);
    g_nav[n] = 0;
    g_nav_len = n;
}

static int run_one(const char *start, const char *e) {
    const char *s = skip_ws(start, e);
    const char *adv = s;
    if (ident_is(s, e, "document", &adv)) {
        s = skip_ws(adv, e);
        if (s >= e || *s != '.') return 0;
        s = skip_ws(s + 1, e);
        if (!ident_is(s, e, "getElementById", &adv)) return 0;
        s = skip_ws(adv, e);
        if (s >= e || *s != '(') return 0;
        s++;
        char id[64];
        int idn = 0;
        s = parse_js_string(s, e, id, (int)sizeof(id), &idn);
        s = skip_ws(s, e);
        if (s >= e || *s != ')') return 0;
        s = skip_ws(s + 1, e);
        if (s >= e || *s != '.') return 0;
        s = skip_ws(s + 1, e);
        int node = 0;
        for (int i = 1; i < g_node_n; i++) {
            if (g_nodes[i].dead || (int)g_nodes[i].id_len != idn) continue;
            if (memcmp(g_pool + g_nodes[i].id_off, id, (size_t)idn) == 0) { node = i; break; }
        }
        if (!node) return 0;
        if (ident_is(s, e, "textContent", &adv) || ident_is(s, e, "innerText", &adv) ||
            ident_is(s, e, "innerHTML", &adv)) {
            s = skip_ws(adv, e);
            if (s >= e || *s != '=') return 0;
            char val[240];
            int vn = 0;
            parse_js_string(s + 1, e, val, (int)sizeof(val), &vn);
            return set_elem_text(node, val, vn);
        }
        if (ident_is(s, e, "style", &adv)) {
            s = skip_ws(adv, e);
            if (s >= e || *s != '.') return 0;
            s = skip_ws(s + 1, e);
            int kind = 0;
            if (ident_is(s, e, "color", &adv)) kind = JS_COLOR;
            else if (ident_is(s, e, "backgroundColor", &adv) || ident_is(s, e, "background", &adv)) kind = JS_BG;
            else if (ident_is(s, e, "display", &adv)) kind = JS_DISP;
            else return 0;
            s = skip_ws(adv, e);
            if (s >= e || *s != '=') return 0;
            char val[64];
            int vn = 0;
            parse_js_string(s + 1, e, val, (int)sizeof(val), &vn);
            if (kind == JS_DISP) {
                if (eq_lit(val, vn, "none")) g_nodes[node].js_display = DISP_NONE;
                else if (eq_lit(val, vn, "block")) g_nodes[node].js_display = DISP_BLOCK;
                else if (eq_lit(val, vn, "flex") || eq_lit(val, vn, "inline-flex")) g_nodes[node].js_display = DISP_FLEX;
                else g_nodes[node].js_display = DISP_INLINE;
                g_nodes[node].js_mask |= JS_DISP;
                style_touch(node);
                return 1;
            }
            int col = 0;
            if (!parse_color(val, vn, &col)) return 0;
            if (col < 0) return 0;      /* transparent/currentColor/inherit: not modelled here */
            if (kind == JS_COLOR) g_nodes[node].js_color = (RbColor)col;
            else g_nodes[node].js_bg = (RbColor)col;
            g_nodes[node].js_mask |= (uint32_t)kind;
            style_touch(node);
            return 1;
        }
        return 0;
    }
    s = skip_ws(start, e);
    if (ident_is(s, e, "window", &adv)) {
        s = skip_ws(adv, e);
        if (s < e && *s == '.') s = skip_ws(s + 1, e);
    }
    if (!ident_is(s, e, "location", &adv)) return 0;
    s = skip_ws(adv, e);
    if (s < e && *s == '.') {
        s = skip_ws(s + 1, e);
        if (!ident_is(s, e, "href", &adv)) return 0;
        s = skip_ws(adv, e);
    }
    if (s >= e || *s != '=') return 0;
    char url[400];
    int un = 0;
    parse_js_string(s + 1, e, url, (int)sizeof(url), &un);
    if (un <= 0) return 0;
    set_nav(url, un);
    return 2;
}

static const char *stmt_end(const char *s, const char *e) {
    int depth = 0;
    char q = 0;
    while (s < e) {
        char c = *s;
        if (q) {
            if (c == '\\' && s + 1 < e) { s += 2; continue; }
            if (c == q) q = 0;
            s++;
            continue;
        }
        if (c == '"' || c == '\'') { q = c; s++; continue; }
        if (c == '{') depth++;
        else if (c == '}') { if (depth) depth--; }
        else if (c == ';' && depth == 0) return s;
        s++;
    }
    return e;
}

static int run_script(const char *s, int n) {
    const char *p = s;
    const char *e = s + n;
    int result = 0;
    while (p < e) {
        p = skip_ws(p, e);
        if (p >= e) break;
        if (p + 1 < e && p[0] == '/' && p[1] == '/') {
            while (p < e && *p != '\n') p++;
            continue;
        }
        if (p + 1 < e && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < e && !(p[0] == '*' && p[1] == '/')) p++;
            if (p + 1 < e) p += 2;
            continue;
        }
        const char *adv = p;
        if (ident_is(p, e, "function", &adv)) {
            p = adv;
            int depth = 0;
            int seen = 0;
            char q = 0;
            while (p < e) {
                char c = *p++;
                if (q) { if (c == q) q = 0; continue; }
                if (c == '"' || c == '\'') { q = c; continue; }
                if (c == '{') { depth++; seen = 1; }
                else if (c == '}') {
                    if (depth) depth--;
                    if (seen && depth == 0) break;
                } else if (c == ';' && depth == 0 && !seen) break;
            }
            continue;
        }
        const char *end = stmt_end(p, e);
        int r = run_one(p, end);
        if (r == 2) result = 2;
        else if (r == 1 && result == 0) result = 1;
        p = end < e ? end + 1 : e;
    }
    return result;
}

/* --- scripting support state (rb_set_scripting, the fragment parser) --- */
static int g_scripting;       /* sticky across rb_open */
#define SCRIPT_MAX 256
typedef struct { int node; const char *text; int len; } ScriptEnt;
static ScriptEnt g_script[SCRIPT_MAX];   /* text points into the host's rb_open buffer */
static int g_script_n;
static int g_frag;            /* parsing an innerHTML fragment: no scripts, sheets or title; html/head/body tags dropped */
static int g_last_el;         /* the element start_element created last (0 when it made none) */
static int g_tag_nn;          /* length of the tag name start_element is working on */

/* Names of unknown/custom elements. Node keeps only a hash for matching, so the
   name is interned here (lower-case, once per distinct name) for tag_name and
   serialization. Past 255 distinct names an element reports no name. */
#define TNAME_MAX 255
static struct { uint32_t off; uint8_t len; } g_tn[TNAME_MAX];
static int g_tn_n;

static int tname_intern(const char *s, int n) {
    char buf[64];
    if (n <= 0) return 0;
    if (n > 63) n = 63;
    for (int i = 0; i < n; i++) buf[i] = (char)lower_ch((unsigned char)s[i]);
    for (int i = 0; i < g_tn_n; i++)
        if (g_tn[i].len == n && memcmp(g_pool + g_tn[i].off, buf, (size_t)n) == 0) return i + 1;
    if (g_tn_n >= TNAME_MAX) return 0;
    int off = pool_add(buf, n);
    if (off < 0) return 0;
    g_tn[g_tn_n].off = (uint32_t)off;
    g_tn[g_tn_n].len = (uint8_t)n;
    return ++g_tn_n;
}

static void script_note(int node, const char *text, int len) {
    if (!node || g_script_n >= SCRIPT_MAX) return;
    g_script[g_script_n].node = node;
    g_script[g_script_n].text = text;
    g_script[g_script_n].len = len;
    g_script_n++;
}

static void queue_imports_fwd(int mark, int parent, int importer_ord);

static void css_add_sheet(const char *s, int n) {
    int mark = g_import_n;
    g_sheet_idx = 0;
    g_cur_origin = 1;
    parse_sheet(s, n, 0);
    queue_imports_fwd(mark, -1, g_sheet_ord);     /* @import in an inline <style> */
    g_sheet_ord += SHEET_STEP;
    g_rules_dirty = 1;
}

static int run_script(const char *s, int n);
#include "html_parse.inc"
static void queue_imports_fwd(int mark, int parent, int importer_ord) { queue_imports(mark, parent, importer_ord); }

/* Part 2 forward declaration: listeners and focus are per-document state. */
static void ev_reset(void);

static void reset_doc(void) {
    memset(g_nodes, 0, sizeof(g_nodes));
    g_node_n = 1;
    g_free_n = 0;
    g_box_n = 0;
    g_web = g_want_web;
    if (g_ua_profile != g_want_web) css_ua_init(g_want_web);
    else css_doc_reset();
    g_rules_dirty = 1;
    g_focus_n = 0;
    g_pool_n = 0;
    g_lay_pool_lo = 0;
    g_lay_pool_hi = -1;
    g_nav_len = 0;
    g_nav[0] = 0;
    g_title[0] = 0;
    g_script_n = 0;
    g_tn_n = 0;
    g_frag = 0;
    g_page_h = 0;
    g_stop = 0;
    g_oom = 0;
    g_oom_line = 0;
    ev_reset();
    int root = new_node();
    g_nodes[root].tag = RB_TAG_DOC;
}

static int tag_at(const char *s, int n, int i, const char *lit) {
    if (i >= n || s[i] != '<') return 0;
    int j = i + 1;
    int k = 0;
    while (lit[k]) {
        if (j >= n || lower_ch((unsigned char)s[j]) != (unsigned char)lit[k]) return 0;
        j++;
        k++;
    }
    if (j < n) {
        int c = (unsigned char)s[j];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':')
            return 0;
    }
    return 1;
}

static int skip_gt(const char *s, int n, int i) {
    int q = 0;
    while (i < n) {
        char c = s[i++];
        if (q) {
            if (c == q) q = 0;
            continue;
        }
        if (c == '"' || c == '\'') { q = c; continue; }
        if (c == '>') break;
    }
    return i;
}

static int skip_element(const char *s, int n, int i, const char *lit) {
    int llen = 0;
    while (lit[llen]) llen++;
    i = skip_gt(s, n, i);
    while (i + llen + 2 < n) {
        if (s[i] == '<' && i + 1 < n && s[i + 1] == '/') {
            int j = i + 2;
            while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) j++;
            int k = 0;
            int ok = 1;
            while (lit[k]) {
                if (j >= n || lower_ch((unsigned char)s[j]) != (unsigned char)lit[k]) { ok = 0; break; }
                j++;
                k++;
            }
            if (ok && (j >= n || !((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z') ||
                                   (s[j] >= '0' && s[j] <= '9') || s[j] == '-' || s[j] == '_' || s[j] == ':')))
                return skip_gt(s, n, j);
        }
        i++;
    }
    return n;
}

int rb_compact_markup(const char *src, int n, char *dst, int cap) {
    if (!dst || cap <= 0) return 0;
    if (!src || n < 0) n = 0;
    int w = 0;
    int i = 0;
    while (i < n && w + 1 < cap) {
        const char *drop = 0;
        if (tag_at(src, n, i, "script")) drop = "script";
        else if (tag_at(src, n, i, "style")) drop = "style";
        if (drop) {
            i = skip_element(src, n, i, drop);
            continue;
        }
        dst[w++] = src[i++];
    }
    dst[w] = 0;
    return w;
}

/* ---- the layout stack ------------------------------------------------------
   Layout and parsing recurse once per DOM level (layout_block, layout_inline,
   layout_inline_el and their callees: about 1.6 KB per level at -O0 on
   i686), far more than the shell's stack can be trusted with. The entry
   points below run on a private stack in .bss; the host build just calls. */
#define LAY_STACK_SIZE (768 * 1024)
#ifndef RBROWSE_HOST
static uint8_t g_lay_stack[LAY_STACK_SIZE] __attribute__((aligned(16)));
static int g_on_lay_stack;

static int run_big(int (*fn)(void *), void *arg) {
    if (g_on_lay_stack) return fn(arg);
    int ret;
    uint8_t *top = g_lay_stack + LAY_STACK_SIZE;
    g_on_lay_stack = 1;
    __asm__ volatile(
        "movl %%esp, %%esi\n\t"
        "movl %[top], %%esp\n\t"
        "subl $12, %%esp\n\t"
        "pushl %[arg]\n\t"
        "call *%[fn]\n\t"
        "movl %%esi, %%esp\n\t"
        : "=a"(ret)
        : [top] "r"(top), [arg] "r"(arg), [fn] "r"(fn)
        : "esi", "ecx", "edx", "memory", "cc");
    g_on_lay_stack = 0;
    return ret;
}
#else
static int run_big(int (*fn)(void *), void *arg) {
    char marker;
    g_stack_hi = (uintptr_t)&marker;
    g_stack_lo = g_stack_hi;
    return fn(arg);
}
#endif

typedef struct { const char *html; int len; int web; } OpenArgs;

static int open_impl(void *p) {
    OpenArgs *a = (OpenArgs *)p;
    g_want_web = a->web;
    reset_doc();
    parse_html(a->html, a->len);
    do_layout();
    return 0;
}

int rb_open_web(const char *html, int len) {
    OpenArgs a;
    a.html = html ? html : "";
    a.len = len < 0 ? (int)strlen(a.html) : len;
    a.web = 1;
    return run_big(open_impl, &a);
}

int rb_open(const char *html, int len) {
    OpenArgs a;
    a.html = html ? html : "";
    a.len = len < 0 ? (int)strlen(a.html) : len;
    a.web = 0;
    return run_big(open_impl, &a);
}

static int relayout_impl(void *p) { (void)p; do_layout(); return 0; }

void rb_relayout(void) { run_big(relayout_impl, 0); }
int rb_page_height(void) { return g_page_h; }
int rb_page_bg(void) { return g_page_bg; }

/* The page viewport (see engine.h). Plain statics: every user reads them
   through RB_VIEW_W / RB_VIEW_H at layout time. */
static int g_view_w = RB_VIEW_W_DEFAULT, g_view_h = RB_VIEW_H_DEFAULT;

int rb_view_width(void) { return g_view_w; }
int rb_view_height(void) { return g_view_h; }

void rb_set_viewport(int w, int h) {
    if (w < 64) w = 64;
    if (w > RB_VIEW_W_MAX) w = RB_VIEW_W_MAX;
    if (h < 32) h = 32;
    if (h > 4096) h = 4096;
    g_view_w = w;
    g_view_h = h;
    g_media_w = w + 16;
    g_media_h = h * 2;
    /* computed styles resolve vw and % margins against the viewport */
    for (int i = 0; i < MAX_NODES; i++) g_style_cache[i].valid = 0;
}

int rb_oom(void) { return g_oom != 0; }

/* Host only: the element's border box from the last layout. Returns 0 when it
   had none (inline, display:none, never reached). */
int rb_debug_box(int node, int *x, int *y, int *w, int *h) {
#ifdef RBROWSE_HOST
    if (node <= 0 || node >= g_node_n || !g_nbox_set[node]) return 0;
    *x = g_nbox[node][0]; *y = g_nbox[node][1]; *w = g_nbox[node][2]; *h = g_nbox[node][3];
    return 1;
#else
    (void)node; (void)x; (void)y; (void)w; (void)h;
    return 0;
#endif
}
int rb_debug_oom_line(void) { return g_oom_line; }
const char *rb_debug_oom_file(void) { return g_oom_file ? g_oom_file : ""; }

/* ---- external stylesheets ---------------------------------------------------
   <link rel=stylesheet> (and @import inside a provided sheet) only name the
   sheet; the host fetches it and hands the text back. The sheet keeps the
   document-order slot its <link> had. */
int rb_css_pending(void) { return g_pcss_n; }

int rb_css_parent(int i) { return i >= 0 && i < g_pcss_n ? g_pcss[i].parent : -1; }

int rb_css_done(int i) { return i >= 0 && i < g_pcss_n ? g_pcss[i].done : 1; }

int rb_css_href(int i, char *out, int cap) {
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    if (i < 0 || i >= g_pcss_n) return 0;
    int n = (int)g_pcss[i].href_len;
    if (n >= cap) n = cap - 1;
    const char *src = g_pcss[i].interned ? intern_str((int)g_pcss[i].href_off, &n) : g_pool + g_pcss[i].href_off;
    if (g_pcss[i].interned) { if (n >= cap) n = cap - 1; }
    memcpy(out, src, (size_t)n);
    out[n] = 0;
    return n;
}

static int css_provide_impl(int i, const char *css, int len) {
    if (i < 0 || i >= g_pcss_n || !css || len < 0) return 0;
    PendCss *pc = &g_pcss[i];
    pc->done = 1;
    int save_ord = g_sheet_ord;
    uint32_t save_idx = g_sheet_idx;
    int mark = g_import_n;
    g_sheet_ord = pc->ord;
    g_sheet_idx = pc->next_idx;
    g_cur_origin = 1;
    /* Sheets bigger than the scratch buffer go through in pieces, each cut at
       a top-level rule boundary. UTF-8 folds to the font's ASCII on the way. */
    vocab_build();
    g_prune = g_node_n > 1;
    int pos = 0;
    while (pos < len) {
        /* Cut at a top-level rule boundary: first try a RAW_MAX/4 window, and
           when one rule is bigger than that (a long @media block, a data: URL)
           keep widening up to the whole scratch buffer. */
        int end = len < pos + RAW_MAX / 2 ? len : -1;
        for (int win = RAW_MAX / 4; end < 0 && win <= RAW_MAX - 1024; win += RAW_MAX / 4) {
            int limit = pos + win > len ? len : pos + win;
            int depth = 0, last = -1;
            char q = 0;
            for (int k = pos; k < limit; k++) {
                char c = css[k];
                if (q) { if (c == '\\' && k + 1 < limit) k++; else if (c == q) q = 0; continue; }
                if (c == '"' || c == '\'') q = c;
                else if (c == '/' && k + 1 < limit && css[k + 1] == '*') {
                    k += 2;
                    while (k + 1 < limit && !(css[k] == '*' && css[k + 1] == '/')) k++;
                    k++;
                } else if (c == '{') depth++;
                else if (c == '}') { if (depth > 0) depth--; if (depth == 0) last = k + 1; }
            }
            if (limit == len) end = len;
            else if (last > pos) end = last;
        }
        if (end < 0) end = pos + RAW_MAX - 1024 > len ? len : pos + RAW_MAX - 1024;     /* one rule bigger than the buffer: give up on it */
        int fn = text_fold(css + pos, end - pos, g_raw, RAW_MAX, 0);
        parse_sheet(g_raw, fn, 0);
        pos = end;
    }
    g_prune = 0;
    pc->next_idx = g_sheet_idx;
    g_sheet_ord = save_ord;
    g_sheet_idx = save_idx;
    queue_imports(mark, i, pc->ord);
    g_rules_dirty = 1;
    return 1;
}

typedef struct { int i; const char *css; int len; } CssArgs;
static int css_provide_tramp(void *p) {
    CssArgs *a = (CssArgs *)p;
    return css_provide_impl(a->i, a->css, a->len);
}

int rb_css_provide(int i, const char *css, int len) {
    CssArgs a;
    a.i = i; a.css = css; a.len = len;
    return run_big(css_provide_tramp, &a);
}

/* Debug: the computed style of a node, as text (for tools). */
static void dbg_put(char *out, int cap, int *w, const char *s) {
    while (*s && *w < cap - 1) out[(*w)++] = *s++;
    out[*w] = 0;
}
static void dbg_int(char *out, int cap, int *w, const char *name, int v) {
    char t[16];
    int k = 0, neg = v < 0;
    unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    dbg_put(out, cap, w, name);
    if (neg) dbg_put(out, cap, w, "-");
    if (u == 0) t[k++] = '0';
    while (u > 0 && k < 15) { t[k++] = (char)('0' + u % 10); u /= 10; }
    char r[16];
    for (int i = 0; i < k; i++) r[i] = t[k - 1 - i];
    r[k] = 0;
    dbg_put(out, cap, w, r);
}

int rb_debug_style(int node, char *out, int cap) {
    if (!out || cap < 2) return 0;
    out[0] = 0;
    if (node <= 0 || node >= g_node_n || g_nodes[node].dead) return 0;
    int p = g_nodes[node].parent;
    Style st = compute_style(node, p ? g_nodes[p].color : default_fg(), 1, 0);
    int w = 0;
    dbg_int(out, cap, &w, "tag=", g_nodes[node].tag);
    dbg_int(out, cap, &w, " disp=", st.display);
    dbg_int(out, cap, &w, " fpx=", st.fpx);
    dbg_int(out, cap, &w, " scale=", st.scale);
    dbg_int(out, cap, &w, " w=", st.width);
    dbg_int(out, cap, &w, " h=", st.height);
    dbg_int(out, cap, &w, " m=", st.mt); dbg_int(out, cap, &w, ",", st.mr);
    dbg_int(out, cap, &w, ",", st.mb); dbg_int(out, cap, &w, ",", st.ml);
    dbg_int(out, cap, &w, " in=", st.pt); dbg_int(out, cap, &w, ",", st.pr);
    dbg_int(out, cap, &w, ",", st.pb); dbg_int(out, cap, &w, ",", st.pl);
    dbg_int(out, cap, &w, " lh=", st.x.lineh == LH_UNSET ? 9999 : st.x.lineh);
    dbg_int(out, cap, &w, " fl=", st.flt);
    dbg_int(out, cap, &w, " pos=", st.pos);
    dbg_int(out, cap, &w, " col=", st.color);
    dbg_int(out, cap, &w, " bg=", st.has_bg ? st.bg : -1);
    return w;
}

/* Capacity probe for tools: how full the engine's fixed tables are. */
int rb_debug_stat(int which) {
    switch (which) {
    case 0: return g_node_n;
    case 1: return g_box_n;
    case 2: return g_pool_n;
    case 3: return g_atom_n;
    case 4: return g_crule_n;
    case 5: return g_decl_n;
    case 6: return g_ient_n;
    case 7: return g_istr_n;
    case 8: return g_work;
    case 10: return g_var_n;
    case 13: return g_node_n - g_free_n;   /* live nodes */
#ifdef RBROWSE_HOST
    case 11: {                       /* the node that was laid out the most times */
        int best = 0;
        for (int i = 1; i < g_node_n; i++) if (g_dbg_cnt[i] > g_dbg_cnt[best]) best = i;
        return best;
    }
    case 12: { int best = 0; for (int i = 1; i < g_node_n; i++) if (g_dbg_cnt[i] > g_dbg_cnt[best]) best = i; return g_dbg_cnt[best]; }
#endif
#ifdef RBROWSE_HOST
    case 9: return g_stack_hi > g_stack_lo ? (int)(g_stack_hi - g_stack_lo) : 0;
#endif
    default: return -1;
    }
}

int rb_style_stats(int *hits, int *misses) {
    if (hits) *hits = g_style_hits;
    if (misses) *misses = g_style_misses;
    return g_style_hits + g_style_misses;
}

int rb_title(char *out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = (int)strlen(g_title);
    if (n >= cap) n = cap - 1;
    memcpy(out, g_title, (size_t)n);
    out[n] = 0;
    return n;
}

int rb_find_id(const char *id) {
    if (!id || !*id) return 0;
    int n = (int)strlen(id);
    for (int i = 1; i < g_node_n; i++) {
        if (g_nodes[i].dead || (int)g_nodes[i].id_len != n) continue;
        if (memcmp(g_pool + g_nodes[i].id_off, id, (size_t)n) != 0) continue;
        /* a detached subtree (removed by a script, kept alive) is not in the document */
        int p = i;
        while (g_nodes[p].parent) p = g_nodes[p].parent;
        if (p == 1) return i;
    }
    return 0;
}

int rb_node_tag(int node) {
    if (node <= 0 || node >= g_node_n || g_nodes[node].dead) return RB_TAG_OTHER;
    return g_nodes[node].tag;
}

int rb_node_color(int node) {
    if (node <= 0 || node >= g_node_n || g_nodes[node].dead) return 0;
    return g_nodes[node].color;
}

int rb_node_href(int node, char *out, int cap) {
    if (!out || cap <= 0 || node <= 0 || node >= g_node_n || g_nodes[node].dead) return 0;
    if (g_nodes[node].tag != RB_TAG_A || g_nodes[node].href_len == 0) return 0;
    int n = g_nodes[node].href_len;
    if (n >= cap) n = cap - 1;
    memcpy(out, g_pool + g_nodes[node].href_off, (size_t)n);
    out[n] = 0;
    return 1;
}

static int collect_rec(int node, char *out, int cap, int pos, int skip_raw) {
    if (!node || g_nodes[node].dead || pos >= cap || g_nodes[node].pseudo) return pos;
    if (g_nodes[node].kind == 1) {
        int n = g_nodes[node].text_len;
        if (pos + n > cap) n = cap - pos;
        if (n > 0) memcpy(out + pos, g_pool + g_nodes[node].text_off, (size_t)n);
        return pos + n;
    }
    if ((g_nodes[node].tag == RB_TAG_INPUT || g_nodes[node].tag == RB_TAG_TEXTAREA) &&
        g_nodes[node].text_len && g_nodes[node].first == 0) {
        int n = g_nodes[node].text_len;
        if (pos + n > cap) n = cap - pos;
        if (n > 0) memcpy(out + pos, g_pool + g_nodes[node].text_off, (size_t)n);
        return pos + n;
    }
    if (skip_raw && (g_nodes[node].tag == RB_TAG_SCRIPT || g_nodes[node].tag == RB_TAG_STYLE)) return pos;
    for (int c = g_nodes[node].first; c; c = g_nodes[c].next)
        pos = collect_rec(c, out, cap, pos, 1);
    return pos;
}

int rb_collect_text(int node, char *out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = collect_rec(node, out, cap - 1, 0, 0);
    if (n < 0) n = 0;
    out[n] = 0;
    return n;
}

int rb_box_count(void) { return g_box_n; }
const RbBox *rb_box(int i) {
    if (i < 0 || i >= g_box_n) return 0;
    return &g_box[i];
}

const char *rb_box_text(int i, int *len) {
    if (len) *len = 0;
    if (i < 0 || i >= g_box_n || g_box[i].tlen == 0) return "";
    if (len) *len = g_box[i].tlen;
    return g_pool + g_box[i].toff;
}

int rb_first_box(int node, int *x, int *y, int *w, int *h) {
    for (int i = 0; i < g_box_n; i++) {
        if (g_box[i].node == node && g_box[i].kind != RB_BOX_BG && g_box[i].kind != RB_BOX_BORDER && g_box[i].w > 0 && g_box[i].h > 0) {
            if (x) *x = g_box[i].x;
            if (y) *y = g_box[i].y;
            if (w) *w = g_box[i].w;
            if (h) *h = g_box[i].h;
            return 1;
        }
    }
    return 0;
}

static int clickable(int node) {
    if (node <= 0 || node >= g_node_n) return 0;
    int tag = g_nodes[node].tag;
    if (tag == RB_TAG_A && g_nodes[node].href_len) return 1;
    if (tag == RB_TAG_BUTTON || tag == RB_TAG_INPUT || tag == RB_TAG_TEXTAREA) return 1;
    return 0;
}

int rb_box_clip(int i, int *x, int *y, int *w, int *h) {
    if (i < 0 || i >= g_box_n || g_box[i].clip == 0 || g_box[i].clip > g_clip_n) return 0;
    ClipRect c = g_clip_tab[g_box[i].clip - 1];
    if (x) *x = c.x;
    if (y) *y = c.y;
    if (w) *w = c.w;
    if (h) *h = c.h;
    return 1;
}

/* Each word of a line is its own text box, one space apart, and nothing
   covers the space: a click exactly between two words of a link fell through
   it. When no box is under the pointer, the nearest text box on its row to
   the left and to the right decide: the same clickable element on both
   sides, no more than two spaces apart, owns the gap. */
static int hit_word_gap(int x, int y, int scroll) {
    int l = -1, r = -1;
    for (int i = 0; i < g_box_n; i++) {
        const RbBox *b = &g_box[i];
        if (b->kind != RB_BOX_TEXT || b->w <= 0 || b->h <= 0 || !clickable(b->node)) continue;
        int py = (b->flags & RB_BOX_FIXED) ? y - scroll : y;
        if (py < b->y || py >= b->y + b->h) continue;
        if (b->clip && b->clip <= g_clip_n) {
            ClipRect c = g_clip_tab[b->clip - 1];
            if (x < c.x || py < c.y || x >= c.x + c.w || py >= c.y + c.h) continue;
        }
        if (b->x + b->w <= x) {
            if (l < 0 || b->x + b->w > g_box[l].x + g_box[l].w) l = i;
        } else if (b->x > x) {
            if (r < 0 || b->x < g_box[r].x) r = i;
        }
    }
    if (l < 0 || r < 0) return -1;
    const RbBox *a = &g_box[l], *b = &g_box[r];
    int space = 8 * (a->scale ? a->scale : 1) + a->ls;
    if (a->node != b->node || a->y != b->y || ((a->flags ^ b->flags) & RB_BOX_FIXED)) return -1;
    if (b->x - (a->x + a->w) > 2 * space) return -1;
    return a->node;
}

/* Hit test. (x, y) is the pointer in document space for the given scroll
   offset, so a fixed box (stored in viewport space) is compared at y - scroll.
   Boxes are walked in paint order from the top; a clipped box only answers
   inside its clip rect. */
int rb_hit_at(int x, int y, int scroll) {
    for (int i = g_box_n - 1; i >= 0; i--) {
        RbBox *b = &g_box[i];
        if (b->kind == RB_BOX_BORDER) continue;
        if (b->kind == RB_BOX_BG && !clickable(b->node)) continue;
        if (b->w <= 0 || b->h <= 0) continue;
        int py = (b->flags & RB_BOX_FIXED) ? y - scroll : y;
        if (x < b->x || py < b->y || x >= b->x + b->w || py >= b->y + b->h) continue;
        if (b->clip && b->clip <= g_clip_n) {
            ClipRect c = g_clip_tab[b->clip - 1];
            if (x < c.x || py < c.y || x >= c.x + c.w || py >= c.y + c.h) continue;
        }
        return b->node;
    }
    return hit_word_gap(x, y, scroll);
}

int rb_hit(int x, int y) { return rb_hit_at(x, y, 0); }

static const NodePos *nodepos_of(int node) {
    if (node <= 0 || node >= g_node_n || node >= MAX_NODES || g_nodes[node].dead ||
        g_nodes[node].kind == 1)
        return 0;
    return &g_nodepos[node];
}

int rb_style_position(int node) {
    const NodePos *p = nodepos_of(node);
    return p ? p->pos : POS_STATIC;
}
int rb_style_overflow(int node) {
    const NodePos *p = nodepos_of(node);
    return p ? p->overflow : OVF_VISIBLE;
}
int rb_style_visibility(int node) {
    const NodePos *p = nodepos_of(node);
    return (p && p->visibility == VIS_HIDDEN) ? VIS_HIDDEN : VIS_VISIBLE;
}
int rb_style_cursor(int node) {
    const NodePos *p = nodepos_of(node);
    return p ? p->cursor : CUR_AUTO;
}
int rb_style_opacity(int node) {
    const NodePos *p = nodepos_of(node);
    return p ? p->opacity : 255;
}
int rb_style_zindex(int node, int *is_auto) {
    const NodePos *p = nodepos_of(node);
    if (is_auto) *is_auto = p ? p->z_auto : 1;
    return p ? p->z_index : 0;
}

int rb_focus_count(void) { return g_focus_n; }
int rb_focus_node(int index) {
    if (index < 0 || index >= g_focus_n) return 0;
    return g_focus[index];
}

int rb_focus_index(int node) {
    for (int i = 0; i < g_focus_n; i++) if (g_focus[i] == node) return i;
    return -1;
}

int rb_tab(int index, int backward) {
    if (g_focus_n <= 0) return -1;
    if (index < 0 || index >= g_focus_n) return backward ? g_focus_n - 1 : 0;
    if (backward) return index == 0 ? g_focus_n - 1 : index - 1;
    return index + 1 >= g_focus_n ? 0 : index + 1;
}

static int form_of(int node) {
    for (int p = g_nodes[node].parent; p; p = g_nodes[p].parent) {
        if (g_nodes[p].tag == RB_TAG_FORM) return p;
    }
    return 0;
}

/* Built with the URL helpers below. Returns 1 when a navigation was queued.
   submit_build does NOT fire RB_EV_SUBMIT; submit_form does, then delegates. */
static int submit_build(int form, int submitter);
static int submit_form(int form, int submitter);

/* Set by submit_form while it fires RB_EV_SUBMIT, so the event's default
   action knows not to build the URL a second time. The default action also
   reads the submitter from here: unlike a real DOM SubmitEvent, the event
   object has no submitter field.

   Nesting is correct as-is: submit_form saves/restores the pair around its
   dispatch, so a SUBMIT listener that submits another form (through the
   click/Enter path, i.e. rb_activate/rb_click) still gets its own form AND
   submitter for the inner dispatch — verified by
   test_submit_nested_cross. What is NOT fixable in this mechanism: the
   navigation target is a SINGLE slot, so when both the inner and the outer
   submission queue a URL the outer one (built last) wins — the same "final
   navigation wins" behaviour a nested form.submit() produces in real
   browsers. A direct rb_dispatch(form, RB_EV_SUBMIT) has no pending entry at
   all and builds with submitter 0 by design. */
static int g_submit_form;
static int g_submit_submitter;

/* Part 2 forward declaration. enter selects the historical rb_activate (Enter)
   semantics for fields; a public rb_dispatch click focuses fields instead.
   prevented_out (optional) reports preventDefault to the caller. key/x/y/button
   are the event payload; the plain entry point passes the empty payload. */
static int dispatch_ev_ex(int target, int type, int enter, int key, int x, int y,
                          int button, int buttons, int mods, int *prevented_out);
static int dispatch_ev(int target, int type, int enter, int *prevented_out);

int rb_activate(int node) {
    /* Activation is a CLICK dispatch with the Enter semantics below: disabled
       form controls do not fire their handler and cannot submit; an inline
       onclick runs as a bubble handler at the node; <a href> queues nav; and
       a control submits its nearest GET form (a text field press included —
       the UI routes mouse clicks on fields to focus without coming here). */
    return dispatch_ev(node, RB_EV_CLICK, 1, 0);
}

/* 1 when the node's nearest <form> ancestor declares method="post". The engine
   refuses such submissions, so callers can turn the silent rb_activate no-op
   into a status message. */
int rb_form_post(int node) {
    if (node <= 0 || node >= g_node_n || g_nodes[node].dead) return 0;
    int form = form_of(node);
    return form ? g_nodes[form].method_post : 0;
}

int rb_click(int x, int y) {
    int n = rb_hit(x, y);
    if (n < 0) return -1;
    /* A plain click dispatches CLICK; a non-clickable node still has no
       effect, and this path focuses text fields instead of submitting. */
    dispatch_ev(n, RB_EV_CLICK, 0, 0);
    return n;
}

int rb_take_nav(char *out, int cap) {
    if (g_nav_len <= 0 || !out || cap <= 0) return 0;
    int n = g_nav_len;
    if (n >= cap) n = cap - 1;
    memcpy(out, g_nav, (size_t)n);
    out[n] = 0;
    g_nav_len = 0;
    g_nav[0] = 0;
    return 1;
}

int rb_is_field(int node) {
    if (node <= 0 || node >= g_node_n || g_nodes[node].hidden || g_nodes[node].disabled) return 0;
    if (g_nodes[node].tag == RB_TAG_TEXTAREA) return 1;
    if (g_nodes[node].tag == RB_TAG_INPUT && !g_nodes[node].input_btn &&
        g_nodes[node].ctl != RB_CTL_CHECKBOX && g_nodes[node].ctl != RB_CTL_RADIO)
        return 1;
    return 0;
}

static int field_set(int node, const char *s, int n) {
    if (n > 60) n = 60;
    int off = pool_add(s, n);
    if (off < 0) return 0;
    g_nodes[node].text_off = (uint32_t)off;
    g_nodes[node].text_len = (uint32_t)n;
    return 1;
}

int rb_type_char(int node, char ch) {
    if (!rb_is_field(node) || ch < 32 || ch > 126) return 0;
    int n = g_nodes[node].text_len;
    if (n > 59) return 0;
    char buf[64];
    int on = n;
    if (n > 0) memcpy(buf, g_pool + g_nodes[node].text_off, (size_t)n);
    buf[n++] = ch;
    buf[n] = 0;
    if (!field_set(node, buf, n)) return 0;
    /* INPUT and CHANGE fire after the mutation; a listener that prevents the
       input event reverts to the pre-edit value and reports no change. The
       dispatch can remove this node and recycle the slot, so remember the
       generation: writing the revert into a reused slot would edit a
       different field. */
    int gen = g_nodes[node].gen;
    int prevented = 0;
    dispatch_ev(node, RB_EV_INPUT, 0, &prevented);
    if (prevented) {
        if (g_nodes[node].gen == gen && !g_nodes[node].dead) field_set(node, buf, on);
        return 0;
    }
    dispatch_ev(node, RB_EV_CHANGE, 0, 0);
    return 1;
}

int rb_backspace(int node) {
    if (!rb_is_field(node)) return 0;
    int n = g_nodes[node].text_len;
    if (n <= 0) return 0;
    if (n > 60) n = 60;
    char buf[64], old[64];
    memcpy(buf, g_pool + g_nodes[node].text_off, (size_t)(n - 1));
    memcpy(old, g_pool + g_nodes[node].text_off, (size_t)n);
    if (!field_set(node, buf, n - 1)) return 0;
    int gen = g_nodes[node].gen;
    int prevented = 0;
    dispatch_ev(node, RB_EV_INPUT, 0, &prevented);
    if (prevented) {
        if (g_nodes[node].gen == gen && !g_nodes[node].dead) field_set(node, old, n);
        return 0;
    }
    dispatch_ev(node, RB_EV_CHANGE, 0, 0);
    return 1;
}

static int begins(const char *s, const char *lit) {
    int i = 0;
    while (lit[i]) {
        if (s[i] != lit[i]) return 0;
        i++;
    }
    return 1;
}

static void append_n(char *out, int cap, int *w, const char *s, int n) {
    for (int i = 0; i < n && *w + 1 < cap; i++) out[(*w)++] = s[i];
    if (*w < cap) out[*w] = 0;
}

/* Percent-encode s[0..n) into out (RFC 3986: keep unreserved, escape the
   rest). Space becomes %20, not '+', so the encoding stays valid for the
   query component of a GET URL. */
static void pct_append(char *out, int cap, int *w, const char *s, int n) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        int safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            if (*w + 1 < cap) out[(*w)++] = (char)c;
        } else if (*w + 3 < cap) {
            out[(*w)++] = '%';
            out[(*w)++] = hex[c >> 4];
            out[(*w)++] = hex[c & 15];
        }
    }
    if (*w < cap) out[*w] = 0;
}

/* Append "name=value", percent-encoded, to the query string under construction.
   The first pair is joined with qmark ('?' or '&' when the action already has a
   query); later pairs always use '&'. */
static void form_pair(const char *name, int nlen, const char *val, int vlen,
                      char *out, int cap, int *w, int *used, int qmark) {
    if (*w + 1 < cap) out[(*w)++] = *used ? '&' : (char)qmark;
    *used = 1;
    pct_append(out, cap, w, name, nlen);
    if (*w + 1 < cap) out[(*w)++] = '=';
    pct_append(out, cap, w, val, vlen);
}

/* Document order walk of the form subtree. Successful controls: text, hidden
   and other <input>s plus <textarea>s by their current edited value, and the
   activated submit control itself. Controls without a name are skipped, as are
   disabled controls. A nested <form> owns its own controls, so descent stops
   there. checkbox/radio contribute only when checked (clicks toggle them, so
   the engine state is what gets submitted). */
static void form_walk(int node, int submitter, int qmark,
                      char *out, int cap, int *w, int *used) {
    for (int c = g_nodes[node].first; c; c = g_nodes[c].next) {
        if (g_nodes[c].dead) continue;
        if (g_nodes[c].kind != 1 && g_nodes[c].name_len && !g_nodes[c].disabled) {
            int tag = g_nodes[c].tag;
            int include = 0;
            if (tag == RB_TAG_INPUT) {
                int ctl = g_nodes[c].ctl;
                if (g_nodes[c].input_btn) include = (c == submitter && ctl == RB_CTL_SUBMIT);
                else if (ctl == RB_CTL_CHECKBOX || ctl == RB_CTL_RADIO) include = g_nodes[c].checked ? 1 : 0;
                else include = 1;
            } else if (tag == RB_TAG_TEXTAREA) {
                include = 1;
            } else if (tag == RB_TAG_BUTTON) {
                include = (c == submitter);
            }
            if (include) {
                form_pair(g_pool + g_nodes[c].name_off, g_nodes[c].name_len,
                          g_pool + g_nodes[c].text_off, g_nodes[c].text_len,
                          out, cap, w, used, qmark);
            }
        }
        if (g_nodes[c].kind != 1 && g_nodes[c].tag != RB_TAG_FORM)
            form_walk(c, submitter, qmark, out, cap, w, used);
    }
}

/* Build the GET URL for a form submit: action + '?' + encoded controls.
   The engine has no base URL, so an absent/empty action yields "?query" and
   the caller resolves it against the current page (rb_resolve keeps the base
   path for query-only links). POST forms never reach this (rb_activate
   refuses them). */
static int submit_build(int form, int submitter) {
    char out[512];
    int w = 0;
    int used = 0;
    const char *action = "";
    int alen = 0;
    if (g_nodes[form].action_len) {
        action = g_pool + g_nodes[form].action_off;
        alen = g_nodes[form].action_len;
    }
    for (int i = 0; i < alen; i++) if (action[i] == '#') { alen = i; break; }
    int qmark = '?';
    for (int i = 0; i < alen; i++) if (action[i] == '?') { qmark = '&'; break; }
    append_n(out, (int)sizeof(out), &w, action, alen);
    form_walk(form, submitter, qmark, out, (int)sizeof(out), &w, &used);
    if (w <= 0) return 0;
    set_nav(out, w);
    return 1;
}

/* Public submit path: fire RB_EV_SUBMIT on the form, then build the URL
   unless a listener prevented the event. The build is delegated to the
   SUBMIT default action (ev_default_action) so that a listener runs before
   anything is queued; an un-prevented event that the default action skipped
   (a removed form) falls back to building here. */
static int submit_form(int form, int submitter) {
    if (form <= 0 || form >= g_node_n || g_nodes[form].dead) return 0;
    /* A POST form is refused outright: no event, no navigation (same policy as
       the click/Enter guards, kept here so the fallback cannot build one). */
    if (g_nodes[form].method_post) return 0;
    int prev_form = g_submit_form;
    int prev_sub = g_submit_submitter;
    int nav0 = g_nav_len;
    int dropped0 = rb_event_dropped();
    g_submit_form = form;
    g_submit_submitter = submitter;
    int prevented = 0;
    dispatch_ev(form, RB_EV_SUBMIT, 0, &prevented);
    /* The depth cap rejects the dispatch before any listener runs: the event
       was never delivered, so it must be treated like a prevented one (do not
       build/navigate). A silent navigation the page could never observe would
       be worse than dropping the submit. */
    int delivered = (rb_event_dropped() == dropped0);
    /* The default action clears the pending form once it has built the URL. */
    int built = (g_submit_form != form);
    g_submit_form = prev_form;
    g_submit_submitter = prev_sub;
    if (prevented || !delivered) return 0;
    /* "built" only says the default action ran; report whether it really
       queued a navigation (a form with no controls builds nothing). */
    if (built) return g_nav_len > nav0;
    if (form >= g_node_n || g_nodes[form].dead) return 0;
    return submit_build(form, submitter);
}

static void collapse_path(char *path, int cap) {
    if (!path || cap < 2) { if (path && cap > 0) path[0] = 0; return; }
    char *tmp = g_collapse_tmp;
    char (*parts)[64] = g_collapse_parts;
    int max = cap < (int)sizeof(g_collapse_tmp) ? cap : (int)sizeof(g_collapse_tmp);
    int n = 0;
    while (path[n] && n + 1 < max) { tmp[n] = path[n]; n++; }
    tmp[n] = 0;
    int np = 0;
    int i = 0;
    if (tmp[0] == '/') i++;
    while (tmp[i] && np < 40) {
        int st = i;
        while (tmp[i] && tmp[i] != '/') i++;
        int len = i - st;
        if (len == 1 && tmp[st] == '.') {
        } else if (len == 2 && tmp[st] == '.' && tmp[st + 1] == '.') {
            if (np) np--;
        } else if (len > 0 && len < 64) {
            memcpy(parts[np], tmp + st, (size_t)len);
            parts[np][len] = 0;
            np++;
        }
        if (tmp[i] == '/') i++;
    }
    int w = 0;
    path[w++] = '/';
    for (int p = 0; p < np; p++) {
        int L = (int)strlen(parts[p]);
        for (int k = 0; k < L && w + 1 < cap; k++) path[w++] = parts[p][k];
        if (p + 1 < np && w + 1 < cap) path[w++] = '/';
    }
    path[w] = 0;
}

static int file_url_to_path(const char *href, char *out, int cap) {
    const char *p = href + 7;
    if (p[0] == '-' && p[1] == '/') p++;
    if (begins(p, "localhost/")) p += 9;
    if (p[0] != '/') {
        int w = 0;
        append_n(out, cap, &w, "/", 1);
        append_n(out, cap, &w, p, (int)strlen(p));
    } else {
        int w = 0;
        append_n(out, cap, &w, p, (int)strlen(p));
    }
    char *hash = out;
    while (*hash && *hash != '#') hash++;
    *hash = 0;
    char *q = out;
    while (*q && *q != '?') q++;
    *q = 0;
    collapse_path(out, cap);
    return 1;
}

int rb_resolve(const char *base, const char *href, char *out, int cap) {
    if (!out || cap < 2) return 0;
    out[0] = 0;
    if (!href || !href[0] || href[0] == '#') return 0;
    while (*href == ' ' || *href == '\t') href++;
    if (begins(href, "file://")) return file_url_to_path(href, out, cap);
    if (begins(href, "http://") || begins(href, "https://")) {
        int w = 0;
        append_n(out, cap, &w, href, (int)strlen(href));
        char *hash = out;
        while (*hash && *hash != '#') hash++;
        *hash = 0;
        return out[0] ? 1 : 0;
    }
    if (!base) base = "/";
    char query[960];
    query[0] = 0;
    const char *q = href;
    while (*q && *q != '?' && *q != '#') q++;
    if (*q == '?') {
        int i = 0;
        while (q[i] && q[i] != '#' && i + 1 < (int)sizeof(query)) { query[i] = q[i]; i++; }
        query[i] = 0;
    }
    int hlen = (int)(q - href);
    /* Query-only href ("?x=1"): keep the base path. Without this the relative
       branch below would truncate the path to the base's directory, so a form
       with no action (or <a href="?page=2">) would drop the page name. */
    if (href[0] == '?') {
        const char *b = base;
        while (*b && *b != '?' && *b != '#') b++;
        int w = 0;
        append_n(out, cap, &w, base, (int)(b - base));
        /* A base like "http://host" has an implicit root path; without the
           added '/' the result would be the invalid "http://host?q=1". */
        int schemed = begins(base, "http://") || begins(base, "https://");
        if (schemed) {
            const char *stop = begins(base, "https://") ? base + 8 : base + 7;
            int rooted = 0;
            for (const char *t = stop; t < b; t++) if (*t == '/') { rooted = 1; break; }
            if (!rooted && w + 1 < cap && w > 0) out[w++] = '/';
        }
        append_n(out, cap, &w, query, (int)strlen(query));
        return out[0] ? 1 : 0;
    }
    int http = begins(base, "http://") || begins(base, "https://");
    if (http) {
        const char *p = base + (begins(base, "https://") ? 8 : 7);
        const char *slash = p;
        while (*slash && *slash != '/' && *slash != '?' && *slash != '#') slash++;
        int origin = (int)(slash - base);
        if (hlen >= 2 && href[0] == '/' && href[1] == '/') {
            /* Scheme-relative: //host/path keeps the base scheme. Query is
               appended here because the common tail only runs for resolves
               that fall through; only the fragment is dropped. */
            int w = 0;
            const char *scheme = begins(base, "https://") ? "https:" : "http:";
            append_n(out, cap, &w, scheme, (int)strlen(scheme));
            append_n(out, cap, &w, href, hlen);
            char *hash = out;
            while (*hash && *hash != '#') hash++;
            *hash = 0;
            if (query[0]) {
                w = (int)strlen(out);
                append_n(out, cap, &w, query, (int)strlen(query));
            }
            return out[0] ? 1 : 0;
        }
        if (hlen > 0 && href[0] == '/') {
            int w = 0;
            append_n(out, cap, &w, base, origin);
            append_n(out, cap, &w, href, hlen);
        } else {
            const char *path = (*slash == '/') ? slash : "/";
            int path_len = 0;
            if (*slash == '/') {
                while (path[path_len] && path[path_len] != '?' && path[path_len] != '#') path_len++;
            }
            int dir = path_len;
            while (dir > 0 && path[dir - 1] != '/') dir--;
            if (dir == 0) dir = path_len;
            int w = 0;
            append_n(out, cap, &w, base, origin);
            if (*slash == '/') append_n(out, cap, &w, path, dir);
            else append_n(out, cap, &w, "/", 1);
            append_n(out, cap, &w, href, hlen);
        }
        char *path = out;
        if (begins(out, "https://")) path = out + 8;
        else if (begins(out, "http://")) path = out + 7;
        while (*path && *path != '/') path++;
        if (*path == '/') collapse_path(path, cap - (int)(path - out));
    } else {
        char pathbase[480];
        int w = 0;
        if (begins(base, "file://")) file_url_to_path(base, pathbase, (int)sizeof(pathbase));
        else {
            append_n(pathbase, (int)sizeof(pathbase), &w, base, (int)strlen(base));
        }
        if (hlen > 0 && href[0] == '/') {
            w = 0;
            append_n(out, cap, &w, href, hlen);
        } else {
            int n = (int)strlen(pathbase);
            int dir = n;
            while (dir > 0 && pathbase[dir - 1] != '/') dir--;
            w = 0;
            if (dir == 0) append_n(out, cap, &w, "/", 1);
            else append_n(out, cap, &w, pathbase, dir);
            append_n(out, cap, &w, href, hlen);
        }
        collapse_path(out, cap);
    }
    if (query[0]) {
        int w = (int)strlen(out);
        append_n(out, cap, &w, query, (int)strlen(query));
    }
    return out[0] ? 1 : 0;
}

/* =====================================================================
   Mutable DOM core (PART 1).

   Handles are ints; 0 is the null handle. Mutators never relayout -- call
   rb_relayout() to rebuild RbBox g_box, then read geometry. Create calls
   return a detached node; append/insert/replace attach it.
   ===================================================================== */

static int node_ok(int node) {
    return node > 0 && node < g_node_n && !g_nodes[node].dead;
}

static int name_eq(const char *name, int n, const char *lit) {
    if (n < 0) n = name ? (int)strlen(name) : 0;
    return name != 0 && n > 0 && eq_lit(name, n, lit);
}

/* ---- images: the per-<img> record the host fills in (see engine.h) ---- */

void rb_set_images(int on) { g_images = on != 0; }
int rb_images_on(void) { return g_images; }

static int is_img(int node) {
    return node_ok(node) && g_nodes[node].kind == 0 && g_nodes[node].tag == RB_TAG_IMG;
}

/* A new src/srcset means a new image: forget the old outcome. */
static void img_src_changed(int node, const char *ln, int nlen) {
    if (!is_img(node) || !(name_eq(ln, nlen, "src") || name_eq(ln, nlen, "srcset"))) return;
    Node *n = &g_nodes[node];
    n->img_state = RB_IMG_NONE;
    n->img_w = n->img_h = 0;
    n->img_slot = 0;
}

int rb_dom_img_next(int after) {
    if (after < 1) after = 1;
    for (int i = after + 1; i < g_node_n; i++)
        if (!g_nodes[i].dead && g_nodes[i].kind == 0 && g_nodes[i].tag == RB_TAG_IMG) return i;
    return 0;
}

int rb_dom_img_state(int node, int *slot, int *nw, int *nh) {
    if (!is_img(node)) return RB_IMG_NONE;
    const Node *n = &g_nodes[node];
    if (slot) *slot = n->img_slot;
    if (nw) *nw = n->img_w;
    if (nh) *nh = n->img_h;
    return n->img_state;
}

int rb_dom_img_set(int node, int state, int slot, int nw, int nh) {
    if (!is_img(node) || state < RB_IMG_NONE || state > RB_IMG_EMPTY) return -1;
    Node *n = &g_nodes[node];
    if (state != RB_IMG_OK) nw = nh = 0;
    if (nw < 0 || nh < 0) nw = nh = 0;
    n->img_state = (uint8_t)state;
    n->img_slot = (uint16_t)(slot > 0 ? slot : 0);
    n->img_w = (uint16_t)(nw > 65535 ? 65535 : nw);
    n->img_h = (uint16_t)(nh > 65535 ? 65535 : nh);
    /* An image laid out at an explicit size already has its RB_IMAGE box:
       the painter picks the pixels up. Anything else changes the layout
       (natural size, or the alt text of a broken image). */
    return (state == RB_IMG_OK && n->img_fixed && g_web) ? 0 : 1;
}

/* ---- canvas (see engine.h) ---- */

void rb_set_canvas(int on) { g_canvas = on != 0; }
int rb_canvas_on(void) { return g_canvas; }

/* One canvas dimension attribute, HTML's rules for non-negative integers. */
static int canvas_dim(int node, const char *name, int def) {
    const char *v;
    int vn;
    if (!attr_get(node, name, (int)strlen(name), &v, &vn)) return def;
    int i = 0;
    while (i < vn && (v[i] == ' ' || v[i] == '\t' || v[i] == '\n' || v[i] == '\r' || v[i] == '\f')) i++;
    if (i < vn && v[i] == '+') i++;
    if (i >= vn || v[i] < '0' || v[i] > '9') return def;
    int n = 0;
    for (; i < vn && v[i] >= '0' && v[i] <= '9'; i++)
        if (n < 65535) n = n * 10 + (v[i] - '0');
    return n > 65535 ? 65535 : n;
}

int rb_canvas_attr_size(int node, int *w, int *h) {
    int ok = node_ok(node) && g_nodes[node].kind == 0 && g_nodes[node].tag == RB_TAG_CANVAS;
    int cw = ok ? canvas_dim(node, "width", 300) : 300;
    int ch = ok ? canvas_dim(node, "height", 150) : 150;
    if (w) *w = cw;
    if (h) *h = ch;
    return ok;
}

/* Copy n bytes into an existing pool slot in place when they fit (the old
   slot holds old_len+1 bytes, so n <= old_len is safe and a repeated
   set_text of the same size never grows g_pool). Otherwise bump-allocate.
   memmove because a caller may hand us a pointer into g_pool itself. */
static int store_str(uint32_t *off, uint32_t *len, const char *s, int n) {
    if (n < 0) n = s ? (int)strlen(s) : 0;
    if (n > 0 && !s) return 0;
    if (n == 0) { *len = 0; return 1; }
    if (*len > 0 && n <= (int)*len) {
        memmove(g_pool + *off, s, (size_t)n);
        g_pool[*off + n] = 0;
        *len = (uint32_t)n;
        return 1;
    }
    int o = pool_add(s, n);
    if (o < 0) return 0;
    *off = (uint32_t)o;
    *len = (uint32_t)n;
    return 1;
}

/* Remove node from g_focus so a mutation cannot leave the tab order pointing
   at a recycled slot until the next relayout rebuilds it. */
static void focus_drop(int node) {
    for (int i = 0; i < g_focus_n; i++) {
        if (g_focus[i] != node) continue;
        for (int j = i; j + 1 < g_focus_n; j++) g_focus[j] = g_focus[j + 1];
        g_focus_n--;
        i--;
    }
}

/* Mark a whole subtree dead and return every slot to the free list. Iterative
   (explicit stack): a 512-deep chain would otherwise cost ~10KB of the 16KB
   shell stack. free_node does not read the children, so a pre-order walk that
   pushes each node's children before freeing it is enough and each slot is
   pushed at most once. */
static uint16_t g_recycle_stack[MAX_NODES];
static void recycle_subtree(int node) {
    if (!node) return;
    int top = 0;
    g_recycle_stack[top++] = (uint16_t)node;
    while (top > 0) {
        int nd = g_recycle_stack[--top];
        int c = g_nodes[nd].first;      /* capture children before free_node */
        while (c) {
            if (top < MAX_NODES) g_recycle_stack[top++] = (uint16_t)c;
            c = g_nodes[c].next;
        }
        focus_drop(nd);
        free_node(nd);
    }
}

/* O(1) unlink of child from its parent, using the prev/next links. */
static void detach(int child) {
    int p = g_nodes[child].parent;
    if (!p) return;
    int prv = g_nodes[child].prev;
    int nxt = g_nodes[child].next;
    if (prv) g_nodes[prv].next = (uint16_t)nxt;
    else g_nodes[p].first = (uint16_t)nxt;
    if (nxt) g_nodes[nxt].prev = (uint16_t)prv;
    g_nodes[child].parent = 0;
    g_nodes[child].prev = 0;
    g_nodes[child].next = 0;
}

static int is_ancestor(int anc, int node) {
    for (int p = node; p; p = g_nodes[p].parent) if (p == anc) return 1;
    return 0;
}

int rb_dom_create_element(int tag) {
    if (tag < 0 || tag > 255) return 0;
    int n = new_node();
    if (!n) return 0;
    g_nodes[n].tag = (uint8_t)tag;
    return n;
}

int rb_dom_create_text(const char *s, int len) {
    int n = len;
    if (n < 0) n = s ? (int)strlen(s) : 0;
    if (n > 0 && !s) return 0;
    int off = 0;
    if (n > 0) {
        off = pool_add(s, n);
        if (off < 0) return 0;
    }
    int t = new_node();
    if (!t) return 0;
    g_nodes[t].kind = 1;
    g_nodes[t].text_off = (uint32_t)off;
    g_nodes[t].text_len = (uint32_t)n;
    return t;
}

int rb_dom_append_child(int parent, int child) {
    if (!node_ok(parent) || !node_ok(child) || child == 1) return 0;
    if (g_nodes[parent].kind != 0) return 0;
    if (child == parent || is_ancestor(child, parent)) return 0;
    detach(child);
    add_child(parent, child);
    return 1;
}

int rb_dom_insert_before(int parent, int child, int ref) {
    if (!node_ok(parent) || !node_ok(child) || child == 1) return 0;
    if (g_nodes[parent].kind != 0) return 0;
    if (child == parent || is_ancestor(child, parent)) return 0;
    if (ref == 0) return rb_dom_append_child(parent, child);
    if (!node_ok(ref) || g_nodes[ref].parent != parent) return 0;
    if (child == ref) return 1;
    detach(child);
    int prv = g_nodes[ref].prev;
    g_nodes[child].parent = (uint16_t)parent;
    g_nodes[child].prev = (uint16_t)prv;
    g_nodes[child].next = (uint16_t)ref;
    g_nodes[ref].prev = (uint16_t)child;
    if (prv) g_nodes[prv].next = (uint16_t)child;
    else g_nodes[parent].first = (uint16_t)child;
    return 1;
}

int rb_dom_replace_child(int parent, int newchild, int oldchild) {
    if (!node_ok(parent) || !node_ok(newchild) || !node_ok(oldchild) || newchild == 1) return 0;
    if (g_nodes[parent].kind != 0) return 0;
    if (g_nodes[oldchild].parent != parent) return 0;
    if (newchild == oldchild) return 1;
    if (newchild == parent || is_ancestor(newchild, parent)) return 0;
    detach(newchild);
    int prv = g_nodes[oldchild].prev;
    int nxt = g_nodes[oldchild].next;
    if (prv) g_nodes[prv].next = (uint16_t)newchild;
    else g_nodes[parent].first = (uint16_t)newchild;
    if (nxt) g_nodes[nxt].prev = (uint16_t)newchild;
    g_nodes[newchild].parent = (uint16_t)parent;
    g_nodes[newchild].prev = (uint16_t)prv;
    g_nodes[newchild].next = (uint16_t)nxt;
    g_nodes[oldchild].parent = 0;
    g_nodes[oldchild].prev = 0;
    g_nodes[oldchild].next = 0;
    recycle_subtree(oldchild);
    return 1;
}

int rb_dom_remove_child(int child) {
    if (!node_ok(child) || child == 1) return 0;
    detach(child);
    recycle_subtree(child);
    return 1;
}

int rb_dom_root(int node) {
    if (!node_ok(node)) return 0;
    int guard = MAX_NODES;
    while (g_nodes[node].parent && guard-- > 0) node = g_nodes[node].parent;
    return node;
}

int rb_dom_release(int node) {
    if (!node_ok(node) || node == 1 || g_nodes[node].parent) return 0;
    recycle_subtree(node);
    return 1;
}

int rb_dom_is_text(int node) { return node_ok(node) && g_nodes[node].kind == 1; }
int rb_dom_parent(int node) { return node_ok(node) ? g_nodes[node].parent : 0; }
/* Generated ::before/::after children are real nodes but not part of the
   document the API describes. */
static int skip_pseudo(int n) {
    while (n && g_nodes[n].pseudo) n = g_nodes[n].next;
    return n;
}
int rb_dom_first(int node) { return node_ok(node) ? skip_pseudo(g_nodes[node].first) : 0; }
int rb_dom_next(int node) { return node_ok(node) ? skip_pseudo(g_nodes[node].next) : 0; }

int rb_dom_children_count(int node) {
    if (!node_ok(node)) return 0;
    int n = 0;
    for (int c = g_nodes[node].first; c; c = g_nodes[c].next)
        if (!g_nodes[c].dead && !g_nodes[c].pseudo) n++;
    return n;
}

int rb_dom_set_text(int node, const char *s, int len) {
    if (!node_ok(node)) return 0;
    if (len < 0) len = s ? (int)strlen(s) : 0;
    if (len > 0 && !s) return 0;
    Node *n = &g_nodes[node];
    if (n->kind == 1) {
        if (!store_str(&n->text_off, &n->text_len, s, len)) return 0;
        return 1;
    }
    /* <input>/<textarea> carry their value on the node itself, exactly as the
       parser stores it, so set_text rewrites that field. */
    if (n->tag == RB_TAG_INPUT || n->tag == RB_TAG_TEXTAREA) {
        if (!store_str(&n->text_off, &n->text_len, s, len)) return 0;
        style_touch(node);
        return 1;
    }
    /* textContent =: a single text child is rewritten in place, which keeps a
       repeated set_text from allocating a new node and pool slot each time. */
    int c = n->first;
    if (c && !g_nodes[c].dead && g_nodes[c].kind == 1 && g_nodes[c].next == 0) {
        if (!store_str(&g_nodes[c].text_off, &g_nodes[c].text_len, s, len)) return 0;
        style_touch(node);
        return 1;
    }
    /* Otherwise drop every child and append one fresh text node. Pool is
       reserved before the tree is touched, so an exhausted pool returns 0
       with the subtree intact (the old order destroyed it first). When there
       are children to recycle, that guarantees a free slot for the text node. */
    int off = -1;
    if (len > 0) {
        off = pool_add(s, len);
        if (off < 0) return 0;
    }
    int ch = n->first;
    n->first = 0;
    while (ch) {
        int nx = g_nodes[ch].next;
        recycle_subtree(ch);
        ch = nx;
    }
    if (len > 0) {
        int t = new_node();
        if (!t) return 0;   /* atomic: only reachable when there were no children */
        g_nodes[t].kind = 1;
        g_nodes[t].text_off = (uint32_t)off;
        g_nodes[t].text_len = (uint32_t)len;
        add_child(node, t);
    }
    style_touch(node);
    return 1;
}

int rb_dom_get_text(int node, char *out, int cap) {
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    if (!node_ok(node)) return 0;
    if (g_nodes[node].kind == 1) {
        int n = g_nodes[node].text_len;
        if (n > cap - 1) n = cap - 1;
        if (n > 0) memcpy(out, g_pool + g_nodes[node].text_off, (size_t)n);
        out[n] = 0;
        return n;
    }
    int n = collect_rec(node, out, cap - 1, 0, 0);
    if (n < 0) n = 0;
    out[n] = 0;
    return n;
}

/* type= only means something on a form control. Non-control tags ignore it:
   otherwise rb_dom_set_attr(div, "type", "checkbox") would leave ctl=RADIO on
   a <div> and ev_default_action would toggle a nonexistent control. */
static int takes_type_attr(int tag) {
    return tag == RB_TAG_INPUT || tag == RB_TAG_TEXTAREA || tag == RB_TAG_BUTTON;
}

static void apply_type(Node *n, const char *val, int vn) {
    if (n->tag == RB_TAG_BUTTON) {
        n->input_btn = 0;
        if (eq_lit(val, vn, "reset")) n->ctl = RB_CTL_RESET;
        else if (eq_lit(val, vn, "button")) n->ctl = RB_CTL_BUTTON;
        else n->ctl = RB_CTL_SUBMIT;   /* submit is the button default */
        return;
    }
    if (n->tag != RB_TAG_INPUT) return;  /* textarea's type is fixed */
    n->input_btn = 0;
    n->ctl = RB_CTL_TEXT;
    if (eq_lit(val, vn, "hidden")) n->hidden = 1;
    else if (eq_lit(val, vn, "submit")) { n->input_btn = 1; n->ctl = RB_CTL_SUBMIT; }
    else if (eq_lit(val, vn, "button")) { n->input_btn = 1; n->ctl = RB_CTL_BUTTON; }
    else if (eq_lit(val, vn, "reset")) { n->input_btn = 1; n->ctl = RB_CTL_RESET; }
    else if (eq_lit(val, vn, "image")) { n->input_btn = 1; n->ctl = RB_CTL_IMAGE; }
    else if (eq_lit(val, vn, "checkbox")) n->ctl = RB_CTL_CHECKBOX;
    else if (eq_lit(val, vn, "radio")) n->ctl = RB_CTL_RADIO;
}

/* ---- attributes --------------------------------------------------------------
   id, class, style, href, name, action, onclick, value/alt (one slot: text_off)
   and the boolean flags have Node fields. Everything else ("other attributes")
   lives in the node's blob of "name\0value\0" pairs, names lower-case; the
   parser fills it (a whitelist, or everything while scripting) and the
   mutators below edit it. type and method are in the blob AND drive a field. */
#define BLOB_MAX 16384
static char g_blob_tmp[BLOB_MAX];

static int attr_name_ok(const char *name, int nlen) {
    if (!name || nlen < 1 || nlen > 64) return 0;
    for (int i = 0; i < nlen; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c <= ' ' || c == '/' || c == '>' || c == '<' || c == '=' || c == '"' || c == '\'' || c == 127) return 0;
    }
    return 1;
}

static int blob_lookup(const Node *nd, const char *ln, int nn, const char **v, int *vn) {
    if (!nd->attr_len) return 0;
    const char *p = g_pool + nd->attr_off;
    const char *e = p + nd->attr_len;
    while (p < e) {
        int pl = (int)strlen(p);
        const char *val = p + pl + 1;
        int lv = (int)strlen(val);
        if (pl == nn && memcmp(p, ln, (size_t)nn) == 0) { *v = val; *vn = lv; return 1; }
        p = val + lv + 1;
    }
    return 0;
}

/* Replace (or append, or with remove drop) the blob entry `ln`. A blob that
   still fits its old pool slot is rewritten in place; otherwise it moves. */
static int blob_edit(int node, const char *ln, int nn, const char *val, int vn, int remove) {
    Node *nd = &g_nodes[node];
    int w = 0, done = 0;
    const char *p = nd->attr_len ? g_pool + nd->attr_off : 0;
    const char *e = p ? p + nd->attr_len : 0;
    if (vn < 0) vn = 0;
    while (p && p < e) {
        int pl = (int)strlen(p);
        const char *v = p + pl + 1;
        int lv = (int)strlen(v);
        const char *next = v + lv + 1;
        int match = pl == nn && memcmp(p, ln, (size_t)nn) == 0;
        if (match && remove) { p = next; continue; }
        if (match && !done) {
            done = 1;
            if (w + nn + vn + 2 > BLOB_MAX) return 0;
            memcpy(g_blob_tmp + w, ln, (size_t)nn); w += nn; g_blob_tmp[w++] = 0;
            if (vn) memcpy(g_blob_tmp + w, val, (size_t)vn);
            w += vn; g_blob_tmp[w++] = 0;
        } else {
            if (w + pl + lv + 2 > BLOB_MAX) return 0;
            memcpy(g_blob_tmp + w, p, (size_t)(pl + lv + 2)); w += pl + lv + 2;
        }
        p = next;
    }
    if (!done && !remove) {
        if (w + nn + vn + 2 > BLOB_MAX) return 0;
        memcpy(g_blob_tmp + w, ln, (size_t)nn); w += nn; g_blob_tmp[w++] = 0;
        if (vn) memcpy(g_blob_tmp + w, val, (size_t)vn);
        w += vn; g_blob_tmp[w++] = 0;
    }
    if (w == 0) { nd->attr_len = 0; return 1; }
    if (nd->attr_len && w <= (int)nd->attr_len) {
        memcpy(g_pool + nd->attr_off, g_blob_tmp, (size_t)w);
        nd->attr_len = (uint32_t)w;
        return 1;
    }
    int off = pool_add(g_blob_tmp, w);
    if (off < 0) return 0;
    nd->attr_off = (uint32_t)off;
    nd->attr_len = (uint32_t)w;
    return 1;
}

static int type_is(const Node *nd, const char *lit) {
    const char *v; int vn;
    return blob_lookup(nd, "type", 4, &v, &vn) && eq_lit(v, vn, lit);
}

int rb_dom_set_attr(int node, const char *name, int nlen, const char *val, int vlen) {
    if (!node_ok(node)) return 0;
    if (nlen < 0) nlen = name ? (int)strlen(name) : 0;
    if (vlen < 0) vlen = val ? (int)strlen(val) : 0;
    if (vlen > 0 && !val) return 0;
    if (!attr_name_ok(name, nlen)) return 0;
    char ln[65];
    for (int i = 0; i < nlen; i++) ln[i] = (char)lower_ch((unsigned char)name[i]);
    ln[nlen] = 0;
    Node *n = &g_nodes[node];
    img_src_changed(node, ln, nlen);
    if (name_eq(ln, nlen, "id")) {
        if (!store_str(&n->id_off, &n->id_len, val, vlen)) return 0;
        style_touch(node);
        return 1;
    }
    if (name_eq(ln, nlen, "class")) {
        if (!store_str(&n->class_off, &n->class_len, val, vlen)) return 0;
        style_touch(node);
        return 1;
    }
    if (name_eq(ln, nlen, "style")) {
        if (!store_str(&n->style_off, &n->style_len, val, vlen)) return 0;
        style_touch(node);
        return 1;
    }
    if (name_eq(ln, nlen, "href")) {
        if (!store_str(&n->href_off, &n->href_len, val, vlen)) return 0;
        style_touch(node);
        return 1;
    }
    if (name_eq(ln, nlen, "name")) {
        if (!store_str(&n->name_off, &n->name_len, val, vlen)) return 0;
        style_touch(node);
        return 1;
    }
    if (name_eq(ln, nlen, "action"))
        return store_str(&n->action_off, &n->action_len, val, vlen);
    if (name_eq(ln, nlen, "onclick"))
        return store_str(&n->click_off, &n->click_len, val, vlen);
    if (name_eq(ln, nlen, "value") || name_eq(ln, nlen, "alt")) {
        if (!store_str(&n->text_off, &n->text_len, val, vlen)) return 0;
        style_touch(node);
        return 1;
    }
    if (name_eq(ln, nlen, "checked")) { n->checked = 1; style_touch(node); return 1; }
    if (name_eq(ln, nlen, "disabled")) { n->disabled = 1; style_touch(node); return 1; }
    if (name_eq(ln, nlen, "hidden")) { n->hidden = 1; style_touch(node); return 1; }
    if (name_eq(ln, nlen, "method"))
        n->method_post = (uint8_t)(vlen == 4 &&
            (val[0] | 32) == 'p' && (val[1] | 32) == 'o' &&
            (val[2] | 32) == 's' && (val[3] | 32) == 't');
    else if (name_eq(ln, nlen, "type") && takes_type_attr(n->tag)) {
        /* a hidden input came from type=hidden: changing the type shows it again */
        if (n->tag == RB_TAG_INPUT && type_is(n, "hidden")) n->hidden = 0;
        apply_type(n, val, vlen);
    }
    if (!blob_edit(node, ln, nlen, val, vlen, 0)) return 0;
    style_touch(node);          /* attribute selectors may now match differently */
    return 1;
}

static int copy_field(const uint32_t *off, const uint32_t *len, char *out, int cap) {
    int n = *len;
    if (n >= cap) n = cap - 1;
    if (n > 0) memcpy(out, g_pool + *off, (size_t)n);
    out[n] = 0;
    return *len > 0;
}

static int copy_span(const char *v, int vn, char *out, int cap) {
    if (vn >= cap) vn = cap - 1;
    if (vn > 0) memcpy(out, v, (size_t)vn);
    out[vn] = 0;
    return 1;
}

static int put_lit(char *out, int cap, const char *lit) {
    int n = (int)strlen(lit);
    if (n >= cap) n = cap - 1;
    memcpy(out, lit, (size_t)n);
    out[n] = 0;
    return 1;
}

int rb_dom_get_attr(int node, const char *name, int nlen, char *out, int cap) {
    if (out && cap > 0) out[0] = 0;
    if (!node_ok(node) || !out || cap <= 0) return 0;
    if (nlen < 0) nlen = name ? (int)strlen(name) : 0;
    if (!attr_name_ok(name, nlen)) return 0;
    char ln[65];
    for (int i = 0; i < nlen; i++) ln[i] = (char)lower_ch((unsigned char)name[i]);
    ln[nlen] = 0;
    name = ln;
    Node *n = &g_nodes[node];
    if (name_eq(name, nlen, "id")) return copy_field(&n->id_off, &n->id_len, out, cap);
    if (name_eq(name, nlen, "class")) return copy_field(&n->class_off, &n->class_len, out, cap);
    if (name_eq(name, nlen, "style")) return copy_field(&n->style_off, &n->style_len, out, cap);
    if (name_eq(name, nlen, "href")) return copy_field(&n->href_off, &n->href_len, out, cap);
    if (name_eq(name, nlen, "name")) return copy_field(&n->name_off, &n->name_len, out, cap);
    if (name_eq(name, nlen, "action")) return copy_field(&n->action_off, &n->action_len, out, cap);
    if (name_eq(name, nlen, "onclick")) return copy_field(&n->click_off, &n->click_len, out, cap);
    if (name_eq(name, nlen, "value") || name_eq(name, nlen, "alt"))
        return copy_field(&n->text_off, &n->text_len, out, cap);
    if (name_eq(name, nlen, "checked")) return n->checked ? put_lit(out, cap, "") : 0;
    if (name_eq(name, nlen, "disabled")) return n->disabled ? put_lit(out, cap, "") : 0;
    if (name_eq(name, nlen, "hidden"))
        return n->hidden && !(n->tag == RB_TAG_INPUT && type_is(n, "hidden")) ? put_lit(out, cap, "") : 0;
    const char *v; int vn;
    if (blob_lookup(n, ln, nlen, &v, &vn)) return copy_span(v, vn, out, cap);
    if (name_eq(name, nlen, "method"))
        return n->method_post ? put_lit(out, cap, "post") : 0;
    if (name_eq(name, nlen, "type")) {
        /* the effective type of a control, even when the attribute is absent
           (use rb_dom_has_attr to tell) */
        if (!takes_type_attr(n->tag)) return 0;
        if (n->tag == RB_TAG_TEXTAREA) return put_lit(out, cap, "textarea");
        if (n->tag == RB_TAG_BUTTON) {
            if (n->ctl == RB_CTL_RESET) return put_lit(out, cap, "reset");
            if (n->ctl == RB_CTL_BUTTON) return put_lit(out, cap, "button");
            return put_lit(out, cap, "submit");
        }
        if (n->hidden) return put_lit(out, cap, "hidden");
        if (n->input_btn) {
            if (n->ctl == RB_CTL_SUBMIT) return put_lit(out, cap, "submit");
            if (n->ctl == RB_CTL_BUTTON) return put_lit(out, cap, "button");
            if (n->ctl == RB_CTL_RESET) return put_lit(out, cap, "reset");
            if (n->ctl == RB_CTL_IMAGE) return put_lit(out, cap, "image");
        }
        if (n->ctl == RB_CTL_CHECKBOX) return put_lit(out, cap, "checkbox");
        if (n->ctl == RB_CTL_RADIO) return put_lit(out, cap, "radio");
        return put_lit(out, cap, "text");
    }
    return 0;
}

/* Length of the attribute's value, or -1 when the element does not have it
   (rb_dom_get_attr truncates to the buffer; this sizes one). */
int rb_dom_attr_len(int node, const char *name, int nlen) {
    if (!node_ok(node)) return -1;
    if (nlen < 0) nlen = name ? (int)strlen(name) : 0;
    if (!rb_dom_has_attr(node, name, nlen)) return -1;
    char ln[65];
    if (!attr_name_ok(name, nlen)) return -1;
    for (int i = 0; i < nlen; i++) ln[i] = (char)lower_ch((unsigned char)name[i]);
    ln[nlen] = 0;
    Node *n = &g_nodes[node];
    const char *v; int vn;
    if (name_eq(ln, nlen, "id")) return (int)n->id_len;
    if (name_eq(ln, nlen, "class")) return (int)n->class_len;
    if (name_eq(ln, nlen, "style")) return (int)n->style_len;
    if (name_eq(ln, nlen, "href")) return (int)n->href_len;
    if (name_eq(ln, nlen, "name")) return (int)n->name_len;
    if (name_eq(ln, nlen, "action")) return (int)n->action_len;
    if (name_eq(ln, nlen, "onclick")) return (int)n->click_len;
    if (name_eq(ln, nlen, "value") || name_eq(ln, nlen, "alt")) return (int)n->text_len;
    if (name_eq(ln, nlen, "checked") || name_eq(ln, nlen, "disabled") || name_eq(ln, nlen, "hidden")) return 0;
    if (blob_lookup(n, ln, nlen, &v, &vn)) return vn;
    return name_eq(ln, nlen, "method") ? 4 : 0;
}

int rb_dom_has_attr(int node, const char *name, int nlen) {
    if (!node_ok(node)) return 0;
    if (nlen < 0) nlen = name ? (int)strlen(name) : 0;
    if (!attr_name_ok(name, nlen)) return 0;
    char ln[65];
    for (int i = 0; i < nlen; i++) ln[i] = (char)lower_ch((unsigned char)name[i]);
    ln[nlen] = 0;
    name = ln;
    Node *n = &g_nodes[node];
    if (name_eq(name, nlen, "id")) return n->id_len > 0;
    if (name_eq(name, nlen, "class")) return n->class_len > 0;
    if (name_eq(name, nlen, "style")) return n->style_len > 0;
    if (name_eq(name, nlen, "href")) return n->href_len > 0;
    if (name_eq(name, nlen, "name")) return n->name_len > 0;
    if (name_eq(name, nlen, "action")) return n->action_len > 0;
    if (name_eq(name, nlen, "onclick")) return n->click_len > 0;
    if (name_eq(name, nlen, "value") || name_eq(name, nlen, "alt")) return n->text_len > 0;
    if (name_eq(name, nlen, "checked")) return n->checked;
    if (name_eq(name, nlen, "disabled")) return n->disabled;
    if (name_eq(name, nlen, "hidden")) return n->hidden && !(n->tag == RB_TAG_INPUT && type_is(n, "hidden"));
    const char *v; int vn;
    if (blob_lookup(n, ln, nlen, &v, &vn)) return 1;
    if (name_eq(name, nlen, "method")) return n->method_post;
    return 0;
}

/* 1 for any live node and well-formed name, whether or not the attribute was there. */
int rb_dom_remove_attr(int node, const char *name, int nlen) {
    if (!node_ok(node)) return 0;
    if (nlen < 0) nlen = name ? (int)strlen(name) : 0;
    if (!attr_name_ok(name, nlen)) return 0;
    char ln[65];
    for (int i = 0; i < nlen; i++) ln[i] = (char)lower_ch((unsigned char)name[i]);
    ln[nlen] = 0;
    name = ln;
    Node *n = &g_nodes[node];
    img_src_changed(node, ln, nlen);
    if (name_eq(name, nlen, "id")) { n->id_len = 0; style_touch(node); return 1; }
    if (name_eq(name, nlen, "class")) { n->class_len = 0; style_touch(node); return 1; }
    if (name_eq(name, nlen, "style")) { n->style_len = 0; style_touch(node); return 1; }
    if (name_eq(name, nlen, "href")) { n->href_len = 0; style_touch(node); return 1; }
    if (name_eq(name, nlen, "name")) { n->name_len = 0; style_touch(node); return 1; }
    if (name_eq(name, nlen, "action")) { n->action_len = 0; return 1; }
    if (name_eq(name, nlen, "onclick")) { n->click_len = 0; return 1; }
    if (name_eq(name, nlen, "value") || name_eq(name, nlen, "alt")) { n->text_len = 0; style_touch(node); return 1; }
    if (name_eq(name, nlen, "checked")) { n->checked = 0; style_touch(node); return 1; }
    if (name_eq(name, nlen, "disabled")) { n->disabled = 0; style_touch(node); return 1; }
    if (name_eq(name, nlen, "hidden")) {
        if (!(n->tag == RB_TAG_INPUT && type_is(n, "hidden"))) { n->hidden = 0; style_touch(node); }
        return 1;
    }
    if (name_eq(name, nlen, "method")) n->method_post = 0;
    else if (name_eq(name, nlen, "type") && takes_type_attr(n->tag)) {
        if (n->tag == RB_TAG_INPUT && type_is(n, "hidden")) n->hidden = 0;
        apply_type(n, "", 0);
    }
    if (!blob_edit(node, ln, nlen, 0, 0, 1)) return 0;
    style_touch(node);
    return 1;
}

/* Attribute enumeration: the dedicated fields in a fixed order, then the blob
   in source order. Pointers are into the pool or string literals. */
typedef struct { const char *name; int nn; const char *val; int vn; } AttrRef;

static int attr_ref(int node, int idx, AttrRef *r) {
    Node *n = &g_nodes[node];
    int k = 0;
#define DED(cond, nm, vp, vl) do { if (cond) { if (k++ == idx) { r->name = (nm); r->nn = (int)strlen(nm); r->val = (vp); r->vn = (int)(vl); return 1; } } } while (0)
    DED(n->id_len, "id", g_pool + n->id_off, n->id_len);
    DED(n->class_len, "class", g_pool + n->class_off, n->class_len);
    DED(n->style_len, "style", g_pool + n->style_off, n->style_len);
    DED(n->href_len, "href", g_pool + n->href_off, n->href_len);
    DED(n->name_len, "name", g_pool + n->name_off, n->name_len);
    DED(n->action_len, "action", g_pool + n->action_off, n->action_len);
    DED(n->click_len, "onclick", g_pool + n->click_off, n->click_len);
    DED(n->text_len && n->tag != RB_TAG_TEXTAREA,
        n->tag == RB_TAG_IMG || n->tag == RB_TAG_AREA ? "alt" : "value", g_pool + n->text_off, n->text_len);
    DED(n->checked, "checked", "", 0);
    DED(n->disabled, "disabled", "", 0);
    DED(n->hidden && !(n->tag == RB_TAG_INPUT && type_is(n, "hidden")), "hidden", "", 0);
#undef DED
    if (!n->attr_len) return 0;
    const char *p = g_pool + n->attr_off;
    const char *e = p + n->attr_len;
    while (p < e) {
        int pl = (int)strlen(p);
        const char *v = p + pl + 1;
        int lv = (int)strlen(v);
        if (k++ == idx) { r->name = p; r->nn = pl; r->val = v; r->vn = lv; return 1; }
        p = v + lv + 1;
    }
    return 0;
}

int rb_dom_attr_count(int node) {
    if (!node_ok(node) || g_nodes[node].kind != 0) return 0;
    AttrRef r;
    int n = 0;
    while (attr_ref(node, n, &r)) n++;
    return n;
}

int rb_dom_attr_at(int node, int i, char *name, int ncap, char *val, int vcap) {
    if (name && ncap > 0) name[0] = 0;
    if (val && vcap > 0) val[0] = 0;
    if (!node_ok(node) || g_nodes[node].kind != 0 || i < 0) return 0;
    AttrRef r;
    if (!attr_ref(node, i, &r)) return 0;
    if (name && ncap > 0) copy_span(r.name, r.nn, name, ncap);
    if (val && vcap > 0) copy_span(r.val, r.vn, val, vcap);
    return 1;
}

int rb_dom_set_id(int node, const char *id, int len) {
    if (!node_ok(node)) return 0;
    if (len < 0) len = id ? (int)strlen(id) : 0;
    if (len > 0 && !id) return 0;
    if (!store_str(&g_nodes[node].id_off, &g_nodes[node].id_len, id, len)) return 0;
    style_touch(node);
    return 1;
}

int rb_dom_class_contains(int node, const char *name, int len) {
    if (!node_ok(node)) return 0;
    if (len < 0) len = name ? (int)strlen(name) : 0;
    if (len <= 0 || !name) return 0;
    return class_has(node, name, len);
}

int rb_dom_add_class(int node, const char *name, int len) {
    if (!node_ok(node)) return 0;
    if (len < 0) len = name ? (int)strlen(name) : 0;
    if (len <= 0 || !name) return 0;
    if (class_has(node, name, len)) return 1;
    char buf[512];
    int w = 0;
    int cl = g_nodes[node].class_len;
    if (cl > 0) {
        if (cl >= (int)sizeof(buf) - 1) return 0;
        memcpy(buf, g_pool + g_nodes[node].class_off, (size_t)cl);
        w = cl;
        if (buf[w - 1] != ' ') buf[w++] = ' ';
    }
    if (w + len >= (int)sizeof(buf)) return 0;
    memcpy(buf + w, name, (size_t)len);
    w += len;
    if (!store_str(&g_nodes[node].class_off, &g_nodes[node].class_len, buf, w)) return 0;
    style_touch(node);
    return 1;
}

int rb_dom_remove_class(int node, const char *name, int len) {
    if (!node_ok(node)) return 0;
    if (len < 0) len = name ? (int)strlen(name) : 0;
    if (len <= 0 || !name) return 0;
    if (!class_has(node, name, len)) return 1;
    const char *s = g_pool + g_nodes[node].class_off;
    int sl = g_nodes[node].class_len;
    char buf[512];
    int w = 0;
    int i = 0;
    while (i < sl) {
        while (i < sl && s[i] == ' ') i++;
        int st = i;
        while (i < sl && s[i] != ' ') i++;
        int tl = i - st;
        if (tl <= 0) continue;
        if (tl == len && memcmp(s + st, name, (size_t)len) == 0) continue;
        if (w > 0) buf[w++] = ' ';
        if (w + tl >= (int)sizeof(buf)) return 0;
        memcpy(buf + w, s + st, (size_t)tl);
        w += tl;
    }
    if (!store_str(&g_nodes[node].class_off, &g_nodes[node].class_len, buf, w)) return 0;
    style_touch(node);
    return 1;
}

/* ---- scripting support -------------------------------------------------------- */

static int g_active;                                    /* active element (the event section owns it) */

void rb_set_scripting(int on) { g_scripting = on != 0; }

int rb_script_count(void) { return g_script_n; }

int rb_script_get(int i, int *node, const char **text, int *len) {
    if (i < 0 || i >= g_script_n) return 0;
    if (node) *node = g_script[i].node;
    if (text) *text = g_script[i].text;
    if (len) *len = g_script[i].len;
    return 1;
}

void rb_request_nav(const char *url, int len) {
    if (len < 0) len = url ? (int)strlen(url) : 0;
    if (!url) len = 0;
    set_nav(url, len);
}

/* ---- tag names ---------------------------------------------------------------- */

int rb_tag_from_name(const char *name, int len) {
    if (len < 0) len = name ? (int)strlen(name) : 0;
    if (!name) return RB_TAG_OTHER;
    return tag_of(name, len);
}

static const char *tag_str(int node, int *n) {
    const Node *nd = &g_nodes[node];
    const char *s = "";
    if (nd->kind == 1) s = "#text";
    else if (nd->tag == RB_TAG_DOC) s = "#document";
    else if (nd->tag == RB_TAG_OTHER) {
        if (nd->tname && nd->tname <= g_tn_n) {
            *n = g_tn[nd->tname - 1].len;
            return g_pool + g_tn[nd->tname - 1].off;
        }
        s = "unknown";
    } else s = tag_name_of(nd->tag, 0);
    *n = (int)strlen(s);
    return s;
}

int rb_dom_tag_name(int node, char *out, int cap) {
    if (out && cap > 0) out[0] = 0;
    if (!node_ok(node)) return 0;
    int n;
    const char *s = tag_str(node, &n);
    if (out && cap > 0) copy_span(s, n, out, cap);
    return n;
}

int rb_dom_create_element_named(const char *name, int len) {
    if (len < 0) len = name ? (int)strlen(name) : 0;
    if (!attr_name_ok(name, len) || !is_alpha_c((unsigned char)name[0])) return 0;
    int tag = tag_of(name, len);
    int n = rb_dom_create_element(tag);
    if (!n) return 0;
    if (tag == RB_TAG_OTHER) {
        g_nodes[n].name_hash = name_hash(name, len);
        g_nodes[n].tname = (uint8_t)tname_intern(name, len);
    }
    return n;
}

/* ---- siblings, text ------------------------------------------------------------ */

int rb_dom_prev(int node) {
    if (!node_ok(node)) return 0;
    int p = g_nodes[node].prev;
    while (p && g_nodes[p].pseudo) p = g_nodes[p].prev;
    return p;
}

int rb_dom_last(int node) {
    if (!node_ok(node)) return 0;
    int last = 0;
    for (int c = g_nodes[node].first; c; c = g_nodes[c].next)
        if (!g_nodes[c].pseudo) last = c;
    return last;
}

/* Pre-order successor of cur inside root's subtree (0 at the end), skipping
   generated nodes. Iterative: a script can build a tree deeper than the shell
   stack allows recursing over. */
static int dfs_next(int cur, int root, int descend) {
    if (descend) {
        int c = skip_pseudo(g_nodes[cur].first);
        if (c) return c;
    }
    while (cur != root) {
        int nx = skip_pseudo(g_nodes[cur].next);
        if (nx) return nx;
        cur = g_nodes[cur].parent;
        if (!cur) return 0;
    }
    return 0;
}

/* textContent: every descendant text node in order, script and style bodies
   included. Returns the full length; out gets a NUL-terminated prefix. */
int rb_dom_text_content(int node, char *out, int cap) {
    if (out && cap > 0) out[0] = 0;
    if (!node_ok(node)) return 0;
    int total = 0;
    int cur = node;
    do {
        const Node *nd = &g_nodes[cur];
        const char *s = 0;
        int sn = 0;
        if (nd->kind == 1) { s = g_pool + nd->text_off; sn = (int)nd->text_len; }
        else if (nd->tag == RB_TAG_TEXTAREA && !nd->first && nd->text_len) { s = g_pool + nd->text_off; sn = (int)nd->text_len; }
        if (s && sn > 0) {
            if (out && total < cap - 1) {
                int room = cap - 1 - total;
                memcpy(out + total, s, (size_t)(sn < room ? sn : room));
            }
            total += sn;
        }
        cur = dfs_next(cur, node, nd->kind == 0);
    } while (cur);
    if (out && cap > 0) out[total < cap - 1 ? total : cap - 1] = 0;
    return total;
}

/* ---- selectors ----------------------------------------------------------------- */

#define QSEL_ALTS 32
static int g_qfirst[QSEL_ALTS];

typedef struct { int atoms, ients, istrs, oom, oom_line, budget; const char *oom_file; } QSave;

static void q_begin(QSave *q) {
    q->atoms = g_atom_n; q->ients = g_ient_n; q->istrs = g_istr_n;
    q->oom = g_oom; q->oom_line = g_oom_line; q->oom_file = g_oom_file;
    q->budget = g_match_budget;
}

/* Leave the cascade's tables as found: drop the atoms and the strings the
   selector interned (a dynamic [data-id="7"] would otherwise fill the table). */
static void q_end(const QSave *q) {
    g_atom_n = q->atoms;
    if (g_ient_n != q->ients) {
        g_ient_n = q->ients;
        g_istr_n = q->istrs;
        intern_rebuild_hash();
    }
    g_oom = q->oom; g_oom_line = q->oom_line; g_oom_file = q->oom_file;
    g_match_budget = q->budget;
}

/* Split at top-level commas and compile each part. 0 on success (*nf parts
   that can match), -1 on a syntax error. A part that is valid but can never
   match here (:hover, ::before) is simply left out. */
static int q_compile(const char *s, int n, int *nf) {
    *nf = 0;
    int i = 0;
    for (;;) {
        int e = i, d = 0;
        char q = 0;
        while (e < n) {
            char c = s[e];
            if (q) { if (c == '\\' && e + 1 < n) e++; else if (c == q) q = 0; }
            else if (c == '\\' && e + 1 < n) e++;
            else if (c == '"' || c == '\'') q = c;
            else if (c == '(' || c == '[') d++;
            else if (c == ')' || c == ']') { if (d > 0) d--; }
            else if (c == ',' && d == 0) break;
            e++;
        }
        const char *ps = s + i;
        int pn = e - i;
        trim_tok(&ps, &pn);
        if (pn <= 0) return -1;
        int spec = 0, pseudo = 0;
        int first = compile_selector(ps, pn, &spec, &pseudo);
        if (first < 0) {
            if (!g_sel_never) return -1;
        } else if (!pseudo) {
            if (*nf >= QSEL_ALTS) return -1;
            g_qfirst[(*nf)++] = first;
        }
        if (e >= n) break;
        i = e + 1;
    }
    return 0;
}

static int q_match(int node, int nf) {
    if (g_nodes[node].kind != 0) return 0;
    for (int k = 0; k < nf; k++) {
        g_match_budget = 4000;
        if (match_complex(node, g_qfirst[k])) return 1;
    }
    return 0;
}

int rb_dom_query(int root, const char *sel, int len, int *out, int max) {
    if (!node_ok(root) || !sel) return -1;
    if (len < 0) len = (int)strlen(sel);
    QSave sv;
    q_begin(&sv);
    int nf, total = 0;
    if (q_compile(sel, len, &nf) < 0) { q_end(&sv); return -1; }
    if (nf > 0 && g_nodes[root].kind == 0) {
        for (int cur = dfs_next(root, root, 1); cur; cur = dfs_next(cur, root, 1)) {
            if (!q_match(cur, nf)) continue;
            if (out && total < max) out[total] = cur;
            total++;
        }
    }
    q_end(&sv);
    return total;
}

int rb_dom_matches(int node, const char *sel, int len) {
    if (!node_ok(node) || !sel) return -1;
    if (len < 0) len = (int)strlen(sel);
    QSave sv;
    q_begin(&sv);
    int nf, r;
    if (q_compile(sel, len, &nf) < 0) r = -1;
    else r = nf > 0 && q_match(node, nf);
    q_end(&sv);
    return r;
}

int rb_dom_closest(int node, const char *sel, int len) {
    if (!node_ok(node) || !sel) return -1;
    if (len < 0) len = (int)strlen(sel);
    QSave sv;
    q_begin(&sv);
    int nf, r = 0;
    if (q_compile(sel, len, &nf) < 0) r = -1;
    else if (nf > 0)
        for (int p = node; p && !g_nodes[p].dead; p = g_nodes[p].parent)
            if (q_match(p, nf)) { r = p; break; }
    q_end(&sv);
    return r;
}

/* ---- markup ------------------------------------------------------------------- */

/* Parse html as the children of a fresh detached element shaped like ctx (same
   tag, so the builder's context rules apply). Returns the container, or 0
   when the tree or pool ran out; nothing live is touched either way. */
static int frag_parse(int ctx, const char *html, int len) {
    if (len < 0) len = html ? (int)strlen(html) : 0;
    if (len > 0 && !html) return 0;
    if (len > 0 && g_pool_n + 3 * len + 1024 >= POOL_MAX) { SET_OOM(); return 0; }
    int tmp = new_node();
    if (!tmp) return 0;
    g_nodes[tmp].tag = g_nodes[ctx].tag;
    g_nodes[tmp].name_hash = g_nodes[ctx].name_hash;
    g_nodes[tmp].tname = g_nodes[ctx].tname;
    int save_stop = g_stop;
    g_stop = 0;
    parse_fragment(html, len, tmp);
    int failed = g_stop;
    g_stop = save_stop;
    if (failed) { recycle_subtree(tmp); return 0; }
    return tmp;
}

int rb_dom_set_inner_html(int node, const char *html, int len) {
    if (!node_ok(node) || node == 1 || g_nodes[node].kind != 0) return 0;
    int tmp = frag_parse(node, html, len);
    if (!tmp) return 0;
    Node *n = &g_nodes[node];
    int ch = n->first;
    n->first = 0;
    while (ch) {
        int nx = g_nodes[ch].next;
        recycle_subtree(ch);
        ch = nx;
    }
    n->first = g_nodes[tmp].first;
    for (int c = n->first; c; c = g_nodes[c].next) g_nodes[c].parent = (uint16_t)node;
    g_nodes[tmp].first = 0;
    recycle_subtree(tmp);
    style_touch(node);
    return 1;
}

/* where: 0 beforebegin, 1 afterbegin, 2 beforeend, 3 afterend */
int rb_dom_insert_adjacent_html(int node, int where, const char *html, int len) {
    if (!node_ok(node) || node == 1 || g_nodes[node].kind != 0 || where < 0 || where > 3) return 0;
    int parent = where == 0 || where == 3 ? g_nodes[node].parent : node;
    if (!parent || g_nodes[parent].kind != 0) return 0;
    int tmp = frag_parse(parent, html, len);
    if (!tmp) return 0;
    int ref = 0;
    if (where == 0) ref = node;
    else if (where == 1) ref = skip_pseudo(g_nodes[node].first);
    else if (where == 3) ref = skip_pseudo(g_nodes[node].next);
    for (int c = g_nodes[tmp].first; c;) {
        int nx = g_nodes[c].next;
        rb_dom_insert_before(parent, c, ref);
        c = nx;
    }
    recycle_subtree(tmp);
    style_touch(parent);
    return 1;
}

typedef struct { char *out; int cap; int n; } Ser;

static void ser_ch(Ser *s, char c) {
    if (s->out && s->n + 1 < s->cap) s->out[s->n] = c;
    s->n++;
}
static void ser_mem(Ser *s, const char *p, int n) { for (int i = 0; i < n; i++) ser_ch(s, p[i]); }
static void ser_lit(Ser *s, const char *p) { while (*p) ser_ch(s, *p++); }

static void ser_esc(Ser *s, const char *p, int n, int attr) {
    for (int i = 0; i < n; i++) {
        char c = p[i];
        if (c == '&') ser_lit(s, "&amp;");
        else if (c == '<') ser_lit(s, "&lt;");
        else if (c == '>' && !attr) ser_lit(s, "&gt;");
        else if (c == '"' && attr) ser_lit(s, "&quot;");
        else if (c == NBSP_CH) ser_lit(s, "&nbsp;");
        else ser_ch(s, c);
    }
}

/* Start tag (or the whole text node). Returns 1 when the element can have content. */
static int ser_open(Ser *s, int cur) {
    const Node *nd = &g_nodes[cur];
    if (nd->kind == 1) {
        int ptag = nd->parent ? g_nodes[nd->parent].tag : 0;
        const char *t = g_pool + nd->text_off;
        if (ptag == RB_TAG_SCRIPT || ptag == RB_TAG_STYLE) ser_mem(s, t, (int)nd->text_len);
        else ser_esc(s, t, (int)nd->text_len, 0);
        return 0;
    }
    int nn;
    const char *name = tag_str(cur, &nn);
    ser_ch(s, '<');
    ser_mem(s, name, nn);
    AttrRef r;
    for (int i = 0; attr_ref(cur, i, &r); i++) {
        ser_ch(s, ' ');
        ser_mem(s, r.name, r.nn);
        ser_lit(s, "=\"");
        ser_esc(s, r.val, r.vn, 1);
        ser_ch(s, '"');
    }
    ser_ch(s, '>');
    if (tag_is_void(nd->tag)) return 0;
    if (nd->tag == RB_TAG_TEXTAREA && !nd->first && nd->text_len) ser_esc(s, g_pool + nd->text_off, (int)nd->text_len, 0);
    return 1;
}

static void ser_close(Ser *s, int cur) {
    const Node *nd = &g_nodes[cur];
    if (nd->kind != 0 || tag_is_void(nd->tag)) return;
    int nn;
    const char *name = tag_str(cur, &nn);
    ser_lit(s, "</");
    ser_mem(s, name, nn);
    ser_ch(s, '>');
}

/* Serialize node's children (outer=0) or node itself. Full length is returned
   as snprintf would; out holds a NUL-terminated prefix. <title> text is not in
   the tree (rb_title has it) and a source <script>'s body is kept by
   rb_script_get, so both serialize empty. */
int rb_dom_get_html(int node, int outer, char *out, int cap) {
    if (out && cap > 0) out[0] = 0;
    if (!node_ok(node)) return 0;
    if (node == 1) outer = 0;
    Ser s;
    s.out = out; s.cap = cap; s.n = 0;
    int cur = outer ? node : (g_nodes[node].kind == 0 ? skip_pseudo(g_nodes[node].first) : 0);
    while (cur) {
        int fc = ser_open(&s, cur) ? skip_pseudo(g_nodes[cur].first) : 0;
        if (fc) { cur = fc; continue; }
        for (;;) {
            ser_close(&s, cur);
            if (outer && cur == node) { cur = 0; break; }
            int nx = skip_pseudo(g_nodes[cur].next);
            if (nx) { cur = nx; break; }
            cur = g_nodes[cur].parent;
            if (!cur || (!outer && cur == node)) { cur = 0; break; }
        }
    }
    if (out && cap > 0) out[s.n < cap - 1 ? s.n : cap - 1] = 0;
    return s.n;
}

/* ---- detach ------------------------------------------------------------------- */

/* JS removeChild: unlink the subtree but keep every node alive (same
   generation) so it can be inserted again. 0 for the root, a bad handle or a
   node that has no parent. */
int rb_dom_detach(int node) {
    if (!node_ok(node) || node == 1 || !g_nodes[node].parent) return 0;
    detach(node);
    if (g_active && is_ancestor(node, g_active)) g_active = 0;
    for (int i = 0; i < g_focus_n; i++)
        if (is_ancestor(node, g_focus[i])) { focus_drop(g_focus[i]); i--; }
    return 1;
}

/* ---- form state ---------------------------------------------------------------- */

static int opt_value(int o, char *out, int cap) {
    const Node *nd = &g_nodes[o];
    if (nd->text_len) {
        if (out && cap > 0) copy_span(g_pool + nd->text_off, (int)nd->text_len, out, cap);
        return (int)nd->text_len;
    }
    return rb_dom_text_content(o, out, cap);
}

/* -1 when the node has no value (not a form control), else the full length. */
int rb_dom_get_value(int node, char *out, int cap) {
    if (out && cap > 0) out[0] = 0;
    if (!node_ok(node)) return -1;
    const Node *nd = &g_nodes[node];
    if (nd->kind != 0) return -1;
    if (nd->tag == RB_TAG_INPUT && !nd->text_len && (nd->ctl == RB_CTL_CHECKBOX || nd->ctl == RB_CTL_RADIO)) {
        if (out && cap > 0) put_lit(out, cap, "on");
        return 2;
    }
    switch (nd->tag) {
    case RB_TAG_INPUT: case RB_TAG_TEXTAREA: case RB_TAG_BUTTON:
        if (out && cap > 0) copy_span(g_pool + nd->text_off, (int)nd->text_len, out, cap);
        return (int)nd->text_len;
    case RB_TAG_OPTION:
        return opt_value(node, out, cap);
    case RB_TAG_SELECT: {
        int o = select_pick(node);
        return o ? opt_value(o, out, cap) : 0;
    }
    default:
        return -1;
    }
}

int rb_dom_set_value(int node, const char *s, int len) {
    if (!node_ok(node)) return 0;
    if (len < 0) len = s ? (int)strlen(s) : 0;
    if (len > 0 && !s) return 0;
    Node *nd = &g_nodes[node];
    if (nd->kind != 0) return 0;
    switch (nd->tag) {
    case RB_TAG_INPUT: case RB_TAG_TEXTAREA: case RB_TAG_BUTTON: case RB_TAG_OPTION:
        if (!store_str(&nd->text_off, &nd->text_len, s, len)) return 0;
        style_touch(node);
        return 1;
    case RB_TAG_SELECT: {
        /* select the first option whose value equals s; the others lose `selected` */
        int match = 0;
        static char vbuf[256];
        for (int c = nd->first; c; c = g_nodes[c].next) {
            int grp = g_nodes[c].kind == 0 && g_nodes[c].tag == RB_TAG_OPTGROUP;
            for (int o = grp ? g_nodes[c].first : c; o; o = grp ? g_nodes[o].next : 0) {
                if (g_nodes[o].kind != 0 || g_nodes[o].tag != RB_TAG_OPTION || g_nodes[o].dead) continue;
                /* opt_value returns the full length but copies what fits:
                   a value that did not fit cannot be compared (and is not
                   matched), or memcmp would run off the end of vbuf */
                int vn = opt_value(o, vbuf, (int)sizeof(vbuf));
                int hit = !match && vn == len && vn < (int)sizeof(vbuf) &&
                          (len == 0 || memcmp(vbuf, s, (size_t)len) == 0);
                if (hit) { match = o; blob_edit(o, "selected", 8, "", 0, 0); }
                else blob_edit(o, "selected", 8, 0, 0, 1);
                style_touch(o);
            }
        }
        return match != 0;
    }
    default:
        return 0;
    }
}

int rb_dom_get_checked(int node) {
    if (!node_ok(node)) return 0;
    const Node *nd = &g_nodes[node];
    return nd->kind == 0 && nd->tag == RB_TAG_INPUT && (nd->ctl == RB_CTL_CHECKBOX || nd->ctl == RB_CTL_RADIO) && nd->checked;
}

static void ev_radio_clear(int node);

int rb_dom_set_checked(int node, int on) {
    if (!node_ok(node)) return 0;
    Node *nd = &g_nodes[node];
    if (nd->kind != 0 || nd->tag != RB_TAG_INPUT || (nd->ctl != RB_CTL_CHECKBOX && nd->ctl != RB_CTL_RADIO)) return 0;
    nd->checked = on ? 1 : 0;
    if (on && nd->ctl == RB_CTL_RADIO) ev_radio_clear(node);
    style_touch(node);
    return 1;
}

/* The union of the painted boxes of `node` (descendants too when `deep`). */
static int in_subtree(int n, int root) {
    for (int d = 0; n && d < 4096; n = g_nodes[n].parent, d++) if (n == root) return 1;
    return 0;
}

static int box_union(int node, int deep, int *x, int *y, int *w, int *h) {
    int minx = 0, miny = 0, maxx = 0, maxy = 0, found = 0;
    for (int i = 0; i < g_box_n; i++) {
        RbBox *b = &g_box[i];
        if (b->w <= 0 || b->h <= 0) continue;
        if (deep ? !in_subtree(b->node, node) : b->node != node) continue;
        int bx0 = b->x, by0 = b->y, bx1 = b->x + b->w, by1 = b->y + b->h;
        if (!found) { minx = bx0; miny = by0; maxx = bx1; maxy = by1; found = 1; }
        else {
            if (bx0 < minx) minx = bx0;
            if (by0 < miny) miny = by0;
            if (bx1 > maxx) maxx = bx1;
            if (by1 > maxy) maxy = by1;
        }
    }
    if (!found) return 0;
    if (x) *x = minx;
    if (y) *y = miny;
    if (w) *w = maxx - minx;
    if (h) *h = maxy - miny;
    return 1;
}

/* The node's own painted boxes when it has any (they carry the final flex
   and table placement); else the border box layout recorded for it (a block
   with nothing painted of its own); else everything painted inside it (an
   inline element, a text node). */
int rb_dom_rect(int node, int *x, int *y, int *w, int *h) {
    if (!node_ok(node)) return 0;
    if (box_union(node, 0, x, y, w, h)) return 1;
    if (node < MAX_NODES && g_nbox_set[node]) {
        if (x) *x = g_nbox[node][0];
        if (y) *y = g_nbox[node][1];
        if (w) *w = g_nbox[node][2];
        if (h) *h = g_nbox[node][3];
        return 1;
    }
    return box_union(node, 1, x, y, w, h);
}

int rb_dom_offset_width(int node, int *w) {
    int x, y, ww, h;
    if (!rb_dom_rect(node, &x, &y, &ww, &h)) return 0;
    if (w) *w = ww;
    return 1;
}

int rb_dom_offset_height(int node, int *h) {
    int x, y, w, hh;
    if (!rb_dom_rect(node, &x, &y, &w, &hh)) return 0;
    if (h) *h = hh;
    return 1;
}

/* =====================================================================
   PART 2: event system + focus.

   Listeners live in one flat table in registration order; a node's handlers
   run in table order, per phase. Dispatch is fully iterative and
   recursion-free (the shell stack is ~16KB):

     - the ancestor path target -> root is built once into a per-dispatch
       static slot (a nested dispatch owns the next slot, so an outer walk
       is never clobbered), bounded by MAX_NODES, with dead-node and
       parent-walk guards;
     - capture runs over that path from the root down to the target itself,
       bubble runs from the target up to the root;
     - stop() finishes the current node's remaining handlers (its inline
       onclick included), stopImmediate() halts both, prevent() only
       cancels the default action, which runs after the bubble phase.

   Re-entrant dispatch is safe: every nesting level owns its own event-state
   record. Inline onclick is treated as a bubble handler at the target node,
   run through run_script exactly as rb_activate used to; its 1 marks a
   mutation, its 2 short-circuits dispatch (the script already queued the
   navigation). rb_activate keeps its historical Enter semantics for fields,
   while a public rb_dispatch click focuses text fields instead.
   ===================================================================== */

/* Native listener registrations per page: one per (node, type, phase) a
   script listens on. 256 was too few for a page with a few hundred
   clickable elements: the rest silently never saw a click. */
#define EV_LISTEN_MAX 4096
/* One live dispatch plus three nested re-entrant dispatches. The default
   action's FOCUS/BLUR dispatch consumes one level, so this still allows a
   click -> focus -> nested dispatch -> nested dispatch chain. Kept small on
   purpose: the per-depth path/match scratch below is multiplied by it and the
   shell has a 16KB stack and the kernel a tight BSS budget. */
#define EV_MAX_DEPTH 4

typedef struct {
    int node;          /* owning node handle */
    int type;          /* RB_EV_* */
    uint8_t capture;   /* 0 = bubble, 1 = capture */
    uint8_t dead;      /* tombstone: removed while a dispatch was walking */
    RbListener fn;
    void *user;
} EvListen;
static EvListen g_ev_listen[EV_LISTEN_MAX];
static int g_ev_listen_n;

typedef struct {
    int type, target, current;
    int prevented, stopped, stop_now, skip_default;
    /* Event payload. key is a character code for keyboard events (0
       otherwise); x/y are content-space coordinates for mouse events; button
       is 0=left, 1=middle, 2=right, -1=none; buttons is the raw PS/2 mask
       (bit0 left, bit1 right, bit2 middle); mods is RB_MOD_* for keyboard
       events. Each dispatch depth owns its own record, so a nested dispatch
       cannot clobber an outer event's payload. */
    int key, x, y, button;
    int buttons, mods;
} EvState;
static EvState g_ev_states[EV_MAX_DEPTH];
static int g_ev_depth;

static int g_active;                                    /* active element */
static int g_ev_dropped;   /* dispatches rejected by the depth cap since reset */
/* preventDefault of the most recently finished dispatch. rb_event_prevented()
   is only valid inside a listener, so a UI seam that owns a default action
   (typing a key, submitting) reads this after the dispatch returns. Set at the
   top of every dispatch (an early-rejected dispatch is not prevented) and
   again when the top-level dispatch finishes. */
static int g_ev_last_prevented;

/* Per-depth dispatch scratch. A nested dispatch owns the next row, so an
   outer walk is never clobbered. Path entries remember {slot, gen}: a slot
   recycled mid-dispatch (and possibly reused) is skipped rather than visited
   as a different node. Slots and listener indices are uint16_t. */
static uint16_t g_ev_path[EV_MAX_DEPTH][MAX_NODES];      /* slot ids */
static uint16_t g_ev_path_gen[EV_MAX_DEPTH][MAX_NODES];  /* generation at snapshot */
static uint16_t g_ev_match[EV_MAX_DEPTH][EV_LISTEN_MAX]; /* listener indices */
static uint16_t g_ev_walk[MAX_NODES];                    /* radio-group DFS scratch */

static void ev_reset(void) {
    g_ev_listen_n = 0;
    g_active = 0;
    g_ev_dropped = 0;
    g_submit_form = 0;
    g_submit_submitter = 0;
    g_ev_last_prevented = 0;
    /* A listener may call rb_open (which resets the document) while an outer
       dispatch is still walking. Zeroing g_ev_depth here would drive that
       outer dispatch's exit to -1, so the next dispatch would index
       g_ev_states[-1]; and wiping the state rows would drop the outer walk's
       stopped/prevented flags. Both are deferred until the dispatch stack is
       empty (the same deferred-compaction motion the listener table uses):
       each dispatch restores its own entry depth on exit (see dispatch_ev_ex),
       so a reset mid-walk self-heals when the outer dispatch unwinds, and the
       rows are only cleared when no dispatch owns them. */
    if (g_ev_depth == 0) memset(g_ev_states, 0, sizeof(g_ev_states));
}

/* Drop tombstones. Only safe while no dispatch is walking indices; the
   mid-dispatch paths mark entries dead and defer compaction until idle. */
static void ev_compact(void) {
    int w = 0;
    for (int i = 0; i < g_ev_listen_n; i++)
        if (!g_ev_listen[i].dead) g_ev_listen[w++] = g_ev_listen[i];
    g_ev_listen_n = w;
}

static int ev_live_count(void) {
    int n = 0;
    for (int i = 0; i < g_ev_listen_n; i++) if (!g_ev_listen[i].dead) n++;
    return n;
}

/* Dead nodes keep no listeners: called from free_node on every recycle.
   During a dispatch the entries are tombstoned instead of removed so the
   indices the current walk snapshotted stay valid; the walk still runs the
   handlers that were registered when it reached this node (DOM-like: a node
   removal by an earlier handler does not drop that node's remaining
   handlers). Idle compaction reclaims the slots. */
static void ev_free_node(int node) {
    int removed = 0;
    for (int i = 0; i < g_ev_listen_n; i++) {
        if (g_ev_listen[i].node == node && !g_ev_listen[i].dead) {
            g_ev_listen[i].dead = 1;
            removed++;
        }
    }
    (void)removed;
    if (g_ev_depth == 0) ev_compact();
    /* A dead slot must never stay the active element: the slot may be reused
       by a different node before the next focus change. */
    if (g_active == node) g_active = 0;
}

static int ev_listen_add(int node, int type, int capture, RbListener fn, void *user) {
    if (!node_ok(node) || type < 0 || type >= RB_EV_COUNT || !fn) return 0;
    if (g_ev_depth == 0) ev_compact();
    /* A full table is a registration failure, not page truncation: g_oom is
       reserved for the parse/pool limits and drives the "engine memory full"
       banner, which over-registering a framework must not trigger.
       Mid-dispatch starvation: while g_ev_depth > 0, removals leave tombstones
       that still occupy slots, so if live + tombstones >= EV_LISTEN_MAX a
       fresh registration transiently returns 0 here. Every table call (this
       function, both removes and rb_listener_count) compacts once the dispatch
       ends, so the capacity recovers on the next idle call. */
    if (g_ev_listen_n >= EV_LISTEN_MAX) return 0;
    EvListen *L = &g_ev_listen[g_ev_listen_n++];
    L->node = node;
    L->type = type;
    L->capture = (uint8_t)(capture ? 1 : 0);
    L->dead = 0;
    L->fn = fn;
    L->user = user;
    return 1;
}

int rb_listen(int node, int type, RbListener fn, void *user) {
    return ev_listen_add(node, type, 0, fn, user);
}

int rb_listen_capture(int node, int type, RbListener fn, void *user) {
    return ev_listen_add(node, type, 1, fn, user);
}

/* Returns the number of listeners removed. */
int rb_listen_remove(int node, int type) {
    int removed = 0;
    for (int i = 0; i < g_ev_listen_n; i++) {
        if (g_ev_listen[i].node == node && g_ev_listen[i].type == type &&
            !g_ev_listen[i].dead) {
            g_ev_listen[i].dead = 1;
            removed++;
        }
    }
    if (g_ev_depth == 0) ev_compact();
    return removed;
}

/* Returns the number of listeners removed. */
int rb_listen_remove_all(int node) {
    int removed = 0;
    for (int i = 0; i < g_ev_listen_n; i++) {
        if (g_ev_listen[i].node == node && !g_ev_listen[i].dead) {
            g_ev_listen[i].dead = 1;
            removed++;
        }
    }
    if (g_ev_depth == 0) ev_compact();
    return removed;
}

int rb_listener_count(void) {
    if (g_ev_depth == 0) ev_compact();
    return ev_live_count();
}

/* --- current-dispatch state (valid only inside a listener) --- */

int rb_event_type(void) { return g_ev_depth > 0 ? g_ev_states[g_ev_depth - 1].type : 0; }
int rb_event_target(void) { return g_ev_depth > 0 ? g_ev_states[g_ev_depth - 1].target : 0; }
int rb_event_current(void) { return g_ev_depth > 0 ? g_ev_states[g_ev_depth - 1].current : 0; }
int rb_event_prevented(void) { return g_ev_depth > 0 ? g_ev_states[g_ev_depth - 1].prevented : 0; }
int rb_event_depth(void) { return g_ev_depth; }
int rb_event_dropped(void) { return g_ev_dropped; }

/* Payload getters. Out of a dispatch they report the empty payload: key 0,
   coordinates 0, button -1 (none), buttons 0, mods 0, matching
   rb_dispatch's defaults. */
int rb_event_key(void) { return g_ev_depth > 0 ? g_ev_states[g_ev_depth - 1].key : 0; }
int rb_event_x(void) { return g_ev_depth > 0 ? g_ev_states[g_ev_depth - 1].x : 0; }
int rb_event_y(void) { return g_ev_depth > 0 ? g_ev_states[g_ev_depth - 1].y : 0; }
int rb_event_button(void) { return g_ev_depth > 0 ? g_ev_states[g_ev_depth - 1].button : -1; }
int rb_event_buttons(void) { return g_ev_depth > 0 ? g_ev_states[g_ev_depth - 1].buttons : 0; }
int rb_event_mods(void) { return g_ev_depth > 0 ? g_ev_states[g_ev_depth - 1].mods : 0; }

int rb_last_dispatch_prevented(void) { return g_ev_last_prevented; }

void rb_event_prevent(void) {
    if (g_ev_depth > 0) g_ev_states[g_ev_depth - 1].prevented = 1;
}

void rb_event_stop(void) {
    if (g_ev_depth > 0) g_ev_states[g_ev_depth - 1].stopped = 1;
}

void rb_event_stop_immediate(void) {
    if (g_ev_depth > 0) {
        g_ev_states[g_ev_depth - 1].stop_now = 1;
        g_ev_states[g_ev_depth - 1].stopped = 1;
    }
}

/* --- focus --- */

int rb_dom_is_focusable(int node) {
    if (!node_ok(node) || g_nodes[node].kind == 1) return 0;
    if (!is_focusable(node)) return 0;
    /* Match the focus list: a hidden or display:none ancestor hides it. */
    return focus_reachable(node);
}

/* Set the active element, dispatching BLUR on the old and FOCUS on the new.
   The two dispatches are nested inside a click dispatch's default action, so
   they run one level deeper and cannot disturb the outer event state. */
static int ev_focus(int node) {
    if (!rb_dom_is_focusable(node)) return 0;
    if (g_active == node) return 1;
    int old = g_active;
    if (old > 0 && old < g_node_n && !g_nodes[old].dead) dispatch_ev(old, RB_EV_BLUR, 0, 0);
    g_active = node;
    dispatch_ev(node, RB_EV_FOCUS, 0, 0);
    return 1;
}

int rb_dom_focus(int node) { return ev_focus(node); }

int rb_dom_blur(void) {
    int old = g_active;
    if (!old || old >= g_node_n || g_nodes[old].dead) {
        g_active = 0;
        return 0;
    }
    dispatch_ev(old, RB_EV_BLUR, 0, 0);
    /* A blur listener may have focused something else; only clear then. */
    if (g_active == old) g_active = 0;
    return 1;
}

int rb_dom_active_element(void) {
    if (g_active > 0 && g_active < g_node_n && !g_nodes[g_active].dead) return g_active;
    return 0;
}

/* Generation of a node slot: bumped on every recycle, so a consumer that
   holds a node handle across a DOM mutation can tell "still the same element"
   from "the slot was freed and reused by some other node". 0 for an
   invalid/dead node. */
int rb_dom_generation(int node) {
    if (node <= 0 || node >= g_node_n || g_nodes[node].dead) return 0;
    return g_nodes[node].gen;
}

/* Relayout hook: an active element that fell out of the focus list (hidden,
   display:none, removed) stops being active. If the list overflowed, an
   absent node may simply not have fit MAX_FOCUS, so keep it when it is still
   focusable instead of evicting it (and silently not firing BLUR). */
static void ev_prune_active(void) {
    if (g_active <= 0) return;
    if (g_active >= g_node_n || g_nodes[g_active].dead) { g_active = 0; return; }
    for (int i = 0; i < g_focus_n; i++)
        if (g_focus[i] == g_active) return;
    if (g_focus_overflow && rb_dom_is_focusable(g_active)) return;
    g_active = 0;
}

/* --- dispatch internals --- */

/* Run the node's listeners for one phase, in registration order. The table
   may be mutated by an earlier listener (register/remove/free), so the
   matching indices are snapshotted first. Removals during a dispatch are
   tombstones (indices do not shift), and the snapshot deliberately ignores
   the tombstone flag when re-validating: a handler that was registered when
   this node was reached still runs even if an earlier handler just removed
   the node. Listeners added mid-walk are appended and simply absent from the
   snapshot. stopImmediate() halts the remaining handlers of this node. */
static void ev_visit(int node, EvState *st, int phase, int *result) {
    if (g_nodes[node].dead) return;
    st->current = node;
    /* The snapshot lives in this dispatch level's own slot: a listener that
       re-enters rb_dispatch uses the next depth's slot and cannot clobber the
       indices this walk has not read yet. */
    uint16_t *match = g_ev_match[(int)(st - g_ev_states)];
    int m = 0;
    for (int i = 0; i < g_ev_listen_n; i++) {
        EvListen *L = &g_ev_listen[i];
        if (!L->dead && L->node == node && L->type == st->type &&
            L->capture == (uint8_t)phase && m < EV_LISTEN_MAX)
            match[m++] = (uint16_t)i;
    }
    for (int k = 0; k < m; k++) {
        int i = match[k];
        if (i >= g_ev_listen_n) continue;
        EvListen *L = &g_ev_listen[i];
        if (L->node != node || L->type != st->type || L->capture != (uint8_t)phase) continue;
        if (st->stop_now) break;
        RbListener fn = L->fn;
        void *user = L->user;
        fn(st->type, st->target, node, user);
    }
    /* Inline onclick is a bubble handler at its own node on a click event:
       same script, same order as the old rb_activate (before <a href> nav and
       before any default action). The script's 2 short-circuits: navigation
       is already queued, so skip the default action and all further
       propagation; 1 marks a mutation and dispatch continues. */
    if (!st->stop_now && !phase && st->type == RB_EV_CLICK && g_nodes[node].click_len && !g_nodes[node].dead &&
        !g_scripting) {       /* with scripting on, the host's JS engine owns onclick */
        int r = run_script(g_pool + g_nodes[node].click_off, g_nodes[node].click_len);
        if (r == 2) {
            if (*result < 2) *result = 2;
            st->skip_default = 1;
            st->stopped = 1;
            st->stop_now = 1;
        } else if (r == 1 && *result == 0) {
            *result = 1;
        }
    }
}

/* Clear every same-named radio in the enclosing form but this one. Iterative
   DFS (no recursion inside dispatch): each subtree node is stacked exactly
   once, so the MAX_NODES-sized scratch never overflows. Nested forms own
   their own radio groups and are not descended into. */
static void ev_radio_clear(int node) {
    int form = form_of(node);
    if (!form) return;
    int nl = g_nodes[node].name_len;
    if (nl <= 0) return;
    const char *nm = g_pool + g_nodes[node].name_off;
    int top = 0;
    for (int c = g_nodes[form].first; c && top < MAX_NODES; c = g_nodes[c].next)
        g_ev_walk[top++] = c;
    while (top > 0) {
        int nd = g_ev_walk[--top];
        if (g_nodes[nd].dead) continue;
        if (nd != node && g_nodes[nd].tag == RB_TAG_INPUT &&
            g_nodes[nd].ctl == RB_CTL_RADIO && (int)g_nodes[nd].name_len == nl &&
            memcmp(g_pool + g_nodes[nd].name_off, nm, (size_t)nl) == 0) {
            g_nodes[nd].checked = 0;
            style_touch(nd);
        }
        /* Skip sibling subtrees hanging off a nested <form>, but never skip
           the walk over the owning form's other children. */
        if (g_nodes[nd].tag == RB_TAG_FORM && nd != form) continue;
        for (int c = g_nodes[nd].first; c && top < MAX_NODES; c = g_nodes[c].next)
            g_ev_walk[top++] = c;
    }
}

/* The default action: runs after the bubble phase, only when not prevented
   and not short-circuited. carried is the result accumulated so far (from
   the inline script); the return value is the higher of the two, using the
   rb_activate contract (2 nav > 1 mutation > 0).

   enter selects the historical Enter path: a field (text input or textarea)
   or a button-like control submits its nearest GET form, a post form stays
   a silent no-op (rb_form_post explains it), button/reset never submit.
   A plain public click instead focuses fields; submit/image inputs and
   bare <button>s submit. Checkbox clicks toggle, radios pick one of their
   same-name group (new behaviour: these used to be no-ops). */
static int ev_default_action(int target, int type, int enter, int carried) {
    /* A listener may have removed the target during the bubble phase; its
       fields (href, ctl, ...) are stale, so act on nothing. */
    if (target <= 0 || target >= g_node_n || g_nodes[target].dead) return carried;
    Node *n = &g_nodes[target];
    int r = carried;
    if (type == RB_EV_SUBMIT) {
        if (n->tag != RB_TAG_FORM || n->method_post) return r;
        /* submit_form pre-set the pending form+submitter, so the URL is built
           here (after the listeners ran) rather than inside submit_form; that
           keeps a single build per submission. A direct rb_dispatch(fm,
           SUBMIT) has no pending entry and builds with submitter 0. */
        int sub = 0;
        if (target == g_submit_form) {
            sub = g_submit_submitter;
            g_submit_form = 0;
        }
        if (submit_build(target, sub) && r < 2) r = 2;
        return r;
    }
    if (type != RB_EV_CLICK) return r;

    if (n->tag == RB_TAG_A && n->href_len) {
        set_nav(g_pool + n->href_off, n->href_len);
        if (r < 2) r = 2;
        return r;
    }

    /* A pointer click toggles a checkbox/radio; Enter does not (historic
       rb_activate semantics). Enter must also not let them fall through to the
       submit branch below, or a checkbox would submit its form. */
    if (!enter && n->ctl == RB_CTL_CHECKBOX) {
        n->checked = (uint8_t)!n->checked;
        style_touch(target);
        if (r < 1) r = 1;
        return r;
    }
    if (!enter && n->ctl == RB_CTL_RADIO) {
        n->checked = 1;
        ev_radio_clear(target);
        style_touch(target);
        if (r < 1) r = 1;
        return r;
    }

    int ctl = n->ctl;
    if (enter) {
        if (n->tag == RB_TAG_BUTTON || n->tag == RB_TAG_TEXTAREA ||
            (n->tag == RB_TAG_INPUT && !n->hidden)) {
            if (ctl == RB_CTL_BUTTON || ctl == RB_CTL_RESET ||
                ctl == RB_CTL_CHECKBOX || ctl == RB_CTL_RADIO) return r;
            int form = form_of(target);
            if (form && !g_nodes[form].method_post &&
                submit_form(form, target) && r < 2) r = 2;
            return r;
        }
        return r;
    }

    if (rb_is_field(target)) {
        if (ev_focus(target) && r < 1) r = 1;
        return r;
    }
    int submit_click = 0;
    if (n->tag == RB_TAG_INPUT && !n->hidden &&
        (ctl == RB_CTL_SUBMIT || ctl == RB_CTL_IMAGE)) submit_click = 1;
    else if (n->tag == RB_TAG_BUTTON && ctl != RB_CTL_BUTTON && ctl != RB_CTL_RESET)
        submit_click = 1;   /* bare <button> defaults to type=submit */
    if (submit_click) {
        int form = form_of(target);
        if (form && !g_nodes[form].method_post &&
            submit_form(form, target) && r < 2) r = 2;
    }
    return r;
}

/* Dispatch target -> root. Returns 0 (no effect), 1 (document mutated),
   2 (navigation queued); 0 for node 0, a dead node or a bad type. */
static int dispatch_ev_ex(int target, int type, int enter, int key, int x, int y,
                          int button, int buttons, int mods, int *prevented_out) {
    g_ev_last_prevented = 0;
    if (type < 0 || type >= RB_EV_COUNT) return 0;
    if (target <= 0 || target >= g_node_n || g_nodes[target].dead) return 0;
    /* A disabled control fires nothing: no listeners, no default action
       (same policy the old rb_activate had). disabled is meaningless on
       other tags, e.g. <a>. */
    if (g_nodes[target].disabled &&
        (g_nodes[target].tag == RB_TAG_INPUT || g_nodes[target].tag == RB_TAG_BUTTON ||
         g_nodes[target].tag == RB_TAG_TEXTAREA)) return 0;
    /* Re-entrancy: each nesting level owns its own state record and its own
       ancestor-path scratch, so a nested full dispatch inside a listener
       never corrupts the outer one. */
    if (g_ev_depth >= EV_MAX_DEPTH) {
        /* A dropped nested dispatch is indistinguishable from "no listeners";
           the counter makes the loss observable (rb_event_dropped). */
        g_ev_dropped++;
        return 0;
    }

    int nav0 = g_nav_len;
    int d = g_ev_depth;
    /* Entry depth. Restoring it (rather than decrementing) makes the counter
       self-healing if a listener calls rb_open mid-dispatch: ev_reset leaves
       the depth alone while g_ev_depth > 0, and this dispatch still puts it
       back exactly where it found it. Identical to "--" in the normal case. */
    int d_in = d;
    EvState *st = &g_ev_states[d];
    uint16_t *path = g_ev_path[d];
    uint16_t *path_gen = g_ev_path_gen[d];
    int target_gen = g_nodes[target].gen;
    int result = 0;
    st->type = type;
    st->target = target;
    st->current = 0;
    st->prevented = 0;
    st->stopped = 0;
    st->stop_now = 0;
    st->skip_default = 0;
    st->key = key;
    st->x = x;
    st->y = y;
    st->button = button;
    st->buttons = buttons;
    st->mods = mods;

    /* Build the ancestor path target -> root iteratively. The parent walk is
       bounded by MAX_NODES even if a parent cycle ever appeared (append
       guards already reject cycle-creating appends), and a dead entry stops
       the walk. */
    int n = 0;
    for (int p = target; p > 0 && p < g_node_n && !g_nodes[p].dead && n < MAX_NODES;
         p = g_nodes[p].parent) {
        path[n] = (uint16_t)p;
        path_gen[n] = g_nodes[p].gen;
        n++;
    }

    g_ev_depth++;
    /* Capture: root down to and including the target, so a capture listener
       on the target itself runs (before the target's bubble listeners).
       Each entry is validated by {slot, gen} before visiting: a listener that
       frees an ancestor (or recycles and reuses any slot on the path) makes
       that entry stale, and it is skipped instead of delivering the event to
       an unrelated node. */
    for (int i = n - 1; i >= 0 && !st->stopped; i--) {
        int s = path[i];
        if (g_nodes[s].dead || g_nodes[s].gen != path_gen[i]) continue;
        ev_visit(s, st, 1, &result);
    }
    /* Bubble: target up to the root; the inline onclick runs at the target. */
    for (int i = 0; i < n && !st->stopped; i++) {
        int s = path[i];
        if (g_nodes[s].dead || g_nodes[s].gen != path_gen[i]) continue;
        ev_visit(s, st, 0, &result);
    }
    /* Default action while still "inside" the dispatch, so focus changes
       nest their FOCUS/BLUR events instead of reusing this state slot. A dead
       or recycled target (removed and its slot reused by a listener) is
       skipped: acting on the new occupant would be a foreign side effect. */
    if (!st->prevented && !st->skip_default &&
        target > 0 && target < g_node_n && !g_nodes[target].dead &&
        g_nodes[target].gen == target_gen)
        result = ev_default_action(target, type, enter, result);
    g_ev_depth = d_in;

    /* Anything that queued a navigation during the dispatch (the script, the
       default action or a listener's nested dispatch) reports as 2. */
    if (result < 2 && g_nav_len > nav0) result = 2;
    if (prevented_out) *prevented_out = st->prevented;
    g_ev_last_prevented = st->prevented;
    return result;
}

/* Empty-payload entry used by the internal callers (activate, focus, input,
   blur...). Same contract as rb_dispatch_ex with the defaults. */
static int dispatch_ev(int target, int type, int enter, int *prevented_out) {
    return dispatch_ev_ex(target, type, enter, 0, 0, 0, -1, 0, 0, prevented_out);
}

int rb_dispatch_ex(int target, int type, int key, int x, int y, int button,
                   int buttons, int mods) {
    return dispatch_ev_ex(target, type, 0, key, x, y, button, buttons, mods, 0);
}

int rb_dispatch(int target, int type) {
    return rb_dispatch_ex(target, type, 0, 0, 0, -1, 0, 0);
}


/* After the host resumed from a CPU fault or a watchdog hang the engine may
   have been stopped anywhere: on the layout stack, mid-dispatch, inside a
   measuring pass. Clear the flags that outlive a call so the next rb_open
   (which resets the document itself) starts clean. */
void rb_crash_reset(void) {
#ifndef RBROWSE_HOST
    g_on_lay_stack = 0;
#endif
    g_ev_depth = 0;
    memset(g_ev_states, 0, sizeof(g_ev_states));
    g_probe_depth = 0;
    g_fit = 0;
    g_in_layer = 0;
    g_stop = 0;
}
