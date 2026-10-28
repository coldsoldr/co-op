import { describe, it, expect } from 'vitest';
import { patchFileNameProblem } from '@co-op/shared/packs';
import { assertPatchFileName } from './operations';

/**
 * shared must not import web code, so it carries its own copy of the FAT-safe file name rule.
 * This table keeps the two copies in step.
 */
const NAMES = [
  'kick.aif',
  'Kick.AIFF',
  'pad (1).aif',
  'déjà vu.aif',
  'a.aif',
  '',
  '.aif',
  '.hidden.aif',
  'kick.wav',
  'kick',
  'kick.aif.',
  'kick.aif ',
  'a/b.aif',
  'a\\b.aif',
  'a:b.aif',
  'a*b.aif',
  'a?b.aif',
  'a"b.aif',
  'a<b.aif',
  'a>b.aif',
  'a|b.aif',
  'tab\there.aif',
  'del\u007f.aif',
  'nul\u0000.aif',
  'aiff',
  'x.aiff',
  'x.aifc',
  `${'a'.repeat(60)}.aif`,
  `${'a'.repeat(61)}.aif`,
  `${'a'.repeat(59)}.aiff`,
  `${'a'.repeat(60)}.aiff`,
];

const opsAccepts = (name: string): boolean => {
  try {
    assertPatchFileName(name);
    return true;
  } catch {
    return false;
  }
};

describe('FAT-safe file name rules', () => {
  it('agree between operations.ts and shared/packs.ts', () => {
    for (const name of NAMES) {
      expect({ name, accepted: patchFileNameProblem(name) === null }).toEqual({ name, accepted: opsAccepts(name) });
    }
  });

  it('the table covers both outcomes', () => {
    expect(NAMES.some(opsAccepts)).toBe(true);
    expect(NAMES.some((n) => !opsAccepts(n))).toBe(true);
  });
});
