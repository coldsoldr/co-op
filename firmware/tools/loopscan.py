"""Scan stock functions for per-sample waste (docs/performance.md, "Per-engine cost and the engine scan"): hardware loops
(LSETUP), software loops (backward branches), the CALLs inside them and the stack (SP / FP) accesses per pass, for a
function and its larger callees, two levels deep. The input is the objdump text of the decrypted stock image (stock.dis,
see the handover for how to regenerate it).

    python3 -I tools/loopscan.py DIS_DIR NAME=HEXADDR [NAME=HEXADDR ...]
    python3 -I tools/loopscan.py ~/dis dsynth=197e1d8 punch=19e6260

sw-uncond lines are usually a switch dispatch, not a loop; 0x01919AA8 is the assert path. Stdlib only, Python 3.8."""
import re, sys
D = sys.argv[1]
rx = re.compile(r'^ ([0-9a-f]{7,8}):\t((?:[0-9a-f]{2} )+)\s*\t?(.*)$')
order = []; text = {}
for l in open(D + '/stock.dis', errors='replace'):
    m = rx.match(l)
    if m:
        a = int(m.group(1), 16)
        if a in text: continue
        order.append(a); text[a] = m.group(3).strip()
idx = {a: i for i, a in enumerate(order)}
def func_range(start, maxn=3000):
    """from start to the last return before the next LINK that follows a return"""
    i = idx[start]; end = start; seen_ret = False
    for j in range(i, min(i + maxn, len(order))):
        a = order[j]; t = text[a]
        if j > i and seen_ret and t.startswith('LINK'): break
        if j > i and seen_ret and re.match(r'^\[--SP\] = RETS', t): break
        if t.startswith('RTS') or re.match(r'^JUMP \(P\d\);', t) or t.startswith('UNLINK'):
            seen_ret = True
        end = a
    return start, end
def body(s, e):
    return [(a, text[a]) for a in order[idx[s]:idx[e] + 1]]
def target(t, k):
    m = re.search(k + r' 0x0x([0-9a-f]+)', t)
    return int(m.group(1), 16) if m else None
def analyse(name, start, depth, seen):
    if start in seen or start not in idx: return
    seen.add(start)
    s, e = func_range(start)
    ins = body(s, e)
    loops = []
    for a, t in ins:
        m = re.search(r'LSETUP\(0x0x([0-9a-f]+), 0x0x([0-9a-f]+)\) (LC\d)', t)
        if m: loops.append(('hw ' + m.group(3), int(m.group(1), 16), int(m.group(2), 16)))
        if 'JUMP' in t and 'IF' in t:
            tg = target(t, 'JUMP')
            if tg is not None and tg < a and tg >= s: loops.append(('sw', tg, a))
        if re.match(r'^JUMP(\.S|\.L)? 0x0x', t):
            tg = target(t, r'JUMP(?:\.S|\.L)?')
            if tg is not None and tg < a and tg >= s: loops.append(('sw-uncond', tg, a))
    print('%s%s %08x..%08x  %d insns, %d loops' % ('  ' * depth, name, s, e, len(ins), len(loops)))
    callees = []
    for kind, ls, le in loops:
        lb = [(a, t) for a, t in ins if ls <= a <= le]
        calls = [t for a, t in lb if t.startswith('CALL') or ' CALL ' in t]
        stk = sum(1 for a, t in lb if re.search(r'\[(SP|FP) ?[+-]', t))
        print('%s  %-9s %08x..%08x %4d insns  calls %d  frame acc %d %s' % ('  ' * depth, kind, ls, le, len(lb), len(calls), stk,
              ' '.join(sorted(set(re.sub(r'CALL 0x0x', '', c).rstrip(';') for c in calls)))[:150]))
        for c in calls:
            tg = target(c, 'CALL')
            if tg: callees.append(tg)
    if depth < 2:
        for a, t in ins:
            tg = target(t, 'CALL')
            if tg and tg not in seen: callees.append(tg)
        for c in dict.fromkeys(callees):
            if c in idx and c not in seen:
                cs, ce = func_range(c)
                n = idx[ce] - idx[cs] + 1
                if n > 25: analyse('fn_%08x' % c, c, depth + 1, seen)
for arg in sys.argv[2:]:
    n, a = arg.split('=')
    analyse(n, int(a, 16), 0, set())
    print()
