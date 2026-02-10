#include <stdint.h>
#include "shell.h"
#include "uart.h"
#include "string.h"
#include "sbi.h"

static unsigned long g_hartid = 0;
static unsigned long g_dtb = 0;
#define UART_BOOT_MAGIC 0x544F4F42UL /* "BOOT" */
#define UART_BOOT_MAX_SIZE (16UL * 1024UL * 1024UL)
#ifdef QEMU
#define UART_BOOT_LOAD_ADDR 0x82000000UL
#else
#define UART_BOOT_LOAD_ADDR 0x20000000UL
#endif

void shell_set_context(unsigned long hartid, unsigned long dtb_addr) {
    g_hartid = hartid;
    g_dtb = dtb_addr;
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static unsigned int str_len(const char *s) {
    unsigned int n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int streq(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static void trim_in_place(char *s) {
    char *p = s;
    while (is_space(*p)) {
        p++;
    }
    if (p != s) {
        char *dst = s;
        while (*p != '\0') {
            *dst++ = *p++;
        }
        *dst = '\0';
    }

    unsigned int len = str_len(s);
    while (len > 0 && is_space(s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
}

static void getCommand(char* buffer, int max_len) {
    int idx = 0;
    int ignore_next_lf = 0;

    while (1) {
        char c = uart_recv();

        if (ignore_next_lf) {
            if (c == '\n') {
                ignore_next_lf = 0;
                continue;
            }
            ignore_next_lf = 0;
        }

        if (c == '\r' || c == '\n') {
            uart_send_string("\r\n");
            if (c == '\r') {
                ignore_next_lf = 1;
            }
            buffer[idx] = '\0';
            return;
        }

        if (c == KEY_BACKSPACE || c == '\b' || c == 127) {
            if (idx > 0) {
                idx--;
                uart_send_string("\b \b");
            }
            continue;
        }

        if (c >= ' ' && c <= '~') {
            if (idx < max_len - 1) {
                buffer[idx++] = c;
                uart_send(c);
            }
        }
    }
}

static void print_info(void) {
    unsigned long spec = (unsigned long)sbi_get_spec_version();
    unsigned long impl_id = (unsigned long)sbi_get_impl_id();
    unsigned long impl_ver = (unsigned long)sbi_get_impl_version();
    unsigned long mimpid = (unsigned long)sbi_get_mimpid();
    unsigned long spec_major = (spec >> 24) & 0x7fUL;
    unsigned long spec_minor = spec & 0x00ffffffUL;
    unsigned long impl_major = (impl_ver >> 16) & 0xffUL;
    unsigned long impl_minor = (impl_ver >> 8) & 0xffUL;
    unsigned long impl_patch = impl_ver & 0xffUL;

    uart_send_string("core id: ");
    uart_send_dec(g_hartid);
    uart_send_string("\n");

    uart_send_string("dtb addr: ");
    uart_send_hex(g_dtb);
    uart_send_string("\n");

    uart_send_string("sbi spec: ");
    uart_send_hex(spec);
    uart_send_string(" (");
    uart_send_dec(spec_major);
    uart_send_string(".");
    uart_send_dec(spec_minor);
    uart_send_string(")");
    uart_send_string("\n");

    uart_send_string("sbi impl id: ");
    uart_send_hex(impl_id);
    uart_send_string("\n");

    uart_send_string("sbi impl version: ");
    uart_send_hex(impl_ver);
    uart_send_string(" (");
    uart_send_dec(impl_major);
    uart_send_string(".");
    uart_send_dec(impl_minor);
    uart_send_string(".");
    uart_send_dec(impl_patch);
    uart_send_string(", decoded)");
    uart_send_string("\n");

    uart_send_string("mimpid: ");
    uart_send_hex(mimpid);
    uart_send_string("\n");
}

static const char* hsm_state_name(long st) {
    switch (st) {
        case SBI_HSM_STATE_STARTED: return "started";
        case SBI_HSM_STATE_STOPPED: return "stopped";
        case SBI_HSM_STATE_START_PENDING: return "start_pending";
        case SBI_HSM_STATE_STOP_PENDING: return "stop_pending";
        case SBI_HSM_STATE_SUSPENDED: return "suspended";
        case SBI_HSM_STATE_SUSPEND_PENDING: return "suspend_pending";
        case SBI_HSM_STATE_RESUME_PENDING: return "resume_pending";
        default: return "unknown";
    }
}

static void print_cores(void) {
    unsigned long core;
    long hsm = sbi_probe_extension(SBI_EXT_HSM);
    uart_send_string("sbi hsm support: ");
    uart_send_hex((unsigned long)hsm);
    uart_send_string("\n");

    for (core = 0; core < 8; core++) {
        long st = sbi_hart_get_status(core);
        uart_send_string("core ");
        uart_send_dec(core);
        uart_send_string(": ");
        if (st < 0) {
            uart_send_string("err=");
            uart_send_hex((unsigned long)st);
        } else {
            uart_send_string(hsm_state_name(st));
            uart_send_string(" (");
            uart_send_hex((unsigned long)st);
            uart_send_string(")");
        }
        uart_send_string("\n");
    }
}

static unsigned long recv_u32_le(void) {
    unsigned long v = 0;
    v |= (unsigned long)(uint8_t)uart_recv();
    v |= (unsigned long)(uint8_t)uart_recv() << 8;
    v |= (unsigned long)(uint8_t)uart_recv() << 16;
    v |= (unsigned long)(uint8_t)uart_recv() << 24;
    return v;
}

static void recv_bytes(void *dst, unsigned long size) {
    unsigned char *p = (unsigned char *)dst;
    unsigned long i;
    for (i = 0; i < size; i++) {
        p[i] = (unsigned char)uart_recv();
    }
}

static unsigned long checksum_bytes(const uint8_t *data, unsigned long size) {
    unsigned long sum = 0;
    unsigned long i;

    for (i = 0; i < size; i++) {
        sum = (sum + (unsigned long)data[i]) & 0xffffffffUL;
    }
    return sum;
}

static void boot_load_and_jump(void) {
    unsigned long magic;
    unsigned long size;
    unsigned long expected_sum;
    unsigned long actual_sum;
    uint8_t *dst = (uint8_t *)UART_BOOT_LOAD_ADDR;
    void (*next_kernel)(unsigned long, unsigned long);

    uart_send_string("UART loader ready\n");
    uart_send_string("send header <magic,size,checksum> then payload\n");

    magic = recv_u32_le();
    size = recv_u32_le();
    expected_sum = recv_u32_le();

    if (magic != UART_BOOT_MAGIC) {
        uart_send_string("load fail: bad magic ");
        uart_send_hex(magic);
        uart_send_string("\n");
        return;
    }

    if (size == 0 || size > UART_BOOT_MAX_SIZE) {
        uart_send_string("load fail: bad size ");
        uart_send_dec(size);
        uart_send_string("\n");
        return;
    }

    uart_send_string("loading ");
    uart_send_dec(size);
    uart_send_string(" bytes to ");
    uart_send_hex(UART_BOOT_LOAD_ADDR);
    uart_send_string("\n");

    recv_bytes(dst, size);

    actual_sum = checksum_bytes(dst, size);
    if (actual_sum != expected_sum) {
        uart_send_string("load fail: checksum exp=");
        uart_send_hex(expected_sum);
        uart_send_string(" got=");
        uart_send_hex(actual_sum);
        uart_send_string("\n");
        return;
    }

    uart_send_string("load ok, jump ");
    uart_send_hex(UART_BOOT_LOAD_ADDR);
    uart_send_string("\n");

    asm volatile("fence rw, rw" ::: "memory");
    asm volatile(".word 0x0000100f" ::: "memory");

    next_kernel = (void (*)(unsigned long, unsigned long))UART_BOOT_LOAD_ADDR;
    next_kernel(g_hartid, g_dtb);
    uart_send_string("loaded image returned\n");
}

void processCommand(shell_t* shell) {
    if (shell->command[0] == '\0') {
        return;
    }

    if (streq(shell->command, "help")) {
        uart_send_string("Available commands:\n");
        uart_send_string("  help   - Show this message\n");
        uart_send_string("  hello  - Print Hello World!\n");
        uart_send_string("  info   - Print core and SBI info\n");
        uart_send_string("  cores  - Show HSM status for core 0..7\n");
        uart_send_string("  load   - Load kernel via UART and jump\n");
        return;
    }

    if (streq(shell->command, "hello")) {
        uart_send_string("Hello World!\n");
        return;
    }

    if (streq(shell->command, "load")) {
        boot_load_and_jump();
        return;
    }

    if (streq(shell->command, "info")) {
        print_info();
        return;
    }

    if (streq(shell->command, "cores")) {
        print_cores();
        return;
    }

    uart_send_string("Unknown command: ");
    uart_send_string(shell->command);
    uart_send_string("\n");


}

void runAShell(int32_t pid) {
    char* prompt_start = "> ";
    shell_t shell; 
    char command_buffer[128]; 
    shell.pid = pid;
    uart_send_string(prompt_start);

    getCommand(command_buffer, 128);
    trim_in_place(command_buffer);

    shell.command = command_buffer; 

    processCommand(&shell); 
}
