/**
 * The co-op Worker. Routes:
 *   OPTIONS *              CORS preflight
 *   POST /feedback         files a GitHub issue from the feedback form
 *   POST /op1fun/patches   op1.fun upload proxy (forwards the caller's credentials, stores nothing)
 *   GET  /op1fun/file      op1.fun / S3 file proxy for patch files, previews and pack zips
 * Only origins listed in ALLOWED_ORIGINS get CORS headers; anything else is refused with 403.
 * Nothing here logs request bodies or headers.
 */
import type { Env } from './env';
import { handleFeedback } from './feedback';
import { allowedOrigin, corsHeaders, errors } from './http';
import { handleFile, handleUpload } from './op1fun';

async function route(request: Request, env: Env, origin: string): Promise<Response> {
  const { pathname } = new URL(request.url);
  if (request.method === 'POST' && pathname === '/feedback') {
    return handleFeedback(request, env, origin);
  }
  if (request.method === 'POST' && pathname === '/op1fun/patches') {
    return handleUpload(request, env, origin);
  }
  if (request.method === 'GET' && pathname === '/op1fun/file') {
    return handleFile(request, env, origin);
  }
  return errors(404, ['not found'], origin);
}

export default {
  async fetch(request: Request, env: Env, _ctx: ExecutionContext): Promise<Response> {
    const origin = allowedOrigin(request, env);
    if (request.method === 'OPTIONS') {
      return new Response(null, { status: 204, headers: corsHeaders(origin) });
    }
    if (origin === null) return errors(403, ['origin not allowed'], null);
    try {
      return await route(request, env, origin);
    } catch (_error) {
      // Deliberately not logged: the error could carry request content.
      return errors(500, ['internal error'], origin);
    }
  },
} satisfies ExportedHandler<Env>;
