CC = gcc
LD = ld

INCLUDES = -Ilib -Isrc -Isrc/phase1 -Isrc/phase2 -Isrc/phase3 -Isrc/phase4 -Isrc/phase5 -Isrc/phase6 -Isrc/phase7 -Isrc/phase8 -Isrc/phase9 -Isrc/phase10 -Isrc/phase11 -Isrc/phase12 -Isrc/phase13 -Isrc/phase14 -Isrc/phase15 -Isrc/phase16 -Isrc/phase17 -Isrc/phase18 -Isrc/phase19 -Isrc/phase20 -Isrc/phase21 -Isrc/phase22 -Isrc/phase23 -Isrc/phase24 -Isrc/phase25 -Isrc/phase26 -Isrc/phase27 -Isrc/phase28 -Isrc/phase29 -Isrc/phase30 -Isrc/phase31 -Isrc/phase32 -Isrc/phase33 -Isrc/phase34 -Isrc/phase35 -Isrc/phase36 -Isrc/phase37 -Isrc/phase38

CFLAGS = -Wall -Wextra -O2 -pipe $(INCLUDES) \
         -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
         -mno-red-zone -mcmodel=kernel -mno-mmx -mno-sse -mno-sse2 \
         -masm=intel
LDFLAGS = -nostdlib -T linker.ld -z max-page-size=0x1000 -z text

CFILES = $(shell find src lib tests -name '*.c')
SFILES = $(shell find src lib tests -name '*.S')
OBJFILES = $(patsubst %.c, build/obj/%.o, $(CFILES)) $(patsubst %.S, build/obj/%.o, $(SFILES))

.PHONY: all clean iso run run-gdb

all: build/kernel.elf

build/kernel.elf: $(OBJFILES) linker.ld
	$(LD) $(LDFLAGS) $(OBJFILES) -o $@

build/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/obj/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build
	rm -f limine/limine
	rm -rf iso_root

limine/limine: limine/limine.c
	$(CC) -O2 -pipe $< -o $@

iso: build/kernel.elf limine/limine
	rm -rf iso_root
	mkdir -p iso_root
	cp build/kernel.elf limine.conf iso_root/
	mkdir -p iso_root/limine
	cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/limine/
	xorriso -as mkisofs -b limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o build/BashketKernel.iso
	./limine/limine bios-install build/BashketKernel.iso

run: iso
	qemu-system-x86_64 -m 2G -cdrom build/BashketKernel.iso -boot d

run-gdb: iso
	qemu-system-x86_64 -m 2G -cdrom build/BashketKernel.iso -boot d -s -S
