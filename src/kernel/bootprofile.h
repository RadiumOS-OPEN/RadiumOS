#ifndef BOOTPROFILE_H
#define BOOTPROFILE_H

// Writes a timestamped boot milestone to the QEMU debug console (see bootprofile.c).
void boot_mark(const char *label);

#endif // BOOTPROFILE_H
