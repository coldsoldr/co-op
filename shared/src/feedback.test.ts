import { describe, it, expect } from 'vitest';
import {
  FEEDBACK_CATEGORIES,
  MAX_CONTACT,
  MAX_MESSAGE,
  MAX_TITLE,
  MAX_TOKEN,
  MIN_MESSAGE,
  issueBody,
  issueTitle,
  neutraliseMentions,
  validateFeedback,
  type FeedbackPayload,
} from './feedback';

const ZWSP = '\u200B';

const context = (): Record<string, unknown> => ({
  appVersion: '0.5.0',
  commit: 'abc1234',
  profile: 'op1/stock',
  store: 'fs-access',
  userAgent: 'Mozilla/5.0 (Windows NT 10.0) Chrome/130',
  deviceMounted: true,
});

const input = (overrides: Record<string, unknown> = {}): Record<string, unknown> => ({
  category: 'bug',
  message: 'The drum view crashes when I open it.',
  contact: 'me@example.com',
  context: context(),
  turnstileToken: 'token-abc',
  ...overrides,
});

const payload = (overrides: Record<string, unknown> = {}): FeedbackPayload => {
  const result = validateFeedback(input(overrides));
  if (!result.ok) throw new Error(result.errors.join('; '));
  return result.value;
};

const errorsFor = (value: unknown): string[] => {
  const result = validateFeedback(value);
  if (result.ok) throw new Error('expected validation to fail');
  return result.errors;
};

describe('validateFeedback', () => {
  it('accepts a valid payload', () => {
    const result = validateFeedback(input());
    expect(result).toEqual({ ok: true, value: input() });
  });

  it('accepts every category', () => {
    for (const category of FEEDBACK_CATEGORIES) {
      expect(validateFeedback(input({ category })).ok).toBe(true);
    }
  });

  it('rejects non-objects', () => {
    for (const bad of [null, undefined, 'text', 42, [], true]) {
      expect(errorsFor(bad)).toEqual(['feedback must be an object']);
    }
  });

  it('rejects unknown and missing categories', () => {
    expect(errorsFor(input({ category: 'praise' }))).toEqual([
      'category must be one of bug, idea, pack-request, other',
    ]);
    expect(errorsFor(input({ category: undefined }))).toHaveLength(1);
    expect(errorsFor(input({ category: 'BUG' }))).toHaveLength(1);
  });

  it('trims the message and enforces its bounds after trimming', () => {
    expect(payload({ message: '   ten chars!  \n' }).message).toBe('ten chars!');
    expect(errorsFor(input({ message: '  short    ' }))).toEqual([
      `message must be ${MIN_MESSAGE} to ${MAX_MESSAGE} characters`,
    ]);
    expect(payload({ message: 'x'.repeat(MIN_MESSAGE) }).message).toHaveLength(MIN_MESSAGE);
    expect(payload({ message: 'x'.repeat(MAX_MESSAGE) }).message).toHaveLength(MAX_MESSAGE);
    expect(payload({ message: ` ${'x'.repeat(MAX_MESSAGE)} ` }).message).toHaveLength(MAX_MESSAGE);
    expect(errorsFor(input({ message: 'x'.repeat(MAX_MESSAGE + 1) }))).toHaveLength(1);
    expect(errorsFor(input({ message: 12345678901 }))).toHaveLength(1);
  });

  it('treats contact as optional, trimmed and bounded', () => {
    expect(payload({ contact: undefined })).not.toHaveProperty('contact');
    expect(payload({ contact: '   ' })).not.toHaveProperty('contact');
    expect(payload({ contact: ' @me ' }).contact).toBe('@me');
    expect(payload({ contact: 'c'.repeat(MAX_CONTACT) }).contact).toHaveLength(MAX_CONTACT);
    expect(errorsFor(input({ contact: 'c'.repeat(MAX_CONTACT + 1) }))).toEqual([
      `contact must be at most ${MAX_CONTACT} characters`,
    ]);
    expect(errorsFor(input({ contact: 7 }))).toHaveLength(1);
  });

  it('bounds the turnstile token', () => {
    expect(payload({ turnstileToken: 't'.repeat(MAX_TOKEN) }).turnstileToken).toHaveLength(MAX_TOKEN);
    expect(errorsFor(input({ turnstileToken: '' }))).toEqual([
      `turnstileToken must be 1 to ${MAX_TOKEN} characters`,
    ]);
    expect(errorsFor(input({ turnstileToken: 't'.repeat(MAX_TOKEN + 1) }))).toHaveLength(1);
    expect(errorsFor(input({ turnstileToken: undefined }))).toHaveLength(1);
  });

  it('validates the context', () => {
    expect(errorsFor(input({ context: undefined }))).toEqual(['context must be an object']);
    expect(payload({ context: { ...context(), profile: null } }).context.profile).toBeNull();
    for (const store of ['fs-access', 'drop', 'none']) {
      expect(payload({ context: { ...context(), store } }).context.store).toBe(store);
    }
    expect(errorsFor(input({ context: { ...context(), store: 'usb' } }))).toEqual([
      'context.store must be one of fs-access, drop, none',
    ]);
    expect(errorsFor(input({ context: { ...context(), deviceMounted: 'yes' } }))).toEqual([
      'context.deviceMounted must be a boolean',
    ]);
    expect(errorsFor(input({ context: { ...context(), profile: 3 } }))).toEqual([
      'context.profile must be a string or null',
    ]);
    expect(errorsFor(input({ context: { ...context(), appVersion: undefined } }))).toHaveLength(1);
    expect(errorsFor(input({ context: { ...context(), userAgent: 'u'.repeat(501) } }))).toHaveLength(1);
    expect(errorsFor(input({ context: { ...context(), commit: 'c'.repeat(41) } }))).toHaveLength(1);
  });

  it('reports every problem at once', () => {
    expect(errorsFor({ category: 'nope', message: '', context: {}, turnstileToken: '' }).length).toBeGreaterThanOrEqual(5);
  });

  it('ignores extra keys at both levels', () => {
    const result = validateFeedback(
      input({ admin: true, labels: ['security'], context: { ...context(), secret: 'x' } }),
    );
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(Object.keys(result.value).sort()).toEqual(['category', 'contact', 'context', 'message', 'turnstileToken']);
    expect(Object.keys(result.value.context).sort()).toEqual([
      'appVersion',
      'commit',
      'deviceMounted',
      'profile',
      'store',
      'userAgent',
    ]);
  });
});

describe('neutraliseMentions', () => {
  it('breaks user and team mentions', () => {
    expect(neutraliseMentions('thanks @octocat and @coldsoldr/maintainers')).toBe(
      `thanks @${ZWSP}octocat and @${ZWSP}coldsoldr/maintainers`,
    );
  });

  it('breaks issue references in every form', () => {
    expect(neutraliseMentions('see #1, owner/repo#22 and GH-3')).toBe(
      `see #${ZWSP}1, owner/repo#${ZWSP}22 and GH-${ZWSP}3`,
    );
    expect(neutraliseMentions('gh-4')).toBe(`gh-${ZWSP}4`);
  });

  it('keeps email addresses intact but catches mentions after punctuation', () => {
    expect(neutraliseMentions('me@example.com')).toBe('me@example.com');
    expect(neutraliseMentions('(@bob) .@carol')).toBe(`(@${ZWSP}bob) .@${ZWSP}carol`);
    expect(neutraliseMentions(`${ZWSP}@dave`)).toBe(`${ZWSP}@${ZWSP}dave`);
  });

  it('leaves text without mentions alone', () => {
    expect(neutraliseMentions('# heading, @ alone, C# code, 100%')).toBe('# heading, @ alone, C# code, 100%');
  });

  it('is idempotent', () => {
    const once = neutraliseMentions('@a #1 GH-1');
    expect(neutraliseMentions(once)).toBe(once);
  });
});

describe('issueTitle', () => {
  it('prefixes the category to the first line', () => {
    expect(issueTitle(payload({ message: 'Crash on save\nmore detail here' }))).toBe('[bug] Crash on save');
    expect(issueTitle(payload({ category: 'pack-request', message: 'More drum kits\r\nplease' }))).toBe(
      '[pack-request] More drum kits',
    );
  });

  it('truncates to the maximum with an ellipsis', () => {
    const title = issueTitle(payload({ message: 'word '.repeat(40) }));
    expect(Array.from(title)).toHaveLength(MAX_TITLE);
    expect(title.endsWith('…')).toBe(true);
    expect(title.startsWith('[bug] word word')).toBe(true);
  });

  it('keeps a title that fits exactly', () => {
    const line = 'y'.repeat(MAX_TITLE - '[bug] '.length);
    expect(issueTitle(payload({ message: line }))).toBe(`[bug] ${line}`);
  });

  it('does not split surrogate pairs when truncating', () => {
    const title = issueTitle(payload({ message: '🎹'.repeat(200) }));
    expect(Array.from(title)).toHaveLength(MAX_TITLE);
    expect(title).not.toMatch(/[\uD800-\uDBFF](?![\uDC00-\uDFFF])/);
    expect(title.endsWith('🎹…')).toBe(true);
  });

  it('neutralises mentions and strips control characters', () => {
    expect(issueTitle(payload({ message: '@everyone fix\tthis #9\u0007' }))).toBe(
      `[bug] @${ZWSP}everyone fix this #${ZWSP}9`,
    );
  });
});

describe('issueBody', () => {
  const quoteLines = (body: string): string[] => {
    const lines = body.split('\n');
    const start = lines.indexOf('### Message') + 2;
    const end = lines.indexOf('', start);
    return lines.slice(start, end);
  };

  it('quotes every message line and lists contact and context', () => {
    const body = issueBody(payload({ message: 'line one\n\nline three' }));
    expect(quoteLines(body)).toEqual(['> line one', '>', '> line three']);
    expect(body).toContain('**Contact:** me@example.com');
    expect(body).toContain('| App version | 0.5.0 |');
    expect(body).toContain('| Commit | abc1234 |');
    expect(body).toContain('| Profile | op1/stock |');
    expect(body).toContain('| Store | fs-access |');
    expect(body).toContain('| Device mounted | yes |');
    expect(body).toContain('| User agent | Mozilla/5.0 (Windows NT 10.0) Chrome/130 |');
  });

  it('notes missing contact and profile', () => {
    const body = issueBody(payload({ contact: undefined, context: { ...context(), profile: null, deviceMounted: false } }));
    expect(body).toContain('**Contact:** not given');
    expect(body).toContain('| Profile | none |');
    expect(body).toContain('| Device mounted | no |');
  });

  it('never includes the turnstile token', () => {
    const body = issueBody(payload({ turnstileToken: 'SECRET-TOKEN-123' }));
    expect(body).not.toContain('SECRET-TOKEN-123');
  });

  it('cannot be broken out of with code fences or backticks', () => {
    const body = issueBody(payload({ message: 'before\n```\n</details>\n# Pwned\n```\n`code` after' }));
    expect(body).not.toContain('`');
    expect(quoteLines(body).every((l) => l === '>' || l.startsWith('> '))).toBe(true);
    expect(quoteLines(body)).toHaveLength(6);
  });

  it('escapes html so a message cannot close or open the details block', () => {
    const body = issueBody(
      payload({ message: '</details><details open><summary>x</summary><img src=x onerror=alert(1)>' }),
    );
    expect(body.match(/<details>/g)).toHaveLength(1);
    expect(body.match(/<\/details>/g)).toHaveLength(1);
    expect(body).not.toContain('<img');
    expect(body).toContain('&lt;/details&gt;');
  });

  it('neutralises mentions and references in every user field', () => {
    const body = issueBody(
      payload({
        message: 'ping @org/team and @alice about #1 and GH-2',
        contact: '@bob #3',
        context: { ...context(), profile: '@carol', userAgent: 'UA #4 @dave', appVersion: '@v', commit: '#5' },
      }),
    );
    expect(body).not.toMatch(/@[A-Za-z0-9]/);
    expect(body).not.toMatch(/#\d/);
    expect(body).not.toMatch(/GH-\d/i);
  });

  it('keeps user values from breaking the context table', () => {
    const body = issueBody(
      payload({ contact: 'a | b\nc', context: { ...context(), userAgent: 'evil | col\n| injected | row |\\' } }),
    );
    const rows = body.split('\n').filter((l) => l.startsWith('|'));
    expect(rows).toHaveLength(8);
    for (const row of rows) expect(row.match(/\|/g)).toHaveLength(3);
    expect(body).toContain('**Contact:** a &#x7C; b c');
  });

  it('handles very long lines and maximum length messages within GitHub limits', () => {
    const long = issueBody(payload({ message: 'z'.repeat(MAX_MESSAGE) }));
    expect(quoteLines(long)).toEqual([`> ${'z'.repeat(MAX_MESSAGE)}`]);
    const worst = issueBody(payload({ message: '`'.repeat(MAX_MESSAGE) }));
    expect(worst.length).toBeLessThan(65536);
  });

  it('preserves unicode text', () => {
    const body = issueBody(payload({ message: 'Ünïcödé 🎛️ テスト\nزر لا يعمل' }));
    expect(quoteLines(body)).toEqual(['> Ünïcödé 🎛️ テスト', '> زر لا يعمل']);
  });

  it('normalises carriage returns so no line escapes the quote', () => {
    const body = issueBody(payload({ message: 'one\r\ntwo\rthree\u2028four' }));
    expect(quoteLines(body)).toEqual(['> one', '> two', '> three', '> four']);
    expect(body).not.toContain('\r');
  });
});
