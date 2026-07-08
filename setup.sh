#!/bin/bash
set -e

echo "Detecting package manager to install BashketKernel build dependencies..."

if command -v apt-get &> /dev/null; then
    echo "Detected apt (Debian/Ubuntu)..."
    sudo apt-get update
    sudo apt-get install -y build-essential xorriso qemu-system-x86 gdb
elif command -v pacman &> /dev/null; then
    echo "Detected pacman (Arch Linux)..."
    sudo pacman -Sy --needed --noconfirm base-devel xorriso qemu-desktop gdb
elif command -v dnf &> /dev/null; then
    echo "Detected dnf (Fedora/RHEL)..."
    sudo dnf groupinstall -y "Development Tools"
    sudo dnf install -y xorriso qemu-system-x86 gdb
elif command -v zypper &> /dev/null; then
    echo "Detected zypper (openSUSE)..."
    sudo zypper install -y -t pattern devel_basis
    sudo zypper install -y xorriso qemu-x86 gdb
else
    echo "Could not detect a supported package manager (apt, pacman, dnf, zypper)."
    echo "Please manually install: a C compiler suite (gcc/make), xorriso, qemu-system-x86, and gdb."
    exit 1
fi

echo ""
echo "Dependencies installed successfully!"
echo "You can now run 'make iso' to build the kernel or 'make run' to boot it in QEMU."
    