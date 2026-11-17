/* pe.c - PE32 loader: maps an image, applies relocations, resolves imports against the built-in DLLs (k32.c, crt.c, gui.c) or
 * other PE files on disk, runs TLS callbacks and DllMain. 32-bit x86 only; the image must carry relocations because the
 * addresses it asks for (0x400000...) belong to the kernel here. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <sys/stat.h>
#include "winapi.h"

#pragma pack(push, 1)
typedef struct { uint16_t e_magic; uint8_t pad[58]; uint32_t e_lfanew; } dos_hdr;
typedef struct { uint16_t Machine, NumberOfSections; uint32_t TimeDateStamp, PointerToSymbolTable, NumberOfSymbols; uint16_t SizeOfOptionalHeader, Characteristics; } file_hdr;
typedef struct { uint32_t VirtualAddress, Size; } data_dir;
typedef struct {
    uint16_t Magic; uint8_t MajorLinker, MinorLinker;
    uint32_t SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData, AddressOfEntryPoint, BaseOfCode, BaseOfData, ImageBase,
             SectionAlignment, FileAlignment;
    uint16_t MajorOS, MinorOS, MajorImage, MinorImage, MajorSubsys, MinorSubsys;
    uint32_t Win32Version, SizeOfImage, SizeOfHeaders, CheckSum;
    uint16_t Subsystem, DllCharacteristics;
    uint32_t SizeOfStackReserve, SizeOfStackCommit, SizeOfHeapReserve, SizeOfHeapCommit, LoaderFlags, NumberOfRvaAndSizes;
    data_dir dir[16];
} opt_hdr;
typedef struct { uint32_t Signature; file_hdr fh; opt_hdr oh; } nt_hdr;
typedef struct { char Name[8]; uint32_t VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData, PtrReloc, PtrLine; uint16_t NumReloc, NumLine; uint32_t Characteristics; } sect_hdr;
typedef struct { uint32_t OriginalFirstThunk, TimeDateStamp, ForwarderChain, Name, FirstThunk; } import_desc;
typedef struct { uint32_t Characteristics, TimeDateStamp; uint16_t Major, Minor; uint32_t Name, Base, NumberOfFunctions, NumberOfNames, AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals; } export_dir;
typedef struct { uint32_t StartRaw, EndRaw, AddressOfIndex, AddressOfCallbacks, SizeOfZeroFill, Characteristics; } tls_dir;
#pragma pack(pop)

enum { DIR_EXPORT = 0, DIR_IMPORT = 1, DIR_RELOC = 5, DIR_TLS = 9, DIR_DELAY = 13 };

static pe_module_t *g_modules;
static pe_module_t *g_main;
static char g_exe_dir[200] = ".";
extern uint32_t *win_tls_array;      /* winrun.c: the table %fs:[0x2C] points to */
static int g_next_tls = 0;

void *pe_main_base(void) { return g_main ? g_main->base : NULL; }
pe_module_t *pe_all(void) { return g_modules; }

void win_missing(const char *what)
{
    fprintf(stderr, "\nwinrun: the program called a Windows function RadiumOS does not have: %s\n", what);
    fflush(NULL);
    exit(127);
}

/* ---- names ----------------------------------------------------------------------------------------------------------------- */

static void canon(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (; in[n] && n < cap - 1; n++) out[n] = (char)(in[n] >= 'A' && in[n] <= 'Z' ? in[n] + 32 : in[n]);
    out[n] = 0;
    if (n > 4 && !strcmp(out + n - 4, ".dll")) out[n - 4] = 0;
    if (!strncmp(out, "api-ms-win-crt", 14) || !strncmp(out, "msvcr", 5) || !strncmp(out, "ucrtbase", 8) || !strncmp(out, "vcruntime", 9) || !strcmp(out, "crtdll"))
        strcpy(out, "msvcrt");
    else if (!strcmp(out, "kernelbase") || !strncmp(out, "api-ms-win-", 11)) strcpy(out, "kernel32");
}

static const char *const k_empty_dlls[] = { "advapi32", "shell32", "ole32", "oleaut32", "winmm", "comctl32", "comdlg32", "ws2_32", "opengl32",
    "glu32", "version", "shlwapi", "imm32", "setupapi", "dbghelp", "psapi", "ntdll", "wsock32", "user32", "gdi32", "kernel32", "msvcrt", NULL };

pe_module_t *pe_builtin(const char *name)
{
    char c[64]; canon(name, c, sizeof c);
    for (pe_module_t *m = g_modules; m; m = m->next) if (m->builtin && !strcmp(m->name, c)) return m;
    for (int i = 0; k_empty_dlls[i]; i++) {
        if (strcmp(k_empty_dlls[i], c)) continue;
        pe_module_t *m = calloc(1, sizeof *m);
        strncpy(m->name, c, sizeof m->name - 1);
        m->builtin = 1; m->is_dll = 1; m->base = (uint8_t *)m; m->refs = 1;
        m->next = g_modules; g_modules = m;
        return m;
    }
    return NULL;
}

pe_module_t *pe_find(const char *name)
{
    char c[64]; canon(name, c, sizeof c);
    for (pe_module_t *m = g_modules; m; m = m->next) if (!strcmp(m->name, c)) return m;
    return NULL;
}

static void *builtin_lookup(const char *dll, const char *name)
{
    const win_export_t *tabs[3] = { win_k32_exports, win_crt_exports, win_gui_exports };
    for (int t = 0; t < 3; t++)
        for (const win_export_t *e = tabs[t]; e->dll; e++)
            if (!strcmp(e->dll, dll) && !strcmp(e->name, name)) return e->addr;
    return NULL;
}

/* a code stub that reports which import was missing when it is called */
static void *make_stub(const char *dll, const char *name)
{
    static uint8_t *mem; static int used;
    if (!mem || used + 16 > 4096) { mem = memalign(4096, 4096); used = 0; }
    char *what = malloc(strlen(dll) + strlen(name) + 3);
    sprintf(what, "%s!%s", dll, name);
    uint8_t *p = mem + used; used += 16;
    p[0] = 0x68; *(uint32_t *)(p + 1) = (uint32_t)what;                          /* push what */
    p[5] = 0xE8; *(int32_t *)(p + 6) = (int32_t)((uint8_t *)win_missing - (p + 10));   /* call win_missing */
    return p;
}

/* ---- file helpers ---------------------------------------------------------------------------------------------------------- */

static uint8_t *read_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *size = (uint32_t)n;
    return b;
}

const char *win_path(const char *p)
{
    static char buf[320];
    size_t o = 0;
    if (!p) return "";
    if (p[0] && p[1] == ':') p += 2;
    for (; *p && o < sizeof(buf) - 6; p++) buf[o++] = *p == '\\' ? '/' : *p;
    buf[o] = 0;
    if (buf[0] == '/' && strncmp(buf, "/disk", 5) && strncmp(buf, "/user", 5) && strncmp(buf, "/tmp", 4)) {
        memmove(buf + 5, buf, o + 1);
        memcpy(buf, "/disk", 5);
    }
    return buf;
}

/* ---- loading --------------------------------------------------------------------------------------------------------------- */

static void *resolve_import(const char *dll, const char *name)
{
    char c[64]; canon(dll, c, sizeof c);
    void *a = NULL;
    pe_module_t *m = pe_find(c);
    if (!m && !pe_builtin(c)) {                       /* a DLL next to the program? */
        char path[300];
        snprintf(path, sizeof path, "%s/%s.dll", g_exe_dir, c);
        m = pe_load(path, 0);
        if (!m) { snprintf(path, sizeof path, "./%s.dll", c); m = pe_load(path, 0); }
    }
    if (m && !m->builtin) a = pe_get_proc(m, name);
    else a = builtin_lookup(c, name);
    return a ? a : make_stub(c, name);
}

static int bind_imports(uint8_t *base, const data_dir *d)
{
    if (!d->VirtualAddress) return 1;
    for (const import_desc *im = (const import_desc *)(base + d->VirtualAddress); im->Name; im++) {
        const char *dll = (const char *)(base + im->Name);
        const uint32_t *lookup = (const uint32_t *)(base + (im->OriginalFirstThunk ? im->OriginalFirstThunk : im->FirstThunk));
        uint32_t *iat = (uint32_t *)(base + im->FirstThunk);
        for (int i = 0; lookup[i]; i++) {
            if (lookup[i] & 0x80000000u) {
                char nm[24]; snprintf(nm, sizeof nm, "#%u", lookup[i] & 0xFFFF);
                iat[i] = (uint32_t)resolve_import(dll, nm);
            } else iat[i] = (uint32_t)resolve_import(dll, (const char *)(base + lookup[i] + 2));
        }
    }
    return 1;
}

pe_module_t *pe_load(const char *path, int is_main)
{
    uint32_t fsz = 0;
    uint8_t *fb = read_file(path, &fsz);
    if (!fb) { if (is_main) fprintf(stderr, "winrun: cannot read %s\n", path); return NULL; }
    const dos_hdr *dh = (const dos_hdr *)fb;
    if (fsz < 0x200 || dh->e_magic != 0x5A4D || dh->e_lfanew + sizeof(nt_hdr) > fsz) { fprintf(stderr, "winrun: %s is not a Windows program\n", path); free(fb); return NULL; }
    const nt_hdr *nt = (const nt_hdr *)(fb + dh->e_lfanew);
    if (nt->Signature != 0x4550) { fprintf(stderr, "winrun: %s: bad PE signature\n", path); free(fb); return NULL; }
    if (nt->fh.Machine != 0x14C || nt->oh.Magic != 0x10B) {
        fprintf(stderr, "winrun: %s is a %s program; only 32-bit (x86) Windows programs can run\n", path, nt->oh.Magic == 0x20B ? "64-bit" : "non-x86");
        free(fb); return NULL;
    }
    const opt_hdr *oh = &nt->oh;
    const data_dir *dirs = oh->dir;
    uint32_t size = (oh->SizeOfImage + 0xFFF) & ~0xFFFu;
    uint8_t *base = memalign(4096, size);
    if (!base) { fprintf(stderr, "winrun: out of memory for the image (%u KB)\n", size >> 10); free(fb); return NULL; }
    memset(base, 0, size);
    memcpy(base, fb, oh->SizeOfHeaders < fsz ? oh->SizeOfHeaders : fsz);
    const sect_hdr *sec = (const sect_hdr *)((const uint8_t *)&nt->oh + nt->fh.SizeOfOptionalHeader);
    for (int i = 0; i < nt->fh.NumberOfSections; i++) {
        uint32_t n = sec[i].SizeOfRawData < sec[i].VirtualSize || !sec[i].VirtualSize ? sec[i].SizeOfRawData : sec[i].VirtualSize;
        if (sec[i].PointerToRawData + n > fsz || sec[i].VirtualAddress + n > size) { n = 0; }
        if (n) memcpy(base + sec[i].VirtualAddress, fb + sec[i].PointerToRawData, n);
    }
    free(fb);

    int32_t delta = (int32_t)((uint32_t)base - oh->ImageBase);
    if (delta) {
        if (!dirs[DIR_RELOC].VirtualAddress) {
            fprintf(stderr, "winrun: %s has no relocation table and wants to live at 0x%X, which belongs to the kernel here.\n"
                            "        Link it with ASLR/relocations (MinGW: -Wl,--dynamicbase; MSVC: /DYNAMICBASE).\n", path, oh->ImageBase);
            free(base); return NULL;
        }
        const uint8_t *r = base + dirs[DIR_RELOC].VirtualAddress, *end = r + dirs[DIR_RELOC].Size;
        while (r + 8 <= end) {
            uint32_t page = *(const uint32_t *)r, blk = *(const uint32_t *)(r + 4);
            if (blk < 8) break;
            for (const uint16_t *e = (const uint16_t *)(r + 8); (const uint8_t *)e < r + blk; e++) {
                int type = *e >> 12; uint32_t off = page + (*e & 0xFFF);
                if (type == 3 && off + 4 <= size) *(uint32_t *)(base + off) += (uint32_t)delta;
                else if (type != 0) { fprintf(stderr, "winrun: unsupported relocation type %d\n", type); free(base); return NULL; }
            }
            r += blk;
        }
    }

    pe_module_t *m = calloc(1, sizeof *m);
    const char *slash = strrchr(path, '/');
    char nm[64]; strncpy(nm, slash ? slash + 1 : path, sizeof nm - 1); nm[sizeof nm - 1] = 0;
    canon(nm, m->name, sizeof m->name);
    m->base = base; m->size = size; m->entry = oh->AddressOfEntryPoint; m->refs = 1;
    m->is_dll = (nt->fh.Characteristics & 0x2000) != 0;
    m->next = g_modules; g_modules = m;
    if (is_main) {
        g_main = m;
        const char *s2 = strrchr(path, '/');
        if (s2) { size_t n = (size_t)(s2 - path); if (n >= sizeof g_exe_dir) n = sizeof g_exe_dir - 1; memcpy(g_exe_dir, path, n); g_exe_dir[n] = 0; if (!n) strcpy(g_exe_dir, "/"); }
    }

    bind_imports(base, &dirs[DIR_IMPORT]);

    /* thread-local storage: one thread, so one block per module */
    const uint32_t tlsrva = dirs[DIR_TLS].VirtualAddress;
    if (tlsrva) {
        const tls_dir *t = (const tls_dir *)(base + tlsrva);
        uint32_t tsz = t->EndRaw - t->StartRaw, total = tsz + t->SizeOfZeroFill;
        uint8_t *blk = calloc(1, total ? total : 4);
        memcpy(blk, (const void *)(t->StartRaw + (uint32_t)delta), tsz);
        int idx = g_next_tls++;
        if (win_tls_array && idx < 64) win_tls_array[idx] = (uint32_t)blk;
        if (t->AddressOfIndex) *(uint32_t *)(t->AddressOfIndex + (uint32_t)delta) = (uint32_t)idx;
        if (t->AddressOfCallbacks)
            for (const uint32_t *cb = (const uint32_t *)(t->AddressOfCallbacks + (uint32_t)delta); *cb; cb++)
                ((void (WINAPI *)(void *, uint32_t, void *))*cb)(base, 1, NULL);
    }

    if (m->is_dll && m->entry) {
        int ok = ((int (WINAPI *)(void *, uint32_t, void *))(base + m->entry))(base, 1, NULL);
        if (!ok) { fprintf(stderr, "winrun: %s refused to load (DllMain failed)\n", path); return NULL; }
    }
    return m;
}

void pe_call_entry(pe_module_t *m)
{
    ((void (WINAPI *)(void))(m->base + m->entry))();
}

void pe_unload(pe_module_t *m)
{
    if (!m || m->builtin || --m->refs > 0) return;
    if (m->is_dll && m->entry) ((int (WINAPI *)(void *, uint32_t, void *))(m->base + m->entry))(m->base, 0, NULL);
    /* the image stays mapped: other modules may still hold pointers into it */
}

void *pe_get_proc(pe_module_t *m, const char *name)
{
    if (!m) return NULL;
    if (m->builtin) return builtin_lookup(m->name, name);
    const dos_hdr *dh = (const dos_hdr *)m->base;
    const nt_hdr *nt = (const nt_hdr *)(m->base + dh->e_lfanew);
    const data_dir *d = &nt->oh.dir[DIR_EXPORT];
    if (!d->VirtualAddress) return NULL;
    const export_dir *ed = (const export_dir *)(m->base + d->VirtualAddress);
    const uint32_t *funcs = (const uint32_t *)(m->base + ed->AddressOfFunctions);
    uint32_t rva = 0;
    if (name[0] == '#') {
        uint32_t ord = (uint32_t)atoi(name + 1);
        if (ord >= ed->Base && ord - ed->Base < ed->NumberOfFunctions) rva = funcs[ord - ed->Base];
    } else {
        const uint32_t *names = (const uint32_t *)(m->base + ed->AddressOfNames);
        const uint16_t *ords = (const uint16_t *)(m->base + ed->AddressOfNameOrdinals);
        for (uint32_t i = 0; i < ed->NumberOfNames; i++)
            if (!strcmp((const char *)(m->base + names[i]), name)) { rva = funcs[ords[i]]; break; }
    }
    if (!rva) return NULL;
    if (rva >= d->VirtualAddress && rva < d->VirtualAddress + d->Size) {      /* forwarded export "DLL.func" */
        char dll[64], fn[96];
        const char *f = (const char *)(m->base + rva), *dot = strchr(f, '.');
        if (!dot) return NULL;
        size_t n = (size_t)(dot - f); if (n >= sizeof dll) n = sizeof dll - 1;
        memcpy(dll, f, n); dll[n] = 0; strncpy(fn, dot + 1, sizeof fn - 1); fn[sizeof fn - 1] = 0;
        pe_module_t *o = pe_find(dll) ? pe_find(dll) : pe_builtin(dll);
        return o ? (o->builtin ? builtin_lookup(o->name, fn) : pe_get_proc(o, fn)) : NULL;
    }
    return m->base + rva;
}
