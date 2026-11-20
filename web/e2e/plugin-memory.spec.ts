import { fileURLToPath } from 'node:url';
import { expect, test, type Page } from '@playwright/test';
import { coopRow, stubCoop } from './coop-stub';

interface IndexBuild {
  firmware?: string;
  file?: string;
  sizeBytes?: number;
  version?: string;
}

/** The inventory row of a coop plugin of the catalogue as the OP-1 gives it once loaded: its real name and size. */
async function catalogueRow(page: Page, file: string): Promise<string> {
  const index = (await (await page.request.get('plugins/index.json')).json()) as { plugins: (IndexBuild & { kind: string; builds?: IndexBuild[] })[] };
  for (const plugin of index.plugins) {
    const build = (plugin.builds ?? [plugin]).find((b) => b.firmware === 'op1/coop' && b.file === file);
    if (build) return coopRow({ file, kind: plugin.kind === 'synth' ? 'synth' : 'fx', version: build.version ?? '', size: build.sizeBytes ?? 0 });
  }
  throw new Error(`${file} is not a coop plugin of the catalogue`);
}

/** The rows of every coop plugin of the catalogue but `without`, loaded. */
async function catalogueRows(page: Page, without: readonly string[]): Promise<string[]> {
  const index = (await (await page.request.get('plugins/index.json')).json()) as { plugins: (IndexBuild & { builds?: IndexBuild[] })[] };
  const files = index.plugins.flatMap((p) => (p.builds ?? [p]).flatMap((b) => (b.firmware === 'op1/coop' && b.file ? [b.file] : [])));
  return Promise.all(files.filter((f) => !without.includes(f)).map((f) => catalogueRow(page, f)));
}

/** Two files co-op does not have, loaded: counted by their size, 703 KB each as engines. */
const strangers = [
  coopRow({ file: 'my_synth.coop', kind: 'synth', size: 720_000 }),
  coopRow({ file: 'my_other.coop', kind: 'synth', size: 720_000 }),
];

async function readPlugins(page: Page): Promise<void> {
  await page.goto('./');
  await page.getByRole('tab', { name: 'Settings' }).click();
  await page.getByRole('radio', { name: /OP-1 \(coop\)/ }).check();
  await page.getByRole('tab', { name: 'Plugins' }).click();
  await page.getByRole('button', { name: 'Read plugins from the OP-1' }).click();
  await expect(page.getByRole('table', { name: 'Plugin files on the OP-1' })).toBeVisible();
}

const shot = async (page: Page, name: string): Promise<void> => {
  if (process.env.E2E_SHOTS) await page.locator('.coop-device').screenshot({ path: `${process.env.E2E_SHOTS}/${name}.png` });
};

test.describe('Plugins: the coop plugin memory', () => {
  test('shows how full the plugin memory is, and what each plugin takes', async ({ page }) => {
    await stubCoop(page, { rows: [await catalogueRow(page, 'saw.coop'), await catalogueRow(page, 'drive.coop')] });
    await readPlugins(page);

    const memory = page.locator('.plugin-memory');
    await expect(memory).toHaveAttribute('data-level', 'fits');
    const meter = page.getByRole('meter', { name: 'Plugin memory' });
    await expect(meter).toContainText('of 1,536 KB');
    await expect(meter).toContainText(/\d+ %/);
    await expect(memory).toContainText('What the plugins switched on take at the next power cycle.');
    await expect(memory).toContainText(/of it is working memory the effects share, sized for drive\./);
    await expect(memory).not.toContainText('estimate');

    const table = page.getByRole('table', { name: 'Plugin files on the OP-1' });
    await expect(table.getByRole('columnheader', { name: 'Memory' })).toBeVisible();
    await expect(table.getByRole('row', { name: /saw\.coop/ })).toContainText(/\d KB/);
    await expect(table.getByRole('row', { name: /Working memory the effects share/ })).toContainText(/\d KB/);
    await shot(page, 'plugin-memory-fits');
  });

  test('adds up the catalogue’s own plugins without estimating', async ({ page }) => {
    // The set of the firmware's e9 gate: every coop plugin of the catalogue but the two largest effects.
    const rows = await catalogueRows(page, ['fickle.coop', 'petri.coop']);
    await stubCoop(page, { rows });
    await readPlugins(page);

    const memory = page.locator('.plugin-memory');
    await expect(page.getByRole('meter', { name: 'Plugin memory' })).toContainText(/[\d,]+ of 1,536 KB/);
    await expect(memory).not.toContainText('about');
    await expect(memory).not.toContainText('estimate');
    const cells = page.locator('table.coop-plugins tbody td:nth-child(4)');
    await expect(cells).toHaveCount(rows.length);
    await expect(cells.filter({ hasText: /^[\d,]+ KB$/ })).toHaveCount(rows.length);
    await shot(page, 'plugin-memory-catalogue-set');
  });

  test('warns when nearly full, and says what it could only guess', async ({ page }) => {
    await stubCoop(page, { rows: [await catalogueRow(page, 'saw.coop'), await catalogueRow(page, 'drive.coop'), ...strangers] });
    await readPlugins(page);

    const memory = page.locator('.plugin-memory');
    await expect(memory).toHaveAttribute('data-level', 'nearly');
    await expect(memory).toContainText('Nearly full. Everything still loads, but there is little room for another plugin.');
    await expect(memory).toContainText('co-op does not have my_synth.coop, my_other.coop, so what they take is an estimate.');
    await expect(page.getByRole('meter', { name: 'Plugin memory' })).toContainText('about ');
    await expect(page.getByRole('row', { name: /my_synth\.coop/ })).toContainText('about 703 KB');
    await shot(page, 'plugin-memory-nearly-full');
  });

  test('says when the plugins do not fit, with the OP-1’s own figures for a file it refused', async ({ page }) => {
    await stubCoop(page, {
      rows: [
        await catalogueRow(page, 'saw.coop'),
        await catalogueRow(page, 'drive.coop'),
        strangers[0] as string,
        coopRow({ file: 'big_verb.coop', boot: 'failed', size: 30_000, reason: 'over the plugin memory budget (needs 900 KB, 812 KB of 1536 KB left)' }),
      ],
    });
    await readPlugins(page);

    const memory = page.locator('.plugin-memory');
    await expect(memory).toHaveAttribute('data-level', 'over');
    await expect(memory).toContainText('Too much for the OP-1’s plugin memory. At the next power cycle at least one plugin will not load.');
    await expect(memory).toContainText(/The largest: big verb \(900 KB\), my synth \(\d+ KB\)/);
    // 1536 - 812 KB are in use now, by the OP-1's count; the refused file would add its 900.
    await expect(memory).toContainText('Loaded now: about 724 KB, by the OP-1’s own count.');
    await expect(page.getByRole('row', { name: /big_verb\.coop/ })).toContainText('about 900 KB');
    await shot(page, 'plugin-memory-over');
  });

  test('asks before sending a file that does not fit, and sends it when told to', async ({ page }) => {
    await stubCoop(page, { rows: [await catalogueRow(page, 'saw.coop'), await catalogueRow(page, 'drive.coop'), ...strangers] });
    await readPlugins(page);
    const rings = fileURLToPath(new URL('../../plugins/rings/rings.coop', import.meta.url));
    const questions: string[] = [];

    page.once('dialog', (dialog) => {
      questions.push(dialog.message());
      void dialog.dismiss();
    });
    await page.locator('.coop-device input[type=file]').setInputFiles(rings);
    await expect(page.getByText('Nothing was sent.')).toBeVisible();
    expect(questions[0]).toContain('rings.coop does not fit in the OP-1’s plugin memory.');
    expect(questions[0]).toMatch(/The largest on the OP-1: my (synth|other) \(703 KB\)/);
    expect(questions[0]).toContain('Install anyway?');
    expect(await page.evaluate(() => window.coopStub.puts)).toEqual([]);

    page.once('dialog', (dialog) => void dialog.accept());
    await page.locator('.coop-device input[type=file]').setInputFiles(rings);
    await expect(page.getByText(/^Installed rings\.coop/)).toBeVisible({ timeout: 60_000 });
    expect(await page.evaluate(() => window.coopStub.puts)).toEqual(['rings.coop']);
    await expect(page.locator('.plugin-memory')).toHaveAttribute('data-level', 'over');
    await expect(page.getByRole('row', { name: /rings\.coop/ })).toContainText(/\d KB/);
    await expect(page.getByRole('row', { name: /rings\.coop/ })).not.toContainText('about');
  });

  test('takes the OP-1’s own figure when asked', async ({ page }) => {
    const stats =
      `coop-stats 3 build e10\n${'cb_max 12345 cb_sum 678901 blocks 4321\n'.repeat(12)}plugins seen 3 loaded 3 failed 0\n` +
      'plugmem budget 1572864 used 740000 over_budget 0 heap_short 0 skipped 0 badname 0 tape_node 524288 heap_free_boot 15058684 scratch_files 1 fxbuf 1600 fxbuf_mem 8040 fxbuf_short 0\n' +
      'top seq 4 blocks 100\n';
    await stubCoop(page, { rows: [await catalogueRow(page, 'saw.coop'), await catalogueRow(page, 'drive.coop'), strangers[0] as string], stats });
    await readPlugins(page);

    const memory = page.locator('.plugin-memory');
    await expect(memory).toContainText('co-op does not have my_synth.coop');
    await memory.getByRole('button', { name: 'Ask the OP-1 for its own figure' }).click();
    await expect(memory).toContainText('Loaded now: 723 KB, by the OP-1’s own count.');
    // The one file co-op does not have holds what the others do not: nothing is estimated any more.
    await expect(memory).not.toContainText('estimate');
    await expect(memory.getByRole('button', { name: 'Ask the OP-1 for its own figure' })).toHaveCount(0);
  });
});
