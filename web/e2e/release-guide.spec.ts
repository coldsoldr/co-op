import { test, expect } from '@playwright/test';

test('the release guide for plugin authors has a page of its own, from the footer and from Submit a plugin', async ({ page }) => {
  await page.goto('./');
  await page.getByRole('link', { name: 'RELEASE GUIDE' }).click();
  await expect(page).toHaveURL(/#release-guide$/);
  const guide = page.getByRole('region', { name: 'Release guide for plugin authors' });
  await expect(guide.getByRole('heading', { name: 'Release guide for plugin authors' })).toBeFocused();
  for (const section of ['How co-op follows your plugin', 'What your repo needs', 'Publishing an update', 'Checklist']) {
    await expect(guide.getByRole('heading', { name: section })).toBeVisible();
  }
  await expect(guide).toContainText('Mark betas and test builds as pre-releases');
  await guide.getByRole('button', { name: 'Back' }).click();
  await expect(page.getByRole('tab', { name: 'Plugins' })).toBeVisible();

  await page.getByRole('tab', { name: 'Plugins' }).click();
  await page.getByRole('button', { name: 'Submit a plugin' }).click();
  const panel = page.getByRole('tabpanel', { name: 'hundekraft' });
  await expect(panel.getByRole('heading', { name: 'Updates' })).toBeVisible();
  await panel.getByRole('link', { name: 'release guide' }).click();
  await expect(page.getByRole('region', { name: 'Release guide for plugin authors' })).toBeVisible();
});
