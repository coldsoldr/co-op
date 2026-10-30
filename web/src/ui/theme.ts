import { useCallback, useState } from 'react';

export type Theme = 'light' | 'dark';

const THEME_KEY = 'co-op.theme';

/** The theme chosen here before, if storage works and holds one. */
function storedTheme(): Theme | null {
  try {
    const value = localStorage.getItem(THEME_KEY);
    return value === 'light' || value === 'dark' ? value : null;
  } catch {
    return null;
  }
}

function systemTheme(): Theme {
  return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
}

const applyTheme = (theme: Theme): void => {
  document.documentElement.dataset.theme = theme;
};

/** Sets the theme on the page, so it paints right the first time. Called before React renders. */
export function initTheme(): void {
  applyTheme(storedTheme() ?? systemTheme());
}

/** The theme in use and a toggle that remembers the choice. */
export function useTheme(): { theme: Theme; toggle: () => void } {
  const [theme, setTheme] = useState<Theme>(() => storedTheme() ?? systemTheme());
  const toggle = useCallback(() => {
    const next: Theme = theme === 'dark' ? 'light' : 'dark';
    setTheme(next);
    applyTheme(next);
    try {
      localStorage.setItem(THEME_KEY, next);
    } catch {
      // Without storage the choice lasts until the page closes.
    }
  }, [theme]);
  return { theme, toggle };
}
