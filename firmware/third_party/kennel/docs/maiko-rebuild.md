# Maiko-inspired utilities

<!-- Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). -->

Scale, naming, FILE, flash check, undo and tape tools are Hundekraft implementations informed by Maiko Pruett’s #249/#250 behavior. No #249 utility code or bytes are included. Separate #250 effect reconstructions are credited in their plugin sources and provenance.

## Scale

Scale is a note-pipeline stage before Chord. White keys map to the selected scale degree; excluded black keys are silent. It applies to keyboard, sequencer and MIDI input. The Shift+T2 overlay edits root and scale. Keep shared held-note identity/reset behavior intact when changing the mapping.

## Naming and storage

Naming uses a 13×3 character grid and a 12-character name limit, adding a suffix on collision. FILE uses core storage enumeration and flash-check services, with cached results refreshed after disk mode or on request. These are feature pages, not replacements for raw filesystem ownership.

## Undo and tape edits

Undo keeps history on flash and preserves complete tape-write event boundaries. Writes must flow through tape-aware services. Before/after edit events, flushed/drained state and cache identity prevent reading or restoring partial operations. Four-tape switching is refused during recording, disk mode or incompatible clipboard work.

Keep save failures recoverable, validate file/range bounds, retain dirty state for retry and avoid direct cache bypass. See `op1_feature_tape.h`, `kn_tape.c`, `kn_tapesvc.c`, `undo/` and `tapes/` for the implemented contracts.
