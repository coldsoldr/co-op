import { useCallback, useMemo, useState } from 'react';
import { Op1FunClient } from '../core/op1fun/client';
import { clearCredentials, loadCredentials, saveCredentials, type SavedCredentials } from '../core/op1fun/credentials';
import { Op1FunAuthError, type Op1FunCredentials } from '../core/op1fun/types';
import { messageOf } from '../core/util';

/** The co-op Worker this build knows, or '' when it has none (uploads are then unavailable). */
export const WORKER_URL: string = import.meta.env.VITE_WORKER_URL?.trim() ?? '';

/** The op1.fun account shared by the op1.fun tab and the Library's upload action. */
export interface Op1Fun {
  /** The saved credentials, or null when none are set. */
  creds: SavedCredentials | null;
  /** A client for the saved credentials, or null. */
  client: Op1FunClient | null;
  /** The co-op Worker URL, or ''. */
  workerUrl: string;
  /** Bumped by every save and forget, so views keyed on it start over with the new account. */
  version: number;
  /** Saves for this page and, with `remember`, in this browser. False when the browser refused to remember. */
  save(creds: Op1FunCredentials & { username: string }, remember: boolean): boolean;
  /** Records the username last browsed, keeping the Remember choice. */
  setUsername(username: string): void;
  forget(): void;
}

/** Why installing from op1.fun is not possible, or null when it is. */
export function installUnavailable(workerUrl: string): string | null {
  return workerUrl
    ? null
    : "Installing from op1.fun needs the co-op Worker: op1.fun's file host does not let the browser download patches directly, and this build of co-op has no Worker.";
}

export function makeClient(creds: Op1FunCredentials): Op1FunClient {
  return new Op1FunClient(creds, WORKER_URL ? { workerUrl: WORKER_URL } : {});
}

export function useOp1Fun(): Op1Fun {
  const [creds, setCreds] = useState<SavedCredentials | null>(() => loadCredentials());
  const [version, setVersion] = useState(0);

  const save = useCallback((next: Op1FunCredentials & { username: string }, remember: boolean): boolean => {
    const stored = saveCredentials(next, remember);
    setCreds(loadCredentials());
    setVersion((n) => n + 1);
    return stored;
  }, []);

  const setUsername = useCallback(
    (username: string) => {
      if (!creds || creds.username === username) return;
      saveCredentials({ ...creds, username }, creds.remember);
      setCreds(loadCredentials());
    },
    [creds],
  );

  const forget = useCallback(() => {
    clearCredentials();
    setCreds(null);
    setVersion((n) => n + 1);
  }, []);

  const email = creds?.email;
  const token = creds?.token;
  const client = useMemo(
    () => (email && token ? makeClient({ email, token }) : null),
    [email, token],
  );

  return { creds, client, workerUrl: WORKER_URL, version, save, setUsername, forget };
}

/** An op1.fun error as shown to the user; auth errors point at the Account settings. */
export function op1FunMessage(error: unknown): string {
  if (error instanceof Op1FunAuthError) {
    return 'op1.fun rejected the email or token. Check them on the op1.fun tab and save again.';
  }
  return messageOf(error);
}
