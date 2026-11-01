# The OP-1 R246 audio graph (as driven by the L1 callback)

How the stock audio callback drives one audio block, reconstructed from the decrypted L1
(`l1_audio_callback` at `0xffa04804`) this session. This is the map Phase 2 works against: coop-fw
repoints the plain-SDRAM imm pair at `0x0191AB52` to its own callback and wraps or replaces these
stages. Tags: **[C]** confirmed from the bytes, **[I]** inferred.

## The key structural fact

The callback is in L1 (was encrypted), but **most of the graph is plain SDRAM** (`0x0193xxxx`–
`0x01a8xxxx`), which was never encrypted and is readable directly. Only the orchestration (the
callback itself) and the tightest inner kernels are in L1: the voice loop (`0xffa009a0`), the tape
resampler (`0xffa0619a`), the mix-loop meter ops (`0xffa0709c`/`0xffa0712c`), and two helpers the
callback calls directly (`0xffa036c4`, `0xffa017ec`). So Phase 2 does not have to re-implement the
whole core in L1; it can drive the existing SDRAM DSP from its own callback and replace only the
stages it must (the stereo buses, the master suite), keeping the hot kernels in L1.

## Block sequence (top-level calls, in order) [C for the order and addresses]

| Callback site | Target | Region | Role (confirmed / inferred) |
|---|---|---|---|
| `0xffa04812` | `0x0193ee7c` | SDRAM | first stage: input / MIDI drain [I] |
| `0xffa04822` | `CYCLES` | — | block timing starts (under `CLI`) [C] |
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
