/* Entry point. The core loads this program to the start of SDRAM and the
 * SoC's boot ROM jumps here. */
.section .text.start
.global _start
_start:
    la sp, _stack_top
    /* .data: its first values, from where the program was loaded to where it runs */
    la a0, _data_load
    la a1, _data_start
    la a2, _data_end
0:  bgeu a1, a2, 1f
    lw t0, 0(a0)
    sw t0, 0(a1)
    addi a0, a0, 4
    addi a1, a1, 4
    j 0b
1:  la a0, _bss_start
    la a1, _bss_end
2:  bgeu a0, a1, 3f
    sw zero, 0(a0)
    addi a0, a0, 4
    j 2b
3:  /* enable the FPU (mstatus.FS = initial) */
    li t0, 0x2000
    csrs mstatus, t0
    call main
4:  j 4b
