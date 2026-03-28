CROSS_COMPILE ?= riscv64-unknown-elf-
CC      := $(CROSS_COMPILE)gcc
LD      := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
MKIMAGE := mkimage
QEMU    := qemu-system-riscv64

SRC_DIR   := src
BUILD_DIR := build
INC_DIR   := include
LINKER    := linker.ld
ITS_FILE  := kernel.its
DTB_FILE  := x1_orangepi-rv2.dtb
ROOTFS_DIR := rootfs
INITRAMFS := initramfs.cpio
ROOTFS_FILES := $(shell [ -d $(ROOTFS_DIR) ] && find $(ROOTFS_DIR) -print)
USER_DIR   := user
USER_BUILD_DIR := $(BUILD_DIR)/user
USER_ROOTFS_DIR := $(ROOTFS_DIR)/bin
USER_TEST_SRC := $(USER_DIR)/test.S
USER_TEST_ELF := $(USER_BUILD_DIR)/test.elf
USER_TEST_BIN := $(USER_ROOTFS_DIR)/test.bin
USER_BADINST_SRC := $(USER_DIR)/badinst.S
USER_BADINST_ELF := $(USER_BUILD_DIR)/badinst.elf
USER_BADINST_BIN := $(USER_ROOTFS_DIR)/badinst.bin
USER_FORK_START_SRC := $(USER_DIR)/fork_start.S
USER_FORK_SRC := $(USER_DIR)/fork.c
USER_FORK_ELF := $(USER_BUILD_DIR)/fork.elf
USER_FORK_BIN := $(USER_ROOTFS_DIR)/fork.bin
USER_PREEMPT_START_SRC := $(USER_DIR)/preempt_start.S
USER_PREEMPT_SRC := $(USER_DIR)/preempt.c
USER_PREEMPT_ELF := $(USER_BUILD_DIR)/preempt.elf
USER_PREEMPT_BIN := $(USER_ROOTFS_DIR)/preempt.bin

C_SRCS    := $(wildcard $(SRC_DIR)/*.c)
ASM_SRCS  := $(wildcard $(SRC_DIR)/*.S)
BASEN     := $(basename $(notdir $(C_SRCS) $(ASM_SRCS)))
BOARD_OBJS := $(addprefix $(BUILD_DIR)/board_,$(addsuffix .o,$(BASEN)))
QEMU_OBJS  := $(addprefix $(BUILD_DIR)/qemu_,$(addsuffix .o,$(BASEN)))
BOARD_DEPS := $(BOARD_OBJS:.o=.d)
QEMU_DEPS  := $(QEMU_OBJS:.o=.d)

COMMON_CFLAGS := -mcmodel=medany -ffreestanding -nostdlib -g -Wall -I$(INC_DIR) -MMD -MP
BOARD_CFLAGS := $(COMMON_CFLAGS) -DBOARD
QEMU_CFLAGS  := $(COMMON_CFLAGS) -DQEMU
BOARD_LDFLAGS := --defsym=KERNEL_LOAD_ADDR=0x20000000
QEMU_LDFLAGS  := --defsym=KERNEL_LOAD_ADDR=0x82000000
USER_LINK_ADDR := 0x32000000

.PHONY: all qemu clean

all: kernel.elf kernel.bin kernel.fit

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(USER_BUILD_DIR): | $(BUILD_DIR)
	mkdir -p $(USER_BUILD_DIR)

$(USER_ROOTFS_DIR):
	mkdir -p $(USER_ROOTFS_DIR)

$(BUILD_DIR)/board_%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(BOARD_CFLAGS) -c $< -o $@

$(BUILD_DIR)/board_%.o: $(SRC_DIR)/%.S | $(BUILD_DIR)
	$(CC) $(BOARD_CFLAGS) -c $< -o $@

$(BUILD_DIR)/qemu_%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c $< -o $@

$(BUILD_DIR)/qemu_%.o: $(SRC_DIR)/%.S | $(BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_board.elf: $(BOARD_OBJS) $(LINKER)
	$(LD) $(BOARD_LDFLAGS) -T $(LINKER) -o $@ $(BOARD_OBJS)

$(BUILD_DIR)/kernel_qemu.elf: $(QEMU_OBJS) $(LINKER)
	$(LD) $(QEMU_LDFLAGS) -T $(LINKER) -o $@ $(QEMU_OBJS)

kernel.elf: $(BUILD_DIR)/kernel_board.elf
	cp $< $@

kernel.bin: $(BUILD_DIR)/kernel_board.elf
	$(OBJCOPY) -O binary -S $< $@

$(USER_TEST_ELF): $(USER_TEST_SRC) | $(USER_BUILD_DIR)
	$(CC) $(BOARD_CFLAGS) -nostartfiles -Wl,-Ttext=$(USER_LINK_ADDR) -Wl,-e,_start -o $@ $<

$(USER_TEST_BIN): $(USER_TEST_ELF) | $(USER_ROOTFS_DIR)
	$(OBJCOPY) -O binary -S $< $@

$(USER_BADINST_ELF): $(USER_BADINST_SRC) | $(USER_BUILD_DIR)
	$(CC) $(BOARD_CFLAGS) -nostartfiles -Wl,-Ttext=$(USER_LINK_ADDR) -Wl,-e,_start -o $@ $<

$(USER_BADINST_BIN): $(USER_BADINST_ELF) | $(USER_ROOTFS_DIR)
	$(OBJCOPY) -O binary -S $< $@

$(USER_FORK_ELF): $(USER_FORK_START_SRC) $(USER_FORK_SRC) | $(USER_BUILD_DIR)
	$(CC) $(BOARD_CFLAGS) -nostartfiles -Wl,-Ttext=$(USER_LINK_ADDR) -Wl,-e,_start -o $@ $(USER_FORK_START_SRC) $(USER_FORK_SRC)

$(USER_FORK_BIN): $(USER_FORK_ELF) | $(USER_ROOTFS_DIR)
	$(OBJCOPY) -O binary -S $< $@

$(USER_PREEMPT_ELF): $(USER_PREEMPT_START_SRC) $(USER_PREEMPT_SRC) | $(USER_BUILD_DIR)
	$(CC) $(BOARD_CFLAGS) -nostartfiles -Wl,-Ttext=$(USER_LINK_ADDR) -Wl,-e,_start -o $@ $(USER_PREEMPT_START_SRC) $(USER_PREEMPT_SRC)

$(USER_PREEMPT_BIN): $(USER_PREEMPT_ELF) | $(USER_ROOTFS_DIR)
	$(OBJCOPY) -O binary -S $< $@

$(INITRAMFS): $(ROOTFS_FILES) $(USER_TEST_BIN) $(USER_BADINST_BIN) $(USER_FORK_BIN) $(USER_PREEMPT_BIN)
	@if [ ! -d "$(ROOTFS_DIR)" ]; then \
		echo "Missing $(ROOTFS_DIR)/ directory"; \
		exit 1; \
	fi
	cd $(ROOTFS_DIR) && find . -print0 | LC_ALL=C sort -z | cpio --null -o -H newc --owner=0:0 > ../$@

kernel.fit: kernel.bin $(ITS_FILE) $(DTB_FILE) $(INITRAMFS)
	$(MKIMAGE) -f $(ITS_FILE) $@

qemu: $(BUILD_DIR)/kernel_qemu.elf $(INITRAMFS)
	$(QEMU) -M virt -smp 1 -m 256M -nographic -bios default -kernel $(BUILD_DIR)/kernel_qemu.elf -initrd $(INITRAMFS)

qemu-bin: $(BUILD_DIR)/kernel_qemu.elf
	$(OBJCOPY) -O binary -S $< build/kernel_qemu.bin

clean:
	rm -rf $(BUILD_DIR) kernel.elf kernel.bin kernel.fit $(INITRAMFS) $(USER_TEST_BIN)

-include $(BOARD_DEPS) $(QEMU_DEPS)
