/* bitcrusher's screen (e9, docs/plugins.md "e9"): te-op1-cfw's kennel screen (ports/knl/bitcrusher/crush_knl.cc
 * ScreenInit / ScreenUpdate, the canvas "Bitcrusher screen" option C) on the coop effect-screen calls
 * (sdk/coop_fx_screen.h). The drawing is kennel's; what changed (README "Screen"):
 *   - inputs: the four turns from view->knobs (kennel's op1_fx_knobs_live), the gain from view->words[0] (word 5), the
 *     input level from the state the render publishes (state.h, kennel's in_level[] in .bss);
 *   - kennel's blue page 2 is fn held (view->mods bit 1): fn + blue turns the gain, so while fn is down the gain's
 *     bracket shows and the staircase (rate, blue's turn) dims; kennel's dB readout glyphs are gone (the core's popup
 *     names the value);
 *   - the clock row is raster lines (coop_scr_line) instead of 8 x 2 SVG groups (17 KB of the file).
 *
 *   stair0..stair7   variant set by rate: the staircase; the clock row's ticks are drawn at the same step width
 *   bit0..bit7       count set, 8 - round(7 x bits) (16 bits: 8)
 *   dot              the sampling dot, running the steps
 *   gbar, gbox       the gain gauge: the bar scaled -1..1 about the centre, its blue brightening with the input level;
 *                    the bracket while fn is held
 *   mbar             the mix gauge, scaled about its foot
 *
 * GUI thread only. Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include <stdint.h>
#include "coop_plugin.h"
#include "coop_fx_screen.h"
#include "state.h"

namespace {

using namespace coop_scr;

const int32_t kX0 = 40, kX1 = 288, kCY = 76, kBottom = 124;
const int32_t kClkTop = 10, kClkBottom = 18;     // the clock row (kennel's tick lines, y 10..18)
// The variants' step widths x 10 (gen_bitcrusher_svg.py STEP_WIDTHS).
const int32_t kStepW10[8] = { 80, 106, 139, 184, 243, 321, 423, 559 };
// 48 sin(2 pi i / 64), the reference sine's height per 1/64 of the span.
const int32_t kSine48[64] = {
  0, 5, 9, 14, 18, 22, 27, 30, 34, 37, 40, 42, 44, 46, 47, 48, 48, 48, 47, 46, 44, 42, 40, 37, 34, 30, 27, 22, 18, 14, 9, 5,
  0, -5, -9, -14, -18, -22, -27, -30, -34, -37, -40, -42, -44, -46, -47, -48, -48, -48, -47, -46, -44, -42, -40, -37, -34, -30, -27, -22, -18, -14, -9, -5 };

const char* const kStairIds[8] = { "stair0", "stair1", "stair2", "stair3", "stair4", "stair5", "stair6", "stair7" };
const char* const kBitIds[8] = { "bit0", "bit1", "bit2", "bit3", "bit4", "bit5", "bit6", "bit7" };
const char* const kNodeIds[4] = { "dot", "gbar", "gbox", "mbar" };
const char* const kPaletteIds[5] = { "palwhite", "paldim", "palblue", "paldimblue", "palmid" };
enum { N_DOT, N_GBAR, N_GBOX, N_MBAR };
enum { C_WHITE, C_DIM, C_BLUE, C_DIMBLUE, C_MID };

bool ready;
void* stair_nodes[8];
void* bit_nodes[8];
void* nodes[4];
uint8_t* stair_styles[8];
uint8_t* gbar_fill;
Colour pal[5];
uint32_t frame_rng;
uint32_t dot_phase;   // Q16 steps

void ScreenInit(void* scr) {
  ready = false;
  frame_rng = 1;
  dot_phase = 0;
  void* tmp[5];
  if (!FindAll(scr, kStairIds, stair_nodes, 8) || !FindAll(scr, kBitIds, bit_nodes, 8) ||
      !FindAll(scr, kNodeIds, nodes, 4) || !FindAll(scr, kPaletteIds, tmp, 5)) {
    return;
  }
  for (int32_t i = 0; i < 5; ++i) ReadColour(&pal[i], LineStyle(scr, kPaletteIds[i]));
  for (int32_t i = 0; i < 8; ++i) stair_styles[i] = LineStyle(scr, kStairIds[i]);
  gbar_fill = FillStyle(scr, "gbar");
  ready = true;
}

// The clock row: one tick a step at the variant's width, the two halves (even / odd ticks) shaken apart by `shake`.
void ClockRow(int32_t k, int32_t shake) {
  void* r = coop_scr_raster();
  if (!r) return;
  int32_t i = 0;
  for (int32_t x10 = kX0 * 10; x10 <= kX1 * 10; x10 += kStepW10[k], ++i) {
    const int32_t x = x10 * 1024 / 10 + ((i & 1) ? -shake : shake);
    const int32_t p0[2] = { x, kClkTop << 10 }, p1[2] = { x, kClkBottom << 10 };
    coop_scr_line(r, p0, p1, 1536, pal[C_WHITE].rgba);
  }
}

void ScreenUpdate(void* scr, int dt, const coop_fx_view_t* v) {
  int32_t k = 0, shake = 0;
  if (ready) {
    const int32_t rate = 32767 - v->knobs[0], bits = 32767 - v->knobs[1], jitter = v->knobs[2], mix = v->knobs[3];   // mirrored, as the render
    const int32_t gain = Clamp15(v->words[0]);
    const bool fn = (v->mods & 2) != 0;              // fn + blue turns the gain (kennel: blue on page 2)
    const volatile coop_bitcrusher::State* st = Published<coop_bitcrusher::State>(v);
    const int32_t level = st ? Clamp15(st->disp.level * 4) : 0;

    // Rate: the staircase (and the clock row below), dimmed while fn is held.
    k = Steps(Clamp15(rate), 7);
    ShowOne(stair_nodes, 8, k);
    WriteColour(stair_styles[k], fn ? pal[C_DIM] : pal[C_BLUE]);
    // Bits: the ladder.
    ShowFirst(bit_nodes, 8, 8 - Steps(Clamp15(bits), 7));
    // Jitter: the clock row's two halves shaken apart, up to 4 units.
    frame_rng = frame_rng * 1664525u + 1013904223u;
    const int32_t noise = static_cast<int32_t>(frame_rng >> 28) - 8;   // -8..7
    shake = (Clamp15(jitter) * noise) >> 6;                              // Q10, |.| <= 4 units
    // The sampling dot: one step per tick of the clock, faster at higher rates.
    dot_phase += 1800 + 400 * k;
    const int32_t steps = (kX1 - kX0) * 10 / kStepW10[k] + 1;
    const int32_t step = static_cast<int32_t>((dot_phase >> 16) % static_cast<uint32_t>(steps));
    const int32_t x10 = kX0 * 10 + step * kStepW10[k];
    const int32_t i64 = ((x10 - kX0 * 10) * 64 / ((kX1 - kX0) * 10)) & 63;
    const int32_t level_y = kCY - ((kSine48[i64] + (kSine48[i64] >= 0 ? 6 : -6)) / 12) * 12;
    SetTranslate(nodes[N_DOT], (x10 - kX0 * 10) * 1024 / 10, (level_y - kCY) * 1024);
    // Gain: the bar scaled -1..1 about the centre; its blue brightens with the input. The bracket while fn is held.
    const int32_t d = (gain - 16384) >> 4;                               // Q10, -1024..1023
    SetMatrix(nodes[N_GBAR], 0x400, d, 0, kCY * (0x400 - d));
    WriteColour(gbar_fill, Blend(pal[C_DIMBLUE], pal[C_BLUE], level));
    SetVisible(nodes[N_GBOX], fn);
    // Mix: the bar scaled about its foot.
    const int32_t s = mix >> 5;
    SetMatrix(nodes[N_MBAR], 0x400, s < 8 ? 8 : s, 0, kBottom * (0x400 - (s < 8 ? 8 : s)));
  }
  coop_scr_draw(scr, dt);
  if (ready) ClockRow(k, shake);
}

}  // namespace

extern "C" const coop_fx_screen_t SCREEN_bitcrusher = { ScreenInit, ScreenUpdate };
