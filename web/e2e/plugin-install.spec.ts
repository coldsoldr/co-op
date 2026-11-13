import { test, expect, type Page } from '@playwright/test';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { crc32 } from 'node:zlib';
import { fileURLToPath } from 'node:url';
import { stubOp1 } from './op1-stub';
import { serveHostedOnly, openPlugins } from './plugin-index';

test.beforeEach(async ({ page }) => {
  await serveHostedOnly(page);
});

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
  await openPlugins(page);
  await page.getByRole('article', { name: 'ensemble', exact: true }).getByRole('button', { name: 'Install ensemble.knl over USB' }).click();
  return page.getByRole('dialog', { name: 'Install ensemble over USB' });
}

test('tries a plugin: installs it, then takes it off again after T1', async ({ page }) => {
  await stubOp1(page, {
    inventory: inventoryOf(DELAY),
    inventoryAfterRestart: inventoryOf(DELAY, ENSEMBLE('loaded')),
    inventoryAfterRemove: inventoryOf(DELAY),
  });
  await page.goto('./');
  await openPlugins(page);
  const card = page.getByRole('article', { name: 'ensemble', exact: true });
  await card.getByRole('button', { name: 'Try ensemble on my OP-1' }).click();
  const install = page.getByRole('dialog', { name: 'Install ensemble over USB' });
  await install.getByRole('button', { name: 'Install ensemble.knl' }).click();
  await expect(install.getByRole('status')).toContainText('Press T1');
  await page.evaluate(() => window.op1Stub.press('T1'));
  await expect(install.locator('[data-state="loaded"]')).toBeVisible();
  await install.getByRole('button', { name: 'Close' }).click();

  await expect(card.getByRole('status')).toContainText('ensemble is on your OP-1');
  await card.getByRole('button', { name: 'Remove ensemble' }).click();
  const remove = page.getByRole('dialog', { name: 'Remove ensemble' });
  await remove.getByRole('button', { name: 'Remove ensemble' }).click();
  await expect(remove.getByRole('status')).toContainText('Press T1 on the OP-1 to remove it');
  await page.evaluate(() => window.op1Stub.press('T1'));
  await expect(remove.getByRole('status')).toHaveText('ensemble is off the OP-1.');
  expect(await page.evaluate(() => window.op1Stub.removed)).toEqual(['plugins/ensemble.knl']);
  await remove.getByRole('button', { name: 'Close' }).click();
  await expect(card.getByRole('button', { name: 'Try ensemble on my OP-1' })).toBeVisible();
  await expect(card.getByRole('button', { name: 'Keep ensemble' })).toBeHidden();
});

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

test('stays open while the OP-1 shows its prompt: the answer is T1 or T2 there', async ({ page }) => {
  await stubOp1(page, { inventory: inventoryOf(DELAY) });
  const dialog = await openInstall(page);
  await dialog.getByRole('button', { name: 'Install ensemble.knl' }).click();
  const prompt = dialog.getByRole('status');
  await expect(prompt).toContainText('to cancel, press T2 there');
  await expect(prompt).toBeFocused();
  await expect(dialog.getByRole('button')).toHaveCount(0);
  await page.keyboard.press('Escape');
  await expect(dialog).toBeVisible();
  expect(await page.evaluate(() => window.op1Stub.prompting)).toBe(true);

  await page.evaluate(() => window.op1Stub.press('T2'));
  await expect(prompt).toHaveText('Cancelled on the OP-1 (T2). Nothing was installed.');
  await dialog.getByRole('button', { name: 'Close' }).click();
  await expect(dialog).toBeHidden();
});

test('flags a plugin already on the OP-1 and overwrites it on request, in one restart', async ({ page }) => {
  const older = ['fx', 'ensemble', 'knl', 'plugins/ensemble.knl', '0.1', 'coldsoldr', '20000', '0badf00d', 'loaded', '', ''].join('\t');
  await stubOp1(page, {
    inventory: inventoryOf(DELAY, older),
    inventoryAfterRestart: inventoryOf(DELAY, ENSEMBLE('loaded')),
  });
  const dialog = await openInstall(page);
  await dialog.getByRole('button', { name: 'Install ensemble.knl' }).click();

  await expect(dialog.getByRole('status')).toHaveText('ensemble is already on your OP-1.');
  const version = dialog.getByRole('row', { name: /^Version/ });
  await expect(version).toContainText('0.1');
  await expect(version).toContainText('0.2');
  await expect(dialog).toContainText('Different versions: 0.1 on the OP-1, 0.2 in co-op.');
  await expect(dialog).toContainText(/The one in co-op is [\d.]+ KB larger\./);
  // Nothing has changed on the OP-1 yet.
  expect(await writesOf(page)).toEqual([]);

  await dialog.getByRole('button', { name: 'Overwrite ensemble' }).click();
  await expect(dialog.getByRole('status')).toContainText('Press T1 on the OP-1 to install and restart');
  await page.evaluate(() => window.op1Stub.press('T1'));
  await expect(dialog.locator('[data-state="loaded"]')).toHaveText('ensemble is installed and loaded.');
  expect(await page.evaluate(() => window.op1Stub.removed)).toEqual([]);
  expect((await page.evaluate(() => window.op1Stub.installed)).map((f) => f.path)).toEqual(['plugins/ensemble.knl']);
  expect(await writesOf(page)).not.toContain(10);
});

test('changes nothing on the OP-1 when the overwrite is declined in co-op', async ({ page }) => {
  await stubOp1(page, { inventory: inventoryOf(DELAY, ENSEMBLE('loaded')) });
  const dialog = await openInstall(page);
  await dialog.getByRole('button', { name: 'Install ensemble.knl' }).click();
  await dialog.getByRole('button', { name: 'Cancel' }).click();
  await expect(dialog).toBeHidden();
  expect(await writesOf(page)).toEqual([]);
});

test('offers USB install on hundekraft plugin cards and manuals', async ({ page }) => {
  await page.goto('./');
  await openPlugins(page);
  await expect(page.getByRole('button', { name: /over USB$/ })).toHaveCount(11);
  await page.getByRole('button', { name: 'Read the manual of rings' }).click();
  await expect(page.getByRole('button', { name: 'Install rings.knl over USB' })).toBeVisible();
});

/** ensemble.knl renamed to a synth called erdtree (saved name, kind and CRC-32), a stand-in for the author's file. */
function standInErdtree(): Buffer {
  const bytes = Buffer.from(ensemble);
  const h = bytes.indexOf('KNPL');
  bytes.fill(0, h + 20, h + 36);
  bytes.write('erdtree', h + 20, 'latin1');
  bytes[h + 18] = 1;
  bytes.writeUInt32LE(0, h + 12);
  bytes.writeUInt32LE(crc32(bytes), h + 12);
  return bytes;
}
const erdtreeKnl = standInErdtree();

test('installs a listed plugin from its author: fetched from its fileUrl, checked, then sent like a hosted one', async ({ page }) => {
  // The real index, with erdtree's hundekraft build pointed at a stand-in file so the test needs no network.
  const fileUrl = 'https://raw.githubusercontent.com/someone/erdtree/0123456789abcdef0123456789abcdef01234567/build/erdtree.knl';
  const sha256 = createHash('sha256').update(erdtreeKnl).digest('hex');
  await page.unroute('**/plugins/index.json');
  await page.route('**/plugins/index.json', async (route) => {
    const response = await route.fetch();
    const index = (await response.json()) as { plugins: Array<Record<string, unknown> & { builds?: Array<Record<string, unknown>> }> };
    const erdtree = index.plugins.find((p) => p.id === 'erdtree');
    const hundekraft = erdtree?.builds?.find((b) => b.firmware === 'op1/hundekraft');
    if (!erdtree || !hundekraft) throw new Error('erdtree and its hundekraft build are in the index');
    for (const entry of [erdtree, hundekraft]) Object.assign(entry, { fileUrl, sha256, sizeBytes: erdtreeKnl.length });
    await route.fulfill({ response, json: index });
  });
  let fetched = 0;
  await page.route(fileUrl, async (route) => {
    fetched += 1;
    await route.fulfill({ body: erdtreeKnl, headers: { 'access-control-allow-origin': '*' } });
  });
  const ERDTREE = ['synth', 'erdtree', 'knl', 'plugins/erdtree.knl', '0.5.0', 'Sammi', String(erdtreeKnl.length), '0badf00d', 'loaded', '', ''].join('\t');
  await stubOp1(page, { inventory: inventoryOf(DELAY), inventoryAfterRestart: inventoryOf(DELAY, ERDTREE) });

  await page.goto('./');
  await page.getByRole('tab', { name: 'Plugins' }).click();
  await page.getByRole('button', { name: /^SammiBabe666/ }).click();
  const card = page.getByRole('article', { name: 'erdtree', exact: true });
  await expect(card.getByRole('link', { name: /Get it from github\.com/ })).toBeVisible();
  await expect(card).toContainText("fetches erdtree.knl from its author's repo");
  await expect(card.getByRole('button', { name: /^Download/ })).toHaveCount(0);
  // A listed plugin with no fileUrl still only links out.
  await page.getByRole('button', { name: /^fungiboletus/ }).click();
  await expect(page.getByRole('article', { name: 'Moai', exact: true }).getByRole('button', { name: /over USB$/ })).toHaveCount(0);

  await card.getByRole('button', { name: 'Install erdtree.knl over USB' }).click();
  const dialog = page.getByRole('dialog', { name: 'Install erdtree over USB' });
  await dialog.getByRole('button', { name: 'Install erdtree.knl' }).click();
  await expect(dialog.getByRole('status')).toContainText('Press T1 on the OP-1 to install and restart');
  await page.evaluate(() => window.op1Stub.press('T1'));
  await expect(dialog.locator('[data-state="loaded"]')).toHaveText('erdtree is installed and loaded.');
  expect(fetched).toBeGreaterThan(0);
  const installed = await page.evaluate(() => window.op1Stub.installed);
  expect(installed.map((f) => f.path)).toEqual(['plugins/erdtree.knl']);
  expect(Buffer.from(installed[0]?.bytes ?? []).equals(erdtreeKnl)).toBe(true);
});

test('refuses a listed plugin whose fetched file does not match the index', async ({ page }) => {
  const fileUrl = 'https://raw.githubusercontent.com/someone/erdtree/0123456789abcdef0123456789abcdef01234567/build/erdtree.knl';
  await page.unroute('**/plugins/index.json');
  await page.route('**/plugins/index.json', async (route) => {
    const response = await route.fetch();
    const index = (await response.json()) as { plugins: Array<Record<string, unknown> & { builds?: Array<Record<string, unknown>> }> };
    const erdtree = index.plugins.find((p) => p.id === 'erdtree');
    const hundekraft = erdtree?.builds?.find((b) => b.firmware === 'op1/hundekraft');
    if (!erdtree || !hundekraft) throw new Error('erdtree and its hundekraft build are in the index');
    for (const entry of [erdtree, hundekraft]) Object.assign(entry, { fileUrl, sha256: 'ab'.repeat(32), sizeBytes: ensemble.length });
    await route.fulfill({ response, json: index });
  });
  await page.route(fileUrl, (route) => route.fulfill({ body: ensemble, headers: { 'access-control-allow-origin': '*' } }));
  await stubOp1(page, { inventory: inventoryOf(DELAY) });

  await page.goto('./');
  await page.getByRole('tab', { name: 'Plugins' }).click();
  await page.getByRole('button', { name: /^SammiBabe666/ }).click();
  await page.getByRole('article', { name: 'erdtree', exact: true }).getByRole('button', { name: 'Install erdtree.knl over USB' }).click();
  const dialog = page.getByRole('dialog', { name: 'Install erdtree over USB' });
  await dialog.getByRole('button', { name: 'Install erdtree.knl' }).click();
  await expect(dialog.getByRole('alert')).toContainText('does not match the plugin index');
  expect(await writesOf(page)).toEqual([]);
});
