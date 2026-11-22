import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { expect, test, type Page } from '@playwright/test';
import { ownBytes, type CoopMemory } from '../src/core/plugins/memory';
import { crc32 } from '../src/core/usbmidi/framing';
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
    const stub = await page.evaluate(() => window.coopStub);
    expect(stub.puts).toEqual(['rings.coop']);
    // The file went with three chunks in flight, none was dropped, and every byte arrived.
    const sentBytes = new Uint8Array(await readFile(rings));
    expect(stub.files['rings.coop']).toEqual({ size: sentBytes.length, crc: crc32(sentBytes) });
    expect(stub.deepest).toBe(3);
    expect(stub.drops).toBe(0);
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

test.describe('Plugins: the coop plugin memory on e12', () => {
  /** Every coop plugin of the catalogue as an e12 OP-1 lists it once loaded, with the need and scratch the core counts. */
  async function e12Rows(page: Page): Promise<{ rows: string[]; need: number; buffer: number }> {
    const index = (await (await page.request.get('plugins/index.json')).json()) as {
      plugins: (IndexBuild & { kind: string; memory?: CoopMemory; builds?: (IndexBuild & { memory?: CoopMemory })[] })[];
    };
    const rows: string[] = [];
    let need = 0;
    let buffer = 0;
    for (const plugin of index.plugins) {
      const build = (plugin.builds ?? [plugin]).find((b) => b.firmware === 'op1/coop' && b.file && b.memory);
      if (!build?.file || !build.memory) continue;
      const own = ownBytes(build.memory, 'arena');
      need += own;
      buffer = Math.max(buffer, build.memory.scratch);
      rows.push(coopRow({ file: build.file, kind: build.memory.kind, version: build.version ?? '', size: build.sizeBytes ?? 0, need: own, scratch: build.memory.scratch }));
    }
    return { rows, need, buffer };
  }

  test('shows the OP-1’s own figures: its budget, what is in use and each file’s need', async ({ page }) => {
    const { rows, need, buffer } = await e12Rows(page);
    // A file co-op does not have: the OP-1 sizes it, so nothing is estimated.
    const mine = coopRow({ file: 'my_synth.coop', kind: 'synth', size: 90_000, need: 700_000, scratch: 0 });
    const used = need + 700_000 + buffer;
    await stubCoop(page, { build: 'e12', rows: [...rows, mine], memory: { budget: 5_898_240, used, fxbuf: buffer, fxbufs: 4 } });
    await readPlugins(page);

    const memory = page.locator('.plugin-memory');
    await expect(memory).toHaveAttribute('data-level', 'fits');
    const kb = (bytes: number): string => `${Math.round(bytes / 1024).toLocaleString('en-US')} KB`;
    await expect(page.getByRole('meter', { name: 'Plugin memory' })).toContainText(`${kb(used).replace(' KB', '')} of 5,760 KB`);
    await expect(memory).toContainText(`Loaded now: ${kb(used)}, by the OP-1’s own count.`);
    await expect(memory).toContainText(`${kb(buffer)} of it is working memory the effects share`);
    await expect(memory).toContainText('Stereo tape lift: 47 s.');
    await expect(memory).not.toContainText('about');
    await expect(memory).not.toContainText('estimate');
    await expect(memory).not.toContainText('effect positions');
    // The inventory has the figures: no slow read of the statistics file is offered.
    await expect(memory.getByRole('button', { name: 'Ask the OP-1 for its own figure' })).toHaveCount(0);
    await expect(page.getByRole('row', { name: /my_synth\.coop/ })).toContainText('684 KB');
    await expect(page.getByRole('row', { name: /my_synth\.coop/ })).not.toContainText('about');
    await expect(page.getByRole('row', { name: /Working memory the effects share/ })).toContainText(kb(buffer));
    await shot(page, 'plugin-memory-e12');
  });

  test('names the plugin the OP-1 will leave out, by name order, and how many positions run a buffered effect', async ({ page }) => {
    // A core built with a small budget (the firmware's test switch), so three files decide it.
    const rows = [
      coopRow({ file: 'c_echo.coop', size: 20_000, need: 200_000, scratch: 100_000 }),
      coopRow({ file: 'a_verb.coop', size: 20_000, need: 600_000, scratch: 100_000 }),
      coopRow({ file: 'b_synth.coop', kind: 'synth', boot: 'failed', size: 20_000, need: 500_000, reason: 'over the plugin memory budget (needs 489 KB, 97 KB of 977 KB left)' }),
    ];
    await stubCoop(page, { build: 'e12', rows, memory: { budget: 1_000_000, used: 900_000, fxbuf: 100_000, fxbufs: 1, clip: 1_572_864 } });
    await readPlugins(page);

    const memory = page.locator('.plugin-memory');
    await expect(memory).toHaveAttribute('data-level', 'over');
    await expect(memory).toContainText('The OP-1 loads them in name order, so it will leave out b synth.');
    await expect(memory).toContainText('Loaded now: 879 KB, by the OP-1’s own count.');
    await expect(memory).toContainText('Stereo tape lift: 17 s.');
    await shot(page, 'plugin-memory-e12-over');
  });

  test('says when not every effect position gets working memory', async ({ page }) => {
    const rows = [coopRow({ file: 'a_verb.coop', size: 20_000, need: 600_000, scratch: 150_000 })];
    await stubCoop(page, { build: 'e12', rows, memory: { budget: 1_000_000, used: 750_000, fxbuf: 150_000, fxbufs: 2 } });
    await readPlugins(page);
    const memory = page.locator('.plugin-memory');
    await expect(memory).toHaveAttribute('data-level', 'fits');
    await expect(memory).toContainText('With this much loaded, only 2 of the 4 effect positions can run such an effect at once.');
  });

  test('asks before sending a file that does not fit and says which plugin would be left out', async ({ page }) => {
    const rows = [
      coopRow({ file: 'a_verb.coop', size: 20_000, need: 300_000, scratch: 100_000 }),
      coopRow({ file: 'z_synth.coop', kind: 'synth', size: 20_000, need: 300_000 }),
    ];
    await stubCoop(page, { build: 'e12', rows, memory: { budget: 1_200_000, used: 700_000, fxbuf: 100_000, fxbufs: 4 } });
    await readPlugins(page);
    const questions: string[] = [];
    page.once('dialog', (dialog) => {
      questions.push(dialog.message());
      void dialog.dismiss();
    });
    // rings needs 652 KB; by name it loads before z synth, which then no longer fits.
    await page.locator('.coop-device input[type=file]').setInputFiles(fileURLToPath(new URL('../../plugins/rings/rings.coop', import.meta.url)));
    await expect(page.getByText('Nothing was sent.')).toBeVisible();
    expect(questions[0]).toContain('rings.coop does not fit in the OP-1’s plugin memory.');
    expect(questions[0]).toContain('the OP-1 has 1,172 KB');
    expect(questions[0]).toContain('At the next power cycle the OP-1 would leave out z synth: it loads the plugins in name order.');
    expect(await page.evaluate(() => window.coopStub.puts)).toEqual([]);
  });
});
