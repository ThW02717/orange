/* Lab 4 diagnostic image, reconstructed from the supplied 14-byte prog.bin:
 * https://github.com/nycu-caslab/OSC2026/blob/main/uploads/prog.bin
 * a7 counts ecalls here; these are not P1 syscall numbers. exec prog selects
 * the diagnostic handler. The final loop intentionally keeps U-mode alive.
 */
.section .text
.global _start
_start:
    li a7, 5
lab_ecall:
    ecall
    addi a7, a7, -1
    bne zero, a7, lab_ecall
lab_wait:
    j lab_wait
