import { useEffect, useRef, useState, type DragEvent, type ChangeEvent } from 'react';
import { readPatch, decodePcm, type Patch } from '../core/codec/patch';
import { op1Stock } from '../core/profiles/op1-stock';
import { getProfile, profiles } from '../core/profiles/registry';
import type { Device } from './useDevice';
import { playPcm } from './audio';

interface Loaded {
  fileName: string;
  sizeBytes: number;
  patch: Patch;
}

/**
 * `active` is false while the tab is hidden; playback stops then. Compatibility is judged by the
 * open drive's profile when one is connected, else by a profile chosen here (stock by default).
 */
export function Inspector({ active = true, device }: { active?: boolean; device?: Device }) {
  const [loaded, setLoaded] = useState<Loaded | null>(null);
  const [chosenId, setChosenId] = useState(op1Stock.id);
  const connected = device?.source != null;
  const profile = connected && device ? device.profile : (getProfile(chosenId) ?? op1Stock);
  const [error, setError] = useState<string | null>(null);
  const stopRef = useRef<(() => void) | null>(null);

  const stop = (): void => {
    stopRef.current?.();
    stopRef.current = null;
  };

  useEffect(() => {
    if (!active) {
      stopRef.current?.();
      stopRef.current = null;
    }
  }, [active]);

  useEffect(
    () => () => {
      stopRef.current?.();
    },
    [],
  );

  async function load(file: File) {
    stop();
    try {
      const bytes = new Uint8Array(await file.arrayBuffer());
      setLoaded({ fileName: file.name, sizeBytes: bytes.length, patch: readPatch(bytes) });
      setError(null);
    } catch (e) {
      setLoaded(null);
      setError((e as Error).message);
    }
  }

  function onDrop(event: DragEvent<HTMLDivElement>) {
    event.preventDefault();
    const file = event.dataTransfer.files[0];
    if (file) void load(file);
  }

  function onPick(event: ChangeEvent<HTMLInputElement>) {
    const file = event.target.files?.[0];
    if (file) void load(file);
  }

  function play() {
    if (!loaded) return;
    stop();
    try {
      stopRef.current = playPcm(decodePcm(loaded.patch), loaded.patch.comm.sampleRate);
    } catch (e) {
      setError(`cannot play: ${(e as Error).message}`);
    }
  }

  const compat = loaded ? profile.compatibility(loaded.patch) : null;

  return (
    <section>
      <h2>Patch inspector</h2>
      <div
        onDrop={onDrop}
        onDragOver={(e) => e.preventDefault()}
        className="dropzone"
      >
        <p>Drop an OP-1 .aif patch here, or</p>
        <label>
          Choose a patch file <input type="file" accept=".aif,.aiff" onChange={onPick} />
        </label>
      </div>
      {connected ? (
        <p>
          Profile: <strong>{profile.label}</strong> <span className="muted">(from the open drive)</span>
        </p>
      ) : (
        <p>
          <label>
            Judge compatibility for{' '}
            <select value={chosenId} onChange={(e) => setChosenId(e.target.value)}>
              {profiles.map((p) => (
                <option key={p.id} value={p.id}>
                  {p.label}
                </option>
              ))}
            </select>
          </label>
        </p>
      )}
      {error && <p role="alert" className="error">Could not read file: {error}</p>}
      {loaded && compat && (
        <div>
          <h3>{loaded.fileName}</h3>
          <table>
            <tbody>
              <tr><th align="left">Kind</th><td>{loaded.patch.kind}</td></tr>
              <tr><th align="left">Name</th><td>{loaded.patch.name || '(none)'}</td></tr>
              <tr><th align="left">Form</th><td>{loaded.patch.form.formType}</td></tr>
              <tr><th align="left">Sample rate</th><td>{loaded.patch.comm.sampleRate} Hz</td></tr>
              <tr><th align="left">Bits</th><td>{loaded.patch.comm.bitsPerSample}</td></tr>
              <tr><th align="left">Channels</th><td>{loaded.patch.comm.channels}</td></tr>
              <tr><th align="left">Compression</th><td>{loaded.patch.comm.compression}</td></tr>
              <tr><th align="left">Frames</th><td>{loaded.patch.comm.frames}</td></tr>
              <tr><th align="left">Chunks</th><td>{loaded.patch.form.chunks.map((c) => `${c.id} (${c.data.length})`).join(', ')}</td></tr>
              <tr><th align="left">Size</th><td>{loaded.sizeBytes} bytes</td></tr>
              <tr><th align="left">Metadata canonical</th><td>{loaded.patch.op1Canonical ? 'yes' : 'no (an edit would re-serialise it)'}</td></tr>
              <tr><th align="left">{profile.label}</th><td>{compat.level}{compat.reason ? `: ${compat.reason}` : ''}</td></tr>
            </tbody>
          </table>
          <button type="button" onClick={play}>Play</button>
          <h4>Metadata</h4>
          <pre>{loaded.patch.meta ? JSON.stringify(loaded.patch.meta, null, 2) : 'no op-1 metadata'}</pre>
        </div>
      )}
    </section>
  );
}
