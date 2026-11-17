// cmdline.h - the multiboot command line GRUB passes to the kernel
//
// Words understood by RadiumOS (boot/grub/grub.cfg sets them; press `e` on a GRUB entry to add your own):
//   source=disk | iso | old    which kernel GRUB loaded (see `update`)
//   radium.target=NAME         boot target: rescue, multi-user (default) or graphical   (also systemd.unit=NAME.target)
//   rescue | single | 1        same as radium.target=rescue
//   safe                       rescue target, no network, no startup script
//   nonet                      do not bring the network card up
//   norc                       skip the startup script (.radiumrc)
//   verbose | debug            show the command line and each unit's output while booting
//   quiet                      only print failed units
//   nosplash                   skip the boot splash
//   fastboot                   skip the boot-time waits
//   autoupdate                 after booting run `update install` (the staged package, else the online one)
//   autorollback               after booting run `update rollback` (drop the updated kernel from /disk)
#ifndef CMDLINE_H
#define CMDLINE_H

#include <stdbool.h>
#include <stddef.h>

void cmdline_set(const char *text);
const char *cmdline_get(void);
// True if `word` appears as a whole word.
bool cmdline_has(const char *word);
// Copies the value of key=value into out; false if the key is not there.
bool cmdline_value(const char *key, char *out, size_t size);

#endif // CMDLINE_H
