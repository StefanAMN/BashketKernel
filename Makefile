CC = gcc
LD = ld

# Output directory. `make test` uses a separate one so that the test kernel
# and the normal kernel never share object files.
BUILD ?= build

INCLUDES = -Ilib -Isrc -Isrc/include -Isrc/drivers/virtio -Isrc/init -Isrc/arch/x86_64 -Isrc/boot \
           -Isrc/drivers/block -Isrc/drivers/bus -Isrc/drivers/char -Isrc/drivers/console -Isrc/drivers/input -Isrc/drivers/net -Isrc/drivers/tty \
           -Isrc/fs -Isrc/gui/compositor -Isrc/kernel -Isrc/kernel/ipc -Isrc/kernel/log -Isrc/kernel/proc -Isrc/kernel/sched -Isrc/kernel/sync -Isrc/kernel/syscall -Isrc/kernel/time \
           -Isrc/lib -Isrc/mm -Isrc/net

CFLAGS = -Wall -Wextra -O2 -g -pipe $(INCLUDES) \
         -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
         -fno-omit-frame-pointer \
         -mno-red-zone -mcmodel=kernel -mno-mmx -mno-sse -mno-sse2 \
         -masm=intel -MMD -MP
LDFLAGS = -nostdlib -T linker.ld -z max-page-size=0x1000 -z text

SRC_DIRS = src lib

# KERNEL_TESTS=1 compiles the in-kernel unit tests and makes the kernel run
# them at boot, then power off QEMU with a pass/fail exit status.
ifeq ($(KERNEL_TESTS),1)
CFLAGS += -DKERNEL_TESTS
SRC_DIRS += tests
endif

CFILES = $(shell find $(SRC_DIRS) -name '*.c')
SFILES = $(shell find $(SRC_DIRS) -name '*.S')
OBJFILES = $(patsubst %.c, $(BUILD)/obj/%.o, $(CFILES)) $(patsubst %.S, $(BUILD)/obj/%.o, $(SFILES))

KERNEL = $(BUILD)/kernel.elf
ISO = $(BUILD)/BashketKernel.iso
ISO_ROOT = $(BUILD)/iso_root

# --- User programs and the initrd -------------------------------------------
# Every user/*.c becomes /bin/<name> in the initrd, linked against the small
# library in user/lib. User code may use floating point and SSE: the kernel
# saves that state per task.
USER_CFLAGS = -Wall -Wextra -O2 -g -pipe -Iuser/lib \
              -ffreestanding -fno-stack-protector -fpie \
              -masm=intel -MMD -MP
# Static position-independent executables: no dynamic linker, and no
# relocations in the code itself (-z text makes that an error)
USER_LDFLAGS = -nostdlib -static -pie --no-dynamic-linker -z text -T user/linker.ld -z max-page-size=0x1000

USER_LIB_OBJS = $(BUILD)/user/lib/crt0.o $(BUILD)/user/lib/ulib.o
USER_PROGS = $(patsubst user/%.c, $(BUILD)/initrd/bin/%, $(wildcard user/*.c))
USER_ROOTFS = $(shell find user/rootfs -type f)
INITRD = $(BUILD)/initrd.tar

# (User objects also depend on this Makefile: a change of compiler flags, such
# as switching to position-independent code, must rebuild them.)
$(BUILD)/user/%.o: user/%.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/%.o: user/%.S Makefile
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/initrd/bin/%: $(BUILD)/user/%.o $(USER_LIB_OBJS) user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(BUILD)/user/lib/crt0.o $< $(BUILD)/user/lib/ulib.o -o $@

# USTAR archive of bin/ plus the static files under user/rootfs
$(INITRD): $(USER_PROGS) $(USER_ROOTFS)
	cp -r user/rootfs/. $(BUILD)/initrd/
	tar --format=ustar -C $(BUILD)/initrd -cf $@ $$(ls $(BUILD)/initrd)

.PRECIOUS: $(BUILD)/user/%.o

QEMU = qemu-system-x86_64
# The disk QEMU presents as a virtio block device. `make run` writes to it
# for real, so what is stored there survives a reboot.
DISK = $(BUILD)/disk.img
DISK_FLAGS = -drive file=$(DISK),if=virtio,format=raw

# A network card on QEMU's built-in user-mode network: the guest is
# 10.0.2.15 and a simulated gateway answers at 10.0.2.2. Needs nothing
# configured on the host.
NET_FLAGS = -netdev user,id=n0 -device virtio-net-pci,netdev=n0

QEMU_FLAGS = -m 2G -cdrom $(ISO) -boot d $(DISK_FLAGS) $(NET_FLAGS)

.PHONY: all clean iso run run-gdb test test-smp test-lowmem test-disk test-edit test-net fuzz check

all: $(KERNEL)

$(KERNEL): $(OBJFILES) linker.ld
	$(LD) $(LDFLAGS) $(OBJFILES) -o $@

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build build-test
	rm -f limine/limine
	rm -rf iso_root

# Limine is not vendored. Fetch the pinned binary release on first build.
LIMINE_TAG = v8.7.0-binary

limine/limine.c:
	git clone --branch=$(LIMINE_TAG) --depth=1 https://github.com/limine-bootloader/limine.git limine

limine/limine: limine/limine.c
	$(CC) -O2 -pipe $< -o $@

iso: $(ISO) $(DISK)

$(ISO): $(KERNEL) $(INITRD) limine/limine limine.conf
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/limine
	cp $(KERNEL) $(INITRD) limine.conf $(ISO_ROOT)/
	cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin $(ISO_ROOT)/limine/
	xorriso -as mkisofs -b limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ISO_ROOT) -o $(ISO) 2>/dev/null
	./limine/limine bios-install $(ISO) 2>/dev/null

# A FAT32 image holding the files under disk/ plus one program, so that
# something can be executed straight from the disk. fsck.fat (if installed)
# checks the image: it is an independent implementation of the format.
DISK_FILES = $(shell find disk -type f)
$(DISK): tools/mkfat.py $(DISK_FILES) $(BUILD)/initrd/bin/hello
	@mkdir -p $(BUILD)
	tools/mkfat.py $@ 64 disk $(BUILD)/initrd/bin/hello=bin/hello
	@if command -v fsck.fat >/dev/null; then fsck.fat -n $@ >/dev/null || { echo "fsck.fat rejects $@"; rm -f $@; exit 1; }; fi

run: iso $(DISK)
	$(QEMU) $(QEMU_FLAGS) -serial mon:stdio

run-gdb: iso
	$(QEMU) $(QEMU_FLAGS) -s -S

# CPU=max runs the test targets on QEMU's fullest CPU model, which has the
# protections the default model lacks (SMEP, SMAP, UMIP): make test CPU=max
CPU ?=
QEMU_CPU = $(if $(CPU),-cpu $(CPU))
SHELL_CPU = $(if $(CPU),--qemu-arg=-cpu --qemu-arg=$(CPU))

# Build the test kernel, boot it headless, and turn its verdict into an exit
# status. The kernel writes to the isa-debug-exit port when the tests finish;
# QEMU then exits with (value << 1) | 1, so 0x10 (pass) becomes 33.
test:
	@$(MAKE) --no-print-directory BUILD=build-test KERNEL_TESTS=1 iso build-test/disk.img
	@timeout 120 $(QEMU) $(QEMU_CPU) -m 2G -cdrom build-test/BashketKernel.iso -boot d \
		-drive file=build-test/disk.img,if=virtio,format=raw,snapshot=on $(NET_FLAGS) \
		-display none -serial stdio -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 > build-test/test.log 2>&1; \
	status=$$?; \
	grep -E '^\s+\[FAIL\]|CPU EXCEPTION|KERNEL PANIC' build-test/test.log; \
	sed -n '/--- Test Summary ---/,/^----------/p' build-test/test.log; \
	if [ $$status -eq 33 ]; then echo "TESTS PASSED"; \
	elif [ $$status -eq 124 ]; then echo "TESTS TIMED OUT (see build-test/test.log)"; exit 1; \
	else echo "TESTS FAILED (qemu exit $$status, see build-test/test.log)"; exit 1; fi

# Run the resource-exhaustion scenarios (user/stress.c) on a machine with
# little RAM. Each must end with the kernel refusing politely and everything
# still working; a crash, a hang or a "FAILED" line fails the target.
test-lowmem: iso
	@for mode in mem brk fork cow cowsys file; do \
		tools/qemu_shell.py --quiet --mem 64M --timeout 120 $(SHELL_CPU) \
			--expect "stress $$mode: ok" --forbid FAILED \
			"exec /bin/stress $$mode" 'exec /bin/hello' > $(BUILD)/lowmem-$$mode.log 2>&1 \
			&& echo "lowmem $$mode: ok" \
			|| { echo "lowmem $$mode: FAILED (see $(BUILD)/lowmem-$$mode.log)"; exit 1; }; \
	done

# Random system calls with hostile arguments (user/fuzz.c). The kernel must
# survive every seed and still pass the system call checks afterwards.
# Success is judged by exit statuses, not by text the fuzzer prints: a fuzzed
# write() can dump the fuzzer's own memory, string constants included, to the
# console. The kernel shell reports a program it ran that exits with a
# non-zero status, in words no user program contains.
# Seeds are fixed so that a failure can be reproduced: make fuzz FUZZ_SEEDS="7"
FUZZ_SEEDS ?= 1 2 3 4 5 6 7 8
FUZZ_ROUNDS ?= 40
fuzz: iso
	@for seed in $(FUZZ_SEEDS); do \
		tools/qemu_shell.py --quiet --timeout 300 $(SHELL_CPU) \
			--expect 'syscheck: all checks passed' --forbid '\[pid [0-9]+ exited with status' \
			"exec /bin/fuzz $$seed $(FUZZ_ROUNDS)" 'exec /bin/syscheck' > $(BUILD)/fuzz-$$seed.log 2>&1 \
			&& echo "fuzz seed $$seed: ok ($$(grep -a -o '[0-9]* ended normally.*' $(BUILD)/fuzz-$$seed.log))" \
			|| { echo "fuzz seed $$seed: FAILED (see $(BUILD)/fuzz-$$seed.log)"; exit 1; }; \
	done

# The disk keeps its contents across reboots: write a sector in one boot of
# the kernel, read it in a second boot, and look for it in the image file
# from the host as well. Then the same for files: create, append, copy,
# remove and overwrite in one boot, have fsck.fat check the volume, and read
# everything back in a second boot. Finally boot against deliberately damaged
# copies of the FAT32 image (tools/corrupt_fat.py) and read and write on
# them: the kernel must neither fault nor hang, whatever the disk says.
test-disk: iso
	@rm -f $(BUILD)/persist.img && truncate -s 64M $(BUILD)/persist.img
	@tools/qemu_shell.py --quiet --disk $(BUILD)/persist.img --keep-disk-writes $(SHELL_CPU) \
		'exec /bin/diskio write 1000 "written in boot one"' > $(BUILD)/disk-boot1.log 2>&1 \
		|| { echo "disk: FAILED in the first boot (see $(BUILD)/disk-boot1.log)"; exit 1; }
	@tools/qemu_shell.py --quiet --disk $(BUILD)/persist.img $(SHELL_CPU) \
		--expect '\nwritten in boot one' 'exec /bin/diskio read 1000' > $(BUILD)/disk-boot2.log 2>&1 \
		|| { echo "disk: FAILED, the second boot did not read the data back (see $(BUILD)/disk-boot2.log)"; exit 1; }
	@dd if=$(BUILD)/persist.img bs=512 skip=1000 count=1 2>/dev/null | grep -aq 'written in boot one' \
		|| { echo "disk: FAILED, the data is not in the image file"; exit 1; }
	@echo "disk persistence (raw sectors): ok"
	@cp $(DISK) $(BUILD)/fs-persist.img
	@tools/qemu_shell.py --quiet --timeout 120 --disk $(BUILD)/fs-persist.img --keep-disk-writes $(SHELL_CPU) \
		--forbid 'invalid|no such|error' \
		'exec /bin/sh' 'cd /disk' 'echo saved in boot one > note.txt' 'echo first > log.txt' \
		'echo second >> log.txt' 'cat pattern.bin > copy.bin' 'mkdir work' 'echo inside > work/a.txt' \
		'echo gone > temp.txt' 'rm temp.txt' 'rm notes/empty.txt' 'echo replaced > readme.txt' 'exit' \
		> $(BUILD)/fs-boot1.log 2>&1 \
		|| { echo "filesystem: FAILED in the first boot (see $(BUILD)/fs-boot1.log)"; exit 1; }
	@if command -v fsck.fat >/dev/null; then fsck.fat -n $(BUILD)/fs-persist.img > $(BUILD)/fs-fsck.log 2>&1 \
		|| { echo "filesystem: FAILED, fsck.fat rejects the volume after writing (see $(BUILD)/fs-fsck.log)"; exit 1; }; fi
	@tools/qemu_shell.py --quiet --timeout 60 --disk $(BUILD)/fs-persist.img $(SHELL_CPU) \
		--expect 'saved in boot one\nfirst\nsecond\ninside\nreplaced\n' --expect '400 1201 102400' \
		--expect 'temp.txt: no such file' --forbid 'empty.txt' \
		'exec /bin/sh' 'cd /disk' 'cat note.txt log.txt work/a.txt readme.txt' 'wc copy.bin' \
		'ls notes' 'cat temp.txt' 'exit' > $(BUILD)/fs-boot2.log 2>&1 \
		|| { echo "filesystem: FAILED, the second boot does not see the files (see $(BUILD)/fs-boot2.log)"; exit 1; }
	@echo "disk persistence (files, checked by fsck.fat): ok"
	@for kind in file-loop dir-loop dir-cycle fat-garbage huge-size bad-cluster; do \
		tools/corrupt_fat.py $(DISK) $$kind $(BUILD)/damaged.img && \
		tools/qemu_shell.py --quiet --timeout 90 --disk $(BUILD)/damaged.img $(SHELL_CPU) \
			--expect 'Hello from ring 3' \
			'exec /bin/ls /disk' 'exec /bin/sh -c "wc /disk/pattern.bin"' \
			'exec /bin/sh -c "cat /disk/readme.txt | wc"' 'exec /bin/sh -c "ls /disk/notes | wc"' \
			'exec /bin/sh -c "echo written > /disk/w.txt"' 'exec /bin/sh -c "cat /disk/pattern.bin >> /disk/readme.txt"' \
			'exec /bin/sh -c "echo y > /disk/notes/n.txt"' 'exec /bin/sh -c "rm /disk/pattern.bin /disk/readme.txt"' \
			'exec /bin/sh -c "mkdir /disk/d1"' 'exec /bin/hello' > $(BUILD)/damaged-$$kind.log 2>&1 \
			&& echo "damaged disk ($$kind): survived" \
			|| { echo "damaged disk ($$kind): FAILED (see $(BUILD)/damaged-$$kind.log)"; exit 1; }; \
	done

# The full-screen editor, driven by scripted keystrokes: type, move with the
# arrow keys, insert in three places, save, quit; then check the file. Also
# that Ctrl-C does not kill it and that the terminal is back in line mode
# afterwards (the shell prompt echoes what is typed).
test-edit: iso
	@tools/qemu_shell.py --quiet --timeout 60 $(SHELL_CPU) \
		--expect 'first line tail\nX-second line\nthird!\n' --expect '\n2 6 36\n' --expect 'still-in-line-mode' \
		'!exec /bin/edit /tmp/t.txt' '~first line\rsecond line\rthird' '~\e[A\e[HX-\e[B\e[F!' \
		'~\e[A\e[A\e[F tail' '~^C' '~^S^Q' 'cat tmp/t.txt' 'exec /bin/sh -c "wc /tmp/t.txt"' \
		'echo still-in-line-mode' > $(BUILD)/edit.log 2>&1 \
		&& echo "editor: ok" || { echo "editor: FAILED (see $(BUILD)/edit.log)"; exit 1; }

# Real UDP datagrams between the host and the kernel, in both directions
# (see tools/test_net.py). Uses only the loopback interface of the host.
test-net: iso
	@tools/test_net.py $(SHELL_CPU) > $(BUILD)/net.log 2>&1 \
		&& tail -1 $(BUILD)/net.log || { echo "network: FAILED (see $(BUILD)/net.log)"; exit 1; }

# A machine with four CPUs: all four must start and report in (they are then
# parked), and everything else must work exactly as with one.
test-smp:
	@$(MAKE) --no-print-directory BUILD=build-test KERNEL_TESTS=1 iso build-test/disk.img
	@timeout 120 $(QEMU) $(QEMU_CPU) -smp 4 -m 2G -cdrom build-test/BashketKernel.iso -boot d \
		-drive file=build-test/disk.img,if=virtio,format=raw,snapshot=on $(NET_FLAGS) \
		-display none -serial stdio -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 > build-test/test-smp.log 2>&1; \
	status=$$?; \
	if [ $$status -eq 33 ] && grep -aq 'SMP: 4 of 4 CPUs started' build-test/test-smp.log; then \
		echo "four CPUs: all started, unit tests pass"; \
	else echo "four CPUs: FAILED (qemu exit $$status, see build-test/test-smp.log)"; \
		grep -aE '^\s+\[FAIL\]|CPU EXCEPTION|SMP:' build-test/test-smp.log | head -5; exit 1; fi

# Everything, on both CPU models: what to run before calling a change done.
# Takes about three minutes.
check:
	@$(MAKE) --no-print-directory test
	@$(MAKE) --no-print-directory test CPU=max
	@$(MAKE) --no-print-directory test-smp
	@$(MAKE) --no-print-directory test-lowmem
	@$(MAKE) --no-print-directory test-lowmem CPU=max
	@$(MAKE) --no-print-directory test-disk
	@$(MAKE) --no-print-directory test-edit
	@$(MAKE) --no-print-directory test-net
	@$(MAKE) --no-print-directory test-net CPU=max
	@$(MAKE) --no-print-directory fuzz FUZZ_SEEDS="1 2 3"
	@$(MAKE) --no-print-directory fuzz CPU=max FUZZ_SEEDS="4 5"
	@echo "ALL CHECKS PASSED"

# Header dependencies generated by -MMD, so editing a header rebuilds its users
-include $(OBJFILES:.o=.d) $(wildcard $(BUILD)/user/*.d $(BUILD)/user/lib/*.d)
