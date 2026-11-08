/* Phase 2 milestone 1: the coop audio-callback wrapper, linked into free SDRAM region A.
 * inject_app.py adds the load segments as LDR blocks and patches the audio-callback imm pair. */
OUTPUT_FORMAT("elf32-bfin")
ENTRY(_op1_header)
MEMORY { injected (rwx) : ORIGIN = 0x02c00000, LENGTH = 0x00400000
         near (rwx)     : ORIGIN = 0x01e00400, LENGTH = 0x00000c00 }   /* region C window in pcrel24 reach of the stock (inject_app.py NEAR_*) */
SECTIONS {
  .header : { KEEP(*(.op1header)) } > injected
  .text   : { *(.text .text.*) } > injected
  .near   : { KEEP(*(.near)) } > near
  .rodata : { *(.rodata .rodata.*) } > injected
  .data   : { *(.data .data.*) } > injected
  .bss (NOLOAD) : ALIGN(32) { __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(4); __bss_end = .; } > injected
  ___image_end = .;
  /DISCARD/ : { *(.init_array*) *(.ctors*) *(.comment) *(.eh_frame*) *(.fini_array*) *(.dtors*) }
}
