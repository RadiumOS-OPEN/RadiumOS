/*
 * Host-side hostile-input fuzzer for the rbrowse engine.
 *
 * The engine parses untrusted HTML/CSS/JS and writes into fixed static pools,
 * so it is exercised here under ASan+UBSan. Not part of the kernel build.
 *
 *   clang -g -O1 -fsanitize=address,undefined -DRBROWSE_HOST -I src/rbrowse \
 *       tools/rbrowse/fuzz.c src/rbrowse/engine.c -o /tmp/rbrowse_fuzz
 *   /tmp/rbrowse_fuzz [iterations] [seed]
 */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng = 0x12345678;

static unsigned rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (unsigned)(rng >> 16);
}

static const char *toks[] = {
    "<a href=\"/x\">", "</a>", "<div>", "</div>", "<p ", "class=\"a b\"", "id=i",
    "style=\"", "color:#ff0000;", "width:100%;", "height:99999px;", "margin:1 2 3 4;",
    "padding:", "background:", "display:none", "display:block", "<script>", "</script>",
    "<style>", "</style>", ".cls{", "#id{", "@media{", "/*", "*/", "}", "{", ":",
    "<input ", "type=hidden", "type=submit", "value=\"", "onclick=\"", "<img ", "alt=",
    "name=\"q\"", "name=a", "action=\"/s\"", "action=", "method=get", "method=post",
    "<form action=\"/s\">", "<form><input name=q><input type=submit></form>", "=",
    "<textarea>", "</textarea>", "<title>", "</title>", "<!--", "-->", "<!DOCTYPE html>",
    "&amp;", "&#x41;", "&#999999999999999999;", "&#", "&nbsp;", "<", ">", "&", "\"", "'",
    "&mdash;", "&hellip;", "&lsquo;", "&divide;", "&unknown;", "id=\"&#45;&#46;\"",
    "class=m&#32;n", "style=\"&#99;olor:#ff0000\"",
    "<b>", "</b>", "<ul><li>", "<table><tr><td>", "<pre>", "\n", "\r\n", " ", "text",
    "document.getElementById('x').textContent='y';", "location.href='z';",
    "function f(){", "//", "\x80\x81\xff", "<h1>", "<center>", "<form>", "</form>",
    "<div><div><div><div><div><div><div><div><div><div>", "</div></div></div></div></div>",
    "<span><b><i><u><a href=\"x\">", "</a></i></b></span>", "<table><tr><td><td><tr>",
    "<button>", "</button>", "<input type=button>", "<hr>", "<br/>", "<meta http-equiv=x>",
    "<form action=\"/s?x=1#f\">", "<form method=POST>", "<form method=POST action=\"/x\">",
    "<form><form><input name=n>", "</form><input name=after>", "<input name=\"\">",
    "<input type=reset>", "<input type=image>", "<textarea name=t>", "<input name=q value=\"v&w=\">",
    "<input name=d disabled>", "<input type=checkbox name=c value=on checked>",
    "<input type=radio name=r value=1>", "<input type=submit disabled>", "disabled checked",
    "a=\"", "href=\"http://", ".c1#i2>p{", "*{", "p,a{", "  ,  {", "@import url(x);",
    "p.c1#i2{color:#00ff00}", ".a.b{", "#i.c{", "p{", "p.", "#", ".{", "p#i{",
    "background:url(x)", "position:absolute", "z-index:99", "font-size:", "9999",
    "%", "px", "em", "!important", "onclick=\"location.href='q'\"",
    /* flex (PART B1) */
    "display:flex;", "display:inline-flex;", "flex-direction:row;",
    "flex-direction:column;", "flex-direction:row-reverse;",
    "flex-direction:column-reverse;", "justify-content:center;",
    "justify-content:space-between;", "justify-content:space-around;",
    "justify-content:space-evenly;", "justify-content:flex-end;",
    "align-items:stretch;", "align-items:center;", "align-items:baseline;",
    "align-items:flex-end;", "align-self:center;", "align-self:auto;",
    "gap:0;", "gap:9999;", "gap:3;", "row-gap:3;", "column-gap:3;",
    "flex-grow:0;", "flex-grow:99999;", "flex-grow:0.5;", "flex-shrink:0;",
    "flex-shrink:2;", "flex-basis:auto;", "flex-basis:40px;", "flex-basis:50%;",
    "flex:1;", "flex:none;", "flex:auto;", "flex:initial;", "flex:1 2 3px;",
    "flex:0 0 100%;", "flex:2 1;", "style=\"display:flex\"", "style=\"flex:1\"",
    "<style>div{background-image:url('</style>')}", "<script>var a='</script>';</script>",
    "<textarea>&#x41;&#65;</textarea>", "<title>ti</title>", "<img src=x alt=y>",
    "http://", "https://", "file://-/", "/..", "../", "?", "#", ":", "@",
    /* positioning, stacking, clipping (PART C1) */
    "position:relative;", "position:absolute;", "position:fixed;", "position:sticky;",
    "position:static;", "top:-99999px;", "left:-9999px;", "right:99999px;", "bottom:0;",
    "top:50%;", "left:-50%;", "top:5px;bottom:5px;", "left:1px;right:1px;", "z-index:-99999;",
    "z-index:2147483647;", "z-index:auto;", "overflow:hidden;", "overflow:auto;",
    "overflow:clip;", "overflow:scroll;", "visibility:hidden;", "visibility:visible;",
    "opacity:0.5;", "opacity:0;", "opacity:9;", "opacity:-1;", "cursor:pointer;",
    "style=\"position:absolute;top:0;left:0\"", "style=\"position:fixed;bottom:0\"",
    "style=\"position:relative;overflow:hidden;height:5px\"",
    /* box model, borders, text, grid (PART C2/C3) */
    "display:grid;", "display:inline-grid;", "grid-template-columns:repeat(3,1fr);",
    "grid-template-columns:50px 1fr 2fr;", "grid-template-columns:repeat(99,1fr);",
    "grid-template-columns:repeat(auto-fill,minmax(10px,1fr));", "grid-template-columns:25% auto;",
    "grid-template-columns:repeat(2,", "grid-template-columns:minmax(", "grid-template-columns:",
    "grid-column:1 / -1;", "grid-column:span 99;", "grid-column:-9 / 9;", "grid-row:span 99;",
    "grid-row:2 / 4;", "grid-row:99999;", "justify-items:center;", "place-items:end;",
    "border:1px solid #f00;", "border:99px solid red;", "border:none;", "border-bottom:2px solid;",
    "border-radius:999px;", "border-radius:50%;", "border-radius:3px;", "border-width:7px;",
    "margin:0 auto;", "margin-left:auto;", "margin:auto;", "box-sizing:border-box;",
    "box-sizing:content-box;", "font-weight:bold;", "font-weight:900;", "text-decoration:underline;",
    "text-decoration:line-through;", "text-transform:uppercase;", "text-transform:capitalize;",
    "white-space:nowrap;", "white-space:pre;", "letter-spacing:99px;", "letter-spacing:2px;",
    "line-height:99999;", "line-height:0;", "line-height:1.5;", "line-height:150%;",
    "line-height:20px;",
};

static const char *res_bases[] = {
    "/home/user/a.html", "http://example.com/a/b.html", "https://a.example.com/x/y/z",
    "file://-/home/user/x.html", "http://h/", "/", "http://a/b?q=1#f", "root/sub/dir/f",
};
static const char *res_hrefs[] = {
    "hello.html", "../x/../../y", "/abs/path", "file://-/etc/passwd",
    "file://localhost/a/b", "http://other/z", "//proto/rel", "?q=2", "#frag",
    "../../../../../../../../../../x", "a/b/c/../../../d", "",
};

static void exercise_resolve(void) {
    char base[512], href[512];
    int bn = rnd() % (int)sizeof(base);
    int hn = rnd() % (int)sizeof(href);
    for (int i = 0; i < bn; i++) base[i] = (char)(rnd() & 0xff);
    base[bn] = 0;
    for (int i = 0; i < hn; i++) href[i] = (char)(rnd() & 0xff);
    href[hn] = 0;
    int cap = 2 + (int)(rnd() % 520);
    char *out = (char *)malloc((size_t)cap);
    rb_resolve(base, href, out, cap);
    free(out);
    for (unsigned i = 0; i < sizeof(res_bases) / sizeof(res_bases[0]); i++) {
        for (unsigned j = 0; j < sizeof(res_hrefs) / sizeof(res_hrefs[0]); j++) {
            for (int c = 2; c <= 600; c += 7) {
                char *o = (char *)malloc((size_t)c);
                rb_resolve(res_bases[i], res_hrefs[j], o, c);
                free(o);
            }
        }
    }
}

static void exercise_compact(const char *buf, int n) {
    char out[128];
    for (int cap = 1; cap <= (int)sizeof(out); cap++) {
        memset(out, 0x7f, sizeof(out));
        rb_compact_markup(buf, n, out, cap);
        /* rb_compact_markup must NUL-terminate within cap. */
        for (int i = 0; i < (int)sizeof(out); i++) {
            if (out[i] == 0) break;
            if (i >= cap) __builtin_trap();
        }
    }
}

/* Recursively gather live handles from the root. A cycle would blow the depth
   guard (the public mutators refuse ancestor cycles). */
static void dom_gather(int node, int *out, int *n, int cap, int depth) {
    if (!node || *n >= cap) return;
    if (depth > 2048) __builtin_trap();
    out[(*n)++] = node;
    for (int c = rb_dom_first(node); c; c = rb_dom_next(c))
        dom_gather(c, out, n, cap, depth + 1);
}

/* Randomized mutable-DOM churn on a fresh small document, then a relayout and
   a full re-read. Run under ASan/UBSan to catch bad links, OOB pool writes or
   recycling mistakes. */
static void exercise_dom(void) {
    static const int tags[] = {
        RB_TAG_DIV, RB_TAG_SPAN, RB_TAG_P, RB_TAG_A, RB_TAG_B, RB_TAG_BUTTON,
        RB_TAG_INPUT, RB_TAG_TEXTAREA, RB_TAG_FORM, RB_TAG_LI, RB_TAG_UL
    };
    int ntags = (int)(sizeof(tags) / sizeof(tags[0]));
    rb_open("<div id=\"r\"></div>", 16);
    int held[128];
    int hn = 0;
    for (int it = 0; it < 48; it++) {
        int op = (int)(rnd() % 9);
        if (op <= 2) {
            int n;
            if (op == 0) n = rb_dom_create_element(tags[rnd() % (unsigned)ntags]);
            else n = rb_dom_create_text("t", -1);
            if (n && hn < 128) held[hn++] = n;
        } else if (op == 3) {
            int c = hn ? held[rnd() % (unsigned)hn] : 0;
            int ref = hn ? held[rnd() % (unsigned)hn] : 0;
            if (rnd() & 1) rb_dom_append_child(1, c);
            else rb_dom_insert_before(1, c, (rnd() & 1) ? ref : 0);
        } else if (op == 4) {
            int n = hn ? held[rnd() % (unsigned)hn] : 0;
            rb_dom_set_attr(n, "id", 2, "x", 1);
            rb_dom_set_attr(n, "class", 5, "a b", 3);
            rb_dom_add_class(n, "c", 1);
            rb_dom_remove_class(n, "a", 1);
            rb_dom_set_attr(n, "style", 5, "color:#ff0000", 13);
            rb_dom_set_text(n, "abc", -1);
            rb_dom_remove_attr(n, "id", 2);
        } else if (op == 5) {
            int c = hn ? held[rnd() % (unsigned)hn] : 0;
            int d = hn ? held[rnd() % (unsigned)hn] : 0;
            if (rnd() & 1) rb_dom_replace_child(rb_dom_parent(c), d, c);
            else rb_dom_remove_child(c);
        } else if (op == 6) {
            int n = hn ? held[rnd() % (unsigned)hn] : 0;
            char buf[64];
            rb_dom_get_text(n, buf, (int)sizeof(buf));
            rb_dom_get_attr(n, "class", 5, buf, (int)sizeof(buf));
            rb_dom_get_attr(n, "id", 2, buf, (int)sizeof(buf));
            rb_dom_has_attr(n, "id", 2);
            rb_dom_class_contains(n, "a", 1);
            rb_dom_children_count(n);
            rb_dom_is_text(n);
            int x, y, w, h;
            rb_dom_rect(n, &x, &y, &w, &h);
            rb_dom_offset_width(n, &w);
            rb_dom_offset_height(n, &h);
        } else {
            rb_relayout();
            for (int i = 0; i < rb_box_count(); i++) {
                const RbBox *b = rb_box(i);
                if (b) { volatile int z = b->node + b->x + b->y + b->w; (void)z; }
            }
        }
    }
    rb_relayout();
    int live[300];
    int ln = 0;
    dom_gather(1, live, &ln, 300, 0);
    for (int i = 0; i < ln; i++) {
        int x, y, w, h;
        rb_dom_rect(live[i], &x, &y, &w, &h);
        char buf[64];
        rb_dom_get_text(live[i], buf, (int)sizeof(buf));
        rb_dom_set_text(live[i], "q", 1);
    }
}

/* --- event + focus fuzz ---------------------------------------------------
   Listeners that log, stop (immediately), prevent, re-enter the dispatcher and
   remove nodes mid-event. Run under ASan/UBSan: re-entrancy, capture/bubble
   order, the shared listener table and the dead-path guards all get stressed
   with handles that may die during the dispatch. */

static void ev_fz_log(int type, int target, int current, void *user) {
    (void)user;
    volatile int z = type + target + current;
    (void)z;
}

static void ev_fz_stop(int type, int target, int current, void *user) {
    (void)type; (void)target; (void)current; (void)user;
    if (rnd() & 1) rb_event_stop();
    else rb_event_stop_immediate();
}

static void ev_fz_prevent(int type, int target, int current, void *user) {
    (void)type; (void)target; (void)current; (void)user;
    rb_event_prevent();
}

/* Reads the whole payload (including out of a dispatch, where the getters
   report the empty payload) and sometimes prevents/cancels. */
static void ev_fz_payload(int type, int target, int current, void *user) {
    (void)user;
    volatile int z = type + target + current + rb_event_key() + rb_event_x() +
                     rb_event_y() + rb_event_button() + rb_event_buttons() +
                     rb_event_mods();
    (void)z;
    if ((rnd() % 8) == 0) rb_event_prevent();
    if ((rnd() % 8) == 0) rb_event_stop();
}

/* Re-enters the dispatcher past the engine's depth cap (EV_MAX_DEPTH): the
   calls that exceed it must return 0 without corrupting the outer state.
   Half the re-entries carry their own payload, so a nested dispatch with a
   payload must not clobber the outer event's payload. */
static void ev_fz_reenter(int type, int target, int current, void *user) {
    (void)type; (void)target; (void)user;
    if (rb_event_depth() < 8 && current > 0) {
        if (rnd() & 1)
            rb_dispatch(current, (rnd() & 1) ? RB_EV_CHANGE : RB_EV_CLICK);
        else
            rb_dispatch_ex(current, RB_EV_CHANGE, 'x',
                           (int)(rnd() % 400), (int)(rnd() % 260),
                           (int)(rnd() % 4) - 1,
                           (int)(rnd() % 8),       /* buttons mask */
                           (int)(rnd() % 8));      /* mods */
    }
}

/* Kills the target or its parent while the event is still walking, and
   sometimes removes-then-recreates so the LIFO free list recycles a path
   slot underneath the walk. */
static void ev_fz_remove(int type, int target, int current, void *user) {
    (void)type; (void)current; (void)user;
    unsigned r = rnd() % 5;
    if (r == 0) rb_dom_remove_child(target);
    else if (r == 1) rb_dom_remove_child(rb_dom_parent(target));
    else if (r == 2) {
        int p = rb_dom_parent(target);
        rb_dom_remove_child(target);
        for (int i = 0; i < 3; i++) {
            int n = rb_dom_create_element((i & 1) ? RB_TAG_SPAN : RB_TAG_DIV);
            if (!n) continue;
            if (p > 0) rb_dom_append_child(p, n);
            rb_listen(n, (int)(rnd() % RB_EV_COUNT), ev_fz_log, 0);
        }
    }
}

/* Reloads the document in the middle of a dispatch: the F1 crash path. ev_reset
   must not corrupt the dispatch stack (the entry depth is restored on exit and
   the state wipe is deferred). Run occasionally so most iterations still walk
   the normal path. */
static void ev_fz_reopen(int type, int target, int current, void *user) {
    (void)type; (void)target; (void)current; (void)user;
    if ((rnd() % 16) == 0) {
        const char *h = "<div id=\"rr\"><input id=\"ff\"><a href=\"/y\">y</a></div>";
        rb_open(h, (int)strlen(h));
    }
}

static RbListener ev_fz_cbs[] = {
    ev_fz_log, ev_fz_stop, ev_fz_prevent, ev_fz_reenter, ev_fz_remove, ev_fz_payload,
    ev_fz_reopen
};

static void exercise_events(void) {
    const char *html =
        "<div id=\"r\"><a id=\"a\" href=\"/x\" onclick=\"location.href='/js'\">l</a>"
        "<input id=\"f\" value=\"ab\"><button id=\"b\">b</button>"
        "<input type=\"checkbox\" id=\"c\"><form action=\"/s\">"
        "<input name=\"q\" id=\"q\"><input type=\"submit\" id=\"s\"></form>"
        "<div id=\"z\"><span id=\"w\">w</span></div></div>";
    rb_open(html, (int)strlen(html));

    int live[64];
    int ln = 0;
    dom_gather(1, live, &ln, 64, 0);

    /* Producers for the payload-carrying types, on top of the random
       registrations below: KEYDOWN/KEYUP/KEYPRESS on the text field, SUBMIT
       on the form, MOUSEOVER/MOUSEOUT on the root. */
    int fld = rb_find_id("f");
    int frm = 0;
    for (int i = 0; i < ln; i++)
        if (rb_node_tag(live[i]) == RB_TAG_FORM) frm = live[i];
    if (fld > 0) {
        rb_listen(fld, RB_EV_KEYDOWN, ev_fz_payload, 0);
        rb_listen(fld, RB_EV_KEYUP, ev_fz_payload, 0);
        rb_listen(fld, RB_EV_KEYPRESS, ev_fz_payload, 0);
    }
    if (frm > 0) rb_listen(frm, RB_EV_SUBMIT, ev_fz_payload, 0);
    rb_listen(1, RB_EV_MOUSEOVER, ev_fz_payload, 0);
    rb_listen(1, RB_EV_MOUSEOUT, ev_fz_payload, 0);
    /* Guarantee the mid-dispatch reload (F1) path runs, not just when the
       random registration happens to pick ev_fz_reopen. */
    rb_listen(1, RB_EV_CLICK, ev_fz_reopen, 0);
    rb_listen(1, RB_EV_KEYDOWN, ev_fz_reopen, 0);

    int regs = (int)(rnd() % 14);
    for (int i = 0; i < regs && ln > 0; i++) {
        int n = live[rnd() % (unsigned)ln];
        int type = (int)(rnd() % RB_EV_COUNT);
        unsigned cb = rnd() % (sizeof(ev_fz_cbs) / sizeof(ev_fz_cbs[0]));
        if (rnd() & 1) rb_listen_capture(n, type, ev_fz_cbs[cb], (void *)(intptr_t)i);
        else rb_listen(n, type, ev_fz_cbs[cb], (void *)(intptr_t)i);
    }

    char nav[64];
    for (int i = 0; i < 24 && ln > 0; i++) {
        int n = live[rnd() % (unsigned)ln];
        switch (rnd() % 7) {
            case 0: rb_dispatch(n, (int)(rnd() % RB_EV_COUNT)); break;
            case 1: rb_dom_focus(n); break;
            case 2: rb_dom_blur(); break;
            case 3: rb_activate(n); break;
            case 4: rb_type_char(n, (char)('a' + (int)(rnd() % 26))); break;
            case 5: {
                int key = (rnd() & 1) ? (int)('a' + (int)(rnd() % 26)) : 0;
                rb_dispatch_ex(n, (int)(rnd() % RB_EV_COUNT), key,
                               (int)(rnd() % 400) - 20, (int)(rnd() % 260) - 20,
                               (int)(rnd() % 4) - 1,
                               (int)(rnd() % 8),       /* buttons mask */
                               (int)(rnd() % 8));      /* mods */
                break;
            }
            default: rb_backspace(n); break;
        }
        if ((rnd() % 6) == 0) rb_dom_remove_child(n);
        rb_take_nav(nav, (int)sizeof(nav));
    }
    volatile int act = rb_dom_active_element();
    (void)act;
    for (int i = 0; i < ln; i++) {
        volatile int gg = rb_dom_generation(live[i]);   /* 0 for a dead/recycled slot */
        (void)gg;
    }
    rb_listener_count();
    rb_relayout();
    rb_dispatch(1, RB_EV_CLICK);
    /* Re-focusing the active element is quiet; blur is idempotent. */
    for (int i = 0; i < ln; i++) {
        rb_dom_focus(live[i]);
        rb_dom_focus(live[i]);
    }
    rb_dom_blur();
    rb_dom_blur();
}

/* --- flex fuzz ------------------------------------------------------------
   Builds random flex containers (and nested flex items) with random direction,
   justify, align, gap, grow/shrink/basis and cross sizes, including 0,
   fractional and huge values. Run under ASan/UBSan so the two-pass measure and
   the sizing math (clamps, normalization, box translation) are exercised on
   hostile numbers. */
/* Deterministic signature of the whole laid-out box array plus the page
   metrics. Two consecutive layouts of an unchanged document must produce the
   same signature; drift means layout static state leaked between passes (the
   stale-cross scratch class). */
static unsigned long long box_sig(void) {
    unsigned long long h = 1469598103934665603ULL;
    int bc = rb_box_count();
    h = (h ^ (unsigned)bc) * 1099511628211ULL;
    h = (h ^ (unsigned)rb_page_height()) * 1099511628211ULL;
    for (int i = 0; i < bc; i++) {
        const RbBox *b = rb_box(i);
        if (!b) continue;
        h = (h ^ (unsigned long long)(unsigned short)b->x) * 1099511628211ULL;
        h = (h ^ (unsigned long long)(unsigned short)b->y) * 1099511628211ULL;
        h = (h ^ (unsigned long long)(unsigned short)b->w) * 1099511628211ULL;
        h = (h ^ (unsigned long long)(unsigned short)b->h) * 1099511628211ULL;
        h = (h ^ (unsigned long long)b->node) * 1099511628211ULL;
        h = (h ^ (unsigned long long)(unsigned)b->color) * 1099511628211ULL;
        h = (h ^ (unsigned long long)(unsigned)b->bg) * 1099511628211ULL;
    }
    return h;
}

static void canary_stable(const char *where) {
    unsigned long long a = box_sig();
    rb_relayout();
    unsigned long long b = box_sig();
    if (a != b) {
        fprintf(stderr, "fuzz: box signature drift in %s: %016llx -> %016llx\n",
                where, a, b);
        abort();
    }
}

#define FX_CAP ((int)sizeof(buf))
#define FX_APPEND(...) do { \
    if (n < FX_CAP) { \
        int fx_r = snprintf(buf + n, (size_t)(FX_CAP - n), __VA_ARGS__); \
        if (fx_r > 0) n += fx_r; \
    } \
    if (n > FX_CAP - 1) n = FX_CAP - 1; \
} while (0)

static void exercise_flex(void) {
    static const char *dirs[] = {"row", "row-reverse", "column", "column-reverse"};
    static const char *justs[] = {"flex-start", "flex-end", "center",
                                  "space-between", "space-around", "space-evenly"};
    static const char *aligns[] = {"flex-start", "flex-end", "center", "stretch", "baseline"};
    static const char *wraps[] = {"flex-wrap:nowrap;", "flex-wrap:wrap;",
                                  "flex-wrap:wrap-reverse;"};
    static const char *contents[] = {"align-content:flex-start;", "align-content:flex-end;",
                                     "align-content:center;", "align-content:stretch;",
                                     "align-content:space-between;", "align-content:space-around;",
                                     "align-content:space-evenly;"};
    /* one of the three spellings, consistently per container */
    static const char *gapfmt[] = {"gap:%dpx;", "row-gap:%dpx;", "column-gap:%dpx;"};
    static const char *mm[] = {"", "min-width:0px;", "min-width:40px;", "min-width:9999px;",
                               "max-width:20px;", "max-width:0px;", "min-height:10px;",
                               "max-height:5px;", "min-height:0;", "max-height:0;",
                               "max-width:50%;"};
    static const char *orders[] = {"", "order:0;", "order:1;", "order:-1;", "order:2;",
                                   "order:99999;", "order:-99999;", "order:0;"}; /* ties included */
    static const char *bases[] = {"", "flex-basis:auto;", "flex-basis:40px;",
                                  "flex-basis:50%;", "flex-basis:0;", "flex-basis:9999px;"};
    static const char *grows[] = {"", "flex-grow:0;", "flex-grow:1;", "flex-grow:0.5;",
                                  "flex-grow:99999;", "flex-shrink:0;", "flex-shrink:3;",
                                  "flex:1;", "flex:none;", "flex:auto;", "flex:initial;",
                                  "flex:1 2 3px;", "flex:0 0 100%;", "flex:2 1;"};
    static const char *cross[] = {"", "width:30px;", "height:12px;", "width:0;",
                                  "height:9999px;", "width:200px;height:40px;"};
    static const char *selfs[] = {"", "align-self:flex-start;", "align-self:flex-end;",
                                  "align-self:center;", "align-self:stretch;", "align-self:auto;"};
    char buf[4096];
    int n = 0;
    char gstr[32];
    snprintf(gstr, sizeof(gstr), gapfmt[rnd() % 3], (int)(rnd() % 45));
    FX_APPEND("<div style=\"display:flex;flex-direction:%s;justify-content:%s;"
              "align-items:%s;%s%s;"                     /* wrap + align-content */
              "%s%s"                                     /* min/max + order */
              "%swidth:%dpx;height:%dpx;\">",
              dirs[rnd() % 4], justs[rnd() % 6], aligns[rnd() % 5],
              wraps[rnd() % 3], contents[rnd() % 7],
              mm[rnd() % 11], orders[rnd() % 8],
              gstr, (int)(rnd() % 320), (int)(rnd() % 220));
    int items = 3 + (int)(rnd() % 4);
    for (int i = 0; i < items && n < FX_CAP - 400; i++) {
        if (i == 1) {
            /* Deterministic deep path: item 1 of >=3 is a WRAPPING nested flex
               with a grower sibling (item 0). flex:1 gives it a basis of 0, so
               its main size moves off the Pass A measure and the parent ALWAYS
               re-measures the nested container at its final main size — Pass
               A2 on a container whose cross genuinely changes. */
            FX_APPEND("<div id=\"f%d\" style=\"flex:1 1 0;\">"
                      "<div style=\"display:flex;flex-wrap:wrap;width:100px\">"
                      "<div style=\"width:60px;height:8px\"></div>"
                      "<div style=\"width:60px;height:8px\"></div>"
                      "<div style=\"width:60px;height:8px\"></div></div></div>",
                      i);
            continue;
        }
        FX_APPEND("<div id=\"f%d\" style=\"%s%s%s%s%s%s\">", i,
                  cross[rnd() % 6], i == 0 ? "flex-grow:1;" : grows[rnd() % 14],
                  bases[rnd() % 6], selfs[rnd() % 6], mm[rnd() % 11], orders[rnd() % 8]);
        if (rnd() % 3 == 0) FX_APPEND("text%d", i);
        if (rnd() % 4 == 0) {
            FX_APPEND("<div style=\"display:flex;flex-direction:%s;%s;gap:%d\">"
                      "<div style=\"width:%dpx;flex:1\"></div>"
                      "<div style=\"height:%dpx;flex-grow:1\"></div></div>",
                      dirs[rnd() % 4], wraps[rnd() % 3],
                      (int)(rnd() % 20), (int)(rnd() % 60), (int)(rnd() % 40));
        }
        if (rnd() % 5 == 0) FX_APPEND("<span style=\"display:none\">x</span>");
        FX_APPEND("</div>");
    }
    FX_APPEND("</div>");
    rb_open(buf, n);
    rb_relayout();
    int x, y, w, h;
    for (int i = 0; i < items; i++) {
        char id[8];
        snprintf(id, sizeof(id), "f%d", i);
        int node = rb_find_id(id);
        if (rb_dom_rect(node, &x, &y, &w, &h)) {
            volatile int z = x + y + w + h;
            (void)z;
        }
    }
    rb_page_height();
    canary_stable("exercise_flex");
}

/* PART C1: random trees of positioned/clipped/hidden elements with hostile
   offsets, checked against structural invariants after layout. */
static void exercise_c1(void) {
    static const char *pos[] = {"static", "relative", "absolute", "fixed", "sticky"};
    static const char *off[] = {"0", "5px", "-7px", "50%", "-30%", "99999px", "-99999px", "auto",
                                "8000px", "-8000px", "200%"};
    static const char *ovf[] = {"visible", "hidden", "clip", "auto", "scroll"};
    static const char *zs[] = {"auto", "0", "1", "-1", "7", "2147483647", "-2147483648", "99999"};
    static const char *sides[] = {"top", "right", "bottom", "left"};
    char buf[8192];
    int n = 0;
    FX_APPEND("<body>");
    int depth = 0;
    int elems = 4 + (int)(rnd() % 40);
    for (int e = 0; e < elems; e++) {
        int r = rnd() % 8;
        if (r == 0 && depth > 0) { FX_APPEND("</div>"); depth--; continue; }
        if (r == 1) { FX_APPEND("some words to wrap around the line "); continue; }
        if (r == 2) { FX_APPEND("<a href=\"/x\">lnk</a>"); continue; }
        FX_APPEND("<div style=\"position:%s;", pos[rnd() % 5]);
        for (int k = 0; k < 4; k++)
            if (rnd() % 3 == 0) FX_APPEND("%s:%s;", sides[k], off[rnd() % 11]);
        if (rnd() % 2) FX_APPEND("overflow:%s;", ovf[rnd() % 5]);
        if (rnd() % 2) FX_APPEND("z-index:%s;", zs[rnd() % 8]);
        if (rnd() % 4 == 0) FX_APPEND("visibility:%s;", (rnd() & 1) ? "hidden" : "visible");
        if (rnd() % 4 == 0) FX_APPEND("opacity:%d.%d;", (int)(rnd() % 2), (int)(rnd() % 10));
        if (rnd() % 3 == 0) FX_APPEND("width:%dpx;", (int)(rnd() % 400));
        if (rnd() % 3 == 0) FX_APPEND("height:%dpx;", (int)(rnd() % 300));
        if (rnd() % 4 == 0) FX_APPEND("display:flex;");
        if (rnd() % 4 == 0) FX_APPEND("padding:%dpx;", (int)(rnd() % 20));
        if (rnd() % 2) FX_APPEND("background:#%02x%02x%02x;", (int)(rnd() & 255), (int)(rnd() & 255), (int)(rnd() & 255));
        FX_APPEND("\">");
        depth++;
    }
    while (depth-- > 0) FX_APPEND("</div>");
    buf[n] = 0;
    rb_open(buf, n);
    rb_relayout();

    int prev_z = -32768, seen_pos = 0;
    int bc = rb_box_count();
    for (int i = 0; i < bc; i++) {
        const RbBox *b = rb_box(i);
        if (b->x < -30000 || b->x > 30000 || b->y < -4000000 || b->y > 4000000 ||
            b->w < 0 || b->h < 0) {
            fprintf(stderr, "fuzz: C1 box out of range: %d,%d %dx%d\n", b->x, b->y, b->w, b->h);
            abort();
        }
        int cx, cy, cw, ch;
        if (rb_box_clip(i, &cx, &cy, &cw, &ch) && (cw < 0 || ch < 0)) {
            fprintf(stderr, "fuzz: negative clip rect\n");
            abort();
        }
        if (b->flags & RB_BOX_POS) {
            /* Positioned boxes form one z-sorted tail after the static flow. */
            if (b->z < prev_z) { fprintf(stderr, "fuzz: z order violated\n"); abort(); }
            prev_z = b->z;
            seen_pos = 1;
        } else if (seen_pos) {
            fprintf(stderr, "fuzz: static box after the positioned tail\n");
            abort();
        }
        if ((b->flags & RB_BOX_FIXED) && !(b->flags & RB_BOX_POS)) abort();
    }
    for (int y = 0; y < 400; y += 11)
        for (int x = 0; x < 320; x += 17) { rb_hit(x, y); rb_hit_at(x, y, y); }
    for (int node = 1; node < 200; node++) {
        rb_style_position(node); rb_style_overflow(node); rb_style_visibility(node);
        rb_style_cursor(node); rb_style_opacity(node);
        int a; rb_style_zindex(node, &a);
    }
    rb_page_height();
    canary_stable("exercise_c1");
}

/* PART C2/C3: random trees of grids, borders, auto margins and text styling
   with hostile values, checked against structural invariants. */
static void exercise_c2(void) {
    static const char *cols[] = {
        "repeat(3,1fr)", "50px 1fr 2fr", "repeat(99,1fr)", "repeat(auto-fill,minmax(40px,1fr))",
        "25% auto 10px", "1fr", "repeat(2,minmax(10px,1fr)) 30px", "repeat(0,1fr)", "bogus",
        "repeat(auto-fit,1fr)", "9999px 9999px", "0.5fr 1.5fr"
    };
    static const char *lines[] = {"1", "2", "-1", "span 2", "1 / 3", "2 / span 3", "-2 / -1",
                                  "0", "99", "span 99", "3 / 1", "auto"};
    static const char *ws[] = {"normal", "nowrap", "pre", "pre-wrap"};
    char buf[8192];
    int n = 0;
    FX_APPEND("<body>");
    int depth = 0;
    int elems = 4 + (int)(rnd() % 40);
    for (int e = 0; e < elems; e++) {
        int r = rnd() % 8;
        if (r == 0 && depth > 0) { FX_APPEND("</div>"); depth--; continue; }
        if (r == 1) { FX_APPEND("some wrapping words to flow around "); continue; }
        if (r == 2) { FX_APPEND("<a href=\"/x\">lnk</a><b>bold</b>"); continue; }
        FX_APPEND("<div style=\"");
        if (rnd() % 3 == 0) {
            FX_APPEND("display:grid;");
            if (rnd() % 4) FX_APPEND("grid-template-columns:%s;", cols[rnd() % 12]);
            if (rnd() % 3 == 0) FX_APPEND("gap:%dpx;", (int)(rnd() % 30));
            if (rnd() % 4 == 0) FX_APPEND("justify-items:center;align-items:center;");
        }
        if (rnd() % 3 == 0) FX_APPEND("grid-column:%s;", lines[rnd() % 12]);
        if (rnd() % 3 == 0) FX_APPEND("grid-row:%s;", lines[rnd() % 12]);
        if (rnd() % 3 == 0) FX_APPEND("border:%dpx solid #%02x%02x%02x;", (int)(rnd() % 12), (int)(rnd() & 255), (int)(rnd() & 255), (int)(rnd() & 255));
        if (rnd() % 5 == 0) FX_APPEND("border-bottom:%dpx solid;", (int)(rnd() % 6));
        if (rnd() % 3 == 0) FX_APPEND("border-radius:%dpx;", (int)(rnd() % 300));
        if (rnd() % 3 == 0) FX_APPEND("margin:0 auto;");
        if (rnd() % 4 == 0) FX_APPEND("box-sizing:%s;", (rnd() & 1) ? "border-box" : "content-box");
        if (rnd() % 4 == 0) FX_APPEND("font-weight:bold;text-decoration:underline;");
        if (rnd() % 4 == 0) FX_APPEND("text-transform:uppercase;letter-spacing:%dpx;", (int)(rnd() % 12));
        if (rnd() % 4 == 0) FX_APPEND("line-height:%d;", (int)(rnd() % 400));
        if (rnd() % 4 == 0) FX_APPEND("white-space:%s;", ws[rnd() % 4]);
        if (rnd() % 3 == 0) FX_APPEND("width:%dpx;", (int)(rnd() % 400));
        if (rnd() % 3 == 0) FX_APPEND("height:%dpx;", (int)(rnd() % 300));
        if (rnd() % 3 == 0) FX_APPEND("padding:%dpx;", (int)(rnd() % 30));
        if (rnd() % 5 == 0) FX_APPEND("display:flex;");
        if (rnd() % 6 == 0) FX_APPEND("position:%s;top:%dpx;", (rnd() & 1) ? "absolute" : "relative", (int)(rnd() % 50));
        if (rnd() % 2) FX_APPEND("background:#%02x%02x%02x;", (int)(rnd() & 255), (int)(rnd() & 255), (int)(rnd() & 255));
        FX_APPEND("\">");
        depth++;
    }
    while (depth-- > 0) FX_APPEND("</div>");
    buf[n] = 0;
    rb_open(buf, n);
    rb_relayout();
    int bc = rb_box_count();
    for (int i = 0; i < bc; i++) {
        const RbBox *b = rb_box(i);
        if (b->x < -30000 || b->x > 30000 || b->y < -4000000 || b->y > 4000000 || b->w < 0 || b->h < 0) {
            fprintf(stderr, "fuzz: C2 box out of range: %d,%d %dx%d\n", b->x, b->y, b->w, b->h);
            abort();
        }
        if (b->kind == RB_BOX_BORDER && (b->scale == 0 || b->radius > 255)) abort();
        if ((b->kind == RB_BOX_BG || b->kind == RB_BOX_BORDER) && b->radius &&
            b->radius * 2 > (b->w > b->h ? b->w : b->h) + 1) {
            fprintf(stderr, "fuzz: radius %d larger than box %dx%d\n", b->radius, b->w, b->h);
            abort();
        }
    }
    for (int y = 0; y < 400; y += 13)
        for (int x = 0; x < 320; x += 19) rb_hit_at(x, y, y / 3);
    rb_page_height();
    canary_stable("exercise_c2");
}

static void exercise(const char *buf, int n) {
    char compact[16384];
    rb_compact_markup(buf, n, compact, (int)sizeof(compact));
    rb_open(compact, (int)strlen(compact));
    rb_open(buf, n);

    int boxes = rb_box_count();
    for (int i = 0; i < boxes; i++) {
        const RbBox *b = rb_box(i);
        if (b) {
            volatile int z = b->x + b->y + b->w + b->h + b->color + b->bg;
            (void)z;
        }
        int len = 0;
        const char *t = rb_box_text(i, &len);
        if (t && len > 0) {
            volatile char c = t[len - 1];
            (void)c;
        }
    }
    int fc = rb_focus_count();
    for (int i = 0; i < fc; i++) {
        int node = rb_focus_node(i);
        char out[512];
        rb_node_href(node, out, (int)sizeof(out));
        rb_collect_text(node, out, (int)sizeof(out));
        rb_node_tag(node);
        rb_node_color(node);
        if (rb_is_field(node)) {
            rb_type_char(node, 'q');
            rb_backspace(node);
        }
        rb_activate(node);
        char nav[512];
        rb_take_nav(nav, (int)sizeof(nav));
    }
    for (int y = 0; y < 400; y += 7)
        for (int x = 0; x < 320; x += 13)
            rb_hit(x, y);
    rb_page_height();
    rb_page_bg();
    char title[80];
    rb_title(title, (int)sizeof(title));
    rb_relayout();

    char out[384];
    rb_resolve("/home/user/a.html", "sub/b.html", out, (int)sizeof(out));
    rb_resolve("http://ex.com/a/b", "../c?x=1#f", out, (int)sizeof(out));
    rb_resolve("/a", "file://-/home/x", out, (int)sizeof(out));
    rb_resolve("http://ex.com/", "//other/p", out, (int)sizeof(out));
}

int main(int argc, char **argv) {
    int iters = argc > 1 ? atoi(argv[1]) : 20000;
    if (argc > 2) rng = strtoull(argv[2], 0, 0);
    char buf[8192];
    for (int it = 0; it < iters; it++) {
        int mode = rnd() % 4;
        int n = 0;
        if (mode == 0) {
            n = rnd() % (int)sizeof(buf);
            for (int i = 0; i < n; i++) buf[i] = (char)(rnd() & 0xff);
        } else if (mode == 3) {
            int depth = 1 + (int)(rnd() % 300);
            while (depth-- > 0 && n < (int)sizeof(buf) - 8) {
                memcpy(buf + n, "<div>", 5);
                n += 5;
            }
            depth = 1 + (int)(rnd() % 300);
            while (depth-- > 0 && n < (int)sizeof(buf) - 8) {
                memcpy(buf + n, "</div>", 6);
                n += 6;
            }
        } else {
            int want = rnd() % (int)sizeof(buf);
            while (n < want) {
                const char *t = toks[rnd() % (sizeof(toks) / sizeof(toks[0]))];
                int tl = (int)strlen(t);
                if (n + tl > (int)sizeof(buf)) break;
                memcpy(buf + n, t, tl);
                n += tl;
            }
        }
        exercise(buf, n);
        if ((it % 3) == 0) exercise_dom();
        if ((it % 3) == 1) exercise_events();
        if ((it % 4) == 0) exercise_flex();
        if ((it % 2) == 0) exercise_c1();
        if ((it % 2) == 1) exercise_c2();
        if ((it % 50) == 0) exercise_resolve();
        if ((it % 25) == 0) exercise_compact(buf, n);
    }
    printf("fuzz done\n");
    return 0;
}
