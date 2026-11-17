#ifndef RBROWSE_ENGINE_H
#define RBROWSE_ENGINE_H

#include <stdint.h>

/* The page viewport in screen px, set by the host with rb_set_viewport()
   before rb_open*. RB_VIEW_W is the content width left of the scrollbar;
   RB_VIEW_H is the content height, used as the height of the initial
   containing block (PART C1): a fixed box is anchored to it, and an absolute
   box with no positioned ancestor resolves top/bottom percentages against it.
   The defaults (304 x 200) are the original 320x200 mode-13h page, which the
   host test suites are written against. */
#define RB_VIEW_W (rb_view_width())
#define RB_VIEW_H (rb_view_height())
#define RB_VIEW_W_DEFAULT 304
#define RB_VIEW_H_DEFAULT 200
#define RB_VIEW_W_MAX 1024   /* rb_set_viewport clamps to this (the UI stops at 640) */
/* Length of the tab-stop list. */
#define RB_MAX_FOCUS 512

enum {
    RB_TAG_DOC = 0,
    RB_TAG_HTML, RB_TAG_HEAD, RB_TAG_BODY, RB_TAG_TITLE,
    RB_TAG_STYLE, RB_TAG_SCRIPT,
    RB_TAG_DIV, RB_TAG_P, RB_TAG_SPAN, RB_TAG_A,
    RB_TAG_H1, RB_TAG_H2, RB_TAG_H3, RB_TAG_H4, RB_TAG_H5, RB_TAG_H6,
    RB_TAG_UL, RB_TAG_OL, RB_TAG_LI,
    RB_TAG_BR, RB_TAG_HR, RB_TAG_IMG,
    RB_TAG_BUTTON, RB_TAG_INPUT, RB_TAG_TEXTAREA, RB_TAG_FORM,
    RB_TAG_B, RB_TAG_STRONG, RB_TAG_I, RB_TAG_EM, RB_TAG_U,
    RB_TAG_PRE, RB_TAG_CODE,
    RB_TAG_NAV, RB_TAG_HEADER, RB_TAG_FOOTER, RB_TAG_MAIN,
    RB_TAG_SECTION, RB_TAG_ARTICLE,
    RB_TAG_TABLE, RB_TAG_TR, RB_TAG_TD, RB_TAG_TH,
    RB_TAG_CENTER,
    RB_TAG_DL, RB_TAG_DT, RB_TAG_DD, RB_TAG_BLOCKQUOTE, RB_TAG_FIGURE, RB_TAG_FIGCAPTION,
    RB_TAG_ASIDE, RB_TAG_SMALL, RB_TAG_SUB, RB_TAG_SUP, RB_TAG_LABEL, RB_TAG_SELECT,
    RB_TAG_OPTION, RB_TAG_OPTGROUP, RB_TAG_FIELDSET, RB_TAG_LEGEND, RB_TAG_THEAD, RB_TAG_TBODY,
    RB_TAG_TFOOT, RB_TAG_CAPTION, RB_TAG_COLGROUP, RB_TAG_COL, RB_TAG_ADDRESS, RB_TAG_DETAILS,
    RB_TAG_SUMMARY, RB_TAG_ABBR, RB_TAG_CITE, RB_TAG_Q, RB_TAG_S, RB_TAG_STRIKE, RB_TAG_DEL,
    RB_TAG_INS, RB_TAG_MARK, RB_TAG_KBD, RB_TAG_SAMP, RB_TAG_VAR, RB_TAG_TT, RB_TAG_BIG,
    RB_TAG_FONT, RB_TAG_NOBR, RB_TAG_WBR, RB_TAG_IFRAME, RB_TAG_OBJECT, RB_TAG_EMBED,
    RB_TAG_VIDEO, RB_TAG_AUDIO, RB_TAG_CANVAS, RB_TAG_SVG, RB_TAG_NOSCRIPT, RB_TAG_TEMPLATE,
    RB_TAG_META, RB_TAG_LINK, RB_TAG_BASE, RB_TAG_PICTURE, RB_TAG_SOURCE, RB_TAG_MENU,
    RB_TAG_DIR, RB_TAG_HGROUP, RB_TAG_DIALOG, RB_TAG_DFN, RB_TAG_TIME, RB_TAG_DATA, RB_TAG_BDI,
    RB_TAG_BDO, RB_TAG_RUBY, RB_TAG_RT, RB_TAG_RP, RB_TAG_MAP, RB_TAG_AREA, RB_TAG_PARAM,
    RB_TAG_TRACK, RB_TAG_OUTPUT, RB_TAG_PROGRESS, RB_TAG_METER, RB_TAG_DATALIST, RB_TAG_SEARCH,
    RB_TAG_XMP, RB_TAG_LISTING, RB_TAG_PLAINTEXT, RB_TAG_FRAME, RB_TAG_FRAMESET, RB_TAG_NOFRAMES,
    RB_TAG_APPLET, RB_TAG_ACRONYM, RB_TAG_MARQUEE, RB_TAG_BLINK, RB_TAG_SPACER, RB_TAG_MATH,
    RB_TAG_OTHER
};

enum {
    RB_BOX_TEXT = 0,
    RB_BOX_BG = 1,
    RB_BOX_CONTROL = 2,
    RB_BOX_RULE = 3,
    RB_BOX_BORDER = 4,     /* a rounded border ring: bg = colour, scale = width, radius */
    RB_BOX_IMAGE = 5,      /* an <img>'s content box: toff = the <img> node, bg = placeholder fill */
    RB_BOX_CANVAS = 6      /* a <canvas>'s content box: toff = the <canvas> node, bg = its background (RB_BOX_IMGBG) */
};

/* Per-box paint flags (PART C1). RbBox.flags carries them; the painter in
   paint.c reads RB_BOX_FIXED to skip the scroll offset, and the engine sets
   RB_BOX_POS on every box that belongs to a positioned element so the final
   pass can float it above the flow and z-order it. */
#define RB_BOX_FIXED 1u   /* position:fixed: paint at the viewport, not the scroll origin */
#define RB_BOX_POS   2u   /* box belongs to a positioned (relative/absolute/fixed) element */
#define RB_BOX_BOLD  4u   /* font-weight: bold (painted as a 1px smear) */
#define RB_BOX_UNDER 8u   /* text-decoration: underline */
#define RB_BOX_STRIKE 16u /* text-decoration: line-through */
#define RB_BOX_REPL  64u  /* engine-internal: placeholder box of a replaced element (counts toward its container's intrinsic width) */
#define RB_BOX_OOF   32u  /* engine-internal: box of an absolute/fixed element (not part of its container's intrinsic width) */
#define RB_BOX_IMGBG 128u /* RB_BOX_IMAGE / RB_BOX_CANVAS: bg is the element's own background (painted under the pixels), not just the loading fill */

/* Colours are 24-bit RGB packed as 0x00RRGGBB in both profiles. Values above
   0xFFFFFF are engine-internal sentinels and never reach a box. */
typedef uint32_t RbColor;
#define RB_RGB(r, g, b) ((RbColor)(((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))
#define RB_R(c) ((int)(((c) >> 16) & 255))
#define RB_G(c) ((int)(((c) >> 8) & 255))
#define RB_B(c) ((int)((c) & 255))

typedef struct {
    int16_t x, w;         /* horizontal: the page is at most RB_VIEW_W wide, so int16 is ample */
    int32_t y, h;         /* vertical: a real page can be tens of thousands of px tall */
    uint16_t node;
    uint32_t toff, tlen;
    RbColor color, bg;    /* 0xRRGGBB: text/line colour, fill colour */
    uint8_t scale, kind;
    uint8_t flags;        /* RB_BOX_* paint flags */
    uint8_t opacity;      /* 0..255, 255 = opaque (alpha-blended by the painter) */
    int16_t z;            /* z-index of the owning positioned element (0 for auto) */
    uint8_t clip;         /* 1-based index into the engine's clip table, 0 = unclipped */
    uint8_t ls;           /* letter-spacing in px (text boxes) */
    uint8_t radius;       /* border-radius in px (background / border boxes) */
} RbBox;

/* rb_map_rgb packs a colour (channels clamped to 0..255) into an RbColor.
   It is exact in both profiles; the name survives from the palette days.
   For an 8-bit display the host quantizes at present time: rb_rgb_index is
   the nearest entry of the 6x6x6 cube at palette indices 16..231 or the gray
   ramp at 232..255 (0, 10, .. 230), and rb_cube_rgb gives the RGB of such an
   index. Indices 0..15 stay free for chrome. */
int rb_map_rgb(int r, int g, int b);
int rb_rgb_index(RbColor c);
void rb_cube_rgb(int index, int *r, int *g, int *b);

/* Page viewport in screen px (content width, content height); clamped to
   64..RB_VIEW_W_MAX x 32..4096. Layout picks it up at the next rb_open* or
   rb_relayout; @media rules are matched when a sheet is parsed, so they see
   it from the next rb_open*. @media width is w + 16 (the scrollbar) and
   height is 2 * h (the web profile's CSS px: vertical lengths are halved). */
void rb_set_viewport(int w, int h);
int rb_view_width(void);
int rb_view_height(void);

int rb_open(const char *html, int len);
/* Same, with the web profile: white page, standard UA stylesheet, vertical
   CSS px at half scale, the media viewport from rb_set_viewport. rb_open
   keeps the native look. */
int rb_open_web(const char *html, int len);
/* Copy markup into dst, dropping script and style elements so a short
   buffer keeps the visible body instead of a truncated script. */
int rb_compact_markup(const char *src, int n, char *dst, int cap);
void rb_relayout(void);
int rb_page_height(void);
int rb_page_bg(void);
int rb_title(char *out, int cap);

/* First element in the document with that id (0 for ""); a node detached by a script is not found. */
int rb_find_id(const char *id);
int rb_node_tag(int node);
int rb_node_color(int node);
int rb_node_href(int node, char *out, int cap);
int rb_collect_text(int node, char *out, int cap);

int rb_box_count(void);
const RbBox *rb_box(int i);
const char *rb_box_text(int i, int *len);
int rb_first_box(int node, int *x, int *y, int *w, int *h);
/* PART C1: the clip rectangle the painter must honour for box i, in document
   space (same origin as RbBox.x/y). Returns 1 and fills the rect when the box
   is clipped by an ancestor's overflow:hidden/clip/auto/scroll; 0 when not.
   A clipped box's own x/y/w/h are left untouched (geometry is preserved for
   hit testing and rb_dom_rect); only the painter narrows to this rect. */
int rb_box_clip(int i, int *x, int *y, int *w, int *h);

/* --- PART C1: computed style probes ---
   Values are valid after rb_relayout() for a live element node; a text node,
   a dead handle or a node that layout never reached reports the initial value
   (RB_POS_STATIC / RB_OVF_VISIBLE / RB_VIS_VISIBLE / RB_CURSOR_AUTO / 255).

   position: static (default), relative, absolute, fixed, sticky. sticky is
   implemented as relative (see engine.c handoff); the value is still reported
   as RB_POS_STICKY so the simplification is observable rather than silent.
   overflow: visible/hidden/clip/auto/scroll. hidden and clip actually clip
   in the painter; auto and scroll ALSO behave as hidden — this engine has no
   per-element scrollbars, so a scrolling box shows its overflow clipped at the
   padding box with no way to scroll it (documented approximation).
   visibility: visible/hidden. hidden occupies its space but emits no box.
   opacity: 0..255 (CSS 0..1 scaled). 0 and values below ~32 paint nothing;
   everything else is alpha-blended box by box by the painter (overlapping
   boxes of one element blend separately, not as a group). See engine.c.
   cursor: RB_CURSOR_*; stored only, for the UI to consult (no pointer sprite). */
enum { RB_POS_STATIC = 0, RB_POS_RELATIVE, RB_POS_ABSOLUTE, RB_POS_FIXED, RB_POS_STICKY };
enum { RB_OVF_VISIBLE = 0, RB_OVF_HIDDEN, RB_OVF_CLIP, RB_OVF_AUTO, RB_OVF_SCROLL };
enum { RB_VIS_VISIBLE = 0, RB_VIS_HIDDEN };
enum {
    RB_CURSOR_AUTO = 0, RB_CURSOR_DEFAULT, RB_CURSOR_POINTER, RB_CURSOR_TEXT,
    RB_CURSOR_MOVE, RB_CURSOR_WAIT, RB_CURSOR_HELP, RB_CURSOR_CROSSHAIR,
    RB_CURSOR_NOT_ALLOWED, RB_CURSOR_GRAB
};

int rb_style_position(int node);      /* RB_POS_* */
int rb_style_overflow(int node);      /* RB_OVF_* */
int rb_style_visibility(int node);    /* RB_VIS_* */
int rb_style_cursor(int node);        /* RB_CURSOR_* */
int rb_style_opacity(int node);       /* 0..255 */
/* z-index: the value, with *is_auto set when the property was auto/absent.
   In the engine's flat stacking context a negative z-index still paints above
   the in-flow content; see the PART C1 notes. */
int rb_style_zindex(int node, int *is_auto);

int rb_hit(int x, int y);
/* Hit test for a pointer at document-space (x, y) while the page is scrolled
   by `scroll`: a position:fixed box lives in viewport space, so it is compared
   at y - scroll. rb_hit(x, y) is rb_hit_at(x, y, 0). */
int rb_hit_at(int x, int y, int scroll);
int rb_focus_count(void);
int rb_focus_node(int index);
int rb_focus_index(int node);
/* index is a focus-list slot. backward wraps. Returns the new slot, or -1. */
int rb_tab(int index, int backward);

int rb_click(int x, int y);
/* 0 unchanged, 1 document mutated, 2 navigation queued. */
int rb_activate(int node);
int rb_take_nav(char *out, int cap);
int rb_type_char(int node, char ch);
int rb_backspace(int node);
int rb_is_field(int node);

/* 1 and writes out, or 0 when href is empty or a same-page fragment. */
int rb_resolve(const char *base, const char *href, char *out, int cap);

/* Nonzero when the last rb_open hit a fixed pool limit and dropped content. */
int rb_oom(void);
/* Host recovered from a fault or hang inside the engine: clear in-progress
   state (layout stack, dispatch depth) before the next rb_open. */
void rb_crash_reset(void);

/* External stylesheets. After rb_open_web, rb_css_pending() counts the sheets
   the document names (<link rel=stylesheet>); an @import inside a provided
   sheet appends more, so loop while i < rb_css_pending(). Resolve the href
   against the page URL, fetch it, hand the text to rb_css_provide(), and call
   rb_relayout() once at the end. */
int rb_css_pending(void);
int rb_css_done(int i);
/* Index of the sheet that @imported sheet i (resolve its href against that
   sheet's URL), or -1 when the page itself named it. */
int rb_css_parent(int i);
int rb_css_href(int i, char *out, int cap);
int rb_css_provide(int i, const char *css, int len);
/* Table fill levels: 0 nodes 1 boxes 2 pool bytes 3 atoms 4 rules 5 decls 6 interned 7 string bytes 8 layout work. */
int rb_debug_stat(int which);
/* Source location of the first table overflow in the last document (tools). */
int rb_debug_oom_line(void);
const char *rb_debug_oom_file(void);
/* The node's computed style as text, for tools. Returns its length. */
int rb_debug_style(int node, char *out, int cap);
/* Host builds: the element's border box from the last real layout (for diffing against a reference browser). */
int rb_debug_box(int node, int *x, int *y, int *w, int *h);
/* Calls to compute_style in the last layout pass: hits come from the per-pass
   memo, misses from real scans. 0 both when no layout has run since reset. */
int rb_style_stats(int *hits, int *misses);

/* 1 when the node's nearest <form> ancestor declares method="post". The
   engine refuses to submit POST forms, so callers use this to explain why
   rb_activate did nothing. */
int rb_form_post(int node);

/* --- mutable tree ---
   Handles are ints (0 = null). Create returns a detached node; append/insert/
   replace attach it. Mutators never relayout: call rb_relayout() then read
   geometry/boxes. Mutators return 1 on success, 0 on bad handle or exhaustion. */
int rb_dom_create_element(int tag);                        /* new detached element node, or 0 */
int rb_dom_create_text(const char *s, int len);            /* new detached text node (len<0 => strlen), or 0 */
int rb_dom_append_child(int parent, int child);
int rb_dom_insert_before(int parent, int child, int ref);  /* ref==0 => append */
int rb_dom_replace_child(int parent, int newchild, int oldchild);
int rb_dom_remove_child(int child);                        /* detach child's whole subtree, recycle all of it */
int rb_dom_is_text(int node);
int rb_dom_parent(int node);
int rb_dom_first(int node);
int rb_dom_next(int node);
int rb_dom_children_count(int node);

/* --- content and attributes --- */
int rb_dom_set_text(int node, const char *s, int len);     /* element: replaces text content; text node: its data */
int rb_dom_get_text(int node, char *out, int cap);
int rb_dom_set_attr(int node, const char *name, int nlen, const char *val, int vlen);
int rb_dom_get_attr(int node, const char *name, int nlen, char *out, int cap);
int rb_dom_has_attr(int node, const char *name, int nlen);
int rb_dom_remove_attr(int node, const char *name, int nlen);
int rb_dom_set_id(int node, const char *id, int len);
int rb_dom_class_contains(int node, const char *name, int len);
int rb_dom_add_class(int node, const char *name, int len);
int rb_dom_remove_class(int node, const char *name, int len);

/* --- scripting ---
   What a JS engine needs on top of the tree API above. All handles are the same
   ints; "text"/"name" buffers are bytes with an explicit length (len < 0 =
   strlen). Strings the engine stores are ASCII-folded when they come from
   markup (UTF-8 and entities map to the nearest ASCII, as for display); text a
   caller passes to rb_dom_set_text/set_attr/set_value is stored as given. */

/* Scripting mode (sticky across rb_open). When on: every <script> element's
   body is recorded for the host (rb_script_*) instead of going to the
   built-in pattern stub, and onclick attributes are no longer run by the stub
   during dispatch (the host's JS engine owns them). Also while on, the parser
   keeps EVERY attribute (names up to 64 bytes, values up to 4000), not just
   the whitelist the styling needs. Default off: today's behaviour is
   unchanged when off. Set it before rb_open*. */
void rb_set_scripting(int on);

/* <script> elements of the last rb_open/rb_open_web document, in source
   order (only recorded while scripting is on; fragments parsed by
   rb_dom_set_inner_html never add any). the text and length out-params: the inline body exactly
   as in the source (no entity decoding, no size cap), pointing INTO the
   buffer the host passed to rb_open*, so it is valid while the host keeps
   that buffer unchanged. External scripts have a src attribute (read it with
   rb_dom_get_attr). Returns 1, or 0 for a bad index. The table holds 256
   scripts; later ones are not recorded. The script element itself has no
   children in the tree (its body lives only here). */
int rb_script_count(void);
int rb_script_get(int i, int *node, const char **text, int *len);

/* Tag names. rb_tag_from_name: case-insensitive, RB_TAG_OTHER if unknown.
   rb_dom_tag_name: lower-case name of an element ("div", or the original
   name of an unknown/custom element such as "my-widget"), "#text" for text
   nodes, "#document" for the root; returns its length (0 for a bad node);
   out gets a NUL-terminated prefix, so a length >= cap means truncated.
   rb_dom_create_element_named: like rb_dom_create_element but by name, keeping
   unknown names (0 for an empty/invalid name or exhaustion). Up to 255
   distinct custom names are remembered per document; past that an unknown
   element reports "unknown". */
int rb_tag_from_name(const char *name, int len);
int rb_dom_tag_name(int node, char *out, int cap);
int rb_dom_create_element_named(const char *name, int len);

/* Siblings/children not covered by first/next. Generated ::before/::after
   nodes are skipped everywhere in this API. */
int rb_dom_prev(int node);
int rb_dom_last(int node);

/* Attributes: rb_dom_set_attr accepts ANY well-formed name (it is lower-cased;
   no whitespace, / > < = quotes; 1..64 bytes). Unknown names go to the
   element's attribute list, replacing an existing value, and call style_touch
   so attribute selectors re-match after rb_relayout(). rb_dom_get_attr/has/
   remove see them too; remove also clears checked/disabled/hidden and
   returns 1 for any live node even when the attribute was absent.
   Boolean attributes (checked, disabled, hidden) read back as "". "checked"
   is the live state, so a click that toggled a checkbox shows up as an
   attribute. "value" and "alt" share one slot (alt on <img>/<area>), and an
   empty id/class/style/href/name value counts as absent. get_attr("type") on
   an <input>/<button>/<textarea> returns the effective type even when the
   attribute is absent; has_attr says whether it was written.
   Enumeration covers every attribute the element has, dedicated fields
   included (id, class, style, href, name, action, onclick, value|alt,
   checked, disabled, hidden, in that order), then the rest in source/insertion
   order. rb_dom_attr_at returns 1 and fills name/value (NUL-terminated,
   truncated to cap), 0 past the end. rb_dom_attr_len is the full value length
   (-1 if absent) for sizing a buffer. */
int rb_dom_attr_count(int node);
int rb_dom_attr_at(int node, int i, char *name, int ncap, char *val, int vcap);
int rb_dom_attr_len(int node, const char *name, int nlen);

/* Text. rb_dom_get_text (above) is the display text: it returns the number of
   bytes WRITTEN (truncated to cap-1, not the full length), skips <script> and
   <style> descendants, and gives an <input>/<textarea> its value.
   rb_dom_text_content is DOM textContent: every descendant text node in order
   (script/style bodies included), returns the FULL length like snprintf, out
   gets a NUL-terminated prefix. A text node returns its own data. */
int rb_dom_text_content(int node, char *out, int cap);

/* Selectors (same matcher as the cascade, selector lists allowed).
   rb_dom_query: elements in root's subtree (root itself excluded; root may be
   1 = the document) that match, in document order; writes up to max handles,
   returns the total number of matches found (may exceed max), or -1 if the
   selector does not parse. It leaves the cascade's tables as it found them.
   rb_dom_matches: 1/0, -1 if the selector does not parse.
   rb_dom_closest: nearest inclusive ancestor element matching, 0 if none,
   -1 if the selector does not parse.
   A well-formed selector that cannot match here (:hover, :focus, :has(),
   ::before, ...) is valid and matches nothing. Up to 32 comma alternatives
   and 8 compound selectors per alternative. */
int rb_dom_query(int root, const char *sel, int len, int *out, int max);
int rb_dom_matches(int node, const char *sel, int len);
int rb_dom_closest(int node, const char *sel, int len);

/* Markup. rb_dom_set_inner_html: replace node's children with the parsed
   fragment (same tokenizer/tree builder as documents, with node as the
   insertion context; html/head/body tags in the fragment are dropped, scripts
   are never recorded or run, <style> and <link> inside are ignored, <title>
   text is dropped). Returns 1, 0 on bad node (the document root included) or
   exhaustion, in which case the old children are untouched.
   rb_dom_insert_adjacent_html: where 0 beforebegin, 1 afterbegin, 2 beforeend,
   3 afterend (parsed into a temporary detached container, then moved).
   rb_dom_get_html: serialize node's children (outer=0) or node itself
   (outer=1) as HTML: lower-case tags, attributes (as enumerated above, always
   name="value") with values escaped (& " <), text escaped (& < >), void
   elements without end tags, raw-text elements (script/style) unescaped.
   Writes a NUL-terminated prefix into out and returns the full length (like
   snprintf), so the caller can retry. A <title>'s text is not in the tree and
   a source <script>'s body is only in rb_script_get; both serialize empty. */
int rb_dom_set_inner_html(int node, const char *html, int len);
int rb_dom_insert_adjacent_html(int node, int where, const char *html, int len);
int rb_dom_get_html(int node, int outer, char *out, int cap);

/* Unlink node and its subtree from its parent but keep every node alive and
   valid (same generation), so it can be inserted again (JS removeChild).
   Returns 1, or 0 for the root, a bad handle or a node with no parent.
   Clears the active element and the tab stops that pointed into the subtree.
   Detached subtrees are reclaimed by the next rb_open (rb_find_id and the
   selector API never see them). Unlike rb_dom_remove_child nothing is
   recycled. rb_dom_append_child/insert_before/replace_child MOVE a node that
   is attached elsewhere (unlink first, no recycle, generation unchanged) and
   refuse cycles and the document root; rb_dom_replace_child recycles the
   node it replaces, so to keep that one alive insert the new node before it
   and rb_dom_detach it instead. */
int rb_dom_detach(int node);

/* Navigation requested by a script (location.href = ...): same slot
   rb_take_nav reads; the host resolves it against the current URL. At most
   479 bytes are kept. */
void rb_request_nav(const char *url, int len);

/* Form state for scripts. value: the live value of input/textarea/select
   (what typing changed; button and option too). get returns the FULL length
   (out holds a NUL-terminated prefix) or -1 for a node that has no value;
   a checkbox/radio without a value reads "on"; a select reads its selected
   (else first) option's value. set returns 1/0: input/textarea/button/option
   store the string; a select selects the first option whose value equals it
   and returns 1, or deselects every option (the first one shows) and returns 0
   when none matches. checked: checkbox/radio state; set(on) of a radio clears the
   same-named radios of its form. get_checked is 0 for anything else, set 0
   when the node is not a checkbox/radio. */
int rb_dom_get_value(int node, char *out, int cap);
int rb_dom_set_value(int node, const char *s, int len);
int rb_dom_get_checked(int node);
int rb_dom_set_checked(int node, int on);

/* --- geometry (valid after rb_relayout()) --- */
int rb_dom_rect(int node, int *x, int *y, int *w, int *h); /* union of the node's boxes; 1 if it has one */
int rb_dom_offset_width(int node, int *w);
int rb_dom_offset_height(int node, int *h);

/* --- images (Phase 4b) ---
   The engine keeps a small image record per <img> element; the host (the
   loader in rbrowse.c, which owns fetching and decoding through image.c)
   fills it in. Layout reads the natural size from it, and the painter finds
   the pixels through the RB_BOX_IMAGE box's node.

   Image mode (sticky across rb_open, default off like scripting): when off,
   every <img> keeps the old placeholder (a pale box or the alt text) and the
   records are ignored, so a host without a loader sees no change. When on:
   an <img> whose record is RB_IMG_NONE (not loaded yet) or RB_IMG_OK lays
   out as an RB_BOX_IMAGE box (painted as the image once decoded, as the
   placeholder fill until then, without alt text); RB_IMG_BROKEN and
   RB_IMG_EMPTY keep the old placeholder with the alt text. The native
   profile (rb_open) shows only RB_IMG_OK images; the others keep its
   "[alt]" control look.

   Sizing (replaced element): width/height from CSS or the attributes, the
   other one from the natural aspect ratio, or the natural size; min/max
   clamps keep the ratio; an image is never wider than its container (as if
   max-width:100% were always set). Web profile: unlike every other vertical
   length, an image is NOT halved vertically (an image W x H css px is
   W x H screen px), so it keeps its aspect ratio; CSS/attribute heights of
   images are doubled back from the halved value (odd heights round up by
   one px). An unsized image whose size is not known yet has no box: its alt
   text flows inline until it loads. <picture> is an ordinary inline
   wrapper (the host picks the <source>). Not done: vertical-align (an
   inline image sits at the top of its line box, like every inline box
   here), object-fit/object-position, border-radius on the image itself,
   CSS background-image. */
enum { RB_IMG_NONE = 0, RB_IMG_OK = 1, RB_IMG_BROKEN = 2, RB_IMG_EMPTY = 3 };
void rb_set_images(int on);
int rb_images_on(void);
/* Next live <img> element after handle `after` in slot order (attached or
   detached: a script's new Image() counts), 0 at the end. Start with 0. */
int rb_dom_img_next(int after);
/* The record: returns the state (RB_IMG_NONE for anything that is not a
   live <img>) and, when given, the host's slot and the natural size. */
int rb_dom_img_state(int node, int *slot, int *nw, int *nh);
/* Host: store the outcome for an <img>. Returns 1 when the last layout
   depends on it (rb_relayout needed), 0 when a repaint is enough (an image
   laid out at an explicit size that just finished loading), -1 for a bad
   node. Changing an <img>'s src or srcset attribute resets its record to
   RB_IMG_NONE (the host notices on its next scan). */
int rb_dom_img_set(int node, int state, int slot, int nw, int nh);

/* --- canvas (Phase 4c) ---
   Canvas mode (sticky across rb_open, default off like image mode): when
   on, a <canvas> is a replaced element whose intrinsic size is its width/
   height attributes (rb_canvas_attr_size; they are not presentational CSS
   sizes then), laid out like an image (CSS width/height override, one of
   them alone keeps the aspect ratio, never wider than its container, not
   halved vertically in the web profile), in both profiles, as one
   RB_BOX_CANVAS box. The host owns the pixels (src/rbrowse/canvas.c) and
   the painter shows them through its canvas hook (paint.h). When off, a
   canvas keeps the old "[canvas]" placeholder. Changing width/height is an
   attribute change, so the next rb_relayout picks it up. */
void rb_set_canvas(int on);
int rb_canvas_on(void);
/* The canvas size the width/height attributes give (HTML rules for
   non-negative integers: leading spaces, digits, anything after ignored;
   missing or invalid = 300 x 150), capped at 65535. Returns 1 for a live
   <canvas> element, 0 (and 300 x 150) otherwise. */
int rb_canvas_attr_size(int node, int *w, int *h);

/* A CSS colour string with its alpha, for the canvas: 1 and 0xAARRGGBB
   (straight alpha; transparent is 0) for hex, rgb()/rgba(), hsl()/hsla(),
   named colours, transparent and light-dark(); 0 for anything else
   (currentColor and inherit included). */
int rb_parse_color(const char *s, int len, uint32_t *argb);

/* --- events (Part 2) ---
   Order is part of the ABI; keep it. New types append at the end
   (RB_EV_MOUSEMOVE is newest). */
enum {
    RB_EV_CLICK = 0, RB_EV_MOUSEDOWN, RB_EV_MOUSEUP, RB_EV_MOUSEOVER, RB_EV_MOUSEOUT,
    RB_EV_INPUT, RB_EV_CHANGE, RB_EV_SUBMIT, RB_EV_FOCUS, RB_EV_BLUR,
    RB_EV_KEYDOWN, RB_EV_KEYUP, RB_EV_KEYPRESS, RB_EV_SCROLL, RB_EV_LOAD,
    RB_EV_MOUSEMOVE,
    RB_EV_COUNT
};

/* Modifier bits for rb_event_mods(). Only SHIFT is tracked by the UI today
   (the PS/2 firmware handles ctrl/alt chords without reporting them); the
   bits exist so a future producer can set them without an ABI change. */
enum {
    RB_MOD_SHIFT = 1,
    RB_MOD_CTRL  = 2,
    RB_MOD_ALT   = 4
};

/* type = RB_EV_*, target = the node the event was dispatched on,
   current = the node whose listener is running. The fields are valid only
   inside a listener during dispatch. */
typedef void (*RbListener)(int type, int target, int current, void *user);

int rb_listen(int node, int type, RbListener fn, void *user);           /* bubble phase */
int rb_listen_capture(int node, int type, RbListener fn, void *user);   /* capture phase */
int rb_listen_remove(int node, int type);        /* remove that node's listeners for that type (both phases) */
int rb_listen_remove_all(int node);
int rb_listener_count(void);                     /* total registered */

/* Dispatch from target to the root: capture root->target, then bubble
   target->root. Then run the default action unless a listener called
   rb_event_prevent(). Returns 0 (no effect), 1 (document mutated),
   2 (navigation queued) — same contract as rb_activate. Returns 0 for
   node 0 / dead node / bad type. */
int rb_dispatch(int target, int type);

/* Extended dispatch with an event payload. key is a character code for
   keyboard events (0 otherwise); x/y are content-space coordinates for mouse
   events; button is 0=left, 1=middle, 2=right, -1=none; buttons is the PS/2
   button bitmask (bit0 left, bit1 right, bit2 middle, as the packet carries
   it); mods is RB_MOD_* for keyboard events. The payload lives in the
   per-depth dispatch state, so a nested dispatch cannot clobber an outer
   event's payload.
   rb_dispatch(t, ty) == rb_dispatch_ex(t, ty, 0, 0, 0, -1, 0, 0). */
int rb_dispatch_ex(int target, int type, int key, int x, int y, int button,
                   int buttons, int mods);

/* Nonzero when the last rb_open's document (or an earlier one in the same
   document) dropped a nested dispatch past the re-entrancy cap (EV_MAX_DEPTH,
   currently 4). A rejected dispatch returns 0 exactly like one with no
   listeners, so this counter is the only way to tell the two apart; it is a
   monotonic count reset by rb_open, in the same spirit as rb_oom. */
int rb_event_dropped(void);

/* preventDefault of the most recently finished dispatch. rb_event_prevented()
   is only valid inside a listener; a UI seam that owns a default action (e.g.
   typing a key) reads this after the dispatch returns to decide whether to run
   it. A dispatch rejected up front (dead target, depth cap) reports 0. */
int rb_last_dispatch_prevented(void);

/* Current-dispatch state, valid only inside a listener. */
int  rb_event_type(void);
int  rb_event_target(void);
int  rb_event_current(void);
/* Payload getters: the key code, content-space coordinates and button of the
   event being dispatched. Outside a dispatch they report the empty payload
   (key 0, x 0, y 0, button -1, buttons 0, mods 0). */
int  rb_event_key(void);
int  rb_event_x(void);
int  rb_event_y(void);
int  rb_event_button(void);
/* Raw PS/2 button mask (bit0 left, bit1 right, bit2 middle) of the mouse
   event being dispatched — rb_event_button() only carries one button index.
   0 outside a dispatch. */
int  rb_event_buttons(void);
/* RB_MOD_* bitmask for keyboard events (SHIFT tracked by the UI; set for
   KEYDOWN/KEYUP/KEYPRESS). 0 outside a dispatch. */
int  rb_event_mods(void);
void rb_event_prevent(void);         /* preventDefault */
void rb_event_stop(void);            /* stopPropagation: finish current node, then stop */
void rb_event_stop_immediate(void);  /* stop all remaining listeners and propagation */
int  rb_event_prevented(void);
int  rb_event_depth(void);           /* 0 when not dispatching */

/* --- focus --- */
int rb_dom_is_focusable(int node);   /* 1 if it would appear in the focus list */
int rb_dom_focus(int node);          /* dispatches BLUR on the old active element,
                                        FOCUS on this one; 1 if focused */
int rb_dom_blur(void);
int rb_dom_active_element(void);     /* node or 0 */
/* Generation of a node slot (bumped on every recycle), or 0 for an
   invalid/dead node: lets a stored handle be validated across a script's DOM
   mutation so a recycled slot cannot receive an event aimed at the old node. */
int rb_dom_generation(int node);

/* --- Known gaps (documented follow-up; not implemented yet) ---
   1. The event payload is limited to a key code, content-space x/y, button
      and button-mask plus a keyboard modifier mask: no target-relative
      coordinates and no submitter on SubmitEvent (submit_form carries the
      submitter out-of-band — see g_submit_form in engine.c). Ctrl/Alt are not
      tracked by the UI (the PS/2 firmware owns those chords), so
      RB_MOD_CTRL/ALT are never set today.
   1b. The navigation target is a single slot: nested dispatches that both
      queue a URL end with the outer one winning (documented at g_submit_form
      and test_submit_nested_cross).
   2. Per-listener identity is not tracked: no once-only registration and no
      remove-by-function; removal is by (node, type) or all of a node.
   3. Several RB_EV_* types have no producer yet (script/load of a mutated
      document, and the mouse family on paths other than the wired UI).
   4. Script-driven dispatchEvent/element.click() is not supported; only the
      C API (rb_dispatch/rb_dispatch_ex/rb_activate/rb_click) dispatches.
   Also: the listener table is a fixed 256-entry array; registrations past
   that return 0 (they do not set the page-truncation flag). */

/* --- Flexbox (PART B1 core, PART B2 completion) ---
   Full flex-direction (row/row-reverse/column/column-reverse are all real
   since B2: reverses swap the main start/end edge, cascading into
   justify-content and align-items), flex-wrap (wrap/wrap-reverse;
   line breaking on the pre-distribution basis), align-content (all seven
   values; ignored for a single line; stretch grows each line by an equal
   share, remainder to the last line), order (stable sort, ties keep source
   order), min/max-width/height (flex items clamp after distribution with a
   single documented re-pass; plain blocks clamp width in place and height
   like an explicit height), row-gap/column-gap (axis roles swap in a column
   container).
   Approximations to scrutinise:
   - Line breaking runs on the min/max-CLAMPED basis, so min/max does change
     where lines break. What does not re-wrap is a grow/shrink size change:
     distribution happens per line, after breaking.
   - min/max only ever re-passes ONCE: a second clamp violation stands.
   - Cross-axis min/max percentages resolve against the container's DEFINITE
     cross extent (an explicit height for a row, the content width for a
     column); against an indefinite (auto-height) row they are treated as
     auto, as CSS does. The resolved clamp is applied before line breaking, so
     it does re-size the item's line.
   - Stretch fills the line's MARGIN BOX minus the item's own pad*2 + vertical
     margins, so a padded/margined item no longer overflows its line. The
     overhead is taken from the non-percentage box metrics; a percentage
     padding/margin would not be re-derived.
   - The 2000px main cap applies only to an explicit-height container; a
     content-sized column is allowed to exceed it. An individual item is still
     capped at 2000px per axis.
   - More than FLEX_ITEMS_PER children in one container (or MAX_FLEX_TOTAL
     overall) are dropped and the truncation sets rb_oom().
   - An auto-basis control (input/button) is measured at its intrinsic size
     rather than at the flex slot, matching the block path; a wide intrinsic
     control can still dominate a narrow row before shrink.
   - align-items/align-self: baseline maps to flex-start (text baselines are
     not tracked per item; there is no font mask of ascent data).
   - inline-flex behaves as flex (block-level); true inline participation
     was not worth the layout-inline rewrite.
   - A column container only wraps with an explicit height (auto-height
     columns are content-sized and cannot overflow, as in CSS); a wrapped
     column container overflows its box horizontally rather than inflating
     its width. */

/* --- PART C1: positioning, stacking, overflow, opacity ---
   Real: position static/relative/absolute/fixed; top/right/bottom/left as
   signed px or % of the containing block; z-index paint order; overflow
   clipping; visibility (including visibility:visible inside a hidden subtree);
   opacity (per-box alpha); cursor stored and exposed; hit testing that
   respects clips and fixed boxes (rb_hit_at).

   How it works. Layout is still one forward pass. An absolute/fixed element
   is DEFERRED when the pass reaches it (it takes no room in flow or in a flex
   line) and is PLACED when its containing block finishes: the nearest
   positioned ancestor's padding box, else the initial containing block
   (RB_VIEW_W x RB_VIEW_H at the document origin); fixed always uses the
   viewport. Placement lays the element out at the origin and then moves it,
   so right/bottom anchors use the real size. Containing-block frames, the
   deferral table (1024 entries) and the clip table (255 rects) are bounded
   static arrays; a full deferral table drops the element and sets rb_oom(); a
   full clip table reuses the last clip silently (the box is still clipped, to
   a possibly different rect). Re-laying an item (flex measure passes) overwrites its deferral
   instead of duplicating it.

   Stacking: boxes of every positioned element are tagged RB_BOX_POS and moved,
   stably ordered by z-index, to the end of the box list, so paint order is
   box order. relative boxes move by their offsets after layout (siblings and
   the parent's height are unaffected). A fixed box is stored in viewport space
   and flagged RB_BOX_FIXED: the painter must not subtract the scroll offset.

   Documented approximations (read these before relying on a feature):
   1. STICKY IS NOT IMPLEMENTED. position:sticky lays out exactly like
      relative (its offsets apply once, no scroll-dependent stickiness).
      rb_style_position still reports RB_POS_STICKY.
   2. ONE FLAT STACKING CONTEXT. Positioned boxes float above the whole static
      flow, ordered by z-index then source order. No per-ancestor stacking
      contexts: a positioned descendant's z-index is overridden by an ancestor
      with an explicit z-index (its subtree travels as a unit), while a
      z-index:auto ancestor leaves it alone. A NEGATIVE z-index still paints
      above static content (not behind it).
   3. Static position (auto left/right or top/bottom) is the start of the next
      line / the flow cursor when the element was reached; for a block it is
      approximate inside inline runs.
   4. Shrink-to-fit is "lay out at the available width, measure the widest
      non-background box, lay out again at that width". It ignores the CSS
      min-content rule and does not account for nested backgrounds.
   5. top+bottom with auto height stretches through the engine's force-height
      seam; content taller than the stretch still grows the box, because an
      explicit height is a minimum everywhere except on a clipper.
   6. overflow: hidden/clip/auto/scroll all clip to the PADDING box and an
      explicit height on a clipper is exact (content past it is cut). There are
      no scrollbars, so auto/scroll content past the edge cannot be reached.
      (The CSS shorthand collapses overflow-x/-y into one value: hidden or clip
      wins, otherwise auto/scroll.)
      A clipper clips absolute descendants only when it is (or contains) their
      containing block, as in CSS. overflow-x/-y are not independent.
   7. opacity is per-box alpha: values under ~0.125 paint nothing, otherwise
      each box is blended on its own (no group compositing, so a translucent
      element's own background shows through its text). The control frame
      and the focus ring are opaque. Opacity multiplies down the tree and a
      subtree below the threshold is hidden as a whole.
   8. visibility:hidden keeps the space and removes the paint, hit testing and
      tab stop of an element and everything in its subtree except descendants
      with an explicit visibility:visible.
   9. A vertical percentage offset on a relative element resolves to 0 (its
      containing block has no definite height). Offsets are clamped to
      +-8000px, and box coordinates to +-16000 so they stay in int16_t.
  10. Absolute elements whose containing block is a positioned INLINE element
      resolve against the next positioned block ancestor instead (inline
      elements are not containing blocks here).
  11. The document height grows to include the bottom of non-fixed positioned
      boxes, capped at 4000px.
  12. cursor is stored and exposed (rb_style_cursor) but the UI draws no
      per-cursor shape.
  13. Script-driven .style.position/z-index/opacity is not wired (the JS style
      setter still handles only color/background/display).
  14. Layout text lives in a pool span that the next layout reclaims, so plain
      relayouts do not leak the text pool. A DOM edit (typing into a field) in
      between stops that one reclaim and its text stays as a hole; the 1 MB pool
      makes that harmless in practice. */

/* --- PART C2: box model, borders, text ---
   margin: auto (and margin-left/right: auto) centres or pushes a block that
   has a definite width; with width:auto it resolves to 0, as in CSS.

   box-sizing. Without the property the engine keeps its long-standing
   behaviour: an explicit WIDTH is the border box and an explicit HEIGHT is the
   content box (this differs from CSS, whose default is content-box on both).
   box-sizing:content-box / border-box give the CSS meaning on both axes
   (min/max sizes included). Percentage sizes are not rescaled.

   border / border-top|right|bottom|left / border-width / border-color /
   border-style:none. Width 0-8px, any colour, no colour = currentColor. A side
   needs a width and a visible style; it then takes layout room (it is part of
   that side's inset), is the padding box's outer edge for overflow and
   absolute positioning, and is a BG-kind strip (one RB_BOX_BORDER ring box
   when all four are equal and there is a radius). Styles other
   than none/hidden are all drawn solid (dashed/dotted/double are not drawn),
   and border-width/-color longhands take one value and apply to all sides.
   Controls (button/input) keep their own frame; CSS borders on them add the
   inset but are not drawn.

   border-radius: one radius (the first value), px or %, capped at half the
   shorter side; 50% or >= 100px is a circle/pill. Painted per scanline by
   paint.c; it applies to the fill (RbBox.radius on the BG box) and to the
   ring. Per-corner radii and elliptical radii are not supported.

   Text. font-weight (bold >= 600; <b>, <strong>, <th>, h1-h6 are bold),
   text-decoration (underline / line-through / none; links and <u> underline),
   text-transform (uppercase / lowercase / capitalize, applied to the box copy,
   not the DOM), white-space (normal / nowrap / pre / pre-wrap; pre-line acts
   as normal), line-height (px, unitless or %, normal; glyphs sit in the middle
   of the line box), letter-spacing (0-8px). All inherit. Bold is the glyph
   smeared one pixel (the 8x8 bitmap font has no bold face), so the advance is
   unchanged. Text-decoration is painted per text box in the box's colour.
   font-family and font-style (italic) are ignored; font-size is still only
   scale 1 or 2.

   --- PART C3: grid ---
   display:grid | inline-grid (block-level either way); grid-template-columns
   with px, %, fr, auto (sized like 1fr), minmax(a, b) (uses b), fit-content()
   (auto), repeat(N, ...) up to 12 tracks and repeat(auto-fill|auto-fit,
   minmax(Npx, 1fr) | Npx | 1fr) (the column count is fixed from the container
   width; auto-fit does not collapse empty tracks); gap / row-gap / column-gap;
   grid-column and grid-row on items as line numbers, "span N", "a / b",
   "a / span N", negative lines; sparse row-major auto-placement with column
   and row spans (a row-spanning item's extra height is shared equally by the
   rows it spans); justify-items, align-items, align-self and place-items
   (start / center / end / stretch); stretch gives an auto-height item the row
   height. Not supported: grid-template-rows / -areas, grid-area, grid-auto-*
   (rows are always auto-sized from their items), dense packing, subgrid,
   justify-content / align-content for the tracks, fr tracks with a content
   minimum. Up to 96 items and 48 rows per layout; more set rb_oom() and are
   dropped. Absolute/fixed children of a grid leave the grid as in flex. */

/* --- WEB PROFILE (rb_open_web) -------------------------------------------------
   rb_open keeps the original native look (amber on brown, CSS px == screen px).
   rb_open_web renders a real page: a white page, a standard user-agent
   stylesheet, and a cascade close to the real thing.

   Scale. The 8x8 font advances 8px per character, which is what a 16px
   proportional font averages, so HORIZONTAL css px map 1:1 to screen px (the
   viewport is RB_VIEW_W css px wide: 622 on the 640x360 screen, 304 in the
   320x200 fallback). The glyph is half as tall as that font's line, so
   VERTICAL css px are halved; that keeps margins, paddings and line heights
   in proportion to the text, at the price of squashing explicit heights
   (an element 100x100 css px is 100x50 on screen). font-size picks scale 1
   below 26px, 2 above. @media queries see (RB_VIEW_W + 16) x (2 * RB_VIEW_H)
   css px: 638x664 on the 640x360 screen.

   CSS. Selectors: type (including unknown/custom elements), *, .class, #id,
   [attr] with = ~= |= ^= $= *= (and the i flag), descendant, child, +, ~,
   :not() :is() :where(), :first/last/only-child, :nth-child/-last-child/
   -of-type(an+b), :first/last/only-of-type, :empty :root :link :checked
   :disabled :enabled :required :optional, ::before/::after (generated text).
   :hover, :focus, :active, :visited and the other dynamic states never match.
   Cascade: UA < presentational hints (bgcolor, align, cellpadding, width,
   <font>...) < author < inline < !important, by specificity then source order;
   <link> sheets keep their <link>'s place. @media, @supports, @layer and
   @import are understood (@font-face, @keyframes, @container, @page are
   skipped). Custom properties and var() work for variables defined by rules
   that match <html> or <body>.
   Values: px em rem % pt cm mm in vw vh vmin vmax ch ex, calc() min() max()
   clamp(), hex/rgb()/hsl()/named/transparent/currentColor/light-dark().
   Layout: block, inline, inline-block, list-item (markers), table (auto
   layout, colspan/rowspan, border-spacing/collapse, caption), float and clear,
   flex (with min-width:auto), grid, positioning (PART C1), overflow, margin
   collapsing between adjacent block siblings, min/max sizes, box-sizing,
   borders and radius, transform: translate.
   Images: <img> (see "images" above; PNG/JPEG/GIF/BMP through the host's
   loader). Not done: background images and gradients, any face but the one 8x8 font (italic is ignored), transforms
   other than translate, animations, ::first-line/::marker/::selection,
   counters, attr() in content, sticky positioning, scripts.

   Text is folded to ASCII for the font: UTF-8 and entities map to the nearest
   ASCII (accents drop, curly quotes straighten, "..." for an ellipsis).

   External stylesheets: after rb_open_web the page's <link rel=stylesheet> and
   the @imports they pull in are listed by rb_css_pending(); the host fetches
   each (rb_css_href, resolved against the page URL), hands the text to
   rb_css_provide(), and calls rb_relayout() once. Rules that name a class, id
   or tag the document does not contain are dropped before they cost memory.

   Limits (static tables; overflow sets rb_oom): 16384 nodes and boxes, a 1 MB
   text pool, 16000 rules, 48000 selector atoms, 40000 declarations, 1536
   custom properties, 90000 layout_block calls per layout (a page needing more
   is cut short), 140 levels of nesting. Layout and parsing run on a private
   768 KB stack on the kernel build, so deep pages do not depend on the shell
   stack. */

#endif
