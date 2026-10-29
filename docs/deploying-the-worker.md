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
2. Name it `co-op feedback`. Hostnames: `coldsoldr.github.io` and
   `localhost`. Widget mode: Managed.
3. Copy the **Site key** (public) and the **Secret key** (private).

The Worker only accepts a token solved on the hostname of one of its
`ALLOWED_ORIGINS` (`worker/wrangler.toml`), so a token from another site is
refused even if Turnstile says it is valid.

## 3. GitHub token for filing issues

1. GitHub, Settings, Developer settings, Personal access tokens,
   **Fine-grained tokens**, Generate new token.
2. Resource owner: `coldsoldr`. Repository access: **Only select
   repositories**, `coldsoldr/co-op`.
3. Repository permissions: **Issues: Read and write**. Leave everything
   else at No access (Metadata: Read-only is added automatically).
4. Pick an expiry you will remember to renew (the Worker answers
   `could not file feedback` once it expires).

The labels the Worker applies (`feedback`, `bug`, `idea`, `pack-request`,
`other`) already exist. If one is ever deleted, recreate it with
`gh label create <name> --force`; the issues-only token cannot be relied on
to create labels itself.

## 4. First deploy and the Worker's secrets

From the repository root:

```bash
pnpm install
pnpm --filter @co-op/worker exec wrangler login        # opens a browser
pnpm --filter @co-op/worker exec wrangler deploy
pnpm --filter @co-op/worker exec wrangler secret put GITHUB_TOKEN       # paste the GitHub token
pnpm --filter @co-op/worker exec wrangler secret put TURNSTILE_SECRET   # paste the Turnstile secret key
```

`wrangler deploy` prints the Worker URL. Secrets are stored encrypted by
Cloudflare and are never in the repository.

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
| `FEEDBACK_GITHUB_TOKEN` | optional: the GitHub token from step 3; the workflow then writes it to the Worker's `GITHUB_TOKEN` on each deploy, so renewing it is a matter of updating this secret |
| `TURNSTILE_SECRET` | optional: the Turnstile secret key, written to the Worker the same way |

Without the two Cloudflare secrets the workflow still runs the tests and a
dry-run build, then skips the deploy with a notice.

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

Put the secrets in `worker/.dev.vars` (gitignored), one `NAME=value` per line
(`GITHUB_TOKEN=...`, `TURNSTILE_SECRET=...`), then run
`pnpm --filter @co-op/worker dev` and start the app with
`VITE_WORKER_URL=http://localhost:8787` and `VITE_TURNSTILE_SITE_KEY` set
(in `web/.env.local`, also gitignored).

Cloudflare's Turnstile test keys (site key `1x00000000000000000000AA`,
secret `1x0000000000000000000000000000000AA`) always pass, but siteverify
then reports a placeholder hostname rather than `localhost`. To use them,
override `ALLOWED_ORIGINS` in `.dev.vars` with your local origins plus that
hostname as an origin (for example `http://example.com`). Never put it in
`wrangler.toml`. Use a scratch repository in `GITHUB_REPO` (also
overridable in `.dev.vars`) if you do not want test issues here.

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

It stores nothing and logs no request bodies or headers. op1.fun uploads
and files pass straight through: the email, token and file are never
written anywhere. Each issue holds
the message, the optional contact (public, the form says so), and the
context table shown in the dialog's "What is attached" section. The
Turnstile token is never written anywhere.
