import { test, expect } from '@playwright/test';
import { createHash } from 'node:crypto';
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readPatch, withMeta } from '../src/core/codec/patch';
import type { Op1Meta } from '../src/core/codec/op1-meta';

const fixtures = fileURLToPath(new URL('../test/fixtures/', import.meta.url));

/** The cluster fixture re-saved as a hundekraft supersaw sound, built by hand. */
function supersaw(): Uint8Array {
  const patch = readPatch(new Uint8Array(readFileSync(join(fixtures, 'synth-cluster.aif'))));
  return withMeta(patch, { ...patch.meta, type: 'supersaw', name: 'saw' } as Op1Meta).raw;
}

let parent: string;
let drive: string;

test.beforeEach(() => {
  parent = mkdtempSync(join(tmpdir(), 'co-op-e2e-firmware-'));
  drive = join(parent, 'OP-1');
  mkdirSync(join(drive, 'synth', 'user'), { recursive: true });
  mkdirSync(join(drive, 'drum', 'user'), { recursive: true });
  mkdirSync(join(drive, 'tape'), { recursive: true });
  copyFileSync(join(fixtures, 'synth-cluster.aif'), join(drive, 'synth', 'user', 'cluster.aif'));
});

test.afterEach(() => {
  rmSync(parent, { recursive: true, force: true });
});

test('suggests the hundekraft profile for a drive holding hundekraft sounds', async ({ page }) => {
  writeFileSync(join(drive, 'synth', 'user', 'saw.aif'), supersaw());
  await page.goto('./');
  await page.getByLabel('Choose the OP-1 drive folder').setInputFiles(drive);

  const saw = page.locator('tr[data-path="synth/user/saw.aif"]');
  await expect(saw.locator('.badge')).toContainText('unsupported');
  await expect(saw.locator('.badge')).toContainText('may stop a stock OP-1 at boot');
  await saw.getByText('details').click();
  await expect(saw.locator('.traits')).toContainText('supersaw');

  const suggestion = page.getByRole('status').filter({ hasText: 'Switch profile?' });
  await expect(suggestion).toContainText('1 of 2 patches use hundekraft engines');
  await suggestion.getByRole('button', { name: 'Switch to OP-1 (hundekraft)' }).click();

  await expect(page.getByLabel('Profile')).toHaveValue('op1/hundekraft');
  await expect(saw.locator('.badge')).toHaveText('ok');
  await expect(page.locator('tr[data-path="synth/user/cluster.aif"] .badge')).toHaveText('ok');
  await expect(suggestion).toBeHidden();
});

test('a dismissed suggestion stays hidden for the drive', async ({ page }) => {
  writeFileSync(join(drive, 'synth', 'user', 'saw.aif'), supersaw());
  await page.goto('./');
  await page.getByLabel('Choose the OP-1 drive folder').setInputFiles(drive);
  const suggestion = page.getByRole('status').filter({ hasText: 'Switch profile?' });
  await suggestion.getByRole('button', { name: 'Keep OP-1 (stock firmware)' }).click();
  await expect(suggestion).toBeHidden();
  await page.getByRole('button', { name: 'Refresh' }).click();
  await expect(page.locator('tbody tr[data-path]')).toHaveCount(2);
  await expect(suggestion).toBeHidden();
  await expect(page.getByLabel('Profile')).toHaveValue('op1/stock');
});

test('warns before installing a hundekraft pack patch onto a stock drive', async ({ page }) => {
  const bytes = supersaw();
  const sha256 = createHash('sha256').update(bytes).digest('hex');
  // Add a hundekraft pack to the real catalogue; nothing like it is committed.
  await page.route('**/packs/index.json', async (route) => {
    const index = await (await route.fetch()).json();
    index.packs.push({
      slug: 'saws',
      name: 'Saws',
      author: 'co-op',
      description: 'A hundekraft supersaw for tests.',
      license: 'CC0-1.0',
      firmware: ['op1/hundekraft'],
      tags: ['test'],
      sizeBytes: bytes.length,
      hasReadme: false,
      patches: [{ file: 'synth/saw.aif', kind: 'synth', name: 'saw', engine: 'supersaw', sizeBytes: bytes.length, sha256 }],
    });
    await route.fulfill({ json: index });
  });
  await page.route('**/packs/saws/synth/saw.aif', (route) =>
    route.fulfill({ body: Buffer.from(bytes), contentType: 'audio/x-aiff' }),
  );

  await page.goto('./');
  await page.getByLabel('Choose the OP-1 drive folder').setInputFiles(drive);
  await expect(page.locator('tbody tr[data-path]')).toHaveCount(1);
  await page.getByRole('tab', { name: 'Packs' }).click();
  await page.getByRole('button', { name: 'View Saws' }).click();

  await page.getByRole('button', { name: 'Install all (1)' }).click();
  const dialog = page.getByRole('dialog', { name: 'Not supported on OP-1 (stock firmware)' });
  await expect(dialog).toContainText('may stop a stock OP-1 at boot');
  await expect(dialog).toContainText('synth/saw.aif');
  await expect(dialog.getByRole('button', { name: 'Cancel' })).toBeFocused();
  await dialog.getByRole('button', { name: 'Cancel' }).click();
  await expect(dialog).toBeHidden();
  await expect(page.getByText('Installed 1 patch')).toBeHidden();

  await page.getByRole('button', { name: 'Install all (1)' }).click();
  await dialog.getByRole('button', { name: 'Write anyway' }).click();
  await expect(page.getByRole('status').filter({ hasText: 'Installed 1 patch' })).toBeVisible();
  await page.getByRole('tab', { name: 'Library' }).click();
  await expect(page.locator('tr[data-path="synth/saws/saw.aif"] .badge')).toContainText('unsupported');
});

test('a profile chosen by hand is never second-guessed', async ({ page }) => {
  writeFileSync(join(drive, 'synth', 'user', 'saw.aif'), supersaw());
  await page.goto('./');
  await page.getByLabel('Choose the OP-1 drive folder').setInputFiles(drive);
  const suggestion = page.getByRole('status').filter({ hasText: 'Switch profile?' });
  await expect(suggestion).toBeVisible();
  await page.getByLabel('Profile').selectOption('op1/stock');
  await expect(page.getByText('chosen by hand')).toBeVisible();
  await expect(page.locator('tr[data-path="synth/user/saw.aif"] .badge')).toContainText('unsupported');
  await expect(suggestion).toBeHidden();
  await page.getByRole('button', { name: 'Refresh' }).click();
  await expect(page.locator('tbody tr[data-path]')).toHaveCount(2);
  await expect(suggestion).toBeHidden();
});

test('detects an opus drive by its mods folder and lists the plugins', async ({ page }) => {
  mkdirSync(join(drive, 'mods'), { recursive: true });
  writeFileSync(join(drive, 'mods', 'x.opmod'), Buffer.from('OPMD\u0001\u0002\u0003\u0004', 'latin1'));
  writeFileSync(join(drive, 'mods', 'y.opmod'), Buffer.from('nope', 'latin1'));
  writeFileSync(join(drive, 'mods', '.co-op-tmp-z.opmod'), Buffer.from('OPMD', 'latin1'));
  await page.goto('./');
  await page.getByLabel('Choose the OP-1 drive folder').setInputFiles(drive);

  await expect(page.getByLabel('Profile')).toHaveValue('op1/opus');
  await expect(page.getByText('detected: found mods folder with opus plugins')).toBeVisible();
  const plugins = page.getByRole('region', { name: 'Installed opus plugins' });
  await expect(plugins.locator('tr[data-plugin="mods/x.opmod"]')).toContainText('8 B');
  await expect(plugins.locator('tr[data-plugin="mods/x.opmod"] .badge')).toHaveText('ok');
  await expect(plugins.locator('tr[data-plugin="mods/y.opmod"] .badge')).toContainText('not an opus plugin');
  await expect(page.locator('tr[data-path="synth/user/cluster.aif"] .badge')).toHaveText('ok');

  // A read-only copy is never cleaned up, so the leftover is shown, not listed as a plugin.
  await expect(plugins.getByRole('status')).toContainText('mods/.co-op-tmp-z.opmod');
  await expect(plugins.locator('tbody tr')).toHaveCount(2);

  await expect(page.getByText('also include mods/ (the opus plugins)')).toBeVisible();

  // Switching to stock hides the opus-only section.
  await page.getByLabel('Profile').selectOption('op1/stock');
  await expect(plugins).toBeHidden();
});
