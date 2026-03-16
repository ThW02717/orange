# OrangePi RV2 OS Project

This repository contains a small operating system / kernel project targeting the OrangePi RV2 board.
The project is developed and tested on real hardware, with the kernel delivered through a UART bootloader workflow.

The main goal of this project is to build core low-level OS components directly on bare metal and make their behavior observable during development, rather than to emulate a full production kernel.

## Current Scope

- UART boot and interactive shell
- FDT-based memory discovery
- initrd / cpio file access
- buddy page allocator
- slab-on-buddy kernel heap allocator

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
- `slabinfo`
- `buddyinfo`
- `slabcheck`

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
- Extend the kernel with process, scheduling, and trap/syscall work

## Notes

This project is intentionally scoped as a learning and systems-building project rather than a Linux-sized kernel implementation.
The current allocator focuses on clear structure, observability, and correctness of the core path on real hardware.
