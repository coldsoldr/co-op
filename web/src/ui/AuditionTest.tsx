import { useState } from 'react';
import { sendAudition, type AuditionReply, type AuditionRequest } from '../core/usbmidi/audition';
import { messageOf } from '../core/util';
import { openOp1Port } from './webmidi';

const MESSAGES: Record<AuditionReply, string> = {
  hello: 'The audition plugin answered.',
  applied: 'The test sound is on the current synth slot. Play the keys.',
  'put back': 'Your own sound is back in the slot.',
  'not synth': 'Press the synth key on the OP-1 first, and close any sound editor.',
  'nothing to put back': 'There is nothing to put back: no audition is running.',
  busy: 'The OP-1 was busy saving. Try again.',
};

/** Only in the dev build: drives the audition proof of concept. */
export function AuditionTest() {
  const [working, setWorking] = useState(false);
  const [result, setResult] = useState<{ text: string; failed: boolean } | null>(null);

  async function send(request: AuditionRequest): Promise<void> {
    setWorking(true);
    setResult(null);
    try {
      const port = await openOp1Port();
      try {
        setResult({ text: MESSAGES[await sendAudition(port, request)], failed: false });
      } finally {
        port.close();
      }
    } catch (e) {
      setResult({ text: messageOf(e), failed: true });
    } finally {
      setWorking(false);
    }
  }

  return (
    <section className="panel" aria-labelledby="audition-heading">
      <h3 id="audition-heading">Audition test (dev only)</h3>
      <p className="muted">
        Needs the audition plugin on hundekraft 3.0.3, with the OP-1 plugged in normally (not in disk mode) and on a synth
        slot. Do not touch the knobs while the test sound is on.
      </p>
      <div className="plugin-buttons">
        <button type="button" onClick={() => void send('hello')} disabled={working}>
          Check plugin
        </button>
        <button type="button" className="primary" onClick={() => void send('audition')} disabled={working}>
          Audition test sound
        </button>
        <button type="button" onClick={() => void send('putBack')} disabled={working}>
          Put my sound back
        </button>
      </div>
      {result && (
        <p role={result.failed ? 'alert' : 'status'} className={result.failed ? 'error' : undefined}>
          {result.text}
        </p>
      )}
    </section>
  );
}
