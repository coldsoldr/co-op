/**
 * A plugin submission from the site's Submit a plugin page: an author's GitHub repo link, which the
 * Worker files as a `plugin-request` issue. The plugin-request workflow then drafts the listing from
 * the repo and opens a pull request for the owner (web/scripts/plugin-request-lib.ts).
 *
 * Everything here is untrusted. The repo must be a plain github.com owner/repo, and the note and
 * contact go into the issue escaped the way feedback is, so they can't ping people or link issues.
 */
import { neutraliseMentions } from './feedback';

export const REQUEST_FIRMWARES = ['hundekraft', 'opus', 'op1r', 'unsure'] as const;
export type RequestFirmware = (typeof REQUEST_FIRMWARES)[number];

export const MAX_REPO = 200;
export const MAX_NOTE = 2000;
export const MAX_REQUEST_CONTACT = 200;
export const MAX_REQUEST_TOKEN = 2048;
/** The label the workflow listens for. */
export const REQUEST_LABEL = 'plugin-request';
/** A hidden line in the issue that the workflow reads the repo from. */
export const REQUEST_MARKER = 'co-op-plugin-request';

export interface PluginRequestPayload {
  /** owner/repo, normalised from whatever link was pasted. */
  repo: string;
  firmware: RequestFirmware;
  note?: string;
  contact?: string;
  /** Never written into the issue. */
  turnstileToken: string;
}

export type PluginRequestResult = { ok: true; value: PluginRequestPayload } | { ok: false; errors: string[] };

/** GitHub's own rules: an owner is letters, digits and single hyphens; a repo also allows . and _. */
const OWNER = /^[A-Za-z0-9](?:[A-Za-z0-9]|-(?=[A-Za-z0-9])){0,38}$/;
const NAME = /^[A-Za-z0-9._-]{1,100}$/;

/**
 * owner/repo from a pasted link or name: https://github.com/o/r, github.com/o/r.git, o/r, or a link
 * deeper into the repo (/tree/main/x, /releases). Null for anything else, including other hosts.
 */
export function parseRepo(input: string): string | null {
  let text = input.trim();
  if (text.length === 0 || text.length > MAX_REPO) return null;
  text = text.replace(/^https?:\/\//i, '').replace(/^www\./i, '');
  if (/^github\.com\//i.test(text)) text = text.slice('github.com/'.length);
  else if (text.includes('.') && text.split('/')[0]?.includes('.')) return null;
  const [owner, rawName] = text.split(/[/?#]/);
  if (owner === undefined || rawName === undefined) return null;
  const name = rawName.replace(/\.git$/i, '');
  if (!OWNER.test(owner) || !NAME.test(name) || name === '.' || name === '..') return null;
  return `${owner}/${name}`;
}

const isRecord = (v: unknown): v is Record<string, unknown> => typeof v === 'object' && v !== null && !Array.isArray(v);
const wellFormed = (s: string): boolean => !/[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/.test(s);

function optionalText(input: Record<string, unknown>, key: 'note' | 'contact', max: number, errors: string[]): string | undefined {
  const v = input[key];
  if (v === undefined) return undefined;
  if (typeof v !== 'string' || v.trim().length > max) {
    errors.push(`${key} must be at most ${max} characters`);
    return undefined;
  }
  if (!wellFormed(v)) {
    errors.push(`${key} must be valid Unicode text`);
    return undefined;
  }
  return v.trim() === '' ? undefined : v.trim();
}

/** Checks untrusted input and returns a payload built only from known keys. */
export function validatePluginRequest(input: unknown): PluginRequestResult {
  if (!isRecord(input)) return { ok: false, errors: ['the request must be an object'] };
  const errors: string[] = [];
  const repo = typeof input.repo === 'string' ? parseRepo(input.repo) : null;
  if (repo === null) errors.push('repo must be a GitHub repository link, like https://github.com/you/your-plugin');
  else if (repo.toLowerCase() === 'coldsoldr/co-op') errors.push('repo must be the plugin’s own repository, not co-op');
  const firmware = input.firmware;
  if (typeof firmware !== 'string' || !(REQUEST_FIRMWARES as readonly string[]).includes(firmware)) {
    errors.push(`firmware must be one of ${REQUEST_FIRMWARES.join(', ')}`);
  }
  const note = optionalText(input, 'note', MAX_NOTE, errors);
  const contact = optionalText(input, 'contact', MAX_REQUEST_CONTACT, errors);
  const token = input.turnstileToken;
  if (typeof token !== 'string' || token.length < 1 || token.length > MAX_REQUEST_TOKEN || !wellFormed(token)) {
    errors.push(`turnstileToken must be 1 to ${MAX_REQUEST_TOKEN} characters`);
  }
  if (errors.length > 0 || repo === null) return { ok: false, errors };
  const value: PluginRequestPayload = { repo, firmware: firmware as RequestFirmware, turnstileToken: token as string };
  if (note !== undefined) value.note = note;
  if (contact !== undefined) value.contact = contact;
  return { ok: true, value };
}

const LINE_BREAK = /\r\n|[\r\n\u2028\u2029]/;
// eslint-disable-next-line no-control-regex
const CONTROL = /[\u0000-\u001F\u007F-\u009F\u202A-\u202E\u2066-\u2069]/g;
const escapeMarkdown = (text: string): string =>
  text
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/`/g, '&#x60;')
    .replace(/\\/g, '&#x5C;')
    .replace(/\|/g, '&#x7C;');
const inline = (text: string): string => escapeMarkdown(neutraliseMentions(text.replace(CONTROL, ' ').replace(/\s+/g, ' ').trim()));

/** `Plugin: owner/repo`. The repo passed parseRepo, so it holds no mention or markup. */
export const requestTitle = (p: PluginRequestPayload): string => `Plugin: ${p.repo}`;

/** The issue markdown, with the marker line the workflow reads. */
export function requestBody(p: PluginRequestPayload): string {
  const note =
    p.note === undefined
      ? ['(none)']
      : p.note
          .split(LINE_BREAK)
          .map((line) => escapeMarkdown(neutraliseMentions(line.replace(CONTROL, (c) => (c === '\t' ? c : '')))))
          .map((line) => (line.trim() === '' ? '>' : `> ${line}`));
  return [
    `<!-- ${REQUEST_MARKER} repo=${p.repo} firmware=${p.firmware} -->`,
    `**Repo:** https://github.com/${p.repo}`,
    '',
    `**Firmware:** ${p.firmware}`,
    '',
    '### Note',
    '',
    ...note,
    '',
    `**Contact:** ${p.contact === undefined ? 'not given' : inline(p.contact)}`,
    '',
    '<sub>Sent with the co-op Submit a plugin form. A workflow drafts the listing and links its pull request here.</sub>',
    '',
  ].join('\n');
}

/**
 * The repo an issue asks for: the marker line the Worker writes, else the first github.com repo link in
 * the body (the GitHub issue form's Source field) that isn't co-op itself. Null when there is none.
 */
export function repoFromIssue(body: string): { repo: string; firmware: RequestFirmware | null } | null {
  const marker = new RegExp(`<!-- ${REQUEST_MARKER} repo=(\\S+) firmware=(\\S+) -->`).exec(body);
  if (marker?.[1]) {
    const repo = parseRepo(marker[1]);
    const firmware = (REQUEST_FIRMWARES as readonly string[]).includes(marker[2] ?? '') ? (marker[2] as RequestFirmware) : null;
    if (repo) return { repo, firmware };
  }
  for (const match of body.matchAll(/https?:\/\/(?:www\.)?github\.com\/[^\s)<>"'\]]+/gi)) {
    const repo = parseRepo(match[0]);
    if (repo && repo.toLowerCase() !== 'coldsoldr/co-op') {
      const firmware = /\bopus\b/i.test(body)
        ? 'opus'
        : /\b(hundekraft|kennel)\b/i.test(body)
          ? 'hundekraft'
          : /\bop1r\b|\.op1p\b/i.test(body)
            ? 'op1r'
            : null;
      return { repo, firmware };
    }
  }
  return null;
}
