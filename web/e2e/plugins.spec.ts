import { test, expect } from '@playwright/test';
import { readFileSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const seed = fileURLToPath(new URL('../../plugins/', import.meta.url));
const SEED_IDS = ['bitcrusher', 'drive', 'ensemble', 'freq_shift', 'grids', 'marbles', 'rings', 'tape_drive', 'tape_echo', 'wavefolder'];
const seedNames = SEED_IDS.map((id) => (JSON.parse(readFileSync(`${seed}${id}/plugin.json`, 'utf8')) as { name: string }).name);

test('lists the seed plugins and filters them', async ({ page }) => {
  await page.goto('./');
  await page.getByRole('tab', { name: 'Plugins' }).click();
  await expect(page.getByRole('status').filter({ hasText: '10 plugins' })).toBeVisible();
  await expect(page.locator('#panel-plugins article')).toHaveCount(10);
  for (const name of seedNames) await expect(page.getByRole('article', { name, exact: true })).toBeVisible();

  const rings = page.getByRole('article', { name: 'rings', exact: true });
  await expect(rings).toContainText('coldsoldr');
  await expect(rings).toContainText('version 0.1');
  await expect(rings).toContainText('GPL-3.0-or-later');
  await expect(rings).toContainText('Needs hundekraft 3.0.3.');
  await expect(rings).toContainText('Mutable Instruments Rings');
  await expect(rings.getByRole('img', { name: 'The rings screen on the OP-1' })).toHaveAttribute('loading', 'lazy');
  const source = rings.getByRole('link', { name: 'Source code of rings' });
  await expect(source).toHaveAttribute('href', 'https://github.com/coldsoldr/te-op1-cfw');
  await expect(source).toHaveAttribute('target', '_blank');
  await expect(source).toHaveAttribute('rel', /noopener/);
  await expect(rings.getByRole('link', { name: 'hundekraft workshop' })).toHaveAttribute('href', 'https://hundekraft.github.io/op1-workshop/');
  await expect(rings.getByRole('button', { name: 'Download rings.knl' })).toHaveAccessibleDescription(/runs as firmware code/);

  await page.getByLabel('Kind').selectOption('synth');
  await expect(page.locator('#panel-plugins article')).toHaveCount(1);
  await expect(rings).toBeVisible();
  await expect(page.getByRole('status').filter({ hasText: '1 of 10 plugins' })).toBeVisible();
  await page.getByLabel('Kind').selectOption('feature');
  await expect(page.locator('#panel-plugins article')).toHaveCount(2);
  await page.getByLabel('Kind').selectOption('fx');
  await expect(page.locator('#panel-plugins article')).toHaveCount(7);
  await page.getByRole('searchbox', { name: 'Search plugins' }).fill('tape');
  await expect(page.locator('#panel-plugins article')).toHaveCount(2);
  await page.getByRole('searchbox', { name: 'Search plugins' }).fill('no such plugin');
  await expect(page.getByText('No plugins match')).toBeVisible();
});

test('opens a manual with its table and screenshot', async ({ page }) => {
  await page.goto('./');
  await page.getByRole('tab', { name: 'Plugins' }).click();
  await page.getByRole('button', { name: 'Read the manual of rings' }).click();
  await expect(page.getByRole('heading', { name: 'rings', level: 3 })).toBeFocused();

  const manual = page.getByRole('region', { name: 'Manual of rings' });
  await expect(manual.getByRole('heading', { name: 'Controls' })).toBeVisible();
  const table = manual.getByRole('table');
  await expect(table).toBeVisible();
  await expect(table.getByRole('columnheader', { name: 'Encoder' })).toBeVisible();
  await expect(table.getByRole('row')).toHaveCount(5);
  await expect(manual.getByRole('listitem').filter({ hasText: 'FM voice' })).toBeVisible();

  const screenshot = manual.getByRole('img', { name: 'rings screen' });
  await expect(screenshot).toHaveAttribute('src', '/co-op/plugins/rings/screenshot.png');
  await screenshot.scrollIntoViewIfNeeded();
  await expect.poll(() => screenshot.evaluate((img: HTMLImageElement) => img.complete && img.naturalWidth)).toBe(960);
  await expect(manual.getByRole('link', { name: 'hundekraft workshop' })).toHaveAttribute('target', '_blank');

  await page.getByRole('button', { name: 'Back to plugins' }).click();
  await expect(page.getByRole('button', { name: 'Read the manual of rings' })).toBeFocused();
});

test('downloads a plugin file', async ({ page }) => {
  await page.goto('./');
  await page.getByRole('tab', { name: 'Plugins' }).click();
  const [download] = await Promise.all([
    page.waitForEvent('download'),
    page.getByRole('article', { name: 'rings', exact: true }).getByRole('button', { name: 'Download rings.knl' }).click(),
  ]);
  expect(download.suggestedFilename()).toBe('rings.knl');
  const saved = await download.path();
  expect(statSync(saved).size).toBe(statSync(`${seed}rings/rings.knl`).size);
  expect(readFileSync(saved).equals(readFileSync(`${seed}rings/rings.knl`))).toBe(true);
});

test('a download that fails its check says so', async ({ page }) => {
  await page.goto('./');
  await page.route('**/plugins/rings/rings.knl', (route) => route.fulfill({ body: 'not the plugin' }));
  await page.getByRole('tab', { name: 'Plugins' }).click();
  const rings = page.getByRole('article', { name: 'rings', exact: true });
  await rings.getByRole('button', { name: 'Download rings.knl' }).click();
  await expect(rings.getByRole('alert')).toContainText('does not match the plugin index');
});
