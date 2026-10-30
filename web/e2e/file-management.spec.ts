import { test, expect, type Page } from '@playwright/test';
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const fixtures = fileURLToPath(new URL('../test/fixtures/', import.meta.url));
const synth = readFileSync(join(fixtures, 'synth-cluster.aif')).toString('base64');
const drum = readFileSync(join(fixtures, 'drum-boombap.aif')).toString('base64');

/** Puts files in a folder in the browser's private storage, and has the folder picker return it. */
async function myFolder(page: Page, files: Record<string, string>): Promise<void> {
  await page.evaluate(async (entries) => {
    const opfs = await navigator.storage.getDirectory();
    await opfs.removeEntry('mine', { recursive: true }).catch(() => {});
    const folder = await opfs.getDirectoryHandle('mine', { create: true });
    for (const [path, base64] of Object.entries(entries)) {
      const parts = path.split('/');
      const name = parts.pop() as string;
      let dir = folder;
      for (const part of parts) dir = await dir.getDirectoryHandle(part, { create: true });
      const writable = await (await dir.getFileHandle(name, { create: true })).createWritable();
      await writable.write(Uint8Array.from(atob(base64), (c) => c.charCodeAt(0)));
      await writable.close();
    }
    Object.defineProperty(window, 'showDirectoryPicker', { value: async () => folder, configurable: true });
  }, files);
}

const exists = (page: Page, path: string): Promise<boolean> =>
  page.evaluate(async (target) => {
    const parts = target.split('/');
    const name = parts.pop() as string;
    let dir = await (await navigator.storage.getDirectory()).getDirectoryHandle('mine');
    try {
      for (const part of parts) dir = await dir.getDirectoryHandle(part);
      await dir.getFileHandle(name).catch(() => dir.getDirectoryHandle(name));
      return true;
    } catch {
      return false;
    }
  }, path);

async function openPatchesFolder(page: Page): Promise<void> {
  await page.getByRole('tab', { name: 'Patches' }).click();
  await page.getByRole('button', { name: 'Choose a folder' }).click();
  await expect(page.locator('#panel-patches tr[data-path]').first()).toBeVisible();
}

test('moves, copies and deletes many patches in My folder on the Patches tab', async ({ page }) => {
  await page.goto('./');
  await myFolder(page, { 'pads/one.aif': synth, 'pads/two.aif': synth, 'kits/boom.aif': drum });
  await openPatchesFolder(page);
  const panel = page.locator('#panel-patches');

  await panel.getByRole('checkbox', { name: 'Select pads/one.aif' }).check();
  await panel.getByRole('checkbox', { name: 'Select pads/two.aif' }).check();
  await expect(panel.getByText('2 selected')).toBeVisible();

  await panel.getByRole('button', { name: 'Copy', exact: true }).click();
  await page.getByRole('dialog').getByLabel('Folder').selectOption('kits');
  await page.getByRole('dialog').getByRole('button', { name: 'Copy' }).click();
  await expect(panel.getByRole('status').filter({ hasText: 'Copied 2 patches' })).toBeVisible();
  await expect(panel.locator('tr[data-path="kits/one.aif"]')).toBeVisible();
  expect(await exists(page, 'pads/one.aif')).toBe(true);

  await panel.getByRole('checkbox', { name: 'Select pads/one.aif' }).check();
  await panel.getByRole('button', { name: 'Move', exact: true }).click();
  await page.getByRole('dialog').getByLabel('Folder').selectOption({ label: 'New folder…' });
  await page.getByRole('dialog').getByLabel('New folder name').fill('fresh');
  await page.getByRole('dialog').getByRole('button', { name: 'Move' }).click();
  await expect(panel.locator('tr[data-path="fresh/one.aif"]')).toBeVisible();
  await expect(panel.locator('tr[data-path="pads/one.aif"]')).toHaveCount(0);
  expect(await exists(page, 'pads/one.aif')).toBe(false);

  await panel.getByRole('checkbox', { name: 'Select all shown' }).check();
  await panel.getByRole('button', { name: 'Delete', exact: true }).click();
  const confirm = page.getByRole('dialog');
  await expect(confirm).toContainText('cannot be undone');
  await confirm.getByRole('button', { name: 'Delete' }).click();
  await expect(panel.getByRole('status').filter({ hasText: 'Deleted 5 patches' })).toBeVisible();
  expect(await exists(page, 'kits/boom.aif')).toBe(false);
});

test('renames a patch in My folder from its menu', async ({ page }) => {
  await page.goto('./');
  await myFolder(page, { 'pads/one.aif': synth });
  await openPatchesFolder(page);
  const row = page.locator('tr[data-path="pads/one.aif"]');
  await row.getByRole('button', { name: 'More actions for one.aif' }).click();
  await row.getByRole('button', { name: 'Rename file one.aif' }).click();
  await page.getByLabel('New file name').fill('better.aif');
  await page.getByRole('dialog').getByRole('button', { name: 'Rename' }).click();
  await expect(page.locator('tr[data-path="pads/better.aif"]')).toBeVisible();
  expect(await exists(page, 'pads/one.aif')).toBe(false);
});

test('adds and removes folders in My folder from the Library, with no OP-1', async ({ page }) => {
  await page.goto('./');
  await myFolder(page, { 'pads/deep/one.aif': synth });
  await openPatchesFolder(page);
  await page.getByRole('tab', { name: 'Library' }).click();
  const aside = page.locator('#panel-library aside');
  await aside.getByRole('button', { name: 'Add a local folder' }).click();
  await aside.getByLabel('New local folder name').fill('empty');
  await aside.getByRole('button', { name: 'Add folder' }).click();
  await expect(aside.getByRole('button', { name: /^empty/ })).toBeVisible();
  expect(await exists(page, 'empty')).toBe(true);

  await aside.getByRole('button', { name: /^pads/ }).click();
  await aside.getByRole('button', { name: 'Remove this local folder' }).click();
  await expect(aside.getByRole('alert')).toContainText('1 patch');
  await aside.getByRole('button', { name: 'Remove', exact: true }).click();
  await expect(aside.getByRole('button', { name: /^pads/ })).toHaveCount(0);
  expect(await exists(page, 'pads')).toBe(false);
});

test.describe('with a dropped drive', () => {
  let parent: string;
  let drive: string;

  test.beforeEach(() => {
    parent = mkdtempSync(join(tmpdir(), 'co-op-e2e-files-'));
    drive = join(parent, 'OP-1');
    mkdirSync(join(drive, 'synth', 'user'), { recursive: true });
    mkdirSync(join(drive, 'drum', 'user'), { recursive: true });
    mkdirSync(join(drive, 'tape'), { recursive: true });
    copyFileSync(join(fixtures, 'synth-cluster.aif'), join(drive, 'synth', 'user', 'one.aif'));
    copyFileSync(join(fixtures, 'drum-boombap.aif'), join(drive, 'drum', 'user', 'kit.aif'));
  });

  test.afterEach(() => {
    rmSync(parent, { recursive: true, force: true });
  });

  test('stages a bulk move and delete, with one folder picker per kind', async ({ page }) => {
    await page.goto('./');
    await page.getByLabel('Choose the OP-1 drive folder').setInputFiles(drive);
    await page.getByRole('checkbox', { name: 'Select all shown patches' }).check();
    await page.locator('#panel-library').getByRole('button', { name: 'Move', exact: true }).click();
    const dialog = page.getByRole('dialog');
    await expect(dialog.getByLabel('synth patches go to')).toBeVisible();
    await expect(dialog.getByLabel('drum patches go to')).toBeVisible();
    await dialog.getByLabel('synth patches go to').selectOption({ label: 'New folder…' });
    await dialog.getByLabel('New folder name').fill('pads');
    await dialog.getByRole('button', { name: 'Move' }).click();
    await expect(page.locator('#panel-library').getByRole('status').filter({ hasText: 'Moved 2 patches' })).toBeVisible();
    await expect(page.locator('tr[data-path="synth/pads/one.aif"]')).toBeVisible();

    await page.getByRole('checkbox', { name: 'Select all shown patches' }).check();
    await page.locator('#panel-library').getByRole('button', { name: 'Delete', exact: true }).click();
    await expect(page.getByRole('dialog')).toContainText('listed for deletion');
    await page.getByRole('dialog').getByRole('button', { name: 'Delete' }).click();
    await expect(page.locator('#panel-library').getByRole('status').filter({ hasText: 'Deleted 2 patches' })).toBeVisible();
    await expect(page.getByRole('button', { name: /Download changes/ })).toBeEnabled();
  });

  test('copies OP-1 patches to My folder and leaves them on the OP-1', async ({ page }) => {
    await page.goto('./');
    await myFolder(page, {});
    await page.getByLabel('Choose the OP-1 drive folder').setInputFiles(drive);
    await page.getByRole('checkbox', { name: 'Select all shown patches' }).check();
    await page.locator('#panel-library').getByRole('button', { name: 'Copy to My folder (2)' }).click();
    await page.getByRole('dialog').getByRole('button', { name: 'Copy' }).click();
    await expect(page.locator('#panel-library').getByRole('status').filter({ hasText: 'Copied 2' })).toBeVisible();
    expect(await exists(page, 'op-1/synth/one.aif')).toBe(true);
    expect(await exists(page, 'op-1/drum/kit.aif')).toBe(true);
    await expect(page.locator('tr[data-path="synth/user/one.aif"]')).toBeVisible();
  });
});
