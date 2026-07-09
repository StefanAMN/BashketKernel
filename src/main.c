#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"
#include "serial.h"
#include "console.h"
#include "kprintf.h"
#include "test.h"

// Set the base revision to 2 or 1. Let's use 2 as it's the latest in v8.x/v12.x
__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(2);

__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

// Halt and catch fire function.
static void hcf(void) {
    asm("cli");
    while (1) {
        asm("hlt");
    }
}

// The following will be our kernel's entry point.
// If renaming _start() to something else, make sure to change the
// linker script accordingly.
void _start(void) {
    // Ensure the bootloader actually understands our base revision (see spec).
    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        hcf();
    }

    serial_init();
    
    if (framebuffer_request.response == NULL
     || framebuffer_request.response->framebuffer_count < 1) {
        hcf();
    }
    
    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    console_init(fb);
    
    kprintf("Hello, BashketKernel!\n");
    kprintf("We are in 64-bit long mode.\n");
    kprintf("Framebuffer address: %p\n", fb->address);
    kprintf("Resolution: %dx%d\n", fb->width, fb->height);
    kprintf("Phase 1 initialization complete.\n");

    // Run Kernel Unit Tests
    run_all_tests();

    // Phase 1: Halt
    while (1) {
        asm("hlt");
    }
}
