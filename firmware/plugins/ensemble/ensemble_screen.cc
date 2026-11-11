/* ensemble's screen (e9, docs/plugins.md "e9"): te-op1-cfw's kennel screen (ports/knl/ensemble/ensemble_knl.cc
 * ScreenInit / ScreenUpdate, the canvas "Ensemble screen" design C "Drift") on the coop effect-screen calls
 * (sdk/coop_fx_screen.h). The drawing is kennel's; what changed (README "Screen"):
 *   - inputs: the four turns from view->knobs (kennel's knl::ScreenPage1, the stock pages' values with the LFO), the
 *     gain from view->words[0] (word 5). Kennel's screen read nothing from the DSP, so the render publishes nothing;
 *   - kennel's blue page 2 is fn held (view->mods bit 1): fn + blue turns the gain, so while fn is down the gain bar
 *     lights blue (blue's dark tone at rest) with its bracket, and the rate dot and bar (blue's plain turn) dim;
 *     kennel's dB readout glyphs are gone (the core's popup names the value);
 *   - the R lane always shows (coop is stereo at every position; kennel showed it at the master only).
 *
 *   vL1..vL3, vR1..vR3   the voices' strings, a count set per lane (1 + round(2 x voices)), each translated by
 *                        depth x 28 units x sin(phase + i x 120 deg (+180 R))
 *   hL1..hL3, hR1..hR3   the home marks with their green depth ticks, the same count set, each scaled about its
 *                        mark by depth
 *   sweep, sdot          the dial's dot, circling at the rate (0.05..8 Hz, log; the screen's own phase)
 *   rbar                 the rate bar, scaled about its left end
 *   gbar, gbox           the gain gauge (scaled -1..1 about the centre) and its bracket while fn is held
 *   mbar                 the mix gauge, scaled about its foot
 *
 * One phase for every position, as kennel had it (only one position is on screen at a time).
 *
 * GUI thread only. Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include <stdint.h>
#include "coop_plugin.h"
#include "coop_fx_screen.h"

namespace {

using namespace coop_scr;

const int32_t kCY = 76, kBottom = 124, kSpan = 28, kDialR = 10, kRateX0 = 252;
const int32_t kHomes[3] = { 80, 140, 200 };

const char* const kVoiceIds[6] = { "vL1", "vL2", "vL3", "vR1", "vR2", "vR3" };
const char* const kHomeIds[6] = { "hL1", "hL2", "hL3", "hR1", "hR2", "hR3" };
const char* const kNodeIds[5] = { "sweep", "rbar", "gbar", "gbox", "mbar" };
const char* const kPaletteIds[3] = { "paldim", "palblue", "paldimblue" };
enum { N_SWEEP, N_RBAR, N_GBAR, N_GBOX, N_MBAR };
enum { C_DIM, C_BLUE, C_DIMBLUE };

bool ready;
void* voice_nodes[6];
void* home_nodes[6];
void* nodes[5];
uint8_t* sdot_fill;
uint8_t* rbar_fill;
uint8_t* gbar_fill;
Colour pal[3];
FrameClock frame_clock;
uint32_t phase;

// The rate's speed on the screen, 0.05 .. 8 Hz (log), Q16 (kennel's RateHzQ16, unchanged).
int32_t RateHzQ16(int32_t u15) {
  const int32_t oct_q12 = (u15 * 29983) >> 15;   // 7.32 octaves, Q12
  const int32_t whole = oct_q12 >> 12;
  const int32_t frac = oct_q12 & 0xfff;
  const int32_t base = 3277 << whole;            // 0.05 Hz, Q16
  return base + static_cast<int32_t>((static_cast<int64_t>(base) * frac) >> 12);
}

void ScreenInit(void* scr) {
  ready = false;
  phase = 0;
  frame_clock.last = Cycles();
  void* tmp[3];
  if (!FindAll(scr, kVoiceIds, voice_nodes, 6) || !FindAll(scr, kHomeIds, home_nodes, 6) ||
      !FindAll(scr, kNodeIds, nodes, 5) || !FindAll(scr, kPaletteIds, tmp, 3) || !Find(scr, "sdot")) {
    return;
  }
  for (int32_t i = 0; i < 3; ++i) ReadColour(&pal[i], LineStyle(scr, kPaletteIds[i]));
  sdot_fill = FillStyle(scr, "sdot");
  rbar_fill = FillStyle(scr, "rbar");
  gbar_fill = FillStyle(scr, "gbar");
  ready = true;
}

void ScreenUpdate(void* scr, int dt, const coop_fx_view_t* v) {
  if (ready) {
    const int32_t rate = Clamp15(v->knobs[0]), depth = Clamp15(v->knobs[1]), voices = Clamp15(v->knobs[2]),
                  mix = Clamp15(v->knobs[3]);
    const int32_t gain = Clamp15(v->words[0]);
    const bool fn = (v->mods & 2) != 0;              // fn + blue turns the gain (kennel: blue on page 2)

    // Rate: the dot round the dial, and the bar; both dim while fn is held (blue's turn is the gain then).
    phase += PhaseStep(RateHzQ16(rate), frame_clock.Tick());
    SetTranslate(nodes[N_SWEEP], (kDialR * SinQ15(phase)) >> 5, (kDialR << 10) - ((kDialR * CosQ15(phase)) >> 5));
    ScaleX(nodes[N_RBAR], rate < 655 ? 20 : rate >> 5, kRateX0);
    WriteColour(sdot_fill, fn ? pal[C_DIM] : pal[C_BLUE]);
    WriteColour(rbar_fill, fn ? pal[C_DIM] : pal[C_BLUE]);
    // The voices: a count set per lane; each string sways about its home by depth x 28 units, the three a third of a
    // turn apart, the R lane opposite; its home mark's ticks spread to the same reach.
    const int32_t count = 1 + Steps(voices, 2);
    const int32_t reach = (depth * kSpan) >> 5;    // Q10 units
    int32_t scale = depth >> 5;                     // Q10
    if (scale < 20) scale = 20;
    for (int32_t lane = 0; lane < 2; ++lane) {
      for (int32_t i = 0; i < 3; ++i) {
        const int32_t k = lane * 3 + i;
        const bool on = i < count;
        SetVisible(voice_nodes[k], on);
        SetVisible(home_nodes[k], on);
        const uint32_t ph = phase + static_cast<uint32_t>(i) * 0x55555555u + (lane ? 0x80000000u : 0u);
        SetTranslate(voice_nodes[k], (reach * SinQ15(ph)) >> 15, 0);
        ScaleX(home_nodes[k], scale, kHomes[i]);
      }
    }
    // Gain: the bar scaled -1..1 about the centre, blue's dark tone at rest and blue with its bracket while fn is held.
    const int32_t d = (gain - 16384) >> 4;          // Q10, -1024..1023
    SetMatrix(nodes[N_GBAR], 0x400, d, 0, kCY * (0x400 - d));
    WriteColour(gbar_fill, fn ? pal[C_BLUE] : pal[C_DIMBLUE]);
    SetVisible(nodes[N_GBOX], fn);
    // Mix: the bar scaled about its foot.
    const int32_t s = mix >> 5;
    SetMatrix(nodes[N_MBAR], 0x400, s < 8 ? 8 : s, 0, kBottom * (0x400 - (s < 8 ? 8 : s)));
  }
  coop_scr_draw(scr, dt);
}

}  // namespace

extern "C" const coop_fx_screen_t SCREEN_ensemble = { ScreenInit, ScreenUpdate };
