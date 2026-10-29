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
  buildConfig,
  feedbackAvailable,
  feedbackContext,
  feedbackErrorMessages,
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

/**
 * Loads the Turnstile script once, on first use. A failed load removes its script element and
 * forgets the promise, so opening the dialog again retries cleanly.
 */
function loadTurnstile(): Promise<Turnstile> {
  turnstileLoad ??= new Promise<Turnstile>((resolve, reject) => {
    const script = document.createElement('script');
    script.src = TURNSTILE_SCRIPT;
    script.async = true;
    const fail = () => {
      script.remove();
      turnstileLoad = null;
      reject(new Error('Turnstile did not load'));
    };
    script.onload = () => {
      const api = (window as unknown as { turnstile?: Turnstile }).turnstile;
      if (api) resolve(api);
      else fail();
    };
    script.onerror = fail;
    document.head.appendChild(script);
  });
  return turnstileLoad;
}

const LOAD_FAILED =
  'The verification check could not load. Check your connection, or allow challenges.cloudflare.com if a blocker stops it, then close and reopen this dialog.';
const CHECK_FAILED =
  'The verification check failed. Reload the page and try again; privacy extensions and strict tracking protection can make it fail.';

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
  const [problem, setProblem] = useState<string | null>(null);
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
          callback: (token) => {
            setProblem(null);
            report.current(token);
          },
          'expired-callback': () => report.current(null),
          'error-callback': () => {
            setProblem(CHECK_FAILED);
            report.current(null);
          },
        });
        widget.current = { api, id };
      },
      () => {
        if (!cancelled) setProblem(LOAD_FAILED);
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
      {problem && (
        <p className="error" role="alert">
          {problem}
        </p>
      )}
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
  const alertRef = useRef<HTMLDivElement>(null);
  // While sending, Escape and Cancel do nothing, so the result is never lost.
  const close = sending ? () => undefined : onClose;

  const context = feedbackContext({
    profile: device.profile.id,
    store: storeOf(device),
    deviceMounted: device.source !== null,
  });
  const length = message.trim().length;
  const messageOk = length >= MIN_MESSAGE && length <= MAX_MESSAGE;

  // After a failed send, focus moves to the errors so they are read out and easy to find.
  useEffect(() => {
    if (result?.kind === 'failed') alertRef.current?.focus();
  }, [result]);

  async function send(): Promise<void> {
    if (!token) return;
    setSending(true);
    setResult(null);
    try {
      const payload = { category, message: message.trim(), context, turnstileToken: token };
      const sent = await sendFeedback(contact.trim() ? { ...payload, contact: contact.trim() } : payload);
      setResult({ kind: 'sent', ...sent });
    } catch (error) {
      setResult({ kind: 'failed', errors: feedbackErrorMessages(error) });
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
    <Dialog title="Send feedback" onClose={close}>
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
        </label>
        <p id={counterId} className="muted counter">
          {length} / {MAX_MESSAGE} characters{length < MIN_MESSAGE ? `, at least ${MIN_MESSAGE}` : ''}
        </p>
        {available && (
          <label>
            Contact (optional, shown publicly on the issue)
            <input type="text" value={contact} onChange={(e) => setContact(e.target.value)} maxLength={MAX_CONTACT} />
          </label>
        )}
        <details>
          <summary>What is attached</summary>
          {available ? (
            <p className="muted">
              Your message, your contact if you give one, and this table go into a public GitHub issue. The
              verification check runs through Cloudflare Turnstile, which sees your IP address and browser. The co-op
              Worker uses your IP address only for rate limiting and keeps nothing.
            </p>
          ) : (
            <p className="muted">The new issue is prefilled with your message and this table. Nothing is sent until you submit it on GitHub.</p>
          )}
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
              <div role="alert" tabIndex={-1} ref={alertRef}>
                <ul className="error">
                  {result.errors.map((e) => (
                    <li key={e}>{e}</li>
                  ))}
                </ul>
              </div>
            )}
            <div className="dialog-buttons">
              <button type="button" onClick={close} disabled={sending}>
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
