#!/usr/bin/env python3
"""Compare a device recording of a test run with op1emu's dump of the same run, section by section.

    python3 -I tools/run_compare.py tests/sim/run_p5a.txt DEVICE.wav EMU.wav

DEVICE.wav is what tools/coop_sysex.html recorded from the audio interface; EMU.wav is the runner's op1emu dump
(tests/sim/run_emu.sh, the "_b" file). Both runs start with the runner's click (block 0: one block of a +-12000
doublet on the master bus) and end with another at the END event, so:
  - each file's start click is found (the first sample far above the file's quiet floor: start the recording before
    the run, with the OP-1 quiet),
  - the end click is looked for where the script puts it: its offset gives the clock ratio between the OP-1 and the
    interface (the two clocks drift; the comparison uses the measured ratio),
  - every section of the script (#@mark) is cut from both and compared: RMS level relative to the file's loudest
    section (the interface gain and the OP-1 volume differ, so absolute levels mean nothing), the zero-crossing rate
    (a rough brightness figure), and the peak. A section that is silent in one file and not the other, or whose
    level differs by more than --tol dB, is flagged.
Stdlib only; reads 16-bit WAVs (mono or stereo, any rate: the device file is resampled by index to the script's
44.1 kHz grid).
"""
import argparse, math, os, struct, sys, wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_script import compile_script, EV_MARK, EV_CLICK  # noqa: E402

BLOCK = 64
RATE = 44100


def load(path):
    w = wave.open(path, 'rb')
    if w.getsampwidth() != 2:
        sys.exit('%s: 16-bit WAV expected' % path)
    n, ch, rate = w.getnframes(), w.getnchannels(), w.getframerate()
    a = struct.unpack('<%dh' % (n * ch), w.readframes(n))
    mono = [(a[i] + a[i + 1]) / 2.0 for i in range(0, len(a) - 1, 2)] if ch == 2 else list(a)
    return mono, rate


def threshold(x, rate):
    """Far above the quiet floor (the first 0.3 s) and at least a fifth of the file's peak: the runner's click is about
    half the loudest music on op1emu (12000 before the master level stage)."""
    head = sorted(abs(v) for v in x[:int(0.3 * rate)]) or [0.0]
    floor = head[len(head) // 2] + 1.0
    return max(20 * floor, 0.2 * max(abs(v) for v in x))


def first_above(x, th, lo, hi):
    for i in range(max(0, lo), min(len(x), hi)):
        if abs(x[i]) >= th:
            return i
    return None


def stats(x, a, b, rate):
    seg = x[max(0, a):max(0, b)]
    if not seg:
        return 0.0, 0.0, 0.0
    rms = math.sqrt(sum(v * v for v in seg) / len(seg))
    zc = sum(1 for i in range(1, len(seg)) if (seg[i - 1] < 0) != (seg[i] < 0)) / (len(seg) / rate) / 2
    return rms, zc, max(abs(v) for v in seg)


def db(v, ref):
    return 20 * math.log10(v / ref) if v > 0 and ref > 0 else -999.0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('script')
    ap.add_argument('device')
    ap.add_argument('emu')
    ap.add_argument('--tol', type=float, default=3.0, help='flag a section whose relative level differs by more (dB)')
    a = ap.parse_args()
    name, marks, events, _ = compile_script(open(a.script, encoding='utf-8').read())
    end_block = [e for e in events if e[1] == EV_CLICK][-1][0]
    first_event = events[1][0]
    secs = []                                            # (name, start block, end block)
    mk = [(e[0], marks[e[2]]) for e in events if e[1] == EV_MARK]
    for i, (blk, nm) in enumerate(mk):
        secs.append((nm, blk, mk[i + 1][0] if i + 1 < len(mk) else end_block))
    files = {}
    for tag, path in (('device', a.device), ('emu', a.emu)):
        x, rate = load(path)
        th = threshold(x, rate)
        c0 = first_above(x, th, 0, len(x))               # the start click: the first loud sample of the file
        if c0 is None:
            sys.exit('%s: no start click found' % path)
        expect = c0 + end_block * BLOCK * rate / RATE
        c1 = first_above(x, th, int(expect - 0.25 * rate), int(expect + 0.25 * rate))
        ratio = (c1 - c0) / (end_block * BLOCK) if c1 else rate / RATE
        files[tag] = (x, c0, ratio, rate)
        print('%-6s %s: %d Hz, start click %.4f s, end click %.4f s (expected %.4f): %.6f samples per OP-1 sample (%+.0f ppm)'
              % (tag, os.path.basename(path), rate, c0 / rate, (c1 or 0) / rate, expect / rate, ratio,
                 (ratio * RATE / rate - 1) * 1e6))
    rows = []
    for tag in ('device', 'emu'):
        x, c0, ratio, rate = files[tag]
        r = []
        for nm, b0, b1 in secs:
            s0 = int(c0 + b0 * BLOCK * ratio)
            s1 = int(c0 + b1 * BLOCK * ratio) - int(0.05 * rate)        # 50 ms short of the next section
            r.append(stats(x, s0, s1, rate))
        rows.append(r)
    ref = [max(v[0] for v in r) or 1.0 for r in rows]
    print('\n%-12s %8s %8s %6s   %8s %8s   %s' % ('section', 'dev dB', 'emu dB', 'diff', 'dev zc', 'emu zc', ''))
    flagged = 0
    for i, (nm, b0, b1) in enumerate(secs):
        d, e = rows[0][i], rows[1][i]
        dd, ed = db(d[0], ref[0]), db(e[0], ref[1])
        silent = (dd < -45) != (ed < -45)
        bad = silent or (dd > -45 and ed > -45 and abs(dd - ed) > a.tol)
        flagged += bad
        print('%-12s %8.1f %8.1f %6.1f   %8.0f %8.0f   %s' % (nm, dd, ed, dd - ed, d[1], e[1],
                                                         'SILENT IN ONE' if silent else ('CHECK' if bad else '')))
    print('\n%d of %d sections flagged (tolerance %.1f dB relative level)' % (flagged, len(secs), a.tol))
    sys.exit(1 if flagged else 0)


if __name__ == '__main__':
    main()
