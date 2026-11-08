#ifndef BOOTPAGE_H
#define BOOTPAGE_H

// Runs the startup script, draws the boot page and shows the prompt. Called once at the end of boot.
void boot_finish(void);
// Draws the boot page again (the `bootpage` command).
void show_boot_page(void);
void bootpage_command(int argc, char *argv[]);

// Boot-screen building blocks for the startup script (.radiumrc).
void banner_command(int argc, char *argv[]);
void center_command(int argc, char *argv[]);
void color_command(int argc, char *argv[]);
void box_command(int argc, char *argv[]);

// Dynamic shell variables ($CPU, $MEM_INFO, $UPTIME, ...). Returns NULL for unknown names.
const char *bootpage_var(const char *name);

#endif // BOOTPAGE_H
