import { test, expect } from '@playwright/test';

test('shows the site version in the footer', async ({ page }) => {
  await page.goto('./');
  await expect(page.getByRole('contentinfo')).toContainText(/V\d+\.\d+\.\d+/);
});

test('opens the changelog from the footer, newest first, and has a link of its own', async ({ page }) => {
  await page.goto('./');
  await page.getByRole('link', { name: 'CHANGELOG' }).click();
  await expect(page.getByRole('heading', { name: 'Changelog' })).toBeVisible();
  await expect(page.getByRole('tab', { name: 'Library' })).toBeHidden();
  const releases = page.locator('.release summary h3');
  await expect(releases.first()).toHaveText('V0.9.0');
  await expect(page.locator('.release').first()).toHaveAttribute('open', '');
  await expect(page.locator('.release').nth(1)).not.toHaveAttribute('open', '');
  await page.getByText('V0.8.0', { exact: true }).click();
  await expect(page.getByRole('heading', { name: 'New Patches tab' })).toBeVisible();

  await page.reload();
  await expect(page.getByRole('heading', { name: 'Changelog' })).toBeVisible();
  await page.getByRole('button', { name: 'Back' }).click();
  await expect(page.getByRole('tab', { name: 'Library' })).toBeVisible();
  await expect(page.getByRole('heading', { name: 'Changelog' })).toBeHidden();
});

// The e2e build has no Turnstile site key, so the dialog offers the prefilled GitHub issue instead.
test('feedback falls back to a prefilled GitHub issue', async ({ page }) => {
  await page.goto('./');
  await page.getByRole('button', { name: 'Feedback' }).click();
  const dialog = page.getByRole('dialog', { name: 'Send feedback' });
  await expect(dialog).toBeVisible();

  const open = dialog.getByRole('button', { name: 'Open GitHub issue' });
  await expect(open).toBeDisabled();

  await dialog.getByLabel('Category').selectOption('idea');
  const message = 'Please add a sort by date & time option #3';
  await dialog.getByLabel('Message').fill(message);
  await expect(dialog.getByText(`${message.length} / 5000 characters`)).toBeVisible();
  // Contact is only asked for when feedback goes through the Worker.
  await expect(dialog.getByLabel(/Contact/)).toHaveCount(0);

  await dialog.getByText('What is attached').click();
  await expect(dialog.getByRole('row', { name: /Store none/ })).toBeVisible();
  await expect(dialog.getByRole('row', { name: /Device mounted no/ })).toBeVisible();

  const link = dialog.getByRole('link', { name: 'Open GitHub issue' });
  const href = await link.getAttribute('href');
  expect(href).toMatch(/^https:\/\/github\.com\/coldsoldr\/co-op\/issues\/new\?labels=feedback,idea&title=/);
  expect(href).toContain(encodeURIComponent('Please add a sort by date &amp; time option'));
  const body = new URL(href ?? '').searchParams.get('body') ?? '';
  expect(body).toContain('> Please add a sort by date &amp; time option #\u200B3');
  // Turnstile is never loaded without a Worker.
  await expect(page.locator('script[src*="challenges.cloudflare.com"]')).toHaveCount(0);

  await dialog.getByRole('button', { name: 'Cancel' }).click();
  await expect(dialog).toBeHidden();
  await expect(page.getByRole('button', { name: 'Feedback' })).toBeFocused();
});
