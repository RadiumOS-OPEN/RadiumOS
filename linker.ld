ENTRY(_start)
SECTIONS
{
    . = 1M;

    .multiboot ALIGN(4K) : {
        *(.multiboot)
    }

    .text ALIGN(4K) : {
        *(.text)
        *(.text.*)
    }

    .rodata ALIGN(4K) : {
        *(.rodata)
        *(.rodata.*)
    }

    .data ALIGN(4K) : {
        *(.data)
        *(.data.*)
    }

    .bss ALIGN(4K) (NOLOAD) : {
        _bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
        . = ALIGN(4K);
        _bss_end = .;
    }

    .heap ALIGN(4K) (NOLOAD) : {
        _heap_start = .;
        . += 128M;
        _heap_end = .;
    }

    .stack ALIGN(4K) (NOLOAD) : {
        _stack_bottom = .;
        . += 8M;
        _stack_top = .;
    }

    .stack_guard ALIGN(4K) (NOLOAD) : {
        _stack_guard_start = .;
        . += 4K;
        _stack_guard_end = .;
    }

    _kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}