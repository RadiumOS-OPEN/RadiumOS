; ----- Multiboot Header -----
section .multiboot
MB_MAGIC    equ 0x1BADB002
MB_FLAGS    equ 0x3
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)
dd MB_MAGIC
dd MB_FLAGS
dd MB_CHECKSUM

section .bss
align 4096
global bpd
bpd:
    resb 4096

global bptl
bptl:
    resb 4096

align 4096
bpt1:
    resb 4096
bpt2:
    resb 4096
bpt3:
    resb 4096

; ----- Boot -----
section .text
global _start
extern _stack_top
_start:
    cli                         ; Disable interrupts during boot
    mov esi, ebx                ; Preserve Multiboot info pointer
    mov edi, eax                ; Preserve Multiboot boot magic
    mov esp, _stack_top          ; Use the stack reserved by linker.ld
    push esi                    ; kernel_main(magic, info) argument order
    push edi
    call setup_paging
    extern kernel_main
    call kernel_main
    
    cli
.hang:
    hlt
    jmp .hang
; ----- Enable PAE and identity-map the 32-bit physical address space -----
setup_paging:
    ; Identity-map physical memory below 3 GiB.
    mov edi, bptl
    xor ebx, ebx
    mov esi, 3
.fill_directory:
    mov ecx, 512
.fill_entry:
    mov eax, ebx
    or eax, 0x083               ; Present + RW + 2-MiB page
    mov [edi], eax
    mov dword [edi + 4], 0
    add ebx, 0x200000
    add edi, 8
    loop .fill_entry
    dec esi
    jnz .fill_directory

    ; Map RAM remapped at physical 4-5 GiB into virtual 3-4 GiB.
    mov edi, bpt3
    xor ebx, ebx
    mov ecx, 512
.fill_high_memory:
    mov eax, ebx
    or eax, 0x083
    mov [edi], eax
    mov dword [edi + 4], 1
    add ebx, 0x200000
    add edi, 8
    loop .fill_high_memory

    ; Keep QEMU's VBE framebuffer, PCI/APIC windows, and BIOS ROM reachable.
%macro map_identity_range 2
    mov edi, bpt3 + (((%1) - 0xC0000000) >> 21) * 8
    mov eax, %1
    mov ecx, %2
%%map:
    mov edx, eax
    or edx, 0x083
    mov [edi], edx
    mov dword [edi + 4], 0
    add eax, 0x200000
    add edi, 8
    loop %%map
%endmacro

    map_identity_range 0xFD000000, 8
    map_identity_range 0xFEA00000, 1
    map_identity_range 0xFEC00000, 2
    map_identity_range 0xFFE00000, 1
%unmacro map_identity_range 2

    ; The page-directory-pointer table references all 4 GiB of virtual space.
    mov edi, bpd
    mov eax, bptl
    or eax, 1
    mov [edi], eax
    mov dword [edi + 4], 0
    mov eax, bpt1
    or eax, 1
    mov [edi + 8], eax
    mov dword [edi + 12], 0
    mov eax, bpt2
    or eax, 1
    mov [edi + 16], eax
    mov dword [edi + 20], 0
    mov eax, bpt3
    or eax, 1
    mov [edi + 24], eax
    mov dword [edi + 28], 0

    ; Enable PAE and 2-MiB pages before loading the PDPT in CR3.
    mov eax, cr4
    or eax, 0x30
    mov cr4, eax
    mov eax, bpd
    mov cr3, eax

    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    ret

; ----- GDT -----
global load_gdt
load_gdt:
    mov eax, [esp + 4]
    lgdt [eax]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.load_cs
.load_cs:
    ret

; ----- Interrupts -----
%macro ISR_NO_ERROR_CODE 1
global isr%1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro

%macro ISR_ERROR_CODE 1
global isr%1
isr%1:
    push dword %1
    jmp isr_common
%endmacro

isr_common:
    pushad
    push ds
    push es
    push fs
    push gs
    push ebx
    mov bx, 0x10
    mov ds, bx
    mov es, bx
    mov fs, bx
    mov gs, bx
    pop ebx
    extern handle_interrupt
    call handle_interrupt
    pop gs
    pop fs
    pop es
    pop ds
    popad
    add esp, 8
    iret

ISR_NO_ERROR_CODE 0
ISR_NO_ERROR_CODE 1
ISR_NO_ERROR_CODE 2
ISR_NO_ERROR_CODE 3
ISR_NO_ERROR_CODE 4
ISR_NO_ERROR_CODE 5
ISR_NO_ERROR_CODE 6
ISR_NO_ERROR_CODE 7
ISR_ERROR_CODE    8
ISR_NO_ERROR_CODE 9
ISR_ERROR_CODE    10
ISR_ERROR_CODE    11
ISR_ERROR_CODE    12
ISR_ERROR_CODE    13
ISR_ERROR_CODE    14
ISR_NO_ERROR_CODE 15
ISR_NO_ERROR_CODE 16
ISR_NO_ERROR_CODE 17
ISR_NO_ERROR_CODE 18
ISR_NO_ERROR_CODE 19
ISR_NO_ERROR_CODE 20
ISR_NO_ERROR_CODE 21
ISR_NO_ERROR_CODE 22
ISR_NO_ERROR_CODE 23
ISR_NO_ERROR_CODE 24
ISR_NO_ERROR_CODE 25
ISR_NO_ERROR_CODE 26
ISR_NO_ERROR_CODE 27
ISR_NO_ERROR_CODE 28
ISR_NO_ERROR_CODE 29
ISR_NO_ERROR_CODE 30
ISR_NO_ERROR_CODE 31

ISR_NO_ERROR_CODE 32
ISR_NO_ERROR_CODE 33
ISR_NO_ERROR_CODE 34
ISR_NO_ERROR_CODE 35
ISR_NO_ERROR_CODE 36
ISR_NO_ERROR_CODE 37
ISR_NO_ERROR_CODE 38
ISR_NO_ERROR_CODE 39
ISR_NO_ERROR_CODE 40
ISR_NO_ERROR_CODE 41
ISR_NO_ERROR_CODE 42
ISR_NO_ERROR_CODE 43
ISR_NO_ERROR_CODE 44
ISR_NO_ERROR_CODE 45
ISR_NO_ERROR_CODE 46
ISR_NO_ERROR_CODE 47

ISR_NO_ERROR_CODE 128
global isr128

global isr_redirect_table
isr_redirect_table:
%assign i 0
%rep 48
dd isr%+i
%assign i i+1
%endrep

; ----- Tasks -----
global switch_context

TASK_ID_OFFSET          equ 0
TASK_PID_OFFSET         equ 4
TASK_KESP_OFFSET        equ 8
TASK_KESP_BOTTOM_OFFSET equ 12

switch_context:
    mov eax, [esp + 4]
    mov edx, [esp + 8]
    
    push ebx
    push esi
    push edi
    push ebp
    
    mov [eax + TASK_KESP_OFFSET], esp
    mov esp, [edx + TASK_KESP_OFFSET]
    
    pop ebp
    pop edi
    pop esi
    pop ebx
    
    ret

global new_task_setup
new_task_setup:
    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    iret

extern vga_set_mode13h
global set_vga_mode13h
set_vga_mode13h:
    jmp vga_set_mode13h
