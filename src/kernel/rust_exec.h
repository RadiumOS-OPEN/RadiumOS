#ifndef RUST_EXEC_H
#define RUST_EXEC_H

#include <stdint.h>
#include <stddef.h>

// Process structure
typedef struct Process Process;

// ELF binary execution
extern int rust_exec_elf(const uint8_t* elf_data, size_t size);

// Flat binary execution (raw machine code)
extern int rust_exec_flat(const uint8_t* code, size_t size);

// Process management
extern Process* rust_create_process(const uint8_t* elf_data, size_t size, uint32_t pid);
extern int rust_run_process(Process* proc);
extern void rust_destroy_process(Process* proc);
extern void rust_create_sample_binaries(void);
extern int rust_exec_file(const char* path);
extern void rust_test_executor(void);

// Testing
extern void rust_test_executor(void);
extern void rust_hello(void);
extern void rust_print_number(int num);

#endif