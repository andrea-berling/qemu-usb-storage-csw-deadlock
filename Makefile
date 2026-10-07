# Minimal PoC image for the QEMU usb-storage CSW deadlock.
#
# Build:  make            -> poc.img
# Run:    make run        -> boots the image as a USB stick in QEMU
#
# Requirements: gcc (i386 backend), binutils (elf_i386), nasm,
# qemu-system-x86_64.

CC      = gcc
LD      = ld
OBJCOPY = objcopy
NASM    = nasm
NM      = nm
QEMU    ?= qemu-system-x86_64

CFLAGS := -m32 -O2 -g -Wall -Wextra -Wno-unused-parameter -std=c11 \
          -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
          -fno-zero-initialized-in-bss -fno-asynchronous-unwind-tables \
          -fno-builtin -mno-sse -mno-sse2 -mno-mmx

all: poc.img

ehci_msc_bug.o: ehci_msc_bug.c
	$(CC) $(CFLAGS) -c $< -o $@

poc.elf: ehci_msc_bug.o linker.ld
	$(LD) -m elf_i386 -T linker.ld -nostdlib ehci_msc_bug.o -o $@

poc.bin: poc.elf
	$(OBJCOPY) -O binary $< $@

boot.bin: boot.S poc.bin poc.elf
	@SECT=$$(( $$(stat -c %s poc.bin) + 511 )); SECT=$$(( SECT / 512 )); \
	ADDR=$$($(NM) poc.elf | awk '/ cmain$$/ { printf "0x%s", $$1 }'); \
	$(NASM) -f bin -D PAYLOAD_SECTORS=$$SECT -D CMAIN_ADDR=$$ADDR -o $@ $<

poc.img: boot.bin poc.bin
	cat boot.bin poc.bin > $@
	@truncate -s 1048576 $@

run: poc.img
	$(QEMU) -drive if=none,id=usbstick,format=raw,file=poc.img \
		-device usb-ehci,id=ehci \
		-device usb-storage,bus=ehci.0,drive=usbstick \
		-serial stdio -display none

clean:
	rm -f ehci_msc_bug.o poc.elf poc.bin boot.bin poc.img

.PHONY: all run clean
