/**
 * @file signal.c
 * @brief Signals (see signal.h)
 */

#include "signal.h"
#include "sched.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"
#include "fpu.h"
#include "gdt.h"
#include "cpu.h"
#include "kstring.h"
#include "kerrno.h"
#include "spinlock.h"

// What the kernel writes on the user stack before entering a handler.
// `restorer` is at the lowest address, where a `call` would have put the
// return address, so the handler's `ret` goes there.
typedef struct {
    uint64_t restorer;
    uint64_t signo;
    struct registers regs;                  // The interrupted state
    uint8_t fpu[FPU_STATE_SIZE] __attribute__((aligned(16)));
} signal_frame_t;

// The System V ABI lets a function use the 128 bytes below RSP without
// moving RSP (the "red zone"). The interrupted code may be doing so.
#define RED_ZONE 128

// RFLAGS bits a program may set for itself: the arithmetic flags, the
// direction flag, and the trap flag. Everything else (interrupt enable, I/O
// privilege level, nested task, ...) is the kernel's.
#define RFLAGS_USER_MASK 0x0DD5ULL
#define RFLAGS_ALWAYS    0x0202ULL          // IF, and the bit that always reads 1

// Offset and writable bits of MXCSR inside an FXSAVE image. Loading reserved
// MXCSR bits with fxrstor raises #GP in ring 0, so they are cleared.
#define FXSAVE_MXCSR_OFFSET 24
#define MXCSR_VALID_BITS    0xFFBFu

static bool valid_signal(int sig) {
    return sig > 0 && sig < TASK_NSIG;
}

int64_t signal_set_handler(task_t *task, int sig, uint64_t handler, uint64_t restorer) {
    if (!valid_signal(sig) || sig == SIGKILL) return -EINVAL;

    // A handler must be user code. (A bad address inside user space is the
    // program's own problem: it will fault when the handler is entered.)
    if (handler != SIG_DFL && handler != SIG_IGN) {
        if (handler >= USER_SPACE_END || restorer == 0 || restorer >= USER_SPACE_END) return -EINVAL;
    }

    uint64_t flags = irq_save();
    uint64_t previous = task->sig_handlers[sig];
    task->sig_handlers[sig] = handler;
    if (handler != SIG_DFL && handler != SIG_IGN) task->sig_restorer = restorer;
    if (handler == SIG_IGN) task->sig_pending &= ~(1u << sig); // Nothing left to deliver
    irq_restore(flags);
    return (int64_t)previous;
}

void signal_send(task_t *task, int sig) {
    if (!task || !task->is_user || !valid_signal(sig)) return;

    uint64_t flags = irq_save();
    uint64_t handler = (sig == SIGKILL) ? SIG_DFL : task->sig_handlers[sig];

    if (handler == SIG_DFL) {
        sched_request_kill(task, sig);      // Ends it at its next safe point
    } else if (handler != SIG_IGN && task->state != TASK_ZOMBIE) {
        task->sig_pending |= (1u << sig);   // Delivered when it next returns to user mode
    }
    irq_restore(flags);
}

void signal_reset_on_exec(task_t *task) {
    for (int sig = 0; sig < TASK_NSIG; sig++) {
        if (task->sig_handlers[sig] != SIG_IGN) task->sig_handlers[sig] = SIG_DFL;
    }
    task->sig_pending = 0;
    task->sig_restorer = 0;
}

// Make sure [addr, addr+len) of the current process is writable, growing the
// stack into it if it lies in the stack region.
static bool prepare_user_stack(uint64_t *space, uint64_t addr, size_t len) {
    for (uint64_t page = addr & ~0xFFFULL; page < addr + len; page += PAGE_SIZE) {
        if (!(vmm_get_pte(space, page) & VMM_PRESENT)) process_grow_stack(page);
    }
    return vmm_user_range_ok(space, addr, len, true);
}

void signal_deliver(struct registers *regs) {
    task_t *task = sched_get_current_task();
    if (!task || !task->is_user || task->sig_pending == 0) return;
    if ((regs->cs & 3) != 3) return;        // Not a return to user mode

    // Take the lowest-numbered pending signal
    uint64_t flags = irq_save();
    int sig = 0;
    for (int s = 1; s < TASK_NSIG; s++) {
        if (task->sig_pending & (1u << s)) { sig = s; break; }
    }
    if (sig) task->sig_pending &= ~(1u << sig);
    uint64_t handler = sig ? task->sig_handlers[sig] : SIG_IGN;
    irq_restore(flags);

    if (handler == SIG_IGN) return;         // Disposition changed since it was sent
    if (handler == SIG_DFL) {
        task_exit_status(128 + sig);
    }

    // Where the frame goes: below the red zone of the interrupted code, and
    // placed so that RSP at handler entry looks as it does right after a
    // `call` (RSP + 8 is a multiple of 16).
    uint64_t frame_addr = ((regs->rsp - RED_ZONE - sizeof(signal_frame_t)) & ~0xFULL) - 8;
    uint64_t *space = vmm_space_from_phys((uint64_t)task->pml4_phys);

    if (frame_addr >= regs->rsp || !prepare_user_stack(space, frame_addr, sizeof(signal_frame_t))) {
        // No usable stack to run the handler on
        task_exit_status(128 + SIGSEGV);
    }

    // Build the frame in kernel memory, then copy it out in one piece
    signal_frame_t frame;
    frame.restorer = task->sig_restorer;
    frame.signo = (uint64_t)sig;
    frame.regs = *regs;
    fpu_save(frame.fpu);                    // The live FPU registers belong to the interrupted code

    user_access_begin();
    memcpy((void *)frame_addr, &frame, sizeof(frame));
    user_access_end();

    // Continue in the handler: handler(sig), returning to the restorer
    regs->rip = handler;
    regs->rdi = (uint64_t)sig;
    regs->rsp = frame_addr;
    regs->rflags &= ~(1ULL << 10);          // DF clear on function entry, as the ABI requires
    regs->rflags &= ~(1ULL << 8);           // Do not single-step into the handler
}

void signal_return(struct registers *regs) {
    task_t *task = sched_get_current_task();
    uint64_t *space = vmm_space_from_phys((uint64_t)task->pml4_phys);

    // The handler's `ret` popped the restorer address, so RSP now points at
    // the signo field. Everything read from here on is untrusted.
    uint64_t frame_addr = regs->rsp - sizeof(uint64_t);
    if (frame_addr > regs->rsp || !vmm_user_range_ok(space, frame_addr, sizeof(signal_frame_t), false)) {
        task_exit_status(128 + SIGSEGV);
    }

    signal_frame_t frame;
    user_access_begin();
    memcpy(&frame, (const void *)frame_addr, sizeof(frame));
    user_access_end();

    // Restore only what a user program is entitled to. Without these checks a
    // program could write a frame that "returns" with kernel segment
    // selectors or with interrupts disabled.
    if (frame.regs.rip >= USER_SPACE_END || frame.regs.rsp >= USER_SPACE_END) {
        task_exit_status(128 + SIGSEGV);    // iretq to such an address would fault in ring 0
    }
    frame.regs.cs = USER_CS;
    frame.regs.ss = USER_DS;
    frame.regs.rflags = (frame.regs.rflags & RFLAGS_USER_MASK) | RFLAGS_ALWAYS;

    uint32_t mxcsr;
    memcpy(&mxcsr, frame.fpu + FXSAVE_MXCSR_OFFSET, sizeof(mxcsr));
    mxcsr &= MXCSR_VALID_BITS;
    memcpy(frame.fpu + FXSAVE_MXCSR_OFFSET, &mxcsr, sizeof(mxcsr));
    fpu_restore(frame.fpu);

    *regs = frame.regs;

    // Every register must come back exactly, including RCX and R11, which
    // the fast sysretq path cannot restore. A non-zero int_no tells the
    // system call exit code to use iretq instead.
    regs->int_no = SYSCALL_RETURN_FULL_FRAME;
}
