# OS8 top-level build entry point.
#
# The selected kernel provider is owned by Makefile.multiarch.  Keep this file
# as a compatibility front door so plain `make` uses the same provider path as
# CI and release builds.

ARCH ?= arm64
KERNEL_PROVIDER ?= xnu

MULTIARCH_MAKEFILE := Makefile.multiarch
MULTIARCH_ARGS := -f $(MULTIARCH_MAKEFILE) ARCH=$(ARCH) KERNEL_PROVIDER=$(KERNEL_PROVIDER)

UNAME_S := $(shell uname -s)

.PHONY: all kernel image sdk qemu qemu-bios qemu-uefi qemu-debug clean distclean \
        check-source-complete check-xnu-integration check-xnu-build-env check-xnu-boot-abi check-xnu-boot-surface check-xnu-uefi-handoff check-xnu-entry-handoff check-xnu-uefi-boot-image check-xnu-uefi-boot-smoke check-xnu-compiled-uefi-boot-smoke check-xnu-provider-workflow check-xnu-provider-proof \
        storage-format-stress storage-driver-compile-check custom-uefi-chain xnu-uefi-chain \
        os8-all os8-kernel os8-image xnu-kernel xnu-image \
        x86_64 x86_64-kernel x86_64-image x86_64-qemu x86_64-qemu-bios \
        x86_64-qemu-uefi x86_64-qemu-debug \
        drivers libc userspace runtimes test run run-gui run-gpu toolchain help

all kernel image sdk qemu qemu-bios qemu-uefi qemu-debug clean check-source-complete \
check-xnu-integration check-xnu-build-env check-xnu-boot-abi check-xnu-boot-surface check-xnu-uefi-handoff check-xnu-entry-handoff check-xnu-uefi-boot-image check-xnu-uefi-boot-smoke check-xnu-compiled-uefi-boot-smoke check-xnu-provider-workflow check-xnu-provider-proof storage-format-stress storage-driver-compile-check \
custom-uefi-chain xnu-uefi-chain xnu-kernel xnu-image:
	@$(MAKE) $(MULTIARCH_ARGS) $@

os8-all:
	@$(MAKE) -f $(MULTIARCH_MAKEFILE) ARCH=$(ARCH) KERNEL_PROVIDER=os8 all

os8-kernel:
	@$(MAKE) -f $(MULTIARCH_MAKEFILE) ARCH=$(ARCH) KERNEL_PROVIDER=os8 os8-kernel

os8-image:
	@$(MAKE) -f $(MULTIARCH_MAKEFILE) ARCH=$(ARCH) KERNEL_PROVIDER=os8 os8-image

x86_64:
	@$(MAKE) -f $(MULTIARCH_MAKEFILE) ARCH=x86_64 KERNEL_PROVIDER=$(KERNEL_PROVIDER) all

x86_64-kernel:
	@$(MAKE) -f $(MULTIARCH_MAKEFILE) ARCH=x86_64 KERNEL_PROVIDER=$(KERNEL_PROVIDER) kernel

x86_64-image:
	@$(MAKE) -f $(MULTIARCH_MAKEFILE) ARCH=x86_64 KERNEL_PROVIDER=$(KERNEL_PROVIDER) image

x86_64-qemu:
	@$(MAKE) -f $(MULTIARCH_MAKEFILE) ARCH=x86_64 KERNEL_PROVIDER=$(KERNEL_PROVIDER) qemu

x86_64-qemu-bios:
	@$(MAKE) -f $(MULTIARCH_MAKEFILE) ARCH=x86_64 KERNEL_PROVIDER=$(KERNEL_PROVIDER) qemu-bios

x86_64-qemu-uefi:
	@$(MAKE) -f $(MULTIARCH_MAKEFILE) ARCH=x86_64 KERNEL_PROVIDER=$(KERNEL_PROVIDER) qemu-uefi

x86_64-qemu-debug:
	@$(MAKE) -f $(MULTIARCH_MAKEFILE) ARCH=x86_64 KERNEL_PROVIDER=$(KERNEL_PROVIDER) qemu-debug

drivers libc userspace runtimes run run-gui run-gpu:
	@echo "[BUILD] $@ is not a selected-provider target."
	@echo "[BUILD] Use 'make kernel' or 'make image'; add KERNEL_PROVIDER=os8 for the OS8 compatibility kernel."
	@exit 2

test:
	@$(MAKE) $(MULTIARCH_ARGS) check-source-complete
	@$(MAKE) $(MULTIARCH_ARGS) kernel

distclean:
	@$(MAKE) -f $(MULTIARCH_MAKEFILE) ARCH=$(ARCH) KERNEL_PROVIDER=$(KERNEL_PROVIDER) clean-all

toolchain:
	@echo "[TOOLCHAIN] Installing build dependencies..."
	@if [ "$(UNAME_S)" = "Darwin" ]; then \
		./scripts/setup-toolchain.sh; \
	else \
		./scripts/setup-toolchain-linux.sh; \
	fi

help:
	@echo "OS8 Build System"
	@echo "================"
	@echo ""
	@echo "The default kernel provider is XNU:"
	@echo "  make kernel"
	@echo "  make image"
	@echo "  make ARCH=x86_64 image"
	@echo ""
	@echo "Use the OS8 compatibility kernel explicitly when needed:"
	@echo "  make KERNEL_PROVIDER=os8 kernel"
	@echo "  make os8-image ARCH=x86_64"
	@echo ""
	@echo "Provider targets are delegated to Makefile.multiarch."
