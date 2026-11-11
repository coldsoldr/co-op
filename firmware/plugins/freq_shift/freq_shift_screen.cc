/* freq_shift's screen (e9, docs/plugins.md "e9"): te-op1-cfw's kennel screen (ports/knl/freq_shift/shift_knl.cc
 * ScreenInit / ScreenUpdate, the canvas "Freq shift screen" design B "Dial") on the coop effect-screen calls
 * (sdk/coop_fx_screen.h). The drawing is kennel's; what changed (README "Screen"):
 *   - inputs: the four turns from view->knobs (kennel's knl::ScreenPage1), the gain from view->words[0] (word 5), the
 *     mode from view->words[1] (word 6, a step index: 1 = ring), the input level from the state the render publishes
 *     (state.h, kennel's in_level[] in .bss);
 *   - coop is stereo at every position, so the two needles part with the spread everywhere (kennel: the master only);
 *   - kennel's blue page 2 is fn held (view->mods bit 1): fn + blue turns the gain, so while fn is down the gain's
 *     bracket shows and the needles (shift, blue's turn) dim; kennel's dB readout glyphs are gone (the core's popup
 *     names the value);
 *   - kennel's green page 2 (the mode) is a plain green click now: no page, so its bracket (mbox) is gone; the mode
 *     marks always show the mode;
 *   - the ring-mode feedback set (fr1..fr4) is the up set turned to the top.
 *
 *   ndlL, ndlR       the needles, turned about the hub: 78 degrees each way for +-2 kHz (L at shift + spread, R at
 *                    shift - spread; in ring mode at the carriers |shift + spread|, |shift - spread|, mirrored)
 *   sp1..sp7         the spread arc, a variant set by the spread, turned to the shift's angle (to |shift| in ring mode)
 *   rp1..rp4         the sound's ripples: arcs scaled from r 20 out to the rim and faded, born at the input level
 *   fu1..fu4         feedback, a count set: arcs sweeping right from the top, mirrored for a shift down, turned to
 *                    centre on the top in ring mode
 *   modea1, modea2, moder   the mode marks: the lit one green
 *   gbar, gbox       the gain gauge: the bar scaled -1..1 about the centre; the bracket while fn is held
 *   mbar             the mix gauge, scaled about its foot
 *
 * GUI thread only. Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include <stdint.h>
#include "coop_plugin.h"
#include "coop_fx_screen.h"
#include "state.h"

namespace {

using namespace coop_scr;

const int32_t kCY = 76, kBottom = 124, kHubX = 160, kHubY = 124;
const int32_t kRippleScale = 4096;         // a ripple grows from r 20 to the rim: x5 = 1 + 4096/1024
const int32_t kRippleHz = 78643;           // 1.2 Hz (Q16): a ripple's journey takes 0.8 s
const int32_t kRippleGate = 1500;          // the level (0..32767) under which no ripple starts
const uint32_t kRippleGap = 0x30000000u;   // the youngest ripple's phase before the next may start
const uint32_t kSweepTurn = 930537128u;    // 78 degrees as a Q32 turn (kennel's constant)
const uint32_t kRingTurn = 0u - 417566265u;   // -35 degrees: the feedback arcs (90..20 degrees) centred on the top

const char* const kRpIds[4] = { "rp1", "rp2", "rp3", "rp4" };
const char* const kFuIds[4] = { "fu1", "fu2", "fu3", "fu4" };
const char* const kSpIds[7] = { "sp1", "sp2", "sp3", "sp4", "sp5", "sp6", "sp7" };
const char* const kNodeIds[5] = { "ndlL", "ndlR", "gbar", "gbox", "mbar" };
const char* const kModeIds[3] = { "modea1", "modea2", "moder" };
const char* const kPaletteIds[5] = { "paldim", "palmid", "palblack", "palblue", "palgreen" };
enum { N_NDLL, N_NDLR, N_GBAR, N_GBOX, N_MBAR };
enum { C_DIM, C_MID, C_BLACK, C_BLUE, C_GREEN };

bool ready;
void* rp_nodes[4];
uint8_t* rp_lines[4];
uint32_t rp_phase[4];
int32_t rp_amp[4];
int32_t rp_young;   // the ripple started last
FrameClock clock;
void* fu_nodes[4];
void* sp_nodes[7];
void* nodes[5];
uint8_t* mode_lines[3];
uint8_t* needle_lines[2];
Colour pal[5];

// Turn a node by the Q32 turn `turn` (clockwise on the screen) about (cx, cy): matrix(cos, sin, -sin, cos, e, f).
void RotateAbout(void* node, uint32_t turn, int32_t cx, int32_t cy) {
  const int32_t c = CosQ15(turn) >> 5;   // Q10
  const int32_t s = SinQ15(turn) >> 5;
  SetMatrix6(node, c, s, -s, c, cx * (0x400 - c) + cy * s, cy * (0x400 - c) - cx * s);
}

// A needle's turn for a position -1..1 (Q15): 78 degrees each way.
uint32_t Turn(int32_t f15) {
  return static_cast<uint32_t>(static_cast<int64_t>(f15) * static_cast<int32_t>(kSweepTurn) >> 15);
}

int32_t Abs(int32_t v) { return v < 0 ? -v : v; }
int32_t Clamp1(int32_t v) { return v < -32767 ? -32767 : (v > 32767 ? 32767 : v); }

void ScreenInit(void* scr) {
  ready = false;
  void* tmp[5];
  if (!FindAll(scr, kRpIds, rp_nodes, 4) || !FindAll(scr, kFuIds, fu_nodes, 4) || !FindAll(scr, kSpIds, sp_nodes, 7) ||
      !FindAll(scr, kNodeIds, nodes, 5) || !FindAll(scr, kModeIds, tmp, 3) || !FindAll(scr, kPaletteIds, tmp, 5)) {
    return;
  }
  for (int32_t i = 0; i < 5; ++i) ReadColour(&pal[i], LineStyle(scr, kPaletteIds[i]));
  for (int32_t i = 0; i < 3; ++i) mode_lines[i] = LineStyle(scr, kModeIds[i]);
  needle_lines[0] = LineStyle(scr, "ndlL");
  needle_lines[1] = LineStyle(scr, "ndlR");
  for (int32_t i = 0; i < 4; ++i) {
    rp_lines[i] = LineStyle(scr, kRpIds[i]);
    rp_amp[i] = 0;
    rp_phase[i] = 0;
    SetVisible(rp_nodes[i], false);
  }
  rp_young = -1;
  clock.last = Cycles();
  ready = true;
}

// The sound's ripples: born at the hub when the input is up, growing to the rim over 0.8 s and fading, at most one
// every fifth of a journey (kennel's, shared by every position as kennel's were).
void Ripples(int32_t level) {
  const uint32_t step = PhaseStep(kRippleHz, clock.Tick());
  for (int32_t i = 0; i < 4; ++i) {
    if (rp_amp[i] == 0) continue;
    const uint32_t next = rp_phase[i] + step;
    if (next < rp_phase[i]) {   // wrapped: the journey is over
      rp_amp[i] = 0;
      SetVisible(rp_nodes[i], false);
      continue;
    }
    rp_phase[i] = next;
    const int32_t grow = static_cast<int32_t>((static_cast<uint64_t>(next) * kRippleScale) >> 32);   // Q10, 0..4096
    const int32_t s = 0x400 + grow;
    SetMatrix(rp_nodes[i], s, s, kHubX * (0x400 - s), kHubY * (0x400 - s));
    const int32_t life = 32767 - static_cast<int32_t>(next >> 17);                                  // Q15, fading
    WriteColour(rp_lines[i], Blend(pal[C_BLACK], pal[C_MID], (life * rp_amp[i]) >> 15));
  }
  if (level > kRippleGate && (rp_young < 0 || rp_amp[rp_young] == 0 || rp_phase[rp_young] > kRippleGap)) {
    for (int32_t i = 0; i < 4; ++i) {
      if (rp_amp[i] == 0) {
        rp_amp[i] = level;
        rp_phase[i] = 0;
        rp_young = i;
        SetMatrix(rp_nodes[i], 0x400, 0x400, 0, 0);
        WriteColour(rp_lines[i], Blend(pal[C_BLACK], pal[C_MID], level));
        SetVisible(rp_nodes[i], true);
        break;
      }
    }
  }
}

void ScreenUpdate(void* scr, int dt, const coop_fx_view_t* v) {
  if (ready) {
    const int32_t gain = Clamp15(v->words[0]);
    const bool ring = v->words[1] == 1;              // word 6: step 0 shift, 1 ring (as the render)
    const bool fn = (v->mods & 2) != 0;              // fn + blue turns the gain (kennel: blue on page 2)
    const volatile coop_freq_shift::State* st = Published<coop_freq_shift::State>(v);
    const int32_t level = st ? Clamp15(st->disp.level * 4) : 0;

    // The mode marks: the lit one green, the other in the body tone.
    WriteColour(mode_lines[0], ring ? pal[C_DIM] : pal[C_GREEN]);
    WriteColour(mode_lines[1], ring ? pal[C_DIM] : pal[C_GREEN]);
    WriteColour(mode_lines[2], ring ? pal[C_GREEN] : pal[C_DIM]);

    // Shift: -1..1 (Q15); the spread parts the needles a third of the sweep each way (every position is stereo).
    const int32_t f = Clamp1((Clamp15(v->knobs[0]) - 16384) * 2);
    const int32_t af = Abs(f);
    const int32_t sp = Clamp15(v->knobs[2]);
    const int32_t half = (sp * 10923) >> 15;   // spread / 3
    if (ring) {   // the carriers |shift + spread|, |shift - spread|, mirrored about the top
      RotateAbout(nodes[N_NDLL], Turn(Clamp1(Abs(f + half))), kHubX, kHubY);
      RotateAbout(nodes[N_NDLR], Turn(-Clamp1(Abs(f - half))), kHubX, kHubY);
    } else {
      RotateAbout(nodes[N_NDLL], Turn(Clamp1(f + half)), kHubX, kHubY);
      RotateAbout(nodes[N_NDLR], Turn(Clamp1(f - half)), kHubX, kHubY);
    }
    WriteColour(needle_lines[0], fn ? pal[C_DIM] : pal[C_BLUE]);
    WriteColour(needle_lines[1], fn ? pal[C_DIM] : pal[C_BLUE]);
    // Spread: the arc (a variant by the spread), turned to the shift (to |shift| in ring mode).
    const int32_t k = Steps(sp, 7);
    ShowOne(sp_nodes, 7, k - 1);
    if (k > 0) RotateAbout(sp_nodes[k - 1], Turn(ring ? af : f), kHubX, kHubY);
    // The sound's ripples, from the published input level.
    Ripples(level);
    // Feedback: the arcs, the way the sound climbs (mirrored for down); centred on the top in ring mode.
    ShowFirst(fu_nodes, 4, Steps(Clamp15(v->knobs[1]), 4));
    for (int32_t i = 0; i < 4; ++i) {
      if (ring) {
        RotateAbout(fu_nodes[i], kRingTurn, kHubX, kHubY);
      } else if (f < 0) {
        SetMatrix(fu_nodes[i], -0x400, 0x400, (2 * kHubX) << 10, 0);   // mirrored about the hub
      } else {
        SetTranslate(fu_nodes[i], 0, 0);
      }
    }
    // Gain: the bar scaled -1..1 about the centre; the bracket while fn is held.
    const int32_t d = (gain - 16384) >> 4;   // Q10, -1024..1023
    SetMatrix(nodes[N_GBAR], 0x400, d, 0, kCY * (0x400 - d));
    SetVisible(nodes[N_GBOX], fn);
    // Mix: the bar scaled about its foot.
    const int32_t s = Clamp15(v->knobs[3]) >> 5;
    SetMatrix(nodes[N_MBAR], 0x400, s < 8 ? 8 : s, 0, kBottom * (0x400 - (s < 8 ? 8 : s)));
  }
  coop_scr_draw(scr, dt);
}

}  // namespace

extern "C" const coop_fx_screen_t SCREEN_freq_shift = { ScreenInit, ScreenUpdate };
