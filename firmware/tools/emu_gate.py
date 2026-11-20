#!/usr/bin/env python3
"""The op1emu gate as one command with a machine-readable verdict (docs/test-loop.md, tier 0).

    python3 -I tools/emu_gate.py --image ~/op1emu-work/e9/e9.ldr --scenario det_chord
    python3 -I tools/emu_gate.py --elf build/core/coop_combined.elf --plugins build/plugins/test_fx.coop \
        --scenario det_fx_controls --scenario det_fx_reload --chain \
        --expect 'plugins.loaded==1' --expect 'plugins.failed==0' --expect 'fxctl.turns>=1'
    python3 -I tools/emu_gate.py --image new.ldr --ref-image old.ldr --scenario det_chord      # regression compare

Runs in WSL. For every scenario: a scratch NAND (the pristine base, or the stock package plus the plugin files), one
headless deterministic op1emu run with the audio dumped, then the checks: the emulator's exit, faults in its log, the
stats file read out of the NAND (tools/nand_file.py), the --expect lines against it, the audio's levels, and with
--ref-image the same scenario on the reference image compared sample for sample (then aligned on the onset, the
known run-to-run block shift: docs/emulator.md "Gate runs"). Screenshots the scenario takes are redirected into the
output directory and converted to PNG. The verdict is JSON on stdout (and --json FILE); exit 0 pass, 1 fail, 2 the
gate could not run (lock, missing input).

Rules it keeps (CLAUDE.md, the owner's disk rule):
  - one emulator at a time: it holds ~/op1emu-work/emu.lock (the flock the shell wrappers take) for the whole run
    and waits for any op1emu started outside the lock to end;
  - scratch NANDs (553 MB each, at most three at a time) are named `gate_<pid>_...` in ~/op1emu-work/gate-tmp
    (OP1EMU_SCRATCH) and deleted after every run, on errors and signals too; a stale one of a dead gate is swept at
    the start, and the gate refuses to start with under 4 GB free there. They are on WSL's own disk on purpose: with
    the NAND on /mnt/i (9p) the emulator reached 8.7 s of guest time where it reaches 43.7 s on ext4, and the
    scenario's wall-timed `quit` came before its chord (2026-11-20). Logs, stats, screenshots and kept WAVs go to
    /mnt/i/op1emu-tmp/gate (OP1EMU_TMP), off the C: disk;
  - the bases (*pristine*, *stock*, otp.bin) are only ever read;
  - nothing under build/ is written: --elf builds its image into the output directory.
Stdlib only, Python 3.8.
"""
import argparse
import array
import hashlib
import json
import math
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import wave
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import nand_file  # noqa: E402

GATE_FLAGS = ['--tape-stereo', '--set-word', 'g_coop_spread=32767', '--set-word', 'g_coop_force_stereo_out=1']
FAULT_RE = re.compile(r'fault|exception|assert|Emulation stopped', re.I)
DUMP_RE = re.compile(r'Audio dump: (\d+) frames \(([\d.]+) s\), (\d+) nonzero')
PROTECTED = ('pristine', 'stock', 'otp')
NAND_FILES = ('coop_stats.txt', 'coop_run.txt', 'coop_sysex_test.txt')


class GateError(Exception):
    """The gate could not run (exit 2): not a verdict on the image."""


# ---- pure helpers (tests/tools/test_emu_gate.py) ----

def parse_stats(text):
    """coop_stats.txt / coop_run.txt -> {tag: [entry, ...]}; an entry is {'_': [bare words], key: value, ...}.

    `key value` pairs become ints where they parse; `name=avg/max/n[/p0/p1]` items become {'avg','max','n',...};
    other `name=text` items stay text. A tag that repeats (plugfile, top, win, section) keeps every line in order.
    """
    out = {}
    for line in text.splitlines():
        w = line.split()
        if not w:
            continue
        tag, rest = w[0], w[1:]
        e = {'_': []}
        i = 0
        while i < len(rest):
            t = rest[i]
            if '=' in t:
                k, v = t.split('=', 1)
                parts = v.split('/')
                if len(parts) >= 3 and all(re.fullmatch(r'-?\d+', p) for p in parts):
                    d = {'avg': int(parts[0]), 'max': int(parts[1]), 'n': int(parts[2])}
                    for j, p in enumerate(parts[3:]):
                        d['p%d' % j] = int(p)
                    e[k] = d
                else:
                    e[k] = v
                i += 1
            elif i + 1 < len(rest) and re.fullmatch(r'[A-Za-z_][\w.]*', t) and re.fullmatch(r'-?\d+', rest[i + 1]):
                e[t] = int(rest[i + 1])
                i += 2
            else:
                e['_'].append(t)
                i += 1
        e['line'] = line
        out.setdefault(tag, []).append(e)
    return out


EXPECT_RE = re.compile(r'^\s*([\w.]+)\s*(==|!=|>=|<=|>|<)\s*(-?\d+)\s*$')
OPS = {'==': lambda a, b: a == b, '!=': lambda a, b: a != b, '>=': lambda a, b: a >= b,
       '<=': lambda a, b: a <= b, '>': lambda a, b: a > b, '<': lambda a, b: a < b}


def lookup(values, path):
    """`plugins.loaded`, `cost.fx1_plugin.max`, `audio.nonzero`, `faults`: the first line of a tag is used."""
    cur = values
    for part in path.split('.'):
        if isinstance(cur, list):
            cur = cur[0] if cur else None
        if not isinstance(cur, dict) or part not in cur:
            return None
        cur = cur[part]
    if isinstance(cur, list):
        cur = cur[0] if cur else None
    return cur


def check_expect(expr, values):
    """Returns (ok, detail). An unknown name fails: a gate must not pass on a typo."""
    m = EXPECT_RE.match(expr)
    if not m:
        raise GateError('bad --expect %r (NAME OP INTEGER, e.g. plugins.loaded==1)' % expr)
    got = lookup(values, m.group(1))
    if not isinstance(got, int):
        return False, '%s is not in the results' % m.group(1)
    return OPS[m.group(2)](got, int(m.group(3))), '%s = %d' % (m.group(1), got)


def rewrite_screenshots(text, out_dir, prefix):
    """Scenario text with every `screenshot PATH` sent to out_dir/prefix_<basename>; returns (text, [paths])."""
    paths, lines = [], []
    for line in text.splitlines():
        w = line.split()
        if len(w) == 2 and w[0] == 'screenshot':
            p = os.path.join(out_dir, '%s_%s' % (prefix, os.path.basename(w[1])))
            paths.append(p)
            line = 'screenshot ' + p
        lines.append(line)
    return '\n'.join(lines) + '\n', paths


def last_at_ms(text):
    """The guest time of a scenario's last `at MS` line, or None: the audio dump must reach past it."""
    ms = [int(w[1]) for w in (l.split() for l in text.splitlines()) if len(w) == 2 and w[0] == 'at' and w[1].isdigit()]
    return max(ms) if ms else None


def ppm_to_png(ppm_path, png_path):
    """A binary P6 PPM (op1emu's headless screenshot) as an 8-bit RGB PNG. Returns (width, height)."""
    with open(ppm_path, 'rb') as f:
        data = f.read()
    tok, pos = [], 0
    while len(tok) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            pos = data.index(b'\n', pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        tok.append(data[pos:end])
        pos = end
    if tok[0] != b'P6' or int(tok[3]) != 255:
        raise ValueError('%s: not an 8-bit P6 PPM' % ppm_path)
    w, h = int(tok[1]), int(tok[2])
    pix = data[pos + 1:pos + 1 + w * h * 3]
    raw = b''.join(b'\0' + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
    with open(png_path, 'wb') as f:
        f.write(png)
    return w, h


def scratch_path(tmp_dir, name):
    """The only place this tool creates or deletes NAND images: tmp_dir/gate_*; never a base."""
    base = os.path.basename(name)
    if not base.startswith('gate_') or any(p in base for p in PROTECTED):
        raise GateError('refusing scratch name %r' % name)
    return os.path.join(tmp_dir, base)


def remove_scratch(path):
    base = os.path.basename(path)
    if base.startswith('gate_') and not any(p in base for p in PROTECTED) and os.path.isfile(path):
        os.unlink(path)
        return True
    return False


def read_wav(path):
    w = wave.open(path, 'rb')
    ch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    raw = w.readframes(n)
    w.close()
    if sw != 2:
        raise GateError('%s: 16-bit WAV expected' % path)
    a = array.array('h')
    a.frombytes(raw[:len(raw) // (2 * ch) * 2 * ch])
    if sys.byteorder == 'big':
        a.byteswap()
    return a, ch, rate


def wav_summary(a, ch, rate, skip_s=5.0, onset_thresh=200):
    """Levels after the boot: per-channel rms and peak, the nonzero frame count, the first frame at the threshold."""
    skip = int(skip_s * rate) * ch
    frames = len(a) // ch
    out = {'frames': frames, 'seconds': round(frames / float(rate), 2), 'rate': rate, 'channels': ch}
    rms, peak = [], []
    for c in range(ch):
        x = a[skip + c::ch]
        rms.append(round(math.sqrt(sum(v * v for v in x) / len(x)), 1) if x else 0.0)
        peak.append(max(max(x), -min(x)) if x else 0)
    out['rms'], out['peak'] = rms, peak
    onset = None
    for i in range(skip, len(a)):
        if a[i] >= onset_thresh or a[i] <= -onset_thresh:
            onset = i // ch
            break
    out['onset_frame'] = onset
    if ch == 2 and len(a) > skip:
        L, R = a[skip::2], a[skip + 1::2]
        side = math.sqrt(sum((l - r) * (l - r) for l, r in zip(L, R)) / len(L))
        mid = math.sqrt(sum((l + r) * (l + r) for l, r in zip(L, R)) / len(L))
        out['width'] = round(side / mid, 4) if mid else 0.0
    return out


def max_diff(a, b, start_a, start_b):
    n = min(len(a) - start_a, len(b) - start_b)
    if n <= 0:
        return 0, 0
    if a[start_a:start_a + n] == b[start_b:start_b + n]:
        return 0, n
    worst = 0
    for i in range(n):
        d = a[start_a + i] - b[start_b + i]
        if d < 0:
            d = -d
        if d > worst:
            worst = d
    return worst, n


def wav_compare(a, b, ch, rate, sa, sb, skip_s=5.0, tol=16):
    """identical: sample-exact from skip_s on. aligned: within tol once both start at their onsets. differs: neither."""
    skip = int(skip_s * rate) * ch
    worst, n = max_diff(a, b, skip, skip)
    res = {'frames': n // ch, 'max_diff': worst, 'tolerance': tol}
    if worst == 0:
        res['verdict'] = 'identical'
        return res
    oa, ob = sa.get('onset_frame'), sb.get('onset_frame')
    if oa is not None and ob is not None:
        w2, n2 = max_diff(a, b, oa * ch, ob * ch)
        res.update({'onset_a': oa, 'onset_b': ob, 'aligned_max_diff': w2, 'aligned_frames': n2 // ch})
        res['verdict'] = 'aligned' if w2 <= tol else 'differs'
    else:
        res['verdict'] = 'differs'
    lv = [abs(x - y) / max(x, y, 1.0) for x, y in zip(sa['rms'], sb['rms'])]
    res['rms_rel_diff'] = round(max(lv), 4) if lv else 0.0
    return res


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for blk in iter(lambda: f.read(1 << 20), b''):
            h.update(blk)
    return h.hexdigest()


# ---- the machine side ----

def emulators_running():
    pids = []
    for d in os.listdir('/proc'):
        if d.isdigit():
            try:
                if open('/proc/%s/comm' % d).read().strip() == 'op1emu':
                    pids.append(int(d))
            except OSError:
                pass
    return pids


class Lock:
    """flock on the work tree's emu.lock, the lock tests/sim/run_emu.sh and tools/op1emu_gui.sh take with flock(1)."""

    def __init__(self, path, timeout, log):
        self.path, self.timeout, self.log, self.fd, self.waited = path, timeout, log, None, 0.0

    def __enter__(self):
        import fcntl                                        # here: the pure helpers above also load on Windows
        self.fcntl = fcntl
        t0 = time.time()
        self.fd = os.open(self.path, os.O_RDWR | os.O_CREAT, 0o644)
        said = False
        while True:
            try:
                fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                foreign = emulators_running()               # started without the lock (hear.sh by hand)
                if not foreign:
                    break
                fcntl.flock(self.fd, fcntl.LOCK_UN)
                why = 'op1emu pid %s runs outside the lock' % foreign
            except OSError:
                why = 'the lock is held'
            if time.time() - t0 > self.timeout:
                os.close(self.fd)
                raise GateError('no emulator slot after %d s: %s' % (self.timeout, why))
            if not said:
                self.log('waiting: %s' % why)
                said = True
            time.sleep(5)
        self.waited = round(time.time() - t0, 1)
        return self

    def __exit__(self, *exc):
        self.fcntl.flock(self.fd, self.fcntl.LOCK_UN)
        os.close(self.fd)


def sweep_stale(tmp_dir, log):
    """Scratch NANDs of gates that died: gate_<pid>_... whose pid is gone."""
    for f in sorted(os.listdir(tmp_dir)):
        m = re.match(r'gate_(\d+)_.*\.bin$', f)
        if m and not os.path.exists('/proc/%s' % m.group(1)):
            if remove_scratch(os.path.join(tmp_dir, f)):
                log('removed the stale scratch NAND %s' % f)


def find_stock_op1():
    cands = [os.environ.get('STOCK_OP1'), os.path.join(ROOT, 'incoming', 'op1_246.op1')]
    users = '/mnt/c/Users'
    if os.path.isdir(users):
        cands += [os.path.join(users, u, 'Downloads', 'op1_246.op1') for u in sorted(os.listdir(users))]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    raise GateError('stock op1_246.op1 not found (set STOCK_OP1): needed for a NAND with plugin files')


def resolve_scenario(name):
    for c in (name, os.path.join(ROOT, 'tests', 'sim', name), os.path.join(ROOT, 'tests', 'sim', name + '.txt')):
        if os.path.isfile(c):
            return os.path.abspath(c)
    raise GateError('scenario not found: %s' % name)


def run(cmd, log_path=None, timeout=None, cwd=None, env=None):
    """Returns (exit code or None on timeout, seconds)."""
    t0 = time.time()
    out = open(log_path, 'wb') if log_path else subprocess.DEVNULL
    try:
        p = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, cwd=cwd, env=env, start_new_session=True)
        try:
            return p.wait(timeout=timeout), time.time() - t0
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            return None, time.time() - t0
        except BaseException:
            os.killpg(p.pid, signal.SIGKILL)                # a signal to the gate: no orphan emulator
            p.wait()
            raise
    finally:
        if log_path:
            out.close()


class Gate:
    def __init__(self, a):
        self.a = a
        self.work = os.path.expanduser(os.environ.get('OP1EMU_WORK', '~/op1emu-work'))
        self.tmp = os.path.expanduser(os.environ.get('OP1EMU_SCRATCH', os.path.join(self.work, 'gate-tmp')))
        self.outroot = os.environ.get('OP1EMU_TMP', '/mnt/i/op1emu-tmp')
        if not os.path.isdir(os.path.dirname(self.outroot.rstrip('/')) or '/'):
            self.outroot = os.path.join(self.work, 'gate-out')   # no I: drive here
        self.emu = os.path.join(self.work, 'op1emu', 'build', 'op1emu')
        self.pristine = os.path.join(self.work, 'pristine.bin')
        stamp = time.strftime('%Y%m%d-%H%M%S')
        self.out = os.path.abspath(a.out or os.path.join(self.outroot, 'gate', '%s-%s' % (a.label, stamp)))
        self.scratch = set()
        self.seq = 0

    def log(self, msg):
        if not self.a.quiet:
            sys.stderr.write('[gate %s] %s\n' % (time.strftime('%H:%M:%S'), msg))
            sys.stderr.flush()

    def new_scratch(self, what):
        self.seq += 1
        p = scratch_path(self.tmp, 'gate_%d_%d_%s.bin' % (os.getpid(), self.seq, what))
        self.scratch.add(p)
        return p

    def drop(self, path):
        if path in self.scratch:
            remove_scratch(path)
            self.scratch.discard(path)

    def cleanup(self):
        for p in list(self.scratch):
            self.drop(p)

    def build_image(self, elf, name):
        out = os.path.join(self.out, name + '.ldr')
        flags = ([] if self.a.no_default_flags else list(GATE_FLAGS))
        for sw in self.a.set_word:
            flags += ['--set-word', sw]
        env = dict(os.environ, ELF=os.path.abspath(elf))
        self.log('building %s from %s' % (out, elf))
        code, _ = run(['bash', os.path.join(HERE, 'build_image.sh'), out] + flags,
                      log_path=os.path.join(self.out, name + '_build.log'), timeout=600, env=env)
        if code != 0:
            raise GateError('build_image.sh failed (%s): see %s_build.log' % (code, name))
        return out, flags

    def base_nand(self):
        """The NAND every scenario starts from: pristine.bin, or the stock package plus the overlay (built once)."""
        a = self.a
        if not a.plugins and not a.overlay:
            return self.pristine, False
        ov = tempfile.mkdtemp(prefix='gate_ov_')
        try:
            if a.overlay:
                for root, _, files in os.walk(a.overlay):
                    for f in files:
                        src = os.path.join(root, f)
                        dst = os.path.join(ov, os.path.relpath(src, a.overlay))
                        os.makedirs(os.path.dirname(dst), exist_ok=True)
                        shutil.copyfile(src, dst)
            if a.plugins:
                os.makedirs(os.path.join(ov, 'plugins'), exist_ok=True)
                for f in a.plugins:
                    shutil.copyfile(f, os.path.join(ov, 'plugins', os.path.basename(f)))
            base = self.new_scratch('base')
            self.log('making the NAND with %d plugin file(s)' % len(a.plugins))
            code, _ = run([sys.executable, '-I', os.path.join(HERE, 'make_nand.py'),
                           '--provision', os.path.join(self.work, 'provision_nand.py'), '--firmware', find_stock_op1(),
                           '--mkyaffs2', os.path.join(self.work, 'yaffs2utils', 'mkyaffs2'),
                           '--overlay', ov, '--output', base],
                          log_path=os.path.join(self.out, 'make_nand.log'), timeout=900)
            if code != 0:
                raise GateError('make_nand.py failed (%s): see make_nand.log' % code)
            return base, True
        finally:
            shutil.rmtree(ov, ignore_errors=True)

    def emulate(self, image, scen_path, nand, tag):
        """One op1emu run. Returns the raw facts; the NAND is modified in place (the stats file lands in it)."""
        text, shots = rewrite_screenshots(open(scen_path, encoding='utf-8').read(), self.out, tag)
        script = os.path.join(self.out, tag + '.script.txt')
        open(script, 'w', encoding='utf-8').write(text)
        wav = os.path.join(self.out, tag + '.wav')
        log = os.path.join(self.out, tag + '.log')
        cmd = [self.emu, nand, image, '--headless', '--deterministic', '--audio-dump', wav, '--input-script', script]
        cmd += self.a.emu_arg
        self.log('run %s' % tag)
        code, secs = run(cmd, log_path=log, timeout=self.a.timeout, cwd=os.path.join(self.work, 'op1emu'))
        logtext = open(log, errors='replace').read()
        r = {'tag': tag, 'exit': code, 'timed_out': code is None, 'seconds': round(secs, 1), 'log': log, 'wav': wav,
             'faults': len(FAULT_RE.findall(logtext)), 'fault_lines': [l for l in logtext.splitlines() if FAULT_RE.search(l)][:5]}
        m = DUMP_RE.findall(logtext)
        if m:
            r['dump'] = {'frames': int(m[-1][0]), 'seconds': float(m[-1][1]), 'nonzero': int(m[-1][2])}
        pngs = []
        for p in shots:
            if os.path.isfile(p):
                png = os.path.splitext(p)[0] + '.png'
                try:
                    ppm_to_png(p, png)
                    os.unlink(p)
                    pngs.append(png)
                except (ValueError, IndexError):
                    pngs.append(p)
        r['screenshots'] = pngs
        r['screenshots_missing'] = len(shots) - len(pngs)
        r['last_at_ms'] = last_at_ms(text)
        return r

    def scenario(self, image, ref_image, scen_path, base, base_is_scratch, chained_nand):
        a = self.a
        name = os.path.splitext(os.path.basename(scen_path))[0]
        res = {'name': name, 'scenario': scen_path, 'checks': []}
        t0 = time.time()

        def check(what, ok, detail=''):
            res['checks'].append({'name': what, 'ok': bool(ok), 'detail': detail})

        nand = chained_nand
        if nand is None:
            nand = self.new_scratch(name)
            shutil.copyfile(base, nand)
        try:
            r = self.emulate(image, scen_path, nand, name)
            res.update({k: r[k] for k in ('seconds', 'log', 'screenshots')})
            check('emulator_exit', r['exit'] == 0, 'timed out after %d s' % a.timeout if r['timed_out'] else 'exit %s' % r['exit'])
            check('no_fault', r['faults'] == 0, '; '.join(r['fault_lines']))
            if r['last_at_ms'] is not None and 'dump' in r:
                # A slow run (a busy host, a NAND on a slow disk) ends on the script's wall-timed `wait` / `quit`
                # before its guest-timed events were played: that is not a result.
                reached = r['dump']['seconds'] * 1000.0
                check('ran_to_end', reached >= r['last_at_ms'] + 500,
                      'guest time %.1f s, the last event is at %.1f s' % (reached / 1000.0, r['last_at_ms'] / 1000.0))
            if r['screenshots_missing']:
                check('screenshots', False, '%d screenshot(s) not written' % r['screenshots_missing'])
            values = {'faults': r['faults']}
            files = {}
            for fn in NAND_FILES:
                data = nand_file.read_file(nand, fn)
                if data is not None:
                    path = os.path.join(self.out, '%s_%s' % (name, fn))
                    open(path, 'wb').write(data)
                    files[fn] = path
            res['files'] = files
            if 'coop_stats.txt' in files:
                stats = parse_stats(open(files['coop_stats.txt'], errors='replace').read())
                values.update(stats)
                head = stats.get('coop_stats', [{}])[0]
                res['stats'] = {'build': (head.get('_') or [None, None, None])[-1] if 'build' in head.get('_', []) else None}
                for tag in a.report:
                    if tag in stats:
                        res['stats'][tag] = [{k: v for k, v in e.items() if k != '_' or v} for e in stats[tag]]
            if 'coop_run.txt' in files:
                values['run'] = parse_stats(open(files['coop_run.txt'], errors='replace').read())
            if not a.stock:
                check('stats_written', 'coop_stats.txt' in files, '' if 'coop_stats.txt' in files else 'no coop_stats.txt on the NAND')
            wavdata = None
            if os.path.isfile(r['wav']) and os.path.getsize(r['wav']) > 44:
                wavdata = read_wav(r['wav'])
                res['audio'] = wav_summary(*wavdata, skip_s=a.skip_seconds)
                if 'dump' in r:
                    res['audio']['nonzero'] = r['dump']['nonzero']
                values['audio'] = res['audio']
            else:
                check('audio_dump', False, 'no audio was dumped')
            for ex in a.expect:
                ok, detail = check_expect(ex, values)
                check('expect ' + ex, ok, detail)
            for rx in a.expect_line:
                text = open(files['coop_stats.txt'], errors='replace').read() if 'coop_stats.txt' in files else ''
                hit = [l for l in text.splitlines() if re.search(rx, l)]
                check('expect-line ' + rx, bool(hit), hit[0][:200] if hit else 'no stats line matches')
            if ref_image and wavdata is not None:
                res['compare'] = self.compare(ref_image, scen_path, base, name, wavdata, res['audio'], image, check)
        finally:
            if chained_nand is None:
                self.drop(nand)
            if not a.keep_wav:
                for f in os.listdir(self.out):
                    if f.endswith('.wav'):
                        os.unlink(os.path.join(self.out, f))
        res['verdict'] = 'pass' if all(c['ok'] for c in res['checks']) else 'fail'
        res['seconds'] = round(time.time() - t0, 1)
        return res

    def compare(self, ref_image, scen_path, base, name, wavdata, summary, image, check):
        """The reference image on the same scenario and a fresh copy of the same base, then the sample compare. A run
        that neither matches nor aligns is tried again (--retries): one image differs from itself that way from run
        to run (docs/device-tests.md, e6 gate notes)."""
        a = self.a
        best = None
        cand = (wavdata, summary)
        for attempt in range(a.retries + 1):
            nand = self.new_scratch(name + '_ref')
            shutil.copyfile(base, nand)
            try:
                r = self.emulate(ref_image, scen_path, nand, '%s_ref%d' % (name, attempt))
            finally:
                self.drop(nand)
            if r['exit'] != 0 or not os.path.isfile(r['wav']):
                check('reference_run', False, 'the reference image did not run (exit %s)' % r['exit'])
                return {'verdict': 'no reference'}
            ref = read_wav(r['wav'])
            rs = wav_summary(*ref, skip_s=a.skip_seconds)
            c = wav_compare(cand[0][0], ref[0], ref[1], ref[2], cand[1], rs, skip_s=a.skip_seconds, tol=a.tolerance)
            c['attempt'] = attempt + 1
            c['reference_audio'] = rs
            order = {'identical': 0, 'aligned': 1, 'differs': 2}
            if best is None or order[c['verdict']] < order[best['verdict']]:
                best = c
            if c['verdict'] != 'differs':
                break
            if attempt < a.retries:                          # a second run of the candidate, then compare again
                nand = self.new_scratch(name + '_again')
                shutil.copyfile(base, nand)
                try:
                    r2 = self.emulate(image, scen_path, nand, '%s_again%d' % (name, attempt))
                finally:
                    self.drop(nand)
                if r2['exit'] == 0 and os.path.isfile(r2['wav']):
                    w2 = read_wav(r2['wav'])
                    cand = (w2, wav_summary(*w2, skip_s=a.skip_seconds))
        ok = best['verdict'] == 'identical' or (best['verdict'] == 'aligned' and not a.exact)
        check('matches_reference', ok, '%s (max diff %s%s)' % (
            best['verdict'], best['max_diff'],
            ', %s aligned' % best['aligned_max_diff'] if 'aligned_max_diff' in best else ''))
        return best

    def main(self):
        a = self.a
        t0 = time.time()
        for need in (self.emu, self.pristine):
            if not os.path.exists(need):
                raise GateError('%s not found (tools/setup_op1emu.sh)' % need)
        scens = [resolve_scenario(s) for s in a.scenario]
        for f in a.plugins:
            if not os.path.isfile(f):
                raise GateError('plugin file not found: %s' % f)
        os.makedirs(self.tmp, exist_ok=True)
        os.makedirs(self.out, exist_ok=True)
        free = shutil.disk_usage(self.tmp).free
        if free < 4 << 30:
            raise GateError('only %.1f GB free in %s: a gate needs room for three 553 MB NANDs' % (free / 2.0 ** 30, self.tmp))
        report = {'gate': 1, 'label': a.label, 'started': time.strftime('%Y-%m-%dT%H:%M:%S'), 'out': self.out,
                  'plugins': [{'file': os.path.basename(f), 'bytes': os.path.getsize(f), 'sha256': sha256(f)} for f in a.plugins],
                  'scenarios': []}
        with Lock(os.path.join(self.work, 'emu.lock'), a.lock_timeout, self.log) as lock:
            report['lock_wait_seconds'] = lock.waited
            sweep_stale(self.tmp, self.log)
            try:
                image, flags = (a.image, None) if a.image else self.build_image(a.elf, 'image')
                image = os.path.abspath(os.path.expanduser(image))
                if not os.path.isfile(image):
                    raise GateError('image not found: %s' % image)
                report['image'] = {'path': image, 'sha256': sha256(image)}
                if flags is not None:
                    report['image'].update({'elf': os.path.abspath(a.elf), 'elf_sha256': sha256(a.elf), 'flags': flags})
                ref = None
                if a.ref_image or a.ref_elf:
                    ref = a.ref_image or self.build_image(a.ref_elf, 'ref')[0]
                    ref = os.path.abspath(os.path.expanduser(ref))
                    if not os.path.isfile(ref):
                        raise GateError('reference image not found: %s' % ref)
                    report['ref_image'] = {'path': ref, 'sha256': sha256(ref)}
                base, base_is_scratch = self.base_nand()
                chained = None
                if a.chain:
                    chained = self.new_scratch('chain')
                    shutil.copyfile(base, chained)
                for sp in scens:
                    report['scenarios'].append(self.scenario(image, ref, sp, base, base_is_scratch, chained))
            finally:
                self.cleanup()
                left = [f for f in os.listdir(self.tmp) if f.startswith('gate_%d_' % os.getpid())]
                report['cleanup'] = {'scratch_nands_left': left}
        report['seconds'] = round(time.time() - t0, 1)
        report['verdict'] = 'pass' if report['scenarios'] and all(s['verdict'] == 'pass' for s in report['scenarios']) else 'fail'
        report['failed'] = ['%s: %s' % (s['name'], c['name']) for s in report['scenarios'] for c in s['checks'] if not c['ok']]
        return report


def parser():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--image', help='a plaintext op1emu image (tools/build_image.sh OUT.ldr)')
    g.add_argument('--elf', help='build the image from this combined ELF into the output directory (the gate flags: '
                                 '--tape-stereo, spread 32767, force_stereo_out 1)')
    ap.add_argument('--set-word', action='append', default=[], metavar='NAME=VALUE', help='with --elf: more image words')
    ap.add_argument('--no-default-flags', action='store_true', help='with --elf: only the --set-word words')
    ap.add_argument('--ref-image', help='compare every scenario with this image')
    ap.add_argument('--ref-elf', help='... or with an image built from this ELF, same flags')
    ap.add_argument('--scenario', action='append', required=True, help='a tests/sim name (det_chord) or a path; repeatable')
    ap.add_argument('--plugins', nargs='*', default=[], metavar='FILE.coop', help='plugin files for /yaffs2/plugins')
    ap.add_argument('--overlay', help='a directory whose tree lands at the yaffs root (tools/make_nand.py)')
    ap.add_argument('--chain', action='store_true', help='run the scenarios in order on one NAND (power cycles)')
    ap.add_argument('--expect', action='append', default=[], metavar='NAME OP N',
                    help="e.g. 'plugins.loaded==1', 'cost.fx1_plugin.max<60000', 'audio.nonzero>0'; repeatable")
    ap.add_argument('--expect-line', action='append', default=[], metavar='REGEX', help='a stats line must match')
    ap.add_argument('--report', nargs='*', default=['plugins', 'plugmem', 'plugfile', 'cost', 'engines', 'fxctl'],
                    help='stats tags copied into the JSON (the whole file is saved beside it)')
    ap.add_argument('--stock', action='store_true', help='the image is not coop: no stats file is expected')
    ap.add_argument('--exact', action='store_true', help='with a reference: only a sample-identical run passes')
    ap.add_argument('--tolerance', type=int, default=16, help='largest |diff| of an onset-aligned compare (default 16)')
    ap.add_argument('--retries', type=int, default=1, help='more tries when a compare differs (default 1)')
    ap.add_argument('--skip-seconds', type=float, default=5.0, help='the boot, left out of levels and compares')
    ap.add_argument('--timeout', type=int, default=900, help='seconds per emulator run')
    ap.add_argument('--lock-timeout', type=int, default=7200, help='seconds to wait for the emulator slot')
    ap.add_argument('--emu-arg', action='append', default=[], help='one more op1emu argument; repeatable')
    ap.add_argument('--keep-wav', action='store_true', help='leave the audio dumps in the output directory')
    ap.add_argument('--out', help='the output directory (default $OP1EMU_TMP/gate/LABEL-TIME, /mnt/i/op1emu-tmp)')
    ap.add_argument('--label', default='gate')
    ap.add_argument('--json', help='also write the verdict here')
    ap.add_argument('--quiet', action='store_true', help='no progress lines on stderr')
    return ap


def main(argv=None):
    a = parser().parse_args(argv)
    gate = Gate(a)

    def stop(signum, _frame):
        raise KeyboardInterrupt('signal %d' % signum)
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGHUP, stop)
    try:
        report = gate.main()
        code = 0 if report['verdict'] == 'pass' else 1
    except GateError as e:
        report, code = {'gate': 1, 'verdict': 'error', 'error': str(e)}, 2
    except KeyboardInterrupt as e:
        report, code = {'gate': 1, 'verdict': 'error', 'error': 'interrupted (%s)' % e}, 2
    finally:
        gate.cleanup()
    text = json.dumps(report, indent=1)
    if a.json:
        open(a.json, 'w').write(text + '\n')
    print(text)
    return code


if __name__ == '__main__':
    sys.exit(main())
