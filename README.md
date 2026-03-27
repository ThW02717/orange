# OrangePi RV2 Bare-Metal Kernel

This repository is a small bare-metal kernel project for the OrangePi RV2. It is developed and tested on real hardware through a UART boot workflow instead of relying only on emulation. The project is intentionally narrow: the focus is on board bring-up, devicetree-based discovery, a layered memory allocator, RISC-V trap handling, user-mode entry, interrupt-driven UART, and a multiplexed timer subsystem.

The code is closer to a systems side project than a full OS. Each subsystem is exposed through a small shell so the control flow can be observed directly on the board.

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

The main data structure is the classic buddy free list organized by order.

### Slab allocator

The upper layer provides small kernel-object allocation on top of the buddy allocator. Each slab cache corresponds to an object size class, and slab pages are carved into fixed-size objects.

This layer is responsible for:

- fast small allocations
- object reuse
- per-size-class freelists
- reclaiming empty slab pages back to buddy

At a high level, the slab layer tracks:

- slab caches by size class
- page metadata for each slab page
- free objects inside each slab page

So the design is:

- buddy allocator for pages
- slab allocator for small objects
- `kmalloc` / `kfree` route requests to the correct layer

## Trap System

The kernel uses one common supervisor trap entry:

- `stvec -> trap_entry -> do_trap()`

`trap_entry.S` is responsible for:

- switching to the kernel stack when trapping from U-mode
- saving the interrupted context into a trapframe
- calling the C dispatcher

`trap_return` restores that trapframe and ends with:

- `sret`

So the trap system has one shared entry path, one shared trapframe format, and then different logical subpaths depending on the cause.

### S-mode to U-mode

User-mode entry is minimal but explicit:

1. `runu <name>` loads a raw binary from the initramfs into `USER_CODE_BASE`
2. the kernel synchronizes the instruction stream after copying the image
3. `enter_user_mode()` writes the user entry PC to `sepc`
4. `sstatus` is prepared so `sret` returns to U-mode
5. `trap_return -> sret` transfers control into the user program

This directly implements the basic RISC-V mode switch path from S-mode to U-mode.

### User program trap path

There are two user programs used to validate the user trap flow:

- `runu test`
- `runu badinst`

`test.bin` exercises the syscall path:

1. U-mode executes `ecall`
2. CPU traps to S-mode
3. `do_trap()` classifies it as a user `ecall`
4. the kernel prints `scause`, `sepc`, and `stval`
5. `SYS_test` advances `sepc` and returns to U-mode through `trap_return -> sret`
6. `SYS_exit` marks the user program as finished and returns to the shell instead of resuming U-mode

`badinst.bin` exercises the fault path:

1. U-mode executes an illegal instruction
2. CPU traps to S-mode with `scause = illegal instruction`
3. the kernel prints the fault diagnostics
4. the user context is terminated
5. control returns to the shell

So the trap code covers both:

- recoverable U-mode traps
- terminating U-mode faults

### UART interrupt path

UART is interrupt-driven on OrangePi RV2 and routed through the PLIC.

The receive path is:

1. a byte arrives at UART0
2. UART raises its interrupt line
3. PLIC delivers a supervisor external interrupt
4. `do_trap()` enters the UART external-interrupt path
5. `plic_claim()` identifies the UART IRQ
6. `uart_handle_irq()` drains hardware RX into the RX ring buffer
7. the shell later consumes bytes from the RX ring buffer
8. `plic_complete()` finishes the interrupt

The transmit path is:

1. shell output queues bytes into the TX ring buffer
2. UART TX interrupts are enabled
3. when the UART can accept more data, an external interrupt arrives
4. `uart_handle_irq()` drains queued bytes into `THR`
5. TX interrupts are disabled again when the TX ring buffer becomes empty

The compact end-to-end check for this subsystem is:

- `uartdemo`

### Timer interrupt path

The timer subsystem uses one hardware timer source plus a software timer queue.

The core idea is:

- software timers are stored in a sorted singly linked list by absolute `expire`
- the hardware timer is always programmed to the current head deadline

So timer handling is no longer a fixed periodic heartbeat. It is now a deadline dispatcher.

The path is:

1. `addtimer <sec>` converts seconds into timer ticks and calls `add_timer(...)`
2. `add_timer(...)` allocates a `timer_event`, computes its absolute `expire`, and inserts it into the sorted pending list
3. the hardware timer is programmed to the earliest pending deadline
4. when that deadline arrives, the CPU takes a supervisor timer interrupt
5. `do_trap()` dispatches to `timer_handle_interrupt()`
6. `timer_handle_interrupt()` pops every expired event, runs its callback, frees the node, and then reprograms the next deadline

The important invariant is:

- the programmed hardware deadline always matches the list head

The main shell-facing commands for this subsystem are:

- `addtimer <sec>`
- `timer`

## Shell

The shell is intentionally small. It is not a process manager or userspace environment; it is a board-side debug interface for the kernel subsystems above.

### Command summary

- `help`: print the command list
- `hello`: quick shell sanity check
- `info`: print board, SBI, memory, and initrd information
- `cores`: show secondary-hart bring-up state
- `ls`: list files in the initramfs
- `cat <file>`: print a file from the initramfs
- `mem`: print allocator, slab, and buddy state
- `memtest <name>`: run allocator regression tests
- `timer`: print timer state and the pending software-timer queue
- `addtimer <sec>`: queue a one-shot software timer
- `uartdemo`: compact RX/TX interrupt-driven UART demo
- `runu <name>`: load and execute `bin/<name>.bin` from the initramfs in U-mode

## Notes from Debugging

One bug was especially important in this project.

### Reusing one user-code window requires instruction-stream sync

All user binaries are copied to the same execution address. That means loading a new program is really an overwrite of the previous one. Without instruction-stream synchronization, the CPU may still execute stale instructions from the previous image.

In this codebase, that bug showed up when:

- `runu test` was executed first
- then `runu badinst` was loaded into the same address
- the board still behaved as if the old program was running

The fix is a `fence.i` after copying the new user image and before entering U-mode again.
