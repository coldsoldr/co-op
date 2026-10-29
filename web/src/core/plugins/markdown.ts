/**
 * A small Markdown parser for plugin manuals, written for safety rather than completeness. It
 * turns the source into a tree of plain data (blocks and inlines) that the UI renders as React
 * elements, so nothing in a manual ever reaches the page as HTML: raw HTML stays text, links go
 * only to http(s) or into the plugin's own folder, and images come only from that folder.
 *
 * Supported: ATX headings, paragraphs, hard breaks (two trailing spaces), emphasis and strong
 * (`*` and `_`, nested), inline code, fenced code blocks, ordered and unordered lists (nested),
 * GFM tables, block quotes, rules, links, images, `<https://...>` and bare http(s) URLs, and
 * backslash escapes. Not supported (shown as text): raw HTML, reference links, setext headings,
 * indented code blocks, footnotes.
 */

export type Inline =
  | { type: 'text'; text: string }
  | { type: 'code'; text: string }
  | { type: 'em'; children: Inline[] }
  | { type: 'strong'; children: Inline[] }
  /** `external` links leave the app (http or https); the others point into the plugin's folder. */
  | { type: 'link'; href: string; external: boolean; children: Inline[] }
  | { type: 'image'; src: string; alt: string }
  | { type: 'break' };

export type Align = 'left' | 'center' | 'right' | null;

export type Block =
  | { type: 'heading'; level: 1 | 2 | 3 | 4 | 5 | 6; children: Inline[] }
  | { type: 'paragraph'; children: Inline[] }
  | { type: 'code'; lang: string; text: string }
  /** A tight list has no blank lines between or inside its items; its paragraphs render bare. */
  | { type: 'list'; ordered: boolean; start: number; tight: boolean; items: Block[][] }
  | { type: 'table'; align: Align[]; head: Inline[][]; rows: Inline[][][] }
  | { type: 'quote'; children: Block[] }
  | { type: 'rule' };

/** Deeper lists, quotes or links than this are shown as text, so hostile input cannot overflow the stack. */
const MAX_DEPTH = 8;
/** How far a link's `]` may be from its `[`; longer link texts are not links. */
const MAX_LINK_TEXT = 1000;

/** A path inside the plugin folder: plain segments, none starting with a dot, no scheme, no leading slash. */
const RELATIVE_PATH = /^[A-Za-z0-9_-][A-Za-z0-9_.-]*(?:\/[A-Za-z0-9_-][A-Za-z0-9_.-]*)*$/;

function inFolder(path: string, folderUrl: string): string | null {
  if (!RELATIVE_PATH.test(path)) return null;
  return folderUrl + path.split('/').map(encodeURIComponent).join('/');
}

/**
 * Where a manual link may go: an http(s) URL (external), or a relative path resolved into the
 * plugin's folder (`folderUrl`, ending in a slash). Anything else, including every other scheme,
 * a leading slash or a `..` segment, returns null and the link is shown as its text.
 */
export function safeLinkTarget(href: string, folderUrl: string): { href: string; external: boolean } | null {
  if (/^https?:\/\//i.test(href)) {
    try {
      const url = new URL(href);
      if ((url.protocol === 'http:' || url.protocol === 'https:') && url.hostname !== '') return { href: url.href, external: true };
    } catch {
      // Not a URL; refused below.
    }
    return null;
  }
  const local = inFolder(href, folderUrl);
  return local === null ? null : { href: local, external: false };
}

/** Where a manual image may come from: the plugin's own folder only, so a manual cannot track its readers. */
export function safeImageSource(src: string, folderUrl: string): string | null {
  return inFolder(src, folderUrl);
}

/** Parses a manual. Relative links and images resolve against `folderUrl`, the plugin folder's URL with a trailing slash. */
export function parseMarkdown(source: string, folderUrl: string): Block[] {
  const lines = source.replace(/\r\n?/g, '\n').split('\n').map(expandIndent);
  return parseBlocks(lines, { folderUrl, depth: 0 });
}

interface Context {
  folderUrl: string;
  depth: number;
}

// ---------------------------------------------------------------------------------------------
// Blocks

const FENCE = /^( {0,3})(`{3,}|~{3,})[ \t]*([^`\s]*)[^`]*$/;
const HEADING = /^ {0,3}(#{1,6})(?:[ \t]+(.*?))?(?:[ \t]+#+)?[ \t]*$/;
const RULE = /^ {0,3}([-*_])(?:[ \t]*\1){2,}[ \t]*$/;
const QUOTE = /^ {0,3}> ?/;
const LIST_ITEM = /^( {0,3})([-*+]|\d{1,9}[.)])( +|$)(.*)$/;
const DELIMITER_CELL = /^:?-+:?$/;

/** Leading tabs as four spaces, so indentation can be counted in spaces. */
function expandIndent(line: string): string {
  const lead = /^[ \t]*/.exec(line)?.[0] ?? '';
  return lead.includes('\t') ? lead.replace(/\t/g, '    ') + line.slice(lead.length) : line;
}

const isBlank = (line: string): boolean => line.trim() === '';
const indentOf = (line: string): number => /^ */.exec(line)?.[0].length ?? 0;

/** True for a line that starts a block other than a paragraph or table, which ends a paragraph. */
function startsBlock(line: string): boolean {
  return FENCE.test(line) || HEADING.test(line) || RULE.test(line) || QUOTE.test(line) || isListStart(line);
}

function isListStart(line: string): boolean {
  const m = LIST_ITEM.exec(line);
  return m !== null && (m[4] ?? '') !== '';
}

function parseBlocks(lines: readonly string[], ctx: Context): Block[] {
  const blocks: Block[] = [];
  let paragraph: string[] = [];
  const flush = (): void => {
    if (paragraph.length === 0) return;
    blocks.push({ type: 'paragraph', children: parseInline(paragraph.join('\n').trimEnd(), ctx) });
    paragraph = [];
  };
  const nested = ctx.depth < MAX_DEPTH;

  let i = 0;
  while (i < lines.length) {
    const line = lines[i] as string;
    if (isBlank(line)) {
      flush();
      i++;
      continue;
    }

    const fence = FENCE.exec(line);
    if (fence) {
      flush();
      const [, indent = '', marker = '', lang = ''] = fence;
      const close = new RegExp(`^ {0,3}${marker[0] === '`' ? '`' : '~'}{${marker.length},}[ \\t]*$`);
      const body: string[] = [];
      i++;
      while (i < lines.length && !close.test(lines[i] as string)) {
        const content = lines[i] as string;
        body.push(content.slice(Math.min(indent.length, indentOf(content))));
        i++;
      }
      i++; // the closing fence, or past the end
      blocks.push({ type: 'code', lang, text: body.join('\n') });
      continue;
    }

    const heading = HEADING.exec(line);
    if (heading) {
      flush();
      const level = (heading[1] as string).length as 1 | 2 | 3 | 4 | 5 | 6;
      blocks.push({ type: 'heading', level, children: parseInline(heading[2] ?? '', ctx) });
      i++;
      continue;
    }

    if (RULE.test(line)) {
      flush();
      blocks.push({ type: 'rule' });
      i++;
      continue;
    }

    if (nested && QUOTE.test(line)) {
      flush();
      const body: string[] = [];
      while (i < lines.length && QUOTE.test(lines[i] as string)) {
        body.push((lines[i] as string).replace(QUOTE, ''));
        i++;
      }
      blocks.push({ type: 'quote', children: parseBlocks(body, { ...ctx, depth: ctx.depth + 1 }) });
      continue;
    }

    if (nested && isListStart(line)) {
      flush();
      const list = parseList(lines, i, ctx);
      blocks.push(list.block);
      i = list.next;
      continue;
    }

    const next = lines[i + 1];
    if (line.includes('|') && next !== undefined) {
      const head = splitRow(line);
      const delimiter = splitRow(next);
      if (
        next.includes('|') &&
        delimiter.length === head.length &&
        delimiter.every((cell) => DELIMITER_CELL.test(cell.trim()))
      ) {
        flush();
        const table = parseTable(lines, i, head, delimiter, ctx);
        blocks.push(table.block);
        i = table.next;
        continue;
      }
    }

    paragraph.push(line.replace(/^\s+/, ''));
    i++;
  }
  flush();
  return blocks;
}

/** A list item marker's type: bullets of one character, or numbers with one delimiter, belong together. */
const markerType = (marker: string): string => (/^\d/.test(marker) ? marker.slice(-1) : marker);

function parseList(lines: readonly string[], from: number, ctx: Context): { block: Block; next: number } {
  const first = LIST_ITEM.exec(lines[from] as string) as RegExpExecArray;
  const type = markerType(first[2] as string);
  const ordered = /^\d/.test(first[2] as string);
  const start = ordered ? Number.parseInt(first[2] as string, 10) : 1;
  const items: string[][] = [];
  let tight = true;

  let i = from;
  while (i < lines.length) {
    const m = LIST_ITEM.exec(lines[i] as string);
    if (!m || markerType(m[2] as string) !== type) break;
    const [, indent = '', marker = '', spaces = '', rest = ''] = m;
    // Five or more spaces after the marker start indented content; count only one of them.
    const gap = spaces.length >= 5 || rest === '' ? 1 : spaces.length;
    const contentIndent = indent.length + marker.length + gap;
    const item = [spaces.length >= 5 ? ' '.repeat(spaces.length - 1) + rest : rest];
    i++;
    while (i < lines.length) {
      const line = lines[i] as string;
      if (isBlank(line)) {
        // A blank line stays in the item only when the item goes on, indented, after it.
        let k = i;
        while (k < lines.length && isBlank(lines[k] as string)) k++;
        if (k === lines.length || indentOf(lines[k] as string) < contentIndent) break;
        for (; i < k; i++) item.push('');
        tight = false;
        continue;
      }
      if (indentOf(line) >= contentIndent) {
        item.push(line.slice(contentIndent));
      } else if (!isBlank(item[item.length - 1] as string) && !startsBlock(line)) {
        // A lazy continuation line of the item's last paragraph.
        item.push(line.trim());
      } else {
        break;
      }
      i++;
    }
    items.push(item);

    // Blank lines between items make the list loose; anything else after them ends it.
    let k = i;
    while (k < lines.length && isBlank(lines[k] as string)) k++;
    if (k > i) {
      const m2 = k < lines.length ? LIST_ITEM.exec(lines[k] as string) : null;
      if (!m2 || markerType(m2[2] as string) !== type || (m2[1] as string).length > indent.length) break;
      tight = false;
      i = k;
    }
  }

  const inner = { ...ctx, depth: ctx.depth + 1 };
  return { block: { type: 'list', ordered, start, tight, items: items.map((item) => parseBlocks(item, inner)) }, next: i };
}

/** The cells of a table row, split at pipes that are not escaped; the outer pipes are optional. */
function splitRow(line: string): string[] {
  let row = line.trim();
  if (row.startsWith('|')) row = row.slice(1);
  if (row.endsWith('|') && !row.endsWith('\\|')) row = row.slice(0, -1);
  const cells: string[] = [];
  let cell = '';
  for (let i = 0; i < row.length; i++) {
    const c = row[i] as string;
    if (c === '\\' && i + 1 < row.length) {
      cell += c + (row[i + 1] as string);
      i++;
    } else if (c === '|') {
      cells.push(cell);
      cell = '';
    } else {
      cell += c;
    }
  }
  cells.push(cell);
  return cells;
}

function parseTable(
  lines: readonly string[],
  from: number,
  head: string[],
  delimiter: string[],
  ctx: Context,
): { block: Block; next: number } {
  const align = delimiter.map((raw): Align => {
    const cell = raw.trim();
    const left = cell.startsWith(':');
    const right = cell.endsWith(':');
    return left && right ? 'center' : right ? 'right' : left ? 'left' : null;
  });
  const cells = (row: string[]): Inline[][] =>
    head.map((_, c) => {
      const raw = row[c];
      return raw === undefined ? [] : parseInline(raw.trim(), ctx);
    });
  const rows: Inline[][][] = [];
  let i = from + 2;
  while (i < lines.length) {
    const line = lines[i] as string;
    if (isBlank(line) || !line.includes('|') || startsBlock(line)) break;
    rows.push(cells(splitRow(line)));
    i++;
  }
  return { block: { type: 'table', align, head: cells(head), rows }, next: i };
}

// ---------------------------------------------------------------------------------------------
// Inlines

/** A run of `*` or `_` that may open or close emphasis, as in CommonMark's delimiter runs. */
interface Delimiter {
  type: 'delimiter';
  char: '*' | '_';
  count: number;
  open: boolean;
  close: boolean;
}

type Item = Inline | Delimiter;

const PUNCTUATION = /[\p{P}\p{S}]/u;
const ASCII_PUNCTUATION = /[!-/:-@[-`{-~]/;
const BARE_URL = /^https?:\/\/[^\s<>]+/i;
const AUTOLINK = /^<(https?:\/\/[^\s<>]*)>/i;

const isSpace = (c: string | undefined): boolean => c === undefined || /\s/.test(c);
const isPunct = (c: string | undefined): boolean => c !== undefined && PUNCTUATION.test(c);

function parseInline(source: string, ctx: Context): Inline[] {
  const items: Item[] = [];
  let buffer = '';
  const flushText = (): void => {
    if (buffer !== '') items.push({ type: 'text', text: buffer });
    buffer = '';
  };
  const push = (...nodes: Inline[]): void => {
    flushText();
    items.push(...nodes);
  };

  let i = 0;
  while (i < source.length) {
    const c = source[i] as string;

    if (c === '\\') {
      const next = source[i + 1];
      if (next !== undefined && ASCII_PUNCTUATION.test(next)) {
        buffer += next;
        i += 2;
        continue;
      }
      if (next === '\n') {
        push({ type: 'break' });
        i += 2;
        continue;
      }
    }

    if (c === '`') {
      const run = /^`+/.exec(source.slice(i))?.[0] as string;
      const close = findBacktickRun(source, i + run.length, run.length);
      if (close !== -1) {
        let code = source.slice(i + run.length, close).replace(/\n/g, ' ');
        if (code.length >= 2 && code.startsWith(' ') && code.endsWith(' ') && code.trim() !== '') code = code.slice(1, -1);
        push({ type: 'code', text: code });
        i = close + run.length;
      } else {
        buffer += run;
        i += run.length;
      }
      continue;
    }

    if ((c === '[' || (c === '!' && source[i + 1] === '[')) && ctx.depth < MAX_DEPTH) {
      const image = c === '!';
      const link = parseLink(source, image ? i + 1 : i);
      if (link) {
        if (image) {
          const src = safeImageSource(link.destination, ctx.folderUrl);
          const alt = plainText(parseInline(link.text, { ...ctx, depth: ctx.depth + 1 }));
          if (src !== null) push({ type: 'image', src, alt });
          else buffer += alt;
        } else {
          const children = parseInline(link.text, { ...ctx, depth: ctx.depth + 1 });
          const target = safeLinkTarget(link.destination, ctx.folderUrl);
          push(...(target ? [{ type: 'link', ...target, children } as Inline] : children));
        }
        i = link.end;
        continue;
      }
    }

    if (c === '<') {
      const auto = AUTOLINK.exec(source.slice(i));
      const target = auto ? safeLinkTarget(auto[1] as string, ctx.folderUrl) : null;
      if (auto && target) {
        push({ type: 'link', ...target, children: [{ type: 'text', text: auto[1] as string }] });
        i += auto[0].length;
        continue;
      }
    }

    if ((c === 'h' || c === 'H') && !/[\p{L}\p{N}]/u.test(source[i - 1] ?? ' ')) {
      const bare = BARE_URL.exec(source.slice(i));
      if (bare) {
        const url = trimUrl(bare[0]);
        const target = safeLinkTarget(url, ctx.folderUrl);
        if (target) {
          push({ type: 'link', ...target, children: [{ type: 'text', text: url }] });
          i += url.length;
          continue;
        }
      }
    }

    if (c === '*' || c === '_') {
      const run = (c === '*' ? /^\*+/ : /^_+/).exec(source.slice(i))?.[0] as string;
      const before = source[i - 1];
      const after = source[i + run.length];
      const left = !isSpace(after) && (!isPunct(after) || isSpace(before) || isPunct(before));
      const right = !isSpace(before) && (!isPunct(before) || isSpace(after) || isPunct(after));
      flushText();
      items.push({
        type: 'delimiter',
        char: c,
        count: run.length,
        // `_` does not open or close inside a word, so snake_case stays as it is.
        open: c === '*' ? left : left && (!right || isPunct(before)),
        close: c === '*' ? right : right && (!left || isPunct(after)),
      });
      i += run.length;
      continue;
    }

    if (c === '\n') {
      // Two or more trailing spaces make a hard break; otherwise the line break is a space.
      const hard = / {2,}$/.test(buffer);
      buffer = buffer.replace(/ +$/, '');
      if (hard) push({ type: 'break' });
      else buffer += ' ';
      i++;
      while (source[i] === ' ') i++;
      continue;
    }

    buffer += c;
    i++;
  }
  flushText();
  return resolveEmphasis(items);
}

/** The index of the next run of exactly `length` backticks at or after `from`, or -1. */
function findBacktickRun(source: string, from: number, length: number): number {
  let i = source.indexOf('`', from);
  while (i !== -1) {
    let end = i;
    while (source[end] === '`') end++;
    if (end - i === length) return i;
    i = source.indexOf('`', end);
  }
  return -1;
}

/** Drops trailing punctuation from a bare URL, and a closing parenthesis it does not open. */
function trimUrl(url: string): string {
  let out = url.replace(/[.,:;!?'"*_]+$/, '');
  while (out.endsWith(')') && (out.match(/\(/g)?.length ?? 0) < (out.match(/\)/g)?.length ?? 0)) {
    out = out.slice(0, -1).replace(/[.,:;!?'"*_]+$/, '');
  }
  return out;
}

/**
 * Parses `[text](destination "optional title")` at `from` (the `[`). Returns the raw text, the
 * destination and the index after the `)`, or null when it is not a link.
 */
function parseLink(source: string, from: number): { text: string; destination: string; end: number } | null {
  let depth = 0;
  let close = -1;
  const limit = Math.min(source.length, from + MAX_LINK_TEXT);
  for (let i = from; i < limit; i++) {
    const c = source[i];
    if (c === '\\') i++;
    else if (c === '[') depth++;
    else if (c === ']' && --depth === 0) {
      close = i;
      break;
    }
  }
  if (close === -1 || source[close + 1] !== '(') return null;

  let i = close + 2;
  const skipSpace = (): void => {
    while (i < source.length && /[ \t\n]/.test(source[i] as string)) i++;
  };
  skipSpace();
  let destination = '';
  if (source[i] === '<') {
    const end = source.indexOf('>', i);
    if (end === -1 || source.slice(i + 1, end).includes('\n')) return null;
    destination = source.slice(i + 1, end);
    i = end + 1;
  } else {
    let parens = 0;
    while (i < source.length) {
      const c = source[i] as string;
      if (/\s/.test(c)) break;
      if (c === '(') parens++;
      if (c === ')' && parens-- === 0) break;
      destination += c;
      i++;
    }
  }
  skipSpace();
  const quote = source[i];
  if (quote === '"' || quote === "'" || quote === '(') {
    const end = source.indexOf(quote === '(' ? ')' : quote, i + 1);
    if (end === -1) return null;
    i = end + 1;
    skipSpace();
  }
  if (source[i] !== ')') return null;
  return { text: source.slice(from + 1, close), destination, end: i + 1 };
}

/** Matches openers to closers into em and strong, innermost first, as CommonMark does (without its rule of three). */
function resolveEmphasis(items: Item[]): Inline[] {
  // Per character, the lowest index worth searching for an opener: below it every search failed.
  const bottom = { '*': -1, _: -1 };
  let i = 0;
  while (i < items.length) {
    const closer = items[i] as Item;
    if (closer.type !== 'delimiter' || !closer.close || closer.count === 0) {
      i++;
      continue;
    }
    let j = i - 1;
    for (; j > bottom[closer.char]; j--) {
      const opener = items[j] as Item;
      if (opener.type === 'delimiter' && opener.char === closer.char && opener.open && opener.count > 0) break;
    }
    if (j <= bottom[closer.char]) {
      bottom[closer.char] = i - 1;
      i++;
      continue;
    }
    const opener = items[j] as Delimiter;
    const use = opener.count >= 2 && closer.count >= 2 ? 2 : 1;
    opener.count -= use;
    closer.count -= use;
    const children = finish(items.slice(j + 1, i));
    items.splice(j + 1, i - j - 1, use === 2 ? { type: 'strong', children } : { type: 'em', children });
    i = j + 2;
    if (opener.count === 0) {
      items.splice(j, 1);
      i--;
    }
    if (closer.count === 0) items.splice(i, 1);
  }
  return finish(items);
}

/** Unmatched delimiters back to text, and neighbouring text joined. */
function finish(items: readonly Item[]): Inline[] {
  const out: Inline[] = [];
  for (const item of items) {
    const node: Inline | null =
      item.type === 'delimiter' ? (item.count > 0 ? { type: 'text', text: item.char.repeat(item.count) } : null) : item;
    if (node === null) continue;
    const last = out[out.length - 1];
    if (node.type === 'text' && last?.type === 'text') out[out.length - 1] = { type: 'text', text: last.text + node.text };
    else out.push(node);
  }
  return out;
}

/** The text of inlines without their markup, for an image's alt text. */
function plainText(inlines: readonly Inline[]): string {
  return inlines
    .map((node) => {
      switch (node.type) {
        case 'text':
        case 'code':
          return node.text;
        case 'image':
          return node.alt;
        case 'break':
          return ' ';
        default:
          return plainText(node.children);
      }
    })
    .join('');
}
