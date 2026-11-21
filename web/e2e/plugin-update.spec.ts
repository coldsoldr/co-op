import { expect, test, type Page } from '@playwright/test';
import { coopRow, stubCoop } from './coop-stub';

interface IndexBuild {
  firmware?: string;
  file?: string;
  sizeBytes?: number;
  version?: string;
}

/** The catalogue's coop build of `file`: its version and size. */
async function catalogueBuild(page: Page, file: string): Promise<{ version: string; sizeBytes: number; kind: 'fx' | 'synth' }> {
  const index = (await (await page.request.get('plugins/index.json')).json()) as { plugins: (IndexBuild & { kind: string; builds?: IndexBuild[] })[] };
  for (const plugin of index.plugins) {
    const build = (plugin.builds ?? [plugin]).find((b) => b.firmware === 'op1/coop' && b.file === file);
    if (build) return { version: build.version ?? '', sizeBytes: build.sizeBytes ?? 0, kind: plugin.kind === 'synth' ? 'synth' : 'fx' };
  }
  throw new Error(`${file} is not a coop plugin of the catalogue`);
}

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

test.describe('Plugins: updating the plugins on a coop OP-1', () => {
  test('Update all replaces the plugins that have a newer build, and only those', async ({ page }) => {
    const echo = await catalogueBuild(page, 'tape_echo.coop');
    const crusher = await catalogueBuild(page, 'bitcrusher.coop');
    const saw = await catalogueBuild(page, 'saw.coop');
    await stubCoop(page, {
      build: 'e12',
      rows: [
        // Two files from before the catalogue's builds, one that is current, one co-op does not know.
        coopRow({ file: 'bitcrusher.coop', version: '0.0.9', size: 12_000, need: 5_000, scratch: 32 }),
        coopRow({ file: 'saw.coop', kind: 'synth', version: saw.version, size: saw.sizeBytes, need: 4_000 }),
        coopRow({ file: 'tape_echo.coop', version: '0.0.1', size: 23_000, need: 14_000, scratch: 134_784, next: 'off' }),
        coopRow({ file: 'my_synth.coop', kind: 'synth', version: '0.1', size: 90_000, need: 700_000 }),
      ],
      memory: { budget: 5_898_240, used: 858_000, fxbuf: 134_784, fxbufs: 4 },
    });
    await readPlugins(page);
    // Reading the plugins left no port open.
    expect(await page.evaluate(() => window.coopStub.openPorts)).toEqual([]);

    const all = page.getByRole('button', { name: 'Update all (2)' });
    await expect(all).toBeVisible();
    await expect(page.getByRole('row', { name: /bitcrusher\.coop/ })).toContainText(`update available: 0.0.9 → ${crusher.version}`);
    await expect(page.getByRole('row', { name: /tape_echo\.coop/ })).toContainText(`update available: 0.0.1 → ${echo.version}`);
    await expect(page.getByRole('row', { name: /saw\.coop/ })).not.toContainText('update available');
    await expect(page.getByRole('row', { name: /my_synth\.coop/ })).not.toContainText('update available');
    await expect(page.getByRole('row', { name: /bitcrusher\.coop/ }).getByRole('button', { name: 'Update', exact: true })).toBeVisible();
    await expect(page.getByRole('row', { name: /saw\.coop/ }).getByRole('button', { name: 'Update', exact: true })).toHaveCount(0);
    await shot(page, 'plugin-update-waiting');

    const questions: string[] = [];
    page.on('dialog', (dialog) => {
      questions.push(dialog.message());
      void dialog.accept();
    });
    await all.click();
    await expect(page.getByText('2 plugins updated. Power-cycle the OP-1 to load them.')).toBeVisible({ timeout: 60_000 });

    // One question, with both plugins and their versions; no "Replace it?" for each.
    expect(questions).toEqual([
      `Update 2 plugins on the OP-1?\n\nbitcrusher: 0.0.9 → ${crusher.version}\ntape echo: 0.0.1 → ${echo.version}\n\nEach replaces the file on the OP-1.`,
    ]);
    const stub = await page.evaluate(() => window.coopStub);
    expect(stub.puts).toEqual(['bitcrusher.coop', 'tape_echo.coop']);
    expect(stub.files['bitcrusher.coop']?.size).toBe(crusher.sizeBytes);
    expect(stub.files['tape_echo.coop']?.size).toBe(echo.sizeBytes);
    expect(stub.drops).toBe(0);
    // Update all opened the ports once for the whole run (and the read before it once), and closed them after.
    expect(stub.openPorts).toEqual([]);
    expect(stub.opens).toBe(4);

    // The panel was read again: nothing is left to update, and the rows say what waits for the power cycle.
    await expect(page.getByRole('button', { name: /Update all/ })).toHaveCount(0);
    await expect(page.getByRole('row', { name: /bitcrusher\.coop/ })).toContainText(`${crusher.version} sent, loads after a power cycle`);
    await expect(page.getByRole('row', { name: /tape_echo\.coop/ })).toContainText(`${echo.version} sent, loads after a power cycle`);
    await shot(page, 'plugin-update-done');
  });

  test('a row’s own Update sends that plugin alone', async ({ page }) => {
    const echo = await catalogueBuild(page, 'tape_echo.coop');
    await stubCoop(page, {
      rows: [coopRow({ file: 'bitcrusher.coop', version: '0.0.9', size: 12_000 }), coopRow({ file: 'tape_echo.coop', version: '0.0.1', size: 23_000 })],
    });
    await readPlugins(page);
    page.on('dialog', (dialog) => void dialog.accept());
    await page.getByRole('row', { name: /tape_echo\.coop/ }).getByRole('button', { name: 'Update', exact: true }).click();
    await expect(page.getByText('1 plugin updated. Power-cycle the OP-1 to load it.')).toBeVisible({ timeout: 60_000 });
    expect(await page.evaluate(() => window.coopStub.puts)).toEqual(['tape_echo.coop']);
    expect((await page.evaluate(() => window.coopStub.files))['tape_echo.coop']?.size).toBe(echo.sizeBytes);
    // The other update still waits.
    await expect(page.getByRole('button', { name: 'Update all (1)' })).toBeVisible();
  });

  test('shows no update button when every plugin is current, and holds back a build the core cannot run', async ({ page }) => {
    const saw = await catalogueBuild(page, 'saw.coop');
    await stubCoop(page, {
      build: 'e7',
      rows: [coopRow({ file: 'saw.coop', kind: 'synth', version: saw.version, size: saw.sizeBytes }), coopRow({ file: 'tape_echo.coop', version: '0.0.1', size: 23_000 })],
    });
    await readPlugins(page);
    await expect(page.getByRole('button', { name: /Update/ })).toHaveCount(0);
    // tape_echo's catalogue build asks for a later core than e7.
    await expect(page.getByRole('row', { name: /tape_echo\.coop/ })).toContainText(/needs coop e\d+/);
  });
});
