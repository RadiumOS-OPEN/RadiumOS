// spinlock.c
#include "spinlock.h"
#include "../terminal/terminal.h"

// CPU identification (implement based on your system)
static inline uint32_t get_cpu_id(void) {
    // For single-core systems, always return 0
    // For multi-core, implement APIC ID reading
    return 0;
}

// Atomic exchange operation
static inline uint32_t atomic_exchange(volatile uint32_t* ptr, uint32_t new_value) {
    uint32_t old_value;
    __asm__ volatile(
        "xchg %0, %1"
        : "=r"(old_value), "+m"(*ptr)
        : "0"(new_value)
        : "memory"
    );
    return old_value;
}

// Atomic compare and exchange
static inline bool atomic_compare_exchange(volatile uint32_t* ptr, uint32_t expected, uint32_t new_value) {
    uint32_t prev;
    __asm__ volatile(
        "lock cmpxchgl %2, %1"
        : "=a"(prev), "+m"(*ptr)
        : "r"(new_value), "0"(expected)
        : "memory"
    );
    return prev == expected;
}

// CPU pause instruction for spin-waiting
static inline void cpu_pause(void) {
    __asm__ volatile("pause" ::: "memory");
}

// Disable interrupts and return previous state
static inline uint32_t disable_interrupts(void) {
    uint32_t flags;
    __asm__ volatile(
        "pushf\n\t"
        "pop %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );
    return flags;
}

// Restore interrupt state
static inline void restore_interrupts(uint32_t flags) {
    if (flags & 0x200) { // IF flag
        __asm__ volatile("sti" ::: "memory");
    }
}

// Initialize spinlock
void spinlock_init(Spinlock* lock, const char* name) {
    if (!lock) {
        return;
    }
    
    lock->lock = 0;
    lock->holder_cpu = 0xFFFFFFFF;
    lock->name = name ? name : "unnamed";
}

// Acquire spinlock
void spinlock_acquire(Spinlock* lock) {
    if (!lock) {
        return;
    }
    
    uint32_t cpu_id = get_cpu_id();
    uint32_t spin_count = 0;
    const uint32_t MAX_SPIN = 0x10000000; // Prevent infinite loops
    
    // Disable interrupts to prevent deadlock
    disable_interrupts();
    
    // Spin until we acquire the lock
    while (atomic_exchange(&lock->lock, 1) != 0) {
        // Use pause instruction to improve performance and reduce power consumption
        cpu_pause();
        
        spin_count++;
        if (spin_count > MAX_SPIN) {
            // Deadlock detection
            print("[spinlock] WARNING: Possible deadlock detected on lock '");
            if (lock->name) {
                print(lock->name);
            }
            print("' (held by CPU ");
            print_decimal(lock->holder_cpu);
            print(")\n");
            spin_count = 0;
        }
    }
    
    // Record which CPU holds the lock
    lock->holder_cpu = cpu_id;
    
    // Memory barrier to ensure lock acquisition is visible
    __asm__ volatile("mfence" ::: "memory");
}

// Release spinlock
void spinlock_release(Spinlock* lock) {
    if (!lock) {
        return;
    }
    
    // Check if we actually hold the lock
    uint32_t cpu_id = get_cpu_id();
    if (lock->holder_cpu != cpu_id) {
        print("[spinlock] WARNING: CPU ");
        print_decimal(cpu_id);
        print(" releasing lock '");
        if (lock->name) {
            print(lock->name);
        }
        print("' held by CPU ");
        print_decimal(lock->holder_cpu);
        print("\n");
    }
    
    // Clear holder
    lock->holder_cpu = 0xFFFFFFFF;
    
    // Memory barrier before releasing
    __asm__ volatile("mfence" ::: "memory");
    
    // Release the lock
    lock->lock = 0;
    
    // Re-enable interrupts
    restore_interrupts(0x200); // Enable IF flag
}

// Try to acquire spinlock without blocking
bool spinlock_try_acquire(Spinlock* lock) {
    if (!lock) {
        return false;
    }
    
    // Try to acquire lock
    if (atomic_compare_exchange(&lock->lock, 0, 1)) {
        // Successfully acquired
        lock->holder_cpu = get_cpu_id();
        __asm__ volatile("mfence" ::: "memory");
        return true;
    }
    
    return false;
}

// Check if spinlock is locked
bool spinlock_is_locked(Spinlock* lock) {
    if (!lock) {
        return false;
    }
    
    return lock->lock != 0;
}