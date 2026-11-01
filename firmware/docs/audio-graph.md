# The OP-1 R246 audio graph (as driven by the L1 callback)

> **SECOND CORRECTION (2026-11-01, op1emu write-traced; supersedes the probe-based note that stood
> here).** Both buses are real, in series: **`d34`/`db4` (`0xff800d34`/`0xff800db4`, int16 x64, planar)
> is the mixed master bus** (the mix loops write it, the stock master FX render is handed it), and the
> **stock master level/comp stage `0xffa04280` reads d34/db4 and writes the int32 output pair
> `e34`/`f34` (`0xff800e34`/`0xff800f34`, 64 x int32, planar; store loops at `0xffa044e0` L /
> `0xffa046f0` R)**, which the **SPORT TX conversion loop `0xffa0171c..0xffa0178e` turns into the codec
> DMA ring at `0xff800770` (descriptor list, 128 x 32-bit frames)** and the USB stream (`0x01915608`)
> reads too. The earlier "e34/f34 always zero" reading came from a probe at the callback entry, before
> the stage had run in that block. **Milestone B's image (`mbF`) replaced `0xffa04280` with the coop
> stage, so e34/f34 were never written and the codec output was silence** while d34/db4 looked right
> (the `--audio-dump` WAV showed 0 nonzero frames vs 603k for stock). The coop master stage now
> stands in for the trivial SDRAM getter the stock code calls just before (`CALL (P1)` at `0xffa017ac`,
> P1 = `0x019d90e8`: `R0 = 0xff80187c; RTS`; repointed with `l1_insert.py --imm-pair-site 0xffa017a4`),
> processes d34/db4 there, returns the getter's value, and leaves `0xffa04280` in place at its original
> SP (it uses a caller stack slot, which is why calling it through a trampoline hung). Item 11's full
> takeover of the level stage must reproduce the d34/db4 -> e34/f34 level/volume mapping (gain table at
> `0x0106a8f4`, state at `0xff801880`). The "Corrected model" below had e34/f34 right as the final
> pair; its "always zero" successor was wrong.

How the stock audio callback drives one audio block, reconstructed from the decrypted L1
(`l1_audio_callback` at `0xffa04804`) this session. This is the map Phase 2 works against: coop-fw
repoints the plain-SDRAM imm pair at `0x0191AB52` to its own callback and wraps or replaces these
stages. Tags: **[C]** confirmed from the bytes, **[I]** inferred.

## The key structural fact

The callback is in L1 (was encrypted), but **most of the graph is plain SDRAM** (`0x0193xxxx`â€“
`0x01a8xxxx`), which was never encrypted and is readable directly. Only the orchestration (the
callback itself) and the tightest inner kernels are in L1: the voice loop (`0xffa009a0`), the tape
resampler (`0xffa0619a`), the mix-loop meter ops (`0xffa0709c`/`0xffa0712c`), and two helpers the
callback calls directly (`0xffa036c4`, `0xffa017ec`). So Phase 2 does not have to re-implement the
whole core in L1; it can drive the existing SDRAM DSP from its own callback and replace only the
stages it must (the stereo buses, the master suite), keeping the hot kernels in L1.

## Corrected model (three RE passes, 2026-11-01) — source of truth

The entire instrument / mixer / FX / decimation / master DSP is in **L1**. The only plain-SDRAM
functions in the audio path are the USB-ring I/O and the EQ-coefficient builder. The audio graph:

1. **Voice loop `0xffa009a0` (L1):** renders 6 synth voices (engine `render` writes int16 at 88.2 kHz
   to `0xff80113c`), applies per-voice LFO + envelope + gain, and **sums to a mono 88.2 kHz bus at
   `0xff800bb4`** (128 x int16). Drums go through the same renderer via `0xffa005d4`.
2. **Master stage `0xffa00e50` (L1, recursive node renderer):** decimates the mono bus 88.2 -> 44.1
   (FIR, on the summed bus), runs the per-position FX (object/vtable dispatch: render = `*(*instance
   + 0x14)`), **pan-mixes each mono track into the stereo master pair**, runs the master EQ (biquad,
   continuation `0xffa04280`, writes `e34`/`f34` in place) + comp/drive, and **downmixes to the codec
   + meter** (the int16 read loop starts at `0xffa0143a`). Reached at `0xffa02116` / `0xffa02db2`.
3. **Master pair (final): `0xff800e34` (L) / `0xff800f34` (R), 64 x int32 (Q31), planar.** Sinks take
   the top 16 bits.
4. **Two sinks, both fed from the master signal computed in `0xffa00e50`:**
   - **Codec / speaker:** SPORT0 TX DMA buffer `0xff800770`/`0xff800970` (int16 interleaved, ping-pong;
     SPORT ISR `0xffa004fe`). The downmix inside `0xffa00e50` feeds this.
   - **USB:** `l1v_push_lr` (`0x019e1b3c`, SDRAM) reads `e34`/`f34` and interleaves into the USB ring
     `0x01aaeb60`. Called at the callback tail `0xffa04a04` with R0=`e34`, R1=`f34`.
5. Tape/track audio mixes in **before** the master (`0xffa036c4`, fed by tape streaming `0x01a8e340`),
   so the master pair is post-track-mix.

### What this means for the three milestones

- **The DSP is L1, so these are L1 modifications, not SDRAM trampolines.** We can do them: the L1
  decrypt/re-encrypt round-trip is byte-exact (`l1_decrypt.py encrypt-ldr`), and the emulator runs the
  plaintext LDR directly (patch L1 in the plaintext, no re-encrypt for testing).
- **Master bus suite (item 11):** insert a stereo comp -> drive -> EQ pass on the master pair
  `e34`/`f34` **inside `0xffa00e50`, just before the downmix read `0xffa0143a`** (or own the stage).
  This is the ONLY point before the codec/USB split, so it affects **both** outputs. **Do NOT trampoline
  the push `0x019e1b3c` — it is USB-only and would desync the speaker.**
- **Stereo instrument bus (item 2):** stock "stereo" is mono bus + pan into `e34`/`f34`. True stereo
  synths need coop's callback to carry stereo per-bus buffers (region A/B) and stereo FX, then sum to
  `e34`/`f34` — a larger reimplementation around `0xffa00e50`.
- **Two FX slots (item 3):** the FX render is the vtable dispatch inside `0xffa00e50`; a second stock FX
  is a second `render = *(*instance2 + 0x14)` call on the same bus buffer, with instance2 given its own
  state/scratch in region B (`0x02800000`). An L1 insert.

`0x01a88028` is NOT FX (it sets a 0..5 level indicator); the callback's three calls to it are a UI
level bucket gated ~every 20 blocks. The block table below is kept for call order only.

> **Earlier note (superseded by the model above):** `0x01a88028` is NOT the FX dispatcher (it is a level
> indicator setter; the 3 calls are if/elseif/else gated ~every 20 blocks). The real FX render is a
> vtable dispatch inside `0xffa00e50` (render = `*(*instance+0x14)`; master buffers L `0xff800d34`,
> R `0xff800db4`). The table below is kept for the call order but the `0x01a88028` "FX positions"
> reading is SUPERSEDED. See `symbols/sdram.sym`.

## Block sequence (top-level calls, in order) [C for the order and addresses]

| Callback site | Target | Region | Role (confirmed / inferred) |
|---|---|---|---|
| `0xffa04812` | `0x0193ee7c` | SDRAM | first stage: input / MIDI drain [I] |
| `0xffa04822` | `CYCLES` | â€” | block timing starts (under `CLI`) [C] |
| `0xffa04850` | `0x019e193c` | SDRAM | early stage [I] |
| `0xffa0487c` | `0x019e1a88` | SDRAM | `l1v_push_interleaved` (silent/muted block push) [C: kennel] |
| `0xffa048fc` | `0xffa036c4` | L1 | helper: sets up 4-wide state at `0xff801870` (tracks / FX positions?) [I] |
| `0xffa04914` | `0x01a11118` | SDRAM | stage [I] |
| `0xffa04920` | `0x01a10e58` | SDRAM | stage [I] |
| `0xffa04956`, `0x496e`, `0x497c` | `0x01a88028` (x3) | SDRAM | the same function three times: the three FX positions 0/1/2 (synth, drum, master) [I, strong] |
| `0xffa04988` | `0x01a8e340` | SDRAM | stage after the FX [I] |
| `0xffa049a0`, `0x49ae` | `0x019ae538` (x2) | SDRAM | paired stage (L/R?) [I] |
| `0xffa049cc` | `0x019ae914` | SDRAM | stage [I] |
| `0xffa049e0` | `0x019ae7cc` | SDRAM | stage [I] |
| `0xffa049e8` | `0xffa017ec` | L1 | helper: `CLI`, state at `0xff802618` (master chain?) [I] |
| `0xffa049fc` | `0x019e1b3c` | SDRAM | `l1v_push_lr`: push master L/R (+ tracks) to USB [C: kennel] |
| `0xffa04a04` | `0x019e2f24` | SDRAM | **MIDI-out drain** (calls `fw_midi_send`) [C] |
| `0xffa04a30`.. | `0x019c2f24` / `0x019c2f04` / `0x019c3044` | SDRAM | **input, not audio**: `KeyHandler::PushKey`, wake KeyHandler, `AddKnob` [C] |

The per-sample voice work (`l1_voice_loop` `0xffa009a0`, the pitch array `0xff801950`, the decimation
88.2 -> 44.1) runs inside the dispatch region around `0xffa04b90` [C: pitch-array reference there].

**Corrected reading (this session).** The audio DSP ends at the master push `l1v_push_lr`
(`0x019e1b3c`). Everything after is not audio: `0x019e2f24` drains MIDI to USB, the `0x019aexxxx`
calls scan the key banks, and the `0x019c2xxx`/`0x019c3044` calls queue keys/encoders and wake the
KeyHandler thread. `0x01a8e340` is tape chunk streaming (`fw_yaffs_pread`/`close`), not an audio
stage. So the audio ISR does: input/USB setup, voices + FX + master, push output, then scan and
queue the key matrix. See `symbols/sdram.sym`.

## What this means for the target coop-fw graph

The target graph (plan, Phase 2) stays the same, but the build order is clearer:

- The three FX-position calls (`0x01a88028` x3) are the hook for **two FX slots per position**: wrap
  that call so each position runs FX1 then FX2.
- `l1v_push_lr` (`0x019e1b3c`) is where the final master L/R leaves for the codec/USB: the **master
  bus suite (comp -> drive -> EQ)** goes just before it, fed by `l1_cb_sub_017ec` (`0xffa017ec`), the
  likely current master stage.
- `l1_cb_sub_036c4` (`0xffa036c4`), with its 4-wide state, is the prime suspect for the **tape / track
  mix** (four tracks) and so the place stereo tape tracks are summed; `0x01a8e340` (tape chunk
  streaming) feeds it from the tape file.

## Next

Label the SDRAM targets by disassembling them from the stock image (they are plain, so
`ldr_dump.py --elf` + `objdump`, no key needed), confirm the FX-position and master-chain guesses,
and record them in `symbols/l1.sym` (and a companion SDRAM symbol list). Then prototype the Phase 2
milestone-1 callback (identity wrap + measure) against op1emu.

## Stereo-bus insertion point (for milestone 2)

The voice loop `l1_voice_loop_body` (`0xffa009a0`, called from `0xffa02798`) renders each voice,
applies its LFO and envelope, and sums to a **mono** instrument bus; the FX positions
(`0x01a88028` x3) then process synth/drum/master. So coop-fw makes stereo **at and after the
instrument bus**: our callback carries the mono bus into stereo L/R and runs stereo FX, rather than
panning individual voices inside the L1 voice loop (that would mean replacing the loop). Per-voice pan
is a later feature that does touch the voice loop. This matches the plan: "stereo is made at the bus
and in stereo-by-design engines and FX."
