# Inventory over SysEx

The workshop web app reads what a kennel OP-1 has installed and what its last boot decided, and writes the list of
switched-off plugins. It uses the SysEx update protocol (`synth_sdk/devupd/`)
with three more commands. The host resolves conflicts against the installed inventory before staging changes.

## Messages

Same framing as the update: `F0 00 20 76 7D cmd seq_lo seq_hi <7-bit packed payload> F7`, reply `cmd | 0x40`, same
seq, payload `status, ...`. Every message both ways is at most 96 bytes (the OP-1's USB-MIDI input stops on longer
SysEx), and the host keeps one message in flight.

| cmd | Request | Reply (after the status byte) |
|---|---|---|
| 1 HELLO | | u32 protocol version (1), **u8 capabilities**: bit 0 = inventory, bit 1 = plugin files (below). Stock 2.x devupd builds send no byte 4. |
| 7 LIST | u32 offset | u32 total length, up to 64 bytes of the inventory text from offset. Offset past the end: status 5 (offset). |
| 8 OFF_GET | u8 which (0 stored, 1 in effect since boot), u16 offset | u32 total length, up to 64 bytes of the list |
| 9 OFF_SET | u16 offset, u16 total, up to 64 bytes | u16 bytes received. Complete -> stored, then OK. |
| 10 DELETE | path, NUL | staged: the plugin file is removed at the restart (see Plugin files) |
| 11 RESTART | | 9 (wait key) until T1 on the prompt, then OK and the restart; 2 (state) after T2 |

Without the inventory callbacks (`du_fs.inv_read` etc. = 0) the three commands answer status 1 (format).

OFF_SET: a piece at offset 0 starts a new value; a piece at any other offset must continue where the last one ended,
else status 5 with the byte count the OP-1 has (a lost reply: the host resends from there; a resent last piece
answers 5 with count = total). The value is `[A-Za-z0-9 ._,:-]`, at most 1023 characters (`DU_OFF_MAX`: every plugin's id fits; `kn_off_store`). Entries are ids (`docs/plugin-format.md` "Names"),
stored as `op1rsettings` key `kennel.off` and read back before the reply. It applies at the next boot: the loader
and the registry read the list once (`kn_off_boot`), so OFF_GET with which = 1 still shows the old one.

## Plugin files

After one core install the page sends, updates and removes single plugin files, without the firmware:
- Paths: BEGIN takes `plugins/<file>.knl|.op1p` as well as `OP1_vdk.ldr` and `content/...` (one level, no hidden
  names: `plugins/.boot` is the crash marker). DELETE takes a plugin file in `content/` or `plugins/` and nothing
  else (`du_plugin_path`). A path sent and deleted in one session is refused (state); a resent DELETE is fine.
- Staging as before: every file with its size and CRC, deletes as `D <path>` lines in the manifest, all of it
  only after T1 (COMMIT when files or deletes are staged, RESTART otherwise, e.g. only the switched-off list
  changed). T2 discards everything.
- At the restart the boot hook checks every staged file first, then copies and deletes. Without `OP1_vdk.ldr` in the
  manifest it boots on (no second restart): the loader runs later in the same boot and loads the new files.
- Same core: the LIST header carries `core <start> <len> <crc>`, the CRC-32 of the running core's `.text` +
  `.rodata` (0x01E00000 to link.ld's `__core_ro_end`, computed once at the first LIST). The page computes the same
  over its build's `OP1_vdk.ldr` blocks (`ldrRangeCrc`); equal means only plugin files need to go. Otherwise it
  sends the whole firmware as before.

## Inventory text

Built once, on the first LIST, from records made at boot, so every page of one read belongs to the same text.

```
kennel-inventory 1\tcore <start hex8> <len hex8> <crc hex8>\tpresets <count dec5> cap <cap dec5>
<kind>\t<name>\t<source>\t<path>\t<version>\t<author>\t<size>\t<crc>\t<state>\t<claims>\t<reason>
...
```

Header fields are tab-separated and found by their first word, so a page ignores the ones it does not know.
`presets`: R246's count of user synth presets (0x018B610C, recounted from the database) and the cap of its save gate
(`R1 = cap (X)` at 0x019FEF02: 100 in R246, 300 in a 300 build), five digits each. A build with a smaller cap than
the count stops R246's boot on `patch.cpp:632 synthSynthes_patch_count <= kMaxSynthSynthes_patches`, so the page must
refuse it. The count is rewritten in the cached text at each LIST from offset 0 (saves since boot show); the field
is left out if the gate instruction is not `R1 = imm (X)` (0xE121).

| Field | Values |
|---|---|
| kind | `synth`, `fx`, `feature`, `-` (the file did not parse) |
| name | the plugin's id (`docs/plugin-format.md` "Names"): a file's name without `.knl` / `.op1p`, a built-in feature's name |
| source | `builtin`, `knl`, `op1p` |
| path | `plugins/<file>` or `content/<file>`; empty for built-ins |
| version, author | `.knl` extension table (tags 3, 2); empty if absent |
| size | file size in bytes (0 for built-ins) |
| crc | 8 hex digits: the CRC the file carries (`.knl` header offset 12, `.op1p` trailer); 0 for built-ins. Compare with the same field of the file the page has. |
| state | `loaded`, `failed` (a check failed), `skipped` (crash marker: the last boot did not reach the UI), `off` (switched off by the user, lost a claim, or a built-in replaced by a file) |
| claims | comma-separated, each with a prefix: `+` held, `!` lost (to the feature named in reason), `~` not held (the plugin is not running). From the `.knl` extension table, else the feature object (built-ins). |
| reason | why it failed / is off (`CRC mismatch`, `switched off`, `lost page:t4 to chord`, `replaced by a file`), `replaces the built-in` for a file that took a built-in's number, else empty |

Order: plugin files (plugins/ first, then content/), built-in engines and effects, built-in features. Tabs and
newlines in names become spaces. The page should match records by path (files) or kind + name (built-ins).

## Device side

- `kennel/kn_inv.c`, `kn_inv.h`: records (arena, `kn_alloc`), the text, `kn_inv_read`.
- `kennel/kn_boot.c` `load_file`: one record per file (path, size, state, reason); `load_knl` / `load_op1p` add kind,
  name, CRC and the extension table (`kn_parse` now returns claims, author and version in `kn_info`); a crash-marker
  skip records every file as skipped; built-in engines and effects are added after the loader.
- `kennel/kn_features.c`: `kn_feature_register` reports each decision (`kn_inv_feature_result`: a new record for a
  built-in, the file's record for a file); `resolve_claims` reports the loser, its claim and the winner
  (`kn_inv_lost`) before it compacts the list. Both are weak, so the registry's host test runs without them.
- `devupd/du.c`: LIST / OFF_GET / OFF_SET framing and the OFF_SET reassembly (`du_session.off_buf`, 1024 bytes);
  `devupd/du_fw.c`: the callbacks (`kn_inv_read`, `op1_setting_get_str`, `kn_off_store`, `kn_off_boot`).
- Memory: records and text in the arena (about 100 bytes per record plus the strings; the text about 80 bytes per
  line), nothing at boot beyond the records. BSS: `du_session` grew by 168 bytes.
- The DB write for OFF_SET runs in `du_service` (main loop), the same thread as the update's file writes.
