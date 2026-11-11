# coop-fw: kennel 3.1 tools/kennel/pack.py (GPL-3.0-or-later, Hundekraft) adapted: the magic COOP (reloc.py), the
# coop SDK and core/include on the include path, C++ sources, <id>.coop output. Usage:
#   python3 -I tools/plugin/pack.py plugins/stereo_delay -o build/plugins/stereo_delay.coop --toolchain ~/op1-toolchain
#!/usr/bin/env python3
"""Pack a synth/FX plugin folder (plugin.json + C sources) into a kennel plugin file (docs/plugin-format.md).

    python3 tools/kennel/pack.py synth_sdk/plugins/supersaw -o build/supersaw.knl [--toolchain DIR]

Compiles the sources (same flags as make_firmware.py), links them with libgcc into one relocatable object
(`bfin-elf-ld -r` with merge.ld: one .text, .rodata, .data, .bss), then lays the sections out from offset 0 and
resolves every relocation it can. What depends on the load address stays in the file's relocation table:
absolute words and hi/lo immediates that point into the plugin, and everything that points at a core import.
Any other relocation type or allocated section is an error.
Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
if not hasattr(Path, "is_relative_to"):      # coop-fw: Python 3.8 (WSL)
    def _irt(self, other):
        try: self.relative_to(other); return True
        except ValueError: return False
    Path.is_relative_to = _irt

sys.path.insert(0, str(Path(__file__).resolve().parent))
import reloc as F

KENNEL = Path(__file__).resolve().parents[2]
SDK = KENNEL / 'sdk'                    # coop-fw: the public ABI headers
CORE_INC = KENNEL / 'core' / 'include'  # coop-fw: the DSP headers a plugin may share with the core
MERGE_LD = Path(__file__).resolve().parent / 'merge.ld'
CFLAGS = ['-O2', '-mcpu=bf524', '-ffreestanding', '-fno-builtin', '-nostdlib', '-fno-common', '-Wall']


def diag_build():
    """The test build (KN_DIAG=1 in the environment): monitor page 7 and the diagnostic core (ARCHITECTURE.md
    "Release and test builds"). Unset, empty or 0: the release build. make_firmware.py reads the same variable."""
    v = os.environ.get('KN_DIAG', '')
    if v not in ('', '0', '1'):
        raise PackError(f'KN_DIAG must be 0 or 1, not {v!r}')
    return v == '1'


def diag_cflags():
    return ['-DKN_DIAG=1'] if diag_build() else []


def core_exports(text=None):
    """kn_exports.c as the build sees it: the KN_DIAG block left out unless this is the test build (the tests read the
    export table as text: a plugin must not import an export only the test build has)."""
    text = (SDK / 'coop_exports.cc').read_text() if text is None else text
    if diag_build():
        return text
    return re.sub(r'^#if KN_DIAG\b.*?^#endif[^\n]*\n', '', text, flags=re.S | re.M)

# ELF relocation types (binutils include/elf/bfin.h) the plugins produce
R_BFIN_PCREL24, R_BFIN_PCREL24_JUMP_L = 0x0a, 0x0d
R_BFIN_LUIMM16, R_BFIN_HUIMM16, R_BFIN_BYTE4_DATA = 0x06, 0x07, 0x12
ELF_TO_FILE = {R_BFIN_BYTE4_DATA: F.R_ABS32, R_BFIN_HUIMM16: F.R_HI16, R_BFIN_LUIMM16: F.R_LO16,
               R_BFIN_PCREL24: F.R_PCREL24, R_BFIN_PCREL24_JUMP_L: F.R_PCREL24}
SECTIONS = ('.text', '.rodata', '.data', '.bss')
SHT_NOBITS, SHT_RELA, SHT_REL, SHF_ALLOC = 8, 4, 9, 2
SHN_UNDEF, SHN_ABS = 0, 0xFFF1


class PackError(Exception):
    pass


def svg_turn_backs(svg):
    """synth_sdk/harness/svg_check.py's turn-back rule (one rule for the harness and the packer)"""
    sys.path.insert(0, str(KENNEL / 'third_party' / 'kennel' / 'synth_sdk' / 'harness'))   # coop-fw: the vendored copy
    try:
        import svg_check
    finally:
        sys.path.pop(0)
    return [t for t in svg_check.turn_backs_in(svg) if not t.startswith('not parsed')]   # parse errors: the loader's call


def find_toolchain(arg=None):
    """-> (gcc, ld) paths: --toolchain DIR, $KENNEL_TOOLCHAIN, OP1-Emulator's toolchain/ (the first one walking up
    from this checkout: ext/kennel or a worktree under it), or PATH."""
    roots = [arg, os.environ.get('KENNEL_TOOLCHAIN'), *(str(p / 'toolchain') for p in KENNEL.parents)]
    for r in filter(None, roots):
        gcc, ld = Path(r) / 'bfin/bin/bfin-elf-gcc', Path(r) / 'bfin/bin/bfin-elf-ld'   # coop-fw: ~/op1-toolchain
        if gcc.exists() and ld.exists():
            return str(gcc), str(ld)
    gcc, ld = shutil.which('bfin-elf-gcc'), shutil.which('bfin-elf-ld')
    if gcc and ld:
        return gcc, ld
    raise PackError('bfin-elf-gcc / bfin-elf-ld not found (use --toolchain or KENNEL_TOOLCHAIN)')


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise PackError(f'{" ".join(cmd)}\n{r.stdout}{r.stderr}')
    return r.stdout


def scratch_bytes(meta):
    """coop e8: plugin.json "scratch" (the position buffer bytes an effect works in, sdk/coop_plugin.h
    coop_fx_scratch) -> the int, or None when not declared. An effect only, 1..SCRATCH_MAX."""
    if 'scratch' not in meta:
        return None
    n = meta['scratch']
    if type(n) is not int or not 0 < n <= F.SCRATCH_MAX:
        raise PackError(f'scratch must be the bytes the effect needs, 1..{F.SCRATCH_MAX} (COOP_FX_SCRATCH_MAX)')
    if meta.get('kind') != 'fx':
        raise PackError('scratch is for effects only (kind fx)')
    return n


def compile_plugin(plugin_dir, outdir, toolchain=None):
    """Compile plugin.json's sources -> list of object paths."""
    gcc, _ = find_toolchain(toolchain)
    meta = json.loads((Path(plugin_dir) / 'plugin.json').read_text())
    flags = meta.get('cflags', [])
    scratch = scratch_bytes(meta)                       # e8: the sources see the declared bytes (a port static_asserts)
    defs = [f'-DCOOP_FX_SCRATCH_BYTES={scratch}'] if scratch else []
    allowed = re.compile(r'-(?:D[A-Za-z_][A-Za-z0-9_]*(?:=[A-Za-z0-9_]+)?|O[0123sg]|fno-tree-loop-distribute-patterns)$')
    source_flags = meta.get('source_cflags', {})
    if not isinstance(source_flags,dict) or any(k not in meta['sources'] for k in source_flags): raise PackError('Invalid source compiler flags')
    for group in [flags, *source_flags.values()]:
        if not isinstance(group,list) or any(not isinstance(x,str) or not allowed.fullmatch(x) for x in group): raise PackError('Unsupported compiler flags')
    includes = []
    for rel in meta.get('include_dirs', []):
        path = (Path(plugin_dir) / rel).resolve()
        if not path.is_relative_to(Path(plugin_dir).resolve()): raise PackError('Include escapes plugin')
        includes.append('-I' + str(path))
    sdk_root = Path(plugin_dir).resolve().parents[1] if Path(plugin_dir).parent.name == 'plugins' else SDK
    for rel in meta.get('sdk_include_dirs', []):
        path = (sdk_root / rel).resolve()
        if not path.is_relative_to(sdk_root.resolve()): raise PackError('Generated include escapes SDK')
        includes.append('-I' + str(path))
    objs = []
    for src in meta['sources']:
        path = (Path(plugin_dir) / src).resolve()
        if not (path.is_relative_to(Path(plugin_dir).resolve()) or path.is_relative_to(sdk_root.resolve())):
            raise PackError('Source escapes plugin and SDK')
        o = Path(outdir) / (Path(src).stem + '.o')
        cxx = str(path).endswith(('.cc', '.cpp'))        # coop-fw: C++ sources through bfin-elf-g++
        cc = str(gcc)[:-3] + 'g++' if cxx else gcc
        extra = ['-std=c++17', '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics'] if cxx else []
        run([cc, *CFLAGS, *extra, *diag_cflags(), *defs, *flags, *meta.get('source_cflags', {}).get(src, []), *includes, f'-I{SDK}', f'-I{CORE_INC}', '-c', str(Path(plugin_dir) / src), '-o', str(o)])
        objs.append(str(o))
    return objs


def generated_ready(plugin_dir):
    """True when the build-time files plugin.json lists under "generated" (relative to the SDK) exist. make_firmware.py
    writes them; for a test run without a firmware build the Oblique Strategies cards (build/oblique_data.c) are made
    here from the stock opfont.svg (KENNEL_OPFONT, or a firmware tree next to the repo) when it can be found."""
    meta = json.loads((Path(plugin_dir) / 'plugin.json').read_text())
    for rel in meta.get('generated', []):
        sdk_root = Path(plugin_dir).resolve().parents[1] if Path(plugin_dir).parent.name == 'plugins' else SDK
        out = sdk_root / rel
        if out.exists():
            continue
        fonts = [os.environ.get('KENNEL_OPFONT', '')] + [str(r / 'op1_246/content/display/opfont.svg')
                                                         for r in (SDK.parent, SDK.parent.parent.parent / 'firmware')]
        font = next((f for f in fonts if f and os.path.exists(f)), None)
        if rel != 'build/oblique_data.c' or not font:
            return False
        sys.path.insert(0, str(SDK / 'display'))
        import gen_oblique
        out.parent.mkdir(exist_ok=True)
        gen_oblique.emit(gen_oblique.build(font, str(SDK / 'screens/oblique/cards.txt'))[0], str(out))
    return True


def libgcc(toolchain=None):
    gcc, _ = find_toolchain(toolchain)
    return run([gcc, '-mcpu=bf524', '-print-libgcc-file-name']).strip()


def link_relocatable(objs, out, toolchain=None):
    """bfin-elf-ld -r: plugin objects + the libgcc members they need -> one object with four sections."""
    _, ld = find_toolchain(toolchain)
    # coop-fw (2026-11-10, the rings port): --force-group-allocation, as te-op1-cfw's knl_pack.py links. C++ inline
    # variables, out-of-line inline functions and template instances sit in COMDAT groups, which a relocatable link
    # otherwise keeps as their own sections (pack() refuses those); the flag places them as a final link would, so
    # merge.ld's four sections are all there is. A plugin without COMDAT groups packs byte-identical with or without it.
    run([ld, '-r', '--force-group-allocation', '-T', str(MERGE_LD), *objs, libgcc(toolchain), '-o', str(out)])
    return out


class Elf:
    """Just enough ELF32-LE: section headers, symbols, RELA sections."""

    def __init__(self, data):
        if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
            raise PackError('not an ELF32 little-endian file')
        if struct.unpack_from('<H', data, 16)[0] != 1:
            raise PackError('not a relocatable object (ET_REL)')
        self.data = data
        shoff, = struct.unpack_from('<I', data, 32)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 46)
        self.sh = [dict(zip(('name', 'type', 'flags', 'addr', 'offset', 'size', 'link', 'info', 'align', 'entsize'),
                            struct.unpack_from('<10I', data, shoff + i * shentsize))) for i in range(shnum)]
        names = self.sh[shstrndx]
        for s in self.sh:
            s['name'] = self.cstr(names['offset'] + s['name'])
        symtab = next(s for s in self.sh if s['type'] == 2)
        strtab = self.sh[symtab['link']]['offset']
        self.syms = []
        for i in range(symtab['size'] // 16):
            name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', data, symtab['offset'] + i * 16)
            self.syms.append({'name': self.cstr(strtab + name), 'value': value, 'size': size, 'bind': info >> 4,
                              'type': info & 15, 'shndx': shndx})

    def cstr(self, off):
        return self.data[off:self.data.index(b'\0', off)].decode()

    def contents(self, s):
        return b'' if s['type'] == SHT_NOBITS else self.data[s['offset']:s['offset'] + s['size']]

    def relas(self, s):
        return [struct.unpack_from('<IIi', self.data, s['offset'] + i * 12) for i in range(s['size'] // 12)]


def api_version():
    m = re.search(r'#define\s+COOP_PLUGIN_API\s+(\d+)', (SDK / 'coop_plugin.h').read_text())
    return int(m.group(1))


def pcrel24(img, off, disp, where):
    """Write a CALL / JUMP.L displacement; off = ELF r_offset = second halfword of the instruction."""
    if disp % 2 or not F.PCREL24_MIN <= disp <= F.PCREL24_MAX:
        raise PackError(f'{where}: PCREL24 displacement {disp} out of range')
    d = (disp >> 1) & 0xFFFFFF
    hw, = struct.unpack_from('<H', img, off - 2)
    struct.pack_into('<HH', img, off - 2, (hw & 0xFF00) | (d >> 16), d & 0xFFFF)


def section_layout(elf):
    """-> (secs {name: section index}, layout {name: (offset, size)}, image alignment): the file's image layout"""
    secs = {}
    for i, s in enumerate(elf.sh):
        if s['flags'] & SHF_ALLOC:
            if s['name'] not in SECTIONS:
                raise PackError(f'allocated section {s["name"]} not supported (only {", ".join(SECTIONS)})')
            secs[s['name']] = i
        if s['type'] == SHT_REL:
            raise PackError(f'{s["name"]}: REL relocations not supported (expected RELA)')
    # layout: text at 0, then rodata, data, bss, each aligned to max(4, its alignment), as base.ld places them
    layout, cur, align = {}, 0, 4
    for name in SECTIONS:
        s = elf.sh[secs[name]] if name in secs else {'align': 4, 'size': 0}
        a = max(4, s['align'])
        align = max(align, a)
        cur = (cur + a - 1) & -a
        layout[name] = (cur, s['size'])
        cur += s['size']
    return secs, layout, align


def image_symbols(obj_bytes):
    """`ld -r` output (pack --keep: <id>.ro) -> [(image offset, size, name)] of its functions and objects, sorted:
    for turning a relocated address into a symbol (symbolicate.py)"""
    elf = Elf(obj_bytes)
    secs, layout, _ = section_layout(elf)
    base = {i: layout[name][0] for name, i in secs.items()}
    out = {(base[s['shndx']] + s['value'], s['size'], s['name']) for s in elf.syms
           if s['shndx'] in base and s['type'] in (1, 2) and s['name']}       # STT_OBJECT, STT_FUNC
    return sorted(out)


# coop e4: engine controls beyond the four turns (docs/engines.md "Controls", guide/plugin-guidelines.md §3.1, §4).
CONTROL_ACTIONS = {'click': 1, 'shift_click': 3, 'fn_turn': 4, 'fn_click': 5, 'shift_fn_turn': 6, 'shift_fn_click': 7}
CONTROL_ENCODERS = {'blue': 0, 'green': 1, 'white': 2, 'orange': 3}
STOCK_GLYPHS = set('abcdefghijklmnopqrstuvwxyz0123456789!"#%*+,-./:;<=>?_ ')


def control_lines(meta, kind):
    """plugin.json "controls" -> the text of extension tag 6, one tab-separated line per control:
    action encoder word kind count default random detents name step,step,... [flags] (core/sdram/engine_ui.cc).
    flags (an 11th field): 1 wrap (steps, e5), 2 fine (continuous fn_turn, e6). A latch (effects, e6) packs as kind l
    with its bit in the word column."""
    controls = meta.get('controls')
    if controls is None:
        return b''
    if kind not in (F.KIND_SYNTH, F.KIND_FX):
        raise PackError('controls are for synth engines and effects')
    fx = kind == F.KIND_FX
    if not isinstance(controls, list) or not 1 <= len(controls) <= 8:
        raise PackError('controls must be a list of 1 to 8 controls')

    def label(text, what):
        if not isinstance(text, str) or not 1 <= len(text) <= 8 or set(text) - STOCK_GLYPHS or ',' in text:
            raise PackError(f'{what} {text!r}: 1 to 8 characters, lower case, from the stock glyphs, no comma')
        return text

    lines, words, slots, bits, fines, deep = [], set(), set(), set(), set(), set()
    for c in controls:
        if not isinstance(c, dict):
            raise PackError('each control is an object')
        unknown = set(c) - {'id', 'name', 'action', 'encoder', 'word', 'kind', 'steps', 'default', 'random', 'detents', 'wrap',
                            'fine', 'bit', 'step', 'fine_step'}
        if unknown:
            raise PackError(f'control {c.get("id")!r}: unknown fields {sorted(unknown)}')
        cid = c.get('id')
        if not isinstance(cid, str) or not cid or len(cid) > 15 or not all(ch.isdigit() or 'a' <= ch <= 'z' or ch == '_' for ch in cid):
            raise PackError(f'control id {cid!r}: 1 to 15 of a-z, 0-9, _')
        if c.get('action') not in CONTROL_ACTIONS:
            raise PackError(f'control {cid}: action must be one of {sorted(CONTROL_ACTIONS)} (turn and shift_turn are the stock knobs 0..3)')
        if c.get('encoder') not in CONTROL_ENCODERS:
            raise PackError(f'control {cid}: encoder must be one of {sorted(CONTROL_ENCODERS)}')
        ckind = c.get('kind')
        if ckind == 'latch':                           # coop e6: momentary state, never saved: a bit of params[8]
            if not fx:
                raise PackError(f'control {cid}: a latch is for effects only')
            if not c['action'].endswith('click'):
                raise PackError(f'control {cid}: a latch needs a click action')
            if 'word' in c:
                raise PackError(f'control {cid}: a latch has a bit (0..15 of params[8]), not a word')
            word = c.get('bit')
            if type(word) is not int or not 0 <= word <= 15:
                raise PackError(f'control {cid}: bit must be 0..15 (params[8], the latch word)')
            if word in bits:
                raise PackError(f'control {cid}: bit {word} is used twice')
            bits.add(word)
        else:
            if 'bit' in c:
                raise PackError(f'control {cid}: bit is for a latch')
            word = c.get('word')
            if word not in (5, 6, 7):
                raise PackError(f'control {cid}: word must be 5, 6 or 7 (the free knob words; word 4 is the selector the core owns)')
            if word in words:
                raise PackError(f'control {cid}: word {word} is used twice')
            words.add(word)
        slot = (c['action'], c['encoder'])
        if slot in slots:
            raise PackError(f'control {cid}: {c["action"]} on {c["encoder"]} is declared twice')
        slots.add(slot)
        name = label(c.get('name', cid), f'control {cid}: name')
        random = c.get('random', True)
        detents = c.get('detents', 1)
        if not isinstance(random, bool):
            raise PackError(f'control {cid}: random must be true or false')
        if type(detents) is not int or not 1 <= detents <= 8:
            raise PackError(f'control {cid}: detents must be 1..8')
        if ckind == 'steps':
            steps = c.get('steps')
            if not isinstance(steps, list) or not 2 <= len(steps) <= 16:
                raise PackError(f'control {cid}: steps must list 2 to 16 step names')
            names = [label(st, f'control {cid}: step') for st in steps]
            default = c.get('default', 0)
            if type(default) is not int or not 0 <= default < len(names):
                raise PackError(f'control {cid}: default must be a step index 0..{len(names) - 1}')
            count, tail = len(names), ','.join(names)
        elif ckind == 'continuous':
            if c['action'].endswith('click'):
                raise PackError(f'control {cid}: a click needs a steps control')
            if 'steps' in c:
                raise PackError(f'control {cid}: steps only for kind "steps"')
            default = c.get('default', 0)
            if type(default) is not int or not 0 <= default <= 32767:
                raise PackError(f'control {cid}: default must be 0..32767 (Q15)')
            count, tail = 0, ''
        elif ckind == 'latch':
            if 'steps' in c or 'default' in c or 'detents' in c:
                raise PackError(f'control {cid}: a latch has no steps, default or detents (it starts off)')
            if c.get('random', False):
                raise PackError(f'control {cid}: a latch is never randomised')
            random, count, default, tail = False, 0, 0, ''
        else:
            raise PackError(f'control {cid}: kind must be "steps", "continuous" or "latch"')
        wrap = c.get('wrap', False)                    # coop e5: a turn past the last step goes to the first (engine_ui.cc)
        if not isinstance(wrap, bool):
            raise PackError(f'control {cid}: wrap must be true or false')
        if wrap and ckind != 'steps':
            raise PackError(f'control {cid}: wrap is for a steps control')
        fine = c.get('fine', False)                    # coop e6: shift + fn + turn on this encoder is this control, 1/16 the step
        if not isinstance(fine, bool):
            raise PackError(f'control {cid}: fine must be true or false')
        if fine and (ckind != 'continuous' or c['action'] != 'fn_turn'):
            raise PackError(f'control {cid}: fine is for a continuous fn_turn control')
        step, fine_step = c.get('step'), c.get('fine_step')   # coop e6: Q15 a detent (default 328) and its fine (default step/16)
        if step is not None and (ckind != 'continuous' or type(step) is not int or not 1 <= step <= 32767):
            raise PackError(f'control {cid}: step is 1..32767 (Q15 a detent) on a continuous control')
        if fine_step is not None:
            if not fine:
                raise PackError(f'control {cid}: fine_step needs "fine": true')
            if type(fine_step) is not int or not 1 <= fine_step <= (step if step is not None else 328):
                raise PackError(f'control {cid}: fine_step is 1..step (Q15 a detent)')
        if fine:
            fines.add(c['encoder'])
        if c['action'] == 'shift_fn_turn':
            deep.add(c['encoder'])
        fields = [CONTROL_ACTIONS[c['action']], CONTROL_ENCODERS[c['encoder']], word, ckind[0], count, default,
                  int(random), detents, name, tail]
        if wrap:
            fields.append(1)                           # an 11th field: a core before e5 skips it and clamps
        if fine or step is not None:
            fields.append(2 if fine else 0)            # e6: the same 11th field (flags)
        if step is not None or fine_step is not None:
            fields += [step or 0, fine_step or 0]      # e6: 12th / 13th fields, 0 = the default
        lines.append('\t'.join(str(f) for f in fields))
    clash = fines & deep
    if clash:
        raise PackError(f'shift_fn_turn on {sorted(clash)[0]} is the fine of that encoder\'s fn_turn: declare one or the other')
    return ('\n'.join(lines) + '\n').encode('ascii') + b'\0'

COOP_FX_MAGIC = 0x58464F43                             # "COFX" (sdk/coop_plugin.h)
COOP_FX_F_SCREEN = 0x1                                  # e9: coop_fx_t.screen is present
COOP_FX_FLAGS = COOP_FX_F_SCREEN                        # every coop_fx_t flag the e9 core knows
FX_SCREEN_FIELD = 20                                    # offsetof(coop_fx_t, screen): magic api flags init render screen
SVG_FX_MAX = 16384                                      # e9: an effect screen's SVG at most (guidelines §5.3: about 8 KiB of art)


def fx_screen_check(img, image_size, entry, entry_size, screen, screen_off, relocs, have_svg):
    """coop e9: an effect's coop_fx_t.flags must be known, COOP_FX_F_SCREEN set exactly when plugin.json declares
    "screen", and then coop_fx_t.screen must point at the screen symbol (a core before e9 never reads either: the
    effect plays there with the stock page). Kennel-layout entry objects (no COFX magic) are left alone."""
    if entry + 12 > image_size or struct.unpack_from('<I', img, entry)[0] != COOP_FX_MAGIC:
        if screen:
            raise PackError('an effect screen needs a coop_fx_t entry object (COFX)')
        return
    flags = struct.unpack_from('<I', img, entry + 8)[0]
    if flags & ~COOP_FX_FLAGS:
        raise PackError(f'coop_fx_t.flags {flags:#x}: bits this core does not know')
    if bool(flags & COOP_FX_F_SCREEN) != bool(screen):
        raise PackError('plugin.json "screen" on an effect and COOP_FX_F_SCREEN in coop_fx_t.flags go together: set both '
                        '(or neither)')
    if not screen:
        return
    if entry_size < FX_SCREEN_FIELD + 4 or entry + FX_SCREEN_FIELD + 4 > image_size:
        raise PackError('effect entry object truncated: coop_fx_t.screen missing (rebuild with the e9 sdk)')
    target = [r for r in relocs if r[0] == entry + FX_SCREEN_FIELD and r[1] == F.R_ABS32]
    if len(target) != 1 or target[0][2] != F.SYM_LOCAL or target[0][3] != screen_off:
        raise PackError(f'coop_fx_t.screen must point at the "screen" symbol {screen["symbol"]}')
    if not have_svg:
        raise PackError('an effect screen needs its SVG ("screen": {"svg": ...})')


def pack(meta, obj_bytes, svg_dir=None):
    """plugin.json dict + `ld -r` output -> (plugin file bytes, info dict for reports and tests)."""
    elf = Elf(obj_bytes)
    secs, layout, align = section_layout(elf)
    image_size = layout['.data'][0] + layout['.data'][1]
    img = bytearray(image_size)
    sec_base = {}
    for name, i in secs.items():
        sec_base[i] = layout[name][0]
        if name != '.bss':
            off = layout[name][0]
            img[off:off + elf.sh[i]['size']] = elf.contents(elf.sh[i])

    imports, relocs, types = [], [], {}
    for rs in elf.sh:
        if rs['type'] != SHT_RELA or not elf.sh[rs['info']]['flags'] & SHF_ALLOC:
            continue
        target = rs['info']
        for r_offset, r_info, addend in elf.relas(rs):
            etype, sym = r_info & 0xFF, elf.syms[r_info >> 8]
            where = f'{elf.sh[target]["name"]}+{r_offset:#x} ({sym["name"] or "section"})'
            if etype not in ELF_TO_FILE:
                raise PackError(f'{where}: relocation type {etype:#x} not supported')
            if target not in sec_base or elf.sh[target]['name'] == '.bss':
                raise PackError(f'{where}: relocation in an unsupported section')
            types[etype] = types.get(etype, 0) + 1
            ftype = ELF_TO_FILE[etype]
            off = sec_base[target] + r_offset
            if sym['shndx'] == SHN_UNDEF:
                if not sym['name']:
                    raise PackError(f'{where}: undefined symbol without a name')
                if sym['name'] not in imports:
                    imports.append(sym['name'])
                relocs.append((off, ftype, imports.index(sym['name']), addend))
                continue
            if sym['shndx'] == SHN_ABS:
                value, local = sym['value'] + addend, False
            elif sym['shndx'] in sec_base:
                value, local = sec_base[sym['shndx']] + sym['value'] + addend, True
            else:
                raise PackError(f'{where}: symbol in section {sym["shndx"]} not supported')
            value &= 0xFFFFFFFF
            if ftype == F.R_PCREL24:
                if not local:
                    raise PackError(f'{where}: PC-relative reference to an absolute address')
                pcrel24(img, off, value - (off - 2), where)
                continue
            if ftype == F.R_ABS32:
                struct.pack_into('<I', img, off, value)
            else:
                struct.pack_into('<H', img, off, value >> 16 if ftype == F.R_HI16 else value & 0xFFFF)
            if local:
                relocs.append((off, ftype, F.SYM_LOCAL, value))
    relocs.sort()

    def symbol(name):
        for s in elf.syms:
            if s['name'] == '_' + name and s['shndx'] in sec_base and s['bind'] == 1:
                return sec_base[s['shndx']] + s['value']
        raise PackError(f'global symbol {name} not found')

    kind = F.KINDS[meta['kind']]
    entry = symbol(meta['symbol'])
    screen = meta.get('screen')
    screen_off = symbol(screen['symbol']) if screen else F.NONE
    svg = (Path(svg_dir) / screen['svg']).read_bytes() if screen and svg_dir else b''
    name = check_id(meta).encode('ascii')                   # the saved name: derived, never written by hand
    order = meta.get('order', 0)
    if not 0 <= order <= 31:
        raise PackError(f'order {order} outside 0..31')
    defaults = list(meta.get('defaults', [0, 0, 0, 0]))
    page2 = meta.get('page2_defaults')
    if kind == F.KIND_FX and meta.get('controls') is not None:
        # coop e6: an effect's controls set the page-2 flag (a core before e6 refuses the file) and its defaults are the
        # declared defaults of words 5..7 (0 where none). Never written by hand next to "controls".
        if page2 is not None:
            raise PackError('page2_defaults: an effect with "controls" takes them from the controls')
        page2 = [0, 0, 0]
        for c in meta['controls']:
            if isinstance(c, dict) and c.get('kind') in ('steps', 'continuous') and c.get('word') in (5, 6, 7):
                page2[c['word'] - 5] = c.get('default', 0)
    if 'stereo' in meta and not isinstance(meta['stereo'], bool):
        raise PackError('stereo must be a boolean')
    if meta.get('stereo') and kind != F.KIND_FX:
        raise PackError('stereo flag requires FX kind')
    if 'input_engine' in meta and not isinstance(meta['input_engine'], bool):
        raise PackError('input_engine must be a boolean')
    if meta.get('input_engine') and (kind != F.KIND_SYNTH or meta.get('uses_sample')):
        raise PackError('input flag requires nonsampler synth')
    coarse = meta.get('coarse', [])
    if not isinstance(coarse, list) or any(type(k) is not int or not 0 <= k <= 3 for k in coarse) \
            or len(set(coarse)) != len(coarse):
        raise PackError('coarse must be a list of distinct knob numbers 0..3')
    if coarse and kind != F.KIND_SYNTH:
        raise PackError('coarse knobs require synth kind')
    cost = meta.get('cost')                             # declared worst-block cost (harness/slot_budget.py)
    cost_keys = ('voice', 'start') if kind == F.KIND_SYNTH else ('call', 'init')
    if cost is not None:
        if kind not in (F.KIND_SYNTH, F.KIND_FX) or not isinstance(cost, dict) or set(cost) - set(cost_keys) \
                or any(type(v) is not int or not 0 <= v < 1 << 22 for v in cost.values()):
            raise PackError(f'cost must be {{{cost_keys[0]!r}, {cost_keys[1]!r}}}: bcore packets 0..4194303 (synth or FX)')
    entry_symbol = next(s for s in elf.syms if s['name'] == '_' + meta['symbol'] and s['bind'] == 1)
    if meta.get('stereo') and (entry_symbol['size'] < 16 or entry + 16 > image_size):
        raise PackError('stereo entry object truncated')
    if meta.get('input_engine') and (entry_symbol['size'] < 32 or entry + 32 > image_size):
        raise PackError('input entry object truncated')
    voice_rate = meta.get('voice_rate', 88200)          # coop v1: the engine's render rate (docs/engines.md "The 44.1 kHz voice")
    if 'voice_rate' in meta and kind != F.KIND_SYNTH:
        raise PackError('voice_rate is for synth engines only')
    if type(voice_rate) is not int or voice_rate not in (88200, 44100):
        raise PackError('voice_rate must be 88200 (128 samples a render) or 44100 (64)')
    if kind == F.KIND_SYNTH:                            # coop_engine_t.flags (sdk/coop_plugin.h COOP_ENGINE_F_44K) must agree
        if entry_symbol['size'] < 12 or entry + 12 > image_size:
            raise PackError('engine entry object truncated')
        eflags = struct.unpack_from('<I', img, entry + 8)[0]
        if bool(eflags & 1) != (voice_rate == 44100):
            raise PackError(f'voice_rate {voice_rate} but the engine object\'s flags say '
                            f'{"COOP_ENGINE_F_44K" if eflags & 1 else "0 (88.2 kHz)"}: set both')
    if kind == F.KIND_FX:                               # coop e9: coop_fx_t.flags and the effect screen (sdk/coop_plugin.h)
        fx_screen_check(img, image_size, entry, entry_symbol['size'], screen, screen_off, relocs, bool(svg) or svg_dir is None)
    bigbuf = meta.get('bigbuf')                         # a declared pool buffer: OP1_BIGBUF(<symbol>, min, max) in .data
    bigbuf_off = None
    if bigbuf is not None:
        if not isinstance(bigbuf, str):
            raise PackError('bigbuf must be the name of an OP1_BIGBUF symbol')
        bigbuf_off = symbol(bigbuf)
        if bigbuf_off % 4 or not layout['.data'][0] <= bigbuf_off <= layout['.data'][0] + layout['.data'][1] - F.BIGBUF.size:
            raise PackError('bigbuf symbol must be an initialised op1_bigbuf_t in .data')
        magic, version, mn, mx, ptr, size = F.BIGBUF.unpack_from(img, bigbuf_off)
        if (magic, version) != (F.BIGBUF_MAGIC, F.BIGBUF_VERSION) or mn > mx or not mx or mn > F.BIGBUF_MAX or ptr or size:
            raise PackError(f'bigbuf {bigbuf}: not a valid OP1_BIGBUF (min <= max, min <= {F.BIGBUF_MAX}, max > 0)')
    scratch = scratch_bytes(meta)                       # coop e8: the scratch flag (a core before e8 refuses the file)
    if '_coop_fx_scratch' in imports and not scratch:
        raise PackError('the effect calls coop_fx_scratch: declare "scratch" (its bytes) in plugin.json')
    flags = ((F.FLAG_SCRATCH if scratch else 0) |
             (F.FLAG_BIGBUF if bigbuf is not None else 0) | (F.FLAG_USES_SAMPLE if meta.get('uses_sample') else 0) |
             (F.FLAG_PAGE2 if page2 else 0) | (F.FLAG_STEREO if meta.get('stereo') else 0) | (F.FLAG_INPUT if meta.get('input_engine') else 0) |
             sum(1 << (F.FLAG_COARSE_SHIFT + k) for k in coarse))
    if len(defaults) != 4 or (page2 and len(page2) != 3):
        raise PackError('defaults needs 4 values, page2_defaults 3')

    strings = bytearray(b'\0')
    def string(s):
        off = len(strings)
        strings.extend(s.encode('ascii') + b'\0')
        return off
    import_names = [string(n) for n in imports]

    pad4 = lambda b: bytes(b) + bytes(-len(b) % 4)
    if b'\0' in svg or (svg and not svg.lstrip().startswith(b'<')):
        raise PackError('screen SVG must be text starting with <')
    if kind == F.KIND_FX and len(svg) > SVG_FX_MAX:     # e9: every byte is parsed into the stock TE heap once
        raise PackError(f'effect screen SVG {len(svg)} bytes: at most {SVG_FX_MAX} (guide/plugin-guidelines.md §5.3)')
    long_num = re.search(rb'\d\.\d{5,}', svg)          # R246's SVG parser panics at boot on these (kn_boot.c svg_ok)
    if long_num:
        raise PackError(f'screen SVG number {long_num.group().decode()} has more than 4 decimals; round it')
    bad_cmd = re.search(rb'\sd="[^"]*?([A-BD-GI-KN-RT-UW-Yabd-gi-kn-rt-uw-y])', svg)   # Parser.cpp:525 (kn_boot.c svg_err)
    if bad_cmd:
        raise PackError(f'screen SVG path command {bad_cmd.group(1).decode()} is not in R246\'s parser (only M L H V C S Z); '
                        'write it as C curves')
    turns = svg_turn_backs(svg) if svg else []           # R246's stroke join draws these as a spike to the screen edge
    if turns:
        raise PackError(f'screen SVG: {turns[0]} (R246 draws a spike there; drop the point)'
                        + (f', and {len(turns) - 1} more' if len(turns) > 1 else ''))
    prefix = pad4(svg + b'\0') if svg else b''          # the stock screen loader reads the file as the SVG
    parts, off = [], len(prefix) + F.HEADER_SIZE
    def place(blob):
        nonlocal off
        start = off
        parts.append(pad4(blob))
        off += len(parts[-1])
        return start
    image_file_offset = place(img)
    reloc_offset = place(b''.join(F.RELOC.pack(o, t, 0, s, a) for o, t, s, a in relocs))
    import_offset = place(b''.join(F.IMPORT.pack(n) for n in import_names))
    strings_offset = place(strings)
    ext = b''                                            # extension table: claims, author, version (tag, len, bytes)
    for tag, val in ((F.EXT_CLAIMS, b''.join(c.encode('ascii') + b'\0' for c in meta.get('claims', [])) + b'\0'),
                     (F.EXT_AUTHOR, meta.get('author', '').encode('ascii') + b'\0'),
                     (F.EXT_VERSION, str(meta.get('version', '')).encode('ascii') + b'\0')):
        if len(val) > 1:
            ext += struct.pack('<HH', tag, len(val)) + val + bytes(-len(val) % 4)
    if bigbuf_off is not None:
        ext += struct.pack('<HHI', F.EXT_BIGBUF, 4, bigbuf_off)
    if cost:
        ext += struct.pack('<HHII', F.EXT_COST, 8, cost.get(cost_keys[0], 0), cost.get(cost_keys[1], 0))
    if scratch:                                          # coop e8
        ext += struct.pack('<HHI', F.EXT_SCRATCH, 4, scratch)
    controls = control_lines(meta, kind)                 # coop e4
    if controls:
        ext += struct.pack('<HH', F.EXT_CONTROLS, len(controls)) + controls + bytes(-len(controls) % 4)
    ext_offset = place(ext + struct.pack('<HH', 0, 0)) if ext else 0
    header = F.HEADER.pack(
        F.MAGIC, F.FORMAT_VERSION, F.HEADER_SIZE, off, 0, api_version(), kind, order, name, flags,
        *defaults, *(list(page2) + [0] if page2 else [0, 0, 0, 0]),
        layout['.text'][1], *layout['.rodata'], *layout['.data'], *layout['.bss'], image_size, align,
        entry, screen_off, image_file_offset, reloc_offset, len(relocs), import_offset, len(imports),
        strings_offset, len(strings), 0, len(svg) + 1 if svg else 0, F.NONE, ext_offset)
    data = bytearray(prefix + header + b''.join(parts))
    struct.pack_into('<I', data, len(prefix) + 12, F.crc32(bytes(data), len(prefix) + 12))
    info = {'layout': layout, 'imports': imports, 'elf_reloc_types': types, 'relocs': len(relocs),
            'entry': entry, 'screen': screen_off, 'scratch': scratch, 'svg': len(svg)}
    return bytes(data), info


# Names (docs/plugin-format.md "Names"): a plugin's id is its folder, file (<id>.knl), plugin.json "name", kennel.off
# entry, inventory name and feature name; the saved name (the header's type name: what the OP-1 shows and saved sounds
# store) is derived from it. SAVED_NAMES is frozen: names already in users' saved files that break the rule.
ID = re.compile(r'[a-z0-9_-]{1,15}')
SAVED_NAMES = {'drumplus': 'drum+'}                  # drum patch JSON "engine": "drum+", the drum_types row


def check_id(meta, folder=None):
    """refuse a plugin.json whose id breaks the rule (or differs from its folder); -> its saved name"""
    pid = meta.get('name', '')
    if not ID.fullmatch(pid):
        raise PackError(f'plugin id {pid!r}: only a-z 0-9 _ -, 1..15 characters (docs/plugin-format.md "Names")')
    if folder is not None and folder != pid:
        raise PackError(f'plugin folder {folder!r} differs from its id (plugin.json "name") {pid!r}')
    if 'saved_name' in meta and SAVED_NAMES.get(pid) != meta['saved_name']:
        raise PackError(f'{pid}: "saved_name" only for the frozen exceptions {SAVED_NAMES}')
    return SAVED_NAMES[pid] if 'saved_name' in meta else pid.replace('_', ' ')


FILE_NAME = re.compile(r'[A-Za-z0-9_.-]{1,35}\.(knl|op1p)')


def file_name_ok(name):
    """a plugin file name the SysEx update can send, list and delete (devupd du_path_ok: [A-Za-z0-9_.-]) and the loader
    lists (under 40 characters)"""
    return bool(FILE_NAME.fullmatch(name)) and '..' not in name


def pack_dir(plugin_dir, out, toolchain=None, keep=None):
    """Compile, link and pack one plugin folder; keep = directory to leave the objects in."""
    plugin_dir = Path(plugin_dir)
    meta = json.loads((plugin_dir / 'plugin.json').read_text())
    check_id(meta, plugin_dir.name)
    if Path(out).name != meta['name'] + '.coop':
        raise PackError(f'plugin file name {Path(out).name!r}: must be <id>.coop ({meta["name"]}.coop)')
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(keep or tmp)
        work.mkdir(parents=True, exist_ok=True)
        objs = compile_plugin(plugin_dir, work, toolchain)
        ro = link_relocatable(objs, work / (meta['name'] + '.ro'), toolchain)
        data, info = pack(meta, Path(ro).read_bytes(), plugin_dir)
    Path(out).write_bytes(data)
    return data, info


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('plugin_dir')
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--toolchain', help='OP1-Emulator toolchain/ (bfin-gcc/bin, bfin/bin)')
    ap.add_argument('--keep', help='keep objects and the ld -r output in this directory')
    a = ap.parse_args()
    try:
        data, info = pack_dir(a.plugin_dir, a.output, a.toolchain, a.keep)
    except PackError as e:
        sys.exit(f'error: {e}')
    names = {v: k for k, v in globals().items() if k.startswith('R_BFIN_')}
    print(f'{a.output}: {len(data)} bytes, sections '
          + ', '.join(f'{k} {v[1]}' for k, v in info['layout'].items())
          + f'; {info["relocs"]} load-time relocations; ELF relocations '
          + ', '.join(f'{names.get(t, hex(t))} {n}' for t, n in sorted(info['elf_reloc_types'].items()))
          + f'; imports {", ".join(info["imports"]) or "none"}'
          + (f'; scratch {info["scratch"]} (one relocation, the position buffers)' if info.get('scratch') else '')
          + (f'; screen SVG {info["svg"]} bytes' if info.get('svg') else ''))


if __name__ == '__main__':
    main()
