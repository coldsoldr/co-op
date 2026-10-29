import { test, expect, type Page } from '@playwright/test';
import { createHash } from 'node:crypto';
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readPatch, withMeta } from '../src/core/codec/patch';
import type { Op1Meta } from '../src/core/codec/op1-meta';
import { stubOp1 } from './op1-stub';

const fixtures = fileURLToPath(new URL('../test/fixtures/', import.meta.url));

/** The cluster fixture re-saved as a sound of the rings seed plugin, built by hand. */
function ringsPatch(): Uint8Array {
  const patch = readPatch(new Uint8Array(readFileSync(join(fixtures, 'synth-cluster.aif'))));
  return withMeta(patch, { ...patch.meta, type: 'rings', name: 'ring' } as Op1Meta).raw;
}

/** Adds a hundekraft pack holding the rings sound to the real catalogue; nothing like it is committed. */
async function serveRingsPack(page: Page): Promise<void> {
  const bytes = ringsPatch();
  const sha256 = createHash('sha256').update(bytes).digest('hex');
  await page.route('**/packs/index.json', async (route) => {
    const index = await (await route.fetch()).json();
    index.packs.push({
      slug: 'rings-tones',
      name: 'Rings Tones',
      author: 'co-op',
      description: 'A rings sound for tests.',
      license: 'CC0-1.0',
      firmware: ['op1/hundekraft'],
      tags: ['test'],
      sizeBytes: bytes.length,
      hasReadme: false,
      patches: [{ file: 'synth/ring.aif', kind: 'synth', name: 'ring', engine: 'rings', sizeBytes: bytes.length, sha256 }],
    });
    await route.fulfill({ json: index });
  });
  await page.route('**/packs/rings-tones/synth/ring.aif', (route) =>
    route.fulfill({ body: Buffer.from(bytes), contentType: 'audio/x-aiff' }),
  );
}

let parent: string;
let drive: string;

test.beforeEach(() => {
  parent = mkdtempSync(join(tmpdir(), 'co-op-e2e-needs-'));
  drive = join(parent, 'OP-1');
  mkdirSync(join(drive, 'synth', 'user'), { recursive: true });
  copyFileSync(join(fixtures, 'synth-cluster.aif'), join(drive, 'synth', 'user', 'cluster.aif'));
});

test.afterEach(() => {
  rmSync(parent, { recursive: true, force: true });
});

test('asks to install the plugin a pack patch needs, and installs the patch without it', async ({ page }) => {
  await serveRingsPack(page);
  await page.goto('./');
  await page.getByLabel('Choose the OP-1 drive folder').setInputFiles(drive);
  await page.getByLabel('Profile').selectOption('op1/hundekraft');
  await expect(page.getByText('chosen by hand')).toBeVisible();

  await page.getByRole('tab', { name: 'Packs' }).click();
  await page.getByRole('button', { name: 'View Rings Tones' }).click();
  const install = page.getByRole('button', { name: 'Install all (1)' });
  await install.click();

  // A library plugin is a warning, not an unsupported engine: no write guard, the plugin prompt.
  const dialog = page.getByRole('dialog', { name: 'Plugin needed' });
  await expect(dialog).toContainText('This patch needs the plugin rings. Install it?');
  await expect(dialog.getByRole('button', { name: 'Install rings.knl over USB' })).toBeVisible();
  await expect(dialog.getByRole('button', { name: 'Download rings.knl' })).toBeVisible();
  await expect(dialog.getByRole('button', { name: 'Cancel' })).toBeFocused();
  await dialog.getByRole('button', { name: 'Cancel' }).click();
  await expect(dialog).toBeHidden();
  await expect(page.getByText('Installed 1 patch')).toBeHidden();

  await install.click();
  await dialog.getByRole('button', { name: 'Continue without' }).click();
  await expect(page.getByRole('status').filter({ hasText: 'Installed 1 patch' })).toBeVisible();

  await page.getByRole('tab', { name: 'Library' }).click();
  const row = page.locator('tr[data-path="synth/rings-tones/ring.aif"]');
  await expect(row.locator('.badge')).toContainText('warn');
  await expect(row.locator('.badge')).toContainText('needs plugin "rings"');
  await row.getByText('details').click();
  await expect(row.locator('.traits')).toContainText('Needs plugin');
  await expect(row.locator('.traits')).toContainText('rings (rings.knl)');
});

test('installs without asking when the plugin index cannot be loaded', async ({ page }) => {
  await serveRingsPack(page);
  await page.route('**/plugins/index.json', (route) => route.fulfill({ status: 500, body: 'down' }));
  await page.goto('./');
  await page.getByLabel('Choose the OP-1 drive folder').setInputFiles(drive);
  await page.getByLabel('Profile').selectOption('op1/hundekraft');
  await expect(page.getByText('chosen by hand')).toBeVisible();

  await page.getByRole('tab', { name: 'Packs' }).click();
  await page.getByRole('button', { name: 'View Rings Tones' }).click();
  await page.getByRole('button', { name: 'Install all (1)' }).click();
  // Without the library, rings is an engine hundekraft does not know: the write guard asks instead.
  await page.getByRole('dialog', { name: 'Not supported on OP-1 (hundekraft)' }).getByRole('button', { name: 'Write anyway' }).click();
  await expect(page.getByRole('status').filter({ hasText: 'Installed 1 patch' })).toBeVisible();
  await expect(page.getByRole('dialog', { name: 'Plugin needed' })).toBeHidden();
});

test('judges an open drive again after a USB check without reading it, as it is gone out of disk mode', async ({ page }) => {
  const inventory = [
    'kennel-inventory 1',
    ['synth', 'rings', 'knl', 'plugins/rings.knl', '1.0', 'coldsoldr', '111768', '0badf00d', 'loaded', '+engine:rings', ''].join('\t'),
  ].join('\n');
  await stubOp1(page, { answer: true, inventory });
  await page.goto('./');
  // The browser's origin-private file system stands in for the drive behind the folder picker.
  const ring = Buffer.from(ringsPatch()).toString('base64');
  await page.evaluate(async (base64) => {
    const root = await navigator.storage.getDirectory();
    for (const name of ['synth', 'drum', 'tape']) await root.removeEntry(name, { recursive: true }).catch(() => {});
    await root.getDirectoryHandle('tape', { create: true });
    await root.getDirectoryHandle('drum', { create: true }).then((d) => d.getDirectoryHandle('user', { create: true }));
    const user = await (await root.getDirectoryHandle('synth', { create: true })).getDirectoryHandle('user', { create: true });
    const writable = await (await user.getFileHandle('ring.aif', { create: true })).createWritable();
    await writable.write(Uint8Array.from(atob(base64), (c) => c.charCodeAt(0)));
    await writable.close();
    Object.defineProperty(window, 'showDirectoryPicker', { value: async () => root, configurable: true });
  }, ring);
  await page.getByRole('button', { name: 'Connect OP-1' }).click();
  await page.getByLabel('Profile').selectOption('op1/hundekraft');
  const row = page.locator('tr[data-path="synth/user/ring.aif"]');
  await expect(row.locator('.badge')).toContainText('needs plugin "rings"');

  // Out of disk mode the drive is gone: here its folders are, so a re-read would list nothing.
  await page.evaluate(async () => {
    const root = await navigator.storage.getDirectory();
    for (const name of ['synth', 'drum', 'tape']) await root.removeEntry(name, { recursive: true });
  });
  await page.getByRole('button', { name: 'Check over USB' }).click();
  const dialog = page.getByRole('dialog', { name: 'Check the OP-1 over USB' });
  await dialog.getByRole('button', { name: 'Check', exact: true }).click();
  await expect(dialog).toContainText('Firmware: hundekraft 3.x');
  await dialog.getByRole('button', { name: 'Close' }).click();

  await expect(row.locator('.badge')).toContainText('ok');
  await expect(row.locator('.badge')).not.toContainText('needs plugin');
  await expect(page.getByRole('alert')).toHaveCount(0);
});
