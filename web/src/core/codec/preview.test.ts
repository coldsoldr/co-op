import { describe, expect, it } from 'vitest';
import { readPatch, type Patch } from './patch';
import { hasPreview, keyPlan, VOICE_CLIP } from './preview';
import { fixtureBytes } from '../../../test/fixture-bytes';
import { sha256Hex } from '../util';

const synth = (): Patch => readPatch(fixtureBytes('synth-cluster.aif'));
const kit = (): Patch => readPatch(fixtureBytes('real/drum.aif'));

function sound(patch: Patch): Uint8Array {
  return (patch.form.chunks.find((c) => c.id === 'SSND') as { data: Uint8Array }).data.subarray(8);
}

describe('hasPreview', () => {
  it('is true for audio that is really there', async () => {
    await expect(hasPreview(synth())).resolves.toBe(true);
  });

  it('is false for the voice clip, recognised by its fingerprint', async () => {
    const patch = synth();
    const bytes = sound(patch);
    await expect(hasPreview(patch, { sha256: await sha256Hex(bytes.slice()), bytes: bytes.length })).resolves.toBe(false);
  });

  it('is false for silence and for no audio', async () => {
    const silent = synth();
    sound(silent).fill(0);
    await expect(hasPreview(silent)).resolves.toBe(false);
    const empty = synth();
    await expect(hasPreview({ ...empty, form: { ...empty.form, chunks: empty.form.chunks.filter((c) => c.id !== 'SSND') } })).resolves.toBe(false);
  });

  it('knows the real clip by size and hash', () => {
    expect(VOICE_CLIP.bytes).toBe(57_792);
    expect(VOICE_CLIP.sha256).toMatch(/^[0-9a-f]{64}$/);
  });
});

describe('keyPlan', () => {
  it('plays a kit key as its slice, unpitched', () => {
    const patch = kit();
    const meta = patch.meta as unknown as { start: number[]; end: number[] };
    const plan = keyPlan(patch);
    expect(plan.startFrame(1)).toBe(Math.floor((meta.start[1] as number) / 4058));
    expect(plan.endFrame(1)).toBe(Math.floor((meta.end[1] as number) / 4058));
    expect(plan.rate(5)).toBe(1);
  });

  it('plays a render at its pitch on middle C, and an octave up twelve keys on', () => {
    const plan = keyPlan(synth());
    expect(plan.rate(7)).toBeCloseTo(1, 3);
    expect(plan.rate(19)).toBeCloseTo(2, 3);
    expect(plan.startFrame(3)).toBe(0);
    expect(plan.endFrame(3)).toBeNull();
  });

  it('repitches a sampler from its stored root', () => {
    const patch = synth();
    (patch.meta as { base_freq?: number }).base_freq = 440;
    expect(keyPlan(patch).rate(16)).toBeCloseTo(1, 3);
  });
});
