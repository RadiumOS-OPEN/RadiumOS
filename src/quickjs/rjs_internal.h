/* rjs_internal.h - for code that calls QuickJS directly on rjs's context
   (the DOM bindings). Every entry from C into JS goes between rjs_enter and
   rjs_leave: that re-anchors the stack-overflow check on the current stack
   and arms the time budget. */
#ifndef RJS_INTERNAL_H
#define RJS_INTERNAL_H

#include "quickjs.h"

JSContext *rjs_context(void);
void rjs_enter(int timeout_ms);
void rjs_leave(void);
/* Print the pending exception to the output sink and clear it. */
void rjs_report(void);
/* 1 when the script running now is past its time budget (or cancelled):
   long natives poll it and stop early; QuickJS then interrupts the script. */
int rjs_over_budget(void);

#endif
