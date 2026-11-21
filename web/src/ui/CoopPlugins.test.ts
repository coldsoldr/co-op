import { afterEach, describe, expect, it, vi } from 'vitest';
import type { HostedPluginEntry, PluginIndex, PluginIndexEntry } from '@co-op/shared/plugins';
import { FakeCoop } from '../../test/fake-coop';
import { CoopCaps, readCoopInventory } from '../core/usbmidi/coop';
import { updateCoopPlugins } from '../core/usbmidi/coop-update';
import { COOP_TOO_OLD, withCoop } from './CoopPlugins';
import { openMidiLeases } from './webmidi';

/** Web MIDI over a FakeCoop: the OP-1's two ports, which count how often they were opened and whether they are open. */
function stubOp1(device: FakeCoop) {
  const ports = { opens: 0, open: new Set<string>(), requests: 0 };
  const port = (id: string) => ({
    name: 'OP-1 Midi Device',
    state: 'connected',
    open: () => {
      ports.opens++;
      ports.open.add(id);
      return Promise.resolve();
    },
    close: () => {
      ports.open.delete(id);
      return Promise.resolve();
    },
  });
  const input = { ...port('in'), onmidimessage: null as ((e: { data: Uint8Array }) => void) | null };
  const output = { ...port('out'), send: (bytes: Uint8Array) => device.send(bytes) };
  device.onSysex((data) => input.onmidimessage?.({ data }));
  const access = Object.assign(new EventTarget(), { inputs: new Map([['in', input]]), outputs: new Map([['out', output]]) });
  vi.stubGlobal('navigator', {
    requestMIDIAccess: () => {
      ports.requests++;
      return Promise.resolve(access);
    },
  });
  return { ports, input };
}

const entry = (id: string): PluginIndexEntry => ({
  id,
  name: id,
  firmware: 'op1/coop',
  kind: 'fx',
  author: 'coldsoldr',
  version: '0.3',
  description: 'An effect.',
  file: `${id}.coop`,
  screenshot: `${id}/screenshot.png`,
  manual: `${id}/manual.md`,
  requires: 'coop e10',
  tags: [],
  sizeBytes: 200,
  sha256: 'a'.repeat(64),
});
const ids = ['a', 'b', 'c', 'd'];
const index: PluginIndex = { format: 1, generatedAt: '2026-11-21T00:00:00.000Z', plugins: ids.map(entry) };
const device = (more: ConstructorParameters<typeof FakeCoop>[0] = {}): FakeCoop =>
  new FakeCoop({
    build: 'e12',
    files: Object.fromEntries(ids.map((id) => [`${id}.coop`, new Uint8Array(150)])),
    boot: Object.fromEntries(ids.map((id) => [`${id}.coop`, { kind: 'fx', name: id, state: 'loaded', version: '0.2', author: 'coldsoldr' }])),
    ...more,
  });

afterEach(() => {
  vi.unstubAllGlobals();
  expect(openMidiLeases()).toBe(0);
});

describe('withCoop', () => {
  it('closes both ports once the work is done', async () => {
    const { ports, input } = stubOp1(device());
    const inventory = await withCoop((session) => {
      expect(ports.open).toEqual(new Set(['in', 'out']));
      return readCoopInventory(session);
    });
    expect(inventory.records.length).toBe(ids.length);
    expect(ports.open.size).toBe(0);
    expect(input.onmidimessage).toBeNull();
  });

  it('closes both ports when the work fails, and when the OP-1 is too old', async () => {
    const { ports } = stubOp1(device());
    await expect(withCoop(() => Promise.reject(new Error('broken')))).rejects.toThrow('broken');
    expect(ports.open.size).toBe(0);

    const old = stubOp1(device({ caps: CoopCaps.INVENTORY }));
    await expect(withCoop(() => Promise.resolve())).rejects.toThrow(COOP_TOO_OLD);
    expect(old.ports.open.size).toBe(0);
  });

  it('keeps the port for the whole of Update all, not per file, and frees it after', async () => {
    const op1 = device();
    const { ports } = stubOp1(op1);
    const outcome = await withCoop((session) =>
      updateCoopPlugins(session, {
        index,
        known: [],
        fetchBytes: (p: HostedPluginEntry) => Promise.resolve(Uint8Array.from({ length: 200 }, (_, i) => (i + p.id.charCodeAt(0)) & 0xff)),
        confirm: () => true,
        onProgress: () => undefined,
      }),
    );
    expect(outcome.kind === 'ran' ? outcome.done.length : outcome).toBe(ids.length);
    expect(ports.requests).toBe(1);
    expect(ports.opens).toBe(2);
    expect(ports.open.size).toBe(0);
  });

  it('opens the port lazily again for the next operation', async () => {
    const { ports } = stubOp1(device());
    for (let i = 0; i < 3; i++) {
      await withCoop((session) => readCoopInventory(session));
      expect(ports.open.size).toBe(0);
    }
    expect(ports.requests).toBe(3);
  });
});
