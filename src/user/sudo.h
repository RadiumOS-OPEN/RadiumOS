#ifndef SUDO_H
#define SUDO_H

#include <stdint.h>
#include <stdbool.h>

void sudo_command(int argc, char* argv[]);
void cpw_command(int argc, char* argv[]);

// True once /disk/.sudoInfo exists, i.e. a sudo password has been created.
bool sudo_is_configured(void);
bool sudo_is_unlocked(void);
uint32_t sudo_unlocked_seconds_left(void);
void sudo_lock(void);
// True while a command line is running under `sudo` (the shell is root only for that command).
bool sudo_is_elevated(void);
// Ctrl+C ended a sudo command line without returning through sudo: back to normal rights.
void sudo_reset_elevation(void);
// Absolute normalized path that a non-root account may not read or change (see users.c).
bool sudo_path_protected(const char *resolved, bool destructive);

// The shell calls this before running `command`: false (after printing why) when it needs sudo
// or touches /disk/.sudoInfo while sudo is locked.
bool sudo_gate(const char *command, int argc, char *argv[]);
// The same for the files named by < and > redirections.
bool sudo_gate_files(const char *command, const char *in_file, const char *out_file);
#endif
