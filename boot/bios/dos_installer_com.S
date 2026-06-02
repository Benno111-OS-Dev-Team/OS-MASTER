/*
 * OS8 DOS runnable rescue installer utility.
 *
 * Build output is a flat .COM binary intended to be copied next to OSSYS.IMG
 * on an existing DOS system. The program writes that image to a selected BIOS
 * hard disk using Int 13h extensions.
 */

.code16
.section .text
.global _start

#define IMAGE_FILENAME "OSSYS.IMG"
#define BOOT_AREA_SECTORS 2048

_start:
    cli
    mov     %cs, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %ss
    mov     $stack_top, %sp
    cld                     /* DOS does not guarantee DF, but string ops below do. */
    sti

    movb    $0, selected_drive
    mov     $0x0003, %ax
    int     $0x10
    call    find_default_target

main_menu:
    call    clear_screen
    mov     $chrome_header_text, %si
    call    print_string
    mov     $title_text, %si
    call    print_string
    mov     $subtitle_text, %si
    call    print_string
    call    print_target_summary
    mov     $menu_text, %si
    call    print_string
    mov     $footer_text, %si
    call    print_string
    call    read_key

    cmp     $0x3D, %ah
    je      exit_to_dos
    cmp     $13, %al
    je      install_image
    cmp     $'c', %al
    je      select_target_disk
    cmp     $'C', %al
    je      select_target_disk
    cmp     $'r', %al
    je      apply_bootloader_menu
    cmp     $'R', %al
    je      apply_bootloader_menu
    cmp     $'h', %al
    je      show_help
    cmp     $'H', %al
    je      show_help
    cmp     $27, %al
    je      exit_to_dos
    jmp     main_menu

install_image:
    call    clear_screen
    mov     $chrome_header_text, %si
    call    print_string
    mov     $install_intro_text, %si
    call    print_string
    call    print_target_summary

    movb    selected_drive, %al
    test    %al, %al
    jz      no_target_disk

    mov     $confirm_text, %si
    call    print_string
    call    read_key
    cmp     $0x3D, %ah
    je      main_menu
    cmp     $13, %al
    je      do_install
    jmp     main_menu

do_install:
    call    clear_screen
    mov     $chrome_header_text, %si
    call    print_string
    mov     $copy_begin_text, %si
    call    print_string
    call    print_target_summary
    call    install_image_to_target
    jc      install_failed

    mov     $copy_success_text, %si
    call    print_string
    call    wait_for_keypress
    jmp     main_menu

install_failed:
    mov     $copy_failed_text, %si
    call    print_string
    call    wait_for_keypress
    jmp     main_menu

no_target_disk:
    mov     $no_target_text, %si
    call    print_string
    call    wait_for_keypress
    jmp     main_menu

apply_bootloader_menu:
    call    clear_screen
    mov     $chrome_header_text, %si
    call    print_string
    mov     $bootloader_intro_text, %si
    call    print_string
    call    print_target_summary

    movb    selected_drive, %al
    test    %al, %al
    jz      no_target_disk

    mov     $bootloader_confirm_text, %si
    call    print_string
    call    read_key
    cmp     $0x3D, %ah
    je      main_menu
    cmp     $13, %al
    je      do_apply_bootloader
    jmp     main_menu

do_apply_bootloader:
    call    clear_screen
    mov     $chrome_header_text, %si
    call    print_string
    mov     $bootloader_begin_text, %si
    call    print_string
    call    print_target_summary
    call    install_bootloader_to_target
    jc      bootloader_failed

    mov     $bootloader_success_text, %si
    call    print_string
    call    wait_for_keypress
    jmp     main_menu

bootloader_failed:
    mov     $bootloader_failed_text, %si
    call    print_string
    call    wait_for_keypress
    jmp     main_menu

select_target_disk:
    call    select_next_target
    call    clear_screen
    mov     $chrome_header_text, %si
    call    print_string
    jc      select_target_none
    mov     $target_updated_text, %si
    call    print_string
    call    print_target_summary
    call    wait_for_keypress
    jmp     main_menu

select_target_none:
    mov     $target_missing_text, %si
    call    print_string
    call    wait_for_keypress
    jmp     main_menu

show_help:
    call    clear_screen
    mov     $chrome_header_text, %si
    call    print_string
    mov     $help_text, %si
    call    print_string
    call    print_target_summary
    call    wait_for_keypress
    jmp     main_menu

clear_screen:
    push    %ax
    push    %bx
    push    %cx
    push    %dx
    mov     $0x0600, %ax
    mov     $0x1F00, %bx
    mov     $0x0000, %cx
    mov     $0x184F, %dx
    int     $0x10
    mov     $0x0200, %ax
    xor     %bx, %bx
    xor     %dx, %dx
    int     $0x10
    pop     %dx
    pop     %cx
    pop     %bx
    pop     %ax
    ret

read_key:
    mov     $0x0000, %ax
    int     $0x16
    ret

wait_for_keypress:
    mov     $press_key_text, %si
    call    print_string
    call    read_key
    ret

print_string:
    push    %ax
    push    %bx
print_string_loop:
    lodsb
    test    %al, %al
    jz      print_string_done
    mov     $0x0E, %ah
    mov     $0x0007, %bx
    int     $0x10
    jmp     print_string_loop
print_string_done:
    pop     %bx
    pop     %ax
    ret

print_char:
    push    %ax
    push    %bx
    mov     $0x0E, %ah
    mov     $0x0007, %bx
    int     $0x10
    pop     %bx
    pop     %ax
    ret

print_hex_nibble:
    and     $0x0F, %al
    cmp     $10, %al
    jb      print_hex_nibble_digit
    add     $'A' - 10, %al
    jmp     print_char
print_hex_nibble_digit:
    add     $'0', %al
    jmp     print_char

print_hex_byte:
    push    %ax
    mov     %al, %ah
    shr     $4, %al
    call    print_hex_nibble
    mov     %ah, %al
    call    print_hex_nibble
    pop     %ax
    ret

print_drive_id:
    push    %ax
    mov     $'0', %al
    call    print_char
    mov     $'x', %al
    call    print_char
    pop     %ax
    call    print_hex_byte
    ret

print_target_summary:
    push    %ax
    mov     $image_file_text, %si
    call    print_string
    mov     $image_filename_text, %si
    call    print_string
    mov     $crlf_text, %si
    call    print_string
    mov     $target_drive_text, %si
    call    print_string
    movb    selected_drive, %al
    test    %al, %al
    jz      print_target_summary_none
    call    print_drive_id
    mov     $crlf_text, %si
    call    print_string
    pop     %ax
    ret
print_target_summary_none:
    mov     $none_text, %si
    call    print_string
    pop     %ax
    ret

probe_drive:
    push    %ax
    push    %bx
    push    %cx
    push    %dx
    mov     $0x55AA, %bx
    mov     $0x4100, %ax
    int     $0x13
    jc      dos_probe_drive_fail
    cmp     $0xAA55, %bx
    jne     dos_probe_drive_fail
    test    $0x0001, %cx
    jz      dos_probe_drive_fail
    clc
    jmp     dos_probe_drive_done
dos_probe_drive_fail:
    stc
dos_probe_drive_done:
    pop     %dx
    pop     %cx
    pop     %bx
    pop     %ax
    ret

find_default_target:
    movb    $0, selected_drive
    movb    $0x81, %bl
dos_find_target_scan:
    cmpb    $0x88, %bl
    jae     dos_find_target_fallback
    movb    %bl, %dl
    call    probe_drive
    jc      dos_find_target_next
    movb    %bl, selected_drive
    ret
dos_find_target_next:
    inc     %bl
    jmp     dos_find_target_scan
dos_find_target_fallback:
    movb    $0x80, %dl
    call    probe_drive
    jc      dos_find_target_done
    movb    $0x80, selected_drive
dos_find_target_done:
    ret

select_next_target:
    push    %bx
    push    %cx
    push    %dx
    movb    selected_drive, %bl
    cmpb    $0x80, %bl
    jae     dos_select_target_cycle
    movb    $0x80, %bl
dos_select_target_cycle:
    movb    $8, %cl
dos_select_target_try:
    inc     %bl
    cmpb    $0x88, %bl
    jb      dos_select_target_check
    movb    $0x80, %bl
dos_select_target_check:
    movb    %bl, %dl
    call    probe_drive
    jc      dos_select_target_continue
    movb    %bl, selected_drive
    clc
    jmp     dos_select_target_done
dos_select_target_continue:
    dec     %cl
    jnz     dos_select_target_try
    movb    $0, selected_drive
    stc
dos_select_target_done:
    pop     %dx
    pop     %cx
    pop     %bx
    ret

bios_reset_drive:
    push    %ax
    mov     $0x0000, %ax
    int     $0x13
    pop     %ax
    ret

bios_write_dap_with_retry:
    push    %ax
    push    %cx
    mov     $3, %cx
dos_bios_write_retry:
    mov     $0x4300, %ax
    int     $0x13
    jnc     dos_bios_write_success
    call    bios_reset_drive
    loop    dos_bios_write_retry
    stc
    jmp     dos_bios_write_done
dos_bios_write_success:
    clc
dos_bios_write_done:
    pop     %cx
    pop     %ax
    ret

install_image_to_target:
    push    %ax
    push    %bx
    push    %cx
    push    %dx
    push    %si
    push    %di

    movb    selected_drive, %dl
    call    probe_drive
    jc      dos_install_fail

    mov     $image_filename_dos, %dx
    mov     $0x3D00, %ax
    int     $0x21
    jc      dos_install_file_open_failed
    mov     %ax, file_handle

    xor     %ax, %ax
    mov     %ax, progress_divider
    mov     %ax, current_lba
    mov     %ax, current_lba + 2
    mov     %ax, current_lba + 4
    mov     %ax, current_lba + 6

dos_install_read_loop:
    mov     file_handle, %bx
    mov     $sector_buffer, %dx
    mov     $512, %cx
    mov     $0x3F, %ah
    int     $0x21
    jc      dos_install_read_failed
    test    %ax, %ax
    jz      dos_install_success_close

    cmp     $512, %ax
    je      dos_install_write_sector

    mov     %ax, %cx
    mov     $sector_buffer, %di
    add     %ax, %di
    mov     $512, %bx
    sub     %ax, %bx
    xor     %ax, %ax
    mov     %bx, %cx
    rep stosb

dos_install_write_sector:
    movw    $1, dap_sector_count
    movw    $sector_buffer, dap_buffer_offset
    mov     %ds, %ax
    movw    %ax, dap_buffer_segment

    movw    current_lba, %ax
    movw    %ax, dap_lba
    movw    current_lba + 2, %ax
    movw    %ax, dap_lba + 2
    movw    current_lba + 4, %ax
    movw    %ax, dap_lba + 4
    movw    current_lba + 6, %ax
    movw    %ax, dap_lba + 6

    movb    selected_drive, %dl
    mov     $dap, %si
    call    bios_write_dap_with_retry
    jc      dos_install_write_failed

    incw    current_lba
    jnz     dos_install_progress
    incw    current_lba + 2
    jnz     dos_install_progress
    incw    current_lba + 4
    jnz     dos_install_progress
    incw    current_lba + 6

dos_install_progress:
    incw    progress_divider
    testw   $0x003F, progress_divider
    jnz     dos_install_read_loop
    mov     $'.', %al
    call    print_char
    jmp     dos_install_read_loop

dos_install_success_close:
    mov     file_handle, %bx
    mov     $0x3E, %ah
    int     $0x21
    movw    $0xFFFF, file_handle
    mov     $crlf_text, %si
    call    print_string
    clc
    jmp     dos_install_done

dos_install_file_open_failed:
    mov     $file_missing_text, %si
    call    print_string
    stc
    jmp     dos_install_done

dos_install_read_failed:
    mov     file_handle, %bx
    mov     $0x3E, %ah
    int     $0x21
    movw    $0xFFFF, file_handle
    mov     $file_read_error_text, %si
    call    print_string
    stc
    jmp     dos_install_done

dos_install_write_failed:
    mov     file_handle, %bx
    mov     $0x3E, %ah
    int     $0x21
    movw    $0xFFFF, file_handle
    mov     $disk_write_error_text, %si
    call    print_string
    stc
    jmp     dos_install_done

dos_install_fail:
    stc

dos_install_done:
    pop     %di
    pop     %si
    pop     %dx
    pop     %cx
    pop     %bx
    pop     %ax
    ret

install_bootloader_to_target:
    push    %ax
    push    %bx
    push    %cx
    push    %dx
    push    %si

    movb    selected_drive, %dl
    call    probe_drive
    jc      dos_install_bootloader_write_failed

    mov     $image_filename_dos, %dx
    mov     $0x3D00, %ax
    int     $0x21
    jc      dos_install_bootloader_open_failed
    mov     %ax, file_handle

    xor     %ax, %ax
    mov     %ax, current_lba
    mov     %ax, current_lba + 2
    mov     %ax, current_lba + 4
    mov     %ax, current_lba + 6
    mov     $BOOT_AREA_SECTORS, %ax
    mov     %ax, bootloader_remaining_sectors

dos_install_bootloader_copy_loop:
    mov     bootloader_remaining_sectors, %cx
    test    %cx, %cx
    jz      dos_install_bootloader_success

    mov     file_handle, %bx
    mov     $sector_buffer, %dx
    mov     $512, %cx
    mov     $0x3F, %ah
    int     $0x21
    jc      dos_install_bootloader_read_failed
    cmp     $512, %ax
    jne     dos_install_bootloader_read_failed

    movw    $1, dap_sector_count
    movw    $sector_buffer, dap_buffer_offset
    mov     %ds, %ax
    movw    %ax, dap_buffer_segment

    movw    current_lba, %ax
    movw    %ax, dap_lba
    movw    current_lba + 2, %ax
    movw    %ax, dap_lba + 2
    movw    current_lba + 4, %ax
    movw    %ax, dap_lba + 4
    movw    current_lba + 6, %ax
    movw    %ax, dap_lba + 6

    movb    selected_drive, %dl
    mov     $dap, %si
    call    bios_write_dap_with_retry
    jc      dos_install_bootloader_write_failed

    incw    current_lba
    jnz     dos_install_bootloader_next
    incw    current_lba + 2
    jnz     dos_install_bootloader_next
    incw    current_lba + 4
    jnz     dos_install_bootloader_next
    incw    current_lba + 6
dos_install_bootloader_next:
    decw    bootloader_remaining_sectors
    jmp     dos_install_bootloader_copy_loop

dos_install_bootloader_success:

    mov     file_handle, %bx
    mov     $0x3E, %ah
    int     $0x21
    movw    $0xFFFF, file_handle
    clc
    jmp     dos_install_bootloader_done

dos_install_bootloader_open_failed:
    mov     $file_missing_text, %si
    call    print_string
    stc
    jmp     dos_install_bootloader_done

dos_install_bootloader_read_failed:
    mov     file_handle, %bx
    mov     $0x3E, %ah
    int     $0x21
    movw    $0xFFFF, file_handle
    mov     $file_read_error_text, %si
    call    print_string
    stc
    jmp     dos_install_bootloader_done

dos_install_bootloader_write_failed:
    mov     file_handle, %bx
    mov     $0x3E, %ah
    int     $0x21
    movw    $0xFFFF, file_handle
    mov     $disk_write_error_text, %si
    call    print_string
    stc

dos_install_bootloader_done:
    pop     %si
    pop     %dx
    pop     %cx
    pop     %bx
    pop     %ax
    ret

exit_to_dos:
    mov     $0x4C00, %ax
    int     $0x21

chrome_header_text:
    .asciz  " OS8 Text Setup                                                           \r\n\r\n"
title_text:
    .asciz  "                             OS8 Text Setup                              \r\n"
subtitle_text:
    .asciz  "This installer runs under MS-DOS and applies the OS8 system image\r\nto another BIOS hard disk using a Windows NT-style text setup flow.\r\n\r\n"
menu_text:
    .asciz  "  ENTER = Install OS8 to the selected disk\r\n  C     = Change target disk\r\n  R     = Repair BIOS boot code only\r\n  H     = Help and usage notes\r\n  F3    = Exit Setup\r\n\r\n"
help_text:
    .asciz  "Usage:\r\n  1. Copy OSINST.COM and OSSYS.IMG into the same DOS directory.\r\n  2. Boot MS-DOS, change to that directory, and run OSINST.COM.\r\n  3. Press C to choose the destination BIOS disk.\r\n  4. Press ENTER to install the regular raw OS8 system image.\r\n  5. Press R if you only need to rewrite early BIOS boot code.\r\n\r\nFor safety, target selection tries BIOS disks 0x81-0x87 before 0x80.\r\nExisting data on the selected target disk will be overwritten.\r\n\r\n"
install_intro_text:
    .asciz  "OS8 Setup is preparing to install the system image to the selected disk.\r\nThe destination disk contents will be overwritten sector-by-sector.\r\n\r\n"
bootloader_intro_text:
    .asciz  "OS8 Setup can also repair only the BIOS boot area from OSSYS.IMG.\r\nUse this when the installed system files are intact but the disk\r\nis no longer bootable.\r\n\r\n"
confirm_text:
    .asciz  "Press ENTER to continue the installation, or F3/ESC to cancel.\r\n"
bootloader_confirm_text:
    .asciz  "Press ENTER to rewrite BIOS boot code, or F3/ESC to cancel.\r\n"
copy_begin_text:
    .asciz  "Setup is now copying files and disk sectors to your OS8 target.\r\nProgress: "
bootloader_begin_text:
    .asciz  "Setup is repairing BIOS boot code on the selected target...\r\n"
copy_success_text:
    .asciz  "OS8 Setup has finished writing the target disk.\r\nYou can now restart and boot the selected disk into OS8.\r\n\r\n"
bootloader_success_text:
    .asciz  "BIOS boot code was repaired successfully.\r\n\r\n"
copy_failed_text:
    .asciz  "\r\nOS8 Setup could not finish writing the system image.\r\n\r\n"
bootloader_failed_text:
    .asciz  "\r\nOS8 Setup could not repair BIOS boot code.\r\n\r\n"
no_target_text:
    .asciz  "No alternate BIOS target disk is available for installation.\r\n\r\n"
target_updated_text:
    .asciz  "The selected installation disk has been updated.\r\n\r\n"
target_missing_text:
    .asciz  "No alternate BIOS disk could be selected.\r\n\r\n"
file_missing_text:
    .asciz  "\r\nOSSYS.IMG was not found in the current DOS directory.\r\n\r\n"
file_read_error_text:
    .asciz  "\r\nDOS file read failed while reading OSSYS.IMG.\r\n\r\n"
disk_write_error_text:
    .asciz  "\r\nBIOS disk write failed on the selected target.\r\n\r\n"
press_key_text:
    .asciz  "Press any key to continue...\r\n"
image_file_text:
    .asciz  "Image file:    "
image_filename_text:
    .asciz  IMAGE_FILENAME
target_drive_text:
    .asciz  "Target drive:  "
none_text:
    .asciz  "none\r\n"
crlf_text:
    .asciz  "\r\n"
footer_text:
    .asciz  "                                                                      F3=Exit\r\n"
image_filename_dos:
    .asciz  IMAGE_FILENAME

selected_drive:
    .byte   0
    .byte   0
file_handle:
    .word   0xFFFF
progress_divider:
    .word   0
current_lba:
    .quad   0
bootloader_remaining_sectors:
    .word   0

dap:
    .byte   0x10
    .byte   0
dap_sector_count:
    .word   1
dap_buffer_offset:
    .word   sector_buffer
dap_buffer_segment:
    .word   0
dap_lba:
    .quad   0

sector_buffer:
    .space  512

stack_space:
    .space  1024
stack_top:
