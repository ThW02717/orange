/* Board resources come from DT; drivers own their configured MMIO bases. */
#include "platform.h"
#include "fdt.h"
#include "uart.h"
#include "plic.h"

static struct boot_info g_boot_info;

static int platform_init_devices(const void *fdt)
{
    int uart_node;
    int plic_node;
    uint64_t uart_base;
    uint64_t plic_base;
    uint32_t irq;

    if (fdt == 0) {
        return -1;
    }

#ifdef QEMU
    uart_node = fdt_find_compatible(fdt, "ns16550a");
#else
    uart_node = fdt_find_compatible(fdt, "ky,pxa-uart");
#endif
    plic_node = fdt_find_compatible(fdt, "riscv,plic0");
    if (uart_node < 0 || plic_node < 0) {
        return -1;
    }
    if (fdt_get_reg_base(fdt, uart_node, &uart_base) != 0 ||
        fdt_get_interrupt_id(fdt, uart_node, &irq) != 0 ||
        fdt_get_reg_base(fdt, plic_node, &plic_base) != 0) {
        return -1;
    }

    uart_set_platform((uintptr_t)uart_base, irq);
    plic_set_base((uintptr_t)plic_base);
    return 0;
}

int platform_init(unsigned long hartid, uintptr_t dtb,
                  uint64_t initrd_hint_start, uint64_t initrd_hint_end)
{
    uint64_t start = 0;
    uint64_t end = 0;

    if (platform_init_devices((const void *)dtb) != 0) {
        return -1;
    }
    g_boot_info.hartid = hartid;
    g_boot_info.dtb = dtb;
    if (fdt_get_initrd_range((const void *)dtb, &start, &end) != 0 ||
        start == 0 || end <= start) {
        start = 0;
        end = 0;
        /* UART boot bundles may provide the range outside /chosen. */
        if (initrd_hint_start != 0 && initrd_hint_end > initrd_hint_start &&
            initrd_hint_end - initrd_hint_start <= 256UL * 1024UL * 1024UL) {
            start = initrd_hint_start;
            end = initrd_hint_end;
        }
    }
    g_boot_info.initrd_start = start;
    g_boot_info.initrd_end = end;
    return 0;
}

const struct boot_info *platform_boot_info(void)
{
    return &g_boot_info;
}
