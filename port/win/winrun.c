/* winrun.c - `app /disk/winrun.app PROGRAM.EXE [args]`: runs a 32-bit Windows program. Sets up the process environment Windows
 * code expects (a TEB at %fs:0 with a PEB, thread-local-storage table, command line), then jumps to the program's entry point. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include "winapi.h"
#include "../rt/radium_api.h"

extern const radium_api_t *radium_api_ptr;

uint8_t *win_teb;
uint32_t *win_tls_array;
char *win_cmdline;
int win_argc;
char **win_argv;
const char *win_exe_path = "";

void win_set_last_error(DWORD e) { if (win_teb) *(DWORD *)(win_teb + 0x34) = e; }

static void setup_process(void)
{
    win_teb = memalign(4096, 4096);
    uint8_t *peb = memalign(4096, 4096);
    memset(win_teb, 0, 4096); memset(peb, 0, 4096);
    win_tls_array = calloc(64, sizeof(uint32_t));
    uint32_t sp;
    __asm__ volatile("movl %%esp, %0" : "=r"(sp));
    uint32_t *t = (uint32_t *)win_teb;
    t[0] = 0xFFFFFFFFu;                       /* end of the exception handler chain */
    t[1] = sp + 0x1000;                       /* stack base ... */
    t[2] = sp - 0x40000;                      /* ... and limit */
    t[6] = (uint32_t)win_teb;                 /* 0x18: self */
    t[8] = 1; t[9] = 1;                       /* process and thread id */
    t[11] = (uint32_t)win_tls_array;          /* 0x2C: implicit TLS */
    t[12] = (uint32_t)peb;                    /* 0x30 */
    *(uint32_t *)(peb + 0x08) = 0;            /* ImageBaseAddress: filled in when the image is mapped */
    *(uint32_t *)(peb + 0x18) = 0x00100000;   /* ProcessHeap */
    *(uint32_t *)(peb + 0xA4) = 6; *(uint32_t *)(peb + 0xA8) = 1; *(uint16_t *)(peb + 0xAC) = 7601; *(uint32_t *)(peb + 0xB0) = 2;
    uint32_t sel = radium_api_ptr->fs_selector((uint32_t)win_teb);
    __asm__ volatile("movw %w0, %%fs" : : "r"(sel));
}

void __attribute__((weak)) win_gui_shutdown(void) {}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: app /disk/winrun.app PROGRAM.EXE [arguments]\n"
               "Runs a 32-bit Windows program (PE32 with relocations: MinGW -Wl,--dynamicbase or MSVC /DYNAMICBASE).\n");
        return 1;
    }
    win_exe_path = argv[1];
    win_argc = argc - 1;
    win_argv = calloc((size_t)argc + 1, sizeof(char *));
    for (int i = 1; i < argc; i++) win_argv[i - 1] = argv[i];
    size_t cap = 64;
    for (int i = 1; i < argc; i++) cap += strlen(argv[i]) + 3;
    win_cmdline = malloc(cap);
    win_cmdline[0] = 0;
    for (int i = 1; i < argc; i++) {
        int sp = strchr(argv[i], ' ') != NULL;
        if (i > 1) strcat(win_cmdline, " ");
        if (sp) strcat(win_cmdline, "\"");
        strcat(win_cmdline, argv[i]);
        if (sp) strcat(win_cmdline, "\"");
    }
    setup_process();
    pe_module_t *m = pe_load(argv[1], 1);
    if (!m) return 1;
    *(uint32_t *)(((uint8_t **)win_teb)[12] + 0x08) = (uint32_t)m->base;
    char *slash = strrchr(argv[1], '/');
    if (slash && slash != argv[1]) { *slash = 0; chdir(argv[1]); *slash = '/'; }
    pe_call_entry(m);
    fflush(NULL);
    return 0;
}
