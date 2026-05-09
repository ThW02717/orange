
#include <stdint.h>
#include "uart.h" 
#include "shell.h"

/*
 * bootstrap_hart: tracks which hart is the "primary" core.
 * ~0UL (all bits 1) means "not yet chosen".
 * The first hart to reach kernel_main() claims this role;
 * all other harts sleep forever.
 */
static volatile unsigned long bootstrap_hart = ~0UL;

void secondary_main(unsigned long hartid) {
    (void)hartid;
    /* secondary harts just sleep forever */
    while (1) {
        asm volatile("wfi");           // wait for interrupt, low-power
    }
}

/*
 * kernel_main: entry point called from boot.S (_start).
 * Receives hartid and dtb_addr from OpenSBI via a0, a1.
 */
void kernel_main(unsigned long hartid, unsigned long dtb_addr) {
    /* first hart to arrive becomes the bootstrap hart */
    if (bootstrap_hart == ~0UL) {
        bootstrap_hart = hartid;
    }

    /* all other harts idle forever — only one core runs the shell */
    if (hartid != bootstrap_hart) {
        while (1) {
            asm volatile("wfi");
        }        
    }

    /* clean up UART hardware state (disable interrupts, flush FIFO) */
    uart_init();
    /* remember hartid and dtb_addr so we can pass them to the kernel later */
    shell_set_context(hartid, dtb_addr);

    uart_send_string("\nUART Bootloader ready\n");
    uart_send_string("load addr: ");
#ifdef QEMU
    uart_send_hex(0x82000000UL);
#else
    uart_send_hex(0x20000000UL);
#endif
    uart_send_string("\n");
    uart_send_string("Type 'help' for commands\n");

    {
        int32_t pid = 1;
        /* main loop: repeatedly run the shell prompt */
        while (1) {
            runAShell(pid++);
        }
    }

    while (1) {
        asm volatile("wfi");
    }
}
