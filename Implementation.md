# BashketKernel Development Roadmap (Phases 15–16 & Phase 6/7 Enhancements)

This document outlines the completed roadmap for making **BashketKernel** fully interactive, functional, and usable as a standalone operating system.

---

## Roadmap Overview (STATUS: COMPLETED)

### 1. Phase 6 & 7 Enhancements: Interactive Shell, Coreutils & VFS File Operations [COMPLETED]
- **VFS File Operations**: Added `vfs_unlink` (file deletion), `vfs_mkdir`, and `vfs_stat`.
- **Shell Coreutils Expansion**:
  - `ps`: Display active processes/tasks with PID, state, stack usage, and execution status.
  - `kill <pid>`: Terminate process by PID using `process_kill`.
  - `uptime`: Display kernel uptime in seconds and timer ticks.
  - `touch <filename>`: Create empty file in current VFS directory.
  - `mkdir <dirname>`: Create new directory in VFS.
  - `rm <filename>`: Unlink/delete file from VFS.
  - `exec <path>`: Load and execute an ELF binary image via `process_execve`.

### 2. Phase 15: PS/2 Mouse Driver & Framebuffer Canvas (`src/phase15/`) [COMPLETED]
- **PS/2 Mouse Hardware Driver (`mouse.c`/`mouse.h`)**:
  - Enable auxiliary device on PS/2 controller (IRQ 12 / Vector 44).
  - Parse 3-byte mouse packets (X movement, Y movement, left/right/middle button states).
- **Framebuffer Graphical Cursor (`cursor.c`/`cursor.h`)**:
  - Draw and update mouse pointer sprite dynamically on linear framebuffer console.

### 3. Phase 16: Userland Environment & ELF Execution (`src/phase16/`) [COMPLETED]
- **Userland Binary Suite (`user_apps.c`/`user_apps.h`)**:
  - Embedded ELF binaries (`/bin/hello`, `/bin/calc`) mounted in VFS `/bin`.
  - Execution pipeline connecting Shell -> `process_execve` -> `elf_load` -> Ring 3 user mode.

---

## Verification Results
- All **283 kernel unit tests pass with 0 failures** in QEMU.
- Interactive shell coreutils fully operational.
