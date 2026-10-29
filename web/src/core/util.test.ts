import { describe, it, expect } from 'vitest';
import { sleep } from './util';

describe('sleep', () => {
  it('resolves after the wait', async () => {
    await expect(sleep(1)).resolves.toBeUndefined();
  });

  it('rejects with the reason as soon as the signal aborts, or at once when it already has', async () => {
    const controller = new AbortController();
    const waiting = sleep(60_000, controller.signal);
    controller.abort(new Error('closed'));
    await expect(waiting).rejects.toThrow('closed');
    await expect(sleep(60_000, controller.signal)).rejects.toThrow('closed');
  });
});
