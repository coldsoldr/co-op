import { useState, type DragEvent, type ChangeEvent } from 'react';
import { readPatch, decodePcm, type Patch } from '../core/codec/patch';
import { op1Stock } from '../core/profiles/op1-stock';

interface Loaded {
  fileName: string;
  sizeBytes: number;
  patch: Patch;
}

export function Inspector() {
  const [loaded, setLoaded] = useState<Loaded | null>(null);
  const [error, setError] = useState<string | null>(null);

  async function load(file: File) {
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
    const pcm = decodePcm(loaded.patch);
    const context = new AudioContext({ sampleRate: loaded.patch.comm.sampleRate });
    const buffer = context.createBuffer(1, pcm.length, loaded.patch.comm.sampleRate);
    buffer.getChannelData(0).set(pcm);
    const source = context.createBufferSource();
    source.buffer = buffer;
    source.connect(context.destination);
    source.start();
  }

  const compat = loaded ? op1Stock.compatibility(loaded.patch) : null;

  return (
    <section>
      <h2>Patch inspector</h2>
      <div
        onDrop={onDrop}
        onDragOver={(e) => e.preventDefault()}
        style={{ border: '2px dashed #999', padding: 24, borderRadius: 8, textAlign: 'center' }}
      >
        <p>Drop an OP-1 .aif patch here, or</p>
        <input type="file" accept=".aif,.aiff" onChange={onPick} />
      </div>
      {error && <p role="alert" style={{ color: '#b00020' }}>Could not read file: {error}</p>}
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
              <tr><th align="left">Stock OP-1</th><td>{compat.level}{compat.reason ? `: ${compat.reason}` : ''}</td></tr>
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
