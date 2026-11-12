/**
 * Finds updates to listed plugins in their authors' GitHub repos (docs/plugin-release-guide.md):
 *
 * - A build with a fileUrl (a .knl committed in the author's repo, pinned to a commit) follows that file: a
 *   newer commit that changed it gives the build the new commit, size, hash and the version in the file's
 *   header. The new file must pass co-op's kennel check and keep the plugin's saved name and kind.
 * - Any other build whose link or source is a GitHub repo, and which has a version, follows the repo's latest
 *   release (pre-releases are not releases): a newer tag becomes its version.
 *
 * Listings the workshop and opus refresher (listed_plugins.py) keeps are left to it, as are hosted plugins.
 * Nothing is applied that fails a check; it is reported instead, for the pull request to show.
 */
import { createHash } from 'node:crypto';
import { checkKnl } from '@co-op/shared/plugins';

/** GitHub's REST API and plain downloads; null for a 404. */
export interface Remote {
  api: (path: string) => Promise<unknown>;
  download: (url: string) => Promise<Uint8Array | null>;
}

export interface Change {
  plugin: string;
  firmware: string;
  repo: string;
  from: string;
  to: string;
  /** What moved: the file at a new commit, or a new release. */
  kind: 'file' | 'release';
  /** A link to look at what changed. */
  compare: string;
  notes?: string;
}

export interface Problem {
  plugin: string;
  firmware: string;
  message: string;
}

const RAW = /^https:\/\/raw\.githubusercontent\.com\/([A-Za-z0-9_.-]+)\/([A-Za-z0-9_.-]+)\/([0-9a-f]{40})\/(.+)$/;
const REPO = /^https:\/\/github\.com\/([A-Za-z0-9_.-]+)\/([A-Za-z0-9_.-]+)(?:[/?#]|$)/;
/** Kept by web/scripts/listed_plugins.py from the workshop's and opus's own releases. */
const REFRESHED = ['https://hundekraft.github.io/op1-workshop/', 'https://github.com/LizardMan245/opus/releases'];
const MAX_NOTES = 1500;

type Build = Record<string, unknown> & { firmware: string; version?: string; link?: string; source?: string; fileUrl?: string };

/** The numbers in a version or tag ("v1.5.2" → [1, 5, 2]); null when it has none. */
export function versionParts(version: string): number[] | null {
  const numbers = version.replace(/^v/i, '').match(/\d+/g);
  return numbers ? numbers.map(Number) : null;
}

/** Positive when a is newer than b. */
export function compareVersions(a: string, b: string): number {
  const x = versionParts(a) ?? [];
  const y = versionParts(b) ?? [];
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    const d = (x[i] ?? 0) - (y[i] ?? 0);
    if (d !== 0) return d;
  }
  return 0;
}

const repoOf = (url: string | undefined): string | null => {
  const match = url === undefined ? null : REPO.exec(url);
  return match ? `${match[1]}/${match[2]}` : null;
};

/**
 * Checks one plugin.json (parsed) for updates and applies the ones that pass to it in place. Returns what
 * changed and what needs a person.
 */
export async function updatePlugin(manifest: Record<string, unknown>, remote: Remote): Promise<{ changes: Change[]; problems: Problem[] }> {
  const id = manifest.id as string;
  const kind = manifest.kind as string;
  const builds = (Array.isArray(manifest.builds) ? manifest.builds : [manifest]) as Build[];
  const changes: Change[] = [];
  const problems: Problem[] = [];
  for (const build of builds) {
    const link = build.link ?? (manifest.link as string | undefined);
    if (link === undefined || REFRESHED.some((r) => link.startsWith(r))) continue;
    const problem = (message: string): void => {
      problems.push({ plugin: id, firmware: build.firmware, message });
    };

    if (build.fileUrl !== undefined) {
      const pinned = RAW.exec(build.fileUrl);
      if (!pinned) continue;
      const [, owner, name, sha, path] = pinned as unknown as [string, string, string, string, string];
      const repo = `${owner}/${name}`;
      const commits = (await remote.api(`repos/${repo}/commits?path=${encodeURIComponent(path)}&per_page=1`)) as { sha: string }[] | null;
      const latest = commits?.[0]?.sha;
      if (!latest) {
        problem(`${path} is no longer in ${repo}: it was moved or removed. Point fileUrl at its new place.`);
        continue;
      }
      if (latest === sha) continue;
      const url = `https://raw.githubusercontent.com/${repo}/${latest}/${path}`;
      const bytes = await remote.download(url);
      if (!bytes) {
        problem(`${url} could not be downloaded.`);
        continue;
      }
      const check = checkKnl(bytes);
      if (!check.info) {
        problem(`the new ${path} (commit ${latest.slice(0, 7)}) fails co-op's check: ${check.problems.join('; ')}. Not applied.`);
        continue;
      }
      if (check.info.savedName !== id || check.info.kind !== kind) {
        problem(
          `the new ${path} (commit ${latest.slice(0, 7)}) is a ${check.info.kind ?? '?'} saved as "${check.info.savedName ?? '?'}", not the ${kind} "${id}" listed. Not applied.`,
        );
        continue;
      }
      const version = check.info.version ?? build.version;
      if (version !== undefined && build.version !== undefined && compareVersions(version, build.version) < 0) {
        problem(`the new ${path} (commit ${latest.slice(0, 7)}) says version ${version}, older than the listed ${build.version}. Not applied.`);
        continue;
      }
      changes.push({
        plugin: id,
        firmware: build.firmware,
        repo,
        from: `${build.version ?? '?'} (${sha.slice(0, 7)})`,
        to: `${version ?? '?'} (${latest.slice(0, 7)})`,
        kind: 'file',
        compare: `https://github.com/${repo}/compare/${sha}...${latest}`,
      });
      build.fileUrl = url;
      build.sha256 = createHash('sha256').update(bytes).digest('hex');
      build.sizeBytes = bytes.length;
      if (version !== undefined) build.version = version;
      continue;
    }

    const repo = repoOf(link) ?? repoOf(build.source ?? (manifest.source as string | undefined));
    if (!repo || build.version === undefined || versionParts(build.version) === null) continue;
    const release = (await remote.api(`repos/${repo}/releases/latest`)) as { tag_name: string; html_url: string; body?: string } | null;
    if (!release || versionParts(release.tag_name) === null) continue;
    if (compareVersions(release.tag_name, build.version) <= 0) continue;
    const version = release.tag_name.replace(/^v(?=\d)/i, '');
    const notes = (release.body ?? '').trim();
    changes.push({
      plugin: id,
      firmware: build.firmware,
      repo,
      from: build.version,
      to: version,
      kind: 'release',
      compare: release.html_url,
      ...(notes ? { notes: notes.length > MAX_NOTES ? `${notes.slice(0, MAX_NOTES)}…` : notes } : {}),
    });
    build.version = version;
  }
  return { changes, problems };
}

/** The pull request's body: every change with its link, then what needs a person. */
export function summary(changes: readonly Change[], problems: readonly Problem[]): string {
  const lines: string[] = [];
  if (changes.length > 0) {
    lines.push('Updates found in the plugin authors’ own repos by the daily check (`web/scripts/plugin-updates.ts`).', '');
    lines.push('| Plugin | Firmware | From | To | What changed |', '|---|---|---|---|---|');
    for (const c of changes) {
      lines.push(`| ${c.plugin} | ${c.firmware} | ${c.from} | ${c.to} | [${c.kind === 'file' ? 'the file’s commits' : 'the release'}](${c.compare}) |`);
    }
    lines.push(
      '',
      'Each new .knl passed co-op’s kennel check with the listed saved name and kind, and plugins:validate passed on the result. ' +
        'Install over USB sends these files to people’s OP-1s: look at what changed before merging. If the controls changed, ' +
        're-run `/add-plugin <repo>` to rewrite the manual.',
    );
    for (const c of changes.filter((x) => x.notes)) lines.push('', `<details><summary>${c.plugin} ${c.to} release notes</summary>`, '', c.notes as string, '', '</details>');
  }
  if (problems.length > 0) {
    lines.push('', '### Needs a person', '');
    for (const p of problems) lines.push(`- **${p.plugin}** (${p.firmware}): ${p.message}`);
  }
  if (lines.length === 0) lines.push('No plugin updates today.');
  return `${lines.join('\n')}\n`;
}
