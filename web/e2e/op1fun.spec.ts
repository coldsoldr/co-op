import { test, expect, type Page, type Route } from '@playwright/test';
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const fixtures = fileURLToPath(new URL('../test/fixtures/', import.meta.url));
const S3 = 'https://op1fun.s3.amazonaws.com/patches';

let parent: string;
let drive: string;

test.beforeEach(() => {
  parent = mkdtempSync(join(tmpdir(), 'co-op-e2e-op1fun-'));
  drive = join(parent, 'OP-1');
  mkdirSync(join(drive, 'synth', 'user'), { recursive: true });
  mkdirSync(join(drive, 'drum', 'user'), { recursive: true });
  mkdirSync(join(drive, 'tape'), { recursive: true });
  copyFileSync(join(fixtures, 'synth-cluster.aif'), join(drive, 'synth', 'user', 'cluster.aif'));
});

test.afterEach(() => {
  rmSync(parent, { recursive: true, force: true });
});

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

const patchResource = (id: string, name: string, type: string, file: string) => ({
  id,
  type: 'patches',
  attributes: { name, description: `${name} from op1.fun`, license: 'CC BY', public: true, 'patch-type': type, 'user-id': 'alice' },
  links: { self: `https://op1.fun/users/alice/patches/${id}`, file: `${S3}/${file}`, preview: `${S3}/${id}.mp3` },
});

const patchesBody = {
  data: [patchResource('1', 'Boom Kit', 'drum', 'boombap.aif'), patchResource('2', 'Cluster Pad', 'synth', 'cluster.aif')],
};
const packsBody = {
  data: [
    {
      id: '9',
      type: 'packs',
      attributes: { name: 'Starter', description: 'two patches', public: true, 'user-id': 'alice' },
      relationships: { patches: { data: [{ id: '2', type: 'patches' }, { id: '3', type: 'patches' }] } },
    },
  ],
};
const packBody = {
  data: packsBody.data[0],
  included: [patchResource('2', 'Cluster Pad', 'synth', 'cluster.aif'), patchResource('3', 'Gone', 'synth', 'missing.aif')],
};

/** Serves fixture AIFFs for the S3 file links; any other file is a 404. */
async function mockFiles(page: Page): Promise<void> {
  const files: Record<string, Buffer> = {
    'boombap.aif': readFileSync(join(fixtures, 'drum-boombap.aif')),
    'cluster.aif': readFileSync(join(fixtures, 'synth-cluster.aif')),
  };
  await page.route(`${S3}/**`, async (route) => {
    const name = new URL(route.request().url()).pathname.split('/').pop() ?? '';
    const body = files[name];
    // File links never get the op1.fun credentials.
    expect(route.request().headers()['x-user-token']).toBeUndefined();
    if (!body) {
      await route.fulfill({ status: 404, headers: { 'access-control-allow-origin': '*' }, body: 'missing' });
      return;
    }
    await route.fulfill({ status: 200, headers: { 'access-control-allow-origin': '*', 'content-type': 'audio/aiff' }, body });
  });
}

async function signIn(page: Page): Promise<void> {
  await mockApi(page, { '/users/alice': userBody, '/users/alice/patches': patchesBody, '/users/alice/packs': packsBody, '/users/alice/packs/9': packBody });
  await mockFiles(page);
  await page.goto('./');
  await page.getByLabel('Choose the OP-1 drive folder').setInputFiles(drive);
  await expect(page.locator('tbody tr[data-path]')).toHaveCount(1);
  await page.getByRole('tab', { name: 'op1.fun' }).click();
  await fillAccount(page);
  await page.getByRole('form', { name: 'op1.fun account' }).getByRole('button', { name: 'Save' }).click();
  await page.getByRole('button', { name: 'Load' }).click();
}

test('browses op1.fun patches and installs one into a dropped drive', async ({ page }) => {
  await signIn(page);
  const table = page.getByRole('table', { name: "alice's patches" });
  await expect(table.getByRole('row')).toHaveCount(3);
  await expect(table).toContainText('Boom Kit');
  await expect(table).toContainText('CC BY');
  await expect(table.getByLabel('Preview Boom Kit')).toHaveAttribute('src', `${S3}/1.mp3`);

  await page.getByRole('searchbox', { name: 'Filter' }).fill('synth');
  await expect(table.getByRole('row')).toHaveCount(2);
  await page.getByRole('searchbox', { name: 'Filter' }).fill('');

  await table.getByRole('button', { name: 'Install Boom Kit' }).click();
  await expect(page.getByRole('status').filter({ hasText: 'Installed 1 patch.' })).toContainText('drum/op1fun/Boom Kit.aif');

  await page.getByRole('tab', { name: 'Library' }).click();
  await expect(page.getByRole('button', { name: 'Download changes (1)' })).toBeEnabled();
  await expect(page.locator('tr[data-path="drum/op1fun/Boom Kit.aif"]')).toBeVisible();
});

test('installs an op1.fun pack patch by patch and reports a failed file', async ({ page }) => {
  await signIn(page);
  await page.getByRole('radio', { name: /Packs/ }).check();
  await page.getByRole('button', { name: 'View pack Starter' }).click();
  const table = page.getByRole('table', { name: 'Patches in Starter' });
  await expect(table.getByRole('row')).toHaveCount(3);
  await page.getByRole('button', { name: 'Install all (2)' }).click();
  await expect(page.getByRole('status').filter({ hasText: 'Installed 1 patch.' })).toContainText('synth/op1fun/Cluster Pad.aif');
  await expect(page.getByRole('alert')).toContainText('Gone: download failed: HTTP 404');

  await page.getByRole('tab', { name: 'Library' }).click();
  await expect(page.getByRole('button', { name: 'Download changes (1)' })).toBeEnabled();
});
