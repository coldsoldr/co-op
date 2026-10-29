/**
 * The op1.fun routes. op1.fun does not allow cross-origin POST, so uploads come through here; its
 * S3 bucket sends no CORS headers, so patch files and previews are fetched through here too. Neither route
 * stores or logs anything: credentials and bodies pass straight through.
 */
import type { Env } from './env';
import { clientKey, corsHeaders, errors, readCapped } from './http';

export const OP1FUN_PATCHES = 'https://api.op1.fun/v1/patches';
/** Base64 of a 9 MiB patch (12 MiB) plus the JSON around it, with room to spare. */
export const MAX_UPLOAD_BYTES = 13 * 1024 * 1024;
/** Patch files and previews; matches MAX_FILE_BYTES in the web client. */
export const MAX_PROXY_BYTES = 20 * 1024 * 1024;
export const OP1FUN_TIMEOUT_MS = 60_000;
const AIFF_DATA_URI = 'data:audio/x-aiff;base64,';
const FILE_EXTENSIONS = ['.aif', '.aiff', '.mp3'];
const FILE_HOSTS = ['op1.fun', 'www.op1.fun'];
/** op1.fun's bucket, virtual-hosted style, with or without a region (us-east-1, us-gov-west-1). */
const BUCKET_HOST = /^op1fun\.s3(\.[a-z]{2}(-[a-z]+)+-\d)?\.amazonaws\.com$/;
/** S3 path style, where the bucket is the first path segment; same optional region. */
const PATH_STYLE_HOST = /^s3(\.[a-z]{2}(-[a-z]+)+-\d)?\.amazonaws\.com$/;
const BUCKET_PATH = '/op1fun/';
/** The only Content-Types passed back; anything else becomes application/octet-stream. */
const AUDIO_TYPES = ['audio/x-aiff', 'audio/aiff', 'audio/mpeg'];

const tooMany = (origin: string) =>
  errors(429, ['too many requests, try again in a minute'], origin, { 'Retry-After': '60' });

/** The per-client op1.fun limiter, keyed by route so uploads and downloads count separately. */
async function rateLimited(env: Env, request: Request, route: 'upload' | 'file'): Promise<boolean> {
  if (!env.OP1FUN_LIMIT) return false;
  const key = `op1fun-${route}:${clientKey(request.headers.get('CF-Connecting-IP'))}`;
  return !(await env.OP1FUN_LIMIT.limit({ key })).success;
}

const isObject = (value: unknown): value is Record<string, unknown> =>
  typeof value === 'object' && value !== null && !Array.isArray(value);

/** True for `{ data: { type: 'patches', attributes: { file: 'data:audio/x-aiff;base64,...' } } }`. */
function isPatchUpload(value: unknown): boolean {
  if (!isObject(value) || !isObject(value.data)) return false;
  const { type, attributes } = value.data;
  return type === 'patches' && isObject(attributes) && typeof attributes.file === 'string' && attributes.file.startsWith(AIFF_DATA_URI);
}

/** POST /op1fun/patches: checks size and shape, then forwards to op1.fun with the caller's credentials. */
export async function handleUpload(request: Request, env: Env, origin: string): Promise<Response> {
  const email = request.headers.get('X-User-Email') ?? '';
  const token = request.headers.get('X-User-Token') ?? '';
  if (email === '' || token === '') return errors(401, ['X-User-Email and X-User-Token are required'], origin);
  if (await rateLimited(env, request, 'upload')) return tooMany(origin);

  const body = await readCapped(request, MAX_UPLOAD_BYTES);
  if (body === null) return errors(413, ['upload is too large'], origin);
  let input: unknown;
  try {
    input = JSON.parse(new TextDecoder().decode(body));
  } catch (_error) {
    return errors(400, ['body must be JSON'], origin);
  }
  if (!isPatchUpload(input)) return errors(400, ['expected a patches resource with an AIFF data URI file'], origin);

  let upstream: Response;
  try {
    upstream = await fetch(OP1FUN_PATCHES, {
      method: 'POST',
      headers: { 'X-User-Email': email, 'X-User-Token': token, 'Content-Type': 'application/json' },
      body,
      // Never replay the credentials to wherever a redirect points. Workers reject redirect: 'error'
      // (every fetch with it throws), so take the redirect as a response and refuse it here.
      redirect: 'manual',
      signal: AbortSignal.timeout(OP1FUN_TIMEOUT_MS),
    });
  } catch (_error) {
    return errors(502, ['could not reach op1.fun'], origin);
  }
  if (upstream.status >= 300 && upstream.status < 400) {
    await upstream.body?.cancel().catch(() => undefined);
    return errors(502, ['op1.fun redirected the upload; not followed'], origin);
  }
  return new Response(upstream.body, {
    status: upstream.status,
    headers: { 'Content-Type': upstream.headers.get('Content-Type') ?? 'application/json', ...corsHeaders(origin) },
  });
}

/** The URL if it is an https link on op1.fun or its S3 bucket to a patch or preview; null otherwise. */
export function allowedFileUrl(raw: string | null): URL | null {
  if (!raw) return null;
  let url: URL;
  try {
    url = new URL(raw);
  } catch (_error) {
    return null;
  }
  if (url.protocol !== 'https:' || url.username !== '' || url.password !== '' || url.port !== '') return null;
  const host = url.hostname.toLowerCase();
  const hostOk =
    FILE_HOSTS.includes(host) ||
    BUCKET_HOST.test(host) ||
    (PATH_STYLE_HOST.test(host) && url.pathname.startsWith(BUCKET_PATH));
  const path = url.pathname.toLowerCase();
  return hostOk && FILE_EXTENSIONS.some((ext) => path.endsWith(ext)) ? url : null;
}

/** The upstream media type if it is one of AUDIO_TYPES, else application/octet-stream. */
function audioType(header: string | null): string {
  const type = (header ?? '').split(';')[0]?.trim().toLowerCase() ?? '';
  return AUDIO_TYPES.includes(type) ? type : 'application/octet-stream';
}

/** Passes bytes through and errors the stream once more than `max` have gone by. */
function capStream(max: number): TransformStream<Uint8Array, Uint8Array> {
  let total = 0;
  return new TransformStream<Uint8Array, Uint8Array>({
    transform(chunk, controller) {
      total += chunk.byteLength;
      if (total > max) {
        controller.error(new Error('file is too large'));
        return;
      }
      controller.enqueue(chunk);
    },
  });
}

/** GET /op1fun/file?url=: fetches an allowed file without credentials and streams it back, capped. */
export async function handleFile(request: Request, env: Env, origin: string): Promise<Response> {
  const url = allowedFileUrl(new URL(request.url).searchParams.get('url'));
  if (url === null) return errors(400, ['url must be an https op1.fun or op1.fun S3 link to an .aif, .aiff or .mp3 file'], origin);
  if (await rateLimited(env, request, 'file')) return tooMany(origin);

  let upstream: Response;
  try {
    // Redirects are refused, not followed: an allowed URL must not bounce the proxy elsewhere.
    upstream = await fetch(url.href, { redirect: 'manual', signal: AbortSignal.timeout(OP1FUN_TIMEOUT_MS) });
  } catch (_error) {
    return errors(502, ['could not reach op1.fun'], origin);
  }
  const discard = () => upstream.body?.cancel().catch(() => undefined);
  if (upstream.status >= 300 && upstream.status < 400) {
    await discard();
    return errors(502, ['op1.fun redirected the file; not followed'], origin);
  }
  if (!upstream.ok) {
    await discard();
    return errors(502, [`op1.fun answered HTTP ${upstream.status}`], origin);
  }
  const declared = Number(upstream.headers.get('Content-Length'));
  if (Number.isFinite(declared) && declared > MAX_PROXY_BYTES) {
    await discard();
    return errors(413, ['file is too large'], origin);
  }
  const headers = {
    'Content-Type': audioType(upstream.headers.get('Content-Type')),
    'Content-Disposition': 'attachment',
    'Content-Security-Policy': "default-src 'none'; sandbox",
    'Cache-Control': 'no-store',
    'X-Content-Type-Options': 'nosniff',
    ...corsHeaders(origin),
  };
  if (upstream.body === null) return new Response(null, { status: 200, headers });
  return new Response(upstream.body.pipeThrough(capStream(MAX_PROXY_BYTES)), { status: 200, headers });
}
