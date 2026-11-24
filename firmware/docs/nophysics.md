# nophysics: TE's Box2D out of coop (e16, 2026-11-22)

coop removes TE's Box2D physics library from the image, together with its only two users: the **Tombola**
sequencer and the hidden **Lander** game. The owner approved losing Tombola. The edits are the `nophysics` feature
of **op1-sdk** by **BertBrandstifter** (<https://github.com/BertBrandstifter/op1-sdk>, MIT, commit `cc62d81`),
vendored unchanged in `third_party/op1-sdk/nophysics/` (its `feature.json`, `stub.svg`, `README.md` and `LICENSE`).
Thanks to its author: the analysis, the patch table and the reachability argument are theirs. coop's part is
applying it in coop's pipeline, re-running the proof against coop's own image (where coop's code, its L1 inserts and
the whole decrypted L1 are part of the argument), the saved-state check and the measurements below.

Same stock: op1-sdk's table is checked against `op1_246.op1` sha256 `c5315218...`, which is the stock coop builds
from (verified, 2026-11-22) [C].

## What changes on the OP-1

- **The sequencer picker:** Tombola still shows its icon, but choosing it starts the **pattern** sequencer.
- **A saved Tombola setting** (`op1.db`, table `sequencer`, `synth_sequencer` / `drum_sequencer` = `tombola`) boots
  as pattern; the next time a sequencer is picked, TE writes that one into the setting. The three `tombola*` knob rows
  in `op1.db` (`knobs`: gravity, restitution, angle) stay and are never read.
- **Lander:** already unreachable in coop (T4d, `core/sdram/lander_off.cc`: the four-mode-key combo opens coop's
  settings menu). nophysics also makes the stock's own combo compare never match.
- **Boot:** no Box2D world is built (TE built two at every boot, Tombola's and Lander's, used or not); Box2D's 30 static
  initialisers return at once. The device package also replaces `content/display/lander.svg` (168,658 bytes) and
  `tombola.svg` (17,685) with op1-sdk's empty 303-byte SVG, so neither is parsed at boot.
- **Code:** 144,888 bytes of TE code become `RTS` (table below). Not reused yet.
- **The package:** 89,202 bytes smaller than e15 dev (12,082,114 -> 11,992,912; LZMA packs the RTS runs and the stubs).

## How it is applied

- `tools/nophysics.py IN.ldr OUT.ldr` reads `third_party/op1-sdk/nophysics/feature.json` and, **before writing
  anything**, checks every one of the 57 sites: the 40 patches against their stock bytes, the 17 blanked ranges
  against the sha256 of their stock bytes. Then it writes the edits and checks them again (`--check` / `--verify`
  report either state). Only plain SDRAM blocks are touched; one FILL block inside Box2D (`0x0193252c`, 32 bytes of a
  repeated instruction) gets the RTS word as its fill value. Every block keeps its size and file offset.
- `tools/build_image.sh` runs it right after `l1_stereo_insert.py`, i.e. after every other SDRAM patch of coop's, so
  its stock checks also prove that no coop patch lands on anything it changes [C: 57/57 sites stock in the coop image,
  2026-11-22]. `NOPHYSICS=0` builds the image without it (the A/B of the gates).
- `tools/build_device_package.sh` copies the stub SVG over the two content files (`NOPHYSICS_SVG=0` keeps TE's).
- The 36 code pointers that led into removed code (30 ctor-table entries `0x01000030..0x010000a4`, the Lander
  contact listener `0x019106a0`, the Lander screen's draw / SVG-loaded slots `0x019106d8` / `0x019106e0`, the Tombola
  knob handler `0x01911d10`, the Tombola screen's draw / SVG-loaded slots `0x01911ee0` / `0x01911ee8`) now point at
  `0x0191fe8c`, the first RTS of the blanked Box2D range. The other four patches: SeqIds `"tombola"` -> id 0
  (`0x018fa640`), the player factory's case 2 -> case 0 (switch table `0x018f2f00`), a `JUMP.S` over the Tombola
  physics setup in the sequencer-settings init (`0x01a63fda` -> `0x01a64008`), the stock Lander combo compare
  (`0x019b66aa`, mask `0xa0` -> `0x100`).
- `tests/tools/test_nophysics.py`: the tool on a synthetic LDR (blank across blocks and a FILL block, refusal of a
  changed byte and of a second application, the real table's counts).

## The Lander screen and coop's L1 pool

- **No boot hang reintroduced.** T4d found that kennel's *stub screen object* for Lander hung the boot in op1emu, so
  coop keeps TE creating the Lander screen as stock does. nophysics does the same: the screen objects of Lander and
  Tombola are still constructed (their constructors and the getter `0x019c4ecc` are not blanked; `b2WorldCallbacks.cpp`
  `0x0193bff0..0x0193c12c` stays because TombolaScreen's boot initialiser calls one of its constructors); only their
  draw and SVG-loaded slots return at once. coop's own Lander sites (`l1_stereo_insert.py` `LANDER_GETTER`,
  `LANDER_SITES`, the key dispatch `KEYEV_SITE` `0x019c2d3e`) are outside every blanked range [C]. Booted in op1emu with
  and without the stub SVGs: "Gates" below.
- **The L1 pool keeps the Lander's sound buffers.** `l1_pool.cc` takes the 40 Lander sound buffers in L1 data B
  only when their table (`0xff906908` / `0xff9069a8`) has the stock layout. That table is loaded data of the image
  (it holds `0xff90081c, 0xff900a94, ...` / `0x277, 0x277, ...` in the nophysics image [C]), not written by Lander
  code, and nophysics changes no L1 data byte; every gate run reports `l1 state 1` (the pool taken).

## Why nothing reaches the removed code: `tools/nophysics_prove.py`

op1-sdk's `prove.py`, ported to coop's tools and run against **coop's image** (not stock): the plaintext gate image
built with `NOPHYSICS=0`, disassembled with `bfin-elf-objdump` into a temporary directory (TE code: never kept).

```sh
python3 -I tools/nophysics_prove.py IMAGE.ldr [--elf build/core/coop_combined.elf]
```

- **Code scanned for references** (CALL / JUMP targets, and `X.H` + `X.L` constants of one register within 10
  instructions): TE's SDRAM code `0x0191209e..0x01aa8000` as coop leaves it (595,905 instructions), the **whole L1
  instruction SRAM decrypted** (stock audio core plus coop's L1 inserts, 18,325), coop's region-C trampolines and
  `.near` thunks (461) and coop's region-A `.text` (60,409).
- **Data scanned:** every 32-bit word at every halfword offset of every loaded byte outside L1 instruction SRAM:
  TE's data, L1 data A / B, coop's `.rodata` / `.data` (6,884 words that fall in TE's code range).
- **Fixpoint:** Box2D and the Lander / Tombola regions are cut into chunks where nothing can fall through; a chunk
  is live once anything outside the dead set refers to any byte of it; repeat until stable. Every blanked range must
  lie in dead chunks, and no coop or L1 code may refer to a blanked address at all.
- **Result [C, 2026-11-22]: PASS.** 17 ranges, 144,888 bytes blanked; 144,998 bytes of dead code found (the
  difference is the tails of live functions at the region edges, which stay).

Differences from op1-sdk's run, each checked by hand:
- op1-sdk's L1 dump had 8-byte blocks it could not decrypt, which hid the low half of 10 L1 calls (its
  `WINDOWS_CHECKED`, argued from neighbouring calls). coop's L1 is decrypted whole: **every L1 constant is resolved**
  and no window is left unexplained.
- op1-sdk's pairing (every `X.L` within 10 instructions of an `X.H`) makes one false reference in the now-visible L1:
  at `0xffa01bc6` / `0xffa01bd0`, `P1.H = 0x19c; CALL (P1)` is followed by `P1.L = 0x4c40; P1.H = 0x1de` (the
  constant `0x01de4c40`, outside), which the loose rule also pairs as `0x019c4c40` (inside a blanked Lander range). The
  port drops a pairing only in exactly that shape: an `X.L` that is followed by the next `X.H` of the register
  before anything reads it (the value never exists). `--loose-pairs` runs op1-sdk's rule and shows this as the only
  failure.
- One coop table looks like a pointer: `coop_bend.cc`'s `uint16_t ms_of[129]` (ramp times), where `0x0180, 0x0192`
  side by side read as `0x01920180`. Exempted by symbol (`COOP_NUMBER_TABLES`), so a rebuild that moves it keeps the
  exemption and anything else of coop's that looks like a pointer fails the proof.
- op1-sdk's other assumptions are kept as they are (its `NOT_POINTERS`, Box2D's own tables, which the port re-checks
  are loaded only from Box2D code, and TE's exit-time destructor records, run only by `exit()`). A stray call into a
  blanked range would return at once (RTS), with the stack and the callee-saved registers as they were.

**Re-run it whenever coop adds a patch or calls into TE code near these ranges** (`tools/build_image.sh` already
refuses an image where a coop patch lands on a site, but only the proof covers coop code that *calls* there).

## Code space freed (not used yet)

coop cannot place code in TE's region yet (its code is in region A / C). For later:

| Range | Bytes | What |
|---|---|---|
| `0x0191fe8c`..`0x0193bff0` | 115,044 | Box2D (b2BlockAllocator .. b2World). **Keep the first halfword (`0x0191fe8c`) an RTS**: the 36 pointers lead there. Usable: `0x0191fe8e`..`0x0193bff0`, 115,042 bytes, one block, within pcrel24 reach of all of TE's code |
| `0x019c38a8`..`0x019c926c`, 5 ranges | 22,224 | Lander (`0x019c4ff0`..`0x019c926c` is 17,020 of it) |
| `0x01a63e50`..`0x01a66780`, 11 ranges | 7,620 | Tombola (largest `0x01a648f4`..`0x01a65730`, 3,644) |
| total | 144,888 | |

Using any of it needs: the LDR block layout there (one FILL block at `0x0193252c`), the I-cache CPLB for TE's code
pages (already code, cached), and nophysics applied first. The full per-range table is `feature.json`'s `blank`.

## Measurements (op1emu, 2026-11-22) [C op1emu]

Images: the gate flags (`--tape-stereo`, spread 32767, forced stereo out), core ELF of dev `571ad17` (e15 + CS1, whose
headers the core does not include: the same image bytes as main's `cd7b846`), built twice, `NOPHYSICS=0` ("base") and
with nophysics ("np"); "np + stub" is the np image on a NAND whose `lander.svg` / `tombola.svg` are the stub. Scenario
`det_chord`; heap 0 is the stats' `plugins ... heap_largest / heap_free` at the end of the run (after boot, with the
8-lane stereo tape cache placed and a chord played). Boot milestones from op1emu's write tracer (`--watch-writes`, guest
time): `g_plugins_booted` (coop's boot hook, after the mount), `g_blocks` (coop's first audio block), and the GUI's
first store into the framebuffer (`0x03000000`, writer `0x0193e848`): the first frame drawn.

**Heap 0 free after boot: +324,336 bytes (316.7 KiB), every time.**

| NAND | base: free / largest | np: free / largest | gain free / largest |
|---|---|---|---|
| fresh from the package, first boot | 2,729,524 / 2,726,044 | 3,053,860 / 2,986,556 | **+324,336** / +260,512 |
| the same NAND, second boot | 3,055,476 / 3,053,204 | 3,379,812 / 3,354,836 | **+324,336** / +301,632 |
| the gate's pristine NAND | 2,729,484 / 2,729,052 | 3,053,820 / 3,024,004 | **+324,336** / +294,952 |

- The stub SVGs change nothing in heap 0 (np + stub: the np figures to the byte): TE frees what the parse takes.
- The gain is the two Box2D worlds and what TE built into them at boot (Tombola's physics bodies, Lander's terrain
  from its SVG), which stayed allocated for the whole session. It is about 0.6 of one 512 KiB tape cache node, or a
  fifth of the 1,536 KiB plugin budget (`docs/plugins.md`), and lands where the stock and the plugins compete.
- `l1 state 1` in every run: coop's L1 pool still takes the Lander's sound buffers.

**Boot: the GUI's first frame 13-87 ms sooner; nothing earlier moves.**

| NAND | base | np | np + stub |
|---|---|---|---|
| fresh, first boot | 4.5716 s | 4.5334 s (-38 ms) | 4.4847 s (**-87 ms**) |
| second boot | 2.6000 s | 2.5834 s (-17 ms) | 2.5236 s (**-76 ms**) |
| pristine | 3.0167 s | 3.0037 s (-13 ms) | - |

coop's boot hook (0.68 / 1.10 / 2.64 s) and its first audio block (1.75 / 2.17 / 3.71 s) are identical in every pair:
the worlds and the SVGs are built after them, on the way to the first screen. Most of the gain is the stub SVGs (the
168,658-byte `lander.svg` no longer parsed), so a web-built e16 (stock content) gets the smaller share. op1emu's
guest time is not the device's (NAND reads and the parse run at the emulator's speed); the device test times it.

## Gates

op1emu, one run at a time under the shared lock, fresh scratch NANDs deleted by the gate after every run; outputs
(logs, stats, screenshots) in `I:\op1emu-tmp\np\`. [C op1emu]

| Run | Result |
|---|---|
| np against base, pristine, `det_chord` | aligned, max diff 1 (the onset lands 13 ms sooner with np; `det_chord`'s known spread, e15 notes: max diff 2) |
| np against base, pristine, `det_drum` | **identical** (max diff 0) |
| np against base, pristine, `det_tape_play` | **identical** (max diff 0) |
| np, `det_lander_combo` (the four mode keys twice, then a chord) | pass, `lander_swallowed 2`, no fault, sound |
| np, `det_ui_settings` (the combo opens coop's settings menu, a change saved) | pass: `set_opens 1 set_saves 1`, the menu drawn (screenshots) |
| np, `det_chord` on fresh / second-boot / pristine NANDs, with and without the stub SVGs | pass, no fault, `l1 state 1` (the table above) |
| base, `det_np_pick.txt` (the picker -> TOMBOLA) | the Tombola screen comes up and plays (the hexagon and its balls) |
| np, `det_np_pick.txt` | **the pattern sequencer** comes up and plays the chord; no fault |
| both, `det_np_pick` then `det_np_seq` on the same NAND (second boot) | pass, no fault |
| **saved Tombola across images**: base picks Tombola (`op1.db`: `synth_sequencer tombola`), then np boots that NAND twice (`det_np_seq.txt`) | both boots: no fault, no assert, sound; the seq key opens **pattern**; after the first np boot `op1.db` says `synth_sequencer pattern` (TE wrote the resolved name back) |

Not usable as a test: an `op1.db` dropped into a *fresh* NAND (`user/op1.db` in the overlay) makes the first boot skip
TE's user-content provisioning and stop on a missing `/yaffs2/user/audio/drum/drum_0.aif` (`SampleBuffer.cpp:225`) with
**either** image; the cross-image run above is the real path (a Tombola setting saved by a running OP-1).

## Risks

- **Not run on a device yet** (op1-sdk's status is "sim-verified" too). The device test is in `docs/device-tests.md`
  "e16-nophysics".
- **The web builder** (`tools/make_web_delta.py`, the site's Build firmware tab) packs the delta's `OP1_vdk.ldr`
  with the **stock** content, so a web-built e16 keeps TE's `lander.svg` / `tombola.svg`. That is safe (their
  SVG-loaded slots return at once; measured as "np, stock SVGs" below), it only parses them as stock does. Carrying the
  two stub files in the delta is a follow-up for the release.
- Anyone who used Tombola loses it; a saved Tombola project plays its notes through pattern. The Tombola knob rows in
  `op1.db` are kept, so flashing back to stock or an older coop brings Tombola back with its settings.
- A future coop patch inside the freed ranges, or one that calls a removed function, is refused by the stock-byte
  checks only if it *patches* there; `nophysics_prove.py` must be re-run for new calls into TE code.
