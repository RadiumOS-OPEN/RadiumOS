/* Host checks for the browser's JavaScript DOM: QuickJS + rjs + rjs_dom +
   dom_prelude driving the real engine, natively (no kernel).

     T=/tmp; Q=src/quickjs   # $T/font.c: made as in test_paint.c
     clang -g -O1 -fsanitize=address,undefined -fwrapv -DRBROWSE_HOST '-DCONFIG_VERSION="host"' \
         -I src/rbrowse -I $Q -w tools/rbrowse/test_js.c src/rbrowse/engine.c \
         $Q/quickjs.c $Q/libregexp.c $Q/libunicode.c $Q/cutils.c $Q/dtoa.c \
         $Q/rjs.c $Q/rjs_dom.c $Q/rjs_canvas.c $Q/dom_prelude.c \
         src/rbrowse/canvas.c src/rbrowse/image.c src/stb/stb_image.c $T/font.c -lm -o /tmp/rbtest_js && /tmp/rbtest_js

   (Regenerate dom_prelude.c first if dom_prelude.js changed:
    python3 tools/rbrowse/js2c.py src/quickjs/dom_prelude.js src/quickjs/dom_prelude.c rjs_dom_prelude)
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "engine.h"
#include "rjs.h"
#include "rjs_dom.h"
#include "compat/qjs_port.h"
#include "compat/qjs_alloc.h"

/* ---- the kernel pieces rjs expects ---- */
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

static int fake_fetch(const char *url, char *out, int cap) {
    const char *b = 0;
    if (strcmp(url, "http://cdn.example.net/lib.js") == 0) b = "window.libLoaded = (window.libLoaded || 0) + 1;";
    if (b) {
        int n = (int)strlen(b);
        memcpy(out, b, (size_t)n);
        return n;
    }
    if (strcmp(url, "http://example.com/data.json") == 0) {
        b = "{\"n\":42}";
        int n = (int)strlen(b);
        if (n >= cap) return -1;
        memcpy(out, b, (size_t)n);
        return n;
    }
    return -1;
}

/* Open a page the way rbrowse does and run its scripts. */
static void page(const char *html) {
    rb_set_scripting(1);
    rb_open_web(html, -1);
    g_console_n = 0;
    g_console[0] = 0;
    rjs_set_output(capture);
    if (rjs_start() != RJS_OK) { printf("rjs_start failed\n"); exit(1); }
    if (rjs_dom_install("http://example.com/dir/page.html", fake_fetch) != 0) {
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
}

static char g_out[8192];

static const char *js(const char *src) {
    int rc = rjs_eval(src, -1, "<check>", 2000, g_out, (int)sizeof(g_out));
    if (rc != RJS_OK) {
        static char err[8300];
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

static void tick(int ms) {
    ticks += (uint32_t)ms;
    rjs_dom_tick();
    if (rjs_dom_take_dirty()) rb_relayout();
}

int main(void) {
    /* tree basics and queries */
    page("<!doctype html><html><head><title>T</title></head><body>"
         "<div id=a class='x y'>Hello <b>world</b></div><ul><li>1<li>2<li>3</ul>"
         "<my-widget data-k=v>c</my-widget></body></html>");
    EXPECT("document.getElementById('a').textContent", "Hello world");
    EXPECT("document.title", "T");
    EXPECT("document.querySelectorAll('li').length", "3");
    EXPECT("document.querySelector('ul > li:last-child').textContent", "3");
    EXPECT("document.body.children.length", "3");
    EXPECT("document.querySelector('my-widget').tagName", "MY-WIDGET");
    EXPECT("document.querySelector('my-widget').dataset.k", "v");
    EXPECT("document.getElementById('a') === document.getElementById('a')", "true");
    EXPECT("document.getElementById('a').classList.contains('y')", "true");
    EXPECT("try { document.querySelector('[[') } catch (e) { e.name }", "SyntaxError");
    EXPECT("document.getElementsByClassName('x')[0].id", "a");
    EXPECT("document.getElementById('a').parentNode === document.body", "true");
    EXPECT("document.documentElement.tagName", "HTML");
    /* the viewport comes from rb_set_viewport (304x200 by default = 304x400 css) */
    EXPECT("innerWidth + 'x' + innerHeight + ' ' + screen.colorDepth", "304x400 24");
    EXPECT("matchMedia('(min-width: 600px)').matches", "false");

    /* the 640x360 screen's viewport */
    rb_set_viewport(622, 332);
    page("<!doctype html><body>v</body>");
    EXPECT("innerWidth + 'x' + innerHeight", "622x664");
    EXPECT("matchMedia('(min-width: 600px)').matches", "true");
    rb_set_viewport(RB_VIEW_W_DEFAULT, RB_VIEW_H_DEFAULT);
    page("<!doctype html><html><head><title>T</title></head><body>"
         "<div id=a class='x y'>Hello <b>world</b></div><ul><li>1<li>2<li>3</ul>"
         "<my-widget data-k=v>c</my-widget></body></html>");

    /* mutation */
    EXPECT("var d = document.createElement('p'); d.id = 'n'; d.textContent = 'new';"
           "document.body.appendChild(d); document.querySelector('#n').textContent", "new");
    EXPECT("var a = document.getElementById('a'); a.classList.toggle('x'); a.className", "y");
    EXPECT("a.style.display = 'none'; a.getAttribute('style')", "display: none");
    EXPECT("a.style.backgroundColor = 'red'; a.style.backgroundColor", "red");
    EXPECT("a.style.display = ''; a.getAttribute('style')", "background-color: red");
    EXPECT("a.setAttribute('aria-expanded', 'true'); a.getAttribute('aria-expanded')", "true");
    EXPECT("document.querySelectorAll('[aria-expanded=true]').length", "1");
    EXPECT("a.innerHTML = '<i>x</i><br>y'; a.innerHTML", "<i>x</i><br>y");
    EXPECT("a.firstChild.tagName + a.childNodes.length", "I3");
    EXPECT("var li = document.querySelector('li'); var ul = li.parentNode; ul.removeChild(li);"
           "ul.appendChild(li); [...ul.children].map(e => e.textContent).join()", "2,3,1");
    EXPECT("li.remove(); ul.children.length + ',' + li.isConnected", "2,false");
    EXPECT("var f = document.createDocumentFragment(); f.appendChild(document.createElement('em'));"
           "f.appendChild(document.createElement('em')); ul.appendChild(f); ul.querySelectorAll('em').length", "2");
    EXPECT("a.insertAdjacentHTML('beforebegin', '<hr id=h>'); a.previousElementSibling.id", "h");
    EXPECT("a.cloneNode(true).innerHTML === a.innerHTML", "true");
    EXPECT("var r1 = document.createElement('p'); r1.id = 'r1'; ul.replaceChild(r1, ul.firstElementChild);"
           "ul.firstElementChild.id", "r1");
    EXPECT("a.outerHTML.slice(0, 7)", "<div id");

    /* events: JS dispatch, el.click(), engine dispatch */
    EXPECT("var hits = []; a.addEventListener('click', e => hits.push('a:' + e.eventPhase));"
           "document.body.addEventListener('click', e => hits.push('body'), true);"
           "a.click(); hits.join()", "body,a:2");
    EXPECT("var got = ''; a.addEventListener('ping', e => got = e.detail);"
           "a.dispatchEvent(new CustomEvent('ping', {detail: 7})); got", "7");
    EXPECT("var stopped = 0; document.body.addEventListener('click', () => stopped++);"
           "a.addEventListener('click', e => e.stopPropagation()); a.click(); stopped", "0");
    {
        int a = rb_find_id("a");
        js("hits = []; a.addEventListener('mousedown', e => { hits.push(e.type + e.clientX); e.preventDefault(); })");
        rb_dispatch_ex(a, RB_EV_MOUSEDOWN, 0, 5, 6, 0, 1, 0);
        EXPECT("hits.join()", "mousedown5");
        if (!rb_last_dispatch_prevented()) { printf("FAIL: preventDefault did not reach the engine\n"); g_fail++; }
        else g_pass++;
    }
    EXPECT("var n = 0; a.onclick = () => n++; a.click(); a.click(); n", "2");

    /* timers, promises, async */
    EXPECT("var t = []; setTimeout(() => t.push('b'), 50); setTimeout(() => t.push('a'), 10);"
           "Promise.resolve().then(() => t.push('p')); t.join()", "");
    EXPECT("t.join()", "p");
    tick(20);
    EXPECT("t.join()", "p,a");
    tick(40);
    EXPECT("t.join()", "p,a,b");
    EXPECT("var iv = 0, id = setInterval(() => { if (++iv == 3) clearInterval(id) }, 10); 0", "0");
    for (int i = 0; i < 6; i++) tick(10);
    EXPECT("iv", "3");

    /* fetch over the host's fetcher */
    EXPECT("var fr; fetch('/data.json').then(r => r.json()).then(j => fr = j.n); 0", "0");
    tick(1);
    EXPECT("fr", "42");
    EXPECT("var xr; var x = new XMLHttpRequest(); x.open('GET', '../data.json'); x.onload = () => xr = x.responseText; x.send(); 0", "0");
    tick(1);
    EXPECT("xr", "{\"n\":42}");

    EXPECT("var cr; fetch('http://other.example/data.json').catch(e => cr = 'blocked'); 0", "0");
    tick(1);
    EXPECT("cr", "blocked");
    EXPECT("var u = new Uint32Array(4); crypto.getRandomValues(u); u.some(x => x !== 0)", "true");
    EXPECT("/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(crypto.randomUUID())", "true");

    /* location, URL, navigation */
    EXPECT("location.hostname + location.pathname", "example.com/dir/page.html");
    EXPECT("new URL('../x?q=1#h', 'http://e.com/a/b/c').href", "http://e.com/a/x?q=1#h");
    EXPECT("new URLSearchParams('a=1&b=x%20y').get('b')", "x y");
    {
        char nav[256];
        js("location.href = 'other.html'");
        if (!rb_take_nav(nav, sizeof(nav)) || strcmp(nav, "http://example.com/dir/other.html") != 0) {
            printf("FAIL: location.href navigation (%s)\n", nav);
            g_fail++;
        } else g_pass++;
    }

    /* load order, document.write, inline handlers, errors */
    page("<body><p id=o>0</p>"
         "<script>var order = ['s1']; document.addEventListener('DOMContentLoaded', () => order.push('dcl'));"
         "window.onload = () => order.push('load');</script>"
         "<script>document.write('<span id=w>written</span>'); order.push('s2');</script>"
         "<button id=b onclick=\"document.getElementById('o').textContent = +document.getElementById('o').textContent + 1\">+</button>"
         "<script>throw new Error('boom')</script>"
         "</body>");
    EXPECT("order.join()", "s1,s2,dcl");
    rb_dispatch(1, RB_EV_LOAD);
    EXPECT("order.join()", "s1,s2,dcl,load");
    EXPECT("document.getElementById('w').previousElementSibling.tagName", "SCRIPT");
    EXPECT("document.getElementById('b').click(); document.getElementById('b').click();"
           "document.getElementById('o').textContent", "2");
    if (!strstr(g_console, "Uncaught Error: boom")) { printf("FAIL: uncaught error not reported: %s\n", g_console); g_fail++; }
    else g_pass++;
    {
        int b = rb_find_id("b");
        rb_dispatch(b, RB_EV_CLICK);
        EXPECT("document.getElementById('o').textContent", "3");
    }

    /* inline handlers in markup added after the page loaded: from an event
       handler (innerHTML), a timer (insertAdjacentHTML), and setAttribute */
    page("<body><div id=d></div><button id=go onclick=\"render()\">go</button><p id=sa>p</p><script>var hits = 0;"
         "function render() { document.getElementById('d').innerHTML = '<button id=late onclick=\"hits++\">late</button>'; }"
         "setTimeout(() => { document.body.insertAdjacentHTML('beforeend', '<button id=tm onclick=\"hits += 10\">t</button>');"
         "  document.getElementById('sa').setAttribute('onclick', 'hits += 100'); }, 10);</script></body>");
    rb_dispatch(rb_find_id("go"), RB_EV_CLICK);
    rb_dispatch(rb_find_id("late"), RB_EV_CLICK);
    EXPECT("hits", "1");
    tick(20);
    rb_dispatch(rb_find_id("tm"), RB_EV_CLICK);
    rb_dispatch(rb_find_id("sa"), RB_EV_CLICK);
    rb_dispatch(rb_find_id("late"), RB_EV_CLICK);       /* bound once, not once per rebind */
    EXPECT("hits", "112");

    /* scripts inserted by scripts */
    EXPECT("var ev = []; var sc = document.createElement('script'); sc.src = 'http://cdn.example.net/lib.js';"
           "sc.onload = () => ev.push('load'); document.head.appendChild(sc);"
           "var bad = document.createElement('script'); bad.src = '/missing.js'; bad.onerror = () => ev.push('error');"
           "document.body.appendChild(bad); var inl = document.createElement('script'); inl.textContent = 'window.inlineRan = 1';"
           "document.body.appendChild(inl); window.inlineRan + ',' + ev.length", "1,0");
    tick(1);
    EXPECT("libLoaded + ',' + ev.sort().join()", "1,error,load");
    EXPECT("document.body.appendChild(sc); document.body.innerHTML += '<script>window.never = 1</script>'; typeof never", "undefined");
    tick(1);
    EXPECT("libLoaded", "1");

    /* old children survive textContent/innerHTML replacement (jQuery relies on it) */
    EXPECT("var hold = document.createElement('div'); hold.innerHTML = '<b>1</b><i>2</i>'; var kids = [...hold.childNodes];"
           "hold.textContent = ''; var dst = document.createElement('div'); kids.forEach(k => dst.appendChild(k)); dst.innerHTML", "<b>1</b><i>2</i>");
    EXPECT("var k2 = dst.firstChild; dst.innerHTML = 'x'; hold.appendChild(k2); hold.innerHTML", "<b>1</b>");

    /* ...while content no script ever looked at is recycled: a page that
       rewrites textContent or innerHTML in a timer must not run out of
       nodes (16384) however long it runs */
    {
        int before = rb_debug_stat(0);
        EXPECT("var live = document.createElement('div'); document.body.appendChild(live); var bad = -1;"
               "for (var i = 0; i < 40000 && bad < 0; i++) { live.textContent = 'tick ' + i; if (live.textContent !== 'tick ' + i) bad = i; }"
               "for (var i = 0; i < 20000 && bad < 0; i++) { live.innerHTML = '<span>n <b>' + i + '</b></span>';"
               "  if (live.innerHTML !== '<span>n <b>' + i + '</b></span>') bad = i; } bad", "-1");
        if (rb_debug_stat(0) - before > 64) { printf("FAIL: %d nodes leaked by content updates\n", rb_debug_stat(0) - before); g_fail++; }
        else g_pass++;
    }
    /* a held node deep inside keeps its whole branch; its unseen siblings go */
    EXPECT("live.innerHTML = '<p><em id=deep>k</em></p><i>gone</i>'; var deep = live.querySelector('#deep'); live.textContent = 'x';"
           "[deep.textContent, deep.parentNode.tagName, deep.isConnected, live.innerHTML].join()", "k,P,false,x");
    EXPECT("live.innerHTML = '<b>1</b><i>2</i><u>3</u>'; var keepI = live.children[1], nu = document.createElement('s');"
           "live.replaceChildren(nu, keepI); live.innerHTML", "<s></s><i>2</i>");
    EXPECT("live.replaceChildren(); live.childNodes.length", "0");

    /* select.value against an option whose value is longer than the
       engine's compare buffer (it used to memcmp past the buffer) */
    EXPECT("live.innerHTML = '<select id=sl><option>short</option><option id=lo>x</option></select>';"
           "var sl = document.getElementById('sl'), longv = 'v'.repeat(3000); document.getElementById('lo').value = longv;"
           "sl.value = longv; sl.value = 'short'; sl.value", "short");

    /* detached documents never touch the page */
    EXPECT("var before = document.body.children.length; var d2 = document.implementation.createHTMLDocument('');"
           "d2.body.innerHTML = '<form></form><form></form>'; d2.body.childNodes.length + ',' + (document.body.children.length === before)", "2,true");
    EXPECT("new DOMParser().parseFromString('<p id=q>hi</p>', 'text/html').getElementById('q').textContent", "hi");

    /* JSON data islands */
    page("<body><script type=application/json id=cfg>{\"k\": [1, 2]}</script></body>");
    EXPECT("JSON.parse(document.getElementById('cfg').textContent).k[1]", "2");

    /* geometry after a script relayout */
    page("<body><div id=g style='width:100px'>box</div></body>");
    EXPECT("document.getElementById('g').getBoundingClientRect().width", "100");
    EXPECT("var g = document.getElementById('g'); g.style.width = '50px'; g.offsetWidth", "50");

    /* images: the host (rbrowse.c's loader) stores each outcome with
       rb_dom_img_set and fires load/error through rjs_dom_image_event; here
       the test plays the host */
    rb_set_images(1);
    rb_set_viewport(622, 332);
    page("<body style='margin:0'><img id=im src=a.png onload=\"window.ev = (window.ev || '') + 'L'\">"
         "<img id=bad src=b.png onerror=\"this.alt = 'gone'\"><img id=fx src=c.png width=30 height=20>"
         "<a href=/x><img id=ln src=d.png width=40 height=10></a></body>");
    EXPECT("var im = document.getElementById('im'); [im instanceof HTMLImageElement, im instanceof Image, im.complete, im.naturalWidth, im.width].join()",
           "true,true,false,0,0");
    EXPECT("var fx = document.getElementById('fx'); [fx.width, fx.height, fx.complete].join()", "30,20,false");
    EXPECT("window.order = []; im.addEventListener('load', e => order.push(e.type + ':' + e.bubbles + ':' + (e.target === im)));"
           "document.addEventListener('load', () => order.push('doc'), true);"
           "window.addEventListener('load', () => order.push('win'), true); var dec = 'wait'; im.decode().then(() => dec = 'ok'); 1", "1");
    {
        int im = rb_find_id("im"), bad = rb_find_id("bad"), ln = rb_find_id("ln");
        rb_dom_img_set(im, RB_IMG_OK, 0, 64, 48);
        rjs_dom_image_event(im, 1);
        rb_dom_img_set(bad, RB_IMG_BROKEN, 0, 0, 0);
        rjs_dom_image_event(bad, 0);
        rb_dom_img_set(ln, RB_IMG_OK, 1, 80, 20);
        rjs_dom_image_event(ln, 1);
        rb_relayout();
    }
    /* the window does not see an element's load; the document's capture
       does (once for each of the two images that loaded) */
    EXPECT("window.ev + ',' + order.join(' ') + ',' + dec", "L,doc load:false:true doc,ok");
    EXPECT("[im.complete, im.naturalWidth, im.naturalHeight, im.width, im.height].join()", "true,64,48,64,48");
    EXPECT("var r = im.getBoundingClientRect(); r.width + 'x' + r.height", "64x48");
    EXPECT("var bad = document.getElementById('bad'); [bad.complete, bad.naturalWidth, bad.alt].join()", "true,0,gone");
    EXPECT("var r2; bad.decode().catch(e => r2 = e.name); 1", "1");
    EXPECT("r2", "EncodingError");
    EXPECT("document.getElementById('ln').width", "40");                 /* inside a link */
    /* new Image(): a detached <img> the host still finds and loads */
    EXPECT("var ni = new Image(5, 6); ni.src = 'n.png'; window.nl = 0; ni.onload = () => nl++;"
           "[ni.tagName, ni.width, ni.height, ni.complete, ni.isConnected, ni.currentSrc].join()",
           "IMG,5,6,false,false,http://example.com/dir/n.png");
    {
        int found = 0;
        for (int n = rb_dom_img_next(0); n; n = rb_dom_img_next(n)) {
            char src[32];
            if (rb_dom_get_attr(n, "src", 3, src, (int)sizeof(src)) && strcmp(src, "n.png") == 0 &&
                rb_dom_img_state(n, 0, 0, 0) == RB_IMG_NONE) {
                found = 1;
                rb_dom_img_set(n, RB_IMG_OK, 2, 7, 9);
                rjs_dom_image_event(n, 1);
            }
        }
        if (!found) { printf("FAIL: new Image() not visible to the host\n"); g_fail++; } else g_pass++;
    }
    EXPECT("[nl, ni.complete, ni.naturalWidth, ni.naturalHeight].join()", "1,true,7,9");
    EXPECT("ni.src = 'm.png'; ni.complete + ',' + ni.naturalWidth", "false,0");   /* a new src starts over */
    EXPECT("var e2 = document.createElement('img'); e2.complete + ',' + e2.naturalWidth", "true,0");   /* no src */
    rb_set_images(0);
    rb_set_viewport(RB_VIEW_W_DEFAULT, RB_VIEW_H_DEFAULT);

    /* runaway script is interrupted, page survives */
    if (strncmp(js("while (true) {}"), "!InternalError: interrupted", 27) != 0) {
        printf("FAIL: runaway loop not interrupted: %s\n", g_out);
        g_fail++;
    } else g_pass++;
    EXPECT("1 + 1", "2");
    /* ...also after a native re-entered JS: el.click() runs the engine's
       dispatch and its JS listener inside the script. Leaving that inner
       entry used to disarm the outer script's time budget for good. */
    page("<body><button id=b>x</button><script>var b = document.getElementById('b'), n = 0;"
         "b.addEventListener('click', () => n++);</script></body>");
    if (strncmp(js("b.click(); while (true) {}"), "!InternalError: interrupted", 27) != 0) {
        printf("FAIL: runaway loop after a nested dispatch not interrupted: %s\n", g_out);
        g_fail++;
    } else g_pass++;
    EXPECT("n", "1");

    printf("%d passed, %d failed\n", g_pass, g_fail);
    if (!g_fail) printf("rbrowse js checks passed\n");
    return g_fail ? 1 : 0;
}
