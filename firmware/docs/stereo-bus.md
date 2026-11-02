# Stereo instrument bus (Phase 2 milestone C, item 2)

Goal: true stereo synths and drums — per-voice panning into a stereo bus — and the voice-spread control
(random per-voice pan scaled by an amount; see the `synth-voice-spread-feature` memory).

## The stock pipeline (op1emu write-traced, 2026-11-01/02) [C]

Dynamically confirmed with the op1emu write tracer (`docs/emulator.md`, `tests/sim/watch_writes.sh`)
on the stock R246 image, tracing every store into the voice bus, the render/envelope arrays, the
decimator output, the master pair and the output buses through a synth chord and a drum hit, then
reading the disassembly at the PCs that fired. Per audio block, in execution order:

1. **Clear the master pair.** `LSETUP(0xffa06a48, 0xffa06a4a) LC0=32`: 32 int32 stores each to
   d34 (`0xff800d34`, L) and db4 (`0xff800db4`, R).
2. **Set the decimator ramp step.** `0xffa030b8` writes `W[0xff80107e]` (0x40 in play).
3. **Clear the voice bus.** `0xffa01d1e` (`LSETUP` x64 int32): `0xff800bb4..0xff800cb3`, 128 x int16.
4. **Per-voice envelope, render, mono accumulate.** For each sounding voice (6 synth slots, R7 = index;
   12 drum slots, R7 counts 12 down to 1):
   - the SDRAM ADSR (`0x01912252`/`0x01912300`/`0x01912478`) writes the 128-entry envelope
     `0xff801d50`;
   - the engine render is called through its vtable: **synth** `CALL (P2)` at `0xffa00d28`,
     `P2 = *(vtable+0x14)`, args R0 = instance + `W[vtable+0x10]`, **R1 = `0xff80113c` (the 128-sample
     output)**, R2 = `0xff801724` (knob scratch); the default engine (`0x01a52040`) renders a scratch
     at `0xff801e50` first and then its output at `0xff80113c` (`0x01a52136..0x01a521d2`). **Drums**
     `CALL (P0)` at `0xffa00904`, `P0 = *(vtable+0x24)`, R0 = instance + `W[vtable+0x20]`, same R1/R2;
     the caller tests the drum render's R0 (0 marks the voice inactive, `0xffa00906`).
   - the accumulate: synth `LSETUP(0xffa00d4a, 0xffa00d62) LC0=127` + epilogue:
     `bus[i] = sat(bus[i] + (out[i] * env[i]) * gain)` with 16-bit fractional multiplies (default
     rounding), `gain = W[0x01b4ef6e]` (0x6000 at run time, P4 set at `0xffa00a5e`); stores at
     **`0xffa00d5a`** and **`0xffa00d70`**. Drums `LSETUP(0xffa00926, 0xffa0093e) LC0=128`:
     `bus[i] = sat(bus[i] + sat(2 * (out[i] * gain)))`, same gain word (P5, `0xffa00742`); store at
     **`0xffa0093e`**. Then the voice meter `R0 = MAX(R0,R1)(V)` at `0xffa00d7c` into `[P0+0x40]`.
5. **Half-band decimation 128 -> 64, in L1** (`0xffa005d4..0xffa00682`, outer loop LC1): reads the
   voice bus as 32-bit words (even sample .L, odd .H), keeps two 32-entry circular rings (`0xff80123c`
   even, `0xff80127c` odd, L2 = L3 = 0x40), and per output computes
   `acc = odd[16 back] * 0.5 + sum_k coef[k] * even[oldest + k]` with 32 `(W32)` MACs over the live tap
   table `0x0106a6f0`, then `out = (acc >> 16) * ramp`, `ramp = sat(ramp + step)`; the ramp word is
   `0xff80107c` (.L ramp, .H step; written back at `0xffa0067e`; rises from 0x1000 at boot and sits at
   0x7fff). Writes 64 int16 to **`0xff801e50`** (store `0xffa0066e`).
6. **DC blocker, in SDRAM** (`0x019674f8..0x01967612`; earlier labelled "the decimator"): reads the 64
   samples at `0xff801e50`, `y[n] = sat(sat(y[n-1] - X[n-1]) + X[n]) - c * Yq[n-1]` with `X = in << 16`,
   `Yq = out << 16`, `c` = the live word `0x01bab53c`, three state words at `0x01aaac14`, 32x32
   products as three 16-bit MACs; writes 64 int16 back over **`0xff800bb4..0xff800c32`** (stores
   `0x01967586`, `0x0196759a`, `0x019675d2`, `0x0196760e`).
7. **Mix into the master pair**, two 64-iteration loops back to back in the master stage:
   - **Loop A** `LSETUP(0xffa00f30, 0xffa00f86)`: an L gain `R0.H` and an R gain `R2.H` times `R6.L`,
     peak meters, accumulates into d34 (`0xffa00f76`) / db4 (`0xffa00f7e`). **Wrote zero in every
     block of every scenario**; the earlier static pass called this "the global pan".
   - **Loop B** `LSETUP(0xffa00fb2, 0xffa00fda)` at `0xffa00fae`: `v = (M[i] * gain) * ramp` with
     `gain = R0.L` (`W[0x01b4ef6e]` again), `ramp = W[P1]` stepped by `R2.L` each sample (saturating,
     written back), and adds **the same v to L and to R**: d34 at **`0xffa00fca`**, db4 at
     **`0xffa00fd2`**. This is the active path; no pan.
8. **SDRAM post-mix** `LSETUP(0x019de5ac, 0x019de5e8)`: adds one mono term to both channels in place
   (zero here: a silent source, metronome/click?) [I].
9. **Master-bus pass** writes L back at `0xffa033c0` then R at `0xffa03590` (level/meter stage; zeroes
   small values) [I on identity].
10. **Coop master stage** at the getter site `0xffa017ac`, then the **stock master level/comp
    `0xffa04280`**: processes L (`I2 = d34`, 64-loop at `0xffa04430`) and R (`I2 = db4`, `0xffa0462c`)
    separately into the int32 output pair **e34/f34** (stores `0xffa044e0` / `0xffa046f0`).
11. **SPORT TX conversion** (`0xffa013f2..`): the output-mode byte **`0x018b621d`** selects the path.
    **0 = speaker: the mono path `0xffa01698..0xffa017a0` sums `(e34 + f34) / 2`**, soft-limits it
    (0x7bb8 / -0x3472) and writes one word per 8-byte SPORT frame. Nonzero = headphones/line out: the
    stereo paths at `0xffa0142a` / `0xffa015f0`. op1emu leaves the byte at 0, so **every emulator
    WAV is mono by construction**; `g_coop_force_stereo_out` (stereo_bus.cc) sets the byte each block
    for emulator stereo checks only. (The writer of the byte on the device is the jack detection [I];
    see `docs/device-tests.md` for the on-device check.)

So the stock synth and drum output reaching the master is **mono: identical L and R** (loop B), and the
only pan machinery (loop A) is silent. The chain from the voice bus to the master is linear (FIR
decimator, first-order DC blocker, gains), which the coop design below exploits.

## coop design: the difference bus (implemented, C1)

The stock mono path is left running untouched (voice sums, decimator, DC blocker, insert FX, meters,
scope). For each panned voice coop adds only the **difference** between its stereo contribution and its
mono one, runs that through replicas of the stock decimator and DC blocker, and the replaced mix loop
adds it per channel:

```
for each rendered voice (hook right after the stock render, before the stock sum):
    v[i]   = the exact stock per-voice value       (synth: (out*env)*gain ; drums: sat(2*(out*gain)))
    dL[i] += v[i] * (gL - 1)                        (int32 difference buses, 128 @ 88.2 kHz)
    dR[i] += v[i] * (gR - 1)
at loop B (replaced):
    dL64 = DCblock(decimate(dL)),  dR64 = DCblock(decimate(dR))      (coop replicas, own state)
    L[i] = sat(L[i] + (sat(M[i] + dL64[i]) * gain) * ramp)            (M = the stock DC-blocked mono bus)
    R[i] = sat(R[i] + (sat(M[i] + dR64[i]) * gain) * ramp)
```

- **Pan law** (`coop_pan_deltas`, `core/include/coop_stereo.h`): equal power with unity at centre,
  `gL = sqrt2 cos(u pi/2)`, `gR = sqrt2 sin(u pi/2)`, `u = (pan+1)/2`; `gL^2 + gR^2 = 2` everywhere
  (the power of the stock L = R pair). The centre deltas are exactly 0.
- **Why the difference bus:** with every voice centred `dL = dR = 0` and the output is the stock
  output **bit for bit, insert FX included** — the regression gate holds by construction, independent
  of the replicas. The stock mono FX stays on the mid; a panned voice's placement bypasses the FX (as a
  mono send/return does). C3 moves the FX into the stereo chain (docs/fx-abi.md).
- **Replicas** (`coop_hb_decimate`, `coop_dc_block`, `coop_mix_b`): same taps (read live from
  `0x0106a6f0`), same ramp word, same DC coefficient (`0x01bab53c`), same 16-bit fractional ALU ops
  (`coop_fr16_*` are the Blackfin instructions on the device, biased-rounding models on the host), so a
  hard-panned voice cancels from the far channel to the LSB. 32-tap MAC loop in inline asm (hardware
  loop, circular I0, `(W32)` accumulate).
- **Spread** (C2 model, `coop_spread`): a voice that renders this block but did not render last block
  is a note-on and draws a fresh random pan scaled by `g_coop_spread` (Q15; 0 = stock mono, 32767 =
  full field). Synth and drum slots have separate tables and RNG streams.
- **Cost:** hooks skip centred voices entirely; at spread 0 the only work is the loop B replica.

## C1 takeover mechanism (implemented 2026-11-02) [C]

Three L1 sites are rewritten by `tools/l1_stereo_insert.py` (stock bytes verified first), three
trampolines (40 bytes) go into kennel's free L1 range `0xffa0bfb4`, and the SDRAM side is
`core/sdram/stereo_bus.cc` + `stereo_thunks.S` in the combined payload:

| site | stock | coop |
|---|---|---|
| synth render `0xffa00d22..0xffa00d2a` | `P2=[P0+0x14]; R3=W[P0+0x10](X); R0=R0+R3; CALL (P2)` | `R3=W[P0+0x10](X); R0=R0+R3; CALL tramp_synth` — tramp: `P2=[P0+0x14]; JUMP coop_synth_render_thunk` (stock render, then `coop_synth_voice_hook(R7)`) |
| drum render `0xffa008fc..0xffa00906` | `P0=[P1+0x24]; R1=W[P1+0x20](X); R0=R0+R1; CALL (P0)` | same shape via tramp_drum / `coop_drum_render_thunk` (preserves the render's R0 the caller tests) |
| mix loop B `0xffa00fae` | `LSETUP(0xffa00fb2,0xffa00fda) LC0=P0; <body>` | `CALL tramp_mix; JUMP.S 0xffa00fdc` — thunk saves the DAG/accumulator state, stores P1 (ramp state) and calls `coop_mix_entry(R0 gain, R1 ramp, R2 step)` |
| synth note-on, L1 `0xffa00e2e..0xffa00e36` (C2) | `P0=[P1+0x1c]; R2=W[P1+0x18](X); R0=R0+R2; CALL (P0)` | same shape via tramp_noteon / `coop_synth_noteon_thunk` → `coop_synth_note_on_hook(R7, 1)`. The voice loop's own dispatch; it did not fire for played keys in the emulator (kept: same code as Core::NoteOn, another path into it) |
| synth note-on, SDRAM `Core::NoteOn` `0x01967854` callers `0x01a8442c`, `0x01a848e4`, `0x01a8564e` (C2) | `CALL 0x01967854` | `CALL 0x01e00000` (a 16-byte trampoline in free region C: region A is beyond pcrel24 reach) → `coop_core_noteon_thunk` runs the stock function, then `coop_synth_note_on_hook(voice, result)`, returns the result. **This is the path a played key takes** (3 calls for the 3-note chord). |

The render hooks are as safe as the `CALL (Px)` they replace (everything live across a render is in
callee-saved registers or memory, and the C hooks honour the ABI; the thunks reserve the ABI's 12-byte
outgoing argument area). The mix thunk replaces inline code, so it also saves I0-I3/B0/L0/M0/A0/A1/LC1
around the C call; the stock code after loop B reloads everything else it uses.

Build: `make -C core/sdram combined`, then `tools/build_image.sh OUT.ldr [--set-word ...]` (inject ->
decrypt -> master imm -> stereo insert; `--device` re-encrypts). Image-time controls: `g_coop_spread`,
`g_coop_stereo_enable`, `g_coop_force_stereo_out` (emulator only), `g_coop_master_flags` (bit 0 demo
FX delay, bit 1 master suite; default 0 = transparent).

## Verification (op1emu, 2026-11-02)

Deterministic scenario `tests/sim/det_chord.txt` (guest-timed `at` events: synth at 30 s, a C-E-G chord
33-34 s) rendered with `tests/sim/hear.sh`, compared with `tools/wav_compare.py`:

- **Gate: coop image with `g_coop_spread=0` vs stock — identical, sample for sample, over the whole
  run** (3.17 M frames from 5 s; both mono, rms 63.2 / 63.2). [C]
- `g_coop_spread=32767` without the output-mode override: still mono (the speaker path sums L+R) and
  quieter (rms 61.3 vs 63.2), as expected for a mono sum of panned voices. [C]
- `g_coop_spread=32767` with `g_coop_force_stereo_out=1` (stereo SPORT path, patched dumper) vs the
  same image at spread 0: **real stereo, power conserved** — L/R rms 127.8 / 101.2 vs 115.3 / 115.3,
  side rms 49.4 vs 0.6 (width 0.22 vs 0.003), L²+R² within 0.1% of the centred pair. Three voices,
  random pans from the C2 model. [C] WAVs: `~/op1emu-work/c1_full_st.wav`, `c1_zero_st.wav` (copies in
  `C:\Users\byron\Downloads`). Repeated with the pans assigned by the real `Core::NoteOn` hook
  (`c2_full_st.wav`): 3 note-ons counted, side rms 49.2, width 0.219. [C]
- **Device packages:** `tools/build_device_package.sh` builds the `.op1` (re-encrypted L1 with the
  check-key arrangement, round trip `encrypt(decrypt(stock)) == stock` verified; `verify_bootloader`
  and `pkg_check` pass; only `OP1_vdk.ldr` differs from stock). Spreadfull flashed: PASS.

The earlier wall-time scripts (`cap.txt`, `hear.txt`) are not comparable run to run: the note onset
lands in a different guest block, and the aligned residue (max 85 LSB) is the stock engine's own
time-dependent modulation, not a coop difference.

## Status

- [C] Pipeline traced dynamically (above). [C] `coop_voice.h` + `coop_stereo.h` DSP, host tests
  (`tests/host/stereo_test.cc`), tool tests (`tests/tools/test_l1_stereo_insert.py`).
- [C] **C1 done in the emulator**: takeover wired, spread-0 image sample-identical to stock, full
  spread produces power-conserving stereo on the stereo output path. **Device: PASS** (2026-11-02, spreadfull image flashed by the owner,
  plays and sounds right; `docs/device-tests.md`).
- [C] Drums hooked with the same mechanism (hook + accumulate formula in place; a drum-mode stereo
  listen is still to do).
- [C] C2 model: synth voices draw their pan at the real note-on (`Core::NoteOn` hook, verified: 3
  note-ons for the 3-note chord), drum voices on their first rendered block (heuristic; the drum
  note-on dispatch is still to pin), both scaled by `g_coop_spread`. [ ] C2 UI: the spread control is an
  image-time word until the plugin UI runtime exists (Phase 4); co-op can set it over USB before then.
- [ ] C3 stereo engines/FX.

## Drums are pannable too (owner decision, 2026-11-01)

Per-voice panning applies to the **drum kit** as well: each drum voice (up to 12) gets its own pan +
spread, the same model as synth voices (`COOP_DRUM_VOICES = 12`). The drum render hook at
`0xffa008fc` feeds `coop_side_accum_drum` with the drum sum's formula.

## Stereo through the FX chain

The whole insert/master chain is stereo in / stereo out, with insert FX placed after the per-voice pan,
so per-voice placement and spread survive the effects. Mono effects are hosted dual-mono. See
docs/fx-abi.md for the full contract and the 3rd-party porting rules. Until C3, the stock insert FX
stays on the mid signal (see "coop design").

## Addendum (2026-11-02, T3): registers the code after a replaced loop still reads

The mix thunk replaces INLINE loops, and the stock code after loop B keeps using **P0** (loop B sets
`P0 = 0x40` for its LSETUP and the code after the loop reads P0 again) and after loop A **P0 = 0x01b4ef6e**
(the loop reloads it from `[FP-0x4]` every iteration). The C1 thunk never restored P0: the C function
happened to leave `0x40` in it (GCC used P0 as the count of its own 64-sample loops), so the image was
bit-exact by accident. When the C code grew (T1), P0 came back different and the tape played a block
late; a later edit put the coincidence back and looked like a fix. The thunk now sets P0 from
`g_mix_p0_out` (0x40 after loop B, 0x01b4ef6e after loop A) and R1 as the stock loops leave it (loop B:
the ramp sign-extended; loop A: .H = the final ramp, .L untouched). Rule for inline-loop replacements:
list every register the code after the loop reads before reloading it, and set each one explicitly.

Loop A (`0xffa00f2c..0xffa00f86`, the tape-page monitor and the "tape thru" path while the tape runs) is
replaced too since T3, through the same trampoline: the thunk tells the sites apart by the return address
the trampoline pushed, and loop A keeps its ramp/meter record at the stack slot `[SP+0x3c]` of the site
(P1 there is the loop count). Per sample: `L += (m*gL)*ramp`, `R += (m*gR)*ramp`, `meter = max(meter,
|m*level|)`, `ramp += step`, gL/gR = R0.H/R2.H, step = R0.L, level = W[0x01b4ef6e]; coop adds dL/dR to m.
Verified sample-identical to stock on the record scenario and the tape-page chord scenario.
