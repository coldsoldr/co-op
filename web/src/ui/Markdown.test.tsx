import { describe, it, expect } from 'vitest';
import { renderToStaticMarkup } from 'react-dom/server';
import { Markdown } from './Markdown';

const FOLDER = '/co-op/plugins/rings/';
const html = (source: string, headingOffset?: number): string =>
  renderToStaticMarkup(
    headingOffset === undefined ? (
      <Markdown source={source} folderUrl={FOLDER} />
    ) : (
      <Markdown source={source} folderUrl={FOLDER} headingOffset={headingOffset} />
    ),
  );

describe('Markdown', () => {
  it('renders headings (offset and capped), paragraphs, emphasis and code', () => {
    expect(html('# rings\n\nA *very* **loud** `knob`.', 2)).toBe(
      '<div class="markdown"><h3>rings</h3><p>A <em>very</em> <strong>loud</strong> <code>knob</code>.</p></div>',
    );
    expect(html('##### deep', 3)).toBe('<div class="markdown"><h6>deep</h6></div>');
  });

  it('renders nested emphasis', () => {
    expect(html('***both*** and **a *b* c**')).toBe(
      '<div class="markdown"><p><em><strong>both</strong></em> and <strong>a <em>b</em> c</strong></p></div>',
    );
  });

  it('renders code blocks as text', () => {
    expect(html('```\n<b>x</b> & y\n```')).toBe('<div class="markdown"><pre><code>&lt;b&gt;x&lt;/b&gt; &amp; y</code></pre></div>');
  });

  it('renders tight and loose lists, keeping an ordered start', () => {
    expect(html('- a\n- b')).toBe('<div class="markdown"><ul><li>a</li><li>b</li></ul></div>');
    expect(html('- a\n\n- b')).toBe('<div class="markdown"><ul><li><p>a</p></li><li><p>b</p></li></ul></div>');
    expect(html('2. a\n3. b')).toBe('<div class="markdown"><ol start="2"><li>a</li><li>b</li></ol></div>');
    expect(html('1. a')).toBe('<div class="markdown"><ol><li>a</li></ol></div>');
  });

  it('renders tables with header cells and alignment', () => {
    expect(html('| Encoder | Main |\n|---|:-:|\n| blue | **structure** |')).toBe(
      '<div class="markdown"><table class="library manual-table"><thead><tr><th scope="col">Encoder</th>' +
        '<th scope="col" style="text-align:center">Main</th></tr></thead><tbody><tr><td>blue</td>' +
        '<td style="text-align:center"><strong>structure</strong></td></tr></tbody></table></div>',
    );
  });

  it('opens external links in a new tab without an opener, and keeps folder links in place', () => {
    expect(html('[workshop](https://hundekraft.github.io/op1-workshop/) [file](rings.knl)')).toBe(
      '<div class="markdown"><p><a href="https://hundekraft.github.io/op1-workshop/" target="_blank" rel="noopener noreferrer">workshop</a> ' +
        '<a href="/co-op/plugins/rings/rings.knl">file</a></p></div>',
    );
  });

  it('loads images from the plugin folder lazily, with their alt text', () => {
    expect(html('![rings screen](screenshot.png)')).toBe(
      '<div class="markdown"><p><img src="/co-op/plugins/rings/screenshot.png" alt="rings screen" loading="lazy"/></p></div>',
    );
  });

  describe('never lets markup or script through', () => {
    const attacks = [
      '<script>alert(1)</script>',
      '<img src=x onerror=alert(1)>',
      '<a href="javascript:alert(1)">x</a>',
      '<iframe src="https://evil.example"></iframe>',
      '[x](javascript:alert(1))',
      '[x](JAVASCRIPT:alert(1))',
      '[x](<javascript:alert(1)>)',
      '[x](data:text/html;base64,PHNjcmlwdD5hbGVydCgxKTwvc2NyaXB0Pg==)',
      '[x](vbscript:msgbox(1))',
      '<javascript:alert(1)>',
      '![x](https://evil.example/track.png)',
      '![x](//evil.example/track.png)',
      '![x](data:image/png;base64,AAAA)',
      '![x](../grids/screenshot.png)',
      '[x](https://example.com/" onmouseover="alert(1))',
      '| a |\n|---|\n| <script>alert(1)</script> |',
      '- <b onclick="alert(1)">x</b>',
      '**<script>alert(1)</script>**',
    ];
    for (const attack of attacks) {
      it(JSON.stringify(attack), () => {
        const out = html(attack);
        // The only tags are the renderer's own; every < from the manual is escaped.
        for (const [, tag, attributes] of out.matchAll(/<\/?([a-z0-9]+)([^>]*)>/g)) {
          expect(['div', 'p', 'a', 'img', 'table', 'thead', 'tbody', 'tr', 'th', 'td', 'ul', 'li', 'strong', 'em']).toContain(tag);
          for (const [, name, value] of (attributes as string).matchAll(/\s([a-z-]+)="([^"]*)"/g)) {
            expect(['class', 'href', 'target', 'rel', 'src', 'alt', 'loading', 'scope', 'style']).toContain(name);
            // Links go to http(s) or into the plugin folder; images only come from the folder.
            if (name === 'href') expect(value).toMatch(/^(https?:\/\/|\/co-op\/plugins\/rings\/)/);
            if (name === 'src') expect(value).toMatch(/^\/co-op\/plugins\/rings\//);
          }
        }
      });
    }
  });
});
