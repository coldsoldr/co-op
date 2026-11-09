"""kennel's SDRAM map for Python: reads the #defines of synth_sdk/kn_mem.h (the only place the addresses live), so
tools and harness scripts use the same numbers as the core.

    import kn_mem
    kn_mem.ARENA_LO, kn_mem.ARENA_HI, kn_mem.FILE_MAX, ...   (KN_ prefix dropped)

Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""
import re
from pathlib import Path

HEADER = Path(__file__).resolve().parent / 'kn_mem.h'


def load(path=HEADER):
    """{'ARENA_LO': 0x02980000, ...}: every `#define KN_<NAME> <expr>` of the header, evaluated in order."""
    vals = {}
    for m in re.finditer(r'^#define\s+KN_(\w+)\s+([^/\n]+?)\s*(?:/\*.*)?$', Path(path).read_text(), re.M):
        name, expr = m.groups()
        if '(' in name or name == 'MEM_H':
            continue
        expr = re.sub(r'\b(0x[0-9A-Fa-f]+|\d+)[uU]\b', r'\1', expr)
        expr = re.sub(r'\bKN_(\w+)', lambda k: str(vals[k.group(1)]), expr)
        vals[name] = int(eval(expr, {'__builtins__': {}}))
    return vals


globals().update(load())
