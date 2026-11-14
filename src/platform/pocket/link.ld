/* Taiko Pocket firmware. 64 MB SDRAM at 0x4000_0000:
 *   0x4000_0000  program, data, bss, stack (30 MB)
 *   0x41E0_0000  two frame buffers (taiko_regs.h)
 *   0x4200_0000  music ring, 32 MB */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { ram : ORIGIN = 0x40000000, LENGTH = 0x01E00000 }
SECTIONS {
    .text : { KEEP(*(.text.start)) *(.text .text.*) } > ram
    .rodata : { . = ALIGN(8); *(.rodata .rodata.* .srodata .srodata.*) } > ram
    .data : { . = ALIGN(8); *(.data .data.*) *(.sdata .sdata.*) . = ALIGN(8); } > ram
    .bss (NOLOAD) : { _bss_start = .; *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON) . = ALIGN(8); _bss_end = .; } > ram
    _stack_top = ORIGIN(ram) + LENGTH(ram);
    ASSERT(_bss_end + 0x20000 < _stack_top, "program and its static memory do not fit below the frame buffers")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.riscv.attributes) }
}
