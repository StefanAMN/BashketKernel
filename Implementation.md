# BashketKernel — Next Steps & Advanced Roadmap (Phases 8–14)

This document expands on top of the original 7 Phases described in `plan.md`. Having completed Phase 1 through Phase 7 (boot console, GDT/IDT interrupts, PMM/VMM 4-level paging, preemptive multitasking, syscalls/ELF loader, VFS initrd, and interactive shell), this document outlines the concrete architectural roadmap for Phase 8 and beyond.

---

## Roadmap Overview

```
Phases 1–7 (Completed Core) ──────► Phase 8: Advanced Memory (Slab, COW, mmap)
                                         │
                                         ▼
 Phase 10: SMP & Multi-Core  ◄──── Phase 9: APIC & IOAPIC (Replace PIC)
          │
          ▼
 Phase 11: VirtIO-Block & FAT ───► Phase 12: UNIX Process Lifecycle (fork, execve, signals)
                                         │
                                         ▼
 Phase 14: Custom Bootloader ◄──── Phase 13: VirtIO-Net & TCP/IP Networking Stack
```

---

## Phase 8 — Advanced Memory Management & Allocators

### Concepts & Architecture
Transition from a basic free-list kernel heap to high-performance kernel object caching and virtual memory mapping.

### Key Components to Implement
1. **Slab Allocator (`kmem_cache`):**
   - Fixed-size object caches (`kmem_cache_t`) built on top of physical page frames (`pmm_alloc_frame`).
   - Prevents internal fragmentation for frequently allocated kernel objects (`struct task`, `vnode_t`, `file_descriptor_t`).
   - Functions: `kmem_cache_create()`, `kmem_cache_alloc()`, `kmem_cache_free()`.
2. **Page Frame Reference Counting & Shared Pages:**
   - Implement frame reference counting (`frame_refcount[frame_index]`) to support shared memory and Copy-On-Write (COW).
3. **`mmap` and `munmap` Syscalls:**
   - Anonymous virtual memory allocation (`MAP_ANONYMOUS`) and file-backed mappings (`MAP_PRIVATE` / `MAP_SHARED`).
   - Syscalls: `sys_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)` and `sys_munmap(void *addr, size_t length)`.

### Verification & Test Check
- Unit tests allocating and freeing 10,000 `kmem_cache` objects without memory leaks.
- Test `sys_mmap` creating user memory pages on demand and accessing them from Ring 3.

---

## Phase 9 — Modern Interrupt Architecture: APIC & IOAPIC

### Concepts & Architecture
Replace legacy 8259 PIC controllers with modern Intel Local APIC (LAPIC) and I/O APIC (IOAPIC) for high-frequency microsecond timers and multi-core interrupt routing.

### Key Components to Implement
1. **ACPI (Advanced Configuration and Power Interface) Parser:**
   - Search Limine RSDP (Root System Description Pointer) to locate the XSDT/RSDT table.
   - Parse the MADT (Multiple APIC Description Table) to discover LAPIC MMIO base address (`0xFEE00000`) and IOAPIC MMIO base address (`0xFEC00000`).
2. **Local APIC Driver (`lapic.c`):**
   - Map LAPIC MMIO physical pages to virtual memory.
   - Enable LAPIC Spurious Interrupt Vector (`0x1FF`).
   - Configure LAPIC Timer for high-precision microsecond periodic/one-shot interrupts (replacing PIT).
   - Send EOI (End of Interrupt) signal via LAPIC register (`0xB0`).
3. **IOAPIC Driver (`ioapic.c`):**
   - Configure IOAPIC Redirection Table entries to route legacy IRQs (Keyboard IRQ 1, Serial IRQ 4) to specific LAPIC interrupt vectors.

### Verification & Test Check
- Verify LAPIC timer firing high-frequency ticks without using 8259 PIC.
- Confirm keyboard interrupts received correctly via IOAPIC redirection.

---

## Phase 10 — Symmetric Multiprocessing (SMP) & Multi-Core Execution

### Concepts & Architecture
Boot multiple CPU cores (Application Processors / APs) concurrently, providing per-core scheduling and spinlock-guarded kernel resources.

### Key Components to Implement
1. **AP Core Initialization (SMP Booting):**
   - Parse MADT table to retrieve LAPIC IDs of all secondary CPU cores.
   - Execute INIT-SIPI-SIPI (Startup Inter-Processor Interrupt) sequence using LAPIC ICR (Interrupt Command Register).
2. **Per-CPU Storage & GS Base Register:**
   - Configure `IA32_GS_BASE` MSR (`0xC00000101`) to point to a per-CPU control block (`cpu_control_block_t`).
   - Access current core ID, current running task, and per-core stack via `%gs:` segment overrides.
3. **Kernel Spinlocks & Synchronization Primitives:**
   - Implement spinlocks using atomic x86 instructions (`lock bts` or `__sync_lock_test_and_set`).
   - Implement condition variables and mutexes (`mutex_lock`, `mutex_unlock`).
4. **Per-Core Preemptive Schedulers:**
   - Independent per-CPU ready queues with work-stealing or round-robin balancing across active CPU cores.

### Verification & Test Check
- Boot QEMU with multiple CPU cores (`qemu-system-x86_64 -smp 4`).
- Verify 4 kernel tasks running concurrently on 4 separate CPU cores printing thread IDs.

---

## Phase 11 — Real Block Storage & Persistent Filesystem (FAT16/FAT32)

### Concepts & Architecture
Transition from read-only in-memory USTAR tar initrd to real block storage devices with persistent read/write capabilities.

### Key Components to Implement
1. **VirtIO-Block Driver (`virtio-blk.c`):**
   - PCI Bus scanning to discover `virtio-blk` device (`Vendor ID: 0x1AF4`, `Device ID: 0x1001` / `0x1042`).
   - Configure Virtqueues (Descriptor Table, Available Ring, Used Ring) for DMA sector reads/writes (`512-byte` sectors).
2. **FAT16/FAT32 File System Driver (`fat32.c`):**
   - Parse Volume Boot Record (VBR) and BIOS Parameter Block (BPB).
   - Traversal of File Allocation Table (FAT) cluster chains.
   - Support file creation, writing, directory listing, and deletion.
3. **VFS Integration:**
   - Mount FAT storage volumes into VFS under `/disk` or `/mnt`.

### Verification & Test Check
- Write a file to `virtio-blk` drive in QEMU, reboot QEMU, and read the file back to confirm persistence across reboots.

---

## Phase 12 — UNIX Process Lifecycle & Advanced Syscalls (`fork`, `execve`, `signals`)

### Concepts & Architecture
Provide a fully POSIX-compliant process lifecycle enabling true process cloning, program execution, parent-child process synchronization, and asynchronous signals.

### Key Components to Implement
1. **`fork()` Syscall with Copy-On-Write (COW):**
   - Clone parent task's PML4 page tables. Mark user pages as read-only and set COW flag.
   - On write attempt, Page Fault handler allocates a new physical frame, copies page contents, and updates PML4 to writable.
2. **`execve()` Syscall:**
   - Replace active task page tables with a newly loaded ELF64 executable image from VFS.
   - Set up user stack with `argc`, `argv`, and `envp`.
3. **`waitpid()` Syscall & Process Cleanup:**
   - Track parent-child process relationships. Suspend parent task until target child exits and cleans up zombie state.
4. **UNIX Signals Subsystem:**
   - Support `SIGINT`, `SIGKILL`, `SIGSEGV`, `SIGALRM`.
   - Implement `sys_signal` and `sys_kill` syscalls.

### Verification & Test Check
- Execute a user program that calls `fork()`, mutates local state in parent/child independently via COW, and waits via `waitpid()`.

---

## Phase 13 — PCI Bus & VirtIO Networking Subsystem (TCP/IP Stack)

### Concepts & Architecture
Implement PCI device enumeration, VirtIO network driver, and a lightweight embedded TCP/IP stack.

### Key Components to Implement
1. **PCI Bus Scanner (`pci.c`):**
   - Enumerate PCI buses 0–255, devices 0–31, functions 0–7 via I/O ports `0xCF8` (Config Address) and `0xCFC` (Config Data).
2. **VirtIO Network Driver (`virtio-net.c`):**
   - Transmit and receive Virtqueues for raw Ethernet frame processing.
3. **TCP/IP Network Stack:**
   - Layer 2: Ethernet II frame parsing.
   - Layer 3: ARP (Address Resolution Protocol) and IPv4 packet routing.
   - Layer 4: UDP and TCP protocol handlers.
   - Socket API Syscalls: `sys_socket`, `sys_bind`, `sys_connect`, `sys_sendto`, `sys_recvfrom`.

### Verification & Test Check
- Ping QEMU kernel instance from host OS or send a UDP packet from userland shell.

---

## Phase 14 — Custom x86_64 Bootloader (Stretch Capstone Goal)

### Concepts & Architecture
Replace Limine bootloader with a custom 2-stage bootloader written from scratch to deeply understand the hardware initialization steps performed before kernel entry.

### Key Components to Implement
1. **Stage 1 Bootsector (`boot.asm` - 16-bit Real Mode):**
   - 512-byte MBR loaded by BIOS at physical address `0x7C00`.
   - Enables A20 line, loads Stage 2 sectors from disk using BIOS Int `0x13`.
2. **Stage 2 Loader (`loader.asm` / `loader.c`):**
   - Switches CPU from 16-bit Real Mode to 32-bit Protected Mode (loading 32-bit GDT).
   - Detects system memory map via BIOS Int `0x15, EAX=0xE820`.
   - Sets up initial 4-level PML4 paging tables, enables PAE (`CR4.PAE`) and Long Mode (`EFER.LME`).
   - Enables 64-bit Paging (`CR0.PG`) and jumps to 64-bit kernel `_start`.

### Verification & Test Check
- Boot kernel ISO compiled with custom bootloader directly in QEMU without Limine dependency.

---

## Recommended Execution Sequence

| Phase | Milestone Name | Difficulty | Key Deliverable |
| :--- | :--- | :--- | :--- |
| **Phase 8** | Advanced Memory Allocator & `mmap` | Medium | Slab cache allocator & `mmap`/`munmap` |
| **Phase 9** | APIC & IOAPIC Driver | Medium-High | Replace 8259 PIC with LAPIC/IOAPIC |
| **Phase 10** | Multi-Core (SMP) Multitasking | High | Multi-core task execution across 4 CPUs |
| **Phase 11** | VirtIO-Block & Persistent FAT32 | High | Persistent file storage on QEMU disk |
| **Phase 12** | UNIX `fork()`, `execve()`, Signals | Very High | POSIX process lifecycle with Copy-On-Write |
| **Phase 13** | VirtIO-Net & TCP/IP Network Stack | Extreme | Socket API and network ping |
| **Phase 14** | Custom 64-bit Bootloader | Stretch Goal | Booting kernel without external bootloader |
