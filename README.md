# OrangePi RV2 Bare-Metal Kernel

This repository is a bare-metal RISC-V kernel project for the OrangePi RV2, developed and tested on real hardware through a UART boot workflow. The main focus is trap handling, interrupt-driven UART, a software timer multiplexer, nested deferred interrupt handling, user-mode entry, devicetree-based platform discovery, and a layered buddy/slab memory allocator.

The code is intentionally small and board-facing. It is closer to a systems side project than a full OS, and each subsystem is exposed through a small shell so the control flow can be observed directly on the board.

## Bootloader and Boot Flow

The development workflow avoids rewriting the SD card for every kernel change. A host-side UART loader sends the kernel image to the board over `/dev/ttyUSB0`, and the board-side boot flow jumps to the loaded image.

At a high level:

1. the host builds `kernel.bin`
2. the host sends it through `bootloader/tools/uart_load.py`
3. the image is loaded at the kernel execution address used by this project
4. firmware transfers control to the kernel and passes the devicetree address in `a1`

This workflow keeps iteration fast on real hardware and matches the bring-up environment used throughout the project.

## Build and Board Test Flow

The normal edit / build / test loop on OrangePi RV2 is:

1. build the kernel image
2. open a serial console to the board
3. send the new kernel image through UART
4. observe boot logs and interact with the shell

Typical commands look like this:

```bash
make -C kernel
sudo screen /dev/ttyUSB0 115200
sudo python3 bootloader/tools/uart_load.py /dev/ttyUSB0 kernel/kernel.bin --initrd kernel/initramfs.cpio --load-cmd never
```

In practice:

- `make -C kernel` builds `kernel.bin` and related artifacts
- `screen` is used as the board console
- `uart_load.py` sends the kernel through the UART boot path instead of rewriting the SD card

Once the kernel boots, all subsystem checks in this project are done from the on-board shell.

## Devicetree and Initramfs

The kernel does not assume hardcoded board addresses for major devices. Instead, it parses the flattened devicetree passed at boot and uses it as the source of platform information.

The current devicetree usage is:

- UART MMIO base
- PLIC MMIO base
- UART IRQ ID
- `timebase-frequency`
- memory regions
- initramfs start/end from `/chosen`

The parser is intentionally small. It implements the minimum FDT pieces needed for this kernel:

- header validation
- structure-block token walking
- node lookup by path
- property lookup by name
- compatibility matching

The initramfs is a `newc` cpio archive. The kernel reads the initrd range from the devicetree instead of hardcoding a ramdisk address, then uses a simple cpio parser to implement:

- `ls`
- `cat <file>`
- `runu <name>` for loading user programs from `bin/<name>.bin`

## Memory System

The memory system is layered.

### Buddy allocator

The lower layer manages memory at page granularity. It tracks free memory in power-of-two page blocks and is responsible for:

- splitting larger blocks when a smaller request arrives
- merging buddy pairs on free
- maintaining the global free-page structure

The key data structures in this layer are:

- a per-page metadata array (`struct page`) used as the frame table
- buddy free lists organized by order
- page state such as order, allocation state, and slab ownership metadata

So this layer behaves like a frame allocator:

- each physical page frame has metadata
- free blocks are linked through the per-order free lists
- allocation and free operations update both the frame metadata and the buddy lists

### Slab allocator

The upper layer provides small kernel-object allocation on top of the buddy allocator. Each slab cache corresponds to an object size class, and slab pages are carved into fixed-size objects.

This layer is responsible for:

- fast small allocations
- object reuse
- per-size-class freelists
- reclaiming empty slab pages back to buddy

The key data structures in this layer are:

- slab caches indexed by object size class
- per-cache lists for partial, full, and empty slab pages
- slab-page metadata stored in the shared page/frame table
- in-page free-object lists for fixed-size objects

So the design is:

- buddy allocator for pages
- slab allocator for small objects
- `kmalloc` / `kfree` route requests to the correct layer

## Trap System

The kernel uses one shared supervisor trap path:

- `stvec -> trap_entry -> trap_dispatch()`

`trap_entry.S` saves the interrupted context, switches to the kernel stack when needed, and enters the C dispatcher. `trap_return` restores the trapframe and finishes with `sret`.

At a high level, the trap system is organized as:

- mode switch: S-mode to U-mode
- exceptions: user `ecall` and user faults
- interrupts: UART and timer
- nested deferred interrupt handling: top half + bottom half

### S-mode to U-mode

User-mode entry is explicit:

1. `runu <name>` loads a raw binary from the initramfs into `USER_CODE_BASE`
2. the kernel synchronizes the instruction stream after copying the image
3. `enter_user_mode()` writes the user entry PC to `sepc`
4. `sstatus` is prepared so `sret` returns to U-mode
5. `trap_return -> sret` transfers control into the user program

### Exceptions

There are two user programs used to validate the exception path:

- `runu test`
- `runu badinst`

`test.bin` exercises the syscall path:

1. U-mode executes `ecall`
2. CPU traps to S-mode
3. `trap_dispatch()` classifies it as a user exception
4. the kernel prints `scause`, `sepc`, and `stval`
5. `SYS_test` advances `sepc` and returns to U-mode
6. `SYS_exit` terminates the user context and returns to the shell

`badinst.bin` exercises the fault path:

1. U-mode executes an illegal instruction
2. CPU traps to S-mode with `scause = illegal instruction`
3. the kernel prints the fault diagnostics
4. the user context is terminated
5. control returns to the shell

So the exception side covers both:

- recoverable user traps
- terminating user faults

### Interrupts

The interrupt side currently has two device paths:

- UART external interrupt through the PLIC
- supervisor timer interrupt

#### UART interrupt path

UART is interrupt-driven on OrangePi RV2 and routed through the PLIC.

The receive path is:

1. a byte arrives at UART0
2. UART raises its interrupt line
3. PLIC delivers a supervisor external interrupt
4. `trap_dispatch()` enters the UART external-interrupt path
5. `plic_claim()` identifies the UART IRQ
6. the UART top half masks the relevant direction and enqueues deferred work
7. the UART bottom half drains RX into the RX ring buffer or drains TX into `THR`
8. `plic_complete()` finishes the interrupt

The main data structures are:

- UART RX ring buffer
- UART TX ring buffer
- persistent deferred tasks for UART RX and UART TX

The compact end-to-end check for this subsystem is:

- `demo uart`

#### Timer interrupt path

The timer subsystem uses one hardware timer source plus a software timer queue.

The core idea is:

- software timers are stored in a sorted singly linked list by absolute `expire`
- the hardware timer is always programmed to the current list head

So timer handling is no longer a fixed periodic heartbeat. It is a deadline multiplexer built on top of one hardware timer source.

The path is:

1. `addtimer <sec>` converts seconds into timer ticks and calls `add_timer(...)`
2. `add_timer(...)` allocates a `timer_event`
3. the event is inserted into a sorted singly linked list by absolute `expire`
4. the hardware timer is reprogrammed to the earliest pending deadline
5. when that deadline arrives, the CPU takes a supervisor timer interrupt
6. the timer top half masks the timer source and enqueues deferred timer work
7. the timer bottom half pops expired events, runs callbacks, frees nodes, and reprograms the next deadline

The main data structure is:

- a sorted singly linked list of `timer_event`

The important invariant is:

- the programmed hardware deadline always matches the list head

The main shell-facing commands for this subsystem are:

- `addtimer <sec>`
- `timer`

### Nested Interrupts and Deferred Work

The advanced interrupt work extends both UART and timer into a shared deferred-work model.

The execution model is:

1. top halves stay short and only acknowledge, mask, and enqueue work
2. deferred work is represented as `irq_task`
3. `irq_task_run_before_return()` runs before `trap_return -> sret`
4. interrupts are re-enabled while bottom halves run
5. a timer interrupt can therefore arrive while UART bottom-half work is still in progress

This is the path that enables visible nested-interrupt behavior in the demo, where timer markers can appear between UART stress markers.

The main data structures are:

- the generic deferred-task queue of `irq_task`
- persistent task objects for timer, UART RX, and UART TX
- the existing UART RX/TX ring buffers

The main tradeoff is that responsiveness improves, but console output becomes harder to keep clean because shell prompts, UART stress output, timer markers, and debug logs all share the same serial line.

## Thread and User Process

This codebase now has two different execution models above the trap layer:

- cooperative kernel threads for the Basic Exercise 1 scheduler work
- explicit U-mode entry for loading and running a user binary through `runu`

They are intentionally separate in the current design. Kernel threads do not reuse the user-mode trap path, and `runu` is not yet integrated into the scheduler as a schedulable process.

### Kernel Threads

The kernel-thread work is intentionally small in scope:

- single hart
- cooperative scheduling
- round-robin policy
- one idle thread
- zombie recycling by the idle thread

The key data structures are:

- `struct thread` for thread metadata, saved context, and kernel-stack ownership
- `struct thread_context` for `ra`, `sp`, and `s0` through `s11`
- `struct runqueue` for the runnable queue, current thread, and idle thread
- a zombie list for terminated threads waiting to be reclaimed

The core path is:

1. `thread_create(entry, arg, ...)` allocates a thread object and a kernel stack
2. the initial saved context is seeded with `sp = kstack_top` and `ra = thread_bootstrap`
3. `schedule()` pops the next runnable thread from the FIFO run queue
4. `switch_to(prev, next)` saves the old context and restores the new one
5. `thread_bootstrap()` calls `entry(arg)` the first time a fresh thread runs
6. `thread_exit()` marks the thread as zombie and hands control back to the scheduler
7. the idle thread reclaims zombie stacks and thread objects

The thread demo is exposed through:

- `demo thread`: tagged worker threads (`A/B/C`) to show visible round-robin interleaving
- `demo foo`: a minimal example that directly uses `thread_create(foo, ...)` to show that a function name is passed as the thread entry

The main tradeoff is that this scheduler is cooperative rather than timer-preemptive. That keeps it decoupled from the existing trap/deferred-interrupt backend and makes the context-switching path easier to validate first.

### User Program Path

User execution is still exposed through `runu <name>` and remains separate from the kernel-thread scheduler.

The current user path is:

1. load `bin/<name>.bin` from the initramfs
2. copy it to the fixed user-code window at `USER_CODE_BASE`
3. execute `fence.i` so the CPU does not reuse stale instructions
4. prepare `sepc` and `sstatus`
5. return to U-mode with `sret`

So at this stage:

- kernel threads are a kernel-only scheduling exercise
- `runu` is an explicit user-mode launch path
- there is not yet a unified process model combining the two

## Shell

The shell is intentionally small. It is not a process manager or userspace environment; it is a board-side debug interface for the kernel subsystems above.

### Command summary

- `help`: print the command list
- `hello`: quick shell sanity check
- `info`: print board, SBI, memory, and initrd information
- `cores`: show secondary-hart bring-up state
- `ls`: list files in the initramfs
- `cat <file>`: print a file from the initramfs
- `demo <name>`: run grouped demos and tests such as `demo foo`, `demo thread`, `demo uart`, `demo stress`, `demo mem <name>`, `demo nested`, and `demo trace`
- `mem`: print allocator, slab, and buddy state
- `timer`: print timer state and the pending software-timer queue
- `addtimer <sec>`: queue a one-shot software timer
- `runu <name>`: load and execute `bin/<name>.bin` from the initramfs in U-mode

Legacy aliases such as `uartdemo`, `uartstress`, `memtest`, and `kmtest` still exist, but the main public entry point is now `demo <name>`.

## Notes from Debugging

One bug was especially important in this project.

### Reusing one user-code window requires instruction-stream sync

All user binaries are copied to the same execution address. That means loading a new program is really an overwrite of the previous one. Without instruction-stream synchronization, the CPU may still execute stale instructions from the previous image.

In this codebase, that bug showed up when:

- `runu test` was executed first
- then `runu badinst` was loaded into the same address
- the board still behaved as if the old program was running

The fix is a `fence.i` after copying the new user image and before entering U-mode again.

### Serial console state can become stale after heavy UART output

During the nested-interrupt demo, the board console may become sluggish even though the kernel still receives input correctly. In practice this was usually not a permanent logic failure. Heavy UART stress output, repeated kernel reloads, and reconnecting the host serial session could leave the development setup in a stale state.

In practice, the most reliable recovery was:

- restart the board
- reconnect the serial session
- reload the kernel through the UART boot flow

This is a practical board-side debugging note rather than a claim that rebooting is part of the design.

### Console output from multiple paths can interleave

This kernel does not implement a full TTY or console-locking layer. As a result, shell prompts, timer callbacks, allocator logs, and UART demo output can appear on the same serial line and sometimes interleave at character granularity.

That behavior affects how clean the console looks, but it does not change the core interrupt design. The nested-interrupt work was validated by checking both:

- visible UART/timer interleaving in the demo
- the internal trace from `demo trace`
