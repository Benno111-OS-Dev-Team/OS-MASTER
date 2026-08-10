# XNU Integration

OS8 treats Apple XNU as an external kernel source tree under `External/xnu`.
That tree is read-only input. Do not patch, generate into, or commit files under
`External/xnu`; keep OS8 glue in project-owned files such as `Makefile.multiarch`,
`scripts/`, and `.github/workflows/`.

`make -f Makefile.multiarch kernel` now dispatches through `KERNEL_PROVIDER`.
The default provider is `xnu`; `KERNEL_PROVIDER=os8` keeps the previous OS8
freestanding kernel available while the XNU boot path is being brought up.

CI fetches XNU from `https://github.com/apple-oss-distributions/xnu.git`, marks
the checkout read-only, and runs the XNU provider path in source-validation mode
on Ubuntu. Real XNU compilation is driven by `scripts/build-xnu-kernel.sh` on
macOS with Xcode and matching Apple kernel dependencies, with all generated
objects rooted under `build/<arch>/xnu`.

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

`make -f Makefile.multiarch KERNEL_PROVIDER=xnu ARCH=<arch> image` creates an
XNU provider media archive at `image/xnu-<arch>-provider.tar.gz`. On macOS with
a completed XNU build, the archive includes the compiled kernel payload. On
Linux CI, it includes the provider manifests produced by source-validation mode
so CI can still prove that the external read-only source boundary and selected
provider contract are intact.

Numbered CI releases publish XNU provider media for both `x86_64` and `arm64`
next to the OS8 compatibility boot media.

Each `metadata/xnu-provider.manifest` records the external source origin,
commit, and cleanliness state. CI asserts that release provider media is sourced
from the standalone Apple OSS XNU checkout and that the checkout is clean.

Each provider media archive also includes `docs/XNU_BOOT_CONTRACT.md`, which
defines the repository-owned loader handoff requirements for making the XNU
provider bootable.

`scripts/check-xnu-provider-media.sh` is the CI gate for provider archives. It
validates the provider manifest, media manifest, boot contract, source policy,
architecture, payload mode, generated boot handoff manifest, and compiled
kernel payload when present.

Local usage:

```sh
git clone https://github.com/apple-oss-distributions/xnu.git External/xnu
chmod -R a-w External/xnu
make -f Makefile.multiarch xnu-kernel
```

For the compatibility OS8 media build:

```sh
make -f Makefile.multiarch KERNEL_PROVIDER=os8 ARCH=x86_64 image
```

The existing OS8 freestanding kernel build remains available through the normal
`KERNEL_PROVIDER=os8` path while the XNU boot and ABI adaptation layer is
developed in repository-owned integration code.
