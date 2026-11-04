import type { CopyState } from './useCopyToFolder';

const summary = (copied: number, already: number, zip: boolean): string =>
  [`${zip ? 'Added' : 'Copied'} ${copied}${zip ? ' to the zip' : ''}`, ...(already > 0 ? [`${already} already there`] : [])].join(', ');

/** What the last copy did. */
export function CopyOutcome({ outcome }: { outcome: CopyState | null }) {
  if (!outcome) return null;
  if (outcome.kind === 'error') {
    return (
      <p role="alert" className="error">
        {outcome.message}
      </p>
    );
  }
  const { copied, already, failed } = outcome.result;
  return (
    <div role={failed.length > 0 ? 'alert' : 'status'} className={failed.length > 0 ? 'error' : 'notice'}>
      <p>{summary(copied.length, already.length, outcome.zip)}.</p>
      {failed.length > 0 && (
        <ul className="file-list">
          {failed.map((f) => (
            <li key={f.path}>
              <code>{f.path}</code>: {f.message}
            </li>
          ))}
        </ul>
      )}
    </div>
  );
}
