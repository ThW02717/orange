# Orange

An educational RV64 kernel for OrangePi RV2, written in C and RISC-V assembly.

## Reading the code

Start at the entry points below. File-local helpers stay `static`; the
`*_internal.h` headers are shared implementation contracts, not user APIs.

| What to trace | File | Start here |
| --- | --- | --- |
| Boot and hardware discovery | `kernel.c`, `platform.c` | `kernel_main()`, `platform_init()` |
| Kernel console | `shell.c` | `shell_run()` -> `shell_readline()` -> `shell_execute()` |
| Raw user image loading | `exec.c` | `exec_spawn()`, `exec_load_image()` |
| Thread creation and lifetime | `thread.c` | `thread_create_kernel()`, `thread_create_user()`, `thread_mark_current_zombie()` |
| CPU scheduling | `sched.c` | `schedule()`, `thread_consume_resched_request()` |
| Trap entry, dispatch and return | `trap_entry.S`, `trap.c` | `trap_entry`, `trap_dispatch()`, `trap_return` |
| User syscalls | `syscall.c` | `handle_user_ecall()` -> `sys_*()` |
| Deferred work | `deferred_work.c` | `deferred_work_enqueue()`, `deferred_work_add()`, `irq_run_bottom_halves()` |
| Timer and UART | `timer.c`, `uart.c` | `timer_irq_top()`, `uart_irq_top()` and their callbacks |
| Physical page allocation | `memory.c` | `memory_init()`, `p_alloc()`, `p_free()` |
| Byte allocation | `slab.c` | `kmalloc()`, `kfree()` |
| Tests and event history | `demo.c`, `demo_uart.c`, `trace.c` | `demo_run()`, `trace_dump()` |

```text
shell / syscall
    +-- exec.c ----------> thread.c <----> sched.c --> switch_to.S
    +-- kmalloc/kfree ---> slab.c -------> memory.c (buddy)

trap_entry.S --> trap.c --> timer / PLIC + UART top half
                           |
                           v
                      deferred_work.c --> timer / UART callbacks

platform.c --> configure UART + PLIC; resolve one boot/initramfs description
irqflags.h --> local SIE save/restore, shared by kernel critical sections
trace.c    --> event storage; demos read it, drivers do not call demo code
```

The deferred-work queue holds short callbacks; the scheduler run queue holds
runnable threads. Timer and UART top halves enqueue persistent work items;
`deferred_work_add()` queues one-shot custom work, including the `demo tasks`
ordering test. Local IRQ masking protects both queues on the scheduling hart;
it is not an SMP lock or kernel-preemption mechanism.

## Verification

```sh
make -C kernel all
make -C bootloader all
python3 -B scripts/run_allocator_tests.py
python3 -B scripts/run_regression_tests.py
python3 -B scripts/run_system_tests.py
python3 -B scripts/run_bootloader_tests.py
```

The regression runner checks malformed CPIO/DTB inputs with host ASan/UBSan,
then exercises integer-register trap round trips, scheduler reclamation,
thread groups, allocation-failure rollback (including partially created kernel demos), priority/FIFO ordering, callback
non-reentrancy during nested traps, callback-pool exhaustion/reuse, timer
payload cleanup, and masked UART polling in RV64 QEMU.
The system runner boots the real QEMU kernel and its separately linked user
images to test syscalls, faults, fork, same-process user threads, preemption,
timers, and allocator demos. It also checks real QEMU UART/PLIC interrupts,
nonblocking timeouts, the two-second timer demo, and all five ecalls in the
supplied Lab 4 program.
The bootloader runner sends a real bundle through QEMU UART, checks the initrd
hint fallback, and launches a user image from the received archive.
These checks do not validate OrangePi UART/PLIC wiring or the physical upload path.
Dependencies: a RISC-V bare-metal toolchain, QEMU, Python 3, a host C compiler,
`dtc`, `cpio`, and `mkimage` (for the board image).

## Core Design

### Memory Layout

The kernel uses physical addresses directly. There are no per-process page
tables: kernel code, user images, stacks, and allocator storage share one flat
address space. Separate stack allocations do not provide memory isolation.
The addresses below refer to the board build, not the QEMU build.

#### Physical RAM and reservations

[`memory_init()`](kernel/src/memory.c) reads RAM regions from the DTB rather than
assuming one fixed RAM range. Only complete 4 KiB pages inside those regions are
tracked. Reservations are rounded outward to keep any occupied page out of the
initial buddy free lists.

```text
RAM declared by the DTB (one or more regions)
|
+-- Reserved before buddy free-list creation
|   +-- Physical page 0, if it falls within declared RAM
|   +-- Kernel image, including static storage and the boot stack
|   +-- DTB
|   +-- Initramfs archive
|   +-- Fixed user-code execution window
|   +-- DTB header memory-reservation entries
|   +-- Board /reserved-memory ranges
|   `-- Frame metadata array allocated by the startup allocator
|
`-- Remaining complete pages
    +-- Free buddy blocks
    `-- Runtime allocations
        +-- Slab backing blocks and other kernel objects
        +-- Per-thread kernel stacks
        `-- Per-user-task user stacks

Accounting view, not physical address order; blocks need not be adjacent.
```

`regions` describes RAM; `reserves` records occupied ranges. Neither array
is itself the memory it describes. Both are static kernel storage. Likewise,
the `frame_array` pointer is static, but the metadata array it points to is
allocated during startup and reserved before buddy initialization.

The initramfs is a CPIO archive containing files, including user binaries. Its
storage is reserved; a user binary is copied elsewhere before execution. UART
and PLIC registers are MMIO, not allocator-managed RAM.

#### Kernel image

[`kernel/linker.ld`](kernel/linker.ld) places the board kernel at `0x20000000`
and defines the image boundaries. [`boot.S`](kernel/src/boot.S) sets `sp` to
`_stack_top` and clears `[__bss_start, __bss_end)` before calling `kernel_main()`.

```text
Higher addresses

_phys_end = _stack_top
             +--------------------------------------------+
             | Boot stack: 16 KiB                         |
             | Used by bootstrap kernel / shell execution |
             | Grows toward lower addresses               |
             +--------------------------------------------+
             | Alignment padding, if needed               |
__bss_end    +--------------------------------------------+
             | .bss, including .sbss and COMMON           |
             | Zero-initialized globals and static arrays |
             | Includes secondary-hart stacks             |
__bss_start  +--------------------------------------------+
             | .data / .sdata: initialized writable data  |
             +--------------------------------------------+
             | .rodata / .srodata: constants              |
             +--------------------------------------------+
             | .text: kernel instructions                 |
             | .text.boot is kept at the beginning        |
_phys_start  +--------------------------------------------+
0x20000000

Lower addresses
```

Section sizes and alignment gaps depend on the linked build. The whole
`[_phys_start, _phys_end)` range is reserved, including the boot stack.
Compiler-generated small-data sections are grouped with their corresponding
data sections in this diagram.

The first scheduler initialization registers the existing shell flow as the
bootstrap idle thread (TID 0). It keeps using the boot stack; initialization is
idempotent and does not reset live tasks or the TID counter. Shell command
iterations do not create threads or assign process IDs. Secondary harts use separate static 4 KiB stacks in `.bss`; they do not
participate in the scheduler. Scheduling runs on the bootstrap hart.

#### User image and per-task storage

User programs are linked at `0x32000000`, converted from ELF to raw `.bin`
files, and packed into initramfs. The
[`raw-image loader`](kernel/src/exec.c) copies `bin/<name>.bin` into the fixed
1 MiB window `[0x32000000, 0x32100000)`. The entry address is `0x32000000`;
the window size is a loading limit, not the size of every program.

Each scheduled user task has a separate control block, kernel stack, and user
stack. Their addresses come from the allocator, not from a fixed per-process
virtual memory map.

```text
Kernel-owned task resources                Shared user-image window

Task A                                     0x32000000
+-- struct thread A                        +--------------------------+
+-- Kernel stack A: 16 KiB                 | One loaded raw image     |
+-- User stack A: 64 KiB                   | Entry at the window base |
`-- user_entry --------------------------->|                          |
                                           | Unused window capacity   |
Task B                                     +--------------------------+
+-- struct thread B                        0x32100000 (exclusive end)
+-- Kernel stack B: 16 KiB
+-- User stack B: 64 KiB
`-- user_entry ---------------------------> function in the same image

Resource relationships, not physical address order or separate address spaces.
```

Kernel-only worker threads have a control block and a 16 KiB kernel stack, but
no user stack. All these allocations remain kernel-managed and are reclaimed
after the thread exits, once it is no longer executing on its own stack.

This is not an ELF process loader. It does not construct per-process text,
data, BSS, heap, or TLS mappings. The shared image window is reserved before
buddy initialization. The loader rejects image replacement while another live
user task uses the window; this prevents accidental overwrite, not unauthorized
access or per-process isolation.

#### Allocator boundary

`memory.c` owns RAM regions, reservations, frame metadata and buddy lists.
`slab.c` owns size-class caches, slab headers and object freelists. It queries
page ownership through `memory_internal.h`; it does not modify the buddy's
private frame array directly.

```text
p_alloc(pages)  --> buddy block (page count rounded to a power of two)

kmalloc(bytes)
    +-- 1..2048  --> class 16/32/64/128/256/512/1024/2048
    |               one 4 KiB page per slab, including header and padding
    `-- >2048    --> round bytes to pages --> p_alloc()

kfree(ptr)      --> page owner tag --> slab object or large buddy allocation
p_free(ptr)     --> original buddy allocation head only
```

The current RV64 slab header is 48 bytes. There is no multi-page slab path;
large byte requests still work through `kmalloc()`. Each cache retains at most
one empty slab. Extra empty slabs return to buddy. The double-free check scans
the object freelist, so the whole `kfree()` path is not constant-time merely
because list insertion is O(1).

### Execution Contexts

A `struct thread` is the scheduler's control block, not a register snapshot or
a stack. It holds identity, state, queue links, stack addresses, and two
different kinds of saved execution state.

| State | Storage | Contents | Save / restore boundary |
| --- | --- | --- | --- |
| `thread_context` | Inline in `struct thread` | `ra`, `sp`, `s0`-`s11` (112 bytes) | Kernel function-call boundary in `switch_to()` |
| `trap_context` | On a kernel stack | Slots for 31 integer registers and `sepc`, `sstatus`, `scause`, `stval` (280 bytes) | Trap entry and return; also seeded for first user entry |

#### Control block and kernel stack

[`struct thread`](kernel/include/thread.h) embeds `thread_context` as its first
field, so assembly can use the thread pointer as the saved-context base.
`tc` is a pointer into the user task's kernel stack, not another embedded
context. User-origin traps use its top-of-stack slot; supervisor-origin traps
allocate a context below the current kernel `sp`.

```text
struct thread (kernel allocation)
+----------------------------------------+
| thread_context                         |  Inline; not on the kernel stack
|   ra, sp, s0 ... s11                   |
+----------------------------------------+
| tid, tgid, kind, state, queue links    |
| kstack_base, kstack_top                | --> Kernel stack allocation
| user_stack_base, user_stack_top        | --> User stack allocation
| tc                                     | --> Top-of-stack trap-context slot
+----------------------------------------+
```

For a newly created user task, let `K` be its kernel-stack base. The stack is
16 KiB, and the 280-byte context occupies a 288-byte slot to preserve 16-byte
stack alignment. Kernel calls and deferred callbacks share this stack with
nested hardware-interrupt contexts. Callbacks need bounded stack use; there
is no MMU guard page.

```text
Higher addresses

K + 16384 +-----------------------------------------+  kstack_top
          | Alignment padding: 8 bytes              |
K + 16376 +-----------------------------------------+
          | trap_context: 280 bytes                 |
          | Integer register slots + four CSRs      |
K + 16096 +-----------------------------------------+  tc
          | Initial thread_context.sp points here   |
          |                                         |
          | Kernel call frames and local variables  |
          | Stack grows toward lower addresses      |
K         +-----------------------------------------+  kstack_base

Lower addresses
```

The stack is working storage for kernel calls. The contexts are register
snapshots. Neither snapshot is updated continuously as instructions execute.
The diagram shows the initial user-task layout, not the position of every
nested supervisor trap.

#### Why two contexts

[`switch_to()`](kernel/src/switch_to.S) is called by kernel code. It saves the
callee-saved registers plus `ra` and `sp`, restores the next thread's values,
sets `tp` to that thread, and returns into its saved kernel continuation.
Caller-saved registers do not need a second copy at this function-call boundary.

A trap can interrupt code between arbitrary instructions. Its context needs
slots for caller-saved registers as well, plus the return PC and privilege
state. The CPU supplies trap information in CSRs; assembly creates the memory
snapshot.

A privilege transition is not necessarily a task switch. If A is suspended
inside the trap handler, its trap context remains on A's kernel stack while
`thread_context` records where its kernel execution should resume. The
[trap return flow](#trap-return-and-task-switching) shows both paths.

#### First user entry and register convention

[`thread_prepare_user()`](kernel/src/thread.c) builds both initial contexts.
It is shared by process launch, same-process thread creation, and fork:

- `thread_context.ra = thread_bootstrap`; `thread_context.sp = kstack_top - 288`.
- `tc->sp = user_stack_top`; `tc->tp = thread`; `tc->sepc = user_entry`.
- `tc->sstatus = SSTATUS_SPIE`: initial `SPP = 0`, `SPIE = 1`.

The caller finishes any operation-specific fields before
`thread_publish()` makes the task runnable. A new user thread also gets
its argument in `a0` and a user-space return-to-exit stub in `ra`. Fork instead
copies the parent's interrupted context and adjusts it to resume after `ecall`.

Assembly offsets live in `context_offsets.h`; `context_layout.c` checks every
register offset and both structure sizes at build time.

The first switch follows this path without requiring a previous user trap:

```text
switch_to() -> thread_bootstrap() -> user_task_entry()
            -> trap_return(tc) -> sret -> user entry
```

In this kernel, `tp` points to the current `struct thread` in both U-mode and
S-mode; user TLS is not implemented. Trap entry selects the stack and saves
the interrupted `sp`; the [trap handling diagrams](#trap-handling) cover entry,
deferred work, and return.

### Processes and Threads

The scheduler still schedules `struct thread`, not a separate process object.
Each task has a unique `tid`. User threads in one process have the same `tgid`;
`getpid()` returns that group ID and `gettid()` returns the calling thread's ID.
The first thread has `tid == tgid`. Its exit does not change its siblings' TGID.

There is no `struct process` or `mm_struct` yet. Group membership is recorded in
each task and checked by scanning the task list. The one user-image window is
global reserved storage, not an allocation owned by a group. Adding a group ID
does not turn it into a private address space.

Group counting and stack-range checks are O(number of tasks). This keeps the
small single-hart scheduler's ownership model simple; it is not a scalable
process-resource or address-space manager.

```text
Example identities, not fixed boot-time IDs

Process ID 10                         Process ID 13 (fork child)
+-- Thread: TID 10, TGID 10            +-- Thread: TID 13, TGID 13
|   +-- user stack                    |   +-- copied/rebased user stack
|   +-- kernel stack + trap_context   |   +-- new kernel stack + trap_context
|   `-- thread_context               |   `-- thread_context
|
`-- Thread: TID 11, TGID 10
    +-- new user stack                Both groups still point into ONE
    +-- kernel stack + trap_context   physical user-image window.
    `-- thread_context                There is no process memory isolation.
```

Threads in a group share the loaded code and globals and can pass pointers
into each other's live user stacks. Each thread keeps its own stacks,
register contexts, and scheduler state. There is no user heap, TLS runtime,
file-descriptor table, or pthread implementation.

#### Four different creation paths

| Entry point | Identity | Initial execution | User stack |
| --- | --- | --- | --- |
| `thread_create_kernel(fn, arg)` | New TID, TGID = TID | Calls `fn(arg)` in S-mode | None |
| `user_process_create(entry, base, top)` | New TID, TGID = TID | Starts at the loaded image entry in U-mode | Supplied by the kernel caller |
| `thread_create_user(entry, arg, return_pc)` | New TID, current user's TGID | Starts a new function in U-mode | Newly allocated; not copied |
| User `fork()` -> `sys_fork(tc)` | New TID, TGID = TID | Continues after the parent's `ecall` | Copied and heuristically rebased |

The first three names are kernel-side functions in
[`thread.c`](kernel/src/thread.c). Syscall dispatch, user-buffer checks, and
fork are in [`syscall.c`](kernel/src/syscall.c).
[`trap.c`](kernel/src/trap.c) classifies traps and forwards user ecalls to
`handle_user_ecall(tc)`. The user-side `thread_create_user(fn, arg)` wrapper
has two arguments. It supplies the return stub as the syscall's third argument;
it does not call the kernel C function directly.

A kernel worker is independent scheduled S-mode work. Entering a syscall or
handling a trap does not create one: the interrupted user thread executes the
kernel path using its own kernel stack.

#### Loading the first user process

```text
Boot -> kernel shell
          |
          | runu threads
          v
exec_spawn("threads", 1, &result)
          | exec_load_image("threads", ...)
          | find bin/threads.bin in initramfs
          | reject replacement if another live user task uses the window
          | copy the image and synchronize instruction fetch
          v
allocate user stack
          v
user_process_create(entry, stack_base, stack_top)
          | thread_prepare_user(): allocate task + kernel stack, seed contexts
          | keep TGID = new TID
          | thread_publish(): add the finished task to the run queue
          v
schedule -> switch_to -> user_task_entry -> trap_return -> sret
          v
_start in the loaded user program
```

`user_process_create()` does not load a binary. The shell calls `exec_spawn()`,
which loads it first and supplies its entry address and a user stack. This separates
launching the first process from creating a sibling thread. There is no
permanent user-space init process or user shell at boot.

`demo preempt` launches two process groups using the same loaded image.
`demo threads` launches one process, which then creates sibling threads.
These demonstrate different identity relationships even though both use the
same run queue and the same physical image window.

#### Creating a thread from U-mode

The small wrapper in [`user_api.h`](kernel/user/user_api.h) is enough for the
demo; it is not a libc or pthread API.

```c
static void worker(void *arg)
{
    /* Use arg while the object it points to is still alive. */
    /* Returning exits this thread, not its siblings. */
}

long tid = thread_create_user(worker, arg);
/* tid < 0 means creation failed. This is not a join handle. */
```

```text
User wrapper                           Kernel
thread_create_user(fn, arg)
  a0 = fn
  a1 = arg
  a2 = user_thread_return
  a7 = SYS_thread_create
         |
         `-- ecall ------------------> thread_create_user(...)
                                        | validate entry and return addresses
                                        | allocate fresh user/kernel stacks
                                        | new TID, parent's TGID
                                        | tc: sepc=fn, a0=arg, ra=return stub
                                        | tc: sp=new stack top, tp=new task
                                        `-- publish, return new TID to caller

When the new thread is scheduled:
  sret -> fn(arg) -> ret -> user_thread_return -> SYS_exit
```

The entry and return stub must be two-byte-aligned addresses inside the fixed
user-image window. This is a range check, not validation of executable ELF
segments or a security boundary. `gp` is inherited; `tp` remains the new
current-thread pointer rather than a TLS pointer.

#### Fork is not thread creation

```text
Parent: one live user thread
  fork()
    |
    +-- reject invalid stack state or a multithreaded group
    +-- allocate and copy the user stack
    +-- rebase stack-looking values in the copy
    +-- prepare an unpublished child with a new TID/TGID
    +-- copy trap_context; fix sp, stack pointers and tp
    +-- child a0 = 0; child sepc = parent's sepc + 4
    `-- publish the completed child

Parent resumes after ecall with a0 = child PID
Child resumes after ecall with a0 = 0
```

This fork copies the user stack and the interrupted register state. It does
not copy the kernel call stack, create page tables, implement copy-on-write,
or isolate globals. A copied integer that happens to look like a stack address
can be rebased incorrectly. The existing demo works within that limitation;
this is not general-purpose fork semantics.

Fork from a multithreaded group returns `-1`. Exec also rejects a multithreaded
group, and the loader rejects replacement while any other live user task uses
the image, even one in a different process. A successful exec keeps the
caller's TID/TGID and resets its user entry/context; it does not create a task.

#### Exit, ownership, and cleanup

- `SYS_exit` / user `thread_exit(status)`: end only the calling thread.
- Returning from a user worker: the return stub issues `SYS_exit(0)`.
- `SYS_exit_group` / `exit_group(status)`: end every user thread with the
  caller's TGID.
- `SYS_stop` / `stop(pid)`: stop that user process group, even if its original
  leader has already exited.
- Kernel `thread_stop_tid()`: stop one specific task; used for launch rollback.

Threads are detached: there is no join/wait syscall or retained exit-status
record after reclamation. Passing a pointer to a sibling does not extend the
pointed-to object's lifetime. In particular, do not use a thread's stack after
that thread exits. Syscall buffer checks accept live same-group stacks and
reject other groups' stacks; direct memory accesses remain unisolated.

```text
NEW (private allocation; not an I/O wait)
      |
      | constructor fills entry, stacks and contexts
      v
thread_publish() -> RUNNABLE <----> RUNNING
                                    |
                                    | yield / timer: return to RUNNABLE

RUNNING / RUNNABLE
      |
      | exit or stop; unlink from run queue if queued
      v
    ZOMBIE
      |
      | another stack reaps it (normally idle; also failed-launch cleanup)
      v
free user stack (if any) + kernel stack + struct thread
```

There is no blocked I/O state or wait queue yet. `NEW` only covers construction;
it must not be treated as a sleeping task. The shell still doubles as the idle
fallback, not a separately scheduled user shell.

The shared image window is never freed through `kfree()`. It stays reserved
and can be reused by the loader once no other live user task needs it.
Allocation failures free everything allocated by that failed operation and
leave no partial child runnable.

#### Try it

```text
demo threads
runu threads
demo fork
demo preempt
```

The threads demo checks same-PID/different-TID identities, shared globals and
a sibling's stack buffer, invalid entry addresses, multithreaded fork/exec
rejection, worker return, single-thread fork, cross-process image replacement
rejection, and both group-exit paths. It also stops a group after its leader
exits. The system runner repeats the launch and then loads another image to
check that cleanup releases the loader's busy condition.

### Trap Handling

The following paths run on the primary hart and cover traps routed to S-mode.
Timer and UART interrupts are asynchronous events. Exceptions come from the
instruction being executed, including deliberate user `ecall` instructions.

#### Trap entry and dispatch

[`trap_entry.S`](kernel/src/trap_entry.S) saves CPU state before calling
[`trap_dispatch()`](kernel/src/trap.c). The dispatcher handles interrupts and
exceptions; only returning handlers reach the deferred-work runner.

```text
Code running in U-mode or S-mode
                |
         Trap routed to S-mode
                |
                v
CPU records trap state
  sepc, scause, stval
  SPP  <- previous privilege
  SPIE <- SIE
  SIE  <- 0
                |
                v
trap_entry                         [trap_entry.S]
  U-origin: select kernel stack using sscratch
  S-origin: keep current kernel stack
  Save integer registers and trap CSRs
  Set sscratch = 0 for kernel execution
                |
                | a0 = trap_context address
                v
trap_dispatch(tc)                  [trap.c]
  |
  +-- Interrupt
  |     |
  |     +-- Timer
  |     |     Mask STIE and queue timer bottom half
  |     |     Request rescheduling if needed for U-origin
  |     |
  |     +-- External
  |     |     PLIC claim; for UART, handle device state
  |     |     and queue RX/TX work; PLIC complete
  |     |
  |     `-- Other interrupt: report the unhandled cause
  |
  +-- U-mode ecall
  |     Handle syscall, or the selected Lab diagnostic
  |
  +-- Other U-mode exception
  |     Report fault and mark the user task exited
  |
  `-- Other S-mode exception
        Report fatal kernel fault
        STOP -- does not return

Returning handlers
        |
        v
irq_run_bottom_halves()            [deferred_work.c]
        |
        v
Check origin, task exit and rescheduling
        |
        v
Restore a live context, or switch away
```

`sstatus.SIE` controls ordinary S-mode interrupts while executing in S-mode; it
does not mask synchronous exceptions. Individual sources have separate enables
in `sie`, including STIE for timers and SEIE for external interrupts. In U-mode,
SIE does not gate interrupts routed to S-mode; source enables still apply.

#### Nested interrupts and deferred work

A deferred task is a queued callback. It runs on the interrupted thread's
kernel stack, with hardware interrupts enabled outside protected sections.
It does not get a worker thread or a separate stack.

```text
Outer runner: deferred_work_running = 1
    |
    +-- Run callback A, rank 5
    |     SIE = 1 outside protected critical sections
    |     |
    |     +-- Hardware IRQ B arrives
    |     |      |
    |     |      CPU enters S-mode trap: SIE = 0
    |     |      Save A's interrupted context
    |     |      |
    |     |      Run B's top half
    |     |      Queue B's bottom half, rank 1
    |     |      |
    |     |      Call irq_run_bottom_halves()
    |     |      deferred_work_running is already 1
    |     |      Leave B queued and return
    |     |      |
    |     |      Restore A's context
    |     |      sret resumes A in S-mode
    |     |
    |     +-- A continues
    |     `-- A returns
    |
    +-- Update A's state with interrupts disabled
    |     Complete A, or requeue it if more work remains
    |
    +-- Select the highest-priority queued task
    |     B can now run
    |
    +-- Continue until the queue is empty
    |
    `-- deferred_work_running = 0

Priority: 0 is highest, 7 is lowest; equal ranks are FIFO.
Priority selects the next callback; it does not preempt one.
```

[`irq_run_bottom_halves()`](kernel/src/deferred_work.c) changes queue links and task
states with local interrupts disabled. A persistent timer/UART task stays
`RUNNING` while paused; another enqueue sets `rerun` instead of inserting the
same object twice. Queue priority and PLIC source priority are separate.

This follows the same-CPU non-reentrancy rule of non-PREEMPT_RT Linux softirqs.
P1 has no softirq bitmask, execution budget, or `ksoftirqd` worker. The Lab 4
extension for high-priority bottom-half preemption is not implemented by this
policy. Local IRQ masking protects this single-hart design, not other harts.

#### Trap return and task switching

SPP means **Supervisor Previous Privilege**, bit 8 of `sstatus`. Its saved value
records the trap's origin: 0 for U-mode, 1 for S-mode. `sret` uses SPP to select
the return privilege. SPIE holds the previous supervisor interrupt-enable bit;
`sret` copies it back to SIE.

```text
irq_run_bottom_halves() returns
                |
                v
Check saved sstatus.SPP
                |
        +-------+------------------------------+
        |                                      |
        v                                      v
SPP = 1: S-origin                     SPP = 0: U-origin
        |                                      |
Resume interrupted kernel work        Has the user task exited?
        |                               |              |
        |                              Yes             No
        |                               |              |
        |                        Switch away;      Consume reschedule
        |                        never restore     request
        |                        exited context        |
        |                                      +-------+-------+
        |                                      |               |
        |                                  No switch        schedule()
        |                                      |               |
        |                                      |        If A switches to B:
        |                                      |        save A.thread_context
        |                                      |        restore B.thread_context
        |                                      |               |
        |                                      |        B's kernel continuation
        |                                      |
        +-------------------+------------------+
                            |
                            v
                     trap_return(tc)
                       Keep SIE = 0 while restoring
                       Restore sepc and sstatus
                       Prepare sscratch:
                         Return to U: kernel stack top
                         Return to S: zero
                       Restore integer registers and sp
                            |
                            v
                           sret
                       PC   <- sepc
                       Mode <- saved SPP
                       SIE  <- SPIE


When A is scheduled again:
  Restore A's thread_context
      -> Continue after A's schedule() call
      -> trap_return(A's trap_context)
      -> sret
```

`scause` and `stval` are diagnostic snapshots, not restored execution state.
Deferred timer work rearms its source before a user-task switch. A new user
task uses the same return routine with a context prepared by the kernel, as
shown in [first user entry](#first-user-entry-and-register-convention).

#### Deferred work API

```c
int deferred_work_add(deferred_callback_t callback, void *arg, int priority);

/* Queue two kernel callbacks. The first one has higher priority. */
deferred_work_add(handle_urgent, urgent_request, 1);
deferred_work_add(handle_background, background_request, 6);
```

`deferred_work_add()` uses a static pool of 32 callback slots. It returns 0 on
success and -1 for an invalid callback/priority or a full pool. The slot is released
after the callback returns. The argument is borrowed: its owner must keep it
alive until completion. Callbacks must not sleep, call the thread scheduler,
or wait for any other deferred callback, even one with higher priority. Keep
callbacks bounded: a long callback delays all queued work. Strict priority does
not guarantee fairness under an endless stream of higher-priority work.

#### Timer and UART split

The timer bottom half updates scheduling deadlines, runs expired events, and
programs the next SBI deadline before re-enabling STIE. One hardware timer
serves the 20 Hz scheduler tick, one-shot events, and the optional two-second
uptime demo. Conversion uses the DT `timebase-frequency`.

Timer-list changes are protected while callbacks run outside the critical
section. `add_timer()` borrows its argument. `add_timer_with_cleanup()` also
accepts a cleanup function: after successful registration, cleanup runs once
after firing or when `timer_stop()` discards the event. Failed registration
leaves the argument with the caller.

UART RX copies up to 16 bytes into its ring in the top half. The bottom half
drains bounded batches, requeues if needed, and re-enables RX when finished.
TX drains the software ring and leaves TX interrupts disabled when idle. Ring
updates, IER read/modify/write, and stress-marker batches use local IRQ masking.
Early-boot and IRQ-off console paths retain polling fallbacks. QEMU exercises
its PLIC/UART IRQ route; board addresses come from the supplied DTB.

#### Lab commands

```text
demo tasks
setTimeout 3 later message
setTimeout 1 earlier message
timer periodic on
timer periodic off
exec test
exec prog
```

`demo tasks` checks that a real timer top half interrupts a running callback
while the timer bottom half waits for that callback to return. It reports
`[tasks] PASS` after checking execution order.

`setTimeout` returns to the shell after registering an event. It copies the
message out of the reusable shell input buffer and prints the submission and
firing times in seconds since timer initialization. Multiple events are ordered
by absolute deadline, with FIFO insertion for equal deadlines. Zero delay is
accepted; decimal input supports up to three fractional digits.

`timer periodic on` enables uptime output every two seconds without replacing
the scheduler tick or queued timeouts. `off` disables only this demonstration.

`exec test` runs the normal P1 syscall image with CSR diagnostics. `exec prog`
(also the default for bare `exec`) selects the Lab 4 diagnostic handler for
that task. The supplied image uses `a7 = 5, 4, 3, 2, 1` as an ecall counter;
treating those values as P1 syscall numbers would exit at the first call.
The diagnostic handler prints `scause`, `sepc`, and `stval`, advances `sepc`
past `ecall`, and restores the user registers. Other tasks keep the P1 ABI.

[`prog.S`](kernel/user/prog.S) reproduces the supplied 14-byte
[OSC2026 program](https://github.com/nycu-caslab/OSC2026/blob/main/uploads/prog.bin).
The system test checks the exact bytes and the five ecall returns. The image
then loops forever in U-mode, as the original does; reset the board to leave
that demo. Timer interrupts continue during the loop. Use `runu` or `exec test`
for programs that exit back to the shell.
