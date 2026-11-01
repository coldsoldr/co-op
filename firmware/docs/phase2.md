# Phase 2: audio-core takeover

coop-fw repoints the audio callback and replaces the graph stage by stage, each milestone built with
the bfin-elf toolchain, injected into the stock firmware, decrypted to a plaintext LDR, and booted in
op1emu (docs/emulator.md). The callback seam is the imm pair at `0x0191AB52` that builds the stock
callback pointer `0xffa04804` and stores it at `0xff80036c` (confirmed in the stock disassembly).

## Milestone 1 — identity wrap + measure  [VERIFIED 2026-11-01]

`core/sdram/coop_cb.cc`: a region-A payload whose `CoopAudioCb` (at `0x02c00020`) calls the stock
callback unchanged and times it with `CYCLES`, keeping per-block cost in region-A words
(`g_blocks 0x02c000a0`, `g_max 0x02c000a4`, `g_last 0x02c000a8`). `inject_app.py` repoints the imm
pair (`0x0191AB54` `4804`->low, `0x0191AB58` `ffa0`->high) to `CoopAudioCb` after checking the stock
bytes. **Result:** the coop image boots to the normal tape screen in op1emu — our callback sits in
the audio path and is transparent.

**Confirmed our callback actually executes (negative control).** A variant whose `CoopAudioCb` spins
instead of calling the stock callback hangs the firmware: op1emu times out waiting for frames and
writes no screenshot, while the identity wrap boots normally. So the imm patch is live and our code
runs every audio block. The measurement words are stored for a later on-screen readout (op1emu has no
headless guest-memory peek).

### Build / inject / boot recipe

```sh
make -C core/sdram                                   # -> build/core/coop_cb.elf
python tools/inject_app.py "<stock OP1_vdk.ldr>" coop.ldr build/core/coop_cb.elf
python tools/l1_decrypt.py decrypt-ldr coop.ldr <KEYHEX> coop_plaintext.ldr \
    --patches third_party/kennel/synth_sdk/l1_patches.json
# boot in op1emu from its source root (see docs/emulator.md), screenshot to verify
```

## Next milestones (revised after the 3-pass RE, 2026-11-01)

The audio DSP is all in L1 (`docs/audio-graph.md`), so these are L1 edits, not SDRAM trampolines.
That is fine: the L1 decrypt/re-encrypt round-trip is byte-exact (`l1_decrypt.py encrypt-ldr`), and the
emulator runs the plaintext LDR directly (patch L1 in the plaintext; re-encrypt only for the device).
Each milestone is measured against the 0.75-block ceiling (`docs/performance.md`).

**Recommended order (sequential, not parallel � all three touch the L1 master stage `0xffa00e50`):**

A. **Master bus suite (item 11) FIRST.** Insert a stereo comp -> drive -> EQ pass on the master pair
   `0xff800e34`/`0xff800f34` (64 x int32 Q31) inside `0xffa00e50`, just before the downmix read at
   `0xffa0143a`. This is the only point before the codec/USB split, so it affects both outputs. It is
   a bounded L1 insert, the DSP is fully host-testable, and it is self-contained. **Not** the push
   `0x019e1b3c` (USB-only). Proves the L1-modification dev loop.
B. **Two FX slots (item 3).** After slot 1's stock render inside `0xffa00e50`, call the render
   primitive again for a second stock effect (`render = *(*instance2 + 0x14)`) on the same bus, with
   instance2's own state/scratch in region B `0x02800000`. An L1 insert near the FX dispatch.
C. **Stereo instrument bus (item 2).** The larger one: coop carries stereo per-bus buffers and stereo
   FX, summing to `e34`/`f34`. Likely a partial takeover of `0xffa00e50`.

Then LFO x2 and stereo tape (Phase 3).

A reader for the measurement words (`g_*`) is still a useful small step (on-screen or op1emu probe).
