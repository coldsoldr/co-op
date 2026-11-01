# L1 map: the OP-1 R246 audio core (decrypted)

The OP-1's audio core lives in XTEA-encrypted L1 (45 CALLBACK blocks, 80,324 B). This document
records the key's provenance and verification, how to reproduce the decryption locally, and the
annotated map of the decrypted code. No key bytes and no decrypted Teenage Engineering code are
stored in this repository; everything below is reverse-engineering annotation (addresses and
behaviour), as in `te-op1-cfw/docs/`.

Tags: **[C]** confirmed from the decrypted bytes this session; **[I]** inferred.

## The key

- The XTEA key is OTP page `0xD0`, 128 bits, and is **the same on every OP-1** (TE ships one image;
  `te-op1-cfw/docs/app-boot-chain.md`). So a published reference dump is valid for any unit.
- Source used: the community reference OTP dump `TE-OP-1-BF524-OTP-dump.txt` in
  [Tolsi/op1dumps](https://github.com/Tolsi/op1dumps) (page `0xd0`). The key is also what
  op1emu's `otp.bin` and the [op-decryptor2](https://github.com/GosuBoy/op-decryptor2) tool consume.
- **Verified genuine, not trusted blindly.** `tools/l1_decrypt.py check-key` decrypts the five
  encrypted 8-byte units that kennel's `l1_patches.json` exposes (their stock plaintext is known) and
  they all match. All three plausible byte orderings of page `0xd0` converge on the same key words
  with unit word order little-endian. The reproducing arrangement for our tool is
  `--swap be --order 0123 --words "<"`.
- The key value is kept only in the local, gitignored scratchpad and in the user's own OTP dump, per
  the project's `NOTICE` (the OTP key is never committed).

## Reproduce the decryption (local, needs the key)

```sh
# 1. verify the key (page 0xd0 of your OTP dump, 32 hex chars) against kennel's known pairs
python tools/l1_decrypt.py check-key "<stock OP1_vdk.ldr>" third_party/kennel/synth_sdk/l1_patches.json <KEYHEX>
# -> MATCH ... --swap be --order 0123 --words "<"

# 2. decrypt every CALLBACK block to <out>/region_<addr>.bin (+ per-block files)
python tools/l1_decrypt.py decrypt "<stock OP1_vdk.ldr>" <KEYHEX> <out> --swap be --order 0123 --words "<"
```

Alternatives that reach the same plaintext: [op-decryptor2](https://github.com/GosuBoy/op-decryptor2)
(Rust; `.op1` + OTP dump -> decrypted `OP1_vdk.ldr`, verified byte-for-byte on 246) and
[sualk/op1-decryptor](https://github.com/sualk/op1-decryptor) (C; key + LDR). For op1emu, build the
NAND image from the stock `.op1` with [op1emu PR #76](https://github.com/op1emu/op1emu/pull/76)'s
`provision_nand.py` and supply the same OTP as `otp.bin`.

## Decrypted regions

| Region | Address | Size | Contents |
|---|---|---|---|
| L1 instruction SRAM | `0xffa00000` | 49,076 B | the audio core: crt0, event vectors, voice loop, decimator, ADSR, LFO dispatch, tape resampler/meters, master EQ/comp/drive, the audio callback |
| L1 data A | `0xff800000` | 12,212 B | CPLB tables, DMA descriptors, stacks, the live mixer/EQ/FX/pitch/gain words |
| L1 data B | `0xff900000` | 32,256 B | core data tables, the Lander samples (free with Lander off) |

Byte-entropy of the instruction region after decryption is **6.78 bits/byte** [C], the signature of
plain Blackfin code (ciphertext was 7.99), matching `app-boot-chain.md`.

## Validation (three independent predictions, all confirmed) [C]

- **crt0 at `0xffa00000`** sets `SP.L=0x3fb0; SP.H=0xff80` at `0xffa00054`, i.e. `SP=0xff803fb0`, the
  exact value `memory-map.md` predicted. The region starts with the event-vector loads, then crt0.
- **Audio DMA callback at `0xffa04804`** (kennel's `sm_audio_cb` target): a real function prologue
  (`LINK`, register push) that reads `R2 = CYCLES` early, as a timed audio ISR does.
- **LFO dispatch at `0xffa00ad4`** (the readable fragment in `lfo-interface.md`): loads the voice's
  LFO pointer, null-checks it, reads the `lfo_active` byte, and branches, as described.

## The audio core, as mapped so far

The symbol map is `symbols/l1.sym`. Confirmed this session from the decrypted code:

- **`l1_audio_callback` (`0xffa04804`)** is the audio DMA callback and whole-graph driver (kennel's
  `sm_audio_cb` target). It reads `CYCLES` at entry, masks interrupts (`CLI`), then runs the graph as
  a long series of indirect `CALL (P1)` dispatches (the per-voice renders, the FX, the master chain)
  plus two direct calls, `l1_cb_sub_036c4` and `l1_cb_sub_017ec`. It ends ~`0xffa05214`. **This is the
  Phase 2 seam**: coop-fw repoints the plain-SDRAM imm pair at `0x0191AB52` to its own callback, which
  can wrap or replace this.
- **`l1_voice_loop` (`0xffa009a0`)** is the Core voice loop (matches `engine-interface.md`), near the
  per-sample pitch-array setup (`d_pitch_inc` `0xff801950`).
- **`l1_lfo_dispatch` (`0xffa00ad4`)** loads each voice's LFO object, checks `lfo_active`, and calls
  its process method, as `lfo-interface.md` reconstructed.

The pitch array `0xff801950` is referenced from three sites: the voice-loop setup (~`0xffa00708`,
`0xffa00c32`) and inside the callback's render dispatch (`0xffa04b90`).

## Worklist: unidentified L1 functions

The decrypted region has 66 distinct direct call targets. Identified ones are in `symbols/l1.sym`;
the rest are the reverse-engineering worklist (name them as each is analysed). Notable clusters:
`0xffa06194`/`0xffa06248`/`0xffa06334` (around the tape resampler `0xffa0619a`), the `0xffa09xxx`
band (~40 functions: math/DSP helpers, the master chain and meters), and the early
`0xffa00102..0xffa00e50` group (boot/init and the voice/LFO machinery). The full list is in the
local disassembly (`scratchpad/.../l1.dis`), not committed.

## Status

Phase 1's hard gate is **met**: a verified key, the full L1 decrypted, three structural
confirmations, and the start of the symbol map (`symbols/l1.sym`) including the audio-callback seam.
The on-device dumper (`tools/dump_l1/`) is no longer needed for the gate; it remains only as an
optional device-vs-emulator cross-check. Next: follow the callback's dispatches to label the voice
render, decimator, FX and master-chain functions, and begin Phase 2 at `0xffa04804`, with op1emu as
the harness.
