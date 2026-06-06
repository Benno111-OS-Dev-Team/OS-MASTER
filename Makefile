PRODUCT_NAME ?= os-master-freebsd
FREEBSD_RELEASE ?= 14.4-RELEASE
FREEBSD_ARCH ?= amd64
FREEBSD_IMAGE_BASENAME ?= disc1.iso
BUILD_DIR ?= build/freebsd
OUTPUT_DIR ?= image

.PHONY: all image qemu clean help

all: image

image:
	@PRODUCT_NAME="$(PRODUCT_NAME)" \
	FREEBSD_RELEASE="$(FREEBSD_RELEASE)" \
	FREEBSD_ARCH="$(FREEBSD_ARCH)" \
	FREEBSD_IMAGE_BASENAME="$(FREEBSD_IMAGE_BASENAME)" \
	BUILD_DIR="$(BUILD_DIR)" \
	OUTPUT_DIR="$(OUTPUT_DIR)" \
	bash ./scripts/fetch-freebsd-release.sh

qemu: image
	@PRODUCT_NAME="$(PRODUCT_NAME)" \
	FREEBSD_RELEASE="$(FREEBSD_RELEASE)" \
	FREEBSD_ARCH="$(FREEBSD_ARCH)" \
	FREEBSD_IMAGE_BASENAME="$(FREEBSD_IMAGE_BASENAME)" \
	OUTPUT_DIR="$(OUTPUT_DIR)" \
	bash ./scripts/run-freebsd-amd64-qemu.sh

clean:
	@rm -rf build image

help:
	@echo "OS-MASTER FreeBSD Wrapper"
	@echo ""
	@echo "Targets:"
	@echo "  image  - Download, verify, and stage the official FreeBSD image"
	@echo "  qemu   - Boot the staged amd64 installer image in QEMU"
	@echo "  clean  - Remove local build and image output"
	@echo ""
	@echo "Variables:"
	@echo "  PRODUCT_NAME           Default: $(PRODUCT_NAME)"
	@echo "  FREEBSD_RELEASE        Default: $(FREEBSD_RELEASE)"
	@echo "  FREEBSD_ARCH           Default: $(FREEBSD_ARCH)"
	@echo "  FREEBSD_IMAGE_BASENAME Default: $(FREEBSD_IMAGE_BASENAME)"
	@echo "  BUILD_DIR              Default: $(BUILD_DIR)"
	@echo "  OUTPUT_DIR             Default: $(OUTPUT_DIR)"
