# Deploying the feedback Worker

The app's Feedback button works without any of this: until the Worker is
deployed, it opens a prefilled GitHub issue that the user submits with their
own GitHub account. Deploying the Worker lets anyone send feedback without
an account. The Worker checks a Cloudflare Turnstile token, rate limits, and
opens a labelled issue in `coldsoldr/co-op` with a token that can do nothing
else.

You need about twenty minutes, a Cloudflare account (the free plan is
enough) and admin rights on this repository. Everything below is a one-off.

## 1. Cloudflare account and API token

1. Sign up or log in at <https://dash.cloudflare.com>.
2. Note your **Account ID**: Workers & Pages, then the right-hand column
   of the overview (or the URL, `dash.cloudflare.com/<account id>/...`).
3. Pick a workers.dev subdomain if asked (Workers & Pages, Overview). The
   Worker will live at `https://co-op-feedback.<subdomain>.workers.dev`.
4. Create an API token: My Profile, API Tokens, Create Token, template
   **Edit Cloudflare Workers**. Limit it to your account. Copy the token;
   it is shown once.

## 2. Turnstile widget

1. In the dashboard, Turnstile, Add widget.
2. Name it `co-op feedback`. Hostname: `coldsoldr.github.io` only.
   Widget mode: Managed.
3. Copy the **Site key** (public) and the **Secret key** (private).

The Worker only accepts a token solved on the hostname of one of its
`ALLOWED_ORIGINS`, which in `worker/wrangler.toml` is just
`https://coldsoldr.github.io`, so a token from another site is refused even
if Turnstile says it is valid. For local development, make a second widget
(see "Local development" below) rather than adding `localhost` to this one.

## 3. GitHub token for filing issues

1. Sign in to GitHub **as the account that owns the repository**
   (`coldsoldr`). A fine-grained token can only reach repositories its own
   account owns (or an organisation that account belongs to), so a token
   made by a collaborator cannot be scoped to this repository. Feedback
   issues will show `coldsoldr` as their author.
2. Settings, Developer settings, Personal access tokens,
   **Fine-grained tokens**, Generate new token.
3. Resource owner: `coldsoldr`. Repository access: **Only select
   repositories**, `coldsoldr/co-op`.
4. Repository permissions: **Issues: Read and write**. Leave everything
   else at No access (Metadata: Read-only is added automatically).
5. Pick an expiry and put a reminder in your calendar. Once the token
   expires, the Worker answers every submission with `could not file
   feedback` until you replace it (step 5 below, then run the worker
   workflow).

The labels the Worker applies (`feedback`, `bug`, `idea`, `pack-request`,
`other`) already exist. If one is ever deleted, recreate it with
`gh label create <name> --force`; the issues-only token cannot be relied on
to create labels itself.

## 4. First deploy and the Worker's secrets

The simplest path needs no local tools: set all four repository secrets in
step 5, including the optional `FEEDBACK_GITHUB_TOKEN` and
`TURNSTILE_SECRET`, then run Actions, **worker**, Run workflow on `main`.
The workflow uploads those two secrets together with the deploy
(`wrangler deploy --secrets-file`), so the very first deploy already has
them and there is no window where the Worker runs without a token. The
workflow's log prints the Worker URL.

Or, from your own machine, at the repository root:

```bash
pnpm install
pnpm --filter @co-op/worker exec wrangler login        # opens a browser
pnpm --filter @co-op/worker exec wrangler deploy
pnpm --filter @co-op/worker exec wrangler secret put GITHUB_TOKEN       # paste the GitHub token
pnpm --filter @co-op/worker exec wrangler secret put TURNSTILE_SECRET   # paste the Turnstile secret key
```

Here the Worker exists for a moment before its secrets do; anything sent
in that window gets a 502 and nothing is filed. `wrangler deploy` prints
the Worker URL. Secrets are stored encrypted by Cloudflare and are never in
the repository.

The three rate limiters in `wrangler.toml` are created on deploy: the
feedback limits (5 requests a minute per client, 30 a minute in total) and
`OP1FUN_LIMIT` for the op1.fun routes (60 a minute per client). Their
`namespace_id` values (`1001`, `1002`, `1003`) only need to be unique
within your account.

## 5. Repository secrets and variables

Settings, Secrets and variables, Actions.

Secrets (used by `.github/workflows/worker.yml` to redeploy on every push
to `main` that touches `worker/` or `shared/`):

| Name | Value |
| --- | --- |
| `CLOUDFLARE_API_TOKEN` | the API token from step 1 |
| `CLOUDFLARE_ACCOUNT_ID` | the account ID from step 1 |
| `FEEDBACK_GITHUB_TOKEN` | optional: the GitHub token from step 3; the workflow uploads it as the Worker's `GITHUB_TOKEN` with each deploy |
| `TURNSTILE_SECRET` | optional: the Turnstile secret key, uploaded the same way |

Without the two Cloudflare secrets the workflow still runs the tests and a
dry-run build, then skips the deploy with a notice. It only ever deploys
from `main`. A secret you leave unset keeps the value the Worker already
has.

**After changing any secret** (for example a renewed GitHub token), run
Actions, **worker**, Run workflow on `main`. The workflow only runs by
itself when `worker/` or `shared/` changes, so a new secret does nothing
until the next run.

Variables (read by `.github/workflows/pages.yml` when building the app):

| Name | Value |
| --- | --- |
| `WORKER_URL` | the Worker URL, e.g. `https://co-op-feedback.<subdomain>.workers.dev` |
| `TURNSTILE_SITE_KEY` | the Turnstile site key from step 2 |

## 6. Rebuild the site

Actions, **pages**, Run workflow (on `main`). Once it is deployed, the
Feedback dialog shows a Turnstile check and a Send button instead of "Open
GitHub issue".

## 7. Test it

1. Open <https://coldsoldr.github.io/co-op/>, click **Feedback**, choose
   a category, write a message of at least ten characters, wait for the
   check to pass, and press Send.
2. The dialog links to the new issue, labelled `feedback` and the
   category, with the message quoted and a context table.
3. From a terminal, a request from another origin is refused:

   ```bash
   curl -i -X POST https://co-op-feedback.<subdomain>.workers.dev/feedback \
     -H 'Origin: https://example.com' -H 'Content-Type: application/json' -d '{}'
   # HTTP/2 403 ... {"errors":["origin not allowed"]}
   ```

4. Delete or close the test issue.

## Local development

The deployed Worker allows only the Pages origin. Locally, `.dev.vars`
overrides the `[vars]` in `wrangler.toml`, so localhost never reaches
production.

1. Create a second Turnstile widget, `co-op feedback (dev)`, with the
   hostname `localhost`, and copy its keys.
2. Write `worker/.dev.vars` (gitignored), one `NAME=value` per line:

   ```
   ALLOWED_ORIGINS=http://localhost:5173,http://localhost:4173
   GITHUB_REPO=<you>/<scratch-repo>
   GITHUB_TOKEN=<a fine-grained token for the scratch repo>
   TURNSTILE_SECRET=<the dev widget's secret key>
   ```

3. Run `pnpm --filter @co-op/worker dev` (it listens on
   `http://localhost:8787`).
4. In `web/.env.local` (gitignored) set
   `VITE_WORKER_URL=http://localhost:8787` and
   `VITE_TURNSTILE_SITE_KEY=<the dev widget's site key>`, then run
   `pnpm --filter @co-op/web dev`.

Cloudflare's Turnstile test keys (site key `1x00000000000000000000AA`,
secret `1x0000000000000000000000000000000AA`) also work but report a
placeholder hostname rather than `localhost`; add that hostname as an origin
(for example `http://example.com`) to `ALLOWED_ORIGINS` in `.dev.vars` to
use them.

## Routes

| Route | What it does |
| --- | --- |
| `POST /feedback` | Checks the feedback and its Turnstile token, then files a labelled issue in `GITHUB_REPO`. |
| `POST /op1fun/patches` | Uploads a patch to op1.fun for the app: requires `X-User-Email` and `X-User-Token`, caps the body at 12 MB, checks it is a `patches` resource with an AIFF data URI, and forwards it with only those two headers and `Content-Type` to `https://api.op1.fun/v1/patches` (redirects refused). The upstream status and body come back unchanged. |
| `GET /op1fun/file?url=` | Fetches a patch file, MP3 preview or pack zip for the app when op1.fun's file host does not allow the browser to: `https` only, on `op1.fun`, `www.op1.fun` or `*.amazonaws.com`, paths ending `.aif`, `.aiff`, `.mp3` or `.zip`. No credentials are sent, redirects are refused, and the body is cut off past 20 MB. |

Every route answers only the origins in `ALLOWED_ORIGINS`. The op1.fun
routes need no secrets, so the op1.fun tab's uploads work as soon as the
Worker is deployed and `WORKER_URL` is set for the Pages build. To check
the file proxy refuses other hosts:

```bash
curl -i 'https://co-op-feedback.<subdomain>.workers.dev/op1fun/file?url=https%3A%2F%2Fexample.com%2Fx.aif' \
  -H 'Origin: https://coldsoldr.github.io'
# HTTP/2 400
```

## What the Worker does and does not keep

It stores nothing and logs no request bodies or headers.

- **Feedback.** The message, the optional contact and the context table
  shown in the dialog's "What is attached" section go into a public GitHub
  issue. The verification check runs through Cloudflare Turnstile, which
  sees the sender's IP address and browser. The Worker uses the IP address
  only as a rate-limit key (IPv6 by its /64) and passes it to Turnstile's
  siteverify; it keeps nothing. The Turnstile token is never written
  anywhere.
- **op1.fun.** Uploads and files pass straight through: the email, token
  and file are never written anywhere.

The Feedback dialog says the same in its "What is attached" section.
