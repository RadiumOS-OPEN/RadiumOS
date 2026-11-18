/* rjs.c - see rjs.h */
#include "quickjs.h"
#include "rjs.h"
#include "compat/qjs_alloc.h"
#include "compat/qjs_port.h"
#include "rjs_internal.h"

extern volatile uint32_t ticks;   /* 1 kHz PIT counter */

#define RJS_DEFAULT_TIMEOUT_MS 5000
#define RJS_STACK_LIMIT (512 * 1024)
#define RJS_SRC_MAX (4 * 1024 * 1024)

static JSRuntime *g_rt;
static JSContext *g_ctx;
static RjsOutput g_output;

/* Time budget of the eval in progress. checks is the fallback for a frozen
   clock (interrupts off): QuickJS polls about every 10k bytecodes. */
static uint32_t g_eval_start;
static uint32_t g_eval_budget;
static uint32_t g_checks;
static uint32_t g_check_cap;
static int (*g_cancel)(void);
/* How many rjs_enter()s are open. A native called from a script can re-enter
   JS (el.click() runs the engine's dispatch, whose listener is JS again):
   only the outermost entry anchors the stack check and owns the budget, so
   leaving the inner one neither disarms the script that is still running
   nor moves its stack limit. */
static int g_depth;

void rjs_set_cancel(int (*fn)(void)) {
    g_cancel = fn;
}

static int interrupt_cb(JSRuntime *rt, void *opaque) {
    (void)rt;
    (void)opaque;
    if (g_cancel && g_cancel()) return 1;
    if (!g_eval_budget) return 0;
    if (ticks - g_eval_start >= g_eval_budget) return 1;
    return ++g_checks > g_check_cap;
}

int rjs_over_budget(void) {
    if (g_cancel && g_cancel()) return 1;
    return g_eval_budget && ticks - g_eval_start >= g_eval_budget;
}

static void out(const char *s, int len) {
    qjs_write(s, len);
}

/* console.log and friends: arguments joined by spaces, then a newline. */
static JSValue js_console_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    for (int i = 0; i < argc; i++) {
        if (i) out(" ", 1);
        size_t len;
        const char *s = JS_ToCStringLen(ctx, &len, argv[i]);
        if (!s) return JS_EXCEPTION;
        out(s, (int)len);
        JS_FreeCString(ctx, s);
    }
    out("\n", 1);
    return JS_UNDEFINED;
}

static void install_globals(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    static const char *const names[] = {"log", "info", "warn", "error", "debug"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        JS_SetPropertyStr(ctx, console, names[i], JS_NewCFunction(ctx, js_console_log, names[i], 1));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, js_console_log, "print", 1));
    JS_FreeValue(ctx, global);
}

void rjs_set_output(RjsOutput fn) {
    g_output = fn;
    qjs_set_sink(fn);
}

int rjs_running(void) {
    return g_ctx != 0;
}

void rjs_stop(void) {
    if (g_ctx) JS_FreeContext(g_ctx);
    if (g_rt) JS_FreeRuntime(g_rt);
    g_ctx = 0;
    g_rt = 0;
    g_depth = 0;
    g_eval_budget = 0;
}

void rjs_abandon(void) {
    g_ctx = 0;
    g_rt = 0;
    g_depth = 0;        /* thrown away mid-call: the open entries went with it */
    g_eval_budget = 0;
    qjs_alloc_reset();
}

int rjs_start(void) {
    /* Drop any previous runtime without walking it (a page's leftovers can be
       large, and nothing else lives in the arena): start from a clean one. */
    g_ctx = 0;
    g_rt = 0;
    g_depth = 0;
    g_eval_budget = 0;
    qjs_alloc_reset();
    g_rt = JS_NewRuntime();
    if (!g_rt) return RJS_NO_MEMORY;
    size_t total;
    qjs_alloc_stats(0, 0, &total);
    JS_SetMemoryLimit(g_rt, total - total / 8);
    JS_SetMaxStackSize(g_rt, RJS_STACK_LIMIT);
    JS_SetInterruptHandler(g_rt, interrupt_cb, 0);
    g_ctx = JS_NewContext(g_rt);
    if (!g_ctx) {
        JS_FreeRuntime(g_rt);
        g_rt = 0;
        return RJS_NO_MEMORY;
    }
    install_globals(g_ctx);
    return RJS_OK;
}

static void put(char *dst, int cap, int *n, const char *s, int len) {
    if (!dst || cap <= 0) return;
    for (int i = 0; i < len && *n + 1 < cap; i++) dst[(*n)++] = s[i];
    dst[*n] = 0;
}

/* The pending exception as "Name: message" plus its stack, into `dst` or,
   without one, onto the output sink. */
static void report_exception(char *dst, int cap);

JSContext *rjs_context(void) {
    return g_ctx;
}

void rjs_report(void) {
    report_exception(0, 0);
}

static void report_exception(char *dst, int cap) {
    JSValue ex = JS_GetException(g_ctx);
    int n = 0;
    if (dst && cap > 0) dst[0] = 0;
    const char *msg = JS_ToCString(g_ctx, ex);
    const char *text = msg ? msg : "(exception)";
    if (dst) {
        put(dst, cap, &n, text, (int)strlen(text));
    } else {
        out("Uncaught ", 9);
        out(text, (int)strlen(text));
        out("\n", 1);
    }
    if (msg) JS_FreeCString(g_ctx, msg);
    if (JS_IsError(g_ctx, ex)) {
        JSValue st = JS_GetPropertyStr(g_ctx, ex, "stack");
        if (!JS_IsUndefined(st)) {
            const char *s = JS_ToCString(g_ctx, st);
            if (s && s[0]) {
                if (dst) {
                    put(dst, cap, &n, "\n", 1);
                    put(dst, cap, &n, s, (int)strlen(s));
                } else {
                    out(s, (int)strlen(s));
                }
            }
            if (s) JS_FreeCString(g_ctx, s);
        }
        JS_FreeValue(g_ctx, st);
    }
    JS_FreeValue(g_ctx, ex);
}

static void arm_budget(int timeout_ms) {
    g_eval_start = ticks;
    g_eval_budget = (uint32_t)(timeout_ms > 0 ? timeout_ms : RJS_DEFAULT_TIMEOUT_MS);
    g_checks = 0;
    g_check_cap = g_eval_budget * 200;
}

void rjs_enter(int timeout_ms) {
    if (g_depth++ > 0) return;      /* nested: the outer entry's budget and stack anchor stand */
    JS_UpdateStackTop(g_rt);
    arm_budget(timeout_ms);
}

void rjs_leave(void) {
    if (g_depth > 0 && --g_depth > 0) return;
    g_eval_budget = 0;
}

int rjs_run_jobs(void) {
    if (!g_rt) return RJS_NOT_STARTED;
    int ran = 0;
    for (;;) {
        JSContext *jctx;
        int r = JS_ExecutePendingJob(g_rt, &jctx);
        if (r == 0) break;
        if (r < 0) {
            report_exception(0, 0);
            return RJS_EXCEPTION;
        }
        ran++;
    }
    return ran;
}

int rjs_eval(const char *src, int len, const char *filename, int timeout_ms, char *dst, int cap) {
    if (!g_ctx) return RJS_NOT_STARTED;
    if (dst && cap > 0) dst[0] = 0;
    if (len < 0) len = (int)strlen(src);
    if (len > RJS_SRC_MAX) return RJS_NO_MEMORY;
    /* JS_Eval wants a NUL right after the source. */
    char *copy = js_malloc(g_ctx, (size_t)len + 1);
    if (!copy) return RJS_NO_MEMORY;
    memcpy(copy, src, (size_t)len);
    copy[len] = 0;
    /* The caller may be on a different stack than at rjs_start (the
       browser's layout stack): rjs_enter re-anchors the overflow check. */
    rjs_enter(timeout_ms);
    JSValue v = JS_Eval(g_ctx, copy, (size_t)len, filename ? filename : "<eval>", JS_EVAL_TYPE_GLOBAL);
    js_free(g_ctx, copy);
    int rc = RJS_OK;
    if (JS_IsException(v)) {
        report_exception(dst, cap);
        rc = RJS_EXCEPTION;
    } else if (dst && cap > 0) {
        size_t sl;
        const char *s = JS_ToCStringLen(g_ctx, &sl, v);
        int n = 0;
        if (s) {
            put(dst, cap, &n, s, (int)sl);
            JS_FreeCString(g_ctx, s);
        }
    }
    JS_FreeValue(g_ctx, v);
    if (rjs_run_jobs() == RJS_EXCEPTION && rc == RJS_OK) rc = RJS_EXCEPTION;
    rjs_leave();
    return rc;
}

void rjs_memory(unsigned *in_use, unsigned *peak, unsigned *total) {
    size_t a, b, c;
    qjs_alloc_stats(&a, &b, &c);
    if (in_use) *in_use = (unsigned)a;
    if (peak) *peak = (unsigned)b;
    if (total) *total = (unsigned)c;
}
