/* kennel plugin packer: `bfin-elf-ld -r -T merge.ld` merges a plugin's objects (and the libgcc members they
 * need) into one .text, .rodata, .data and .bss; libgcc's .eh_frame is dropped (no unwinding). Anything
 * else that is allocated ends up as its own section and pack.py refuses it.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
SECTIONS
{
  .text   : { *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .data   : { *(.data .data.*) }
  .bss    : { *(.bss .bss.*) *(.fxbuf .fxbuf.*) }   /* .fxbuf: hundekraft FX delay lines (NOLOAD in the core build) */
  /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame) }
}
