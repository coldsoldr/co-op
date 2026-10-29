import { useCallback, useEffect, useRef, useState, type ReactNode } from 'react';
import type { PluginIndexEntry } from '@co-op/shared/plugins';
import { readPatch } from '../core/codec/patch';
import { soundTraits } from '../core/codec/sound-traits';
import { listOpusPlugins } from '../core/library/mods';
import { fetchPluginBytes } from '../core/plugins/catalogue';
import { installedNames, requiredPlugins } from '../core/plugins/requirements';
import { op1Opus } from '../core/profiles/op1-opus';
import type { DeviceProfile } from '../core/profiles/types';
import { messageOf, sleep } from '../core/util';
import { CopyToMods } from './CopyToMods';
import { Dialog } from './Dialog';
import { saveBlob } from './download';
import { InstallOverUsb } from './PluginInstall';
import type { Device } from './useDevice';

const base = (): string => import.meta.env.BASE_URL;

/** How long an install waits for a plugin index still loading before it goes on without it. */
const INDEX_WAIT_MS = 5_000;

interface Asking {
  plugins: PluginIndexEntry[];
  /** How many of the patches need one of them. */
  patches: number;
  resolve: (ok: boolean) => void;
}

/** "a", "a and b", "a, b and c". */
function listOf(names: readonly string[]): string {
  return names.length <= 1 ? (names[0] ?? '') : `${names.slice(0, -1).join(', ')} and ${names.at(-1)}`;
}

/** The question the prompt asks, singular or plural. */
export function pluginQuestion(plugins: readonly { name: string }[], patches: number): string {
  const who = patches === 1 ? 'This patch needs' : 'These patches need';
  const what = plugins.length === 1 ? 'the plugin' : 'the plugins';
  return `${who} ${what} ${listOf(plugins.map((p) => p.name))}. Install ${plugins.length === 1 ? 'it' : 'them'}?`;
}

/** The names of the valid .opmod files in mods/, lower-cased; empty when they cannot be listed. */
async function modsFiles(device: Device): Promise<Set<string>> {
  if (!device.source) return new Set();
  try {
    const { plugins } = await listOpusPlugins(device.source.store);
    return new Set(plugins.filter((p) => p.valid).map((p) => p.name.toLowerCase()));
  } catch {
    return new Set();
  }
}

/**
 * The plugin prompt for installs, after the write guard and before writing.
 * `confirmPlugins(files, profile)` finds the library plugins the patches need on that profile that
 * are not known to be there (loaded in the last USB check for hundekraft, in mods/ for opus) and,
 * when there are any, asks: install over USB, copy to mods/ or download each one, then Continue
 * without (true) or Cancel (false, the default). Without the plugin index it resolves true at
 * once: installs never wait on it. Render `dialog` somewhere in the tree.
 */
export function usePluginPrompt(device: Device): {
  confirmPlugins: (files: readonly Uint8Array[], profile: DeviceProfile) => Promise<boolean>;
  dialog: ReactNode;
} {
  const [asking, setAsking] = useState<Asking | null>(null);
  const cancelRef = useRef<HTMLButtonElement>(null);
  // Cancel is the default. This runs after the dialog's showModal, which focuses the first button.
  useEffect(() => {
    if (asking) cancelRef.current?.focus();
  }, [asking]);
  const { loadPluginIndex, installedPlugins } = device;

  const confirmPlugins = useCallback(
    async (files: readonly Uint8Array[], profile: DeviceProfile): Promise<boolean> => {
      const index = await Promise.race([loadPluginIndex(), sleep(INDEX_WAIT_MS).then(() => null)]);
      if (!index) return true;
      const loaded = installedNames(installedPlugins);
      const inMods = profile.id === op1Opus.id ? await modsFiles(device) : new Set<string>();
      const known = (p: PluginIndexEntry): boolean =>
        p.firmware === op1Opus.id ? inMods.has(p.file.toLowerCase()) : p.savedName !== undefined && loaded.has(p.savedName);
      const needed = new Map<string, PluginIndexEntry>();
      let patches = 0;
      for (const bytes of files) {
        let traits;
        try {
          traits = soundTraits(readPatch(bytes).meta);
        } catch {
          continue;
        }
        if (!traits) continue;
        const missing = requiredPlugins(traits, index, profile.id).filter((p) => !known(p));
        if (missing.length > 0) patches++;
        for (const p of missing) needed.set(p.id, p);
      }
      if (needed.size === 0) return true;
      return new Promise<boolean>((resolve) => setAsking({ plugins: [...needed.values()], patches, resolve }));
    },
    [device, installedPlugins, loadPluginIndex],
  );

  const answer = (ok: boolean): void => {
    asking?.resolve(ok);
    setAsking(null);
  };

  const loaded = installedNames(installedPlugins);
  const dialog = asking ? (
    <Dialog title={asking.plugins.length === 1 ? 'Plugin needed' : 'Plugins needed'} onClose={() => answer(false)}>
      <p>{pluginQuestion(asking.plugins, asking.patches)}</p>
      <ul className="file-list plugin-needs">
        {asking.plugins.map((plugin) => (
          <li key={plugin.id}>
            <strong>{plugin.name}</strong> (<code>{plugin.file}</code>){' '}
            {plugin.firmware === 'op1/hundekraft' ? (
              <InstallOverUsb plugin={plugin} onInstalled={device.setInstalledPlugins} />
            ) : (
              <CopyToMods plugin={plugin} device={device} />
            )}{' '}
            <DownloadPlugin plugin={plugin} />
            {plugin.savedName !== undefined && loaded.has(plugin.savedName) && (
              <span role="status"> {plugin.name} is loaded on the OP-1.</span>
            )}
          </li>
        ))}
      </ul>
      <div className="dialog-buttons">
        {/* Cancel is the default, as in the other install guards: Enter never writes. */}
        <button ref={cancelRef} type="button" onClick={() => answer(false)}>
          Cancel
        </button>
        <button type="button" className="primary" onClick={() => answer(true)}>
          Continue without
        </button>
      </div>
    </Dialog>
  ) : null;

  return { confirmPlugins, dialog };
}

/** Download for a plugin file, checked against the index first. */
function DownloadPlugin({ plugin }: { plugin: PluginIndexEntry }) {
  const [working, setWorking] = useState(false);
  const [error, setError] = useState<string | null>(null);

  async function download(): Promise<void> {
    setWorking(true);
    setError(null);
    try {
      const bytes = await fetchPluginBytes(base(), plugin);
      saveBlob(new Blob([bytes as Uint8Array<ArrayBuffer>], { type: 'application/octet-stream' }), plugin.file);
    } catch (e) {
      setError(`Download failed: ${messageOf(e)}`);
    } finally {
      setWorking(false);
    }
  }

  return (
    <>
      <button type="button" onClick={() => void download()} disabled={working} aria-label={`Download ${plugin.file}`}>
        Download
      </button>
      {error && (
        <span role="alert" className="error">
          {' '}
          {error}
        </span>
      )}
    </>
  );
}
