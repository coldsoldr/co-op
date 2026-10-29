import { test, expect, type Page } from '@playwright/test';
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const fixtures = fileURLToPath(new URL('../test/fixtures/', import.meta.url));

const INVENTORY = [
  ['kennel-inventory 1', 'core 01e00000 0003a000 1a2b3c4d', 'presets 00042 cap 00100'].join('\t'),
  ['synth', 'rings', 'knl', 'plugins/rings.knl', '1.0', 'coldsoldr', '111768', '0badf00d', 'loaded', '+engine:rings', ''].join('\t'),
  ['fx', 'tape_echo', 'knl', 'plugins/tape_echo.knl', '1.1', 'coldsoldr', '53844', '12345678', 'off', '', 'switched off'].join('\t'),
  ['fx', 'delay', 'builtin', '', '', '', '0', '00000000', 'loaded', '', ''].join('\t'),
].join('\n');

interface StubOptions {
  /** False for firmware that shows the port but never answers HELLO. */
  answer: boolean;
  /** Drop the OP-1's ports when it is asked for the inventory. */
  disconnectOnList?: boolean;
  /** Show no OP-1 port at all, as in disk mode. */
  noPorts?: boolean;
}

/**
 * Replaces Web MIDI in the page with a fake OP-1 that answers HELLO, LIST and OFF_GET the way
 * hundekraft 3.x does (see docs/research/2026-10-29-hundekraft-usb-and-plugins.md).
 */
async function stubOp1(page: Page, options: StubOptions): Promise<void> {
  await page.addInitScript(
    ({ answer, disconnectOnList, noPorts, inventory, off }) => {
      const HEADER = [0xf0, 0x00, 0x20, 0x76, 0x7d];
      const pack7 = (raw: number[]): number[] => {
        const out: number[] = [];
        for (let i = 0; i < raw.length; i += 7) {
          const group = raw.slice(i, i + 7);
          out.push(group.reduce((high, b, k) => high | ((b >> 7) << k), 0), ...group.map((b) => b & 0x7f));
        }
        return out;
      };
      const unpack7 = (packed: number[]): number[] => {
        const out: number[] = [];
        for (let i = 0; i < packed.length; ) {
          const high = packed[i++] as number;
          for (let k = 0; k < 7 && i < packed.length; k++) out.push((packed[i++] as number) | (((high >> k) & 1) << 7));
        }
        return out;
      };
      const u32 = (v: number): number[] => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
      const pageOf = (text: number[], offset: number): number[] =>
        offset > text.length ? [5] : [0, ...u32(text.length), ...text.slice(offset, offset + 64)];
      const listText = [...new TextEncoder().encode(inventory)];
      const offText = [...new TextEncoder().encode(off)];

      const access = new EventTarget() as EventTarget & { inputs: Map<string, unknown>; outputs: Map<string, unknown> };
      const input = { name: 'OP-1 Midi Device', state: 'connected', onmidimessage: null as null | ((e: unknown) => void), open: () => Promise.resolve(), close: () => Promise.resolve() };
      const output = {
        name: 'OP-1 Midi Device',
        state: 'connected',
        open: () => Promise.resolve(),
        close: () => Promise.resolve(),
        send(data: Uint8Array) {
          const bytes = [...data];
          const cmd = bytes[5] as number;
          const payload = unpack7(bytes.slice(8, -1));
          if (cmd === 7 && disconnectOnList) {
            input.state = 'disconnected';
            output.state = 'disconnected';
            access.dispatchEvent(new Event('statechange'));
            return;
          }
          if (!answer) return;
          const body =
            cmd === 1 ? [0, ...u32(1), 3]
            : cmd === 7 ? pageOf(listText, (payload[0] ?? 0) | ((payload[1] ?? 0) << 8))
            : cmd === 8 ? pageOf(offText, (payload[1] ?? 0) | ((payload[2] ?? 0) << 8))
            : [1];
          const reply = [...HEADER, cmd | 0x40, bytes[6] as number, bytes[7] as number, ...pack7(body), 0xf7];
          // A clock byte inside the SysEx, as a real OP-1 may send.
          reply.splice(3, 0, 0xf8);
          setTimeout(() => input.onmidimessage?.({ data: Uint8Array.from(reply) }), 1);
        },
      };
      access.inputs = new Map(noPorts ? [] : [['in', input]]);
      access.outputs = new Map(noPorts ? [] : [['out', output]]);
      Object.defineProperty(navigator, 'requestMIDIAccess', {
        configurable: true,
        value: () => Promise.resolve(access),
      });
    },
    { ...options, disconnectOnList: options.disconnectOnList ?? false, noPorts: options.noPorts ?? false, inventory: INVENTORY, off: 'tape_echo' },
  );
}

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
  await stubOp1(page, { answer: true });
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
  await stubOp1(page, { answer: true });
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
  await stubOp1(page, { answer: true });
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
  await stubOp1(page, { answer: false });
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
  await stubOp1(page, { answer: true, disconnectOnList: true });
  await page.goto('./');
  await page.getByRole('button', { name: 'Check over USB' }).click();
  const dialog = panel(page);
  await dialog.getByRole('button', { name: 'Check', exact: true }).click();
  await expect(dialog.getByRole('alert')).toContainText('The OP-1 was disconnected during the check.');
  await expect(dialog.getByRole('button', { name: 'Check again' })).toBeEnabled();
});

test('points out disk mode when there is no OP-1 MIDI port', async ({ page }) => {
  await stubOp1(page, { answer: true, noPorts: true });
  await page.goto('./');
  await page.getByRole('button', { name: 'Check over USB' }).click();
  const dialog = panel(page);
  await dialog.getByRole('button', { name: 'Check', exact: true }).click();
  await expect(dialog.getByRole('alert')).toContainText('not in disk mode');
});
