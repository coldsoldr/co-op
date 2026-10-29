import { useEffect, useState } from 'react';
import { listOpusPlugins, type OpusPluginListing } from '../core/library/mods';
import type { DeviceStore } from '../core/store/store';
import { messageOf } from '../core/util';
import { formatSize } from './format';

/**
 * The opus profile's read-only list of plugins in mods/: file names in load order and sizes,
 * flagging files that do not start with the OPMD magic, plus a warning for co-op leftovers that
 * recovery could not clear. Re-read each time a library load settles (`settledCount`); it only
 * reads, so it does not go through the operation queue.
 */
export function OpusPlugins({ store, settledCount }: { store: DeviceStore; settledCount: number }) {
  const [listing, setListing] = useState<OpusPluginListing | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let live = true;
    listOpusPlugins(store).then(
      (next) => {
        if (!live) return;
        setListing(next);
        setError(null);
      },
      (e: unknown) => {
        if (live) setError(messageOf(e));
      },
    );
    return () => {
      live = false;
    };
  }, [store, settledCount]);

  const plugins = listing?.plugins ?? null;
  return (
    <section className="panel" aria-labelledby="opus-plugins-heading">
      <h3 id="opus-plugins-heading">Installed opus plugins</h3>
      {error && <p className="error">Could not list mods: {error}</p>}
      {listing === null && !error && <p role="status">Reading mods…</p>}
      {plugins?.length === 0 && <p>No .opmod files in the mods folder.</p>}
      {plugins && plugins.length > 0 && (
        <table className="plugins">
          <caption>{plugins.length} plugin file(s) in mods, in the order opus loads them</caption>
          <thead>
            <tr>
              <th scope="col">File</th>
              <th scope="col">Size</th>
              <th scope="col">Check</th>
            </tr>
          </thead>
          <tbody>
            {plugins.map((plugin) => (
              <tr key={plugin.path} data-plugin={plugin.path}>
                <td>
                  <code>{plugin.name}</code>
                </td>
                <td>{formatSize(plugin.sizeBytes)}</td>
                <td>
                  {plugin.error ? (
                    <span className="badge unreadable">unreadable ({plugin.error})</span>
                  ) : plugin.valid ? (
                    <span className="badge ok">ok</span>
                  ) : (
                    <span className="badge unsupported">not an opus plugin (no OPMD header)</span>
                  )}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
      {listing && listing.leftovers.length > 0 && (
        <div className="warning" role="status">
          <p>
            These files were left in mods by an interrupted co-op write and were not cleared (co-op never cleans up a
            read-only copy). Reconnect the drive in Chrome or Edge to recover them, or check them before deleting:
          </p>
          <ul className="file-list">
            {listing.leftovers.map((path) => (
              <li key={path}>
                <code>{path}</code>
              </li>
            ))}
          </ul>
        </div>
      )}
    </section>
  );
}
