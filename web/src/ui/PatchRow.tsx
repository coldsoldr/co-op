import type { ReactNode } from 'react';
import type { LocalPatch } from '../core/library/local-library';
import { clickedControl, transferRow } from './AuditionBar';
import { formatSize } from './format';
import type { Audition } from './useAudition';
import type { usePatchPreview } from './usePatchPreview';

/** Worked out from the list, before a patch is read. */
const playsOnOp1 = (patch: LocalPatch): boolean => (patch.kind === 'synth' || patch.engine === 'drum' || patch.engine === 'dbox') && patch.error === null;

interface PatchRowProps {
  patch: LocalPatch;
  audition: Audition;
  preview: ReturnType<typeof usePatchPreview>;
  load: () => Promise<Uint8Array>;
  /** The text under the name; the patch's path unless given. */
  file?: string;
  notes?: string;
  /** Shows a checkbox cell first. */
  select?: { label: string; checked: boolean; onChange: (on: boolean) => void };
  /** Shows a last cell, for the row's menu. */
  actions?: ReactNode;
}

/** One patch in a list: click to audition on the OP-1, or preview it in the browser. */
export function PatchRow({ patch, audition, preview, load, file, notes, select, actions }: PatchRowProps) {
  const onOp1 = audition.ready && playsOnOp1(patch);
  const playing = preview.playing === patch.path;
  const noPreview = patch.preview === false;
  const play = (): void => void audition.play(patch.path, patch.name, { load });
  return (
    <tr
      data-path={patch.path}
      {...(onOp1 ? transferRow(audition, patch.path) : {})}
      onClick={(e) => {
        if (onOp1 && !clickedControl(e.target)) play();
      }}
    >
      {select && (
        <td className="select">
          <input type="checkbox" aria-label={select.label} checked={select.checked} onChange={(e) => select.onChange(e.target.checked)} />
        </td>
      )}
      <th scope="row" className="name">
        {onOp1 ? (
          <button type="button" className="row-pick" onClick={play} aria-label={`Audition ${patch.name}`}>
            {patch.name}
          </button>
        ) : (
          patch.name
        )}
        <code className="file">{file ?? patch.path}</code>
        {(patch.engine || patch.fx) && <span className="sound">{[patch.engine, patch.fx].filter(Boolean).join(' · ')}</span>}
        {noPreview && <span className="sound">no preview</span>}
        {patch.error && <span className="row-error">{patch.error}</span>}
        {notes && <span className="sound">{notes}</span>}
      </th>
      <td className="kind">{patch.kind}</td>
      <td className="size">{patch.error ? '' : formatSize(patch.sizeBytes)}</td>
      <td className="play">
        <button
          type="button"
          className={`play-key ${patch.kind}`}
          disabled={noPreview || patch.error !== null}
          title={noPreview ? 'No preview: the OP-1 only stored its voice clip' : 'Preview in the browser'}
          aria-label={`${playing ? 'Stop' : 'Preview'} ${patch.name}`}
          aria-pressed={playing}
          onClick={() => void preview.toggle(patch.path, patch.name, load)}
        >
          <svg aria-hidden="true" viewBox="0 0 24 24">
            <path d={playing ? 'M6 5h12v14H6z' : 'M7 4l13 8-13 8z'} />
          </svg>
        </button>
      </td>
      {actions !== undefined && <td>{actions}</td>}
    </tr>
  );
}
