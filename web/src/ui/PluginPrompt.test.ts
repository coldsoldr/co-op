import { describe, expect, it } from 'vitest';
import { pluginQuestion } from './PluginPrompt';

describe('pluginQuestion', () => {
  it('asks in the singular for one patch and one plugin', () => {
    expect(pluginQuestion([{ name: 'rings' }], 1)).toBe('This patch needs the plugin rings. Install it?');
  });

  it('asks in the plural for several patches or plugins', () => {
    expect(pluginQuestion([{ name: 'rings' }], 2)).toBe('These patches need the plugin rings. Install it?');
    expect(pluginQuestion([{ name: 'rings' }, { name: 'tape drive' }], 1)).toBe(
      'This patch needs the plugins rings and tape drive. Install them?',
    );
    expect(pluginQuestion([{ name: 'a' }, { name: 'b' }, { name: 'c' }], 3)).toBe(
      'These patches need the plugins a, b and c. Install them?',
    );
  });
});
