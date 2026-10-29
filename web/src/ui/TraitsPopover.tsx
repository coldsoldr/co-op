import type { SoundTraits } from '../core/codec/sound-traits';
import { traitDetails } from './format';
import type { LibraryRow } from './rows';

/** The traits behind a row's badge: engine, effect, LFO, selectors and plugin. */
export function TraitsPopover({ row, traits }: { row: LibraryRow; traits: SoundTraits }) {
  return (
    <details className="traits">
      <summary aria-label={`Sound details of ${row.fileName}`}>details</summary>
      <dl>
        {traitDetails(traits).map(([label, value]) => (
          <div key={label}>
            <dt>{label}</dt>
            <dd>{value}</dd>
          </div>
        ))}
      </dl>
    </details>
  );
}
