#ifndef STACKGUARD_H
#define STACKGUARD_H

#include <stdint.h>

// Seeds the stack canary used by -fstack-protector-strong; call first thing in kernel_main, before any
// protected function that must return across the call is running.
void stackguard_init(void);
uint32_t stackguard_value(void);

#endif
