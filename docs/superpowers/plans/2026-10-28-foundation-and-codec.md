# Foundation and Codec Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the pnpm workspace, CI and GitHub Pages deploy, then build the OP-1 AIFF patch codec and the stock OP-1 device profile with a small inspector page for checking real device files.

**Architecture:** A pnpm workspace with a `web` package (Vite, React, TypeScript). All patch logic lives in `web/src/core` as pure TypeScript with no React: an IFF chunk layer, a COMM parser, the OP-1 `APPL` JSON chunk codec, a patch model on top, and a `DeviceProfile` registry with the stock OP-1 profile. The UI for this plan is a single inspector screen that reads a dropped `.aif` and shows what the codec found.

**Tech Stack:** Node 22, pnpm 11, Vite, React 19, TypeScript, Vitest, tsx (fixture script), GitHub Actions, GitHub Pages.

**Spec:** `docs/superpowers/specs/2026-10-28-op-patch-librarian-design.md`, sections 4, 5.1, 5.2, 8, 10, phases 1 and 2.

---

## Background the engineer needs

**OP-1 patch files** are AIFF (`FORM` / `AIFF`) or AIFF-C (`FORM` / `AIFC`) files. Audio is 44.1 kHz, 16-bit, mono. Metadata lives in an `APPL` chunk placed before the `SSND` chunk:

```
"APPL"            4 bytes chunk id
size              4 bytes big-endian, counts everything after this field
"op-1"            4 bytes application signature
JSON text         compact, keys in alphabetical order
filler            0x0A then 0x1E bytes until the chunk size is even
```

The FORM size at bytes 4..8 is the file length minus 8. IFF chunks with an odd size carry one pad byte that is not counted in the size.

**Synth JSON** fields (from real files via the teoperator project):
`adsr[8]`, `fx_active`, `fx_params[8]`, `fx_type`, `knobs[8]`, `lfo_active`, `lfo_params[8]`, `lfo_type`, `name`, `octave`, `synth_version`, `type` (engine: `cluster`, `digital`, `dna`, `drwave`, `dsynth`, `fm`, `phase`, `pulse`, `string`, `voltage`, `sampler`, `iter`), optional `base_freq` for samplers.

**Drum JSON** fields: `drum_version`, `dyna_env[8]`, `end[24]`, `fx_active`, `fx_params[8]`, `fx_type`, `lfo_active`, `lfo_params[8]`, `lfo_type`, `name`, `octave`, `pitch[24]`, `playmode[24]`, `reverse[24]`, `start[24]`, `type` (`"drum"`), `volume[24]`.

**Endianness:** plain `AIFF` sound data is big-endian. `AIFC` files declare a compression type in `COMM`; `NONE` or `twos` is big-endian, `sowt` is little-endian. The codec handles both.

**Drive layout (stock OP-1 in disk mode):** top-level folders `album`, `drum`, `synth`, `tape`. `synth` and `drum` contain one level of subfolders (`user`, `snapshot`, and user-made folders) holding `.aif` patches.

---

## File structure

```
package.json                         workspace root, scripts fan out with pnpm -r
pnpm-workspace.yaml
tsconfig.base.json                   shared compiler options
.gitattributes, .editorconfig, .npmrc
.github/workflows/ci.yml             typecheck, test, build on every PR and push
.github/workflows/pages.yml          build web and deploy to GitHub Pages on main
web/
  package.json, vite.config.ts, tsconfig.json, index.html
  src/main.tsx, src/App.tsx, src/index.css
  src/core/codec/iff.ts              generic IFF FORM reader and writer
  src/core/codec/comm.ts             COMM chunk and 80-bit extended floats
  src/core/codec/op1-chunk.ts        APPL "op-1" JSON chunk
  src/core/codec/op1-meta.ts         synth and drum metadata types and guards
  src/core/codec/ssnd.ts             SSND decode to float, encode from int16
  src/core/codec/patch.ts            Patch model: readPatch, writePatch, withMeta, decodePcm
  src/core/store/listing.ts          DirectoryListing interface
  src/core/store/memory-listing.ts   in-memory listing for tests
  src/core/profiles/types.ts         DeviceProfile and related types
  src/core/profiles/op1-stock.ts     stock OP-1 profile
  src/core/profiles/scan.ts          enumerate patch files via a profile's layout
  src/core/profiles/registry.ts      profile list, lookup, detection
  src/ui/Inspector.tsx               drop a file, see what the codec read
  test/fixtures/meta.ts              real-world metadata samples used by fixtures and tests
  test/fixtures/build-fixtures.ts    generates the .aif fixtures
  test/fixtures/*.aif                generated, committed
```

Every `*.ts` module in `src/core` has a sibling `*.test.ts`.

---

### Task 1: Workspace root

**Files:**
- Create: `package.json`, `pnpm-workspace.yaml`, `tsconfig.base.json`, `.gitattributes`, `.editorconfig`, `.npmrc`

- [ ] **Step 1: Create the root files**

`package.json`:
```json
{
  "name": "co-op",
  "private": true,
  "packageManager": "pnpm@11.22.0",
  "engines": { "node": ">=22" },
  "scripts": {
    "typecheck": "pnpm -r typecheck",
    "test": "pnpm -r test",
    "build": "pnpm -r build"
  }
}
```

`pnpm-workspace.yaml`:
```yaml
packages:
  - web
  - worker
  - shared
```

`tsconfig.base.json`:
```json
{
  "compilerOptions": {
    "target": "ES2022",
    "module": "ESNext",
    "moduleResolution": "bundler",
    "strict": true,
    "noUncheckedIndexedAccess": true,
    "exactOptionalPropertyTypes": true,
    "noFallthroughCasesInSwitch": true,
    "isolatedModules": true,
    "skipLibCheck": true,
    "esModuleInterop": true,
    "forceConsistentCasingInFileNames": true,
    "noEmit": true
  }
}
```

`.gitattributes`:
```
* text=auto eol=lf
*.aif binary
*.aiff binary
*.png binary
```

`.editorconfig`:
```
root = true

[*]
charset = utf-8
end_of_line = lf
insert_final_newline = true
indent_style = space
indent_size = 2
trim_trailing_whitespace = true
```

`.npmrc`:
```
engine-strict=true
```

- [ ] **Step 2: Verify pnpm accepts the workspace**

Run: `pnpm install`
Expected: completes with no packages installed and writes `pnpm-lock.yaml`.

- [ ] **Step 3: Commit**

```bash
git add package.json pnpm-workspace.yaml pnpm-lock.yaml tsconfig.base.json .gitattributes .editorconfig .npmrc
git commit -m "chore: add pnpm workspace root"
```

---

### Task 2: Web package scaffold

**Files:**
- Create: `web/package.json`, `web/vite.config.ts`, `web/tsconfig.json`, `web/index.html`, `web/src/main.tsx`, `web/src/App.tsx`, `web/src/index.css`, `web/src/smoke.test.ts`

- [ ] **Step 1: Create package.json and install dependencies**

`web/package.json`:
```json
{
  "name": "@co-op/web",
  "private": true,
  "type": "module",
  "scripts": {
    "dev": "vite",
    "build": "vite build",
    "preview": "vite preview",
    "test": "vitest run",
    "test:watch": "vitest",
    "typecheck": "tsc --noEmit",
    "fixtures": "tsx test/fixtures/build-fixtures.ts"
  }
}
```

Run from the repo root:
```bash
pnpm --filter @co-op/web add react react-dom
pnpm --filter @co-op/web add -D vite @vitejs/plugin-react typescript vitest tsx @types/react @types/react-dom @types/node
```
Expected: both commands finish and `web/node_modules` exists.

- [ ] **Step 2: Create the Vite and TypeScript config**

`web/vite.config.ts`:
```ts
import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

export default defineConfig({
  base: '/co-op/',
  plugins: [react()],
  test: {
    environment: 'node',
    include: ['src/**/*.test.ts'],
  },
});
```

`web/tsconfig.json`:
```json
{
  "extends": "../tsconfig.base.json",
  "compilerOptions": {
    "lib": ["ES2022", "DOM", "DOM.Iterable"],
    "jsx": "react-jsx",
    "types": ["vite/client", "node"]
  },
  "include": ["src", "test", "vite.config.ts"]
}
```

- [ ] **Step 3: Create the placeholder app**

`web/index.html`:
```html
<!doctype html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <title>co-op patch librarian</title>
  </head>
  <body>
    <div id="root"></div>
    <script type="module" src="/src/main.tsx"></script>
  </body>
</html>
```

`web/src/main.tsx`:
```tsx
import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { App } from './App';
import './index.css';

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
```

`web/src/App.tsx`:
```tsx
export function App() {
  return (
    <main>
      <h1>co-op patch librarian</h1>
      <p>Coming soon: a patch librarian for the OP-1, OP-1 Field and OP-XY.</p>
    </main>
  );
}
```

`web/src/index.css`:
```css
:root {
  font-family: system-ui, sans-serif;
  color: #1a1a1a;
  background: #f6f6f4;
}
body {
  margin: 0;
}
main {
  max-width: 960px;
  margin: 0 auto;
  padding: 24px 16px;
}
pre {
  overflow: auto;
  background: #ececea;
  padding: 12px;
  border-radius: 6px;
}
```

- [ ] **Step 4: Add a smoke test so the test runner is proven**

`web/src/smoke.test.ts`:
```ts
import { it, expect } from 'vitest';

it('runs tests', () => {
  expect(1 + 1).toBe(2);
});
```

- [ ] **Step 5: Run typecheck, test and build**

Run: `pnpm typecheck && pnpm test && pnpm build`
Expected: typecheck prints nothing, vitest reports `1 passed`, vite writes `web/dist/index.html`.

- [ ] **Step 6: Commit**

```bash
git add web pnpm-lock.yaml
git commit -m "chore: scaffold web package with vite, react and vitest"
```

---

### Task 3: CI and GitHub Pages deploy

**Files:**
- Create: `.github/workflows/ci.yml`, `.github/workflows/pages.yml`

- [ ] **Step 1: Create the CI workflow**

`.github/workflows/ci.yml`:
```yaml
name: ci

on:
  pull_request:
  push:
    branches: [main]

jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: pnpm/action-setup@v4
      - uses: actions/setup-node@v4
        with:
          node-version: 22
          cache: pnpm
      - run: pnpm install --frozen-lockfile
      - run: pnpm typecheck
      - run: pnpm test
      - run: pnpm build
```

- [ ] **Step 2: Create the Pages workflow**

`.github/workflows/pages.yml`:
```yaml
name: pages

on:
  push:
    branches: [main]
  workflow_dispatch:

permissions:
  contents: read
  pages: write
  id-token: write

concurrency:
  group: pages
  cancel-in-progress: true

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: pnpm/action-setup@v4
      - uses: actions/setup-node@v4
        with:
          node-version: 22
          cache: pnpm
      - run: pnpm install --frozen-lockfile
      - run: pnpm --filter @co-op/web build
      - uses: actions/upload-pages-artifact@v3
        with:
          path: web/dist

  deploy:
    needs: build
    runs-on: ubuntu-latest
    environment:
      name: github-pages
      url: ${{ steps.deployment.outputs.page_url }}
    steps:
      - id: deployment
        uses: actions/deploy-pages@v4
```

- [ ] **Step 3: Enable Pages with the Actions build type**

Run: `gh api -X POST repos/coldsoldr/co-op/pages -f build_type=workflow`
Expected: JSON response containing `"build_type":"workflow"`. If it returns 409 the site already exists, which is fine.

- [ ] **Step 4: Commit and push, then watch the deploy**

```bash
git add .github
git commit -m "ci: add check and pages workflows"
git push origin main
gh run watch --exit-status
```
Expected: both workflows succeed and `https://coldsoldr.github.io/co-op/` shows the placeholder page.

---

### Task 4: IFF FORM reader and writer

**Files:**
- Create: `web/src/core/codec/iff.ts`
- Test: `web/src/core/codec/iff.test.ts`

- [ ] **Step 1: Write the failing tests**

`web/src/core/codec/iff.test.ts`:
```ts
import { describe, it, expect } from 'vitest';
import { readForm, writeForm, IffError } from './iff';

function ascii(s: string): Uint8Array {
  return new TextEncoder().encode(s);
}
function u32(n: number): Uint8Array {
  const b = new Uint8Array(4);
  new DataView(b.buffer).setUint32(0, n);
  return b;
}
function concat(...parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) {
    out.set(p, o);
    o += p.length;
  }
  return out;
}

const body = concat(
  ascii('AIFF'),
  ascii('ABCD'), u32(3), ascii('xyz'), new Uint8Array([7]),
  ascii('EFGH'), u32(2), ascii('ok'),
);
const file = concat(ascii('FORM'), u32(body.length), body);

describe('readForm', () => {
  it('parses chunks and keeps the pad byte of odd-sized chunks', () => {
    const form = readForm(file);
    expect(form.formType).toBe('AIFF');
    expect(form.chunks.map((c) => c.id)).toEqual(['ABCD', 'EFGH']);
    expect(Array.from(form.chunks[0]!.data)).toEqual(Array.from(ascii('xyz')));
    expect(Array.from(form.chunks[0]!.pad!)).toEqual([7]);
    expect(form.chunks[1]!.pad).toBeUndefined();
  });

  it('rejects files without a FORM header', () => {
    expect(() => readForm(ascii('RIFF....WAVE'))).toThrow(IffError);
  });

  it('rejects a chunk that runs past the end of the file', () => {
    const bad = concat(ascii('FORM'), u32(16), ascii('AIFF'), ascii('ABCD'), u32(100), ascii('xy'));
    expect(() => readForm(bad)).toThrow(/runs past end/);
  });
});

describe('writeForm', () => {
  it('round-trips byte for byte', () => {
    expect(Array.from(writeForm(readForm(file)))).toEqual(Array.from(file));
  });

  it('writes the FORM size as total length minus 8 and pads odd chunks', () => {
    const out = writeForm({ formType: 'AIFF', chunks: [{ id: 'ABCD', data: ascii('x') }] });
    expect(out.length).toBe(12 + 8 + 2);
    expect(new DataView(out.buffer).getUint32(4)).toBe(out.length - 8);
    expect(out[out.length - 1]).toBe(0);
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `pnpm --filter @co-op/web test -- iff`
Expected: FAIL, cannot resolve `./iff`.

- [ ] **Step 3: Implement iff.ts**

`web/src/core/codec/iff.ts`:
```ts
const encoder = new TextEncoder();
const latin1 = new TextDecoder('latin1');

export interface Chunk {
  /** Four ASCII characters. */
  id: string;
  /** Exactly `size` bytes; the pad byte is not included. */
  data: Uint8Array;
  /** The pad byte that followed an odd-sized chunk, kept so writes are byte-identical. */
  pad?: Uint8Array;
}

export interface IffForm {
  /** "AIFF" or "AIFC" for the files we care about. */
  formType: string;
  chunks: Chunk[];
}

export class IffError extends Error {}

function readId(bytes: Uint8Array, offset: number): string {
  return latin1.decode(bytes.subarray(offset, offset + 4));
}

export function readForm(bytes: Uint8Array): IffForm {
  if (bytes.length < 12 || readId(bytes, 0) !== 'FORM') {
    throw new IffError('not an IFF FORM file');
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const formType = readId(bytes, 8);
  const chunks: Chunk[] = [];
  let offset = 12;
  while (offset + 8 <= bytes.length) {
    const id = readId(bytes, offset);
    const size = view.getUint32(offset + 4);
    const start = offset + 8;
    const end = start + size;
    if (end > bytes.length) throw new IffError(`chunk ${id} runs past end of file`);
    const chunk: Chunk = { id, data: bytes.slice(start, end) };
    offset = end;
    if (size % 2 === 1 && offset < bytes.length) {
      chunk.pad = bytes.slice(offset, offset + 1);
      offset += 1;
    }
    chunks.push(chunk);
  }
  return { formType, chunks };
}

export function writeForm(form: IffForm): Uint8Array {
  let total = 12;
  for (const c of form.chunks) total += 8 + c.data.length + (c.data.length % 2);
  const out = new Uint8Array(total);
  const view = new DataView(out.buffer);
  out.set(encoder.encode('FORM'), 0);
  view.setUint32(4, total - 8);
  out.set(encoder.encode(form.formType), 8);
  let offset = 12;
  for (const c of form.chunks) {
    out.set(encoder.encode(c.id), offset);
    view.setUint32(offset + 4, c.data.length);
    out.set(c.data, offset + 8);
    offset += 8 + c.data.length;
    if (c.data.length % 2 === 1) {
      out[offset] = c.pad?.[0] ?? 0;
      offset += 1;
    }
  }
  return out;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `pnpm --filter @co-op/web test -- iff`
Expected: 5 passed.

- [ ] **Step 5: Commit**

```bash
git add web/src/core/codec/iff.ts web/src/core/codec/iff.test.ts
git commit -m "feat(codec): IFF FORM reader and writer"
```

---

### Task 5: COMM chunk and 80-bit extended floats

**Files:**
- Create: `web/src/core/codec/comm.ts`
- Test: `web/src/core/codec/comm.test.ts`

- [ ] **Step 1: Write the failing tests**

`web/src/core/codec/comm.test.ts`:
```ts
import { describe, it, expect } from 'vitest';
import { readExtended, writeExtended, parseComm, buildComm, type CommInfo } from './comm';

// 44100 Hz as an 80-bit IEEE 754 extended float, the value every OP-1 file carries.
const RATE_44100 = [0x40, 0x0e, 0xac, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00];

describe('extended floats', () => {
  it('reads 44100', () => {
    const view = new DataView(new Uint8Array(RATE_44100).buffer);
    expect(readExtended(view, 0)).toBe(44100);
  });

  it('writes 44100', () => {
    const bytes = new Uint8Array(10);
    writeExtended(new DataView(bytes.buffer), 0, 44100);
    expect(Array.from(bytes)).toEqual(RATE_44100);
  });

  it('round-trips other rates and zero', () => {
    for (const rate of [0, 8000, 22050, 48000, 96000]) {
      const bytes = new Uint8Array(10);
      writeExtended(new DataView(bytes.buffer), 0, rate);
      expect(readExtended(new DataView(bytes.buffer), 0)).toBe(rate);
    }
  });
});

describe('COMM', () => {
  const info: CommInfo = {
    channels: 1,
    frames: 44100,
    bitsPerSample: 16,
    sampleRate: 44100,
    compression: 'NONE',
    compressionName: '',
  };

  it('builds and parses a plain AIFF COMM chunk of 18 bytes', () => {
    const chunk = buildComm(info, 'AIFF');
    expect(chunk.id).toBe('COMM');
    expect(chunk.data.length).toBe(18);
    expect(parseComm(chunk, 'AIFF')).toEqual(info);
  });

  it('builds and parses an AIFC COMM chunk with a sowt compression type', () => {
    const sowt: CommInfo = { ...info, compression: 'sowt', compressionName: 'not compressed' };
    const chunk = buildComm(sowt, 'AIFC');
    // 18 + 4 (type) + 1 (length byte) + 14 (name) + 1 (pad to even) = 38
    expect(chunk.data.length).toBe(38);
    expect(parseComm(chunk, 'AIFC')).toEqual(sowt);
  });

  it('rejects a COMM chunk that is too short', () => {
    expect(() => parseComm({ id: 'COMM', data: new Uint8Array(10) }, 'AIFF')).toThrow(/too short/);
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `pnpm --filter @co-op/web test -- comm`
Expected: FAIL, cannot resolve `./comm`.

- [ ] **Step 3: Implement comm.ts**

`web/src/core/codec/comm.ts`:
```ts
import type { Chunk } from './iff';

export interface CommInfo {
  channels: number;
  frames: number;
  bitsPerSample: number;
  sampleRate: number;
  /** AIFC compression type. 'NONE' for plain AIFF. 'sowt' means little-endian PCM. */
  compression: string;
  compressionName: string;
}

const latin1 = new TextDecoder('latin1');
const encoder = new TextEncoder();

/** Reads an 80-bit IEEE 754 extended float (sign, 15-bit exponent, 64-bit mantissa with explicit integer bit). */
export function readExtended(view: DataView, offset: number): number {
  const signAndExponent = view.getUint16(offset);
  const sign = signAndExponent & 0x8000 ? -1 : 1;
  const exponent = signAndExponent & 0x7fff;
  const hi = view.getUint32(offset + 2);
  const lo = view.getUint32(offset + 6);
  if (exponent === 0 && hi === 0 && lo === 0) return 0;
  const mantissa = hi * 2 ** 32 + lo;
  return sign * mantissa * 2 ** (exponent - 16383 - 63);
}

export function writeExtended(view: DataView, offset: number, value: number): void {
  if (value === 0) {
    for (let i = 0; i < 10; i++) view.setUint8(offset + i, 0);
    return;
  }
  const sign = value < 0 ? 0x8000 : 0;
  const abs = Math.abs(value);
  const exponent = Math.floor(Math.log2(abs));
  // abs / 2^exponent is in [1, 2), so the mantissa lands in [2^63, 2^64).
  const mantissa = abs * 2 ** (63 - exponent);
  const hi = Math.floor(mantissa / 2 ** 32);
  const lo = Math.floor(mantissa - hi * 2 ** 32);
  view.setUint16(offset, sign | (exponent + 16383));
  view.setUint32(offset + 2, hi);
  view.setUint32(offset + 6, lo);
}

export function parseComm(chunk: Chunk, formType: string): CommInfo {
  const d = chunk.data;
  if (d.length < 18) throw new Error('COMM chunk too short');
  const view = new DataView(d.buffer, d.byteOffset, d.byteLength);
  const info: CommInfo = {
    channels: view.getInt16(0),
    frames: view.getUint32(2),
    bitsPerSample: view.getInt16(6),
    sampleRate: readExtended(view, 8),
    compression: 'NONE',
    compressionName: '',
  };
  if (formType === 'AIFC' && d.length >= 22) {
    info.compression = latin1.decode(d.subarray(18, 22));
    const nameLength = d[22] ?? 0;
    info.compressionName = latin1.decode(d.subarray(23, 23 + nameLength));
  }
  return info;
}

export function buildComm(info: CommInfo, formType: string): Chunk {
  const name = encoder.encode(info.compressionName);
  // AIFC appends a 4-char compression type and a Pascal string padded to an even length.
  const extra = formType === 'AIFC' ? 4 + 1 + name.length + ((1 + name.length) % 2) : 0;
  const data = new Uint8Array(18 + extra);
  const view = new DataView(data.buffer);
  view.setInt16(0, info.channels);
  view.setUint32(2, info.frames);
  view.setInt16(6, info.bitsPerSample);
  writeExtended(view, 8, info.sampleRate);
  if (formType === 'AIFC') {
    data.set(encoder.encode(info.compression), 18);
    data[22] = name.length;
    data.set(name, 23);
  }
  return { id: 'COMM', data };
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `pnpm --filter @co-op/web test -- comm`
Expected: 6 passed.

- [ ] **Step 5: Commit**

```bash
git add web/src/core/codec/comm.ts web/src/core/codec/comm.test.ts
git commit -m "feat(codec): COMM chunk parser and extended float codec"
```

---

### Task 6: Metadata types and sample data

**Files:**
- Create: `web/src/core/codec/op1-meta.ts`, `web/test/fixtures/meta.ts`
- Test: `web/src/core/codec/op1-meta.test.ts`

- [ ] **Step 1: Create the sample metadata used by fixtures and tests**

These values are taken from real OP-1 files (via the teoperator project's defaults).

`web/test/fixtures/meta.ts`:
```ts
import type { SynthMeta, DrumMeta } from '../../src/core/codec/op1-meta';

export const SYNTH_META: SynthMeta = {
  adsr: [64, 64, 0, 64, 14336, 64, 4000, 4000],
  fx_active: true,
  fx_params: [64, -14337, 4515, 7232, 0, 0, 0, 0],
  fx_type: 'nitro',
  knobs: [3072, 0, 512, 3, 0, 0, 0, 0],
  lfo_active: false,
  lfo_params: [4608, 32767, 8448, 15360, 0, 0, 0, 0],
  lfo_type: 'value',
  name: 'default',
  octave: 0,
  synth_version: 2,
  type: 'cluster',
};

export const SAMPLER_META: SynthMeta = {
  adsr: [512, 10746, 32767, 10000, 4000, 64, 4000, 4000],
  base_freq: 261.6253662109375,
  fx_active: false,
  fx_params: [4480, 15544, 10788, 12104, 0, 0, 0, 0],
  fx_type: 'delay',
  knobs: [0, 0, 32767, 32767, 12000, 0, 0, 8304],
  lfo_active: false,
  lfo_params: [11840, 18431, 1024, 15144, 0, 0, 0, 0],
  lfo_type: 'random',
  name: '20150108_0251',
  octave: 0,
  synth_version: 1,
  type: 'sampler',
};

const slots = (value: number): number[] => Array.from({ length: 24 }, () => value);

export const DRUM_META: DrumMeta = {
  drum_version: 2,
  dyna_env: [0, 8192, 0, 8192, 0, 0, 0, 0],
  end: [
    97643143, 165163892, 211907777, 282025634, 313446583, 372916297, 413167412, 454132733,
    478541489, 492549640, 582028126, 642634075, 642634075, 642634075, 642634075, 642634075,
    642634075, 642634075, 642634075, 642634075, 642634075, 642634075, 642634075, 2032606256,
  ],
  fx_active: false,
  fx_params: [8000, 8000, 8000, 8000, 8000, 8000, 8000, 8000],
  fx_type: 'delay',
  lfo_active: false,
  lfo_params: [16000, 16000, 16000, 16000, 0, 0, 0, 0],
  lfo_type: 'tremolo',
  name: 'boombap1',
  octave: 0,
  pitch: slots(0),
  playmode: slots(8192),
  reverse: slots(8192),
  start: [
    0, 97647201, 165167950, 211911835, 282029692, 313450641, 372920355, 413171470,
    454136790, 478545547, 492553698, 582032184, 582032184, 582032184, 582032184, 582032184,
    582032184, 582032184, 582032184, 582032184, 582032184, 582032184, 582032184, 642638133,
  ],
  type: 'drum',
  volume: slots(8192),
};
```

- [ ] **Step 2: Write the failing tests**

`web/src/core/codec/op1-meta.test.ts`:
```ts
import { describe, it, expect } from 'vitest';
import { isSynthMeta, isDrumMeta, metaKind } from './op1-meta';
import { SYNTH_META, SAMPLER_META, DRUM_META } from '../../../test/fixtures/meta';

describe('op1 metadata guards', () => {
  it('recognises synth metadata including samplers', () => {
    expect(isSynthMeta(SYNTH_META)).toBe(true);
    expect(isSynthMeta(SAMPLER_META)).toBe(true);
    expect(isSynthMeta(DRUM_META)).toBe(false);
  });

  it('recognises drum metadata', () => {
    expect(isDrumMeta(DRUM_META)).toBe(true);
    expect(isDrumMeta(SYNTH_META)).toBe(false);
  });

  it('maps metadata to a patch kind', () => {
    expect(metaKind(SYNTH_META)).toBe('synth');
    expect(metaKind(DRUM_META)).toBe('drum');
    expect(metaKind({ name: 'mystery' })).toBe('unknown');
    expect(metaKind(null)).toBe('unknown');
  });

  it('tolerates extra fields from newer firmware', () => {
    expect(isSynthMeta({ ...SYNTH_META, future_field: [1, 2, 3] })).toBe(true);
  });
});
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `pnpm --filter @co-op/web test -- op1-meta`
Expected: FAIL, cannot resolve `./op1-meta`.

- [ ] **Step 4: Implement op1-meta.ts**

`web/src/core/codec/op1-meta.ts`:
```ts
export type PatchKind = 'synth' | 'drum' | 'unknown';

/** Fields common to every op-1 JSON blob. Unknown fields are kept and written back untouched. */
interface BaseMeta {
  name: string;
  octave: number;
  type: string;
  fx_active: boolean;
  fx_params: number[];
  fx_type: string;
  lfo_active: boolean;
  lfo_params: number[];
  lfo_type: string;
  [extra: string]: unknown;
}

export interface SynthMeta extends BaseMeta {
  synth_version: number;
  adsr: number[];
  knobs: number[];
  /** Present on sampler patches. */
  base_freq?: number;
}

export interface DrumMeta extends BaseMeta {
  type: 'drum';
  drum_version: number;
  dyna_env: number[];
  start: number[];
  end: number[];
  pitch: number[];
  playmode: number[];
  reverse: number[];
  volume: number[];
}

export type Op1Meta = SynthMeta | DrumMeta;

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

export function isSynthMeta(value: unknown): value is SynthMeta {
  return (
    isRecord(value) &&
    typeof value['synth_version'] === 'number' &&
    typeof value['type'] === 'string' &&
    value['type'] !== 'drum' &&
    Array.isArray(value['adsr']) &&
    Array.isArray(value['knobs'])
  );
}

export function isDrumMeta(value: unknown): value is DrumMeta {
  return (
    isRecord(value) &&
    value['type'] === 'drum' &&
    typeof value['drum_version'] === 'number' &&
    Array.isArray(value['start']) &&
    Array.isArray(value['end'])
  );
}

export function metaKind(value: unknown): PatchKind {
  if (isDrumMeta(value)) return 'drum';
  if (isSynthMeta(value)) return 'synth';
  return 'unknown';
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `pnpm --filter @co-op/web test -- op1-meta`
Expected: 4 passed.

- [ ] **Step 6: Commit**

```bash
git add web/src/core/codec/op1-meta.ts web/src/core/codec/op1-meta.test.ts web/test/fixtures/meta.ts
git commit -m "feat(codec): synth and drum metadata types with guards"
```

---

### Task 7: APPL "op-1" chunk codec

**Files:**
- Create: `web/src/core/codec/op1-chunk.ts`
- Test: `web/src/core/codec/op1-chunk.test.ts`

- [ ] **Step 1: Write the failing tests**

`web/src/core/codec/op1-chunk.test.ts`:
```ts
import { describe, it, expect } from 'vitest';
import { isOp1Chunk, parseOp1Chunk, buildOp1Chunk } from './op1-chunk';
import { SYNTH_META, DRUM_META } from '../../../test/fixtures/meta';

const text = (s: string) => new TextEncoder().encode(s);

describe('op-1 APPL chunk', () => {
  it('detects the op-1 signature', () => {
    expect(isOp1Chunk({ id: 'APPL', data: text('op-1{}') })).toBe(true);
    expect(isOp1Chunk({ id: 'APPL', data: text('abcd{}') })).toBe(false);
    expect(isOp1Chunk({ id: 'SSND', data: text('op-1{}') })).toBe(false);
  });

  it('parses JSON and keeps the trailing filler bytes', () => {
    const chunk = { id: 'APPL', data: text('op-1{"name":"a}b","octave":1}\n\x1e') };
    const parsed = parseOp1Chunk(chunk);
    expect(parsed.meta).toEqual({ name: 'a}b', octave: 1 });
    expect(Array.from(parsed.trailing)).toEqual([0x0a, 0x1e]);
  });

  it('builds a chunk with sorted keys, a newline filler and an even length', () => {
    const chunk = buildOp1Chunk({ octave: 0, name: 'zz' });
    const body = new TextDecoder().decode(chunk.data);
    expect(body.startsWith('op-1{"name":"zz","octave":0}')).toBe(true);
    expect(chunk.data.length % 2).toBe(0);
  });

  it('round-trips real synth and drum metadata byte for byte', () => {
    for (const meta of [SYNTH_META, DRUM_META]) {
      const built = buildOp1Chunk(meta);
      const parsed = parseOp1Chunk(built);
      expect(parsed.meta).toEqual(meta);
      expect(Array.from(buildOp1Chunk(parsed.meta, parsed.trailing))).toEqual(Array.from(built));
    }
  });

  it('rejects a chunk without a JSON object', () => {
    expect(() => parseOp1Chunk({ id: 'APPL', data: text('op-1nothing') })).toThrow(/no JSON/);
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `pnpm --filter @co-op/web test -- op1-chunk`
Expected: FAIL, cannot resolve `./op1-chunk`.

- [ ] **Step 3: Implement op1-chunk.ts**

`web/src/core/codec/op1-chunk.ts`:
```ts
import type { Chunk } from './iff';

export const OP1_SIGNATURE = 'op-1';

const encoder = new TextEncoder();
const latin1 = new TextDecoder('latin1');
const utf8 = new TextDecoder('utf-8');

export interface Op1ChunkContent {
  meta: Record<string, unknown>;
  /** Bytes after the JSON's closing brace: the OP-1 filler (0x0A then 0x1E bytes). */
  trailing: Uint8Array;
}

export function isOp1Chunk(chunk: Chunk): boolean {
  return (
    chunk.id === 'APPL' &&
    chunk.data.length >= 4 &&
    latin1.decode(chunk.data.subarray(0, 4)) === OP1_SIGNATURE
  );
}

export function parseOp1Chunk(chunk: Chunk): Op1ChunkContent {
  if (!isOp1Chunk(chunk)) throw new Error('not an op-1 APPL chunk');
  const body = chunk.data.subarray(4);
  const end = body.lastIndexOf(0x7d); // '}'
  if (end < 0) throw new Error('op-1 chunk has no JSON object');
  const meta: unknown = JSON.parse(utf8.decode(body.subarray(0, end + 1)));
  if (typeof meta !== 'object' || meta === null || Array.isArray(meta)) {
    throw new Error('op-1 chunk JSON is not an object');
  }
  return { meta: meta as Record<string, unknown>, trailing: body.slice(end + 1) };
}

function sortKeys(meta: Record<string, unknown>): Record<string, unknown> {
  return Object.fromEntries(Object.keys(meta).sort().map((k) => [k, meta[k]]));
}

/**
 * Serialises metadata the way the OP-1 does: compact JSON with alphabetical keys,
 * a newline, then 0x1E bytes until the chunk length is even.
 */
export function buildOp1Chunk(meta: Record<string, unknown>, trailing?: Uint8Array): Chunk {
  const json = encoder.encode(JSON.stringify(sortKeys(meta)));
  const filler = trailing && trailing.length > 0 ? Array.from(trailing) : [0x0a];
  if ((4 + json.length + filler.length) % 2 === 1) filler.push(0x1e);
  const data = new Uint8Array(4 + json.length + filler.length);
  data.set(encoder.encode(OP1_SIGNATURE), 0);
  data.set(json, 4);
  data.set(filler, 4 + json.length);
  return { id: 'APPL', data };
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `pnpm --filter @co-op/web test -- op1-chunk`
Expected: 5 passed.

- [ ] **Step 5: Commit**

```bash
git add web/src/core/codec/op1-chunk.ts web/src/core/codec/op1-chunk.test.ts
git commit -m "feat(codec): op-1 APPL chunk parser and builder"
```

---

### Task 8: SSND decode and encode

**Files:**
- Create: `web/src/core/codec/ssnd.ts`
- Test: `web/src/core/codec/ssnd.test.ts`

- [ ] **Step 1: Write the failing tests**

`web/src/core/codec/ssnd.test.ts`:
```ts
import { describe, it, expect } from 'vitest';
import { buildSsnd, decodeSsnd } from './ssnd';
import type { CommInfo } from './comm';

const comm = (compression: string, channels = 1): CommInfo => ({
  channels,
  frames: 4,
  bitsPerSample: 16,
  sampleRate: 44100,
  compression,
  compressionName: '',
});

describe('SSND', () => {
  const samples = Int16Array.from([0, 16384, -32768, 32767]);

  it('encodes big-endian with an 8-byte header', () => {
    const chunk = buildSsnd(samples, false);
    expect(chunk.id).toBe('SSND');
    expect(Array.from(chunk.data.subarray(0, 8))).toEqual([0, 0, 0, 0, 0, 0, 0, 0]);
    expect(Array.from(chunk.data.subarray(8, 12))).toEqual([0x00, 0x00, 0x40, 0x00]);
  });

  it('encodes little-endian when asked', () => {
    const chunk = buildSsnd(samples, true);
    expect(Array.from(chunk.data.subarray(8, 12))).toEqual([0x00, 0x00, 0x00, 0x40]);
  });

  it('decodes big-endian AIFF to floats in [-1, 1)', () => {
    const out = decodeSsnd(buildSsnd(samples, false), comm('NONE'));
    expect(Array.from(out)).toEqual([0, 0.5, -1, 32767 / 32768]);
  });

  it('decodes sowt little-endian AIFC', () => {
    const out = decodeSsnd(buildSsnd(samples, true), comm('sowt'));
    expect(Array.from(out)).toEqual([0, 0.5, -1, 32767 / 32768]);
  });

  it('mixes stereo down to mono', () => {
    const stereo = Int16Array.from([16384, -16384, 32767, 32767]);
    const out = decodeSsnd(buildSsnd(stereo, false), comm('NONE', 2));
    expect(Array.from(out)).toEqual([0, 32767 / 32768]);
  });

  it('rejects unsupported bit depths and compression types', () => {
    const chunk = buildSsnd(samples, false);
    expect(() => decodeSsnd(chunk, { ...comm('NONE'), bitsPerSample: 24 })).toThrow(/bit depth/);
    expect(() => decodeSsnd(chunk, comm('ulaw'))).toThrow(/compression/);
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `pnpm --filter @co-op/web test -- ssnd`
Expected: FAIL, cannot resolve `./ssnd`.

- [ ] **Step 3: Implement ssnd.ts**

`web/src/core/codec/ssnd.ts`:
```ts
import type { Chunk } from './iff';
import type { CommInfo } from './comm';

export class SsndError extends Error {}

/** Builds an SSND chunk from interleaved 16-bit samples with a zero offset and block size. */
export function buildSsnd(samples: Int16Array, littleEndian: boolean): Chunk {
  const data = new Uint8Array(8 + samples.length * 2);
  const view = new DataView(data.buffer);
  for (let i = 0; i < samples.length; i++) {
    view.setInt16(8 + i * 2, samples[i]!, littleEndian);
  }
  return { id: 'SSND', data };
}

/** Decodes 16-bit PCM to mono floats in [-1, 1), averaging channels. */
export function decodeSsnd(chunk: Chunk, comm: CommInfo): Float32Array {
  if (comm.bitsPerSample !== 16) {
    throw new SsndError(`unsupported bit depth ${comm.bitsPerSample}`);
  }
  const compression = comm.compression;
  if (compression !== 'NONE' && compression !== 'twos' && compression !== 'sowt') {
    throw new SsndError(`unsupported compression ${compression}`);
  }
  const littleEndian = compression === 'sowt';
  const view = new DataView(chunk.data.buffer, chunk.data.byteOffset, chunk.data.byteLength);
  const start = 8 + view.getUint32(0);
  const channels = Math.max(1, comm.channels);
  const frames = Math.floor((chunk.data.length - start) / (2 * channels));
  const out = new Float32Array(frames);
  for (let frame = 0; frame < frames; frame++) {
    let sum = 0;
    for (let ch = 0; ch < channels; ch++) {
      sum += view.getInt16(start + (frame * channels + ch) * 2, littleEndian);
    }
    out[frame] = sum / channels / 32768;
  }
  return out;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `pnpm --filter @co-op/web test -- ssnd`
Expected: 6 passed.

- [ ] **Step 5: Commit**

```bash
git add web/src/core/codec/ssnd.ts web/src/core/codec/ssnd.test.ts
git commit -m "feat(codec): SSND encode and decode with sowt support"
```

---

### Task 9: Fixture generator

**Files:**
- Create: `web/test/fixtures/build-fixtures.ts`
- Generated: `web/test/fixtures/synth-cluster.aif`, `web/test/fixtures/synth-sampler-sowt.aif`, `web/test/fixtures/drum-boombap.aif`, `web/test/fixtures/no-metadata.aif`

- [ ] **Step 1: Write the generator**

`web/test/fixtures/build-fixtures.ts`:
```ts
import { writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { writeForm, type Chunk } from '../../src/core/codec/iff';
import { buildComm } from '../../src/core/codec/comm';
import { buildOp1Chunk } from '../../src/core/codec/op1-chunk';
import { buildSsnd } from '../../src/core/codec/ssnd';
import { SYNTH_META, SAMPLER_META, DRUM_META } from './meta';

const SAMPLE_RATE = 44100;

function sine(seconds: number, hz: number): Int16Array {
  const frames = Math.round(seconds * SAMPLE_RATE);
  const out = new Int16Array(frames);
  for (let i = 0; i < frames; i++) {
    out[i] = Math.round(Math.sin((2 * Math.PI * hz * i) / SAMPLE_RATE) * 12000);
  }
  return out;
}

function fverChunk(): Chunk {
  const data = new Uint8Array(4);
  new DataView(data.buffer).setUint32(0, 0xa2805140); // AIFC version 1 timestamp
  return { id: 'FVER', data };
}

interface Spec {
  file: string;
  formType: 'AIFF' | 'AIFC';
  meta: Record<string, unknown> | null;
  seconds: number;
  hz: number;
}

const specs: Spec[] = [
  { file: 'synth-cluster.aif', formType: 'AIFF', meta: SYNTH_META, seconds: 0.25, hz: 220 },
  { file: 'synth-sampler-sowt.aif', formType: 'AIFC', meta: SAMPLER_META, seconds: 0.25, hz: 261.63 },
  { file: 'drum-boombap.aif', formType: 'AIFF', meta: DRUM_META, seconds: 0.5, hz: 110 },
  { file: 'no-metadata.aif', formType: 'AIFF', meta: null, seconds: 0.1, hz: 440 },
];

for (const spec of specs) {
  const samples = sine(spec.seconds, spec.hz);
  const little = spec.formType === 'AIFC';
  const chunks: Chunk[] = [];
  if (spec.formType === 'AIFC') chunks.push(fverChunk());
  chunks.push(
    buildComm(
      {
        channels: 1,
        frames: samples.length,
        bitsPerSample: 16,
        sampleRate: SAMPLE_RATE,
        compression: little ? 'sowt' : 'NONE',
        compressionName: little ? 'not compressed' : '',
      },
      spec.formType,
    ),
  );
  if (spec.meta) chunks.push(buildOp1Chunk(spec.meta));
  chunks.push(buildSsnd(samples, little));
  const bytes = writeForm({ formType: spec.formType, chunks });
  const target = fileURLToPath(new URL(spec.file, import.meta.url));
  writeFileSync(target, bytes);
  console.log(`${spec.file}: ${bytes.length} bytes`);
}
```

- [ ] **Step 2: Generate the fixtures**

Run: `pnpm --filter @co-op/web fixtures`
Expected: four lines like `synth-cluster.aif: 22xxx bytes` and four `.aif` files in `web/test/fixtures`.

- [ ] **Step 3: Sanity-check one file's layout by hand**

Run: `xxd web/test/fixtures/synth-cluster.aif | head -8`
Expected: first line starts with `FORM`, then `AIFF`, then `COMM`; an `APPL` chunk with `op-1{"adsr":[64,...` appears before `SSND`.

- [ ] **Step 4: Commit**

```bash
git add web/test/fixtures
git commit -m "test: add OP-1 patch fixture generator and generated fixtures"
```

---

### Task 10: Patch model

**Files:**
- Create: `web/src/core/codec/patch.ts`
- Test: `web/src/core/codec/patch.test.ts`

- [ ] **Step 1: Write the failing tests**

`web/src/core/codec/patch.test.ts`:
```ts
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { readPatch, writePatch, withMeta, decodePcm, PatchError } from './patch';
import { SYNTH_META, SAMPLER_META, DRUM_META } from '../../../test/fixtures/meta';

const fixture = (name: string): Uint8Array =>
  new Uint8Array(readFileSync(fileURLToPath(new URL(`../../../test/fixtures/${name}`, import.meta.url))));

describe('readPatch', () => {
  it('reads a synth patch', () => {
    const patch = readPatch(fixture('synth-cluster.aif'));
    expect(patch.kind).toBe('synth');
    expect(patch.name).toBe('default');
    expect(patch.meta).toEqual(SYNTH_META);
    expect(patch.comm.sampleRate).toBe(44100);
    expect(patch.comm.compression).toBe('NONE');
  });

  it('reads a little-endian AIFC sampler patch', () => {
    const patch = readPatch(fixture('synth-sampler-sowt.aif'));
    expect(patch.kind).toBe('synth');
    expect(patch.meta).toEqual(SAMPLER_META);
    expect(patch.comm.compression).toBe('sowt');
  });

  it('reads a drum patch', () => {
    const patch = readPatch(fixture('drum-boombap.aif'));
    expect(patch.kind).toBe('drum');
    expect(patch.name).toBe('boombap1');
    expect(patch.meta).toEqual(DRUM_META);
  });

  it('reads an AIFF with no op-1 metadata as unknown', () => {
    const patch = readPatch(fixture('no-metadata.aif'));
    expect(patch.kind).toBe('unknown');
    expect(patch.meta).toBeNull();
    expect(patch.name).toBe('');
  });

  it('rejects non-AIFF input', () => {
    expect(() => readPatch(new TextEncoder().encode('hello'))).toThrow(PatchError);
  });
});

describe('writePatch', () => {
  it('round-trips every fixture byte for byte when nothing changed', () => {
    for (const name of ['synth-cluster.aif', 'synth-sampler-sowt.aif', 'drum-boombap.aif', 'no-metadata.aif']) {
      const bytes = fixture(name);
      expect(Array.from(writePatch(readPatch(bytes)))).toEqual(Array.from(bytes));
    }
  });

  it('writes an edited name that reads back', () => {
    const original = readPatch(fixture('synth-cluster.aif'));
    const renamed = withMeta(original, { ...SYNTH_META, name: 'renamed' });
    const reread = readPatch(writePatch(renamed));
    expect(reread.name).toBe('renamed');
    expect(reread.meta).toEqual({ ...SYNTH_META, name: 'renamed' });
    expect(Array.from(decodePcm(reread))).toEqual(Array.from(decodePcm(original)));
  });

  it('adds an APPL chunk before SSND when a file had none', () => {
    const plain = readPatch(fixture('no-metadata.aif'));
    const tagged = withMeta(plain, DRUM_META);
    const bytes = writePatch(tagged);
    const reread = readPatch(bytes);
    expect(reread.kind).toBe('drum');
    const ids = reread.form.chunks.map((c) => c.id);
    expect(ids.indexOf('APPL')).toBeLessThan(ids.indexOf('SSND'));
  });
});

describe('decodePcm', () => {
  it('returns the expected number of frames', () => {
    const patch = readPatch(fixture('synth-cluster.aif'));
    expect(decodePcm(patch).length).toBe(Math.round(0.25 * 44100));
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `pnpm --filter @co-op/web test -- patch`
Expected: FAIL, cannot resolve `./patch`.

- [ ] **Step 3: Implement patch.ts**

`web/src/core/codec/patch.ts`:
```ts
import { readForm, writeForm, type IffForm, type Chunk } from './iff';
import { parseComm, type CommInfo } from './comm';
import { isOp1Chunk, parseOp1Chunk, buildOp1Chunk } from './op1-chunk';
import { metaKind, type Op1Meta, type PatchKind } from './op1-meta';
import { decodeSsnd } from './ssnd';

export interface Patch {
  kind: PatchKind;
  /** The name stored in the op-1 metadata, or '' when there is none. */
  name: string;
  meta: Op1Meta | null;
  comm: CommInfo;
  form: IffForm;
  /** The bytes this patch was read from. Unchanged operations copy these verbatim. */
  raw: Uint8Array;
  /** Filler bytes that followed the JSON in the original APPL chunk. */
  op1Trailing: Uint8Array | null;
}

export class PatchError extends Error {}

export function readPatch(bytes: Uint8Array): Patch {
  let form: IffForm;
  try {
    form = readForm(bytes);
  } catch (error) {
    throw new PatchError(`not an AIFF file: ${(error as Error).message}`);
  }
  if (form.formType !== 'AIFF' && form.formType !== 'AIFC') {
    throw new PatchError(`unsupported FORM type ${form.formType}`);
  }
  const commChunk = form.chunks.find((c) => c.id === 'COMM');
  if (!commChunk) throw new PatchError('missing COMM chunk');
  const comm = parseComm(commChunk, form.formType);

  let meta: Op1Meta | null = null;
  let op1Trailing: Uint8Array | null = null;
  const applChunk = form.chunks.find(isOp1Chunk);
  if (applChunk) {
    try {
      const content = parseOp1Chunk(applChunk);
      meta = content.meta as Op1Meta;
      op1Trailing = content.trailing;
    } catch (error) {
      throw new PatchError(`bad op-1 metadata: ${(error as Error).message}`);
    }
  }
  const kind = metaKind(meta);
  const name = typeof meta?.name === 'string' ? meta.name : '';
  return { kind, name, meta, comm, form, raw: bytes, op1Trailing };
}

/** Returns a copy of the patch carrying new metadata. The audio chunks are untouched. */
export function withMeta(patch: Patch, meta: Op1Meta): Patch {
  return { ...patch, meta, kind: metaKind(meta), name: meta.name };
}

/** Serialises the patch, rebuilding only the op-1 APPL chunk. */
export function writePatch(patch: Patch): Uint8Array {
  const chunks: Chunk[] = [];
  let wroteAppl = false;
  for (const chunk of patch.form.chunks) {
    if (isOp1Chunk(chunk)) {
      if (patch.meta) {
        chunks.push(buildOp1Chunk(patch.meta, patch.op1Trailing ?? undefined));
        wroteAppl = true;
      }
      continue;
    }
    if (chunk.id === 'SSND' && patch.meta && !wroteAppl) {
      chunks.push(buildOp1Chunk(patch.meta, patch.op1Trailing ?? undefined));
      wroteAppl = true;
    }
    chunks.push(chunk);
  }
  if (patch.meta && !wroteAppl) chunks.push(buildOp1Chunk(patch.meta));
  return writeForm({ formType: patch.form.formType, chunks });
}

/** Decodes the sound data to mono floats for preview. */
export function decodePcm(patch: Patch): Float32Array {
  const ssnd = patch.form.chunks.find((c) => c.id === 'SSND');
  if (!ssnd) throw new PatchError('missing SSND chunk');
  return decodeSsnd(ssnd, patch.comm);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `pnpm --filter @co-op/web test -- patch`
Expected: 9 passed.

- [ ] **Step 5: Run the whole suite and typecheck**

Run: `pnpm typecheck && pnpm test`
Expected: no type errors, all tests pass.

- [ ] **Step 6: Commit**

```bash
git add web/src/core/codec/patch.ts web/src/core/codec/patch.test.ts
git commit -m "feat(codec): patch model with read, write, withMeta and decodePcm"
```

---

### Task 11: Directory listing interface and in-memory implementation

**Files:**
- Create: `web/src/core/store/listing.ts`, `web/src/core/store/memory-listing.ts`
- Test: `web/src/core/store/memory-listing.test.ts`

- [ ] **Step 1: Write the failing test**

`web/src/core/store/memory-listing.test.ts`:
```ts
import { describe, it, expect } from 'vitest';
import { MemoryListing } from './memory-listing';

describe('MemoryListing', () => {
  const listing = new MemoryListing({
    synth: { user: { 'a.aif': 'file', 'b.aif': 'file' }, snapshot: {} },
    drum: { user: {} },
    tape: { 'track1.aif': 'file' },
  });

  it('lists the root', async () => {
    const entries = await listing.list('');
    expect(entries).toEqual([
      { name: 'drum', kind: 'dir' },
      { name: 'synth', kind: 'dir' },
      { name: 'tape', kind: 'dir' },
    ]);
  });

  it('lists nested folders with files', async () => {
    expect(await listing.list('synth/user')).toEqual([
      { name: 'a.aif', kind: 'file' },
      { name: 'b.aif', kind: 'file' },
    ]);
  });

  it('returns an empty list for a missing path', async () => {
    expect(await listing.list('nope/nothing')).toEqual([]);
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `pnpm --filter @co-op/web test -- memory-listing`
Expected: FAIL, cannot resolve `./memory-listing`.

- [ ] **Step 3: Implement the interface and the in-memory listing**

`web/src/core/store/listing.ts`:
```ts
export interface DirEntry {
  name: string;
  kind: 'file' | 'dir';
}

/**
 * Read-only view of a directory tree. Paths are '/'-separated and relative to
 * the root; '' is the root itself. Entries come back sorted by name.
 */
export interface DirectoryListing {
  list(path: string): Promise<DirEntry[]>;
}
```

`web/src/core/store/memory-listing.ts`:
```ts
import type { DirectoryListing, DirEntry } from './listing';

/** A folder is an object; a file is the string 'file'. */
export type MemoryTree = { [name: string]: MemoryTree | 'file' };

export class MemoryListing implements DirectoryListing {
  constructor(private readonly tree: MemoryTree) {}

  async list(path: string): Promise<DirEntry[]> {
    let node: MemoryTree | 'file' = this.tree;
    for (const part of path.split('/').filter(Boolean)) {
      if (node === 'file') return [];
      const next: MemoryTree | 'file' | undefined = node[part];
      if (next === undefined) return [];
      node = next;
    }
    if (node === 'file') return [];
    return Object.entries(node)
      .map(([name, value]): DirEntry => ({ name, kind: value === 'file' ? 'file' : 'dir' }))
      .sort((a, b) => a.name.localeCompare(b.name));
  }
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `pnpm --filter @co-op/web test -- memory-listing`
Expected: 3 passed.

- [ ] **Step 5: Commit**

```bash
git add web/src/core/store
git commit -m "feat(store): DirectoryListing interface with in-memory implementation"
```

---

### Task 12: Device profile types and the stock OP-1 profile

**Files:**
- Create: `web/src/core/profiles/types.ts`, `web/src/core/profiles/op1-stock.ts`
- Test: `web/src/core/profiles/op1-stock.test.ts`

- [ ] **Step 1: Write the failing tests**

`web/src/core/profiles/op1-stock.test.ts`:
```ts
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { op1Stock, STOCK_SYNTH_ENGINES } from './op1-stock';
import { MemoryListing } from '../store/memory-listing';
import { readPatch, withMeta } from '../codec/patch';
import { SYNTH_META } from '../../../test/fixtures/meta';

const fixture = (name: string) =>
  readPatch(new Uint8Array(readFileSync(fileURLToPath(new URL(`../../../test/fixtures/${name}`, import.meta.url)))));

describe('op1/stock detection', () => {
  it('is confident when synth, drum and tape folders exist', async () => {
    const root = new MemoryListing({ album: {}, drum: {}, synth: {}, tape: {} });
    const result = await op1Stock.detect(root);
    expect(result.confidence).toBeGreaterThanOrEqual(0.9);
    expect(result.reason).toMatch(/synth, drum and tape/);
  });

  it('ignores folder name case', async () => {
    const root = new MemoryListing({ DRUM: {}, SYNTH: {}, TAPE: {} });
    expect((await op1Stock.detect(root)).confidence).toBeGreaterThanOrEqual(0.9);
  });

  it('is unsure with only two of the folders', async () => {
    const root = new MemoryListing({ drum: {}, synth: {} });
    expect((await op1Stock.detect(root)).confidence).toBe(0.5);
  });

  it('rejects an unrelated folder', async () => {
    const root = new MemoryListing({ Documents: {}, Music: {} });
    expect((await op1Stock.detect(root)).confidence).toBe(0);
  });
});

describe('op1/stock compatibility', () => {
  it('accepts stock synth and drum patches', () => {
    expect(op1Stock.compatibility(fixture('synth-cluster.aif'))).toEqual({ level: 'ok' });
    expect(op1Stock.compatibility(fixture('drum-boombap.aif'))).toEqual({ level: 'ok' });
  });

  it('warns on an engine stock firmware does not have', () => {
    const patch = withMeta(fixture('synth-cluster.aif'), { ...SYNTH_META, type: 'wavetable' });
    const compat = op1Stock.compatibility(patch);
    expect(compat.level).toBe('warn');
    expect(compat.reason).toMatch(/wavetable/);
  });

  it('marks files without op-1 metadata unsupported', () => {
    expect(op1Stock.compatibility(fixture('no-metadata.aif')).level).toBe('unsupported');
  });

  it('knows every stock engine', () => {
    expect(STOCK_SYNTH_ENGINES).toContain('cluster');
    expect(STOCK_SYNTH_ENGINES).toContain('sampler');
    expect(STOCK_SYNTH_ENGINES).toContain('iter');
  });
});

describe('op1/stock layout', () => {
  it('scans synth and drum as patch roots', () => {
    expect(op1Stock.layout.patchRoots).toEqual([
      { path: 'synth', kind: 'synth' },
      { path: 'drum', kind: 'drum' },
    ]);
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `pnpm --filter @co-op/web test -- op1-stock`
Expected: FAIL, cannot resolve `./op1-stock`.

- [ ] **Step 3: Implement the types**

`web/src/core/profiles/types.ts`:
```ts
import type { Patch } from '../codec/patch';
import type { PatchKind } from '../codec/op1-meta';
import type { DirectoryListing } from '../store/listing';

export type DeviceId = 'op1' | 'op1-field' | 'opxy';

export interface PatchRoot {
  /** Folder relative to the drive root, for example 'synth'. Its subfolders hold patches. */
  path: string;
  kind: PatchKind;
}

export interface DriveLayout {
  patchRoots: PatchRoot[];
  /** Folders that exist on the drive but hold no patches, kept for detection and backups. */
  otherRoots: string[];
}

export interface PatchKindSpec {
  kind: PatchKind;
  /** Lower-case extensions including the dot. */
  extensions: string[];
}

export interface DetectResult {
  /** 0 means definitely not this profile, 1 means certain. */
  confidence: number;
  reason: string;
}

export interface Compat {
  level: 'ok' | 'warn' | 'unsupported';
  reason?: string;
}

export interface DeviceProfile {
  /** '<device>/<firmware>', for example 'op1/stock'. */
  id: string;
  device: DeviceId;
  firmware: string;
  label: string;
  layout: DriveLayout;
  patchKinds: PatchKindSpec[];
  detect(root: DirectoryListing): Promise<DetectResult>;
  compatibility(patch: Patch): Compat;
}
```

- [ ] **Step 4: Implement the stock profile**

`web/src/core/profiles/op1-stock.ts`:
```ts
import type { DeviceProfile, DetectResult, Compat } from './types';
import type { DirectoryListing } from '../store/listing';
import type { Patch } from '../codec/patch';
import { isSynthMeta } from '../codec/op1-meta';

/** Synth engine names as they appear in the op-1 JSON `type` field on stock firmware. */
export const STOCK_SYNTH_ENGINES: readonly string[] = [
  'cluster', 'digital', 'dna', 'drwave', 'dsynth', 'fm', 'iter',
  'phase', 'pulse', 'sampler', 'string', 'voltage',
];

const SIGNATURE_FOLDERS = ['synth', 'drum', 'tape'];

async function detect(root: DirectoryListing): Promise<DetectResult> {
  const entries = await root.list('');
  const dirs = new Set(entries.filter((e) => e.kind === 'dir').map((e) => e.name.toLowerCase()));
  const found = SIGNATURE_FOLDERS.filter((name) => dirs.has(name));
  if (found.length === SIGNATURE_FOLDERS.length) {
    return { confidence: 0.9, reason: 'found synth, drum and tape folders' };
  }
  if (found.length === 2) {
    return { confidence: 0.5, reason: `found ${found.join(' and ')} folders but not ${SIGNATURE_FOLDERS.filter((n) => !found.includes(n)).join(', ')}` };
  }
  return { confidence: 0, reason: 'no OP-1 folders found' };
}

function compatibility(patch: Patch): Compat {
  if (patch.kind === 'unknown' || !patch.meta) {
    return { level: 'unsupported', reason: 'file has no op-1 metadata' };
  }
  if (patch.kind === 'synth' && isSynthMeta(patch.meta) && !STOCK_SYNTH_ENGINES.includes(patch.meta.type)) {
    return { level: 'warn', reason: `engine "${patch.meta.type}" is not part of stock firmware` };
  }
  return { level: 'ok' };
}

export const op1Stock: DeviceProfile = {
  id: 'op1/stock',
  device: 'op1',
  firmware: 'stock',
  label: 'OP-1 (stock firmware)',
  layout: {
    patchRoots: [
      { path: 'synth', kind: 'synth' },
      { path: 'drum', kind: 'drum' },
    ],
    otherRoots: ['tape', 'album'],
  },
  patchKinds: [
    { kind: 'synth', extensions: ['.aif', '.aiff'] },
    { kind: 'drum', extensions: ['.aif', '.aiff'] },
  ],
  detect,
  compatibility,
};
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `pnpm --filter @co-op/web test -- op1-stock`
Expected: 9 passed.

- [ ] **Step 6: Commit**

```bash
git add web/src/core/profiles/types.ts web/src/core/profiles/op1-stock.ts web/src/core/profiles/op1-stock.test.ts
git commit -m "feat(profiles): DeviceProfile types and stock OP-1 profile"
```

---

### Task 13: Drive scanner

**Files:**
- Create: `web/src/core/profiles/scan.ts`
- Test: `web/src/core/profiles/scan.test.ts`

- [ ] **Step 1: Write the failing tests**

`web/src/core/profiles/scan.test.ts`:
```ts
import { describe, it, expect } from 'vitest';
import { scanDrive } from './scan';
import { op1Stock } from './op1-stock';
import { MemoryListing } from '../store/memory-listing';

describe('scanDrive', () => {
  const root = new MemoryListing({
    synth: {
      user: { '1.aif': 'file', '2.AIF': 'file', 'notes.txt': 'file' },
      snapshot: { '20240101_1200.aif': 'file' },
      'stray.aif': 'file',
    },
    drum: { user: { 'kit.aif': 'file' }, empty: {} },
    tape: { 'track1.aif': 'file' },
  });

  it('finds patches one folder below each patch root, matching extensions case-insensitively', async () => {
    const found = await scanDrive(root, op1Stock);
    expect(found).toEqual([
      { path: 'synth/snapshot/20240101_1200.aif', folder: 'snapshot', fileName: '20240101_1200.aif', kind: 'synth' },
      { path: 'synth/user/1.aif', folder: 'user', fileName: '1.aif', kind: 'synth' },
      { path: 'synth/user/2.AIF', folder: 'user', fileName: '2.AIF', kind: 'synth' },
      { path: 'drum/user/kit.aif', folder: 'user', fileName: 'kit.aif', kind: 'drum' },
    ]);
  });

  it('returns nothing for a drive without patch roots', async () => {
    expect(await scanDrive(new MemoryListing({ Music: {} }), op1Stock)).toEqual([]);
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `pnpm --filter @co-op/web test -- scan`
Expected: FAIL, cannot resolve `./scan`.

- [ ] **Step 3: Implement scan.ts**

`web/src/core/profiles/scan.ts`:
```ts
import type { DeviceProfile } from './types';
import type { DirectoryListing } from '../store/listing';
import type { PatchKind } from '../codec/op1-meta';

export interface ScannedPatch {
  /** Path relative to the drive root, for example 'synth/user/1.aif'. */
  path: string;
  /** The category folder under the patch root, for example 'user'. */
  folder: string;
  fileName: string;
  kind: PatchKind;
}

function extensionOf(fileName: string): string {
  const dot = fileName.lastIndexOf('.');
  return dot < 0 ? '' : fileName.slice(dot).toLowerCase();
}

/** Enumerates patch files without reading them: <root>/<folder>/<file> for each patch root. */
export async function scanDrive(root: DirectoryListing, profile: DeviceProfile): Promise<ScannedPatch[]> {
  const found: ScannedPatch[] = [];
  for (const patchRoot of profile.layout.patchRoots) {
    const extensions = profile.patchKinds.find((k) => k.kind === patchRoot.kind)?.extensions ?? [];
    const folders = (await root.list(patchRoot.path)).filter((e) => e.kind === 'dir');
    for (const folder of folders) {
      const folderPath = `${patchRoot.path}/${folder.name}`;
      for (const entry of await root.list(folderPath)) {
        if (entry.kind !== 'file' || !extensions.includes(extensionOf(entry.name))) continue;
        found.push({
          path: `${folderPath}/${entry.name}`,
          folder: folder.name,
          fileName: entry.name,
          kind: patchRoot.kind,
        });
      }
    }
  }
  return found;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `pnpm --filter @co-op/web test -- scan`
Expected: 2 passed.

- [ ] **Step 5: Commit**

```bash
git add web/src/core/profiles/scan.ts web/src/core/profiles/scan.test.ts
git commit -m "feat(profiles): drive scanner driven by profile layout"
```

---

### Task 14: Profile registry and detection

**Files:**
- Create: `web/src/core/profiles/registry.ts`
- Test: `web/src/core/profiles/registry.test.ts`

- [ ] **Step 1: Write the failing tests**

`web/src/core/profiles/registry.test.ts`:
```ts
import { describe, it, expect } from 'vitest';
import { profiles, getProfile, detectProfiles } from './registry';
import { MemoryListing } from '../store/memory-listing';

describe('profile registry', () => {
  it('lists the stock OP-1 profile', () => {
    expect(profiles.map((p) => p.id)).toEqual(['op1/stock']);
  });

  it('looks a profile up by id', () => {
    expect(getProfile('op1/stock')?.label).toBe('OP-1 (stock firmware)');
    expect(getProfile('nope')).toBeUndefined();
  });

  it('ranks profiles by detection confidence', async () => {
    const root = new MemoryListing({ drum: {}, synth: {}, tape: {} });
    const ranked = await detectProfiles(root);
    expect(ranked[0]?.profile.id).toBe('op1/stock');
    expect(ranked[0]?.result.confidence).toBeGreaterThan(0);
  });

  it('returns an empty ranking when nothing matches', async () => {
    expect(await detectProfiles(new MemoryListing({ Music: {} }))).toEqual([]);
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `pnpm --filter @co-op/web test -- registry`
Expected: FAIL, cannot resolve `./registry`.

- [ ] **Step 3: Implement registry.ts**

`web/src/core/profiles/registry.ts`:
```ts
import type { DeviceProfile, DetectResult } from './types';
import type { DirectoryListing } from '../store/listing';
import { op1Stock } from './op1-stock';

/** Every supported profile. Later firmware profiles are appended here. */
export const profiles: readonly DeviceProfile[] = [op1Stock];

export function getProfile(id: string): DeviceProfile | undefined {
  return profiles.find((p) => p.id === id);
}

export interface RankedProfile {
  profile: DeviceProfile;
  result: DetectResult;
}

/** Runs every profile's detector and returns the matches, highest confidence first. */
export async function detectProfiles(root: DirectoryListing): Promise<RankedProfile[]> {
  const ranked = await Promise.all(
    profiles.map(async (profile) => ({ profile, result: await profile.detect(root) })),
  );
  return ranked
    .filter((r) => r.result.confidence > 0)
    .sort((a, b) => b.result.confidence - a.result.confidence);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `pnpm --filter @co-op/web test -- registry`
Expected: 4 passed.

- [ ] **Step 5: Commit**

```bash
git add web/src/core/profiles/registry.ts web/src/core/profiles/registry.test.ts
git commit -m "feat(profiles): registry with ranked detection"
```

---

### Task 15: Inspector page

A small screen so real OP-1 files can be dropped in and checked against the codec before the full librarian exists. This is the only UI in this plan.

**Files:**
- Create: `web/src/ui/Inspector.tsx`
- Modify: `web/src/App.tsx`
- Delete: `web/src/smoke.test.ts`

- [ ] **Step 1: Implement the inspector**

`web/src/ui/Inspector.tsx`:
```tsx
import { useState, type DragEvent, type ChangeEvent } from 'react';
import { readPatch, decodePcm, type Patch } from '../core/codec/patch';
import { op1Stock } from '../core/profiles/op1-stock';

interface Loaded {
  fileName: string;
  sizeBytes: number;
  patch: Patch;
}

export function Inspector() {
  const [loaded, setLoaded] = useState<Loaded | null>(null);
  const [error, setError] = useState<string | null>(null);

  async function load(file: File) {
    try {
      const bytes = new Uint8Array(await file.arrayBuffer());
      setLoaded({ fileName: file.name, sizeBytes: bytes.length, patch: readPatch(bytes) });
      setError(null);
    } catch (e) {
      setLoaded(null);
      setError((e as Error).message);
    }
  }

  function onDrop(event: DragEvent<HTMLDivElement>) {
    event.preventDefault();
    const file = event.dataTransfer.files[0];
    if (file) void load(file);
  }

  function onPick(event: ChangeEvent<HTMLInputElement>) {
    const file = event.target.files?.[0];
    if (file) void load(file);
  }

  function play() {
    if (!loaded) return;
    const pcm = decodePcm(loaded.patch);
    const context = new AudioContext({ sampleRate: loaded.patch.comm.sampleRate });
    const buffer = context.createBuffer(1, pcm.length, loaded.patch.comm.sampleRate);
    buffer.copyToChannel(pcm, 0);
    const source = context.createBufferSource();
    source.buffer = buffer;
    source.connect(context.destination);
    source.start();
  }

  const compat = loaded ? op1Stock.compatibility(loaded.patch) : null;

  return (
    <section>
      <h2>Patch inspector</h2>
      <div
        onDrop={onDrop}
        onDragOver={(e) => e.preventDefault()}
        style={{ border: '2px dashed #999', padding: 24, borderRadius: 8, textAlign: 'center' }}
      >
        <p>Drop an OP-1 .aif patch here, or</p>
        <input type="file" accept=".aif,.aiff" onChange={onPick} />
      </div>
      {error && <p role="alert" style={{ color: '#b00020' }}>Could not read file: {error}</p>}
      {loaded && compat && (
        <div>
          <h3>{loaded.fileName}</h3>
          <table>
            <tbody>
              <tr><th align="left">Kind</th><td>{loaded.patch.kind}</td></tr>
              <tr><th align="left">Name</th><td>{loaded.patch.name || '(none)'}</td></tr>
              <tr><th align="left">Form</th><td>{loaded.patch.form.formType}</td></tr>
              <tr><th align="left">Sample rate</th><td>{loaded.patch.comm.sampleRate} Hz</td></tr>
              <tr><th align="left">Bits</th><td>{loaded.patch.comm.bitsPerSample}</td></tr>
              <tr><th align="left">Channels</th><td>{loaded.patch.comm.channels}</td></tr>
              <tr><th align="left">Compression</th><td>{loaded.patch.comm.compression}</td></tr>
              <tr><th align="left">Frames</th><td>{loaded.patch.comm.frames}</td></tr>
              <tr><th align="left">Chunks</th><td>{loaded.patch.form.chunks.map((c) => c.id).join(', ')}</td></tr>
              <tr><th align="left">Size</th><td>{loaded.sizeBytes} bytes</td></tr>
              <tr><th align="left">Stock OP-1</th><td>{compat.level}{compat.reason ? `: ${compat.reason}` : ''}</td></tr>
            </tbody>
          </table>
          <button type="button" onClick={play}>Play</button>
          <h4>Metadata</h4>
          <pre>{loaded.patch.meta ? JSON.stringify(loaded.patch.meta, null, 2) : 'no op-1 metadata'}</pre>
        </div>
      )}
    </section>
  );
}
```

- [ ] **Step 2: Mount it in App.tsx**

`web/src/App.tsx`:
```tsx
import { Inspector } from './ui/Inspector';

export function App() {
  return (
    <main>
      <h1>co-op patch librarian</h1>
      <p>Early build. Drop an OP-1 patch below to check that we read it correctly.</p>
      <Inspector />
    </main>
  );
}
```

- [ ] **Step 3: Remove the smoke test**

Run: `git rm web/src/smoke.test.ts`

- [ ] **Step 4: Verify in the browser**

Run: `pnpm --filter @co-op/web dev`
Open the printed URL (it ends in `/co-op/`). Drop `web/test/fixtures/drum-boombap.aif` on the page.
Expected: kind `drum`, name `boombap1`, compression `NONE`, chunks `COMM, APPL, SSND`, Stock OP-1 `ok`. Clicking Play produces a half-second low tone. Dropping `web/test/fixtures/no-metadata.aif` shows `unsupported: file has no op-1 metadata`.

- [ ] **Step 5: Run the full checks**

Run: `pnpm typecheck && pnpm test && pnpm build`
Expected: all pass, `web/dist` rebuilt.

- [ ] **Step 6: Commit and push**

```bash
git add web/src/ui/Inspector.tsx web/src/App.tsx
git commit -m "feat(web): patch inspector page"
git push origin main
gh run watch --exit-status
```
Expected: CI and Pages succeed; `https://coldsoldr.github.io/co-op/` shows the inspector.

---

### Task 16: Check real device files

The fixtures are synthetic. This task confirms the codec against files written by an actual OP-1 and turns them into fixtures where the license allows.

**Files:**
- Possibly add: `web/test/fixtures/real/*.aif`
- Modify: `web/src/core/codec/patch.test.ts`

- [ ] **Step 1: Collect files**

Ask the owner for one synth patch and one drum patch copied straight from an OP-1 in disk mode, ideally from `synth/user` and `drum/user`, that they are happy to publish in the repo.

- [ ] **Step 2: Inspect them**

Open the deployed inspector and drop each file. Record kind, compression, chunk list and whether Play sounds right. If the chunk list contains ids other than `FVER`, `COMM`, `APPL`, `SSND`, note them: the codec keeps them verbatim, but the spec open item about layout should mention them.

- [ ] **Step 3: Add them as fixtures and extend the round-trip test**

Copy the files to `web/test/fixtures/real/` and add this test to `web/src/core/codec/patch.test.ts` inside the `writePatch` describe block:

```ts
  it('round-trips real device files byte for byte', () => {
    const dir = fileURLToPath(new URL('../../../test/fixtures/real/', import.meta.url));
    const files = readdirSync(dir).filter((f) => f.toLowerCase().endsWith('.aif'));
    expect(files.length).toBeGreaterThan(0);
    for (const name of files) {
      const bytes = new Uint8Array(readFileSync(dir + name));
      expect(Array.from(writePatch(readPatch(bytes)))).toEqual(Array.from(bytes));
    }
  });
```

Add `readdirSync` to the `node:fs` import at the top of the file.

- [ ] **Step 4: Run the test**

Run: `pnpm --filter @co-op/web test -- patch`
Expected: PASS. If a real file fails the byte-for-byte check, the most likely causes are a FORM size that does not match the file length or a non-zero pad byte; fix `readForm`/`writeForm` to preserve what the device wrote and add a unit test for that case in `iff.test.ts`.

- [ ] **Step 5: Commit**

```bash
git add web/test/fixtures/real web/src/core/codec/patch.test.ts
git commit -m "test: round-trip real OP-1 device files"
```

If the owner cannot supply files yet, skip steps 3 to 5, leave the test out, and record in the PR or commit message that real-file verification is pending.

---

## Self-review

**Spec coverage for phases 1 and 2:**
- Workspace, CI, Pages deploy with placeholder: Tasks 1 to 3.
- Codec preserving unknown chunks, APPL JSON, little-endian handling, byte-identical round trip: Tasks 4, 5, 7, 8, 10.
- Patch model with kind, name, meta, raw: Task 10.
- DeviceProfile with layout, patch kinds, detect, compatibility; stock profile; registry: Tasks 12 to 14.
- Fixtures and Vitest coverage: Tasks 6, 9, 16.
- Not in this plan (later phases): DeviceStore adapters, device library UI, packs, Worker, op1.fun, Playwright, hundekraft and opus profiles.

**Type consistency:** `Chunk`, `IffForm` (Task 4) used by Tasks 5, 7, 8, 9, 10. `CommInfo` (Task 5) used by 8, 9, 10. `Op1Meta`, `PatchKind`, `metaKind` (Task 6) used by 10, 12, 13. `Patch`, `readPatch`, `writePatch`, `withMeta`, `decodePcm` (Task 10) used by 12, 15, 16. `DirectoryListing`, `DirEntry` (Task 11) used by 12, 13, 14. `DeviceProfile` fields `layout.patchRoots`, `patchKinds`, `detect`, `compatibility` (Task 12) used by 13, 14, 15.
