# XNU Integration

OS8 treats Apple XNU as an external kernel source tree under `External/xnu`.
That tree is read-only input. Do not patch, generate into, or commit files under
`External/xnu`; keep OS8 glue in project-owned files such as `Makefile.multiarch`,
`scripts/`, and `.github/workflows/`.

`make kernel` and `make -f Makefile.multiarch kernel` now dispatch through
`KERNEL_PROVIDER`. The default provider is `xnu`; `KERNEL_PROVIDER=os8` keeps
the previous OS8 freestanding kernel available while the XNU boot path is being
brought up.

CI fetches XNU from `https://github.com/apple-oss-distributions/xnu.git`, marks
the checkout read-only, and runs the top-level `make` XNU provider path in
source-validation mode on Ubuntu. The provider path requires the standalone XNU
checkout to remain clean, including untracked generated files. Real XNU
compilation is driven by `scripts/build-xnu-kernel.sh` on macOS with Xcode and
matching Apple kernel dependencies, with all generated objects rooted under
`build/<arch>/xnu`. The build wrapper validates the discovered compiled kernel
with the repository-owned Mach-O artifact verifier before staging it as
`build/<arch>/kernel/xnu-<arch>.kernel`.

The `XNU Provider Build` workflow is a manual macOS CI path for real XNU
provider compilation. It fetches XNU as read-only external input, runs the same
`KERNEL_PROVIDER=xnu` Makefile path, verifies the compiled provider artifact and
media archive, and uploads the provider media as a workflow artifact.

Before the macOS build starts, `make -f Makefile.multiarch
KERNEL_PROVIDER=xnu ARCH=<arch> check-xnu-build-env` verifies the Xcode tools,
SDK, standalone Apple XNU checkout identity, clean source state, and required
KDK path for ARM64 builds.

The selected kernel payload path is provider-owned:

- XNU: `build/<arch>/kernel/xnu-<arch>.kernel`
- OS8 compatibility: `build/<arch>/kernel/os-<arch>.elf`

Image creation scripts accept `KERNEL_PATH` so provider glue can hand them the
selected payload without modifying the external source tree.

`make KERNEL_PROVIDER=xnu ARCH=<arch> image` creates an XNU provider media
archive at `image/xnu-<arch>-provider.tar.gz`. On macOS with a completed XNU
build, the archive includes the compiled kernel payload. On Linux CI, it
includes the provider manifests produced by source-validation mode so CI can
still prove that the external read-only source boundary and selected provider
contract are intact.

When `ARCH=x86_64` and a compiled XNU payload exists,
`make KERNEL_PROVIDER=xnu ARCH=x86_64 image` also builds
`image/xnu-x86_64-uefi.img`. `make KERNEL_PROVIDER=xnu ARCH=x86_64 qemu-uefi`
boots that FAT UEFI image directly through OVMF. Source-validation provider
media intentionally does not produce a bootable XNU image because it contains no
compiled kernel payload, and CI rejects source-validation archives that carry a
stale kernel payload or bootable XNU UEFI artifacts.

Numbered CI releases publish XNU provider media for both `x86_64` and `arm64`
next to the OS8 compatibility boot media.

Each `metadata/xnu-provider.manifest` records the external source origin,
commit, and cleanliness state. CI asserts that release provider media is sourced
from the standalone Apple OSS XNU checkout and that the checkout is clean.

Each provider media archive also includes `docs/XNU_BOOT_CONTRACT.md`, which
defines the repository-owned loader handoff requirements for making the XNU
provider bootable. The media also carries `boot/xnu/xnu_boot_handoff.h` and
`boot/xnu/xnu_boot_handoff_builder.h`, the repository-owned ABI and builder
headers that loader code must use before entering XNU. It also carries
`boot/xnu/xnu_macho_loader.h`, the repository-owned Mach-O64 payload inspection
contract for validating and locating selected XNU kernel payload ranges, and
`boot/xnu/xnu_uefi_handoff.h`, the repository-owned helper for converting UEFI
memory-map and framebuffer inputs into the XNU handoff format. For x86_64 EFI
boot, the media also carries `boot/xnu/xnu_x86_64_boot_args.h`, which builds
XNU's 4096-byte `boot_args` payload from validated kernel, EFI memory-map,
firmware table, command-line, and framebuffer inputs.

`scripts/check-xnu-boot-surface.sh` verifies the selected external XNU checkout
still exposes the architecture boot entry, boot argument fields, platform
initialization, and machine-startup handoff expected by the provider contract.
The provider build path and CI both run this before accepting XNU media.

`scripts/check-xnu-provider-media.sh` is the CI gate for provider archives. It
validates the provider manifest, media manifest, boot contract, source policy,
architecture, payload mode, generated boot handoff manifest, generated boot
plan manifest, packaged boot handoff ABI, packaged Mach-O payload inspector,
and compiled kernel payload when present.

`scripts/check-xnu-kernel-artifact.sh` validates compiled provider payloads with
the repository-owned Mach-O inspector before packaging compiled media. The manual
macOS provider workflow runs this directly, and the provider media verifier runs
it again against the packaged kernel payload.

`make check-xnu-boot-abi` compiles a small C translation unit against
`boot/xnu/xnu_boot_handoff.h` and verifies the magic, version, enum values,
field offsets, and struct sizes used by the repository-owned handoff contract.
It also compiles the packaged x86_64 `boot_args` builder and checks the XNU
revision, version, EFI mode, memory-map fields, kernel fields, framebuffer
fields, command-line copy behavior, and 4096-byte size invariant. The provider
media verifier also emits layout metadata from the packaged copy of the
handoff header and compares it with `metadata/xnu-boot-handoff.manifest`.

`make check-xnu-uefi-handoff` compile-checks the XNU UEFI handoff helpers with
the same freestanding x86_64 Windows target style used by the custom UEFI boot
chain, proving the packaged helpers are consumable from loader code.

`make check-xnu-entry-handoff` compile-checks the custom UEFI assembly shim
that enters x86_64 XNU. The shim loads the selected page table and jumps to the
kernel entry with the physical `boot_args` address in `%edi`, matching XNU's
`osfmk/x86_64/start.s` bootstrap contract.

`make ARCH=x86_64 check-xnu-uefi-boot-image` builds a synthetic XNU-format
custom UEFI startup config, creates the FAT `xnu-x86_64-uefi.img`, and inspects
the image with mtools to prove the startup files, config, and `/boot/main.sys`
payload are staged where the XNU startup path expects them.

`make ARCH=x86_64 check-xnu-uefi-boot-smoke` boots the synthetic XNU UEFI image
through QEMU/OVMF and requires the custom startup executable to reach the XNU
Mach-O validation error path. This proves the firmware can load the XNU-configured
boot chain, while still stopping before any real XNU payload is required.

The custom x86_64 startup executable now keeps the OS8 ELF path as the default
and selects the XNU Mach-O64 path when `os8boot.cfg` contains
`kernel_format=xnu`. That path validates the trusted payload as XNU Mach-O64,
loads its segments, prepares the XNU EFI `boot_args`, captures the final EFI
memory map, and enters through `startup_enter_xnu_kernel`.

For compiled x86_64 XNU providers, `make -f Makefile.multiarch
KERNEL_PROVIDER=xnu ARCH=x86_64 xnu-uefi-chain` builds the custom UEFI startup
artifacts with `kernel_format=xnu` and a SHA-256 hash for the compiled XNU
payload. The macOS XNU provider workflow runs this before packaging media, and
compiled x86_64 provider archives must include `boot/custom-uefi/STARTUPX64.EFI`
and `boot/custom-uefi/os8boot.cfg`. The same target also creates
`image/xnu-x86_64-uefi.img`, a FAT UEFI boot image that stages the compiled XNU
payload at `/boot/main.sys` next to the XNU-format startup config.

Local usage:

```sh
git clone https://github.com/apple-oss-distributions/xnu.git External/xnu
chmod -R a-w External/xnu
make -f Makefile.multiarch xnu-kernel
make -f Makefile.multiarch ARCH=x86_64 KERNEL_PROVIDER=xnu image
```

For the compatibility OS8 media build:

```sh
make KERNEL_PROVIDER=os8 ARCH=x86_64 image
```

The existing OS8 freestanding kernel build remains available through the normal
`KERNEL_PROVIDER=os8` path while the XNU boot and ABI adaptation layer is
developed in repository-owned integration code.
