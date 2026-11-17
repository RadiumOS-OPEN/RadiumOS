/* winrun: a loader for 32-bit Windows programs (PE32) on RadiumOS, and the small slice of Win32 they need (port/win).
 * The program's own code is native i686 code and runs as is; what it imports from kernel32/msvcrt/user32/gdi32 is resolved
 * to the functions in k32.c, crt.c and gui.c. Windows code expects stdcall for the API and a TEB at %fs:0 (see winrun.c). */
#ifndef WINAPI_H
#define WINAPI_H

#include <stdint.h>
#include <stddef.h>

#define WINAPI __attribute__((stdcall))

typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t  LONG;
typedef int      BOOL;
typedef void    *HANDLE;
typedef void    *HMODULE;
typedef void    *LPVOID;
typedef const void *LPCVOID;
typedef uint32_t UINT;
typedef char    *LPSTR;
typedef const char *LPCSTR;
typedef uint16_t WCHAR;
typedef WCHAR   *LPWSTR;
typedef const WCHAR *LPCWSTR;
typedef uint32_t SIZE_T;
typedef int (WINAPI *FARPROC)(void);

#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

/* one exported function or variable of a built-in DLL; `dll` is the canonical lower-case name without ".dll" */
typedef struct { const char *dll; const char *name; void *addr; } win_export_t;

extern const win_export_t win_k32_exports[];
extern const win_export_t win_crt_exports[];
extern const win_export_t win_gui_exports[];

/* ---- loader (pe.c) ---- */
typedef struct pe_module {
    char name[64];                 /* as imported, lower case, without ".dll" */
    uint8_t *base;                 /* image base; for built-in DLLs the address of this struct */
    uint32_t size, entry;          /* SizeOfImage, AddressOfEntryPoint (RVA) */
    int is_dll, builtin, refs;
    struct pe_module *next;
} pe_module_t;

pe_module_t *pe_load(const char *path, int is_main);
pe_module_t *pe_find(const char *name);
pe_module_t *pe_builtin(const char *name);       /* handle for a built-in DLL name, NULL if there is no such DLL */
void *pe_get_proc(pe_module_t *m, const char *name);
void  pe_unload(pe_module_t *m);
void *pe_main_base(void);
void  pe_call_entry(pe_module_t *m);              /* exe: the program's entry point (does not return normally) */
const char *win_path(const char *p);             /* "C:\\dir\\f.txt" -> "/disk/dir/f.txt" (static buffer) */
extern char *win_cmdline;                         /* the whole command line, as GetCommandLineA returns it */
extern int win_argc;
extern char **win_argv;
void win_missing(const char *what) __attribute__((noreturn));

/* ---- k32.c / gui.c hooks ---- */
void win_set_last_error(DWORD e);
void win_gui_shutdown(void);
void win_gui_tick(void);                          /* poll input and show the window; called from Sleep */

#endif
