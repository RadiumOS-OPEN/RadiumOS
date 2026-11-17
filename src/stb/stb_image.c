/* stb_image.c - the one translation unit that includes the vendored
 * stb_image.h (see README.md), configured for rbrowse's <img> decoder.
 * Everything stb defines is static; the rest of the browser sees only the
 * three rb_stb_* functions below (declared in src/rbrowse/image.c).
 *
 * The kernel builds this file at -O2 against the QuickJS compat libc headers
 * (makefile); the host tools compile it against the system libc. Either way
 * its memory comes from image.c's decode arena, which is reset after every
 * image, so nothing stb allocates outlives one decode. */
#include <stddef.h>
#include <stdint.h>

void *rb_imgmem_malloc(size_t n);
void *rb_imgmem_realloc(void *p, size_t oldsz, size_t newsz);
void rb_imgmem_free(void *p);

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 4096
#define STBI_ASSERT(x) do { if (!(x)) __builtin_trap(); } while (0)
#define STBI_MALLOC(sz) rb_imgmem_malloc(sz)
#define STBI_REALLOC_SIZED(p, oldsz, newsz) rb_imgmem_realloc(p, oldsz, newsz)
#define STBI_FREE(p) rb_imgmem_free(p)
#include "stb_image.h"

/* Size and channel count from the header alone (nothing is decoded or
   allocated). 1 when the format is recognised. */
int rb_stb_info(const unsigned char *buf, int len, int *w, int *h, int *comp) {
    if (!buf || len <= 0) return 0;
    return stbi_info_from_memory(buf, len, w, h, comp);
}

/* Decode to 8-bit RGBA (4 bytes per pixel, rows top to bottom); the first
   frame of an animated GIF. NULL on any error. The buffer lives in the
   decode arena. */
unsigned char *rb_stb_load(const unsigned char *buf, int len, int *w, int *h) {
    int comp = 0;
    if (!buf || len <= 0) return 0;
    return stbi_load_from_memory(buf, len, w, h, &comp, 4);
}

/* Why the last decode failed (a short static string). */
const char *rb_stb_reason(void) {
    const char *r = stbi_failure_reason();
    return r ? r : "unknown";
}
