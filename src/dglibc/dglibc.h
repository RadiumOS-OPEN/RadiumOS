// dglibc.h - the small slice of the C library that Doom needs, on top of RadiumOS (AVFS files, Rust heap, terminal).
// Every function is defined in dglibc.c under a dg_ name and renamed here, so nothing clashes with the kernel's own
// string/printf helpers. The stdio.h, stdlib.h, ... headers in this folder only include this file.
#ifndef DGLIBC_H
#define DGLIBC_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <limits.h>

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 512
#define FILENAME_MAX 256
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767
#define PATH_MAX 256

typedef struct dg_file FILE;
typedef long off_t;
typedef int mode_t;

extern FILE *dg_stdin, *dg_stdout, *dg_stderr;
#define stdin dg_stdin
#define stdout dg_stdout
#define stderr dg_stderr

#define printf dg_printf
#define fprintf dg_fprintf
#define sprintf dg_sprintf
#define snprintf dg_snprintf
#define vsnprintf dg_vsnprintf
#define vfprintf dg_vfprintf
#define vprintf dg_vprintf
#define sscanf dg_sscanf
#define puts dg_puts
#define putchar dg_putchar
#define fputs dg_fputs
#define fputc dg_fputc
#define putc dg_fputc
#define fgetc dg_fgetc
#define getc dg_fgetc
#define fgets dg_fgets
#define fopen dg_fopen
#define fclose dg_fclose
#define fread dg_fread
#define fwrite dg_fwrite
#define fseek dg_fseek
#define ftell dg_ftell
#define fflush dg_fflush
#define feof dg_feof
#define ferror dg_ferror
#define rewind dg_rewind
#define remove dg_remove
#define rename dg_rename
#define perror dg_perror
#define setbuf dg_setbuf
#define fileno dg_fileno
#define ungetc dg_ungetc

int dg_printf(const char *fmt, ...);
int dg_fprintf(FILE *f, const char *fmt, ...);
int dg_sprintf(char *buf, const char *fmt, ...);
int dg_snprintf(char *buf, size_t n, const char *fmt, ...);
int dg_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int dg_vfprintf(FILE *f, const char *fmt, va_list ap);
int dg_vprintf(const char *fmt, va_list ap);
int dg_sscanf(const char *s, const char *fmt, ...);
int dg_puts(const char *s);
int dg_putchar(int c);
int dg_fputs(const char *s, FILE *f);
int dg_fputc(int c, FILE *f);
int dg_fgetc(FILE *f);
char *dg_fgets(char *s, int n, FILE *f);
FILE *dg_fopen(const char *path, const char *mode);
int dg_fclose(FILE *f);
size_t dg_fread(void *p, size_t size, size_t n, FILE *f);
size_t dg_fwrite(const void *p, size_t size, size_t n, FILE *f);
int dg_fseek(FILE *f, long off, int whence);
long dg_ftell(FILE *f);
int dg_fflush(FILE *f);
int dg_feof(FILE *f);
int dg_ferror(FILE *f);
void dg_rewind(FILE *f);
int dg_remove(const char *path);
int dg_rename(const char *from, const char *to);
void dg_perror(const char *s);
void dg_setbuf(FILE *f, char *buf);
int dg_fileno(FILE *f);
int dg_ungetc(int c, FILE *f);

#define malloc dg_malloc
#define calloc dg_calloc
#define realloc dg_realloc
#define free dg_free
#define exit dg_exit
#define abort dg_abort
#define atexit dg_atexit
#define atoi dg_atoi
#define atol dg_atoi
#define atof dg_atof
#define strtol dg_strtol
#define strtoul dg_strtoul
#define abs dg_abs
#define labs dg_abs
#define qsort dg_qsort
#define getenv dg_getenv
#define system dg_system
#define rand dg_rand
#define srand dg_srand

void *dg_malloc(size_t n);
void *dg_calloc(size_t n, size_t size);
void *dg_realloc(void *p, size_t n);
void dg_free(void *p);
void dg_exit(int code) __attribute__((noreturn));
void dg_abort(void) __attribute__((noreturn));
int dg_atexit(void (*fn)(void));
int dg_atoi(const char *s);
double dg_atof(const char *s);
long dg_strtol(const char *s, char **end, int base);
unsigned long dg_strtoul(const char *s, char **end, int base);
int dg_abs(int x);
void dg_qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
char *dg_getenv(const char *name);
int dg_system(const char *cmd);
int dg_rand(void);
void dg_srand(unsigned seed);

#define strdup dg_strdup
#define strcasecmp dg_strcasecmp
#define strncasecmp dg_strncasecmp
#define strerror dg_strerror
#define strcpy dg_strcpy
#define strncpy dg_strncpy
#define strcat dg_strcat
#define strncat dg_strncat
#define strlen dg_strlen
#define strcmp dg_strcmp
#define strncmp dg_strncmp
#define strchr dg_strchr
#define strrchr dg_strrchr
#define strstr dg_strstr
#define strpbrk dg_strpbrk
#define memchr dg_memchr
#define toupper dg_toupper
#define tolower dg_tolower
#define isalpha dg_isalpha
#define isdigit dg_isdigit
#define isspace dg_isspace
#define isupper dg_isupper
#define islower dg_islower
#define isalnum dg_isalnum
#define isprint dg_isprint
#define isxdigit dg_isxdigit

char *dg_strdup(const char *s);
int dg_strcasecmp(const char *a, const char *b);
int dg_strncasecmp(const char *a, const char *b, size_t n);
char *dg_strerror(int e);
char *dg_strcpy(char *d, const char *s);
char *dg_strncpy(char *d, const char *s, size_t n);
char *dg_strcat(char *d, const char *s);
char *dg_strncat(char *d, const char *s, size_t n);
size_t dg_strlen(const char *s);
int dg_strcmp(const char *a, const char *b);
int dg_strncmp(const char *a, const char *b, size_t n);
char *dg_strchr(const char *s, int c);
char *dg_strrchr(const char *s, int c);
char *dg_strstr(const char *h, const char *n);
char *dg_strpbrk(const char *s, const char *set);
void *dg_memchr(const void *s, int c, size_t n);
int dg_toupper(int c);
int dg_tolower(int c);
int dg_isalpha(int c);
int dg_isdigit(int c);
int dg_isspace(int c);
int dg_isupper(int c);
int dg_islower(int c);
int dg_isalnum(int c);
int dg_isprint(int c);
int dg_isxdigit(int c);

// memcpy, memmove, memset and memcmp come from the kernel (the compiler also emits calls to them)
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

extern int dg_errno;
#define errno dg_errno
#define ENOENT 2
#define EEXIST 17
#define EINVAL 22
#define ENOMEM 12
#define EISDIR 21

#define fabs __builtin_fabs
#define sqrt __builtin_sqrt

#define mkdir(path, mode) dg_mkdir(path)
#define access(path, mode) dg_access(path)
#define R_OK 4
#define W_OK 2
#define F_OK 0
int dg_mkdir(const char *path);
int dg_access(const char *path);

#define assert(x) ((x) ? (void)0 : dg_assert_fail(#x, __FILE__, __LINE__))
void dg_assert_fail(const char *expr, const char *file, int line) __attribute__((noreturn));

// Frees every block Doom still holds (call after dg_run).
void dg_release_all(void);

// Runs `main_fn` so that exit() / I_Error can come back here; returns the exit code.
int dg_run(int (*main_fn)(void));

#endif // DGLIBC_H
