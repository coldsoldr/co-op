/* Links the L1 dumper into the OP-1 app's free SDRAM region A (docs/memory-map.md).
 * tools/inject_app.py turns the PT_LOAD segments into extra LDR blocks and .bss into a FILL
 * block. Modelled on te-op1-cfw/ports/rings/op1/link.ld. */

OUTPUT_FORMAT("elf32-bfin")
ENTRY(_op1_header)

MEMORY
{
  injected (rwx) : ORIGIN = 0x01e00000, LENGTH = 0x00200000
}

SECTIONS
{
  .header : { KEEP(*(.op1header)) } > injected
  .text   : { *(.text .text.*) } > injected
  .rodata : { *(.rodata .rodata.*) } > injected
  .data   : { *(.data .data.*) } > injected
  .bss (NOLOAD) : ALIGN(32)
  {
    __bss_start = .;
    *(.bss .bss.* COMMON)
    . = ALIGN(4);
    __bss_end = .;
  } > injected
  ___image_end = .;

  /* No crt0: nothing may need run-time initialisation. */
  /DISCARD/ : { *(.init_array*) *(.ctors*) *(.comment) *(.eh_frame*) *(.fini_array*) *(.dtors*) }
}
