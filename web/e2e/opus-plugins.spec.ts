import { test, expect, type Page } from '@playwright/test';
import { createHash } from 'node:crypto';
import { copyFileSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const fixtures = fileURLToPath(new URL('../test/fixtures/', import.meta.url));

/** A made-up opus plugin: the OPMD magic and a few bytes. Nothing like it is committed. */
const OPMOD = Buffer.from('OPMD test plugin bytes', 'latin1');

/** Adds the test plugin to the real plugin index and serves its file. */
async function serveOpusPlugin(page: Page): Promise<void> {
  await page.route('**/plugins/index.json', async (route) => {
    const index = await (await route.fetch()).json();
    index.plugins.push({
      id: 'test.mod',
      name: 'Test Mod',
      firmware: 'op1/opus',
      kind: 'fx',
      author: 'co-op',
      version: '1.0',
      description: 'An opus plugin for tests.',
      license: 'MIT',
      file: 'testmod.opmod',
      screenshot: 'test.mod/screenshot.png',
      manual: 'test.mod/manual.md',
      tags: ['test'],
      sizeBytes: OPMOD.length,
      sha256: createHash('sha256').update(OPMOD).digest('hex'),
      savedName: 'opus.test.mod',
    });
    await route.fulfill({ json: index });
  });
  await page.route('**/plugins/test.mod/testmod.opmod', (route) =>
    route.fulfill({ body: OPMOD, contentType: 'application/octet-stream' }),
  );
}

let parent: string;
let drive: string;

test.beforeEach(() => {
  parent = mkdtempSync(join(tmpdir(), 'co-op-e2e-opus-'));
  drive = join(parent, 'OP-1');
  mkdirSync(join(drive, 'synth', 'user'), { recursive: true });
  copyFileSync(join(fixtures, 'synth-cluster.aif'), join(drive, 'synth', 'user', 'cluster.aif'));
  mkdirSync(join(drive, 'mods'), { recursive: true });
  writeFileSync(join(drive, 'mods', 'a.opmod'), Buffer.from('OPMD', 'latin1'));
});

test.afterEach(() => {
  rmSync(parent, { recursive: true, force: true });
});

test('explains what is needed to copy an opus plugin when no opus drive is open', async ({ page }) => {
  await serveOpusPlugin(page);
  await page.goto('./');
  await page.getByRole('tab', { name: 'Plugins' }).click();
  const card = page.getByRole('article', { name: 'Test Mod' });
  const copy = card.getByRole('button', { name: 'Copy testmod.opmod to mods/' });
  await expect(copy).toBeDisabled();
  await expect(copy).toHaveAccessibleDescription(/open an opus drive on the Library tab/);
});

test('copies an opus plugin into mods/ of a dropped drive, asking before replacing it', async ({ page }) => {
  await serveOpusPlugin(page);
  await page.goto('./');
  await page.getByLabel('Choose the OP-1 drive folder').setInputFiles(drive);
  await expect(page.getByLabel('Profile')).toHaveValue('op1/opus');
  const installed = page.getByRole('region', { name: 'Installed opus plugins' });
  await expect(installed.locator('tbody tr')).toHaveCount(1);

  await page.getByRole('tab', { name: 'Plugins' }).click();
  const card = page.getByRole('article', { name: 'Test Mod' });
  const copy = card.getByRole('button', { name: 'Copy testmod.opmod to mods/' });
  await expect(copy).toHaveAccessibleDescription('opus loads the plugins in mods/ in file-name order.');
  await copy.click();
  await expect(card.getByRole('status').filter({ hasText: 'Copied to mods/testmod.opmod.' })).toContainText(
    'Download the changes from the Library tab',
  );

  await page.getByRole('tab', { name: 'Library' }).click();
  await expect(installed.locator('tr[data-plugin="mods/testmod.opmod"] .badge')).toHaveText('ok');
  await expect(installed.locator('tbody tr')).toHaveCount(2);
  await expect(page.getByRole('button', { name: 'Download changes (1)' })).toBeEnabled();

  // A file of the same name is only replaced after asking; Cancel is the default.
  await page.getByRole('tab', { name: 'Plugins' }).click();
  await copy.click();
  const dialog = page.getByRole('dialog', { name: 'Replace mods/testmod.opmod?' });
  await expect(dialog).toContainText('already has a file named testmod.opmod');
  await expect(dialog.getByRole('button', { name: 'Cancel' })).toBeFocused();
  await dialog.getByRole('button', { name: 'Cancel' }).click();
  await expect(dialog).toBeHidden();
  await expect(copy).toBeEnabled();

  await copy.click();
  await dialog.getByRole('button', { name: 'Replace' }).click();
  await expect(card.getByRole('status').filter({ hasText: 'Copied to mods/testmod.opmod.' })).toBeVisible();
  await page.getByRole('tab', { name: 'Library' }).click();
  await expect(installed.locator('tbody tr')).toHaveCount(2);
  await expect(page.getByRole('button', { name: 'Download changes (1)' })).toBeEnabled();
});
