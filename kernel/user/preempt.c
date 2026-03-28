#include <stdint.h>

#define SYS_getpid     0UL
#define SYS_uart_read  1UL
#define SYS_uart_write 2UL
#define SYS_exec       3UL
#define SYS_fork       4UL
#define SYS_exit       5UL
#define SYS_stop       6UL

#define DO_SYSCALL0(nr)                                                       \
    ({                                                                        \
        register long a0_ asm("a0");                                          \
        register long a7_ asm("a7") = (nr);                                   \
        asm volatile("ecall" : "=r"(a0_) : "r"(a7_) : "memory");              \
        a0_;                                                                   \
    })

#define DO_SYSCALL1(nr, arg0)                                                 \
    ({                                                                        \
        register long a0_ asm("a0") = (arg0);                                 \
        register long a7_ asm("a7") = (nr);                                   \
        asm volatile("ecall" : "+r"(a0_) : "r"(a7_) : "memory");              \
        a0_;                                                                   \
    })

#define DO_SYSCALL2(nr, arg0, arg1)                                           \
    ({                                                                        \
        register long a0_ asm("a0") = (arg0);                                 \
        register long a1_ asm("a1") = (arg1);                                 \
        register long a7_ asm("a7") = (nr);                                   \
        asm volatile("ecall" : "+r"(a0_) : "r"(a1_), "r"(a7_) : "memory");    \
        a0_;                                                                   \
    })

static long getpid(void) { return DO_SYSCALL0(SYS_getpid); }
static long uart_write(const char *buf, long count) { return DO_SYSCALL2(SYS_uart_write, (long)buf, count); }
static void exit_proc(int status) { (void)DO_SYSCALL1(SYS_exit, status); for (;;) {} }

static long str_len(const char *s)
{
    long n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void put_str(const char *s)
{
    uart_write(s, str_len(s));
}

static char *append_str(char *dst, const char *src)
{
    while (*src != '\0') {
        *dst++ = *src++;
    }
    return dst;
}

static char *append_dec(char *dst, long value)
{
    char tmp[24];
    int n = 0;
    unsigned long v;

    if (value < 0) {
        *dst++ = '-';
        v = (unsigned long)(-value);
    } else {
        v = (unsigned long)value;
    }
    if (v == 0UL) {
        *dst++ = '0';
        return dst;
    }
    while (v != 0UL) {
        tmp[n++] = (char)('0' + (v % 10UL));
        v /= 10UL;
    }
    while (n > 0) {
        *dst++ = tmp[--n];
    }
    return dst;
}

void preempt_main(void)
{
    long pid = getpid();
    long iter;
    volatile long spin;
    char buf[64];
    char *p;

    for (iter = 0; iter < 4; iter++) {
        for (spin = 0; spin < 30000000L; spin++) {
            asm volatile("" ::: "memory");
        }

        p = buf;
        p = append_str(p, "busy: pid=");
        p = append_dec(p, pid);
        *p++ = '\n';
        p = append_str(p, "  iter=");
        p = append_dec(p, iter);
        *p++ = '\n';
        *p = '\0';
        put_str(buf);
    }

    exit_proc(0);
}
