# Phase 5: Feedback Worker Implementation Plan

**Status: complete (2026-10-29).** All six tasks are implemented on `main`; the Worker is not deployed until the owner follows `docs/deploying-the-worker.md`, and the site uses the GitHub issue fallback until then. Review added rules this text does not list: the body cap is counted while the request streams; `neutraliseMentions` also breaks `github.com` and leaves `@` after a word character alone (so emails survive); control, C1 and bidi characters are stripped and lone surrogates rejected; siteverify must report a hostname of `ALLOWED_ORIGINS` and its `internal-error` is a 502; both subrequests time out after 10 s; a second rate limiter, `FEEDBACK_GLOBAL_LIMIT` (30 a minute under one `global` key), sits beside the per-client one, which keys IPv6 by /64; CORS exposes `Retry-After` and already allows `X-User-Email` and `X-User-Token` for Phase 6. `worker.yml` also runs a `wrangler deploy --dry-run` and can sync the Worker's secrets from the optional repo secrets `FEEDBACK_GITHUB_TOKEN` and `TURNSTILE_SECRET`. `ci.yml` warns on PRs that mix `packs/` with other changes. The contact field is shown only in Worker mode. A second review moved localhost out of the production `ALLOWED_ORIGINS` (it lives in `.dev.vars` with a separate dev Turnstile widget), keyed IPv4-mapped IPv6 as IPv4, made `invalid-input-secret`/`missing-input-secret` a 502, deploys only from `main` with the optional secrets uploaded in the same `wrangler deploy --secrets-file` call, and in the dialog blocks closing while sending, focuses errors after a failed send, uses `Retry-After` in the 429 message, and cuts the fallback link on grapheme clusters with lone surrogates replaced. The code and the spec's sections 5.4, 7 and 8 are the reference.

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Anyone can send feedback from the app without a GitHub account. A Cloudflare Worker validates it, checks a Turnstile token, rate limits, and opens a labelled GitHub Issue in this repo. Until the owner deploys the Worker, the app degrades to a prefilled GitHub issue link.

**Architecture:** `shared/src/feedback.ts` holds the payload schema and validation used by both the web form and the Worker. `worker/` is a TypeScript Cloudflare Worker with a single `fetch` handler, unit-tested by calling the handler with `Request` objects and a mocked `fetch` (no Miniflare dependency). The web app reads `VITE_WORKER_URL` and `VITE_TURNSTILE_SITE_KEY` at build time; when either is missing the feedback form shows the GitHub fallback. Deployment of the Worker is a separate workflow that runs only when Cloudflare secrets exist.

**Owner prerequisites (not available yet, see docs/deploying-the-worker.md written in Task 6):** a Cloudflare account and API token, a Turnstile widget (site key + secret), a fine-grained GitHub token with Issues read/write on coldsoldr/co-op only. Everything in this plan must build, test and deploy the web app without them.

---

### Task 1: feedback schema in shared

`shared/src/feedback.ts`:
```ts
export const FEEDBACK_CATEGORIES = ['bug', 'idea', 'pack-request', 'other'] as const;
export type FeedbackCategory = (typeof FEEDBACK_CATEGORIES)[number];
export interface FeedbackContext {
  appVersion: string;      // package version
  commit: string;          // short git sha injected at build
  profile: string | null;  // active profile id
  store: 'fs-access' | 'drop' | 'none';
  userAgent: string;
  deviceMounted: boolean;
}
export interface FeedbackPayload {
  category: FeedbackCategory;
  message: string;         // 10..5000 chars after trim
  contact?: string;        // 0..200 chars, optional
  context: FeedbackContext;
  turnstileToken: string;  // 1..2048 chars
}
export function validateFeedback(input: unknown): { ok: true; value: FeedbackPayload } | { ok: false; errors: string[] };
export function issueTitle(p: FeedbackPayload): string;   // `[${category}] ` + first line of message, max 80 chars with ellipsis
export function issueBody(p: FeedbackPayload): string;    // markdown: message in a quoted block, contact line, context table
export function neutraliseMentions(text: string): string; // '@name' -> '@​name', also '#123' -> '#​123' so feedback cannot ping people or link issues
```
`issueBody` runs every user-provided string through `neutraliseMentions`, escapes `<` and backticks fences safely (wrap the message in a `> ` quote per line, never inside a code fence the user can close), and does not include `turnstileToken`. Tests for every rule including mention neutralising, length bounds, unknown categories, extra keys ignored, and title truncation. Commit `feat(shared): feedback payload schema`.

### Task 2: Worker skeleton and tests

- `worker/package.json` (`@co-op/worker`, private, type module; scripts `test`, `typecheck`, `dev: wrangler dev`, `deploy: wrangler deploy`), dev deps `wrangler`, `@cloudflare/workers-types`, `vitest`, `typescript`. `worker/tsconfig.json` with `types: ["@cloudflare/workers-types"]`. `worker/wrangler.toml`: name `co-op-feedback`, `main = "src/index.ts"`, `compatibility_date` set to the date of implementation, `[vars] ALLOWED_ORIGINS = "https://coldsoldr.github.io,http://localhost:5173,http://localhost:4173"`, `GITHUB_REPO = "coldsoldr/co-op"`, and a rate limiter declared with the Workers Rate Limiting binding (`[[ratelimits]] name = "FEEDBACK_LIMIT" namespace_id = "1001" simple = { limit = 5, period = 60 }`). If the installed wrangler rejects this syntax, use whatever the installed version documents and record it.
- `worker/src/env.ts`: `interface Env { GITHUB_TOKEN: string; TURNSTILE_SECRET: string; ALLOWED_ORIGINS: string; GITHUB_REPO: string; FEEDBACK_LIMIT?: { limit(o: { key: string }): Promise<{ success: boolean }> } }`.
- `worker/src/index.ts`: default export `{ fetch(request, env, ctx) }` routing `OPTIONS *` (CORS preflight), `POST /feedback`, `POST /op1fun/patches` (Phase 6 fills it; return 501 for now), else 404. CORS: echo the Origin only if it is in `ALLOWED_ORIGINS`; otherwise 403 for non-OPTIONS. Never log request bodies or headers.
- Add `worker` to the eslint config globs with worker globals. Commit `feat(worker): skeleton with routing and CORS`.

### Task 3: POST /feedback

`worker/src/feedback.ts`:
1. Reject bodies over 16 KB (413) and non-JSON (400).
2. `validateFeedback` -> 400 with `{ errors }`.
3. Rate limit by `CF-Connecting-IP` through `env.FEEDBACK_LIMIT` when present -> 429 with `Retry-After: 60`.
4. Verify Turnstile: POST `https://challenges.cloudflare.com/turnstile/v0/siteverify` with `secret`, `response`, `remoteip`; on `success: false` -> 403 `{ errors: ['verification failed'] }`.
5. Create the issue: POST `https://api.github.com/repos/${GITHUB_REPO}/issues` with headers `Authorization: Bearer`, `Accept: application/vnd.github+json`, `X-GitHub-Api-Version: 2022-11-28`, `User-Agent: co-op-feedback-worker`, body `{ title: issueTitle(p), body: issueBody(p), labels: ['feedback', p.category] }`. On non-2xx -> 502 `{ errors: ['could not file feedback'] }` (do not leak the GitHub response).
6. 201 `{ url: html_url, number }`.
Tests with a mocked global fetch for every branch, asserting the exact GitHub request (labels, headers, no token in body) and that responses carry CORS headers. Commit `feat(worker): feedback endpoint files GitHub issues`.

### Task 4: web feedback form

- Build-time constants via Vite `define`: `__APP_VERSION__` (web package version), `__COMMIT__` (`process.env.GITHUB_SHA?.slice(0,7) ?? 'dev'`). Env: `import.meta.env.VITE_WORKER_URL`, `VITE_TURNSTILE_SITE_KEY` (typed in `web/src/vite-env.d.ts`).
- `web/src/core/feedback/client.ts`: `feedbackAvailable()`, `sendFeedback(payload, fetchImpl)` returning `{ url }` or throwing `FeedbackError` with server errors, and `githubIssueUrl(category, message, context)` building `https://github.com/coldsoldr/co-op/issues/new?labels=feedback,<cat>&title=...&body=...` (URL length capped at 6000 chars by truncating the message, with a note).
- `web/src/ui/Feedback.tsx`: a "Feedback" button in the header opening a `<dialog>` with category select, message textarea with counter, optional contact, a disclosure listing exactly what context is attached, the Turnstile widget (load `https://challenges.cloudflare.com/turnstile/v0/api.js?render=explicit` only when the dialog opens; render with the site key; get token from the callback), Send, and the result: issue link on success, errors inline on failure. When `feedbackAvailable()` is false, the dialog shows the same fields and an "Open GitHub issue" button using `githubIssueUrl`.
- Unit tests for client.ts. Extend Playwright: open Feedback, fill it, assert the GitHub fallback link contains the encoded message (the CI build has no Worker URL). Commit `feat(web): feedback form with GitHub fallback`.

### Task 5: Worker deploy workflow

- `.github/workflows/worker.yml`: on push to main touching `worker/**` or `shared/**`, plus `workflow_dispatch`. Job: checkout, setup action, `pnpm --filter @co-op/worker test`, then a step `check` that sets output `ready=true` only if `CLOUDFLARE_API_TOKEN` and `CLOUDFLARE_ACCOUNT_ID` secrets are non-empty (via env vars, since `secrets` cannot be used in `if:` directly), then `pnpm --filter @co-op/worker exec wrangler deploy` only when ready. Secrets `GITHUB_TOKEN` (named `FEEDBACK_GITHUB_TOKEN` in repo secrets to avoid clashing with the built-in) and `TURNSTILE_SECRET` are set with `wrangler secret put` by the owner once, documented in Task 6.
- `pages.yml` build step passes `VITE_WORKER_URL: ${{ vars.WORKER_URL }}` and `VITE_TURNSTILE_SITE_KEY: ${{ vars.TURNSTILE_SITE_KEY }}` (repository variables, empty until the owner sets them).
- ci.yml runs `pnpm test` which now includes worker and shared (they are workspace packages with `test` scripts). Commit `ci: worker tests and conditional deploy`.

### Task 6: owner setup guide, labels, docs

- `docs/deploying-the-worker.md`: numbered steps for the owner: create Cloudflare account, API token with Workers edit permission, account id; create Turnstile widget for `coldsoldr.github.io` and localhost; create fine-grained GitHub token (repo coldsoldr/co-op, Issues read/write, nothing else); `pnpm --filter @co-op/worker exec wrangler login`, `wrangler secret put GITHUB_TOKEN`, `wrangler secret put TURNSTILE_SECRET`; add repo secrets `CLOUDFLARE_API_TOKEN`, `CLOUDFLARE_ACCOUNT_ID`; add repo variables `WORKER_URL` and `TURNSTILE_SITE_KEY`; re-run the pages workflow; how to test.
- Create GitHub labels now with `gh label create` (idempotent with `--force`): feedback, bug, idea, pack-request, other.
- Spec sections 7 and 8 updated (secret names, rate limiting binding, deploy gating). README phase 5 summary. Mark plan done.
- Push, watch ci, pages and worker (worker deploy step should be skipped, job green). Commit `docs: phase 5 feedback worker`.
