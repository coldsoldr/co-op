# Stereo tape (Phase 3) — traced model, design and milestone T1

Status 2026-11-02: **milestone T1 (stereo record + play in RAM) implemented and verified in op1emu**,
see section 7. Sections 1-3 are the traced stock engine, section 4 the design as first written
(interleaved frames); T1 took the lane layout described in section 7 instead. The plan of record (owner's plan, "Phase 3: stereo tape") decides the shape:
**4 stereo tracks, no album**, a fresh sparse FAT16 image on first boot after a co-op backup prompt,
`tape.db` schema unchanged, kennel `kn_tapes.c` / `kn_tape.c` reused for the image and the write-back
cadence. Everything below marked [C] was traced on the stock image in op1emu (write tracer,
`tests/sim/det_tape_rec.txt`: REC+PLAY at 32-36 s guest, a C-E-G chord at 33-34 s, STOP x2, PLAY at
38-43 s) or read in the decrypted L1 / plain SDRAM disassembly.

## 1. Storage and cache [C]

- **File.** `/yaffs2/user/tape_c.raw`, a presized (0x18000000 = 384 MB) FAT16 image with six AIFC
  files: 4 mono tracks of 0xF28000 frames (0x1E50000 bytes each, 121.6 MB) and 2 stereo album sides
  of 0xF23FA0 frames (~243 MB) — `third_party/kennel/docs/tapes.md`. 4 stereo tracks of 0xF28000
  frames = 243 MB, so the stock image size holds them once the album is dropped. Track data offsets
  in the file: `0x018C1810[4]` (filled at mount, kennel `TRACK_BASE`).
- **Cache object** (`tape_c`, static, L1 code addresses it as `P5 = 0x01b4ef34`):

  | address | field | meaning |
  |---|---|---|
  | `0x01b4ef34` / `38` | +0 / +4 | loop in / loop out (samples; `tape_set_loop_in/out` `0x01A550A8` / `0x01A55100`) |
  | `0x01b4ef3c` | +8 | cache list head (most recently used node) |
  | `0x01b4ef40` | +0xc | list tail |
  | `0x01b4ef44` | +0x10 | fd of `tape_c.raw` |
  | `0x01b4ef48` | +0x14 | chunk directory: `int32[485]` node pointer per chunk (0 = not cached), 1940 B heap block |
  | `0x01b4ef58..68` | +0x24.. | CYCLES profiling words written by the L1 tick |
  | `0x01b4ef6e` | +0x3a | W gain (0x6000 at init; the sum gain the instrument path also reads) |
  | `0x01b4ef70` | +0x3c | =1 once the audio path runs |
  | `0x01b4ef74` | +0x40 | =1 while the tape runs (set by the tick at start) |
  | `0x01b4ef78` | +0x44 | tape level ramp (.L ramp, 0x7fff) |
  | `0x01b4ef7c..88` | +0x48 | 4 pointers to 594-sample (0x4a4 B) **loop-seam staging buffers** (heap, `0x00065160` + n*0x4b0 in the emulator) |

- **Nodes.** 24 nodes, built at `0x01a54168` (called once at boot, 1.019 s in the emulator): each a
  28-byte struct from `0x1a5e3b0(0x1c)` (array `0x0107d034`, stride 0x28 with the pool header) plus a
  **256 KiB data block** from `malloc` (`0x1a9f348`, heap 0 = low SDRAM): `B[+0]` loaded/valid,
  `B[+1]`, `B[+2]` flush request, `W[+4]` dirty mask (bit per track), `W[+6]` chunk index (-1 free),
  `W[+8]`, `[+0xc]` chunk start sample (chunk << 15), `[+0x10]` data, `[+0x14]` next, `[+0x18]` prev.
  Data layout: **track t's 32,768 samples at `data + t * 0x10000`** (4 x 64 KiB). In the emulator the
  24 blocks sit at `0x000e2940 + n * 0x40000` (0x000e2940..0x006e2940, 6 MB); heap 0's top pointer
  ended the run at ~0x0072c458, i.e. **~7.2 MB of the 16 MB heap 0 in use, ~8.8 MB free**.
- **Lookup.** `tape_c::lookup(pos, &dir)` `0x01a54d5c`: chunk = pos >> 15; node = directory[chunk] or
  a list search on `W[+6]`; a hit moves the node to the head (`0x01a54dc4`); then it requests chunks
  chunk-1, chunk, chunk+1 (`0x01a54cd4`, loop-aware via `0xff8029c7`) and returns the node (0 if not
  loaded). Chunk loads are `memcpy`-class copies (`0x01aa6e40`, 32-bit) and DMA'd NAND reads landing in
  the node data (`tape_cache_io` `0x01a8e340`, `yaffs_read` `0x01A99C90`); write-back is `yaffs_write`
  at `0x01A54842` in `tape_cache_io_step`, `len` 0x10000 per track-chunk, file offset
  `TRACK_BASE[t] + chunk * 0x10000` (kennel `kn_tape_write`).
- **Not the cache:** the 7.94 MB array at `0x02040000` (0 stores over the whole run), regions A/B/C
  (0 stores: coop's homes are idle in stock), `0x03000000` (display work area).

## 2. The L1 tape tick [C]

`l1_tape_tick` `0xffa067a8..0xffa0791c` (4.5 KB of L1, called once per block from `0xffa01aca` in
the audio callback) does, per block while the tape runs:

1. **Position / speed.** Advances the position (1/64-sample units, `0xff802974`), handles loop in/out
   with a **297-sample crossfade** at the seam (constants 0x129; the four 594-sample staging buffers
   hold the seam material; `0x018c17f4` = seam valid, `W[0x018c17f0]` = seam mask), computes the
   sample count N to fetch for this block from the speed (about 130 at speed 1: `[SP+0x64]`).
2. **Per-track gains.** Pan law at `0xffa06d7e..0xffa06dc2` from the mixer's per-track level
   (`0xff801928`), pan (`0xff801930`) and `0xff8018b4`: gL[4], gR[4] as Q15 words at `[SP+0x5c]`,
   `gL = W[P+2t]`, `gR = W[P+8+2t]`.
3. **Chunk loop** (`0xffa06e04..0xffa071d4`, repeats until N samples are fetched, crossing chunk
   boundaries): `lookup(pos)` -> node; offset = pos - node.start; count = min(N left, 0x8000 - offset);
   then the **4-track mix**: `LSETUP(0xffa0714c, 0xffa0716c)` over count frames per track,
   `s = W[I3++]` from `node.data + t*0x10000 + 2*offset` (track stride `[FP-0x74]` = 0x10000),
   `L[i] = sat(L[i] + gL*s)` (store `0xffa07164`), `R[i] = sat(R[i] + gR*s)` (`0xffa0716c`) into the
   L/R play buffers `0x01b5ef94` / `0x01b5f444` (= `0x01b5ef84` / `0x01b5f434` + 8 samples of
   interpolator history), plus the per-track **peak meters** `W[[SP+0x60] + 2t] = max(|s|)`. Inside
   297 samples of a loop point the same mix runs from the staging buffers instead
   (`LSETUP(0xffa070b8, 0xffa070d8)`, tracks selected by the seam mask). Unloaded chunk -> zeros.
4. **Resample to the master.** `l1_tape_play_resample` `0xffa06194` (`CALL 0xffa07250`): 16-tap
   polyphase (coefs `0xff802154`, 16 x 64 int16, column 63-frac) over the two play buffers at the
   tape speed, x gain, writes **all 64 of d34 (L) and db4 (R)** — the master pair is stereo here.
5. **Level stage** `0xffa072c4..0xffa07314`: 64-sample in-place one-pole/level on d34/db4 with the
   ramp `0x01b4ef78`, then position bookkeeping and the end-of-tape / loop wrap.

The instrument then adds through loops A and B (nonzero gains only while the tape runs) and the
master chain follows.

## 3. Record path [C]

`l1_tape_record(mono_in)` `0xffa06334..0xffa067a8`, called from the audio callback with
`R0 = [SP+0x38]` (the mono instrument bus pointer) at `0xffa0217a` (synth path, right after the
insert-FX return is summed) and `0xffa02dca` (drum path): per block while recording
(`B[0x018c17d8]` = 1, set at `0xffa067a4`, cleared at `0xffa063a8`):

1. **Input ring.** Shifts the last 18 samples of the previous block to `0xff8029f4` (history for the
   interpolator) and writes the 64 new samples x level ramp (`[P4+0x7c] * [P4+0x80]`) at
   `0xff802a18..0xff802a96` (stores `0xffa06496`, `0xffa064bc`, `0xffa064cc`, `0xffa064ee`).
2. **Resample** `l1_tape_rec_resample(count, phase)` `0xffa06248`: count <= 600 outputs (64 at speed 1)
   from the ring at the tape speed (phase `0x018c17dc`, 6 fractional bits) through the same
   coefficient table, x gain `0x01b4ef6e`, into **`0x01b5f8f4`** (store `0xffa062f6`; 167,584 stores
   over the take — the earlier note that this buffer was idle was wrong).
3. **Overdub into the chunk.** Record node `[0x018c17ec]` from `0x01a54bb4` (the record-side lookup,
   loop-aware); `W[node+4] |= 1 << track` (`0xffa0660e`, track `0xff802964`); then per sample
   `p = node.data + (track << 16) + 2*offset; W[p] = sat(W[p] + in)` at **`0xffa065de`** (94,061
   nonzero of 134,916 stores in the take, into the node data). Chunk boundary -> next node via the
   outer `LSETUP(0xffa06556, 0xffa06638)`.

So stock records **mono, post insert FX, pre master**, by saturating add (overdub) directly into the
cache; the write-back to flash is the main loop's business (`tape_c::service(0)` at `0x019F0A24`).

## 4. Design: interleaved stereo chunks on the stock engine

The stock machinery is lane-agnostic except at a handful of places: everything that keys on the
**chunk index** (lookup, directory, dirty flags, list, write-back cadence, loop points, clips in
`tape.db` as frame ranges) stays as it is. What changes is the **size of a track-chunk** (64 KiB ->
128 KiB) and the **sample access** inside the three L1 loops that touch chunk data.

1. **Chunk layout.** A track-chunk is 32,768 **stereo frames** (interleaved L,R int16, 128 KiB), so a
   node's data block is 4 x 128 KiB = 512 KiB with track t at `data + t * 0x20000`, and the AIFC files
   are plain interleaved stereo (sowt, 2 channels, 0xF28000 frames) that disk mode and co-op read
   unchanged. File offset of a chunk: `TRACK_BASE[t] + chunk * 0x20000`.
2. **Cache.** 24 nodes x 512 KiB = 12 MB from heap 0 (~8.8 MB free today -> +6 MB fits with ~2.7 MB
   headroom; **measure the heap peak** with a sampler-heavy session before committing; the fallback is
   16 nodes = +2 MB, 11.9 s of look-ahead instead of 17.8 s). Patches: the data size `BITSET(R0, 0x12)`
   at `0x01a54172` -> bit 19 (0x80000); the write-back `len`/offset constants in `tape_cache_io` /
   `tape_cache_io_step` (0x10000 -> 0x20000, to be listed from the disassembly); the chunk load size.
3. **Play mix** (L1, `0xffa07134..0xffa0716e`, 60 bytes): `sL = W[I3++]; sR = W[I3++]`,
   `L[i] += gL*sL`, `R[i] += gR*sR` — the mixer's track pan becomes **balance** (gL, gR from the
   same pan law), same MAC count as stock; track stride `[FP-0x74]` 0x10000 -> 0x20000, frame offset
   x4 instead of x2 (`P1 = data + 4*offset`). The seam loop (`0xffa070b8`) gets the same treatment
   and the staging buffers double (0x4a4 -> 0x948 B, allocation at `0x01a54136`). Meters: per track
   `max(|sL|, |sR|)` (or two meters per track for the UI later).
4. **Record tap** (stereo in). The tap input becomes the **coop stereo instrument bus** at the stock
   tap point: `L = M + dL`, `R = M + dR` with M the mono pointer `[SP+0x38]` the stock call passes and
   dL/dR the difference buses of `stereo_bus.cc` for this block (spread 0 -> L = R = M = stock). Two
   input rings (the stock one at `0xff8029e4` + a coop twin), two resampler passes (the stock
   `0xffa06248` is reusable as is: it is parameterised by count/phase and fixed on its ring/output —
   the coop twin takes a second ring/output pair), and the overdub loop writes both channels:
   `p = data + (t << 17) + 4*offset; W[p] += inL; W[p+2] += inR`. Done as a coop replacement of
   `l1_tape_record` (the two `CALL 0xffa06334` sites are rewritten to a trampoline, as the stereo
   bus does for its sites), so the stock function stays in the image unused.
5. **Clipboard / edits.** Lift/drop/erase/split/join work in frames (`tape.db`) but copy bytes through
   the cache (`0x01A5485C` lift, `0x01A54984` lift all, `0x01A54B08` erase, `tape_clips_*`); each
   needs its byte math doubled — list the constants from the disassembly, patch, verify with a lift +
   drop of a stereo take. The clipboard buffer is likely the 7.94 MB array at `0x02040000` [I]; stereo
   halves its capacity in seconds.
6. **Image.** `tools/tape_image.py` writes the FAT16 metadata for 4 stereo AIFC (channels 2,
   0xF28000 frames, sowt 44.1/16) + a `coop_tape` version marker; first boot detects a
   missing/mismatched marker, shows a one-time notice and rebuilds (sparse, kennel's
   `kn_tape_image_meta` pattern). The factory-image rebuild path (`tape_image_mount_and_index`
   `0x01996F38`, zero-fills 257 MB and checks for six files) is bypassed for the coop layout.
7. **UI.** Album key inert; stereo per-track meters on `tape.svg` later; disk mode shows 4 stereo AIFC.

Order of work: (a) cache + write-back constants and the play mix on a **mono-compatible** layout check
(stereo chunks of duplicated L=R built from a stock take: play must equal stock), (b) record tap,
(c) image tool + first-boot rebuild, (d) clipboard math, (e) UI.

## 5. Gates

Record + play 4 stereo tracks with L/R correct (emulator: a recorded spread chord plays back with the
same width as the live signal); no dropouts under 4-track play + record (write-back latency measured);
power-cycle keeps recordings; co-op and a DAW open the AIFC files; image with spread 0 and the tape
stopped == stock output sample for sample.

## 6. Open points

- Heap 0 peak usage with the sampler engines loaded (decides 24 vs 16 nodes).
- Exact write-back/load constants in `tape_cache_io` / `tape_cache_io_step`, and the clipboard byte math.
- The record input pointer `[SP+0x38]` at the two call sites: which L1 buffer (bus `0xff800bb4` or the
  FX return) — matters only for where coop reads M; the difference buses are per block regardless.
- Emulator facts: the provisioned NAND tape is empty (silence on PLAY); record-then-play works
  deterministically with `det_tape_rec.txt`; wall-time `wait` scripts are not comparable.

## 7. Milestone T1: stereo tracks as lane pairs on the stock engine [C, op1emu]

Implemented 2026-11-02 in `core/sdram/tape_stereo.cc`, two thunks in `stereo_thunks.S`
(`coop_tape_rec_thunk`, `coop_tape_gains_thunk`) and `tools/l1_stereo_insert.py --tape-stereo`
(`tools/build_image.sh --tape-stereo`). Emulator only so far: **do not flash a T1 image** (section 7.3).

### 7.1 Layout: 8 mono lanes instead of interleaved frames

Section 4 planned interleaved stereo frames per track-chunk, which needs new L1 loops and a new record
function. T1 uses the fact that the whole stock engine is parameterised by a *lane index* with a 64 KiB
stride: node data holds `lanes x 0x10000` bytes, the record function writes lane `W[0xff802964]` at
`data + (lane << 16) + 2*offset`, the play mix walks lanes with stride `[FP-0x74] = 0x10000`. So a stereo
track t is the lane pair **2t = L, 2t+1 = R** of an 8-lane node (512 KiB), and the stock code is reused
with these changes only:

| what | stock | T1 (`l1_stereo_insert.py --tape-stereo`) |
|---|---|---|
| node data size, `tape_cache_init` | `BITSET (R0, 18)` at `0x01a54172` (256 KiB) | bit 19 (512 KiB): `904a -> 984a` |
| play mix lane count | `P2 = 4` at `0xffa07106` | `P2 = 8` (`2268 -> 4268`) |
| seam mix lane count | `P4 = 4` at `0xffa0705a` | `P4 = 8` (`2468 -> 4468`); the seam path itself is still 4-wide (T3) |
| right gain of a lane | `W[P0 + 8]` at `0xffa07138`, `W[P1 + 8]` at `0xffa070a6` | `W[P + 0x10]`: the gain table is 8 + 8 words |
| gain table / meters | stack `gL[4], gR[4]` at `[SP+0x5c]`, 4 meters at `[SP+0x60]` | `coop_tape_gains(sp)` (hook replaces the 8-byte `P1.L/P1.H = 0x018c17f0` at `0xffa06de0` after the pan law) rewrites the two slots: lanes `(gL_t, 0)` / `(0, gR_t)`, an 8-word coop meter array folded into the 4 stock meters one block later |
| record | `CALL 0xffa06334` at `0xffa0217a` / `0xffa02dca` | `CALL tramp -> coop_tape_rec_thunk -> coop_tape_record(mono)` |

**The mixer's track pan becomes balance**, the stock pan law unchanged.

### 7.2 Record: the stock function twice per block

`coop_tape_record(mono)` builds `L = sat(M + dL)`, `R = sat(M + dR)` from the mono bus the stock call
passes (`[SP+0x38]` = `0xff800bb4`) and the stereo bus's decimated difference buses for this block
(`coop_stereo_side_bufs`, valid because the tap runs after loop B; zero when the mix was mono), then
runs the **unmodified stock `l1_tape_record`** once per channel with the lane index swapped
(`0xff802964 = 2t`, then `2t+1`, restored to `t` at the end). Between the passes the engine state is put
back to what it was before pass 1, and after pass 2 it is set to what pass 1 left, so the two passes
look like one stock pass plus a second lane's data:

- snapshot/restore: `d_tape_rec_state` `0x018c17d8` (8 words) and the tape state words
  `0xff802960..0xff8029f4` (37 words; position, record position, level words, chunk requests);
- each channel keeps its own copy of the record input ring `0xff8029f4..0xff802a98` (41 words:
  18 samples of interpolator history + 64 new), swapped in before and out after its pass;
- the resampled buffer `0x01b5f8f4` and the node's dirty bits need no handling (rewritten / OR-ed).

Self-checks: `g_tape_state_mismatch` (record state after pass 2 != after pass 1) and
`g_tape_state_mismatch2` (tape state) stayed 0 over the whole run. Cost: `g_tape_rec_cycles_max`
12,737 cycles (2.2% of a block; the stock pass is ~5k, the second pass plus 4 x 41-word copies the rest).

Finding on the way: a first version that restored the state only *between* the passes recorded the same
sample values into both lanes but **not** stock's values, and the spread-0 image played the take back
one block late; also putting the post-pass-1 state back *after* pass 2 made both lanes bit-identical to
stock's track (0 of 32,680 samples differ in the traced chunk) and the output identical. The mismatch
counters never fired in either version, so the difference sat in a word pass 2 touches that the checks
do not cover (a candidate: the chunk-request words `0xff8029e4/e8`, which `tape_rec_node_lookup`
rewrites); the fix removes the dependence. Diagnostic lesson recorded in `docs/emulator.md`: compare
**RAM contents** (record stores) as well as WAVs, and remember that the gains hook keys on the same
control word (a "stock single pass" probe with the stereo gain layout plays the right lane silent).

### 7.3 Verified (op1emu, `tests/sim/det_tape_rec.txt` and `det_chord.txt`)

| image | result |
|---|---|
| `--tape-stereo`, spread 0 (`t1_zero3`) | **sample-identical to stock** over the whole record + play run (4,040,573 frames from 5 s) and over the chord scenario (3,137,942 frames). Both recorded lanes identical to stock's recorded track in RAM. |
| `--tape-stereo`, spread 32767, `g_coop_force_stereo_out=1` (`t1_full2`, copy `C:\Users\byron\Downloads\t1_full_st.wav`) | live chord L 977 / R 698 / side 218, width 0.27; **playback L 745 / R 532 / side 169, width 0.27** (level x0.76 from the stock tape play gain): the stereo take plays back with the live width and balance. |
| `g_coop_tape_stereo=0` (8-lane loops, stock single pass, stock gain layout) | identical to stock (the loop/gain patches alone are transparent). |

Not covered yet (and why a T1 image must not be flashed): the **write-back and chunk load still run the
stock 4-lane loops** (`tape_cache_io_step` `0x1a54710`: `R4 = 4` at `0x1a5476a`, `R1 = 4` at
`0x1a547e8`, `TRACK_BASE` `0x018c1810[4]`), so on a stock image lane 1 (track 1 R) is saved into the
`track_2.aif` file and lanes 4-7 are never loaded: the take survives in RAM only, and a device would
write a stereo take across the mono track files. T2 fixes this.

### 7.4 Next: T2 file layout and image, T3 seams/edits

- **T2.** Loop counts 4 -> 8 in `tape_cache_io_step` and an 8-entry coop base table instead of
  `0x018c1810` (also referenced by lift `0x1a5485c`, lift-all `0x1a549b8`, erase `0x1a54b08`, mount
  `0x19970aa`); the mount/index of a coop image with **8 mono AIFC files** (`track_1_L.aif`,
  `track_1_R.aif`, ... 0xF28000 frames each: a DAW pairs them, co-op pairs them; 243 MB, the album
  dropped) — or 4 stereo AIFC with an interleave/de-interleave step in the chunk I/O (128 KiB bounce
  buffer); `tools/tape_image.py` on kennel's `kn_tape_image_meta`; a first-boot rebuild with a
  `coop_tape` marker. Heap: 24 x 512 KiB = 12 MB of the 16 MB heap 0 (7.2 MB in use today);
  fallback 16 nodes.
- **T3.** The seam path (`0xffa070b8` and the SDRAM seam fill `0x1a54284`, 4 x 594-sample staging
  buffers) for 8 lanes; lift/drop/erase/split/join byte math for lane pairs (`src_ch/dst_ch < 4`
  asserts in `tape_c.cpp`); stereo meters in the tape UI; the album key inert.

## 8. Milestone T2: the file side (coop image, chunk I/O, power-cycle safe) [C, op1emu]

Implemented 2026-11-02 in `core/sdram/tape_io.cc`; the owner chose the fresh image (option A below).

- **Image.** The stock image file (`/yaffs2/user/tape_c.raw`, 384 MB, kennel's FAT16 layout) now holds
  **four stereo AIFC tracks** (`/tape/track_1..4.aif`: sowt, 2 channels, 0xF28000 frames, 63.6 MB
  each, 7761 clusters, data at `0x37070`, `0x3cd9070`, `0x797b070`, `0xb61d070`) and no album. A marker
  `/yaffs2/user/coop_tape.txt` ("coop_tape 1 stereo4") says the image has this layout.
- **Mount.** `coop_tape_mount_and_index` replaces `tape_image_mount_and_index` (`0x1996f38`; callers
  `0x199758e`, `0x19f0354`, both ignore the result) through region-C trampoline 2 (`0x01e00020`). The
  layout is fixed, so the data offsets are constants: it fills `TRACK_BASE 0x018c1810[4]` and the coop
  table `g_coop_track_base[4]`, and points the album offsets `0xff9002c0/c4` at tracks 1/2 so the inert
  album code never reads outside the file. **Without the marker it rebuilds the image sparse** (kennel's
  `make_tape` + `mount_tape`: unmount, close, reopen with O_TRUNC so the old audio becomes holes, presize,
  stock boot sector, FATs + directories + headers from `coop_tape_image_meta`, reopen, rebind, remount,
  write the marker). `g_coop_tape_image=0` disables the rebuild (tables only). The stock `tape.db` is
  kept (stale clips of the old tape stay as silent clips; a reset is T3's).
- **Chunk I/O.** `coop_tape_io_step(mode)` replaces `tape_cache_io_step` (`0x1a54710`; callers
  `0x19f0a24` service(0) and `0x19f0be2` service(1)) through trampoline 1 (`0x01e00010`): the stock
  list walk and flags (`B+2` flush request, `B+1` loading, `W+4` dirty, chunk re-check), with lane
  pairs: a chunk of track t is 32,768 interleaved frames (128 KiB) at `base[t] + chunk * 0x20000`,
  de-interleaved on load into lanes 2t/2t+1 and interleaved from them on flush through a 128 KiB
  static bounce buffer. Counters `g_tape_mounts/rebuilds/rebuild_err/io_loads/io_flushes/io_errors`.
- **Going back to stock:** stock's mount misses its six files and rebuilds its own image (minutes of
  zero-fill on that boot); the stereo recordings are gone, nothing else changes. No factory reset.
- **Host test** `tests/host/tape_image_test.cc` checks the FAT chains, directories, headers and offsets
  of the generated metadata (`COOP_HOST` build of `tape_io.cc`).

### 8.1 Verified (op1emu, `NAND_KEEP` in `hear.sh` / `watch_writes.sh`)

| step | result |
|---|---|
| first boot of the full-spread image (fresh NAND) | mounts 1, rebuilds 1, rebuild errors 0, base table as computed on the host |
| `det_tape_rec_quit.txt`: record the spread chord, STOP x2, 45 s for the write-back, quit | loads 11, flushes 418, I/O errors 0 (the stock flush cadence writes the recording chunk on every main-loop pass, 128 KiB each: kennel's held saves are the T3 fix) |
| second boot on the kept NAND, `det_tape_play_start.txt` (tape, STOP x2, PLAY) | mounts 1, **no rebuild**, loads 25, errors 0; **the take plays back after the power cycle with the live width**: L 748 / R 534 / side 170, width 0.27 vs live 910 / 649 / 209, 0.27 (`C:\Users\byron\Downloads\t2_play_after_reboot.wav`) |
| spread-0 image, second boot, `det_tape_rec.txt` | envelopes equal to stock within a few percent (live 333/523/435 vs 342/525/425; playback 395/398/249 vs 401/395/242). Not sample-identical any more: the coop mount changes the boot timeline, so the synth oscillators sit at a different phase when the chord lands. The bit-exact gate now needs a scenario keyed to the audio start; T1's in-session gate stays as the record/play reference. |

Device: `build/pkg/op1_246_coop_t2_stereo_tape.op1` (also in Downloads) — **flashing it reformats the
tape on the first boot** (recordings lost; back up with co-op first). Pending the owner's result in
`docs/device-tests.md`.

## 9. T3 (2026-11-02): lane order, right-lane pass without the track word, held saves, eight-lane seams

- **Device bug from T2.** The owner flashed the T2 package: stereo recording worked, but switching to track 2
  asserted `ReadInterface.cpp:471 ch >= 0 && ch < kTapeNumTracks` and the device then asserted at every
  boot. The T2 record hook wrote the lane index 4-7 into the track word `0xff802964` for its second pass;
  the tape screen draw (`0x1a5c984` -> `0x1a29a74`, the function that asserts and returns a per-track byte
  from `0xff8010a0[ch]`) and `tape_db_save` read the word from the main thread and on the device they saw
  it, so the bad track was drawn and saved. Lesson: a word the UI thread reads must never carry a transient
  value from the audio path, whatever the emulator shows. Fix: `coop_tape_clamp_track` (mount, every audio
  block, every I/O step) and the pass below.
- **Lane order** is now L lanes 0-3, R lanes 4-7 (`t`, `4 + t`): the stock four-track code (seam fill,
  record extent, dirty bits, meters) then sees the L lanes as its tracks. The file layout is unchanged.
- **Right-lane pass.** `coop_tape_record` runs the stock pass once as is (lane t) and once with every node
  data pointer moved up by `4 * 0x10000` under `cli`, so the stock code addresses lane `4 + t` while the
  track word stays `t`; both passes set dirty bit t and the I/O step flushes the pair on bit t or `4 + t`.
- **Held saves** (kennel kn_tape.c rule): while recording, a node saved less than 1 s ago keeps its flush
  request and the step goes on to loads; the take end flushes everything. Emulator take: 12 flushes instead
  of 418, held 596 times, 0 errors.
- **Loop seams for eight lanes.** `coop_seam_fill` (the L1 `P1 = 0x01a54284` at `0xffa073b8` now loads its
  address) runs the stock fill twice, the second time with the node pointers moved and the stock staging
  table pointed at coop buffers (R lanes); hook B (`0xffa0706e`, the `P2 = 0x01b4ef7c` before the seam
  loop, through `coop_seam_prologue_thunk`) hands the loop an 8-entry pointer table and mirrors the
  valid nibble `W[0x018c17f0]` into bits 4-7; hook A clears bits 4-7 each block so the stock `== 0xf`
  fill decision keeps working. The L1 note-on trampoline (which never fired) gave its 12 bytes to hook B.
  Tested in T3c (section 11): the first build faulted on a wrong stack slot in hook B's thunk; fixed.
- Verified (op1emu): full-spread take in-session and after a power cycle plays with width 0.27 (live 0.27);
  state-mismatch counters 0; spread-0 envelopes equal stock within the boot-timeline shift.
- Still open: lift/drop/erase/split/join copy one lane; the instrument monitor on the tape page is mono;
  tape.db reset on rebuild; first-boot notice; album key inert; a loop test.

## 10. T3b (2026-11-02): stereo lift / drop / erase, the tape-page monitor, and two thunk lessons

- **Edits** (`core/sdram/tape_edit.cc`): `coop_lift` / `coop_drop` / `coop_erase` stand in for the stock file-level
  edit functions (lift `0x1a5485c` with callers `0x191b89c` and lift-all `0x1a549a0`; drop `0x1a549b8` with
  callers `0x191b67e` and drop-all `0x1a54aee`; `fast_tape_erase` `0x1a54b08` from `0x191b9f0`) through
  region-C trampolines 3-5 (`0x01e00030/40/50`). The stock functions read and write the tape FILE in mono at
  `TRACK_BASE[ch] + pos*2`; with stereo tracks a frame is at `pos*4`. Lift reads the frames, keeps the LEFT
  samples in the stock clipboard slot (`0x02040010`, sized by the service for `len` samples, so the UI and
  `tape.db` keep working in frames) and the RIGHT samples in a coop pool (region B `0x02800000`, 4 MB; a
  longer clip keeps its left side only and drops as mono, `g_edit_mono_fallbacks`), zeroes the frames and
  invalidates the cache range; drop writes the frames back from both; erase zeroes 485 x 128 KiB per track.
  These three take their 4th/5th argument on the stack, so their trampolines are plain jumps
  (`build_jump_tramp`): the CALL-based region-C trampoline shifts stack arguments by 4 (the first build read
  `hdr` from the wrong slot). Clipboard facts: queue entries of 32 bytes at `0x01aa7130` (16), consumer
  `0x01aa7330`, producer `0x01aa7334`, command 2 = lift (then `start`, `stop` in frames, `0x3c8fe8` the pool
  capacity in samples), pushed by `0x191bbd0` from the tape controller; state record `0x01aa7100`
  (channel, length, has-clip), pool header `0x02040000` (4 slot pointers), data from `0x02040010`.
- **Tape-page monitor** (mono on the device at T3): mix loop A (`0xffa00f2c..0xffa00f86`) carries the
  instrument on the tape page and while the tape runs; it is now replaced through the same trampoline as
  loop B (docs/stereo-bus.md addendum): the thunk tells the sites apart by the return address the trampoline
  pushed, loop A keeps its ramp/meter record at the stack slot `[SP+0x3c]` of the site (P1 there is the loop
  count), and coop adds dL/dR before the gains. Verified **sample-identical to stock** on the record scenario
  and on the tape-page chord scenario with the base image; stereo on the tape page with the full image.
- **Thunk lessons** (cost most of the day): the stock code after a replaced inline loop keeps using registers
  the loop did not touch -- P0 (loop B: its count 0x40; loop A: `0x01b4ef6e`) and R2 (loop A: the loop B step
  when the branch at `0xffa00f8e` skips). The C1 thunk never restored them and was right by the accident of
  what GCC left in P0; the T1 "a block late" bug was that accident breaking, and its "fix" put it back. The
  thunk now saves R0/R2 and sets P0 from `g_mix_p0_out`. And the assembler silently swallows a code line
  placed inside a multi-line comment: keep comments on their own lines in `.S` files (twice today).
- Verified (op1emu, `det_tape_liftdrop3.txt`): record a spread chord, lift it (left slot + 89,519 nonzero
  right samples in the coop pool, 6 chunk reads/writes, 0 errors), drop it at the start, play: the dropped
  clip plays with its width (0.25-0.28) at the original level; the lifted range is silent. Full-spread
  in-session and power-cycle gates as before. `C:\Users\byron\Downloads\t3_lift_drop_stereo.wav`.
- Still open: split/join (frame ranges in tape.db, believed unaffected), erase untested on the device, the
  loop seam path untested (no loop keys in the emulator map), tape.db reset on rebuild, first-boot notice,
  album key inert, stereo meters.

## 11. T3c (2026-11-02): the loop seam path, tested and fixed

- **Loop keys.** The original OP-1 has no loop key: on the tape page the sound keys 1, 2, 3 are loop in,
  loop out and loop on/off (tape key handler `0x1a589a8`, jump table `0x18fc038` by key code - 6; the
  codes 0x2d / 0x2e / 0x2f are GPIO bank 5 bits 13-15 = op1emu `s1` / `s2` / `s3`; key code = bank * 16 +
  bit - 0x30, te-op1-cfw screen-interface.md). Key 1 / 2 set the point at the playhead through
  `0x1a56ae8` / `0x1a56a70` and turn the loop on (`0xff8029c5` on, `0xff8029c6` effective; a locate clears
  effective, `0x1a59380` sets it again when the playhead enters the loop). Scenario `tests/sim/det_tape_loop.txt`:
  chord at 0.8-1.8 s, loop in at 0, loop out at 1.636 s (72141 samples), play 8.5 s.
- **The bug** [C]: the first seam block faulted (`Data alignment fault PC=ffa07092 ADDR=0166018b`, the
  emulator then replays the last DMA buffer for ever, which the T3b build heard as a steady tone). Hook B's
  thunk wrote the 8-entry staging table into `[SP + 76]`, believing `[--SP] = (R7:0, P5:0)` leaves P0 at
  `[SP]`; the push stores R0 first and P5 last, so `[SP] = P5`, `[SP + 12] = P2`, `[SP + 52] = R0` (checked
  in the GNU simulator). The table went into P3's slot, P2 kept the trampoline's thunk address, the seam
  loop read the thunk's first instruction bytes (`0x01660167`) as a buffer pointer, plus twice the offset 18 =
  the fault address. Fix: `[SP + 80]`.
- **Verified** (op1emu, full spread): the chord repeats five times in 8.5 s with width 0.25-0.28 on every
  pass, the largest sample step at the seams no larger than stock's, pass spacing (1.75 / 1.5 s alternating)
  equal to stock's; the spread-0 image loops the same mono take five times. No fault, counters 0.
  `C:/Users/byron/Downloads/t3e_loop_full.wav` (Windows path).
- Package `build/pkg/op1_246_coop_t3c_stereo_tape.op1` (sha256 6b0adf63...): T3b plus this fix.
- Still open: split/join, erase on the device, tape.db reset on rebuild, first-boot notice, album key
  inert, stereo meters.

## 12. T3c on the device, and the per-voice question (2026-11-02)

- T3c device result: stereo monitor on the tape page, record, lift, drop all work (docs/device-tests.md).
- On the older T3 package the owner heard a recorded chord played back as the whole chord panned to one
  position per strike rather than each voice at its own position. In op1emu the per-note right/left ratio of
  the C4 / E4 / G4 chord is the same live, on tape in session and on tape after a power cycle (0.49 / 0.56 /
  0.80 live; 0.46 / 0.58 / 0.85-0.93 on tape), and constant across each note's harmonics, so the emulator
  keeps per-voice placement in both the record and the file path. The device check is pending: the tape file
  copied in disk mode (frames as written) bisects record from playback; the stats file's `mismatch` counters
  tell whether the right-lane pass ever diverged on the device.
- Performance on the device: the tape animation goes choppy while a chord is recorded; the T3d package writes
  a stats file to measure it (docs/performance.md).

### 8.2 T3 next (as written before T3)

Held saves while recording (kennel `kn_tape.c`); the loop seam path for 8 lanes (`0xffa070b8`, the
SDRAM seam fill `0x1a54284`, 4 x 594-sample staging buffers); lift/drop/erase/split/join for lane
pairs (`0x1a5485c`, `0x1a549b8`, `0x1a54b08`, `tape_clips_*`; the `TRACK_BASE` users now see stereo
offsets but copy mono-sized ranges); a tape.db reset on rebuild; stereo meters; the album key inert;
a one-time first-boot notice.

### 7.5 T2 plan of record as written before T2 (kept for the record; option A was taken)

Two ways to give the R lanes storage. **(A) Fresh image** (the plan): 8 mono AIFC files, a coop
mount/index replacing `tape_image_mount_and_index` (stock indexes exactly six names via `0x1997c58`:
tracks `0x18d9eac` + 0x18*t -> `0x018c1810[t]`, `side_a` -> `0xff9002c0`, `side_b` -> `0xff9002c4`),
`tools/tape_image.py`, a first-boot rebuild. **(B) Album-sides bridge** (less work, no reformat, keeps
the user's mono tracks as the L lanes): lane 2t+1 lives in the album storage as a planar mono stream,
lanes 1/3 at `side_a + {0, 0x1E47F40}`, lanes 5/7 at `side_b + {0, 0x1E47F40}`; the album sides hold
0xF23FA0 frames, so R lanes have 484 chunks instead of 485 (the last 0.37 s of the tape is mono) and
chunk 484 of an R lane must be skipped on load (zero) and write. Either way `tape_cache_io_step`
`0x1a54710` becomes a coop C function (both callers `0x19f0a24` service(0) and `0x19f0be2` service(1)
retargeted through a region-C trampoline like `Core::NoteOn`): walk the list from the head
`0x1b4ef3c`, flush request `B[+2]` -> write dirty lanes 0..7 (`lseek 0x1a9a0e4`, `write 0x1a99f84`,
fd `0x1b4ef44`), else load an invalid node (`W[+6] >= 0`, `B[+0] == 0`) lane by lane (`read
0x1a99c90`), keeping the `B[+1]` loading flag and the chunk-index re-check of the stock code. Test:
a two-run emulator scenario on a kept NAND copy (hear.sh needs a NAND_KEEP option): record + stop +
quit, then boot + play; the stereo take must come back with its width.
