import { describe, it, expect } from 'vitest';
import { PLUGIN_PROMPT, extractPrompt } from './submission';

describe('extractPrompt', () => {
  it('takes the text fence and leaves the rest of the document', () => {
    expect(extractPrompt('intro\n\n````text\nline 1\nline 2\n````\n\n````markdown\nno\n````\n')).toBe('line 1\nline 2');
  });

  it('refuses a document without a prompt', () => {
    expect(() => extractPrompt('# nothing here')).toThrow('no prompt');
  });
});

describe('PLUGIN_PROMPT', () => {
  it('covers every required plugin.json field and the plugin folder', () => {
    for (const field of ['format', 'id', 'name', 'firmware', 'kind', 'author', 'version', 'description', 'license', 'file', 'screenshot', 'manual']) {
      expect(PLUGIN_PROMPT).toContain(`"${field}"`);
    }
    expect(PLUGIN_PROMPT).toContain('960 by 480');
    expect(PLUGIN_PROMPT).toContain('## Credits and licence');
  });
});
