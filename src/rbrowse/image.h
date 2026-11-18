#ifndef RBROWSE_IMAGE_H
#define RBROWSE_IMAGE_H

/* rbrowse images: decoding (stb_image, src/stb), the per-page image table,
   scaling to layout size, and the helpers the loader in rbrowse.c needs
   (srcset choice, data: URLs). Shared by the kernel and the host tools.

   Memory never comes from a general heap. Three static arenas:
   - decode arena (RB_IMAGE_DECODE_ARENA): everything stb allocates while
     decoding ONE image; reset after each decode.
   - store (RB_IMAGE_STORE_ARENA): each decoded image's pixels, reduced to fit
     RB_IMAGE_MASTER_MAX x RB_IMAGE_MASTER_MAX; reset by rb_image_reset() when
     a new page opens. When it is full, further images are refused (broken).
   - scaled cache (RB_IMAGE_SCALED_ARENA): copies scaled to layout size for
     the painter; thrown away wholesale when full and rebuilt on demand.

   Hostile input: files over RB_IMAGE_FILE_MAX, dimensions over
   RB_IMAGE_DIM_MAX or more than RB_IMAGE_PIXELS_MAX pixels are refused from
   the header alone, before anything is allocated; stb's own checks and the
   arena bounds cover the rest (a failed allocation is a failed decode). */

#include <stdint.h>

#define RB_IMAGE_MAX 64                     /* distinct images (URLs) per page */
#define RB_IMAGE_URL_MAX 4096               /* longest URL the loader handles (data: included) */
#define RB_IMAGE_FILE_MAX (2 * 1024 * 1024) /* encoded bytes of one image */
#define RB_IMAGE_DIM_MAX 4096               /* width and height each */
#define RB_IMAGE_PIXELS_MAX 2400000         /* width * height of one image */
#define RB_IMAGE_MASTER_MAX 1024            /* stored copy fits in this square */
#define RB_IMAGE_DECODE_ARENA (40u * 1024 * 1024)
#define RB_IMAGE_STORE_ARENA (32u * 1024 * 1024)
#define RB_IMAGE_SCALED_ARENA (12u * 1024 * 1024)

/* Slot states. */
enum { RB_IMAGE_PENDING = 0, RB_IMAGE_OK = 1, RB_IMAGE_BROKEN = 2 };

/* New page: forget every image and reset all three arenas. Pointers handed
   out before (rb_image_view, rb_image_node_pixels) become invalid. */
void rb_image_reset(void);

/* The slot already holding url (same string), or -1. */
int rb_image_find(const char *url);
/* A new PENDING slot for url, or -1 when the table is full. */
int rb_image_add(const char *url);
/* Decode len bytes into slot: RB_IMAGE_OK or RB_IMAGE_BROKEN (also the new
   state of the slot). */
int rb_image_decode(int slot, const uint8_t *data, int len);
/* Mark a slot broken (fetch failed, unsupported, over a cap). */
void rb_image_fail(int slot);
/* State of a slot (RB_IMAGE_BROKEN for a bad index); for an OK slot the
   natural size in image pixels. */
int rb_image_state(int slot, int *nw, int *nh);
/* Why the last decode of this page failed, for the debug log. */
const char *rb_image_error(void);
/* Number of slots in use and bytes used in the store (tests, status). */
int rb_image_count(void);
unsigned rb_image_store_used(void);

/* Pixels of an OK slot at a given size, for the painter. Pixels are
   0xAARRGGBB with straight alpha; *alpha is 1 when any pixel is not opaque.
   Returns 1 and fills the view. When the request fits the scaled cache the
   view is exactly w x h (box filter when shrinking, bilinear when
   enlarging, built once per size and kept); for a huge box it is the stored
   copy at its own size and the caller scales it on the fly. The pointer is
   valid until the next rb_image_view call or rb_image_reset. */
typedef struct {
    const uint32_t *px;
    int w, h, stride;
    int alpha;
} RbImageView;
int rb_image_view(int slot, int w, int h, RbImageView *out);

/* Painter hook (see rb_paint_set_images in paint.h): the view for the <img>
   element `node` at w x h, through the engine's per-node image state. */
int rb_image_view_node(int node, int w, int h, RbImageView *out);

/* For canvas drawImage: the decoded pixels of an <img> element. Returns 1
   when node is an <img> whose image decoded; px is 0xAARRGGBB (straight
   alpha), rows of *w pixels (stride == *w), *h rows. That buffer is the
   stored copy: the same as the natural size (*nw x *nh, what naturalWidth/
   naturalHeight report) unless the image was larger than
   RB_IMAGE_MASTER_MAX on a side, in which case it is reduced by an integer
   factor (map natural coordinates with *w / *nw). Valid until the page
   changes (rb_image_reset). Returns 0 for anything else. */
int rb_image_node_pixels(int node, const uint32_t **px, int *w, int *h, int *nw, int *nh);

/* Scale a 0xAARRGGBB image (sw x sh, row stride sstride pixels) to dw x dh
   into dst (stride dw). tmp must hold dw * sh pixels. Box filter on an axis
   that shrinks, bilinear on one that grows; alpha-weighted when alpha. */
void rb_image_scale(const uint32_t *src, int sw, int sh, int sstride, uint32_t *dst, int dw, int dh,
                    uint32_t *tmp, int alpha);

/* srcset: choose a candidate. With width descriptors ("a.jpg 320w") the
   smallest at least want_w wide (else the widest); with densities ("1x",
   "2x", none = 1x) the 1x one, else the lowest density above 1x, else the
   highest. Writes the URL (NUL-terminated) and returns 1; 0 when nothing
   usable (or it does not fit in cap). *is_w tells which kind was used. */
int rb_image_pick_srcset(const char *srcset, int len, int want_w, char *out, int cap, int *is_w);

/* data: URL payload (base64 or percent-encoded) into out. Returns the byte
   count, or -1 when it is not a data: URL, is malformed or does not fit. */
int rb_image_data_url(const char *url, uint8_t *out, int cap);

/* 1 when the URL names a format the decoder cannot read (SVG, WebP, AVIF,
   ICO by extension or data: media type), so the loader can skip the fetch. */
int rb_image_unsupported(const char *url);

#endif
