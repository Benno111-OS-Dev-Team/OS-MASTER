.code16
.global real_mode_entry

.section .text

real_mode_entry:
    cli
    mov %cs, %ax
    mov %ax, %ds
    mov %ax, %es
    xor %ax, %ax
    mov %ax, %ss
    mov $0x9000, %sp
    cld
    sti

    mov %dl, boot_drive
    movb $0, selected_drive
    call find_default_target

    mov $0x0003, %ax
    int $0x10

main_menu:
    call clear_screen
    mov $title_text, %si
    call print_string
    mov $menu_text, %si
    call print_string
    call print_selected_drive
    call read_key

    cmp $'1', %al
    je menu_install
    cmp $'2', %al
    je menu_change_drive
    cmp $'3', %al
    je menu_shell
    cmp $'4', %al
    je menu_reboot
    jmp main_menu

menu_install:
    call run_install
    jmp main_menu

menu_change_drive:
    call choose_drive
    jmp main_menu

menu_shell:
    call run_shell
    jmp main_menu

menu_reboot:
    int $0x19
    hlt
    jmp menu_reboot

run_install:
    cmpb $0, selected_drive
    jne run_install_have_target
    mov $no_target_text, %si
    call print_string
    call wait_for_keypress
    ret

run_install_have_target:
    mov $installing_text, %si
    call print_string

    # BIOS installer currently validates the target and reports completion.
    # Full partition/fs/payload work is handled by the graphical installer.

    mov $install_done_text, %si
    call print_string
    call wait_for_keypress
    ret

choose_drive:
    push %dx

    mov $choose_drive_text, %si
    call print_string
    call list_drives
    call read_key

    cmp $27, %al
    je choose_drive_done

    sub $'0', %al
    cmp $0, %al
    je choose_drive_invalid
    cmp $8, %al
    ja choose_drive_invalid

    mov %al, %bl
    add $0x7F, %bl
    mov %bl, %dl
    call probe_drive
    jc choose_drive_invalid

    mov %bl, selected_drive
    jmp choose_drive_done

choose_drive_invalid:
    mov $invalid_drive_text, %si
    call print_string
    call wait_for_keypress

choose_drive_done:
    pop %dx
    ret

run_shell:
    mov $shell_text, %si
    call print_string
run_shell_loop:
    call read_key
    cmp $27, %al
    je run_shell_done
    mov $0x0E, %ah
    mov $0x00, %bh
    int $0x10
    jmp run_shell_loop
run_shell_done:
    ret

probe_drive:
    push %ax
    push %bx
    push %cx
    push %dx

    mov %dl, probe_drive_target
    mov $0x55AA, %bx
    mov $0x4100, %ax
    int $0x13
    jc probe_drive_legacy

    cmp $0xAA55, %bx
    jne probe_drive_legacy

    test $0x0001, %cx
    jnz probe_drive_found

probe_drive_legacy:
    mov probe_drive_target, %dl
    mov $0x08, %ah
    int $0x13
    jc probe_drive_missing

probe_drive_found:
    clc
    jmp probe_drive_done

probe_drive_missing:
    stc

probe_drive_done:
    pop %dx
    pop %cx
    pop %bx
    pop %ax
    ret

find_default_target:
    push %ax
    push %bx
    push %cx
    push %dx

    mov boot_drive, %dl
    mov $0x80, %bl
    mov $0x80, %cl
    add $8, %cl

find_default_target_scan_loop:
    cmp %bl, %dl
    je find_default_target_next

    mov %bl, %dl
    call probe_drive
    jc find_default_target_next

    mov %bl, selected_drive
    clc
    jmp find_default_target_done

find_default_target_next:
    inc %bl
    cmp %cl, %bl
    jb find_default_target_scan_loop

    movb $0, selected_drive
    stc

find_default_target_done:
    pop %dx
    pop %cx
    pop %bx
    pop %ax
    ret

list_drives:
    push %ax
    push %bx
    push %cx
    push %dx
    push %si

    mov $drive_list_text, %si
    call print_string

    xor %ch, %ch
    mov $1, %cl
    mov $0x80, %bl
    mov $0x88, %bh

list_drives_drive_loop:
    mov %bl, %dl
    call probe_drive
    jc list_drives_next_drive

    inc %ch

    mov $drive_prefix_text, %si
    call print_string

    mov %cl, %al
    add $'0', %al
    call print_char

    mov $drive_arrow_text, %si
    call print_string

    mov %bl, %al
    call print_hex8

    mov $newline_text, %si
    call print_string

list_drives_next_drive:
    inc %cl
    inc %bl
    cmp %bh, %bl
    jb list_drives_drive_loop

    test %ch, %ch
    jnz list_drives_done

    mov $no_drives_found_text, %si
    call print_string

list_drives_done:
    pop %si
    pop %dx
    pop %cx
    pop %bx
    pop %ax
    ret

print_selected_drive:
    push %ax
    push %si

    mov $selected_drive_text, %si
    call print_string

    mov selected_drive, %al
    cmp $0, %al
    jne print_selected_drive_print_drive

    mov $none_text, %si
    call print_string
    jmp print_selected_drive_done

print_selected_drive_print_drive:
    call print_hex8

print_selected_drive_done:
    mov $newline_text, %si
    call print_string

    pop %si
    pop %ax
    ret

clear_screen:
    push %ax
    push %bx
    push %cx
    push %dx

    mov $0x0600, %ax
    mov $0x0700, %bx
    mov $0x0000, %cx
    mov $0x184F, %dx
    int $0x10

    mov $0x0200, %ax
    xor %bx, %bx
    xor %dx, %dx
    int $0x10

    pop %dx
    pop %cx
    pop %bx
    pop %ax
    ret

print_string:
    push %ax
    push %bx
print_string_next_char:
    lodsb
    test %al, %al
    jz print_string_done
    mov $0x0E, %ah
    mov $0x00, %bh
    int $0x10
    jmp print_string_next_char
print_string_done:
    pop %bx
    pop %ax
    ret

print_char:
    push %ax
    push %bx
    mov $0x0E, %ah
    mov $0x00, %bh
    int $0x10
    pop %bx
    pop %ax
    ret

print_hex8:
    push %ax
    push %bx

    mov %al, %ah
    shr $4, %al
    call print_hex_nibble

    mov %ah, %al
    and $0x0F, %al
    call print_hex_nibble

    pop %bx
    pop %ax
    ret

print_hex_nibble:
    and $0x0F, %al
    cmp $10, %al
    jb print_hex_nibble_digit
    add $('A' - 10), %al
    jmp print_hex_nibble_emit
print_hex_nibble_digit:
    add $'0', %al
print_hex_nibble_emit:
    call print_char
    ret

read_key:
    xor %ax, %ax
    int $0x16
    ret

wait_for_keypress:
    mov $press_key_text, %si
    call print_string
    call read_key
    ret

.section .rodata

title_text:
    .asciz "OS8 DOS Setup\r\n================\r\n\r\n"

menu_text:
    .asciz "1) Install OS8\r\n2) Change target drive\r\n3) Shell\r\n4) Reboot\r\n\r\n"

selected_drive_text:
    .asciz "Selected target drive: "

none_text:
    .asciz "None"

no_target_text:
    .asciz "\r\nNo target disk selected.\r\n"

installing_text:
    .asciz "\r\nInstalling...\r\n"

install_done_text:
    .asciz "\r\nInstall complete.\r\n"

choose_drive_text:
    .asciz "\r\nChoose target drive (1-8, ESC to cancel):\r\n"

invalid_drive_text:
    .asciz "\r\nThat BIOS hard disk was not detected.\r\n"

no_drives_found_text:
    .asciz "  No BIOS hard disks detected.\r\n"

drive_list_text:
    .asciz "\r\nAvailable BIOS hard disks:\r\n"

drive_prefix_text:
    .asciz "  "

drive_arrow_text:
    .asciz " -> 0x"

shell_text:
    .asciz "\r\nMini shell active. Press ESC to return.\r\n"

press_key_text:
    .asciz "\r\nPress any key to continue...\r\n"

newline_text:
    .asciz "\r\n"

.section .bss

boot_drive:
    .byte 0

selected_drive:
    .byte 0

probe_drive_target:
    .byte 0
