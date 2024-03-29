ENTRY(vector_table)

MEMORY
{
  FLASH (rx)      : ORIGIN = FLASH_BASE, LENGTH = FLASH_LEN
#if MCU == STM32F1
  RAM (rwx)       : ORIGIN = 0x20000000, LENGTH = 20K
#elif MCU == STM32F7
  RAM (rwx)       : ORIGIN = 0x20000000, LENGTH = 64K
  EXT_RAM (rwx)   : ORIGIN = 0x20010000, LENGTH = 64K /* 192K - U_BUF_SZ */
#elif MCU == AT32F4
  RAM (rwx)       : ORIGIN = 0x20000000, LENGTH = 32K
#endif
}

SECTIONS
{
  .text : {
    _stext = .;
    *(.vector_table)
    *(.text)
    *(.text*)
    *(.rodata)
    *(.rodata*)
    KEEP (*(.init))
    KEEP (*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

#if MCU == STM32F1 || MCU == AT32F4
  .flags : {
    _reset_flag = .;
    . = . + 4;
  } >RAM
#endif

  .data : AT (_etext) {
    . = ALIGN(4);
    _sdat = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edat = .;
    _ldat = LOADADDR(.data);
  } >RAM

  .bss : {
    . = ALIGN(8);
    _irq_stackbottom = .;
    . = . + 512;
    _irq_stacktop = .;
    _thread_stackbottom = .;
    . = . + 1024;
    _thread_stacktop = .;
    _sbss = .;
    *(.bss)
    *(.bss*)
    . = ALIGN(4);
    _ebss = .;
#if MCU == STM32F1 || MCU == AT32F4
    u_buf = .;
#endif
  } >RAM

#if MCU == STM32F7
  .ext_ram (NOLOAD) : {
    _ext_ram_start = .;
    _reset_flag = .;
    . = . + 4;
    *(.ext_ram)
    . = ALIGN(4);
    _ext_ram_end = .;
    u_buf = .; /* 128K */
  } >EXT_RAM
#endif

  /DISCARD/ : {
    *(.eh_frame)
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
