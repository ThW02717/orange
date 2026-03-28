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

static long getpid(void);
static long fork_proc(void);
static long uart_write(const char *buf, long count);
static void exit_proc(int status);
static long str_len(const char *s);
static void put_str(const char *s);
static char *append_str(char *dst, const char *src);
static char *append_dec(char *dst, long value);
static char *append_hex(char *dst, uintptr_t value);
static void print_state(const char *who, long cnt, int *cnt_addr);

void fork_main(void)
{
    int cnt = 1;
    long ret;
    volatile long spin;

    put_str("Fork test (pid = ");
    {
        char num[32];
        char *p = num;
        p = append_dec(p, getpid());
        p = append_str(p, ")\n");
        *p = '\0';
        put_str(num);
    }

    ret = fork_proc();
    if (ret == 0) {
        print_state("child1", cnt, &cnt);
        cnt++;

        ret = fork_proc();
        if (ret != 0) {
            print_state("child1", cnt, &cnt);
        } else {
            while (cnt < 5) {
                print_state("child2", cnt, &cnt);
                for (spin = 0; spin < 5000000L; spin++) {
                    asm volatile("" ::: "memory");
                }
                cnt++;
            }
        }
    } else {
        char buf[48];
        char *p = buf;

        p = append_str(p, "parent: pid=");
        p = append_dec(p, getpid());
        *p++ = '\n';
        p = append_str(p, "  child pid=");
        p = append_dec(p, ret);
        *p++ = '\n';
        *p = '\0';
        put_str(buf);
        print_state("parent", cnt, &cnt);
    }

    exit_proc(0);
}

static long getpid(void) { return DO_SYSCALL0(SYS_getpid); }
static long fork_proc(void) { return DO_SYSCALL0(SYS_fork); }
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

static char *append_hex(char *dst, uintptr_t value)
{
    static const char hex[] = "0123456789abcdef";
    int shift;

    *dst++ = '0';
    *dst++ = 'x';
    for (shift = (int)(sizeof(uintptr_t) * 8U) - 4; shift >= 0; shift -= 4) {
        *dst++ = hex[(value >> (unsigned int)shift) & 0xFUL];
    }
    return dst;
}

static void print_state(const char *who, long cnt, int *cnt_addr)
{
    char buf[96];
    char *p = buf;
    uintptr_t cur_sp;

    asm volatile("mv %0, sp" : "=r"(cur_sp));

    p = append_str(p, who);
    p = append_str(p, ": pid=");
    p = append_dec(p, getpid());
    p = append_str(p, " cnt=");
    p = append_dec(p, cnt);
    *p++ = '\n';
    p = append_str(p, "  &cnt=");
    p = append_hex(p, (uintptr_t)cnt_addr);
    *p++ = '\n';
    p = append_str(p, "  sp=");
    p = append_hex(p, cur_sp);
    *p++ = '\n';
    *p = '\0';
    put_str(buf);
}
