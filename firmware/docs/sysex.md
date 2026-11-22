# The SysEx file channel (milestone U1)

Disk mode shows a FAT the stock disk layer synthesises (`synth/`, `drum/`, `tape/`, `album/`); no yaffs file is ever
visible there (docs/performance.md, T3g). While the OP-1 runs it is a USB-MIDI device, and kennel's development
update already speaks SysEx with it (third_party/kennel/synth_sdk/devupd/, docs/inventory.md). coop-fw uses the same
transport to read any file off the device while it runs.

## Device side: `core/sdram/coop_sysex.cc` [I until the device test]

- Receive: the stock `PlayerManager::tick -> midi_poll` calls `midi_rx_parse` (`CALL 0x019E31A4` at `0x019E3CC8`,
  timer context). The package retargets that CALL through a jump trampoline at `0x01E00060` to `coop_sysex_rx_poll`,
  which copies our messages (`F0 00 20 76 7D cmd seq_lo seq_hi <7-bit packed payload> F7`) out of the USB-MIDI
  receive ring (`0x01AB0510`, 1024 x 4-byte packets, counters `0x01AB1510/4`: kennel du_fw.c) into a four-entry
  queue, then runs the stock parser, which ignores them.
- Service: `coop_sysex_service` runs from the main loop (the tape I/O step, mode 0) and answers through the stock
  `usbmidi_tx_enqueue` (`0x019E0D18`) as USB-MIDI packets. Every message both ways is at most 96 bytes (the OP-1's
  USB-MIDI input stops on longer SysEx; co-op's sysex.js). The queue (`QN` = 4, `coop_sysex.cc`) is first in, first
  out: the service answers every queued message in the order it arrived, one reply each, on its next pass. A message
  that arrives with the queue full is dropped without a reply (`g_sysex_drop`). So a host may keep more than one
  message in flight, but never more than the queue holds: for every command but PUT_DATA the hosts keep one; for
  `PUT_DATA` they may keep up to `QN` - 1 = 3 (the librarian since web commit `ca8cfe9`, 2026-11-20; see "File writes"
  below).
- Commands (replies are `cmd | 0x40` with the same seq; payload = status byte + data; status 0 ok, 1 format,
  3 path, 7 io):
  - `0x21 HELLO`: reply `"coop"`, u16 protocol (1), u8 capabilities (p6k: bit 0 file read, 1 file write, 2 SLOT2, 3 RUN; e3: 4 INVENTORY, 5 PLUGIN_SET), then the build tag text (p6k: the package tag from core/include/coop_build.h, the same the stats file prints).
  - `0x20 FILE_GET`: payload u32 offset (LE) + path bytes; reply status, u32 total size (LE), up to 64 bytes from
    the offset. The host walks the file in 64-byte steps (about 1.5 KB/s with one message in flight).
- File writes (P1b): `0x22 PUT_BEGIN` path (under `/yaffs2/plugins/` or `/yaffs2/user/` only; the plugins directory is
  created if missing; the file is created and truncated), `0x23 PUT_DATA` u32 offset + up to 64 bytes in order (reply:
  status, u32 next offset; a wrong offset is status 5 and the reply still carries the offset the file has reached),
  `0x24 PUT_END` u32 total + u32 crc32 (zlib) closes and checks
  (status 4 size, 6 crc), `0x25 DELETE` path. The host page's "Install plugin" button sends a `.coop` this way; the
  loader reads the directory at boot, so a power cycle follows.
- **Several `PUT_DATA` in flight.** The host may send up to three chunks before the first is answered. The core
  takes them in order: a chunk is written only at the offset the file has reached, and every reply's "next offset" is
  cumulative, so an ok that names the end of a chunk confirms every byte before it. If one chunk is lost (the queue
  was full: dropped, no reply) the chunks behind it arrive at the wrong offset and each gets status 5 with the offset
  reached; nothing is written out of order. The host then resends from that offset, one chunk at a time
  (`web/src/core/usbmidi/coop.ts` `COOP_WINDOW`, `putWindowed`). `PUT_END`'s size and CRC-32 check is unchanged and is
  what proves the file. Why three and not four: the slot of the message being answered is free only when the service
  has moved past it. [C device, e11, 2026-11-20: the librarian installed `rings.coop` (113 KB) in under 20 s, about
  76 s with one chunk in flight. The drop and the wrong-offset replies: by the code and the web fakes (`web/test/fake-coop.ts`), not provoked on the device [I].]
- Counters `g_sysex_polls / rx / drop / replies / errors` (image words; not in `coop_stats.txt`, so the drops of a
  device session cannot be read back yet).

## Host side: `tools/coop_sysex.html`

One HTML file, Web MIDI (Chrome or Edge), no dependencies: pick the OP-1 port, Hello, Read file, Save as. The
framing matches co-op's `sysex.js` (HDR, pack7/unpack7), so the same commands can move into the librarian later.

## Status

Built into the T4a package; the emulator has no USB-MIDI, so it only proves the hook does not disturb boot or
audio. Device test pending (docs/device-tests.md, T4a).

## Second effect slot: `0x26 SLOT2` (S2, 2026-11-03)

Payload byte 0 is the operation:
- `0` get: reply status + 10 bytes per slot (instrument, master): kind (0 none, 1 stock type, 2 plugin), id (stock
  type 0 delay, 1 grid, 2 punch, 3 phone, 4 spring, 5 filter, 6 nitro, 7 cwo; or the plugin number), 4 x u16 knob
  (Q15, little-endian).
- `1` set: pos (0 instrument, 1 master), kind, id, 4 x u16 knob. Status 2 = unknown effect or no memory for it,
  7 = the file write failed. Saved to `/yaffs2/user/coop_slot2.bin`; the reply is the get reply.
- `2` plugin list: status + `n=name;` text for every loaded plugin.

The host page's "Second effect slot" section reads both slots and the plugin list, and applies one slot at a time.

## Test runner: `0x27 RUN` (p5b, 2026-11-04)

Payload byte 0 is the operation: `0` status, `1` start (then the path of a CRUN file under `/yaffs2/user/`, which the
host first writes with PUT), `2` abort. Every reply is the status byte (start: 0, or 2 when the runner refused the
file) and 12 bytes: state (0 idle, 1 armed, 2 running, 3 done, 4 stopped, 5 error), u32 block, u16 event index,
u16 events, sections closed, error (1 open, 2 format, 3 too many events, 4 bad event, 5 busy), the run file written.
An image without the runner answers 1 (format). The host page's "Run a test" section drives it; docs/test-runner.md.

## Plugin management: `0x28 INVENTORY`, `0x29 PLUGIN_SET` (e3, 2026-11-09)

For the co-op platform's Plugins tab (install, remove, switch on and off from the browser). Both answer from the main
loop like every command; capabilities bits 4 and 5 say they are there.

- `0x28 INVENTORY`: payload u32 offset (LE). Reply: status, u32 total length (LE), up to 64 bytes of text from the
  offset. Offset 0 takes a fresh snapshot (the directory as it is now, the boot scan's results, the settings file's
  list); later offsets page through that snapshot. The text:
  - line 1: `coop-inventory 1<TAB>build <tag><TAB>protocol 1`;
  - then one line per `.coop` file in `/yaffs2/plugins`, tab-separated: file, kind (`fx`, `synth`, `?` for a file the
    boot did not read), name (the type name the lists show), boot (`loaded`, `failed`, `disabled`: skipped by the
    settings list, `new`: the file came after the boot), next (`on` or `off`: what the next boot does), version,
    author, size in bytes, reason (the loader's text for a failed file, else empty). e7: up to 48 files (8 KB of text);
    a file the boot refused for the plugin memory budget or skipped past the 32-file limit is `failed` with the reason
    (`over the plugin memory budget (needs N KB, M KB of 1536 KB left)` (1728 KB on e11, 5760 KB from e12), `not enough memory left for the tape` (e7-e11 only),
    `more than 32 plugin files`).
- `0x29 PLUGIN_SET`: payload enable byte (1 on, 0 off) + the file name (no directory). Puts the file on or off the
  settings file's disabled list and writes `/yaffs2/user/coop_settings.bin` at once. Reply: status, the enable byte.
  Status 2: the device's settings menu is open (its own edit wins: close it and retry); 3: a name with `/`; 4: the
  list is full (32 files since e7, 16 before); 7: the write failed. Applied at the next boot, like the menu's toggle.
- No restart command exists: after PUT, DELETE or PLUGIN_SET the host asks the user to power-cycle.
- Proven in op1emu with the image-time self-test `--set-word g_coop_sysex_selftest=1`: once the plugins are loaded,
  `coop_sysex_service` feeds `handle()` HELLO, a full INVENTORY, PLUGIN_SET off `saw.coop`, INVENTORY, PLUGIN_SET on,
  a bad name, and writes every reply as a hex line to `/yaffs2/user/coop_sysex_test.txt` (op1emu has no USB-MIDI).
  Result (2026-11-09, `saw.coop` and `stereo_delay.coop` on the NAND): HELLO `coop` protocol 1 caps `0x3f` build `e3`;
  the inventory lists both files `loaded on` with kind, name, version, author and size; after PLUGIN_SET off the saw
  row says `off`; on returns 0; `../x` returns 3. [C op1emu]

## INVENTORY on e12: the budget, each file's memory (2026-11-20)

e12 (docs/plugins.md "e12") appends to the inventory text; nothing that was there moved, so a host written for e3-e11
reads it unchanged (the librarian's parser, `web/src/core/usbmidi/coop.ts`, finds the header's `build` field by its
name and takes a row's first nine tab-separated columns) [C: `coop_usb.py --transport usbip inventory` against op1emu,
whose parser is the same].

- **Header line**, after `coop-inventory 1<TAB>build <tag><TAB>protocol 1`, five more tab-separated `name value`
  fields:
  - `budget <bytes>`: the plugin memory budget, what a memory bar shows as 100 % (5,898,240 on e12);
  - `used <bytes>`: what the budget counts for the loaded plugins (the `plugmem` line's `used`);
  - `fxbuf <bytes>`: the size of one effect position buffer (0: no scratch effect loaded);
  - `fxbufs <n>`: how many position buffers exist (0..4; under 4, not every position can run a scratch effect);
  - `clip <bytes>`: the stereo clipboard's pool (4,194,304 unless the plugins spilled into region B; never under
    1,572,864).
- **Rows**: two more columns after `reason`:
  - column 10, `need`: the bytes the file takes of the budget by itself, or would have taken (image, voice state,
    screen object, control table; without the shared position buffer). 0 for a file the boot did not read that far
    (disabled, new, past the 32-file limit, refused before its header was parsed);
  - column 11, `scratch`: the file's position buffer size, rounded up to 32 (0: not a scratch effect, or not read).
  A set's cost is the sum of its `need`s plus the largest `scratch` once (docs/plugins.md "e12", the rule).
- `size` (column 8) of a `new` file (one that arrived after the boot) is now its real size: the file is opened and
  measured when the inventory is taken. Before e12 it was 0. Its `need` and `scratch` stay 0 until a boot reads it.
- `reason` (column 9) texts on e12:
  - `over the plugin memory budget (needs N KB, L KB of 5760 KB left)`: N = the file's `need` plus what it would
    grow the budget's one position buffer by, L = budget - used, both in whole KB as before;
  - `plugin memory full`: the budget had room but neither arena range could hold the block whole (the two ranges
    are not contiguous);
  - a **loaded** file may now carry a reason: `no memory for this position` (a scratch effect was selected at a
    position that had no buffer left; it runs at the others). The stats name the positions (`plugfx` line);
  - `not enough memory left for the tape` no longer occurs (the heap guard went with e12).

Example (op1emu, e12, tabs shown as ` | `):
```
coop-inventory 1 | build e12 | protocol 1 | budget 5898240 | used 980992 | fxbuf 134784 | fxbufs 4 | clip 4194304
rings.coop | synth | rings | loaded | on | 0.5 | coldsoldr | 113048 |  | 667712 | 0
tape_echo.coop | fx | tape echo | loaded | on | 0.3 | coldsoldr | 23892 |  | 14592 | 134784
zzover.coop | ? |  | new | on |  |  | 428 |  | 0 | 0
```
(the 13 catalogue files, two of the rows, and a 428-byte test file put over USB after the boot). From two other
runs: a file over the budget, and a scratch effect that found no buffer at a position (an image that carves one):
```
zzover.coop | synth | zzover | failed | on | 0.1 | coldsoldr | 428 | over the plugin memory budget (needs 63 KB, 33 KB of 5760 KB left) | 64192 | 0
tape_echo.coop | fx | tape echo | loaded | on | 0.3 | coldsoldr | 23892 | no memory for this position | 14592 | 134784
```

## INVENTORY on e13: two budgets and the placement policy (2026-11-21)

e13 (docs/plugins.md "e13") places each class of plugin memory in heap 0 or in coop's arena by a boot policy, with a
budget for each. The inventory appends again; every e12 field keeps its place and a meaning a one-budget host can
use [C: `coop_usb.py --transport usbip inventory` against op1emu].

- **Header**, after e12's `budget used fxbuf fxbufs clip`:
  - `budget` / `used` (e12's): now **the budget that binds**, i.e. the one with less room left: the heap budget's
    figures when the policy puts anything in heap 0 and it has less room than the arena, else the arena's. With
    `all=arena` they are e12's exactly;
  - `heap_budget <bytes>` (1,769,472), `heap_used <bytes>`: what the policy put in heap 0;
  - `arena_budget <bytes>` (5,898,240), `arena_used <bytes>`: what it put in the arena (always the screen objects
    and control tables; one position buffer when `buf=arena`);
  - `place eng=heap,fx=heap,voice=heap,buf=arena`: the policy in force (one field, commas, no spaces);
  - `place_src default | file | malformed | marker`: the built-in default (no policy file), the file, the file was
    ignored as malformed, the file was ignored because the last boot with it did not finish (delete
    `/yaffs2/user/coop_place.try`).
- **Rows**: column 12, `heap`: the part of the file's `need` (column 10) that is in heap 0; the arena part is
  `need - heap`. `need` is the file's own bytes in both pools together; `scratch` as e12.
- `reason`: `over the plugin memory budget (needs N KB, L KB of B KB left)` names the budget that refused the file
  (B = 1728 for the heap's, 5760 for the arena's); `not enough memory left for the tape` is back for files whose
  heap part the heap guard refuses.
- The policy file is `/yaffs2/user/coop_place.txt`, written with PUT and applied at the next boot (tokens:
  docs/plugins.md "e13").

Example header (op1emu, e13 default, the 13 catalogue files, tabs shown as ` | `):
```
coop-inventory 1 | build e13 | protocol 1 | budget 1769472 | used 833168 | fxbuf 134784 | fxbufs 4 | clip 4194304 | heap_budget 1769472 | heap_used 833168 | arena_budget 5898240 | arena_used 156416 | place eng=heap,fx=heap,voice=heap,buf=arena | place_src default
rings.coop | synth | rings | loaded | on | 0.10 | coldsoldr | 113816 |  | 668536 | 0 | 666872
tape_echo.coop | fx | tape echo | loaded | on | 0.4 | coldsoldr | 25188 |  | 15760 | 134784 | 14096
```
