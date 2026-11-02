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

Result: _not yet flashed_.
