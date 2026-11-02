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
  - `0x21 HELLO`: reply `"coop"`, u16 protocol (1), the build tag text.
  - `0x20 FILE_GET`: payload u32 offset (LE) + path bytes; reply status, u32 total size (LE), up to 64 bytes from
    the offset. The host walks the file in 64-byte steps (about 1.5 KB/s with one message in flight).
- Counters `g_sysex_polls / rx / drop / replies / errors`.

## Host side: `tools/coop_sysex.html`

One HTML file, Web MIDI (Chrome or Edge), no dependencies: pick the OP-1 port, Hello, Read file, Save as. The
framing matches co-op's `sysex.js` (HDR, pack7/unpack7), so the same commands can move into the librarian later.

## Status

Built into the T4a package; the emulator has no USB-MIDI, so it only proves the hook does not disturb boot or
audio. Device test pending (docs/device-tests.md, T4a).
