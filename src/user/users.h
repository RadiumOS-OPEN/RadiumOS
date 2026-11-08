#ifndef USERS_H
#define USERS_H

#include <stdint.h>
#include <stdbool.h>

// Accounts live in /disk/etc/passwd ("name:uid:sudo:hash", hash = PBKDF2 record from rust_pw_hash, empty = locked).
// Until that file exists the system keeps its old single-user behaviour: no login, the session is root.
// Once it exists a login is required at boot, root may be locked, and sudo needs an account with sudo rights.

bool        users_enabled(void);
const char *users_current_name(void);
uint32_t    users_current_uid(void);
bool        users_is_root(void);              // uid 0, or a command line running under sudo
bool        users_can_sudo(void);             // the session user may use sudo
bool        users_verify_current(const char *password);

// Asks for a login before the shell starts. Does nothing without an account database or with `nologin` on the command line.
void        users_boot_login(void);

// Graphical login (desktop): the same accounts without the text prompt.
int         users_count(void);
const char *users_name_at(int i);
bool        users_has_password_at(int i);
bool        users_sudo_at(int i);
bool        users_has_session(void);
bool        users_login_skipped(void);        // `nologin` on the command line: session is root
bool        users_session_login(const char *name, const char *password);
void        users_session_logout(void);
void        users_login_prompt(void);         // blocks until a valid login

// Reads one line with the keyboard (echo '*' when hidden). False on Ctrl+C.
bool        users_read_line(char *out, int cap, bool echo);

int users_selftest(void);

void useradd_command(int argc, char *argv[]);
void userdel_command(int argc, char *argv[]);
void usermod_command(int argc, char *argv[]);
void passwd_command(int argc, char *argv[]);
void su_command(int argc, char *argv[]);
void id_command(int argc, char *argv[]);
void users_command(int argc, char *argv[]);
void logout_command(int argc, char *argv[]);

#endif
