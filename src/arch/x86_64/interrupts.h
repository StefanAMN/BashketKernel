/**
 * @file interrupts.h
 * @brief CPU Register Frame Layout on Interrupt Entry
 * 
 * When an interrupt fires, the CPU automatically pushes:
 * SS, RSP, RFLAGS, CS, RIP (and error code for select exceptions).
 * The common assembly stub pushes RAX..R15 and the interrupt number.
 * This structure maps 1-to-1 with the layout of the CPU stack frame.
 */

#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Value of int_no in a system call frame that must be restored completely
// with iretq (see syscall_entry.S). A system call frame normally has 0 there.
#define SYSCALL_RETURN_FULL_FRAME 0x100

struct registers {
    // Pushed by common_isr_stub (Pushed last -> lowest memory address)
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

    // Interrupt Info
    uint64_t int_no;
    uint64_t err_code; // Error code (or dummy 0)

    // Pushed automatically by CPU (Pushed first -> highest memory address)
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

/**
 * @brief Have `handler` called for every interrupt on ISA IRQ line `irq`
 *        (0..15, delivered as vector 32 + irq), with the interrupted
 *        registers. Lines are shared by PCI devices, so up to four handlers
 *        can be registered per line; all are called each time, and each
 *        must ask its own device whether it raised the interrupt.
 *        Registering the same handler twice is a no-op; a fifth handler is
 *        refused with a message. Called at boot, before the line is routed.
 */
void register_irq_handler(uint8_t irq, void (*handler)(struct registers *regs));

// Vectors handed out to message-signalled interrupts (MSI-X): above the 16
// ISA lines (32..47) and below the inter-processor ones (0xF0, 0xF1) and
// the local APIC's spurious vector (0xFF). A vector's upper four bits are
// its priority class (Intel SDM Vol. 3A, 11.8.3); these all share class 3.
#define MSI_VECTOR_FIRST 0x30
#define MSI_VECTOR_COUNT 16

/**
 * @brief Take a free MSI vector and have `handler(arg)` called, with
 *        interrupts off, every time it arrives (on whichever CPU the device
 *        was told to send it to). Unlike a line, a vector belongs to one
 *        source: the handler need not ask the device whether it was the
 *        one. `name` is shown in /proc/interrupts. Called at boot.
 * @return The vector, or 0 if all MSI_VECTOR_COUNT are taken.
 */
uint8_t register_msi_handler(const char *name, void (*handler)(void *arg), void *arg);

/**
 * @brief The address and data words that make a device's memory write
 *        deliver `vector` to the local APIC with id `apic_id`: fixed
 *        delivery, edge triggered, physical destination (Intel SDM Vol. 3A,
 *        11.11.1 and 11.11.2).
 * @return false if `apic_id` does not fit the 8-bit destination field
 *         (larger ids need x2APIC or interrupt remapping, not done here).
 */
bool msi_message(uint32_t apic_id, uint8_t vector, uint64_t *address, uint32_t *data);

/**
 * @brief How many interrupts each line and each MSI vector has delivered,
 *        per CPU: the text of /proc/interrupts.
 */
size_t interrupts_describe(char *buf, size_t size);

#define MSI_ALL_CPUS 0xFFFFFFFFu

/**
 * @brief Interrupts that MSI vector `vector` has delivered on CPU number
 *        `cpu`, or on all of them together for MSI_ALL_CPUS (0 for a vector
 *        not handed out), for tests.
 */
uint64_t msi_vector_count(uint8_t vector, uint32_t cpu);

/**
 * @brief Interrupts that ISA line `irq` has delivered on all CPUs, for tests.
 */
uint64_t irq_line_count(uint8_t irq);

#endif // INTERRUPTS_H
