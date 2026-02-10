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
ITS_FILE  := bootloader.its
DTB_FILE  := x1_orangepi-rv2.dtb

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
BOARD_LDFLAGS := --defsym=BOOTLOADER_LOAD_ADDR=0x00200000
QEMU_LDFLAGS  := --defsym=BOOTLOADER_LOAD_ADDR=0x80200000

.PHONY: all qemu clean

all: bootloader.elf bootloader.bin bootloader.fit

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/board_%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(BOARD_CFLAGS) -c $< -o $@

$(BUILD_DIR)/board_%.o: $(SRC_DIR)/%.S | $(BUILD_DIR)
	$(CC) $(BOARD_CFLAGS) -c $< -o $@

$(BUILD_DIR)/qemu_%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c $< -o $@

$(BUILD_DIR)/qemu_%.o: $(SRC_DIR)/%.S | $(BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c $< -o $@

$(BUILD_DIR)/bootloader_board.elf: $(BOARD_OBJS) $(LINKER)
	$(LD) $(BOARD_LDFLAGS) -T $(LINKER) -o $@ $(BOARD_OBJS)

$(BUILD_DIR)/bootloader_qemu.elf: $(QEMU_OBJS) $(LINKER)
	$(LD) $(QEMU_LDFLAGS) -T $(LINKER) -o $@ $(QEMU_OBJS)

bootloader.elf: $(BUILD_DIR)/bootloader_board.elf
	cp $< $@

bootloader.bin: $(BUILD_DIR)/bootloader_board.elf
	$(OBJCOPY) -O binary -S $< $@

bootloader.fit: bootloader.bin $(ITS_FILE) $(DTB_FILE)
	$(MKIMAGE) -f $(ITS_FILE) $@

qemu: $(BUILD_DIR)/bootloader_qemu.elf
	$(QEMU) -M virt -smp 1 -m 256M -nographic -bios default -kernel $(BUILD_DIR)/bootloader_qemu.elf -serial pty -monitor none

clean:
	rm -rf $(BUILD_DIR) bootloader.elf bootloader.bin bootloader.fit

-include $(BOARD_DEPS) $(QEMU_DEPS)
