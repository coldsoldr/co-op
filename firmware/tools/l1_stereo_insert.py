#!/usr/bin/env python3
"""Wire the coop stereo bus (Phase 2 milestone C1) into a plaintext OP-1 LDR: three L1 trampolines in
free L1 and three small rewrites of the stock L1 code, each checked against the stock bytes first.

  l1_stereo_insert.py IN.ldr OUT.ldr --elf build/core/coop_combined.elf [--tramp-at 0xffa0bfb4]

What it changes (docs/stereo-bus.md "C1 takeover mechanism"; all addresses [C] from the decrypted
R246 L1 and the op1emu write tracer):

  synth render call 0xffa00d22..0xffa00d2a
      stock:  P2 = [P0+0x14]; R3 = W[P0+0x10] (X); R0 = R0 + R3; CALL (P2)
      coop:   R3 = W[P0+0x10] (X); R0 = R0 + R3; CALL tramp_synth
      tramp_synth (12 B):  P2 = [P0+0x14]; P1 = coop_synth_render_thunk; JUMP (P1)
  drum render call 0xffa008fc..0xffa00906
      stock:  P0 = [P1+0x24]; R1 = W[P1+0x20] (X); R0 = R0 + R1; CALL (P0)
      coop:   R1 = W[P1+0x20] (X); R0 = R0 + R1; CALL tramp_drum
      tramp_drum (12 B):   P0 = [P1+0x24]; P2 = coop_drum_render_thunk; JUMP (P2)
  mix loop B 0xffa00fae..0xffa00fb4
      stock:  LSETUP(0xffa00fb2, 0xffa00fda) LC0 = P0; <loop body ...>
      coop:   CALL tramp_mix; JUMP.S 0xffa00fdc          (the stock body stays as dead bytes)
      tramp_mix (16 B):    [--SP] = RETS; P0 = coop_mix_thunk; CALL (P0); RETS = [SP++]; RTS
  synth note-on dispatch 0xffa00e2e..0xffa00e36 (C2: the voice loop calls *(vtable+0x1c) for a voice
  with a pending note; R7 = voice index)
      stock:  P0 = [P1+0x1c]; R2 = W[P1+0x18] (X); R0 = R0 + R2; CALL (P0)
      coop:   R2 = W[P1+0x18] (X); R0 = R0 + R2; CALL tramp_noteon
      tramp_noteon (12 B): P0 = [P1+0x1c]; P2 = coop_synth_noteon_thunk; JUMP (P2)

The SDRAM thunk addresses come from the ELF's symbol table (inject_app.read_elf_symbols), so this runs
after inject_app added the payload and l1_decrypt decrypt-ldr produced the plaintext image. For the
device, encrypt-ldr afterwards. Stdlib only; no firmware bytes are stored here.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ldr_dump                                            # noqa: E402
from inject_app import call24, plain_block, read_elf_symbols, symbol_address   # noqa: E402
from l1_insert import build_tramp   # noqa: E402  ([--SP]=RETS; P0=fn; CALL (P0); RETS=[SP++]; RTS)

TRAMP_BASE = 0xffa0bfb4            # kennel's free .l1tramp range, up to 0xffa0c000
TRAMP_END = 0xffa0c000

# --- stock bytes at the sites (little-endian halfwords as objdump shows them) ---
SYNTH_SITE = 0xffa00d22
SYNTH_STOCK = bytes.fromhex('42ad' '03aa' '1850' '6200')          # P2=[P0+0x14]; R3=W[P0+0x10](X); R0=R0+R3; CALL (P2)
SYNTH_CALL_AT = 0xffa00d26                                        # the CALL lands after the two kept insns
DRUM_SITE = 0xffa008fc
DRUM_STOCK = bytes.fromhex('48ae' '49e51000' '0850' '6000')       # P0=[P1+0x24]; R1=W[P1+0x20](X); R0=R0+R1; CALL (P0)
DRUM_CALL_AT = 0xffa00902
MIX_SITE = 0xffa00fae
MIX_STOCK = bytes.fromhex('a2e01600' '03c8')                      # LSETUP(0xffa00fb2,0xffa00fda) LC0=P0; first body halfword
MIX_RESUME = 0xffa00fdc                                           # first instruction after loop B
# mix loop A (0xffa00f2c): the instrument monitor on the tape page / while the tape runs; the same trampoline
# (the thunk tells the sites apart by the pushed RETS) -> coop_mix_entry runs the loop A replica with dL/dR.
MIXA_SITE = 0xffa00f2c
MIXA_STOCK = bytes.fromhex('a2e02d10' '03c8')                     # LSETUP(0xffa00f30,0xffa00f86) LC0=P1; first body halfword
MIXA_RESUME = 0xffa00f88
# S3 (stereo bus v2, docs/stereo-bus-v2.md): the stock decimator + DC block call pairs, 14 bytes each:
#   CALL 0xffa005d4; P1.L = 0x74f8; P1.H = 0x0196; CALL (P1)
# become an inline far call to coop_instr_thunk (no L1 trampoline: the tape build fills the free L1 range):
#   [--SP] = RETS; P0.L = lo; P0.H = hi; CALL (P0); RETS = [SP++]
# The code after each site reloads P0 / P1 / P2 from its frame (and M1, which the thunk preserves).
DECIM_FN = 0xffa005d4
DC_TAIL = bytes.fromhex('09e1f874' '49e19601' '6100')            # P1.L = 0x74f8; P1.H = 0x196; CALL (P1)
INSTR_SITES = (0xffa01eb4, 0xffa02456, 0xffa02888)
# The drum path's bus stage (0x01a66922: an in-place 128-sample dynamics / level stage on the summed bus, between the
# voices and the decimator): `P1.L = 0x6922; P1.H = 0x01a6` before `CALL (P1)` at 0xffa02364 -> coop_drumbus_thunk,
# which runs it on each side of the pair.
DRUMBUS_SITE = 0xffa0235c
DRUMBUS_FN = 0x01a66922
# synth note-on dispatch inside the voice loop (reached from 0xffa00aa6 for a voice with a pending note):
#   P0 = [P1+0x1c]; R2 = W[P1+0x18] (X); R0 = R0 + R2; CALL (P0)   ->  R2 = ...; R0 = ...; CALL tramp_noteon
NOTEON_SITE = 0xffa00e2e
NOTEON_STOCK = bytes.fromhex('c8ad' '0aab' '1050' '6000')
NOTEON_CALL_AT = 0xffa00e32
# Core::NoteOn (SDRAM 0x01967854, R0 = voice) is the path a played key takes; its three VoiceCtl callers
# are CALL24s. Region A is beyond pcrel24 reach (+-16 MB) from 0x01a8xxxx, so they call a 16-byte
# trampoline in free region C (0x01e00000, kennel's core region; nothing in the stock LDR loads
# 0x01e00000..0x01ffffff) which reaches coop_core_noteon_thunk in region A.
CORE_NOTEON = 0x01967854
CORE_NOTEON_CALLERS = (0x01a8442c, 0x01a848e4, 0x01a8564e)
SDRAM_TRAMP_AT = 0x01e00000

# --- stereo tape, milestone T1 (docs/tape-stereo.md section 4; --tape-stereo) ---
# The stock L1 record function and the tape tick stay; the tick runs its 4-track mix over 8 lanes with a
# coop gain table, and the record CALL sites go through coop_tape_rec_thunk (two stock passes, L/R lanes).
TAPE_REC_SITES = (0xffa0217a, 0xffa02dca)                         # CALL 0xffa06334 (synth path, drum path)
TAPE_REC_FN = 0xffa06334
TAPE_GAINS_SITE = 0xffa06de0                                      # P1.L = 0x17f0; P1.H = 0x18c (after the pan law)
TAPE_GAINS_STOCK = bytes.fromhex("09e1f017" "49e18c01")
TAPE_IO_STEP = 0x01a54710                                          # tape_cache_io_step
TAPE_IO_CALLERS = (0x019f0a24, 0x019f0be2)                           # tape_c::service(0) in the main loop, service(1) at shutdown
TAPE_MOUNT = 0x01996f38                                              # tape_image_mount_and_index
TAPE_MOUNT_CALLERS = (0x0199758e, 0x019f0354)
TAPE_LIFT = 0x01a5485c                                               # tape_clips lift (file, mono) -> coop_lift (tramp +48)
TAPE_LIFT_CALLERS = (0x0191b89c, 0x01a549a0)                         # clipboard service, lift-all
TAPE_DROP = 0x01a549b8                                               # -> coop_drop (tramp +64)
TAPE_DROP_CALLERS = (0x0191b67e, 0x01a54aee)                         # clipboard service, drop-all
TAPE_ERASE = 0x01a54b08                                              # fast_tape_erase -> coop_erase (tramp +80)
TAPE_ERASE_CALLERS = (0x0191b9f0,)
# --- SysEx file channel, milestone U1 (core/sdram/coop_sysex.cc) ---
# PlayerManager::tick -> midi_poll: CALL midi_rx_parse (kennel du_fw.c); retargeted at a jump trampoline (+96) to
# coop_sysex_rx_poll, which runs the stock parser itself. Only when the ELF has the symbol.
MIDI_RX_SITE = 0x019e3cc8
MIDI_RX_PARSE = 0x019e31a4
# --- the stock sine table in L1 (milestone T4c, docs/performance.md) ---
# OP1_SINE_TABLE (8192 x int16 at 0x0101E5E8, SDRAM) is read per sample by the FM and other engines; the device
# pays a cache miss on nearly every lookup. coop copies it to L1 data B (l1_pool.cc, coop_l1_sine_copy) and every
# `Px.L = 0xe5e8; Px.H = 0x0101` pair in the loaded code (27: 26 in SDRAM, one in L1) is retargeted at the copy.
SINE_TABLE = 0x0101e5e8
SINE_L1 = 0xff902900
SINE_REFS = 27
# --- the Lander game removed (milestone T4d, core/sdram/lander_off.cc, kennel's recipe) ---
LANDER_GETTER = 0x019c4ecc                                        # the Lander screen instance getter
LANDER_SITES = (0x01a3bdb0, 0x019c35f0, 0x019c369c)                # screen list, LanderController key_down / key_up
UI_FRAME_SITE = 0x019b2690                                         # gfx_render_frame: CALL screens_update -> coop_ui_frame (P3a)
UI_FRAME_FN = 0x01a3bb20
UI_KNOB_SITE = 0x019c2e94                                          # KeyHandler encoder step: CALL 0x019b6c0c -> coop_ui_knob
UI_KNOB_FN = 0x019b6c0c
KEYEV_SITE = 0x019c2d3e                                           # KeyHandler dispatch: CALL key_dispatch (code, pressed)
KEY_DISPATCH = 0x019b6c74
# --- FX plugins at the stock positions (milestone P2, core/sdram/plugin_host.cc): kennel's filter-type hijack ---
# The stock filter FX object's vtables: mono (synth / drum positions) at 0x01910C94 and the stereo master wrapper at
# 0x01910CB4, entry 2 = render, entry 3 = init (kennel make_firmware.py). The fx_types name compare at 0x019AAD8E
# (`R3 = R0; P1 = R3`) becomes a JUMP.L to a region-C jump trampoline -> coop_fx_name_eq.
# Stereo bus v2 (S1, docs/stereo-bus-v2.md): every stock mono FX type's vtable (entries 8 bytes apart: render at +0x14,
# init at +0x1c) dispatches to coop, which runs the type's stereo object on the instrument's stereo pair; the stereo
# master filter's entries carry the plugin path at the master position. (mono vtable, render, init) in the stock
# name-table order delay, grid, punch, phone, spring, filter, nitro, cwo (RE 2026-11-03).
FX_MONO_CLASSES = [
    (0x0190FB6C, 0x0197D8C4, 0x0197D7E8), (0x01910204, 0x019AF338, 0x019AF1B6), (0x01910C1C, 0x019E6260, 0x019E6160),
    (0x019112F4, 0x01A1BB74, 0x01A1C03E), (0x019113BC, 0x01A326E8, 0x01A32E9A), (0x01910C94, 0x019E7934, 0x019E780A),
    (0x01910014, 0x019A62A4, 0x019A61BE), (0x0190F574, 0x0194480C, 0x019436FE)]
FX_VTABLE_PATCHES = [(vt + 0x14, render, 'fx_render_mono') for vt, render, init in FX_MONO_CLASSES] + \
                    [(vt + 0x1c, init, 'fx_init_mono') for vt, render, init in FX_MONO_CLASSES] + \
                    [(0x01910CC8, 0x019E7EE4, 'fx_render_st'), (0x01910CD0, 0x019E78D0, 'fx_init_st')]
FXNAME_SITE = 0x019aad8e
FXNAME_STOCK = bytes.fromhex('18300b32')
SEAM_SITE = 0xffa0706e                                               # P2.L = 0xef7c; P2.H = 0x1b4 before the seam loop (hook B)
SEAM_STOCK = bytes.fromhex("0ae17cef" "4ae1b401")
SEAM_FILL_SITE = 0xffa073b8                                          # P1 = 0x01a54284 (the stock seam fill) -> coop_seam_fill
SEAM_FILL_STOCK = bytes.fromhex("09e18442" "49e1a501")
TAPE_BYTE_PATCHES = [                                             # (address, stock, new): lanes 4 -> 8, gR at +0x10
    (0xffa0705a, bytes.fromhex("2468"), bytes.fromhex("4468")),   # seam loop LC1: P4 = 4 -> 8
    (0xffa07106, bytes.fromhex("2268"), bytes.fromhex("4268")),   # main mix LC1: P2 = 4 -> 8
    (0xffa07138, bytes.fromhex("00a9"), bytes.fromhex("00aa")),   # R0 = W[P0 + 0x8] (X) -> W[P0 + 0x10]  (gR, main mix)
    (0xffa070a6, bytes.fromhex("0aa9"), bytes.fromhex("0aaa")),   # R2 = W[P1 + 0x8] (X) -> W[P1 + 0x10]  (gR, seam mix)
    (0x01a54172, bytes.fromhex("904a"), bytes.fromhex("984a")),   # tape_cache_init: BITSET (R0, 18) -> 19: node data 256 -> 512 KiB
]


def imm_pair_bytes(preg, value):
    """`Px.L = lo; Px.H = hi` for P0..P5 (LDIMMhalf: e108 + reg for .L, e148 + reg for .H)."""
    lo = 0xe108 | preg
    hi = 0xe148 | preg
    return struct.pack('<HHHH', lo, value & 0xffff, hi, (value >> 16) & 0xffff)


def jump_l(address, target):
    """JUMP.L pcrel24 (0xE200 | offset[23:16], offset[15:0], offset = (target - address) / 2)."""
    off = (target - address) >> 1
    if not -(1 << 23) <= off < (1 << 23) or (target - address) & 1:
        raise SystemExit('JUMP.L {:#x} -> {:#x} out of range'.format(address, target))
    return struct.pack('<HH', 0xE200 | ((off >> 16) & 0xFF), off & 0xFFFF)


def call_l(address, target):
    """CALL pcrel24 (0xE300 | offset[23:16], offset[15:0], offset = (target - address) / 2)."""
    off = (target - address) >> 1
    if not -(1 << 23) <= off < (1 << 23) or (target - address) & 1:
        raise SystemExit('CALL {:#x} -> {:#x} out of range'.format(address, target))
    return struct.pack('<HH', 0xE300 | ((off >> 16) & 0xFF), off & 0xFFFF)


def jump_s(address, target):
    """JUMP.S pcrel12 (0x2000 | imm12, imm = (target - address) / 2)."""
    off = (target - address) >> 1
    if not -(1 << 11) <= off < (1 << 11) or (target - address) & 1:
        raise SystemExit('JUMP.S {:#x} -> {:#x} out of range'.format(address, target))
    return struct.pack('<H', 0x2000 | (off & 0xfff))


# Clean-up after S2 (docs/stereo-bus-v2.md): the stock per-voice sum into the mono bus 0xff800bb4 is skipped. Since S3
# its result is never read (coop's decimator reads the pair, and the bus's first 64 samples are overwritten with the
# mid); the voice hook sums into it itself when the stereo bus is off or the debug compare runs. Synth: after the
# render (the trampoline returns to 0xffa00d2a, `P1 = [FP -0x28]`, kept) `R0 = W[P5++]` at 0xffa00d2c becomes a jump
# over the loop to 0xffa00d72, which reloads P1 / P0 and reads nothing the loop wrote. Drum: the loop's LSETUP at
# 0xffa00922 becomes a jump to 0xffa00940, where the stock goes for a voice it does not sum (0xffa0075e).
VOICE_SUM_SKIPS = [
    (0xffa00d2c, bytes.fromhex('6894'), 0xffa00d72, 0),
    (0xffa00922, bytes.fromhex('a2e00e20'), 0xffa00940, 1),
]


def voice_sum_patches():
    return [(site, stock, jump_s(site, target) + b'\x00\x00' * pad) for site, stock, target, pad in VOICE_SUM_SKIPS]


# Own tanks for the stereo grid / spring objects (plugin_host.cc, tank_bind): the tank's immediate pair in each class's
# render and init becomes a JUMP.L to coop_tank_* (stereo_thunks.S), which jumps back after the pair.
TANK_SITES = [
    (0x019af846, bytes.fromhex('07e10cc7' '47e1d801'), 'tank_grid_render'),    # R7.L = 0xc70c; R7.H = 0x1d8
    (0x019af28c, bytes.fromhex('10e10cc7' '50e1d801'), 'tank_grid_init'),      # I0.L = 0xc70c; I0.H = 0x1d8
    (0x01a33002, bytes.fromhex('09e15262' '49e1de01'), 'tank_spring_render'),  # P1.L = 0x6252; P1.H = 0x1de
    (0x01a336b0, bytes.fromhex('08e15262' '48e1de01'), 'tank_spring_init'),    # P0.L = 0x6252; P0.H = 0x1de
]


# Performance (2026-11-04): the stock DSynth voice loop (0x0197E974..0x0197EA6A, four accessor CALLs per sample) becomes
# coop_dsynth_voice_loop (stereo_thunks.S, the same arithmetic in a hardware loop). Every path into the loop goes
# through 0x0197E956, where `P1 = FP; P1 += -0xa` becomes the JUMP.L.
DSYNTH_LOOP_SITE = 0x0197e956
DSYNTH_LOOP_STOCK = bytes.fromhex('4f32' 'b16f')

# Performance (2026-11-04): the stock cwo stereo render (0x019450FC) loop 0x019457D6..0x01945AC6 (six helper CALLs per
# sample) becomes coop_cwo_loop (cwo_fast.S, the helpers inlined). The only way into the loop is 0x019457D2,
# `R2 = 0x40 (X)`, which becomes the JUMP.L (coop_cwo_loop_near executes it when g_coop_fast_cwo = 0). The four
# __cxa_vec_ctor CALLs (0x01AA3862) zeroing the stack arrays that 0x019446E0 then fills whole call coop_cwo_vec_ctor,
# which returns at once while the switch is on.
CWO_LOOP_SITE = 0x019457d2
CWO_LOOP_STOCK = bytes.fromhex('22e14000')
CWO_VEC_CTOR_SITES = [(0x0194569a, bytes.fromhex('0ae3e4f0')), (0x019456b6, bytes.fromhex('0ae3d6f0')),
                      (0x019456dc, bytes.fromhex('0ae3c3f0')), (0x019456ee, bytes.fromhex('0ae3baf0'))]

# Performance (2026-11-04): the stock phone mono render (0x01A1BB74, called twice by the stereo render 0x01A1C25C) output
# loop 0x01A1BCB0..0x01A1BF34 becomes coop_phone_loop (phone_fast.S). The only way into the loop is its LSETUP at
# 0x01A1BCAC, which becomes the JUMP.L (coop_phone_loop_near sets LT1 / LB1 / LC1 by hand when g_coop_fast_phone = 0).
PHONE_LOOP_SITE = 0x01a1bcac
PHONE_LOOP_STOCK = bytes.fromhex('b2e04421')
# Its capture loop 0x01A1BC00..0x01A1BC58 the same way: the LSETUP at 0x01A1BBFC jumps to coop_phone_capture_near.
PHONE_CAPTURE_SITE = 0x01a1bbfc
PHONE_CAPTURE_STOCK = bytes.fromhex('a2e02e00')
# Performance (2026-11-04): the stock punch stereo render (0x019E662C, dual-mono: two CALLs of the mono render
# 0x019E6260) becomes coop_punch_render_st (punch_fast.S, both channels in one pass). Its only way in is the stereo
# class vtable's render entry (0x01910C3C + 0x14; no CALL or jump in the image targets 0x019E662C), which is pointed
# at coop; with g_coop_fast_punch = 0 coop jumps to the untouched stock render.
PUNCH_ST_RENDER_ENTRY = 0x01910c50
PUNCH_ST_RENDER = 0x019e662c
# Performance (2026-11-04): the stock grid stereo render (0x019AF83C) loop 0x019AF932..0x019AFCC8 (353 instructions, 88
# frame accesses per sample) becomes coop_grid_loop (grid_fast.S). The only way into the loop is its LSETUP at
# 0x019AF92E, which becomes the JUMP.L (coop_grid_loop_near sets LT0 / LB0 / LC0 by hand when g_coop_fast_grid = 0).
GRID_LOOP_SITE = 0x019af92e
GRID_LOOP_STOCK = bytes.fromhex('a2e0cd01')

# Performance (2026-11-04): the stock delay stereo render (0x0197DC42) per-sample loop LC1 0x0197DD4A..0x0197DF7A (160
# instructions with two LC0 loops, 15 frame accesses per sample) becomes coop_delay_loop (delay_fast.S), back at
# 0x0197DF7C. The only way into the loop is its LSETUP at 0x0197DD46, which becomes the JUMP.L (coop_delay_loop_near
# sets LT1 / LB1 / LC1 by hand when g_coop_fast_delay = 0).
DELAY_LOOP_SITE = 0x0197dd46
DELAY_LOOP_STOCK = bytes.fromhex('b2e01a11')

# Performance (2026-11-04): the stock nitro mono render (0x019A62A4, called twice by the stereo render 0x019A684C) loops
# 0x019A64A8..0x019A6682 and 0x019A6698..0x019A67F6 become coop_nitro_loop (nitro_fast.S), back at 0x019A67F8. The only
# way into them is loop 1's LSETUP at 0x019A64A4, which becomes the JUMP.L (coop_nitro_loop_near sets LT1 / LB1 / LC1
# by hand when g_coop_fast_nitro = 0; loop 2's LSETUP stays stock).
NITRO_LOOP_SITE = 0x019a64a4
NITRO_LOOP_STOCK = bytes.fromhex('b2e0ef20')


def tank_patches(thunks):
    return [(site, stock, jump_l(site, thunks[name]) + bytes.fromhex('00000000')) for site, stock, name in TANK_SITES]


def instr_patches(thunk):
    """[(address, stock, new)] for the three decimator + DC call pairs (S3)."""
    out = []
    for site in INSTR_SITES:
        stock = call24(site, DECIM_FN) + DC_TAIL
        new = bytes.fromhex('6701') + imm_pair_bytes(0, thunk) + bytes.fromhex('6000' '2701')
        assert len(stock) == len(new) == 14
        out.append((site, stock, new))
    return out


def drumbus_patch(thunk):
    return (DRUMBUS_SITE, imm_pair_bytes(1, DRUMBUS_FN), imm_pair_bytes(1, thunk))


def tramp_synth(thunk):
    # P2 = [P0 + 0x14]  = 42 ad ;  P1 = thunk ;  JUMP (P1) = 51 00
    return bytes.fromhex('42ad') + imm_pair_bytes(1, thunk) + bytes.fromhex('5100')


def tramp_drum(thunk):
    # P0 = [P1 + 0x24]  = 48 ae ;  P2 = thunk ;  JUMP (P2) = 52 00
    return bytes.fromhex('48ae') + imm_pair_bytes(2, thunk) + bytes.fromhex('5200')


def tramp_mix(thunk):
    # [--SP] = RETS = 67 01 ; P0 = thunk ; CALL (P0) = 60 00 ; RETS = [SP++] = 27 01 ; RTS = 10 00
    return bytes.fromhex('6701') + imm_pair_bytes(0, thunk) + bytes.fromhex('6000' '2701' '1000')


def tramp_plain(thunk):
    """12-byte trampoline for a site that was already a CALL: P2 = thunk; JUMP (P2); the thunk RTSes."""
    return imm_pair_bytes(2, thunk) + bytes.fromhex("5200") + bytes.fromhex("0000")


def tramp_noteon(thunk):
    # P0 = [P1 + 0x1c]  = c8 ad ;  P2 = thunk ;  JUMP (P2) = 52 00
    return bytes.fromhex('c8ad') + imm_pair_bytes(2, thunk) + bytes.fromhex('5200')


def layout(tramp_at, thunks, tape=False):
    """(name, address, bytes) for the trampolines packed from tramp_at."""
    parts = [('synth', tramp_synth(thunks['synth'])), ('drum', tramp_drum(thunks['drum'])),
             ('mix', tramp_mix(thunks['mix']))]
    if tape:
        parts += [('tape_rec', tramp_plain(thunks['tape_rec'])), ('tape_gains', tramp_plain(thunks['tape_gains'])),
                  ('seam', tramp_plain(thunks['seam']))]
    else:
        parts.append(('noteon', tramp_noteon(thunks['noteon'])))
    out, a = [], tramp_at
    for name, b in parts:
        out.append((name, a, b))
        a += len(b)
    if a > TRAMP_END:
        raise SystemExit('trampolines do not fit in free L1 {:#x}..{:#x}'.format(tramp_at, TRAMP_END))
    return out


def site_patches(tramps):
    """[(address, stock_bytes, new_bytes)] for the three L1 sites."""
    t = {name: a for name, a, _ in tramps}
    synth_new = SYNTH_STOCK[2:6] + call24(SYNTH_CALL_AT, t['synth'])
    drum_new = DRUM_STOCK[2:8] + call24(DRUM_CALL_AT, t['drum'])
    mix_new = call24(MIX_SITE, t['mix']) + jump_s(MIX_SITE + 4, MIX_RESUME)
    mixa_new = call24(MIXA_SITE, t['mix']) + jump_s(MIXA_SITE + 4, MIXA_RESUME)
    assert len(synth_new) == len(SYNTH_STOCK) and len(drum_new) == len(DRUM_STOCK) and len(mix_new) == len(MIX_STOCK)
    assert len(mixa_new) == len(MIXA_STOCK)
    out = [(SYNTH_SITE, SYNTH_STOCK, synth_new), (DRUM_SITE, DRUM_STOCK, drum_new), (MIX_SITE, MIX_STOCK, mix_new)]
    if not os.environ.get('COOP_NO_MIXA'):                       # experiment switch: leave loop A stock
        out.append((MIXA_SITE, MIXA_STOCK, mixa_new))
    if 'noteon' in t:
        noteon_new = NOTEON_STOCK[2:6] + call24(NOTEON_CALL_AT, t['noteon'])
        assert len(noteon_new) == len(NOTEON_STOCK)
        out.append((NOTEON_SITE, NOTEON_STOCK, noteon_new))
    return out


def patch_at(dxe, address, stock, new):
    """Replace len(stock) bytes at `address` in the loaded block that holds them, after checking them."""
    if len(stock) != len(new):
        raise SystemExit('{:#x}: patch length mismatch'.format(address))
    for b in dxe:
        loaded = ldr_dump.loaded_bytes(b)
        if loaded is None:
            continue
        start, end = b['target'], b['target'] + len(loaded)
        if start <= address and address + len(stock) <= end:
            if b['flags'] & 0x0100:
                raise SystemExit('{:#x} is in a FILL block, cannot patch'.format(address))
            payload = bytearray(b['payload'])
            off = address - start
            got = bytes(payload[off:off + len(stock)])
            if got != stock:
                raise SystemExit('{:#x}: stock bytes {} != expected {} (wrong image, or already patched?)'.format(
                    address, got.hex(), stock.hex()))
            payload[off:off + len(new)] = new
            b['payload'] = bytes(payload)
            return
    raise SystemExit('{:#x} not found in a loaded block'.format(address))


thunks_global = {}


def build_jump_tramp(fn):
    """16-byte region-C trampoline that does not touch the stack: P0 = fn; JUMP (P0). For replacements of functions
    that take arguments on the stack (lift: 4, drop: 5): a CALL-based trampoline would shift them by 4."""
    return imm_pair_bytes(0, fn) + bytes.fromhex('5000') + bytes.fromhex('0000' * 3)


def tape_patches(tramps, sdram_tramp_at=SDRAM_TRAMP_AT):
    """[(address, stock, new)] for the stereo tape: record CALL sites, the gains hook, the lane constants, and the
    SDRAM callers of tape_cache_io_step / tape_image_mount_and_index retargeted at region-C trampolines 1 and 2."""
    t = {name: a for name, a, _ in tramps}
    out = [(site, call24(site, TAPE_REC_FN), call24(site, t['tape_rec'])) for site in TAPE_REC_SITES]
    out.append((TAPE_GAINS_SITE, TAPE_GAINS_STOCK, call24(TAPE_GAINS_SITE, t['tape_gains']) + bytes.fromhex('00000000')))
    out.append((SEAM_SITE, SEAM_STOCK, call24(SEAM_SITE, t['seam']) + bytes.fromhex('00000000')))
    out.append((SEAM_FILL_SITE, SEAM_FILL_STOCK, imm_pair_bytes(1, thunks_global['seam_fill'])))
    out += [(site, call24(site, TAPE_IO_STEP), call24(site, sdram_tramp_at + 16)) for site in TAPE_IO_CALLERS]
    out += [(site, call24(site, TAPE_MOUNT), call24(site, sdram_tramp_at + 32)) for site in TAPE_MOUNT_CALLERS]
    out += [(site, call24(site, TAPE_LIFT), call24(site, sdram_tramp_at + 48)) for site in TAPE_LIFT_CALLERS]
    out += [(site, call24(site, TAPE_DROP), call24(site, sdram_tramp_at + 64)) for site in TAPE_DROP_CALLERS]
    out += [(site, call24(site, TAPE_ERASE), call24(site, sdram_tramp_at + 80)) for site in TAPE_ERASE_CALLERS]
    if thunks_global.get('sysex_rx'):
        out.append((MIDI_RX_SITE, call24(MIDI_RX_SITE, MIDI_RX_PARSE), call24(MIDI_RX_SITE, sdram_tramp_at + 96)))
    if thunks_global.get('fx_name_eq'):
        for addr, stock, name in FX_VTABLE_PATCHES:
            out.append((addr, struct.pack('<I', stock), struct.pack('<I', thunks_global[name])))
        out.append((FXNAME_SITE, FXNAME_STOCK, jump_l(FXNAME_SITE, sdram_tramp_at + 128)))
    if thunks_global.get('ui_frame'):                          # P3a: the slot 2 page (coop_ui.cc)
        out.append((UI_FRAME_SITE, call24(UI_FRAME_SITE, UI_FRAME_FN), call24(UI_FRAME_SITE, sdram_tramp_at + 144)))
        out.append((UI_KNOB_SITE, call24(UI_KNOB_SITE, UI_KNOB_FN), call24(UI_KNOB_SITE, sdram_tramp_at + 160)))
    if thunks_global.get('keyev'):                             # the Lander combo swallowed (the getter sites stay stock:
        out.append((KEYEV_SITE, call24(KEYEV_SITE, KEY_DISPATCH), call24(KEYEV_SITE, sdram_tramp_at + 112)))   # a stub screen hung the boot in op1emu)
    return out + list(TAPE_BYTE_PATCHES)


def sine_patches(dxe):
    """Retarget every `Px.L = lo; Px.H = hi` pair that names SINE_TABLE at SINE_L1, in every loaded non-FILL block.
    Returns report lines; refuses the image when the count is not SINE_REFS (a different stock image)."""
    lines = []
    found = 0
    for b in dxe:
        loaded = ldr_dump.loaded_bytes(b)
        if loaded is None or b['flags'] & 0x0100:
            continue
        payload = bytearray(b['payload'])
        i = 0
        while i + 8 <= len(payload):
            lo, hi = payload[i], payload[i + 4]
            if (payload[i + 1] == 0xe1 and payload[i + 5] == 0xe1 and 0xe108 <= (lo | 0xe100) <= 0xe10d
                    and hi == lo + 0x40 and payload[i + 2:i + 4] == bytes.fromhex('e8e5')
                    and payload[i + 6:i + 8] == bytes.fromhex('0101')):
                payload[i + 2:i + 4] = struct.pack('<H', SINE_L1 & 0xffff)
                payload[i + 6:i + 8] = struct.pack('<H', SINE_L1 >> 16)
                lines.append('sine ref {:#x}: P{} -> {:#x}'.format(b['target'] + i, lo & 7, SINE_L1))
                found += 1
                i += 8
            else:
                i += 2
        b['payload'] = bytes(payload)
    if found != SINE_REFS:
        raise SystemExit('sine table references: found {}, expected {}'.format(found, SINE_REFS))
    return lines


def core_noteon_patches(sdram_tramp_at=SDRAM_TRAMP_AT):
    """[(address, stock, new)] retargeting the Core::NoteOn callers at the region-C trampoline."""
    return [(site, call24(site, CORE_NOTEON), call24(site, sdram_tramp_at)) for site in CORE_NOTEON_CALLERS]


def thunk_addresses(elf):
    syms = read_elf_symbols(elf)
    try:
        keyev = symbol_address(syms, 'coop_key_event')[0]
    except Exception:
        keyev = None
    fxh = {}
    try:
        for name in ('fx_name_eq', 'fx_render_mono', 'fx_init_mono', 'fx_render_st', 'fx_init_st'):
            fxh[name] = symbol_address(syms, 'coop_' + name)[0]
    except Exception:
        fxh = {}
    try:
        instr = symbol_address(syms, 'coop_instr_thunk')[0]
        drumbus = symbol_address(syms, 'coop_drumbus_thunk')[0]
    except Exception:                                  # an ELF before S3
        instr = drumbus = None
    ui = {}
    try:                                               # P3a: the slot 2 page
        ui = {'ui_frame': symbol_address(syms, 'coop_ui_frame')[0], 'ui_knob': symbol_address(syms, 'coop_ui_knob')[0]}
    except Exception:
        ui = {}
    tanks = {}
    try:                                               # clean-up after S2: own grid / spring tanks
        for name in ('tank_grid_render', 'tank_grid_init', 'tank_spring_render', 'tank_spring_init'):
            tanks[name] = symbol_address(syms, 'coop_' + name)[0]
    except Exception:
        tanks = {}
    try:                                               # performance: the DSynth voice loop
        tanks['dsynth_loop'] = symbol_address(syms, 'coop_dsynth_voice_loop')[0]
    except Exception:
        pass
    try:                                               # performance: the cwo stereo loop
        tanks['cwo_loop'] = symbol_address(syms, 'coop_cwo_loop_near')[0]
        tanks['cwo_vec_ctor'] = symbol_address(syms, 'coop_cwo_vec_ctor')[0]
    except Exception:
        pass
    try:                                               # performance: the phone output loop
        tanks['phone_loop'] = symbol_address(syms, 'coop_phone_loop_near')[0]
        tanks['phone_capture'] = symbol_address(syms, 'coop_phone_capture_near')[0]
    except Exception:
        pass
    try:                                               # performance: the grid stereo loop
        tanks['grid_loop'] = symbol_address(syms, 'coop_grid_loop_near')[0]
    except Exception:
        pass
    try:                                               # performance: the delay stereo loop
        tanks['delay_loop'] = symbol_address(syms, 'coop_delay_loop_near')[0]
    except Exception:
        pass
    try:                                              # performance: the punch stereo render
        tanks['punch_st'] = symbol_address(syms, 'coop_punch_render_st')[0]
    except Exception:
        pass
    try:                                              # performance: the nitro mono render loops
        tanks['nitro_loop'] = symbol_address(syms, 'coop_nitro_loop_near')[0]
    except Exception:
        pass
    try:                                               # the voice hook can stand in for the stock voice sum
        tanks['voice_sum'] = symbol_address(syms, 'g_coop_voice_sum_own')[0]
    except Exception:
        pass
    try:
        sine_copy = symbol_address(syms, 'coop_l1_sine_copy')[0]
    except Exception:
        sine_copy = None
    try:
        sysex_rx = symbol_address(syms, 'coop_sysex_rx_poll')[0]
    except Exception:                                  # an ELF without the SysEx channel
        sysex_rx = None
    return {'instr': instr, 'drumbus': drumbus, 'sysex_rx': sysex_rx, 'sine_copy': sine_copy, 'keyev': keyev, **fxh, **tanks, **ui, 'synth': symbol_address(syms, 'coop_synth_render_thunk')[0],
            'drum': symbol_address(syms, 'coop_drum_render_thunk')[0],
            'mix': symbol_address(syms, 'coop_mix_thunk')[0],
            'noteon': symbol_address(syms, 'coop_synth_noteon_thunk')[0],
            'core_noteon': symbol_address(syms, 'coop_core_noteon_thunk')[0],
            'tape_rec': symbol_address(syms, 'coop_tape_rec_thunk')[0],
            'tape_gains': symbol_address(syms, 'coop_tape_gains_thunk')[0],
            'seam': symbol_address(syms, 'coop_seam_prologue_thunk')[0],
            'seam_fill': symbol_address(syms, 'coop_seam_fill')[0],
            'tape_io': symbol_address(syms, 'coop_tape_io_step')[0],
            'tape_mount': symbol_address(syms, 'coop_tape_mount_and_index')[0],
            'lift': symbol_address(syms, 'coop_lift')[0],
            'drop': symbol_address(syms, 'coop_drop')[0],
            'erase': symbol_address(syms, 'coop_erase')[0]}


def insert(ldr_bytes, thunks, tramp_at=TRAMP_BASE, tape=False):
    """The whole operation on LDR bytes -> (new bytes, report lines)."""
    dxes = ldr_dump.parse(ldr_bytes)
    if len(dxes) != 1:
        raise SystemExit('expected a single-DXE LDR')
    dxe = dxes[0]
    tramps = layout(tramp_at, thunks, tape)
    thunks_global.update(thunks)
    lines = []
    patches = site_patches(tramps) + core_noteon_patches()
    if thunks.get('instr'):
        patches += instr_patches(thunks['instr']) + [drumbus_patch(thunks['drumbus'])]
        if thunks.get('voice_sum'):
            patches += voice_sum_patches()
    if thunks.get('tank_grid_render'):
        patches += tank_patches(thunks)
    if thunks.get('dsynth_loop'):
        patches.append((DSYNTH_LOOP_SITE, DSYNTH_LOOP_STOCK, jump_l(DSYNTH_LOOP_SITE, thunks['dsynth_loop'])))
    if thunks.get('cwo_loop'):
        patches.append((CWO_LOOP_SITE, CWO_LOOP_STOCK, jump_l(CWO_LOOP_SITE, thunks['cwo_loop'])))
        patches += [(site, stock, call_l(site, thunks['cwo_vec_ctor'])) for site, stock in CWO_VEC_CTOR_SITES]
    if thunks.get('phone_loop'):
        patches.append((PHONE_LOOP_SITE, PHONE_LOOP_STOCK, jump_l(PHONE_LOOP_SITE, thunks['phone_loop'])))
        patches.append((PHONE_CAPTURE_SITE, PHONE_CAPTURE_STOCK, jump_l(PHONE_CAPTURE_SITE, thunks['phone_capture'])))
    if thunks.get('grid_loop'):
        patches.append((GRID_LOOP_SITE, GRID_LOOP_STOCK, jump_l(GRID_LOOP_SITE, thunks['grid_loop'])))
    if thunks.get('delay_loop'):
        patches.append((DELAY_LOOP_SITE, DELAY_LOOP_STOCK, jump_l(DELAY_LOOP_SITE, thunks['delay_loop'])))
    if thunks.get('punch_st'):
        patches.append((PUNCH_ST_RENDER_ENTRY, struct.pack('<I', PUNCH_ST_RENDER), struct.pack('<I', thunks['punch_st'])))
    if thunks.get('nitro_loop'):
        patches.append((NITRO_LOOP_SITE, NITRO_LOOP_STOCK, jump_l(NITRO_LOOP_SITE, thunks['nitro_loop'])))
    if tape:
        patches += tape_patches(tramps)
    for address, stock, new in patches:
        patch_at(dxe, address, stock, new)
        lines.append('site {:#x}: {} -> {}'.format(address, stock.hex(), new.hex()))
    if tape and thunks.get('sine_copy'):
        lines += sine_patches(dxe)
    blob = b''.join(b for _, _, b in tramps)
    final = [i for i, b in enumerate(dxe) if b['flags'] & 0x8000]
    if len(final) != 1:
        raise SystemExit('expected exactly one FINAL block')
    dxe.insert(final[0], plain_block(tramp_at, blob))
    sdram_blob = build_tramp(thunks['core_noteon'])
    if tape:                                               # +16: tape_cache_io_step, +32: tape_image_mount_and_index
        sdram_blob += build_tramp(thunks['tape_io']) + build_tramp(thunks['tape_mount'])
        sdram_blob += build_jump_tramp(thunks['lift']) + build_jump_tramp(thunks['drop']) + build_jump_tramp(thunks['erase'])   # +48, +64, +80
        if thunks.get('sysex_rx'):
            sdram_blob += build_jump_tramp(thunks['sysex_rx'])                                                             # +96
        if thunks.get('keyev'):
            sdram_blob += build_jump_tramp(thunks['keyev'])                                                                # +112
        if thunks.get('fx_name_eq'):
            sdram_blob += build_jump_tramp(thunks['fx_name_eq'])                                                           # +128
        if thunks.get('ui_frame'):
            if len(sdram_blob) != 144:
                raise SystemExit('region-C trampolines: the slot 2 page needs +144 / +160 (have {})'.format(len(sdram_blob)))
            sdram_blob += build_jump_tramp(thunks['ui_frame']) + build_jump_tramp(thunks['ui_knob'])                    # +144, +160
    dxe.insert(final[0], plain_block(SDRAM_TRAMP_AT, sdram_blob))
    for name, a, b in tramps:
        lines.append('tramp_{} at {:#x} ({} B) -> {:#x}'.format(name, a, len(b), thunks[name]))
    lines.append('sdram tramp at {:#x} (16 B) -> {:#x} for Core::NoteOn callers {}'.format(
        SDRAM_TRAMP_AT, thunks['core_noteon'], ', '.join('{:#x}'.format(s) for s in CORE_NOTEON_CALLERS)))
    if tape and thunks.get('fx_name_eq'):
        lines.append('fx plugins: filter vtables -> {:#x}/{:#x} (mono), {:#x}/{:#x} (master); fx_types name compare {:#x} -> tramp {:#x} -> {:#x}'.format(
            thunks['fx_render_mono'], thunks['fx_init_mono'], thunks['fx_render_st'], thunks['fx_init_st'], FXNAME_SITE, SDRAM_TRAMP_AT + 128, thunks['fx_name_eq']))
    if tape and thunks.get('keyev'):
        lines.append('sdram tramp at {:#x} -> {:#x} (key dispatch caller {:#x}: the Lander combo swallowed)'.format(
            SDRAM_TRAMP_AT + 112, thunks['keyev'], KEYEV_SITE))
    if tape and thunks.get('sysex_rx'):
        lines.append('sdram tramp at {:#x} -> {:#x} (midi_rx_parse caller {:#x}: the SysEx file channel)'.format(
            SDRAM_TRAMP_AT + 96, thunks['sysex_rx'], MIDI_RX_SITE))
    if tape:
        lines.append('sdram tramp at {:#x} -> {:#x} (tape_cache_io_step callers {}), {:#x} -> {:#x} (mount callers {})'.format(
            SDRAM_TRAMP_AT + 16, thunks['tape_io'], ', '.join('{:#x}'.format(s) for s in TAPE_IO_CALLERS),
            SDRAM_TRAMP_AT + 32, thunks['tape_mount'], ', '.join('{:#x}'.format(s) for s in TAPE_MOUNT_CALLERS)))
    return ldr_dump.serialize(dxes), lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ldr_in')
    ap.add_argument('ldr_out')
    ap.add_argument('--elf', required=True, help='the coop combined ELF (for the thunk addresses)')
    ap.add_argument('--tramp-at', type=lambda s: int(s, 0), default=TRAMP_BASE)
    ap.add_argument('--tape-stereo', action='store_true', help='also wire the stereo tape (T1): 8 lanes, coop record tap')
    args = ap.parse_args()
    thunks = thunk_addresses(args.elf)
    out, lines = insert(open(args.ldr_in, 'rb').read(), thunks, args.tramp_at, args.tape_stereo)
    open(args.ldr_out, 'wb').write(out)
    for line in lines:
        print(line)
    print('{}: stereo bus wired{}'.format(args.ldr_out, ' + stereo tape (T1)' if args.tape_stereo else ''))


if __name__ == '__main__':
    main()
