# The test loop: an agent that tests its own changes (2026-11-20)

Today every firmware change ends in a `.op1` the owner flashes by hand and a checklist the owner walks through:
`docs/device-tests.md` has 74 entries headed "device test (pending)". This document says what a coding agent on
the owner's PC (Windows 11, WSL, the OP-1 on USB) can test without the owner, in three tiers, what each tier costs to
build, and what was built with it. Confidence marks as everywhere: [C] confirmed (in bytes, in op1emu, on the device,
as stated), [I] inferred. `docs/brick-safety.md` binds everything here.

## Decisions for the owner (recommended option first)

1. **A restart command in coop** (tier 1 needs it to be hands-free).
   **Recommended: add it the way kennel does (watchdog reset after the flushes), without a prompt on dev images only,
   with a T1 prompt on release images.** Otherwise: a prompt always (one key press per cycle), or none (one power
   cycle by hand per plugin change). Risk: an unsaved edit lost if a flush is wrong; no brick path (section "Restart").
2. **How a new core gets on the device** (tier 2).
   **Recommended: in this order: (a) now, a script that copies the package when te-boot's disk appears, with `RBT`
   in the file name so te-boot reboots by itself: the owner holds COM, powers on and presses 1, nothing else;
   (b) later, a dev-only resident loader that takes the core from a file on the NAND at boot, so a core change is a
   file transfer and a restart.** Not recommended: flashing the boot area from the running app (kennel's development
   update): it is the one option where app code erases NAND boot blocks, and brick-safety rule 4 says te-boot only.
3. **`RBT` in dev package names.** **Recommended: yes for dev packages, no for releases.** brick-safety rule 3 calls it
   harmless ("only adds a reboot after the update") but "best avoided"; `verify_bootloader.py` accepts it [C].
4. **Scratch NANDs on WSL's own disk.** **Recommended: yes, as `tools/emu_gate.py` does**: at most three 553 MB files,
   deleted after every run, never the bases. The rule after the full C: disk was "working copies under /mnt/i"; the
   emulator on a NAND there ran five times slower and the scenario ended before its chord (measured below). The
   alternative is `OP1EMU_SCRATCH=/mnt/i/op1emu-tmp` and scenarios rewritten without wall-timed waits.
5. **The screen from the device.** **Recommended: a firmware screenshot the host fetches over SysEx (design below);**
   a webcam only for what the panel itself shows (colour, flicker, the boot picture's position).
6. **Standing permissions for the agent.** **Recommended: allow the gate and the read commands of `coop_usb.py`
   without a prompt; allow `--write` for `/yaffs2/plugins/` and `/yaffs2/user/` once tier 1 is in use; a flash stays
   the owner's act.** (Claude Code settings; not changed by this document.)
7. **One check with the OP-1 plugged in** (two minutes, no flash): does Windows list an "OP-1" recording device? The
   firmware's USB descriptors have audio streaming both ways [C op1emu]; if it records, tier 1 needs no audio cable.

## What exists today

| Piece | State |
|---|---|
| op1emu headless, deterministic, guest-timed keys and encoders, audio dump, screenshots, write traces, memory dump at exit | [C] `docs/emulator.md`, patches 0001-0004 |
| NAND with plugin files, a file read back out of the NAND | [C] `tools/make_nand.py`, `tools/nand_file.py` |
| Gate procedure | by hand: build an image, make a NAND, run `tests/sim/hear.sh`, read the stats, compare WAVs, type the result into `docs/device-tests.md`; ad hoc `gate.sh` copies in `~/op1emu-work/e7..e9` |
| SysEx on the device: HELLO, FILE_GET (any path), PUT / DELETE (plugins/, user/), SLOT2, RUN, INVENTORY, PLUGIN_SET | [C device for file read, file write and RUN: `docs/device-stats/p1_*`, `p5d_*`, `p5e_*`; C op1emu for INVENTORY and PLUGIN_SET; device pending for those two] |
| The same inputs on the device and in op1emu, block-exact, with sections and cost lines | [C] the runner, `docs/test-runner.md`; started from a browser page by the owner |
| Device audio compared with the emulator's | [C on a simulated recording] `tools/run_compare.py`; the owner's interface is a Steinberg UR44C (its MIDI port is listed on this PC) |
| Hosts | browser only: `tools/coop_sysex.html`, the librarian (`web/src/core/usbmidi/coop.ts`), tested against fakes (`web/test/fake-coop.ts`, `web/e2e/op1-stub.ts`) |
| Flash | by hand: te-boot menu (COM at power-on, "1. Upload firmware"), copy the `.op1`, power-cycle |
| Restart | none in coop: after PUT, DELETE or PLUGIN_SET the owner power-cycles |

### What op1emu does not cover (collected from the docs)

- No cache model, no SDRAM timing, no performance counters (`pf_live 0`): cycle figures are instruction-level. Every
  cost question (`docs/performance.md`, the m1 and p6 series, the rings budget) is the device's. [C]
- Timing: "faster than the device whenever the host is"; wall-timed script lines (`wait`, `screenshot`, `quit`) move
  with the host's load. `det_chord` is not sample-exact run to run on one image any more (the onset lands on one block
  or the next; e6 gate notes), so a compare needs the onset alignment or a second run. Cause not found [I].
- The output-mode byte is never set: a stereo check needs `g_coop_force_stereo_out=1`. [C]
- The stock engine list is unusable headless (keys play nothing while it is open); sounds come from the eight slots. [C]
- Encoder acceleration differs from the device's feel [I]; the tempo screen moved 0.4 / 0.5 BPM a step.
- Not exercised by this project: audio input (line, mic, radio), MIDI clock, the soft power switch and shutdown path,
  te-boot's update menu, panel variants and gamma.
- **Corrected here: "op1emu has no USB-MIDI" (`docs/sysex.md`, the handover) is no longer true.** See the next section.

## Findings of this survey

1. **op1emu serves the emulated OP-1's USB port over USB/IP on TCP 3240, headless too** (its README, "USB";
   `gui/usbipd.cpp`). A stdlib client can attach without a kernel module. Done today against the e9 image with
   `saw.coop` on the NAND: HELLO `coop` protocol 1 caps `0x3f` build `e9`; INVENTORY `saw.coop synth saw loaded on
   0.2 coldsoldr 6520`; FILE_GET of `coop_stats.txt` 4,405 bytes in 1.92 s (69 messages, 2.3 KB/s); RUN status
   idle. [C op1emu] So the whole SysEx surface (PUT, a power cycle as a second run on the same NAND, the runner) can
   be gated in the emulator; `g_coop_sysex_selftest` is no longer the only way. One client at a time; the port
   belongs to the first emulator started.
2. **The R246 firmware describes itself as a USB audio device** (2367:0004): interface 0 audio control, 1 audio
   streaming with an isochronous IN endpoint 0x86 (276 bytes a packet), 2 audio streaming with an isochronous OUT
   0x07 and its feedback 0x82, 3 MIDI streaming. [C op1emu, read from the firmware's own descriptors] 276 bytes fits
   44.1 kHz stereo 24-bit [I]. The stream's bus is `0xff800e34 / 0xff800f34` (`docs/handover-c1.md`), not the codec
   master, so whether coop's stereo stages are on it is open [I]: decision 7.
3. **kennel has both things coop lacks, in source we already vendor** (`third_party/kennel/synth_sdk/devupd/`):
   - a restart: `WDOG_CTL = 0xAD0; WDOG_CNT = 1; WDOG_CTL = 0xFF0` (`du_fw.c`), the reset te-boot itself uses after an
     update (te-boot `0x169ec`), after draining the delayed patch saves, flushing the tape chunks, 400 ms, the stock
     `unmount_yaffs` (`0x019A5F20`) and 100 ms; behind a T1 prompt on the OP-1. Every plugin install through the
     hundekraft workshop on the owner's unit ended in this restart [C device under hundekraft 3.0.3, te-op1-cfw
     `docs/device-tests.md`]. kennel's note: a plain power-off is wrong there
     ("over USB the OP-1 stayed off"), hence the watchdog.
   - a development update: the core LDR staged on yaffs over SysEx, applied by a boot hook before USB, audio and the
     GUI: size, CRC, the LDR block chain and an R246 fingerprint checked, then the stock erase of blocks 4..131
     (`0x019EABB4`) and `write_boot_block` per block, each block number through a guard; te-boot paths refused. Built
     always, hooked only in `--dev` firmware.
4. **A scratch NAND on /mnt/i makes the emulator too slow for the scenarios.** `det_chord` on the e9 image: NAND on
   /mnt/i/op1emu-tmp, 8.7 s of guest time in 60 s, the scripted `quit` came before the chord (no onset, peak 64); NAND
   on WSL's ext4, 43.7 s of guest time in 55 s, the chord at frame 1,319,152, peak 3,221, width 0.24. [C, one run
   each] The gate now fails a run whose audio ends before the scenario's last `at` (check `ran_to_end`).
5. **The OP-1 was not on USB during this survey** (no 2367 device, no OP-1 MIDI port; only the UR44C). No MIDI
   package is installed in either Python (no mido, rtmidi, pygame) and no node MIDI module; Windows' own winmm through
   `ctypes` needs none. ffmpeg is installed (audio capture from a command line), OpenCV too (a camera).
6. **A USB relay cannot power-cycle an OP-1**: it has a battery. Hands-free restarts are software only.

## Tier 0: the emulator gate, one command (built)

`tools/emu_gate.py`: image or ELF in, JSON verdict out.

```sh
# from Windows: wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && python3 -I tools/emu_gate.py ...'
python3 -I tools/emu_gate.py --elf build/core/coop_combined.elf --ref-image ~/op1emu-work/e9/e9.ldr \
    --scenario det_chord --label core                                   # a core change: no regression on pristine
python3 -I tools/emu_gate.py --image IMG.ldr --plugins build/plugins/test_fx.coop \
    --scenario det_fx_controls --scenario det_fx_reload --chain \
    --expect 'plugins.loaded==1' --expect 'plugins.failed==0' --expect 'fxctl.turns>=1' \
    --expect 'cost.fx1_plugin.max<60000' --label test_fx                # a plugin, then a power cycle on the same NAND
```

- Per scenario: a scratch NAND (the pristine base, or the stock package plus the plugin files), one headless
  deterministic run, then the checks: `emulator_exit`, `no_fault` (the log), `ran_to_end`, `stats_written`, every
  `--expect` (`tag.key`, `cost.name.avg|max|n`, `audio.nonzero`, `faults`; an unknown name fails) and `--expect-line`
  regex against `coop_stats.txt`, and with `--ref-image` / `--ref-elf` `matches_reference`: sample-identical, or
  within `--tolerance` (16) once both are aligned on the onset, with one more try of both runs before "differs"
  (`--exact` accepts identical only).
- Output: the verdict as JSON on stdout (`verdict`, `failed`, per scenario the checks, the audio's levels, peak,
  onset and width, the stats tags asked for with `--report`), and a directory under `/mnt/i/op1emu-tmp/gate/` with
  the log, the whole `coop_stats.txt` / `coop_run.txt` / `coop_sysex_test.txt`, and **the scenario's screenshots as
  PNG** (their paths rewritten into that directory), which an agent can open and look at. Exit 0 pass, 1 fail, 2 the
  gate could not run.
- One emulator at a time: it holds `~/op1emu-work/emu.lock` (the lock `run_emu.sh` and `op1emu_gui.sh` take) for the
  whole gate and waits for any op1emu started outside the lock. Scratch NANDs are `gate_<pid>_*.bin`, removed after
  each run, on errors and signals, and swept when their gate died; names with `pristine`, `stock` or `otp` are refused
  in code. Nothing under `build/` is written.
- Time: 55-60 s per `det_chord` run [C]; a compare is two to four runs; the long UI scenarios 2-4 min each.
  Runs in parallel are not safe (wall-timed lines, one USB/IP port).

What it catches: boot hangs, faults and asserts, a plugin that does not load and why, memory budgets, wrong control
words, persistence across a power cycle, audio regressions against the last image, screens (by eye, from the PNGs).
What it cannot: everything in "does not cover" above.

Cost to finish tier 0 (all S, no owner action, no brick surface):
- op1emu patch 0005: `at MS quit` and `at MS screenshot PATH` in guest time, so no scenario depends on wall time
  (ends finding 4 and the truncation risk, and lets the NAND sit on any disk).
- A `tests/sim/gates.json`: named gate sets (core, plugin, ui) with their scenarios and expectations, so "run the
  gate" is one name; a stored reference per scenario and image hash to halve compare time.
- SysEx in the gate: start `coop_usb.py --transport usbip` beside the run (install a plugin over PUT, second run,
  INVENTORY says `loaded`); drive the runner with RUN instead of the autostart word.
- Screen assertions: compare a PNG with a stored one (exact, or by region) once a screen is accepted.

## Tier 1: the device, plugins and files only, no flash

What can change on the real OP-1 over USB today, with no flash:

| Action | State | Brick risk |
|---|---|---|
| Install, replace, remove a `.coop`; switch it on or off | exists (PUT / DELETE / PLUGIN_SET), applied at the next boot | none: files under `/yaffs2/plugins`; a plugin that hangs the boot is removed after a boot with the plugin scan skipped [I: coop has no skip key yet, see below] |
| Restart | **missing**; small firmware addition (below) | none to te-boot; an unsaved edit can be lost |
| Read `coop_stats.txt`, `coop_run.txt`, any file | exists (FILE_GET, 1.5 KB/s on the device, 2.3 in op1emu) | none |
| Keys and encoders at block-exact times, sections with costs | exists (CRUN file + RUN) | none |
| Notes over MIDI | stock behaviour [I], not needed beside the runner | none |
| Record the output | line out into the UR44C exists (the browser page records); from a command line: ffmpeg [I, not run]; USB audio [I, decision 7] | none |
| The screen | **missing**; small firmware addition (below), or a camera | none |
| FX 2 slots | exists (SLOT2) | none |

Built for this tier: `tools/coop_usb.py`, the SysEx client for a command line (ports, hello, inventory, stats, get,
run-status; put, delete, plugin-set behind `--write`). Transports: `winmm` for the OP-1 on Windows (ctypes, nothing
to install) [I: written from the API, enumeration run, never opened on the OP-1], `usbip` for op1emu [C], `mock` [C,
13 tests]. Windows gives a MIDI port to one program: the librarian's tab must not hold it.

Firmware additions, as designs (not built; core source untouched):

- **`0x2B RESTART`** (S). Main loop only, never from the timer context. Refuse (status 2) while recording, while a
  PUT is open, while the settings menu is open or a run is playing. Then: reply, stop the runner and SysEx service,
  flush coop's own state (slot file, settings, markers, the tape I/O step's held chunks: the paths the stats already
  count), wait for the stock background file worker's ring to empty (bounded, 3 s, as kennel's `bgs_drain`), 400 ms,
  the stock unmount `0x019A5F20`, 100 ms, the watchdog sequence above, loop. Release images show kennel's T1 / T2
  prompt first; an image built with a dev word (`g_coop_dev=1`) skips the prompt. Capability bit 6. Gate: op1emu,
  if it models the watchdog reset [I]; else the second-run power cycle. Risk: the flush list is incomplete and a
  knob turn of the last seconds is lost (the same loss as a crash); te-boot is reached before any app code, so no
  brick path. The addresses are kennel's for R246 and need checking against `symbols/sdram.sym`.
- **Boot guard** (S): a marker file written before the plugin scan and removed after the first main-loop pass; a
  boot that finds it skips the plugin files once (kennel does this: "an unfinished previous boot triggers one safe
  start without plugin files"). With RESTART this makes a hanging plugin self-healing; without it an agent could
  leave the OP-1 needing a reflash to clear a bad file. **Build before any unattended plugin loop.**
- **`0x2A SCREEN`** (S-M): op 0 snapshots the frame buffer (320 x 160) on the gfx thread into a buffer, run-length
  coded (stock screens are a handful of colours; a few KB [I]); op 1 pages it out like FILE_GET. As a runner event
  (`#@shot NAME` in the script) the shot lands at an exact block. Read-only. Until the frame buffer's address and
  stride are confirmed in `docs/screens.md` this is [I].
- **Faster transfer** (S): several PUT_DATA in flight, or 120-byte messages if the device's input takes them [I:
  the firmware accepts 128, the host limit of 96 came from the librarian]. Matters for tier 2 (b), not for plugins.

The loop then, for a plugin change, with nobody at the device:
`emu_gate` passes → `coop_usb put` → `restart` → wait for the port and HELLO → `inventory` says `loaded` → PUT a
CRUN, RUN start, record (ffmpeg) → `get coop_run.txt`, `stats` → `run_compare.py` against the emulator's dump of the
same scenario → a result block for `docs/device-tests.md` written by the agent, marked as measured by the loop.

What it catches beyond tier 0: real cycle costs with the caches and SDRAM (the `cost`, `engines`, `gaps`, `slow`
lines, the performance counters), main-loop stalls, real USB timing, the audio through the codec, memory on the
real heap, behaviour across real restarts. What stays the owner's: how it sounds and feels, the encoders under the
fingers, the panel, anything with the tape he cares about.

What the owner does once: flash one dev image that has RESTART, the boot guard and SCREEN; leave the OP-1 on USB with
line out in the UR44C (or nothing, if decision 7 says USB audio works) at a fixed volume; keep the librarian's tab
closed while the loop runs. Never again for plugin work: power cycles, copying stats, the page's Run button.
Until that image exists the loop still works with one human act per cycle (a power cycle on request).

Cost: RESTART + boot guard S, SCREEN S-M, the orchestrator script (`tools/dev_loop.py`, stdlib, Windows) S, the
winmm transport's first device run S. Brick risk: none beyond a normal coop flash (the dev image goes through the
te-boot menu like every package).

## Tier 2: the device with core changes

Today: build `.op1` → the owner holds COM at power-on, presses 1, copies the file, waits, power-cycles, walks the
checklist. Options, cheapest first:

| Option | Hands | Firmware work | brick-safety |
|---|---|---|---|
| (a) Watcher script: when te-boot's disk (2367:0002) appears, check the sha256 and `verify_bootloader.py`, copy the `.op1` (with `RBT` in its name), eject; te-boot flashes and reboots; then HELLO, the tier-1 run | COM + power, key 1 | none | inside the rules: te-boot menu, no te-boot.ldr, guard passed. `RBT`: decision 3 |
| (b) Resident loader, dev images only: the flashed image keeps a complete core; at boot, before audio starts, a small resident part looks for `/yaffs2/user/coop_core.bin` (magic, ABI number, size, CRC, the resident's own build id), copies it over the core's region and syncs the caches; any failure, the boot guard's marker, or a held key keeps the flashed core | none (PUT ~180 KB, 2 min at 1.5 KB/s, then RESTART) | L | inside the rules: nothing but a yaffs file is written; the flashed core is the fallback and te-boot is untouched |
| (c) kennel's development update: stage the LDR over SysEx, flash blocks 4..131 from a boot hook | none (2.3 MB: 26 min at 1.5 KB/s unless the transfer gets faster) | M (the code exists under GPL, with its guards) | **outside rule 4** ("install only through the te-boot menu"); the app erases boot-area blocks; a guard bug is the brick scenario. Owner's decision; not recommended |
| (d) The stock app-side path (a package found at boot, "Path B") | none | S-M | the doc's own analysis says a package without te-boot.ldr never reaches the bootloader writer, but rule 4 excludes the path, force is hard-wired to 1, and how a package reaches `/yaffs2` is not mapped [I]. No |
| (e) USB DFU or a ROM USB boot | - | - | te-boot offers mass storage behind a menu key only; the boot-mode pins are not reachable. Not possible [I] |
| (f) A relay or switched hub for power | - | - | useless: battery (finding 6). A servo on the power switch and the COM key would work and is not worth building |

Notes on (b): the core is 165 KB of code, 12 KB of constants and 610 KB of zeroed data in region A, with trampolines
in L1 and patched stock call sites that hold core addresses fixed at image time (`inject_app.py`, `l1_insert.py`,
`l1_stereo_insert.py`). A loaded core therefore has to keep every address the patches use: either all hooks go
through a fixed table at the start of the region (the `OP1X` header is the natural place), or the loader refuses a
file whose table differs from the resident's. A change to an L1 patch, a hook site or the table still needs a flash.
The audit of how many sites embed a core address decides L or more [I]. What it would give: most `core/sdram/*.cc`
changes (stats, UI, SysEx, the plugin host, the runner, the engines' C) tested on the device with nobody there.

What tier 2 catches: the same as tier 1, for core changes. What the owner does once for (a): nothing but the two key
actions per flash; for (b): flash the loader image once, then only when a patch site changes.

**Can te-boot's own update be rehearsed?** Probably: op1emu boots te-boot from the NAND and lists the upload disk as
2367:0002 over USB/IP [C its README; not tried here]. A dry run of the watcher script against the emulator is the
way to prove (a) before it meets the device.

## Hardware worth buying

Nothing is needed. The UR44C and a cable from the OP-1's output are already there.
- **A webcam on a small arm over the screen** (about 25 EUR): the only thing that sees the real panel (colours,
  flicker, the boot picture's position, frame rate). OpenCV is installed. Only if decision 5 is not enough.
- Not worth it: a USB relay or switched hub (battery), an audio loopback cable (the interface is better, and USB
  audio may replace both), a logic analyser (no test here needs one).

## From Claude Code

- Emulator: `wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && python3 -I tools/emu_gate.py ...'`,
  read the JSON, open the PNGs under `I:\op1emu-tmp\gate\...`.
- Device: `python tools\coop_usb.py hello | inventory | stats | get PATH` with the Windows Python (3.11, anaconda);
  `--write put FILE.coop` once allowed. No daemon is needed: each call opens the port, asks, closes, as the
  librarian does. A lock file would be needed only if two agents shared the device.
- Recording: `ffmpeg -f dshow -i audio="<the UR44C input>" -t SECONDS out.wav` [I], started before RUN; then
  `tools/run_compare.py SCRIPT out.wav EMU.wav` (it finds the runner's clicks and the clock drift).
- Is the OP-1 there: `python tools\coop_usb.py ports` (`op1_present`), enumeration only.

## Recommended order

1. Use `emu_gate.py` for every gate from now on; add patch 0005 and `gates.json` (tier 0, S).
2. With the OP-1 plugged in: decision 7's check, then `coop_usb.py hello`, `inventory`, `stats` (reads only): the
   winmm transport's first device run.
3. One dev image: boot guard, RESTART, SCREEN; gated in op1emu over USB/IP; the owner flashes it once.
4. `tools/dev_loop.py`: the tier-1 loop end to end; from here plugin work needs the owner only to listen.
5. The watcher script for (a), rehearsed in op1emu; core changes then cost the owner two key actions.
6. The resident loader (b), after the hook audit, if core iteration is still the bottleneck.

## Built with this document

- `tools/emu_gate.py` and `tests/tools/test_emu_gate.py` (13 tests). Run on 2026-11-20, e9 image: `det_chord` on
  the pristine base, and with `saw.coop` (`plugins.loaded==1`, `ran_to_end`, verdict pass, 55 s, no scratch NAND
  left). The reference compare and `--chain` paths are covered by the unit tests of their parts, not yet by an
  emulator run (another agent was using the emulator; runs were kept to two).
- `tools/coop_usb.py` and `tests/tools/test_coop_usb.py` (13 tests): framing equal to `framing.ts`, the paged reads,
  PUT with size and CRC, a lost reply, the write guard. Against op1emu over USB/IP: HELLO, INVENTORY, FILE_GET, RUN
  status [C]. Nothing was sent to a MIDI port.

## Open

- The run-to-run onset shift of `det_chord` (cause in the harness unknown); whether USB/IP traffic changes a
  deterministic run [I]; whether op1emu models the watchdog reset [I].
- USB audio on the device: present, usable, and which bus (decision 7).
- The frame buffer's address and format for SCREEN; the stock flush list for RESTART.
- INVENTORY and PLUGIN_SET on the device (e3, pending): step 2 of the order settles it without a flash.
