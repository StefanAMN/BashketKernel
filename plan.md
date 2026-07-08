# Project Kernel — A Learning-First x86_64 OS

**Authors:** Cristi & Stefan
**Goal:** Build a small but real x86_64 kernel in C — boots, handles interrupts, manages memory, does preemptive multitasking, runs userspace programs off a filesystem, and drops you into a shell — **and actually understand every layer of it.**

> This is not a "ship a product" repo. It's a **lesson-oriented repo**. The kernel is the excuse; the understanding is the deliverable. AI agents will write most of the code. Our job is to make sure that by the end, we could have written it ourselves.

---

## 0. The Learning Contract (read this every time you're tempted to skip it)

We chose "AI writes most, we review + explain-back." That only produces learning if the **explain-back half is enforced like a law**, because kernel dev punishes shallow understanding harder than any other kind of programming: a bug doesn't throw a stack trace, it silently triple-faults and reboots QEMU with zero explanation. You cannot debug what you don't understand. So:

### The Three Gates (no kernel code merges without passing all three)

1. **The Explain-Back Gate.** Before code merges, the *reviewer* (the person who did **not** drive the AI for that piece) writes 3–8 sentences in the PR explaining what the non-obvious lines do and *why*. If the reviewer can't, the PR doesn't merge until they can. Reviewing here means understanding, not skimming for style.

2. **The Delete-and-Rederive Test** (core primitives only — see list below). For the foundational pieces, delete the AI's code and rewrite it from your own notes/lesson doc. If you can't reproduce it, you didn't learn it; go back to the source material. This is the single highest-signal test of real understanding.

3. **The Lesson Doc.** Every phase ends with a `docs/NN-topic.md` written **in your own words, by a human, not the AI.** What the component does, why it exists, the gotchas you hit, and one diagram. If you can't explain it on paper, you don't understand it yet.

**Primitives that MUST pass Delete-and-Rederive** (these are the "muscle" pieces — the conceptual heart of an OS):
- Setting up and loading the **IDT** + writing an interrupt handler end-to-end
- The **page-table walk / map function** (4-level paging)
- The **physical frame allocator**
- The **context switch** (the assembly that saves one task and restores another)
- The **syscall entry path** (user → kernel → back)

For everything else (GDT boilerplate, PIC remap constants, framebuffer glyph blitting, ELF header structs, tar parsing) — let the AI write it, review it, explain it, move on. Not everything needs to be rederived; these five do.

### The "It Boots ≠ It's Correct" rule
A green build and a booting QEMU are **not** the bar. Kernels routinely "work" while being subtly broken (a paging bug that only bites under memory pressure three phases later). The bar is: *we understand why it works, and we predicted it would.*

---

## 1. Working With AI Without Rotting Your Brain (kernel-specific)

AI coding agents are unusually dangerous in kernel dev for specific, learnable reasons. Knowing them turns the danger into a learning tool.

**Why AI is confidently wrong here more than elsewhere:**
- **32-bit vs 64-bit contamination.** Most OSDev tutorials online are 32-bit (i386). The agent will constantly hand you 32-bit conventions — wrong register widths, `int 0x80` instead of `syscall`, 2-level paging, wrong GDT layout — when you want x86_64. **Watch for this constantly.**
- **Buggy training data.** Famous tutorials (James Molloy's) have well-known bugs that propagated across thousands of blog posts the model trained on. The AI will confidently reproduce those bugs.
- **Hallucinated hardware details.** Page-table bit positions, I/O port numbers, MSR numbers, PIC command bytes — the AI guesses and sounds sure. These must be checked against a primary source.

**The rule:** *The AI is a fast junior who never says "I'm not sure." The OSDev Wiki and the Intel SDM are the senior engineers.* When they disagree, the manual wins.

**Turn this into a learning artifact:** keep `docs/ai-was-wrong.md`. Every time the agent gives you buggy or 32-bit or hallucinated code and a primary source corrects it, log it (the wrong code, the right code, the source). This file will teach you more about x86_64 than any tutorial, because each entry is a misconception you personally corrected.

### The "Review an AI kernel patch" checklist
Paste this into every PR template:
- [ ] Is this **64-bit**, not leftover 32-bit? (register widths, `syscall` not `int 0x80`, 4-level paging)
- [ ] Are page-table entry bits correct **vs the Intel SDM**? (present, RW, US, NX, huge…)
- [ ] Are I/O ports / MSR numbers verified against a source, not guessed?
- [ ] Does it wrongly assume identity mapping when we run **higher-half** (HHDM)?
- [ ] Is the **red zone disabled** where required (`-mno-red-zone`)? Are interrupts disabled where they must be?
- [ ] Off-by-one in table sizes? (IDT = 256 entries; GDT entry count)
- [ ] Does it match the **Limine protocol version and config format** we actually use?

---

## 2. Toolchain & Ground Rules (Phase 0)

**Target:** x86_64, booted by **Limine** (currently v12.x — config file is `limine.conf`; verify the current version when you start, the protocol evolves).

**Why Limine and not "write your own bootloader":** writing the real-mode → protected-mode → long-mode transition yourself is 2–3 weeks of pain that teaches you about *legacy boot*, not about *operating systems*. Limine hands your kernel a 64-bit long-mode environment, a memory map, a framebuffer, and a higher-half mapping. We **read about** the boot transition (so we understand what Limine did for us — write it up in `docs/00`), but we don't implement it. If we finish everything else and want the pain, we can revisit it as a stretch goal.

**Tools to install:**
- A cross/freestanding compiler. Two options: build the classic `x86_64-elf-gcc` cross-toolchain (the "proper" OSDev way, follow the wiki's GCC Cross-Compiler page), **or** use `clang` with `--target=x86_64-unknown-none` (no cross-toolchain build needed — simpler to start). Either is fine; document which you picked and why in `docs/00`.
- `qemu-system-x86_64` — our machine. We develop against QEMU, not real hardware.
- `xorriso` + Limine's tools — to build a bootable ISO.
- `make` (or CMake). Keep the build boring and understandable.
- `gdb` — QEMU exposes a gdb stub (`-s -S`). Learning to attach gdb to the kernel early is worth a full phase of debugging pain saved.

**Critical compiler/linker flags (and know why each exists — this is a mini-lesson):**
`-ffreestanding` (no hosted libc), `-fno-stack-protector`, `-fno-pie -fno-pic`, `-mno-red-zone` (the red zone corrupts on interrupt — classic kernel bug), `-mcmodel=kernel`, `-mno-mmx -mno-sse -mno-sse2` (no SIMD until we set up the FPU), `-nostdlib` at link, and a **custom linker script** placing the kernel higher-half at `0xffffffff80000000`.

**Deliverable for Phase 0:** repo builds an ISO, QEMU boots Limine, Limine loads our (empty) kernel and calls `_start` which just halts. `docs/00-toolchain.md` explains the whole boot chain from power-on to `_start`.

---

## 3. The Roadmap

Each phase lists: **Concepts** (what you must understand), **Build** (what the AI writes), **Explain-back targets** (what the reviewer must be able to teach), and **Break-it** (deliberately break it and predict the failure — the best kernel-learning technique there is).

### Phase 1 — Boot to Output & Debug Infrastructure
- **Concepts:** higher-half kernels, the Limine boot protocol (requests/responses), linear framebuffers vs old VGA text mode, why serial is the kernel dev's lifeline.
- **Build:** Limine framebuffer request → a text console that blits bitmap-font glyphs to pixels; a **serial (COM1, port 0x3F8) driver** for debug logging; a `kprintf`; a `panic()`.
- **Explain-back:** How does control get from Limine to our `_start`? Why do we get a pixel framebuffer instead of a text buffer? Why is serial output more reliable than the screen for debugging?
- **Break-it:** point the framebuffer writes one byte off per pixel — predict the visual glitch before running.

### Phase 2 — CPU Tables & Interrupts *(contains Delete-and-Rederive: IDT)*
- **Concepts:** why segmentation is *vestigial* in long mode but the **GDT** is still mandatory; the **TSS** and why it exists (kernel stack on privilege change, `RSP0`, IST); the **IDT**, exceptions (0–31) vs hardware IRQs; the legacy **8259 PIC** and why we remap it off 0x20–0x2F; the **PIT** timer tick.
- **Build:** minimal GDT (null, kernel code/data, user code/data, TSS); IDT with 256 gates; assembly ISR stubs → common C handler that dumps the register frame; PIC remap; PIT tick; PS/2 keyboard IRQ (scancode → console).
- **Explain-back:** Trace exactly what the CPU pushes onto the stack on an interrupt, and what `iretq` restores. Why must the PIC be remapped?
- **Break-it:** trigger a divide-by-zero and confirm your handler catches exception 0. Then *forget* to remap the PIC and watch a timer IRQ get misinterpreted as an exception — explain why.

### Phase 3 — Memory Management *(contains Delete-and-Rederive: frame allocator + page map)*
This is the conceptual heart of the OS. Slow down here.
- **Concepts:** the Limine memory map; **physical vs virtual** memory; x86_64 **4-level paging** (PML4 → PDPT → PD → PT); every **page-table entry bit** (present, RW, US, PWT/PCD, accessed, dirty, huge, global, NX) *checked against the SDM*; the **HHDM** offset Limine gives us; the higher-half kernel mapping; TLB and when to flush it.
- **Build:** a **physical frame allocator** (bitmap or free-list, 4 KiB frames) from the memory map; a **paging module** with `map(virt, phys, flags)` / `unmap` / address-space creation; a **kernel heap** — start with a bump allocator, then a real `kmalloc` (free-list, later slab).
- **Explain-back:** Walk a virtual address through all four page tables by hand on paper. What exactly happens on a page fault, and how does the CPU tell you the faulting address (CR2)?
- **Break-it:** map a page **without** the present bit and touch it → predict the page fault. Unmap a page you're using → predict the crash. Forget to flush the TLB after remapping → explain the stale-translation bug.

### Phase 4 — Multitasking *(contains Delete-and-Rederive: context switch)*
- **Concepts:** what a "task/thread" *is* (a saved register set + stack + address space); **context switching**; **cooperative vs preemptive** scheduling; the timer IRQ as the preemption driver; **synchronization** (spinlocks, why disabling interrupts matters even on one core).
- **Build:** a task control block; the **context-switch assembly** (save callee state, swap `RSP`, swap `CR3` for per-process address spaces); a **round-robin scheduler**; preemption via the PIT tick; kernel threads first (ring 0 — simpler before userspace).
- **Explain-back:** Which registers must the context switch save, and why can we skip the caller-saved ones? What exactly happens on the stack when a timer IRQ preempts a task and the scheduler picks another?
- **Break-it:** deliberately forget to save one register in the context switch — predict the corruption. Create a race between two threads on a shared counter without a lock — predict the lost update.

### Phase 5 — Userspace & System Calls *(contains Delete-and-Rederive: syscall entry)*
- **Concepts:** **privilege rings** (0 vs 3), CPL/DPL/RPL; how the CPU switches to the kernel stack on entry from ring 3 (`TSS.RSP0`); the `syscall`/`sysret` mechanism and its MSRs (`EFER.SCE`, `STAR`, `LSTAR`, `SFMASK`); user vs kernel page permissions (the US bit); the **ELF64** format.
- **Build:** TSS wired for `RSP0`; syscall entry/exit (MSR setup + assembly trampoline); a handful of syscalls (`write`, `exit`, `read`); user page mappings; an **ELF64 loader** (parse program headers, map segments); entering ring 3; a tiny user-side syscall wrapper "libc."
- **Explain-back:** Trace a `write()` syscall from the user program's instruction all the way to kernel code and back. How does the CPU know which kernel stack to switch to?
- **Break-it:** have a user program execute a privileged instruction (e.g. `cli`) → predict the `#GP` fault. Have it dereference a kernel address → predict the fault (and confirm US-bit protection works).

### Phase 6 — Filesystem
- **Concepts:** the **VFS** abstraction (a uniform `open/read/write/readdir/close` over different backends); on-disk vs in-memory filesystems; inodes/directories.
- **Build (staged, simplest first):**
  1. Load an **initrd** as a Limine module — a **USTAR (tar) archive**, read-only. Dead simple to parse, teaches path lookup and file reads without a disk driver. Get the shell reading real files from this first.
  2. *(Optional, if time)* A real filesystem: **FAT16/32** (extremely well documented) with an **ATA PIO** disk driver (ports `0x1F0…`) or `virtio-blk`. This adds real read/write and teaches block I/O.
- **Explain-back:** How does `open("/bin/ls")` resolve to actual bytes? What does the VFS layer buy us over calling the tar parser directly?
- **Break-it:** corrupt a tar header length field → predict how the parser fails, and add validation.

### Phase 7 — Shell & Userland (the payoff)
- **Concepts:** device input (PS/2 keyboard scancode set 1 → ASCII, line buffering); process spawning; tying every prior phase together.
- **Build:** a keyboard-driven input line; a **spawn** syscall (load ELF from the FS + schedule it); a **userspace shell** that reads a line, parses it, execs a program from the filesystem; a few coreutils-lite programs (`echo`, `ls`, `cat`, `help`).
- **Explain-back:** When you type `ls` and hit enter, narrate the complete journey: keyboard IRQ → buffer → shell reads via syscall → FS lookup → ELF load → new task → scheduler → its `write` syscalls → framebuffer. *If you can narrate this whole path, you understand your OS.* This narration is the capstone lesson doc.

### Stretch goals (only after the above feels solid)
SMP + APIC/IOAPIC (replace the PIC), more syscalls (`fork`/`exec` properly, `mmap`), signals, a writable real filesystem, a proper slab allocator, `virtio` drivers, and — the deep end — networking or a GUI. Also: **write your own bootloader** now that you understand what Limine did for you.

---

## 4. Two-Person Git Workflow

- **`main` is protected.** All work on feature branches → PRs. No direct pushes.
- **The reviewer is always the person who did NOT drive the AI** for that branch. The reviewer's job is the Explain-Back Gate (Section 0). Approval means "I understand this and could defend it," not "looks fine."
- **Rotate the driver every phase** so both of you get hands-on with every subsystem. Kernel knowledge doesn't transfer by osmosis; you have to have built each layer at least once.
- **Pair-program the three hardest pieces** (paging, context switch, syscall entry) — same screen, both brains. These are where "I reviewed it" isn't enough.
- **Track the roadmap as GitHub milestones/issues** (one milestone per phase). Each phase's lesson doc + passing gates closes the milestone.
- **Weekly 30-min sync:** what merged, what broke, what confused us, who drives next. Keep a short retro note.

**Repo layout:**
```
/plan.md                <- this file
/docs/                  <- human-written lesson docs (00…07) + ai-was-wrong.md
/src/                   <- kernel source
/user/                  <- userspace programs (later phases)
/toolchain/ or Makefile <- build
/limine.conf, iso/      <- boot setup
```

---

## 5. How We'll Know We're Actually Learning (assessment)

Not "does it boot." The real tests:
1. **Delete-and-Rederive** passes for all five core primitives (Section 0).
2. **Teach-each-other:** at the end of each phase, one of you whiteboards the subsystem to the other from memory. Gaps = go re-read.
3. **The `ai-was-wrong.md` log is non-empty and growing** — proof you're checking the AI against reality, not trusting it.
4. **The Phase-7 capstone narration** (`ls` from keystroke to pixels) is complete and correct, written by you.

If we land short of the full FS+shell but every phase we *did* finish passes these tests — **that's a success.** A deeply-understood memory manager and scheduler beats a shallow, half-broken shell.

---

## 6. Reality Check on Scope & Pace

FS + userspace shell in a semester, alongside coursework, for two first-years, is **ambitious**. That's fine — the roadmap is built so every phase is independently satisfying and a legitimate stopping point. Phases 1–4 alone (boot → interrupts → memory → multitasking) are a genuinely impressive, deeply educational project. Treat 5–7 as "we'd be thrilled" rather than "we failed if we don't." Pace for understanding, not for the finish line. The gates protect you from the trap of rushing forward on foundations you don't actually hold.

---

## 7. Resources (ground truth first)

**Primary sources (the AI must be checked against these):**
- **OSDev Wiki** — `wiki.osdev.org`. The community bible. Read its pages on: Bare Bones, GDT, IDT, Interrupts, Paging, Higher Half Kernel, Limine, PS/2 Keyboard, ATA PIO. Also the **"James Molloy's Known Bugs"** errata page.
- **Intel 64 and IA-32 Architectures SDM**, especially **Volume 3 (System Programming)** — the definitive word on paging bits, descriptors, MSRs, syscall. When AI and wiki disagree, this decides.
- **Limine** — official repo `PROTOCOL.md`, `CONFIG.md`, and the C example/template kernel. Match the version you install.

**Concept books (read alongside — these make the "why" click):**
- **OSTEP — Operating Systems: Three Easy Pieces** (free online). The best conceptual OS textbook. Read the relevant chapter *before* each phase. This is your "understand it, not just build it" companion.
- **xv6** (MIT) — a tiny, clean, readable Unix-like teaching OS *with an accompanying book*. Read it for design and structure. (Its current version is RISC-V; the concepts transfer directly, and the older x86 edition maps even more closely.)
- **"Writing an OS in Rust"** by Philipp Oppermann (`os.phil-opp.com`) — it's Rust, not C, but the *explanations* of x86_64 interrupts and paging are the clearest written anywhere. Read for concepts, translate to C.
- **The little book about OS development** (`littleosbook.github.io`) — good end-to-end walkthrough (32-bit, so adapt).

**Community:** `r/osdev`, the OSDev forums, and the Limine Matrix/Discord for when you're stuck on something version-specific.

---

*Remember the whole point: the AI is allowed to write the code. It is not allowed to do the understanding for us. Every gate in this document exists to keep that line bright.*
