/* Run the production driver with a small UART model. Inject an IRQ after
 * sampling LSR; delivery waits until SIE is set, just like a local IRQ.
 */
#include <stdint.h>
#include "irqflags.h"
#include "thread.h"
#include "thread_internal.h"
#include "sched.h"
#include "kernel_test.h"
#include "memory.h"
#include "syscall.h"
#include "syscall_dispatch.h"
#include "user_layout.h"

static uint32_t uart_test_reg_read(uintptr_t reg);
static void uart_test_reg_write(uintptr_t reg, uint32_t value);
static void uart_test_irq_restore(uint64_t status);

#define UART_MMIO_TEST
#define local_irq_restore uart_test_irq_restore
#include "../kernel/src/uart.c"
#undef local_irq_restore

static int model_active;
static unsigned char rx[32], tx[RINGBUF_CAPACITY + 2U];
static unsigned int rx_head, rx_count, tx_count;
static unsigned int empty_reads, busy_writes, reads, tx_busy_reads;
static uint32_t ier;
static int arrival, pending_irq, in_irq, inject_tx, inject_byte;
static int inject_on_rx_unmask;
static unsigned int delivered;

static void deliver_irq(void)
{
    uint64_t status;

    if (!pending_irq || in_irq || !local_irqs_enabled())
        return;
    pending_irq = 0;
    in_irq = 1;
    status = local_irq_save();
    delivered++;
    if (inject_byte) {
        rx[rx_head + rx_count++] = 'p';
        inject_byte = 0;
    }
    if (inject_tx)
        uart_send('!');
    else {
        uart_irq_top();
        /* Trap return runs pending work after the hard handler. Nested IRQs
         * may enter here too; the production runner prevents BH recursion.
         */
        in_irq = 0;
        run_bottom_halves();
    }
    in_irq = 0;
    local_irq_restore(status);
}

static void uart_test_irq_restore(uint64_t status)
{
    local_irq_restore(status);
    if (model_active)
        deliver_irq();
}

static uint32_t uart_test_reg_read(uintptr_t reg)
{
    uint32_t status;

    if (!model_active)
        return *(volatile uint8_t *)reg;
    if (reg == UART_LSR_REG) {
        status = (rx_count ? LSR_RX_READY : 0U) |
                 (tx_busy_reads ? 0U : LSR_TX_IDLE);
        if (tx_busy_reads)
            tx_busy_reads--;
        /* Bound a broken polling loop so the test reports a failure. */
        if (++reads > 1000)
            status |= LSR_RX_READY | LSR_TX_IDLE;
        if (arrival) {
            arrival = 0;
            pending_irq = 1;
            deliver_irq();
        }
        return status;
    }
    if (reg == UART_RBR_REG) {
        if (!rx_count) {
            empty_reads++;
            return 0xee;
        }
        rx_count--;
        return rx[rx_head++];
    }
    if (reg == UART_IER_REG)
        return ier;
    return 0;
}

static void uart_test_reg_write(uintptr_t reg, uint32_t value)
{
    if (!model_active) {
        *(volatile uint8_t *)reg = (uint8_t)value;
        return;
    }
    if (reg == UART_IER_REG) {
        if (inject_on_rx_unmask && !(ier & UART_IER_RX_ENABLE) &&
            (value & UART_IER_RX_ENABLE)) {
            check(rx_head + rx_count < sizeof(rx), "RX model storage exhausted");
            rx[rx_head + rx_count++] = 'q';
            inject_on_rx_unmask = 0;
            pending_irq = 1;
        }
        ier = value;
    }
    if (reg == UART_THR_REG) {
        if (tx_busy_reads)
            busy_writes++;
        if (tx_count < sizeof(tx))
            tx[tx_count++] = (unsigned char)value;
        tx_busy_reads = 1;
    }
}

static void reset_model(void)
{
    local_irq_disable();
    model_active = 1;
    rx_head = rx_count = tx_count = 0;
    empty_reads = busy_writes = reads = tx_busy_reads = 0;
    arrival = pending_irq = in_irq = inject_tx = inject_byte = 0;
    inject_on_rx_unmask = 0;
    delivered = 0;
    ier = UART_IER_TX_ENABLE;
    uart_reset_runtime_state();
    uart_irq_enabled = 0;
    local_irq_enable();
}

static void service_tx_irq(void)
{
    uint64_t status = local_irq_save();

    /* The transmitter finished its previous byte and asserted TX-ready. */
    tx_busy_reads = 0;
    uart_irq_top();
    local_irq_restore(status);
    uart_tx_run_bottom_half();
}

static int io_waiter_tid;
static int io_waiter_done;
static int cancel_reader;
static int buffer_owner_tid;
static char *shared_buffer;
static int syscall_is_write;

/* Run the real syscall C path on a prepared user task, without executing
 * the fixture's placeholder user image. Enter with SIE clear like ecall.
 */
static void blocking_syscall(void *arg)
{
    struct thread *task = thread_current();
    (void)arg;

    local_irq_disable();
    task->tc->a7 = syscall_is_write ? SYS_uart_write : SYS_uart_read;
    task->tc->a0 = (uintptr_t)shared_buffer;
    task->tc->a1 = syscall_is_write ? 2 : 1;
    handle_user_ecall(task->tc);
    check(!local_irqs_enabled(), "UART syscall lost its entry IRQ state");
    check((long)task->tc->a0 == (syscall_is_write ? 1 : -1),
          "UART syscall used a buffer after its owner exited");
    check(shared_buffer[0] == (syscall_is_write ? '\n' : '?') && shared_buffer[1] == 'z',
          "sleeping read wrote through an expired stack pointer");
    io_waiter_done = 1;
}

static void expire_shared_buffer(void *arg)
{
    struct thread *task = thread_test_find(io_waiter_tid);
    (void)arg;

    check(task && task->state == THREAD_WAITING && !io_waiter_done,
          "UART syscall did not sleep");
    check(thread_stop_tid(buffer_owner_tid, 0) == 0, "stop shared-buffer owner");
    thread_reap_zombies();
    if (syscall_is_write) {
        for (unsigned int i = 0; i < 2; i++) {
            service_tx_irq();
            thread_yield();
        }
        check(io_waiter_done, "writer failed to stop at expired buffer");
        uart_tx_wait_idle();
        check(tx_count == RINGBUF_CAPACITY + 2U && tx[RINGBUF_CAPACITY] == '\r' &&
              tx[RINGBUF_CAPACITY + 1U] == '\n', "sleeping write reread an expired newline");
    } else {
        uint64_t status = local_irq_save();
        rx[0] = 'r';
        rx_count = 1;
        uart_irq_top();
        local_irq_restore(status);
        uart_rx_run_bottom_half();
        thread_yield();
        check(io_waiter_done, "reader failed to reject expired buffer");
    }
}

static void blocking_reader(void *arg)
{
    (void)arg;
    check(uart_recv() == 'r', "sleeping RX reader got the wrong byte");
    io_waiter_done = 1;
}

static void rx_producer(void *arg)
{
    struct thread *reader = thread_test_find(io_waiter_tid);
    uint64_t status;
    (void)arg;

    check(reader && reader->state == THREAD_WAITING && !io_waiter_done && !reads,
          "RX reader polled instead of letting its producer run");
    if (cancel_reader) {
        check(thread_stop_tid(io_waiter_tid, 0) == 0, "stop RX sleeper");
        thread_reap_zombies();
        check(!uart_rx_wait.head && !uart_rx_wait.tail, "cancelled RX waiter still linked");
    }
    rx[0] = 'r';
    rx_count = 1;
    status = local_irq_save();
    uart_irq_top();
    local_irq_restore(status);
    uart_rx_run_bottom_half();
    thread_yield();
    check(io_waiter_done == !cancel_reader, "RX wake or cancellation failed");
}

static void blocking_writer(void *arg)
{
    (void)arg;
    uart_send('b');
    io_waiter_done = 1;
}

static void tx_consumer(void *arg)
{
    struct thread *writer = thread_test_find(io_waiter_tid);
    (void)arg;

    check(writer && writer->state == THREAD_WAITING && !io_waiter_done && !tx_count && !reads,
          "full-ring writer polled instead of letting its consumer run");
    service_tx_irq();
    thread_yield();
    check(io_waiter_done && !uart_tx_wait.head && !uart_tx_wait.tail, "TX writer was not woken");
    uart_tx_wait_idle();
    check(!busy_writes && tx_count == RINGBUF_CAPACITY + 1U && tx[RINGBUF_CAPACITY] == 'b',
          "TX sleep reordered or lost the new byte");
    for (unsigned int i = 0; i < RINGBUF_CAPACITY; i++)
        check(tx[i] == 'a', "TX sleep reordered buffered bytes");
}

void test_uart_waits(void)
{
    uint64_t status = local_irq_save();
    uint64_t sources;

    asm volatile("csrrw %0, sie, zero" : "=r"(sources) : : "memory");
    for (cancel_reader = 0; cancel_reader < 2; cancel_reader++) {
        reset_model();
        uart_irq_enabled = 1;
        ier = UART_IER_RX_ENABLE;
        io_waiter_done = 0;
        io_waiter_tid = thread_create_kernel(blocking_reader, 0);
        check(io_waiter_tid > 0 && thread_create_kernel(rx_producer, 0) > 0,
              "create RX wait fixture");
        thread_run_until_idle();
        check(!uart_rx_wait.head && !uart_rx_wait.tail && !thread_has_live_workers(),
              "RX sleep fixture leaked a waiter");
    }
    reset_model();
    uart_irq_enabled = 1;
    ier = UART_IER_RX_ENABLE;
    io_waiter_done = 0;
    for (unsigned int i = 0; i < RINGBUF_CAPACITY; i++)
        check(ringbuf_push(&uart_tx_buf, 'a') == 0, "fill TX wait fixture");
    io_waiter_tid = thread_create_kernel(blocking_writer, 0);
    check(io_waiter_tid > 0 && thread_create_kernel(tx_consumer, 0) > 0,
          "create TX wait fixture");
    thread_run_until_idle();

    for (syscall_is_write = 0; syscall_is_write < 2; syscall_is_write++) {
        uintptr_t stack;
        struct thread *owner;
        struct thread *task;

        reset_model();
        uart_irq_enabled = 1;
        ier = UART_IER_RX_ENABLE;
        io_waiter_done = 0;
        stack = (uintptr_t)kmalloc(USER_STACK_SIZE);
        buffer_owner_tid = user_process_create(USER_CODE_BASE, stack, stack + USER_STACK_SIZE);
        owner = thread_test_find(buffer_owner_tid);
        check(owner != 0, "create buffer owner");
        /* This owner stays parked until the producer terminates it. */
        local_irq_disable();
        sched_remove(owner);
        owner->state = THREAD_WAITING;
        local_irq_enable();
        shared_buffer = (char *)stack;
        shared_buffer[0] = syscall_is_write ? '\n' : '?';
        shared_buffer[1] = 'z';
        stack = (uintptr_t)kmalloc(USER_STACK_SIZE);
        io_waiter_tid = user_process_create(USER_CODE_BASE, stack, stack + USER_STACK_SIZE);
        task = thread_test_find(io_waiter_tid);
        check(task != 0, "create shared-buffer syscall task");
        task->tgid = owner->tgid;
        task->entry = blocking_syscall;
        if (syscall_is_write) {
            for (unsigned int i = 0; i < RINGBUF_CAPACITY; i++)
                check(ringbuf_push(&uart_tx_buf, 'a') == 0, "fill syscall TX fixture");
        }
        check(thread_create_kernel(expire_shared_buffer, 0) > 0, "create buffer expiry producer");
        thread_run_until_idle();
    }
    local_irq_disable();
    uart_reset_runtime_state();
    model_active = 0;
    asm volatile("csrw sie, %0" : : "r"(sources) : "memory");
    local_irq_restore(status);
}

const char *test_uart_races(void)
{
    uint64_t status = local_irq_save();
    uint64_t sources;
    const char *error = 0;
    char c;

    /* Only the controlled arrivals below may interrupt these scenarios. */
    asm volatile("csrrw %0, sie, zero" : "=r"(sources) : : "memory");
#define VERIFY(cond, message) do { if (!(cond)) { error = message; goto out; } } while (0)

    /* RX top only masks and raises. More than one budget must survive until
     * the bottom half drains it, including a TX IRQ on the shared handler.
     * The 32-byte model is a controlled backlog, not the board FIFO depth.
     */
    reset_model();
    ier = UART_IER_RX_ENABLE | UART_IER_TX_ENABLE;
    for (unsigned int i = 0; i < sizeof(rx); i++)
        rx[i] = (unsigned char)('A' + i % 26);
    rx_count = sizeof(rx);
    VERIFY(ringbuf_push(&uart_tx_buf, '!') == 0, "TX interleave fixture");
    local_irq_disable();
    uart_irq_top();
    VERIFY(!local_irqs_enabled() && rx_head == 0 && rx_count == sizeof(rx) &&
           !uart_rx_buf.count && !uart_rx_irq_count && !tx_count &&
           !(ier & UART_IER_RX_ENABLE) && !(ier & UART_IER_TX_ENABLE) &&
           bottom_halves_pending(), "UART top moved bytes or failed to mask/defer");
    local_irq_enable();
    clear_bottom_half(BH_UART_RX);
    uart_rx_run_bottom_half();
    VERIFY(rx_head == UART_DEFERRED_WORK_BUDGET &&
           uart_rx_buf.count == UART_DEFERRED_WORK_BUDGET &&
           rx_count == sizeof(rx) - UART_DEFERRED_WORK_BUDGET &&
           !(ier & UART_IER_RX_ENABLE) && bottom_halves_pending(),
           "RX bottom lost its budget, continuation or source mask");
    /* TX can bring us through uart_irq_top again while RX is masked. */
    local_irq_disable();
    uart_tx_unmask();
    uart_irq_top();
    VERIFY(rx_head == UART_DEFERRED_WORK_BUDGET &&
           rx_count == sizeof(rx) - UART_DEFERRED_WORK_BUDGET,
           "shared TX handler consumed RX bytes in top half");
    local_irq_enable();
    run_bottom_halves();
    VERIFY(!rx_count && uart_rx_buf.count == sizeof(rx) && !empty_reads &&
           !busy_writes && tx_count == 1 && tx[0] == '!' &&
           (ier & UART_IER_RX_ENABLE) && !(ier & UART_IER_TX_ENABLE) &&
           !bottom_halves_pending(), "deferred RX/TX failed to drain and rearm");
    for (unsigned int i = 0; i < sizeof(rx); i++)
        VERIFY(ringbuf_pop(&uart_rx_buf, &c) == 0 && c == (char)('A' + i % 26),
               "deferred RX backlog changed FIFO order");

    reset_model();
    ier = UART_IER_RX_ENABLE;
    rx[0] = 'h';
    rx_count = 1;
    local_irq_disable();
    uart_irq_top();
    /* A second byte arrives while the RX source is masked, with no new IRQ. */
    rx[1] = 'i';
    rx_count++;
    local_irq_enable();
    /* One more byte arrives exactly when the source is re-enabled. */
    inject_on_rx_unmask = 1;
    run_bottom_halves();
    VERIFY(delivered == 1 && !rx_count && !empty_reads && uart_rx_buf.count == 3 &&
           (ier & UART_IER_RX_ENABLE) && !bottom_halves_pending(),
           "RX arrival during masking/rearm was stranded or read twice");
    for (unsigned int i = 0; i < 3; i++)
        VERIFY(ringbuf_pop(&uart_rx_buf, &c) == 0 && c == "hiq"[i],
               "masked/rearm RX arrival order changed");

    reset_model();
    rx[0] = 'p';
    rx_count = 1;
    arrival = 1;
    uart_rx_run_bottom_half();
    VERIFY(delivered == 1 && !empty_reads && uart_rx_buf.count == 1,
           "RX bottom half consumed an empty RBR after a nested UART IRQ");
    VERIFY(ringbuf_pop(&uart_rx_buf, &c) == 0 && c == 'p', "RX byte changed");
    VERIFY(local_irqs_enabled(), "RX bottom half left IRQs disabled");

    reset_model();
    rx[0] = 'h'; rx[1] = 'e'; rx[2] = 'l'; rx[3] = 'p';
    rx_count = 4;
    arrival = 1;
    uart_rx_run_bottom_half();
    VERIFY(delivered == 1 && !empty_reads && uart_rx_buf.count == 4,
           "RX interleaving lost or duplicated bytes");
    for (unsigned int i = 0; i < 4; i++)
        VERIFY(ringbuf_pop(&uart_rx_buf, &c) == 0 && c == "help"[i], "RX FIFO order changed");

    reset_model();
    rx[0] = 'p';
    rx_count = 1;
    arrival = 1;
    c = uart_recv();
    VERIFY(c == 'p' && delivered == 1 && !empty_reads && !uart_rx_buf.count,
           "RX polling read an empty RBR after a nested UART IRQ");

    reset_model();
    arrival = inject_byte = 1;
    c = uart_recv();
    VERIFY(c == 'p' && delivered == 1 && !empty_reads && !uart_rx_buf.count && reads < 1000,
           "RX polling missed the byte moved into the ring while waiting");

    reset_model();
    arrival = inject_tx = 1;
    uart_send('x');
    VERIFY(delivered == 1 && !busy_writes && tx_count == 2 && tx[0] == 'x' && tx[1] == '!',
           "TX polling wrote to a busy THR after an interrupt");
    VERIFY(local_irqs_enabled(), "TX polling left IRQs disabled");

    reset_model();
    uart_irq_enabled = 1;
    ier = UART_IER_RX_ENABLE;
    VERIFY(uart_write_async("", 0) == 0 && !(ier & UART_IER_TX_ENABLE),
           "Empty TX submission armed an interrupt");
    VERIFY(uart_write_async("a", 1) == 0, "TX fixture enqueue failed");
    uart_send('b');
    VERIFY(!tx_count && !reads && uart_tx_buf.count == 2 &&
           (ier & UART_IER_TX_ENABLE),
           "Normal TX polled or wrote THR before its first interrupt");
    service_tx_irq();
    service_tx_irq();
    VERIFY(!busy_writes && tx_count == 2 && tx[0] == 'a' && tx[1] == 'b' &&
           !uart_tx_buf.count && !(ier & UART_IER_TX_ENABLE),
           "TX interrupt failed to drain FIFO or disarm an empty ring");
    uart_send('c');
    VERIFY(tx_count == 2 && (ier & UART_IER_TX_ENABLE),
           "TX restart bypassed interrupts");
    service_tx_irq();
    VERIFY(tx_count == 3 && tx[2] == 'c' && !(ier & UART_IER_TX_ENABLE),
           "TX restart lost or duplicated a byte");
    VERIFY(!uart_tx_poll_count && uart_tx_irq_count == 3,
           "Normal TX used the polling fallback");

    reset_model();
    uart_irq_enabled = 1;
    ier = UART_IER_RX_ENABLE;
    tx_busy_reads = 2;
    VERIFY(uart_write_async("a", 1) == 0, "TX fixture enqueue failed");
    uart_send('b');
    VERIFY(!tx_count && !reads && uart_tx_buf.count == 2,
           "TX producer polled a busy transmitter");
    local_irq_disable();
    uart_send('c');
    VERIFY(!local_irqs_enabled() && !uart_tx_buf.count && !busy_writes &&
           tx_count == 3 && tx[0] == 'a' && tx[1] == 'b' && tx[2] == 'c',
           "IRQ-off TX overtook queued bytes or changed SIE");

    /* A full-ring producer in a callback must not wait for this same runner. */
    reset_model();
    uart_irq_enabled = 1;
    for (unsigned int i = 0; i < RINGBUF_CAPACITY; i++)
        VERIFY(ringbuf_push(&uart_tx_buf, 'a') == 0, "Full TX fixture enqueue failed");
    uart_send('b');
    VERIFY(tx_count == 1 && tx[0] == 'a' && uart_tx_buf.count == RINGBUF_CAPACITY,
           "Full TX ring failed to make room in FIFO order");
    uart_tx_wait_idle();
    VERIFY(tx_count == RINGBUF_CAPACITY + 1U && tx[RINGBUF_CAPACITY] == 'b' &&
           !busy_writes && !uart_tx_buf.count, "Full TX flush lost output");
    for (unsigned int i = 0; i < RINGBUF_CAPACITY; i++)
        VERIFY(tx[i] == 'a', "Full TX flush reordered output");

    reset_model();
    uart_irq_enabled = 1;
    VERIFY(ringbuf_push(&uart_rx_buf, 'r') == 0, "RX fixture enqueue failed");
    VERIFY(uart_recv() == 'r' && !reads && !uart_rx_fallback_count,
           "IRQ-enabled RX polled before the first UART interrupt");

    reset_model();
    local_irq_disable();
    rx[0] = 'z';
    rx_count = 1;
    VERIFY(uart_recv() == 'z' && !local_irqs_enabled(), "IRQ-off RX changed SIE");
    uart_send('z');
    VERIFY(!local_irqs_enabled() && tx_count == 1 && !busy_writes, "IRQ-off TX changed SIE");

out:
    local_irq_disable();
    pending_irq = 0;
    uart_reset_runtime_state();
    uart_irq_enabled = 0;
    model_active = 0;
    asm volatile("csrw sie, %0" : : "r"(sources) : "memory");
    local_irq_restore(status);
    return error;
#undef VERIFY
}
