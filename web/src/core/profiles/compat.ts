import type { Compat, DeviceProfile, ProfileSupports } from './types';
import type { Patch } from '../codec/patch';
import { soundTraits, type SoundTraits } from '../codec/sound-traits';

export type { ProfileSupports };

/** One reason a patch may not work on a firmware. */
export interface Finding {
  level: 'warn' | 'unsupported';
  reason: string;
}

/**
 * A profile's own judgement. It receives the traits and the findings from the supported-name
 * check and returns the final findings: it may add to them, drop them or change their level.
 */
export type TraitRule = (traits: SoundTraits, findings: readonly Finding[]) => Finding[];

export interface TraitCompatibilityOptions {
  rule?: TraitRule;
  /** Appended once to the reason when the result is unsupported for a trait finding. */
  unsupportedNote?: string;
}

/** One finding per saved name that `supports` does not list. A missing name is fine. */
export function nameFindings(traits: SoundTraits, supports: ProfileSupports): Finding[] {
  const findings: Finding[] = [];
  const check = (label: string, name: string | null, known: readonly string[]) => {
    if (name !== null && !known.includes(name)) {
      findings.push({ level: 'unsupported', reason: `${label} "${name}" is not part of this firmware` });
    }
  };
  if (traits.kind === 'drum') check('drum engine', traits.engine, supports.drumEngines);
  else check('engine', traits.engine, supports.synthEngines);
  check('effect', traits.fx, supports.fx);
  check('LFO', traits.lfo, supports.lfo);
  return findings;
}

/**
 * Builds a compatibility check from the names a firmware supports. Files without usable op-1
 * metadata, and metadata of an unknown kind, are unsupported. Otherwise every engine, effect or
 * LFO name outside `supports` is an unsupported finding, then the profile's `rule` has the final
 * say. The result takes the worst level and joins the reasons with '; '.
 */
export function traitCompatibility(
  supports: ProfileSupports,
  options: TraitCompatibilityOptions = {},
): DeviceProfile['compatibility'] {
  return (patch: Patch): Compat => {
    const traits = soundTraits(patch.meta);
    if (!traits) return { level: 'unsupported', reason: 'file has no op-1 metadata' };
    if (traits.kind === 'unknown') return { level: 'unsupported', reason: 'op-1 metadata is of an unrecognised kind' };
    const named = nameFindings(traits, supports);
    const findings = options.rule ? options.rule(traits, named) : named;
    if (findings.length === 0) return { level: 'ok' };
    const level = findings.some((f) => f.level === 'unsupported') ? 'unsupported' : 'warn';
    const reasons = findings.map((f) => f.reason);
    if (level === 'unsupported' && options.unsupportedNote) reasons.push(options.unsupportedNote);
    return { level, reason: reasons.join('; ') };
  };
}
