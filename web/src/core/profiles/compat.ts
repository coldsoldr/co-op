import type { Compat, DeviceProfile, PluginContext, ProfileSupports } from './types';
import type { Patch } from '../codec/patch';
import { soundTraits, type SoundTraits } from '../codec/sound-traits';

export type { ProfileSupports };

/** One reason a patch may not work on a firmware. */
export interface Finding {
  level: 'warn' | 'unsupported';
  reason: string;
  /** Set on findings from the supported-name check: which saved name was not supported. */
  subject?: 'engine' | 'fx' | 'lfo';
  /** With `subject`: the saved name that was not supported. */
  name?: string;
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
  const check = (subject: NonNullable<Finding['subject']>, label: string, name: string | null, known: readonly string[]) => {
    if (name !== null && !known.includes(name)) {
      findings.push({ level: 'unsupported', reason: `${label} "${name}" is not part of this firmware`, subject, name });
    }
  };
  if (traits.kind === 'drum') check('engine', 'drum engine', traits.engine, supports.drumEngines);
  else check('engine', 'engine', traits.engine, supports.synthEngines);
  check('fx', 'effect', traits.fx, supports.fx);
  check('lfo', 'LFO', traits.lfo, supports.lfo);
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

/**
 * Rewrites the supported-name findings for an engine (synth sounds only) or effect that is a
 * library plugin: `needs plugin "<id>"` as a warning, or nothing once the device reports it
 * loaded. Other findings pass through. Run it before a profile's own rule, which then sees only
 * the findings that are not about library plugins.
 */
export function pluginFindings(traits: SoundTraits, findings: readonly Finding[], context: PluginContext): Finding[] {
  return findings.flatMap((finding): Finding[] => {
    const named = finding.subject === 'fx' || (finding.subject === 'engine' && traits.kind === 'synth');
    const id = named && finding.name !== undefined ? context.library.get(finding.name) : undefined;
    if (id === undefined) return [finding];
    if (context.loaded.has(finding.name as string)) return [];
    return [{ level: 'warn', reason: `needs plugin "${id}"` }];
  });
}

/**
 * The profile with a compatibility check that knows the plugin library and what is loaded, for
 * the places the UI judges patches. Profiles without `pluginCompatibility`, or no context, give
 * the profile back unchanged.
 */
export function withPlugins(profile: DeviceProfile, context: PluginContext | null): DeviceProfile {
  if (!context || !profile.pluginCompatibility) return profile;
  return { ...profile, compatibility: profile.pluginCompatibility(context) };
}
