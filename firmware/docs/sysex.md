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
  USB-MIDI input stops on longer SysEx; co-op's sysex.js), the host keeps one message in flight.
- Commands (replies are `cmd | 0x40` with the same seq; payload = status byte + data; status 0 ok, 1 format,
  3 path, 7 io):
  - `0x21 HELLO`: reply `"coop"`, u16 protocol (1), u8 capabilities (p6k: bit 0 file read, 1 file write, 2 SLOT2, 3 RUN), then the build tag text (p6k: the package tag from core/include/coop_build.h, the same the stats file prints).
  - `0x20 FILE_GET`: payload u32 offset (LE) + path bytes; reply status, u32 total size (LE), up to 64 bytes from
    the offset. The host walks the file in 64-byte steps (about 1.5 KB/s with one message in flight).
- File writes (P1b): `0x22 PUT_BEGIN` path (under `/yaffs2/plugins/` or `/yaffs2/user/` only; the plugins directory is
  created if missing; the file is created and truncated), `0x23 PUT_DATA` u32 offset + up to 64 bytes in order (reply:
  status, u32 next offset; a wrong offset is status 5), `0x24 PUT_END` u32 total + u32 crc32 (zlib) closes and checks
  (status 4 size, 6 crc), `0x25 DELETE` path. The host page's "Install plugin" button sends a `.coop` this way; the
  loader reads the directory at boot, so a power cycle follows.
- Counters `g_sysex_polls / rx / drop / replies / errors`.

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
