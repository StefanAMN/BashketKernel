#!/bin/bash
set -e

echo "Installing BashketKernel build dependencies for Debian/Ubuntu..."
sudo apt-get update
sudo apt-get install -y build-essential xorriso qemu-system-x86 gdb

echo ""
echo "Dependencies installed successfully!"
echo "You can now run 'make iso' to build the kernel or 'make run' to boot it in QEMU."
