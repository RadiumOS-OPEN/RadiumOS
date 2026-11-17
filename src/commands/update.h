#ifndef UPDATE_H
#define UPDATE_H

#include <stdbool.h>
#include <stddef.h>

// update [status] | check [SOURCE] | apply [SOURCE] | rollback
void update_command(int argc, char *argv[]);

// True while an unsigned package is installed: the screen shows "(unverified)" down its right edge.
bool update_is_unverified(void);
// Shows or hides that marker to match the state on /disk (call at boot and after anything that changes it).
void update_refresh_marker(void);

// One quiet request for the online manifest; true when it names a newer version (5.1 < 5.2) than the current one.
bool update_online_available(char *version, size_t size);

// The current release version, e.g. "5.1": the newer of the built-in one and the last package installed.
const char *update_current_version(void);

#endif // UPDATE_H
