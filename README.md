# Orange

An educational RV64 kernel for OrangePi RV2, written in C and RISC-V assembly.

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

`g_regions` describes RAM; `g_reserves` records occupied ranges. Neither array
is itself the memory it describes. Both are static kernel storage. Likewise,
the `g_frame_array` pointer is static, but the metadata array it points to is
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

The bootstrap shell later becomes the idle thread and keeps using the boot
stack. Secondary harts use separate static 4 KiB stacks in `.bss`; they do not
participate in the scheduler. Scheduling runs on the bootstrap hart.

#### User image and per-task storage

User programs are linked at `0x32000000`, converted from ELF to raw `.bin`
files, and packed into initramfs. The
[`shell loader`](kernel/src/shell.c) copies `bin/<name>.bin` into the fixed
1 MiB window `[0x32000000, 0x32100000)`. The entry address is `0x32000000`;
the window size is a loading limit, not the size of every program.

Each scheduled user task has a separate control block, kernel stack, and user
stack. Their addresses come from the allocator, not from a fixed per-process
virtual memory map.

```text
Kernel-owned task resources                Shared user-image window

Task A                                     0x32000000
+-- struct thread A                        +--------------------------+
+-- Kernel stack A: 4 KiB                  | One loaded raw image     |
+-- User stack A: 64 KiB                   | Entry at the window base |
`-- user_entry --------------------------->|                          |
                                           | Unused window capacity   |
Task B                                     +--------------------------+
+-- struct thread B                        0x32100000 (exclusive end)
+-- Kernel stack B: 4 KiB
+-- User stack B: 64 KiB
`-- user_entry ---------------------------> same image entry

Resource relationships, not physical address order or separate address spaces.
```

Kernel-only worker threads have a control block and a 4 KiB kernel stack, but
no user stack. All these allocations remain kernel-managed and are reclaimed
after the thread exits, once it is no longer executing on its own stack.

This is not an ELF process loader. It does not construct per-process text,
data, BSS, heap, or TLS mappings. Loading another binary overwrites the shared
image window. Also, `memory_init()` does not explicitly reserve that window;
its fixed address alone does not protect it from allocator reuse.

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
| pid, kind, state, entry, queue links   |
| kstack_base, kstack_top                | --> Kernel stack allocation
| user_stack_base, user_stack_top        | --> User stack allocation
| tc                                     | --> Top-of-stack trap-context slot
+----------------------------------------+
```

For a newly created user task, let `K` be its kernel-stack base. The stack is
4 KiB, and the 280-byte context occupies a 288-byte slot to preserve 16-byte
stack alignment.

```text
Higher addresses

K + 4096  +-----------------------------------------+  kstack_top
          | Alignment padding: 8 bytes              |
K + 4088  +-----------------------------------------+
          | trap_context: 280 bytes                 |
          | Integer register slots + four CSRs      |
K + 3808  +-----------------------------------------+  tc
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
snapshot. `trap_return()` restores `sepc`, `sstatus`, and the integer-register
slots before executing `sret`. `scause` and `stval` are diagnostic fields, not
restored execution state.

```text
Task A running in U-mode
        |
        | trap: switch to A's kernel stack and create trap_context
        v
Kernel handling A's trap
        |
        +-- No task switch --> trap_return() --> sret --> A in U-mode
        |
        `-- schedule() --> switch_to(A, B)
                              |
                              | Save A's kernel thread_context
                              | Restore B's kernel thread_context
                              v
                         B's kernel continuation

Later, when A is scheduled again:
restore A's thread_context --> resume its kernel path
                          --> trap_return() --> sret --> A in U-mode
```

A privilege transition is not necessarily a task switch. If A is suspended
inside the trap handler, its trap context remains on A's kernel stack while
`thread_context` records where its kernel execution should resume.

#### First user entry and register convention

[`thread_create_user()`](kernel/src/thread.c) builds both initial contexts:

- `thread_context.ra = thread_bootstrap`; `thread_context.sp = kstack_top - 288`.
- `tc->sp = user_stack_top`; `tc->tp = thread`; `tc->sepc = user_entry`.
- The initial trap context is zeroed, so `sstatus.SPP = 0` selects U-mode.

The first switch follows this path without requiring a previous user trap:

```text
switch_to() -> thread_bootstrap() -> user_task_entry()
            -> trap_return(tc) -> sret -> user entry
```

In this kernel, `tp` points to the current `struct thread` in both U-mode and
S-mode; user TLS is not implemented. While a task runs in U-mode, `sp` points
into its user stack and `sscratch` holds its kernel-stack top. On a user-origin
trap, `csrrw sp, sscratch, sp` switches stacks and retains the interrupted user
`sp` for the snapshot. A supervisor-origin trap keeps the current kernel stack.

Current register-preservation limitation: in
[`trap_entry.S`](kernel/src/trap_entry.S), the entry path uses `t0` before saving
it, and the return path reuses `t1` after restoring it. The original `t0` and
`t1` values therefore are not fully preserved across traps. The context layout
above describes the storage format, not complete trap-register preservation.
