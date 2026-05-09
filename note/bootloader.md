# Orange Bootloader Design Notes

## 1. What is a bootloader?

A bootloader is a **role**, not a specific program. Its core responsibilities are:

1. Prepare enough hardware/runtime state
2. Load the next stage
3. Pass necessary boot information
4. Transfer control

The key questions to classify any bootloader:

| Question | Our UART Bootloader |
|---|---|
| Where does it load from? | UART |
| What does it load? | kernel.bin + initramfs.cpio (custom bundle) |
| How does it hand off? | Function pointer jump to fixed address `0x20000000` |

---

## 2. Boot Flow

```
Power on
  → BROM (on-chip ROM, immutable)
  → SPL (U-Boot's smaller stage, initializes DRAM)
  → U-Boot (full bootloader, reads SD card)
      └─ embedded OpenSBI (M-mode firmware, provides ecall services)
  → OpenSBI switches to S-mode, jumps to 0x00200000
      a0 = hartid, a1 = dtb_addr (RISC-V SBI boot convention)
  → boot.S (bootloader _start)
      ├─ la sp, _stack_top         // set stack pointer
      ├─ la t0, __bss_start        // clear BSS section
      ├─ la t1, __bss_end
      ├─ bss_loop: sb zero → addi → j
      └─ call kernel_main          // → kernel.c
  → kernel_main()
      ├─ Multi-hart: only first hart runs shell, others idle (wfi)
      ├─ uart_init()               // flush FIFO, disable interrupts
      └─ Shell loop: wait for "load" command
  → User types "load"
  → boot_load_and_jump()
      ├─ Receive header (magic + size + checksum) over UART
      ├─ Receive payload = metadata(16B) + kernel.bin + initramfs.cpio
      ├─ Parse kernel size, initrd size, checksums
      ├─ Verify checksums
      ├─ copy kernel  → 0x20000000
      ├─ copy initrd  → 0x46100000
      ├─ fence rw,rw + fence.i     // ensure copied code is visible to instruction fetch
      └─ next_kernel(hartid, dtb, initrd_start, initrd_end)
            → jump to 0x20000000 (kernel _start)
```

---

## 3. Memory Layout (linker.ld)

```
0x00200000 ┌──────────────┐
           │ .text.boot   │ ← boot.S, KEEP() placed first
           │ .text.*      │ ← remaining .text
           ├──────────────┤
           │ .rodata      │ ← read-only constants
           ├──────────────┤
           │ .data        │ ← initialized globals
           ├──────────────┤
           │ .bss         │ ← __bss_start (zero-init / uninit variables)
           │              │    cleared by boot.S loop
           │              │ ← __bss_end
           ├──────────────┤
           │  stack       │ ← 16 KiB, grows downward
           └──────────────┘ ← _stack_top
```

linker.ld defines the memory map at **compile time**. boot.S uses these symbols to initialize the runtime environment. The linker replaces `__bss_start`, `__bss_end`, `_stack_top` with absolute addresses in the final binary.

---

## 4. File Structure

```
bootloader/
├── bootloader.its          # FIT image descriptor for U-Boot
├── linker.ld               # memory layout
├── Makefile
├── include/
│   ├── sbi.h               # SBI ecall interface definitions
│   ├── shell.h              # shell_t struct
│   ├── uart.h               # UART function declarations
│   ├── utils.h              # get32/put32/delay (volatile MMIO)
│   └── peripherals/
│       └── mini_uart.h      # UART register address definitions
├── src/
│   ├── boot.S              # first code: set sp, clear BSS, call kernel_main
│   ├── kernel.c            # main: multi-hart selection, init, shell loop
│   ├── uart.c              # UART driver: init/send/recv (polling)
│   ├── sbi.c               # OpenSBI ecall wrappers
│   ├── shell.c             # Shell + UART bundle loading protocol
│   └── string.c            # string utility functions
└── tools/
    └── uart_load.py        # PC side: reads kernel+initrd, builds bundle, sends via UART
```

---

## 5. Core Design Concepts

### 5.1 RISC-V Privilege Modes

| Mode | Privilege | Role |
|------|-----------|------|
| M-mode | Highest | OpenSBI (resident, provides timer/hart services) |
| S-mode | Middle | Bootloader + Kernel |
| U-mode | Lowest | User programs |

The bootloader runs in **S-mode**. When it needs M-mode operations (setting timers, starting harts), it uses `ecall` to invoke OpenSBI. OpenSBI lives in M-mode and handles the privileged hardware access.

### 5.2 Multi-hart Bootstrap

```c
static volatile unsigned long bootstrap_hart = ~0UL;  // ~0 = unclaimed

void kernel_main(unsigned long hartid, unsigned long dtb_addr) {
    if (bootstrap_hart == ~0UL) bootstrap_hart = hartid;  // first hart claims
    if (hartid != bootstrap_hart) { while(1) wfi; }        // others sleep
    // ... only one hart runs the shell
}
```

`~0UL` (all bits 1, i.e. `0xFFFFFFFFFFFFFFFF`) serves as a sentinel value. No real hartid can equal this, so the first hart to arrive always wins. On real hardware, OpenSBI starts harts sequentially, so hart 0 always wins in practice.

### 5.3 UART Driver (16550-compatible, polling mode)

```
uart_init()  → disable interrupts, flush FIFOs
               (board: U-Boot already set baud rate, so we skip re-config)

uart_send(c) → while (TX FIFO full) spin;  put32(THR, c);
uart_recv()  → while (RX FIFO empty) spin; return get32(RBR);
```

Board register base: `0xD4017000` (Allwinner D1), QEMU: `0x10000000`.

The 16550 UART internally has a 16-byte RX FIFO and a 16-byte TX FIFO. Writing to `0xD4017000` puts a byte into the TX FIFO (the hardware auto-transmits). Reading from `0xD4017000` pops a byte from the RX FIFO. The same address goes to different hardware registers depending on read vs write.

#### Why `volatile` is essential

```c
// without volatile:
while ((uart_reg_read(UART_LSR_REG) & LSR_RX_READY) == 0) {}
// compiler may read LSR once, cache it, infinite loop or never execute

// with volatile:
while ((*(volatile uint32_t *)addr & bit) == 0) {}
// compiler guarantees: every read hits actual memory
```

Hardware registers change value **without any software write** — the compiler cannot see this. `volatile` forces it to re-read every time.

### 5.4 Memory-mapped I/O

```c
// utils.h
static inline uint32_t get32(uintptr_t addr) {
    return *(volatile uint32_t *)addr;
}
static inline void put32(uintptr_t addr, uint32_t value) {
    *(volatile uint32_t *)addr = value;
}
```

No `/dev/ttyS0`, no `write()` syscall, no kernel. Just pointer dereference at a physical address. That's bare-metal I/O.

- `inline` → compiler expands the function body at the call site (no function call overhead)
- `volatile` → compiler reads/writes memory every time (no caching, no reordering, no elimination)

### 5.5 UART Bundle Protocol

```
Header (12 bytes): [magic 4B][payload_size 4B][payload_checksum 4B]

Payload:           [kernel_len 4B][initrd_len 4B][kernel_csum 4B][initrd_csum 4B]
                   [kernel.bin raw bytes...]
                   [initramfs.cpio raw bytes...]
```

- magic = `0x324F4F42` ("BOO2")
- checksum = sum of all bytes, truncated to 32 bits
- PC side `uart_load.py` packs the bundle, board side `boot_load_and_jump()` unpacks it

The bundle is sent as raw binary over UART. No encoding, no escape sequences — just bytes. The checksum detects transmission errors (bit flips from UART noise).

### 5.6 Jumping to Kernel via Function Pointer

```c
// Cast address 0x20000000 to a function pointer type
void (*next_kernel)(unsigned long, unsigned long, unsigned long, unsigned long);
next_kernel = (void (*)(...))0x20000000;

// Call it → compiler auto-loads a0~a3, then jalr to 0x20000000
next_kernel(hartid, dtb_addr, initrd_start, initrd_end);
// a0 = hartid
// a1 = dtb_addr
// a2 = initrd_start
// a3 = initrd_end
```

`void*` (data pointer) ≠ function pointer in C. You cannot call through a `void*`. The explicit function-pointer cast tells the compiler: "this address is executable code with this signature."

The kernel's own `boot.S` starts at `_start`, sets its own sp, clears its own BSS, then calls `kernel_main(hartid, dtb, initrd_start, initrd_end)`.

The `loaded image returned` line after the call is a safety net — `kernel_main` should never return.

### 5.7 `volatile` vs `fence` vs `atomic`

| | `volatile` | `fence` | `atomic` (AMO/LR-SC) |
|---|---|---|---|
| Controls | Compiler | CPU hardware | CPU hardware |
| Prevents | Optimization reorder/elimination | Store buffer reorder / cache delay | Read-modify-write torn |
| Cross-core | No | Yes | Yes |
| Use case | MMIO, ISR flags | Cross-core sync, pre-jump barrier | Locks, counters, CAS |

`volatile` is a **compiler barrier**. `fence` is a **hardware barrier**. Neither replaces the other. `volatile` can't stop the CPU store buffer from reordering stores — only `fence` can.

### 5.8 Fence Instructions Before Jump

```c
asm volatile("fence rw, rw" ::: "memory");
// fence rw,rw: all prior reads & writes complete before any subsequent memory ops

asm volatile(".word 0x0000100f" ::: "memory");
// fence.i (raw opcode): synchronize instruction fetch after writing code bytes
```

`fence rw, rw` orders earlier memory reads/writes before the jump.
`fence.i` synchronizes the instruction stream with prior data writes. This matters because the bootloader copied the kernel image as data and then immediately jumps to execute it as instructions.

---

## 6. Physical Connection

```
PC                                OrangePi RV2 Board
──                                ──
minicom                        your uart.c
  ↓ write()                       ↑ uart_send() / uart_recv()
/dev/ttyUSB0                          ↓↑ put32(get32())
  ↓ USB                                ↓↑
USB-to-TTL cable  ←────────→  D1 UART hardware (0xD4017000)
                       TX/RX/GND pins
```

No driver registration, no enumeration. The hardware is at a fixed address. You write to it, bits go out the pin. You read from it, you get whatever came in on the pin.

---

## 7. Bootloader Comparison

### 7.1 Overview

A bootloader is a **role** — a piece of software that hands control to the next stage. Different bootloaders serve different purposes.

| | Our UART Bootloader | U-Boot | UEFI | GRUB |
|---|---|---|---|---|
| **Goal** | Fast kernel dev iteration | General embedded bootloader | Standard firmware interface | OS boot manager |
| **Source** | UART | SD/eMMC/SPI/NAND/USB/network | EFI System Partition, disk, network | Disk filesystem |
| **Loads** | kernel.bin + initramfs bundle | Image/uImage/FIT + DTB + initramfs | EFI applications | Linux kernel + initrd |
| **Handoff** | Function pointer jump to fixed addr | booti / bootm / bootz | ExitBootServices → OS | Linux boot protocol |
| **Filesystem** | No | FAT/ext4 etc. | EFI Simple File System, FAT | ext4/btrfs/xfs etc. |
| **Complexity** | Low | High | High / standardized | Medium |
| **Best for** | Bare-metal kernel dev | Embedded Linux products | PC/server/standardized platforms | Multi-OS boot menus |

### 7.2 Our UART Bootloader

```
OpenSBI → our bootloader → our kernel
```

Design priorities:
- Minimal feature set
- Fixed addresses
- Fixed format
- No filesystem
- No boot menu
- No kernel command line

Advantage: rapid kernel iteration. Change your kernel → send over UART → no SD card re-flash needed.

It's a **developer loader**, not a production bootloader.

### 7.3 U-Boot

```
Boot ROM → SPL → U-Boot proper → Linux kernel
```

U-Boot is a general-purpose embedded bootloader. Key features:
- Supports many input sources (SD, eMMC, SPI, NAND, USB, TFTP)
- Reads FAT/ext4 filesystems
- Interactive shell with environment variables
- Can modify device tree at runtime
- Typical commands: `load`, `booti`, `bootm`, `fdt`, `printenv`, `setenv`

U-Boot is more like a **pre-OS platform manager**.

### 7.4 UEFI

```
CPU reset → platform firmware → UEFI firmware → UEFI Boot Manager
→ EFI application (GRUB or Linux EFI stub) → OS kernel
```

UEFI is a **standardized firmware interface**, not just a loader. It provides:
- Boot Services and Runtime Services
- Protocols for disk, network, display, input
- Memory map and ACPI tables

Key step: `ExitBootServices()` — the OS takes full control of hardware.

### 7.5 GRUB

```
UEFI → GRUB → Linux kernel
```

GRUB is a **boot manager** running on top of firmware:
- Displays boot menus
- Reads ext4/btrfs/xfs
- Loads multiple kernels
- Passes kernel command line
- Supports multiboot

GRUB handles OS selection and kernel configuration, not hardware bring-up.

### 7.6 Where OpenSBI Fits

OpenSBI is **not a traditional bootloader**. It's M-mode firmware that provides runtime services (via ecall):

- `sbi_set_timer()` — program timer interrupts
- `sbi_hart_start()` — wake up secondary cores
- `sbi_probe_extension()` — check available services

Common RISC-V flows:

```
Boot ROM → OpenSBI → U-Boot → Linux
Boot ROM → OpenSBI → Linux
Boot ROM → OpenSBI → our UART bootloader → our kernel  (this project)
```

OpenSBI is the M-mode foundation. U-Boot / our bootloader handle loading the OS.

### 7.7 Design Trade-offs Summary

| Design dimension | Our bootloader | U-Boot | UEFI |
|---|---|---|---|
| Address policy | Fixed | Environment variables | Loader decides |
| Boot info passed | hartid, FDT ptr, initramfs range | FDT, bootargs, initrd, machine info | Memory map, ACPI/DT, config tables |
| Hardware support | UART only | Many drivers | Via UEFI protocols |
| Image format | Custom bundle | Image, uImage, FIT, raw | EFI executable |

### 7.8 Interview Answer Template

> "Bootloaders vary by platform and stage. First-stage loaders like SPL mainly initialize DRAM. Second-stage loaders like U-Boot can load kernel images from storage or network and pass FDT/initramfs to Linux. In my project, I implemented a minimal UART bootloader focused on fast kernel iteration: it receives a kernel/initramfs bundle over UART, validates it, copies to fixed addresses, and jumps to the kernel."

---

## 8. Quick Reference

| Concept | RISC-V ASM | GCC C |
|---|---|---|
| Load label address | `la rd, symbol` | — |
| Store byte | `sb rs, offset(rd)` | `*(uint8_t*)addr = val` |
| Store word | `sw rs, offset(rd)` | `*(uint32_t*)addr = val` |
| Sleep until interrupt | `wfi` | `asm volatile("wfi")` |
| Memory barrier | `fence rw, rw` | `asm volatile("fence rw, rw":::"memory")` |
| Call M-mode firmware | `ecall` | `sbi_ecall(ext, fid, ...)` |
| Jump subroutine | `call label` (pseudo) | `func()` |
| Function pointer call | — | `void (*f)(...) = (void (*)(...))addr; f(...);` |

---

## 9. 常見問題整理

### 9.1 為什麼這個 bootloader 用 polling 而不是中斷？

bootloader 的生命週期極短，只做三件事：等人打指令、收 binary、跳 kernel。

UART 在 115200 baud 下，一個 byte 大約花 87 μs，而 CPU 跑幾百 MHz，一個 polling loop 只要幾十 ns。polling 完全不會浪費 CPU，反而省掉了設中斷向量、寫 ISR、開關中斷的複雜度。一來 bootloader 沒有排程、沒有其他工作要做，二來越簡單越不容易出 bug。

與其說「用 polling 是偷懶」，不如說「在這個場景下，polling 是最合理的選擇」。

---

### 9.2 為什麼不用更強的 checksum（如 CRC32）？

UART 傳輸錯誤率很低（一般環境下幾乎不會有 bit flip），簡單的累加 checksum（每個 byte 加起來取低 32-bit）已經足以抓到傳輸錯誤。

板子上跑的是 bare-metal，沒有硬體 CRC 加速器，手算 CRC 或 SHA 反而增加 bootloader 的 code size 跟複雜度。這是開發工具，不是 production bootloader，夠用就好。

---

### 9.3 bundle 協議為什麼不用標準格式（如 FIT image）？

FIT image 需要 libfdt 去解析 device tree 結構，parser 本身就不小，對一個 minimal bootloader 來說太重了。

我自訂的 bundle 格式只有 16 bytes metadata 後接 raw binary：

```
[magic 4B][payload_len 4B][payload_csum 4B]
[kernel_len 4B][initrd_len 4B][kernel_csum 4B][initrd_csum 4B]
[kernel.bin]
[initramfs.cpio]
```

parser 就幾十行 C code：先收 header → 驗 checksum → 收 payload → 拆出 kernel 跟 initrd → 搬到固定位址 → 跳。不用引入任何 library。

而且 kernel 端已經有自己的 FIT image 了，bootloader 不需要再解一層。

---

### 9.4 bundle 協議為什麼用 raw binary 而不是 ELF？

kernel 在 compile 階段已經透過 `objcopy -O binary -S` 從 ELF 轉成 raw binary，strip 掉 symbol table、relocation entry 這些執行時用不到的東西。

raw binary 的好處是 bootloader 直接 `copy_bytes` 塞到固定位址就能跑，不需要寫 ELF loader 去 parse program header、處理 relocation。因為 kernel 已經事先被 linker 編排到 `0x20000000`，二進位內容就是記憶體內容的直接映射。

---

### 9.5 為什麼 bootloader 跟 kernel 都有 boot.S？

兩者是**獨立的 binary**，各自有各自的 linker script、各自的 stack、各自的 BSS。

U-Boot 跳 bootloader 時不會幫忙設好 stack 跟清 BSS，bootloader 跳 kernel 時也一樣。所以各自都需要一段組語程式初始化自己的 C 執行環境：

```asm
la sp, _stack_top        // 設自己的 stack
la t0, __bss_start       // 清自己的 BSS
la t1, __bss_end
...
call kernel_main
```

如果共用同一塊 stack 或 BSS，兩者互相踩到對方的記憶體就直接 crash。

---

### 9.6 為什麼 bootloader 只有一個 hart 做事？其他 hart 在做什麼？

RISC-V 多核心處理：用一個 `bootstrap_hart` 變數來決定誰是主核心。

```c
static volatile unsigned long bootstrap_hart = ~0UL;  // ~0 代表未佔用

if (bootstrap_hart == ~0UL) bootstrap_hart = hartid;   // 第一個到的佔住
if (hartid != bootstrap_hart) { while(1) wfi; }         // 其他的睡覺
```

第一個到的 hart 把自己的編號寫進去，成為主核心，負責跑 shell、收 kernel、跳轉。其他 hart 全部在 `wfi` 睡覺。等 kernel 起來之後，kernel 會用 `sbi_hart_start` 把其他 hart 叫起來跑 kernel 的 `secondary_start`。

---

### 9.7 為什麼跳 kernel 之前要 fence，而且要兩條？

搬 kernel 跟 initrd 到記憶體後，這些 write 可能還在 CPU 的 store buffer 裡，還沒真正寫進 DDR。如果直接跳，跑到的可能是舊的或不完整的內容。

```c
asm volatile("fence rw, rw" ::: "memory");    // memory ordering
asm volatile(".word 0x0000100f" ::: "memory"); // fence.i，讓剛寫入的 kernel code 可被正確取指
```

- `fence rw, rw` — 確保前面所有 read/write 操作完成後，才執行後面的記憶體操作
- `fence.i` — 確保剛剛用 data store 寫入的 kernel bytes，接下來 instruction fetch 能看到最新內容

兩條 fence 放的位置在 `next_kernel()` 之前就行，不需要夾在每個 `copy_bytes` 中間，因為 kernel 跟 initrd 之間沒有依賴關係，只要跳的時候兩者都到位就好。

因為 bootloader 是先把 kernel 當作 data 複製到 RAM，下一步又要把同一段 RAM 當作 instruction 執行，所以跳轉前需要 `fence.i`。

---

### 9.8 `volatile` 跟 `fence` 差在哪裡？

| | `volatile` | `fence` |
|---|---|---|
| 管誰 | compiler | CPU 硬體 |
| 防止 | 最佳化省略、重排 | store buffer 延遲、cache 不一致 |
| 跨核 | 管不到 | 管得到 |
| 例子 | `while(reg & flag){}` 不會被刪掉 | 搬完 kernel 後確保其他核看得到 |

`volatile` 是 compiler barrier，`fence` 是 hardware barrier。兩個管不同層，誰也取代不了誰。`volatile` 可以阻止 compiler 把 `while (uart_reg_read(...) & bit)` 最佳化成一讀永逸，但阻止不了 CPU 硬體把 store 重排序。反過來，`fence` 管 CPU 但管不了 compiler 亂省略指令。

---

### 9.9 跟 U-Boot 比，這個 bootloader 少了什麼？

故意少的東西：

- 檔案系統支援（FAT/ext4）
- 網路 boot（TFTP/NFS）
- 環境變數（bootcmd、bootargs）
- Boot menu
- Kernel command line 傳遞
- 多種 image 格式解析（Image/uImage/FIT/zImage）
- Device tree 修改（fdt set/print）

這些都是**故意省略**的。這個 bootloader 的目標是極簡開發工具，不是通用 embedded bootloader。開發階段只要一條 UART 線就能反覆送 kernel，U-Boot 的複雜度在這個場景是多餘的。

---

### 9.10 傳輸到一半 UART 斷線怎麼辦？

`recv_bytes` 是 blocking polling，UART 線斷了就永遠卡在那裡等不到下一個 byte。bootloader 沒有 timeout 機制，因為這是開發工具，開發者通常會注意到終端機沒回應，手動重開板子就好。

如果是 production bootloader，通常會加 watchdog 或傳輸 timeout，但開發階段這些都是不必要的複雜度。

---

### 9.11 為什麼要自己寫 UART driver，不用現成的？

bare-metal 世界沒有「現成的 driver」。沒有 Linux kernel、沒有 `/dev/ttyS0`、沒有 `open()`/`write()` 這些 system call。

能做的就是直接戳硬體暫存器（memory-mapped I/O）：

```c
#define UART_BASE 0xD4017000UL   // D1 晶片 datasheet 寫死的位址
put32(UART_BASE + 0x00, 'A');     // 寫 TX FIFO
char c = get32(UART_BASE + 0x00); // 讀 RX FIFO
```

這種「driver」本質上就是一層很薄的 wrapper，把暫存器讀寫包成 `uart_send()`/`uart_recv()` 供上層使用。16550 UART 的規格是標準的，datasheet 查得到每個 register offset 的意義，實作本身不難。

---

### 9.12 跳 kernel 時參數怎麼傳？函式指標的型態是什麼？

RISC-V 的 calling convention（ABI 規範）：前四個參數走 a0、a1、a2、a3。

```c
// 宣告一個指向「吃四個 unsigned long，不回傳值」的函式指標
void (*next_kernel)(unsigned long, unsigned long, unsigned long, unsigned long);

// 把實體位址 0x20000000 轉型成這個指標型態
next_kernel = (void (*)(...))0x20000000;

// 呼叫 → compiler 自動把參數塞進 a0~a3，然後 jalr 跳過去
next_kernel(g_hartid, g_dtb, g_initrd_start, g_initrd_end);
```

傳的四個參數：
- `hartid` — kernel 需要知道自己在哪個核心上跑
- `dtb_addr` — device tree 位址，kernel 從這裡解析硬體資訊（UART 位址、timer、記憶體大小）
- `initrd_start/end` — initramfs 起訖位址，kernel 從這裡讀取使用者程式

這些資訊都是 kernel 開機必備的，不傳的話 kernel 等於盲人上路。
