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

    // Run Kernel Unit Tests (Phases 1 - 7)
    run_all_tests();

    // Create interactive shell task
    task_create("shell", shell_task_entry);

    // Idle loop yielding to shell task
    while (1) {
        sched_yield();
        asm("hlt");
    }
}
