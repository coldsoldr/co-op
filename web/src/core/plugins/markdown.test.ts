import { describe, it, expect } from 'vitest';
import { parseMarkdown, safeImageSource, safeLinkTarget, type Block, type Inline } from './markdown';

const FOLDER = '/co-op/plugins/rings/';
const parse = (source: string): Block[] => parseMarkdown(source, FOLDER);
const text = (value: string): Inline => ({ type: 'text', text: value });
/** The inlines of the only block, which must be a paragraph. */
function inlines(source: string): Inline[] {
  const blocks = parse(source);
  expect(blocks).toHaveLength(1);
  const [block] = blocks;
  if (block?.type !== 'paragraph') throw new Error(`expected a paragraph, got ${JSON.stringify(block)}`);
  return block.children;
}

describe('safeLinkTarget', () => {
  it('allows http and https links, as external', () => {
    expect(safeLinkTarget('https://hundekraft.github.io/op1-workshop/', FOLDER)).toEqual({
      href: 'https://hundekraft.github.io/op1-workshop/',
      external: true,
    });
    expect(safeLinkTarget('HTTP://example.com', FOLDER)).toEqual({ href: 'http://example.com/', external: true });
  });

  it('resolves a relative link into the plugin folder', () => {
    expect(safeLinkTarget('rings.knl', FOLDER)).toEqual({ href: '/co-op/plugins/rings/rings.knl', external: false });
    expect(safeLinkTarget('docs/more.md', FOLDER)).toEqual({ href: '/co-op/plugins/rings/docs/more.md', external: false });
  });

  it('refuses every other scheme and anything that could leave the folder', () => {
    for (const href of [
      'javascript:alert(1)',
      'JavaScript:alert(1)',
      ' javascript:alert(1)',
      'java\tscript:alert(1)',
      'jav&#x61;script:alert(1)',
      'data:text/html,<script>alert(1)</script>',
      'vbscript:msgbox',
      'mailto:a@example.com',
      'file:///etc/passwd',
      '//evil.example/x',
      '/etc/passwd',
      '../grids/grids.knl',
      'a/../../x',
      './x',
      'a\\b',
      '#controls',
      '',
      'https://',
    ]) {
      expect(safeLinkTarget(href, FOLDER), href).toBeNull();
    }
  });
});

describe('safeImageSource', () => {
  it('allows images from the plugin folder only', () => {
    expect(safeImageSource('screenshot.png', FOLDER)).toBe('/co-op/plugins/rings/screenshot.png');
    for (const src of [
      'https://example.com/a.png',
      'http://example.com/a.png',
      '//example.com/a.png',
      'data:image/png;base64,AAAA',
      'javascript:alert(1)',
      '../grids/screenshot.png',
      '/co-op/plugins/rings/screenshot.png',
    ]) {
      expect(safeImageSource(src, FOLDER), src).toBeNull();
    }
  });
});

describe('parseMarkdown blocks', () => {
  it('parses headings, paragraphs and rules', () => {
    expect(parse('# rings\n\nOne\ntwo.\n\n---\n\n### Third ###')).toEqual([
      { type: 'heading', level: 1, children: [text('rings')] },
      { type: 'paragraph', children: [text('One two.')] },
      { type: 'rule' },
      { type: 'heading', level: 3, children: [text('Third')] },
    ]);
  });

  it('needs a space after the hashes, and allows at most six', () => {
    expect(parse('#tag')).toEqual([{ type: 'paragraph', children: [text('#tag')] }]);
    expect(parse('####### seven')).toEqual([{ type: 'paragraph', children: [text('####### seven')] }]);
  });

  it('parses fenced code blocks verbatim, with the language', () => {
    expect(parse('```c\nint *a = **b;\n<b>x</b>\n```\nafter')).toEqual([
      { type: 'code', lang: 'c', text: 'int *a = **b;\n<b>x</b>' },
      { type: 'paragraph', children: [text('after')] },
    ]);
    // An unclosed fence runs to the end.
    expect(parse('~~~\na\n\nb')).toEqual([{ type: 'code', lang: '', text: 'a\n\nb' }]);
  });

  it('parses unordered and ordered lists with continuation lines', () => {
    const blocks = parse('- one\n  more\n- two\n\n3. three\n4. four');
    expect(blocks).toEqual([
      {
        type: 'list',
        ordered: false,
        start: 1,
        tight: true,
        items: [[{ type: 'paragraph', children: [text('one more')] }], [{ type: 'paragraph', children: [text('two')] }]],
      },
      {
        type: 'list',
        ordered: true,
        start: 3,
        tight: true,
        items: [[{ type: 'paragraph', children: [text('three')] }], [{ type: 'paragraph', children: [text('four')] }]],
      },
    ]);
  });

  it('parses nested lists and marks loose lists', () => {
    const [list] = parse('- a\n  - b\n  - c\n\n- d');
    expect(list).toEqual({
      type: 'list',
      ordered: false,
      start: 1,
      tight: false,
      items: [
        [
          { type: 'paragraph', children: [text('a')] },
          {
            type: 'list',
            ordered: false,
            start: 1,
            tight: true,
            items: [[{ type: 'paragraph', children: [text('b')] }], [{ type: 'paragraph', children: [text('c')] }]],
          },
        ],
        [{ type: 'paragraph', children: [text('d')] }],
      ],
    });
  });

  it('parses tables with alignment, padding short rows and dropping extra cells', () => {
    expect(parse('| Encoder | Main page | Press |\n|:---|:-:|--:|\n| blue | **structure** | `model` |\n| white | damping |\n| a | b | c | d |')).toEqual([
      {
        type: 'table',
        align: ['left', 'center', 'right'],
        head: [[text('Encoder')], [text('Main page')], [text('Press')]],
        rows: [
          [[text('blue')], [{ type: 'strong', children: [text('structure')] }], [{ type: 'code', text: 'model' }]],
          [[text('white')], [text('damping')], []],
          [[text('a')], [text('b')], [text('c')]],
        ],
      },
    ]);
  });

  it('keeps an escaped pipe inside a cell', () => {
    const [table] = parse('a | b\n--|--\nx \\| y | z');
    expect(table).toMatchObject({ rows: [[[text('x | y')], [text('z')]]] });
  });

  it('parses block quotes', () => {
    expect(parse('> quoted\n> text')).toEqual([{ type: 'quote', children: [{ type: 'paragraph', children: [text('quoted text')] }] }]);
  });

  it('keeps raw HTML blocks as plain text', () => {
    expect(parse('<script>alert(1)</script>')).toEqual([{ type: 'paragraph', children: [text('<script>alert(1)</script>')] }]);
    expect(parse('<div onclick="x()">\nhi\n</div>')).toEqual([
      { type: 'paragraph', children: [text('<div onclick="x()"> hi </div>')] },
    ]);
  });

  it('survives deep nesting without overflowing the stack', () => {
    expect(() => parse('>'.repeat(5000) + ' x')).not.toThrow();
    expect(() => parse(Array.from({ length: 500 }, (_, i) => `${' '.repeat(i * 2)}- x`).join('\n'))).not.toThrow();
    expect(() => parse('*'.repeat(20000) + 'x')).not.toThrow();
    expect(() => parse('['.repeat(20000) + 'x')).not.toThrow();
  });
});

describe('parseMarkdown inlines', () => {
  it('parses emphasis, strong and inline code', () => {
    expect(inlines('a *em* and **strong** and `co*de*`')).toEqual([
      text('a '),
      { type: 'em', children: [text('em')] },
      text(' and '),
      { type: 'strong', children: [text('strong')] },
      text(' and '),
      { type: 'code', text: 'co*de*' },
    ]);
  });

  it('nests emphasis', () => {
    expect(inlines('**bold *both* bold**')).toEqual([
      { type: 'strong', children: [text('bold '), { type: 'em', children: [text('both')] }, text(' bold')] },
    ]);
    expect(inlines('*em **both** em*')).toEqual([
      { type: 'em', children: [text('em '), { type: 'strong', children: [text('both')] }, text(' em')] },
    ]);
    expect(inlines('***both***')).toEqual([{ type: 'em', children: [{ type: 'strong', children: [text('both')] }] }]);
    expect(inlines('_em __strong__ em_')).toEqual([
      { type: 'em', children: [text('em '), { type: 'strong', children: [text('strong')] }, text(' em')] },
    ]);
  });

  it('leaves unmatched or intraword underscores and spaced stars alone', () => {
    expect(inlines('tape_echo and freq_shift')).toEqual([text('tape_echo and freq_shift')]);
    expect(inlines('2 * 3 * 4')).toEqual([text('2 * 3 * 4')]);
    expect(inlines('**unclosed')).toEqual([text('**unclosed')]);
  });

  it('honours backslash escapes', () => {
    expect(inlines('\\*not em\\* and \\`not code\\`')).toEqual([text('*not em* and `not code`')]);
  });

  it('parses links with safe targets', () => {
    expect(inlines('[the **workshop**](https://hundekraft.github.io/op1-workshop/ "title")')).toEqual([
      {
        type: 'link',
        href: 'https://hundekraft.github.io/op1-workshop/',
        external: true,
        children: [text('the '), { type: 'strong', children: [text('workshop')] }],
      },
    ]);
    expect(inlines('[file](rings.knl)')).toEqual([
      { type: 'link', href: '/co-op/plugins/rings/rings.knl', external: false, children: [text('file')] },
    ]);
  });

  it('turns links with unsafe targets into their text', () => {
    expect(inlines('[click](javascript:alert(1)) now')).toEqual([text('click now')]);
    expect(inlines('[x](data:text/html,hi)')).toEqual([text('x')]);
    expect(inlines('[x](../grids/manual.md)')).toEqual([text('x')]);
  });

  it('links bare and angle-bracket http(s) URLs, without trailing punctuation', () => {
    expect(inlines('Source: https://github.com/coldsoldr/te-op1-cfw.')).toEqual([
      text('Source: '),
      {
        type: 'link',
        href: 'https://github.com/coldsoldr/te-op1-cfw',
        external: true,
        children: [text('https://github.com/coldsoldr/te-op1-cfw')],
      },
      text('.'),
    ]);
    expect(inlines('<https://example.com/a>')).toEqual([
      { type: 'link', href: 'https://example.com/a', external: true, children: [text('https://example.com/a')] },
    ]);
    expect(inlines('<javascript:alert(1)>')).toEqual([text('<javascript:alert(1)>')]);
  });

  it('allows images from the plugin folder only, otherwise shows the alt text', () => {
    expect(inlines('![rings screen](screenshot.png)')).toEqual([
      { type: 'image', src: '/co-op/plugins/rings/screenshot.png', alt: 'rings screen' },
    ]);
    expect(inlines('![tracker](https://evil.example/pixel.png)')).toEqual([text('tracker')]);
    expect(inlines('![x](data:image/svg+xml,%3Csvg%20onload=alert(1)%3E)')).toEqual([text('x')]);
    // A space ends the destination, so this is no image at all, just text.
    expect(inlines('![x](data:image/svg+xml,<svg onload=alert(1)>)')).toEqual([text('![x](data:image/svg+xml,<svg onload=alert(1)>)')]);
  });

  it('keeps inline HTML as text', () => {
    expect(inlines('a <img src=x onerror=alert(1)> b <b>bold</b>')).toEqual([text('a <img src=x onerror=alert(1)> b <b>bold</b>')]);
  });

  it('turns two trailing spaces into a line break', () => {
    expect(inlines('one  \ntwo')).toEqual([text('one'), { type: 'break' }, text('two')]);
  });
});

describe('parseMarkdown on a seed manual', () => {
  it('reads the rings manual shape', async () => {
    const { readFileSync } = await import('node:fs');
    const source = readFileSync(new URL('../../../../plugins/rings/manual.md', import.meta.url), 'utf8');
    const blocks = parse(source);
    expect(blocks[0]).toEqual({ type: 'heading', level: 1, children: [text('rings')] });
    expect(blocks.some((b) => b.type === 'table')).toBe(true);
    const image = blocks.flatMap((b) => (b.type === 'paragraph' ? b.children : [])).find((i) => i.type === 'image');
    expect(image).toEqual({ type: 'image', src: '/co-op/plugins/rings/screenshot.png', alt: 'rings screen' });
    const lists = blocks.filter((b) => b.type === 'list');
    expect(lists.some((l) => l.type === 'list' && l.ordered && l.items.length === 6)).toBe(true);
  });
});
