import { describe, it, expect } from 'vitest';
import { MAX_DESCRIPTION, MAX_FILE_BYTES, MAX_UPLOAD_BYTES, Op1FunClient, UPLOAD_TIMEOUT_MS, mapPack, mapPatch, toBase64 } from './client';
import { Op1FunAuthError, Op1FunError, type Op1FunCredentials } from './types';
import { fixtureBytes } from '../../../test/fixture-bytes';

const creds: Op1FunCredentials = { email: 'me@example.com', token: 'secret-token' };
const API = 'https://api.op1.fun/v1';
const WORKER = 'https://worker.example.dev';
const S3_FILE = 'https://op1fun.s3.amazonaws.com/patches/kick.aif';

interface Call {
  url: string;
  init: RequestInit | undefined;
}

type Handler = (url: string, init: RequestInit | undefined) => Response | Promise<Response>;

function fakeFetch(handler: Handler): { fetch: typeof fetch; calls: Call[] } {
  const calls: Call[] = [];
  const impl = async (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    const url = typeof input === 'string' ? input : input instanceof URL ? input.href : input.url;
    calls.push({ url, init });
    return handler(url, init);
  };
  return { fetch: impl as typeof fetch, calls };
}

function json(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } });
}

function headersOf(call: Call | undefined): Headers {
  return new Headers(call?.init?.headers);
}

function hasCredentialHeaders(call: Call | undefined): boolean {
  const headers = headersOf(call);
  return headers.has('X-User-Email') || headers.has('X-User-Token');
}

const patchResource = {
  id: '42',
  type: 'patches',
  attributes: {
    name: 'Kick Pack Kick',
    description: 'a kick',
    license: 'CC BY',
    public: true,
    'patch-type': 'drum',
    'user-id': 'alice',
  },
  links: { self: 'https://op1.fun/users/alice/patches/42', file: S3_FILE, preview: 'https://op1fun.s3.amazonaws.com/patches/kick.mp3' },
};

describe('Op1FunClient API reads', () => {
  it('getUser sends both auth headers to api.op1.fun and maps the user', async () => {
    const f = fakeFetch(() =>
      json({
        data: {
          id: 7,
          type: 'users',
          attributes: { username: 'alice' },
          relationships: {
            patches: { data: [{ id: '42', type: 'patches' }, { id: 43, type: 'patches' }] },
            packs: { data: [{ id: '9', type: 'packs' }] },
          },
        },
      }),
    );
    const client = new Op1FunClient(creds, { fetch: f.fetch });
    const user = await client.getUser('alice');
    expect(user).toEqual({ id: '7', username: 'alice', patchIds: ['42', '43'], packIds: ['9'] });
    expect(f.calls[0]?.url).toBe(`${API}/users/alice`);
    const headers = headersOf(f.calls[0]);
    expect(headers.get('X-User-Email')).toBe('me@example.com');
    expect(headers.get('X-User-Token')).toBe('secret-token');
    expect(f.calls[0]?.init?.redirect).toBe('error');
  });

  it('maps a refused redirect (TypeError) to Op1FunError', async () => {
    const f = fakeFetch(() => {
      throw new TypeError('fetch failed: unexpected redirect');
    });
    await expect(new Op1FunClient(creds, { fetch: f.fetch }).listPatches('alice')).rejects.toBeInstanceOf(Op1FunError);
  });

  it.each(['', '.', '..'])('rejects the username %j without a request', async (username) => {
    const f = fakeFetch(() => json({ data: [] }));
    const client = new Op1FunClient(creds, { fetch: f.fetch });
    await expect(client.getUser(username)).rejects.toThrow(new Op1FunError('invalid username'));
    await expect(client.listPatches(username)).rejects.toThrow(new Op1FunError('invalid username'));
    await expect(client.listPacks(username)).rejects.toThrow(new Op1FunError('invalid username'));
    expect(f.calls).toHaveLength(0);
  });

  it.each(['', '.', '..'])('rejects the id %j without a request', async (badId) => {
    const f = fakeFetch(() => json({ data: {} }));
    const client = new Op1FunClient(creds, { fetch: f.fetch });
    await expect(client.getPatch('alice', badId)).rejects.toThrow(new Op1FunError('invalid id'));
    await expect(client.getPack('alice', badId)).rejects.toThrow(new Op1FunError('invalid id'));
    await expect(client.requestPackZip('alice', badId)).rejects.toThrow(new Op1FunError('invalid id'));
    expect(f.calls).toHaveLength(0);
  });

  it.each([
    ['https://api.op1.fun:8443/v1', 'a different port'],
    ['http://api.op1.fun/v1', 'plain http'],
    ['https://api.op1.fun.evil.com/v1', 'a lookalike host'],
  ])('never attaches credentials to %s (%s)', async (base) => {
    const f = fakeFetch(() => json({ data: { id: '7', type: 'users' } }));
    await new Op1FunClient(creds, { fetch: f.fetch, base }).getUser('alice');
    expect(hasCredentialHeaders(f.calls[0])).toBe(false);
  });

  it('encodes the username and id in the path', async () => {
    const f = fakeFetch(() => json({ data: patchResource }));
    await new Op1FunClient(creds, { fetch: f.fetch }).getPatch('a b/c', '4 2');
    expect(f.calls[0]?.url).toBe(`${API}/users/a%20b%2Fc/patches/4%202`);
  });

  it('getUser tolerates missing attributes and relationships', async () => {
    const f = fakeFetch(() => json({ data: { id: '7', type: 'users' } }));
    const user = await new Op1FunClient(creds, { fetch: f.fetch }).getUser('alice');
    expect(user).toEqual({ id: '7', username: '', patchIds: [], packIds: [] });
  });

  it('listPatches maps every patch', async () => {
    const f = fakeFetch(() => json({ data: [patchResource, { id: '43', type: 'patches', attributes: { name: 'Bare' } }] }));
    const patches = await new Op1FunClient(creds, { fetch: f.fetch }).listPatches('alice');
    expect(f.calls[0]?.url).toBe(`${API}/users/alice/patches`);
    expect(patches).toEqual([
      {
        id: '42',
        name: 'Kick Pack Kick',
        description: 'a kick',
        license: 'CC BY',
        public: true,
        patchType: 'drum',
        fileUrl: S3_FILE,
        previewUrl: 'https://op1fun.s3.amazonaws.com/patches/kick.mp3',
        selfUrl: 'https://op1.fun/users/alice/patches/42',
        userId: 'alice',
      },
      {
        id: '43',
        name: 'Bare',
        description: '',
        license: '',
        public: false,
        patchType: '',
        fileUrl: '',
        previewUrl: '',
        selfUrl: '',
        userId: '',
      },
    ]);
  });

  it('getPatch maps a single patch', async () => {
    const f = fakeFetch(() => json({ data: patchResource }));
    const patch = await new Op1FunClient(creds, { fetch: f.fetch }).getPatch('alice', '42');
    expect(f.calls[0]?.url).toBe(`${API}/users/alice/patches/42`);
    expect(patch.name).toBe('Kick Pack Kick');
    expect(patch.fileUrl).toBe(S3_FILE);
  });

  it('listPacks maps every pack', async () => {
    const f = fakeFetch(() =>
      json({
        data: [
          {
            id: '9',
            type: 'packs',
            attributes: { name: 'Drums', description: 'some drums', public: true, 'user-id': 'alice' },
            links: { download: `${API}/users/alice/packs/9/download` },
            relationships: { patches: { data: [{ id: '42', type: 'patches' }] } },
          },
          { id: '10', type: 'packs' },
        ],
      }),
    );
    const packs = await new Op1FunClient(creds, { fetch: f.fetch }).listPacks('alice');
    expect(f.calls[0]?.url).toBe(`${API}/users/alice/packs`);
    expect(packs).toEqual([
      {
        id: '9',
        name: 'Drums',
        description: 'some drums',
        public: true,
        patchIds: ['42'],
        downloadUrl: `${API}/users/alice/packs/9/download`,
        userId: 'alice',
      },
      { id: '10', name: '', description: '', public: false, patchIds: [], downloadUrl: '', userId: '' },
    ]);
  });

  it('getPack returns the pack and its included patches', async () => {
    const f = fakeFetch(() =>
      json({
        data: {
          id: '9',
          type: 'packs',
          attributes: { name: 'Drums' },
          relationships: { patches: { data: [{ id: '43', type: 'patches' }, { id: '42', type: 'patches' }] } },
        },
        included: [
          patchResource,
          { id: '7', type: 'users', attributes: { username: 'alice' } },
          { id: '43', type: 'patches', attributes: { name: 'Snare' } },
        ],
      }),
    );
    const { pack, patches } = await new Op1FunClient(creds, { fetch: f.fetch }).getPack('alice', '9');
    expect(f.calls[0]?.url).toBe(`${API}/users/alice/packs/9`);
    expect(pack.name).toBe('Drums');
    expect(patches.map((p) => p.id)).toEqual(['43', '42']);
  });

  it('getPack tolerates a missing included array', async () => {
    const f = fakeFetch(() => json({ data: { id: '9', type: 'packs' } }));
    const { patches } = await new Op1FunClient(creds, { fetch: f.fetch }).getPack('alice', '9');
    expect(patches).toEqual([]);
  });

  it('maps 401 to Op1FunAuthError', async () => {
    const f = fakeFetch(() => json({ errors: [{ title: 'Unauthorized' }] }, 401));
    const promise = new Op1FunClient(creds, { fetch: f.fetch }).getUser('alice');
    await expect(promise).rejects.toBeInstanceOf(Op1FunAuthError);
    await expect(promise).rejects.toBeInstanceOf(Op1FunError);
  });

  it('maps other HTTP errors to Op1FunError', async () => {
    const f = fakeFetch(() => new Response('nope', { status: 404 }));
    const promise = new Op1FunClient(creds, { fetch: f.fetch }).getUser('ghost');
    await expect(promise).rejects.toBeInstanceOf(Op1FunError);
    await expect(promise).rejects.not.toBeInstanceOf(Op1FunAuthError);
  });

  it('maps a network failure to Op1FunError', async () => {
    const f = fakeFetch(() => {
      throw new TypeError('Failed to fetch');
    });
    await expect(new Op1FunClient(creds, { fetch: f.fetch }).getUser('alice')).rejects.toBeInstanceOf(Op1FunError);
  });

  it.each([
    ['data is a string', { data: 'hello' }],
    ['data is null', { data: null }],
    ['data is missing', {}],
    ['data is an array for a single resource', { data: [patchResource] }],
    ['the resource has no id', { data: { type: 'patches', attributes: {} } }],
  ])('rejects a single-resource response where %s', async (_label, body) => {
    const f = fakeFetch(() => json(body));
    const client = new Op1FunClient(creds, { fetch: f.fetch });
    await expect(client.getPatch('alice', '42')).rejects.toThrow(new Op1FunError('unexpected response'));
    await expect(client.getUser('alice')).rejects.toThrow(new Op1FunError('unexpected response'));
    await expect(client.getPack('alice', '9')).rejects.toThrow(new Op1FunError('unexpected response'));
  });

  it.each([
    ['data is an object', { data: patchResource }],
    ['data is missing', {}],
    ['an item is not an object', { data: [patchResource, 3] }],
  ])('rejects a list response where %s', async (_label, body) => {
    const f = fakeFetch(() => json(body));
    const client = new Op1FunClient(creds, { fetch: f.fetch });
    await expect(client.listPatches('alice')).rejects.toThrow(new Op1FunError('unexpected response'));
    await expect(client.listPacks('alice')).rejects.toThrow(new Op1FunError('unexpected response'));
  });

  it('rejects a body that is not JSON', async () => {
    const f = fakeFetch(() => new Response('<html>', { status: 200 }));
    await expect(new Op1FunClient(creds, { fetch: f.fetch }).getUser('alice')).rejects.toThrow(
      new Op1FunError('unexpected response'),
    );
  });

  it('never attaches credentials when the base is not api.op1.fun', async () => {
    const f = fakeFetch(() => json({ data: { id: '7', type: 'users' } }));
    await new Op1FunClient(creds, { fetch: f.fetch, base: 'https://evil.example.com/v1' }).getUser('alice');
    expect(f.calls[0]?.url).toBe('https://evil.example.com/v1/users/alice');
    expect(hasCredentialHeaders(f.calls[0])).toBe(false);
  });
});

describe('Op1FunClient.requestPackZip', () => {
  it('returns the url and passes the 65 s timeout signal', async () => {
    const requested: number[] = [];
    const signal = new AbortController().signal;
    const f = fakeFetch(() => json({ url: 'https://op1fun.s3.amazonaws.com/packs/9.zip' }));
    const client = new Op1FunClient(creds, {
      fetch: f.fetch,
      timeoutSignal: (ms) => {
        requested.push(ms);
        return signal;
      },
    });
    const url = await client.requestPackZip('alice', '9');
    expect(url).toBe('https://op1fun.s3.amazonaws.com/packs/9.zip');
    expect(f.calls[0]?.url).toBe(`${API}/users/alice/packs/9/download`);
    expect(requested).toEqual([65000]);
    expect(f.calls[0]?.init?.signal).toBe(signal);
    expect(headersOf(f.calls[0]).get('X-User-Token')).toBe('secret-token');
  });

  it('maps a timeout to Op1FunError', async () => {
    const f = fakeFetch(() => {
      throw new DOMException('The operation timed out.', 'TimeoutError');
    });
    const client = new Op1FunClient(creds, { fetch: f.fetch, timeoutSignal: () => new AbortController().signal });
    await expect(client.requestPackZip('alice', '9')).rejects.toBeInstanceOf(Op1FunError);
  });

  it.each(['http://op1fun.s3.amazonaws.com/packs/9.zip', 'javascript:alert(1)', 'not a url'])(
    'rejects a pack link that is not https: %s',
    async (url) => {
      const f = fakeFetch(() => json({ url }));
      await expect(new Op1FunClient(creds, { fetch: f.fetch }).requestPackZip('alice', '9')).rejects.toThrow(
        new Op1FunError('unexpected response'),
      );
    },
  );

  it('rejects a response without a url', async () => {
    const f = fakeFetch(() => json({ nope: true }));
    await expect(new Op1FunClient(creds, { fetch: f.fetch }).requestPackZip('alice', '9')).rejects.toThrow(
      new Op1FunError('unexpected response'),
    );
  });
});

describe('Op1FunClient.fetchFile', () => {
  const bytes = new Uint8Array([0x46, 0x4f, 0x52, 0x4d, 1, 2, 3]);

  it('fetches S3 links directly without credentials', async () => {
    const f = fakeFetch(() => new Response(bytes));
    const result = await new Op1FunClient(creds, { fetch: f.fetch, workerUrl: WORKER }).fetchFile(S3_FILE);
    expect(result).toEqual(bytes);
    expect(f.calls).toHaveLength(1);
    expect(f.calls[0]?.url).toBe(S3_FILE);
    expect(hasCredentialHeaders(f.calls[0])).toBe(false);
  });

  it('falls back to the Worker proxy on a network/CORS TypeError, without credentials', async () => {
    const f = fakeFetch((url) => {
      if (url === S3_FILE) throw new TypeError('Failed to fetch');
      return new Response(bytes);
    });
    const result = await new Op1FunClient(creds, { fetch: f.fetch, workerUrl: `${WORKER}/` }).fetchFile(S3_FILE);
    expect(result).toEqual(bytes);
    expect(f.calls.map((c) => c.url)).toEqual([S3_FILE, `${WORKER}/op1fun/file?url=${encodeURIComponent(S3_FILE)}`]);
    expect(f.calls.some(hasCredentialHeaders)).toBe(false);
  });

  it('does not fall back on an HTTP error', async () => {
    const f = fakeFetch(() => new Response('gone', { status: 403 }));
    await expect(
      new Op1FunClient(creds, { fetch: f.fetch, workerUrl: WORKER }).fetchFile(S3_FILE),
    ).rejects.toBeInstanceOf(Op1FunError);
    expect(f.calls).toHaveLength(1);
  });

  it('does not fall back on errors other than TypeError, and wraps them', async () => {
    const f = fakeFetch(() => {
      throw new DOMException('aborted', 'AbortError');
    });
    await expect(
      new Op1FunClient(creds, { fetch: f.fetch, workerUrl: WORKER }).fetchFile(S3_FILE),
    ).rejects.toBeInstanceOf(Op1FunError);
    expect(f.calls).toHaveLength(1);
  });

  it('follows redirects for file links', async () => {
    const f = fakeFetch(() => new Response(bytes));
    await new Op1FunClient(creds, { fetch: f.fetch }).fetchFile(S3_FILE);
    expect(f.calls[0]?.init?.redirect).not.toBe('error');
  });

  it('says the Worker is needed when the direct fetch fails and no Worker is configured', async () => {
    const f = fakeFetch(() => {
      throw new TypeError('Failed to fetch');
    });
    const promise = new Op1FunClient(creds, { fetch: f.fetch }).fetchFile(S3_FILE);
    await expect(promise).rejects.toBeInstanceOf(Op1FunError);
    await expect(promise).rejects.toThrow(/Worker/);
    expect(f.calls).toHaveLength(1);
  });

  it('says the Worker is needed when the proxy also fails', async () => {
    const f = fakeFetch(() => {
      throw new TypeError('Failed to fetch');
    });
    const promise = new Op1FunClient(creds, { fetch: f.fetch, workerUrl: WORKER }).fetchFile(S3_FILE);
    await expect(promise).rejects.toBeInstanceOf(Op1FunError);
    await expect(promise).rejects.toThrow(/Worker/);
    expect(f.calls).toHaveLength(2);
  });

  it('reports a proxy HTTP error as Op1FunError', async () => {
    const f = fakeFetch((url) => {
      if (url === S3_FILE) throw new TypeError('Failed to fetch');
      return new Response('bad host', { status: 400 });
    });
    await expect(
      new Op1FunClient(creds, { fetch: f.fetch, workerUrl: WORKER }).fetchFile(S3_FILE),
    ).rejects.toBeInstanceOf(Op1FunError);
  });
});

describe('Op1FunClient.uploadPatch', () => {
  const drum = fixtureBytes('real/drum.aif');
  const input = { name: 'My Drum', description: 'thumpy', license: 'CC BY-SA', public: true, bytes: drum };

  it('needs a Worker URL', async () => {
    const f = fakeFetch(() => json({}));
    await expect(new Op1FunClient(creds, { fetch: f.fetch }).uploadPatch(input)).rejects.toThrow(
      new Op1FunError('uploads need the co-op Worker'),
    );
    expect(f.calls).toHaveLength(0);
  });

  it('POSTs the data URI to the Worker with the auth headers', async () => {
    const f = fakeFetch(() => json({ data: [{ ...patchResource, id: '99' }] }, 201));
    const patch = await new Op1FunClient(creds, { fetch: f.fetch, workerUrl: `${WORKER}/` }).uploadPatch(input);
    expect(patch.id).toBe('99');
    const call = f.calls[0];
    expect(f.calls).toHaveLength(1);
    expect(call?.url).toBe(`${WORKER}/op1fun/patches`);
    expect(call?.init?.method).toBe('POST');
    const headers = headersOf(call);
    expect(headers.get('X-User-Email')).toBe('me@example.com');
    expect(headers.get('X-User-Token')).toBe('secret-token');
    expect(headers.get('Content-Type')).toBe('application/json');
    expect(call?.init?.redirect).toBe('error');
    const body = JSON.parse(String(call?.init?.body)) as {
      data: { type: string; attributes: Record<string, unknown> };
    };
    expect(body.data.type).toBe('patches');
    expect(body.data.attributes).toMatchObject({
      name: 'My Drum',
      description: 'thumpy',
      license: 'CC BY-SA',
      public: true,
    });
    const file = String(body.data.attributes.file);
    const prefix = 'data:audio/x-aiff;base64,';
    expect(file.startsWith(prefix)).toBe(true);
    expect(new Uint8Array(Buffer.from(file.slice(prefix.length), 'base64'))).toEqual(drum);
  });

  it('refuses files over 9 MB before sending, so the Worker never has to', async () => {
    expect(MAX_UPLOAD_BYTES).toBe(9 * 1024 * 1024);
    const f = fakeFetch(() => json({}));
    const big = { ...input, bytes: new Uint8Array(MAX_UPLOAD_BYTES + 1) };
    await expect(new Op1FunClient(creds, { fetch: f.fetch, workerUrl: WORKER }).uploadPatch(big)).rejects.toThrow(
      new Op1FunError('the patch is too large to upload (over 9 MB)'),
    );
    expect(f.calls).toHaveLength(0);
  });

  it('refuses a description over 2000 characters', async () => {
    expect(MAX_DESCRIPTION).toBe(2000);
    const f = fakeFetch(() => json({}));
    const client = new Op1FunClient(creds, { fetch: f.fetch, workerUrl: WORKER });
    await expect(client.uploadPatch({ ...input, description: 'x'.repeat(2001) })).rejects.toThrow(
      new Op1FunError('the description is longer than 2000 characters'),
    );
    expect(f.calls).toHaveLength(0);
  });

  it('gives the upload a 65 s timeout', async () => {
    const asked: number[] = [];
    const controller = new AbortController();
    const f = fakeFetch(() => json({ data: [{ ...patchResource, id: '99' }] }, 201));
    const client = new Op1FunClient(creds, {
      fetch: f.fetch,
      workerUrl: WORKER,
      timeoutSignal: (ms) => {
        asked.push(ms);
        return controller.signal;
      },
    });
    await client.uploadPatch(input);
    expect(UPLOAD_TIMEOUT_MS).toBe(65000);
    expect(asked).toEqual([65000]);
    expect(f.calls[0]?.init?.signal).toBe(controller.signal);
  });

  it('reports an upload timeout', async () => {
    const f = fakeFetch(() => {
      throw new DOMException('timed out', 'TimeoutError');
    });
    await expect(
      new Op1FunClient(creds, { fetch: f.fetch, workerUrl: WORKER }).uploadPatch(input),
    ).rejects.toThrow(new Op1FunError('the upload took too long'));
  });

  it('accepts a create response with a single object', async () => {
    const f = fakeFetch(() => json({ data: { ...patchResource, id: '100' } }, 201));
    const patch = await new Op1FunClient(creds, { fetch: f.fetch, workerUrl: WORKER }).uploadPatch(input);
    expect(patch.id).toBe('100');
    expect(patch.selfUrl).toBe('https://op1.fun/users/alice/patches/42');
  });

  it('rejects an empty create array', async () => {
    const f = fakeFetch(() => json({ data: [] }, 201));
    await expect(
      new Op1FunClient(creds, { fetch: f.fetch, workerUrl: WORKER }).uploadPatch(input),
    ).rejects.toThrow(new Op1FunError('unexpected response'));
  });

  it('maps 401 to Op1FunAuthError', async () => {
    const f = fakeFetch(() => json({ errors: [{ title: 'Unauthorized' }] }, 401));
    await expect(
      new Op1FunClient(creds, { fetch: f.fetch, workerUrl: WORKER }).uploadPatch(input),
    ).rejects.toBeInstanceOf(Op1FunAuthError);
  });

  it('surfaces the upstream error detail', async () => {
    const f = fakeFetch(() => json({ errors: [{ detail: 'name has already been taken' }] }, 422));
    await expect(
      new Op1FunClient(creds, { fetch: f.fetch, workerUrl: WORKER }).uploadPatch(input),
    ).rejects.toThrow(/name has already been taken/);
  });
});

describe('toBase64', () => {
  it('matches Node for the empty array and short inputs', () => {
    for (const length of [0, 1, 2, 3, 4, 5]) {
      const data = new Uint8Array(length).map((_, i) => (i * 97 + 13) & 0xff);
      expect(toBase64(data)).toBe(Buffer.from(data).toString('base64'));
    }
  });

  it('matches Node across chunk boundaries', () => {
    const data = new Uint8Array(32768 * 3 + 7).map((_, i) => (i * 31 + 7) & 0xff);
    expect(toBase64(data)).toBe(Buffer.from(data).toString('base64'));
  });

  it('round-trips the real drum fixture', () => {
    const drum = fixtureBytes('real/drum.aif');
    expect(drum.length).toBeGreaterThan(32768);
    expect(new Uint8Array(Buffer.from(toBase64(drum), 'base64'))).toEqual(drum);
  });
});

describe('review fixes', () => {
  it.each([
    ['data.url', { data: { url: 'https://s3.amazonaws.com/p.zip' } }],
    ['data.attributes.url', { data: { attributes: { url: 'https://s3.amazonaws.com/p.zip' } } }],
  ])('requestPackZip reads %s', async (_label, body) => {
    const f = fakeFetch(() => json(body));
    expect(await new Op1FunClient(creds, { fetch: f.fetch }).requestPackZip('alice', '9')).toBe(
      'https://s3.amazonaws.com/p.zip',
    );
  });

  it.each([
    ['patch_type', { patch_type: 'synth' }],
    ['patchType', { patchType: 'synth' }],
  ])('mapPatch reads the %s spelling', (_label, attributes) => {
    expect(mapPatch({ id: '1', attributes }).patchType).toBe('synth');
  });

  it('mapPatch reads user_id and falls back to the user relationship', () => {
    expect(mapPatch({ id: '1', attributes: { user_id: 'bob' } }).userId).toBe('bob');
    expect(mapPatch({ id: '1', relationships: { user: { data: { id: '7' } } } }).userId).toBe('7');
    expect(mapPack({ id: '1', relationships: { user: { data: { id: '7' } } } }).userId).toBe('7');
  });

  it.each([null, undefined, 'x', 3, [], [{ id: '1' }]])('mappers reject %j directly', (value) => {
    expect(() => mapPatch(value)).toThrow(new Op1FunError('unexpected response'));
    expect(() => mapPack(value)).toThrow(new Op1FunError('unexpected response'));
  });

  it('fetchFile rejects a Content-Length over the cap without reading the body', async () => {
    let pulled = 0;
    let cancelled = false;
    const stream = new ReadableStream<Uint8Array>({
      pull(controller) {
        pulled++;
        controller.enqueue(new Uint8Array(1024));
      },
      cancel() {
        cancelled = true;
      },
    });
    const f = fakeFetch(
      () => new Response(stream, { headers: { 'Content-Length': String(MAX_FILE_BYTES + 1) } }),
    );
    await expect(new Op1FunClient(creds, { fetch: f.fetch }).fetchFile(S3_FILE)).rejects.toThrow(
      new Op1FunError('file is too large'),
    );
    expect(cancelled).toBe(true);
    expect(pulled).toBeLessThanOrEqual(1);
  });

  function endless(): { stream: ReadableStream<Uint8Array>; state: { sent: number; cancelled: boolean } } {
    const chunk = new Uint8Array(1024 * 1024);
    const state = { sent: 0, cancelled: false };
    const stream = new ReadableStream<Uint8Array>({
      pull(controller) {
        state.sent += chunk.length;
        controller.enqueue(chunk);
      },
      cancel() {
        state.cancelled = true;
      },
    });
    return { stream, state };
  }

  it('fetchFile counts a body without Content-Length and cancels it past the cap', async () => {
    const { stream, state } = endless();
    const f = fakeFetch(() => new Response(stream));
    await expect(new Op1FunClient(creds, { fetch: f.fetch }).fetchFile(S3_FILE)).rejects.toThrow(
      new Op1FunError('file is too large'),
    );
    expect(state.cancelled).toBe(true);
    expect(state.sent).toBeLessThan(MAX_FILE_BYTES + 4 * 1024 * 1024);
  });

  it('the Worker fallback enforces the cap too', async () => {
    const { stream, state } = endless();
    const f = fakeFetch((url) => {
      if (url === S3_FILE) throw new TypeError('Failed to fetch');
      return new Response(stream);
    });
    await expect(
      new Op1FunClient(creds, { fetch: f.fetch, workerUrl: WORKER }).fetchFile(S3_FILE),
    ).rejects.toThrow(new Op1FunError('file is too large'));
    expect(state.cancelled).toBe(true);
  });

  it('the Worker fallback rejects a Content-Length over the cap', async () => {
    const f = fakeFetch((url) => {
      if (url === S3_FILE) throw new TypeError('Failed to fetch');
      return new Response('x', { headers: { 'Content-Length': String(MAX_FILE_BYTES + 1) } });
    });
    await expect(
      new Op1FunClient(creds, { fetch: f.fetch, workerUrl: WORKER }).fetchFile(S3_FILE),
    ).rejects.toThrow(new Op1FunError('file is too large'));
  });

  it('fetchFile accepts a file exactly at the cap', async () => {
    const data = new Uint8Array(MAX_FILE_BYTES);
    const f = fakeFetch(() => new Response(data));
    expect((await new Op1FunClient(creds, { fetch: f.fetch }).fetchFile(S3_FILE)).length).toBe(MAX_FILE_BYTES);
  });

  it('fetchFile wraps a body read error in Op1FunError', async () => {
    const stream = new ReadableStream<Uint8Array>({
      pull(controller) {
        controller.error(new TypeError('connection reset'));
      },
    });
    const f = fakeFetch(() => new Response(stream));
    await expect(
      new Op1FunClient(creds, { fetch: f.fetch, workerUrl: WORKER }).fetchFile(S3_FILE),
    ).rejects.toBeInstanceOf(Op1FunError);
    expect(f.calls).toHaveLength(1);
  });

  it('fetchFile passes a 60 s timeout signal to the direct and Worker fetches', async () => {
    const requested: number[] = [];
    const signal = new AbortController().signal;
    const f = fakeFetch((url) => {
      if (url === S3_FILE) throw new TypeError('Failed to fetch');
      return new Response(new Uint8Array([1]));
    });
    const client = new Op1FunClient(creds, {
      fetch: f.fetch,
      workerUrl: WORKER,
      timeoutSignal: (ms) => {
        requested.push(ms);
        return signal;
      },
    });
    await client.fetchFile(S3_FILE);
    expect(requested).toEqual([60000, 60000]);
    expect(f.calls.map((c) => c.init?.signal)).toEqual([signal, signal]);
  });

  it('fetchFile maps a timeout to Op1FunError without falling back', async () => {
    const f = fakeFetch(() => {
      throw new DOMException('The operation timed out.', 'TimeoutError');
    });
    await expect(
      new Op1FunClient(creds, { fetch: f.fetch, workerUrl: WORKER }).fetchFile(S3_FILE),
    ).rejects.toBeInstanceOf(Op1FunError);
    expect(f.calls).toHaveLength(1);
  });

  it('no error message ever contains the token', async () => {
    const leaky = (status: number): Response =>
      json({ errors: [{ detail: 'bad token secret-token for me@example.com' }] }, status);
    const upload = { name: 'n', description: '', license: '', public: false, bytes: new Uint8Array([1]) };
    const scenarios: Array<[Handler, (c: Op1FunClient) => Promise<unknown>]> = [
      [() => leaky(401), (c) => c.getUser('alice')],
      [() => leaky(403), (c) => c.getUser('alice')],
      [() => leaky(422), (c) => c.uploadPatch(upload)],
      [() => leaky(500), (c) => c.listPacks('alice')],
      [
        () => {
          throw new TypeError('secret-token');
        },
        (c) => c.getUser('alice'),
      ],
      [() => json({ data: 'secret-token' }), (c) => c.getPatch('alice', '1')],
      [
        () => {
          throw new TypeError('x');
        },
        (c) => c.fetchFile(S3_FILE),
      ],
      [() => new Response('secret-token', { status: 500 }), (c) => c.fetchFile(S3_FILE)],
    ];
    for (const [handler, run] of scenarios) {
      const f = fakeFetch(handler);
      const client = new Op1FunClient(creds, { fetch: f.fetch, workerUrl: WORKER });
      const error = await run(client).then(
        () => null,
        (e: unknown) => e,
      );
      expect(error).toBeInstanceOf(Op1FunError);
      expect((error as Error).message).not.toContain('secret-token');
    }
  });
});

describe('link mapping', () => {
  it('keeps only https: links', () => {
    const links = (value: string) => ({ self: value, file: value, preview: value, download: value });
    for (const bad of ['javascript:alert(1)', 'http://op1fun.s3.amazonaws.com/a.aif', 'data:audio/x-aiff;base64,AA', '/relative.aif', 'not a url']) {
      const patch = mapPatch({ ...patchResource, links: links(bad) });
      expect([patch.fileUrl, patch.previewUrl, patch.selfUrl], bad).toEqual(['', '', '']);
      expect(mapPack({ id: '9', type: 'packs', links: links(bad) }).downloadUrl, bad).toBe('');
    }
    const good = 'https://op1fun.s3.amazonaws.com/a.aif';
    const patch = mapPatch({ ...patchResource, links: links(good) });
    expect([patch.fileUrl, patch.previewUrl, patch.selfUrl]).toEqual([good, good, good]);
    expect(mapPack({ id: '9', type: 'packs', links: links(good) }).downloadUrl).toBe(good);
  });
});
