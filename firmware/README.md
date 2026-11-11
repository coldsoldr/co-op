# coop-fw

A custom firmware for the original Teenage Engineering OP-1 (firmware R246), built as a patcher
over your own `op1_246.op1`. Stock firmware stays the operating system; coop-fw replaces the
audio core and adds a plugin runtime.

Goals (no compromise): four stereo tape tracks (album dropped), stereo synths, two FX slots, two
LFO slots, a stereo plugin ABI with a porting guide for hundekraft and opus plugins, firmware
upgrade over USB, seamless co-op integration, a master bus suite (compressor, drive, EQ), and a
UI that stays as smooth as stock.

Status: Phase 0 (scaffolding). See `docs/` for the plan's working documents and
`docs/brick-safety.md` before flashing anything.

## Layout

| Path | Contents |
|---|---|
| `core/l1/` | L1-resident code: audio callback shim, hot kernels, L1 audio stack |
| `core/sdram/` | SDRAM core: mixer graph, tape, master bus, LFO/FX hosts, loader, USB update |
| `core/patches/` | patch tables, hook assembly, link scripts |
| `core/include/` | internal headers: firmware addresses (generated from `symbols/`), fixed point, memory map |
| `sdk/` | public plugin ABI headers and the export table |
| `plugins/` | coop-fw plugins |
| `tools/` | patcher, packer, LDR tools, L1 dumper and decryptor, symbol tools |
| `symbols/` | our address maps |
| `docs/`, `guide/` | working documents; the UI/UX guide and the porting guide |
| `tests/` | host unit tests and simulator harness |
| `third_party/kennel/` | vendored subset of hundekraft's kennel core (GPL-3.0-or-later) |

## Building

Firmware and plugins build under WSL with the bfin-elf GCC toolchain from
`te-op1-cfw/tools/build_bfin_toolchain.sh` and `build_bfin_gcc.sh` (default `~/op1-toolchain/bfin/bin`).
Host tests build natively. See the `Makefile`.

## Publishing a release to co-op

The site's Build firmware tab makes the package in the browser from the user's own `op1_246.op1`, with a
delta that holds only coop's bytes. For a new release, after its package has passed its gates and been flashed:

    python3 -I tools/make_web_delta.py op1_246.op1 build/pkg/op1_246_coop_<tag>.op1 ../web/public/firmware/coop-<tag>.delta

then copy the printed hashes into `web/src/core/firmware/release.ts` (tag, file name, delta path) and remove the
old delta. Building from the unpacked stock folder rather than TE's `op1_246.op1` is fine: the delta only covers
`OP1_vdk.ldr`, and the browser takes every other file from the user's own package.

## Licence

GPL-3.0-or-later with the OP-1 firmware permission (see `LICENSE` and `NOTICE`). This repository
contains no Teenage Engineering firmware and no device keys; the patcher needs your own copy of
the official R246 image.
