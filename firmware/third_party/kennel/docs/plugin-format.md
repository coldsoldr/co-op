# kennel plugin file format (version 1)

<!-- Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). -->

One file holds one synth engine or effect (later: one feature): its code and data linked from address 0, the
relocations that depend on where the loader puts it, the core symbols it calls, its screen SVG and its defaults.
`tools/kennel/pack.py` writes it from a plugin folder; `tools/kennel/reloc.py` is the reference loader (the
checks and the relocation the C loader must do, in the same order). `tools/kennel/test_plugin_format.py` packs
every plugin in `synth_sdk/plugins`, relocates it to two addresses and compares the bytes with a plain
`bfin-elf-ld` link at those addresses.

File name: `<id>.knl` (see "Names"). The loader reads `/yaffs2/plugins` and `/yaffs2/content` (`synth_sdk/kennel/kn_boot.c`;
it also reads op1r 34's `.op1p` files). The C loader is `synth_sdk/kennel/kn_load.c`, tested against
`reloc.py` by `tools/kennel/test_loader.py`.

## Names

One rule for every plugin; nothing else identifies one, and nothing matches a second name.

1. **The id**: `[a-z0-9_-]`, 1..15 characters. It is the plugin's folder (`synth_sdk/plugins/<id>`), its file
   (`<id>.knl`), the `plugin.json` `"name"`, the entry in `kennel.off`, the name in the inventory and the name a
   feature registers (`op1_feature_t.name`).
2. **The saved name**: the header's `type_name`, what the OP-1 shows and what saved sounds and kits store. It is the id
   with `_` turned into a space (`in_a_room` -> "in a room"). The packer derives it; it is never written by hand.
3. **Frozen exceptions**: names already in users' saved files that rule 2 cannot give. Only `drumplus` -> "drum+"
   (drum patch JSON `"engine": "drum+"`, the `drum_types` row). `plugin.json` may carry `"saved_name"` only for an id
   on this list (`pack.SAVED_NAMES`; `test_plugin_format.py` `test_names` holds the list fixed).

`pack.py` (`check_id`, also called by `plugins.py` for `make_firmware`) refuses an id or folder that breaks rule 1, a
folder that differs from its id, a file not named `<id>.knl` and a `"saved_name"` outside the list. The loader
matches `kennel.off` by the file's id (its name without `.knl` / `.op1p`), also for a file it cannot read, and lists
every file under its id. Engines and effects are numbered and picked by their saved name (`docs/preset-identity.md`).

## Rules
- Little-endian. Every block starts on a 4-byte boundary in the file; the file size is a multiple of 4.
- No PIC. The code is linked for address 0 and relocated at load. Absolute firmware addresses in the code
  (`0x01DF6F78`, `0xFF801950`, ...) are plain immediates and have no relocation.
- The loader skips a file that fails any check below, prints why, and boots on. A broken file never halts.

## SVG in front
A plugin with a screen starts with its SVG text, a NUL and zero padding to a 4-byte boundary; the header starts
there (`header_offset`). The stock SVG parser stops at the NUL, so the loader registers the plugin file itself as the
screen's SVG. Every other file offset in the header counts from the start of the file; the CRC is taken over the
whole file with the 4 CRC bytes (header + 12) as zero. Without a screen the header is at 0.

## Header (144 bytes)

| Off | Type | Field | Meaning |
|---|---|---|---|
| 0 | char[4] | `magic` | `KNPL` |
| 4 | u16 | `format_version` | 1. A loader skips any other value. |
| 6 | u16 | `header_size` | 144 for now. Later version-1 files may append fields; a loader reads the first 144 bytes and ignores the rest. |
| 8 | u32 | `file_size` | whole file in bytes; must equal the size read |
| 12 | u32 | `crc32` | CRC-32 (zlib/IEEE, poly 0xEDB88320, init and final XOR 0xFFFFFFFF) over bytes 0..`file_size`-1 with these 4 bytes taken as zero |
| 16 | u16 | `api_version` | `OP1_PLUGIN_API` of `synth_sdk/op1_plugin.h` the plugin was built with; must equal the core's |
| 18 | u8 | `kind` | 1 synth engine (`op1_engine_t`), 2 effect (`op1_fx_t`), 3 feature (`op1_feature_t`, `synth_sdk/op1_feature.h`; the loader also checks its `api` against `OP1_FEATURE_API`) |
| 19 | u8 | `legacy_order` | `plugin.json` `order`: old sounds select the engine by `knobs[4] = 0x5300 + order` (`synth_sdk/plugins/ORDERS.md`). 0 = none (new plugins without an old selector). 1..31. |
| 20 | char[16] | `type_name` | the saved name ("Names": derived from the id): saved sounds pick the engine by this name (`docs/preset-identity.md`). ASCII, 1..15 characters, NUL-padded. |
| 36 | u32 | `flags` | bit 0 `uses_sample` (the slot's `synth_<slot>.raw` is loaded at boot), bit 1 `page2_defaults` valid, bit 2 `stereo` FX entry; unknown bits refused |
| 40 | i16[4] | `defaults` | initial knob positions 0..32767 (blue, green, white, red) |
| 48 | i16[4] | `page2_defaults` | initial page-2 words 5..7 (FX `params[5..7]`), fourth entry 0; only with flag bit 1 |
| 56 | u32 | `text_size` | `.text` bytes, at image offset 0 |
| 60 | u32 | `rodata_offset` | image offset of `.rodata` |
| 64 | u32 | `rodata_size` | |
| 68 | u32 | `data_offset` | image offset of `.data` |
| 72 | u32 | `data_size` | |
| 76 | u32 | `bss_offset` | offset of `.bss` from the load address (>= `image_size`) |
| 80 | u32 | `bss_size` | bytes the loader zeroes after the image |
| 84 | u32 | `image_size` | bytes of image in the file (`data_offset + data_size`) |
| 88 | u32 | `image_align` | the load address must be a multiple of this (power of two, >= 4) |
| 92 | u32 | `entry_offset` | offset of the `op1_engine_t` / `op1_fx_t` object (or `op1_fx_stereo_t` with flag bit 2) from the load address |
| 96 | u32 | `screen_offset` | offset of the `op1_screen_t` object, or 0xFFFFFFFF |
| 100 | u32 | `image_file_offset` | file offset of the image |
| 104 | u32 | `reloc_offset` | file offset of the relocation table |
| 108 | u32 | `reloc_count` | entries (12 bytes each) |
| 112 | u32 | `import_offset` | file offset of the import table |
| 116 | u32 | `import_count` | entries (4 bytes each) |
| 120 | u32 | `strings_offset` | file offset of the string table |
| 124 | u32 | `strings_size` | bytes; the table starts with a NUL and ends with one |
| 128 | u32 | `svg_offset` | reserved, 0 (the SVG is always in front) |
| 132 | u32 | `svg_size` | 0 = no screen SVG; else its text length + the NUL (the file starts with it) |
| 136 | u32 | `svg_name` | reserved, 0xFFFFFFFF |
| 140 | u8[4] | reserved | 0 |

The packer writes the blocks after the header in this order: image, relocations, imports, strings, each
padded with zeros to 4 bytes. A loader must not rely on the order; it uses the offsets.

## Memory layout at load

```
base + 0                 .text
base + rodata_offset     .rodata
base + data_offset       .data
base + bss_offset        .bss (bss_size bytes, zeroed)
base + bss_offset + bss_size   end: the loader allocates this much
```

Each section starts at the next multiple of max(4, its ELF alignment). The image in the file covers
`0 .. image_size` (text, rodata, data and the padding between them).

## Relocation table

Entries sorted by `offset`, 12 bytes each:

| Off | Type | Field |
|---|---|---|
| 0 | u32 | `offset`: image offset of the field (see the types) |
| 4 | u8 | `type` |
| 5 | u8 | reserved, 0 |
| 6 | u16 | `sym`: 0xFFFF = local (target inside the plugin), else index into the import table |
| 8 | i32 | `addend` |

Target value `V`: local: `base + addend` (the addend is an offset from the load address, 0..end); import:
`import_address[sym] + addend`. All arithmetic is mod 2^32. The loader writes the whole field; the bytes the
file holds there do not matter (the packer leaves the value for base 0).

| type | Name | Field | Write |
|---|---|---|---|
| 1 | `ABS32` | u32 at `offset` | `V` |
| 2 | `HI16` | u16 at `offset` (the immediate half of a `reg.H = imm16` instruction) | `V >> 16` |
| 3 | `LO16` | u16 at `offset` (`reg.L = imm16`) | `V & 0xFFFF` |
| 4 | `PCREL24` | 32-bit `CALL` / `JUMP.L` starting at `offset - 2` | `d = V - (base + offset - 2)`; d must be even and in -2^24 .. 2^24-2, else the plugin is skipped. `d >>= 1` (24 bits): halfword at `offset - 2` keeps its high byte and gets `d >> 16` in the low byte; halfword at `offset` = `d & 0xFFFF`. |

`HI16` and `LO16` are independent entries, each with the full target: no pairing, no carry adjustment (Blackfin
loads the halves with separate instructions). `PCREL24` appears only with an import; calls inside the plugin are
resolved by the packer because they move with it. A local `PCREL24` is an error.

Mapping from the ELF types (binutils `include/elf/bfin.h`) the packer accepts; anything else stops the packer:

| ELF | Number | File type |
|---|---|---|
| `R_BFIN_BYTE4_DATA` | 0x12 | `ABS32` (local or import) |
| `R_BFIN_HUIMM16` | 0x07 | `HI16` |
| `R_BFIN_LUIMM16` | 0x06 | `LO16` |
| `R_BFIN_PCREL24` | 0x0A | resolved by the packer when local, `PCREL24` for an import |
| `R_BFIN_PCREL24_JUMP_L` | 0x0D | as `R_BFIN_PCREL24` |

ELF `r_offset` of the two PCREL24 types points at the second halfword of the instruction, as `offset` does here.
A relocation against an absolute symbol is resolved by the packer (except PC-relative: error).

## Imports

`import_count` entries of one u32 each: the string-table offset of a symbol name the plugin needs from the core.
Names are stored as the ELF symbol names, with the C leading underscore (`_op1_fx_page`, `_read_fx_param_safe`,
`_sq_text`). The core keeps a table of the symbols it exports; a name it lacks makes the loader skip the plugin.
libgcc is linked into each plugin (`__divsi3`, `__muldi3`, ...), so the core does not have to export it.

## Optional stereo FX entry

`plugin.json` may set `"stereo": true` for an FX. The entry is `op1_fx_stereo_t`: the existing
12-byte Blackfin `op1_fx_t` prefix followed by a 4-byte `render_stereo(pos, left[64], right[64], params)`
callback. The prefix remains the synth/drum implementation; the stereo callback processes both master
channels directly at position 2. All state remains per position. The packer checks the entry symbol's
size, and the loader requires all 16 bytes inside the initialized image. After relocation its init,
mono render and stereo render must be non-null, halfword-aligned addresses with at least one instruction
inside this plugin's `.text`. A truncated entry, non-FX flag or invalid callback skips the file.

Existing mono `.knl` and `.op1p` objects keep their original layout and master mono-sum routing. A separate
core callback table holds only validated stereo callbacks; registering a mono replacement clears that slot.
Generated built-in tables initialize this callback table before file registration. No new legacy order or
Maiko selector is implied by the flag: an unordered loadable plugin keeps name-based identity at 32..63.
The optional entry does not change the file/header version or existing mono ABI.

## Loader steps (reloc.py `parse` then `relocate`)
1. Read the whole file. Check `magic`, `format_version`, `header_size`, `file_size`, `crc32`, `api_version`,
   `kind`, `legacy_order` <= 31, a NUL-terminated non-empty `type_name`.
2. Check every block lies inside the file and is 4-aligned (SVG and strings: any alignment), the section
   layout is in order (`text_size <= rodata_offset <= rodata end <= data_offset <= data end == image_size <=
   bss_offset`), `image_align` is a power of two >= 4, every string ends inside the table.
3. Check every relocation: known type, reserved byte 0, field inside the image and halfword-aligned, a local
   target inside 0..end, an import index below `import_count`, no local `PCREL24`. Check `entry_offset`
   (required) and `screen_offset` (or 0xFFFFFFFF) lie inside 0..end and are 4-aligned.
4. Look up every import in the core's export table.
5. Allocate end bytes at a base aligned to `image_align`; copy the image, zero `.bss`.
6. Apply the relocations; a `PCREL24` out of reach fails the plugin (free the block, skip the file).
   `kn_validate` checks complete initialized descriptors, nested feature descriptors, bounded strings/claims,
   and required/optional callbacks before registration. Callbacks must be halfword-aligned within this
   plugin's `.text`; wrap a core service in a local callback instead of storing an imported address directly.
   OP1P has no text boundary, so its callback range is the initialized image. Synth/FX/screen descriptors
   require16/12/8 bytes (stereo FX16); feature API1 requires84. Claims extensions must end in an empty string.
7. Flush the data cache and invalidate the instruction cache over the block before calling into it.
8. Register: `base + entry_offset` into the engine/FX table, the screen object and SVG, the type name and the
   legacy selector `0x5300 + legacy_order` (when non-zero).

## Constraints the C loader must keep
- **Call reach.** `PCREL24` reaches +-16 MB. The core's code (the exports) starts at 0x01E00000; the planned
  plugin block 0x02980000..0x03000000 is 11.5..18 MB away, so a plugin above about 0x02DFFFFF cannot call the
  start of the core. Allocate from the bottom of the block, or have the core
  export a call target that is near (a veneer), or build plugins with `-mlong-calls` (then calls into the core
  become HI16/LO16 pairs and the packer needs no change). Today's plugins call the core only for
  `read_fx_param_safe` and the `sq_*` text helpers.
- **libgcc.** Linked into each plugin (a few hundred bytes for the division helpers). Its `.eh_frame` is dropped
  by `merge.ld`.
- **Strings.** `ld -r` does not merge duplicate strings across `.rodata.str*`, a final link does. The file
  carries the `ld -r` layout, so the test compares against a final link of the `ld -r` object.
- **Alignment.** All current plugins have `image_align` 4.

## Packer (`tools/kennel/pack.py`)
1. Compile each `plugin.json` source with `bfin-elf-gcc -O2 -mcpu=bf524 -ffreestanding -fno-builtin -nostdlib
   -fno-common -Wall -Isynth_sdk` (as `make_firmware.py`).
2. `bfin-elf-ld -r -T tools/kennel/merge.ld *.o libgcc.a` -> one object with `.text`, `.rodata`, `.data`,
   `.bss`. Any other allocated section stops the packer.
3. Lay the sections out from 0, apply every relocation, keep the address-dependent ones in the table.
4. Fill the header from `plugin.json` (`name`, `kind`, `order`, `symbol`, `screen`, `defaults`,
   `page2_defaults`, `uses_sample`) and `OP1_PLUGIN_API` from `op1_plugin.h`; append the SVG; write the CRC.

## Continuous input synth extension (3.0.1)

`input_engine: true` sets KNPL flag8, valid only for a nonsampler synth. The entry is the 32-byte `op1_input_engine_t` in `op1_input.h`: unchanged16-byte voice prefix, mandatory audio reset/process, optional main-loop service and optional checked T2 screen descriptor. The loader validates the complete extension and all callbacks before registration. Old cores reject unknown flag8. Header API1 and ordinary synth/FX ABI stay unchanged. The packer requires the full entry size; use the SDK matching the new core. The new input callback replaces the64-sample post-decimator bus once/block, before T3. Parameters are a raw saved-word snapshot; voice-based T4 modulation is not applied. Audio input and output must not alias; no allocation, filesystem or unbounded service in audio. `reset(-1, rate)` deactivates; service/Gfx must use epochs/owned buffers, not race reset state. Rate is selected from observed400MHz callback cadence (22050 or44100); confirm physical hardware timing before claiming tuner calibration.
