import { test, expect, type Page } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { stubOp1 } from './op1-stub';

const ensemble = readFileSync(fileURLToPath(new URL('../../plugins/ensemble/ensemble.knl', import.meta.url)));

const HEADER = ['kennel-inventory 1', 'core 01e00000 0003a000 1a2b3c4d', 'presets 00042 cap 00100'].join('\t');
const DELAY = ['fx', 'delay', 'builtin', '', '', '', '0', '00000000', 'loaded', '', ''].join('\t');
const ENSEMBLE = (state: string, reason = ''): string =>
  ['fx', 'ensemble', 'knl', 'plugins/ensemble.knl', '0.2', 'coldsoldr', String(ensemble.length), '0badf00d', state, '', reason].join('\t');
const inventoryOf = (...records: string[]): string => [HEADER, ...records].join('\n');

/** The write commands: ABORT, BEGIN, DATA, END, COMMIT, and the ones co-op never sends. */
const WRITES = [2, 3, 4, 5, 6, 9, 10, 11];
const writesOf = (page: Page): Promise<number[]> =>
  page.evaluate((writes) => window.op1Stub.commands.filter((c) => writes.includes(c)), WRITES);

async function openInstall(page: Page) {
  await page.goto('./');
  await page.getByRole('tab', { name: 'Plugins' }).click();
  await page.getByRole('article', { name: 'ensemble', exact: true }).getByRole('button', { name: 'Install ensemble.knl over USB' }).click();
  return page.getByRole('dialog', { name: 'Install ensemble over USB' });
}

test('installs a seed plugin over USB: confirm, progress, T1, restart and the check after it', async ({ page }) => {
  await stubOp1(page, {
    inventory: inventoryOf(DELAY),
    inventoryAfterRestart: inventoryOf(DELAY, ENSEMBLE('loaded')),
    holdDataAt: 72 * 100,
  });
  const dialog = await openInstall(page);
  await expect(dialog).toContainText('must be out of disk mode');
  await expect(dialog.getByRole('listitem')).toContainText('plugins/ensemble.knl');
  await expect(dialog).toContainText('A plugin runs as firmware code on the OP-1.');
  await expect(dialog).toContainText('then restarts');
  await expect(dialog).toContainText('switch it off and on: it then starts without plugin files');
  await expect(dialog.getByRole('link', { name: 'hundekraft workshop' })).toHaveAttribute('href', 'https://hundekraft.github.io/op1-workshop/');
  // Cancel is the default.
  await expect(dialog.getByRole('button', { name: 'Cancel' })).toBeFocused();
  await dialog.getByRole('button', { name: 'Install ensemble.knl' }).click();

  // The stub holds one DATA reply, so the transfer can be seen half way.
  const progress = dialog.getByRole('progressbar', { name: 'Sending plugins/ensemble.knl' });
  await expect(progress).toHaveAttribute('value', String(72 * 100));
  await expect(progress).toHaveAttribute('max', String(ensemble.length));
  await expect(dialog.getByRole('status')).toContainText('Sending plugins/ensemble.knl to the OP-1');
  await page.evaluate(() => window.op1Stub.release());

  await expect(dialog.getByRole('status')).toContainText('Press T1 on the OP-1 to install and restart, or T2 to cancel.');
  await page.evaluate(() => window.op1Stub.press('T1'));

  await expect(dialog.locator('[data-state="loaded"]')).toHaveText('ensemble is installed and loaded.');
  const installed = await page.evaluate(() => window.op1Stub.installed);
  expect(installed.map((f) => f.path)).toEqual(['plugins/ensemble.knl']);
  expect(Buffer.from(installed[0]?.bytes ?? []).equals(ensemble)).toBe(true);
  // ABORT, BEGIN, DATA..., END, then COMMIT until T1; never OFF_SET, DELETE or RESTART.
  const writes = (await writesOf(page)).filter((c) => c !== 3);
  expect(writes.slice(0, 3)).toEqual([6, 2, 4]);
  expect(writes.length).toBeGreaterThanOrEqual(5);
  expect(writes.slice(3).every((c) => c === 5)).toBe(true);
  await dialog.getByRole('button', { name: 'Close' }).click();
  await expect(dialog).toBeHidden();
});

test('shows a plugin that failed to load after the restart', async ({ page }) => {
  await stubOp1(page, {
    inventory: inventoryOf(DELAY),
    inventoryAfterRestart: inventoryOf(DELAY, ENSEMBLE('failed', 'needs a newer core')),
  });
  const dialog = await openInstall(page);
  await dialog.getByRole('button', { name: 'Install ensemble.knl' }).click();
  await expect(dialog.getByRole('status')).toContainText('Press T1');
  await page.evaluate(() => window.op1Stub.press('T1'));
  await expect(dialog.locator('[data-state="failed"]')).toContainText('did not load (needs a newer core)');
});

test('reports T2 on the OP-1 as cancelled', async ({ page }) => {
  await stubOp1(page, { inventory: inventoryOf(DELAY) });
  const dialog = await openInstall(page);
  await dialog.getByRole('button', { name: 'Install ensemble.knl' }).click();
  await expect(dialog.getByRole('status')).toContainText('Press T1');
  await page.evaluate(() => window.op1Stub.press('T2'));
  await expect(dialog.getByRole('status')).toHaveText('Cancelled on the OP-1 (T2). Nothing was installed.');
  expect(await page.evaluate(() => window.op1Stub.installed)).toEqual([]);
});

test('refuses a plugin that is already installed, before sending anything', async ({ page }) => {
  await stubOp1(page, { inventory: inventoryOf(DELAY, ENSEMBLE('loaded')) });
  const dialog = await openInstall(page);
  await dialog.getByRole('button', { name: 'Install ensemble.knl' }).click();
  const alert = dialog.getByRole('alert');
  await expect(alert).toContainText('ensemble is already on the OP-1 (plugins/ensemble.knl, loaded).');
  await expect(alert).toContainText('Remove it first in the hundekraft workshop');
  await expect(alert).toContainText('replacing a plugin in place can hang the restart');
  expect(await writesOf(page)).toEqual([]);
  await expect(dialog.getByRole('button', { name: 'Try again' })).toBeEnabled();
});

test('offers USB install on hundekraft plugin cards and manuals', async ({ page }) => {
  await page.goto('./');
  await page.getByRole('tab', { name: 'Plugins' }).click();
  await expect(page.getByRole('button', { name: /over USB$/ })).toHaveCount(10);
  await page.getByRole('button', { name: 'Read the manual of rings' }).click();
  await expect(page.getByRole('button', { name: 'Install rings.knl over USB' })).toBeVisible();
});
