/* Host tool: lay a self-contained HTML file out with the web profile and print
   one line per element in document order, so the result can be diffed against a
   reference browser (tools/rbrowse/refdiff/).

     clang -g -O1 -DRBROWSE_HOST -I src/rbrowse tools/rbrowse/dump_layout.c \
         src/rbrowse/engine.c -o /tmp/rb_dump
     /tmp/rb_dump page.html > ours.tsv

   Columns: index, tag enum, x, y, w, h (border box, or -1 when the element has
   none: inline, display:none, ...), tx, ty (left/top of the first text box
   inside it, or -1). Pseudo-elements and text nodes are not listed. */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ty_best[16384], tx_best[16384];

static void walk(int node, int *idx) {
    for (int c = rb_dom_first(node); c; c = rb_dom_next(c)) {
        if (rb_dom_is_text(c)) continue;
        int x, y, w, h;
        int have = rb_debug_box(c, &x, &y, &w, &h);
        if (!have) x = y = w = h = -1;
        int tx = tx_best[c] == 0x7fffffff ? -1 : tx_best[c];
        int ty = ty_best[c] == 0x7fffffff ? -1 : ty_best[c];
        printf("%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", (*idx)++, rb_node_tag(c), x, y, w, h, tx, ty);
        walk(c, idx);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: rb_dump page.html\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    static char buf[1 << 24];
    int n = (int)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    rb_open_web(buf, n);
    for (int i = 0; i < 16384; i++) { ty_best[i] = tx_best[i] = 0x7fffffff; }
    /* first text of each element: lexicographically smallest (y, x) among the
       text boxes of its subtree */
    for (int i = 0; i < rb_box_count(); i++) {
        const RbBox *b = rb_box(i);
        if (b->kind != RB_BOX_TEXT) continue;
        int len = 0;
        const char *t = rb_box_text(i, &len);
        int blank = 1;
        for (int k = 0; k < len; k++) if (t[k] != ' ') { blank = 0; break; }
        if (blank) continue;
        for (int nd = b->node; nd > 0 && nd < 16384; nd = rb_dom_parent(nd)) {
            if (b->y < ty_best[nd] || (b->y == ty_best[nd] && b->x < tx_best[nd])) { ty_best[nd] = b->y; tx_best[nd] = b->x; }
        }
    }
    int idx = 0;
    walk(1, &idx);
    return 0;
}
