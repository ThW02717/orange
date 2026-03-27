#include <stdint.h>
#include "shell.h"
#include "uart.h"
#include "string.h"
#include "sbi.h"
#include "fdt.h"
#include "cpio.h"
#include "memory.h"
#include "memory_test.h"
#include "demo.h"
#include "trap.h"
#include "timer.h"
#include "user.h"

/* Interactive UART shell with initramfs-backed file commands and user-program launch. */
/* Shell-global boot context and initramfs location discovered during startup. */
static unsigned long g_hartid = 0;
static unsigned long g_dtb = 0;
static uint64_t g_initrd_start = 0;
static uint64_t g_initrd_end = 0;
static uint64_t g_initrd_hint_start = 0;
static uint64_t g_initrd_hint_end = 0;

#define CPIO_MODE_TYPE_MASK 0170000U
#define CPIO_MODE_REGULAR   0100000U

/* Timer callbacks may print while the REPL is blocked in uart_recv(), so keep
 * the prompt string in one place and redraw it after asynchronous messages.
 */
static const char g_shell_prompt[] = "> ";

/* Validate fallback initrd hints before using them in place of DT-provided data. */
static int initrd_hint_valid(uint64_t start, uint64_t end) {
    if (start == 0 || end <= start) {
        return 0;
    }
    if ((end - start) > (256UL * 1024UL * 1024UL)) {
        return 0;
    }
    return 1;
}

/* Record shell-visible boot context and resolve the initramfs address range once. */
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

/* Small local helpers used by the shell parser and loader commands. */
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

static int parse_u64_dec(const char *s, uint64_t *out) {
    uint64_t value = 0;
    unsigned int i = 0;

    if (s == 0 || out == 0 || s[0] == '\0') {
        return -1;
    }

    while (s[i] != '\0') {
        char c = s[i];

        if (c < '0' || c > '9') {
            return -1;
        }
        value = (value * 10ULL) + (uint64_t)(c - '0');
        i++;
    }

    *out = value;
    return 0;
}

/* Parse decimal seconds into timer ticks using fixed-point arithmetic.
 * Accepts forms like "1", "1.5", and "0.125" without pulling in libgcc
 * floating-point helpers in the freestanding kernel build.
 */
static int parse_seconds_to_ticks(const char *s, uint64_t *ticks_out, uint64_t *whole_out)
{
    uint64_t whole = 0;
    uint64_t frac = 0;
    uint64_t frac_scale = 1;
    unsigned int i = 0;
    int seen_dot = 0;

    if (s == 0 || ticks_out == 0 || whole_out == 0 || s[0] == '\0' || g_timebase_freq == 0U) {
        return -1;
    }

    while (s[i] != '\0') {
        char c = s[i];

        if (c == '.') {
            if (seen_dot) {
                return -1;
            }
            seen_dot = 1;
            i++;
            continue;
        }
        if (c < '0' || c > '9') {
            return -1;
        }

        if (!seen_dot) {
            whole = (whole * 10ULL) + (uint64_t)(c - '0');
        } else {
            if (frac_scale >= 1000ULL) {
                return -1;
            }
            frac = (frac * 10ULL) + (uint64_t)(c - '0');
            frac_scale *= 10ULL;
        }
        i++;
    }

    *ticks_out = (whole * (uint64_t)g_timebase_freq) +
                 ((frac * (uint64_t)g_timebase_freq) / frac_scale);
    *whole_out = whole;
    return 0;
}

/* Minimal memcpy replacement for the freestanding kernel build. */
static void copy_bytes(void *dst, const void *src, unsigned long size) {
    unsigned long i;
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    for (i = 0; i < size; i++) {
        d[i] = s[i];
    }
}

/* Build the initramfs path used by runu: bin/<name>.bin. */
static int build_user_program_path(const char *name, char *out, unsigned int out_size) {
    static const char prefix[] = "bin/";
    static const char suffix[] = ".bin";
    unsigned int pos = 0;
    unsigned int i = 0;

    if (name == 0 || out == 0 || out_size == 0) {
        return -1;
    }

    while (prefix[i] != '\0') {
        if (pos + 1 >= out_size) {
            return -1;
        }
        out[pos++] = prefix[i++];
    }

    i = 0;
    while (name[i] != '\0') {
        if (pos + 1 >= out_size) {
            return -1;
        }
        out[pos++] = name[i++];
    }

    i = 0;
    while (suffix[i] != '\0') {
        if (pos + 1 >= out_size) {
            return -1;
        }
        out[pos++] = suffix[i++];
    }

    out[pos] = '\0';
    return 0;
}

/* Normalize leading and trailing shell input whitespace in place. */
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

/* Read a single command line from UART with basic editing support. */
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

/* Shell info commands. */
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

/* Translate SBI HSM hart states into short shell-visible names. */
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

/* Query SBI HSM state for each hart and print a compact summary. */
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

/* Split the command line into the first token and the remaining argument string. */
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

/* Emit raw initramfs file bytes to UART, preserving terminal newlines. */
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

/* initramfs-backed shell commands. */
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

/* List every visible initramfs entry once the archive has been validated. */
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

/* Print a regular initramfs file to UART. */
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

/* Single timer status command: basic counters plus the pending queue. */
static void cmd_timer(void) {
    uint64_t now;
    uint64_t uptime;

    now = timer_read();
    if (g_timebase_freq != 0) {
        uptime = (now - g_boot_time) / g_timebase_freq;
    } else {
        uptime = 0;
    }

    uart_send_string("timer freq: ");
    uart_send_dec(g_timebase_freq);
    uart_send_string("\n");

    uart_send_string("timer boot: ");
    uart_send_dec(g_boot_time);
    uart_send_string("\n");

    uart_send_string("timer now: ");
    uart_send_dec(now);
    uart_send_string("\n");

    uart_send_string("timer next: ");
    uart_send_dec(g_next_deadline);
    uart_send_string("\n");

    uart_send_string("timer uptime: ");
    uart_send_dec(uptime);
    uart_send_string("\n");
    uart_send_string("timer pending: ");
    uart_send_dec(timer_pending_count());
    uart_send_string("\n");
    uart_send_string("timer irq_count: ");
    uart_send_dec(g_timer_irq_count);
    uart_send_string("\n");
    timer_dump_queue();
}

/* Minimal shell-facing callback used to prove that software timer events are
 * really leaving the pending list and executing at interrupt time.
 */
static void shell_timer_callback(void *arg)
{
    uint64_t now;
    uint64_t uptime;
    uint64_t progress;
    unsigned long id = (unsigned long)(uintptr_t)arg;

    now = timer_read();
    if (g_timebase_freq != 0U) {
        uptime = (now - g_boot_time) / g_timebase_freq;
    } else {
        uptime = 0U;
    }
    progress = uart_stress_progress();

    uart_send_string("\n[T");
    uart_send_dec(id);
    uart_send_string(" t=");
    uart_send_dec((unsigned long)uptime);
    uart_send_string("s u=");
    uart_send_dec((unsigned long)progress);
    uart_send_string("]\n");
}

/* Timer callbacks and allocator frees both print asynchronously while the
 * REPL is blocked in uart_recv(). Redraw the prompt only after the whole timer
 * interrupt path has finished, with a tiny delay to avoid landing on the same
 * line as the allocator free trace.
 */
void shell_redraw_prompt_delayed(void)
{
    uint64_t start;
    uint64_t wait_ticks;

    if (g_timebase_freq == 0) {
        uart_send_string(g_shell_prompt);
        return;
    }

    start = timer_read();
    wait_ticks = g_timebase_freq / 200U;
    if (wait_ticks == 0) {
        wait_ticks = 1;
    }

    while ((timer_read() - start) < wait_ticks) {
        asm volatile("" ::: "memory");
    }

    uart_send_string(g_shell_prompt);
}

/* Shell test entry only:
 * - parse seconds
 * - translate to timer ticks
 * - hand the real work to add_timer()
 */
static void cmd_addtimer(const char *arg)
{
    static unsigned long next_timer_id = 1;
    uint64_t ticks;
    uint64_t whole_seconds;
    unsigned long id;

    if (arg == 0) {
        uart_send_string("usage: addtimer <seconds>\n");
        return;
    }
    if (g_timebase_freq == 0) {
        uart_send_string("addtimer: timer frequency unavailable\n");
        return;
    }
    if (parse_seconds_to_ticks(arg, &ticks, &whole_seconds) != 0 || ticks == 0U) {
        uart_send_string("usage: addtimer <seconds>\n");
        uart_send_string("note: decimal seconds like 1.5 are supported\n");
        return;
    }

    id = next_timer_id++;

    if (add_timer(shell_timer_callback, (void *)(uintptr_t)id, ticks) != 0) {
        uart_send_string("addtimer: failed to allocate timer event\n");
        return;
    }

    uart_send_string("addtimer: scheduled event ");
    uart_send_dec(id);
    uart_send_string(" after ");
    uart_send_string(arg);
    uart_send_string(" sec\n");
}

/* Single memory status command: counters plus slab/buddy snapshots. */
static void cmd_mem(void)
{
    uart_send_string("memory stats\n");
    memory_print_memstat();
    uart_send_string("slab state\n");
    memory_print_slabinfo();
    uart_send_string("buddy state\n");
    memory_print_buddyinfo();
}

/* Emit enough text to force the TX path to queue and drain asynchronously. */
/* One-shot UART demo:
 * - emits enough output to exercise the TX interrupt path
 * - asks the user for one input character to prove RX interrupts feed the
 *   shell path
 * - prints a compact pass/fail summary based on the UART driver counters
 */
static void cmd_uartdemo(void) {
    struct uart_demo_stats stats;
    char c;
    int pass;

    uart_demo_reset_stats();

    uart_send_string("uartdemo: TX phase\n");
    uart_send_string("uartdemo: TX interrupt-driven output test line\n");

    uart_send_string("uartdemo: RX phase, type one printable key: ");
    c = uart_recv();
    uart_send_string("\n");

    uart_demo_get_stats(&stats);

    uart_send_string("uartdemo: got '");
    uart_send(c);
    uart_send_string("'\n");

    uart_send_string("uartdemo: stats async_ready=");
    uart_send_dec(stats.async_ready);
    uart_send_string(" ext_irq=");
    uart_send_dec(stats.ext_irq_count);
    uart_send_string(" rx_irq=");
    uart_send_dec(stats.rx_irq_count);
    uart_send_string(" tx_irq=");
    uart_send_dec(stats.tx_irq_count);
    uart_send_string(" rx_fallback=");
    uart_send_dec(stats.rx_fallback_count);
    uart_send_string(" tx_poll=");
    uart_send_dec(stats.tx_poll_count);
    uart_send_string("\n");

    pass = (stats.async_ready != 0UL) &&
           (stats.ext_irq_count != 0UL) &&
           (stats.rx_irq_count != 0UL) &&
           (stats.tx_irq_count != 0UL) &&
           (stats.rx_fallback_count == 0UL) &&
           (stats.tx_poll_count == 0UL);

    if (pass) {
        uart_send_string("uartdemo: PASS - RX/TX are interrupt-driven\n");
    } else {
        uart_send_string("uartdemo: FAIL - counters do not match the interrupt-driven path\n");
    }
}

/* Queue many small TX chunks so timer markers have a clear chance to
 * interleave with UART bottom-half work.
 */
static void cmd_uartstress(const char *arg)
{
    uint64_t count = 8192ULL;

    if (arg != 0 && arg[0] != '\0') {
        if (parse_u64_dec(arg, &count) != 0 || count == 0ULL) {
            uart_send_string("usage: uartstress [count]\n");
            return;
        }
    }

    if (uart_stress_start(count) != 0) {
        uart_send_string("uartstress: failed to start TX stress\n");
        return;
    }

    uart_send_string("uartstress: started ");
    uart_send_dec((unsigned long)count);
    uart_send_string(" TX markers\n");
}

/* Load a raw user binary from initramfs into the reserved execution window. */
static void cmd_runu(const char *arg) {
    const void *data;
    unsigned long size;
    unsigned int mode;
    char path[64];

    /* Validate shell input and make sure initramfs is available first. */
    if (arg == 0) {
        uart_send_string("usage: runu <name>\n");
        return;
    }

    if (g_initrd_start == 0 || g_initrd_end <= g_initrd_start) {
        uart_send_string("runu: initramfs unavailable\n");
        return;
    }

    if (build_user_program_path(arg, path, sizeof(path)) != 0) {
        uart_send_string("runu: program name too long\n");
        return;
    }

    if (cpio_find((const void *)g_initrd_start, (const void *)g_initrd_end,
                  path, &data, &size, &mode) != 0) {
        uart_send_string("runu: file not found: ");
        uart_send_string(path);
        uart_send_string("\n");
        return;
    }

    /* Accept only regular files that fit in the reserved user code window. */
    if ((mode & CPIO_MODE_TYPE_MASK) != CPIO_MODE_REGULAR) {
        uart_send_string("runu: not a regular file: ");
        uart_send_string(path);
        uart_send_string("\n");
        return;
    }

    if (size == 0 || size > USER_CODE_SIZE) {
        uart_send_string("runu: invalid program size: ");
        uart_send_dec(size);
        uart_send_string("\n");
        return;
    }

    /* Stage the raw user binary at the fixed execution address. */
    copy_bytes((void *)(uintptr_t)USER_CODE_BASE, data, size);
    /* Freshly copied user code will be executed from the same fixed address on
     * every runu invocation. Make the new instruction stream visible to the
     * CPU before enter_user_mode() jumps there, otherwise a previous program
     * may still be sitting in the I-cache.
     */
    asm volatile(".word 0x0000100f" ::: "memory");

    uart_send_string("runu: found file ");
    uart_send_string(path);
    uart_send_string("\n");
    uart_send_string("runu: copied to ");
    uart_send_hex((unsigned long)USER_CODE_BASE);
    uart_send_string("\n");
    uart_send_string("runu: size = ");
    uart_send_dec(size);
    uart_send_string("\n");
    uart_send_string("runu: entry = ");
    uart_send_hex((unsigned long)USER_CODE_BASE);
    uart_send_string("\n");
    uart_send_string("runu: user stack top = ");
    uart_send_hex((unsigned long)USER_STACK_TOP);
    uart_send_string("\n");
    uart_send_string("runu: requested name = ");
    uart_send_string(arg);
    uart_send_string("\n");
    uart_send_string("runu: switching S-mode -> U-mode via sret\n");

    /* Hand control to the prepared user-mode entry path. */
    enter_user_mode();
}

/* Main command dispatcher for the interactive shell. */
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
        uart_send_string("  demo <name> - Run higher-level demos/tests\n");
        uart_send_string("  mem    - Show allocator, slab, and buddy state\n");
        uart_send_string("  timer  - Show timer subsystem state and pending queue\n");
        uart_send_string("  addtimer <sec> - Schedule a one-shot software timer (supports 1.5)\n");
        uart_send_string("  uartdemo - Run one-shot RX/TX interrupt-driven UART demo\n");
        uart_send_string("  uartstress [count] - Queue many UART TX markers for interleave testing\n");
        uart_send_string("  memtest <name> - Legacy alias for allocator tests\n");
        uart_send_string("  runu <name> - Load and run bin/<name>.bin from initramfs\n");
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

    if (streq(cmd, "memtest") || streq(cmd, "kmtest")) {
        memory_run_kmtest(arg);
        return;
    }

    if (streq(cmd, "demo")) {
        demo_run(arg);
        return;
    }

    if (streq(cmd, "mem")) {
        cmd_mem();
        return;
    }

    if (streq(cmd, "timer")) {
        cmd_timer();
        return;
    }

    if (streq(cmd, "addtimer")) {
        cmd_addtimer(arg);
        return;
    }

    if (streq(cmd, "uartdemo")) {
        cmd_uartdemo();
        return;
    }

    if (streq(cmd, "uartstress")) {
        cmd_uartstress(arg);
        return;
    }

    if (streq(cmd, "runu")) {
        cmd_runu(arg);
        return;
    }

    uart_send_string("Unknown command: ");
    uart_send_string(cmd);
    uart_send_string("\n");
}

/* Single-command REPL wrapper used by kernel_main. */
void runAShell(int32_t pid) {
    shell_t shell;
    char command_buffer[128];
    shell.pid = pid;
    uart_send_string(g_shell_prompt);

    getCommand(command_buffer, 128);
    trim_in_place(command_buffer);

    shell.command = command_buffer;

    processCommand(&shell);
}
