
#include <stdint.h>
#include "uart.h" 
#include "shell.h"
#include "sbi.h"

static volatile unsigned long bootstrap_hart = ~0UL;
static volatile unsigned int greet_ticket = 0;
static volatile unsigned int greet_serving = 0;
static volatile unsigned int greeted_count = 0;
static volatile unsigned int started_cores = 1;

#define MAX_CORES 8
#define SECONDARY_STACK_SIZE 4096

static unsigned char secondary_stacks[MAX_CORES - 1][SECONDARY_STACK_SIZE] __attribute__((aligned(16)));

extern void secondary_start(void);

static void greet_once(unsigned long core_id) {
    unsigned int my_ticket = __sync_fetch_and_add(&greet_ticket, 1);
    while (greet_serving != my_ticket) {
        asm volatile("nop");
    }

    uart_send_string("Hello from core ");
    uart_send_dec(core_id);
    uart_send_string("\n");

    __sync_synchronize();
    greet_serving++;
    __sync_fetch_and_add(&greeted_count, 1);
}

void secondary_main(unsigned long hartid) {
    greet_once(hartid);
    while (1) {
        asm volatile("wfi");
    }
}

void kernel_main(unsigned long hartid, unsigned long dtb_addr) {
    unsigned int stack_slot = 0;
    unsigned long target_hart;

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

    for (target_hart = 0; target_hart < MAX_CORES; target_hart++) {
        long ret;
        unsigned long stack_top;

        if (target_hart == hartid) {
            continue;
        }
        if (stack_slot >= (MAX_CORES - 1)) {
            break;
        }

        stack_top = (unsigned long)&secondary_stacks[stack_slot][SECONDARY_STACK_SIZE];
        ret = sbi_hart_start(target_hart, (unsigned long)&secondary_start, stack_top);
        if (ret == SBI_SUCCESS) {
            started_cores++;
            stack_slot++;
        }
    }

    greet_once(hartid);

    {
        unsigned long wait = 50000000UL;
        while (greeted_count < started_cores && wait-- > 0) {
            asm volatile("nop");
        }
        if (greeted_count < started_cores) {
            uart_send_string("core greet timeout: ");
            uart_send_dec(greeted_count);
            uart_send_string("/");
            uart_send_dec(started_cores);
            uart_send_string("\n");
        }
    }

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
