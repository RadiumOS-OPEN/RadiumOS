/* qjs_alloc.h - QuickJS's private heap (see qjs_alloc.c). */
#ifndef QJS_ALLOC_H
#define QJS_ALLOC_H

#include <stddef.h>

/* Forget every allocation and start over with one empty arena. */
void qjs_alloc_reset(void);
void qjs_alloc_stats(size_t *in_use, size_t *peak, size_t *total);

#endif
