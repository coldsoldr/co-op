#!/usr/bin/env python3
"""Play a scenario on the real OP-1 and record its line out, with nobody at the device (docs/test-loop.md, tier 1).

    devloop-venv\\Scripts\\python tools\\devloop.py tests\\sim\\dev_rings_hold.txt --out RESULTS_DIR

Steps: compile the scenario to CRUN (tools/run_script.py), refuse any key outside the safe set, write it to
/yaffs2/user/devloop.crun over SysEx, start recording the audio interface, start the coop test runner (SysEx 0x27 op 1),
poll until it is done (abort on timeout or Ctrl+C), stop recording, then fetch /yaffs2/user/coop_run.txt and the stats.
In RESULTS_DIR: run.wav (the line-out pair), coop_run.txt, coop_stats.txt, crun.bin and summary.json, whose sections
give the device's own per-section costs (from the run file) beside the recording's level per section (RMS dBFS of
the interface input, aligned on the runner's start click).

Needs the OP-1 on USB in normal mode with no other program holding its MIDI port (close the co-op browser tab), and
the line out into the interface. Recording uses the `sounddevice` package with the ASIO driver (Windows MME/WASAPI
expose only the UR44C's first pair; ASIO gives all six inputs): run it with a Python that has `sounddevice` and
`numpy`, e.g. a venv outside the repo. --no-audio skips the recording (stdlib only then).

Safety: the runner only presses what the scenario says, and this tool refuses tape recording and editing keys (rec,
lift, drop, split, mic) always, and tape transport and the mixer unless --allow-tape. COM (0x2C) is refused by the
runner itself. Knob turns change the current sound's settings; nothing is saved by the runner.
"""
import argparse, json, math, os, struct, sys, threading, time, wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_script import compile_script, crun_bytes, KEYS, EV_KEY, EV_MARK, EV_CLICK, ms_to_block  # noqa: E402
import coop_usb  # noqa: E402

RATE, BLOCK = 44100, 64
DEVICE_PATH = '/yaffs2/user/devloop.crun'
NEVER = {'rec', 'lift', 'drop', 'split', 'mic'}
TAPE = {'tape', 'play', 'stop', 'left', 'right', 'mixer'}


def check_safe(events, allow_tape):
    names = {v: k for k, v in KEYS.items()}
    bad = set()
    for _, ty, a, b in events:
        if ty != EV_KEY:
            continue
        n = names.get(a, hex(a))
        if n in NEVER or (n in TAPE and not allow_tape) or n not in KEYS:
            bad.add(n)
    if bad:
        raise SystemExit('refused: the scenario presses %s (see --allow-tape; rec/lift/drop/split/mic are never '
                         'allowed)' % ', '.join(sorted(bad)))


class Recorder:
    """The interface's input pair, through ASIO (sounddevice), into memory."""

    def __init__(self, device, channels):
        os.environ.setdefault('SD_ENABLE_ASIO', '1')
        import sounddevice as sd  # noqa: deferred so --no-audio needs nothing
        import numpy as np
        self.sd, self.np = sd, np
        self.dev = self.find(device)
        info = sd.query_devices(self.dev)
        self.ch = [c - 1 for c in channels]
        self.n_in = max(max(self.ch) + 1, 2)
        if info['max_input_channels'] < self.n_in:
            raise SystemExit('%s has %d inputs, %s asked for' % (info['name'], info['max_input_channels'], channels))
        self.name = info['name']
        self.chunks, self.lock = [], threading.Lock()
        self.stream = None

    def find(self, pattern):
        for i, d in enumerate(self.sd.query_devices()):
            api = self.sd.query_hostapis(d['hostapi'])['name']
            if d['max_input_channels'] > 0 and pattern.lower() in (api + ' ' + d['name']).lower():
                return i
        raise SystemExit('no input device matches %r (python -c "import sounddevice as sd; print(sd.query_devices())")'
                         % pattern)

    def _cb(self, data, frames, t, status):
        with self.lock:
            self.chunks.append(data[:, self.ch].copy())

    def start(self):
        self.stream = self.sd.InputStream(device=self.dev, channels=self.n_in, samplerate=RATE, dtype='int16',
                                          callback=self._cb, blocksize=0)
        self.stream.start()

    def stop(self):
        self.stream.stop()
        self.stream.close()
        with self.lock:
            return self.np.concatenate(self.chunks) if self.chunks else self.np.zeros((0, len(self.ch)), 'int16')


def write_wav(path, frames):
    w = wave.open(path, 'wb')
    w.setnchannels(frames.shape[1])
    w.setsampwidth(2)
    w.setframerate(RATE)
    w.writeframes(frames.astype('<i2').tobytes())
    w.close()


def parse_run(text):
    """coop_run.txt: per section its blocks, cb_avg/cb_max and the cost / engines entries."""
    secs, cur = [], None
    for line in text.splitlines():
        p = line.split()
        if not p:
            continue
        if p[0] == 'section':
            cur = {'name': p[1]}
            for k, v in zip(p[2::2], p[3::2]):
                if v.lstrip('-').isdigit():
                    cur[k] = int(v)
            secs.append(cur)
        elif cur is not None and p[0] in ('cost', 'engines'):
            for item in p[1:]:
                if '=' in item:
                    k, v = item.split('=', 1)
                    nums = v.split('/')
                    if nums[0].lstrip('-').isdigit():
                        cur.setdefault(p[0], {})[k] = [int(x) for x in nums if x.lstrip('-').isdigit()]
    return secs


def analyse(frames, events, marks, np):
    """Level per section of the recording, aligned on the start click."""
    if not len(frames):
        return {'error': 'no audio recorded'}
    x = frames.astype(np.float64).mean(axis=1)
    head = np.abs(x[:int(0.3 * RATE)])
    floor = float(np.median(head)) + 1.0
    th = max(20 * floor, 0.2 * float(np.abs(x).max()))
    hit = np.nonzero(np.abs(x) >= th)[0]
    if not len(hit):
        return {'error': 'start click not found', 'peak_dbfs': db(float(np.abs(x).max()))}
    t0 = int(hit[0])
    out = {'click_sample': t0, 'floor_dbfs': db(floor), 'sections': []}
    spans, start, name = [], None, None
    for blk, ty, a, _ in events:
        if ty == EV_MARK:
            if start is not None:
                spans.append((name, start, blk))
            name = marks[a]
            start = None if name == '-' else blk
    for name, a, b in spans:
        seg = x[t0 + a * BLOCK:t0 + b * BLOCK]
        if not len(seg):
            continue
        rms = float(np.sqrt((seg * seg).mean()))
        # level per second, to see a tail fall
        per_s = [db(float(np.sqrt((s * s).mean()))) for s in np.array_split(seg, max(1, len(seg) // RATE))]
        out['sections'].append({'name': name, 'seconds': round(len(seg) / RATE, 2), 'rms_dbfs': db(rms),
                                'peak_dbfs': db(float(np.abs(seg).max())), 'rms_dbfs_per_second': per_s})
    return out


def db(v):
    return round(20 * math.log10(v / 32768.0), 1) if v > 0 else -200.0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('scenario')
    ap.add_argument('--out', required=True, help='results directory (created)')
    ap.add_argument('--device', default='Steinberg USB ASIO', help='input device name part (default: the UR44C ASIO driver)')
    ap.add_argument('--channels', default='5,6', help='interface inputs, 1-based (default 5,6)')
    ap.add_argument('--no-audio', action='store_true')
    ap.add_argument('--allow-tape', action='store_true', help='allow tape/play/stop/left/right/mixer keys')
    ap.add_argument('--port', default=None, help='MIDI port name part (default: OP-1)')
    ap.add_argument('--dry', action='store_true', help='compile and check only')
    ap.add_argument('--wait-port', type=float, default=0, help='seconds to wait for the OP-1 MIDI port to appear')
    a = ap.parse_args(argv)

    text = open(a.scenario, encoding='utf-8').read()
    name, marks, events, _ = compile_script(text, os.path.splitext(os.path.basename(a.scenario))[0])
    check_safe(events, a.allow_tape)
    crun = crun_bytes(name, marks, events)
    end_block = max(e[0] for e in events)
    run_s = end_block * BLOCK / RATE
    print('scenario %s: %d events, %d marks, %.1f s' % (name, len(events), len(marks), run_s), flush=True)
    if a.dry:
        return 0
    os.makedirs(a.out, exist_ok=True)
    open(os.path.join(a.out, 'crun.bin'), 'wb').write(crun)

    t, until = None, time.time() + a.wait_port
    while t is None:
        try:
            t = coop_usb.WinMMTransport(a.port)
        except coop_usb.CoopError as e:
            if time.time() > until:
                raise SystemExit(str(e))
            time.sleep(2.0)
    s = coop_usb.Session(t, allow_write=True)
    summary = {'scenario': a.scenario, 'name': name, 'seconds': round(run_s, 1)}
    rec = None
    try:
        summary['hello'] = coop_usb.hello(s)
        if 'run' not in summary['hello'].get('caps_names', []):
            raise SystemExit('this core has no test runner')
        st = coop_usb.run_status(s)
        if st['state'] in ('armed', 'running'):
            raise SystemExit('a run is already in progress: %s' % st)
        coop_usb.put(s, DEVICE_PATH, crun)
        if not a.no_audio:
            rec = Recorder(a.device, [int(c) for c in a.channels.split(',')])
            summary['audio_device'] = rec.name
            rec.start()
            time.sleep(0.5)
        status, _ = s.ask(coop_usb.RUN, b'\x01' + DEVICE_PATH.encode())
        if status != 0:
            raise SystemExit('the runner refused the start: status %d' % status)
        t_start = time.time()
        deadline = t_start + run_s + 20
        last = None
        while True:
            time.sleep(1.0)
            st = coop_usb.run_status(s)
            if st['state'] != last:
                print('  %5.1f s  %s' % (time.time() - t_start, st['state']), flush=True)
                last = st['state']
            if st['state'] in ('done', 'stopped', 'error'):
                break
            if time.time() > deadline:
                s.ask(coop_usb.RUN, b'\x02')
                print('  timeout: aborted', flush=True)
                st = coop_usb.run_status(s)
                break
        summary['runner'] = st
        time.sleep(1.0)
    except KeyboardInterrupt:
        try:
            s.ask(coop_usb.RUN, b'\x02')
        except Exception:
            pass
        print('interrupted: run aborted')
    finally:
        if rec is not None:
            frames = rec.stop()
            write_wav(os.path.join(a.out, 'run.wav'), frames)
            summary['audio'] = analyse(frames, events, marks, rec.np)
    for path, fn in (('/yaffs2/user/coop_run.txt', 'coop_run.txt'), ('/yaffs2/user/coop_stats.txt', 'coop_stats.txt')):
        try:
            data = coop_usb.file_get(s, path)
            open(os.path.join(a.out, fn), 'wb').write(data)
            if fn == 'coop_run.txt':
                summary['run_sections'] = parse_run(data.decode('latin-1'))
        except coop_usb.CoopError as e:
            summary.setdefault('errors', []).append('%s: %s' % (path, e))
    json.dump(summary, open(os.path.join(a.out, 'summary.json'), 'w'), indent=1)
    print(json.dumps({k: summary[k] for k in summary if k in ('runner', 'audio', 'errors')}, indent=1)[:6000])
    return 0


if __name__ == '__main__':
    sys.exit(main())
