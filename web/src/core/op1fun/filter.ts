import type { Op1FunPack, Op1FunPatch } from './types';

/** The words of a filter box, lower-cased. */
const wordsOf = (text: string): string[] => text.toLowerCase().split(/\s+/).filter(Boolean);

function matches(fields: readonly string[], words: readonly string[]): boolean {
  if (words.length === 0) return true;
  const haystack = fields.join('\n').toLowerCase();
  return words.every((word) => haystack.includes(word));
}

/**
 * The patches whose name, description, type or licence contain every word of `text`, ignoring
 * case. op1.fun has no search, so this runs over the loaded list.
 */
export function filterOp1FunPatches(patches: readonly Op1FunPatch[], text: string): Op1FunPatch[] {
  const words = wordsOf(text);
  return patches.filter((p) => matches([p.name, p.description, p.patchType, p.license], words));
}

/** The packs whose name or description contain every word of `text`, ignoring case. */
export function filterOp1FunPacks(packs: readonly Op1FunPack[], text: string): Op1FunPack[] {
  const words = wordsOf(text);
  return packs.filter((p) => matches([p.name, p.description], words));
}
