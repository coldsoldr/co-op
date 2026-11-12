/** The pages shown in place of the app, each at an address of its own. */
export const CHANGELOG_HASH = '#changelog';
export const RELEASE_GUIDE_HASH = '#release-guide';

export type Page = 'changelog' | 'release-guide';

export function pageOf(hash: string): Page | null {
  return hash === CHANGELOG_HASH ? 'changelog' : hash === RELEASE_GUIDE_HASH ? 'release-guide' : null;
}
