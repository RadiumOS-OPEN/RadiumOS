#ifndef RBROWSE_ENGINE_H
#define RBROWSE_ENGINE_H

#include <stdint.h>

/* Content width inside the 320x200 mode-13h page, left of the scrollbar. */
#define RB_VIEW_W 304

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
    RB_TAG_OTHER
};

enum {
    RB_BOX_TEXT = 0,
    RB_BOX_BG = 1,
    RB_BOX_CONTROL = 2,
    RB_BOX_RULE = 3
};

typedef struct {
    int16_t x, y, w, h;
    uint16_t node;
    uint16_t toff, tlen;
    uint8_t color, bg, scale, kind;
} RbBox;

/* 6x6x6 cube at palette indices 16..231. Indices 0..15 stay free for chrome. */
int rb_map_rgb(int r, int g, int b);
void rb_cube_rgb(int index, int *r, int *g, int *b);

int rb_open(const char *html, int len);
/* Copy markup into dst, dropping script and style elements so a short
   buffer keeps the visible body instead of a truncated script. */
int rb_compact_markup(const char *src, int n, char *dst, int cap);
void rb_relayout(void);
int rb_page_height(void);
int rb_page_bg(void);
int rb_title(char *out, int cap);

int rb_find_id(const char *id);
int rb_node_tag(int node);
int rb_node_color(int node);
int rb_node_href(int node, char *out, int cap);
int rb_collect_text(int node, char *out, int cap);

int rb_box_count(void);
const RbBox *rb_box(int i);
const char *rb_box_text(int i, int *len);
int rb_first_box(int node, int *x, int *y, int *w, int *h);

int rb_hit(int x, int y);
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

#endif
