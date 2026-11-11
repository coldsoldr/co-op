import { useEffect, useId, useRef, useState, type ChangeEvent } from 'react';
import manual from '../../../docs/coop-manual.md?raw';
import type { BuildStep } from '../core/firmware/build';
import type { WorkerMessage } from '../core/firmware/build.worker';
import { COOP_RELEASE } from '../core/firmware/release';
import { formatSize } from './format';
import { Markdown } from './Markdown';
import { saveBlob } from './download';

const TE_DOWNLOADS = 'https://teenage.engineering/downloads/op-1';

const STEPS: { step: BuildStep; label: string }[] = [
  { step: 'reading', label: 'Unpack your firmware' },
  { step: 'checking', label: 'Check it is stock R246' },
  { step: 'patching', label: `Patch in coop ${COOP_RELEASE.tag}` },
  { step: 'packing', label: 'Pack the new firmware' },
  { step: 'verifying', label: 'Check the package is safe to flash' },
];

type State =
  | { kind: 'idle' }
  | { kind: 'building'; step: BuildStep; fraction?: number }
  | { kind: 'done'; bytes: Uint8Array; fileName: string; sha256: string; stockFiles: number }
  | { kind: 'failed'; message: string };

const base = (): string => import.meta.env.BASE_URL;

/** Build firmware: turns the owner's own stock op1_246.op1 into a coop package, in the browser. */
export function BuildFirmware() {
  const [state, setState] = useState<State>({ kind: 'idle' });
  const worker = useRef<Worker | null>(null);
  const input = useRef<HTMLInputElement>(null);
  const progressId = useId();

  useEffect(() => () => worker.current?.terminate(), []);

  function build(file: File): void {
    worker.current?.terminate();
    setState({ kind: 'building', step: 'reading' });
    void file.arrayBuffer().then((stock) => {
      const w = new Worker(new URL('../core/firmware/build.worker.ts', import.meta.url), { type: 'module' });
      worker.current = w;
      w.onmessage = (event: MessageEvent<WorkerMessage>) => {
        const message = event.data;
        if (message.type === 'progress') {
          setState(message.fraction === undefined ? { kind: 'building', step: message.step } : { kind: 'building', step: message.step, fraction: message.fraction });
        } else if (message.type === 'done') {
          setState({ kind: 'done', ...message.result });
          w.terminate();
        } else {
          setState({ kind: 'failed', message: message.message });
          w.terminate();
        }
      };
      w.onerror = () => setState({ kind: 'failed', message: 'The build stopped unexpectedly. Reload the page and try again.' });
      w.postMessage({ stock, deltaUrl: `${base()}${COOP_RELEASE.delta}` }, [stock]);
    });
  }

  function onFile(event: ChangeEvent<HTMLInputElement>): void {
    const file = event.target.files?.[0];
    event.target.value = '';
    if (file) build(file);
  }

  const building = state.kind === 'building';
  const at = building ? STEPS.findIndex((s) => s.step === state.step) : state.kind === 'done' ? STEPS.length : -1;

  return (
    <div className="build-firmware">
      <h2>Build coop firmware</h2>
      <p>
        coop is a custom firmware for the original OP-1, built over Teenage Engineering’s own R246. co-op holds no TE
        firmware: you give it your copy of <code>op1_246.op1</code> (from{' '}
        <a href={TE_DOWNLOADS} target="_blank" rel="noopener noreferrer">
          TE’s OP-1 downloads
        </a>
        ), and this page patches coop {COOP_RELEASE.tag} into it. Everything happens in your browser; nothing is uploaded.
      </p>
      <p className="notice">
        The package never contains <code>te-boot.ldr</code>, so the bootloader is never touched: if anything goes wrong, hold
        COM at power-on and flash stock again. Back up your tape and sounds before you flash. Read the manual below first.
      </p>

      <p>
        <button type="button" className="primary" disabled={building} onClick={() => input.current?.click()}>
          Choose op1_246.op1
        </button>
        <input ref={input} type="file" accept=".op1" hidden aria-label="Your stock op1_246.op1" onChange={onFile} />
      </p>

      {state.kind !== 'idle' && (
        <ol className="build-steps" aria-label="Build steps">
          {STEPS.map((s, i) => (
            <li key={s.step} data-state={i < at ? 'done' : i === at && building ? 'working' : state.kind === 'failed' && i === at ? 'failed' : 'waiting'}>
              {s.label}
              {i < at && <span className="visually-hidden"> (done)</span>}
            </li>
          ))}
        </ol>
      )}

      {building && (
        <p role="status" id={progressId}>
          {STEPS.find((s) => s.step === state.step)?.label}…
          {state.step === 'packing' && ' This is the long part: a minute or more.'}
          {state.fraction !== undefined && (
            <progress aria-labelledby={progressId} max={1} value={state.fraction}>
              {Math.round(state.fraction * 100)} %
            </progress>
          )}
        </p>
      )}

      {state.kind === 'failed' && (
        <p role="alert" className="error">
          {state.message}
        </p>
      )}

      {state.kind === 'done' && (
        <div className="build-result">
          <p role="status">
            Built <strong>{state.fileName}</strong> ({formatSize(state.bytes.length)}): coop {COOP_RELEASE.tag}’s tested
            OP1_vdk.ldr, your {state.stockFiles} stock content files, and no te-boot.ldr. It passed the same checks as the
            owner’s builds.
          </p>
          <p>
            <button
              type="button"
              className="primary"
              onClick={() => saveBlob(new Blob([state.bytes as Uint8Array<ArrayBuffer>], { type: 'application/octet-stream' }), state.fileName)}
            >
              Download {state.fileName}
            </button>
          </p>
          <p className="muted mono">sha256 {state.sha256}</p>
          <h3>Flash it</h3>
          <ol>
            <li>Switch the OP-1 off. Hold COM and switch it on: the te-boot menu appears.</li>
            <li>Choose 1. Upload firmware. The OP-1 appears on your computer as a drive.</li>
            <li>Copy {state.fileName} onto it and eject it. te-boot checks and writes the firmware; follow the screen.</li>
            <li>Switch the OP-1 on normally: coop’s boot picture shows for about two seconds.</li>
          </ol>
        </div>
      )}

      <section className="build-manual" aria-label="The coop manual">
        <Markdown source={manual} folderUrl={`${base()}`} headingOffset={1} />
      </section>
    </div>
  );
}
