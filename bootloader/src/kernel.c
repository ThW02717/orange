
#include <stdint.h>
#include "uart.h" 
#include "shell.h"

static volatile unsigned long bootstrap_hart = ~0UL;

void secondary_main(unsigned long hartid) {
    (void)hartid;
    while (1) {
        asm volatile("wfi");
    }
}

void kernel_main(unsigned long hartid, unsigned long dtb_addr) {
    if (bootstrap_hart == ~0UL) {
        bootstrap_hart = hartid;
    }

    if (hartid != bootstrap_hart) {
        while (1) {
            asm volatile("wfi");
        }        
    }

    /* Start every bootloader session from a clean UART console state. */
    uart_init();
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
        while (1) {
            runAShell(pid++);
        }
    }

    while (1) {
        asm volatile("wfi");
    }
}
