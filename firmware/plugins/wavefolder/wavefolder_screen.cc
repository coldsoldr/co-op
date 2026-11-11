/* wavefolder's screen (e9, docs/plugins.md "e9"): te-op1-cfw's kennel screen (ports/knl/wavefolder/fold_knl.cc
 * ScreenInit / ScreenUpdate, the canvas "Fold screen" design A "Wave, aligned") on the coop effect-screen calls
 * (sdk/coop_fx_screen.h). The drawing is kennel's; what changed (README "Screen"):
 *   - inputs: the four turns from view->knobs (kennel's knl::ScreenPage1), the gain from view->words[0] (word 5);
 *     kennel's screen read no DSP state, so the render publishes nothing (the state is unchanged);
 *   - kennel's blue page 2 is fn held (view->mods bit 1): fn + blue turns the gain, so while fn is down the gain's
 *     bracket shows and the wave (fold, blue's plain turn) dims to the body tone; kennel's dB readout glyphs are gone
 *     (the core's popup names the value);
 *   - the wave is raster lines (coop_scr_line) instead of eight 97-point SVG polylines (12 KB of kennel's 16 KB file):
 *     the same curve, its fold factor following the turn continuously instead of in eight poses.
 *
 *   (code)           the output cycle: y = CY - AMP sin(g pi/2 sin th), g = 1 + 4.2 fold (kennel's poses: 1 + 0.6 k)
 *   symmetry         the offset line, translated -24 (2v - 1) units
 *   tonef, tones     the low-pass glyph: the flat scaled about its left end, the slope moved with the corner
 *   gbar, gbox       the gain gauge: the bar scaled -1..1 about the centre; the bracket while fn is held
 *   mbar             the mix gauge, scaled about its foot
 *
 * GUI thread only. Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include <stdint.h>
#include "coop_plugin.h"
#include "coop_fx_screen.h"

namespace {

using namespace coop_scr;

const int32_t kCY = 76, kBottom = 124, kToneX = 248, kToneWalk = 20, kToneFlat = 28;
const int32_t kX0 = 44, kAmp = 36;          // the wave's left end and amplitude (gen_wavefolder_svg.py X0, AMP)
const int32_t kSegs = 96;                   // kennel's 96 steps a cycle; 192 units wide, 2 units a segment
const uint32_t kThStep = 44739243u;         // 2^32 / 96, rounded
const int32_t kGSpan = 17203;               // g - 1 at full fold, Q12 (4.2)

const char* const kNodeIds[6] = { "symmetry", "tonef", "tones", "gbar", "gbox", "mbar" };
const char* const kPaletteIds[2] = { "palblue", "paldim" };
enum { N_SYM, N_TONEF, N_TONES, N_GBAR, N_GBOX, N_MBAR };
enum { C_BLUE, C_DIM };

bool ready;
void* nodes[6];
Colour pal[2];

void ScreenInit(void* scr) {
  ready = false;
  void* tmp[2];
  if (!FindAll(scr, kNodeIds, nodes, 6) || !FindAll(scr, kPaletteIds, tmp, 2)) return;
  for (int32_t i = 0; i < 2; ++i) ReadColour(&pal[i], LineStyle(scr, kPaletteIds[i]));
  ready = true;
}

// The output cycle, g in Q12, as raster lines over the SVG.
void Wave(int32_t g_q12, const Colour& c) {
  void* r = coop_scr_raster();
  if (!r) return;
  int32_t prev[2] = { kX0 << 10, kCY << 10 };
  for (int32_t i = 1; i <= kSegs; ++i) {
    const int32_t inner = SinQ15(static_cast<uint32_t>(i) * kThStep);           // sin th, Q15
    // g pi/2 sin th as a phase: g / 4 turns at sin th = 1, i.e. g_q12 x inner x 2^32 / 2^29 (wrapping is the turn)
    const uint32_t phase = static_cast<uint32_t>(g_q12 * inner) << 3;
    const int32_t p[2] = { (kX0 << 10) + i * 2048, (kCY << 10) - ((kAmp * SinQ15(phase)) >> 5) };
    coop_scr_line(r, prev, p, 1536, c.rgba);
    prev[0] = p[0];
    prev[1] = p[1];
  }
}

void ScreenUpdate(void* scr, int dt, const coop_fx_view_t* v) {
  int32_t g = 0;
  bool fn = false;
  if (ready) {
    const int32_t fold = Clamp15(v->knobs[0]), symmetry = Clamp15(v->knobs[1]), tone = Clamp15(v->knobs[2]),
                  mix = Clamp15(v->knobs[3]);
    const int32_t gain = Clamp15(v->words[0]);
    fn = (v->mods & 2) != 0;                       // fn + blue turns the gain (kennel: blue on page 2)

    g = 4096 + fold * kGSpan / 32767;
    SetTranslate(nodes[N_SYM], 0, (24 << 10) - ((symmetry * 48) >> 5));     // -24 (2v - 1) units
    // Tone: the glyph's corner walks left as it darkens (the flat scaled about x 248, the slope translated), 20 units
    // at full dark.
    const int32_t walk = ((32767 - tone) * kToneWalk) >> 5;                   // Q10 units, 0..20
    ScaleX(nodes[N_TONEF], 0x400 - walk / kToneFlat, kToneX);
    SetTranslate(nodes[N_TONES], -walk, 0);
    // Gain: the bar scaled -1..1 about the centre; the bracket while fn is held.
    const int32_t d = (gain - 16384) >> 4;                                     // Q10, -1024..1023
    SetMatrix(nodes[N_GBAR], 0x400, d, 0, kCY * (0x400 - d));
    SetVisible(nodes[N_GBOX], fn);
    // Mix: the bar scaled about its foot.
    const int32_t s = mix >> 5;
    SetMatrix(nodes[N_MBAR], 0x400, s < 8 ? 8 : s, 0, kBottom * (0x400 - (s < 8 ? 8 : s)));
  }
  coop_scr_draw(scr, dt);
  if (ready) Wave(g, pal[fn ? C_DIM : C_BLUE]);
}

}  // namespace

extern "C" const coop_fx_screen_t SCREEN_wavefolder = { ScreenInit, ScreenUpdate };
