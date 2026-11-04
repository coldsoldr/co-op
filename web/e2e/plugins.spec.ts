import { test, expect } from '@playwright/test';
import { readFileSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const seed = fileURLToPath(new URL('../../plugins/', import.meta.url));
const SEED_IDS = ['audition', 'bitcrusher', 'drive', 'ensemble', 'freq_shift', 'grids', 'marbles', 'rings', 'tape_drive', 'tape_echo', 'wavefolder'];
const seedNames = SEED_IDS.map((id) => (JSON.parse(readFileSync(`${seed}${id}/plugin.json`, 'utf8')) as { name: string }).name);

test('explains how to submit a plugin and copies the prompt', async ({ page, context }) => {
  await context.grantPermissions(['clipboard-read', 'clipboard-write']);
  await page.goto('./');
  await page.getByRole('tab', { name: 'Plugins' }).click();
  await page.getByRole('button', { name: 'Submit a plugin' }).click();
  await expect(page.getByRole('heading', { name: 'What your plugin needs' })).toBeVisible();
  await expect(page.locator('#panel-plugins article')).toHaveCount(0);
  await expect(page.getByRole('link', { name: 'Read the full guide' })).toHaveAttribute('href', /contributing-plugins\.md$/);

  await page.getByRole('button', { name: 'Copy the prompt' }).click();
  await expect(page.getByRole('status').filter({ hasText: 'Copied' })).toBeVisible();
  const copied = await page.evaluate(() => navigator.clipboard.readText());
  expect(copied).toContain('## plugin.json');
  expect(copied).toContain('## screenshot.png');
  expect(copied).toContain('## manual.md');
  expect(copied).not.toContain('````');

  await page.getByRole('button', { name: 'Back to plugins' }).click();
  await expect(page.locator('#panel-plugins article')).toHaveCount(11);
  await expect(page.getByRole('button', { name: 'Submit a plugin' })).toBeFocused();
});

test('groups plugins by author, closed with several authors and open for a search', async ({ page }) => {
  await page.route('**/plugins/index.json', async (route) => {
    const response = await route.fetch();
    const index = (await response.json()) as { plugins: Array<{ id: string; author: string }> };
    for (const plugin of index.plugins) if (plugin.id === 'rings') plugin.author = 'someone else';
    await route.fulfill({ response, json: index });
  });
  await page.goto('./');
  await page.getByRole('tab', { name: 'Plugins' }).click();

  const someone = page.getByRole('button', { name: /^someone else/ });
  await expect(page.getByRole('button', { name: /^coldsoldr/ })).toContainText('10 plugins');
  await expect(someone).toHaveAttribute('aria-expanded', 'false');
  await expect(page.locator('#panel-plugins article')).toHaveCount(0);

  await someone.click();
  await expect(page.locator('#panel-plugins article')).toHaveCount(1);

  // Back from a manual, the group is as it was left.
  await page.getByRole('button', { name: 'Read the manual of rings' }).click();
  await page.getByRole('button', { name: 'Back to plugins' }).click();
  await expect(page.locator('#panel-plugins article')).toHaveCount(1);

  await page.getByRole('searchbox', { name: 'Search plugins' }).fill('tape');
  await expect(page.locator('#panel-plugins article')).toHaveCount(2);
  await expect(someone).toBeHidden();
});

test('leaves a lone author open, with its buttons apart', async ({ page }) => {
  await page.goto('./');
  await page.getByRole('tab', { name: 'Plugins' }).click();
  const rings = page.getByRole('article', { name: 'rings', exact: true });
  await expect(rings.locator('.plugin-buttons')).toHaveCSS('gap', '8px');

  await page.getByRole('button', { name: /^coldsoldr/ }).click();
  await expect(page.locator('#panel-plugins article')).toHaveCount(0);
});

test('chooses a device from the logo, remembers it and keeps the OP-1 page', async ({ page }) => {
  await page.goto('./');
  const logo = page.getByRole('button', { name: /Choose a device/ });
  await logo.click();
  await expect(page.getByRole('button', { name: 'OP-1', exact: true })).toHaveAttribute('aria-pressed', 'true');
  await expect(page.getByRole('button', { name: 'OP-1F (coming soon)' })).toBeDisabled();

  await page.getByRole('button', { name: 'OP-XY' }).click();
  await expect(page.getByRole('heading', { name: 'Connect OP–XY' })).toBeVisible();
  await expect(page.getByRole('tab', { name: 'Library' })).toBeHidden();
  await expect(page.getByRole('note').filter({ hasText: 'Design preview' })).toBeVisible();
  await expect(page.getByRole('contentinfo')).toContainText('OP–XY');

  await page.getByRole('button', { name: 'Find OP–XY' }).click();
  await expect(page.getByRole('button', { name: 'Play glass harp' })).toBeVisible();
  await page.getByRole('button', { name: /samples/ }).click();
  await expect(page.getByRole('button', { name: 'Play recording 014' })).toBeVisible();
  await page.getByRole('button', { name: 'Add a samples folder' }).click();
  await page.getByLabel('New samples folder name').fill('loops');
  await page.getByRole('button', { name: 'Add folder' }).click();
  await expect(page.getByText('Nothing in this folder yet.')).toBeVisible();

  await page.reload();
  await expect(page.getByRole('heading', { name: 'Connect OP–XY' })).toBeVisible();
  await page.getByRole('button', { name: /Choose a device/ }).click();
  await page.getByRole('button', { name: 'OP-1', exact: true }).click();
  await expect(page.getByRole('tab', { name: 'Library' })).toBeVisible();
  await expect(page.getByRole('heading', { name: 'Connect OP–XY' })).toBeHidden();
});

test('browses a local folder with nothing plugged in, and remembers it', async ({ page }) => {
  const fixtures = fileURLToPath(new URL('../test/fixtures/', import.meta.url));
  const files = {
    'my patches/pads/glass.aif': readFileSync(`${fixtures}synth-cluster.aif`).toString('base64'),
    'my patches/kits/boom.aif': readFileSync(`${fixtures}drum-boombap.aif`).toString('base64'),
    'my patches/notes.txt': 'bm90ZXM=',
  };
  await page.goto('./');
  await page.evaluate(async (entries) => {
    const opfs = await navigator.storage.getDirectory();
    await opfs.removeEntry('my patches', { recursive: true }).catch(() => {});
    for (const [path, base64] of Object.entries(entries)) {
      const parts = path.split('/');
      const name = parts.pop() as string;
      let dir = opfs;
      for (const part of parts) dir = await dir.getDirectoryHandle(part, { create: true });
      const writable = await (await dir.getFileHandle(name, { create: true })).createWritable();
      await writable.write(Uint8Array.from(atob(base64), (c) => c.charCodeAt(0)));
      await writable.close();
    }
    const folder = await opfs.getDirectoryHandle('my patches');
    Object.defineProperty(window, 'showDirectoryPicker', { value: async () => folder, configurable: true });
  }, files);

  await page.getByRole('tab', { name: 'Patches' }).click();
  await page.getByRole('button', { name: 'Choose a folder' }).click();
  const rows = page.locator('#panel-patches tr[data-path]');
  await expect(rows).toHaveCount(2);
  await expect(page.locator('#panel-patches').getByRole('status')).toHaveText('2 patch file(s) in my patches');
  await expect(page.locator('tr[data-path="pads/glass.aif"] .sound')).toContainText('cluster');

  await page.getByRole('searchbox', { name: 'Search patches' }).fill('cluster');
  await expect(rows).toHaveCount(1);
  await page.getByRole('searchbox', { name: 'Search patches' }).fill('');
  await page.locator('#panel-patches').getByLabel('Kind').selectOption('drum');
  await expect(rows).toHaveCount(1);
  await expect(rows.first()).toHaveAttribute('data-path', 'kits/boom.aif');

  await page.reload();
  await page.getByRole('tab', { name: 'Patches' }).click();
  await expect(page.locator('#panel-patches tr[data-path]')).toHaveCount(2);

  await page.getByRole('button', { name: 'Forget folder' }).click();
  await expect(page.getByRole('button', { name: 'Choose a folder' })).toBeVisible();
  await page.reload();
  await page.getByRole('tab', { name: 'Patches' }).click();
  await expect(page.getByRole('button', { name: 'Choose a folder' })).toBeVisible();
  await expect(page.locator('#panel-patches tr[data-path]')).toHaveCount(0);
});

test('plays community patches and opens a keyboard, with nothing plugged in', async ({ page }) => {
  await page.goto('./');
  await page.getByRole('tab', { name: 'Patches' }).click();
  await page.getByRole('button', { name: 'Community' }).click();
  const rows = page.locator('#panel-patches tr[data-path]');
  await expect(rows).toHaveCount(3);
  await expect(page.locator('#panel-patches').getByRole('status')).toHaveText('3 patch file(s) in community packs');

  await rows.first().getByRole('button', { name: /^Preview/ }).click();
  const keyboard = page.getByRole('region', { name: /^Keyboard for/ });
  await expect(keyboard).toBeVisible();
  await keyboard.getByRole('button', { name: 'Key 8' }).dispatchEvent('pointerdown');
  await keyboard.getByRole('button', { name: 'Key 8' }).dispatchEvent('pointerup');
  await page.keyboard.press('a');
  await keyboard.getByRole('button', { name: 'Close keyboard' }).click();
  await expect(keyboard).toBeHidden();

  await page.locator('#panel-patches').getByLabel('Kind').selectOption('drum');
  await expect(rows.first()).toBeVisible();
  await page.getByRole('button', { name: 'My folder', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Choose a folder' })).toBeVisible();
});

test('sends ticked local patches to the OP-1 from the Library', async ({ page }) => {
  const fixtures = fileURLToPath(new URL('../test/fixtures/', import.meta.url));
  const files = {
    'drive/synth/user/keep.aif': readFileSync(`${fixtures}synth-cluster.aif`).toString('base64'),
    'drive/synth/pads/old.aif': readFileSync(`${fixtures}synth-cluster.aif`).toString('base64'),
    'drive/drum/kits/kit.aif': readFileSync(`${fixtures}drum-boombap.aif`).toString('base64'),
    'mine/pads/one.aif': readFileSync(`${fixtures}synth-cluster.aif`).toString('base64'),
    'mine/pads/two.aif': readFileSync(`${fixtures}drum-boombap.aif`).toString('base64'),
  };
  await page.goto('./');
  await page.evaluate(async (entries) => {
    const opfs = await navigator.storage.getDirectory();
    for (const name of ['drive', 'mine']) await opfs.removeEntry(name, { recursive: true }).catch(() => {});
    for (const [path, base64] of Object.entries(entries)) {
      const parts = path.split('/');
      const name = parts.pop() as string;
      let dir = opfs;
      for (const part of parts) dir = await dir.getDirectoryHandle(part, { create: true });
      const writable = await (await dir.getFileHandle(name, { create: true })).createWritable();
      await writable.write(Uint8Array.from(atob(base64), (c) => c.charCodeAt(0)));
      await writable.close();
    }
    await (await opfs.getDirectoryHandle('drive')).getDirectoryHandle('tape', { create: true });
    const picks = [await opfs.getDirectoryHandle('mine'), await opfs.getDirectoryHandle('drive')];
    Object.defineProperty(window, 'showDirectoryPicker', { value: async () => picks.shift(), configurable: true });
  }, files);

  await page.getByRole('tab', { name: 'Patches' }).click();
  await page.getByRole('button', { name: 'Choose a folder' }).click();
  await expect(page.locator('#panel-patches tr[data-path]')).toHaveCount(2);
  await page.getByRole('tab', { name: 'Library' }).click();
  await page.getByRole('button', { name: 'Connect OP-1' }).click();

  const folders = page.getByRole('complementary', { name: 'Folders' });
  await folders.getByRole('region', { name: 'local folders' }).getByRole('button', { name: /^pads/ }).click();
  const send = page.getByRole('button', { name: 'Send to OP-1 (2)' });
  await page.getByRole('checkbox', { name: 'Select all local patches here' }).check();
  await send.click();
  const dialog = page.getByRole('dialog', { name: 'Send 2 patches to the OP-1' });
  await expect(dialog.getByLabel('synth patches go to')).toHaveValue('pads');
  await expect(dialog.getByLabel('drum patches go to')).toHaveValue('kits');
  await dialog.getByRole('button', { name: 'Send', exact: true }).click();
  await expect(page.getByRole('status').filter({ hasText: 'Sent 2 patches to the OP-1.' })).toBeVisible();

  const sent = await page.evaluate(async () => {
    const drive = await (await navigator.storage.getDirectory()).getDirectoryHandle('drive');
    const names = async (dir: FileSystemDirectoryHandle): Promise<string[]> => {
      const out: string[] = [];
      for await (const name of (dir as unknown as { keys(): AsyncIterable<string> }).keys()) out.push(name);
      return out.sort();
    };
    const synth = await drive.getDirectoryHandle('synth');
    const drum = await drive.getDirectoryHandle('drum');
    return { pads: await names(await synth.getDirectoryHandle('pads')), kits: await names(await drum.getDirectoryHandle('kits')) };
  });
  expect(sent).toEqual({ pads: ['old.aif', 'one.aif'], kits: ['kit.aif', 'two.aif'] });
});

test('auditions a synth patch on the OP-1 and puts the sound back', async ({ page }) => {
  // A Web MIDI port that answers like the audition plugin and keeps what it was sent.
  await page.addInitScript(() => {
    const state = { sent: [] as number[][] };
    (window as unknown as { midiLog: typeof state }).midiLog = state;
    let input: { onmidimessage: ((e: { data: Uint8Array }) => void) | null } | null = null;
    const reply = (value: number): void => input?.onmidimessage?.({ data: Uint8Array.of(0xbf, 119, value) });
    const port = (name: string) => ({
      name,
      state: 'connected',
      onmidimessage: null,
      open: async () => undefined,
      close: async () => undefined,
      send(bytes: Uint8Array) {
        state.sent.push([...bytes]);
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
  await page.getByRole('tab', { name: 'Patches' }).click();
  await page.getByRole('button', { name: 'Community' }).click();
  const rows = page.locator('#panel-patches tr[data-path]');
  await expect(rows).toHaveCount(3);
  await page.locator('#panel-patches').getByLabel('Kind').selectOption('synth');
  const row = rows.first();
  await expect(row.getByRole('button', { name: /^Preview/ })).toBeVisible();
  await expect(row.getByRole('button', { name: /^Audition/ })).toBeHidden();

  await page.locator('#panel-patches .audition-bar').getByRole('button', { name: 'Connect for audition' }).click();
  await expect(page.locator('#panel-patches').getByRole('status').filter({ hasText: 'The audition plugin answered.' })).toBeVisible();
  await row.locator('.kind').click();
  await expect(page.locator('#panel-patches .audition-bar')).toContainText('On the OP-1:');
  await expect(row).toHaveClass(/on-op1/);
  const sent = await page.evaluate(() => (window as unknown as { midiLog: { sent: number[][] } }).midiLog.sent);
  expect(sent.some((m) => m[0] === 0xae)).toBe(true);
  await expect(page.getByRole('region', { name: /^Keyboard for/ })).toBeHidden();

  await page.getByRole('button', { name: 'Put my sound back' }).click();
  await expect(page.locator('#panel-patches').getByRole('status').filter({ hasText: 'Your own sound is back in the slot.' })).toBeVisible();
  await expect(page.locator('#panel-patches .audition-bar')).toContainText('OP-1 connected');
});

test('lists the seed plugins and filters them', async ({ page }) => {
  await page.goto('./');
  await page.getByRole('tab', { name: 'Plugins' }).click();
  await expect(page.getByRole('status').filter({ hasText: '11 plugins' })).toBeVisible();
  await expect(page.locator('#panel-plugins article')).toHaveCount(11);
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
  await expect(page.getByRole('status').filter({ hasText: '1 of 11 plugins' })).toBeVisible();
  await page.getByLabel('Kind').selectOption('feature');
  await expect(page.locator('#panel-plugins article')).toHaveCount(3);
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
