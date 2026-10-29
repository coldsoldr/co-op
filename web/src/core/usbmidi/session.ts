/**
 * A conversation with a hundekraft 3.x OP-1 over its USB MIDI port: one message in flight, each
 * reply matched to its request by command and seq, a resend with a fresh seq after a timeout.
 * Pure: the port comes from the caller (Web MIDI in the app, a fake device in tests).
 */
import {
  Command,
  SEQ_MASK,
  Status,
  SysexAssembler,
  buildMessage,
  parseReply,
  readU32le,
  statusName,
  u32le,
  type Reply,
} from './framing';

/** A MIDI connection to the OP-1 that carries SysEx both ways. */
export interface MidiPort {
  send(bytes: Uint8Array): void;
  /** Sets the one listener for incoming bytes; a run may hold part of a message, or several. */
  onSysex(cb: (bytes: Uint8Array) => void): void;
  close(): void;
}

export interface AskOptions {
  /** How long to wait for each reply. */
  timeoutMs?: number;
  /** How many times to send, each with a new seq, before giving up. */
  tries?: number;
}

export interface SessionOptions extends AskOptions {
  /** Stops a wait at once; the ask rejects with the signal's reason. */
  signal?: AbortSignal;
}

export const DEFAULT_TIMEOUT_MS = 3000;
export const DEFAULT_TRIES = 5;

/** The most text one LIST or OFF_GET reply carries. */
export const PAGE_BYTES = 64;

const COMMAND_NAMES: Record<number, string> = Object.fromEntries(Object.entries(Command).map(([name, cmd]) => [cmd, name]));
const commandName = (cmd: number): string => COMMAND_NAMES[cmd] ?? `command ${cmd}`;

/** No reply came after every try: the firmware does not speak the protocol, or the OP-1 is busy. */
export class NoAnswerError extends Error {
  constructor(
    readonly cmd: number,
    readonly tries: number,
  ) {
    super(`the OP-1 did not answer ${commandName(cmd)} (${tries} ${tries === 1 ? 'try' : 'tries'})`);
    this.name = 'NoAnswerError';
  }
}

/** The OP-1 answered with a status other than ok. */
export class RefusedError extends Error {
  constructor(
    readonly cmd: number,
    readonly status: number,
    what: string,
  ) {
    super(`the OP-1 refused ${what} (${statusName(status)})`);
    this.name = 'RefusedError';
  }
}

/** What HELLO reports. */
export interface Hello {
  /** The protocol version. */
  version: number;
  /** LIST and OFF_GET are there (capability bit 0). */
  inventory: boolean;
  /** Plugin files can be sent and deleted (capability bit 1). */
  plugins: boolean;
}

/** Reads a HELLO reply. Throws RefusedError on a status other than ok. */
export function parseHello(reply: Reply): Hello {
  if (reply.status !== Status.OK) throw new RefusedError(reply.cmd, reply.status, 'HELLO');
  // Builds without the capabilities byte send only the version.
  const caps = reply.payload[4] ?? 0;
  return { version: readU32le(reply.payload), inventory: (caps & 1) !== 0, plugins: (caps & 2) !== 0 };
}

/** Which switched-off list OFF_GET reads. */
export type OffList = 'stored' | 'boot';

export interface Session {
  /**
   * Sends one command and resolves with its reply, whatever the status. Calls wait their turn, so
   * only one message is ever in flight. Rejects with NoAnswerError after the last try.
   */
  ask(cmd: number, payload?: Uint8Array, options?: AskOptions): Promise<Reply>;
  /** HELLO, asked once per session. */
  hello(): Promise<Hello>;
  /** The whole inventory text, read in LIST pages. */
  readInventory(): Promise<string>;
  /**
   * The switched-off plugin ids: `stored` is the list the next boot applies, `boot` the one in
   * effect since the last boot.
   */
  readOff(which: OffList): Promise<string[]>;
}

const decoder = new TextDecoder();

function concat(parts: readonly Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let offset = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.length;
  }
  return out;
}

export function createSession(port: MidiPort, options: SessionOptions = {}): Session {
  const { signal } = options;
  const assembler = new SysexAssembler();
  let seq = 0;
  /** The reply the message in flight waits for. */
  let waiting: { cmd: number; seq: number; resolve: (reply: Reply) => void } | null = null;
  /** Settles after the previous ask, so the next one starts only then. */
  let turn: Promise<unknown> = Promise.resolve();
  let hello: Promise<Hello> | null = null;

  port.onSysex((bytes) => {
    for (const message of assembler.push(bytes)) {
      const reply = parseReply(message);
      // Anything else is a stale reply to an earlier try, or not for us.
      if (reply && waiting && reply.cmd === waiting.cmd && reply.seq === waiting.seq) waiting.resolve(reply);
    }
  });

  /** Sends one message and waits for its reply; null after the timeout. */
  function exchange(message: Uint8Array, cmd: number, want: number, timeoutMs: number): Promise<Reply | null> {
    return new Promise((resolve, reject) => {
      const finish = (reply: Reply | null, error?: unknown): void => {
        clearTimeout(timer);
        signal?.removeEventListener('abort', onAbort);
        waiting = null;
        if (error === undefined) resolve(reply);
        else reject(error);
      };
      const onAbort = (): void => finish(null, signal?.reason);
      const timer = setTimeout(() => finish(null), timeoutMs);
      signal?.addEventListener('abort', onAbort, { once: true });
      // Set before sending, so a reply that arrives at once is not missed.
      waiting = { cmd, seq: want, resolve: (reply) => finish(reply) };
      try {
        port.send(message);
      } catch (e) {
        finish(null, e);
      }
    });
  }

  async function askNow(cmd: number, payload: Uint8Array, ask: AskOptions): Promise<Reply> {
    const timeoutMs = ask.timeoutMs ?? options.timeoutMs ?? DEFAULT_TIMEOUT_MS;
    const tries = ask.tries ?? options.tries ?? DEFAULT_TRIES;
    for (let attempt = 0; attempt < tries; attempt++) {
      signal?.throwIfAborted();
      seq = (seq + 1) & SEQ_MASK;
      const reply = await exchange(buildMessage(cmd, seq, payload), cmd, seq, timeoutMs);
      if (reply) return reply;
    }
    throw new NoAnswerError(cmd, tries);
  }

  function ask(cmd: number, payload: Uint8Array = new Uint8Array(0), askOptions: AskOptions = {}): Promise<Reply> {
    const result = turn.then(() => askNow(cmd, payload, askOptions));
    turn = result.catch(() => undefined);
    return result;
  }

  function expectOk(reply: Reply, what: string): void {
    if (reply.status !== Status.OK) throw new RefusedError(reply.cmd, reply.status, what);
  }

  /**
   * Reads a text the OP-1 serves in pages: `request(offset)` builds the payload, each reply carries
   * the total length and up to PAGE_BYTES from the offset (a piece past the total is dropped).
   */
  async function readPaged(cmd: number, request: (offset: number) => Uint8Array, what: string): Promise<string> {
    const parts: Uint8Array[] = [];
    let got = 0;
    let total: number;
    do {
      const reply = await ask(cmd, request(got));
      expectOk(reply, what);
      total = readU32le(reply.payload);
      const piece = reply.payload.subarray(4, 4 + Math.max(0, total - got));
      if (piece.length === 0 && got < total) throw new Error(`the OP-1 sent an empty piece of ${what}`);
      parts.push(piece);
      got += piece.length;
    } while (got < total);
    return decoder.decode(concat(parts));
  }

  return {
    ask,
    hello() {
      hello ??= ask(Command.HELLO).then(parseHello);
      // A failed HELLO may be asked again.
      hello.catch(() => {
        hello = null;
      });
      return hello;
    },
    readInventory() {
      return readPaged(Command.LIST, (offset) => Uint8Array.from(u32le(offset)), 'the plugin inventory');
    },
    async readOff(which) {
      const text = await readPaged(
        Command.OFF_GET,
        (offset) => {
          if (offset > 0xffff) throw new Error('the switched-off list is longer than the OP-1 can send');
          return Uint8Array.from([which === 'boot' ? 1 : 0, offset & 0xff, offset >> 8]);
        },
        'the switched-off list',
      );
      return text
        .split(',')
        .map((id) => id.trim())
        .filter((id) => id !== '');
    },
  };
}
