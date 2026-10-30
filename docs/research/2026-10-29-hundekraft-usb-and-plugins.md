# hundekraft over USB MIDI, and plugin file formats

Facts gathered on 2026-10-29 from the owner's hundekraft clone in `vendor/hundekraft`
(GPL-3.0): the workshop page (`op1-workshop-main/op1push.js`) and the 3.0.3 firmware
source in `op1-workshop-main/source/hundekraft-3.0.3-source.tar.gz`
(`ext/kennel/docs/inventory.md`, `ext/kennel/docs/plugin-format.md`,
`ext/kennel/synth_sdk/devupd/du.h`). We record the protocol and file layouts here and
write our own implementation from them; no code is copied.

Nothing here has been tried against a real OP-1 by co-op yet.

## The USB connection

- In normal (non disk mode) use the OP-1 is a class-compliant USB MIDI device whose
  port names contain "OP-1". hundekraft 3.0 and later answer a SysEx protocol on it.
  The browser needs Web MIDI with `sysex: true` (Chrome, Edge, Firefox; the user is
  asked for permission).
- Every message, both ways: `F0 00 20 76 7D <cmd> <seq lo> <seq hi> <payload> F7`.
  `seq` is 14 bits sent as two 7-bit bytes. The payload is 7-bit packed: each group of
  up to 7 raw bytes is sent as one byte holding their high bits (bit k = high bit of
  byte k) followed by the 7 bytes with their high bit cleared.
- A reply has `cmd | 0x40` and the same seq; its unpacked payload starts with a status
  byte. Statuses: 0 ok, 1 format, 2 state, 3 path, 4 size, 5 offset, 6 crc, 7 io,
  8 full, 9 wait key.
- **At most 96 bytes per message, one message in flight.** The stock USB MIDI input
  takes at most 32 USB MIDI packets per transfer; a longer SysEx stops all MIDI input
  until the OP-1 restarts. So DATA carries at most 72 raw bytes.
- MIDI realtime bytes (0xF8 and up, e.g. clock) can arrive inside a SysEx and must be
  skipped when reassembling.
- A host resends with a new seq after a timeout (the workshop uses 3 s for reads,
  10 s for control steps, a few tries).
- Older hundekraft (2.7 and before), op1r and TE's firmware show the MIDI port but do
  not answer HELLO.

### Commands

| cmd | name | request payload | reply after status |
| --- | --- | --- | --- |
| 1 | HELLO | none | u32 protocol version; then a capabilities byte: bit 0 inventory, bit 1 plugin files. Builds without it send only the u32. |
| 2 | BEGIN | u32 size, u32 CRC-32, path, NUL | |
| 3 | DATA | u32 offset, up to 72 bytes | u32 bytes written; status 5 means a gap, resend from that count |
| 4 | END | none | |
| 5 | COMMIT | none | 9 (wait key) until the user presses T1 on the OP-1 prompt, then 0 and the OP-1 restarts and applies; 2 after T2 (cancelled) |
| 6 | ABORT | none | clears the staging area |
| 7 | LIST | u32 offset | u32 total length, up to 64 bytes of inventory text; offset past the end gives status 5 |
| 8 | OFF_GET | u8 which (0 stored, 1 in effect since boot), u16 offset | u32 total, up to 64 bytes of the switched-off list |
| 9 | OFF_SET | u16 offset, u16 total, up to 64 bytes | u16 bytes received |
| 10 | DELETE | path, NUL | staged removal of a plugin file |
| 11 | RESTART | none | like COMMIT when nothing but deletes or the off list changed |

All integers little-endian. CRC-32 is zlib's (poly 0xEDB88320, init and final XOR
0xFFFFFFFF).

- BEGIN paths allowed by the device: `OP1_vdk.ldr`, `content/<file>`, and
  `plugins/<file>.knl|.op1p` (one level, no leading dot; `plugins/.boot` is the crash
  marker). Anything with `te-boot`, `..`, `//`, a leading or trailing `/`, or a character
  outside `[A-Za-z0-9_./-]` is refused, as is a path of 96 bytes or more.
- **User sounds are not reachable.** Saved sounds live in `/yaffs2/user/audio/...`, which
  is not on the allow-list, and no command reads a file back. Patches still need disk mode.
- Files are staged in `/yaffs2/op1r_stage` and only installed after T1 at the restart.
  A broken plugin never halts the boot: the loader skips it. If a boot does not reach
  the UI, the next boot starts without plugin files (crash marker).
- Replacing a plugin file with a same-name file in one go can hang the restart on the
  "kennel: updated" screen (te-op1-cfw README); the safe path is remove, restart, add.
- A path sent and deleted in the same session is refused (state).

### Inventory text (LIST)

Built once per boot. Tab-separated, first line a header whose fields are found by their
first word:

    kennel-inventory 1<TAB>core <start hex8> <len hex8> <crc hex8><TAB>presets <count dec5> cap <cap dec5>

then one line per plugin or built-in:

    kind  name  source  path  version  author  size  crc  state  claims  reason

- kind: `synth`, `fx`, `feature`, or `-` (did not parse).
- name: the plugin id; the saved name in sounds is the id with `_` as a space
  (exception: `drumplus` is "drum+").
- source: `builtin`, `knl`, `op1p`. path: `plugins/<file>` or `content/<file>`, empty
  for built-ins.
- state: `loaded`, `failed`, `skipped` (crash marker), `off`. reason explains failed/off.
- claims: comma list, each prefixed `+` held, `!` lost, `~` not held.
- `presets` (newer cores): R246's count of user synth presets and the cap of its save
  gate (100 on stock R246, 300 on a 300 build).

## kennel plugin file (`.knl`, format 1)

- Optional screen SVG text first, then a NUL, padded with zeros to 4 bytes; the header
  starts there. A file starting with `<` has an SVG.
- Header (144 bytes, little-endian, offsets from the header start): 0 magic `KNPL`;
  4 u16 format 1; 6 u16 header size (>= 144); 8 u32 file size (= actual); 12 u32
  CRC-32 over the whole file with these 4 bytes taken as zero; 16 u16 API version (1);
  18 u8 kind (1 synth engine, 2 effect, 3 feature); 19 u8 legacy order (0..31);
  20 char[16] saved type name, NUL padded, 1..15 printable ASCII; 36 u32 flags (bit 0
  uses sample, bit 1 page-2 defaults, bit 2 stereo FX); 140 u32 offset of an optional
  extension table.
- Extension table entries: u16 tag, u16 length, bytes padded to 4; tag 0 ends it.
  Tag 1 claims (NUL-separated), 2 author, 3 version.
- File size a multiple of 4, at most 512 KiB (the loader's limit).
- **The id** is `[a-z0-9_-]{1,15}`, the saved name with spaces as `_`, and the file is
  `<id>.knl`.
- **Boot hazard:** R246's SVG parser hangs the boot on a number with more than four
  decimals in the screen SVG. A file whose SVG has `\d\.\d{5,}` must be refused.

## op1r plugin file (`.op1p`)

- 16-byte trailer at the end: u32 magic 0x5431504F, u32 binary offset, u32 binary length,
  u32 CRC-32 of the binary. The binary offset is 4-aligned; the binary is at least 76 bytes.
- Binary: u32 magic 0x5031504F, u32 1, u32 API version (1), u32 kind (0 synth, 1 effect),
  char[16] saved name (`[a-z0-9 _-]{1,15}`). Not padded to 4. At most 512 KiB.

## The ten seed plugins (coldsoldr/te-op1-cfw, branch hk-plugins)

Header facts read from the files on 2026-10-29: all format 1, API 1, legacy order 0.

| id | kind | saved name | bytes |
| --- | --- | --- | --- |
| rings | synth (1) | rings | 111768 |
| bitcrusher | effect (2) | bitcrusher | 37400 |
| drive | effect (2) | drive | 31788 |
| tape_drive | effect (2) | tape drive | 32452 |
| wavefolder | effect (2) | wavefolder | 30464 |
| freq_shift | effect (2) | freq shift | 35776 |
| ensemble | effect (2) | ensemble | 27584 |
| tape_echo | effect (2) | tape echo | 53844 |
| marbles | feature (3) | marbles | 141508 |
| grids | feature (3) | grids | 27940 |

They need hundekraft 3.0.3. The owner publishes them under GPL-3.0-or-later with source at
https://github.com/coldsoldr/te-op1-cfw (rings, marbles, grids and the Warps frequency
shifter port Mutable Instruments code, MIT, by Emilie Gillet).

## opus plugins (`.opmod`)

Only known facts: magic `OPMD` in the first four bytes, copied into `mods/` on the
disk-mode drive, loaded in file-name order (see the custom firmware notes).

## Flash size

The OP-1 has 512 MB of flash, counted in binary units (`synth_sdk/file/file.c` in the
3.0.3 source). hundekraft's FILE screen walks the storage on the device; nothing on USB
reports free space, so co-op estimates it from the files the drive shows.

## Live audition: proof of concept (2026-10-30)

Confirmed by the owner on a real OP-1 running hundekraft 3.0.3, out of disk mode, from the
dev site in Vivaldi. The plugin is `ports/knl/audition` in coldsoldr/te-op1-cfw (GPL-3.0).

- A browser reached a kennel feature plugin over USB MIDI and got its replies. Plugins do
  not receive SysEx (`midi_raw` carries channel messages and real time only), so the test
  used CC 118 on channel 16 to ask and CC 119 to answer.
- The plugin replaced the current synth slot's whole sound, engine included, from a JSON
  object it built: stop voices, `PATCH_SET` (0x01A052DC), `op1_sound_apply`, refresh the
  screen. This is a preset drop without its save job.
- The slot's own sound came back from a copy of its JSON, and the slot still held it after
  switching the OP-1 off and on. So `PATCH_SET` alone does not save.
- Not tested: sending a whole patch over MIDI. The test sound was built into the plugin.
  Turning a knob during an audition is expected to let the stock code save the test sound.
- Speed, plugin 0.2: polyphonic pressure on channel 15 reached the plugin at a ceiling of
  18,479 messages a second (31.6 KB a second at 14 bits a message), with nothing lost and
  the sum correct at every rate tried. Timed by the plugin's reply, which travels behind
  the data. Asking for 32,000 a second still delivered 18,479, so the USB path sets the
  limit, not the plugin. Host and browser: the owner's Windows PC in Vivaldi.

## What a patch's audio section holds (2026-10-30)

- The firmware holds one spoken clip, `content/audio/speech/op1patch.raw` (57,792 bytes,
  1.31 s at 22,050 Hz). Bytes sampled from the `fm` test patch match it at the same offset.
- `synth/user/2.aif` from the owner's OP-1 (`voltage` engine) holds instead a 1.00 s render
  of middle C: 44,160 frames at 44,100 Hz. The firmware names per-slot files
  `user/audio/synth/prelisten_<n>.raw`, which are likely its source.
- The owner copied a user slot patch into `snapshot` with co-op, which copies bytes
  unchanged, and it then played the spoken clip. So the OP-1 replaced the audio. Inferred,
  not confirmed in code: `.aif` files on the drive are a view of `.raw` files and database
  rows inside the OP-1, so a render in a preset file does not survive.

## As built (phases 9-12, 2026-10-29)

co-op implements the USB check (`web/src/core/usbmidi/`, `web/src/ui/UsbCheck.tsx`),
installs over USB (`install.ts`, `PluginInstall.tsx`) and the file checks
(`shared/src/plugins.ts`) from the facts above. Everything is tested against a fake OP-1
written from these notes (`web/test/fake-op1.ts`, and the Web MIDI stub in
`web/e2e/op1-stub.ts`), not against the device. What rests on assumptions not yet seen on
hardware:

- **The USB check has never been run against a real OP-1.** HELLO, the capabilities byte,
  LIST paging, OFF_GET and the inventory text are all as the source documents them.
- **The port name.** Matching `/op-?1/i` (and skipping names with "Field") is from the
  workshop and the class-compliant name; the first real check is the first time it is
  seen.
- **Windows and Chrome.** On the owner's Windows 11 21H2 machine Chrome did not list the
  OP-1's MIDI port while Vivaldi did. The likely cause is WinMM allowing one client per
  MIDI port, with another program (or another browser) holding it. Not confirmed. When no
  OP-1 port turns up, co-op now lists the ports the browser sees and explains this.
- **Install over USB has never been tried on a device.** BEGIN, DATA, END, COMMIT and the
  T1/T2 prompt follow the docs and `du_fw.c`. Reading that source during the build showed
  that the OP-1 keeps its COMMIT prompt up after the host leaves, and once T1 is pressed
  the prompt takes no more keys (T2 included) and stays up through ABORT; the next COMMIT
  from any host is then applied at once, without a new T1 (`f_key`, `du_prompt_key`;
  ABORT resets neither). co-op therefore keeps its install dialog open from just before
  the first COMMIT until T1, T2 or silence. A resent BEGIN answering "state" is read as
  opened, as the device and workshop document; untested.
- **The `.knl` extension table offset** (header byte 140) is read as relative to the start
  of the file, not the header. The format doc does not say; the ten seed files parse that
  way (author "BJ", versions 0.1 to 0.3), which is the only confirmation.
- **opus `.opmod`** is checked for the `OPMD` magic and a 2 MiB limit only. Nothing else of
  the format is known, so a broken opus plugin with the right magic passes.
- **The `presets` field.** Whether R246's user synth preset count includes files added in
  disk mode, or only presets saved on the device, is unknown. co-op only displays it
  ("reported by the OP-1") and draws no conclusion from it.
