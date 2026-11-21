import { describe, expect, it } from 'vitest';
import type { HostedPluginEntry, PluginIndex, PluginIndexEntry } from '@co-op/shared/plugins';
import { FakeCoop, type CoopBootRecord } from '../../../test/fake-coop';
import type { KnownCoopFile } from '../plugins/memory';
import { updateCoopPlugins, updateSummary, type UpdateOptions, type UpdateOutcome } from './coop-update';
import { createSession } from './session';

const plugin = (id: string, version: string, more: Partial<PluginIndexEntry> = {}): PluginIndexEntry => ({
  id,
  name: id.replaceAll('_', ' '),
  firmware: 'op1/coop',
  kind: 'fx',
  author: 'coldsoldr',
  version,
  description: 'An effect.',
  file: `${id}.coop`,
  screenshot: `${id}/screenshot.png`,
  manual: `${id}/manual.md`,
  requires: 'coop e10',
  tags: [],
  sizeBytes: 200,
  sha256: 'a'.repeat(64),
  ...more,
});
const index: PluginIndex = {
  format: 1,
  generatedAt: '2026-11-21T00:00:00.000Z',
  plugins: [plugin('bitcrusher', '0.3'), plugin('fickle', '0.3'), plugin('rings', '0.10', { kind: 'synth' }), plugin('saw', '0.2', { kind: 'synth' }), plugin('tape_echo', '0.4')],
};

/** The new build of a plugin: 200 bytes that differ by plugin, four chunks on the wire. */
const newBytes = (id: string): Uint8Array => Uint8Array.from({ length: 200 }, (_, i) => (i * 7 + id.length * 31 + id.charCodeAt(0)) & 0xff);
const oldBytes = (id: string): Uint8Array => Uint8Array.from({ length: 150 }, (_, i) => (i + id.length) & 0xff);
const boot = (id: string, version: string, more: Partial<CoopBootRecord> = {}): CoopBootRecord => ({ kind: 'fx', name: id, state: 'loaded', version, author: 'coldsoldr', ...more });

/** An OP-1 with yesterday's files: four of them behind the catalogue, saw current, and a file co-op does not know. */
const yesterday = (more: ConstructorParameters<typeof FakeCoop>[0] = {}): FakeCoop =>
  new FakeCoop({
    build: 'e12',
    files: Object.fromEntries(['bitcrusher', 'fickle', 'rings', 'saw', 'tape_echo', 'mine'].map((id) => [`${id}.coop`, oldBytes(id)])),
    boot: {
      'bitcrusher.coop': boot('bitcrusher', '0.2'),
      'fickle.coop': boot('fickle', '0.2'),
      'rings.coop': boot('rings', '0.9', { kind: 'synth' }),
      'saw.coop': boot('saw', '0.2', { kind: 'synth' }),
      'tape_echo.coop': boot('tape_echo', '0.3'),
      'mine.coop': boot('mine', '0.1'),
    },
    ...more,
  });

interface Run {
  outcome: UpdateOutcome;
  questions: string[];
  progress: string[];
  fetched: string[];
}

async function run(device: FakeCoop, more: Partial<UpdateOptions> = {}, answers: boolean[] = []): Promise<Run> {
  const questions: string[] = [];
  const progress: string[] = [];
  const fetched: string[] = [];
  const outcome = await updateCoopPlugins(createSession(device, { timeoutMs: 20 }), {
    index,
    known: [],
    fetchBytes: (p: HostedPluginEntry) => {
      fetched.push(p.id);
      return Promise.resolve(newBytes(p.id));
    },
    confirm: (q) => {
      questions.push(q);
      return answers[questions.length - 1] ?? true;
    },
    onProgress: (text) => progress.push(text),
    ...more,
  });
  return { outcome, questions, progress, fetched };
}

describe('updateCoopPlugins', () => {
  it('replaces every file the catalogue has a newer build of, after one question that lists them', async () => {
    const device = yesterday();
    const { outcome, questions, progress, fetched } = await run(device);

    expect(questions).toEqual([
      'Update 4 plugins on the OP-1?\n\nbitcrusher: 0.2 → 0.3\nfickle: 0.2 → 0.3\nrings: 0.9 → 0.10\ntape echo: 0.3 → 0.4\n\nEach replaces the file on the OP-1.',
    ]);
    expect(fetched).toEqual(['bitcrusher', 'fickle', 'rings', 'tape_echo']);
    for (const id of fetched) expect(device.files.get(`${id}.coop`), id).toEqual(newBytes(id));
    // What was current or unknown to co-op is left as it was.
    expect(device.files.get('saw.coop')).toEqual(oldBytes('saw'));
    expect(device.files.get('mine.coop')).toEqual(oldBytes('mine'));

    expect(outcome).toMatchObject({ kind: 'ran', failed: null, skipped: [], blocked: [] });
    expect(outcome.kind === 'ran' && outcome.done.map((u) => u.file)).toEqual(['bitcrusher.coop', 'fickle.coop', 'rings.coop', 'tape_echo.coop']);
    expect(outcome.inventory?.records).toHaveLength(6);
    expect(progress[0]).toBe('Updating 1 of 4: bitcrusher');
    expect(progress).toContain('Updating 3 of 4: rings, 100 %');
    expect(progress.at(-1)).toBe('Updating 4 of 4: tape echo, 100 %');
    expect(updateSummary(outcome)).toBe('4 plugins updated. Power-cycle the OP-1 to load them.');
  });

  it('stops at the first file that fails and says what was done, what failed and what was not sent', async () => {
    const device = yesterday();
    device.failWriteOf = 6; // the second chunk of the second file
    const { outcome, fetched } = await run(device);

    expect(fetched).toEqual(['bitcrusher', 'fickle']);
    expect(outcome).toMatchObject({ kind: 'ran' });
    if (outcome.kind !== 'ran') return;
    expect(outcome.done.map((u) => u.file)).toEqual(['bitcrusher.coop']);
    expect(outcome.failed).toMatchObject({ update: { file: 'fickle.coop' }, sending: true });
    expect(outcome.failed?.error).toMatch(/a write error on the OP-1/);
    expect(outcome.skipped.map((u) => u.file)).toEqual(['rings.coop', 'tape_echo.coop']);
    // The files not reached are untouched; the inventory was read again all the same.
    expect(device.files.get('bitcrusher.coop')).toEqual(newBytes('bitcrusher'));
    expect(device.files.get('rings.coop')).toEqual(oldBytes('rings'));
    expect(device.files.get('tape_echo.coop')).toEqual(oldBytes('tape_echo'));
    expect(outcome.inventory).not.toBeNull();
    const summary = updateSummary(outcome);
    expect(summary).toMatch(/^Updated 1 of 4: bitcrusher\. fickle failed: the OP-1 refused writing fickle\.coop.*a write error on the OP-1\./);
    expect(summary).toContain('Its file on the OP-1 may be incomplete and will not load until it is sent again.');
    expect(summary).toMatch(/Not sent: rings, tape echo\. Power-cycle the OP-1 to load the updated ones\.$/);
  });

  it('counts a download that fails as the failure, with nothing of that file sent', async () => {
    const device = yesterday();
    const { outcome } = await run(device, {
      fetchBytes: (p) => (p.id === 'bitcrusher' ? Promise.reject(new Error('bitcrusher/bitcrusher.coop does not match the plugin index')) : Promise.resolve(newBytes(p.id))),
    });
    expect(outcome).toMatchObject({ kind: 'ran', done: [], failed: { sending: false } });
    expect(device.files.get('bitcrusher.coop')).toEqual(oldBytes('bitcrusher'));
    expect(device.requests.some((r) => r.cmd === 0x22)).toBe(false);
    expect(updateSummary(outcome)).toBe(
      'Updated none of 4. bitcrusher failed: bitcrusher/bitcrusher.coop does not match the plugin index. Not sent: fickle, rings, tape echo.',
    );
  });

  it('sends nothing when everything is current', async () => {
    const device = new FakeCoop({ build: 'e12', files: { 'saw.coop': oldBytes('saw'), 'mine.coop': oldBytes('mine') }, boot: { 'saw.coop': boot('saw', '0.2'), 'mine.coop': boot('mine', '9') } });
    const { outcome, questions, fetched } = await run(device);
    expect(outcome).toMatchObject({ kind: 'none', blocked: [] });
    expect(questions).toEqual([]);
    expect(fetched).toEqual([]);
    expect(updateSummary(outcome)).toBe('All plugins are up to date.');
  });

  it('sends nothing when the user says no to the list', async () => {
    const device = yesterday();
    const { outcome, fetched } = await run(device, {}, [false]);
    expect(outcome.kind).toBe('cancelled');
    expect(fetched).toEqual([]);
    expect(device.requests.some((r) => r.cmd === 0x22)).toBe(false);
    expect(updateSummary(outcome)).toBe('Nothing was sent.');
  });

  it('leaves out a build that needs a later core, and says so', async () => {
    const later: PluginIndex = { ...index, plugins: index.plugins.map((p) => (p.id === 'fickle' ? { ...p, requires: 'coop e13' } : p)) };
    const device = yesterday();
    const { outcome, questions, fetched } = await run(device, { index: later });
    expect(questions[0]).toContain('Update 3 plugins on the OP-1?');
    expect(questions[0]).toMatch(/\n\nNot updated: fickle 0\.3 needs coop e13\.$/);
    expect(fetched).toEqual(['bitcrusher', 'rings', 'tape_echo']);
    expect(device.files.get('fickle.coop')).toEqual(oldBytes('fickle'));
    expect(updateSummary(outcome)).toBe('3 plugins updated. Power-cycle the OP-1 to load them. Not updated: fickle 0.3 needs coop e13.');

    // When that is the only newer build there is nothing to do, and the reason is given.
    const only = new FakeCoop({ build: 'e12', files: { 'fickle.coop': oldBytes('fickle') }, boot: { 'fickle.coop': boot('fickle', '0.2') } });
    const none = await run(only, { index: later });
    expect(none.fetched).toEqual([]);
    expect(updateSummary(none.outcome)).toBe('All plugins are up to date. Not updated: fickle 0.3 needs coop e13.');
  });

  it('asks again when the updated set would not fit the plugin memory, and stops on a no', async () => {
    // rings 0.10 grew: 900,000 bytes where the OP-1 holds 300,000 of a budget of 1,000,000 with the others.
    const known: KnownCoopFile[] = [{ file: 'rings.coop', sizeBytes: 200, memory: { kind: 'synth', cap: 900_000, scratch: 0, voice: 0, screen: false, controls: false } }];
    const device = (): FakeCoop =>
      new FakeCoop({
        build: 'e12',
        files: { 'rings.coop': oldBytes('rings'), 'saw.coop': oldBytes('saw'), 'zz.coop': oldBytes('zz') },
        boot: {
          'rings.coop': boot('rings', '0.9', { kind: 'synth', need: 300_000 }),
          'saw.coop': boot('saw', '0.2', { kind: 'synth', need: 100_000 }),
          'zz.coop': boot('zz', '1', { need: 150_000 }),
        },
        memory: { budget: 1_000_000, used: 550_000 },
      });

    const no = device();
    const refused = await run(no, { known }, [true, false]);
    expect(refused.questions).toHaveLength(2);
    expect(refused.questions[1]).toContain('With the updates, the plugins do not fit in the OP-1’s plugin memory: those switched on need 1,123 KB; the OP-1 has 977 KB.');
    expect(refused.questions[1]).toContain('At the next power cycle the OP-1 would leave out zz: it loads the plugins in name order.');
    expect(refused.questions[1]).toContain('Update anyway?');
    expect(refused.outcome.kind).toBe('cancelled');
    expect(no.files.get('rings.coop')).toEqual(oldBytes('rings'));

    const yes = device();
    const sent = await run(yes, { known }, [true, true]);
    expect(sent.outcome).toMatchObject({ kind: 'ran', failed: null });
    expect(yes.files.get('rings.coop')).toEqual(newBytes('rings'));

    // An update that fits is not asked about twice.
    const small = await run(device(), { known: [{ ...(known[0] as KnownCoopFile), memory: { ...(known[0] as KnownCoopFile).memory, cap: 300_000 } }] });
    expect(small.questions).toHaveLength(1);
  });

  it('updates one file when asked for one, and skips what was already sent since the boot', async () => {
    const device = yesterday();
    const one = await run(device, { only: 'rings.coop' });
    expect(one.questions[0]).toBe('Update 1 plugin on the OP-1?\n\nrings: 0.9 → 0.10\n\nIt replaces the file on the OP-1.');
    expect(one.fetched).toEqual(['rings']);
    expect(updateSummary(one.outcome)).toBe('1 plugin updated. Power-cycle the OP-1 to load it.');

    // The inventory still names 0.9 for rings until the power cycle; it is not sent a second time.
    const rest = await run(device, { already: new Map([['rings.coop', '0.10']]) });
    expect(rest.fetched).toEqual(['bitcrusher', 'fickle', 'tape_echo']);
  });

  it('offers a file with no version the catalogue’s build, as unknown', async () => {
    const device = new FakeCoop({ build: 'e12', files: { 'saw.coop': oldBytes('saw') } });
    const { questions, outcome } = await run(device);
    expect(questions[0]).toContain('saw: unknown version → 0.2');
    expect(outcome).toMatchObject({ kind: 'ran', failed: null });
  });
});
