// spinlock.h
#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>
#include <stdbool.h>

// Check if Spinlock is already defined
#ifndef SPINLOCK_DEFINED
#define SPINLOCK_DEFINED

// Basic spinlock structure
typedef struct Spinlock {
    volatile uint32_t lock;
    uint32_t holder_cpu;
    const char* name;
} Spinlock;

#endif // SPINLOCK_DEFINED

// Advanced spinlock with interrupt state tracking
typedef struct {
    volatile uint32_t lock;
    uint32_t holder_cpu;
    uint32_t saved_flags;
    uint32_t acquire_count;
    const char* name;
} SpinlockAdvanced;

// Basic spinlock functions
void spinlock_init(Spinlock* lock, const char* name);
void spinlock_acquire(Spinlock* lock);
void spinlock_release(Spinlock* lock);
bool spinlock_try_acquire(Spinlock* lock);
bool spinlock_is_locked(Spinlock* lock);

// Advanced spinlock functions
void spinlock_advanced_init(SpinlockAdvanced* lock, const char* name);
void spinlock_advanced_acquire(SpinlockAdvanced* lock);
void spinlock_advanced_release(SpinlockAdvanced* lock);
bool spinlock_advanced_try_acquire(SpinlockAdvanced* lock);

// Legacy compatibility macros (if your code uses these names)
#define spinlockAcquire(lock) spinlock_acquire(lock)
#define spinlockRelease(lock) spinlock_release(lock)

// Spinlock initialization macro
#define SPINLOCK_INIT(name) { .lock = 0, .holder_cpu = 0xFFFFFFFF, .name = name }
#define SPINLOCK_ADVANCED_INIT(name) { .lock = 0, .holder_cpu = 0xFFFFFFFF, .saved_flags = 0, .acquire_count = 0, .name = name }

#endif // SPINLOCK_H