#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"
#include "serial.h"
#include "console.h"
#include "kprintf.h"
#include "test.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "sched.h"
#include "pit.h"
#include "syscall.h"
#include "elf.h"
#include "vfs.h"
#include "initrd.h"
#include "keyboard.h"
#include "shell.h"
#include "slab.h"
#include "cow.h"
#include "mmap.h"
#include "lapic.h"
#include "ioapic.h"
#include "smp.h"
#include "virtio_blk.h"
#include "fat32.h"
#include "process.h"
#include "pci.h"
#include "virtio_net.h"
#include "boot_loader.h"
#include "mouse.h"
#include "cursor.h"
#include "rtc.h"
#include "env.h"
#include "pipe.h"
#include "ansi.h"
#include "ramfs.h"
#include "priority.h"
#include "sem.h"
#include "procfs.h"
#include "tty.h"
#include "fpu.h"
#include "swap.h"
#include "elf_dyn.h"
#include "socket.h"
#include "window.h"
#include "auth.h"
#include "klog.h"
#include "hrtimer.h"
#include "mount.h"
#include "shm.h"
#include "crypto.h"
#include "dev.h"
#include "bench.h"
#include "acpi.h"
#include "io.h"
#include "cpu.h"

__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(2);

__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0
};

// The initrd (a tar archive of user programs) is loaded as a boot module
__attribute__((used, section(".requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

// Ask Limine for a 128 KiB boot stack. kmain keeps running on it as PID 0.
__attribute__((used, section(".requests")))
static volatile struct limine_stack_size_request stack_size_request = {
    .id = LIMINE_STACK_SIZE_REQUEST,
    .revision = 0,
    .stack_size = 128 * 1024
};

static void hcf(void) {
    asm("cli");
    console_refresh();
    while (1) {
        asm("hlt");
    }
}

static void shell_task_entry(void) {
    kprintf("\n=== Interactive Shell Started ===\n");
    kprintf("Type 'help' for available commands.\n\n");

    char line_buf[128];
    size_t line_pos = 0;

    kprintf("BashketOS> ");

    while (1) {
        char c = keyboard_getchar(); // Sleeps until a key arrives

        if (c == '\n' || c == '\r') {
            kprintf("\n");
            line_buf[line_pos] = '\0';
            if (line_pos > 0) {
                shell_run_cmd(line_buf);
                line_pos = 0;
            }
            kprintf("BashketOS> ");
        } else if (c == '\b') {
            if (line_pos > 0) {
                line_pos--;
                kprintf("\b \b");
            }
        } else if (line_pos < sizeof(line_buf) - 1) {
            line_buf[line_pos++] = c;
            char str[2] = {c, '\0'};
            kprintf("%s", str);
        }
    }
}

void _start(void) {
    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        hcf();
    }

    serial_init();

    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        hcf();
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    console_init(fb);

    kprintf("Hello, BashketKernel!\n");
    kprintf("We are in 64-bit long mode.\n");
    kprintf("Framebuffer address: %p\n", fb->address);
    kprintf("Resolution: %dx%d\n", fb->width, fb->height);

    // Phase 2: CPU Tables & Interrupts
    gdt_init();
    kprintf("GDT loaded.\n");

    idt_init();
    kprintf("IDT loaded.\n");

    cpu_protection_init();

    pic_remap(32, 40);
    kprintf("PIC remapped.\n");

    asm volatile("sti");
    kprintf("Interrupts enabled (STI).\n");

    // Phase 3: Memory Management
    if (memmap_request.response == NULL || hhdm_request.response == NULL) {
        kprintf("FATAL: Limine memory map or HHDM response missing!\n");
        hcf();
    }

    pmm_init(memmap_request.response, hhdm_request.response->offset);
    kprintf("HHDM Offset: %p\n", (void *)pmm_get_hhdm_offset());
    vmm_init();
    kheap_init();
    console_enable_backbuffer();

    // Phase 4: Multitasking & Scheduler
    sched_init();
    pit_init(TIMER_HZ);

    // Phase 5: Userspace & System Calls
    syscall_init();

    // Phase 6: Filesystem & Initrd
    vfs_init();
    if (module_request.response && module_request.response->module_count > 0) {
        struct limine_file *initrd = module_request.response->modules[0];
        if (!initrd_mount(initrd->address, initrd->size)) {
            kprintf("WARNING: initrd is corrupt; continuing with what was loaded\n");
        }
    } else {
        kprintf("WARNING: no initrd module; /bin will be empty\n");
    }

    // Phase 7: Keyboard & Shell
    keyboard_init();
    shell_init();

    // Phase 8: Advanced Memory Management (Slab, Refcounting, mmap)
    cow_init();

    // Phase 9: Modern Interrupt Architecture (LAPIC & IOAPIC)
    // The PIT stays the only tick source. The LAPIC timer is left off until
    // it is calibrated and given its own vector.
    lapic_init();
    acpi_init(rsdp_request.response ? (void *)rsdp_request.response->address : NULL);
    ioapic_init();

    // Phase 10: Symmetric Multiprocessing (SMP & Spinlocks)
    smp_init();

    // Phase 11: Real Block Storage & Persistent Filesystem (VirtIO-Blk & FAT32)
    fat32_init();

    // Phase 13: PCI Bus & VirtIO Network Stack
    virtio_net_init();

    // Phase 14: Custom x86_64 Bare-Metal Bootloader Transition
    boot_loader_init();

    // Phase 15: PS/2 Mouse Driver & Framebuffer Canvas
    mouse_init();
    cursor_init();

    // Phase 17: Real-Time Clock & CMOS Driver
    rtc_init();

    // Phase 18: Process Environment Variables
    env_init();

    // Phase 21: Dynamic RAM Filesystem (RamFS)
    ramfs_init();

    // Phase 24: Pseudo-Filesystem ProcFS
    procfs_init();

    // Phase 25: Virtual Terminal TTY
    tty_init();

    // Phase 26: Hardware FPU & SSE State Management
    fpu_init();

    // Phase 27: Virtual Memory Swap Manager
    swap_init(1024);

    // Phase 31: User Accounts & Authentication
    auth_init();

    // Phase 32: Kernel Logging Ring Buffer
    klog_init();

    // Phase 33: High-Resolution Timers
    hrtimer_init();

    // Phase 37: Virtual Devices (/dev)
    dev_init();

#ifdef KERNEL_TESTS
    // Test build (`make test`): run the unit tests, report the verdict to
    // QEMU through the isa-debug-exit device, and stop.
    bool tests_ok = run_all_tests();
    outb(0xf4, tests_ok ? 0x10 : 0x11);
    kprintf("isa-debug-exit device not present; halting.\n");
    hcf();
#endif

    // Create interactive shell task
    task_create("shell", shell_task_entry);

    // Idle loop: free exited tasks, run whatever is runnable, and halt the
    // CPU until the next interrupt when nothing is.
    while (1) {
        sched_reap();
        sched_yield();
        asm("hlt");
    }
}
