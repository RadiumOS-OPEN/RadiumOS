; ----- Multiboot Header -----
section .multiboot
MB_MAGIC    equ 0x1BADB002
MB_FLAGS    equ 0
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)
dd MB_MAGIC
dd MB_FLAGS
dd MB_CHECKSUM

; ----- Initial Stack ----- FIXED!
section .bss
align 16
stack_bottom:           ; Low address
    resb 16384          ; 16 KiB stack space
stack_top:              ; High address - stack grows DOWN from here

align 4096
global bpd
bpd:
    resb 4096

global bptl
bptl:
    resb 4096

; ----- Boot -----
section .text
global _start
_start:
    cli                         ; Disable interrupts during boot
    mov esp, stack_top          ; FIXED: Point to TOP of stack
    

    
    ; Now call kernel
    mov ebx, [esp + 4]         ; multiboot info pointer
    push ebx
    extern kernel_main
    call kernel_main
    
    cli
.hang:
    hlt
    jmp .hang
; ----- Setup Paging -----
setup_paging:
    ; Step 1: Setup page table for first 4MB (kernel + stack)
    mov edi, bptl
    mov eax, 0x00000003         ; Present + RW
    mov ecx, 1024               ; 1024 entries * 4KB = 4MB
.fill_low_table:
    stosd
    add eax, 0x1000             ; Next 4KB page
    loop .fill_low_table
    
    ; Step 2: Clear page directory
    mov edi, bpd
    xor eax, eax
    mov ecx, 1024
    rep stosd
    
    ; Step 3: First PDE points to our page table
    mov dword [bpd], bptl
    or dword [bpd], 0x003    ; Present + RW
    
    ; Step 4: Map framebuffer 0xE0000000-0xF0000000 (256MB) using 4MB pages
    mov edi, bpd
    add edi, (0xE0000000 >> 22) * 4  ; edi = &page_directory[896]
    
    mov eax, 0xE0000000
    or eax, 0x83                ; Present + RW + 4MB page
    mov ecx, 64                 ; Map 64 * 4MB = 256MB
.map_fb:
    stosd
    add eax, 0x400000           ; Next 4MB
    loop .map_fb
    
    ; Step 5: Also map 0xF0000000-0xFF000000 (backup addresses)
    mov eax, 0xF0000000
    or eax, 0x83
    mov ecx, 60
.map_fb2:
    stosd
    add eax, 0x400000
    loop .map_fb2
    
    ; Step 6: Load page directory into CR3
    mov eax, bpd
    mov cr3, eax
    
    ; Step 7: Enable PSE (Page Size Extension) for 4MB pages
    mov eax, cr4
    or eax, 0x10                ; Set PSE bit
    mov cr4, eax
    
    ; Step 8: Enable paging
    mov eax, cr0
    or eax, 0x80000000          ; Set PG bit
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

global set_vga_mode13h
set_vga_mode13h:
    push eax
    push edx
    
    mov dx, 0x3C2
    mov al, 0x63
    out dx, al
    
    mov dx, 0x3C4
    mov al, 0x00
    out dx, al
    inc dx
    mov al, 0x01
    out dx, al
    
    pop edx
    pop eax
    ret