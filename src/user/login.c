// login.c - the old window-based login screen was never reachable and read characters as scancodes; login now lives in users.c
#include "login.h"
#include "users.h"

bool modern_login_screen(void) {
    users_login_prompt();
    return true;
}

void login(void) {
    users_login_prompt();
}
