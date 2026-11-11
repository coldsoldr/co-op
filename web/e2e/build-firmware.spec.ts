import { test, expect } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { crc32 } from 'node:zlib';
import { lzmaFromSource } from '../src/core/firmware/lzma';

const require = createRequire(import.meta.url);
const lzma = lzmaFromSource(readFileSync(require.resolve('lzma/src/lzma_worker.js'), 'utf8'));

/** A one-file tar (GNU header) holding `name`. */
function tarOf(name: string, data: Buffer): Buffer {
  const header = Buffer.alloc(512);
  header.write(name, 0);
  header.write('0100666\0', 100);
  header.write('0000000\0', 108);
  header.write('0000000\0', 116);
  header.write(`${data.length.toString(8).padStart(11, '0')}\0`, 124);
  header.write('14334044173\0', 136);
  header.write('        ', 148);
  header[156] = 0x30;
  header.write('ustar  \0', 257);
  let sum = 0;
  for (const b of header) sum += b;
  header.write(`${sum.toString(8).padStart(6, '0')}\0 `, 148);
  const body = Buffer.alloc(Math.ceil(data.length / 512) * 512);
  data.copy(body);
  return Buffer.concat([header, body, Buffer.alloc(10240)]);
}

/** A package in the .op1 layout whose OP1_vdk.ldr is not R246's. */
async function notStock(): Promise<Buffer> {
  const body = Buffer.from(await lzma.compress(new Uint8Array(tarOf('OP1_vdk.ldr', Buffer.from('not the stock app')))));
  const crc = Buffer.alloc(4);
  crc.writeUInt32LE(crc32(body));
  return Buffer.concat([crc, body]);
}

test('replaces the Inspector with Build firmware, which explains, shows the manual and refuses a file that is not R246', async ({ page }) => {
  await page.goto('./');
  await expect(page.getByRole('tab', { name: 'Inspector' })).toHaveCount(0);
  await page.getByRole('tab', { name: 'Build firmware' }).click();
  const panel = page.getByRole('tabpanel', { name: 'Build firmware' });
  await expect(panel.getByRole('heading', { name: 'Build coop firmware' })).toBeVisible();
  await expect(panel).toContainText('never contains te-boot.ldr');
  await expect(panel.getByRole('link', { name: 'TE’s OP-1 downloads' })).toHaveAttribute('href', 'https://teenage.engineering/downloads/op-1');

  // The manual is on the tab, under its own headings.
  const manual = panel.getByRole('region', { name: 'The coop manual' });
  await expect(manual.getByRole('heading', { name: 'Stereo everywhere' })).toBeVisible();
  await expect(manual.getByRole('heading', { name: 'The settings menu' })).toBeVisible();
  await expect(manual.getByRole('heading', { name: 'Using plugins' })).toBeVisible();

  // Not stock R246: refused before anything is patched, nothing to download.
  await panel.getByLabel('Your stock op1_246.op1').setInputFiles({ name: 'op1_246.op1', mimeType: 'application/octet-stream', buffer: await notStock() });
  await expect(panel.getByRole('alert')).toContainText('This is not the stock R246 firmware');
  await expect(panel.getByRole('button', { name: /^Download/ })).toHaveCount(0);

  // Not a firmware file at all.
  await panel.getByLabel('Your stock op1_246.op1').setInputFiles({ name: 'op1_246.op1', mimeType: 'application/octet-stream', buffer: Buffer.from('hello, this is not firmware') });
  await expect(panel.getByRole('alert')).toContainText('checksum is wrong');
});
