import { useState, type FormEvent, type ReactNode } from 'react';
import {
  XY_FOLDERS,
  XY_PRESETS,
  XY_PROJECTS,
  XY_SAMPLES,
  type XyFolder,
  type XyFolderTab,
  type XyStatus,
  type XyTab,
} from './opxy-sample';

const TABS: readonly XyTab[] = ['presets', 'samples', 'projects', 'packs'];
const NOUNS: Record<XyFolderTab, string> = { presets: 'preset', samples: 'sample', projects: 'project' };
/** The OP-XY's encoder greys, darkest first: the preset types, the steps and the actions use them in this order. */
const TYPE_ORDER = ['drum', 'multisampler', 'sampler', 'synth'];
const ACTIONS = ['check', 'back up', 'restore', 'eject'];
const STEPS = [
  { key: 'usb', title: 'Plug in USB-C', body: 'Use the cable that came with your OP–XY.' },
  { key: 'com', title: 'Press com', body: 'The com key opens the connection screen.' },
  { key: 'M4', title: 'Press M4', body: 'This starts MTP mode. The option only shows once a computer is connected.' },
  { key: 'go', title: 'Choose OP–XY here', body: 'co-op then reads your presets, samples and projects.' },
];

const count = (n: number, noun: string): string => `${n} ${noun}${n === 1 ? '' : 's'}`;

function Status({ status, note }: { status: XyStatus; note: string }) {
  return (
    <span className="xy-status">
      <span className={`xy-dot ${status}`} />
      {note}
    </span>
  );
}

const PlayIcon = () => (
  <svg aria-hidden="true" viewBox="0 0 24 24">
    <path d="M7 4l13 8-13 8z" />
  </svg>
);
const CloseIcon = () => (
  <svg aria-hidden="true" viewBox="0 0 24 24">
    <path d="M6 6l12 12M18 6L6 18" />
  </svg>
);

interface OpXyProps {
  /** The logo and its device menu. */
  brand: ReactNode;
  /** Feedback and the theme switch. */
  actions: ReactNode;
}

/**
 * The OP-XY design as a working page over made-up data. co-op cannot reach an OP-XY yet: its files
 * go over MTP, which a browser folder picker does not open. Nothing here touches a device.
 */
export function OpXy({ brand, actions }: OpXyProps) {
  const [connected, setConnected] = useState(false);
  const [tab, setTab] = useState<XyTab>('presets');
  const [mode, setMode] = useState<'category' | 'engine'>('category');
  const [folders, setFolders] = useState<readonly XyFolder[]>(XY_FOLDERS);
  const [presets, setPresets] = useState(XY_PRESETS);
  const [samples, setSamples] = useState(XY_SAMPLES);
  const [projects, setProjects] = useState(XY_PROJECTS);
  const [selected, setSelected] = useState<Record<XyFolderTab, string>>({ presets: 'pad', samples: 's-user', projects: 'p-user' });
  const [engine, setEngine] = useState('axis');
  const [adding, setAdding] = useState<string | null>(null);
  const [draft, setDraft] = useState('');
  const [confirming, setConfirming] = useState<string | null>(null);
  const [writing, setWriting] = useState(false);

  const items = { presets, samples, projects };
  const inFolder = (folder: XyFolder): number => items[folder.tab].filter((item) => item.folder === folder.id).length;

  function choose(folderTab: XyFolderTab, id: string): void {
    setSelected((prev) => ({ ...prev, [folderTab]: id }));
    setConfirming(null);
  }

  function addFolder(event: FormEvent, folderTab: XyFolderTab, group: string): void {
    event.preventDefault();
    const clean = draft.trim().toLowerCase();
    if (!clean) return;
    const name = `[${clean}]`;
    const same = folders.find((f) => f.tab === folderTab && f.name === name);
    const id = same?.id ?? `${folderTab}-${clean}`;
    if (!same) setFolders([...folders, { id, tab: folderTab, group, name }]);
    choose(folderTab, id);
    setAdding(null);
    setDraft('');
  }

  function removeFolder(folder: XyFolder): void {
    const rest = folders.filter((f) => f.id !== folder.id);
    setFolders(rest);
    setPresets((list) => list.filter((p) => p.folder !== folder.id));
    setSamples((list) => list.filter((p) => p.folder !== folder.id));
    setProjects((list) => list.filter((p) => p.folder !== folder.id));
    choose(folder.tab, rest.find((f) => f.tab === folder.tab)?.id ?? '');
  }

  const byEngine = tab === 'presets' && mode === 'engine';
  const shownPresets = tab !== 'presets' ? [] : presets.filter((p) => (byEngine ? p.engine === engine : p.folder === selected.presets));
  const shownSamples = tab !== 'samples' ? [] : samples.filter((s) => s.folder === selected.samples);
  const shownProjects = tab !== 'projects' ? [] : projects.filter((p) => p.folder === selected.projects);
  const empty = shownPresets.length + shownSamples.length + shownProjects.length === 0;
  const current = tab === 'packs' ? undefined : folders.find((f) => f.id === selected[tab]);
  const crumb = tab === 'packs' ? 'PACKS' : byEngine ? `ENGINE · ${engine.toUpperCase()}` : `${tab} · ${current?.name ?? ''}`.toUpperCase();

  return (
    <div className="xy">
      <header className="app-header">
        <div className="brand">
          {brand}
          <span className="eyebrow">PATCH LIBRARIAN / OP–XY</span>
        </div>
        <nav aria-label="Views" className="xy-tabs">
          {TABS.map((name, i) => (
            <button
              key={name}
              type="button"
              aria-pressed={tab === name}
              onClick={() => {
                setTab(name);
                setAdding(null);
                setConfirming(null);
              }}
            >
              <span>{i + 1}</span>
              {name}
            </button>
          ))}
        </nav>
        <div className="header-actions">{actions}</div>
      </header>

      <p className="xy-preview" role="note">
        Design preview: the contents below are made up, and co-op cannot reach an OP–XY yet.
      </p>

      {!connected && (
        <section aria-labelledby="xy-connect">
          <div className="eyebrow xy-top">01 / NOT CONNECTED</div>
          <h2 id="xy-connect">Connect OP–XY</h2>
          <ol className="xy-steps">
            {STEPS.map((step, i) => (
              <li key={step.key}>
                <span className={`xy-keycap grey-${i}`}>{step.key}</span>
                <span>
                  <strong>{step.title}</strong>
                  <span className="muted">{step.body}</span>
                </span>
              </li>
            ))}
          </ol>
          <div className="xy-connect-bar">
            <button type="button" className="primary" onClick={() => setConnected(true)}>
              Find OP–XY
            </button>
            <span className="mono muted">on a mac, OP–XY needs the field kit app</span>
          </div>
        </section>
      )}

      {connected && (
        <>
          <section className="hero" aria-labelledby="xy-connected">
            <div>
              <div className="eyebrow">01 / CONNECTED</div>
              <h2 id="xy-connected">OP–XY</h2>
              <div className="pills">
                <span className="pill mono">
                  <span className="dot" />
                  mtp mode
                </span>
                <span className="pill quiet">firmware [version]</span>
              </div>
            </div>
            <div className="keys">
              {ACTIONS.map((label, i) => (
                <button
                  key={label}
                  type="button"
                  className="key xy-encoder"
                  onClick={label === 'eject' ? () => setConnected(false) : undefined}
                >
                  <span className="xy-well">
                    <span className={`xy-knob grey-${i}`} />
                  </span>
                  {label}
                </button>
              ))}
            </div>
          </section>

          <div className="library-layout">
            <aside className="folders" aria-label="Folders">
              <div className="eyebrow rule">02 / FOLDERS</div>

              {tab === 'presets' && (
                <div role="group" aria-label="Browse presets by" className="xy-modes">
                  {(['category', 'engine'] as const).map((name) => (
                    <button key={name} type="button" aria-pressed={mode === name} onClick={() => setMode(name)}>
                      {name}
                    </button>
                  ))}
                </div>
              )}

              {byEngine && (
                <section aria-label="engine folders" className="folder-group">
                  <div className="folder-group-head">
                    <span className="mono">engine</span>
                  </div>
                  {[...new Set(presets.map((p) => p.engine))].sort().map((name) => (
                    <div key={name} className={`folder${name === engine ? ' on' : ''}`}>
                      <div className="folder-row">
                        <button type="button" className="folder-pick" aria-current={name === engine} onClick={() => setEngine(name)}>
                          <span>{name}</span>
                          <span className="mono">{count(presets.filter((p) => p.engine === name).length, 'preset')}</span>
                        </button>
                      </div>
                    </div>
                  ))}
                </section>
              )}

              {!byEngine &&
                tab !== 'packs' &&
                [...new Set(folders.filter((f) => f.tab === tab).map((f) => f.group))].map((group) => (
                  <section key={group} aria-label={`${group} folders`} className="folder-group">
                    <div className="folder-group-head">
                      <span className="mono">{group}</span>
                      {group !== 'category' && (
                        <button
                          type="button"
                          className="round-hit"
                          aria-label={`Add a ${group} folder`}
                          onClick={() => {
                            setAdding(group);
                            setDraft('');
                            setConfirming(null);
                          }}
                        >
                          <span className="ring">
                            <svg aria-hidden="true" viewBox="0 0 24 24">
                              <path d="M12 5v14M5 12h14" />
                            </svg>
                          </span>
                        </button>
                      )}
                    </div>

                    {folders
                      .filter((f) => f.tab === tab && f.group === group)
                      .map((folder) => {
                        const n = inFolder(folder);
                        const on = folder.id === selected[folder.tab];
                        const label = count(n, NOUNS[folder.tab]);
                        return (
                          <div key={folder.id} className={`folder${on ? ' on' : ''}`}>
                            <div className="folder-row">
                              <button type="button" className="folder-pick" aria-current={on} onClick={() => choose(folder.tab, folder.id)}>
                                <span>{folder.name}</span>
                                <span className="mono">{label}</span>
                              </button>
                              {on && !folder.fixed && (
                                <button
                                  type="button"
                                  className="round-hit"
                                  aria-label={`Remove folder ${folder.name}`}
                                  onClick={() => (n === 0 ? removeFolder(folder) : setConfirming(folder.id))}
                                >
                                  <CloseIcon />
                                </button>
                              )}
                            </div>
                            {confirming === folder.id && (
                              <div className="folder-confirm" role="alert">
                                <p>
                                  Remove {folder.name} and its {label} from the OP–XY?
                                </p>
                                <div>
                                  <button type="button" className="solid-danger" onClick={() => removeFolder(folder)}>
                                    Remove
                                  </button>
                                  <button type="button" className="on-ink" onClick={() => setConfirming(null)}>
                                    Keep
                                  </button>
                                </div>
                              </div>
                            )}
                          </div>
                        );
                      })}

                    {adding === group && (
                      <form className="folder-add" onSubmit={(e) => addFolder(e, tab, group)}>
                        <input
                          type="text"
                          value={draft}
                          onChange={(e) => setDraft(e.target.value)}
                          maxLength={24}
                          placeholder="folder name"
                          aria-label={`New ${group} folder name`}
                          required
                          autoFocus
                        />
                        <button type="submit" className="round-hit filled" aria-label="Add folder">
                          <svg aria-hidden="true" viewBox="0 0 24 24">
                            <path d="M5 13l4 4L19 7" />
                          </svg>
                        </button>
                        <button type="button" className="round-hit" aria-label="Cancel" onClick={() => setAdding(null)}>
                          <CloseIcon />
                        </button>
                      </form>
                    )}
                  </section>
                ))}

              <section aria-label="Storage" className="xy-storage">
                <div>
                  <span>free space</span>
                  <span className="xy-free">5.4 GB</span>
                </div>
                <div className="xy-ticks" role="img" aria-label="2.6 GB of 8 GB used: samples 1.4, presets 0.8, projects 0.4">
                  {Array.from({ length: 40 }, (_, i) => (
                    <span key={i} className={i < 7 ? 'samples' : i < 11 ? 'presets' : i < 13 ? 'projects' : ''} />
                  ))}
                </div>
                <div className="xy-legend mono muted">
                  {['samples', 'presets', 'projects'].map((name) => (
                    <span key={name}>
                      <span className={name} />
                      {name}
                    </span>
                  ))}
                </div>
              </section>
            </aside>

            <div className="library-main">
              <div className="library-bar">
                <div className="eyebrow">03 / {crumb}</div>
                <div className="xy-bar-actions">
                  <label className="search">
                    <span className="visually-hidden">Search {tab}</span>
                    <svg aria-hidden="true" viewBox="0 0 24 24">
                      <circle cx="11" cy="11" r="7" />
                      <path d="M21 21l-4.3-4.3" />
                    </svg>
                    <input type="search" placeholder={`Search ${tab}`} />
                  </label>
                  <button type="button" className="primary" onClick={() => setWriting(true)}>
                    Add {tab}
                  </button>
                </div>
              </div>

              {writing && (
                <div className="xy-writing" role="status">
                  <span className="xy-rec" />
                  <span className="mono">writing 2 of 3 · presets/pad/night choir.preset</span>
                  <button type="button" onClick={() => setWriting(false)}>
                    Stop
                  </button>
                </div>
              )}

              {empty && <p className="empty">{tab === 'packs' ? 'No community packs for OP–XY yet.' : 'Nothing in this folder yet.'}</p>}

              <div className="table-scroll">
                <div className="xy-rows">
                  {shownPresets.map((p) => (
                    <div key={p.name} className="xy-row preset">
                      <button type="button" className={`play-key xy-play grey-${TYPE_ORDER.indexOf(p.type)}`} aria-label={`Play ${p.name}`}>
                        <PlayIcon />
                      </button>
                      <div>
                        <div className="xy-name">{p.name}</div>
                        <div className="mono muted">{p.type}</div>
                      </div>
                      <div className="mono muted">{p.engine}</div>
                      <Status status={p.status} note={p.note} />
                      <span className="mono muted">{p.size}</span>
                      <button type="button" className="more" aria-label={`More actions for ${p.name}`}>
                        ···
                      </button>
                    </div>
                  ))}
                  {shownSamples.map((s) => (
                    <div key={s.name} className="xy-row sample">
                      <button type="button" className="play-key xy-play grey-2" aria-label={`Play ${s.name}`}>
                        <PlayIcon />
                      </button>
                      <div>
                        <div className="xy-name">{s.name}</div>
                        <div className="mono muted">
                          {s.format} · {s.pitch}
                        </div>
                      </div>
                      <div>
                        <div className="xy-length">
                          <span style={{ width: `${Math.min(100, s.seconds * 5)}%` }} />
                        </div>
                        <div className="mono muted">{s.seconds} s of 20 s</div>
                      </div>
                      <Status status={s.status} note={s.note} />
                      <button type="button" className="more" aria-label={`More actions for ${s.name}`}>
                        ···
                      </button>
                    </div>
                  ))}
                  {shownProjects.map((p, i) => (
                    <div key={p.name} className="xy-row project">
                      <span className="xy-keycap" aria-hidden="true">
                        {i + 1}
                      </span>
                      <div>
                        <div className="xy-name">{p.name}</div>
                        <div className="mono muted">{p.name}.xy</div>
                      </div>
                      <div className="mono muted">{p.versions}</div>
                      <span className="mono muted">{p.size}</span>
                      <button type="button" className="more" aria-label={`More actions for ${p.name}`}>
                        ···
                      </button>
                    </div>
                  ))}
                </div>
              </div>
            </div>
          </div>
        </>
      )}
    </div>
  );
}
