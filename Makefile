.PHONY: all bootloader kernel qemu clean

all: bootloader kernel

bootloader:
	$(MAKE) -C bootloader all

kernel:
	$(MAKE) -C kernel all

qemu:
	$(MAKE) -C kernel qemu

clean:
	$(MAKE) -C bootloader clean
	$(MAKE) -C kernel clean
