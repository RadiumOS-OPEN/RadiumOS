#ifndef GUARD_H
#define GUARD_H
void guard_command(int argc, char *argv[]);
void guard_flush_log(void);        // appends new guard events to /disk/guard.log (normal context only)
unsigned guard_alert_count(void);
#endif
