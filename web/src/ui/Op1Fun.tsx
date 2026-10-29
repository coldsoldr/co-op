import { useId, useState, type FormEvent } from 'react';
import { makeClient, op1FunMessage, type Op1Fun } from './useOp1Fun';
import type { Device } from './useDevice';

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
export function Op1FunTab({ op1fun }: Op1FunTabProps) {
  return (
    <section aria-labelledby="op1fun-heading">
      <h2 id="op1fun-heading">op1.fun</h2>
      <Account op1fun={op1fun} />
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
