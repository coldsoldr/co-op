import { useRef, useState, type KeyboardEvent } from 'react';
import type { PluginIndexEntry } from '@co-op/shared/plugins';
import { Feedback } from './ui/Feedback';
import { CopyToMods } from './ui/CopyToMods';
import { Inspector } from './ui/Inspector';
import { Library } from './ui/Library';
import { Op1FunTab } from './ui/Op1Fun';
import { Packs } from './ui/Packs';
import { InstallOverUsb } from './ui/PluginInstall';
import { Plugins } from './ui/Plugins';
import { useDevice } from './ui/useDevice';
import { useOp1Fun } from './ui/useOp1Fun';

const TABS = [
  { id: 'library', label: 'Library' },
  { id: 'packs', label: 'Packs' },
  { id: 'plugins', label: 'Plugins' },
  { id: 'op1fun', label: 'op1.fun' },
  { id: 'inspector', label: 'Inspector' },
] as const;

type TabId = (typeof TABS)[number]['id'];

export function App() {
  const [tab, setTab] = useState<TabId>('library');
  // One connection for every tab, so the Packs tab installs onto the drive the Library shows.
  const device = useDevice();
  // The op1.fun account, shared by the op1.fun tab and the Library's upload action.
  const op1fun = useOp1Fun();

  // hundekraft plugins install over USB MIDI; the inventory the OP-1 reports afterwards is shared.
  // opus plugins are copied into mods/ on the open drive.
  const pluginActions = (plugin: PluginIndexEntry) =>
    plugin.firmware === 'op1/hundekraft' ? (
      <InstallOverUsb plugin={plugin} onInstalled={device.setInstalledPlugins} />
    ) : (
      <CopyToMods plugin={plugin} device={device} />
    );
  const buttons = useRef<Partial<Record<TabId, HTMLButtonElement | null>>>({});

  function openLibrary(): void {
    setTab('library');
    buttons.current.library?.focus();
  }

  /** Arrow keys, Home and End move between tabs, as in the WAI-ARIA tabs pattern. */
  function onKeyDown(event: KeyboardEvent<HTMLDivElement>): void {
    const index = TABS.findIndex((t) => t.id === tab);
    const last = TABS.length - 1;
    const next =
      event.key === 'ArrowRight' ? (index + 1) % TABS.length
      : event.key === 'ArrowLeft' ? (index + last) % TABS.length
      : event.key === 'Home' ? 0
      : event.key === 'End' ? last
      : null;
    if (next === null) return;
    event.preventDefault();
    const id = (TABS[next] as (typeof TABS)[number]).id;
    setTab(id);
    buttons.current[id]?.focus();
  }

  return (
    <>
      <main>
        <header className="app-header">
          <h1>co-op patch librarian</h1>
          <Feedback device={device} />
        </header>
        <div role="tablist" aria-label="Views" className="tabs" onKeyDown={onKeyDown}>
          {TABS.map((t) => (
            <button
              key={t.id}
              ref={(el) => {
                buttons.current[t.id] = el;
              }}
              type="button"
              role="tab"
              id={`tab-${t.id}`}
              aria-selected={tab === t.id}
              aria-controls={`panel-${t.id}`}
              tabIndex={tab === t.id ? 0 : -1}
              onClick={() => setTab(t.id)}
            >
              {t.label}
            </button>
          ))}
        </div>
        {/* Every panel stays mounted so the connected drive and inspected file survive a tab switch. */}
        <div role="tabpanel" id="panel-library" aria-labelledby="tab-library" hidden={tab !== 'library'}>
          <Library device={device} active={tab === 'library'} op1fun={op1fun} />
        </div>
        <div role="tabpanel" id="panel-packs" aria-labelledby="tab-packs" hidden={tab !== 'packs'}>
          <Packs device={device} active={tab === 'packs'} onOpenLibrary={openLibrary} />
        </div>
        <div role="tabpanel" id="panel-plugins" aria-labelledby="tab-plugins" hidden={tab !== 'plugins'}>
          <Plugins active={tab === 'plugins'} renderActions={pluginActions} />
        </div>
        <div role="tabpanel" id="panel-op1fun" aria-labelledby="tab-op1fun" hidden={tab !== 'op1fun'}>
          <Op1FunTab op1fun={op1fun} device={device} active={tab === 'op1fun'} onOpenLibrary={openLibrary} />
        </div>
        <div role="tabpanel" id="panel-inspector" aria-labelledby="tab-inspector" hidden={tab !== 'inspector'}>
          <Inspector active={tab === 'inspector'} device={device} />
        </div>
      </main>
      <footer>
        <a href="https://github.com/coldsoldr/co-op">co-op on GitHub</a>
      </footer>
    </>
  );
}
