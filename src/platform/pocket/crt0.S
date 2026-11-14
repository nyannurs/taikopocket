/* Entry point. The core loads this program to the start of SDRAM and the
 * SoC's boot ROM jumps here. */
.section .text.start
.global _start
_start:
    la sp, _stack_top
    la a0, _bss_start
    la a1, _bss_end
1:  bgeu a0, a1, 2f
    sw zero, 0(a0)
    addi a0, a0, 4
    j 1b
2:  /* enable the FPU (mstatus.FS = initial) */
    li t0, 0x2000
    csrs mstatus, t0
    call main
3:  j 3b
