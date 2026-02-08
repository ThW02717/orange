#include <stdint.h>
#include "shell.h"
#include "uart.h"
#include "string.h"
#include "sbi.h"

static unsigned long g_hartid = 0;
static unsigned long g_dtb = 0;

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

    uart_send_string("core id: ");
    uart_send_dec(g_hartid);
    uart_send_string("\n");

    uart_send_string("dtb addr: ");
    uart_send_hex(g_dtb);
    uart_send_string("\n");

    uart_send_string("sbi spec: ");
    uart_send_hex(spec);
    uart_send_string("\n");

    uart_send_string("sbi impl id: ");
    uart_send_hex(impl_id);
    uart_send_string("\n");

    uart_send_string("sbi impl version: ");
    uart_send_hex(impl_ver);
    uart_send_string("\n");

    uart_send_string("mimpid: ");
    uart_send_hex(mimpid);
    uart_send_string("\n");
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
        uart_send_string("  reboot - Reboot board via SBI legacy call\n");
        return;
    }

    if (streq(shell->command, "hello")) {
        uart_send_string("Hello World!\n");
        return;
    }

    if (streq(shell->command, "info")) {
        print_info();
        return;
    }

    if (streq(shell->command, "reboot")) {
        long has_srst = sbi_probe_extension(0x53525354);
        if (has_srst <= 0) {
            uart_send_string("reboot not supported by current SBI, skip.\n");
            return;
        }

        uart_send_string("Rebooting...\n");
        sbi_system_reboot();
        uart_send_string("reboot request returned unexpectedly.\n");
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
