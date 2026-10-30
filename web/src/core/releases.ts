const notes = import.meta.glob<string>('../../../docs/release-notes/v*.md', { query: '?raw', import: 'default', eager: true });

export interface Release {
  /** For example '0.8.0'. */
  version: string;
  /** The heading, without the product name: 'V0.8.0' or 'V0.5.0 and earlier'. */
  title: string;
  /** For example '2026-10-30'; null when the notes do not say. */
  date: string | null;
  /** The notes below the heading and date. */
  body: string;
}

const numbers = (version: string): number[] => version.split('.').map(Number);

/** Compares versions number by number, so 0.10.0 comes after 0.9.0. Newest first. */
function newestFirst(a: Release, b: Release): number {
  const [x, y] = [numbers(a.version), numbers(b.version)];
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    const difference = (y[i] ?? 0) - (x[i] ?? 0);
    if (difference !== 0) return difference;
  }
  return 0;
}

/** One notes file: `# co-op V0.8.0`, then `Released 2026-10-30.`, then the notes. */
export function parseRelease(path: string, text: string): Release | null {
  const version = /v(\d+\.\d+\.\d+)\.md$/.exec(path)?.[1];
  if (!version) return null;
  const lines = text.replace(/\r\n/g, '\n').split('\n');
  const heading = /^# (?:co-op )?(.+)$/.exec(lines[0] ?? '');
  const rest = lines.slice(heading ? 1 : 0);
  const at = rest.findIndex((line) => line.trim() !== '');
  const date = /^Released (\d{4}-\d{2}-\d{2})\.$/.exec(rest[at] ?? '')?.[1] ?? null;
  return {
    version,
    title: heading?.[1] ?? `V${version}`,
    date,
    body: rest.slice(date ? at + 1 : 0).join('\n').trim(),
  };
}

export function parseReleases(files: Readonly<Record<string, string>>): Release[] {
  return Object.entries(files)
    .flatMap(([path, text]) => parseRelease(path, text) ?? [])
    .sort(newestFirst);
}

/** Every release in docs/release-notes, read when the site is built. */
export const RELEASES = parseReleases(notes);
