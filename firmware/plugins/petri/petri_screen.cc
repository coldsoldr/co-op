/* petri's screen (e9, docs/plugins.md "e9"): the lead's mock-up "petri screen" (Petri.dc.html) on the coop effect-screen
 * calls (sdk/coop_fx_screen.h). A stompbox seen from above in 3/4: an orthographic box 190 x 100 x 30 units, yawed a
 * little and swaying slowly, hopping on the input's onsets (higher with activity), tipping forward while fn is held (so
 * its front panel shows). On its top a petri dish where the voices are dots: the angle is the voice's read position in
 * the buffer (clockwise from 12 o'clock), the distance from the centre its pitch (16 + 8 log2 |rate| units), the tint
 * its envelope; a tick on the rim is the write head (gone while held). Four knobs (blue time, green activity, white
 * repeats, orange mix) as cylinders with pointers, the reverse (blue) and hold (orange) footswitches pressed down and
 * their LEDs lit while the latches are on, the dish's rim orange while held. On the front: the 11 program LEDs (the
 * effect lit blue), the filter (white) and space (orange) gauges.
 *
 * How it is drawn (README "Screen"): every face is flat art in petri.svg (gen_petri_svg.py), moved by ONE matrix a
 * frame computed here in screen space (a face's origin and two unit axes through the projection), so nothing depends
 * on group transforms composing; the moving strokes (knob pointers, the write tick, the dish's wall lines, the gauges'
 * fills) are raster lines (coop_scr_line) over the SVG. Fixed point (Q10 screen units, Q15 trigonometry, 64-bit
 * products), no float, no libc. Inputs: view->knobs (the turns, live with the LFO), view->words (effect, filter,
 * space), view->latches (hold, reverse), view->mods (fn), and the render's display words (state.h Display: the voices,
 * the write head, the onsets) while the view is live.
 *
 * GUI thread only. Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include <stdint.h>
#include "coop_plugin.h"
#include "coop_fx_screen.h"
#include "state.h"

namespace {

using namespace coop_scr;

const int32_t kW = 95, kD = 50, kH = 30;                 // gen_petri_svg.py W, D, H
const int32_t kCx = 160, kCy = 84;                       // the projection's centre (the mock's 78, 6 lower: the popup)
const int32_t kDishU = -45, kDishV = 5;
const int32_t kElRestQ16 = 66191, kElFnQ16 = 45875;      // the elevation: 1.01 rad, 0.70 with fn (Q16 radians)
const int32_t kRadToPhase = 10430;                       // Q16 radians -> a 2^32 phase: 2^32 / (2 pi 2^16)
const int32_t kOne = 1024;                               // a unit, Q10

struct Knob {
  int16_t u, v, r, h;
};
const Knob kKnobs[6] = { { 20, 30, 10, 8 }, { 58, 30, 10, 8 }, { 20, 2, 10, 8 }, { 58, 2, 10, 8 },
                         { 20, -34, 7, 7 }, { 58, -34, 7, 7 } };   // gen_petri_svg.py KNOBS

const Colour kBlue = { { 0x69, 0x8E, 0xFF, 0xFF } }, kGreen = { { 0x00, 0xED, 0x95, 0xFF } };
const Colour kWhite = { { 0xFF, 0xFF, 0xFF, 0xFF } }, kOrange = { { 0xFF, 0x3A, 0x5D, 0xFF } };
const Colour kBlueD = { { 0x2B, 0x3A, 0x70, 0xFF } }, kOrangeD = { { 0x6E, 0x1A, 0x2B, 0xFF } };
const Colour kBody = { { 0x38, 0x35, 0x4D, 0xFF } }, kPencil = { { 0xDF, 0xD9, 0xFF, 0xFF } };
const Colour kBlack = { { 0x01, 0x01, 0x01, 0xFF } };
const Colour* const kKnobColour[4] = { &kBlue, &kGreen, &kWhite, &kOrange };

const char* const kFaceIds[4] = { "ffront", "fside", "ftop", "flid" };
enum { F_FRONT, F_SIDE, F_TOP, F_LID };
const char* const kSideIds[6] = { "ks0", "ks1", "ks2", "ks3", "ks4", "ks5" };
const char* const kTopIds[6] = { "kt0", "kt1", "kt2", "kt3", "kt4", "kt5" };
const char* const kDotIds[8] = { "vd0", "vd1", "vd2", "vd3", "vd4", "vd5", "vd6", "vd7" };
const char* const kDotFillIds[8] = { "vc0", "vc1", "vc2", "vc3", "vc4", "vc5", "vc6", "vc7" };
const char* const kProgIds[11] = { "pl0", "pl1", "pl2", "pl3", "pl4", "pl5", "pl6", "pl7", "pl8", "pl9", "pl10" };

bool ready;
void* faces[4];
void* sides[6];
void* tops[6];
void* dots[8];
uint8_t* dot_fill[8];
uint8_t* prog_fill[11];
uint8_t* prog_line[11];
uint8_t *rev_fill, *rev_line, *hold_fill, *hold_line, *rim_line;
uint32_t sway;                                           // the yaw's slow sway (a phase)
int32_t el_q16[COOP_FX_SLOTS];                           // the elevation, per position (it eases)
int32_t hop[COOP_FX_SLOTS];                              // the onset hop, Q15, decaying
int32_t last_onsets[COOP_FX_SLOTS];

// The projection for this frame (the mock's P(x, y, z) = [cx + x c - y s, cy - ((x s + y c) se + (z + bob) ce)]):
// yaw th and elevation el as Q15 sines and cosines, the hop in Q10 units.
struct Proj {
  int64_t c, s, se, ce, bob;
};

// A point in Q10 units -> the screen, Q10.
void Project(const Proj& p, int32_t x, int32_t y, int32_t z, int32_t* out) {
  const int64_t a = (x * p.s + y * p.c) >> 15;
  out[0] = static_cast<int32_t>((static_cast<int64_t>(kCx) << 10) + ((x * p.c - y * p.s) >> 15));
  out[1] = static_cast<int32_t>((static_cast<int64_t>(kCy) << 10) - ((a * p.se + (z + p.bob) * p.ce) >> 15));
}

// A direction in Q10 units -> its screen vector, Q10 (the projection's linear part).
void Direction(const Proj& p, int32_t x, int32_t y, int32_t z, int32_t* out) {
  const int64_t a = (x * p.s + y * p.c) >> 15;
  out[0] = static_cast<int32_t>((x * p.c - y * p.s) >> 15);
  out[1] = static_cast<int32_t>(-((a * p.se + z * p.ce) >> 15));
}

// A face: local (x, y) -> origin + x U + y V (Q10 units), as the node's matrix.
void Face(void* node, const Proj& p, int32_t ox, int32_t oy, int32_t oz, int32_t ux, int32_t uy, int32_t uz,
          int32_t vx, int32_t vy, int32_t vz) {
  int32_t o[2], u[2], v[2];
  Project(p, ox, oy, oz, o);
  Direction(p, ux, uy, uz, u);
  Direction(p, vx, vy, vz, v);
  SetMatrix6(node, u[0], u[1], v[0], v[1], o[0], o[1]);
}

void Line(void* r, const Proj& p, int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1, int32_t z1,
          int32_t width_q10, const Colour& col) {
  int32_t a[2], b[2];
  Project(p, x0, y0, z0, a);
  Project(p, x1, y1, z1, b);
  coop_scr_line(r, a, b, width_q10, col.rgba);
}

// r x sin and r x cos of a phase, Q10 units (r in Q10).
inline int32_t RSin(int32_t r, uint32_t a) { return static_cast<int32_t>((static_cast<int64_t>(r) * SinQ15(a)) >> 15); }
inline int32_t RCos(int32_t r, uint32_t a) { return static_cast<int32_t>((static_cast<int64_t>(r) * CosQ15(a)) >> 15); }

// log2 of a Q8 rate's size, Q10 (piecewise linear between powers of two: a screen distance).
int32_t Log2Q10(int32_t rate_q8) {
  int32_t r = rate_q8 < 0 ? -rate_q8 : rate_q8;
  if (r < 16) r = 16;
  if (r > 65535) r = 65535;
  int32_t n = 0;
  while ((r >> (n + 1)) > 0) ++n;                                    // r in [2^n, 2^(n + 1))
  const int32_t frac = ((r - (1 << n)) << 10) >> n;                   // 0..1023
  return ((n - 8) << 10) + frac;
}

void ScreenInit(void* scr) {
  ready = false;
  if (!FindAll(scr, kFaceIds, faces, 4) || !FindAll(scr, kSideIds, sides, 6) || !FindAll(scr, kTopIds, tops, 6) ||
      !FindAll(scr, kDotIds, dots, 8)) {
    return;
  }
  for (int32_t i = 0; i < 8; ++i) {
    dot_fill[i] = FillStyle(scr, kDotFillIds[i]);
    if (!dot_fill[i]) return;
  }
  for (int32_t i = 0; i < 11; ++i) {
    prog_fill[i] = FillStyle(scr, kProgIds[i]);
    prog_line[i] = LineStyle(scr, kProgIds[i]);
    if (!prog_fill[i] || !prog_line[i]) return;
  }
  rev_fill = FillStyle(scr, "lrev");
  rev_line = LineStyle(scr, "lrev");
  hold_fill = FillStyle(scr, "lhold");
  hold_line = LineStyle(scr, "lhold");
  rim_line = LineStyle(scr, "lidr");
  if (!rev_fill || !rev_line || !hold_fill || !hold_line || !rim_line) return;
  sway = 0;
  for (int32_t i = 0; i < COOP_FX_SLOTS; ++i) {
    el_q16[i] = kElRestQ16;
    hop[i] = 0;
    last_onsets[i] = 0;
  }
  ready = true;
}

void ScreenUpdate(void* scr, int dt, const coop_fx_view_t* v) {
  if (!ready) {
    coop_scr_draw(scr, dt);
    return;
  }
  const int32_t slot = v->slot >= 0 && v->slot < COOP_FX_SLOTS ? v->slot : 0;
  const int32_t ms = dt < 0 ? 0 : (dt > 200 ? 200 : dt);            // the frame's time, as the core passes it
  const uint32_t cycles = static_cast<uint32_t>(ms) * 400000u;        // PhaseStep counts 400 MHz cycles
  const bool fn = (v->mods & 2) != 0;
  const bool hold = (v->latches & 1) != 0, rev = (v->latches & 2) != 0;
  const int32_t act = Clamp15(v->knobs[1]);
  const volatile coop_petri::Instance* st = Published<coop_petri::Instance>(v);

  // the motion: the sway (0.45 rad/s), the elevation easing to fn's, the hop on an onset
  sway += PhaseStep(4694, cycles);                                    // 0.0716 Hz, Q16
  const int32_t th_q16 = -6554 + ((3932 * SinQ15(sway)) >> 15);       // -0.10 + 0.06 sin(0.45 t) rad
  const int32_t target = fn ? kElFnQ16 : kElRestQ16;
  int32_t k = ms * 197;                                               // dt x 6 a second, Q15
  if (k > 32767) k = 32767;
  el_q16[slot] += static_cast<int32_t>((static_cast<int64_t>(target - el_q16[slot]) * k) >> 15);
  if (st) {
    const int32_t on = st->disp.onsets;
    if (on != last_onsets[slot]) hop[slot] = 32767;
    last_onsets[slot] = on;
  }
  for (int32_t f = 0; f < ms / 16 + 1 && f < 8; ++f) hop[slot] = (hop[slot] * 28835) >> 15;   // x 0.88 a 16 ms frame
  int32_t sn = SinQ15(static_cast<uint32_t>(hop[slot]) * 64665u);     // sin(3.1 hop)
  if (sn < 0) sn = -sn;
  Proj p;
  p.bob = ((static_cast<int64_t>(hop[slot]) * (32768 + 3 * act) >> 15) * sn) >> 20;   // hop (1 + 3 act) |sin|, Q10
  const uint32_t thp = static_cast<uint32_t>(th_q16 * kRadToPhase);
  const uint32_t elp = static_cast<uint32_t>(el_q16[slot] * kRadToPhase);
  p.c = CosQ15(thp);
  p.s = SinQ15(thp);
  p.se = SinQ15(elp);
  p.ce = CosQ15(elp);

  // the faces and the cylinders, each one matrix
  Face(faces[F_FRONT], p, 0, -kD * kOne, 0, kOne, 0, 0, 0, 0, kOne);
  Face(faces[F_SIDE], p, kW * kOne, 0, 0, 0, kOne, 0, 0, 0, kOne);    // the yaw stays negative: the right side shows
  Face(faces[F_TOP], p, 0, 0, kH * kOne, kOne, 0, 0, 0, kOne, 0);
  Face(faces[F_LID], p, 0, 0, (kH + 4) * kOne, kOne, 0, 0, 0, kOne, 0);
  const int32_t tx = static_cast<int32_t>(p.c >> 5), ty = static_cast<int32_t>(-p.s >> 5);   // the tangent at -th
  int32_t height[6];
  for (int32_t i = 0; i < 6; ++i) {
    const Knob& kn = kKnobs[i];
    height[i] = kn.h * kOne;
    if ((i == 4 && rev) || (i == 5 && hold)) height[i] = 2560;       // pressed: 2.5 units
    const int32_t scale = height[i] / kn.h;                           // the side's rect is h units tall
    Face(sides[i], p, kn.u * kOne, kn.v * kOne, kH * kOne, tx, ty, 0, 0, 0, scale);
    Face(tops[i], p, kn.u * kOne, kn.v * kOne, kH * kOne + height[i], kOne, 0, 0, 0, kOne, 0);
  }

  // the colours: the program LEDs, the footswitch LEDs, the rim
  int32_t effect = v->words[0];
  effect = effect < 0 ? 0 : (effect > 10 ? 10 : effect);
  for (int32_t i = 0; i < 11; ++i) {
    WriteColour(prog_fill[i], i == effect ? kBlue : kBlack);
    WriteColour(prog_line[i], i == effect ? kBlue : kBody);
  }
  WriteColour(rev_fill, rev ? kBlue : kBlack);
  WriteColour(rev_line, rev ? kBlue : kBlueD);
  WriteColour(hold_fill, hold ? kOrange : kBlack);
  WriteColour(hold_line, hold ? kOrange : kOrangeD);
  const Colour& rim = hold ? kOrange : kPencil;
  WriteColour(rim_line, rim);

  // the voices in the dish: the angle their read position, the distance their pitch, the tint their envelope
  for (int32_t i = 0; i < 8; ++i) {
    const int32_t pos = st ? st->disp.v[i].pos : -1;
    const int32_t level = st ? Clamp15(st->disp.v[i].level) : 0;
    if (pos < 0 || pos > 32767 || level < 600) {
      SetVisible(dots[i], false);
      continue;
    }
    int32_t rq = (16 << 10) + 8 * Log2Q10(st->disp.v[i].rate);
    rq = rq < (6 << 10) ? (6 << 10) : (rq > (29 << 10) ? (29 << 10) : rq);
    const uint32_t a = static_cast<uint32_t>(pos) << 17;
    int32_t at[2];
    Project(p, kDishU * kOne + RSin(rq, a), kDishV * kOne + RCos(rq, a), (kH + 4) * kOne, at);
    SetTranslate(dots[i], at[0], at[1]);
    WriteColour(dot_fill[i], Blend(kBody, kPencil, level));
    SetVisible(dots[i], true);
  }

  coop_scr_draw(scr, dt);

  // the raster strokes over the art
  void* r = coop_scr_raster();
  if (!r) return;
  for (int32_t sg = 0; sg < 2; ++sg) {                               // the dish's wall at its silhouette (phi -th, pi - th)
    const int32_t cu = static_cast<int32_t>(((sg ? -p.c : p.c) * 36) >> 5);   // 36 cos(phi), Q10
    const int32_t cv = static_cast<int32_t>(((sg ? p.s : -p.s) * 36) >> 5);   // 36 sin(phi)
    Line(r, p, kDishU * kOne + cu, kDishV * kOne + cv, kH * kOne, kDishU * kOne + cu, kDishV * kOne + cv,
         (kH + 4) * kOne, 1536, rim);
  }
  if (st && !hold) {                                                  // the write head: a tick from r 31 to r 36
    const uint32_t a = static_cast<uint32_t>(Clamp15(st->disp.head)) << 17;
    Line(r, p, kDishU * kOne + RSin(31 * kOne, a), kDishV * kOne + RCos(31 * kOne, a), (kH + 4) * kOne,
         kDishU * kOne + RSin(36 * kOne, a), kDishV * kOne + RCos(36 * kOne, a), (kH + 4) * kOne, 2048, kPencil);
  }
  for (int32_t i = 0; i < 4; ++i) {                                   // the knobs' pointers, -135 .. +135 degrees
    const Knob& kn = kKnobs[i];
    const uint32_t a = 0xA0000000u + ((static_cast<uint32_t>(Clamp15(v->knobs[i])) * 3u) << 15);
    const int32_t len = (kn.r * kOne * 4) / 5;
    const int32_t z = kH * kOne + height[i];
    Line(r, p, kn.u * kOne, kn.v * kOne, z, kn.u * kOne + RSin(len, a), kn.v * kOne + RCos(len, a), z, 2048,
         *kKnobColour[i]);
  }
  // the gauges on the front: filter (white) and space (orange), at least 3 %
  int32_t fil = Clamp15(v->words[1]), spa = Clamp15(v->words[2]);
  fil = fil < 983 ? 983 : fil;
  spa = spa < 983 ? 983 : spa;
  Line(r, p, -60 * kOne, -kD * kOne, 9 * kOne, -60 * kOne + ((1728 * fil) >> 10), -kD * kOne, 9 * kOne, 2048, kWhite);
  Line(r, p, 6 * kOne, -kD * kOne, 9 * kOne, 6 * kOne + ((1728 * spa) >> 10), -kD * kOne, 9 * kOne, 2048, kOrange);
}

}  // namespace

extern "C" const coop_fx_screen_t SCREEN_petri = { ScreenInit, ScreenUpdate };
