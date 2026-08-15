#pragma once
#include <stdint.h>
#include <stdbool.h>

// Constants
#define NUM_GDT_ENTRIES 6
#define MAX_TASKS 64
#define MAX_EVENTS 128
#define MAX_EVENT_HANDLERS 32

// VGA Memory
#define VGA_MEMORY ((uint16_t*)0xB8000)

// GDT Selectors
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20
#define GDT_TSS         0x28

// Requested Privilege Level
#define RPL_USER 3

// Event Types
typedef enum {
    EVENT_NONE = 0,
    EVENT_TIMER,
    EVENT_KEYBOARD,
    EVENT_NETWORK,
    EVENT_DISK_IO,
    EVENT_CUSTOM,
    EVENT_TASK_COMPLETE,
    EVENT_DATA_READY
} EventType;

// Task States
typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_WAITING,      // Waiting for event
    TASK_BLOCKED,
    TASK_TERMINATED
} TaskState;

// Event structure
typedef struct {
    EventType type;
    uint32_t source_pid;
    uint32_t data;
    void* ptr_data;
    bool is_active;
    uint32_t timestamp;
} Event;

// Event handler callback type
typedef void (*EventHandler)(Event* event);

// Event subscription
typedef struct {
    EventType event_type;
    uint32_t task_pid;
    EventHandler handler;
    bool is_active;
} EventSubscription;

// Async operation
typedef struct {
    uint32_t id;
    uint32_t task_pid;
    EventType wait_event;
    void* result_buffer;
    bool is_complete;
    uint32_t timeout;
} AsyncOperation;

// GDT Entry
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) GDTEntry;

// GDT Pointer
typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) GDTPointer;

// Task State Segment (TSS)
typedef struct {
    uint16_t previous_task, __previous_task_unused;
    uint32_t esp0;
    uint16_t ss0, __ss0_unused;
    uint32_t esp1;
    uint16_t ss1, __ss1_unused;
    uint32_t esp2;
    uint16_t ss2, __ss2_unused;
    uint32_t cr3;
    uint32_t eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint16_t es, __es_unused;
    uint16_t cs, __cs_unused;
    uint16_t ss, __ss_unused;
    uint16_t ds, __ds_unused;
    uint16_t fs, __fs_unused;
    uint16_t gs, __gs_unused;
    uint16_t ldt_selector, __ldt_sel_unused;
    uint16_t debug_flag, io_map;
} __attribute__((packed)) TSS;

// IDT Entry (Gate Descriptor)
typedef struct {
    uint16_t isr_low;
    uint16_t segment_selector;
    uint8_t  reserved;
    uint8_t  attributes;
    uint16_t isr_high;
} __attribute__((packed)) IDTEntry;

// IDT Pointer
typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) IDTPointer;

// Trap Frame (pushed on interrupt)
typedef struct {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t interrupt, error;
    uint32_t eip, cs, eflags, usermode_esp, usermode_ss;
} TrapFrame;

// New Task Kernel Stack Layout
typedef struct {
    uint32_t ebp, edi, esi, ebx;
    uint32_t switch_context_return_addr;
    uint32_t data_selector;
    uint32_t eip, cs, eflags, usermode_esp, usermode_ss;
} NewTaskKernelStack;

// Task Control Block
typedef struct {
    uint32_t id;              // Internal slot ID (0 to MAX_TASKS-1)
    uint32_t pid;             // Process ID (starts from 9123)
    uint32_t kesp;            // Kernel stack pointer
    uint32_t kesp_bottom;     // Bottom of kernel stack
    uint8_t priority;         // Task priority (0-255, higher = more important)
    uint32_t time_slice;      // How many scheduler ticks this task gets
    uint32_t remaining_time;  // Ticks remaining in current time slice
    TaskState state;          // Current task state
    bool is_active;           // Whether this slot is in use
    
    // Async fields
    EventType waiting_for_event;  // Event type this task is waiting for
    uint32_t event_data;          // Data from received event
    AsyncOperation* current_async_op;  // Current async operation
} Task;

// Assembly functions (in multitask.asm)
void load_gdt(uint32_t addr);
void switch_context(Task* from, Task* to);
void new_task_setup();

// GDT functions
int setup_gdt();
int set_gdt_entry(uint32_t num, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags);

// IDT functions
int setup_interrupts(void);
int set_idt_entry(uint8_t vector, void* isr, uint8_t attributes);

// PIC functions
int remap_pic(void);

// PIT functions
int setup_pit(uint32_t frequency);

// Interrupt handling
void handle_interrupt(TrapFrame regs);

// Task management functions
int setup_tasks();
int create_task(uint32_t eip, uint32_t user_stack, uint32_t kernel_stack, bool is_kernel_task, uint8_t priority);
void schedule();
void cleanup_task(uint32_t pid);
void block_task(uint32_t pid);
void unblock_task(uint32_t pid);
void set_task_priority(uint32_t pid, uint8_t new_priority);
void yield();

// Task query functions
uint32_t get_current_pid();
Task* get_task_by_pid(uint32_t pid);
bool validate_task_memory(uint32_t task_id);

// Async/Event System
void init_event_system();
uint32_t emit_event(EventType type, uint32_t data, void* ptr_data);
void subscribe_event(uint32_t task_pid, EventType event_type, EventHandler handler);
void unsubscribe_event(uint32_t task_pid, EventType event_type);
void wait_for_event(EventType event_type);
void process_events();

// Async operations
uint32_t async_start_operation(EventType completion_event, void* result_buffer, uint32_t timeout);
bool async_is_complete(uint32_t async_id);
void async_complete(uint32_t async_id, void* result);

// Async monitoring tasks
void async_cpu_monitor_task(uint32_t pid);
void async_memory_monitor_task(uint32_t pid);
void async_network_monitor_task(uint32_t pid);
void async_disk_monitor_task(uint32_t pid);
void event_generator_task(uint32_t pid);
void start_async_monitors();

// Helper functions
void print_string_at(const char* str, int x, int y, uint8_t color);
void print_hex_at(uint32_t value, int x, int y, uint8_t color);
void runme();
// Inline assembly macros
#define halt() asm volatile("hlt")
#define enable_interrupts() asm volatile("sti")
#define disable_interrupts() asm volatile("cli")