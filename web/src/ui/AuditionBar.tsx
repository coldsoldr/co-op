import type { CSSProperties } from 'react';
import type { Audition } from './useAudition';

/** The fill behind a row as a patch loads on the OP-1, and the mark of the one on it now. */
export function transferRow(audition: Audition, id: string): { className: string; style?: CSSProperties } {
  const moving = audition.transfer?.id === id ? audition.transfer.fraction : null;
  const on = audition.current?.id === id;
  const className = ['live-row', moving !== null ? 'loading' : '', on ? 'on-op1' : ''].filter(Boolean).join(' ');
  return moving === null ? { className } : { className, style: { '--fill': `${(moving * 100).toFixed(1)}%` } as CSSProperties };
}

/** True when the click was on a control of its own in the row, not on the row. */
export const clickedControl = (target: EventTarget): boolean =>
  target instanceof Element && target.closest('button, input, select, a, summary, [popover]') !== null;

/** The OP-1 link, shown on every tab that can audition. */
export function AuditionBar({ audition }: { audition: Audition }) {
  return (
    <section className="audition-bar" aria-label="Audition on the OP-1">
      <div>
        <strong>{audition.current ? `On the OP-1: ${audition.current.name}` : audition.ready ? 'OP-1 connected' : 'Audition on your OP-1'}</strong>
        <p className="muted">
          {audition.ready
            ? 'Click a synth patch to load it on the current slot. If you turn a knob during an audition, the OP-1 saves that sound over the slot.'
            : 'Needs hundekraft 3.0.3 with the audition plugin, and the OP-1 plugged in normally, not in disk mode.'}
        </p>
      </div>
      <div className="plugin-buttons">
        {audition.current ? (
          <button type="button" className="primary" onClick={() => void audition.putBack()}>
            Put my sound back
          </button>
        ) : (
          <button type="button" onClick={() => void audition.connect()}>
            {audition.ready ? 'Check again' : 'Connect for audition'}
          </button>
        )}
      </div>
      {audition.message && (
        <p role={audition.message.failed ? 'alert' : 'status'} className={audition.message.failed ? 'error' : undefined}>
          {audition.message.text}
        </p>
      )}
    </section>
  );
}
