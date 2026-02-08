CROSS_COMPILE ?= riscv64-unknown-elf-
CC      := $(CROSS_COMPILE)gcc
LD      := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
MKIMAGE := mkimage
QEMU    := qemu-system-riscv64

SRC_DIR    := src
BUILD_DIR  := build
INC_DIR    := include
LINKER     := $(SRC_DIR)/linker.ld
ITS_FILE   := kernel.its
DTB_FILE   := x1_orangepi-rv2.dtb

C_SRCS     := $(wildcard $(SRC_DIR)/*.c)
ASM_SRCS   := $(wildcard $(SRC_DIR)/*.S)
SRC_BASEN  := $(basename $(notdir $(C_SRCS) $(ASM_SRCS)))

BOARD_OBJS := $(addprefix $(BUILD_DIR)/board_,$(addsuffix .o,$(SRC_BASEN)))
QEMU_OBJS  := $(addprefix $(BUILD_DIR)/qemu_,$(addsuffix .o,$(SRC_BASEN)))
BOARD_DEPS := $(BOARD_OBJS:.o=.d)
QEMU_DEPS  := $(QEMU_OBJS:.o=.d)

COMMON_CFLAGS := -mcmodel=medany -ffreestanding -nostdlib -g -Wall -I$(INC_DIR) -MMD -MP
BOARD_CFLAGS  := $(COMMON_CFLAGS) -DBOARD
QEMU_CFLAGS   := $(COMMON_CFLAGS) -DQEMU

.PHONY: all qemu clean

all: kernel.elf kernel.bin kernel.fit

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

$(BUILD_DIR)/kernel_board.elf: $(BOARD_OBJS) $(LINKER)
	$(LD) -T $(LINKER) -o $@ $(BOARD_OBJS)

$(BUILD_DIR)/kernel_qemu.elf: $(QEMU_OBJS) $(LINKER)
	$(LD) -T $(LINKER) -o $@ $(QEMU_OBJS)

kernel.elf: $(BUILD_DIR)/kernel_board.elf
	cp $< $@

kernel.bin: $(BUILD_DIR)/kernel_board.elf
	$(OBJCOPY) -O binary -S $< $@

kernel.fit: kernel.bin $(ITS_FILE) $(DTB_FILE)
	$(MKIMAGE) -f $(ITS_FILE) $@

qemu: $(BUILD_DIR)/kernel_qemu.elf
	$(QEMU) -M virt -smp 1 -m 256M -nographic -bios default -kernel $(BUILD_DIR)/kernel_qemu.elf

clean:
	rm -rf $(BUILD_DIR) kernel.elf kernel.bin kernel.fit

-include $(BOARD_DEPS) $(QEMU_DEPS)
