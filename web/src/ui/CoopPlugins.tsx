import { useRef, useState } from 'react';
import { checkCoop, idFromSavedName, type HostedPluginEntry, type PluginIndex } from '@co-op/shared/plugins';
import { fetchPluginBytes } from '../core/plugins/catalogue';
import {
  clipSeconds,
  coopMemoryOf,
  formatKb,
  largestRows,
  planMemory,
  shortNote,
  type DeviceMemoryReport,
  type KnownCoopFile,
  type MemoryPlan,
} from '../core/plugins/memory';
import {
  CoopCaps,
  coopDeletePlugin,
  coopHello,
  coopPutPlugin,
  coopSetPlugin,
  readCoopInventory,
  readCoopMemoryReport,
  type CoopHello,
} from '../core/usbmidi/coop';
import { deviceFilesOf, deviceReportOf, updateCoopPlugins, updateSummary } from '../core/usbmidi/coop-update';
import type { Inventory, InventoryRecord } from '../core/usbmidi/inventory';
import { listUpdates } from '../core/plugins/updates';
import { createSession, type Session } from '../core/usbmidi/session';
import { messageOf } from '../core/util';
import { formatSize } from './format';
import { openOp1Port } from './webmidi';

const base = (): string => import.meta.env.BASE_URL;

export const NOT_COOP =
  'This OP-1 does not answer as coop. Check it runs the coop firmware and is plugged in normally, not in disk mode.';
export const COOP_TOO_OLD =
  'This OP-1 runs coop, but a build without plugin management. Flash coop e3 or later to manage plugins from here.';
export const POWER_CYCLE = 'Power-cycle the OP-1 (switch it off and on) to apply the change, then read it again.';
export const NOTHING_SENT = 'Nothing was sent.';
export const NO_MEMORY_REPORT = 'This OP-1 did not give its own memory figure; the bar shows what co-op adds up.';

/**
 * One conversation with a coop OP-1: open the MIDI port, check HELLO, run `work`, close the port again (Windows lets
 * one program hold a MIDI port at a time). Throws NOT_COOP or COOP_TOO_OLD as plain errors.
 */
export async function withCoop<T>(work: (session: Session, hello: CoopHello) => Promise<T>): Promise<T> {
  const port = await openOp1Port();
  try {
    const session = createSession(port);
    const hello = await coopHello(session).catch(() => null);
    if (!hello) throw new Error(NOT_COOP);
    if (!(hello.caps & CoopCaps.INVENTORY) || !(hello.caps & CoopCaps.FILE_WRITE)) throw new Error(COOP_TOO_OLD);
    return await work(session, hello);
  } finally {
    port.close();
  }
}

const fileOf = (r: InventoryRecord): string => r.path.replace(/^plugins\//, '');
const titleOf = (file: string): string => file.replace(/\.coop$/, '').replaceAll('_', ' ');

const listOf = (files: readonly string[]): string => files.map(titleOf).join(', ');

/** `add` with its share of the plugin memory, or null for bytes the sum cannot read (the OP-1 judges those itself). */
function knownFile(file: string, bytes: Uint8Array): KnownCoopFile | null {
  try {
    return { file, sizeBytes: bytes.length, memory: coopMemoryOf(bytes) };
  } catch {
    return null;
  }
}

const about = (plan: MemoryPlan): string => (plan.approximate ? 'about ' : '');

/**
 * What to ask before sending a file that takes the set on the OP-1 over its plugin memory; null when it fits (or
 * the core has no budget). The file is harmless there: the OP-1 just leaves a plugin out at boot.
 */
export function overBudgetQuestion(name: string, inventory: Inventory, known: readonly KnownCoopFile[], add: KnownCoopFile | null): string | null {
  if (!add) return null;
  const plan = planMemory(deviceFilesOf(inventory), known, { build: inventory.build, report: deviceReportOf(inventory), add });
  if (!plan || plan.level !== 'over') return null;
  const others = largestRows(plan, 4)
    .filter((r) => r.file !== add.file)
    .slice(0, 3)
    .map((r) => `${titleOf(r.file)} (${formatKb(r.frees)})`);
  return [
    `${name} does not fit in the OP-1’s plugin memory. With it, the plugins switched on need ${about(plan)}${formatKb(plan.used)}; the OP-1 has ${formatKb(plan.budget)}.`,
    plan.refused.length > 0
      ? `At the next power cycle the OP-1 would leave out ${listOf(plan.refused)}: it loads the plugins in name order.`
      : 'At the next power cycle the OP-1 would leave out at least one plugin, and not always the new one.',
    ...(shortNote(plan) ? [shortNote(plan) as string] : []),
    ...(others.length > 0 ? [`The largest on the OP-1: ${others.join(', ')}. Remove or switch off one to make room.`] : []),
    'Install anyway? The file does no harm; it loads once there is room.',
  ].join('\n\n');
}

interface CoopInstallProps {
  plugin: HostedPluginEntry;
  /** The catalogue's coop files, to add up the plugin memory of what is on the OP-1. */
  known: readonly KnownCoopFile[];
  /** The inventory read after the install, shared with the rest of the app. */
  onInventory: (inventory: Inventory) => void;
}

/** "Install on OP-1" for a coop plugin from the library: the file goes to /yaffs2/plugins; a power cycle loads it. */
export function CoopInstall({ plugin, known, onInventory }: CoopInstallProps) {
  const [state, setState] = useState<{ busy: boolean; message: string | null; error: boolean }>({ busy: false, message: null, error: false });

  async function install(): Promise<void> {
    setState({ busy: true, message: 'Downloading…', error: false });
    try {
      const bytes = await fetchPluginBytes(base(), plugin);
      const inventory = await withCoop(async (session) => {
        const before = await readCoopInventory(session);
        const there = before.records.some((r) => r.path === `plugins/${plugin.file}`);
        if (there && !window.confirm(`${plugin.name} is already on the OP-1. Replace it?`)) return null;
        const question = overBudgetQuestion(plugin.name, before, known, knownFile(plugin.file, bytes));
        if (question && !window.confirm(question)) return null;
        await coopPutPlugin(session, plugin.file, bytes, {
          onProgress: (n, total) => setState({ busy: true, message: `Sending ${Math.round((100 * n) / total)} %`, error: false }),
        });
        return readCoopInventory(session);
      });
      if (!inventory) {
        setState({ busy: false, message: null, error: false });
        return;
      }
      onInventory(inventory);
      setState({ busy: false, message: `Installed. ${POWER_CYCLE}`, error: false });
    } catch (e) {
      setState({ busy: false, message: messageOf(e), error: true });
    }
  }

  return (
    <>
      <button type="button" onClick={() => void install()} disabled={state.busy}>
        Install on OP-1
      </button>
      {state.message && (
        <p role={state.error ? 'alert' : 'status'} className={state.error ? 'error' : 'muted'}>
          {state.message}
        </p>
      )}
    </>
  );
}

const STATE_TEXT: Record<string, string> = {
  loaded: 'loaded',
  failed: 'failed to load',
  off: 'switched off',
  pending: 'installed, loads after a power cycle',
  skipped: 'skipped',
};

const LEVEL_NOTE = {
  fits: null,
  nearly: 'Nearly full. Everything still loads, but there is little room for another plugin.',
  over: 'Too much for the OP-1’s plugin memory. At the next power cycle at least one plugin will not load. Remove or switch off one to make room.',
} as const;

/** e13: a placement test file on the OP-1 is a developer's state; the owner should see it. */
const PLACE_NOTE: Record<string, string> = {
  file: 'A placement test file is in force on this OP-1.',
  malformed: 'This OP-1 has a placement test file it could not read, and uses the default.',
  marker: 'This OP-1 ignored its placement test file: the last boot with it did not finish.',
};

interface PluginMemoryProps {
  plan: MemoryPlan;
  /** The stereo clipboard's pool in bytes, where the OP-1 reports it (e12). */
  clip?: number | undefined;
  busy: boolean;
  /** Asks the OP-1 for its own figure. */
  onAsk: () => void;
}

/**
 * The plugin memory of the files the next power cycle loads: how full, in three states, with what is loaded now
 * when that differs and what co-op could only guess.
 */
function PluginMemory({ plan, clip, busy, onAsk }: PluginMemoryProps) {
  const percent = Math.round((100 * plan.used) / plan.budget);
  const guessed = plan.rows.filter((r) => r.counted && !r.exact);
  const largest = plan.level === 'over' ? largestRows(plan) : [];
  const loaded = plan.loadedNow;
  // From e13 there are two memories: the bar is the one with the least room, the other gets a line.
  const [shown, other] = plan.pools;
  return (
    <div className="plugin-memory" data-level={plan.level}>
      <div
        className="memory-bar"
        role="meter"
        aria-label="Plugin memory"
        aria-valuemin={0}
        aria-valuemax={100}
        aria-valuenow={Math.min(100, percent)}
        aria-valuetext={`${percent} %, ${about(plan)}${formatKb(plan.used)} of ${formatKb(plan.budget)}`}
      >
        <span className="memory-fill" style={{ width: `${Math.min(100, percent)}%` }} />
        <span className="memory-label">
          Plugin memory
          <span className="muted">
            {' '}
            {about(plan)}
            {formatKb(plan.used).replace(' KB', '')} of {formatKb(plan.budget)}
          </span>
        </span>
        <span className="memory-value">{percent} %</span>
      </div>
      {LEVEL_NOTE[plan.level] && (
        <p className="memory-note">
          {LEVEL_NOTE[plan.level]}
          {shortNote(plan) && <> {shortNote(plan)}</>}
          {plan.refused.length > 0 && <> The OP-1 loads them in name order, so it will leave out {listOf(plan.refused)}.</>}
          {largest.length > 0 && <> The largest: {largest.map((r) => `${titleOf(r.file)} (${formatKb(r.frees)})`).join(', ')}.</>}
        </p>
      )}
      <p className="muted">
        What the plugins switched on take at the next power cycle.
        {other && (
          <>
            {' '}
            The bar is the memory for {shown?.holds}. For {other.holds} the OP-1 has another:{' '}
            <span data-level={other.level}>
              {about(plan)}
              {formatKb(other.used).replace(' KB', '')} of {formatKb(other.budget)}
            </span>
            .
          </>
        )}
        {(loaded.fromDevice || loaded.bytes !== plan.used) && (
          <>
            {' '}
            Loaded now: {loaded.exact ? '' : 'about '}
            {formatKb(loaded.bytes)}
            {loaded.fromDevice ? ', by the OP-1’s own count' : ''}.
          </>
        )}
        {plan.sharedBy && (
          <>
            {' '}
            {formatKb(plan.shared)} of it is working memory the effects share, sized for {titleOf(plan.sharedBy)}.
          </>
        )}
        {guessed.length > 0 && (
          <>
            {' '}
            co-op does not have {guessed.map((r) => r.file).join(', ')}, so what {guessed.length === 1 ? 'it takes' : 'they take'} is an estimate.
          </>
        )}
        {plan.level !== 'over' && plan.positions !== null && plan.positions < 4 && (
          <>
            {' '}
            With this much loaded, only {plan.positions} of the 4 effect positions can run such an effect at once.
          </>
        )}
        {clip !== undefined && <> Stereo tape lift: {clipSeconds(clip)} s.</>}
        {plan.placeSrc && PLACE_NOTE[plan.placeSrc] && (
          <strong>
            {' '}
            {PLACE_NOTE[plan.placeSrc]}
            {plan.place && plan.placeSrc === 'file' && <> ({Object.entries(plan.place).map(([k, v]) => `${k}=${v}`).join(', ')})</>}
          </strong>
        )}
        {!plan.budgetKnown && <> co-op does not know this build’s limit yet; the bar uses the newest it knows.</>}{' '}
        {!(loaded.fromDevice && loaded.exact) && (
          <button type="button" className="link" disabled={busy} onClick={onAsk}>
            Ask the OP-1 for its own figure
          </button>
        )}
      </p>
    </div>
  );
}

interface CoopDeviceProps {
  inventory: Inventory | null;
  /** The plugin catalogue, to see which files on the OP-1 have a newer build; null until it has loaded. */
  index: PluginIndex | null;
  /** The catalogue's coop files, to add up the plugin memory of what is on the OP-1. */
  known: readonly KnownCoopFile[];
  onInventory: (inventory: Inventory | null) => void;
}

/**
 * The plugins on a coop OP-1, read over USB: each file with what the last boot made of it and what it takes of the
 * plugin memory, switched on or off for the next boot, removable; and a .coop file from this computer installed
 * beside them.
 */
export function CoopDevice({ inventory, index, known, onInventory }: CoopDeviceProps) {
  const [busy, setBusy] = useState<string | null>(null);
  const [message, setMessage] = useState<{ text: string; error: boolean } | null>(null);
  const [build, setBuild] = useState<string | null>(null);
  const [changed, setChanged] = useState(false);
  /** Files this page sent: the inventory gives a file that came after the boot no size to know it by. */
  const [sent, setSent] = useState<KnownCoopFile[]>([]);
  /** The OP-1's own memory figure, asked for; it holds until the next power cycle, so a fresh read drops it. */
  const [report, setReport] = useState<DeviceMemoryReport | null>(null);
  const fileInput = useRef<HTMLInputElement>(null);
  const records = (inventory?.records ?? []).filter((r) => r.source === 'coop');
  // An e12 inventory carries the OP-1's own figures; before it they come from the statistics file, when asked for.
  const own = deviceReportOf(inventory) ?? report;
  const plan = inventory ? planMemory(deviceFilesOf(inventory), [...sent, ...known], { build: inventory.build ?? build, report: own }) : null;
  const memoryOf = new Map(plan?.rows.map((r) => [r.file, r]));
  /** Versions sent from here: the OP-1's inventory names the version its boot read until the next power cycle. */
  const [updated, setUpdated] = useState<ReadonlyMap<string, string>>(new Map());
  const list = listUpdates(inventory, index);
  const updates = list.updates.filter((u) => updated.get(u.file) !== u.to);
  const updateOf = new Map(updates.map((u) => [u.file, u]));
  const waitingOf = new Map(list.updates.filter((u) => updated.get(u.file) === u.to).map((u) => [u.file, u]));
  const blockedOf = new Map(list.blocked.map((b) => [b.file, b]));

  /** Replaces every plugin the catalogue has a newer build of (or only `only`), after one question that lists them. */
  async function update(only?: string): Promise<void> {
    setBusy('Talking to the OP-1…');
    setMessage(null);
    try {
      const outcome = await withCoop((session, hello) => {
        setBuild(hello.build);
        return updateCoopPlugins(session, {
          index,
          known: [...sent, ...known],
          already: updated,
          ...(only === undefined ? {} : { only }),
          fetchBytes: (plugin) => fetchPluginBytes(base(), plugin),
          confirm: (question) => window.confirm(question),
          onProgress: setBusy,
        });
      });
      if (outcome.inventory) onInventory(outcome.inventory);
      if (outcome.kind === 'ran') setUpdated((before) => new Map([...before, ...outcome.done.map((u) => [u.file, u.to] as const)]));
      setMessage({ text: updateSummary(outcome), error: outcome.kind === 'ran' && outcome.failed !== null });
    } catch (e) {
      setMessage({ text: messageOf(e), error: true });
    } finally {
      setBusy(null);
    }
  }

  /** `work` returns a note to show, null for none, or false when nothing was changed after all. */
  async function run(work: (session: Session) => Promise<string | null | false>, change: boolean, doing = 'Talking to the OP-1…'): Promise<void> {
    setBusy(doing);
    setMessage(null);
    try {
      const note = await withCoop(async (session, hello) => {
        setBuild(hello.build);
        const text = await work(session);
        onInventory(await readCoopInventory(session));
        return text;
      });
      if (change && note !== false) setChanged(true);
      setMessage(note === false ? { text: NOTHING_SENT, error: false } : note ? { text: note, error: false } : null);
    } catch (e) {
      setMessage({ text: messageOf(e), error: true });
    } finally {
      setBusy(null);
    }
  }

  async function installFile(file: File): Promise<void> {
    const bytes = new Uint8Array(await file.arrayBuffer());
    const check = checkCoop(bytes);
    if (!check.info) {
      setMessage({ text: `${file.name} is not a coop plugin: ${check.problems.join('; ')}`, error: true });
      return;
    }
    const target = `${idFromSavedName(check.info.savedName ?? '')}.coop`;
    if (records.some((r) => fileOf(r) === target) && !window.confirm(`${target} is already on the OP-1. Replace it?`)) return;
    const add = knownFile(target, bytes);
    await run(async (session) => {
      const question = overBudgetQuestion(target, await readCoopInventory(session), [...sent, ...known], add);
      if (question && !window.confirm(question)) return false;
      await coopPutPlugin(session, target, bytes);
      if (add) setSent((list) => [add, ...list.filter((k) => k.file !== target)]);
      return `Installed ${target} (${formatSize(bytes.length)}).`;
    }, true);
  }

  return (
    <div className="panel coop-device" aria-labelledby="coop-device-heading">
      <h3 id="coop-device-heading">On your OP-1</h3>
      <p className="muted">
        Plug the OP-1 in by USB normally (not in disk mode). co-op reads its plugins over MIDI; every change applies at
        the next power cycle.
      </p>
      <div className="toolbar">
        <button
          type="button"
          onClick={() => {
            setReport(null);
            void run(async () => null, false);
          }}
          disabled={busy !== null}
        >
          {inventory ? 'Read again' : 'Read plugins from the OP-1'}
        </button>
        <button type="button" onClick={() => fileInput.current?.click()} disabled={busy !== null}>
          Install a .coop file…
        </button>
        <input
          ref={fileInput}
          type="file"
          accept=".coop"
          hidden
          onChange={(e) => {
            const file = e.target.files?.[0];
            e.target.value = '';
            if (file) void installFile(file);
          }}
        />
        {updates.length > 0 && (
          <button type="button" onClick={() => void update()} disabled={busy !== null}>
            Update all ({updates.length})
          </button>
        )}
        {build && <span className="pill quiet">coop {build}</span>}
      </div>
      {busy && <p role="status">{busy}</p>}
      {message && (
        <p role={message.error ? 'alert' : 'status'} className={message.error ? 'error' : 'muted'}>
          {message.text}
        </p>
      )}
      {changed && <p className="notice">{POWER_CYCLE}</p>}
      {inventory && records.length === 0 && <p>No plugin files on the OP-1.</p>}
      {plan && records.length > 0 && (
        <PluginMemory
          plan={plan}
          clip={own?.clip}
          busy={busy !== null}
          onAsk={() =>
            void run(
              async (session) => {
                const asked = await readCoopMemoryReport(session);
                setReport(asked);
                return asked ? null : NO_MEMORY_REPORT;
              },
              false,
              'Asking the OP-1 for its memory figure. This can take ten seconds…',
            )
          }
        />
      )}
      {records.length > 0 && (
        <div className="table-scroll">
          <table className="library coop-plugins">
            <caption className="visually-hidden">Plugin files on the OP-1</caption>
            <thead>
              <tr>
                <th scope="col">Plugin</th>
                <th scope="col">Kind</th>
                <th scope="col">Version</th>
                {plan && <th scope="col">Memory</th>}
                <th scope="col">Now</th>
                <th scope="col">Next boot</th>
                <th scope="col">
                  <span className="visually-hidden">Actions</span>
                </th>
              </tr>
            </thead>
            <tbody>
              {records.map((r) => {
                const file = fileOf(r);
                const memory = memoryOf.get(file);
                return (
                  <tr key={file}>
                    <th scope="row">
                      {r.name.replaceAll('_', ' ')}
                      <div className="mono muted">{file}</div>
                    </th>
                    <td>{r.kind ?? '?'}</td>
                    <td>
                      {r.version || '–'}
                      {updateOf.has(file) && (
                        <div className="muted">
                          update available: {updateOf.get(file)?.from ?? '?'} → {updateOf.get(file)?.to}
                        </div>
                      )}
                      {waitingOf.has(file) && <div className="muted">{waitingOf.get(file)?.to} sent, loads after a power cycle</div>}
                      {blockedOf.has(file) && (
                        <div className="muted">
                          {blockedOf.get(file)?.to} needs {blockedOf.get(file)?.needs}
                        </div>
                      )}
                    </td>
                    {plan && (
                      <td className={memory?.counted ? 'mono' : 'mono muted'}>
                        {!memory || (!memory.exact && memory.bytes === 0) ? '?' : `${memory.exact ? '' : 'about '}${formatKb(memory.bytes)}`}
                      </td>
                    )}
                    <td className={r.state === 'failed' ? 'error' : undefined}>
                      {STATE_TEXT[r.state ?? ''] ?? 'unknown'}
                      {r.reason && <div className="muted">{r.reason}</div>}
                    </td>
                    <td>{r.enabled === false ? 'off' : 'on'}</td>
                    <td>
                      {updateOf.has(file) && (
                        <>
                          <button type="button" disabled={busy !== null} onClick={() => void update(file)}>
                            Update
                          </button>{' '}
                        </>
                      )}
                      <button
                        type="button"
                        disabled={busy !== null}
                        onClick={() =>
                          void run(async (session) => {
                            await coopSetPlugin(session, file, r.enabled === false);
                            return null;
                          }, true)
                        }
                      >
                        {r.enabled === false ? 'Switch on' : 'Switch off'}
                      </button>{' '}
                      <button
                        type="button"
                        disabled={busy !== null}
                        onClick={() => {
                          if (!window.confirm(`Remove ${file} from the OP-1? Sounds that use it play the stock engine or filter until it is back.`)) return;
                          void run(async (session) => {
                            await coopDeletePlugin(session, file);
                            return `Removed ${file}.`;
                          }, true);
                        }}
                      >
                        Remove
                      </button>
                    </td>
                  </tr>
                );
              })}
            </tbody>
            {plan && plan.shared > 0 && (
              <tfoot>
                <tr>
                  <th scope="row" colSpan={3}>
                    Working memory the effects share
                  </th>
                  <td className="mono">{formatKb(plan.shared)}</td>
                  <td colSpan={3} />
                </tr>
              </tfoot>
            )}
          </table>
        </div>
      )}
    </div>
  );
}
