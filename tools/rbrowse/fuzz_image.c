/*
 * Host-side fuzzer for rbrowse's image path: decode (stb_image through
 * image.c, with its caps and arenas), the stored-copy reduction, scaling to
 * random box sizes, and the srcset / data: URL parsers, on truncated and
 * corrupted inputs, under ASan + UBSan. Run from the repository root (the
 * seeds are the files in tools/rbrowse/img, plus any given on the command
 * line). -fwrapv as in the kernel build (src/stb/README.md): stb's integer
 * IDCT may overflow on corrupt JPEG coefficients, which only garbles pixels.
 *
 *   clang -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fwrapv \
 *       -DRBROWSE_HOST -I src/rbrowse tools/rbrowse/fuzz_image.c src/rbrowse/image.c \
 *       src/stb/stb_image.c src/rbrowse/engine.c -o /tmp/rbfuzz_image
 *   /tmp/rbfuzz_image [iterations] [seed] [more seed files ...]
 *
 * Mutations: byte flips, random bytes, truncation, chunk copies, and header
 * edits that make PNG/GIF/BMP/JPEG claim other sizes. Every decode that
 * succeeds must report a size within the caps, and its views are scaled to
 * a few random sizes (both the cached and the too-big path).
 */
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng = 0x9E3779B97F4A7C15ULL;

static unsigned rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (unsigned)(rng >> 16);
}

#define MAX_SEEDS 16
static uint8_t *g_seed[MAX_SEEDS];
static int g_seed_len[MAX_SEEDS], g_nseeds;
static uint8_t g_buf[1 << 20];
static int g_fails;

static void add_seed(const char *path) {
    if (g_nseeds >= MAX_SEEDS) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t *b = malloc(sizeof(g_buf));
    int n = (int)fread(b, 1, sizeof(g_buf), f);
    fclose(f);
    if (n <= 0) { free(b); return; }
    g_seed[g_nseeds] = b;
    g_seed_len[g_nseeds++] = n;
}

static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put_le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v) { put_le16(p, v); put_le16(p + 2, v >> 16); }

static uint32_t odd_size(void) {
    static const uint32_t s[] = { 0, 1, 2, 3, 255, 1024, 1025, 2049, 4095, 4096, 4097, 65535, 65536, 0x7FFFFFFF, 0xFFFFFFFF };
    return rnd() % 3 ? s[rnd() % 15] : rnd() % 5000;
}

/* Make the header claim another size (where the format puts it). */
static void edit_header(uint8_t *b, int n) {
    if (n > 24 && b[0] == 0x89 && b[1] == 'P') { put_be32(b + 16, odd_size()); put_be32(b + 20, odd_size()); }
    else if (n > 10 && b[0] == 'G' && b[1] == 'I') { put_le16(b + 6, odd_size()); put_le16(b + 8, odd_size()); }
    else if (n > 26 && b[0] == 'B' && b[1] == 'M') { put_le32(b + 18, odd_size()); put_le32(b + 22, rnd() % 2 ? odd_size() : (uint32_t)-(int)odd_size()); }
    else if (n > 4 && b[0] == 0xFF && b[1] == 0xD8) {
        for (int i = 2; i + 9 < n; i++)
            if (b[i] == 0xFF && (b[i + 1] == 0xC0 || b[i + 1] == 0xC2)) {
                b[i + 5] = (uint8_t)(odd_size() >> 8); b[i + 6] = (uint8_t)rnd();
                b[i + 7] = (uint8_t)(odd_size() >> 8); b[i + 8] = (uint8_t)rnd();
                break;
            }
    }
}

static int make_input(void) {
    int s = (int)(rnd() % (unsigned)g_nseeds);
    int n = g_seed_len[s];
    memcpy(g_buf, g_seed[s], (size_t)n);
    int muts = 1 + (int)(rnd() % 6);
    for (int m = 0; m < muts; m++) {
        switch (rnd() % 7) {
        case 0: if (n) g_buf[rnd() % (unsigned)n] ^= (uint8_t)(1u << (rnd() % 8)); break;
        case 1: if (n) g_buf[rnd() % (unsigned)n] = (uint8_t)rnd(); break;
        case 2: if (n > 1) n = 1 + (int)(rnd() % (unsigned)n); break;           /* truncate */
        case 3: edit_header(g_buf, n); break;
        case 4: {                                                             /* copy a chunk */
            if (n < 8) break;
            int len = 1 + (int)(rnd() % 64), from = (int)(rnd() % (unsigned)n), to = (int)(rnd() % (unsigned)n);
            if (from + len > n) len = n - from;
            if (to + len > (int)sizeof(g_buf)) len = (int)sizeof(g_buf) - to;
            memmove(g_buf + to, g_buf + from, (size_t)len);
            if (to + len > n) n = to + len;
            break;
        }
        case 5: if (n > 16) { int at = 8 + (int)(rnd() % (unsigned)(n - 8)); memset(g_buf + at, rnd() % 2 ? 0xFF : 0, (size_t)(n - at < 16 ? n - at : 16)); } break;
        default: break;
        }
    }
    return n;
}

static void fuzz_strings(void) {
    static const char *parts[] = { "a.png", " ", ",", "2x", "1.5x", "640w", "0w", "-3w", "x", "w", ".", "data:", ";base64,",
                                   "iVBORw0KGgo=", "%2", "%41", "==", "\t", "999999999999w", "h", "1e3x", "//x/y,z.jpg" };
    char s[512], out[256];
    int n = 0;
    int k = (int)(rnd() % 24);
    for (int i = 0; i < k; i++) {
        const char *p = parts[rnd() % (sizeof(parts) / sizeof(parts[0]))];
        int l = (int)strlen(p);
        if (n + l >= (int)sizeof(s)) break;
        memcpy(s + n, p, (size_t)l);
        n += l;
    }
    s[n] = 0;
    int is_w;
    if (rb_image_pick_srcset(s, n, (int)(rnd() % 2000), out, (int)(rnd() % 2 ? sizeof(out) : 1 + rnd() % 8), &is_w)) {
        if (strlen(out) == 0) { printf("fuzz: empty srcset pick for '%s'\n", s); g_fails++; }
    }
    uint8_t bin[300];
    int r = rb_image_data_url(s, bin, (int)(1 + rnd() % sizeof(bin)));
    if (r > (int)sizeof(bin)) { printf("fuzz: data url overflow\n"); g_fails++; }
    (void)rb_image_unsupported(s);
}

int main(int argc, char **argv) {
    int iters = argc > 1 ? atoi(argv[1]) : 2000;
    if (argc > 2) rng = (unsigned long long)atoll(argv[2]) * 0x9E3779B97F4A7C15ULL + 1;
    static const char *defaults[] = { "sun.png", "lake.jpg", "lake_prog.jpg", "stripes.gif", "ramp.bmp", "check.png" };
    for (int i = 0; i < 6; i++) {
        char p[128];
        snprintf(p, sizeof(p), "tools/rbrowse/img/%s", defaults[i]);
        add_seed(p);
    }
    for (int a = 3; a < argc; a++) add_seed(argv[a]);
    if (!g_nseeds) { printf("no seeds (run from the repository root)\n"); return 2; }
    int ok = 0;
    for (int it = 0; it < iters; it++) {
        if (it % 50 == 0) rb_image_reset();
        int n = make_input();
        int slot = rb_image_add("x");
        if (slot < 0) { rb_image_reset(); slot = rb_image_add("x"); }
        int st = rb_image_decode(slot, g_buf, n);
        int nw = 0, nh = 0;
        if (st == RB_IMAGE_OK) {
            ok++;
            rb_image_state(slot, &nw, &nh);
            if (nw <= 0 || nh <= 0 || nw > RB_IMAGE_DIM_MAX || nh > RB_IMAGE_DIM_MAX ||
                (long long)nw * nh > RB_IMAGE_PIXELS_MAX) {
                printf("fuzz: decoded size %dx%d past the caps (iteration %d)\n", nw, nh, it);
                g_fails++;
            }
            for (int k = 0; k < 3; k++) {
                int w = 1 + (int)(rnd() % (k == 2 ? 3000 : 700)), h = 1 + (int)(rnd() % (k == 2 ? 9000 : 500));
                RbImageView v;
                if (rb_image_view(slot, w, h, &v)) {
                    /* touch every pixel of the view: ASan checks the bounds */
                    uint32_t acc = 0;
                    for (int y = 0; y < v.h; y++)
                        for (int x = 0; x < v.w; x++) acc ^= v.px[y * v.stride + x];
                    (void)acc;
                }
            }
        }
        fuzz_strings();
        if (g_fails) {
            FILE *f = fopen("/tmp/rbfuzz_image_fail.bin", "wb");
            if (f) { fwrite(g_buf, 1, (size_t)n, f); fclose(f); }
            printf("fuzz: failing input written to /tmp/rbfuzz_image_fail.bin\n");
            return 1;
        }
    }
    printf("fuzz_image done: %d iterations, %d decoded\n", iters, ok);
    return 0;
}
