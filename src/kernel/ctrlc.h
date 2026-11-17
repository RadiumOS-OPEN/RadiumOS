#ifndef CTRLC_H
#define CTRLC_H

#include <stdint.h>
#include <stdbool.h>

// System-wide Ctrl+C for commands typed at the shell. While such a command runs, interrupts are on and the timer tick
// (task.c) calls ctrlc_tick(), which watches the keyboard through kbdtap.c; on Ctrl+C it jumps back into the shell
// (run_interactive in keyboard.c). Games and full-screen programs are exempt (ctrlc_exempt) and run as before.

typedef struct { uint32_t ebx, esi, edi, ebp, esp, eip, eflags; } kjmp_t;
int  kjmp_set(kjmp_t *env) __attribute__((returns_twice));
void kjmp_long(kjmp_t *env, int value) __attribute__((noreturn));

extern kjmp_t g_ctrlc_jmp;

bool ctrlc_exempt(const char *line);
void ctrlc_begin(bool ctrl_down);   // after kjmp_set(&g_ctrlc_jmp) returned 0
void ctrlc_end(void);
void ctrlc_disarm(void);            // the running program wants its own keys (a game): stop watching, interrupts off again
bool ctrlc_running(void);           // a command is running under the tap (the tick must not start another task)
void ctrlc_tick(void);
void ctrlc_abort(void) __attribute__((noreturn));   // leave the running command as if Ctrl+C was pressed

// While held, Ctrl+C waits (a disk update must not be cut in half); it takes effect at the matching release.
void ctrlc_hold(void);
void ctrlc_release(void);

typedef struct { char unused; } ctrlc_scope_t;
static inline void ctrlc_scope_end(ctrlc_scope_t *s) { (void)s; ctrlc_release(); }
#define CTRLC_SCOPE ctrlc_scope_t _ctrlc_scope __attribute__((cleanup(ctrlc_scope_end), unused)) = (ctrlc_hold(), (ctrlc_scope_t){0})

#endif
