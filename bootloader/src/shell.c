/*
 * shell.c - Bootloader interactive shell and UART kernel loader.
 *
 * The shell provides two commands:
 *   help  - show available commands
 *   load  - receive kernel+initrd bundle over UART and jump to it
 *
 * UART bundle protocol (also see tools/uart_load.py on the PC side):
 *   1. Bootloader prints "UART loader ready..."
 *   2. PC sends header:  [magic:4B][payload_size:4B][checksum:4B]
 *   3. PC sends payload: [kernel_len:4B][initrd_len:4B]
 *                         [kernel_csum:4B][initrd_csum:4B]
 *                         [kernel.bin raw bytes...]
 *                         [initramfs.cpio raw bytes...]
 *   4. Bootloader verifies checksums, copies kernel to 0x20000000
 *      and initrd to 0x46100000, then jumps to the kernel.
 */

#include <stdint.h>
#include "shell.h"
#include "uart.h"
#include "string.h"

static unsigned long g_hartid = 0;
static unsigned long g_dtb = 0;

/* protocol magic: "BOO2" = 0x324F4F42 */
#define UART_BOOT_BUNDLE_MAGIC 0x324F4F42UL
/* max kernel image: 16 MiB */
#define UART_BOOT_MAX_SIZE (16UL * 1024UL * 1024UL)
/* max initrd image: 64 MiB */
#define UART_INITRD_MAX_SIZE (64UL * 1024UL * 1024UL)
/* max bundle: header(16) + kernel + initrd */
#define UART_BOOT_BUNDLE_MAX_SIZE (UART_BOOT_MAX_SIZE + UART_INITRD_MAX_SIZE + 16UL)
#ifdef QEMU
#define UART_BOOT_LOAD_ADDR 0x82000000UL
#define UART_INITRD_LOAD_ADDR 0x88000000UL
#else
/* kernel loaded at 0x20000000 (matches linker.ld KERNEL_LOAD_ADDR) */
#define UART_BOOT_LOAD_ADDR 0x20000000UL
/* initrd loaded at 0x46100000 (matches kernel.its ramdisk load addr) */
#define UART_INITRD_LOAD_ADDR 0x46100000UL
#endif
static unsigned long g_initrd_start = 0;
static unsigned long g_initrd_end = 0;

void shell_set_context(unsigned long hartid, unsigned long dtb_addr) {
    g_hartid = hartid;
    g_dtb = dtb_addr;
}

/* check if character is whitespace */
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

/* simple string equality check */
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

/* trim leading and trailing whitespace in-place */
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

/*
 * getCommand: read a line from UART into buffer.
 * Supports backspace (127, '\b') for editing.
 * Converts '\r' or '\n' to line terminator.
 */
static void getCommand(char* buffer, int max_len) {
    int idx = 0;
    int ignore_next_lf = 0;

    while (1) {
        char c = uart_recv();

        /* handle "\r\n" (CR+LF) — skip LF if we saw CR */
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
            buffer[idx] = '\0';      // null-terminate the command
            return;
        }

        /* backspace handling */
        if (c == KEY_BACKSPACE || c == '\b' || c == 127) {
            if (idx > 0) {
                idx--;
                uart_send_string("\b \b");  // erase the last char on terminal
            }
            continue;
        }

        /* printable characters only */
        if (c >= ' ' && c <= '~') {
            if (idx < max_len - 1) {
                buffer[idx++] = c;
                uart_send(c);               // echo back to terminal
            }
        }
    }
}

/* read a little-endian 32-bit unsigned integer from UART (4 bytes) */
static unsigned long recv_u32_le(void) {
    unsigned long v = 0;
    v |= (unsigned long)(uint8_t)uart_recv();
    v |= (unsigned long)(uint8_t)uart_recv() << 8;
    v |= (unsigned long)(uint8_t)uart_recv() << 16;
    v |= (unsigned long)(uint8_t)uart_recv() << 24;
    return v;
}

/* read 'size' raw bytes from UART into dst */
static void recv_bytes(void *dst, unsigned long size) {
    unsigned char *p = (unsigned char *)dst;
    unsigned long i;
    for (i = 0; i < size; i++) {
        p[i] = (unsigned char)uart_recv();
    }
}

/* read a little-endian 32-bit unsigned integer from memory */
static uint32_t load_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* copy bytes handle overlapping regions correctly */
static void copy_bytes(uint8_t *dst, const uint8_t *src, unsigned long size) {
    unsigned long i;
    if (dst < src) {
        for (i = 0; i < size; i++) {
            dst[i] = src[i];
        }
    } else if (dst > src) {
        for (i = size; i > 0; i--) {
            dst[i - 1] = src[i - 1];
        }
    }
}

/* compute a simple 32-bit sum checksum */
static unsigned long checksum_bytes(const uint8_t *data, unsigned long size) {
    unsigned long sum = 0;
    unsigned long i;

    for (i = 0; i < size; i++) {
        sum = (sum + (unsigned long)data[i]) & 0xffffffffUL;
    }
    return sum;
}

/*
 * uart_load_image_payload: receive a block of data over UART.
 * Reads 'size' bytes into 'dst', verifies the checksum.
 * Returns 0 on success, -1 on error.
 */
static int uart_load_image_payload(uint8_t *dst, unsigned long max_size,
                                   unsigned long load_addr,
                                   unsigned long size, unsigned long expected_sum,
                                   unsigned long *size_out) {
    unsigned long actual_sum;

    if (size == 0 || size > max_size) {
        uart_send_string("load fail: bad size ");
        uart_send_dec(size);
        uart_send_string("\n");
        return -1;
    }

    uart_send_string("loading ");
    uart_send_dec(size);
    uart_send_string(" bytes to ");
    uart_send_hex(load_addr);
    uart_send_string("\n");

    recv_bytes(dst, size);

    actual_sum = checksum_bytes(dst, size);
    if (actual_sum != expected_sum) {
        uart_send_string("load fail: checksum exp=");
        uart_send_hex(expected_sum);
        uart_send_string(" got=");
        uart_send_hex(actual_sum);
        uart_send_string("\n");
        return -1;
    }

    if (size_out != 0) {
        *size_out = size;
    }
    return 0;
}

/*
 * boot_load_and_jump: handle the "load" command.
 *
 * Receives a bundle containing kernel + initrd over UART,
 * unpacks them to their target addresses, then jumps to the kernel.
 *
 * Bundle format (sent by tools/uart_load.py):
 *   Header:  [magic:4B][payload_size:4B][checksum:4B]
 *   Payload: [kernel_size:4B][initrd_size:4B]
 *            [kernel_checksum:4B][initrd_checksum:4B]
 *            [kernel raw bytes...][initrd raw bytes...]
 */
static void boot_load_and_jump(void) {
    unsigned long magic;
    unsigned long size;
    unsigned long expected_sum;
    uint8_t *dst = (uint8_t *)UART_BOOT_LOAD_ADDR;
    void (*next_kernel)(unsigned long, unsigned long, unsigned long, unsigned long);

    uart_send_string("UART loader ready for kernel+initrd bundle\n");
    uart_send_string("send header <magic,size,checksum> then payload\n");

    /* step 1: read header */
    magic = recv_u32_le();
    size = recv_u32_le();
    expected_sum = recv_u32_le();

    if (magic == UART_BOOT_BUNDLE_MAGIC) {
        const uint8_t *bundle = dst;
        uint32_t kernel_size;
        uint32_t initrd_size;
        uint32_t kernel_sum;
        uint32_t initrd_sum;
        const uint8_t *kernel_src;
        const uint8_t *initrd_src;
        unsigned long total_needed;

        /* step 2: receive entire bundle payload into memory */
        if (uart_load_image_payload(dst, UART_BOOT_BUNDLE_MAX_SIZE, UART_BOOT_LOAD_ADDR,
                                    size, expected_sum, 0) != 0) {
            return;
        }

        if (size < 16UL) {
            uart_send_string("load fail: bundle too small\n");
            return;
        }

        /* step 3: parse bundle metadata (first 16 bytes) */
        kernel_size = load_u32_le(bundle);        // offset 0
        initrd_size = load_u32_le(bundle + 4);    // offset 4
        kernel_sum = load_u32_le(bundle + 8);     // offset 8
        initrd_sum = load_u32_le(bundle + 12);    // offset 12

        /* step 4: validate sizes */
        total_needed = 16UL + (unsigned long)kernel_size + (unsigned long)initrd_size;
        if (kernel_size == 0U || kernel_size > UART_BOOT_MAX_SIZE ||
            initrd_size == 0U ||
            initrd_size > UART_INITRD_MAX_SIZE || total_needed != size) {
            uart_send_string("load fail: bad bundle layout\n");
            return;
        }

        kernel_src = bundle + 16;
        initrd_src = kernel_src + kernel_size;

        /* step 5: verify kernel & initrd checksums */
        if (checksum_bytes(kernel_src, (unsigned long)kernel_size) != (unsigned long)kernel_sum ||
            checksum_bytes(initrd_src, (unsigned long)initrd_size) != (unsigned long)initrd_sum) {
            uart_send_string("load fail: bad bundle checksum\n");
            return;
        }

        /* step 6: copy kernel and initrd to their final addresses */
        copy_bytes((uint8_t *)UART_BOOT_LOAD_ADDR, kernel_src, (unsigned long)kernel_size);
        copy_bytes((uint8_t *)UART_INITRD_LOAD_ADDR, initrd_src, (unsigned long)initrd_size);

        g_initrd_start = UART_INITRD_LOAD_ADDR;
        g_initrd_end = UART_INITRD_LOAD_ADDR + (unsigned long)initrd_size;
        if (g_initrd_end < g_initrd_start) {
            g_initrd_start = 0;
            g_initrd_end = 0;
            uart_send_string("load fail: initrd range overflow\n");
            return;
        }
        uart_send_string("bundle ok: kernel=");
        uart_send_dec(kernel_size);
        uart_send_string(" initrd=");
        uart_send_dec(initrd_size);
        uart_send_string("\n");
    } else {
        uart_send_string("load fail: bad magic ");
        uart_send_hex(magic);
        uart_send_string(" (expect BOO2 bundle)\n");
        return;
    }

    uart_send_string("load ok, jump ");
    uart_send_hex(UART_BOOT_LOAD_ADDR);
    if (g_initrd_end > g_initrd_start) {
        uart_send_string(" (initrd ");
        uart_send_hex(g_initrd_start);
        uart_send_string("-");
        uart_send_hex(g_initrd_end);
        uart_send_string(")");
    }
    uart_send_string("\n");

    /*
     * Memory barriers before jumping into freshly copied code.
     * fence rw,rw:  order all prior reads & writes before later ones.
     * fence.i:      synchronize instruction fetch after writing code bytes.
     */
    asm volatile("fence rw, rw" ::: "memory");
    asm volatile(".word 0x0000100f" ::: "memory");

    /*
     * Jump to the kernel at UART_BOOT_LOAD_ADDR.
     * Pass four arguments via RISC-V a0-a3:
     *   a0 = hartid
     *   a1 = device tree address
     *   a2 = initrd start address
     *   a3 = initrd end address
     */
    next_kernel = (void (*)(unsigned long, unsigned long, unsigned long, unsigned long))UART_BOOT_LOAD_ADDR;
    next_kernel(g_hartid, g_dtb, g_initrd_start, g_initrd_end);
    uart_send_string("loaded image returned\n");
}

/* processCommand: dispatch shell commands */
void processCommand(shell_t* shell) {
    if (shell->command[0] == '\0') {
        return;
    }

    if (streq(shell->command, "help")) {
        uart_send_string("Available commands:\n");
        uart_send_string("  help   - Show this message\n");
        uart_send_string("  load   - Load kernel+initrd bundle via UART and jump\n");
        return;
    }

    if (streq(shell->command, "load")) {
        boot_load_and_jump();
        return;
    }

    uart_send_string("Unknown command: ");
    uart_send_string(shell->command);
    uart_send_string("\n");


}

/* runAShell: display prompt, read a line, dispatch it */
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
