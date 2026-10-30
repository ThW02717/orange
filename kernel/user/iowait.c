#include "user_api.h"

/* Keep shared state in the raw image, not in a leader stack that exits. */
static long reader_started = -1;

static void puts_user(const char *s)
{
    long size = 0;

    while (s[size] != '\0')
        size++;
    (void)uart_write(s, size);
}

static void reader(void *arg)
{
    char c;
    (void)arg;

    __atomic_store_n(&reader_started, 1, __ATOMIC_RELEASE);
    if (uart_read(&c, 1) != 1 || c != 'R') {
        puts_user("[iowait] FAIL: expected R\n");
        exit_group(1);
    }
    puts_user("[iowait] reader resumed: PASS\n");
}

static void observer(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&reader_started, __ATOMIC_ACQUIRE)) {}
    puts_user("[iowait] other thread ran while reader waited; type R\n");
}

void iowait_main(void)
{
    __atomic_store_n(&reader_started, 0, __ATOMIC_RELEASE);
    if (thread_create_user(reader, 0) < 0 || thread_create_user(observer, 0) < 0) {
        puts_user("[iowait] FAIL: thread creation\n");
        exit_group(1);
    }
    /* The remaining reader may be the only live task. Keep the shell paused. */
    thread_exit(0);
}
