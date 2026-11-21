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
`e10`; the core is e9's otherwise, plus the tempo export (below).

- **The e10 package to flash (boot picture + the tempo export, 2026-11-11, branch tempo-export):**
  `build/pkg/op1_246_coop_e10.op1` sha256 `23367c931e4ec33dfabbe00c44f2035bd3f505a53ed2f4ab2b7b359e722e23f4`, copy in
  Downloads (overwrote the boot-picture-only build), with `tape_echo.coop` 0.3 (sha256
  `be55ea5b50b8a610cdbe2e0306798934f118c977239860323c0d2038575a86b8`). `tools/build_device_package.sh ... --tape-stereo
  --set-word g_coop_spread=32767`: `verify_bootloader.py` PASS (no te-boot.ldr), `pkg_check` = only `OP1_vdk.ldr`
  differs. Its core adds `coop_tempo_bpm_q16` (export 15) and the stats `fxctl` fields `tempo_q16` / `tempo_inc`; the
  op1emu gate of that core is in "tape_echo 0.3" below (pristine `det_chord` identical to `e10.ldr` within the run
  spread).
- Superseded (boot picture only): `build/pkg/op1_246_coop_e10.op1` sha256
  `f184a2f46b34af90bc045cb818ca57a31c46004d47ad91a95b1e29627fd41fee`, copy in
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

## tape_echo 0.3: sync follows the project tempo (the e10 tempo export) — device test (pending, 2026-11-11)

What changed (`docs/request-tempo-export-tape-echo.md`, `docs/fx-abi.md` "Tempo"): the core exports the tempo screen's
BPM (`coop_tempo_bpm_q16`, read from the stock's float BPM word); tape_echo's sync (fn + green) follows it instead of
a fixed 120 BPM, live while it plays, and its screen's beat ticks with it. Needs the e10 image: tape_echo 0.3 on an
older core is refused at boot ("an import is not exported by the core", in the stats and the inventory); 0.2 still
loads there (sync at 120). The stats `fxctl` line now ends its counters with `tempo_q16` (the export: BPM x 65536) and
`tempo_inc` (the stock LFO increment, bpm x 1662149.125).

- `build/plugins/tape_echo.coop` 0.3 (23,892 bytes, sha256
  `be55ea5b50b8a610cdbe2e0306798934f118c977239860323c0d2038575a86b8`), on the e10 image
  (`op1_246_coop_e10.op1` sha256 `23367c93...`, the e10 entry above).
- [C host, GNU simulator] bit-identical with the kennel reference at 120, 90, 133.5, 174 BPM and a seven-tempo walk
  (`make -C tests/host/tape_echo`; `sim` at 133.5 and 120), `plugins/tape_echo/README.md` "Tests".

**op1emu gate (2026-11-11, one run at a time, the e10 core of this branch; images `--tape-stereo --set-word
g_coop_spread=32767`, plus `g_coop_force_stereo_out=1` for the tempo runs)** [C op1emu]:

| Run | Result |
|---|---|
| tape_echo alone, `tests/sim/det_fx_tape_echo_tempo.txt` (tempo screen `turn enc1 ccw 60`, sync 1/4, notes; then `turn enc1 cw 100`, notes) | no fault; `plugins seen 1 loaded 1 failed 0`; `fxctl ... tempo_q16 9502720 tempo_inc 241011616 p0=77100i1:16384/8/0/0` (145.0 BPM at the end; the increment is the stock's own 145 x 1662149.125 in single precision; sync word 8 = 1/4); `cost fx1_plugin=17197/19074` |
| the same scenario up to the tempo change (a scratch copy ending after section A) | no fault; `fxctl ... tempo_q16 6225920 tempo_inc 157904160` (95.0 BPM) |
| echo period (envelope autocorrelation of the WAV over each note's repeats) | at 95 BPM: 0.6317 s and 0.6325 s (60/95 = 0.6316 s); at 145 BPM, after the tempo was changed with the effect running: 0.4137 s and 0.4134 s (60/145 = 0.4138 s). Within 0.15 %: the repeats follow the tempo, and the change took effect live |
| pristine `det_chord.txt`, e10 (`e10.ldr`, the boot picture alone) vs this core, twice each | no fault; this core's run a identical to both e10 runs (3,502,766 frames from 5 s); its run b differs by the usual onset shift (max 3,073 unaligned, 1 aligned with `--align 200`, rms 58.4 both), the same spread as e10 vs e10 in the boot-picture gate and e9 vs e9 (3,073 / 1). No regression |

The tempo screen in op1emu moved the BPM by 0.4 a step down (60 steps: 120 -> 95) and 0.5 a step up (100 steps: 95 ->
145) [I: encoder acceleration]. Not seen in op1emu [I]: the beat ticks on the screen, the drum and master positions,
a tempo change in the middle of a repeat (the time glides like any time change, by the DSP), external MIDI clock, the
boot value (120.0f in the image bytes; every run here set the tempo first).

**What to do** (flash the e10 image, install `tape_echo.coop` 0.3 with the librarian, power-cycle):
1. Copy `/yaffs2/user/coop_stats.txt`: the `plugins` line says tape_echo loaded, failed 0.
2. Press the tempo key (metronome) and set the tempo to **90** BPM. Synth: T3, shift + T3, pick **tape echo**, T3 so
   FX 1 is on. Hold Help and turn green to **SYNC 1/4**.
3. Start the sequencer or the metronome at 90 and play short notes on the beat: each repeat lands on the next beat
   (a quarter at 90 = 667 ms), steady against the click. Try 1/8 (333 ms) and 1/2 (1.33 s) too.
4. Set the tempo to **140** while the echo is still repeating: the repeats move to the new beat (the time glides
   there, a short pitch bend, no click). A quarter at 140 = 429 ms. Turn the tempo slowly up and down: the echo
   follows.
5. Hold Help: the beat ticks over the tape are closer together at 140 than at 90 (they follow the tempo).
6. Sync `OFF`: the blue turn sets the time again, the tempo no longer moves it.
7. Power-cycle with the project at 140: after the boot the echo is at 140 with no change (the tempo is read from boot
   on). A tempo between 40 and 220 (the stock's range) all work; 1/2 falls back to the blue turn below 80 BPM (over
   1.5 s).
8. Copy `coop_stats.txt` again: `tempo_q16` = BPM x 65536 (140: 9175040; 90: 5898240), `tempo_inc` = BPM x 1662149
   (140: about 232700880).
9. [I] With MIDI clock sync (if you use it): does the echo follow the external tempo? Note what happens.

**If something is wrong:** power off; remove `tape_echo.coop` with the librarian (or put 0.2 back), or recovery: hold
COM at power-on and flash the previous coop image (or stock).

Result: (pending)

## fickle 0.2 / petri 0.2: both in tape_echo's buffer size, the 13-file set fits e10 — device test (pending, 2026-11-20)

What changed (`plugins/fickle/README.md`, `plugins/petri/README.md` "For the owner" / "Open points"): with all 13
catalogue plugins on e10, fickle 0.1 was refused at boot (`over the plugin memory budget`): fickle and petri declared
position buffers larger than tape_echo's and so grew all five. 0.2 of each declares no more than tape_echo's 134,784
bytes. **fickle**: the loop buffer is 15,288 clock samples (was 24,576): the longest loop 0.62 s at 24 kHz (was 1.00 s),
7.4 s at 2 kHz (12.0 s). **petri**: the record is 1.36 s (was 1.49 s), pattern's longest bar 1.27 s (1.39 s); the
space's sound is unchanged. They need core e10, as 0.1 did; the kennel builds are not touched.

- `plugins/fickle/fickle.coop` 0.2 (35,596 bytes, sha256
  `269cb27c08011cae00462824e7ee6a45f808915d9194d515c3aafa25441ad22c`), `plugins/petri/petri.coop` 0.2 (36,104 bytes,
  sha256 `33d7bdf063d31dc98c31b5d2d5c13256fe1621d8eae4e1698b8909ffe8c980c6`); copies in Downloads.
- [C host, GNU simulator] `make -C tests/host/fickle` (stress 60,000 blocks, holds, the e8 check: passed; `sim` within
  1 % of 0.1's table). `make -C tests/host/petri`: passed; built with the old record length through the new mapping,
  the 31 listening takes are byte-identical to 0.1's; `sim`: both kernels 0 differ, all effects 21,111 avg / 36,152
  max (0.1: 20,936 / 33,416: the space's copy comes four times as often).

**Gate notes (op1emu, 2026-11-20, one run at a time, fresh NAND each; the e10 image is HEAD's core before e11, its
device LDR byte-identical to the one in `op1_246_coop_e10.op1`; gate images `--tape-stereo --set-word
g_coop_spread=32767 --set-word g_coop_force_stereo_out=1`)** [C op1emu]:

| Run | Result |
|---|---|
| **the 13 catalogue files on e10** (the nine ports, rings, saw, fickle 0.2, petri 0.2), `det_fx_select.txt` | boots and plays, no fault: `plugins seen 13 loaded 13 failed 0 ... over_budget 0`, `fscr_built 11`; `plugmem budget 1572864 used 1502128 over_budget 0 heap_short 0 ... heap_free_boot 14222524 scratch_files 11 fxbuf 134784 fxbuf_mem 673960 fxbuf_short 0`; the chord through wavefolder `fx1_plugin=11295/24189`; end `heap_largest 1111444 heap_free 1205596` |
| the same NAND, second boot | 13 loaded, the same `plugmem` (`heap_free_boot 14552700`); end `heap_largest 1444172 heap_free 1535772` |
| **fickle alone on e10**, `det_fx_fickle.txt` (every position, a chord, loop / dub / freeze, the fn layers) | no fault, 125.9 s of audio; `plugmem ... used 698344 ... scratch_files 1 fxbuf 134784`; `fxctl fx 1 turns 6 ... latches 10 ... p0=41009i2:1/2/4/0 p1=18275i2:0/1/0/0 p2=35612i2:0/1/0/0 p3=46528i1:0/1/0/0 p4=29301i1:0/1/0/0` (all five positions rendered it; wet mode delay, routing loop, loop mode rev 1 at p0); `fx1_plugin=18102/32204 s2_instr=20630/31083 s2_master=20624/22237`; `fscr_built 1` |
| **petri alone on e10**, `det_fx_petri.txt` (every position, a chord, reverse / hold, the fn layers) | no fault, 113.5 s; `plugmem ... used 701112 ... scratch_files 1 fxbuf 134752 fxbuf_mem 673800`; `fxctl fx 1 turns 18 ... latches 8 ... p0=40444i2:10/28688/9312/0 p1=8052i2:0/30000/8000/0 p2=25390i2:0/30000/8000/0 p3=36526i1:0/30000/8000/0 p4=19298i1:0/30000/8000/0` (effect warp, filter and space moved at p0); `fx1_plugin=25913/35414 s2_instr=18815/24417 s2_master=18869/24440`; `fscr_built 1` |
| the same two on e11 | as on e10: fickle `p0=41010i2:1/2/4/0`, `fx1_plugin=17870/32298`; petri `p0=40445i2:10/28688/9312/0`, `fx1_plugin=25915/35414`; no fault |

Not shown in op1emu [I]: how the shorter loop and record sound (a listening test), the screens' look, the device's
`cost` figures (expect 10-35 % over these; petri's warp and fickle's loop + dub at a high clock are the heavy cases).

**What to do** (the OP-1 on e10 as it is today):
1. Install `fickle.coop` 0.2 and `petri.coop` 0.2 with the librarian over the 0.1 files, with the other eleven
   catalogue plugins present. Power-cycle. It must boot to the tape.
2. The librarian's plugin list: all 13 loaded, none failed. Or read `coop_stats.txt` over USB MIDI (`python firmware	ools\coop_usb.py stats -o stats.txt`, the OP-1 plugged in normally, **not** disk mode, the librarian's browser tab closed; the agent can run it: yaffs files are not visible in disk mode): `plugins seen 13
   loaded 13 failed 0`, `plugmem budget 1572864 used` about `1502128`, `fxbuf 134784`, no `plugfile` line.
3. **fickle** at synth FX 1: play, white click (loop): the last 0.6 s comes back as a loop; green click (dub) and
   play over it; white click again (off). Turn the clock (white) down: the loop gets longer and darker (1 s at about
   15 kHz). Orange click: freeze. fn + blue / green / white: the wet mode, the routing, the loop mode.
4. **petri** at synth FX 1: each of the eleven effects (fn + blue) with a few notes; pattern with time at the top (the
   bar is now 1.27 s); orange click (hold), blue click (reverse); fn + white filter, fn + orange space. Listen for
   clicks or crackle when a long loop wraps: there should be none.
5. Both at FX 2 and on the master once. Then read the stats again (`coop_usb.py stats`) after a few minutes: the `plugins` line's
   `heap_free` / `heap_largest` and the `cost` line (`fx1_plugin`, `s2_instr`, `s2_master`).

**If something is wrong:** remove the file with the librarian (or put 0.1 back: then one of the 13 is refused at boot,
as before). Nothing here touches the firmware.

Result: (pending: the listening steps 3-5.) Steps 1-2 on **e11** instead of e10 (2026-11-20, the e11 entry below)
[C device]: the 13 files with fickle 0.2 and petri 0.2 installed, all 13 loaded, `plugmem ... used 1502128 ... fxbuf
134784`, no `plugfile` line: the figure e10 would charge too (1,502,128 of its 1,572,864).

## e11: the plugin memory budget 1,728 KiB — device test (partly confirmed 2026-11-20: flashed, boots, 13 loaded; steps 4-5 pending)

What changed (`docs/plugins.md` "e11"): all plugins together may take 1,728 KiB of heap 0 (1,769,472 bytes), was
1,536 KiB. Nothing else in the core; build tag `e11`, the boot picture's line `coop e11 - 246`. Why that figure, in
short: a first boot without plugins ends with 2,729,484 bytes of heap 0 free; after the boot coop takes at most
742,024 more (the two FX 2 slots' delay / cwo lines, spring tanks and the master's grid tank, each of which may fail
harmlessly) and the stock took nothing more in the gate's runs, so a full budget leaves ~0.2 MB on a first boot and
~0.5 MB on any later one for what the stock was not seen to take.

- `build/pkg/op1_246_coop_e11.op1` sha256 `34c78ca1e4297d46f25b5e9f86da49273660cf0540933e296e3e843bcbc37739`, copy in
  Downloads. Release image as e10 (`tools/build_device_package.sh ... --tape-stereo --set-word g_coop_spread=32767`):
  `verify_bootloader.py` PASS (no te-boot.ldr), `pkg_check` = only `OP1_vdk.ldr` differs. The ELF holds the tag `e11`
  (and no `e10`), the package's LDR the line `coop e11 - 246`. Against the e10 core: six immediates (the budget's five
  loads and the inventory text's 1536 -> 1728) and the tag, by disassembly.
- The site does not offer e11 yet (no web delta made): it follows the owner's device result.

**Gate notes (op1emu, 2026-11-20, one run at a time, fresh NAND each; images `--tape-stereo --set-word
g_coop_spread=32767 --set-word g_coop_force_stereo_out=1`; e10 = the core before this change)** [C op1emu]:

| Run | Result |
|---|---|
| (a) **the 13 catalogue files on e11**, `det_fx_select.txt` | boots and plays, no fault: `plugins seen 13 loaded 13 failed 0 ... over_budget 0`, `fscr_built 11`; `plugmem budget 1769472 used 1502128 over_budget 0 heap_short 0 skipped 0 badname 0 tape_node 524288 heap_free_boot 14222524 scratch_files 11 fxbuf 134784 fxbuf_mem 673960 fxbuf_short 0`; `fx1_plugin=11296/24189`; end `heap_largest 1111444 heap_free 1205596` |
| (a) the same NAND, second boot | 13 loaded, the same `plugmem` (`heap_free_boot 14552700`); end `heap_largest 1444172 heap_free 1535772` |
| (b) the 13 files on e10 | the entry above: `used 1502128` of 1,572,864, the heap figures equal to e11's to the byte |
| (c) fickle alone, petri alone on e11 (`det_fx_fickle.txt`, `det_fx_petri.txt`) | the entry above: every position, latches and fn layers, no fault |
| (d) pristine `det_chord.txt`, e11 vs e10 | **identical over 2,263,660 frames** (from 5 s; rms 151.7 / 116.7, width 0.2327), first run of each; `plugmem budget 1769472 used 0`, end `heap_free 2729484` on both |
| late use, pristine, `det_heap_late.txt` (every FX 2 type at both slots; sounds, kits, a tape recording, lift and drops) | no fault, 234 s; `s2_changes 42 tanks 118 tank_short 0`; end `heap_largest 1987028 heap_free 1987460`: 742,024 bytes below (d), all of it coop's own late allocations; the same figure on e10 with a shorter version of the run |
| (e) **the budget full**: the 13 files + two test engines (204,804 and 57,348 bytes of `.bss`), fresh NAND, `det_heap_late.txt` | **boots** (the tape's cache got its 24 nodes: no halt) and plays 210 s, no fault: `plugins seen 15 loaded 15 failed 0`, `plugmem budget 1769472 used 1764688 over_budget 0 heap_short 0 ... heap_free_boot 13959964`; every late allocation given (`tanks 118 tank_short 0`); end **`heap_largest 167220 heap_free 201012`** |
| (e) the same NAND, second boot, `det_chord.txt` | 15 loaded; `heap_free_boot 14290140`; end `heap_largest 1181468 heap_free 1272180` |
| over the budget: the same and a third test engine (16,388 bytes), SysEx self-test image | boots, no fault: `plugins seen 16 loaded 15 failed 1 ... over_budget 1`, `plugmem budget 1769472 used 1756848`, `plugfile fickle.coop failed needs 24384: over the plugin memory budget`; inventory: `fickle.coop fx fickle failed on 0.2 coldsoldr 35596 over the plugin memory budget (needs 24 KB, 12 KB of 1728 KB left)` |

The stock's USB disk heap (96 KiB, e7 kept room for it) is taken during the boot: its base word `0x018C1EA8` held
`0x00104030` at the end of a pristine `det_chord.txt` run [C op1emu]. Not shown in op1emu [I]: the device's own heap
figures (they decide whether the margin is what the emulator says); the sampler's recording, the album, the radio and
USB disk mode with a full budget; whether the tape lift really landed in the synth and drum samplers in
`det_heap_late.txt` (no screenshot was taken).

**What to do:**
1. Flash `op1_246_coop_e11.op1` (keep your plugins). The boot picture's line reads `coop e11 - 246`. It boots to
   the tape as e10.
2. With the 13 catalogue plugins (fickle 0.2 and petri 0.2 among them): read `coop_stats.txt` over USB MIDI (`python firmware	ools\coop_usb.py stats -o stats.txt`, the OP-1 plugged in normally, **not** disk mode, the librarian's browser tab closed; the agent can run it: yaffs files are not visible in disk mode). Expected:
   `coop_stats 3 build e11`, `plugins seen 13 loaded 13 failed 0`, `plugmem budget 1769472 used` about `1502128`, no
   `plugfile` line. Note the `plugins` line's `heap_free` and `heap_largest`: the emulator said 1,535,772 / 1,444,172
   on a normal boot. **These two numbers from your device are the main result.**
3. The librarian's plugin list: all loaded; its memory bar may still show 1,536 KB until the site knows e11.
4. Put a delay or cwo at instrument FX 2 and at master FX 2, a spring at each, a grid on the master FX 2, play a few
   minutes; sample something into the synth and the drum, record on the tape, open USB disk mode (Shift + COM) and
   leave it. Back out of disk mode, read the stats again (`coop_usb.py stats`): `heap_free` / `heap_largest` once more
   and `tank_short` (expected 0).
5. If you have more plugins than the 13, add them until the bar is nearly full, power-cycle and repeat 2 and 4: a file
   past the budget is named in the stats and the list (`... KB of 1728 KB left`), the OP-1 must always boot.

**If something is wrong:** power off; recovery: hold COM at power-on and flash `op1_246_coop_e10.op1` (or stock). The
13 files fit e10 too (the entry above); a set over 1,536 KiB has a file refused there, named in the stats.

Result (2026-11-20, the owner's OP-1; read by the agent over USB MIDI, nothing copied in disk mode) [C device]:
- **Steps 1-3: confirmed.** `op1_246_coop_e11.op1` flashed through the te-boot menu; it boots. `coop_usb.py` over the
  winmm transport (its first use on the real device, and it works: `docs/test-loop.md`): HELLO `coop` build `e11`
  caps 63 (`0x3f`); INVENTORY: 13 rows, all loaded, fickle 0.2 and petri 0.2 among them; FILE_GET of
  `/yaffs2/user/coop_stats.txt`, 11,197 bytes: `plugins seen 13 loaded 13 failed 0 ... over_budget 0`, `plugmem budget
  1769472 used 1502128 over_budget 0 heap_short 0 ... heap_free_boot 14534692 scratch_files 11 fxbuf 134784 fxbuf_mem
  673960 fxbuf_short 0`. `used`, `fxbuf` and `fxbuf_mem` are op1emu's figures to the byte (gate row (a)); the
  device's `heap_free_boot` lies between the emulator's first-boot and second-boot values (14,222,524 / 14,552,700).
- **The librarian's windowed install** (web commit `ca8cfe9`: three PUT_DATA in flight, `docs/sysex.md`) on this
  image: `rings.coop` installed in under 20 s (about 76 s with one chunk in flight). `g_sysex_drop` is not in the
  stats file, so whether any chunk was dropped and resent could not be read.
- **Not done yet:** the rest of the checklist: step 4 (FX 2 lines and tanks at both slots, the samplers, a tape
  recording, the lift, the album, the radio, USB disk mode, then `heap_free` / `heap_largest` / `tank_short` again)
  and step 5 (a set past the budget). The `plugins` line's `heap_free` / `heap_largest` of this read were not noted
  down here: read them with step 4.

## e12: plugin memory in coop's own SDRAM (5,760 KiB), four position buffers, the clipboard shares region B — device test (2026-11-21: memory confirmed, CPU regression with rings: not acceptable; superseded by e13)

What changed (`docs/plugins.md` "e12"): no plugin byte comes from the stock heap 0 any more. The plugin images, the
engines' voice state, the screen objects, the control tables and the effect position buffers come from one arena in
coop's own SDRAM: region A above the core (`0x02CE0000..0x03000000`, 3,276,800 bytes), then the upper 2.5 MiB of
region B (`0x02980000..0x02C00000`, filled downwards). **The budget is the arena: 5,898,240 bytes (5,760 KiB), was
1,728 KiB.** The synth and drum positions share one position buffer (four buffers, not five), the budget charges one
of them and up to three more are carved from what is left. The stereo clipboard keeps a floor of 1.5 MiB of region B
(17.8 s of right channel, all four slots together) and whatever the plugins do not take: all 4 MiB (47.5 s) while
the plugins fit region A. A track's old clip is now given back when the track is lifted again. The files load in
name order. Build tag `e12`, the boot picture's line `coop e12 - 246`. Existing `.coop` files load unchanged.

- `build/pkg/op1_246_coop_e12.op1` sha256 `d89b3e1838960f322be52ccda301006024d4a6ace0a7f6b42e5aab63febb9cae`, copy in Downloads. Release image as e11
  (`tools/build_device_package.sh ... --tape-stereo --set-word g_coop_spread=32767`): `verify_bootloader.py` PASS (no
  te-boot.ldr), `pkg_check` = only `OP1_vdk.ldr` differs. The ELF holds the tag `e12` (and no `e11` / `e10`), the
  package's LDR the line `coop e12 - 246`.
- The site does not offer e12 yet (no web delta made), and its memory bar still computes e11's rule: it follows the
  owner's device result. The rule to port is in `docs/plugins.md` "e12".

**Gate notes (op1emu, 2026-11-20, one run at a time, a fresh NAND each; images `--tape-stereo --set-word
g_coop_spread=32767 --set-word g_coop_force_stereo_out=1`; scripts and outputs in `I:\op1emu-tmp\e12gate\`)** [C
op1emu]:

| Run | Result |
|---|---|
| **the stock image**, `--watch-writes 0x027ffb04-0x02ffffff`, pristine first boot, `det_heap_late.txt` (sounds, kits, tape record and play, lift, drops into the samplers and the tape) | **0 writer PCs, 0 stores**: the stock does not write regions A or B |
| (a) no plugins: pristine `det_chord.txt`, e12 vs e11 (`emu_gate.py`) | **identical over 2,281,355 frames** (max diff 0, the second try of both runs; the first tries' onsets differed by a block, as known); `plugmem budget 5898240 used 0 ... clip 4194304`, end `heap_free 2729484` as e11 |
| (b) **the 13 catalogue files**, first boot | 13 loaded, `fscr_built 11 scr_built 2`, no fault: `plugmem budget 5898240 used 980992 over_budget 0 heap_short 0 ... heap_free_boot 15739524 scratch_files 11 fxbuf 134784 fxbuf_mem 539136 fxbuf_short 0 fxbufs 4 fxbuf_refused 0 arena 1385344 arena_a 1385344 arena_b 0 clip 4194304`; end **`heap_largest 2722396 heap_free 2725820`** (e11: 1111444 / 1205596; a boot without plugins: 2729052 / 2729484) |
| (b) the same NAND, second boot, `det_chord.txt` | 13 loaded, the same `plugmem` (`heap_free_boot 16069700`); end `heap_largest 3055244 heap_free 3055996` |
| (b) an effect of the 13 selected and played (`det_fx_select.txt`) | the last row of the stock FX list picked at synth FX 1, a chord through it: `fxctl p0=8231i6`, `plugin_blocks 8231`, `fx1_plugin=11294/24221` (e11: 11296/24189), no fault |
| (c) **the arena filled**: the 13 + 13 test engines (12 of 400,000 bytes of `.bss`, one of 80,000) + one more of 64,000, `det_heap_late.txt` | **boots** (the tape's cache got its 24 nodes) and plays 214 s, no fault: `plugins seen 27 loaded 26 failed 1`, `plugmem budget 5898240 used 5863488 over_budget 1 ... fxbufs 1 fxbuf_refused 11 arena 5863488 arena_a 3247360 arena_b 2616128 clip 1578176` (99.4 % of the budget; 5,312 bytes of region B and 29,440 of region A unused), `plugfile zzover.coop failed needs 64192: over the plugin memory budget`; inventory `... (needs 63 KB, 33 KB of 5760 KB left)`. One position buffer: instrument FX 2 took it (`p3` 12 inits, every effect in turn), master FX 2 got none: eleven `plugfx <name> no memory for this position: slots 4` lines, the slot stayed off, no fault. A lift and a drop at the floor: `edits lifts 1 drops 1 errors 0 mono_fallbacks 0 pool 789088`. End `heap_largest 1982436 heap_free 1983796`: e11's late-use figure (1,987,460 after every FX 2 tank and line) less 3,664 |
| (c) the same NAND, second boot | the same 26 loaded and the same file refused (name order), `used 5863488`, end `heap_free 3054444` |
| (d) one scratch effect at three positions (`det_fx_two_pos.txt`, tape_echo) | `fxbufs 4`, `fxctl p0=21646i1 p3=35129i1`, `plugin_blocks 21646 s2_instr 35129`, no fault |
| (d) switching effects at a position (`det_fx_switch.txt`, cs_hall + tape_echo) | `p0=71372i3 p3=47199i3`: one init per selection, no fault |
| (d) every position, synth / drum / synth / drum (`det_fx_fickle.txt`, `det_fx_petri.txt`) | fickle: `p0=41010i2 p1=14809i2 p2=32147i2 p3=43063i1 p4=25835i1` (the latch and fn words as e11: `1/2/4/0`), the block and init counts **equal to e11's gate to the block**; petri: `p0=40445i2 p1=4595i2 p2=21933i2 p3=33068i1 p4=15841i1`. Synth and drum each re-initialised at every change (two inits each across synth, drum, synth, drum) with the two positions on one buffer; no fault |
| (d) **one buffer only** (`g_coop_fxbuf_max=1`), `det_fx_two_pos.txt` | `fxbufs 1 fxbuf_refused 1`, `plugfx tape echo no memory for this position: slots 3`; `p0=21645i1` plays, `p3=0i0`, `s2_instr 0`; inventory row `tape_echo.coop ... loaded ... no memory for this position`; no fault |
| (d) one buffer only, `det_fx_fickle.txt` | `fxbufs 1 fxbuf_refused 3`, `plugfx fickle no memory for this position: slots 2 3 4`; **the synth and drum positions both play from the one buffer** (`p0=41009i2 p1=11370i2`), the master and both FX 2 positions stay off (`p2=0i0 p3=0i0 p4=0i0`, `s2_instr 0 s2_master 0`), 118 s of audio, no fault |
| (e) clipboard, no plugins (the whole region), `det_tape_liftdrop.txt` | `edits lifts 1 drops 1 errors 0 mono_fallbacks 0 pool 2097152 pool_used 167648`; the dropped chord plays back as wide as it was recorded (side / mid 0.26 against 0.26) |
| (e) **a lift past the pool** (`g_coop_clip_max=65536`: 32,768 samples, the clip 167,648) | `mono_fallbacks 1 pool 32768 pool_used 0 errors 0`; the drop plays back **mono** (side / mid 0.000, the level kept), nothing else differs |
| (e) the pool at its floor (the arena full: the 13 + the 13 test engines), the same lift and drop | `clip 1578176`, `pool 789088 pool_used 167648 mono_fallbacks 0`; plays back at side / mid 0.26 |
| (e) lifting a track again (`det_tape_relift.txt`: two takes on two tracks, shift + lift, shift + drop, twice) | `edits lifts 3 drops 2 errors 0 mono_fallbacks 0 pool_used 335296 pool_moves 1`: two clips' worth in the pool after three lifts (e11 kept all three), one slot moved down; the last playback at side / mid 0.25-0.26, no fault. The move itself on the host: `tests/host/clip_pool_test.cc`, 1,800 random lifts on three pool sizes, 1,205 moves, 557 that did not fit, every slot intact [C host] |
| (e) `det_tape_liftdrop3.txt` | as e11's: 1 lift, 1 drop, no fallback, stereo |
| (f) **who writes the two regions** with the 13 files and an effect played, and on pristine with a lift and drop (`--watch-writes 0x02800000-0x02bfffff 0x02cc1280-0x02ffffff`) | region A above the image: `coop_arena_take` (the zeroing, 346,336 stores), the loader (`relocate`, 398,947; the control tables' parser), the plugins' own code running in the arena (11.2 M stores up to `0x02DCF7EC`), and at the first frame the screen objects (`build_one`, `escr_init`, `coop_screens_frame` and the stock screen constructor at `0x01A3BAC8..`, 13 stores each for 13 screens). Region B: the staging block during the scan only (2.64-2.72 s: the stock NAND layer and the stock copy loop at `0x01AA6E40`, up to `0x0281B994`), and on the pristine run `coop_lift` alone (167,648 stores, `0x02800000..0x02851DBE`). **Nothing else writes either region; nothing writes between the image's end and the arena.** Both runs complete, no fault |
| (g) saw: a note (`det_eng_note.txt`) | `plugmem ... used 3968 ... arena 3968`, `eng_renders 11725 eng_noteons 17`, `eng_saw=7345/7440`, `scr_built 1`, no fault; end `heap_free 2729524` |
| (g) rings: every model (`det_eng_rings.txt`) | `used 667712`, `eng_renders 118595 eng_noteons 329`, `eng_rings=9602/34129`, `eng_rings_block=54444/115235`, `scr_built 1`, 132 s, no fault |
| (h) INVENTORY over USB/IP (`coop_usb.py --transport usbip`) | header `budget 5898240, used 980992, fxbuf 134784, fxbufs 4, clip 4194304`; every row with `need` and `scratch` (rings 667712 / 0, tape_echo 14592 / 134784, bitcrusher 5440 / 32, ...: they sum to 846,208, + 134,784 = `used`); a 428-byte file PUT after the boot: `new`, size 428 (was 0 before e12); the old parser reads the text unchanged |

Not shown in op1emu [I]: anything the caches decide (the code is cache-synced after every relocation as since e4b,
but op1emu has no caches); region A above 0.75 MiB and region B's top on real SDRAM; the cycle cost of plugin code and
state in this SDRAM bank instead of heap 0's (kennel measured a slower cold line fill there: `docs/plugins.md` "e12");
the stock's sampler recording, album, radio and disk mode against the two regions (they were not run under the write
watch); a NAND read landing in region B on the device (the staging block; in op1emu the NAND layer writes it).

**What to do:**
1. Flash `op1_246_coop_e12.op1` (keep your plugins). The boot picture's line reads `coop e12 - 246`. It must boot
   to the tape as e11, with all your plugins. **If it hangs or crashes at boot with plugins, that is the cache or
   SDRAM question: note it and go back to e11.**
2. With the 13 catalogue plugins: read `coop_stats.txt` over USB MIDI (`python firmware	ools\coop_usb.py stats -o stats.txt`, the OP-1 plugged in normally, **not** disk mode, the librarian's browser tab closed; the agent can run it: yaffs files are not visible in disk mode). Expected: `coop_stats 3 build e12`,
   `plugins seen 13 loaded 13 failed 0`, **`plugmem budget 5898240 used 980992 ... fxbufs 4 fxbuf_refused 0 arena
   1385344 arena_a 1385344 arena_b 0 clip 4194304`**, no `plugfile` or `plugfx` line. On the `plugins` line note
   `heap_free` and `heap_largest`: the emulator says about 3.05 MB on a normal boot (e11: 1.5 MB), i.e. what the
   OP-1 has without any plugin.
3. Play each kind: a scratch effect at synth FX 1 (tape echo), the same or another at instrument FX 2 and at the
   master (two different effects at two positions at once), switch effects at one position a few times, go synth ->
   drum -> synth with a plugin effect on both: each restarts cleanly, no crackle, no stuck sound. Play saw and rings
   (every rings model). **Listen for anything e11 did not do**: glitches, a slower UI, notes cutting out.
4. Read the stats again (`coop_usb.py stats`) after a few minutes of that and compare the `cost` line's `fx1_plugin`, `s2_instr`,
   `s2_master` and the `engines` line (rings, saw: avg / max per block) with an e11 stats file of the same playing.
   **These figures are the main result**: the plugins now run from another SDRAM bank than heap 0, and only the
   device can say whether that costs cycles.
5. The clipboard, on the tape: record about 8 bars in stereo (something wide: a synth with spread, or two tracks
   panned), lift it, drop it elsewhere, play it: still stereo. Lift and drop a few more times (before e12 the
   stereo clipboard ran out after 47.5 s of lifts in a session; now each new lift of a track gives its old clip
   back). Then lift one clip **longer than 48 s** (the whole pool with your 13 plugins) and drop it: it must drop
   and play, in mono, nothing else wrong; the stats `edits` line then shows `mono_fallbacks 1`. Read the stats
   (`coop_usb.py stats`) for the `edits` line (`pool 2097152` expected: samples).
6. The librarian: the plugin list shows all loaded; its memory bar still uses e11's sums until the site knows e12.
7. If you have more plugins than the 13: add them, power-cycle, repeat 2. Nothing should be refused until the bar
   is far beyond what e11 allowed; a refused file is named in the stats and the list (`... KB of 5760 KB left`), and
   the OP-1 must always boot.

**If something is wrong:** power off; recovery: hold COM at power-on and flash `op1_246_coop_e11.op1` (or stock).
Your plugin files are untouched by either; e11 loads the same files under its 1,728 KiB budget.

**Result (the owner's OP-1, 2026-11-21; e12 and e11 flashed back to back, the stats read over USB MIDI with
`tools/coop_usb.py stats`, files in `docs/device-stats/e11_2026-11-21_*`, `e12_2026-11-21_*`):**
- **Memory: confirmed.** Boots with the 13 files, all loaded: `plugmem budget 5898240 used 989248 over_budget 0
  heap_short 0 ... heap_free_boot 16044276 scratch_files 11 fxbuf 134784 fxbuf_mem 539136 fxbuf_short 0 fxbufs 4
  fxbuf_refused 0 arena 1393600 arena_a 1393600 arena_b 0 clip 4194304` (the catalogue's 2026-11-21 files: rings
  0.10 etc., hence 989,248 and not the gate's 980,992). The inventory's `budget used fxbuf fxbufs clip` and the rows'
  `need` / `scratch` read over USB MIDI. Nothing faulted: the cache sync, region A above 0.75 MiB and the staging
  block in region B work on the device.
- **CPU: a regression, not acceptable for rings.** rings 0.10, six voices, the tape rolling or not: the whole
  callback 367-403 k cycles a block per 5 s window on e11 (window maxima 450-660 k, `io_gap_max` <= 25 M) against
  **457-520 k on e12 (maxima 600-856 k: dropouts; the owner: the UI "very laggy"; `io_gap_max` up to 204 M)**.
  `eng_rings` per voice render 24,892 / 82,178 (avg / max) on e11, 27,529 / 213,030 on e12; its code stall cycles
  6,060 -> 10,420, data stalls 4,423 -> 5,553. Nothing sounding: 91-92 k a block on both.
- fickle 0.2 at synth FX 1 on a sampler: playing windows 255-257 k a block on e11, 262 k on e12 (+6 k); blocks over
  the stats mark ~300 against 971; `fx1_plugin` 29,615 -> 43,188 cycles a block, of which code stalls 11,427 ->
  22,398 and data stalls 4,308 -> 7,742 (the stock sampler's voices got 4.8 k a block cheaper in the same windows).
  On the device fickle costs ~30 k a block where op1emu says 18 k.
- The CPLB tables are the same on both cores and map region A cacheable and write-back like heap 0
  (`docs/plugins.md` "e13"): it is not a cache-policy setting. Steps 5 (the clipboard) and 7 were not done.
- **The owner is back on e11.** e13 (below) puts the plugins' images and voice state back in heap 0 by default and
  makes the placement a boot policy to measure.

## Plugin idle paths and the divides out of the steady block: tape_echo 0.4, cs_hall 0.3, cs_plate 0.3, fickle 0.3, petri 0.3; tape_drive 0.6, bitcrusher 0.3, rings 0.10 — device test (pending, 2026-11-20)

Two changes to the plugins, `docs/efficiency-audit.md` findings 8 and 6. Core unchanged: the files load on e10, e11
and e12 as their versions before did (no new import). **In the catalogue since 2026-11-21** (`plugins/<id>/<id>.coop`,
the eight files of `firmware/build/plugins-next/`), after the op1emu gate on e11 and e12 below ("Gate notes"). The
two tables that follow are host and GNU simulator.

**1. No divide in a steady block (finding 8), bit-exact.** The knob scaling `(word - lo) x 32767 / (hi - lo)` and its
kin ran as libgcc divides every block (~190 cycles each). Each plugin now keeps the values per slot and redoes one
only when its word moves; rings FM's five divides a render are the constant's shifts.

| Plugin | divides a block before | simulator, cycles a block: before -> after (the plugin's own test scenario) | proof it is the same output |
|---|---|---|---|
| tape_echo 0.4 | 6 (7 and a table walk with sync on) | 16,079 -> 15,039 | te-op1-cfw's echo, 5 tempo runs x 20,000 blocks host, 3,000 simulator: bit-identical |
| cs_hall 0.3 | 4 | 17,753-21,942 -> 17,277-21,486 by scenario; the turns held 18,517 -> 18,027 | te-op1-cfw's hall, host and simulator: bit-identical |
| cs_plate 0.3 | 4 | 17,873 -> 17,379; still knobs 17,419 -> 16,788 | te-op1-cfw's plate, 52,309 blocks host, 10,835 simulator: bit-identical |
| tape_drive 0.6 | 4 | 9,197 -> 8,689 | te-op1-cfw's tape, 20,000 blocks host and simulator: bit-identical |
| bitcrusher 0.3 | 7 and a 64-bit multiply | 6,507 -> 5,426 (the test moves each word in 3 blocks of 10) | te-op1-cfw's crusher, 2 x 40,000 blocks: 0 mismatching samples |
| fickle 0.3 | 4, + 2 in the reverb mode, + 1 for the display while listening | reverb 20,299 -> 19,275, delay 13,304 -> 12,195, slip 14,590 -> 13,626 (default clock) | the test's digest of every output and display word, 0.2's sources against 0.3's: equal, host (60,000 blocks) and simulator |
| petri 0.3 | 4 | all effects 21,111 -> 20,488 | the digest, as fickle: equal, host and simulator |
| rings 0.10 | 5 a voice render in FM | the FM render 15,726 -> 14,810 (the fm stage 8,548 -> 7,637) | 0.9's output byte for byte: the 90 s scenario on the host, 6,000 FM blocks in the simulator |

In every one of these tests the controls also move every block for stretches and jump at arbitrary blocks, and the
effect is re-initialised mid-run. A block in which all four turns move at once costs 150-340 cycles more than before
(the compares and stores beside the same divides).

**2. Idle paths (finding 6).** An effect that is on, with exact silence at its input and nothing left to play,
stops computing: tape_echo, cs_hall, cs_plate, fickle, petri. (When an effect is off or not selected its render is
not called at all; that was already so.) The check is at the top of the render: the first block with any input is
rendered in full. Each README has an "Idle" section with the conditions and the test figures.

| Effect | idle when (input exact silence, and) | hold | never idle | while idle against the same effect without the path | after waking | simulator: playing -> idle, cycles a block |
|---|---|---|---|---|---|---|
| tape_echo | every pair written to the tape is zero | 1.52 s (the tape once round) | hold; feedback 0.97 and up; a ghost the loop's rounding keeps (high feedback, new tape) | identical | identical (0 LSB in every test) | 14,371 -> 1,382 |
| cs_hall | its wet output is exact zeros | 1.06 s | freeze; infinite decay | identical | 0 on the first block, then 4-10 LSB under a loud tail: the hall's own rounding noise, the same as between two halls that never idle | 17,720 -> 1,084 |
| cs_plate | its wet output is exact zeros | 0.93 s | freeze | identical | 0 on the first block, then 4 LSB (defaults) to 29 LSB (longest decay, full wet): the same as between two plates that never idle | 16,869 -> 1,113 |
| fickle | the wet channel took in, gave and wrote only zeros, every small state at rest, the looper listening | 16,448 clock samples (0.69 s at 24 kHz, 8.2 s at 2 kHz) | freeze; a loop playing or dubbing; the delay's modify from 31,000 up | identical | identical (bit-exact: its digest test runs with the path on) | 19,190 -> 1,887 (reverb), 12,128 -> 1,927 (delay), 13,171 -> 2,889 (slip) |
| petri | only zeros written to the record and given as wet | 1.37 s (the record once round) | hold; warp with a bus not at rest | identical | 0 on the first block, then 0-3 LSB (defaults), up to 9 LSB (repeats and space at the top): the same as between two that never idle; the grains are the same grains | 16,565-29,359 -> 2,043-3,316 |

A playing block costs 50-180 cycles more than before for the path (a six-sample silence test and the state's flags);
a silent block that is not idle yet 400-900 more (the input read through, the check). fickle's slip is 280 cheaper.
Thresholds: none. Every level in the conditions is zero, which each effect does reach (the hall's dithered rounding,
the plate's tap gain, fickle's and petri's rounding towards zero, the echo's tape); where one does not (the echo's
ghost) the effect stays awake.

**rings' reverb model is not idled**, and it shows something to hear for: with a key of string + reverb held, the
voice that carries the reverb gives a steady -52 LSB (2 LSB of ripple) for as long as the key is down [C host, the
build as it ships: `plugins/rings/README.md` "0.10"]. It was always so (the reverb's rounding); it is why its tail
never gets under any idle level.

**Gate notes (op1emu, 2026-11-21, `tools/emu_gate.py`, one run at a time, a fresh NAND each; the images of the e11
and e12 gates: `~/op1emu-work/e11/e11g.ldr`, `~/op1emu-work/e12/e12g.ldr`, `--tape-stereo --set-word
g_coop_spread=32767 --set-word g_coop_force_stereo_out=1`; scenarios, scripts and stats in `I:\op1emu-tmp\idlegate\`;
"0.x" below is the catalogue file before the swap)** [C op1emu]. No fault in any run on either core.

| Run | e11 | e12 |
|---|---|---|
| (a) **the 13 files** (the eight new, and drive, ensemble, freq_shift, saw, wavefolder as they were), first boot, `det_fx_select.txt` (its wall wait lengthened: the 13-file first boot is slow) | `plugins seen 13 loaded 13 failed 0 ... over_budget 0`, `fscr_built 11 scr_built 2`; `plugmem budget 1769472 used 1510352 over_budget 0 heap_short 0 skipped 0 badname 0 tape_node 524288 heap_free_boot 14214300 scratch_files 11 fxbuf 134784 fxbuf_mem 673960 fxbuf_short 0`; the chord through wavefolder `p0=25458i6`, `fx1_plugin=10502/23949`; end `heap_largest 1104252 heap_free 1197372` | 13 loaded; `plugmem budget 5898240 used 989248 over_budget 0 ... heap_free_boot 15739524 scratch_files 11 fxbuf 134784 fxbuf_mem 539136 fxbuf_short 0 fxbufs 4 fxbuf_refused 0 arena 1393600 arena_a 1393600 arena_b 0 clip 4194304`; `p0=25470i6`, `fx1_plugin=10500/23947`; end `heap_largest 2722396 heap_free 2725820` |
| (a) the same NAND, second boot, `det_chord.txt` | 13 loaded, the same `plugmem` (`heap_free_boot 14544476`); end `heap_largest 1434892 heap_free 1527548` | 13 loaded, the same `plugmem` (`heap_free_boot 16069700`); end `heap_largest 3055244 heap_free 3055996` |
| (b) tape_echo 0.4: `det_fx_tape_echo_tempo.txt` | `tempo_q16 9502720 tempo_inc 241011616 p0=59874i1:16384/8/0/0`, `fx1_plugin=7902/18468` (0.3: 17197/19074: the silences now idle); the repeats 0.63 s apart at 95 BPM and 0.414 s at 145, the same intervals as 0.3 in a run beside it | `p0=59869i1:16384/8/0/0`, `fx1_plugin=7897/18457` |
| (b) tape_echo: `det_fx_two_pos.txt`; `det_fx_tape_echo.txt` (every position, its controls) | `p0=21646i1 p3=38591i1`, `s2_instr=5456/16174`; controls `p0=36117i2:11197/0/1/0 p1=49410i2:17749/0/1/0 p2=68153i1:15019/0/0/1 p3=77871i1:19114/0/1/0 p4=59609i1:16384/3/2/0` (the words the scenario expects), `fx1_plugin=9067/17532 s2_instr=3928/15693 s2_master=4149/15703` | `p0=21645i1 p3=38587i1`, `fxbufs 4`; the same control words, `fx1_plugin=9062/17467 s2_instr=3934/15690 s2_master=4146/15700` |
| (b) cs_hall 0.3: `det_fx_cs_hall.txt` | `p0=36861i2:1985/16384/32419/0 p1=53599i2:3605/16712/32767/0 p2=71240i1:2949/16384/32767/1 p3=81316i1:3933/16384/32767/0 p4=63054i1:3277/16384/32111/0` (as expected), `fx1_plugin=9823/22234 s2_instr=4064/20972 s2_master=4404/21019` | the same words, `fx1_plugin=9820/22232 s2_instr=4063/20972 s2_master=4404/21019` |
| (b) cs_hall + tape_echo: `det_fx_switch.txt` | 2 loaded, `p0=61033i3 p3=36861i3`: one init per selection | `p0=64479i3 p3=40305i3` |
| (b) cs_plate 0.3: `det_fx_cs_plate.txt` | `p0=53030i1:3/23594/10178/0` (the 2026-11-11 gate's words), `fx1_plugin=9674/21028` | `p0=49595i1:3/23594/10178/0`, `fx1_plugin=10116/21026` |
| (b) fickle 0.3: `det_fx_fickle.txt` | `p0=41010i2:1/2/4/0 p1=14819i2:0/1/0/0 p2=32157i2:0/1/0/0 p3=43072i1:0/1/0/0 p4=25844i1:0/1/0/0` (0.2's words and inits), `fx1_plugin=9302/30206` (0.2: 17870/32298) `s2_instr=6781/28991 s2_master=6872/22082` | `p0=41009i2:1/2/4/0 p1=14809i2 p2=32147i2 p3=43063i1 p4=25835i1` (0.2's e12 line to the block), `fx1_plugin=9282/30110` |
| (b) petri 0.3: `det_fx_petri.txt` | `p0=40444i2:10/28688/9312/0 p1=4605i2:0/30000/8000/0 p2=21943i2 p3=33079i1 p4=15851i1`, `fx1_plugin=16844/35343` (0.2: 25915/35414) `s2_instr=9398/24278 s2_master=10576/23833` | `p0=40445i2:10/28688/9312/0 p1=21830i2 p2=39168i2 p3=50304i1 p4=33076i1` (a longer run: the first try ended on its wall-timed `quit` before the last events and was repeated with a longer wait), `fx1_plugin=13982/35341` |
| (b) tape_drive 0.6: `det_fx_controls.txt` | `p0=36117i2:11197/0/0/0 p1=42512i2:17749/0/0/0 p2=61256i1:15019/0/0/0 p3=70972i1:19114/0/0/0 p4=52710i1:16384/0/0/0`, `fx1_plugin=10418/12153 s2_instr=9157/10190 s2_master=9203/10237`; a held chord: 10,419 avg against 0.5's 11,076 | the same words, `fx1_plugin=10417/12189 s2_instr=9157/10194 s2_master=9203/10237` |
| (b) bitcrusher 0.3: `det_fx_controls.txt` with the first row picked | `p0=36862i2:11197/0/0/0 p1=57038i2:17749/0/0/0 p2=74679i1:15019/0/0/0 p3=84754i1:19114/0/0/0 p4=66493i1:16384/0/0/0` (0.1's words), `fx1_plugin=6374/8255 s2_instr=4474/6909 s2_master=5021/6476` | the same words, `fx1_plugin=6085/7808 s2_instr=4485/5889 s2_master=5021/6476`; a chord alone: 6,236 avg against 0.2's 7,704 |
| (b) rings 0.10: `det_eng_rings.txt` (every model, FM among them, the polyphony steps, the fast notes) | `plugmem ... used 667120`, `eng_renders 118600 eng_noteons 329`, `eng_rings=9513/34173 eng_rings_block=53934/115493`, `scr_built 1` | `used 668512` (0.5: 667712), `eng_renders 118595 eng_noteons 329` (0.5's counts), `eng_rings=9515/34170 eng_rings_block=53950/115475` (0.5: 9602/34129, 54444/115235) |

**The 13-file set and e10.** e10 charges what e11 charges (the e11 entry: "the heap figures equal to e11's to the
byte"): `used 1510352` of e10's 1,572,864, 62,512 bytes to spare (the set before: 1,502,128; the eight files are
8,224 bytes of heap larger together, 8,256 of e12's arena). On e11: 259,120 to spare. The site's memory sums give both
cores' figures to the byte (`web/src/core/plugins/memory.test.ts`).

**(c) The idle path in the signal path.** One effect alone on the NAND, switched on at synth FX 1; a chord at 39-40 s,
nothing until 80 s, the chord again, to the end (`idlegate/scen/idle_*.txt`). The measure is the stats' 5-second
windows (`win ... blocks cb_sum`: the whole audio callback's cycles a block); a window with no effect on is 45,675.
"Silent" is the windows 20-40 s after the first chord, the same to within 5 cycles in every such window and after
the second chord again.

| Effect | 0.x, silent | new, silent (idle) | the position costs: 0.x -> new, cycles a block | `fx1_plugin=` avg over the run, 0.x -> new | e12, new, silent |
|---|---|---|---|---|---|
| cs_hall 0.2 -> 0.3 | 66,223 | 49,441 | 20,548 -> 3,766 | 19,993 -> 7,033 | 49,437 |
| cs_plate 0.2 -> 0.3 | 65,381 | 49,472 | 19,706 -> 3,797 | 19,164 -> 8,109 | 49,468 |
| tape_echo 0.3 -> 0.4 | 63,251 | 49,643 | 17,576 -> 3,968 | 17,029 -> 5,452 | 49,635 |
| fickle 0.2 -> 0.3 | 68,056 | 49,918 | 22,381 -> 4,243 | 21,842 -> 4,894 | 49,914 |
| petri 0.2 -> 0.3 | 66,296 | 50,376 | 20,621 -> 4,701 | 20,077 -> 6,638 | 50,372 |
| cs_hall at **master FX 1** | 64,893 | 48,111 | 19,218 -> 2,436 | (no cost slot) | - |

- **Every one of the five goes idle in the real signal path and wakes**: the window with the second chord costs what
  the first did (cs_hall 98,111, the 0.2 file 101,716), and the windows after it fall back to the idle figure. What
  is left while idle, 3.8-4.7 k at the synth position (2.4 k on the master), is the idle plugin's 1-3 k and the
  wrap's conversions (audit finding 9).
- **Does a silent position give the effect exact zeros? Yes, in op1emu, at every position tried**: synth FX 1 and
  master FX 1 (the table), and instrument FX 2 and master FX 2 by the played runs above (cs_hall's `s2_instr=4064` avg
  against a 20,972 max over 81,316 blocks, `s2_master=4404/21019`; tape_echo's 3,928 and 4,149: both idle most of
  those runs). The conditions are exact (the top 16 bits of every input sample zero for a whole block, then the
  effect's own state): had any stage ahead left +-1 LSB, none would have passed. The 63 / 64 LSB with a flicker
  that the dumped output carries in silence comes after the effects (it is there with no effect on).
  [I] On the device the instrument positions take the same stock buffers; a tape track or the line / mic / radio
  input into the master is silence only if it is digitally so: the master's idle is the device's to show (checklist
  step 10 at master FX).
- **The audio against the 0.x file, same scenario** (the dumped output, 16-bit; "matched" = the two runs are
  sample-identical up to the first chord, see below):

| Effect | pair | first chord and its tail | the silence (idle against not idle) | the chord after 40 s idle, and its tail |
|---|---|---|---|---|
| cs_hall | matched | identical | identical | 2 LSB at the most under the chord, then the 1 LSB flicker (predicted: 4-10 LSB of the hall's own rounding, at the plugin's full scale) |
| cs_plate | matched | identical | identical | 2 LSB, then the flicker (predicted: 4 LSB at the defaults) |
| tape_echo | matched (the second 0.4 run; the first was not: 54) | identical | identical | **identical** (predicted: 0) |
| cs_hall at master FX 1 | not matched | 2 LSB | the flicker | 2 LSB |
| fickle | never matched (three 0.3 runs against two 0.2 runs) | 2 LSB in the best pair, up to 203 in the others | the flicker only | 70 in the best pair, up to 176 |
| petri | never matched (three and two) | up to 2,009 | the flicker only | up to 2,053 |

  **Why fickle and petri could not be compared sample for sample here.** Which block a key event lands in depends
  on the boot's timing, and that follows the plugin file on the NAND: a file of another size starts the effect a
  block or more apart, and the dumped output's 1 LSB flicker is then out of step from before the effect is even
  picked. Two runs of one file are identical more often than not (petri 0.2 twice and fickle 0.2 twice: 0 over
  114 s and 126 s), but not always, and an effect with free-running parts (fickle's clock and smear, petri's grains
  and random draws) then gives another take: **two runs of the same fickle 0.3 file on e11 differ by 196 under the
  first chord and 168 under the second, two of the same petri 0.3 file by 1,856 and 1,680, and by nothing in the
  silences**: the size of the 0.2 / 0.3 differences in the table. So those two rows show no difference that is the
  plugin's, and no proof either: fickle's and petri's sample-exactness rests on their host digests and the lockstep
  test (`tests/host/idle`). In every pair, matched or not, the silent stretches differ by the flicker alone.
- **(d) A scenario that is never silent, new file against the 0.x file** (a chord held 21-23 s, the effect at synth
  FX 1): **cs_hall identical** over 2,436,659 frames (e11); **tape_drive identical** over 65 s (e11, and the 0.6 run
  on e12 against 0.5 on e11; a second pair on e11 was not matched and differed by up to 106 from the FX list's blip
  on); **bitcrusher identical** over 1,441,325 frames (e12); **tape_echo identical** through the whole chord (e12;
  68 frames of up to 4 LSB before it, in the blip while the stock effects are scrolled past in the FX list, where
  unmatched runs always differ).

Not shown in op1emu [I]: the device's cycle figures (no cache model: the idle saving in cycles should carry, the
playing cost is 10-35 % higher there); the moment of going idle and of waking by ear; idle with a real tape or input
on the master; the longer-decay cases of the listening checklist; rings between the two cores sample for sample (the
fast-note section lands differently run to run); rings 0.6-0.9 had no op1emu gate of their own: 0.10 carries them.

**What to do** (headphones, the volume well up; each effect once at an instrument's FX and once at master FX):

1. Install the eight files with the librarian (the catalogue has them). The librarian shows
   tape_echo 0.4, cs_hall 0.3, cs_plate 0.3, fickle 0.3, petri 0.3, tape_drive 0.6, bitcrusher 0.3, rings 0.10.
   Rings goes from 0.5 to 0.10 on the OP-1: besides FM's divides that brings 0.6-0.8 (`plugins/rings/README.md`): a
   model or polyphony change cuts the notes still ringing, a chord's steals go silent at once, a string's line is
   cleared as it is reached.
2. **The tails into silence.** For each of the five: one loud note or chord, then hands off. Listen to the tail to its
   very end and a few seconds past it. Expected: the tail ends as it always did, and nothing happens when the effect
   goes idle (tape_echo about 4 s after the note at its defaults, cs_hall 7 s, cs_plate 11 s, fickle 1.5 s, petri 4 s):
   no tick, no step, no change in the hiss. If you hear anything at that moment, that is a fault: note the effect,
   the position and the settings.
   - cs_hall: also with decay near the top and the mix full wet (idle about 40 s after the note).
   - cs_plate: also with decay at the top, size 8, full wet (a tail of a minute and a half).
   - tape_echo: also with feedback at about 3 o'clock on a worn tape (age up).
3. **The wake on a soft note.** After at least ten seconds of silence, play one note as softly as the engine allows
   (low velocity, a soft sound), then a normal one. Expected: the first note has its effect from its first instant,
   the echo's first repeat, the reverb's onset, petri's first grain, at the usual level; no click at the note's start
   and no dip. Try it a few times with different waits (10 s, a minute).
4. **The turns while silent.** In silence (idle), turn each knob through its range and the fn layers, then play.
   Expected: as if the effect had been running: tape_echo's time glides to where the knob is, the hall's size change
   has happened, fickle's clock is where you left it. No burst of old sound from any turn.
5. **The states that hold a sound, left for a minute.** Each must still be sounding, unchanged, after a minute with
   no input:
   - tape_echo: hold (green click) on a phrase; feedback at the top (it howls on).
   - cs_hall: freeze (orange click) on a chord; decay at the top (infinite).
   - cs_plate: freeze.
   - fickle: freeze in reverb, in delay and in slip; a loop playing (white click), in a tape mode and in stretch; a
     loop with dub on; the delay with modify at the top.
   - petri: hold (green click) in mosaic, tunnel, pattern and warp.
   Then release each: the sound decays as before and the effect goes quiet.
6. **fickle's looper after a silence.** Play, wait ten seconds in silence, press loop. Expected: the loop holds that
   silence (the looper kept listening), as in 0.2; play again with loop off, press loop: the last phrase, as ever.
7. **petri, warp and back.** In warp while it sounds, switch to another effect, wait in silence, switch back to warp.
   Expected: as in 0.2 (whatever it did there).
8. **tape_echo's ghost.** Longest time, feedback at about 2-3 o'clock, age at minimum: one note, then wait two
   minutes with the volume up. You may hear a very faint repeat going on and on (it was there in 0.3 too). It is why
   this echo never goes idle at those settings; say whether you would rather have it cut.
9. **rings.** FM: play as usual; nothing should have changed. String + reverb: hold a chord for half a minute; nothing
   has changed there either (the reverb is as it was).
10. **The cost.** With one of the five at instrument FX 1 and nothing playing for ten seconds, read
   `coop_stats.txt` over USB MIDI twice, ten seconds apart, then once more after ten seconds of playing (`python
   firmware	ools\coop_usb.py stats -o stats.txt`, the OP-1 plugged in normally, **not** disk mode, the librarian's
   tab closed; the agent can run it). The `cost` line is cumulative since the boot (`fx1_plugin=avg/max/n`), so the
   figure for a stretch is `(avg2 x n2 - avg1 x n1) / (n2 - n1)` of two reads; the `win` lines' `cb_sum / blocks`
   give the whole callback per 5-second window without the sum. Expected: `fx1_plugin=` (or `s2_instr=` /
   `s2_master=` at FX 2) falls from its playing figure to about 3-6 k in the silent stretch (the plugin's 1-3 k and
   the wrap's conversions, which still run around an idle plugin: audit finding 9; op1emu: 3.8-4.7 k). These two
   figures per effect are the result that matters most.
11. tape_drive, bitcrusher: play them; they must sound as before. Their `fx1_plugin=` figure should be about 0.5 k
   and 1 k lower than their last recorded one.

**If something is wrong:** reinstall the plugin's previous file from the catalogue (the versions before these); note
which effect, where, and what you did just before.

Result: (pending)

## rings 0.11 and tape_echo 0.5: tails that end, so both can idle — device test (pending, 2026-11-21)

The two decisions left open by the entry above (2026-11-21: do both). Core unchanged; the two files load on e10, e11
and e12 as before (the same imports). They are in `firmware/build/plugins-next2/` (`rings.coop` 114,948 bytes,
`tape_echo.coop` 25,444 bytes), **in the catalogue since 2026-11-21, after the op1emu gate on e11 and e12** ("Gate
notes" below); the table is host and GNU simulator. Both change the sound a little, on purpose.

| | what changed | what was shown |
|---|---|---|
| rings 0.11 | The shared reverb of string + reverb writes to its buffer truncated towards zero (as upstream Rings; 0.10 floored). The steady offset a held key rested on is gone (0.10: -52 LSB with the default patch, -127 with damping 29,000) and the reverb's tail ends in exact zeros. With the tail gone, nothing sent and the voice's limiter at rest, the reverb is not run and a held, sleeping voice renders as silence. | The idle build against its twin that never idles: the same sample for sample, before, in and after the idle state (chords held 24 s and 100 s). Against 0.10: the offset (40-53 LSB mean) and 5-15 LSB RMS besides; the decay the same within the test's 0.25 s window. The reverb alone reaches exact zero 5.5 s (time 0.81) and 21.5 s (0.98) after a noise burst; 0.10 never. The other five models: 0.10's bytes. Simulator: four held voices 15,510 cycles a block before, 2,518 idle; playing +1.4 k a block. |
| tape_echo 0.5 | While the input is silent, a block of tape written wholly within 127 (-48 dBFS) is taken one LSB towards zero. The faint endless repeat at high feedback is gone (0.4 held 4 to 47 LSB of output for good) and every echo under feedback 0.97 fades to exact zero and idles. | In lockstep with 0.4's echo over 53 settings (feedback 0.6 to 0.97, new to worn tape, four times, sync): no difference while the input plays; at the defaults 4 LSB at most; the largest differences (up to 81 LSB) where 0.4's own echo is under about 100 LSB. All 53 reach exact zero and the idle state (longest time: 25 s at feedback 0.6, 59 s at 0.87, 107 s at 0.95, 166-238 s at 0.97). The build with `-DCOOP_TAPE_ECHO_GHOST` is still bit-identical to te-op1-cfw's echo. |

Details and tables: `plugins/rings/README.md` "0.11", `plugins/tape_echo/README.md` "The tail settles (0.5)".

**Gate notes (op1emu, 2026-11-21, `tools/emu_gate.py`, one run at a time, a fresh NAND each; the e11 and e12 gate
images as in the entry above; scenarios, scripts and stats in `I:\op1emu-tmp\gate2\`)** [C op1emu]. No fault in any
run on either core. **Both files are in the catalogue since this gate** (`plugins/rings/rings.coop` 0.11,
`plugins/tape_echo/tape_echo.coop` 0.5).

| Run | e11 | e12 |
|---|---|---|
| (a) **the 13 catalogue files with these two**, first boot, `det_fx_select.txt` (the longer wall wait) | `plugins seen 13 loaded 13 failed 0 ... over_budget 0`, `fscr_built 11 scr_built 2`; `plugmem budget 1769472 used 1511704 over_budget 0 heap_short 0 skipped 0 badname 0 tape_node 524288 heap_free_boot 14212948 scratch_files 11 fxbuf 134784 fxbuf_mem 673960 fxbuf_short 0`; end `heap_largest 1102900 heap_free 1196020` | 13 loaded; `plugmem budget 5898240 used 990592 over_budget 0 ... heap_free_boot 15739524 scratch_files 11 fxbuf 134784 fxbuf_mem 539136 fxbuf_short 0 fxbufs 4 fxbuf_refused 0 arena 1394944 arena_a 1394944 arena_b 0 clip 4194304` |
| (a) the same NAND, second boot, `det_chord.txt` | 13 loaded, the same `plugmem` (`heap_free_boot 14543124`); end `heap_largest 1433540 heap_free 1526196` | 13 loaded, the same `plugmem` (`heap_free_boot 16069700`) |
| (b) rings 0.11: `det_eng_rings.txt` (every model, FM and string + reverb among them) | `plugmem ... used 668216`, `eng_renders 118594 eng_noteons 329`, `eng_rings=9603/34183 eng_rings_block=54452/117934` (0.10: 9513/34173, 53934/115493), `scr_built 1` | `used 669600` (0.10: 668512), `eng_renders 118594 eng_noteons 329`, `eng_rings=9601/34187 eng_rings_block=54437/115613` |
| (c) tape_echo 0.5: `det_fx_tape_echo_tempo.txt` | `tempo_q16 9502720 tempo_inc 241011616 p0=59873i1:16384/8/0/0`, `fx1_plugin=8030/19140` (0.4: 7902/18468) | `p0=59869i1:16384/8/0/0`, `fx1_plugin=7976/18590` |
| (c) tape_echo: `det_fx_two_pos.txt`; `det_fx_tape_echo.txt` | `p0=21646i1 p3=38591i1`; the control walk `p0=36117i2:11197/0/1/0 p1=49410i2:17749/0/1/0 p2=68153i1:15019/0/0/1 p3=77871i1:19114/0/1/0 p4=59610i1:16384/3/2/0` (0.4's words), `fx1_plugin=9085/17920 s2_instr=3931/16328 s2_master=4161/16116` | `p0=21645i1 p3=35152i1`, `fxbufs 4`; the same control words, `fx1_plugin=9318/17914 s2_instr=4009/16273 s2_master=4264/16108` |

**The 13-file set and e10.** `used 1511704` of e10's 1,572,864 (e10 charges what e11 charges): 61,160 bytes to spare;
257,768 on e11. The two files are 1,352 bytes of heap larger together (1,344 of e12's arena). The site's memory sums
give both cores' figures to the byte (`web/src/core/plugins/memory.test.ts`).

**rings: the held reverb voice idles** (`gate2/scen/rings_hold3b.txt`: the second boot of the NAND `det_eng_rings.txt`
leaves, so the sound is rings on string + reverb and no sequencer key was pressed; damping turned down, two keys held
from 32 s to 92 s, a third key at 82 s). The stats' 5-second windows, the whole callback's cycles a block; no key
down is 45,670.

| | 0.10 | 0.11 |
|---|---|---|
| the chord just struck (first 10 s) | 91,508 / 90,132 | 92,687 / 91,304 (the rounding's and the idle state's ~1.2 k) |
| the keys still down, 13 s after the strike and on (seven windows, all the same) | **72,430** | **57,030** |
| the same, over no key down | 26,760 | 11,360 (two held voices through the stock's voice path, rings idle in each) |
| the third key struck into the held chord | 86,773 | 88,003 |
| `eng_rings_block=` avg / max over the 42,270 blocks of the hold | 22,295 / 47,021 | 12,892 / 48,482 (e12: 12,886 / 48,476) |

- The cost falls by 15,400 cycles a block once the strings are asleep and the reverb has run out, stays there for as
  long as the keys are down, and comes back in full with the new note. 0.10 pays for the reverb the whole minute.
  (The host's figure was 15,510 -> 2,518 for four voices; here two voices, measured with the stock's voice path
  around them.)
- In the dumped output both versions end on the same silence (0.10's -52 LSB offset is not in the output: the DC is
  taken out after the engine [I]).
- **A scenario trap found on the way:** after `tap seq` (how `det_eng_rings.txt` and the FX scenarios close a list) a
  held key is retriggered by the stock sequencer for as long as it is down (`eng_noteons 980` for four keys in a
  minute). A "held chord" in those scenarios never goes quiet; the hold was therefore run as a second boot.
- **The other five models against 0.10, sample for sample: the emulator does not allow it.** rings' output depends
  on where in its own run a strike falls: the same 0.11 file on e11 and on e12 differs by thousands of LSB in every
  section (4,545 to 7,193). In the one pair whose timing matched up to the first note (0.11 and 0.10 on e11), FM,
  the model without a noise burst, is within 2 LSB of 0.10 (the output's flicker), the others differ like any two
  runs. So nothing here contradicts "the same bytes as 0.10", and the proof stays the host's (`make models`).

**tape_echo: idle at the defaults and with the feedback up** (alone at synth FX 1; windows as above, no effect on is
45,675):

| Scenario | 0.4 | 0.5 |
|---|---|---|
| the defaults: a chord, 40 s nothing, a chord (`idle_tape_echo.txt`) | idle: 49,643 (the entry above) | **idle: 49,642** (e12: 49,636), the second chord's window 95,505, idle again; `fx1_plugin=5637/18175` |
| **a new tape (white at the bottom), the shortest time, feedback up** (green 22 detents: `te_hf_short.txt`): a short note, 45 s nothing, a note | **never idle**: 62,570 in every silent window to the end, `fx1_plugin=15241/18012`; the output rests on 62 / 64 LSB (a 1 LSB ghost) | **idle within the note's own 5-second window**: 49,643 in every window after it, and after the second note again; `fx1_plugin=4708/18169` (e12: 49,636, 4706/18105); the output rests on 63 / 64 as with no effect |
| the same, feedback higher (green 32 detents: `te_hf64_short.txt`) | never idle: 62,570, `fx1_plugin=15243/17705` | idle a few seconds later (the next window 52,502, then 49,643), `fx1_plugin=5532/18339` |
| **a new tape, the longest time (1.5 s a repeat), feedback up** (22 detents, each repeat 0.73 of the one before: `te_hf_long.txt`): a note, 106 s nothing, a note | **never idle**: 62,598 in every window of the last two minutes, `fx1_plugin=15833/17686`; a 1-2 LSB ghost | the last sample over the flicker 42 s after the note; **idle before the second note** (49,648), awake for it (67,880, then 63,100 for eight windows), **idle again about 46 s after it**; `fx1_plugin=10401/18575` (e12 the same: 49,640, 10394/18594) |
| the same, feedback higher (32 detents, each repeat 0.86 of the one before) | - | **not idle within the 106 s** (63,023 throughout; the tail still 24 LSB and falling 96 s after the note): the host's time to zero at feedback 0.95 is 107 s. A run with a longer silence was queued behind another gate and not made. |

- The feedback's value is not in the stats; "0.73" and "0.86" are the repeats' measured ratios (the tape's loss is in
  them, so the knob is higher: [I] about 0.8 and 0.93).
- **With the input playing, against 0.4** (`held_tape_echo.txt`: a chord retriggered for 23 s, then released): the
  timing-matched pair on e11 is **identical** from the boot through the whole chord; in the 3 s after the release
  6,892 frames differ by 1 LSB (the tail on a silent input, where 0.5 nudges: by design), then identical again.
  Cost while playing: `fx1_plugin=12610/17859` against 0.4's 12573/17937.
- The defaults' idle scenario and the tempo scenario against 0.4 gave no matched pair (the note landed a block
  apart); the tempo run's repeats are the same intervals as before (`tempo_q16` and the sync word as 0.4).

Not shown in op1emu [I]: the device's cycle figures; the sound of the shorter last tail and of the reverb's end by
ear (the checklist below); rings' reverb with damping near the top (the string itself rings for minutes); idle at
feedback 0.95 and over at the long times beyond what the last row shows.

**What to do** (headphones, the volume well up):

1. Install the two files with the librarian (the catalogue has them). The librarian shows
   rings 0.11 and tape_echo 0.5.
2. **rings, a long reverb tail into silence.** String + reverb, damping at about 2 o'clock, one loud chord, the keys
   held down. Listen to the tail to its very end and ten seconds past it. Expected: the string dies, then the reverb,
   smoothly into nothing; no tick or step at the end (the reverb stops being computed about a third of a second
   after its last sample), and no faint rumble or pressure left under the silence.
3. **rings, the held chord against before.** The same with rings 0.10 if you still have it: with the keys held after
   the sound has gone, 0.10 sat on a small constant offset (inaudible as a tone; you may hear a soft thump when
   the keys come up after a long hold). 0.11: nothing when the keys come up. If you can't hear either, that is the
   expected result too: say so.
4. **rings, a note into the held chord.** Keys still held from step 2, in silence: play one soft note, then a loud
   one. Expected: the reverb is there from the first instant of each, as ever; no click at the note.
5. **rings, the reverb's sound.** Play string + reverb as usual for a minute, damping low and high. Expected: the
   reverb as you know it (level, length, the slow shimmer). The other five models are untouched: play one or two.
6. **tape_echo, high feedback, new tape.** Age at minimum, feedback at about 2 o'clock, then 3 o'clock (under the
   howl), the time long: one note, hands off. Expected: the repeats fade as before for as long as you could hear
   them before, and then they are gone: no faint repeat left going round after a minute or two (0.4 kept one for
   good). Listen for the end of the tail: it should thin out, not stop short.
7. **tape_echo, the same on an aged tape** (age at about 2 o'clock and at maximum), and once with sync on (1/4).
   Expected: the same: a full fade to nothing, the audible part as in 0.4.
8. **tape_echo, short time.** Time at minimum, feedback at 3 o'clock: the metallic ring after a note fades out
   completely in a few seconds. This is the setting where 0.5 differs most from 0.4 (the last of the ring is
   shorter); say if the ring now ends too soon for your taste.
9. **tape_echo, what must not change:** feedback at the top still howls on for a minute; hold still holds for a
   minute; a normal echo at the defaults sounds as it did.
10. **The cost.** rings: with a string + reverb chord held in silence for 30 s, read `coop_stats.txt` over USB MIDI
   (`python firmware	ools\coop_usb.py stats -o stats.txt`, not disk mode, the librarian's tab closed) twice ten
   seconds apart (the figures are cumulative: take the difference of the two reads): the `engines` line's rings figure per render should drop to a few hundred cycles once
   the tail has gone (it stayed near 4 k a held voice in 0.10). tape_echo at an instrument's FX with feedback at
   3 o'clock: `fx1_plugin=` falls to about 3-4 k a minute or two after the last note (it never fell in 0.4).

**If something is wrong:** reinstall the plugin's previous file (rings 0.10, tape_echo 0.4: `build/plugins-next/` or
the catalogue); note which, the settings, and what you did just before.

Result: (pending)

## e13: where plugin memory goes is a boot policy; images and voice state back in heap 0 by default — device test (2026-11-21: the default policy confirmed; the A/B steps B-H not yet run)

What changed (`docs/plugins.md` "e13"): e12's arena made the plugins' code stalls about double on the device (the e12
result above), so e13 places each class of plugin memory in heap 0 (as e11) or in the arena (as e12) by a policy read
at boot: `eng` / `fx` (an engine's / an effect's image), `voice` (the engines' voice state), `buf` (the effect
position buffers). **Built-in default: `eng=heap fx=heap voice=heap buf=arena`** (code and the state inside the
images where e11 had them; the buffers, screen objects and control tables in the arena). Two budgets: the heap's
1,769,472 bytes (e11's, with e11's tape guard), the arena's 5,898,240. The policy file `/yaffs2/user/coop_place.txt`
is written over USB MIDI and applied at the next boot, with a boot marker that makes a policy that hangs the boot
cost one power cycle. New measurements: per-window `eng` / `peng` / `fx` / `s2` sums on the `win` lines, a `sysex`
line, CPLB change counters, and (policy token `probe=1`) a live fill probe and the SDRAM controller's registers.
Build tag `e13`, the boot picture's line `coop e13 - 246`. Existing `.coop` files load unchanged.

- `build/pkg/op1_246_coop_e13.op1` sha256 `efc40e65014b40b90bfed3ecb7ef2a0a5978aceae9fab6d4cfb35fbeb69e66d7`, copy in Downloads. Release image as e12
  (`tools/build_device_package.sh ... --tape-stereo --set-word g_coop_spread=32767`): `verify_bootloader.py` PASS (no
  te-boot.ldr), `pkg_check` = only `OP1_vdk.ldr` differs. The ELF holds the tag `e13` (and no `e12` / `e11`).
- No web delta made; the site still computes e12's rule (it overestimates on e13's default: it warns early, never
  late). The rule to port: `docs/plugins.md` "e13".

**Gate notes (op1emu, 2026-11-21, one run at a time under the shared lock, a fresh NAND each; images `--tape-stereo
--set-word g_coop_spread=32767 --set-word g_coop_force_stereo_out=1`; scripts and outputs in
`I:\op1emu-tmp\e13gate\`)** [C op1emu]. **op1emu has no caches and no SDRAM timing: it cannot show the speed
difference between the policies.** That is the device's part, below.

| Run | Result |
|---|---|
| pristine `det_chord.txt`, e13 vs e12 (`emu_gate.py`) | **identical over 2,381,872 frames** (max diff 0, first try) |
| pristine `det_chord.txt`, e13 vs e11 | aligned on the onset, max diff 2 over 2,577,006 frames (the two runs' onsets one block apart, the known spread; e12 vs e11 was identical on its second try) |
| **default (no policy file), the 13 catalogue files** (rings 0.10 ...), first boot, an effect picked and played (`det_fx_select.txt`) | 13 loaded, `fscr_built 11 scr_built 2`, no fault: `plugmem budget 1769472 used 833168 over_budget 0 heap_short 0 ... heap_free_boot 14906356 ... fxbufs 4 ... arena 560768 arena_a 560768 arena_b 0 clip 4194304 heap_budget 1769472 heap_used 833168 arena_budget 5898240 arena_used 156416 place_eng 1 place_fx 1 place_voice 1 place_buf 0 place_src 0`, `place eng=heap,fx=heap,voice=heap,buf=arena src=default marker_writes 0`; `fxctl p0=8231i6`; end `heap_largest 1891052 heap_free 1892644`; no marker written |
| the same NAND, second boot | the same `plugmem`; end `heap_free 2222820` |
| **`all=arena`** (e12's placement), two boots | 13 loaded: `budget 5898240 used 989248 ... heap_used 0 arena_used 989248 ... place_src 1`, `place eng=arena,fx=arena,voice=arena,buf=arena src=file marker_writes 1 marker_clears 1 marker_errors 0`; end `heap_free 2725820` / `3055996`: e12's figures to the byte; the marker gone from the NAND after each boot |
| **`all=heap`** (e11's placement, four buffers), two boots | 13 loaded: `budget 1769472 used 1372344 ... heap_used 1372344 arena_used 21632`, `fxbufs 4`; end `heap_free 1353468` / `1683644`; an effect played (`plugin_blocks 8231`) |
| `eng=heap fx=arena voice=heap buf=arena` | `heap_used 669192 arena_used 320096`, 13 loaded, the effect played |
| `img=arena voice=arena buf=heap` | `heap_used 539176 arena_used 854464` (the four buffers alone in heap 0), 13 loaded, the effect played from a heap buffer |
| a malformed file (`eng=heap fx=arena bogus=1`) | ignored whole: `place_src 2`, `src=malformed`, the default's figures (not `fx=arena`), no marker, boots |
| a file of 270 bytes of valid tokens | `place_src 2`, the default, boots |
| an empty file | a valid file: `src=file`, the default's figures, marker written and cleared |
| **the marker present** with `all=arena`, two boots | `place_src 3`, `src=marker marker_writes 0`, the default's figures both boots (the policy ignored), the marker still on the NAND; inventory `place_src marker` |
| `pf=1 probe=1` | `pf_mode 1`; `fill n 6 state 1 code_a_min 2051 code_heap_min 2051 data_a_min 518 data_heap_min 517 code_warm 2054 data_warm 518` (op1emu has no cache: cold = warm; the probe ran from region A and from its heap copy without a fault); no `sdram` line (skipped in op1emu) |
| **the heap budget full**: default policy, the 13 + five test engines (400,000 + 400,000 + 80,000 + 50,000 + 4,000 bytes of `.bss`), `det_heap_late.txt` | **boots** (the tape's cache got its 24 nodes) and plays, no fault: 18 loaded, `heap_used 1768160` of 1,769,472 (99.9 %), `heap_short 0`, `fxbufs 4`; every late allocation given (`tank_short 0`); end **`heap_largest 214036 heap_free 215628`** (e11 at its full budget: 167220 / 201012). Second boot: end `heap_free 1286804` |
| the same and one engine more | `over_budget 1`, `plugfile zzover.coop failed needs 64208: over the plugin memory budget`; inventory `... (needs 63 KB, 1 KB of 1728 KB left)`, the row's `need 64208 heap 64208` |
| **the arena full**: `all=arena`, the 13 + 13 test engines + one more, `det_heap_late.txt`, two boots | as e12's gate: 26 loaded, `arena_used 5871744` (99.6 %), `zzover.coop ... over the plugin memory budget`, `fxbufs 1 fxbuf_refused 11` with eleven `plugfx ... no memory for this position: slots 4` lines, `clip 1578176`, a lift and a drop at the floor (`mono_fallbacks 0`), no fault; end `heap_free 1983796` |
| **the heap guard alone**: `all=heap`, the heap budget set to 16 MiB (`g_coop_heap_budget`), the 13 + 13 test engines | boots, no fault: 16 loaded, ten refused `not enough memory left for the tape` (`heap_short 10`, the guard stopping at `heap_used 2572968`); end `heap_free 152844` |
| fickle alone at every position (`det_fx_fickle.txt`), default | `heap_used 26608 arena_used 136448`; `p0=41009i2 p1=18251i2 p2=35589i2 p3=46505i1 p4=29277i1`, the words as e11 / e12; a playing window `fx 12747076 s2 16977671` |
| the same with `all=heap` (the buffers in heap 0) | `heap_used 565784`; `p0=41010i2 p1=18256i2 p2=35593i2 p3=46509i1 p4=29281i1`, the same window sums |
| `det_fx_two_pos.txt` (tape_echo), `det_fx_switch.txt` (cs_hall + tape_echo), default | `p0=21645i1 p3=45478i1`; `p0=81711i3 p3=57538i3`: as e12 |
| rings, every model (`det_eng_rings.txt`), default: its image in heap 0 | `heap_used 666872`, `eng_renders 118600 eng_noteons 329`, 141 s, no fault; the windows' sums, e.g. `blocks 3446 cb_sum 365115789 ... eng 128074187 peng 126783355 fx 0 s2 0` (rings 36.8 k of the callback's 106 k a block in that window, op1emu cycles) |
| saw (`det_eng_note.txt`), default | `heap_used 2320`, `eng_renders 11725 eng_noteons 17` |
| the clipboard (`det_tape_relift.txt`) | `edits lifts 3 drops 2 errors 0 mono_fallbacks 0 pool 2097152 pool_used 335296 pool_moves 1`: as e12 |
| INVENTORY over USB/IP (`coop_usb.py --transport usbip`) | header `budget 1769472, used 833168, fxbuf 134784, fxbufs 4, clip 4194304, heap_budget 1769472, heap_used 833168, arena_budget 5898240, arena_used 156416, place eng=heap,fx=heap,voice=heap,buf=arena, place_src default`; rows with `need` / `scratch` / `heap` (rings 668536 / 0 / 666872, tape_echo 15760 / 134784 / 14096); `sysex polls 28169 rx 20 drop 0 replies 20 errors 0` after the read |
| the stats' new fields | `emu_gate.parse_stats` reads `plugmem.heap_used`, `plugmem.place_buf`, `sysex.drop`, `cplb.dcplb_changes` (0 in every run), `win[].peng`; the longest stats text of the gate 15,082 bytes |
| host tests | `tests/host/place_test.cc`: 25 policy texts, every byte value, every prefix of a valid text: valid texts taken whole, the others refused whole |

Not shown in op1emu [I]: every cycle figure; the `sdram` line and the PFCTL test that guards it (skipped in op1emu,
read under the boot marker on the device); the fill probe's real figures (op1emu: cold = warm); whether heap 0 on the
device has what op1emu's has (e11's question, answered once: the owner's e11 ran the 13 files at 1,510,352 of the
same budget).

**What to do: an A/B on the device, no flash between the steps.** The owner only plays; the coordinator writes the
policy file and reads the stats **over USB MIDI** (`python firmware/tools/coop_usb.py stats -o FILE`; the OP-1's
disk mode does not show `/yaffs2`, so there is nothing to copy by hand).

0. Flash `op1_246_coop_e13.op1` (keep the plugins). The boot line reads `coop e13 - 246`. `coop_usb.py hello`,
   `inventory`: `build e13`, 13 loaded, header `budget 1769472 used 833168 ... heap_used 833168 arena_used 156416 place
   eng=heap,fx=heap,voice=heap,buf=arena place_src default`.
1. For each policy below, in this order: the coordinator writes the file (`coop_usb.py --write put place.txt --to
   /yaffs2/user/coop_place.txt`; for A, `--write delete /yaffs2/user/coop_place.txt`), **the owner power-cycles**,
   the coordinator checks `stats` shows `place ... src=file` (`src=default` for A) and the expected `plugmem` figures,
   then **the owner plays, each for a full minute**: (i) rings, six-note chords held and re-struck, the tape
   stopped; (ii) a sampler sound with fickle at synth FX 1, the same kind of playing. The coordinator reads the stats
   after (i) and after (ii) (each read takes ~10 s; the `win` lines hold the last two minutes).

   | Step | `coop_place.txt` | What it is | `plugmem` expected (13 files) |
   |---|---|---|---|
   | A | (no file) | e13's default | `heap_used 833168 arena_used 156416` |
   | B | `all=heap` | e11's placement (four buffers in heap 0) | `heap_used 1372344 arena_used 21632` |
   | C | `all=arena` | e12's placement | `heap_used 0 arena_used 989248` |
   | D | `eng=arena fx=heap voice=heap buf=arena` | only the engines' images in the arena (rings) | `heap_used 163976 arena_used 825568` |
   | E | `eng=heap fx=arena voice=heap buf=arena` | only the effects' images in the arena (fickle's code) | `heap_used 669192 arena_used 320096` |
   | F | `probe=1` | the default, with the live fill probe and the `sdram` line | as A; a `fill` and an `sdram` line |
   | G | `all=arena probe=1 pf=1` | e12's placement, line fills counted instead of stall cycles | as C; `pf_mode 1` |
   | H | `pf=1` | the default, line fills counted | as A; `pf_mode 1` |

   Then delete the policy file (back to the default) and power-cycle once more.
2. What to read, per step and per kind of playing:
   - the playing windows' `cb_sum / blocks` (the whole callback a block), `cb_max`, `peng / blocks` (rings' own
     renders a block), `fx / blocks` (fickle a block), `eng / blocks`; `io_gap_max` (the main loop: the UI's lag);
   - `engines ... eng_rings=avg/max/n/p0/p1` and `cost ... fx1_plugin=avg/max/n/p0/p1` (pf mode 0: p0 code stall
     cycles, p1 data stall cycles; mode 1: fills);
   - `cplb ... dcplb_changes icplb_changes` (expected 0 after the first windows: the tables are not being swapped);
   - F: `fill` (`code_a` against `code_heap`, `data_a` against `data_heap`: is a cold line dearer in region A while
     the OP-1 plays?) and `sdram`;
   - `sysex ... drop` (expected 0).
3. The questions the table answers: A against B: do the buffers in the arena cost anything (fickle)? A against C:
   is the default as good as e11 for rings? D and E against A: is it the engine's image, the effects' code, or both?
   F, G, H: is a fill dearer in region A, or are there more of them?
4. If a step's stats say `place ... src=marker` or `place_src 3`: the boot before did not finish with that policy.
   The OP-1 is on the default; note it, then `coop_usb.py --write delete /yaffs2/user/coop_place.try` and go on with
   the next step. If the OP-1 does not answer on USB MIDI after a power cycle: power-cycle once more (the marker then
   makes it boot on the default).

**If something is wrong:** power off; recovery: hold COM at power-on and flash `op1_246_coop_e11.op1` (or stock).
The plugin files are untouched by either. A policy file left on the OP-1 is ignored by e11 and e12.

**Result (the owner's OP-1, 2026-11-21, the default policy, read over USB MIDI): the default restores e11's speed.**
- Step 0 / A: `build e13`; `plugmem ... heap_used 834520 arena_used 156416 ... place_src 0` (the catalogue's rings
  0.11 and tape_echo 0.5, hence 834,520 and not the gate's 833,168); `sysex ... drop 0`; `dcplb_changes 0
  icplb_changes 0`: **the CPLB tables are not being swapped** while the OP-1 plays.
- rings 0.11: `eng_rings=27475/106262` (avg / max per voice render), `eng_rings_block=162542/228613`: e11's figures
  (27,222-28,058 avg, 104-117 k max; e12: 27,529 avg but 213,030 max). A held six-key chord: `peng` 158-160 k a
  block, flat; the whole callback 334-356 k, worst block 453 k (e12: 457-520 k, worst 600-856 k).
- The effects' idle paths work on the device: nothing sounding with tape_echo 0.5 idle at synth FX 1, `fx` 13.1 k a
  block in every window (44-46 k active on e11), the whole callback ~116 k against ~91 k with no effect on: an idle
  effect still costs ~13 k in `fx` and ~25 k of the callback (its render, the wrap's conversions, the stage and the
  mix after it: e14's rest paths and audit item 9).
- **Not yet run: the A/B steps B-H** (the other policies, the fill probe, `pf=1`), so why the arena is slower and
  what the buffers cost there are still open.

## rings 0.12: a tail that stalls under 4 LSB sleeps; the held chord that cost the same for 75 s — device test (pending, 2026-11-21)

**The finding this answers** [C device, rings 0.11 on e13, 2026-11-21]: string + reverb, a chord held about 75 s, no
effects: rings' own cost (`peng`) 151,700 cycles a block in the strike window, then 158,278-159,812 in every
5-second window to the end; "it sounded like it had died down". The damping was not noted.

**What the host and the simulator say** [C host, simulator; `plugins/rings/README.md` "0.12"]. With the default patch
six held keys are asleep 17.4 s after the strike (the last non-zero 16-bit sample at 17.1 s), host and simulator the
same samples; the cost falls from 10 s on. Nothing in the core strikes a held string again (`note_on` once, then
`render` with pitch and controls). A flat cost for 75 s needs strings that ring that long: with the other controls at
their defaults, damping from about 29,800 up (30,000: asleep at 91 s, the sum at 18 LSB, -65 dBFS, by 75 s; 31,000:
176 s). So the likely cause is a long tail at a high damping, quiet to the ear and not to the arithmetic [I]: 0.12
does not shorten that, on purpose (those strings still sound in 16 bits). What 0.12 does remove is a voice that
stalls for good between half an LSB and 4 LSB (FM and the strings at some structure settings from damping 26,000 up)
and computed at full cost for as long as its key was down: such a voice sleeps after 10 s under 4 LSB.

File: `firmware/build/plugins-next3/rings.coop`, 0.12, 115,036 bytes, sha256
`d39db785bb8541723694c39a545cd6e740658241745550b30853a144aef64963`. The same imports as 0.11 (loads on e10 and
later). **Not in the catalogue: it needs the op1emu gate first** (13 files load, `det_eng_rings.txt`, the held-chord
scenario of the 0.11 gate).

**Owner's checklist** (the stats are read over USB MIDI with `coop_usb.py stats`; I read `peng` per 5-second window):

1. Copy `rings.coop` 0.12 to the OP-1, restart, select rings. No effects, no sequencer.
2. **The default patch first** (this tells a long tail from a fault): string + reverb, polyphony "all", the four
   controls where a fresh rings patch has them (damping about 70 %). Strike a chord of four to six keys and hold it
   60 s. Expected: `peng` starts to fall about 10 s after the strike and is at its floor (a few thousand cycles a
   block, the six render calls) from about 20 s. If it stays flat here, the cause is not the tail's length: say so,
   that is a fault to find.
3. **The setting of the first test**: turn the damping to where it was (if it was high, near the top), hold the
   same chord 60 s, and write down where the damping stood (the kennel page's number). Expected at a high damping:
   flat for the whole 60 s, as before; that is the string still ringing under what the ear picks out. Say whether
   anything is still audible with the volume up near the end.
4. **A stalled tail** (what 0.12 changes): the FM model, structure fully down, damping at about 80 %; hold six keys
   45 s. Expected: silence to the ear after about 12 s, and `peng` at its floor from about 22 s after the strike
   (0.11: at full cost for as long as the keys are down).
5. Strike keys again after each of these: every note sounds at once, at its usual level, with no click.
6. By ear, any model: tails end as they did in 0.11; nothing is cut that could be heard.

## rings 0.13: the early cut, a voice under -66 dBFS for 5 s is faded out over 1 s and sleeps; a -60 dBFS file to compare — device test (pending, 2026-11-21)

**What it is** [C host, simulator; `plugins/rings/README.md` "0.13"]. The owner's decision after the 0.11 device run
(a chord at a long damping: `peng` about 162 k cycles a block for minutes, "sounded like it had died down" well
before 60-75 s): a voice whose own level has stayed under a threshold for 5 s is faded out over 1 s and then sleeps,
key held or not, in every model. The threshold is -66 dBFS per voice (16 LSB peak of the 16-bit output) in 0.13 and
-60 dBFS (33 LSB) in a second file built for this test. Above the threshold every sample is 0.12's (host proof); a
note struck during the hold or the fade plays as in 0.12; a voice that was faded out is struck from rest. The
reverb is not cut: fed nothing more, it runs its tail out and idles as since 0.11.

**What the host says the test will show, before it is run** [C host]:

- **At the very top of the damping, nothing changes, and the chord is loud.** Six keys, the default patch otherwise:
  the summed output peaks at -12 dBFS 60 s after the strike and -13 dBFS at 75 s in string + reverb, -8.5 and -10
  dBFS in the string model, and the strings ring for more than five minutes; with other settings of the structure
  and the brightness between -3 and -27 dBFS at 75 s (only the structure at the top ends inside a minute). No
  threshold near -60 dBFS reaches that, and 0.13 costs there what 0.12 costs. So if the chord of the 0.11 run could
  not be heard after a minute with the damping at the top, what made it quiet came after rings (the OP-1's
  envelope, the levels, the listening volume) [I]: step 1 below is there to settle that.
- **Where the cut works is the top tenth below the very top.** Seconds after the strike until every voice sleeps,
  six keys held (`peng` falls a step each time a voice sleeps, and is at its floor from the last time on):

| six keys, damping (of 32,767) | 0.12 | 0.13 (-66) | the -60 file |
|---|---|---|---|
| string + reverb, 28,000 (85 %) | 39.0 s | 30.7 s | 27.5 s |
| string + reverb, 30,000 (92 %): the four strings | 34.5, 61.0, 69.5, 90.2 s | 29.5, 51.1, 54.4, 72.3 s | 25.9, 45.4, 47.8, 63.7 s |
| string + reverb, 31,000 (95 %): the four strings | 56.3, 108.6, 122.8, 171.7 s | 44.9, 88.3, 99.6, 144.3 s | 39.2, 78.0, 87.3, 126.7 s |
| string + reverb, 32,767 (the top) | 97.8, 214.9, 255.4, 322.0 s | 77.0, 199.3, 255.4, 322.0 s | 67.0, 176.3, 254.5, 322.0 s |
| modal, 28,000: the six together | 62.8 s | 46.7 s | 40.6 s |
| modal, 30,000: the six together | 104.6 s | 77.5 s | 66.9 s |
| string, 30,000 | 20.4 s | 19.0 s | 17.5 s |
| one key (C3), string + reverb, 30,000 | 107.6 s | 84.7 s | 74.4 s |

  The summed output when the last fade starts: about -67 dBFS (0.13) and -57 dBFS (the -60 file) in string +
  reverb at 30,000; in the modal model the six voices fade together, with the sum at -57 dBFS (0.13) and -50 dBFS
  (the -60 file). The modal chord is the case most likely to be heard.

**Files** (both named `rings.coop`: the second replaces the first; side by side as `rings60` they would not cost the
same, the second copy gets no L1 lease):

| file | version | bytes | sha256 |
|---|---|---|---|
| `firmware/build/plugins-next4/rings.coop` | 0.13 (-66 dBFS) | 115,788 | `269258f483ef82fff0924a32dcafdba8931196cb3a4fda0b49a3294397ef7074` |
| `firmware/build/plugins-next4/alt60/rings.coop` | 0.13-60 (-60 dBFS) | 115,796 | `94c3422ca9cfab098ca0f3a07ca2a65eed8ac6ed07e9ecca301e6bc4313b24a1` |
| `firmware/build/plugins-next3/rings.coop` | 0.12 (the reference) | 115,036 | `d39db785bb8541723694c39a545cd6e740658241745550b30853a144aef64963` |

The same imports as 0.12 (they load on e10 and later). **0.13 is in the catalogue since the op1emu gate of
2026-11-22** (the gate notes after the checklist); the -60 file is not (copy it by hand for step 7).
`coop_usb.py inventory` shows which version is loaded.

**Owner's checklist.** Headphones; no effects, no sequencer; rings' other controls where a fresh patch has them
unless a step says otherwise. Do each timed step once per file, in the order 0.12, 0.13, the -60 file (copy the
file to the OP-1 as for 0.12, restart, select rings). The stats are not something to copy: they are read over USB
MIDI by the agent with `python firmware\tools\coop_usb.py stats`, and they keep only the last two minutes in
5-second windows, so say "done" right after each step and leave the OP-1 connected; `peng` per window should fall
at the times in the table.

1. **The longest damping** (the setting of the first run): damping fully up, string + reverb, polyphony "all",
   a six-key chord held 90 s. At a normal listening level, then again at a high one: can you still hear the chord
   at 60 s and at 90 s? The host says it is loud (-12 dBFS). If you cannot hear it, write down the synth's envelope
   settings and the volume: the quiet then comes after rings, `peng` will be flat in all three files, and the cut
   cannot help there. Expected in all three files: `peng` flat for the 90 s (0.13 and the -60 file lose the top
   string at 77 s and 67 s).
2. **The tail's end, string + reverb:** damping at about 92 % (write down the number the page shows), a six-key
   chord held 100 s. Listen for how the tail ends at a normal level and at a high one. 0.12 ends by itself near
   90 s; 0.13 fades the last string at 71-72 s, the -60 file at 63-64 s. Does either end sound cut, or shorter
   than 0.12's? `peng` should step down at about 30, 51, 54 and 72 s (0.13); 26, 45, 48 and 64 s (-60); 35, 61, 70
   and 90 s (0.12).
3. **The tail's end, modal** (the most exposed case: the six voices fade in the same second): the modal model,
   damping at about 85 %, a six-key chord held 70 s. 0.12 ends near 63 s, 0.13 fades at 45-47 s, the -60 file at
   39-41 s. Is the fade heard as an early end at a normal level? At a high level?
4. **A single quiet note:** string + reverb, damping about 92 %, one key (a low C) held. 0.12 rings to 108 s, 0.13
   fades at 84-85 s, the -60 file at 73-74 s. Listen at a high level for the last ten seconds before those times:
   is anything missed?
5. **A note struck during the fade:** repeat step 3 with 0.13 and strike one of the held keys again at about 46 s
   (during the fade), then repeat and strike at about 50 s (after it). Each time the note sounds at once, at its
   usual level, with no click and nothing odd in the first moment; the other voices stay silent.
6. **Nothing else changed:** play normally in a few models for a minute with 0.13: no note cut while it is still
   heard, no click when a tail ends.
7. Say which of -66 and -60 is acceptable by ear, or whether the level should move (up: the cut comes sooner;
   down: later). It is one constant (`OP1RINGS_EARLY_LEVEL`).

**Gate notes (op1emu, 2026-11-22, `tools/emu_gate.py` through a wrapper that also reads `op1.db` out of the NAND, one
run at a time under the shared lock, a fresh NAND per chain; images `~/op1emu-work/e13/e13g.ldr` and
`~/op1emu-work/e14/f.ldr` (e14's gate image: `prof=1 statsfile=1`); scripts, scenarios and outputs in
`I:\op1emu-tmp\r13gate\`)** [C op1emu]. No fault in any run, on either core, with either file. **0.13 is in the
catalogue since this gate** (`plugins/rings/rings.coop`, `op1/coop` build 0.13); the -60 file is not.

How it was run:
- **Holds on a second boot.** `det_eng_rings.txt` first (it selects rings and closes the list with `tap seq`, after
  which the stock sequencer retriggers held keys), then each hold scenario as the next boot of the same NAND, no seq
  tap. That NAND leaves rings on verb with polyphony index 1 ("2"; the earlier gates' held chords therefore ran at
  polyphony 2); each hold sets polyphony "all" with fn + green.
- **The damping set with the white encoder** (`turn enc3`), measured in `op1.db` after the run: a detent (two Gray
  steps) is 328 of the word, a fine detent (Shift held) 20; 22,937 + 21 detents + 9 fine = **30,005**; + 15 detents
  + 7 fine = **27,997**. Every hold's `op1.db` holds the intended words (model, polyphony 3, damping).
- Six keys **F3 G3 C4 E4 G4 C5** pressed 2 ms apart at 36 s guest (op1emu has no C3 key without an octave change;
  string + reverb keeps the last four strikes, C4 E4 G4 C5, as the host's chord does), held 100 s (default damping
  40 s, modal 70 s). `volume 255` (op1emu's volume knob; the default 128 leaves the tails at 1-2 LSB in the dump).
- **The stats keep 24 windows**; a poller copied `coop_stats.txt` out of the live scratch NAND every 12 s and the
  windows were merged by sequence number. Below, t is seconds after the strike at the window's start (window n:
  t = 5n - 34).
- Clicks: the dump's largest sample step per 50 ms block, at each fade against the 4.5 s before and the 4.5 s after,
  and every block whose step is over 3 times the median of the 2 s around it. The audio dumps were deleted once
  read, and every scratch NAND by the gate; none is left.

| Run | e13 | e14 |
|---|---|---|
| (a) **the 13 catalogue files with rings 0.13**, first boot, `det_fx_select` (slow waits) | 13 loaded, failed 0, over_budget 0; `plugmem budget 1769472 used 835408 over_budget 0 heap_short 0 ... scratch_files 11 fxbuf 134784 fxbuf_mem 539136 fxbufs 4 ... heap_budget 1769472 heap_used 835408 arena_budget 5898240 arena_used 156416 place_src 0`; end `heap_largest 1888812 heap_free 1890404` | the same figures to the byte |
| (a) the same NAND, second boot, `det_chord` | 13 loaded, the same `plugmem`; end `heap_free 2220580` | the same |
| (b) `det_eng_rings` (every model, FM among them), rings alone | `heap_used 668856`, `eng_renders`/n 118,600, `eng_rings=9649/34247` | `eng_rings=9648/34240`, `eng_rings_block=54706/118329` (on e14 the same scenario with 0.12: 9616/34199, with 0.11: 9603/34183; 0.11 on e11: 9,603/34,183) |

**Memory.** 835,408 is 0.11's 834,520 and 888 bytes (rings alone 668,856). e11 charges an engine's image as e13 does
in heap 0, so the e11 sum is 1,511,704 + 888 = **1,512,592** of e10's 1,572,864 (60,272 to spare) and e11's 1,769,472
(256,880); e12's rule 991,488. The site's sums give these to the byte (`web/src/core/plugins/memory.test.ts`,
`memory-e13.test.ts`, the vectors moved to the new file).

**(c) The held chords: rings' own cost (`peng`, cycles a block) per 5 s window, e14** (e13 the same to within 0.5 %;
its whole callback stays higher with every voice asleep, 77.6 k for e14's 55.1 k, as e13 has no voice skip):

| string + reverb, damping 30,005, t | 0.13 | 0.12 | 0.11 |
|---|---|---|---|
| 1-26 (four strings and the reverb) | 65,135-65,347 | 65,058-65,131 | 64,980-65,053 |
| 31-36 | **56,373** | 65,125 | 65,041 |
| 41 | 54,230 | 59,668 | 65,000 |
| 46-56 | **47,404** | 56,197 | 65,032 |
| 61 | 43,403 | 56,208 | 65,038 |
| 66-76 | **38,432** | 47,271 | 65,020 |
| 81 | 26,313 | 43,666 | 61,422 |
| 86-96 (keys still down) | **4,917** (every voice asleep) | 38,372 (one string awake at the release) | 56,106 |

- 0.13's steps fall at about 31, 45, 64 and 84 s (the last with the reverb's own tail), 0.12's at about 42, 49, 66 and
  83 s with the last string still awake when the keys come up at 100 s, 0.11's (no quiet sleep) at 81 s and never.
  The host predicted 29.5 / 51.1 / 54.4 / 72.3 s (0.13) and 34.5 / 61.0 / 69.5 / 90.2 s (0.12); op1emu's chord is
  not the host's (F3 and G3 struck first and stolen, real pitch words, hand-made strike order) and runs about 10 s
  longer in both builds, so the comparison is between the builds: **0.13 is at its floor 16 s or more before 0.12**,
  as the host says (18 s).
- The output (dump, peak per second): last second above -60 / -70 / -80 dBFS 51 / 63 / 81 s (0.13), 51 / 63 / 82 s
  (0.12), 51 / 62 / 99 s (0.11): to -80 dBFS the three builds play the same tail.
- **No click**: at the four fades the largest step is 11, 6, 6, 5 LSB against 15, 7, 6, 5 in the 4.5 s before and
  8, 6, 6, 3 after (the tail falling smoothly); no block flagged after the strike.
- Awake, 0.13 costs 216 cycles a block more than 0.12 (65,344 for 65,128: +0.3 %; the host's simulator: +0.7-1 %).

| the other holds, e14 | 0.13 | 0.12 | 0.11 |
|---|---|---|---|
| string + reverb, default damping (22,937), t 1 / 6 / 11 / 16 / 21 / 26 | 65,340 / 61,367 / 48,659 / 22,694 / **4,917** / 4,917 | 65,123 / 61,177 / 56,207 / 49,143 / 24,371 / **4,921** | 65,045 / 61,099 / 56,131 / 52,272 / 47,229 / 47,246 (one string stalled for the rest of the hold: 0.12's case) |
| the same, output: last second above -60 / -70 / -80 dBFS | 7 / 9 / 14 s | 7 / 9 / 13 s | 7 / 9 / 13 s |
| **modal, damping 27,997**, six keys: t 1-41 / 46 / 51-66 | 123,843 / **16,418** / 2,765 (the six asleep at about 46.6 s) | 123,534 / 123,535 / 123,536 to t 56, 114,629 at 61, 44,700 at 66, then released (asleep about 67 s) | - |
| the same, output | -80 dBFS last at 46 s, the floor from 47 s | -80 dBFS last at 65 s | - |
| one key held (the C4 key after `tap left`), string + reverb, 30,005 | 22,697-22,731 flat to t 91; asleep at about 98 s (the window t 96-101 is 10,597, from the asleep render's 1,200 and the playing 22.7 k), 2 s before the release; the output at its floor from about 98 s; the host's C3: 84.7 s | - | - |

- At the default damping 0.13 is at its floor about 3 s before 0.12 here (the host: 0.2 s), with the same audible
  tail to -80 dBFS; 0.11 never sleeps one string. Not an equality, as the brief expected, but not a difference in the
  sound.
- The modal chord, the most exposed case: the six fade together at about 45.5-46.5 s (host 46.7 s), the output
  falling from 8-10 LSB peak to the 1 LSB floor over that second; largest step 5 / 4 LSB in the fade against 6 before
  and 2-4 after: **no click**.
- The single note: whether `tap left` moved the octave is not shown (the dump's zero crossings, about 550 Hz, say
  little about a string's pitch), so the note may be C4; the fade at about 98 s: largest step 4 LSB against 4 before
  and 2 after, no click. Not run with 0.12 (its time, beyond the 100 s hold, would not show).

**(d) Restrikes, modal at 27,997** (`rs_m28.txt`, both cores, the same in both): C5 released at 81.3 s and struck at
81.5 s, inside the six fades (45.0-46.0 s after the strike in the hold), E4 released and struck at 86.2 s (every voice
faded), a seventh key A4 at 90 s (a steal of a faded voice). No fault; each note starts at once at its level (peaks
4,000 / 5,500 / 5,800 LSB in its first 50 ms, the step 0.4-0.55 of the peak as in a fresh strike's first blocks, and
the blocks before it 2-4 LSB / 70-85 LSB); `peng` 38.9 k, 51.4 k, 63.4 k in the windows after, 0 after the release.

**(e) The -60 file** (0.13-60, sha256 `94c3422ca9cfab098ca0f3a07ca2a65eed8ac6ed07e9ecca301e6bc4313b24a1`), both cores, the same runs as (a) and (c):
- (a) 13 loaded, failed 0, first and second boot on e13 and e14: `plugmem budget 1769472 used 835416 ...
  heap_used 835416 arena_used 156416` (8 bytes more than 0.13); e11 by the same sum 1,512,600 (60,264 under e10's
  budget).
- (c), e14 (e13 the same `peng` to within 0.3 %), `peng` per window:

| hold | -60 | 0.13 |
|---|---|---|
| string + reverb, 30,005: the steps | at about 26, 36, 51 and 71 s; **4,917 from t 76** | 31, 45, 64, 84 s; 4,917 from t 86 |
| the same, output: last second above -60 / -70 / -80 dBFS | 51 / 63 / **70** s | 51 / 63 / 81 s |
| default damping | 4,917 from t 21 (t 11: 44,194, t 16: 13,328) | from t 21 (48,659, 22,694) |
| modal, 27,997 | the six asleep at about 40 s (host 40.6 s), 2,765 from t 41 | about 46.6 s |
| the same, output: -70 / -80 dBFS last | 39 / 40 s | 42 / 46 s |
| one key, 30,005 | asleep at about 88 s | about 98 s |

- No click at any -60 fade: largest step 15, 8, 6, 5 LSB at the four string + reverb fades against 18, 10, 6, 6 before;
  modal 4-5 against 5 before and 2-4 after; the single note 4 against 4.
- No fault in any run. `eng_rings=9649/34241` in `det_eng_rings` (0.13: 9648/34240).

Not shown in op1emu [I]: every device cycle figure (op1emu has no caches and no SDRAM timing);
how the fades sound (the owner's checklist above, unchanged); the host's exact times (op1emu's chord and pitch words
are not the host's: the times run about 10 s longer in every build at 30,005, the modal times match the host's to
0.5 s; the -60 file 0.6 s).

## rings at the top of the damping: the chord that fades in 4 s and costs the same for 75 s — the measurement that tells why (pending, 2026-11-21)

**The device** [C device, the owner, rings 0.11 on e13]: string + reverb, the damping at its longest end, the
envelope's sustain at the top, six keys held: the chord fades in about 4 seconds; `peng` 158-163 k cycles a block
for 75 s, `eng_noteons 6`.

**The host and the simulator** [C; `plugins/rings/README.md` "0.13", "What the tail at the top of the damping is"]:
the same chord at damping 32,767 (which is what the knob's top sends: the stock iter range is 0 to 32,767 and
rings takes the word as it is) is the chord itself for minutes: the strings' fundamentals at -8 dBFS RMS in
200 Hz - 2 kHz at 5 s, -22 dBFS at 60 s, -26 dBFS at 75 s; no DC (under 50 LSB), nothing under 20 Hz or near
Nyquist, no stuck state; each voice a clean sine. A listener would hear it at 60 s. The simulator with the assembly
kernels gives the host's samples. No damping value from 0 to the top, and no structure or brightness at the top,
gives "gone in 4 s and never asleep" on the host. So either something after the engine closes the sound while rings
plays on, or rings on the device does not do what it does on the host. No rule in rings can be chosen before that
is known, and no rings 0.14 was built.

**Two observations, the first without any tool** (0.13 or 0.12 in place; e14 with the profile on, `prof=1`, for the
second; no effects, no sequencer, sustain at the top, the damping at its longest end, string + reverb, six keys held
60 s):

1. **Look at rings' own screen while the chord is held.** Its voice levels are drawn from the engine's 16-bit
   output before the OP-1's envelope. Do they stay up after the sound has gone (at 10 s, 30 s, 60 s), or do they
   fall with the sound in the first seconds?
2. Say "done" and leave the OP-1 connected: the agent reads `python firmware\tools\coop_usb.py stats` within two
   minutes: `peng` per 5-second window, and on the mode line `env_peak`, `vgain`, `ramp` (e14: the largest
   envelope value in the window, the voice sum gain, the decimator's ramp), with `env_skips`.
3. Write down the other three rings controls and the polyphony as the patch has them, the octave played, and whether
   the synth's LFO or the element is assigned to anything.

| what is seen | what it means | what follows |
|---|---|---|
| the screen's levels stay up, `env_peak` (or `vgain`, `ramp`) falls within seconds | rings plays the chord; the stock closes it afterwards | the cost can only be saved in the core (it knows the envelope); rings' early cut never sees it |
| the levels stay up, `env_peak`, `vgain` and `ramp` stay up too | the chord is in the output; the fade is in the listening (level, speaker) or the report | repeat on headphones at a fixed volume |
| the levels fall within seconds, `peng` stays flat | rings on the device decays where the host rings on, and its voices do not sleep | record the device's output (or dump a voice's `out` on the device) with this patch and compare with the host run of the same control words |

## e14: the core's stages at rest, the measuring code behind a switch, the stats text in RAM — device test (pending, 2026-11-21)

What changed (`docs/performance.md` "e14"): e13 with the efficiency audit's core fixes, each its own commit and
switch: (1) the tape record tap makes one stock call while nothing records; (2) the measuring code (cost table,
marks, per-voice records, block-start folds) is behind `g_coop_prof` / the policy token `prof=0|1`; (3) the S3 stage
and the mix loops are skipped on a silent instrument; (4) a voice that rendered silence is not placed or summed
(rings 0.11's idle voices); (5) the stats text is made in RAM and served over SysEx, and `coop_stats.txt` and the
tape tail are no longer written every 5 s. e13's placement policy, default and budgets are unchanged. Build tag
`e14`, the boot picture's line `coop e14 - 246`. Existing `.coop` files load and behave unchanged.

Two packages from one ELF (sha256 of each below), both in Downloads:
- **`op1_246_coop_e14.op1`, the release flavour**: the measuring code off (`g_coop_prof=0`), no periodic stats
  file. sha256 `93d0c275041b45b55a96dd1b3a75ef858106d66c8e4f14742fb55b6189abbcc7`.
- **`op1_246_coop_e14_dev.op1`, the dev flavour**: the measuring code on, no periodic stats file. sha256 `c5725f1cd9bc67e842767a23d0a4087d68cd44bcf6fbe23e35841b620946b7d9`.
Both: `tools/build_device_package.sh ... --tape-stereo --set-word g_coop_spread=32767 --set-word g_coop_statsfile=0`
(release adds `--set-word g_coop_prof=0`); `verify_bootloader.py` PASS (no te-boot.ldr), `pkg_check` = only
`OP1_vdk.ldr` differs; the ELF holds the tag `e14` (and no `e13` / `e12`). Either flavour becomes the other at the
next boot with a policy file: `prof=1` or `prof=0` in `/yaffs2/user/coop_place.txt` (e13's file; its other tokens
as before; `statsfile=1` brings the 5 s file writes back).
- No web delta made. The site needs no change for the stats path (it is answered from RAM under the same name). On
  a release image the `cost` / `engines` lines are empty and the windows' `eng` / `peng` / `fx` / `s2` are 0; a
  reader that shows them should say "profiling off" when the stats' `mode` line has `prof 0`.

**Gate notes (op1emu, 2026-11-21, one run at a time, fresh NANDs; images `--tape-stereo --set-word
g_coop_spread=32767 --set-word g_coop_force_stereo_out=1`; the 13 catalogue plugins of that day, rings 0.11 and
tape_echo 0.5 among them; scripts and outputs in `I:\op1emu-tmp\e14gate\`)** [C op1emu]. A scenario with several
key events is not sample-identical run to run on one image (e13 against itself on `det_tape_relift.txt`: max diff
4,124): audio identity is shown on the scenarios with one event time, with retries, and the multi-event scenarios
are covered by the check images, which compare every fast-path block with the real code in place.

| Run | Result |
|---|---|
| The check image (all three fast paths in check mode), pristine NAND, six scenarios | pass; `idle_bad 0` (31,624-146,170 blocks a scenario), `skip_bad 0` (81,569-441,453) |
| The check image, the 13 catalogue plugins, `det_fx_select` (waits x3) | pass; `skip_checked 335673 skip_bad 0`, `idle_checked 114330 idle_bad 0` |
| The check image, cs_hall + tape_echo, `det_fx_switch` | pass; `skip_checked 164624 skip_bad 0` |
| The check image, fickle, `det_fx_fickle` | `skip_checked 158845 skip_bad 0`; the run ended 1.9 s before its last event (the host was loaded: `ran_to_end` failed, nothing else) |
| The check image, rings 0.11, `det_eng_rings` | pass; `skip_bad 0`, `voice_checked 41610 voice_bad 0` |
| e14 against e13, pristine | `det_drum` and `det_tape_play` identical; `det_chord` max diff 2; `det_tape_rec`: aligned, max diff 2 over 3,707,181 frames (a scenario with several key events: another run of the same pair differed by 4,108, as e13 does against itself) |
| e14 with `g_coop_prof=0` against e13, pristine | `det_drum` identical, `det_chord` max diff 2 |
| e14 against e13, the 13 catalogue plugins, `det_fx_select` | differs (max diff 5,312): a scenario with many key events, not identical run to run on one image either; covered by the check image |
| Six rings 0.12 voices held 68 s (`rings_held6` after `det_eng_rings`) | cycles a block held and asleep: e13 76,405; e14 53,978; profile off 50,712; `voice_skips 273999` |
| A plain test engine, the stock envelope (one note; six keys held with the sustain down; the decay down too) | the stock stops rendering a voice when its envelope ends, key held or not; `env_skips 0` |
| The release image over USB/IP | `mode prof 0 statsfile 0`; two reads 8 s apart over SysEx, 10,361 and 12,171 bytes, 13 and 19 windows (the second made afresh: `ram_reads 162`), `sysex drop 0 errors 0`, `stats writes 0 tail_writes 0`, no `coop_stats.txt` on the NAND afterwards; with `prof=1 statsfile=1` in the policy file the same image reports `mode prof 1 statsfile 1`, `writes 7 tail_writes 7` and the file is on the NAND; the dev image `prof 1 statsfile 0`, `writes 0`. `tools/emu_gate.py` read its stats from the NAND in every other run here (gate images keep `statsfile=1`) |

Not shown in op1emu [I]: every device cycle figure (op1emu has no cache and no stall); the main loop's stall that
the stats write caused; the stats read over the real USB MIDI port from RAM (the path is the same code as in the
USB/IP gate).

**What to do** (the owner plays; the coordinator reads the stats **over USB MIDI**, `python
firmware/tools/coop_usb.py stats -o FILE`; nothing is copied in disk mode, and on e14 the stats file is not on the
OP-1's flash at all unless `statsfile=1`):
1. Flash **`op1_246_coop_e14_dev.op1`** first (keep the plugins and any policy file). Boot line `coop e14 - 246`.
   `coop_usb.py hello` -> `build e14`; `stats` -> `mode prof 1 statsfile 0 ... idle_skip 1 ... voice_skip 1`, and on
   the `tape` line `idle_mode 1`. The stats must read at once after boot and again any time later (they come from RAM).
2. **At rest** (nothing sounding, the tape stopped, a minute): read the stats. On the `win` lines `cb_sum / blocks`
   was 91-92 k on e11; expected clearly lower. `rec_sum / blocks` (the record tap; ~7.2 k before), `mix_sum /
   blocks`; `mode ... stage_skips` and `mix_skips` counting up; `tape ... idle_blocks` counting, `mismatch 0`.
3. **rings**, six-note chords held and re-struck for a minute, then **let a held chord ring out and keep the keys
   down** (rings 0.11 idles its voices): read the stats. `cb_sum / blocks` and `cb_max` in the playing windows
   (e11: 367-403 k, peaks to 660 k), `peng / blocks` (rings' own renders), `engines eng_rings` and
   `eng_rings_block`; `mode ... voice_skips` counting while the held chord is silent, and those windows' `cb_sum /
   blocks` falling towards the at-rest figure.
   **And the chord that "died down"**: rings at the longest damping, six keys held until it sounds dead, then read
   the stats while still holding: `mode ... env_skips N env_peak N vgain N ramp N` beside `peng`. `env_peak` near 0
   = the stock envelope closed the voices (then `env_skips` counting = the stock still renders under it, which
   op1emu says it does not); `env_peak` high with `vgain 24576 ramp 32767` = every gain between rings and the mix is
   open and the silence is in rings' own output or after the mix. Then release the keys and read again after the
   envelope's release time: `peng` must be 0 (the stock stops rendering a released voice when its envelope ends).
4. **A sampler sound with fickle at synth FX 1**, a minute: `cb_sum / blocks`, `fx / blocks`, `cost fx1_plugin`.
5. **Recording**: record a take on the tape while playing, play it back, lift and drop it: `rec_sum / blocks` while
   recording (unchanged from e13: ~20 k under a chord), `tape ... mismatch 0 mismatch2 0`, the take plays back in
   stereo as before; no click at the start or the end of the take.
6. Listen through all of it for anything e13 / e11 did not do: a first note after silence must start exactly as
   before (no lost or soft first block), effect tails must ring out to the end, the tape must loop seamlessly.
7. Then the release flavour **without a flash**: `coop_usb.py --write put place.txt --to
   /yaffs2/user/coop_place.txt` with `prof=0` in the file (plus whatever placement tokens are in use), power-cycle,
   repeat 2 and 3: `mode prof 0`, the `cost` / `engines` lines empty, `cb_sum / blocks` lower again by what the
   measuring cost (the audit: 5-6 k a block playing, more with the per-voice calls; 2-4 k at rest). Delete the token
   (or the file) to go back.
8. The figures the coordinator records, each for e14 dev and e14 with `prof=0`, beside e11's: at rest; rings chord
   (and rings held and silent); sampler + fickle; recording.

**If something is wrong:** the switches are image words, so a fault can be bisected with test packages; on the
device, power off; recovery: hold COM at power-on and flash `op1_246_coop_e13.op1`, `e11` or stock. The plugin
files and the policy file are untouched.

Result: (pending)

## Device loop, first runs: rings at the longest damping, held 60 s and released (2026-11-22, e13, by the agent)

The first scenarios played on the owner's OP-1 with nobody at the device: `tools/devloop.py` (commit 2b89187) put the
CRUN over SysEx, recorded the line out on UR44C inputs 5-6 through ASIO, ran the coop test runner and fetched the run
file. The sound was the owner's rings patch (string + reverb model, damping at its longest end, sustain at maximum),
rings 0.11, core e13 default placement. Run files: `docs/device-stats/2026-11-22-e13-devloop-*-run.txt`.

`tests/sim/dev_hold_chord.txt`, six keys held 60 s, then 20 s after release (whole callback per 5 s section, cycles a
block; line-out level RMS dBFS at the interface, whose floor at this gain is -69):

| section | cb_avg | line out RMS |
|---|---|---|
| rest | 90,818 | -69.2 (floor) |
| held 0-20 s | 350,468-351,323 | -40.0 to -41.8 |
| held 20-60 s | 322,312-323,065 | -40.1 to -42.0 |
| released 0-5 s | 331,684 | -47.0 |
| released 5-10 s | 331,939 | -57.2 |
| released 10-20 s | 331,694-332,014 | -66.4, -68.4 (floor) |

`tests/sim/dev_release_long.txt`, six keys held 10 s, then 100 s after release: held 352,387 (-40.4 dBFS); released
0-10 s 352-361 k while the level falls -41 -> -65; released 10-30 s 330 k with the line out at the floor; at 30-35 s
127,185 (the renders end inside that section); from 35 s on 90.6-91.0 k, rest.

[C] What this settles:
- The held chord does not fade: the line out holds -40 to -42 dBFS for the whole 60 s, so rings at the longest damping
  sustains, as the host said (the owner's "fades in about 4 s" was not reproduced with these settings).
- After release the OP-1 keeps rendering the six voices for about 30 s, the length of this patch's envelope release,
  while the line out is at the interface's floor (about 29 dB under the chord) from about 12 s: the last ~18 s of
  every release costs 240 k cycles a block (41 % of the budget) for sound below -69 dBFS at this gain. e14's
  `env_peak` reading would show the envelope's level through it.
- One voice set less at 20 s while held (351 k -> 322 k) is not explained by the run file (its engine lines are
  cumulative over the run on e13, not per section); e14's per-window `peng` will tell.
