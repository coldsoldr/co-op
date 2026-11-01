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
the audio path and is transparent. This is the seam; the measurement is stored for a later on-screen
readout.

### Build / inject / boot recipe

```sh
make -C core/sdram                                   # -> build/core/coop_cb.elf
python tools/inject_app.py "<stock OP1_vdk.ldr>" coop.ldr build/core/coop_cb.elf
python tools/l1_decrypt.py decrypt-ldr coop.ldr <KEYHEX> coop_plaintext.ldr \
    --patches third_party/kennel/synth_sdk/l1_patches.json
# boot in op1emu from its source root (see docs/emulator.md), screenshot to verify
```

## Next milestones (plan)

2. Own mixer graph with stock engines via an adapter; stereo instrument buses (start master L=R).
3. Stereo tape (Phase 3).
4. Master bus suite (comp -> drive -> EQ) before the master push `l1v_push_lr` (`0x019e1b3c`),
   fed by the current master stage `0xffa017ec`.
5. LFO x2.
6. FX x2: wrap the per-position FX call (`0x01a88028`, run 3x/block) to run FX1 then FX2.

A reader for the measurement words (`g_*`) is the next small step, so milestone 1's cycle figures can
be read on screen or via an op1emu memory probe.
