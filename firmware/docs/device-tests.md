# Device tests

coop-fw packages are built here and flashed by the owner. Every test package:
- leaves out `te-boot.ldr`;
- passes `tools/verify_bootloader.py`;
- decodes to stock firmware 246 with only `OP1_vdk.ldr` changed (`tools/pkg_check.py`).

Recovery always works the same way: hold COM at power-on, choose "1. Upload firmware", and copy stock
`op1_246.op1`. That path runs before any app code (`docs/brick-safety.md`). Proven on the owner's unit
2026-10-14 (te-op1-cfw Test 1).

## Protocol

Each test gets a numbered section below: package name, what changed against stock (blocks, patched words),
the procedure as numbered steps, what to read back (screen page, beep sequence, file on the disk-mode drive),
and the result as numbered points matching the steps. Numbering continues from te-op1-cfw's device tests, which
ended at Test 63.

How results come back without JTAG:
- **screen**: a readout page drawn with the stroke font (white click pages, as te-op1-cfw cpuprobe);
- **file**: written to `/yaffs2/user/` by a main-loop hook, copied off in disk mode;
- **beeps**: `te-op1-cfw/tools/decode_probe_beeps.py` on a phone recording;
- **photo**: a phone photo of the screen when a number must be exact.

## Test 64: L1 dumper (Phase 1)

**Package:** built from `tools/dump_l1/` injected into region C; see `tools/dump_l1/README.md` for
the exact build, inject, package and verify commands, and the on-device read-back.

What changed against stock (confirmed by `pkg_check.py`): only `OP1_vdk.ldr`, and inside it only the
main-loop tape-service CALL at `0x019F0A24` (retargeted to the payload's `ServiceHook`, stock target
`0x01A54710` re-checked by `inject_app.py`) plus the appended region-C code/FILL blocks. The package
passes `verify_bootloader.py` (no `te-boot.ldr`).

**Procedure (owner):**
1. Prove stock recovery once (COM at power-on, flash stock `op1_246.op1`).
2. Flash the dumper package through the te-boot menu.
3. Boot normally, wait a few seconds.
4. Disk mode, copy `user/coop_l1.bin` off.
5. Reflash stock.

**Read back / HARD GATE (offline):**
- `l1_decrypt.py check-key` with the key bytes from `coop_l1.bin` must print a matching arrangement.
- `l1_decrypt.py compare` of the decrypted CALLBACK blocks against the `0xffa00000` region in the
  dump must report **zero mismatches**. Phase 2 does not start until it does.

**Run 1 (2026-11-01): HANG on the loading screen.** Two bugs, both fixed:
1. The dumper read L1 through MDMA0 (`0xffc00f00`), which is the firmware's display-copy engine
   (memory-map.md 5; confirmed: MDMA0 registers have many references in the disassembly, MDMA1 none).
   Seizing it mid-boot broke the display DMA, hence the hang. Fix: the minimal build no longer uses
   DMA at all (L1 data and the key are read by ordinary loads); only the optional `DUMP_L1CODE`
   build reads L1 instruction SRAM, and it uses MDMA1 with a bounded wait.
2. `ServiceHook` was declared `(void* self, int arg)`, but the stock tape service takes one `int` in
   R0 (kennel's `STOCK_SERVICE` is `int(*)(int)`). The forwarded call passed the wrong registers.
   Fix: `ServiceHook(int arg)`, forwarding `((int(*)(int))0x01A54710)(arg)`.

**Run 2 plan (one thing at a time):**
- `op1_246_coop_canary.op1`: the hook only forwards to the stock service (72 bytes). Boots cleanly =>
  the CALL-retarget hook and signature are sound.
- then `op1_246_coop_dump.op1`: DMA-free capture of the key + L1 data, writes `coop_l1.bin`.
- then `op1_246_coop_dump_otp.op1` / `_full.op1`: add the OTP (boot ROM) and L1 instruction SRAM
  (MDMA1) once the minimal path works.

**Resolution (2026-11-01):** the Phase 1 gate was met a different way, without run 2. The OTP key is
the same on every OP-1, so the public reference dump (Tolsi/op1dumps) is valid for this unit;
`l1_decrypt.py check-key` verified it against kennel's `l1_patches.json` pairs, and the full L1
decrypts to real code (see `docs/l1-map.md`). The dumper builds (canary/minimal/otp/full) remain
available for an optional device-vs-emulator cross-check but are not needed for the gate.

## Phase 2 milestone C1/C2: stereo instrument bus — device test (PASS, 2026-11-02)

Built by `tools/build_device_package.sh` (sha256 in `build/pkg/`, copies in `C:\Users\byron\Downloads`):
- `op1_246_coop_c1_spread0.op1` — the regression image: must sound and behave exactly like stock
  (every stock path runs; coop adds hooks that do nothing at spread 0). Emulator: sample-identical.
- `op1_246_coop_c1_spreadfull.op1` — `g_coop_spread=32767`: each synth note lands at a random stereo
  position (headphones/line out; the speaker sums to mono by design), drums too per voice.

Both: no `te-boot.ldr`, `verify_bootloader.py` PASS, `pkg_check.py` = only `OP1_vdk.ldr` differs,
L1 re-encrypted with the check-key arrangement and round-trip verified (`tools/build_image.sh --device`).
Read `docs/brick-safety.md`; flash spread0 first.

**What to record:** boots to the tape screen; synth + drums play; no clicks/pumping under a 6-note
chord with an insert FX on (loop B replacement + hooks in the audio path); headphones: spread0 mono
image as stock, spreadfull voices spread L/R and total loudness unchanged; UI fps unchanged (perf gate,
`docs/performance.md`). If the device is silent or hangs: the mix thunk's register save set
(`stereo_thunks.S`) is the first suspect, then the region-C trampoline at `0x01e00000`.

**Result (2026-11-02, owner):** `op1_246_coop_c1_spreadfull.op1` (sha256 `6f100465...`) flashed first (the
owner's call). Boots, plays, \"sounds great\" [C]: the stereo instrument bus, the pan-at-note-on hook
(region-C trampoline at `0x01e00000`, `Core::NoteOn` callers patched), the replaced mix loop B, the
LFO2 pre-render thunks (slot off) and the coop payload at region A all run on the device without
silence, hang or audible artefacts. This is the first coop audio-path image confirmed on hardware.
Not yet recorded: the spread0 regression image on the device (emulator: stock bit for bit), a
measured fps/perf check (`docs/performance.md`), and a six-voice chord with an insert FX under
headphones for clicks. Those stay on the list for the next flash.

## Phase 3 milestone T2: stereo tape — device test (pending, 2026-11-02)

`build/pkg/op1_246_coop_t2_stereo_tape.op1` (copy in `C:\Users\byron\Downloads`): the C1/C2 image plus the
stereo tape (`docs/tape-stereo.md` sections 7-8): 8-lane cache, stock record function twice per block,
coop chunk I/O and a coop tape image (four stereo AIFC tracks, no album).

**WARNING: the first boot reformats the tape.** The coop mount finds no `coop_tape.txt` marker and
rebuilds `tape_c.raw` sparse (a few seconds): every tape recording and the album are gone. Back the tape
up with co-op (or disk mode) first. Going back to stock later makes stock rebuild its own image on its
first boot (minutes of zero-fill, the stereo takes gone); nothing else is touched, no factory reset.

Checks: no `te-boot.ldr`, `verify_bootloader.py` PASS, `pkg_check.py` = only `OP1_vdk.ldr` differs,
L1 round trip verified. Emulator: record + power cycle + play keeps the stereo take (section 8.1).

**What to record:** boots to the tape screen (first boot: a pause while the image is rebuilt); record a
spread chord on track 1, STOP, power off/on, play: the take is there and stereo on headphones; the tape
meters move; disk mode shows `tape/track_1..4.aif` as stereo files that open in a DAW; no dropouts
while recording with 4 tracks playing (the 128 KiB flush per main-loop pass is the thing to watch).

**Result (2026-11-02, owner): FAIL, device recovered by the T3 package.** Flashed; boots, the first boot
rebuilt the tape; synth auditions in stereo on the synth page but **mono on the tape page** (the tape page
monitors the instrument through the tape path; coop does not stereo-ise that path yet); **recording to tape
is stereo**; **lift took one side** (expected at T2: lift/drop/erase still copy the stock mono range);
**switching to track 2 crashed** with `ReadInterface.cpp:471 ch >= 0 && ch < kTapeNumTracks` and the device
then asserted at every boot. Cause [C]: the T2 record hook set the track word `0xff802964` to 4-7 for its
right-lane pass; the tape screen draw (`0x1a5c984 -> 0x1a29a74`, which asserts) and `tape_db_save` read
that word from the main thread and on the device they can see that window (the emulator never did), so a
track of 4-7 was drawn and saved to `tape.db`, which then asserted on every boot. Fix (T3 package): the
right-lane pass no longer touches the track word (node data pointers moved by four lanes for the pass,
interrupts masked), and a saved track >= 4 is clamped at mount, at every audio block and at every main-loop
I/O step before the tape screen can read it.

## Phase 3 milestone T3: stereo tape fixes — device test (pending, 2026-11-02)

`build/pkg/op1_246_coop_t3_stereo_tape.op1` (copy in Downloads). Recovery from the T2 assert loop: flash it;
the clamp runs before the tape screen reads the track. If the device still asserts at boot, the fallback is
the bootloader factory reset (tape and user presets lost) and a fresh flash.

Changes since T2: lane order L 0-3 / R 4-7 (same files on disk); right-lane pass through node pointers (no
track word writes); held saves while recording (one 128 KiB flush per second per chunk instead of one per
main-loop pass: 12 flushes instead of 418 in the emulator take); the loop-seam crossfade for eight lanes
(**untested**: the emulator scenarios set no loop; if loops misbehave or crash, report it and avoid loops);
track clamp. Still mono: lift/drop (one side), the instrument monitor on the tape page.

**What to record:** boots without the assert; record stereo, power cycle, play (stereo); a loop (in/out)
around a stereo take plays without a click or crash; lift still takes one side (known).

**Result (2026-11-02, owner): boots** (the assert loop is gone: the clamp works on the device). Stereo
record/play after a power cycle, loops and the rest of the list still to be reported.

## Phase 3 milestone T3c: stereo edits, tape-page monitor, loop seams — device test (pending, 2026-11-02)

`build/pkg/op1_246_coop_t3c_stereo_tape.op1` (sha256 6b0adf63..., copy in Downloads). Same tape image as
T2/T3 (no reformat). Supersedes T3b (never flashed): T3b plus the loop seam fix (`docs/tape-stereo.md`
section 11; the T3b build would have hung in the first loop pass).
New since T3: lift/drop/erase keep both channels (section 10); the instrument monitor on the tape page is
stereo; mix loops A and B replaced with the register fixes (base image sample-identical to stock on the
record and tape-page scenarios); loops around a stereo take verified in the emulator (five passes, width
kept, no seam click).

**What to record:** stereo monitor on the tape page; record a spread chord, lift it, drop it elsewhere,
play: both sides present; a loop around the take (sound keys 1 / 2 on the tape page) plays round with both
sides and no click; shift+erase (whole tape) works; no clicks or hangs with four tracks playing.

Result (2026-11-02, the owner): **PASS** for the tested items. The stereo image auditions on the tape page;
recording, lifting and dropping work. **Observed:** the tape animation gets choppy while a chord is recorded
(performance; see `docs/performance.md`, T3c section). Loops were not reported. A per-voice placement check of
the recorded take (the owner heard the whole chord panned as a block on the older T3 package) is still to do
with the tape file (`docs/tape-stereo.md` section 12).

## Phase 3 milestone T3e: stats file + split node loads — device test (pending, 2026-11-02)

`build/pkg/op1_246_coop_t3e_stereo_tape.op1` (copy in Downloads; sha256 in the commit). Supersedes T3d (not
flashed). T3c plus: the stats file `/yaffs2/user/coop_stats.txt` (`docs/performance.md`, "The device stats
file") and node loads split into eight passes of 64 KiB (`docs/performance.md`, T3e), the one main-thread
change aimed at the choppy tape animation while recording. Same tape image, no reformat.

**What to do:** boot, let it sit 10 s, record a chord from the synth page while watching the tape animation
(is it smoother than T3c?), stop, play it back from the tape page, stop, wait 15 s. Then disk mode (shift+COM),
copy to Downloads: `user/coop_stats.txt` (the `user` folder of the disk-mode drive, where `coop_l1.bin` was) and `tape/track_1.aif` as `coop_track_1.aif` (about
63 MB; it bisects the per-voice question of `docs/tape-stereo.md` section 12). If possible also a line-out WAV
of the tape playback of that chord and of the same chord played live on the synth page.

Result (2026-11-02, the owner): **no `coop_stats.txt` in the disk root** (it was being looked for in the root; the file is under `user/`, as `coop_l1.bin` was) (whether T3e or still T3c was on the
device for this run is not recorded; T3c has no writer). The tape files were copied (`track_1..4.aif`). **Lag:**
recording with nothing playing is smooth; as soon as voices play, the tape animation lags. **Tape file:** the
recorded chord (32.0-42.5 s of track 1) has its partials at several distinct right/left ratios (families near
0.8, 1.5 and 2.3, each constant across a note's harmonics), so per-voice placement survives the record path on
the device (`docs/tape-stereo.md` section 12). The disk-mode export appends a clip table after the SSND chunk
(track 1: 4 entries, the empty tracks 1), not a coop artefact.

## Phase 3 milestone T3f: stats file v2 (build tag, early write, main-loop gaps) — device test (pending, 2026-11-02)

`build/pkg/op1_246_coop_t3f_stereo_tape.op1` (copy in Downloads; sha256 in the commit). Supersedes T3e. Same
audio and tape code as T3e; the stats file now names its build, is written once ~1 s after boot and then every
5 s while not recording, and its windows carry the average load, heavy-block count, voice-hook / side / record
counts, loads / flushes and the longest main-loop gap (`docs/performance.md`, T3f).

**What to do:** flash, boot, wait 10 s. On the tape page, with the tape stopped, play and hold a chord for
5 s (does the screen lag?). Then record 10 s of silence, stop. Then record the chord for 5 s, stop. Play it
back, stop, wait 15 s. Disk mode (shift+COM), copy `user/coop_stats.txt` (the `user` folder, not the root) to Downloads. If the file
is missing after that, say so: it then means the package did not take, not the stats logic. Note which of the
three steps lagged.

Result (2026-11-02, the owner): flashed (T3e before it). **Still no file**, and the `user` folder does not
exist on the disk-mode drive: yaffs is not exposed in disk mode (the drive is synthesised: `synth/`, `drum/`,
`tape/`, `album/`). The channel was wrong from T3d on. **Lag:** worse with the FM and Voltage models; with the
string model the animation looks fine. Disk mode is shift+COM while running (not after a power-off).

## Phase 3 milestone T3g: stats read back through the tape audio — device test (pending, 2026-11-02)

`build/pkg/op1_246_coop_t3g_stereo_tape.op1` (copy in Downloads; sha256 in the commit). Supersedes T3f. Same
audio and tape code; the stats text is now also written into the last 4 KiB of track 4's audio in the tape
image (`docs/performance.md`, T3g), which disk mode copies as `tape/track_4.aif`. No reformat.

**What to do:** flash, boot, wait 10 s. On the tape page with the tape stopped, hold a chord for 5 s with the
FM model (lags?), then with the string model (lags?). Record 10 s of silence, stop. Record the FM chord for
5 s, stop. Play it back, stop, wait 15 s (the last window has to close and be written while idle). Shift+COM
into disk mode, copy `tape/track_4.aif` to Downloads (63 MB). I run `tools/tape_tail_stats.py` on it. Note
which steps lagged.

Result (2026-11-02, the owner): **PASS for the channel**: `track_4.aif` copied, `tools/tape_tail_stats.py`
prints the stats (`docs/device-stats/t3g_2026-11-02_fm_chord.txt`; 22 tail writes, 0 errors, mismatch
counters 0). The first device cycle numbers, read in `docs/performance.md` ("T3g: the first device numbers"):
the audio callback peaks at **461,297 cycles = 79% of a block** (ceiling 75%; the emulator said 21%), the
voice hook costs up to 33,710 cycles per voice on the device (13x the emulator), the record tap and the mix
3-3.6x. The 12-window ring had been pushed past the recording windows by the time the file was written
(26 windows), so the recorded-chord windows are lost; the chord window that survived shows the UI's
main-loop passes dropping from ~600 to 371 per 5 s and a 138 ms main-loop gap. Lag by model (owner): FM
and Voltage worse than string. Next: milestone T4 (`docs/performance.md`).

## Milestone T4a: kernels, L1 pool, split flushes, stats v3, SysEx file channel — device test (pending, 2026-11-02)

`build/pkg/op1_246_coop_t4a_stereo_tape.op1` (copy in Downloads; sha256 in the commit). Supersedes T3g. The first
optimisation round from the T3g numbers (`docs/performance.md`, T4a) plus the SysEx file channel (`docs/sysex.md`).
No reformat. The stock Lander game's sounds are silent on this image (its L1 buffers hold coop's hot data).

**What to do:**
1. Flash, boot, wait 10 s. Tape page, tape stopped: hold an FM chord 5 s, then a string chord 5 s. Record 10 s of
   silence, stop. Record the FM chord 5 s, stop. Play it back, stop. Wait 15 s. Note which steps lag, and how it
   compares with T3g.
2. With the device still on and plugged in: open `tools/coop_sysex.html` in Chrome or Edge, pick the OP-1 port,
   click Hello (it should answer `coop protocol 1 build t4a`), then Read file with the default path
   `/yaffs2/user/coop_stats.txt`; Save as... into Downloads. If that fails, say what the page said, and fall back to
   shift+COM disk mode and `tape/track_4.aif` as before.
3. Leave the end of track 4 alone (the stats live in its last 46 ms).

Result (2026-11-03, the owner): **PASS for the SysEx channel**: `tools/coop_sysex.html` answered Hello and read
`/yaffs2/user/coop_stats.txt` off the running device (`docs/device-stats/t4a_2026-11-03_sysex.txt`; the last line
came back cut because the device rewrites the file every 5 s while the page reads it in 64-byte steps: T4b's page
restarts a read when the size changes). The owner recorded one section legato (one voice) and then re-recorded the
chord. Lag not reported separately. Numbers in `docs/performance.md`, "T4a on the device": the voice hook fell from
33.7k to 7.5k cycles worst and 2.8k average per voice; coop's stages are now about 8% of the block under a recorded
six-voice FM chord, but the callback averages 55% there (the stock FM engine plus the tape is the rest), and a
64 KiB flush half took 85-173 ms with the audio at that load. Next: T4b.

## Milestone T4b: loads and flushes in 16 KiB parts — device test (pending, 2026-11-03)

`build/pkg/op1_246_coop_t4b_stereo_tape.op1` (copy in Downloads; sha256 in the commit). Supersedes T4a. One change:
node loads and chunk flushes move 16 KiB per main-loop pass instead of 64 KiB (`docs/performance.md`, T4b), so
the longest main-loop stall under a heavy chord should drop from ~170 ms to ~45 ms. The SysEx page restarts a read
when the file changes underneath.

**What to do:** flash, boot, wait 10 s. Tape page: record the FM chord for 10 s (hold it), stop, play back, stop,
wait 15 s. Say whether the tape animation still hitches while the chord is recorded, and how it compares with
T4a. Then read `/yaffs2/user/coop_stats.txt` with `tools/coop_sysex.html` (Hello should say `build t4b`) and save
it to Downloads.

Result (2026-11-03, the owner): stats read over SysEx (`docs/device-stats/t4b_2026-11-03_sysex.txt`; the owner's
verdict on the animation not yet given). The main-loop side is fixed: the longest stall per pass under the recorded
FM chord is 26 ms (one 53 ms), the longest gap outside our step 17-26 ms, against 85-173 ms and 48 ms on T4a; loads
19-20 ms per 16 KiB. The audio side is unchanged, as expected: 51-55% average during the chord (also 51% with the
chord sounding and the tape only rolling), coop's stages about 10% of the block, one block over the budget
(613,634 cycles = 106%). Mismatch counters 0. Numbers in `docs/performance.md`, "T4b on the device".

## Milestone T4c: the stock sine table in L1 — device test (pending, 2026-11-03)

`build/pkg/op1_246_coop_t4c_stereo_tape.op1` (copy in Downloads; sha256 in the commit). Supersedes T4b. One change,
aimed at the stock engine's own cost: the firmware's 16 KiB sine table (`OP1_SINE_TABLE`, read per sample by the FM
and other engines) is copied into L1 data B at the first audio block and every stock reader (27 pointer constants)
is retargeted at the copy (`docs/performance.md`, T4c). The emulator cannot show the gain (no cache model); the
device can. If the FM chord's average load falls from ~53%, this was the stock engine's miss cost.

**What to do:** flash, boot, wait 10 s. Tape page: hold an FM chord 5 s with the tape stopped, then record the FM
chord 10 s, stop, play back, stop, wait 15 s. Then the same with the Voltage model if convenient. Say whether the
animation hitches, and whether anything sounds different (it must not: the table is a copy). Read
`/yaffs2/user/coop_stats.txt` with `tools/coop_sysex.html` (Hello: `build t4c`), save to Downloads.

Result (2026-11-03, the owner): FM, Voltage and DSynth chords recorded in that order; stats read over SysEx
(`docs/device-stats/t4c_2026-11-03_sysex.txt`, `l1 ... sine 1`). Reading the windows in that order
(`docs/performance.md`, "T4c on the device"): the six-voice FM chord averages **38%** of the block (51-55% on T4b),
Voltage 50-64% with one block at 105%, DSynth 47-59%. Mismatch counters 0; two blocks of 2.1 M cycles (3.7 blocks)
at engine switches, stock work. No sound difference reported.

## Milestone T4d: the Lander game unreachable — device test (pending, 2026-11-03)

`build/pkg/op1_246_coop_t4d_stereo_tape.op1` (copy in Downloads; sha256 in the commit). Supersedes T4c. One change:
the four-mode-key combo that enters the stock Lander game is swallowed (`core/sdram/lander_off.cc`), so the game
whose sound buffers now hold coop's L1 data can never run. Kennel's stub screen object was tried first and hung the
boot in op1emu, so the game's screen is still created at boot as stock does; it is simply unreachable. Verified in
op1emu (`tests/sim/det_lander_combo.txt`): boots, plays, the combo swallowed twice, no fault.

**What to do:** flash, hold synth + drum + tape + mixer together: nothing should happen (stock would start the
game). Then use the device as usual; the stats are read the same way (Hello: `build t4d`). No other change.

Result: _superseded by T4e before a run; test the combo there_.

## Milestone T4e: the decimator pair as one dual-MAC kernel — device test (pending, 2026-11-03)

`build/pkg/op1_246_coop_t4e_stereo_tape.op1` (copy in Downloads; sha256 in the commit). Supersedes T4d (its Lander
change included). One audio-side change: the two half-band decimators of the difference buses run as one kernel
(`docs/performance.md`, T4e), bit-exact (simulator check and the chord gate). Expected: the mix stage's average
per block down from ~20k by a few thousand cycles; nothing audible.

**What to do:** as T4d (the four-key combo must do nothing), then a recorded FM chord and the stats over SysEx
(Hello: `build t4e`). Say whether the tape animation hitches at all now, with FM and with Voltage.

Result: _superseded by T4f before a run_.

## Milestone T4f: 8 KiB I/O parts — device test (pending, 2026-11-03)

`build/pkg/op1_246_coop_t4f_stereo_tape.op1` (copy in Downloads; sha256 in the commit). Supersedes T4e (which it
includes: the decimator pair, the Lander combo swallowed). One change: node loads and chunk flushes move 8 KiB per
main-loop pass instead of 16 (`docs/performance.md`, T4f), halving the longest stall under a DSynth chord once more
(19-52 ms per pass on T4c). DSynth itself is the stock engine's own arithmetic (`docs/performance.md`, "DSynth is
compute-bound"); coop cannot make it cheaper, only keep the main loop's stalls short.

**What to do:** flash. Hold synth + drum + tape + mixer together: nothing should happen. Then on the tape page
record a DSynth chord for 10 s, play back, stop, wait 15 s. Say whether the animation hitches with DSynth now,
and with FM. Read `/yaffs2/user/coop_stats.txt` with `tools/coop_sysex.html` (Hello: `build t4f`), save it.

Result (2026-11-03, the owner): **FM is smooth while recording; DSynth is still a bit laggy, not unbearable.**
Stats over SysEx (`docs/device-stats/t4f_2026-11-03_sysex.txt`): the recorded six-voice DSynth chord averages
65-67% of the block (3,368-3,448 of 3,450 blocks over half the budget, worst 85%); coop's stages are ~50k of
that (8.6%: hooks 15k, record tap 17-20k, mix 17k), the stock engine ~57%. Main loop under it: 81 passes/s,
longest I/O step 25-44 ms, longest gap outside our step 19-53 ms. Mismatch counters 0; 26 loads, 26 flushes,
0 errors. The combo was not reported. Reading: DSynth with six voices leaves the main loop a third of the CPU
on stock too; coop adds nine points. `docs/performance.md`, "T4f on the device: where the lag sits now".

## Phase 4 milestone P1: plugin loader, FX host, SysEx file writes — device test (pending, 2026-11-03)

`build/pkg/op1_246_coop_p1_plugins.op1` (copy in Downloads; sha256 in the commit). T4f plus the plugin system's first
milestone (`docs/plugins.md`): `.coop` files in `/yaffs2/plugins` are loaded at boot and the first FX plugin drives
the master insert slot; the SysEx page can install and delete files (`docs/sysex.md`). Without a plugin installed
the audio is T4f's. `build/plugins/stereo_delay.coop` (copy in Downloads) is the test plugin: the coop stereo delay
(0.15 s slap echo on everything).

**What to do:**
1. Flash. Play: it must sound as T4f (no echo). Hello on `tools/coop_sysex.html` says `build p1`.
2. On the page, choose `stereo_delay.coop` under "Install a plugin", click Install plugin, wait for
   "Installed ... Power-cycle". Power-cycle. Play: every sound now has a 0.15 s echo (the plugin in the master slot).
   Read `/yaffs2/user/coop_stats.txt`: the `plugins` line must say `seen 1 loaded 1 failed 0`.
3. Put `/yaffs2/plugins/stereo_delay.coop` in the path field, click Delete file, power-cycle: the echo is gone.
4. If anything misbehaves after the install, delete the file (step 3) or, failing that, reflash.

Result (2026-11-03, the owner): installed over SysEx, power-cycled, **no echo heard**. Stats
(`docs/device-stats/p1_2026-11-03_sysex.txt`): `plugins seen 1 loaded 1 failed 0`, FX slot peak 8,166 cycles: the
file install, the loader and the plugin's render all work on the device; what is unproven is the master-insert
path (`coop_master_apply`, the getter stand-in before the stock level stage), which until now had only the
emulator's word. Open: a retest with a short pluck on headphones and on the speaker would say whether the echo is
absent or just quiet. P2 hosts plugins at the stock FX positions instead, the object path the stock master FX
takes, which is known to reach the output on the device.

## Phase 4 milestone P2: FX plugins at the stock positions — device test (pending, 2026-11-03)

`build/pkg/op1_246_coop_p2_plugins.op1` (copy in Downloads; sha256 in the commit). Supersedes P1. A loaded FX
plugin now appears **in the stock FX list under its own name** (synth, drum and master positions) and runs when
selected there; nothing plays until it is selected (`docs/plugins.md`, P2). `stereo_delay.coop` stays the test
plugin (install as for P1 if it is no longer on the device).

**What to do:**
1. Flash, power-cycle once more (the plugin's list row is written at the first boot that finds the file).
2. Synth page, key 4 (the FX page), shift + 4: the FX list should now end with "stereo delay". Pick it, play a
   short pluck: a 0.15 s echo. Pick "delay" or another stock FX: the stock effect, no echo.
3. Mixer page, the master FX list: pick "stereo delay" there too and play: the echo on everything, tape included.
4. Read `/yaffs2/user/coop_stats.txt`: `plugins ... mono_calls / st_calls / plugin_blocks` count the dispatches
   and the blocks a plugin rendered.

Result (2026-11-03, the owner): **PASS.** "stereo delay" selectable from the synth FX list: the echo is audible,
the first note in stereo as before, the repeats mono (equal loudness in both channels), which is the documented P2
limit (the insert gets L = R = the instrument's mono sum). On the master slot the echoes are true stereo: a hard-left
note gives hard-left echoes. So the list row, the filter-type dispatch and the stock master path all work on the
device. P2b (per-instrument side buses) is the fix for the synth slot.

## Phase 4 milestone P2b: per-instrument side buses, one instance per position — device test (pending, 2026-11-03)

`build/pkg/op1_246_coop_p2b_plugins.op1` (copy in Downloads; sha256 in the commit). Supersedes P2. The synth's and
the drums' per-voice placement now travel on separate side pairs, so an insert at the synth or drum position sees
its instrument in stereo and its stereo output replaces that instrument's side (`docs/plugins.md`, P2b); the mix and
the tape take the sum, so with no plugin selected nothing changes. Each plugin file is relocated once per position,
so the same plugin at two positions keeps two states.

**What to do:** as P2 step 2: "stereo delay" at the synth position, a hard-left note: the echoes should now be
hard-left too (and a hard-right note's hard-right). Then the drums with a panned kit through the same plugin at
the drum position. The master slot as before. Then play without any plugin selected: it must sound as T4f.

Result (2026-11-03, the owner, on P2/P2b): **the design is wrong at the instrument positions.** A stock filter at
the synth position lets the dry signal through: the per-voice side signal bypasses the mono effect and is added
back at the mix, so with any spread the unfiltered stereo part is heard. The owner's recommendation, adopted: a
native stereo instrument bus from the pan hook on, every FX slot stereo, the stock master effects (already stereo)
instantiated for the instrument positions, the mono buses dropped. `docs/stereo-bus-v2.md`. P2b is not retested.

## Stereo bus v2 step S1: stock effects in stereo at the instrument positions — device test (pending, 2026-11-03)

`build/pkg/op1_246_coop_s1_plugins.op1` (copy in Downloads; sha256
`d9f84f1d515fc29434f0b5bba00a009c53360274ffdab2051f76a8676790e8af`, rebuilt with the nitro page fix). Supersedes P2b. Every effect at the synth and
drum positions now runs on the instrument's whole stereo signal: a stock effect runs as its stereo (master) class,
a plugin as before (`docs/stereo-bus-v2.md`, S1). Emulator: with phone or spring at the synth position the side
signal is processed (correlation of the side between the two effects 0.994 on P2b, 0.185 on S1); with no effect
chosen the chord is P2b's within 2 LSB; the plugin path is sample-identical.

**What to do:**
1. Flash. Hello on `tools/coop_sysex.html` says `build s1`. Play with spread up: it must sound as before.
2. Synth page, FX list (shift + key 3): pick **phone**, then **punch**, then **nitro**, with a wide chord. The whole
   sound must be processed: no dry part leaking through on either side (the P2b fault). Headphones show it best.
3. Pick **spring**: the reverb should now be wide, not centred.
4. Pick **delay**: the echoes are centred. That is the stock delay's design (it sums L and R into one line, on the
   master too); "stereo delay" (the coop plugin, if installed) keeps the placement.
5. The drums with a panned kit through phone at the drum position: no dry leak.
6. Known limit, worth one try: grid or spring on the synth **and** the same type on the master at once interfere
   (they share one tank in the stock code). Any crash or hang is a fail; odd sound there is expected.
7. Turn the knobs on the nitro, punch and phone pages at the synth position: the dials must follow (fixed after the
   owner found nitro's blue and orange dials frozen on the first S1 build).
8. Stats: the `plugins` line has `stock_st_blocks` (blocks a stock effect ran in stereo) and `stock_st_made`.

Result: _not yet flashed_.

## Stereo bus v2 step S3: the stereo pair from the voice on — device test (pending, 2026-11-03)

`build/pkg/op1_246_coop_s3_stereo.op1` (copy in Downloads; sha256
`414eff29f87170773bcb29614adfc9cdde1ba2fd8a0a7fd49707f9e21cd04c64`). Supersedes S1 (includes it). Each voice goes into
one stereo pair right after its render; coop's stereo decimator and DC block replace the stock mono ones; in drum
mode the stock drum bus stage runs on each side (`docs/stereo-bus-v2.md`, S3). Emulator: at spread 0 the bus,
decimator and DC output equal the stock's sample for sample; full spread equals S1 within 1 LSB.

**What to do:**
1. Flash. Hello on `tools/coop_sysex.html` says `build s3`. Play synth chords and drums: the level and tone must
   be as on S1 (and as stock with the spread at 0). Drums especially: the same loudness and punch as before.
2. Drums with an effect (phone, punch) and the synth together while on the drum page: the whole sound goes through
   the effect, both instruments (the stock has one instrument effect, the selected instrument's, on everything).
3. A hard-panned drum hit: its level shaping (the stock drum bus stage) now acts on its own side only. Listen for
   anything odd in the drum dynamics with a wide kit.
4. Record to tape and play back: as S1.
5. Animation while playing a dense DSynth or FM chord: should be no worse than S1 (stats `stages ... stage_max`
   is coop's new instrument stage; send the stats file).

Result: _not yet flashed_.

## Stereo bus v2 S1 + S3 + S2 together — device test (pending, 2026-11-03)

`build/pkg/op1_246_coop_s2_slots.op1` (copy in Downloads; sha256
`48e6e2217afb7a9cdeb9ac5ae4ef46b27103b2f27a256e47f0b68508f048107a`). Supersedes the S1 and S3 packages and contains both
(the S1 and S3 entries above stay as their test plans). New in S2: a second effect slot after the instrument effect
and after the master effect, chosen on the host page (`tools/coop_sysex.html`, "Second effect slot"). Test plugin:
`stereo_delay.coop` (Downloads).

**What to do:**
1. Flash. Hello says `build s2`. Without touching slot 2 everything must sound as S3: run the S1 and S3 checks above
   (stock effects on the whole stereo sound, drums at the stock level, tape).
2. Install `stereo_delay.coop` if it is not on the device (Install plugin), power-cycle.
3. On the page: Read slots. Instrument: **delay (stock)**, Apply. Play the synth: the stock effect you chose on the
   OP-1 (e.g. phone) and then a delay behind it; with the OP-1's effect off, still the delay. Try **phone** or
   **punch** in slot 2 with a different stock effect in slot 1.
4. Master: **stereo delay (plugin)**, Apply. Play: a 0.15 s slap echo on everything, tape included. This is the coop
   master stand-in that P1's echo went through unheard: if this echo is missing too, say so (the instrument slot
   working while the master one is silent is the answer we need).
5. Power-cycle: both slots must come back as set (Read slots shows them; they keep playing).
6. Set both to none: the sound returns to S3's.
7. Stats: `plugins ... s2_instr / s2_master` count the blocks each slot rendered, `s2_changes` the changes taken.

Known: the stock delay's echoes are centred; grid or spring in two places at once interfere (one tank); slot 2 has
no page on the OP-1 yet and is the same for every sound.

Result: _not yet flashed_.

## Clean-ups after S2: voice sum off, own grid / spring memory — device test (pending, 2026-11-04)

`build/pkg/op1_246_coop_s2c_cleanup.op1` (copy in Downloads; sha256
`eb7cceb55c5e1854b5a39cf571630dae5bbce33e4ea5695a174ce9a38c6a68d3`). Contains S1 + S3 + S2 and supersedes the S2
package: test this one with the S2 entry above, then the two points below. Hello and the stats say `build s2c`.
(`docs/stereo-bus-v2.md`, "Clean-ups after S2".)
- The stock per-voice mono sum is patched out (unused since S3): nothing should sound different from S2.
- Every coop grid and spring object has its own reverb memory: the same one of those two types in two places at
  once no longer interferes.

**What to do (after the S2 steps):**
1. Play the synth and the drums with no effect and with phone: as on S2. Any click, level change or silence is a
   fail.
2. Grid at the synth position (FX list) and grid in instrument slot 2 and in master slot 2 (host page, Apply each):
   three clean grids, each with its own tail. Same with spring. On S2 these interfered.
3. Grid or spring on the master (the OP-1's master effect) and the same type in master slot 2: both clean. (This
   pair is not covered in the emulator.)
4. Expected once: a short glitch at the moment grid is applied to a slot 2 (its init clears 264 KB in the audio
   thread). Anything longer, or a hang, is a fail.
5. Stats: `plugins ... tanks` (bits: 1, 2, 4 grid at the instrument / slot 2 / master slot 2; 16, 32, 64 the same
   for spring) and `tank_short` (0 expected: a tank that did not fit). `stage_max` should be a little lower than on
   S2 with six voices playing.

Emulator (op1emu, old = S2, one job): chord and drums identical to S2 (drums after the boot), with the stereo bus off
too; tape record the same energy (one-block shift, as S3 to S2); the write tracer shows each grid / spring object on
its own tank and the stock tanks untouched after boot; no faults.

Result: _not yet flashed_.

## P3a: the FX 2 page on the OP-1, and the tape tail guard — device test (pending, 2026-11-04)

`build/pkg/op1_246_coop_p3a_fx2page.op1` (copy in Downloads; sha256
`ce9eec015379492904d9c602de3e47d369a208f2ecbd7dffb0c8c4e64a39fd44`). Contains everything above (S1, S3, S2, the
clean-ups) and supersedes the `s2c` package: run the S2 and clean-up checks on this one. Hello and stats say `p3a`.
New: the second effect slot's page (`docs/screens.md`), and the tape fix taken from hundekraft 3.1.4
(`docs/tape-stereo.md` section 13).

**What to do:**
1. Synth, T3 (the effect page, any effect). **Shift + Help**: INSTRUMENT FX 2 fills the screen. Turn the four
   encoders: the four bars and numbers follow, 1 per detent (if it moves 2 per detent, say so). Play keys meanwhile:
   the page stays up and the sound follows the knobs.
2. **Shift + blue**: the name steps through NONE, DELAY ... CWO, then the plugins, in blue with "PUSH BLUE TO SET".
   A **blue push** sets it: you hear it. With grid, one short glitch at the push is expected.
3. **Help** closes the page (so do Shift + Help, a mode key or T1-T4). The stock effect page is back unchanged.
4. On the master effect page, Shift + Help shows MASTER FX 2, which is the master's slot 2.
5. Power-cycle: the slot comes back as set (the page shows it; the host page's Read slots too).
6. Tape: record a long take (a minute or more, ideally on a fuller flash), play it back: no clicks, no silent
   stretches. Stats: `io ... guarded` counts the flushes the tail guard started.
7. Stats `plugins ... ui_opens / ui_applies`.

Emulator (op1emu, one job): the page opens, takes the encoders and applies DELAY, and draws cleanly over the phone
page; with the page never opened the chord, phone and nitro scenarios are sample-identical to the build before;
tape scenarios as before (play and loop identical; record and lift / drop the usual one-block take shift); no faults.
The tail guard cannot be exercised in op1emu (its flash is fast).

Result: _not yet flashed_.

## Performance: DSynth lag — device measurement (pending, 2026-11-04)

The owner on `p3a` (`coop_stats (5).txt`): with DSynth the UI became unbearably laggy, worst with cwo. The stats
agree: while a six-voice DSynth chord played the callback averaged 483-572 k cycles a block (83-99 % of 580,499;
3,359 of 3,452 blocks over the ceiling in window 19); the T4 builds spent 251-297 k on the same chord. FX 2 was on
in those windows (the mix step, which runs instrument slot 2, rose from 18 k to 65 k), and the record pass from 9 k
to 23 k. The rest (about 150 k) has no breakdown in that file. [C: the file]

`build/pkg/op1_246_coop_p3m_measure.op1` (copy in Downloads; sha256
`bc25d03aa5734105245d0ea7c7b7a9b9e2b0d3aa550029f4ca4a747d4dd6b83f`): p3a plus a `cost` line in the stats (average /
peak / calls for each coop stage, and slot 1's stock effect by type), and an "FX2" tag in the corner of an effect
page whose slot 2 is on. Build tag `p3m`.

**What to do** (each step about 15 s with a held six-note DSynth chord, no recording):
1. FX 2 off on both (open the page with Shift + Help, Shift + blue to NONE, blue push; no "FX2" tag), OP-1 effect
   off.
2. OP-1 effect cwo at the synth. 3. Phone instead. 4. Spring instead.
5. Effect off, instrument FX 2 = cwo. 6. Master FX 2 = spring.
7. Copy `coop_stats.txt`. The `cost` line then gives each part's cycles per block directly.

Result: _not yet flashed_.

## Performance 1: DSynth voice loop — device test (pending, 2026-11-04)

`build/pkg/op1_246_coop_p4a_dsynth.op1` (copy in Downloads; sha256
`7cc6379bfbe3b7a25e858e559c8bd01d78e939de4139374840fdba087a19739a`). p3m plus the rewritten stock DSynth voice
loop (`docs/performance.md`, step 1). Build tag `p4a`.

**What to do:** as the p3m measurement above (a held six-note DSynth chord, about 15 s each): effect off and FX 2
off; then cwo at the synth. Listen for any change in DSynth's sound (there should be none: the samples are
identical in op1emu), and feel the UI. Copy `coop_stats.txt`.

Expected: the six-voice chord about 45 k cycles a block cheaper than on p3m (more if the stock loop's stack traffic
missed the cache). [I until the file]

Result: _not yet flashed_.

## Performance 2-4: S3 stage and cwo in assembly — device test (pending, 2026-11-04)

`build/pkg/op1_246_coop_p4b_perf.op1` (copy in Downloads; sha256
`d7bf49312b65603c0b4e13c028b0c81a4ab4fe146f3b8d08b6de2c9933b78b06`). Supersedes p4a: the DSynth voice loop, coop's S3
stage in assembly and the stock stereo cwo loop in assembly (`docs/performance.md`). Build tag `p4b`. (The DSynth
oscillator rewrite was withdrawn before packaging: it was wrong for a gain that follows the previous sample.)

op1emu, six voices: DSynth alone 238 k -> 189 k emulator cycles per block (-21 %); DSynth with cwo at the synth
290 k -> 215 k (-26 %); the sound level the same second by second in both, faults none.

**What to do** (each about 15 s with a held six-note DSynth chord): effect off, FX 2 off; cwo at the synth; cwo in
instrument FX 2; cwo on the master. Listen for any change in DSynth's or cwo's sound against p3a / stock (there
should be none), turn cwo's knobs while it plays, feel the UI. Copy `coop_stats.txt`.

Result (2026-11-04, the owner, `coop_stats (7).txt`): **the UI feels much better; everything sounds as
normal.** Cost line (cycles per call, device): cwo at the synth 60.5 k (p3m 92.3 k, -34 %), cwo in FX 2 62.0 k
(92.5 k, -33 %), S3 stage 10.3 k (15.2 k, -32 %), master stand-in idle 1.8 k. Windows with a six-voice chord: FX 2 =
cwo 322 k a block (p3m 448 k), one cwo 305 k (p3m about 394 k); the chord alone about 245 k (42 %) by subtraction.
Not yet rewritten (same as before within run-to-run spread): punch 78 k, phone 49 k, spring 47 k, nitro 46 k, grid
36 k, delay 29 k. [C]

## Diagnostic 5g (p5g_diag): where an ordinary block's time goes — device run (done, 2026-11-05)

p5f's idle lead-in costs 164 k cycles per block on the device (cwo + the stage + the master account for 73 k; op1emu's
whole idle block is 45 k): about 90 k per block, every block, that no coop counter sees. p5g adds a gap profile to the
slow-block marks: every block, the time between each pair of consecutive marks is summed by pair (`prev>id`, ids in
`core/include/coop_cost.h`), so the gaps between coop's hooks, which are stock code or interrupts, get numbers. The
runner snapshots it per section: each section of the run file gets a `gaps` line (per block / average / max / count,
largest first), the stats file one since boot. Switched by `g_coop_gap_prof`, 1 only in this package (0 in p5g's
normal build); it runs after the callback's own timing, so the figures it reports are not changed by it.

- `build/pkg/op1_246_coop_p5g_diag.op1` (sha256 `6c758d9d9652b1df0fb4b2815c4e086bcbb7bda7ff8121e0e1d7ec9900201dd1`):
  p5f perf plus the gap profile. Passes `verify_bootloader` and `pkg_check`; copy in Downloads.

Steps: flash it, run the automatic test (`run_p5a.txt`; Hello says `build p5g`), send the run and stats files. Then
flash p5f perf back for normal use (the profile costs a few k cycles per block of audio-interrupt time).

op1emu (`RUN_EXTRA="--set-word g_coop_gap_prof=1" tests/sim/run_emu.sh tests/sim/run_check.txt ...`): the idle block
splits into mix entry -> master 16.7 k, FX 2 master -> tape tap 14.0 k, the stage 7.4 k, the rest small; no fault.

**Result (owner, 2026-11-05): the gap profile ran** (`docs/device-stats/p5g_diag_2026-11-05_*`; no fault). The idle
lead-in (168 k cycles per block, op1emu 45 k), per block:

| gap | what lies between the marks | device | op1emu |
|---|---|---|---|
| `34>50` | after the mix entry up to coop's master stand-in: stock code (the master effect, if one is on; the tape-track and mixer stages) | 55.3 k | 16.7 k |
| `44>34` | the instrument effect render (here cwo, slot 1) | 48.4 k | |
| `4b>38` | after FX 2 master up to the tape tap: the stock level stage, the SPORT conversion | 17.5 k | 14.0 k |
| `38>03` | coop's tape record tap (~7.5 k per block while not recording, the stats windows' `rec_sum`) and the stock tail (USB audio push, MIDI drain, key scan) | 14.1 k | 3.3 k |
| `30>44` | coop's S3 stage | 13.6 k | 7.4 k |
| `02>30` | the stock callback before the stage (voice loops with nothing playing) | 10.6 k | 2.3 k |
| `01>02` | coop's block start | 4.5 k | 0.2 k |

`34>50` is 55-64 k in every section of the run, the largest single item after the voices and the instrument effect.
The owner (2026-11-05): nitro was the master effect (main slot; FX 2 master none), so `34>50` is mostly that effect (nitro ~45 k on the device) plus the stock mixer: a choice, not overhead. With this, nothing large is unaccounted for in the block; the owner is satisfied with the performance work to here.

## Performance 5f (p5f): no effect construction in the audio interrupt — device test (PASS, 2026-11-04)

**Result (owner, 2026-11-04): PASS.** No slow block in the run or after it (`slow count 0`); the drwave section's
worst block 1.99 M -> 308 k, pulse 485 k -> 298 k. 66 twins built on the main thread, 3,044 dry blocks: about 46
blocks (~70 ms) per change on the device (op1emu: 1.3), so an effect comes in ~70 ms late after a sound change. One
797 k block early in the session (below the profiler's 1 M threshold); FX 2 was changed three times then, and FX 2
still builds in the interrupt. Effect costs as p5e; instrument FX 2 was off in this run (callback averages ~40 k
lower for that reason). No recording (ASIO interface). Files: `docs/device-stats/p5f_2026-11-04_*`.

p5e's profiler put every long block inside `st_make` (below). In p5f the audio interrupt only posts the type of the
stereo twin it needs, with a sequence number; the main loop (`coop_fx_service`, every pass) builds the twin and
publishes the number it built; the render uses the twin only while that number is the latest, and passes the
instrument through dry until then. FX 2 still builds in the interrupt when it is changed (rare: the page or the
on-device FX 2 page); the same change can follow.

- `build/pkg/op1_246_coop_p5f_perf.op1` (sha256 `02bffbb312d5a6a388fb67f422f9c905bee462948be46dae3955264de4fe5781`).
  **Flash this one.**
- `build/pkg/op1_246_coop_p5f_safe.op1` (sha256 `29e5d25e56d38277d673b41968c114a4afe62c218b0bf4d47f9df36c9abb055f`).

Both pass `verify_bootloader` and `pkg_check`; copies in Downloads. op1emu (`run_p5a.txt` both ways): no fault, levels
equal, every effect section ran its effect as before (punch 8144 ... delay 10414 renders), 52 twins built on the main
thread with 68 dry blocks in all (about 1.3 blocks, 2 ms, per change), the Voltage section's worst block 169 k ->
130 k.

Steps: flash p5f perf; switch sound slots s1..s8 by hand several times and listen for dropouts; select delay once
(clean, as in p5e); run the automatic test; send `coop_stats.txt`. Expected: `slow count 0` (or only blocks without
`42@...43`), `st0_builds` counting the effect constructions. Recording: the interface is ASIO-only, so untick
"record audio" and record channels 5/6 in the DAW while the test runs; `tools/run_compare.py` takes any WAV that
holds the runner's start and end clicks.

Result: pending.

## Performance 5e (p5e): the delay crackle fixed, the slow-block profiler, the cache table — device test (done, 2026-11-04)

p5d plus three things:
- **Delay crackle:** coop's own stereo delay / cwo line (176 KB) came from the heap uncleared; the stock init never
  clears a line (stock's lines start zero from boot). Slot 1's line is now made and zeroed at boot on the main
  thread, slot 2's when it is chosen (main thread), and the audio-side fallback zeroes too.
- **Slow-block profiler:** every coop hook in the audio callback leaves a timestamped mark; a block over 1 M cycles
  keeps its marks, and the stats file prints `slow count`, `slow_worst` and the last four `slow_recent` timelines
  (`id@cycles` from the callback start, `!` after a gap over 200 k; ids in `core/include/coop_cost.h`). The ~2 M-cycle
  blocks after sound-slot changes do not happen in op1emu (`tests/sim/det_slot_switch.txt`: worst 169 k), so only the
  device can say where they are.
- **Cache table:** `cplb`, `dcplb`, `icplb` lines (the CPLB registers) and `buffers` (where slot 1's line, spring tank
  and object live). op1emu shows the firmware's set-up: heap 0 (the effect buffers) and coop's code cacheable,
  write-back.

- `build/pkg/op1_246_coop_p5e_perf.op1` (sha256 `82ea77b836b52c83d1038f035067560f46c9f3167c3d45b91f9d7f7dc41bdf81`).
  **Flash this one.**
- `build/pkg/op1_246_coop_p5e_safe.op1` (sha256 `77e9f34214ccbc92441b40551bc39dd6171a583dcfe4f738cfea859d0326c937`):
  p5a's eight switches at 0.

Both pass `verify_bootloader` and `pkg_check`; copies in Downloads. op1emu: `run_p5a.txt` both ways, no fault,
levels equal, callback +0.2 k per block for the marks.

Steps:
1. Flash p5e perf. Select delay on a sound for the first time and play: no crackle.
2. Run the automatic test (`run_p5a.txt`; Hello says `build p5e`), then switch sound slots by hand a few times (s1..s8),
   noting any dropout you hear.
3. Read `coop_stats.txt` (or keep the test's `_stats.txt`) after that and send it: the `slow` lines locate the long
   blocks.

**Result (owner, 2026-11-04): the profiler found the dropouts.** Files `docs/device-stats/p5e_2026-11-04_*`. Four
slow blocks (1.97-1.99 M cycles; one in the test's drwave section, three while the owner switched slots by hand), all
with the same timeline: `40@~20k 42@~22k 43@~1.90M! 41`. That is `st_make`, coop's stereo twin of the stock effect,
built inside the audio interrupt from the stock's effect init when a sound loads: ~1.88 M cycles, the stock init of
the stereo class clearing the effect's buffers in SDRAM. Fixed in p5f (the twin is built on the main thread). The
device's cache table matches op1emu's: heap 0 and coop's code cacheable, write-back (`dcplb` entries `0x0003109f` /
`0x0003109d`). The page lists no inputs of the owner's interface: it is ASIO-only (input on channels 5/6), which
browsers cannot open.

## Performance 5d (p5d): the spring crash fixed (second try) — device test (PASS, 2026-11-04)

p5c crashed exactly as p5b did (below). p5d takes the P-register stores out of spring's two accumulator-shift
packets.

- `build/pkg/op1_246_coop_p5d_perf.op1` (sha256 `f48a1844d7dfc01383bdaaeb38b06813bc966172ce7a9881191c7e476032f059`):
  everything on. **Flash this one.**
- `build/pkg/op1_246_coop_p5d_safe.op1` (sha256 `a77282a63690ac841a43a817433745b0b640a54367885fc0016363095c036b72`):
  p5a's eight new switches at 0. Only if perf misbehaves.

Both pass `verify_bootloader` (te-boot.ldr absent) and `pkg_check` (only OP1_vdk.ldr differs); copies in Downloads.
Steps as p5c's below: by hand through the FX list first, then the automatic test (Hello says `build p5d`). If
**List inputs** does not show the interface, send the status line and the device list it prints
(docs/test-runner.md, "The page cannot see the audio interface").

What changed: in p5c the crash packet was `A1 = A1 >> 16 || [FP - 0x64] = P0 || NOP` (the load already moved out),
same RETX `0x02C03868`, SEQSTAT `0xC022` (EXCAUSE 0x22), so slot 3 was not the cause. What the stock code never does
is pair a parallel P-register store with a shift: its ~860 parallel P stores sit beside ALU or multiply instructions
only, and GCC's own MAC + P-store packet in coop_master_apply has run on the device every block since the first
build. Spring had two such packets (`A1 = A1 >> 16`, `A1 = A1 >>> 15`); both stores are now plain instructions
(+2 cycles per sample). `tools/asm_lint.py` flags P stores beside shifts (both packets in p5c, nothing in p5d).
Lockstep (`-DCOOP_SPRING_CHECK`, det_fx_stock_spring_knobs): 0 differences in 8,713 calls.

Risk left: the other seven p5a rewrites have never run on the device (the test always stopped at spring). They use
some slot combinations the stock never does, all with D registers (none is a P store); two of those kinds already
run in the p4b cwo rewrite. A crash photo's RETX names the packet, as it did here.

**Result (owner, 2026-11-04): PASS, stable, no crash.** The automatic test ran to the end (`state 3`, 1,554 events,
15 sections, none lost), the sequencer stayed off. No recording came back (three text files only; the interface
question is open). Files: `docs/device-stats/p5d_2026-11-04_{run,stats,stats_after}.txt`.

Device costs per call (cycles of the 580,499 block) against the stock figures measured on the device in p3m (six-voice
DSynth, `docs/performance.md`) and the op1emu saving:

| | device p5d | device stock (p3m) | device change | op1emu change |
|---|---|---|---|---|
| punch | 71.5 k | 77 k | -7 % | -26 % |
| phone | 30.2 k | 43 k | -30 % | -51 % |
| nitro | 44.8 k | 44 k | 0 % | -18 % |
| grid | 28.8 k | 32 k | -10 % | -42 % |
| spring | 38.8 k | 45 k | -14 % | -51 % |
| delay | 23.2 k | 24 k | -3 % | -34 % |
| cwo (p4b) | 62 k | 92 k | -33 % | -48 % |
| S3 stage (p4b) | 11.3 k | 15 k | -25 % | |

The p3m figures were not taken under the same knob settings (punch's cost grows with its stage count), so the rows
are rough. **The p5d safe run of the same script (owner, 2026-11-04, `docs/device-stats/p5d_safe_2026-11-04_*`)
gives the exact device figures, rewrite vs stock under identical input:** phase 7.5 k vs 16.3 k per voice render
(-54 %), phone 30.2 k vs 46.6 k (-35 %), grid 28.8 k vs 37.1 k (-22 %), spring 38.8 k vs 46.7 k (-17 %), delay
23.2 k vs 25.4 k (-9 %), punch 71.5 k vs 76.5 k (-7 %), nitro 44.8 k vs 46.3 k (-3 %); dsynth (both p4b) 21.5 k /
21.3 k. The callback averages are not comparable between the two runs (the sounds' effects and FX 2 differed at the
start of each). The safe run also had a ~1.99 M-cycle block in the drwave section: the long blocks are not the
rewrites'. The owner heard dropouts, and on the first delay selection "a loud crackling that echoes out": coop's own
stereo delay line came from the heap uncleared (fixed in p5e). Engines per voice render on the device:
dsynth 22.9 k, drwave 16.0 k, digital 10.2 k, fm 10.0 k, sampler 9.8 k, phase 7.5 k (op1emu: 18.6, 9.6, 7.1, 5.9,
6.9, 5.8 k).

What the file also shows:
- The owner's sound slots are not all factory: s2 is FM (the Voltage section played fm), s3 the sampler. The Voltage
  rewrite has therefore not run on the device yet.
- Instrument FX 2 is on (`s2_instr` 36 k per block in every section): about 6 % of the block on top of everything.
- Three blocks of about 2.04 M cycles (3.5 blocks long, an audible dropout) in the heaviest windows, two of them in
  the run (the Voltage and drwave sections, both right after a sound-slot change) and one before it. To investigate.
- Callback per block, six-voice DSynth with no effect: 320 k average, 438 k peak (55 % / 75 %, FX 2 included).

## Performance 5c (p5c): p5b with the spring crash fixed — device test (FAIL, 2026-11-04)

**Result (owner, 2026-11-04): FAIL.** Selecting spring (or scrolling past punch) crashed again: SEQSTAT 0xC022,
RETX `0x02C03868`, the same packet with the load moved out (`A1 = A1 >> 16 || [FP - 0x64] = P0 || NOP`); see p5d.
The page did not list the audio interface (microphone permission granted); p5d's page reports what the browser sees.

p5b crashed when the FX list reached spring (below). p5c moves the offending load into its own packet; the build now
runs `tools/asm_lint.py`, and the test script closes the FX list with the synth key instead of SEQ + synth.

- `build/pkg/op1_246_coop_p5c_perf.op1` (sha256 `33f8a0884f48aa8909ebd96d86feccd79458bf0b5b009071309bb9be5ae4278b`):
  everything on. **Flash this one.**
- `build/pkg/op1_246_coop_p5c_safe.op1` (sha256 `6c0aeb4c6a0b06aefc5e861508b6f6a3bff827f960b2ea1bfe08d677c9a0ad06`):
  p5a's eight new switches at 0. Only if perf misbehaves.

Both pass `verify_bootloader` (te-boot.ldr absent) and `pkg_check` (only OP1_vdk.ldr differs); copies in Downloads.

Steps:
1. Flash p5c perf. **By hand first:** on s5 (DSynth), open the FX list (shift + T3) and scroll slowly through every
   effect to the bottom and back, a chord held; then pick spring and play. This is the move that crashed p5b.
2. Then the automatic test exactly as p5b's steps 2-6 below (Hello must say `build p5c`; script
   `tests/sim/run_p5a.txt`, the new version). The sequencer must stay off throughout.

op1emu (2026-11-04, the new script on the fixed image, `run_emu.sh ... p5c`): every effect section ran its effect
(punch 8144, phone 8364, nitro 8474, grid 8584, spring 9756, delay 10417 renders), no fault, A and B levels equal
(RMS 344.9 / 344.9). The effect sections' callback average fell from ~147 k to ~113 k cycles: the SEQ tap had started
the sequencer in op1emu too. Spring costs 13,662 cycles per call (13,598 before: the one added cycle per sample).

Result: pending.

## Performance 5b (p5b): the p5a image plus the test runner — device test (FAIL, 2026-11-04)

p5b is p5a (below) plus the test runner (`docs/test-runner.md`). The page plays the p5a test on the OP-1, records it
from your interface and fetches the results, so the device and op1emu get the same inputs. **Flash p5b instead of
p5a**; the p5a steps below stay as the manual fallback.

- `build/pkg/op1_246_coop_p5b_perf.op1` (sha256 `f6e756b19e2b4b07a463e790f9b72437465754dd9c0be49f0f6fc93d3a2f8c46`):
  everything on. **Flash this one.**
- `build/pkg/op1_246_coop_p5b_safe.op1` (sha256 `62299fb35d1ec2f18c06a78d3c80b4e23dfac5eaf71674d8e6fc929a865383f2`):
  p5a's eight new switches at 0, the runner in. Only if perf misbehaves.

Both pass `tools/verify_bootloader.py` (te-boot.ldr absent) and `pkg_check` (only OP1_vdk.ldr differs). Copies in
Downloads.

Steps:
1. Flash `op1_246_coop_p5b_perf.op1` through the te-boot menu. Play a few notes by hand: it should behave as p5a.
2. Wiring: OP-1 USB to the computer; the OP-1's headphone or line out into the audio interface input. Set the OP-1
   volume to where you usually play, and the interface gain so the loudest chord peaks well below clipping (the
   runner's start click is about half as loud as the loudest music).
3. The test assumes the factory sound slots on the synth page (s1 digital, s2 voltage, s3 pulse, s4 drwave, s5 dsynth,
   s6 fm, s7 phase, s8 sampler). It changes s5's effect (it ends on spring) and turns effect knobs, so save any sound
   you care about first. The drum part plays whatever kit is loaded.
4. Open `tools/coop_sysex.html` in Chrome or Edge, pick the OP-1 port, press **Hello**: it must say `build p5b`.
5. **Run a test**: choose `tests/sim/run_p5a.txt`, press **List inputs** (allow the microphone permission), pick
   the interface input, press **Run test**. It takes about 3 min 15 s. Hands off the OP-1; watch the screen: the FX
   list should scroll and the effects change as in op1emu. **Stop** ends it early and releases every key.
6. Save the three files it offers (`p5a_<date>.wav`, `_run.txt`, `_stats.txt`) and send them over. I compare the
   recording with op1emu's run of the same script (`tools/run_compare.py`) and read the per-section costs.

What to report besides the files: anything that sounded wrong, a key that stayed down, the page stopping with an
error, or the screen doing something the script did not ask for.

**Result (2026-11-04, owner): FAIL, a crash in the spring rewrite.** The runner worked: it played the test until the
first effect change. Two faults:
- **The sequencer came on.** The script closed the FX list with SEQ then the synth key (a habit of the op1emu
  scenarios); on the device SEQ starts the sequencer. Fixed in the script: the synth key alone closes the list (the
  owner: T1-T4 or the synth key close it).
- **Crash, SEQSTAT 0x22 (illegal instruction combination), RETX `0x02C03868`**, when the FX list scrolled past punch.
  Also reproduced by hand. The address is `lp_entry` in `spring_fast.S`, the first packet of spring's sample loop:
  `A1 = A1 >> 16 || [FP - F_W9] = P0 || R1.H = W[I2]`. A P-register store in slot 2 beside a load in slot 3: the
  device refuses it, op1emu runs it (op1emu raises 0x22 only for slots that fail to decode). The stock code has about
  860 parallel P-register stores, all with NOP beside them. The row after punch is spring, whose preview runs that
  loop at once; that is why "scrolling past punch" crashed. Call stack on the screen: `CoopAudioCb` (0x02C0003E) ->
  the stock callback -> the FX render; the frame at 0x02C0E458 is `coop_ui_frame` on the UI side.
  Fix: the load moved to its own packet (one cycle per sample); `tools/asm_lint.py` flags the pattern (it finds only
  this packet in the p5b sources). The lockstep build (`-DCOOP_SPRING_CHECK`, det_fx_stock_spring_knobs) gives
  0 differences in 8,933 calls. Re-test in p5c.

op1emu (2026-11-04): `tests/sim/run_emu.sh tests/sim/run_p5a.txt tests/sim/idle_200s.txt`. The runner played all
1,566 events and 15 sections with no fault. Each effect section shows its effect: `fx1_punch` n 8144, `fx1_phone`
8364, `fx1_nitro` 8474, `fx1_grid` 8584, `fx1_spring` 9756, `fx1_delay` 10417; the small counts of other effects are
the list previewing rows while it scrolls. Engine costs per render as p5a: voltage 8391, phase 5818, dsynth 18612,
digital 7100, pulse 16502, drwave 9641, fm 5867, sampler 6874. Level A (op1emu input) vs B (runner): RMS 453.5 vs
455.4. `run_compare.py` on a simulated recording (gain 0.4, 0.7 s late, noise, +100 ppm clock) found both clicks, got
+100 ppm, and gave 0.0 dB in every section.

Result: see above (FAIL, fixed in p5c).

## Performance 5 (p5a): six effects and two engines in assembly, per-engine cost — device test (pending, 2026-11-04)

Two packages (copies in Downloads), build tag `p5a`; the stats file's new `switches` line says which one ran.

- `build/pkg/op1_246_coop_p5a_perf.op1` (sha256 `b29c7732742d13d6ae1ae429ebb1618a900e0cade2567f9b7a274d0edcd286f8`):
  everything on. **Flash this one.**
- `build/pkg/op1_246_coop_p5a_safe.op1` (sha256 `f914fae7b9d734857e4c8a96b2a742d58bce0e6890b9dadf8ef1a5c34d0c4f8a`): the
  same image with this run's eight new switches at 0 (p4b's DSynth / S3 stage / cwo rewrites stay on). Only if the
  perf package misbehaves: it tells a p5a rewrite apart from anything else.

Both pass `tools/verify_bootloader.py` (te-boot.ldr absent) and `pkg_check` (only OP1_vdk.ldr differs from stock).
Supersedes p4b. What is new (`docs/performance.md`, "p5a: what the run changed"; op1emu per call, stock -> fast):

| Switch | What | op1emu |
|---|---|---|
| `g_coop_fast_punch` | punch, both channels in one pass | -26 % |
| `g_coop_fast_phone` | phone's two loops | -51 % |
| `g_coop_fast_nitro` | nitro's two loops | -18 % |
| `g_coop_fast_grid` | grid's sample loop | -42 % |
| `g_coop_fast_spring` | spring's sample loop (segments between line wraps) | -51 % |
| `g_coop_fast_delay` | delay's sample loop | -34 % |
| `g_coop_fast_voltage` | Voltage's voice loop | -42 % per voice |
| `g_coop_fast_phase` | Phase's voice loop | -43 % per voice |

Every one is sample-identical to stock in op1emu or, where op1emu's run-to-run variation splits runs, passed a
lockstep build (stock and rewrite on the same state every block / voice, 0 differences). Also new: the stats file's
`engines` line (cycles per voice render for each engine, and `synth=` / `drum=` per block), 24 time windows (was
12), the heaviest windows first, and the `switches` line.

**What to do** (each about 15 s with a held six-note chord; DSynth is factory sound 5, Voltage 2, Phase 7):
1. Voltage, no effect; then Phase, no effect. Listen for any change in the sound; turn the four engine knobs while
   it plays; feel the UI (Voltage was the laggy one).
2. DSynth with each stock effect at the synth in turn: punch, phone, nitro, grid, spring, delay (and cwo for
   comparison). Turn all four effect knobs while it plays. Listen for clicks, dropouts, a changed tone or level,
   anything stuck. spring and delay: let the tail ring out after releasing the chord.
3. One effect at the master as well (spring), and one in FX 2 (punch) if time allows.
4. Play a few other engines briefly (FM, DSynth, a drum kit) so the `engines` line ranks them on the device.
5. Copy `coop_stats.txt` over SysEx (`tools/coop_sysex.html`); the tape tail keeps only the first 8 KiB.

**Device-only questions** (op1emu cannot answer them): the real saving per effect (op1emu counts instructions, not
stalls; p4b's rewrites gave about two-thirds of their op1emu saving on the device); spring's segments enter its
hardware loop mid-body (legal on the BF524, proven in op1emu only); delay keeps one stock packet that loads one half
of a register while multiplying into the other (kept exactly as stock, so it behaves as stock whatever the hardware
does).

Result: (pending)

## UI 1 (u6): settings, FX 2, list columns, randomise, solo, markers, bend, overwrite save, rename, drum pan — device test (DONE 2026-11-07, u5 flashed: see the u7 entry)

The first package of the owner's UI feature set (`docs/ui-features.md`; mocked first in
`tools/playground/coop-playground.html`). Build tag `u6` (the stats file's `build` word; u1-u4 were earlier cuts of
the same package, never flashed; u5 = u6 without the drum pan, flashed 2026-11-07). Everything from p5g stays in.

- `build/pkg/op1_246_coop_u6_ui.op1` (sha256 `baa05379dd4945052d9b5cd3a3dc0978f16fd6825a488618d4a8bbab29ee2f1c`),
  copy in Downloads. `--tape-stereo --set-word g_coop_spread=32767` as p5f. `verify_bootloader.py` PASS (no
  `te-boot.ldr`), `pkg_check` = only `OP1_vdk.ldr` differs. **Flash this one**; p5f is the fall-back.

What is new (`docs/screens.md` "u1 / u2", "u3", "u4", "u5", "u6"):
0. **The stock font** on both coop pages (`core/sdram/coop_text.h`: the firmware's Text object, as kennel's chord
   plugin draws it), with the manifesto's colour code: the cursor row blue (blue moves it), the value green (green
   changes it), titles lavender, hints dim; on the FX 2 page the knob numbers in their encoder colours and a pending
   type in blue.
1. **Settings menu**: hold synth + drum + tape + mixer (the old Lander combo). Blue moves, right arrow enters, left
   arrow backs out and leaves at the top, a mode key leaves and acts. Green edits the value (green on screen). VOICES
   / SPREAD is live (0-100, it is the stereo spread of `g_coop_spread`: 100 at boot from the image word, the file
   overrides it afterwards); the other VOICES and UI rows show STOCK / BOTTOM / NOT YET and cannot change yet.
   PLUGINS lists every `.coop` file in `/yaffs2/plugins` with ON / OFF; an encoder push toggles a row, applied at the
   next boot. Saved to `/yaffs2/user/coop_settings.bin` when the menu closes.
2. **FX 2 by drop / lift**: on any effect page (synth, drum, master) **drop** focuses FX 2 (its page opens), **lift**
   focuses FX 1 (it closes). With FX 2 focused, **T3** (T2 on the mixer) switches FX 2 off and on: `FX2 OFF`. The
   focus is remembered per position: leave with T1 and come back with T3 and the slot last touched is shown. The
   stock effect page shows `fx2` (blue) or `fx2 off` (grey) in its corner while a slot 2 is chosen. Shift + Help no
   longer opens the page. Lift / drop on an effect page no longer copy / paste the sound (other pages unchanged).
   Shift + blue on the page still chooses the type with a blue push to apply.
3. **The green column on the lists**: shift + T3 (synth, drum, and the mixer's master FX page) opens the stock FX
   list as before; its empty right half now holds coop's column, `none` then the same rows, in the stock's own
   green. **Green** moves it (four detents a row, as blue); the cursor starts on the slot's current choice. **T3**
   closes the list as stock does and, if green was turned, loads the green row into FX 2 of that position and puts
   FX 2 in focus (so the FX 2 page follows). Any other closing key (a mode key, T1 / T2 / T4) discards the green
   choice. Shift + T4: the same on the LFO list with `none`, `tremolo`, `value`, `random`: T4 loads it into **LFO
   2** (one LFO 2 for both instruments for now, its four parameters set to the middle of the stock ranges; no page
   for them yet). Both saved in `coop_slot2.bin` (now version 3).
4. **Randomise**: hold **synth** (or **drum**) and press **right**: the page shown gets random values inside the stock
   ranges (synth T1 engine knobs, T2 envelope A D S R, T3 effect knobs, T4 LFO; drum T2 dynamic envelope, T3, T4;
   the drum T1 page is per-voice sample data and is left alone; an effect or LFO page that is off is skipped). Hold
   the key and press **left**: back through the last five. The arrows do nothing else while the key is held (no
   octave change). A plain tap of synth / drum still goes back to T1 as stock; the tap now acts on the release.
5. **Help is fn**: a tap opens help as stock (on the release); a hold is the modifier. **Solo**: on the tape,
   Shift + Help + T1..T4 solos that track (the other three muted through the stock mute, the mixer page shows it),
   the same combo again restores the mutes as they were; `solo n` top right. Note: the live instrument is monitored through the
   current track, and the stock mute of that track silences it (op1emu: a stock Shift + T1 mute silences the live
   chord exactly as the solo of another track does), so soloing a track you are not on silences your playing until
   the solo is off. If that is not wanted, the solo could leave the current track unmuted: the owner's call.
6. **Markers**: on the tape page, Shift + an encoder push drops that colour's marker at the playhead (again with the
   playhead on it: removed; elsewhere: moved); a plain push jumps there (the loop is suspended as for the stock's
   own jumps). Carets in the encoder colours above the tape, a grey line through the tracks; saved in
   `/yaffs2/user/coop_markers.bin`, cleared by an erase.
7. **Per-sound coop data**: a `"coop"` member kept inside the sound's JSON through the stock's own loader (the
   keep hook), so it travels with slots, user presets, lift / drop; factory and old sounds get defaults.
8. **Bend amount and time** (shifted T2): hold Shift on T2: the stock page gains two rows, white **bend** (-12..+12
   semitones, default +12 = the stock range) and orange **time** (off / 1-127 / max, 5 ms .. 2 s). Shift + left /
   right bends as before, by the amount, over the time. Saved per sound.
9. **Overwrite save**: hold **synth** (or drum) + a sound key through the clock: "overwriting <name>" shows and the
   sound replaces its own user preset instead of making a new snapshot; a factory preset or an unsaved sound saves as
   stock. (A stray empty marker file lands in the snapshot folder; a sampler preset's audio is not re-copied.)
10. **Rename**: in the preset browser (Shift + a sound key) on a user preset, **orange push** starts naming: blue
   letters, green digits, white symbols, turn to pick, push to keep, push with nothing picked = space; orange
   anticlockwise deletes; orange push saves (the list shows the new name); any other key cancels. Factory and sampler
   presets are refused ("no" blinks).
11. **Drum per-voice pan**: drum T2 with Shift held: **green** pans the key last played, L50 .. C .. R50 ("pan 7 l50"
   in green); each key keeps its own, saved with the kit. The drum's spread is gone: a key plays where it is panned.

**What to do** (about 5 minutes):
1. Boot; play a chord (everything as p5f). Hold the four mode keys: the menu appears, no game. Release.
2. Blue one detent down (UI), back up (VOICES), right arrow. Green down ten detents: SPREAD 90, and a chord should
   narrow a little (spread is random per note, listen on headphones). Green back up. Left, left: the menu closes;
   the synth page is back; play. Hold the combo again: right arrow into VOICES: SPREAD still 100 (the file).
3. Combo, blue two detents down (PLUGINS), right: the installed `.coop` files (ON). Push blue on one: OFF. Left,
   left. Power-cycle: the stats `plugins` line should say `disabled 1`, and that plugin is missing from the FX list.
   Combo again, PLUGINS, push: ON. Left, left (and a power-cycle later brings it back).
4. Synth, pick phone with shift + T3, T3 to the effect page. **Drop**: the FX 2 page. Shift + blue one detent, blue
   push: DELAY. Green up: echoes on top of the phone. **T3**: `FX2 OFF`, the echo stops; T3: back. **Lift**: the stock
   phone page with `FX2` in the corner. Drop, then T1, then T3: the FX 2 page comes back by itself. Lift, T1, T3: the
   stock page. Mixer T2 (master FX): the same with drop / lift / T2.
5. **The lists**: synth, T3, shift + T3: the two columns. Blue picks phone (loads at once, as stock). Green two
   rows down (delay), T3: the FX 2 page opens with delay; play: phone + echoes. Lift, T3 (the phone page), shift +
   T3 again: the green cursor should sit on delay. Green to `none`, T3: the corner `fx2` mark goes, the echo stops.
   Then shift + T3, green to grid, **T1** instead of T3: nothing changes (discarded). Mixer, T3 (master FX page),
   shift + T3: the same for the master. T4, shift + T4: green to tremolo, T4: the sound should wobble (LFO 2 at
   mid-range); shift + T4, green to none, T4: it stops.
6. **Randomise**: synth T1, play; hold synth + right: the knobs jump, the sound changes; again; hold synth + left
   twice: back to the loaded sound. T2, hold synth + right: the envelope changes (the page must stay on T2 while
   synth is held). T3 with an effect on, T4: the same. Drum: T2 / T3 / T4 the same; T1 does nothing. A plain synth
   tap from T3: back to T1 as stock. Save the sound and power-cycle: it must load (the values stay inside the stock
   ranges; a failure is the "Invalid synth patch" boot stop, which p5f's package recovers from by re-flashing).
7. **Solo and markers**: tape, play a bar on tracks 1 and 2. Shift + Help + T2: only track 2 plays; the mixer page
   shows 1, 3, 4 muted; Shift + Help + T2 again: all back. Help tapped alone: help, as stock. Shift + blue push:
   a blue caret under the playhead; move away, blue push: the playhead jumps back; Shift + blue push on it: gone.
   Four colours. Power-cycle: the markers are still there. Erase the tape: gone.
8. **Bend**: synth T2, hold Shift: bend +12, time off. Hold Shift + right while playing: an octave up (the stock
   bend; confirm the interval: the semitone scale of the stock's bend word is inferred). Set bend to +2, time to 60:
   Shift + right glides up a tone over ~0.1 s. Set bend -12: Shift + right goes DOWN an octave. Save the sound, change
   slot and back, power-cycle: the values stay.
9. **Overwrite**: hold S3 through the clock (a normal save: a new snapshot preset). Tweak a knob; hold synth + S3
   through the clock: "overwriting" and the tick; the browser shows one preset for it, with the tweak. Load a
   factory sound, hold synth + S4: a normal save (a new snapshot).
10. **Rename**: Shift + S3 opens the browser; find the snapshot preset; orange push; type a name; orange push:
   the list shows it. On a factory preset: "no".
11. **Drum pan**: drum, hit a key, T2, hold Shift: green to L50; hit it: hard left (headphones); another key:
   centre. Save the kit, power-cycle: still left.
12. Copy `coop_stats.txt` over SysEx: the `plugins` line gains `disabled`, `ui_toggles`, `ui_col2_sets`,
   `set_opens`, `set_saves`, `rnd`, `rnd_back`, `solo`, `markers`, `jumps`, `help_taps`, `mode_taps`, `mode_holds`,
   `keeps`, `kept`, `sound_writes`, `bend`, `bend_knobs`, `bend_ramps`, `overwrites`, `saves`, `renames`, `drumpan`.
13. The font: the menu and the FX 2 page should read like the stock lists (op1emu draws them so; the device is the
   proof), with no "te_new out of memory" on screen changes (each coop text object is built once).

op1emu (2026-11-07, `tests/sim/det_ui_settings.txt`, `det_ui_fx2.txt`, `det_lander_combo.txt`, and for u3
`det_ui_fxlist.txt`, `det_ui_lfolist.txt`, `det_ui_fxlist_t3.txt`): no fault in any run; the FX list shows the green
column, delay applied by T3 opens the FX 2 page, the chord plays through phone + delay, the reopened list starts on
delay and green moves it to grid; the LFO list the same with tremolo (the chord's level rises: mean 412 against ~190
without), reopened on value; closing the list with T3 leaves FX 1 on (no FX OFF on the phone page); u4
(`det_ui_random.txt` at spread 0): the chord's level 316 -> 263 after the T1 randomise, 317 after the step back, 533
after the T2 randomise with the page still on T2, T1 after a plain synth tap; `det_ui_tape.txt`: two markers set
and saved (35,016 and 70,030 samples), the jump lands on the blue one, `solo 2` drawn; `det_ui_help_tap.txt`: a
Help tap opens the stock screen and the chord after it sounds; `det_ui_tape_solo.txt` / `det_ui_mute_stock.txt`:
with track 2 soloed the live chord on track 1 is silent (RMS 63, the floor) and back at 203 when the solo is
switched off, and the stock Shift + T1 mute gives the same numbers, so that is the stock's monitoring, not the solo; u5 (`det_ui_bend.txt`): the shifted T2 page shows the two coop
rows (bend -8 after 20 detents down from +12, time 40), the chord sounds through a 2 s bend, no fault;
`det_ui_save.txt` (kept NAND, op1.db read back): a stock save makes one snapshot row, a knob is turned, then synth +
the sound key through the clock UPDATEs that same row (one row, the turned knob in it, the name unchanged; the hook's
trace markers 1-7 complete); an earlier cut hung the guest in the stock's db exec because a LIKE pattern's `%` goes
through its printf (now GLOB); `det_ui_rename.txt`: after a stock save the browser opens on the snapshot preset, the orange push starts naming, two
characters deleted, 'a' typed and kept, orange push saves: the row reads `20640101_00a` (`renames 1`); an earlier cut
refused factory presets as designed; u6 (`det_ui_drumpan.txt`, spread 0): a key hit at centre measures L 75 / R 75,
after green 50 detents down on the drum's shifted T2 it measures L 85 / R 63 (63 is the floor: hard left), another
key L 90 / R 90; the kit's row carries `pan[7] = -50`; the keep hook runs on
every sound load (`keeps` 209 at boot) and after the bend edit the slot's row in op1.db carries
`"coop":{"bend":-8,"bend_t":40,"pan":[...]}`;
chords sound after the menu closes (peaks ~2,600 RMS). With the image at spread 50 the menu shows 50, green ten
detents gives 60 on screen and the saved file reads `CST 1 3c` (60 %), no disabled names (the pristine NAND has no
plugin files: the PLUGINS page says so). The FX 2 scenario: drop opens the page, delay applied at knob 2 = 10, T3
twice (off, on), lift closes, T1 then T3 stays on the stock page, drop + T1 + T3 brings the page back.

**Device-only questions**: whether the four-key combo is comfortable to hold (the keys must all be down together);
whether T3 closing the list toggles FX 1 on the device as the key path reads (op1emu says no);
whether the stock copies the sound on lift before our hook sees the key (it should not: the hook runs first);
the file writes on the device's yaffs (slot 2's did in p3a).

Result: (pending)

**Result (owner, 2026-11-07, u5 flashed; files `docs/device-stats/u5_2026-11-07_*`):** the lists' blue and green
columns work and the focusing works; the settings menu looks right; randomise is great; the markers and solo work;
`keeps 948 kept 1`, `bend 16`, `overwrites 1`, `rename_refused 4`. Found: a crash (SEQSTAT 0x2a) leaving the settings
menu after a plugin toggle (a yaffs write from the KeyHandler thread); FX 2 should look like the stock effect page,
no corner marks; no hints in the menu, a blue push should enter; Shift + synth must act on the press; solo wanted
on the mixer too and as a set, no text; markers as flags; the bend bent an octave slowly whatever the amount (the
stock word is a rate); overwrite did nothing outside the snapshot folder; rename refused (sampler or factory
presets: `rename_starts 0`); the drum pan wanted its own PAN page on the drum's shifted T2 with blue. The p5a runner
script ran on this image too (`u5_2026-11-07_p5a_run.txt`: engines and effects as p5f; one slow block of 2.08 M
during an FX 2 change, which still builds in the interrupt).

## UI 2 (u7): the corrections from the first device run — device test (pending, 2026-11-07)

- `build/pkg/op1_246_coop_u7_ui.op1` (sha256 `85cc21adadb1b4eae30fd06d98bcf01dc6b4c71cc21c99056c32f0951f2291c3`), copy in Downloads. `--tape-stereo --set-word
  g_coop_spread=32767` as before; `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr` differs. Build tag `u7`.

What changed (`docs/screens.md` "u7"):
1. **The crash**: every coop file and database write now runs on the main loop (a pending flag from the key thread).
2. **Settings**: a blue push enters a submenu (as the right arrow); no hints on screen.
3. **FX 2 looks like stock**: with a stock type in FX 2, drop shows the stock effect page of THAT type with FX 2's
   knobs and animation (the page's dial getters are fed slot 2); lift shows FX 1's page with nothing added. Only two
   additions on the FX 2 page: `fx2 off` top right when T3 switched it off, and the pending type (Shift + blue) in
   blue top left until the blue push applies it. A plugin in FX 2 keeps coop's own page (the stock has none).
   Known: the stock page's own big "FX OFF" is FX 1's flag and still shows on FX 2's page while FX 1 is off (op1emu;
   the flag's reader in the page is not mapped yet).
4. **Shift + synth** acts on the press as stock (the deferral only applies without Shift).
5. **Solo** is a set: Shift + Help + several T keys solo several tracks; on the tape and the mixer; nothing drawn.
6. **Markers** are little flags (a pole and a ">" pennant).
7. **Bend**: amount 0..12, the same both ways; coop drives the live bend word the MIDI handler writes, so the amount
   and the time both apply (op1emu, pitch-tracked: amount 2 gives x1.122 after the table fix; time 63 ramps in
   ~100 ms). Please listen: +12 should be an octave, +2 a tone; MAX time ~2 s.
8. **Overwrite** works for user presets in any folder.
9. **Rename**: unchanged in what it accepts; factory and SAMPLER presets refuse (a sampler's audio file is named after
   it: its rename needs the stock's file rename, not mapped). The stats say how many pushes it saw (`rename_pushes`).
   If your user presets are samples, say so and the file rename goes on the list.
10. **Drum pan**: Shift on the drum's T2 page shows coop's PAN page ("pan" and the value in blue, the key in grey);
    **blue** pans the last played key.

**What to do**: the u6 script's steps, plus: leave the settings menu after a plugin toggle (no crash); drop on an
effect page with a stock type in FX 2 (the stock page of that type, its dials moving with the four encoders); T3
(`fx2 off`); lift (the stock FX 1 page, nothing added); synth T2 + Shift: bend 12, time off: Shift + right = an
octave; bend 2, time max: a slow tone; the drum's T2 + Shift: the PAN page, blue pans the last key.

Result: (pending)

## Tracker 1 (t1): the tracker sequencer in place of the tape view — device test (pending, 2026-11-07)

- `build/pkg/op1_246_coop_t1_tracker.op1` (sha256 `ef4d86b2b3390ee1b6117c416b025d62ff4d93e19fcde6750f95f08ec6096092`), copy in Downloads. `--tape-stereo
  --set-word g_coop_spread=32767` as u7; `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr` differs. Build
  tag `t1`. Includes u7 plus the overwrite / rename fix (the row's JSON spelling: `"name":"x"`, `"name": "x"` and
  `"name" : "x"` all match now). If the overwritten sound still comes back unchanged, the next hypothesis is
  that the reload reads the preset FILE in the folder, not the db row coop updates: then the overwrite must rewrite
  the file the stock save writes (its path is in the save hooks, coop_save.cc).

What it is: `docs/tracker.md` (the first pass, with every guess marked). Tape mode, **Shift + tape** (or settings
→ UI → tape view) switches to the tracker: T1 overview, T2 zoom, T3 song, T4 setup; the keyboard writes notes,
blue / green / white / orange edit the cell, play / stop / rec the transport and edit flag, lift / drop / scissors
copy / paste / clear; setup's orange turns the screen (landscape, portrait a, portrait b).

**One family at a time** [C op1emu]: the core's play mode (`[0xFF801038]`) renders synth OR drum voices and asserts
on a note for the other family (`VoiceCtl.cpp:387`; the first t1 build died on it). The tracker plays the cells of
the live family (the header says `syn` / `drm`: the mode you were in before the tape) and skips the rest.

op1emu (`tests/sim/det_ui_tracker.txt`, `det_ui_tracker_p.txt`): no faults; four synth notes and three drum cells
written; play runs the 16-row pattern with the playhead bar following; the WAV repeats the pattern every 2.00 s
(16 rows x 125 ms at 120 bpm, speed 6: the clock is right on average; the row jitter is the main loop's pass
interval, to be heard on the device); the zoom, song and setup views draw; portrait a and b draw the stroke font
turned, b mirrored (the stock Text object takes only the translation of its matrix, docs/screens.md "t1"); the stock tape page (det_ui_tape) is untouched with the tracker off. Stats
`trk_rows 82 trk_notes 38` in the landscape run.

**What to do**: tape, Shift + tape (the tracker); write a few notes on the synth (the keyboard; the cursor moves
down); play (is the row grid even? listen for swing); T2, T3, T4; setup orange one detent (portrait a: turn the
device anticlockwise, the keyboard on the left), another (portrait b: clockwise); back; drum mode, tape: the drum cells play now (the synth ones are
skipped); Shift + tape again: the tape page comes back (the markers, solo as u7). Say what feels wrong in the
controls and the orientation: everything in `docs/tracker.md` is a guess for you to react to. The stats file
(`trk_*`) and the fps line say the cost of the drawing.

Result: (pending)

## Tracker 2 (t2): F1, a second engine at the same time — device test (pending, 2026-11-07)

- `build/pkg/op1_246_coop_t2_tracker_fm.op1` (sha256 `08d806639a205343d9cbaa2f399fe2530b200b0c9e74b2e2a8e5b86a2dd8116e`),
  copy in Downloads. Supersedes t1 (everything in it, plus the FM voice). Build tag `t1` still.

The question "can notes from different engines sound together" answered by rendering a stock FM voice in coop's own
block, outside the core's play mode (`docs/tracker.md` "F1", `core/sdram/coop_xvoice.cc`). The tracker's green
knob has a third instrument, **`fm`**: cells on it play coop's FM voice whatever the live family. op1emu: the synth
and the FM voice together, no faults, 9.8 k cycles a block for the voice (`xv_max` on the stats "stages" line).

**What to do**: tape, Shift + tape; write a synth note or two on channel 1; Shift + right, green two detents (`fm`
in the zoom view's instrument column), write notes on channel 2; play: both sound together, the FM voice an octave below the synth
(its stock patch words carry a 0.5 carrier ratio; the synth itself follows its octave setting), a plucky FM sound (the
stock's FM patch words, not yet the channel's own). Then the drum kit live (drum, tape): the `fm` channel still
plays while the drum cells do. The stats file's `xv_max` and `fps` say the cost. Crashes here would point at the
FM render running outside the voice loop (its parameter block or the sine copy); say what you were doing.

Result: (pending)

## Tracker 3 (t3): F2, a voice per channel from four engine kinds — device test (pending, 2026-11-07)

- `build/pkg/op1_246_coop_t3_tracker_x.op1` (sha256 `c6841f892cc192b6c8cd20cf7e19b8c8a41f30a71cccd97e23a08be6b234afbd`), copy in Downloads. Supersedes t2.

The tracker's green knob now cycles `syn`, `drm` (the live families), then coop's own voices **`fm`, `str`, `dig`,
`dsy`** (a stock FM, string, digital or dsynth voice rendered by coop's block beside whatever the core plays;
`docs/tracker.md` "F2"). One voice a channel, mono, centred, mid-value knobs (no patch yet). op1emu: every kind
sounds, no faults; costs a block fm 10 k, dig 13 k, dsy 25 k, str 30 k; three together 63 k peak (11 %).

**What to do**: tape, Shift + tape; channel 1 a synth note; channels 2-4 on fm / str / dsy (green), a note each;
play. Four engines at once: listen for dropouts; the stats `xv_max` (cycles a block for the coop voices, the
cache included), `stage_max`, `cb_max` and `fps` are the measurement the handover asked for (F3). Then the same
with the live synth set to DSynth: the `dsy` channels clash with the live voices' tables (a known probe limit; say
what it sounds like). Known: the `fm` voice sounds an octave below the others (its stock patch's carrier ratio).

Result: (pending)

## Tracker 4 (t4): F2b, the channel's patch — device test (pending, 2026-11-07)

- `build/pkg/op1_246_coop_t4_tracker_patch.op1` (sha256 `90a809dad6f52e846f5c63c926644251f8966a0fbee3336bf6ae3b54daab9462`),
  copy in Downloads. Supersedes t3 (same code plus the patch view; the song file is version 2, an older one is
  ignored).

**Shift + T2** on a channel whose instrument is a coop voice (`fm` `str` `dig` `dsy`): four bars in the encoder
colours, the engine's knobs 0..99, pan top right (Shift + blue). The values apply live while the voice sounds and
are saved with the song. op1emu: the view draws, the string plays with the patch, no faults.

**What to do**: a `str` channel with a note, Shift + T2, turn the four encoders while it plays (the bars move, the
sound changes), Shift + blue to pan it; power-cycle: the patch comes back. Then t3's four-engines test on this
image (it is the same code).

Result: (pending)

## Tracker 5 (t5): render to tape — device test (pending, 2026-11-07)

- `build/pkg/op1_246_coop_t5_tracker_render.op1` (sha256 `665fb3d3ec20535e1f79729301799ce447a13359086ed55d99fcf78d1b265e2c`),
  copy in Downloads. Supersedes t4 (everything so far plus the render).

**Shift + play** in the tracker renders the song once onto the current tape track at the current position: coop
pushes the stock's own rec + play (spaced, after Shift is up), plays the song from its start when the tape rolls,
and pushes stop when the order list wraps. "rec" in orange in the header meanwhile; stop cancels. Transport
change: stop when stopped puts the cursor on row 0, a second stop at the song's start (plain play always starts
from the cursor). op1emu: the song plays, the tape holds the clip (the red bar on the track), it plays back.

**What to do**: a short song (a pattern or two); Shift + play; watch the tape counter run and "rec" show; when it
stops, Shift + tape: the clip is on the track; stop, stop, play: the song from tape. Also with `fm` / `str`
channels in the song (the coop voices go through the same record tap). Then the t4 and t3 tests on this image.

Result: (pending)

## Tracker 6 (t6): the phase engine as a coop voice — device test (pending, 2026-11-07)

- `build/pkg/op1_246_coop_t6_tracker_pha.op1` (sha256 `8cdaed073317267c7a5e03f95785abed7a5159922f1634292a8c3df9515d66f5`),
  copy in Downloads. Supersedes t5 (everything so far; the green knob now also offers `pha`).

The tracker's coop voices are fm, str, dig, dsy and **pha** (the stock phase engine, Pd.cpp); op1emu: it sounds
(a bright harmonic series at the nominal note), 11.7 k cycles a block, no faults; the three-engine run unchanged.
All 30 voice objects sit in the coop L1 pool (`xv_l1 30`). Voltage, dna, drwave, cluster and pulse are not
offered: their getters build the voice inline at the scratchpad address (no constructor to call on coop memory).

**What to do**: the t5 / t4 / t3 tests on this image, plus a `pha` channel beside the others.

Result: (pending)

## Tracker 7 (t7): voltage and cluster as coop voices — device test (pending, 2026-11-08)

- `build/pkg/op1_246_coop_t7_tracker_engines.op1` (sha256 `1964cd9110a8ebd44ecaf6113935cfa4e66adef2ad8646c6b7e459a3b7a1fd9d`),
  copy in Downloads. Supersedes t6: the green knob now offers `syn drm fm str dig dsy pha vol clu`.

Voltage and cluster voices are built by hand on coop memory (their stock getters construct inline at the
scratchpad address). op1emu: both sound at the nominal note, 14 k and 18 k cycles a block, no faults; all 42
voice objects (6 channels x 7 kinds) sit in the coop L1 pool. dna, drwave and pulse stay out (docs/tracker.md).

**What to do**: the t5 / t4 / t3 tests on this image; a song with one channel per engine kind; the stats
`xv_max` with several kinds sounding together is the mixed-engine cache number.

Result: (pending)

## Tracker parked (2026-11-08)

The owner parked the tracker before any of Tracker 1..7 ran on the device ("a lot of gaps we need to fill"). The
t1..t7 packages are superseded; the code is in `attic/tracker/` and the branch history. The next package (u8) is
u7 plus the overwrite / rename spelling fix, without the tracker.

## UI 3 (u8): u7 plus the overwrite / rename fix, the tracker parked — device test (pending, 2026-11-08)

- `build/pkg/op1_246_coop_u8_ui.op1` (sha256 `182785d5946dca3eb48dfbaeaef8d5e02e7310ed113ba964e9e6bb49e1f4d3f8`), copy in Downloads.
  `--tape-stereo --set-word g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr`
  differs. Build tag `u8`. The image is u7 with the overwrite / rename WHERE matching the three JSON spellings
  (ca33b62) and nothing of the tracker. op1emu: settings, tape page, FX 2 scenarios no faults.
  **Superseded by u9 before any device run** (the FX 2 page fixes below; u8 still has u7's dials-at-zero bug).

**What to do**: the u7 list (that entry), and the overwrite again: edit a user preset, synth + S-key held, reload
it. If the change still does not persist, the next hypothesis is the reload reading the preset file rather than
the db row (the u7 entry).

Result: (pending)

## UI 4 (u9): the FX 2 page stock-like, LFO 2 on the stock LFO page, no "none" — device test (pending, 2026-11-08)

- `build/pkg/op1_246_coop_u9_ui.op1` (sha256 `9b865e299952a975108a109250ff6879a68554ff9711984e358e74cc61f04f42`), copy in Downloads.
  `--tape-stereo --set-word g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr`
  differs. Build tag `u9`. u8 (the owner's u7 notes applied) plus the fixes found in op1emu below.

What changed (docs/screens.md "u8", docs/ui-features.md §7 / §8):
- **No "none"**: the FX 2 lists hold the stock types and the plugins. The first drop on an empty slot 2 copies FX 1's
  type and knobs, switched off; T3 switches FX 2 on / off; **Shift + T3 opens the stock list** (blue FX 1, green FX 2).
- **The dials follow FX 2's encoders.** u7's bug: read_fx_param's entries are (raw - min) / (max - min) in the type's
  range units, not knob fractions; u7 wrote q15 / 32767 and every dial sat at 0. The wrapper now fills all eight as
  the stock does; the first drop converts FX 1's words the same way in reverse.
- **The stock "FX OFF" / "LFO OFF" overlays show the focused slot's state** (the overlay predicates' on-flag getter
  CALLs are retargeted, 0x019AAB40 / 5A / 76 and 0x019C97DA / F6). coop's small "fx2 off" / "lfo2 off" texts are gone.
- **LFO 2 on the stock LFO page**: drop / lift focus, T4 off / on, the four encoders with the stock's ApplyRange,
  Shift + T4 the list; LFO 2 is per sound ("lfo2" in the sound JSON's coop member).
- **Two thread fixes**: slot-2 sets that allocate run on the main loop (slot2_request / coop_ui_service), and slot 2's
  stock twin is constructed on the main loop (slot2_build_service) instead of in the audio interrupt.

op1emu (one end state per scenario, screenshots long after the last action, no faults in any run):
- `det_ui_fx2same`: FX 1 cwo on (delay 05, fdback 09); drop; T3; green +10 %: FX 2's page shows **DELAY 15, FDBACK
  09** (the dial lit as touched); `det_ui_fx2lift` (the same plus lift): FX 1's page shows 05 / 09. `ui_knobs2 10`.
- `det_ui_fx2off`: drop with FX 1 on: FX 2's page carries the stock **FX OFF** overlay, dials dimmed at FX 1's values;
  `det_ui_fx2offlift`: lift: FX 1's page, no overlay; `det_ui_fx2on1off`: FX 1 off, FX 2 on: no overlay, dials lit.
- `det_ui_lfo2` / `det_ui_lfo2lift`: LFO 2's page (tremolo, amount 04 after the turns) vs LFO 1's after lift (speed
  6, amount 10); `lfo2view_on` counted; `det_ui_lfo2off`: drop on the LFO page: LFO 2's page (tremolo, middles) carries the stock **LFO OFF** overlay, dials dimmed.
- Regressions `det_ui_fxlist_t3`, `det_ui_settings`, `det_ui_tape` unchanged.
- The u8 image froze op1emu whole at the first drop with cwo in FX 1: coop returned an **empty** dial vector for
  "off", and every effect page's Update asserts four entries (the L1 abort 0xFFA0A1E0). Found with
  `tests/sim/watch_writes.sh` on a trace word (the per-PC table's last-time column shows which thread stopped first).

**What to do**: the u7 list, then on a synth with an effect: T3 page, drop (FX 2 = FX 1's type, FX OFF shown),
T3 (on), turn the four encoders (the dials follow, the sound changes), lift (FX 1's page, its own dials), Shift + T3
(the list with both columns; the green column sets FX 2), the same on the drum and the master (mixer page 2); T4
page: drop (LFO 2, LFO OFF shown), T4, encoders, Shift + T4, lift; a sound saved with an LFO 2 reloads with it.
Then the overwrite question from the u8 entry.

Result: (pending)

## Performance 6a (p6a): the performance monitor in the stats — device test (pending, 2026-11-08)

- `build/pkg/op1_246_coop_p6a_pf.op1` (sha256 `4ccff2c98454bcf34d007bde2bb2009dc7d9dbfa9f718a04b3277fb3d8da259e`), copy in Downloads.
  `--tape-stereo --set-word g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr`
  differs. Build tag `p6a`. u9 plus lever 1 of docs/fx-data-placement-plan.md: the Blackfin performance monitor
  programmed at boot (mode 0: code-fill and data-fill STALL CYCLES), read around every effect render; the stats
  file's `cost` line gains `/p0/p1` per entry (docs/performance.md "p6a"). No audio change.

**What to do**: the u9 UI list still applies (same image plus the counters). For the table: the p5 script
(docs/test-runner.md) with each stock effect in slot 1 in turn (delay, grid, punch, phone, spring, filter, nitro,
cwo), FX on, a held chord; then copy `coop_stats.txt` in disk mode. Expected per effect: `fx1_<type>=avg/max/n/p0/p1`
with `pf_live 1`. If time allows, the same once more as a `g_coop_pf_mode=1` build (ask: it is one more package) for
the miss counts. What decides the next lever: p1 (data stall cycles) near `avg` minus the op1emu figure says data,
p0 large says code (lever 6 first).

Result: (pending)

## UI 5 (u10): randomise acts on the focused slot — device test (pending, 2026-11-08)

- `build/pkg/op1_246_coop_u10_ui.op1` (sha256 `6ee3e7f506ab6124e15fb0348023c63af255b81a79acbae7b6ed9e37528a91e2`), copy in Downloads.
  `--tape-stereo --set-word g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr`
  differs. Build tag `u10`. p6a (u9 + the performance counters) plus: hold synth / drum + right on the effect page
  with **FX 2 in focus** randomises FX 2's four knobs (a plugin in FX 2 is refused); on the LFO page with **LFO 2 in
  focus** the shown sound's LFO 2 parameters. Both refuse while the slot is off, as the stock page does. Left steps
  back through the slot's values too (while the slot still holds that type). docs/ui-features.md §10.

op1emu (no faults): `det_ui_rnd2` (FX 1 cwo 05 / 09 on; drop; T3; synth + right): FX 2's page shows freq moved,
delay 17, fdback 35; `det_ui_rnd2lift` (+ lift): FX 1's page still 05 / 09. `det_ui_rnd2lfo` (T4, drop, T4, synth +
right): LFO 2's page shows speed moved, amount -90. `rnd 1 rnd_slot2 1` in each; the u4 `det_ui_random` run unchanged
(`rnd 3 rnd_back 1`).

**What to do**: the u9 UI list on this image (it supersedes u9 and p6a), then: FX page, drop, T3 (FX 2 on), hold synth
+ right several times (FX 2's dials jump, the sound follows, FX 1 untouched after lift), hold synth + left (back);
the same on the drum; LFO page, drop, T4, synth + right / left. The p6a counter table from this image is the same
measurement (the counters are in it).

Result: (pending)

**Result (p6a counters, run on the u10 image, 2026-11-08)**: `pf_live 1`, the p5a script complete (`run events 1554
blocks 129130 state 3 lost_sections 0`), every section's `cost` line carries `/p0/p1`. p0 (code fill stalls, cycles):
punch 29,625 of 70,509, nitro 17,419 of 48,959, cwo 17,260 of 59,161, spring 10,258 of 40,115, phone 8,557 of
30,810, delay 7,293 of 22,124, grid 6,946 of 29,575, stage 2,236 of 11,227 (table and reading in
docs/fx-data-placement-plan.md §5). p1 void on this run (0x9A is an occurrence event, counted as cycles): p6b.
The stats also show `rnd 13 rnd_back 3 rnd_slot2 7`, `ui_toggles 6`, `fx2view_on 564`, `lfo2view_on 206`: the u10
randomise and the FX 2 / LFO 2 pages were exercised on the device; the owner's verdict on them is pending.

## Performance 6b (p6b): the data-stall column — device test (pending, 2026-11-08)

- `build/pkg/op1_246_coop_p6b_pf.op1` (sha256 `69c6a3cf6a800f77f6fcee87f0e7bb99f33cc5ce71cf21eebc95fd6330a27de4`), copy in Downloads. `--tape-stereo --set-word
  g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr` differs. Build tag `p6b`.
  u10 with the counter pairs corrected: p0 "code fill stalls" 0x82, p1 "data stalls" 0x98, both cycles. No other change.

**What to do**: the p5a script once more, `coop_stats.txt` copied: the p1 column per effect is the data side of the
table (delay / spring / grid / cwo are the ones to look at). Everything else as u10.

Result: (pending)

**Result for UI 4 (u9) / the FX 2 and LFO 2 pages (the owner, 2026-11-08, on u10)**: "FX2 and LFO 2 pages now are
working perfectly". Pass. (The u10 randomise verdict and the overwrite question stay open.)

## Performance 6c (p6c): code residency with a switch — device test (pending, 2026-11-08)

- `build/pkg/op1_246_coop_p6c_codeprio.op1` (sha256 `6bdcd74b0a3fc87a98df2c67716b5ec55a5f4b40a5f422a7abe36d6f0e713d5f`), copy in Downloads. `--tape-stereo --set-word
  g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr` differs. Build tag `p6c`.
  p6b plus lever 6 (docs/performance.md "p6c"): coop's text split, the per-block code alone in region A's page,
  the rest in region B `0x02810000` (a first for coop: code outside region A); settings > voices > **code prio**
  (green right = on, left = stock) marks the hot page LRUPRIO in the instruction cache. Off at every boot.

**What to do** (the counters are in this image, p6b's pairing):
1. Boot, play: everything as u10 (the cold code's new home must change nothing).
2. The p5a script once with code prio **stock** (= the p6b table: p0 code stalls, p1 data stalls per effect).
3. Settings (four mode keys) > right > blue four down to CODE PRIO > green right: "on" (if it reads "refused", the
   CPLB table was not as expected: tell me, nothing was written). Left, left. Play, and watch the UI: tape page,
   mixer, the effect pages, the browser: any new lag or stutter is the cost (sets full of priority lines).
4. The p5a script again with it on; copy `coop_stats.txt`. Expected: p0 per effect well below step 2's; `fps` not
   lower; `codeprio 1 codeprio_sets 1` on the `l1` line.
5. Green left: "stock" again; a reboot also clears it.

Result: (pending)

**Result (p6b, 2026-11-08)**: `pf_live 1`, the p5a script complete; the data column is in (p1 = data stalls 0x98):
delay 4,418 of 21,988, grid 7,353 of 29,168, punch 7,538 of 72,504, phone 7,728 of 31,270, spring 12,657 of 39,326,
nitro 4,448 of 49,208, cwo 12,216 of 62,463, stage 1,204 of 11,374 (table in docs/fx-data-placement-plan.md §5).
Together with p0 about half of every effect's block is memory stall. Pass (the counters do their job).

**Result (p6c, the owner, 2026-11-08)**: "codeprio on completely breaks everything. the sound is all glitched the
ui is super laggy. It's not even worth testing." Fail, as the documented risk, only total: 30 KB of hot text marked
high fills every cache set with four protected lines over time, and the stock's own SDRAM code (the audio classes,
the UI, the main loop) runs uncached. The split image itself (switch off) ran. Next: the protected set shrinks to
the assembly kernels alone in a page of their own (p6d), with a periodic LRUPRIORST so old highs do not accumulate.

## Performance 6d (p6d): p6c withdrawn, the lift crash fixed — device test (pending, 2026-11-08)

- `build/pkg/op1_246_coop_p6d_pf.op1` (sha256 `538f638d27e198699a9f4cbf6d7b03a51f3cc190c79d3b10b6f73b9decf8b0ff`), copy in Downloads. `--tape-stereo --set-word
  g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr` differs. Build tag `p6d`.
  = p6b (u10 + the corrected counters) exactly: one text in region A again, no "code prio" row. The p6c crash on
  lift was the cold code at `0x02810000` sitting inside coop's own clipboard pool (region B, `tape_edit.cc`): the
  first lift overwrote it (SEQSTAT 0x21 undefined instruction, RETX 0x0281xxxx). Nothing of p6c remains in the image.

**What to do**: lift and drop on the tape (the crash is gone); otherwise everything as u10. No new measurement.

Result: (pending)

## Performance 6e (p6e): the counters on the voice renders — device test (pending, 2026-11-08)

- `build/pkg/op1_246_coop_p6e_pf.op1` (sha256 `07539fd5520deb046ec9523e949d5b3849fc7f0abf9791b265bb7aa099e82936`), copy in Downloads. `--tape-stereo --set-word
  g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr` differs. Build tag `p6e`.
  p6d plus the two counter reads around every voice render (docs/performance.md "p6e"). No audio change.

**What to do**: the p5a script once more, `coop_stats.txt` copied. Expected: every `engines` entry reads
`name=avg/max/n/p0/p1` (per voice render) and `synth=` / `drum=` per block the same; the `cost` line as p6b. The
engine sections (dsynth6, voltage6, phase6, digital, pulse, drwave, fm, sampler, drum) are the ones to read.

Result: (pending)

**Result (p6e, 2026-11-08)**: the engines line carries the counters; per voice render dsynth 22,949 cycles with 12,047
code stalls, voltage 10,407 / 5,866, digital 10,281 / 5,212, phase 7,423 / 3,485, fm 8,988 / 4,332, sampler 9,804 /
3,753, drwave 16,268 / 5,797 (data stalls 0-2.5 k); six DSynth voices lose 72 k of 138 k per block to code fills
(docs/fx-data-placement-plan.md §5). Pass.

## Performance 6f (p6f): first voice vs the rest, and the instruction-cache probe — device test (pending, 2026-11-08)

- `build/pkg/op1_246_coop_p6f_pf.op1` (sha256 `ca40848a1cf53502f458abb7e1821528d1f3ecaa33e47575ad2fb9c6c19cdb70`), copy in Downloads. `--tape-stereo --set-word
  g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr` differs. Build tag `p6f`.
  p6e plus `synth_first=` / `synth_rest=` on the engines line and the boot-time `icache` probe line
  (docs/performance.md "p6f"). No audio change.

**What to do**: the p5a script once more, `coop_stats.txt` copied (the `icache` line is in every stats file, the
dsynth6 section is the one for first / rest).

Result: (pending)

**Result (p6f, 2026-11-08)**: `icache c1 27175 p1 523 c2 2058 p2 530 c3 27594 p3 516`; `synth_first` 28,747 /
`synth_rest` 20,789 cycles per DSynth render with the same p0 (~12 k) on both. The probe shows p0 is not a stall count
(the same ~520 with and without 25 k cycles of fill stalls): the counter-0 event field is mis-placed in the PFCTL
layout coop used. The p0 columns of p6a-p6f are instruction-fetch counts; the "code-bound" reading is withdrawn.
The first / rest split is still informative on cycles: the first voice of a block costs 8 k more than the others
(28.7 k vs 20.8 k), so about a third of a DSynth render's excess is paid once per block, the rest per voice.

## Performance 6g (p6g): the counter layout proven by the probe — device test (pending, 2026-11-08)

- `build/pkg/op1_246_coop_p6g_pf.op1` (sha256 `52af6e2181b4bee0b59e8eecfa2963baf50b03ce141240363b53964ac519cea3`), copy in Downloads. `--tape-stereo --set-word
  g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr` differs. Build tag `p6g`.
  p6f plus: the boot probe runs under both PFCTL layouts (kennel's, Linux's) and keeps the one that counts stall
  cycles (`pf_layout` on the cost line: 0 or 1; 0x11 = neither proved, Linux's kept). No audio change.

**What to do**: the p5a script once more, the two downloads. First the `icache_l0` / `icache_l1` lines: the proven
layout shows p1 in the thousands and p2 near 0. Then the tables are read afresh on that layout.

Result: (pending)

**Result (p6g, 2026-11-08)**: `pf_layout 17` (neither proved). `icache_l0 c1 27576 p1 518 c2 2058 p2 530 c3 26546
p3 518` (kennel's positions: counts fetches, not stalls, as p6f); `icache_l1 c1 2058 p1 0 c2 2057 p2 0 c3 27386 p3 0`
(Linux's positions: nothing counted, not even through the 27 k-cycle cold pass; the data counter read 0 everywhere
too). So kennel's field positions are the working ones and the event number is what is wrong: 0x82 counts
instruction fetches on this core. The cycles columns of the run are valid and unchanged (dsynth 22.1 k per render,
first 28.7 k / rest 20.8 k; the effects as p6b).

## Performance 6h (p6h): the event sweep — device test (pending, 2026-11-08)

- `build/pkg/op1_246_coop_p6h_pf.op1` (sha256 `e935d4b91f24699960aa80d85f12d2c5eb4ba239496a53ce763e8b92437ec703`), copy in Downloads. `--tape-stereo --set-word
  g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr` differs. Build tag `p6h`.
  p6g plus, once at boot: every code event 0x80..0x8F on counter 0 and every data event 0x90..0x9F on counter 1,
  each as cycles ("s") and as occurrences ("o"), against the cold / warm probes (code: the 4 KB NOP function after
  24 KB of other code; data: an 8 KB array read after a 32 KB walk). Two stats lines, `pfsweep_code` and
  `pfsweep_data`: `ev s cold/warm` and `ev o cold/warm` per event. ~40 ms of boot, no audio change; the live
  counters stay on kennel's positions, mode 0.

**What to do**: boot it and copy `coop_stats.txt` (no script needed; a p5a run does no harm). The two `pfsweep`
lines are what I read: the stall event is the one with ~25 k cold / ~0 warm on the code side, the miss count the
one with ~128 cold / ~0 warm; the data side the same against ~256 lines.

Result: (pending)

**Result (p6h, 2026-11-08)**: the sweep (table in docs/performance.md "p6h"): counter 0 written 0x83 as cycles =
code fetch stall cycles (26,359 cold / 21 warm), 0x81 as occurrences = I-cache fills (133 / 4), 0x82 = fetches
delivered (the p6a..p6g column); counter 1 0x99 as cycles = data stalls sent to the processor (52,571 / 0), 0x9A as
occurrences = D-cache fills (256 / 1), 0x98 = p6b's data column (57,077 / 1,227, valid). Pass: the events are known.

## Performance 6i (p6i): the first true code-stall table — device test (pending, 2026-11-08)

- `build/pkg/op1_246_coop_p6i_pf.op1` (sha256 `7a163b4556aa4bc8e26213e4cca92ced90bba6291eb3332464fc0b30d3c5916e`), copy in Downloads. `--tape-stereo --set-word
  g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr` differs. Build tag `p6i`.
  p6h with the sweep's events live: mode 0 p0 = code fetch stall cycles (0x83 s), p1 = data stalls sent to the
  processor (0x99 s); the sweep off unless `g_coop_pf_sweep=1`. No audio change.

**What to do**: the p5a script, the two downloads. Check first: the `icache_l0` line should now read p1 ~26 k, p2
~20, p3 ~26 k (the probe proving the live event). Then the effect and voice tables are read for the first time
with a code column that means what it says.

Result: (pending)

**Result (p6i, 2026-11-08)**: `icache_l0 c1 26866 p1 25867 c2 2058 p2 0 c3 26236 p3 25237`: the live counter is the
stall event. The table (docs/fx-data-placement-plan.md §5): voices compute-bound (dsynth 22 k per render, 7.7 k of
code stall for the first voice of a block, ~0.8 k for the others); effects: punch / nitro compute-bound (stall 14 k
of 71 k, 11 k of 48 k), spring / grid / delay / cwo / phone memory-bound (47-66 %, code and data stalls of the same
order, the code stall about the kernel's size: refetched every block); the stage 4.3 k code stall a block; slot 2
off 1.2 k a block of pure code stall. Pass: the counters measure.

## Performance 6j (p6j): the idle paths cost nothing — device test (pending, 2026-11-08)

- `build/pkg/op1_246_coop_p6j_pf.op1` (sha256 `7eb58d9fe72e19a656e851a98e36ab626074d8fd6cf591878f812b7e05752331`), copy in Downloads. `--tape-stereo --set-word
  g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr` differs. Build tag `p6j`.
  p6i plus: the mix entry skips the instrument slot 2 while it has nothing to run, the master stand-in returns at
  once while the master slot is idle and the suite is off; a knob-only change of slot 2 no longer passes it dry for
  a main-loop pass (docs/performance.md "p6j"). Audio: identical when the slots run; the dry gap on an FX 2 knob
  turn gone.

**What to do**: the p5a script, the two downloads: `s2_instr=` and `s2_master=` should be absent from the sections
where slot 2 is idle (nothing counted) and `master=` gone or tiny; the per-block `cb_avg` of each section ~2.5 k
below p6i's. Then play: FX 2 on and off, its encoders while it runs (no dropout), the master's FX 2, the tape.

Result: (pending)

## Platform fixes (p6k): HELLO tag and caps, 40-character plugin names, the built-in delay removed — device test (pending, 2026-11-08)

- `build/pkg/op1_246_coop_p6k_platform.op1` (sha256 `8f3c93f01e34ec753ad6352b21602ed709fc4807cf13e187e34dc17324ff7fc0`), copy in Downloads. `--tape-stereo --set-word
  g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr` differs. Build tag `p6k`.
  p6j plus the three handover items (docs/handover-coop-platform.md §0): `HELLO` answers the package tag
  (`core/include/coop_build.h`, shared with the stats and run files) and a capabilities byte; the settings menu
  stores plugin file names up to 40 characters (settings file v2, v1 read); the SDK describes the real `params`.
  And the firmware's built-in stereo delay (master suite flag bit 0) is removed. No audio change on any shipped
  path (the delay was never on in a package).

**What to do**: boot and play as u10. With the OP-1 plugged in normally, `tools/coop_sysex.html` > Hello should
read `coop protocol 1 caps 0xf build p6k`. If a plugin file is present, settings > plugins: toggle a long-named one
off, reboot, it is skipped (`plugins ... disabled 1` in the stats).

Result: (pending)

## Engines (e2): plugin synth engines and their screens — device test (pending, 2026-11-09)

- `build/pkg/op1_246_coop_e2_engines.op1` (sha256 `0dc8b7f0eb95f359babe889b2e318a169358740ed71af057d58d0f92925e904c`), copy
  in Downloads with `saw.coop`. `--tape-stereo --set-word g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check`
  = only `OP1_vdk.ldr` differs. Build tag `e2`. p6k plus E1 (synth engine plugins through the iter engine) and E2
  (their screens), docs/engines.md. With no engine file the image plays as p6k (op1emu: identical, the Gfx path within
  1 LSB, docs/engines.md).

**What to do**: flash, boot, play as before (nothing should change). Then install `saw.coop` with
`tools/coop_sysex.html` > Install (it goes to `/yaffs2/plugins`), power-cycle. On the synth page, shift + T1: "saw" is
in the engine list between sampler and string; pick it. The page shows two saws (blue, green), a white square under
them and an orange dot: turn each encoder and its own part should move (blue the saws' height, green the second saw
sideways, white the square's height, orange the dot's wobble). Play: a bright detuned saw; every key in tune across the
keyboard; six notes at once; bend and the LFO work as on any engine. Save the sound, power-cycle: it comes back as saw.
Then remove the file (SysEx Delete, or the settings menu's plugin row off) and power-cycle: the sound loads as iter,
no crash. The stats file's `plugins` line: `engines 1`, `eng_*`, `scr_*`; the `engines` line `plugin_engine=` (the
device cost a voice: op1emu says ~6.6 k cycles).

Result: (pending)

## Plugin management (e3): the settings crash, INVENTORY and PLUGIN_SET — device test (pending, 2026-11-09)

- `build/pkg/op1_246_coop_e3_plugins.op1`, copy in Downloads. `--tape-stereo --set-word g_coop_spread=32767`;
  `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr` differs. Build tag `e3`. e2 plus: the settings file's
  buffer is static (it was 650 bytes on the main loop's stack since p6k, and 2 bytes short with 16 names: the likeliest
  cause of the e2 crash, "leaving the menu after turning a plugin off, saw + stereo_delay installed", which op1emu does
  not reproduce); SysEx `0x28 INVENTORY` and `0x29 PLUGIN_SET` (docs/sysex.md), HELLO caps `0x3f`.

**What to do**: settings > plugins: turn one off, leave the menu (mode key, and once with the left arrow): no crash;
power-cycle: it is skipped. Then manage plugins from co-op's Plugins tab (dev site) once it speaks coop. If it still
crashes, a photo of the crash screen (SEQSTAT, RETX) says where.

Result: (pending)

## Engine controls and L1 (e4): the rings port's core request — device test (pending, 2026-11-10)

- `build/pkg/op1_246_coop_e4_engine_controls.op1` (sha256 `7294ceeda3ae55e985a9ee4a4cbc02fca70792c3b3473247c20728c2d7358a83`),
  copy in Downloads with the new `saw.coop` (0.2: it needs e4, it imports the L1 lease and the new screen calls).
  `--tape-stereo --set-word g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr`
  differs. Build tag `e4`. e3 plus engine controls (fn layers, clicks), the L1 lease and scratch, the heap figure, the
  screen exports, per-engine block cost (docs/engines.md "e4").

**What to do**: flash; install the new `saw.coop` (co-op Plugins > On your OP-1 > Install a .coop file, or the SysEx
page), power-cycle, pick saw (shift + T1). Then on saw's page:
1. Hold Help and turn blue: the shape steps saw / square / triangle (two detents a step); "SHAPE SQUARE" shows top left
   in blue for about a second; the oscillators on the screen change; while Help is held the other shape icons turn
   dim blue. Play: square and triangle sound as they look.
2. Hold shift and Help, click orange: the sub drops to -2 oct ("SUB -2 OCT" in orange; two dots bottom right). Again:
   back to -1.
3. Hold Help and turn green or white: nothing happens. Tap Help alone: help opens as before.
4. Press blue, press shift, release blue: nothing happens (a combo, not a click).
5. Shift + turn on any encoder: fine adjustment, as on a stock engine.
6. Synth key + right arrow (randomise) a few times on saw's page: the shape changes with the knobs, the sub's octave
   never (it is marked "random": false).
7. Save, power-cycle: the shape and octave come back.
8. Copy `coop_stats.txt` (co-op or the SysEx page): the `l1` line (`leased 192 leases 1 ... free`), the `plugins` line
   (`heap_largest`, `ctl_*`), the `engines` line (`eng_saw=` per voice render, `eng_saw_block=` per block: the device
   costs rings will be judged against).

Result: (pending)

**Result (owner, 2026-11-10, e4 + saw 0.2): FAIL, crash at boot** with the new `saw.coop` installed: `SEQSTAT 0x21`
(undefined instruction), `RETX 0x648d0` (heap 0, where plugin code is relocated), `ADDR 0xa`, `STATUS 0x3e`, call
stack `0xffa00424`, `0xffa035a8`. Cause [I, strong]: the loader wrote the relocated code with data stores and never
wrote the data cache back or dropped the instruction cache before running it; e4's saw calls into the core (the L1
lease, through a call stub) from `init` right after its relocation at boot, so the core could fetch stale bytes. Kennel
syncs both caches after every relocation (`kn_boot.c sync_caches`); coop's port of the loader had left it out, and
op1emu models no caches. Earlier plugins only ran later (FX init on selection, renders), by when the lines had been
written back. Fixed in e4b (below).

## e4b: cache sync after relocation — device test (pending, 2026-11-10)

- `build/pkg/op1_246_coop_e4b_cache_sync.op1` (sha256 `d9bd3b1054498c6c7ccfdbd53458fdac34b65b203fb56f7a3ad86d141cd76fef`),
  copy in Downloads. `--tape-stereo --set-word g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check` = only
  `OP1_vdk.ldr` differs. Build tag `e4b`. e4 plus `code_sync` in plugin_host.cc: after every relocation (each effect
  instance, each engine) `FLUSH` every line of the image and its call stubs, `SSYNC`, `IFLUSH` every line, `CSYNC`,
  before anything of the plugin runs. Stats `plugins` line: `code_syncs` (7 with saw, stereo_delay and a test engine
  in op1emu: 5 + 1 + 1). op1emu: no fault, every plugin loads and plays, `det_chord.txt` identical to e4.

**What to do**: the OP-1 crashes at boot while saw 0.2 is on it, so hold COM while switching on (te-boot runs before
any coop code), "1. Upload firmware", flash e4b. Then it should boot with saw installed; continue with the e4 checklist
above. If it still crashes, a photo again; to get out, flash e3 (it refuses saw 0.2 for its import and boots) and
remove the file from co-op.

**Result (owner, 2026-11-10): PASS.** "Everything is working well with the saw plugin test now": e4b boots with saw 0.2
installed and the e4 checklist passed (the shape and sub controls, the screen, randomise, save and power cycle). Two
UI bugs found and fixed in e4c (below), and the rings port is cleared to resume.

## e4c: the settings menu's plugin rows and Shift + Help — device test (pending, 2026-11-10)

- `build/pkg/op1_246_coop_e4c_settings_fn.op1` (sha256 `7b974fc41a3b72d85f9b9a0223c5317e37c53fc28dedb609c7bf8b75a89211bd`),
  copy in Downloads. `--tape-stereo --set-word g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check` = only
  `OP1_vdk.ldr` differs. Build tag `e4c`. e4b plus the owner's two reports:
  1. Settings > plugins: "the row stays ON" when pushed. The rows only took a push, while every other value row in the
     menu takes the green turn (the owner's menu design, ui-features §1), and a push that bounced toggled OFF and back
     ON. Now green sets the row (clockwise ON, anticlockwise OFF) and a push toggles once per 250 ms.
  2. Shift down, Help down, Help up opened the stock date and time screen (a Help tap is passed to the stock, and
     Shift was still held). Now Help never reaches the stock while Shift is held, on the press or the release (the
     owner's choice); plain Help taps still open help.
- op1emu (`tests/sim/det_ui_e4c.txt`, a NAND with saw): `set_plugin_turns 2 set_plugin_pushes 1 set_plugin_bounces 1`,
  the settings file then lists `saw.coop`, the next boot skips it (`loaded 0 disabled 1`); `fn_shift_kept 1
  help_taps 0`, no date screen. `det_chord.txt` identical to e4b.

**What to do**: settings > plugins: turn green left on the saw row: OFF; right: ON; push: toggles once. Leave the menu
with saw OFF, power-cycle: saw is gone from the engine list (a sound on it plays iter). Then Shift down, Help down,
Help up: nothing opens. Help alone: help as before.

**Result (owner, 2026-11-10, on e4e): PASS for Shift + Help** ("shift+help is now fixed": no date and time screen).

**Result (owner, 2026-11-10): FAIL for the plugin rows.** Settings > plugins: neither a green turn nor a push changes
the saw row; co-op's Switch off says "the OP-1 refused switching saw.coop off: a size mismatch, or the list is full"
(PLUGIN_SET status 4: the disabled list already held 16 names, none of them saw.coop). And co-op showed the build as
`e4b`: the package carried e4c's code with e4b's tag, because `core/sdram/Makefile` did not list the headers, so the
rebuild after the tag fix in `coop_build.h` was skipped as up to date. Both fixed in e4d.

## Rings (E3): the first ported synth engine — device test (pending, 2026-11-10)

- `build/plugins/rings.coop` (111,132 bytes, sha256 `5ca9fdadf4776a7faa0a86464f22d5b59fc9e29cd0df6987fa7f8170daf1849d`),
  `plugins/rings`. Needs the e4c package (or later) on the OP-1: the controls, `coop_l1_lease` and `coop_l1_scratch`.
  Install it from the co-op Settings tab (or `tools/coop_sysex.html`) and power-cycle.
- Host: bit-identical to te-op1-cfw's engine (`make -C tests/host/rings`). op1emu: `docs/engines.md` E3
  (`eng_rings_block` 85,742 avg / 153,017 max cycles a block in the emulator; the device runs 10-35 % higher).

**What to do**:
1. Shift + T1: "rings" is in the engine list (after pulse). Select it: rings' screen (the rings, MODEL MODAL, POLY ALL).
2. Play six-note chords and runs in the modal model; turn all four encoders while notes ring. Listen for clicks,
   crackle or dropouts, the worst case being six voices ringing with the encoders moving (kennel's Tests 12-14
   crackled there before its coefficient budget).
3. Hold Help and turn blue: two detents a step through MODAL, SYMP, STRING, FM, QUANT, VERB (the core's popup names
   each; the screen brackets the row while Help is held). Play each. Then Help + green through ALL, 1, 2, 4: at 1, play
   fast notes (one voice, steals with a short fade).
4. Shift + turn: fine adjustment on all four.
5. Save the sound, power-cycle: it comes back as rings with the same model and polyphony.
6. Copy `/yaffs2/user/coop_stats.txt`, most of all the `engines` line (`eng_rings=` and `eng_rings_block=`) and the
   `l1` line (`leased 1056`), from the busiest moment (VERB or FM, six voices, encoders turning).

Compare with the kennel build on hundekraft, if you still have it: the same sound, by ear.

Result: (pending)

## e4d: the disabled list keeps only real files — device test (pending, 2026-11-10)

- `build/pkg/op1_246_coop_e4d_settings_list.op1` (sha256 `41d83e7f2a14deb679d25d353d1c31c80795603b4e977389f97cd51481b03e02`),
  copy in Downloads. `--tape-stereo --set-word g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check` = only
  `OP1_vdk.ldr` differs. Build tag `e4d` (checked in the ELF). e4c plus:
  - at boot, after the directory listing, the disabled list keeps only names of `.coop` files that are there, once
    each; garbage (a damaged file: the e2 crash happened while this file was being written), duplicates and files
    gone since are dropped and the cleaned file is saved (`set_listed`, `set_dropped` in the stats `plugins` line);
    names are written up to their NUL (the rest of the 40-byte slot zero);
  - `core/sdram/Makefile` lists every header in `core/include`, `sdk` and the kennel screens as prerequisites, so a
    changed tag or header always rebuilds the image.
- op1emu: a NAND with saw.coop and a settings file whose list held 16 entries (8 names of files that don't exist, 8 of
  random bytes): boot `set_dropped 16 set_listed 0`; `det_ui_e4c.txt` then turns saw OFF (`set_plugin_turns 2
  set_plugin_pushes 1 set_plugin_bounces 1`), the file holds `saw.coop` and zeros, the next boot skips saw (`loaded 0
  disabled 1`). With no plugin files the prune is not run (nothing loads; the next boot with files cleans the list).
  `det_chord.txt` (no plugins): one e4d run identical to e4c (3,175,159 frames), another within 14 LSB of it; the same
  image gives either outcome, as other scenarios do run to run, so not a change of e4d.

**What to do**: flash e4d. co-op > Plugins > Read plugins: the pill says `coop e4d`. Then settings > plugins: green
left on saw: OFF; leave the menu; power-cycle: saw is gone. co-op's Switch off / on works too. If you can, copy the
stats `plugins` line: `set_dropped` says how many bad names the device's file held.

**Result (owner, 2026-11-10): PASS** for the menu: saw turns off in settings > plugins. Found: the switched-off saw was
still in the engine list and loaded as iter (its `synth_types` row stayed: E1 never removed rows). Fixed in e4e.

## e4e: the lists show only plugins that loaded — device test (pending, 2026-11-10)

- `build/pkg/op1_246_coop_e4e_list_rows.op1` (sha256 `0f28fd8dc18fbf61497b9084dc334825eea8ad78ecf369a68e1ec18780cf0a2b`), copy in Downloads.
  `--tape-stereo --set-word g_coop_spread=32767`; `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr` differs.
  Build tag `e4e`. e4d plus: at boot, every plugin in the number map that did not load (switched off, deleted, failed)
  loses its `synth_types` / `fx_types` row in the factory database (read first: a boot that changes nothing writes
  nothing; stock names never touched; also when no plugin file is left). Its number stays, and a sound saved on it
  loads as iter (engines) or the filter (effects) and plays as the plugin again once it is back.
- op1emu (a NAND with saw): saw on, row present; switched off in the menu (`det_ui_e4c.txt`), next boot `loaded 0
  disabled 1 rows_dropped 1`, no saw row; switched on (`det_ui_plug_on.txt`), next boot `loaded 1`, the row back.

**What to do**: flash e4e. Switch saw off in settings > plugins, leave the menu, power-cycle: saw is gone from the
engine list (Shift + T1). Switch it on again, power-cycle: it is back between sampler and string, with its screen.

**Result (owner, 2026-11-10): PASS.** "Yes it's working now": switched off, saw leaves the engine list; switched on, it
is back with its screen.

## Rings 0.2: the first optimisation — device test (pending, 2026-11-10)

- `build/plugins/rings.coop` 0.2 (112,004 bytes, sha256 `55516391ad42477580177d5b8df83c0006a56e9cea968e7d091650b20008d45f`).
  From the 0.1 device run (`coop_stats (21).txt`: rings 181,951 avg / 415,022 max cycles a block, crackles, the UI
  lagging with one effect): the upsampler's half-band stage in assembly (bit-identical) and the control rate halved
  (each voice's coefficients and string parameters every 2.9 ms instead of 1.45 ms; the owner's choice). Simulator
  compute 11-18 % lower per render, op1emu -11 %; the device should gain more (half the renders skip the control
  code, so fewer instruction-cache misses). `plugins/rings/README.md` "Optimisation".

**What to do**: install over 0.1 (same id), power-cycle. Then the 0.1 session again, as close as you can: six-voice
chords in each model with the encoders turning, the same effect(s) as before. Listen for:
1. crackles: fewer? when do they still happen (model, polyphony, effect)?
2. the UI with rings and one effect: still laggy?
3. the sound: anything different from 0.1? (A knob turn now reaches the sound in 2.9 ms steps instead of 1.45; a
   fast sweep of structure or position on a ringing note is where it could show.)
Then send `coop_stats.txt`: the `engines` line (`eng_rings=`, `eng_rings_block=`, `synth_rest=` with its two stall
columns) and the `win` lines of the busy part.

Result: (pending)

## Rings 0.3: four voices, fewer modes, batched control — device test (pending, 2026-11-10)

- Result of 0.2 (owner, `coop_stats (22).txt`): felt worse. With the tape recording, the callback averaged 460-470 k
  in the busy windows and peaked at 882 k; rings itself 173,719 / 278,457 a block (5 % below 0.1).
- `build/plugins/rings.coop` 0.3 (111,888 bytes, sha256 `3552d8453560ca022e68991c3a23a15e30705d0b3cdb8958cf4e5b18c355cadb`):
  four voices at most (the polyphony steps are now 4, 1, 2; an old sound plays as 4), 10 modes a modal voice, and
  the voices' control work batched once a block. op1emu: 55,373 a block (0.2: 76,056; 0.1: 85,742).

**What to do**: install over 0.2, power-cycle. Two short sessions, one change at a time so the stats separate them:
1. Rings alone, no recording, no effect: six-note chords (four sound; the oldest fades) in each model, encoders
   turning. Copy `coop_stats.txt`.
2. The same with the tape recording (and then with one effect, if 1 went well). Copy it again.
Listen for crackles and UI lag in each, and whether four voices is enough for how you play rings.

Result: (pending)

## m1: where the rest of the block goes while rings plays — device test (pending, 2026-11-10)

From `docs/request-rings-collateral.md`. A **test image**, not a release: it plays as e4e, plus measuring.
- `build/pkg/op1_246_coop_m1_collateral.op1` (sha256 `61a058bf4d8fed828c8b5d988c34910cc407d5907623b1017e43bd4895d16037`),
  copy in Downloads. `COOP_M1=1`, `--tape-stereo --set-word g_coop_spread=32767 --set-word g_coop_gap_prof=1`;
  `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr` differs. Build tag `m1`. e4e plus:
  - seven stock calls of the audio callback through timing thunks (`core/sdram/m1_thunks.S`: the DSP `0xffa017ec`,
    the synth and drum voice loops, the master stage from its two sites, `l1v_push_lr`, the callback's last call
    `0xffa036c4`), and marks at each voice's thunk entry, the voice hook's end and the plugin's own render, all with
    the two performance counters; each runner section gets a `stages` line (`docs/performance.md` "m1");
  - the runner's `#@pfmode` event (the counters' events per run).
- Three test engines, in Downloads: `probe_alu.coop` (sha256 `2118b3d1...2929`), `probe_code.coop` (`9156a13f...a90f`),
  `probe_sdram.coop` (`ae413921...bb17`). Each costs 25,000 cycles a render and loads one thing only: compute, 4-12 KB
  of code, or 64 cache lines of SDRAM through a 16-128 KB region.
- Runner scripts `tests/sim/run_m1_mode0.txt`, `run_m1_mode2.txt`, `run_m1_mode4.txt` (one counter mode each, 4 min
  33 s): idle, voltage, rings modal, rings sympathetic, probe_alu, probe_code 4 / 8 / 12 KB, probe_sdram's eight loads,
  rings modal with the four encoders turning; 10 s of a held six-note chord each.
- op1emu (`tests/sim/run_emu.sh` with the five plugins, the runner from the NAND) [C op1emu]: every section landed
  on the engine it names (`lost_sections 0`, `ctl_turns 22`: the variant steps the script asks for), no fault, and
  the block adds up (`cb = dsp + push + cb_tail + the rest`). op1emu has no caches, so every engine there leaves the
  same ~21.6 k a block beside its renders (the stock's per-voice work 19.4 k, coop's thunk 2.1 k): only the device
  can show what rings adds. The chord scenario with the thunks wired matches the plain image but for 25-36 samples
  1 LSB apart in the first 0.23 s (the boot's timing; a release image has none of this).

**What to do**:
1. Flash m1. Install `probe_alu.coop`, `probe_code.coop`, `probe_sdram.coop` (co-op Plugins > On your OP-1, or the
   SysEx page); rings 0.3 and saw stay. **The synth engine list must hold exactly the stock engines, saw, rings and
   the three probes**: the script picks engines by their row (remove or switch off any other synth plugin; effect
   plugins don't matter). Power-cycle.
2. Pick a sound slot in poly mode (the script changes its engine: voltage, rings, the probes; save anything you want
   to keep first), stay on the synth page.
3. The SysEx page, "Run a test": `run_m1_mode0.txt`, Run (recording on). It takes 4 min 33 s; don't touch the OP-1.
   Then `run_m1_mode2.txt`, then `run_m1_mode4.txt` (the same session, other counters).
4. Send the three `_run.txt` files (and the stats file of the last). If a section played the wrong engine (the
   recording or your ears), say which: the rows are the one thing op1emu could not prove for your list.

If the image crashes at boot (a photo), hold COM while switching on and flash e4e.

**Result (owner, 2026-11-10): PASS, the run is in.** Three runs (`m1_mode0_20261110-1645`, `m1_mode2_20261110-1656`,
`m1_mode4_20261110-1701`), every section on its engine, `lost_sections 0`, no crash; the last two sections
(`sdram_ws128k`, `rings_knobs`) did not fit the run file's 64 KB (raised to 128 KB). The slot had nitro in FX 1.
Tables in `docs/m1-results.md`, the reading in `plugins/rings/README.md` "m1: where the collateral goes": code
(coop's per-voice code refetched after every render) is the larger part, stores the other.

## m1b: the per-voice path in one place — device test (pending, 2026-11-10)

The first core lever from m1 (`plugins/rings/README.md` "m1: where the collateral goes"): coop's own per-voice code,
fetched again after every render once an engine's code is large.
- `build/pkg/op1_246_coop_m1b_voicepath.op1` (sha256 `5bcaececd1f823ee4687042b3b05199c6af4fe3cd872ff0f786391bc3b8143f5`),
  copy in Downloads. The m1 test image (`COOP_M1=1`, `g_coop_gap_prof=1`, the same thunks and scripts) plus:
  - the code a synth voice runs between two renders (the render thunk, LFO 2's stage 0, the counters, the cost
    bookkeeping, the dispatcher, the voice hook) in one section, `.text.coop_voice`, first in region A
    (`0x02c00020..0x02c009f8`, 2,520 bytes: 79 cache lines, every one in its own set of the 4-way instruction
    cache, so they cannot evict each other, and an engine's code has to fill a whole set to evict one);
  - the cold branches out of it (the debug compare, the drum-only code of the shared hook, the engine switch).
  `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr` differs. Build tag `m1b`.
- op1emu: `det_chord.txt` matches the previous image in each of op1emu's two run-to-run outcomes but for 31 samples
  1 LSB apart in the first 0.23 s (boot timing; no audio code changed, only where it sits).
  `run_m1_mode0.txt` with the five plugins: all 17 sections (the run file now holds 128 KB), no fault, every figure
  within 0.2 % of m1's (op1emu has no cache, so the gain can only show on the device).

**What to do**: as the m1 run, so the two compare: flash m1b, the same plugins, **the same sound slot with nitro in
FX 1 as last time**, and `run_m1_mode0.txt` once (mode 0 is enough). Send the `_run.txt`. What should move: the
`thunk` and `hook` columns and the collateral of `code4k` / `code12k` and both rings sections, against m1's.

**Result (owner, 2026-11-10): PASS, a gain for rings, a loss for 8-12 KB engines.** `m1_mode0_20261110-1828`,
all 17 sections, nitro on FX 1. Against m1 (callback over idle): rings sympathetic -33 k a block, modal -2 k,
probe_code 4 KB -44 k, probe_code 8 KB +12 k, 12 KB +24 k (set conflicts, `docs/m1-results.md` "m1b"). Rings' remaining
excess is now mostly stores.

## m1c: the three levers together — device test (pending, 2026-11-10)

The levers from m1b (`docs/m1-results.md` "m1b"), merged (`fw-m1c`), in one image and one plugin:
- `build/pkg/op1_246_coop_m1c_three.op1` (sha256 `0e74601c522ac41b9afe1720789761991d90cb5bbc7fef7db92fb103cf825ee0`),
  copy in Downloads. The m1 test image (`COOP_M1=1`, `g_coop_gap_prof=1`, the same thunks and scripts) plus:
  - **v2, the per-voice path cut** (`docs/engines.md` "v2"): its bookkeeping once a block, the profile's marks cold,
    LFO 2's `sync` once a block, no per-voice call out of the section; with the 44.1 kHz repeat it is 1,736 bytes
    (`0x02c00020..0x02c006e8`, 55 lines; m1b 2,520);
  - **the 44.1 kHz voice** (`docs/engines.md` "The 44.1 kHz voice"): `COOP_ENGINE_F_44K`, the core repeats each sample.
  `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr` differs. Build tag `m1c`.
- `build/plugins/rings.coop` **0.4** (sha256 `6fa7541e01cbc7ba751b79f84aebfd5f73ed562645bf67d8daea2b6e077de63c`,
  111,928 bytes), copy in Downloads: the per-block state in L1 (3,296 bytes leased; modal 86 -> 31 SDRAM lines written
  a block) and the 44.1 kHz voice (no Lagrange stage). Host: bit-identical with 0.3 at 88.2 kHz; at 44.1 kHz the even
  samples bit for bit (`plugins/rings/README.md` "0.4"). **An older core (e4e, m1, m1b) refuses this rings** ("engine
  flags not supported yet"): after going back to one, reinstall rings 0.3.
- op1emu (`run_m1_mode0.txt`, saw, rings 0.4 and the three probes) [C op1emu, no cache]: all 17 sections, `lost_sections
  0`, no fault; rings modal renders 68.4 k a block (m1: 72.9 k), coop's thunk 1.2 k (2.1 k), the dispatcher 0.7 k (0.9 k).

**What to do**: as m1b: flash m1c, **replace rings with 0.4** (the same name, so the same row; the probes and saw stay),
the same sound slot with nitro in FX 1, `run_m1_mode0.txt` once, send the `_run.txt`. What should move against m1b:
the 8-12 KB code probes (v2), both rings sections (all three), `thunk` and `hook` everywhere.

**Result (owner, 2026-11-10): partial, run again.** `m1_mode0_20261110-2110`, all 17 sections, no fault, rings 0.4
loaded on the 44.1 kHz voice with its lease; but no nitro on FX 1 and two variant steps lost (`rings_symp` played
modal, `code12k` played 8 KB; `docs/m1-results.md` "m1c"). Rings modal -28.5 k a block against m1b, probe_code 8 KB
-8.8 k. The scenario now positions every variant absolutely (`run_m1_mode0.txt` regenerated, 5 min 4 s): run it again
in the m1b slot with nitro on FX 1.

**Result, second run (owner, 2026-11-10): PASS.** `m1_mode0_20261110-2136`, nitro on FX 1, every variant switched, no
fault. Against m1b, each section over idle: rings modal -29.2 k, sympathetic -13.0 k, knobs turning -25.9 k, probe_code
8 KB -24.3 k, 12 KB -29.3 k, the rest -3 to -5 k; v2's once-a-block folds cost 5-6 k of that back (`cb_rest`). Rings'
peaks still pass the budget with sympathetic (684 k) and the knobs turning (602 k). `docs/m1-results.md` "m1c, second run".

## e5: release image, rings 0.5 — device test (pending, 2026-11-10)

The owner left rings at 0.4's performance (m1c) and asked for polyphony "all" back and controls that wrap.
- `build/pkg/op1_246_coop_e5.op1` (sha256 `89b4b2cfdb5ea186495045c1f8773367416e1c6f46252f7a79ef0f14223024c3`), copy in
  Downloads. A **release** image: m1c's core (v2's per-voice path, the 44.1 kHz voice) without the m1 measuring (no
  `COOP_M1`, no `g_coop_gap_prof`), `--tape-stereo --set-word g_coop_spread=32767`, plus `"wrap"` for steps controls
  (`core/sdram/engine_ui.cc`; `docs/engines.md` "Controls"). `verify_bootloader.py` PASS, `pkg_check` = only
  `OP1_vdk.ldr` differs. Build tag `e5`.
- `build/plugins/rings.coop` **0.5** (sha256 `8fcd30d13575e1239c9920b200256f83aa0c5c6fe71fa0ba1cfd90c726fb477b`, 113,048
  bytes): polyphony 1, 2, 4, all (default 4), fn + blue and fn + green wrap (`plugins/rings/README.md` "0.5").
- [C host] the new order plays bit for bit what 0.4 played for 1, 2, 4, and "all" what the uncapped build plays.
- [C op1emu] `tests/sim/run_rings_wrap.txt` (`gen_run_wrap.py`) on e5: one model step 144,174 a block, six more
  144,181 (the same model: wrapped), one back 149,758 (the start's 149,593); polyphony one step to "all" 205,883, four
  more 205,913 (wrapped). 7 sections, no fault.

**What to do**: flash e5, install rings 0.5. fn + blue: past verb comes modal, and back; fn + green: 1, 2, 4, all, 1.
Play "all" (six voices) and say how it sounds. A sound saved with rings 0.3 / 0.4 opens one polyphony step off (the
word is the step index: 0 was 4, is now 1).

Result: (pending)


## e6: effect controls (fn + turn, its fine, clicks, latches) — device test (pending, 2026-11-11)

The owner's decisions of 2026-11-11 (`docs/ui-features.md` §14), for the eleven kennel effect ports: effects get the
engines' control layers at every effect position (`docs/plugins.md` "Effect controls").
- `build/pkg/op1_246_coop_e6.op1` (first build sha256 `724f98ba...`; rebuilt after the port merge: `e8a9352a...`, the same `OP1_vdk.ldr`, below), copy in Downloads. A **release** image as e5 (no
  `COOP_M1`, no gap profile, `--tape-stereo --set-word g_coop_spread=32767`), plus e6. `verify_bootloader.py` PASS,
  `pkg_check` = only `OP1_vdk.ldr` differs. Build tag `e6`.
- `build/plugins/test_fx.coop` (sha256 `64e09115802e02f2d811d33655a8801d7802db4a56ecf17adea8fbb3be2e466d`, 432 bytes): the test effect, not a real plugin. fn + blue = gain
  (16384 = unity, 0..2x; 1365 a detent as the ports' 0.5 dB), shift + fn + blue = the same gain by 273 (0.1 dB), green click = polarity (normal / invert),
  fn + white = depth (shown, does nothing), orange click = mute (a latch).
- [C op1emu] `tests/sim/det_fx_controls.txt` then `det_fx_reload.txt` (a power cycle on the same NAND): every position
  received the words and the latch, and kept the words but not the latches (figures in the gate notes below).
- rings 0.5, saw and stereo_delay need no new files: their `.coop` are byte-identical when repacked.

**What to do** (install `test_fx.coop` with the librarian, power-cycle; play something through each effect):
1. Synth: T3, shift + T3, pick **test_fx**, T3 (FX 1 on). Hold Help and turn blue left a few detents: the sound gets
   quieter, "GAIN 41"-style text top left in blue, 4 % a detent (test_fx's gain is linear; the ports' gain words
   make the same 1365 a detent 0.5 dB). Hold Shift + Help and turn blue: 273 a detent, the percentage moves about one
   in every 1.2 detents. Click green: the sound inverts polarity (hard to hear alone; with FX 2 also on it may thin); "POLARITY
   INVERT" in green. Click orange: silence, "MUTE ON" in red; click again: back. Help + white: "DEPTH" moves, the sound
   doesn't. Shift + Help + white: nothing (undeclared). A plain Help tap still opens help.
2. Drop (FX 2), shift + blue to test_fx, blue push, T3 (on): repeat 1 on FX 2 (instrument).
3. Mixer T3: master FX test_fx (shift + T3, pick, T3 on): Help + blue, orange click. Drop: master FX 2, the same.
4. Drum: T3, pick test_fx, T3: Help + blue, green click.
5. Power-cycle. Gain, polarity and depth are as you left them at every position; every mute is off.
6. Leave mute on at the synth, switch to drum and back: mute is off (the stock restarts the instrument effect;
   accepted by the owner, 2026-11-11).
6b. Randomise: on the synth's FX page with test_fx (FX 1 on), hold the synth key and press right a few times:
   polarity and depth change (the popup does not show; check the depth by Help + white one detent), **gain stays**
   (`"random": false`), mute stays as it was. The same on the instrument's FX 2 with test_fx: the four dials move too.
   Left arrow with the synth key held steps back.
7. Copy `/yaffs2/user/coop_stats.txt`: the `fxctl` line.

**If something is wrong:** power off; recovery: hold COM at power-on and flash `op1_246_coop_e5.op1` (or stock).

**Gate notes (op1emu, e6, 2026-11-11)** [C op1emu]:
- (First runs, test_fx before the step fields: 328 / 20.5 a detent.) `det_fx_controls.txt`, test_fx alone on the NAND: `fxctl fx 1 turns 11 fine 1 clicks 3 latches 4 ignored 1
  cancelled 1 popup_frames 405 from_params 0 master_saves 1 p0=36117i2:15092/1/8520/0 p1=52852i2:16712/1/8192/0
  p2=71596i1:16056/0/8192/1 p3=81313i1:17040/1/8192/0 p4=63052i1:16384/0/8848/0` (pN = blocks, inits, words 5/6/7 and
  the latch word the render was last given). Expected per position: 15092 = 16384 - 4 x 328 + 20 (a fine detent),
  17040 = + 2 x 328, 16056 = - 328, 16712 = + 328, 8520 / 8848 = 8192 + 1 / 2 x 328. The p0 latch was on (1) until the
  drum visit restarted the instrument effect (i2); p2's stayed on. `plugins seen 1 loaded 1 failed 0`, no fault.
- `det_fx_reload.txt` (power cycle, same NAND): `fxctl ... p0=80898i2:15092/1/8520/0 p1=2402i1:16712/1/8192/0
  p2=83300i1:16056/0/8192/0 p3=82695i1:17040/1/8192/0 p4=82695i1:16384/0/8848/0`: every word kept, every latch off.
  op1.db master `fx_params` `[7296,12288,11264,20479,21280,16056,0,8192]` (before the stock writer was called the
  master's 16056 came back as 16384: the reason for `master_save_service`). coop_slot2.bin carries the `'X', 1` words.
- Regression against e5 (the e5 core rebuilt from 94be6bd, same image flags): pristine NAND `det_chord.txt`
  identical over 3,506,560 frames; stereo_delay `det_fx_select.txt` identical over 1,937,088 frames (`plugin_blocks
  8188` both, e6 `fxctl fx 0`); saw `det_eng_controls.txt` on e6: `ctl_turns 1 ctl_clicks 1 ctl_ignored 1
  ctl_cancelled 1 help_taps 1`, slot 0 knobs `[20000,8000,6000,4000,21280,1,0,1]`, as e4.
- The e6 probe (`COOP_PROBE_ITER`): `filter_ranges [64,14528,14464] [0,24576,10485] [0,22528,32767] [8192,32767,10485]
  [0,32767,0] [0,32767,0] [0,32767,0] [0,32767,0]`: words 5..7 of the filter type accept 0..32767.
- **Re-run on a quiet machine (2026-11-11, dev after the nine effect ports merged; the core is 50e7dbd's), one run at
  a time, image rebuilt** (`e6.ldr` sha256 `09e6923d...`, release core, `--tape-stereo --set-word g_coop_spread=32767
  --set-word g_coop_force_stereo_out=1`) [C op1emu]. Gain steps and randomise as built:
  - `det_fx_controls.txt`, test_fx alone: `fxctl fx 1 turns 11 fine 1 clicks 3 latches 4 ignored 1 cancelled 1
    popup_frames 406 from_params 0 master_saves 1 random 2 p0=36116i2:11197/1/24265/0 p1=63191i2:17749/1/8192/0
    p2=81935i1:15019/0/8192/1 p3=91651i1:19114/1/2203/0 p4=73390i1:16384/0/8848/0`, `rnd 2 rnd_refused 0 rnd_slot2 1`,
    `plugins seen 1 loaded 1 failed 0`, no fault. Every expected gain word (11197, 19114, 15019, 17749: the 1365 / 273
    steps), depth drawn at p0 and p3 (8520 -> 24265, 8192 -> 2203), gain kept by randomise, p2's latch on, p0's
    cleared by the drum visit. Cost (test_fx): `fx1_plugin=3068/3630 s2_instr=1804/2916 s2_master=1850/2117`.
  - `det_fx_reload.txt` (same NAND): `p0=84349i2:11197/1/24265/0 p1=2402i1:17749/1/8192/0 p2=86751i1:15019/0/8192/0
    p3=86146i1:19114/1/2203/0 p4=86146i1:16384/0/8848/0`: every word kept, every latch 0.
  - stereo_delay `det_fx_select.txt` e6 vs e5: identical over 1,559,021 frames; `fxctl fx 0`.
  - saw `det_eng_controls.txt` on e6: `help_taps 1 ctl_turns 1 ctl_clicks 1 ctl_ignored 1 ctl_cancelled 1`, slot 0
    knobs `[20000,8000,6000,4000,21280,1,0,1]`, as e4.
  - **Pristine `det_chord.txt`, e6 vs e5: no regression.** The first pair differed from frame 1,395,072 (31.634 s:
    "64 vs 63", max 5120); aligned on the onset, max 10 LSB. Repeating each image showed why: **e5 against itself**
    differs the same way at the same frame, and so does e6 against itself, while **e5 (second run) vs e6 (first run)
    is identical over 3,501,612 frames**. The chord's onset lands on one block boundary or the next from run to run of
    one image, on a quiet machine too: `det_chord.txt` is not exact run to run any more (it was when the earlier gates
    ran), whichever image. A gate comparison has to try a second run (or `--align 200`) before calling a difference.
    The cause in the harness is not found [I].
  - **Combined NANDs (the nine ports, rings, saw)** [C op1emu], `det_fx_select.txt` (a longer wait where it hung):
    - **At most 8 files load** (`plugin_host.cc` `MAX_FILES 8`): with all 11 on the NAND, 8 loaded (saw, tape_drive,
      bitcrusher, ensemble, rings, cs_plate, drive, tape_echo: directory order) and freq_shift, wavefolder and
      cs_hall were skipped without a word (`plugins seen 8 loaded 8 failed 0`). That set booted, no fault,
      `heap_largest 8388608`, tape_echo at the synth `fx1_plugin=16614/18055`.
    - **A boot hang above ~2.1 MB of plugin memory.** Sets by load (effects x 5 relocations, engines x 1): rings +
      tape_echo + cs_plate + saw + freq_shift (1.89 MB), + wavefolder instead (1.87 MB), rings + cs_plate + saw +
      ensemble + freq_shift + tape_drive + wavefolder (1.39 MB), the 11-file set's 8 above (2.05 MB), and the eight
      heaviest effects without rings (tape_echo, cs_hall, cs_plate, bitcrusher, ensemble, freq_shift, tape_drive,
      wavefolder: 1.97 MB) all booted and played. **rings + tape_echo + cs_hall + cs_plate (2.34 MB)**, and rings +
      tape_echo + cs_plate + saw + ensemble + freq_shift + tape_drive + wavefolder (2.11 MB), and the same with
      cs_hall for saw (2.62 MB) **never reached audio**: the screen stays on the first-boot tape FAT creation at
      "side_b.aif 94 %" (3.5 min of wall time), no fault, no stats file. The loader finished (op1.db's
      `coopplugins` map numbers all eight), so the stock's own boot runs out of something afterwards, most likely
      heap 0 [I]. `heap_largest` stays 8,388,608 in every run that booted, so it does not show this margin [I].
      Not tried on a NAND that has booted once (the FAT exists), nor on the device [I]: until then, keep rings,
      tape_echo and cs_hall from being installed together.
- **Package** (2026-11-11, rebuilt from dev after the merge): `build/pkg/op1_246_coop_e6.op1` sha256
  `e8a9352a1ab9afd0992352eeb723733e9de3767ed9559dd2766f332173fe325a`, copy in Downloads; `verify_bootloader.py` PASS,
  `pkg_check` = only `OP1_vdk.ldr` differs. Its `OP1_vdk.ldr` (sha256 `73cc1fe6...`) is byte-identical to the earlier
  e6 package's (`724f98ba...`): the core did not change with the ports, only the archive's hash did.
- Unproven [I]: device timing and feel; the master writer called from coop's main loop (op1emu: one call, no fault);
  the drum and master pages' stock handling of encoder clicks (op1emu: coop takes them first, nothing else reacts);
  the popup's look over the filter page (counted, not seen: op1emu screenshots are wall-timed).

Result: (pending)


## bitcrusher 0.1 (port of the kennel bitcrusher, e6 controls) — device test (pending, 2026-11-11)

- On the e6 package (`build/pkg/op1_246_coop_e6.op1`, above); `build/plugins/bitcrusher.coop` (1,680 bytes with the gain steps, sha256
  `d5286b0fa70e0b2e41b0c2a48531354ed557c2213c9f51c99247734d9a85f90a`),
  installed with the librarian, then a power cycle. Notes: `plugins/bitcrusher/README.md`.
- [C host] bit-identical to te-op1-cfw's crush core and kennel mapping (`tests/host/bitcrusher`). op1emu gate: below.

- **op1emu gate (2026-11-11, e6 image `09e6923d...` from dev after the merge, bitcrusher alone on the NAND, one run at a time)** [C op1emu]:
  - `plugins seen 1 loaded 1 failed 0`, no fault in any run. In the alphabetical stock FX list it is row 0 (first: `ccw 96`).
  - `det_fx_select.txt` (synth FX 1, the chord): `fx1_plugin=6467/8095`.
  - `det_fx_controls.txt` (FX 1 picks `ccw 96`): `p0=36860i2:11197/0/0/0 p3=84765i1:19114/0/0/0 p2=74691i1:15019/0/0/0 p4=66504i1:16384/0/0/0 p1=57051i2:17749/0/0/0` (w5/w6/w7/latch; gain 11197 / 19114 / 15019 / 16384 / 17749: the 1365 / 273 steps), `master_saves 1`, `rnd 2 rnd_slot2 1`; `det_fx_reload.txt`: the same words after the power cycle, every latch 0. Cost: `fx1_plugin=6446/7415 s2_instr=5586/6254 s2_master=5440/5486`.
  - Costs are op1emu figures (cycles a block, avg/peak); the device is typically 10-35 % higher. Well under 30 k at every metered position. Master FX 1 has no cost slot of its own (`fx1_plugin=` is the instrument FX 1; the master runs the same render) [I].

**What to do** (play a chord or a drum loop through each):
1. Synth: T3, shift + T3, pick **bitcrusher**, T3 (FX 1 on). Blue clockwise: a higher rate (cleaner), anticlockwise
   down to 500 Hz (heavy aliasing). Green clockwise: more bits; anticlockwise down to 2 bits. White: jitter (the
   rate gets rough, noisy). Orange: mix; at the bottom the dry sound unchanged.
2. Hold Help and turn blue: "GAIN" top left in blue; the crusher is driven harder (+6 dB at the top, clipping) or
   softer, 0.5 dB a detent; with mix at 0 the sound must not change. Shift + Help + blue: 0.1 dB a detent. Clicks and the
   other encoders' Help layers do nothing.
3. Spread up, a chord at the synth position: both sides crush, the placement kept (stereo at every position is new).
4. The same on FX 2 (drop), on the master (mixer T3) and its FX 2, and on the drum.
5. Power-cycle: the gain is as you left it at every position.
6. Copy `/yaffs2/user/coop_stats.txt`: the `cost` line (`fx1_plugin=`, `s2_instr=`, `s2_master=`) and `fxctl`.
   Budget: under 30 k a block (simulator: 5.3 k avg, 6.7 k max).

**If something is wrong:** delete `bitcrusher.coop` with the librarian and power-cycle (sounds saved on it play the
filter until it returns); recovery as e6.

## drive 0.1: the kennel drive effect on coop — device test (pending, 2026-11-11)

Needs the e6 core (effect controls). `build/plugins/drive.coop` (9,616 bytes with the gain steps, sha256
`a56f197a752e8df8a21ca17fa56c51ce5fd066be27be9a4faf72a5bb8ad9484b`), `plugins/drive/README.md`.

- **op1emu gate (2026-11-11, e6 image `09e6923d...` from dev after the merge, drive alone on the NAND, one run at a time)** [C op1emu]:
  - `plugins seen 1 loaded 1 failed 0`, no fault in any run. In the alphabetical stock FX list it is row 2 (after cwo and delay: `ccw 96` then `cw 16`; `tests/sim/det_fx_drive_select.txt` now does that).
  - `det_fx_select.txt` (synth FX 1): `fx1_plugin=7617/10484`.
  - `det_fx_controls.txt` (FX 1 picks `ccw 96`, `cw 16`): `p0=35291i2:11197/0/0/0 p3=70992i1:19114/0/0/0 p2=60007i2:15019/0/0/0 p4=52731i1:16384/0/0/0 p1=41705i2:17749/0/0/0` (w5/w6/w7/latch; gain 11197 / 19114 / 15019 / 16384 / 17749: the 1365 / 273 steps), `master_saves 1`, `rnd 2 rnd_slot2 1`; `det_fx_reload.txt`: the same words after the power cycle, every latch 0. Cost: `fx1_plugin=7615/10484 s2_instr=6348/8837 s2_master=6395/8884`.
  - Costs are op1emu figures (cycles a block, avg/peak); the device is typically 10-35 % higher. Well under 30 k. Master FX 1 has no cost slot of its own (`fx1_plugin=` is the instrument FX 1; the master runs the same render) [I].

**What to do** (install `drive.coop` with the librarian, power-cycle):
1. Synth: T3, shift + T3, pick **drive** (between delay and grid), T3 (FX 1 on). Play a chord. Blue (drive) up: it
   saturates, the level stays about the same (auto-level). Green (tone): darker left, brighter right, flat at the
   centre. White (bias): fuller, more even harmonics. Orange (mix): fully left is the dry sound exactly.
2. Hold Help and turn blue: `GAIN` top left in blue; more grit up, less down, the dry part unchanged. Shift + Help +
   blue: the same gain, 0.1 dB a detent (Help + blue: 0.5 dB). Other encoders with Help: nothing.
3. Check the stereo survives: a wide chord (voice spread) through drive at the synth stays wide.
4. The same at the drum (T3 on the drum page), the master (mixer T3), and in FX 2 of the instrument and the master.
5. Power-cycle: the gain is where you left it at every position.
6. Copy `/yaffs2/user/coop_stats.txt`: the `cost` line (`fx1_plugin=` for FX 1, `s2_instr=` / `s2_master=` for
   FX 2; budget under 30 k a block, simulator 5.5 k still / 8 k peak moving) and the `fxctl` line.

**If something is wrong:** power off; delete `drive.coop` over SysEx or recover with COM at power-on.

## ensemble 0.3: the kennel ensemble as a coop effect, stereo everywhere — device test (pending, 2026-11-11)

- `build/plugins/ensemble.coop` (10,828 bytes with the gain steps, sha256
  `2a0873d264ef56d8b5621cdfb01355fdda2ca3ca64dc584386249d4b526b92d6`),
  `plugins/ensemble` (`README.md` has the controls, the cost plan and the gate plan). Needs the e6 package
  (`op1_246_coop_e6.op1`): the file declares a control, so a core before e6 refuses it ("plugin flags not supported
  yet"). Install from the librarian, power-cycle.
- Host and simulator: bit-identical to te-op1-cfw's kennel ensemble (`make -C tests/host/ensemble`, `sim`); coop render
  12,801 mean / 14,823 max cycles a block in bfin-elf-run (no cache model). op1emu gate: below.

- **op1emu gate (2026-11-11, e6 image `09e6923d...` from dev after the merge, ensemble alone on the NAND, one run at a time)** [C op1emu]:
  - `plugins seen 1 loaded 1 failed 0`, no fault in any run. In the alphabetical stock FX list it is row 2 (`ccw 96` then `cw 16`).
  - `det_fx_select.txt` (synth FX 1): `fx1_plugin=13522/14588`. `det_fx_ensemble.txt` (all four turns, white top and bottom: both kernels; its final `wait` lengthened to 75 s, the first run ended before its events): `p0=58371i1:20479/0/0/0` (gain + 3 x 1365), `fx1_plugin=12679/16085`.
  - `det_fx_controls.txt` (FX 1 picks `ccw 96`, `cw 16`): `p0=35289i2:11197/0/0/0 p3=74433i1:19114/0/0/0 p2=63450i2:15019/0/0/0 p4=56173i1:16384/0/0/0 p1=45147i2:17749/0/0/0` (w5/w6/w7/latch; gain 11197 / 19114 / 15019 / 16384 / 17749: the 1365 / 273 steps), `master_saves 1`, `rnd 2 rnd_slot2 1`; `det_fx_reload.txt`: the same words after the power cycle, every latch 0. Cost: `fx1_plugin=13725/15176 s2_instr=12493/13871 s2_master=12161/13370`.
  - Costs are op1emu figures (cycles a block, avg/peak); the device is typically 10-35 % higher. Under 30 k. Master FX 1 has no cost slot of its own (`fx1_plugin=` is the instrument FX 1; the master runs the same render) [I].

**What to do**:
1. Synth: T3, shift + T3, pick **ensemble**, T3 (FX 1 on). Play a chord. Turn white (voices) from the bottom (a plain
   chorus) to the top (three copies a side with a vibrato: the string ensemble); green (depth) and blue (rate) up and
   down; orange (mix) from dry to wet. Listen for clicks while turning, most of all on white near the bottom (the
   kernel switch) and on rate at the top.
2. Hold Help and turn blue: "GAIN" top left in blue, the wet part louder or quieter (mix up to hear it; at mix 0
   nothing changes), 0.5 dB a detent. Shift + Help + blue: 0.1 dB a detent.
3. Headphones: the chorus is wide at the synth position too (new on coop: kennel ran it mono there).
4. The same at drum FX 1, master FX 1 and both FX 2 slots (drop, shift + blue to ensemble, blue push, T3).
5. Switch FX 1 off (T3) for a few seconds with notes playing and on again: no burst of old sound (the restart passes
   one block dry).
6. Save the sound, power-cycle: the gain comes back as left.
7. Copy `/yaffs2/user/coop_stats.txt`: the `cost` line (`fx1_plugin=`, `s2_instr=`, `s2_master=`) from the busiest
   moment (white at the top, a six-note chord), and `fxctl`.

Compare by ear with the kennel build on hundekraft if you still have it (master position: it should sound the same).

## freq_shift 0.1: the kennel frequency shifter as a coop effect — device test (pending, 2026-11-11)

`plugins/freq_shift` (`docs/plugins.md` "Port: freq_shift", `plugins/freq_shift/README.md`). Needs the e6 core
(`op1_246_coop_e6.op1`: effect controls).
- `build/plugins/freq_shift.coop` (10,308 bytes with the gain steps; sha256
  `d73a8279ec6cdf6f22a62ca146bae677b9114e747c2f5fde63649b4f93602761`). Load size 14,168 bytes, five relocations.
- [C host] bit-identical to te-op1-cfw's DSP per channel and, with L == R, to its stereo path; [C GNU simulator] the
  assembly equals the host C; 9,762 / 14,383 cycles a block (one pair / two). op1emu gate: below.

- **op1emu gate (2026-11-11, e6 image `09e6923d...` from dev after the merge, freq_shift alone on the NAND, one run at a time)** [C op1emu]:
  - `plugins seen 1 loaded 1 failed 0`, no fault in any run. In the alphabetical stock FX list it is row 2 (`ccw 96` then `cw 16`).
  - `det_fx_select.txt` (synth FX 1): `fx1_plugin=10561/13428`. `det_fx_freq_shift.txt` (the shift through 0 Hz, feedback, spread, ring and back, gain with its fine; its final `wait` lengthened to 75 s): `fxctl ... clicks 2 ... p0=41475i1:8740/0/0/0` (16384 - 6 x 1365 + 2 x 273), `fx1_plugin=11489/15753`.
  - `det_fx_controls.txt` (FX 1 picks `ccw 96`, `cw 16`): `p0=35289i2:11197/0/0/0 p3=67541i1:19114/0/0/0 p2=56557i2:15019/0/0/0 p4=49280i1:16384/0/0/0 p1=38255i2:17749/1/0/0` (w5/w6/w7/latch; gain 11197 / 19114 / 15019 / 16384 / 17749: the 1365 / 273 steps), `master_saves 1`, `rnd 2 rnd_slot2 1`; `det_fx_reload.txt`: the same words after the power cycle, every latch 0. The mode: p1 1 (green click); p0 and p3 drawn by randomise after their clicks (both drew 0). Cost: `fx1_plugin=11159/16151 s2_instr=9776/14129 s2_master=8859/12211`.
  - Costs are op1emu figures (cycles a block, avg/peak); the device is typically 10-35 % higher. Under 30 k. Master FX 1 has no cost slot of its own (`fx1_plugin=` is the instrument FX 1; the master runs the same render) [I].

**What to do** (install `freq_shift.coop` with the librarian, power-cycle; play a sustained sound through it):
1. Synth: T3, shift + T3, pick **freq shift**, T3 (FX 1 on). Orange up (wet). Turn blue slowly either side of the
   middle: near the middle a slow phasing / beating, further out the chord turns clangorous, up on one side, down on
   the other (cubic: fine near 0, ±2 kHz at the ends). No zipper noise while turning.
2. Green up: the barber-pole (a sweep that climbs or falls without end, the way the shift points). Top feedback must
   stay bounded and die to silence within about a second of the sound stopping.
3. White up: L and R move apart (L up, R down): the image widens. With white at 0, L and R are the same shift.
4. Click green: `MODE RING` (green, top left), the sound becomes ring modulation (both sidebands, quieter by about
   6 dB, no click on the switch: a 6 ms crossfade). Click again: `MODE SHIFT`.
5. Hold Help, turn blue: `GAIN` (blue) moves, the wet gets quieter / louder (-6 .. +6 dB), the dry part doesn't.
   0.5 dB a detent. Hold Shift + Help and turn blue: 0.1 dB a detent.
6. Orange to 0: exactly the dry sound (compare with FX 1 off: no difference).
7. Repeat at the drum (drum T3), the master (mixer T3), and FX 2 of the instrument and the master (drop): the same
   behaviour; on the master the spread widens the whole mix.
8. A panned sound (synth voices spread, or a stereo source on the master) through a small shift: the placement stays
   (left voices stay left): the reason for a Hilbert pair per channel.
9. Power-cycle: gain and mode as you left them at every position.
10. Copy `/yaffs2/user/coop_stats.txt`: the `cost` line (`fx1_plugin=`, `s2_instr=`, `s2_master=`: avg / peak), the
    `fxctl` and `plugins` lines. Expect well under 30 k a block [I: the simulator gives ~10-16 k; the device adds
    cache and SDRAM waits].

**If something is wrong:** power off; remove the file with the librarian (or recovery: hold COM at power-on and flash
`op1_246_coop_e5.op1` or stock).

## tape_drive 0.4: the kennel tape_drive ported to coop — device test (pending, 2026-11-11)

**What:** `build/plugins/tape_drive.coop` (`make -C plugins`).
- The file is 11,064 bytes; its load size is 13,912 bytes, x 5 relocations.
- It needs a core with e6: the file has controls, so a core before e6 refuses it.
- Port notes: `plugins/tape_drive/README.md`.
- Host test (`make -C tests/host/tape_drive`): bit-identical to te-op1-cfw's DSP as kennel drove it, both on the
  host (C kernels) and in the simulator (assembly kernels). Simulator cost: avg 9,061, peak 9,286 cycles a block.

**Do:**
1. Install `tape_drive.coop` over SysEx, then power-cycle.
2. Put `tape drive` on FX 1 of a synth sound and play a chord.
   - Blue is saturate.
   - Green is wobble: wow, then flutter past half.
   - White is age: clockwise is newer and brighter.
   - Orange is mix: fully left is the dry sound, unchanged.
3. fn + blue is the gain (`GAIN` top left in blue), -6..+6 dB into the saturation only.
   - At mix 0 nothing changes.
   - With saturate up you get more or less grit, and the level stays about the same.
   - 0.5 dB a detent; shift + fn + blue moves the same control 0.1 dB a detent.
4. Repeat at the drum, the master, FX 2 of the instrument and FX 2 of the master. The instrument positions now hear
   the effect in stereo (kennel ran them mono): the voice spread should survive.
5. Save the sound, power-cycle and reload: the gain should be kept.
6. Listen for clicks on knob moves and in silence: no hiss and no DC, so silence in should give silence out.
7. Copy these lines from `/yaffs2/user/coop_stats.txt`:
   - the `cost` line (`fx1_plugin=`, `s2_instr=`, `s2_master=`);
   - the `plugins` and `fxctl` lines.
   - Expected: about 10-13 k a block at one position [I]. Over 30 k is a finding.

**If something is wrong:** power off, then delete the plugin file (the position plays the filter until the file
returns), or recover as for e6.

## tape_echo 0.1: the kennel tape echo as a coop effect — device test (pending, 2026-11-11)

- `build/plugins/tape_echo.coop` (sha256 `757a047b31e9a6692907b149de1bbe3645bb0c75834bed12100ce1846c3416ae`, 10,664
  bytes), on an image with e6 and its per-control steps (dev 50e7dbd: gain declares `"step"` / `"fine_step"`, which an
  older core refuses). Contract: `plugins/tape_echo/README.md`.
- [C host, GNU simulator] bit-identical with te-op1-cfw's tape echo and the kennel glue, sync included against
  kennel's tempo lock at 120 BPM (`make -C tests/host/tape_echo` 20,000 blocks; `sim`, 3,000 blocks with the
  assembly). Simulator compute 15,880 / 16,637 cycles a block.

- **op1emu gate (2026-11-11, e6 image `09e6923d...` from dev after the merge, tape_drive alone on the NAND, one run at a time)** [C op1emu]:
  - `plugins seen 1 loaded 1 failed 0`, no fault in any run. In the alphabetical stock FX list it is row last (after spring: `cw 96`).
  - `det_fx_select.txt` (synth FX 1): `fx1_plugin=10931/11837`.
  - `det_fx_controls.txt` (FX 1 picks `cw 96`, unchanged): `p0=36117i2:11197/0/0/0 p3=67542i1:19114/0/0/0 p2=57825i1:15019/0/0/0 p4=49281i1:16384/0/0/0 p1=39081i2:17749/0/0/0` (w5/w6/w7/latch; gain 11197 / 19114 / 15019 / 16384 / 17749: the 1365 / 273 steps), `master_saves 1`, `rnd 2 rnd_slot2 1`; `det_fx_reload.txt`: the same words after the power cycle, every latch 0. Cost: `fx1_plugin=10931/12207 s2_instr=9666/10530 s2_master=9712/9966`.
  - Costs are op1emu figures (cycles a block, avg/peak); the device is typically 10-35 % higher. Under 30 k. Master FX 1 has no cost slot of its own (`fx1_plugin=` is the instrument FX 1; the master runs the same render) [I].

- **op1emu gate (2026-11-11, e6 image `09e6923d...` from dev after the merge, tape_echo alone on the NAND, one run at a time)** [C op1emu]:
  - `plugins seen 1 loaded 1 failed 0`, no fault in any run. In the alphabetical stock FX list it is row last (`cw 96`).
  - `det_fx_select.txt` (synth FX 1): `fx1_plugin=16778/18312`. `det_fx_tape_echo.txt`: `fxctl fx 1 turns 11 fine 1 clicks 5 latches 4 ignored 2 cancelled 1 ... p0=36117i2:11197/0/1/0 p1=39076i2:17749/0/1/0 p2=57821i1:15019/0/0/1 p3=67537i1:19114/0/1/0 p4=49276i1:16384/3/2/0`: every word and count as the scenario expects; `fx1_plugin=16787/18293 s2_instr=15536/16615 s2_master=15690/16237`.
  - `det_fx_controls.txt` (test_fx's walk on tape_echo's layers): `p0=36116i2:11197/1/1/0 p1=42523i2:17749/0/0/0 p2=61266i1:15019/0/0/0 p3=70982i1:19114/2/1/1 p4=52721i1:16384/0/0/0` (sync and heads drawn by randomise at p0 and p3; the green click left hold on at p3), `master_saves 1`; `det_fx_reload.txt`: the same words, every latch 0. Cost `fx1_plugin=16821/18176 s2_instr=15632/16447 s2_master=15581/16140`.
  - Costs are op1emu figures (cycles a block, avg/peak); the device is typically 10-35 % higher. Under 30 k. The restart's ~25 ms of dry is accepted (the owner, 2026-11-11): listen for it. Master FX 1 has no cost slot of its own (`fx1_plugin=` is the instrument FX 1; the master runs the same render) [I].

**What to do** (install `tape_echo.coop` with the librarian, power-cycle; play through it):
1. Synth: T3, shift + T3, pick **tape echo**, T3 (FX 1 on). Play short notes: echoes. Blue turn: the echo time (the
   pitch bends while it moves, no clicks); green: more repeats, past ~90 % it sustains and howls (bounded); white:
   the repeats get darker and wobble; orange: dry to wet.
2. White click: the head pattern steps 3, 2+3, 1+2+3, 1+3 ("HEADS 2+3" top left in white), and wraps.
3. Green click: hold ("HOLD ON" in green): new notes don't enter, the tape keeps looping what it has and fades
   slowly. Click again to release.
4. Hold Help and turn blue: "GAIN" moves 0.5 dB a detent (the level into the tape, not the dry); Shift + Help +
   blue: the same, 0.1 dB a detent.
4b. Hold Help and turn green: "SYNC 1/8" etc. in green. The echo time jumps (glides) to that division at 120 BPM
   (1/8 = 250 ms, 1/4 = 500 ms, 1/2 = 1 s) and the blue turn no longer moves it; back to `OFF` and blue sets the time
   again. It doesn't wrap at either end. Power-cycle: the setting is kept.
5. The same at drum FX 1, master FX 1 and both FX 2 slots (drop); listen for stereo (L and R echo separately).
6. Power-cycle: gain, sync and heads kept, hold off. Hold on at the synth, switch to drum and back: hold is off
   (accepted).
7. Bypass (T3) and back on: the old echoes are gone (the tape starts clean), no click; the first ~25 ms after a
   (re)start pass dry (the tape clear; accepted as it is by the owner, 2026-11-11): listen for it.
8. Copy `/yaffs2/user/coop_stats.txt`: the `cost` line (`fx1_plugin=`, `s2_instr=`, `s2_master=`) and `fxctl`.
   Expected [I]: 20-35 k a position; over 60 k is a bug.

**If something is wrong:** power off; remove the file with the librarian, or recovery: hold COM at power-on and flash
the previous coop image (or stock).

## wavefolder 0.1 (a kennel effect port) — device test (pending, 2026-11-11)

Needs the e6 image (`op1_246_coop_e6.op1`, effect controls). `build/plugins/wavefolder.coop` (9,980 bytes; sha256 at
packing `ad38fe00930b456bae51b54ed0a14d3d9ba0fbce1a9a6849bd2aca51ad1c6eb8`); contract in `plugins/wavefolder/README.md`.
- [C host] bit-identical to te-op1-cfw's FOLD as the kennel plugin drove it (`tests/host/wavefolder`, native and
  simulator). op1emu gate: below.

- **op1emu gate (2026-11-11, e6 image `09e6923d...` from dev after the merge, wavefolder alone on the NAND, one run at a time)** [C op1emu]:
  - `plugins seen 1 loaded 1 failed 0`, no fault in any run. In the alphabetical stock FX list it is row last (`cw 96`).
  - `det_fx_select.txt` (synth FX 1): `fx1_plugin=6047/7292`.
  - `det_fx_controls.txt` (FX 1 picks `cw 96`, unchanged): `p0=36116i2:11197/0/0/0 p3=81329i1:19114/0/0/0 p2=71614i1:15019/0/0/0 p4=63068i1:16384/0/0/0 p1=52870i2:17749/0/0/0` (w5/w6/w7/latch; gain 11197 / 19114 / 15019 / 16384 / 17749: the 1365 / 273 steps), `master_saves 1`, `rnd 2 rnd_slot2 1`; `det_fx_reload.txt`: the same words after the power cycle, every latch 0. Cost: `fx1_plugin=5786/7305 s2_instr=4532/6041 s2_master=5735/6088`.
  - Costs are op1emu figures (cycles a block, avg/peak); the device is typically 10-35 % higher. Well under 30 k. Master FX 1 has no cost slot of its own (`fx1_plugin=` is the instrument FX 1; the master runs the same render) [I].

**What to do** (install `wavefolder.coop` with the librarian, power-cycle; play a synth sound through it):
1. Synth: T3, shift + T3, pick **wavefolder**, T3 (FX 1 on). Blue: more folds (brighter, buzzier) from about a third
   up; green: the folds turn lopsided either side of the centre (even harmonics), no thump while you turn it; white:
   darker to open; orange: dry to wet. At orange fully left the sound is exactly the dry sound.
2. Hold Help and turn blue: "GAIN" top left in blue; up gives more folds at the same fold setting, down fewer; the
   loudness hardly changes (the folder compresses); 0.5 dB a detent. Shift + Help + blue: 0.1 dB a detent. With
   orange fully left the gain changes nothing.
3. The same on the drum (T3 list), the master (mixer T3) and both FX 2 slots (drop): it is stereo everywhere; a
   panned or spread sound keeps its width.
4. Power-cycle: the gain is as you left it at each position.
5. Copy `/yaffs2/user/coop_stats.txt`: the `cost` line (`fx1_plugin=` for FX 1, `s2_instr=` / `s2_master=` for FX 2)
   with the plugin playing, and the `fxctl` line. Budget: under 30 k a block; estimate 9-15 k [I].

**If something is wrong:** power off; delete the plugin with the librarian, or recover by holding COM at power-on and
flash `op1_246_coop_e5.op1` (or stock).

## cs_hall: the hall reverb port (kennel cs_hall 0.1) — device test (pending, 2026-11-11)

- `build/plugins/cs_hall.coop` (17,712 bytes, sha256 `d42ced60e31706db53cd2ed55ca81cc7e29e9754e3c66ad74155001453dde0a1`),
  `plugins/cs_hall` (README: the contract, the cost plan). Needs the e6 package (effect controls). Install it with the
  librarian (or `tools/coop_sysex.html`) and power-cycle.
- Host: bit-identical to te-op1-cfw's hall through kennel's glue, on the host and in `bfin-elf-run` with the assembly
  (`make -C tests/host/cs_hall sim`): 18.2-22.0 k cycles a block avg, 26.2 k max (simulator, no cache stalls).

- **op1emu gate (2026-11-11, e6 image `09e6923d...` from dev after the merge, cs_hall alone on the NAND, one run at a time)** [C op1emu]:
  - `plugins seen 1 loaded 1 failed 0`, no fault in any run. In the alphabetical stock FX list it is row 0 ("cs hall" sorts first: `ccw 96`; `tests/sim/det_fx_cs_hall.txt` now does that).
  - `det_fx_select.txt` (synth FX 1): `fx1_plugin=20016/21780`. `det_fx_cs_hall.txt`: `p0=36860i2:1985/16384/32419/0 p1=36376i2:3605/16712/32767/0 p2=54017i1:2949/16384/32767/1 p3=64092i1:3933/16384/32767/0 p4=45831i1:3277/16384/32111/0`, every word as the scenario expects; `fx1_plugin=20004/22723 s2_instr=18757/20205 s2_master=18802/20252`.
  - `det_fx_controls.txt`: `p0=36860i2:21943/19936/1568/0` and `p3=64080i1:15529/12294/31826/0` (all three drawn by randomise), `p1=...:3605/16384/32767/0 p2=...:2949/16384/32767/1 p4=...:3277/16384/32767/0` (width clamps at the top); `det_fx_reload.txt`: the same words, every latch 0. Cost `fx1_plugin=20010/26893 s2_instr=18763/25644 s2_master=18800/20252` (the peaks: size crossfades and randomised jumps).
  - Costs are op1emu figures (cycles a block, avg/peak); the device is typically 10-35 % higher. Under 30 k on average; the highest peak of the nine (26.9 k). The ~30 ms dry after a restart, width at every position and the tank's L + R input are accepted (the owner, 2026-11-11): listen for the dry gap. Master FX 1 has no cost slot of its own (`fx1_plugin=` is the instrument FX 1; the master runs the same render) [I].

**What to do**:
1. Synth: T3, shift + T3, pick **cs hall**, T3 on. Play chords: a hall tail. Turn blue (size: no click while it
   moves), green (decay; at the top the tail never ends), white (damp: darker tails), orange (mix: dry to wet).
2. Hold Help and turn blue (PREDELAY), green (EARLY), white (WIDTH): name and value top left in the encoder's colour;
   shift + Help + the same encoder moves it in fine steps. Width at 0 is mono.
3. Click orange: FREEZE ON, the tail holds while you play (nothing new goes in); click again: it decays.
4. The same on drum FX 1, master FX 1 (mixer T3) and both FX 2 (drop): the effect is stereo at each.
5. Change sound (or re-pick cs hall): ~30 ms dry (accepted as it is by the owner, 2026-11-11: listen for it), then
   the reverb starts empty (no old tail, no click).
6. Power-cycle: predelay, early and width as you left them; freeze off.
7. Copy `/yaffs2/user/coop_stats.txt` with cs hall on two positions and size moving: the `cost` line
   (`fx1_plugin=`, `s2_instr=`, `s2_master=`) and `plugins` (`heap_largest`). Budget: under 30 k a block, over 60 k
   does not ship.

Listen for: crackle or dropouts with the size knob moving (the crossfade block is the peak), a frozen tail that grows
or dies, a click at the clear.

## cs_plate 0.1: coldsoldr's plate reverb, the first ported kennel effect with controls — device test (pending, 2026-11-11)

- `build/plugins/cs_plate.coop` (11,460 bytes, sha256 `5eb1368044fae3674632fcb964e075037c9fd642425d8a2fa3a052bc012d8770`),
  `plugins/cs_plate` (`README.md` has the contract). It needs the e6 package or later (effect controls); an older
  core refuses the file ("plugin flags not supported yet"). Install it from the co-op Settings tab (or
  `tools/coop_sysex.html`) and power-cycle.
- Host: bit-identical to te-op1-cfw's plate and its kennel half-rate path (`make -C tests/host/cs_plate`; the
  simulator run checks the assembly against upstream's C too). GNU simulator: 17,456 cycles a block steady, 19,828
  max. Load size ~87.5 KB a position (x 5 = ~438 KB of heap 0).

- **op1emu gate (2026-11-11, e6 image `09e6923d...` from dev after the merge, cs_plate alone on the NAND, one run at a time)** [C op1emu]:
  - `plugins seen 1 loaded 1 failed 0`, no fault in any run. In the alphabetical stock FX list it is row 0 (`ccw 96`; `tests/sim/det_fx_cs_plate.txt` now does that).
  - `det_fx_select.txt` (synth FX 1): `fx1_plugin=19176/20585`. `det_fx_cs_plate.txt` (its final `wait` lengthened to 80 s: the first run ended before the scenario's events): `fxctl fx 1 turns 6 fine 1 clicks 0 latches 2 ignored 1 ... p0=66818i1:3/23594/10178/0` as expected; `fx1_plugin=19187/21218`.
  - `det_fx_controls.txt`: `p0=...:1/22902/20068/0` and `p3=...:5/2114/13987/0` (drawn), `p1=39820i2:4/22938/9830/0`, `p2=57461i1:4/22938/9830/1`, `p4=49275i1:4/22938/10486/0` (size moves a step per 4 detents, so the 1- and 2-detent turns leave it; `master_saves 0`: nothing changed at the master); `det_fx_reload.txt`: the same words, every latch 0. Cost `fx1_plugin=19181/21187 s2_instr=17922/19934 s2_master=17966/18469`.
  - Costs are op1emu figures (cycles a block, avg/peak); the device is typically 10-35 % higher. Under 30 k. The ~15 ms dry after a restart and the plate's L + R input are accepted (the owner, 2026-11-11): listen for the dry gap. Master FX 1 has no cost slot of its own (`fx1_plugin=` is the instrument FX 1; the master runs the same render) [I].

**What to do**:
1. Synth: T3, shift + T3, pick **cs plate**, T3 (FX 1 on). Play chords and short notes: a bright plate tail. Turn
   blue (decay: short to very long), green (damp: bright to dark), white (pre-delay: a gap before the tail, up to
   200 ms), orange (mix: dry to fully wet). Listen for clicks, zipper noise while turning, crackle or dropouts.
2. Hold Help (fn) and turn blue: "SIZE 1".."8" top left, one step every 4 detents, stopping at 1 and 8 (no wrap).
   Play a long chord and change size under it: the tail dips for about 30 ms and carries on at the new size, with no
   click. Help + green: diffusion (smeared onsets at the top, grainy echoes at 0). Help + white: motion (chorus-like
   shimmer at the top, a static tail at 0). Shift + Help + green / white: the same values in fine steps. Shift +
   Help + blue: nothing (size has no fine).
3. Click orange: "FREEZE ON"; the tail holds forever and new notes are not added to it. Change size while frozen:
   the frozen tail carries on at the new size. Click again: it decays.
4. Freeze at the synth, switch to drum and back: freeze is off (the stock restarts the instrument effect; accepted).
5. The same at the drum position, the master (mixer T3) and both FX 2 slots (drop, shift + blue to cs plate, blue
   push, T3 on). Run it on two or three positions at once with a six-voice chord: no dropouts.
6. Randomise on the synth FX page (synth key + right): size, diffusion and motion change with the four turns, and
   freeze stays as it was.
7. Save the sound, power-cycle: size, diffusion and motion come back; freeze is off.
8. Copy `/yaffs2/user/coop_stats.txt`: the `cost` line (`fx1_plugin=` at a stock position, `s2_instr=` / `s2_master=`
   in FX 2), from a moment with a chord ringing and the encoders turning; the `plugins` line (`heap_largest`); the
   `fxctl` line.

**If something is wrong:** power off; recovery: hold COM at power-on and flash the e6 package (or stock), then
delete the plugin from the Settings tab.

Result: (pending)

## e7: 32 plugin files, a plugin memory budget, the boot hang fixed — device test (pending, 2026-11-11)

What changed (`docs/plugins.md` "e7"): up to 32 plugin files load (was 8), every file that does not load is named
with a reason in the stats and the inventory, and the plugins may take at most 1,536 KiB of heap 0, with a second
check that the stock tape cache (24 x 512 KiB, built from the same heap right after the plugin scan) still fits. The
e6 hang with large sets was the stock tape asserting `tape_c.cpp:62 data` when a cache node no longer fit in heap 0
[C op1emu].

- `build/pkg/op1_246_coop_e7.op1` sha256 `f334c83d2c937233bdcb3c3940ca9fcd96bf3b7c7fbb78efc81cf60af32a6a55`, copy in
  Downloads. Release image as e6 (`--tape-stereo --set-word g_coop_spread=32767`, no `COOP_M1`), build tag `e7`.
  `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr` differs.
- No plugin file changed: the nine ports, rings, saw and test_fx are e6's.

**Gate notes (op1emu, 2026-11-11, one run at a time; e7 images `--tape-stereo --set-word g_coop_spread=32767
--set-word g_coop_force_stereo_out=1`, the release core of the commit with this entry)** [C op1emu]:

| Run | Result |
|---|---|
| pristine `det_chord.txt`, e7 vs e6 | the known one-block onset shift (frame 1,395,072, "63 vs 64", max 5120) in two pairs, max 2 LSB with `--align 200`; an earlier e7 build (same code but the heap check placed after allocating) was **identical to e6 over 3,225,100 frames**. No regression. `plugmem ... used 0`, `heap_free 2729484` (first boot) |
| `det_fx_controls.txt` (test_fx) | `fxctl fx 1 turns 11 fine 1 clicks 3 latches 4 ignored 1 cancelled 1 popup_frames 406 from_params 0 master_saves 1 random 2 p0=36116i2:11197/1/16703/0 p1=52844i2:17749/1/8192/0 p2=71587i1:15019/0/8192/1 p3=81303i1:19114/0/26899/0 p4=63042i1:16384/0/8848/0`, `rnd 2 rnd_back 0 rnd_refused 0 rnd_slot2 1`: the gain words as e6 (11197, 17749, 15019, 19114), randomised depth / polarity (drawn values differ run to run, as in e6), p2's latch on. `fx1_plugin=3068/3570` |
| `det_fx_reload.txt` (same NAND) | `p0=84344i2:11197/1/16703/0 p1=2402i1:17749/1/8192/0 p2=86746i1:15019/0/8192/0 p3=86141i1:19114/0/26899/0 p4=86141i1:16384/0/8848/0`: words kept, every latch 0 |
| saw `det_eng_controls.txt` | `help_taps 1`, `ctl_turns 1 ctl_clicks 1 ctl_ignored 1 ctl_cancelled 1`, as e4 / e6; `plugmem ... used 2320` |
| **rings + tape_echo + cs_hall + cs_plate (2.34 MB, hung on e6), fresh NAND**, `det_fx_select.txt` | boots and plays (43.6 s of audio, no fault); see below |
| **11 files (the nine ports + rings + saw), fresh NAND**, `det_fx_select.txt` + the SysEx self-test | boots and plays; 9 load, 2 refused; see below; the chord through wavefolder (the FX list's last row) `fx1_plugin=6432/14588` |
| the same 11-file NAND, second boot | the same 9 / 2; `heap_free_boot 14605628` (fresh: 14279156), end `heap_free 1590924` (fresh: 1264972) |
| the 11 files with `g_coop_plug_budget=16777216` (budget off, the heap check alone), fresh NAND | boots and plays: `plugins seen 11 loaded 8 failed 3`, `plugmem budget 16777216 used 2048664 over_budget 0 heap_short 3 ... heap_free_boot 13683428`, `plugfile wavefolder.coop failed needs 50280: not enough memory left for the tape`, the same for `cs_hall.coop` (514880) and `freq_shift.coop` (71080); end `heap_largest 572900 heap_free 669244` |
| the 2.34 MB set with the budget off | all four load (`used 2337344`) and it **boots**: without e4's probe the set fits (end `heap_free 386372`); e7's budget still refuses two |
| the e6 image on the NAND that hung on e6 (its second boot) | **boots and plays** (287 s, `plugins seen 4 loaded 4`, `heap_largest 8388608`): the second boot has ~0.3 MB more heap 0, so this set hung on the first boot only; heavier sets would hang on every boot (same mechanism) [I] |

First version found by this gate: the heap check ran after the plugin's blocks were taken and freed a refused plugin;
with the budget off the 11-file set still hung (`heap_short 1`, freq_shift refused, then the 24th tape node failed:
a freed block never goes back above heap 0's break). The check now runs before allocating, counts the blocks as taken
off the break and keeps one node spare (the run above).

2.34 MB set (directory order rings, cs_plate, tape_echo, cs_hall):
```
plugins seen 4 loaded 2 failed 2 disabled 0 skipped 0 over_budget 2 ...
plugmem budget 1572864 used 1103984 over_budget 2 heap_short 0 skipped 0 badname 0 tape_node 524288 heap_free_boot 14636460
plugfile tape_echo.coop failed needs 718480: over the plugin memory budget
plugfile cs_hall.coop failed needs 514880: over the plugin memory budget
```
end of run: `heap_largest 1456596 heap_free 1622516 heap_top 1456596`.

11 files (loaded: saw, tape_drive, bitcrusher, ensemble, rings, cs_plate, drive, wavefolder, freq_shift):
```
plugins seen 11 loaded 9 failed 2 disabled 0 skipped 0 over_budget 2 ...
plugmem budget 1572864 used 1451544 over_budget 2 heap_short 0 skipped 0 badname 0 tape_node 524288 heap_free_boot 14279156
plugfile tape_echo.coop failed needs 718480: over the plugin memory budget
plugfile cs_hall.coop failed needs 514880: over the plugin memory budget
```
end of run: `heap_largest 1168628 heap_free 1264972 heap_top 1168628`. Inventory (SysEx self-test, `g_coop_sysex_selftest=1`):
```
coop-inventory 1	build e7	protocol 1
saw.coop	synth	saw	loaded	on	0.2	coldsoldr	6520
tape_drive.coop	fx	tape drive	loaded	on	0.4	coldsoldr	11072
bitcrusher.coop	fx	bitcrusher	loaded	on	0.1	coldsoldr	1680
ensemble.coop	fx	ensemble	loaded	on	0.3	coldsoldr	10828
rings.coop	synth	rings	loaded	on	0.5	coldsoldr	113048
cs_plate.coop	fx	cs plate	loaded	on	0.1	coldsoldr	11460
drive.coop	fx	drive	loaded	on	0.1	coldsoldr	9616
tape_echo.coop	fx	tape echo	failed	on	0.1	coldsoldr	10664	over the plugin memory budget (needs 702 KB, 118 KB of 1536 KB left)
wavefolder.coop	fx	wavefolder	loaded	on	0.1	coldsoldr	9988
cs_hall.coop	fx	cs hall	failed	on	0.1	coldsoldr	17712	over the plugin memory budget (needs 503 KB, 118 KB of 1536 KB left)
freq_shift.coop	fx	freq shift	loaded	on	0.1	coldsoldr	10308
```
("left" is what the budget has left after the whole scan.) Not exercised: more than 32 files, a name of 40+ characters
(`skipped` / `badname` paths) [I].

**What to do:**
1. Flash `op1_246_coop_e7.op1` (keep your plugins). Power-cycle once; it boots to the tape as before.
2. Install rings, tape_echo, cs_hall and cs_plate together (the set that hung on e6 in the emulator) and power-cycle.
   The OP-1 must boot and play. Copy `/yaffs2/user/coop_stats.txt`: the `plugins` line, the `plugmem` line and every
   `plugfile` line. Expected: two of the four refused with `over the plugin memory budget` (which two depends on the
   directory order on the device).
3. Open the librarian's plugin list: the refused files show as failed with the same reason.
4. Put all nine effect ports, rings and saw on the device (11 files), power-cycle: every file is either loaded or has
   a `plugfile` line; play an effect that loaded (wavefolder, freq_shift) and rings.
5. After a few minutes of playing (an FX 2 delay on the synth and the master too), copy the stats again: `heap_free`
   and `heap_largest` on the `plugins` line are the device's own heap 0 margin. Send both stats files.

**If something is wrong:** power off; recovery: hold COM at power-on and flash `op1_246_coop_e6.op1` (or stock), and
remove the big plugins (tape_echo, cs_hall) before booting e6.

Result: (pending)

## e8: the position buffers, every effect port relocated once — device test (pending, 2026-11-11)

What changed (`docs/plugins.md` "e8"): the core owns one working buffer per FX position (synth, drum, master, instrument
FX 2, master FX 2) and the effect active there works in it, as on kennel; an effect file is relocated once instead of
five times; an effect restarts whenever it is selected at a position. The nine effect ports are rebuilt for it (their
plugin.json declares `"scratch"`): all nine together take 758,248 bytes of heap 0 instead of ~2 MB, so tape_echo and
cs_hall load beside rings. **The new port files need the e8 image** (an older image refuses them: "unknown plugin
flags"); test_fx, stereo_delay, rings and saw are unchanged.

- `build/pkg/op1_246_coop_e8.op1` sha256 `d74be2c1d981891c830ca7aef1fca5c283cb0bc582966c0f0d2ff74dc686bcda`, copy in
  Downloads with the nine e8 port files (overwriting the e6-era copies) and rings.coop (saw.coop there is unchanged).
  Release image as e7 (`--tape-stereo --set-word g_coop_spread=32767`, no `COOP_M1`), build tag `e8`.
  `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr` differs.
- Host tests (this branch, 2026-11-11) [C host / GNU simulator]: all nine ports bit-identical to upstream on the host,
  with the position buffers filled with random bytes before init; `coop_fx_scratch_check` (the port at slots 0 and 3 at
  once, and restarted on a dirtied buffer) 0 differing blocks for each. Simulator builds: drive, ensemble, freq_shift,
  tape_drive, tape_echo, wavefolder, cs_hall, cs_plate bit-identical (assembly) with the scratch check passing; bitcrusher's
  profile 5,900 / 7,572 cycles a block. Simulator costs, cycles a block (avg / max): drive 7,662 / 8,126 moving, ensemble
  12,815 / 14,824, freq_shift 9,833 / 11,095 (L == R) and 14,450 / 15,950, tape_drive 9,194 / 9,419, tape_echo 15,987 /
  16,746, wavefolder 5,749 / 7,046 (stereo); about what they cost before e8 (one pointer load a block more).

**Gate notes (op1emu, 2026-11-11, one run at a time; e8 image `--tape-stereo --set-word g_coop_spread=32767 --set-word
g_coop_force_stereo_out=1`, release core of commit 48a3d15 on branch e8-shared-buffers)** [C op1emu]:

| Run | Result |
|---|---|
| **11 files (the nine e8 ports + rings + saw), fresh NAND**, `det_fx_select.txt` | boots and plays (46.0 s of audio, no fault): **all 11 load**, `heap_short 0 over_budget 0`; the chord through wavefolder (last FX row) `fx1_plugin=6339/18112`; end `heap_largest 1185388 heap_free 1287100` |
| the same NAND, second boot | boots (63.8 s, no fault), 11 loaded; `heap_free_boot 14627756` (fresh 14301284); end `heap_largest 1518940 heap_free 1613572` |
| pristine `det_chord.txt`, e8 vs e7 (`e7_st.ldr`) | the known one-block onset shift (31.635 vs 31.637 s), then max 2 LSB with `--align 200`, equal rms / width (154.7 / 118.6, 0.2339): no regression. `plugmem ... used 0 ... scratch_files 0 fxbuf 0`, end `heap_free 2729484` (e7: 2729484) |
| `det_fx_controls.txt` + `det_fx_reload.txt` (test_fx, the old model: 5 relocations, `used 720`) | `p0=36116i2:11197/0/28121/0 p1=59736i2:17749/1/8192/0 p2=78479i1:15019/0/8192/1 p3=88195i1:19114/1/21903/0 p4=69934i1:16384/0/8848/0`, `turns 11 fine 1 clicks 3 latches 4 ignored 1 cancelled 1 master_saves 1 random 2` (gains as e6 / e7; the drawn words differ run to run); after the power cycle `p0=87791i2:11197/0/28121/0 p1=2402i1:17749/1/8192/0 p2=90193i1:15019/0/8192/0 p3=89588i1:19114/1/21903/0 p4=89588i1:16384/0/8848/0`: words kept, latches 0 |
| tape_echo at synth FX 1 and instrument FX 2 at once (`det_fx_two_pos.txt`) | no fault; `p0=21645i1 p3=42028i1`; `fx1_plugin=16891/18310 s2_instr=15642/16202`; `plugmem ... used 683152 ... scratch_files 1 fxbuf 134784 fxbuf_mem 673960 fxbuf_short 0`, `heap_free_boot 15058684`, end `heap_free 2042916` |
| cs_hall the same (`det_fx_two_pos_first.txt`) | no fault; `p0=22390i1 p3=45473i1`; `fx1_plugin=19971/21573 s2_instr=18732/20191`; `used 450072 ... fxbuf 86752 fxbuf_mem 433800` |
| cs_plate the same | no fault; `p0=22390i1 p3=42043i1`; `fx1_plugin=19157/20648 s2_instr=17896/18974`; `used 398480 ... fxbuf 77664 fxbuf_mem 388360` |
| switching (`det_fx_switch.txt`, cs_hall + tape_echo) | no fault; synth FX 1 cs hall -> tape echo -> cs hall: `p0=54141i3` (one init a selection); instrument FX 2 first -> second -> first: `p3=29968i3`; `plugmem ... used 699424 ... scratch_files 2 fxbuf 134784` (the larger declaration) |

Not shown in op1emu [I]: the master FX 2 pick in `det_fx_two_pos.txt` landed on a stock type (s2_master ran stock), so
a third simultaneous position was not exercised; the restart's audible result (no old tail after a switch) is a
listening test for the device.

**What to do** (once the package exists):
1. Flash `op1_246_coop_e8.op1`. Copy the nine e8 port files (bitcrusher, drive, ensemble, freq_shift, tape_drive,
   tape_echo, wavefolder, cs_hall, cs_plate) and rings and saw to the plugins folder, all eleven together. Power-cycle.
   It must boot to the tape. Copy `/yaffs2/user/coop_stats.txt`: the `plugins` line (loaded 11, failed 0) and the
   `plugmem` line. Expected: `used` about 1.43 MB of `budget 1572864`, `scratch_files 9 fxbuf 134784 fxbuf_mem 673960
   fxbuf_short 0`, no `plugfile` line.
2. Play each port at **synth FX 1, drum FX 1, master FX 1 and FX 2** (instrument and master) once: it plays at each.
3. **Two positions at once**: tape_echo at synth FX 1 and at instrument FX 2 together, a chord: two echoes, each with
   its own knobs, no crackle, no fault. The same with cs_hall and with cs_plate.
4. **Restart on select**: play a long cs_hall tail at synth FX 1, switch FX 1 to tape_echo, then back to cs_hall while
   the old tail would still ring: cs_hall starts clean (after ~30 ms dry), no burst of the old tail, no noise. The same
   at FX 2. A first selection of cs_hall is now also ~30 ms dry (it was immediate before e8).
5. Leave it playing a few minutes and copy the stats again: `heap_free` / `heap_largest` on the `plugins` line (the
   device's own margin), the `cost` line's `fx1_plugin`, `s2_instr`, `s2_master`.
6. Open the librarian's plugin list: all eleven loaded.

**If something is wrong:** power off; recovery: hold COM at power-on and flash `op1_246_coop_e7.op1` (or stock). The e8
port files do not load on e7 (refused, named in its stats): put the e7-era port files back or remove them.

Result: (pending)

## e9: effect screens, the nine kennel effect ports with their screens — device test (pending, 2026-11-11)

What changed (`docs/plugins.md` "e9"): an effect plugin can ship its own screen, shown on the effect page instead of
the stock filter page (FX 1) or coop's FX 2 page, at every position; the core's name / value popup draws over it. The
nine kennel effect ports (bitcrusher 0.2, drive 0.2, ensemble 0.4, freq_shift 0.2, tape_drive 0.5, tape_echo 0.2,
wavefolder 0.2, cs_hall 0.2, cs_plate 0.2) have their kennel art and animation back. Their audio is unchanged
(bit-identical host tests). The SVGs go to the TE heap, not the plugin budget; heap 0 pays a 248-byte screen object a
file plus the screen code (1,292-3,560 bytes a port). The e9 port files load on an e8 image too (the screen is ignored
there); an e8 port file on e9 plays with the stock page [I: by the ABI, not run].

- `build/pkg/op1_246_coop_e9.op1` sha256 `32be0d0401d167e1f676beb308d8d54ee9fc5da23727dfa7bad8ecf99c82ec72`, copy in
  Downloads with the nine e9 port files and rings.coop and saw.coop (overwriting the older copies). Release image as e8 (`--tape-stereo --set-word
  g_coop_spread=32767`, no `COOP_M1`), build tag `e9`. `verify_bootloader.py` PASS, `pkg_check` = only `OP1_vdk.ldr`
  differs.

**Gate notes (op1emu, 2026-11-11, one run at a time; e9 image `--tape-stereo --set-word g_coop_spread=32767 --set-word
g_coop_force_stereo_out=1`, the release core of commit d4f26c7 on dev)** [C op1emu]:

| Run | Result |
|---|---|
| bitcrusher alone, `det_fx_screen.txt` (twice) | no fault; `fscr_built 1 fscr_swaps 6 fscr_updates 1589 fscr_te 15744 te_used 4724000 te_max 4966440`; synth FX 1 and instrument FX 2 both ran it (`p0=...i2 p3=...i1`) |
| the same NAND, power cycle, `det_fx_screen_reload.txt` | no fault; `fscr_built 1` after the boot scan, `fscr_swaps 1 fscr_updates 1891`; the screen on the effect page, at FX 2 and back |
| **11 files (the nine e9 ports + rings + saw), fresh NAND**, `det_fx_screen_switch.txt` | no fault; `plugins seen 11 loaded 11 failed 0 ... over_budget 0`; `fscr_built 9 fscr_failed 0 fscr_skipped 0 fscr_swaps 25 fscr_updates 3890 fscr_te 118896 te_used 4667560 te_max 5107504`; `plugmem budget 1572864 used 1450224 ... scratch_files 9 fxbuf 134784 fxbuf_mem 673960 fxbuf_short 0`, `heap_free_boot 14280420`, end `heap_largest 1166164 heap_free 1263988`. Screens in order: bitcrusher, cs hall, the stock cwo page, wavefolder, cs plate |
| the same set, fresh NAND, `det_fx_screen_others.txt` | no fault; 11 loaded, `fscr_built 9 fscr_swaps 58 fscr_updates 5577`, `te_used 4739232`; screens in order: drive (the stock "FX OFF" overlay over it until FX 1 was switched on), ensemble, freq shift, tape drive, tape echo, each with its fn layer while fn was held |
| pristine `det_chord.txt`, e9 vs e8 (`e8.ldr`) | one run of each identical over 2,159,572 frames (onset 1,395,120, rms 154.7 / 118.7, width 0.2339 as e8's gate): no regression. Other runs of either image start the chord 2-10 frames later and differ by up to ~3,400 LSB after `--align 200`, equal levels: run-to-run, the same for both images |

What the screens looked like in op1emu (each the kennel screen redrawn in coop's colours):
- bitcrusher: a sine and its stepped (crushed) copy, a clock-tick row above, the input meter right.
- drive: a schematic, input resistor and diode pair into a tube, the output line in green.
- ensemble: two delay lines with their moving taps, an LFO dial and a level bar right.
- freq shift: a half-circle dial, the shift needle and a green arc, an arrow and mode circle top right.
- tape drive: two reels with green play triangles, the tape path with a wave under them, a saturation curve right.
- tape echo: a tape loop with four heads (the active one white), the feedback path with green arrows, a sync glyph top right.
- wavefolder: the folded wave between two limit lines, a transfer-curve icon right.
- cs hall: a hall outline and the decay envelope below it; cs plate: a plate with rings and the decay envelope.
- With fn held: a blue bracket around the gain meter (left), the plain turns' parts dimmed.

Not shown in op1emu [I]: drum and master positions and FX 2 with the ports other than bitcrusher; the look on the
device's real display (colours, flicker); the animation rate under load; tape_echo's beat ticks (the bottom row showed
a plain line in op1emu at the default sync setting).

**What to do** (once the package exists):
1. Flash `op1_246_coop_e9.op1`. Copy the nine e9 port files, rings and saw to the plugins folder, all eleven
   together. Power-cycle. It must boot to the tape. Copy `/yaffs2/user/coop_stats.txt`: the `plugins` line (`loaded
   11 failed 0`, `fscr_built 9 fscr_failed 0 fscr_skipped 0`, `fscr_te`, `te_used`, `te_max`) and the `plugmem` line
   (op1emu: `used 1450224` of `budget 1572864`).
2. Pick each effect at **synth FX 1** and look at its screen (list above): it is the plugin's screen, not the stock
   filter page; turn each encoder, the part it moves follows and the popup names the value.
3. The same at **FX 2** (drop), at **drum FX 1** and **master FX 1 / FX 2**: the screen of the effect at that position,
   switching right when you move between positions (lift / drop, synth / drum / mixer).
4. Hold **fn**: the fn layer (the gain bracket, dimmed plain parts); release: back to normal.
5. **Switch** plugins at one position (e.g. bitcrusher -> cs hall -> cwo -> wavefolder), and two plugins at two
   positions (e.g. tape_echo at synth FX 1, cs_plate at FX 2): each redraw is the right screen, no flicker, no
   leftover of the previous screen, stock types still show their own pages.
6. Let it play a few minutes and copy the stats again: the `plugins` line (`heap_free`, `heap_largest`, `te_used`,
   `te_max`, `fscr_updates`) and the `cost` line (`fx1_plugin`, `s2_instr`, `s2_master`).
7. Your 15-file set: with e9's ~22 KB more one file may be refused (named in the stats and the inventory); note which.

**If something is wrong:** power off; recovery: hold COM at power-on and flash `op1_246_coop_e8.op1` (or stock). The
e9 port files load on e8 with the stock pages.

Result: (pending)

## e10: the boot picture, co-op's logo and the build's version — device test (pending, 2026-11-11)

What changed (`docs/screens.md` "e10"): the picture at power-on is the app's (a raw RGB565 image the app LDR loads at
0x02000000, shown until the GUI's first frame, ~2 s after the app starts), with the version printed by the stock
console from the string at 0x018F7358. `tools/boot_logo.py` (in `tools/build_image.sh`) replaces the picture with
coop's (four dots blue / green / white / orange in coop's brights, the "co-op" wordmark) and the line with
`coop e10 - 246`, generated from `COOP_BUILD_TAG` at build time. Data only: no code, no hook, no CALLBACK block; the
stock picture's sha256 and the stock string are checked first. te-boot and its own screen are untouched. Build tag
`e10`; the core is e9's otherwise.

- `build/pkg/op1_246_coop_e10.op1` sha256 `f184a2f46b34af90bc045cb818ca57a31c46004d47ad91a95b1e29627fd41fee`, copy in
  Downloads. Release image as e9 (`--tape-stereo --set-word g_coop_spread=32767`). `verify_bootloader.py` PASS (no
  te-boot.ldr), `pkg_check` = only `OP1_vdk.ldr` differs. Its L1 decrypts back to the op1emu-gated `e10.ldr` byte for
  byte. Loaded memory against the e9 image: the picture (7,977 bytes in 0x0200622C..0x0201F729), the console string
  (9 bytes in 0x018F7384..0x018F738E), and the core's build tag ("e10": the core's data and its two near/region-C
  references move, 8,235 + 15 bytes).

**Gate notes (op1emu, 2026-11-11, one run at a time, fresh NAND each)** [C op1emu]:

| Run | Result |
|---|---|
| e10, boot, screenshots every 1.5 s | the first frame is coop's picture with `coop e10 - 246` under it (equal to the tool's preview within RGB565 rounding, max 7 / 255); the tape screen from the 7th shot (~9 s wall) on, identical to stock's and e9's; no fault |
| stock and e9 (`e9base.ldr`), the same | TE logo + `R. 00246` until the tape screen; e9's picture identical to stock's (the reference) |
| pristine `det_chord.txt`, e9 vs e10, twice each, alternating | no fault; ch0 rms equal pairwise (58.1 / 58.2, 58.8 / 58.8); e10 vs e9 max 3,073 unaligned / 14 aligned (`--align 200`), within the run-to-run spread of one image (e9 vs e9: 3,073 / 1; e10 vs e10: 14 / 14). No regression |

**What to do:**
1. Flash `op1_246_coop_e10.op1` through the te-boot menu (COM at power-on, "1. Upload firmware").
2. Power on normally and look at the boot picture (about 2 s before the tape screen): on black, four dots left to
   right blue, green, white, orange/red, then "co-op" in white, centred; a line `coop e10 - 246` below in the small
   white console font, centred under it. Note: whether the whole picture is on the panel (the left / right edges,
   the dots' and wordmark's colours and edges), whether it is centred, how long it shows, any flicker or a TE logo
   flash before it. If the picture is shifted (other panel variants place it 12 or 40 px further left [I]), say by how much.
3. It must then boot to the tape screen as e9 does; play a chord.
4. Power-cycle once more: the same picture. (The first boot after a tape rebuild may show console text over or instead
   of it, as stock does, e.g. "(re-)creating fat AIFC file": that is the stock log, not the picture.)
5. Hold COM at power-on: te-boot's own screen with TE's logo and the menu must appear as before (recovery path).

**If something is wrong:** power off; hold COM at power-on and flash `op1_246_coop_e9.op1` (or stock). A wrong picture
cannot stop the boot (it is only data the display shows), and te-boot is reached before any app code runs.

Result: (pending)
