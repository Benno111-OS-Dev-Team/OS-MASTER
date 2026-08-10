# XNU Boot Contract

This contract defines the repository-owned boot interface for the external XNU
kernel provider. The XNU source tree remains read-only input under
`External/xnu`; boot adaptation code, generated metadata, and packaged media
must live under project-owned paths.

## Provider Inputs

- Kernel payload: `build/<arch>/kernel/xnu-<arch>.kernel`
- Provider manifest: `build/<arch>/kernel/xnu-provider.manifest`
- Boot handoff manifest: `build/<arch>/xnu-boot/xnu-boot-handoff.manifest`
- Boot plan manifest: `build/<arch>/xnu-boot/xnu-boot-plan.manifest`
- Boot handoff ABI: `boot/xnu/xnu_boot_handoff.h`
- Boot handoff builder: `boot/xnu/xnu_boot_handoff_builder.h`
- Mach-O payload inspector: `boot/xnu/xnu_macho_loader.h`
- UEFI handoff helper: `boot/xnu/xnu_uefi_handoff.h`
- x86_64 EFI boot args builder: `boot/xnu/xnu_x86_64_boot_args.h`
- x86_64 entry handoff shim: `boot/custom/startup-handoff.S`
- x86_64 startup loader: `boot/custom/startup.c`
- Source boot surface verifier: `scripts/check-xnu-boot-surface.sh`
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
- The generated boot handoff manifest must publish every
  `os8_xnu_boot_handoff_t` field as `handoff_field_<name>=<offset>:<type>`.
- Provider media verification must derive ABI layout metadata from the packaged
  header and compare it with the generated boot handoff manifest.
- The packaged boot handoff builder must compile against the packaged ABI and
  populate all `os8_xnu_boot_handoff_t` fields from validated loader inputs.
- The packaged boot handoff builder must derive kernel base, size, and entry
  from validated Mach-O payload metadata before the final handoff is built.
- The packaged boot handoff builder must validate and apply boot arguments,
  memory map, platform data, timer, CPU topology, and framebuffer inputs before
  the final handoff is built.
- The packaged UEFI handoff helper must convert UEFI memory descriptors into
  `os8_xnu_range_t` entries and convert UEFI framebuffer geometry into the
  repository-owned XNU handoff framebuffer format.
- The packaged x86_64 boot args builder must populate XNU's EFI `boot_args`
  revision `0`, version `2`, 64-bit EFI mode structure, preserve the 4096-byte
  size invariant, and reject values that do not fit the 32-bit physical address
  fields in the upstream x86_64 boot contract.
- The x86_64 UEFI entry shim must expose `startup_enter_xnu_kernel`, load the
  selected page table into `CR3`, place the 32-bit physical `boot_args` address
  in `%edi`, clear `%rbp`, and jump to the selected XNU entry address.
- The x86_64 startup loader must accept `kernel_format=xnu`, validate the
  trusted payload as XNU Mach-O64, load Mach-O segments, build XNU EFI
  `boot_args`, capture the final EFI memory map, and enter through the XNU
  entry shim without using the OS8 ELF/Limine handoff.
- The x86_64 startup loader must build XNU EFI `boot_args` after the final
  `GetMemoryMap` call and before `ExitBootServices` so build failures can still
  return a firmware error instead of entering XNU with a missing argument block.
- The x86_64 startup loader must place loaded XNU Mach-O segments below 4G so
  the upstream x86_64 `boot_args.kaddr` field can address the kernel payload.
- The x86_64 UEFI boot image creator must be covered by CI with a synthetic
  XNU-format payload and mtools inspection of the generated FAT image layout.
- The packaged Mach-O payload inspector must compile against the packaged ABI
  and expose validated segment file offsets, virtual ranges, protections, and
  entry metadata for the selected architecture before a loader enters XNU.
- The generated boot plan manifest must enumerate the selected XNU entry
  protocol and the loader steps required to enter the selected kernel payload.
- The external XNU checkout must pass the source boot surface verifier for the
  target architecture before CI accepts provider media.
- Release or workflow artifacts must include this contract with the provider
  media.
- Release or workflow artifacts must include the generated boot handoff
  manifest with the provider media.
- Release or workflow artifacts must include the generated boot plan manifest
  with the provider media.
- A provider media archive that does not contain a compiled kernel payload must
  mark `payload_mode=source-validation`.
- A source-validation provider media archive must not include a compiled kernel
  payload or bootable XNU UEFI artifacts.
- A provider media archive that contains a compiled kernel payload must mark
  `payload_mode=compiled`.
- A compiled x86_64 provider media archive must include custom UEFI startup
  artifacts under `boot/custom-uefi/` with `kernel_format=xnu` and a SHA-256
  hash for the packaged XNU kernel payload.
- A compiled x86_64 provider media archive must include a FAT UEFI boot image
  at `image/xnu-x86_64-uefi.img` containing `/EFI/BOOT/BOOTX64.EFI`,
  `/EFI/OS8/STARTUPX64.EFI`, `/EFI/OS8/os8boot.cfg`, and `/boot/main.sys`.
