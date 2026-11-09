import { expect, test } from '@playwright/test';

test.describe('Settings: the firmware choice', () => {
  test('choosing coop filters the Plugins tab to coop plugins and shows the OP-1 panel', async ({ page }) => {
    await page.goto('./');
    await page.getByRole('tab', { name: 'Settings' }).click();
    const coop = page.getByRole('radio', { name: /OP-1 \(coop\)/ });
    await coop.check();
    await expect(coop).toBeChecked();
    if (process.env.E2E_SHOTS) await page.screenshot({ path: `${process.env.E2E_SHOTS}/settings.png`, fullPage: true });

    await page.getByRole('tab', { name: 'Plugins' }).click();
    await expect(page.getByText('Showing plugins for OP-1 (coop), the firmware chosen in Settings.')).toBeVisible();
    await expect(page.getByRole('heading', { name: 'On your OP-1' })).toBeVisible();
    await expect(page.getByRole('button', { name: 'Read plugins from the OP-1' })).toBeVisible();
    // Filtered to coop, the author groups are open.
    await expect(page.getByRole('article', { name: 'saw', exact: true })).toBeVisible();
    await expect(page.getByRole('button', { name: 'Install on OP-1' }).first()).toBeVisible();
    if (process.env.E2E_SHOTS) await page.screenshot({ path: `${process.env.E2E_SHOTS}/plugins-coop.png`, fullPage: true });

    // The choice is remembered across a reload.
    await page.reload();
    await page.getByRole('tab', { name: 'Settings' }).click();
    await expect(page.getByRole('radio', { name: /OP-1 \(coop\)/ })).toBeChecked();
  });

  test('stock firmware says it runs no plugins', async ({ page }) => {
    await page.goto('./');
    await page.getByRole('tab', { name: 'Settings' }).click();
    await page.getByRole('radio', { name: /OP-1 \(stock firmware\)/ }).check();
    await page.getByRole('tab', { name: 'Plugins' }).click();
    await expect(page.getByText(/OP-1 \(stock firmware\) runs no plugins/)).toBeVisible();
  });
});
