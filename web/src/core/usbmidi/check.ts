/**
 * The read-only USB check: does the OP-1 on this MIDI port answer the kennel protocol, and if so,
 * what plugins did its last boot load. Pure; the port comes from the caller, who also closes it.
 */
import { parseInventory, type Inventory } from './inventory';
import { NoAnswerError, createSession, type Hello, type MidiPort, type SessionOptions } from './session';

export interface OffLists {
  /** The ids the next boot switches off. */
  stored: string[];
  /** The ids switched off since the last boot. */
  boot: string[];
}

export type CheckResult =
  | {
      /** hundekraft 3.x answered HELLO. */
      kind: 'kennel';
      hello: Hello;
      /** null when the build has no inventory (HELLO without capability bit 0). */
      inventory: Inventory | null;
      off: OffLists | null;
    }
  | {
      /**
       * The port is there but HELLO got no answer: TE's firmware, op1r, hundekraft 2.7 or older,
       * or an OP-1 busy restarting.
       */
      kind: 'silent';
    };

export async function checkDevice(port: MidiPort, options: SessionOptions = {}): Promise<CheckResult> {
  const session = createSession(port, options);
  let hello: Hello;
  try {
    hello = await session.hello();
  } catch (e) {
    if (e instanceof NoAnswerError) return { kind: 'silent' };
    throw e;
  }
  if (!hello.inventory) return { kind: 'kennel', hello, inventory: null, off: null };
  const inventory = parseInventory(await session.readInventory());
  const off = { stored: await session.readOff('stored'), boot: await session.readOff('boot') };
  return { kind: 'kennel', hello, inventory, off };
}
