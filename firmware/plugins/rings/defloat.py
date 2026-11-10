#!/usr/bin/env python3
"""Turn the Rings port's compile-time double constants into integer literals (guidelines §2.1: no float or double in
a coop plugin's files, not even constexpr). Run once over the files vendored from te-op1-cfw/ports/rings:

    python3 -I defloat.py src/string_fixed.cc src/string_voice.cc src/reverb_fixed.cc ...

Each call Q(x, n), St(x), LfoIncrement(hz) and FFloat(<number>) whose argument is a constant expression is evaluated
with the upstream C++ definition, in IEEE double (Python's float, the same arithmetic and order as GCC's constexpr
evaluation), at OP1RINGS_SAMPLE_RATE = 22050, the coop build's rate. The original call stays in a comment beside the
literal. A call it cannot evaluate is reported and left alone. The coop host test (tests/rings_host.cc) checks the
result against the te-op1-cfw build bit for bit. Stdlib only, Python 3.8.
"""
import re
import sys

RATE = 22050
INT32_MIN = -(1 << 31)


def q(x, n):                     # static_cast<int32_t>(x * double(1LL << n) + (x < 0.0 ? -0.5 : 0.5))
    v = int(x * float(1 << n) + (-0.5 if x < 0.0 else 0.5))
    if not INT32_MIN <= v < (1 << 31):
        raise ValueError(f'Q({x}, {n}) out of int32')
    return v


def lfo_reverb(hz):              # reverb_fixed.cc: 0.9003163161571061 * hz * 32.0 / RATE * 4294967296.0
    return int(0.9003163161571061 * hz * 32.0 / RATE * 4294967296.0)


def lfo_string(hz):              # string_voice.cc: 0.9003163161571061 * hz / RATE * 4294967296.0
    return int(0.9003163161571061 * hz / RATE * 4294967296.0)


def ffloat(v):                   # ffloat.h constexpr FFloat(double): -> (m, e), value = m * 2^e, |m| in [2^30, 2^31)
    if v == 0.0:
        return 0, 0
    e, a = 0, -v if v < 0.0 else v
    while a >= 2147483648.0:
        a *= 0.5
        e += 1
    while a < 1073741824.0:
        a *= 2.0
        e -= 1
    m = int(a)
    return (-m if v < 0.0 else m), e


def lit(v):
    return '(-2147483647 - 1)' if v == INT32_MIN else str(v)


def evaluate(expr):
    expr = expr.replace('OP1RINGS_SAMPLE_RATE', str(RATE))
    if not re.fullmatch(r'[0-9.eE+\-*/() ]+', expr):
        raise ValueError('not a constant expression')
    if re.search(r'\d+\s*/\s*\d+(?![.\deE])', expr) and not re.search(r'\.', expr):
        raise ValueError('integer division')   # C would divide as integers; none expected
    return float(eval(expr, {'__builtins__': {}}))


def args_at(text, open_paren):
    """-> (end index after ')', [top-level comma-separated args])"""
    depth, start, out = 0, open_paren + 1, []
    for i in range(open_paren, len(text)):
        c = text[i]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                out.append(text[start:i].strip())
                return i + 1, out
        elif c == ',' and depth == 1:
            out.append(text[start:i].strip())
            start = i + 1
    raise ValueError('unbalanced parentheses')


CALL = re.compile(r'\b(Q|St|LfoIncrement|FFloat)\(')
DEFINITION = re.compile(r'constexpr\s+\w+\s+(Q|St|LfoIncrement)\(\s*double')


def convert(path):
    text = open(path).read()
    lfo = lfo_reverb if 'reverb_fixed' in path else lfo_string
    out, pos, done, skipped = [], 0, 0, []
    for m in CALL.finditer(text):
        if m.start() < pos:
            continue
        line_start = text.rfind('\n', 0, m.start()) + 1
        if DEFINITION.match(text, line_start + len(text[line_start:m.start()]) - len(text[line_start:m.start()].lstrip())) \
                or DEFINITION.search(text[line_start:m.end() + 8]):
            continue                                   # the helper's own definition: removed by hand
        end, args = args_at(text, m.end() - 1)
        name, call = m.group(1), text[m.start():end]
        try:
            if name == 'Q':
                rep = lit(q(evaluate(args[0]), int(args[1])))
            elif name == 'St':
                rep = lit(q(evaluate(args[0]), 24))
            elif name == 'LfoIncrement':
                rep = f'{lfo(evaluate(args[0]))}u'
            else:
                if not args[0] or not re.search(r'\d\.|\.\d', args[0]):
                    continue                           # FFloat() or FFloat(non-literal): not the double constructor
                mm, ee = ffloat(evaluate(args[0]))
                rep = f'FFloat::Raw({lit(mm)}, {ee})'
        except ValueError as e:
            skipped.append(f'{call}: {e}')
            continue
        out.append(text[pos:m.start()])
        out.append(f'{rep} /* {call} */')
        pos, done = end, done + 1
    out.append(text[pos:])
    open(path, 'w', newline='\n').write(''.join(out))
    print(f'{path}: {done} constants')
    for s in skipped:
        print(f'  left: {s}')
    return not skipped


if __name__ == '__main__':
    ok = all([convert(p) for p in sys.argv[1:]])
    sys.exit(0 if ok else 1)
