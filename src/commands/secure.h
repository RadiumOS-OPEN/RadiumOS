#ifndef SECURE_H
#define SECURE_H

#include <stdbool.h>

// Security tasks (see secure.c).
void secaudit_command(int argc, char *argv[]);
void integrity_command(int argc, char *argv[]);
void prp_command(int argc, char *argv[]);
void sha256sum_command(int argc, char *argv[]);
void vault_command(int argc, char *argv[]);
void shred_command(int argc, char *argv[]);
void genpw_command(int argc, char *argv[]);

// Number of watched files that changed since `integrity init` (printing them), or -1 when there is no record.
int integrity_check(bool quiet);

#endif // SECURE_H
