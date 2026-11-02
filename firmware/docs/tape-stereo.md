# Stereo tape (Phase 3) — research notes and design

Status 2026-11-02: research started after milestone C and LFO x2. Nothing implemented. The plan of
record (owner's plan, "Phase 3: stereo tape") decides the shape: **4 stereo tracks, no album**, a fresh
sparse FAT16 image on first boot after a co-op backup prompt, `tape.db` schema unchanged, kennel
`kn_tapes.c` / `kn_tape.c` reused for the image and the write-back cadence. This document collects what
the code says so far and what still has to be traced.

## What is established [C unless marked]

- **Storage.** `/yaffs2/user/tape_c.raw` is a presized (0x18000000) FAT16 image holding six AIFC
  files: 4 mono tracks of 0xF28000 frames and 2 stereo album sides (`third_party/kennel/docs/tapes.md`
  "Making a tape"; the metadata layout is reproduced byte for byte by kennel's `kn_tape_image_meta`).
  4 mono tracks + 2 stereo sides = the storage of 4 stereo tracks, so the image size stays.
- **Cache.** Audio streams through a chunk cache (32,768-sample chunks, write-back from the main loop's
  `tape_c::service(0)` call at `0x019F0A24`; `tape_cache_io` `0x01a8e340` does the yaffs reads/writes;
  the `yaffs_write` in `tape_cache_io_step` is at `0x01A54842`). The 7.94 MB array at `0x02040000`
  (getter `0x191b320`) is the prime candidate for the cache [I, memory-map.md]; stereo doubles the
  per-track chunk (64 -> 128 KiB).
- **Audio path (L1).** `0xffa06194` (`l1_tape_resampler`, called once per block from `0xffa07250`)
  addresses the master pair directly (`I1 = 0xff800d34`, `I2 = 0xff800db4`), walks four per-track
  state records (`0x01b5ef84`, `0x01b5f434`, stride to be confirmed) in an outer `LC1` loop with an
  inner `LC0` sample loop, and uses the L1 buffers `0xff802154` and `0xff8029dc`: this is the tape
  **play** mix (4 mono tracks, per-track level + pan, into L/R) [I: read of d34/db4 confirmed
  statically; the record/play split is to be traced]. `0xffa06248` (same region; gain word
  `0x01b4ef6e`, `0xff802154`, `0x01b5f8f4`) is the companion stage, likely the **record** tap
  (post-instrument, pre-master, as stock overdub) [I]. The audio-callback callee `0xffa036c4` with its
  4-wide state at `0xff801870` is the track/tape stage of the master graph [I].
- **Controller state.** Tape state `0x018C17D4` (4/5 = recording), track `0xFF802964`, loop flags
  `0xFF8029C5/C6`, position via `tape_locate` `0x01A55024`, `tape_db_save` `0x01A569BC`
  (`tapes.md` "R246 layout").
- **Emulator.** The provisioned NAND's tape is empty: `tests/sim/det_tape_play.txt` (tape, play, stop)
  produces silence (`stock_tape.wav`, rms 0.7 throughout). Tracing the tape path needs a record-then-
  play scenario (`tests/sim/det_tape_rec.txt`, first attempt in progress) or a NAND with recorded tape.

## Design (from the plan, refined)

1. **Image.** `tools/tape_image.py` writes the FAT16 metadata for 4 stereo AIFC (channels 2,
   0xF28000 frames, sowt 44.1/16) + a `coop_tape` version marker; the firmware's first boot detects a
   missing/mismatched marker, shows a one-time notice and rebuilds (sparse, kennel pattern). The
   factory-image rebuild path (`tape_image_mount_and_index` `0x01996F38`, zero-fills 257 MB) must be
   bypassed for the coop layout.
2. **Engine.** Replace the L1 play mix and record tap with stereo versions in coop code: per track a
   stereo chunk reader (interleaved sowt frames), level + **balance** (the mixer's track pan becomes
   balance), summed into d34/db4 after the instrument mix; the record tap takes the stereo instrument
   bus (post coop difference mix, post insert FX, pre master) into the track's stereo chunk, overdub as
   stock. Resampling (tape speed / wow) stays on the stock algorithm applied per channel. Clip
   ranges in `tape.db` are frames, so lift/drop/split/join need no schema change; the clipboard copies
   twice the bytes.
3. **Memory.** Doubled chunk cache from heap 0's ~9 MB free (kennel 3.1 pattern) or by halving the
   `0x02040000` window once its role is confirmed; keep the main-loop write-back cadence, adopt
   kennel's held saves while recording and the bounded shutdown flush (`kn_tape_final`).
4. **UI.** Album key inert; stereo per-track meters on `tape.svg` (content, editable); disk mode shows 4
   stereo AIFC that co-op reads unchanged.

## Gates

Record + play 4 stereo tracks with L/R correct (emulator: a recorded panned chord plays back with
the same width); no dropouts under 4-track play + record (write-back latency measured); power-cycle
keeps recordings; co-op opens the files; image at spread 0 and tape stopped == stock output.

## Next steps

1. Get a recorded tape in the emulator (`det_tape_rec.txt`; if the key choreography is wrong, record
   through the GUI with `play.sh` once and keep that NAND as `tape_nand.bin`).
2. Trace with `watch_writes.sh`: writers of d34/db4 during playback (the play mix PCs), of the L1
   buffers `0xff802154`/`0xff8029dc` (chunk staging), and of the `0x02040000` array (cache role),
   plus the record tap during recording.
3. Read the per-track state records and the resampler's step/phase format; decide where the stereo
   engine lives (L1 residency for the per-sample loops, SDRAM for the chunk logic).
4. Write `tools/tape_image.py` against kennel's `kn_tape_image_meta` and the AIFC headers, and a host
   test that mounts the image with a FAT16 reader.
