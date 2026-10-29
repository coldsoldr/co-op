import { useCallback, useState, type ReactNode } from 'react';
import type { DeviceProfile } from '../core/profiles/types';
import type { UnsupportedPatch } from '../core/profiles/guard';
import { Dialog } from './Dialog';

interface Asking {
  patches: readonly UnsupportedPatch[];
  profile: DeviceProfile;
  resolve: (ok: boolean) => void;
}

/** The dialog's message: stock gets the boot warning, other profiles a milder one. */
export function unsupportedMessage(profile: DeviceProfile): string {
  return profile.firmware === 'stock'
    ? 'These patches use engines, effects or data that stock firmware does not have. Writing them may stop a stock OP-1 at boot.'
    : `These patches may not load on ${profile.label}.`;
}

/**
 * The shared write guard. `confirmUnsupported(patches, profile)` resolves true straight away when
 * `patches` is empty, otherwise shows a dialog naming each patch and its reason and resolves with
 * the choice; Cancel (the default) or Escape resolves false. Render `dialog` somewhere in the tree.
 */
export function useConfirmUnsupported(): {
  confirmUnsupported: (patches: readonly UnsupportedPatch[], profile: DeviceProfile) => Promise<boolean>;
  dialog: ReactNode;
} {
  const [asking, setAsking] = useState<Asking | null>(null);

  const confirmUnsupported = useCallback(
    (patches: readonly UnsupportedPatch[], profile: DeviceProfile): Promise<boolean> => {
      if (patches.length === 0) return Promise.resolve(true);
      return new Promise<boolean>((resolve) => setAsking({ patches, profile, resolve }));
    },
    [],
  );

  const answer = (ok: boolean): void => {
    asking?.resolve(ok);
    setAsking(null);
  };

  const dialog = asking ? (
    <Dialog title={`Not supported on ${asking.profile.label}`} onClose={() => answer(false)}>
      <p>{unsupportedMessage(asking.profile)}</p>
      <ul className="file-list">
        {asking.patches.map((p, index) => (
          <li key={p.id ?? index}>
            <code>{p.path}</code>: {p.reason}
          </li>
        ))}
      </ul>
      <div className="dialog-buttons">
        <button type="button" onClick={() => answer(false)} autoFocus>
          Cancel
        </button>
        <button type="button" className="danger" onClick={() => answer(true)}>
          Write anyway
        </button>
      </div>
    </Dialog>
  ) : null;

  return { confirmUnsupported, dialog };
}
