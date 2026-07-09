# BashketKernel

BashketKernel is a learning-first x86_64 higher-half kernel. It is built to strictly follow a phased roadmap where each layer of the operating system is built from scratch and thoroughly understood.

## Prerequisites

To compile and run BashketKernel, you need the following tools:
- A C compiler suite (e.g., `build-essential`, `base-devel`, or `Development Tools`)
- `xorriso` (for building the bootable ISO image)
- `qemu-system-x86` (for emulating the hardware)
- `gdb` (for debugging)

### Automated Setup

We provide a setup script that automatically detects your Linux distribution's package manager (`apt`, `pacman`, `dnf`, or `zypper`) and installs the required dependencies.

```bash
chmod +x setup.sh
./setup.sh
```



## Building and Running

The project uses `Makefile` to handle building the kernel, combining it with the Limine bootloader, and spinning it up in QEMU.

### 1. Build the ISO
To compile the kernel and package it into `BashketKernel.iso`:
```bash
make iso
```

### 2. Run in QEMU
To boot the generated ISO in QEMU:
```bash
make run
```

### 3. Debugging with GDB
To start QEMU in a suspended state waiting for a GDB connection on `localhost:1234`:
```bash
make run-gdb
```
*(In a separate terminal, run `gdb kernel.elf` and type `target remote :1234`)*
