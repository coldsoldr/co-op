# Plan: where the effects' data sits (2026-11-08)

The owner: "optimise all the effects we can, they're all useful" and "will this work for plugins too". This is the
plan; nothing here is built yet. [C] = measured, [I] = inferred.

## 1. What we know

- **Device vs op1emu.** The p5a assembly rewrites saved 18–51 % of instructions in op1emu and 0–33 % on the device
  (`docs/performance.md` "p5d on the device"). The gap splits the effects in two [C device]:
  - *compute-bound*: the engines (1.2–1.7× op1emu), phone (−30 %), cwo (−33 %): fewer instructions paid;
  - *memory-bound*: spring, grid, delay at 2–2.8× op1emu, −3 to −14 % from rewrites that halved their instructions.
    Their per-sample loops stall on SDRAM.
- **Device cost per block today** (p5d, 580,499-cycle block): punch 71.5 k, cwo 62 k, nitro 44.8 k, spring 38.8 k,
  phone 30.2 k, grid 28.8 k, delay 23.2 k; the instrument slot's effect runs stereo (dual-mono for punch / phone /
  filter / nitro: the mono core twice; native stereo for delay / grid / spring / cwo).
- **The memory map that matters** (`docs/l1-map.md`, `core/sdram/l1_pool.cc`):
  - L1 instruction SRAM (48 KB) is full: the stock audio core (49,076 B). Coop code runs from SDRAM through the
    16 KB, 4-way I-cache. Hot code must stay small (the ~12 KB rule).
  - L1 data A (0xFF80xxxx) is the stock's working set (buses, pitch words, knob scratch, the decimator's buffers).
  - L1 data B: 32 KB; coop's pool is the Lander region, 0xFF900820..0xFF902900 (8.4 KB, **3.2 KB used**: the stereo
    stages' hot data) and the 16 KB sine copy at 0xFF902900 (T4c: it removed the FM lag).
  - The 16 KB data cache (write-back, write-allocate) serves everything else: a line miss is 100–190 cycles, more
    under display DMA.
- **The effects' data** [C in bytes]: delay line 0x2B14C B (176 KB) per object; grid tank 0x42040 B (264 KB: two
  260 × 260 halfword planes); spring tank 0xE3A0 B (58 KB: nine delay lines, the longest 0xDA2 samples); cwo's own
  stereo line; punch / phone / filter / nitro keep their state in the object (small). None of the big ones fits L1.
- **Access patterns** (the kernels' headers in `core/sdram/*_fast.S`):
  - *delay*: three 16.16 positions walking one line (read with a 7-tap FIR, a clear, a scattered 7-tap write): three
    sequential streams a block, plus the FIR table 0x0106A730 (rows of 64 B) and the L1 scratch 0xFF801B50.
  - *spring*: nine lines in one tank, each read at `w - d` and written at `w`: 18 sequential streams a block (the
    taps are fixed distances), plus four one-pole stages and nine indices in the object.
  - *grid*: two walkers over a 2D mesh: walker 1 injects at a moving cell and its mirror in the other plane; walker 2
    runs the wave equation at a cell and its mirror from the four neighbours in the other plane. Per sample ~8
    scattered halfword reads / writes across 264 KB, 88 frame accesses (state through the stack) in the stock loop.
  - *cwo*: a stereo delay line read at a moving fractional index (two taps, four in motion), filters and allpass
    chains in the object.
- **Code also misses.** Everything coop runs per block from SDRAM (the stages, the effect kernels, the plugin host)
  shares the 16 KB I-cache with the main loop, which evicts it between blocks [C: `docs/performance.md` T3g/T4].
  A 2 KB kernel re-fetched every block is ~64 line misses: 6–12 k cycles. This is part of the 2–2.8× too [I].

## 2. The levers, in order of value per risk

1. **Measure first, on the device, per effect** [1 day]. Add the Blackfin performance counters to the stats: the
   PFCNTR0/1 pair can count I-cache misses and data-cache misses (PFCTL events `DCACHE_MISS`, `ICACHE_MISS`) per
   block, read with the cycle counter around each effect render in the existing thunks. One stats line per effect:
   cycles, i-misses, d-misses. This tells, per effect, whether the stall is code or data, and sizes every gain below
   before it is built. op1emu cannot do this (no cache model). Risk: none (counters only).
2. **Prefetch in the kernels** [2 days, delay / spring / cwo]. Blackfin `PREFETCH [P]` pulls a line into the data
   cache without stalling. The streams are known a block ahead: delay's A / B / C positions, spring's nine `w` and
   `w - d`, cwo's index. Issue one prefetch per stream per 16 samples (one line of halfwords), two lines ahead. The
   write streams allocate on write anyway; prefetching them avoids the read-for-ownership stall. Expected: the
   per-stream first-touch misses hidden; on paper up to half of what these effects pay over op1emu [I], and nothing
   if the stall is elsewhere (that is what lever 1 says first). Plugins: the same instruction, the same rule.
3. **Block-process the stages, not the samples** [3–4 days, spring; grid partly]. Spring runs nine lines per sample;
   the stock order touches nine distant places 64 times each. Reordered as nine passes of 64 samples (line 1 for the
   block into an L1 scratch, then line 2 …) every pass is one sequential read + one sequential write, cache-perfect,
   and the per-line state stays in registers. The sound is bit-identical only where lines do not feed each other
   within the same sample; spring's lines feed a running sum (`r`) in order, so the pass order has to respect the
   data flow (lines that read the sum must run after the ones that add to it): a per-effect analysis, verified
   bit-exact against the stock loop in op1emu as the p5a rewrites were (lockstep builds, `tests/sim/kern_check.cc`).
   L1 scratch needed: 64 × 4 B per pass, a few hundred bytes from the pool.
4. **Put the small hot state in L1** [1 day, all]. Grid's 88 frame accesses a sample are the stock loop's; the
   coop kernel keeps most in registers already. What remains per effect: the object (tens of bytes, read every
   sample) and the FIR / coefficient tables (delay's 0x0106A730: 2 KB). Copy the table into the L1 pool at boot and
   point the kernel at it (as the sine table was: the biggest single device win so far). The pool has 5.2 KB free;
   the delay FIR table fits, the rest is per-object state.
5. **Grid: a cache-friendly mesh walk** [1 week, grid only]. The mesh is 264 KB and the walkers jump planes; a
   window cannot hold it. What can change is the layout: interleave the two planes cell by cell (cell (x, y) of both
   planes in one line) so the wave step's "other plane" neighbours sit in the same cache lines as the cell; and
   process walker 2 along a row with its three row-neighbours kept in registers (a classic stencil). This changes
   the tank layout the stock init writes (coop owns the tank already: `coop_tank_grid_*`), so the init must be
   coop's too. Big, grid only; do it last and only if lever 1 shows grid is data-bound (its loop is also the longest
   code: 353 instructions).
6. **Code residency** [2 days, all]. Keep every per-block coop code path contiguous and under the cache's reach:
   one section (`.text.hot`) for the stages + the active kernels, laid out together in region A so they map to
   distinct cache sets; and stop touching cold code from the block (the stats, the UI service, the SysEx poll all
   run on the main loop already). Measured by lever 1's I-miss counter. L1 instruction SRAM has no room, so this is
   the only code lever.
7. **Mid / side hosting of the dual-mono effects** [2 days, punch / phone / filter / nitro]: run the mono core once on
   the mid, pass the side dry. Halves the cost of the dual-mono types (punch 71 → ~36 k, nitro 45 → ~22 k, phone 30
   → 15 k [I: straight halving]); the wet signal is centred. Offered as a per-slot setting ("stereo / mono fx"),
   default stereo, so the owner picks the trade per sound. Not a cache lever but the largest single number on the
   list.

## 3. Does it apply to plugins?

Yes, with the same tools, and two additions:

- The plugin ABI gets a **memory lease**: a plugin declares in its contract how many bytes of hot data it wants in
  L1 (`guide/plugin-guidelines.md` §2.3 already proposes this, after kennel's `kn_l1.c`), the loader grants it from
  the pool (or refuses the plugin with "plugin memory full"), and the plugin's render reads its hot tables and
  state from there. That is lever 4 for plugins.
- The **guidelines** gain the rules of levers 2 and 3 (prefetch your streams; process a delay line in block
  passes; keep the per-sample state in registers; keep the render under the I-cache's reach), and the stats'
  per-slot cycle / miss line (lever 1) becomes the measurement every port must publish (§2.4 of the guide).
- The mono / mid-side option (lever 7) is free for plugins: a mono port is already hosted dual-mono by the host
  (`render(slot, l, r, …)` with per-channel state); the host can run it once on the mid instead when the slot says
  so.

## 4. Order of work and the gates

1. Lever 1 (the counters) first: one package, one device run of the p5 script → a table of cycles / i-miss / d-miss
   per effect. Everything after is chosen from that table.
2. Then, per effect in device-cost order where the counters say "data": delay (prefetch + FIR table in L1), spring
   (prefetch, then block passes), cwo (prefetch), grid (layout, last). Where they say "code": lever 6 first.
3. Lever 7 as its own package: it is a sound decision for the owner, not a measurement.

Every kernel change keeps the p5a method: bit-exact against the stock loop in op1emu (lockstep), the WAV gates, then
the device figure under the same script, judged by the device figure only.

## 5. Status

- 2026-11-08: lever 1 built (p6a, `docs/performance.md` "p6a"): the monitor is programmed at boot, every effect cost
  entry carries the two counters' averages (`cost` line `name=avg/max/n/p0/p1`, `g_coop_pf_mode` selects the events).
  Waiting for the device table. Nothing else started.
