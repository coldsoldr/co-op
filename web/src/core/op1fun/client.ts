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
/** The only host that ever receives the X-User-* headers directly from the browser. */
const API_HOST = 'api.op1.fun';
/** The pack download endpoint is slow (60 s upstream timeout), so allow a little more. */
export const PACK_ZIP_TIMEOUT_MS = 65000;
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
    fileUrl: str(links.file),
    previewUrl: str(links.preview),
    selfUrl: str(links.self),
    userId: relationId(r, 'user'),
  };
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
    downloadUrl: str(obj(r.links).download),
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

function hostOf(url: string): string {
  try {
    return new URL(url).hostname.toLowerCase();
  } catch {
    return '';
  }
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

async function failure(response: Response, what: string): Promise<Op1FunError> {
  if (response.status === 401) {
    return new Op1FunAuthError('op1.fun rejected the email or token');
  }
  const detail = await errorDetail(response);
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
    const init: RequestInit = {};
    // Credentials go only to api.op1.fun; a different base (tests, mocks) gets none.
    if (hostOf(url) === API_HOST) init.headers = this.authHeaders();
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
    if (!response.ok) throw await failure(response, what);
    return readJson(response);
  }

  private userPath(username: string): string {
    return `/users/${encodeURIComponent(username)}`;
  }

  async getUser(username: string): Promise<Op1FunUser> {
    return mapUser(single(await this.api(this.userPath(username), 'load the user')));
  }

  async listPatches(username: string): Promise<Op1FunPatch[]> {
    return list(await this.api(`${this.userPath(username)}/patches`, 'list patches')).map(mapPatch);
  }

  async getPatch(username: string, patchId: string): Promise<Op1FunPatch> {
    const path = `${this.userPath(username)}/patches/${encodeURIComponent(patchId)}`;
    return mapPatch(single(await this.api(path, 'load the patch')));
  }

  async listPacks(username: string): Promise<Op1FunPack[]> {
    return list(await this.api(`${this.userPath(username)}/packs`, 'list packs')).map(mapPack);
  }

  /** The pack and its included patches, in the pack's order where it gives one. */
  async getPack(username: string, packId: string): Promise<{ pack: Op1FunPack; patches: Op1FunPatch[] }> {
    const body = await this.api(`${this.userPath(username)}/packs/${encodeURIComponent(packId)}`, 'load the pack');
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
    const path = `${this.userPath(username)}/packs/${encodeURIComponent(packId)}/download`;
    const body = await this.api(path, 'prepare the pack download', this.timeoutSignal(PACK_ZIP_TIMEOUT_MS));
    if (!isObject(body)) throw unexpected();
    const data = obj(body.data);
    const url = str(body.url) || str(data.url) || str(obj(data.attributes).url);
    if (!url) throw unexpected();
    return url;
  }

  /**
   * Downloads a patch file or preview. Never sends credentials. Tries the link directly; when the
   * browser blocks that (a TypeError: network or CORS) and a Worker is configured, retries through
   * the Worker's file proxy. HTTP errors are reported as they are, without a retry.
   */
  async fetchFile(url: string): Promise<Uint8Array> {
    let response: Response;
    try {
      response = await this.fetchImpl(url);
    } catch (error) {
      if (!(error instanceof TypeError)) throw error;
      if (!this.workerUrl) throw new Op1FunError(WORKER_NEEDED, { cause: error });
      return this.fetchViaWorker(url);
    }
    if (!response.ok) throw new Op1FunError(`download failed: HTTP ${response.status}`);
    return new Uint8Array(await response.arrayBuffer());
  }

  private async fetchViaWorker(url: string): Promise<Uint8Array> {
    let response: Response;
    try {
      response = await this.fetchImpl(`${this.workerUrl}/op1fun/file?url=${encodeURIComponent(url)}`);
    } catch (error) {
      throw new Op1FunError(`${WORKER_NEEDED}, but the co-op Worker could not be reached`, { cause: error });
    }
    if (!response.ok) throw new Op1FunError(`the co-op Worker could not fetch the file: HTTP ${response.status}`);
    return new Uint8Array(await response.arrayBuffer());
  }

  /** Uploads a patch through the co-op Worker (op1.fun does not allow cross-origin POST). */
  async uploadPatch(upload: Op1FunUpload): Promise<Op1FunPatch> {
    if (!this.workerUrl) throw new Op1FunError('uploads need the co-op Worker');
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
      });
    } catch (error) {
      throw new Op1FunError('could not reach the co-op Worker to upload', { cause: error });
    }
    if (!response.ok) throw await failure(response, 'upload');
    const body = await readJson(response);
    if (!isObject(body)) throw unexpected();
    // The create response wraps the patch in an array; accept a bare object too.
    const data = Array.isArray(body.data) ? body.data[0] : body.data;
    return mapPatch(data);
  }
}
