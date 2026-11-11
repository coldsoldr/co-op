/* tape_drive's screen (e9, docs/plugins.md "e9"): te-op1-cfw's kennel screen (ports/knl/tape_drive/tape_knl.cc
 * ScreenInit / ScreenUpdate, the canvas "Tape screen" design A "Reels, aligned") on the coop effect-screen calls
 * (sdk/coop_fx_screen.h). The drawing and its motion are kennel's; what changed (README "Screen"):
 *   - inputs: the four turns from view->knobs (kennel's knl::ScreenPage1, op1_fx_knobs_live), the gain from
 *     view->words[0] (word 5). Kennel's screen read nothing from the DSP, so the render publishes nothing;
 *   - kennel's blue page 2 is fn held (view->mods bit 1): fn + blue turns the gain, so while fn is down the gain's
 *     bracket shows and the saturation wave (blue's plain turn) dims to the body tone; kennel's dB readout glyphs are
 *     gone (the core's popup names the value).
 *
 *   sat0..sat7     variant set by saturate: the wave on the tape (blue; the body tone while fn is held)
 *   reelL, reelR   the hubs (green), jittered per frame at the wobble's rate: 0.5 Hz (wow) .. 8 Hz (flutter), up to
 *                  2 x wobble units; the right hub a quarter turn behind
 *   wobble         the marker on the wow -> flutter scale (green), 0..104 units
 *   agef, ages     the low-pass glyph (white): the flat scaled about its left end, the slope moved with the corner;
 *                  both dim toward the worn white with age
 *   age0..age5     the hiss specks (white), a count set
 *   gbar, gbox     the gain gauge (blue): the bar scaled -1..1 about the centre; the bracket while fn is held
 *   mbar           the mix gauge (orange), scaled about its foot
 *
 * The animation state (the frame clock, the hubs' phase) is shared by the positions, as kennel's was: one screen shows
 * at a time. GUI thread only. Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include <stdint.h>
#include "coop_plugin.h"
#include "coop_fx_screen.h"

namespace {

using namespace coop_scr;

const int32_t kCY = 76, kBottom = 124, kScaleSpan = 104, kAgeX = 248, kAgeWalk = 22, kAgeFlat = 28;

const char* const kSatIds[8] = { "sat0", "sat1", "sat2", "sat3", "sat4", "sat5", "sat6", "sat7" };
const char* const kAgeIds[6] = { "age0", "age1", "age2", "age3", "age4", "age5" };
const char* const kNodeIds[8] = { "wobble", "reelL", "reelR", "agef", "ages", "gbar", "gbox", "mbar" };
const char* const kPaletteIds[4] = { "palwhite", "paldim", "palblue", "palworn" };
enum { N_WOBBLE, N_REELL, N_REELR, N_AGEF, N_AGES, N_GBAR, N_GBOX, N_MBAR };
enum { C_WHITE, C_DIM, C_BLUE, C_WORN };

bool ready;
void* sat_nodes[8];
void* age_nodes[6];
void* nodes[8];
uint8_t* sat_lines[8];
uint8_t* agef_line;
uint8_t* ages_line;
Colour pal[4];
FrameClock frame_clock;
uint32_t phase;

void ScreenInit(void* scr) {
  ready = false;
  void* tmp[4];
  if (!FindAll(scr, kSatIds, sat_nodes, 8) || !FindAll(scr, kAgeIds, age_nodes, 6) ||
      !FindAll(scr, kNodeIds, nodes, 8) || !FindAll(scr, kPaletteIds, tmp, 4)) {
    return;
  }
  for (int32_t i = 0; i < 4; ++i) ReadColour(&pal[i], LineStyle(scr, kPaletteIds[i]));
  for (int32_t i = 0; i < 8; ++i) sat_lines[i] = LineStyle(scr, kSatIds[i]);
  agef_line = LineStyle(scr, "agef");
  ages_line = LineStyle(scr, "ages");
  frame_clock.Tick();
  phase = 0;
  ready = true;
}

void ScreenUpdate(void* scr, int dt, const coop_fx_view_t* v) {
  if (ready) {
    const int32_t saturate = Clamp15(v->knobs[0]), wobble = Clamp15(v->knobs[1]);
    const int32_t age = 32767 - Clamp15(v->knobs[2]), mix = Clamp15(v->knobs[3]);   // age mirrored, as the render
    const int32_t gain = Clamp15(v->words[0]);
    const bool fn = (v->mods & 2) != 0;              // fn + blue turns the gain (kennel: blue on page 2)

    // Saturation: the wave on the tape, dimmed while fn is held.
    const int32_t k = Steps(saturate, 7);
    ShowOne(sat_nodes, 8, k);
    WriteColour(sat_lines[k], fn ? pal[C_DIM] : pal[C_BLUE]);
    // Wobble: the marker, and the hubs: 0.5 Hz (wow) .. 8 Hz (flutter) as wobble rises, up to 2 x wobble units; the
    // right hub a quarter turn behind.
    SetTranslate(nodes[N_WOBBLE], (wobble * kScaleSpan) >> 5, 0);
    const int32_t hz_q16 = 32768 + ((wobble * 491520) >> 15);
    phase += PhaseStep(hz_q16, frame_clock.Tick());
    const int32_t amp = (wobble * 2) >> 5;                                         // Q10 units, 0..2
    SetTranslate(nodes[N_REELL], (amp * SinQ15(phase)) >> 15, (amp * CosQ15(phase * 3)) >> 16);
    SetTranslate(nodes[N_REELR], (amp * SinQ15(phase + 0x40000000u)) >> 15,
                 (amp * CosQ15(phase * 3 + 0x40000000u)) >> 16);
    // Age: the glyph's corner walks left (the flat scaled about x 248, the slope translated), both dimming toward the
    // worn white; the specks count up.
    const int32_t walk = (age * kAgeWalk) >> 5;                                    // Q10 units, 0..22
    ScaleX(nodes[N_AGEF], 0x400 - walk / kAgeFlat, kAgeX);
    SetTranslate(nodes[N_AGES], -walk, 0);
    const Colour worn = Blend(pal[C_WHITE], pal[C_WORN], age >> 1);
    WriteColour(agef_line, worn);
    WriteColour(ages_line, worn);
    ShowFirst(age_nodes, 6, Steps(age, 6));
    // Gain: the bar scaled -1..1 about the centre; the bracket while fn is held.
    const int32_t d = (gain - 16384) >> 4;                                         // Q10, -1024..1023
    SetMatrix(nodes[N_GBAR], 0x400, d, 0, kCY * (0x400 - d));
    SetVisible(nodes[N_GBOX], fn);
    // Mix: the bar scaled about its foot.
    const int32_t s = mix >> 5;
    SetMatrix(nodes[N_MBAR], 0x400, s < 8 ? 8 : s, 0, kBottom * (0x400 - (s < 8 ? 8 : s)));
  }
  coop_scr_draw(scr, dt);
}

}  // namespace

extern "C" const coop_fx_screen_t SCREEN_tape_drive = { ScreenInit, ScreenUpdate };
