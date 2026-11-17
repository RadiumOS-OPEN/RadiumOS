/* rjs.h - RadiumOS's QuickJS embedding: one runtime, one context.
 *
 * Plain C types only, so kernel code can include it without the QuickJS
 * compat headers. QuickJS allocates from its own arena (qjs_alloc.c); a
 * runtime that died mid-call (CPU fault, watchdog) is dropped with
 * rjs_abandon(), which resets the arena instead of walking corrupt state.
 *
 * Scripts get console.log/info/warn/error and print(), all routed to the
 * output sink (the terminal unless rjs_set_output changes it). Each eval has
 * a time budget: past it, QuickJS throws "interrupted" at the next check, so
 * a runaway loop ends as an ordinary JS exception. The clock only advances
 * while interrupts are enabled.
 */
#ifndef RJS_H
#define RJS_H

typedef void (*RjsOutput)(const char *s, int len);

#define RJS_OK 0
#define RJS_EXCEPTION (-1)
#define RJS_NOT_STARTED (-2)
#define RJS_NO_MEMORY (-3)

/* Fresh runtime + context (stops any previous one). RJS_OK or RJS_NO_MEMORY. */
int rjs_start(void);
/* Free the runtime normally. */
void rjs_stop(void);
/* Forget the runtime without touching it, and reset the JS heap. */
void rjs_abandon(void);
int rjs_running(void);

/* Where console output and uncaught-error reports go (NULL: terminal). */
void rjs_set_output(RjsOutput fn);

/* Run src as a global script, then any queued promise jobs. timeout_ms 0
   means the default (5 s). On RJS_OK, `out` (if given) gets the completion
   value as a string; on RJS_EXCEPTION it gets the error and stack, and the
   report is also written to the output sink unless `out` is given. */
int rjs_eval(const char *src, int len, const char *filename, int timeout_ms,
             char *out, int cap);

/* Run queued promise jobs; returns how many ran, or RJS_EXCEPTION. */
int rjs_run_jobs(void);

/* JS heap usage in bytes. */
void rjs_memory(unsigned *in_use, unsigned *peak, unsigned *total);

#endif
