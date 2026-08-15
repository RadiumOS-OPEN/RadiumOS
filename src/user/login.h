// modern_login.h - Modern Login System Interface
#ifndef MODERN_LOGIN_H
#define MODERN_LOGIN_H

#include <stdbool.h>

// Main login function - displays modern login screen and authenticates user
// Returns: true if login successful, false if failed
bool modern_login_screen(void);

// Wrapper function for easy integration
void login(void);

#endif // MODERN_LOGIN_H