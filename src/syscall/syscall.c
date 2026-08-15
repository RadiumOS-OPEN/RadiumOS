// syscall.c - System call handler for RadiumOS kernel
#include "../terminal/terminal.h"
#include "../utility/utility.h"

#define SYS_EXIT    1
#define SYS_WRITE   2
#define SYS_READ    3
#define SYS_OPEN    4
#define SYS_CLOSE   5

// IDT entry structure
struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed));

// External IDT pointer (assumed to be defined elsewhere in kernel)
extern struct idt_entry idt[256];

// Set an IDT gate
static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].selector = sel;
    idt[num].zero = 0;
    idt[num].flags = flags;
}

// System call handler
void syscall_handler(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx) {
    switch (eax) {
        case SYS_EXIT:
            // Exit program - for now just return
            // In real OS, would terminate process
            print("\n[Program exited with code: ");
            print_decimal(ebx);
            print("]\n");
            break;
            
        case SYS_WRITE:
            // Write to file descriptor
            if (ebx == 1) { // stdout
                const char* buf = (const char*)ecx;
                for (int i = 0; i < (int)edx; i++) {
                    terminal_putchar(buf[i]);
                }
            }
            break;
            
        case SYS_READ:
            // Read from file descriptor - not implemented yet
            break;
            
        default:
            print("[Unknown syscall: ");
            print_decimal(eax);
            print("]\n");
            break;
    }
}

// Assembly wrapper for syscall handler
__attribute__((naked)) void syscall_entry() {
    asm volatile(
        "push %ebp\n"
        "push %edi\n"
        "push %esi\n"
        "push %edx\n"
        "push %ecx\n"
        "push %ebx\n"
        "push %eax\n"
        "push %edx\n"    // arg3
        "push %ecx\n"    // arg2
        "push %ebx\n"    // arg1
        "push %eax\n"    // syscall number
        "call syscall_handler\n"
        "add $16, %esp\n"
        "pop %eax\n"
        "pop %ebx\n"
        "pop %ecx\n"
        "pop %edx\n"
        "pop %esi\n"
        "pop %edi\n"
        "pop %ebp\n"
        "iret\n"
    );
}

// Initialize system call handler (call during kernel init)
void syscall_init(void) {
    // Install interrupt handler for INT 0x80
    idt_set_gate(0x80, (uint32_t)syscall_entry, 0x08, 0xEE);
    print("System calls initialized\n");
}