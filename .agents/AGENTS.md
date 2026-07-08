# Project Rules

This is a **learning-first** x86_64 kernel project (see `plan.md` for the
full roadmap). The kernel is the excuse; the understanding is the
deliverable. These rules apply to any agent working in this repo — read them
before writing code, not just before committing it.

## Project Context
- **Language:** C, plus small amounts of assembly (ISR stubs, context
  switch, syscall trampoline).
- **Architecture:** x86_64 only, long mode. No 32-bit code paths.
- **Bootloader:** **Limine** (verify current protocol version before
  scaffolding boot code — it evolves). Config file is `limine.conf`. Do not
  write a custom bootloader; that's an explicit stretch goal for later, not
  part of the core roadmap.
- **Build system:** Make (or CMake) — keep it boring and readable.
- **Toolchain:** either a proper `x86_64-elf-gcc` cross-compiler or
  `clang --target=x86_64-unknown-none`. Whichever is chosen, document it in
  `docs/00-toolchain.md` and don't silently switch later.
- **Critical build flags to preserve:** `-ffreestanding`,
  `-fno-stack-protector`, `-fno-pie -fno-pic`, `-mno-red-zone`,
  `-mcmodel=kernel`, `-mno-mmx -mno-sse -mno-sse2`, `-nostdlib`, and the
  custom higher-half linker script (`0xffffffff80000000`). Don't remove or
  "simplify" these without understanding why each exists — they each guard
  against a specific class of kernel bug.

## Working Incrementally
- **One phase at a time**, per the roadmap in `plan.md` (Phase 1: boot &
  console → Phase 2: GDT/IDT/interrupts → Phase 3: memory management →
  Phase 4: multitasking → Phase 5: userspace & syscalls → Phase 6:
  filesystem → Phase 7: shell). Do not preemptively build later-phase
  infrastructure, even if convenient — flag it and ask first.
- **Verify before moving on.** Every phase has a concrete "this works" check
  (QEMU output, register dump, a specific fault triggered on purpose). State
  what it is and confirm it passes before treating the phase as done.
- **"It boots" is not the bar.** A green build and a booting QEMU are not
  sufficient proof of correctness — kernels routinely run while being
  subtly broken. Don't declare a phase complete just because nothing
  crashed; confirm the behavior matches what was predicted.

## The Learning Gates (do not skip these — they are the actual point of the repo)
1. **Explain-Back Gate.** Every PR needs 3–8 sentences from the *human*
   reviewer (not the AI, not the person who drove the AI) explaining what
   the non-obvious code does and why. If the agent is asked to write this
   explanation itself, decline and explain that it must come from the human
   reviewer — an AI-written explanation defeats the purpose of the gate.
2. **Delete-and-Rederive primitives.** These five must be understood well
   enough for a human to rewrite from notes, not just reviewed:
   - IDT setup + an end-to-end interrupt handler
   - The page-table walk / map function (4-level paging)
   - The physical frame allocator
   - The context switch (save one task, restore another)
   - The syscall entry/exit path
   When working on any of these, favor over-explaining and exposing
   intermediate steps over handing over a finished, opaque block — the
   human needs to be able to reconstruct it later.
3. **Lesson docs.** Each phase ends in a human-written `docs/NN-topic.md`.
   The agent should not draft this file's content wholesale on the human's
   behalf; it can help by pointing out what topics/gotchas the phase
   surfaced, but the write-up itself is the human's job.
4. **Break-it exercises.** Each phase includes deliberately breaking
   something (unmapped page touch, missing PIC remap, corrupted tar header,
   etc.) and predicting the failure *before* running it. When implementing a
   phase, mention what the natural break-it exercise for that piece would
   be, even if the human doesn't ask.

## AI-Specific Review Checklist (apply this to your own output before handing it over)
- Is this genuinely **64-bit**, not leftover 32-bit convention? (register
  widths, `syscall`/`sysret` not `int 0x80`, 4-level paging, correct GDT
  layout)
- Are page-table entry bits (present, RW, US, NX, huge, etc.) stated
  correctly per the **Intel SDM**, not just "commonly seen online"?
- Are I/O ports / MSR numbers cited from a real source rather than
  recalled/guessed?
- Does the code wrongly assume identity mapping when the kernel runs
  **higher-half** (via the HHDM Limine provides)?
- Is the **red zone** disabled where required? Are interrupts disabled
  where they must be?
- Any off-by-one in table sizes (IDT = 256 entries, GDT entry count)?
- Does it match the **Limine protocol version and config format** actually
  in use in this repo (check `limine.conf` / vendored headers, don't assume
  a version from memory)?
- If well-known OSDev tutorials are a likely source of the pattern being
  used (e.g. James Molloy's tutorial), check it isn't reproducing one of
  its known bugs.

When any of the above is uncertain, say so explicitly and point to where it
should be verified (OSDev Wiki, Intel SDM, Limine's own `PROTOCOL.md`)
rather than presenting a guess with confidence. If corrected against a
primary source, note that it's the kind of entry that belongs in
`docs/ai-was-wrong.md` (a human writes the actual entry).

## Code Style
- **Assembly Syntax:** Use Intel syntax (not AT&T) for all inline assembly and standalone assembly files. Ensure `-masm=intel` is used in the build system.
- Favor clarity over cleverness — comment the *why*, not just the *what*,
  especially around magic numbers, bit layouts, and hardware/ABI quirks.
- Keep the repo layout intact:
  ```
  /plan.md
  /docs/            <- human-written lesson docs + ai-was-wrong.md
  /src/             <- kernel source
  /user/            <- userspace programs (later phases)
  /limine.conf, iso/
  ```

## Testing
- Test in **QEMU**, not real hardware. Always give the exact
  `qemu-system-x86_64` invocation used.
- Use GDB attached to QEMU (`-s -S`) for boot/crash debugging rather than
  guessing from symptoms.
- Prefer serial output for kernel debug logging once it exists (Phase 1);
  use the framebuffer console for user-facing output.

## Git Workflow & Commits
- `main` is protected — all work happens on feature branches via PRs, no
  direct pushes.
- **Feature Completion:** Always commit changes after finishing the
  implementation or update of a new feature.
- **Commit Management:** Avoid making too many small commits for minor
  changes. Use rebasing and merging (`git commit --amend`, interactive
  rebase, etc.) to bundle related small modifications together.
- **Ignore Files:** Add new entries to `.gitignore` if any files should not
  be tracked by Git.

## Documentation
- **README updates:** Update `README.md` whenever a new feature or change
  requires updated documentation or context.
- **Lesson docs and `ai-was-wrong.md`** are separate from the README — see
  the Learning Gates section above. These are human-authored records, not
  something the agent should generate on its own initiative.
