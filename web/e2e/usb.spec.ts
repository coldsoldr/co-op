import { test, expect, type Page } from '@playwright/test';
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { stubOp1, type StubOptions } from './op1-stub';

const fixtures = fileURLToPath(new URL('../test/fixtures/', import.meta.url));

const INVENTORY = [
  ['kennel-inventory 1', 'core 01e00000 0003a000 1a2b3c4d', 'presets 00042 cap 00100'].join('\t'),
  ['synth', 'rings', 'knl', 'plugins/rings.knl', '1.0', 'coldsoldr', '111768', '0badf00d', 'loaded', '+engine:rings', ''].join('\t'),
  ['fx', 'tape_echo', 'knl', 'plugins/tape_echo.knl', '1.1', 'coldsoldr', '53844', '12345678', 'off', '', 'switched off'].join('\t'),
  ['fx', 'delay', 'builtin', '', '', '', '0', '00000000', 'loaded', '', ''].join('\t'),
].join('\n');

/** The shared Web MIDI stub, with this file's inventory and switched-off list. */
const stub = (page: Page, options: Omit<StubOptions, 'inventory'> = {}): Promise<void> =>
  stubOp1(page, { inventory: INVENTORY, off: 'tape_echo', ...options });

let parent: string;
let drive: string;

test.beforeEach(() => {
  parent = mkdtempSync(join(tmpdir(), 'co-op-e2e-usb-'));
  drive = join(parent, 'OP-1');
  mkdirSync(join(drive, 'synth', 'user'), { recursive: true });
  mkdirSync(join(drive, 'drum', 'user'), { recursive: true });
  mkdirSync(join(drive, 'tape'), { recursive: true });
  copyFileSync(join(fixtures, 'synth-cluster.aif'), join(drive, 'synth', 'user', 'cluster.aif'));
});

test.afterEach(() => {
  rmSync(parent, { recursive: true, force: true });
});

const panel = (page: Page) => page.getByRole('dialog', { name: 'Check the OP-1 over USB' });

test('lists the plugins of a hundekraft OP-1 and remembers the profile for the next drive', async ({ page }) => {
  await stub(page, { answer: true });
  await page.goto('./');
  await page.getByRole('button', { name: 'Check over USB' }).click();
  const dialog = panel(page);
  await expect(dialog).toContainText('Leave disk mode first');
  await dialog.getByRole('button', { name: 'Check', exact: true }).click();

  await expect(dialog).toContainText('Firmware: hundekraft 3.x (kennel protocol 1)');
  await expect(dialog).toContainText('Synth presets: 42 of 100 (reported by the OP-1)');
  await expect(dialog.locator('tr[data-plugin="rings"] td')).toHaveText(['rings', 'synth', '.knl', '1.0', 'coldsoldr', 'loaded']);
  await expect(dialog.locator('tr[data-plugin="tape_echo"]')).toContainText('off (switched off)');
  await expect(dialog.locator('tr[data-plugin="delay"]')).toContainText('built-in');
  await expect(dialog).toContainText('Switched off: tape_echo');

  await dialog.getByRole('button', { name: 'Use OP-1 (hundekraft)' }).click();
  await expect(dialog).toContainText('The next drive you open starts on OP-1 (hundekraft).');
  await dialog.getByRole('button', { name: 'Close' }).click();
  await expect(dialog).toBeHidden();

  await page.getByLabel('Choose the OP-1 drive folder').setInputFiles(drive);
  await expect(page.getByLabel('Profile')).toHaveValue('op1/hundekraft');
  await expect(page.getByText('remembered from last time')).toBeVisible();
});

test('switches a dropped copy of the drive to the hundekraft profile at once', async ({ page }) => {
  await stub(page, { answer: true });
  await page.goto('./');
  await page.getByLabel('Choose the OP-1 drive folder').setInputFiles(drive);
  await expect(page.getByLabel('Profile')).toHaveValue('op1/stock');
  await expect(page.getByText('Read-only source')).toBeVisible();

  await page.getByRole('button', { name: 'Check over USB' }).click();
  const dialog = panel(page);
  await dialog.getByRole('button', { name: 'Check', exact: true }).click();
  await dialog.getByRole('button', { name: 'Use OP-1 (hundekraft)' }).click();
  await expect(dialog).toContainText('The library now uses OP-1 (hundekraft).');
  await dialog.getByRole('button', { name: 'Close' }).click();
  await expect(page.getByLabel('Profile')).toHaveValue('op1/hundekraft');
  await expect(page.getByText('chosen by hand')).toBeVisible();
});

test('leaves a connected drive alone and starts it on the hundekraft profile when it is opened again', async ({
  page,
}) => {
  await stub(page, { answer: true });
  await page.goto('./');
  // The browser's origin-private file system stands in for the drive behind the folder picker.
  const cluster = readFileSync(join(fixtures, 'synth-cluster.aif')).toString('base64');
  await page.evaluate(async (base64) => {
    const root = await navigator.storage.getDirectory();
    for (const name of ['synth', 'drum', 'tape']) await root.removeEntry(name, { recursive: true }).catch(() => {});
    await root.getDirectoryHandle('tape', { create: true });
    await root.getDirectoryHandle('drum', { create: true }).then((d) => d.getDirectoryHandle('user', { create: true }));
    const user = await (await root.getDirectoryHandle('synth', { create: true })).getDirectoryHandle('user', { create: true });
    const writable = await (await user.getFileHandle('cluster.aif', { create: true })).createWritable();
    await writable.write(Uint8Array.from(atob(base64), (c) => c.charCodeAt(0)));
    await writable.close();
    Object.defineProperty(window, 'showDirectoryPicker', { value: async () => root, configurable: true });
  }, cluster);
  await page.getByRole('button', { name: 'Connect OP-1' }).click();
  await expect(page.locator('tbody tr[data-path]')).toHaveCount(1);
  await expect(page.getByLabel('Profile')).toHaveValue('op1/stock');

  await page.getByRole('button', { name: 'Check over USB' }).click();
  const dialog = panel(page);
  await dialog.getByRole('button', { name: 'Check', exact: true }).click();
  await dialog.getByRole('button', { name: 'Use OP-1 (hundekraft)' }).click();
  await expect(dialog).toContainText('The drive starts on OP-1 (hundekraft) when you open it again.');
  await dialog.getByRole('button', { name: 'Close' }).click();
  // The drive is not reloaded (on a real OP-1 it is gone while out of disk mode).
  await expect(page.getByLabel('Profile')).toHaveValue('op1/stock');
  await expect(page.getByRole('alert')).toHaveCount(0);

  await page.getByRole('button', { name: 'Disconnect' }).click();
  await page.getByRole('button', { name: 'Connect OP-1' }).click();
  await expect(page.getByLabel('Profile')).toHaveValue('op1/hundekraft');
  await expect(page.getByText('remembered from last time')).toBeVisible();
});

test('explains an OP-1 that does not answer', async ({ page }) => {
  await page.clock.install();
  await stub(page, { answer: false });
  await page.goto('./');
  await page.getByRole('button', { name: 'Check over USB' }).click();
  const dialog = panel(page);
  await dialog.getByRole('button', { name: 'Check', exact: true }).click();
  await expect(dialog.getByRole('status')).toContainText('Checking the OP-1');
  // Five tries of three seconds each.
  for (let i = 0; i < 6; i++) await page.clock.runFor(3100);
  await expect(dialog).toContainText('Firmware: no answer.');
  await expect(dialog).toContainText('hundekraft 2.7 or older');
  await expect(dialog.getByRole('button', { name: /Use OP-1/ })).toHaveCount(0);
});

test('says when the OP-1 disconnects during the check', async ({ page }) => {
  await stub(page, { answer: true, disconnectOnList: true });
  await page.goto('./');
  await page.getByRole('button', { name: 'Check over USB' }).click();
  const dialog = panel(page);
  await dialog.getByRole('button', { name: 'Check', exact: true }).click();
  await expect(dialog.getByRole('alert')).toContainText('The OP-1 was disconnected during the check.');
  await expect(dialog.getByRole('button', { name: 'Check again' })).toBeEnabled();
});

test('points out disk mode when there is no OP-1 MIDI port', async ({ page }) => {
  await stub(page, { answer: true, noPorts: true });
  await page.goto('./');
  await page.getByRole('button', { name: 'Check over USB' }).click();
  const dialog = panel(page);
  await dialog.getByRole('button', { name: 'Check', exact: true }).click();
  await expect(dialog.getByRole('alert')).toContainText('not in disk mode');
});
