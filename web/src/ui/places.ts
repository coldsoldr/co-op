import type { DeviceStore } from '../core/store/store';
import { rootOf } from './format';
import type { Device } from './useDevice';
import type { LocalLibrary } from './useLocalLibrary';

/** Selected patches that share a destination: each patch root on the OP-1, or all of My folder. */
export interface BulkGroup {
  key: string;
  /** What the destination picker calls this group; '' when there is only ever one. */
  label: string;
  root: string;
  /** Folders under `root`; '' is `root` itself. */
  folders: readonly string[];
  paths: string[];
}

/** Somewhere patches live and can be changed: the OP-1's drive, or My folder. */
export interface Place {
  busy: boolean;
  /** True when changes are collected for download rather than written. */
  staged: boolean;
  /** The name used when warning that a delete is permanent. */
  name: string;
  groups(paths: readonly string[]): BulkGroup[];
  /** Runs a change. `op` returns the paths it created, so the list can show them. */
  run(op: (store: DeviceStore) => Promise<string[]>, touched: readonly string[]): Promise<void>;
}

export function drivePlace(device: Device): Place {
  return {
    busy: device.busy,
    staged: device.source?.staged != null,
    name: 'the OP-1 drive',
    groups(paths) {
      const groups = new Map<string, BulkGroup>();
      for (const path of paths) {
        const root = rootOf({ path });
        const group = groups.get(root) ?? { key: root, label: `${root} patches`, root, folders: device.folders[root] ?? [], paths: [] };
        group.paths.push(path);
        groups.set(root, group);
      }
      return [...groups.values()];
    },
    run: (op, touched) => device.run(op, touched),
  };
}

export function localPlace(local: LocalLibrary): Place {
  return {
    busy: local.progress !== null,
    staged: false,
    name: 'your folder',
    groups: (paths) => [{ key: 'local', label: '', root: '', folders: ['', ...local.folders], paths: [...paths] }],
    run: (op, touched) => local.apply(op, touched),
  };
}
