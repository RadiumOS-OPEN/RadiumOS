#include "task.h"
#include "../utility/utility.h"
#include "../terminal/terminal.h"
#include "../errors/error.h"
#include "../io/io.h"
#include "../keyboard/keyboard.h"
extern void rust_schedule(void);
// ----- GDT / TSS -----

GDTEntry gdt_entries[NUM_GDT_ENTRIES];
GDTPointer gdt_pointer;
TSS tss;

int set_gdt_entry(uint32_t num, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    if (num >= NUM_GDT_ENTRIES) {
        memory_error("GDT entry setup", "0x001");
        return 0;
    }
    
    gdt_entries[num].base_low    = base & 0xFFFF;
    gdt_entries[num].base_mid    = base >> 16 & 0xFF;
    gdt_entries[num].base_high   = base >> 24 & 0xFF;
    gdt_entries[num].limit_low   = limit & 0xFFFF;
    gdt_entries[num].granularity = (flags & 0xF0) | (limit >> 16 & 0xF);
    gdt_entries[num].access      = access;
    
    return 1;
}

int setup_gdt() {
    gdt_pointer.limit = NUM_GDT_ENTRIES * 8 - 1;
    gdt_pointer.base = (uint32_t) &gdt_entries;

    memset((uint8_t*) &tss, 0, sizeof(tss));
    tss.ss0 = GDT_KERNEL_DATA;

    if (!set_gdt_entry(0, 0, 0, 0, 0)) return 0;
    if (!set_gdt_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xC0)) return 0;
    if (!set_gdt_entry(2, 0, 0xFFFFFFFF, 0x92, 0xC0)) return 0;
    if (!set_gdt_entry(3, 0, 0xFFFFFFFF, 0xFA, 0xC0)) return 0;
    if (!set_gdt_entry(4, 0, 0xFFFFFFFF, 0xF2, 0xC0)) return 0;
    if (!set_gdt_entry(5, (uint32_t) &tss, sizeof(tss), 0x89, 0x40)) return 0;

    load_gdt((uint32_t) &gdt_pointer);
    asm("ltr %%ax" :: "a"((uint16_t) GDT_TSS));
    
    return 1;
}

// ----- IDT / Interrupts -----

IDTEntry idt[256] __attribute__((aligned(16)));
IDTPointer idt_pointer;

int set_idt_entry(uint8_t vector, void* isr, uint8_t attributes) {
    if (!isr) {
        system_error("IDT entry setup", "0x003");
        return 0;
    }
    
    idt[vector].isr_low    = (uint32_t) isr & 0xFFFF;
    idt[vector].segment_selector = GDT_KERNEL_CODE;
    idt[vector].reserved   = 0;
    idt[vector].attributes = attributes;
    idt[vector].isr_high   = (uint32_t) isr >> 16;
    
    return 1;
}

int remap_pic() {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0x00);
    outb(0xA1, 0x00);
    
    return 1;
}

extern void* isr_redirect_table[];
extern void isr128();

int setup_interrupts() {
    if (!remap_pic()) {
        hardware_fault("PIC", "0x002");
        return 0;
    }

    memset((uint8_t*) &idt, 0, sizeof(IDTEntry) * 256);

    for (int i = 0; i < 48; i++) {
        if (!isr_redirect_table[i]) {
            system_error("ISR table initialization", "0x004");
            continue;
        }
        if (!set_idt_entry(i, isr_redirect_table[i], 0x8E)) {
            return 0;
        }
    }
    
    if (!set_idt_entry(0x80, isr128, 0xEE)) {
        return 0;
    }

    idt_pointer.limit = sizeof(IDTEntry) * 256 - 1;
    idt_pointer.base  = (uint32_t) &idt;
    asm("lidt %0" :: "m"(idt_pointer));
    
    return 1;
}
void handle_interrupt(TrapFrame regs) {
    if (regs.interrupt > 255) {
        system_error("Invalid interrupt vector", "0x005");
        return;
    }
    
    // Visual indicator at top of screen
    *(VGA_MEMORY + 79) = 0xF100 | 'T';

    if (regs.interrupt >= 32 && regs.interrupt <= 47) {
        // Timer interrupt (IRQ 0)
        if (regs.interrupt == 32) {
            // Call Rust scheduler instead of C scheduler
            extern void rust_schedule(void);
            rust_schedule();
        }
        
        // Send EOI to PIC
        if (regs.interrupt >= 40) {
            outb(0xA0, 0x20);
        }
        outb(0x20, 0x20);
    }
}

int setup_pit(uint32_t frequency) {
    if (frequency == 0) {
        hardware_fault("PIT", "0x007");
        return 0;
    }
    
    uint32_t divisor = 1193180 / frequency;
    outb(0x43, 0x36);
    uint8_t l = (uint8_t) (divisor & 0xFF);
    uint8_t h = (uint8_t) (divisor >> 8 & 0xFF);
    outb(0x40, l);
    outb(0x40, h);
    
    return 1;
}

// ----- Task Management with PID System -----

Task tasks[MAX_TASKS];
int num_tasks;
Task* current_task;
uint32_t next_pid = 9123;

// Stack allocation functions
uint32_t allocate_kernel_stack() {
    static uint32_t stack_base = 0x200000;
    static uint32_t stack_size = 0x4000;
    uint32_t allocated_stack = stack_base;
    stack_base += stack_size;
    return allocated_stack + stack_size;
}

uint32_t allocate_user_stack() {
    static uint32_t user_stack_base = 0x400000;
    static uint32_t user_stack_size = 0x4000;
    uint32_t allocated_stack = user_stack_base;
    user_stack_base += user_stack_size;
    return allocated_stack + user_stack_size;
}

uint32_t allocate_pid() {
    return next_pid++;
}

int find_free_task_slot() {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (!tasks[i].is_active) {
            return i;
        }
    }
    return -1;
}

Task* get_task_by_pid(uint32_t pid) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].is_active && tasks[i].pid == pid) {
            return &tasks[i];
        }
    }
    return NULL;
}

int create_task(uint32_t eip, uint32_t user_stack, uint32_t kernel_stack, bool is_kernel_task, uint8_t priority) {
    int id = find_free_task_slot();
    if (id == -1) {
        task_error("Task creation - no free slots", "0x009");
        return -1;
    }
    
    if (!eip) {
        memory_error("Invalid task entry point", "0x00A");
        return -1;
    }

    // Allocate stacks if not provided
    if (kernel_stack == 0) {
        kernel_stack = allocate_kernel_stack();
        if (kernel_stack == 0) {
            memory_error("Kernel stack allocation failed", "0x00C");
            return -1;
        }
    }
    
    if (!is_kernel_task && user_stack == 0) {
        user_stack = allocate_user_stack();
        if (user_stack == 0) {
            memory_error("User stack allocation failed", "0x00D");
            return -1;
        }
    }

    uint32_t pid = allocate_pid();

    if (!is_kernel_task) {
        user_stack -= 4;
        *(uint32_t*) user_stack = pid;
        user_stack -= 4;
        *(uint32_t*) user_stack = 0;
    }

    uint32_t code_selector = is_kernel_task ? GDT_KERNEL_CODE : (GDT_USER_CODE | RPL_USER);
    uint32_t data_selector = is_kernel_task ? GDT_KERNEL_DATA : (GDT_USER_DATA | RPL_USER);

    uint8_t* kesp = (uint8_t*) kernel_stack;

    kesp -= sizeof(NewTaskKernelStack);
    NewTaskKernelStack* stack = (NewTaskKernelStack*) kesp;
    stack->ebp = stack->edi = stack->esi = stack->ebx = 0;
    stack->switch_context_return_addr = (uint32_t) new_task_setup;
    stack->data_selector = data_selector;
    stack->eip = eip;
    stack->cs = code_selector;
    stack->eflags = 0x200;

    if (!is_kernel_task) {
        stack->usermode_esp = user_stack; 
        stack->usermode_ss = data_selector;
    } else {
        stack->usermode_esp = (uint32_t) kesp;
        stack->usermode_ss = data_selector;
    }

    tasks[id].kesp_bottom = kernel_stack;
    tasks[id].kesp = (uint32_t) kesp;
    tasks[id].id = id;
    tasks[id].pid = pid;
    tasks[id].priority = priority;
    tasks[id].time_slice = priority;
    tasks[id].remaining_time = tasks[id].time_slice;
    tasks[id].state = TASK_READY;
    tasks[id].is_active = true;
    tasks[id].waiting_for_event = EVENT_NONE;
    tasks[id].event_data = 0;
    tasks[id].current_async_op = NULL;
    
    num_tasks++;
    
    return pid;
}

int setup_tasks() {
    memset((uint8_t*) tasks, 0, sizeof(Task) * MAX_TASKS);

    num_tasks = 1;
    current_task = &tasks[0];
    current_task->id = 0;
    current_task->pid = 9123;
    current_task->priority = 255;
    current_task->time_slice = 255;
    current_task->remaining_time = 255;
    current_task->state = TASK_RUNNING;
    current_task->is_active = true;
    current_task->kesp_bottom = 0x100000;
    current_task->kesp = 0x100000 - 0x1000;
    current_task->waiting_for_event = EVENT_NONE;
    current_task->event_data = 0;
    current_task->current_async_op = NULL;
    
    next_pid = 9124;
    
    if (!current_task) {
        kernel_panic("Task initialization failed", "0x00F");
        return 0;
    }
    
    return 1;
}

// ----- Event System -----

Event event_queue[MAX_EVENTS];
EventSubscription event_subscriptions[MAX_EVENT_HANDLERS];
AsyncOperation async_operations[MAX_TASKS];
uint32_t system_tick_count = 0;
uint32_t next_async_id = 1;

void init_event_system() {
    memset((uint8_t*)event_queue, 0, sizeof(Event) * MAX_EVENTS);
    memset((uint8_t*)event_subscriptions, 0, sizeof(EventSubscription) * MAX_EVENT_HANDLERS);
    memset((uint8_t*)async_operations, 0, sizeof(AsyncOperation) * MAX_TASKS);
    system_tick_count = 0;
    next_async_id = 1;
}

uint32_t emit_event(EventType type, uint32_t data, void* ptr_data) {
    for (int i = 0; i < MAX_EVENTS; i++) {
        if (!event_queue[i].is_active) {
            event_queue[i].type = type;
            event_queue[i].source_pid = get_current_pid();
            event_queue[i].data = data;
            event_queue[i].ptr_data = ptr_data;
            event_queue[i].is_active = true;
            event_queue[i].timestamp = system_tick_count;
            return i;
        }
    }
    return 0xFFFFFFFF;
}

void subscribe_event(uint32_t task_pid, EventType event_type, EventHandler handler) {
    for (int i = 0; i < MAX_EVENT_HANDLERS; i++) {
        if (!event_subscriptions[i].is_active) {
            event_subscriptions[i].event_type = event_type;
            event_subscriptions[i].task_pid = task_pid;
            event_subscriptions[i].handler = handler;
            event_subscriptions[i].is_active = true;
            return;
        }
    }
}

void unsubscribe_event(uint32_t task_pid, EventType event_type) {
    for (int i = 0; i < MAX_EVENT_HANDLERS; i++) {
        if (event_subscriptions[i].is_active &&
            event_subscriptions[i].task_pid == task_pid &&
            event_subscriptions[i].event_type == event_type) {
            event_subscriptions[i].is_active = false;
        }
    }
}

void wait_for_event(EventType event_type) {
    Task* task = get_task_by_pid(get_current_pid());
    if (task) {
        task->state = TASK_WAITING;
        task->waiting_for_event = event_type;
        
    }
}

void process_events() {
    for (int i = 0; i < MAX_EVENTS; i++) {
        if (!event_queue[i].is_active) continue;
        
        Event* event = &event_queue[i];
        
        for (int j = 0; j < MAX_EVENT_HANDLERS; j++) {
            if (!event_subscriptions[j].is_active) continue;
            
            if (event_subscriptions[j].event_type == event->type) {
                Task* waiting_task = get_task_by_pid(event_subscriptions[j].task_pid);
                if (waiting_task && waiting_task->state == TASK_WAITING && 
                    waiting_task->waiting_for_event == event->type) {
                    waiting_task->state = TASK_READY;
                    waiting_task->event_data = event->data;
                    waiting_task->waiting_for_event = EVENT_NONE;
                }
                
                if (event_subscriptions[j].handler) {
                    event_subscriptions[j].handler(event);
                }
            }
        }
        
        for (int j = 0; j < MAX_TASKS; j++) {
            if (async_operations[j].id != 0 &&
                async_operations[j].wait_event == event->type &&
                !async_operations[j].is_complete) {
                async_operations[j].is_complete = true;
                
                Task* task = get_task_by_pid(async_operations[j].task_pid);
                if (task && task->state == TASK_WAITING) {
                    task->state = TASK_READY;
                    task->waiting_for_event = EVENT_NONE;
                }
            }
        }
        
        event->is_active = false;
    }
}

uint32_t async_start_operation(EventType completion_event, void* result_buffer, uint32_t timeout) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (async_operations[i].id == 0) {
            async_operations[i].id = next_async_id++;
            async_operations[i].task_pid = get_current_pid();
            async_operations[i].wait_event = completion_event;
            async_operations[i].result_buffer = result_buffer;
            async_operations[i].is_complete = false;
            async_operations[i].timeout = system_tick_count + timeout;
            return async_operations[i].id;
        }
    }
    return 0;
}

bool async_is_complete(uint32_t async_id) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (async_operations[i].id == async_id) {
            if (async_operations[i].timeout > 0 && 
                system_tick_count > async_operations[i].timeout) {
                async_operations[i].is_complete = true;
                return true;
            }
            return async_operations[i].is_complete;
        }
    }
    return false;
}

void async_complete(uint32_t async_id, void* result) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (async_operations[i].id == async_id) {
            if (async_operations[i].result_buffer && result) {
                *(uint32_t*)async_operations[i].result_buffer = *(uint32_t*)result;
            }
            async_operations[i].is_complete = true;
            
            emit_event(async_operations[i].wait_event, async_id, result);
            break;
        }
    }
}

// ----- Scheduler -----

void schedule() {
    if (num_tasks == 0 || !current_task) {
        return;
    }

    system_tick_count++;
    
    // Show we're being called (debug)
    *(VGA_MEMORY + 79) = 0x4F00 | '.';

    // Process events
    process_events();

    // Decrement time slice
    if (current_task->remaining_time > 0) {
        current_task->remaining_time--;
    }

    // If current task still has time, keep it running
    if (current_task->remaining_time > 0 && current_task->state == TASK_RUNNING) {
        return;
    }

    // Find next task (simple round-robin)
    int next_id = (current_task->id + 1) % MAX_TASKS;
    int attempts = 0;
    
    while (attempts < MAX_TASKS) {
        Task* next = &tasks[next_id];
        
        if (next->is_active && 
            next->kesp_bottom != 0 && 
            next->kesp != 0 &&
            next->kesp < next->kesp_bottom) {
            
            // Reset time slice
            next->remaining_time = next->time_slice;
            
            Task* old = current_task;
            if (old->state == TASK_RUNNING) {
                old->state = TASK_READY;
            }
            
            current_task = next;
            current_task->state = TASK_RUNNING;
            tss.esp0 = next->kesp_bottom;
            
            switch_context(old, next);
            return;
        }
        
        next_id = (next_id + 1) % MAX_TASKS;
        attempts++;
    }
    
    // No other tasks, reset current task's time
    current_task->remaining_time = current_task->time_slice;
}
void block_task(uint32_t pid) {
    Task* task = get_task_by_pid(pid);
    if (task && task->is_active) {
        task->state = TASK_BLOCKED;
        if (task == current_task) {
            schedule();
        }
    }
}

void unblock_task(uint32_t pid) {
    Task* task = get_task_by_pid(pid);
    if (task && task->is_active) {
        task->state = TASK_READY;
        task->remaining_time = task->time_slice;
    }
}

void set_task_priority(uint32_t pid, uint8_t new_priority) {
    Task* task = get_task_by_pid(pid);
    if (task && task->is_active) {
        task->priority = new_priority;
        task->time_slice = new_priority;
        task->remaining_time = task->time_slice;
    }
}

void cleanup_task(uint32_t pid) {
    Task* task = get_task_by_pid(pid);
    if (!task) {
        return;
    }
    
    if (task->pid == 9123) {
        return;
    }
    
    task->is_active = false;
    task->state = TASK_TERMINATED;
    num_tasks--;
    
    memset(task, 0, sizeof(Task));
    
    if (current_task == task) {
        schedule();
    }
}

bool validate_task_memory(uint32_t task_id) {
    if (task_id >= MAX_TASKS) {
        return false;
    }
    
    Task* task = &tasks[task_id];
    
    if (!task->is_active) {
        return false;
    }
    
    if (!task->kesp || !task->kesp_bottom) {
        return false;
    }
    
    if (task->kesp >= task->kesp_bottom) {
        return false;
    }
    
    if ((task->kesp_bottom - task->kesp) < sizeof(NewTaskKernelStack)) {
        return false;
    }
    
    return true;
}

uint32_t get_current_pid() {
    return current_task ? current_task->pid : 0;
}


// ----- Helper Functions -----

void print_string_at(const char* str, int x, int y, uint8_t color) {
    uint16_t* vga = VGA_MEMORY + (y * 80 + x);
    uint16_t attribute = color << 8;
    
    while (*str && x < 80) {
        *vga++ = attribute | *str++;
        x++;
    }
}

void print_hex_at(uint32_t value, int x, int y, uint8_t color) {
    uint16_t* vga = VGA_MEMORY + (y * 80 + x);
    uint16_t attribute = color << 8;
    
    char hex_chars[] = "0123456789ABCDEF";
    
    for (int i = 7; i >= 0; i--) {
        uint8_t nibble = (value >> (i * 4)) & 0xF;
        *vga++ = attribute | hex_chars[nibble];
    }
}

// ----- Linux-Style Process Tasks -----

// Task 1: CPU Usage Monitor
void cpu_monitor_task(uint32_t pid) {
    uint32_t iterations = 0;
    
    print_string_at("CPU Monitor started [PID:", 0, 5, 0x0A);
    print_hex_at(pid, 26, 5, 0x0A);
    print_string_at("]", 34, 5, 0x0A);
    
    while (1) {
        iterations++;
        
        // Update display every iteration
        print_string_at("CPU [", 0, 5, 0x0A);
        print_hex_at(pid, 5, 5, 0x0A);
        print_string_at("] Iter:", 13, 5, 0x0A);
        print_hex_at(iterations, 21, 5, 0x0A);
        
        // Do some work
        volatile uint32_t work = 0;
        for (int i = 0; i < 50000; i++) {
            work += i;
        }
        
        // Yield to other tasks
        
    }
}

// Task 2: Memory Monitor
void mem_monitor_task(uint32_t pid) {
    uint32_t iterations = 0;
    uint32_t mem_usage = 0x500000;
    
    print_string_at("MEM Monitor started [PID:", 0, 7, 0x0B);
    print_hex_at(pid, 26, 7, 0x0B);
    print_string_at("]", 34, 7, 0x0B);
    
    while (1) {
        iterations++;
        
        if (iterations % 2 == 0) {
            mem_usage += 0x1000;
        }
        
        print_string_at("MEM [", 0, 7, 0x0B);
        print_hex_at(pid, 5, 7, 0x0B);
        print_string_at("] Usage:", 13, 7, 0x0B);
        print_hex_at(mem_usage, 22, 7, 0x0B);
        
        volatile uint32_t work = 0;
        for (int i = 0; i < 30000; i++) {
            work += i;
        }
        
        
    }
}

// Task 3: Network Monitor
void net_monitor_task(uint32_t pid) {
    uint32_t iterations = 0;
    uint32_t packets = 0;
    
    print_string_at("NET Monitor started [PID:", 0, 9, 0x0D);
    print_hex_at(pid, 26, 9, 0x0D);
    print_string_at("]", 34, 9, 0x0D);
    
    while (1) {
        iterations++;
        
        if (iterations % 3 == 0) {
            packets++;
        }
        
        print_string_at("NET [", 0, 9, 0x0D);
        print_hex_at(pid, 5, 9, 0x0D);
        print_string_at("] Pkts:", 13, 9, 0x0D);
        print_hex_at(packets, 21, 9, 0x0D);
        
        volatile uint32_t work = 0;
        for (int i = 0; i < 40000; i++) {
            work += i;
        }
        
        
    }
}

// Task 4: Disk I/O Monitor
void disk_monitor_task(uint32_t pid) {
    uint32_t iterations = 0;
    uint32_t io_ops = 0;
    
    print_string_at("DISK Monitor started [PID:", 0, 11, 0x0E);
    print_hex_at(pid, 27, 11, 0x0E);
    print_string_at("]", 35, 11, 0x0E);
    
    while (1) {
        iterations++;
        
        if (iterations % 4 == 0) {
            io_ops++;
        }
        
        print_string_at("DISK[", 0, 11, 0x0E);
        print_hex_at(pid, 5, 11, 0x0E);
        print_string_at("] I/O:", 13, 11, 0x0E);
        print_hex_at(io_ops, 20, 11, 0x0E);
        
        volatile uint32_t work = 0;
        for (int i = 0; i < 60000; i++) {
            work += i;
        }
        
        
    }
}


void start_async_monitors() {
    // Clear screen
    for (int i = 0; i < 80 * 25; i++) {
        VGA_MEMORY[i] = 0x0700 | ' ';
    }
    
    init_event_system();
    
    // Header
    print_string_at("=== RadiumOS Linux-Style Multitasking System ===", 0, 0, 0x1F);
    print_string_at("Cooperative Multitasking with Priority Scheduling", 0, 2, 0x07);
    
    // Create tasks
    int pid1 = create_task((uint32_t)cpu_monitor_task, 0, 0, false, 200);
    int pid2 = create_task((uint32_t)mem_monitor_task, 0, 0, false, 180);
    int pid3 = create_task((uint32_t)net_monitor_task, 0, 0, false, 160);
    int pid4 = create_task((uint32_t)disk_monitor_task, 0, 0, false, 140);
    
    
    // Show created tasks
    print_string_at("Created processes:", 0, 22, 0x0E);
    print_string_at("CPU:", 0, 23, 0x0A);
    print_hex_at(pid1, 5, 23, 0x0A);
    print_string_at(" MEM:", 14, 23, 0x0B);
    print_hex_at(pid2, 20, 23, 0x0B);
    print_string_at(" NET:", 29, 23, 0x0D);
    print_hex_at(pid3, 35, 23, 0x0D);
    print_string_at(" DISK:", 44, 23, 0x0E);
    print_hex_at(pid4, 51, 23, 0x0E);
    
    
    print_string_at("System running - watch tasks cooperate...", 0, 24, 0x0C);
}

void runme() {
    // Clear screen with a pattern to verify VGA works
    for (int i = 0; i < 80 * 25; i++) {
        VGA_MEMORY[i] = 0x0700 | ' ';
    }
    
    // Test VGA immediately
    VGA_MEMORY[0] = 0x0F00 | 'A';
    VGA_MEMORY[1] = 0x0F00 | 'B';
    VGA_MEMORY[2] = 0x0F00 | 'C';
    
    print_string_at("=== RadiumOS Debug Boot ===", 0, 0, 0x1F);
    
    // GDT Setup
    print_string_at("Setting up GDT...", 0, 2, 0x07);
    if (!setup_gdt()) {
        print_string_at("GDT FAILED!", 20, 2, 0x0C);
        while(1) asm("hlt");
    }
    print_string_at("OK", 20, 2, 0x0A);
    
    // IDT Setup
    print_string_at("Setting up IDT...", 0, 3, 0x07);
    if (!setup_interrupts()) {
        print_string_at("IDT FAILED!", 20, 3, 0x0C);
        while(1) asm("hlt");
    }
    print_string_at("OK", 20, 3, 0x0A);
    
    // Task Setup
    print_string_at("Setting up Tasks...", 0, 4, 0x07);
    if (!setup_tasks()) {
        print_string_at("TASKS FAILED!", 20, 4, 0x0C);
        while(1) asm("hlt");
    }
    print_string_at("OK", 20, 4, 0x0A);
    print_string_at("Current PID:", 0, 5, 0x07);
    print_hex_at(get_current_pid(), 14, 5, 0x0E);
    
    // PIT Setup
    print_string_at("Setting up PIT (100Hz)...", 0, 6, 0x07);
    if (!setup_pit(100)) {
        print_string_at("PIT FAILED!", 30, 6, 0x0C);
        while(1) asm("hlt");
    }
    print_string_at("OK", 30, 6, 0x0A);
    
    // Event system
    print_string_at("Init event system...", 0, 7, 0x07);
    init_event_system();
    print_string_at("OK", 30, 7, 0x0A);
    
    // Create first task
    print_string_at("Creating Task 1...", 0, 9, 0x0E);
    int pid1 = create_task((uint32_t)cpu_monitor_task, 0, 0, false, 20);
    print_string_at("PID:", 20, 9, 0x0E);
    print_hex_at(pid1, 25, 9, 0x0A);
    
    if (pid1 < 0) {
        print_string_at("FAILED TO CREATE TASK!", 35, 9, 0x0C);
        while(1) asm("hlt");
    }
    
    // Create second task
    print_string_at("Creating Task 2...", 0, 10, 0x0E);
    int pid2 = create_task((uint32_t)mem_monitor_task, 0, 0, false, 20);
    print_string_at("PID:", 20, 10, 0x0E);
    print_hex_at(pid2, 25, 10, 0x0B);
    int pid3 = create_task((uint32_t)keyboard_read_input, 0, 0, false, 20);
    print_string_at("PID:", 20, 10, 0x0E);
    print_hex_at(pid2, 65, 10, 0x0D);
    
    // Show task count
    print_string_at("Total tasks:", 0, 12, 0x0F);
    print_hex_at(num_tasks, 14, 12, 0x0F);
    
    // Test interrupt before enabling
    print_string_at("Testing int handling (before enable)...", 0, 14, 0x07);
    
    // Enable interrupts
    print_string_at("Enabling interrupts NOW...", 0, 15, 0x0C);
    enable_interrupts();
    print_string_at("Interrupts ENABLED", 0, 16, 0x0A);
    
    // Show where timer tick indicator will be
    print_string_at("Watch top-right corner for timer ->", 40, 0, 0x0E);
    VGA_MEMORY[79] = 0x4E00 | '?';
    
    print_string_at("Entering idle loop...", 0, 18, 0x0F);
    
    // Idle loop with counter
    uint32_t idle_counter = 0;
    while (1) {
        idle_counter++;
        
        // Show we're alive
        if (idle_counter % 1000000 == 0) {
            VGA_MEMORY[78] = 0x0E00 | 'K';
            print_hex_at(idle_counter / 1000000, 0, 20, 0x0E);
        }
        
        asm volatile("hlt");
    }
}