/* Taiko Pocket firmware. 64 MB SDRAM at 0x4000_0000:
 *   0x4000_0000  the program as loaded: code, constants, the first values of .data (1 MB)
 *   0x4010_0000  data, bss, stack (29 MB)
 *   0x41E0_0000  two frame buffers (taiko_regs.h)
 *   0x4200_0000  music ring, 32 MB
 * .data runs at `work` and is copied there from the loaded program on every start
 * (crt0.S): a reset that does not load the program again must not leave variables at
 * the values they had when it came. */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY {
    image : ORIGIN = 0x40000000, LENGTH = 0x00100000
    work  : ORIGIN = 0x40100000, LENGTH = 0x01D00000
}
SECTIONS {
    .text : { KEEP(*(.text.start)) *(.text .text.*) } > image
    .rodata : { . = ALIGN(8); *(.rodata .rodata.* .srodata .srodata.*) . = ALIGN(8); } > image
    .data : { _data_start = .; *(.data .data.*) *(.sdata .sdata.*) . = ALIGN(8); _data_end = .; } > work AT > image
    _data_load = LOADADDR(.data);
    .bss (NOLOAD) : { _bss_start = .; *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON) . = ALIGN(8); _bss_end = .; } > work
    _stack_top = ORIGIN(work) + LENGTH(work);
    ASSERT(_bss_end + 0x20000 < _stack_top, "the program's static memory does not fit below the frame buffers")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.riscv.attributes) }
}
