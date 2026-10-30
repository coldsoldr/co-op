/**
 * Talks to the audition proof-of-concept plugin (te-op1-cfw, ports/knl/audition): control changes on
 * channel 16, because plugins are not given SysEx. CC 118 asks, CC 119 answers.
 */
import { isSynthMeta, type Op1Meta, type SampleKitMeta } from '../codec/op1-meta';
import { pack7 } from './framing';
import type { MidiPort } from './session';

const CC_CHANNEL_16 = 0xbf;
const CC_REQUEST = 118;
const CC_REPLY = 119;

export const AUDITION_REQUEST = {
  audition: 127,
  putBack: 0,
  hello: 1,
  zeroSpeed: 2,
  reportSpeed: 3,
  begin: 4,
  apply: 5,
  takeAudio: 6,
  applyAudio: 7,
  cancel: 8,
} as const;
export type AuditionRequest = keyof typeof AUDITION_REQUEST;

export type AuditionReply = 'applied' | 'put back' | 'not synth' | 'nothing to put back' | 'busy' | 'hello' | 'report' | 'bad patch' | 'no room' | 'ready' | 'other slot' | 'not drum';

const REPLIES: Readonly<Record<number, AuditionReply>> = {
  1: 'applied',
  2: 'put back',
  3: 'not synth',
  4: 'nothing to put back',
  5: 'busy',
  6: 'hello',
  7: 'report',
  8: 'bad patch',
  9: 'no room',
  10: 'ready',
  11: 'other slot',
  12: 'not drum',
};

/** What a reply means, for people. */
export const AUDITION_MESSAGES: Record<AuditionReply, string> = {
  hello: 'The audition plugin answered.',
  applied: 'The test sound is on the current synth slot. Play the keys.',
  'put back': 'Your own sound is back in the slot.',
  'not synth': 'Press the synth key on the OP-1 first, and close any sound editor.',
  'not drum': 'Press the drum key on the OP-1 first, and close any sound editor.',
  'nothing to put back': 'There is nothing to put back: no audition is running.',
  busy: 'The OP-1 was busy saving. Try again.',
  report: 'Speed report received.',
  'bad patch': 'The patch did not arrive whole, so the slot has its own sound back. Try again.',
  'no room': 'The OP-1 could not store a copy of your sound, so nothing was changed. Free some space and try again.',
  'other slot': 'An audition is running on another slot. Go back to it, or put your sound back first.',
  ready: 'The OP-1 is ready for the audio.',
};

export const NO_PLUGIN_REPLY = 'The OP-1 did not answer. Check that the audition plugin is installed and that the OP-1 is not in disk mode.';

/** The plugin's answer in a MIDI message, or null for anything else. */
export function parseAuditionReply(bytes: Uint8Array): AuditionReply | null {
  if (bytes.length !== 3 || bytes[0] !== CC_CHANNEL_16 || bytes[1] !== CC_REPLY) return null;
  return REPLIES[bytes[2] as number] ?? null;
}

/** Sends one request and waits for the answer; rejects with NO_PLUGIN_REPLY after `timeoutMs`. */
export function sendAudition(port: MidiPort, request: AuditionRequest, timeoutMs = 4000): Promise<AuditionReply> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(NO_PLUGIN_REPLY)), timeoutMs);
    port.onSysex((bytes) => {
      const reply = parseAuditionReply(bytes);
      if (reply === null) return;
      clearTimeout(timer);
      resolve(reply);
    });
    port.send(Uint8Array.of(CC_CHANNEL_16, CC_REQUEST, AUDITION_REQUEST[request]));
  });
}

/** Patch and speed test data: polyphonic pressure on channel 15, 14 bits a message. */
const PRESSURE_CHANNEL_15 = 0xae;

const FRAME_FORMAT = 3;
const WORDS = 8;
const PADS = 24;
const FLAG_FX = 1;
const FLAG_LFO = 2;
const FLAG_DRUM = 4;
const MAX_NAME = 39;
const MAX_PAYLOAD = 800;
/** The synth sampler's buffer: 6 s at 44.1 kHz, after its guard samples. */
export const MAX_AUDIO_BYTES = (264_600 - 8) * 2;
/** A drum kit's buffer: 12 s. */
export const MAX_KIT_BYTES = (529_200 - 8) * 2;
const AUDIO_RATE = 44_100;
/** Below the measured ceiling of 18,479 messages a second, which other computers may not reach. */
const SEND_RATE = 15_000;
const BATCH_MS = 10;

const isWord = (n: unknown): n is number => Number.isInteger(n) && (n as number) >= -32768 && (n as number) <= 32767;
const validName = (s: unknown): s is string => typeof s === 'string' && /^[\x20-\x7e]{1,39}$/.test(s);
const sumOf = (bytes: Iterable<number>): number => {
  let sum = 0;
  for (const byte of bytes) sum = (sum + byte) >>> 0;
  return sum;
};

export interface AuditionAudio {
  pcm: Float32Array;
  sampleRate: number;
}

const isKit = (meta: Op1Meta | null): meta is SampleKitMeta => meta?.type === 'drum';

/** True for a patch that can play on the OP-1: a synth patch or a sampled kit. A synthesised kit cannot. */
export function canAudition(meta: Op1Meta | null): boolean {
  return isSynthMeta(meta) || isKit(meta);
}

/** True for a patch whose audio has to go too. */
export const needsAudio = (meta: Op1Meta | null): boolean => isKit(meta) || (isSynthMeta(meta) && meta.type === 'sampler');

/** A sampler's audio as the OP-1 holds it, or null when it does not fit that form. */
export function encodeAuditionAudio(audio: AuditionAudio, kit = false): Uint8Array | null {
  if (audio.sampleRate !== AUDIO_RATE) return null;
  const samples = Math.min(audio.pcm.length, (kit ? MAX_KIT_BYTES : MAX_AUDIO_BYTES) / 2);
  const bytes = new Uint8Array(samples * 2);
  const view = new DataView(bytes.buffer);
  for (let i = 0; i < samples; i++) {
    view.setInt16(i * 2, Math.round(Math.max(-1, Math.min(1, audio.pcm[i] as number)) * 32767), true);
  }
  return bytes;
}

/**
 * The frame the plugin reads (audition_frame.h in te-op1-cfw), or null for a patch it cannot take.
 * Patch fields are checked here because the OP-1's parser stops on values it does not expect.
 */
export function encodeAuditionPatch(meta: Op1Meta | null, audio: Uint8Array | null = null): Uint8Array | null {
  if (!canAudition(meta) || !meta) return null;
  const kit = isKit(meta);
  const sampler = !kit && needsAudio(meta);
  if ((kit || sampler) !== (audio !== null)) return null;
  const baseFreq = sampler ? (meta as { base_freq?: number }).base_freq : 0;
  if (sampler && !(typeof baseFreq === 'number' && baseFreq > 0)) return null;
  const synth = isSynthMeta(meta) ? meta : null;
  const groups = [synth?.knobs ?? new Array<number>(WORDS).fill(0), synth?.adsr ?? (meta as SampleKitMeta).dyna_env, meta.fx_params, meta.lfo_params];
  const pads = kit ? [meta.start, meta.end, meta.pitch, meta.playmode, meta.reverse, meta.volume] : [];
  if (!pads.every((a) => Array.isArray(a) && a.length === PADS && a.every((n) => Number.isInteger(n) && Math.abs(n) <= 0x7fffffff))) return null;
  const version = kit ? meta.drum_version : synth?.synth_version;
  const name = meta.name.replace(/[^\x20-\x7e]/g, '').slice(0, MAX_NAME) || 'audition';
  const names = [meta.type, meta.fx_type, meta.lfo_type, name];
  if (!groups.every((g) => Array.isArray(g) && g.length === WORDS && g.every(isWord)) || !names.every(validName)) return null;
  if (!Number.isInteger(meta.octave) || Math.abs(meta.octave) > 127) return null;
  if (!Number.isInteger(version) || (version as number) < 0 || (version as number) > 255) return null;
  const flags = (meta.fx_active ? FLAG_FX : 0) | (meta.lfo_active ? FLAG_LFO : 0) | (kit ? FLAG_DRUM : 0);
  const payload: number[] = [FRAME_FORMAT, flags, meta.octave & 0xff, version as number];
  for (const group of groups) for (const word of group) payload.push(word & 0xff, (word >> 8) & 0xff);
  const numbers = new DataView(new ArrayBuffer(16));
  numbers.setFloat64(0, baseFreq as number, true);
  numbers.setUint32(8, audio?.length ?? 0, true);
  numbers.setUint32(12, audio ? sumOf(audio) : 0, true);
  payload.push(...new Uint8Array(numbers.buffer));
  for (const array of pads) {
    for (const value of array) payload.push(value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff, (value >> 24) & 0xff);
  }
  for (const text of names) {
    for (let i = 0; i < text.length; i++) payload.push(text.charCodeAt(i));
    payload.push(0);
  }
  if (payload.length > MAX_PAYLOAD) return null;
  const sum = sumOf(payload);
  return Uint8Array.from([
    payload.length & 0xff,
    payload.length >> 8,
    ...payload,
    sum & 0xff,
    (sum >>> 8) & 0xff,
    (sum >>> 16) & 0xff,
    sum >>> 24,
  ]);
}

const sendValues = (port: MidiPort, values: Uint8Array, from: number, to: number): void => {
  for (let i = from; i < to; i += 2) port.send(Uint8Array.of(PRESSURE_CHANNEL_15, values[i] as number, values[i + 1] ?? 0));
};

const request = (port: MidiPort, name: AuditionRequest): void => port.send(Uint8Array.of(CC_CHANNEL_16, CC_REQUEST, AUDITION_REQUEST[name]));

export interface SendOptions {
  signal?: AbortSignal;
  /** How much of the transfer has gone, 0 to 1. */
  onProgress?: (fraction: number) => void;
  /** Messages a second; only tests change it. */
  rate?: number;
}

/**
 * Sends a patch to the plugin and asks it to load it on the current slot. A sampler's audio follows
 * its frame at a steady rate, so progress is what has really gone. Aborting tells the plugin to
 * drop what it has, and rejects with the signal's reason.
 */
export async function sendAuditionPatch(port: MidiPort, frame: Uint8Array, audio: Uint8Array | null = null, options: SendOptions = {}): Promise<AuditionReply> {
  const { signal, onProgress, rate = SEND_RATE } = options;
  request(port, 'begin');
  const values = pack7(frame);
  sendValues(port, values, 0, values.length);
  if (!audio) {
    onProgress?.(1);
    return sendAudition(port, 'apply');
  }
  // The OP-1 first stores the slot's own audio, which takes a few seconds the first time.
  const ready = await sendAudition(port, 'takeAudio', 30_000);
  if (ready !== 'ready') return ready;
  const packed = pack7(audio);
  const started = performance.now();
  for (let at = 0; at < packed.length; ) {
    if (signal?.aborted) {
      request(port, 'cancel');
      throw signal.reason;
    }
    const due = Math.ceil(((performance.now() - started) / 1000) * rate) * 2;
    const to = Math.min(packed.length, Math.max(at + 2, due));
    sendValues(port, packed, at, to);
    at = to;
    onProgress?.(at / packed.length);
    await new Promise((resolve) => setTimeout(resolve, BATCH_MS));
  }
  return sendAudition(port, 'applyAudio', 30_000);
}

/** Speed test data travels the same way, 14 bits a message. */
const CC_COUNT = 20;
const CC_SUM = 25;
export const BYTES_PER_MESSAGE = 14 / 8;
const REPORT_TIMEOUT_MS = 30_000;
export interface SpeedResult {
  /** The rate asked for, messages a second. */
  target: number;
  sent: number;
  received: number;
  /** The rate measured from the first message to the plugin's reply. */
  messagesPerSecond: number;
  /** True when every message arrived with its value intact. */
  intact: boolean;
}

/** The 14-bit value of test message `i`, varied so a dropped or corrupted message changes the sum. */
const valueOf = (i: number): number => (i * 31 + 7) & 0x3fff;

function testSum(count: number): number {
  let sum = 0;
  for (let i = 0; i < count; i++) sum = (sum + valueOf(i)) >>> 0;
  return sum;
}

/** Reads the 7-bit groups of a report: count on CC 20 to 24, sum on CC 25 to 29, low first. */
export function collectReport(messages: readonly Uint8Array[]): { count: number; sum: number } {
  const word = (first: number): number => {
    let value = 0;
    for (const m of messages) {
      const group = (m[1] as number) - first;
      if (m[0] === CC_CHANNEL_16 && group >= 0 && group < 5) value += (m[2] as number) * 2 ** (7 * group);
    }
    return value >>> 0;
  };
  return { count: word(CC_COUNT), sum: word(CC_SUM) };
}

const wait = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));

/**
 * Sends test data at about `target` messages a second for `seconds`, then asks the plugin what
 * arrived. Batches go out every few milliseconds, because a browser timer cannot pace single
 * messages. The request travels behind the data, so the reply also times the delivery: a browser
 * accepts messages faster than the USB link carries them.
 */
export async function measureSpeed(port: MidiPort, target: number, seconds = 2): Promise<SpeedResult> {
  await sendAudition(port, 'zeroSpeed');
  const started = performance.now();
  const total = Math.round(target * seconds);
  let sent = 0;
  while (sent < total) {
    const due = Math.min(total, Math.ceil(((performance.now() - started) / 1000) * target));
    for (; sent < due; sent++) {
      const value = valueOf(sent);
      port.send(Uint8Array.of(PRESSURE_CHANNEL_15, value >> 7, value & 127));
    }
    await wait(BATCH_MS);
  }
  const report: Uint8Array[] = [];
  await new Promise<void>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(NO_PLUGIN_REPLY)), REPORT_TIMEOUT_MS);
    port.onSysex((bytes) => {
      if (parseAuditionReply(bytes) === 'report') {
        clearTimeout(timer);
        resolve();
      } else {
        report.push(bytes);
      }
    });
    port.send(Uint8Array.of(CC_CHANNEL_16, CC_REQUEST, AUDITION_REQUEST.reportSpeed));
  });
  const elapsed = (performance.now() - started) / 1000;
  const { count, sum } = collectReport(report);
  return { target, sent, received: count, messagesPerSecond: sent / elapsed, intact: count === sent && sum === testSum(sent) };
}

const FIRST_TARGET = 8000;
const LAST_TARGET = 256_000;
/** How close the highest clean and lowest failing rates get before the search ends. */
const SEARCH_CLOSE = 0.1;
/** A run this far below its target means the link, not the plugin, set the pace. */
const PACED_BELOW = 0.8;

/** Why a speed test ended. */
export type SpeedStop = 'lost data' | 'no reply' | 'link limit' | 'top of the test';

/**
 * Doubles the rate until data is lost, then halves the gap between the highest clean rate and the
 * lowest failing one. It also ends when the link cannot carry the rate asked for, or when the
 * plugin stops answering, which a run that overwhelmed the OP-1's MIDI input would cause.
 */
export async function runSpeedTest(
  port: MidiPort,
  onResult: (result: SpeedResult) => void,
  seconds = 2,
  pacedBelow = PACED_BELOW,
): Promise<SpeedStop> {
  let clean = 0;
  let failed = Number.POSITIVE_INFINITY;
  let target = FIRST_TARGET;
  for (;;) {
    let result: SpeedResult;
    try {
      result = await measureSpeed(port, target, seconds);
    } catch {
      return 'no reply';
    }
    onResult(result);
    if (result.intact) {
      clean = target;
      if (result.messagesPerSecond < target * pacedBelow) return 'link limit';
    } else {
      failed = target;
    }
    if (failed === Number.POSITIVE_INFINITY) {
      target *= 2;
      if (target > LAST_TARGET) return 'top of the test';
    } else {
      if (failed - clean <= failed * SEARCH_CLOSE) return 'lost data';
      target = Math.round((clean + failed) / 2);
    }
  }
}

/** Seconds to send `bytes` at a measured rate, with no allowance for checking or resending. */
export const secondsToSend = (bytes: number, messagesPerSecond: number): number => bytes / (messagesPerSecond * BYTES_PER_MESSAGE);
