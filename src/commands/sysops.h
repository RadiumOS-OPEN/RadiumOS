#ifndef SYSOPS_H
#define SYSOPS_H

// System monitoring and maintenance commands (see sysops.c).
void cpuinfo_command(int argc, char *argv[]);
void meminfo_command(int argc, char *argv[]);
void lspci_command(int argc, char *argv[]);
void watch_command(int argc, char *argv[]);
void sysstat_command(int argc, char *argv[]);
void syshealth_command(int argc, char *argv[]);

#endif // SYSOPS_H
