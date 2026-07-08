# BashketKernel

BashketKernel is a learning-first x86_64 higher-half kernel. It is built to strictly follow a phased roadmap where each layer of the operating system is built from scratch and thoroughly understood.

## Prerequisites

To compile and run BashketKernel, you need a C toolchain, `xorriso` (for
building the bootable ISO image), a QEMU x86 system emulator, and `gdb` (for
debugging). Exact package names by distribution:

| Distro | Packages |
|---|---|
| Ubuntu/Debian | `build-essential xorriso qemu-system-x86 gdb` |
| Fedora/RHEL | `gcc gcc-c++ make binutils xorriso qemu-system-x86 gdb` |
| Arch/Manjaro | `base-devel xorriso qemu-system-x86 gdb` |

On any other distribution, install the equivalent packages for those four
tools manually.

### Automated Setup

We have provided a setup script that detects your distribution (or lets you
pick one) and installs the dependencies above.

```bash
chmod +x setup.sh
./setup.sh
```

It installs the OS packages above and also clones the
[Limine bootloader](https://github.com/limine-bootloader/limine) (pinned to a
specific release tag) into `./limine`, which `make iso` needs to build the
bootable image. It will show what it's about to do and ask for confirmation
before making any changes. For non-interactive use:

```bash
./setup.sh --distro=fedora --yes
./setup.sh --distro=arch --yes
```

Run `./setup.sh --help` for all options.

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
