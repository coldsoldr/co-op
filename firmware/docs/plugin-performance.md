# Writing fast plugins (and fast coop code)

Every coop plugin, every coop stage and every rewrite of stock code follows these rules. They come from measuring the
OP-1 (R246, BF524 at 400 MHz) on the device and in op1emu, and from rewriting the stock DSynth voice loop, coop's S3
stage and the stock cwo effect in assembly (2026-11-04, `docs/performance.md`). The owner's rule: never compromise
performance.

## The budget

- One audio block is 64 samples at 44.1 kHz (voices render 128 at 88.2 kHz): **580,499 cycles**, interrupts included.
- The stock engines already take most of it: a six-voice DSynth chord is about 245 k cycles a block (42 %) after the
  coop rewrites, about 57 % before. The UI starts to lag when the audio stays above about half the block.
- What the stock effects cost per block in their stereo class (device, `coop_stats` cost line): cwo 60 k (92 k
  before its rewrite), punch 78 k, phone 49 k, spring 47 k, nitro 46 k, grid 36 k, delay 29 k. An effect slot is
  expected to stay well under 30 k; anything above 60 k is a bug to fix before release.
- Measure every plugin on the device: the stats file's `cost` line gives average / peak / calls per stage
  (`fx1_plugin=` for a plugin at slot 1, `s2_instr=` / `s2_master=` for slot 2). The emulator counts instructions,
  not stalls: device figures are typically 10-35 % higher than op1emu's for code with memory traffic.
- The `engines` line (p5a) gives the same for every synth and drum engine, per voice render, and `synth=` / `drum=`
  per block: a synth plugin's voice is measured against the stock engines it sits next to.
- Find the waste before rewriting: `tools/loopscan.py` lists every loop of a function and its callees with the CALLs
  and frame accesses inside. A CALL in a per-sample loop, or tens of frame accesses per pass, is the first target.

## Rules for the per-sample loop

1. **No calls inside the loop.** A CALL / LINK / UNLINK / JUMP (P) per sample costs 10-25 cycles plus the spills
   around it. The stock DSynth voice loop (four one-line accessor calls per sample) and the stock cwo loop (six helper
   calls) were the two outliers found; inlining them saved 19 % of the whole callback (DSynth) and 34-48 % of the
   effect (cwo). In C, mark helpers `static inline` and check the disassembly.
2. **A hardware loop** (`LSETUP` / LC0, LC1 for an outer loop), never a counted software loop with a back-branch.
   GCC does this at -O2 for simple loops; check.
3. **State in registers, not in memory.** Load the filter states and coefficients once before the loop and write
   them back once after. The compiled stock loops pass values through stack slots every sample.
4. **Hoist only what is really invariant.** Before moving a "constant" out of the loop, prove nothing in the loop
   writes it: a halfword load from the frame can be half of a word the loop stores. (The DSynth oscillator rewrite
   hoisted such a gain and came out 22 % quieter in one scenario; it was withdrawn.)
5. **Parallel issue.** A 32-bit DSP instruction can carry up to two loads (or a load and a store) in the same
   cycle: `A0 += R0.L * R1.L || R2 = [I0++] || R3.L = W[I1++];`. Only short-form loads can go in parallel (small
   offsets from P / FP, or I registers); long-offset `W[SP + big]` loads cannot. When both memory slots are used, one
   must go through an I register.
6. **Circular buffers in hardware.** A delay line or a FIR ring uses I / B / L registers (`L0 = length` in bytes,
   `B0 = base`); the address wraps for free. Set the L registers back to 0 before returning (the C ABI requires it).
7. **The dual MAC.** Stereo filters run both sides in one instruction: `A0 += R0.L * R5.L, A1 += R0.H * R5.L` with L
   and R packed in one 32-bit word (coop's S3 decimator: 32 taps x 2 sides = 32 cycles per output).
8. **The 32 x 32 product is six instructions** (the A1 / A0 FU / M sequence). Use 16 x 16 or 16 x 32 where the
   precision allows; keep the full sequence only where the sound depends on it.
9. **Data the loop touches every block in L1** (the coop L1 pool, `coop_l1.h`): an SDRAM cache-line miss costs about
   100 cycles (Opus measured 107 per 32-byte line), and SDRAM reads slow down further while the display is fed.
   Long delay lines stay in SDRAM; their per-block working set (the read / write heads) stays small.
10. **No allocation, no file access, no waiting in the audio thread.** A MALLOC from the audio interrupt hung op1emu
   (the heap lock). Allocate and construct on the main thread (coop_slot2_set pre-allocates for slot 2). A slow init
   (grid clears 264 KB, about 1.65 M cycles) belongs off the audio thread too.
11. **Fixed point only**: no float, no libc, no static constructors (the project's hard rules). Saturating ops
   (`(S)`) where the stock does, rounding as the stock does; when replacing stock code, every instruction keeps the
   stock options so the output is bit-identical.
12. **Work per block, not per sample**, for anything that changes slowly: coefficient updates, LFO steps that only
   need block rate, parameter smoothing to a per-block ramp.

## Rules for the structure

- **Process only what is there.** A stereo effect fed a signal with no width (L == R) can run its mono half once
  and copy: half the cost, the same sound (planned for the dual-mono stock types).
- **Do not run what is off.** An effect that is bypassed must cost nothing beyond a check.
- **One pass over a buffer** where possible (coop's S3 stage fused the DC blockers and the mid / side split into one
  loop: -42 %).
- **Size the code for the I-cache** (16 KB, 4 ways): keep the hot loop small and contiguous.

## How a rewrite is accepted

1. A bit-exactness test where one is possible: the GNU simulator (`tests/sim/kern_check.cc`, run with
   `tests/sim/run_kern_check.sh`) against the C reference over thousands of random blocks, with the state carried.
2. A switch word (`g_coop_fast_*`, default 1; 0 runs the old code): the A/B and the kill switch on the device.
3. `tests/sim/ab_switch.sh SWITCH SCENARIO [COST_INDEX]` in op1emu, in at least two scenarios, one with moving inputs:
   identical samples, or equal levels second by second with a whole-block offset (`tools/wav_levels.py`).
4. A device run with the stats file: the cost line before and after, and the owner's ears.

## For stock-code rewrites in particular

- Find every way into the loop (grep the disassembly for jumps into its range); patch the single entry.
- List what the code after the loop reads (registers and stack slots) and leave exactly that.
- Compute the frame (FP = SP + LINK size + pushed registers + SP adjust) and check every frame load against the
  loop's stores (rule 4).
- Code that must JUMP.L back into stock code goes in the `.near` region-C window (0x01E00400-0x01E01000, coop_cb.ld);
  region A is out of pcrel24 reach. Longer code goes in region A behind a `.near` stub (cwo does this).
- Keep the stock path reachable behind the switch (for a loop entered by LSETUP, set LT0 / LB0 / LC0 by hand).
- **Loops over sub-steps fuse into one chain.** phone ran three loops over the N new samples of each output sample
  (capture read into a ring, a high-pass over the ring, a low-pass over the ring); each stage reads only its own
  position, so one pass per new sample with the filter states in registers gives the same numbers in a third of the
  instructions (the ring's intermediate writes vanish). Check that no stage reads a neighbour before fusing.
- **A lockstep check when knob events make the A/B ambiguous.** A faster callback moves key and knob events by a
  block, and an effect with time-periodic state (phone's capture buffer closes every few thousand samples, silence or
  not) then sounds different by percents in the seconds after an event, with no fixed offset. The proof then is a
  test build that runs the stock code on a copy of the object and I/O block and the rewrite on the real ones, every
  block, and counts differences (`-DCOOP_PHONE_CHECK` in `plugin_host.cc`, with hit counters on the rare paths in
  the assembly); shared buffers both runs write with the same values are fine.
- A whole function reached only through a vtable needs no region-C stub: point the vtable entry at the rewrite (a
  data patch, any address) and jump to the untouched stock function for the switch-off and unusual cases (punch).
- **A constant stream in the third slot.** A loop that needs more per-block constants than registers can read them in
  order from a circular buffer: write them below the frame in the order the loop body reads them, `B3 = I3 = base`,
  `L3 = 4 * N`, and each `Rx = [I3++]` in a parallel slot gives the next one for free, every pass, with no reset (grid:
  27 words; rare branch paths read the frame instead so the count per pass stays fixed). The P slot is then free for
  the object, the state and the data. Restore L3 = 0 and the stack before returning.
- **Slot rules the assembler enforces** (binutils 2.43): an I-register modify (`I2 += M0`, `I2 -= 4`) goes only in the
  second slot, with an I-register load or NOP in the third; a P-based load or store goes in the second slot and the third
  must then be an I-register access; at most one store per packet. A register read by the DSP op may be loaded in the
  same packet (the stock does this), two writes to one register may not.
- **Reordering in-place I/O.** Reading an input before an output of the other channel is written is only safe when the
  two buffers differ; check `L != R` at the entry and take the stock path otherwise (grid, punch).
- When an A/B is not identical, run the stock image two or three times: op1emu's variation gives several stock
  outcomes, and the rewrite is right only if it matches one of them sample for sample. The level check is not
  enough: it weighs loud seconds, and punch's first version (a shift pair that kept extra bits) passed it while 1-2
  LSB of DC off in every quiet second. Work every bit-field trick through bit by bit.
- **Compute a shared lookup once.** nitro's second loop starts each sample with S(s) of its state and ends it with
  S(y_3) for the output, and the state is y_3: the same lookup twice. Look for a value a loop recomputes at the top
  from what it produced at the bottom.
- **Cheap table-lookup bookkeeping** (nitro, an interpolated 16-bit table read 6 times a sample): a reversed index
  `base - 2 * (v >>> 20)` is `(base - 2) + 2 * ~(v >>> 20)` (no subtract), the 4-bit fraction `(v.H & 0xf) << 11` is
  `v.H << 12 >> 1` on the half, and `a * f + b * (1 - f)` summed in one accumulator (`A0 = ...; Rn = (A0 += ...)`)
  equals the two saturated products added with saturation when neither product can saturate (only 0x8000 * 0x8000
  does). A 32-bit result from MAC0 goes to an even register, from MAC1 to an odd one.
- **Rotate a loop so its last store rides in the next pass's first packet** (`R4 = R0 >>> 20 || NOP || [I1++] = R2`):
  a packet may carry a P / FP load in the first slot and an I-register store in the second. Peel the first pass's part
  before the LSETUP. A sign-extended input sample shifted left can come through an I-register half load followed by an
  integer multiply by a power of two (`R2 = R2.L * R7.L (IS)`), which frees the P slot.
- **More delay lines than I registers: pointers in the frame, wraps between segments** (spring, nine lines, 18 read /
  write heads). The stock recomputed each head from an index every sample, with a compare for each wrap (19
  instructions a line). Keep the heads as addresses in frame slots, load / post-increment / store them in the parallel
  slots, and cut the block where the first head would reach its line's end: when every line is longer than the block,
  each head wraps at most once a block, so the cut costs one pass over the heads per segment (about 2 segments a
  block), and inside a segment there is no compare at all. 393 packets a sample became 162.
- **Stores rotated into the next pass's first packets must be done once more after the loop**, and skipped on the first
  pass: enter the loop body after them (set LT0 / LB0 / LC0 by hand, replay the skipped packets' other work, jump in;
  op1emu runs this). spring's first version flushed the outputs but forgot a pointer store riding there.
- **High-half loads and stores with post-increment**: there is no `W[P0++] = R0.H` or `R0.H = W[P0++]`; with a P
  register holding 2, `W[P0 ++ P5] = R0.H` and `R0.H = W[P1 ++ P5]` are 16-bit and go in a parallel slot. An I
  register parked on a halfword constant or a state word (`R1.H = W[I2]`) gives slot-3 loads that leave the P slot free.
- **A lockstep check for an effect with a large memory** (spring's tank, 58 KB): run the stock on a shadow object with a
  shadow copy of its memory, compare the whole memory after. When the memory is looked up from the object (coop's tank
  map), point the lookup's default at the shadow for the stock call.
- **Pair identical chains in the dual MAC, also in a mono engine** (Voltage, 2026-11-04): two waveshapers with the same
  instruction sequence on different inputs (f(|s4|) and f(|s3|); f(s2) and f(w)) run as one, the inputs in the two
  halves of one register: dual 16 x 16 multiplies (`R1.H = R2.H * R5.H, R1.L = R2.H * R5.L`), vector add / subtract
  with (S), one ABS (V); each half keeps the stock's operands and modes. Load the two inputs straight into the halves
  (`R5.H = W[P5]`, `R5.L = W[P2]`). 111 packets a sample became 64 with this and the stream / rotation tricks above.
- **The output store rotated without a peeled pass**: run the output pointer circular over the output block (B0 = out,
  L0 = its length, I0 = its last element); the first pass's rotated store writes the last element, the stores after
  wrap to the start, and the one store after the loop rewrites the last element. Set L0 = 0 before the stock code
  after the loop uses I0.
- **A simulator check against the stock instructions themselves** (`tests/sim/run_voltage_check.sh`): transcribe the
  stock function from the disassembly (in a temp directory, never into the repo), its data addresses as test arrays and
  the replaced LSETUP as the JUMP.L to the `.near` stub, and run it with the switch at 0 and 1 on random state. This
  tests the rewrite and the switch-off path before op1emu, in seconds. **bfin-elf-run gets one case wrong**: it
  commits a dsp32 ALU result (add, subtract, ABS) before a store in the same packet reads the register (multiplies and
  shifts it defers), so `R0 = ABS R3 (V) || W[P4 + 0x1c] = R0` stores the ABS result there and the old R0 on the hardware
  and in op1emu (which read every register of a packet first). Split such packets in the transcription, and in your own code never
  store a register in the packet whose ALU op writes it.
- **Assembler:** bfin gas does not substitute a macro parameter written directly before `]` (`[P4 + \off]` is a syntax
  error); write `[P4 + \off\()]`.
