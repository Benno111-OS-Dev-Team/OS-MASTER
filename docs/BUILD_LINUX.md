# Building OS8 on Ubuntu/Linux

This guide helps you build OS8 on Ubuntu or other Linux distributions.

## Quick Start

```bash
# Install dependencies (automated)
./scripts/setup-toolchain-linux.sh

# Fetch external XNU source for the default kernel provider
git clone https://github.com/apple-oss-distributions/xnu.git External/xnu
chmod -R a-w External/xnu

# Validate the XNU provider on Linux
XNU_SOURCE_VALIDATION_ONLY=1 make kernel

# Build the previous OS8 bootable image if you need a local QEMU path
make KERNEL_PROVIDER=os8 image
```

## Manual Installation

If the automated script doesn't work, install dependencies manually:

```bash
sudo apt update
sudo apt install -y \
    clang \
    lld \
    llvm \
    llvm-runtime \
    libc6-dev \
    build-essential \
    make \
    cmake \
    ninja-build \
    nasm \
    qemu-system-arm \
    qemu-efi-aarch64 \
    ovmf \
    python3 \
    wget \
    curl \
    git \
    dosfstools \
    e2fsprogs \
    xorriso \
    mtools

# Optional: ARM64 GCC toolchain
sudo apt install -y gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu
```

## Verify Installation

```bash
# Check Clang version
clang --version

# Check LLD linker
ld.lld --version

# Check QEMU ARM64
qemu-system-aarch64 --version
```

## Build Commands

```bash
# Build selected provider media
XNU_SOURCE_VALIDATION_ONLY=1 make all

# Build selected kernel provider only
XNU_SOURCE_VALIDATION_ONLY=1 make kernel

# Build previous OS8 compatibility kernel only
make KERNEL_PROVIDER=os8 kernel

# Create selected provider media
XNU_SOURCE_VALIDATION_ONLY=1 make image

# Create previous OS8 boot media
make KERNEL_PROVIDER=os8 image
```

## x86_64 Build

```bash
# Build x86_64 XNU provider media
XNU_SOURCE_VALIDATION_ONLY=1 make x86_64

# Build only the x86_64 XNU provider
XNU_SOURCE_VALIDATION_ONLY=1 make x86_64-kernel

# Create the x86_64 XNU provider media archive
XNU_SOURCE_VALIDATION_ONLY=1 make x86_64-image

# Create the previous OS8 x86_64 hybrid ISO
make KERNEL_PROVIDER=os8 x86_64-image
```

## Run in QEMU

```bash
# With OS8 compatibility boot image
make KERNEL_PROVIDER=os8 qemu

# Debug mode with OS8 compatibility kernel
make KERNEL_PROVIDER=os8 qemu-debug

# x86_64 OS8 compatibility in QEMU
make KERNEL_PROVIDER=os8 x86_64-qemu

# x86_64 BIOS boot
make KERNEL_PROVIDER=os8 x86_64-qemu-bios

# x86_64 UEFI boot
make KERNEL_PROVIDER=os8 x86_64-qemu-uefi

# x86_64 with GDB server
make KERNEL_PROVIDER=os8 x86_64-qemu-debug
```

## Troubleshooting

### Missing LLVM tools
If you get "clang: command not found":
```bash
sudo apt install clang lld llvm
```

### QEMU not found
```bash
sudo apt install qemu-system-arm qemu-efi-aarch64
```

### Build errors
Make sure you have all dependencies installed:
```bash
sudo apt install build-essential make cmake nasm
```

### Permission denied on setup script
```bash
chmod +x scripts/setup-toolchain-linux.sh
./scripts/setup-toolchain-linux.sh
```

## Platform Differences

The Makefile now auto-detects your OS:
- **macOS**: Uses Homebrew paths (`/opt/homebrew/opt/llvm/bin`)
- **Linux**: Uses system paths (`/usr/bin`)

You can override toolchain paths via environment variables:
```bash
export LLVM_PATH=/custom/path/llvm/bin
XNU_SOURCE_VALIDATION_ONLY=1 make kernel
```

## CPU Target

The build uses `-mcpu=cortex-a72` which works on:
- QEMU virt machine (default)
- Raspberry Pi 4
- Most ARM64 development boards

For Apple Silicon, use macOS build with `-mcpu=apple-m2` (modify Makefile if needed).

## Next Steps

See [AGENTS.md](../AGENTS.md) for detailed development guide.
