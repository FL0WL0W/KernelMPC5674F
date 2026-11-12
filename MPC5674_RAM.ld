ENTRY(_start)

__HEAP_SIZE = 32K;
__STACK_SIZE = 32K;

MEMORY
{
    /* Preserve the stock loader's entry address and existing RAM allocation. */
    SRAM_CODE (rwx) : ORIGIN = 0x40010000, LENGTH = 128K
    SRAM      (rw)  : ORIGIN = 0x40000000, LENGTH = 64K
}

SECTIONS
{
    .startup : ALIGN(4)
    {
        KEEP(*(.startup))
    } > SRAM_CODE

    .text : ALIGN(4)
    {
        *(.text .text.* .text_booke)
    } > SRAM_CODE

    .preinit_array : ALIGN(4)
    {
        __preinit_array_start = .;
        KEEP(*(.preinit_array .preinit_array.*))
        __preinit_array_end = .;
    } > SRAM_CODE

    .init_array : ALIGN(4)
    {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } > SRAM_CODE

    .rodata : ALIGN(4)
    {
        *(.rodata .rodata.* .sdata2 .sdata2.*)
    } > SRAM_CODE

    .data : ALIGN(4)
    {
        *(.data .data.* .sdata .sdata.*)
    } > SRAM_CODE

    .bss (NOLOAD) : ALIGN(16)
    {
        __BSS_START = .;
        *(.bss .bss.* .sbss .sbss.* .sbss2 .sbss2.*)
        *(COMMON)
        __BSS_END = .;
    } > SRAM_CODE
    __BSS_SIZE = __BSS_END - __BSS_START;

    .heap (NOLOAD) : ALIGN(16)
    {
        __HEAP = .;
        . += __HEAP_SIZE;
        __HEAP_END = .;
    } > SRAM

    .stack ORIGIN(SRAM) + LENGTH(SRAM) - __STACK_SIZE (NOLOAD) :
    {
        _stack_end = .;
        . += __STACK_SIZE;
        __SP_INIT = .;
    } > SRAM

    /* Startup runs constructors explicitly; the kernel never tears down. */
    /DISCARD/ :
    {
        *(.init .init.* .fini .fini.* .fini_array .fini_array.*)
        *(.eh_frame .eh_frame.* .eh_frame_hdr)
        *(.comment .note.GNU-stack)
    }
}

ASSERT(__HEAP_END <= _stack_end, "Heap overlaps stack")
ASSERT(_start == ORIGIN(SRAM_CODE), "Entry point must match the RAM loader address")
