import { useEffect, useId, useRef, useState, type KeyboardEvent } from 'react';
import { MAX_NOTE, MAX_REQUEST_CONTACT, parseRepo, type RequestFirmware } from '@co-op/shared/plugin-request';
import { feedbackAvailable, buildConfig, FeedbackError } from '../core/feedback/client';
import { OP1R_PATCHER_URL } from '../core/plugins/catalogue';
import { requestIssueUrl, sendPluginRequest } from '../core/plugins/request';
import { PLUGIN_PROMPT } from '../core/plugins/submission';
import { TurnstileWidget } from './Feedback';
import { RELEASE_GUIDE_HASH } from './pages';

const REPO = 'https://github.com/coldsoldr/co-op';
const GUIDE_URL = `${REPO}/blob/main/docs/contributing-plugins.md`;
const EXAMPLE_URL = `${REPO}/tree/main/plugins/bitcrusher`;
const LISTED_EXAMPLE_URL = `${REPO}/tree/main/plugins/erdtree`;
const REQUEST_URL = `${REPO}/issues/new?template=plugin-request.yml`;
const CLAUDE_CODE_URL = 'https://claude.com/claude-code';
const PORT_GUIDE_URL = `${REPO}/blob/main/firmware/guide/plugin-guidelines.md`;

/** The firmwares a plugin can be submitted for, each with its own way in. */
const FIRMWARES = [
  { id: 'op1/hundekraft', label: 'hundekraft', file: '.knl', request: 'hundekraft' },
  { id: 'op1/opus', label: 'opus', file: '.opmod', request: 'opus' },
  { id: 'op1/coop', label: 'coop', file: '.coop', request: 'coop' },
  { id: 'op1/op1r', label: 'op1r', file: '.op1p', request: 'op1r' },
] as const;
type Firmware = (typeof FIRMWARES)[number];

const requestFor = (firmware: Firmware): string => `${REQUEST_URL}&firmware=${encodeURIComponent(firmware.request)}`;

interface PluginSubmitProps {
  onBack: () => void;
  /** The site's firmware: its tab opens first. */
  firmwareId?: string | undefined;
}

/** How to get a plugin into the library: one tab per firmware, each with its own instructions. */
export function PluginSubmit({ onBack, firmwareId }: PluginSubmitProps) {
  const [chosen, setChosen] = useState<Firmware>(FIRMWARES.find((f) => f.id === firmwareId) ?? FIRMWARES[0]);
  const back = useRef<HTMLButtonElement>(null);
  const tabs = useRef<Partial<Record<string, HTMLButtonElement | null>>>({});

  useEffect(() => back.current?.focus(), []);

  function onKeyDown(event: KeyboardEvent<HTMLDivElement>): void {
    const index = FIRMWARES.findIndex((f) => f.id === chosen.id);
    const last = FIRMWARES.length - 1;
    const next =
      event.key === 'ArrowRight' ? (index + 1) % FIRMWARES.length
      : event.key === 'ArrowLeft' ? (index + last) % FIRMWARES.length
      : event.key === 'Home' ? 0
      : event.key === 'End' ? last
      : null;
    if (next === null) return;
    event.preventDefault();
    const firmware = FIRMWARES[next] as Firmware;
    setChosen(firmware);
    tabs.current[firmware.id]?.focus();
  }

  const panelId = (f: Firmware): string => `submit-panel-${f.label}`;
  const tabId = (f: Firmware): string => `submit-tab-${f.label}`;

  return (
    <div className="plugin-submit">
      <p>
        <button ref={back} type="button" onClick={onBack}>
          Back to plugins
        </button>
      </p>
      <p>
        A plugin runs as firmware code on the OP-1, so a maintainer decides each one: only from an author they trust, or
        after looking at its source. Which firmware is your plugin for?
      </p>
      <div role="tablist" aria-label="Firmware" className="tabs submit-tabs" onKeyDown={onKeyDown}>
        {FIRMWARES.map((f) => (
          <button
            key={f.id}
            ref={(el) => {
              tabs.current[f.id] = el;
            }}
            type="button"
            role="tab"
            id={tabId(f)}
            aria-selected={chosen.id === f.id}
            aria-controls={panelId(f)}
            tabIndex={chosen.id === f.id ? 0 : -1}
            onClick={() => setChosen(f)}
          >
            {f.label}
          </button>
        ))}
      </div>
      <div role="tabpanel" id={panelId(chosen)} aria-labelledby={tabId(chosen)}>
        {chosen.id === 'op1/coop' ? <CoopSteps /> : <FromYourRepo firmware={chosen} />}
      </div>
      <p>
        <a href={GUIDE_URL}>Read the full guide</a>.
      </p>
    </div>
  );
}

/** hundekraft, opus and op1r: the plugin stays in its author's repo, and co-op lists it from there. */
function FromYourRepo({ firmware }: { firmware: Firmware }) {
  const hundekraft = firmware.id === 'op1/hundekraft';
  const op1r = firmware.id === 'op1/op1r';
  return (
    <>
      <p>
        co-op lists {firmware.label} plugins under their author and links to your repo. It hosts no copy: the{' '}
        <a href={LISTED_EXAMPLE_URL}>erdtree listing</a> is an example.
      </p>

      <h3>What your repo needs</h3>
      <ul>
        <li>A public GitHub repo with a README that says what the plugin does, who made it, and which {firmware.label} version it needs. For a port, credit the original.</li>
        <li>A licence file.</li>
        <li>A picture of the plugin’s screen in the README. Without one, its card gets a plain name card.</li>
        <li>For updates: a GitHub release per version, with the built file replaced at the same path.</li>
        <li>
          The built <code>{hundekraft ? '<id>.knl' : op1r ? '<name>.op1p' : '<name>.opmod'}</code> committed in the repo, not
          only inside a zip or a release.{' '}
          {hundekraft ? (
            <>
              Then its card gets <strong>Install over USB</strong>: co-op fetches the file from your repo, checks it, and
              sends it to the OP-1.
            </>
          ) : op1r ? (
            <>
              Then its card gets a <strong>Download</strong>: co-op fetches the file from your repo and checks it, and
              people install it with{' '}
              <a href={OP1R_PATCHER_URL} target="_blank" rel="noopener noreferrer">
                op1r’s web patcher
              </a>
              . co-op reads its name, kind and author from the file’s header.
            </>
          ) : (
            <>Then co-op can install it from your repo once opus installs from an author’s repo are supported; until then the card links to your repo.</>
          )}
        </li>
      </ul>

      <h3>Send it in</h3>
      <p>
        Paste your plugin’s GitHub link. co-op reads your repo and drafts the listing under your name, and a maintainer
        checks it before it goes up. You get a link to follow along.
      </p>
      <SendRepo firmware={firmware} />

      <details className="submit-hosted">
        <summary>Or write the listing yourself with Claude Code</summary>
        <ol>
          <li>
            Fork <a href={REPO}>coldsoldr/co-op</a> on GitHub and clone your fork.
          </li>
          <li>
            Open <a href={CLAUDE_CODE_URL}>Claude Code</a> in it and run <code>/add-plugin https://github.com/you/your-plugin</code>.
            It reads your repo and writes <code>plugins/&lt;id&gt;/</code>: the listing, its manual and screenshot, under
            your name, and checks them.
          </li>
          <li>Read what it wrote against how the plugin really behaves, then push the branch and open a pull request.</li>
        </ol>
      </details>

      <h3>Updates</h3>
      <p>
        Once you’re listed, co-op checks your repo every day and proposes your new versions for a maintainer to merge. The{' '}
        <a href={RELEASE_GUIDE_HASH}>release guide</a> shows how to publish them so it can.
      </p>

      {hundekraft && <HostedFolder />}
    </>
  );
}

/** What to show for a failed send. */
function requestErrors(error: unknown): string[] {
  if (!(error instanceof FeedbackError)) return ['Something went wrong sending it.'];
  if (error.status === 429) return ['Too many submissions from here just now. Try again in a minute.'];
  return error.errors;
}

type Sent = { url: string; number: number };

/**
 * The repo link form. With the Worker it files the request itself, behind Turnstile, with no GitHub
 * account; without it, the link opens GitHub's issue form with the repo filled in.
 */
function SendRepo({ firmware }: { firmware: Firmware }) {
  const request: RequestFirmware = firmware.request === 'coop' ? 'unsure' : firmware.request;
  const available = feedbackAvailable();
  const [repo, setRepo] = useState('');
  const [note, setNote] = useState('');
  const [contact, setContact] = useState('');
  const [token, setToken] = useState<string | null>(null);
  const [resetKey, setResetKey] = useState(0);
  const [sending, setSending] = useState(false);
  const [sent, setSent] = useState<Sent | null>(null);
  const [failed, setFailed] = useState<string[] | null>(null);
  const hintId = useId();
  const alertRef = useRef<HTMLDivElement>(null);
  const parsed = parseRepo(repo);
  const repoOk = parsed !== null && parsed.toLowerCase() !== 'coldsoldr/co-op';

  useEffect(() => {
    if (failed) alertRef.current?.focus();
  }, [failed]);

  async function send(): Promise<void> {
    if (!token || !parsed) return;
    setSending(true);
    setFailed(null);
    try {
      const payload = { repo: parsed, firmware: request, turnstileToken: token };
      setSent(
        await sendPluginRequest({
          ...payload,
          ...(note.trim() ? { note: note.trim() } : {}),
          ...(contact.trim() ? { contact: contact.trim() } : {}),
        }),
      );
    } catch (error) {
      setFailed(requestErrors(error));
      setToken(null);
      setResetKey((k) => k + 1);
    } finally {
      setSending(false);
    }
  }

  if (sent) {
    return (
      <p role="status" className="submit-sent">
        Thank you. It is{' '}
        <a href={sent.url} target="_blank" rel="noreferrer">
          request #{sent.number}
        </a>
        : the draft listing and its pull request will be linked there in a few minutes.
      </p>
    );
  }

  const repoField = (
    <>
      <label className="submit-field">
        Your plugin’s GitHub repo
        <input
          type="url"
          inputMode="url"
          value={repo}
          onChange={(e) => setRepo(e.target.value)}
          placeholder="https://github.com/you/your-plugin"
          aria-describedby={hintId}
          required
        />
      </label>
      <p id={hintId} className="muted">
        {repo.trim() === '' || repoOk ? 'A link to the repo, or to the folder or release the plugin is in.' : 'That isn’t a GitHub repo link.'}
      </p>
    </>
  );

  if (!available) {
    return (
      <div className="submit-form">
        {repoField}
        <p>
          {repoOk ? (
            <a className="button primary" href={requestIssueUrl(request, repo)} target="_blank" rel="noreferrer">
              Offer your plugin in an issue
            </a>
          ) : (
            <a className="button" href={requestIssueUrl(request)} target="_blank" rel="noreferrer">
              Offer your plugin in an issue
            </a>
          )}
        </p>
        <p className="muted">This opens GitHub’s form with your link filled in. You need a GitHub account to send it.</p>
      </div>
    );
  }

  return (
    <form
      className="submit-form"
      onSubmit={(event) => {
        event.preventDefault();
        void send();
      }}
    >
      {repoField}
      <label className="submit-field">
        Anything we should know (optional)
        <textarea value={note} onChange={(e) => setNote(e.target.value)} rows={3} maxLength={MAX_NOTE} />
      </label>
      <label className="submit-field">
        Contact (optional, shown publicly on the issue)
        <input type="text" value={contact} onChange={(e) => setContact(e.target.value)} maxLength={MAX_REQUEST_CONTACT} />
      </label>
      <p className="muted">
        Your link, note and contact go into a public GitHub issue. The check below runs through Cloudflare Turnstile; the
        co-op Worker uses your IP address only for rate limiting and keeps nothing.
      </p>
      <TurnstileWidget siteKey={buildConfig.siteKey ?? ''} onToken={setToken} resetKey={resetKey} />
      {failed && (
        <div role="alert" tabIndex={-1} ref={alertRef}>
          <ul className="error">
            {failed.map((e) => (
              <li key={e}>{e}</li>
            ))}
          </ul>
        </div>
      )}
      <p>
        <button type="submit" className="primary" disabled={!repoOk || !token || sending}>
          {sending ? 'Sending…' : 'Send it in'}
        </button>
      </p>
      <p className="muted">
        No luck here? <a href={requestIssueUrl(request, repo)}>Open the GitHub form instead</a>.
      </p>
    </form>
  );
}

/** The older way for a hundekraft plugin: the whole folder, with the file, hosted by co-op. */
function HostedFolder() {
  return (
    <details className="submit-hosted">
      <summary>Or send the plugin folder itself, for co-op to host</summary>
      <HostedSteps />
    </details>
  );
}

/** coop: plugins are built in co-op's own firmware folder, and co-op hosts the .coop file. */
function CoopSteps() {
  return (
    <>
      <p>
        coop plugins are built in co-op’s own firmware, in the <code>firmware/</code> folder of the repo, and co-op hosts the
        built <code>&lt;id&gt;.coop</code>. Start from <a href={PORT_GUIDE_URL}>the plugin guidelines</a>; to bring a
        hundekraft or opus plugin over, run <code>/port &lt;plugin&gt;</code> in Claude Code from a clone of the repo.
      </p>
      <h3>Send it in</h3>
      <ol>
        <li>Build the plugin and pass its gate as the guidelines describe.</li>
        <li>
          Add its folder to <code>plugins/</code>: <code>plugin.json</code>, <code>&lt;id&gt;.coop</code>,{' '}
          <code>manual.md</code> and <code>screenshot.png</code>.
        </li>
        <li>Open a pull request; a check runs on it and says if anything needs fixing.</li>
      </ol>
      <p>
        Not ready to build it? <a href={requestFor(FIRMWARES[2])}>Suggest it in an issue</a>.
      </p>
    </>
  );
}

/** The folder co-op hosts: its four files, the prompt for an author's agent, and the pull request. */
function HostedSteps() {
  const [copied, setCopied] = useState<'yes' | 'failed' | null>(null);
  function copy(): void {
    navigator.clipboard.writeText(PLUGIN_PROMPT).then(
      () => setCopied('yes'),
      () => setCopied('failed'),
    );
  }
  return (
    <>
      <h3>What your plugin needs</h3>
      <p>One folder named after the plugin’s id, with exactly four files:</p>
      <ul className="file-list">
        <li>
          <code>plugin.json</code>: its name, author, kind, version, licence, description and a link to the source
        </li>
        <li>
          <code>&lt;id&gt;.knl</code>: the built plugin (<code>.op1p</code> for op1r)
        </li>
        <li>
          <code>manual.md</code>: the manual, with the same sections as every other plugin
        </li>
        <li>
          <code>screenshot.png</code>: its screen on black, 960 by 480 pixels
        </li>
      </ul>
      <p>
        The <a href={EXAMPLE_URL}>bitcrusher folder</a> is a complete example.
      </p>

      <h3>Let your coding agent prepare it</h3>
      <p>
        Paste this into your coding agent from the folder that holds the plugin’s source. It prepares the folder so your
        plugin looks like the rest of the library. Check what it writes against how the plugin really behaves.
      </p>
      <p>
        <button type="button" className="primary" onClick={copy}>
          Copy the prompt
        </button>{' '}
        <span role="status">
          {copied === 'yes' && 'Copied.'}
          {copied === 'failed' && 'Could not copy. Select the text below instead.'}
        </span>
      </p>
      <pre className="prompt" tabIndex={0} aria-label="The prompt for your coding agent">
        {PLUGIN_PROMPT}
      </pre>

      <h3>Send the folder</h3>
      <ol>
        <li>
          Fork <a href={REPO}>coldsoldr/co-op</a> on GitHub.
        </li>
        <li>
          In your fork, open the <code>plugins</code> folder, choose Add file, then Upload files, and drag your whole folder
          onto the page. Commit it to a new branch.
        </li>
        <li>Open a pull request and fill in the checklist, including how you built the file.</li>
        <li>A check runs on the pull request and says if anything needs fixing.</li>
      </ol>
    </>
  );
}

