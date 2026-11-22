# Efficiency audit (2026-11-20)

A read of the whole firmware (`core/`, `sdk/`, the 13 plugins, the build) for what it could cost less: cycles per
audio block, heap 0 and L1, boot and NAND writes, code size, install speed. Nothing was changed. Every finding names
the code and, where one exists, a figure already recorded in `docs/`; estimates say what they rest on.

Marks: **[C]** read in the current code or measured (device or op1emu, as named); **[I]** inferred, with the
measurement that would settle it. Costs are cycles per 64-sample block of 580,499 (`docs/performance.md`).
"Device" figures come from the recorded stats files (`docs/device-stats/`, `docs/m1-results.md`); op1emu counts
instructions, the device is 10-35 % higher and more where code or data is refetched.

State of the tree: dev at `ac5072c`, core tag `e10`. Another session has uncommitted work on fickle and petri
(`"scratch"` 171,936 -> 134,784 and 150,624 -> 134,752): the memory figures below use the working tree's values and
say where HEAD differs. No device or op1emu run was made for this audit (op1emu was in use); the sizes come from
`bfin-elf-size` / `nm` on `build/core/coop_combined.elf` (11 Nov) and the packed files in `build/plugins/`.

## Summary, ranked

Effort S = hours, M = a day or two, L = a week. "Decision" = changes behaviour or needs the owner.

| # | Finding | Where | Estimated gain | Effort | Risk | Verify with |
|---|---|---|---|---|---|---|
| 1 | **Done in e12; slower on the device; a boot policy in e13.** Plugin memory sits in heap 0 beside the tape cache while 3.25 MiB of coop's own region A is unused | `plugin_host.cc:490,599,690`, `coop_cb.ld:5` | 0.67 MiB of heap 0 back at once (the position buffers), 1.0 MiB staging block gone, the budget can go from 1.5 to ~2.2 MiB with heap 0 untouched [I] | M | region A's tail is mapped [C] but only its first 0.75 MiB has run on a device | op1emu `--watch-writes` over `0x02cc0000..0x02ffffff` on a stock session, then the 13-file set |
| 2 | **Done in e14.** The tape record tap runs the stock record twice, with six state copies, on every block, recording or not | `tape_stereo.cc:112-175` | 4-6 k of the 7.2-7.5 k it costs a block [C device for the cost, I for the share] | M | must prove the stock function writes no lane while not recording | `rec_sum / recs` in the stats windows, `det_tape_rec*.txt` bit-exact |
| 3 | **Done in e14 (a switch).** The measuring code is always on: per-voice cost records, marks, a 63-slot scan and two folds at every block start | `stereo_bus.cc:142-161`, `engine_host.cc:166-181`, `coop_stats.cc:153-208`, `stereo_thunks.S:35-68` | 5-6 k a block while voices play [C device, m1c], more with the per-voice calls; 2-4 k idle [I] | S-M | Decision: the stats page loses its live cost table unless it is a switch | `cb_rest`, `thunk` in an m1 run with the switch off / on |
| 4 | **Done in e14.** `coop_stats.txt` and the 8 KiB tape tail are rewritten every 5 s for the whole session | `coop_stats.cc:28,671-677,724` | ~19 KiB of NAND per 5 s (13.7 MB an hour) and a 10-50 ms main-loop stall each time [I from the recorded write times] | S | the librarian reads the file: serve it from RAM | `stats writes` count, `io_gap_max`, fps |
| 5 | **Done in e14.** The S3 stage and the mix run in full on a silent instrument | `stereo_bus.cc:398-427,519-546` | ~11 k + ~8 k a block while nothing sounds [C device costs], bit-exact [I] | M | the skip conditions must be exact (ring, DC state, ramp) | `det_chord.txt` identical; `stage=` and `mix_sum` idle |
| 6 | Idle paths missing in the heavy effects (tape_echo, cs_hall, cs_plate, fickle, petri, rings verb) | each `render` | 13-18 k a block per idle position [I: plugin cost minus the wrap] | M each | not bit-exact (thresholds); a tail cut below ~-96 dB | each plugin's `prof`, `s2_*` / `fx1_plugin` idle |
| 7 | **Done in e12.** Positions 0 (synth) and 1 (drum) never run at once but each has a buffer | `plugin_host.cc:83,385,770-772,1129-1134` | one buffer of five: 134,784 bytes (171,936 at HEAD) [I] | S-M | the stock must re-init the effect on every synth / drum change | `det_fx_two_pos.txt`, `p0=`/`p1=` init counts |
| 8 | Software divides every block in nine plugins (the knob scaling); five per voice render in rings FM | `tape_echo.cc:109-114` and its copies, `rings/src/fm_voice_fixed.cc:132-138` | 0.75-1.5 k a block per effect; rings FM ~1.1 k a render, 4-7 k a block [I: ~190 cycles a `__divsi3`] | S | none, bit-exact | host tests bit-identical; `fx1_plugin=` |
| 9 | **Done in e15.** The effect wrap is compiled C: int16 mid / side -> Q31 L / R and back, 2.3 k for a do-nothing effect | `plugin_host.cc:1198-1221,1248-1251,1465-1468,1506-1512` | ~1.5 k a block per plugin position, ~5 k with all four in use [I from test_fx's 3,068 C op1emu] | S | bit-exact kernel, sim-checkable | `fx1_plugin=` of test_fx |
| 10 | SysEx installs keep one 64-byte message in flight | `coop_sysex.cc:38-39,242-253`, `web/src/core/usbmidi/coop.ts:50,156-163` | 1.5 -> ~4.5 KB/s with three in flight: rings 76 s -> ~25 s, the 13-file set 4 min -> 1.3 min [I] | S-M | drops when the queue overflows; needs the device | time a rings install |
| 11 | rings: reverb buffer twice the size it reads; 48 string lines for at most ~16 live | `rings/src/reverb_fixed.h:32-33`, `src/string_voice.h:109`, `src/string_fixed.h:199` | 54 KB (S, bit-exact) and ~260 KB (M) of its 572 KB `.bss` [I] | S / M | the string pool touches voice stealing | host bit-exact, `plugmem used` |
| 12 | The largest effect state sets all five buffers: tape_echo's 1.5 s | `tape_echo/src/tape_echo_layout.h:13`, `plugin_host.cc:473` | 220-240 KB with a ~1.0 s cap; or size classes (two large, five small) ~390 KB [I] | S / M | Decision: a shorter echo, or at most N large effects at once | `plugmem fxbuf` |
| 13 | The master suite divides per sample (64-bit in the compressor, `/ 3` in the drive) | `core/include/coop_master.h:30,51` | none today (flag off); without a rewrite the suite cannot be switched on inside the budget [I] | S | changes rounding of an unshipped stage | `tests/sim` cycle count of `coop_master_process` |
| 14 | **Core side done in e15** (the plugin side is pack.py's). Plugins link `ld -r` with no section GC; the core ships its probes and test code | `tools/plugin/pack.py:39,187-195`, `coop_stats.cc:364-365`, `coop_runner.cc:64` | ~3 KB of dead kernels in cs_hall / cs_plate / rings; 28.7 KB of 165 KB core text is two NOP probes [C sizes] | S | none | `bfin-elf-size`, packed sizes |
| 15 | Kernels still to write: the shared half-band (four plugins), rings FM loop, rings modal tables, fickle diffusers | see "Plugins" | ~2 k a block per half-band plugin, ~4 k a rings FM render, 3-19 k on a rings strike (device) [I] | M-L | bit-exact if transcribed | each plugin's sim check |
| 16 | Boot: every effect screen's SVG is parsed at the first frame; two SELECTs per absent plugin name every boot | `engine_screen.cc:260-273`, `plugin_host.cc:249-271` | boot time, size unknown: needs a timestamp around `build_fx_all` | S-M | a first-show delay instead | CYCLES around both, in the stats |
| 17 | Tape cache: 24 nodes of 512 KiB whatever the tracks hold | `plugin_host.cc:323-324`, `docs/tape-stereo.md` §1 | 512 KiB of heap 0 per node dropped | M | Decision: less tape in RAM, more loads when shuttling | tape scrub / loop tests on the device |

Items 1, 4, 8, 9, 14 and the small ones under "Small per-block items" are mechanical. Items 3, 6, 12, 13, 17 need a
decision. The rest are bit-exact by design but need a gate.

What coop costs a silent OP-1 today, from the device files: record tap 7.2 k + mix ~8.4 k (9.6 k before p6j) + S3
stage 10.9 k + block start 4.5 k = **~31 k cycles a block, 5.3 % of the budget, before any note** [C device:
`docs/device-stats/u5_2026-11-07_stats.txt` idle windows, `p5g_diag` `01>02`, the `cost` line]. Items 2, 3 and 5
address about two thirds of it.

---

## A. Safe, mechanical gains

### 1. Plugin memory out of heap 0 (region A's tail)

Evidence [C]:
- Coop links into region A, `0x02c00000`, length `0x400000` (`core/sdram/coop_cb.ld:5`). The image ends at
  `___image_end = 0x02cc02b4` (`nm` on the e10 ELF: text 164,980, rodata 11,692, bss 610,036). From there to
  `0x03000000` is **3,407,180 bytes (3.25 MiB)** that nothing uses: the last fixed address in it, the old delay
  line at `0x02d00000`, went with p6k (`master_suite.cc:46-48`). The page's DCPLB and ICPLB entries are visible in
  the stock table (`te-op1-cfw/docs/memory-map.md` §4: `0x02c00000`, `0x0003101d` / `0x00031005`), and coop's code
  and bss already run in its first 0.75 MiB.
- Every plugin byte is taken from heap 0 with the stock `MALLOC`: the 1 MiB staging block
  (`plugin_host.cc:59,690`), each image (`:490`, `:550`), the five position buffers (`:599`), the engines' voice
  state (`engine_host.cc:59`), the screen objects (`engine_screen.cc:220`). Heap 0 is also where the stock builds
  its 24 x 512 KiB tape cache right after the scan; that collision is the e6 boot hang and the reason for the
  1,536 KiB budget and `heap_room_ok` (`plugin_host.cc:309-330,403-414`, `docs/plugins.md` "e7").
- A side effect of staging in the heap: the freed block is `blk(0x100000) = 0x100008` bytes and a tape node needs
  `0x80008`, so the hole holds one node and leaves 0x80000 bytes, 8 short of a second (`docs/plugins.md` e7: "one
  from the staging hole"). Half a MiB is usable only by small allocations.

Proposed: a bump allocator from `___image_end` (32-byte aligned, never freed, zeroed on take, the same
`code_sync` after relocation) for, in order of value: (a) the position buffers (one line changed at `:599`;
673,960 bytes of heap 0 back with the working-tree set, 753 KB at HEAD); (b) the staging block (placed at the top of
the region, used only during the scan); (c) the images and voice state. With all three, heap 0 is not touched by the
scan, `heap_room_ok` has nothing left to guard, and the budget becomes region A's: 3.25 MiB less 1 MiB staging =
**~2.2 MiB for plugins** against 1.5 MiB, with ~1.5 MiB more of heap 0 free for the stock. The inject tool already
refuses an image outside the region (`tools/inject_app.py:52,58`); the arena's bounds need the same check at run
time.

If only the staging block is to be fixed in place: `MALLOC(FILE_MAX + 8)` makes the hole two nodes exactly.

Risk [I]: region A beyond 0.75 MiB has not been written on a device. Gate: op1emu `--watch-writes` over the tail
during a stock session (boot, tape, lift / drop: the p6c crash was coop's own clipboard pool in region B,
`tape_edit.cc:34`, not this region), then the 13-file NAND, fresh and second boot, `plugmem` and `heap_free_boot`.

**Done in e12 (2026-11-20; `docs/plugins.md` "e12", gate in `docs/device-tests.md` "e12").** All three parts, and
more than proposed: the arena is region A above the core (`0x02CE0000..0x03000000`, 3,276,800 bytes) and then the
upper 2.5 MiB of region B, 5,898,240 bytes in all (the budget, against 1,728 KiB), with the stereo clipboard keeping
a 1.5 MiB floor of region B and whatever the plugins leave. The staging block is the clipboard floor's first MiB
(not the arena's top: it costs nothing there), so the scan takes nothing from heap 0, `heap_room_ok` is gone, and
the control tables (`engine_ui.cc`, uncounted heap 0 before) moved too. Measured [C op1emu]: the stock writes
nothing in `0x027ffb04..0x02ffffff` through a first boot, sounds, kits, tape, lift and drops; with the 13 files heap
0 ends a first boot with 2,725,820 bytes free (e11: 1,205,596; a boot without plugins: 2,729,484). Open [I, the
device]: region A above 0.75 MiB on real SDRAM, and what running plugin code and state from this SDRAM bank instead
of heap 0's costs in cycles (kennel measured a slower cold line fill there).

**The device on e12 (2026-11-21), and e13.** The memory side held (13 loaded, heap 0 free as without plugins) and
the estimate of its cost was wrong: with the plugins' code and state in region A their code stall cycles about
doubled (fickle at FX 1: 29,615 -> 43,188 cycles a block, code stalls 11,427 -> 22,398; rings a voice render: code
stalls 6,060 -> 10,420), the callback with rings went from 367-403 k to 457-520 k a block with dropouts, and the
owner went back to e11 (`docs/plugins.md` "e13", `docs/device-stats/e1[12]_2026-11-21_*`). The CPLB words say region A
is cached exactly as heap 0 is, so it is not a cache-policy setting [C]; whether a line costs more to fill there or
the plugins miss more is not known [I]. **e13** makes the placement a boot policy per class (engine images, effect
images, voice state, position buffers; heap 0 or the arena), default images and voice state in heap 0 under e11's
budget and guard and the buffers and cold data in the arena, and adds the measurements to decide it on the device
(per-window engine / effect sums, a live fill probe in both regions, the SDRAM registers). What this finding still
gives for certain: the staging block, the screen objects, the control tables and, by default, the position buffers
out of heap 0 (the 13 files: 833,168 bytes of the 1,728 KiB heap budget against e11's 1,510,352).

### 4. The stats file every 5 seconds

Evidence [C]: `coop_stats_tick` closes a window every `WINDOW_CYCLES` = 5 s (`coop_stats.cc:28`) and, when not
recording, calls `write_file` (`:724`), which formats ~11-12 KB of text (the recorded files are 10.6-12.4 KB),
writes its first 8 KiB into the tape image's last chunk (`:671`, `tape_io.cc:286-293`) and rewrites
`/yaffs2/user/coop_stats.txt` with O_TRUNC (`:672-676`). The u5 device file shows it: `stats writes 361 ...
windows 360` after 30 minutes.

Cost [I]: ~19 KiB to the NAND every 5 s, 13.7 MB an hour, in a release image nobody is measuring. A 64 KiB yaffs
write took 35 ms idle and 85-173 ms under a chord on the device (`docs/performance.md` "T4a on the device"), so each
rewrite stalls the main loop for roughly 10 ms idle to 25-50 ms under load, plus the formatting (a `__divsi3` per
digit in `putn`). That is one to three UI frames every 5 s.

Proposed: keep the windows in RAM as now; write the file (a) once after `FIRST_WRITE_PASSES` so a session names its
build, (b) after a test run (`coop_stats_write_now`), (c) when a host asks: `FILE_GET` of the stats path at offset 0
calls `write_file` into `g_text` and is served from that buffer, no NAND at all. The tape-tail copy only behind a
settings row or an image flag (it exists for disk mode without a host). The web client's new plugin-memory read
(`web/src/core/usbmidi/coop.ts`, uncommitted) reads the same path and would keep working.

**Done in e14 (2026-11-21; `docs/performance.md` "e14" 5).** The text is made in RAM and a FILE_GET of the stats
path is answered from it (made afresh when a read starts); the file and the tape tail are written only with
`g_coop_statsfile` (1 in the ELF for the op1emu gate, 0 in the device packages, the policy token `statsfile=1`).
The windows tick without the write. Not measurable in op1emu (no NAND timing): the stall is the device's.

### 8. Software divides per block and per render

Evidence [C by reading; the call counts from disassembly of test builds, not the packed files]:
- The knob scaling `(p[k] - lo) * 32767 / (hi - lo)` runs four times a block in tape_echo (`tape_echo.cc:109-114`,
  plus two in the gain, `:118-127`), cs_hall (`cs_hall.cc:55-60`), cs_plate (`cs_plate.cc:62-64`), tape_drive
  (`tape_drive.cc:56-66`), bitcrusher (`bitcrusher.cc:38-41`), fickle (`fickle.cc:164-169`, plus the decay at
  `:503-507`) and petri (`petri.cc:66-71`). `hi - lo` is a constant per knob but the compiler sees a table.
  ensemble, freq_shift, drive and wavefolder already cache on a changed word (`ensemble.cc:72-84`).
- rings FM: five divides by `n = size` per voice render (`rings/src/fm_voice_fixed.cc:132-138`); `size` is always
  `kSubblockSize` = 32 (`src/op1_voice.cc:437`).
- libgcc's `__divsi3` on this CPU is a 32-pass loop, ~190 cycles a call.

Gain [I]: 0.75-1.5 k a block per effect (a third of bitcrusher's own 3.4 k); rings FM ~1.1 k a render, 4-7 k a
block at four to six voices. Proposed: ensemble's `ReadParams` cache in the seven effects; `n` as the constant in
FM (shifts, same truncation). Bit-exact; the host tests prove it.

### 9. The effect wrap in assembly, its work block in L1

Evidence [C]: a plugin at instrument FX 1 gets its block through two compiled loops: mid + side -> saturated L / R
-> `<< 16` (`plugin_host.cc:1210-1214`), and back: `>> 16`, saturate, mid, two differences (`:1216-1221`). Instrument
FX 2 does the same twice over (`:1506-1512` to int16 L / R, `:1465-1468` to Q31 and back); the master positions
widen and narrow (`:1248-1251`). The stock twins take the int16 form (`:1198-1203`). The do-nothing test effect
costs 3,068 cycles a block at FX 1 in op1emu (`docs/device-tests.md` "e7"), of which its own loop is ~0.7 k: the
wrap is ~2.3 k. The same shows as the constant ~1.25 k between `fx1_plugin` and `s2_instr` for every port
(`docs/plugins.md` "The nine effect ports": tape_drive 10,931 / 9,666, cs_hall 20,010 / 18,763), and slot 2's own
conversions sit outside its timer.

Proposed: two hardware-loop kernels, `ms16_to_lr32` and `lr32_to_ms16` (~5 and ~8 instructions a sample against
~14 and ~22), used by all four call sites; one 512-byte L / R work block in the L1 pool for every position (they
never nest) instead of the interrupt stack at slots 0-2 and SDRAM statics at slots 3-4 (`:1465`, `:1506`: 24 data
lines that are refetched whenever the block evicted them). Gain ~1.5 k a block per plugin position in op1emu, more
on the device [I]. The L1 pool has the room: 8,416 bytes, 3,200 used by the core, 4,096 for engine leases
(`l1_pool.cc:92-93`, the `l1` stats line).

**Done in e15 (`docs/performance.md` "e15" 1).** Five kernels in `core/sdram/wrap_fast.S` (the way back has two:
FX 1 wraps `l - m`, slot 2 saturates it, and they differ at one corner of the input), every plugin call site, one
512-byte work block from the L1 pool shared by all positions with a nesting guard, `g_coop_fast_wrap` 0 / 1 / 2
(e14's C / the kernels / the kernels checked against the C every block). The idle marker in `l[0]` was not built:
not exact for today's files (the reasons there).

### 14. Build: dead code ships, flags differ

Evidence [C]:
- Plugins: `pack.py` compiles with `-O2 -mcpu=bf524 -ffreestanding -fno-builtin -nostdlib -fno-common`
  (`tools/plugin/pack.py:39`), no `-ffunction-sections`, and links `bfin-elf-ld -r` (`:187-195`): every function of
  every listed source ships. Found: cs_hall's two mono kernels, 1,750 bytes, assembled with no guard and no caller
  (`cs_hall/hall_bfin.S:405,409`); cs_plate's `_PlateTankRunMonoAsm`, 504 bytes (`cs_plate/plate_bfin.S:297`);
  rings' `Limiter::Process`, `RingsVoice::Render` and `MonoLimiterLoopC`, ~1 KB. Proposed: `#ifdef` them now;
  `-ffunction-sections -fdata-sections` plus `--gc-sections -u <entry>` on the relocatable link as the general fix.
- The half-band files are copied into six plugins (identical but for comments and ensemble's symbol prefix):
  ~1.6 KB of image each, 6-9 KB in all. Not worth a core export for the bytes; worth one shared source so a kernel
  rewrite (item 15) lands everywhere.
- Core: 165 KB of text holds `probe_24k` and `probe_4k`, 24,578 + 4,098 bytes of NOPs for the p6f instruction-cache
  probe (`coop_stats.cc:364-365`), run at every boot (`plugin_host.cc:668`); the SysEx self-test
  (`coop_sysex.cc:354`); the lockstep debug compare inside `coop_instr_stage` (`stereo_bus.cc:404-414,428-447`).
  The bss holds the runner's 128 KiB text and 16 KiB event list (`coop_runner.cc:57,64`), two 128 KiB bounce
  buffers that are never used at once (`tape_io.cc:156`, `tape_edit.cc:44`), and 8 + 32 KiB of probe data
  (`coop_stats.cc:408-409`). None of it costs cycles; it is 29 KB of the LDR block and the web delta, and ~330 KB
  of region A that item 1 could use. Proposed: a `COOP_DIAG` define, off in release packages.
- The core's flags (`core/sdram/Makefile:7-9`) lack the `-fno-builtin` and `-fno-tree-loop-distribute-patterns`
  the top Makefile and the plugins use; the source works around it by hand (`plugin_host.cc:488`, "an initialiser
  here became a memset call"). Add both.
- LTO is not used anywhere. Not proposed for the core: the hot paths are assembly or already inlined through
  headers, and LTO would undo the `.text.coop_voice` placement unless the section attributes survive [I]. `-Os`
  was tried and rejected for rings (its README).

**Core side done in e15 (`docs/performance.md` "e15" 2).** `COOP_DIAG` (`core/include/coop_build.h`): the release
package's ELF (`make -C core/sdram combined-release`) is built without the probes, the pf sweep, the fill probe, the
SysEx self-test and the stage / voice-hook debug compare; the dev ELF and every gate image keep them. The two flags
are in the core's Makefile (the loaded bytes of e14's sources are identical with them). Section GC was already on
for the core (`-ffunction-sections -fdata-sections`, `--gc-sections`). Kept in both: the runner (`tools/devloop.py`
drives the release image too), the two tape bounce buffers (bss, no cycles; merging them is not a size question for
the LDR). The plugin side (pack.py) is not part of e15.

### Small per-block items (each under ~0.3 k; do them when the file is open)

- `fx_sanitize` walks 17 slots on every dispatch, twice a block (`plugin_host.cc:1102-1115`, called at `:1186` and
  `:1243`). It guards a stored value; it can run on an init or a change.
- `fx_params` rebuilds the 16-word block and three stats words per position per block (`:1072-1088`).
- `st_render` swaps eight display words in and out around every stock twin render and re-checks the tank binding
  (`:1027-1054`).
- `coop_stereo_block_start` calls `coop_l1_sine_copy`, `coop_tape_clamp_track` and two `coop_spread_set` every
  block (`stereo_bus.cc:148-159`); the first is a boot action, the others belong on a change.
- The voice hook keeps seven counters per voice in SDRAM bss (`stereo_bus.cc:323-341`).
- petri's screen does 48 `__muldi3` call sites a frame (`petri/petri_screen.cc:74-88`): UI thread, not audio.

---

## B. Bit-exact by design, need a gate

### 2. The record tap while not recording

Evidence [C]: `coop_tape_record` stands in for the stock `l1_tape_record` at both call sites and runs every block
(`rec_calls 1244073` of `blocks 1244073`). Each call builds L and R from mid and side (64 samples,
`tape_stereo.cc:126-134`), snapshots the record and tape state (45 words), runs the stock function on the left
lane, snapshots again, restores, walks all 24 cache nodes twice with interrupts masked to shift their data
pointers, runs the stock function on the right lane, compares 45 words and restores (`:143-170`): about 475 word
copies through `volatile` pointers besides the two stock passes. On the device it costs 7.2 k cycles a block with
nothing recording (`u5` windows: `rec_sum` 24.98 M over 3,450 blocks; `docs/device-tests.md` p5g: "~7.5 k per block
while not recording"), 20 k under a recorded chord (T4a).

Proposed [I]: when the tape is not recording, one stock pass on the mono input and no snapshots; the second ring
is brought up to date from the first when recording starts (as `g_rings_inited` does at `:135-139`). This needs
the stock function read once to confirm that, not recording, it only advances its input ring (the right-lane pass
then has nothing to write). The recording flag is the one `tape_io.cc:427` reads. Gain: 4-6 k a block, always.
Gate: `det_tape_rec*.txt`, `det_tape_liftdrop*.txt` bit-exact; `mismatch 0`.

Further [I]: the 45-word copies are per-word `volatile` loops; a hardware-loop copy halves what is left while
recording.

**Done in e14 (`docs/performance.md` "e14" 1).** The stock function was read: with the tape state word not 1 and the
recording byte 0 it makes two stores into the tape state and returns; it does not advance the input ring (the
proposal above assumed it did). So the idle block is one stock call with no copies at all, not a mono pass plus a
ring catch-up. op1emu: the tap 2,710 -> 178 cycles a block; a check image compared every idle block.

### 5. Silence in the instrument path

Evidence [C]: `coop_instr_stage` runs every block (`stage_calls` = blocks) at 10,880 cycles average on the device
(the `cost` line; 11.4 k in the p6b table, of which 3.4 k are stalls), whether or not a voice was rendered. The mix
entry then adds the 64 samples into the master pair: 9.6 k a block in the u5 idle windows (`mix_sum` 33.0 M over
3,450 blocks), ~8.4 k since p6j.

Proposed: skip the stage when its output is provably zero: no voice hooked this block (`g_pair_dirty` 0), the
previous block was the same (the 32-entry rings then hold only zeros: a block pushes 64 pairs), and both DC states
have `xprev == 0`, `yq == 0` and `0 <= y < 65536` (then the stock recurrence leaves `y` unchanged and the output 0;
`coop_stereo.h:406-420`). The skip writes 64 zeros to the mid and both sides once, then nothing, and advances the
ramp word as 64 saturating adds do. With the stage skipped and the instrument's effects idle, the mix adds zero:
it only has to advance its ramp and, for loop A, leave the meter (`max(meter, 0)`). Gain ~11 k + ~8 k a block
while the instrument is silent. It does not lower the peak block; it gives the UI thread its time back at rest,
which is where the frame rate is judged. Gate: `det_chord.txt` and the stock-compare switch (`g_coop_dbg_cmp`)
sample-identical across note starts and tails.

**Done in e14 (`docs/performance.md` "e14" 3 and 4).** The conditions above, exactly, for the stage; for the mix a
test of the three buffers for zero instead of "the effects idle" (the core cannot know an effect's tail, but it can
see the buffers), so it holds with effects on. Added: a voice that rendered 128 zeros is not accumulated and does
not mark the pair dirty, so a held, silent rings chord (0.11) reaches the rest path. A check image compared every
skipped block with the real code. What an idle effect still costs (its render and the wrap's conversions around
it: on the device 13 k a block in `fx` for an idle tape_echo, e13) is item 9's and the effects' own idle paths.

### 7. One buffer for the synth and drum positions

Evidence [C]: five position buffers, each the largest declared size (`plugin_host.cc:83,385-392,598`). The stock
has one instrument effect object for both instrument positions (`:770-772`), and `coop_fx_init_mono` resets
`g_cur[pos]` whenever the stock constructs it (`:1129-1134`), so the plugin's `init` runs again and resets its state
(the e8 rule: nothing is preserved across a selection).

Inferred: the synth and the drum are never rendered in the same block, and each change between them goes through
that init. Then slots 0 and 1 can return the same buffer: 134,784 bytes saved (171,936 at HEAD). To confirm: that
no path renders slot 1 after slot 0 without an init between (`p0=` / `p1=` init counts across a synth / drum / synth
change in `det_fx_controls.txt`), and that the screen's `view->live` goes false for the other slot (set
`g_cur[other] = -1` at the init).

**Done in e12 (2026-11-20).** Slots 0 and 1 return one buffer (four buffers, not five). The inference is not
needed: the dispatcher renders at `g_fx_pos`, which only `coop_fx_init_mono` changes, and that function now forgets
what both positions had initialised (`g_cur[0] = g_cur[1] = -1`, which also makes the screen's `view->live` false for
the other slot), so the restart is forced by coop's own code whatever the stock does [C in the code]. The stock does
construct the effect on every synth / drum change as well [C op1emu: `det_fx_fickle.txt`, `p0` and `p1` two inits
each across synth, drum, synth, drum, on e11 and e12 alike]. Item 12 was answered differently from its options: the
budget charges one buffer of the largest size, up to three more are carved from what the arena has left, and a
position that gets none refuses a scratch effect cleanly (`docs/plugins.md` "e12").

### 10. Install speed

Evidence [C]: a `.coop` goes up as `PUT_DATA` messages of 64 bytes, each awaited before the next
(`web/src/core/usbmidi/coop.ts:50,156-163`); the device answers from the main loop and writes each piece to yaffs
(`coop_sysex.cc:242-253`). Recorded throughput ~1.5 KB/s (`docs/sysex.md`). The packed set is 368 KB (rings
113,716, petri 35,856, fickle 35,616, the rest 12-24 KB): about four minutes, rings alone 76 s.

The device already queues four messages (`QN 4`, `:38`) and drains the whole queue on each pass (`:376-379`); a
piece out of order is answered with the offset it expects (`:247`), which the client already treats as a resend
marker. Proposed: the host keeps three pieces in flight (no firmware change), then `QN` 8 and a window of six with
the next image. Expected ~3x [I]: the limit is the round trip, not the bytes. Smaller steps: 70 bytes a piece still
fits the 96-byte message (+9 %); writing to yaffs in 2 KiB runs instead of 64-byte calls. Compression would give
1.5-2.5x on the effects and 1.5x on rings (gzip -9: cs_hall 23,184 -> 11,579, rings 113,716 -> 77,493) but needs an
inflater on the device: not proposed. Needs the device (op1emu has no USB-MIDI): count `g_sysex_drop`.

### 11. rings memory

Evidence [C by reading, sizes I]: rings loads 666 KB, 572 KB of it `.bss`, the largest item in the budget.
- The reverb keeps upstream's `kSize` 32768 / `kBufferSize` 54400 (`src/reverb_fixed.h:32-33`) but runs at
  22.05 kHz, where its lines end at sample 10,816: `kSize` 16384 needs 27,200 samples. 54.4 KB, bit-exact (the
  kernel takes the alias offset as an argument).
- Six voices each hold eight string lines of 8.2 KB (`src/string_voice.h:109`, `src/string_fixed.h:199`): 393 KB,
  while a voice uses 8 / polyphony strings (`src/string_voice.cc:185`), at most ~16 at once. A shared pool: ~260 KB.
  This one touches the steal logic and needs the host tests extended.
- `kSineQ31[4097]`, 16.4 KB (`src/fixed_tables.cc:1071`), is quarter-symmetric: 12.3 KB of file, and FM's working
  set falls from 512 cache lines to 129.

### 15. Kernels still worth writing

All [I] until the simulator's `prof` targets are run; each is bit-exact if transcribed.
- **The half-band decimator and interpolator** (`tape_echo/src/halfband_bfin.S:43-102` and its copies): four
  instructions per tap pair, no parallel loads, one channel at a time. A stereo dual-MAC kernel (as the core's
  `coop_hb_fir2`, `coop_stereo.h:342-376`): ~2 k a block in each of tape_echo, cs_hall, cs_plate, ensemble. cs_hall
  and cs_plate also decimate L and R separately and then sum (`cs_hall.cc:215`, `hall.cc:759-777`): summing first
  saves ~1 k each, 1 LSB different.
- **rings FM** (`src/fm_voice_fixed.cc:153-203`): the only model without a kernel; ~210 instructions a sample, a
  software loop, 12 stack accesses a pass. ~4 k a render.
- **rings modal geometry**: four 4 KB tables read at one index (`src/modal_resonator.cc:191-194`,
  `src/fixed_tables.cc:7,181,355,529`); one interleaved table saves ~3 misses a mode: ~3 k on a 10-mode strike,
  ~19 k on a 60-mode one, device only. The generator is in te-op1-cfw.
- **rings string outside structure 0.24-0.815** falls back to a C loop (`src/string_fixed.cc:521-633`).
- **rings verb after a clear** runs the C loop for ~99 blocks (`src/reverb_fixed.cc:129-135`): ~7 k a block for
  the first 143 ms of a note after a gap; lazy clearing keeps the kernel.
- **fickle's diffusers** (`fickle.cc:558-581`, four calls): ~1.4-1.9 k.
- tape_echo's five glue passes into two (`tape_echo.cc:195-204`, `src/tape_echo_core.cc:450,469-472`): under 1 k.

### 16. Boot

Evidence [C]: on the first frame after the scan `build_fx_all` constructs a screen object and parses the SVG of
every effect that has one (`engine_screen.cc:260-273`): 11 files in the present set, 118,896 bytes of the TE heap
for nine (`docs/device-tests.md` "e9"). `drop_unloaded_rows` runs two SELECTs on the factory database for every
name in the map that did not load, on every boot, and the map never forgets a name (`plugin_host.cc:249-271`, 64
entries). `row_ensure` runs one SELECT per loaded plugin (`:210-238`); `tanks_boot` takes and zeroes 234 KiB
(a delay line and a spring tank) before anyone selects those effects (`:825-832`), though since p5f the twins are
built on the main loop, where a lazy take is safe.

No boot time is recorded anywhere, so no estimate: put CYCLES stamps at the scan's start and end and around
`build_fx_all` into the stats first. Then: build a screen on its first show; mark a dropped name in the map so its
rows are not queried again.

---

## C. Behaviour changes and owner decisions

### 3. Measuring code in the release image

Evidence [C]:
- Every voice render goes through `coop_eng_pf_start`, `coop_eng_cost` (two marks and a record) and a mark in the
  hook (`stereo_thunks.S:35-68`, `coop_stats.cc:123,180-193`, `stereo_bus.cc:347`). The monitor is programmed at
  every boot (`plugin_host.cc:667`), so on the device each of these reads two MMRs.
- Every block start folds the records into four tables in SDRAM, then scans all 63 engine slots of `g_ecost`
  (40 bytes each, 2.5 KB, 79 cache lines) for the ones that rendered (`engine_host.cc:166-181`), most blocks none.
- `CoopAudioCb` writes marks into `g_mk` at about a dozen sites a block (`coop_cost.h:81-92`).

Device [C, m1c second run, `docs/m1-results.md`]: "the price of v2's folds: `cb_rest` is 5-6 k a block higher in
every section", and the block start alone was 4.5 k on the device against 0.2 k in op1emu (p5g, `01>02`) before
the engine tables existed. The per-voice path ("thunk") is 10.5 k a block for six voices in the measuring image.

Proposed: one word in L1, "profile", off by default, on from the settings menu or the SysEx stats request; with it
off the thunk skips the three calls and the block start skips the folds. The scan becomes a walk of the engines
that rendered (at most six). Gain: 5-6 k a block playing [C], plus the per-voice share and 2-4 k idle [I]. The
decision: the librarian's stats page shows the cost table only after the switch is on.

**Done in e14 (`docs/performance.md` "e14" 2).** `g_coop_prof`, the policy token `prof=0|1`; dev packages on,
release packages off (the owner's decision). Off skips the per-voice calls, every mark and cost entry, the
block-start fold and the counter reads; the 63-slot walk now runs only after a plugin engine rendered, switch or
not. The stats' `mode` line says which.

### 6. Idle paths in the effects

Evidence [C by reading]: none of the heavy renders tests for silence: tape_echo (`tape_echo.cc:174-213`), cs_hall
(`cs_hall.cc:200-222`), cs_plate (`cs_plate.cc:97-118`), fickle (`fickle.cc:871-1068`), petri (`petri.cc:119-162`,
`src/petri_engine.cc:928-1025`); rings exempts the verb model from its asleep path, so the shared reverb runs while
any verb key is held (`rings.cc:386-390,414`, `op1/reverb_bus.h:114-124`). The core cannot do it for them: it does
not know an effect's tail.

An effect left on a position costs its whole figure for the session: 15-20 k a block each (`docs/plugins.md`
table), two or three positions is 10 % of the budget on silence. Proposed per effect: after N blocks of zero input
with the wet under a threshold, advance only pointers and LFOs and pass the dry. Exact-zero tests rarely fire (the
hall's dither, the echo's loss filter hold 1 LSB), so it is a threshold with hysteresis: not bit-exact, a tail cut
below about -96 dB. fickle's can be exact (its line holds true zeros). The decision is whether that is acceptable.

### 12. What sets the five buffers

Evidence [C]: each buffer is the largest `"scratch"` of the loaded effects (`plugin_host.cc:473,598`). Declared
now: tape_echo 134,784, fickle 134,784, petri 134,752 (working tree), cs_hall 86,752, cs_plate 77,664, ensemble
10,400, the other five under 5 KB. tape_echo's is 33,217 int16 stereo pairs at 22.05 kHz for 1.5 s
(`tape_echo/src/tape_echo_layout.h:13-14`): already half-rate, 16-bit, one ring.

Options, each on top of items 1 and 7:
- Cap the three large ones near cs_hall's size (tape_echo ~1.0 s: scratch 90,688): 5 x 44 KB = 220 KB. A shorter
  echo, fickle loop and petri record.
- Size classes: five small buffers (10,400) and two or three large ones handed out on selection; a third or fourth
  large effect at once is refused with a message. 5 x 10.4 + 2 x 134.8 = 322 KB against 674 KB.
- The two reverb tanks are stored twice for the no-wrap kernels (`cs_hall/hall_layout.h:53-56`,
  `cs_plate/plate_layout.h:35,61`): real wrapping gives 86,752 -> ~48.8 KB and 77,664 -> ~44.2 KB, a kernel rewrite
  with a CPU cost; it pays only once the large three are smaller.

If item 1 is done first the pressure is gone and none of this is needed for the present set.

### 13. The master suite before it is switched on

Evidence [C]: `coop_master_process` is compiled in and off (`master_suite.cc:49,80`). Its compressor computes
`(target << 31) / env` in 64 bits per sample per channel when over the threshold, and its drive `g3 / 3`
(`core/include/coop_master.h:51,30`); `coop_master_apply` holds ten libgcc call sites (disassembly). `__divdi3` is
1.1 KB of bit loop. The recorded 18.5 k a block is the transparent case (threshold at full scale: no divide).

Proposed before any package enables it: the gain once per block (or a reciprocal table of the envelope), `/ 3` as a
multiply by `0x2AAAAAAB`, the biquad's five 64-bit products as MACs. Measure `coop_master_process` compressing in
the GNU simulator first; the figure is unknown.

### 17. The tape cache

Evidence [C]: 24 nodes, one `MALLOC(0x80000)` each, 12 MiB of heap 0's 16 (`docs/tape-stereo.md` §1 and §7.1; the
count is the stock loop bound at `0x01A54200`, the size the patched instruction at `0x01A54172`). Each node holds
all four stereo tracks of one 0.74 s chunk whether or not the tracks hold anything; 24 nodes are 17.8 s of tape.

Not proposed as a change: the tail guard and the held saves depend on the depth (`tape_io.cc:321,390-408`), and item
1 removes the need. For the record: each node dropped is 512 KiB; per-track nodes (lanes only for tracks in use)
would halve the cache for a two-track project but means replacing the stock lookup. Related, cheaper [I]: a node
load reads all four tracks in 32 passes (`tape_io.cc:302-304`); a per-track "ever recorded" bitmap would let it
skip empty tracks and load a one-track tape four times faster.

Also unmeasured [I]: the play mix runs the stock lane loop over eight lanes where each lane has a zero gain on one
side (`docs/tape-stereo.md` §7.1), twice the multiply-adds a stereo track needs. The share of the tape-rolling block
(15 % of the budget, T4a) that is this loop is not recorded; measure with the gap profile before considering a
kernel.

---

## Looked at and found efficient

- **L1 data**: the 8,416-byte pool and the 16 KiB sine copy are full and well spent: the sine table took 15 points
  off a six-voice FM chord on the device (T4c); the stereo and tape state in L1 cut the voice hook 4.5x (T4a).
  There is no free L1 code: the stock fills the 48 KB.
- **The core's kernels**: voice accumulate, the dual-MAC decimator and the S3 stage, the mix loops, the 44.1 kHz
  repeat, and the nine rewritten stock loops are hardware loops with state in registers. The per-voice path is one
  contiguous 1.7 KB section (v1 / v2). Idle second slots and the transparent master return on one test (p6j).
- **Effect construction and the heap** are off the audio interrupt (p5f, u8).
- **Flash writes besides the stats**: settings, FX 2 and the number map are written on change only
  (`coop_settings.cc:202`, `coop_ui.cc:266`, `plugin_host.cc:708`); the master save waits 0.5 s after the last turn
  (`:1151-1165`); the factory rows are read before they are written (`:228-231`).
- **Tape I/O**: 16 KiB parts per main-loop pass, only dirty tracks flushed, the hold and the tail guard.
- **Plugins**: every heavy sample loop has an assembly kernel except those in item 15; coefficient work is gated
  on change in rings, cs_hall, cs_plate, drive, wavefolder, ensemble, freq_shift; tank clears are spread over
  blocks; no 64-bit multiply or divide in any per-sample loop. Effect state is int16 and half-rate where it is
  large. SVGs are 5-7 KB and live in the TE heap, which has 3 MiB free.
- **Code residency** (the hot / cold split with cache priority) was built, failed on the device and was withdrawn
  (p6c / p6d); not re-proposed. The kernel-only page stays parked as the docs leave it.
- **`attic/`** (tracker, codeprio; 133 KB of source) is in no Makefile and no image.

## Not assessed

- Anything on the device or in op1emu: no run was made. Every [I] estimate above names its measurement.
- The audio interrupt's stack depth. The supervisor stack is ~4 KB of L1 (`memory-map.md` §3); a plugin render adds
  512 bytes of core block plus its own frames, and nothing records a high-water mark. A canary word in the stats
  would settle it.
- Boot time (no figure exists) and the real cost of an SVG parse.
- The device cost of fickle, petri and saw (their READMEs: device figures pending); fickle's render is one 14.4 KB
  function, over the 12 KB rule on paper.
- Whether the stock uses heap 1 (`0x03040000`, 15.7 MiB) to a degree that leaves room there; untraced.
- The packed 0.9 rings and the e10 plugin objects: only test builds were on disk, so instruction counts for the
  plugins are indicative.
- `tools/` and the web build.
