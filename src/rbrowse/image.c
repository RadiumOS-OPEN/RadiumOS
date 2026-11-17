// rbrowse images: decode (stb_image), per-page table, scaling, srcset and
// data: URL helpers. See image.h for the memory model and the caps. Built at
// -O2 in the kernel (makefile): decoding and scaling touch every pixel.

#include "image.h"
#include "engine.h"

#include <stddef.h>
#ifdef RBROWSE_HOST
#include <string.h>
#else
/* the kernel's libc (src/utility); utility.h itself drags in unused statics */
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *ptr, int value, size_t num);
size_t strlen(const char *str);
#endif

int rb_stb_info(const unsigned char *buf, int len, int *w, int *h, int *comp);
unsigned char *rb_stb_load(const unsigned char *buf, int len, int *w, int *h);
const char *rb_stb_reason(void);

/* ---- arenas ----------------------------------------------------------------
   Declared as uint32_t so pixel words may be read and written through them
   (stb's byte stores are character accesses, which may alias anything). */

static uint32_t g_dec[RB_IMAGE_DECODE_ARENA / 4] __attribute__((aligned(16)));
static size_t g_dec_top;               /* bytes used */
static size_t g_dec_last;              /* offset of the newest block's header, for in-place realloc */

static uint32_t g_store[RB_IMAGE_STORE_ARENA / 4] __attribute__((aligned(16)));
static size_t g_store_top;             /* pixels used */

static uint32_t g_scaled[RB_IMAGE_SCALED_ARENA / 4] __attribute__((aligned(16)));
static size_t g_scaled_top;            /* pixels used */
static uint32_t g_scaled_gen = 1;      /* bumped when the scaled cache is thrown away */

/* Decode arena: a bump allocator with a 16-byte header holding the block
   size. free() of the newest block gives its space back and realloc() of
   the newest block grows in place, which is exactly how stb grows its zlib
   and IDAT buffers; anything else waits for the reset after the decode. */
#define HDR 16u

void *rb_imgmem_malloc(size_t n) {
    if (n > RB_IMAGE_DECODE_ARENA) return 0;
    size_t need = HDR + ((n + 15u) & ~(size_t)15u);
    if (need > RB_IMAGE_DECODE_ARENA - g_dec_top) return 0;
    uint8_t *b = (uint8_t *)g_dec + g_dec_top;
    *(size_t *)b = n;
    g_dec_last = g_dec_top;
    g_dec_top += need;
    return b + HDR;
}

static int is_last(const void *p) {
    return p && g_dec_top > 0 && (const uint8_t *)p == (const uint8_t *)g_dec + g_dec_last + HDR;
}

void rb_imgmem_free(void *p) {
    if (is_last(p)) {
        g_dec_top = g_dec_last;
        g_dec_last = g_dec_top;    /* the block below is unknown: no further in-place growth */
    }
}

void *rb_imgmem_realloc(void *p, size_t oldsz, size_t newsz) {
    (void)oldsz;
    if (!p) return rb_imgmem_malloc(newsz);
    if (newsz > RB_IMAGE_DECODE_ARENA) return 0;
    uint8_t *hdr = (uint8_t *)p - HDR;
    size_t have = *(size_t *)hdr;
    if (is_last(p)) {
        size_t need = HDR + ((newsz + 15u) & ~(size_t)15u);
        if (need > RB_IMAGE_DECODE_ARENA - g_dec_last) return 0;
        *(size_t *)hdr = newsz;
        g_dec_top = g_dec_last + need;
        return p;
    }
    void *q = rb_imgmem_malloc(newsz);
    if (!q) return 0;
    memcpy(q, p, have < newsz ? have : newsz);
    return q;
}

static void dec_reset(void) {
    g_dec_top = 0;
    g_dec_last = 0;
}

static uint32_t *store_alloc(size_t px) {
    px = (px + 3u) & ~(size_t)3u;          /* keep 16-byte alignment */
    if (px > RB_IMAGE_STORE_ARENA / 4 - g_store_top) return 0;
    uint32_t *p = g_store + g_store_top;
    g_store_top += px;
    return p;
}

/* ---- the image table ---------------------------------------------------------- */

typedef struct {
    uint32_t hash;
    int ulen;
    char url[256];          /* prefix; hash + length + prefix identify the URL */
    uint8_t state;          /* RB_IMAGE_* */
    uint8_t alpha;          /* some pixel is not opaque */
    int nw, nh;             /* natural size */
    int mw, mh;             /* stored copy */
    uint32_t *px;           /* stored copy, 0xAARRGGBB, stride mw */
    uint32_t *sc;           /* scaled copy, valid while sgen == g_scaled_gen */
    int sw, sh;
    uint32_t sgen;
} ImgEnt;

static ImgEnt g_img[RB_IMAGE_MAX];
static int g_img_n;
static const char *g_err = "";

void rb_image_reset(void) {
    g_img_n = 0;
    dec_reset();
    g_store_top = 0;
    g_scaled_top = 0;
    g_scaled_gen++;
    g_err = "";
}

static uint32_t url_hash(const char *s, int *len) {
    uint32_t h = 2166136261u;
    int n = 0;
    for (; s[n]; n++) h = (h ^ (uint8_t)s[n]) * 16777619u;
    *len = n;
    return h;
}

int rb_image_find(const char *url) {
    if (!url) return -1;
    int n;
    uint32_t h = url_hash(url, &n);
    for (int i = 0; i < g_img_n; i++) {
        const ImgEnt *e = &g_img[i];
        if (e->hash != h || e->ulen != n) continue;
        int k = 0;
        while (k < (int)sizeof(e->url) - 1 && url[k] && e->url[k] == url[k]) k++;
        if (k == (int)sizeof(e->url) - 1 || (!url[k] && !e->url[k])) return i;
    }
    return -1;
}

int rb_image_add(const char *url) {
    if (!url || g_img_n >= RB_IMAGE_MAX) return -1;
    ImgEnt *e = &g_img[g_img_n];
    memset(e, 0, sizeof(*e));
    e->hash = url_hash(url, &e->ulen);
    int k = 0;
    while (k < (int)sizeof(e->url) - 1 && url[k]) { e->url[k] = url[k]; k++; }
    e->url[k] = 0;
    e->state = RB_IMAGE_PENDING;
    return g_img_n++;
}

static ImgEnt *ent(int slot) {
    return (slot >= 0 && slot < g_img_n) ? &g_img[slot] : 0;
}

void rb_image_fail(int slot) {
    ImgEnt *e = ent(slot);
    if (e) e->state = RB_IMAGE_BROKEN;
}

int rb_image_state(int slot, int *nw, int *nh) {
    ImgEnt *e = ent(slot);
    if (!e) return RB_IMAGE_BROKEN;
    if (e->state == RB_IMAGE_OK) {
        if (nw) *nw = e->nw;
        if (nh) *nh = e->nh;
    }
    return e->state;
}

const char *rb_image_error(void) { return g_err; }
int rb_image_count(void) { return g_img_n; }
unsigned rb_image_store_used(void) { return (unsigned)(g_store_top * 4); }

/* stb's RGBA bytes -> 0xAARRGGBB words, in place, reduced by an integer
   factor k (box average, alpha-weighted) so the copy fits the master cap.
   Writing out[y*mw+x] never overtakes a pixel a later block still reads. */
static int reduce_in_place(uint32_t *buf, int w, int h, int k, int *mw, int *mh) {
    uint8_t *b = (uint8_t *)buf;
    int ow = (w + k - 1) / k, oh = (h + k - 1) / k;
    int alpha = 0;
    for (int oy = 0; oy < oh; oy++) {
        int y0 = oy * k, y1 = y0 + k < h ? y0 + k : h;
        for (int ox = 0; ox < ow; ox++) {
            int x0 = ox * k, x1 = x0 + k < w ? x0 + k : w;
            uint32_t r = 0, g = 0, bl = 0, a = 0, cnt = 0;
            for (int y = y0; y < y1; y++) {
                const uint8_t *p = b + ((size_t)y * w + x0) * 4;
                for (int x = x0; x < x1; x++, p += 4) {
                    r += p[0] * (uint32_t)p[3];
                    g += p[1] * (uint32_t)p[3];
                    bl += p[2] * (uint32_t)p[3];
                    a += p[3];
                    cnt++;
                }
            }
            uint32_t oa = a / cnt, orr = 0, og = 0, ob = 0;
            if (a) { orr = r / a; og = g / a; ob = bl / a; }
            if (oa < 255) alpha = 1;
            buf[(size_t)oy * ow + ox] = (oa << 24) | (orr << 16) | (og << 8) | ob;
        }
    }
    *mw = ow;
    *mh = oh;
    return alpha;
}

int rb_image_decode(int slot, const uint8_t *data, int len) {
    ImgEnt *e = ent(slot);
    if (!e) return RB_IMAGE_BROKEN;
    e->state = RB_IMAGE_BROKEN;
    if (!data || len <= 0 || len > RB_IMAGE_FILE_MAX) { g_err = "file too large or empty"; return RB_IMAGE_BROKEN; }
    int w = 0, h = 0, comp = 0;
    if (!rb_stb_info(data, len, &w, &h, &comp)) { g_err = "not a PNG/JPEG/GIF/BMP image"; return RB_IMAGE_BROKEN; }
    if (w <= 0 || h <= 0 || w > RB_IMAGE_DIM_MAX || h > RB_IMAGE_DIM_MAX ||
        (long long)w * h > RB_IMAGE_PIXELS_MAX) {
        g_err = "image dimensions over the cap";
        return RB_IMAGE_BROKEN;
    }
    dec_reset();
    int dw = 0, dh = 0;
    uint8_t *rgba = rb_stb_load(data, len, &dw, &dh);
    if (!rgba || dw != w || dh != h) {
        g_err = rgba ? "decoded size differs from the header" : rb_stb_reason();
        dec_reset();
        return RB_IMAGE_BROKEN;
    }
    int k = 1;
    while ((w + k - 1) / k > RB_IMAGE_MASTER_MAX || (h + k - 1) / k > RB_IMAGE_MASTER_MAX) k++;
    int mw, mh;
    int alpha = reduce_in_place((uint32_t *)(void *)rgba, w, h, k, &mw, &mh);
    uint32_t *px = store_alloc((size_t)mw * mh);
    if (!px) {
        g_err = "page image memory full";
        dec_reset();
        return RB_IMAGE_BROKEN;
    }
    memcpy(px, rgba, (size_t)mw * mh * 4);
    dec_reset();
    e->px = px;
    e->mw = mw;
    e->mh = mh;
    e->nw = w;
    e->nh = h;
    e->alpha = (uint8_t)alpha;
    e->sc = 0;
    e->state = RB_IMAGE_OK;
    return RB_IMAGE_OK;
}

/* ---- scaling ---------------------------------------------------------------------
   Separable: rows first into tmp (dw x sh), then columns into dst. Positions
   are in 1/256 of a source pixel and advance by exact integer steps (a
   quotient and a remainder), so everything stays in 32-bit arithmetic: the
   kernel's 64-bit division helpers are slow bit loops. Alpha weights are
   kept to 8 bits so a long box sum of colour * alpha * weight still fits. */

static void line_resample(const uint32_t *s, int sn, int sstep, uint32_t *d, int dn, int dstep, int alpha) {
    if (dn == sn) {
        for (int i = 0; i < dn; i++) d[i * dstep] = s[i * sstep];
        return;
    }
    uint32_t S = (uint32_t)sn << 8;            /* source span of one output pixel, times dn */
    uint32_t sq = S / (uint32_t)dn, sr = S % (uint32_t)dn;
    if (dn < sn) {
        uint32_t x0 = 0, r0 = 0;               /* floor(i * S / dn) and its remainder */
        for (int i = 0; i < dn; i++) {
            uint32_t x1 = x0 + sq, r1 = r0 + sr;
            if (r1 >= (uint32_t)dn) { x1++; r1 -= (uint32_t)dn; }
            uint32_t ar = 0, ag = 0, ab = 0, aw = 0, sw = 0, as = 0;
            for (uint32_t j = x0 >> 8; j < (uint32_t)sn && (j << 8) < x1; j++) {
                uint32_t lo = j << 8 > x0 ? j << 8 : x0;
                uint32_t hi = (j + 1) << 8 < x1 ? (j + 1) << 8 : x1;
                uint32_t wt = hi - lo;
                uint32_t p = s[(int)j * sstep];
                uint32_t a = p >> 24;
                uint32_t cw = alpha ? (wt * a + 128) >> 8 : wt;
                ar += ((p >> 16) & 255) * cw;
                ag += ((p >> 8) & 255) * cw;
                ab += (p & 255) * cw;
                aw += cw;
                as += a * wt;
                sw += wt;
            }
            uint32_t oa = sw ? as / sw : 0;
            uint32_t r = aw ? ar / aw : 0, g = aw ? ag / aw : 0, b = aw ? ab / aw : 0;
            d[i * dstep] = (oa << 24) | (r << 16) | (g << 8) | b;
            x0 = x1;
            r0 = r1;
        }
        return;
    }
    /* centre of output pixel i in source space: (2i + 1) * S / (2 * dn) */
    uint32_t cq = (S >> 1) / (uint32_t)dn, cr = (S >> 1) % (uint32_t)dn;
    for (int i = 0; i < dn; i++) {
        int pos = (int)cq - 128;               /* minus half a source pixel */
        if (pos < 0) pos = 0;
        int j = pos >> 8, f = pos & 255;
        if (j >= sn - 1) { j = sn - 1; f = 0; }
        uint32_t p = s[j * sstep], q = f ? s[(j + 1) * sstep] : p;
        uint32_t pa = p >> 24, qa = q >> 24;
        uint32_t wp = (uint32_t)(256 - f), wq = (uint32_t)f;
        if (alpha) { wp *= pa; wq *= qa; }
        uint32_t den = wp + wq;
        uint32_t oa = (pa * (uint32_t)(256 - f) + qa * (uint32_t)f) >> 8;
        uint32_t r = 0, g = 0, b = 0;
        if (den) {
            r = (((p >> 16) & 255) * wp + ((q >> 16) & 255) * wq) / den;
            g = (((p >> 8) & 255) * wp + ((q >> 8) & 255) * wq) / den;
            b = ((p & 255) * wp + (q & 255) * wq) / den;
        }
        d[i * dstep] = (oa << 24) | (r << 16) | (g << 8) | b;
        cq += sq;
        cr += sr;
        if (cr >= (uint32_t)dn) { cq++; cr -= (uint32_t)dn; }
    }
}

void rb_image_scale(const uint32_t *src, int sw, int sh, int sstride, uint32_t *dst, int dw, int dh,
                    uint32_t *tmp, int alpha) {
    /* 16384 keeps every sum above inside 32 bits */
    if (!src || !dst || !tmp || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 ||
        sw > 16384 || sh > 16384 || dw > 16384 || dh > 16384) return;
    for (int y = 0; y < sh; y++) line_resample(src + (size_t)y * sstride, sw, 1, tmp + (size_t)y * dw, dw, 1, alpha);
    for (int x = 0; x < dw; x++) line_resample(tmp + x, sh, dw, dst + x, dh, dw, alpha);
}

int rb_image_view(int slot, int w, int h, RbImageView *out) {
    ImgEnt *e = ent(slot);
    if (!e || e->state != RB_IMAGE_OK || !e->px || w <= 0 || h <= 0 || !out) return 0;
    out->alpha = e->alpha;
    if (w == e->mw && h == e->mh) {
        out->px = e->px; out->w = e->mw; out->h = e->mh; out->stride = e->mw;
        return 1;
    }
    if (e->sc && e->sgen == g_scaled_gen && e->sw == w && e->sh == h) {
        out->px = e->sc; out->w = w; out->h = h; out->stride = w;
        return 1;
    }
    size_t need = (size_t)w * h + (size_t)w * e->mh;     /* result + row-pass temporary */
    /* rb_image_scale does nothing past 16384 on a side: a wide, flat box
       (20000 x 20) fits the arena but would be handed out unwritten */
    if (need > RB_IMAGE_SCALED_ARENA / 4 || w > 16384 || h > 16384) {
        /* too big to cache: hand out the stored copy, the painter samples it */
        out->px = e->px; out->w = e->mw; out->h = e->mh; out->stride = e->mw;
        return 1;
    }
    if (need > RB_IMAGE_SCALED_ARENA / 4 - g_scaled_top) {
        g_scaled_top = 0;
        g_scaled_gen++;
    }
    uint32_t *dst = g_scaled + g_scaled_top;
    rb_image_scale(e->px, e->mw, e->mh, e->mw, dst, w, h, dst + (size_t)w * h, e->alpha);
    g_scaled_top += ((size_t)w * h + 3u) & ~(size_t)3u;
    e->sc = dst; e->sw = w; e->sh = h; e->sgen = g_scaled_gen;
    out->px = dst; out->w = w; out->h = h; out->stride = w;
    return 1;
}

int rb_image_view_node(int node, int w, int h, RbImageView *out) {
    int slot = -1;
    if (rb_dom_img_state(node, &slot, 0, 0) != RB_IMG_OK) return 0;
    return rb_image_view(slot, w, h, out);
}

int rb_image_node_pixels(int node, const uint32_t **px, int *w, int *h, int *nw, int *nh) {
    int slot = -1;
    if (rb_dom_img_state(node, &slot, 0, 0) != RB_IMG_OK) return 0;
    ImgEnt *e = ent(slot);
    if (!e || e->state != RB_IMAGE_OK || !e->px) return 0;
    if (px) *px = e->px;
    if (w) *w = e->mw;
    if (h) *h = e->mh;
    if (nw) *nw = e->nw;
    if (nh) *nh = e->nh;
    return 1;
}

/* ---- URL helpers -------------------------------------------------------------------- */

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int prefix_ci(const char *s, const char *lit) {
    for (int i = 0; lit[i]; i++) if (lc((unsigned char)s[i]) != lit[i]) return 0;
    return 1;
}

static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

int rb_image_pick_srcset(const char *s, int len, int want_w, char *out, int cap, int *is_w) {
    int best_s = -1, best_n = 0;     /* chosen URL: start, length */
    long best_v = 0;                 /* its width, or density in 1/1000 */
    int best_kind = 0;               /* 1 width, 2 density */
    int i = 0;
    if (!s || len <= 0 || !out || cap <= 1) return 0;
    while (i < len) {
        while (i < len && (is_ws((unsigned char)s[i]) || s[i] == ',')) i++;
        if (i >= len) break;
        int us = i;
        while (i < len && !is_ws((unsigned char)s[i])) i++;
        int ue = i;
        int ended = 0;               /* a URL ending in ',' has no descriptors */
        while (ue > us && s[ue - 1] == ',') { ue--; ended = 1; }
        long v = 1000;
        int kind = 2, bad = 0;
        if (!ended) {
            /* descriptors up to the next ',' */
            while (i < len && s[i] != ',') {
                while (i < len && is_ws((unsigned char)s[i])) i++;
                int ds = i;
                while (i < len && !is_ws((unsigned char)s[i]) && s[i] != ',') i++;
                int dn = i - ds;
                if (dn < 2) { if (dn) bad = 1; continue; }
                char unit = (char)lc((unsigned char)s[ds + dn - 1]);
                long ip = 0, frac = 0, fd = 1;
                int k = ds, dot = 0, digits = 0;
                for (; k < ds + dn - 1; k++) {
                    char c = s[k];
                    if (c >= '0' && c <= '9') {
                        digits++;
                        if (dot) { if (fd < 1000) { frac = frac * 10 + (c - '0'); fd *= 10; } }
                        else if (ip < 100000) ip = ip * 10 + (c - '0');
                    } else if (c == '.' && !dot) dot = 1;
                    else { bad = 1; break; }
                }
                if (!digits) bad = 1;
                if (unit == 'w') { kind = 1; v = ip; }
                else if (unit == 'x') { kind = 2; v = ip * 1000 + frac * 1000 / fd; }
                else if (unit != 'h') bad = 1;       /* 'h' (future syntax) is ignored */
            }
        }
        if (bad || ue <= us || v <= 0) continue;
        int better = 0;
        if (best_s < 0) better = 1;
        else if (kind != best_kind) better = kind == 1;          /* width descriptors win */
        else if (kind == 1) {
            if (best_v < want_w) better = v > best_v;            /* still too small: take bigger */
            else better = v >= want_w && v < best_v;             /* big enough: take smaller */
        } else {
            if (best_v == 1000) better = 0;
            else if (v == 1000) better = 1;
            else if (best_v < 1000) better = v > best_v;
            else better = v > 1000 && v < best_v;
        }
        if (better) { best_s = us; best_n = ue - us; best_v = v; best_kind = kind; }
    }
    if (best_s < 0 || best_n >= cap) return 0;
    memcpy(out, s + best_s, (size_t)best_n);
    out[best_n] = 0;
    if (is_w) *is_w = best_kind == 1;
    return 1;
}

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lc(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int rb_image_data_url(const char *url, uint8_t *out, int cap) {
    if (!url || !out || cap <= 0 || !prefix_ci(url, "data:")) return -1;
    const char *p = url + 5;
    int comma = -1;
    for (int i = 0; p[i]; i++) if (p[i] == ',') { comma = i; break; }
    if (comma < 0) return -1;
    int b64 = comma >= 7 && prefix_ci(p + comma - 7, ";base64");
    const char *d = p + comma + 1;
    int n = 0;
    if (b64) {
        uint32_t acc = 0;
        int bits = 0;
        for (; *d; d++) {
            int c = (unsigned char)*d;
            if (is_ws(c)) continue;
            if (c == '=') break;
            if (c == '%' && d[1] == '2' && lc((unsigned char)d[2]) == '0') { d += 2; continue; }   /* an encoded space */
            int v = b64_val(c);
            if (v < 0) return -1;
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                if (n >= cap) return -1;
                out[n++] = (uint8_t)(acc >> bits);
            }
        }
        return n;
    }
    for (; *d; d++) {
        int c = (unsigned char)*d;
        if (c == '%' && hex_digit((unsigned char)d[1]) >= 0 && hex_digit((unsigned char)d[2]) >= 0) {
            c = hex_digit((unsigned char)d[1]) * 16 + hex_digit((unsigned char)d[2]);
            d += 2;
        }
        if (n >= cap) return -1;
        out[n++] = (uint8_t)c;
    }
    return n;
}

int rb_image_unsupported(const char *url) {
    if (!url) return 1;
    if (prefix_ci(url, "data:")) {
        const char *m = url + 5;
        return prefix_ci(m, "image/svg") || prefix_ci(m, "image/webp") || prefix_ci(m, "image/avif") ||
               prefix_ci(m, "image/x-icon") || prefix_ci(m, "image/vnd.microsoft.icon") || prefix_ci(m, "text/");
    }
    int end = 0;
    while (url[end] && url[end] != '?' && url[end] != '#') end++;
    static const char *const bad[] = { ".svg", ".svgz", ".webp", ".avif", ".ico", 0 };
    for (int k = 0; bad[k]; k++) {
        int n = (int)strlen(bad[k]);
        if (end >= n && prefix_ci(url + end - n, bad[k])) return 1;
    }
    return 0;
}
