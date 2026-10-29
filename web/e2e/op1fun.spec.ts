import { test, expect, type Page, type Route } from '@playwright/test';

const CORS = { 'access-control-allow-origin': '*', 'content-type': 'application/vnd.api+json' };

const userBody = {
  data: {
    id: 'alice',
    type: 'users',
    attributes: { username: 'alice' },
    relationships: {
      patches: { data: [{ id: '1', type: 'patches' }, { id: '2', type: 'patches' }] },
      packs: { data: [{ id: '9', type: 'packs' }] },
    },
  },
};

/** Serves the op1.fun API from `routes` (path after /v1 to body); anything else is a 404. No real network. */
async function mockApi(page: Page, routes: Record<string, unknown>, seen: Route[] = []): Promise<void> {
  await page.route('https://api.op1.fun/**', async (route) => {
    seen.push(route);
    const path = new URL(route.request().url()).pathname.replace(/^\/v1/, '');
    const body = routes[path];
    if (route.request().headers()['x-user-token'] !== 'secret-token') {
      await route.fulfill({ status: 401, headers: CORS, body: JSON.stringify({ errors: [{ title: 'Unauthorized' }] }) });
      return;
    }
    if (body === undefined) {
      await route.fulfill({ status: 404, headers: CORS, body: JSON.stringify({ errors: [{ title: 'Not found' }] }) });
      return;
    }
    await route.fulfill({ status: 200, headers: CORS, body: JSON.stringify(body) });
  });
}

async function openTab(page: Page): Promise<void> {
  await page.goto('./');
  await page.getByRole('tab', { name: 'op1.fun' }).click();
}

async function fillAccount(page: Page, token = 'secret-token'): Promise<void> {
  const account = page.getByRole('form', { name: 'op1.fun account' });
  await account.getByLabel('Email').fill('me@example.com');
  await account.getByLabel('API token').fill(token);
  await account.getByLabel('op1.fun username').fill('alice');
}

const stored = (page: Page) => page.evaluate(() => localStorage.getItem('co-op.op1fun'));

test('keeps op1.fun credentials in memory unless Remember is ticked, and forgets them', async ({ page }) => {
  const seen: Route[] = [];
  await mockApi(page, { '/users/alice': userBody }, seen);
  await openTab(page);
  const account = page.getByRole('form', { name: 'op1.fun account' });
  await expect(account).toContainText('Your token is sent only to op1.fun');
  await fillAccount(page);

  const token = account.getByLabel('API token');
  await expect(token).toHaveAttribute('type', 'password');
  await account.getByRole('button', { name: 'Show token' }).click();
  await expect(token).toHaveAttribute('type', 'text');
  await account.getByRole('button', { name: 'Hide token' }).click();
  await expect(token).toHaveAttribute('type', 'password');

  await account.getByRole('button', { name: 'Test connection' }).click();
  await expect(account.getByRole('status')).toContainText('Connected: alice has 2 patch(es) and 1 pack(s). Saved for this page only.');
  const headers = seen.at(-1)?.request().headers() ?? {};
  expect(headers['x-user-email']).toBe('me@example.com');
  expect(headers['x-user-token']).toBe('secret-token');
  expect(await stored(page)).toBeNull();

  // Not remembered: a reload starts empty.
  await page.reload();
  await page.getByRole('tab', { name: 'op1.fun' }).click();
  await expect(account.getByLabel('Email')).toHaveValue('');

  await fillAccount(page);
  await account.getByLabel('Remember on this device').check();
  await account.getByRole('button', { name: 'Save' }).click();
  await expect(account.getByRole('status')).toContainText('Saved in this browser.');
  expect(JSON.parse((await stored(page)) ?? '{}')).toEqual({ email: 'me@example.com', token: 'secret-token', username: 'alice' });

  await page.reload();
  await page.getByRole('tab', { name: 'op1.fun' }).click();
  await expect(account.getByLabel('Email')).toHaveValue('me@example.com');
  await expect(account.getByLabel('Remember on this device')).toBeChecked();

  await account.getByRole('button', { name: 'Forget' }).click();
  await expect(account.getByLabel('Email')).toHaveValue('');
  await expect(account.getByLabel('API token')).toHaveValue('');
  expect(await stored(page)).toBeNull();
});

test('a rejected token asks to check the credentials', async ({ page }) => {
  await mockApi(page, { '/users/alice': userBody });
  await openTab(page);
  await fillAccount(page, 'wrong-token');
  const account = page.getByRole('form', { name: 'op1.fun account' });
  await account.getByRole('button', { name: 'Test connection' }).click();
  await expect(account.getByRole('alert')).toContainText('op1.fun rejected the email or token');
  expect(await stored(page)).toBeNull();
});
