# OrangePi RV2 Bare-Metal Kernel

This repository is a small bare-metal kernel project built for the OrangePi RV2 and tested on real hardware through a UART bootloader workflow. The goal is not to emulate a production OS, but to implement and debug core kernel mechanisms directly on the board: trap handling, interrupt routing, user-mode entry, timer interrupts, UART RX/TX, and a slab-on-buddy memory allocator.

The project is intentionally narrow and observable. Most features are exposed through a small shell so the control flow can be exercised and verified without a full userspace or scheduler.

## What This Project Covers

- real-board boot and UART shell
- FDT-driven platform discovery
- initramfs / cpio-backed file loading
- RISC-V S-mode trap entry and return
- U-mode program entry and return-to-shell flow
- SBI timer interrupt bring-up
- interrupt-driven UART0 RX/TX through the PLIC
- buddy page allocator
- slab-based kernel heap allocator

## Trap Architecture

The kernel uses one common supervisor trap entry:

- `stvec -> trap_entry -> do_trap()`

From `do_trap()`, control is dispatched by cause:

- user `ecall` -> `handle_user_ecall()`
- user fault -> `handle_user_fault()`
- supervisor timer interrupt -> `timer_handle_interrupt()`
- supervisor external interrupt -> `plic_claim()` -> `uart_handle_irq()` -> `plic_complete()`

If the interrupted context should continue running, the trap exits through:

- `trap_return -> sret`

If a user program has terminated, the kernel deliberately skips `trap_return` and hands control back to the shell instead.

## Timer Interrupt Path

Timer bring-up currently uses:

- `rdtime` as the clock source
- SBI `set_timer` as the next-event mechanism
- `timebase-frequency` from the device tree

The current demo path is:

1. `timeron`
2. `timer_init()`
3. program the first deadline
4. timer expires
5. CPU traps into `trap_entry`
6. `do_trap()` sees `SCAUSE_S_TIMER_INT`
7. `timer_handle_interrupt()` updates demo state and rearms the next deadline

`timeroff` disables the periodic demo path again.

On OrangePi RV2, firmware reports SBI v1.0, but the newer TIME extension probe does not succeed. The code therefore keeps a legacy SBI timer fallback for board-side bring-up.

## UART Interrupt Path

UART0 RX and TX are both interrupt-driven on OrangePi RV2.

The driver now reads:

- UART MMIO base
- PLIC MMIO base
- UART IRQ ID

from the device tree during boot instead of hard-coding those values in the driver.

### RX flow

1. a byte arrives at UART0
2. UART hardware raises its interrupt line
3. PLIC forwards the IRQ to hart0 S-mode
4. CPU traps into `trap_entry`
5. `do_trap()` sees `SCAUSE_S_EXT_INT`
6. `plic_claim()` returns the UART IRQ ID
7. `uart_handle_irq()` drains bytes from `RBR` into the RX ring buffer
8. shell-side `uart_recv()` later consumes bytes from that RX ring buffer
9. `plic_complete()` finishes the interrupt

### TX flow

1. shell output calls `uart_send()` / `uart_send_string()`
2. bytes are queued in the TX ring buffer
3. `uart_tx_kick()` enables TX-ready interrupts
4. UART hardware raises an interrupt when it can accept more output
5. CPU traps into `trap_entry`
6. `do_trap()` dispatches the UART external interrupt
7. `uart_handle_irq()` repeatedly calls `uart_tx_try_drain_one()`
8. `uart_tx_try_drain_one()` pops one byte from the TX ring buffer and writes it to `THR`
9. when the TX ring buffer becomes empty, TX interrupts are disabled again

The `uartdemo` shell command is the compact end-to-end check for this path. It verifies that:

- RX interrupts are being serviced
- TX interrupts are draining output
- the driver is not silently falling back to polling

## User-Mode Program Path

The user-mode path is intentionally minimal and fixed-address:

1. `runu <name>` looks up `bin/<name>.bin` in the initramfs
2. the binary is copied to `USER_CODE_BASE`
3. the kernel synchronizes the instruction stream for the freshly written code
4. `enter_user_mode()` prepares an initial trapframe and writes `sepc`
5. `trap_return -> sret` transfers control from S-mode to U-mode

### `runu test`

`test.bin` is a minimal syscall-path check. It executes two `ecall`s:

1. `a7 = SYS_test`
2. CPU traps to S-mode with `scause = 8`
3. `handle_user_ecall()` prints `scause`, `sepc`, and `stval`
4. kernel advances `sepc`
5. `trap_return -> sret` resumes U-mode
6. `a7 = SYS_exit`
7. CPU traps to S-mode again
8. kernel marks the user program as finished
9. the exit path skips `trap_return` and returns to the shell

This path is useful for verifying that:

- the initial U-mode entry works
- `ecall` traps are decoded correctly
- `sepc` is advanced properly
- the kernel can resume U-mode once and then terminate cleanly on `SYS_exit`

### `runu badinst`

`badinst.bin` starts with:

- `.word 0xffffffff`

So the expected path is:

1. U-mode executes an illegal instruction
2. CPU traps to S-mode with `scause = 2`
3. `handle_user_fault()` prints `scause`, `sepc`, and `stval`
4. kernel terminates the user program
5. control returns to the shell instead of resuming U-mode

This is the shortest end-to-end check for the user fault path.

## Debugging Note: Reusing `USER_CODE_BASE`

All user programs are currently loaded to the same execution address:

- `USER_CODE_BASE = 0x32000000`

That design is simple, but it exposed a subtle bring-up bug:

- running `runu test`
- then running `runu badinst`
- sometimes still executed the old `test` instructions

The bug was not in shell dispatch or fault handling. The problem was instruction-stream coherence: the kernel overwrote the old user image in memory, but the CPU could still execute stale instructions from the previous program image.

The fix was to perform instruction-stream synchronization after copying the new binary and before jumping to `USER_CODE_BASE`.

In this codebase, that fix is implemented inside `cmd_runu()` using the raw encoding of `fence.i`.

## Allocator

The allocator is split into two layers:

- a buddy allocator for page-granularity management
- a slab allocator on top for small kernel objects

Current design points:

- small allocations use slab caches
- large allocations use the page allocator directly
- each size class is managed by a `kmem_cache`
- each slab page owns its own metadata and freelist
- empty slab pages are retained briefly before being reclaimed to buddy
- `kfree()` dispatches through page metadata instead of older header/tag logic
- freed objects and reclaimed slabs are poisoned for debug visibility

The allocator is intentionally observable from the shell through commands such as:

- `memstat`
- `slabinfo`
- `buddyinfo`
- `slabcheck`
- `kmtest <name>`

## Shell Commands

Current interactive commands include:

- `help`
- `hello`
- `info`
- `cores`
- `ls`
- `cat <file>`
- `allocdemo`
- `kmtest <name>`
- `memstat`
- `time`
- `timeron`
- `timeroff`
- `uartdemo`
- `slabinfo`
- `buddyinfo`
- `slabcheck`
- `runu <name>`

Allocator regression targets currently include:

- `kmtest basic`
- `kmtest boundary`
- `kmtest slab`
- `kmtest multislab`
- `kmtest reclaim`
- `kmtest large`
- `kmtest buddy`
- `kmtest invalid`
- `kmtest stress`
- `kmtest all`

