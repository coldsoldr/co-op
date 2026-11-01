# Phase 2: audio-core takeover

coop-fw repoints the audio callback and replaces the graph stage by stage, each milestone built with
the bfin-elf toolchain, injected into the stock firmware, decrypted to a plaintext LDR, and booted in
op1emu (docs/emulator.md). The callback seam is the imm pair at `0x0191AB52` that builds the stock
callback pointer `0xffa04804` and stores it at `0xff80036c` (confirmed in the stock disassembly).

## Milestone 1 â€” identity wrap + measure  [VERIFIED 2026-11-01]

`core/sdram/coop_cb.cc`: a region-A payload whose `CoopAudioCb` (at `0x02c00020`) calls the stock
callback unchanged and times it with `CYCLES`, keeping per-block cost in region-A words
(`g_blocks 0x02c000a0`, `g_max 0x02c000a4`, `g_last 0x02c000a8`). `inject_app.py` repoints the imm
pair (`0x0191AB54` `4804`->low, `0x0191AB58` `ffa0`->high) to `CoopAudioCb` after checking the stock
bytes. **Result:** the coop image boots to the normal tape screen in op1emu â€” our callback sits in
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

**Recommended order (sequential, not parallel — all three touch the L1 master stage `0xffa00e50`):**

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

## Master-suite integration: status and the next step

The master-suite DSP is built and host-tested (`core/include/coop_master.h`, `tests/host/master_test.cc`):
compressor -> drive -> biquad EQ on the stereo master pair (int32 Q31), `coop_master_process`.

The remaining work is *where* to call it inside the L1 master stage `0xffa00e50`. Static analysis
located the buffers (`e34`/`f34`) and the downmix read (`0xffa0143a`), but `0xffa00e50` is a
hand-optimized, recursive, 3-op-bundled function with many live registers mid-flow; a blind
mid-function trampoline is high-risk, and the exact point where `e34`/`f34` are final yet unread by
both sinks is not clear statically.

**Next step (serves all three milestones): trace it in op1emu.** Build op1emu with
`-DENABLE_PROFILING=ON` and use `--profile-trace` / `--profile-probes` (deterministic) to record the
read/write order of `0xff800e34`/`0xff800f34` across a block. That pins the single instruction after
which the master pair is final and before both the codec downmix and the USB push read it, giving a
safe CALL-boundary insertion (ideally retarget the stock EQ call at `0xffa017ae`, or a clean point the
trace reveals). The same tracing approach locates the FX-dispatch and pan-mix points for milestones B
and C. The emulator makes these L1 experiments safe (a wrong insert garbles/crashes the emulator, not
a device).


### Refined integration design (lower risk than the mid-function insert)

Rather than a blind trampoline at the bundled `0xffa0143a`, the cleaner candidate is to **retarget the
stock master-EQ CALL at `0xffa017ae`** (which calls `0xffa04280`, the EQ that writes `e34`/`f34`) to
coop's suite. That is a single call boundary: our function reads `e34`/`f34`, runs comp -> drive -> EQ,
writes them back, replacing the stock EQ. Downstream (codec downmix, USB push) then read the processed
pair. Because L1 cannot reach SDRAM by a direct CALL, this uses a tiny L1 trampoline in free L1
(`0xffa0bfb4`, the range kennel's `.l1tramp` uses): the L1 CALL retargets to the trampoline (pcrel24
within L1), which saves RETS, indirect-CALLs `coop_master_apply` in SDRAM, restores RETS, returns.

The profiling build + trace confirms the execution order (EQ before both sinks) before we commit to
this point. The payload `core/sdram/master_suite.cc` (`coop_master_apply`, transparent by default,
liveness counter `g_master_blocks`) is built and ready to wire.

Perf note: the compressor's per-sample divide is skipped at the transparent default; replace it with a
reciprocal table before enabling compression, and measure against the 0.75-block ceiling.

## Milestone A (master suite) — INTEGRATION VERIFIED in op1emu (2026-11-01)

The master suite now runs inside the L1 audio DSP. Chain:
1. `inject_app.py` adds the SDRAM payload (`coop_master_apply` at `0x02c00020`).
2. `l1_decrypt.py decrypt-ldr` -> plaintext LDR.
3. `l1_insert.py` adds a 16-byte trampoline in free L1 `0xffa0bfb4` (`core/patches/l1_tramp.S`) and
   retargets the stock master-EQ CALL at `0xffa017ae` (was `0xffa04280`) to it; the trampoline
   indirect-calls `coop_master_apply` in SDRAM (L1 cannot reach SDRAM by pcrel24), preserving RETS.
4. op1emu boots it to the normal tape screen: our suite runs in the audio path, transparent by default.

Proof it executes: a first build with the trampoline immediate at the wrong byte offset crashed with
EXCAUSE 0x21 (undefined instruction) at `0xffa0bfba` *inside our trampoline* — i.e. the retargeted
call reaches our code; fixing the offsets makes it boot. The stock-byte check in `l1_insert.py` also
confirmed `0xffa017ae` is the CALL to `0xffa04280`.

For the device: `l1_decrypt.py encrypt-ldr` re-encrypts the modified L1 (byte-exact round trip).

**Next:** enable real comp/drive/EQ params (replace the compressor's per-sample divide with a
reciprocal table first), and measure cycles/block with the profiling op1emu build (`--profile-probes`
to read `g_master_blocks`/the cycle counter) against the 0.75-block ceiling.


## Milestone B status (2026-11-01): second FX slot DONE in the emulator; master bus corrected

Two findings while wiring milestone B, both from op1emu probing:

1. **The master bus is `d34`/`db4` (int16), not `e34`/`f34`.** `0xff800e34`/`f34` are always zero in
   the speaker configuration (that is the USB-audio bus); the live codec master is
   `0xff800d34`/`0xff800db4`, int16, 64 samples. Milestone A was inserting on `e34`/`f34` and so
   processing **silence** -- its transparent boot only proved "no crash." The master suite + FX now
   operate on d34/db4 (widen int16 -> Q31, narrow back).
2. **`0xffa04280` is the master compressor/level DSP, not the EQ.** It has its own frame and works on
   `0xff801880` comp state. Our insert at `0xffa017ae` replaces it, which is exactly item 11 (the coop
   master suite takes over comp/drive/EQ). (`l1_insert.py` augment mode -- call stock then coop -- was
   tried and dropped: it hangs here because `0xffa04280` is sensitive to the caller stack frame, and
   replace is the correct design for this stage anyway.)

Milestone B itself: a coop stereo feedback delay (`core/include/coop_fx.h`) is the second master
insert slot, in series before the suite, delay line in region A `0x02d00000`. Validated in op1emu:
the delay line stays zero at idle and fills during a chord (reads live audio); a mute build confirms
the coop insert owns the master-bus content. Cost: FX ~1.9%, suite ~3.3%, whole coop callback 16.0%
under a chord (docs/performance.md). Final audible confirmation is a device test.

**Still open for a full milestone B (two STOCK FX):** running a *second stock* FX via its vtable
(`render = *(instance2 + 0x14)`, as the stock master FX render at `0xffa017e6` does with d34/db4)
needs instantiating a second stock FX object -- deferred; the coop FX slot above proves the mechanism
and is the plugin-system path.
