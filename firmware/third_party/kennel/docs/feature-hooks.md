# Runtime services and feature integration

<!-- Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). -->

Features declare an `op1_feature_t` in `synth_sdk/op1_feature.h`. The loader validates the descriptor and callbacks, resolves exclusive claims, then registers enabled providers. Read the descriptor size/API comments and `kennel/kn_exports.c` before using a service.

## Execution contexts

- `OP1_EV_REGISTER`: register provider types after claims resolve and before screen construction.
- `OP1_EV_SERVICE`: bounded, nonblocking main-loop work.
- Input and key synchronization: KeyHandler thread.
- Audio `tick` and instrument/effect callbacks: audio interrupt; no file I/O, allocation, waiting or GUI calls.
- `midi_raw`: timer context; queue substantial work for another context.
- Screen update and overlays: graphics thread; parsed screen objects remain alive until restart.

Other event contexts are specified in the headers. Do not assume a callback is a safe place to save files.

## Service map

| Header | Contract |
|---|---|
| `op1_feature.h` | Feature lifecycle, input, overlays, notes, settings, text entry, files, storage and musical context |
| `op1_feature_sound.h` | Validated sound access, sound changes and saved settings |
| `op1_feature_seq.h` | Sequencer provider registration and note handling |
| `op1_feature_lfo.h` | LFO registration, modulation and saved-word validation |
| `op1_feature_midi.h` | MIDI input/output and queued control changes |
| `op1_feature_tape.h` | Tape edits, cache-aware writeback, before/after events and exclusive tape hooks |
| `op1_feature_sampler.h` / `op1_feature_samplerplus.h` | Sampler lifecycle and editing |
| `op1_feature_drum.h` | Extensions to sampled drums |
| `op1_feature_drum_synth.h` | Native synthesized-drum providers and save epochs |
| `op1_input.h` | Once-per-block live-input instrument processing and scoped T2 tuner |

## Notes and claims

Scale runs before Chord in the note pipeline. Forward transformed output through `op1_note_on/off(next, ...)`. Track exact emitted note identities; later note-off must not be recomputed from changed octave, chord, scale or slot settings. Handle reset, repeated notes, source changes and engine switches.

Claims describe exclusive resources. The core resolves them deterministically: built-in providers, priority, then name. A losing feature is disabled as a whole. Mirror claims in the manifest and descriptor; an empty list explicitly claims nothing. See the API header and `kn_features.c` for supported claim names.

## Storage and graphics ownership

Use the storage/tape services rather than bypassing caches or writing from audio callbacks. Keep dirty state after failures; check slot identity and save epochs before committing deferred writes. Gin demonstrates provider-scoped state and transactional retries.

The graphics host snapshots the shared screen table for each frame. Removed documents release drawing caches on the graphics thread; parsed SVG trees and cached node pointers remain valid. Input/audio callbacks must not free drawing buffers.

## Implementation map

`kennel/kn_core.c` dispatches feature events. `kn_midi.c` queues controls, `kn_seq.c` hosts sequencers, `kn_lfo.c` hosts LFOs, `kn_tape.c` and `kn_tapesvc.c` preserve tape-cache contracts, `kn_drum_synth.c` hosts drums, and `kn_input.c` routes selected input. Hook bytes and checked locations are defined by `make_firmware.py`, `hook.S` and the relevant core assembly stubs.

The complete plugin workflow, callback examples and persistence/screen contracts are in `../../../PLUGIN_GUIDE.md`.
