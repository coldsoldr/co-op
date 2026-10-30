/**
 * Talks to the audition proof-of-concept plugin (te-op1-cfw, ports/knl/audition): control changes on
 * channel 16, because plugins are not given SysEx. CC 118 asks, CC 119 answers.
 */
import type { MidiPort } from './session';

const CC_CHANNEL_16 = 0xbf;
const CC_REQUEST = 118;
const CC_REPLY = 119;

export const AUDITION_REQUEST = { audition: 127, putBack: 0, hello: 1, zeroSpeed: 2, reportSpeed: 3 } as const;
export type AuditionRequest = keyof typeof AUDITION_REQUEST;

export type AuditionReply = 'applied' | 'put back' | 'not synth' | 'nothing to put back' | 'busy' | 'hello' | 'report';

const REPLIES: Readonly<Record<number, AuditionReply>> = {
  1: 'applied',
  2: 'put back',
  3: 'not synth',
  4: 'nothing to put back',
  5: 'busy',
  6: 'hello',
  7: 'report',
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

/** Speed test data: polyphonic pressure on channel 15, 14 bits a message. */
const PRESSURE_CHANNEL_15 = 0xae;
const CC_COUNT = 20;
const CC_SUM = 25;
export const BYTES_PER_MESSAGE = 14 / 8;
const REPORT_TIMEOUT_MS = 30_000;
const BATCH_MS = 10;

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

function sumOf(count: number): number {
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
  return { target, sent, received: count, messagesPerSecond: sent / elapsed, intact: count === sent && sum === sumOf(sent) };
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
