# OS8

OS8 is a Unix-like operating system project with a multi-architecture build system and an actively evolving kernel, GUI, installer, and storage stack.

## License

This repository is released under a very restrictive proprietary license.

- All rights are reserved for the original project material.
- No copying, modification, redistribution, derivative works, or reuse is permitted without prior written permission.
- Third-party components bundled in the repository keep their own separate license terms where noted.

See [LICENSE](LICENSE) for the full terms.

The repository currently targets:

- `arm64` for the main UEFI-based build flow
- `x86_64` for Limine-based bring-up, GUI work, and installer development
- `x86` for a legacy BIOS-oriented path

This project is still in heavy development. Features, layouts, boot flows, and on-disk formats are changing often.

## What Is In This Repo

- A freestanding kernel with architecture-specific bring-up code
- An in-memory VFS and RAM-backed root filesystem
- A desktop-style GUI, file manager, installer UI, and basic apps
- Storage probing and simple partition management
- Boot image generation scripts for different targets
- Runtime-seeded demo content, applications, and installer payloads

## Current State

The x86_64 path is the fastest-moving part of the project right now. It includes:

- Limine boot support
- Framebuffer GUI bring-up
- PCI and basic storage discovery
- PS/2 input
- An installer workflow that stages a bundled system image
- A setup-media mirror exposed at `/setup/` while running the installer

The default x86_64 boot profiles now start in `drivers=generic` mode so the OS
leans on the most conservative shared low-level path first instead of
auto-enabling experimental vendor-specific PCI, GPU, audio, USB, and network
bring-up during early boot.

The live environment is still largely RAMFS-based. The installer writes a staged system image into an installed target layout, but this is still an experimental OS project, not a finished general-purpose system.

## Repository Layout

```text
assets/               Shared art, wallpapers, and branding assets
assets/boot-assets/   Boot UI artwork and installer imagery
assets/screenshots/   Captured product and UI screenshots
boot/                 Boot configs and bootloader-related assets
shared-api/           Versioned headers shared across repo boundaries
docs/                 Build notes and supporting documentation
drivers/              Shared driver code
fixes/                Patch snapshots and one-off fix bundles
kernel/               Core kernel, arch code, GUI, FS, media, apps
libc/                 C library work
runtimes/             Runtime/toolchain experiments
scripts/              Image creation and helper scripts
userspace/            Userspace programs, support headers, and examples
os-x86_64/            Limine config and x86_64 boot assets
Makefile.multiarch    Main build entry point
```

Repo hygiene notes:

- generated output belongs under `build/` and `image/`
- exported app SDK headers are staged under `build/sdk/include/`
- helper launchers live under `scripts/`
- standalone docs belong under `docs/`
- ad-hoc patch files belong under `fixes/`

## Build System

The main entry point is:

```sh
make -f Makefile.multiarch ARCH=<arch> <target>
```

If your default `make` already uses `Makefile.multiarch`, you can usually run:

```sh
make ARCH=<arch> <target>
```

Supported architectures:

- `ARCH=arm64`
- `ARCH=x86_64`
- `ARCH=x86`

Common targets:

- `all` builds the kernel and boot image
- `kernel` builds only the kernel
- `sdk` exports app-facing headers under `build/sdk/include/`
- `image` builds the bootable image or ISO
- `system-image` builds the staged x86_64 install tree and `system-image.zip`
- `install-boot-files` stages boot files into an existing `INSTALL_ROOT`
- `installer-image` builds the x86_64 installer ISO
- `qemu` runs the default emulator flow
- `qemu-bios` runs BIOS boot where supported
- `qemu-uefi` runs UEFI boot where supported
- `clean` removes build artifacts for the selected architecture

## Toolchain Expectations

The project expects a freestanding LLVM-style toolchain and emulator utilities.

Typical requirements:

- `clang`
- `ld.lld`
- `llvm-ar`
- `llvm-objcopy`
- `llvm-objdump`
- `qemu-system-*`

For x86_64 ISO creation, the current scripts also expect:

- `xorriso`
- Boot assets refreshed automatically from the latest OS-BOOT-MANAGER release during the x86_64 build

Host assumptions in the current build system are best on Linux or macOS-style environments. Some helper scripts are shell-based and assume Unix tooling.

For Windows-based repo automation, use:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\auto-push.ps1 -Message "Your commit message"
```

The helper looks for `git.exe` on `PATH`, standard Git installs, and GitHub
Desktop's bundled Git, then performs `git add -A`, `git commit -m ...`, and
`git push origin HEAD`.

The Windows desktop launcher lives at `scripts/run-desktop.cmd`.

For Linux host setup notes, see `docs/BUILD_LINUX.md`.

## Quick Start

### x86_64 installer ISO

```sh
make ARCH=x86_64 installer-image
```

This produces:

```text
image/os8-x86_64-installer.iso
```

To build just the packaged OS image used by the installer:

```sh
make ARCH=x86_64 system-image
```

To stage boot files into an existing OS install root:

```sh
make ARCH=x86_64 install-boot-files INSTALL_ROOT=/path/to/os-root
```

### x86_64 default image

```sh
make ARCH=x86_64 image
```

This produces:

```text
image/os-x86_64.iso
```

### ARM64 image

```sh
make ARCH=arm64 image
```

### Run in QEMU

```sh
make ARCH=x86_64 qemu
make ARCH=x86_64 qemu-uefi
make ARCH=arm64 qemu-uefi
```

## x86_64 Installer Flow

The x86_64 installer image is built around a Limine-booted GUI environment.

The build now prepares the OS install payload ahead of time:

- a staged install tree at `build/x86_64/system-image`
- a packaged archive at `build/x86_64/system-image.zip`

At installer boot, the media exposes:

- a bundled system image under `/install/system-image`
- a packaged archive at `/install/system-image.zip`
- a setup-media mirror under `/setup/`

The installer UI prefers the packaged image archive and uses the staged tree as a compatible fallback.

Important notes:

- `/setup/` is currently a mirrored runtime view of the installer media, not a true ISO9660 mount
- the installer path is still evolving
- storage and partition handling are development-grade and should be treated carefully

## Runtime Filesystem Model

A lot of the current OS experience is seeded into RAMFS during boot.

That seeded content includes things like:

- `Desktop`
- `Documents`
- `Downloads`
- `Pictures`
- `System`
- `bin`
- `sbin`
- `usr`
- `examples`

It also seeds demo files and embedded binaries such as:

- `/sbin/init`
- `/bin/login`
- `/bin/sh`
- desktop notes and readme files
- sample media assets
- example scripts and NanoLang samples

That same baseline tree is now built outside the kernel and packaged into the install payload so the installer can copy a real image instead of reconstructing the OS layout at runtime.

## Boot Images

The x86_64 ISO builder script creates a hybrid BIOS+UEFI image and validates that key payload files exist inside the final ISO.

The generated installer image includes:

- top-level boot files used by the installer environment
- a bundled install payload under `/install/system-image`
- a packaged install archive at `/install/system-image.zip`

The x86_64 build uses Limine configuration files from:

- `os-x86_64/limine.conf`
- `os-x86_64/limine-installer.conf`

## Development Notes

This is an experimental codebase. Expect rough edges in:

- storage handling
- persistent installation behavior
- filesystem semantics
- nested path handling
- GUI/window manager behavior
- boot/runtime parity across architectures

When working in this repo, it is a good idea to verify changes against actual serial logs or VM boots rather than assuming the build scripts and runtime behavior fully match.

## Recommended Workflow

For x86_64 bring-up and installer work:

1. Build `ARCH=x86_64 installer-image`
2. Boot it in QEMU with serial output enabled
3. Watch the serial log for storage detection, setup payload staging, and installer status
4. Reboot into the installed target and compare behavior against the live seeded environment

### xHCI Bring-up Toggle (x86_64)

On the current x86/x86_64 bring-up path, the shipped boot profiles default to
`drivers=generic` for stability. That keeps the conservative shared low-level
path in place and skips the experimental vendor-specific components during
early boot.

To opt back into the more experimental x86 PCI/USB bring-up during testing,
remove `drivers=generic` from the Limine cmdline and, for xHCI specifically,
add this kernel cmdline flag:

```text
xhci=on
```

Without that flag, the kernel keeps xHCI off on x86 and relies on the other
input paths.

## Known Reality Of The Project

This repository is not a polished distribution or a production OS.

It is a development playground for:

- kernel experimentation
- desktop/UI iteration
- bootloader and installer work
- architecture bring-up
- filesystem and storage prototyping

That is also what makes it useful: most of the interesting systems work is visible, editable, and still moving.



## Cleaning Builds

Remove build output for one architecture:

```sh
make ARCH=x86_64 clean
```

If needed, remove everything under `build/` and `image/` using the available clean targets in the build system.

## Contributing To Your Own Sanity

If you are changing boot or installer behavior, always check:

- the serial log
- the generated ISO contents
- the live root tree
- the staged `/install/system-image` tree
- the staged `/setup/` tree in installer mode

Those paths are often the fastest way to see whether a change landed in the runtime you actually booted.
