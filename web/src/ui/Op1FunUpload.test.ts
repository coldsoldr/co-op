import { describe, expect, it } from 'vitest';
import { uploadUnavailable } from './Op1FunUpload';
import { makeClient, type Op1Fun } from './useOp1Fun';

const op1fun = (overrides: Partial<Op1Fun>): Op1Fun => ({
  creds: null,
  client: null,
  workerUrl: '',
  save: () => true,
  setUsername: () => undefined,
  forget: () => undefined,
  ...overrides,
});

describe('uploadUnavailable', () => {
  const client = makeClient({ email: 'me@example.com', token: 't' });

  it('needs the Worker first, then credentials', () => {
    expect(uploadUnavailable(op1fun({ client }))).toMatch(/needs the co-op Worker/);
    expect(uploadUnavailable(op1fun({ workerUrl: 'https://w.example' }))).toMatch(/Save your op1.fun email and token/);
    expect(uploadUnavailable(op1fun({ workerUrl: 'https://w.example', client }))).toBeNull();
  });
});
