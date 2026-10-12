ENTRY(Reset_Handler)
MEMORY {
  FLASH (rx) : ORIGIN = 0, LENGTH = 4M
  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}
SECTIONS {
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) } > FLASH
  .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > FLASH
  .data : { _sdata = .; *(.data*) _edata = .; } > RAM AT> FLASH
  _sidata = LOADADDR(.data);
  .bss : { _sbss = .; *(.bss*) *(COMMON) _ebss = .; } > RAM
  . = ALIGN(8);
  end = .;
  _end = .;
}
