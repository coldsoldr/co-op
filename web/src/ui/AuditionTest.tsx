import { useState } from 'react';
import { runSpeedTest, secondsToSend, sendAudition, type AuditionReply, type AuditionRequest, type SpeedResult } from '../core/usbmidi/audition';
import { messageOf } from '../core/util';
import { openOp1Port } from './webmidi';

const MESSAGES: Record<AuditionReply, string> = {
  hello: 'The audition plugin answered.',
  applied: 'The test sound is on the current synth slot. Play the keys.',
  'put back': 'Your own sound is back in the slot.',
  'not synth': 'Press the synth key on the OP-1 first, and close any sound editor.',
  'nothing to put back': 'There is nothing to put back: no audition is running.',
  busy: 'The OP-1 was busy saving. Try again.',
  report: 'Speed report received.',
};

/** The median and largest sample patches in the test set. */
const TYPICAL_BYTES = 462_566;
const LARGEST_BYTES = 1_059_790;

const minutes = (bytes: number, rate: number): string => `${(secondsToSend(bytes, rate) / 60).toFixed(1)} min`;

/** Only in the dev build: drives the audition proof of concept. */
export function AuditionTest() {
  const [working, setWorking] = useState(false);
  const [result, setResult] = useState<{ text: string; failed: boolean } | null>(null);
  const [speed, setSpeed] = useState<readonly SpeedResult[]>([]);

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

  async function testSpeed(): Promise<void> {
    setWorking(true);
    setResult(null);
    setSpeed([]);
    try {
      const port = await openOp1Port();
      try {
        await runSpeedTest(port, (one) => setSpeed((prev) => [...prev, one]));
        setResult({ text: 'Speed test finished.', failed: false });
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
        <button type="button" onClick={() => void testSpeed()} disabled={working}>
          Speed test
        </button>
      </div>
      {speed.length > 0 && (
        <div className="table-scroll">
          <table className="library">
            <caption>Sample patch send estimates, before any checking or resending</caption>
            <thead>
              <tr>
                <th scope="col">Messages a second</th>
                <th scope="col">Arrived</th>
                <th scope="col">KB a second</th>
                <th scope="col">Typical patch (460 KB)</th>
                <th scope="col">Largest (1 MB)</th>
              </tr>
            </thead>
            <tbody>
              {speed.map((one) => (
                <tr key={one.target}>
                  <th scope="row">{Math.round(one.messagesPerSecond)}</th>
                  <td>
                    {one.received} of {one.sent}
                    {one.intact ? ', intact' : ', LOST DATA'}
                  </td>
                  <td>{((one.messagesPerSecond * 1.75) / 1024).toFixed(1)}</td>
                  <td>{minutes(TYPICAL_BYTES, one.messagesPerSecond)}</td>
                  <td>{minutes(LARGEST_BYTES, one.messagesPerSecond)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
      {result && (
        <p role={result.failed ? 'alert' : 'status'} className={result.failed ? 'error' : undefined}>
          {result.text}
        </p>
      )}
    </section>
  );
}
