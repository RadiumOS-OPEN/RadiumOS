#ifndef SHELLVARS_H
#define SHELLVARS_H

#include <stddef.h>

// Environment variable value, or NULL when unset. "PWD" always reflects the current directory.
const char *shell_getenv(const char *name);
// 0 on success, -1 on an invalid name, a value that is too long, or a full table.
int shell_setenv(const char *name, const char *value);

// Replaces $NAME, ${NAME} and $? in `in`. Names that are not defined are left as written, so
// "$1" and other text that merely contains a dollar sign pass through; "\$" produces a literal '$'.
void shell_expand_vars(const char *in, char *out, size_t out_size);

// The text an alias stands for, or NULL.
const char *shell_alias_lookup(const char *name);
// Name of the n-th defined alias (for tab completion), or NULL past the end.
const char *shell_alias_name_at(int n);

// Runs a file of commands (blank lines and # comments ignored). Returns how many ran, or -1 if unreadable.
int shell_run_script(const char *path);
// The startup script that exists, /disk/.radiumrc first, or NULL.
const char *shell_rc_path(void);
// Runs the startup script; -1 if there is none.
int shell_run_rc(void);

void rrc_command(int argc, char *argv[]);

void export_command(int argc, char *argv[]);
void unset_command(int argc, char *argv[]);
void env_command(int argc, char *argv[]);
void alias_command(int argc, char *argv[]);
void unalias_command(int argc, char *argv[]);
void history_command(int argc, char *argv[]);

#endif // SHELLVARS_H
