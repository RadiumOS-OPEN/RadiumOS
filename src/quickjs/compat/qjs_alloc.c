/* qjs_alloc.c - QuickJS's heap: a two-level segregated-fit allocator (the
 * TLSF scheme) over one static arena. malloc and free are O(1); free
 * neighbours coalesce through boundary tags. The arena is private to
 * QuickJS, so qjs_alloc_reset() can throw everything away at once, which is
 * how a crashed or abandoned runtime is reclaimed without walking it.
 *
 * Block layout (all sizes multiples of 8, header included):
 *   [size|FREE] [prev_size] payload...
 * A free block keeps its free-list links in the first 8 payload bytes, so
 * the smallest block is 16 bytes. prev_size always holds the physical
 * predecessor's size (0 for the first block); a 0-sized used sentinel ends
 * the arena so the last block never looks past it.
 */
#include "include/qjs_libc.h"
#include "qjs_alloc.h"

#define QJS_ARENA_SIZE (48u * 1024 * 1024)

#define HDR 8u
#define MIN_BLOCK 16u
#define FREE_BIT 1u
#define SIZE_MASK (~7u)

/* Size classes: below SMALL_LIMIT one class per 8 bytes; above it, each
 * power of two is split into 2^SL_BITS classes. */
#define SL_BITS 5
#define SL_COUNT (1 << SL_BITS)
#define FL_SHIFT (SL_BITS + 3)            /* 256 */
#define SMALL_LIMIT (1u << FL_SHIFT)
#define FL_COUNT 26                       /* covers sizes below 2^(FL_SHIFT+FL_COUNT-1) */

typedef struct Block {
    uint32_t size;          /* block size including header | FREE_BIT */
    uint32_t prev_size;     /* physical predecessor's size, 0 if none */
    struct Block *next_free;
    struct Block *prev_free;
} Block;

static uint8_t g_arena[QJS_ARENA_SIZE] __attribute__((aligned(16)));
static uint32_t g_fl_bitmap;
static uint32_t g_sl_bitmap[FL_COUNT];
static Block *g_bins[FL_COUNT][SL_COUNT];
static int g_ready;
static size_t g_in_use;
static size_t g_peak;

static inline uint32_t bsize(const Block *b) { return b->size & SIZE_MASK; }
static inline int is_free(const Block *b) { return (b->size & FREE_BIT) != 0; }
static inline Block *next_phys(Block *b) { return (Block *)((uint8_t *)b + bsize(b)); }
static inline Block *prev_phys(Block *b) { return (Block *)((uint8_t *)b - b->prev_size); }
static inline void *payload(Block *b) { return (uint8_t *)b + HDR; }
static inline Block *from_payload(void *p) { return (Block *)((uint8_t *)p - HDR); }

static inline int fls32(uint32_t v) { return 31 - __builtin_clz(v); }

static void mapping(uint32_t size, int *fl, int *sl) {
    if (size < SMALL_LIMIT) {
        *fl = 0;
        *sl = (int)(size >> 3);
    } else {
        int f = fls32(size);
        *sl = (int)((size >> (f - SL_BITS)) ^ (1u << SL_BITS));
        *fl = f - FL_SHIFT + 1;
    }
}

static void insert_free(Block *b) {
    int fl, sl;
    mapping(bsize(b), &fl, &sl);
    b->prev_free = 0;
    b->next_free = g_bins[fl][sl];
    if (b->next_free) b->next_free->prev_free = b;
    g_bins[fl][sl] = b;
    g_fl_bitmap |= 1u << fl;
    g_sl_bitmap[fl] |= 1u << sl;
}

static void remove_free(Block *b) {
    int fl, sl;
    mapping(bsize(b), &fl, &sl);
    if (b->prev_free) b->prev_free->next_free = b->next_free;
    else g_bins[fl][sl] = b->next_free;
    if (b->next_free) b->next_free->prev_free = b->prev_free;
    if (!g_bins[fl][sl]) {
        g_sl_bitmap[fl] &= ~(1u << sl);
        if (!g_sl_bitmap[fl]) g_fl_bitmap &= ~(1u << fl);
    }
}

/* A free block whose class is at least `size`'s rounded-up class, so any
 * block found fits without searching the list. */
static Block *find_free(uint32_t size) {
    if (size >= SMALL_LIMIT) size += (1u << (fls32(size) - SL_BITS)) - 1;
    int fl, sl;
    mapping(size, &fl, &sl);
    if (fl >= FL_COUNT) return 0;
    uint32_t sl_map = g_sl_bitmap[fl] & (~0u << sl);
    if (!sl_map) {
        uint32_t fl_map = fl + 1 < 32 ? g_fl_bitmap & (~0u << (fl + 1)) : 0;
        if (!fl_map) return 0;
        fl = __builtin_ctz(fl_map);
        sl_map = g_sl_bitmap[fl];
    }
    sl = __builtin_ctz(sl_map);
    return g_bins[fl][sl];
}

static void set_size(Block *b, uint32_t size, int free_flag) {
    b->size = size | (free_flag ? FREE_BIT : 0);
    next_phys(b)->prev_size = size;
}

void qjs_alloc_reset(void) {
    g_fl_bitmap = 0;
    memset(g_sl_bitmap, 0, sizeof(g_sl_bitmap));
    memset(g_bins, 0, sizeof(g_bins));
    Block *first = (Block *)g_arena;
    uint32_t span = QJS_ARENA_SIZE - HDR;   /* room left for the end sentinel */
    Block *end = (Block *)(g_arena + span);
    end->size = 0;                          /* used, size 0 */
    first->prev_size = 0;
    set_size(first, span, 1);
    insert_free(first);
    g_in_use = 0;
    g_peak = 0;
    g_ready = 1;
}

static uint32_t request_size(size_t n) {
    if (n > QJS_ARENA_SIZE) return 0;
    uint32_t size = ((uint32_t)n + HDR + 7u) & SIZE_MASK;
    return size < MIN_BLOCK ? MIN_BLOCK : size;
}

/* Cut `b` (already off the free lists) down to `size`, returning the tail
 * to the free lists when it is big enough to be a block. */
static void trim(Block *b, uint32_t size) {
    uint32_t total = bsize(b);
    if (total - size >= MIN_BLOCK) {
        set_size(b, size, 0);
        Block *rest = next_phys(b);
        rest->prev_size = size;
        set_size(rest, total - size, 1);
        /* The tail may sit before another free block (realloc shrink). */
        Block *after = next_phys(rest);
        if (is_free(after)) {
            remove_free(after);
            set_size(rest, bsize(rest) + bsize(after), 1);
        }
        insert_free(rest);
    } else {
        set_size(b, total, 0);
    }
}

void *qjs_malloc(size_t n) {
    if (!g_ready) qjs_alloc_reset();
    uint32_t size = request_size(n);
    if (!size) return 0;
    Block *b = find_free(size);
    if (!b) return 0;
    remove_free(b);
    trim(b, size);
    g_in_use += bsize(b);
    if (g_in_use > g_peak) g_peak = g_in_use;
    return payload(b);
}

void qjs_free(void *p) {
    if (!p) return;
    Block *b = from_payload(p);
    uint32_t size = bsize(b);
    g_in_use -= size;
    Block *nx = next_phys(b);
    if (is_free(nx)) {
        remove_free(nx);
        size += bsize(nx);
    }
    if (b->prev_size) {
        Block *pv = prev_phys(b);
        if (is_free(pv)) {
            remove_free(pv);
            size += bsize(pv);
            b = pv;
        }
    }
    set_size(b, size, 1);
    insert_free(b);
}

void *qjs_realloc(void *p, size_t n) {
    if (!p) return qjs_malloc(n);
    if (n == 0) {
        qjs_free(p);
        return 0;
    }
    Block *b = from_payload(p);
    uint32_t want = request_size(n);
    if (!want) return 0;
    uint32_t have = bsize(b);
    if (want <= have) {
        g_in_use -= have;
        trim(b, want);
        g_in_use += bsize(b);
        return p;
    }
    /* Grow in place into a free successor. */
    Block *nx = next_phys(b);
    if (is_free(nx) && have + bsize(nx) >= want) {
        remove_free(nx);
        set_size(b, have + bsize(nx), 0);
        g_in_use -= have;
        trim(b, want);
        g_in_use += bsize(b);
        if (g_in_use > g_peak) g_peak = g_in_use;
        return p;
    }
    void *q = qjs_malloc(n);
    if (!q) return 0;
    memcpy(q, p, have - HDR);
    qjs_free(p);
    return q;
}

void *qjs_calloc(size_t n, size_t size) {
    if (size && n > (size_t)-1 / size) return 0;
    void *p = qjs_malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

size_t qjs_malloc_usable_size(void *p) {
    return p ? bsize(from_payload(p)) - HDR : 0;
}

void qjs_alloc_stats(size_t *in_use, size_t *peak, size_t *total) {
    if (in_use) *in_use = g_in_use;
    if (peak) *peak = g_peak;
    if (total) *total = QJS_ARENA_SIZE;
}
