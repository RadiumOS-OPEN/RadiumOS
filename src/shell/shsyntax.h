#ifndef SHSYNTAX_H
#define SHSYNTAX_H

#include <stddef.h>
#include <stdbool.h>

// Runs shell text: one command line or a whole script (newlines separate commands).
// Supports ; && || ! | > >> < quoting, $VAR ${VAR:-x} $? $(cmd) `cmd` $((math)), globs, {a,b} {1..5},
// if/elif/else/fi, for/while/until/do/done, case/esac, { group; }, NAME=value and # comments.
void sh_execute(const char *text);
void sh_abort_reset(void);

// Splits one command into words (quotes removed, variables and substitutions expanded, globs and braces
// expanded). kind[i] is 1 for a redirection operator (> >> < 2> &> ...), else 0. raw keeps quotes and expands
// nothing (inline Helios source). Returns the word count, or -1 if the result does not fit.
int sh_split(const char *text, char *buf, size_t cap, char *argv[], unsigned char kind[], int max, bool raw);

// Same expansion into one string (no word splitting, no globbing). Returns 0, or -1 if it does not fit.
int sh_expand_string(const char *text, char *out, size_t cap);

// Cuts `cmd` in place at every '|' that is not quoted. Returns the stage count, or -1 if there are more than max.
int sh_split_pipes(char *cmd, char *stages[], int max);

// First word of a command when it needs no expansion (quotes removed); false when it has to be expanded first.
bool sh_plain_word(const char *stage, char *out, size_t cap);

void break_command(int argc, char *argv[]);
void continue_command(int argc, char *argv[]);
void read_command(int argc, char *argv[]);
void source_command(int argc, char *argv[]);

#endif // SHSYNTAX_H
