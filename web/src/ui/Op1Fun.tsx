import { useEffect, useId, useRef, useState, type FormEvent } from 'react';
import type { Op1FunClient } from '../core/op1fun/client';
import { filterOp1FunPacks, filterOp1FunPatches } from '../core/op1fun/filter';
import { installOp1FunPatches, type FailedPatch, type InstalledPatch } from '../core/op1fun/install';
import type { Op1FunPack, Op1FunPatch } from '../core/op1fun/types';
import { makeClient, op1FunMessage, type Op1Fun } from './useOp1Fun';
import type { Device, Progress } from './useDevice';

export const OP1FUN_SITE = 'https://op1.fun';

interface Op1FunTabProps {
  op1fun: Op1Fun;
  /** The connection shared with the Library tab; installs go onto it. */
  device: Device;
  /** False while the tab is hidden. */
  active: boolean;
  onOpenLibrary: () => void;
}

/** The op1.fun tab: the account, then browsing and installing. */
export function Op1FunTab({ op1fun, device, active, onOpenLibrary }: Op1FunTabProps) {
  const sectionRef = useRef<HTMLElement>(null);
  // Hiding the tab stops any preview that is playing.
  useEffect(() => {
    if (!active) sectionRef.current?.querySelectorAll('audio').forEach((audio) => audio.pause());
  }, [active]);
  const { client, creds } = op1fun;
  return (
    <section ref={sectionRef} aria-labelledby="op1fun-heading">
      <h2 id="op1fun-heading">op1.fun</h2>
      <Account op1fun={op1fun} />
      {client && creds ? (
        <Browse
          key={`${creds.email} ${creds.token}`}
          client={client}
          initialUsername={creds.username}
          onUsername={op1fun.setUsername}
          device={device}
          onOpenLibrary={onOpenLibrary}
        />
      ) : (
        <p>Save your op1.fun email and token above to browse patches and packs.</p>
      )}
    </section>
  );
}

interface Status {
  ok: boolean;
  message: string;
}

function Account({ op1fun }: { op1fun: Op1Fun }) {
  const saved = op1fun.creds;
  const [email, setEmail] = useState(saved?.email ?? '');
  const [token, setToken] = useState(saved?.token ?? '');
  const [username, setUsername] = useState(saved?.username ?? '');
  const [remember, setRemember] = useState(saved?.remember ?? false);
  const [showToken, setShowToken] = useState(false);
  const [testing, setTesting] = useState(false);
  const [status, setStatus] = useState<Status | null>(null);
  const noteId = useId();

  const complete = email.trim() !== '' && token.trim() !== '';

  function store(): string {
    const ok = op1fun.save({ email: email.trim(), token: token.trim(), username: username.trim() }, remember);
    if (!remember) return 'Saved for this page only.';
    return ok ? 'Saved in this browser.' : 'Saved for this page only: this browser would not store them.';
  }

  function onSave(event: FormEvent): void {
    event.preventDefault();
    if (!complete) return;
    setStatus({ ok: true, message: store() });
  }

  async function onTest(): Promise<void> {
    const name = username.trim();
    if (!complete || name === '') return;
    setTesting(true);
    setStatus(null);
    try {
      const user = await makeClient({ email: email.trim(), token: token.trim() }).getUser(name);
      const where = store();
      setStatus({
        ok: true,
        message: `Connected: ${user.username || name} has ${user.patchIds.length} patch(es) and ${user.packIds.length} pack(s). ${where}`,
      });
    } catch (e) {
      setStatus({ ok: false, message: `Test failed: ${op1FunMessage(e)}` });
    } finally {
      setTesting(false);
    }
  }

  function onForget(): void {
    op1fun.forget();
    setEmail('');
    setToken('');
    setUsername('');
    setRemember(false);
    setShowToken(false);
    setStatus({ ok: true, message: 'Forgotten: the email and token are cleared from this page and this browser.' });
  }

  const profileUrl = username.trim() ? `${OP1FUN_SITE}/users/${encodeURIComponent(username.trim())}` : OP1FUN_SITE;

  return (
    <form className="panel op1fun-account" onSubmit={onSave} aria-labelledby="op1fun-account-heading">
      <h3 id="op1fun-account-heading">op1.fun account</h3>
      <p id={noteId} className="muted">
        Your token is sent only to op1.fun (and to the co-op Worker when uploading). It is stored in this browser only
        if you tick Remember. Your API token is shown on your{' '}
        <a href={profileUrl} target="_blank" rel="noreferrer">
          op1.fun profile page
        </a>{' '}
        when you are signed in.
      </p>
      <p>
        <label>
          Email{' '}
          <input type="email" autoComplete="username" value={email} onChange={(e) => setEmail(e.target.value)} />
        </label>
      </p>
      <p>
        <label>
          API token{' '}
          <input
            type={showToken ? 'text' : 'password'}
            autoComplete="off"
            spellCheck={false}
            value={token}
            onChange={(e) => setToken(e.target.value)}
            aria-describedby={noteId}
          />
        </label>{' '}
        <button type="button" aria-pressed={showToken} onClick={() => setShowToken(!showToken)}>
          {showToken ? 'Hide token' : 'Show token'}
        </button>
      </p>
      <p>
        <label>
          op1.fun username{' '}
          <input type="text" autoComplete="off" value={username} onChange={(e) => setUsername(e.target.value)} />
        </label>{' '}
        <span className="muted">(op1.fun has no "who am I" call, so testing needs your username)</span>
      </p>
      <p>
        <label>
          <input type="checkbox" checked={remember} onChange={(e) => setRemember(e.target.checked)} /> Remember on
          this device
        </label>
      </p>
      <p>
        <button type="submit" className="primary" disabled={!complete}>
          Save
        </button>{' '}
        <button type="button" onClick={() => void onTest()} disabled={!complete || username.trim() === '' || testing}>
          Test connection
        </button>{' '}
        <button type="button" onClick={onForget} disabled={!saved && !complete}>
          Forget
        </button>
      </p>
      {testing && <p role="status">Testing…</p>}
      {status && (
        <p role={status.ok ? 'status' : 'alert'} className={status.ok ? 'notice' : 'error'}>
          {status.message}
        </p>
      )}
    </form>
  );
}

const plural = (n: number, one: string, many: string): string => `${n} ${n === 1 ? one : many}`;

type View = 'patches' | 'packs';

interface Outcome {
  installed: InstalledPatch[];
  failed: FailedPatch[];
  /** Set when the install as a whole failed (for example, the drive changed). */
  error: string | null;
}

interface BrowseProps {
  client: Op1FunClient;
  initialUsername: string;
  onUsername: (username: string) => void;
  device: Device;
  onOpenLibrary: () => void;
}

/** Lists a user's patches and packs, previews them and installs them onto the open drive. */
function Browse({ client, initialUsername, onUsername, device, onOpenLibrary }: BrowseProps) {
  const [username, setUsername] = useState(initialUsername);
  const [loadedFor, setLoadedFor] = useState<string | null>(null);
  const [patches, setPatches] = useState<Op1FunPatch[] | null>(null);
  const [packs, setPacks] = useState<Op1FunPack[] | null>(null);
  const [loading, setLoading] = useState(false);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [view, setView] = useState<View>('patches');
  const [filter, setFilter] = useState('');
  const [openPack, setOpenPack] = useState<{ pack: Op1FunPack; patches: Op1FunPatch[] } | null>(null);
  const [working, setWorking] = useState(false);
  const [progress, setProgress] = useState<Progress | null>(null);
  const [outcome, setOutcome] = useState<Outcome | null>(null);
  const noticeId = useId();
  /** Bumped by every load, so a slow answer for an earlier username is dropped. */
  const loadToken = useRef(0);

  async function load(event: FormEvent): Promise<void> {
    event.preventDefault();
    const name = username.trim();
    if (name === '') return;
    const token = ++loadToken.current;
    setLoading(true);
    setLoadError(null);
    setOpenPack(null);
    const [patchList, packList] = await Promise.allSettled([client.listPatches(name), client.listPacks(name)]);
    if (token !== loadToken.current) return;
    setLoading(false);
    setLoadedFor(name);
    setPatches(patchList.status === 'fulfilled' ? patchList.value : null);
    setPacks(packList.status === 'fulfilled' ? packList.value : null);
    const failures = [patchList, packList].filter((r) => r.status === 'rejected');
    if (failures.length > 0) {
      setLoadError(`Could not load ${name} from op1.fun: ${op1FunMessage((failures[0] as PromiseRejectedResult).reason)}`);
    }
    if (failures.length < 2) onUsername(name);
  }

  async function viewPack(pack: Op1FunPack): Promise<void> {
    if (loadedFor === null) return;
    setLoading(true);
    setLoadError(null);
    try {
      setOpenPack(await client.getPack(loadedFor, pack.id));
    } catch (e) {
      setLoadError(`Could not load the pack ${pack.name}: ${op1FunMessage(e)}`);
    } finally {
      setLoading(false);
    }
  }

  async function install(list: readonly Op1FunPatch[]): Promise<void> {
    const key = device.source?.key;
    const profileId = device.profile.id;
    setWorking(true);
    setOutcome(null);
    setProgress({ done: 0, total: list.length });
    let result: Outcome = { installed: [], failed: [], error: null };
    try {
      // Downloads and writes run as one queued action, as on the Packs tab.
      await device.run(async (store, source) => {
        if (source.key !== key) throw new Error('the drive changed; install again');
        const done = await installOp1FunPatches(
          store,
          profileId,
          list,
          (url) => client.fetchFile(url),
          (count, total) => setProgress({ done: count, total }),
        );
        result = { ...done, error: null };
      });
    } catch (e) {
      result = { ...result, error: op1FunMessage(e) };
    } finally {
      setOutcome(result);
      setProgress(null);
      setWorking(false);
    }
  }

  const connected = device.source !== null;
  const staged = device.source?.staged != null;
  const blocked = !connected || device.busy || working;

  const installPanel = (
    <>
      {!connected && (
        <p className="notice">
          <span id={noticeId}>Connect an OP-1 or open its drive folder on the Library tab to install patches.</span>{' '}
          <button type="button" onClick={onOpenLibrary}>
            Go to Library
          </button>
        </p>
      )}
      {connected && (
        <p className="muted">
          Patches install into {device.profile.layout.patchRoots.map((r) => `${r.path}/op1fun`).join(' and ')} under a
          free name, so nothing on the drive is replaced.
          {staged ? ' The open drive is a read-only copy: installs are added to the changes to download from the Library tab.' : ''}
        </p>
      )}
      {progress && (
        <p role="status">
          Installing {progress.done} of {progress.total}…
        </p>
      )}
      {outcome && <InstallOutcome outcome={outcome} staged={staged} />}
    </>
  );

  const rowProps = { blocked, describedBy: connected ? undefined : noticeId, onInstall: (p: Op1FunPatch) => void install([p]) };

  return (
    <div className="op1fun-browse">
      <h3>Browse op1.fun</h3>
      <form className="toolbar" onSubmit={(e) => void load(e)}>
        <label>
          Username{' '}
          <input type="text" value={username} onChange={(e) => setUsername(e.target.value)} autoComplete="off" />
        </label>
        <button type="submit" disabled={loading || username.trim() === ''}>
          Load
        </button>
      </form>
      {loading && <p role="status">Loading from op1.fun…</p>}
      {loadError && (
        <p role="alert" className="error">
          {loadError}
        </p>
      )}

      {openPack ? (
        <div className="op1fun-pack">
          <p>
            <button type="button" onClick={() => setOpenPack(null)}>
              Back to packs
            </button>
          </p>
          <h4>{openPack.pack.name || `Pack ${openPack.pack.id}`}</h4>
          {openPack.pack.description && <p>{openPack.pack.description}</p>}
          <PatchTable caption={`Patches in ${openPack.pack.name}`} patches={openPack.patches} {...rowProps} />
          <p>
            <button
              type="button"
              className="primary"
              onClick={() => void install(openPack.patches)}
              disabled={blocked || openPack.patches.length === 0}
              aria-describedby={rowProps.describedBy}
            >
              Install all ({openPack.patches.length})
            </button>
          </p>
          {installPanel}
        </div>
      ) : (
        loadedFor !== null && (
          <>
            <div className="toolbar">
              <fieldset className="view-toggle">
                <legend>Show</legend>
                <label>
                  <input type="radio" name="op1fun-view" checked={view === 'patches'} onChange={() => setView('patches')} />{' '}
                  Patches{patches ? ` (${patches.length})` : ''}
                </label>
                <label>
                  <input type="radio" name="op1fun-view" checked={view === 'packs'} onChange={() => setView('packs')} />{' '}
                  Packs{packs ? ` (${packs.length})` : ''}
                </label>
              </fieldset>
              <label>
                Filter{' '}
                <input type="search" value={filter} onChange={(e) => setFilter(e.target.value)} placeholder="name, type, licence" />
              </label>
            </div>
            {view === 'patches' && patches && (
              <>
                <PatchTable
                  caption={`${loadedFor}'s patches`}
                  patches={filterOp1FunPatches(patches, filter)}
                  {...rowProps}
                />
                {patches.length === 0 && <p>{loadedFor} has no patches on op1.fun.</p>}
              </>
            )}
            {view === 'packs' && packs && (
              <PackTable caption={`${loadedFor}'s packs`} packs={filterOp1FunPacks(packs, filter)} onView={(p) => void viewPack(p)} />
            )}
            {view === 'patches' && installPanel}
          </>
        )
      )}
    </div>
  );
}

interface PatchTableProps {
  caption: string;
  patches: readonly Op1FunPatch[];
  blocked: boolean;
  describedBy: string | undefined;
  onInstall: (patch: Op1FunPatch) => void;
}

function PatchTable({ caption, patches, blocked, describedBy, onInstall }: PatchTableProps) {
  return (
    <table className="library op1fun">
      <caption>{caption}</caption>
      <thead>
        <tr>
          <th scope="col">Name</th>
          <th scope="col">Type</th>
          <th scope="col">Licence</th>
          <th scope="col">Preview</th>
          <th scope="col">Install</th>
        </tr>
      </thead>
      <tbody>
        {patches.map((p) => {
          const label = p.name || `patch ${p.id}`;
          return (
            <tr key={p.id} data-op1fun-patch={p.id}>
              <th scope="row" className="name">
                {p.selfUrl ? (
                  <a href={p.selfUrl} target="_blank" rel="noreferrer">
                    {label}
                  </a>
                ) : (
                  label
                )}
                {p.description && (
                  <ul className="details">
                    <li className="muted">{p.description}</li>
                  </ul>
                )}
              </th>
              <td className="kind">{p.patchType || 'unknown'}</td>
              <td>{p.license || 'none given'}</td>
              <td>
                {p.previewUrl ? (
                  // A plain media element needs no CORS, so previews play straight from op1.fun.
                  <audio controls preload="none" src={p.previewUrl} aria-label={`Preview ${label}`} />
                ) : (
                  <span className="muted">no preview</span>
                )}
              </td>
              <td>
                <button
                  type="button"
                  onClick={() => onInstall(p)}
                  disabled={blocked || !p.fileUrl}
                  aria-label={`Install ${label}`}
                  aria-describedby={describedBy}
                  title={p.fileUrl ? undefined : 'op1.fun gave no file for this patch'}
                >
                  Install
                </button>
              </td>
            </tr>
          );
        })}
      </tbody>
    </table>
  );
}

function PackTable({ caption, packs, onView }: { caption: string; packs: readonly Op1FunPack[]; onView: (pack: Op1FunPack) => void }) {
  if (packs.length === 0) return <p>No packs to show.</p>;
  return (
    <table className="library op1fun">
      <caption>{caption}</caption>
      <thead>
        <tr>
          <th scope="col">Name</th>
          <th scope="col">Patches</th>
          <th scope="col">Open</th>
        </tr>
      </thead>
      <tbody>
        {packs.map((p) => {
          const label = p.name || `pack ${p.id}`;
          return (
            <tr key={p.id}>
              <th scope="row" className="name">
                {label}
                {p.description && (
                  <ul className="details">
                    <li className="muted">{p.description}</li>
                  </ul>
                )}
              </th>
              <td>{p.patchIds.length}</td>
              <td>
                <button type="button" onClick={() => onView(p)} aria-label={`View pack ${label}`}>
                  View
                </button>
              </td>
            </tr>
          );
        })}
      </tbody>
    </table>
  );
}

function InstallOutcome({ outcome, staged }: { outcome: Outcome; staged: boolean }) {
  const { installed, failed, error } = outcome;
  const where = staged && installed.length > 0 ? ' Download them from the Library tab to copy them onto the OP-1.' : '';
  return (
    <>
      {(installed.length > 0 || (failed.length === 0 && error === null)) && (
        <div role="status" className="notice">
          <p>
            Installed {plural(installed.length, 'patch', 'patches')}.{where}
          </p>
          <ul className="file-list">
            {installed.map(({ path }) => (
              <li key={path}>
                <code>{path}</code>
              </li>
            ))}
          </ul>
        </div>
      )}
      {(failed.length > 0 || error !== null) && (
        <div role="alert" className="error">
          {error !== null && <p>Install failed: {error}</p>}
          {failed.length > 0 && (
            <>
              <p>Could not install {plural(failed.length, 'patch', 'patches')}:</p>
              <ul className="file-list">
                {failed.map(({ patch, reason }) => (
                  <li key={patch.id}>
                    {patch.name || `patch ${patch.id}`}: {reason}
                  </li>
                ))}
              </ul>
            </>
          )}
        </div>
      )}
    </>
  );
}
