import { expect, test, type Page } from '@playwright/test';
import { setPools, type CoopMemory } from '../src/core/plugins/memory';
import { coopRow, stubCoop } from './coop-stub';

interface IndexBuild {
  firmware?: string;
  file?: string;
  sizeBytes?: number;
  version?: string;
  memory?: CoopMemory;
}

async function readPlugins(page: Page): Promise<void> {
  await page.goto('./');
  await page.getByRole('tab', { name: 'Settings' }).click();
  await page.getByRole('radio', { name: /OP-1 \(coop\)/ }).check();
  await page.getByRole('tab', { name: 'Plugins' }).click();
  await page.getByRole('button', { name: 'Read plugins from the OP-1' }).click();
  await expect(page.getByRole('table', { name: 'Plugin files on the OP-1' })).toBeVisible();
}

const shot = async (page: Page, name: string): Promise<void> => {
  if (process.env.E2E_SHOTS) await page.locator('.coop-device').screenshot({ path: `${process.env.E2E_SHOTS}/${name}.png` });
};
const kb = (bytes: number): string => `${Math.round(bytes / 1024).toLocaleString('en-US')} KB`;

/** Every coop plugin of the catalogue as an e13 OP-1 with the default policy lists it: need, scratch and heap. */
async function catalogueOnE13(page: Page): Promise<{ rows: string[]; memories: CoopMemory[] }> {
  const index = (await (await page.request.get('plugins/index.json')).json()) as { plugins: (IndexBuild & { builds?: IndexBuild[] })[] };
  const memories: CoopMemory[] = [];
  const rows: string[] = [];
  for (const plugin of index.plugins) {
    const build = (plugin.builds ?? [plugin]).find((b) => b.firmware === 'op1/coop' && b.file && b.memory);
    if (!build?.file || !build.memory) continue;
    // The file by itself: its two parts, without the one buffer the arena's sum adds for a scratch effect.
    const alone = setPools([build.memory], 'placed');
    const heap = alone.heap;
    const need = alone.heap + alone.arena - build.memory.scratch;
    memories.push(build.memory);
    rows.push(coopRow({ file: build.file, kind: build.memory.kind, version: build.version ?? '', size: build.sizeBytes ?? 0, need, scratch: build.memory.scratch, heap }));
  }
  return { rows, memories };
}

test.describe('Plugins: the coop plugin memory on e13', () => {
  test('shows the memory with the least room as the bar and the other in a line, from the OP-1’s own figures', async ({ page }) => {
    const { rows, memories } = await catalogueOnE13(page);
    const used = setPools(memories, 'placed');
    const buffer = Math.max(...memories.map((m) => m.scratch));
    await stubCoop(page, {
      build: 'e13',
      rows,
      memory: { budget: 1_769_472, used: used.heap, fxbuf: buffer, fxbufs: 4, e13: { heapBudget: 1_769_472, heapUsed: used.heap, arenaBudget: 5_898_240, arenaUsed: used.arena } },
    });
    await readPlugins(page);

    const memory = page.locator('.plugin-memory');
    await expect(memory).toHaveAttribute('data-level', 'fits');
    await expect(page.getByRole('meter', { name: 'Plugin memory' })).toContainText(`${kb(used.heap).replace(' KB', '')} of 1,728 KB`);
    await expect(memory).toContainText(
      `The bar is the memory for code and voices. For buffers, screens and controls the OP-1 has another: ${kb(used.arena).replace(' KB', '')} of 5,760 KB.`,
    );
    await expect(memory).toContainText(`Loaded now: ${kb(used.heap)}, by the OP-1’s own count.`);
    await expect(memory).not.toContainText('about');
    await expect(memory).not.toContainText('estimate');
    await expect(memory).not.toContainText('placement test file');
    await expect(memory.getByRole('button', { name: 'Ask the OP-1 for its own figure' })).toHaveCount(0);
    await shot(page, 'plugin-memory-e13');
  });

  test('says which memory is short, names the plugin left out and points out a placement test file', async ({ page }) => {
    // A core built with small budgets, under a policy file that moves effect code to the arena.
    const rows = [
      coopRow({ file: 'a_synth.coop', kind: 'synth', size: 20_000, need: 402_000, scratch: 0, heap: 400_000 }),
      coopRow({ file: 'b_synth.coop', kind: 'synth', boot: 'failed', size: 20_000, need: 402_000, scratch: 0, heap: 400_000, reason: 'over the plugin memory budget (needs 390 KB, 292 KB of 683 KB left)' }),
      coopRow({ file: 'c_echo.coop', size: 20_000, need: 30_000, scratch: 100_000, heap: 0 }),
    ];
    await stubCoop(page, {
      build: 'e13',
      rows,
      memory: {
        budget: 700_000,
        used: 400_000,
        fxbuf: 100_000,
        fxbufs: 4,
        e13: { heapBudget: 700_000, heapUsed: 400_000, arenaBudget: 5_898_240, arenaUsed: 134_000, place: 'eng=heap,fx=arena,voice=heap,buf=arena', placeSrc: 'file' },
      },
    });
    await readPlugins(page);

    const memory = page.locator('.plugin-memory');
    await expect(memory).toHaveAttribute('data-level', 'over');
    await expect(page.getByRole('meter', { name: 'Plugin memory' })).toContainText('781 of 684 KB');
    await expect(memory).toContainText('What is short is the memory for engine code and voices.');
    await expect(memory).toContainText('The OP-1 loads them in name order, so it will leave out b synth.');
    await expect(memory).toContainText('For effect code, buffers, screens and controls the OP-1 has another: 131 of 5,760 KB.');
    await expect(memory).toContainText('A placement test file is in force on this OP-1. (eng=heap, fx=arena, voice=heap, buf=arena)');
    await shot(page, 'plugin-memory-e13-over');
  });

  test('points out a placement test file the OP-1 ignored', async ({ page }) => {
    const rows = [coopRow({ file: 'a_synth.coop', kind: 'synth', size: 20_000, need: 402_000, scratch: 0, heap: 400_000 })];
    await stubCoop(page, {
      build: 'e13',
      rows,
      memory: { budget: 1_769_472, used: 400_000, e13: { heapBudget: 1_769_472, heapUsed: 400_000, arenaBudget: 5_898_240, arenaUsed: 2_000, placeSrc: 'marker' } },
    });
    await readPlugins(page);
    await expect(page.locator('.plugin-memory')).toContainText('This OP-1 ignored its placement test file: the last boot with it did not finish.');
  });
});
