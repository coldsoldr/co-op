import type { PluginIndex } from '@co-op/shared/plugins';
import type { SoundTraits } from '../core/codec/sound-traits';
import { requiredPlugins } from '../core/plugins/requirements';
import { traitDetails } from './format';
import type { LibraryRow } from './rows';

/** What naming the plugins a patch needs takes: the library, the active firmware, what is loaded. */
export interface PluginRequirements {
  /** Null while the plugin library is not loaded: no plugins are named then. */
  index: PluginIndex | null;
  firmwareId: string;
  /** Saved names the last USB check found loaded. */
  loaded: ReadonlySet<string>;
}

/** The traits behind a row's badge: engine, effect, LFO, selectors and plugin, and the library plugins it needs. */
export function TraitsPopover({ row, traits, plugins }: { row: LibraryRow; traits: SoundTraits; plugins?: PluginRequirements }) {
  const needs = plugins?.index ? requiredPlugins(traits, plugins.index, plugins.firmwareId) : [];
  const lines = [
    ...traitDetails(traits),
    ...needs.map((p): [string, string] => [
      'Needs plugin',
      `${p.name} (${p.file})${p.savedName !== undefined && plugins?.loaded.has(p.savedName) ? ', loaded on the OP-1' : ''}`,
    ]),
  ];
  return (
    <details className="traits">
      <summary aria-label={`Sound details of ${row.fileName}`}>details</summary>
      <dl>
        {lines.map(([label, value]) => (
          <div key={`${label} ${value}`}>
            <dt>{label}</dt>
            <dd>{value}</dd>
          </div>
        ))}
      </dl>
    </details>
  );
}
