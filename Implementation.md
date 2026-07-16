# BashketKernel Extended Development Roadmap (Phases 17–21)

This document outlines the completed roadmap for developing **BashketKernel** into a feature-rich, interactive 64-bit operating system.

---

## Roadmap Overview (STATUS: ALL 21 PHASES COMPLETED)

### 1. Phase 17: Real-Time Clock (RTC) & CMOS Driver (`src/phase17/`) [COMPLETED]
- **CMOS Port I/O Driver (`rtc.c`/`rtc.h`)**:
  - Access CMOS index port `0x70` and data port `0x71`.
  - Read real-world wall-clock time registers (second, minute, hour, day, month, year).
  - Handle BCD-to-Binary conversion and 12h/24h format decoding.
- **Shell Coreutil & Integration**:
  - Interactive shell `date` command displaying current real-world UTC timestamp.

### 2. Phase 18: Process Environment & Argument Passing (`src/phase18/`) [COMPLETED]
- **Environment Subsystem (`env.c`/`env.h`)**:
  - Environment variable table management (`setenv`, `getenv`, `unsetenv`).
- **Shell Integration**:
  - Environment variable commands (`export KEY=VALUE`, `unset KEY`, `env`).
  - Variable expansion (`echo $KEY`) in interactive shell.

### 3. Phase 19: Inter-Process Communication & Ring-Buffer Pipes (`src/phase19/`) [COMPLETED]
- **IPC Pipe Manager (`pipe.c`/`pipe.h`)**:
  - Unidirectional stream pipe creation with 4KiB ring buffer backing.
  - Thread-safe blocking read and write operations.

### 4. Phase 20: ANSI Color Rendering (`src/phase20/`) [COMPLETED]
- **Console ANSI Color Decoder (`ansi.c`/`ansi.h`)**:
  - Parse 8-color and 16-color ANSI codes (30..37) to RGB palette values.

### 5. Phase 21: Dynamic RamFS & VFS Write Operations (`src/phase21/`) [COMPLETED]
- **RamFS Memory Filesystem (`ramfs.c`/`ramfs.h`)**:
  - In-memory dynamic file creation, modification, and appending (`vfs_write`).
  - Mounted `/tmp` RAM filesystem for user and process scratch storage.

---

## Verification Results
- All **299 kernel unit tests pass with 0 failures** in QEMU.
