import js from '@eslint/js';
import tseslint from 'typescript-eslint';
import reactHooks from 'eslint-plugin-react-hooks';
import playwright from 'eslint-plugin-playwright';
import globals from 'globals';

export default tseslint.config(
  { ignores: ['**/dist/**', '**/node_modules/**', 'vendor/**'] },
  js.configs.recommended,
  ...tseslint.configs.recommended,
  {
    rules: {
      '@typescript-eslint/no-unused-vars': [
        'error',
        { argsIgnorePattern: '^_', varsIgnorePattern: '^_', caughtErrorsIgnorePattern: '^_' },
      ],
    },
  },
  {
    files: ['web/src/**/*.{ts,tsx}'],
    languageOptions: { globals: globals.browser },
  },
  {
    files: ['web/src/**/*.tsx'],
    ...reactHooks.configs.flat.recommended,
  },
  {
    files: ['web/test/**/*.ts', 'web/e2e/**/*.ts', '*.{js,ts}', 'web/*.{js,ts}'],
    languageOptions: { globals: globals.node },
  },
  {
    files: ['web/e2e/**/*.ts'],
    ...playwright.configs['flat/recommended'],
  },
);
