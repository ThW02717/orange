
#include <stdint.h>
#include "uart.h" 
#include "shell.h"
#include "sbi.h"
#include "string.h"

#ifndef NUM_HARTS
#define NUM_HARTS 4
#endif

static volatile unsigned long bootstrap_hart = ~0UL;

void kernel_main(unsigned long hartid, unsigned long dtb_addr) {
    if (bootstrap_hart == ~0UL) {
        bootstrap_hart = hartid;
    }

    if (hartid != bootstrap_hart) {
        while (1) {
            asm volatile("wfi");
        }        
    }

    /* Keep bootloader UART state first to avoid serial regressions. */
    shell_set_context(hartid, dtb_addr);

    uart_send_string("Hello from orange ");
    uart_send_dec(hartid);
    uart_send_string("\n");

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
