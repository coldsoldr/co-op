# Four tapes

<!-- Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). -->

Implemented by `kennel/kn_tapes.c` and the tapes feature. Behavior follows Maiko’s #250 guide (https://maikopruett.com/op-1/guide/tapes); no Maiko code is included.

## Behaviour
- Hold tape and press T1..T4: switch to that tape. A popup over the reels shows the tape and its tempo ("TAPE 2" /
  "125 BPM"): the tape popup undo and redo use too (`synth_sdk/screens/tape_popup.h`), drawn with kennel's own lines
  and stroke font from a tape draw hook. Tape + the current tape's number only shows the popup. Refusals ("stop
  recording first", "a new tape needs 64 mb free", ...) stay toasts. T1..T4 alone select tracks as before.
- Each tape has its own four tracks, clips, loop, position, track, tempo and album (side a / b).
- Tape 1 is TE's tape (stock paths). Tapes 2..4 are made on first use: empty, at the tempo of that moment, and they
  take flash only for what is recorded (the image is sparse).
- A switch stops playback first. It is refused while recording (tape or album), in disk mode, while R246's
  clipboard queue has work, and for a new tape with less than 64 MB free. Each tape keeps its own undo
  history (the undo feature listens to `OP1_EV_TAPE_SWITCH`); only an undo job cut short by a switch drops that tape's
  history.
- Disk mode shows the current tape only (the disk layer is bound to the current image).
- REC shows "flash full" and does not record when less than 32 MB is free (ours: one full-length track take is
  31.8 MB; a take that runs out of flash would be lost).
- Boot always starts on tape 1 (decision: no path hook in the boot sequence, so stock's start-up and its marker
  recovery only ever see tape 1). No delete yet.
- Stock firmware sees tape 1 only; the other tapes sit in their own folders it never opens.

## Files
| Tape | Image | Database | Marker |
|---|---|---|---|
| 1 | `/yaffs2/user/tape_c.raw` | `/yaffs2/user/tape.db` | `/yaffs2/user/marker.txt` |
| 2..4 | `/yaffs2/user/tape<N>/tape_c.raw` | `/yaffs2/user/tape<N>/tape.db` | `/yaffs2/user/tape<N>/marker.txt` |

`/yaffs2/user/tape<N>/ready` is written last when a tape is made; a folder without it is incomplete and is made
again (never with R246's marker, see below). Each tape.db gets one extra table, `kn_tape (id INTEGER PRIMARY KEY,
bpm10 INTEGER)` (tempo x 10), which stock ignores.

## R246 layout used by the switch
- **Paths are RAM strings**: `tape_image_open_at_boot` (0x019971B0, once, guard 0xFF906EF8) builds the image path
  into 0x01AABCE4 and the marker path into 0x01AAC934 (0x200 each, snprintf 0x01A4CCD8). Every later user reads
  the buffers (recreate 0x01996E04, marker inc / dec / dirty 0x01997FA0 / 0x0199812C / 0x019987F8 open the marker
  by path each time). #250 replaces the image path snprintf at 0x0199725E (lead only; we don't patch it).
- **Image descriptor**: 0x018C180C, the tape cache's copy via `tape_set_file_handle` 0x01A5404C. FAT disk layer:
  `fat_disk_bind_tape_image` 0x019761D8(fd), FatFs volume 0x01AABEE4 (`f_mount(volume 0, fs)` 0x01998E74, R0 the volume, R1
  the object; fs 0 unmounts). `tape_image_mount_and_index` 0x01996F38 checks the six files (recreates the whole image if one is
  missing: zero fill, about 257 MB; recreates a bad track file and flags it in 0xFF906EFC..FF), defragments when
  fragmented (bracketed by the marker), then stores the data offsets (0x018C1810 tracks, 0xFF9002C0 / C4 album).
- **tape.db**: `db_open_all` (0x0196F6B4) opens `/yaffs2/user/tape.db` into a 4-byte box whose address is kept at
  0x01BAB72C (`FUN_0196dd50` returns it; every db2 call takes the box and dereferences it under the statement lock
  0x01BAB718). Close = sqlite 0x019D6134(sqlite*), open = 0x019D73C8(path, box, 2, 0) (sqlite3_open_v2: the fourth argument, the VFS name, goes on the stack; R246 passes 0). Holding the lock while swapping
  the sqlite pointer inside the same box keeps every holder of the box valid.
- **Background SQL**: clip statements go through the BgFile worker (enqueue 0x019410C8, ring head 0x01BA33E4, tail
  0x01BA33E8, job in progress 0x01BA29D8); after an empty queue the worker saves dirty objects
  (`bg_flush_dirty_objects` 0x0193EF64, the tape row through `tape_db_save`). Idle = head == tail and no job.
- **Clip pool**: 0x01AA8D70 (512 x 12 bytes), has_clip 0x01AAA570, free count 0x01AAA770, load guard 0x01AA7368,
  `tape_clips_load_from_db` 0x0195CE88(pool) (SELECTs through the box).
- **Tape row**: `tape_db_save` 0x01A569BC (no arguments: loop 0xFF8029C5, loop in / out, track 0xFF802964,
  position). Restore as the controller's first use does: loop on 0xFF8029C5 and loop effective 0xFF8029C6,
  `tape_set_loop_in` 0x01A550A8, `tape_set_loop_out` 0x01A55100, track 0xFF802964, `tape_locate` 0x01A55024.
- **Tempo**: the tempo controller 0x01B60D90 (built once, guard 0x01B60D8C) keeps knob objects; `tempobpmdecimal`
  at 0x01B60E18 holds bpm x 10 (value +0x10 = v x scale +0xC), `tempobpm` at 0x01B60D98. The tempo screen sets
  both with `ctl_knob_set` 0x019696E8 and calls `tempo_set_bpm` 0x01A5E9A4 (float bpm, 40..220).
- **Cache**: after a flush every node is clean; `tape_cache_invalidate_clean` 0x01A54078 drops them all and the
  loop seams.
- **Guards**: tape state 0x018C17D4 (4 / 5 recording), album state 0xFF9002B4 (6 / 7 recording), disk mode
  0xFF906EF9, clipboard queue 0x01AA7330 / 0x01AA7334.

## Making a tape (ours, sparse)
R246's own build writes silence through every file (about 257 MB of flash). Ours writes only the metadata and lets
YAFFS keep the audio as holes, which read back as zeros:
1. mkdir `/yaffs2/user/tape<N>`; open `tape_c.raw` (create, truncate), `tape_image_presize` (0x01A99FB4: 0x18000000
   bytes, no data written).
2. Write the FAT16 metadata as R246 lays it out (stock FAT16 layout): boot sector, both FATs
   (contiguous chains, entries 0 and 1 zero as R246 leaves them), root directory (TAPE, ALBUM), the TAPE and ALBUM
   directory clusters, and the six AIFC headers (tracks mono 0xF28000 frames, album sides stereo 0xF23FA0).
   Generated by `kn_tape_image_meta` and checked byte for byte against an installed R246 image

3. Copy `/yaffs2/content/tape.db` (the factory copy) to `tape<N>/tape.db`, write `marker.txt` "0", then `ready`.
About 600 KB of flash. The 64 MB rule leaves room for recording on it.

## Switch (main loop, `kn_tapes_service` from `ci_service`)
Requested from the key hook (KeyHandler); runs on the main loop, which owns the tape file and the cache:
1. Refuse: recording, album recording, disk mode, clipboard queue busy, target new and less than 64 MB free.
2. Playing: press STOP (KeyHandler::dispatch_key 0x23) and retry on the next passes until stopped.
3. `tape_db_save()`; write the tempo into the current tape.db (`kn_tape`); wait until the BgFile worker is idle;
   `op1_tape_flush()`.
4. `OP1_EV_TAPE_SWITCH` before (arg = old | new << 8 | 1 << 16).
5. `tape_cache_invalidate_clean()`; unmount (0x01998E74(0)); close the image; descriptor -1.
6. Make the tape if needed.
7. Both path buffers to the new tape; open, descriptor, `tape_set_file_handle`, bind, mount,
   `tape_image_mount_and_index`.
8. Statement lock: close tape.db, open the new one into the same box, unlock.
9. Clip pool: has_clip 0, free 512, guard 0, `tape_clips_load_from_db`; recreated-track flags as at boot.
10. Tape row via `db2_get`; loop, loop points, track, locate. Tempo from `kn_tape` (a new tape stores the current).
11. `OP1_EV_TAPE_SWITCH` after (arg without the before bit); the popup.

Marker safety: R246's marker forces a rebuild of whatever 0x01AABCE4 names at boot, and at boot that is tape 1.
Both buffers point at the current tape before any stock call that can raise the marker (mount check and
defragmenter), so a power cut during tape 2's defragmentation leaves tape 2's marker raised, never tape 1's. The
switch checks the target's marker first: raised = R246 rebuilds that tape in its mount check (its own recovery).

## Limitations
- Remembering the tape across a restart (boot is always tape 1).
- Deleting a tape.
- FILE view: the storage walk counts `user/tape<N>/` under tape (`kn_storage.c`).
