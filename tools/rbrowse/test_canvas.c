/* Host checks for <canvas>: the 2D context in dom_prelude.js, its natives
   (src/quickjs/rjs_canvas.c) and the rasterizer (src/rbrowse/canvas.c),
   driven through real pages like rbrowse does.

     T=/tmp; Q=src/quickjs
     (echo '#include <stdint.h>'; sed -n '/^uint8_t vga_font_8x8\[256\]\[8\] = {/,/^};/p' \
         src/terminal/terminal.c) > $T/font.c
     clang -g -O1 -fsanitize=address,undefined -fwrapv -DRBROWSE_HOST '-DCONFIG_VERSION="host"' \
         -I src/rbrowse -I $Q -w tools/rbrowse/test_canvas.c src/rbrowse/engine.c \
         src/rbrowse/canvas.c src/rbrowse/image.c src/stb/stb_image.c $T/font.c \
         $Q/quickjs.c $Q/libregexp.c $Q/libunicode.c $Q/cutils.c $Q/dtoa.c \
         $Q/rjs.c $Q/rjs_dom.c $Q/rjs_canvas.c $Q/dom_prelude.c -lm -o $T/rbtest_canvas && $T/rbtest_canvas

   CANVAS_PPM=dir writes each checked canvas as dir/<name>.ppm (composited
   on white) for looking at.
   CANVAS_PAGE=file.html runs that page instead (30 animation frames); with
   tools/rbrowse/fuzz_canvas.html (SEED and ITER filled in) it is a stress
   test of the natives under the sanitizers:
     for s in 1 2 3 4; do sed "s/SEED/$s$RANDOM/; s/ITER/30000/" tools/rbrowse/fuzz_canvas.html > $T/fz.html
       CANVAS_PAGE=$T/fz.html $T/rbtest_canvas; done
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "engine.h"
#include "canvas.h"
#include "image.h"
#include "rjs.h"
#include "rjs_dom.h"
#include "rjs_canvas.h"
#include "compat/qjs_port.h"
#include "compat/qjs_alloc.h"

volatile uint32_t ticks = 1000;
static QjsSink g_sink;
void qjs_set_sink(QjsSink s) { g_sink = s; }
void qjs_write(const char *s, int len) {
    if (g_sink) g_sink(s, len);
    else fwrite(s, 1, (size_t)len, stdout);
}
void qjs_alloc_reset(void) {}
void qjs_alloc_stats(size_t *a, size_t *b, size_t *c) {
    if (a) *a = 0;
    if (b) *b = 0;
    if (c) *c = 256u << 20;
}

static int g_fail, g_pass;
static char g_console[8192];
static int g_console_n;

static void capture(const char *s, int len) {
    for (int i = 0; i < len && g_console_n < (int)sizeof(g_console) - 1; i++) g_console[g_console_n++] = s[i];
    g_console[g_console_n] = 0;
}

static void page(const char *html) {
    rb_set_scripting(1);
    rb_set_canvas(1);
    rb_set_images(1);
    rb_image_reset();
    rb_canvas_reset();
    rb_open_web(html, -1);
    g_console_n = 0;
    g_console[0] = 0;
    rjs_set_output(capture);
    if (rjs_start() != RJS_OK) { printf("rjs_start failed\n"); exit(1); }
    if (rjs_dom_install("http://example.com/c.html", 0) != 0) {
        printf("prelude failed:\n%s\n", g_console);
        exit(1);
    }
    for (int i = 0; i < rb_script_count(); i++) {
        int node, len;
        const char *text;
        if (rb_script_get(i, &node, &text, &len) && len > 0) rjs_dom_exec(node, text, len, "inline");
    }
    rjs_dom_loaded();
    if (rjs_dom_take_dirty()) rb_relayout();
    rjs_canvas_flush();         /* what the host's main loop would rasterize */
}

static char g_out[262144];

static const char *js(const char *src) {
    int rc = rjs_eval(src, -1, "<check>", 4000, g_out, (int)sizeof(g_out));
    if (rc != RJS_OK) {
        static char err[262400];
        snprintf(err, sizeof(err), "!%s", g_out);
        return err;
    }
    return g_out;
}

#define EXPECT(src, want) do {                                              \
    const char *got_ = js(src);                                             \
    if (strcmp(got_, (want)) != 0) {                                        \
        printf("FAIL line %d: %s\n  got:  %s\n  want: %s\n", __LINE__, (src), got_, (want)); \
        g_fail++;                                                           \
    } else g_pass++;                                                        \
} while (0)

#define CHECK(cond) do {                                                    \
    if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); g_fail++; } \
    else g_pass++;                                                          \
} while (0)

static int g_abort_calls;
static int abort_after_3(void) { return ++g_abort_calls >= 3; }

/* The canvas element with this id: its pixels, or 0. */
static uint32_t *canvas_px(const char *id, int *w, int *h) {
    rjs_canvas_flush();
    int node = rb_find_id(id);
    int slot = rb_canvas_find(node);
    if (slot < 0) return 0;
    rb_canvas_size(slot, w, h);
    return rb_canvas_pixels(slot);
}

static void dump(const char *id, const char *name) {
    const char *dir = getenv("CANVAS_PPM");
    if (!dir) return;
    int w, h;
    uint32_t *px = canvas_px(id, &w, &h);
    if (!px) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint32_t p = px[i], a = p >> 24;
        for (int s = 16; s >= 0; s -= 8) {
            uint32_t c = (p >> s) & 255;
            fputc((int)((c * a + 255 * (255 - a)) / 255), f);
        }
    }
    fclose(f);
}

/* pixel helpers for the scripts */
static const char *PX =
    "var cv = document.getElementById('c'), ctx = cv.getContext('2d');"
    "function px(x, y) { return Array.from(ctx.getImageData(x, y, 1, 1).data).join(); }"
    "function a(x, y) { return ctx.getImageData(x, y, 1, 1).data[3]; }"
    "function count(x0, y0, w, h) { var d = ctx.getImageData(x0, y0, w, h).data, n = 0;"
    "  for (var i = 3; i < d.length; i += 4) if (d[i] > 127) n++; return n; }"
    "1";

/* CANVAS_PAGE=file.html: run that page instead of the checks for 30
   animation frames, report console output and dump canvases #anim, #pad
   and #c (with CANVAS_PPM). */
static int run_page_file(const char *path) {
    static char html[1 << 20];
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    size_t n = fread(html, 1, sizeof(html) - 1, f);
    fclose(f);
    html[n] = 0;
    page(html);
    for (int i = 0; i < 30; i++) {
        ticks += 17;
        rjs_dom_tick();
        if (rjs_dom_take_dirty()) rb_relayout();
        while (rjs_canvas_pump(5)) {}
    }
    printf("console: %s\n", g_console_n ? g_console : "(empty)");
    dump("anim", "anim");
    dump("pad", "pad");
    dump("c", "c");
    return g_console_n ? 1 : 0;
}

int main(void) {
    if (getenv("CANVAS_PAGE")) return run_page_file(getenv("CANVAS_PAGE"));
    static const char *CANVAS_PAGE =
        "<!doctype html><html><body style='margin:0'>"
        "<canvas id=c width=100 height=80></canvas><canvas id=d></canvas>"
        "<img id=im src='x.png'></body></html>";

    /* element and context basics */
    page(CANVAS_PAGE);
    EXPECT(PX, "1");
    EXPECT("[cv.width, cv.height, cv instanceof HTMLCanvasElement, cv instanceof HTMLElement].join()",
           "100,80,true,true");
    EXPECT("var d = document.getElementById('d'); [d.width, d.height].join()", "300,150");
    EXPECT("cv.getContext('2d') === ctx && ctx.canvas === cv && cv.getContext('webgl') === null", "true");
    EXPECT("document.createElement('canvas').getContext('2d') instanceof CanvasRenderingContext2D", "true");
    {
        int x, y, w, h;
        CHECK(rb_dom_rect(rb_find_id("c"), &x, &y, &w, &h) && w == 100 && h == 80);
    }

    /* styles */
    EXPECT("ctx.fillStyle", "#000000");
    EXPECT("ctx.fillStyle = '#f00'; ctx.fillStyle", "#ff0000");
    EXPECT("ctx.fillStyle = 'rgba(0, 0, 255, 0.5)'; ctx.fillStyle", "rgba(0, 0, 255, 0.502)");
    EXPECT("ctx.fillStyle = 'not a colour'; ctx.fillStyle", "rgba(0, 0, 255, 0.502)");
    EXPECT("ctx.fillStyle = 'hsl(120, 100%, 50%)'; ctx.fillStyle", "#00ff00");
    EXPECT("ctx.lineWidth = -1; ctx.lineWidth = 3; ctx.lineWidth = 'x'; ctx.lineWidth", "3");
    EXPECT("ctx.lineCap = 'round'; ctx.lineCap = 'bogus'; ctx.lineCap", "round");
    EXPECT("ctx.setLineDash([1, 2, 3]); ctx.getLineDash().join()", "1,2,3,1,2,3");
    EXPECT("ctx.setLineDash([]); ctx.font = '20px monospace'; ctx.font", "20px monospace");
    EXPECT("ctx.font = 'garbage'; ctx.font", "20px monospace");
    EXPECT("ctx.globalCompositeOperation = 'xor'; ctx.globalCompositeOperation", "xor");
    EXPECT("ctx.globalCompositeOperation = 'source-over'; ctx.globalAlpha = 2; ctx.globalAlpha", "1");

    /* fillRect, exact and antialiased edges */
    EXPECT("ctx.fillStyle = '#f00'; ctx.fillRect(10, 10, 20, 20); [px(15, 15), px(5, 5), px(29, 29), px(30, 30)].join(' ')",
           "255,0,0,255 0,0,0,0 255,0,0,255 0,0,0,0");
    EXPECT("ctx.fillRect(40.5, 10, 10, 10); [a(40, 12), a(41, 12), a(50, 12)].join()", "128,255,128");
    {
        int w, h;
        uint32_t *p = canvas_px("c", &w, &h);
        CHECK(p && w == 100 && h == 80 && p[15 * 100 + 15] == 0xFFFF0000u);
        CHECK(rb_canvas_take_dirty() == 1 && rb_canvas_take_dirty() == 0);
    }
    /* clearRect, under a transform */
    EXPECT("ctx.translate(10, 10); ctx.clearRect(0, 0, 5, 5); ctx.setTransform(1, 0, 0, 1, 0, 0);"
           "[a(12, 12), a(16, 16)].join()", "0,255");

    /* paths: a circle */
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.beginPath(); ctx.arc(50, 40, 20, 0, Math.PI * 2); ctx.fill();"
           "var n = count(0, 0, 100, 80); [n > 1230 && n < 1290, a(50, 40), a(50, 18), a(50, 21)].join()",
           "true,255,0,255");
    /* a half circle: arc direction */
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.beginPath(); ctx.arc(50, 40, 20, 0, Math.PI); ctx.fill();"
           "[a(50, 50), a(50, 30)].join()", "255,0");
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.beginPath(); ctx.arc(50, 40, 20, 0, Math.PI, true); ctx.fill();"
           "[a(50, 50), a(50, 30)].join()", "0,255");
    /* even-odd: a hole */
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.beginPath(); ctx.rect(10, 10, 60, 60); ctx.rect(30, 30, 20, 20);"
           "ctx.fill('evenodd'); [a(20, 20), a(40, 40)].join()", "255,0");
    EXPECT("ctx.fill(); a(40, 40)", "255");   /* nonzero: same direction, filled */
    EXPECT("[ctx.isPointInPath(40, 40), ctx.isPointInPath(40, 40, 'evenodd'), ctx.isPointInPath(5, 5)].join()",
           "true,false,false");
    /* curves */
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.beginPath(); ctx.moveTo(10, 70); ctx.quadraticCurveTo(50, -10, 90, 70);"
           "ctx.closePath(); ctx.fill(); [a(50, 60), a(50, 32), a(50, 27)].join()", "255,255,0");
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.beginPath(); ctx.moveTo(10, 70); ctx.bezierCurveTo(10, 0, 90, 0, 90, 70);"
           "ctx.fill(); [a(50, 60), a(50, 30), a(50, 10)].join()", "255,255,0");

    /* strokes */
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.lineWidth = 4; ctx.lineCap = 'butt'; ctx.strokeStyle = '#00f';"
           "ctx.beginPath(); ctx.moveTo(10, 40); ctx.lineTo(90, 40); ctx.stroke();"
           "[px(50, 38), px(50, 41), a(50, 37), a(50, 42), a(9, 40), a(90, 40)].join(' ')",
           "0,0,255,255 0,0,255,255 0 0 0 0");
    EXPECT("ctx.lineCap = 'square'; ctx.stroke(); [a(9, 40), a(90, 40), a(92, 40)].join()", "255,255,0");
    EXPECT("ctx.lineCap = 'round'; ctx.clearRect(0, 0, 100, 80); ctx.stroke(); [a(8, 40) > 200, a(7, 37)].join()", "true,0");
    EXPECT("[ctx.isPointInStroke(50, 41), ctx.isPointInStroke(50, 45)].join()", "true,false");
    /* joins: miter fills the outer corner, bevel cuts it */
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.lineWidth = 10; ctx.lineCap = 'butt'; ctx.lineJoin = 'miter';"
           "ctx.beginPath(); ctx.moveTo(20, 60); ctx.lineTo(20, 20); ctx.lineTo(60, 20); ctx.stroke(); a(16, 16)", "255");
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.lineJoin = 'bevel'; ctx.stroke(); [a(16, 16), a(22, 22)].join()", "0,255");
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.lineJoin = 'round'; ctx.stroke(); [a(15, 15), a(17, 17)].join()", "0,255");
    /* a closed path joins at its start */
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.lineJoin = 'miter'; ctx.strokeRect(20, 20, 40, 30); [a(16, 16), a(40, 35)].join()",
           "255,0");
    /* dashes */
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.lineWidth = 2; ctx.setLineDash([10, 10]);"
           "ctx.beginPath(); ctx.moveTo(0, 40); ctx.lineTo(100, 40); ctx.stroke(); ctx.setLineDash([]);"
           "[a(5, 40), a(15, 40), a(25, 40), a(35, 40)].join()", "255,0,255,0");
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.setLineDash([10, 10]); ctx.lineDashOffset = 5; ctx.stroke();"
           "ctx.setLineDash([]); ctx.lineDashOffset = 0; [a(2, 40), a(7, 40), a(12, 40)].join()", "255,0,0");

    /* transforms and save/restore */
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.save(); ctx.fillStyle = '#0f0'; ctx.translate(50, 40); ctx.scale(2, 2);"
           "ctx.fillRect(-5, -5, 10, 10); ctx.restore(); [px(41, 31), a(39, 40), a(60, 40), ctx.fillStyle].join(' ')",
           "0,255,0,255 0 0 #ff0000");
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.save(); ctx.translate(50, 40); ctx.rotate(Math.PI / 4);"
           "ctx.fillRect(-10, -10, 20, 20); ctx.restore(); [a(50, 27), a(50, 24), a(40, 30)].join()", "255,0,0");
    EXPECT("var m = ctx.getTransform(); [m.a, m.e, m.isIdentity].join()", "1,0,true");
    EXPECT("ctx.setTransform(2, 0, 0, 2, 5, 5); var m2 = ctx.getTransform(); ctx.resetTransform(); [m2.a, m2.e].join()", "2,5");
    /* the current path keeps the transform each point was added under */
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.beginPath(); ctx.moveTo(10, 10); ctx.translate(50, 0); ctx.lineTo(10, 10);"
           "ctx.resetTransform(); ctx.lineTo(10, 20); ctx.closePath(); ctx.lineWidth = 1; ctx.fill(); [a(30, 12), a(30, 25)].join()",
           "255,0");

    /* globalAlpha and compositing */
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.fillStyle = '#fff'; ctx.fillRect(0, 0, 100, 80);"
           "ctx.globalAlpha = 0.5; ctx.fillStyle = '#000'; ctx.fillRect(0, 0, 10, 10); ctx.globalAlpha = 1; px(5, 5)",
           "127,127,127,255");
    EXPECT("ctx.globalCompositeOperation = 'destination-out'; ctx.fillRect(20, 20, 10, 10);"
           "ctx.globalCompositeOperation = 'source-over'; [a(25, 25), a(35, 25)].join()", "0,255");
    EXPECT("ctx.globalCompositeOperation = 'copy'; ctx.fillStyle = 'rgba(255, 0, 0, 0.5)'; ctx.fillRect(40, 40, 10, 10);"
           "ctx.globalCompositeOperation = 'source-over'; px(45, 45)", "255,0,0,128");

    /* gradients */
    EXPECT("ctx.clearRect(0, 0, 100, 80); var g = ctx.createLinearGradient(0, 0, 100, 0);"
           "g.addColorStop(0, '#000'); g.addColorStop(1, '#fff'); ctx.fillStyle = g; ctx.fillRect(0, 0, 100, 80);"
           "var p0 = ctx.getImageData(0, 5, 1, 1).data[0], p50 = ctx.getImageData(50, 5, 1, 1).data[0],"
           "    p99 = ctx.getImageData(99, 5, 1, 1).data[0];"
           "[p0 < 4, p50 > 120 && p50 < 136, p99 > 250, ctx.fillStyle === g].join()", "true,true,true,true");
    EXPECT("var e1, e2; try { g.addColorStop(2, '#000'); } catch (e) { e1 = e.name; }"
           "try { g.addColorStop(0.5, 'nope'); } catch (e) { e2 = e.name; } [e1, e2].join()", "IndexSizeError,SyntaxError");
    EXPECT("ctx.clearRect(0, 0, 100, 80); var rg = ctx.createRadialGradient(50, 40, 0, 50, 40, 30);"
           "rg.addColorStop(0, 'rgb(255, 255, 0)'); rg.addColorStop(1, 'rgba(255, 255, 0, 0)');"
           "ctx.fillStyle = rg; ctx.fillRect(0, 0, 100, 80); [a(50, 40) > 245, a(65, 40) > 110 && a(65, 40) < 145, a(85, 40)].join()",
           "true,true,0");
    /* a gradient follows the transform it is painted under */
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.save(); ctx.translate(50, 0); ctx.fillStyle = g; ctx.fillRect(-50, 0, 100, 10);"
           "ctx.restore(); var q = ctx.getImageData(75, 5, 1, 1).data[0]; q > 56 && q < 72", "true");

    /* clip (bounding box) */
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.save(); ctx.beginPath(); ctx.rect(20, 20, 30, 30); ctx.clip();"
           "ctx.fillStyle = '#000'; ctx.fillRect(0, 0, 100, 80); ctx.restore(); [a(25, 25), a(10, 10), a(55, 55)].join()",
           "255,0,0");
    EXPECT("ctx.fillStyle = '#000'; ctx.fillRect(0, 0, 5, 5); a(2, 2)", "255");   /* restore drops the clip */

    /* pixels */
    EXPECT("ctx.clearRect(0, 0, 100, 80); var id = ctx.createImageData(2, 2); id.data.set([1,2,3,255, 4,5,6,255, 7,8,9,255, 10,11,12,0]);"
           "ctx.putImageData(id, 10, 10); [px(10, 10), px(11, 11), px(11, 10)].join(' ')", "1,2,3,255 0,0,0,0 4,5,6,255");
    EXPECT("ctx.putImageData(id, 20, 20, 1, 0, 1, 2); [a(20, 20), px(21, 20)].join(' ')", "0 4,5,6,255");
    EXPECT("var gd = ctx.getImageData(-1, -1, 3, 3); [gd.width, gd.data.length, gd.data[3]].join()", "3,36,0");
    EXPECT("var ex; try { ctx.getImageData(0, 0, 0, 5); } catch (e) { ex = e.name; } ex", "IndexSizeError");
    EXPECT("new ImageData(3, 2).data.length + ',' + new ImageData(new Uint8ClampedArray(8), 2).height", "24,1");

    /* putImageData far off the canvas: the offsets used to wrap past the
       clamps (2^31 - 1 + x1 < 0) and write gigabytes away from the arena;
       a wrapped dirty-rect origin read before the image */
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.fillStyle = '#010203'; ctx.fillRect(0, 0, 100, 80);"
           "var big = [2147483647, -2147483648, 4294967296 * 3 + 2147483647, -2147483647, 2147483646];"
           "for (var bx of big) for (var by of [0, 3].concat(big)) { ctx.putImageData(id, bx, by); ctx.putImageData(id, by, bx); }"
           "ctx.putImageData(id, 5, 5, 2147483647, 0, 2147483650, 1); ctx.putImageData(id, 5, 5, 0, 2147483647, 1, 2147483650);"
           "ctx.putImageData(id, 5, 5, -2147483648, -2147483648, 2147483649, 2147483649);"
           "ctx.putImageData(id, -1, 79, -5, -5, 100, 100);"
           "[px(0, 0), px(5, 5), px(99, 79), px(0, 79), px(1, 79), count(0, 0, 100, 80)].join(' ')",
           /* only the last put touches the canvas: its (1,0) pixel at (0,79) */
           "1,2,3,255 1,2,3,255 1,2,3,255 4,5,6,255 1,2,3,255 8000");
    /* an ImageData is a plain object: a page can change its fields. The
       natives take the buffer only after every number is converted (a
       valueOf may detach it) and size-check it in 64 bits. */
    EXPECT("var t1 = ctx.createImageData(2, 2); t1.width = 65536; t1.height = 65536; ctx.putImageData(t1, 0, 0);"
           "var t2 = ctx.createImageData(2, 2); t2.width = {valueOf() { t2.data.buffer.transfer(); return 2; }};"
           "ctx.putImageData(t2, 0, 0); t2.data.length + ',' + px(0, 0)", "0,1,2,3,255");
    EXPECT("var keep = ctx.__s.m; ctx.__s.m = [1, 0, 0, 1, 0, {valueOf() { ctx.__rp.d.buffer.transfer(); return 0; }}];"
           "ctx.fillStyle = '#fff'; ctx.fillRect(0, 0, 5, 5); ctx.__rp = null; ctx.__s.m = keep; px(0, 0)", "1,2,3,255");

    /* toDataURL is a PNG the image decoder reads back */
    {
        const char *url = js("ctx.clearRect(0, 0, 100, 80); ctx.fillStyle = '#123456'; ctx.fillRect(0, 0, 3, 2); cv.toDataURL()");
        CHECK(strncmp(url, "data:image/png;base64,", 22) == 0);
        static uint8_t png[200000];
        int n = rb_image_data_url(url, png, (int)sizeof(png));
        int slot = rb_image_add("test://png");
        int nw = 0, nh = 0;
        CHECK(n > 0 && slot >= 0 && rb_image_decode(slot, png, n) == RB_IMAGE_OK);
        if (g_fail) printf("  png bytes %d slot %d: %s\n", n, slot, rb_image_error());
        CHECK(rb_image_state(slot, &nw, &nh) == RB_IMAGE_OK && nw == 100 && nh == 80);
        RbImageView v;
        CHECK(rb_image_view(slot, 100, 80, &v) && v.px[0] == 0xFF123456u && v.px[3] == 0 && (v.px[100] >> 24) == 255);
    }

    /* drawImage: another canvas, scaled */
    EXPECT("var d2 = document.getElementById('d'); d2.width = 4; d2.height = 4; var dc = d2.getContext('2d');"
           "dc.fillStyle = '#0f0'; dc.fillRect(0, 0, 2, 4); ctx.clearRect(0, 0, 100, 80); ctx.imageSmoothingEnabled = false;"
           "ctx.drawImage(d2, 10, 10, 40, 40); [px(15, 15), a(35, 15), a(5, 5)].join(' ')", "0,255,0,255 0 0");
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.drawImage(d2, 1, 0, 2, 4, 0, 0, 20, 40); [a(5, 5), a(15, 5)].join()", "255,0");
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.drawImage(d2, 0, 0); [a(1, 1), a(3, 1)].join()", "255,0");
    /* drawImage of a canvas onto itself */
    EXPECT("ctx.drawImage(cv, 0, 0, 4, 4, 50, 50, 4, 4); [a(51, 51), a(53, 51)].join()", "255,0");
    /* drawImage: an <img> once decoded; nothing before */
    EXPECT("var im = document.getElementById('im'); ctx.clearRect(0, 0, 100, 80); ctx.drawImage(im, 0, 0); a(0, 0)", "0");
    {
        static uint8_t file[4 * 1024 * 1024];
        FILE *f = fopen("tools/rbrowse/img/check.png", "rb");
        int n = f ? (int)fread(file, 1, sizeof(file), f) : 0;
        if (f) fclose(f);
        int node = rb_find_id("im");
        int slot = rb_image_add("http://example.com/x.png");
        int nw = 0, nh = 0;
        CHECK(n > 0 && rb_image_decode(slot, file, n) == RB_IMAGE_OK && rb_image_state(slot, &nw, &nh) == RB_IMAGE_OK);
        rb_dom_img_set(node, RB_IMG_OK, slot, nw, nh);
        rjs_dom_image_event(node, 1);
        const uint32_t *ip;
        int iw, ih, inw, inh;
        CHECK(rb_image_node_pixels(node, &ip, &iw, &ih, &inw, &inh));
        char src[200];
        snprintf(src, sizeof(src),
                 "ctx.clearRect(0, 0, 100, 80); ctx.drawImage(im, 0, 0);"
                 "var q = ctx.getImageData(0, 0, 1, 1).data; (q[3] << 24 | q[0] << 16 | q[1] << 8 | q[2]) >>> 0");
        char want[32];
        snprintf(want, sizeof(want), "%u", ip[0]);
        EXPECT(src, want);
        EXPECT("im.naturalWidth > 0 && ctx.getImageData(0, 0, im.naturalWidth, 1).data.length === im.naturalWidth * 4", "true");
    }

    /* text */
    EXPECT("ctx.font = '16px monospace'; var tm = ctx.measureText('Hello'); tm.width", "40");
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.fillStyle = '#000'; ctx.textBaseline = 'top'; ctx.fillText('HI', 10, 10);"
           "var tn = count(10, 10, 16, 16); [tn > 20, count(30, 10, 20, 16), count(0, 30, 100, 50)].join()", "true,0,0");
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.textAlign = 'right'; ctx.fillText('HI', 50, 10); ctx.textAlign = 'start';"
           "[count(34, 10, 16, 16) > 20, count(50, 10, 20, 16)].join()", "true,0");
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.fillText('HIHIHI', 10, 10, 24); [count(10, 10, 24, 16) > 30, count(35, 10, 30, 16)].join()",
           "true,0");
    dump("c", "text");

    /* Path2D */
    EXPECT("ctx.clearRect(0, 0, 100, 80); var p2 = new Path2D('M10 10 h20 v20 h-20 z'); ctx.fill(p2);"
           "[a(15, 15), a(35, 15), ctx.isPointInPath(p2, 20, 20)].join()", "255,0,true");
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.translate(40, 0); ctx.fill(p2); ctx.resetTransform(); [a(15, 15), a(55, 15)].join()",
           "0,255");
    EXPECT("ctx.clearRect(0, 0, 100, 80); var p3 = new Path2D(); p3.arc(50, 40, 10, 0, 7); var p4 = new Path2D(p3);"
           "p4.addPath(p2); ctx.fill(p4); [a(50, 40), a(15, 15)].join()", "255,255");
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.fill(new Path2D('M10 40 A 20 20 0 0 1 50 40 Z')); [a(30, 35), a(30, 45)].join()",
           "255,0");
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.beginPath(); ctx.roundRect(10, 10, 40, 40, 10); ctx.fill();"
           "[a(30, 30), a(11, 11), a(20, 11)].join()", "255,0,255");
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.beginPath(); ctx.moveTo(10, 10); ctx.arcTo(60, 10, 60, 60, 20);"
           "ctx.lineTo(60, 60); ctx.lineTo(10, 60); ctx.fill(); [a(58, 12), a(45, 15)].join()", "0,255");
    EXPECT("ctx.clearRect(0, 0, 100, 80); ctx.beginPath(); ctx.ellipse(50, 40, 30, 10, 0, 0, Math.PI * 2); ctx.fill();"
           "[a(75, 40), a(50, 48), a(50, 55)].join()", "255,255,0");

    /* setting the size clears the pixels and resets the state */
    EXPECT("ctx.fillStyle = '#f00'; ctx.translate(5, 5); ctx.fillRect(0, 0, 10, 10); cv.width = cv.width;"
           "[a(7, 7), ctx.fillStyle, ctx.getTransform().e].join()", "0,#000000,0");
    EXPECT("cv.width = 50; [cv.width, ctx.getImageData(0, 0, 50, 1).data.length].join()", "50,200");
    rb_relayout();
    {
        int x, y, w, h;
        CHECK(rb_dom_rect(rb_find_id("c"), &x, &y, &w, &h) && w == 50);
    }

    /* hostile input: non-finite numbers, huge paths and sizes */
    EXPECT("ctx.fillRect(NaN, 0, 10, 10); ctx.moveTo(Infinity, 0); ctx.lineTo(0, NaN); ctx.arc(0, 0, 1e308, 0, 7);"
           "ctx.fill(); ctx.stroke(); ctx.scale(Infinity, 1); ctx.drawImage(cv, NaN, 0); 'ok'", "ok");
    EXPECT("var er; try { ctx.arc(0, 0, -1, 0, 1); } catch (e) { er = e.name; } er", "IndexSizeError");
    EXPECT("ctx.beginPath(); for (var i = 0; i < 40000; i++) ctx.lineTo(Math.random() * 50, Math.random() * 80);"
           "ctx.lineWidth = 3; ctx.lineJoin = 'round'; ctx.stroke(); ctx.fill(); 'ok'", "ok");
    EXPECT("var big = document.createElement('canvas'); big.width = 100000; var bc = big.getContext('2d');"
           "bc.fillRect(0, 0, 10, 10); [big.width, bc.getImageData(0, 0, 1, 1).data[3], big.toDataURL()].join()",
           "65535,0,data:,");
    EXPECT("var cs = []; for (var i = 0; i < 40; i++) { var e = document.createElement('canvas'); e.width = e.height = 8;"
           "cs.push(e.getContext('2d')); } cs[39].fillRect(0, 0, 8, 8); cs[39].getImageData(0, 0, 1, 1).data[3]", "0");
    EXPECT("1 + 1", "2");

    /* mouse coordinates on a canvas: offsetX/offsetY are canvas pixels, and
       clientY - rect.top scaled by height / rect.height lands on the same one */
    page("<body style='margin:0'><p>x</p><canvas id=c width=100 height=50></canvas></body>");
    EXPECT("var cv = document.getElementById('c'), got = ''; cv.addEventListener('click', e => {"
           "var r = cv.getBoundingClientRect(); got = [e.offsetX, e.offsetY,"
           "Math.round((e.clientY - r.top) * cv.height / r.height)].join(); }); 1", "1");
    {
        int x, y, w, h;
        CHECK(rb_dom_rect(rb_find_id("c"), &x, &y, &w, &h) && h == 50);
        rb_dispatch_ex(rb_find_id("c"), RB_EV_CLICK, 0, x + 30, y + 20, 0, 1, 0);
    }
    EXPECT("got", "30,20,20");

    /* a script-drawn page: the draw happens during load, the host sees dirty pixels */
    page("<canvas id=c width=64 height=64></canvas><script>"
         "var x = document.getElementById('c').getContext('2d');"
         "x.fillStyle = 'orange'; x.beginPath(); x.arc(32, 32, 30, 0, Math.PI * 2); x.fill();"
         "x.strokeStyle = 'navy'; x.lineWidth = 4; x.stroke();"
         "x.fillStyle = 'white'; x.font = '16px sans-serif'; x.textAlign = 'center'; x.textBaseline = 'middle';"
         "x.fillText('JS', 32, 32);"
         "requestAnimationFrame(function (t) { x.fillStyle = 'red'; x.fillRect(0, 0, 4, 4); });"
         "</script>");
    CHECK(g_console_n == 0);
    if (g_console_n) printf("console: %s\n", g_console);
    {
        int w, h;
        uint32_t *p = canvas_px("c", &w, &h);
        CHECK(p && w == 64 && p[32 * 64 + 2] == 0xFF000080u && (p[32 * 64 + 10] >> 24) == 255);
        CHECK(rb_canvas_take_dirty());
        ticks += 20;
        rjs_dom_tick();
        /* drawing is queued until the host rasterizes it */
        CHECK(p[0] != 0xFFFF0000u && rjs_canvas_pending());
        CHECK(rjs_canvas_pump(5) == 0);
        CHECK(p[0] == 0xFFFF0000u && rb_canvas_take_dirty());
        dump("c", "page");
    }

    /* the screen shows the last presented frame: drawing (even rasterized)
       stays invisible until rb_canvas_present */
    {
        page("<canvas id=c width=10 height=10></canvas><script>"
             "document.getElementById('c').getContext('2d').fillRect(0, 0, 10, 10);</script>");
        int node = rb_find_id("c");
        RbImageView v;
        CHECK(rb_canvas_view_node(node, 10, 10, &v) && v.px[0] == 0);
        CHECK(rb_canvas_present() == 1 && rb_canvas_node_dirty(node));
        CHECK(rb_canvas_view_node(node, 10, 10, &v) && v.px[0] == 0xFF000000u);
        rb_canvas_clean();
        CHECK(!rb_canvas_node_dirty(node) && rb_canvas_present() == 0);
    }

    /* drawing rasterized one band of rows at a time (the host's pump with no
       time to spare) gives exactly the pixels of drawing it in one go */
    {
        static const char *scene =
            "var x = document.getElementById('c').getContext('2d'); x.clearRect(0, 0, 300, 260);"
            "var g = x.createLinearGradient(0, 0, 300, 260); g.addColorStop(0, '#f00'); g.addColorStop(1, 'rgba(0,0,255,0.5)');"
            "x.fillStyle = g; x.fillRect(5, 5, 290, 250);"
            "x.save(); x.translate(150, 130); x.rotate(0.7); x.beginPath(); x.arc(0, 0, 100, 0, 5); x.lineWidth = 9;"
            "x.lineJoin = 'round'; x.setLineDash([12, 5]); x.strokeStyle = 'rgba(0,255,0,0.6)'; x.stroke(); x.restore();"
            "x.beginPath(); for (var i = 0; i < 400; i++) x.lineTo((i * 37) % 300, (i * 53) % 260); x.fillStyle = 'rgba(255,255,0,0.4)';"
            "x.fill('evenodd'); x.font = '24px sans-serif'; x.fillStyle = '#000'; x.fillText('banded', 40, 200);"
            "x.globalAlpha = 0.5; x.drawImage(x.canvas, 0, 0, 150, 130, 150, 130, 150, 130); x.globalAlpha = 1;"
            "x.save(); x.beginPath(); x.rect(20, 100, 120, 70); x.clip(); x.fillStyle = 'navy'; x.fillRect(0, 0, 300, 260); x.restore(); 1";
        page("<canvas id=c width=300 height=260></canvas>");
        js(scene);
        CHECK(rjs_canvas_pending());
        int calls = 0;
        while (rjs_canvas_pump(0) && calls < 100000) calls++;
        int w, h;
        static uint32_t banded[300 * 260];
        uint32_t *p = canvas_px("c", &w, &h);
        CHECK(p && w == 300 && h == 260 && calls > 8);
        if (p) memcpy(banded, p, sizeof(banded));
        js(scene);
        rjs_canvas_flush();
        p = canvas_px("c", &w, &h);
        int diff = 0;
        for (int i = 0; p && i < 300 * 260; i++) if (p[i] != banded[i]) diff++;
        if (diff) printf("  %d pixels differ between banded and whole rasterization\n", diff);
        CHECK(p && diff == 0);
    }

    /* a long fill polls the abort hook and stops when the script is out of
       time (the kernel's hook is rjs_over_budget) */
    {
        page("<canvas id=c width=64 height=64></canvas>");
        int slot = rb_canvas_open(rb_find_id("c"));
        CHECK(rb_canvas_resize(slot, 64, 64));
        rb_cv_set_abort(abort_after_3);
        g_abort_calls = 0;
        rb_cv_begin();
        rb_cv_line(0, 0, 64, 0); rb_cv_line(64, 0, 64, 64); rb_cv_line(64, 64, 0, 64); rb_cv_line(0, 64, 0, 0);
        RbCvPaint p = {0};
        p.kind = RB_CV_SOLID;
        p.argb = 0xFF00FF00u;
        rb_cv_fill(slot, 0, &p, 255, RB_CV_OVER, 0);
        uint32_t *px = rb_canvas_pixels(slot);
        /* rows 0..7 and 8..15 filled, stopped at row 16 */
        CHECK(px && px[15 * 64] == 0xFF00FF00u && px[16 * 64] == 0 && px[63 * 64] == 0);
        rb_cv_set_abort(0);
        rb_cv_fill(slot, 0, &p, 255, RB_CV_OVER, 0);
        CHECK(px[63 * 64 + 63] == 0xFF00FF00u);
    }

    printf("%d passed, %d failed\n", g_pass, g_fail);
    if (!g_fail) printf("rbrowse canvas checks passed\n");
    return g_fail ? 1 : 0;
}
