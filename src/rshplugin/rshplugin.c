// =============================================================================
// RSH Plugin Loader  --  rsh_plugin.c
//
// How it works:
//
//   LOAD TIME (rsh_plugin_load)
//   ─────────────────────────────────────────────────────────────────────────
//   1.  Allocate a slot in g_plugins[].
//   2.  Call script_init_ctx() to zero-initialise a fresh ScriptCtx for this
//       plugin.  Every plugin gets its own isolated variable/function space.
//   3.  Call exec_file_ctx(path, ctx) — this is the normal first-pass that
//       collects function definitions and const MAP blocks.  It does NOT call
//       main() because ^entrypoint is absent and the script has no top-level
//       executable statements.
//   4.  Call call_function_ctx("information", 0 args, ctx).  The "information"
//       function populates the RadiTest map.  We then read RadiTest["Name"]
//       to obtain the shell command name.
//   5.  Call register_command(name, description, rsh_dispatch_shim) from
//       keyboard.c.  The shim is the same for every plugin; it uses argv[0]
//       to look up which plugin to invoke.
//
//   DISPATCH TIME (rsh_dispatch_shim)
//   ─────────────────────────────────────────────────────────────────────────
//   1.  argv[0] is the command name typed by the user.
//   2.  Look up the matching RshPlugin slot.
//   3.  Inject args: set RSH var "ARG_COUNT" = argc-1, then $1..$N for each
//       extra argv.  (RSH treats $1 as the first user-supplied argument,
//       exactly matching the shell convention in the example script.)
//   4.  Call call_function_ctx("command", 0, ctx).  The "command" function
//       can read $1..$N freely; they were set in step 3.
//   5.  Clean up injected arg vars so the context stays tidy between calls.
//
// =============================================================================

#include "rshplugin.h"

// ── RSH engine C interface (defined in rsh_engine.rs / rsh.h) ───────────────
// These are the symbols exported by the Rust script engine via #[no_mangle].
extern void script_reset_flags_ctx(void *ctx);
extern void  script_init_ctx    (void *ctx);
extern int   exec_file_ctx      (const char *path, void *ctx);
extern int   call_function_ctx  (const char *name, const char **args, int argc, void *ctx);
extern void  script_set_var_ctx (const char *name, const char *val,  void *ctx);
extern int   script_get_var_ctx (const char *name, char *out, int out_len, void *ctx);
extern int script_map_get_ctx(const char *map_name, const char *key,
                               char *out, int out_size, const void *ctx);
// If your build links the single-context versions instead, define the wrapper
// shims at the bottom of this file and flip this flag.
#define RSH_PLUGIN_MULTI_CTX 1

// ── keyboard.c interface ─────────────────────────────────────────────────────
// register_command() comes from keyboard.h.  We need the full signature here.
extern int register_command(const char *name, const char *description,
                            void (*execute)(int, char *[]));

// ── Terminal helpers ──────────────────────────────────────────────────────────
// Provided by terminal.c / utility.c in RadiumOS.
extern void print  (const char *s);
extern void printr (const char *s);

// ── Local helpers ─────────────────────────────────────────────────────────────
static void  _print_int   (int n);
static char *_itoa_local  (int n, char *buf, int bufsz);

// =============================================================================
// SCRIPT CONTEXT POOL
//
// Each plugin needs its own ScriptCtx (~178 KB) so their variables and
// function tables don't collide.  We allocate them statically here.
//
// ScriptCtx is defined in the Rust crate; we only need its SIZE in C.
// The size is stable (it's all fixed arrays, no heap).  If you change
// the Rust constants you must update RSH_CTX_SIZE_BYTES below.
//
// Size breakdown (from the Rust source comment at the top of rsh_engine.rs):
//   FUNC_BODY_POOL  [Line; 3072]      = 786,432 bytes  (external static, not in ctx)
//   GLOBAL_CTX      ScriptCtx         = ~178,000 bytes  (our target)
//   LINES_D0..D3    [Line; 1000] × 4  = 1,024,000 bytes (external, not in ctx)
//   FBUF_D0..D3     [u8; 32768] × 4   = 131,072 bytes   (external, not in ctx)
//
// ScriptCtx alone is ~178 KB.  16 plugins × 178 KB = ~2.8 MB total.
// That is tight on a 4 MB kernel heap; trim RSH_MAX_PLUGINS if needed.
// =============================================================================
#define RSH_CTX_SIZE_BYTES  234496
// Statically allocate a pool of script contexts.
// __attribute__((aligned(64))) ensures the Rust-side atomics are happy.
static unsigned char g_ctx_pool[RSH_MAX_PLUGINS][RSH_CTX_SIZE_BYTES]
    __attribute__((aligned(64)));

// =============================================================================
// PLUGIN TABLE
// =============================================================================

static RshPlugin g_plugins[RSH_MAX_PLUGINS];
static int       g_plugin_count = 0;

// Returns the context buffer for slot i.
static void *ctx_for(int i) {
    return (void *)g_ctx_pool[i];
}

// Find a plugin slot by command name.  Returns -1 if not found.
static int find_plugin(const char *cmd_name) {
    for (int i = 0; i < g_plugin_count; i++) {
        if (!g_plugins[i].loaded) continue;
        // simple strcmp-equivalent (no libc guarantee in no_std kernel)
        const char *a = g_plugins[i].cmd_name;
        const char *b = cmd_name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return i;
    }
    return -1;
}

// =============================================================================
// DISPATCH SHIM
//
// This single C function is registered with keyboard.c for EVERY plugin.
// It uses argv[0] (the command name the user typed) to route to the right
// plugin context, then calls command().
// =============================================================================

static void rsh_dispatch_shim(int argc, char *argv[]) {
    if (argc < 1 || !argv || !argv[0]) return;

    int slot = find_plugin(argv[0]);
    if (slot < 0) {
        print("[rsh] no plugin for command: ");
        print(argv[0]);
        print("\n");
        return;
    }

    void *ctx = ctx_for(slot);

    char count_buf[8];
    int  user_argc = argc - 1;
    _itoa_local(user_argc, count_buf, sizeof(count_buf));
    script_set_var_ctx("ARG_COUNT", count_buf, ctx);

    char idx_buf[8];
    for (int i = 1; i < argc && i <= RSH_MAX_ARGS; i++) {
        _itoa_local(i, idx_buf, sizeof(idx_buf));
        script_set_var_ctx(idx_buf, argv[i] ? argv[i] : "", ctx);
    }

    script_reset_flags_ctx(ctx);   // ← ADD THIS
    call_function_ctx("command", (const char **)0, 0, ctx);

    script_set_var_ctx("ARG_COUNT", "0", ctx);
    for (int i = 1; i <= RSH_MAX_ARGS; i++) {
        _itoa_local(i, idx_buf, sizeof(idx_buf));
        script_set_var_ctx(idx_buf, "", ctx);
    }
}

// =============================================================================
// rsh_plugin_load
// =============================================================================


RshPlugin *rsh_plugin_load(const char *path) {
    if (!path || !path[0]) {
        print("[rsh] plugin load: null or empty path\n");
        return (void *)0;
    }

    // Find a free slot.
    int slot = -1;
    for (int i = 0; i < RSH_MAX_PLUGINS; i++) {
        if (!g_plugins[i].loaded) { slot = i; break; }
    }
    if (slot < 0) {
        print("[rsh] plugin table full (max ");
        _print_int(RSH_MAX_PLUGINS);
        print(")\n");
        return (void *)0;
    }

    void *ctx = ctx_for(slot);

    // ── 1. Initialise a fresh ScriptCtx ──────────────────────────────────────
    script_init_ctx(ctx);

    // ── 2. Execute the file (collect functions) ───────────────────────────────
    int rc = exec_file_ctx(path, ctx);
    if (rc != 0) {
        print("[rsh] exec_file failed for: ");
        print(path);
        print("\n");
        return (void *)0;
    }

    // ── 3. Call information() to set metadata variables ──────────────────────
    script_reset_flags_ctx(ctx);
    rc = call_function_ctx("information", (const char **)0, 0, ctx);
    if (rc != 0) {
        print("[rsh] 'information' function failed or missing in: ");
        print(path);
        print("\n");
        return (void *)0;
    }

    // ── 4. Read metadata from VARIABLES ──────────────────────────────────────
    char name_var[RSH_CMD_NAME_MAX] = { 0 };
    char ver_var[32]                = { 0 };
    char author_var[64]             = { 0 };
    char desc_var[128]              = { 0 };

    script_get_var_ctx("PLUGIN_NAME",   name_var,   sizeof(name_var),   ctx);
    script_get_var_ctx("PLUGIN_VER",    ver_var,    sizeof(ver_var),    ctx);
    script_get_var_ctx("PLUGIN_AUTHOR", author_var, sizeof(author_var), ctx);
    script_get_var_ctx("PLUGIN_DESC",   desc_var,   sizeof(desc_var),   ctx);

    if (name_var[0] == '\0') {
        print("[rsh] plugin failed to define PLUGIN_NAME in: ");
        print(path);
        print("\n");
        return (void *)0;
    }

    // ── 5. Check for duplicate registration ───────────────────────────────────
    if (find_plugin(name_var) >= 0) {
        print("[rsh] command already registered: ");
        print(name_var);
        print(" (from ");
        print(path);
        print(")\n");
        return (void *)0;
    }

    // ── 6. Build description string ───────────────────────────────────────────
    static char s_desc[RSH_MAX_PLUGINS][128];
    char *desc = s_desc[slot];
    if (desc_var[0] != '\0') {
        // Use the plugin-provided description directly
        char *d = desc;
        const char *s = desc_var;
        while (*s && d < s_desc[slot] + 127) *d++ = *s++;
        *d = '\0';
    } else {
        // Fallback: synthesize from version + author
        char *d = desc;
        const char *prefix = "RSH plugin v";
        while (*prefix) *d++ = *prefix++;
        const char *v = ver_var[0] ? ver_var : "?";
        while (*v)      *d++ = *v++;
        const char *mid = " by ";
        while (*mid)    *d++ = *mid++;
        const char *a = author_var[0] ? author_var : "?";
        while (*a)      *d++ = *a++;
        *d = '\0';
    }

    // ── 7. Fill in the plugin table slot FIRST (so name ptr is persistent) ───
    RshPlugin *p = &g_plugins[slot];
    {
        int i = 0;
        while (name_var[i] && i < RSH_CMD_NAME_MAX - 1) { p->cmd_name[i] = name_var[i]; i++; }
        p->cmd_name[i] = '\0';

        i = 0;
        while (ver_var[i] && i < 31) { p->version[i] = ver_var[i]; i++; }
        p->version[i] = '\0';

        i = 0;
        while (author_var[i] && i < 63) { p->author[i] = author_var[i]; i++; }
        p->author[i] = '\0';

        i = 0;
        while (path[i] && i < 255) { p->path[i] = path[i]; i++; }
        p->path[i] = '\0';
    }
    p->loaded = true;
    if (slot >= g_plugin_count) g_plugin_count = slot + 1;

    // ── 8. Register with keyboard.c using persistent p->cmd_name pointer ─────
    int ok = register_command(p->cmd_name, desc, rsh_dispatch_shim);
    if (!ok) {
        print("[rsh] register_command failed for: ");
        print(p->cmd_name);
        print("\n");
        p->loaded = false;  // roll back so the slot is reusable
        return (void *)0;
    }

    print("[rsh] loaded plugin '");
    print(p->cmd_name);
    print("' from ");
    print(path);
    print("\n");
    return p;
}


// =============================================================================
// rsh_plugin_unload
// =============================================================================

void rsh_plugin_unload(const char *cmd_name) {
    int slot = find_plugin(cmd_name);
    if (slot < 0) {
        print("[rsh] unload: not found: ");
        print(cmd_name);
        print("\n");
        return;
    }
    // Zero-wipe the context so stale state can't leak if the slot is reused.
    unsigned char *ctx = (unsigned char *)ctx_for(slot);
    for (int i = 0; i < RSH_CTX_SIZE_BYTES; i++) ctx[i] = 0;

    // Clear the slot metadata.
    g_plugins[slot].loaded      = false;
    g_plugins[slot].cmd_name[0] = '\0';
    g_plugins[slot].path[0]     = '\0';

    // NOTE: keyboard.c has no unregister_command() in the current design.
    // The entry stays in the command table but its context is zeroed, so
    // calling it after unload will silently do nothing (find_plugin returns -1
    // and rsh_dispatch_shim prints an error message).
    // If you later add unregister_command(), call it here.

    print("[rsh] unloaded plugin: ");
    print(cmd_name);
    print("\n");
}

// =============================================================================
// rsh_plugin_reload
// =============================================================================

RshPlugin *rsh_plugin_reload(const char *cmd_name) {
    int slot = find_plugin(cmd_name);
    if (slot < 0) {
        print("[rsh] reload: not found: ");
        print(cmd_name);
        print("\n");
        return (void *)0;
    }

    // Copy the path before unloading (unload clears it).
    char saved_path[256];
    {
        int i = 0;
        while (g_plugins[slot].path[i] && i < 255) { saved_path[i] = g_plugins[slot].path[i]; i++; }
        saved_path[i] = '\0';
    }

    rsh_plugin_unload(cmd_name);
    return rsh_plugin_load(saved_path);
}

// =============================================================================
// rsh_plugin_list
// =============================================================================

void rsh_plugin_list(void) {
    print("=== RSH plugins ===\n");
    int any = 0;
    for (int i = 0; i < g_plugin_count; i++) {
        if (!g_plugins[i].loaded) continue;
        print("  ");
        print(g_plugins[i].cmd_name);
        print("  v");
        print(g_plugins[i].version);
        print("  by ");
        print(g_plugins[i].author);
        print("  [");
        print(g_plugins[i].path);
        print("]\n");
        any = 1;
    }
    if (!any) print("  (none loaded)\n");
}

// =============================================================================
// UTILITY
// =============================================================================

static char *_itoa_local(int n, char *buf, int bufsz) {
    if (bufsz < 2) return buf;
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return buf; }
    int neg = (n < 0);
    if (neg) n = -n;
    int i = bufsz - 1;
    buf[i--] = '\0';
    while (n > 0 && i >= 0) {
        buf[i--] = '0' + (n % 10);
        n /= 10;
    }
    if (neg && i >= 0) buf[i--] = '-';
    return buf + i + 1;
}

static void _print_int(int n) {
    char buf[16];
    print(_itoa_local(n, buf, sizeof(buf)));
}

// =============================================================================
// SINGLE-CONTEXT COMPATIBILITY SHIMS
//
// If the RSH engine was compiled with only the global-context API
// (script_init, exec_file, call_function, etc. from the Rust #[no_mangle]
// exports in rsh_engine.rs) rather than the per-context variants, flip
// RSH_PLUGIN_MULTI_CTX to 0 and use these shims instead.
//
// WARNING: single-context mode means all plugins share one ScriptCtx.
// Variable and function names can clash across plugins.  Only use this
// if you have just one plugin loaded at a time.
// =============================================================================

#if !RSH_PLUGIN_MULTI_CTX

// The Rust engine exposes these (global context versions):
extern void  script_init        (void);
extern int   script_execute_file(const char *path);
extern int   script_execute_line_c(const char *line);
extern void  script_set_var_c   (const char *name, const char *val);
extern int   script_get_var_c   (const char *name, char *out, int out_size);

void  script_init_ctx    (void *ctx) { (void)ctx; script_init(); }
int   exec_file_ctx      (const char *path, void *ctx)
    { (void)ctx; return script_execute_file(path); }
int   call_function_ctx  (const char *name, const char **args, int argc, void *ctx) {
    (void)args; (void)argc; (void)ctx;
    // Build "call <name>" as a single RSH line.
    char cmd[128] = "call ";
    int i = 5, j = 0;
    while (name[j] && i < 127) cmd[i++] = name[j++];
    cmd[i] = '\0';
    return script_execute_line_c(cmd);
}
int   exec_line_ctx      (const char *line, void *ctx)
    { (void)ctx; return script_execute_line_c(line); }
void  script_set_var_ctx (const char *name, const char *val, void *ctx)
    { (void)ctx; script_set_var_c(name, val); }
int   script_get_var_ctx (const char *name, char *out, int out_len, void *ctx)
    { (void)ctx; return script_get_var_c(name, out, out_len); }

#endif // !RSH_PLUGIN_MULTI_CTX