/* rjs_dom.h - the browser's DOM for QuickJS (rjs_dom.c + dom_prelude.js).
 *
 * The engine (src/rbrowse/engine.h) owns the document; JS objects are thin
 * wrappers around node handles. Natives live on a hidden __rb object, the
 * DOM classes (Node, Element, Document, Event, timers, location...) are
 * written in JS in dom_prelude.js on top of them.
 *
 * Host flow for one page: rjs_start(); rb_open_web(...); rjs_dom_install(url);
 * for each <script> in order: rjs_dom_exec(...); rjs_dom_loaded(); then every
 * main-loop turn rjs_dom_tick() and, when rjs_dom_take_dirty() says so,
 * rb_relayout() and repaint.
 */
#ifndef RJS_DOM_H
#define RJS_DOM_H

/* Fetches url (absolute, http/https) into out; returns the length or -1.
   same_origin is 1 when the page reads the reply (fetch/XHR): then every
   redirect must stay on url's origin, or the fetch fails. */
typedef int (*RjsFetch)(const char *url, char *out, int cap, int same_origin);

/* What the host adds beyond GET; any member may be 0, and the whole thing
   is optional (host tests run without it). Kept across pages.
   cookie_get: document.cookie for url (script-visible cookies only) into
     out, NUL-terminated; returns the length, or -1 when url has no jar (a
     local file: the page then keeps a per-page jar of its own).
   cookie_set: document.cookie = text for url; returns 1 stored, 0 refused
     (HttpOnly, foreign domain...), -1 no jar for url.
   post: POST len bytes of body with Content-Type ctype to url, redirects
     kept on url's origin like RjsFetch with same_origin = 1; the reply into
     out; returns its length or -1. */
typedef struct {
    int (*cookie_get)(const char *url, char *out, int cap);
    int (*cookie_set)(const char *url, const char *text);
    int (*post)(const char *url, const char *body, int len, const char *ctype, char *out, int cap);
} RjsNet;

/* Set (or with 0, remove) the host's RjsNet; the struct is copied. */
void rjs_dom_set_net(const RjsNet *net);

/* Install natives + prelude on the running context for the page at url.
   0 on success. */
int rjs_dom_install(const char *url, RjsFetch fetch);

/* Run one script element's code (node = the <script>, for
   document.currentScript and document.write). Exceptions are reported to
   the output sink; returns 0 ok, -1 exception. */
int rjs_dom_exec(int node, const char *code, int len, const char *name);

/* DOMContentLoaded on document; the host fires the native load itself. */
void rjs_dom_loaded(void);

/* An <img> finished: fire load (ok) or error at it (no bubbling), after the
   host stored the outcome with rb_dom_img_set. */
void rjs_dom_image_event(int node, int ok);

/* Run due timers and promise jobs. Returns 1 if any JS ran. */
int rjs_dom_tick(void);

/* 1 if scripts changed the DOM since the last call (relayout needed). */
int rjs_dom_take_dirty(void);

/* Milliseconds until the next timer is due, or -1 if none. */
int rjs_dom_next_due(void);

/* What the host's last repaint after script activity cost, in ms:
   requestAnimationFrame spaces frames by it, so a page whose frames are
   expensive to draw gets fewer of them instead of starving input. */
void rjs_dom_set_paint_cost(int ms);

#endif
