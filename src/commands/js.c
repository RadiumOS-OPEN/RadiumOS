// js.c - run JavaScript (QuickJS) from the shell
//
//   js <file.js>      run a script from AVFS
//   js -e <code...>   evaluate the rest of the line and print the result
//   js --test         engine self-test
//   js --mem          JS heap usage
//
// Like rbrowse, this runs inside the timer interrupt with IRQs off; it turns
// them on (holding the scheduler) so script time budgets and Date advance,
// and runs under the fault guard so an engine crash returns to the shell.

#include "../terminal/terminal.h"
#include "../Avfs/Avfs.h"
#include "../utility/utility.h"
#include "../scheduler/task.h"
#include "../kernel/fault_guard.h"
#include "../quickjs/rjs.h"

#define JS_FILE_MAX (512 * 1024)

static char g_js_src[JS_FILE_MAX + 1];
static char g_js_out[2048];
static FgJmp g_js_guard;

static void js_print_num(unsigned v) {
    char buf[12];
    int n = 0;
    char rev[12];
    do {
        rev[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    for (int i = 0; i < n; i++) buf[i] = rev[n - 1 - i];
    buf[n] = 0;
    print(buf);
}

static int js_streq(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

/* Expression, expected String(result). An expected value starting with '!'
   means "throws, and the message contains the rest". */
static const char *const js_tests[][2] = {
    {"1+2", "3"},
    {"0.1+0.2", "0.30000000000000004"},
    {"(1/3).toFixed(5)", "0.33333"},
    {"(123.456).toPrecision(4)", "123.5"},
    {"String(1e21) + ' ' + (0.000001234)", "1e+21 0.000001234"},
    {"(25).toString(2) + ' ' + Number('0x1f')", "11001 31"},
    {"parseFloat('3.25e2')", "325"},
    {"Math.sqrt(2)", "1.4142135623730951"},
    {"Math.pow(2, 0.5)", "1.4142135623730951"},
    {"Math.exp(1)", "2.718281828459045"},
    {"Math.log(10)", "2.302585092994046"},
    {"Math.atan2(1, 1) * 4", "3.141592653589793"},
    {"Math.sin(Math.PI / 6).toFixed(12)", "0.500000000000"},
    {"Math.cos(0) + Math.tan(0)", "1"},
    {"Math.cbrt(27) + ' ' + Math.hypot(3, 4)", "3 5"},
    {"Math.round(2.5) + ',' + Math.round(-2.5) + ',' + Math.trunc(-1.7)", "3,-2,-1"},
    {"Math.fround(5.5) + Math.sign(-3)", "4.5"},
    {"1/0 + ' ' + (-0 === 0) + ' ' + Object.is(-0, 0)", "Infinity true false"},
    {"Number.MAX_SAFE_INTEGER", "9007199254740991"},
    {"(2n ** 64n).toString()", "18446744073709551616"},
    {"[3, 1, 2].sort().join()", "1,2,3"},
    {"Array.from({length: 4}, (_, i) => i * i).join()", "0,1,4,9"},
    {"Math.max(...[1, 5, 3])", "5"},
    {"JSON.stringify({a: [1, 'x', null], b: true})", "{\"a\":[1,\"x\",null],\"b\":true}"},
    {"JSON.parse('{\"k\":[1,2,{\"z\":3}]}').k[2].z", "3"},
    {"'abc'.replace(/b/g, 'B') + 'x'.padStart(3, '-')", "aBc--x"},
    {"/(\\d+)-(\\d+)/.exec('10-20')[2]", "20"},
    {"'Grüße'.toUpperCase() + [...'héllo'].length", "GRÜSSE5"},
    {"`${2 ** 10}`", "1024"},
    {"class A { #x = 4; get x() { return this.#x } }; new A().x", "4"},
    {"(() => { let s = 0; for (let i = 0; i < 100000; i++) s += i; return s })()", "4999950000"},
    {"new Map([[1, 2]]).get(1) + new Set([1, 1, 2]).size", "4"},
    {"Object.entries({x: 1, y: 2}).toString()", "x,1,y,2"},
    {"let {a, ...rest} = {a: 1, b: 2, c: 3}; Object.keys(rest).join()", "b,c"},
    {"function* g() { yield 1; yield 2 } [...g()].join()", "1,2"},
    {"new Date(0).toISOString()", "1970-01-01T00:00:00.000Z"},
    {"Date.now() > 1600000000000", "true"},
    {"eval('2 * 21')", "42"},
    {"try { null.x } catch (e) { e instanceof TypeError }", "true"},
    {"var r = 0; (async () => { r = await 7 })(); 0", "0"},
    {"r", "7"},
    {"Promise.resolve(1).then(v => globalThis.pr = v + 1); 0", "0"},
    {"pr", "2"},
    {"function f() { return f() } f()", "!stack overflow"},
    {"while (true) {}", "!interrupted"},
    {"new Array(1e6).fill(1.5).reduce((a, b) => a + b)", "1500000"},
};

static int js_run_tests(void) {
    int n = (int)(sizeof(js_tests) / sizeof(js_tests[0]));
    int pass = 0;
    for (int i = 0; i < n; i++) {
        const char *src = js_tests[i][0];
        const char *want = js_tests[i][1];
        int throws = want[0] == '!';
        int rc = rjs_eval(src, -1, "<test>", throws ? 1000 : 5000, g_js_out, (int)sizeof(g_js_out));
        int ok;
        if (throws) {
            ok = rc == RJS_EXCEPTION && strstr(g_js_out, want + 1) != 0;
        } else {
            ok = rc == RJS_OK && js_streq(g_js_out, want);
        }
        if (ok) {
            pass++;
        } else {
            print("FAIL ");
            print(src);
            print("\n  got: ");
            /* first line only */
            for (int k = 0; g_js_out[k]; k++) if (g_js_out[k] == '\n') { g_js_out[k] = 0; break; }
            print(g_js_out);
            print("\n  want: ");
            print(want);
            print("\n");
        }
    }
    js_print_num((unsigned)pass);
    print("/");
    js_print_num((unsigned)n);
    print(" JS checks passed\n");
    return pass == n;
}

static void js_mem(void) {
    unsigned used, peak, total;
    rjs_memory(&used, &peak, &total);
    print("JS heap: ");
    js_print_num(used / 1024);
    print(" KB in use, peak ");
    js_print_num(peak / 1024);
    print(" KB, arena ");
    js_print_num(total / 1024);
    print(" KB\n");
}

static int js_load_file(const char *arg) {
    char path[256];
    int w = 0;
    if (arg[0] != '/') {
        const char *cwd = avfs_getcwd();
        if (!cwd || !cwd[0]) cwd = "/";
        while (cwd[w] && w < 200) { path[w] = cwd[w]; w++; }
        if (w == 0 || path[w - 1] != '/') path[w++] = '/';
    }
    for (int i = 0; arg[i] && w < 255; i++) path[w++] = arg[i];
    path[w] = 0;
    int size = avfs_get_filesize(path);
    if (size < 0) {
        print("js: not found: ");
        print(path);
        print("\n");
        return -1;
    }
    if (size > JS_FILE_MAX) {
        print("js: file too large\n");
        return -1;
    }
    if (avfs_read_file(path, g_js_src, (uint32_t)size, 0) != 0) {
        print("js: read error\n");
        return -1;
    }
    g_js_src[size] = 0;
    return size;
}

static void js_body(int argc, char *argv[]) {
    if (argc < 2 || js_streq(argv[1], "--help") || js_streq(argv[1], "-h")) {
        print("js <file.js> | js -e <code> | js --test | js --mem\n");
        print("  QuickJS JavaScript engine (ES2023).\n");
        return;
    }
    if (rjs_start() != RJS_OK) {
        print("js: out of memory starting the engine\n");
        return;
    }
    if (js_streq(argv[1], "--test")) {
        js_run_tests();
        js_mem();
    } else if (js_streq(argv[1], "--mem")) {
        js_mem();
    } else if (js_streq(argv[1], "-e")) {
        int w = 0;
        for (int i = 2; i < argc; i++) {
            for (int k = 0; argv[i][k] && w < JS_FILE_MAX - 1; k++) g_js_src[w++] = argv[i][k];
            if (i + 1 < argc && w < JS_FILE_MAX - 1) g_js_src[w++] = ' ';
        }
        g_js_src[w] = 0;
        int rc = rjs_eval(g_js_src, w, "<cmdline>", 10000, g_js_out, (int)sizeof(g_js_out));
        if (rc == RJS_EXCEPTION) print("Uncaught ");
        print(g_js_out);
        print("\n");
    } else {
        int n = js_load_file(argv[1]);
        if (n >= 0) rjs_eval(g_js_src, n, argv[1], 30000, 0, 0);
    }
    rjs_stop();
}

void js_command(int argc, char *argv[]) {
    uint32_t saved_flags;
    __asm__ volatile("pushf; pop %0" : "=r"(saved_flags));
    sched_hold_push();
    __asm__ volatile("sti");
    if (fg_setjmp(&g_js_guard)) {
        char why[120];
        fg_describe(why, (int)sizeof(why));
        rjs_abandon();
        print("js: engine crashed, state discarded. ");
        print(why);
        print("\n");
    } else {
        fg_arm(&g_js_guard, 0);
        js_body(argc, argv);
    }
    fg_disarm();
    if (!(saved_flags & 0x200)) __asm__ volatile("cli");
    sched_hold_pop();
}
