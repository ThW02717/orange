#include "user_api.h"

/* The raw loader copies .data but does not create a per-process BSS mapping.
 * Keep these test globals in the file, and reset them before each run.
 */
static long leader_pid = -1;
static long leader_leaving = -1;
static long shared_global = -1;
static long fork_release = -1;

struct shared_args {
    long pid;
    long tid;
    long ready;
    long release;
    char message[40];
};

static long load(const long *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void store(long *p, long value) { __atomic_store_n(p, value, __ATOMIC_RELEASE); }

static void puts_user(const char *s)
{
    long n = 0;
    while (s[n] != '\0') { n++; }
    (void)uart_write(s, n);
}

static void require(int ok, const char *reason)
{
    if (!ok) {
        puts_user("[threads] FAIL: ");
        puts_user(reason);
        puts_user("\n");
        exit_group(1);
    }
}

static void wait_for(const long *p, long value)
{
    /* Let timer preemption do the switching; there is no yield syscall. */
    for (unsigned long i = 0; i < 500000000UL; i++) {
        if (load(p) == value) { return; }
    }
    require(0, "timed out waiting for sibling");
}

static void shared_worker(void *arg)
{
    struct shared_args *shared = arg;

    require(getpid() == shared->pid && gettid() != shared->pid, "sibling identity");
    shared->tid = gettid();
    /* This buffer belongs to the live parent stack, not to our own stack. */
    require(uart_write(shared->message, 36) == 36, "shared stack syscall buffer");
    store(&shared_global, 42);
    store(&shared->ready, 1);
    wait_for(&shared->release, 1);
    /* Test the return-to-exit stub, not an explicit thread_exit() call. */
}

static void parked_worker(void *arg)
{
    (void)arg;
    for (;;) { asm volatile("" ::: "memory"); }
}

static void surviving_worker(void *arg)
{
    (void)arg;
    wait_for(&leader_leaving, 1);
    require(getpid() == leader_pid && gettid() != leader_pid, "group survives leader");
    require(thread_create_user(parked_worker, 0) > 0, "create from nonleader");
    puts_user("[threads] leader exit and nonleader create: PASS\n");
    puts_user("[threads] stop group by original PID\n");
    (void)stop(leader_pid);
    require(0, "group stop returned to a stopped thread");
}

void threads_main(void)
{
    struct shared_args shared = {
        .pid = getpid(), .tid = -1, .ready = 0, .release = 0,
        .message = "[threads] shared stack buffer: PASS\n"
    };
    long tid;
    long child = -1;

    leader_pid = getpid();
    store(&leader_leaving, 0);
    store(&shared_global, 0);
    store(&fork_release, 0);
    require(getpid() == gettid(), "first thread identity");
    require(thread_create_user(0, 0) == -1, "null entry accepted");
    require(user_syscall(SYS_thread_create, (long)shared_worker | 1, 0,
                         (long)user_thread_return) == -1, "unaligned entry accepted");
    require(user_syscall(SYS_thread_create, (long)shared_worker, 0, 0) == -1,
            "null return stub accepted");
    require(user_syscall(SYS_thread_create, (long)&shared, 0,
                         (long)user_thread_return) == -1, "stack entry accepted");
    puts_user("[threads] invalid entry/return: PASS\n");

    tid = thread_create_user(shared_worker, &shared);
    require(tid > 0 && tid != gettid(), "new TID");
    require(stop(tid) == -1, "stop interpreted a nonleader TID as a process ID");
    require(fork() == -1, "multithreaded fork accepted");
    require(exec("test") == -1, "multithreaded exec accepted");
    wait_for(&shared.ready, 1);
    require(shared.tid == tid && load(&shared_global) == 42, "shared state");
    puts_user("[threads] same PID, different TID, shared global: PASS\n");
    puts_user("[threads] multithreaded fork/exec rejection: PASS\n");
    store(&shared.release, 1);

    /* No join API yet. Fork stays rejected until the returning sibling has
     * exited, then succeeds with one live thread and a new process ID.
     */
    for (unsigned long i = 0; i < 1000000UL && child < 0; i++) { child = fork(); }
    require(child >= 0, "returning worker did not exit");
    if (child == 0) {
        require(getpid() == gettid() && getpid() != shared.pid, "fork identity");
        puts_user("[threads] fork gets a new PID/TID: PASS\n");
        wait_for(&fork_release, 1);
        require(thread_create_user(parked_worker, 0) > 0, "fork child sibling");
        puts_user("[threads] exit_group with a live sibling\n");
        exit_group(0);
    }
    require(exec("test") == -1, "exec overwrote another process's live image");
    puts_user("[threads] cross-process image replacement rejected: PASS\n");
    store(&fork_release, 1);
    puts_user("[threads] returning worker and single-thread fork: PASS\n");
    require(thread_create_user(surviving_worker, 0) > 0, "surviving worker");
    store(&leader_leaving, 1);
    thread_exit(0);
}
