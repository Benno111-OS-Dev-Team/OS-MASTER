ENTRY(EfiMain)
SECTIONS {
  . = 0;
  .text : { *(.text*) }
  .data : { *(.data*) *(.rodata*) }
  .bss : { *(.bss*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
