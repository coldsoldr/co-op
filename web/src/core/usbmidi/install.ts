/**
 * Installs hundekraft plugin files on an OP-1 over its USB MIDI port (the kennel update protocol;
 * see docs/research/2026-10-29-hundekraft-usb-and-plugins.md). Everything that can refuse is
 * checked before the first write: the paths, the files themselves, the firmware's capabilities and
 * what is already installed. Then ABORT clears the staging area, each file goes over as BEGIN, DATA
 * and END, and COMMIT asks the user to press T1 (install and restart) or T2 (cancel) on the OP-1.
 *
 * Only `plugins/<id>.knl` and `plugins/<id>.op1p` are ever sent. co-op never sends OFF_SET. DELETE
 * and RESTART only remove a plugin the person chose, and the OP-1 asks for T1 first.
 */
import { checkKnl, checkOp1p, idFromSavedName } from '@co-op/shared/plugins';
import { Command, MAX_DATA, Status, crc32, readU32le, statusName, u32le, type Reply } from './framing';
import { sleep } from '../util';
import { parseInventory, type InventoryRecord } from './inventory';
import { NoAnswerError, createSession, parseHello, type MidiPort, type Session } from './session';

/** Where hundekraft plugins are removed, and where the firmware that installs them comes from. */
export const WORKSHOP_URL = 'https://hundekraft.github.io/op1-workshop/';

/** The only paths co-op sends: a plugin id and its extension, in plugins/. */
const PLUGIN_PATH = /^plugins\/([a-z0-9_-]{1,15})\.(knl|op1p)$/;

export const CONTROL_TIMEOUT_MS = 10_000;
export const DATA_TIMEOUT_MS = 3_000;
export const TRIES = 5;
/** How often COMMIT asks again while the OP-1 waits for T1 or T2. */
export const POLL_MS = 300;

export interface PluginFile {
  /** `plugins/<id>.knl` or `plugins/<id>.op1p`. */
  path: string;
  bytes: Uint8Array;
}

export interface InstallOptions {
  /** Bytes the OP-1 has confirmed so far, of all files. */
  onProgress?: (sent: number, total: number) => void;
  /**
   * Called once, just before the first COMMIT goes out: from then on the OP-1 shows its prompt (T1
   * installs and restarts, T2 cancels). The caller should keep the install running until one of
   * them is pressed: a prompt left without a host never applies anything, but after a T1 on it
   * the OP-1 keeps the prompt up and applies the next COMMIT it gets, from any host, at once.
   */
  onPrompt?: () => void;
  /** Stops at the next step; the install rejects with the signal's reason. */
  signal?: AbortSignal;
  /** Per reply, for HELLO, ABORT, BEGIN, END and COMMIT, which touch the OP-1's file system. */
  controlTimeoutMs?: number;
  dataTimeoutMs?: number;
  /** Per reply, for the inventory pages (when a port, not a session, is given). */
  readTimeoutMs?: number;
  tries?: number;
  pollMs?: number;
}

/**
 * `accepted`: the OP-1 answered COMMIT with ok and restarts to install. `unconfirmed`: COMMIT was
 * sent and then the OP-1 went quiet, which is what a restart after T1 looks like when its ok was
 * lost. That holds even when no "wait key" came back first: the first COMMIT may have raised the
 * prompt with every reply lost, and the prompt must not be left behind as if nothing was asked. A
 * fresh check after the restart tells.
 */
export type InstallOutcome = 'accepted' | 'unconfirmed';

/** T2 on the OP-1's prompt: nothing was installed. */
export class CancelledOnOp1Error extends Error {
  constructor(what = 'Nothing was installed.') {
    super(`Cancelled on the OP-1 (T2). ${what}`);
    this.name = 'CancelledOnOp1Error';
  }
}

const NOTHING_INSTALLED = 'Nothing was installed.';
const NOTHING_REMOVED = 'Nothing was removed.';

export const NO_ANSWER = `The OP-1 did not answer. Installing plugins over USB needs hundekraft 3.0.3 or later; TE’s own firmware, op1r and hundekraft 2.7 or older do not answer. If the OP-1 is restarting, wait for the tape screen and try again. Update hundekraft with the hundekraft workshop: ${WORKSHOP_URL}`;

export const NO_PLUGIN_CAPABILITY = `This OP-1’s firmware cannot install plugins over USB: it does not report its plugins or take plugin files. Update to hundekraft 3.0.3 or later with the hundekraft workshop: ${WORKSHOP_URL}`;

/** The refusal for a plugin that is already there; `found` says where. */
export const alreadyInstalled = (id: string, found: string): string =>
  `${id} is already on the OP-1 (${found}). Remove it first in the hundekraft workshop (${WORKSHOP_URL}), let the OP-1 restart, then install it here: replacing a plugin in place can hang the restart.`;

const stoppedAnswering = (what: string): string =>
  `The OP-1 stopped answering ${what}. ${NOTHING_INSTALLED} Check the USB cable and try again.`;

const isSession = (target: MidiPort | Session): target is Session => 'ask' in target;

/** Checks everything that needs no device: the paths and the files. Returns the ids, in order. */
function checkFiles(files: readonly PluginFile[]): string[] {
  if (files.length === 0) throw new Error('There is no plugin file to install.');
  const ids: string[] = [];
  for (const { path, bytes } of files) {
    const match = PLUGIN_PATH.exec(path);
    if (!match) throw new Error(`${path}: co-op only installs plugin files, as plugins/<id>.knl or plugins/<id>.op1p.`);
    const [, id = '', ext] = match;
    if (ids.includes(id)) throw new Error(`${id} is in the list twice; install one file per plugin.`);
    const check = ext === 'knl' ? checkKnl(bytes) : checkOp1p(bytes);
    if (!check.info) throw new Error(`${path} is not a plugin the OP-1 can load: ${check.problems.join('; ')}.`);
    const saved = check.info.savedName ?? '';
    // The OP-1 knows a plugin by the id its saved name gives; a file named otherwise would slip
    // past the check for one already installed.
    if (idFromSavedName(saved) !== id) {
      throw new Error(`${path}: the file’s saved name "${saved}" gives the id ${idFromSavedName(saved)}, not ${id}.`);
    }
    ids.push(id);
  }
  return ids;
}

/** The inventory record that means `id` is already on the OP-1, if any. Built-ins do not count. */
export function installedAs(id: string, records: readonly InventoryRecord[]): InventoryRecord | undefined {
  const paths = ['plugins', 'content'].flatMap((dir) => [`${dir}/${id}.knl`, `${dir}/${id}.op1p`]);
  return records.find(
    (r) => paths.includes(r.path) || (r.name === id && (r.source === 'knl' || r.source === 'op1p')),
  );
}

/**
 * Removes one plugin file: the OP-1 asks for T1 or T2, then restarts and deletes it. Resolves as
 * installPlugins does, and rejects the same ways. The port stays open; the caller closes it.
 */
export async function removePlugin(target: MidiPort | Session, path: string, options: InstallOptions = {}): Promise<InstallOutcome> {
  const { signal, onPrompt } = options;
  if (!PLUGIN_PATH.test(path)) throw new Error(`${path}: co-op only removes plugin files, as plugins/<id>.knl or plugins/<id>.op1p.`);
  const control = { timeoutMs: options.controlTimeoutMs ?? CONTROL_TIMEOUT_MS, tries: 1 };
  const tries = options.tries ?? TRIES;
  const pollMs = options.pollMs ?? POLL_MS;
  const session = isSession(target)
    ? target
    : createSession(target, { timeoutMs: options.readTimeoutMs ?? DATA_TIMEOUT_MS, ...(signal ? { signal } : {}) });

  async function ask(cmd: number, payload?: Uint8Array): Promise<Reply> {
    for (let attempt = 0; attempt < tries; attempt++) {
      signal?.throwIfAborted();
      try {
        return await session.ask(cmd, payload, control);
      } catch (e) {
        if (!(e instanceof NoAnswerError)) throw e;
      }
    }
    throw new NoAnswerError(cmd, tries);
  }

  const hello = await ask(Command.HELLO).then(parseHello, (e: unknown) => {
    throw e instanceof NoAnswerError ? new Error(NO_ANSWER, { cause: e }) : e;
  });
  if (!hello.plugins || !hello.inventory) throw new Error(NO_PLUGIN_CAPABILITY);
  const inventory = parseInventory(await session.readInventory());
  if (!inventory.records.some((r) => r.path === path)) throw new Error(`${path} is not on the OP-1.`);

  const cleared = await ask(Command.ABORT);
  if (cleared.status !== Status.OK) throw new Error(`The OP-1 refused to clear its staging area (${statusName(cleared.status)}). ${NOTHING_REMOVED}`);
  const deleted = await ask(Command.DELETE, Uint8Array.from([...new TextEncoder().encode(path), 0]));
  if (deleted.status !== Status.OK) throw new Error(`The OP-1 refused to remove ${path} (${statusName(deleted.status)}). ${NOTHING_REMOVED}`);

  onPrompt?.();
  let prompted = false;
  for (;;) {
    let reply: Reply;
    try {
      reply = await ask(Command.RESTART);
    } catch (e) {
      if (signal?.aborted) throw e;
      if (prompted || e instanceof NoAnswerError) return 'unconfirmed';
      throw e;
    }
    if (reply.status === Status.OK) return 'accepted';
    if (reply.status === Status.STATE && prompted) {
      await ask(Command.ABORT).catch(() => undefined);
      throw new CancelledOnOp1Error(NOTHING_REMOVED);
    }
    if (reply.status !== Status.WAIT_KEY) throw new Error(`The OP-1 refused the restart (${statusName(reply.status)}). ${NOTHING_REMOVED}`);
    prompted = true;
    await sleep(pollMs, signal);
  }
}

/**
 * Installs `files` and resolves once COMMIT is through (see InstallOutcome). Rejects with a
 * message for the user on every refusal, on silence, on T2 (CancelledOnOp1Error) and on abort.
 * The port stays open; the caller closes it.
 */
export async function installPlugins(
  target: MidiPort | Session,
  files: readonly PluginFile[],
  options: InstallOptions = {},
): Promise<InstallOutcome> {
  const { signal, onProgress, onPrompt } = options;
  const control = { timeoutMs: options.controlTimeoutMs ?? CONTROL_TIMEOUT_MS, tries: 1 };
  const dataAsk = { timeoutMs: options.dataTimeoutMs ?? DATA_TIMEOUT_MS, tries: 1 };
  const tries = options.tries ?? TRIES;
  const pollMs = options.pollMs ?? POLL_MS;

  // 1. The paths and the files, before anything is sent.
  const ids = checkFiles(files);
  const session = isSession(target)
    ? target
    : createSession(target, { timeoutMs: options.readTimeoutMs ?? DATA_TIMEOUT_MS, ...(signal ? { signal } : {}) });

  /**
   * Sends one command, resending after a lost reply up to `tries` times. `resent` says whether a
   * reply was lost on the way, which matters for the commands that are not idempotent.
   */
  async function ask(cmd: number, payload?: Uint8Array): Promise<{ reply: Reply; resent: boolean }> {
    for (let attempt = 0; attempt < tries; attempt++) {
      signal?.throwIfAborted();
      try {
        return { reply: await session.ask(cmd, payload, control), resent: attempt > 0 };
      } catch (e) {
        if (!(e instanceof NoAnswerError)) throw e;
      }
    }
    throw new NoAnswerError(cmd, tries);
  }

  // 2. The firmware must take plugin files and report what it has installed.
  const hello = await ask(Command.HELLO).then(
    ({ reply }) => parseHello(reply),
    (e: unknown) => {
      throw e instanceof NoAnswerError ? new Error(NO_ANSWER, { cause: e }) : e;
    },
  );
  if (!hello.plugins || !hello.inventory) throw new Error(NO_PLUGIN_CAPABILITY);

  // 3. Nothing already installed under the same id.
  const text = await session.readInventory().catch((e: unknown) => {
    throw e instanceof NoAnswerError
      ? new Error('The OP-1 stopped answering while listing its plugins. Nothing was changed. Try again.', { cause: e })
      : e;
  });
  const inventory = parseInventory(text);
  for (const id of ids) {
    const found = installedAs(id, inventory.records);
    if (found) throw new Error(alreadyInstalled(id, `${found.path || found.name}, ${found.state ?? 'unknown'}`));
  }

  // 4. Every check passed: from here on the OP-1's staging area changes.
  const total = files.reduce((n, f) => n + f.bytes.length, 0);
  let done = 0;
  onProgress?.(0, total);
  try {
    const { reply } = await ask(Command.ABORT);
    if (reply.status !== Status.OK) throw new Error(`The OP-1 refused to clear its staging area (${statusName(reply.status)}). ${NOTHING_INSTALLED}`);
    for (const file of files) {
      await sendFile(file, (sent) => onProgress?.(done + sent, total));
      done += file.bytes.length;
    }
  } catch (e) {
    if (e instanceof NoAnswerError) throw new Error(stoppedAnswering('during the transfer'), { cause: e });
    throw e;
  }

  // 5. COMMIT: the OP-1 asks for T1 or T2 and answers "wait key" until one is pressed.
  onPrompt?.();
  let prompted = false;
  for (;;) {
    let reply: Reply;
    try {
      reply = (await ask(Command.COMMIT)).reply;
    } catch (e) {
      if (signal?.aborted) throw e;
      // After the prompt, silence (or a port that went away) is most likely the restart after T1.
      // Silence after a COMMIT was sent is the same: the OP-1 may be showing its prompt with every
      // reply lost, so the install goes on to wait for the restart rather than fail as if nothing
      // was asked.
      if (prompted || e instanceof NoAnswerError) return 'unconfirmed';
      throw e;
    }
    if (reply.status === Status.OK) return 'accepted';
    if (reply.status === Status.STATE && prompted) throw new CancelledOnOp1Error();
    if (reply.status !== Status.WAIT_KEY) throw new Error(`The OP-1 refused the install (${statusName(reply.status)}). ${NOTHING_INSTALLED}`);
    prompted = true;
    await sleep(pollMs, signal);
  }

  /** BEGIN, DATA in pieces of at most MAX_DATA with one in flight, END. */
  async function sendFile({ path, bytes }: PluginFile, progress: (sent: number) => void): Promise<void> {
    const header = [...u32le(bytes.length), ...u32le(crc32(bytes)), ...new TextEncoder().encode(path), 0];
    const begin = await ask(Command.BEGIN, Uint8Array.from(header));
    // BEGIN answers "state" while a file is open. After the ABORT above, and with every earlier
    // file closed by END, the only file that can be open is this one: a resent BEGIN whose first
    // try opened it and lost its reply (the workshop reads it the same way).
    const opened = begin.reply.status === Status.OK || (begin.reply.status === Status.STATE && begin.resent);
    if (!opened) throw new Error(`The OP-1 refused ${path} (${statusName(begin.reply.status)}). ${NOTHING_INSTALLED}`);

    let acked = 0;
    let quiet = 0;
    while (acked < bytes.length) {
      signal?.throwIfAborted();
      const piece = bytes.subarray(acked, acked + MAX_DATA);
      let reply: Reply;
      try {
        reply = await session.ask(Command.DATA, Uint8Array.from([...u32le(acked), ...piece]), dataAsk);
      } catch (e) {
        if (!(e instanceof NoAnswerError)) throw e;
        // Lost: send again from the last count the OP-1 confirmed.
        if (++quiet >= tries) throw new Error(stoppedAnswering(`while receiving ${path}`), { cause: e });
        continue;
      }
      if (reply.status !== Status.OK && reply.status !== Status.OFFSET) {
        throw new Error(`The OP-1 refused part of ${path} (${statusName(reply.status)}). ${NOTHING_INSTALLED}`);
      }
      // Both carry the bytes written so far; "offset" means a gap (a reply was lost, so the OP-1
      // is ahead of us, or a message went missing): carry on from its count.
      const written = readU32le(reply.payload);
      if (written > bytes.length) throw new Error(`The OP-1 reports more of ${path} than was sent. ${NOTHING_INSTALLED}`);
      // A gap that does not move forward counts as a lost reply, so a confused OP-1 cannot loop us.
      if (written <= acked && ++quiet >= tries) {
        throw new Error(`The OP-1 kept asking for the same part of ${path}. ${NOTHING_INSTALLED} Try again.`);
      }
      if (written > acked) quiet = 0;
      acked = written;
      progress(acked);
    }

    const end = await ask(Command.END);
    if (end.reply.status === Status.OK) return;
    if (end.reply.status === Status.STATE && end.resent) {
      // END closes the file whether its check passes or not, so "state" on a resend cannot tell
      // whether the first END staged the file.
      throw new Error(`The OP-1 did not confirm that ${path} arrived whole. ${NOTHING_INSTALLED} Try again.`);
    }
    if (end.reply.status === Status.CRC) throw new Error(`${path} arrived damaged (its CRC-32 does not match). ${NOTHING_INSTALLED} Try again.`);
    if (end.reply.status === Status.SIZE) throw new Error(`${path} arrived with the wrong size. ${NOTHING_INSTALLED} Try again.`);
    throw new Error(`The OP-1 refused ${path} at the end (${statusName(end.reply.status)}). ${NOTHING_INSTALLED}`);
  }
}
