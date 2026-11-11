/* drive's screen (e9, docs/plugins.md "e9"): te-op1-cfw's kennel screen (ports/knl/drive/drive_knl.cc ScreenInit /
 * ScreenUpdate, the canvas "Drive screen" design A "Schematic") on the coop effect-screen calls
 * (sdk/coop_fx_screen.h). The drawing is kennel's; what changed (README "Screen"):
 *   - inputs: the four turns from view->knobs (kennel's knl::ScreenPage1), the gain from view->words[0] (word 5).
 *     Kennel's screen read no DSP state, so the render publishes nothing (no display words);
 *   - kennel's blue page 2 is fn held (view->mods bit 1): fn + blue turns the gain, so while fn is down the bracket
 *     over the pot shows and the diodes dim, their fill gone (kennel's page 2 did the dimming); kennel's dB readout
 *     glyphs are gone (the core's popup names the value);
 *   - the diodes' fill (kennel's clip1..clip7 variants) is raster lines (coop_scr_line) from the tip.
 *
 *   dio / diodim     the diodes' outlines, blue, or dim while fn is held
 *   (raster)         the fill from the tip by drive, round(7 x drive) sevenths (none at 0); conduction rays from 5
 *   wiper            the pot's blue arrow, translated -13..13 units by the gain
 *   gbox             the bracket over the wiper's travel while fn is held
 *   fil, glow        the triode's filament (line) and the glow under its cathode (fill), warming with bias
 *   tilt             the tilt EQ's S response, scaled about the pivot by 2t - 1 (flat at the centre, flipped for dark)
 *   mbar             the mix gauge, scaled about its foot
 *
 * GUI thread only. Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include <stdint.h>
#include "coop_plugin.h"
#include "coop_fx_screen.h"

namespace {

using namespace coop_scr;

const int32_t kY = 78, kWiperSwing = 13, kBottom = 124;
// The diodes (gen_drive_svg.py DIODE_L / DIODE_R / DIODE_A / DIODE_B): the left one's tip at the bottom row, the
// right one's at the top; 12 units tall, 10 wide at the base.
const int32_t kDiodeL = 52, kDiodeR = 80, kDiodeA = 96, kDiodeB = 108;
const int32_t kRay[2][2] = { { kDiodeL, 101 }, { kDiodeR, 103 } };   // each diode's column and middle row (the rays)

const char* const kNodeIds[6] = { "wiper", "dio", "diodim", "gbox", "tilt", "mbar" };
const char* const kPaletteIds[5] = { "palwhite", "paldim", "palblue", "palblack", "palglow" };
enum { N_WIPER, N_DIO, N_DIODIM, N_GBOX, N_TILT, N_MBAR };
enum { C_WHITE, C_DIM, C_BLUE, C_BLACK, C_GLOW };

bool ready;
void* nodes[6];
uint8_t* fil_line;
uint8_t* glow_fill;
Colour pal[5];

void ScreenInit(void* scr) {
  ready = false;
  void* tmp[5];
  if (!FindAll(scr, kNodeIds, nodes, 6) || !FindAll(scr, kPaletteIds, tmp, 5) || !Find(scr, "fil") ||
      !Find(scr, "glow")) {
    return;
  }
  for (int32_t i = 0; i < 5; ++i) ReadColour(&pal[i], LineStyle(scr, kPaletteIds[i]));
  fil_line = LineStyle(scr, "fil");
  glow_fill = FillStyle(scr, "glow");
  ready = true;
}

void Line(void* r, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {   // Q10 coordinates, 1.5 wide, blue
  const int32_t p0[2] = { x0, y0 }, p1[2] = { x1, y1 };
  coop_scr_line(r, p0, p1, 1536, pal[C_BLUE].rgba);
}

// The diodes' fill, k sevenths from the tip (kennel's clip1..clip7): a scan line a unit, the triangle's width there;
// the conduction rays beside each diode from k = 5 (above its middle), and below it too at 7.
void DiodeFill(int32_t k) {
  if (k <= 0) return;
  void* r = coop_scr_raster();
  if (!r) return;
  const int32_t rows = (12 * k + 3) / 7;                        // round(12 k / 7)
  for (int32_t i = 1; i <= rows; ++i) {
    const int32_t hw = 5 * i * 1024 / 12;                       // Q10 half width i units from the tip
    Line(r, kDiodeL * 1024 - hw, (kDiodeB - i) << 10, kDiodeL * 1024 + hw, (kDiodeB - i) << 10);
    Line(r, kDiodeR * 1024 - hw, (kDiodeA + i) << 10, kDiodeR * 1024 + hw, (kDiodeA + i) << 10);
  }
  if (k < 5) return;
  for (int32_t d = 0; d < 2; ++d) {
    const int32_t x = kRay[d][0], y = kRay[d][1];
    for (int32_t s = -1; s <= 1; s += 2) {                        // above the middle, and (at 7) below it
      if (s > 0 && k < 7) break;
      const int32_t yy = (y + 3 * s) << 10;
      Line(r, (x - 9) << 10, yy, (x - 7) << 10, yy);
      Line(r, (x + 7) << 10, yy, (x + 9) << 10, yy);
    }
  }
}

void ScreenUpdate(void* scr, int dt, const coop_fx_view_t* v) {
  int32_t k = 0;
  if (ready) {
    const int32_t drive = Clamp15(v->knobs[0]), tone = Clamp15(v->knobs[1]), bias = Clamp15(v->knobs[2]),
                  mix = Clamp15(v->knobs[3]);
    const int32_t gain = Clamp15(v->words[0]);
    const bool fn = (v->mods & 2) != 0;               // fn + blue turns the gain (kennel: blue on page 2)

    // Drive: the diodes fill from the tip (drawn after the SVG); dim outlines and no fill while fn is held.
    k = fn ? 0 : Steps(drive, 7);
    SetVisible(nodes[N_DIO], !fn);
    SetVisible(nodes[N_DIODIM], fn);
    // Gain: the wiper along the pot, -13..13 units; the bracket over its travel while fn is held.
    SetTranslate(nodes[N_WIPER], ((gain - 16384) * kWiperSwing) >> 4, 0);   // Q10
    SetVisible(nodes[N_GBOX], fn);
    // Bias: the filament and the glow warm.
    WriteColour(fil_line, Blend(pal[C_DIM], pal[C_WHITE], bias));
    WriteColour(glow_fill, Blend(pal[C_BLACK], pal[C_GLOW], bias));
    // Tone: the S response scaled about the pivot, 2t - 1 (never quite zero: a flat line).
    int32_t d = (tone >> 4) - 0x400;   // Q10
    if (d > -4 && d < 4) d = 4;
    SetMatrix(nodes[N_TILT], 0x400, d, 0, kY * (0x400 - d));
    // Mix: the bar scaled about its foot.
    const int32_t s = mix >> 5;
    SetMatrix(nodes[N_MBAR], 0x400, s < 8 ? 8 : s, 0, kBottom * (0x400 - (s < 8 ? 8 : s)));
  }
  coop_scr_draw(scr, dt);
  if (ready) DiodeFill(k);
}

}  // namespace

extern "C" const coop_fx_screen_t SCREEN_drive = { ScreenInit, ScreenUpdate };
