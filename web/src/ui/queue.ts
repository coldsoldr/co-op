/** Runs async tasks one at a time, in the order they were queued. */
export interface TaskQueue {
  /** Runs `task` once every task queued before it has settled; resolves or rejects with its result. */
  run<T>(task: () => Promise<T>): Promise<T>;
  /** Tasks queued or running. */
  readonly pending: number;
}

/** A TaskQueue; `onPendingChange` hears every change in the number of queued or running tasks. */
export function createQueue(onPendingChange?: (pending: number) => void): TaskQueue {
  let tail: Promise<unknown> = Promise.resolve();
  let pending = 0;
  const change = (delta: number): void => {
    pending += delta;
    onPendingChange?.(pending);
  };
  return {
    get pending() {
      return pending;
    },
    run<T>(task: () => Promise<T>): Promise<T> {
      change(1);
      const result = tail.then(task);
      tail = result.then(
        () => change(-1),
        () => change(-1),
      );
      return result;
    },
  };
}
