#ifndef APPRUN_H
#define APPRUN_H
#include <stdbool.h>

// app FILE [args...]: run a static ELF app (see port/README in port/Makefile)
void app_command(int argc, char *argv[]);
int app_selftest(void);
bool app_file_exists(const char *path);   // on /disk or in the fetch store
void app_abort_cleanup(void);   // after Ctrl+C: frees what a running app holds

#endif
