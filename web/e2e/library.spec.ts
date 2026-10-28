import { test, expect } from '@playwright/test';
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const fixtures = fileURLToPath(new URL('../test/fixtures/', import.meta.url));

/** An OP-1 drive folder: one synth patch, one drum patch and an empty tape folder. */
function buildDrive(): { parent: string; drive: string } {
  const parent = mkdtempSync(join(tmpdir(), 'co-op-e2e-'));
  const drive = join(parent, 'OP-1');
  mkdirSync(join(drive, 'synth', 'user'), { recursive: true });
  mkdirSync(join(drive, 'drum', 'user'), { recursive: true });
  mkdirSync(join(drive, 'tape'), { recursive: true });
  copyFileSync(join(fixtures, 'synth-cluster.aif'), join(drive, 'synth', 'user', 'cluster.aif'));
  copyFileSync(join(fixtures, 'drum-boombap.aif'), join(drive, 'drum', 'user', 'boombap.aif'));
  return { parent, drive };
}

let parent: string;
let drive: string;

test.beforeEach(() => {
  ({ parent, drive } = buildDrive());
});

test.afterEach(() => {
  rmSync(parent, { recursive: true, force: true });
});

test('opens a dropped drive read-only, stages a delete and downloads the changes', async ({ page }, testInfo) => {
  await page.goto('./');
  await expect(page.getByRole('tab', { name: 'Library' })).toHaveAttribute('aria-selected', 'true');
  await expect(page.getByRole('button', { name: 'Connect OP-1' })).toBeVisible();

  await page.getByLabel('Choose the OP-1 drive folder').setInputFiles(drive);

  const table = page.getByRole('table');
  await expect(table.getByRole('caption')).toHaveText('2 patch file(s) on OP-1');
  const rows = table.locator('tbody tr[data-path]');
  await expect(rows).toHaveCount(2);
  const synth = table.locator('tr[data-path="synth/user/cluster.aif"]');
  const drum = table.locator('tr[data-path="drum/user/boombap.aif"]');
  await expect(synth.getByRole('cell').nth(1)).toHaveText('synth');
  await expect(drum.getByRole('cell').nth(1)).toHaveText('drum');
  await expect(synth.locator('.badge')).toHaveText('ok');
  await expect(drum.locator('.badge')).toHaveText('ok');
  await expect(page.getByText('Read-only source')).toBeVisible();
  await expect(page.getByRole('button', { name: 'Download changes (0)' })).toBeDisabled();

  // SCREENSHOT=<path> saves the loaded library somewhere other than the test output folder.
  await page.screenshot({ path: process.env.SCREENSHOT ?? testInfo.outputPath('library.png'), fullPage: true });

  await drum.getByRole('button', { name: 'Delete boombap.aif' }).click();
  const dialog = page.getByRole('dialog', { name: 'Delete patch' });
  await expect(dialog).toContainText('drum/user/boombap.aif');
  await dialog.getByRole('button', { name: 'Delete' }).click();

  await expect(rows).toHaveCount(1);
  const download = page.getByRole('button', { name: 'Download changes (1)' });
  await expect(download).toBeEnabled();
  const [file] = await Promise.all([page.waitForEvent('download'), download.click()]);
  expect(file.suggestedFilename()).toMatch(/^op1-changes-\d{4}-\d{2}-\d{2}\.zip$/);

  await page.getByRole('tab', { name: 'Inspector' }).click();
  await expect(page.getByRole('heading', { name: 'Patch inspector' })).toBeVisible();
});

test('writes to a connected drive and recovers an interrupted write', async ({ page }) => {
  // The browser's origin-private file system stands in for the OP-1 drive behind the folder picker.
  const files = {
    'synth/user/cluster.aif': readFileSync(join(fixtures, 'synth-cluster.aif')).toString('base64'),
    // Left behind by a write interrupted after the old file was set aside.
    'drum/user/.co-op-old-boombap.aif': readFileSync(join(fixtures, 'drum-boombap.aif')).toString('base64'),
  };
  await page.goto('./');
  await page.evaluate(async (entries) => {
    const root = await navigator.storage.getDirectory();
    for (const name of ['synth', 'drum', 'tape']) await root.removeEntry(name, { recursive: true }).catch(() => {});
    await root.getDirectoryHandle('tape', { create: true });
    for (const [path, base64] of Object.entries(entries)) {
      const parts = path.split('/');
      const name = parts.pop() as string;
      let dir = root;
      for (const part of parts) dir = await dir.getDirectoryHandle(part, { create: true });
      const writable = await (await dir.getFileHandle(name, { create: true })).createWritable();
      await writable.write(Uint8Array.from(atob(base64), (c) => c.charCodeAt(0)));
      await writable.close();
    }
    Object.defineProperty(window, 'showDirectoryPicker', { value: async () => root, configurable: true });
  }, files);

  await page.getByRole('button', { name: 'Connect OP-1' }).click();
  await expect(page.getByText('An earlier write was interrupted')).toBeVisible();
  await expect(page.getByRole('status').filter({ hasText: 'interrupted' })).toContainText('drum/user/boombap.aif');
  await expect(page.locator('tbody tr[data-path]')).toHaveCount(2);
  await expect(page.getByText('Read-only source')).toBeHidden();

  const synth = page.locator('tr[data-path="synth/user/cluster.aif"]');
  await synth.getByRole('button', { name: 'Rename file cluster.aif' }).click();
  const dialog = page.getByRole('dialog', { name: 'Rename cluster.aif' });
  await dialog.getByLabel('New file name').fill('pad.aif');
  await dialog.getByRole('button', { name: 'Rename' }).click();
  await expect(page.locator('tr[data-path="synth/user/pad.aif"]')).toBeVisible();

  const names = await page.evaluate(async () => {
    const root = await navigator.storage.getDirectory();
    const list = async (dir: FileSystemDirectoryHandle): Promise<string[]> => {
      const out: string[] = [];
      for await (const name of (dir as unknown as { keys(): AsyncIterable<string> }).keys()) out.push(name);
      return out.sort();
    };
    const synthUser = await (await root.getDirectoryHandle('synth')).getDirectoryHandle('user');
    const drumUser = await (await root.getDirectoryHandle('drum')).getDirectoryHandle('user');
    return { synth: await list(synthUser), drum: await list(drumUser) };
  });
  expect(names).toEqual({ synth: ['pad.aif'], drum: ['boombap.aif'] });

  // Back up, delete a patch, and restore it from the backup.
  const [backup] = await Promise.all([
    page.waitForEvent('download'),
    page.getByRole('button', { name: 'Download backup' }).click(),
  ]);
  expect(backup.suggestedFilename()).toMatch(/^op1-backup-\d{4}-\d{2}-\d{2}\.zip$/);
  const backupPath = join(parent, backup.suggestedFilename());
  await backup.saveAs(backupPath);

  await page.getByRole('button', { name: 'Delete pad.aif' }).click();
  const confirm = page.getByRole('dialog', { name: 'Delete patch' });
  await expect(confirm).toContainText('synth/user/pad.aif');
  await confirm.getByRole('button', { name: 'Delete' }).click();
  await expect(page.locator('tbody tr[data-path]')).toHaveCount(1);

  await page.getByLabel('Restore from backup').setInputFiles(backupPath);
  const item = page.getByRole('checkbox', { name: /synth\/user\/pad\.aif/ });
  await expect(item).toBeChecked();
  await expect(page.getByText('1 file(s) are identical')).toBeVisible();
  await page.getByRole('button', { name: 'Restore 1 file(s)' }).click();
  await expect(page.getByText('Restored 1 file(s).')).toBeVisible();
  await expect(page.locator('tr[data-path="synth/user/pad.aif"]')).toBeVisible();
});

test('shows an operation error inline in the row', async ({ page }) => {
  await page.goto('./');
  await page.getByLabel('Choose the OP-1 drive folder').setInputFiles(drive);
  const synth = page.locator('tr[data-path="synth/user/cluster.aif"]');
  await expect(synth.locator('.badge')).toHaveText('ok');
  await synth.getByRole('button', { name: 'Rename file cluster.aif' }).click();
  const dialog = page.getByRole('dialog', { name: 'Rename cluster.aif' });
  await dialog.getByLabel('New file name').fill('cluster.wav');
  await dialog.getByRole('button', { name: 'Rename' }).click();
  await expect(synth.getByRole('alert')).toContainText('must end in .aif or .aiff');
});
