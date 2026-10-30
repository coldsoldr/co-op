import { test, expect } from '@playwright/test';
import { copyFileSync, mkdirSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const fixtures = fileURLToPath(new URL('../test/fixtures/', import.meta.url));

let parent: string;
let drive: string;

test.beforeEach(() => {
  parent = mkdtempSync(join(tmpdir(), 'co-op-e2e-packs-'));
  drive = join(parent, 'OP-1');
  mkdirSync(join(drive, 'synth', 'user'), { recursive: true });
  mkdirSync(join(drive, 'drum', 'user'), { recursive: true });
  mkdirSync(join(drive, 'tape'), { recursive: true });
  copyFileSync(join(fixtures, 'synth-cluster.aif'), join(drive, 'synth', 'user', 'cluster.aif'));
});

test.afterEach(() => {
  rmSync(parent, { recursive: true, force: true });
});

test('browses the pack catalogue and filters it', async ({ page }, testInfo) => {
  await page.goto('./');
  await page.getByRole('tab', { name: 'Packs' }).click();
  const card = page.getByRole('article', { name: 'Test Tones' });
  await expect(card).toBeVisible();
  await expect(card).toContainText('co-op');
  await expect(card).toContainText('CC0-1.0');
  await expect(card).toContainText('OP-1 (stock firmware)');
  await expect(card).toContainText('3 patches');
  await expect(page.getByRole('link', { name: 'Share a pack' })).toHaveAttribute('href', /contributing-packs\.md$/);

  await page.getByRole('searchbox', { name: 'Search packs' }).fill('no such pack');
  await expect(card).toBeHidden();
  await expect(page.getByText('No packs match')).toBeVisible();
  await expect(page.getByRole('status').filter({ hasText: '0 of 1 pack' })).toBeVisible();
  await page.getByRole('searchbox', { name: 'Search packs' }).fill('sine');
  await expect(card).toBeVisible();
  await page.getByLabel('Kind').selectOption('drum');
  await expect(card).toBeVisible();

  // PACKS_SCREENSHOT=<path> saves the catalogue somewhere other than the test output folder.
  await page.screenshot({ path: process.env.PACKS_SCREENSHOT ?? testInfo.outputPath('packs.png'), fullPage: true });

  await card.getByRole('button', { name: 'View Test Tones' }).click();
  await expect(page.getByRole('heading', { name: 'Test Tones' })).toBeVisible();
  await expect(page.locator('table.pack tbody tr')).toHaveCount(3);
  // Nothing is connected, so installing explains how to connect instead.
  await expect(page.getByRole('button', { name: /Install all/ })).toBeDisabled();
  await expect(page.getByText('Connect an OP-1 or open its drive folder on the Library tab')).toBeVisible();
  await expect(page.getByRole('button', { name: /Install all/ })).toHaveAccessibleDescription(/Connect an OP-1/);

  // Focus follows the view, the notes and README are shown.
  await expect(page.getByRole('heading', { name: 'Test Tones' })).toBeFocused();
  await expect(page.locator('table.pack')).toContainText('Cluster engine over a 0.5 s, 220 Hz sine.');
  await expect(page.getByRole('link', { name: 'Read the pack README' })).toHaveAttribute('href', /packs\/test-tones\/README\.md$/);
  await page.getByRole('button', { name: 'Back to packs' }).click();
  await expect(page.getByRole('button', { name: 'View Test Tones' })).toBeFocused();
  await page.getByRole('button', { name: 'View Test Tones' }).click();
  await page.getByRole('button', { name: 'Go to Library' }).click();
  await expect(page.getByRole('tab', { name: 'Library' })).toHaveAttribute('aria-selected', 'true');
  await expect(page.getByRole('tab', { name: 'Library' })).toBeFocused();
});

test('installs a pack into a dropped drive as staged changes', async ({ page }) => {
  await page.goto('./');
  await page.getByLabel('Choose the OP-1 drive folder').setInputFiles(drive);
  await expect(page.locator('tbody tr[data-path]')).toHaveCount(1);
  await expect(page.getByRole('button', { name: 'Download changes (0)' })).toBeDisabled();

  await page.getByRole('tab', { name: 'Packs' }).click();
  await page.getByRole('button', { name: 'View Test Tones' }).click();
  await expect(page.getByLabel('Install synth patches into')).toHaveValue('synth/test-tones');
  await expect(page.getByLabel('Install drum patches into')).toHaveValue('drum/test-tones');
  await page.getByRole('button', { name: 'Install all (3)' }).click();
  await expect(page.getByRole('status').filter({ hasText: 'Installed 3 patches' })).toBeVisible();

  await page.getByRole('tab', { name: 'Library' }).click();
  await expect(page.getByRole('button', { name: 'Download changes (3)' })).toBeEnabled();
  await expect(page.locator('tr[data-path="synth/test-tones/tone-cluster.aif"]')).toBeVisible();
  await expect(page.locator('tr[data-path="drum/test-tones/tone-drum.aif"]')).toBeVisible();

  // Installing again picks free names instead of replacing anything.
  await page.getByRole('tab', { name: 'Packs' }).click();
  await page.getByRole('checkbox', { name: 'Select synth/tone-cluster.aif' }).check();
  await page.getByRole('button', { name: 'Install selected (1)' }).click();
  await expect(page.getByRole('status').filter({ hasText: 'Installed 1 patch.' })).toContainText(
    'synth/test-tones/tone-cluster-2.aif',
  );
  await page.getByRole('tab', { name: 'Library' }).click();
  const download = page.getByRole('button', { name: 'Download changes (4)' });
  await expect(download).toBeEnabled();

  // After a download, leaving asks nothing; a later change that keeps the count the same
  // (renaming a staged file) makes the changes unsaved again.
  await Promise.all([page.waitForEvent('download'), download.click()]);
  await expect(page.getByText('All changes are downloaded.')).toBeVisible();
  const row = page.locator('tr[data-path="synth/test-tones/tone-cluster-2.aif"]');
  await row.getByRole('button', { name: 'More actions for tone-cluster-2.aif' }).click();
  await row.getByRole('button', { name: 'Rename file tone-cluster-2.aif' }).click();
  const rename = page.getByRole('dialog', { name: 'Rename tone-cluster-2.aif' });
  await rename.getByLabel('New file name').fill('second.aif');
  await rename.getByRole('button', { name: 'Rename' }).click();
  await expect(page.locator('tr[data-path="synth/test-tones/second.aif"]')).toBeVisible();
  await expect(page.getByRole('button', { name: 'Download changes (4)' })).toBeEnabled();
  await expect(page.getByText('All changes are downloaded.')).toBeHidden();
  await page.getByRole('button', { name: 'Disconnect' }).click();
  await expect(page.getByRole('dialog', { name: 'Discard staged changes?' })).toBeVisible();
});

test('a second install cannot start while one is running', async ({ page }) => {
  await page.goto('./');
  await page.getByLabel('Choose the OP-1 drive folder').setInputFiles(drive);
  await expect(page.locator('tbody tr[data-path]')).toHaveCount(1);
  await page.getByRole('tab', { name: 'Packs' }).click();
  await page.getByRole('button', { name: 'View Test Tones' }).click();
  // Hold the patch downloads so the install stays in the queue.
  let release: () => void = () => {};
  const held = new Promise<void>((resolve) => (release = resolve));
  await page.route('**/packs/test-tones/**/*.aif', async (route) => {
    await held;
    await route.continue();
  });
  const all = page.getByRole('button', { name: 'Install all (3)' });
  await all.click();
  await expect(all).toBeDisabled();
  release();
  await expect(page.getByRole('status').filter({ hasText: 'Installed 3 patches' })).toBeVisible();
  await page.getByRole('tab', { name: 'Library' }).click();
  await expect(page.getByRole('button', { name: 'Download changes (3)' })).toBeEnabled();
});
