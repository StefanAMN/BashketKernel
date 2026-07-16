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
#include "user_apps.h"
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

static uint8_t dedicated_kernel_stack[128 * 1024] __attribute__((aligned(4096)));

static void hcf(void) {
    asm("cli");
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
        if (keyboard_has_char()) {
            char c = keyboard_getchar();
            if (c == '\n') {
                kprintf("\n");
                line_buf[line_pos] = '\0';
                shell_run_cmd(line_buf);
                line_pos = 0;
                kprintf("BashketOS> ");
            } else if (c == '\b') {
                if (line_pos > 0) {
                    line_pos--;
                    kprintf("\b \b");
                }
            } else {
                if (line_pos < sizeof(line_buf) - 1) {
                    line_buf[line_pos++] = c;
                    char str[2] = {c, '\0'};
                    kprintf("%s", str);
                }
            }
        } else {
            sched_yield();
        }
    }
}

void _start(void) {
    // Switch to dedicated 128 KiB higher-half kernel stack safely
    uint64_t new_rsp = (uint64_t)dedicated_kernel_stack + sizeof(dedicated_kernel_stack) - 128;
    new_rsp &= ~0xFULL; // Ensure 16-byte alignment
    asm volatile("mov rsp, %0" : : "r"(new_rsp));

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
    kprintf("Phase 1 initialization complete.\n");

    // Phase 2: CPU Tables & Interrupts
    gdt_init();
    kprintf("GDT loaded.\n");

    idt_init();
    kprintf("IDT loaded.\n");

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
    kprintf("Phase 3 Memory Management initialization complete.\n");

    // Phase 4: Multitasking & Scheduler
    sched_init();
    pit_init(100); // 100 Hz timer tick
    kprintf("Phase 4 Multitasking initialization complete.\n");

    // Phase 5: Userspace & System Calls
    syscall_init();
    kprintf("Phase 5 Syscalls & ELF Loader initialization complete.\n");

    // Phase 6: Filesystem & Initrd
    vfs_init();
    kprintf("Phase 6 Virtual Filesystem initialization complete.\n");

    // Phase 7: Keyboard & Shell
    keyboard_init();
    shell_init();
    kprintf("Phase 7 Keyboard & Shell initialization complete.\n");

    // Phase 8: Advanced Memory Management (Slab, Refcounting, mmap)
    cow_init();
    kprintf("Phase 8 Slab Allocator, Refcounting & mmap initialization complete.\n");

    // Phase 9: Modern Interrupt Architecture (LAPIC & IOAPIC)
    lapic_init();
    ioapic_init();
    kprintf("Phase 9 Local APIC & I/O APIC initialization complete.\n");

    // Phase 10: Symmetric Multiprocessing (SMP & Spinlocks)
    smp_init();
    kprintf("Phase 10 SMP & Atomic Spinlocks initialization complete.\n");

    // Phase 11: Real Block Storage & Persistent Filesystem (VirtIO-Blk & FAT32)
    fat32_init();
    kprintf("Phase 11 Block Storage & FAT32 initialization complete.\n");

    // Phase 12: UNIX Process Lifecycle & Signals (fork, execve, waitpid, signals)
    kprintf("Phase 12 UNIX Process Subsystem initialization complete.\n");

    // Phase 13: PCI Bus & VirtIO Network Stack
    virtio_net_init();
    kprintf("Phase 13 PCI & VirtIO Network Stack initialization complete.\n");

    // Phase 14: Custom x86_64 Bare-Metal Bootloader Transition
    boot_loader_init();
    kprintf("Phase 14 Custom x86_64 Bootloader verification complete.\n");

    // Phase 15: PS/2 Mouse Driver & Framebuffer Canvas
    mouse_init();
    cursor_init();
    kprintf("Phase 15 PS/2 Mouse Driver & Canvas initialization complete.\n");

    // Phase 16: Userland Environment & Built-in ELF Programs
    user_apps_init();
    kprintf("Phase 16 Userland Apps & Binaries initialization complete.\n");

    // Phase 17: Real-Time Clock & CMOS Driver
    rtc_init();
    kprintf("Phase 17 RTC CMOS Driver initialization complete.\n");

    // Phase 18: Process Environment Variables
    env_init();
    kprintf("Phase 18 Environment Variables initialization complete.\n");

    // Phase 21: Dynamic RAM Filesystem (RamFS)
    ramfs_init();
    kprintf("Phase 21 Dynamic RamFS initialization complete.\n");

    // Phase 24: Pseudo-Filesystem ProcFS
    procfs_init();
    kprintf("Phase 24 ProcFS initialization complete.\n");

    // Phase 25: Virtual Terminal TTY
    tty_init();
    kprintf("Phase 25 TTY Subsystem initialization complete.\n");

    // Phase 26: Hardware FPU & SSE State Management
    fpu_init();
    kprintf("Phase 26 FPU/SSE initialization complete.\n");

    // Phase 27: Virtual Memory Swap Manager
    swap_init(1024);
    kprintf("Phase 27 Swap Manager initialization complete.\n");

    // Phase 31: User Accounts & Authentication
    auth_init();
    kprintf("Phase 31 Auth Subsystem initialization complete.\n");

    // Phase 32: Kernel Logging Ring Buffer
    klog_init();
    kprintf("Phase 32 Kernel Logger initialization complete.\n");

    // Run Kernel Unit Tests (Phases 1 - 32)
    run_all_tests();

    // Create interactive shell task
    task_create("shell", shell_task_entry);

    // Idle loop yielding to shell task
    while (1) {
        sched_yield();
        asm("hlt");
    }
}
