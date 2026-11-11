import type { Page } from '@playwright/test';

interface Entry {
  link?: string;
  builds?: { link?: string }[];
  [key: string]: unknown;
}

/** True for a listed plugin: every build links to its author's page and co-op hosts no file. */
const listed = (p: Entry): boolean => (p.builds ?? [p]).every((b) => b.link !== undefined);

/** The index without its listed plugins, so tests about hosted plugins see only those. */
export function hostedOnly<T extends { plugins: Entry[] }>(index: T): T {
  return { ...index, plugins: index.plugins.filter((p) => !listed(p)) };
}

/**
 * Serves the real plugin index without its listed plugins (hundekraft's and opus's, which co-op links
 * to). The hosted plugins' tests count, open and install those, and the listed ones would otherwise
 * put several author groups in the way. A test's own index route, registered later, wins over this
 * one, so it applies hostedOnly itself.
 */
export async function serveHostedOnly(page: Page): Promise<void> {
  await page.route('**/plugins/index.json', async (route) => {
    const response = await route.fetch();
    await route.fulfill({ response, json: hostedOnly(await response.json()) });
  });
}
