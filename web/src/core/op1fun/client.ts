import {
  Op1FunAuthError,
  Op1FunError,
  type Op1FunCredentials,
  type Op1FunPack,
  type Op1FunPatch,
  type Op1FunUpload,
  type Op1FunUser,
} from './types';

export const OP1FUN_API = 'https://api.op1.fun/v1';
/** The only origin that ever receives the X-User-* headers directly from the browser. */
const API_ORIGIN = 'https://api.op1.fun';
/** The pack download endpoint is slow (60 s upstream timeout), so allow a little more. */
export const PACK_ZIP_TIMEOUT_MS = 65000;
/** Patch files and previews; matches the Worker proxy's cap. */
export const MAX_FILE_BYTES = 20 * 1024 * 1024;
export const FILE_TIMEOUT_MS = 60000;
/** The largest patch uploaded: its base64 (12 MiB) plus the JSON stays under the Worker's 13 MiB cap. */
export const MAX_UPLOAD_BYTES = 9 * 1024 * 1024;
/** The longest upload description, so the body stays under the Worker's cap. */
export const MAX_DESCRIPTION = 2000;
/** Uploads go through the Worker, which waits up to 60 s for op1.fun. */
export const UPLOAD_TIMEOUT_MS = 65000;
const UNEXPECTED = 'unexpected response';
const WORKER_NEEDED =
  'could not download the file directly from op1.fun (the browser blocked it); the co-op Worker is needed to fetch it';

export interface Op1FunClientOptions {
  fetch?: typeof fetch;
  /** The API base, `https://api.op1.fun/v1` by default. Credentials are attached only when its host is api.op1.fun. */
  base?: string;
  /** The co-op Worker, used for uploads and as the file proxy. */
  workerUrl?: string;
  /** Builds the abort signal for slow requests; AbortSignal.timeout by default. */
  timeoutSignal?: (ms: number) => AbortSignal;
}

type Json = Record<string, unknown>;

function isObject(value: unknown): value is Json {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function unexpected(): Op1FunError {
  return new Op1FunError(UNEXPECTED);
}

function str(value: unknown): string {
  if (typeof value === 'string') return value;
  if (typeof value === 'number' && Number.isFinite(value)) return String(value);
  return '';
}

/** The value if it is an absolute https: URL, else ''. Other schemes (javascript:, data:, http:) are dropped. */
function httpsUrl(value: unknown): string {
  const raw = str(value);
  try {
    return raw !== '' && new URL(raw).protocol === 'https:' ? raw : '';
  } catch {
    return '';
  }
}

function id(value: unknown): string {
  const result = str(value);
  if (result === '') throw unexpected();
  return result;
}

function obj(value: unknown): Json {
  return isObject(value) ? value : {};
}

/** The ids in relationships[name].data, ignoring entries without an id. */
function relationIds(resource: Json, name: string): string[] {
  const data = obj(obj(resource.relationships)[name]).data;
  if (!Array.isArray(data)) return [];
  return data.map((entry) => str(obj(entry).id)).filter((value) => value !== '');
}

function relationId(resource: Json, name: string): string {
  return str(obj(obj(obj(resource.relationships)[name]).data).id);
}

function resource(value: unknown): Json {
  if (!isObject(value)) throw unexpected();
  return value;
}

function single(body: unknown): Json {
  if (!isObject(body)) throw unexpected();
  return resource(body.data);
}

function list(body: unknown): Json[] {
  if (!isObject(body) || !Array.isArray(body.data)) throw unexpected();
  return body.data.map(resource);
}

export function mapPatch(value: unknown): Op1FunPatch {
  const r = resource(value);
  const attributes = obj(r.attributes);
  const links = obj(r.links);
  return {
    id: id(r.id),
    name: str(attributes.name),
    description: str(attributes.description),
    license: str(attributes.license),
    public: attributes.public === true,
    patchType: str(attributes['patch-type'] ?? attributes.patch_type ?? attributes.patchType),
    fileUrl: httpsUrl(links.file),
    previewUrl: httpsUrl(links.preview),
    selfUrl: httpsUrl(links.self),
    userId: owner(r, attributes),
  };
}

/** The owner's username: attributes["user-id"] (or user_id), else the user relationship id. */
function owner(r: Json, attributes: Json): string {
  return str(attributes['user-id'] ?? attributes.user_id) || relationId(r, 'user');
}

export function mapPack(value: unknown): Op1FunPack {
  const r = resource(value);
  const attributes = obj(r.attributes);
  return {
    id: id(r.id),
    name: str(attributes.name),
    description: str(attributes.description),
    public: attributes.public === true,
    patchIds: relationIds(r, 'patches'),
    downloadUrl: httpsUrl(obj(r.links).download),
    userId: owner(r, attributes),
  };
}

export function mapUser(value: unknown): Op1FunUser {
  const r = resource(value);
  return {
    id: id(r.id),
    username: str(obj(r.attributes).username),
    patchIds: relationIds(r, 'patches'),
    packIds: relationIds(r, 'packs'),
  };
}

const BASE64_CHUNK = 0x8000;

/** Standard base64 of the bytes, built 32 KB at a time so large files never spread into one call. */
export function toBase64(bytes: Uint8Array): string {
  let binary = '';
  for (let i = 0; i < bytes.length; i += BASE64_CHUNK) {
    const chunk = bytes.subarray(i, i + BASE64_CHUNK);
    binary += String.fromCharCode.apply(null, chunk as unknown as number[]);
  }
  return btoa(binary);
}

function trimSlash(url: string): string {
  return url.replace(/\/+$/, '');
}

function originOf(url: string): string {
  try {
    return new URL(url).origin;
  } catch {
    return '';
  }
}

const PROFILE_LINK = /^(?:https?:\/\/)?(?:www\.)?op1\.fun\/users\/([^/?#]+)/i;

/**
 * The slug op1.fun addresses a user by: lower case ("ByronJZA" is /users/byronjza, and the
 * mixed-case path is a 404). A pasted profile link gives its slug.
 */
export function userSlug(input: string): string {
  const trimmed = input.trim();
  const raw = PROFILE_LINK.exec(trimmed)?.[1];
  let name = trimmed;
  if (raw !== undefined) {
    try {
      name = decodeURIComponent(raw);
    } catch {
      name = raw;
    }
  }
  return name.toLowerCase();
}

function pathSegment(value: string, what: 'username' | 'id'): string {
  if (value === '' || value === '.' || value === '..') throw new Op1FunError(`invalid ${what}`);
  return encodeURIComponent(value);
}

/**
 * Reads at most MAX_FILE_BYTES of the body: a larger Content-Length is refused up front, and a
 * body without one is counted as it streams and cancelled once it passes the cap.
 */
async function readCapped(response: Response): Promise<Uint8Array> {
  const declared = Number(response.headers.get('Content-Length'));
  if (Number.isFinite(declared) && declared > MAX_FILE_BYTES) {
    await response.body?.cancel().catch(() => undefined);
    throw new Op1FunError('file is too large');
  }
  if (!response.body) return new Uint8Array(0);
  const reader = response.body.getReader();
  const chunks: Uint8Array[] = [];
  let total = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      total += value.length;
      if (total > MAX_FILE_BYTES) {
        await reader.cancel().catch(() => undefined);
        throw new Op1FunError('file is too large');
      }
      chunks.push(value);
    }
  } catch (error) {
    if (error instanceof Op1FunError) throw error;
    throw new Op1FunError('the download was interrupted', { cause: error });
  }
  const result = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    result.set(chunk, offset);
    offset += chunk.length;
  }
  return result;
}

async function readJson(response: Response): Promise<unknown> {
  try {
    return JSON.parse(await response.text()) as unknown;
  } catch {
    throw unexpected();
  }
}

/** The JSON:API error detail or title from a failed response, or ''. */
async function errorDetail(response: Response): Promise<string> {
  try {
    const body = JSON.parse(await response.text()) as unknown;
    const first = isObject(body) && Array.isArray(body.errors) ? obj(body.errors[0]) : {};
    return str(first.detail) || str(first.title);
  } catch {
    return '';
  }
}

async function failure(response: Response, what: string, secret: string): Promise<Op1FunError> {
  if (response.status === 401) {
    return new Op1FunAuthError('op1.fun rejected the email or token');
  }
  // Never echo the token, even if the upstream message contains it.
  const raw = await errorDetail(response);
  const detail = secret ? raw.split(secret).join('[token]') : raw;
  return new Op1FunError(`${what} failed: HTTP ${response.status}${detail ? ` (${detail})` : ''}`);
}

export class Op1FunClient {
  private readonly fetchImpl: typeof fetch;
  private readonly base: string;
  private readonly workerUrl: string;
  private readonly timeoutSignal: (ms: number) => AbortSignal;

  constructor(
    private readonly creds: Op1FunCredentials,
    opts: Op1FunClientOptions = {},
  ) {
    const impl = opts.fetch ?? fetch;
    // Bind so the global fetch is not called with this client as its receiver.
    this.fetchImpl = ((input, init) => impl(input, init)) as typeof fetch;
    this.base = trimSlash(opts.base ?? OP1FUN_API);
    this.workerUrl = opts.workerUrl ? trimSlash(opts.workerUrl) : '';
    this.timeoutSignal = opts.timeoutSignal ?? ((ms) => AbortSignal.timeout(ms));
  }

  private authHeaders(): Record<string, string> {
    return { 'X-User-Email': this.creds.email, 'X-User-Token': this.creds.token };
  }

  private async api(path: string, what: string, signal?: AbortSignal): Promise<unknown> {
    const url = `${this.base}${path}`;
    // Refuse redirects so the credentials can never be replayed to another URL.
    const init: RequestInit = { redirect: 'error' };
    // Credentials go only to https://api.op1.fun; a different base (tests, mocks) gets none.
    if (originOf(url) === API_ORIGIN) init.headers = this.authHeaders();
    if (signal) init.signal = signal;
    let response: Response;
    try {
      response = await this.fetchImpl(url, init);
    } catch (error) {
      const timedOut = error instanceof Error && error.name === 'TimeoutError';
      throw new Op1FunError(timedOut ? `${what} took too long` : `could not reach op1.fun to ${what}`, {
        cause: error,
      });
    }
    if (!response.ok) throw await failure(response, what, this.creds.token);
    return readJson(response);
  }

  private userPath(username: string): string {
    return `/users/${pathSegment(userSlug(username), 'username')}`;
  }

  async getUser(username: string): Promise<Op1FunUser> {
    return mapUser(single(await this.api(this.userPath(username), 'load the user')));
  }

  async listPatches(username: string): Promise<Op1FunPatch[]> {
    return list(await this.api(`${this.userPath(username)}/patches`, 'list patches')).map(mapPatch);
  }

  async getPatch(username: string, patchId: string): Promise<Op1FunPatch> {
    const path = `${this.userPath(username)}/patches/${pathSegment(patchId, 'id')}`;
    return mapPatch(single(await this.api(path, 'load the patch')));
  }

  async listPacks(username: string): Promise<Op1FunPack[]> {
    return list(await this.api(`${this.userPath(username)}/packs`, 'list packs')).map(mapPack);
  }

  /** The pack and its included patches, in the pack's order where it gives one. */
  async getPack(username: string, packId: string): Promise<{ pack: Op1FunPack; patches: Op1FunPatch[] }> {
    const body = await this.api(`${this.userPath(username)}/packs/${pathSegment(packId, 'id')}`, 'load the pack');
    const pack = mapPack(single(body));
    const included = isObject(body) && Array.isArray(body.included) ? body.included : [];
    const patches = included.filter((entry) => obj(entry).type === 'patches').map(mapPatch);
    const order = new Map(pack.patchIds.map((patchId, index) => [patchId, index]));
    const rank = (patch: Op1FunPatch): number => order.get(patch.id) ?? order.size;
    patches.sort((a, b) => rank(a) - rank(b));
    return { pack, patches };
  }

  /** Asks op1.fun to build the pack zip and returns its URL. Slow: allows 65 s. */
  async requestPackZip(username: string, packId: string): Promise<string> {
    const path = `${this.userPath(username)}/packs/${pathSegment(packId, 'id')}/download`;
    const body = await this.api(path, 'prepare the pack download', this.timeoutSignal(PACK_ZIP_TIMEOUT_MS));
    if (!isObject(body)) throw unexpected();
    const data = obj(body.data);
    // Only an https link is followed; anything else is treated as no link.
    const url = httpsUrl(str(body.url) || str(data.url) || str(obj(data.attributes).url));
    if (!url) throw unexpected();
    return url;
  }

  /**
   * Downloads a patch file or preview. Never sends credentials. Tries the link directly; when the
   * browser blocks that (a TypeError: network or CORS) and a Worker is configured, retries through
   * the Worker's file proxy. HTTP errors, timeouts and other failures are reported without a retry.
   * Each attempt has a 60 s timeout and a MAX_FILE_BYTES cap.
   */
  async fetchFile(url: string): Promise<Uint8Array> {
    let response: Response;
    try {
      response = await this.fetchImpl(url, { signal: this.timeoutSignal(FILE_TIMEOUT_MS) });
    } catch (error) {
      if (error instanceof TypeError) {
        if (!this.workerUrl) throw new Op1FunError(WORKER_NEEDED, { cause: error });
        return this.fetchViaWorker(url);
      }
      const timedOut = error instanceof Error && error.name === 'TimeoutError';
      throw new Op1FunError(timedOut ? 'the download took too long' : 'the download failed', { cause: error });
    }
    if (!response.ok) throw new Op1FunError(`download failed: HTTP ${response.status}`);
    return readCapped(response);
  }

  private async fetchViaWorker(url: string): Promise<Uint8Array> {
    let response: Response;
    try {
      response = await this.fetchImpl(`${this.workerUrl}/op1fun/file?url=${encodeURIComponent(url)}`, {
        signal: this.timeoutSignal(FILE_TIMEOUT_MS),
      });
    } catch (error) {
      throw new Op1FunError(`${WORKER_NEEDED}, but the co-op Worker could not be reached`, { cause: error });
    }
    if (!response.ok) throw new Op1FunError(`the co-op Worker could not fetch the file: HTTP ${response.status}`);
    return readCapped(response);
  }

  /** Uploads a patch through the co-op Worker (op1.fun does not allow cross-origin POST). */
  async uploadPatch(upload: Op1FunUpload): Promise<Op1FunPatch> {
    if (!this.workerUrl) throw new Op1FunError('uploads need the co-op Worker');
    if (upload.bytes.length > MAX_UPLOAD_BYTES) throw new Op1FunError('the patch is too large to upload (over 9 MB)');
    if (upload.description.length > MAX_DESCRIPTION) {
      throw new Op1FunError(`the description is longer than ${MAX_DESCRIPTION} characters`);
    }
    const payload = {
      data: {
        type: 'patches',
        attributes: {
          name: upload.name,
          public: upload.public,
          description: upload.description,
          license: upload.license,
          file: `data:audio/x-aiff;base64,${toBase64(upload.bytes)}`,
        },
      },
    };
    let response: Response;
    try {
      response = await this.fetchImpl(`${this.workerUrl}/op1fun/patches`, {
        method: 'POST',
        headers: { ...this.authHeaders(), 'Content-Type': 'application/json' },
        body: JSON.stringify(payload),
        redirect: 'error',
        signal: this.timeoutSignal(UPLOAD_TIMEOUT_MS),
      });
    } catch (error) {
      const timedOut = error instanceof Error && error.name === 'TimeoutError';
      throw new Op1FunError(timedOut ? 'the upload took too long' : 'could not reach the co-op Worker to upload', {
        cause: error,
      });
    }
    if (!response.ok) throw await failure(response, 'upload', this.creds.token);
    const body = await readJson(response);
    if (!isObject(body)) throw unexpected();
    // The create response wraps the patch in an array; accept a bare object too.
    const data = Array.isArray(body.data) ? body.data[0] : body.data;
    return mapPatch(data);
  }
}
