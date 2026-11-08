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
