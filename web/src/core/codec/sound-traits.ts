import { metaKind, type Op1Meta, type PatchKind } from './op1-meta';

/**
 * The firmware-relevant facts of one sound, taken from its op-1 metadata. Device profiles judge
 * compatibility and sniff firmware from these, never from the raw JSON.
 */
export interface SoundTraits {
  kind: PatchKind;
  /** The engine in `type` (a synth engine, or drum, dbox, gin); '' when missing. */
  engine: string;
  /** `fx_type`, whether or not the effect is switched on; null when missing or empty. */
  fx: string | null;
  /** `lfo_type`, whether or not the LFO is switched on; null when missing or empty. */
  lfo: string | null;
  /** `knobs[4]` when it holds an op1r-family selector or an opus engine mark; otherwise null. */
  engineSelector: number | null;
  /** `fx_params[4]` when it holds an op1r-family selector; otherwise null. */
  fxSelector: number | null;
  /** True for a hundekraft Gin kit: a `gin_state` or `dbox_data` member is present. */
  hasGin: boolean;
  /** True when the sound has an `opus` member (saved on opus firmware). */
  hasOpus: boolean;
  /** The plugin the `opus` member names, when it can be found; null otherwise. */
  opusPlugin: string | null;
  /** Effect names in the `opus` member's `<prefix>_type` slots, empty slots left out. */
  opusEffects: string[];
}

/** hundekraft and op1r engine and effect selectors in `knobs[4]` and `fx_params[4]`. */
export const OP1R_SELECTOR_MIN = 0x5300;
export const OP1R_SELECTOR_MAX = 0x53ff;
/** opus engine sounds save as `iter` with a mark of at least this in `knobs[4]`. */
export const OPUS_MARK_MIN = 0x100;

/**
 * Keys that may name the plugin in the `opus` member. The opus docs say the member names the
 * plugin but not under which key, so these are guesses tried in order before any other string.
 */
const OPUS_PLUGIN_KEYS = ['plugin', 'engine', 'id', 'name'];

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function nonEmptyString(value: unknown): string | null {
  return typeof value === 'string' && value !== '' ? value : null;
}

/** True for an op1r-family selector word (hundekraft and op1r engines and effects). */
export function isOp1rSelector(value: unknown): value is number {
  return typeof value === 'number' && value >= OP1R_SELECTOR_MIN && value <= OP1R_SELECTOR_MAX;
}

function slot(list: unknown, index: number): unknown {
  return Array.isArray(list) ? list[index] : undefined;
}

function opusPluginOf(opus: Record<string, unknown>): string | null {
  for (const key of OPUS_PLUGIN_KEYS) {
    const value = nonEmptyString(opus[key]);
    if (value) return value;
  }
  for (const [key, value] of Object.entries(opus)) {
    if (key.endsWith('_type')) continue;
    const text = nonEmptyString(value);
    if (text) return text;
  }
  return null;
}

/** Reads the traits of one sound; null when there is no metadata. */
export function soundTraits(meta: Op1Meta | null): SoundTraits | null {
  if (!isRecord(meta)) return null;
  const opus = isRecord(meta['opus']) ? meta['opus'] : null;
  const knob4 = slot(meta['knobs'], 4);
  const fx4 = slot(meta['fx_params'], 4);
  const opusMark = opus !== null && typeof knob4 === 'number' && knob4 >= OPUS_MARK_MIN;
  return {
    kind: metaKind(meta),
    engine: typeof meta['type'] === 'string' ? meta['type'] : '',
    fx: nonEmptyString(meta['fx_type']),
    lfo: nonEmptyString(meta['lfo_type']),
    engineSelector: isOp1rSelector(knob4) || opusMark ? (knob4 as number) : null,
    fxSelector: isOp1rSelector(fx4) ? fx4 : null,
    hasGin: meta['gin_state'] !== undefined || meta['dbox_data'] !== undefined,
    hasOpus: opus !== null,
    opusPlugin: opus ? opusPluginOf(opus) : null,
    opusEffects: opus
      ? Object.entries(opus)
          .filter(([key]) => key.endsWith('_type'))
          .map(([, value]) => nonEmptyString(value))
          .filter((value): value is string => value !== null)
      : [],
  };
}
