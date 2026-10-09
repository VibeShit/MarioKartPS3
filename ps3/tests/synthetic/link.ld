ENTRY(_start)
SECTIONS {
  . = 0x80004000;
  .text : { *(.text._start) *(.text*) }
  . = ALIGN(32);
  .rodata : { *(.rodata*) }
  . = ALIGN(32);
  .data : { *(.data*) *(.sdata*) *(.sdata2*) }
  . = ALIGN(32);
  .bss : { *(.bss*) *(.sbss*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) *(.gnu*) }
}
