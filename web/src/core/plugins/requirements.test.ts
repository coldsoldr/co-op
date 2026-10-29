import { describe, expect, it } from 'vitest';
import type { PluginIndex, PluginIndexEntry } from '@co-op/shared/plugins';
import { soundTraits, type SoundTraits } from '../codec/sound-traits';
import type { Op1Meta } from '../codec/op1-meta';
import { parseInventory } from '../usbmidi/inventory';
import { installedNames, pluginContext, requiredPlugins } from './requirements';
import { SYNTH_META, DRUM_META } from '../../../test/fixtures/meta';

const entry = (id: string, firmware: PluginIndexEntry['firmware'], savedName?: string): PluginIndexEntry => ({
  id,
  name: id,
  firmware,
  kind: 'fx',
  author: 'co-op',
  version: '1',
  description: 'test',
  license: 'MIT',
  file: firmware === 'op1/opus' ? `${id}.opmod` : `${id}.knl`,
  screenshot: `${id}/screenshot.png`,
  manual: `${id}/manual.md`,
  tags: [],
  sizeBytes: 1,
  sha256: '0'.repeat(64),
  ...(savedName === undefined ? {} : { savedName }),
});

const INDEX: PluginIndex = {
  format: 1,
  generatedAt: '2026-10-29T00:00:00Z',
  plugins: [
    entry('rings', 'op1/hundekraft', 'rings'),
    entry('tape_drive', 'op1/hundekraft', 'tape drive'),
    entry('nameless', 'op1/hundekraft'),
    // A plugin that replaces a built-in effect: sounds naming it need nothing.
    entry('delay', 'op1/hundekraft', 'delay'),
    entry('dx', 'op1/opus', 'opus.dx'),
    entry('chorus2', 'op1/opus', 'opus.chorus2'),
  ],
};

const synth = (change: Record<string, unknown>): SoundTraits =>
  soundTraits({ ...SYNTH_META, ...change } as Op1Meta) as SoundTraits;
const ids = (list: readonly PluginIndexEntry[]): string[] => list.map((p) => p.id);

describe('requiredPlugins', () => {
  it('finds the library plugins of a synth engine and an effect for the firmware', () => {
    expect(ids(requiredPlugins(synth({ type: 'rings', fx_type: 'tape drive' }), INDEX, 'op1/hundekraft'))).toEqual([
      'rings',
      'tape_drive',
    ]);
    expect(ids(requiredPlugins(synth({ type: 'rings' }), INDEX, 'op1/stock'))).toEqual([]);
    expect(ids(requiredPlugins(synth({}), INDEX, 'op1/hundekraft'))).toEqual([]);
  });

  it('leaves out names the firmware has built in, and drum engines', () => {
    expect(ids(requiredPlugins(synth({ fx_type: 'delay' }), INDEX, 'op1/hundekraft'))).toEqual([]);
    const kit = soundTraits({ ...DRUM_META, type: 'rings' } as Op1Meta) as SoundTraits;
    expect(ids(requiredPlugins(kit, INDEX, 'op1/hundekraft'))).toEqual([]);
  });

  it('finds the opus plugin and effects an opus sound names', () => {
    const traits = synth({ type: 'iter', knobs: [0, 0, 0, 0, 0x100, 0, 0, 0], opus: { plugin: 'opus.dx', fx_type: 'opus.chorus2' } });
    expect(ids(requiredPlugins(traits, INDEX, 'op1/opus'))).toEqual(['dx', 'chorus2']);
  });
});

describe('installedNames', () => {
  it('gives the saved names of loaded records only', () => {
    const inventory = parseInventory(
      [
        'kennel-inventory 1',
        ['fx', 'tape_drive', 'knl', 'plugins/tape_drive.knl', '', '', '1', '0', 'loaded', '', ''].join('\t'),
        ['synth', 'rings', 'knl', 'plugins/rings.knl', '', '', '1', '0', 'failed', '', 'bad'].join('\t'),
        ['fx', 'drumplus', 'knl', 'plugins/drumplus.knl', '', '', '1', '0', 'loaded', '', ''].join('\t'),
      ].join('\n'),
    );
    expect([...installedNames(inventory)]).toEqual(['tape drive', 'drum+']);
    expect(installedNames(null).size).toBe(0);
  });
});

describe('pluginContext', () => {
  it('maps the saved names of the firmware plugins to their ids', () => {
    const context = pluginContext(INDEX, 'op1/hundekraft', null);
    expect(context && [...context.library]).toEqual([
      ['rings', 'rings'],
      ['tape drive', 'tape_drive'],
      ['delay', 'delay'],
    ]);
    expect(pluginContext(null, 'op1/hundekraft', null)).toBeNull();
  });
});
