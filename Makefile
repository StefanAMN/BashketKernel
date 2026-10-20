CC = gcc
LD = ld

# Output directory. `make test` uses a separate one so that the test kernel
# and the normal kernel never share object files.
BUILD ?= build

INCLUDES = -Ilib -Isrc -Isrc/include -Isrc/drivers/virtio -Isrc/init -Isrc/arch/x86_64 -Isrc/boot \
           -Isrc/drivers/block -Isrc/drivers/bus -Isrc/drivers/char -Isrc/drivers/console -Isrc/drivers/input -Isrc/drivers/net -Isrc/drivers/tty \
           -Isrc/fs -Isrc/kernel -Isrc/kernel/ipc -Isrc/kernel/log -Isrc/kernel/proc -Isrc/kernel/sched -Isrc/kernel/sync -Isrc/kernel/syscall -Isrc/kernel/time \
           -Isrc/lib -Isrc/mm -Isrc/net

CFLAGS = -std=gnu17 -Wall -Wextra -O2 -g -pipe $(INCLUDES) \
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
DEV_ISO = $(BUILD)/BashketKernel-dev.iso
ISO_ROOT = $(BUILD)/iso_root

# --- User programs and the initrd -------------------------------------------
# Every user/*.c becomes /bin/<name> in the initrd, linked against the small
# library in user/lib. User code may use floating point and SSE: the kernel
# saves that state per task.
USER_CFLAGS = -std=gnu17 -Wall -Wextra -O2 -g -pipe -Iuser/lib -Ilib/crypto -Ilib/time \
              -ffreestanding -fno-stack-protector -fpie \
              -masm=intel -MMD -MP
# Static position-independent executables: no dynamic linker, and no
# relocations in the code itself (-z text makes that an error)
USER_LDFLAGS = -nostdlib -static -pie --no-dynamic-linker -z text -T user/linker.ld -z max-page-size=0x1000

# sha256.o and date.o are the kernel tree's own lib/crypto/sha256.c and
# lib/time/date.c, compiled again for user programs (login hashes passwords
# with the one; date and ls turn seconds into dates with the other)
USER_LIB_OBJS = $(BUILD)/user/lib/crt0.o $(BUILD)/user/lib/ulib.o $(BUILD)/user/lib/sha256.o $(BUILD)/user/lib/date.o $(BUILD)/user/lib/password.o $(BUILD)/user/lib/lineedit.o $(BUILD)/user/lib/glob.o $(BUILD)/user/lib/lines.o
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

$(BUILD)/user/lib/sha256.o: lib/crypto/sha256.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/lib/date.o: lib/time/date.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/initrd/bin/%: $(BUILD)/user/%.o $(USER_LIB_OBJS) user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(BUILD)/user/lib/crt0.o $< $(BUILD)/user/lib/ulib.o $(BUILD)/user/lib/password.o $(BUILD)/user/lib/sha256.o $(BUILD)/user/lib/date.o $(BUILD)/user/lib/lineedit.o $(BUILD)/user/lib/glob.o $(BUILD)/user/lib/lines.o -o $@

# USTAR archive of bin/ plus the static files under user/rootfs
$(INITRD): $(USER_PROGS) $(USER_ROOTFS)
	cp -r user/rootfs/. $(BUILD)/initrd/
	# Set-uid root: these two do for an ordinary user what only root may
	# (see src/kernel/proc/auth.h). The archive carries the bit.
	chmod 4755 $(BUILD)/initrd/bin/su $(BUILD)/initrd/bin/passwd
	tar --format=ustar -C $(BUILD)/initrd -cf $@ $$(ls $(BUILD)/initrd)

.PRECIOUS: $(BUILD)/user/%.o

QEMU = qemu-system-x86_64
# The disk QEMU presents as a virtio block device. `make run` writes to it
# for real, so what is stored there survives a reboot.
DISK = $(BUILD)/disk.img
# The system disk: an ext2 volume that the kernel mounts as "/", with the
# programs, /etc and the users' home directories (tools/mkroot.sh). Built
# with mke2fs and debugfs; without them (e2fsprogs) there is no system disk,
# and the kernel runs from the boot module instead (the rescue system).
ROOT_IMG = $(BUILD)/root.img
HOME_FILES = $(shell find home -type f | sed 's/ /\\ /g')
HAVE_E2FS = $(shell command -v mke2fs >/dev/null && command -v debugfs >/dev/null && command -v e2fsck >/dev/null && echo yes)
ifeq ($(HAVE_E2FS),yes)
DISKS = $(DISK) $(ROOT_IMG)
ROOT_FLAGS = -drive file=$(ROOT_IMG),if=virtio,format=raw
else
DISKS = $(DISK)
ROOT_FLAGS =
endif
DISK_FLAGS = -drive file=$(DISK),if=virtio,format=raw $(ROOT_FLAGS)

# A swap disk, made by the host's mkswap (so the kernel's reading of the
# header is checked against the program that defines it). Attached to the
# unit tests and to `make test-swap` only: with it, a machine can promise
# more memory than it has, which is not what the other scenarios are about.
SWAP_IMG = $(BUILD)/swap.img
MKSWAP = $(shell command -v mkswap || ls /sbin/mkswap /usr/sbin/mkswap 2>/dev/null | head -1)
ifneq ($(MKSWAP),)
DISKS += $(SWAP_IMG)
$(SWAP_IMG):
	@mkdir -p $(BUILD)
	truncate -s 64M $@
	$(MKSWAP) $@ > /dev/null
SWAP_TEST_FLAGS = -drive file=build-test/swap.img,if=virtio,format=raw,snapshot=on
endif

# A network card on QEMU's built-in user-mode network: the guest is
# 10.0.2.15 and a simulated gateway answers at 10.0.2.2. Needs nothing
# configured on the host.
NET_FLAGS = -netdev user,id=n0 -device virtio-net-pci,netdev=n0

QEMU_FLAGS = -m 2G -cdrom $(ISO) -boot d $(DISK_FLAGS) $(NET_FLAGS)

comma := ,

.PHONY: disks all clean iso run run-dev run-gdb test test-smp test-lowmem test-disk test-home test-rescue test-kshell test-nmi test-boot test-shutdown test-crash test-paint test-swap test-edit test-login test-jobs test-tools test-net fuzz check

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

iso: $(ISO) $(DEV_ISO) $(DISKS)

# A bootable CD image: the kernel, the boot module and Limine, with a
# limine.conf made from the one in the tree by $(3) (a filter command).
# $(1): the image, $(2): a directory to assemble it in.
define make_iso
	rm -rf $(2)
	mkdir -p $(2)/limine
	cp $(KERNEL) $(INITRD) $(2)/
	$(3) limine.conf > $(2)/limine.conf
	cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin $(2)/limine/
	xorriso -as mkisofs -b limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(2) -o $(1) 2>/dev/null
	./limine/limine bios-install $(1) 2>/dev/null
endef

# The system: boots to the login prompt (after the menu's few seconds)
$(ISO): $(KERNEL) $(INITRD) limine/limine limine.conf
	$(call make_iso,$@,$(ISO_ROOT),cat)

# For development and the scripted tests: no menu, straight to the second
# entry, the kernel's own shell
$(DEV_ISO): $(KERNEL) $(INITRD) limine/limine limine.conf
	$(call make_iso,$@,$(ISO_ROOT)-dev,sed -e 's/^timeout: .*/timeout: 0\ndefault_entry: 2/')

# A FAT32 image holding the files under disk/ plus one program, so that
# something can be executed straight from the disk. fsck.fat (if installed)
# checks the image: it is an independent implementation of the format.
DISK_FILES = $(shell find disk -type f | sed 's/ /\\ /g')
disks: $(DISKS)

$(ROOT_IMG): tools/mkroot.sh $(INITRD) $(HOME_FILES)
	tools/mkroot.sh $@ 64M $(BUILD)/initrd home

$(DISK): tools/mkfat.py $(DISK_FILES) $(BUILD)/initrd/bin/hello
	@mkdir -p $(BUILD)
	tools/mkfat.py $@ 64 disk $(BUILD)/initrd/bin/hello=bin/hello
	@if command -v fsck.fat >/dev/null; then fsck.fat -n $@ >/dev/null || { echo "fsck.fat rejects $@"; rm -f $@; exit 1; }; fi

# The system, as a user would start it: boot menu, then the login prompt
run: iso $(DISKS)
	$(QEMU) $(QEMU_FLAGS) -serial mon:stdio

# The same with the kernel's own shell on the console, for development
run-dev: iso $(DISKS)
	$(QEMU) $(subst $(ISO),$(DEV_ISO),$(QEMU_FLAGS)) -serial mon:stdio

run-gdb: iso
	$(QEMU) $(subst $(ISO),$(DEV_ISO),$(QEMU_FLAGS)) -s -S

# CPU=max runs the test targets on QEMU's fullest CPU model, which has the
# protections the default model lacks (SMEP, SMAP, UMIP): make test CPU=max
CPU ?=

# A CPU model without a timestamp counter: the kernel can then not read the
# time off the CPU, and has to keep its periodic 100 Hz tick (see
# src/kernel/time/timer.h). Run so that the fallback stays tested.
NO_TSC_CPU = qemu64,-tsc

# VIRTIO=legacy or VIRTIO=modern gives the disks and the network card only
# that one of virtio's two register interfaces (see
# src/drivers/virtio/virtio_pci.h). By default QEMU's devices offer both and
# the kernel uses the modern one. Each run also checks that the interface
# in use is the one intended: a driver that quietly fell back to the other
# would pass every other test.
VIRTIO ?=
VIRTIO_OFF = $(if $(filter legacy,$(VIRTIO)),disable-modern=on,$(if $(filter modern,$(VIRTIO)),disable-legacy=on))
VIRTIO_GLOBALS = $(if $(VIRTIO_OFF),-global virtio-blk-pci.$(VIRTIO_OFF) -global virtio-net-pci.$(VIRTIO_OFF))
VIRTIO_UNWANTED = $(if $(filter legacy,$(VIRTIO)),modern,legacy) interface

QEMU_CPU = $(if $(CPU),-cpu $(CPU)) $(VIRTIO_GLOBALS)
# SMP=n runs any of the scripted targets below on a machine with n CPUs
SHELL_CPU = $(if $(CPU),--qemu-arg=-cpu --qemu-arg=$(CPU)) $(if $(SMP),--qemu-arg=-smp --qemu-arg=$(SMP)) \
	$(foreach arg,$(VIRTIO_GLOBALS),--qemu-arg=$(arg)) --forbid '$(VIRTIO_UNWANTED)'

# Build the test kernel, boot it headless, and turn its verdict into an exit
# status. The kernel writes to the isa-debug-exit port when the tests finish;
# QEMU then exits with (value << 1) | 1, so 0x10 (pass) becomes 33.
test:
	@$(MAKE) --no-print-directory BUILD=build-test KERNEL_TESTS=1 iso disks
	@timeout 120 $(QEMU) $(QEMU_CPU) -m 2G -cdrom build-test/BashketKernel-dev.iso -boot d \
		-drive file=build-test/disk.img,if=virtio,format=raw,snapshot=on $(if $(ROOT_FLAGS),-drive file=build-test/root.img$(comma)if=virtio$(comma)format=raw$(comma)snapshot=on) $(SWAP_TEST_FLAGS) $(NET_FLAGS) \
		-display none -serial stdio -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 > build-test/test.log 2>&1; \
	status=$$?; \
	grep -E '^\s+\[FAIL\]|CPU EXCEPTION|KERNEL PANIC' build-test/test.log; \
	sed -n '/--- Test Summary ---/,/^----------/p' build-test/test.log; \
	if grep -aq '$(VIRTIO_UNWANTED)' build-test/test.log; then echo "TESTS FAILED: a device was driven through its $(VIRTIO_UNWANTED)"; exit 1; fi; \
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
		'echo gone > temp.txt' 'rm temp.txt' 'rm notes/empty.txt' 'echo replaced > readme.txt' \
		'echo long > "A name too long for eight and three.txt"' 'echo mixed > MixedCase.Txt' \
		'mkdir "Project Files"' 'mv log.txt "Project Files/Log of events.txt"' 'mv work "Project Files/Work"' \
		'echo doomed > "Remove me, please.txt"' 'rm "Remove me, please.txt"' 'mv MixedCase.Txt renamed.txt' \
		'cat pattern.bin > cut.bin' 'truncate -s 5000 cut.bin' 'truncate -s 9000 cut.bin' 'touch -d 1000000000 note.txt' 'exit' \
		> $(BUILD)/fs-boot1.log 2>&1 \
		|| { echo "filesystem: FAILED in the first boot (see $(BUILD)/fs-boot1.log)"; exit 1; }
	@if command -v fsck.fat >/dev/null; then fsck.fat -n $(BUILD)/fs-persist.img > $(BUILD)/fs-fsck.log 2>&1 \
		|| { echo "filesystem: FAILED, fsck.fat rejects the volume after writing (see $(BUILD)/fs-fsck.log)"; exit 1; }; fi
	@tools/qemu_shell.py --quiet --timeout 60 --disk $(BUILD)/fs-persist.img $(SHELL_CPU) \
		--expect 'saved in boot one\nfirst\nsecond\ninside\nreplaced\n' --expect '400 1201 102400' \
		--expect 'temp.txt: no such file' --forbid 'empty.txt' \
		--expect 'long\nmixed\n' --expect 'Log of events.txt' --expect 'Work/' \
		--forbid 'Remove me' --forbid 'MixedCase' --forbid '~1' \
		--expect 'cut.bin\n  Type: file  Size: 9000 ' --expect 'Modified: 2001-09-09 01:46:40' \
		'exec /bin/sh' 'cd /disk' 'cat note.txt "Project Files/Log of events.txt" "Project Files/Work/a.txt" readme.txt' \
		'wc copy.bin' 'ls notes' 'cat temp.txt' 'cat "A name too long for eight and three.txt" renamed.txt' \
		'ls' 'ls "Project Files"' 'stat cut.bin note.txt' 'exit' > $(BUILD)/fs-boot2.log 2>&1 \
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
	@# A file larger than the editor holds is loaded in part, and must not be
	@# saved (that would cut it short): the count of its bytes is unchanged
	@tools/qemu_shell.py --quiet --timeout 60 $(SHELL_CPU) \
		--expect '\n(\d+ \d+ \d+)\n[\s\S]*not saved[\s\S]*\n\1\n' \
		'exec /bin/sh -c "cat /bin/syscheck /bin/fuzz /bin/sh > /tmp/big"' 'exec /bin/sh -c "wc /tmp/big"' \
		'!exec /bin/edit /tmp/big' '~^S' '@wait:1' '~^Q' 'exec /bin/sh -c "wc /tmp/big"' > $(BUILD)/edit-big.log 2>&1 \
		&& echo "editor (a file too large to hold is not saved): ok" \
		|| { echo "editor (large file): FAILED (see $(BUILD)/edit-big.log)"; exit 1; }

# Real UDP datagrams between the host and the kernel, in both directions
# (see tools/test_net.py). Uses only the loopback interface of the host.
test-net: iso
	@tools/test_net.py $(SHELL_CPU) > $(BUILD)/net.log 2>&1 \
		&& tail -1 $(BUILD)/net.log || { echo "network: FAILED (see $(BUILD)/net.log)"; exit 1; }

# More than one CPU. First the unit tests on four: all must start and
# schedule, the kernel's shared structures must survive all four using them
# at once, and everything else must work exactly as with one. Then real
# programs on 2, 4 and 8 CPUs: the system call checks, floating point state
# across task switches (and now across CPUs), and a pipeline.
SMP_PROGRAM_CPUS ?= 2 4 8
test-smp:
	@$(MAKE) --no-print-directory BUILD=build-test KERNEL_TESTS=1 iso disks
	@timeout 180 $(QEMU) $(QEMU_CPU) -smp 4 -m 2G -cdrom build-test/BashketKernel-dev.iso -boot d \
		-drive file=build-test/disk.img,if=virtio,format=raw,snapshot=on $(if $(ROOT_FLAGS),-drive file=build-test/root.img$(comma)if=virtio$(comma)format=raw$(comma)snapshot=on) $(SWAP_TEST_FLAGS) $(NET_FLAGS) \
		-display none -serial stdio -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 > build-test/test-smp.log 2>&1; \
	status=$$?; \
	if [ $$status -eq 33 ] && grep -aq 'SMP: 4 CPUs scheduling' build-test/test-smp.log; then \
		echo "four CPUs: all scheduling, unit tests pass"; \
	else echo "four CPUs: FAILED (qemu exit $$status, see build-test/test-smp.log)"; \
		grep -aE '^\s+\[FAIL\]|CPU EXCEPTION|SMP:' build-test/test-smp.log | head -5; exit 1; fi
	@$(MAKE) --no-print-directory iso
	@for n in $(SMP_PROGRAM_CPUS); do \
		tools/qemu_shell.py --quiet --timeout 120 $(SHELL_CPU) --qemu-arg=-smp --qemu-arg=$$n \
			--expect "SMP: $$n CPUs scheduling" --expect 'syscheck: all checks passed' \
			--expect 'fptest: all workers computed the right values' --expect '\n$(words $(USER_PROGS)) [0-9]+ [0-9]+\n' \
			--forbid 'LOCK RULE' \
			'exec /bin/syscheck' 'exec /bin/fptest' 'exec /bin/sh -c "ls /bin | wc"' > $(BUILD)/smp-$$n.log 2>&1 \
			&& echo "$$n CPUs: programs run" \
			|| { echo "$$n CPUs: FAILED (see $(BUILD)/smp-$$n.log)"; exit 1; }; \
	done

# A scripted login: wrong password refused, then a shell as an ordinary user
# who can use the system but not change it, then back to root.
test-login: iso
	@tools/qemu_shell.py --quiet --timeout 90 $(SHELL_CPU) \
		--expect 'incorrect name or password' --expect 'uid 1000\ngid 100\n' \
		--expect 'passwd: permission denied' --expect 'hello: permission denied' \
		--expect 'motd: permission denied' --expect 'vda: permission denied' \
		--expect 'Hello from ring 3' --expect 'uid 0 \(root\)' --forbid 'alice:1000' \
		--expect 'su: incorrect name or password' --expect 'as-root-now\n\$$ id\nuid 0 \(root\)\ngid 0' \
		--expect 'date: cannot set the time: operation not permitted' --expect '2030-01-01 00:00:0[0-9] UTC' \
		--expect 'passwd: password changed for alice' --expect 'you may only change your own password' \
		--expect 'passwd: the two do not match' --expect 'passwd: incorrect password' \
		--expect 'old-password[\s\S]*login: incorrect name or password[\s\S]*new-password[\s\S]*uid 1000\ngid 100\n[\s\S]*done-as-alice' \
		--expect 'uid 1000\ngid 100\ngroups 50\n' --expect 'cat /srv/staff.txt\nnotes for the staff group' \
		--expect 'uid 1001\ngid 100\n\$$ cat /srv/staff.txt\ncat: /srv/staff.txt: permission denied' \
		--expect '/srv/staff-id\nuid 1001\ngid 100\neffective gid 50\n' \
		'!exec /bin/login alice' '~wrong\r' '@wait:1' '!exec /bin/login alice' '~alice\r' \
		'id' 'date -s "2030-01-01 00:00"' 'cat /etc/passwd' 'rm /bin/hello' 'echo x > /etc/motd' 'diskio size' \
		'echo mine > /tmp/mine.txt' 'cat /tmp/mine.txt' 'hello' \
		'!su' '~wrong\r' '@wait:1' '!su' '~root\r' 'echo as-root-now' 'id' 'date -s "2030-01-01 00:00"' 'exit' \
		'!passwd' '~wrong\r' '@wait:1' '!passwd' '~alice\r' '~one-thing\r' '~another\r' '@wait:1' \
		'!passwd bob' '@wait:1' '!passwd' '~alice\r' '~wonderland\r' '~wonderland\r' '@wait:1' 'exit' \
		'exec /bin/echo old-password' '!exec /bin/login alice' '~alice\r' '@wait:1' \
		'exec /bin/echo new-password' '!exec /bin/login alice' '~wonderland\r' 'id' 'echo done-as-alice' 'exit' \
		'exec /bin/id' \
		'!exec /bin/login alice' '~wonderland\r' 'cat /srv/staff.txt' 'exit' \
		'!exec /bin/login bob' '~bob\r' 'id' 'cat /srv/staff.txt' '/srv/staff-id' 'exit' \
		> $(BUILD)/login.log 2>&1 \
		&& echo "login, su, passwd and permissions: ok" || { echo "login: FAILED (see $(BUILD)/login.log)"; exit 1; }

# Everything, on both CPU models: what to run before calling a change done.
# Takes about three minutes.

# The ext2 volume at /home: a user's files, with their owner and mode, must
# survive a reboot, and e2fsck must find nothing wrong with what the kernel
# wrote. Then images damaged on purpose: the kernel must stay up.
test-home: iso
	@if [ -z "$(ROOT_FLAGS)" ]; then echo "home: skipped (no mke2fs, debugfs and e2fsck on this machine)"; exit 0; fi; \
	cp $(DISK) $(BUILD)/home-fat.img && cp $(ROOT_IMG) $(BUILD)/root-persist.img && \
	tools/qemu_shell.py --quiet --timeout 90 --disk $(BUILD)/home-fat.img --root-disk $(BUILD)/root-persist.img \
		--keep-disk-writes $(SHELL_CPU) \
		--expect 'intruder.txt: permission denied' --expect 'kept.txt: permission denied' --expect 'Bob was here' \
		--forbid 'no such|invalid|error' \
		'!exec /bin/login alice' '~alice\r' 'cd /home/alice' 'echo kept across the reboot > kept.txt' \
		'chmod 600 kept.txt' 'mkdir projects' 'mv notes.txt projects/notes.txt' 'cat /bin/sh > projects/shell.bin' \
		'echo gone > temp.txt' 'rm temp.txt' 'rm diary.txt' 'echo no > /home/bob/intruder.txt' 'cat /home/bob/hello.txt' \
		'ln -s projects/notes.txt short-link' 'ln -s /home/alice/projects/./././././././././././././././notes.txt long-link' \
		'ln projects/notes.txt notes-again' 'cat /bin/sh > cut.bin' 'truncate -s 400000 cut.bin' \
		'truncate -s 300000 cut.bin' 'truncate -s 100000 cut.bin' 'truncate -s 1000 cut.bin' 'touch -d 1000000000 kept.txt' \
		'exit' '!exec /bin/login bob' '~bob\r' 'cat /home/alice/kept.txt' \
		'echo from bob > /home/bob/second.txt' 'exit' > $(BUILD)/home-boot1.log 2>&1 \
		|| { echo "home: FAILED in the first boot (see $(BUILD)/home-boot1.log)"; exit 1; }; \
	e2fsck -fn $(BUILD)/root-persist.img > $(BUILD)/home-fsck.log 2>&1 \
		|| { echo "home: FAILED, e2fsck rejects the volume after the kernel wrote to it (see $(BUILD)/home-fsck.log)"; exit 1; }; \
	tools/qemu_shell.py --quiet --timeout 60 --disk $(BUILD)/home-fat.img --root-disk $(BUILD)/root-persist.img $(SHELL_CPU) \
		--expect '-rw------- +1 +1000 +100 +23 [0-9-]+ [0-9:]+ kept.txt' \
		--expect 'drwxr-xr-x +2 +1000 +100 +[0-9]+ [0-9-]+ [0-9:]+ projects' \
		--expect '-rw-r--r-- +1 +1001 +100 +9 [0-9-]+ [0-9:]+ second.txt' \
		--expect 'kept across the reboot\nAlice keeps her notes here' --expect '\n([0-9]+ [0-9]+ [0-9]+)\n\1\n' \
		--expect 'lrwxrwxrwx +1 +1000 +100 +18 [0-9-]+ [0-9:]+ short-link -> projects/notes.txt' \
		--expect 'long-link -> /home/alice/projects/\./\./' --expect '(Alice keeps her notes here\.\n){3}' \
		--forbid 'diary' --forbid 'temp.txt' --forbid 'intruder' \
		--expect 'cut.bin\n  Type: file  Size: 1000 ' --expect 'kept.txt\n.*\n.*\n  Modified: 2001-09-09 01:46:40' \
		'exec /bin/ls -l /home/alice' 'exec /bin/ls -l /home/bob' \
		'exec /bin/cat /home/alice/kept.txt /home/alice/projects/notes.txt' \
		'exec /bin/cat /home/alice/short-link /home/alice/long-link /home/alice/notes-again' \
		'exec /bin/sh -c "wc /home/alice/projects/shell.bin > /tmp/a"' 'exec /bin/sh -c "wc /bin/sh > /tmp/b"' \
		'exec /bin/sh /home/../etc/compare.sh' 'exec /bin/stat /home/alice/cut.bin /home/alice/kept.txt' > $(BUILD)/home-boot2.log 2>&1 \
		|| { echo "home: FAILED, the second boot does not see the files as they were left (see $(BUILD)/home-boot2.log)"; exit 1; }; \
	echo "home (ext2: owners and modes across a reboot, checked by e2fsck): ok"
	@if [ -n "$(ROOT_FLAGS)" ]; then for kind in dir-reclen dir-overrun block-outside indirect-loop huge-size inode-table bitmap-full root-not-dir; do \
		tools/corrupt_ext2.py $(ROOT_IMG) $$kind $(BUILD)/home-damaged.img && \
		tools/qemu_shell.py --quiet --timeout 90 --root-disk $(BUILD)/home-damaged.img $(SHELL_CPU) \
			--expect 'Hello from ring 3' \
			'exec /bin/ls -l /home' 'exec /bin/ls -l /home/alice' 'exec /bin/sh -c "cat /home/alice/notes.txt | wc"' \
			'exec /bin/sh -c "cat /home/readme.txt | wc"' 'exec /bin/sh -c "echo written > /home/alice/w.txt"' \
			'exec /bin/sh -c "cat /bin/sh > /home/big.bin"' 'exec /bin/mkdir /home/alice/d' \
			'exec /bin/rm /home/alice/notes.txt' 'exec /bin/mv /home/readme.txt /home/alice/r.txt' \
			'exec /bin/hello' > $(BUILD)/home-damaged-$$kind.log 2>&1 \
			&& echo "damaged ext2 ($$kind): survived" \
			|| { echo "damaged ext2 ($$kind): FAILED (see $(BUILD)/home-damaged-$$kind.log)"; exit 1; }; \
	done; fi

# Crash consistency: kill QEMU while a program is rewriting the ext2 volume,
# boot again (the kernel repairs it at mount) and have e2fsck judge the
# result, several times over on the same image.
CRASH_ROUNDS ?= 6
test-crash: iso
	@tools/crash_test.py --rounds $(CRASH_ROUNDS) $(SHELL_CPU) > $(BUILD)/crash.log 2>&1 \
		&& tail -1 $(BUILD)/crash.log || { echo "crash: FAILED (see $(BUILD)/crash.log)"; exit 1; }

# The mouse and the screen as devices: /bin/paint takes the screen, the
# mouse is moved and clicked through QEMU's monitor, and screenshots show
# what was drawn. Then the program ends (once by its own way out, once
# killed) and the text screen has to be back both times. The screen is
# cleared first: the checks look at pixels where the stroke was, which the
# boot messages would otherwise reach (how far depends on how many there are).
PAINT_SHOTS = $(BUILD)/paint
test-paint: iso
	@rm -rf $(PAINT_SHOTS) && mkdir -p $(PAINT_SHOTS)
	@tools/qemu_shell.py --quiet --timeout 60 $(SHELL_CPU) \
		--expect 'paint: 1280x800, pointer at 640,400' --expect 'paint: 2 strokes; pointer last at 124,320' \
		--expect 'Hello from ring 3' --forbid 'no mouse|no screen' \
		'exec /bin/clear' '!exec /bin/paint' \
		'@button:1' '@mouse:60,0' '@mouse:0,40' '@button:0' \
		'@screendump:$(PAINT_SHOTS)/1-stroke.ppm' \
		'@mouse:-100,-100' '@mouse:-100,-100' '@mouse:-100,-100' '@mouse:-100,-124' '@mouse:-100,0' '@mouse:-100,0' '@mouse:-24,0' \
		'@button:1' '@button:0' \
		'@mouse:0,100' '@mouse:0,100' '@mouse:0,104' '@button:1' '@mouse:48,0' '@button:0' \
		'@screendump:$(PAINT_SHOTS)/2-red.ppm' \
		'@button:2' '@wait:1' \
		'@screendump:$(PAINT_SHOTS)/3-text.ppm' \
		'exec /bin/hello' '!exec /bin/paint' '@button:1' '@mouse:30,30' '@button:0' \
		'@screendump:$(PAINT_SHOTS)/4-again.ppm' '^C' '@wait:1' \
		'@screendump:$(PAINT_SHOTS)/5-killed.ppm' 'exec /bin/hello' > $(BUILD)/paint.log 2>&1 \
		|| { echo "paint: FAILED (see $(BUILD)/paint.log)"; exit 1; }
	@tools/check_ppm.py $(PAINT_SHOTS)/1-stroke.ppm 100,300=202428 670,400=ffffff 700,420=ffffff 690,410=202428 \
			28,16=ffffff 8,4=ffffff 60,4=101010 1250,6=c02020 640,395=202428 \
		&& tools/check_ppm.py $(PAINT_SHOTS)/2-red.ppm 670,400=ffffff 100,320=e04040 80,16=e04040 60,4=e04040 \
			8,4=101010 100,300=202428 \
		&& tools/check_ppm.py $(PAINT_SHOTS)/3-text.ppm 670,400!=ffffff 100,320!=e04040 1250,6!=c02020 1000,700=000000 \
		&& tools/check_ppm.py $(PAINT_SHOTS)/4-again.ppm 655,415=ffffff 1250,6=c02020 100,320=202428 \
		&& tools/check_ppm.py $(PAINT_SHOTS)/5-killed.ppm 655,415!=ffffff 1250,6!=c02020 1000,700=000000 \
		&& echo "paint (mouse and screen as devices; the text screen returns after exit and after a kill): ok" \
		|| { echo "paint: FAILED, the screenshots in $(PAINT_SHOTS) are not as expected"; exit 1; }

# Swap: a program that needs 80 MiB on a machine with 64. Without a swap
# disk the kernel must refuse it the memory; with one, every page must come
# back as it was written, and nothing may be left on the swap disk after.
# Killed half way, it leaves no frame and no swap slot behind. (That this
# happens as it dies, not when it is reaped, is a_zombie_holds_no_memory's
# check: here both are reaped at once.) Ctrl-C kills its forked child as
# well, which the kernel shell does not wait for, so the check waits (ten
# seconds at most) until no swapper is left alive; without that it read
# the dying child's share as lost about 1 run in 10.
test-swap: iso
	@if [ -z "$(MKSWAP)" ]; then echo "swap: skipped (no mkswap on this machine)"; exit 0; fi; \
	tools/qemu_shell.py --quiet --mem 64M --timeout 120 $(SHELL_CPU) \
		--expect 'swapper: the kernel will not promise 80 MiB' --expect 'Hello from ring 3' --forbid 'swapper: FAILED|out of memory' \
		'exec /bin/swapper 80' 'exec /bin/hello' > $(BUILD)/swap-without.log 2>&1 \
		|| { echo "swap: FAILED without a swap disk (see $(BUILD)/swap-without.log)"; exit 1; }; \
	tools/qemu_shell.py --quiet --mem 64M --timeout 300 --swap-disk $(SWAP_IMG) $(SHELL_CPU) \
		--expect 'swap: disk [0-9], 16383 pages' \
		--expect 'swapper: 80 MiB written and read back; [0-9]{5,} pages went out to disk, [0-9]{5,} came back' \
		--expect 'syscheck: all checks passed' \
		--expect 'SwapFree:   65532 kB' --forbid 'swapper: FAILED|could not be read back|out of memory' \
		--expect '(swapper: 45 MiB written and read back; [0-9]+ pages went out[^\n]*\n[^\n]*){2}' \
		'exec /bin/swapper 80' 'exec /bin/syscheck' 'exec /bin/sh' 'swapper 45 &' 'swapper 45' '@wait:8' 'exit' \
		'cat /proc/meminfo' > $(BUILD)/swap-with.log 2>&1 \
		|| { echo "swap: FAILED with a swap disk (see $(BUILD)/swap-with.log)"; exit 1; }; \
	tools/qemu_shell.py --quiet --mem 64M --timeout 120 --swap-disk $(SWAP_IMG) $(SHELL_CPU) \
		--expect 'MemFree:    [45][0-9]{4} kB\nPromised:   0 kB\nSwapTotal:  65532 kB\nSwapFree:   65532 kB' \
		--expect 'Swapped:    [0-9]{4,} pages out' --forbid 'swapper: ' \
		'!exec /bin/swapper 80' '@wait:6' '^C' 'exec /bin/sh' \
		'for i in 1 2 3 4 5 6 7 8 9 10; do ps | grep -v zombie | grep swapper > /dev/null || break; sleep 1; done' \
		'exit' 'cat /proc/meminfo' 'exec /bin/ps' > $(BUILD)/swap-killed.log 2>&1 \
		|| { echo "swap: FAILED, a program killed while swapping did not give everything back at once (see $(BUILD)/swap-killed.log)"; exit 1; }; \
	echo "swap (80 MiB used on a 64 MiB machine, then two programs of 45; refused without a swap disk): ok"

# Shutting down: a change made in one boot (a file, and a password set with
# passwd) is there in the next; "shutdown" switches the machine off (QEMU
# must exit by itself) and leaves the volume marked clean, so the next boot
# skips the check; e2fsck agrees; "shutdown -r" restarts it.
SHUTDOWN_SHELL = $(BOOT_SHELL) --prompt 'new password: ' --prompt 'again: ' --keep-disk-writes \
	--disk $(BUILD)/shutdown-fat.img --root-disk $(BUILD)/shutdown-root.img --timeout 60 $(SHELL_CPU)
test-shutdown: iso
	@cp $(DISK) $(BUILD)/shutdown-fat.img && cp $(ROOT_IMG) $(BUILD)/shutdown-root.img
	@$(SHUTDOWN_SHELL) --expect 'ext2: volume marked clean' --expect 'QEMU exited by itself \(status 0\)' \
		--forbid 'shutdown: ' \
		'root' 'root' 'echo made in the first boot > /root/proof' 'passwd alice' 'changed!' 'changed!' \
		'!shutdown' '@wait:15' > $(BUILD)/shutdown-1.log 2>&1 \
		|| { echo "shutdown: FAILED in the first boot (see $(BUILD)/shutdown-1.log)"; exit 1; }
	@e2fsck -fn $(BUILD)/shutdown-root.img > $(BUILD)/shutdown-fsck.log 2>&1 && dumpe2fs -h $(BUILD)/shutdown-root.img 2>/dev/null | grep -q 'state: *clean$$' \
		|| { echo "shutdown: FAILED, the volume is not clean after a shutdown (see $(BUILD)/shutdown-fsck.log)"; exit 1; }
	@$(SHUTDOWN_SHELL) --expect 'ext2: the volume was shut down cleanly; no check needed' \
		--expect 'login: incorrect name or password' --expect 'uid 1000' --expect 'made in the first boot' \
		--expect 'The system is going down for a restart' --expect 'QEMU exited by itself \(status 0\)' \
		--forbid 'repaired' \
		'alice' 'alice' 'alice' 'changed!' 'id' 'exit' 'root' 'root' 'cat /root/proof' \
		'!shutdown -r' '@wait:15' > $(BUILD)/shutdown-2.log 2>&1 \
		|| { echo "shutdown: FAILED in the second boot (see $(BUILD)/shutdown-2.log)"; exit 1; }
	@echo "shutdown (a change survives a clean power-off; no check after it; restart): ok"

# The kernel shell's commands that write to the terminal directly (the unit
# tests see what goes through the kernel log, and not these). dmesg goes
# last and is not waited for: the log it prints contains earlier prompts.
test-kshell: iso
	@tools/qemu_shell.py --quiet --timeout 60 $(SHELL_CPU) \
		--expect 'Hello, BashketKernel' --expect '/dev/vdb / ext2' --expect 'memcpy' --expect 'The kernel shell.s commands' \
		--forbid 'command not found' \
		'help' 'mount' 'bench' '!dmesg' '@wait:3' > $(BUILD)/kshell.log 2>&1 \
		&& echo "kernel shell (dmesg, mount, bench and help): ok" \
		|| { echo "kernel shell: FAILED (see $(BUILD)/kshell.log)"; exit 1; }
	@tools/qemu_shell.py --quiet --timeout 60 $(SHELL_CPU) --qemu-arg=-machine --qemu-arg=pc,i8042=off \
		--expect 'no PS/2 controller answered' --expect 'Hello from ring 3' \
		'exec /bin/hello' > $(BUILD)/no-ps2.log 2>&1 \
		&& echo "no PS/2 controller (boots, input from the serial line): ok" \
		|| { echo "no PS/2 controller: FAILED (see $(BUILD)/no-ps2.log)"; exit 1; }

# NMIs: one, reported and survived; then thirty while /bin/syscheck makes
# its system calls, so that some land in the instructions of a system call's
# entry and exit where the CPU is in ring 0 with the user's GS base (the
# NMI stub must decide swapgs from the GS base itself, not the saved CS).
NMI_STORM = $(foreach n,1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30,'@monitor:nmi' '@wait:0.2')
test-nmi: iso
	@tools/qemu_shell.py --quiet --timeout 120 $(SHELL_CPU) \
		--expect 'NMI received' --expect 'nmis: +1\n' --expect 'syscheck: all checks passed' \
		'@monitor:nmi' '@wait:1' 'cat /proc/cpuinfo' '!exec /bin/syscheck' $(NMI_STORM) '@wait:30' \
		'exec /bin/hello' > $(BUILD)/nmi.log 2>&1 \
		&& echo "NMIs (reported; thirty during system calls, survived): ok" \
		|| { echo "NMIs: FAILED (see $(BUILD)/nmi.log)"; exit 1; }

# The rescue system: without a system disk the kernel runs from its boot
# module, in memory. Everything must still work; nothing outside /disk lasts.
test-rescue: iso
	@tools/qemu_shell.py --quiet --timeout 120 --no-root-disk $(SHELL_CPU) \
		--expect 'No system disk: running the rescue system' --expect 'syscheck: all checks passed' \
		--expect 'memory / memfs' --expect 'Hello from ring 3' --forbid 'ext2: mounted' \
		'exec /bin/syscheck' 'cat /proc/mounts' 'exec /bin/hello' > $(BUILD)/rescue.log 2>&1 \
		&& echo "rescue system (no system disk: root in memory from the boot module): ok" \
		|| { echo "rescue system: FAILED (see $(BUILD)/rescue.log)"; exit 1; }

# The system as it boots for a user: no kernel shell, but /bin/init, /etc/rc
# and a login prompt that comes back after every session. A wrong password,
# two users, and Ctrl-C, which must reach the programs on the terminal and
# not init.
BOOT_SHELL = tools/qemu_shell.py --quiet --iso $(ISO) --boot-prompt 'login: ' --prompt 'login: ' --prompt 'password: '
test-boot: iso
	@$(BOOT_SHELL) --timeout 60 $(SHELL_CPU) \
		--expect 'dhcp: address 10\.0\.2\.15' --expect 'Welcome to BashketOS' \
		--expect 'login: incorrect name or password' --expect 'uid 0 \(root\)' --expect 'uid 1000\ngid 100' \
		--expect '[0-9]+\t0\t0\t[a-z]+ *\tuser\t[^\n]*\tinit\n' --forbid 'Interactive Shell Started|BashketOS> ' \
		'alice' 'wrong' 'root' 'root' 'id' 'cat /proc/tasks' 'exit' \
		'alice' 'alice' 'id' '!spin' '^C' 'exit' 'bob' 'bob' 'exit' > $(BUILD)/boot.log 2>&1 \
		&& echo "boot (init, /etc/rc, login prompt after every session): ok" \
		|| { echo "boot: FAILED (see $(BUILD)/boot.log)"; exit 1; }

# Jobs in the user shell: a command in the background, another stopped with
# Ctrl-Z, continued, brought back and interrupted; and an orphaned process,
# which init must adopt and collect.
test-jobs: iso
	@tools/qemu_shell.py --quiet --timeout 60 $(SHELL_CPU) \
		--expect '\[1\] [0-9]+\n' --expect '\[2\] Stopped   spin' --expect '[0-9]+\t[0-9]+\t0\tstopped\tuser\t[0-9]\t[0-9]+\t[0-9]+\tspin' \
		--expect '\[2\] spin &' --expect '\[1\] Running   sleep 30\n\[2\] Running   spin' \
		--expect 'jobs-left:\n\$$ ' --expect 'orphan [0-9]+: parent was [0-9]+, is now 3\n' \
		--forbid 'zombie' --forbid '\tspin\n.*\n.*after-exit' \
		'exec /bin/sh' 'sleep 30 &' 'jobs' '!spin' '^Z' 'jobs' 'ps' 'bg 2' 'jobs' '!fg 2' '^C' '!fg' '^C' \
		'echo jobs-left:' 'jobs' 'orphan 200' 'sleep 1' 'ps' 'exit' 'echo after-exit' 'ps' > $(BUILD)/jobs.log 2>&1 \
		&& echo "jobs and orphans: ok" || { echo "jobs and orphans: FAILED (see $(BUILD)/jobs.log)"; exit 1; }
	@# Command lists: ';', '&&', '||', separators in quotes, $? between parts
	@tools/qemu_shell.py --quiet --timeout 60 $(SHELL_CPU) \
		--expect '\nand-ran\n' --expect '\nor-ran\n' --expect '\none\ntwo\n' --expect '\nfallback\n' \
		--expect '\na;b && c\n' --expect '\nstatus 1\n' --expect 'sh: syntax error' --expect 'exited with status 3' \
		--forbid '\nshould-not\n' \
		'exec /bin/sh' 'true && echo and-ran' 'false && echo should-not' 'false || echo or-ran' 'echo one ; echo two' \
		'false && echo should-not || echo fallback' 'echo "a;b && c"' 'false ; echo status $$?' '&& echo should-not' \
		'false || exit 3' > $(BUILD)/lists.log 2>&1 \
		&& echo "command lists (; && ||): ok" || { echo "command lists: FAILED (see $(BUILD)/lists.log)"; exit 1; }
	@# Line editing at the prompt: a command recalled with Up and changed; a
	@# shell reading a pipe edits nothing (no escape sequences in its output)
	@tools/qemu_shell.py --quiet --timeout 60 $(SHELL_CPU) \
		--expect '\nfirst\n' --expect '\nfirsx\n' --expect '\nfrom-a-pipe\n\$$ ' --forbid 'pipe.*\x1b\[K' \
		'exec /bin/sh' 'echo first' '~\e[A^Hx\r' '@wait:1' 'echo echo from-a-pipe | sh' 'exit' > $(BUILD)/editing.log 2>&1 \
		&& echo "line editing and history at the prompt: ok" || { echo "line editing: FAILED (see $(BUILD)/editing.log)"; exit 1; }
	@# Wildcards: sorted matches with words around them, two patterns on a
	@# line, no match kept as typed, quotes and hidden names respected
	@tools/qemu_shell.py --quiet --timeout 60 $(SHELL_CPU) \
		--expect '\n<a /tmp/w/b1 /tmp/w/b2 /tmp/w/c3 z>\n' --expect '\n< a3 b1 b2 /tmp/w/a3 >\n' \
		--expect '\n< none\* >\n' --expect '\n</tmp/w/\*>\n' --expect '\nremoved: 0\n' \
		'exec /bin/sh' 'mkdir /tmp/w' 'cd /tmp/w' 'echo > b2 ; echo > a3 ; echo > c3 ; echo > b1 ; echo > .hid' \
		'echo "<a" /tmp/w/[bc]* "z>"' 'echo "<" [ab]* /tmp/w/a? ">"' 'echo "<" none* ">"' 'echo "</tmp/w/*>"' \
		'rm * ; rm .hid ; cd / ; rm /tmp/w' 'echo removed: $$?' 'exit' > $(BUILD)/wildcards.log 2>&1 \
		&& echo "wildcards: ok" || { echo "wildcards: FAILED (see $(BUILD)/wildcards.log)"; exit 1; }
	@# Tab completion: a program name, a directory, a file in it; two Tabs
	@# list the names in /etc, sorted
	@tools/qemu_shell.py --quiet --timeout 60 $(SHELL_CPU) \
		--expect '\n/etc/motd\n' --expect '\ncompare.sh  demo.sh  group  motd  passwd  rc\n' \
		'exec /bin/sh' '~ech^I/e^Imo^I\r' '@wait:1' '~ls /etc/^I^I^U\r' '@wait:1' 'exit' > $(BUILD)/completion.log 2>&1 \
		&& echo "Tab completion: ok" || { echo "Tab completion: FAILED (see $(BUILD)/completion.log)"; exit 1; }
	@# Compound commands at the prompt: the rest of an if is asked for with
	@# "> " (lines sent without waiting for "$ "), and nothing runs before fi
	@tools/qemu_shell.py --quiet --timeout 60 $(SHELL_CPU) \
		--expect '\$$ if true; then\n> echo inner\n> fi\ninner\n\$$ ' --expect 'done\nn1\nn2\n\$$ ' \
		'exec /bin/sh' '!if true; then' '@wait:1' '!echo inner' '@wait:1' 'fi' \
		'for x in 1 2; do echo n$$x; done' 'exit' > $(BUILD)/control.log 2>&1 \
		&& echo "if and for at the prompt: ok" || { echo "if and for at the prompt: FAILED (see $(BUILD)/control.log)"; exit 1; }

# The text tools against files with known answers: head, tail, grep (with
# -v, -c, several files, the exit status), sort (-n, -r, stable), cp (into
# a directory, onto itself)
test-tools: iso
	@tools/qemu_shell.py --quiet --timeout 60 $(SHELL_CPU) \
		--expect 'head -n 2 a\none\ntwo\n\$$ ' --expect 'tail -n 2 a\nfour\nfive\n\$$ ' \
		--expect 'grep tw a\ntwo\n\$$ ' --expect 'grep -v -c three a\n4\n\$$ ' \
		--expect 'grep two a b\na:two\nb:two again\n\$$ ' --expect '\nnone: 1\n' --expect '\nmissing: 2\n' \
		--expect 'sort s\n100\n20\n3\nb-first\nb-second\n\$$ ' --expect 'sort -n s\nb-first\nb-second\n3\n20\n100\n\$$ ' \
		--expect 'sort -r -n s\n100\n20\n3\nb-first\nb-second\n\$$ ' --expect 'cat d/a\none\ntwo\nthree\nfour\nfive\n\$$ ' \
		--expect 'cp: a and /tmp/t/../t/a are the same file' --expect 'wc a\n5 5 24\n\$$ ' \
		--forbid 'not found|usage:' \
		'exec /bin/sh' 'mkdir /tmp/t ; cd /tmp/t' \
		'echo one > a ; echo two >> a ; echo three >> a ; echo four >> a ; echo five >> a ; echo two again > b' \
		'head -n 2 a' 'tail -n 2 a' 'grep tw a' 'grep -v -c three a' \
		'grep two a b' 'grep nothing a ; echo none: $$?' 'grep x /no/such ; echo missing: $$?' \
		'echo b-first > s ; echo 20 >> s ; echo 100 >> s ; echo b-second >> s ; echo 3 >> s' \
		'sort s' 'sort -n s' 'sort -r -n s' 'mkdir d ; cp a d' 'cat d/a' 'cp a /tmp/t/../t/a' 'wc a' \
		'rm d/a ; rm d ; rm a ; rm b ; rm s ; cd / ; rm /tmp/t' 'exit' > $(BUILD)/tools.log 2>&1 \
		&& echo "text tools (head, tail, grep, sort, cp): ok" || { echo "text tools: FAILED (see $(BUILD)/tools.log)"; exit 1; }
	@# uniq, cut and tr in pipelines, as they are used: counting words
	@# (sort | uniq -c), a column of /etc/passwd, case and deletion
	@tools/qemu_shell.py --quiet --timeout 60 $(SHELL_CPU) \
		--expect 'sort w \| uniq -c\n2 apple\n1 kiwi\n3 pear\n\$$ ' \
		--expect 'uniq -d w2\napple\npear\n\$$ ' \
		--expect 'cut -d : -f 1,2 /etc/passwd \| grep alice\nalice:1000\n\$$ ' \
		--expect 'cut -c 1-3 w2 \| uniq\napp\nkiw\npea\n\$$ ' \
		--expect 'echo Mixed Case \| tr a-z A-Z\nMIXED CASE\n\$$ ' \
		--expect 'cat w \| tr -d aeiou \| head -n 2\npr\nppl\n\$$ ' \
		--forbid 'not found|usage:' \
		'exec /bin/sh' 'mkdir /tmp/u ; cd /tmp/u' \
		'echo pear > w ; echo apple >> w ; echo pear >> w ; echo kiwi >> w ; echo apple >> w ; echo pear >> w' \
		'sort w | uniq -c' 'sort w > w2' 'uniq -d w2' 'cut -d : -f 1,2 /etc/passwd | grep alice' \
		'cut -c 1-3 w2 | uniq' 'echo Mixed Case | tr a-z A-Z' 'cat w | tr -d aeiou | head -n 2' \
		'rm w ; rm w2 ; cd / ; rm /tmp/u' 'exit' > $(BUILD)/line-tools.log 2>&1 \
		&& echo "line tools (uniq, cut, tr): ok" || { echo "line tools: FAILED (see $(BUILD)/line-tools.log)"; exit 1; }
	@# tee, xargs, seq, basename and dirname as they are used together:
	@# a copy kept on the way through a pipe, names fed to a command (wc
	@# prints no name: "4 4 8" is the file, its empty standard input "0 0 0")
	@tools/qemu_shell.py --quiet --timeout 60 $(SHELL_CPU) \
		--expect 'seq 4 \| tee s \| xargs -n 2 echo pair\npair 1 2\npair 3 4\n\$$ ' \
		--expect 'cat s \| xargs\n1 2 3 4\n\$$ ' \
		--expect 'basename /tmp/v/s.txt .txt ; dirname /tmp/v/s.txt\ns\n/tmp/v\n\$$ ' \
		--expect 'find /tmp/v -name s \| xargs wc\n4 4 8\n\$$ ' \
		--expect 'seq 10 -3 1 \| tee -a s > /dev/null ; wc s\n8 8 17\n\$$ ' \
		--forbid 'not found|usage:' \
		'exec /bin/sh' 'mkdir /tmp/v ; cd /tmp/v' \
		'seq 4 | tee s | xargs -n 2 echo pair' 'cat s | xargs' 'basename /tmp/v/s.txt .txt ; dirname /tmp/v/s.txt' \
		'find /tmp/v -name s | xargs wc' 'seq 10 -3 1 | tee -a s > /dev/null ; wc s' \
		'rm s ; cd / ; rm /tmp/v' 'exit' > $(BUILD)/more-tools.log 2>&1 \
		&& echo "more tools (tee, xargs, seq, basename, dirname): ok" || { echo "more tools: FAILED (see $(BUILD)/more-tools.log)"; exit 1; }
	@# find and du on a tree made for them: sizes in whole KiB, a link back up
	@# (listed and counted, not followed), filters, and a chain of
	@# directories deeper than they enter (34 levels; the limit is 32)
	@eval set -- $$(python3 -c 'print(" ".join(["\"mkdir d ; cd d\""] * 34))'); \
	tools/qemu_shell.py --quiet --timeout 60 $(SHELL_CPU) \
		--expect 'find /tmp/k \| sort\n/tmp/k\n/tmp/k/a\n/tmp/k/a/b\n/tmp/k/a/b/three.c\n/tmp/k/a/b/up\n/tmp/k/a/two.txt\n/tmp/k/one.txt\n\$$ ' \
		--expect "find /tmp/k -name '\*.txt' \| sort\n/tmp/k/a/two.txt\n/tmp/k/one.txt\n\\$$ " \
		--expect 'find /tmp/k -type d \| sort\n/tmp/k\n/tmp/k/a\n/tmp/k/a/b\n\$$ ' \
		--expect "find /tmp/k -type f -name 't\*' \| sort\n/tmp/k/a/b/three.c\n/tmp/k/a/two.txt\n\\$$ " \
		--expect 'du /tmp/k\n4\t/tmp/k/a/b\n5\t/tmp/k/a\n7\t/tmp/k\n\$$ ' --expect 'du -s /tmp/k\n7\t/tmp/k\n\$$ ' \
		--expect 'find: /nope: no such file or directory\nmissing: 1\n' \
		--expect 'find: /tmp/z(/d){32}: more than 32 levels down, not entered\n' --expect '\ndeep: 1\n' \
		--expect 'du: /tmp/z(/d){32}: more than 32 levels down, not entered\n' \
		--forbid 'usage:|not found|/tmp/z(/d){33}' \
		'exec /bin/sh' 'mkdir /tmp/k ; mkdir /tmp/k/a ; mkdir /tmp/k/a/b' \
		'truncate -s 2000 /tmp/k/one.txt ; truncate -s 100 /tmp/k/a/two.txt ; truncate -s 3000 /tmp/k/a/b/three.c' \
		'ln -s .. /tmp/k/a/b/up' 'find /tmp/k | sort' "find /tmp/k -name '*.txt' | sort" 'find /tmp/k -type d | sort' \
		"find /tmp/k -type f -name 't*' | sort" 'du /tmp/k' 'du -s /tmp/k' 'find /nope ; echo missing: $$?' \
		'mkdir /tmp/z ; cd /tmp/z' "$$@" 'cd /' 'find /tmp/z > /dev/null ; echo deep: $$?' 'du -s /tmp/z' \
		'rm /tmp/k/a/b/up ; rm /tmp/k/a/b/three.c ; rm /tmp/k/a/b ; rm /tmp/k/a/two.txt ; rm /tmp/k/a ; rm /tmp/k/one.txt ; rm /tmp/k' \
		'exit' > $(BUILD)/find-du.log 2>&1 \
		&& echo "find and du: ok" || { echo "find and du: FAILED (see $(BUILD)/find-du.log)"; exit 1; }

check:
	@python3 tools/undocumented.py --check > /dev/null \
		&& echo "every function has a comment: ok" \
		|| { python3 tools/undocumented.py; echo "documentation: FAILED (functions without a comment, above)"; exit 1; }
	@$(MAKE) --no-print-directory test
	@$(MAKE) --no-print-directory test CPU=max
	@$(MAKE) --no-print-directory test-smp
	@$(MAKE) --no-print-directory test CPU=$(NO_TSC_CPU)
	@$(MAKE) --no-print-directory test-smp CPU=$(NO_TSC_CPU) SMP_PROGRAM_CPUS=2
	@$(MAKE) --no-print-directory test-lowmem
	@$(MAKE) --no-print-directory test-lowmem SMP=4
	@$(MAKE) --no-print-directory test-lowmem CPU=max
	@$(MAKE) --no-print-directory test-disk
	@$(MAKE) --no-print-directory test VIRTIO=legacy
	@$(MAKE) --no-print-directory test VIRTIO=modern
	@$(MAKE) --no-print-directory test-disk VIRTIO=legacy
	@$(MAKE) --no-print-directory test-net VIRTIO=legacy
	@$(MAKE) --no-print-directory test-net VIRTIO=modern SMP=2
	@$(MAKE) --no-print-directory test-edit
	@$(MAKE) --no-print-directory test-home
	@$(MAKE) --no-print-directory test-rescue
	@$(MAKE) --no-print-directory test-kshell
	@$(MAKE) --no-print-directory test-nmi
	@$(MAKE) --no-print-directory test-boot
	@$(MAKE) --no-print-directory test-boot SMP=4
	@$(MAKE) --no-print-directory test-shutdown
	@$(MAKE) --no-print-directory test-shutdown SMP=2
	@$(MAKE) --no-print-directory test-crash
	@$(MAKE) --no-print-directory test-crash SMP=4 CRASH_ROUNDS=3
	@$(MAKE) --no-print-directory test-paint
	@$(MAKE) --no-print-directory test-paint SMP=2
	@$(MAKE) --no-print-directory test-swap
	@$(MAKE) --no-print-directory test-swap SMP=4
	@$(MAKE) --no-print-directory test-login
	@$(MAKE) --no-print-directory test-jobs
	@$(MAKE) --no-print-directory test-jobs SMP=4
	@$(MAKE) --no-print-directory test-tools
	@$(MAKE) --no-print-directory test-net
	@$(MAKE) --no-print-directory test-net CPU=max
	@$(MAKE) --no-print-directory fuzz FUZZ_SEEDS="1 2 3"
	@$(MAKE) --no-print-directory fuzz CPU=max FUZZ_SEEDS="4 5"
	@$(MAKE) --no-print-directory fuzz SMP=4 FUZZ_SEEDS="6 7"
	@$(MAKE) --no-print-directory fuzz SMP=2 CPU=max FUZZ_SEEDS="8"
	@echo "ALL CHECKS PASSED"

# Header dependencies generated by -MMD, so editing a header rebuilds its users
-include $(OBJFILES:.o=.d) $(wildcard $(BUILD)/user/*.d $(BUILD)/user/lib/*.d)
