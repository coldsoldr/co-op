import { createHash } from 'node:crypto';
import { expect, test } from '@playwright/test';
import { openPlugins } from './plugin-index';

test.describe('Settings: the firmware choice', () => {
  test('choosing coop filters the Plugins tab to coop plugins and shows the OP-1 panel', async ({ page }) => {
    await page.goto('./');
    await page.getByRole('tab', { name: 'Settings' }).click();
    const coop = page.getByRole('radio', { name: /OP-1 \(coop\)/ });
    await coop.check();
    await expect(coop).toBeChecked();
    if (process.env.E2E_SHOTS) await page.screenshot({ path: `${process.env.E2E_SHOTS}/settings.png`, fullPage: true });

    await openPlugins(page);
    await expect(page.getByText('Showing plugins for OP-1 (coop), the firmware chosen in Settings.')).toBeVisible();
    await expect(page.getByRole('heading', { name: 'On your OP-1' })).toBeVisible();
    await expect(page.getByRole('button', { name: 'Read plugins from the OP-1' })).toBeVisible();
    // Filtered to coop: coldsoldr's group, opened, holds the coop plugins.
    await expect(page.getByRole('article', { name: 'saw', exact: true })).toBeVisible();
    await expect(page.getByRole('button', { name: 'Install on OP-1' }).first()).toBeVisible();
    if (process.env.E2E_SHOTS) await page.screenshot({ path: `${process.env.E2E_SHOTS}/plugins-coop.png`, fullPage: true });

    // The firmware from Settings is not a search: the author group still closes and opens.
    const author = page.getByRole('button', { name: /^coldsoldr/ });
    await expect(author).toBeEnabled();
    await author.click();
    await expect(author).toHaveAttribute('aria-expanded', 'false');
    await expect(page.locator('#panel-plugins article')).toHaveCount(0);
    await author.click();
    await expect(page.getByRole('article', { name: 'saw', exact: true })).toBeVisible();

    // The choice is remembered across a reload.
    await page.reload();
    await page.getByRole('tab', { name: 'Settings' }).click();
    await expect(page.getByRole('radio', { name: /OP-1 \(coop\)/ })).toBeChecked();
  });

  test("choosing op1r lists its plugins with op1r's patcher and a download from op1r's repo", async ({ page }) => {
    // The real index, with reedie's op1r build pointed at a stand-in file so the test needs no network.
    const body = Buffer.from('a stand-in for reedie.op1p');
    const fileUrl = 'https://raw.githubusercontent.com/pcppcp/op1r/0123456789abcdef0123456789abcdef01234567/data/plugins/reedie.op1p';
    const sha256 = createHash('sha256').update(body).digest('hex');
    await page.route('**/plugins/index.json', async (route) => {
      const response = await route.fetch();
      const index = (await response.json()) as { plugins: Array<Record<string, unknown>> };
      const reedie = index.plugins.find((p) => p.id === 'reedie');
      if (!reedie || reedie.firmware !== 'op1/op1r') throw new Error('reedie is an op1r-only listing in the index');
      Object.assign(reedie, { fileUrl, sha256, sizeBytes: body.length });
      await route.fulfill({ response, json: index });
    });
    await page.route(fileUrl, (route) => route.fulfill({ body, headers: { 'access-control-allow-origin': '*' } }));

    await page.goto('./');
    await page.getByRole('tab', { name: 'Settings' }).click();
    await page.getByRole('radio', { name: /OP-1 \(op1r\)/ }).check();
    await openPlugins(page);
    await expect(page.getByText('Showing plugins for OP-1 (op1r), the firmware chosen in Settings.')).toBeVisible();

    const card = page.getByRole('article', { name: 'Reedie', exact: true });
    await expect(card.getByRole('link', { name: /Get it from pcppcp\.github\.io/ })).toBeVisible();
    await expect(card.getByRole('link', { name: "op1r's web patcher" })).toBeVisible();
    await expect(card.getByRole('button', { name: /over USB$/ })).toHaveCount(0);
    const download = page.waitForEvent('download');
    await card.getByRole('button', { name: 'Download reedie.op1p' }).click();
    expect((await download).suggestedFilename()).toBe('reedie.op1p');

    // A plugin on hundekraft and op1r shows its op1r build here: the .op1p, not the workshop's install.
    const aether = page.getByRole('article', { name: 'Aether', exact: true });
    await expect(aether.getByRole('button', { name: /^Download aether\.op1p/ })).toBeVisible();
  });

  test('stock firmware says it runs no plugins', async ({ page }) => {
    await page.goto('./');
    await page.getByRole('tab', { name: 'Settings' }).click();
    await page.getByRole('radio', { name: /OP-1 \(stock firmware\)/ }).check();
    await page.getByRole('tab', { name: 'Plugins' }).click();
    await expect(page.getByText(/OP-1 \(stock firmware\) runs no plugins/)).toBeVisible();
  });
});
