import { describe, expect, it } from 'vitest';
import { NO_PLUGIN_REPLY, parseAuditionReply, sendAudition } from './audition';
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
