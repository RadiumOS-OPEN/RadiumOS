#ifndef RSD_H
#define RSD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// RadiumOS service manager: systemd-style units (.svc files), targets, dependencies, timers and a journal.
//   systemctl  list-units [--type=target] | list-unit-files | list-timers | list-dependencies | status | start |
//              stop | restart | enable | disable | is-active | is-enabled | cat | daemon-reload | reset-failed |
//              get-default | set-default | isolate | rescue | default | is-system-running | reboot | poweroff
//   journalctl [-u UNIT] [-n N]
//   systemd-analyze [blame]
void systemctl_command(int argc, char *argv[]);
void journalctl_command(int argc, char *argv[]);
void analyze_command(int argc, char *argv[]);

// Called once at the end of boot: installs the stock units on first use, loads them and brings the system up to the
// boot target (sysinit, basic, multi-user, graphical; or rescue), printing a systemd-style line for each unit.
void rsd_boot(void);

// Stops every unit newest first and reports the targets going down. action is "Reboot" or "Power Off".
void rsd_shutdown(const char *action);

// Runs due timers and restarts. Cheap; call it often from an idle loop of the shell task.
void rsd_poll(void);

// Read-only views for programs that show the system (sysmon).
typedef struct {
    char name[16];
    char desc[64];
    char target[16];
    const char *state;      // active, waiting, failed, inactive
    bool enabled, failed, running;
    uint32_t runs;
} rsd_unit_view_t;

int  rsd_unit_count(void);
bool rsd_unit_view(int n, rsd_unit_view_t *out);
const char *rsd_current_target(void);
// back = 0 is the newest journal line. False past the oldest.
bool rsd_journal_view(int back, uint32_t *ms, char *unit, size_t unit_size, char *text, size_t text_size, int *level);

#endif // RSD_H
