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
