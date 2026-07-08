CC = gcc
LD = ld

CFLAGS = -Wall -Wextra -O2 -pipe \
         -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
         -mno-red-zone -mcmodel=kernel -mno-mmx -mno-sse -mno-sse2 \
         -masm=intel
LDFLAGS = -nostdlib -T linker.ld -z max-page-size=0x1000 -z text

CFILES = $(wildcard src/*.c)
OBJFILES = $(CFILES:.c=.o)

.PHONY: all clean iso run run-gdb

all: kernel.elf

kernel.elf: $(OBJFILES) linker.ld
	$(LD) $(LDFLAGS) $(OBJFILES) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJFILES) kernel.elf BashketKernel.iso
	rm -f limine/limine
	rm -rf iso_root

limine/limine: limine/limine.c
	$(CC) -O2 -pipe $< -o $@

iso: kernel.elf limine/limine
	rm -rf iso_root
	mkdir -p iso_root
	cp kernel.elf limine.conf iso_root/
	mkdir -p iso_root/limine
	cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/limine/
	xorriso -as mkisofs -b limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o BashketKernel.iso
	./limine/limine bios-install BashketKernel.iso

run: iso
	qemu-system-x86_64 -m 2G -cdrom BashketKernel.iso -boot d

run-gdb: iso
	qemu-system-x86_64 -m 2G -cdrom BashketKernel.iso -boot d -s -S
