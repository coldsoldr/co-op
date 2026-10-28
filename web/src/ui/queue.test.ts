import { describe, it, expect } from 'vitest';
import { createQueue } from './queue';

const deferred = (): { promise: Promise<void>; resolve: () => void; reject: (e: Error) => void } => {
  let resolve!: () => void;
  let reject!: (e: Error) => void;
  const promise = new Promise<void>((res, rej) => {
    resolve = res;
    reject = rej;
  });
  return { promise, resolve, reject };
};

const tick = (): Promise<void> => new Promise((resolve) => setTimeout(resolve, 0));

describe('createQueue', () => {
  it('starts a second action only after the first has finished', async () => {
    const counts: number[] = [];
    const queue = createQueue((n) => counts.push(n));
    const log: string[] = [];
    const first = deferred();
    const a = queue.run(async () => {
      log.push('a start');
      await first.promise;
      log.push('a end');
    });
    const b = queue.run(async () => {
      log.push('b start');
    });
    await tick();
    expect(log).toEqual(['a start']);
    expect(queue.pending).toBe(2);
    first.resolve();
    await Promise.all([a, b]);
    await tick();
    expect(log).toEqual(['a start', 'a end', 'b start']);
    expect(queue.pending).toBe(0);
    expect(counts).toEqual([1, 2, 1, 0]);
  });

  it('keeps going after a failed task and passes its error to the caller', async () => {
    const queue = createQueue();
    const failed = queue.run(async () => {
      throw new Error('boom');
    });
    const next = queue.run(async () => 7);
    await expect(failed).rejects.toThrow('boom');
    expect(await next).toBe(7);
  });
});
