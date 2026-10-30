import { useCallback, useRef, useState } from 'react';
import type { Op1Meta } from '../core/codec/op1-meta';
import {
  AUDITION_MESSAGES,
  NO_PLUGIN_REPLY,
  encodeAuditionPatch,
  sendAudition,
  sendAuditionPatch,
  type AuditionReply,
} from '../core/usbmidi/audition';
import type { MidiPort } from '../core/usbmidi/session';
import { messageOf } from '../core/util';
import { openOp1Port } from './webmidi';

const NOT_LOADED = 'This patch cannot be auditioned on the OP-1: its settings are not in the form the plugin takes.';

export interface Audition {
  /** True once the plugin has answered. */
  ready: boolean;
  /** True while something is being sent. */
  working: boolean;
  /** The patch on the OP-1 now; null when its own sound is in place. */
  current: string | null;
  /** The last thing worth telling the person. */
  message: { text: string; failed: boolean } | null;
  connect(): Promise<void>;
  play(name: string, meta: Op1Meta | null): Promise<void>;
  putBack(): Promise<void>;
}

/**
 * Auditioning on the OP-1 through the audition plugin. The MIDI port is opened for each action and
 * closed straight after, because on Windows one program at a time can hold it.
 */
export function useAudition(): Audition {
  const [ready, setReady] = useState(false);
  const [working, setWorking] = useState(false);
  const [current, setCurrent] = useState<string | null>(null);
  const [message, setMessage] = useState<Audition['message']>(null);
  const busy = useRef(false);

  const withPort = useCallback(async (task: (port: MidiPort) => Promise<AuditionReply>): Promise<AuditionReply | null> => {
    if (busy.current) return null;
    busy.current = true;
    setWorking(true);
    setMessage(null);
    try {
      const port = await openOp1Port();
      try {
        return await task(port);
      } finally {
        port.close();
      }
    } catch (e) {
      const text = messageOf(e);
      if (text === NO_PLUGIN_REPLY) setReady(false);
      setMessage({ text, failed: true });
      return null;
    } finally {
      busy.current = false;
      setWorking(false);
    }
  }, []);

  const connect = useCallback(async () => {
    const reply = await withPort((port) => sendAudition(port, 'hello', 1500));
    if (reply !== 'hello') return;
    setReady(true);
    setMessage({ text: AUDITION_MESSAGES.hello, failed: false });
  }, [withPort]);

  const play = useCallback(
    async (name: string, meta: Op1Meta | null) => {
      const frame = encodeAuditionPatch(meta);
      if (!frame) {
        setMessage({ text: NOT_LOADED, failed: true });
        return;
      }
      const reply = await withPort((port) => sendAuditionPatch(port, frame));
      if (reply === null) return;
      if (reply === 'applied') setCurrent(name);
      setMessage(reply === 'applied' ? { text: `${name} is on the current synth slot. Play the keys.`, failed: false } : { text: AUDITION_MESSAGES[reply], failed: true });
    },
    [withPort],
  );

  const putBack = useCallback(async () => {
    const reply = await withPort((port) => sendAudition(port, 'putBack'));
    if (reply === null) return;
    if (reply === 'put back' || reply === 'nothing to put back') setCurrent(null);
    setMessage({ text: AUDITION_MESSAGES[reply], failed: reply !== 'put back' && reply !== 'nothing to put back' });
  }, [withPort]);

  return { ready, working, current, message, connect, play, putBack };
}
