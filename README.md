# OrangePi RV2 OS Project

This repository contains a small operating system / kernel project targeting the OrangePi RV2 board.
The project is developed and tested on real hardware, with the kernel delivered through a UART bootloader workflow.

The main goal of this project is to build core low-level OS components directly on bare metal and make their behavior observable during development, rather than to emulate a full production kernel.

## Current Scope

- UART boot and interactive shell
- FDT-based memory discovery
- initrd / cpio file access
- RISC-V supervisor trap entry / return path
- user-mode test program entry and return-to-shell path
- SBI timer interrupt bring-up
- interrupt-driven UART0 RX/TX with PLIC and ring buffers
- buddy page allocator
- slab-on-buddy kernel heap allocator

## Trap And User Path

The kernel now has a basic RISC-V trap path built around:

- `stvec -> trap_entry -> do_trap() -> trap_return`

Current trap behavior includes:

- user-mode `ecall` handling
- user fault handling and return to shell
- supervisor timer interrupt dispatch
- supervisor external interrupt dispatch for UART0

The user-mode path is intentionally minimal:

- `runu <name>` loads `bin/<name>.bin` from the initramfs
- `enter_user_mode()` builds an initial user trapframe
- `trap_return + sret` transfers control to U-mode
- user `ecall` or fault traps back into S-mode

This is not yet a full process model or scheduler, but it provides the core control-flow needed for later scheduling work.

## Timer Interrupt

Basic core timer interrupt support is implemented using:

- `rdtime` as the clocksource
- SBI `set_timer` as the next-event programming interface

Current timer behavior:

- the kernel reads `timebase-frequency` from the device tree
- `timeron` schedules the first interrupt 1 second later
- each timer interrupt prints uptime in seconds
- the handler rearms the next one-shot timer 2 seconds later
- `timeroff` disables the periodic demo behavior

On the OrangePi RV2 board, the firmware reports SBI v1.0 but the new TIME extension probe does not succeed, so the timer code keeps a legacy SBI timer fallback for real-hardware bring-up.

## Interrupt-Driven UART

UART0 input and output are now interrupt-driven on OrangePi RV2.

Implementation highlights:

- UART0 is routed through the PLIC
- RX and TX each use a ring buffer
- RX interrupts place incoming bytes into the RX buffer
- TX interrupts drain queued output bytes from the TX buffer
- the shell consumes input from the RX buffer and enqueues output to the TX buffer
- short critical sections protect ring-buffer updates shared between ISR and normal kernel code

## Allocator

One of the main components in this project is a slab-based kernel heap allocator built on top of the buddy page allocator.

The allocator is split into two layers:

- The buddy allocator manages page-level allocation and reclamation.
- `kmalloc` / `kfree` handle kernel heap allocation on top of buddy.

Current design:

- Small allocations use the slab path.
- Large allocations use the page allocator directly.
- Each size class is managed by a `kmem_cache`.
- Each slab page owns its own `slab_header` and per-slab freelist.
- Empty slab pages are reused through a small empty-slab reserve policy before being reclaimed to buddy.
- `kfree()` dispatches through page metadata instead of the old header/tag path.
- Freed objects are poisoned with `0xAA` and reclaimed slab pages are poisoned with `0xCC` for debug visibility.

The allocator also exposes shell-visible observability commands so the internal state can be inspected while running on hardware:

- `memstat`
- `slabinfo`
- `buddyinfo`
- `slabcheck`
- `kmtest <name>`

These commands make it possible to demonstrate allocator behavior directly, including:

- slab state transitions after allocation
- empty slab reuse vs. reclaim
- large allocation / free effects on buddy free lists
- allocator regression checks through `kmtest all`

`KMEM_CACHE_EMPTY_LIMIT` is a policy choice, not a correctness requirement. In the current version, each cache keeps at most one empty slab page as a warm spare to reduce unnecessary buddy allocator churn.

## Shell Commands

Current shell commands include:

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

Allocator test commands currently include:

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

For QEMU-based regression runs, use:

```bash
python3 scripts/run_allocator_tests.py
```

## TODO

- Add thread-safe and interrupt-safe protection
- Improve allocator-side debug checks and fault reporting
- Add more allocator test cases and stress scenarios
- Refine slab empty-page retention policy
- Continue cleanup of code structure and comments
- Extend the kernel with process and scheduler work
- Replace timer demo logging with scheduler-driven time accounting
- Further simplify the UART shell interface now that the interrupt-driven path is working

## Notes

This project is intentionally scoped as a learning and systems-building project rather than a Linux-sized kernel implementation.
The current allocator focuses on clear structure, observability, and correctness of the core path on real hardware.
