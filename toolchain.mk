# Auto-generated toolchain configuration for Linux
# Source this file or include in Makefile

# LLVM paths (Linux system paths)
LLVM_PATH := /usr/bin

# Toolchain binaries
export CC := $(LLVM_PATH)/clang
export CXX := $(LLVM_PATH)/clang++
export LD := $(LLVM_PATH)/ld.lld
export AR := $(LLVM_PATH)/llvm-ar
export AS := $(LLVM_PATH)/clang
export OBJCOPY := $(LLVM_PATH)/llvm-objcopy
export OBJDUMP := $(LLVM_PATH)/llvm-objdump
export STRIP := $(LLVM_PATH)/llvm-strip
export NM := $(LLVM_PATH)/llvm-nm
export RANLIB := $(LLVM_PATH)/llvm-ranlib

# Add to PATH
export PATH := $(LLVM_PATH):$(PATH)
