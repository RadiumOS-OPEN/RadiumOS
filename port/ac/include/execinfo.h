#ifndef RADIUM_EXECINFO_H
#define RADIUM_EXECINFO_H
static inline int backtrace(void **a, int n) { (void)a; (void)n; return 0; }
static inline char **backtrace_symbols(void *const *a, int n) { (void)a; (void)n; return 0; }
static inline void backtrace_symbols_fd(void *const *a, int n, int fd) { (void)a; (void)n; (void)fd; }
#endif
