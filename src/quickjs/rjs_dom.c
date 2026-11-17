/* rjs_dom.c - native half of the browser DOM (see rjs_dom.h). Each __rb.*
 * function maps onto one engine call; node handles cross as plain ints and
 * the JS prelude wraps them. Anything that changes the tree marks the page
 * dirty (the host relayouts) and stale (geometry reads relayout first). */
#include "quickjs.h"
#include "rjs.h"
#include "rjs_internal.h"
#include "rjs_dom.h"
#include "../rbrowse/engine.h"

extern volatile uint32_t ticks;
extern const char rjs_dom_prelude[];
extern const int rjs_dom_prelude_len;
void rjs_canvas_install(JSContext *ctx, JSValueConst rb);   /* rjs_canvas.c */

#define SCRATCH 65536
#define TIMER_BUDGET_MS 3000
#define SCRIPT_BUDGET_MS 5000

static char g_scratch[SCRATCH];
static char g_url[1024];
static RjsFetch g_fetch;
static int g_dirty;
static int g_stale;
static int g_next_due = -1;     /* ms timestamp of the earliest timer, -1 none */
/* Relayouts a geometry read may still trigger in the current entry into JS.
   A full relayout of a big page takes seconds in the kernel; scripts that
   alternate mutations and offsetWidth reads would otherwise relayout per
   read. Past the budget they get the last layout's (slightly stale) boxes. */
static int g_geom_budget;
#define GEOM_RELAYOUTS_PER_ENTRY 1

static const char *const g_ev_names[RB_EV_COUNT] = {
    "click", "mousedown", "mouseup", "mouseover", "mouseout",
    "input", "change", "submit", "focus", "blur",
    "keydown", "keyup", "keypress", "scroll", "load",
    "mousemove",
};

static void mutated(void) {
    g_dirty = 1;
    g_stale = 1;
}

/* ---- argument helpers ---- */

static int arg_int(JSContext *ctx, JSValueConst v) {
    int32_t i = 0;
    JS_ToInt32(ctx, &i, v);
    return i;
}

/* A string argument as (pointer,len); free with JS_FreeCString. */
static const char *arg_str(JSContext *ctx, JSValueConst v, int *len) {
    size_t n = 0;
    const char *s = JS_ToCStringLen(ctx, &n, v);
    *len = (int)n;
    return s;
}

/* Strings the engine writes snprintf-style (returns the full length): try
   the scratch buffer, retry on the JS heap when it was too small. */
typedef int (*GetFn)(int node, int a, char *out, int cap);

static JSValue get_string(JSContext *ctx, GetFn fn, int node, int a) {
    int n = fn(node, a, g_scratch, SCRATCH);
    if (n < 0) return JS_NULL;
    if (n < SCRATCH) return JS_NewStringLen(ctx, g_scratch, (size_t)(n < SCRATCH - 1 ? n : SCRATCH - 1));
    char *big = js_malloc(ctx, (size_t)n + 1);
    if (!big) return JS_EXCEPTION;
    int m = fn(node, a, big, n + 1);
    JSValue r = JS_NewStringLen(ctx, big, (size_t)(m < n ? m : n));
    js_free(ctx, big);
    return r;
}

static int get_text_fn(int node, int a, char *out, int cap) { (void)a; return rb_dom_text_content(node, out, cap); }
static int get_html_fn(int node, int a, char *out, int cap) { return rb_dom_get_html(node, a, out, cap); }
static int get_value_fn(int node, int a, char *out, int cap) { (void)a; return rb_dom_get_value(node, out, cap); }
static int get_tag_fn(int node, int a, char *out, int cap) { (void)a; return rb_dom_tag_name(node, out, cap); }

#define FN(name) static JSValue name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
#define UNUSED (void)this_val; (void)argc
#define N(i) arg_int(ctx, argv[i])
#define INT(x) JS_NewInt32(ctx, (x))

/* ---- tree ---- */

FN(n_tag) { UNUSED; return get_string(ctx, get_tag_fn, N(0), 0); }
FN(n_is_text) { UNUSED; return JS_NewBool(ctx, rb_dom_is_text(N(0))); }
FN(n_parent) { UNUSED; return INT(rb_dom_parent(N(0))); }
FN(n_first) { UNUSED; return INT(rb_dom_first(N(0))); }
FN(n_last) { UNUSED; return INT(rb_dom_last(N(0))); }
FN(n_next) { UNUSED; return INT(rb_dom_next(N(0))); }
FN(n_prev) { UNUSED; return INT(rb_dom_prev(N(0))); }
FN(n_gen) { UNUSED; return INT(rb_dom_generation(N(0))); }

FN(n_create) {
    UNUSED;
    int len;
    const char *s = arg_str(ctx, argv[0], &len);
    if (!s) return JS_EXCEPTION;
    int node = rb_dom_create_element_named(s, len);
    JS_FreeCString(ctx, s);
    return INT(node);
}

FN(n_create_text) {
    UNUSED;
    int len;
    const char *s = arg_str(ctx, argv[0], &len);
    if (!s) return JS_EXCEPTION;
    int node = rb_dom_create_text(s, len);
    JS_FreeCString(ctx, s);
    return INT(node);
}

FN(n_root) { UNUSED; return INT(rb_dom_root(N(0))); }
FN(n_release) { UNUSED; return JS_NewBool(ctx, rb_dom_release(N(0))); }
/* gc(): run the cycle collector now, so wrappers of detached nodes that no
   script can reach are finalized (the prelude then releases their nodes). */
FN(n_gc) { UNUSED; (void)argv; JS_RunGC(JS_GetRuntime(ctx)); return JS_UNDEFINED; }
/* liveNodes(): engine node slots in use. */
FN(n_live_nodes) { UNUSED; (void)argv; return INT(rb_debug_stat(13)); }

FN(n_append) { UNUSED; mutated(); return JS_NewBool(ctx, rb_dom_append_child(N(0), N(1))); }
FN(n_insert) { UNUSED; mutated(); return JS_NewBool(ctx, rb_dom_insert_before(N(0), N(1), N(2))); }
FN(n_replace) { UNUSED; mutated(); return JS_NewBool(ctx, rb_dom_replace_child(N(0), N(1), N(2))); }
FN(n_detach) { UNUSED; mutated(); return JS_NewBool(ctx, rb_dom_detach(N(0))); }

/* ---- text, markup, attributes ---- */

FN(n_text) { UNUSED; return get_string(ctx, get_text_fn, N(0), 0); }

FN(n_set_text) {
    UNUSED;
    int len;
    const char *s = arg_str(ctx, argv[1], &len);
    if (!s) return JS_EXCEPTION;
    mutated();
    int ok = rb_dom_set_text(N(0), s, len);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, ok);
}

/* A <script>'s body: the engine keeps it out of the tree (in the script
   table), but pages read JSON and templates from script elements. */
FN(n_script_text) {
    UNUSED;
    int want = N(0);
    for (int i = 0; i < rb_script_count(); i++) {
        int node, len;
        const char *text;
        if (rb_script_get(i, &node, &text, &len) && node == want)
            return JS_NewStringLen(ctx, text ? text : "", (size_t)(len > 0 ? len : 0));
    }
    return JS_NewString(ctx, "");
}

FN(n_html) { UNUSED; return get_string(ctx, get_html_fn, N(0), N(1)); }

FN(n_set_html) {
    UNUSED;
    int len;
    const char *s = arg_str(ctx, argv[1], &len);
    if (!s) return JS_EXCEPTION;
    mutated();
    int ok = rb_dom_set_inner_html(N(0), s, len);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, ok);
}

FN(n_adj_html) {
    UNUSED;
    int len;
    const char *s = arg_str(ctx, argv[2], &len);
    if (!s) return JS_EXCEPTION;
    mutated();
    int ok = rb_dom_insert_adjacent_html(N(0), N(1), s, len);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, ok);
}

FN(n_get_attr) {
    UNUSED;
    int nl;
    const char *name = arg_str(ctx, argv[1], &nl);
    if (!name) return JS_EXCEPTION;
    int node = N(0);
    JSValue r = JS_NULL;
    /* get_attr answers 1/0 and NUL-terminates; attr_len is the length
       (values are capped at 4000 bytes, well inside the scratch buffer) */
    int n = rb_dom_attr_len(node, name, nl);
    if (n >= 0 && rb_dom_get_attr(node, name, nl, g_scratch, SCRATCH)) {
        if (n > SCRATCH - 1) n = SCRATCH - 1;
        r = JS_NewStringLen(ctx, g_scratch, (size_t)n);
    }
    JS_FreeCString(ctx, name);
    return r;
}

FN(n_set_attr) {
    UNUSED;
    int nl, vl;
    const char *name = arg_str(ctx, argv[1], &nl);
    if (!name) return JS_EXCEPTION;
    const char *val = arg_str(ctx, argv[2], &vl);
    if (!val) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    mutated();
    int ok = rb_dom_set_attr(N(0), name, nl, val, vl);
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, val);
    return JS_NewBool(ctx, ok);
}

FN(n_has_attr) {
    UNUSED;
    int nl;
    const char *name = arg_str(ctx, argv[1], &nl);
    if (!name) return JS_EXCEPTION;
    int r = rb_dom_has_attr(N(0), name, nl);
    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, r);
}

FN(n_remove_attr) {
    UNUSED;
    int nl;
    const char *name = arg_str(ctx, argv[1], &nl);
    if (!name) return JS_EXCEPTION;
    mutated();
    int r = rb_dom_remove_attr(N(0), name, nl);
    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, r);
}

/* [[name, value], ...] */
FN(n_attrs) {
    UNUSED;
    int node = N(0);
    int count = rb_dom_attr_count(node);
    JSValue arr = JS_NewArray(ctx);
    static char name[256];
    for (int i = 0; i < count; i++) {
        if (!rb_dom_attr_at(node, i, name, (int)sizeof(name), g_scratch, SCRATCH)) break;
        JSValue pair = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, pair, 0, JS_NewString(ctx, name));
        JS_SetPropertyUint32(ctx, pair, 1, JS_NewString(ctx, g_scratch));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, pair);
    }
    return arr;
}

FN(n_value) { UNUSED; return get_string(ctx, get_value_fn, N(0), 0); }

FN(n_set_value) {
    UNUSED;
    int len;
    const char *s = arg_str(ctx, argv[1], &len);
    if (!s) return JS_EXCEPTION;
    mutated();
    int ok = rb_dom_set_value(N(0), s, len);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, ok);
}

FN(n_checked) { UNUSED; return JS_NewBool(ctx, rb_dom_get_checked(N(0))); }
FN(n_set_checked) { UNUSED; mutated(); return JS_NewBool(ctx, rb_dom_set_checked(N(0), N(1))); }

/* ---- selectors ---- */

/* Every node of a page can match: the engine holds at most 16384 (MAX_NODES). */
#define QUERY_MAX 16384
static int g_hits[QUERY_MAX];

FN(n_query) {
    UNUSED;
    int len;
    const char *sel = arg_str(ctx, argv[1], &len);
    if (!sel) return JS_EXCEPTION;
    int max = argc > 2 ? N(2) : QUERY_MAX;
    if (max <= 0 || max > QUERY_MAX) max = QUERY_MAX;
    int n = rb_dom_query(N(0), sel, len, g_hits, max);
    if (n < 0) {
        JSValue e = JS_ThrowSyntaxError(ctx, "'%s' is not a valid selector", sel);
        JS_FreeCString(ctx, sel);
        return e;
    }
    JS_FreeCString(ctx, sel);
    if (n > max) n = max;
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < n; i++) JS_SetPropertyUint32(ctx, arr, (uint32_t)i, INT(g_hits[i]));
    return arr;
}

static JSValue sel_call(JSContext *ctx, JSValueConst *argv, int (*fn)(int, const char *, int), int as_bool) {
    int len;
    const char *sel = arg_str(ctx, argv[1], &len);
    if (!sel) return JS_EXCEPTION;
    int r = fn(arg_int(ctx, argv[0]), sel, len);
    if (r < 0) {
        JSValue e = JS_ThrowSyntaxError(ctx, "'%s' is not a valid selector", sel);
        JS_FreeCString(ctx, sel);
        return e;
    }
    JS_FreeCString(ctx, sel);
    return as_bool ? JS_NewBool(ctx, r) : INT(r);
}

FN(n_matches) { UNUSED; return sel_call(ctx, argv, rb_dom_matches, 1); }
FN(n_closest) { UNUSED; return sel_call(ctx, argv, rb_dom_closest, 0); }

FN(n_by_id) {
    UNUSED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    int node = rb_find_id(s);
    JS_FreeCString(ctx, s);
    return INT(node > 0 ? node : 0);
}

/* ---- geometry: relayout first if scripts changed the tree ---- */

static void fresh_layout(void) {
    if (g_stale && g_geom_budget > 0) {
        g_geom_budget--;
        rb_relayout();
        g_stale = 0;
    }
}

FN(n_rect) {
    UNUSED;
    fresh_layout();
    int x, y, w, h;
    if (!rb_dom_rect(N(0), &x, &y, &w, &h)) return JS_NULL;
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, INT(x));
    JS_SetPropertyUint32(ctx, arr, 1, INT(y));
    JS_SetPropertyUint32(ctx, arr, 2, INT(w));
    JS_SetPropertyUint32(ctx, arr, 3, INT(h));
    return arr;
}

FN(n_page_height) { UNUSED; (void)argv; fresh_layout(); return INT(rb_page_height()); }

/* img(node): [state, naturalWidth, naturalHeight, laidOut]. state is the
   engine's RB_IMG_* record (0 not loaded yet, 1 decoded, 2 broken, 3 no
   source); laidOut is 1 when the element has a box in the current layout. */
FN(n_img) {
    UNUSED;
    int node = N(0), nw = 0, nh = 0;
    int st = rb_dom_img_state(node, 0, &nw, &nh);
    if (st != RB_IMG_OK) nw = nh = 0;
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, INT(st));
    JS_SetPropertyUint32(ctx, arr, 1, INT(nw));
    JS_SetPropertyUint32(ctx, arr, 2, INT(nh));
    return arr;
}

/* ---- events ---- */

/* Engine listener: hand the event to the prelude's dispatcher. */
static void on_event(int type, int target, int current, int capture) {
    JSContext *ctx = rjs_context();
    if (!ctx || type < 0 || type >= RB_EV_COUNT) return;
    rjs_enter(TIMER_BUDGET_MS);
    g_geom_budget = GEOM_RELAYOUTS_PER_ENTRY;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fire = JS_GetPropertyStr(ctx, global, "__rb_fire");
    if (JS_IsFunction(ctx, fire)) {
        JSValue args[4] = {
            JS_NewString(ctx, g_ev_names[type]), INT(target), INT(current), JS_NewBool(ctx, capture)
        };
        JSValue r = JS_Call(ctx, fire, global, 4, args);
        if (JS_IsException(r)) rjs_report();
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, args[0]);
    }
    JS_FreeValue(ctx, fire);
    JS_FreeValue(ctx, global);
    rjs_run_jobs();
    rjs_leave();
}

static void on_bubble(int type, int target, int current, void *user) {
    (void)user;
    on_event(type, target, current, 0);
}

static void on_capture(int type, int target, int current, void *user) {
    (void)user;
    on_event(type, target, current, 1);
}

static int ev_type(JSContext *ctx, JSValueConst v) {
    const char *s = JS_ToCString(ctx, v);
    if (!s) return -1;
    int t = -1;
    for (int i = 0; i < RB_EV_COUNT; i++) {
        const char *a = g_ev_names[i], *b = s;
        while (*a && *a == *b) a++, b++;
        if (!*a && !*b) { t = i; break; }
    }
    JS_FreeCString(ctx, s);
    return t;
}

/* listen(node, type, capture): 1 registered, 0 the engine does not produce
   that event type, -1 it does but the registration failed (listener table
   full, or a dead node). */
FN(n_listen) {
    UNUSED;
    int t = ev_type(ctx, argv[1]);
    if (t < 0) return INT(0);
    int ok = N(2) ? rb_listen_capture(N(0), t, on_capture, 0) : rb_listen(N(0), t, on_bubble, 0);
    return INT(ok ? 1 : -1);
}

/* dispatch(node, type): run an engine event (el.click()); -1 if the engine
   has no such type (the prelude then dispatches in JS only). */
FN(n_dispatch) {
    UNUSED;
    int t = ev_type(ctx, argv[1]);
    if (t < 0) return INT(-1);
    int r = rb_dispatch(N(0), t);
    if (r) mutated();
    return INT(r);
}

FN(n_ev_prevent) { UNUSED; (void)ctx; (void)argv; rb_event_prevent(); return JS_UNDEFINED; }
FN(n_ev_stop) { UNUSED; (void)ctx; (void)argv; rb_event_stop(); return JS_UNDEFINED; }
FN(n_ev_stop_imm) { UNUSED; (void)ctx; (void)argv; rb_event_stop_immediate(); return JS_UNDEFINED; }

/* [key, x, y, button, buttons, mods, prevented] of the engine event being dispatched */
FN(n_ev_info) {
    UNUSED;
    (void)argv;
    JSValue arr = JS_NewArray(ctx);
    int v[7] = {rb_event_key(), rb_event_x(), rb_event_y(), rb_event_button(), rb_event_buttons(),
                rb_event_mods(), rb_event_prevented()};
    for (int i = 0; i < 7; i++) JS_SetPropertyUint32(ctx, arr, (uint32_t)i, INT(v[i]));
    return arr;
}

FN(n_focus) { UNUSED; mutated(); return JS_NewBool(ctx, rb_dom_focus(N(0))); }
FN(n_blur) { UNUSED; (void)argv; mutated(); return JS_NewBool(ctx, rb_dom_blur()); }
FN(n_active) { UNUSED; (void)argv; return INT(rb_dom_active_element()); }

/* ---- page, time, network ---- */

FN(n_url) { UNUSED; (void)argv; return JS_NewString(ctx, g_url); }

FN(n_title) {
    UNUSED;
    (void)argv;
    int n = rb_title(g_scratch, SCRATCH);
    return JS_NewStringLen(ctx, g_scratch, (size_t)(n > 0 ? n : 0));
}

/* resolve(href) against the page URL; null if it does not resolve. */
FN(n_resolve) {
    UNUSED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    int ok = rb_resolve(g_url, s, g_scratch, SCRATCH);
    JS_FreeCString(ctx, s);
    return ok ? JS_NewString(ctx, g_scratch) : JS_NULL;
}

FN(n_nav) {
    UNUSED;
    int len;
    const char *s = arg_str(ctx, argv[0], &len);
    if (!s) return JS_EXCEPTION;
    rb_request_nav(s, len);
    JS_FreeCString(ctx, s);
    g_dirty = 1;
    return JS_UNDEFINED;
}

FN(n_now) { UNUSED; (void)argv; return JS_NewFloat64(ctx, (double)ticks); }

/* view(0): the page viewport width in CSS px; view(1): its height in CSS px
   (the web profile halves vertical lengths, so two per screen row). */
FN(n_view) { UNUSED; return INT(N(0) ? rb_view_height() * 2 : rb_view_width()); }

FN(n_set_next_due) {
    UNUSED;
    double d = -1;
    JS_ToFloat64(ctx, &d, argv[0]);
    g_next_due = d < 0 ? -1 : (int)d;
    return JS_UNDEFINED;
}

/* Length of "scheme://host[:port]" at the start of an http(s) URL, 0 if it
   is not one. Scheme and host compare case-insensitively. */
static int origin_len(const char *u) {
    int i = 0;
    if ((u[0] | 32) != 'h' || (u[1] | 32) != 't' || (u[2] | 32) != 't' || (u[3] | 32) != 'p') return 0;
    i = 4;
    if ((u[i] | 32) == 's') i++;
    if (u[i] != ':' || u[i + 1] != '/' || u[i + 2] != '/') return 0;
    i += 3;
    while (u[i] && u[i] != '/' && u[i] != '?' && u[i] != '#') i++;
    return i;
}

static int same_origin(const char *a, const char *b) {
    int na = origin_len(a), nb = origin_len(b);
    if (!na || na != nb) return 0;
    for (int i = 0; i < na; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}

/* fetch(absoluteUrl): the body as a string, or null. Blocking. Same-origin
   only: there is no CORS, so a page may read nothing but its own server
   (not other sites, not the local network, never local files). */
#define FETCH_MAX (1024 * 1024)
static char g_fetch_buf[FETCH_MAX];

FN(n_fetch) {
    UNUSED;
    const char *u = JS_ToCString(ctx, argv[0]);
    if (!u) return JS_EXCEPTION;
    int n = (g_fetch && same_origin(u, g_url)) ? g_fetch(u, g_fetch_buf, FETCH_MAX, 1) : -1;
    JS_FreeCString(ctx, u);
    if (n < 0) return JS_NULL;
    return JS_NewStringLen(ctx, g_fetch_buf, (size_t)n);
}

/* loadScript(absoluteUrl): source of a script a page inserted, or null.
   Like <script src>, any http(s) origin may serve it (it runs as the page's
   own code, nothing is read back cross-origin); never local files. */
FN(n_load_script) {
    UNUSED;
    const char *u = JS_ToCString(ctx, argv[0]);
    if (!u) return JS_EXCEPTION;
    int n = (g_fetch && origin_len(u)) ? g_fetch(u, g_fetch_buf, FETCH_MAX, 0) : -1;
    JS_FreeCString(ctx, u);
    if (n < 0) return JS_NULL;
    return JS_NewStringLen(ctx, g_fetch_buf, (size_t)n);
}

/* ---- crypto.getRandomValues: hardware RNG or nothing ---- */

static int hw_random32(uint32_t *out) {
#if defined(__i386__) || defined(__x86_64__)
    static int have = -1;
    if (have < 0) {
        uint32_t a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
        have = (c >> 30) & 1;   /* RDRAND */
    }
    if (!have) return 0;
    for (int tries = 0; tries < 10; tries++) {
        uint32_t v;
        uint8_t ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) {
            *out = v;
            return 1;
        }
    }
    return 0;
#elif defined(__APPLE__)
    extern uint32_t arc4random(void);
    *out = arc4random();   /* host test build */
    return 1;
#else
    (void)out;
    return 0;
#endif
}

/* randomFill(typedArray): fill its bytes; throws if there is no hardware
   RNG (weak numbers must not pass for cryptographic ones). */
FN(n_random_fill) {
    UNUSED;
    size_t off, len, per;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &len, &per);
    if (JS_IsException(buf)) return buf;
    size_t size;
    uint8_t *p = JS_GetArrayBuffer(ctx, &size, buf);
    JS_FreeValue(ctx, buf);
    if (!p || off + len > size) return JS_ThrowTypeError(ctx, "getRandomValues: bad array");
    if (len > 65536) return JS_ThrowRangeError(ctx, "getRandomValues: more than 65536 bytes");
    for (size_t i = 0; i < len; i += 4) {
        uint32_t r;
        if (!hw_random32(&r)) return JS_ThrowInternalError(ctx, "crypto: no hardware random number generator");
        for (size_t k = 0; k < 4 && i + k < len; k++) p[off + i + k] = (uint8_t)(r >> (8 * k));
    }
    return JS_DupValue(ctx, argv[0]);
}

static const JSCFunctionListEntry g_natives[] = {
    JS_CFUNC_DEF("randomFill", 1, n_random_fill),
    JS_CFUNC_DEF("tag", 1, n_tag),
    JS_CFUNC_DEF("isText", 1, n_is_text),
    JS_CFUNC_DEF("parent", 1, n_parent),
    JS_CFUNC_DEF("first", 1, n_first),
    JS_CFUNC_DEF("last", 1, n_last),
    JS_CFUNC_DEF("next", 1, n_next),
    JS_CFUNC_DEF("prev", 1, n_prev),
    JS_CFUNC_DEF("gen", 1, n_gen),
    JS_CFUNC_DEF("create", 1, n_create),
    JS_CFUNC_DEF("createText", 1, n_create_text),
    JS_CFUNC_DEF("append", 2, n_append),
    JS_CFUNC_DEF("insert", 3, n_insert),
    JS_CFUNC_DEF("replace", 3, n_replace),
    JS_CFUNC_DEF("detach", 1, n_detach),
    JS_CFUNC_DEF("root", 1, n_root),
    JS_CFUNC_DEF("release", 1, n_release),
    JS_CFUNC_DEF("gc", 0, n_gc),
    JS_CFUNC_DEF("liveNodes", 0, n_live_nodes),
    JS_CFUNC_DEF("text", 1, n_text),
    JS_CFUNC_DEF("setText", 2, n_set_text),
    JS_CFUNC_DEF("html", 2, n_html),
    JS_CFUNC_DEF("scriptText", 1, n_script_text),
    JS_CFUNC_DEF("setHtml", 2, n_set_html),
    JS_CFUNC_DEF("adjHtml", 3, n_adj_html),
    JS_CFUNC_DEF("getAttr", 2, n_get_attr),
    JS_CFUNC_DEF("setAttr", 3, n_set_attr),
    JS_CFUNC_DEF("hasAttr", 2, n_has_attr),
    JS_CFUNC_DEF("removeAttr", 2, n_remove_attr),
    JS_CFUNC_DEF("attrs", 1, n_attrs),
    JS_CFUNC_DEF("value", 1, n_value),
    JS_CFUNC_DEF("setValue", 2, n_set_value),
    JS_CFUNC_DEF("checked", 1, n_checked),
    JS_CFUNC_DEF("setChecked", 2, n_set_checked),
    JS_CFUNC_DEF("query", 3, n_query),
    JS_CFUNC_DEF("matches", 2, n_matches),
    JS_CFUNC_DEF("closest", 2, n_closest),
    JS_CFUNC_DEF("byId", 1, n_by_id),
    JS_CFUNC_DEF("rect", 1, n_rect),
    JS_CFUNC_DEF("pageHeight", 0, n_page_height),
    JS_CFUNC_DEF("img", 1, n_img),
    JS_CFUNC_DEF("listen", 3, n_listen),
    JS_CFUNC_DEF("dispatch", 2, n_dispatch),
    JS_CFUNC_DEF("evPrevent", 0, n_ev_prevent),
    JS_CFUNC_DEF("evStop", 0, n_ev_stop),
    JS_CFUNC_DEF("evStopImm", 0, n_ev_stop_imm),
    JS_CFUNC_DEF("evInfo", 0, n_ev_info),
    JS_CFUNC_DEF("focus", 1, n_focus),
    JS_CFUNC_DEF("blur", 0, n_blur),
    JS_CFUNC_DEF("active", 0, n_active),
    JS_CFUNC_DEF("url", 0, n_url),
    JS_CFUNC_DEF("title", 0, n_title),
    JS_CFUNC_DEF("resolve", 1, n_resolve),
    JS_CFUNC_DEF("nav", 1, n_nav),
    JS_CFUNC_DEF("now", 0, n_now),
    JS_CFUNC_DEF("view", 1, n_view),
    JS_CFUNC_DEF("setNextDue", 1, n_set_next_due),
    JS_CFUNC_DEF("fetch", 1, n_fetch),
    JS_CFUNC_DEF("loadScript", 1, n_load_script),
};

/* ---- host API ---- */

static int call_global(const char *fn, int argc, JSValue *argv, int budget_ms) {
    JSContext *ctx = rjs_context();
    if (!ctx) return -1;
    rjs_enter(budget_ms);
    g_geom_budget = GEOM_RELAYOUTS_PER_ENTRY;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue f = JS_GetPropertyStr(ctx, global, fn);
    int rc = 0;
    if (JS_IsFunction(ctx, f)) {
        JSValue r = JS_Call(ctx, f, global, argc, argv);
        if (JS_IsException(r)) {
            rjs_report();
            rc = -1;
        }
        JS_FreeValue(ctx, r);
    }
    for (int i = 0; i < argc; i++) JS_FreeValue(ctx, argv[i]);
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, global);
    if (rjs_run_jobs() == RJS_EXCEPTION) rc = -1;
    rjs_leave();
    return rc;
}

int rjs_dom_install(const char *url, RjsFetch fetch) {
    JSContext *ctx = rjs_context();
    if (!ctx) return -1;
    int i = 0;
    for (; url && url[i] && i < (int)sizeof(g_url) - 1; i++) g_url[i] = url[i];
    g_url[i] = 0;
    g_fetch = fetch;
    g_dirty = 0;
    g_stale = 0;
    g_next_due = -1;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue rb = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, rb, g_natives, (int)(sizeof(g_natives) / sizeof(g_natives[0])));
    rjs_canvas_install(ctx, rb);
    JS_SetPropertyStr(ctx, global, "__rb", rb);
    JS_FreeValue(ctx, global);
    rjs_enter(SCRIPT_BUDGET_MS);
    JSValue r = JS_Eval(ctx, rjs_dom_prelude, (size_t)rjs_dom_prelude_len, "<dom>", JS_EVAL_TYPE_GLOBAL);
    int rc = 0;
    if (JS_IsException(r)) {
        rjs_report();
        rc = -1;
    }
    JS_FreeValue(ctx, r);
    rjs_leave();
    return rc;
}

int rjs_dom_exec(int node, const char *code, int len, const char *name) {
    JSContext *ctx = rjs_context();
    if (!ctx) return -1;
    JSValue args[1] = {INT(node)};
    call_global("__rb_begin_script", 1, args, SCRIPT_BUDGET_MS);
    g_geom_budget = GEOM_RELAYOUTS_PER_ENTRY;
    int rc = rjs_eval(code, len, name, SCRIPT_BUDGET_MS, 0, 0);
    JSValue none[1] = {INT(0)};
    call_global("__rb_begin_script", 1, none, SCRIPT_BUDGET_MS);
    return rc == RJS_OK ? 0 : -1;
}

void rjs_dom_loaded(void) {
    call_global("__rb_loaded", 0, 0, SCRIPT_BUDGET_MS);
}

void rjs_dom_image_event(int node, int ok) {
    JSContext *ctx = rjs_context();
    if (!ctx) return;
    JSValue args[2] = {INT(node), JS_NewBool(ctx, ok)};
    call_global("__rb_img_event", 2, args, TIMER_BUDGET_MS);
}

int rjs_dom_next_due(void) {
    if (g_next_due < 0) return -1;
    int d = g_next_due - (int)ticks;
    return d < 0 ? 0 : d;
}

int rjs_dom_tick(void) {
    if (!rjs_context() || g_next_due < 0 || (int)ticks < g_next_due) return 0;
    JSValue args[1] = {JS_NewFloat64(rjs_context(), (double)ticks)};
    call_global("__rb_tick", 1, args, TIMER_BUDGET_MS);
    return 1;
}

int rjs_dom_take_dirty(void) {
    int d = g_dirty;
    g_dirty = 0;
    if (d) g_stale = 0;   /* the host relayouts now */
    return d;
}
