/**
 * Talks to the audition proof-of-concept plugin (te-op1-cfw, ports/knl/audition): control changes on
 * channel 16, because plugins are not given SysEx. CC 118 asks, CC 119 answers.
 */
import type { MidiPort } from './session';

const CC_CHANNEL_16 = 0xbf;
const CC_REQUEST = 118;
const CC_REPLY = 119;

export const AUDITION_REQUEST = { audition: 127, putBack: 0, hello: 1 } as const;
export type AuditionRequest = keyof typeof AUDITION_REQUEST;

export type AuditionReply = 'applied' | 'put back' | 'not synth' | 'nothing to put back' | 'busy' | 'hello';

const REPLIES: Readonly<Record<number, AuditionReply>> = {
  1: 'applied',
  2: 'put back',
  3: 'not synth',
  4: 'nothing to put back',
  5: 'busy',
  6: 'hello',
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
