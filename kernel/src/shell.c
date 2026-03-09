#include <stdint.h>
#include "shell.h"
#include "uart.h"
#include "string.h"
#include "sbi.h"
#include "fdt.h"
#include "cpio.h"
#include "memory.h"

static unsigned long g_hartid = 0;
static unsigned long g_dtb = 0;
static uint64_t g_initrd_start = 0;
static uint64_t g_initrd_end = 0;
static uint64_t g_initrd_hint_start = 0;
static uint64_t g_initrd_hint_end = 0;

static int initrd_hint_valid(uint64_t start, uint64_t end) {
    if (start == 0 || end <= start) {
        return 0;
    }
    if ((end - start) > (256UL * 1024UL * 1024UL)) {
        return 0;
    }
    return 1;
}

void shell_set_context(unsigned long hartid, unsigned long dtb_addr,
                       uint64_t initrd_start_hint, uint64_t initrd_end_hint) {
    g_hartid = hartid;
    g_dtb = dtb_addr;
    g_initrd_hint_start = initrd_start_hint;
    g_initrd_hint_end = initrd_end_hint;
    g_initrd_start = 0;
    g_initrd_end = 0;
    if (g_dtb != 0) {
        if (fdt_get_initrd_range((const void *)g_dtb, &g_initrd_start, &g_initrd_end) != 0) {
            g_initrd_start = 0;
            g_initrd_end = 0;
        }
    }
    if ((g_initrd_start == 0 || g_initrd_end <= g_initrd_start) &&
        initrd_hint_valid(g_initrd_hint_start, g_initrd_hint_end)) {
        g_initrd_start = g_initrd_hint_start;
        g_initrd_end = g_initrd_hint_end;
    }
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
    struct fdt_mem_region mem_regions[FDT_MAX_MEM_REGIONS];
    uint64_t mem_total = 0;
    int mem_count = 0;
    int i;

    if (g_dtb != 0) {
        mem_count = fdt_get_memory_regions((const void *)g_dtb, mem_regions, FDT_MAX_MEM_REGIONS);
        if (mem_count < 0) {
            mem_count = 0;
        }
        for (i = 0; i < mem_count; i++) {
            mem_total += mem_regions[i].size;
        }
    }

    uart_send_string("core id: ");
    uart_send_dec(g_hartid);
    uart_send_string("\n");

    uart_send_string("dtb addr: ");
    uart_send_hex(g_dtb);
    uart_send_string("\n");

    uart_send_string("initrd: ");
    if (g_initrd_start != 0 && g_initrd_end > g_initrd_start) {
        uart_send_string("start=");
        uart_send_hex((unsigned long)g_initrd_start);
        uart_send_string(" end=");
        uart_send_hex((unsigned long)g_initrd_end);
        uart_send_string(" size=");
        uart_send_hex((unsigned long)(g_initrd_end - g_initrd_start));
    } else {
        uart_send_string("none");
    }
    uart_send_string("\n");

    uart_send_string("memory: ");
    if (mem_count > 0) {
        uart_send_string("regions=");
        uart_send_dec((unsigned long)mem_count);
        uart_send_string(" total=");
        uart_send_hex((unsigned long)mem_total);
    } else {
        uart_send_string("unknown");
    }
    uart_send_string("\n");
    for (i = 0; i < mem_count; i++) {
        uart_send_string("memory[");
        uart_send_dec((unsigned long)i);
        uart_send_string("]: base=");
        uart_send_hex((unsigned long)mem_regions[i].base);
        uart_send_string(" size=");
        uart_send_hex((unsigned long)mem_regions[i].size);
        uart_send_string("\n");
    }

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

static void split_first_arg(char *line, char **cmd, char **arg) {
    char *p = line;
    while (is_space(*p)) {
        p++;
    }
    *cmd = p;
    while (*p != '\0' && !is_space(*p)) {
        p++;
    }
    if (*p != '\0') {
        *p++ = '\0';
    }
    while (is_space(*p)) {
        p++;
    }
    *arg = (*p == '\0') ? 0 : p;
}

static void uart_send_bytes(const uint8_t *data, unsigned long size) {
    unsigned long i;
    for (i = 0; i < size; i++) {
        char c = (char)data[i];
        if (c == '\n') {
            uart_send('\r');
        }
        uart_send(c);
    }
}

struct ls_ctx {
    unsigned int count;
};

static void ls_cb(const char *name, const void *data, unsigned long size,
                  unsigned int mode, void *ctx) {
    struct ls_ctx *st = (struct ls_ctx *)ctx;
    unsigned int type = mode & 0170000U;
    const char *print_name = name;
    (void)data;

    if (name[0] == '.' && name[1] == '/' && name[2] != '\0') {
        print_name = name + 2;
    }

    uart_send_dec(size);
    uart_send_string(" ");
    uart_send_string(print_name);
    if (type == 0040000U && !(print_name[0] == '.' && print_name[1] == '\0')) {
        uart_send_string("/");
    }
    uart_send_string("\n");
    st->count++;
}

static void cmd_ls(void) {
    struct ls_ctx st;
    st.count = 0;
    if (g_initrd_start == 0 || g_initrd_end <= g_initrd_start) {
        uart_send_string("initrd not available\n");
        return;
    }
    if (cpio_iterate((const void *)g_initrd_start, (const void *)g_initrd_end,
                     ls_cb, &st) != 0) {
        uart_send_string("cpio parse error\n");
        return;
    }
    uart_send_string("Total ");
    uart_send_dec(st.count);
    uart_send_string(" files.\n");
}

static void cmd_cat(const char *arg) {
    const void *data = 0;
    unsigned long size = 0;
    unsigned int mode = 0;
    char alt[128];

    if (g_initrd_start == 0 || g_initrd_end <= g_initrd_start) {
        uart_send_string("initrd not available\n");
        return;
    }
    if (arg == 0 || arg[0] == '\0') {
        uart_send_string("usage: cat <file>\n");
        return;
    }

    if (cpio_find((const void *)g_initrd_start, (const void *)g_initrd_end,
                  arg, &data, &size, &mode) != 0) {
        if (arg[0] != '.' || arg[1] != '/') {
            unsigned int i = 0;
            unsigned int j = 0;
            alt[i++] = '.';
            alt[i++] = '/';
            while (arg[j] != '\0' && i + 1 < sizeof(alt)) {
                alt[i++] = arg[j++];
            }
            alt[i] = '\0';
            if (cpio_find((const void *)g_initrd_start, (const void *)g_initrd_end,
                          alt, &data, &size, &mode) != 0) {
                uart_send_string("file not found\n");
                return;
            }
        } else {
            uart_send_string("file not found\n");
            return;
        }
    }

    if ((mode & 0170000U) == 0040000U) {
        uart_send_string("is a directory\n");
        return;
    }

    uart_send_bytes((const uint8_t *)data, size);
    if (size == 0 || ((const uint8_t *)data)[size - 1] != '\n') {
        uart_send_string("\n");
    }
}

static void cmd_allocdemo(void) {
    void *p1;
    void *p2;
    void *k1;
    void *k2;

    uart_send_string("allocator demo start\n");

    p1 = p_alloc(1);
    p2 = p_alloc(3);
    p_free(p1);
    p_free(p2);

    k1 = kmalloc(80);
    k2 = kmalloc(3000);
    kfree(k1);
    kfree(k2);

    uart_send_string("allocator demo done\n");
}

void processCommand(shell_t* shell) {
    char *cmd;
    char *arg;

    if (shell->command[0] == '\0') {
        return;
    }

    split_first_arg(shell->command, &cmd, &arg);

    if (streq(cmd, "help")) {
        uart_send_string("Available commands:\n");
        uart_send_string("  help   - Show this message\n");
        uart_send_string("  hello  - Print Hello World!\n");
        uart_send_string("  info   - Print core and SBI info\n");
        uart_send_string("  cores  - Show HSM status for core 0..7\n");
        uart_send_string("  ls     - List initrd files\n");
        uart_send_string("  cat    - Show initrd file content\n");
        uart_send_string("  allocdemo - Run allocator demo\n");
        return;
    }

    if (streq(cmd, "hello")) {
        uart_send_string("Hello World!\n");
        return;
    }

    if (streq(cmd, "info")) {
        print_info();
        return;
    }

    if (streq(cmd, "cores")) {
        print_cores();
        return;
    }

    if (streq(cmd, "ls")) {
        cmd_ls();
        return;
    }

    if (streq(cmd, "cat")) {
        cmd_cat(arg);
        return;
    }

    if (streq(cmd, "allocdemo")) {
        cmd_allocdemo();
        return;
    }

    uart_send_string("Unknown command: ");
    uart_send_string(cmd);
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
