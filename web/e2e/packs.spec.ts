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
  await expect(page.getByRole('status').filter({ hasText: 'Installed 3 patch(es)' })).toBeVisible();

  await page.getByRole('tab', { name: 'Library' }).click();
  await expect(page.getByRole('button', { name: 'Download changes (3)' })).toBeEnabled();
  await expect(page.locator('tr[data-path="synth/test-tones/tone-cluster.aif"]')).toBeVisible();
  await expect(page.locator('tr[data-path="drum/test-tones/tone-drum.aif"]')).toBeVisible();

  // Installing again picks free names instead of replacing anything.
  await page.getByRole('tab', { name: 'Packs' }).click();
  await page.getByRole('checkbox', { name: 'Select synth/tone-cluster.aif' }).check();
  await page.getByRole('button', { name: 'Install selected (1)' }).click();
  await expect(page.getByRole('status').filter({ hasText: 'Installed 1 patch(es)' })).toContainText(
    'synth/test-tones/tone-cluster-2.aif',
  );
  await page.getByRole('tab', { name: 'Library' }).click();
  await expect(page.getByRole('button', { name: 'Download changes (4)' })).toBeEnabled();
});
