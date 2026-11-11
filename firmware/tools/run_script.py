#!/usr/bin/env python3
"""Compile an op1emu headless scenario into a CRUN file for the coop test runner (core/sdram/coop_runner.cc).

The same scenario then drives the device (tools/coop_sysex.html uploads and starts it) and op1emu (the file on the
NAND overlay as user/coop_autorun.crun, an image with g_coop_run_autostart set: tests/sim/run_emu.sh). docs/test-runner.md
has the whole flow.

    python3 -I tools/run_script.py tests/sim/run_p5a.txt -o build/run_p5a.crun      # the file the page sends
    python3 -I tools/run_script.py tests/sim/run_p5a.txt --list                     # print the events

Script lines (op1emu's own, guest-timed): `at MS`, `press KEY`, `release KEY`, `tap KEY [MS]` (100 ms by default),
`turn ENC cw|ccw [STEPS]` (4 by default, one AddKnob(+-1) per step, 20 ms apart, as op1emu's Gray steps). Lines the
runner has no use for (accel, wait, wait-frames, screenshot, quit, volume) are skipped. Directives in comments, so
op1emu ignores them:
    #@mark NAME     a section starts at the current `at` time (the run file reports each section's costs)
    #@tail MS       how long the run goes on after the last event (default 8000)
    #@lead MS       the gap from the start click to the first event (default 1000)
    #@name NAME     the script name in the run file (default: the file name)
    #@pfmode N      m1: the performance counters' mode (0..4, 255 off; coop_stats.cc) from the current `at` time on;
                    put it before the #@mark of the section it is for

Times are made relative: the first event lands LEAD ms after the start click (block 0), every other event keeps its
distance from the first. A block is 64 samples at 44.1 kHz. The end click and the END event come TAIL ms after the
last event.

The JavaScript compiler in tools/coop_sysex.html follows this file; tests/sim/run_script_check.sh compares the two.
"""
import argparse, os, struct, sys

# Key codes: the KeyHandler code of each op1emu key name (gui/ui.json gpio [bank, bit] -> (bank - 3) * 16 + bit,
# docs/screens.md; the encoder pushes 0x3B..0x3E were traced in op1emu, tests/sim/probe_knobs.txt).
KEYS = {
    'help': 0x00, 'tempo': 0x01, 'synth': 0x02, 'drum': 0x03, 'tape': 0x04, 'mixer': 0x05,
    't1': 0x06, 't2': 0x07, 't3': 0x08, 't4': 0x09, 'lift': 0x0a, 'drop': 0x0b, 'split': 0x0c,
    'f3#': 0x0d, 'g3#': 0x0e, 'a3#': 0x0f, 's4': 0x10, 's5': 0x11, 's6': 0x12, 's7': 0x13, 's8': 0x14,
    'seq': 0x15, 'd4#': 0x16, 'f4#': 0x17, 'g4#': 0x18, 'a4#': 0x19, 'c5#': 0x1a, 'd5#': 0x1b,
    'c4': 0x1c, 'd4': 0x1d, 'e4': 0x1e, 'f4': 0x1f, 'c4#': 0x20, 'rec': 0x21, 'play': 0x22, 'stop': 0x23,
    'left': 0x24, 'right': 0x25, 'shift': 0x26, 'f3': 0x27, 'g3': 0x28, 'a3': 0x29, 'b3': 0x2a, 'mic': 0x2b,
    's1': 0x2d, 's2': 0x2e, 's3': 0x2f, 'g4': 0x30, 'a4': 0x31, 'b4': 0x32, 'c5': 0x33, 'd5': 0x34, 'e5': 0x35,
    'enc1': 0x3b, 'enc2': 0x3c, 'enc3': 0x3d, 'enc4': 0x3e,
}
ENCODERS = {'enc1': 0, 'enc2': 1, 'enc3': 2, 'enc4': 3}
EV_KEY, EV_KNOB, EV_CLICK, EV_MARK, EV_END, EV_PFMODE = 1, 2, 3, 4, 5, 6
NAME_LEN, MARK_LEN, MAX_EV, MAX_MARKS = 32, 16, 2048, 32
SKIP = {'accel', 'wait', 'wait-frames', 'screenshot', 'quit', 'volume'}


class ScriptError(Exception):
    pass


def ms_to_block(ms):
    return (ms * 44100 + 32000) // 64000                 # round(ms * 44.1 / 64)


def compile_script(text, default_name='script'):
    """Returns (name, marks, events): events are (ms, order, type, a, b) before the time shift, sorted."""
    t = None
    raw = []                                             # (ms, seq, type, a, b)
    marks = []
    tail, lead, name = 8000, 1000, default_name
    seq = 0
    for n, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if not s:
            continue
        if s.startswith('#@'):
            parts = s[2:].split(None, 1)
            if not parts:
                continue
            d, arg = parts[0], (parts[1].strip() if len(parts) > 1 else '')
            if d == 'mark':
                if t is None:
                    raise ScriptError('line %d: #@mark needs a preceding at' % n)
                if not arg or len(arg.encode()) > MARK_LEN:
                    raise ScriptError('line %d: mark name of 1..%d bytes expected' % (n, MARK_LEN))
                if arg not in marks:
                    marks.append(arg)
                raw.append((t, seq, EV_MARK, marks.index(arg), 0)); seq += 1
            elif d == 'pfmode':
                if t is None:
                    raise ScriptError('line %d: #@pfmode needs a preceding at' % n)
                m = int(arg)
                if not (0 <= m <= 4 or m == 255):
                    raise ScriptError('line %d: #@pfmode 0..4 or 255 expected' % n)
                raw.append((t, seq, EV_PFMODE, m, 0)); seq += 1
            elif d == 'tail':
                tail = int(arg)
            elif d == 'lead':
                lead = int(arg)
            elif d == 'name':
                name = arg
            else:
                raise ScriptError('line %d: unknown directive #@%s' % (n, d))
            continue
        if s.startswith('#'):
            continue
        a = s.split()
        cmd = a[0]
        if cmd in SKIP:
            continue
        if cmd == 'at':
            if len(a) != 2:
                raise ScriptError('line %d: at MS expected' % n)
            t = int(a[1])
            continue
        if t is None:
            raise ScriptError('line %d: %s needs a preceding at (the runner is guest-timed only)' % (n, cmd))
        if cmd in ('press', 'release', 'tap'):
            if len(a) not in (2, 3) or (cmd != 'tap' and len(a) != 2):
                raise ScriptError('line %d: %s KEY expected' % (n, cmd))
            if a[1] not in KEYS:
                raise ScriptError('line %d: unknown key %s' % (n, a[1]))
            code = KEYS[a[1]]
            if cmd == 'tap':
                dur = int(a[2]) if len(a) == 3 else 100
                raw.append((t, seq, EV_KEY, code, 1)); seq += 1
                raw.append((t + dur, seq, EV_KEY, code, 0)); seq += 1
            else:
                raw.append((t, seq, EV_KEY, code, 1 if cmd == 'press' else 0)); seq += 1
        elif cmd == 'turn':
            if len(a) not in (3, 4) or a[1] not in ENCODERS or a[2] not in ('cw', 'ccw'):
                raise ScriptError('line %d: turn ENC cw|ccw [STEPS] expected' % n)
            steps = int(a[3]) if len(a) == 4 else 4
            if not 1 <= steps <= 4096:
                raise ScriptError('line %d: steps 1..4096' % n)
            d = 1 if a[2] == 'cw' else -1
            for k in range(steps):
                raw.append((t + 20 * k, seq, EV_KNOB, ENCODERS[a[1]], d)); seq += 1
        else:
            raise ScriptError('line %d: the runner cannot play "%s"' % (n, cmd))
    if not raw:
        raise ScriptError('no events')
    if len(marks) > MAX_MARKS:
        raise ScriptError('more than %d marks' % MAX_MARKS)
    raw.sort(key=lambda e: (e[0], e[1]))
    t0 = raw[0][0]
    events = [(ms_to_block(0), EV_CLICK, 0, 0)]
    for ms, _, ty, a_, b_ in raw:
        events.append((ms_to_block(ms - t0 + lead), ty, a_, b_))
    end = ms_to_block(raw[-1][0] - t0 + lead + tail)
    events.append((end, EV_CLICK, 0, 0))
    events.append((end, EV_END, 0, 0))
    if len(events) > MAX_EV:
        raise ScriptError('%d events, the runner holds %d' % (len(events), MAX_EV))
    return name, marks, events, t0 - lead


def crun_bytes(name, marks, events):
    nb = name.encode()[:NAME_LEN]
    out = bytearray(b'CRUN' + struct.pack('<HHHH', 1, len(events), len(marks), 0) + nb + b'\0' * (NAME_LEN - len(nb)))
    for m in marks:
        mb = m.encode()
        out += mb + b'\0' * (MARK_LEN - len(mb))
    for blk, ty, a, b in events:
        out += struct.pack('<IBBbB', blk, ty, a, b, 0)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('script')
    ap.add_argument('-o', '--out', help='write the CRUN file')
    ap.add_argument('--list', action='store_true', help='print the events')
    args = ap.parse_args()
    text = open(args.script, encoding='utf-8').read()
    try:
        name, marks, events, origin = compile_script(text, os.path.splitext(os.path.basename(args.script))[0])
    except ScriptError as e:
        sys.exit('%s: %s' % (args.script, e))
    data = crun_bytes(name, marks, events)
    if args.out:
        open(args.out, 'wb').write(data)
    if args.list:
        names = {v: k for k, v in KEYS.items()}
        for blk, ty, a, b in events:
            what = {EV_KEY: 'key %s %s' % (names.get(a, hex(a)), 'down' if b else 'up'), EV_KNOB: 'knob %d %+d' % (a, b),
                    EV_CLICK: 'click', EV_MARK: 'mark %s' % (marks[a] if ty == EV_MARK else ''), EV_END: 'end',
                    EV_PFMODE: 'pfmode %d' % a}[ty]
            print('%7d  %9.1f ms  %s' % (blk, blk * 64000.0 / 44100, what))
    print('%s: %d events, %d marks, %d blocks (%.1f s), %d bytes; block 0 = op1emu guest %d ms'
          % (name, len(events), len(marks), events[-1][0], events[-1][0] * 64.0 / 44100, len(data), origin))


if __name__ == '__main__':
    main()
