/**
 * Drafts co-op listings from a plugin's GitHub repo, for the plugin-request workflow
 * (.github/workflows/plugin-request.yml). No model is involved: it reads only facts the repo states.
 *
 * - Each built plugin file in the repo (.knl, .opmod, .coop, .op1p) is one plugin; a repo with none is one
 *   plugin named after the repo, if its firmware can be told.
 * - An opus plugin's manifest.txt gives its name, author, version, kind and the Opus core it needs. A
 *   .knl's header gives its saved name, kind, author and version, and a .knl committed in the repo gets
 *   a pinned fileUrl so its card can install it over USB.
 * - An .op1p is an op1r plugin: its header gives its saved name, kind and author, and one committed in the
 *   repo gets a pinned fileUrl so its card can download it (op1r's own web patcher installs it).
 * - The nearest README to the file is the plugin's own page: the link points there when it is a folder of
 *   its own, and its first paragraph and first screen image become the draft description and screenshot.
 *   A README shared by several plugins is read by the section headed with the plugin's name.
 *
 * The description and manual are drafts: plugins/log.json marks them, and the owner's /polish-plugins
 * skill rewrites them later. Everything read from the repo is untrusted text, only ever written into
 * the listing files for the owner to review in the pull request.
 */
import { createHash } from 'node:crypto';
import {
  HUNDEKRAFT_ID,
  MAX_DESCRIPTION,
  MAX_KNL_BYTES,
  MAX_OP1P_BYTES,
  OPUS_ID,
  checkKnl,
  checkOp1p,
  idFromSavedName,
  type PluginFileInfo,
  type PluginKind,
} from '@co-op/shared/plugins';
import type { RequestFirmware } from '@co-op/shared/plugin-request';
import type { Remote } from './plugin-updates-lib';

export type Firmware = 'op1/hundekraft' | 'op1/opus' | 'op1/coop' | 'op1/op1r';

export interface Draft {
  id: string;
  manifest: Record<string, unknown>;
  manual: string;
  /** A raw image URL to make the screenshot from, or null for a name card. */
  screenshot: string | null;
  /** What a person should look at in the pull request. */
  notes: string[];
  /** The plugin file's name, for joining one plugin's builds for several firmwares. */
  base: string;
  /** The plugin file's path in the repo ('' for a repo with none). */
  path: string;
}

export interface DraftResult {
  repo: string;
  drafts: Draft[];
  /** Plugins already listed from this repo, left alone. */
  skipped: string[];
  /** Why nothing could be drafted, or other repo-wide news. */
  problems: string[];
}

/** At most this many plugins from one repo; a repo with more is for a person to look at. */
export const MAX_PLUGINS = 12;
const EXT: Record<string, Firmware> = { '.knl': 'op1/hundekraft', '.opmod': 'op1/opus', '.coop': 'op1/coop', '.op1p': 'op1/op1r' };
const FIRMWARE_LABEL: Record<Firmware, string> = { 'op1/hundekraft': 'hundekraft', 'op1/opus': 'opus', 'op1/coop': 'coop', 'op1/op1r': 'op1r' };
/** op1r's web patcher, which installs .op1p files (co-op downloads them). */
const OP1R_PATCHER_URL = 'https://pcppcp.github.io/op1r/web/';
/** A file name op1r loads from plugins/ (shared/src/plugins.ts, the fileUrl rule for .op1p). */
const OP1P_FILE = /^[A-Za-z0-9_-][A-Za-z0-9_.-]{0,34}\.op1p$/;
/** Folders whose plugin files are someone else's examples or build kit, not this repo's plugins. */
const SKIP_DIR = /(^|\/)(examples?|tests?|sdk|plugin-kit|vendor|third_party|node_modules|\.github)(\/|$)/i;
const TAG_WORDS: [RegExp, string][] = [
  [/\bdelay\b/i, 'delay'],
  [/\breverb\b/i, 'reverb'],
  [/\bgranular|\bgrains?\b/i, 'granular'],
  [/\bloop(er|ing)?\b/i, 'looper'],
  [/\bsequencer\b/i, 'sequencer'],
  [/\barp(eggiator)?\b/i, 'arpeggiator'],
  [/\bchorus\b/i, 'chorus'],
  [/\b(distortion|overdrive|saturation)\b/i, 'distortion'],
  [/\bbit ?crush/i, 'bitcrusher'],
  [/\bsampler\b/i, 'sampler'],
  [/\bdrums?\b/i, 'drum'],
  [/\bwavetable\b/i, 'wavetable'],
  [/\bgenerative\b/i, 'generative'],
  [/\beuclidean\b/i, 'euclidean'],
  [/\bpitch[- ]?shift/i, 'pitch-shift'],
  [/\bclock|\bsync\b/i, 'sync'],
  [/\bmidi\b/i, 'midi'],
  [/mutable instruments/i, 'mutable-instruments'],
];

const ALLOWED_LICENSES = new Set(['MIT', 'BSD-3-Clause', 'Apache-2.0', 'MPL-2.0']);

interface Tree {
  branch: string;
  paths: string[];
}

interface OpusManifest {
  id?: string;
  name?: string;
  author?: string;
  version?: string;
  core?: string;
  adds?: string;
  /** Another plugin it needs, as `<plugin id> <version>`. */
  needs?: string;
}

interface Found {
  path: string;
  base: string;
  firmware: Firmware;
}

const dirOf = (path: string): string => (path.includes('/') ? path.slice(0, path.lastIndexOf('/')) : '');
const baseOf = (path: string): string => path.slice(path.lastIndexOf('/') + 1);
const norm = (s: string): string => s.toLowerCase().replace(/[^a-z0-9]/g, '');
const encodePath = (path: string): string => path.split('/').map(encodeURIComponent).join('/');
const raw = (repo: string, branch: string, path: string): string => `https://raw.githubusercontent.com/${repo}/${encodeURIComponent(branch)}/${encodePath(path)}`;
const text = (bytes: Uint8Array | null): string | null => (bytes === null ? null : new TextDecoder().decode(bytes));

/** The built plugin files in the tree, one per name; files under example or SDK folders are left out. */
export function findPluginFiles(paths: readonly string[]): Found[] {
  const byBase = new Map<string, Found>();
  for (const path of paths) {
    const dot = path.lastIndexOf('.');
    const firmware = dot < 0 ? undefined : EXT[path.slice(dot).toLowerCase()];
    if (firmware === undefined || SKIP_DIR.test(dirOf(path))) continue;
    const base = baseOf(path).slice(0, baseOf(path).lastIndexOf('.'));
    const key = `${firmware}:${base.toLowerCase()}`;
    // The shallowest copy wins: the one the README links, not a build folder's.
    const seen = byBase.get(key);
    if (seen === undefined || path.split('/').length < seen.path.split('/').length) byBase.set(key, { path, base, firmware });
  }
  return [...byBase.values()].sort((a, b) => a.path.localeCompare(b.path));
}

/** An opus manifest.txt's first value for each key it knows. */
export function parseOpusManifest(source: string): OpusManifest {
  const out: OpusManifest = {};
  for (const line of source.split(/\r?\n/)) {
    const m = /^\s*(id|name|author|version|core|adds|needs)\s+(.+?)\s*$/.exec(line);
    if (m?.[1] && m[2] && out[m[1] as keyof OpusManifest] === undefined) out[m[1] as keyof OpusManifest] = m[2];
  }
  return out;
}

/** The manifest an .opmod carries as text after its `opmod 1` line, or null when it has none. */
export function embeddedManifest(bytes: Uint8Array): OpusManifest | null {
  if (bytes.length < 4 || String.fromCharCode(...bytes.subarray(0, 4)) !== 'OPMD') return null;
  let latin = '';
  for (let i = 0; i < bytes.length; i += 8192) latin += String.fromCharCode(...bytes.subarray(i, i + 8192));
  const at = latin.search(/(^|[\n\0])opmod[ \t]+1[ \t]*\r?\n/);
  if (at < 0) return null;
  const block = latin.slice(at, at + 8192).replace(/^[\n\0]/, '').split('\0')[0] ?? '';
  const m = parseOpusManifest(block);
  return m.id === undefined && m.name === undefined ? null : m;
}

/**
 * From a paragraph about several plugins, the clause about this one, as a sentence without its version:
 * "HITS 1.0.0 is a 24-pad drum engine, FLUX 1.0.0 adds two LFOs" gives "HITS is a 24-pad drum engine."
 */
export function aboutName(paragraph: string, names: readonly string[]): string | null {
  const wanted = names.map(norm).filter(Boolean);
  for (const sentence of paragraph.split(/(?<=[.!?])\s+/)) {
    for (const clause of sentence.replace(/[.!?]$/, '').split(/,\s+(?:and\s+)?|;\s+/)) {
      const words = clause.trim().split(/\s+/);
      if (!wanted.includes(norm(words[0] ?? ''))) continue;
      const rest = words.slice(1).filter((w, i) => !(i === 0 && /^v?\d+(\.\d+)+$/.test(w)));
      if (rest.length < 2) continue;
      return `${[words[0], ...rest].join(' ')}.`;
    }
  }
  return null;
}

/** The kind an opus manifest's `adds` line names. */
export function opusKind(adds: string | undefined): PluginKind | null {
  if (adds === undefined) return null;
  if (/^effect\b/i.test(adds)) return 'fx';
  if (/^synth\s+(sequencer|arp)/i.test(adds)) return 'feature';
  if (/^(synth|drum|sampler|engine)\b/i.test(adds)) return 'synth';
  return 'feature';
}

/** Whether a manifest belongs to the plugin file with this name. */
const manifestMatches = (m: OpusManifest, base: string): boolean => {
  const b = norm(base);
  return (
    (m.id !== undefined && norm(m.id.slice(m.id.lastIndexOf('.') + 1)) === b) ||
    (m.adds !== undefined && norm(m.adds.split(/\s+/).at(-1) ?? '') === b) ||
    (m.name !== undefined && norm(m.name) === b)
  );
};

/** The nearest README at or above a folder, as a path, or null. */
export function nearestReadme(paths: readonly string[], dir: string): string | null {
  const readmes = new Map(paths.filter((p) => /^readme(\.md|\.markdown)?$/i.test(baseOf(p))).map((p) => [dirOf(p), p]));
  let at = dir;
  for (;;) {
    const found = readmes.get(at);
    if (found !== undefined) return found;
    if (at === '') return null;
    at = dirOf(at);
  }
}

/** Markdown as plain prose: links and images to their text, emphasis and code marks gone, HTML out. */
export function plain(markdown: string): string {
  return markdown
    .replace(/<!--[\s\S]*?-->/g, '')
    .replace(/!\[[^\]]*\]\([^)]*\)/g, '')
    .replace(/\[([^\]]*)\]\([^)]*\)/g, '$1')
    .replace(/<[^>]+>/g, '')
    .replace(/(\*\*|__)(.*?)\1/g, '$2')
    .replace(/(^|[^\w*])[*_]([^*_\n]+)[*_](?=[^\w*]|$)/g, '$1$2')
    .replace(/`([^`]*)`/g, '$1')
    .replace(/\s+/g, ' ')
    .trim();
}

/** The part of a README under the heading that names this plugin, else null. */
export function sectionFor(readme: string, names: readonly string[]): string | null {
  const lines = readme.split(/\r?\n/);
  const wanted = names.map(norm).filter(Boolean);
  for (let i = 0; i < lines.length; i++) {
    const h = /^(#{1,6})\s+(.*)$/.exec(lines[i] ?? '');
    if (!h?.[1] || h[2] === undefined) continue;
    const title = norm(plain(h[2]));
    if (!wanted.some((w) => title.startsWith(w) || title.includes(w))) continue;
    const depth = h[1].length;
    const end = lines.findIndex((l, j) => j > i && new RegExp(`^#{1,${depth}}\\s`).test(l));
    return lines.slice(i + 1, end < 0 ? undefined : end).join('\n');
  }
  return null;
}

/** The first paragraph of prose: no headings, images, tables, lists, quotes, code or link-only lines. */
export function firstParagraph(markdown: string): string | null {
  const blocks = markdown.replace(/```[\s\S]*?```/g, '').split(/\r?\n\s*\r?\n/);
  for (const block of blocks) {
    const t = block.trim();
    if (!t || /^(#|!\[|\||[-*+] |\d+\. |>|<)/.test(t)) continue;
    const prose = plain(t);
    if (prose.length < 30 || !/[a-z]{3}/i.test(prose)) continue;
    // A line of links ("Download · Manual · Source") is not prose.
    if (plain(t.replace(/\[[^\]]*\]\([^)]*\)/g, '')).replace(/[·|•,\s-]/g, '').length < 20) continue;
    return prose;
  }
  return null;
}

/** Up to three whole sentences within MAX_DESCRIPTION characters. */
export function clip(prose: string, max = MAX_DESCRIPTION): string {
  const sentences = prose.split(/(?<=[.!?])\s+(?=[A-Z0-9"“(])/).map((x) => `${x} `);
  let out = '';
  for (const s of sentences.slice(0, 3)) {
    if ((out + s).trim().length > max) break;
    out += s;
  }
  out = out.trim();
  if (out) return out;
  return `${prose.slice(0, max - 1).trimEnd()}…`;
}

/** In a table that lists several plugins, the longest cell of the row whose first cell is this plugin's name. */
export function tableRowFor(markdown: string, names: readonly string[]): string | null {
  const wanted = names.map(norm).filter(Boolean);
  for (const line of markdown.split(/\r?\n/)) {
    if (!/^\s*\|/.test(line)) continue;
    const cells = line.split('|').slice(1, -1).map((c) => plain(c));
    if (cells.length < 2 || !wanted.includes(norm(cells[0] ?? ''))) continue;
    const best = cells.slice(1).sort((a, b) => b.length - a.length)[0] ?? '';
    if (best.length >= 20 && /[a-z]{3}/i.test(best)) return /[.!?]$/.test(best) ? best : `${best}.`;
  }
  return null;
}

/** An image in the repo named after the plugin (docs/screens/hits.png), a screen folder's first. */
export function imageNamed(paths: readonly string[], names: readonly string[]): string | null {
  const wanted = names.map(norm).filter(Boolean);
  const images = paths.filter((p) => /\.(png|jpe?g|gif|webp)$/i.test(p) && !SKIP_DIR.test(dirOf(p)) && wanted.includes(norm(baseOf(p).replace(/\.[^.]+$/, ''))));
  const score = (p: string): number => (/screen|shot/i.test(p) ? 0 : 1) * 100 + p.split('/').length;
  return images.sort((a, b) => score(a) - score(b))[0] ?? null;
}

/** The first image in some markdown that shows something: not a badge, shield or SVG logo. */
export function firstScreen(markdown: string, prefer: readonly string[] = []): string | null {
  const found: { src: string; alt: string }[] = [];
  for (const m of markdown.matchAll(/!\[([^\]]*)\]\(\s*<?([^)\s>]+)>?(?:\s+"[^"]*")?\s*\)|<img\b[^>]*\bsrc=["']([^"']+)["'][^>]*>/gi)) {
    const src = m[2] ?? m[3];
    if (src === undefined) continue;
    if (/shields\.io|badge|\.svg(\?|$)|travis-ci|circleci|codecov/i.test(src)) continue;
    found.push({ src, alt: m[1] ?? '' });
  }
  const wanted = prefer.map(norm).filter(Boolean);
  const best = found.find((f) => wanted.some((w) => norm(f.alt).includes(w) || norm(f.src).includes(w)));
  return (best ?? found[0])?.src ?? null;
}

/** An image link from a README, as a URL to download: relative ones resolve inside the repo. */
export function imageUrl(src: string, repo: string, branch: string, readmePath: string): string | null {
  if (/^https:\/\//i.test(src)) {
    const blob = /^https:\/\/github\.com\/([^/]+\/[^/]+)\/blob\/([^/]+)\/(.+)$/.exec(src);
    if (blob?.[1] && blob[2] && blob[3]) return `https://raw.githubusercontent.com/${blob[1]}/${blob[2]}/${blob[3]}`;
    return src.replace(/\?raw=true$/, '');
  }
  if (/^[a-z]+:/i.test(src) || src.startsWith('//')) return null;
  const parts = (src.startsWith('/') ? src.slice(1) : `${dirOf(readmePath)}/${src}`).split('/');
  const path: string[] = [];
  for (const p of parts) {
    if (p === '' || p === '.') continue;
    if (p === '..') path.pop();
    else path.push(decodeURIComponent(p.split(/[?#]/)[0] ?? ''));
  }
  return path.length === 0 ? null : raw(repo, branch, path.join('/'));
}

/** The licence a repo or its LICENSE file states, as one of co-op's ids; null when it isn't one of them. */
export function licenseOf(spdx: string | null | undefined, licenseText: string | null, readme: string): string | null {
  // "or later" about the GPL, not about a firmware version ("Opus core 2.31 or later").
  const orLater = /GPL[\w .-]{0,30}?(v?3|version 3)?[\w .,()-]{0,20}?or[- ]later|or \(at your option\) any later/i.test(readme);
  const id =
    spdx && spdx !== 'NOASSERTION'
      ? spdx
      : licenseText === null
        ? null
        : /GNU GENERAL PUBLIC LICENSE[\s\S]{0,80}Version 3/i.test(licenseText)
          ? 'GPL-3.0'
          : /GNU GENERAL PUBLIC LICENSE[\s\S]{0,80}Version 2/i.test(licenseText)
            ? 'GPL-2.0'
            : /Mozilla Public License/i.test(licenseText)
              ? 'MPL-2.0'
              : /Apache License/i.test(licenseText)
                ? 'Apache-2.0'
                : /Permission is hereby granted, free of charge/i.test(licenseText)
                  ? 'MIT'
                  : /Redistribution and use in source and binary forms/i.test(licenseText) && /Neither the name/i.test(licenseText)
                    ? 'BSD-3-Clause'
                    : null;
  if (id === null) return null;
  if (id === 'GPL-3.0' || id === 'GPL-3.0-only' || id === 'GPL-3.0-or-later') return id === 'GPL-3.0-or-later' || orLater ? 'GPL-3.0-or-later' : 'GPL-3.0-only';
  if (id === 'GPL-2.0' || id === 'GPL-2.0-or-later') return id === 'GPL-2.0-or-later' || orLater ? 'GPL-2.0-or-later' : null;
  return ALLOWED_LICENSES.has(id) ? id : null;
}

/** An id that fits the firmware's rule, from a plugin's name. */
export function idFrom(name: string, firmware: Firmware): string {
  const id = name
    .toLowerCase()
    .replace(/[^a-z0-9_-]+/g, '_')
    .replace(/^_+|_+$/g, '');
  return id.slice(0, firmware === 'op1/hundekraft' ? 15 : 40) || 'plugin';
}

/** A name for a plugin from its file or repo name: hyphens and underscores to spaces, words capitalised. */
const titleCase = (s: string): string => s.replace(/[-_]+/g, ' ').replace(/\b[a-z]/g, (c) => c.toUpperCase());

/** A README heading as a plugin name: no version, no subtitle after a dash or colon. */
export function headingName(readme: string | null): string | null {
  const h = readme === null ? null : /^#\s+(.+)$/m.exec(readme);
  if (!h?.[1]) return null;
  const name = plain(h[1])
    .split(/\s+[—–-]\s+|:\s/)[0]
    ?.replace(/\s+v?\d+(\.\d+)+.*$/, '')
    .trim();
  return name && name.length <= 60 ? name : null;
}

function tagsFor(kind: PluginKind, firmware: Firmware, prose: string): string[] {
  const tags: string[] = [kind];
  for (const [re, tag] of TAG_WORDS) if (re.test(prose) && !tags.includes(tag)) tags.push(tag);
  return [...tags.slice(0, 7), FIRMWARE_LABEL[firmware]].filter((t, i, all) => all.indexOf(t) === i).slice(0, 8);
}

function kindFromProse(prose: string): PluginKind {
  if (/\b(sequencer|arpeggiator|tool|utility|clock|sync)\b/i.test(prose)) return 'feature';
  if (/\b(effect|fx|delay|reverb|filter|distortion|chorus)\b/i.test(prose)) return 'fx';
  if (/\b(synth|synthesizer|oscillator|engine)\b/i.test(prose)) return 'synth';
  return 'feature';
}

function firmwareFromProse(prose: string, hint: RequestFirmware | null): Firmware | null {
  if (hint === 'hundekraft') return 'op1/hundekraft';
  if (hint === 'opus') return 'op1/opus';
  if (hint === 'op1r') return 'op1/op1r';
  // Only a README that names exactly one firmware says which it is for.
  const named: Firmware[] = [];
  if (/\bopus\b|\.opmod\b/i.test(prose)) named.push('op1/opus');
  if (/\bhundekraft\b|\bkennel\b|\.knl\b/i.test(prose)) named.push('op1/hundekraft');
  if (/\bop1r\b|\.op1p\b/i.test(prose)) named.push('op1/op1r');
  return named.length === 1 ? (named[0] as Firmware) : null;
}

function requiresFrom(firmware: Firmware, prose: string, core: string | undefined): string | undefined {
  if (firmware === 'op1/opus') {
    if (core) return `opus ${core}`;
    const m = /\bopus(?:\s+core)?\s+v?(\d+(?:\.\d+)+)/i.exec(prose);
    return m?.[1] ? `opus ${m[1]}` : undefined;
  }
  if (firmware === 'op1/hundekraft') {
    const m = /\bhundekraft\s+v?(\d+(?:\.\d+)+)/i.exec(prose);
    return m?.[1] ? `hundekraft ${m[1]}` : undefined;
  }
  if (firmware === 'op1/op1r') {
    // op1r numbers its releases: "op1r 64 or later".
    const m = /\bop1r\s+v?(\d+)\b/i.exec(prose);
    return m?.[1] ? `op1r ${m[1]}` : undefined;
  }
  return undefined;
}

/** The manual draft, with the sections every listing has. */
export function draftManual(p: {
  name: string;
  tagline: string;
  description: string;
  author: string;
  repo: string;
  license: string | null;
  firmware: Firmware;
  requires: string | undefined;
  link: string;
  home: string | null;
}): string {
  const fw = FIRMWARE_LABEL[p.firmware];
  const needs = p.requires ? `It needs ${p.requires}` : `It is a plugin for ${fw}`;
  const links = [`- [Download and install](${p.link})`, `- [Source](https://github.com/${p.repo})`];
  if (p.home && p.home !== p.link) links.push(`- [Its README](${p.home})`);
  return [
    `# ${p.name}`,
    '',
    `*${p.tagline}*`,
    '',
    p.description,
    '',
    '## Who made it',
    '',
    `${p.name} is by ${p.author} ([${p.repo}](https://github.com/${p.repo})${p.license ? `, ${p.license}` : ''}).`,
    '',
    '## Get it',
    '',
    p.firmware === 'op1/op1r'
      ? `${needs}, on the original OP-1. Download the .op1p here or from [its page](${p.link}), and install it with [op1r's web patcher](${OP1R_PATCHER_URL}) (Connect OP-1). co-op hosts no copy: the download comes from the author's repo, checked against its hash.`
      : `${needs}, on the original OP-1. Download it from [its page](${p.link}) and follow the install steps there. co-op lists it for reference and hosts no copy.`,
    '',
    '## Links',
    '',
    ...links,
    '',
  ].join('\n');
}

/** The first sentence, short, without its full stop: the manual's tagline. */
const tagline = (description: string, fallback: string): string => {
  const first = (description.split(/(?<=[.!?])\s/)[0] ?? description).replace(/[.!?]$/, '').trim();
  if (first.length <= 90) return first;
  const clause = first.split(/,\s|;\s|\s[—–]\s/)[0]?.trim() ?? '';
  return clause.length >= 12 && clause.length <= 90 ? clause : fallback;
};

/**
 * Drafts a listing for each plugin in the repo. `existing` maps the ids already in plugins/ to their
 * plugin.json, for clashes and for skipping plugins already listed from this repo.
 */
export async function draftListings(
  repo: string,
  hint: RequestFirmware | null,
  remote: Remote,
  existing: ReadonlyMap<string, Record<string, unknown>>,
  options: { onlyFiles?: boolean; ignore?: (path: string) => boolean } = {},
): Promise<DraftResult> {
  const result: DraftResult = { repo, drafts: [], skipped: [], problems: [] };
  const meta = (await remote.api(`repos/${repo}`)) as {
    full_name?: string;
    description?: string | null;
    default_branch?: string;
    private?: boolean;
    license?: { spdx_id?: string } | null;
    owner?: { login?: string };
  } | null;
  if (meta === null || meta.private) {
    result.problems.push(`https://github.com/${repo} isn't a public repo co-op can read.`);
    return result;
  }
  const fullName = meta.full_name ?? repo;
  result.repo = fullName;
  const branch = meta.default_branch ?? 'main';
  const treeJson = (await remote.api(`repos/${fullName}/git/trees/${encodeURIComponent(branch)}?recursive=1`)) as { tree?: { path: string; type: string }[] } | null;
  const tree: Tree = { branch, paths: (treeJson?.tree ?? []).filter((t) => t.type === 'blob').map((t) => t.path) };
  const release = (await remote.api(`repos/${fullName}/releases/latest`)) as { tag_name?: string } | null;
  const owner = meta.owner?.login ?? fullName.split('/')[0] ?? 'unknown';

  const readmes = new Map<string, string | null>();
  const readme = async (path: string | null): Promise<string | null> => {
    if (path === null) return null;
    if (!readmes.has(path)) readmes.set(path, text(await remote.download(raw(fullName, branch, path))));
    return readmes.get(path) ?? null;
  };
  const rootReadmePath = nearestReadme(tree.paths, '');
  const rootReadme = (await readme(rootReadmePath)) ?? '';

  let found = findPluginFiles(tree.paths).filter((f) => !options.ignore?.(f.path));
  if (found.length > MAX_PLUGINS) {
    result.problems.push(`The repo has ${found.length} plugin files; only the first ${MAX_PLUGINS} were drafted.`);
    found = found.slice(0, MAX_PLUGINS);
  }
  const fileless = found.length === 0;
  // Watching a listed author's repo, only a new built file is a new plugin.
  if (fileless && options.onlyFiles) return result;
  if (fileless) {
    const firmware = firmwareFromProse(rootReadme, hint);
    if (firmware === null) {
      result.problems.push('No built plugin file (.knl, .opmod or .op1p) in the repo, and its README doesn’t say which firmware it is for.');
      return result;
    }
    found = [{ path: '', base: fullName.split('/')[1] ?? 'plugin', firmware }];
  }

  const manifests: OpusManifest[] = [];
  if (found.some((f) => f.firmware === 'op1/opus')) {
    // Its own folders first; a manifest under a build kit still counts when it names one of the repo's files.
    const all = tree.paths.filter((p) => /(^|-)manifest\.txt$/i.test(baseOf(p)));
    const ordered = [...all.filter((p) => !SKIP_DIR.test(dirOf(p))), ...all.filter((p) => SKIP_DIR.test(dirOf(p)))];
    for (const path of ordered.slice(0, 60)) {
      const source = text(await remote.download(raw(fullName, branch, path)));
      if (source !== null) manifests.push(parseOpusManifest(source));
    }
  }

  // Several plugins in one README folder: that README is read by section.
  const homeOf = (f: Found): string | null => nearestReadme(tree.paths, dirOf(f.path));
  // Counted by plugin, not by file: bloodmoon.knl and bloodmoon.opmod are one plugin.
  const sharing = new Map<string | null, Set<string>>();
  for (const f of found) sharing.set(homeOf(f), new Set([...(sharing.get(homeOf(f)) ?? []), norm(f.base)]));

  // The plugins already listed from this repo, by every name they go by, and who they are listed under.
  const fromRepo = [...existing.entries()].filter(([, m]) =>
    [m.source, m.link].some((u) => typeof u === 'string' && u.toLowerCase().startsWith(`https://github.com/${fullName.toLowerCase()}`)),
  );
  const listedNames = new Set(
    fromRepo.flatMap(([id, m]) => [id, id.replace(/^opus\./, ''), m.name, m.savedName].filter((v): v is string => typeof v === 'string').map(norm)),
  );
  const listedAuthors = [...new Set(fromRepo.map(([, m]) => m.author).filter((a): a is string => typeof a === 'string'))];
  const repoAuthor = listedAuthors.length === 1 ? listedAuthors[0] : undefined;
  const pages = (await remote.api(`repos/${fullName}/pages`)) as { html_url?: string; source?: { path?: string } } | null;

  const authors: string[] = [];
  const taken = new Set(existing.keys());
  /** Ids this run gave out, with the plugin they went to: another firmware's build of it shares the id. */
  const given = new Map<string, string>();
  const pending: { f: Found; manifest: OpusManifest | null; file: Uint8Array | null; info: PluginFileInfo | null }[] = [];
  for (const f of found) {
    // An .opmod carries its own manifest; a manifest.txt in the repo is the fallback.
    let manifest: OpusManifest | null = null;
    if (f.firmware === 'op1/opus') {
      const bytes = f.path ? await remote.download(raw(fullName, branch, f.path)) : null;
      manifest = (bytes && bytes.length <= 2 * 1024 * 1024 ? embeddedManifest(bytes) : null) ?? manifests.find((m) => manifestMatches(m, f.base)) ?? null;
    }
    // A .knl or .op1p says its own saved name, kind and author in its header.
    let file: Uint8Array | null = null;
    let info: PluginFileInfo | null = null;
    if ((f.firmware === 'op1/hundekraft' || f.firmware === 'op1/op1r') && f.path) {
      file = await remote.download(raw(fullName, branch, f.path));
      if (file && f.firmware === 'op1/hundekraft' && file.length <= MAX_KNL_BYTES) info = checkKnl(file).info;
      if (file && f.firmware === 'op1/op1r' && file.length <= MAX_OP1P_BYTES) info = checkOp1p(file).info;
      if (info?.author) authors.push(info.author);
    }
    if (manifest?.author && !/\s\/\s|\bby\b/i.test(manifest.author)) authors.push(manifest.author);
    pending.push({ f, manifest, file, info });
  }
  // The author most of the repo's plugins name, for one whose own credit is a list of people.
  const counts = new Map<string, number>();
  for (const a of authors) counts.set(a, (counts.get(a) ?? 0) + 1);
  const usual = [...counts.entries()].sort((a, b) => b[1] - a[1])[0]?.[0];

  for (const { f, manifest, file, info: knlInfo } of pending) {
    const notes: string[] = [];
    const homePath = fileless ? rootReadmePath : homeOf(f);
    const homeText = (await readme(homePath)) ?? '';
    const shared = (sharing.get(homePath)?.size ?? 0) > 1;
    const homeDir = homePath === null ? '' : dirOf(homePath);

    if (file && !knlInfo) notes.push(`\`${f.path}\` fails co-op's check of a ${FIRMWARE_LABEL[f.firmware]} plugin file, so it gets no download`);
    const nameGuess = manifest?.name ?? (shared ? null : headingName(homeText)) ?? titleCase(f.base);
    const name = nameGuess.slice(0, 60);
    const section = shared ? sectionFor(homeText, [name, f.base]) : null;
    const prose = section ?? homeText;

    const kind: PluginKind = opusKind(manifest?.adds) ?? knlInfo?.kind ?? kindFromProse(prose);
    if (!manifest?.adds && !knlInfo?.kind) notes.push(`kind \`${kind}\` is a guess from the README`);

    if ([f.base, name, manifest?.id ?? '', manifest?.id?.replace(/^opus\./, '') ?? ''].some((n) => n !== '' && listedNames.has(norm(n)))) {
      result.skipped.push(idFrom(f.base, f.firmware));
      continue;
    }

    const credited = manifest?.author ?? knlInfo?.author;
    let author = repoAuthor ?? credited ?? usual ?? owner;
    if (/\s\/\s|\bby\b/i.test(author)) author = usual ?? owner;
    author = author.slice(0, 60);
    if (repoAuthor !== undefined && credited !== undefined && norm(credited) !== norm(repoAuthor)) {
      notes.push(`listed under \`${repoAuthor}\` like the repo's other plugins; its own manifest says \`${credited}\``);
    } else if (credited === undefined && repoAuthor === undefined) {
      notes.push(`author \`${author}\` is the repo owner; check the README's credits`);
    }
    if (manifest?.needs) {
      const [needId] = manifest.needs.split(/\s+/);
      const needed = pending.find((p) => p.manifest?.id === needId)?.manifest?.name ?? existing.get(needId ?? '')?.name ?? needId;
      notes.push(`it needs ${String(needed)} (\`${manifest.needs}\`): say so in the manual`);
    }

    let id: string;
    let savedName: string | undefined;
    if (f.firmware === 'op1/opus' && manifest?.id?.startsWith('opus.') && OPUS_ID.test(manifest.id)) {
      id = manifest.id;
      savedName = manifest.id;
    } else if (f.firmware === 'op1/hundekraft' && knlInfo?.savedName && HUNDEKRAFT_ID.test(knlInfo.savedName)) {
      id = knlInfo.savedName;
    } else if (f.firmware === 'op1/op1r' && knlInfo?.savedName && HUNDEKRAFT_ID.test(idFromSavedName(knlInfo.savedName))) {
      // op1r sounds store the header's name, which may have spaces ("pitch delay"): the id has _ for them.
      id = idFromSavedName(knlInfo.savedName);
      savedName = knlInfo.savedName;
    } else {
      id = idFrom(f.base, f.firmware);
    }
    const same = existing.get(id);
    const sameRepo = (m: Record<string, unknown> | undefined): boolean =>
      m !== undefined && [m.source, m.link].some((u) => typeof u === 'string' && u.toLowerCase().startsWith(`https://github.com/${fullName.toLowerCase()}`));
    if (sameRepo(same)) {
      result.skipped.push(id);
      continue;
    }
    let pinned = true;
    if (taken.has(id) && given.get(id) !== norm(f.base)) {
      const max = f.firmware === 'op1/hundekraft' ? 15 : 40;
      const suffix = `_${idFrom(owner, 'op1/opus')}`;
      let candidate = `${id.slice(0, Math.max(1, max - suffix.length))}${suffix}`.slice(0, max);
      for (let n = 2; taken.has(candidate); n++) candidate = `${id.slice(0, max - String(n).length - 1)}_${n}`;
      notes.push(`\`${id}\` is taken by a different plugin, so this one is \`${candidate}\``);
      id = candidate;
      // An op1r listing keeps its saved name under another id; the others' ids are their saved names.
      if (f.firmware !== 'op1/op1r') savedName = undefined;
      pinned = f.firmware === 'op1/op1r';
    }
    taken.add(id);
    given.set(id, norm(f.base));

    // Its own folder, or the repo's release page, or the repo.
    const singleRelease = found.length === 1 && release?.tag_name !== undefined;
    // A file the repo's GitHub Pages site publishes (docs/downloads/x.opmod under a /docs site) is got from that site.
    const pagesDir = pages?.source?.path?.replace(/^\/+|\/+$/g, '');
    const onPages = pages?.html_url?.startsWith('https://') === true && pagesDir !== undefined && pagesDir !== '' && f.path.startsWith(`${pagesDir}/`);
    const link =
      homeDir !== '' && !fileless
        ? `https://github.com/${fullName}/tree/${encodeURIComponent(branch)}/${encodePath(homeDir)}`
        : onPages && pages?.html_url
          ? pages.html_url
          : singleRelease
            ? `https://github.com/${fullName}/releases/latest`
            : `https://github.com/${fullName}`;
    // The repo's release stream is one plugin's only when it holds one plugin (plugin-updates follows it).
    const version = found.length === 1 && release?.tag_name && /^v?\d+(\.\d+)*$/.test(release.tag_name) ? release.tag_name.replace(/^v/, '') : undefined;

    // The plugin folder's own licence file, else the repo's.
    const licensePath = tree.paths
      .filter((p) => (homeDir === '' ? dirOf(p) === '' : p.startsWith(`${homeDir}/`)) && !SKIP_DIR.test(dirOf(p)) && /^(licen[cs]e|copying)(\.md|\.txt)?$/i.test(baseOf(p)))
      .sort((a, b) => a.split('/').length - b.split('/').length)[0];
    const licenseText = licensePath === undefined ? null : text(await remote.download(raw(fullName, branch, licensePath)));
    const license =
      licenseOf(homeDir === '' ? meta.license?.spdx_id : null, licenseText, `${prose} ${homeText}`) ?? licenseOf(meta.license?.spdx_id, null, rootReadme);
    if (license === null) notes.push('no licence co-op recognises, so none is listed');

    const row = shared ? tableRowFor(homeText, [name, f.base]) : null;
    const first = firstParagraph(prose);
    const about = shared && first !== null ? aboutName(first, [name, f.base]) : null;
    const paragraph = row ?? about ?? first ?? (shared ? null : firstParagraph(rootReadme)) ?? meta.description ?? null;
    const description = clip(paragraph ?? `${name} is a ${kind === 'fx' ? 'effect' : kind} for ${FIRMWARE_LABEL[f.firmware]} on the original OP-1.`);
    const requires = requiresFrom(f.firmware, prose, manifest?.core) ?? requiresFrom(f.firmware, `${homeText} ${rootReadme}`, undefined);

    const manifestOut: Record<string, unknown> = {
      format: 1,
      id,
      name,
      firmware: f.firmware,
      kind,
      author,
      description,
      ...(license ? { license } : {}),
      link,
      ...(version ? { version } : {}),
      screenshot: 'screenshot.png',
      manual: 'manual.md',
      source: `https://github.com/${fullName}`,
      ...(requires ? { requires } : {}),
      ...(savedName ? { savedName } : {}),
      tags: tagsFor(kind, f.firmware, `${name} ${description}`),
    };

    // A .knl committed in the repo installs over USB from its card, pinned to the commit that last changed it. An
    // .op1p is downloaded from its card the same way; op1r keeps it under its own file name, whatever the id.
    const knlPin = f.firmware === 'op1/hundekraft' && knlInfo?.savedName === id && baseOf(f.path) === `${id}.knl`;
    const op1pPin = f.firmware === 'op1/op1r' && knlInfo !== null && OP1P_FILE.test(baseOf(f.path));
    if (file && knlInfo && pinned && (knlPin || op1pPin)) {
      const commits = (await remote.api(`repos/${fullName}/commits?path=${encodeURIComponent(f.path)}&per_page=1`)) as
        | { sha?: string; commit?: { committer?: { date?: string } } }[]
        | null;
      const sha = commits?.[0]?.sha;
      // An .op1p has no version of its own: the release's, else the day the file last changed (plugin-updates moves it on).
      const day = /^\d{4}-\d{2}-\d{2}/.exec(commits?.[0]?.commit?.committer?.date ?? '')?.[0];
      const fileVersion = knlInfo.version ?? version ?? (op1pPin ? day : undefined);
      if (sha && fileVersion) {
        manifestOut.version = fileVersion;
        manifestOut.fileUrl = `https://raw.githubusercontent.com/${fullName}/${sha}/${encodePath(f.path)}`;
        manifestOut.sha256 = createHash('sha256').update(file).digest('hex');
        manifestOut.sizeBytes = file.length;
        if (op1pPin && fileVersion === day) notes.push(`version \`${day}\` is the day the .op1p last changed; the repo has no release for it`);
      }
    }

    const src = firstScreen(section ?? homeText, [name, f.base]);
    const named = src === null ? imageNamed(tree.paths, [name, f.base]) : null;
    const screenshot = named !== null ? raw(fullName, branch, named) : src === null || homePath === null ? null : imageUrl(src, fullName, branch, homePath);
    if (screenshot === null) notes.push('no screen image in the README, so the card is a name card');
    else if (named !== null) notes.push(`screenshot from \`${named}\`, an image named after it`);

    const homeUrl = homePath === null ? null : `https://github.com/${fullName}/blob/${encodeURIComponent(branch)}/${encodePath(homePath)}`;
    const manual = draftManual({ name, tagline: tagline(description, `A${kind === 'fx' ? 'n effect' : kind === 'synth' ? ' synth' : ' plugin'} for ${FIRMWARE_LABEL[f.firmware]}`), description, author, repo: fullName, license, firmware: f.firmware, requires, link, home: homeUrl });
    notes.push('the description and manual are drafts from the README');
    result.drafts.push({ id, manifest: manifestOut, manual, screenshot, notes, base: f.base, path: f.path });
  }
  result.drafts = joinBuilds(result.drafts);
  return result;
}

/** The fields that belong to one firmware's build of a plugin. */
const BUILD_FIELDS = ['firmware', 'link', 'version', 'requires', 'fileUrl', 'sha256', 'sizeBytes', 'savedName'] as const;

/**
 * One plugin built for several firmwares (bloodmoon.knl and bloodmoon.opmod) is one listing with a build
 * per firmware: the hundekraft build's id and text lead, since its id has the stricter rule.
 */
export function joinBuilds(drafts: readonly Draft[]): Draft[] {
  const groups = new Map<string, Draft[]>();
  for (const d of drafts) groups.set(norm(d.base), [...(groups.get(norm(d.base)) ?? []), d]);
  const out: Draft[] = [];
  for (const group of groups.values()) {
    const firmwares = new Set(group.map((d) => d.manifest.firmware));
    if (group.length < 2 || firmwares.size !== group.length) {
      out.push(...group);
      continue;
    }
    const order = ['op1/hundekraft', 'op1/opus', 'op1/coop', 'op1/op1r'];
    const sorted = [...group].sort((a, b) => order.indexOf(String(a.manifest.firmware)) - order.indexOf(String(b.manifest.firmware)));
    const lead = sorted[0] as Draft;
    const shared: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(lead.manifest)) if (!(BUILD_FIELDS as readonly string[]).includes(k)) shared[k] = v;
    shared.builds = sorted.map((d) => {
      const build: Record<string, unknown> = {};
      for (const k of BUILD_FIELDS) if (d.manifest[k] !== undefined) build[k] = d.manifest[k];
      return build;
    });
    shared.tags = [...new Set(sorted.flatMap((d) => d.manifest.tags as string[]))].filter((t) => !['hundekraft', 'opus', 'coop', 'op1r'].includes(t)).slice(0, 8);
    out.push({
      ...lead,
      manifest: shared,
      notes: [`one listing for its ${sorted.map((d) => String(d.manifest.firmware).slice(4)).join(' and ')} builds`, ...new Set(sorted.flatMap((d) => d.notes))],
    });
  }
  return out;
}
