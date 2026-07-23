.section .rodata

.global installer_payload_bootx64_efi
.global installer_payload_bootx64_efi_end
installer_payload_bootx64_efi:
    .incbin "build/x86_64/boot/custom-uefi/BOOTX64.EFI"
installer_payload_bootx64_efi_end:

.global installer_payload_startupx64_efi
.global installer_payload_startupx64_efi_end
installer_payload_startupx64_efi:
    .incbin "build/x86_64/boot/custom-uefi/STARTUPX64.EFI"
installer_payload_startupx64_efi_end:

.global installer_payload_limine_bios_sys
.global installer_payload_limine_bios_sys_end
installer_payload_limine_bios_sys:
    .incbin "os-x86_64/limine/bin/limine-bios.sys"
installer_payload_limine_bios_sys_end:

.global installer_payload_limine_bios_cd_bin
.global installer_payload_limine_bios_cd_bin_end
installer_payload_limine_bios_cd_bin:
    .incbin "os-x86_64/limine/bin/limine-bios-cd.bin"
installer_payload_limine_bios_cd_bin_end:
