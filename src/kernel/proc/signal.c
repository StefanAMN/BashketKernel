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
    uint64_t blocked;                       // The set of blocked signals to go back to
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

// Signals that can never be caught, ignored or blocked
#define UNBLOCKABLE ((1u << SIGKILL) | (1u << SIGSTOP))
#define STOP_SIGNALS ((1u << SIGSTOP) | (1u << SIGTSTP))

// What a signal does when the program has not said: nearly all end the
// process; a few are only of interest to a program that asks for them
// (SIGCONT has already had its effect by the time this is asked), and the
// stop signals suspend it
static bool default_is_ignore(int sig) {
    return sig == SIGCHLD || sig == SIGCONT;
}

static bool default_is_stop(int sig) {
    return sig == SIGSTOP || sig == SIGTSTP;
}

bool signal_deliverable(const task_t *task) {
    return (task->sig_pending & ~task->sig_blocked) != 0;
}

// Act on what is pending and not blocked: a signal with the default action
// ends the process (at its next safe point); one with a handler cuts short
// whatever sleep the process is in, so that the handler can run. Caller
// holds the scheduler lock.
static void act_on_pending(task_t *task) {
    uint32_t ready = task->sig_pending & ~task->sig_blocked;
    for (int sig = 1; sig < TASK_NSIG && ready; sig++) {
        if (!(ready & (1u << sig))) continue;
        uint64_t handler = task->sig_handlers[sig];
        if (handler == SIG_DFL) {
            task->sig_pending &= ~(1u << sig);
            if (default_is_stop(sig)) sched_request_stop(task, sig);
            else if (!default_is_ignore(sig)) sched_request_kill(task, sig);
        } else if (handler == SIG_IGN) {
            task->sig_pending &= ~(1u << sig);
        } else {
            sched_interrupt_sleep(task);
        }
    }
}

int signal_set_action(task_t *task, int sig, const struct signal_action *action, uint64_t restorer, struct signal_action *old) {
    if (!valid_signal(sig) || sig == SIGKILL || sig == SIGSTOP) return -EINVAL;

    if (action) {
        // A handler must be user code. (A bad address inside user space is the
        // program's own problem: it will fault when the handler is entered.)
        if (action->handler != SIG_DFL && action->handler != SIG_IGN) {
            if (action->handler >= USER_SPACE_END || restorer == 0 || restorer >= USER_SPACE_END) return -EINVAL;
        }
        if (action->flags & ~(uint32_t)SA_RESTART) return -EINVAL;
    }

    uint64_t flags = sched_lock_irqsave();
    if (old) {
        old->handler = task->sig_handlers[sig];
        old->mask = task->sig_masks[sig];
        old->flags = task->sig_flags[sig];
    }
    if (action) {
        task->sig_handlers[sig] = action->handler;
        task->sig_masks[sig] = action->mask & ~UNBLOCKABLE;
        task->sig_flags[sig] = (uint8_t)action->flags;
        if (action->handler != SIG_DFL && action->handler != SIG_IGN) task->sig_restorer = restorer;

        // One that is now ignored has nothing left to deliver
        bool ignored = action->handler == SIG_IGN || (action->handler == SIG_DFL && default_is_ignore(sig));
        if (ignored) task->sig_pending &= ~(1u << sig);
    }
    sched_unlock_irqrestore(flags);
    return 0;
}

int64_t signal_set_handler(task_t *task, int sig, uint64_t handler, uint64_t restorer) {
    struct signal_action action = { handler, 0, 0 }, old;
    int err = signal_set_action(task, sig, &action, restorer, &old);
    return err != 0 ? err : (int64_t)old.handler;
}

int signal_change_mask(task_t *task, int how, uint32_t set, uint32_t *old) {
    if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) return -EINVAL;

    uint64_t flags = sched_lock_irqsave();
    if (old) *old = task->sig_blocked;
    if (how == SIG_BLOCK) task->sig_blocked |= set;
    else if (how == SIG_UNBLOCK) task->sig_blocked &= ~set;
    else task->sig_blocked = set;
    task->sig_blocked &= ~(UNBLOCKABLE | 1u); // Bit 0 is no signal
    act_on_pending(task);                      // What was held back may now be due
    sched_unlock_irqrestore(flags);
    return 0;
}

void signal_send(task_t *task, int sig) {
    uint64_t flags = sched_lock_irqsave();
    signal_send_locked(task, sig);
    sched_unlock_irqrestore(flags);
}

void signal_send_locked(task_t *task, int sig) {
    if (!task || !task->is_user || !valid_signal(sig) || task->exiting || task->state == TASK_ZOMBIE) return;

    if (sig == SIGKILL) {
        sched_request_kill(task, sig);          // Ends it at its next safe point, whatever it has arranged
        return;
    }

    // Stopping and continuing cancel each other, whatever else the program
    // has arranged for these signals
    if (sig == SIGCONT) {
        task->sig_pending &= ~STOP_SIGNALS;
        sched_continue(task);
    } else if (default_is_stop(sig)) {
        task->sig_pending &= ~(1u << SIGCONT);
    }
    if (sig == SIGSTOP) {
        sched_request_stop(task, sig);          // Like SIGKILL: not the program's to refuse
        return;
    }

    // A signal that would be ignored is dropped at once, blocked or not
    uint64_t handler = task->sig_handlers[sig];
    if (handler == SIG_IGN || (handler == SIG_DFL && default_is_ignore(sig))) return;

    task->sig_pending |= (1u << sig);
    act_on_pending(task);                       // Does nothing while it is blocked
}

void signal_reset_on_exec(task_t *task) {
    for (int sig = 0; sig < TASK_NSIG; sig++) {
        if (task->sig_handlers[sig] != SIG_IGN) task->sig_handlers[sig] = SIG_DFL;
        task->sig_masks[sig] = 0;
        task->sig_flags[sig] = 0;
    }
    // (The set of blocked signals stays: a program is started with whatever
    // its parent arranged, and what is pending stays pending.)
    task->sig_restorer = 0;
    uint64_t flags = sched_lock_irqsave();
    act_on_pending(task);   // Signals whose handlers have just gone now have their default effect
    sched_unlock_irqrestore(flags);
}

// Make sure [addr, addr+len) of the current process is writable, growing the
// stack into it if it lies in the stack region.
static bool prepare_user_stack(uint64_t *space, uint64_t addr, size_t len) {
    if (!process_populate(addr, len)) return false;
    return vmm_user_range_ok(space, addr, len, true);
}

void signal_deliver(struct registers *regs) {
    task_t *task = sched_get_current_task();
    if (!task || !task->is_user) return;
    uint32_t restart = task->restart_syscall;
    task->restart_syscall = 0;
    if (!signal_deliverable(task)) return;
    if ((regs->cs & 3) != 3) return;        // Not a return to user mode

    // Take the lowest-numbered signal that is pending and not blocked
    uint64_t flags = sched_lock_irqsave();
    int sig = 0;
    uint32_t ready = task->sig_pending & ~task->sig_blocked;
    for (int s = 1; s < TASK_NSIG; s++) {
        if (ready & (1u << s)) { sig = s; break; }
    }
    if (sig) task->sig_pending &= ~(1u << sig);
    uint64_t handler = sig ? task->sig_handlers[sig] : SIG_IGN;
    uint32_t blocked_before = task->sig_blocked;
    bool restarts = sig && (task->sig_flags[sig] & SA_RESTART);
    // While the handler runs its own signal is held back (it would otherwise
    // interrupt itself), along with whatever the program asked for
    if (sig && handler != SIG_IGN && handler != SIG_DFL) {
        task->sig_blocked |= (task->sig_masks[sig] | (1u << sig)) & ~UNBLOCKABLE;
    }
    sched_unlock_irqrestore(flags);

    if (handler == SIG_IGN) return;         // Disposition changed since it was sent
    if (handler == SIG_DFL) {
        if (default_is_ignore(sig) || default_is_stop(sig)) return;
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
    frame.blocked = blocked_before;
    frame.regs = *regs;

    // The system call this signal cut short is to be made again once the
    // handler has returned: the saved state goes back to the `syscall`
    // instruction (two bytes long) with the call's number in RAX again. Its
    // arguments are still in their registers; the kernel does not change them.
    if (restart != 0 && restarts && frame.regs.rax == (uint64_t)-EINTR) {
        frame.regs.rax = restart - 1;
        frame.regs.rip -= 2;
    }
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

    // The handler is over: back to the set of blocked signals from before
    // it. (Whatever the frame says: a program can set any mask it likes
    // with sigprocmask as well, except for the signals nobody can block.)
    signal_change_mask(task, SIG_SETMASK, (uint32_t)frame.blocked, NULL);

    *regs = frame.regs;

    // Every register must come back exactly, including RCX and R11, which
    // the fast sysretq path cannot restore. A non-zero int_no tells the
    // system call exit code to use iretq instead.
    regs->int_no = SYSCALL_RETURN_FULL_FRAME;
}
