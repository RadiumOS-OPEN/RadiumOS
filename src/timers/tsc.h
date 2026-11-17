#ifndef TSC_H
#define TSC_H

#include <stdint.h>
#include <stdbool.h>

// Calibrates the TSC against the PIT. Safe to call before interrupts and the timer are set up.
void tsc_calibrate(void);
uint32_t tsc_cycles_per_ms(void);

// A point in the future; poll it with tsc_reached().
uint64_t tsc_deadline_ms(uint32_t ms);
bool tsc_reached(uint64_t deadline);

// Waits up to `ms` for the keyboard controller to have data. True if a key arrived (it is not consumed).
bool tsc_wait_key_ms(uint32_t ms);

#endif // TSC_H
