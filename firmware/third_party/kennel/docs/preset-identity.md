# Preset identity: engines by type name

Saved sounds identify their engine or effect by type name. The dispatcher repairs the selector word before range validation and copying; unknown types retain their fallback selector. See `dispatcher.c`, `patch_type_hook`, `fx_patch_type_hook` and `hook.S`.

## Why the name is usable
Synth patch files store a JSON object with `type` (the engine's own name: op1r engines save `"supersaw"` etc., not
`"iter"`) and `knobs[0..7]`. The name survives load and save, so it is already a stable identity. Today the engine
is picked at render time from `knobs[4] = 0x5300 + order` (`dispatcher.c` `engine_of`); the name is only matched to
type 14 (`instr_name_eq`, hook at 0x019B975C), which sees the string but not the patch.

## R246 patch parser
`FUN_01A037A8` (patch.cpp, "patch_data" assert at 0x01A04BA4) validates a patch JSON and copies it into the
slot's patch JSON. Callers: 0x0199F210, 0x01995680, 0x0199FA4E, 0x01A04BC0 (op1r's `hook_dpload`), 0x01A051F8,
so one hook covers loading at boot, preset browsing and drum+.

| Address | What happens | Registers |
|---|---|---|
| 0x01A037A8 | entry | R0 = source JSON, saved at FP-0x78 for the whole function (also in R7 until 0x01A0401C reuses R7); R1 = destination. Output is first built in a local Json::Value at SP+0x98 |
| 0x01A0387E | `type` read into a std::string at FP-0x38 | |
| **0x01A03884** | `CALL instr_type_from_name` (0x019B9810), which uses the name compare at 0x019B975C | R0 = &type string; source JSON at [FP-0x78] (= R7 here) |
| 0x01A03888.. | the string is freed right after | |
| 0x01A04A14..0x01A04A56 | for i in 0..7: `FUN_01A1DEB0(type, i, &word)` checks `knobs[i]` against the type's range table (0x01DE5890 + type·0x40 + i·8: min, max); failure returns error 15 | |
| 0x01A04A88 | local `knobs` = source `knobs` (whole array copied from the source JSON) | |
| 0x01A04016 | local `fx_type` = source `fx_type` | |
| **0x01A04030** | `CALL fx_type_from_name` (0x019AAE44, name compare 0x019AAD8E = op1r's `fx_name_eq`) on the local copy's `fx_type` string | R0 = &type string; source at [FP-0x78] |
| 0x01A04084..0x01A040AE | for i in 0..7: `FUN_01A1DF4E(fxtype, i, &word)` checks source `fx_params[i]` (range table 0x01DE5C50 + type·0x30 + i·6: min, max, default); failure returns error 0x18 | |
| 0x01A040EE | local `fx_params` = source `fx_params` | |

op1r already patches this function at 0x01A042A2 (LFO range check) and 0x01A044A8 (drum+ "op1r" keep). No Maiko
#249 or hundekraft patch in 0x01A037A8..0x01A04BB0.

Range for iter (type 14) `knobs[4]` and for filter (FX type 5) `fx_params[4]` in the R246 boot snapshot: 0..32767.
Selectors above 0x531F therefore pass validation.

jsoncpp in R246 (Value = 16 bytes: +0 value (8 bytes), +8 type byte, +0xC comments):

| Address | Function |
|---|---|
| 0x019BD890 | `Value(Int)` (type 1; 137 callers) |
| 0x019BD878 | `Value(UInt)` (type 2) |
| 0x019BD854 | `Value(double)` (type 3) |
| 0x019BD832 | `Value(bool)` (type 5) |
| 0x019BFA30 | `Value(ValueType)` |
| 0x019BFEB4 | `operator[](const char *)` |
| 0x019BFAF8 | `operator[](index)` |
| 0x019BE8D8 | `size()` |
| 0x019BE0C0 | `asInt()` |
| 0x019BDDC0 | `operator=(const Value &)` |
| 0x019BFCBC | `append(const Value &)` (= `v[size()] = x`) |

## Hook behavior
- Replace the 4-byte `CALL 0x019B9810` at 0x01A03884 with a CALL to a stub: `R1 = [FP - 0x78]; JUMP.L
  _patch_type_hook` (the parser's FP is still current, the callee has not linked yet).
- `patch_type_hook(const dstring *name, JsonValue *src)`: calls the original `instr_type_from_name(name)`, then, if
  the result is 14 and a loaded engine has that name, writes that engine's selector into `src["knobs"][4]`
  before the range check and the copy. Otherwise it changes nothing: old sounds keep their `knobs[4]` (fallback),
  and unknown names still load as iter.
- Selectors: an engine with a legacy order keeps `0x5300 + order`, so files saved by kennel still open on op1r
  and older hundekraft builds. New engines get a selector assigned at boot from 0x5320 up. A sound saved with
  one is still found by name on the next boot. On older firmware it loads as iter, as an unknown order does today.
- Writing `src["knobs"][4] = sel`: only when `src["knobs"]` is an array of size 8 (`size()` 0x019BE8D8; non-const
  `operator[]` would create members); then `Value(Int)` 0x019BD890 into a temporary and `operator=` 0x019BDDC0. An int
  Value owns no heap memory, so the temporary needs no destructor call.
- Effects: the same stub on `CALL 0x019AAE44` at 0x01A04030 writes `src["fx_params"][4]` before the check and the copy.
