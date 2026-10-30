import { useRef, useState, type KeyboardEvent } from 'react';
import type { PluginIndexEntry } from '@co-op/shared/plugins';
import { Feedback } from './ui/Feedback';
import { CopyToMods } from './ui/CopyToMods';
import { useSiteDevice } from './ui/device-choice';
import { DeviceMenu } from './ui/DeviceMenu';
import { Inspector } from './ui/Inspector';
import { Library } from './ui/Library';
import { Op1FunTab } from './ui/Op1Fun';
import { OpXy } from './ui/OpXy';
import { Packs } from './ui/Packs';
import { PatchesTab } from './ui/PatchesTab';
import { InstallOverUsb } from './ui/PluginInstall';
import { Plugins } from './ui/Plugins';
import { useDevice } from './ui/useDevice';
import { useLocalLibrary } from './ui/useLocalLibrary';
import { useTheme } from './ui/theme';
import { useOp1Fun } from './ui/useOp1Fun';

const TABS = [
  { id: 'library', label: 'Library' },
  { id: 'packs', label: 'Packs' },
  { id: 'patches', label: 'Patches' },
  { id: 'plugins', label: 'Plugins' },
  { id: 'op1fun', label: 'op1.fun' },
  { id: 'inspector', label: 'Inspector' },
] as const;

type TabId = (typeof TABS)[number]['id'];

export function App() {
  const [tab, setTab] = useState<TabId>('library');
  // One connection for every tab, so the Packs tab installs onto the drive the Library shows.
  const device = useDevice();
  const local = useLocalLibrary();
  const { theme, toggle } = useTheme();
  const { device: siteDevice, choose } = useSiteDevice();
  const op1 = siteDevice === 'op1';
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

  const brand = <DeviceMenu device={siteDevice} onChoose={choose} />;
  const headerActions = (
    <>
      <Feedback device={device} />
      <button type="button" className="theme-toggle" onClick={toggle} aria-label="Switch light or dark mode">
        <svg aria-hidden="true" viewBox="0 0 24 24">
          {theme === 'dark' ? (
            <path d="M20 14.5A8.5 8.5 0 1 1 9.5 4a7 7 0 0 0 10.5 10.5z" />
          ) : (
            <>
              <circle cx="12" cy="12" r="5" />
              <path d="M12 1v2M12 21v2M4.2 4.2l1.4 1.4M18.4 18.4l1.4 1.4M1 12h2M21 12h2M4.2 19.8l1.4-1.4M18.4 5.6l1.4-1.4" />
            </>
          )}
        </svg>
      </button>
    </>
  );

  return (
    <>
      {!op1 && (
        <main>
          <OpXy brand={brand} actions={headerActions} />
        </main>
      )}
      {/* Stays mounted while another device is shown, so an open drive and its tabs survive. */}
      <main hidden={!op1}>
        <header className="app-header">
          <div className="brand">
            {brand}
            <span className="eyebrow">PATCH LIBRARIAN / OP–1</span>
          </div>
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
          <div className="header-actions">{headerActions}</div>
        </header>
        {/* Every panel stays mounted so the connected drive and inspected file survive a tab switch. */}
        <div role="tabpanel" id="panel-library" aria-labelledby="tab-library" hidden={tab !== 'library'}>
          <Library device={device} active={op1 && tab === 'library'} op1fun={op1fun} />
        </div>
        <div role="tabpanel" id="panel-packs" aria-labelledby="tab-packs" hidden={tab !== 'packs'}>
          <Packs device={device} active={tab === 'packs'} onOpenLibrary={openLibrary} />
        </div>
        <div role="tabpanel" id="panel-patches" aria-labelledby="tab-patches" hidden={tab !== 'patches'}>
          <PatchesTab local={local} />
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
        <span>
          CO–OP · COMMUNITY LIBRARIAN FOR {op1 ? 'OP–1' : 'OP–XY'} · <span title={`Build ${__COMMIT__}`}>V{__APP_VERSION__}</span>
        </span>
        <a href="https://github.com/coldsoldr/co-op">GITHUB ↗</a>
      </footer>
    </>
  );
}
