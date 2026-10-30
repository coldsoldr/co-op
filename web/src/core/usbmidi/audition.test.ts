import { describe, expect, it } from 'vitest';
import type { Op1Meta, SynthMeta } from '../codec/op1-meta';
import { pack7, unpack7 } from './framing';
import {
  NO_PLUGIN_REPLY,
  canAudition,
  encodeAuditionAudio,
  collectReport,
  encodeAuditionPatch,
  measureSpeed,
  needsAudio,
  parseAuditionReply,
  runSpeedTest,
  secondsToSend,
  sendAudition,
  sendAuditionPatch,
} from './audition';
import type { MidiPort } from './session';

function fakePort(answer: number[][]): MidiPort & { sent: number[][] } {
  let listener: (bytes: Uint8Array) => void = () => undefined;
  const sent: number[][] = [];
  return {
    sent,
    send(bytes) {
      sent.push([...bytes]);
      for (const reply of answer) listener(Uint8Array.from(reply));
    },
    onSysex(cb) {
      listener = cb;
    },
    close() {},
  };
}

describe('parseAuditionReply', () => {
  it('reads the reply CC on channel 16 only', () => {
    expect(parseAuditionReply(Uint8Array.of(0xbf, 119, 1))).toBe('applied');
    expect(parseAuditionReply(Uint8Array.of(0xbf, 119, 6))).toBe('hello');
    expect(parseAuditionReply(Uint8Array.of(0xb0, 119, 1))).toBeNull();
    expect(parseAuditionReply(Uint8Array.of(0xbf, 118, 1))).toBeNull();
    expect(parseAuditionReply(Uint8Array.of(0xbf, 119, 99))).toBeNull();
    expect(parseAuditionReply(Uint8Array.of(0xf8))).toBeNull();
  });
});

/** A plugin that counts test data and drops every message after `keep`. */
function fakeCounter(keep = Infinity): MidiPort & { count: number } {
  let listener: (bytes: Uint8Array) => void = () => undefined;
  let sum = 0;
  const port = {
    count: 0,
    send(bytes: Uint8Array) {
      const answer = (cc: number, value: number): void => listener(Uint8Array.of(0xbf, cc, value));
      if (bytes[0] === 0xae) {
        if (port.count >= keep) return;
        port.count++;
        sum = (sum + ((bytes[1] as number) << 7 | (bytes[2] as number))) >>> 0;
      } else if (bytes[2] === 2) {
        port.count = 0;
        sum = 0;
        answer(119, 6);
      } else if (bytes[2] === 3) {
        for (let i = 0; i < 5; i++) answer(20 + i, Math.floor(port.count / 2 ** (7 * i)) & 127);
        for (let i = 0; i < 5; i++) answer(25 + i, Math.floor(sum / 2 ** (7 * i)) & 127);
        answer(119, 7);
      }
    },
    onSysex(cb: (bytes: Uint8Array) => void) {
      listener = cb;
    },
    close() {},
  };
  return port;
}

const meta = (over: Partial<SynthMeta> = {}): SynthMeta => ({
  adsr: [2624, 64, 17408, 15808, 2048, 6464, 4000, 4000],
  fx_active: false,
  fx_params: [10536, 3968, 7776, 17408, 8000, 8000, 8000, 8000],
  fx_type: 'phone',
  knobs: [22927, 21615, 9151, 32767, 0, 0, 0, -3],
  lfo_active: true,
  lfo_params: [2560, 32767, 5952, 10080, 0, 0, 0, 0],
  lfo_type: 'value',
  name: 'engage',
  octave: -1,
  synth_version: 2,
  type: 'voltage',
  ...over,
});

describe('encodeAuditionPatch', () => {
  it('lays out a frame the plugin can check', () => {
    const frame = encodeAuditionPatch(meta()) as Uint8Array;
    const size = (frame[0] as number) | ((frame[1] as number) << 8);
    expect(frame.length).toBe(size + 6);
    expect([...frame.subarray(2, 6)]).toEqual([4, 2, 255, 2]);
    expect(new DataView(frame.buffer).getInt16(2 + 4 + 14, true)).toBe(-3);
    let sum = 0;
    for (const byte of frame.subarray(2, 2 + size)) sum += byte;
    expect(new DataView(frame.buffer).getUint32(2 + size, true)).toBe(sum);
    expect(new TextDecoder().decode(frame.subarray(2 + 84, 2 + size))).toBe('voltage\0phone\0value\0engage\0');
  });

  it('refuses a sampler without audio, a kit and anything the OP-1 could stop on', () => {
    expect(encodeAuditionPatch(meta({ type: 'sampler', base_freq: 440 }))).toBeNull();
    expect(encodeAuditionPatch(meta({ type: 'sampler' }), Uint8Array.of(1, 2))).toBeNull();
    expect(encodeAuditionPatch(meta(), Uint8Array.of(1, 2))).toBeNull();
    expect(encodeAuditionPatch(null)).toBeNull();
    expect(encodeAuditionPatch(meta({ knobs: [1, 2, 3] }))).toBeNull();
    expect(encodeAuditionPatch(meta({ knobs: [40000, 0, 0, 0, 0, 0, 0, 0] }))).toBeNull();
    expect(encodeAuditionPatch(meta({ type: '' }))).toBeNull();
    expect(canAudition(meta({ type: 'sampler' }))).toBe(true);
    expect(canAudition(null)).toBe(false);
    expect(canAudition(meta())).toBe(true);
  });

  it('keeps a name printable, and never leaves it empty', () => {
    const frame = encodeAuditionPatch(meta({ name: 'é' })) as Uint8Array;
    expect(new TextDecoder().decode(frame)).toContain('audition');
  });
});

describe('drum kits', () => {
  const kit = (over: Record<string, unknown> = {}): Op1Meta =>
    ({
      type: 'drum',
      name: 'dust kit',
      octave: 0,
      drum_version: 2,
      dyna_env: [0, 8192, 0, 8192, 0, 0, 0, 0],
      fx_active: false,
      fx_params: [8000, 8000, 8000, 8000, 8000, 8000, 8000, 8000],
      fx_type: 'delay',
      lfo_active: false,
      lfo_params: [16000, 16000, 16000, 16000, 0, 0, 0, 0],
      lfo_type: 'tremolo',
      start: new Array(24).fill(0).map((_, i) => i * 1_000_000),
      end: new Array(24).fill(1_300_000_000),
      pitch: new Array(24).fill(-1),
      playmode: new Array(24).fill(8192),
      reverse: new Array(24).fill(8192),
      volume: new Array(24).fill(8192),
      ...over,
    }) as unknown as Op1Meta;

  it('lays out the pad arrays before the names', () => {
    const frame = encodeAuditionPatch(kit(), Uint8Array.of(1, 2)) as Uint8Array;
    const view = new DataView(frame.buffer);
    expect(frame[3]).toBe(4);
    expect(view.getInt32(2 + 84 + 4, true)).toBe(1_000_000);
    expect(view.getInt32(2 + 84 + 24 * 4, true)).toBe(1_300_000_000);
    expect(view.getInt32(2 + 84 + 2 * 24 * 4, true)).toBe(-1);
    expect(new TextDecoder().decode(frame).endsWith('dust kit\0' + new TextDecoder().decode(frame.subarray(-4)))).toBe(true);
  });

  const dbox = (over: Record<string, unknown> = {}): Op1Meta =>
    kit({ type: 'dbox', dbox_data: new Array(24).fill(0).map((_, i) => [i, -24320, 32767, 0, 0, 0, 0, 7]), ...over });

  it('sends a dbox kit as settings alone, with its rows before the names', () => {
    const frame = encodeAuditionPatch(dbox()) as Uint8Array;
    const view = new DataView(frame.buffer);
    expect(frame[3]).toBe(12);
    expect(view.getUint32(2 + 76, true)).toBe(0);
    expect(view.getInt16(2 + 84 + 16, true)).toBe(1);
    expect(view.getInt16(2 + 84 + 18, true)).toBe(-24320);
    expect(new TextDecoder().decode(frame)).toContain('dbox\0delay\0tremolo\0dust kit\0');
    expect(canAudition(dbox())).toBe(true);
    expect(needsAudio(dbox())).toBe(false);
  });

  it('refuses a dbox kit with the wrong shape, and given audio', () => {
    expect(encodeAuditionPatch(dbox({ dbox_data: [[1, 2]] }))).toBeNull();
    expect(encodeAuditionPatch(dbox({ dbox_data: new Array(24).fill(new Array(8).fill(40000)) }))).toBeNull();
    expect(encodeAuditionPatch(dbox(), Uint8Array.of(1))).toBeNull();
  });

  it('refuses a kit with no audio, short arrays, and a Gin kit', () => {
    expect(encodeAuditionPatch(kit())).toBeNull();
    expect(encodeAuditionPatch(kit({ start: [1, 2] }), Uint8Array.of(1))).toBeNull();
    expect(canAudition(kit({ type: 'gin' }))).toBe(false);
    expect(canAudition(kit())).toBe(true);
    expect(needsAudio(kit())).toBe(true);
  });

  it('allows a kit its 12 s', () => {
    expect((encodeAuditionAudio({ pcm: new Float32Array(600000), sampleRate: 44100 }, true) as Uint8Array).length).toBe(1058384);
  });
});

describe('sampler audio', () => {
  it('holds it as 16-bit little-endian samples at 44.1 kHz, and no more than the buffer', () => {
    expect([...(encodeAuditionAudio({ pcm: Float32Array.of(0, 1, -1), sampleRate: 44100 }) as Uint8Array)]).toEqual([0, 0, 255, 127, 1, 128]);
    expect(encodeAuditionAudio({ pcm: Float32Array.of(0), sampleRate: 22050 })).toBeNull();
    expect((encodeAuditionAudio({ pcm: new Float32Array(300000), sampleRate: 44100 }) as Uint8Array).length).toBe(529184);
  });

  it('puts its length, sum and root in the frame', () => {
    const frame = encodeAuditionPatch(meta({ type: 'sampler', base_freq: 246.5 }), Uint8Array.of(1, 2, 3)) as Uint8Array;
    const view = new DataView(frame.buffer);
    expect(view.getFloat64(2 + 68, true)).toBe(246.5);
    expect(view.getUint32(2 + 76, true)).toBe(3);
    expect(view.getUint32(2 + 80, true)).toBe(6);
  });

  const samplerPort = (sent: number[][]): MidiPort => {
    let listener: (bytes: Uint8Array) => void = () => undefined;
    return {
      send(bytes) {
        sent.push([...bytes]);
        const answers: Record<number, number> = { 6: 10, 7: 1 };
        const value = bytes[1] === 118 ? answers[bytes[2] as number] : undefined;
        if (value !== undefined) listener(Uint8Array.of(0xbf, 119, value));
      },
      onSysex(cb) {
        listener = cb;
      },
      close() {},
    };
  };

  it('sends frame, audio and apply in order, reporting progress', async () => {
    const sent: number[][] = [];
    const audio = new Uint8Array(1400).fill(200);
    const frame = encodeAuditionPatch(meta({ type: 'sampler', base_freq: 440 }), audio) as Uint8Array;
    const seen: number[] = [];
    await expect(sendAuditionPatch(samplerPort(sent), frame, audio, { rate: 1e7, onProgress: (f) => seen.push(f) })).resolves.toBe('applied');
    const controls = sent.filter((m) => m[1] === 118 && m[0] === 0xbf).map((m) => m[2]);
    expect(controls).toEqual([4, 6, 7]);
    expect(seen.at(-1)).toBe(1);
    const take = sent.findIndex((m) => m[0] === 0xbf && m[2] === 6);
    const values = sent.slice(take + 1, -1).flatMap((m) => [m[1] as number, m[2] as number]);
    expect(unpack7(Uint8Array.from(values)).subarray(0, 1400)).toEqual(audio);
  });

  it('cancels when aborted, and tells the plugin', async () => {
    const sent: number[][] = [];
    const audio = new Uint8Array(4000);
    const frame = encodeAuditionPatch(meta({ type: 'sampler', base_freq: 440 }), audio) as Uint8Array;
    const controller = new AbortController();
    controller.abort(new Error('replaced'));
    await expect(sendAuditionPatch(samplerPort(sent), frame, audio, { signal: controller.signal })).rejects.toThrow('replaced');
    expect(sent.at(-1)).toEqual([0xbf, 118, 8]);
  });
});

describe('sendAuditionPatch', () => {
  it('opens, sends the values two by two, and applies', async () => {
    const sent: number[][] = [];
    let listener: (bytes: Uint8Array) => void = () => undefined;
    const port: MidiPort = {
      send(bytes) {
        sent.push([...bytes]);
        if (bytes[1] === 118 && bytes[2] === 5) listener(Uint8Array.of(0xbf, 119, 1));
      },
      onSysex(cb) {
        listener = cb;
      },
      close() {},
    };
    const frame = encodeAuditionPatch(meta()) as Uint8Array;
    await expect(sendAuditionPatch(port, frame)).resolves.toBe('applied');
    expect(sent[0]).toEqual([0xbf, 118, 4]);
    expect(sent.at(-1)).toEqual([0xbf, 118, 5]);
    const values = sent.slice(1, -1).flatMap((m) => [m[1] as number, m[2] as number]);
    expect(sent.slice(1, -1).every((m) => m[0] === 0xae)).toBe(true);
    expect(unpack7(Uint8Array.from(values)).subarray(0, frame.length)).toEqual(frame);
    expect(pack7(frame).length % 2 === 0 ? values.length : values.length - 1).toBe(pack7(frame).length);
  });
});

describe('speed test', () => {
  it('reads a report of seven-bit groups, low first', () => {
    const cc = (n: number, v: number): Uint8Array => Uint8Array.of(0xbf, n, v);
    expect(collectReport([cc(20, 1), cc(21, 2), cc(25, 127), cc(26, 1)])).toEqual({ count: 257, sum: 255 });
  });

  it('calls a run intact when everything arrived', async () => {
    const result = await measureSpeed(fakeCounter(), 500, 0.05);
    expect(result).toMatchObject({ target: 500, sent: 25, received: 25, intact: true });
  });

  it('narrows in on the rate where data starts to go missing', async () => {
    const seen: number[] = [];
    await expect(runSpeedTest(fakeCounter(50), (r) => seen.push(r.target), 0.01, 0)).resolves.toBe('lost data');
    expect(seen).toEqual([8000, 4000, 6000, 5000, 5500]);
  });

  it('stops at the top of the test when nothing is ever lost', async () => {
    await expect(runSpeedTest(fakeCounter(), () => undefined, 0.002, 0)).resolves.toBe('top of the test');
  });

  it('stops when the link cannot carry the rate', async () => {
    await expect(runSpeedTest(fakeCounter(), () => undefined, 0.002, Infinity)).resolves.toBe('link limit');
  });

  it('reports a plugin that stops answering', async () => {
    await expect(runSpeedTest(fakePort([]), () => undefined, 0.01)).resolves.toBe('no reply');
  }, 10000);

  it('estimates the time to send a file', () => {
    expect(secondsToSend(1750, 1000)).toBeCloseTo(1);
  });
});

describe('sendAudition', () => {
  it('sends the request and skips other messages until the reply', async () => {
    const port = fakePort([[0xf8], [0xbf, 119, 1]]);
    await expect(sendAudition(port, 'audition')).resolves.toBe('applied');
    expect(port.sent).toEqual([[0xbf, 118, 127]]);
  });

  it('sends 0 to put back and 1 to say hello', async () => {
    const port = fakePort([[0xbf, 119, 2]]);
    await sendAudition(port, 'putBack');
    await sendAudition(port, 'hello');
    expect(port.sent).toEqual([[0xbf, 118, 0], [0xbf, 118, 1]]);
  });

  it('says what to check when nothing answers', async () => {
    await expect(sendAudition(fakePort([]), 'hello', 5)).rejects.toThrow(NO_PLUGIN_REPLY);
  });
});
