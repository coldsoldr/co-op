import { test, expect, type Page } from '@playwright/test';

/** Makes an empty folder in the browser's private storage the one the folder picker returns. */
async function emptyFolder(page: Page): Promise<void> {
  await page.evaluate(async () => {
    const opfs = await navigator.storage.getDirectory();
    await opfs.removeEntry('mine', { recursive: true }).catch(() => {});
    const folder = await opfs.getDirectoryHandle('mine', { create: true });
    Object.defineProperty(window, 'showDirectoryPicker', { value: async () => folder, configurable: true });
  });
}

const fileExists = (page: Page, path: string): Promise<boolean> =>
  page.evaluate(async (target) => {
    const parts = target.split('/');
    const name = parts.pop() as string;
    let dir = await (await navigator.storage.getDirectory()).getDirectoryHandle('mine');
    try {
      for (const part of parts) dir = await dir.getDirectoryHandle(part);
      await dir.getFileHandle(name);
      return true;
    } catch {
      return false;
    }
  }, path);

test('copies selected community patches from the Patches tab, then skips them', async ({ page }) => {
  await page.goto('./');
  await emptyFolder(page);
  await page.getByRole('tab', { name: 'Patches' }).click();
  await page.getByRole('button', { name: 'Community' }).click();
  const panel = page.locator('#panel-patches');
  await panel.getByRole('checkbox', { name: 'Select all shown' }).check();
  await panel.getByRole('button', { name: 'Copy to My folder (3)' }).click();
  await expect(panel.getByRole('status').filter({ hasText: 'Copied 3' })).toBeVisible();
  expect(await fileExists(page, 'test-tones/synth/tone-cluster.aif')).toBe(true);
  expect(await fileExists(page, 'test-tones/drum/tone-drum.aif')).toBe(true);

  await panel.getByRole('checkbox', { name: 'Select all shown' }).check();
  await panel.getByRole('button', { name: 'Copy to My folder (3)' }).click();
  await expect(panel.getByRole('status').filter({ hasText: 'Copied 0, 3 already there' })).toBeVisible();

  await panel.getByRole('button', { name: 'My folder', exact: true }).click();
  await expect(page.locator('#panel-patches tr[data-path]')).toHaveCount(3);
});

test('copies a whole pack from its card', async ({ page }) => {
  await page.goto('./');
  await emptyFolder(page);
  await page.getByRole('tab', { name: 'Packs' }).click();
  await page.getByRole('button', { name: 'Copy Test Tones to My folder' }).click();
  await expect(page.locator('#panel-packs').getByRole('status').filter({ hasText: 'Copied 3' })).toBeVisible();
  expect(await fileExists(page, 'test-tones/synth/tone-sampler.aif')).toBe(true);
});

test('previews and copies from the pack page', async ({ page }) => {
  await page.goto('./');
  await emptyFolder(page);
  await page.getByRole('tab', { name: 'Packs' }).click();
  await page.getByRole('button', { name: 'View Test Tones' }).click();
  await page.locator('tr[data-path="test-tones/synth/tone-sampler.aif"]').getByRole('button', { name: /^Preview/ }).click();
  await expect(page.getByRole('region', { name: /^Keyboard for/ })).toBeVisible();

  await page.getByRole('checkbox', { name: 'Select synth/tone-cluster.aif' }).check();
  await page.getByRole('button', { name: 'Copy selected to My folder' }).click();
  await expect(page.locator('#panel-packs').getByRole('status').filter({ hasText: 'Copied 1' })).toBeVisible();
  expect(await fileExists(page, 'test-tones/synth/tone-cluster.aif')).toBe(true);
  expect(await fileExists(page, 'test-tones/drum/tone-drum.aif')).toBe(false);
});

test('auditions a pack patch on the OP-1 and puts the sound back', async ({ page }) => {
  // A Web MIDI port that answers like the audition plugin.
  await page.addInitScript(() => {
    let input: { onmidimessage: ((e: { data: Uint8Array }) => void) | null } | null = null;
    const reply = (value: number): void => input?.onmidimessage?.({ data: Uint8Array.of(0xbf, 119, value) });
    const port = (name: string) => ({
      name,
      state: 'connected',
      onmidimessage: null,
      open: async () => undefined,
      close: async () => undefined,
      send(bytes: Uint8Array) {
        if (bytes[0] === 0xbf && bytes[1] === 118) {
          const answers: Record<number, number> = { 1: 6, 5: 1, 0: 2 };
          const value = answers[bytes[2] as number];
          if (value !== undefined) setTimeout(() => reply(value), 0);
        }
      },
    });
    const inPort = port('OP-1');
    input = inPort;
    const access = Object.assign(new EventTarget(), { inputs: new Map([['in', inPort]]), outputs: new Map([['out', port('OP-1')]]) });
    Object.defineProperty(navigator, 'requestMIDIAccess', { value: async () => access, configurable: true });
  });
  await page.goto('./');
  await page.getByRole('tab', { name: 'Packs' }).click();
  await page.getByRole('button', { name: 'View Test Tones' }).click();
  const panel = page.locator('#panel-packs');
  await panel.locator('.audition-bar').getByRole('button', { name: 'Connect for audition' }).click();
  await expect(panel.getByRole('status').filter({ hasText: 'The audition plugin answered.' })).toBeVisible();

  const row = page.locator('tr[data-path="test-tones/synth/tone-cluster.aif"]');
  await row.locator('.kind').click();
  await expect(panel.locator('.audition-bar')).toContainText('On the OP-1:');
  await expect(row).toHaveClass(/on-op1/);

  await page.getByRole('button', { name: 'Put my sound back' }).click();
  await expect(panel.getByRole('status').filter({ hasText: 'Your own sound is back in the slot.' })).toBeVisible();
});

test('downloads a zip in a browser that cannot open a folder', async ({ page }) => {
  await page.addInitScript(() => {
    delete (window as { showDirectoryPicker?: unknown }).showDirectoryPicker;
  });
  await page.goto('./');
  await page.getByRole('tab', { name: 'Packs' }).click();
  const [download] = await Promise.all([page.waitForEvent('download'), page.getByRole('button', { name: 'Copy Test Tones to My folder' }).click()]);
  expect(download.suggestedFilename()).toBe('co-op-patches.zip');
  await expect(page.locator('#panel-packs').getByRole('status').filter({ hasText: 'Added 3 to the zip' })).toBeVisible();
});
