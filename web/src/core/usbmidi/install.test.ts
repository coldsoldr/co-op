import { describe, it, expect } from 'vitest';
import { Command, MAX_DATA, crc32, u32le } from './framing';
import {
  CancelledOnOp1Error,
  NO_ANSWER,
  NO_PLUGIN_CAPABILITY,
  WORKSHOP_URL,
  installPlugins,
  type InstallOptions,
  type PluginFile,
} from './install';
import { createSession } from './session';
import { FakeOp1, type FakeOp1Options, type Request } from '../../../test/fake-op1';
import { knlBytes } from '../../../test/plugin-bytes';

/** Short waits keep the timeout and retry tests fast. */
const fast: InstallOptions = { controlTimeoutMs: 20, dataTimeoutMs: 20, readTimeoutMs: 20, pollMs: 1 };

const HEADER = ['kennel-inventory 1', 'core 01e00000 0003a000 1a2b3c4d'].join('\t');
const record = (...fields: string[]): string => fields.join('\t');
const inventoryOf = (...records: string[]): string => [HEADER, ...records].join('\n');
const EMPTY = inventoryOf(record('fx', 'delay', 'builtin', '', '', '', '0', '00000000', 'loaded', '', ''));

/** A 228-byte .knl, so it takes four DATA messages. */
const TAPE_DRIVE: PluginFile = { path: 'plugins/tape_drive.knl', bytes: knlBytes({ name: 'tape drive', author: 'a'.repeat(50), version: '1.0' }) };
const DRIVE: PluginFile = { path: 'plugins/drive.knl', bytes: knlBytes({ name: 'drive' }) };

const device = (options: FakeOp1Options = {}): FakeOp1 => new FakeOp1({ inventory: EMPTY, press: 'T1', ...options });
const names = (requests: readonly Request[]): string[] =>
  requests.map((r) => Object.entries(Command).find(([, cmd]) => cmd === r.cmd)?.[0] ?? `#${r.cmd}`);
const offsetOf = (r: Request): number => (r.payload[0] ?? 0) | ((r.payload[1] ?? 0) << 8) | ((r.payload[2] ?? 0) << 16);

/** No write command at all: what every refusal before the transfer must leave behind. */
function expectNoWrites(op1: FakeOp1): void {
  expect(op1.writes).toEqual([]);
}

/** co-op never sends OFF_SET, DELETE or RESTART, and nothing but the five plugin-file commands. */
function expectOnlyAllowedWrites(op1: FakeOp1): void {
  expect(op1.requests.every((r) => [1, 2, 3, 4, 5, 6, 7, 8].includes(r.cmd))).toBe(true);
}

describe('installPlugins', () => {
  it('stages a file, waits for T1 and reports the OP-1 accepted it', async () => {
    const op1 = device();
    const progress: [number, number][] = [];
    let prompts = 0;
    const outcome = await installPlugins(op1, [TAPE_DRIVE], {
      ...fast,
      onProgress: (sent, total) => progress.push([sent, total]),
      onPrompt: () => prompts++,
    });
    expect(outcome).toBe('accepted');
    expect(op1.installed).toEqual([{ path: TAPE_DRIVE.path, bytes: TAPE_DRIVE.bytes }]);
    expect(prompts).toBe(1);

    const size = TAPE_DRIVE.bytes.length;
    const pieces = Math.ceil(size / MAX_DATA);
    expect(names(op1.requests)).toEqual([
      'HELLO',
      ...Array<string>(Math.ceil(EMPTY.length / 64)).fill('LIST'),
      'ABORT',
      'BEGIN',
      ...Array<string>(pieces).fill('DATA'),
      'END',
      'COMMIT',
      'COMMIT',
    ]);
    const begin = op1.requests.find((r) => r.cmd === Command.BEGIN) as Request;
    expect([...begin.payload]).toEqual([...u32le(size), ...u32le(crc32(TAPE_DRIVE.bytes)), ...new TextEncoder().encode(TAPE_DRIVE.path), 0]);
    const data = op1.requests.filter((r) => r.cmd === Command.DATA);
    expect(data.map(offsetOf)).toEqual(Array.from({ length: pieces }, (_, i) => i * MAX_DATA));
    expect(data.every((r) => r.payload.length <= 4 + MAX_DATA)).toBe(true);
    expect(progress[0]).toEqual([0, size]);
    expect(progress.at(-1)).toEqual([size, size]);
    expectOnlyAllowedWrites(op1);
  });

  it('stages several files before one COMMIT, with progress over all of them', async () => {
    const op1 = device();
    const progress: number[] = [];
    await installPlugins(op1, [TAPE_DRIVE, DRIVE], { ...fast, onProgress: (sent) => progress.push(sent) });
    expect(op1.installed?.map((f) => f.path)).toEqual([TAPE_DRIVE.path, DRIVE.path]);
    expect(names(op1.writes).filter((n) => n !== 'DATA')).toEqual(['ABORT', 'BEGIN', 'END', 'BEGIN', 'END', 'COMMIT', 'COMMIT']);
    expect(progress.at(-1)).toBe(TAPE_DRIVE.bytes.length + DRIVE.bytes.length);
    expect(progress).toEqual([...progress].sort((a, b) => a - b));
  });

  it('works on a session the caller already has', async () => {
    const op1 = device();
    await installPlugins(createSession(op1, { timeoutMs: 20 }), [DRIVE], fast);
    expect(op1.installed).toHaveLength(1);
  });

  describe('refuses before sending anything', () => {
    it.each([
      'content/drive.knl',
      'OP1_vdk.ldr',
      'plugins/../drive.knl',
      'plugins/.boot',
      'plugins/sub/drive.knl',
      'plugins/Drive.knl',
      'plugins/drive.opmod',
      '/plugins/drive.knl',
      'plugins/drive.knl ',
    ])('a path that is not plugins/<id>.knl|.op1p: %j', async (path) => {
      const op1 = device();
      await expect(installPlugins(op1, [{ path, bytes: DRIVE.bytes }], fast)).rejects.toThrow('co-op only installs plugin files');
      expect(op1.requests).toEqual([]);
    });

    it('a file that fails the plugin check', async () => {
      const op1 = device();
      const broken = DRIVE.bytes.slice();
      broken[30] = 0x41;
      await expect(installPlugins(op1, [{ path: DRIVE.path, bytes: broken }], fast)).rejects.toThrow(
        'plugins/drive.knl is not a plugin the OP-1 can load: ',
      );
      await expect(installPlugins(op1, [{ path: 'plugins/drive.op1p', bytes: DRIVE.bytes }], fast)).rejects.toThrow(
        /not a plugin the OP-1 can load: .*op1r trailer/,
      );
      expect(op1.requests).toEqual([]);
    });

    it('a file whose saved name gives another id', async () => {
      const op1 = device();
      await expect(installPlugins(op1, [{ path: 'plugins/drive.knl', bytes: TAPE_DRIVE.bytes }], fast)).rejects.toThrow(
        'gives the id tape_drive, not drive',
      );
      expect(op1.requests).toEqual([]);
    });

    it('the same plugin twice, and an empty list', async () => {
      const op1 = device();
      await expect(installPlugins(op1, [DRIVE, DRIVE], fast)).rejects.toThrow('in the list twice');
      await expect(installPlugins(op1, [], fast)).rejects.toThrow('no plugin file');
      expect(op1.requests).toEqual([]);
    });

    it.each([
      ['inventory only', 1],
      ['plugin files only', 2],
      ['no capabilities byte', null],
    ])('firmware without both capabilities (%s)', async (_, caps) => {
      const op1 = device({ caps });
      const error = (await installPlugins(op1, [DRIVE], fast).catch((e: unknown) => e)) as Error;
      expect(error.message).toBe(NO_PLUGIN_CAPABILITY);
      expect(error.message).toContain(WORKSHOP_URL);
      expect(names(op1.requests)).toEqual(['HELLO']);
    });

    it('an OP-1 that does not answer HELLO', async () => {
      const op1 = device({ silent: true });
      await expect(installPlugins(op1, [DRIVE], fast)).rejects.toThrow(NO_ANSWER);
      expect(names(op1.requests)).toEqual(Array<string>(5).fill('HELLO'));
    });

    it('an OP-1 that stops answering while listing its plugins', async () => {
      const op1 = device({ fate: (r) => (r.cmd === Command.LIST ? 'drop' : undefined) });
      await expect(installPlugins(op1, [DRIVE], fast)).rejects.toThrow('while listing its plugins. Nothing was changed.');
      expectNoWrites(op1);
    });

    it.each([
      ['by path', record('fx', 'drive', 'knl', 'plugins/drive.knl', '1.0', 'x', '160', '00000000', 'loaded', '', '')],
      ['by path, as the other format', record('-', '', '', 'plugins/drive.op1p', '', '', '160', '00000000', 'failed', '', 'bad magic')],
      ['by name, from an .op1p', record('fx', 'drive', 'op1p', 'content/other.op1p', '', '', '160', '00000000', 'off', '', 'switched off')],
      ['switched off', record('fx', 'drive', 'knl', 'plugins/drive.knl', '', '', '160', '00000000', 'off', '', 'switched off')],
    ])('a plugin that is already installed (%s)', async (_, line) => {
      const op1 = device({ inventory: inventoryOf(line) });
      const error = (await installPlugins(op1, [TAPE_DRIVE, DRIVE], fast).catch((e: unknown) => e)) as Error;
      expect(error.message).toMatch(/^drive is already on the OP-1 \(/);
      expect(error.message).toContain('Remove it first in the hundekraft workshop');
      expect(error.message).toContain(WORKSHOP_URL);
      expect(error.message).toContain('replacing a plugin in place can hang the restart');
      expectNoWrites(op1);
    });
  });

  it('installs over a built-in of the same name', async () => {
    const op1 = device({ inventory: inventoryOf(record('fx', 'drive', 'builtin', '', '', '', '0', '00000000', 'loaded', '', '')) });
    expect(await installPlugins(op1, [DRIVE], fast)).toBe('accepted');
  });

  it('carries on from the count a gap reply reports after a lost DATA reply', async () => {
    let dataSeen = 0;
    const op1 = device({ fate: (r) => (r.cmd === Command.DATA && ++dataSeen === 2 ? 'lose' : undefined) });
    await installPlugins(op1, [TAPE_DRIVE], fast);
    const offsets = op1.requests.filter((r) => r.cmd === Command.DATA).map(offsetOf);
    // The second piece was written but its reply lost: the resend at 72 gets "offset" with 144.
    expect(offsets.slice(0, 4)).toEqual([0, 72, 72, 144]);
    expect(op1.installed?.[0]?.bytes).toEqual(TAPE_DRIVE.bytes);
  });

  it('resends from the last confirmed count when a DATA message is lost', async () => {
    let dataSeen = 0;
    const op1 = device({ fate: (r) => (r.cmd === Command.DATA && ++dataSeen === 3 ? 'drop' : undefined) });
    await installPlugins(op1, [TAPE_DRIVE], fast);
    expect(op1.requests.filter((r) => r.cmd === Command.DATA).map(offsetOf).slice(0, 4)).toEqual([0, 72, 144, 144]);
    expect(op1.installed?.[0]?.bytes).toEqual(TAPE_DRIVE.bytes);
  });

  it('takes "state" on a resent BEGIN as the lost first BEGIN having opened the file', async () => {
    let begins = 0;
    const op1 = device({ fate: (r) => (r.cmd === Command.BEGIN && ++begins === 1 ? 'lose' : undefined) });
    expect(await installPlugins(op1, [DRIVE], fast)).toBe('accepted');
    expect(names(op1.writes).slice(0, 3)).toEqual(['ABORT', 'BEGIN', 'BEGIN']);
    expect(op1.installed?.[0]?.bytes).toEqual(DRIVE.bytes);
  });

  it('refuses "state" on a first BEGIN', async () => {
    const op1 = device();
    op1.fate = (r) => {
      if (r.cmd !== Command.BEGIN) return undefined;
      op1.reply(r.cmd, r.seq, [2]);
      return 'drop';
    };
    await expect(installPlugins(op1, [DRIVE], fast)).rejects.toThrow('The OP-1 refused plugins/drive.knl (state). Nothing was installed.');
    expect(names(op1.writes)).toEqual(['ABORT', 'BEGIN']);
  });

  it('fails safely when a resent END answers "state"', async () => {
    let ends = 0;
    const op1 = device({ fate: (r) => (r.cmd === Command.END && ++ends === 1 ? 'lose' : undefined) });
    await expect(installPlugins(op1, [DRIVE], fast)).rejects.toThrow('did not confirm that plugins/drive.knl arrived whole');
    expect(names(op1.writes)).not.toContain('COMMIT');
  });

  it('stops on a CRC mismatch at END', async () => {
    const op1 = device({ damageAt: 100 });
    await expect(installPlugins(op1, [TAPE_DRIVE], fast)).rejects.toThrow(
      'plugins/tape_drive.knl arrived damaged (its CRC-32 does not match). Nothing was installed.',
    );
    expect(names(op1.writes)).not.toContain('COMMIT');
    expect(op1.staged).toEqual([]);
  });

  it('stops when the OP-1 goes silent in the middle of a file', async () => {
    let dataSeen = 0;
    const op1 = device({ fate: (r) => (r.cmd === Command.DATA && ++dataSeen >= 3 ? 'drop' : undefined) });
    await expect(installPlugins(op1, [TAPE_DRIVE], fast)).rejects.toThrow(
      'The OP-1 stopped answering while receiving plugins/tape_drive.knl. Nothing was installed.',
    );
    const data = op1.requests.filter((r) => r.cmd === Command.DATA);
    expect(data.map(offsetOf)).toEqual([0, 72, 144, 144, 144, 144, 144]);
    expect(names(op1.writes)).not.toContain('END');
  });

  it('stops when the OP-1 goes silent after ABORT', async () => {
    const op1 = device({ fate: (_, i) => (i > 3 ? 'drop' : undefined) });
    await expect(installPlugins(op1, [DRIVE], fast)).rejects.toThrow('stopped answering during the transfer');
  });

  it('reports T2 on the OP-1 as cancelled, with nothing installed', async () => {
    const op1 = device({ press: 'T2', afterPolls: 3 });
    let prompts = 0;
    const error = await installPlugins(op1, [DRIVE], { ...fast, onPrompt: () => prompts++ }).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(CancelledOnOp1Error);
    expect((error as Error).message).toBe('Cancelled on the OP-1 (T2). Nothing was installed.');
    expect(prompts).toBe(1);
    expect(op1.installed).toBeNull();
    expect(names(op1.writes).filter((n) => n === 'COMMIT')).toHaveLength(4);
  });

  it('keeps asking while nobody presses a key, until stopped', async () => {
    const op1 = device({ press: undefined });
    const controller = new AbortController();
    const done = installPlugins(op1, [DRIVE], { ...fast, signal: controller.signal, onPrompt: () => setTimeout(() => controller.abort(new Error('closed')), 20) });
    await expect(done).rejects.toThrow('closed');
    expect(op1.prompting).toBe(true);
    expect(op1.installed).toBeNull();
  });

  it('reports a silent OP-1 after the prompt as unconfirmed (the ok of a T1 restart was lost)', async () => {
    // The second COMMIT, after T1, is answered with ok but the reply is lost; the OP-1 then restarts.
    let commits = 0;
    const op1 = device({ fate: (r) => (r.cmd === Command.COMMIT && ++commits >= 2 ? 'lose' : undefined) });
    expect(await installPlugins(op1, [DRIVE], fast)).toBe('unconfirmed');
    expect(op1.installed).toHaveLength(1);
  });

  it('refuses a COMMIT the OP-1 refuses before its prompt', async () => {
    const op1 = device();
    op1.fate = (r) => {
      if (r.cmd !== Command.COMMIT) return undefined;
      op1.reply(r.cmd, r.seq, [7]);
      return 'drop';
    };
    await expect(installPlugins(op1, [DRIVE], fast)).rejects.toThrow('The OP-1 refused the install (io). Nothing was installed.');
  });

  it('stops in the middle of the transfer when aborted', async () => {
    const controller = new AbortController();
    let dataSeen = 0;
    const op1 = device({
      fate: (r) => {
        if (r.cmd === Command.DATA && ++dataSeen === 2) controller.abort(new Error('the OP-1 was disconnected'));
        return undefined;
      },
    });
    await expect(installPlugins(op1, [TAPE_DRIVE], { ...fast, signal: controller.signal })).rejects.toThrow('disconnected');
    expect(names(op1.writes)).not.toContain('END');
  });
});
