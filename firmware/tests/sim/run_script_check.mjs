// The two run-script compilers must agree byte for byte: tools/coop_sysex.html (the page the owner runs) and
// tools/run_script.py (op1emu). Takes the page's compiler block, compiles every tests/sim/run_*.txt, and compares
// with the .crun files tools/run_script.py wrote into DIR:
//
//   (WSL)     for f in tests/sim/run_*.txt; do python3 -I tools/run_script.py $f -o build/crun/$(basename ${f%.txt}).crun; done
//   (Windows) node tests/sim/run_script_check.mjs build/crun
import { readFileSync, readdirSync } from 'node:fs';
import { join, basename } from 'node:path';

const root = new URL('../..', import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1');
const html = readFileSync(join(root, 'tools/coop_sysex.html'), 'utf8');
const a = html.indexOf('// ---- run_script compiler'), b = html.indexOf('// ---- end of the compiler ----');
if (a < 0 || b < 0) { console.error('compiler block not found in coop_sysex.html'); process.exit(2); }
const compileScript = new Function(html.slice(a, b) + '\nreturn compileScript;')();

const dir = process.argv[2];
if (!dir) { console.error('usage: node tests/sim/run_script_check.mjs DIR_OF_PYTHON_CRUNS'); process.exit(2); }
let bad = 0, n = 0;
for (const f of readdirSync(join(root, 'tests/sim')).filter(f => /^run_.*\.txt$/.test(f))) {
  const name = basename(f, '.txt');
  let expect;
  try { expect = readFileSync(join(dir, name + '.crun')); } catch { console.log('skip ' + f + ' (no ' + name + '.crun)'); continue; }
  n++;
  let got;
  try { got = compileScript(readFileSync(join(root, 'tests/sim', f), 'utf8'), name).bytes; }
  catch (e) { console.log('FAIL ' + f + ': ' + e.message); bad++; continue; }
  const same = got.length === expect.length && got.every((v, i) => v === expect[i]);
  console.log((same ? 'ok   ' : 'FAIL ') + f + ': ' + got.length + ' bytes' + (same ? '' : ' (python ' + expect.length + ')'));
  if (!same) bad++;
}
console.log(n + ' scripts, ' + bad + ' different');
process.exit(bad ? 1 : 0);
