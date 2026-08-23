# Kernel Development Learning Guide

This document tracks the recommended order of topics and concepts to learn as features are implemented in the kernel repository. It serves as a self-study roadmap for understanding the inner workings of BashketKernel.

---

## Recommended Learning Path

### 1. Booting & Limine Protocol (Phase 1)
**Goal:** Understand how the kernel starts, boots into 64-bit Long Mode, and writes output to the screen.

- **Order of Learning:**
  1. **Limine Protocol & Boot Request Structure:**
     - Learn how requests (e.g. Framebuffer request) are passed between Limine and the kernel using static volatile structures in C.
     - Files to inspect: `src/main.c`, `limine.h`
  2. **VGA/Framebuffer Drawing:**
     - Understand pixel mapping, linear framebuffer addresses, pitch, and color formats.
     - Files to inspect: `src/phase1/console.h`, `src/phase1/console.c`
  3. **Serial Communications (COM1):**
     - Learn how port I/O (`outb`/`inb`) works in x86 architecture for line-by-line debugging via `0x3F8`.
     - Files to inspect: `src/phase1/serial.h`, `src/phase1/serial.c`, `src/phase1/io.h`
  4. **Formatted Kernel Output (`kprintf`):**
     - Learn how variadic functions (`stdarg.h`) process strings, numbers, hex formatting, and dispatch output to both serial and framebuffer.
     - Files to inspect: `src/phase1/kprintf.h`, `src/phase1/kprintf.c`

---

### 2. CPU Descriptors, Interrupts & Exceptions (Phase 2)
**Goal:** Understand CPU privilege levels, trap handling, state preservation, and hardware interrupt remapping.

- **Order of Learning:**
  1. **Global Descriptor Table (GDT) & Task State Segment (TSS):**
     - **Concepts:** Flat 64-bit segment model, Access Bytes, Segment Selectors (`0x08` Kernel Code, `0x10` Kernel Data), TSS role in long mode (storing `RSP0` for stack switching).
     - **Assembly reload:** Why `gdt_flush.S` uses `retfq` / far return to reload `CS`.
     - Files to inspect: `src/phase2/gdt.h`, `src/phase2/gdt.c`, `src/phase2/gdt_flush.S`
  2. **Interrupt Descriptor Table (IDT):**
     - **Concepts:** 16-byte gate descriptors in 64-bit mode, interrupt gates vs. trap gates, loading IDT via `lidt`.
     - Files to inspect: `src/phase2/idt.h`, `src/phase2/idt.c`
  3. **Assembly ISR Stubs & Stack Framing:**
     - **Concepts:** CPU automatic pushes (`SS`, `RSP`, `RFLAGS`, `CS`, `RIP`, + optional error code).
     - **State Preservation:** Pushing all 15 general-purpose registers (`RAX`..`R15`) to form a complete `struct registers` stack layout.
     - **Return:** `iretq` instruction restoring CPU execution state.
     - Files to inspect: `src/phase2/isr_stubs.S`, `src/phase2/interrupts.h`
  4. **Interrupt Dispatching & PIC Remapping:**
     - **Concepts:** Dispatching from low-level assembly to high-level C `isr_handler`.
     - **Hardware PIC:** Remapping the legacy 8259 dual PIC chips from default vectors (`0x00`-`0x0F`) to (`0x20`-`0x2F` / 32-47) to avoid collision with CPU exception vectors (`0`-`31`).
     - Files to inspect: `src/phase2/isr.c`, `src/phase2/pic.h`, `src/phase2/pic.c`

---

### 3. Memory Management (Phase 3)
**Goal:** Understand physical frame allocation, virtual page tables, and dynamic kernel heap allocation.

- **Order of Learning:**
  1. **Physical Memory Manager (PMM):**
     - **Concepts:** Parsing Limine memory map entries (`USABLE`, `RESERVED`, `KERNEL_AND_MODULES`).
     - **Frame Allocator:** 4KiB page frame bitmap allocation. Using HHDM (`phys + hhdm_offset`) to access physical RAM.
     - Files to inspect: `src/phase3/pmm.h`, `src/phase3/pmm.c`
  2. **Virtual Memory Manager (VMM & 4-Level Paging):**
     - **Concepts:** x86_64 4-level paging structure (PML4 -> PDPT -> PD -> PT).
     - **Page Flags:** `VMM_PRESENT`, `VMM_WRITABLE`, `VMM_USER`, `VMM_NO_EXECUTE`.
     - **Page Mapping & TLB:** Address translation, page table creation, invalidating TLB entries via `invlpg`.
     - Files to inspect: `src/phase3/vmm.h`, `src/phase3/vmm.c`
  3. **Page Fault Exception Diagnostics:**
     - **Concepts:** Vector 14 handling, reading fault address from `CR2`, decoding fault causes (present vs non-present, read vs write, user vs kernel).
     - Files to inspect: `src/phase2/isr.c`
  4. **Kernel Heap Allocator (`kmalloc` / `kfree`):**
     - **Concepts:** Dynamic memory management on virtual address ranges, block headers, first-fit searching, block splitting, and free block coalescing.
     - Files to inspect: `src/phase3/heap.h`, `src/phase3/heap.c`

---

### 4. Multitasking & Preemptive Scheduling (Phase 4)
**Goal:** Task Control Blocks (TCB), stack creation, assembly context switching, 8254 PIT timer driver, and preemptive round-robin scheduling.

- **Order of Learning:**
  1. **Task Control Block (TCB):**
     - **Concepts:** Task states (`READY`, `RUNNING`, `BLOCKED`, `ZOMBIE`), PID allocation, 16 KiB allocated kernel stacks, and per-process page tables (`PML4`).
     - Files to inspect: `src/phase4/task.h`
  2. **Assembly Context Switch (`ctx_switch.S`):**
     - **Concepts:** Saving callee-saved registers (`RBX`, `RBP`, `R12`-`R15`, `RFLAGS`), saving/loading `RSP`, switching `CR3` page table registers, popping saved state, and using `ret` to seamlessly jump into target task entry points.
     - Files to inspect: `src/phase4/ctx_switch.S`
  3. **8254 Programmable Interval Timer (PIT):**
     - **Concepts:** Configuring Channel 0 at 100 Hz (`0x36` command byte to port `0x43`), handling IRQ 0 (Vector 32), and invoking preemptive scheduler ticks.
     - Files to inspect: `src/phase4/pit.h`, `src/phase4/pit.c`
  4. **Round-Robin Scheduler:**
     - **Concepts:** Managing queue of tasks, `sched_yield()`, `sched_tick()`, `task_create()`, and cooperative/preemptive time slicing.
     - Files to inspect: `src/phase4/sched.h`, `src/phase4/sched.c`

---

### 5. Userspace & System Calls (Phase 5)
**Goal:** Fast system calls via `syscall`/`sysret` MSR configuration, assembly entry trampolines, and ELF64 binary executable loading.

- **Order of Learning:**
  1. **x86_64 Syscall MSR Configuration:**
     - **Concepts:** Enabling SCE bit in `IA32_EFER` MSR (`0xC0000080`), setting GDT CS/SS selectors in `IA32_STAR` MSR (`0xC0000081`), setting entry handler in `IA32_LSTAR` MSR (`0xC0000082`), and RFLAGS mask in `IA32_SFMASK` (`0xC0000084`).
     - Files to inspect: `src/phase5/syscall.h`, `src/phase5/syscall.c`
  2. **Syscall Assembly Trampoline (`syscall_entry.S`):**
     - **Concepts:** Preserving user state (`RIP` in `RCX`, `RFLAGS` in `R11`), pushing general-purpose registers to form stack frames, invoking C `syscall_handler`, and returning via `sysretq`.
     - Files to inspect: `src/phase5/syscall_entry.S`
  3. **ELF64 Executable Format Loader:**
     - **Concepts:** Validating ELF magic (`0x7F 'E' 'L' 'F'`), parsing `Elf64_Ehdr` and `Elf64_Phdr`, allocating virtual pages for `PT_LOAD` segments, setting page permissions, and returning binary entry point.
     - Files to inspect: `src/phase5/elf.h`, `src/phase5/elf.c`

---

### 6. Virtual Filesystem & Initrd (Phase 6)
**Goal:** Virtual Filesystem (VFS) tree abstractions, directory traversal, file read/write APIs, and USTAR (tar) ramdisk parsing.

- **Order of Learning:**
  1. **Virtual Filesystem (VFS) Abstraction:**
     - **Concepts:** `vnode_t` structures (`VFS_FILE` vs `VFS_DIRECTORY`), parent/child/sibling tree hierarchy, directory creation, path resolution (`vfs_find`), `vfs_unlink`, `vfs_stat`, `vfs_write`, and `vfs_read`.
     - Files to inspect: `src/phase6/vfs.h`, `src/phase6/vfs.c`
  2. **USTAR (Tar) Initrd Parser:**
     - **Concepts:** Reading 512-byte tar headers, octal size conversion, mounting files and directories into VFS tree on boot.
     - Files to inspect: `src/phase6/initrd.h`, `src/phase6/initrd.c`

---

### 7. Core Input & Interactive Kernel Shell (Phase 7)
**Goal:** PS/2 Keyboard hardware interrupts, scancode mapping, circular input buffers, and interactive command-line shell with coreutils.

- **Order of Learning:**
  1. **PS/2 Keyboard Controller Driver:**
     - **Concepts:** Reading scancodes from port `0x60` on IRQ 1 (Vector 33), Shift key state handling, converting scancodes to ASCII, and buffering keys in a thread-safe circular queue.
     - Files to inspect: `src/phase7/keyboard.h`, `src/phase7/keyboard.c`
  2. **Interactive Kernel Shell & Core Utilities:**
     - **Concepts:** Command loop (`shell_run_cmd`), built-in command handlers (`help`, `ls`, `cat`, `echo`, `mem`, `ps`, `uptime`, `date`, `env`, `export`, `unset`, `dmesg`, `mount`, `bench`, `touch`, `mkdir`, `rm`, `exec`, `kill`, `clear`), terminal prompt rendering, and backspace handling.
     - Files to inspect: `src/phase7/shell.h`, `src/phase7/shell.c`

---

### 8. Advanced Memory Management (Phase 8)
**Goal:** Slab Object Allocators, Physical Frame Reference Counting (COW), and Virtual Memory Mapping (`mmap`/`munmap`).

- **Order of Learning:**
  1. **Slab Allocator (`kmem_cache_t`):**
     - **Concepts:** Cache objects, slab page headers, free object freelists, fixed-size fast allocations.
     - Files to inspect: `src/phase8/slab.h`, `src/phase8/slab.c`
  2. **Copy-on-Write Frame Reference Counter:**
     - **Concepts:** Physical frame reference counting table, `cow_ref_inc`, `cow_ref_dec`, page duplicate on write fault.
     - Files to inspect: `src/phase8/cow.h`, `src/phase8/cow.c`
  3. **Virtual Memory Allocation (`mmap` / `munmap`):**
     - **Concepts:** Anonymous page allocations, page protection flags (`PROT_READ`, `PROT_WRITE`), unmapping pages from PML4.
     - Files to inspect: `src/phase8/mmap.h`, `src/phase8/mmap.c`

---

### 9. Modern Interrupt Architecture (Phase 9)
**Goal:** Local APIC (LAPIC) and I/O APIC (IOAPIC) replacing legacy 8259 PIC.

- **Order of Learning:**
  1. **Local APIC (LAPIC):**
     - **Concepts:** MMIO register mapping (`0xFEE00000`), Spurious Interrupt Vector (`0x0F0`), End-of-Interrupt (`EOI`), LAPIC Timer.
     - Files to inspect: `src/phase9/lapic.h`, `src/phase9/lapic.c`
  2. **I/O APIC (IOAPIC):**
     - **Concepts:** MMIO register mapping (`0xFEC00000`), Redirection Table entries, routing hardware IRQ lines to target LAPIC cores.
     - Files to inspect: `src/phase9/ioapic.h`, `src/phase9/ioapic.c`

---

### 10. Symmetric Multiprocessing & Atomic Spinlocks (Phase 10)
**Goal:** Multi-Core CPU Initialization, Atomic Spinlocks, and Per-CPU Contexts.

- **Order of Learning:**
  1. **Atomic Spinlocks:**
     - **Concepts:** `__atomic_test_and_set`, `__atomic_clear`, CPU `pause` instruction.
     - Files to inspect: `src/phase10/spinlock.h`, `src/phase10/spinlock.c`
  2. **Per-CPU Data & SMP Manager:**
     - **Concepts:** `per_cpu_t` structures, tracking CPU ID and LAPIC ID across multiple cores.
     - Files to inspect: `src/phase10/smp.h`, `src/phase10/smp.c`

---

### 11. Real Block Storage & Persistent FAT32 Filesystem (Phase 11)
**Goal:** VirtIO Block Device Driver and FAT32 File System.

- **Order of Learning:**
  1. **VirtIO-Block Driver:**
     - **Concepts:** 512-byte sector reads and writes, block device interface.
     - Files to inspect: `src/phase11/virtio_blk.h`, `src/phase11/virtio_blk.c`
  2. **FAT32 File System Driver:**
     - **Concepts:** File tables, cluster allocation, reading and writing files to persistent block storage.
     - Files to inspect: `src/phase11/fat32.h`, `src/phase11/fat32.c`

---

### 12. UNIX Process Lifecycle & Signals (Phase 12)
**Goal:** `fork()`, `execve()`, `waitpid()`, and UNIX Signals.

- **Order of Learning:**
  1. **Process Fork & Execve:**
     - **Concepts:** Process PCB duplication, child entry points, binary loading via `execve`.
     - Files to inspect: `src/phase12/process.h`, `src/phase12/process.c`
  2. **Waitpid & Signals:**
     - **Concepts:** Parent waiting for child exit status, sending signal numbers (`SIGINT`, `SIGKILL`).
     - Files to inspect: `src/phase12/process.h`, `src/phase12/process.c`

---

### 13. PCI Bus & VirtIO Network Stack (Phase 13)
**Goal:** PCI Configuration Space Scanner and VirtIO-Net Ethernet Driver.

- **Order of Learning:**
  1. **PCI Bus Scanner:**
     - **Concepts:** Reading PCI configuration registers via ports `0xCF8`/`0xCFC`, Vendor ID & Device ID matching.
     - Files to inspect: `src/phase13/pci.h`, `src/phase13/pci.c`
  2. **VirtIO Network Adapter:**
     - **Concepts:** MAC address configuration, Ethernet packet transmission.
     - Files to inspect: `src/phase13/virtio_net.h`, `src/phase13/virtio_net.c`

---

### 14. Custom Bare-Metal x86_64 Bootloader (Phase 14)
**Goal:** MBR Bootsector, GDT transition, 64-bit Long Mode CPU verification.

- **Order of Learning:**
  1. **CPUID 64-Bit Verification & Transition:**
     - **Concepts:** Checking CPUID bit 29 for Long Mode support, validating PML4 transition layout.
     - Files to inspect: `src/phase14/boot_loader.h`, `src/phase14/boot_loader.c`

---

### 15. PS/2 Mouse Driver & Graphical Cursor (Phase 15)
**Goal:** PS/2 Auxiliary Mouse Packet Parsing (IRQ 12) & Screen Cursor Rendering.

- **Order of Learning:**
  1. **PS/2 Auxiliary Mouse Driver:**
     - **Concepts:** Ports `0x60`/`0x64`, IRQ 12 (Vector 44), decoding 3-byte movement and button state packets.
     - Files to inspect: `src/phase15/mouse.h`, `src/phase15/mouse.c`
  2. **Framebuffer Cursor Renderer:**
     - **Concepts:** Mouse position tracking, cursor update routines.
     - Files to inspect: `src/phase15/cursor.h`, `src/phase15/cursor.c`

---

### 16. Userland Environment & ELF Executables (Phase 16)
**Goal:** Userland Program Binary Installation & Shell Command Execution (`exec`).

- **Order of Learning:**
  1. **Userland Binary Installation:**
     - **Concepts:** Registering built-in user binaries (`/bin/hello`, `/bin/calc`) into VFS `/bin`.
     - Files to inspect: `src/phase16/user_apps.h`, `src/phase16/user_apps.c`

---

### 17. Real-Time Clock & CMOS Driver (Phase 17)
**Goal:** CMOS Register Port Access (`0x70`/`0x71`) & Wall-Clock UTC Timestamp Reading.

- **Order of Learning:**
  1. **CMOS Hardware Driver:**
     - **Concepts:** CMOS register indices, BCD-to-Binary conversion, 12h/24h time decoding.
     - Files to inspect: `src/phase17/rtc.h`, `src/phase17/rtc.c`

---

### 18. Process Environment Variables (Phase 18)
**Goal:** Environment Variable Tables & Shell Variable Operations.

- **Order of Learning:**
  1. **Environment Table Subsystem:**
     - **Concepts:** Environment lookup (`env_get`), modification (`env_set`), unsetting (`env_unset`), `$VAR` expansion.
     - Files to inspect: `src/phase18/env.h`, `src/phase18/env.c`

---

### 19. Inter-Process Communication & Pipes (Phase 19)
**Goal:** 4 KiB Ring-Buffer Stream Pipes for Inter-Task Data Streaming.

- **Order of Learning:**
  1. **IPC Stream Pipe:**
     - **Concepts:** Thread-safe circular buffers, stream reads and writes.
     - Files to inspect: `src/phase19/pipe.h`, `src/phase19/pipe.c`

---

### 20. Terminal ANSI Color Decoder (Phase 20)
**Goal:** ANSI 8-Color & 16-Color Palette Mapping.

- **Order of Learning:**
  1. **ANSI Color Decoder:**
     - **Concepts:** Decoding ANSI escape sequence indices (30..37) into RGB 24-bit colors.
     - Files to inspect: `src/phase20/ansi.h`, `src/phase20/ansi.c`

---

### 21. Dynamic RamFS & VFS Write Operations (Phase 21)
**Goal:** Dynamic RAM Filesystem & Dynamic VFS File Writing.

- **Order of Learning:**
  1. **RamFS & VFS Write:**
     - **Concepts:** `vfs_write` memory allocation, dynamic file growth, `/tmp` dynamic scratch filesystem.
     - Files to inspect: `src/phase21/ramfs.h`, `src/phase21/ramfs.c`

---

### 22. Priority-Based Scheduler & Multi-Level Queues (Phase 22)
**Goal:** Task Execution Priority Levels (`PRIO_HIGH`, `PRIO_NORMAL`, `PRIO_LOW`).

- **Order of Learning:**
  1. **Task Priority Management:**
     - **Concepts:** Priority assignment, priority inheritance, scheduling queue selection.
     - Files to inspect: `src/phase22/priority.h`, `src/phase22/priority.c`

---

### 23. Counting Semaphores & Thread Synchronization (Phase 23)
**Goal:** Counting Semaphores (`sem_t`, `sem_wait`, `sem_post`).

- **Order of Learning:**
  1. **Semaphore Synchronization:**
     - **Concepts:** Atomic spinlock protection, wait queues, signal posting.
     - Files to inspect: `src/phase23/sem.h`, `src/phase23/sem.c`

---

### 24. Pseudo-Filesystem ProcFS (`/proc`) (Phase 24)
**Goal:** Virtual Filesystem Mounted at `/proc` with System Information Nodes.

- **Order of Learning:**
  1. **ProcFS Virtual Nodes:**
     - **Concepts:** `/proc/version`, `/proc/cpuinfo`, dynamic proc node reads.
     - Files to inspect: `src/phase24/procfs.h`, `src/phase24/procfs.c`

---

### 25. Virtual Terminal TTY Device (Phase 25)
**Goal:** Virtual TTY Streams & Screen Input Ring Buffer.

- **Order of Learning:**
  1. **TTY Streams:**
     - **Concepts:** TTY device ring buffers, non-blocking reads and writes.
     - Files to inspect: `src/phase25/tty.h`, `src/phase25/tty.c`

---

### 26. Hardware FPU & SSE State Management (Phase 26)
**Goal:** Enabling x87 FPU, MMX, SSE, and SSE2 Extensions.

- **Order of Learning:**
  1. **FPU / SSE Enablement:**
     - **Concepts:** Control registers `CR0` and `CR4`, `fninit` instruction, floating-point arithmetic support.
     - Files to inspect: `src/phase26/fpu.h`, `src/phase26/fpu.c`

---

### 27. Virtual Memory Swap Manager & Demand Paging (Phase 27)
**Goal:** Frame Eviction, Swap Allocation Bitmaps, and Page Restoration.

- **Order of Learning:**
  1. **Swap Management:**
     - **Concepts:** `swap_out_frame`, `swap_in_frame`, swap frame tracking bitmaps.
     - Files to inspect: `src/phase27/swap.h`, `src/phase27/swap.c`

---

### 28. ELF Dynamic Relocation Loader (Phase 28)
**Goal:** Parsing Relocation Tables and Symbol Resolution.

- **Order of Learning:**
  1. **ELF Dynamic Symbol Relocations:**
     - **Concepts:** `R_X86_64_RELATIVE`, `R_X86_64_GLOB_DAT`, `R_X86_64_JUMP_SLOT`.
     - Files to inspect: `src/phase28/elf_dyn.h`, `src/phase28/elf_dyn.c`

---

### 29. UDP/IP Network Socket Stack (Phase 29)
**Goal:** Socket Layer Abstraction & Internet Checksumming.

- **Order of Learning:**
  1. **Network Sockets:**
     - **Concepts:** `socket_create`, `socket_sendto`, `net_checksum`.
     - Files to inspect: `src/phase29/socket.h`, `src/phase29/socket.c`

---

### 30. Desktop Window Manager & GUI Compositor (Phase 30)
**Goal:** Graphical Desktop Windows & Canvas Buffer Allocations.

- **Order of Learning:**
  1. **Window Canvas Allocator:**
     - **Concepts:** Window structure creation (`window_create`), canvas double-buffering.
     - Files to inspect: `src/phase30/window.h`, `src/phase30/window.c`

---

### 31. User Account Authentication (Phase 31)
**Goal:** Account Authentication & UID/GID Contexts.

- **Order of Learning:**
  1. **Authentication Subsystem:**
     - **Concepts:** Account verification (`auth_login`), current profile context (`auth_get_current_user`).
     - Files to inspect: `src/phase31/auth.h`, `src/phase31/auth.c`

---

### 32. Kernel Log Ring Buffer & dmesg (Phase 32)
**Goal:** Kernel Diagnostic Ring Buffer & `dmesg` Coreutil.

- **Order of Learning:**
  1. **Kernel Ring Logger:**
     - **Concepts:** Ring buffer message writing (`klog_write`), output formatting (`dmesg`).
     - Files to inspect: `src/phase32/klog.h`, `src/phase32/klog.c`

---

### 33. High-Resolution Timers & Nanosleep (Phase 33)
**Goal:** Nanosecond Precision Sleep & High-Resolution Timer Queue.

- **Order of Learning:**
  1. **High-Resolution Timers:**
     - **Concepts:** Nanosecond timer queues, `nanosleep` delay loop.
     - Files to inspect: `src/phase33/hrtimer.h`, `src/phase33/hrtimer.c`

---

### 34. VFS Mount Manager (Phase 34)
**Goal:** Path Mount Points & Mount Table Management.

- **Order of Learning:**
  1. **VFS Mount Point Registry:**
     - **Concepts:** Mount table creation (`vfs_mount`), unmounting (`vfs_umount`).
     - Files to inspect: `src/phase34/mount.h`, `src/phase34/mount.c`

---

### 35. POSIX Shared Memory (SHM) (Phase 35)
**Goal:** Inter-Process Shared Physical Frame Regions.

- **Order of Learning:**
  1. **Shared Memory IPC:**
     - **Concepts:** Region creation (`shm_create`), region resolution (`shm_get`), region destruction (`shm_destroy`).
     - Files to inspect: `src/phase35/shm.h`, `src/phase35/shm.c`

---

### 36. Cryptographic Primitives (Phase 36)
**Goal:** Production-grade Kernel Cryptographic Suite (SHA-1, SHA-256, SHA-512, MD5, BLAKE2s, SipHash-2-4, AES-128/256, AES-XTS, AES-GCM, ChaCha20, Poly1305, HMAC, HKDF-SHA256, Ed25519, CRC32/CRC32C).

- **Order of Learning:**
  1. **Cryptographic Hashing & Checksums:**
     - **Concepts:** SHA-256 (`sha256_hash`), SHA-512 (`sha512_hash`), SHA-1 (`sha1_hash`), MD5 (`md5_hash`), BLAKE2s-256 (`blake2s_hash`), SipHash-2-4 (`siphash24`), CRC32/CRC32C (`crc32_calc`, `crc32c_calc`).
     - Files to inspect: `lib/crypto/sha256.c`, `lib/crypto/sha512.c`, `lib/crypto/sha1.c`, `lib/crypto/blake2s.c`, `lib/crypto/siphash.c`, `lib/crypto/crc32.c`
  2. **Symmetric Ciphers & Modes:**
     - **Concepts:** AES-128 & AES-256 (`aes128_encrypt_block`), IEEE 1619 AES-XTS (`aes128_xts_encrypt`), NIST SP 800-38D AES-GCM AEAD (`aes128_gcm_encrypt`), RFC 7539 ChaCha20 (`chacha20_encrypt`).
     - Files to inspect: `lib/crypto/aes.c`, `lib/crypto/aes_modes.c`, `lib/crypto/chacha20.c`
  3. **Key Derivation, MACs & Signatures:**
     - **Concepts:** RFC 7539 Poly1305 MAC (`poly1305_mac`), RFC 2104 Keyed-HMAC (`hmac_sha256`), RFC 5869 HKDF-SHA256 (`hkdf_sha256`), RFC 8032 Ed25519 Signatures (`ed25519_sign`, `ed25519_verify`).
     - Files to inspect: `lib/crypto/poly1305.c`, `lib/crypto/hmac.c`, `lib/crypto/hkdf.c`, `lib/crypto/ed25519.c`

---

### 37. Virtual Devices (/dev) (Phase 37)
**Goal:** Virtual Character Devices & RDTSC Pseudo-Random Number Generator.

- **Order of Learning:**
  1. **Virtual Device Tree:**
     - **Concepts:** Mount `/dev` nodes (`/dev/null`, `/dev/zero`, `/dev/random`), RDTSC PRNG (`get_random_u32`).
     - Files to inspect: `src/phase37/dev.h`, `src/phase37/dev.c`

---

### 38. Kernel Performance Benchmark Suite (Phase 38)
**Goal:** CPU MIPS Calculation & Memory Throughput Diagnostic Suite.

- **Order of Learning:**
  1. **Benchmark Suite:**
     - **Concepts:** Memory copy benchmark, CPU loop benchmark (`bench_run_all`).
     - Files to inspect: `src/phase38/bench.h`, `src/phase38/bench.c`
