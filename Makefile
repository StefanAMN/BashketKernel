CC = gcc
LD = ld

# Output directory. `make test` uses a separate one so that the test kernel
# and the normal kernel never share object files.
BUILD ?= build

INCLUDES = -Ilib -Isrc -Isrc/include -Isrc/init -Isrc/arch/x86_64 -Isrc/boot \
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
              -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
              -masm=intel -MMD -MP
USER_LDFLAGS = -nostdlib -static -T user/linker.ld -z max-page-size=0x1000

USER_LIB_OBJS = $(BUILD)/user/lib/crt0.o $(BUILD)/user/lib/ulib.o
USER_PROGS = $(patsubst user/%.c, $(BUILD)/initrd/bin/%, $(wildcard user/*.c))
USER_ROOTFS = $(shell find user/rootfs -type f)
INITRD = $(BUILD)/initrd.tar

$(BUILD)/user/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/%.o: user/%.S
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
QEMU_FLAGS = -m 2G -cdrom $(ISO) -boot d

.PHONY: all clean iso run run-gdb test

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

iso: $(ISO)

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

run: iso
	$(QEMU) $(QEMU_FLAGS) -serial mon:stdio

run-gdb: iso
	$(QEMU) $(QEMU_FLAGS) -s -S

# Build the test kernel, boot it headless, and turn its verdict into an exit
# status. The kernel writes to the isa-debug-exit port when the tests finish;
# QEMU then exits with (value << 1) | 1, so 0x10 (pass) becomes 33.
test:
	@$(MAKE) --no-print-directory BUILD=build-test KERNEL_TESTS=1 iso
	@timeout 120 $(QEMU) -m 2G -cdrom build-test/BashketKernel.iso -boot d \
		-display none -serial stdio -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 > build-test/test.log 2>&1; \
	status=$$?; \
	grep -E '^\s+\[FAIL\]|CPU EXCEPTION|KERNEL PANIC' build-test/test.log; \
	sed -n '/--- Test Summary ---/,/^----------/p' build-test/test.log; \
	if [ $$status -eq 33 ]; then echo "TESTS PASSED"; \
	elif [ $$status -eq 124 ]; then echo "TESTS TIMED OUT (see build-test/test.log)"; exit 1; \
	else echo "TESTS FAILED (qemu exit $$status, see build-test/test.log)"; exit 1; fi

# Header dependencies generated by -MMD, so editing a header rebuilds its users
-include $(OBJFILES:.o=.d) $(wildcard $(BUILD)/user/*.d $(BUILD)/user/lib/*.d)
