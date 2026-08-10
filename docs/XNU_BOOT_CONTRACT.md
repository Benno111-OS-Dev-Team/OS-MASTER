# XNU Boot Contract

This contract defines the repository-owned boot interface for the external XNU
kernel provider. The XNU source tree remains read-only input under
`External/xnu`; boot adaptation code, generated metadata, and packaged media
must live under project-owned paths.

## Provider Inputs

- Kernel payload: `build/<arch>/kernel/xnu-<arch>.kernel`
- Provider manifest: `build/<arch>/kernel/xnu-provider.manifest`
- Boot handoff manifest: `build/<arch>/xnu-boot/xnu-boot-handoff.manifest`
- Boot handoff ABI: `boot/xnu/xnu_boot_handoff.h`
- Provider media: `image/xnu-<arch>-provider.tar.gz`

The provider manifest must identify the external source origin, commit, source
state, artifact path, architecture, and build mode.

## Loader Requirements

A bootable XNU media path must provide these inputs to the XNU entry path:

- A compiled XNU kernel payload from the selected provider artifact.
- A boot argument payload for the selected architecture.
- A memory map with usable, reserved, firmware, and device ranges.
- A firmware services handoff state appropriate for the selected architecture.
- CPU topology and timer information.
- Device tree or platform expert data required by the selected XNU target.
- Framebuffer information when graphical boot is requested.
- Init process or early userspace location metadata once userspace handoff is
  implemented.

## Invariants

- The loader must not write into `External/xnu`.
- Generated loader metadata must be staged under `build/<arch>/`.
- The packaged boot handoff ABI must use magic `0x584E55424F4F5431` and
  version `1`.
- The packaged boot handoff ABI must pass `make check-xnu-boot-abi`.
- The generated boot handoff manifest must publish the ABI struct name, struct
  size, framebuffer offset, target architecture ID, platform kind, and required
  boot flags used by the packaged ABI.
- Release or workflow artifacts must include this contract with the provider
  media.
- Release or workflow artifacts must include the generated boot handoff
  manifest with the provider media.
- A provider media archive that does not contain a compiled kernel payload must
  mark `payload_mode=source-validation`.
- A provider media archive that contains a compiled kernel payload must mark
  `payload_mode=compiled`.
