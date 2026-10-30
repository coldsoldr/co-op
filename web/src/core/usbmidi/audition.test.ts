import { describe, expect, it } from 'vitest';
import { NO_PLUGIN_REPLY, collectReport, measureSpeed, parseAuditionReply, runSpeedTest, secondsToSend, sendAudition } from './audition';
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

describe('speed test', () => {
  it('reads a report of seven-bit groups, low first', () => {
    const cc = (n: number, v: number): Uint8Array => Uint8Array.of(0xbf, n, v);
    expect(collectReport([cc(20, 1), cc(21, 2), cc(25, 127), cc(26, 1)])).toEqual({ count: 257, sum: 255 });
  });

  it('calls a run intact when everything arrived', async () => {
    const result = await measureSpeed(fakeCounter(), 500, 0.05, 1);
    expect(result).toMatchObject({ target: 500, sent: 25, received: 25, intact: true });
  });

  it('notices a lost message and stops raising the rate', async () => {
    const seen: number[] = [];
    const results = await runSpeedTest(fakeCounter(100), (r) => seen.push(r.target));
    expect(results.at(-1)?.intact).toBe(false);
    expect(seen.length).toBeLessThan(6);
  }, 20000);

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
