/**
 * The feedback payload the web form sends and the Worker accepts, its validation, and the GitHub
 * issue it becomes. Everything a user typed is untrusted: issueTitle and issueBody neutralise
 * mentions and issue references and escape markdown and HTML so feedback can never ping people,
 * link issues or break out of the quoted block.
 */

export const FEEDBACK_CATEGORIES = ['bug', 'idea', 'pack-request', 'other'] as const;
export type FeedbackCategory = (typeof FEEDBACK_CATEGORIES)[number];

export const FEEDBACK_STORES = ['fs-access', 'drop', 'none'] as const;
export type FeedbackStore = (typeof FEEDBACK_STORES)[number];

/** Lengths are UTF-16 code units, matching a textarea's maxLength. */
export const MIN_MESSAGE = 10;
export const MAX_MESSAGE = 5000;
export const MAX_CONTACT = 200;
export const MAX_TOKEN = 2048;
/** Issue titles, counted in code points, including the category prefix and any ellipsis. */
export const MAX_TITLE = 80;

/** Bounds on the context strings so a hand-made request cannot bloat the issue. */
const CONTEXT_LIMITS = { appVersion: 50, commit: 40, profile: 100, userAgent: 500 } as const;

export interface FeedbackContext {
  /** The web package version. */
  appVersion: string;
  /** Short git sha injected at build. */
  commit: string;
  /** Active profile id. */
  profile: string | null;
  store: FeedbackStore;
  userAgent: string;
  deviceMounted: boolean;
}

export interface FeedbackPayload {
  category: FeedbackCategory;
  /** MIN_MESSAGE..MAX_MESSAGE after trimming. */
  message: string;
  /** Up to MAX_CONTACT after trimming; omitted when blank. */
  contact?: string;
  context: FeedbackContext;
  /** 1..MAX_TOKEN. Never written into the issue. */
  turnstileToken: string;
}

export type FeedbackResult = { ok: true; value: FeedbackPayload } | { ok: false; errors: string[] };

const isRecord = (v: unknown): v is Record<string, unknown> =>
  typeof v === 'object' && v !== null && !Array.isArray(v);

/** True when the string has no lone surrogates (String.prototype.isWellFormed is ES2024). */
const wellFormed = (s: string): boolean =>
  !/[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/.test(s);

const oneOf = <T extends string>(list: readonly T[], v: unknown): v is T =>
  typeof v === 'string' && (list as readonly string[]).includes(v);

function validateContext(input: unknown, errors: string[]): FeedbackContext | undefined {
  if (!isRecord(input)) {
    errors.push('context must be an object');
    return undefined;
  }
  const before = errors.length;
  const text = (key: 'appVersion' | 'commit' | 'userAgent'): string => {
    const v = input[key];
    const max = CONTEXT_LIMITS[key];
    if (typeof v !== 'string' || v.length > max) {
      errors.push(`context.${key} must be a string of at most ${max} characters`);
      return '';
    }
    if (!wellFormed(v)) {
      errors.push(`context.${key} must be valid Unicode text`);
      return '';
    }
    return v;
  };
  const appVersion = text('appVersion');
  const commit = text('commit');
  const userAgent = text('userAgent');

  const profile = input.profile;
  if (profile !== null && (typeof profile !== 'string' || profile.length > CONTEXT_LIMITS.profile)) {
    errors.push('context.profile must be a string or null');
  } else if (profile !== null && !wellFormed(profile)) {
    errors.push('context.profile must be valid Unicode text');
  }
  const store = input.store;
  if (!oneOf(FEEDBACK_STORES, store)) {
    errors.push(`context.store must be one of ${FEEDBACK_STORES.join(', ')}`);
  }
  const deviceMounted = input.deviceMounted;
  if (typeof deviceMounted !== 'boolean') errors.push('context.deviceMounted must be a boolean');

  if (errors.length > before) return undefined;
  return {
    appVersion,
    commit,
    profile: profile as string | null,
    store: store as FeedbackStore,
    userAgent,
    deviceMounted: deviceMounted as boolean,
  };
}

/** Checks untrusted input and returns a payload built only from known keys. */
export function validateFeedback(input: unknown): FeedbackResult {
  if (!isRecord(input)) return { ok: false, errors: ['feedback must be an object'] };
  const errors: string[] = [];

  const category = input.category;
  if (!oneOf(FEEDBACK_CATEGORIES, category)) {
    errors.push(`category must be one of ${FEEDBACK_CATEGORIES.join(', ')}`);
  }

  const message = typeof input.message === 'string' ? input.message.trim() : '';
  if (message.length < MIN_MESSAGE || message.length > MAX_MESSAGE) {
    errors.push(`message must be ${MIN_MESSAGE} to ${MAX_MESSAGE} characters`);
  } else if (!wellFormed(message)) {
    errors.push('message must be valid Unicode text');
  }

  let contact: string | undefined;
  if (input.contact !== undefined) {
    if (typeof input.contact !== 'string' || input.contact.trim().length > MAX_CONTACT) {
      errors.push(`contact must be at most ${MAX_CONTACT} characters`);
    } else if (!wellFormed(input.contact)) {
      errors.push('contact must be valid Unicode text');
    } else if (input.contact.trim() !== '') {
      contact = input.contact.trim();
    }
  }

  const context = validateContext(input.context, errors);

  const token = input.turnstileToken;
  if (typeof token !== 'string' || token.length < 1 || token.length > MAX_TOKEN) {
    errors.push(`turnstileToken must be 1 to ${MAX_TOKEN} characters`);
  } else if (!wellFormed(token)) {
    errors.push('turnstileToken must be valid Unicode text');
  }

  if (errors.length > 0 || context === undefined) return { ok: false, errors };
  const value: FeedbackPayload = {
    category: category as FeedbackCategory,
    message,
    context,
    turnstileToken: token as string,
  };
  if (contact !== undefined) value.contact = contact;
  return { ok: true, value };
}

const ZWSP = '\u200B';

/**
 * Puts a zero-width space after '@' and before the number in '#123' and 'GH-123', so GitHub
 * neither notifies users or teams nor links or cross-references issues. Like GitHub, an '@' right
 * after a letter, digit or underscore is not a mention, so email addresses stay copyable.
 * 'github.com' (any case) is broken too, so full issue, pull request and commit URLs do not
 * cross-reference. Idempotent.
 */
export function neutraliseMentions(text: string): string {
  return text
    .replace(/(?<![A-Za-z0-9_])@(?=[A-Za-z0-9])/g, `@${ZWSP}`)
    .replace(/#(?=\d)/g, `#${ZWSP}`)
    .replace(/\b(GH-)(?=\d)/gi, `$1${ZWSP}`)
    .replace(/github(?=\.com\b)/gi, `$&${ZWSP}`);
}

const LINE_BREAK = /\r\n|[\r\n\u2028\u2029]/;
/** C0 and C1 controls and the bidi embedding, override and isolate characters. */
// eslint-disable-next-line no-control-regex
const CONTROL = /[\u0000-\u001F\u007F-\u009F\u202A-\u202E\u2066-\u2069]/g;

/**
 * Escapes the characters that could open HTML, code spans or fences, or split table cells. Hex
 * entities on purpose: a decimal one such as '&#96;' would read as an issue reference.
 */
const escapeMarkdown = (text: string): string =>
  text
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/`/g, '&#x60;')
    .replace(/\\/g, '&#x5C;')
    .replace(/\|/g, '&#x7C;');

/** A user value on a single line: safe in a table cell or after a label. */
const inline = (text: string): string =>
  escapeMarkdown(neutraliseMentions(text.replace(CONTROL, ' ').replace(/\s+/g, ' ').trim()));

/** `[category] first line`, at most MAX_TITLE code points with an ellipsis when cut. */
export function issueTitle(p: FeedbackPayload): string {
  const firstLine = p.message.split(LINE_BREAK)[0] ?? '';
  const line = neutraliseMentions(
    firstLine
      .replace(/\t/g, ' ')
      .replace(CONTROL, '')
      .trim(),
  );
  const title = Array.from(`[${p.category}] ${line}`);
  if (title.length <= MAX_TITLE) return title.join('');
  return `${title.slice(0, MAX_TITLE - 1).join('').trimEnd()}…`;
}

/** The issue markdown: the message quoted line by line, the contact, and a context table. */
export function issueBody(p: FeedbackPayload): string {
  const quoted = p.message
    .split(LINE_BREAK)
    .map((line) => escapeMarkdown(neutraliseMentions(line.replace(CONTROL, (c) => (c === '\t' ? c : '')))))
    .map((line) => (line.trim() === '' ? '>' : `> ${line}`));
  const c = p.context;
  const rows: [string, string][] = [
    ['App version', c.appVersion],
    ['Commit', c.commit],
    ['Profile', c.profile ?? 'none'],
    ['Store', c.store],
    ['Device mounted', c.deviceMounted ? 'yes' : 'no'],
    ['User agent', c.userAgent],
  ];
  return [
    `**Category:** ${p.category}`,
    '',
    '### Message',
    '',
    ...quoted,
    '',
    `**Contact:** ${p.contact === undefined ? 'not given' : inline(p.contact)}`,
    '',
    '<details><summary>Context</summary>',
    '',
    '| Field | Value |',
    '| --- | --- |',
    ...rows.map(([field, value]) => `| ${field} | ${inline(value) || '(empty)'} |`),
    '',
    '</details>',
    '',
    '<sub>Sent with the co-op feedback form.</sub>',
    '',
  ].join('\n');
}
