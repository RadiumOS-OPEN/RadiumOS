#ifndef ACMODS_H
#define ACMODS_H

#include <stdint.h>

int  acmods_scan(void);                                                   // rescans /disk/mods; number of AC mods
int  acmods_info(int i, char name[32], char desc[48], int *enabled);      // 1 if mod i exists
int  acmods_toggle(int i);                                                // 1 = now on, 0 = now off, -1 = build failed
void acmods_run(uint32_t *screen, uint32_t *api);                         // runs every enabled mod once

#endif
