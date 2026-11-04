import { useState } from 'react';
import type { LocalPatch } from '../core/library/local-library';
import { dirname } from '../core/store/store';
import { PatchActions } from './PatchActions';
import type { LocalLibrary } from './useLocalLibrary';

/** Rename, set name, move, copy and delete for one patch in My folder. */
export function LocalActions({ patch, local }: { patch: LocalPatch; local: LocalLibrary }) {
  const [error, setError] = useState<string | null>(null);
  return (
    <>
      <PatchActions
        entry={{ path: patch.path, fileName: patch.fileName, info: { name: patch.name, hasMeta: patch.error === null } }}
        root=""
        current={dirname(patch.path)}
        folders={['', ...local.folders]}
        run={local.apply}
        staged={false}
        busy={local.progress !== null}
        driveName="your folder"
        onError={setError}
      />
      {error && <span className="row-error">{error}</span>}
    </>
  );
}
