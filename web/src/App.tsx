import { useRef, useState, type KeyboardEvent } from 'react';
import { Inspector } from './ui/Inspector';
import { Library } from './ui/Library';

const TABS = [
  { id: 'library', label: 'Library' },
  { id: 'inspector', label: 'Inspector' },
] as const;

type TabId = (typeof TABS)[number]['id'];

export function App() {
  const [tab, setTab] = useState<TabId>('library');
  const buttons = useRef<Partial<Record<TabId, HTMLButtonElement | null>>>({});

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
        <h1>co-op patch librarian</h1>
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
        {/* Both panels stay mounted so the connected drive and inspected file survive a tab switch. */}
        <div role="tabpanel" id="panel-library" aria-labelledby="tab-library" hidden={tab !== 'library'}>
          <Library active={tab === 'library'} />
        </div>
        <div role="tabpanel" id="panel-inspector" aria-labelledby="tab-inspector" hidden={tab !== 'inspector'}>
          <Inspector active={tab === 'inspector'} />
        </div>
      </main>
      <footer>
        <a href="https://github.com/coldsoldr/co-op">co-op on GitHub</a>
      </footer>
    </>
  );
}
