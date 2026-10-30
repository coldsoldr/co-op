import { useCallback, useRef, useState } from 'react';
import { decodePcm, readPatch } from '../core/codec/patch';
import type { Op1Meta } from '../core/codec/op1-meta';
import {
  AUDITION_MESSAGES,
  NO_PLUGIN_REPLY,
  encodeAuditionAudio,
  encodeAuditionPatch,
  needsAudio,
  sendAudition,
  sendAuditionPatch,
  type AuditionReply,
} from '../core/usbmidi/audition';
import type { MidiPort } from '../core/usbmidi/session';
import { messageOf } from '../core/util';
import { openOp1Port } from './webmidi';

const NOT_LOADED = 'This patch cannot be auditioned on the OP-1: its settings or audio are not in the form the plugin takes.';
const DRIVE_GONE = 'This patch holds audio and is on the OP-1 drive, which cannot be read out of disk mode. Audition it from your local folder.';

/** What the plugin needs to play a patch. */
export interface AuditionSource {
  /** Reads the patch file. */
  load: () => Promise<Uint8Array>;
  /** Its settings from an earlier read, for when the file can no longer be reached. */
  meta?: Op1Meta | null;
}

export interface Audition {
  /** True once the plugin has answered. */
  ready: boolean;
  /** The transfer under way. */
  transfer: { id: string; fraction: number } | null;
  /** The patch on the OP-1 now; null when its own sound is in place. */
  current: { id: string; name: string } | null;
  /** The last thing worth telling the person. */
  message: { text: string; failed: boolean } | null;
  connect(): Promise<void>;
  /** Sends a patch to the OP-1, cancelling any transfer already running. */
  play(id: string, name: string, source: AuditionSource): Promise<void>;
  putBack(): Promise<void>;
}

const CANCELLED = new Error('a newer audition took over');

/**
 * Auditioning on the OP-1 through the audition plugin, shared by the Patches and Library tabs. The
 * MIDI port is opened for each action and closed straight after, because on Windows one program at
 * a time can hold it.
 */
export function useAudition(): Audition {
  const [ready, setReady] = useState(false);
  const [transfer, setTransfer] = useState<Audition['transfer']>(null);
  const [current, setCurrent] = useState<Audition['current']>(null);
  const [message, setMessage] = useState<Audition['message']>(null);
  const running = useRef<{ controller: AbortController; done: Promise<unknown> } | null>(null);
  /** Bumped by every action, so only the newest of several quick clicks goes on to run. */
  const latest = useRef(0);

  /** Stops what is running and waits for it, so the port is free. */
  const cancel = useCallback(async () => {
    const now = running.current;
    if (!now) return;
    now.controller.abort(CANCELLED);
    await now.done.catch(() => undefined);
  }, []);

  const withPort = useCallback(
    async (task: (port: MidiPort, signal: AbortSignal) => Promise<AuditionReply | null>): Promise<AuditionReply | null> => {
      const mine = ++latest.current;
      await cancel();
      if (mine !== latest.current) return null;
      const controller = new AbortController();
      const work = (async (): Promise<AuditionReply | null> => {
        const port = await openOp1Port(controller.signal);
        try {
          return await task(port, controller.signal);
        } finally {
          port.close();
        }
      })();
      const entry = { controller, done: work };
      running.current = entry;
      setMessage(null);
      try {
        return await work;
      } catch (e) {
        if (controller.signal.aborted) return null;
        const text = messageOf(e);
        if (text === NO_PLUGIN_REPLY) setReady(false);
        setMessage({ text, failed: true });
        return null;
      } finally {
        if (running.current === entry) running.current = null;
      }
    },
    [cancel],
  );

  const connect = useCallback(async () => {
    const reply = await withPort((port) => sendAudition(port, 'hello', 1500));
    if (reply !== 'hello') return;
    setReady(true);
    setMessage({ text: AUDITION_MESSAGES.hello, failed: false });
  }, [withPort]);

  const play = useCallback(
    async (id: string, name: string, source: AuditionSource) => {
      setTransfer({ id, fraction: 0 });
      const reply = await withPort(async (port, signal) => {
        let meta = source.meta ?? null;
        let audio: Uint8Array | null = null;
        try {
          const patch = readPatch(await source.load());
          meta = patch.meta;
          if (needsAudio(meta)) audio = encodeAuditionAudio({ pcm: decodePcm(patch), sampleRate: patch.comm.sampleRate }, meta?.type === 'drum');
        } catch (e) {
          if (!meta) throw e;
          if (needsAudio(meta)) throw new Error(DRIVE_GONE, { cause: e });
        }
        signal.throwIfAborted();
        const frame = encodeAuditionPatch(meta, audio);
        if (!frame) throw new Error(NOT_LOADED);
        return sendAuditionPatch(port, frame, audio, {
          signal,
          onProgress: (fraction) => setTransfer((now) => (now?.id === id ? { id, fraction } : now)),
        });
      });
      setTransfer((now) => (now?.id === id ? null : now));
      if (reply === null) return;
      if (reply === 'applied') {
        setCurrent({ id, name });
        setMessage({ text: `${name} is on the current synth slot. Play the keys.`, failed: false });
      } else {
        setMessage({ text: AUDITION_MESSAGES[reply], failed: true });
      }
    },
    [withPort],
  );

  const putBack = useCallback(async () => {
    setTransfer(null);
    const reply = await withPort((port) => sendAudition(port, 'putBack', 30_000));
    if (reply === null) return;
    const fine = reply === 'put back' || reply === 'nothing to put back';
    if (fine) setCurrent(null);
    setMessage({ text: AUDITION_MESSAGES[reply], failed: !fine });
  }, [withPort]);

  return { ready, transfer, current, message, connect, play, putBack };
}
