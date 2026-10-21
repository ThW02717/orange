/*
 * Kernel console: shell_run() -> shell_readline() -> shell_execute().
 * Commands use kernel APIs directly. exec.c owns user-image loading.
 */
#include "demo_uart.h"
#include "platform.h"
#include "exec.h"
#include "sched.h"
#include "user_layout.h"
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
#include "thread.h"
#include "user.h"

/* Interactive UART shell with initramfs-backed file commands and user-program launch. */

#define CPIO_MODE_TYPE_MASK 0170000U
#define CPIO_MODE_REGULAR   0100000U

/*
 * Timer callbacks may print while the REPL is blocked in uart_recv(), so keep
 * the prompt string in one place and redraw it after asynchronous messages.
 */
static const char g_shell_prompt[] = "> ";

/* Small local helpers used by the shell parser and loader commands. */
static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/*
 * Parse decimal seconds into timer ticks using fixed-point arithmetic.
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
    int seen_digit = 0;

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
        seen_digit = 1;

        if (!seen_dot) {
            if (whole > (UINT64_MAX - (uint64_t)(c - '0')) / 10ULL) {
                return -1;
            }
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

    if (!seen_digit) {
        return -1;
    }
    {
        uint64_t fraction_ticks = (frac * (uint64_t)g_timebase_freq) / frac_scale;
        if (whole > (UINT64_MAX - fraction_ticks) / (uint64_t)g_timebase_freq) {
            return -1;
        }
        *ticks_out = whole * (uint64_t)g_timebase_freq + fraction_ticks;
    }
    *whole_out = whole;
    return 0;
}

/* Normalize leading and trailing shell input whitespace in place. */
static void trim_in_place(char *s)
{
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

    unsigned int len = strlen(s);
    while (len > 0 && is_space(s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
}

/* Read a single command line from UART with basic editing support. */
static void shell_readline(char* buffer, int max_len)
{
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
static void print_info(void)
{
    const struct boot_info *boot = platform_boot_info();
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

    if (boot->dtb != 0) {
        mem_count = fdt_get_memory_regions((const void *)boot->dtb, mem_regions, FDT_MAX_MEM_REGIONS);
        if (mem_count < 0) {
            mem_count = 0;
        }
        for (i = 0; i < mem_count; i++) {
            mem_total += mem_regions[i].size;
        }
    }

    uart_send_string("core id: ");
    uart_send_dec(boot->hartid);
    uart_send_string("\n");

    uart_send_string("dtb addr: ");
    uart_send_hex(boot->dtb);
    uart_send_string("\n");

    uart_send_string("initrd: ");
    if (boot->initrd_start != 0 && boot->initrd_end > boot->initrd_start) {
        uart_send_string("start=");
        uart_send_hex((unsigned long)boot->initrd_start);
        uart_send_string(" end=");
        uart_send_hex((unsigned long)boot->initrd_end);
        uart_send_string(" size=");
        uart_send_hex((unsigned long)(boot->initrd_end - boot->initrd_start));
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
static const char* hsm_state_name(long st)
{
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
static void print_cores(void)
{
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
static void split_first_arg(char *line, char **cmd, char **arg)
{
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
static void uart_send_bytes(const uint8_t *data, unsigned long size)
{
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
                  unsigned int mode, void *ctx)
{
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
static void cmd_ls(void)
{
    const struct boot_info *boot = platform_boot_info();
    struct ls_ctx st;
    st.count = 0;
    if (boot->initrd_start == 0 || boot->initrd_end <= boot->initrd_start) {
        uart_send_string("initrd not available\n");
        return;
    }
    if (cpio_iterate((const void *)boot->initrd_start, (const void *)boot->initrd_end,
                     ls_cb, &st) != 0) {
        uart_send_string("cpio parse error\n");
        return;
    }
    uart_send_string("Total ");
    uart_send_dec(st.count);
    uart_send_string(" files.\n");
}

/* Print a regular initramfs file to UART. */
static void cmd_cat(const char *arg)
{
    const struct boot_info *boot = platform_boot_info();
    const void *data = 0;
    unsigned long size = 0;
    unsigned int mode = 0;
    char alt[128];

    if (boot->initrd_start == 0 || boot->initrd_end <= boot->initrd_start) {
        uart_send_string("initrd not available\n");
        return;
    }
    if (arg == 0 || arg[0] == '\0') {
        uart_send_string("usage: cat <file>\n");
        return;
    }

    if (cpio_find((const void *)boot->initrd_start, (const void *)boot->initrd_end,
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
            if (cpio_find((const void *)boot->initrd_start, (const void *)boot->initrd_end,
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
static void cmd_timer(void)
{
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

/* Print a one-shot marker, or let the UART stress stream insert it. */
static void shell_timer_callback(void *arg)
{
    unsigned long id = (unsigned long)(uintptr_t)arg;

    if (demo_uart_stress_note_timer(id) == 0) {
        return;
    }

    uart_send_string("\n[T");
    uart_send_dec(id);
    uart_send_string("]\n");
    uart_send_string(g_shell_prompt);
}

/* Convert shell seconds to the timer API's tick count. */
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

struct shell_timeout {
    uint64_t submitted;
    char message[];
};

static void shell_print_time(uint64_t ticks)
{
    uint64_t elapsed = ticks - g_boot_time;
    unsigned long ms = (unsigned long)((elapsed % g_timebase_freq) *
                                       1000ULL / g_timebase_freq);

    uart_send_dec((unsigned long)(elapsed / g_timebase_freq));
    uart_send('.');
    if (ms < 100UL) {
        uart_send('0');
    }
    if (ms < 10UL) {
        uart_send('0');
    }
    uart_send_dec(ms);
}

static void shell_timeout_callback(void *arg)
{
    struct shell_timeout *timeout = arg;
    uint64_t fired = timer_read();

    uart_send_string("\n[timeout] submitted=");
    shell_print_time(timeout->submitted);
    uart_send_string("s fired=");
    shell_print_time(fired);
    uart_send_string("s message=");
    uart_send_string(timeout->message);
    uart_send_string("\n");
    uart_send_string(g_shell_prompt);
}

static void cmd_set_timeout(char *arg)
{
    char *seconds;
    char *message;
    struct shell_timeout *timeout;
    uint64_t ticks;
    uint64_t whole;
    uint64_t submitted = timer_read();
    unsigned int length;

    if (arg == 0) {
        goto usage;
    }
    split_first_arg(arg, &seconds, &message);
    if (message == 0 || parse_seconds_to_ticks(seconds, &ticks, &whole) != 0) {
        goto usage;
    }

    length = strlen(message) + 1U;
    timeout = kmalloc(sizeof(*timeout) + length);
    if (timeout == 0) {
        uart_send_string("setTimeout: allocation failed\n");
        return;
    }
    timeout->submitted = submitted;
    /*
     * The next shell command reuses its stack buffer. Keep our own message
     * until this event fires or timer_stop cancels it.
     */
    memcpy(timeout->message, message, length);
    if (add_timer_with_cleanup(shell_timeout_callback, timeout, ticks, kfree) != 0) {
        kfree(timeout);
        uart_send_string("setTimeout: timer registration failed\n");
        return;
    }
    uart_send_string("setTimeout: queued\n");
    return;

usage:
    uart_send_string("usage: setTimeout SECONDS MESSAGE\n");
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

/* Compatibility command for demo uart. */
static void cmd_uartdemo(void)
{
    (void)demo_run("uart");
}

/*
 * Queue many small TX chunks so timer markers have a clear chance to
 * interleave with UART bottom-half work.
 */
static void cmd_uartstress(const char *arg)
{
    char buf[48];
    unsigned int i = 0;
    static const char prefix[] = "stress";

    while (prefix[i] != '\0' && i + 1U < sizeof(buf)) {
        buf[i] = prefix[i];
        i++;
    }
    if (arg != 0 && arg[0] != '\0' && i + 1U < sizeof(buf)) {
        unsigned int j = 0;

        buf[i++] = ' ';
        while (arg[j] != '\0' && i + 1U < sizeof(buf)) {
            buf[i++] = arg[j++];
        }
    }
    buf[i] = '\0';
    (void)demo_run(buf);
}

static int shell_launch_user_program_instances(const char *name,
                                               const char *label,
                                               unsigned int instances,
                                               int verbose,
                                               int lab_ecalls)
{
    struct exec_result result;

    if (exec_spawn(name, instances, &result) != 0) {
        uart_send_string(label);
        uart_send_string(": failed to load or create user task\n");
        return -1;
    }
    if (verbose != 0) {
        uart_send_string(label);
        uart_send_string(": found file bin/");
        uart_send_string(name);
        uart_send_string(".bin\n");
        uart_send_string(label);
        uart_send_string(": copied to ");
        uart_send_hex((unsigned long)result.entry);
        uart_send_string("\n");
        uart_send_string(label);
        uart_send_string(": size = ");
        uart_send_dec(result.size);
        uart_send_string("\n");
        uart_send_string(label);
        uart_send_string(": entry = ");
        uart_send_hex((unsigned long)result.entry);
        uart_send_string("\n");
        uart_send_string(label);
        uart_send_string(": requested name = ");
        uart_send_string(name);
        uart_send_string("\n");
    }

    if (lab_ecalls) {
        trap_set_ecall_demo(result.tids[0]);
    }
    uart_send_string(label);
    uart_send_string(": enqueue user task and schedule\n");
    uart_tx_wait_idle();
    thread_run_until_idle();
    if (lab_ecalls) {
        trap_set_ecall_demo(-1);
    }
    uart_send_string(label);
    uart_send_string(": user task finished, back to shell\n");
    return 0;
}

/* Load a raw user binary from initramfs into the reserved execution window. */
static void cmd_runu(const char *arg)
{
    if (arg == 0) {
        uart_send_string("usage: runu <name>\n");
        return;
    }
    (void)shell_launch_user_program_instances(arg, "runu", 1U, 1, 0);
}

static void cmd_exec(const char *arg)
{
    int previous_trace;

    if (arg == 0) {
        arg = "prog";
    }
    /* The basic trap exercise prints CSR snapshots for each user ecall. */
    previous_trace = trap_trace_ecalls(1);
    if (strcmp(arg, "prog") == 0) {
        uart_send_string("exec: lab diagnostic image; final U-mode loop runs until reset\n");
    }
    (void)shell_launch_user_program_instances(arg, "exec", 1U, 1, (strcmp(arg, "prog") == 0));
    (void)trap_trace_ecalls(previous_trace);
}

static void cmd_fork_test(void)
{
    (void)shell_launch_user_program_instances("fork", "fork_test", 1U, 0, 0);
}

static void cmd_preempt_test(void)
{
    (void)shell_launch_user_program_instances("preempt", "preempt_test", 2U, 0, 0);
}

/* A generation token makes an old callback harmless after off/on. */
static unsigned long g_periodic_generation;
static int g_periodic_enabled;

static void shell_periodic_timer(void *arg)
{
    unsigned long generation = (uintptr_t)arg;

    if (!g_periodic_enabled || generation != g_periodic_generation) {
        return;
    }
    uart_send_string("\n[timer] uptime=");
    uart_send_dec((timer_read() - g_boot_time) / g_timebase_freq);
    uart_send_string(" seconds\n");
    uart_send_string(g_shell_prompt);
    if (add_timer(shell_periodic_timer, arg, 2ULL * g_timebase_freq) != 0) {
        g_periodic_enabled = 0;
    }
}

static int shell_periodic_enable(int enabled)
{
    if (!timer_is_running() || g_timebase_freq == 0) {
        return -1;
    }
    if (enabled && g_periodic_enabled) {
        return 0;
    }
    g_periodic_enabled = 0;
    g_periodic_generation++;
    if (enabled) {
        if (add_timer(shell_periodic_timer, (void *)(uintptr_t)g_periodic_generation,
                      2ULL * g_timebase_freq) != 0) {
            return -1;
        }
        g_periodic_enabled = 1;
    }
    return 0;
}

/* Main command dispatcher for the interactive shell. */
static void shell_execute(char *line)
{
    char *cmd;
    char *arg;

    if (line[0] == '\0') {
        return;
    }

    split_first_arg(line, &cmd, &arg);

    if (strcmp(cmd, "help") == 0) {
        uart_send_string("Available commands:\n");
        uart_send_string("  help   - Show this message\n");
        uart_send_string("  hello  - Print Hello World!\n");
        uart_send_string("  info   - Print core and SBI info\n");
        uart_send_string("  cores  - Show HSM status for core 0..7\n");
        uart_send_string("  ls     - List initrd files\n");
        uart_send_string("  cat    - Show initrd file content\n");
        uart_send_string("  demo <name> - Run demos/tests (thread|uart|stress|mem|nested|tasks|trace)\n");
        uart_send_string("  mem    - Show allocator, slab, and buddy state\n");
        uart_send_string("  timer  - Show timer subsystem state and pending queue\n");
        uart_send_string("  timer periodic on|off - Enable/disable the two-second uptime demo\n");
        uart_send_string("  addtimer <sec> - Schedule a one-shot software timer (supports 1.5)\n");
        uart_send_string("  setTimeout <seconds> <message> - Print a saved message after a delay\n");
        uart_send_string("  exec [name] - Run with ecall diagnostics (default: Lab 4 prog)\n");
        uart_send_string("  runu <name> - Load and run bin/<name>.bin from initramfs\n");
        uart_send_string("  fork_test - Run the user-space fork test and return to shell\n");
        uart_send_string("  preempt_test - Run two busy user tasks to test timer preemption\n");
        return;
    }

    if (strcmp(cmd, "hello") == 0) {
        uart_send_string("Hello World!\n");
        return;
    }

    if (strcmp(cmd, "info") == 0) {
        print_info();
        return;
    }

    if (strcmp(cmd, "cores") == 0) {
        print_cores();
        return;
    }

    if (strcmp(cmd, "ls") == 0) {
        cmd_ls();
        return;
    }

    if (strcmp(cmd, "cat") == 0) {
        cmd_cat(arg);
        return;
    }

    if (strcmp(cmd, "memtest") == 0 || strcmp(cmd, "kmtest") == 0) {
        char buf[64];
        unsigned int i = 0;
        static const char prefix[] = "mem";

        while (prefix[i] != '\0' && i + 1U < sizeof(buf)) {
            buf[i] = prefix[i];
            i++;
        }
        if (arg != 0 && arg[0] != '\0' && i + 1U < sizeof(buf)) {
            unsigned int j = 0;

            buf[i++] = ' ';
            while (arg[j] != '\0' && i + 1U < sizeof(buf)) {
                buf[i++] = arg[j++];
            }
        }
        buf[i] = '\0';
        demo_run(buf);
        return;
    }

    if (strcmp(cmd, "demo") == 0) {
        demo_run(arg);
        return;
    }

    if (strcmp(cmd, "mem") == 0) {
        cmd_mem();
        return;
    }

    if (strcmp(cmd, "timer") == 0) {
        if (arg == 0) {
            cmd_timer();
        } else if (strcmp(arg, "periodic on") == 0 || strcmp(arg, "periodic off") == 0) {
            int enabled = (strcmp(arg, "periodic on") == 0);
            if (shell_periodic_enable(enabled) != 0) {
                uart_send_string("timer: subsystem unavailable\n");
            } else {
                uart_send_string(enabled ? "timer: periodic on\n" : "timer: periodic off\n");
            }
        } else {
            uart_send_string("usage: timer [periodic on|off]\n");
        }
        return;
    }

    if (strcmp(cmd, "setTimeout") == 0) {
        cmd_set_timeout(arg);
        return;
    }

    if (strcmp(cmd, "exec") == 0) {
        cmd_exec(arg);
        return;
    }

    if (strcmp(cmd, "addtimer") == 0) {
        cmd_addtimer(arg);
        return;
    }

    if (strcmp(cmd, "uartdemo") == 0) {
        cmd_uartdemo();
        return;
    }

    if (strcmp(cmd, "uartstress") == 0) {
        cmd_uartstress(arg);
        return;
    }

    if (strcmp(cmd, "runu") == 0) {
        cmd_runu(arg);
        return;
    }

    if (strcmp(cmd, "fork_test") == 0) {
        cmd_fork_test();
        return;
    }

    if (strcmp(cmd, "preempt_test") == 0) {
        cmd_preempt_test();
        return;
    }

    uart_send_string("Unknown command: ");
    uart_send_string(cmd);
    uart_send_string("\n");
}

/* The same kernel flow owns every prompt; commands do not create a shell task. */
void shell_run(void)
{
    char line[128];

    for (;;) {
        uart_send_string("> ");
        shell_readline(line, sizeof(line));
        trim_in_place(line);
        shell_execute(line);
    }
}
