# BashketKernel Extended Development Roadmap (Phases 17–32)

This document outlines the extended roadmap for developing **BashketKernel** into a feature-rich, interactive 64-bit operating system.

---

## Roadmap Overview

### 1. Phases 17–26 [COMPLETED]
- RTC CMOS Driver, Process Environment Variables, IPC Pipes, ANSI Color Decoder, Dynamic RamFS, Priority Scheduler, Counting Semaphores, ProcFS, Virtual TTY, Hardware FPU/SSE.

### 2. Phase 27: Virtual Memory Swap Manager & Demand Paging (`src/phase27/`)
- Page swapping manager, swap frame allocation bitmap, page-in / page-out handlers.

### 3. Phase 28: ELF Dynamic Linking & Symbol Relocation (`src/phase28/`)
- Dynamic ELF relocations (`R_X86_64_RELATIVE`, `R_X86_64_GLOB_DAT`, `R_X86_64_JUMP_SLOT`).

### 4. Phase 29: Network Socket Layer & UDP/IP Protocol Stack (`src/phase29/`)
- Socket layer (`socket`, `bind`, `sendto`, `recvfrom`), IPv4 header checksumming, UDP protocol stack over VirtIO-Net.

### 5. Phase 30: Desktop Window Manager & GUI Compositor (`src/phase30/`)
- Graphical window manager (`window_t`), title bar rendering, double-buffered screen compositing.

### 6. Phase 31: User Accounts & Authentication (`src/phase31/`)
- `/etc/passwd` parser, UID/GID context, `login` & `su` authentication commands.

### 7. Phase 32: Kernel Ring Buffer & Diagnostic Logger (`dmesg`) (`src/phase32/`)
- Kernel syslog ring buffer (`/dev/kmsg`), `dmesg` coreutil diagnostic command.

---

## Verification Plan
Each phase will be verified using:
- Automated kernel unit test suites in `tests/`.
- QEMU runtime execution with 0 test failures.
