/* tape_echo's screen (e9, docs/plugins.md "e9"): te-op1-cfw's kennel screen (ports/knl/tape_echo/tape_echo_knl.cc
 * ScreenInit / ScreenUpdate, the canvas "Tape echo screen" design B "Schematic") on the coop effect-screen calls
 * (sdk/coop_fx_screen.h). The drawing is kennel's; what changed (README "Screen"):
 *   - inputs: the four turns from view->knobs (kennel's op1_fx_knobs_live), the gain, sync and head pattern from
 *     view->words[0..2] (words 5..7), hold from view->latches bit 0; the time in force, the sync division in force and
 *     the 96th note's length from the display words the render publishes (state.h Display; kennel read its TimeLock's
 *     div[] and recomputed the time word here);
 *   - kennel's blue page 2 (gain) and its shift-on-the-time-page tempo lock are both fn held now (view->mods bit 1):
 *     fn + blue turns the gain, fn + green the sync, so while fn is down the gain's bracket shows and the beat ticks
 *     are bright green; with a division in force they show at rest in green's dark tone (the sync is engaged);
 *   - the readouts (division, pattern, gain in dB) are gone: the core's popup names the value on a turn or a click,
 *     the lit heads show the pattern; kennel's white page-2 bracket round the heads is gone with the page;
 *   - the beat ticks are raster lines (coop_scr_line) instead of 24 SVG lines.
 *
 *   hd1..hd3 (hp, hf, tp)  the play heads (outline, fill, tap), translated with the time (head 3 at 32 + 130 t units
 *                          from the record head), lit per pattern (white)
 *   tape                   the tape wave, scaled by age about the tape row and breathing with the wow
 *   dot                    the signal on the tape, record head -> head 3 at the tape speed (one trip per TIME, at most
 *                          8 a second)
 *   fb1..fb4               feedback, a count set (all four while holding)
 *   inl, ina, rech, ret1..ret3, hold   the hold colours
 *   gbar, gbox             the gain gauge (the bar about the centre) and its bracket while fn is held
 *   mbar                   the mix gauge, scaled about its foot
 *
 * GUI thread only. Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include <stdint.h>
#include "coop_plugin.h"
#include "coop_fx_screen.h"
#include "state.h"

namespace {

using namespace coop_scr;

const int32_t kCY = 76, kBottom = 124, kTapeY = 56, kRec = 68;
const int32_t kSpan0Q10 = 97 << 10;          // head 3's drawn distance from the record head (gen_tape_echo_svg.py SPAN0)
const int32_t kSpanMinQ10 = 32 << 10;        // at time 0; + 130 units at time 1
const int32_t kDotHzMax = 8 << 16;           // the dot's trips a second at the shortest times (Q16)
const uint32_t kDotHzNum = 22050u << 16;     // Hz (Q16) = this / samples: one trip per TIME
const int32_t kWaveMin = 102;                // the wave at age 0: a tenth (Q10)
const int32_t kTicks = 24;                   // 16th ticks at most (kennel's bt1..bt24)
const int32_t kTickTop = kTapeY - 13, kTickShort = kTapeY - 10, kTickBeat = kTapeY - 7;
const int32_t kNinetySixth120 = 7350;        // a 96th note at 120 BPM in Q4 samples (tape_echo.cc, before a render)

const char* const kHeadIds[3] = { "hd1", "hd2", "hd3" };
const char* const kHeadLineIds[3] = { "hp1", "hp2", "hp3" };
const char* const kHeadFillIds[3] = { "hf1", "hf2", "hf3" };
const char* const kTapIds[3] = { "tp1", "tp2", "tp3" };
const char* const kFbIds[4] = { "fb1", "fb2", "fb3", "fb4" };
const char* const kNodeIds[5] = { "tape", "dot", "gbar", "gbox", "mbar" };
enum { N_TAPE, N_DOT, N_GBAR, N_GBOX, N_MBAR };
const char* const kHoldLineIds[7] = { "inl", "ina", "rech", "ret1", "ret2", "ret3", "hold" };
const char* const kPaletteIds[7] = { "palwhite", "paloff", "palpencil", "paldim", "palgreen", "paldkgreen", "palblack" };
enum { C_WHITE, C_OFF, C_PENCIL, C_DIM, C_GREEN, C_DKGREEN, C_BLACK };

bool ready;
void* head_nodes[3];
void* fb_nodes[4];
void* nodes[5];
uint8_t* head_line[3];
uint8_t* head_fill[3];
uint8_t* tap_line[3];
uint8_t* hold_lines[7];
Colour pal[7];
FrameClock clock;
uint32_t dot_phase, wow_phase;

void ScreenInit(void* scr) {
  ready = false;
  void* tmp[7];
  if (!FindAll(scr, kHeadIds, head_nodes, 3) || !FindAll(scr, kHeadLineIds, tmp, 3) ||
      !FindAll(scr, kHeadFillIds, tmp, 3) || !FindAll(scr, kTapIds, tmp, 3) || !FindAll(scr, kFbIds, fb_nodes, 4) ||
      !FindAll(scr, kNodeIds, nodes, 5) || !FindAll(scr, kHoldLineIds, tmp, 7) || !FindAll(scr, kPaletteIds, tmp, 7)) {
    return;
  }
  for (int32_t i = 0; i < 3; ++i) {
    head_line[i] = LineStyle(scr, kHeadLineIds[i]);
    head_fill[i] = FillStyle(scr, kHeadFillIds[i]);
    tap_line[i] = LineStyle(scr, kTapIds[i]);
  }
  for (int32_t i = 0; i < 7; ++i) hold_lines[i] = LineStyle(scr, kHoldLineIds[i]);
  for (int32_t i = 0; i < 7; ++i) ReadColour(&pal[i], LineStyle(scr, kPaletteIds[i]));
  clock.Tick();
  dot_phase = wow_phase = 0;
  ready = true;
}

// The beat ticks: a 16th (6 x 96th) apart from the record head to head 3, every fourth (a beat) longer.
void BeatTicks(int32_t s96, int32_t time_q4, int32_t span, const Colour& c) {
  void* r = coop_scr_raster();
  if (!r || s96 <= 0 || time_q4 <= 0 || time_q4 >= (1 << 20)) return;
  for (int32_t k = 1; k <= kTicks; ++k) {
    const int32_t at = s96 * 6 * k;                         // the kth 16th, Q4 samples (< 2^20 while it shows)
    if (at > time_q4) break;
    const int32_t frac = (at << 11) / time_q4;               // Q11 of the span: 32-bit (no 64-bit divide linked)
    const int32_t x = (kRec << 10) + ((span * frac) >> 11);
    const int32_t p0[2] = { x, kTickTop << 10 }, p1[2] = { x, (k % 4 == 0 ? kTickBeat : kTickShort) << 10 };
    coop_scr_line(r, p0, p1, 1536, c.rgba);
  }
}

void ScreenUpdate(void* scr, int dt, const coop_fx_view_t* v) {
  bool ticks = false;
  int32_t s96 = 0, time_q4 = 0, span = 0;
  Colour tick_colour = pal[C_GREEN];
  if (ready) {
    const int32_t feedback = Clamp15(v->knobs[1]), age = Clamp15(v->knobs[2]), mix = Clamp15(v->knobs[3]);
    const int32_t gain = Clamp15(v->words[0]);
    int32_t pattern = v->words[2];
    pattern = pattern < 0 ? 0 : (pattern >= op1fx::kTapeEchoNumPatterns ? op1fx::kTapeEchoNumPatterns - 1 : pattern);
    const bool hold = (v->latches & 1) != 0;
    const bool fn = (v->mods & 2) != 0;          // fn + blue the gain, fn + green the sync (kennel: page 2, shift)
    const uint32_t cycles = clock.Tick();

    // The time in force and the division (the render's display words; the turn before the first render).
    const volatile coop_tape_echo::Instance* st = Published<coop_tape_echo::Instance>(v);
    int32_t time = st ? st->disp.time : -1;
    int32_t div = st ? st->disp.div : -1;
    s96 = st ? st->disp.beat : kNinetySixth120;
    if (time < 0 || time > 32767) {
      time = Clamp15(v->knobs[0]);
      div = -1;
    }
    if (s96 <= 0 || s96 > 1000000) s96 = kNinetySixth120;

    // Time: the heads' distance from the record head, the dot's speed, the beat ticks.
    span = kSpanMinQ10 + ((time * 4160) >> 10);   // 32 + 130 t units, Q10
    const int32_t d3 = span - kSpan0Q10;
    const int32_t d1 = d3 / 3;
    SetTranslate(head_nodes[0], d1, 0);
    SetTranslate(head_nodes[1], 2 * d1, 0);
    SetTranslate(head_nodes[2], d3, 0);
    time_q4 = op1fx::TapeEchoTimeSamplesQ4(time);
    const uint32_t samples = static_cast<uint32_t>(time_q4 >> 4);
    uint32_t hz = samples > 0 ? kDotHzNum / samples : kDotHzMax;
    if (hz > static_cast<uint32_t>(kDotHzMax)) hz = kDotHzMax;
    dot_phase += PhaseStep(static_cast<int32_t>(hz), cycles);
    SetTranslate(nodes[N_DOT], (static_cast<int32_t>(dot_phase >> 22) * span) >> 10, 0);
    ticks = fn || div >= 0;                       // bright while fn is held; dark green at rest while a division holds
    tick_colour = fn ? pal[C_GREEN] : pal[C_DKGREEN];

    // The heads lit per pattern (white's click).
    const int32_t heads = op1fx::kTapeEchoPatternHeads[pattern];
    for (int32_t i = 0; i < 3; ++i) {
      const bool on = (heads >> i) & 1;
      WriteColour(head_line[i], on ? pal[C_WHITE] : pal[C_OFF]);
      WriteColour(head_fill[i], on ? pal[C_WHITE] : pal[C_BLACK]);
      WriteColour(tap_line[i], on ? pal[C_WHITE] : pal[C_OFF]);
    }

    // Age: the wave's amplitude, breathing with the wow (a quarter either way, faster with age).
    wow_phase += PhaseStep((1 << 15) + ((age * 3) << 1), cycles);   // kennel's rate: 0.5 Hz + 3 Hz x age (Q16)
    int32_t sy = age >> 5;
    if (sy < kWaveMin) sy = kWaveMin;
    sy = (sy * (0x400 + (SinQ15(wow_phase) >> 7))) >> 10;
    SetMatrix(nodes[N_TAPE], 0x400, sy, 0, kTapeY * (0x400 - sy));

    // Feedback: the chevrons (all four while holding); the hold colours.
    ShowFirst(fb_nodes, 4, hold ? 4 : Steps(feedback, 4));
    for (int32_t i = 0; i < 3; ++i) WriteColour(hold_lines[i], hold ? pal[C_DIM] : pal[C_PENCIL]);     // input, record head
    for (int32_t i = 3; i < 6; ++i) WriteColour(hold_lines[i], hold ? pal[C_GREEN] : pal[C_PENCIL]);   // the return path
    WriteColour(hold_lines[6], hold ? pal[C_GREEN] : pal[C_DKGREEN]);                                  // the glyph

    // Gain: the bar scaled -1..1 about the centre; the bracket while fn is held.
    const int32_t d = (gain - 16384) >> 4;   // Q10, -1024..1023
    SetMatrix(nodes[N_GBAR], 0x400, d, 0, kCY * (0x400 - d));
    SetVisible(nodes[N_GBOX], fn);
    // Mix: the bar scaled about its foot.
    const int32_t s = mix >> 5;
    SetMatrix(nodes[N_MBAR], 0x400, s < 8 ? 8 : s, 0, kBottom * (0x400 - (s < 8 ? 8 : s)));
  }
  coop_scr_draw(scr, dt);
  if (ready && ticks) BeatTicks(s96, time_q4, span, tick_colour);
}

}  // namespace

extern "C" const coop_fx_screen_t SCREEN_tape_echo = { ScreenInit, ScreenUpdate };
