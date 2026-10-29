import { useEffect, useId, useRef, useState } from 'react';
import {
  FEEDBACK_CATEGORIES,
  MAX_CONTACT,
  MAX_MESSAGE,
  MIN_MESSAGE,
  type FeedbackCategory,
  type FeedbackStore,
} from '@co-op/shared/feedback';
import {
  FeedbackError,
  buildConfig,
  feedbackAvailable,
  feedbackContext,
  githubIssueUrl,
  sendFeedback,
} from '../core/feedback/client';
import { Dialog } from './Dialog';
import type { Device } from './useDevice';

const CATEGORY_LABELS: Record<FeedbackCategory, string> = {
  bug: 'Bug',
  idea: 'Idea',
  'pack-request': 'Pack request',
  other: 'Other',
};

const TURNSTILE_SCRIPT = 'https://challenges.cloudflare.com/turnstile/v0/api.js?render=explicit';

interface Turnstile {
  render(
    container: HTMLElement,
    options: {
      sitekey: string;
      callback: (token: string) => void;
      'expired-callback': () => void;
      'error-callback': () => void;
    },
  ): string;
  reset(id: string): void;
  remove(id: string): void;
}

let turnstileLoad: Promise<Turnstile> | null = null;

/** Loads the Turnstile script once, on first use. */
function loadTurnstile(): Promise<Turnstile> {
  turnstileLoad ??= new Promise<Turnstile>((resolve, reject) => {
    const script = document.createElement('script');
    script.src = TURNSTILE_SCRIPT;
    script.async = true;
    script.onload = () => {
      const api = (window as unknown as { turnstile?: Turnstile }).turnstile;
      if (api) resolve(api);
      else reject(new Error('Turnstile did not load'));
    };
    script.onerror = () => {
      turnstileLoad = null;
      reject(new Error('Turnstile did not load'));
    };
    document.head.appendChild(script);
  });
  return turnstileLoad;
}

/** The verification widget; reports a token, or null when it expires or fails. */
function TurnstileWidget({
  siteKey,
  onToken,
  resetKey,
}: {
  siteKey: string;
  onToken: (token: string | null) => void;
  resetKey: number;
}) {
  const container = useRef<HTMLDivElement>(null);
  const widget = useRef<{ api: Turnstile; id: string } | null>(null);
  const [failed, setFailed] = useState(false);
  const report = useRef(onToken);
  useEffect(() => {
    report.current = onToken;
  });

  useEffect(() => {
    let cancelled = false;
    loadTurnstile().then(
      (api) => {
        if (cancelled || !container.current) return;
        const id = api.render(container.current, {
          sitekey: siteKey,
          callback: (token) => report.current(token),
          'expired-callback': () => report.current(null),
          'error-callback': () => report.current(null),
        });
        widget.current = { api, id };
      },
      () => {
        if (!cancelled) setFailed(true);
      },
    );
    return () => {
      cancelled = true;
      if (widget.current) widget.current.api.remove(widget.current.id);
      widget.current = null;
    };
  }, [siteKey]);

  // A token is single use: after each attempt the parent bumps resetKey for a fresh one.
  useEffect(() => {
    if (resetKey > 0 && widget.current) widget.current.api.reset(widget.current.id);
  }, [resetKey]);

  return (
    <>
      <div ref={container} />
      {failed && <p className="error">The verification check could not load. Try again later.</p>}
    </>
  );
}

function storeOf(device: Device): FeedbackStore {
  if (device.source === null) return 'none';
  return device.source.staged ? 'drop' : 'fs-access';
}

type Result = { kind: 'sent'; url: string; number: number } | { kind: 'failed'; errors: string[] } | null;

function FeedbackDialog({ device, onClose }: { device: Device; onClose: () => void }) {
  const available = feedbackAvailable();
  const [category, setCategory] = useState<FeedbackCategory>('bug');
  const [message, setMessage] = useState('');
  const [contact, setContact] = useState('');
  const [token, setToken] = useState<string | null>(null);
  const [resetKey, setResetKey] = useState(0);
  const [sending, setSending] = useState(false);
  const [result, setResult] = useState<Result>(null);
  const counterId = useId();

  const context = feedbackContext({
    profile: device.profile.id,
    store: storeOf(device),
    deviceMounted: device.source !== null,
  });
  const length = message.trim().length;
  const messageOk = length >= MIN_MESSAGE && length <= MAX_MESSAGE;

  async function send(): Promise<void> {
    if (!token) return;
    setSending(true);
    setResult(null);
    try {
      const payload = { category, message: message.trim(), context, turnstileToken: token };
      const sent = await sendFeedback(contact.trim() ? { ...payload, contact: contact.trim() } : payload);
      setResult({ kind: 'sent', ...sent });
    } catch (error) {
      const errors =
        error instanceof FeedbackError
          ? error.status === 429
            ? ['Too much feedback from here just now. Try again in a minute.']
            : error.errors
          : ['Something went wrong sending feedback.'];
      setResult({ kind: 'failed', errors });
      setToken(null);
      setResetKey((k) => k + 1);
    } finally {
      setSending(false);
    }
  }

  if (result?.kind === 'sent') {
    return (
      <Dialog title="Feedback sent" onClose={onClose}>
        <p role="status">
          Thank you. Your feedback is{' '}
          <a href={result.url} target="_blank" rel="noreferrer">
            issue #{result.number}
          </a>
          .
        </p>
        <div className="dialog-buttons">
          <button type="button" onClick={onClose} autoFocus>
            Close
          </button>
        </div>
      </Dialog>
    );
  }

  const rows: [string, string][] = [
    ['App version', context.appVersion],
    ['Commit', context.commit],
    ['Profile', context.profile ?? 'none'],
    ['Store', context.store],
    ['Device mounted', context.deviceMounted ? 'yes' : 'no'],
    ['User agent', context.userAgent],
  ];

  return (
    <Dialog title="Send feedback" onClose={onClose}>
      <form
        className="feedback"
        onSubmit={(event) => {
          event.preventDefault();
          void send();
        }}
      >
        <label>
          Category
          <select value={category} onChange={(e) => setCategory(e.target.value as FeedbackCategory)}>
            {FEEDBACK_CATEGORIES.map((c) => (
              <option key={c} value={c}>
                {CATEGORY_LABELS[c]}
              </option>
            ))}
          </select>
        </label>
        <label>
          Message
          <textarea
            value={message}
            onChange={(e) => setMessage(e.target.value)}
            rows={6}
            maxLength={MAX_MESSAGE}
            aria-describedby={counterId}
            required
          />
          <span id={counterId} className="muted">
            {length} / {MAX_MESSAGE} characters{length < MIN_MESSAGE ? `, at least ${MIN_MESSAGE}` : ''}
          </span>
        </label>
        {available && (
          <label>
            Contact (optional, shown publicly on the issue)
            <input type="text" value={contact} onChange={(e) => setContact(e.target.value)} maxLength={MAX_CONTACT} />
          </label>
        )}
        <details>
          <summary>What is attached</summary>
          <p className="muted">
            {available ? 'This goes into a public GitHub issue' : 'The issue is prefilled with this'}, along with your
            message. Nothing else is sent.
          </p>
          <table className="feedback-context">
            <tbody>
              {rows.map(([field, value]) => (
                <tr key={field}>
                  <th scope="row">{field}</th>
                  <td>{value}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </details>
        {available ? (
          <>
            <TurnstileWidget siteKey={buildConfig.siteKey ?? ''} onToken={setToken} resetKey={resetKey} />
            {result?.kind === 'failed' && (
              <ul className="error" role="alert">
                {result.errors.map((e) => (
                  <li key={e}>{e}</li>
                ))}
              </ul>
            )}
            <div className="dialog-buttons">
              <button type="button" onClick={onClose}>
                Cancel
              </button>
              <button type="submit" className="primary" disabled={!messageOk || !token || sending}>
                {sending ? 'Sending…' : 'Send'}
              </button>
            </div>
          </>
        ) : (
          <>
            <p className="muted">
              Feedback opens a new GitHub issue with this filled in. You need a GitHub account to submit it.
            </p>
            <div className="dialog-buttons">
              <button type="button" onClick={onClose}>
                Cancel
              </button>
              {messageOk ? (
                <a
                  className="button primary"
                  href={githubIssueUrl(category, message, context)}
                  target="_blank"
                  rel="noreferrer"
                >
                  Open GitHub issue
                </a>
              ) : (
                <button type="button" className="primary" disabled>
                  Open GitHub issue
                </button>
              )}
            </div>
          </>
        )}
      </form>
    </Dialog>
  );
}

/** The header's Feedback button and its dialog. */
export function Feedback({ device }: { device: Device }) {
  const [open, setOpen] = useState(false);
  const button = useRef<HTMLButtonElement>(null);
  const wasOpen = useRef(false);
  // Focus returns to the button once the modal has gone; while it is open the page is inert.
  useEffect(() => {
    if (wasOpen.current && !open) button.current?.focus();
    wasOpen.current = open;
  }, [open]);
  return (
    <>
      <button type="button" ref={button} onClick={() => setOpen(true)}>
        Feedback
      </button>
      {open && (
        <FeedbackDialog
          device={device}
          onClose={() => setOpen(false)}
        />
      )}
    </>
  );
}
