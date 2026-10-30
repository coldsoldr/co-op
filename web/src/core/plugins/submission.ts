import promptDoc from '../../../../docs/plugin-prompt.md?raw';

/** The text inside the document's ````text fence, so the site and the docs share one prompt. */
export function extractPrompt(markdown: string): string {
  const prompt = /^````text\r?\n([\s\S]*?)\r?\n````$/m.exec(markdown)?.[1];
  if (prompt === undefined) throw new Error('no prompt in docs/plugin-prompt.md');
  return prompt;
}

export const PLUGIN_PROMPT = extractPrompt(promptDoc);
